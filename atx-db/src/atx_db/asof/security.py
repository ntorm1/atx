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
