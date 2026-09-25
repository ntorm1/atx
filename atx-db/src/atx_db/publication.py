"""Scheduled full-universe publication: Parquet, one manifest, one diff.

A release is a directory of Parquet files plus a single ``manifest.json`` that pins, per
dataset: the exported column schema (``lake._schema_sha256``), the public record contract
(``api.catalog._record_schema_sha256``), the exact SQL text executed, the file bytes, the
row count, and the added/removed/changed counts against the previous release. Every hash
is content-addressed; unchanged warehouse inputs produce identical data, schema and query
hashes with the same DuckDB version. Release metadata includes a caller-supplied load stamp.

The diff is computed in DuckDB over the two Parquet files with a full outer join on the
dataset's key columns and a row digest over every exported column, so it is exact rather
than a row-count delta.

Honest manifest (C4, contract 1.1.0). In the same database snapshot as the exports the
manifest also records:

``source_pins``
    ``raw_source_files`` SHA-256 receipts of the four raw sources (CompanyFacts,
    submissions, TickerHistory, Nasdaq directory).
``evidence``
    One block per released dataset (plus the unreleased survivorship-safe forward
    labels) stating what is modeled, reconstructed, inferred or policy: fundamentals
    availability ``conservative_filing_date_46h``, prices/market ``modeled_trade_date_22h``,
    the market owner-bridge identity and share basis (A5/A8; vendor share-run clocks are
    modeled, never verified), the evidence status of every ``universe_id`` (strict vs
    labeled reconstruction, RX1), delisting inferred/policy counts (A3/RX2) and forward
    label observed/policy counts (R3a, ``calculation_version``).
``gates`` / ``eligibility``
    Five release gates read from stored measurements: item coverage on a *certified*
    3,000-name cohort, every provider SLO ``available``, no failed critical DQC, source
    completeness and strict-universe certification. Each gate is ``passed``, ``failed`` or
    ``unmeasured`` (no stored value, or evidence older than the data it describes).
    ``eligibility`` is ``eligible`` only when every required gate is ``passed``; anything
    else, including a gate that cannot be evaluated, yields ``candidate``. Nothing here
    promotes reconstructed or inferred history to verified historical vintage.
"""

from __future__ import annotations

import datetime as dt
import hashlib
import json
import re
import shutil
import tempfile
from collections.abc import Iterable, Mapping, Sequence
from dataclasses import dataclass
from pathlib import Path
from typing import Any

from ._forward_return_publication import CALCULATION_VERSION as FORWARD_RETURN_CALCULATION_VERSION
from ._fundamental_clock import FUNDAMENTAL_CLOCK_POLICY
from .api.catalog import RecordSchema, _record_schema_sha256, get_schema
from .connection import DuckDBStore
from .delisting import delisting_policy_bias_exposure
from .item_coverage import (
    DEFAULT_SOURCE as ITEM_COVERAGE_SOURCE,
)
from .item_coverage import (
    DEFAULT_UNIVERSE_ID as ANNUAL_COHORT_UNIVERSE_ID,
)
from .item_coverage import (
    ITEM_COVERAGE_GATE_BASIS,
    ITEM_COVERAGE_TARGET_ITEMS,
    ITEM_COVERAGE_TARGET_PCT,
    coverage_gate_count_sql,
)
from .item_coverage_cohort import COHORT_SIZE
from .lake import _object_schema, _schema_sha256
from .market_daily import (
    ARCHIVE_MODELED_LAG_DAYS,
    ARCHIVE_RUN_CLOCKS,
    MARKET_DAILY_STRICT_SOURCE_NAME,
    OWNER_BRIDGE_CHECK_NAME,
    SHARES_AVAILABILITY_BASIS,
    MarketDailyDataset,
)
from .provider_coverage import DEFAULT_PROVIDER_COVERAGE_SLOS
from .universe_us_listed import DEFAULT_US_LISTED_UNIVERSE_ID
from .warehouse import file_sha256

__all__ = [
    "CANDIDATE",
    "CANDIDATE_EXIT_CODE",
    "ELIGIBLE",
    "ELIGIBLE_EXIT_CODE",
    "FUNDAMENTALS_AVAILABILITY_BASIS",
    "GATE_FAILED",
    "GATE_PASSED",
    "GATE_UNMEASURED",
    "MANIFEST_NAME",
    "MARKET_AVAILABILITY_BASIS",
    "PUBLICATION_CONTRACT_VERSION",
    "RELEASE_DATASETS",
    "RELEASE_DATASET_STAGES",
    "RELEASE_GATES",
    "RELEASE_SOURCES",
    "ReleaseDataset",
    "ReleaseDatasetResult",
    "ReleaseResult",
    "activation_stage_run_ids",
    "diff_against",
    "publish_release",
    "read_release_manifest",
    "release_eligibility",
    "release_exit_code",
    "release_query",
    "row_digest_sql",
]

# 1.1.0: additive source pins, per-dataset evidence, gates and eligibility (C4).
PUBLICATION_CONTRACT_VERSION = "1.1.0"
MANIFEST_NAME = "manifest.json"

# --- Release eligibility -------------------------------------------------------------
GATE_PASSED = "passed"
GATE_FAILED = "failed"
GATE_UNMEASURED = "unmeasured"
ELIGIBLE = "eligible"
CANDIDATE = "candidate"
#: Process exit codes of ``scripts/publish_release.py``: an eligible release exits 0; a
#: candidate release (written, but not eligible) exits with a code distinct from
#: success (0), an unhandled error (1) and an argparse usage error (2).
ELIGIBLE_EXIT_CODE = 0
CANDIDATE_EXIT_CODE = 3
#: Every gate must be ``passed`` for ``eligible``. A gate missing from the evaluation is
#: treated as not passed, so the rule cannot be satisfied by omission.
RELEASE_GATES: tuple[str, ...] = (
    "item_coverage",
    "provider_slos",
    "critical_dqc",
    "source_completeness",
    "universe_certification",
)

# --- Evidence labels -----------------------------------------------------------------
#: SEC filing date + 46h (``sec_filed_date_plus_46h_v1``): a conservative modeled date
#: policy, not measured acceptance or delivery. research.panel names the same clock
#: ``conservative_filing_46h``.
FUNDAMENTALS_AVAILABILITY_BASIS = "conservative_filing_date_46h"
#: Every bar (and each market-panel row) is modeled as known at trade_date + 22h.
MARKET_AVAILABILITY_BASIS = "modeled_trade_date_22h"
#: historical_identity vocabulary: only verified_dated identity with verified
#: availability counts toward certification.
CERTIFIED_EVIDENCE_STATUS = "verified_dated"
CERTIFIED_AVAILABILITY_BASIS = "verified"
COVERAGE_FIRST_FISCAL_YEAR = 2015
SOURCE_COMPLETENESS_CHECK_NAME = "source_completeness"
#: (manifest key, ``raw_source_files.dataset_id``) of the pinned raw sources.
RELEASE_SOURCES: tuple[tuple[str, str], ...] = (
    ("companyfacts", "sec_company_facts"),
    ("submissions", "sec_submissions"),
    ("ticker_history", "tbltickerhistory_daily"),
    ("symbol_directory", "nasdaq_symbol_directory"),
)
#: Activation stage producing each released dataset. Gate evidence recorded before the
#: newest completed run of any of them describes an older warehouse state (stale).
RELEASE_DATASET_STAGES: dict[str, str] = {
    "security_master": "security_master",
    "universe": "universe_us_listed",
    "delistings": "delisting_evidence",
    "fundamentals_core": "standardized",
    "derived_metrics": "derived_metrics",
    "market_daily": "market_daily",
}
_FAILURE_STATUSES = frozenset({"failed", "error"})
_LIST_CAP = 50
_PIN_CAP = 32


@dataclass(frozen=True)
class ReleaseDataset:
    name: str
    object_name: str
    key_columns: tuple[str, ...]
    schema_dataset: str | None
    schema_code: str | None

    @property
    def schema_ref(self) -> str | None:
        if self.schema_dataset is None or self.schema_code is None:
            return None
        return f"{self.schema_dataset}/{self.schema_code}"


# The six full-universe release datasets (Tier1-S4 Task 8). ``fundamentals_core`` cites
# the pre-existing ``standardized`` schema code rather than a fourth fundamentals-core
# schema code -- see the S4 preflight ruling recorded in
# .superpowers/sdd/tier1-parity/program.md ("no fourth fundamentals-core schema code").
RELEASE_DATASETS: tuple[ReleaseDataset, ...] = (
    ReleaseDataset(
        "security_master",
        "v_security_master_public",
        ("security_id",),
        "ATX.US.FUNDAMENTALS",
        "security-master",
    ),
    ReleaseDataset(
        "universe",
        "universe_us_listed_membership",
        ("universe_id", "security_id", "valid_from"),
        "ATX.US.EQUITIES",
        "universe",
    ),
    ReleaseDataset(
        "delistings",
        "delisting_events",
        # The public logical key contains nullable security IDs; use the physical event
        # key to retain every event, including unresolved symbols and evidence revisions.
        ("delisting_event_id",),
        "ATX.US.EQUITIES",
        "delistings",
    ),
    ReleaseDataset(
        "fundamentals_core",
        "fundamental_standardized",
        ("standardized_id",),
        "ATX.US.FUNDAMENTALS",
        "standardized",
    ),
    ReleaseDataset(
        "derived_metrics",
        "derived_metric_values",
        # Physical event key preserves invalid states and same-value lineage
        # revisions. Never filter this release to valid/latest-only rows.
        ("derived_value_id",),
        "ATX.US.FUNDAMENTALS",
        "derived-metrics",
    ),
    ReleaseDataset(
        "market_daily",
        "market_daily_metrics",
        ("market_daily_id",),
        "ATX.US.EQUITIES",
        "market-daily-1d",
    ),
)


@dataclass(frozen=True)
class ReleaseDatasetResult:
    name: str
    object_name: str
    parquet_path: Path
    row_count: int
    byte_count: int
    schema_sha256: str
    query_sha256: str
    parquet_sha256: str
    rows_added: int | None
    rows_removed: int | None
    rows_changed: int | None


@dataclass(frozen=True)
class ReleaseResult:
    release_id: str
    out_dir: Path
    manifest_path: Path
    manifest_sha256: str
    previous_release_id: str | None
    datasets: tuple[ReleaseDatasetResult, ...]
    eligibility: str
    gates_not_passed: tuple[str, ...]

    @property
    def total_rows(self) -> int:
        return sum(dataset.row_count for dataset in self.datasets)


def release_eligibility(gates: Mapping[str, Mapping[str, object]]) -> tuple[str, tuple[str, ...]]:
    """(eligibility, gates not passed) for evaluated release gates.

    ``eligible`` requires every :data:`RELEASE_GATES` entry to be present with status
    ``passed``. Missing, ``failed`` and ``unmeasured`` gates all yield ``candidate``.
    """

    unknown = sorted(set(gates) - set(RELEASE_GATES))
    if unknown:
        raise ValueError(f"unknown release gates: {unknown}")
    not_passed = tuple(name for name in RELEASE_GATES if (gates.get(name) or {}).get("status") != GATE_PASSED)
    return (CANDIDATE if not_passed else ELIGIBLE), not_passed


def release_exit_code(eligibility: object) -> int:
    """0 only for an ``eligible`` manifest; any other value is a candidate."""

    return ELIGIBLE_EXIT_CODE if eligibility == ELIGIBLE else CANDIDATE_EXIT_CODE


def _quote(identifier: str) -> str:
    if not identifier.replace("_", "").isalnum() or identifier[0].isdigit():
        raise ValueError(f"unsafe identifier: {identifier!r}")
    return f'"{identifier}"'


def _sha256_text(text: str) -> str:
    return hashlib.sha256(text.encode("utf-8")).hexdigest()


def release_query(dataset: ReleaseDataset, columns: list[str]) -> str:
    """The exact SQL a release executes for one dataset.

    Columns are listed explicitly (never ``SELECT *``) so a warehouse column added after
    the release was cut cannot silently change the export, and the ORDER BY is the
    dataset's key so the Parquet bytes are reproducible.
    """

    projection = ", ".join(_quote(column) for column in columns)
    order = ", ".join(f"{_quote(column)} ASC NULLS LAST" for column in dataset.key_columns)
    return f"SELECT {projection} FROM {_quote(dataset.object_name)} ORDER BY {order}"


def row_digest_sql(alias: str, columns: list[str]) -> str:
    """Hash a JSON tuple so NULL positions and embedded separators are preserved."""

    parts = ", ".join(f"{_quote(alias)}.{_quote(column)}" for column in columns)
    return f"sha256(CAST(to_json(row({parts})) AS VARCHAR))"


def _parquet_columns(store: DuckDBStore, path: Path) -> list[tuple[str, str]]:
    return [
        (str(row[0]), str(row[1]))
        for row in store.con.execute("DESCRIBE SELECT * FROM read_parquet(?)", [str(path)]).fetchall()
    ]


def _validate_keys(store: DuckDBStore, dataset: ReleaseDataset, path: Path) -> None:
    keys = ", ".join(_quote(column) for column in dataset.key_columns)
    duplicate = store.con.execute(
        f"SELECT 1 FROM read_parquet(?) GROUP BY {keys} HAVING count(*) > 1 LIMIT 1", [str(path)]
    ).fetchone()
    if duplicate is not None:
        raise ValueError(f"Duplicate publication keys for {dataset.name!r}: {dataset.key_columns}")


def _copy_parquet(store: DuckDBStore, query: str, path: Path) -> None:
    # Pin writer parallelism/order as well as row-group size: callers may have disabled
    # insertion-order preservation for bulk loading. Restore their session settings.
    settings = store.con.execute(
        "SELECT current_setting('threads'), current_setting('preserve_insertion_order')"
    ).fetchone()
    if settings is None:
        raise RuntimeError("Cannot inspect export session settings")
    try:
        store.con.execute("SET threads = 1")
        store.con.execute("SET preserve_insertion_order = true")
        store.con.execute(
            f"COPY ({query}) TO '{str(path).replace(chr(39), chr(39) * 2)}' "
            "(FORMAT PARQUET, COMPRESSION ZSTD, ROW_GROUP_SIZE 122880, PER_THREAD_OUTPUT false)"
        )
    finally:
        store.con.execute(f"SET threads = {int(settings[0])}")
        store.con.execute(f"SET preserve_insertion_order = {'true' if settings[1] else 'false'}")


def diff_against(
    store: DuckDBStore,
    dataset: ReleaseDataset,
    columns: list[str],
    current_parquet: Path,
    previous_parquet: Path,
) -> tuple[int, int, int]:
    """(added, removed, changed) between two release Parquet files for one dataset.

    A key present only in the current file is ADDED, only in the previous file is REMOVED,
    and present in both with a different row digest is CHANGED. Incompatible column/type
    changes and duplicate keys fail explicitly rather than report a misleading diff.
    """

    current_schema = _parquet_columns(store, current_parquet)
    if current_schema != _parquet_columns(store, previous_parquet) or columns != [c[0] for c in current_schema]:
        raise ValueError(f"Cannot diff incompatible schemas for {dataset.name!r}")
    _validate_keys(store, dataset, current_parquet)
    _validate_keys(store, dataset, previous_parquet)
    join = " AND ".join(
        f"cur.{_quote(column)} IS NOT DISTINCT FROM prev.{_quote(column)}" for column in dataset.key_columns
    )
    row = store.con.execute(
        f"""
        WITH cur AS (
            SELECT *, {row_digest_sql("t", columns)} AS _digest
            FROM read_parquet(?) AS t
        ),
        prev AS (
            SELECT *, {row_digest_sql("t", columns)} AS _digest
            FROM read_parquet(?) AS t
        )
        SELECT
            count(*) FILTER (WHERE prev._digest IS NULL)::BIGINT AS added,
            count(*) FILTER (WHERE cur._digest IS NULL)::BIGINT AS removed,
            count(*) FILTER (
                WHERE cur._digest IS NOT NULL
                  AND prev._digest IS NOT NULL
                  AND cur._digest <> prev._digest
            )::BIGINT AS changed
        FROM cur
        FULL OUTER JOIN prev ON {join}
        """,
        [str(current_parquet), str(previous_parquet)],
    ).fetchone()
    if row is None:
        raise RuntimeError(f"diff query for {dataset.name!r} returned no row")
    return int(row[0]), int(row[1]), int(row[2])


def activation_stage_run_ids(store: DuckDBStore) -> dict[str, str]:
    """The newest completed run id per activation stage, for release provenance."""

    rows = store.con.execute(
        """
        SELECT stage, run_id
        FROM (
            SELECT stage, run_id,
                   row_number() OVER (PARTITION BY stage ORDER BY started_at DESC, run_id DESC) AS rn
            FROM activation_stage_runs
            WHERE status = 'completed'
        )
        WHERE rn = 1
        ORDER BY stage
        """
    ).fetchall()
    return {str(stage): str(run_id) for stage, run_id in rows}


def read_release_manifest(path: Path | str) -> dict[str, object]:
    """Read a release manifest written by :func:`publish_release`."""

    result: dict[str, object] = json.loads(Path(path).read_text(encoding="utf-8"))
    if not isinstance(result, dict):
        raise ValueError("Release manifest must be a JSON object")
    return result


def _schema_for(dataset: ReleaseDataset) -> RecordSchema | None:
    if dataset.schema_dataset is None or dataset.schema_code is None:
        return None
    return get_schema(dataset.schema_dataset, dataset.schema_code)


def _previous_release(previous_dir: Path | str | None) -> tuple[str | None, dict[str, Path]]:
    if previous_dir is None:
        return None, {}
    root = Path(previous_dir)
    manifest = read_release_manifest(root / MANIFEST_NAME)
    release_id, datasets = manifest.get("release_id"), manifest.get("datasets")
    if not isinstance(release_id, str) or not release_id or not isinstance(datasets, list):
        raise ValueError("Previous release manifest lacks release_id or datasets")
    files: dict[str, Path] = {}
    for entry in datasets:
        if not isinstance(entry, dict) or not isinstance(entry.get("name"), str):
            raise ValueError("Invalid previous release dataset")
        name = entry["name"]
        if not re.fullmatch(r"[A-Za-z0-9_]+", name) or name in files or entry.get("parquet") != f"{name}.parquet":
            raise ValueError("Invalid or duplicate previous release dataset path")
        path = root / f"{name}.parquet"
        if file_sha256(path) != entry.get("parquet_sha256"):
            raise ValueError(f"Previous release file hash mismatch: {name}")
        files[name] = path
    return release_id, files


# --- Evidence and gates: read inside the publication snapshot -------------------------
# Lists are sorted in Python (never by session-dependent default ORDER BY direction or
# NULL order) so identical warehouse inputs always produce identical manifest bytes.


def _iso(value: object) -> object:
    return value.isoformat() if isinstance(value, dt.datetime | dt.date) else value


def _key(value: object) -> str:
    return "null" if value is None else str(value)


def _marks(values: Sequence[object]) -> str:
    return ", ".join(["?"] * len(values))


def _table_columns(store: DuckDBStore, table: str) -> set[str]:
    rows = store.con.execute(
        "SELECT column_name FROM duckdb_columns() "
        "WHERE database_name = current_database() AND schema_name = 'main' AND table_name = ?",
        [table],
    ).fetchall()
    return {str(row[0]) for row in rows}


def _count_rows(rows: Iterable[Sequence[Any]], fields: Sequence[str]) -> list[dict[str, object]]:
    """``[(f1, f2, ..., count)]`` -> sorted ``[{f1:, f2:, ..., rows:}]`` (NULL kept as null)."""

    items = [
        {**{field: _iso(value) for field, value in zip(fields, row[:-1], strict=True)}, "rows": int(row[-1])}
        for row in rows
    ]
    return sorted(items, key=lambda item: tuple(_key(item[field]) for field in fields))


def _capped(values: Iterable[str]) -> tuple[list[str], bool]:
    ordered = sorted(values)
    return ordered[:_LIST_CAP], len(ordered) > _LIST_CAP


def _gate(failed: Iterable[str], unmeasured: Iterable[str], requirement: str, **details: object) -> dict[str, object]:
    """One gate record: ``failed`` beats ``unmeasured`` beats ``passed``."""

    failed_reasons, unmeasured_reasons = sorted(set(failed)), sorted(set(unmeasured))
    status = GATE_FAILED if failed_reasons else GATE_UNMEASURED if unmeasured_reasons else GATE_PASSED
    return {
        "status": status,
        "failed_reasons": failed_reasons,
        "unmeasured_reasons": unmeasured_reasons,
        "requirement": requirement,
        **details,
    }


def _evidence_floor(store: DuckDBStore) -> tuple[dt.datetime | None, dict[str, object]]:
    """Newest completed activation run of any release-producing stage."""

    stages = sorted(set(RELEASE_DATASET_STAGES.values()))
    rows = store.con.execute(
        f"""
        SELECT stage, max(coalesce(finished_at, started_at))
        FROM activation_stage_runs
        WHERE status = 'completed' AND stage IN ({_marks(stages)})
        GROUP BY stage
        """,
        stages,
    ).fetchall()
    completed: dict[str, dt.datetime] = {str(stage): at for stage, at in rows if at is not None}
    floor = max(completed.values(), default=None)
    return floor, {
        "floor": _iso(floor),
        "stages": {stage: _iso(completed.get(stage)) for stage in stages},
        "stages_without_completed_run": [stage for stage in stages if stage not in completed],
        "rule": (
            "SLO snapshots and DQC results recorded before the newest completed activation run of a "
            "release-producing stage describe an older warehouse state and are stale (unmeasured)."
        ),
    }


def _source_pins_and_gate(store: DuckDBStore) -> tuple[dict[str, object], dict[str, object]]:
    """raw_source_files SHA-256 pins per raw source, and the source-completeness gate."""

    ids = [dataset_id for _, dataset_id in RELEASE_SOURCES]
    by_pin = store.con.execute(
        f"""
        SELECT dataset_id, pin, count(*)::BIGINT, max(fetched_at)
        FROM (
            SELECT dataset_id, fetched_at,
                   CASE WHEN regexp_full_match(coalesce(sha256, ''), ?) THEN sha256 END AS pin
            FROM raw_source_files WHERE dataset_id IN ({_marks(ids)})
        )
        GROUP BY ALL
        """,
        ["[0-9a-f]{64}", *ids],
    ).fetchall()
    by_status = store.con.execute(
        f"SELECT dataset_id, status, count(*)::BIGINT FROM raw_source_files "
        f"WHERE dataset_id IN ({_marks(ids)}) GROUP BY ALL",
        ids,
    ).fetchall()
    completeness = {
        str(row[0]): (row[1], row[2])
        for row in store.con.execute(
            f"""
            SELECT dataset_id, status, checked_at FROM data_quality_checks
            WHERE check_name = ? AND dataset_id IN ({_marks(ids)})
            QUALIFY row_number() OVER (PARTITION BY dataset_id ORDER BY checked_at DESC, check_id DESC) = 1
            """,
            [SOURCE_COMPLETENESS_CHECK_NAME, *ids],
        ).fetchall()
    }
    pins: dict[str, object] = {}
    sources: dict[str, object] = {}
    failed: list[str] = []
    unmeasured: list[str] = []
    for key, dataset_id in RELEASE_SOURCES:
        pin_rows = sorted(
            ((pin, int(n), at) for source, pin, n, at in by_pin if source == dataset_id), key=lambda r: _key(r[0])
        )
        hashed = [row for row in pin_rows if row[0] is not None]
        statuses = {_key(status): int(n) for source, status, n in by_status if source == dataset_id}
        receipts = sum(n for _, n, _ in pin_rows)
        errors = sum(n for status, n in statuses.items() if status in _FAILURE_STATUSES)
        newest = max((at for _, _, at in pin_rows if at is not None), default=None)
        pins[key] = {
            "dataset_id": dataset_id,
            "receipts": receipts,
            "receipts_by_status": dict(sorted(statuses.items())),
            "hashed_receipts": sum(n for _, n, _ in hashed),
            "unhashed_receipts": receipts - sum(n for _, n, _ in hashed),
            "distinct_sha256": len(hashed),
            "sha256": [{"sha256": pin, "receipts": n, "last_fetched_at": _iso(at)} for pin, n, at in hashed[:_PIN_CAP]],
            "sha256_truncated": len(hashed) > _PIN_CAP,
            "last_fetched_at": _iso(newest),
        }
        source_failed: list[str] = []
        source_unmeasured: list[str] = []
        if not receipts:
            source_unmeasured.append("no_source_receipt")
        elif not hashed:
            source_unmeasured.append("unpinned")
        if errors:
            source_failed.append("error_receipts")
        latest = completeness.get(dataset_id)
        if latest is None:
            source_unmeasured.append("completeness_not_recorded")
        elif latest[0] in _FAILURE_STATUSES:
            source_failed.append("completeness_failed")
        elif latest[0] != GATE_PASSED:
            source_unmeasured.append(f"completeness_{_key(latest[0])}")
        elif newest is not None and latest[1] < newest:
            source_unmeasured.append("completeness_older_than_newest_receipt")
        failed.extend(f"{key}:{reason}" for reason in source_failed)
        unmeasured.extend(f"{key}:{reason}" for reason in source_unmeasured)
        sources[key] = {
            "status": GATE_FAILED if source_failed else GATE_UNMEASURED if source_unmeasured else GATE_PASSED,
            "error_receipts": errors,
            "completeness_status": None if latest is None else latest[0],
            "completeness_checked_at": None if latest is None else _iso(latest[1]),
        }
    gate = _gate(
        failed,
        unmeasured,
        "Each raw source has a SHA-256-pinned receipt, no error receipts, and a latest "
        f"'{SOURCE_COMPLETENESS_CHECK_NAME}' quality row with status 'passed' recorded no earlier "
        "than its newest receipt.",
        sources=sources,
    )
    return pins, gate


def _critical_dqc_gate(store: DuckDBStore, floor: dt.datetime | None) -> dict[str, object]:
    """Every enabled critical check (registry, plus any recorded critical result) passed, fresh."""

    rows = store.con.execute(
        """
        WITH latest AS (
            SELECT dataset_id, table_name, check_name, status, severity, checked_at,
                   status = 'warning' AND json_valid(details_json)
                       AND json_extract(details_json, '$.missing_tables') IS NOT NULL AS missing_inputs
            FROM data_quality_checks
            QUALIFY row_number() OVER (
                PARTITION BY dataset_id, table_name, check_name ORDER BY checked_at DESC, check_id DESC
            ) = 1
        ), registry AS (
            SELECT dataset_id, check_name, enabled, severity FROM quality_check_registry
        ), critical AS (
            SELECT dataset_id, check_name, true AS registered
            FROM registry WHERE enabled AND severity = 'critical'
            UNION
            SELECT l.dataset_id, l.check_name, false
            FROM latest l
            WHERE l.severity = 'critical' AND NOT EXISTS (
                SELECT 1 FROM registry r
                WHERE r.check_name = l.check_name AND (NOT r.enabled OR r.dataset_id = l.dataset_id)
            )
        )
        SELECT c.dataset_id, c.check_name, bool_or(c.registered),
               count(l.status)::BIGINT,
               count(l.status) FILTER (WHERE l.status = 'passed')::BIGINT,
               count(l.status) FILTER (WHERE l.status = 'skipped' OR l.missing_inputs)::BIGINT,
               min(l.checked_at)
        FROM critical c
        LEFT JOIN latest l ON l.dataset_id = c.dataset_id AND l.check_name = c.check_name
        GROUP BY c.dataset_id, c.check_name
        """
    ).fetchall()
    outcomes: dict[str, list[str]] = {"passed": [], "failed": [], "not_run": [], "not_evaluated": [], "stale": []}
    registered = 0
    for dataset_id, check_name, is_registered, results, passed, not_evaluated, oldest in rows:
        name = f"{dataset_id}/{check_name}"
        registered += bool(is_registered)
        if not results:
            outcomes["not_run"].append(name)
        elif results - passed - not_evaluated:
            # Any other latest status (failed, error, a warning-grade failure) fails a
            # critical check: failure_status='warning' never downgrades it.
            outcomes["failed"].append(name)
        elif not_evaluated:
            # Skipped, or a warning recorded because the check's input tables were missing.
            outcomes["not_evaluated"].append(name)
        elif floor is not None and oldest < floor:
            outcomes["stale"].append(name)
        else:
            outcomes["passed"].append(name)
    unmeasured = [outcome for outcome in ("not_run", "not_evaluated", "stale") if outcomes[outcome]]
    if not rows:
        unmeasured.append("no_critical_checks")
    if floor is None:
        unmeasured.append("evidence_floor_unknown")
    failed_checks, failed_truncated = _capped(outcomes["failed"])
    open_checks, open_truncated = _capped(outcomes["not_run"] + outcomes["not_evaluated"] + outcomes["stale"])
    return _gate(
        ["critical_check_failed"] if outcomes["failed"] else [],
        unmeasured,
        "Every enabled critical quality check has a latest result 'passed' recorded no earlier "
        "than the evidence floor; skipped, missing-input, not-run and stale checks never pass.",
        critical_checks=len(rows),
        registered_critical_checks=registered,
        outcomes={outcome: len(names) for outcome, names in outcomes.items()},
        failed_checks=failed_checks,
        unresolved_checks=open_checks,
        lists_truncated=failed_truncated or open_truncated,
    )


def _provider_slo_gate(store: DuckDBStore, floor: dt.datetime | None) -> dict[str, object]:
    """Every active provider SLO (a superset of the code defaults) latest-measured available."""

    rows = store.con.execute(
        """
        WITH slos AS (
            SELECT dataset_id, schema_code, slo_version
            FROM api_schema_coverage_slo WHERE is_active
            QUALIFY row_number() OVER (
                PARTITION BY dataset_id, schema_code ORDER BY valid_from DESC, slo_version DESC
            ) = 1
        ), snaps AS (
            SELECT dataset_id, schema_code, slo_version, condition, observed_at
            FROM api_schema_coverage_snapshot
            QUALIFY row_number() OVER (
                PARTITION BY dataset_id, schema_code
                ORDER BY observed_at DESC, source_loaded_at DESC, coverage_snapshot_id DESC
            ) = 1
        )
        SELECT s.dataset_id, s.schema_code, s.slo_version, p.slo_version, p.condition, p.observed_at
        FROM slos s LEFT JOIN snaps p ON p.dataset_id = s.dataset_id AND p.schema_code = s.schema_code
        """
    ).fetchall()
    required = sorted({(slo.dataset_id, slo.schema_code) for slo in DEFAULT_PROVIDER_COVERAGE_SLOS})
    active = {(str(row[0]), str(row[1])) for row in rows}
    inactive_required = [
        f"{dataset_id}/{schema_code}" for dataset_id, schema_code in required if (dataset_id, schema_code) not in active
    ]
    slos: list[dict[str, object]] = []
    failed: list[str] = ["required_slo_not_active"] if inactive_required else []
    unmeasured: list[str] = [] if rows else ["no_active_slos"]
    for dataset_id, schema_code, active_version, measured_version, condition, observed_at in rows:
        if condition is None:
            outcome = "not_run"
        elif measured_version != active_version:
            outcome = "slo_version_mismatch"
        elif condition != "available":
            outcome = f"condition_{condition}"
        elif floor is not None and observed_at < floor:
            outcome = "stale"
        else:
            outcome = "available"
        if outcome.startswith("condition_"):
            failed.append("slo_not_available")
        elif outcome != "available":
            unmeasured.append(outcome)
        slos.append(
            {
                "slo": f"{dataset_id}/{schema_code}",
                "outcome": outcome,
                "condition": condition,
                "observed_at": _iso(observed_at),
            }
        )
    if floor is None:
        unmeasured.append("evidence_floor_unknown")
    return _gate(
        failed,
        unmeasured,
        "Every active provider coverage SLO (including every code-defined default) has a latest "
        "snapshot with condition 'available' at the active SLO version, observed no earlier than the "
        "evidence floor.",
        required_slos=len(required),
        active_slos=len(rows),
        available_slos=sum(1 for slo in slos if slo["outcome"] == "available"),
        inactive_required_slos=inactive_required,
        slos=sorted(slos, key=lambda slo: str(slo["slo"])),
    )


def _universe_evidence(store: DuckDBStore) -> dict[str, dict[str, object]]:
    """Evidence status and certification status per released ``universe_id``."""

    def label(field: str) -> str:
        return f"CASE WHEN json_valid(rules_json) THEN json_extract_string(rules_json, '$.{field}') END"

    fields = ("evidence_status", "identity_basis", "availability_basis", "listing_basis", "certification_eligible")
    labels = store.con.execute(
        f"""
        SELECT universe_id, {", ".join(f"{label(field)} AS {field}" for field in fields)}, count(*)::BIGINT
        FROM universe_us_listed_membership GROUP BY ALL
        """
    ).fetchall()
    extents = store.con.execute(
        """
        SELECT universe_id, count(*)::BIGINT, count(DISTINCT security_id)::BIGINT,
               count(*) FILTER (WHERE has_cik)::BIGINT, min(valid_from), max(valid_from)
        FROM universe_us_listed_membership GROUP BY universe_id
        """
    ).fetchall()
    reasons = store.con.execute(
        "SELECT universe_id, reason, count(*)::BIGINT FROM universe_us_listed_membership GROUP BY ALL"
    ).fetchall()
    universes: dict[str, dict[str, object]] = {}
    for universe_id, rows, securities, with_cik, first, last in sorted(extents, key=lambda row: str(row[0])):
        combos = [row[1:] for row in labels if row[0] == universe_id]
        verified = sum(
            int(row[-1])
            for row in combos
            if row[0] == CERTIFIED_EVIDENCE_STATUS
            and row[1] == CERTIFIED_EVIDENCE_STATUS
            and row[2] == CERTIFIED_AVAILABILITY_BASIS
        )
        reconstructed = sum(int(row[-1]) for row in combos if row[0] == "reconstructed" or row[4] == "false")
        status = "reconstructed" if reconstructed else "certified" if verified == int(rows) else "not_certified"
        universes[str(universe_id)] = {
            "rows": int(rows),
            "securities": int(securities),
            "rows_with_cik": int(with_cik),
            "first_valid_from": _iso(first),
            "last_valid_from": _iso(last),
            "verified_rows": verified,
            "reconstructed_rows": reconstructed,
            "certification_status": status,
            "strict_release_universe": universe_id == DEFAULT_US_LISTED_UNIVERSE_ID,
            "labels": _count_rows(combos, fields),
            "rows_by_reason": _count_rows([row[1:] for row in reasons if row[0] == universe_id], ("reason",)),
        }
    return universes


def _identity_evidence_counts(store: DuckDBStore) -> object:
    if not _table_columns(store, "security_identity_evidence"):
        return {"status": GATE_UNMEASURED, "reason": "security_identity_evidence absent"}
    rows = store.con.execute(
        "SELECT fact_kind, evidence_status, availability_status, count(*)::BIGINT "
        "FROM security_identity_evidence GROUP BY ALL"
    ).fetchall()
    return _count_rows(rows, ("fact_kind", "evidence_status", "availability_status"))


def _universe_certification_gate(universes: Mapping[str, Mapping[str, object]]) -> dict[str, object]:
    strict = universes.get(DEFAULT_US_LISTED_UNIVERSE_ID)
    status = None if strict is None else strict["certification_status"]
    return _gate(
        [] if strict is None or status == "certified" else [f"strict_universe_{status}"],
        ["strict_universe_empty"] if strict is None else [],
        f"Every published {DEFAULT_US_LISTED_UNIVERSE_ID} membership row carries evidence_status and "
        f"identity_basis '{CERTIFIED_EVIDENCE_STATUS}' and availability_basis "
        f"'{CERTIFIED_AVAILABILITY_BASIS}' (historical_identity vocabulary). Labeled reconstructions "
        "are published but never certify.",
        universe_id=DEFAULT_US_LISTED_UNIVERSE_ID,
        certification_status=status,
    )


def _item_coverage_gate(
    store: DuckDBStore, created_at: dt.datetime, universes: Mapping[str, Mapping[str, object]]
) -> dict[str, object]:
    last_year = created_at.year - 1
    requirement = (
        f"FY{COVERAGE_FIRST_FISCAL_YEAR}-FY{last_year} {ITEM_COVERAGE_GATE_BASIS}: at least "
        f"{ITEM_COVERAGE_TARGET_ITEMS} items at >= {ITEM_COVERAGE_TARGET_PCT:g}% coverage in every completed "
        f"year of the {COHORT_SIZE}-name {ANNUAL_COHORT_UNIVERSE_ID} cohort, measured in the release year; "
        "the cohort is certified only when its listing universe is certified and its market-cap ranking "
        f"uses the strict-identity market panel ({MARKET_DAILY_STRICT_SOURCE_NAME!r})."
    )
    measured_row = store.con.execute(
        "SELECT max(as_of_date) FROM fundamental_item_coverage WHERE source = ? AND universe_id = ? AND basis = ?",
        [ITEM_COVERAGE_SOURCE, ANNUAL_COHORT_UNIVERSE_ID, ITEM_COVERAGE_GATE_BASIS],
    ).fetchone()
    measured = None if measured_row is None else measured_row[0]
    if measured is None:
        return _gate([], ["no_coverage_measurement"], requirement, measured_as_of_date=None)
    unmeasured: list[str] = []
    if measured > created_at.date():
        unmeasured.append("measurement_after_release_date")
    if measured.year != created_at.year:
        unmeasured.append("measurement_not_in_release_year")
    count_row = store.con.execute(
        coverage_gate_count_sql(
            source=ITEM_COVERAGE_SOURCE,
            universe_id=ANNUAL_COHORT_UNIVERSE_ID,
            as_of_date=measured,
            minimum_fiscal_year=COVERAGE_FIRST_FISCAL_YEAR,
            target_pct=ITEM_COVERAGE_TARGET_PCT,
            basis=ITEM_COVERAGE_GATE_BASIS,
        )
    ).fetchone()
    items = 0 if count_row is None else int(count_row[0])
    cohort = {
        int(row[0]): row[1:]
        for row in store.con.execute(
            """
            SELECT fiscal_year, status, selected_count, market_source,
                   CASE WHEN json_valid(rules_json)
                        THEN json_extract_string(rules_json, '$.listing_universe_id') END
            FROM item_coverage_cohort_years WHERE universe_id = ? AND fiscal_year BETWEEN ? AND ?
            """,
            [ANNUAL_COHORT_UNIVERSE_ID, COVERAGE_FIRST_FISCAL_YEAR, measured.year - 1],
        ).fetchall()
    }
    years: list[dict[str, object]] = []
    for fiscal_year in range(COVERAGE_FIRST_FISCAL_YEAR, measured.year):
        status, selected, market_source, listing = cohort.get(fiscal_year, (None, None, None, None))
        listing_status = (universes.get(str(listing)) or {}).get("certification_status") if listing else None
        certified = (
            status == "complete"
            and selected == COHORT_SIZE
            and listing_status == "certified"
            and market_source == MARKET_DAILY_STRICT_SOURCE_NAME
        )
        years.append(
            {
                "fiscal_year": fiscal_year,
                "cohort_status": status,
                "selected_count": selected,
                "listing_universe_id": listing,
                "listing_certification_status": listing_status,
                "market_source": market_source,
                "certified": certified,
            }
        )
    failed: list[str] = []
    if items < ITEM_COVERAGE_TARGET_ITEMS:
        failed.append("items_below_target")
    if any(not year["certified"] for year in years):
        failed.append("cohort_not_certified")
    if not years:
        unmeasured.append("no_completed_years")
    return _gate(
        failed,
        unmeasured,
        requirement,
        measured_as_of_date=_iso(measured),
        completed_fiscal_years=[COVERAGE_FIRST_FISCAL_YEAR, measured.year - 1],
        items_meeting_every_completed_year=items,
        target_items=ITEM_COVERAGE_TARGET_ITEMS,
        target_coverage_pct=ITEM_COVERAGE_TARGET_PCT,
        cohort_years=years,
    )


def _market_evidence(store: DuckDBStore) -> dict[str, object]:
    columns = _table_columns(store, "market_daily_metrics")
    identity_columns = ("identity_basis", "availability_basis", "link_method")
    missing = [column for column in (*identity_columns, "shares_source") if column not in columns]
    identity: object = {"status": GATE_UNMEASURED, "missing_columns": missing}
    shares: object = identity
    if not missing:
        rows = store.con.execute(
            f"SELECT source, {', '.join(identity_columns)}, count(*)::BIGINT FROM market_daily_metrics GROUP BY ALL"
        ).fetchall()
        identity = {
            "rows": _count_rows(rows, ("source", *identity_columns)),
            "verified_identity_rows": sum(
                int(row[-1])
                for row in rows
                if row[1] == CERTIFIED_EVIDENCE_STATUS and row[2] == CERTIFIED_AVAILABILITY_BASIS
            ),
            "unlinked_or_unlabeled_rows": sum(int(row[-1]) for row in rows if row[1] is None),
        }
        shares = _count_rows(
            store.con.execute(
                "SELECT source, shares_source, count(*)::BIGINT FROM market_daily_metrics GROUP BY ALL"
            ).fetchall(),
            ("source", "shares_source"),
        )
    bridges: dict[str, object] = {}
    for source, details, checked_at in store.con.execute(
        """
        SELECT json_extract_string(details_json, '$.source'), details_json, checked_at
        FROM data_quality_checks
        WHERE dataset_id = ? AND check_name = ? AND json_valid(details_json)
        QUALIFY row_number() OVER (
            PARTITION BY json_extract_string(details_json, '$.source') ORDER BY checked_at DESC, check_id DESC
        ) = 1
        """,
        [MarketDailyDataset.dataset_id, OWNER_BRIDGE_CHECK_NAME],
    ).fetchall():
        summary = json.loads(details)
        bridges[_key(source)] = {
            "checked_at": _iso(checked_at),
            **{
                name: summary.get(name)
                for name in (
                    "mode",
                    "identity_basis",
                    "availability_basis",
                    "lines",
                    "linked_lines",
                    "unlinked_lines",
                    "linked_by_identity_basis",
                    "unlinked_by_reason",
                    "share_basis_lines",
                )
            },
        }
    return {
        "availability_basis": MARKET_AVAILABILITY_BASIS,
        "availability_status": "modeled",
        "verified_vintage": False,
        "note": (
            "Bars and panel rows are modeled as known at trade_date + 22h. Owner links are verified only "
            "for identity_basis 'verified_dated' with availability_basis 'verified' (strict bridge); "
            "current_ticker_unverified links are the labeled RX1 reconstruction."
        ),
        "owner_identity": identity,
        "owner_bridge_by_source": bridges,
        "share_basis": {
            "rows_by_shares_source": shares,
            "shares_availability_basis": dict(SHARES_AVAILABILITY_BASIS),
            "vendor_share_run_clocks": dict(ARCHIVE_RUN_CLOCKS),
            "vendor_share_run_clock_status": "modeled",
            "vendor_share_run_clock_verified": False,
            "modeled_lag_days": ARCHIVE_MODELED_LAG_DAYS,
            # The per-row run clock kind is computed in the panel SQL but not stored.
            "rows_by_vendor_share_run_clock": GATE_UNMEASURED,
        },
    }


def _delisting_evidence(store: DuckDBStore) -> dict[str, object]:
    reasons = store.con.execute(
        """
        SELECT delist_reason, inferred_from_absence, evidence_confidence, security_id IS NULL, count(*)::BIGINT
        FROM delisting_events GROUP BY ALL
        """
    ).fetchall()
    identity = store.con.execute(
        """
        SELECT CASE WHEN json_valid(details_json)
                    THEN json_extract_string(details_json, '$.identity_basis') END, count(*)::BIGINT
        FROM delisting_events GROUP BY ALL
        """
    ).fetchall()
    return {
        "event_basis": "inferred",
        "verified_vintage": False,
        "note": (
            "Events are formed from public notices and price cessation (A3); gap-only events are "
            "inferred_from_absence. Terminal returns are observed or policy (RX2 Shumway convention for "
            "unexplained cessations); policy terminals are conventions, not observations."
        ),
        "rows": sum(int(row[-1]) for row in reasons),
        "inferred_from_absence_rows": sum(int(row[-1]) for row in reasons if row[1]),
        "unresolved_security_rows": sum(int(row[-1]) for row in reasons if row[3]),
        "rows_by_reason": _count_rows(
            reasons, ("delist_reason", "inferred_from_absence", "evidence_confidence", "unresolved_security")
        ),
        "rows_by_identity_basis": _count_rows(identity, ("identity_basis",)),
        "terminal_returns": delisting_policy_bias_exposure(store),
    }


def _forward_return_evidence(store: DuckDBStore) -> dict[str, object]:
    columns = _table_columns(store, "forward_returns_survivorship_safe")
    version = "calculation_version" if "calculation_version" in columns else "NULL::VARCHAR"
    rows = store.con.execute(
        f"""
        SELECT source, {version}, is_stitched, terminal_return_source, count(*)::BIGINT
        FROM forward_returns_survivorship_safe WHERE is_latest_revision GROUP BY ALL
        """
    ).fetchall()
    stitched = [row for row in rows if row[2]]
    return {
        "released": False,
        "expected_calculation_version": FORWARD_RETURN_CALCULATION_VERSION,
        "verified_vintage": False,
        "note": "Latest-revision survivorship-safe labels; a stitched label's terminal is observed or policy.",
        "rows": sum(int(row[-1]) for row in rows),
        "current_version_rows": sum(int(row[-1]) for row in rows if row[1] == FORWARD_RETURN_CALCULATION_VERSION),
        "unversioned_legacy_rows": sum(int(row[-1]) for row in rows if row[1] is None),
        "other_version_rows": sum(
            int(row[-1]) for row in rows if row[1] not in (None, FORWARD_RETURN_CALCULATION_VERSION)
        ),
        "stitched_observed_terminal_rows": sum(int(row[-1]) for row in stitched if row[3] == "observed"),
        "stitched_policy_terminal_rows": sum(int(row[-1]) for row in stitched if row[3] == "policy"),
        "stitched_other_terminal_rows": sum(int(row[-1]) for row in stitched if row[3] not in ("observed", "policy")),
        "rows_by_version": _count_rows(
            rows, ("source", "calculation_version", "is_stitched", "terminal_return_source")
        ),
    }


def _release_evidence(store: DuckDBStore, universes: Mapping[str, object]) -> dict[str, object]:
    fundamentals = {
        "availability_basis": FUNDAMENTALS_AVAILABILITY_BASIS,
        "clock_policy": FUNDAMENTAL_CLOCK_POLICY,
        "availability_status": "modeled",
        "verified_vintage": False,
        "note": (
            "SEC filing date + 46h: a conservative date policy, not measured EDGAR acceptance or "
            "delivery vintage. Every revision is published as its own row (is_latest_revision)."
        ),
    }
    standardized = store.con.execute(
        "SELECT source, is_latest_revision, count(*)::BIGINT FROM fundamental_standardized GROUP BY ALL"
    ).fetchall()
    derived_columns = _table_columns(store, "derived_metric_values")
    history = "history_status" if "history_status" in derived_columns else "NULL::VARCHAR"
    derived = store.con.execute(
        f"SELECT {history}, count(*)::BIGINT FROM derived_metric_values GROUP BY ALL"
    ).fetchall()
    security_master = store.con.execute(
        "SELECT source, count(*)::BIGINT FROM v_security_master_public GROUP BY ALL"
    ).fetchall()
    return {
        "security_master": {
            "identity_basis": "current_source_snapshot",
            "verified_vintage": False,
            "note": "Identifiers from current SEC company_tickers / vendor snapshots, not dated identity evidence.",
            "rows_by_source": _count_rows(security_master, ("source",)),
        },
        "universe": {
            "universes": universes,
            "identity_evidence": _identity_evidence_counts(store),
        },
        "delistings": _delisting_evidence(store),
        "fundamentals_core": {
            **fundamentals,
            "rows_by_source": _count_rows(standardized, ("source", "is_latest_revision")),
        },
        "derived_metrics": {
            **fundamentals,
            "note": (
                "Derived from fundamentals (and bars for daily windows); inherits their modeled clocks. "
                "history_status 'event_reconstructed' is reconstructed history, not a verified vintage."
            ),
            "rows_by_history_status": _count_rows(derived, ("history_status",)),
        },
        "market_daily": _market_evidence(store),
        "forward_returns": _forward_return_evidence(store),
    }


def publish_release(
    store: DuckDBStore,
    release_id: str,
    out_dir: Path | str,
    *,
    created_at: dt.datetime,
    previous_dir: Path | str | None = None,
    run_id: str | None = None,
) -> ReleaseResult:
    """Publish every release dataset to ``<out_dir>/<release_id>/`` with one manifest.

    The caller supplies the load stamp; no clock is read here. All six exports and ledger
    writes share one database snapshot. A staging directory is renamed only after all
    files are complete. Existing release IDs/directories cannot be overwritten.

    ``store`` must already be open, writable and migrated through the current schema.
    This library function never initializes or migrates it. The CLI checks for pending
    migrations read-only before opening a writable store.

    Previous files are verified against their manifest before diffing. A dataset absent
    from the previous manifest reports ``None`` for its diff counts. Column/type changes
    require a new baseline (omit ``previous_dir``); they cannot silently produce a diff.

    A release is always written; its manifest ``eligibility`` is ``eligible`` only when
    every stored release gate passes, otherwise ``candidate`` with ``gates_not_passed``.
    ``created_at`` also fixes the coverage gate's required fiscal years (FY2015 through
    the year before ``created_at``).
    """

    if not re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9_.-]*", release_id) or release_id.endswith("."):
        raise ValueError("release_id must be a simple directory name")
    if created_at.tzinfo is not None:
        created_at = created_at.astimezone(dt.UTC).replace(tzinfo=None)
    release_dir = Path(out_dir).resolve() / release_id
    if release_dir.exists():
        raise FileExistsError(f"Release directory already exists: {release_dir}")
    previous_release_id, previous_files = _previous_release(previous_dir)
    release_dir.parent.mkdir(parents=True, exist_ok=True)
    renamed = False
    try:
        with (
            tempfile.TemporaryDirectory(prefix=f".{release_id}-", dir=release_dir.parent) as staging,
            store.transaction(),
        ):
            if store.con.execute("SELECT 1 FROM publication_releases WHERE release_id = ?", [release_id]).fetchone():
                raise ValueError(f"Release ID already published: {release_id}")
            result = _publish_snapshot(
                store,
                release_id,
                release_dir,
                Path(staging),
                created_at,
                previous_release_id,
                previous_files,
                run_id,
            )
            Path(staging).rename(release_dir)
            renamed = True
        return result
    except Exception:
        if renamed:
            shutil.rmtree(release_dir)
        raise


def _publish_snapshot(
    store: DuckDBStore,
    release_id: str,
    release_dir: Path,
    staging: Path,
    created_at: dt.datetime,
    previous_release_id: str | None,
    previous_files: dict[str, Path],
    run_id: str | None,
) -> ReleaseResult:

    results: list[ReleaseDatasetResult] = []
    manifest_datasets: list[dict[str, object]] = []
    for dataset in RELEASE_DATASETS:
        schema = _object_schema(store, dataset.object_name)
        # The shared catalog helper's ORDER BY inherits DuckDB's default direction.
        schema.sort(key=lambda column: int(str(column["ordinal"])))
        columns = [str(column["name"]) for column in schema]
        query = release_query(dataset, columns)
        parquet_path = staging / f"{dataset.name}.parquet"
        _copy_parquet(store, query, parquet_path)
        count_row = store.con.execute("SELECT count(*) FROM read_parquet(?)", [str(parquet_path)]).fetchone()
        if count_row is None:
            raise RuntimeError(f"could not count rows for dataset {dataset.name!r}")
        row_count = int(count_row[0])
        added = removed = changed = None
        if dataset.name in previous_files:
            added, removed, changed = diff_against(store, dataset, columns, parquet_path, previous_files[dataset.name])
        else:
            _validate_keys(store, dataset, parquet_path)
        record_schema = _schema_for(dataset)
        record_sha = None if record_schema is None else _record_schema_sha256(record_schema)
        result = ReleaseDatasetResult(
            name=dataset.name,
            object_name=dataset.object_name,
            parquet_path=release_dir / parquet_path.name,
            row_count=row_count,
            byte_count=parquet_path.stat().st_size,
            schema_sha256=_schema_sha256(schema),
            query_sha256=_sha256_text(query),
            parquet_sha256=file_sha256(parquet_path),
            rows_added=added,
            rows_removed=removed,
            rows_changed=changed,
        )
        results.append(result)
        manifest_datasets.append(
            {
                "name": dataset.name,
                "object": dataset.object_name,
                "schema": dataset.schema_ref,
                "schema_sha256": result.schema_sha256,
                "record_schema_sha256": record_sha,
                "query_sha256": result.query_sha256,
                "query": query,
                "columns": schema,
                "key_columns": list(dataset.key_columns),
                "parquet": parquet_path.name,
                "parquet_sha256": result.parquet_sha256,
                "row_count": result.row_count,
                "byte_count": result.byte_count,
                "diff": {
                    "rows_added": result.rows_added,
                    "rows_removed": result.rows_removed,
                    "rows_changed": result.rows_changed,
                },
            }
        )

    # Evidence and gates share the exports' snapshot, so they describe exactly these files.
    floor, floor_block = _evidence_floor(store)
    source_pins, source_gate = _source_pins_and_gate(store)
    universes = _universe_evidence(store)
    gates: dict[str, dict[str, object]] = {
        "item_coverage": _item_coverage_gate(store, created_at, universes),
        "provider_slos": _provider_slo_gate(store, floor),
        "critical_dqc": _critical_dqc_gate(store, floor),
        "source_completeness": source_gate,
        "universe_certification": _universe_certification_gate(universes),
    }
    eligibility, gates_not_passed = release_eligibility(gates)
    manifest: dict[str, object] = {
        "release_id": release_id,
        "contract_version": PUBLICATION_CONTRACT_VERSION,
        "created_at": created_at.isoformat(),
        "previous_release_id": previous_release_id,
        "activation_stage_run_ids": activation_stage_run_ids(store),
        "datasets": manifest_datasets,
        "source_pins": source_pins,
        "evidence": _release_evidence(store, universes),
        "evidence_floor": floor_block,
        "gates": gates,
        "eligibility": eligibility,
        "gates_not_passed": list(gates_not_passed),
        "eligibility_rule": (
            "eligible only when every gate in "
            + ", ".join(RELEASE_GATES)
            + " is 'passed'; failed or unmeasured gates make the release a candidate"
        ),
    }
    payload = json.dumps(manifest, sort_keys=True, separators=(",", ":"))
    manifest_path = release_dir / MANIFEST_NAME
    (staging / MANIFEST_NAME).write_bytes(payload.encode("utf-8"))
    manifest_sha256 = _sha256_text(payload)

    store.con.execute(
        """
            INSERT INTO publication_releases (
                release_id, contract_version, out_dir, manifest_sha256, dataset_count,
                total_rows, previous_release_id, created_at, run_id, source_loaded_at
            ) VALUES (?,?,?,?,?,?,?,?,?,?)
            """,
        [
            release_id,
            PUBLICATION_CONTRACT_VERSION,
            str(release_dir),
            manifest_sha256,
            len(results),
            sum(result.row_count for result in results),
            previous_release_id,
            created_at,
            run_id,
            created_at,
        ],
    )
    store.con.executemany(
        """
            INSERT INTO publication_release_datasets (
                release_id, dataset_name, object_name, schema_code, parquet_path, row_count,
                byte_count, schema_sha256, query_sha256, parquet_sha256, rows_added,
                rows_removed, rows_changed, source_loaded_at
            ) VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?)
            """,
        [
            (
                release_id,
                result.name,
                result.object_name,
                dataset.schema_ref,
                str(result.parquet_path),
                result.row_count,
                result.byte_count,
                result.schema_sha256,
                result.query_sha256,
                result.parquet_sha256,
                result.rows_added,
                result.rows_removed,
                result.rows_changed,
                created_at,
            )
            for dataset, result in zip(RELEASE_DATASETS, results, strict=True)
        ],
    )

    return ReleaseResult(
        release_id=release_id,
        out_dir=release_dir,
        manifest_path=manifest_path,
        manifest_sha256=manifest_sha256,
        previous_release_id=previous_release_id,
        datasets=tuple(results),
        eligibility=eligibility,
        gates_not_passed=gates_not_passed,
    )
