"""Point-in-time conservative annual asset-growth factor."""

from __future__ import annotations

import duckdb

from ._runner import Migration
from .bodies_0140_0143 import _refresh_schema_contract_v2_pin

SOURCE = "atx-db PIT conservative asset growth v1"
FACTOR_ID = "investment_conservative_asset_growth"


def _conservative_asset_growth(conn: duckdb.DuckDBPyConnection) -> None:
    conn.execute(
        """
        INSERT OR REPLACE INTO factor_definition (
            factor_id,factor_name,family,description,expression,input_ids_json,
            direction,lookback_days,neutralization_spec_json,unit,sign,scale,
            is_point_in_time_safe,available_at_policy,declared_in,owner,source,
            standardization_spec_json,valid_from,valid_to
        ) VALUES (
            ?,'PIT conservative annual asset growth','fundamental_investment',
            'Negative one-year percentage growth in reported total assets. Higher values identify conservative investment, following the Cooper-Gulen-Schill and Fama-French investment constructions.',
            '-((total_assets_t-total_assets_t_minus_1)/total_assets_t_minus_1)',
            '["metric:total_assets","universe:us_common_equity_liquid_v1"]',
            1,550,'{"method":"none","by":[]}','normalized_score',
            'higher_is_better','zscore',true,
            'Current and prior annual total assets must both be visible at the governed monthly close; fiscal periods must be 300-430 days apart and the current observation no more than 550 days old.',
            'atx_db.asset_growth','atx-db',?,
            '{"method":"winsorize_then_zscore_cs","winsor_limits":[0.01,0.01],"annual_period_gap_days":[300,430],"return_fitted_parameters":false}',
            DATE '1900-01-01',NULL
        )
        """,
        [FACTOR_ID, SOURCE],
    )
    conn.execute("DELETE FROM factor_dependency_edges WHERE factor_id=?", [FACTOR_ID])
    conn.execute(
        """
        INSERT INTO factor_dependency_edges (
            dependency_id,factor_id,dependency_type,dependency_name,
            dependency_factor_id,dependency_metric_id,dependency_source_id,
            dependency_depth,expression,lookback_days,is_direct,source
        ) VALUES (sha256(concat_ws('|','asset_growth',?,'total_assets')),?,
                  'metric','total_assets',NULL,'total_assets',NULL,1,
                  'metric:total_assets',550,true,?)
        """,
        [FACTOR_ID, FACTOR_ID, SOURCE],
    )
    _refresh_schema_contract_v2_pin(conn)


MIGRATIONS = [
    Migration(
        version=214,
        name="conservative_asset_growth",
        up=_conservative_asset_growth,
    )
]
