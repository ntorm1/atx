"""Govern point-in-time within-year inventory volatility."""

from __future__ import annotations

import duckdb

from ._runner import Migration
from .bodies_0140_0143 import _refresh_schema_contract_v2_pin

SOURCE = "atx-db PIT within-year inventory volatility v1"
FACTOR_ID = "operations_high_inventory_volatility"
PARENT_FACTOR_ID = "investment_conservative_asset_growth"


def _inventory_volatility(conn: duckdb.DuckDBPyConnection) -> None:
    conn.execute(
        """
        INSERT OR REPLACE INTO factor_definition (
            factor_id,factor_name,family,description,expression,input_ids_json,
            direction,lookback_days,neutralization_spec_json,unit,sign,scale,
            is_point_in_time_safe,available_at_policy,declared_in,owner,source,
            standardization_spec_json,valid_from,valid_to
        ) VALUES (
            ?,'PIT high within-year inventory volatility','fundamental_operations',
            'Coefficient of variation of the latest four positive quarterly inventory levels, following Steinker-Hoberg.',
            'stddev_samp(inventory_q0..q3)/mean(inventory_q0..q3)',
            '["metric:inventory","factor:investment_conservative_asset_growth"]',
            1,500,'{"method":"none","by":[]}','normalized_score',
            'higher_is_better','zscore',true,
            'Four distinct point-in-time-visible quarterly inventory levels spanning 240-310 days are required; the latest must be no older than 200 days; no value is imputed.',
            'atx_db.inventory_volatility','atx-db',?,
            '{"method":"winsorize_then_zscore_cs","winsor_limits":[0.01,0.01],"observations":4,"four_quarter_span_days":[240,310],"maximum_reporting_age_days":200,"maximum_history_days":500,"maximum_raw_value":5.0,"positive_inventory_required":true,"missing_components_imputed":false,"return_fitted_parameters":false}',
            DATE '1900-01-01',NULL
        )
        """,
        [FACTOR_ID,SOURCE],
    )
    conn.execute("DELETE FROM factor_dependency_edges WHERE factor_id=?",[FACTOR_ID])
    dependencies = (
        ("factor",PARENT_FACTOR_ID,PARENT_FACTOR_ID,None),
        ("metric","inventory",None,"inventory"),
    )
    for dependency_type,name,dependency_factor_id,dependency_metric_id in dependencies:
        conn.execute(
            """INSERT INTO factor_dependency_edges (
                dependency_id,factor_id,dependency_type,dependency_name,
                dependency_factor_id,dependency_metric_id,dependency_source_id,
                dependency_depth,expression,lookback_days,is_direct,source
            ) VALUES (sha256(concat_ws('|','inventory_volatility',?,?)),?,?,?, ?,?,NULL,
                      1,?,500,true,?)""",
            [FACTOR_ID,name,FACTOR_ID,dependency_type,name,dependency_factor_id,
             dependency_metric_id,f"{dependency_type}:{name}",SOURCE],
        )
    _refresh_schema_contract_v2_pin(conn)


MIGRATIONS = [Migration(version=247,name="inventory_volatility_factor",up=_inventory_volatility)]
