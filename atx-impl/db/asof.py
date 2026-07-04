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


DELISTING_EVENTS_ASOF_SQL = """
WITH params AS (
    SELECT
        CAST(? AS DATE) AS as_of_date,
        CAST(? AS TIMESTAMP) AS as_of_ts
),
visible_events AS (
    SELECT d.*
    FROM delisting_events d
    {symbol_join}
    {code_join}
    CROSS JOIN params p
    WHERE d.delist_date <= p.as_of_date
      AND d.as_of_date <= p.as_of_date
      AND d.available_at <= p.as_of_ts
),
observation_candidates AS (
    SELECT
        d.delisting_event_id,
        o.delisting_return_observation_id,
        o.source,
        o.provider,
        o.delisting_return,
        row_number() OVER (
            PARTITION BY d.delisting_event_id
            ORDER BY o.available_at DESC, o.source_loaded_at DESC, o.delisting_return_observation_id DESC
        ) AS observation_rank
    FROM visible_events d
    JOIN delisting_return_observations o
      ON o.delist_date = d.delist_date
     AND (
            (
                o.security_id IS NOT NULL
                AND d.security_id IS NOT NULL
                AND o.security_id = d.security_id
            )
         OR (
                o.security_id IS NULL
                AND o.symbol IS NOT NULL
                AND o.symbol = d.symbol
            )
     )
    CROSS JOIN params p
    WHERE o.as_of_date <= p.as_of_date
      AND o.available_at <= p.as_of_ts
)
SELECT
    d.* REPLACE (
        coalesce(o.delisting_return, d.delisting_return) AS delisting_return,
        CASE
            WHEN o.delisting_return_observation_id IS NOT NULL THEN 'OBSERVED_SOURCE'
            ELSE d.delisting_return_type
        END AS delisting_return_type,
        CASE
            WHEN o.delisting_return_observation_id IS NOT NULL THEN false
            ELSE d.is_return_imputed
        END AS is_return_imputed,
        CASE
            WHEN o.delisting_return_observation_id IS NOT NULL THEN 'observed_source'
            ELSE d.return_policy
        END AS return_policy,
        CASE
            WHEN o.delisting_return_observation_id IS NOT NULL THEN 'high'
            ELSE d.return_confidence
        END AS return_confidence,
        coalesce(o.delisting_return_observation_id, d.return_observation_id) AS return_observation_id,
        coalesce(o.source, d.return_observation_source) AS return_observation_source,
        coalesce(o.provider, d.return_observation_provider) AS return_observation_provider
    )
FROM visible_events d
LEFT JOIN observation_candidates o
  ON o.delisting_event_id = d.delisting_event_id
 AND o.observation_rank = 1
ORDER BY d.symbol, d.delist_date, d.delist_code, d.evidence_confidence DESC
"""


DELISTING_RETURN_OBSERVATIONS_ASOF_SQL = """
WITH params AS (
    SELECT
        CAST(? AS DATE) AS as_of_date,
        CAST(? AS TIMESTAMP) AS as_of_ts
)
SELECT o.*
FROM delisting_return_observations o
{symbol_join}
{provider_join}
CROSS JOIN params p
WHERE o.delist_date <= p.as_of_date
  AND o.as_of_date <= p.as_of_date
  AND o.available_at <= p.as_of_ts
ORDER BY o.provider, o.symbol, o.vendor_security_id, o.delist_date
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


INSIDER_TRANSACTION_METRICS_ASOF_SQL = """
WITH params AS (
    SELECT
        CAST(? AS DATE) AS as_of_date,
        CAST(? AS TIMESTAMP) AS as_of_ts
),
visible AS (
    SELECT
        m.*,
        row_number() OVER (
            PARTITION BY m.source, m.security_id, m.signal_date, m.window_days
            ORDER BY m.available_at DESC, m.metric_id
        ) AS rn
    FROM insider_transaction_metrics m
    {security_join}
    {symbol_join}
    CROSS JOIN params p
    WHERE m.signal_date <= p.as_of_date
      AND m.as_of_date <= p.as_of_date
      AND m.available_at <= p.as_of_ts
)
SELECT
    metric_id, source, input_source, security_id, issuer_cik, issuer_name,
    issuer_trading_symbol, signal_date, window_days, cluster_min_buyers,
    cluster_min_purchase_value, transaction_count, open_market_purchase_count,
    open_market_sale_count, discretionary_sale_count, plan_sale_count,
    grant_count, exercise_count, tax_withholding_count, unique_insider_count,
    buyer_count, seller_count, director_count, officer_count,
    ten_percent_owner_count, gross_purchase_shares, gross_sale_shares,
    net_purchase_shares, gross_purchase_value, gross_sale_value,
    discretionary_sale_value, plan_sale_value, net_purchase_value,
    cluster_purchase_count, cluster_buyer_count, cluster_purchase_value,
    cluster_sale_count, cluster_seller_count, cluster_sale_value,
    plan_sale_value_ratio, is_cluster_buy, is_discretionary_sell_pressure,
    is_10b5_1_heavy_sale, source_transaction_ids_json, restatement_seq,
    is_latest_revision, as_of_date, available_at, run_id, source_loaded_at
FROM visible
WHERE rn = 1
ORDER BY signal_date, security_id
"""


SECURITY_LISTING_METRICS_ASOF_SQL = """
WITH params AS (
    SELECT
        CAST(? AS DATE) AS as_of_date,
        CAST(? AS TIMESTAMP) AS as_of_ts
),
visible AS (
    SELECT
        m.*,
        row_number() OVER (
            PARTITION BY m.source, m.security_id
            ORDER BY m.as_of_date DESC, m.available_at DESC, m.metric_id
        ) AS rn
    FROM security_listing_metrics m
    {security_join}
    {symbol_join}
    CROSS JOIN params p
    WHERE m.as_of_date <= p.as_of_date
      AND m.available_at <= p.as_of_ts
)
SELECT
    metric_id, source, input_listing_source, security_id, symbol, as_of_date,
    directory, listing_status, listing_venue_code, listing_venue_name,
    listing_exchange_code, listing_exchange_name, market_category, market_tier,
    security_name, round_lot_size, is_etf, is_test_issue, is_next_shares,
    financial_status_code, financial_status_label, has_financial_status,
    is_listing_compliant, is_deficient, is_delinquent, is_bankrupt, is_noncompliant,
    restatement_seq, is_latest_revision, available_at, run_id, source_loaded_at
FROM visible
WHERE rn = 1
ORDER BY security_id
"""


FORM144_INTENTS_ASOF_SQL = """
WITH params AS (
    SELECT
        CAST(? AS DATE) AS as_of_date,
        CAST(? AS TIMESTAMP) AS as_of_ts
),
visible_intents AS (
    SELECT f.*
    FROM form144_intent f
    {security_join}
    {symbol_join}
    CROSS JOIN params p
    WHERE coalesce(f.as_of_date, f.notice_date, f.filing_date, f.approx_sale_date) <= p.as_of_date
      AND coalesce(f.is_latest, true)
      AND (f.available_at IS NULL OR f.available_at <= p.as_of_ts)
),
visible_links AS (
    SELECT
        l.form144_filing_id,
        count(*) AS matched_transaction_count,
        sum(coalesce(l.shares_matched, 0)) AS matched_shares,
        sum(coalesce(l.value_matched, 0)) AS matched_value,
        max(l.match_confidence) AS max_match_confidence,
        max(l.available_at) AS latest_match_available_at
    FROM form144_to_form4_link l
    JOIN insider_transaction t ON t.transaction_id = l.insider_transaction_id
    CROSS JOIN params p
    WHERE (l.available_at IS NULL OR l.available_at <= p.as_of_ts)
      AND coalesce(t.transaction_date, t.as_of_date) <= p.as_of_date
      AND (t.available_at IS NULL OR t.available_at <= p.as_of_ts)
    GROUP BY l.form144_filing_id
)
SELECT
    f.*,
    coalesce(l.matched_transaction_count, 0) AS matched_transaction_count,
    coalesce(l.matched_shares, 0) AS matched_shares,
    coalesce(l.matched_value, 0) AS matched_value,
    CASE
        WHEN f.shares_proposed IS NOT NULL AND f.shares_proposed <> 0
        THEN coalesce(l.matched_shares, 0) / f.shares_proposed
        ELSE NULL
    END AS completion_ratio,
    l.max_match_confidence,
    l.latest_match_available_at
FROM visible_intents f
LEFT JOIN visible_links l ON l.form144_filing_id = f.filing_id
ORDER BY coalesce(f.filing_date, f.approx_sale_date), f.issuer_trading_symbol, f.seller_name
"""


FORM144_RECONCILIATION_ASOF_SQL = """
WITH params AS (
    SELECT
        CAST(? AS DATE) AS as_of_date,
        CAST(? AS TIMESTAMP) AS as_of_ts
)
SELECT
    l.form144_filing_id,
    l.insider_transaction_id,
    coalesce(l.security_id, f.security_id) AS security_id,
    coalesce(l.insider_id, t.insider_id) AS insider_id,
    coalesce(l.issuer_cik, f.issuer_cik) AS issuer_cik,
    l.seller_cik,
    l.intent_notice_date,
    l.approx_sale_date AS link_approx_sale_date,
    l.transaction_date AS link_transaction_date,
    l.days_between,
    l.shares_proposed AS link_shares_proposed,
    l.transaction_shares AS link_transaction_shares,
    l.execution_ratio,
    l.match_confidence,
    l.match_method,
    l.match_status,
    l.shares_matched,
    l.value_matched,
    l.share_match_ratio,
    l.sale_date,
    l.as_of_date,
    l.available_at,
    l.details_json,
    l.source,
    l.run_id,
    l.source_loaded_at,
    f.accession_number AS form144_accession_number,
    f.seller_name,
    f.issuer_name,
    f.issuer_trading_symbol,
    f.approx_sale_date,
    f.sale_window_end_date,
    f.shares_proposed,
    t.accession_number AS form4_accession_number,
    t.insider_id,
    t.transaction_date,
    t.transaction_shares,
    t.transaction_price,
    t.available_at AS form4_available_at
FROM form144_to_form4_link l
JOIN form144_intent f ON f.filing_id = l.form144_filing_id
JOIN insider_transaction t ON t.transaction_id = l.insider_transaction_id
{security_join}
{symbol_join}
CROSS JOIN params p
WHERE coalesce(l.as_of_date, f.as_of_date, f.notice_date, f.filing_date, f.approx_sale_date) <= p.as_of_date
  AND (f.available_at IS NULL OR f.available_at <= p.as_of_ts)
  AND coalesce(t.transaction_date, t.as_of_date) <= p.as_of_date
  AND (t.available_at IS NULL OR t.available_at <= p.as_of_ts)
  AND (l.available_at IS NULL OR l.available_at <= p.as_of_ts)
ORDER BY f.approx_sale_date, f.seller_name, t.transaction_date
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


ESTIMATE_SECURITY_LINKS_CTE = """
links AS (
    SELECT l.*
    FROM est_security_link l
    CROSS JOIN params p
    WHERE l.link_status = 'accepted'
      AND l.as_of_date <= p.as_of_date
      AND l.available_at <= p.as_of_ts
      AND l.valid_from <= p.as_of_date
      AND coalesce(l.valid_to, DATE '9999-12-31') > p.as_of_date
    QUALIFY row_number() OVER (
        PARTITION BY l.provider, l.vendor_security_id_type, l.vendor_security_id
        ORDER BY
            l.confidence DESC,
            l.as_of_date DESC,
            l.available_at DESC,
            l.source_loaded_at DESC,
            l.est_security_link_id DESC
    ) = 1
)
"""


ESTIMATE_SECURITY_LINK_JOIN = """
LEFT JOIN links l
  ON upper(l.provider) = upper(coalesce({alias}.provider, ''))
 AND upper(l.vendor_security_id_type) = upper(coalesce({alias}.vendor_security_id_type, ''))
 AND upper(l.vendor_security_id) = upper(coalesce({alias}.vendor_security_id, ''))
"""


ESTIMATE_SECURITY_LINK_ASOF_SQL = """
WITH params AS (
    SELECT
        CAST(? AS DATE) AS as_of_date,
        CAST(? AS TIMESTAMP) AS as_of_ts
)
SELECT l.*
FROM est_security_link l
{provider_join}
{id_type_join}
{security_join}
CROSS JOIN params p
WHERE l.as_of_date <= p.as_of_date
  AND l.available_at <= p.as_of_ts
  AND l.valid_from <= p.as_of_date
  AND coalesce(l.valid_to, DATE '9999-12-31') > p.as_of_date
  {status_filter}
ORDER BY l.provider, l.vendor_security_id_type, l.vendor_security_id, l.link_status, l.confidence DESC
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


def delisting_events_asof(
    as_of_date: dt.date,
    as_of_ts: dt.datetime | None = None,
    db_path: Path | str = DEFAULT_DB_PATH,
    *,
    symbols: tuple[str, ...] | list[str] | None = None,
    delist_codes: tuple[str, ...] | list[str] | None = None,
) -> pd.DataFrame:
    as_of_ts = as_of_ts or end_of_day_asof_ts(as_of_date)
    symbol_values = _normalize_symbols(symbols)
    code_values = _normalize_strings(delist_codes)
    with connect(db_path, read_only=True) as store:
        registered = []
        try:
            symbol_join = ""
            code_join = ""
            if _register_filter(store, "asof_delisting_symbol_filter", "symbol", symbol_values):
                registered.append("asof_delisting_symbol_filter")
                symbol_join = "JOIN asof_delisting_symbol_filter sf ON sf.symbol = d.symbol"
            if _register_filter(store, "asof_delisting_code_filter", "delist_code", code_values):
                registered.append("asof_delisting_code_filter")
                code_join = "JOIN asof_delisting_code_filter dcf ON dcf.delist_code = d.delist_code"
            sql = DELISTING_EVENTS_ASOF_SQL.format(symbol_join=symbol_join, code_join=code_join)
            return store.con.execute(sql, [as_of_date, as_of_ts]).df()
        finally:
            for relation in registered:
                store.con.unregister(relation)


def delisting_return_observations_asof(
    as_of_date: dt.date,
    as_of_ts: dt.datetime | None = None,
    db_path: Path | str = DEFAULT_DB_PATH,
    *,
    symbols: tuple[str, ...] | list[str] | None = None,
    providers: tuple[str, ...] | list[str] | None = None,
) -> pd.DataFrame:
    as_of_ts = as_of_ts or end_of_day_asof_ts(as_of_date)
    symbol_values = _normalize_symbols(symbols)
    provider_values = _normalize_strings(providers)
    with connect(db_path, read_only=True) as store:
        registered = []
        try:
            symbol_join = ""
            provider_join = ""
            if _register_filter(store, "asof_delisting_return_symbol_filter", "symbol", symbol_values):
                registered.append("asof_delisting_return_symbol_filter")
                symbol_join = "JOIN asof_delisting_return_symbol_filter sf ON sf.symbol = o.symbol"
            if _register_filter(store, "asof_delisting_return_provider_filter", "provider", provider_values):
                registered.append("asof_delisting_return_provider_filter")
                provider_join = "JOIN asof_delisting_return_provider_filter pf ON pf.provider = o.provider"
            sql = DELISTING_RETURN_OBSERVATIONS_ASOF_SQL.format(
                symbol_join=symbol_join,
                provider_join=provider_join,
            )
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


def insider_transaction_metrics_asof(
    store_or_path: "DuckDBStore | Path | str | None" = None,
    *,
    as_of_date: dt.date,
    as_of_ts: dt.datetime | None = None,
    security_ids: tuple[str, ...] | list[str] | None = None,
    symbols: tuple[str, ...] | list[str] | None = None,
    db_path: Path | str = DEFAULT_DB_PATH,
) -> pd.DataFrame:
    """Return latest-visible insider transaction metrics as of a PIT timestamp."""
    from .connection import DuckDBStore as _DuckDBStore

    as_of_ts = as_of_ts or end_of_day_asof_ts(as_of_date)

    def _run(store):
        registered = []
        try:
            security_join = ""
            symbol_join = ""
            security_values = _normalize_ids(security_ids)
            symbol_values = _normalize_strings(symbols)
            if _register_filter(store, "asof_insider_metric_security_filter", "security_id", security_values):
                registered.append("asof_insider_metric_security_filter")
                security_join = "JOIN asof_insider_metric_security_filter sf ON sf.security_id = m.security_id"
            if _register_filter(store, "asof_insider_metric_symbol_filter", "symbol", symbol_values):
                registered.append("asof_insider_metric_symbol_filter")
                symbol_join = "JOIN asof_insider_metric_symbol_filter symf ON symf.symbol = upper(m.issuer_trading_symbol)"
            sql = INSIDER_TRANSACTION_METRICS_ASOF_SQL.format(
                security_join=security_join,
                symbol_join=symbol_join,
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


def security_listing_metrics_asof(
    store_or_path: "DuckDBStore | Path | str | None" = None,
    *,
    as_of_date: dt.date,
    as_of_ts: dt.datetime | None = None,
    security_ids: tuple[str, ...] | list[str] | None = None,
    symbols: tuple[str, ...] | list[str] | None = None,
    db_path: Path | str = DEFAULT_DB_PATH,
) -> pd.DataFrame:
    """Return the latest-visible security listing reference row per security as of a PIT timestamp."""
    from .connection import DuckDBStore as _DuckDBStore

    as_of_ts = as_of_ts or end_of_day_asof_ts(as_of_date)

    def _run(store):
        registered = []
        try:
            security_join = ""
            symbol_join = ""
            security_values = _normalize_ids(security_ids)
            symbol_values = _normalize_strings(symbols)
            if _register_filter(store, "asof_listing_metric_security_filter", "security_id", security_values):
                registered.append("asof_listing_metric_security_filter")
                security_join = "JOIN asof_listing_metric_security_filter sf ON sf.security_id = m.security_id"
            if _register_filter(store, "asof_listing_metric_symbol_filter", "symbol", symbol_values):
                registered.append("asof_listing_metric_symbol_filter")
                symbol_join = "JOIN asof_listing_metric_symbol_filter symf ON symf.symbol = upper(m.symbol)"
            sql = SECURITY_LISTING_METRICS_ASOF_SQL.format(
                security_join=security_join,
                symbol_join=symbol_join,
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


def form144_intents_asof(
    store_or_path: "DuckDBStore | Path | str | None" = None,
    *,
    as_of_date: dt.date,
    as_of_ts: dt.datetime | None = None,
    security_ids: tuple[str, ...] | list[str] | None = None,
    symbols: tuple[str, ...] | list[str] | None = None,
    db_path: Path | str = DEFAULT_DB_PATH,
) -> pd.DataFrame:
    """Return Form 144 sale-intent rows visible as of a PIT timestamp."""
    from .connection import DuckDBStore as _DuckDBStore

    as_of_ts = as_of_ts or end_of_day_asof_ts(as_of_date)

    def _run(store):
        registered = []
        try:
            security_join = ""
            symbol_join = ""
            security_values = _normalize_ids(security_ids)
            symbol_values = _normalize_strings(symbols)
            if _register_filter(store, "asof_form144_security_filter", "security_id", security_values):
                registered.append("asof_form144_security_filter")
                security_join = "JOIN asof_form144_security_filter sf ON sf.security_id = f.security_id"
            if _register_filter(store, "asof_form144_symbol_filter", "symbol", symbol_values):
                registered.append("asof_form144_symbol_filter")
                symbol_join = "JOIN asof_form144_symbol_filter symf ON symf.symbol = upper(f.issuer_trading_symbol)"
            sql = FORM144_INTENTS_ASOF_SQL.format(
                security_join=security_join,
                symbol_join=symbol_join,
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


def form144_reconciliation_asof(
    store_or_path: "DuckDBStore | Path | str | None" = None,
    *,
    as_of_date: dt.date,
    as_of_ts: dt.datetime | None = None,
    security_ids: tuple[str, ...] | list[str] | None = None,
    symbols: tuple[str, ...] | list[str] | None = None,
    db_path: Path | str = DEFAULT_DB_PATH,
) -> pd.DataFrame:
    """Return visible Form 144 to Form 4 reconciliation links."""
    from .connection import DuckDBStore as _DuckDBStore

    as_of_ts = as_of_ts or end_of_day_asof_ts(as_of_date)

    def _run(store):
        registered = []
        try:
            security_join = ""
            symbol_join = ""
            security_values = _normalize_ids(security_ids)
            symbol_values = _normalize_strings(symbols)
            if _register_filter(store, "asof_form144_link_security_filter", "security_id", security_values):
                registered.append("asof_form144_link_security_filter")
                security_join = "JOIN asof_form144_link_security_filter sf ON sf.security_id = f.security_id"
            if _register_filter(store, "asof_form144_link_symbol_filter", "symbol", symbol_values):
                registered.append("asof_form144_link_symbol_filter")
                symbol_join = "JOIN asof_form144_link_symbol_filter symf ON symf.symbol = upper(f.issuer_trading_symbol)"
            sql = FORM144_RECONCILIATION_ASOF_SQL.format(
                security_join=security_join,
                symbol_join=symbol_join,
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


def est_security_links_asof(
    store_or_path: "DuckDBStore | Path | str | None" = None,
    *,
    as_of_date: dt.date,
    as_of_ts: dt.datetime | None = None,
    security_ids: tuple[str, ...] | list[str] | None = None,
    providers: tuple[str, ...] | list[str] | None = None,
    vendor_security_id_types: tuple[str, ...] | list[str] | None = None,
    link_statuses: tuple[str, ...] | list[str] | None = ("accepted",),
    db_path: Path | str = DEFAULT_DB_PATH,
) -> pd.DataFrame:
    """Return PIT-visible estimate vendor-id to security_id links."""
    from .connection import DuckDBStore as _DuckDBStore

    as_of_ts = as_of_ts or end_of_day_asof_ts(as_of_date)

    def _run(store):
        registered = []
        try:
            provider_join = ""
            id_type_join = ""
            security_join = ""
            status_filter = ""
            provider_values = _normalize_strings(providers)
            id_type_values = _normalize_strings(vendor_security_id_types)
            security_values = _normalize_ids(security_ids)
            status_values = _normalize_strings(link_statuses)
            if _register_filter(store, "asof_est_sec_link_provider_filter", "provider", provider_values):
                registered.append("asof_est_sec_link_provider_filter")
                provider_join = "JOIN asof_est_sec_link_provider_filter pf ON pf.provider = l.provider"
            if _register_filter(store, "asof_est_sec_link_id_type_filter", "vendor_security_id_type", id_type_values):
                registered.append("asof_est_sec_link_id_type_filter")
                id_type_join = "JOIN asof_est_sec_link_id_type_filter tf ON tf.vendor_security_id_type = l.vendor_security_id_type"
            if _register_filter(store, "asof_est_sec_link_security_filter", "target_security_id", security_values):
                registered.append("asof_est_sec_link_security_filter")
                security_join = "JOIN asof_est_sec_link_security_filter sf ON sf.target_security_id = l.target_security_id"
            if status_values:
                store.con.register(
                    "asof_est_sec_link_status_filter",
                    pd.DataFrame({"link_status": status_values}),
                )
                registered.append("asof_est_sec_link_status_filter")
                status_filter = """
                  AND l.link_status IN (
                      SELECT lower(link_status) FROM asof_est_sec_link_status_filter
                  )
                """
            sql = ESTIMATE_SECURITY_LINK_ASOF_SQL.format(
                provider_join=provider_join,
                id_type_join=id_type_join,
                security_join=security_join,
                status_filter=status_filter,
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


def est_consensus_asof(
    store_or_path: "DuckDBStore | Path | str | None" = None,
    *,
    as_of_date: dt.date,
    as_of_ts: dt.datetime | None = None,
    security_ids: tuple[str, ...] | list[str] | None = None,
    symbols: tuple[str, ...] | list[str] | None = None,
    measure_codes: tuple[str, ...] | list[str] | None = None,
    providers: tuple[str, ...] | list[str] | None = None,
    include_stale: bool = False,
    stale_after_days: int = 105,
    db_path: Path | str = DEFAULT_DB_PATH,
) -> pd.DataFrame:
    """Return latest est_consensus snapshots visible as-of a PIT timestamp."""
    from .connection import DuckDBStore as _DuckDBStore

    as_of_ts = as_of_ts or end_of_day_asof_ts(as_of_date)
    stale_min_date = as_of_date - dt.timedelta(days=max(int(stale_after_days), 0))

    def _run(store):
        registered = []
        try:
            joins: list[str] = []
            sid_values = _normalize_ids(security_ids)
            symbol_values = _normalize_symbols(symbols)
            mc_values = _normalize_strings(measure_codes)
            provider_values = _normalize_strings(providers)
            if sid_values:
                store.con.register("asof_est_consensus_sid_filter", pd.DataFrame({"security_id": sid_values}))
                registered.append("asof_est_consensus_sid_filter")
                joins.append("JOIN asof_est_consensus_sid_filter sf ON sf.security_id = v.security_id")
            if symbol_values:
                store.con.register("asof_est_consensus_symbol_filter", pd.DataFrame({"symbol": symbol_values}))
                registered.append("asof_est_consensus_symbol_filter")
                joins.append("JOIN asof_est_consensus_symbol_filter syf ON syf.symbol = v.symbol")
            if mc_values:
                store.con.register("asof_est_consensus_mc_filter", pd.DataFrame({"measure_code": mc_values}))
                registered.append("asof_est_consensus_mc_filter")
                joins.append("JOIN asof_est_consensus_mc_filter mf ON mf.measure_code = v.measure_code")
            if provider_values:
                store.con.register("asof_est_consensus_provider_filter", pd.DataFrame({"provider": provider_values}))
                registered.append("asof_est_consensus_provider_filter")
                joins.append("JOIN asof_est_consensus_provider_filter pf ON pf.provider = v.provider")
            stale_filter = ""
            params: list[object] = [as_of_date, as_of_ts]
            if not include_stale:
                stale_filter = """
                  AND (
                        (c.stale_after_date IS NOT NULL AND c.stale_after_date >= CAST(? AS DATE))
                     OR (c.stale_after_date IS NULL AND c.consensus_date >= CAST(? AS DATE))
                  )
                """
                params.extend([as_of_date, stale_min_date])
            sql = f"""
            WITH params AS (
                SELECT
                    CAST(? AS DATE) AS as_of_date,
                    CAST(? AS TIMESTAMP) AS as_of_ts
            ),
            {ESTIMATE_SECURITY_LINKS_CTE},
            visible AS (
                SELECT
                    c.* REPLACE (coalesce(l.target_security_id, c.security_id) AS security_id),
                    c.security_id AS source_security_id,
                    l.est_security_link_id AS security_link_id,
                    l.link_method AS security_link_method,
                    l.confidence AS security_link_confidence
                FROM est_consensus c
                {ESTIMATE_SECURITY_LINK_JOIN.format(alias='c')}
                CROSS JOIN params p
                WHERE (c.available_at IS NULL OR c.available_at <= p.as_of_ts)
                  AND (c.consensus_date IS NULL OR c.consensus_date <= p.as_of_date)
                  AND (c.as_of_date IS NULL OR c.as_of_date <= p.as_of_date)
                  {stale_filter}
            ),
            filtered AS (
                SELECT v.*
                FROM visible v
                {' '.join(joins)}
            ),
            ranked AS (
                SELECT
                    filtered.*,
                    row_number() OVER (
                        PARTITION BY
                            coalesce(filtered.security_id, ''),
                            coalesce(filtered.symbol, ''),
                            coalesce(filtered.vendor_security_id_type, ''),
                            coalesce(filtered.vendor_security_id, ''),
                            filtered.measure_code,
                            filtered.period_end,
                            coalesce(filtered.fpi, ''),
                            coalesce(filtered.currency, ''),
                            coalesce(filtered.pdf, ''),
                            coalesce(filtered.basis, '')
                        ORDER BY
                            coalesce(filtered.available_at, filtered.source_loaded_at) DESC,
                            filtered.consensus_date DESC NULLS LAST,
                            filtered.source_loaded_at DESC,
                            coalesce(filtered.est_consensus_id, '') DESC
                    ) AS rn
                FROM filtered
            )
            SELECT * EXCLUDE (rn)
            FROM ranked
            WHERE rn = 1
            ORDER BY provider, symbol, security_id, measure_code, period_end
            """
            return store.con.execute(sql, params).df()
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
              AND (g.as_of_date IS NULL OR g.as_of_date <= CAST(? AS DATE))
              AND (g.guidance_date IS NULL OR g.guidance_date <= CAST(? AS DATE))
            ORDER BY g.security_id, g.measure_code, g.period_end
            """
            return store.con.execute(sql, [as_of_ts, as_of_date, as_of_date]).df()
        finally:
            for relation in registered:
                store.con.unregister(relation)

    if isinstance(store_or_path, _DuckDBStore):
        return _run(store_or_path)
    path = store_or_path if store_or_path is not None else db_path
    with connect(path, read_only=True) as store:
        return _run(store)


def est_detail_asof(
    store_or_path: "DuckDBStore | Path | str | None" = None,
    *,
    as_of_date: dt.date,
    as_of_ts: dt.datetime | None = None,
    security_ids: tuple[str, ...] | list[str] | None = None,
    symbols: tuple[str, ...] | list[str] | None = None,
    measure_codes: tuple[str, ...] | list[str] | None = None,
    providers: tuple[str, ...] | list[str] | None = None,
    broker_ids: tuple[str, ...] | list[str] | None = None,
    analyst_ids: tuple[str, ...] | list[str] | None = None,
    db_path: Path | str = DEFAULT_DB_PATH,
) -> pd.DataFrame:
    """Return active detail-estimate rows visible as-of a PIT timestamp.

    The query is strict about bitemporal visibility:
    available_at gates feed availability, announce/estimate/as_of dates gate event
    time, revision_date acts as an inclusive valid-through date when present, and
    stopped estimates are hidden after stop_date.
    """
    from .connection import DuckDBStore as _DuckDBStore

    as_of_ts = as_of_ts or end_of_day_asof_ts(as_of_date)

    def _run(store):
        registered = []
        try:
            joins = []
            sid_values = _normalize_ids(security_ids)
            symbol_values = _normalize_symbols(symbols)
            mc_values = _normalize_strings(measure_codes)
            provider_values = _normalize_strings(providers)
            broker_values = _normalize_ids(broker_ids)
            analyst_values = _normalize_ids(analyst_ids)
            if _register_filter(store, "asof_est_detail_sid_filter", "security_id", sid_values):
                registered.append("asof_est_detail_sid_filter")
                joins.append("JOIN asof_est_detail_sid_filter sf ON sf.security_id = v.security_id")
            if _register_filter(store, "asof_est_detail_symbol_filter", "symbol", symbol_values):
                registered.append("asof_est_detail_symbol_filter")
                joins.append("JOIN asof_est_detail_symbol_filter syf ON syf.symbol = v.symbol")
            if _register_filter(store, "asof_est_detail_mc_filter", "measure_code", mc_values):
                registered.append("asof_est_detail_mc_filter")
                joins.append("JOIN asof_est_detail_mc_filter mf ON mf.measure_code = v.measure_code")
            if _register_filter(store, "asof_est_detail_provider_filter", "provider", provider_values):
                registered.append("asof_est_detail_provider_filter")
                joins.append("JOIN asof_est_detail_provider_filter pf ON pf.provider = v.provider")
            if _register_filter(store, "asof_est_detail_broker_filter", "broker_id", broker_values):
                registered.append("asof_est_detail_broker_filter")
                joins.append("JOIN asof_est_detail_broker_filter bf ON bf.broker_id = v.broker_id")
            if _register_filter(store, "asof_est_detail_analyst_filter", "analyst_id", analyst_values):
                registered.append("asof_est_detail_analyst_filter")
                joins.append("JOIN asof_est_detail_analyst_filter af ON af.analyst_id = v.analyst_id")
            sql = f"""
            WITH params AS (
                SELECT
                    CAST(? AS DATE) AS as_of_date,
                    CAST(? AS TIMESTAMP) AS as_of_ts
            ),
            {ESTIMATE_SECURITY_LINKS_CTE},
            visible AS (
                SELECT
                    d.* REPLACE (coalesce(l.target_security_id, d.security_id) AS security_id),
                    d.security_id AS source_security_id,
                    l.est_security_link_id AS security_link_id,
                    l.link_method AS security_link_method,
                    l.confidence AS security_link_confidence
                FROM est_detail d
                {ESTIMATE_SECURITY_LINK_JOIN.format(alias='d')}
                CROSS JOIN params p
                WHERE (d.available_at IS NULL OR d.available_at <= p.as_of_ts)
                  AND (d.as_of_date IS NULL OR d.as_of_date <= p.as_of_date)
                  AND (d.estimate_date IS NULL OR d.estimate_date <= p.as_of_date)
                  AND (d.announce_date IS NULL OR d.announce_date <= p.as_of_date)
                  AND (d.revision_date IS NULL OR d.revision_date >= p.as_of_date)
                  AND (d.stop_date IS NULL OR d.stop_date > p.as_of_date)
                  AND coalesce(d.estimate_type, '') <> 'S'
            ),
            filtered AS (
                SELECT v.*
                FROM visible v
                {' '.join(joins)}
            ),
            ranked AS (
                SELECT
                    filtered.*,
                    row_number() OVER (
                        PARTITION BY
                            coalesce(filtered.security_id, ''),
                            coalesce(filtered.symbol, ''),
                            coalesce(filtered.vendor_security_id, ''),
                            coalesce(filtered.measure_code, ''),
                            filtered.period_end,
                            coalesce(filtered.broker_id, ''),
                            coalesce(filtered.analyst_id, ''),
                            coalesce(filtered.pdf, ''),
                            coalesce(filtered.basis, '')
                        ORDER BY
                            filtered.available_at DESC NULLS LAST,
                            filtered.revision_date DESC NULLS LAST,
                            filtered.activation_date DESC NULLS LAST,
                            filtered.announce_date DESC NULLS LAST,
                            filtered.source_loaded_at DESC NULLS LAST,
                            filtered.est_detail_id DESC NULLS LAST
                    ) AS rn
                FROM filtered
            )
            SELECT * EXCLUDE (rn)
            FROM ranked
            WHERE rn = 1
            ORDER BY provider, symbol, security_id, measure_code, period_end, broker_id, analyst_id
            """
            return store.con.execute(sql, [as_of_date, as_of_ts]).df()
        finally:
            for relation in registered:
                store.con.unregister(relation)

    if isinstance(store_or_path, _DuckDBStore):
        return _run(store_or_path)
    path = store_or_path if store_or_path is not None else db_path
    with connect(path, read_only=True) as store:
        return _run(store)


def est_recommendation_asof(
    store_or_path: "DuckDBStore | Path | str | None" = None,
    *,
    as_of_date: dt.date,
    as_of_ts: dt.datetime | None = None,
    security_ids: tuple[str, ...] | list[str] | None = None,
    symbols: tuple[str, ...] | list[str] | None = None,
    providers: tuple[str, ...] | list[str] | None = None,
    event_types: tuple[str, ...] | list[str] | None = None,
    broker_ids: tuple[str, ...] | list[str] | None = None,
    analyst_ids: tuple[str, ...] | list[str] | None = None,
    db_path: Path | str = DEFAULT_DB_PATH,
) -> pd.DataFrame:
    """Return latest visible broker recommendation rows as-of a PIT timestamp."""
    from .connection import DuckDBStore as _DuckDBStore

    as_of_ts = as_of_ts or end_of_day_asof_ts(as_of_date)

    def _run(store):
        registered = []
        try:
            joins = []
            sid_values = _normalize_ids(security_ids)
            symbol_values = _normalize_symbols(symbols)
            provider_values = _normalize_strings(providers)
            event_type_values = _normalize_strings(event_types)
            broker_values = _normalize_ids(broker_ids)
            analyst_values = _normalize_ids(analyst_ids)
            if _register_filter(store, "asof_est_rec_sid_filter", "security_id", sid_values):
                registered.append("asof_est_rec_sid_filter")
                joins.append("JOIN asof_est_rec_sid_filter sf ON sf.security_id = v.security_id")
            if _register_filter(store, "asof_est_rec_symbol_filter", "symbol", symbol_values):
                registered.append("asof_est_rec_symbol_filter")
                joins.append("JOIN asof_est_rec_symbol_filter syf ON syf.symbol = v.symbol")
            if _register_filter(store, "asof_est_rec_provider_filter", "provider", provider_values):
                registered.append("asof_est_rec_provider_filter")
                joins.append("JOIN asof_est_rec_provider_filter pf ON pf.provider = v.provider")
            if _register_filter(store, "asof_est_rec_event_type_filter", "event_type", event_type_values):
                registered.append("asof_est_rec_event_type_filter")
                joins.append("JOIN asof_est_rec_event_type_filter ef ON ef.event_type = v.event_type")
            if _register_filter(store, "asof_est_rec_broker_filter", "broker_id", broker_values):
                registered.append("asof_est_rec_broker_filter")
                joins.append("JOIN asof_est_rec_broker_filter bf ON bf.broker_id = v.broker_id")
            if _register_filter(store, "asof_est_rec_analyst_filter", "analyst_id", analyst_values):
                registered.append("asof_est_rec_analyst_filter")
                joins.append("JOIN asof_est_rec_analyst_filter af ON af.analyst_id = v.analyst_id")
            sql = f"""
            WITH params AS (
                SELECT
                    CAST(? AS DATE) AS as_of_date,
                    CAST(? AS TIMESTAMP) AS as_of_ts
            ),
            {ESTIMATE_SECURITY_LINKS_CTE},
            visible AS (
                SELECT
                    r.* REPLACE (coalesce(l.target_security_id, r.security_id) AS security_id),
                    r.security_id AS source_security_id,
                    l.est_security_link_id AS security_link_id,
                    l.link_method AS security_link_method,
                    l.confidence AS security_link_confidence
                FROM est_recommendation r
                {ESTIMATE_SECURITY_LINK_JOIN.format(alias='r')}
                CROSS JOIN params p
                WHERE (r.available_at IS NULL OR r.available_at <= p.as_of_ts)
                  AND (r.as_of_date IS NULL OR r.as_of_date <= p.as_of_date)
                  AND (r.rating_date IS NULL OR r.rating_date <= p.as_of_date)
                  AND (r.announce_date IS NULL OR r.announce_date <= p.as_of_date)
                  AND (r.revision_date IS NULL OR r.revision_date >= p.as_of_date)
                  AND (r.stop_date IS NULL OR r.stop_date > p.as_of_date)
            ),
            filtered AS (
                SELECT v.*
                FROM visible v
                {' '.join(joins)}
            ),
            ranked AS (
                SELECT
                    filtered.*,
                    row_number() OVER (
                        PARTITION BY
                            coalesce(filtered.security_id, ''),
                            coalesce(filtered.symbol, ''),
                            coalesce(filtered.vendor_security_id, ''),
                            coalesce(filtered.event_type, ''),
                            coalesce(filtered.broker_id, ''),
                            coalesce(filtered.analyst_id, '')
                        ORDER BY
                            filtered.available_at DESC NULLS LAST,
                            filtered.activation_date DESC NULLS LAST,
                            filtered.rating_date DESC NULLS LAST,
                            filtered.announce_date DESC NULLS LAST,
                            filtered.source_loaded_at DESC NULLS LAST,
                            filtered.est_recommendation_id DESC NULLS LAST
                    ) AS rn
                FROM filtered
            )
            SELECT * EXCLUDE (rn)
            FROM ranked
            WHERE rn = 1
            ORDER BY provider, symbol, security_id, event_type, broker_id, analyst_id, rating_date
            """
            return store.con.execute(sql, [as_of_date, as_of_ts]).df()
        finally:
            for relation in registered:
                store.con.unregister(relation)

    if isinstance(store_or_path, _DuckDBStore):
        return _run(store_or_path)
    path = store_or_path if store_or_path is not None else db_path
    with connect(path, read_only=True) as store:
        return _run(store)


def est_recommendation_summary_asof(
    store_or_path: "DuckDBStore | Path | str | None" = None,
    *,
    as_of_date: dt.date,
    as_of_ts: dt.datetime | None = None,
    security_ids: tuple[str, ...] | list[str] | None = None,
    symbols: tuple[str, ...] | list[str] | None = None,
    providers: tuple[str, ...] | list[str] | None = None,
    source_vendor_tables: tuple[str, ...] | list[str] | None = None,
    db_path: Path | str = DEFAULT_DB_PATH,
) -> pd.DataFrame:
    """Return latest visible aggregate recommendation/target snapshots as-of PIT."""
    from .connection import DuckDBStore as _DuckDBStore

    as_of_ts = as_of_ts or end_of_day_asof_ts(as_of_date)

    def _run(store):
        registered = []
        try:
            joins = []
            sid_values = _normalize_ids(security_ids)
            symbol_values = _normalize_symbols(symbols)
            provider_values = _normalize_strings(providers)
            table_values = _normalize_strings(source_vendor_tables)
            if _register_filter(store, "asof_est_rec_summary_sid_filter", "security_id", sid_values):
                registered.append("asof_est_rec_summary_sid_filter")
                joins.append("JOIN asof_est_rec_summary_sid_filter sf ON sf.security_id = v.security_id")
            if _register_filter(store, "asof_est_rec_summary_symbol_filter", "symbol", symbol_values):
                registered.append("asof_est_rec_summary_symbol_filter")
                joins.append("JOIN asof_est_rec_summary_symbol_filter syf ON syf.symbol = v.symbol")
            if _register_filter(store, "asof_est_rec_summary_provider_filter", "provider", provider_values):
                registered.append("asof_est_rec_summary_provider_filter")
                joins.append("JOIN asof_est_rec_summary_provider_filter pf ON pf.provider = v.provider")
            if _register_filter(store, "asof_est_rec_summary_table_filter", "source_vendor_table", table_values):
                registered.append("asof_est_rec_summary_table_filter")
                joins.append(
                    "JOIN asof_est_rec_summary_table_filter tf "
                    "ON tf.source_vendor_table = upper(v.source_vendor_table)"
                )
            sql = f"""
            WITH params AS (
                SELECT
                    CAST(? AS DATE) AS as_of_date,
                    CAST(? AS TIMESTAMP) AS as_of_ts
            ),
            {ESTIMATE_SECURITY_LINKS_CTE},
            visible AS (
                SELECT
                    s.* REPLACE (coalesce(l.target_security_id, s.security_id) AS security_id),
                    s.security_id AS source_security_id,
                    l.est_security_link_id AS security_link_id,
                    l.link_method AS security_link_method,
                    l.confidence AS security_link_confidence
                FROM est_recommendation_summary s
                {ESTIMATE_SECURITY_LINK_JOIN.format(alias='s')}
                CROSS JOIN params p
                WHERE s.available_at <= p.as_of_ts
                  AND s.as_of_date <= p.as_of_date
                  AND s.snapshot_date <= p.as_of_date
            ),
            filtered AS (
                SELECT v.*
                FROM visible v
                {' '.join(joins)}
            ),
            ranked AS (
                SELECT
                    filtered.*,
                    row_number() OVER (
                        PARTITION BY
                            coalesce(filtered.security_id, ''),
                            coalesce(filtered.symbol, ''),
                            coalesce(filtered.vendor_security_id_type, ''),
                            coalesce(filtered.vendor_security_id, ''),
                            coalesce(filtered.provider, ''),
                            coalesce(filtered.source_vendor_table, '')
                        ORDER BY
                            filtered.snapshot_date DESC,
                            filtered.available_at DESC,
                            filtered.source_loaded_at DESC,
                            filtered.est_recommendation_summary_id DESC
                    ) AS rn
                FROM filtered
            )
            SELECT * EXCLUDE (rn)
            FROM ranked
            WHERE rn = 1
            ORDER BY provider, symbol, security_id, source_vendor_table, snapshot_date
            """
            return store.con.execute(sql, [as_of_date, as_of_ts]).df()
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


FILER_ALIASES_ASOF_SQL = """
WITH params AS (
    SELECT
        CAST(? AS DATE) AS as_of_date,
        CAST(? AS TIMESTAMP) AS as_of_ts,
        CAST(? AS DOUBLE) AS min_conf
)
SELECT fa.*
FROM filer_13f_cik_alias fa
{cik_join}
{type_join}
CROSS JOIN params p
WHERE fa.valid_from <= p.as_of_date
  AND coalesce(fa.valid_to, DATE '9999-12-31') > p.as_of_date
  AND (fa.available_at IS NULL OR fa.available_at <= p.as_of_ts)
  AND fa.confidence >= p.min_conf
ORDER BY fa.alias_cik, fa.alias_type, fa.valid_from
"""


def filer_aliases_asof(
    store_or_path: "DuckDBStore | Path | str | None" = None,
    *,
    alias_cik: str | None = None,
    alias_types: tuple[str, ...] | list[str] | None = None,
    min_confidence: float = 0.0,
    as_of_date: dt.date,
    as_of_ts: dt.datetime | None = None,
    db_path: Path | str = DEFAULT_DB_PATH,
) -> pd.DataFrame:
    """Return PIT-valid 13F filer-alias rows.

    The default ``min_confidence=0.0`` surfaces every alias type (including
    low-confidence NAME_MATCH_CANDIDATE links) for inspection; raise it to 1.0 to
    see only the authoritative rollup spine. Use ``resolve_primary_cik`` for actual
    CIK resolution, which defaults to authoritative-only.
    """
    from .connection import DuckDBStore as _DuckDBStore

    as_of_ts = as_of_ts or end_of_day_asof_ts(as_of_date)
    type_values = _normalize_strings(alias_types)

    def _run(store):
        registered = []
        try:
            cik_join = ""
            type_join = ""
            if alias_cik is not None:
                store.con.register("asof_filer_alias_cik_filter", pd.DataFrame({"alias_cik": [alias_cik]}))
                registered.append("asof_filer_alias_cik_filter")
                cik_join = "JOIN asof_filer_alias_cik_filter cf ON cf.alias_cik = fa.alias_cik"
            if type_values:
                store.con.register("asof_filer_alias_type_filter", pd.DataFrame({"alias_type": type_values}))
                registered.append("asof_filer_alias_type_filter")
                type_join = "JOIN asof_filer_alias_type_filter tf ON tf.alias_type = fa.alias_type"
            sql = FILER_ALIASES_ASOF_SQL.format(cik_join=cik_join, type_join=type_join)
            return store.con.execute(sql, [as_of_date, as_of_ts, float(min_confidence)]).df()
        finally:
            for relation in registered:
                store.con.unregister(relation)

    if isinstance(store_or_path, _DuckDBStore):
        return _run(store_or_path)

    path = store_or_path if store_or_path is not None else db_path
    with connect(path, read_only=True) as store:
        return _run(store)


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


def offexchange_volume_asof(
    store_or_path: "DuckDBStore | Path | str | None" = None,
    *,
    symbols: tuple[str, ...] | list[str] | None = None,
    as_of_date: dt.date,
    as_of_ts: dt.datetime | None = None,
    db_path: Path | str = DEFAULT_DB_PATH,
) -> pd.DataFrame:
    """Return latest-visible FINRA off-exchange volume rows as of a point in time."""
    from .connection import DuckDBStore as _DuckDBStore

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
    from .connection import DuckDBStore as _DuckDBStore

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


def finra_short_volume_asof(
    store_or_path: "DuckDBStore | Path | str | None" = None,
    *,
    symbols: tuple[str, ...] | list[str] | None = None,
    as_of_date: dt.date,
    as_of_ts: dt.datetime | None = None,
    db_path: Path | str = DEFAULT_DB_PATH,
) -> pd.DataFrame:
    """Return latest-visible FINRA daily short-volume rows as of a point in time."""
    from .connection import DuckDBStore as _DuckDBStore

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
    from .connection import DuckDBStore as _DuckDBStore

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


def offexchange_quality_report_asof(
    store_or_path: "DuckDBStore | Path | str | None" = None,
    *,
    surfaces: tuple[str, ...] | list[str] | None = None,
    as_of_date: dt.date,
    as_of_ts: dt.datetime | None = None,
    db_path: Path | str = DEFAULT_DB_PATH,
) -> pd.DataFrame:
    """Return latest-visible off-exchange quality reports as of a point in time."""
    from .connection import DuckDBStore as _DuckDBStore

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


WAREHOUSE_CATALOG_ASOF_SQL = """
WITH params AS (
    SELECT
        CAST(? AS DATE) AS as_of_date,
        CAST(? AS TIMESTAMP) AS as_of_ts
)
SELECT v.*
FROM v_warehouse_catalog v
{table_join}
{layer_join}
CROSS JOIN params p
WHERE v.table_updated_at <= p.as_of_ts
  AND (v.field_updated_at IS NULL OR v.field_updated_at <= p.as_of_ts)
ORDER BY v.table_name, v.field_name
"""


def warehouse_catalog_asof(
    as_of_date: dt.date,
    as_of_ts: dt.datetime | None = None,
    db_path: Path | str = DEFAULT_DB_PATH,
    *,
    store: "DuckDBStore | None" = None,
    tables: tuple[str, ...] | list[str] | None = None,
    layers: tuple[str, ...] | list[str] | None = None,
) -> pd.DataFrame:
    """Return the warehouse data catalog (table+field+best-effort formula lineage) as of a point in time.

    PF2-S1 S1-3: a queryable/as-of surface over the catalogued ``v_warehouse_catalog`` view
    (migration 0099) so "what did the warehouse catalog look like on date D" is answerable
    with a query instead of introspecting a live connection.

    CRITICAL PIT difference from ``formula_registry_asof``: ``table_catalog`` and
    ``field_catalog`` carry ``updated_at`` (knowledge time), NOT ``valid_from``/``valid_to``
    DEFINITION validity. So unlike ``formula_registry_asof`` (which takes no effective
    ``as_of_ts`` because ``formula_registry`` has no knowledge-time column), this reader DOES
    use the timestamp: a catalog row whose ``updated_at`` is AFTER the as-of instant is
    EXCLUDED (no lookahead). ``effective_ts`` is ``as_of_ts`` when given, else
    ``end_of_day_asof_ts(as_of_date)``.

    Pass an open ``store`` to read through an existing connection (DuckDB forbids a second
    connection to the same file); otherwise a read-only connection to ``db_path`` is opened.
    """
    effective_ts = as_of_ts if as_of_ts is not None else end_of_day_asof_ts(as_of_date)
    table_values = _normalize_strings(tables)
    layer_values = _normalize_strings(layers)

    def _run(active):
        registered = []
        try:
            table_join = ""
            layer_join = ""
            if _register_filter(active, "asof_warehouse_catalog_table_filter", "table_name", table_values):
                registered.append("asof_warehouse_catalog_table_filter")
                table_join = (
                    "JOIN asof_warehouse_catalog_table_filter tf ON tf.table_name = upper(v.table_name)"
                )
            if _register_filter(active, "asof_warehouse_catalog_layer_filter", "layer", layer_values):
                registered.append("asof_warehouse_catalog_layer_filter")
                layer_join = (
                    "JOIN asof_warehouse_catalog_layer_filter lf ON lf.layer = upper(v.layer)"
                )
            sql = WAREHOUSE_CATALOG_ASOF_SQL.format(table_join=table_join, layer_join=layer_join)
            return active.con.execute(sql, [as_of_date, effective_ts]).df()
        finally:
            for relation in registered:
                active.con.unregister(relation)

    if store is not None:
        return _run(store)
    with connect(db_path, read_only=True) as opened:
        return _run(opened)


def _parse_csv_arg(value: str | None) -> list[str] | None:
    """Split a comma-separated CLI argument into a normalized list, or None if empty."""
    if not value:
        return None
    parsed = [part.strip() for part in value.split(",") if part.strip()]
    return parsed or None


def _build_arg_parser():
    import argparse

    parser = argparse.ArgumentParser(
        prog="python -m db.asof",
        description="Thin as-of CLI over db/asof.py readers.",
    )
    subparsers = parser.add_subparsers(dest="command", required=True)

    catalog_parser = subparsers.add_parser(
        "warehouse-catalog",
        help="Print the as-of warehouse data catalog (table_catalog + field_catalog + best-effort formula lineage).",
    )
    catalog_parser.add_argument(
        "--as-of", dest="as_of_date", type=dt.date.fromisoformat, required=True, help="As-of date (YYYY-MM-DD)."
    )
    catalog_parser.add_argument(
        "--as-of-ts", type=dt.datetime.fromisoformat, default=None, help="Optional as-of knowledge-time timestamp."
    )
    catalog_parser.add_argument("--db-path", type=Path, default=DEFAULT_DB_PATH)
    catalog_parser.add_argument("--tables", help="Comma-separated table_name filter.")
    catalog_parser.add_argument("--layers", help="Comma-separated layer filter.")

    return parser


def main(argv: list[str] | None = None) -> int:
    parser = _build_arg_parser()
    args = parser.parse_args(argv)
    if args.command == "warehouse-catalog":
        frame = warehouse_catalog_asof(
            args.as_of_date,
            as_of_ts=args.as_of_ts,
            db_path=args.db_path,
            tables=_parse_csv_arg(args.tables),
            layers=_parse_csv_arg(args.layers),
        )
        print(frame.to_string(index=False))
        return 0
    parser.error(f"unknown command: {args.command}")
    return 2


if __name__ == "__main__":
    raise SystemExit(main())
