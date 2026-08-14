"""Govern point-in-time low RSST total accruals."""

from __future__ import annotations

import duckdb

from ._runner import Migration
from .bodies_0140_0143 import _refresh_schema_contract_v2_pin

SOURCE = "atx-db PIT low RSST accruals v1"
FACTOR_ID = "quality_low_rsst_accruals"
PARENT_FACTOR_ID = "quality_net_operating_assets"


def _rsst_accruals(conn: duckdb.DuckDBPyConnection) -> None:
    conn.execute(
        """
        INSERT OR REPLACE INTO factor_definition (
            factor_id,factor_name,family,description,expression,input_ids_json,
            direction,lookback_days,neutralization_spec_json,unit,sign,scale,
            is_point_in_time_safe,available_at_policy,declared_in,owner,source,
            standardization_spec_json,valid_from,valid_to
        ) VALUES (
            ?,'PIT low RSST total accruals','fundamental_quality',
            'Negative annual change in comprehensive net operating assets scaled by average total assets, following Richardson-Sloan-Soliman-Tuna.',
            '-delta(total_assets-cash_st_inv-total_liabilities+st_debt+lt_debt)/average_total_assets',
            '["metric:total_assets","metric:total_liabilities","metric:cash_st_inv","metric:st_debt","metric:lt_debt","factor:quality_net_operating_assets"]',
            1,1000,'{"method":"none","by":[]}','ratio',
            'higher_is_better','zscore',true,
            'Two consecutive exact-accession annual balance sheets must be visible at formation; all five components are required and no value is imputed.',
            'atx_db.rsst_accruals','atx-db',?,
            '{"method":"winsorize_then_zscore_cs","winsor_limits":[0.01,0.01],"annual_gap_days":[300,430],"maximum_reporting_age_days":550,"maximum_history_days":1000,"maximum_absolute_accrual":5.0,"missing_components_imputed":false,"return_fitted_parameters":false}',
            DATE '1900-01-01',NULL
        )
        """,
        [FACTOR_ID, SOURCE],
    )
    conn.execute("DELETE FROM factor_dependency_edges WHERE factor_id=?", [FACTOR_ID])
    dependencies = (
        ("factor", PARENT_FACTOR_ID, PARENT_FACTOR_ID, None),
        ("metric", "total_assets", None, "total_assets"),
        ("metric", "total_liabilities", None, "total_liabilities"),
        ("metric", "cash_st_inv", None, "cash_st_inv"),
        ("metric", "st_debt", None, "st_debt"),
        ("metric", "lt_debt", None, "lt_debt"),
    )
    for dependency_type, name, dependency_factor_id, dependency_metric_id in dependencies:
        conn.execute(
            """INSERT INTO factor_dependency_edges (
                dependency_id,factor_id,dependency_type,dependency_name,
                dependency_factor_id,dependency_metric_id,dependency_source_id,
                dependency_depth,expression,lookback_days,is_direct,source
            ) VALUES (sha256(concat_ws('|','rsst_accruals',?,?)),?,?,?, ?,?,NULL,
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
    Migration(version=251, name="low_rsst_total_accruals_factor", up=_rsst_accruals)
]
