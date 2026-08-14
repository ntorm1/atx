"""Govern point-in-time organization capital."""

from __future__ import annotations

import duckdb

from ._runner import Migration
from .bodies_0140_0143 import _refresh_schema_contract_v2_pin

SOURCE = "atx-db PIT organization capital v1"
FACTOR_ID = "intangibles_high_organization_capital"
PARENT_FACTOR_ID = "investment_conservative_asset_growth"


def _organization_capital(conn: duckdb.DuckDBPyConnection) -> None:
    conn.execute(
        """
        INSERT OR REPLACE INTO factor_definition (
            factor_id,factor_name,family,description,expression,input_ids_json,
            direction,lookback_days,neutralization_spec_json,unit,sign,scale,
            is_point_in_time_safe,available_at_policy,declared_in,owner,source,
            standardization_spec_json,valid_from,valid_to
        ) VALUES (
            ?,'PIT organization capital to assets','fundamental_intangibles',
            'Eisfeldt-Papanikolaou CPI-adjusted perpetual-inventory stock of SG&A scaled by current real assets.',
            'OC_t=(1-0.15)*OC_t_1+SGA_t/CPI_t; OC_0=(SGA_1/CPI_1)/(0.10+0.15); score=OC_t/(assets_t/CPI_t)',
            '["metric:sga","metric:total_assets","macro:CPIAUCSL","factor:investment_conservative_asset_growth"]',
            1,8000,'{"method":"none","by":[]}','normalized_score',
            'higher_is_better','zscore',true,
            'At least five consecutive, exact-accession, visible annual SG&A and asset observations are required; SG&A is not zero-imputed; CPI is used only as a vintage deflator.',
            'atx_db.organization_capital','atx-db',?,
            '{"method":"winsorize_then_zscore_cs","winsor_limits":[0.01,0.01],"depreciation_rate":0.15,"initial_growth_rate":0.10,"minimum_history_observations":5,"annual_gap_days":[300,430],"maximum_reporting_age_days":550,"maximum_history_days":8000,"maximum_raw_value":20.0,"missing_sga_imputed_as_zero":false,"return_fitted_parameters":false}',
            DATE '1900-01-01',NULL
        )
        """,
        [FACTOR_ID,SOURCE],
    )
    conn.execute("DELETE FROM factor_dependency_edges WHERE factor_id=?",[FACTOR_ID])
    dependencies = (
        ("factor",PARENT_FACTOR_ID,PARENT_FACTOR_ID,None,None),
        ("metric","sga",None,"sga",None),
        ("metric","total_assets",None,"total_assets",None),
        ("source","CPIAUCSL",None,None,"CPIAUCSL"),
    )
    for dependency_type,name,factor_id,metric_id,source_id in dependencies:
        conn.execute(
            """INSERT INTO factor_dependency_edges (
                dependency_id,factor_id,dependency_type,dependency_name,
                dependency_factor_id,dependency_metric_id,dependency_source_id,
                dependency_depth,expression,lookback_days,is_direct,source
            ) VALUES (sha256(concat_ws('|','organization_capital',?,?)),?,?,?, ?,?,?,
                      1,?,8000,true,?)""",
            [FACTOR_ID,name,FACTOR_ID,dependency_type,name,factor_id,metric_id,source_id,
             f"{dependency_type}:{name}",SOURCE],
        )
    _refresh_schema_contract_v2_pin(conn)


MIGRATIONS = [Migration(version=249,name="organization_capital_factor",up=_organization_capital)]
