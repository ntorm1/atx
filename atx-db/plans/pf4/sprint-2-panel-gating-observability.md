# Sprint PF4-S2 — Panel gating + factor observability (closes PF3-S12 operational core)

**Track:** Close pf3 (Track A). **Reserved migrations 0180–0183.** **Depends on PF4-S1** (its gated
**leakage** + **coverage** factor-panel DQC must exist and record `data_quality_checks` rows) and on **pf2-S10**
(the `DatasetOrchestrator` step-quality-gate, `evaluate_quality_gate`/`summarize_quality_gate`, and the
`db/observability.py` freshness/anomaly primitives it extends into the factor domain). **Sequential after PF4-S1.**

**Goal:** make the pf3 factor **panel** *operationally trustworthy* — not by computing any new factor, but by
**gating**, **observing**, and **scheduling** what S1–S10 (and PF4-S1) already produce. Wire PF4-S1's leakage +
coverage DQC and PF3-S10's export-contract check into `db/orchestrator.py` as `severity=critical`
**orchestrator halt gates** for the factor-panel dataset(s) — **reusing** pf2-S10's evaluator + halt semantics,
not a new one — so a leaky / broken / stale / collapsed panel **halts a gated run** instead of shipping green.
Extend `db/observability.py` with three factor-domain surfaces (factor **freshness SLA**, panel **row-count
anomaly**, **lineage-completeness**) all routed through the pf2-S10 gated-check decision path. Codify the S1
DAG's maintenance **cadence as data** (`maintenance_schedule`) read by a thin `scripts/` planner that *plans*
(does not execute) the operator archive. This is the operational core PF3-S12 never built.

**Mandate / Owns:**
- `db/orchestrator.py` — a panel-gate branch in `_run_step`'s existing gate hook (a `panel_quality_gate_halt`
  audit label + a `panel_gate_config`-driven check assembler), **additive** to pf2-S10's gate; nothing in the
  fundamentals gate path changes.
- `db/observability.py` — three new factor surfaces (`evaluate_factor_freshness_slas`,
  `detect_panel_rowcount_anomaly`, `evaluate_lineage_completeness`) plus the panel-gate assembler
  `evaluate_panel_gate`, all **reusing** the existing `_median_mad` / `_robust_z` / `_parse_timestamp` /
  `_slug` / `_upsert_quality_registry` / `quality_check` primitives and `run_warehouse_quality_checks` /
  `summarize_quality_gate`.
- NEW `db/migrations/bodies_0180_0183.py` (registered in `db/migrations/registry.py`): `panel_gate_config`
  (0180), factor-observability schema (0181), its indexes (0182), `maintenance_schedule` (0183).
- NEW `scripts/warehouse_schedule_plan.py` — a read-only cadence planner (dry-run only).
- NEW `db/tests/test_panel_gating.py`.

**Must NOT touch:** the **factor / panel / eval content** — S7's factor engine + operators, S8's fundamental
families, S9's cross-domain namespace, S10's `v_factor_panel` + Parquet/Arrow export, PF4-S1's `db/signal_eval.py`
IC/decay/turnover/crowding bodies and its leakage/coverage DQC evaluators. This sprint **gates and observes**
that content; it never changes what a factor, panel row, or eval metric *computes*, and it never re-implements
PF4-S1's leakage/coverage evaluators — it **consumes** their recorded `data_quality_checks` outcomes and applies
a configured gate severity. Do **not** touch S1's backfill engine (`db/backfill.py` / the windowed-resumable
`DatasetOrchestrator` partition driver — this sprint *schedules* it, it does not re-implement it). Do **not**
re-implement pf2-S10's `evaluate_quality_gate`/`summarize_quality_gate`, its `dataset_freshness_sla` /
`data_quality_anomaly` primitives, `record_audit`, `OrchestratorRunError`/`QualityGateError`, or the
`etl_job_runs`/`etl_job_steps`/`etl_job_audit` ledger — S2 **reuses** them. Do not edit a landed migration or
another sprint's reserved range.

---

## Baseline / where the cycles go (measured 2026-07-06 against `atx-impl/db`)

1. **The factor panel is UNGATED.** pf2-S10 wired a `severity=critical` gate into `DatasetOrchestrator._run_step`
   (`orchestrator.py:1687`) that, under `gate=True`, calls `evaluate_quality_gate(self.store, dataset_id)`, and
   on `decision == "halt"` marks the step `failed`, records `record_audit(action="step_quality_gate_halt")`,
   raises `QualityGateError` (a subclass of `OrchestratorRunError`), and leaves downstream steps `pending`
   (`orchestrator.py:1692–1723`, run-level `run_quality_gate_halt` at `1402–1413`). That gate covers
   *fundamentals* datasets. PF3-S10 registered `factor_panel_export_contract` (`critical`) in
   `quality_check_registry` and `run_warehouse_quality_checks` special-cases it for `dataset_id='factor_panel'`
   (`quality/_runner.py:177–203`), and PF4-S1 adds **leakage** + **coverage** critical DQC — but **no
   orchestrator path treats a factor-panel-critical failure as a halt with a panel-specific audit trail, and
   there is no data-driven registry of which panel checks are gate-critical.** A leaky factor, a broken panel,
   or a stale/empty cross-section can ship a `succeeded` run.

2. **`observability.py` watches fundamentals, not factors.** `evaluate_freshness_slas` (`observability.py:105`)
   reads `dataset_freshness_sla` and emits a severity-tagged `data_quality_checks` row per breached SLA;
   `detect_rowcount_anomalies` (`observability.py:212`) computes a median/MAD `_robust_z` over
   `data_quality_checks` history. Both are fundamentals/pricing-scoped. There is **no factor freshness SLA** (is
   the panel as-of the latest trading day?), **no panel row-count anomaly** (did today's cross-section collapse
   from ~4,000 names to 40?), and **no lineage-completeness** surface (does every emitted panel factor still
   resolve a full chain to source fact + formula + standardization rule + vintage — surpass axis 1 — or did a
   factor silently lose a lineage edge?).

3. **The maintenance SCHEDULE isn't codified.** S1 built a resumable **backfill** mode and an
   **incremental-maintenance** mode with per-partition watermarks (`DatasetOrchestrator.run_backfill` /
   `run_maintenance`, `orchestrator.py:564–618`). But *when* each dataset re-runs (daily bars, quarterly +
   on-filing fundamentals, on-restatement vintages, factor-panel rebuild trigger) and its backfill-window shape
   are tribal knowledge, not data an operator or fresh agent can query and plan from.

**Already good — do not regress:** pf2-S10's `_run_step` gate hook, its `critical`→halt / `error`→partial /
`warning`→record semantics via `summarize_quality_gate` (`quality/_runner.py:35–55`: worst `critical`→`halt`,
worst `error`→`partial`, else `pass`), `QualityGateError`, `record_audit`, and the `etl_job_*` ledger stay
byte-identical. S2 **adds** the factor-panel dataset to the gated set and *factor* SLA rows / *panel* baselines
through the same code paths; it does not fork the evaluators.

---

## PIT / determinism + production contract (clauses honored: A, B, E, F, G, J)

- **(A) Bitemporal / no lookahead.** The panel gate reads only already-materialized panel rows and recorded
  `data_quality_checks` outcomes — no future data enters the halt decision. `available_at ≤ as_of` gating in
  `v_factor_panel` is unchanged; the freshness SLA measures `as_of − max(watermark)` and never reads ahead.
- **(B) Append-only, catalogued migrations.** Migrations **0180–0183** only, forward-only + idempotent
  (`CREATE TABLE IF NOT EXISTS`, `INSERT OR REPLACE`), **schema split from index** (0181 schema / 0182 index per
  the WAL precedent), each new table seeding `table_catalog` + `field_catalog` in the **same** migration via
  `_catalog_fields_for_tables` and closing with `_refresh_schema_contract_v2_pin`. Never renumber; never edit a
  landed migration. (0168–0179 are pf3-S11/S12 + PF4-S1 ranges — the registry validator requires ascending +
  unique versions, not contiguous, so the gap is legal.)
- **(E) Schema-as-contract.** Every new table (`panel_gate_config`, `factor_freshness_sla`,
  `panel_rowcount_anomaly`, `lineage_completeness_checks`, `maintenance_schedule`) lands with a `table_catalog`
  row + `field_catalog` rows in its migration; the S10 drift check fails on any uncatalogued table.
- **(F) Backup-before-migrate.** No live apply of 0180–0183 runs without a scripted CHECKPOINT + timestamped
  DB+WAL backup first (operator step; the offline suite uses the template-copy store).
- **(G) Quality-gated.** The panel checks authored `severity=critical` in `panel_gate_config` are wired into the
  orchestrator and **halt** the affected run through the *same* `summarize_quality_gate` decision pf2-S10 defined
  — same registry + same recorded facts → same halt decision (deterministic). Gating is opt-in per run
  (`gate=True`), preserving today's ungated content-sprint tests.
- **(J) Semantic contract.** The gated set includes S10's `factor_panel_export_contract` (unit/sign/scale +
  universe-as-of + zero-lookahead) — S2 elevates it, alongside S1's leakage/coverage, to the panel-critical set;
  it does not weaken any column's declared domain.

---

## Tasks

### S2-1 — Factor-panel quality gated in the orchestrator *(extends pf2-S10)*

**Root cause:** baseline 1 — the factor-panel dataset is outside pf2-S10's gated set; S1's leakage/coverage DQC
and S10's export-contract check are recorded rows, not halts, and there is no data-driven registry of which
panel checks are gate-critical.

**Fix.** Add a `panel_gate_config` registry (migration **0180**) mapping the factor-panel `dataset_id` → the set
of critical check names + a **retunable per-run severity** (the guardrail against false-halt flakiness: a check
can be demoted to `error`/`warning` without a code deploy). Add a panel-gate **assembler**
`evaluate_panel_gate` in `db/observability.py` that **reuses** pf2-S10's `run_warehouse_quality_checks` +
`summarize_quality_gate`: it runs the live schema/export checks for the dataset, folds in the **latest recorded**
`data_quality_checks` outcome for each configured check (this is how it consumes PF4-S1's leakage/coverage,
which S1's evaluator records), re-tags each configured check's severity to the `panel_gate_config` value, and
returns a `GateResult` through the *unchanged* `summarize_quality_gate` decision. Wire a small **additive** branch
into `DatasetOrchestrator._run_step`: when `gate=True` and the current dataset has an enabled `panel_gate_config`
row, evaluate through `evaluate_panel_gate` and use `panel_quality_gate_halt` / `panel_quality_gate_degrade` /
`panel_quality_gate_warn` audit labels; otherwise the pf2-S10 path is untouched. A `critical` panel failure
raises `QualityGateError` (step → `failed`, run halts, downstream `pending`); `error` → run `partial`;
`warning` → record only.

**Migration 0180** (`db/migrations/bodies_0180_0183.py`):

```python
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
    # Seed the factor-panel critical set. The leakage/coverage check_names reconcile
    # to PF4-S1's landed DQC names at implementation time; the export-contract name is
    # PF3-S10's factor_panel.PANEL_EXPORT_GATE_CHECK_NAME.
    conn.execute(
        """
        INSERT OR REPLACE INTO panel_gate_config
            (dataset_id, check_name, severity, enabled, source, notes, updated_at)
        VALUES
            ('factor_panel', 'factor_panel_export_contract', 'critical', true, 'pf4_s2',
             'PF3-S10 export-boundary unit/sign/scale + universe-as-of + zero-lookahead gate.', now()),
            ('factor_panel', 'factor_panel_leakage_probe',   'critical', true, 'pf4_s2',
             'PF4-S1 t+0 leakage probe; reconcile check_name to the landed S1 DQC.', now()),
            ('factor_panel', 'factor_panel_coverage',        'critical', true, 'pf4_s2',
             'PF4-S1 factor coverage DQC; reconcile check_name to the landed S1 DQC.', now())
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
```

**Assembler** (append to `db/observability.py`; note the new imports at module top —
`from dataclasses import replace`, and `from .quality import QualityResult, Severity, _coerce_severity,
run_warehouse_quality_checks, summarize_quality_gate, GateResult`):

```python
def _latest_recorded_check(store, dataset_id, check_name):
    row = store.con.execute(
        """
        SELECT status, observed_value, threshold_value, table_name
        FROM data_quality_checks
        WHERE dataset_id = ? AND check_name = ?
        ORDER BY checked_at DESC
        LIMIT 1
        """,
        [dataset_id, check_name],
    ).fetchone()
    return row


def evaluate_panel_gate(store, dataset_id, *, record: bool = True) -> "GateResult":
    """Assemble the factor-panel halt decision from the panel_gate_config critical set.

    Reuses pf2-S10's run_warehouse_quality_checks (live schema/export checks) and
    summarize_quality_gate (the unchanged critical->halt / error->partial / warning->pass
    decision). PF4-S1's leakage/coverage outcomes are consumed from their recorded
    data_quality_checks rows; each configured check's severity is re-tagged to the
    panel_gate_config value so a check can be demoted without a deploy.
    """
    live = run_warehouse_quality_checks(store, record=record, dataset_ids=(dataset_id,))
    live_by_name = {result.check_name: idx for idx, result in enumerate(live)}
    results = list(live)
    config_rows = store.con.execute(
        """
        SELECT check_name, severity, enabled
        FROM panel_gate_config
        WHERE dataset_id = ?
        ORDER BY check_name
        """,
        [dataset_id],
    ).fetchall()
    for check_name, severity, enabled in config_rows:
        check_name = str(check_name)
        gate_severity = _coerce_severity(severity, "critical")
        if check_name in live_by_name:
            idx = live_by_name[check_name]
            results[idx] = replace(results[idx], severity=gate_severity)
            continue
        if not bool(enabled):
            continue
        recorded = _latest_recorded_check(store, dataset_id, check_name)
        if recorded is None:
            continue  # not yet evaluated this cycle -> treated as pass for the gate
        status, observed, threshold, table_name = recorded
        results.append(
            QualityResult(
                dataset_id=dataset_id,
                table_name=str(table_name or "data_quality_checks"),
                check_name=check_name,
                status=str(status),
                observed_value=None if observed is None else float(observed),
                threshold_value=None if threshold is None else float(threshold),
                details={"source": "panel_gate_config", "panel_gate": True},
                severity=gate_severity,
            )
        )
    return summarize_quality_gate(dataset_id, results)
```

**Orchestrator wiring** (`db/orchestrator.py`, additive). Add a helper and branch the existing gate hook —
the pf2-S10 fundamentals path is byte-identical when the dataset has no `panel_gate_config` row:

```python
def _panel_gate_dataset(self, dataset_id: str) -> bool:
    try:
        row = self.store.con.execute(
            "SELECT count(*) FROM panel_gate_config WHERE dataset_id = ? AND enabled",
            [dataset_id],
        ).fetchone()
    except Exception:
        return False
    return bool(row and row[0])
```

Inside `_run_step`, replace the `if gate:` body's evaluator selection + audit labels:

```python
if gate:
    panel_gated = self._panel_gate_dataset(dataset_id)
    if panel_gated:
        from .observability import evaluate_panel_gate
        gate_result = evaluate_panel_gate(self.store, dataset_id)
    else:
        gate_result = evaluate_quality_gate(self.store, dataset_id)
    gate_decision = gate_result.decision
    halt_action = "panel_quality_gate_halt" if panel_gated else "step_quality_gate_halt"
    degrade_action = "panel_quality_gate_degrade" if panel_gated else "step_quality_gate_degrade"
    warn_action = "panel_quality_gate_warn" if panel_gated else "step_quality_gate_warn"
    if gate_decision == "halt":
        # ...existing UPDATE etl_job_steps -> failed (unchanged)...
        record_audit(self.store, run_id=run_id, dataset_id=dataset_id, actor=self.actor,
                     action=halt_action, details=_quality_gate_details(gate_result), ts=failed_at)
        raise QualityGateError(dataset_id, gate_result)
    if gate_decision == "partial":
        record_audit(..., action=degrade_action, ...)
    elif gate_result.failed_count:
        record_audit(..., action=warn_action, ...)
```

**PIT:** (B) 0180 catalogued; `panel_gate_config` seeded with its `table_catalog`/`field_catalog` rows. (G) same
config + same recorded panel facts → same `summarize_quality_gate` halt (deterministic); the gate reads only
already-materialized panel rows + recorded outcomes (no lookahead). (J) the gated checks are exactly the
leakage / coverage / unit-sign-scale checks S1/S10 authored.

**TDD (`db/tests/test_panel_gating.py`).** A fixture registry with `factor_source → factor_panel →
factor_panel_export`; the panel step's `run()` is a no-op (the view is fixture-populated); "planting a leakage
breach" = inserting a `data_quality_checks` row for the configured `factor_panel_leakage_probe` check.

```python
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
        VALUES (?, 'factor_panel', 'v_factor_panel', 'factor_panel_leakage_probe',
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
        "WHERE dataset_id = 'factor_panel' AND check_name = 'factor_panel_leakage_probe'")
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
        "WHERE dataset_id = 'factor_panel' AND check_name = 'factor_panel_leakage_probe'")
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
```

**Accept:** a planted panel-`critical` leakage breach driven through `DatasetOrchestrator.run(gate=True)` **halts**
at the factor-panel dataset (`etl_job_runs.status='failed'`, `factor_panel` step `failed`, a
`panel_quality_gate_halt` audit row, `factor_panel_export` left `pending`); the same breach as `error` completes
`partial` (downstream `succeeded`, `panel_quality_gate_degrade` recorded); as `warning` completes `succeeded`
(`panel_quality_gate_warn` recorded); `gate=False` reproduces today's ungated walk with no `panel_quality_gate*`
audit; a clean panel passes.

### S2-2 — Factor observability (freshness SLA + panel anomaly + lineage-completeness)

**Root cause:** baseline 2 — `observability.py` has no *factor* freshness SLA, no *panel* row-count anomaly, and
no *lineage-completeness* surface, so a stale panel, a collapsed cross-section, or a factor that silently lost a
lineage edge (regressing surpass axis 1) is invisible.

**Fix.** Add three surfaces to `db/observability.py` (migration **0181** schema / **0182** index), all routing
through the pf2-S10 gated-check path (each emits a severity-tagged `data_quality_checks` row via the existing
`quality_check` primitive, so a breach can carry `severity=critical` and halt via S2-1). All three **reuse** the
existing primitives — S2 adds *factor* rows and *panel* baselines, it does not fork the evaluators.

**Migration 0181** (schema; `db/migrations/bodies_0180_0183.py`):

```python
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
```

**(1) Factor freshness SLA** (append to `observability.py`; reuses `_parse_timestamp`, `_slug`,
`_coerce_severity`, `_upsert_quality_registry`, `quality_check` — a factor-scoped sibling of
`evaluate_freshness_slas`):

```python
def evaluate_factor_freshness_slas(store, *, as_of=None) -> list[QualityResult]:
    """Evaluate factor/panel freshness SLAs against dataset_watermarks.

    Fresh datasets produce no rows; a panel staler than its SLA emits one
    severity-tagged data_quality_checks row that routes through the S2-1 gate.
    """
    if not _table_exists(store, "factor_freshness_sla"):
        return []
    as_of_ts = _parse_timestamp(as_of) or now_utc_naive()
    rows = store.con.execute(
        "SELECT dataset_id, max_lag_days, severity FROM factor_freshness_sla "
        "WHERE enabled ORDER BY dataset_id"
    ).fetchall()
    results: list[QualityResult] = []
    for dataset_id, max_lag_days, severity in rows:
        dataset_id = str(dataset_id)
        severity_value = _coerce_severity(severity, "warning")
        marks = store.con.execute(
            "SELECT watermark_value FROM dataset_watermarks WHERE dataset_id = ?", [dataset_id]
        ).fetchall()
        parsed = [_parse_timestamp(v) for (v,) in marks]
        latest = max((m for m in parsed if m is not None), default=None)
        max_lag = int(max_lag_days)
        lag_days = math.inf if latest is None else (as_of_ts - latest).total_seconds() / 86400.0
        if latest is not None and lag_days <= max_lag:
            continue
        check_name = f"factor_freshness_sla_{_slug(dataset_id)}"
        status = "warning" if severity_value == "warning" else "failed"
        observed = None if math.isinf(lag_days) else float(lag_days)
        details = {"as_of": as_of_ts.isoformat(), "max_lag_days": max_lag,
                   "latest_watermark": None if latest is None else latest.isoformat()}
        _upsert_quality_registry(
            store, check_name=check_name, dataset_id=dataset_id,
            table_name="dataset_watermarks", severity=severity_value,
            threshold_value=float(max_lag), comparator="le",
            failure_status="warning" if severity_value == "warning" else "failed",
            source="factor_freshness_sla")
        quality_check(store, dataset_id=dataset_id, table_name="dataset_watermarks",
                      check_name=check_name, status=status, severity=severity_value,
                      observed_value=observed, threshold_value=float(max_lag), details=details)
        results.append(QualityResult(
            dataset_id=dataset_id, table_name="dataset_watermarks", check_name=check_name,
            status=status, observed_value=observed, threshold_value=float(max_lag),
            details=details, severity=severity_value))
    return results
```

**(2) Panel row-count anomaly** (reuses `_median_mad` + `_robust_z`; injectable `cross_section_sizes` keeps the
unit test offline and cheap, while production reads live per-as-of sizes from `v_factor_panel`):

```python
@dataclass(frozen=True)
class PanelRowcountAnomaly:
    anomaly_id: str
    dataset_id: str
    as_of_date: str
    baseline_median: float
    baseline_mad: float
    observed_value: float
    z_score: float
    severity: Severity


def detect_panel_rowcount_anomaly(store, *, window: int = 4, z_threshold: float = 3.5,
                                  severity: Severity = "critical", dataset_id: str = "factor_panel",
                                  cross_section_sizes=None) -> PanelRowcountAnomaly | None:
    if window < 2:
        raise ValueError("window must be at least 2")
    if not _table_exists(store, "panel_rowcount_anomaly"):
        return None
    if cross_section_sizes is None:
        cross_section_sizes = store.con.execute(
            "SELECT as_of_date, count(*) FROM v_factor_panel GROUP BY as_of_date ORDER BY as_of_date"
        ).fetchall()
    series = [(str(d), float(n)) for d, n in cross_section_sizes]
    if len(series) <= window:
        return None
    prior = [n for _d, n in series[-(window + 1):-1]]
    as_of_date, observed = series[-1]
    median, mad = _median_mad(prior)
    z_score = _robust_z(observed, median, mad)
    if abs(z_score) < z_threshold:
        return None
    severity_value = _coerce_severity(severity, "warning")
    status = "warning" if severity_value == "warning" else "failed"
    anomaly_id = str(uuid.uuid5(uuid.NAMESPACE_URL, f"{dataset_id}:{as_of_date}:{observed}"))
    checked_at = now_utc_naive()
    details = {"window": window, "z_threshold": z_threshold, "baseline_values": prior,
               "as_of_date": as_of_date}
    store.con.execute(
        """
        INSERT OR REPLACE INTO panel_rowcount_anomaly (
            anomaly_id, dataset_id, as_of_date, baseline_median, baseline_mad,
            observed_value, z_score, is_anomaly, severity, details_json, checked_at
        )
        VALUES (?, ?, ?, ?, ?, ?, ?, true, ?, ?, ?)
        """,
        [anomaly_id, dataset_id, as_of_date, median, mad, observed, z_score,
         severity_value, json_dumps(details), checked_at],
    )
    check_name = f"panel_rowcount_anomaly_{_slug(dataset_id)}"  # 'count' keeps naming consistent
    quality_check(store, dataset_id=dataset_id, table_name="v_factor_panel", check_name=check_name,
                  status=status, severity=severity_value,
                  observed_value=abs(z_score) if math.isfinite(z_score) else float("inf"),
                  threshold_value=float(z_threshold), details=details)
    return PanelRowcountAnomaly(anomaly_id, dataset_id, as_of_date, median, mad, observed,
                                z_score, severity_value)
```

**(3) Lineage-completeness** (each emitted panel factor must resolve every edge; injectable `panel_factors`
keeps the test offline; production reads distinct `factor_id` + `input_lineage_json` from `v_factor_panel` and
existence in `factor_definition`. The standardization/vintage edge names reconcile to pf3's landed lineage
surfaces):

```python
_LINEAGE_EDGES = ("source_fact", "formula", "standardization_rule", "vintage")


def evaluate_lineage_completeness(store, *, panel_factors=None,
                                  required_edges=_LINEAGE_EDGES, severity: Severity = "critical"):
    if not _table_exists(store, "lineage_completeness_checks"):
        return []
    if panel_factors is None:
        rows = store.con.execute(
            "SELECT DISTINCT factor_id, input_lineage_json FROM v_factor_panel "
            "WHERE input_lineage_json IS NOT NULL ORDER BY factor_id"
        ).fetchall()
        panel_factors = [(str(f), lineage) for f, lineage in rows]
    severity_value = _coerce_severity(severity, "critical")
    checked_at = now_utc_naive()
    failures = []
    for factor_id, lineage_json in panel_factors:
        try:
            lineage = json.loads(str(lineage_json)) if not isinstance(lineage_json, dict) else lineage_json
        except Exception:
            lineage = {}
        has_definition = bool(store.con.execute(
            "SELECT count(*) FROM factor_definition WHERE factor_id = ?", [str(factor_id)]
        ).fetchone()[0]) if _table_exists(store, "factor_definition") else True
        missing = [edge for edge in required_edges if not lineage.get(edge)]
        if not has_definition:
            missing.append("factor_definition")
        is_complete = not missing
        check_id = str(uuid.uuid5(uuid.NAMESPACE_URL, f"lineage:{factor_id}:{checked_at.isoformat()}"))
        store.con.execute(
            """
            INSERT OR REPLACE INTO lineage_completeness_checks (
                check_id, dataset_id, factor_id, missing_edges_json, is_complete,
                severity, details_json, checked_at
            )
            VALUES (?, 'factor_panel', ?, ?, ?, ?, ?, ?)
            """,
            [check_id, str(factor_id), json_dumps(missing), is_complete, severity_value,
             json_dumps({"required_edges": list(required_edges)}), checked_at],
        )
        if not is_complete:
            check_name = f"lineage_completeness_{_slug(str(factor_id))}"
            quality_check(store, dataset_id="factor_panel", table_name="v_factor_panel",
                          check_name=check_name, status="failed", severity=severity_value,
                          observed_value=float(len(missing)), threshold_value=0.0,
                          details={"factor_id": str(factor_id), "missing_edges": missing})
            failures.append((str(factor_id), missing))
    return failures
```

**PIT:** (B) 0181 catalogued, 0182 index split. (C) fixtures: a factor watermark past its SLA; a count series
`[4000,4010,3990,4000,40]`; one fully-traced + one edge-severed factor. (D) each surface is a pure function of
recorded history / injected series → deterministic z-score + deterministic missing-edge set.

**TDD (`db/tests/test_panel_gating.py`, continued).**

```python
def test_factor_freshness_sla_flags_stale_not_fresh(tmp_store):
    from db.observability import evaluate_factor_freshness_slas
    from db.quality import evaluate_quality_gate
    tmp_store.con.execute(
        "INSERT OR REPLACE INTO factor_freshness_sla (dataset_id, max_lag_days, severity, enabled, updated_at)"
        " VALUES ('panel_stale', 3, 'critical', true, ?), ('panel_fresh', 3, 'critical', true, ?)",
        [dt.datetime(2026, 7, 6, 12, 0, 0), dt.datetime(2026, 7, 6, 12, 0, 0)])
    for ds, val in (("panel_stale", "2026-06-01T00:00:00"), ("panel_fresh", "2026-07-05T00:00:00")):
        tmp_store.con.execute(
            "INSERT OR REPLACE INTO dataset_watermarks (dataset_id, watermark_name, watermark_value, updated_at)"
            " VALUES (?, 'max_available_at', ?, ?)", [ds, val, dt.datetime(2026, 7, 6, 12, 0, 0)])
    results = evaluate_factor_freshness_slas(tmp_store, as_of=dt.datetime(2026, 7, 6, 12, 0, 0))
    assert [r.dataset_id for r in results] == ["panel_stale"]
    assert results[0].status == "failed" and results[0].severity == "critical"
    gate = evaluate_quality_gate(tmp_store, "panel_stale", record=False, additional_results=results)
    assert gate.decision == "halt"


def test_panel_rowcount_anomaly_flags_collapse_not_stable(tmp_store):
    from db.observability import detect_panel_rowcount_anomaly
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
    from db.observability import evaluate_lineage_completeness
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
```

**Accept:** a stale factor watermark emits a freshness `data_quality_checks` row of the configured severity while
a fresh one emits nothing (and routes through the gate → halt); the `[4000,4010,3990,4000,40]` series flags the
`40` with a recorded `z_score` while a stable series flags nothing; an edge-severed factor surfaces a
`lineage_completeness_checks` failure while a fully-traced panel surfaces none.

### S2-3 — `maintenance_schedule` (cadence-as-data) + a dry-run planner

**Root cause:** baseline 3 — the S1 DAG *can* run incrementally, but *when* each dataset re-runs and its
backfill-window shape are not codified as data an operator or fresh agent can query and plan from.

**Fix.** Codify the cadence as data in `maintenance_schedule` (migration **0183**): `dataset_id` → incremental
`cadence` (daily bars, quarterly + on-filing fundamentals, on-restatement vintages, factor-panel rebuild
trigger) + a `backfill_window_json` shape. A thin `scripts/warehouse_schedule_plan.py` **reads** the schedule
and PLANS (dry-run) the operator archive — it composes, per dataset, the `run_backfill`/`run_maintenance` window
the `DatasetOrchestrator` *would* execute and returns/prints the plan **without touching the live DB** (no
writes, opens `read_only=True` when given a path).

**Migration 0183** (`db/migrations/bodies_0180_0183.py`):

```python
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
```

**Migration registry** (`db/migrations/registry.py`) — append after PF4-S1's file:

```python
from .bodies_0180_0183 import MIGRATIONS as _MIGRATIONS_0180_0183
MIGRATIONS = [ *_MIGRATIONS_0001_0137, ..., *_MIGRATIONS_0176_0179, *_MIGRATIONS_0180_0183 ]
```

and the new bodies module ends with:

```python
MIGRATIONS: list[Migration] = [
    Migration(version=180, name="pf4_s2_panel_gate_config", up=_pf4_s2_panel_gate_config),
    Migration(version=181, name="pf4_s2_factor_observability_schema", up=_pf4_s2_factor_observability_schema),
    Migration(version=182, name="pf4_s2_factor_observability_indexes", up=_pf4_s2_factor_observability_indexes),
    Migration(version=183, name="pf4_s2_maintenance_schedule", up=_pf4_s2_maintenance_schedule),
]
```

**Planner** (`scripts/warehouse_schedule_plan.py`, read-only):

```python
from __future__ import annotations
import argparse, json, sys
from pathlib import Path
from db.connection import DEFAULT_DB_PATH, connect


def plan_maintenance(store) -> list[dict]:
    """Return the dry-run maintenance plan; performs no writes."""
    rows = store.con.execute(
        "SELECT dataset_id, cadence, cadence_trigger, backfill_window_json, chunk, enabled "
        "FROM maintenance_schedule WHERE enabled ORDER BY dataset_id"
    ).fetchall()
    plan = []
    for dataset_id, cadence, trigger, window_json, chunk, enabled in rows:
        window = json.loads(window_json or "{}")
        mode = "backfill" if window.get("shape") in ("windowed", "rebuild") else "maintenance"
        plan.append({
            "dataset_id": str(dataset_id), "cadence": str(cadence),
            "trigger": None if trigger is None else str(trigger),
            "planned_mode": mode, "chunk": None if chunk is None else str(chunk),
            "backfill_window": window, "executes": False, "dry_run": True,
        })
    return plan


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description="Plan (dry-run) the warehouse maintenance archive.")
    parser.add_argument("--db-path", default=str(DEFAULT_DB_PATH))
    args = parser.parse_args(argv)
    with connect(Path(args.db_path), read_only=True) as store:  # read-only: never mutates live data
        plan = plan_maintenance(store)
    sys.stdout.write(json.dumps({"plan": plan, "executed": False}, sort_keys=True) + "\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
```

**PIT:** (B) 0183 catalogued. (H) the planner only *plans* the windowed/chunked backfill; it never executes and
never writes. (C) the dry-run runs against the template-copy store offline; the live archive is operator-run.

**TDD (`db/tests/test_panel_gating.py`, continued).**

```python
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
```

**Accept:** `maintenance_schedule` is queryable per dataset with its cadence + trigger + backfill-window shape
(daily bars, quarterly + on-filing fundamentals, on-restatement vintages, factor-panel rebuild trigger); the
planner returns a per-dataset dry-run plan (`executes=False`), opens the live DB `read_only=True`, and performs
zero writes.

### S2-4 — Suite green + ledger closeout

**Fix.** Run the full offline suite green in the worktree, then close the tranche. **From `atx-impl/`** (never
from `db/` — `db/calendar.py` shadows stdlib `calendar` and breaks collection):

```
python -m pytest atx-impl\db\tests\test_panel_gating.py -q
python -m pytest atx-impl\db\tests\test_observability.py atx-impl\db\tests\test_orchestrator.py \
    atx-impl\db\tests\test_quality_gating.py atx-impl\db\tests\test_migrations.py -q
python -m pytest atx-impl\db\tests -q
```

Then **append** a `WAREHOUSE_PARITY_TRANCHES.md` row (start/end SHA, domains touched, verification commands, the
operator live-DB smoke slot with exact per-dataset counts + `run_id`, caveats/next → PF4-S3 hardening) and
**update** `db/PARITY_GAP.md` to record that the factor panel is now orchestrator-gated + observable
(freshness/anomaly/lineage) with a codified maintenance schedule — flipping the "factor panel is ungated /
unobservable" gap and pointing the residual (live archive execution, S1 leakage/coverage check-name
reconciliation) at the operator + PF4-S3. Stage **explicit paths only** (never `git add -A`); commit trailer
EXACTLY `Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>`.

**Accept:** `python -m pytest atx-impl\db\tests -q` green; `WAREHOUSE_PARITY_TRANCHES.md` has a new PF4-S2 row;
`db/PARITY_GAP.md` reflects the gated + observable + scheduled panel.

---

## Sequencing & expected compounding

**S2-1 → S2-2 → S2-3 → S2-4.** S2-1 first — the panel gate is load-bearing: every S2-2 factor-observability
signal (freshness / anomaly / lineage) escalates through the *same* `summarize_quality_gate` halt path, so the
`panel_gate_config`-driven wiring must land before the observability surfaces route into it. S2-2 turns
`dataset_watermarks` + live cross-section sizes + the lineage chain into live *factor* monitors. S2-3 codifies
the cadence the operator + PF4-S6 activation harness will execute. **Compounding:** once panel-criticals halt
runs, S1's leakage/coverage DQC and S10's export contract become trustworthy SLO gates instead of write-only
rows; factor freshness + panel-collapse + lineage-completeness make a stale/collapsed/lineage-broken panel a
live signal instead of a silent regression; the codified schedule makes the maintenance cadence a queryable
fact PF4-S6 plans from and PF4-S11's runbook drives.

## Risks / guardrails

- **A gate halts a run it should not (false-halt flakiness).** Keep the panel-`critical` set small, curated, and
  data-driven in `panel_gate_config` (retunable to `error`/`warning` without a deploy); keep gating opt-in per
  run (`gate=True`) so content-sprint tests are unaffected. The gate must halt on **real** criticals only — a
  leaky factor, a lookahead, a unit/sign violation, a collapsed panel — never on transient slice sparsity.
- **Reuse, don't fork.** `evaluate_panel_gate` calls the *unchanged* `run_warehouse_quality_checks` +
  `summarize_quality_gate`; the three observability surfaces reuse `_median_mad`/`_robust_z`/`_parse_timestamp`/
  `quality_check`. A re-implemented evaluator or a copied halt path is a review-blocking defect.
- **S1 dependency reconciliation.** `factor_panel_leakage_probe` / `factor_panel_coverage` in `panel_gate_config`
  must reconcile to PF4-S1's landed DQC check_names; the gate consumes their recorded `data_quality_checks`
  outcomes and applies the configured severity — it never re-implements the leakage/coverage computation.
- **The planner must never mutate live data.** `scripts/warehouse_schedule_plan.py` opens `read_only=True`,
  performs zero writes, and plans only; the live archive is operator-run per scope decision #1.
- **Stay in 0180–0183.** Every new table catalogues in the same migration; schema (0181) split from index
  (0182) per the WAL precedent; timestamped DB+WAL backup before any live apply; strictly within the reserved
  range. Do not touch S7–S11 / PF4-S1 content bodies or S1's backfill engine.

## Bench / acceptance

- **Panel gate halts on a planted panel-critical:** planted leakage breach through `run(gate=True)` halts at
  `factor_panel` (`etl_job_runs.status='failed'`, `panel_quality_gate_halt` audit, downstream `pending`);
  `error`→`partial`; `warning`→`succeeded`; `gate=False` reproduces the ungated walk; a clean panel passes.
- **Factor observability surfaces:** a stale factor watermark → a severity-tagged freshness row that halts via
  the gate; `[4000,4010,3990,4000,40]` → a recorded `z_score` anomaly (stable → none); an edge-severed factor →
  a `lineage_completeness_checks` failure (fully-traced → none).
- **Schedule + planner:** `maintenance_schedule` queryable per dataset with cadence + backfill-window; the
  planner dry-runs (`executes=False`, `read_only=True`) with zero writes.
- **Full pytest green offline:** `python -m pytest atx-impl\db\tests\test_panel_gating.py -q` green, and full
  `python -m pytest atx-impl\db\tests -q` green before commit.
- **Live smoke recorded (operator):** a gated live run with the halt exercised on a deliberately-tripped
  panel-critical then reverted, a factor-freshness + panel-anomaly + lineage sweep, and the schedule planner
  dry-run — recorded in the ledger with `run_id` + exact per-dataset/panel counts.
- **Ledger:** `db/PARITY_GAP.md` updated; a PF4-S2 `WAREHOUSE_PARITY_TRANCHES.md` row appended.

**Process:** own git worktree off `main` via `atx-impl/scripts/new_db_worktree.sh new|finish
pf4-s2-panel-gating`; controller `superpowers:subagent-driven-development` (fresh implementer + reviewer per
task; TDD + verification-before-completion). Never `git add -A` (stage explicit paths); never push unless asked.
New module ⇒ new `test_*.py`. `python -m pytest atx-impl\db\tests -q` green in the worktree before every commit —
**run from `atx-impl/`, never from `db/`**. Update `PARITY_GAP.md` and append a `WAREHOUSE_PARITY_TRANCHES.md`
row. Commit trailer EXACTLY `Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>`.
