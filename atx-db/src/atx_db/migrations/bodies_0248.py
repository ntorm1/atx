"""Govern point-in-time operating-cost inflexibility."""

from __future__ import annotations

import duckdb

from ._runner import Migration
from .bodies_0140_0143 import _refresh_schema_contract_v2_pin

SOURCE = "atx-db PIT operating cost inflexibility v1"
FACTOR_ID = "risk_operating_cost_inflexibility"
PARENT_FACTOR_ID = "investment_conservative_asset_growth"


def _operating_cost_inflexibility(conn: duckdb.DuckDBPyConnection) -> None:
    conn.execute(
        """
        INSERT OR REPLACE INTO factor_definition (
            factor_id,factor_name,family,description,expression,input_ids_json,
            direction,lookback_days,neutralization_spec_json,unit,sign,scale,
            is_point_in_time_safe,available_at_policy,declared_in,owner,source,
            standardization_spec_json,valid_from,valid_to
        ) VALUES (
            ?,'PIT operating cost inflexibility','fundamental_risk',
            'Negatively oriented five-year sample standard deviation of log annual COGS plus SG&A, following Taussig SDOC.',
            '-stddev_samp(ln(cogs+sga),five_annual_observations)',
            '["metric:cogs","metric:sga","factor:investment_conservative_asset_growth"]',
            1,2200,'{"method":"none","by":[]}','normalized_score',
            'higher_is_better','zscore',true,
            'Five exact-accession positive annual operating-cost observations spanning 1300-1650 days must be visible; the newest may be at most 550 days old; no cost is imputed.',
            'atx_db.operating_cost_inflexibility','atx-db',?,
            '{"method":"winsorize_then_zscore_cs","winsor_limits":[0.01,0.01],"observations":5,"annual_duration_days":[330,400],"five_year_span_days":[1300,1650],"maximum_reporting_age_days":550,"maximum_history_days":2200,"maximum_sdoc":5.0,"missing_components_imputed":false,"return_fitted_parameters":false}',
            DATE '1900-01-01',NULL
        )
        """,
        [FACTOR_ID,SOURCE],
    )
    conn.execute("DELETE FROM factor_dependency_edges WHERE factor_id=?",[FACTOR_ID])
    dependencies = (
        ("factor",PARENT_FACTOR_ID,PARENT_FACTOR_ID,None),
        ("metric","cogs",None,"cogs"),("metric","sga",None,"sga"),
    )
    for dependency_type,name,dependency_factor_id,dependency_metric_id in dependencies:
        conn.execute(
            """INSERT INTO factor_dependency_edges (
                dependency_id,factor_id,dependency_type,dependency_name,
                dependency_factor_id,dependency_metric_id,dependency_source_id,
                dependency_depth,expression,lookback_days,is_direct,source
            ) VALUES (sha256(concat_ws('|','operating_cost_inflexibility',?,?)),?,?,?, ?,?,NULL,
                      1,?,2200,true,?)""",
            [FACTOR_ID,name,FACTOR_ID,dependency_type,name,dependency_factor_id,
             dependency_metric_id,f"{dependency_type}:{name}",SOURCE],
        )
    _refresh_schema_contract_v2_pin(conn)


MIGRATIONS = [
    Migration(version=248,name="operating_cost_inflexibility_factor",up=_operating_cost_inflexibility)
]
