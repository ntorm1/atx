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
"""

from __future__ import annotations

import datetime as dt
import hashlib
import json
import re
import shutil
import tempfile
from dataclasses import dataclass
from pathlib import Path

from .api.catalog import RecordSchema, _record_schema_sha256, get_schema
from .connection import DuckDBStore
from .lake import _object_schema, _schema_sha256
from .warehouse import file_sha256

__all__ = [
    "MANIFEST_NAME",
    "PUBLICATION_CONTRACT_VERSION",
    "RELEASE_DATASETS",
    "ReleaseDataset",
    "ReleaseDatasetResult",
    "ReleaseResult",
    "activation_stage_run_ids",
    "diff_against",
    "publish_release",
    "read_release_manifest",
    "release_query",
    "row_digest_sql",
]

PUBLICATION_CONTRACT_VERSION = "1.0.0"
MANIFEST_NAME = "manifest.json"


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

    @property
    def total_rows(self) -> int:
        return sum(dataset.row_count for dataset in self.datasets)


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

    manifest: dict[str, object] = {
        "release_id": release_id,
        "contract_version": PUBLICATION_CONTRACT_VERSION,
        "created_at": created_at.isoformat(),
        "previous_release_id": previous_release_id,
        "activation_stage_run_ids": activation_stage_run_ids(store),
        "datasets": manifest_datasets,
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
    )
