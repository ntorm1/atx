"""PF4-S2 migration bodies: panel gating + factor observability + maintenance cadence.

Migration 0180 (panel_gate_config) landed in PF4-S2 S2-1. 0181 (factor-observability
schema) and 0182 (its indexes) land in this task (PF4-S2 S2-2). 0183
(maintenance_schedule) is reserved for S2-3 and will be appended to ``MIGRATIONS`` by
that task.
"""
from __future__ import annotations

import duckdb

from ._runner import Migration
from .bodies_0001_0137 import _catalog_fields_for_tables
from .bodies_0140_0143 import _refresh_schema_contract_v2_pin


def _pf4_s2_panel_gate_config(conn: duckdb.DuckDBPyConnection) -> None:
    """PF4-S2 S2-1: data-driven registry of gate-critical factor-panel checks."""
    conn.execute(
        """
        CREATE TABLE IF NOT EXISTS panel_gate_config (
            dataset_id VARCHAR NOT NULL,
            check_name VARCHAR NOT NULL,
            severity   VARCHAR NOT NULL DEFAULT 'critical',
            enabled    BOOLEAN NOT NULL DEFAULT true,
            source     VARCHAR NOT NULL DEFAULT 'pf4_s2',
            notes      VARCHAR,
            updated_at TIMESTAMP NOT NULL DEFAULT now(),
            PRIMARY KEY (dataset_id, check_name)
        )
        """
    )
    # Seed the factor-panel critical set. check_name values are the LANDED PF4-S1/PF3-S10
    # names (reconciled at implementation time from the sprint plan's placeholders):
    #   factor_leakage_tplus0          -- PF4-S1 signal_eval.LEAKAGE_DQC_CHECK_NAME
    #   factor_coverage_asof_universe  -- PF4-S1 signal_eval.COVERAGE_DQC_CHECK_NAME
    #   factor_panel_export_contract   -- PF3-S10 factor_panel.PANEL_EXPORT_GATE_CHECK_NAME
    conn.execute(
        """
        INSERT OR REPLACE INTO panel_gate_config
            (dataset_id, check_name, severity, enabled, source, notes, updated_at)
        VALUES
            ('factor_panel', 'factor_panel_export_contract', 'critical', true, 'pf4_s2',
             'PF3-S10 export-boundary unit/sign/scale + universe-as-of + zero-lookahead gate.', now()),
            ('factor_panel', 'factor_leakage_tplus0',        'critical', true, 'pf4_s2',
             'PF4-S1 t+0 leakage probe (signal_eval.LEAKAGE_DQC_CHECK_NAME), gated critical.', now()),
            ('factor_panel', 'factor_coverage_asof_universe','critical', true, 'pf4_s2',
             'PF4-S1 factor coverage vs as-of universe (signal_eval.COVERAGE_DQC_CHECK_NAME), gated critical.', now())
        """
    )
    conn.execute(
        """
        INSERT OR REPLACE INTO table_catalog (
            table_name, layer, entity, grain, description,
            natural_key_json, pit_notes, updated_at
        )
        VALUES (
            'panel_gate_config', 'control', 'panel_gate_config',
            'dataset_id,check_name',
            'Data-driven registry of the critical quality checks the orchestrator halt gate evaluates '
            'for each factor-panel dataset, with a per-check severity retunable without a deploy.',
            '["dataset_id","check_name"]',
            'Control table; the orchestrator reads it under gate=True to decide the panel halt set and '
            'the severity applied to each recorded check outcome.',
            now()
        )
        """
    )
    _catalog_fields_for_tables(conn, ("panel_gate_config",))
    _refresh_schema_contract_v2_pin(conn)


def _pf4_s2_factor_observability_schema(conn: duckdb.DuckDBPyConnection) -> None:
    """PF4-S2 S2-2: factor freshness SLA, panel row-count anomaly, lineage-completeness."""
    conn.execute(
        """
        CREATE TABLE IF NOT EXISTS factor_freshness_sla (
            dataset_id   VARCHAR PRIMARY KEY,
            max_lag_days INTEGER NOT NULL,
            severity     VARCHAR NOT NULL DEFAULT 'warning',
            enabled      BOOLEAN NOT NULL DEFAULT true,
            updated_at   TIMESTAMP NOT NULL DEFAULT now()
        )
        """
    )
    conn.execute(
        """
        CREATE TABLE IF NOT EXISTS panel_rowcount_anomaly (
            anomaly_id      VARCHAR PRIMARY KEY,
            dataset_id      VARCHAR NOT NULL,
            as_of_date      DATE NOT NULL,
            baseline_median DOUBLE NOT NULL,
            baseline_mad    DOUBLE NOT NULL,
            observed_value  DOUBLE NOT NULL,
            z_score         DOUBLE NOT NULL,
            is_anomaly      BOOLEAN NOT NULL DEFAULT true,
            severity        VARCHAR NOT NULL DEFAULT 'warning',
            details_json    VARCHAR NOT NULL DEFAULT '{}',
            checked_at      TIMESTAMP NOT NULL DEFAULT now()
        )
        """
    )
    conn.execute(
        """
        CREATE TABLE IF NOT EXISTS lineage_completeness_checks (
            check_id      VARCHAR PRIMARY KEY,
            dataset_id    VARCHAR NOT NULL,
            factor_id     VARCHAR NOT NULL,
            missing_edges_json VARCHAR NOT NULL DEFAULT '[]',
            is_complete   BOOLEAN NOT NULL,
            severity      VARCHAR NOT NULL DEFAULT 'critical',
            details_json  VARCHAR NOT NULL DEFAULT '{}',
            checked_at    TIMESTAMP NOT NULL DEFAULT now()
        )
        """
    )
    # Seed the factor-panel freshness SLA (as-of the latest trading day within 3 days).
    conn.execute(
        """
        INSERT OR REPLACE INTO factor_freshness_sla
            (dataset_id, max_lag_days, severity, enabled, updated_at)
        VALUES ('factor_panel', 3, 'critical', true, now())
        """
    )
    for table_name, entity, grain, description, key, pit in (
        ("factor_freshness_sla", "factor_freshness_sla", "dataset_id",
         "Per factor/panel dataset freshness SLA: max watermark lag before a severity-tagged breach.",
         '["dataset_id"]', "Control table; evaluate_factor_freshness_slas joins dataset_watermarks."),
        ("panel_rowcount_anomaly", "panel_rowcount_anomaly", "anomaly_id",
         "Median/MAD z-score anomalies over per-as-of factor-panel cross-section size (collapse detection).",
         '["anomaly_id"]', "Derived from v_factor_panel cross-section counts; z_score recorded per breach."),
        ("lineage_completeness_checks", "lineage_completeness_checks", "check_id",
         "Per emitted panel factor: whether its full lineage chain (source fact, formula, standardization "
         "rule, vintage) resolves; a missing edge is a surpass-axis-1 regression.",
         '["check_id"]', "Derived from v_factor_panel factor_id + input_lineage_json + factor_definition."),
    ):
        conn.execute(
            """
            INSERT OR REPLACE INTO table_catalog (
                table_name, layer, entity, grain, description,
                natural_key_json, pit_notes, updated_at
            )
            VALUES (?, 'control', ?, ?, ?, ?, ?, now())
            """,
            [table_name, entity, grain, description, key, pit],
        )
    _catalog_fields_for_tables(
        conn,
        ("factor_freshness_sla", "panel_rowcount_anomaly", "lineage_completeness_checks"),
    )
    _refresh_schema_contract_v2_pin(conn)


def _pf4_s2_factor_observability_indexes(conn: duckdb.DuckDBPyConnection) -> None:
    """PF4-S2 S2-2 (0182): indexes split from schema per the WAL precedent."""
    for statement in (
        "CREATE INDEX IF NOT EXISTS idx_panel_rowcount_anomaly_dataset "
        "ON panel_rowcount_anomaly(dataset_id, checked_at)",
        "CREATE INDEX IF NOT EXISTS idx_lineage_completeness_dataset "
        "ON lineage_completeness_checks(dataset_id, factor_id, checked_at)",
    ):
        conn.execute(statement)
    _refresh_schema_contract_v2_pin(conn)


MIGRATIONS: list[Migration] = [
    Migration(version=180, name="pf4_s2_panel_gate_config", up=_pf4_s2_panel_gate_config),
    Migration(version=181, name="pf4_s2_factor_observability_schema",
              up=_pf4_s2_factor_observability_schema),
    Migration(version=182, name="pf4_s2_factor_observability_indexes",
              up=_pf4_s2_factor_observability_indexes),
]
