"""Govern rolling q5 expected-growth model state and factor output."""

from __future__ import annotations

import duckdb

from ._runner import Migration
from .bodies_0001_0137 import _catalog_fields_for_tables
from .bodies_0140_0143 import _refresh_schema_contract_v2_pin

SOURCE = "atx-db PIT q5 rolling WLS expected growth v1"
FACTOR_ID = "investment_q5_expected_growth_rolling_wls"
MODEL_ID = "q5_expected_growth_wls_1y_v1"
DEPENDENCY_FACTORS = (
    "investment_conservative_asset_growth",
    "profitability_cash_operating_profitability",
    "profitability_q_factor_delta_roe",
)


def _rolling_expected_growth(conn: duckdb.DuckDBPyConnection) -> None:
    conn.execute(
        """
        CREATE TABLE IF NOT EXISTS expected_growth_model_slopes (
            slope_id VARCHAR PRIMARY KEY,
            model_id VARCHAR NOT NULL,
            source VARCHAR NOT NULL,
            as_of_date DATE NOT NULL,
            predictor_as_of_date DATE NOT NULL,
            available_at TIMESTAMP NOT NULL,
            intercept DOUBLE NOT NULL,
            log_tobins_q_slope DOUBLE NOT NULL,
            cash_operating_profitability_slope DOUBLE NOT NULL,
            delta_roe_slope DOUBLE NOT NULL,
            n_obs BIGINT NOT NULL,
            condition_number DOUBLE NOT NULL,
            weighted_r2 DOUBLE,
            training_sample_hash VARCHAR NOT NULL,
            input_lineage_json VARCHAR NOT NULL,
            is_latest_revision BOOLEAN NOT NULL DEFAULT true,
            run_id VARCHAR,
            source_loaded_at TIMESTAMP NOT NULL DEFAULT now(),
            updated_at TIMESTAMP NOT NULL DEFAULT now()
        )
        """
    )
    conn.execute(
        """
        CREATE INDEX IF NOT EXISTS idx_expected_growth_model_slopes_model_date
        ON expected_growth_model_slopes(model_id, as_of_date)
        """
    )
    conn.execute(
        """
        INSERT OR REPLACE INTO dataset_catalog (
            dataset_id,source_system_id,name,description,grain,primary_table,
            pit_column,available_at_column,updated_at
        ) VALUES (
            'expected_growth_model_slopes','atx_warehouse',
            'Point-in-time q5 expected-growth WLS slopes',
            'Monthly market-equity-weighted cross-sectional slopes of observable one-year investment-growth changes on predictors lagged twelve months.',
            'source,model_id,as_of_date','expected_growth_model_slopes',
            'as_of_date','available_at',now()
        )
        """
    )
    conn.execute(
        """
        INSERT OR REPLACE INTO table_catalog (
            table_name,layer,entity,grain,description,natural_key_json,pit_notes,updated_at
        ) VALUES (
            'expected_growth_model_slopes','gold','expected_growth_model_slope',
            'source,model_id,as_of_date',
            'Reconstructable monthly q5 WLS coefficient history used only after each target becomes observable.',
            '["source","model_id","as_of_date"]',
            'available_at is the latest visible target, current market-equity weight, or twelve-month-lagged predictor used by the monthly regression; forecasts use strictly prior slope dates.',
            now()
        )
        """
    )
    _catalog_fields_for_tables(conn, ("expected_growth_model_slopes",))
    conn.execute(
        """
        INSERT OR REPLACE INTO factor_definition (
            factor_id,factor_name,family,description,expression,input_ids_json,
            direction,lookback_days,neutralization_spec_json,unit,sign,scale,
            is_point_in_time_safe,available_at_policy,declared_in,owner,source,
            standardization_spec_json,valid_from,valid_to
        ) VALUES (
            ?,'PIT q5 rolling-WLS expected investment growth',
            'fundamental_expected_growth',
            'Point-in-time expected one-year investment growth using monthly market-equity-weighted accounting-target regressions and only the prior 120 eligible slope months.',
            'mean_prior_120m_wls_slopes * [1,ln(tobins_q),cash_operating_profitability,delta_roe]',
            '["factor:investment_conservative_asset_growth","factor:profitability_cash_operating_profitability","factor:profitability_q_factor_delta_roe","source:sec_company_facts","dataset:expected_growth_model_slopes"]',
            1,4200,'{"method":"none","by":[]}','normalized_score',
            'higher_is_better','zscore',true,
            'Annual inputs are 120-550 days old; training targets are observable at the slope date; regressors are twelve calendar months older; forecasts use only slope dates strictly before formation.',
            'atx_db.expected_growth_rolling','atx-db',?,
            '{"method":"rolling_market_cap_wls_then_zscore_cs","slope_window_months":120,"minimum_slope_months":30,"minimum_regression_names":100,"maximum_condition_number":1000,"winsor_limits":[0.01,0.01],"return_fitted_parameters":false}',
            DATE '1900-01-01',NULL
        )
        """,
        [FACTOR_ID, SOURCE],
    )
    conn.execute("DELETE FROM factor_dependency_edges WHERE factor_id=?", [FACTOR_ID])
    for dependency_factor_id in DEPENDENCY_FACTORS:
        conn.execute(
            """
            INSERT INTO factor_dependency_edges (
                dependency_id,factor_id,dependency_type,dependency_name,
                dependency_factor_id,dependency_metric_id,dependency_source_id,
                dependency_depth,expression,lookback_days,is_direct,source
            ) VALUES (sha256(concat_ws('|','q5_rolling_wls',?,?)),?,
                      'factor',?,?,NULL,NULL,1,?,4200,true,?)
            """,
            [
                FACTOR_ID,
                dependency_factor_id,
                FACTOR_ID,
                dependency_factor_id,
                dependency_factor_id,
                f"factor:{dependency_factor_id}",
                SOURCE,
            ],
        )
    for dependency_source in ("sec_company_facts", "expected_growth_model_slopes"):
        conn.execute(
            """
            INSERT INTO factor_dependency_edges (
                dependency_id,factor_id,dependency_type,dependency_name,
                dependency_factor_id,dependency_metric_id,dependency_source_id,
                dependency_depth,expression,lookback_days,is_direct,source
            ) VALUES (sha256(concat_ws('|','q5_rolling_wls',?,?)),?,
                      'source',?,NULL,NULL,?,1,?,4200,true,?)
            """,
            [
                FACTOR_ID,
                dependency_source,
                FACTOR_ID,
                dependency_source,
                dependency_source,
                f"source:{dependency_source}",
                SOURCE,
            ],
        )
    _refresh_schema_contract_v2_pin(conn)


MIGRATIONS = [
    Migration(version=220, name="q5_rolling_wls_expected_growth", up=_rolling_expected_growth)
]
