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
    from ..connection import DuckDBStore as _DuckDBStore

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
    from ..connection import DuckDBStore as _DuckDBStore

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
    from ..connection import DuckDBStore as _DuckDBStore

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
    from ..connection import DuckDBStore as _DuckDBStore

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
    from ..connection import DuckDBStore as _DuckDBStore

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
    from ..connection import DuckDBStore as _DuckDBStore

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
    from ..connection import DuckDBStore as _DuckDBStore

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
