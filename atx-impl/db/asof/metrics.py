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


FUNDAMENTAL_XBRL_METRIC_ASOF_SQL = """
WITH params AS (
    SELECT
        CAST(? AS DATE) AS as_of_date,
        CAST(? AS TIMESTAMP) AS as_of_ts
),
visible AS (
    SELECT
        x.*,
        row_number() OVER (
            PARTITION BY x.security_id, x.canonical_metric, x.period_end
            ORDER BY x.available_at DESC, x.revision_seq DESC, x.metric_id DESC
        ) AS rn
    FROM fundamental_xbrl_metric x
    {symbol_join}
    {metric_join}
    CROSS JOIN params p
    WHERE x.available_at <= p.as_of_ts
      AND x.as_of_date <= p.as_of_date
)
SELECT * EXCLUDE (rn)
FROM visible
WHERE rn = 1
ORDER BY symbol, canonical_metric, period_end
"""

FUNDAMENTAL_RATIOS_ASOF_SQL = """
WITH params AS (
    SELECT
        CAST(? AS DATE) AS as_of_date,
        CAST(? AS TIMESTAMP) AS as_of_ts
)
SELECT r.*
FROM fundamental_ratios r
{symbol_join}
{code_join}
{category_join}
CROSS JOIN params p
WHERE r.available_at <= p.as_of_ts
  AND r.as_of_date <= p.as_of_date
  AND r.is_latest_revision
ORDER BY r.symbol, r.ratio_code, r.basis, r.period_end
"""

FUNDAMENTAL_RATIOS_ASOF_MONTH_SQL = """
WITH params AS (
    SELECT
        CAST(? AS DATE) AS as_of_month,
        CAST(? AS TIMESTAMP) AS as_of_ts
),
ranked AS (
    SELECT
        r.*,
        row_number() OVER (
            PARTITION BY r.security_id, r.ratio_code, r.basis, r.period_end
            ORDER BY coalesce(r.filed_date, r.as_of_date) DESC,
                     r.available_at DESC,
                     coalesce(r.source_accession, '') DESC
        ) AS rn
    FROM fundamental_ratios r
    {symbol_join}
    {code_join}
    {category_join}
    CROSS JOIN params p
    WHERE r.available_at <= p.as_of_ts
      AND r.as_of_date <= p.as_of_month
)
SELECT * EXCLUDE (rn)
FROM ranked
WHERE rn = 1
ORDER BY symbol, ratio_code, basis, period_end
"""

def fundamental_xbrl_metric_asof(
    as_of_date: dt.date,
    as_of_ts: dt.datetime | None = None,
    db_path: Path | str = DEFAULT_DB_PATH,
    *,
    store: "DuckDBStore | None" = None,
    symbols: tuple[str, ...] | list[str] | None = None,
    canonical_metrics: tuple[str, ...] | list[str] | None = None,
) -> pd.DataFrame:
    """Return the latest-visible consolidated inline-XBRL metric per key as of a point in time."""
    as_of_ts = as_of_ts or end_of_day_asof_ts(as_of_date)
    symbol_values = _normalize_symbols(symbols)
    metric_values = _normalize_strings(canonical_metrics)

    def _run(active):
        registered = []
        try:
            symbol_join = ""
            metric_join = ""
            if _register_filter(active, "asof_xbrlm_symbol_filter", "symbol", symbol_values):
                registered.append("asof_xbrlm_symbol_filter")
                symbol_join = "JOIN asof_xbrlm_symbol_filter sf ON sf.symbol = x.symbol"
            if _register_filter(active, "asof_xbrlm_metric_filter", "canonical_metric", metric_values):
                registered.append("asof_xbrlm_metric_filter")
                metric_join = "JOIN asof_xbrlm_metric_filter mf ON mf.canonical_metric = upper(x.canonical_metric)"
            sql = FUNDAMENTAL_XBRL_METRIC_ASOF_SQL.format(symbol_join=symbol_join, metric_join=metric_join)
            return active.con.execute(sql, [as_of_date, as_of_ts]).df()
        finally:
            for relation in registered:
                active.con.unregister(relation)

    if store is not None:
        return _run(store)
    with connect(db_path, read_only=True) as opened:
        return _run(opened)

def fundamental_ratios_asof(
    as_of_date: dt.date,
    as_of_ts: dt.datetime | None = None,
    db_path: Path | str = DEFAULT_DB_PATH,
    *,
    store: "DuckDBStore | None" = None,
    symbols: tuple[str, ...] | list[str] | None = None,
    ratio_codes: tuple[str, ...] | list[str] | None = None,
    categories: tuple[str, ...] | list[str] | None = None,
) -> pd.DataFrame:
    """Return latest-visible derived financial ratios as of a point in time.

    Each ratio's ``available_at`` is the max availability of its specific inputs,
    so a ratio appears only once every input it consumes was knowable at as_of_ts.
    Pass an open ``store`` to read through an existing connection (DuckDB forbids a
    second connection to the same file); otherwise a read-only connection to
    ``db_path`` is opened.
    """
    as_of_ts = as_of_ts or end_of_day_asof_ts(as_of_date)
    symbol_values = _normalize_symbols(symbols)
    code_values = _normalize_strings(ratio_codes)
    category_values = _normalize_strings(categories)

    def _run(active):
        registered = []
        try:
            symbol_join = ""
            code_join = ""
            category_join = ""
            if _register_filter(active, "asof_ratios_symbol_filter", "symbol", symbol_values):
                registered.append("asof_ratios_symbol_filter")
                symbol_join = "JOIN asof_ratios_symbol_filter sf ON sf.symbol = r.symbol"
            if _register_filter(active, "asof_ratios_code_filter", "ratio_code", code_values):
                registered.append("asof_ratios_code_filter")
                code_join = "JOIN asof_ratios_code_filter cf ON cf.ratio_code = upper(r.ratio_code)"
            if _register_filter(active, "asof_ratios_category_filter", "ratio_category", category_values):
                registered.append("asof_ratios_category_filter")
                category_join = "JOIN asof_ratios_category_filter gf ON gf.ratio_category = upper(r.ratio_category)"
            sql = FUNDAMENTAL_RATIOS_ASOF_SQL.format(
                symbol_join=symbol_join,
                code_join=code_join,
                category_join=category_join,
            )
            return active.con.execute(sql, [as_of_date, as_of_ts]).df()
        finally:
            for relation in registered:
                active.con.unregister(relation)

    if store is not None:
        return _run(store)
    with connect(db_path, read_only=True) as opened:
        return _run(opened)

def fundamental_ratios_asof_month(
    as_of_month: dt.date,
    db_path: Path | str = DEFAULT_DB_PATH,
    *,
    store: "DuckDBStore | None" = None,
    symbols: tuple[str, ...] | list[str] | None = None,
    ratio_codes: tuple[str, ...] | list[str] | None = None,
    categories: tuple[str, ...] | list[str] | None = None,
) -> pd.DataFrame:
    """Return the latest ratio vintage visible at the end of ``as_of_month``."""

    month = _month_end(as_of_month)
    as_of_ts = _month_end_asof_ts(as_of_month)
    symbol_values = _normalize_symbols(symbols)
    code_values = _normalize_strings(ratio_codes)
    category_values = _normalize_strings(categories)

    def _run(active):
        registered = []
        try:
            symbol_join = ""
            code_join = ""
            category_join = ""
            if _register_filter(active, "asof_month_ratios_symbol_filter", "symbol", symbol_values):
                registered.append("asof_month_ratios_symbol_filter")
                symbol_join = "JOIN asof_month_ratios_symbol_filter sf ON sf.symbol = r.symbol"
            if _register_filter(active, "asof_month_ratios_code_filter", "ratio_code", code_values):
                registered.append("asof_month_ratios_code_filter")
                code_join = "JOIN asof_month_ratios_code_filter cf ON cf.ratio_code = upper(r.ratio_code)"
            if _register_filter(active, "asof_month_ratios_category_filter", "ratio_category", category_values):
                registered.append("asof_month_ratios_category_filter")
                category_join = "JOIN asof_month_ratios_category_filter gf ON gf.ratio_category = upper(r.ratio_category)"
            sql = FUNDAMENTAL_RATIOS_ASOF_MONTH_SQL.format(
                symbol_join=symbol_join,
                code_join=code_join,
                category_join=category_join,
            )
            return active.con.execute(sql, [month, as_of_ts]).df()
        finally:
            for relation in registered:
                active.con.unregister(relation)

    if store is not None:
        return _run(store)
    with connect(db_path, read_only=True) as opened:
        return _run(opened)

def pit_snapshot_asof(
    snapshot_month: dt.date,
    db_path: Path | str = DEFAULT_DB_PATH,
    *,
    store: "DuckDBStore | None" = None,
    symbols: tuple[str, ...] | list[str] | None = None,
    metrics: tuple[str, ...] | list[str] | None = None,
) -> pd.DataFrame:
    from ..pit_snapshot import pit_snapshot_asof as _pit_snapshot_asof

    return _pit_snapshot_asof(
        snapshot_month,
        db_path=db_path,
        store=store,
        symbols=symbols,
        metrics=metrics,
    )

CORPORATE_ACTION_DIVIDEND_METRICS_ASOF_SQL = """
WITH params AS (
    SELECT
        CAST(? AS DATE) AS as_of_date,
        CAST(? AS TIMESTAMP) AS as_of_ts
)
SELECT m.*
FROM corporate_action_dividend_metrics m
{symbol_join}
CROSS JOIN params p
WHERE m.available_at <= p.as_of_ts
  AND m.as_of_date <= p.as_of_date
  AND m.is_latest_revision
ORDER BY m.symbol, m.ex_date
"""

def corporate_action_dividend_metrics_asof(
    as_of_date: dt.date,
    as_of_ts: dt.datetime | None = None,
    db_path: Path | str = DEFAULT_DB_PATH,
    *,
    store: "DuckDBStore | None" = None,
    symbols: tuple[str, ...] | list[str] | None = None,
) -> pd.DataFrame:
    """Return latest-visible derived cash-dividend metrics as of a point in time.

    ``available_at`` is the later of the dividend-inference and ex-date bar
    availabilities, so a dividend appears only once both legs are knowable. Pass an
    open ``store`` to read through an existing connection.
    """
    as_of_ts = as_of_ts or end_of_day_asof_ts(as_of_date)
    symbol_values = _normalize_symbols(symbols)

    def _run(active):
        registered = []
        try:
            symbol_join = ""
            if _register_filter(active, "asof_cadiv_symbol_filter", "symbol", symbol_values):
                registered.append("asof_cadiv_symbol_filter")
                symbol_join = "JOIN asof_cadiv_symbol_filter sf ON sf.symbol = m.symbol"
            sql = CORPORATE_ACTION_DIVIDEND_METRICS_ASOF_SQL.format(symbol_join=symbol_join)
            return active.con.execute(sql, [as_of_date, as_of_ts]).df()
        finally:
            for relation in registered:
                active.con.unregister(relation)

    if store is not None:
        return _run(store)
    with connect(db_path, read_only=True) as opened:
        return _run(opened)

CORPORATE_ACTION_SPLIT_METRICS_ASOF_SQL = """
WITH params AS (
    SELECT
        CAST(? AS DATE) AS as_of_date,
        CAST(? AS TIMESTAMP) AS as_of_ts
)
SELECT m.*
FROM corporate_action_split_metrics m
{symbol_join}
CROSS JOIN params p
WHERE m.available_at <= p.as_of_ts
  AND m.as_of_date <= p.as_of_date
  AND m.is_latest_revision
ORDER BY m.symbol, m.ex_date, m.bar_source
"""

def corporate_action_split_metrics_asof(
    as_of_date: dt.date,
    as_of_ts: dt.datetime | None = None,
    db_path: Path | str = DEFAULT_DB_PATH,
    *,
    store: "DuckDBStore | None" = None,
    symbols: tuple[str, ...] | list[str] | None = None,
) -> pd.DataFrame:
    """Return latest-visible split-event adjustment reconciliation rows as of a point in time.

    ``available_at`` is the max of the split event availability and adjacent daily
    adjustment-factor rows used for reconciliation. Missing daily rows are preserved
    as ``reconciliation_status='MISSING_DAILY_FACTOR'`` rather than silently dropped.
    """
    as_of_ts = as_of_ts or end_of_day_asof_ts(as_of_date)
    symbol_values = _normalize_symbols(symbols)

    def _run(active):
        registered = []
        try:
            symbol_join = ""
            if _register_filter(active, "asof_casplit_symbol_filter", "symbol", symbol_values):
                registered.append("asof_casplit_symbol_filter")
                symbol_join = "JOIN asof_casplit_symbol_filter sf ON sf.symbol = m.symbol"
            sql = CORPORATE_ACTION_SPLIT_METRICS_ASOF_SQL.format(symbol_join=symbol_join)
            return active.con.execute(sql, [as_of_date, as_of_ts]).df()
        finally:
            for relation in registered:
                active.con.unregister(relation)

    if store is not None:
        return _run(store)
    with connect(db_path, read_only=True) as opened:
        return _run(opened)

CORPORATE_ACTION_FACTOR_RECONCILIATION_ASOF_SQL = """
WITH params AS (
    SELECT
        CAST(? AS DATE) AS as_of_date,
        CAST(? AS TIMESTAMP) AS as_of_ts
)
SELECT m.*
FROM corporate_action_factor_reconciliation m
{symbol_join}
{event_type_join}
CROSS JOIN params p
WHERE m.available_at <= p.as_of_ts
  AND m.as_of_date <= p.as_of_date
  AND m.is_latest_revision
ORDER BY m.symbol, m.ex_date, m.event_type, m.bar_source
"""

def corporate_action_factor_reconciliation_asof(
    as_of_date: dt.date,
    as_of_ts: dt.datetime | None = None,
    db_path: Path | str = DEFAULT_DB_PATH,
    *,
    store: "DuckDBStore | None" = None,
    symbols: tuple[str, ...] | list[str] | None = None,
    event_types: tuple[str, ...] | list[str] | None = None,
) -> pd.DataFrame:
    """Return latest-visible event-level adjustment-factor reconciliation rows."""
    as_of_ts = as_of_ts or end_of_day_asof_ts(as_of_date)
    symbol_values = _normalize_symbols(symbols)
    event_type_values = _normalize_strings(event_types)

    def _run(active):
        registered = []
        try:
            symbol_join = ""
            event_type_join = ""
            if _register_filter(active, "asof_cafactor_symbol_filter", "symbol", symbol_values):
                registered.append("asof_cafactor_symbol_filter")
                symbol_join = "JOIN asof_cafactor_symbol_filter sf ON sf.symbol = m.symbol"
            if _register_filter(active, "asof_cafactor_event_type_filter", "event_type", event_type_values):
                registered.append("asof_cafactor_event_type_filter")
                event_type_join = "JOIN asof_cafactor_event_type_filter ef ON ef.event_type = m.event_type"
            sql = CORPORATE_ACTION_FACTOR_RECONCILIATION_ASOF_SQL.format(
                symbol_join=symbol_join,
                event_type_join=event_type_join,
            )
            return active.con.execute(sql, [as_of_date, as_of_ts]).df()
        finally:
            for relation in registered:
                active.con.unregister(relation)

    if store is not None:
        return _run(store)
    with connect(db_path, read_only=True) as opened:
        return _run(opened)

EQUITY_PRICE_METRICS_ASOF_SQL = """
WITH params AS (
    SELECT
        CAST(? AS DATE) AS as_of_date,
        CAST(? AS TIMESTAMP) AS as_of_ts
)
SELECT m.*
FROM equity_price_metrics m
{symbol_join}
CROSS JOIN params p
WHERE m.available_at <= p.as_of_ts
  AND m.as_of_date <= p.as_of_date
  AND m.is_latest_revision
ORDER BY m.symbol, m.trade_date
"""

MARKET_CAP_ASOF_SQL = """
WITH params AS (
    SELECT
        CAST(? AS DATE) AS as_of_date,
        CAST(? AS TIMESTAMP) AS as_of_ts
)
SELECT m.*
FROM market_cap m
{symbol_join}
CROSS JOIN params p
WHERE m.available_at <= p.as_of_ts
  AND m.as_of_date <= p.as_of_date
  AND m.is_latest_revision
ORDER BY m.symbol, m.trade_date
"""

ENTERPRISE_VALUE_ASOF_SQL = """
WITH params AS (
    SELECT
        CAST(? AS DATE) AS as_of_date,
        CAST(? AS TIMESTAMP) AS as_of_ts
)
SELECT e.*
FROM enterprise_value e
{symbol_join}
CROSS JOIN params p
WHERE e.available_at <= p.as_of_ts
  AND e.as_of_date <= p.as_of_date
  AND e.is_latest_revision
ORDER BY e.symbol, e.trade_date
"""

VALUATION_MULTIPLES_ASOF_SQL = """
WITH params AS (
    SELECT
        CAST(? AS DATE) AS as_of_date,
        CAST(? AS TIMESTAMP) AS as_of_ts
)
SELECT v.*
FROM valuation_multiples v
{symbol_join}
{formula_join}
{category_join}
CROSS JOIN params p
WHERE v.available_at <= p.as_of_ts
  AND v.as_of_date <= p.as_of_date
  AND v.is_latest_revision
ORDER BY v.symbol, v.formula_code, v.trade_date
"""

def market_cap_asof(
    as_of_date: dt.date,
    as_of_ts: dt.datetime | None = None,
    db_path: Path | str = DEFAULT_DB_PATH,
    *,
    store: "DuckDBStore | None" = None,
    symbols: tuple[str, ...] | list[str] | None = None,
) -> pd.DataFrame:
    """Return latest-visible market-cap rows as of a point in time."""
    as_of_ts = as_of_ts or end_of_day_asof_ts(as_of_date)
    symbol_values = _normalize_symbols(symbols)

    def _run(active):
        registered = []
        try:
            symbol_join = ""
            if _register_filter(active, "asof_market_cap_symbol_filter", "symbol", symbol_values):
                registered.append("asof_market_cap_symbol_filter")
                symbol_join = "JOIN asof_market_cap_symbol_filter sf ON sf.symbol = m.symbol"
            sql = MARKET_CAP_ASOF_SQL.format(symbol_join=symbol_join)
            return active.con.execute(sql, [as_of_date, as_of_ts]).df()
        finally:
            for relation in registered:
                active.con.unregister(relation)

    if store is not None:
        return _run(store)
    with connect(db_path, read_only=True) as opened:
        return _run(opened)

def enterprise_value_asof(
    as_of_date: dt.date,
    as_of_ts: dt.datetime | None = None,
    db_path: Path | str = DEFAULT_DB_PATH,
    *,
    store: "DuckDBStore | None" = None,
    symbols: tuple[str, ...] | list[str] | None = None,
) -> pd.DataFrame:
    """Return latest-visible enterprise-value rows as of a point in time."""
    as_of_ts = as_of_ts or end_of_day_asof_ts(as_of_date)
    symbol_values = _normalize_symbols(symbols)

    def _run(active):
        registered = []
        try:
            symbol_join = ""
            if _register_filter(active, "asof_enterprise_value_symbol_filter", "symbol", symbol_values):
                registered.append("asof_enterprise_value_symbol_filter")
                symbol_join = "JOIN asof_enterprise_value_symbol_filter sf ON sf.symbol = e.symbol"
            sql = ENTERPRISE_VALUE_ASOF_SQL.format(symbol_join=symbol_join)
            return active.con.execute(sql, [as_of_date, as_of_ts]).df()
        finally:
            for relation in registered:
                active.con.unregister(relation)

    if store is not None:
        return _run(store)
    with connect(db_path, read_only=True) as opened:
        return _run(opened)

def valuation_multiples_asof(
    as_of_date: dt.date,
    as_of_ts: dt.datetime | None = None,
    db_path: Path | str = DEFAULT_DB_PATH,
    *,
    store: "DuckDBStore | None" = None,
    symbols: tuple[str, ...] | list[str] | None = None,
    formula_codes: tuple[str, ...] | list[str] | None = None,
    categories: tuple[str, ...] | list[str] | None = None,
) -> pd.DataFrame:
    """Return latest-visible valuation multiples as of a point in time."""
    as_of_ts = as_of_ts or end_of_day_asof_ts(as_of_date)
    symbol_values = _normalize_symbols(symbols)
    formula_values = _normalize_strings(formula_codes)
    category_values = _normalize_strings(categories)

    def _run(active):
        registered = []
        try:
            symbol_join = ""
            formula_join = ""
            category_join = ""
            if _register_filter(active, "asof_valuation_symbol_filter", "symbol", symbol_values):
                registered.append("asof_valuation_symbol_filter")
                symbol_join = "JOIN asof_valuation_symbol_filter sf ON sf.symbol = v.symbol"
            if _register_filter(active, "asof_valuation_formula_filter", "formula_code", formula_values):
                registered.append("asof_valuation_formula_filter")
                formula_join = "JOIN asof_valuation_formula_filter ff ON ff.formula_code = upper(v.formula_code)"
            if _register_filter(active, "asof_valuation_category_filter", "category", category_values):
                registered.append("asof_valuation_category_filter")
                category_join = "JOIN asof_valuation_category_filter cf ON cf.category = upper(v.category)"
            sql = VALUATION_MULTIPLES_ASOF_SQL.format(
                symbol_join=symbol_join,
                formula_join=formula_join,
                category_join=category_join,
            )
            return active.con.execute(sql, [as_of_date, as_of_ts]).df()
        finally:
            for relation in registered:
                active.con.unregister(relation)

    if store is not None:
        return _run(store)
    with connect(db_path, read_only=True) as opened:
        return _run(opened)

def equity_price_metrics_asof(
    as_of_date: dt.date,
    as_of_ts: dt.datetime | None = None,
    db_path: Path | str = DEFAULT_DB_PATH,
    *,
    store: "DuckDBStore | None" = None,
    symbols: tuple[str, ...] | list[str] | None = None,
) -> pd.DataFrame:
    """Return latest-visible derived daily price metrics as of a point in time.

    ``available_at`` is carried from the source bar and, for market-relative fields,
    delayed to the latest same-day bar used in the equal-weight proxy. Pass an open
    ``store`` to read through an existing connection (DuckDB forbids a second
    connection to the same file).
    """
    as_of_ts = as_of_ts or end_of_day_asof_ts(as_of_date)
    symbol_values = _normalize_symbols(symbols)

    def _run(active):
        registered = []
        try:
            symbol_join = ""
            if _register_filter(active, "asof_eqpm_symbol_filter", "symbol", symbol_values):
                registered.append("asof_eqpm_symbol_filter")
                symbol_join = "JOIN asof_eqpm_symbol_filter sf ON sf.symbol = m.symbol"
            sql = EQUITY_PRICE_METRICS_ASOF_SQL.format(symbol_join=symbol_join)
            return active.con.execute(sql, [as_of_date, as_of_ts]).df()
        finally:
            for relation in registered:
                active.con.unregister(relation)

    if store is not None:
        return _run(store)
    with connect(db_path, read_only=True) as opened:
        return _run(opened)

MACRO_METRICS_ASOF_SQL = """
WITH params AS (
    SELECT
        CAST(? AS DATE) AS as_of_date,
        CAST(? AS TIMESTAMP) AS as_of_ts
)
SELECT m.*
FROM macro_metrics m
{series_join}
CROSS JOIN params p
WHERE m.available_at <= p.as_of_ts
  AND m.as_of_date <= p.as_of_date
  AND m.is_latest_revision
ORDER BY m.series_id, m.observation_date
"""

def macro_metrics_asof(
    as_of_date: dt.date,
    as_of_ts: dt.datetime | None = None,
    db_path: Path | str = DEFAULT_DB_PATH,
    *,
    store: "DuckDBStore | None" = None,
    series_ids: tuple[str, ...] | list[str] | None = None,
) -> pd.DataFrame:
    """Return latest-visible derived macro metrics as of a point in time.

    Latest-revision FRED (not ALFRED), so macro-revision PIT is approximate;
    ``available_at`` is carried from the source observation. Pass an open ``store``
    to read through an existing connection.
    """
    as_of_ts = as_of_ts or end_of_day_asof_ts(as_of_date)
    series_values = _normalize_strings(series_ids)

    def _run(active):
        registered = []
        try:
            series_join = ""
            if _register_filter(active, "asof_macro_series_filter", "series_id", series_values):
                registered.append("asof_macro_series_filter")
                series_join = "JOIN asof_macro_series_filter sf ON sf.series_id = upper(m.series_id)"
            sql = MACRO_METRICS_ASOF_SQL.format(series_join=series_join)
            return active.con.execute(sql, [as_of_date, as_of_ts]).df()
        finally:
            for relation in registered:
                active.con.unregister(relation)

    if store is not None:
        return _run(store)
    with connect(db_path, read_only=True) as opened:
        return _run(opened)

THIRTEENF_POSITION_METRICS_ASOF_SQL = """
WITH params AS (
    SELECT
        CAST(? AS DATE) AS as_of_date,
        CAST(? AS TIMESTAMP) AS as_of_ts
)
SELECT m.*
FROM thirteenf_position_metrics m
{symbol_join}
CROSS JOIN params p
WHERE m.available_at <= p.as_of_ts
  AND m.as_of_date <= p.as_of_date
  AND m.is_latest_revision
ORDER BY m.report_period, m.security_id, m.manager_id
"""

def thirteenf_position_metrics_asof(
    as_of_date: dt.date,
    as_of_ts: dt.datetime | None = None,
    db_path: Path | str = DEFAULT_DB_PATH,
    *,
    store: "DuckDBStore | None" = None,
    symbols: tuple[str, ...] | list[str] | None = None,
) -> pd.DataFrame:
    """Return latest-visible derived 13F manager-level position metrics as of a point in time.

    ``available_at`` is the filing's availability instant (approximate — the cached feed
    carries the warehouse ingest time, never earlier than the true SEC filing). Pass an
    open ``store`` to read through an existing connection (DuckDB forbids a second
    connection to the same file).
    """
    as_of_ts = as_of_ts or end_of_day_asof_ts(as_of_date)
    symbol_values = _normalize_symbols(symbols)

    def _run(active):
        registered = []
        try:
            symbol_join = ""
            if _register_filter(active, "asof_tf_symbol_filter", "symbol", symbol_values):
                registered.append("asof_tf_symbol_filter")
                symbol_join = "JOIN asof_tf_symbol_filter sf ON sf.symbol = m.symbol"
            sql = THIRTEENF_POSITION_METRICS_ASOF_SQL.format(symbol_join=symbol_join)
            return active.con.execute(sql, [as_of_date, as_of_ts]).df()
        finally:
            for relation in registered:
                active.con.unregister(relation)

    if store is not None:
        return _run(store)
    with connect(db_path, read_only=True) as opened:
        return _run(opened)

THIRTEENF_OPTION_METRICS_ASOF_SQL = """
WITH params AS (
    SELECT
        CAST(? AS DATE) AS as_of_date,
        CAST(? AS TIMESTAMP) AS as_of_ts
)
SELECT m.*
FROM thirteenf_option_metrics m
{symbol_join}
CROSS JOIN params p
WHERE m.available_at <= p.as_of_ts
  AND m.as_of_date <= p.as_of_date
  AND m.is_latest_revision
ORDER BY m.report_period, m.security_id, m.cusip
"""

def thirteenf_option_metrics_asof(
    as_of_date: dt.date,
    as_of_ts: dt.datetime | None = None,
    db_path: Path | str = DEFAULT_DB_PATH,
    *,
    store: "DuckDBStore | None" = None,
    symbols: tuple[str, ...] | list[str] | None = None,
) -> pd.DataFrame:
    """Return latest-visible derived 13F issuer-level option metrics as of a point in time.

    ``available_at`` is the latest filing availability among the visible call/put
    and common-share rows used for the issuer-period aggregate. Pass an open
    ``store`` to read through an existing connection.
    """
    as_of_ts = as_of_ts or end_of_day_asof_ts(as_of_date)
    symbol_values = _normalize_symbols(symbols)

    def _run(active):
        registered = []
        try:
            symbol_join = ""
            if _register_filter(active, "asof_tf_option_symbol_filter", "symbol", symbol_values):
                registered.append("asof_tf_option_symbol_filter")
                symbol_join = "JOIN asof_tf_option_symbol_filter sf ON sf.symbol = m.symbol"
            sql = THIRTEENF_OPTION_METRICS_ASOF_SQL.format(symbol_join=symbol_join)
            return active.con.execute(sql, [as_of_date, as_of_ts]).df()
        finally:
            for relation in registered:
                active.con.unregister(relation)

    if store is not None:
        return _run(store)
    with connect(db_path, read_only=True) as opened:
        return _run(opened)

THIRTEENF_CONCENTRATION_METRICS_ASOF_SQL = """
WITH params AS (
    SELECT
        CAST(? AS DATE) AS as_of_date,
        CAST(? AS TIMESTAMP) AS as_of_ts
)
SELECT m.*
FROM thirteenf_concentration_metrics m
{symbol_join}
CROSS JOIN params p
WHERE m.available_at <= p.as_of_ts
  AND m.as_of_date <= p.as_of_date
  AND m.is_latest_revision
ORDER BY m.report_period, m.security_id, m.cusip
"""

def thirteenf_concentration_metrics_asof(
    as_of_date: dt.date,
    as_of_ts: dt.datetime | None = None,
    db_path: Path | str = DEFAULT_DB_PATH,
    *,
    store: "DuckDBStore | None" = None,
    symbols: tuple[str, ...] | list[str] | None = None,
) -> pd.DataFrame:
    """Return latest-visible derived 13F issuer holder-concentration metrics."""
    as_of_ts = as_of_ts or end_of_day_asof_ts(as_of_date)
    symbol_values = _normalize_symbols(symbols)

    def _run(active):
        registered = []
        try:
            symbol_join = ""
            if _register_filter(active, "asof_tf_concentration_symbol_filter", "symbol", symbol_values):
                registered.append("asof_tf_concentration_symbol_filter")
                symbol_join = "JOIN asof_tf_concentration_symbol_filter sf ON sf.symbol = m.symbol"
            sql = THIRTEENF_CONCENTRATION_METRICS_ASOF_SQL.format(symbol_join=symbol_join)
            return active.con.execute(sql, [as_of_date, as_of_ts]).df()
        finally:
            for relation in registered:
                active.con.unregister(relation)

    if store is not None:
        return _run(store)
    with connect(db_path, read_only=True) as opened:
        return _run(opened)

SHORT_INTEREST_METRICS_ASOF_SQL = """
WITH params AS (
    SELECT
        CAST(? AS DATE) AS as_of_date,
        CAST(? AS TIMESTAMP) AS as_of_ts
)
SELECT m.*
FROM short_interest_metrics m
{symbol_join}
CROSS JOIN params p
WHERE m.available_at <= p.as_of_ts
  AND m.as_of_date <= p.as_of_date
  AND m.is_latest_revision
ORDER BY m.symbol, m.settlement_date
"""

def short_interest_metrics_asof(
    as_of_date: dt.date,
    as_of_ts: dt.datetime | None = None,
    db_path: Path | str = DEFAULT_DB_PATH,
    *,
    store: "DuckDBStore | None" = None,
    symbols: tuple[str, ...] | list[str] | None = None,
) -> pd.DataFrame:
    """Return latest-visible derived short-interest metrics as of a point in time.

    ``available_at`` is the FINRA publication time, so a settlement appears only once
    it was published. Pass an open ``store`` to read through an existing connection
    (DuckDB forbids a second connection to the same file).
    """
    as_of_ts = as_of_ts or end_of_day_asof_ts(as_of_date)
    symbol_values = _normalize_symbols(symbols)

    def _run(active):
        registered = []
        try:
            symbol_join = ""
            if _register_filter(active, "asof_si_symbol_filter", "symbol", symbol_values):
                registered.append("asof_si_symbol_filter")
                symbol_join = "JOIN asof_si_symbol_filter sf ON sf.symbol = m.symbol"
            sql = SHORT_INTEREST_METRICS_ASOF_SQL.format(symbol_join=symbol_join)
            return active.con.execute(sql, [as_of_date, as_of_ts]).df()
        finally:
            for relation in registered:
                active.con.unregister(relation)

    if store is not None:
        return _run(store)
    with connect(db_path, read_only=True) as opened:
        return _run(opened)

FORMULA_REGISTRY_ASOF_SQL = """
WITH params AS (
    SELECT
        CAST(? AS DATE) AS as_of_date
)
SELECT v.*
FROM v_formula_registry v
{code_join}
{family_join}
CROSS JOIN params p
WHERE v.valid_from <= p.as_of_date
  AND coalesce(v.valid_to, DATE '9999-12-31') > p.as_of_date
ORDER BY v.family, v.formula_code
"""

def formula_registry_asof(
    as_of_date: dt.date,
    as_of_ts: dt.datetime | None = None,
    db_path: Path | str = DEFAULT_DB_PATH,
    *,
    store: "DuckDBStore | None" = None,
    formula_codes: tuple[str, ...] | list[str] | None = None,
    families: tuple[str, ...] | list[str] | None = None,
) -> pd.DataFrame:
    """Return the formula catalog (definition + citation + inputs) as of a point in time.

    PF-S4 S4-3: a queryable/as-of surface over ``formula_registry`` (via the
    catalogued ``v_formula_registry`` view -- migration 0077) so "what is
    EV/EBITDA?" is answerable with a query instead of reading Python.

    Filters by the formula DEFINITION's own bitemporal validity --
    ``valid_from <= as_of_date < coalesce(valid_to, 9999-12-31)`` -- so a
    formula's definition HISTORY is queryable (a coefficient or citation
    revision does not erase the prior definition). ``formula_registry`` has
    no ``available_at``/knowledge-time column (only ``valid_from``/
    ``valid_to`` DEFINITION validity, unlike every fact-table ``*_asof``
    reader in this module), so unlike those readers this one takes no
    effective ``as_of_ts`` filter; the ``as_of_ts`` parameter is accepted
    only for call-site symmetry with the rest of this module and is
    otherwise unused.

    Pass an open ``store`` to read through an existing connection (DuckDB
    forbids a second connection to the same file); otherwise a read-only
    connection to ``db_path`` is opened.
    """
    del as_of_ts  # accepted for call-site symmetry; formula_registry has no available_at axis
    code_values = _normalize_strings(formula_codes)
    family_values = _normalize_strings(families)

    def _run(active):
        registered = []
        try:
            code_join = ""
            family_join = ""
            if _register_filter(active, "asof_formula_code_filter", "formula_code", code_values):
                registered.append("asof_formula_code_filter")
                code_join = "JOIN asof_formula_code_filter cf ON cf.formula_code = upper(v.formula_code)"
            if _register_filter(active, "asof_formula_family_filter", "family", family_values):
                registered.append("asof_formula_family_filter")
                family_join = "JOIN asof_formula_family_filter ff ON ff.family = upper(v.family)"
            sql = FORMULA_REGISTRY_ASOF_SQL.format(code_join=code_join, family_join=family_join)
            return active.con.execute(sql, [as_of_date]).df()
        finally:
            for relation in registered:
                active.con.unregister(relation)

    if store is not None:
        return _run(store)
    with connect(db_path, read_only=True) as opened:
        return _run(opened)
