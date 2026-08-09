# Sprint PF3-S1 — Backfill + incremental-maintenance DAG

**Goal:** turn the dataset-scoped rebuild orchestrator into a production backfill platform. Extend pf1-S2's `DatasetOrchestrator` into (a) a windowed / chunked / **resumable historical backfill** engine that walks a dataset's history in bounded partitions, and (b) an **incremental maintenance** mode that advances only the stale tail. Add **per-partition watermarks** so backfill progress survives a crash, **bounded parallel fan-out** so independent partitions run concurrently without exhausting the single-writer DB, a **dead-letter + retry** path so one poisoned partition does not sink a multi-year run, and **DAG-run observability** so per-partition status and watermark are queryable. This is the first pf3 sprint and it *defines* clause **(H) Backfill-safe**. Reserved migrations 0132–0134.

**Mandate / Owns:** NEW `db/backfill.py` (windowed/chunked partition planner + backfill/maintenance drivers over `DATASET_REGISTRY`), a backfill/maintenance extension to `db/orchestrator.py` (`DatasetOrchestrator` gains backfill + maintenance entry points wired through the existing `etl_job_*` manifest write path), NEW `scripts/warehouse_backfill.py` (operator CLI: `backfill` / `resume` / `status`), and `db/tests/test_backfill.py`.

**Must NOT touch:** the **factor / panel surfaces** (PF3-S7–S11: `db/factors/`, `db/factor_panel.py`, `db/signal_eval.py`, `v_factor_panel`) — S1 lays rails, it does not build factors — and the **schema-contract semantic layer** (PF3-S2: `db/schema_contract.py` v2, unit/sign/scale contracts, the 56-table PIT-gap close). S1 provides the resumable, idempotent windowing primitives that **PF3-S4's price backfill rides on**; it does not itself densify any content surface. Do not edit any landed migration (≤ 0131) or another sprint's reserved region.

**Depends on:** pf1-S2's orchestrator (`DatasetOrchestrator`, `DATASET_REGISTRY`, `etl_job_runs`/`etl_job_steps`/`etl_job_audit`, `dataset_watermarks`, `RetryPolicy`, `resume()`) and pf2 governance (clauses E–G: schema-as-contract, backup-before-migrate, quality-gating) **only**. First pf3 sprint; nothing pf3 precedes it.

---

## Baseline / where the cycles go

The orchestrator is a mature *rebuild* engine but has no concept of a bounded historical window; it drives whole datasets, not partitions of a dataset's history. Measured 2026-07-04 against `atx-impl/db/orchestrator.py`.

1. **`DatasetOrchestrator` is a dataset-id DAG for REBUILD, not a windowed backfill.** `DatasetOrchestrator` (`orchestrator.py:361`) topologically orders `DATASET_REGISTRY` into a `DatasetDAG`, then `run()` / `resume()` execute **one step per dataset** — the unit of work is a whole dataset id, recorded once in `etl_job_steps` (`INSERT INTO etl_job_steps`, `orchestrator.py:296`). There is no notion of "dataset D over `[2014-01-01, 2014-04-01)`" as an addressable, independently-schedulable unit. A five-year backfill is today a single opaque step that either finishes or does not.

2. **Watermarks drive incremental SKIP, but there is no per-partition backfill progress.** `_watermark_snapshot` / `_watermarks_for` read `dataset_watermarks` (`orchestrator.py:585`), `_incremental_since` (`orchestrator.py:235`) collapses upstream marks into a single `incremental_since` param, and `resume()` short-circuits a step whose status is already `succeeded`/`skipped` (`orchestrator.py:483`). This gives a **forward-tail** watermark per dataset — enough to skip an already-current dataset — but nothing records "partitions 2014Q1…2019Q4 done, 2020Q1 in-flight, 2020Q2+ pending." Resume is coarse: a crashed multi-year step restarts the whole dataset.

3. **The proof-slice posture means no real backfill has ever run.** `equity_daily_bars` holds ~3.18M rows for **2012–2014 only**; companyfacts fundamentals are 2017–2026 — the near-empty overlap is exactly why pf2-S9 valuation multiples emit so few rows. The orchestrator has never been exercised against a genuine multi-year windowed load, so the rails a real backfill needs (bounded windows, per-partition resume, dead-letter) do not yet exist to be exercised.

4. **No dead-letter / bounded-parallelism / resumable-window primitives.** Retries are per-step and in-process: `RetryPolicy` (`orchestrator.py:79`) with `retry_delay_seconds` retries a step a fixed number of times, then `OrchestratorRunError` aborts the run. There is no **dead-letter** sink to quarantine a permanently-failing partition and continue, no **bounded-parallel** fan-out (steps run sequentially in topological order), and no **window-level** resume cursor — only dataset-level status.

**Already good — do not regress:**
- **The deterministic dataset DAG.** `DatasetDAG` construction, `CycleError`/`MissingDependencyError` validation, and the topological `order` over `DATASET_REGISTRY` stay the substrate. Backfill windows attach *under* a dataset node; they do not reorder the DAG.
- **The `etl_job_*` manifest trail.** `etl_job_runs` / `etl_job_steps` / `etl_job_audit` (`orchestrator.py:272`/`:296`/`:337`) and the `RunManifest` remain the run-level record; per-partition state is a **new** sibling surface that references `run_id`, never a rewrite of the existing manifests.
- **Retry / backoff / resume-by-`run_id`.** `RetryPolicy` + `retry_delay_seconds`, the `resume()` re-entry that flips `etl_job_runs` back to running and skips `succeeded`/`skipped` steps, and `WINDOW_WATERMARK_HINTS` / `INCREMENTAL_WINDOW_PARAM_KEYS` param injection all carry forward — S1 extends them to partition granularity, it does not replace them.

---

## PIT / determinism + production contract

Clauses **(A)** bitemporal correctness, **(B)** append-only catalogued migrations, **(C)** offline/no-network tests, **(D)** determinism/provenance, and **(H)** backfill-safe all apply; **(H) is DEFINED by this sprint.**

- **(H)** Every backfilled surface is windowed, chunked, resumable, and idempotent: re-running a completed window is a **no-op**, a partial window resumes without duplication, and per-partition watermarks record progress — no unbounded full-table rewrite. S1 is the first, load-bearing implementation of this clause; PF3-S4's price backfill and every later content backfill inherit it.
- **(A)** Each partition's produced rows keep the standard bitemporal columns; a partition's completion watermark advances only on inputs whose `available_at` is within the window. Backfill never post-dates a partition ahead of its data.
- **(B)** Migrations **0132–0134** only (integer versions 132–134; never renumber, never edit ≤ 0131), **schema / index / view split** per the pf2-S2 WAL precedent: **0132** creates `backfill_watermark` + `backfill_run` (+ a dead-letter table) with their `table_catalog` / `field_catalog` seed rows in the same migration; **0133** their indexes; **0134** the observability view. Timestamped DB+WAL backup precedes any live apply (clause F).
- **(C)** All tests run against in-memory / template-copy DuckDB with a fixture dataset registry and a synthetic multi-window history. No SEC / FRED / vendor network in pytest. The live multi-year backfill headline is operator-run and recorded in the ledger.
- **(D)** The partition planner is a pure function of `(dataset, window bounds, chunk size)` → deterministic ordered partition list; the same slice replays to the same rows. Partition execution records its input lineage against `run_id`.

---

## Tasks

### S1-0 — Backfill engine core *(the big one)*

**Root cause:** the orchestrator's unit of work is a whole dataset (fact 1); there is no addressable, resumable, idempotent *window* of a dataset's history, so a multi-year load cannot be planned, chunked, checkpointed, or safely re-run (fact 3).

**Fix:** NEW `db/backfill.py` exposing a **windowed / chunked partition planner** over `DATASET_REGISTRY` — `plan_backfill(dataset_id, start, end, chunk) -> list[Partition]` where each `Partition` is `(dataset_id, window_lo, window_hi, partition_key)` — plus a `run_backfill` driver that executes partitions in dependency-respecting order. Persist progress to a NEW `backfill_watermark` table `(dataset_id, partition_key, window_lo, window_hi, status, rows_written, watermark_after, run_id, updated_at)` and a NEW `backfill_run` header `(backfill_run_id, dataset_id, start, end, chunk, status, started_at, finished_at)` — migration **0132** (indexes **0133**), catalogued in the same migration per (B). **Resumability + idempotency are the contract:** before executing a partition the driver consults `backfill_watermark`; a partition already `succeeded` is **skipped** (re-running a completed window is a strict no-op), a partition left `running`/`failed` re-executes from its window bound without double-writing (partition writes are keyed idempotent, mirroring the dataset-level `resume()` short-circuit at `orchestrator.py:483`).

**PIT:** (D) planner is a pure deterministic function of window bounds + chunk. (B) 0132 schema+catalog, 0133 indexes. (H) completed-window no-op is the defining test.

**Accept:** on a fixture dataset with a synthetic 2014–2019 history, `plan_backfill` yields the exact deterministic partition list for a given chunk; a full `run_backfill` populates `backfill_watermark` to all-`succeeded`; a **second immediate run is a no-op** (0 rows re-written, all partitions skipped); killing the run mid-window and re-invoking resumes from the first non-`succeeded` partition with no duplicate rows.

### S1-1 — Incremental maintenance mode

**Root cause:** watermarks today only support a coarse dataset-level forward-tail skip (fact 2); there is no mode that detects *which partitions* have gone stale and schedules only those, so ongoing maintenance would either re-run everything or nothing.

**Fix:** add a **maintenance driver** to `db/backfill.py` that, for a dataset, compares each partition's `backfill_watermark.watermark_after` against the current upstream watermark (reusing `_watermark_snapshot` / `dataset_watermarks`, `orchestrator.py:585`) and schedules **only the stale partitions** — typically the trailing window(s) plus any partition whose upstream inputs have advanced. It emits the same `backfill_watermark` progress rows under a maintenance `backfill_run`. An **immediate re-run with no upstream advance is a no-op** — every partition's watermark already covers the current upstream, so nothing is scheduled.

**PIT:** (A) staleness is decided on `available_at`-bounded upstream watermarks, no lookahead. (D) stale-set selection is a deterministic function of recorded vs current watermarks. (H) no-op-on-no-change is the defining test.

**Accept:** on a fixture where one upstream partition advances, maintenance schedules exactly the affected partition(s) and leaves the rest untouched; an immediate second maintenance run with no upstream change schedules zero partitions (no-op); the maintenance path shares the S1-0 watermark table and never rewrites completed history.

### S1-2 — Orchestrator integration + bounded parallel fan-out + dead-letter / retry

**Root cause:** the backfill engine must run *through* the governed orchestrator (so it inherits `etl_job_*` manifests, retry, and the DAG order), but the orchestrator has no fan-out and no dead-letter, so one poisoned partition aborts a multi-year run (fact 4).

**Fix:** wire the S1-0/S1-1 drivers into `DatasetOrchestrator` via new backfill / maintenance entry points that record each backfill run in `etl_job_runs` / `etl_job_steps` under a `run_id` (extending, not rewriting, the manifest path at `orchestrator.py:272`/`:296`). Add **bounded parallel fan-out**: independent partitions of a dataset execute concurrently up to a `max_parallel` cap, with the cap defaulting low to respect DuckDB's single writer. Add a **dead-letter** table (migration **0132**) `(dataset_id, partition_key, run_id, error, attempts, dead_lettered_at)`: a partition that exhausts its `RetryPolicy` (`orchestrator.py:79`) is **quarantined** to the dead-letter sink and the run **continues** with the remaining partitions, rather than raising `OrchestratorRunError` for the whole run. Retry stays **exponential** off the existing `retry_delay_seconds` backoff.

**PIT:** (B) dead-letter table catalogued in 0132. (D) fan-out preserves per-partition determinism (parallelism changes timing, not output). (G) exhausted-retry → dead-letter is a gate-ready signal.

**Accept:** a fixture partition that always fails is dead-lettered after its retry budget while sibling partitions still complete and the run reaches a terminal state (not aborted); dead-lettered partitions are queryable with their error + attempt count; bounded fan-out with `max_parallel=N` never opens more than N concurrent partition writers; re-running after a dead-letter cause is fixed clears the quarantine and completes the partition.

### S1-3 — DAG-run observability + operator CLI

**Root cause:** even with per-partition state persisted, there is no single queryable surface answering "where is this backfill" — per-partition status, watermark, and dead-letter live in three tables — and no operator entry point to launch / resume / inspect a run.

**Fix:** migration **0134** adds `v_backfill_status` — a catalogued view joining `backfill_run` + `backfill_watermark` (+ the dead-letter table) into one row-per-`(dataset_id, partition_key)` surface exposing window bounds, status, `rows_written`, `watermark_after`, attempts, and dead-letter state — catalogued with its own `table_catalog` / `field_catalog` rows exactly like pf1-S4-3's `v_formula_registry`. Add NEW `scripts/warehouse_backfill.py` with three subcommands: **`backfill`** (`--dataset --start --end --chunk [--max-parallel]`) launches a windowed backfill through the orchestrator; **`resume`** (`--backfill-run-id`) re-enters an interrupted or partially dead-lettered run; **`status`** (`--dataset [--backfill-run-id]`) prints the resolved `v_backfill_status` rows. Live connectors stay behind injectable file options (clause C).

**PIT:** (B) 0134 view catalogued in-migration. (A) the view reports recorded watermarks; it does not compute forward. (C) CLI's live path is injectable; tests drive it over a fixture DB.

**Accept:** `v_backfill_status` returns per-partition status/watermark/dead-letter for a fixture run, filterable by dataset and `backfill_run_id`; `warehouse_backfill status` prints it; `warehouse_backfill resume` re-enters a killed run and drives it to terminal; the view carries its own catalog rows and passes the S1 catalog-completeness check.

---

## Sequencing & expected compounding

**S1-0 → S1-1 → S1-2 → S1-3.** S1-0 lays the windowed engine + `backfill_watermark`/`backfill_run` tables (load-bearing — everything reads the partition planner and progress state). S1-1 adds the maintenance mode on top of that same watermark surface (a maintenance driver with no backfill engine to schedule against would be hollow). S1-2 then runs both drivers *through* the governed orchestrator and adds the fan-out + dead-letter the engine needs to survive a real multi-year run. S1-3 exposes the whole thing as a queryable view + operator CLI last, once there is genuine per-partition state to observe. **Compounding:** everything downstream rides these rails — most immediately **PF3-S4's 2014→present price backfill**, then every later content densification, all inherit resumable/idempotent windowing, per-partition watermarks, dead-letter quarantine, and `v_backfill_status` observability for free rather than each reinventing a load loop.

---

## Risks / guardrails

- **Idempotency is the whole game.** A backfill that double-writes on resume, or re-runs a completed window, silently corrupts history and defeats clause (H). Partition writes must be keyed idempotent and the completed-window no-op must be a first-class test (S1-0 + S1-1 accept), not an afterthought.
- **Do not defeat the incremental-skip short-circuit.** The maintenance mode must schedule *only* stale partitions; it must not force a full re-backfill on every invocation, and it must not undermine the dataset-level `resume()` skip (`orchestrator.py:483`) that already keeps current datasets cheap.
- **Bounded parallelism must not exhaust the single writer.** DuckDB is single-writer; `max_parallel` fan-out must cap concurrent writers conservatively (default low) and back-pressure rather than pile writers onto one connection. Parallelism changes timing only — never per-partition output.
- **Stay in 0132–0134.** Schema (0132) / index (0133) / view (0134) split, strictly within the reserved range; never renumber or edit a landed migration; back up DB+WAL before any live apply.

---

## Bench / acceptance

- Offline pytest green: `python -m pytest atx-impl\db\tests\test_backfill.py -q`, and full `python -m pytest atx-impl\db\tests -q` green before commit.
- Backfill is **resumable + idempotent** on a fixture slice: a full run reaches all-`succeeded`; an immediate re-run is a strict no-op (0 rows re-written); a mid-window kill resumes from the first non-`succeeded` partition with no duplicates.
- Incremental maintenance re-run with no upstream advance is a **no-op** (0 partitions scheduled); a single upstream advance schedules exactly the affected partition(s).
- DAG state is **queryable**: `v_backfill_status` reports per-partition window/status/watermark/dead-letter; dead-lettered partitions surface with error + attempt count while sibling partitions still complete.
- **Live smoke** operator-run against a bounded real slice (backed up first, clause F) and recorded in the ledger: `backfill_run` header, per-partition `backfill_watermark` counts, any dead-letters, and the `run_id`.
- `PARITY_GAP.md` updated (clause H now defined/enforced; backfill rails available to S4); a `WAREHOUSE_PARITY_TRANCHES.md` row appended (start/end SHA, domains, verification commands, live smoke with exact counts + run_id, caveats/next → PF3-S4 price backfill rides these rails).

**Process:** each sprint runs in its own git worktree off `main` via `atx-impl/scripts/new_db_worktree.sh new|finish <slug>`; never `git add -A` (stage explicit paths); never push unless asked. Commit trailer EXACTLY `Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>`.
