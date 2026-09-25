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


def observation_vendor_last_trade_sql(alias: str = "o") -> str:
    """SQL DATE of a ``delisting_return_observations`` row's vendor last-trade date (DLSTDT).

    Loads normalize ``delist_date`` to the first session after the vendor's CRSP-style DLSTDT
    (``delisting._effective_observation_delist_dates``), keep the vendor value in
    ``raw_payload_json`` (``$.delist_date``) and default ``as_of_date`` to it. The raw value
    wins when it parses (ISO date/timestamp, ``YYYYMMDD`` or ``MM/DD/YYYY``); otherwise an
    ``as_of_date`` before ``delist_date`` is the vendor date of a shifted row; otherwise the
    stored date is itself the DLSTDT (a row landed without normalization). Never later than
    ``delist_date``: a row is normalization-shifted exactly when this is earlier.
    """

    raw = f"json_extract_string({alias}.raw_payload_json, '$.delist_date')"
    return (
        f"least({alias}.delist_date, coalesce("
        f"CAST(TRY_CAST({raw} AS TIMESTAMP) AS DATE), "
        f"CAST(try_strptime({raw}, '%Y%m%d') AS DATE), "
        f"CAST(try_strptime({raw}, '%m/%d/%Y') AS DATE), "
        f"CASE WHEN {alias}.as_of_date < {alias}.delist_date THEN {alias}.as_of_date END, "
        f"{alias}.delist_date))"
    )


# The observation match is a UNION ALL of two equi-joins (never an OR of the two date keys:
# that plans as a blockwise nested loop over every event x observation pair).
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
event_keys AS (
    SELECT
        delisting_event_id,
        security_id,
        symbol,
        delist_date,
        TRY_CAST(json_extract_string(details_json, '$.last_observed_trade_date') AS DATE) AS last_trade
    FROM visible_events
),
visible_observations AS (
    SELECT
        o.delisting_return_observation_id,
        o.source,
        o.provider,
        o.delisting_return,
        o.security_id,
        o.symbol,
        o.delist_date,
        o.available_at,
        o.source_loaded_at,
        __VENDOR_LAST_TRADE__ AS vendor_last_trade
    FROM delisting_return_observations o
    CROSS JOIN params p
    WHERE o.as_of_date <= p.as_of_date
      AND o.available_at <= p.as_of_ts
),
observation_matches AS (
    -- An observation keyed on the event's delist_date (the first session after the last
    -- trade; loads normalize the vendor DLSTDT to it) is the event's.
    SELECT e.delisting_event_id, o.*, true AS exact_date
    FROM event_keys e
    JOIN visible_observations o
      ON o.delist_date = e.delist_date
     AND (
            (o.security_id IS NOT NULL AND e.security_id IS NOT NULL AND o.security_id = e.security_id)
         OR (o.security_id IS NULL AND o.symbol IS NOT NULL AND o.symbol = e.symbol)
     )
    UNION ALL
    -- A row stored without that normalization still carries the vendor DLSTDT, i.e. the
    -- event's last observed trade date; it matches there, ranked after any exact-date match.
    -- A normalization-SHIFTED row on that date is a vendor last trade one session EARLIER
    -- than the archive's (its raw DLRET is not measured from the archive's last close) and
    -- never matches through this fallback.
    SELECT e.delisting_event_id, o.*, false AS exact_date
    FROM event_keys e
    JOIN visible_observations o
      ON o.delist_date = e.last_trade
     AND (
            (o.security_id IS NOT NULL AND e.security_id IS NOT NULL AND o.security_id = e.security_id)
         OR (o.security_id IS NULL AND o.symbol IS NOT NULL AND o.symbol = e.symbol)
     )
    WHERE o.vendor_last_trade = o.delist_date
),
observation_candidates AS (
    SELECT
        delisting_event_id,
        delisting_return_observation_id,
        source,
        provider,
        delisting_return,
        row_number() OVER (
            PARTITION BY delisting_event_id
            ORDER BY exact_date DESC,
                     available_at DESC, source_loaded_at DESC, delisting_return_observation_id DESC
        ) AS observation_rank
    FROM observation_matches
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
""".replace("__VENDOR_LAST_TRADE__", observation_vendor_last_trade_sql("o"))

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
