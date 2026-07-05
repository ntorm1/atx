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


WAREHOUSE_CATALOG_ASOF_SQL = """
WITH params AS (
    SELECT
        CAST(? AS DATE) AS as_of_date,
        CAST(? AS TIMESTAMP) AS as_of_ts
),
visible_tables AS (
    SELECT DISTINCT
        v.table_name,
        v.layer,
        v.entity,
        v.grain,
        v.table_description,
        v.natural_key_json,
        v.pit_notes,
        v.table_updated_at
    FROM v_warehouse_catalog v
    CROSS JOIN params p
    WHERE v.table_updated_at <= p.as_of_ts
),
visible_fields AS (
    SELECT
        v.table_name,
        v.field_name,
        v.semantic_type,
        v.field_description,
        v.field_nullable,
        v.field_unit,
        v.source_field,
        v.field_updated_at,
        CASE
            WHEN v.formula_valid_from <= p.as_of_date
             AND coalesce(v.formula_valid_to, DATE '9999-12-31') > p.as_of_date
            THEN v.formula_code
            ELSE NULL
        END AS formula_code,
        CASE
            WHEN v.formula_valid_from <= p.as_of_date
             AND coalesce(v.formula_valid_to, DATE '9999-12-31') > p.as_of_date
            THEN v.formula_family
            ELSE NULL
        END AS formula_family,
        CASE
            WHEN v.formula_valid_from <= p.as_of_date
             AND coalesce(v.formula_valid_to, DATE '9999-12-31') > p.as_of_date
            THEN v.formula_kind
            ELSE NULL
        END AS formula_kind,
        CASE
            WHEN v.formula_valid_from <= p.as_of_date
             AND coalesce(v.formula_valid_to, DATE '9999-12-31') > p.as_of_date
            THEN v.formula_unit
            ELSE NULL
        END AS formula_unit,
        CASE
            WHEN v.formula_valid_from <= p.as_of_date
             AND coalesce(v.formula_valid_to, DATE '9999-12-31') > p.as_of_date
            THEN v.formula_expression
            ELSE NULL
        END AS formula_expression,
        CASE
            WHEN v.formula_valid_from <= p.as_of_date
             AND coalesce(v.formula_valid_to, DATE '9999-12-31') > p.as_of_date
            THEN v.formula_citation
            ELSE NULL
        END AS formula_citation,
        CASE
            WHEN v.formula_valid_from <= p.as_of_date
             AND coalesce(v.formula_valid_to, DATE '9999-12-31') > p.as_of_date
            THEN v.formula_valid_from
            ELSE NULL
        END AS formula_valid_from,
        CASE
            WHEN v.formula_valid_from <= p.as_of_date
             AND coalesce(v.formula_valid_to, DATE '9999-12-31') > p.as_of_date
            THEN v.formula_valid_to
            ELSE NULL
        END AS formula_valid_to
    FROM v_warehouse_catalog v
    CROSS JOIN params p
    WHERE v.field_updated_at <= p.as_of_ts
)
SELECT
    t.table_name,
    t.layer,
    t.entity,
    t.grain,
    t.table_description,
    t.natural_key_json,
    t.pit_notes,
    t.table_updated_at,
    f.field_name,
    f.semantic_type,
    f.field_description,
    f.field_nullable,
    f.field_unit,
    f.source_field,
    f.field_updated_at,
    f.formula_code,
    f.formula_family,
    f.formula_kind,
    f.formula_unit,
    f.formula_expression,
    f.formula_citation,
    f.formula_valid_from,
    f.formula_valid_to
FROM visible_tables t
LEFT JOIN visible_fields f ON f.table_name = t.table_name
{table_join}
{layer_join}
ORDER BY t.table_name, f.field_name
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
                    "JOIN asof_warehouse_catalog_table_filter tf ON tf.table_name = upper(t.table_name)"
                )
            if _register_filter(active, "asof_warehouse_catalog_layer_filter", "layer", layer_values):
                registered.append("asof_warehouse_catalog_layer_filter")
                layer_join = (
                    "JOIN asof_warehouse_catalog_layer_filter lf ON lf.layer = upper(t.layer)"
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
