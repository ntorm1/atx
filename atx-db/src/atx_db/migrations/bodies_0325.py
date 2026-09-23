"""FQ2 bounded fundamental signal evaluation and forward-label provenance."""

from __future__ import annotations

import duckdb

from ._runner import Migration
from .bodies_0001_0137 import _catalog_fields_for_tables
from .bodies_0140_0143 import _refresh_schema_contract_v2_pin


def create_fundamental_signal_evaluation_tables(conn: duckdb.DuckDBPyConnection) -> None:
    """Small-fixture entry point; requires the physical forward-return table."""
    conn.execute("""
        ALTER TABLE forward_returns_survivorship_safe
        ADD COLUMN IF NOT EXISTS price_basis VARCHAR;
        ALTER TABLE forward_returns_survivorship_safe
        ADD COLUMN IF NOT EXISTS calculation_version VARCHAR;
        CREATE TABLE IF NOT EXISTS fundamental_signal_evaluation_runs (
            run_id VARCHAR PRIMARY KEY,
            build_run_id VARCHAR NOT NULL,
            status VARCHAR NOT NULL,
            as_of_date DATE NOT NULL,
            run_at TIMESTAMP NOT NULL,
            label_source VARCHAR NOT NULL,
            config_json VARCHAR NOT NULL,
            config_sha256 VARCHAR NOT NULL,
            build_sha256 VARCHAR NOT NULL,
            calendar_sha256 VARCHAR NOT NULL,
            evaluation_calendar_sha256 VARCHAR,
            sample_sha256 VARCHAR,
            result_sha256 VARCHAR,
            label_rows BIGINT,
            diagnostic_json VARCHAR,
            blockers_json VARCHAR NOT NULL,
            production_eligible BOOLEAN NOT NULL DEFAULT false,
            source_loaded_at TIMESTAMP NOT NULL DEFAULT now()
        );
        CREATE TABLE IF NOT EXISTS fundamental_signal_evaluation_deciles (
            run_id VARCHAR NOT NULL,
            signal_id VARCHAR NOT NULL,
            decision_date DATE NOT NULL,
            session_number BIGINT NOT NULL,
            entry_date DATE,
            expected_end_date DATE,
            horizon_sessions INTEGER NOT NULL,
            split VARCHAR NOT NULL,
            status VARCHAR NOT NULL,
            decile INTEGER NOT NULL,
            cohort_count BIGINT NOT NULL,
            tied_boundary_count BIGINT NOT NULL,
            eligible_count BIGINT NOT NULL,
            labeled_count BIGINT NOT NULL,
            missing_count BIGINT NOT NULL,
            invalid_count BIGINT NOT NULL,
            unsupported_basis_count BIGINT NOT NULL,
            observed_terminal_count BIGINT NOT NULL,
            policy_terminal_count BIGINT NOT NULL,
            mean_score DOUBLE,
            mean_forward_return DOUBLE,
            PRIMARY KEY (run_id,signal_id,decision_date,horizon_sessions,decile)
        );
        CREATE TABLE IF NOT EXISTS fundamental_signal_evaluation_summaries (
            run_id VARCHAR NOT NULL,
            signal_id VARCHAR NOT NULL,
            horizon_sessions INTEGER NOT NULL,
            split VARCHAR NOT NULL,
            primary_hypothesis BOOLEAN NOT NULL,
            status VARCHAR NOT NULL,
            spread_dates BIGINT NOT NULL,
            gross_mean DOUBLE,
            hac_lags INTEGER NOT NULL,
            hac_standard_error DOUBLE,
            z_statistic DOUBLE,
            p_value DOUBLE,
            holm_p_value DOUBLE,
            ci95_low DOUBLE,
            ci95_high DOUBLE,
            net_10bp DOUBLE,
            net_25bp DOUBLE,
            net_50bp DOUBLE,
            eligible_count BIGINT NOT NULL,
            labeled_count BIGINT NOT NULL,
            label_coverage DOUBLE,
            observed_terminal_count BIGINT NOT NULL,
            policy_terminal_count BIGINT NOT NULL,
            observed_terminal_share DOUBLE,
            policy_terminal_share DOUBLE,
            annual_json VARCHAR NOT NULL,
            candidate BOOLEAN NOT NULL,
            production_eligible BOOLEAN NOT NULL DEFAULT false,
            blockers_json VARCHAR NOT NULL,
            PRIMARY KEY (run_id,signal_id,horizon_sessions,split)
        );
        CREATE TABLE IF NOT EXISTS fundamental_signal_evaluation_label_evidence (
            run_id VARCHAR NOT NULL,
            signal_id VARCHAR NOT NULL,
            decision_date DATE NOT NULL,
            horizon_sessions INTEGER NOT NULL,
            selected_rows BIGINT NOT NULL,
            selected_sha256 VARCHAR NOT NULL,
            PRIMARY KEY (run_id,signal_id,decision_date,horizon_sessions)
        );
    """)


def _fundamental_signal_evaluation(conn: duckdb.DuckDBPyConnection) -> None:
    create_fundamental_signal_evaluation_tables(conn)
    conn.execute("""
        INSERT OR REPLACE INTO field_catalog
          (table_name,field_name,semantic_type,description,nullable,unit,source_field,updated_at)
        VALUES
          ('forward_returns_survivorship_safe','price_basis','category',
           'Exact validated equity_daily_bars column selected by the bounded forward-return publisher: adjusted_close or close. NULL on legacy or externally loaded rows; no inference from source or run_id.',
           true,NULL,'equity_daily_bars',now()),
          ('forward_returns_survivorship_safe','calculation_version','category',
           'forward_return_publication_v1 identifies the bounded SQL endpoint, selected-bar and terminal-stitch calculation path. NULL means unverified legacy row. It does not certify vendor price adjustments or original delivery vintages.',
           true,NULL,NULL,now())
    """)
    rows = (
        ("fundamental_signal_evaluation_runs", "run_id", "Immutable completed FQ2 evaluation manifest; building and failed runs are diagnostic only."),
        ("fundamental_signal_evaluation_deciles", "run_id,signal_id,decision_date,horizon_sessions,decile", "Frozen pre-label deciles and outcome attrition by decision session and horizon."),
        ("fundamental_signal_evaluation_summaries", "run_id,signal_id,horizon_sessions,split", "Calendar-HAC spread inference and local-family Holm correction."),
        ("fundamental_signal_evaluation_label_evidence", "run_id,signal_id,decision_date,horizon_sessions", "Digest of exact latest-visible selected label IDs, basis, version and economic fields."),
    )
    conn.executemany("""
        INSERT OR REPLACE INTO table_catalog
          (table_name,layer,entity,grain,description,natural_key_json,pit_notes,updated_at)
        VALUES (?,'research','fundamental_signal_evaluation',?,?,?,
          'Run-scoped immutable evidence; only status complete is consumable.',now())
    """, [(name, grain, description, '["' + grain.replace(',', '","') + '"]')
           for name, grain, description in rows])
    _catalog_fields_for_tables(conn, tuple(row[0] for row in rows))
    conn.executemany("""
        INSERT OR REPLACE INTO pit_exemption
          (table_name,missing_columns,reason,exempted_by,exempted_at,source_loaded_at)
        VALUES (?, ?, 'Run-scoped immutable evaluation snapshot with explicit cutoff and selected label evidence.',
                'FQ2 migration 0325', now(), now())
    """, [(name, '["available_at","is_latest_revision"]' if name ==
             "fundamental_signal_evaluation_runs" else
             '["as_of_date","available_at","is_latest_revision","source_loaded_at"]')
            for name, _, _ in rows])
    _refresh_schema_contract_v2_pin(conn)


MIGRATIONS = [Migration(version=325, name="fundamental_signal_evaluation", up=_fundamental_signal_evaluation)]
