"""Whole-security, ledgered fundamentals stages using the shared legacy queries.

Mapping/routing prerequisites are explicit INSERT-only outputs of the statement
stage. No loader, migration or legacy seed writer is invoked from a bucket.
"""
from __future__ import annotations

import datetime as dt
from dataclasses import astuple, fields
from types import SimpleNamespace

from ._bulk_publication import _identifier
from ._fundamental_publication import BucketedTable, bucketed_publication_stage, security_bucket_sql
from .batch_runner import register_stage, store_for

DEPENDENCIES = (
    "fundamental_publication_stages.py", "industry_templates.py", "seeds/statement_map.csv",
    "_fundamental_publication.py", "fundamentals.py", "fundamental_statements.py",
    "calendarization.py", "shares_outstanding.py", "xbrl_catalog.py", "reported_eps_core.py",
    "_fundamental_clock.py", "_bulk_publication.py", "_standardization_set_based.py",
    "statement_map_seed.py", "clock.py",
)


def _lit(value):
    return "'" + str(value).replace("'", "''") + "'"


def _scope(context, column="security_id"):
    return security_bucket_sql(context.bucket, context.buckets, column)


def _seeds():
    from .industry_templates import INDUSTRY_TEMPLATES, TEMPLATE_ITEMS
    from .statement_map_seed import default_statement_map_rows

    definitions = {}
    for name, values, keys in (
        ("fundamental_statement_map", default_statement_map_rows(), ("source", "taxonomy", "concept", "industry_template")),
        ("industry_template", INDUSTRY_TEMPLATES, ("template_code",)),
        ("industry_template_item", TEMPLATE_ITEMS, ("template_code", "item_id")),
    ):
        columns = tuple(field.name for field in fields(values[0]))
        definitions[name] = (columns, [astuple(value) for value in values], keys)
    return definitions


def _seed_carry():
    carry = {}
    for name, (columns, rows, keys) in _seeds().items():
        values = ",".join("(" + ",".join(_lit(row[columns.index(key)]) for key in keys) + ")" for row in rows)
        joined = " AND ".join(f"old.{key}=seed.{key}" for key in keys)
        carry[name] = f"SELECT old.* FROM {name} old WHERE NOT EXISTS (SELECT 1 FROM (VALUES {values}) seed({','.join(keys)}) WHERE {joined})"
        if name == "fundamental_statement_map":
            carry[name] += " AND (source='SEC companyfacts' AND industry_template='ALL' AND item_id BETWEEN 1428 AND 1440) IS NOT TRUE"
    return carry


def _seed_prerequisites(context):
    for name, (columns, rows, _) in _seeds().items():
        _arrow_insert(context, name + "__next", columns, rows)


def _routes(context):
    from .industry_templates import SOURCE_NAME, entity_industry_templates_sql

    context.insert(entity_industry_templates_sql(
        target="entity_industry_template__next", security_predicate=_scope(context),
        calculation_date_sql="DATE " + _lit(context.state["calculation_date"]),
    ), [SOURCE_NAME, SOURCE_NAME, SOURCE_NAME, None])


def _revisions(context):
    from .fundamentals import fundamental_fact_revisions_sql

    context.insert(fundamental_fact_revisions_sql(
        target="fundamental_fact_revisions__next", security_predicate=_scope(context, "f.security_id"),
    ))


def _statements(context):
    from .fundamental_statements import fundamental_statement_points_sql, derived_reit_statement_points_sql

    context.insert(fundamental_statement_points_sql(
        target="fundamental_statement_points__next", revisions="fundamental_fact_revisions__next",
        security_predicate=_scope(context, "r.security_id"),
        statement_map="fundamental_statement_map__next", routes="entity_industry_template__next",
        calculation_date_sql="DATE " + _lit(context.state["calculation_date"]),
    ))
    context.insert(derived_reit_statement_points_sql(
        table="fundamental_statement_points__next", security_predicate=_scope(context, "p.security_id"),
        routes="entity_industry_template__next", calculation_date_sql="DATE " + _lit(context.state["calculation_date"]),
    ))


def _shares(context):
    from .shares_outstanding import SOURCE_NAME, shares_outstanding_history_sql

    context.insert(shares_outstanding_history_sql(
        target="shares_outstanding_history__next", points="fundamental_statement_points__next",
        security_predicate=_scope(context, "p.security_id"),
    ), [SOURCE_NAME, SOURCE_NAME, context.state["request"].get("shares_run_id")])


def _arrow_insert(context, target, columns, rows):
    """Arrow is only a bounded input view; the complete durable row is receipted."""
    import pyarrow as pa

    rows = list(rows)
    if not rows:
        return
    # Derive Arrow types from the actual catalog, retaining nullable dates/ints.
    schema = context.conn.execute(
        "SELECT " + ",".join(_identifier(name) for name in columns) +
        f" FROM {_identifier(target)} WHERE false"
    ).fetch_arrow_table().schema
    table = pa.Table.from_pylist([dict(zip(columns, row, strict=True)) for row in rows], schema=schema)
    context.conn.register("_fund_bucket_arrow", table)
    try:
        names = ",".join(_identifier(name) for name in columns)
        context.insert(f"INSERT INTO {_identifier(target)} ({names}) SELECT {names} FROM _fund_bucket_arrow")
    finally:
        context.conn.unregister("_fund_bucket_arrow")


def _catalog(context):
    from .xbrl_catalog import _COLUMNS, concept_catalog_aggregate_sql, concept_catalog_rows

    predicate = f"hash(source,taxonomy,concept)%{context.buckets}={context.bucket}"
    cursor = context.conn.cursor()
    try:
        cursor.execute(concept_catalog_aggregate_sql(concept_predicate=predicate))
        while batch := cursor.fetchmany(128):
            _arrow_insert(context, "xbrl_concept_catalog__next", _COLUMNS, concept_catalog_rows(batch))
    finally:
        cursor.close()


def _periods(context):
    from .fundamental_statements import _rdq_clock_inputs, fundamental_periods_sql, stream_period_lineage

    raw_clock, sessions, params = _rdq_clock_inputs(store_for(context.conn))
    lineage_table = stream_period_lineage(context)
    context.insert(fundamental_periods_sql(
        target="fundamental_periods__next", raw_clock=raw_clock, sessions_sql=sessions, lineage=True,
        security_predicate=_scope(context, "p.security_id"), lineage_table=lineage_table,
    ), params)


def _ttm(context):
    from .fundamental_statements import fundamental_ttm_points_sql

    context.insert(fundamental_ttm_points_sql(
        target="fundamental_ttm_points__next", security_predicate=_scope(context),
    ))


def _calendar_map(context):
    from . import calendarization as cal
    from ._standardization_set_based import (
        FISCAL_ANCHOR_EVIDENCE_TABLES, create_fiscal_calendar_labels,
        fiscal_anchor_candidates_sql, fiscal_year_end_form_filings_sql,
    )

    conn = context.conn
    context.view("_bounded_calendar_periods", "SELECT * FROM fundamental_periods WHERE " + _scope(context))
    evidence = tuple(name for name in FISCAL_ANCHOR_EVIDENCE_TABLES if name in context.state["catalogs"])
    if evidence:
        context.view("_calendar_fiscal_candidates", "SELECT * FROM (" +
                     fiscal_anchor_candidates_sql(evidence) + ") q WHERE " + _scope(context))
        annual = "SELECT * FROM (" + fiscal_year_end_form_filings_sql() + ") q WHERE " + _scope(context)
        create_fiscal_calendar_labels(
            SimpleNamespace(con=context.temporary_executor(cal._CALENDAR_FISCAL_TEMP_TABLES)),
            candidates="_calendar_fiscal_candidates", periods="_bounded_calendar_periods",
            prefix="_calendar_fiscal", annual_form_filings=annual,
        )
    context.materialize("_calendar_map_input", cal.calendar_map_input_sql(
        labeled=bool(evidence), periods="_bounded_calendar_periods",
    ))
    # Results are small, native row batches, never a full source pandas frame.
    count = conn.execute("SELECT count(*) FROM _calendar_map_input").fetchone()[0]
    for offset in range(0, count, 256):
        result = conn.execute("SELECT * EXCLUDE(_batch_row) FROM _calendar_map_input "
                              "WHERE _batch_row>? AND _batch_row<=? ORDER BY _batch_row", [offset, offset+256])
        names = [item[0] for item in result.description]
        records = cal.calendar_map_records([dict(zip(names, row, strict=True)) for row in result.fetchall()],
                                          run_id=context.state["request"].get("calendar_run_id"))
        _arrow_insert(context, "fundamental_calendar_map__next", cal.CALENDAR_MAP_COLUMNS,
                      [tuple(row[name] for name in cal.CALENDAR_MAP_COLUMNS) for row in records])


def _calendar_ttm(context):
    from .calendarization import SOURCE_NAME, fundamental_calendar_ttm_sql

    context.insert(fundamental_calendar_ttm_sql(
        target="fundamental_calendar_ttm__next", calendar_map="fundamental_calendar_map__next",
        security_predicate=_scope(context),
    ), [SOURCE_NAME, SOURCE_NAME, SOURCE_NAME, context.state["request"].get("calendar_run_id")])


def _coverage(context):
    from .calendarization import SOURCE_NAME, STITCHED_TTM_METHOD, calendarization_coverage_sql

    date = context.state["request"].get("as_of_date")
    if date is None:
        date = context.conn.execute(
            "SELECT CAST(greatest((SELECT max(available_at) FROM fundamental_calendar_map__next WHERE source=?),"
            "(SELECT max(available_at) FROM fundamental_calendar_ttm__next WHERE source=?)) AS DATE)",
            [SOURCE_NAME, SOURCE_NAME],
        ).fetchone()[0]
    if date is None:
        raise ValueError("calendarization coverage has no explicit or source-derived as-of date")
    date = dt.date.fromisoformat(str(date))
    clock = context.state["request"].get("diagnostic_coverage_loaded_at", context.state["created_at"])
    if "diagnostic_coverage_loaded_at" in context.state["request"] and context.state["request"].get("purpose") != "diagnostic_scratch":
        raise ValueError("coverage operational clock override requires diagnostic scratch")
    context.insert(calendarization_coverage_sql(
        target="calendarization_coverage__next", calendar_map="fundamental_calendar_map__next",
        calendar_ttm="fundamental_calendar_ttm__next", loaded_at_sql="TIMESTAMP " + _lit(clock),
    ), [SOURCE_NAME, SOURCE_NAME, STITCHED_TTM_METHOD, SOURCE_NAME, date, SOURCE_NAME,
        STITCHED_TTM_METHOD, date, dt.datetime.combine(date, dt.time(22)), context.state["request"].get("calendar_run_id")])


STATEMENTS = bucketed_publication_stage(
    stage_name="statement_points_rebuild", bucket_count=32,
    tables=(
        BucketedTable("fundamental_statement_map", ("source", "taxonomy", "concept", "industry_template"),
                      ("source", "taxonomy", "concept", "industry_template"), bucket_key="source,taxonomy,concept,industry_template"),
        BucketedTable("industry_template", ("template_code",), ("template_code",), bucket_key="template_code"),
        BucketedTable("industry_template_item", ("template_code", "item_id"), ("template_code", "item_id"),
                      bucket_key="template_code,item_id"),
        BucketedTable("entity_industry_template", ("security_id", "valid_from"), ("route_id",)),
        BucketedTable("fundamental_fact_revisions", ("security_id", "period_end"), ("fact_revision_id",)),
        BucketedTable("fundamental_statement_points", ("security_id", "period_end"), ("statement_point_id",)),
        BucketedTable("shares_outstanding_history", ("security_id", "period_end"), ("share_history_id",), fc1_date="as_of_date"),
        BucketedTable("xbrl_concept_catalog", ("source", "taxonomy", "concept"), ("source", "taxonomy", "concept"),
                      bucket_key="source,taxonomy,concept"),
    ),
    sources=("sec_company_facts", "fundamental_statement_map", "entity_industry_template", "securities", "entity_classification", "taxonomy",
             "press_release_facts", "sec_earnings_release_receipts"),
    bucket_sources=("sec_company_facts", "entity_classification", "securities"),
    initial_phases=(("seed_prerequisites", _seed_prerequisites),),
    phases=(("routes", _routes), ("revisions", _revisions), ("statements", _statements), ("shares", _shares), ("concept_catalog", _catalog)),
    carry_over_sql={
        **_seed_carry(),
        "entity_industry_template": "SELECT * FROM entity_industry_template WHERE source IS DISTINCT FROM 'derived_industry_templates_v1'",
        "shares_outstanding_history": "SELECT * FROM shares_outstanding_history WHERE source IS DISTINCT FROM 'SEC XBRL share counts'",
        "xbrl_concept_catalog": "SELECT c.* FROM xbrl_concept_catalog c WHERE NOT EXISTS (SELECT 1 FROM sec_company_facts f "
            "WHERE f.source=c.source AND f.taxonomy=c.taxonomy AND f.concept=c.concept "
            "AND f.taxonomy IS NOT NULL AND f.taxonomy<>'' AND f.concept IS NOT NULL AND f.concept<>'')",
    }, code_dependencies=DEPENDENCIES,
)

PERIODS = bucketed_publication_stage(
    stage_name="periods_rebuild", bucket_count=32,
    tables=(BucketedTable("fundamental_periods", ("security_id", "period_end"), ("fundamental_period_id",), fc1_date="as_of_date"),),
    sources=("fundamental_statement_points", "sec_submissions", "trading_calendar"),
    bucket_sources=("fundamental_statement_points",), phases=(("periods", _periods),), code_dependencies=DEPENDENCIES,
)

TTM = bucketed_publication_stage(
    stage_name="ttm_rebuild", bucket_count=32,
    tables=(BucketedTable("fundamental_ttm_points", ("security_id", "ttm_end_date"), ("ttm_point_id",), fc1_date="as_of_date"),),
    sources=("fundamental_statement_points",), bucket_sources=("fundamental_statement_points",),
    phases=(("ttm", _ttm),), code_dependencies=DEPENDENCIES,
)

CALENDAR = bucketed_publication_stage(
    stage_name="calendarization_rebuild", bucket_count=32,
    tables=(
        BucketedTable("fundamental_calendar_map", ("security_id", "period_end"), ("calendar_map_id",), fc1_date="as_of_date"),
        BucketedTable("fundamental_calendar_ttm", ("security_id", "ttm_end_date"), ("calendar_ttm_id",), fc1_date="as_of_date"),
        BucketedTable("calendarization_coverage", ("source",), ("coverage_id",), bucket_key="source"),
    ),
    sources=("fundamental_periods", "fundamental_statement_points", "fundamental_ttm_points", "fundamental_xbrl_metric"),
    bucket_sources=("fundamental_periods", "fundamental_statement_points", "fundamental_xbrl_metric"),
    phases=(("map", _calendar_map), ("calendar_ttm", _calendar_ttm)), final_phases=(("coverage", _coverage),),
    carry_over_sql={name: f"SELECT * FROM {name} WHERE source IS DISTINCT FROM 'derived_calendarization_v1'"
                    for name in ("fundamental_calendar_map", "fundamental_calendar_ttm", "calendarization_coverage")},
    code_dependencies=DEPENDENCIES,
)

for _stage in (STATEMENTS, PERIODS, TTM, CALENDAR):
    register_stage(_stage)
