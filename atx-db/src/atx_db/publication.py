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
    One block per release dataset stating what is modeled, reconstructed, inferred or
    policy: fundamentals
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

Coverage extension (P13, contract 1.2.0). Every :data:`RELEASE_DATASETS` entry is listed
in the manifest with its natural key, basis and evidence basis. A dataset is
``published`` (Parquet + schema hash), ``absent`` (its producer is not landed, not
persisted, or the research store is not attached; the reason is recorded) or
``withheld`` (present but its release key is not unique). Per-dataset ``eligibility``:
``research_only`` for research-basis datasets (never ``eligible``); ``candidate`` for a
production dataset produced outside the activation ladder (its freshness is not bound
by the gates); otherwise the release eligibility. Research-store datasets (RX6: earnings
events, factor returns/exposures, the qualified-signal ledger) are read from a research
store attached READ_ONLY for the duration of the publication.
"""

from __future__ import annotations

import datetime as dt
import hashlib
import json
import re
import shutil
import tempfile
from collections.abc import Callable, Iterable, Mapping, Sequence
from dataclasses import dataclass
from pathlib import Path
from typing import Any

import duckdb

from ._forward_return_publication import CALCULATION_VERSION as FORWARD_RETURN_CALCULATION_VERSION
from ._fundamental_clock import FUNDAMENTAL_CLOCK_POLICY
from .api.catalog import EXTENDED_DATASET_CODE, RecordSchema, _record_schema_sha256, get_schema
from .connection import DuckDBStore
from .delisting import DEFAULT_FORWARD_RETURN_SS_SOURCE, delisting_policy_bias_exposure
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
    ARCHIVE_MODELED_LAG_DAYS_BY_FAMILY,
    ARCHIVE_RUN_CLOCKS,
    MARKET_DAILY_STRICT_SOURCE_NAME,
    OWNER_BRIDGE_CHECK_NAME,
    SHARE_CLOCK_KINDS,
    SHARES_AVAILABILITY_BASIS,
    SHARES_SOURCES_WITHHELD,
    MarketDailyDataset,
)
from .market_owner_bridge import IDENTITY_BASIS_RECONSTRUCTED_HISTORY
from .provider_coverage import DEFAULT_PROVIDER_COVERAGE_SLOS
from .reference_classifications import (
    CLASSIFICATION_BASIS_CURRENT_SIC_SNAPSHOT,
    CLASSIFICATION_BASIS_NOTE,
    CLASSIFICATION_MAPPING_VERSIONS,
)
from .symbol_directory import IDENTITY_BASIS_CURRENT_TICKER
from .universe_us_listed import DEFAULT_US_LISTED_UNIVERSE_ID
from .warehouse import file_sha256

__all__ = [
    "BASIS_PRODUCTION",
    "BASIS_RESEARCH",
    "CANDIDATE",
    "CANDIDATE_EXIT_CODE",
    "DATASET_ABSENT",
    "DATASET_PUBLISHED",
    "DATASET_WITHHELD",
    "ELIGIBLE",
    "ELIGIBLE_EXIT_CODE",
    "EVIDENCE_STAGES",
    "FUNDAMENTALS_AVAILABILITY_BASIS",
    "GATE_FAILED",
    "GATE_PASSED",
    "GATE_UNMEASURED",
    "MANIFEST_NAME",
    "MARKET_AVAILABILITY_BASIS",
    "MONTHLY_FORWARD_LABEL_SOURCE",
    "PUBLICATION_CONTRACT_VERSION",
    "RELEASE_DATASETS",
    "RELEASE_DATASET_STAGES",
    "RELEASE_GATES",
    "RELEASE_SOURCES",
    "RESEARCH_ONLY",
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
# 1.2.0: additive P13 datasets; per-dataset status, natural key, basis, evidence basis and
# eligibility; absent/withheld datasets listed with a reason (no Parquet).
PUBLICATION_CONTRACT_VERSION = "1.2.0"
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
#: A daily-window derived metric needs both its filing inputs and its bars (C4 M5).
DERIVED_DAILY_AVAILABILITY_BASIS = f"max({FUNDAMENTALS_AVAILABILITY_BASIS}, {MARKET_AVAILABILITY_BASIS})"
#: research.labels.MONTHLY_LABEL_SOURCE (R3a); not imported: ``atx_db.research`` loads the
#: research stack on import.
MONTHLY_FORWARD_LABEL_SOURCE = "atx_forward_returns_survivorship_safe_monthly_v1"
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
#: Gate -> the activation stage that produces its evidence (runs after every release stage).
EVIDENCE_STAGES: dict[str, str] = {
    "item_coverage": "item_coverage",
    "provider_slos": "provider_coverage",
    "critical_dqc": "quality",
}
_FAILURE_STATUSES = frozenset({"failed", "error"})
_LIST_CAP = 50
_PIN_CAP = 32

# --- Dataset basis, store and status (P13) ---------------------------------------------
#: ``production`` datasets follow the release eligibility; ``research`` never is eligible.
BASIS_PRODUCTION = "production"
BASIS_RESEARCH = "research"
RESEARCH_ONLY = "research_only"
STORE_WAREHOUSE = "warehouse"
#: The separate research store (ruling RX6), attached READ_ONLY under this alias.
STORE_RESEARCH = "research"
RESEARCH_STORE_ALIAS = "atx_release_research"
DATASET_PUBLISHED = "published"
DATASET_ABSENT = "absent"
DATASET_WITHHELD = "withheld"


@dataclass(frozen=True)
class ReleaseDataset:
    name: str
    object_name: str
    #: Physical release key: unique per exported row, the Parquet ORDER BY and diff key.
    key_columns: tuple[str, ...]
    schema_dataset: str | None
    schema_code: str | None
    #: Logical key a consumer ranks revisions within (defaults to ``key_columns``).
    natural_key: tuple[str, ...] = ()
    basis: str = BASIS_PRODUCTION
    store: str = STORE_WAREHOUSE
    #: Activation stage producing it; a production dataset's stage bounds gate freshness.
    stage: str | None = None
    #: Trusted constant SQL predicate selecting this dataset's rows of ``object_name``.
    row_filter: str | None = None
    #: The six original datasets: always exported; a missing object fails the release.
    required: bool = False
    #: Static reason when no persisted producer exists: listed absent, never exported.
    absent_reason: str | None = None
    #: Short label of what the dataset's clocks/identity rest on (details in ``evidence``).
    evidence_basis: str | None = None

    @property
    def schema_ref(self) -> str | None:
        if self.schema_dataset is None or self.schema_code is None:
            return None
        return f"{self.schema_dataset}/{self.schema_code}"


def _source_filter(source: str) -> str:
    return f"\"source\" = '{source}'"


_RESEARCH_EVIDENCE_BASIS = "research_store_rx6"

# The six full-universe release datasets (Tier1-S4 Task 8), then the P13 extension.
# ``fundamentals_core`` cites the pre-existing ``standardized`` schema code rather than a
# fourth fundamentals-core schema code -- see the S4 preflight ruling recorded in
# .superpowers/sdd/tier1-parity/program.md ("no fourth fundamentals-core schema code").
RELEASE_DATASETS: tuple[ReleaseDataset, ...] = (
    ReleaseDataset(
        "security_master",
        "v_security_master_public",
        ("security_id",),
        "ATX.US.FUNDAMENTALS",
        "security-master",
        stage="security_master",
        required=True,
        evidence_basis="current_source_snapshot",
    ),
    ReleaseDataset(
        "universe",
        "universe_us_listed_membership",
        ("universe_id", "security_id", "valid_from"),
        "ATX.US.EQUITIES",
        "universe",
        stage="universe_us_listed",
        required=True,
        evidence_basis="per_universe_evidence_status",
    ),
    ReleaseDataset(
        "delistings",
        "delisting_events",
        # The public logical key contains nullable security IDs; use the physical event
        # key to retain every event, including unresolved symbols and evidence revisions.
        ("delisting_event_id",),
        "ATX.US.EQUITIES",
        "delistings",
        natural_key=("source", "security_id", "symbol", "delist_date"),
        stage="delisting_evidence",
        required=True,
        evidence_basis="inferred_events_observed_or_policy_terminals",
    ),
    ReleaseDataset(
        "fundamentals_core",
        "fundamental_standardized",
        ("standardized_id",),
        "ATX.US.FUNDAMENTALS",
        "standardized",
        natural_key=("security_id", "item_id", "basis", "period_end"),
        stage="standardized",
        required=True,
        evidence_basis=FUNDAMENTALS_AVAILABILITY_BASIS,
    ),
    ReleaseDataset(
        "derived_metrics",
        "derived_metric_values",
        # Physical event key preserves invalid states and same-value lineage
        # revisions. Never filter this release to valid/latest-only rows.
        ("derived_value_id",),
        "ATX.US.FUNDAMENTALS",
        "derived-metrics",
        natural_key=("revision_group_id",),
        stage="derived_metrics",
        required=True,
        evidence_basis="per_metric_window",
    ),
    ReleaseDataset(
        "market_daily",
        "market_daily_metrics",
        ("market_daily_id",),
        "ATX.US.EQUITIES",
        "market-daily-1d",
        natural_key=("security_id", "trade_date"),
        stage="market_daily",
        required=True,
        evidence_basis=MARKET_AVAILABILITY_BASIS,
    ),
    # --- P13: survivorship-safe forward labels (R3a), one table, split by source --------
    ReleaseDataset(
        "forward_labels_daily",
        "forward_returns_survivorship_safe",
        ("forward_return_id",),
        EXTENDED_DATASET_CODE,
        "forward-labels",
        natural_key=("source", "security_id", "as_of_date", "horizon_days"),
        stage="survivorship_forward_returns",
        row_filter=_source_filter(DEFAULT_FORWARD_RETURN_SS_SOURCE),
        evidence_basis="observed_or_policy_terminal_stitch",
    ),
    ReleaseDataset(
        "forward_labels_monthly",
        "forward_returns_survivorship_safe",
        ("forward_return_id",),
        EXTENDED_DATASET_CODE,
        "forward-labels",
        natural_key=("source", "security_id", "as_of_date", "horizon_days"),
        # Published by research.labels.refresh_monthly_forward_labels, outside activation.
        row_filter=_source_filter(MONTHLY_FORWARD_LABEL_SOURCE),
        evidence_basis="observed_or_policy_terminal_stitch",
    ),
    ReleaseDataset(
        "equity_price_metrics",
        "equity_price_metrics",
        ("metric_id",),
        EXTENDED_DATASET_CODE,
        "price-metrics-1d",
        natural_key=("source", "security_id", "trade_date"),
        stage="equity_price_metrics",
        evidence_basis=MARKET_AVAILABILITY_BASIS,
    ),
    ReleaseDataset(
        "listing_status",
        "listing_status_intervals",
        ("listing_status_id",),
        EXTENDED_DATASET_CODE,
        "listing-status",
        natural_key=("source", "symbol", "listing_venue_code", "valid_from"),
        stage="listing_status",
        evidence_basis=IDENTITY_BASIS_CURRENT_TICKER,
    ),
    ReleaseDataset(
        "owner_links",
        "market_owner_bridge_links",
        ("security_id", "owner_security_id", "valid_from"),
        None,
        None,
        absent_reason=(
            "not_persisted: the A5/P1 owner bridge is computed inside market_daily; per-row links are released "
            "in market_daily (owner_security_id, identity_basis, availability_basis, link_method). A standalone "
            "link table awaits the post-B0 0328 bundle."
        ),
        evidence_basis="owner_bridge_identity_basis",
    ),
    ReleaseDataset(
        "entity_classification",
        "entity_classification",
        ("classification_id",),
        EXTENDED_DATASET_CODE,
        "classification",
        natural_key=("security_id", "taxonomy_id", "is_primary", "valid_from"),
        basis=BASIS_RESEARCH,
        stage="entity_classification",
        evidence_basis=CLASSIFICATION_BASIS_CURRENT_SIC_SNAPSHOT,
    ),
    ReleaseDataset(
        "corporate_action_events",
        "corporate_actions",
        # No physical id column: the event key plus its clocks (duplicates -> withheld).
        ("source", "security_id", "ex_date", "action_type", "available_at", "source_loaded_at"),
        EXTENDED_DATASET_CODE,
        "corporate-actions",
        natural_key=("source", "security_id", "ex_date", "action_type"),
        # Built by scripts/build_corporate_action_events.py (P8), outside activation.
        evidence_basis="reconstructed_vendor_factor",
    ),
    ReleaseDataset(
        "foreign_filer_disclosure",
        "foreign_filer_reason_intervals",
        ("cik", "reason_code", "interval_no"),
        None,
        None,
        evidence_basis="companyfacts_archive_snapshot",
    ),
    # --- P13: research-store datasets (RX6); research basis, never eligible -------------
    ReleaseDataset(
        "earnings_events",
        "research_earnings_events",
        ("event_version", "owner_cik", "fiscal_period_end"),
        None,
        None,
        basis=BASIS_RESEARCH,
        store=STORE_RESEARCH,
        evidence_basis=_RESEARCH_EVIDENCE_BASIS,
    ),
    ReleaseDataset(
        "factor_returns",
        "research_factor_returns",
        ("run_id", "frequency", "period_date", "factor_id"),
        None,
        None,
        basis=BASIS_RESEARCH,
        store=STORE_RESEARCH,
        evidence_basis=_RESEARCH_EVIDENCE_BASIS,
    ),
    ReleaseDataset(
        "factor_exposures",
        "research_factor_exposures",
        ("run_id", "formation_date", "security_id"),
        None,
        None,
        basis=BASIS_RESEARCH,
        store=STORE_RESEARCH,
        evidence_basis=_RESEARCH_EVIDENCE_BASIS,
    ),
    ReleaseDataset(
        "qualified_signals",
        "research_qualification_features",
        ("ledger_id", "feature_id"),
        None,
        None,
        basis=BASIS_RESEARCH,
        store=STORE_RESEARCH,
        evidence_basis=_RESEARCH_EVIDENCE_BASIS,
    ),
)
#: Activation stage producing each released production dataset. Gate evidence recorded
#: before the newest completed run of any of them describes an older warehouse state.
#: Research-basis datasets never bound the floor: they are never eligible.
RELEASE_DATASET_STAGES: dict[str, str] = {
    dataset.name: dataset.stage
    for dataset in RELEASE_DATASETS
    if dataset.stage is not None and dataset.basis == BASIS_PRODUCTION and dataset.absent_reason is None
}
#: Why a dataset has no public record schema (manifest ``schema_absent_reason``).
_NO_API_SCHEMA_REASONS: dict[str, str] = {
    "owner_links": "no persisted relation to serve",
    "foreign_filer_disclosure": (
        "CIK-keyed reason intervals (valid_from/valid_to clocks) without the security_id, as_of_date and "
        "available_at columns the PIT range service ranks on"
    ),
}
_RESEARCH_API_REASON = "research store outputs (RX6) are not served by the warehouse API"
_FOREIGN_FILER_ABSENT = (
    "not_persisted: P11 builds foreign_filer_reason_intervals as a TEMP table and parquet artifacts "
    "(foreign_filers.write_taxonomy_artifacts); no warehouse table"
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
    # Fail-safe defaults for any external constructor: an unevaluated release is a candidate.
    eligibility: str = CANDIDATE
    gates_not_passed: tuple[str, ...] = RELEASE_GATES

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
    where = "" if dataset.row_filter is None else f" WHERE {dataset.row_filter}"
    return f"SELECT {projection} FROM {_relation(dataset)}{where} ORDER BY {order}"


def _relation(dataset: ReleaseDataset) -> str:
    """The quoted relation a dataset reads (research-store objects are catalog-qualified)."""

    if dataset.store == STORE_RESEARCH:
        return f'{_quote(RESEARCH_STORE_ALIAS)}."main".{_quote(dataset.object_name)}'
    return _quote(dataset.object_name)


def row_digest_sql(alias: str, columns: list[str]) -> str:
    """Hash a JSON tuple so NULL positions and embedded separators are preserved."""

    parts = ", ".join(f"{_quote(alias)}.{_quote(column)}" for column in columns)
    return f"sha256(CAST(to_json(row({parts})) AS VARCHAR))"


def _parquet_columns(store: DuckDBStore, path: Path) -> list[tuple[str, str]]:
    return [
        (str(row[0]), str(row[1]))
        for row in store.con.execute("DESCRIBE SELECT * FROM read_parquet(?)", [str(path)]).fetchall()
    ]


def _has_duplicate_keys(store: DuckDBStore, dataset: ReleaseDataset, path: Path) -> bool:
    keys = ", ".join(_quote(column) for column in dataset.key_columns)
    duplicate = store.con.execute(
        f"SELECT 1 FROM read_parquet(?) GROUP BY {keys} HAVING count(*) > 1 LIMIT 1", [str(path)]
    ).fetchone()
    return duplicate is not None


def _validate_keys(store: DuckDBStore, dataset: ReleaseDataset, path: Path) -> None:
    if _has_duplicate_keys(store, dataset, path):
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
    seen: set[str] = set()
    for entry in datasets:
        if not isinstance(entry, dict) or not isinstance(entry.get("name"), str):
            raise ValueError("Invalid previous release dataset")
        name = entry["name"]
        if not re.fullmatch(r"[A-Za-z0-9_]+", name) or name in seen:
            raise ValueError("Invalid or duplicate previous release dataset path")
        seen.add(name)
        # Contract < 1.2.0 entries carry no status: every one of them was published.
        if entry.get("status", DATASET_PUBLISHED) != DATASET_PUBLISHED:
            continue
        if entry.get("parquet") != f"{name}.parquet":
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


def _sort_key(value: object) -> tuple[bool, str, str]:
    """Total order for mixed values: None, 'null', True and 'True' never tie."""

    return (value is not None, type(value).__name__, str(value))


def _marks(values: Sequence[object]) -> str:
    return ", ".join(["?"] * len(values))


def _table_columns(store: DuckDBStore, table: str) -> set[str]:
    rows = store.con.execute(
        "SELECT column_name FROM duckdb_columns() "
        "WHERE database_name = current_database() AND schema_name = 'main' AND table_name = ?",
        [table],
    ).fetchall()
    return {str(row[0]) for row in rows}


def _catalog_schema(store: DuckDBStore, database: str | None, name: str) -> list[dict[str, object]]:
    """``lake._object_schema``'s shape for ``name`` in one catalog (``None``: the warehouse)."""

    rows = store.con.execute(
        "SELECT column_name, data_type, is_nullable, column_index FROM duckdb_columns() "
        "WHERE database_name = coalesce(?, current_database()) AND schema_name = 'main' AND table_name = ? "
        "ORDER BY column_index",
        [database, name],
    ).fetchall()
    return [
        {"ordinal": int(index), "name": str(column), "type": str(kind), "nullable": bool(nullable)}
        for column, kind, nullable, index in rows
    ]


def _rounded(value: object) -> object:
    """Floats rounded to 12 places: multi-threaded float aggregates can differ in the last ulp."""

    if isinstance(value, float):
        return round(value, 12)
    if isinstance(value, Mapping):
        return {key: _rounded(item) for key, item in value.items()}
    if isinstance(value, list | tuple):
        return [_rounded(item) for item in value]
    return value


def _count_rows(rows: Iterable[Sequence[Any]], fields: Sequence[str]) -> list[dict[str, object]]:
    """``[(f1, f2, ..., count)]`` -> sorted ``[{f1:, f2:, ..., rows:}]`` (NULL kept as null)."""

    items = [
        {**{field: _iso(value) for field, value in zip(fields, row[:-1], strict=True)}, "rows": int(row[-1])}
        for row in rows
    ]
    return sorted(items, key=lambda item: tuple(_sort_key(item[field]) for field in fields))


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


@dataclass(frozen=True)
class _Attempt:
    run_id: str
    status: str
    started_at: dt.datetime
    finished_at: dt.datetime | None


@dataclass(frozen=True)
class _StageLedger:
    newest: _Attempt | None
    completed: _Attempt | None
    latest_stamp: dt.datetime | None


def _stage_ledger(store: DuckDBStore, stages: Sequence[str]) -> dict[str, _StageLedger]:
    """Every activation attempt per stage, newest first (``activation.completed_stages`` order).

    ``activation_stage_runs`` is the single wall clock gate freshness is judged on:
    activation stamps SLO snapshots with the pinned ``as_of_date`` 22:00, not the run time.
    """

    rows = store.con.execute(
        f"SELECT stage, run_id, status, started_at, finished_at FROM activation_stage_runs "
        f"WHERE stage IN ({_marks(stages)})",
        list(stages),
    ).fetchall()
    ledger: dict[str, _StageLedger] = {}
    for stage in stages:
        attempts = sorted(
            (_Attempt(str(row[1]), str(row[2]), row[3], row[4]) for row in rows if row[0] == stage),
            key=lambda attempt: (attempt.started_at, attempt.run_id),
            reverse=True,
        )
        ledger[stage] = _StageLedger(
            newest=attempts[0] if attempts else None,
            completed=next((attempt for attempt in attempts if attempt.status == "completed"), None),
            latest_stamp=max((max(a.started_at, a.finished_at or a.started_at) for a in attempts), default=None),
        )
    return ledger


def _attempt_block(attempt: _Attempt | None) -> dict[str, object] | None:
    if attempt is None:
        return None
    return {
        "run_id": attempt.run_id,
        "status": attempt.status,
        "started_at": _iso(attempt.started_at),
        "finished_at": _iso(attempt.finished_at),
    }


def _evidence_floor(ledger: Mapping[str, _StageLedger]) -> tuple[dt.datetime | None, list[str], dict[str, object]]:
    """The latest activation stamp (any attempt, any status) of a release-producing stage.

    A release stage whose newest attempt is not ``completed`` (failed, running: batches may
    be half-committed) or that never completed makes every freshness-bound gate unmeasured.
    """

    stages = sorted(set(RELEASE_DATASET_STAGES.values()))
    reasons: list[str] = []
    for stage in stages:
        entry = ledger[stage]
        if entry.completed is None:
            reasons.append(f"release_stage_never_completed:{stage}")
        elif entry.newest is not None and entry.newest.status != "completed":
            reasons.append(f"release_stage_not_completed:{stage}")
    stamps = [stamp for stage in stages if (stamp := ledger[stage].latest_stamp) is not None]
    floor = max(stamps, default=None)
    return (
        floor,
        reasons,
        {
            "floor": _iso(floor),
            "unmeasured_reasons": reasons,
            "stages": {
                stage: {
                    "newest_attempt": _attempt_block(ledger[stage].newest),
                    "newest_completed": _attempt_block(ledger[stage].completed),
                }
                for stage in stages
            },
            "evidence_stages": {
                stage: {
                    "newest_attempt": _attempt_block(ledger[stage].newest),
                    "newest_completed": _attempt_block(ledger[stage].completed),
                }
                for stage in sorted(EVIDENCE_STAGES.values())
            },
            "rule": (
                "Gate evidence counts only when produced by the newest, completed attempt of its "
                "evidence stage (item_coverage, provider_coverage, quality) started at or after the "
                "latest activation stamp of any release-producing stage, whose newest attempts must all "
                "be completed. Writers outside activation must be followed by item_coverage, "
                "provider_coverage and quality runs before publishing."
            ),
        },
    )


def _evidence_run(
    ledger: Mapping[str, _StageLedger], stage: str, floor: dt.datetime | None, floor_reasons: Sequence[str]
) -> tuple[_Attempt | None, list[str]]:
    """The completed evidence-stage attempt a gate may use, or the reasons it may not."""

    entry = ledger[stage]
    reasons = list(floor_reasons)
    if entry.completed is None:
        reasons.append(f"{stage}_never_completed")
        return None, reasons
    if entry.newest is not None and entry.newest.status != "completed":
        reasons.append(f"{stage}_not_completed")
    if floor is not None and entry.completed.started_at < floor:
        reasons.append(f"{stage}_older_than_release_data")
    return entry.completed, reasons


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
            ((pin, int(n), at) for source, pin, n, at in by_pin if source == dataset_id), key=lambda r: _sort_key(r[0])
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
            # Digest of the full sorted distinct-hash set, so a truncated list still pins it.
            "sha256_set_digest": hashlib.sha256("\n".join(str(row[0]) for row in hashed).encode()).hexdigest(),
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


def _critical_dqc_gate(
    store: DuckDBStore, run: _Attempt | None, run_reasons: Sequence[str], created_at: dt.datetime
) -> dict[str, object]:
    """Every enabled critical check (registry, plus any recorded critical result) passed, fresh.

    ``data_quality_checks.checked_at`` is wall-clock insertion time, the ledger's clock:
    a result counts only when recorded at or after the start of the newest completed
    ``quality`` attempt (``run``) and not after the release's ``created_at``. Registered
    checks join results by name (the registry's key), so only the latest variant per
    (dataset_id, table_name, check_name) recorded inside that window is evaluated (C4 N1):
    an obsolete variant recorded under another dataset_id/table_name by an older run can
    neither fail nor stale a check forever. A check with only older variants is ``stale``.
    """

    window_start = None if run is None else run.started_at
    rows = store.con.execute(
        """
        WITH latest AS (
            SELECT dataset_id, table_name, check_name, status, severity, checked_at,
                   status = 'warning' AND json_valid(details_json)
                       AND json_extract(details_json, '$.missing_tables') IS NOT NULL AS missing_inputs,
                   (CAST(? AS TIMESTAMP) IS NULL OR checked_at >= CAST(? AS TIMESTAMP)) AS in_window
            FROM data_quality_checks
            QUALIFY row_number() OVER (
                PARTITION BY dataset_id, table_name, check_name ORDER BY checked_at DESC, check_id DESC
            ) = 1
        ), registry AS (
            SELECT check_name, dataset_id, enabled, severity FROM quality_check_registry
        ), critical AS (
            -- The registry is keyed by check_name (quality._resolve_spec); results may carry
            -- another dataset_id than the registry row, so registered checks join by name.
            SELECT check_name, dataset_id, true AS registered
            FROM registry WHERE enabled AND severity = 'critical'
            UNION ALL
            SELECT l.check_name, min(l.dataset_id), false
            FROM latest l
            WHERE l.severity = 'critical' AND l.in_window
              AND NOT EXISTS (SELECT 1 FROM registry r WHERE r.check_name = l.check_name)
            GROUP BY l.check_name
        )
        SELECT c.dataset_id, c.check_name, c.registered,
               count(l.status) FILTER (WHERE l.in_window)::BIGINT,
               count(l.status) FILTER (WHERE l.in_window AND l.status = 'passed')::BIGINT,
               count(l.status) FILTER (WHERE l.in_window AND (l.status = 'skipped' OR l.missing_inputs))::BIGINT,
               count(l.status) FILTER (WHERE NOT l.in_window)::BIGINT,
               max(l.checked_at) FILTER (WHERE l.in_window)
        FROM critical c
        LEFT JOIN latest l ON l.check_name = c.check_name
        GROUP BY c.dataset_id, c.check_name, c.registered
        """,
        [window_start, window_start],
    ).fetchall()
    outcomes: dict[str, list[str]] = {
        "passed": [],
        "failed": [],
        "not_run": [],
        "not_evaluated": [],
        "stale": [],
        "recorded_after_release": [],
    }
    registered = 0
    for dataset_id, check_name, is_registered, results, passed, not_evaluated, older, newest in rows:
        name = f"{dataset_id}/{check_name}"
        registered += bool(is_registered)
        if not results:
            # Nothing recorded in the quality run's window: stale when only older variants exist.
            outcomes["stale" if older else "not_run"].append(name)
        elif results - passed - not_evaluated:
            # Any other latest status (failed, error, a warning-grade failure) fails a
            # critical check: failure_status='warning' never downgrades it.
            outcomes["failed"].append(name)
        elif not_evaluated:
            # Skipped, or a warning recorded because the check's input tables were missing.
            outcomes["not_evaluated"].append(name)
        elif newest > created_at:
            outcomes["recorded_after_release"].append(name)
        else:
            outcomes["passed"].append(name)
    unmeasured = [
        outcome for outcome in ("not_run", "not_evaluated", "stale", "recorded_after_release") if outcomes[outcome]
    ]
    if not rows:
        unmeasured.append("no_critical_checks")
    unmeasured.extend(run_reasons)
    failed_checks, failed_truncated = _capped(outcomes["failed"])
    open_checks, open_truncated = _capped(
        [
            name
            for outcome in ("not_run", "not_evaluated", "stale", "recorded_after_release")
            for name in outcomes[outcome]
        ]
    )
    return _gate(
        ["critical_check_failed"] if outcomes["failed"] else [],
        unmeasured,
        "Every enabled critical quality check has a latest result 'passed', recorded (wall clock) "
        "at or after the start of the newest completed 'quality' activation attempt, which itself "
        "started after all release data, and not after the release; only variants recorded in that "
        "window are evaluated (any failed one fails the check); skipped, missing-input, not-run and "
        "stale (only older variants) checks never pass.",
        quality_run=_attempt_block(run),
        critical_checks=len(rows),
        registered_critical_checks=registered,
        outcomes={outcome: len(names) for outcome, names in outcomes.items()},
        failed_checks=failed_checks,
        unresolved_checks=open_checks,
        lists_truncated=failed_truncated or open_truncated,
    )


def _provider_slo_gate(
    store: DuckDBStore, run: _Attempt | None, run_reasons: Sequence[str], created_at: dt.datetime
) -> dict[str, object]:
    """Every active provider SLO (a superset of the code defaults) available in the latest run.

    Snapshot ``observed_at`` is the pinned ``as_of_date`` 22:00 in activation, not a wall
    clock, so freshness comes from the ledger: every latest snapshot must carry the run id
    of the newest completed ``provider_coverage`` attempt (``run``).
    """

    rows = store.con.execute(
        """
        WITH slos AS (
            SELECT dataset_id, schema_code, slo_version
            FROM api_schema_coverage_slo WHERE is_active
            QUALIFY row_number() OVER (
                PARTITION BY dataset_id, schema_code ORDER BY valid_from DESC, slo_version DESC
            ) = 1
        ), snaps AS (
            SELECT dataset_id, schema_code, slo_version, condition, observed_at, run_id
            FROM api_schema_coverage_snapshot
            QUALIFY row_number() OVER (
                PARTITION BY dataset_id, schema_code
                ORDER BY observed_at DESC, source_loaded_at DESC, coverage_snapshot_id DESC
            ) = 1
        )
        SELECT s.dataset_id, s.schema_code, s.slo_version, p.slo_version, p.condition, p.observed_at, p.run_id
        FROM slos s LEFT JOIN snaps p ON p.dataset_id = s.dataset_id AND p.schema_code = s.schema_code
        """
    ).fetchall()
    # activation.stage_provider_coverage: ProviderCoverageOptions(run_id=f"{options.run_id}-coverage").
    expected_run_id = None if run is None else f"{run.run_id}-coverage"
    required = sorted({(slo.dataset_id, slo.schema_code) for slo in DEFAULT_PROVIDER_COVERAGE_SLOS})
    active = {(str(row[0]), str(row[1])) for row in rows}
    inactive_required = [
        f"{dataset_id}/{schema_code}" for dataset_id, schema_code in required if (dataset_id, schema_code) not in active
    ]
    slos: list[dict[str, object]] = []
    failed: list[str] = ["required_slo_not_active"] if inactive_required else []
    unmeasured: list[str] = [] if rows else ["no_active_slos"]
    for dataset_id, schema_code, active_version, measured_version, condition, observed_at, run_id in rows:
        if condition is None:
            outcome = "not_run"
        elif measured_version != active_version:
            outcome = "slo_version_mismatch"
        elif condition != "available":
            outcome = f"condition_{condition}"
        elif run_id != expected_run_id:
            outcome = "not_from_latest_coverage_run"
        elif observed_at > created_at:
            outcome = "observed_after_release"
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
                "run_id": run_id,
            }
        )
    unmeasured.extend(run_reasons)
    return _gate(
        failed,
        unmeasured,
        "Every active provider coverage SLO (including every code-defined default) has a latest "
        "snapshot with condition 'available' at the active SLO version, written by the newest completed "
        "'provider_coverage' activation attempt (run id '<run>-coverage'), which started after all "
        "release data, and observed no later than the release. Activation stamps snapshots at the "
        "pinned as_of_date 22:00, so a ladder pinned to the release date keeps a release published "
        "before 22:00 that day a candidate (observed_after_release): pin the ladder to the last "
        "completed session or publish after 22:00.",
        coverage_run=_attempt_block(run),
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


def _cohort_members(store: DuckDBStore, last_year: int) -> dict[int, tuple[int, int, int, object, object]]:
    """Per cohort year: (members, missing memberships, missing market rows, min/max member universe).

    Bounded: the ids are semi-joined into the big tables (at most 3000 per year).
    """

    rows = store.con.execute(
        """
        WITH cohort AS (
            SELECT fiscal_year, membership_id, market_daily_id FROM item_coverage_annual_cohort
            WHERE universe_id = ? AND fiscal_year BETWEEN ? AND ?
        ), members AS (
            SELECT DISTINCT membership_id, universe_id FROM universe_us_listed_membership
            WHERE membership_id IN (SELECT membership_id FROM cohort)
        ), market AS (
            SELECT DISTINCT market_daily_id FROM market_daily_metrics
            WHERE market_daily_id IN (SELECT market_daily_id FROM cohort)
        )
        SELECT c.fiscal_year, count(*)::BIGINT,
               count(*) FILTER (WHERE u.membership_id IS NULL)::BIGINT,
               count(*) FILTER (WHERE m.market_daily_id IS NULL)::BIGINT,
               min(u.universe_id), max(u.universe_id)
        FROM cohort c
        LEFT JOIN members u ON u.membership_id = c.membership_id
        LEFT JOIN market m ON m.market_daily_id = c.market_daily_id
        GROUP BY c.fiscal_year
        """,
        [ANNUAL_COHORT_UNIVERSE_ID, COVERAGE_FIRST_FISCAL_YEAR, last_year],
    ).fetchall()
    return {int(row[0]): (int(row[1]), int(row[2]), int(row[3]), row[4], row[5]) for row in rows}


def _item_coverage_gate(
    store: DuckDBStore,
    created_at: dt.datetime,
    universes: Mapping[str, Mapping[str, object]],
    run: _Attempt | None,
    run_reasons: Sequence[str],
) -> dict[str, object]:
    last_year = created_at.year - 1
    requirement = (
        f"FY{COVERAGE_FIRST_FISCAL_YEAR}-FY{last_year} {ITEM_COVERAGE_GATE_BASIS}: at least "
        f"{ITEM_COVERAGE_TARGET_ITEMS} items at >= {ITEM_COVERAGE_TARGET_PCT:g}% coverage in every completed "
        f"year of the {COHORT_SIZE}-name {ANNUAL_COHORT_UNIVERSE_ID} cohort, measured in the release year by "
        "the newest completed 'item_coverage' activation attempt, which started after all release data; "
        "a cohort year is certified only when its 3000 members still resolve to current membership rows of "
        "its certified listing universe and to current market-panel rows, and its market-cap ranking uses "
        f"the strict-identity market panel ({MARKET_DAILY_STRICT_SOURCE_NAME!r})."
    )
    measured_row = store.con.execute(
        "SELECT max(as_of_date) FROM fundamental_item_coverage WHERE source = ? AND universe_id = ? AND basis = ?",
        [ITEM_COVERAGE_SOURCE, ANNUAL_COHORT_UNIVERSE_ID, ITEM_COVERAGE_GATE_BASIS],
    ).fetchone()
    measured = None if measured_row is None else measured_row[0]
    if measured is None:
        return _gate(
            [],
            ["no_coverage_measurement", *run_reasons],
            requirement,
            coverage_run=_attempt_block(run),
            measured_as_of_date=None,
        )
    unmeasured: list[str] = list(run_reasons)
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
    members = _cohort_members(store, measured.year - 1)
    years: list[dict[str, object]] = []
    for fiscal_year in range(COVERAGE_FIRST_FISCAL_YEAR, measured.year):
        status, selected, market_source, listing = cohort.get(fiscal_year, (None, None, None, None))
        count, missing_membership, missing_market, low, high = members.get(fiscal_year, (0, 0, 0, None, None))
        listing_status = (universes.get(str(listing)) or {}).get("certification_status") if listing else None
        certified = (
            status == "complete"
            and selected == COHORT_SIZE
            and count == COHORT_SIZE
            and not missing_membership
            and not missing_market
            and low == high == listing
            and listing_status == "certified"
            and market_source == MARKET_DAILY_STRICT_SOURCE_NAME
        )
        years.append(
            {
                "fiscal_year": fiscal_year,
                "cohort_status": status,
                "selected_count": selected,
                "member_rows": count,
                "members_missing_membership": missing_membership,
                "members_missing_market_row": missing_market,
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
        coverage_run=_attempt_block(run),
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
    withheld: object = GATE_UNMEASURED
    if isinstance(shares, list):
        withheld = sum(int(str(item["rows"])) for item in shares if item["shares_source"] in SHARES_SOURCES_WITHHELD)
    bridges: dict[str, dict[str, object]] = {}
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
                    # P1 bridge rule 5: RI1 reconstructed-history links by PIT tier.
                    "reconstructed_history",
                )
            },
            "rows_by_share_clock": (summary.get("share_basis") or {}).get("rows_by_share_clock"),
        }
    # A8 N6: the per-row share clock is stored only once the post-B0 0328 columns exist;
    # before that the owner-bridge quality row of each source's latest run counts it.
    clocks: object = GATE_UNMEASURED
    if "shares_clock" in columns:
        clocks = {
            "measured_from": "market_daily_metrics.shares_clock",
            "rows": _count_rows(
                store.con.execute(
                    "SELECT source, shares_clock, count(*)::BIGINT FROM market_daily_metrics GROUP BY ALL"
                ).fetchall(),
                ("source", "shares_clock"),
            ),
        }
    elif any(bridge["rows_by_share_clock"] is not None for bridge in bridges.values()):
        clocks = {
            "measured_from": f"latest {OWNER_BRIDGE_CHECK_NAME} quality row per source (that run's rows)",
            "rows_by_source": {source: bridge["rows_by_share_clock"] for source, bridge in sorted(bridges.items())},
        }
    return {
        "availability_basis": MARKET_AVAILABILITY_BASIS,
        "availability_status": "modeled",
        "verified_vintage": False,
        "note": (
            "Bars and panel rows are modeled as known at trade_date + 22h. Owner links are verified only "
            "for identity_basis 'verified_dated' with availability_basis 'verified' (strict bridge); "
            "current_ticker_unverified links are the labeled RX1 reconstruction, and "
            f"'{IDENTITY_BASIS_RECONSTRUCTED_HISTORY}' links (P1 rule 5: RI1 share fingerprints, link_method "
            "reconstructed_history_<tier>, availability modeled) are reconstructed history, never verified."
        ),
        "owner_identity": identity,
        "owner_bridge_by_source": bridges,
        "share_basis": {
            "rows_by_shares_source": shares,
            "shares_availability_basis": dict(SHARES_AVAILABILITY_BASIS),
            "vendor_share_run_clocks": dict(ARCHIVE_RUN_CLOCKS),
            "vendor_share_run_clock_status": "modeled",
            "vendor_share_run_clock_verified": False,
            "share_clock_kinds": list(SHARE_CLOCK_KINDS),
            # Unmatched vendor runs: known a filer-family lag after the run start (modeled).
            "modeled_lag_days_by_family": dict(ARCHIVE_MODELED_LAG_DAYS_BY_FAMILY),
            "rows_by_vendor_share_run_clock": clocks,
            # Rows whose shares, market cap and valuation metrics are withheld, by reason.
            "withheld_shares_sources": list(SHARES_SOURCES_WITHHELD),
            "withheld_rows": withheld,
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
        # C4 M3: rounded so float aggregates cannot change manifest bytes between reruns.
        "terminal_returns": _rounded(delisting_policy_bias_exposure(store)),
    }


def _forward_return_evidence(store: DuckDBStore, source: str) -> dict[str, object]:
    """Observed vs policy terminal counts of one label source (R3a), latest revisions."""

    columns = _table_columns(store, "forward_returns_survivorship_safe")
    version = "calculation_version" if "calculation_version" in columns else "NULL::VARCHAR"
    price = "price_basis" if "price_basis" in columns else "NULL::VARCHAR"
    rows = store.con.execute(
        f"""
        SELECT {version}, {price}, horizon_days, is_stitched, terminal_return_source, is_latest_revision,
               count(*)::BIGINT
        FROM forward_returns_survivorship_safe WHERE source = ? GROUP BY ALL
        """,
        [source],
    ).fetchall()
    latest = [row for row in rows if row[5]]
    stitched = [row for row in latest if row[3]]
    return {
        "label_source": source,
        "label_basis": "survivorship_safe_forward_return",
        "availability_status": "modeled",
        "expected_calculation_version": FORWARD_RETURN_CALCULATION_VERSION,
        "note": (
            "Latest-revision survivorship-safe labels on the observed XNYS session calendar; a label whose "
            "line delists inside the window is stitched with an observed or a policy (Shumway convention) "
            "terminal return. Every revision is exported."
        ),
        "rows": sum(int(row[-1]) for row in latest),
        "all_revision_rows": sum(int(row[-1]) for row in rows),
        "current_version_rows": sum(int(row[-1]) for row in latest if row[0] == FORWARD_RETURN_CALCULATION_VERSION),
        "unversioned_legacy_rows": sum(int(row[-1]) for row in latest if row[0] is None),
        "other_version_rows": sum(
            int(row[-1]) for row in latest if row[0] not in (None, FORWARD_RETURN_CALCULATION_VERSION)
        ),
        "stitched_observed_terminal_rows": sum(int(row[-1]) for row in stitched if row[4] == "observed"),
        "stitched_policy_terminal_rows": sum(int(row[-1]) for row in stitched if row[4] == "policy"),
        "stitched_other_terminal_rows": sum(int(row[-1]) for row in stitched if row[4] not in ("observed", "policy")),
        "rows_by_version": _count_rows(
            [(*row[:2], *row[3:5], row[-1]) for row in latest],
            ("calculation_version", "price_basis", "is_stitched", "terminal_return_source"),
        ),
        "rows_by_horizon": _count_rows([(row[2], row[-1]) for row in latest], ("horizon_days",)),
    }


def _price_metric_evidence(store: DuckDBStore) -> dict[str, object]:
    rows = store.con.execute(
        "SELECT source, is_latest_revision, count(*)::BIGINT FROM equity_price_metrics GROUP BY ALL"
    ).fetchall()
    return {
        "availability_basis": MARKET_AVAILABILITY_BASIS,
        "availability_status": "modeled",
        "identity_basis": "price_line",
        "note": (
            "Per price line (security_id of the bar series), known at trade_date + 22h. Bars are a later vendor "
            "snapshot; cross-sectional ranks are over that day's panel of lines."
        ),
        "rows_by_source": _count_rows(rows, ("source", "is_latest_revision")),
    }


def _listing_status_evidence(store: DuckDBStore) -> dict[str, object]:
    rows = store.con.execute(
        "SELECT source, status, method, security_id IS NULL, count(*)::BIGINT FROM listing_status_intervals "
        "GROUP BY ALL"
    ).fetchall()
    return {
        "identity_basis": IDENTITY_BASIS_CURRENT_TICKER,
        "history_basis": "latest_build",
        "availability_status": "modeled",
        "note": (
            "security_id resolved through the current ticker (unverified); unresolved symbols are kept. Each "
            "build replaces the source's prior build, so valid_to reflects the latest build, not PIT revisions."
        ),
        "rows": sum(int(row[-1]) for row in rows),
        "unresolved_security_rows": sum(int(row[-1]) for row in rows if row[3]),
        "rows_by_status": _count_rows(rows, ("source", "status", "method", "unresolved_security")),
    }


def _classification_evidence(store: DuckDBStore) -> dict[str, object]:
    rows = store.con.execute(
        "SELECT taxonomy_id, source, valid_to IS NULL, count(*)::BIGINT FROM entity_classification GROUP BY ALL"
    ).fetchall()
    return {
        "classification_basis": CLASSIFICATION_BASIS_CURRENT_SIC_SNAPSHOT,
        "point_in_time_history": False,
        "valid_to_closed_in_place": True,
        "mapping_versions": dict(CLASSIFICATION_MAPPING_VERSIONS),
        "note": CLASSIFICATION_BASIS_NOTE,
        "rows_by_taxonomy": _count_rows(rows, ("taxonomy_id", "source", "open")),
    }


def _corporate_action_evidence(store: DuckDBStore) -> dict[str, object]:
    rows = store.con.execute(
        """
        SELECT source, action_type,
               CASE WHEN json_valid(details_json) THEN json_extract_string(details_json, '$.evidence_basis') END,
               count(*)::BIGINT
        FROM corporate_actions GROUP BY ALL
        """
    ).fetchall()
    return {
        "event_basis": "reconstructed",
        "availability_status": "modeled",
        "note": (
            "P8: every step of a line's vendor adjustment factor is one labelled event, reconstructed from a later "
            "vendor snapshot (never a verified corporate-action record); available_at is the ex-date bar clock or "
            "the later share-count confirmation. adjustment_unclassified is a hazard, never a split."
        ),
        "rows": sum(int(row[-1]) for row in rows),
        "rows_by_type": _count_rows(rows, ("source", "action_type", "evidence_basis")),
    }


def _foreign_filer_evidence(store: DuckDBStore) -> dict[str, object]:
    columns = _table_columns(store, "foreign_filer_reason_intervals")
    fields = [column for column in ("reason_code", "evidence_basis", "clock_policy") if column in columns]
    open_flag = "valid_to IS NULL" if "valid_to" in columns else "NULL::BOOLEAN"
    rows = store.con.execute(
        f"SELECT {', '.join([*fields, open_flag])}, count(*)::BIGINT FROM foreign_filer_reason_intervals GROUP BY ALL"
    ).fetchall()
    return {
        "availability_basis": FUNDAMENTALS_AVAILABILITY_BASIS,
        "availability_status": "modeled",
        "note": (
            "P11 reason intervals from one retained companyfacts archive snapshot; valid_from/valid_to are filed "
            "+ 46h. A reason applies at t only when valid_from <= t < valid_to."
        ),
        "rows_by_reason": _count_rows(rows, (*fields, "open")),
    }


def _owner_link_evidence(store: DuckDBStore) -> dict[str, object]:
    columns = _table_columns(store, "market_daily_metrics")
    needed = ("owner_security_id", "identity_basis", "availability_basis", "link_method")
    if not set(needed) <= columns:
        return {"linked_rows": GATE_UNMEASURED, "missing_columns": [c for c in needed if c not in columns]}
    rows = store.con.execute(
        "SELECT identity_basis, availability_basis, link_method, count(*)::BIGINT FROM market_daily_metrics "
        "WHERE owner_security_id IS NOT NULL GROUP BY ALL"
    ).fetchall()
    return {
        "released_within": "market_daily",
        "link_columns": list(needed),
        "linked_rows": sum(int(row[-1]) for row in rows),
        "reconstructed_history_rows": sum(
            int(row[-1]) for row in rows if row[0] == IDENTITY_BASIS_RECONSTRUCTED_HISTORY
        ),
        "linked_rows_by_basis": _count_rows(rows, ("identity_basis", "availability_basis", "link_method")),
    }


#: Research-store evidence: (grouping columns of the dataset, version/run header table,
#: header columns). Columns missing from an attached store are skipped, never guessed.
_RESEARCH_EVIDENCE: dict[str, tuple[tuple[str, ...], str, tuple[str, ...]]] = {
    "earnings_events": (
        ("event_version", "announcement_basis", "session_timing"),
        "research_event_versions",
        ("event_version", "status", "query_version"),
    ),
    "factor_returns": (
        ("run_id", "frequency", "basis", "status"),
        "research_factor_runs",
        ("run_id", "status", "factor_version"),
    ),
    "factor_exposures": (("run_id", "basis"), "research_factor_runs", ("run_id", "status", "factor_version")),
    "qualified_signals": (
        ("ledger_id", "status"),
        "research_qualification_ledgers",
        ("ledger_id", "policy_version", "qualification_version", "ledger_sha256"),
    ),
}


def _research_evidence(store: DuckDBStore, dataset: ReleaseDataset) -> dict[str, object]:
    fields, header_table, header_fields = _RESEARCH_EVIDENCE[dataset.name]
    present = {str(c["name"]) for c in _catalog_schema(store, RESEARCH_STORE_ALIAS, dataset.object_name)}
    grouped = [field for field in fields if field in present]
    rows = store.con.execute(
        f"SELECT {''.join(f'{_quote(f)}, ' for f in grouped)}count(*)::BIGINT FROM {_relation(dataset)} GROUP BY ALL"
    ).fetchall()
    header_present = {str(c["name"]) for c in _catalog_schema(store, RESEARCH_STORE_ALIAS, header_table)}
    headers: object = "absent"
    if set(header_fields) <= header_present:
        header_rows = store.con.execute(
            f"SELECT {', '.join(_quote(f) for f in header_fields)} "
            f'FROM {_quote(RESEARCH_STORE_ALIAS)}."main".{_quote(header_table)}'
        ).fetchall()
        items = sorted(
            ({f: _iso(v) for f, v in zip(header_fields, row, strict=True)} for row in header_rows),
            key=lambda item: tuple(_sort_key(item[f]) for f in header_fields),
        )
        headers = {"table": header_table, "rows": items[:_LIST_CAP], "truncated": len(items) > _LIST_CAP}
    return {
        "note": (
            "Research store output (RX6): research basis, never eligible; versions and runs are research "
            "artifacts, not governed warehouse data."
        ),
        "rows": sum(int(row[-1]) for row in rows),
        "rows_by_version": _count_rows(rows, grouped),
        "versions": headers,
    }


#: Evidence of each published P13 dataset (keyed by release dataset name).
_EVIDENCE_BUILDERS: dict[str, Callable[[DuckDBStore], dict[str, object]]] = {
    "forward_labels_daily": lambda store: _forward_return_evidence(store, DEFAULT_FORWARD_RETURN_SS_SOURCE),
    "forward_labels_monthly": lambda store: _forward_return_evidence(store, MONTHLY_FORWARD_LABEL_SOURCE),
    "equity_price_metrics": _price_metric_evidence,
    "listing_status": _listing_status_evidence,
    "entity_classification": _classification_evidence,
    "corporate_action_events": _corporate_action_evidence,
    "foreign_filer_disclosure": _foreign_filer_evidence,
}


def _extension_evidence(store: DuckDBStore, dataset: ReleaseDataset, entry: Mapping[str, object]) -> dict[str, object]:
    block: dict[str, object] = {
        "status": entry["status"],
        "basis": dataset.basis,
        "evidence_basis": dataset.evidence_basis,
        "verified_vintage": False,
    }
    if entry["status"] != DATASET_PUBLISHED:
        block["absent_reason"] = entry["absent_reason"]
        if dataset.name == "owner_links":
            block.update(_owner_link_evidence(store))
        return block
    if dataset.store == STORE_RESEARCH and dataset.name in _RESEARCH_EVIDENCE:
        block.update(_research_evidence(store, dataset))
    elif dataset.name in _EVIDENCE_BUILDERS:
        block.update(_EVIDENCE_BUILDERS[dataset.name](store))
    return block


def _release_evidence(
    store: DuckDBStore, universes: Mapping[str, object], entries: Mapping[str, Mapping[str, object]]
) -> dict[str, object]:
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
    windows = store.con.execute(
        "SELECT metric_window, count(*)::BIGINT FROM derived_metric_values GROUP BY ALL"
    ).fetchall()
    security_master = store.con.execute(
        "SELECT source, count(*)::BIGINT FROM v_security_master_public GROUP BY ALL"
    ).fetchall()
    extension = {
        dataset.name: _extension_evidence(store, dataset, entries[dataset.name])
        for dataset in RELEASE_DATASETS
        if not dataset.required and dataset.name in entries
    }
    return {
        **extension,
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
            # C4 M5: a daily (price-window) metric is known only once its bars are.
            "availability_basis": "per_metric_window",
            "availability_basis_by_window": {
                "daily": DERIVED_DAILY_AVAILABILITY_BASIS,
                "other_windows": FUNDAMENTALS_AVAILABILITY_BASIS,
            },
            "note": (
                "Derived from fundamentals (and bars for daily windows); inherits their modeled clocks: "
                f"{FUNDAMENTALS_AVAILABILITY_BASIS} for filing windows, {DERIVED_DAILY_AVAILABILITY_BASIS} for "
                "daily windows. history_status 'event_reconstructed' is reconstructed history, not a verified "
                "vintage."
            ),
            "rows_by_history_status": _count_rows(derived, ("history_status",)),
            "rows_by_window": [
                {
                    **item,
                    "availability_basis": (
                        DERIVED_DAILY_AVAILABILITY_BASIS
                        if item["metric_window"] == "daily"
                        else FUNDAMENTALS_AVAILABILITY_BASIS
                    ),
                }
                for item in _count_rows(windows, ("metric_window",))
            ],
        },
        "market_daily": _market_evidence(store),
    }


def publish_release(
    store: DuckDBStore,
    release_id: str,
    out_dir: Path | str,
    *,
    created_at: dt.datetime,
    previous_dir: Path | str | None = None,
    run_id: str | None = None,
    research_store: Path | str | None = None,
) -> ReleaseResult:
    """Publish every release dataset to ``<out_dir>/<release_id>/`` with one manifest.

    The caller supplies the load stamp; no clock is read here. All exports and ledger
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

    ``research_store`` (optional) is a research DuckDB file (RX6) attached READ_ONLY for
    the publication and detached afterwards; without it the research-store datasets are
    listed absent with the reason.
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
    research_reason = _attach_research_store(store, research_store)
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
                research_reason,
            )
            Path(staging).rename(release_dir)
            renamed = True
        return result
    except Exception:
        if renamed:
            shutil.rmtree(release_dir)
        raise
    finally:
        if research_reason is None:
            store.con.execute(f"DETACH {_quote(RESEARCH_STORE_ALIAS)}")


def _attach_research_store(store: DuckDBStore, research_store: Path | str | None) -> str | None:
    """Attach the research store READ_ONLY; ``None`` when attached, else why it is not."""

    if research_store is None:
        return "research_store_not_supplied"
    path = Path(research_store)
    if not path.is_file():
        return "research_store_missing"
    literal = str(path.resolve()).replace("'", "''")
    try:
        store.con.execute(f"ATTACH '{literal}' AS {_quote(RESEARCH_STORE_ALIAS)} (READ_ONLY)")
    except duckdb.Error as exc:
        # e.g. a writer process holds the file lock: never block or fail the release on it.
        return f"research_store_unavailable:{type(exc).__name__}"
    return None


def _export_dataset(
    store: DuckDBStore,
    dataset: ReleaseDataset,
    staging: Path,
    release_dir: Path,
    previous_files: Mapping[str, Path],
    research_reason: str | None,
) -> tuple[dict[str, object], ReleaseDatasetResult | None]:
    """One manifest dataset entry, and its result when the dataset is published."""

    record_schema = _schema_for(dataset)
    entry: dict[str, object] = {
        "name": dataset.name,
        "object": dataset.object_name,
        "schema": dataset.schema_ref,
        "record_schema_sha256": None if record_schema is None else _record_schema_sha256(record_schema),
        "key_columns": list(dataset.key_columns),
        "natural_key": list(dataset.natural_key or dataset.key_columns),
        "basis": dataset.basis,
        "store": dataset.store,
        "stage": dataset.stage,
        "evidence_basis": dataset.evidence_basis,
    }
    if record_schema is None:
        entry["schema_absent_reason"] = (
            _RESEARCH_API_REASON if dataset.store == STORE_RESEARCH else _NO_API_SCHEMA_REASONS.get(dataset.name)
        )
    reason = dataset.absent_reason
    if reason is None and dataset.store == STORE_RESEARCH:
        reason = research_reason
    schema: list[dict[str, object]] = []
    if reason is None:
        if dataset.required:
            schema = _object_schema(store, dataset.object_name)
        else:
            database = RESEARCH_STORE_ALIAS if dataset.store == STORE_RESEARCH else None
            schema = _catalog_schema(store, database, dataset.object_name)
            if not schema:
                reason = (
                    _FOREIGN_FILER_ABSENT
                    if dataset.name == "foreign_filer_disclosure"
                    else f"object_missing:{dataset.object_name}"
                )
    if reason is not None:
        return {**entry, "status": DATASET_ABSENT, "absent_reason": reason}, None
    # The shared catalog helper's ORDER BY inherits DuckDB's default direction.
    schema.sort(key=lambda column: int(str(column["ordinal"])))
    columns = [str(column["name"]) for column in schema]
    query = release_query(dataset, columns)
    parquet_path = staging / f"{dataset.name}.parquet"
    _copy_parquet(store, query, parquet_path)
    if not dataset.required and _has_duplicate_keys(store, dataset, parquet_path):
        # An optional dataset never fails the release: it is withheld, with the reason.
        parquet_path.unlink()
        return {**entry, "status": DATASET_WITHHELD, "absent_reason": "duplicate_release_keys"}, None
    count_row = store.con.execute("SELECT count(*) FROM read_parquet(?)", [str(parquet_path)]).fetchone()
    if count_row is None:
        raise RuntimeError(f"could not count rows for dataset {dataset.name!r}")
    added = removed = changed = None
    if dataset.name in previous_files:
        added, removed, changed = diff_against(store, dataset, columns, parquet_path, previous_files[dataset.name])
    else:
        _validate_keys(store, dataset, parquet_path)
    result = ReleaseDatasetResult(
        name=dataset.name,
        object_name=dataset.object_name,
        parquet_path=release_dir / parquet_path.name,
        row_count=int(count_row[0]),
        byte_count=parquet_path.stat().st_size,
        schema_sha256=_schema_sha256(schema),
        query_sha256=_sha256_text(query),
        parquet_sha256=file_sha256(parquet_path),
        rows_added=added,
        rows_removed=removed,
        rows_changed=changed,
    )
    entry.update(
        {
            "status": DATASET_PUBLISHED,
            "schema_sha256": result.schema_sha256,
            "query_sha256": result.query_sha256,
            "query": query,
            "columns": schema,
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
    return entry, result


def _dataset_eligibility(dataset: ReleaseDataset, status: object, release: str) -> tuple[str | None, str | None]:
    """(eligibility, reason) of one dataset entry; research basis is never ``eligible``."""

    if status != DATASET_PUBLISHED:
        return None, "not_published"
    if dataset.basis == BASIS_RESEARCH:
        return RESEARCH_ONLY, "research_basis"
    if dataset.stage is None or RELEASE_DATASET_STAGES.get(dataset.name) != dataset.stage:
        # Produced outside the activation ladder: no gate is bound to its freshness.
        return CANDIDATE, "not_produced_by_activation"
    return release, "release_gates"


def _publish_snapshot(
    store: DuckDBStore,
    release_id: str,
    release_dir: Path,
    staging: Path,
    created_at: dt.datetime,
    previous_release_id: str | None,
    previous_files: dict[str, Path],
    run_id: str | None,
    research_reason: str | None = None,
) -> ReleaseResult:

    results: list[ReleaseDatasetResult] = []
    published: list[ReleaseDataset] = []
    manifest_datasets: list[dict[str, object]] = []
    for dataset in RELEASE_DATASETS:
        entry, result = _export_dataset(store, dataset, staging, release_dir, previous_files, research_reason)
        manifest_datasets.append(entry)
        if result is not None:
            results.append(result)
            published.append(dataset)

    # Evidence and gates share the exports' snapshot, so they describe exactly these files.
    ledger = _stage_ledger(store, sorted({*RELEASE_DATASET_STAGES.values(), *EVIDENCE_STAGES.values()}))
    floor, floor_reasons, floor_block = _evidence_floor(ledger)
    runs = {gate: _evidence_run(ledger, stage, floor, floor_reasons) for gate, stage in EVIDENCE_STAGES.items()}
    source_pins, source_gate = _source_pins_and_gate(store)
    universes = _universe_evidence(store)
    gates: dict[str, dict[str, object]] = {
        "item_coverage": _item_coverage_gate(store, created_at, universes, *runs["item_coverage"]),
        "provider_slos": _provider_slo_gate(store, *runs["provider_slos"], created_at),
        "critical_dqc": _critical_dqc_gate(store, *runs["critical_dqc"], created_at),
        "source_completeness": source_gate,
        "universe_certification": _universe_certification_gate(universes),
    }
    eligibility, gates_not_passed = release_eligibility(gates)
    for dataset, entry in zip(RELEASE_DATASETS, manifest_datasets, strict=True):
        entry["eligibility"], entry["eligibility_reason"] = _dataset_eligibility(dataset, entry["status"], eligibility)
    manifest: dict[str, object] = {
        "release_id": release_id,
        "contract_version": PUBLICATION_CONTRACT_VERSION,
        "created_at": created_at.isoformat(),
        "previous_release_id": previous_release_id,
        "activation_stage_run_ids": activation_stage_run_ids(store),
        "datasets": manifest_datasets,
        "research_store": {"attached": research_reason is None, "absent_reason": research_reason},
        "source_pins": source_pins,
        "evidence": _release_evidence(store, universes, {str(entry["name"]): entry for entry in manifest_datasets}),
        "evidence_floor": floor_block,
        "gates": gates,
        "eligibility": eligibility,
        "gates_not_passed": list(gates_not_passed),
        "eligibility_rule": (
            "eligible only when every gate in "
            + ", ".join(RELEASE_GATES)
            + " is 'passed'; failed or unmeasured gates make the release a candidate. Per dataset: "
            "research-basis datasets are research_only (never eligible); a production dataset produced outside "
            "the activation ladder is a candidate; every other published dataset takes the release eligibility"
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
            for dataset, result in zip(published, results, strict=True)
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
