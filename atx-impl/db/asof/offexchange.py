from __future__ import annotations

from ._common import (
    DEFAULT_DB_PATH,
    Path,
    _month_end,
    _month_end_asof_ts,
    _normalize_ids,
    _normalize_strings,
    _normalize_symbols,
    _register_filter,
    connect,
    dt,
    end_of_day_asof_ts,
    pd,
)


OFFEXCHANGE_VOLUME_ASOF_SQL = """
WITH params AS (
    SELECT
        CAST(? AS DATE) AS as_of_date,
        CAST(? AS TIMESTAMP) AS as_of_ts
)
SELECT v.*
FROM offexchange_volume v
{symbol_join}
CROSS JOIN params p
WHERE v.available_at <= p.as_of_ts
  AND v.is_latest
ORDER BY v.symbol, v.period_type, v.summary_start_date, v.venue_class, v.mpid
"""

OFFEXCHANGE_SECURITY_PERIOD_ASOF_SQL = """
WITH params AS (
    SELECT
        CAST(? AS DATE) AS as_of_date,
        CAST(? AS TIMESTAMP) AS as_of_ts
)
SELECT sp.*
FROM offexchange_security_period sp
{symbol_join}
CROSS JOIN params p
WHERE sp.available_at <= p.as_of_ts
ORDER BY sp.symbol, sp.period_type, sp.summary_start_date
"""

FINRA_SHORT_VOLUME_ASOF_SQL = """
WITH params AS (
    SELECT
        CAST(? AS DATE) AS as_of_date,
        CAST(? AS TIMESTAMP) AS as_of_ts
),
visible AS (
    SELECT
        v.*,
        row_number() OVER (
            PARTITION BY v.source, v.symbol, v.trade_date, v.market_code
            ORDER BY v.available_at DESC, v.volume_id
        ) AS rn
    FROM finra_short_volume v
    {symbol_join}
    CROSS JOIN params p
    WHERE v.available_at <= p.as_of_ts
)
SELECT
    volume_id, security_id, symbol, trade_date, market_code,
    short_volume, short_exempt_volume, total_volume,
    restatement_seq, is_latest, as_of_date, available_at,
    source, source_file, source_file_sha256, raw_payload_json,
    run_id, source_loaded_at, updated_at
FROM visible
WHERE rn = 1
ORDER BY symbol, trade_date, market_code
"""

SHORT_VOLUME_METRICS_ASOF_SQL = """
WITH params AS (
    SELECT
        CAST(? AS DATE) AS as_of_date,
        CAST(? AS TIMESTAMP) AS as_of_ts
),
visible AS (
    SELECT
        m.*,
        row_number() OVER (
            PARTITION BY m.source, m.symbol, m.trade_date
            ORDER BY m.available_at DESC, m.metric_id
        ) AS rn
    FROM short_volume_metrics m
    {symbol_join}
    CROSS JOIN params p
    WHERE m.available_at <= p.as_of_ts
)
SELECT
    metric_id, security_id, symbol, trade_date,
    short_volume, short_exempt_volume, total_volume,
    short_volume_ratio, short_exempt_ratio,
    short_volume_ratio_percentile, short_exempt_ratio_percentile,
    market_count, dominant_market_code, dominant_market_total_volume,
    dominant_market_share_pct, is_high_short_flow,
    restatement_seq, is_latest_revision, as_of_date, available_at,
    source, run_id, source_loaded_at, updated_at
FROM visible
WHERE rn = 1
ORDER BY symbol, trade_date
"""

OFFEXCHANGE_QUALITY_REPORT_ASOF_SQL = """
WITH params AS (
    SELECT
        CAST(? AS DATE) AS as_of_date,
        CAST(? AS TIMESTAMP) AS as_of_ts
),
visible AS (
    SELECT
        r.*,
        row_number() OVER (
            PARTITION BY r.source, r.surface, r.input_source, r.period_type, r.period_start_date
            ORDER BY r.available_at DESC, r.report_id
        ) AS rn
    FROM offexchange_quality_report r
    {surface_join}
    CROSS JOIN params p
    WHERE r.available_at <= p.as_of_ts
)
SELECT
    report_id, source, surface, input_source, period_type,
    period_start_date, period_end_date, row_count, security_count,
    venue_or_market_count, total_volume, ats_volume, non_ats_volume,
    short_volume, short_exempt_volume, short_volume_ratio,
    ats_share_pct, high_short_flow_count, restated_key_count,
    multiple_latest_key_count, bad_row_count, missing_available_at_count,
    max_publication_lag_days, restatement_seq, is_latest_revision,
    as_of_date, available_at, source_inputs_json, run_id,
    source_loaded_at, updated_at
FROM visible
WHERE rn = 1
ORDER BY surface, input_source, period_type, period_start_date
"""

def offexchange_volume_asof(
    store_or_path: "DuckDBStore | Path | str | None" = None,
    *,
    symbols: tuple[str, ...] | list[str] | None = None,
    as_of_date: dt.date,
    as_of_ts: dt.datetime | None = None,
    db_path: Path | str = DEFAULT_DB_PATH,
) -> pd.DataFrame:
    """Return latest-visible FINRA off-exchange volume rows as of a point in time."""
    from ..connection import DuckDBStore as _DuckDBStore

    as_of_ts = as_of_ts or end_of_day_asof_ts(as_of_date)
    symbol_values = _normalize_symbols(symbols)

    def _run(store):
        registered = []
        try:
            symbol_join = ""
            if _register_filter(store, "asof_offx_volume_symbol_filter", "symbol", symbol_values):
                registered.append("asof_offx_volume_symbol_filter")
                symbol_join = "JOIN asof_offx_volume_symbol_filter sf ON sf.symbol = v.symbol"
            sql = OFFEXCHANGE_VOLUME_ASOF_SQL.format(symbol_join=symbol_join)
            return store.con.execute(sql, [as_of_date, as_of_ts]).df()
        finally:
            for relation in registered:
                store.con.unregister(relation)

    if isinstance(store_or_path, _DuckDBStore):
        return _run(store_or_path)
    path = store_or_path if store_or_path is not None else db_path
    with connect(path, read_only=True) as store:
        return _run(store)

def offexchange_security_period_asof(
    store_or_path: "DuckDBStore | Path | str | None" = None,
    *,
    symbols: tuple[str, ...] | list[str] | None = None,
    as_of_date: dt.date,
    as_of_ts: dt.datetime | None = None,
    db_path: Path | str = DEFAULT_DB_PATH,
) -> pd.DataFrame:
    """Return latest-visible off-exchange ATS-share rollup rows as of a point in time."""
    from ..connection import DuckDBStore as _DuckDBStore

    as_of_ts = as_of_ts or end_of_day_asof_ts(as_of_date)
    symbol_values = _normalize_symbols(symbols)

    def _run(store):
        registered = []
        try:
            symbol_join = ""
            if _register_filter(store, "asof_offx_secperiod_symbol_filter", "symbol", symbol_values):
                registered.append("asof_offx_secperiod_symbol_filter")
                symbol_join = "JOIN asof_offx_secperiod_symbol_filter sf ON sf.symbol = sp.symbol"
            sql = OFFEXCHANGE_SECURITY_PERIOD_ASOF_SQL.format(symbol_join=symbol_join)
            return store.con.execute(sql, [as_of_date, as_of_ts]).df()
        finally:
            for relation in registered:
                store.con.unregister(relation)

    if isinstance(store_or_path, _DuckDBStore):
        return _run(store_or_path)
    path = store_or_path if store_or_path is not None else db_path
    with connect(path, read_only=True) as store:
        return _run(store)

def finra_short_volume_asof(
    store_or_path: "DuckDBStore | Path | str | None" = None,
    *,
    symbols: tuple[str, ...] | list[str] | None = None,
    as_of_date: dt.date,
    as_of_ts: dt.datetime | None = None,
    db_path: Path | str = DEFAULT_DB_PATH,
) -> pd.DataFrame:
    """Return latest-visible FINRA daily short-volume rows as of a point in time."""
    from ..connection import DuckDBStore as _DuckDBStore

    as_of_ts = as_of_ts or end_of_day_asof_ts(as_of_date)
    symbol_values = _normalize_symbols(symbols)

    def _run(store):
        registered = []
        try:
            symbol_join = ""
            if _register_filter(store, "asof_short_volume_symbol_filter", "symbol", symbol_values):
                registered.append("asof_short_volume_symbol_filter")
                symbol_join = "JOIN asof_short_volume_symbol_filter sf ON sf.symbol = v.symbol"
            sql = FINRA_SHORT_VOLUME_ASOF_SQL.format(symbol_join=symbol_join)
            return store.con.execute(sql, [as_of_date, as_of_ts]).df()
        finally:
            for relation in registered:
                store.con.unregister(relation)

    if isinstance(store_or_path, _DuckDBStore):
        return _run(store_or_path)
    path = store_or_path if store_or_path is not None else db_path
    with connect(path, read_only=True) as store:
        return _run(store)

def short_volume_metrics_asof(
    store_or_path: "DuckDBStore | Path | str | None" = None,
    *,
    symbols: tuple[str, ...] | list[str] | None = None,
    as_of_date: dt.date,
    as_of_ts: dt.datetime | None = None,
    db_path: Path | str = DEFAULT_DB_PATH,
) -> pd.DataFrame:
    """Return latest-visible daily short-volume metric rows as of a point in time."""
    from ..connection import DuckDBStore as _DuckDBStore

    as_of_ts = as_of_ts or end_of_day_asof_ts(as_of_date)
    symbol_values = _normalize_symbols(symbols)

    def _run(store):
        registered = []
        try:
            symbol_join = ""
            if _register_filter(store, "asof_short_volume_metrics_symbol_filter", "symbol", symbol_values):
                registered.append("asof_short_volume_metrics_symbol_filter")
                symbol_join = "JOIN asof_short_volume_metrics_symbol_filter sf ON sf.symbol = m.symbol"
            sql = SHORT_VOLUME_METRICS_ASOF_SQL.format(symbol_join=symbol_join)
            return store.con.execute(sql, [as_of_date, as_of_ts]).df()
        finally:
            for relation in registered:
                store.con.unregister(relation)

    if isinstance(store_or_path, _DuckDBStore):
        return _run(store_or_path)
    path = store_or_path if store_or_path is not None else db_path
    with connect(path, read_only=True) as store:
        return _run(store)

def offexchange_quality_report_asof(
    store_or_path: "DuckDBStore | Path | str | None" = None,
    *,
    surfaces: tuple[str, ...] | list[str] | None = None,
    as_of_date: dt.date,
    as_of_ts: dt.datetime | None = None,
    db_path: Path | str = DEFAULT_DB_PATH,
) -> pd.DataFrame:
    """Return latest-visible off-exchange quality reports as of a point in time."""
    from ..connection import DuckDBStore as _DuckDBStore

    as_of_ts = as_of_ts or end_of_day_asof_ts(as_of_date)
    surface_values = tuple(str(value) for value in surfaces) if surfaces else ()

    def _run(store):
        registered = []
        try:
            surface_join = ""
            if _register_filter(store, "asof_offx_quality_surface_filter", "surface", surface_values):
                registered.append("asof_offx_quality_surface_filter")
                surface_join = "JOIN asof_offx_quality_surface_filter sf ON sf.surface = r.surface"
            sql = OFFEXCHANGE_QUALITY_REPORT_ASOF_SQL.format(surface_join=surface_join)
            return store.con.execute(sql, [as_of_date, as_of_ts]).df()
        finally:
            for relation in registered:
                store.con.unregister(relation)

    if isinstance(store_or_path, _DuckDBStore):
        return _run(store_or_path)
    path = store_or_path if store_or_path is not None else db_path
    with connect(path, read_only=True) as store:
        return _run(store)
