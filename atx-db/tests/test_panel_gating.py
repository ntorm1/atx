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

from atx_db.orchestrator import DatasetOrchestrator, QualityGateError


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


def _plant_leakage_breach(store, *, status="failed", checked_at=dt.datetime(2026, 7, 6, 12, 0, 0)):
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
        [str(uuid.uuid4()), status, checked_at],
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


# --- PF4-S2 S2-1 review hardening: bound the recorded leakage/coverage read to the run ---


def test_stale_recorded_breach_not_consumed_end_to_end(tmp_store):
    # A stale (prior-run) critical breach recorded BEFORE this run's step start must NOT
    # halt the new gated run: evaluate_panel_gate reads leakage/coverage bounded to the
    # step-start timestamp, so the Day-1 row is excluded (treated as not-evaluated ->
    # fail-open pass). Without the bound this stale critical row would be the latest
    # recorded outcome and would wrongly halt -- so a green run here proves non-consumption.
    _plant_leakage_breach(tmp_store, checked_at=dt.datetime(2026, 7, 5, 12, 0, 0))  # Day-1
    orch = DatasetOrchestrator(tmp_store, _REGISTRY,
                               clock=lambda: dt.datetime(2026, 7, 6, 12, 0, 0))       # Day-2
    result = orch.run(run_id="run_stale", gate=True)
    assert result.status == "succeeded"
    assert _step_status(tmp_store, "run_stale", "factor_panel") == "succeeded"
    assert "panel_quality_gate_halt" not in _audit_actions(tmp_store, "run_stale")


def test_fresh_recorded_breach_within_run_halts(tmp_store):
    # A breach recorded AT the step start (this build's own DQC; the >= bound is inclusive)
    # is fresh and DOES halt.
    _plant_leakage_breach(tmp_store, checked_at=dt.datetime(2026, 7, 6, 12, 0, 0))
    orch = DatasetOrchestrator(tmp_store, _REGISTRY,
                               clock=lambda: dt.datetime(2026, 7, 6, 12, 0, 0))
    with pytest.raises(QualityGateError) as excinfo:
        orch.run(run_id="run_fresh", gate=True)
    assert excinfo.value.dataset_id == "factor_panel"
    assert _step_status(tmp_store, "run_fresh", "factor_panel") == "failed"
    assert "panel_quality_gate_halt" in _audit_actions(tmp_store, "run_fresh")


def test_stale_passed_row_not_consumed_by_bounded_read(tmp_store):
    # The reviewer's Day-1-passed / Day-2-leaky hole at the function boundary: a stale
    # `passed` leakage row must not be consumed as if it described the current build.
    from atx_db.observability import _latest_recorded_check, evaluate_panel_gate

    _plant_leakage_breach(
        tmp_store, status="passed", checked_at=dt.datetime(2026, 7, 5, 12, 0, 0)
    )
    # Unbounded read still sees the stale row; a read bounded to the new step start does not.
    assert _latest_recorded_check(tmp_store, "factor_panel", "factor_leakage_tplus0") is not None
    assert _latest_recorded_check(
        tmp_store, "factor_panel", "factor_leakage_tplus0",
        recorded_since=dt.datetime(2026, 7, 6, 12, 0, 0),
    ) is None
    # The assembler, bounded to the new run, does not consume the stale pass (fail-open).
    gate = evaluate_panel_gate(
        tmp_store, "factor_panel", recorded_since=dt.datetime(2026, 7, 6, 12, 0, 0)
    )
    assert gate.decision == "pass"


# --- PF4-S2 S2-2: factor observability (freshness SLA + panel anomaly + lineage) ---


def test_factor_freshness_sla_flags_stale_not_fresh(tmp_store):
    from atx_db.observability import evaluate_factor_freshness_slas
    from atx_db.quality import evaluate_quality_gate
    tmp_store.con.execute(
        "INSERT OR REPLACE INTO factor_freshness_sla (dataset_id, max_lag_days, severity, enabled, updated_at)"
        " VALUES ('panel_stale', 3, 'critical', true, ?), ('panel_fresh', 3, 'critical', true, ?)",
        [dt.datetime(2026, 7, 6, 12, 0, 0), dt.datetime(2026, 7, 6, 12, 0, 0)])
    # Migration 0181 also seeds a 'factor_panel' freshness SLA row (critical, max_lag_days=3);
    # give it a fresh watermark too so the assertion below isolates the two rows this test
    # is exercising (panel_stale / panel_fresh) without a spurious extra breach.
    for ds, val in (("panel_stale", "2026-06-01T00:00:00"), ("panel_fresh", "2026-07-05T00:00:00"),
                     ("factor_panel", "2026-07-05T00:00:00")):
        tmp_store.con.execute(
            "INSERT OR REPLACE INTO dataset_watermarks (dataset_id, watermark_name, watermark_value, updated_at)"
            " VALUES (?, 'max_available_at', ?, ?)", [ds, val, dt.datetime(2026, 7, 6, 12, 0, 0)])
    results = evaluate_factor_freshness_slas(tmp_store, as_of=dt.datetime(2026, 7, 6, 12, 0, 0))
    assert [r.dataset_id for r in results] == ["panel_stale"]
    assert results[0].status == "failed" and results[0].severity == "critical"
    gate = evaluate_quality_gate(tmp_store, "panel_stale", record=False, additional_results=results)
    assert gate.decision == "halt"


def test_panel_rowcount_anomaly_flags_collapse_not_stable(tmp_store):
    from atx_db.observability import detect_panel_rowcount_anomaly
    collapse = [("2026-07-01", 4000), ("2026-07-02", 4010), ("2026-07-03", 3990),
                ("2026-07-06", 4000), ("2026-07-07", 40)]
    stable = [("2026-07-01", 4000), ("2026-07-02", 4010), ("2026-07-03", 3990),
              ("2026-07-06", 4000), ("2026-07-07", 4005)]
    anomaly = detect_panel_rowcount_anomaly(tmp_store, cross_section_sizes=collapse)
    assert anomaly is not None and anomaly.observed_value == 40 and abs(anomaly.z_score) > 3.5
    row = tmp_store.con.execute(
        "SELECT z_score, is_anomaly FROM panel_rowcount_anomaly WHERE anomaly_id = ?",
        [anomaly.anomaly_id]).fetchone()
    assert row[1] is True and abs(row[0]) > 3.5
    assert detect_panel_rowcount_anomaly(tmp_store, cross_section_sizes=stable) is None


def test_lineage_completeness_flags_broken_not_traced(tmp_store):
    from atx_db.observability import evaluate_lineage_completeness
    # evaluate_lineage_completeness also requires a factor_definition row per factor_id
    # (the "does this panel factor still resolve a live definition" edge); seed both
    # fixture factor_ids so this test isolates the lineage-edge completeness it targets.
    tmp_store.con.execute(
        "INSERT OR REPLACE INTO factor_definition (factor_id, factor_name, family, description, "
        "expression, input_ids_json, direction, lookback_days, neutralization_spec_json, unit, "
        "sign, scale, available_at_policy, declared_in, source) VALUES "
        "('f_traced', 'f_traced', 'test', 'test fixture factor', 'x', '[]', 1, 0, '{}', 'raw', "
        "'higher_is_better', 'raw', 't_plus_0', 'test', 'test'), "
        "('f_broken', 'f_broken', 'test', 'test fixture factor', 'x', '[]', 1, 0, '{}', 'raw', "
        "'higher_is_better', 'raw', 't_plus_0', 'test', 'test')"
    )
    traced = {"source_fact": "revenue", "formula": "roe", "standardization_rule": "z", "vintage": "v1"}
    broken = {"source_fact": "revenue", "standardization_rule": "z", "vintage": "v1"}  # no formula
    failures = evaluate_lineage_completeness(
        tmp_store, panel_factors=[("f_traced", traced), ("f_broken", broken)])
    assert [f[0] for f in failures] == ["f_broken"]
    assert "formula" in dict(failures)["f_broken"]
    counts = dict(tmp_store.con.execute(
        "SELECT factor_id, is_complete FROM lineage_completeness_checks "
        "WHERE factor_id IN ('f_traced', 'f_broken')").fetchall())
    assert counts["f_traced"] is True and counts["f_broken"] is False


# --- PF4-S2 S2-3: maintenance_schedule (cadence-as-data) + read-only dry-run planner ---


def test_maintenance_schedule_queryable_per_dataset(tmp_store):
    row = tmp_store.con.execute(
        "SELECT cadence, cadence_trigger, backfill_window_json, chunk FROM maintenance_schedule "
        "WHERE dataset_id = 'factor_panel'").fetchone()
    import json
    assert row[0] == "on_rebuild" and row[1] == "upstream_watermark_advance"
    assert json.loads(row[2])["shape"] == "rebuild" and row[3] == "P1M"
    ids = [r[0] for r in tmp_store.con.execute(
        "SELECT dataset_id FROM maintenance_schedule WHERE enabled ORDER BY dataset_id").fetchall()]
    assert {"equity_daily_bars", "fundamentals", "vintages", "factor_panel"} <= set(ids)


def test_planner_dry_runs_without_touching_db(tmp_store):
    from scripts.warehouse_schedule_plan import plan_maintenance
    before = tmp_store.con.execute("SELECT count(*) FROM maintenance_schedule").fetchone()[0]
    plan = plan_maintenance(tmp_store)
    by_id = {p["dataset_id"]: p for p in plan}
    assert by_id["factor_panel"]["planned_mode"] == "backfill"  # rebuild shape -> backfill mode
    assert by_id["equity_daily_bars"]["backfill_window"]["start"] == "2004-01-01"
    assert all(p["executes"] is False and p["dry_run"] is True for p in plan)
    after = tmp_store.con.execute("SELECT count(*) FROM maintenance_schedule").fetchone()[0]
    assert after == before  # planner performed no writes
