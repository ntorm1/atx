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
    from ..connection import DuckDBStore as _DuckDBStore

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
    from ..connection import DuckDBStore as _DuckDBStore

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
    from ..connection import DuckDBStore as _DuckDBStore

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
    from ..connection import DuckDBStore as _DuckDBStore

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
    from ..connection import DuckDBStore as _DuckDBStore

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
    from ..connection import DuckDBStore as _DuckDBStore

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
    from ..connection import DuckDBStore as _DuckDBStore

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
    from ..connection import DuckDBStore as _DuckDBStore

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
