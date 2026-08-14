"""Govern point-in-time large R&D-increase events."""

from __future__ import annotations

import duckdb

from ._runner import Migration
from .bodies_0140_0143 import _refresh_schema_contract_v2_pin

SOURCE = "atx-db PIT large R&D increase v1"
FACTOR_ID = "intangibles_large_rd_increase"
PARENT_FACTOR_ID = "investment_conservative_asset_growth"


def _rd_increase(conn: duckdb.DuckDBPyConnection) -> None:
    conn.execute(
        """
        INSERT OR REPLACE INTO factor_definition (
            factor_id,factor_name,family,description,expression,input_ids_json,
            direction,lookback_days,neutralization_spec_json,unit,sign,scale,
            is_point_in_time_safe,available_at_policy,declared_in,owner,source,
            standardization_spec_json,valid_from,valid_to
        ) VALUES (
            ?,'PIT large R&D increase event','fundamental_intangibles',
            'Eberhart event requiring R&D intensities above 5% and greater-than-5% growth in R&D, R&D-to-sales, and R&D-to-average-assets.',
            'I(rd/sales>.05,rd/avg_assets>.05,growth(rd)>.05,growth(rd/sales)>.05,growth(rd/avg_assets)>.05)',
            '["metric:rd_expense","metric:revenue","metric:total_assets","factor:investment_conservative_asset_growth"]',
            1,1300,'{"method":"none","by":[]}','event_score',
            'higher_is_better','zscore',true,
            'Three consecutive exact-accession annual filings must be visible; positive R&D, revenue, and assets are required; no value is imputed.',
            'atx_db.rd_increase','atx-db',?,
            '{"method":"binary_then_zscore_cs","intensity_threshold":0.05,"growth_threshold":0.05,"annual_duration_days":[330,400],"annual_gap_days":[300,430],"maximum_reporting_age_days":550,"maximum_history_days":1300,"missing_components_imputed":false,"return_fitted_parameters":false}',
            DATE '1900-01-01',NULL
        )
        """,
        [FACTOR_ID,SOURCE],
    )
    conn.execute("DELETE FROM factor_dependency_edges WHERE factor_id=?",[FACTOR_ID])
    dependencies = (
        ("factor",PARENT_FACTOR_ID,PARENT_FACTOR_ID,None),
        ("metric","rd_expense",None,"rd_expense"),
        ("metric","revenue",None,"revenue"),
        ("metric","total_assets",None,"total_assets"),
    )
    for dependency_type,name,factor_id,metric_id in dependencies:
        conn.execute(
            """INSERT INTO factor_dependency_edges (
                dependency_id,factor_id,dependency_type,dependency_name,
                dependency_factor_id,dependency_metric_id,dependency_source_id,
                dependency_depth,expression,lookback_days,is_direct,source
            ) VALUES (sha256(concat_ws('|','rd_increase',?,?)),?,?,?, ?,?,NULL,
                      1,?,1300,true,?)""",
            [FACTOR_ID,name,FACTOR_ID,dependency_type,name,factor_id,metric_id,
             f"{dependency_type}:{name}",SOURCE],
        )
    _refresh_schema_contract_v2_pin(conn)


MIGRATIONS = [Migration(version=250,name="large_rd_increase_factor",up=_rd_increase)]
