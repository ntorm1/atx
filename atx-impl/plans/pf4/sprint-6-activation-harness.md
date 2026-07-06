# Sprint PF4-S6 — Activation harness + dense price backfill runbook

**Goal:** turn "migrate → backfill → rebuild" from a hand-run sequence of loose operator commands into a
single **resumable, evidenced, operator-gated activation harness**. Build a thin `scripts/` driver + a `db/`
harness library over the PF3-S1 backfill DAG that (a) **DRY-RUN PLANS** the historical price backfill widening
`equity_daily_bars` back to **2004+** and the full migrate→backfill→rebuild sequence **without touching the
live DB or the network**, and (b) on **explicit per-step operator go**, executes it **backup-first**, with
per-partition watermarks, live-count evidence recording, and a verification report. The live 14 GB archive run
is **OPERATOR-PENDING per scope decision #1** — this sprint makes it push-button and auditable, it does **not**
autonomously mutate production data. Resumability + idempotency are **fixture-proven on a bounded slice**.
Reserved migrations **0192–0194**.

**Mandate / Owns:** NEW `db/activation.py` (the harness library: `plan_activation` dry-run planner,
`apply_activation` operator-gated executor, `verify_activation` reporter, `record_activation_run` evidence
writer, deterministic plan hashing, throwaway-worktree guard), NEW `scripts/warehouse_activate.py` (operator
CLI: `plan` / `apply` / `verify` subcommands, mirroring `scripts/warehouse_backfill.py` and
`scripts/warehouse_migrate.py`), migrations **0192–0194** (activation evidence tables, `v_activation_status`
view, indexes + catalog), and `db/tests/test_activation_harness.py`. `db/activation.py` composes existing
primitives — `db.backfill.plan_backfill` / `run_backfill`, `db.migration_admin.run_governed_migrations`,
`db.orchestrator.DatasetOrchestrator.run(gate=True)` — it does not reimplement them.

**Must NOT touch:** the **backfill engine** (`db/backfill.py` planner/driver/watermark logic — S6 *calls* it,
never edits it), the **migration governance primitives** (`db/migration_admin.py` checkpoint/backup/apply/verify
— S6 *invokes* `run_governed_migrations`, never rewrites it), the **orchestrator** step/gate machinery
(`db/orchestrator.py` — S6 *drives* it via the public `run`/`run_backfill`/`resume_backfill` entry points), and
every **content engine** (`db/pricing_bulk.py`, `db/universe.py`, `db/factors/`, `db/factor_panel.py`). S6 is a
**composition + evidence + operator-safety** layer; it adds no new content transform. Do not edit any landed
migration (≤ 0191) or another sprint's reserved region.

**Depends on:** PF3-S1's backfill rails (`plan_backfill`, `run_backfill`, `Partition`, `backfill_run` /
`backfill_watermark` / `backfill_dead_letter`, `v_backfill_status`), PF3-S4's price-backfill client
(`BulkBarsBackfillDataset` / `BulkBarsBackfillOptions`, `price_backfill_partition`,
`v_price_fundamental_overlap`), PF2-S2's migration governance (`run_governed_migrations`, `backup_database`,
`restore_database`, `BackupArtifact`, the `migration_backup_registry`), PF2-S10's orchestrator quality gate
(`DatasetOrchestrator.run(gate=True)`), and PF4-S4/S5 landed ahead of it (survivorship returns + multi-universe
feed the rebuild the harness drives). Reconcile to landed names where PF4-S4/S5 differ.

---

## Baseline / where the cycles go

Every rail this sprint needs already exists as a **discrete primitive**; what is missing is a single governed
surface that *sequences* them, *records evidence*, and is *safe to hand an operator*. Measured 2026-07-06
against the tree.

1. **The activation sequence is folklore, not a driver.** Bringing the warehouse live today means an operator
   runs `scripts/warehouse_migrate.py` (governed migrate), then `scripts/warehouse_backfill.py backfill`
   (windowed price load), then `scripts/build_quant_warehouse.py` / `scripts/warehouse_rebuild.py` (rebuild),
   by hand, in order, remembering the flags. There is **no plan artifact** enumerating what *would* happen, **no
   single evidence row** proving what *did* happen, and **no resume cursor** spanning the three phases — a crash
   between migrate and backfill leaves no record of where the operator was.

2. **There is no dry-run.** `run_backfill` (`backfill.py:1029`) writes `backfill_run` + `backfill_watermark` on
   the first partition; `run_governed_migrations` (`migration_admin.py:356`) checkpoints, copies a `.bak`, and
   applies. Both are load-bearing and correct, but there is **no read-only path** that enumerates the
   partitions `plan_backfill` would produce, computes the pending-migration set, and estimates cost **without
   writing a byte**. An operator cannot see the blast radius before committing.

3. **`equity_daily_bars` is shallow and the overlap is empty.** The table holds ~3.18M rows for **2012–2014
   only** while fundamentals span 2017–2026, so `v_price_fundamental_overlap` — and every factor table
   downstream — is effectively empty (PARITY_GAP §6). Widening prices to **2004+** is the single highest-value
   data action left, but it is a multi-hour operator archive load, explicitly gated (scope decision #1). The
   harness must **plan** that load reproducibly and **prove the mechanism** on a bounded slice offline, then
   hand the live run to an operator with backup-first evidence.

4. **No activation is auditable or resumable across phases.** `backfill_run` records a *backfill*;
   `migration_backup_registry` records a *backup*; `etl_job_runs` records a *rebuild*. Nothing ties a `git_sha`
   + a plan hash + per-dataset before/after counts + the backup path into **one `activation_run` row** so that a
   completed activation is a queryable, re-enterable fact. Resumability today is *within* a backfill run, not
   *across* the migrate→backfill→rebuild arc.

**Already good — do not regress:**
- **The governed backup-then-apply primitive.** `run_governed_migrations` already does CHECKPOINT → timestamped
  `.bak` (+ WAL) → locked `apply_pending_migrations` → schema verify → **restore-on-failure**. The harness
  *calls* this for its backup+migrate step (clause F); it does not re-implement backup ordering.
- **Windowed, resumable, idempotent backfill.** `plan_backfill` is a pure deterministic partitioner;
  `run_backfill` skips already-`succeeded` partitions as a strict no-op and resumes non-terminal ones. The
  harness *rides* these; the completed-window no-op is the substrate of activation idempotency.
- **The gated rebuild.** `DatasetOrchestrator.run(gate=True)` halts on a `critical` check. The harness drives
  rebuild **through the gate**, never around it.

---

## PIT / determinism + production contract

Clauses **(A)** bitemporal, **(B)** append-only catalogued migrations, **(C)** offline/no-network tests,
**(D)** determinism/provenance, **(F)** backup-before-migrate, **(G)** quality-gated, and **(H)** backfill-safe
all apply. **(F)** and **(H)** are load-bearing here.

- **(F)** Every live apply is **backup-first**: `apply_activation` performs the backup+migrate step by calling
  `run_governed_migrations` (CHECKPOINT + timestamped `.bak` + WAL split + verify + restore-on-failure) **before
  any backfill partition writes a row**. The backup path + sha256 are recorded on the `activation_run` row. No
  backfill or rebuild runs until a verified backup exists.
- **(H)** The backfill phase is the PF3-S1 windowed/resumable/idempotent engine unchanged: a completed window
  re-runs as a strict no-op; a killed run resumes from the first non-`succeeded` partition; per-partition
  watermarks record progress. Activation idempotency = the union of migration forward-only idempotency + backfill
  no-op + a completed-`activation_run` short-circuit.
- **(D)** `plan_activation` is a **pure function** of `(datasets, window bounds, chunk, git_sha, pending
  migrations)` → an `ActivationPlan` with a deterministic `plan_hash` (sha256 over canonical JSON of the ordered
  step sequence + per-dataset partition lists). Same inputs → same hash; a different window → a different hash.
- **(C)** All tests run against in-memory / template-copy DuckDB with a fixture dataset registry and an
  **injected partition executor** (mirroring `test_backfill.py`) — no real 14 GB DB, no archive file, no network.
  The live archive activation is operator-run and recorded in the ledger.
- **(B)** Migrations **0192–0194** only (never renumber, never edit ≤ 0191): **0192** creates the activation
  evidence tables with their `table_catalog` / `field_catalog` seed rows + schema-contract-v2 pin refresh in the
  same migration; **0193** the `v_activation_status` view (catalogued); **0194** indexes + a catalog-completeness
  sweep. Split schema / view / index across the three numbers.
- **(A)** The plan enumerates windows; `apply` advances a partition watermark only on inputs whose `available_at`
  is within the window (inherited from `run_backfill`). Activation never post-dates a partition ahead of its
  data.

---

## Tasks

### S6-0 — Activation evidence schema + migrations 0192–0194

**Root cause:** no single surface ties a `git_sha` + plan hash + per-dataset before/after counts + backup path
into one auditable, re-enterable `activation_run` fact (fact 4); the three phase-local ledgers cannot answer
"what activation ran, did it finish, and where is its backup."

**Fix:** migration **0192** creates three append-only tables, each catalogued in the same migration per (B):
- `activation_run` — `(activation_run_id VARCHAR PK, git_sha, plan_hash, mode /* 'plan' | 'apply' */, status
  /* running|succeeded|partial|failed */, db_path, backup_path, backup_sha256, versions_before_json,
  versions_after_json, datasets_json, window_lo, window_hi, chunk, started_at, finished_at, error_message)`.
- `activation_dataset_evidence` — `(activation_run_id, dataset_id, rows_before BIGINT, rows_after BIGINT,
  partitions_planned, partitions_succeeded, partitions_skipped, partitions_failed, watermark_after,
  updated_at, PRIMARY KEY (activation_run_id, dataset_id))`.
- `activation_step_evidence` — `(activation_run_id, step_ordinal INT, step_name /* backup_migrate | backfill |
  rebuild | verify */, status, backfill_run_id, detail_json, started_at, finished_at, PRIMARY KEY
  (activation_run_id, step_ordinal))`.

Seed `table_catalog` (`layer='control'`) + `field_catalog` rows via `_catalog_fields_for_tables((...))` and call
`_refresh_schema_contract_v2_pin(conn)` — exactly the pattern in `bodies_0164_0167.py`. Add each as a
`Migration(version=192, name="pf4_s6_activation_evidence", up=...)` in a new `db/migrations/bodies_0192_0194.py`
wired into `db/migrations/registry.py`.

**PIT:** (B) 0192 = tables + catalog + contract pin in one migration; forward-only, idempotent
(`CREATE TABLE IF NOT EXISTS`, `INSERT OR REPLACE` catalog rows).

**Accept:** applying through 0192 on a fresh template DB creates all three tables; `test_schema_contract`
catalog-completeness passes (no uncatalogued activation table); the schema-contract-v2 pin still verifies; a
second apply of 0192 is a no-op.

### S6-1 — `plan_activation`: the dry-run planner (writes NOTHING) *(the big one)*

**Root cause:** there is no read-only path that shows an operator exactly what a migrate→backfill→rebuild would
do before it commits (facts 1–2); `run_backfill` and `run_governed_migrations` both write on first use.

**Fix:** NEW `db/activation.py` exposing `plan_activation(db_path, *, datasets, start, end, chunk,
registry=None, git_sha=None) -> ActivationPlan`. It:
1. Opens the target DB **`read_only=True`** (or, for `:memory:` fixtures, a connection it asserts never
   mutates) — a plan **must not** create the migration lock, checkpoint, or write a `.bak`.
2. Enumerates partitions per dataset with the **pure** `plan_backfill(dataset_id, start, end, chunk,
   registry=registry)` — for the price widening, `datasets=("bulk_daily_bars_backfill",)`, `start=2004-01-01`,
   `end=<today>`, `chunk="1q"` (default) — collecting the exact `Partition` list.
3. Computes the **pending-migration set** by reading `schema_migrations` (read-only) and diffing against
   `db.migrations.registry.MIGRATIONS` versions.
4. Resolves the ordered **step sequence**: `backup_migrate` → one `backfill` step per dataset →
   `rebuild(gate=True)` → `verify`.
5. Estimates cost: total partitions, windows per dataset, pending-migration count.
6. Computes `plan_hash = sha256(canonical_json(step_sequence + per_dataset_partition_keys + window bounds +
   chunk + pending_versions))`.

`ActivationPlan` is a frozen dataclass carrying all of the above. Emits the plan and returns; **zero DB writes**.

**PIT:** (D) planner is a pure deterministic function of its inputs → stable `plan_hash`. (C) reads-only; live
connectors stay behind the injectable `db_path`.

**Accept:** on a fixture DB with the `test_backfill`-style registry, `plan_activation` over `[2014-01-01,
2019-01-01)` with `chunk="1q"` returns the **exact** partition list `plan_backfill` yields for that window;
row counts of `activation_run`, `backfill_run`, `backfill_watermark`, and `equity_daily_bars` are **byte-for-byte
unchanged** before and after the call (assert counts equal); `plan_hash` is stable across two calls and
**changes** when the window or chunk changes; a `read_only=True` connection is used for a file-backed fixture.

### S6-2 — `apply_activation`: operator-gated backup-first executor

**Root cause:** executing the sequence live is unguarded, un-evidenced, and not resumable across phases (facts
1, 4); nothing enforces backup-before-migrate at the *activation* level or refuses a run from a throwaway
worktree.

**Fix:** `apply_activation(db_path, plan, *, operator_go, registry=None, executor=None, backup_dir=None,
clock=None, allow_worktree=False, source_options=None) -> ActivationRunResult`. Guards first:
- Raise `ActivationNotAuthorized` unless `operator_go is True` (the per-step operator go of scope decision #1).
- Raise `ActivationWorktreeRefused` if the target DB resolves inside a **linked git worktree** (detected by
  `<repo>/.git` being a *file*, not a directory) and `allow_worktree` is not True — "NEVER run apply from a
  throwaway worktree."
- Verify the passed `plan.plan_hash` recomputes against the current registry/DB (guards a stale plan).

Then execute, recording each step to `activation_step_evidence` and opening/finishing the `activation_run` row:
1. **Resume/idempotency short-circuit.** If an `activation_run` for this `plan_hash` exists with
   `status='succeeded'`, return it as a **no-op** (re-run of a completed activation = 0 rows rewritten). If it
   exists non-terminal, re-enter it (same `activation_run_id`) rather than opening a new one.
2. **Backup + migrate (clause F).** Call `run_governed_migrations(db_path, label="activation", backup_dir=...)`
   — CHECKPOINT + timestamped `.bak` (+WAL) + locked `apply_pending_migrations` + schema verify +
   restore-on-failure. Record `backup_path` + `backup_sha256` + `versions_before/after` on the `activation_run`
   row **before** any backfill. This is the load-bearing "backup happens first" invariant.
3. **Windowed backfill.** For each dataset, open `DuckDBStore(db_path)` and call
   `run_backfill(store, dataset_id, plan.window_lo, plan.window_hi, plan.chunk, registry=registry,
   params=source_options.get(dataset_id, {}), backfill_run_id=<activation-scoped id>, dead_letter=True,
   executor=executor, clock=clock)`. Record `rows_before`/`rows_after` (live `equity_daily_bars` counts pre/post)
   + partition counts + `watermark_after` into `activation_dataset_evidence`.
4. **Gated rebuild.** `DatasetOrchestrator(store, registry, actor="warehouse_activate", clock=clock).run(
   gate=True, full_rebuild=False)` to advance universe/overlap/factor-panel through the critical gate. Record
   rebuild evidence + run_id.
5. Finish `activation_run` (`succeeded` / `partial` if a partition dead-lettered), stamp `finished_at`.

Resumability: a kill mid-backfill leaves partition watermarks + a non-terminal `activation_run`; re-invoking
`apply_activation` with the same plan re-enters — applied migrations are forward-only no-ops, `succeeded`
partitions skip, the run finishes without duplication.

**PIT:** (F) `run_governed_migrations` backup precedes every backfill write. (H) backfill no-op + migration
idempotency = activation idempotency. (G) rebuild runs `gate=True`. (C) tests inject `executor` + a fixture
`registry`; no live archive.

**Accept (bounded fixture slice, injected executor):** `apply_activation` on `[2014-01-01, 2016-01-01)`
populates `activation_run` + per-dataset + per-step evidence and drives the fixture backfill to all-`succeeded`;
a **spy** proves the backup/migrate step ran **before** the first backfill partition write (record ordering);
killing the executor on partition N then re-invoking **resumes and completes with no duplicate rows**; a second
full re-run is a **strict no-op** (`activation_run` short-circuits, 0 partitions re-run); `operator_go=False`
raises `ActivationNotAuthorized` and writes nothing; a `db_path` under a `.git`-file worktree raises
`ActivationWorktreeRefused`.

### S6-3 — `verify_activation` + verification report + `v_activation_status`

**Root cause:** even with evidence recorded, there is no single report answering "did the activation land the
rows it planned, how far did each watermark advance, and did the price×fundamental overlap densify."

**Fix:** migration **0193** adds `v_activation_status` — a catalogued view joining `activation_run` +
`activation_step_evidence` + `activation_dataset_evidence` into one row-per-`(activation_run_id, dataset_id)`
surface exposing mode, status, plan_hash, git_sha, backup_path, per-dataset rows_before/after, partition
counts, watermark_after, and per-step status — catalogued with its own `table_catalog` / `field_catalog` rows
like `v_backfill_status`. Then `db/activation.py` gets `verify_activation(store, activation_run_id) ->
ActivationVerification`, which reads:
- **per-partition** counts from `backfill_watermark` (status histogram) + `price_backfill_partition`
  (`rows_loaded`, `min/max_trade_date`) for the run,
- **per-dataset** row counts (`equity_daily_bars` by `source`, `activation_dataset_evidence.rows_after`),
- **watermark progress** (`max(watermark_after)` vs `plan.window_hi`),
- **overlap-density** from `v_price_fundamental_overlap` (rows before/after),
and emits a structured report dict + the resolved `v_activation_status` rows.

**PIT:** (B) 0193 view catalogued in-migration. (A) the view reports recorded evidence; it computes nothing
forward. (C) driven over a fixture DB.

**Accept:** after the S6-2 fixture apply, `verify_activation` emits per-partition + per-dataset counts that
**match the fixture** (rows loaded == rows the injected executor wrote); watermark progress equals
`plan.window_hi`; the overlap-density report is present; `v_activation_status` returns the run's rows filterable
by `activation_run_id`; the report is deterministic (same DB → same report).

### S6-4 — Operator CLI `scripts/warehouse_activate.py` + runbook + worktree guard

**Root cause:** the harness must be an operator entry point, not a library call, and must document the gated
live runbook so a fresh operator can follow it safely.

**Fix:** migration **0194** adds the activation-evidence indexes (`idx_activation_dataset_evidence_run` on
`(activation_run_id, dataset_id)`, `idx_activation_step_evidence_run` on `(activation_run_id, step_ordinal)`,
`idx_activation_run_plan_hash` on `(plan_hash)`) + a catalog-completeness sweep + contract-pin refresh. NEW
`scripts/warehouse_activate.py` mirrors `warehouse_backfill.py`'s structure with three subcommands:
- **`plan`** (`--datasets --start --end --chunk [--db-path]`) → prints the `ActivationPlan` as JSON; **no
  writes** (opens `read_only=True`).
- **`apply`** (`--db-path --backup-dir [--datasets --start --end --chunk] --operator-go [--allow-worktree]
  [--source-zip/--source-file]`) → refuses without `--operator-go`; refuses from a linked worktree unless
  `--allow-worktree`; recomputes + prints the plan, then runs `apply_activation`; prints the run summary.
- **`verify`** (`--db-path --activation-run-id`) → prints `v_activation_status` rows + the verification report.

The module docstring carries the **operator runbook**: (1) `warehouse_activate plan …` and review the partition
count/estimate; (2) confirm a recent `.bak` exists / disk headroom; (3) `warehouse_activate apply … --operator-go`
from the **primary tree** (never a worktree) with the real archive `--source-zip`; (4) on interruption, re-run
the same `apply` to resume; (5) `warehouse_activate verify …` and record counts in the ledger.

**PIT:** (B) 0194 indexes + catalog sweep, strictly in-range. (C) CLI live path is injectable (`--db-path`);
tests drive `main([...])` over a fixture DB.

**Accept:** `warehouse_activate plan` over a fixture prints the plan and leaves `activation_run` /
`backfill_watermark` / `equity_daily_bars` row counts unchanged; `apply` without `--operator-go` exits nonzero
and writes nothing; `apply` against a `.git`-file worktree path is refused; `verify` prints the run's
`v_activation_status` rows; `main` is exercised via `test_activation_harness.py` with an injected registry +
executor.

### S6-5 — Ledger + parity closeout

**Root cause:** the sprint's live-vs-fixture posture and the operator-pending 2004+ backfill must be recorded so
the next agent + the operator know exactly what is proven and what still requires a gated live run.

**Fix:** append one `WAREHOUSE_PARITY_TRANCHES.md` row (status `committed`; domains = `db/activation.py`,
`scripts/warehouse_activate.py`, migrations `0192`–`0194`, `activation_run` / `activation_dataset_evidence` /
`activation_step_evidence` / `v_activation_status`, `db/tests/test_activation_harness.py`; verification = the
focused + full offline suites; **Live DB Notes = OPERATOR-PENDING**: no live migrate/apply, no 2004+ archive
backfill, no live `activation_run` / evidence counts, no `run_id` claimed; caveats/next = the harness is
fixture-proven and the live dense-price activation is the gated operator milestone). Update
`db/PARITY_GAP.md` §6 (Pricing) to note the activation harness makes the 2004+ widening turnkey + evidenced
while the live density remains operator-pending, and bump the migrations-through line to `0194`.

**Accept:** the ledger row + PARITY_GAP edit land in the same commit as the code; the offline suite is green;
the row is explicit that the live activation is operator-gated and unrun.

---

## Sequencing & expected compounding

**S6-0 → S6-1 → S6-2 → S6-3 → S6-4 → S6-5.** S6-0 lays the evidence tables (load-bearing — every later task
reads/writes `activation_run`). S6-1 builds the write-free planner (an `apply` with no plan to hash against, and
no dry-run to show an operator, is unsafe). S6-2 executes the plan backup-first with resume/idempotency — the
heart of the sprint — riding S6-1's plan + S6-0's evidence. S6-3 adds the `v_activation_status` view + reporter
once there is genuine per-run evidence to report. S6-4 exposes the whole thing as the operator CLI + runbook +
worktree guard last. S6-5 records the posture. **Compounding:** PF4-S11's production capstone runs its
end-to-end activation *through this harness* (recover→migrate→backfill→rebuild→verify), and the operator's
gated live 2004+ backfill becomes a single evidenced `warehouse_activate apply --operator-go` rather than a
folklore command sequence — densifying `equity_daily_bars` → `v_price_fundamental_overlap` → every factor
table that PF4-S1's signal-eval then scores.

---

## Risks / guardrails

- **The plan must write nothing.** If `plan_activation` opens the DB read-write, claims the migration lock, or
  calls `run_backfill`, it stops being a dry-run and can corrupt an operator's mental model. The write-free
  assertion (row counts unchanged) is a first-class test, not an afterthought — open `read_only=True` for file
  DBs.
- **Backup strictly precedes migrate, and migrate strictly precedes backfill.** Reuse `run_governed_migrations`
  (which bundles checkpoint+backup+apply+restore) rather than re-sequencing by hand; the ordering test (spy
  proving backup ran before the first partition write) guards clause F at the activation level.
- **Idempotency is the whole game.** A re-run of a completed activation must be a strict no-op — `activation_run`
  short-circuit + backfill completed-window no-op + migration forward-only idempotency. A resume after a
  mid-backfill kill must not double-write. Both are defining tests (S6-2 accept).
- **NEVER apply from a throwaway worktree.** The `.git`-file worktree guard is mandatory; it must default to
  refusing and require an explicit `--allow-worktree` override (used only in the offline test, never live). The
  live archive run is OPERATOR-PENDING and must be run from the primary tree with backup evidence.
- **No live 14 GB DB, no network, in any test.** All tests use in-memory / template-copy DuckDB + an injected
  partition executor (`test_backfill.py` fixture pattern) + fixture archive params — never a real archive file,
  never the shared DB.
- **Stay in 0192–0194.** Tables+catalog (0192) / view (0193) / indexes+catalog-sweep (0194); never renumber or
  edit a landed migration; back up DB+WAL before any live apply.

---

## Bench / acceptance

- Offline pytest green: `python -m pytest atx-impl\db\tests\test_activation_harness.py -q`, and full
  `python -m pytest atx-impl\db\tests -q` green before commit — **run from `atx-impl/`, never from `db/`**
  (`db/calendar.py` shadows stdlib `calendar`).
- **Plan is write-free + deterministic:** `plan_activation` enumerates the exact `plan_backfill` partitions for
  a fixture window, leaves `activation_run` / `backfill_run` / `backfill_watermark` / `equity_daily_bars` row
  counts unchanged, and produces a stable `plan_hash` that changes with the window/chunk.
- **Apply is backup-first + resumable + idempotent:** on a bounded fixture slice with an injected executor, a
  full `apply_activation` reaches all-`succeeded` with backup recorded before the first partition write; a
  mid-backfill kill resumes to completion with no duplicates; an immediate re-run is a strict no-op; `operator_go=
  False` and worktree paths are refused.
- **Verify emits matching evidence:** `verify_activation` + `v_activation_status` report per-partition +
  per-dataset counts equal to the fixture, watermark progress at the planned end, and an overlap-density report.
- **CLI works over a fixture:** `warehouse_activate plan|apply|verify` drive the harness through `main([...])`
  with an injected registry/executor; `plan` touches nothing; `apply` honors the operator-go + worktree guards.
- **Live activation** is **OPERATOR-PENDING** and documented in the runbook (module docstring), **not executed
  in-module**: the gated 2004+ archive backfill (backed up first, clause F) is an operator job whose live
  `activation_run_id`, per-dataset counts, backup path, and `git_sha` are recorded in the ledger on the operator
  run — this sprint proves the mechanism on fixtures only.
- `db/PARITY_GAP.md` updated (activation harness available; live density operator-pending; migrations through
  `0194`); a `WAREHOUSE_PARITY_TRANCHES.md` row appended (start/end SHA, domains, verification commands, explicit
  OPERATOR-PENDING live notes, caveats/next → PF4-S7 releases + PF4-S11 capstone run through this harness).

**Process:** run in an own git worktree off `main` via `atx-impl/scripts/new_db_worktree.sh new|finish
<slug>`; controller `superpowers:subagent-driven-development` (fresh implementer + reviewer per task; TDD +
verification-before-completion); never `git add -A` (stage explicit paths); never push unless asked. New module
⇒ new `test_*.py`. Commit trailer EXACTLY `Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>`.
