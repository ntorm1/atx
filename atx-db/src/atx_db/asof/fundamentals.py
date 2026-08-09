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


FUNDAMENTALS_ASOF_SQL = """
WITH params AS (
    SELECT
        CAST(? AS DATE) AS as_of_date,
        CAST(? AS TIMESTAMP) AS as_of_ts
),
ranked AS (
    SELECT
        f.*,
        row_number() OVER (
            PARTITION BY f.security_id, f.metric, f.period_start, f.period_end, f.unit
            ORDER BY f.as_of_date DESC, f.source_loaded_at DESC
        ) AS rn
    FROM fundamental_points f
    {symbol_join}
    {metric_join}
    CROSS JOIN params p
    WHERE f.period_end <= p.as_of_date
      AND f.as_of_date <= p.as_of_date
      AND (f.available_at IS NULL OR f.available_at <= p.as_of_ts)
)
SELECT *
FROM ranked
WHERE rn = 1
ORDER BY security_id, metric, period_end
"""

FUNDAMENTAL_STATEMENTS_ASOF_SQL = """
WITH params AS (
    SELECT
        CAST(? AS DATE) AS as_of_date,
        CAST(? AS TIMESTAMP) AS as_of_ts
),
ranked AS (
    SELECT
        p.*,
        row_number() OVER (
            PARTITION BY p.security_id, p.canonical_metric, p.period_start, p.period_end, p.unit
            ORDER BY p.as_of_date DESC,
                     p.available_at DESC NULLS LAST,
                     p.source_loaded_at DESC NULLS LAST,
                     p.statement_point_id DESC
        ) AS rn
    FROM fundamental_statement_points p
    {symbol_join}
    {metric_join}
    {statement_join}
    CROSS JOIN params prm
    WHERE p.period_end <= prm.as_of_date
      AND p.as_of_date <= prm.as_of_date
      AND (p.available_at IS NULL OR p.available_at <= prm.as_of_ts)
)
SELECT *
FROM ranked
WHERE rn = 1
ORDER BY security_id, statement_type, canonical_metric, period_end
"""

FUNDAMENTAL_TTM_ASOF_SQL = """
WITH params AS (
    SELECT
        CAST(? AS DATE) AS as_of_date,
        CAST(? AS TIMESTAMP) AS as_of_ts
),
ranked AS (
    SELECT
        t.*,
        row_number() OVER (
            PARTITION BY t.security_id, t.canonical_metric, t.ttm_end_date, t.unit
            ORDER BY t.as_of_date DESC,
                     t.available_at DESC NULLS LAST,
                     t.source_loaded_at DESC NULLS LAST,
                     t.ttm_point_id DESC
        ) AS rn
    FROM fundamental_ttm_points t
    {symbol_join}
    {metric_join}
    {statement_join}
    CROSS JOIN params prm
    WHERE t.ttm_end_date <= prm.as_of_date
      AND t.as_of_date <= prm.as_of_date
      AND (t.available_at IS NULL OR t.available_at <= prm.as_of_ts)
)
SELECT *
FROM ranked
WHERE rn = 1
ORDER BY security_id, statement_type, canonical_metric, ttm_end_date
"""

FUNDAMENTAL_PERIODS_ASOF_SQL = """
WITH params AS (
    SELECT
        CAST(? AS DATE) AS as_of_date,
        CAST(? AS TIMESTAMP) AS as_of_ts
),
ranked AS (
    SELECT
        fp.*,
        row_number() OVER (
            PARTITION BY fp.period_group_id
            ORDER BY fp.as_of_date DESC,
                     fp.available_at DESC NULLS LAST,
                     fp.source_loaded_at DESC NULLS LAST,
                     fp.fundamental_period_id DESC
        ) AS rn
    FROM fundamental_periods fp
    {symbol_join}
    {period_type_join}
    CROSS JOIN params prm
    WHERE fp.period_end <= prm.as_of_date
      AND fp.as_of_date <= prm.as_of_date
      AND (fp.available_at IS NULL OR fp.available_at <= prm.as_of_ts)
)
SELECT *
FROM ranked
WHERE rn = 1
ORDER BY security_id, period_end, period_start
"""

SHARES_OUTSTANDING_ASOF_SQL = """
WITH params AS (
    SELECT
        CAST(? AS DATE) AS as_of_date,
        CAST(? AS TIMESTAMP) AS as_of_ts
),
ranked AS (
    SELECT
        s.*,
        row_number() OVER (
            PARTITION BY s.security_id, s.share_count_type
            ORDER BY s.effective_date DESC,
                     s.as_of_date DESC,
                     s.available_at DESC NULLS LAST,
                     s.source_loaded_at DESC NULLS LAST,
                     s.share_history_id DESC
        ) AS rn
    FROM shares_outstanding_history s
    {symbol_join}
    {share_type_join}
    CROSS JOIN params p
    WHERE s.effective_date <= p.as_of_date
      AND s.as_of_date <= p.as_of_date
      AND (s.available_at IS NULL OR s.available_at <= p.as_of_ts)
)
SELECT *
FROM ranked
WHERE rn = 1
ORDER BY security_id, share_count_type
"""

def fundamentals_asof(
    as_of_date: dt.date,
    as_of_ts: dt.datetime | None = None,
    db_path: Path | str = DEFAULT_DB_PATH,
    *,
    symbols: tuple[str, ...] | list[str] | None = None,
    metrics: tuple[str, ...] | list[str] | None = None,
) -> pd.DataFrame:
    as_of_ts = as_of_ts or end_of_day_asof_ts(as_of_date)
    symbol_values = _normalize_symbols(symbols)
    metric_values = _normalize_strings(metrics)
    with connect(db_path, read_only=True) as store:
        registered = []
        try:
            symbol_join = ""
            metric_join = ""
            if _register_filter(store, "asof_fundamental_symbol_filter", "symbol", symbol_values):
                registered.append("asof_fundamental_symbol_filter")
                symbol_join = "JOIN asof_fundamental_symbol_filter sf ON sf.symbol = f.symbol"
            if _register_filter(store, "asof_fundamental_metric_filter", "metric", metric_values):
                registered.append("asof_fundamental_metric_filter")
                metric_join = "JOIN asof_fundamental_metric_filter mf ON mf.metric = upper(f.metric)"
            sql = FUNDAMENTALS_ASOF_SQL.format(symbol_join=symbol_join, metric_join=metric_join)
            return store.con.execute(sql, [as_of_date, as_of_ts]).df()
        finally:
            for relation in registered:
                store.con.unregister(relation)

def fundamental_statements_asof(
    as_of_date: dt.date,
    as_of_ts: dt.datetime | None = None,
    db_path: Path | str = DEFAULT_DB_PATH,
    *,
    symbols: tuple[str, ...] | list[str] | None = None,
    metrics: tuple[str, ...] | list[str] | None = None,
    statement_types: tuple[str, ...] | list[str] | None = None,
) -> pd.DataFrame:
    as_of_ts = as_of_ts or end_of_day_asof_ts(as_of_date)
    symbol_values = _normalize_symbols(symbols)
    metric_values = _normalize_strings(metrics)
    statement_values = _normalize_strings(statement_types)
    with connect(db_path, read_only=True) as store:
        registered = []
        try:
            symbol_join = ""
            metric_join = ""
            statement_join = ""
            if _register_filter(store, "asof_statement_symbol_filter", "symbol", symbol_values):
                registered.append("asof_statement_symbol_filter")
                symbol_join = "JOIN asof_statement_symbol_filter sf ON sf.symbol = p.symbol"
            if _register_filter(store, "asof_statement_metric_filter", "canonical_metric", metric_values):
                registered.append("asof_statement_metric_filter")
                metric_join = "JOIN asof_statement_metric_filter mf ON mf.canonical_metric = upper(p.canonical_metric)"
            if _register_filter(store, "asof_statement_type_filter", "statement_type", statement_values):
                registered.append("asof_statement_type_filter")
                statement_join = "JOIN asof_statement_type_filter stf ON stf.statement_type = upper(p.statement_type)"
            sql = FUNDAMENTAL_STATEMENTS_ASOF_SQL.format(
                symbol_join=symbol_join,
                metric_join=metric_join,
                statement_join=statement_join,
            )
            return store.con.execute(sql, [as_of_date, as_of_ts]).df()
        finally:
            for relation in registered:
                store.con.unregister(relation)

def fundamental_ttm_asof(
    as_of_date: dt.date,
    as_of_ts: dt.datetime | None = None,
    db_path: Path | str = DEFAULT_DB_PATH,
    *,
    symbols: tuple[str, ...] | list[str] | None = None,
    metrics: tuple[str, ...] | list[str] | None = None,
    statement_types: tuple[str, ...] | list[str] | None = None,
) -> pd.DataFrame:
    as_of_ts = as_of_ts or end_of_day_asof_ts(as_of_date)
    symbol_values = _normalize_symbols(symbols)
    metric_values = _normalize_strings(metrics)
    statement_values = _normalize_strings(statement_types)
    with connect(db_path, read_only=True) as store:
        registered = []
        try:
            symbol_join = ""
            metric_join = ""
            statement_join = ""
            if _register_filter(store, "asof_ttm_symbol_filter", "symbol", symbol_values):
                registered.append("asof_ttm_symbol_filter")
                symbol_join = "JOIN asof_ttm_symbol_filter sf ON sf.symbol = t.symbol"
            if _register_filter(store, "asof_ttm_metric_filter", "canonical_metric", metric_values):
                registered.append("asof_ttm_metric_filter")
                metric_join = "JOIN asof_ttm_metric_filter mf ON mf.canonical_metric = upper(t.canonical_metric)"
            if _register_filter(store, "asof_ttm_statement_type_filter", "statement_type", statement_values):
                registered.append("asof_ttm_statement_type_filter")
                statement_join = "JOIN asof_ttm_statement_type_filter stf ON stf.statement_type = upper(t.statement_type)"
            sql = FUNDAMENTAL_TTM_ASOF_SQL.format(
                symbol_join=symbol_join,
                metric_join=metric_join,
                statement_join=statement_join,
            )
            return store.con.execute(sql, [as_of_date, as_of_ts]).df()
        finally:
            for relation in registered:
                store.con.unregister(relation)

def fundamental_periods_asof(
    as_of_date: dt.date,
    as_of_ts: dt.datetime | None = None,
    db_path: Path | str = DEFAULT_DB_PATH,
    *,
    symbols: tuple[str, ...] | list[str] | None = None,
    normalized_period_types: tuple[str, ...] | list[str] | None = None,
) -> pd.DataFrame:
    as_of_ts = as_of_ts or end_of_day_asof_ts(as_of_date)
    symbol_values = _normalize_symbols(symbols)
    period_type_values = _normalize_strings(normalized_period_types)
    with connect(db_path, read_only=True) as store:
        registered = []
        try:
            symbol_join = ""
            period_type_join = ""
            if _register_filter(store, "asof_period_symbol_filter", "symbol", symbol_values):
                registered.append("asof_period_symbol_filter")
                symbol_join = "JOIN asof_period_symbol_filter sf ON sf.symbol = fp.symbol"
            if _register_filter(store, "asof_period_type_filter", "normalized_period_type", period_type_values):
                registered.append("asof_period_type_filter")
                period_type_join = (
                    "JOIN asof_period_type_filter ptf "
                    "ON ptf.normalized_period_type = upper(fp.normalized_period_type)"
                )
            sql = FUNDAMENTAL_PERIODS_ASOF_SQL.format(
                symbol_join=symbol_join,
                period_type_join=period_type_join,
            )
            return store.con.execute(sql, [as_of_date, as_of_ts]).df()
        finally:
            for relation in registered:
                store.con.unregister(relation)

def shares_outstanding_asof(
    as_of_date: dt.date,
    as_of_ts: dt.datetime | None = None,
    db_path: Path | str = DEFAULT_DB_PATH,
    *,
    symbols: tuple[str, ...] | list[str] | None = None,
    share_count_types: tuple[str, ...] | list[str] | None = None,
) -> pd.DataFrame:
    as_of_ts = as_of_ts or end_of_day_asof_ts(as_of_date)
    symbol_values = _normalize_symbols(symbols)
    share_type_values = _normalize_strings(share_count_types)
    with connect(db_path, read_only=True) as store:
        registered = []
        try:
            symbol_join = ""
            share_type_join = ""
            if _register_filter(store, "asof_shares_symbol_filter", "symbol", symbol_values):
                registered.append("asof_shares_symbol_filter")
                symbol_join = "JOIN asof_shares_symbol_filter sf ON sf.symbol = s.symbol"
            if _register_filter(store, "asof_shares_type_filter", "share_count_type", share_type_values):
                registered.append("asof_shares_type_filter")
                share_type_join = (
                    "JOIN asof_shares_type_filter stf "
                    "ON stf.share_count_type = upper(s.share_count_type)"
                )
            sql = SHARES_OUTSTANDING_ASOF_SQL.format(
                symbol_join=symbol_join,
                share_type_join=share_type_join,
            )
            return store.con.execute(sql, [as_of_date, as_of_ts]).df()
        finally:
            for relation in registered:
                store.con.unregister(relation)
