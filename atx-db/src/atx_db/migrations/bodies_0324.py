"""Run-versioned point-in-time fundamental research panels."""

from __future__ import annotations

import duckdb

from ._runner import Migration
from .bodies_0001_0137 import _catalog_fields_for_tables
from .bodies_0140_0143 import _refresh_schema_contract_v2_pin


def create_fundamental_signal_tables(conn: duckdb.DuckDBPyConnection) -> None:
    """Create only FQ1 tables for small isolated fixtures."""
    conn.execute("""
        CREATE TABLE IF NOT EXISTS fundamental_signal_runs (
            run_id VARCHAR PRIMARY KEY,
            status VARCHAR NOT NULL,
            spec_json VARCHAR NOT NULL,
            spec_sha256 VARCHAR NOT NULL,
            definitions_json VARCHAR NOT NULL,
            definitions_sha256 VARCHAR NOT NULL,
            query_version VARCHAR NOT NULL,
            code_sha256 VARCHAR NOT NULL,
            source_ids_json VARCHAR NOT NULL,
            calendar_sha256 VARCHAR,
            start_date DATE NOT NULL,
            end_date DATE NOT NULL,
            as_of_date DATE NOT NULL,
            run_at TIMESTAMP NOT NULL,
            decision_policy VARCHAR NOT NULL,
            diagnostic_json VARCHAR,
            blockers_json VARCHAR NOT NULL,
            panel_sha256 VARCHAR,
            production_eligible BOOLEAN NOT NULL DEFAULT false,
            source_loaded_at TIMESTAMP NOT NULL DEFAULT now()
        );
        CREATE TABLE IF NOT EXISTS fundamental_signal_definitions (
            run_id VARCHAR NOT NULL,
            signal_id VARCHAR NOT NULL,
            ordinal INTEGER NOT NULL,
            spec_json VARCHAR NOT NULL,
            spec_sha256 VARCHAR NOT NULL,
            PRIMARY KEY (run_id, signal_id)
        );
        CREATE TABLE IF NOT EXISTS fundamental_signal_values (
            run_id VARCHAR NOT NULL,
            signal_id VARCHAR NOT NULL,
            decision_date DATE NOT NULL,
            security_id VARCHAR NOT NULL,
            decision_at TIMESTAMP NOT NULL,
            entry_date DATE,
            score DOUBLE,
            eligible BOOLEAN NOT NULL,
            reason VARCHAR NOT NULL,
            input_end DATE,
            input_available_at TIMESTAMP,
            cohort_size BIGINT NOT NULL
        );
        CREATE TABLE IF NOT EXISTS fundamental_signal_inputs (
            run_id VARCHAR NOT NULL,
            signal_id VARCHAR NOT NULL,
            decision_date DATE NOT NULL,
            security_id VARCHAR NOT NULL,
            term_ordinal INTEGER NOT NULL,
            metric_code VARCHAR NOT NULL,
            metric_window VARCHAR NOT NULL,
            weight DOUBLE NOT NULL,
            derived_value_id VARCHAR,
            derived_owner_security_id VARCHAR,
            definition_hash VARCHAR,
            inputs_hash VARCHAR,
            selected_input_refs_hash VARCHAR,
            lineage_digest VARCHAR,
            selected_input_cik VARCHAR,
            lineage_status VARCHAR,
            lineage_leaf_ids_json VARCHAR,
            selected_leaf_oldest_end DATE,
            selected_leaf_newest_end DATE,
            period_end DATE,
            fiscal_period_start DATE,
            fiscal_period_end DATE,
            available_at TIMESTAMP,
            value_origin VARCHAR,
            history_status VARCHAR,
            value_status VARCHAR,
            raw_value DOUBLE,
            reason VARCHAR NOT NULL
        );
        CREATE TABLE IF NOT EXISTS fundamental_signal_proofs (
            run_id VARCHAR NOT NULL,
            derived_value_id VARCHAR NOT NULL,
            derived_owner_security_id VARCHAR NOT NULL,
            metric_code VARCHAR NOT NULL,
            metric_window VARCHAR NOT NULL,
            root_available_at TIMESTAMP NOT NULL,
            selected_input_refs_hash VARCHAR,
            selected_cik VARCHAR,
            status VARCHAR NOT NULL,
            reason VARCHAR NOT NULL,
            leaf_ids_json VARCHAR NOT NULL,
            input_clocks_json VARCHAR NOT NULL,
            fiscal_ends_json VARCHAR NOT NULL,
            oldest_fiscal_end DATE,
            newest_fiscal_end DATE,
            latest_input_clock TIMESTAMP,
            proof_digest VARCHAR,
            proof_cutoff TIMESTAMP NOT NULL
        );
        CREATE TABLE IF NOT EXISTS fundamental_signal_coverage (
            run_id VARCHAR NOT NULL,
            signal_id VARCHAR NOT NULL,
            decision_date DATE NOT NULL,
            decision_at TIMESTAMP NOT NULL,
            entry_date DATE,
            status VARCHAR NOT NULL,
            visible_members BIGINT NOT NULL,
            common_members BIGINT NOT NULL,
            overlap_members BIGINT NOT NULL,
            qualified_cik BIGINT NOT NULL,
            unmatched_owner_states BIGINT NOT NULL,
            eligible_scores BIGINT NOT NULL,
            excluded_scores BIGINT NOT NULL,
            constant_scores BOOLEAN NOT NULL,
            reasons_json VARCHAR NOT NULL,
            PRIMARY KEY (run_id, signal_id, decision_date)
        );
    """)


def _fundamental_signal_panel(conn: duckdb.DuckDBPyConnection) -> None:
    create_fundamental_signal_tables(conn)
    rows = (
        ("fundamental_signal_runs", "run_id", "Immutable completed research experiment manifest and digest."),
        ("fundamental_signal_definitions", "run_id,signal_id", "Frozen run-scoped declarative signal specifications."),
        ("fundamental_signal_values", "run_id,signal_id,decision_date,security_id", "Daily long PIT scores including excluded names and next-session entry."),
        ("fundamental_signal_inputs", "run_id,signal_id,decision_date,security_id,term_ordinal", "Selected derived event state and per-term rejection evidence."),
        ("fundamental_signal_proofs", "run_id,derived_value_id", "Exact selected-input proof cached once per run and reused across decision sessions."),
        ("fundamental_signal_coverage", "run_id,signal_id,decision_date", "Every requested observed decision session including empty cohorts."),
    )
    conn.executemany("""
        INSERT OR REPLACE INTO table_catalog
          (table_name,layer,entity,grain,description,natural_key_json,pit_notes,updated_at)
        VALUES (?,'research','fundamental_signal',?,?,?,
          'Immutable run snapshot. Decision cutoff is 22:00 UTC; only completed run manifests are consumable.',now())
    """, [(name, grain, description, '["' + grain.replace(',', '","') + '"]')
           for name, grain, description in rows])
    _catalog_fields_for_tables(conn, tuple(row[0] for row in rows))
    # These are frozen research records, not independently revised source facts.
    conn.executemany("""
        INSERT OR REPLACE INTO pit_exemption
          (table_name,missing_columns,reason,exempted_by,exempted_at,source_loaded_at)
        VALUES (?, ?,
          'Run-scoped immutable research snapshot; decision_at and selected input clocks carry PIT provenance.',
          'FQ1 migration 0324', now(), now())
    """, [(name, '["as_of_date","available_at","is_latest_revision"]'
             if name == "fundamental_signal_runs" else
             '["as_of_date","available_at","is_latest_revision","source_loaded_at"]')
            for name, _, _ in rows])
    _refresh_schema_contract_v2_pin(conn)


MIGRATIONS = [Migration(version=324, name="fundamental_signal_panel", up=_fundamental_signal_panel)]
