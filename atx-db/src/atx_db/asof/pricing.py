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


ADJUSTMENT_FACTORS_ASOF_SQL = """
WITH params AS (
    SELECT
        CAST(? AS DATE) AS as_of_date,
        CAST(? AS TIMESTAMP) AS as_of_ts
)
SELECT
    a.*
FROM adjustment_factor_history a
{symbol_join}
{event_type_join}
CROSS JOIN params p
WHERE a.ex_date <= p.as_of_date
  AND (a.available_at IS NULL OR a.available_at <= p.as_of_ts)
ORDER BY a.security_id, a.ex_date, a.event_type, a.event_ref_id
"""

DAILY_ADJUSTMENT_FACTORS_ASOF_SQL = """
WITH params AS (
    SELECT
        CAST(? AS DATE) AS as_of_date,
        CAST(? AS TIMESTAMP) AS as_of_ts
),
ranked AS (
    SELECT
        d.*,
        row_number() OVER (
            PARTITION BY d.source, d.bar_source, d.factor_source, d.security_id, d.trade_date
            ORDER BY d.as_of_date DESC,
                     d.available_at DESC NULLS LAST,
                     d.source_loaded_at DESC NULLS LAST,
                     d.daily_adjustment_id DESC
        ) AS rn
    FROM daily_adjustment_factors d
    {symbol_join}
    CROSS JOIN params p
    WHERE d.trade_date <= p.as_of_date
      AND d.as_of_date <= p.as_of_date
      AND (d.available_at IS NULL OR d.available_at <= p.as_of_ts)
)
SELECT
    daily_adjustment_id,
    source,
    bar_source,
    factor_source,
    security_id,
    symbol,
    trade_date,
    as_of_date,
    split_price_factor,
    split_share_factor,
    dividend_total_return_factor,
    total_return_price_factor,
    raw_close,
    split_adjusted_close,
    total_return_adjusted_close,
    raw_volume,
    split_adjusted_volume,
    visible_event_count,
    split_event_count,
    cash_div_event_count,
    last_factor_ex_date,
    available_at,
    run_id,
    source_loaded_at,
    updated_at
FROM ranked
WHERE rn = 1
ORDER BY security_id, trade_date, bar_source, factor_source
"""

DAILY_PANEL_ASOF_SQL = """
WITH params AS (
    SELECT
        CAST(? AS DATE) AS as_of_date,
        CAST(? AS TIMESTAMP) AS as_of_ts
),
bars AS (
    SELECT
        b.*
    FROM equity_daily_bars b
    {symbol_join}
    CROSS JOIN params p
    WHERE b.trade_date <= p.as_of_date
      AND (b.available_at IS NULL OR b.available_at <= p.as_of_ts)
),
adjustment_ranked AS (
    SELECT
        d.*,
        row_number() OVER (
            PARTITION BY d.bar_source, d.security_id, d.trade_date
            ORDER BY d.as_of_date DESC,
                     d.available_at DESC NULLS LAST,
                     d.source_loaded_at DESC NULLS LAST,
                     d.factor_source DESC,
                     d.source DESC,
                     d.daily_adjustment_id DESC
        ) AS rn
    FROM daily_adjustment_factors d
    CROSS JOIN params p
    WHERE d.trade_date <= p.as_of_date
      AND d.as_of_date <= p.as_of_date
      AND (d.available_at IS NULL OR d.available_at <= p.as_of_ts)
),
adjustments AS (
    SELECT *
    FROM adjustment_ranked
    WHERE rn = 1
),
adjustment_returns AS (
    SELECT
        a.*,
        lag(a.split_adjusted_close) OVER (
            PARTITION BY a.bar_source, a.security_id
            ORDER BY a.trade_date
        ) AS prev_split_adjusted_close,
        lag(a.total_return_adjusted_close) OVER (
            PARTITION BY a.bar_source, a.security_id
            ORDER BY a.trade_date
        ) AS prev_total_return_adjusted_close
    FROM adjustments a
)
SELECT
    b.security_id,
    b.symbol,
    b.trade_date,
    b.open,
    b.high,
    b.low,
    b.close,
    b.adjusted_close,
    ar.split_adjusted_close,
    ar.total_return_adjusted_close,
    ar.split_price_factor,
    ar.split_share_factor,
    ar.dividend_total_return_factor,
    ar.total_return_price_factor,
    b.volume,
    ar.split_adjusted_volume,
    r.simple_return,
    r.log_return,
    CASE
        WHEN ar.prev_split_adjusted_close IS NOT NULL AND ar.prev_split_adjusted_close > 0
            THEN ar.split_adjusted_close / ar.prev_split_adjusted_close - 1.0
        ELSE NULL
    END AS split_adjusted_return,
    CASE
        WHEN ar.prev_split_adjusted_close IS NOT NULL AND ar.prev_split_adjusted_close > 0
            THEN ln(ar.split_adjusted_close / ar.prev_split_adjusted_close)
        ELSE NULL
    END AS split_adjusted_log_return,
    CASE
        WHEN ar.prev_total_return_adjusted_close IS NOT NULL AND ar.prev_total_return_adjusted_close > 0
            THEN ar.total_return_adjusted_close / ar.prev_total_return_adjusted_close - 1.0
        ELSE NULL
    END AS total_return_adjusted_return,
    CASE
        WHEN ar.prev_total_return_adjusted_close IS NOT NULL AND ar.prev_total_return_adjusted_close > 0
            THEN ln(ar.total_return_adjusted_close / ar.prev_total_return_adjusted_close)
        ELSE NULL
    END AS total_return_adjusted_log_return
FROM bars b
LEFT JOIN v_equity_daily_returns r
  ON r.source = b.source
 AND r.security_id = b.security_id
 AND r.trade_date = b.trade_date
LEFT JOIN adjustment_returns ar
  ON ar.bar_source = b.source
 AND ar.security_id = b.security_id
 AND ar.trade_date = b.trade_date
ORDER BY b.security_id, b.trade_date
"""

FEATURES_ASOF_SQL = """
WITH params AS (
    SELECT
        CAST(? AS DATE) AS as_of_date,
        CAST(? AS TIMESTAMP) AS as_of_ts,
        CAST(? AS VARCHAR) AS feature_set
),
ranked AS (
    SELECT
        f.*,
        row_number() OVER (
            PARTITION BY f.feature_set, f.feature_name, f.security_id
            ORDER BY f.as_of_date DESC,
                     f.available_at DESC NULLS LAST,
                     f.computed_at DESC
        ) AS rn
    FROM feature_values f
    {feature_join}
    {symbol_join}
    CROSS JOIN params p
    WHERE f.feature_set = p.feature_set
      AND f.as_of_date <= p.as_of_date
      AND (f.available_at IS NULL OR f.available_at <= p.as_of_ts)
)
SELECT *
FROM ranked
WHERE rn = 1
ORDER BY security_id, feature_name
"""

SHORT_INTEREST_ASOF_SQL = """
WITH params AS (
    SELECT
        CAST(? AS DATE) AS as_of_date,
        CAST(? AS TIMESTAMP) AS as_of_ts
),
ranked AS (
    SELECT
        s.*,
        row_number() OVER (
            PARTITION BY coalesce(s.security_id, s.symbol), coalesce(s.market_class_code, '')
            ORDER BY s.settlement_date DESC,
                     s.available_at DESC NULLS LAST,
                     s.source_loaded_at DESC
        ) AS rn
    FROM finra_short_interest s
    {symbol_join}
    CROSS JOIN params p
    WHERE s.settlement_date <= p.as_of_date
      AND (s.available_at IS NULL OR s.available_at <= p.as_of_ts)
)
SELECT *
FROM ranked
WHERE rn = 1
ORDER BY symbol, market_class_code
"""

MACRO_ASOF_SQL = """
WITH params AS (
    SELECT
        CAST(? AS DATE) AS as_of_date,
        CAST(? AS TIMESTAMP) AS as_of_ts
),
ranked AS (
    SELECT
        m.*,
        row_number() OVER (
            PARTITION BY m.source, m.series_id
            ORDER BY m.observation_date DESC,
                     m.as_of_date DESC,
                     m.available_at DESC NULLS LAST,
                     m.source_loaded_at DESC
        ) AS rn
    FROM macro_observations m
    {series_join}
    CROSS JOIN params p
    WHERE m.observation_date <= p.as_of_date
      AND m.as_of_date <= p.as_of_date
      AND (m.available_at IS NULL OR m.available_at <= p.as_of_ts)
)
SELECT *
FROM ranked
WHERE rn = 1
ORDER BY source, series_id
"""

UNIVERSE_ASOF_SQL = """
WITH params AS (
    SELECT
        CAST(? AS DATE) AS as_of_date,
        CAST(? AS TIMESTAMP) AS as_of_ts,
        CAST(? AS VARCHAR) AS universe_id
),
snapshot AS (
    SELECT
        u.universe_id,
        max(u.as_of_date) AS snapshot_date
    FROM universe_memberships u
    CROSS JOIN params p
    WHERE u.universe_id = p.universe_id
      AND u.as_of_date <= p.as_of_date
      AND (u.available_at IS NULL OR u.available_at <= p.as_of_ts)
    GROUP BY u.universe_id
)
SELECT u.*
FROM universe_memberships u
JOIN snapshot s
  ON s.universe_id = u.universe_id
 AND s.snapshot_date = u.as_of_date
{symbol_join}
ORDER BY u.symbol, u.security_id
"""

UNIVERSE_MEMBERSHIP_ASOF_SQL = """
WITH params AS (
    SELECT
        CAST(? AS DATE) AS as_of_date,
        CAST(? AS TIMESTAMP) AS as_of_ts,
        CAST(? AS VARCHAR) AS universe_id
),
ranked AS (
    SELECT
        u.*,
        row_number() OVER (
            PARTITION BY u.universe_id, u.security_id
            ORDER BY u.valid_from DESC,
                     u.available_at DESC NULLS LAST,
                     u.source_loaded_at DESC NULLS LAST,
                     u.source DESC
        ) AS rn
    FROM universe_membership u
    {symbol_join}
    {security_join}
    CROSS JOIN params p
    WHERE u.universe_id = p.universe_id
      AND u.valid_from <= p.as_of_date
      AND (u.valid_to IS NULL OR u.valid_to >= p.as_of_date)
      AND u.as_of_date <= p.as_of_date
      AND u.is_member
      AND u.is_latest_revision
      AND (u.available_at IS NULL OR u.available_at <= p.as_of_ts)
)
SELECT
    universe_id,
    security_id,
    symbol,
    valid_from,
    valid_to,
    as_of_date,
    is_member,
    reason,
    rules_json,
    decision_count,
    available_at,
    source,
    run_id,
    is_latest_revision,
    source_loaded_at
FROM ranked
WHERE rn = 1
ORDER BY symbol, security_id
"""

CORPORATE_ACTIONS_ASOF_SQL = """
WITH params AS (
    SELECT
        CAST(? AS DATE) AS as_of_date,
        CAST(? AS TIMESTAMP) AS as_of_ts
)
SELECT c.*
FROM corporate_actions c
{symbol_join}
CROSS JOIN params p
WHERE c.ex_date <= p.as_of_date
  AND (c.available_at IS NULL OR c.available_at <= p.as_of_ts)
ORDER BY c.ex_date, c.security_id, c.action_type
"""

def adjustment_factors_asof(
    as_of_date: dt.date,
    as_of_ts: dt.datetime | None = None,
    db_path: Path | str = DEFAULT_DB_PATH,
    *,
    symbols: tuple[str, ...] | list[str] | None = None,
    event_types: tuple[str, ...] | list[str] | None = None,
) -> pd.DataFrame:
    as_of_ts = as_of_ts or end_of_day_asof_ts(as_of_date)
    symbol_values = _normalize_symbols(symbols)
    event_type_values = _normalize_strings(event_types)
    with connect(db_path, read_only=True) as store:
        registered = []
        try:
            symbol_join = ""
            event_type_join = ""
            if _register_filter(store, "asof_adjustment_symbol_filter", "symbol", symbol_values):
                registered.append("asof_adjustment_symbol_filter")
                symbol_join = "JOIN asof_adjustment_symbol_filter sf ON sf.symbol = a.symbol"
            if _register_filter(store, "asof_adjustment_event_type_filter", "event_type", event_type_values):
                registered.append("asof_adjustment_event_type_filter")
                event_type_join = (
                    "JOIN asof_adjustment_event_type_filter etf "
                    "ON etf.event_type = upper(a.event_type)"
                )
            sql = ADJUSTMENT_FACTORS_ASOF_SQL.format(
                symbol_join=symbol_join,
                event_type_join=event_type_join,
            )
            return store.con.execute(sql, [as_of_date, as_of_ts]).df()
        finally:
            for relation in registered:
                store.con.unregister(relation)

def daily_adjustment_factors_asof(
    as_of_date: dt.date,
    as_of_ts: dt.datetime | None = None,
    db_path: Path | str = DEFAULT_DB_PATH,
    *,
    symbols: tuple[str, ...] | list[str] | None = None,
) -> pd.DataFrame:
    as_of_ts = as_of_ts or end_of_day_asof_ts(as_of_date)
    symbol_values = _normalize_symbols(symbols)
    with connect(db_path, read_only=True) as store:
        registered = []
        try:
            symbol_join = ""
            if _register_filter(store, "asof_daily_adjustment_symbol_filter", "symbol", symbol_values):
                registered.append("asof_daily_adjustment_symbol_filter")
                symbol_join = "JOIN asof_daily_adjustment_symbol_filter sf ON sf.symbol = d.symbol"
            sql = DAILY_ADJUSTMENT_FACTORS_ASOF_SQL.format(symbol_join=symbol_join)
            return store.con.execute(sql, [as_of_date, as_of_ts]).df()
        finally:
            for relation in registered:
                store.con.unregister(relation)

def daily_panel_asof(
    as_of_date: dt.date,
    as_of_ts: dt.datetime | None = None,
    db_path: Path | str = DEFAULT_DB_PATH,
    *,
    symbols: tuple[str, ...] | list[str] | None = None,
) -> pd.DataFrame:
    as_of_ts = as_of_ts or end_of_day_asof_ts(as_of_date)
    symbol_values = _normalize_symbols(symbols)
    with connect(db_path, read_only=True) as store:
        registered = []
        try:
            symbol_join = ""
            if _register_filter(store, "asof_daily_panel_symbol_filter", "symbol", symbol_values):
                registered.append("asof_daily_panel_symbol_filter")
                symbol_join = "JOIN asof_daily_panel_symbol_filter sf ON sf.symbol = b.symbol"
            sql = DAILY_PANEL_ASOF_SQL.format(symbol_join=symbol_join)
            return store.con.execute(sql, [as_of_date, as_of_ts]).df()
        finally:
            for relation in registered:
                store.con.unregister(relation)

def features_asof(
    as_of_date: dt.date,
    as_of_ts: dt.datetime | None = None,
    db_path: Path | str = DEFAULT_DB_PATH,
    *,
    feature_set: str = "equity_daily_v1",
    symbols: tuple[str, ...] | list[str] | None = None,
    feature_names: tuple[str, ...] | list[str] | None = None,
) -> pd.DataFrame:
    as_of_ts = as_of_ts or end_of_day_asof_ts(as_of_date)
    symbol_values = _normalize_symbols(symbols)
    feature_values = _normalize_strings(feature_names)
    with connect(db_path, read_only=True) as store:
        registered = []
        try:
            symbol_join = ""
            feature_join = ""
            if _register_filter(store, "asof_feature_symbol_filter", "symbol", symbol_values):
                registered.append("asof_feature_symbol_filter")
                symbol_join = "JOIN asof_feature_symbol_filter sf ON sf.symbol = f.symbol"
            if _register_filter(store, "asof_feature_name_filter", "feature_name", feature_values):
                registered.append("asof_feature_name_filter")
                feature_join = "JOIN asof_feature_name_filter ff ON ff.feature_name = upper(f.feature_name)"
            sql = FEATURES_ASOF_SQL.format(symbol_join=symbol_join, feature_join=feature_join)
            return store.con.execute(sql, [as_of_date, as_of_ts, feature_set]).df()
        finally:
            for relation in registered:
                store.con.unregister(relation)

def short_interest_asof(
    as_of_date: dt.date,
    as_of_ts: dt.datetime | None = None,
    db_path: Path | str = DEFAULT_DB_PATH,
    *,
    symbols: tuple[str, ...] | list[str] | None = None,
) -> pd.DataFrame:
    as_of_ts = as_of_ts or end_of_day_asof_ts(as_of_date)
    symbol_values = _normalize_symbols(symbols)
    with connect(db_path, read_only=True) as store:
        registered = []
        try:
            symbol_join = ""
            if _register_filter(store, "asof_short_symbol_filter", "symbol", symbol_values):
                registered.append("asof_short_symbol_filter")
                symbol_join = "JOIN asof_short_symbol_filter sf ON sf.symbol = s.symbol"
            sql = SHORT_INTEREST_ASOF_SQL.format(symbol_join=symbol_join)
            return store.con.execute(sql, [as_of_date, as_of_ts]).df()
        finally:
            for relation in registered:
                store.con.unregister(relation)

def macro_asof(
    as_of_date: dt.date,
    as_of_ts: dt.datetime | None = None,
    db_path: Path | str = DEFAULT_DB_PATH,
    *,
    series_ids: tuple[str, ...] | list[str] | None = None,
) -> pd.DataFrame:
    as_of_ts = as_of_ts or end_of_day_asof_ts(as_of_date)
    series_values = _normalize_strings(series_ids)
    with connect(db_path, read_only=True) as store:
        registered = []
        try:
            series_join = ""
            if _register_filter(store, "asof_macro_series_filter", "series_id", series_values):
                registered.append("asof_macro_series_filter")
                series_join = "JOIN asof_macro_series_filter sf ON sf.series_id = m.series_id"
            sql = MACRO_ASOF_SQL.format(series_join=series_join)
            return store.con.execute(sql, [as_of_date, as_of_ts]).df()
        finally:
            for relation in registered:
                store.con.unregister(relation)

def universe_asof(
    as_of_date: dt.date,
    as_of_ts: dt.datetime | None = None,
    db_path: Path | str = DEFAULT_DB_PATH,
    *,
    universe_id: str = "us_liquid_equity_v1",
    symbols: tuple[str, ...] | list[str] | None = None,
) -> pd.DataFrame:
    as_of_ts = as_of_ts or end_of_day_asof_ts(as_of_date)
    symbol_values = _normalize_symbols(symbols)
    with connect(db_path, read_only=True) as store:
        registered = []
        try:
            symbol_join = ""
            if _register_filter(store, "asof_universe_symbol_filter", "symbol", symbol_values):
                registered.append("asof_universe_symbol_filter")
                symbol_join = "JOIN asof_universe_symbol_filter sf ON sf.symbol = u.symbol"
            sql = UNIVERSE_ASOF_SQL.format(symbol_join=symbol_join)
            return store.con.execute(sql, [as_of_date, as_of_ts, universe_id]).df()
        finally:
            for relation in registered:
                store.con.unregister(relation)

def universe_membership_asof(
    as_of_date: dt.date,
    as_of_ts: dt.datetime | None = None,
    db_path: Path | str = DEFAULT_DB_PATH,
    *,
    store=None,
    universe_id: str = "us_common_equity_liquid_v1",
    symbols: tuple[str, ...] | list[str] | None = None,
    security_ids: tuple[str, ...] | list[str] | None = None,
) -> pd.DataFrame:
    as_of_ts = as_of_ts or end_of_day_asof_ts(as_of_date)
    symbol_values = _normalize_symbols(symbols)
    security_values = _normalize_ids(security_ids)

    def _run(active):
        registered = []
        try:
            symbol_join = ""
            security_join = ""
            if _register_filter(active, "asof_universe_member_symbol_filter", "symbol", symbol_values):
                registered.append("asof_universe_member_symbol_filter")
                symbol_join = "JOIN asof_universe_member_symbol_filter sf ON sf.symbol = u.symbol"
            if _register_filter(active, "asof_universe_member_security_filter", "security_id", security_values):
                registered.append("asof_universe_member_security_filter")
                security_join = (
                    "JOIN asof_universe_member_security_filter sif "
                    "ON sif.security_id = u.security_id"
                )
            sql = UNIVERSE_MEMBERSHIP_ASOF_SQL.format(
                symbol_join=symbol_join,
                security_join=security_join,
            )
            return active.con.execute(sql, [as_of_date, as_of_ts, universe_id]).df()
        finally:
            for relation in registered:
                active.con.unregister(relation)

    if store is not None:
        return _run(store)
    with connect(db_path, read_only=True) as opened:
        return _run(opened)

def corporate_actions_asof(
    as_of_date: dt.date,
    as_of_ts: dt.datetime | None = None,
    db_path: Path | str = DEFAULT_DB_PATH,
    *,
    symbols: tuple[str, ...] | list[str] | None = None,
) -> pd.DataFrame:
    as_of_ts = as_of_ts or end_of_day_asof_ts(as_of_date)
    symbol_values = _normalize_symbols(symbols)
    with connect(db_path, read_only=True) as store:
        registered = []
        try:
            symbol_join = ""
            if _register_filter(store, "asof_corporate_action_symbol_filter", "symbol", symbol_values):
                registered.append("asof_corporate_action_symbol_filter")
                symbol_join = "JOIN asof_corporate_action_symbol_filter sf ON sf.symbol = c.symbol"
            sql = CORPORATE_ACTIONS_ASOF_SQL.format(symbol_join=symbol_join)
            return store.con.execute(sql, [as_of_date, as_of_ts]).df()
        finally:
            for relation in registered:
                store.con.unregister(relation)
