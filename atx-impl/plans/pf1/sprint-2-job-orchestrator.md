# Sprint PF-S2 — In-Repo Job Orchestrator

**Goal:** replace the hardcoded sequential refresh loop with a pure-Python DAG engine:
dependency inference, watermark-driven incremental, retry/backoff, resume-from-checkpoint,
run manifests + audit. Reserved migrations `0065-0068`.

**Mandate / Owns (exclusive):** NEW `db/orchestrator.py`; `jobs.py` `refresh_quant_warehouse`
rewrite (the DAG driver — today this is the `run_all_enabled` → `enabled_job_order` pair at
`jobs.py:1641-1689`); the `etl_job_runs` / `etl_job_steps` / `etl_job_audit` tables (schema +
catalog); NEW `db/tests/test_orchestrator.py` (+ `db/tests/test_jobs_dag.py`).

**Must NOT touch:** other sprints' primary modules (PF-S1 `item_registry.py`, PF-S3
`fundamental_statements.py`/`fundamentals.py`, PF-S4 `formula_library.py`, PF-S5 identifier
modules, PF-S6 pricing/valuation, PF-S7 `xbrl_validation.py`, PF-S8 lineage regions). Do NOT
change any dataset's compute logic — only orchestration. Each `Dataset.run(store, options)`
materializer contract stays byte-identical; the option factories in `DATASET_REGISTRY` are
consumed, never edited.

---

## Baseline / where the cycles go (audited, `db/jobs.py` + `db/watermarks.py`)

| Sink | File : region | Gap | Class |
|---|---|---|---|
| **Order is a comment-graph topo sort, not an inferred DAG** | `jobs.py:1644` `enabled_job_order` + `jobs.py:1077` `seed_default_jobs` (every `register_job(..., dependencies=[...])` hand-lists upstreams as **job-name strings**) | Edges live in hand-maintained `dependencies=[...]` literals seeded per run; nothing derives them from what a dataset actually reads. Rename a job and a downstream silently loses its edge. | Missing DAG |
| **No incremental — every run is a full-table rewrite** | `jobs.py:1549` `dataset.run(self.store, options)` (each `run_dataset` call re-materializes the whole dataset); watermarks are recorded post-hoc, never consulted pre-run | `dataset_watermarks` is written by `refresh_warehouse_watermarks` (`watermarks.py:766`) but **read by nothing** in the run path — no delta window is derived from `max(available_at)` / `max(period_end)`. | Full rewrite |
| **Retry is per-dataset, not orchestrated; backoff is fixed-delay** | `jobs.py:1531-1597` (the `for attempt in range(1, max_attempts+1)` block inside `run_dataset`) | Retry exists at the single-step level with a flat `time.sleep(retry_delay_seconds)` — no exponential backoff, and a failure aborts the whole `run_all_enabled` list with no partial-progress record. | Weak retry |
| **No resume — a killed rebuild restarts from zero** | `jobs.py:1641` `run_all_enabled` (`[self.run_job(n) for n in self.enabled_job_order()]`) | The list comprehension holds no cursor; interruption loses all completed-step state. `etl_job_runs` rows exist per dataset but there is no run-level manifest tying them into one rebuild to resume from. | No resume |
| **No run manifest / audit** | `schema.py:1635` `etl_job_runs` (per-`job_run_id`, one dataset each) + `etl_job_events` | There is no parent "rebuild run" row, no per-step table keyed to it, and no append-only audit trail (who/when/what action). | No manifest/audit |
| **`max_retries` / `retry_delay_seconds` present but under-enforced** | `schema.py:1625-1626` (`etl_job_definitions`) + `schema.py:1645-1646` (`etl_job_runs`) | Columns are seeded (`seed_default_jobs` `retry_policy`) and honored only inside `run_dataset`'s local loop; no orchestrator-level policy, no backoff curve. | Unenforced field |

**What is already good — do not regress:**

- **`DATASET_REGISTRY` + `_option_factories` pattern** (`jobs.py:860-999`, the `_*_options`
  helpers): a stable `dataset_id → (DatasetClass, option_factory)` map. The orchestrator drives
  through this map unchanged — it is the sole entry point to compute.
- **`dataset_watermarks` already exists** (`schema.py` catalog row: *"Used for incremental/reload
  planning."*) and is populated by `WATERMARK_QUERIES` / `refresh_warehouse_watermarks`
  (`watermarks.py:11-799`). S2 *reads* these marks; it does not redefine them.
- **Per-dataset `run()` materializer contract** — `dataset.run(store, options) -> DatasetLoadResult`
  with `run_id` / `rows_loaded` / `source` / `details`. Idempotent by design; the orchestrator
  wraps it, never rewrites it.
- **`etl_job_runs` / `etl_job_events` control tables + `event()` logging** (`jobs.py:1620`) — kept;
  S2 adds a *parent* run + step layer above them, it does not delete the existing per-step rows.

---

## PIT / determinism contract (S2)

S2 inherits the shared ROADMAP contract (`atx-impl/plans/pf1/ROADMAP.md §Shared PIT/determinism`)
clauses **(A)–(D)**:

- **(A) Bitemporal correctness.** The orchestrator MUST NOT change any dataset's output. Incremental
  windows are **watermark-driven and PIT-safe**: the delta a step reloads is bounded by the
  upstream `available_at` / `as_of` marks already in `dataset_watermarks` — a value is never
  reloaded or made visible before every input's `available_at`. No lookahead is introduced by
  incremental planning.
- **(B) Append-only, catalogued migrations.** `0065` = `etl_job_runs`/`etl_job_steps`/`etl_job_audit`
  schema + `table_catalog`/`field_catalog` seed; `0066` = indexes (split schema/index per the
  S5g/S5k WAL-replay precedent). `0067`/`0068` reserved for follow-on (e.g. a resumable-run view +
  its catalog). Forward-only, idempotent (`CREATE TABLE IF NOT EXISTS`).
- **(C) Offline / no-network tests.** Every orchestrator test runs against in-memory DuckDB with
  **stub datasets** — a fake `Dataset` whose `run()` is deterministic, can be told to raise once
  (to exercise retry/resume), and touches no SEC/FRED/FINRA endpoint. No live connector on the test
  path.
- **(D) Determinism + provenance.** Given the same DAG, same watermarks, and same params, the
  orchestrator produces the same step order, same skip set, and same manifest shape. The topo sort
  is deterministic (stable tie-break by `dataset_id`), so two runs of an unchanged warehouse yield
  identical `etl_job_steps` sequences.

Simulate failure and resume entirely offline (no network in the test path).

---

## Tasks

### S2-0 — Declared dependency edges + inferred DAG (topo sort + cycle detection)

**Root cause:** Order today is a topo sort (`enabled_job_order`, `jobs.py:1644-1689`) over
**hand-listed** `dependencies=[...]` job-name strings seeded in `seed_default_jobs`
(`jobs.py:1077-1445`). The graph is real but its edges are maintained by hand in ~60 `register_job`
calls; nothing ties an edge to the data a dataset consumes, and the edges key on *job names*, not
`dataset_id`s.

**Fix:** Give each `Dataset` a declared `depends_on: tuple[str, ...]` of **`dataset_id`s** (class
attribute, default `()`; datasets keep compute unchanged — this is metadata only). In
`orchestrator.py` build the DAG from `DATASET_REGISTRY` + declared `depends_on`: adjacency, a
deterministic topological sort (Kahn or DFS with stable `dataset_id` tie-break), and cycle
detection that raises `CycleError` naming the offending path (reuse the visiting/visited pattern
already proven in `enabled_job_order`, `jobs.py:1672-1685`). This DAG — not the hardcoded
`register_job` order — determines execution order in `refresh_quant_warehouse`.

**PIT/determinism:** (D) — topo order is a pure function of declared edges; stable tie-break makes
it reproducible. No dataset output changes (A).

**Accept:** DAG for the seeded warehouse is inferred purely from `depends_on` and matches the
current known-good order for the seeded set; an injected cycle raises `CycleError` with the path;
`test_jobs_dag.py` asserts topo validity (every edge points backward in the order) offline.

---

### S2-1 — Run / step manifest tables + audit (migration 0065 schema, 0066 index)

**Root cause:** `etl_job_runs` (`schema.py:1635`) is one row *per dataset*; there is no parent
"rebuild" entity to attach steps to, and no append-only audit of orchestration actions. A rebuild
is currently just a Python list comprehension (`run_all_enabled`) with no durable manifest.

**Fix:** Migration `0065` creates three tables (each seeds `table_catalog` + `field_catalog` in the
same migration per (B)):

- `etl_job_runs` (parent manifest): `run_id`, `started_at`, `finished_at`, `status`
  (`running`/`succeeded`/`failed`/`partial`), `params_json`, `git_sha`.
- `etl_job_steps`: `run_id`, `dataset_id`, `status` (`pending`/`running`/`succeeded`/`failed`/`skipped`),
  `rows`, `started_at`, `finished_at`, `watermark_before`, `watermark_after`, `error`. One row per
  DAG node per run — the resume cursor.
- `etl_job_audit` (append-only): `actor`, `ts`, `action` (e.g. `run_start`, `step_skip_incremental`,
  `step_retry`, `run_resume`, `run_fail`), plus `run_id` / `dataset_id` context.

> **Naming note.** The existing per-dataset control table is also called `etl_job_runs`
> (`schema.py:1635`). S2's parent manifest reuses the roadmap-mandated name; the migration must
> reconcile this (either extend the existing table with a nullable `parent_run_id` + the manifest
> columns, or introduce the manifest under the mandated name and repoint the per-dataset rows as
> children via `parent_run_id`). Pick one in `0065` and document it; do not leave two conflicting
> `etl_job_runs` definitions.

Migration `0066` adds indexes (`etl_job_steps(run_id, status)` for the resume scan;
`etl_job_audit(run_id, ts)`). Seed the catalog rows for all three.

**PIT/determinism:** (B) append-only/idempotent, split schema/index; (D) manifest shape is
deterministic for a given DAG.

**Accept:** a rebuild writes exactly one `etl_job_runs` parent row, one `etl_job_steps` row per DAG
node, and an ordered `etl_job_audit` trail; catalog rows present for all three tables; migration is
re-runnable (idempotent) and index migration is separable from schema.

---

### S2-2 — Watermark-driven incremental

**Root cause:** `dataset_watermarks` is written post-run (`refresh_warehouse_watermarks`,
`watermarks.py:766`) but **read by nothing** before a run — so every `dataset.run()` is a full-table
rewrite even when nothing upstream moved.

**Fix:** Each dataset exposes an **incremental window** derived from `dataset_watermarks` — its own
`max(available_at)` / `max(period_end)` (or the `max_as_of_date` variants already emitted by
`WATERMARK_QUERIES`) and those of its upstreams. The orchestrator computes, per node: `since =
max(upstream available_at marks)`; passes the window into the option factory (a `since` /
`start_date` param the existing `_*_options` accept — e.g. `start_date`, `as_of_ts`) so only the
delta is reloaded. `--full-rebuild` (CLI flag → orchestrator param) overrides and forces a
whole-table pass. A node whose upstream watermarks are **unchanged since its last successful
`etl_job_steps.watermark_after`** is **skipped** (status `skipped`, audit `step_skip_incremental`).
Windows are computed only from watermarks — never widened past an input's `available_at`.

**PIT/determinism:** (A) — the delta window is bounded by upstream `available_at`; never reload
before-available data, no lookahead. (D) — `input_codes`/window recorded per step so same
watermarks → same skip set.

**Accept:** with all watermarks unchanged, a second run skips every already-current node (all steps
`skipped`); moving one upstream watermark re-runs exactly that node's downstream closure;
`--full-rebuild` re-runs everything regardless; a watermark test proves no needed reload is skipped.

---

### S2-3 — Retry / backoff + resume-from-checkpoint

**Root cause:** Retry today is a flat-delay loop inside `run_dataset` (`jobs.py:1531-1597`,
`time.sleep(retry_delay_seconds)`); the schema `max_retries` / `retry_delay_seconds` fields
(`schema.py:1625-1626`, `1645-1646`) drive only that local loop. There is **no exponential
backoff** and **no resume** — `run_all_enabled` holds no cursor.

**Fix:** In the orchestrator, per step, enforce the existing `max_retries` with **exponential
backoff** (`delay = retry_delay_seconds * 2**(attempt-1)`, optionally capped; the base delay is
injectable and set to 0 in tests so retries are instant and deterministic). On each attempt write
an `etl_job_audit` `step_retry` row. **Resume-from-checkpoint:** given an existing `run_id`, read
`etl_job_steps` and re-run only steps whose status is `pending` or `failed` (nodes already
`succeeded`/`skipped` are left untouched); an interrupted rebuild resumed on the same `run_id`
touches only unfinished nodes. The retry counter is a deterministic function of the injected clock —
no wall-clock nondeterminism in tests.

**PIT/determinism:** (C)+(D) — backoff base injectable (0 in tests) → deterministic, offline-testable;
resume touches no already-committed dataset (A/idempotent `run()`).

**Accept:** a step told to fail `max_retries` then succeed is retried with growing (injected) delays
and one `step_retry` audit row per attempt; a rebuild interrupted mid-DAG and resumed on its
`run_id` re-runs only `pending`/`failed` steps and completes; all offline.

---

### S2-4 — Rewrite `refresh_quant_warehouse` to drive the orchestrator; keep the CLI stable

**Root cause:** The rebuild entry point is `run_all_enabled` (`jobs.py:1641`) — a list comprehension
over `enabled_job_order()` with no manifest, no incremental, no resume. `scripts/warehouse_jobs.py`
`run-all` (`warehouse_jobs.py:162`) calls it directly.

**Fix:** Rewrite `refresh_quant_warehouse` (the new name for the rebuild driver in `jobs.py`) to:
build the DAG (S2-0), open an `etl_job_runs` manifest with the current `git_sha` (S2-1), walk nodes
in topo order applying incremental skips (S2-2) and retry/backoff/resume (S2-3), and finalize the
manifest (`succeeded` / `partial` / `failed`). Keep `scripts/warehouse_jobs.py`'s CLI **stable**:
`run-all` now delegates to `refresh_quant_warehouse`; add `--full-rebuild` and `--resume <run_id>`
flags without changing existing subcommand names or output keys. On any unrecovered step failure:
exit **non-zero** and write a terminal `etl_job_audit` `run_fail` row (partial manifest preserved
for resume). `run_dataset` / `run_job` remain for single-dataset operator use.

**PIT/determinism:** (D) — `git_sha` + params captured in the manifest make each rebuild reproducible
and auditable; no dataset output change (A).

**Accept:** `warehouse_jobs.py run-all` drives the orchestrator and its existing JSON output keys are
unchanged; `--full-rebuild` and `--resume` work; a forced step failure yields a non-zero exit and a
`run_fail` audit row; the parent manifest reflects `partial`/`failed`.

---

## Sequencing & expected compounding

1. **S2-0 (DAG) + S2-1 (manifests) first** — the DAG defines order; the manifest/step/audit tables
   are the substrate every later task writes to. Land together.
2. **S2-2 (incremental) + S2-3 (retry/resume)** — both build on the step table: incremental writes
   `skipped` steps and `watermark_before/after`; resume reads step status. Independent of each
   other, land after S2-0/S2-1.
3. **S2-4 (wire)** last — folds all four into `refresh_quant_warehouse` and the CLI.

**Compounding:** PF-S3 and PF-S6 ship **wide backfills** (full concept dictionary; 2015+ daily
bars). Once S2 lands, those become **incremental + resumable** — a 1,400-security concept sweep or a
decade of bars can be interrupted and resumed, and re-runs skip unchanged securities instead of
rewriting the whole table. Every downstream sprint's rebuild becomes auditable through one manifest.

---

## Risks / guardrails

| Risk | Mitigation |
|---|---|
| **Incremental window skips a reload that was actually needed** (watermark stale or coarser than the real delta). | A `WatermarkSkip_ReloadsNeededDelta` test that moves an upstream mark and asserts the downstream node re-runs; the `--full-rebuild` escape hatch always forces a whole-table pass; skip decisions are logged to `etl_job_audit` for post-hoc audit. |
| **Resume double-writes** a step that had partially committed. | `Dataset.run()` is idempotent by contract (full replace of its own rows); the resume guard re-runs only `pending`/`failed` steps and never `succeeded`/`skipped`; a `ResumeIdempotent_NoDoubleWrite` test asserts row counts are identical whether run straight-through or interrupted-then-resumed. |
| **Two `etl_job_runs` definitions collide** (existing per-dataset table vs. new parent manifest). | Reconcile in migration `0065` (parent/child via `parent_run_id`); pick one representation and catalog it; do not leave conflicting DDL (see S2-1 naming note). |
| **Backoff sleeps make tests slow/flaky.** | Backoff base delay is injectable and set to 0 in tests; the retry counter is driven by an injected clock, not wall time. |
| **Dataset output drifts under orchestration.** | S2 touches no `compute_*` / `run()` body; a golden run of the seeded warehouse must produce identical dataset rows with the orchestrator vs. the old loop. **No dataset output change.** |

---

## Bench / acceptance

- DAG is **inferred from declared `depends_on`** — no hardcoded `register_job` order needed to
  produce a valid execution order.
- An **interrupted rebuild resumes** on its `run_id` and touches only `pending`/`failed` steps.
- An **incremental run skips unchanged datasets** (all-current second run → every step `skipped`);
  moving one watermark re-runs exactly its downstream closure; `--full-rebuild` overrides.
- **Every run is recorded**: one `etl_job_runs` parent, one `etl_job_steps` row per node, an ordered
  `etl_job_audit` trail; failures exit non-zero with a `run_fail` audit row.
- `python -m pytest atx-impl\db\tests\test_orchestrator.py atx-impl\db\tests\test_jobs_dag.py -q`
  green **offline**; full `python -m pytest atx-impl\db\tests -q` green before commit.
- Update `PARITY_GAP.md` status and append a ledger row to `WAREHOUSE_PARITY_TRANCHES.md` (start/end
  SHA, verification commands, live-DB smoke with exact counts + `run_id`, caveats/next).

**Process:** never `git add -A` (stage explicit paths); never push unless asked; commit trailer
EXACTLY `Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>`.

---

## Out of scope

Item dimension / metric registry (PF-S1); XBRL concept coverage (PF-S3); formula library (PF-S4);
identifier spine (PF-S5); pricing/valuation multiples (PF-S6); XBRL validation hardening (PF-S7);
restatement lineage (PF-S8). S2 changes **orchestration only** — no dataset compute, no ratio math,
no schema outside the reserved `0065-0068` range.
