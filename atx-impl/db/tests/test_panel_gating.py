"""PF4-S2 S2-1: factor-panel quality gated in the orchestrator (extends pf2-S10).

A fixture registry (factor_source -> factor_panel -> factor_panel_export) drives
DatasetOrchestrator.run(gate=True) through the panel_gate_config (migration 0180)
branch added to _run_step. "Planting a leakage breach" inserts a data_quality_checks
row for the configured check_name -- reconciled here to PF4-S1's landed
signal_eval.LEAKAGE_DQC_CHECK_NAME ("factor_leakage_tplus0"), NOT the sprint plan's
placeholder ("factor_panel_leakage_probe").
"""

import datetime as dt

import pytest

from db.orchestrator import DatasetOrchestrator, QualityGateError


class _Result:
    def __init__(self, rows_loaded=0):
        self.rows_loaded = rows_loaded


def _dataset(dataset_id, depends_on=()):
    class _DS:
        pass
    _DS.dataset_id = dataset_id
    _DS.depends_on = tuple(depends_on)
    _DS.run = lambda self, store, options: _Result(0)
    _DS.__name__ = f"DS_{dataset_id}"
    return _DS


_REGISTRY = {
    "factor_source": _dataset("factor_source"),
    "factor_panel": _dataset("factor_panel", ("factor_source",)),
    "factor_panel_export": _dataset("factor_panel_export", ("factor_panel",)),
}


def _plant_leakage_breach(store, *, status="failed"):
    import uuid
    store.con.execute(
        """
        INSERT INTO data_quality_checks (
            check_id, dataset_id, table_name, check_name, status, severity,
            observed_value, threshold_value, details_json, checked_at
        )
        VALUES (?, 'factor_panel', 'v_factor_panel', 'factor_leakage_tplus0',
                ?, 'critical', 3.0, 0.0, '{}', ?)
        """,
        [str(uuid.uuid4()), status, dt.datetime(2026, 7, 6, 12, 0, 0)],
    )


def _step_status(store, run_id, dataset_id):
    return store.con.execute(
        "SELECT status FROM etl_job_steps WHERE run_id = ? AND dataset_id = ?",
        [run_id, dataset_id],
    ).fetchone()[0]


def _audit_actions(store, run_id):
    return [r[0] for r in store.con.execute(
        "SELECT action FROM etl_job_audit WHERE run_id = ? ORDER BY ts", [run_id]).fetchall()]


def test_planted_leakage_critical_halts_at_panel(tmp_store):
    _plant_leakage_breach(tmp_store)  # panel_gate_config seeds this check critical (0180)
    orch = DatasetOrchestrator(tmp_store, _REGISTRY,
                               clock=lambda: dt.datetime(2026, 7, 6, 12, 0, 0))
    with pytest.raises(QualityGateError) as excinfo:
        orch.run(run_id="run_halt", gate=True)
    assert excinfo.value.dataset_id == "factor_panel"
    assert tmp_store.con.execute(
        "SELECT status FROM etl_job_runs WHERE run_id = 'run_halt' AND run_kind = 'orchestrator'"
    ).fetchone()[0] == "failed"
    assert _step_status(tmp_store, "run_halt", "factor_panel") == "failed"
    assert _step_status(tmp_store, "run_halt", "factor_panel_export") == "pending"
    assert "panel_quality_gate_halt" in _audit_actions(tmp_store, "run_halt")
    assert "step_quality_gate_halt" not in _audit_actions(tmp_store, "run_halt")


def test_same_breach_as_error_completes_partial(tmp_store):
    _plant_leakage_breach(tmp_store)
    tmp_store.con.execute(
        "UPDATE panel_gate_config SET severity = 'error' "
        "WHERE dataset_id = 'factor_panel' AND check_name = 'factor_leakage_tplus0'")
    orch = DatasetOrchestrator(tmp_store, _REGISTRY,
                               clock=lambda: dt.datetime(2026, 7, 6, 12, 0, 0))
    result = orch.run(run_id="run_partial", gate=True)
    assert result.status == "partial"
    assert _step_status(tmp_store, "run_partial", "factor_panel_export") == "succeeded"
    assert "panel_quality_gate_degrade" in _audit_actions(tmp_store, "run_partial")


def test_same_breach_as_warning_completes_succeeded(tmp_store):
    _plant_leakage_breach(tmp_store)
    tmp_store.con.execute(
        "UPDATE panel_gate_config SET severity = 'warning' "
        "WHERE dataset_id = 'factor_panel' AND check_name = 'factor_leakage_tplus0'")
    orch = DatasetOrchestrator(tmp_store, _REGISTRY,
                               clock=lambda: dt.datetime(2026, 7, 6, 12, 0, 0))
    result = orch.run(run_id="run_warn", gate=True)
    assert result.status == "succeeded"
    assert "panel_quality_gate_warn" in _audit_actions(tmp_store, "run_warn")


def test_gate_false_reproduces_ungated_walk(tmp_store):
    _plant_leakage_breach(tmp_store)
    orch = DatasetOrchestrator(tmp_store, _REGISTRY,
                               clock=lambda: dt.datetime(2026, 7, 6, 12, 0, 0))
    result = orch.run(run_id="run_ungated", gate=False)
    assert result.status == "succeeded"
    actions = _audit_actions(tmp_store, "run_ungated")
    assert not any(a.startswith("panel_quality_gate") for a in actions)


def test_clean_panel_passes(tmp_store):
    # No planted breach; export contract passes on the template's empty v_factor_panel.
    orch = DatasetOrchestrator(tmp_store, _REGISTRY,
                               clock=lambda: dt.datetime(2026, 7, 6, 12, 0, 0))
    result = orch.run(run_id="run_clean", gate=True)
    assert result.status == "succeeded"
    assert _step_status(tmp_store, "run_clean", "factor_panel") == "succeeded"
