"""Govern point-in-time Beneish manipulation-risk scores."""

from __future__ import annotations

import duckdb

from ._runner import Migration
from .bodies_0140_0143 import _refresh_schema_contract_v2_pin

SOURCE = "atx-db PIT low Beneish manipulation risk v1"
FACTOR_ID = "quality_low_beneish_m_score"
PARENT_FACTOR_ID = "investment_conservative_asset_growth"


def _beneish_m_score(conn: duckdb.DuckDBPyConnection) -> None:
    conn.execute(
        """
        INSERT OR REPLACE INTO factor_definition (
            factor_id,factor_name,family,description,expression,input_ids_json,
            direction,lookback_days,neutralization_spec_json,unit,sign,scale,
            is_point_in_time_safe,available_at_policy,declared_in,owner,source,
            standardization_spec_json,valid_from,valid_to
        ) VALUES (
            ?,'PIT low Beneish manipulation risk','fundamental_quality',
            'Negative Beneish 1999 eight-variable M-score; higher values identify lower earnings-manipulation risk.',
            '-(-4.84+0.920*DSRI+0.528*GMI+0.404*AQI+0.892*SGI+0.115*DEPI-0.172*SGAI+4.679*TATA-0.327*LVGI)',
            '["metric:revenue","metric:ar","metric:cogs","metric:total_assets","metric:current_assets","metric:ppe_net","metric:da_cf","metric:da_is","metric:depreciation","metric:sga","metric:total_liabilities","metric:net_income","metric:operating_cash_flow","factor:investment_conservative_asset_growth"]',
            1,1000,'{"method":"none","by":[]}','score',
            'higher_is_better','zscore',true,
            'Two consecutive exact-accession annual statements must be visible; all Beneish inputs are required and no value is imputed.',
            'atx_db.beneish_m_score','atx-db',?,
            '{"method":"winsorize_then_zscore_cs","winsor_limits":[0.01,0.01],"annual_gap_days":[300,430],"maximum_reporting_age_days":550,"maximum_history_days":1000,"maximum_absolute_m_score":100.0,"classic_manipulator_threshold":-2.22,"missing_components_imputed":false,"return_fitted_parameters":false}',
            DATE '1900-01-01',NULL
        )
        """,
        [FACTOR_ID, SOURCE],
    )
    conn.execute("DELETE FROM factor_dependency_edges WHERE factor_id=?", [FACTOR_ID])
    metric_names = (
        "revenue",
        "ar",
        "cogs",
        "total_assets",
        "current_assets",
        "ppe_net",
        "da_cf",
        "da_is",
        "depreciation",
        "sga",
        "total_liabilities",
        "net_income",
        "operating_cash_flow",
    )
    dependencies = [("factor", PARENT_FACTOR_ID, PARENT_FACTOR_ID, None)]
    dependencies.extend(("metric", name, None, name) for name in metric_names)
    for dependency_type, name, dependency_factor_id, dependency_metric_id in dependencies:
        conn.execute(
            """INSERT INTO factor_dependency_edges (
                dependency_id,factor_id,dependency_type,dependency_name,
                dependency_factor_id,dependency_metric_id,dependency_source_id,
                dependency_depth,expression,lookback_days,is_direct,source
            ) VALUES (sha256(concat_ws('|','beneish_m_score',?,?)),?,?,?, ?,?,NULL,
                      1,?,1000,true,?)""",
            [
                FACTOR_ID,
                name,
                FACTOR_ID,
                dependency_type,
                name,
                dependency_factor_id,
                dependency_metric_id,
                f"{dependency_type}:{name}",
                SOURCE,
            ],
        )
    _refresh_schema_contract_v2_pin(conn)


MIGRATIONS = [
    Migration(version=252, name="low_beneish_m_score_factor", up=_beneish_m_score)
]
