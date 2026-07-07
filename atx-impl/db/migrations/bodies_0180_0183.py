"""PF4-S2 migration bodies: panel gating + factor observability + maintenance cadence.

Only migration 0180 (panel_gate_config) lands in this task (PF4-S2 S2-1). 0181
(factor-observability schema), 0182 (its indexes), and 0183 (maintenance_schedule) are
reserved for S2-2/S2-3 and will be appended to ``MIGRATIONS`` by those tasks.
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


MIGRATIONS: list[Migration] = [
    Migration(version=180, name="pf4_s2_panel_gate_config", up=_pf4_s2_panel_gate_config),
]
