"""PF4-S2 migration bodies: panel gating + factor observability + maintenance cadence.

Migration 0180 (panel_gate_config) landed in PF4-S2 S2-1. 0181 (factor-observability
schema) and 0182 (its indexes) landed in PF4-S2 S2-2. 0183 (maintenance_schedule)
lands in this task (PF4-S2 S2-3): cadence-as-data read by the read-only dry-run
planner in ``scripts/warehouse_schedule_plan.py``.
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
    # panel_rowcount_anomaly is a monitoring/detection-results artifact (median/MAD z-score
    # anomalies over the factor-panel cross-section size), analogous to data_quality_checks: its
    # as_of_date is the monitored panel date and checked_at the detection time. It has no
    # bitemporal revision chain and no warehouse-load lineage, so available_at / source_loaded_at /
    # run_id / is_latest_revision are not meaningful. Exempt them explicitly rather than inventing
    # PIT columns on a derived monitoring table.
    conn.execute(
        """
        INSERT OR REPLACE INTO pit_exemption (
            table_name, missing_columns, reason, exempted_by, exempted_at, source_loaded_at
        )
        VALUES (
            'panel_rowcount_anomaly',
            '["available_at","source_loaded_at","run_id","is_latest_revision"]',
            'panel_rowcount_anomaly is a derived monitoring/detection-results table (median/MAD '
            'z-score anomalies over factor-panel cross-section size). as_of_date is the monitored '
            'panel date and checked_at the detection time; there is no row revision chain and no '
            'warehouse-load lineage, so available_at/source_loaded_at/run_id/is_latest_revision are '
            'not meaningful.',
            'pf4-s2-s2-2',
            now(),
            now()
        )
        """
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


def _pf4_s2_maintenance_schedule(conn: duckdb.DuckDBPyConnection) -> None:
    """PF4-S2 S2-3: incremental cadence + backfill-window shape as queryable data."""
    conn.execute(
        """
        CREATE TABLE IF NOT EXISTS maintenance_schedule (
            dataset_id          VARCHAR PRIMARY KEY,
            cadence             VARCHAR NOT NULL,
            cadence_trigger     VARCHAR,
            backfill_window_json VARCHAR NOT NULL DEFAULT '{}',
            chunk               VARCHAR,
            enabled             BOOLEAN NOT NULL DEFAULT true,
            notes               VARCHAR,
            updated_at          TIMESTAMP NOT NULL DEFAULT now()
        )
        """
    )
    conn.execute(
        """
        INSERT OR REPLACE INTO maintenance_schedule
            (dataset_id, cadence, cadence_trigger, backfill_window_json, chunk, enabled, notes, updated_at)
        VALUES
            ('equity_daily_bars', 'daily', 'trading_day_close',
             '{"lookback":"full_history","start":"2004-01-01","shape":"windowed"}', 'P1M', true,
             'Dense price bars; backfill widens equity_daily_bars to 2004+.', now()),
            ('fundamentals', 'quarterly', 'on_filing',
             '{"lookback":"full_history","shape":"windowed"}', 'P3M', true,
             'Quarterly refresh plus event-driven on SEC filing.', now()),
            ('vintages', 'event', 'on_restatement',
             '{"lookback":"affected_periods","shape":"targeted"}', 'P1Y', true,
             'Vintage capture triggered by restatement, not a fixed cadence.', now()),
            ('factor_panel', 'on_rebuild', 'upstream_watermark_advance',
             '{"lookback":"incremental","shape":"rebuild"}', 'P1M', true,
             'Panel rebuild triggered when any upstream factor watermark advances.', now())
        """
    )
    conn.execute(
        """
        INSERT OR REPLACE INTO table_catalog (
            table_name, layer, entity, grain, description,
            natural_key_json, pit_notes, updated_at
        )
        VALUES (
            'maintenance_schedule', 'control', 'maintenance_schedule', 'dataset_id',
            'Cadence-as-data: per-dataset incremental cadence + trigger + backfill-window shape the S1 DAG '
            'runs on; read by the dry-run planner to plan (not execute) the operator archive.',
            '["dataset_id"]',
            'Control table; the planner reads it read-only and never mutates live data.',
            now()
        )
        """
    )
    _catalog_fields_for_tables(conn, ("maintenance_schedule",))
    _refresh_schema_contract_v2_pin(conn)


MIGRATIONS: list[Migration] = [
    Migration(version=180, name="pf4_s2_panel_gate_config", up=_pf4_s2_panel_gate_config),
    Migration(version=181, name="pf4_s2_factor_observability_schema",
              up=_pf4_s2_factor_observability_schema),
    Migration(version=182, name="pf4_s2_factor_observability_indexes",
              up=_pf4_s2_factor_observability_indexes),
    Migration(version=183, name="pf4_s2_maintenance_schedule", up=_pf4_s2_maintenance_schedule),
]
