"""Govern PIT Novy-Marx operating-leverage factor."""

from __future__ import annotations

import duckdb

from ._runner import Migration
from .bodies_0140_0143 import _refresh_schema_contract_v2_pin

SOURCE = "atx-db PIT operating leverage v1"
FACTOR_ID = "risk_operating_leverage"
PARENT_FACTOR_ID = "profitability_operating_profitability"


def _operating_leverage(conn: duckdb.DuckDBPyConnection) -> None:
    conn.execute(
        """
        INSERT OR REPLACE INTO factor_definition (
            factor_id,factor_name,family,description,expression,input_ids_json,
            direction,lookback_days,neutralization_spec_json,unit,sign,scale,
            is_point_in_time_safe,available_at_policy,declared_in,owner,source,
            standardization_spec_json,valid_from,valid_to
        ) VALUES (
            ?,'PIT operating leverage','fundamental_risk',
            'Novy-Marx annual operating costs (COGS plus SG&A) divided by same-filing total assets, inherited from an exact governed PIT parent decision.',
            'zscore(winsorize_1pct((coalesce(cogs,revenue-gross_profit)+sga)/total_assets))',
            '["factor:profitability_operating_profitability"]',
            1,550,'{"method":"none","by":[]}','normalized_score',
            'higher_is_better','zscore',true,
            'All operating-cost and asset inputs are exact values already visible in the governed parent factor lineage; absent expense is never zero-imputed.',
            'atx_db.operating_leverage','atx-db',?,
            '{"method":"winsorize_then_zscore_cs","winsor_limits":[0.01,0.01],"maximum_raw_value":10.0,"minimum_names_per_date":20,"cogs_fallback":"revenue_minus_gross_profit","missing_expense_imputed":false,"return_fitted_parameters":false}',
            DATE '1900-01-01',NULL
        )
        """,
        [FACTOR_ID,SOURCE],
    )
    conn.execute("DELETE FROM factor_dependency_edges WHERE factor_id=?",[FACTOR_ID])
    conn.execute(
        """
        INSERT INTO factor_dependency_edges (
            dependency_id,factor_id,dependency_type,dependency_name,
            dependency_factor_id,dependency_metric_id,dependency_source_id,
            dependency_depth,expression,lookback_days,is_direct,source
        ) VALUES (
            sha256(concat_ws('|','operating_leverage',?,'factor',?)),
            ?,'factor',?,?,NULL,NULL,1,'factor:profitability_operating_profitability',
            550,true,?
        )
        """,
        [FACTOR_ID,PARENT_FACTOR_ID,FACTOR_ID,PARENT_FACTOR_ID,PARENT_FACTOR_ID,SOURCE],
    )
    _refresh_schema_contract_v2_pin(conn)


MIGRATIONS = [
    Migration(version=241,name="operating_leverage_factor_definition",up=_operating_leverage)
]
