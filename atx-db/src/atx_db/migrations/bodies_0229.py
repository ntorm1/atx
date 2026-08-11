"""Govern quarterly inventory-change and inventory-growth factors."""

from __future__ import annotations

import duckdb

from ._runner import Migration
from .bodies_0140_0143 import _refresh_schema_contract_v2_pin

SOURCE = "atx-db PIT quarterly inventory investment v1"
CASH_FACTOR_ID = "profitability_quarterly_cash_operating_profitability_lagged_assets"
INVENTORY_CHANGE_FACTOR_ID = "investment_low_quarterly_inventory_change"
INVENTORY_GROWTH_FACTOR_ID = "investment_low_quarterly_inventory_growth"


def _quarterly_inventory_investment(conn: duckdb.DuckDBPyConnection) -> None:
    definitions = (
        (
            INVENTORY_CHANGE_FACTOR_ID,
            "PIT low quarterly inventory change",
            (
                "Negatively oriented quarterly inventory change scaled by average "
                "current and prior total assets."
            ),
            "-(inventory_t-inventory_t_1)/average(total_assets_t,total_assets_t_1)",
            5.0,
        ),
        (
            INVENTORY_GROWTH_FACTOR_ID,
            "PIT low quarterly inventory growth",
            "Negatively oriented quarter-over-quarter inventory growth rate.",
            "-(inventory_t/inventory_t_1-1)",
            10.0,
        ),
    )
    for factor_id, factor_name, description, expression, maximum in definitions:
        conn.execute(
            """
            INSERT OR REPLACE INTO factor_definition (
                factor_id,factor_name,family,description,expression,input_ids_json,
                direction,lookback_days,neutralization_spec_json,unit,sign,scale,
                is_point_in_time_safe,available_at_policy,declared_in,owner,source,
                standardization_spec_json,valid_from,valid_to
            ) VALUES (?,?,'fundamental_investment',?,?,
                '["factor:profitability_quarterly_cash_operating_profitability_lagged_assets","metric:total_assets"]',
                -1,330,'{"method":"none","by":[]}','normalized_score',
                'lower_is_better','zscore',true,
                'Uses the exact visible current/prior inventory pair from governed quarterly cash profitability and an exact-accession visible current-assets fact.',
                'atx_db.quarterly_inventory_investment','atx-db',?,
                ?,DATE '1900-01-01',NULL)
            """,
            [
                factor_id,
                factor_name,
                description,
                expression,
                SOURCE,
                (
                    '{"method":"negate_then_winsorize_then_zscore_cs",'
                    '"winsor_limits":[0.01,0.01],'
                    f'"maximum_absolute_raw_value":{maximum},'
                    '"exclude_no_inventory_in_either_period":true,'
                    '"return_fitted_parameters":false}'
                ),
            ],
        )
        conn.execute(
            "DELETE FROM factor_dependency_edges WHERE factor_id=?", [factor_id]
        )
        conn.execute(
            """
            INSERT INTO factor_dependency_edges (
                dependency_id,factor_id,dependency_type,dependency_name,
                dependency_factor_id,dependency_metric_id,dependency_source_id,
                dependency_depth,expression,lookback_days,is_direct,source
            ) VALUES
              (sha256(concat_ws('|','quarterly_inventory',?,'factor',?)),?,
               'factor',?,?,NULL,NULL,1,?,330,true,?),
              (sha256(concat_ws('|','quarterly_inventory',?,'metric','total_assets')),?,
               'metric','total_assets',NULL,'total_assets',NULL,1,
               'metric:total_assets',330,true,?)
            """,
            [
                factor_id,
                CASH_FACTOR_ID,
                factor_id,
                CASH_FACTOR_ID,
                CASH_FACTOR_ID,
                f"factor:{CASH_FACTOR_ID}",
                SOURCE,
                factor_id,
                factor_id,
                SOURCE,
            ],
        )
    _refresh_schema_contract_v2_pin(conn)


MIGRATIONS = [
    Migration(
        version=229,
        name="quarterly_inventory_investment",
        up=_quarterly_inventory_investment,
    )
]
