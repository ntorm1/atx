"""Custom daily features and predeclared forward-return research evidence."""

from __future__ import annotations

import duckdb

from ._runner import Migration
from .bodies_0001_0137 import _catalog_fields_for_tables
from .bodies_0140_0143 import _refresh_schema_contract_v2_pin


def create_custom_feature_tables(conn: duckdb.DuckDBPyConnection) -> None:
    """DDL also usable by minimal fixtures; no source scans or auto-migrations."""
    conn.execute("""
        CREATE TABLE IF NOT EXISTS custom_feature_definitions (
            feature_version VARCHAR NOT NULL, feature_id VARCHAR NOT NULL,
            definition_json VARCHAR NOT NULL, PRIMARY KEY(feature_version,feature_id)
        );
        CREATE TABLE IF NOT EXISTS custom_feature_runs (
            run_id VARCHAR PRIMARY KEY, run_kind VARCHAR NOT NULL,
            feature_source VARCHAR NOT NULL, feature_version VARCHAR NOT NULL,
            price_source VARCHAR NOT NULL, source_sha256 VARCHAR NOT NULL,
            as_of_date DATE NOT NULL, run_at TIMESTAMP NOT NULL,
            configuration_json VARCHAR NOT NULL, diagnostics_json VARCHAR NOT NULL
        );
        -- Deliberately no giant ART index. A single scoped atomic writer produces
        -- unique (feature_source,feature_version,security_id,decision_date) keys.
        CREATE TABLE IF NOT EXISTS custom_features_daily (
            feature_source VARCHAR NOT NULL, feature_version VARCHAR NOT NULL,
            security_id VARCHAR NOT NULL, decision_date DATE NOT NULL,
            decision_at TIMESTAMP NOT NULL, input_end_date DATE NOT NULL,
            input_available_at TIMESTAMP NOT NULL, entry_date DATE NOT NULL,
            session_number BIGINT NOT NULL, known_by_decision BOOLEAN NOT NULL,
            cohort_eligible BOOLEAN NOT NULL, raw_close DOUBLE,
            dollar_volume_mean_63 DOUBLE, valid_dollar_volume_63 INTEGER,
            valid_adjusted_close_127 INTEGER, valid_returns_63 INTEGER,
            reversal_5 DOUBLE, momentum_126_skip_21 DOUBLE,
            volatility_scaled_momentum_63 DOUBLE, dollar_volume_shock DOUBLE,
            close_location_pressure_21 DOUBLE, range_compression DOUBLE,
            liquidity_conditioned_reversal DOUBLE, compression_accumulation DOUBLE,
            build_run_id VARCHAR NOT NULL
        );
        CREATE TABLE IF NOT EXISTS custom_feature_deciles (
            run_id VARCHAR NOT NULL, feature_id VARCHAR NOT NULL,
            horizon_sessions INTEGER NOT NULL, decision_date DATE NOT NULL,
            session_number BIGINT NOT NULL, entry_date DATE NOT NULL,
            expected_end_date DATE, split VARCHAR NOT NULL,
            status VARCHAR NOT NULL, decile INTEGER NOT NULL,
            cohort_count BIGINT NOT NULL, feature_eligible_count BIGINT NOT NULL,
            eligible_count BIGINT NOT NULL, labeled_count BIGINT NOT NULL,
            missing_count BIGINT NOT NULL, terminal_count BIGINT NOT NULL,
            imputed_count BIGINT NOT NULL, invalid_label_count BIGINT NOT NULL,
            mean_feature DOUBLE, mean_forward_return DOUBLE,
            label_available_at TIMESTAMP,
            PRIMARY KEY(run_id,feature_id,horizon_sessions,decision_date,decile)
        );
        CREATE TABLE IF NOT EXISTS custom_feature_evaluations (
            run_id VARCHAR NOT NULL, feature_id VARCHAR NOT NULL,
            horizon_sessions INTEGER NOT NULL, split VARCHAR NOT NULL,
            is_primary BOOLEAN NOT NULL, spread_dates INTEGER NOT NULL,
            gross_mean DOUBLE, hac_lags INTEGER NOT NULL, hac_standard_error DOUBLE,
            z_statistic DOUBLE, p_value DOUBLE, holm_p_value DOUBLE,
            ci95_low DOUBLE, ci95_high DOUBLE,
            net_10bp DOUBLE, net_25bp DOUBLE, net_50bp DOUBLE,
            eligible_count BIGINT NOT NULL, labeled_count BIGINT NOT NULL,
            label_coverage DOUBLE, terminal_count BIGINT NOT NULL, imputed_count BIGINT NOT NULL,
            annual_stability_json VARCHAR NOT NULL, statistically_qualified BOOLEAN NOT NULL,
            production_eligible BOOLEAN NOT NULL, blockers_json VARCHAR NOT NULL,
            PRIMARY KEY(run_id,feature_id,horizon_sessions,split)
        );
    """)


def _custom_feature_research(conn: duckdb.DuckDBPyConnection) -> None:
    create_custom_feature_tables(conn)
    descriptions = {
        "custom_feature_definitions": ("feature_version,feature_id", "Pinned formulas and fixed directions for all eight custom research hypotheses."),
        "custom_feature_runs": ("run_id", "Explicit build/evaluation manifests with exact source SHA256 and modeled timing limitations."),
        "custom_features_daily": ("feature_source,feature_version,security_id,decision_date", "Wide causal daily research features; previous-session inputs and next-session-close entry. Uncertified historical listing cohort."),
        "custom_feature_deciles": ("run_id,feature_id,horizon_sessions,decision_date,decile", "Deciles ranked before label attrition, with coverage, terminal counts and excluded-date diagnostics."),
        "custom_feature_evaluations": ("run_id,feature_id,horizon_sessions,split", "Chronological horizon-spread research with calendar-aware HAC and eight-test Holm correction; not portfolio PnL."),
    }
    for table, (grain, description) in descriptions.items():
        natural_key = '["' + grain.replace(',', '","') + '"]'
        conn.execute(
            "INSERT OR REPLACE INTO table_catalog "
            "(table_name,layer,entity,grain,description,natural_key_json,pit_notes,updated_at) "
            "VALUES (?,'research','custom_feature',?,?,?,?,now())",
            [table, grain, description, natural_key,
             "Explicit run timestamps and modeled backfill availability; no certified historical vendor vintage. "
             "Daily rows replace one source/version atomically; aggregate evaluation runs are retained."],
        )
        conn.execute(
            "INSERT OR REPLACE INTO pit_exemption "
            "(table_name,missing_columns,reason,exempted_by,exempted_at,source_loaded_at) "
            "VALUES (?, '[\"as_of_date\",\"available_at\",\"is_latest_revision\",\"source_loaded_at\"]',"
            "'Research snapshot linked to an explicit immutable run manifest; feature input clocks retained.',"
            "'tier1-cf1',now(),now())", [table],
        )
    _catalog_fields_for_tables(conn, tuple(descriptions))
    _refresh_schema_contract_v2_pin(conn)


MIGRATIONS = [Migration(version=313, name="custom_feature_research", up=_custom_feature_research)]
