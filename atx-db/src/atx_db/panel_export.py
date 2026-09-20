"""Wide point-in-time panel exports with reproducible manifests.

Both panels are wide (one row per entity-period, one column per requested item
or metric) and are written as a Parquet file plus a JSON manifest that carries
the query hash, the lake-convention schema sha256, the file sha256, the row
and column counts, and the exact inputs requested -- so a given export is
reproducible and attributable.

Determinism note: the warehouse session disables ``preserve_insertion_order``
(see ``connection.py``), so a plain ``SELECT * FROM <materialized table>``
scan is not guaranteed to preserve the order the rows were computed in. Every
query here therefore carries an explicit ``ORDER BY`` on its natural key both
when the result is materialized and again on the final ``COPY`` that writes
the Parquet file, so repeated exports of the same inputs are byte-identical.
"""

from __future__ import annotations

import datetime as dt
import hashlib
import json
from collections.abc import Iterable, Sequence
from dataclasses import dataclass
from pathlib import Path

from .connection import DuckDBStore
from .derived_registry import DERIVED_SOURCE_NAME, default_derived_definitions, known_item_codes
from .lake import _object_schema, _schema_sha256
from .market_daily import END_OF_DAY_HOURS, MARKET_DAILY_SOURCE_NAME

__all__ = [
    "PANEL_EXPORT_CONTRACT_VERSION",
    "PanelExportResult",
    "export_panel_daily_market",
    "export_panel_quarterly",
]

PANEL_EXPORT_CONTRACT_VERSION = "2.0.0"

#: Columns ``panel_daily_market`` always projects; a requested metric that
#: collides with one of these would duplicate a column in the result, so they
#: are excluded from the set of names callers may pass as ``metrics``.
_DAILY_BASE_COLUMNS = frozenset(
    {
        "security_id",
        "trade_date",
        "symbol",
        "close",
        "adj_close",
        "shares_outstanding",
        "shares_source",
        "available_at",
        "inputs_hash",
    }
)


@dataclass(frozen=True)
class PanelExportResult:
    panel: str
    parquet_path: Path
    manifest_path: Path
    row_count: int
    column_count: int
    query_sha256: str
    schema_sha256: str


def _sha256_text(text: str) -> str:
    return hashlib.sha256(text.encode("utf-8")).hexdigest()


def _sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def _quote(value: str) -> str:
    return "'" + value.replace("'", "''") + "'"


def _identifier(value: str) -> str:
    if not value.replace("_", "").isalnum():
        raise ValueError(f"unsupported panel column name {value!r}")
    return f'"{value}"'


def _write(
    store: DuckDBStore,
    *,
    panel: str,
    sql: str,
    params: Sequence[object],
    inputs: dict[str, object],
    as_of: dt.date,
    out_dir: Path,
) -> PanelExportResult:
    out_dir.mkdir(parents=True, exist_ok=True)
    query_sha256 = _sha256_text(sql)
    # Fold a short selection hash into the filename: two exports with the same
    # as_of but different requested items/metrics have different sql text (see
    # export_panel_quarterly/export_panel_daily_market building item_columns /
    # projection into the query), so this keeps them from clobbering each
    # other's Parquet/manifest at the same out_dir. A repeat of the *same*
    # selection still lands on the same path, which is the desired idempotent
    # overwrite behavior exercised by test_repeated_export_is_byte_identical.
    selection_hash = query_sha256[:8]
    parquet_path = out_dir / f"{panel}_{as_of.isoformat()}_{selection_hash}.parquet"
    manifest_path = out_dir / f"{panel}_{as_of.isoformat()}_{selection_hash}.manifest.json"
    relation = f"_panel_{panel}"
    # A parameterized CREATE VIEW is rejected by DuckDB ("Unexpected prepared
    # parameter"); materialize into a TEMP TABLE instead, which both accepts
    # bound parameters and lands in duckdb_columns() under schema_name='main'
    # the same way a permanent table does, so ``_object_schema`` finds it.
    store.con.execute(f"DROP TABLE IF EXISTS {relation}")
    store.con.execute(f"CREATE TEMP TABLE {relation} AS {sql}", list(params))
    try:
        count_row = store.con.execute(f"SELECT count(*) FROM {relation}").fetchone()
        if count_row is None:
            raise RuntimeError(f"could not count rows in {relation}")
        row_count = int(count_row[0])
        schema = _object_schema(store, relation)
        # Re-sort explicitly: preserve_insertion_order is disabled for this
        # session (connection.py), so a bare "SELECT * FROM relation" scan is
        # not guaranteed to replay the CTAS's own ORDER BY.
        store.con.execute(
            f"COPY (SELECT * FROM {relation} ORDER BY 1, 2) TO {_quote(parquet_path.as_posix())} "
            "(FORMAT PARQUET, COMPRESSION ZSTD)"
        )
    finally:
        store.con.execute(f"DROP TABLE IF EXISTS {relation}")
    manifest: dict[str, object] = {
        "panel": panel,
        "contract_version": PANEL_EXPORT_CONTRACT_VERSION,
        "as_of": as_of.isoformat(),
        "inputs": inputs,
        "query_sha256": query_sha256,
        "schema_sha256": _schema_sha256(schema),
        "row_count": row_count,
        "column_count": len(schema),
        "parquet_path": parquet_path.name,
        "parquet_sha256": _sha256_file(parquet_path),
    }
    manifest_path.write_text(json.dumps(manifest, sort_keys=True, indent=2) + "\n", encoding="utf-8")
    return PanelExportResult(
        panel=panel,
        parquet_path=parquet_path,
        manifest_path=manifest_path,
        row_count=row_count,
        column_count=len(schema),
        query_sha256=str(manifest["query_sha256"]),
        schema_sha256=str(manifest["schema_sha256"]),
    )


def export_panel_quarterly(
    store: DuckDBStore,
    as_of: dt.date,
    items: Iterable[str],
    metrics: Iterable[str],
    out_dir: Path | str,
    *,
    derived_source: str = DERIVED_SOURCE_NAME,
) -> PanelExportResult:
    """Export the wide quarterly panel keyed by ``(security_id, period_end)``.

    One column per requested ``item:`` code (pivoted from
    ``fundamental_standardized``) and per ``metric_code`` (pivoted from
    ``derived_metric_values``), each selected with ``available_at <= as_of``.
    The ``fundamental_standardized`` branch takes the latest revision that was
    actually visible as of that cutoff, with source/rule/basis/stable-ID ties.
    Derived states are ranked by bucket and event before exposing their nullable
    value, so an invalidation cannot resurrect a previously valid state.
    ``panel_available_at`` carries the row-level max across selected columns.
    """
    item_codes = tuple(dict.fromkeys(items))
    metric_codes = tuple(dict.fromkeys(metrics))
    unknown_items = sorted(set(item_codes) - known_item_codes())
    if unknown_items:
        raise ValueError(f"unknown fundamental item codes: {unknown_items}")
    catalog = {definition.metric_code for definition in default_derived_definitions()}
    unknown_metrics = sorted(set(metric_codes) - catalog)
    if unknown_metrics:
        raise ValueError(f"unknown derived metric codes: {unknown_metrics}")

    item_columns = "".join(
        f",\n           max(CASE WHEN u.code = {_quote(code)} THEN u.value END) AS {_identifier(code)}"
        for code in item_codes
    )
    metric_columns = "".join(
        f",\n           max(CASE WHEN u.code = {_quote(code)} THEN u.value END) AS {_identifier(code)}"
        for code in metric_codes
    )
    sql = f"""
    WITH visible_standardized AS (
        SELECT
            security_id, canonical_code, period_end, value, available_at, standardized_id,
            row_number() OVER (
                PARTITION BY security_id, canonical_code, period_end
                ORDER BY available_at DESC, source DESC, rule_id DESC, basis DESC, standardized_id DESC
            ) AS rn
        FROM fundamental_standardized
        WHERE available_at <= ?
          AND basis IN ('quarterly', 'instant')
          AND canonical_code IN ({", ".join(_quote(code) for code in item_codes) or "''"})
    ), visible_derived AS (
        SELECT *, row_number() OVER (
            PARTITION BY security_id, metric_code, metric_window,
                         coalesce(CAST(target_bucket AS VARCHAR), CAST(period_end AS VARCHAR))
            ORDER BY available_at DESC, derived_value_id DESC
        ) AS rn
        FROM derived_metric_values
        WHERE source = ? AND available_at <= ?
          AND metric_code IN ({", ".join(_quote(code) for code in metric_codes) or "''"})
    ), unioned AS (
        SELECT security_id, canonical_code AS code, period_end, value, available_at
        FROM visible_standardized
        WHERE rn = 1
        UNION ALL
        SELECT security_id, metric_code AS code, period_end, value, available_at
        FROM visible_derived WHERE rn = 1
    ), picked AS (
        SELECT security_id, code, period_end,
               arg_max(struct_pack(value := value), available_at).value AS value,
               max(available_at) AS available_at
        FROM unioned
        GROUP BY 1, 2, 3
    )
    SELECT u.security_id, u.period_end{item_columns}{metric_columns},
           max(u.available_at) AS panel_available_at
    FROM picked u
    GROUP BY u.security_id, u.period_end
    ORDER BY u.security_id, u.period_end
    """
    cutoff = dt.datetime.combine(as_of, dt.time(23, 59, 59))
    return _write(
        store,
        panel="panel_quarterly",
        sql=sql,
        params=[cutoff, derived_source, cutoff],
        inputs={
            "items": list(item_codes),
            "metrics": list(metric_codes),
            "source_tables": ["fundamental_standardized", "derived_metric_values"],
            "derived_source": derived_source,
        },
        as_of=as_of,
        out_dir=Path(out_dir),
    )


def export_panel_daily_market(
    store: DuckDBStore,
    as_of: dt.date,
    metrics: Iterable[str],
    out_dir: Path | str,
    *,
    market_source: str = MARKET_DAILY_SOURCE_NAME,
) -> PanelExportResult:
    """Export the wide daily market panel keyed by ``(security_id, trade_date)``.

    The requested subset of ``market_daily_metrics`` columns, filtered
    ``trade_date <= as_of AND available_at <= as_of + INTERVAL 22 HOUR``
    (the bar's end-of-day availability convention -- never ``period_end``).
    """
    metric_codes = tuple(dict.fromkeys(metrics))
    all_columns = {
        str(row[0])
        for row in store.con.execute(
            "SELECT column_name FROM duckdb_columns() "
            "WHERE schema_name = 'main' AND table_name = 'market_daily_metrics'"
        ).fetchall()
    }
    available = all_columns - _DAILY_BASE_COLUMNS
    unknown = sorted(set(metric_codes) - available)
    if unknown:
        raise ValueError(f"unknown market_daily_metrics columns: {unknown}")
    projection = "".join(f", m.{_identifier(code)}" for code in metric_codes)
    sql = f"""
    SELECT m.security_id, m.trade_date, m.symbol, m.close, m.adj_close,
           m.shares_outstanding, m.shares_source{projection},
           m.available_at, m.inputs_hash
    FROM market_daily_metrics m
    WHERE m.source = ? AND m.is_latest_revision
      AND m.trade_date <= ?
      AND m.available_at <= CAST(? AS TIMESTAMP) + INTERVAL {END_OF_DAY_HOURS} HOUR
    ORDER BY m.security_id, m.trade_date
    """
    return _write(
        store,
        panel="panel_daily_market",
        sql=sql,
        params=[market_source, as_of, as_of],
        inputs={
            "metrics": list(metric_codes),
            "source_tables": ["market_daily_metrics"],
            "market_source": market_source,
        },
        as_of=as_of,
        out_dir=Path(out_dir),
    )
