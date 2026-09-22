"""Activate production panel schedules and correct forward-return provenance."""

from __future__ import annotations

import json

import duckdb

from ._runner import Migration
from .bodies_0140_0143 import _refresh_schema_contract_v2_pin


def _production_panel_metadata(conn: duckdb.DuckDBPyConnection) -> None:
    from ..production_panels import PRODUCTION_JOB_SEEDS

    description = (
        "Forward returns over observed calendar endpoints using corrected same-row adjusted_close "
        "(explicit raw-close compatibility mode remains available), geometrically combined with "
        "observed or explicitly named-policy delisting terminals. Shumway performance policy defaults "
        "to -0.55 for historical NASDAQ and -0.30 otherwise; observed returns win. Missing adjusted "
        "inputs do not fall back to raw price. Source economic adjustments and historical publication "
        "vintages require independent validation."
    )
    conn.execute("UPDATE dataset_catalog SET description=?,updated_at=now() "
                 "WHERE dataset_id='forward_returns_survivorship_safe'", [description])
    conn.execute("UPDATE table_catalog SET description=?,updated_at=now() "
                 "WHERE table_name='forward_returns_survivorship_safe'", [description])
    conn.execute("UPDATE field_catalog SET description=?,source_field='equity_daily_bars.adjusted_close',updated_at=now() "
                 "WHERE table_name='forward_returns_survivorship_safe' AND field_name='raw_forward_return'",
                 ["Pre-terminal or surviving adjusted-price return; raw denotes the leg before terminal stitching, "
                  "not the price basis. Default input is corrected adjusted_close; raw close is explicit compatibility."])
    conn.execute("UPDATE dataset_catalog SET name='Observed or policy delisting terminal returns',description=?,updated_at=now() "
                 "WHERE dataset_id='delisting_terminal_returns'",
                 ["Observed returns take precedence; eligible performance delistings use the explicit default "
                  "Shumway exchange policy. Unresolved reasons remain uncovered; policy lineage is retained."])
    for dataset, name, table, grain, detail in (
        ("derived_factor_projection", "Legacy-compatible monthly factor projection", "fundamental_factor_values",
         "source,factor_id,security_id,as_of_date", "23 retired factor mappings using the actual governed legacy liquidity cohort and retained parents."),
        ("fundamental_item_coverage", "Annual top-3000 standardized-item coverage", "fundamental_item_coverage",
         "universe_id,item_code,period_basis,fiscal_year", "Measured registered-item coverage over independently persisted annual PIT top-3000 US common-stock cohorts."),
    ):
        conn.execute("INSERT INTO dataset_catalog (dataset_id,source_system_id,name,description,grain,primary_table," 
                     "pit_column,available_at_column,updated_at) SELECT ?,'atx_warehouse',?,?,?,?,NULL,NULL,now() "
                     "WHERE NOT EXISTS (SELECT 1 FROM dataset_catalog WHERE dataset_id=?)",
                     [dataset, name, detail, grain, table, dataset])
    for job, dataset, dependencies in PRODUCTION_JOB_SEEDS:
        conn.execute("INSERT INTO etl_job_definitions (job_name,dataset_id,params_json,dependencies_json) "
                     "SELECT ?,?,'{}',? WHERE NOT EXISTS (SELECT 1 FROM etl_job_definitions WHERE job_name=?)",
                     [job, dataset, json.dumps(dependencies), job])
    _refresh_schema_contract_v2_pin(conn)


MIGRATIONS = [Migration(version=312, name="production_panel_activation", up=_production_panel_metadata)]
