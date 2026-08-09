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
    from ..connection import DuckDBStore as _DuckDBStore

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
    from ..connection import DuckDBStore as _DuckDBStore

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
