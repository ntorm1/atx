from __future__ import annotations

import datetime as dt
from pathlib import Path

import pandas as pd

from .connection import DEFAULT_DB_PATH, connect


SECURITY_MASTER_ASOF_SQL = """
WITH params AS (
    SELECT
        CAST(? AS DATE) AS as_of_date,
        CAST(? AS TIMESTAMP) AS as_of_ts
),
ids AS (
    SELECT
        i.security_id,
        max(CASE WHEN i.id_type = 'CIK' THEN i.id_value END) AS cik,
        max(CASE WHEN i.id_type = 'CUSIP' THEN i.id_value END) AS cusip,
        max(CASE WHEN i.id_type = 'TICKER' THEN i.id_value END) AS ticker
    FROM security_identifier_history i
    CROSS JOIN params p
    WHERE i.valid_from <= p.as_of_date
      AND coalesce(i.valid_to, DATE '9999-12-31') > p.as_of_date
      AND (i.available_at IS NULL OR i.available_at <= p.as_of_ts)
    GROUP BY i.security_id
)
SELECT
    s.security_id,
    coalesce(ids.ticker, s.primary_symbol) AS symbol,
    s.name,
    s.asset_class,
    s.country,
    s.currency,
    ids.cik,
    ids.cusip
FROM securities s
JOIN ids ON ids.security_id = s.security_id
ORDER BY symbol, security_id
"""


LISTING_STATUS_ASOF_SQL = """
WITH params AS (
    SELECT
        CAST(? AS DATE) AS as_of_date,
        CAST(? AS TIMESTAMP) AS as_of_ts
)
SELECT l.*
FROM listing_status_intervals l
{symbol_join}
{status_join}
CROSS JOIN params p
WHERE l.valid_from <= p.as_of_date
  AND coalesce(l.valid_to, DATE '9999-12-31') > p.as_of_date
  AND l.as_of_date <= p.as_of_date
  AND (l.available_at IS NULL OR l.available_at <= p.as_of_ts)
ORDER BY l.symbol, l.listing_venue_code, l.status, l.valid_from, l.evidence_source
"""


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


DAILY_PANEL_ASOF_SQL = """
WITH params AS (
    SELECT
        CAST(? AS DATE) AS as_of_date,
        CAST(? AS TIMESTAMP) AS as_of_ts
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
    b.volume,
    r.simple_return,
    r.log_return
FROM equity_daily_bars b
LEFT JOIN v_equity_daily_returns r
  ON r.source = b.source
 AND r.security_id = b.security_id
 AND r.trade_date = b.trade_date
{symbol_join}
CROSS JOIN params p
WHERE b.trade_date <= p.as_of_date
  AND (b.available_at IS NULL OR b.available_at <= p.as_of_ts)
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


THIRTEENF_POSITIONING_ASOF_SQL = """
WITH params AS (
    SELECT
        CAST(? AS DATE) AS as_of_date,
        CAST(? AS TIMESTAMP) AS as_of_ts
),
eligible AS (
    SELECT
        h.source_period,
        s.period_of_report AS report_period,
        h.cusip,
        h.security_id,
        h.accession_number,
        h.share_quantity,
        h.share_quantity_type,
        h.value_usd,
        h.put_call
    FROM thirteenf_holdings h
    JOIN thirteenf_submissions s
      ON s.accession_number = h.accession_number
     AND s.source_period = h.source_period
    {cusip_join}
    CROSS JOIN params p
    WHERE s.period_of_report <= p.as_of_date
      AND s.filing_date <= p.as_of_date
      AND s.filing_date::TIMESTAMP + INTERVAL 22 HOURS <= p.as_of_ts
),
positioning AS (
    SELECT
        source_period,
        report_period,
        cusip,
        any_value(security_id) AS security_id,
        count(*) AS holding_rows,
        count(DISTINCT accession_number) AS filing_count,
        sum(
            CASE
                WHEN coalesce(put_call, '') = ''
                 AND upper(coalesce(share_quantity_type, '')) = 'SH'
                THEN coalesce(share_quantity, 0)
                ELSE 0
            END
        ) AS total_common_share_quantity,
        sum(
            CASE
                WHEN coalesce(put_call, '') = ''
                 AND upper(coalesce(share_quantity_type, '')) = 'SH'
                THEN coalesce(value_usd, 0)
                ELSE 0
            END
        ) AS total_common_value_usd,
        sum(CASE WHEN put_call = 'CALL' THEN coalesce(share_quantity, 0) ELSE 0 END) AS call_share_quantity,
        sum(CASE WHEN put_call = 'PUT' THEN coalesce(share_quantity, 0) ELSE 0 END) AS put_share_quantity
    FROM eligible
    GROUP BY source_period, report_period, cusip
),
ranked AS (
    SELECT
        *,
        row_number() OVER (
            PARTITION BY cusip
            ORDER BY report_period DESC NULLS LAST, source_period DESC
        ) AS rn
    FROM positioning
)
SELECT *
FROM ranked
WHERE rn = 1
ORDER BY cusip
"""


OWNERSHIP_ASOF_SQL = """
WITH params AS (
    SELECT
        CAST(? AS DATE) AS as_of_date,
        CAST(? AS TIMESTAMP) AS as_of_ts
),
ranked AS (
    SELECT
        o.*,
        row_number() OVER (
            PARTITION BY coalesce(o.security_id, o.cusip)
            ORDER BY o.report_period DESC NULLS LAST,
                     o.available_at DESC NULLS LAST,
                     o.source_loaded_at DESC
        ) AS rn
    FROM thirteenf_security_ownership o
    {symbol_join}
    {cusip_join}
    CROSS JOIN params p
    WHERE o.report_period <= p.as_of_date
      AND o.as_of_date <= p.as_of_date
      AND (o.available_at IS NULL OR o.available_at <= p.as_of_ts)
)
SELECT *
FROM ranked
WHERE rn = 1
ORDER BY coalesce(symbol, cusip), security_id
"""


INSIDER_TRANSACTIONS_ASOF_SQL = """
WITH params AS (
    SELECT
        CAST(? AS DATE) AS as_of_date,
        CAST(? AS TIMESTAMP) AS as_of_ts
)
SELECT t.*
FROM insider_transaction t
{security_join}
{insider_join}
{code_join}
CROSS JOIN params p
WHERE coalesce(t.transaction_date, t.as_of_date) <= p.as_of_date
  AND (t.as_of_date IS NULL OR t.as_of_date <= p.as_of_date)
  AND (t.available_at IS NULL OR t.available_at <= p.as_of_ts)
ORDER BY coalesce(t.transaction_date, t.as_of_date), t.security_id, t.insider_id, t.transaction_ordinal
"""


INSIDER_RELATIONSHIPS_ASOF_SQL = """
WITH params AS (
    SELECT
        CAST(? AS DATE) AS as_of_date,
        CAST(? AS TIMESTAMP) AS as_of_ts
)
SELECT r.*
FROM insider_relationship r
{security_join}
{insider_join}
CROSS JOIN params p
WHERE r.valid_from <= p.as_of_date
  AND coalesce(r.valid_to, DATE '9999-12-31') > p.as_of_date
  AND (r.as_of_date IS NULL OR r.as_of_date <= p.as_of_date)
  AND (r.available_at IS NULL OR r.available_at <= p.as_of_ts)
ORDER BY r.security_id, r.insider_id, r.valid_from
"""


BLOCKHOLDER_ASOF_SQL = """
WITH params AS (
    SELECT
        CAST(? AS DATE) AS as_of_date,
        CAST(? AS TIMESTAMP) AS as_of_ts
)
SELECT
    f.*,
    p.reporting_person_id,
    p.reporting_person_seq,
    p.insider_id,
    p.entity_id,
    p.reporting_person_name,
    p.type_of_reporting_person,
    p.citizenship_or_place_of_org,
    p.source_of_funds,
    p.sole_voting_power,
    p.shared_voting_power,
    p.sole_dispositive_power,
    p.shared_dispositive_power,
    p.aggregate_beneficially_owned,
    p.percent_of_class,
    p.excludes_certain_shares,
    p.legal_proceedings_flag
FROM blockholder_filing f
LEFT JOIN blockholder_reporting_person p
  ON p.filing_id = f.filing_id
{security_join}
{schedule_join}
CROSS JOIN params prm
WHERE coalesce(f.event_date, f.filing_date) <= prm.as_of_date
  AND (f.available_at IS NULL OR f.available_at <= prm.as_of_ts)
ORDER BY coalesce(f.event_date, f.filing_date), f.accession_number, p.reporting_person_seq
"""


IDENTIFIER_DECISIONS_ASOF_SQL = """
WITH params AS (
    SELECT
        CAST(? AS DATE) AS as_of_date,
        CAST(? AS TIMESTAMP) AS as_of_ts
)
SELECT d.*
FROM identifier_resolution_decisions d
{status_join}
CROSS JOIN params p
WHERE d.as_of_date <= p.as_of_date
  AND (d.available_at IS NULL OR d.available_at <= p.as_of_ts)
ORDER BY d.source_key_type, d.source_key_value, d.target_security_id, d.decision_method
"""


ENTITY_CLASSIFICATION_ASOF_SQL = """
WITH params AS (
    SELECT
        CAST(? AS DATE) AS as_of_date,
        CAST(? AS TIMESTAMP) AS as_of_ts
)
SELECT ec.*
FROM entity_classification ec
JOIN taxonomy t ON t.taxonomy_id = ec.taxonomy_id
{security_join}
{taxonomy_join}
CROSS JOIN params p
WHERE ec.valid_from <= p.as_of_date
  AND coalesce(ec.valid_to, DATE '9999-12-31') > p.as_of_date
  AND (ec.available_at IS NULL OR ec.available_at <= p.as_of_ts)
ORDER BY ec.security_id, t.code, ec.is_primary DESC, ec.valid_from
"""


def end_of_day_asof_ts(as_of_date: dt.date) -> dt.datetime:
    return dt.datetime.combine(as_of_date, dt.time(23, 59, 59))


def _normalize_symbols(symbols: tuple[str, ...] | list[str] | None) -> list[str]:
    if symbols is None:
        return []
    return sorted({str(symbol).strip().upper() for symbol in symbols if str(symbol).strip()})


def _normalize_strings(values: tuple[str, ...] | list[str] | None) -> list[str]:
    if values is None:
        return []
    return sorted({str(value).strip().upper() for value in values if str(value).strip()})


def _normalize_ids(values: tuple[str, ...] | list[str] | None) -> list[str]:
    """Normalize opaque identifiers (e.g. security_id) for filter joins.

    Unlike _normalize_strings, this does NOT upper-case: security_id is an opaque
    internal key (e.g. 'SEC-CIK-0000320193'), not a categorical code, so changing
    its case would break the join. Matches the convention in entity_classification_asof
    (which registers the raw security_id). Strips whitespace and de-dups only.
    """
    if values is None:
        return []
    return sorted({str(value).strip() for value in values if str(value).strip()})


def _register_filter(store, relation_name: str, column_name: str, values: list[str]) -> bool:
    if not values:
        return False
    store.con.register(relation_name, pd.DataFrame({column_name: values}))
    return True


def security_master_asof(
    as_of_date: dt.date,
    as_of_ts: dt.datetime | None = None,
    db_path: Path | str = DEFAULT_DB_PATH,
) -> pd.DataFrame:
    as_of_ts = as_of_ts or end_of_day_asof_ts(as_of_date)
    with connect(db_path, read_only=True) as store:
        return store.con.execute(SECURITY_MASTER_ASOF_SQL, [as_of_date, as_of_ts]).df()


def listing_status_asof(
    as_of_date: dt.date,
    as_of_ts: dt.datetime | None = None,
    db_path: Path | str = DEFAULT_DB_PATH,
    *,
    symbols: tuple[str, ...] | list[str] | None = None,
    statuses: tuple[str, ...] | list[str] | None = None,
) -> pd.DataFrame:
    as_of_ts = as_of_ts or end_of_day_asof_ts(as_of_date)
    symbol_values = _normalize_symbols(symbols)
    status_values = _normalize_strings(statuses)
    with connect(db_path, read_only=True) as store:
        registered = []
        try:
            symbol_join = ""
            status_join = ""
            if _register_filter(store, "asof_listing_status_symbol_filter", "symbol", symbol_values):
                registered.append("asof_listing_status_symbol_filter")
                symbol_join = "JOIN asof_listing_status_symbol_filter sf ON sf.symbol = l.symbol"
            if _register_filter(store, "asof_listing_status_status_filter", "status", status_values):
                registered.append("asof_listing_status_status_filter")
                status_join = "JOIN asof_listing_status_status_filter stf ON stf.status = upper(l.status)"
            sql = LISTING_STATUS_ASOF_SQL.format(symbol_join=symbol_join, status_join=status_join)
            return store.con.execute(sql, [as_of_date, as_of_ts]).df()
        finally:
            for relation in registered:
                store.con.unregister(relation)


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


def thirteenf_positioning_asof(
    as_of_date: dt.date,
    as_of_ts: dt.datetime | None = None,
    db_path: Path | str = DEFAULT_DB_PATH,
    *,
    cusips: tuple[str, ...] | list[str] | None = None,
) -> pd.DataFrame:
    as_of_ts = as_of_ts or end_of_day_asof_ts(as_of_date)
    cusip_values = _normalize_strings(cusips)
    with connect(db_path, read_only=True) as store:
        registered = []
        try:
            cusip_join = ""
            if _register_filter(store, "asof_thirteenf_cusip_filter", "cusip", cusip_values):
                registered.append("asof_thirteenf_cusip_filter")
                cusip_join = "JOIN asof_thirteenf_cusip_filter cf ON cf.cusip = h.cusip"
            sql = THIRTEENF_POSITIONING_ASOF_SQL.format(cusip_join=cusip_join)
            return store.con.execute(sql, [as_of_date, as_of_ts]).df()
        finally:
            for relation in registered:
                store.con.unregister(relation)


def ownership_asof(
    as_of_date: dt.date,
    as_of_ts: dt.datetime | None = None,
    db_path: Path | str = DEFAULT_DB_PATH,
    *,
    symbols: tuple[str, ...] | list[str] | None = None,
    cusips: tuple[str, ...] | list[str] | None = None,
) -> pd.DataFrame:
    as_of_ts = as_of_ts or end_of_day_asof_ts(as_of_date)
    symbol_values = _normalize_symbols(symbols)
    cusip_values = _normalize_strings(cusips)
    with connect(db_path, read_only=True) as store:
        registered = []
        try:
            symbol_join = ""
            cusip_join = ""
            if _register_filter(store, "asof_ownership_symbol_filter", "symbol", symbol_values):
                registered.append("asof_ownership_symbol_filter")
                symbol_join = "JOIN asof_ownership_symbol_filter sf ON sf.symbol = upper(o.symbol)"
            if _register_filter(store, "asof_ownership_cusip_filter", "cusip", cusip_values):
                registered.append("asof_ownership_cusip_filter")
                cusip_join = "JOIN asof_ownership_cusip_filter cf ON cf.cusip = upper(o.cusip)"
            sql = OWNERSHIP_ASOF_SQL.format(symbol_join=symbol_join, cusip_join=cusip_join)
            return store.con.execute(sql, [as_of_date, as_of_ts]).df()
        finally:
            for relation in registered:
                store.con.unregister(relation)


def insider_transactions_asof(
    store_or_path: "DuckDBStore | Path | str | None" = None,
    *,
    as_of_date: dt.date,
    as_of_ts: dt.datetime | None = None,
    security_ids: tuple[str, ...] | list[str] | None = None,
    insider_ids: tuple[str, ...] | list[str] | None = None,
    transaction_codes: tuple[str, ...] | list[str] | None = None,
    db_path: Path | str = DEFAULT_DB_PATH,
) -> pd.DataFrame:
    """Return Section 16 insider transactions visible as-of a PIT timestamp."""
    from .connection import DuckDBStore as _DuckDBStore

    as_of_ts = as_of_ts or end_of_day_asof_ts(as_of_date)

    def _run(store):
        registered = []
        try:
            security_join = ""
            insider_join = ""
            code_join = ""
            security_values = _normalize_ids(security_ids)
            insider_values = _normalize_ids(insider_ids)
            code_values = _normalize_strings(transaction_codes)
            if _register_filter(store, "asof_insider_tx_security_filter", "security_id", security_values):
                registered.append("asof_insider_tx_security_filter")
                security_join = "JOIN asof_insider_tx_security_filter sf ON sf.security_id = t.security_id"
            if _register_filter(store, "asof_insider_tx_insider_filter", "insider_id", insider_values):
                registered.append("asof_insider_tx_insider_filter")
                insider_join = "JOIN asof_insider_tx_insider_filter inf ON inf.insider_id = t.insider_id"
            if _register_filter(store, "asof_insider_tx_code_filter", "transaction_code", code_values):
                registered.append("asof_insider_tx_code_filter")
                code_join = "JOIN asof_insider_tx_code_filter cf ON cf.transaction_code = upper(t.transaction_code)"
            sql = INSIDER_TRANSACTIONS_ASOF_SQL.format(
                security_join=security_join,
                insider_join=insider_join,
                code_join=code_join,
            )
            return store.con.execute(sql, [as_of_date, as_of_ts]).df()
        finally:
            for relation in registered:
                store.con.unregister(relation)

    if isinstance(store_or_path, _DuckDBStore):
        return _run(store_or_path)
    path = store_or_path if store_or_path is not None else db_path
    with connect(path, read_only=True) as store:
        return _run(store)


def insider_relationships_asof(
    store_or_path: "DuckDBStore | Path | str | None" = None,
    *,
    as_of_date: dt.date,
    as_of_ts: dt.datetime | None = None,
    security_ids: tuple[str, ...] | list[str] | None = None,
    insider_ids: tuple[str, ...] | list[str] | None = None,
    db_path: Path | str = DEFAULT_DB_PATH,
) -> pd.DataFrame:
    """Return insider issuer-role relationships visible as-of a PIT timestamp."""
    from .connection import DuckDBStore as _DuckDBStore

    as_of_ts = as_of_ts or end_of_day_asof_ts(as_of_date)

    def _run(store):
        registered = []
        try:
            security_join = ""
            insider_join = ""
            security_values = _normalize_ids(security_ids)
            insider_values = _normalize_ids(insider_ids)
            if _register_filter(store, "asof_insider_rel_security_filter", "security_id", security_values):
                registered.append("asof_insider_rel_security_filter")
                security_join = "JOIN asof_insider_rel_security_filter sf ON sf.security_id = r.security_id"
            if _register_filter(store, "asof_insider_rel_insider_filter", "insider_id", insider_values):
                registered.append("asof_insider_rel_insider_filter")
                insider_join = "JOIN asof_insider_rel_insider_filter inf ON inf.insider_id = r.insider_id"
            sql = INSIDER_RELATIONSHIPS_ASOF_SQL.format(
                security_join=security_join,
                insider_join=insider_join,
            )
            return store.con.execute(sql, [as_of_date, as_of_ts]).df()
        finally:
            for relation in registered:
                store.con.unregister(relation)

    if isinstance(store_or_path, _DuckDBStore):
        return _run(store_or_path)
    path = store_or_path if store_or_path is not None else db_path
    with connect(path, read_only=True) as store:
        return _run(store)


def blockholder_asof(
    store_or_path: "DuckDBStore | Path | str | None" = None,
    *,
    as_of_date: dt.date,
    as_of_ts: dt.datetime | None = None,
    security_ids: tuple[str, ...] | list[str] | None = None,
    schedule_types: tuple[str, ...] | list[str] | None = None,
    db_path: Path | str = DEFAULT_DB_PATH,
) -> pd.DataFrame:
    """Return Schedule 13D/G blockholder rows visible as-of a PIT timestamp."""
    from .connection import DuckDBStore as _DuckDBStore

    as_of_ts = as_of_ts or end_of_day_asof_ts(as_of_date)

    def _run(store):
        registered = []
        try:
            security_join = ""
            schedule_join = ""
            security_values = _normalize_ids(security_ids)
            schedule_values = _normalize_strings(schedule_types)
            if _register_filter(store, "asof_blockholder_security_filter", "security_id", security_values):
                registered.append("asof_blockholder_security_filter")
                security_join = "JOIN asof_blockholder_security_filter sf ON sf.security_id = f.security_id"
            if _register_filter(store, "asof_blockholder_schedule_filter", "schedule_type", schedule_values):
                registered.append("asof_blockholder_schedule_filter")
                schedule_join = "JOIN asof_blockholder_schedule_filter stf ON stf.schedule_type = upper(f.schedule_type)"
            sql = BLOCKHOLDER_ASOF_SQL.format(
                security_join=security_join,
                schedule_join=schedule_join,
            )
            return store.con.execute(sql, [as_of_date, as_of_ts]).df()
        finally:
            for relation in registered:
                store.con.unregister(relation)

    if isinstance(store_or_path, _DuckDBStore):
        return _run(store_or_path)
    path = store_or_path if store_or_path is not None else db_path
    with connect(path, read_only=True) as store:
        return _run(store)


def entity_classification_asof(
    store_or_path: "DuckDBStore | Path | str | None" = None,
    *,
    security_id: str | None = None,
    taxonomy_code: str | None = None,
    as_of_date: dt.date,
    as_of_ts: dt.datetime | None = None,
    db_path: Path | str = DEFAULT_DB_PATH,
) -> pd.DataFrame:
    """Return PIT-valid entity_classification rows.

    Parameters
    ----------
    store_or_path:
        Accepts a live DuckDBStore (for tests) or a path (for CLI use).
        When None, falls back to db_path.
    security_id:
        Optional filter to a single security.
    taxonomy_code:
        Optional filter by taxonomy code (e.g. 'SIC', 'FAMA_FRENCH_12').
    as_of_date:
        The point-in-time date for valid_from/valid_to evaluation.
    as_of_ts:
        The point-in-time timestamp for available_at evaluation.
        Defaults to end-of-day on as_of_date.
    """
    from .connection import DuckDBStore as _DuckDBStore

    as_of_ts = as_of_ts or end_of_day_asof_ts(as_of_date)

    def _run(store):
        registered = []
        try:
            security_join = ""
            taxonomy_join = ""
            if security_id is not None:
                store.con.register(
                    "asof_ec_security_filter",
                    pd.DataFrame({"security_id": [security_id]}),
                )
                registered.append("asof_ec_security_filter")
                security_join = "JOIN asof_ec_security_filter sf ON sf.security_id = ec.security_id"
            if taxonomy_code is not None:
                store.con.register(
                    "asof_ec_taxonomy_filter",
                    pd.DataFrame({"code": [taxonomy_code]}),
                )
                registered.append("asof_ec_taxonomy_filter")
                taxonomy_join = "JOIN asof_ec_taxonomy_filter tf ON tf.code = t.code"
            sql = ENTITY_CLASSIFICATION_ASOF_SQL.format(
                security_join=security_join,
                taxonomy_join=taxonomy_join,
            )
            return store.con.execute(sql, [as_of_date, as_of_ts]).df()
        finally:
            for relation in registered:
                store.con.unregister(relation)

    if isinstance(store_or_path, _DuckDBStore):
        return _run(store_or_path)

    path = store_or_path if store_or_path is not None else db_path
    with connect(path, read_only=True) as store:
        return _run(store)


def est_actual_asof(
    store_or_path: "DuckDBStore | Path | str | None" = None,
    *,
    as_of_date: dt.date,
    as_of_ts: dt.datetime | None = None,
    security_ids: tuple[str, ...] | list[str] | None = None,
    measure_codes: tuple[str, ...] | list[str] | None = None,
    db_path: Path | str = DEFAULT_DB_PATH,
) -> pd.DataFrame:
    """Return the latest revision of each (security_id, measure_code, period_end) as-of a PIT ts.

    Rows with available_at > as_of_ts are hidden (PIT semantics).
    Latest revision = highest available_at <= as_of_ts per (security_id, measure_code, period_end).
    """
    from .connection import DuckDBStore as _DuckDBStore

    as_of_ts = as_of_ts or end_of_day_asof_ts(as_of_date)

    def _run(store):
        registered = []
        try:
            sid_join = ""
            mc_join = ""
            sid_values = _normalize_ids(security_ids)
            mc_values = _normalize_strings(measure_codes)
            if sid_values:
                store.con.register(
                    "asof_est_actual_sid_filter",
                    pd.DataFrame({"security_id": sid_values}),
                )
                registered.append("asof_est_actual_sid_filter")
                sid_join = "JOIN asof_est_actual_sid_filter sf ON sf.security_id = a.security_id"
            if mc_values:
                store.con.register(
                    "asof_est_actual_mc_filter",
                    pd.DataFrame({"measure_code": mc_values}),
                )
                registered.append("asof_est_actual_mc_filter")
                mc_join = "JOIN asof_est_actual_mc_filter mf ON mf.measure_code = a.measure_code"
            sql = f"""
            WITH ranked AS (
                SELECT
                    a.*,
                    row_number() OVER (
                        PARTITION BY a.security_id, a.measure_code, a.period_end
                        ORDER BY a.available_at DESC NULLS LAST
                    ) AS rn
                FROM est_actual a
                {sid_join}
                {mc_join}
                WHERE (a.available_at IS NULL OR a.available_at <= CAST(? AS TIMESTAMP))
            )
            SELECT * EXCLUDE (rn) FROM ranked WHERE rn = 1
            ORDER BY security_id, measure_code, period_end
            """
            return store.con.execute(sql, [as_of_ts]).df()
        finally:
            for relation in registered:
                store.con.unregister(relation)

    if isinstance(store_or_path, _DuckDBStore):
        return _run(store_or_path)
    path = store_or_path if store_or_path is not None else db_path
    with connect(path, read_only=True) as store:
        return _run(store)


def est_surprise_asof(
    store_or_path: "DuckDBStore | Path | str | None" = None,
    *,
    as_of_date: dt.date,
    as_of_ts: dt.datetime | None = None,
    security_ids: tuple[str, ...] | list[str] | None = None,
    measure_codes: tuple[str, ...] | list[str] | None = None,
    db_path: Path | str = DEFAULT_DB_PATH,
) -> pd.DataFrame:
    """Return est_surprise rows visible as-of a PIT timestamp.

    est_surprise has ONE row per (security_id, measure_code, fiscal_year, fiscal_period)
    (no revision chain) but available_at controls when the row becomes visible.
    """
    from .connection import DuckDBStore as _DuckDBStore

    as_of_ts = as_of_ts or end_of_day_asof_ts(as_of_date)

    def _run(store):
        registered = []
        try:
            sid_join = ""
            mc_join = ""
            sid_values = _normalize_ids(security_ids)
            mc_values = _normalize_strings(measure_codes)
            if sid_values:
                store.con.register(
                    "asof_est_surprise_sid_filter",
                    pd.DataFrame({"security_id": sid_values}),
                )
                registered.append("asof_est_surprise_sid_filter")
                sid_join = "JOIN asof_est_surprise_sid_filter sf ON sf.security_id = s.security_id"
            if mc_values:
                store.con.register(
                    "asof_est_surprise_mc_filter",
                    pd.DataFrame({"measure_code": mc_values}),
                )
                registered.append("asof_est_surprise_mc_filter")
                mc_join = "JOIN asof_est_surprise_mc_filter mf ON mf.measure_code = s.measure_code"
            sql = f"""
            SELECT s.*
            FROM est_surprise s
            {sid_join}
            {mc_join}
            WHERE (s.available_at IS NULL OR s.available_at <= CAST(? AS TIMESTAMP))
            ORDER BY s.security_id, s.measure_code, s.period_end
            """
            return store.con.execute(sql, [as_of_ts]).df()
        finally:
            for relation in registered:
                store.con.unregister(relation)

    if isinstance(store_or_path, _DuckDBStore):
        return _run(store_or_path)
    path = store_or_path if store_or_path is not None else db_path
    with connect(path, read_only=True) as store:
        return _run(store)


def est_consensus_asof(
    store_or_path: "DuckDBStore | Path | str | None" = None,
    *,
    as_of_date: dt.date,
    as_of_ts: dt.datetime | None = None,
    security_ids: tuple[str, ...] | list[str] | None = None,
    measure_codes: tuple[str, ...] | list[str] | None = None,
    db_path: Path | str = DEFAULT_DB_PATH,
) -> pd.DataFrame:
    """Return est_consensus rows visible as-of a PIT timestamp."""
    from .connection import DuckDBStore as _DuckDBStore

    as_of_ts = as_of_ts or end_of_day_asof_ts(as_of_date)

    def _run(store):
        registered = []
        try:
            sid_join = ""
            mc_join = ""
            sid_values = _normalize_ids(security_ids)
            mc_values = _normalize_strings(measure_codes)
            if sid_values:
                store.con.register(
                    "asof_est_consensus_sid_filter",
                    pd.DataFrame({"security_id": sid_values}),
                )
                registered.append("asof_est_consensus_sid_filter")
                sid_join = "JOIN asof_est_consensus_sid_filter sf ON sf.security_id = c.security_id"
            if mc_values:
                store.con.register(
                    "asof_est_consensus_mc_filter",
                    pd.DataFrame({"measure_code": mc_values}),
                )
                registered.append("asof_est_consensus_mc_filter")
                mc_join = "JOIN asof_est_consensus_mc_filter mf ON mf.measure_code = c.measure_code"
            sql = f"""
            SELECT c.*
            FROM est_consensus c
            {sid_join}
            {mc_join}
            WHERE (c.available_at IS NULL OR c.available_at <= CAST(? AS TIMESTAMP))
            ORDER BY c.security_id, c.measure_code, c.period_end
            """
            return store.con.execute(sql, [as_of_ts]).df()
        finally:
            for relation in registered:
                store.con.unregister(relation)

    if isinstance(store_or_path, _DuckDBStore):
        return _run(store_or_path)
    path = store_or_path if store_or_path is not None else db_path
    with connect(path, read_only=True) as store:
        return _run(store)


def est_guidance_asof(
    store_or_path: "DuckDBStore | Path | str | None" = None,
    *,
    as_of_date: dt.date,
    as_of_ts: dt.datetime | None = None,
    security_ids: tuple[str, ...] | list[str] | None = None,
    measure_codes: tuple[str, ...] | list[str] | None = None,
    db_path: Path | str = DEFAULT_DB_PATH,
) -> pd.DataFrame:
    """Return est_guidance rows visible as-of a PIT timestamp."""
    from .connection import DuckDBStore as _DuckDBStore

    as_of_ts = as_of_ts or end_of_day_asof_ts(as_of_date)

    def _run(store):
        registered = []
        try:
            sid_join = ""
            mc_join = ""
            sid_values = _normalize_ids(security_ids)
            mc_values = _normalize_strings(measure_codes)
            if sid_values:
                store.con.register(
                    "asof_est_guidance_sid_filter",
                    pd.DataFrame({"security_id": sid_values}),
                )
                registered.append("asof_est_guidance_sid_filter")
                sid_join = "JOIN asof_est_guidance_sid_filter sf ON sf.security_id = g.security_id"
            if mc_values:
                store.con.register(
                    "asof_est_guidance_mc_filter",
                    pd.DataFrame({"measure_code": mc_values}),
                )
                registered.append("asof_est_guidance_mc_filter")
                mc_join = "JOIN asof_est_guidance_mc_filter mf ON mf.measure_code = g.measure_code"
            sql = f"""
            SELECT g.*
            FROM est_guidance g
            {sid_join}
            {mc_join}
            WHERE (g.available_at IS NULL OR g.available_at <= CAST(? AS TIMESTAMP))
            ORDER BY g.security_id, g.measure_code, g.period_end
            """
            return store.con.execute(sql, [as_of_ts]).df()
        finally:
            for relation in registered:
                store.con.unregister(relation)

    if isinstance(store_or_path, _DuckDBStore):
        return _run(store_or_path)
    path = store_or_path if store_or_path is not None else db_path
    with connect(path, read_only=True) as store:
        return _run(store)


def identifier_decisions_asof(
    as_of_date: dt.date,
    as_of_ts: dt.datetime | None = None,
    db_path: Path | str = DEFAULT_DB_PATH,
    *,
    decision_statuses: tuple[str, ...] | list[str] | None = None,
) -> pd.DataFrame:
    as_of_ts = as_of_ts or end_of_day_asof_ts(as_of_date)
    status_values = _normalize_strings(decision_statuses)
    with connect(db_path, read_only=True) as store:
        registered = []
        try:
            status_join = ""
            if _register_filter(store, "asof_identifier_decision_status_filter", "decision_status", status_values):
                registered.append("asof_identifier_decision_status_filter")
                status_join = "JOIN asof_identifier_decision_status_filter sf ON sf.decision_status = upper(d.decision_status)"
            sql = IDENTIFIER_DECISIONS_ASOF_SQL.format(status_join=status_join)
            return store.con.execute(sql, [as_of_date, as_of_ts]).df()
        finally:
            for relation in registered:
                store.con.unregister(relation)
