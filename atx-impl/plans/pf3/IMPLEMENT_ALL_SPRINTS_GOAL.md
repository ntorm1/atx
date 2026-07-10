# GOAL PROMPT — Implement the pf3 Signal-Ready Fundamentals Factor Warehouse series

> Paste below the line into a fresh agent. Detail lives in the plans; this is the driver.

---

Take `atx-impl/db` from **fundamentals depth** (pf2) to a **signal-ready factor product on production
backfill rails**: complete + *surpass* FactSet / S&P GMI Compustat US-equity fundamentals, ratios, and
derived metrics, and materialize a **point-in-time, backtest-ready factor panel** — exported as
schema-contracted views + partitioned Parquet/Arrow — that pipes directly into a quant backtesting engine and
is scored for signal (IC / decay / turnover / crowding), all driven by a **full backfill + incremental-
maintenance DAG** on a **decomposed, contract-hardened** codebase. Implement all 12 sprints
**PF3-S1…PF3-S12** end-to-end. This is the successor to pf1 (spine) and pf2 (depth + production surface) and
**assumes pf1 PF-S1…PF-S8 and pf2 PF2-S1…PF2-S10 have landed** (merged to `main`).

**Use `superpowers:subagent-driven-development`** — you are the controller; fresh subagents implement + review
(implementers use TDD + verification-before-completion; final whole-branch review + finish-branch at the end).
Do not write code yourself.

**One fresh git worktree per sprint, merged back at sprint end.** Each sprint (PF3-S1…PF3-S12) is implemented
in its own isolated worktree so the schema churn never touches the primary tree and the git-ignored multi-GB
`atx_impl.duckdb` is never copied. Use the helper `atx-impl/scripts/new_db_worktree.sh`:

```
scripts/new_db_worktree.sh new    pf3-s1     # branch feat/pf3-s1 off the mainline, worktree at C:/atx-wt/pf3-s1
#   ... run the sprint's SDD implementers/reviewers INSIDE that worktree; offline tests + commits there ...
scripts/new_db_worktree.sh finish pf3-s1     # merge feat/pf3-s1 back into the mainline, remove the worktree
```

- The worktree branches from and merges back into the **integration mainline** — the branch you launch from
  (`main` today, which carries pf1 + pf2). Set `BASE_BRANCH` only if you deliberately target a different branch.
- Offline `python -m pytest atx-impl/db/tests -q` runs fully inside the worktree (in-memory / template DuckDB,
  no live DB). Operator live-DB smoke runs against the shared DB in the primary tree
  (`--db-path <repo>/atx-impl/db/atx_impl.duckdb`), backed up first (clause F). Never run live smoke, a live
  migration apply, or a **live backfill** from inside a throwaway worktree.
- `finish` refuses to merge a worktree with uncommitted changes and keeps the branch for history. Only run it
  once the sprint is green (offline suite passes) and its review is clean.
- Respect the ROADMAP overlap rule: two sprints that share the `fundamental_*`/pricing surfaces (S4/S5/S6) or
  the `db/factors/` package (S7/S8/S9) must not have concurrent live worktrees — merge one back before the
  next. Disjoint-module sprints may have concurrent worktrees.

**Read `atx-impl/plans/pf3/ROADMAP.md` first** — it holds the shared PIT/determinism + production contract
(clauses A–J), ownership, reserved migration ranges (`0132–0175`), sequencing, and the north-star acceptance.
Each `sprint-N-*.md` is the plan file for that sprint; its `S{N}-0, S{N}-1, …` tasks are the task list (one
implementer + one review each). Follow the ROADMAP sequencing (S1→S2→S3 → S4→S5→S6 → S7→S8→S9 → S10→S11 → S12;
serialize any two sprints sharing `fundamental_*`/pricing or `db/factors/`). Track progress in
`.superpowers/sdd/progress.md`; resume from the first incomplete task.

**Non-negotiables** (also in ROADMAP §Shared contract — put them in every implementer + reviewer block):
- Offline tests only (no SEC/FRED/FINRA/OpenFIGI/GLEIF/vendor network in pytest; live connectors are injectable
  `--*-file`/`--*-zip`, smoke is operator-run + recorded in the ledger).
- Data posture: build + **prove the backfill DAG** (windowed, chunked, resumable, idempotent) on a slice — do
  **not** execute the full multi-year historical backfill in-module; the archive load is a documented operator
  job. Content sprints ship loader + engine + fixtures, then an operator-run recent proof slice with live
  counts in the ledger.
- Bitemporal PIT, no lookahead: `available_at = max(input.available_at)`; as-of readers gate on valid window
  **and** `available_at ≤ as_of_ts`. **Panel PIT-safety (clause I):** cross-sectional operators rank only
  within the as-of cross-section; universe membership applied as-of; a lookahead test gates the export.
- Append-only idempotent migrations in the sprint's reserved range; catalog every new table/view in the same
  migration; split schema vs index for WAL safety; **back up before every live apply** (clause F).
- **Schema-as-contract (clause E) + semantic contract (clause J):** no table lands without a contract row +
  `table_catalog` entry + PIT-column assertion; every fact/metric/factor column declares unit + sign + scale;
  the drift check stays green.
- **Backfill-safe (clause H):** re-running a completed window is a no-op; a partial window resumes without
  duplication; per-partition watermarks record progress.
- **Quality-gated (clause G):** load-bearing invariants (incl. factor leakage/coverage) are registered as gated
  checks wired into the orchestrator, not just written to `data_quality_checks`.
- The factor store **extends** the existing `db/features.py` / `db/alpha_research.py` / `formula_registry`
  surfaces (it is not greenfield); the ratio/metric engine must keep pf2 ratio outputs **reconcilable**
  (regression test gates any intended change).
- Don't touch C++/CMake or the non-fundamental domains outside the factor-integration scope (13F/insider/macro/
  short-interest/off-exchange are read as factor inputs in S9, not re-architected).
- `python -m pytest atx-impl\db\tests -q` green before each commit; never `git add -A`; commit trailer
  `Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>`; per sprint, append a
  `WAREHOUSE_PARITY_TRANCHES.md` row + update `PARITY_GAP.md`.

**Autonomy:** run for hours; don't check in between tasks/sprints. Stop only on unresolvable BLOCKED, genuine
ambiguity, destructive risk, or all 12 done. Done = the ROADMAP §North star acceptance.

---

## Implementation status (updated 2026-07-05, America/New_York)

Controller: `superpowers:subagent-driven-development`. Durable ledger was requested at
`.superpowers/sdd/progress.md`, but that file does not currently exist in the PF3-S2 worktree; create/update it
before sprint closeout.

**Overall: PF3-S1 is landed. PF3-S2 is in progress. PF3-S3...PF3-S12 are not started.**

### Current repo/worktree state

- Primary tree `C:\atx` is on `main` at `3de44b9` (`Merge feat/p9 into main - p9 Tradeable Mega-Book...`).
  PF3-S1 is already merged into main at `6ef9cd5` (`Merge feat/pf3-s1 into main`). Do not recreate PF3-S1.
- Active PF3-S2 worktree: `C:\atx-wt\pf3-s2`, branch `feat/pf3-s2`.
- PF3-S2 committed HEAD is `6b72a92` (`Fix PF3-S2 PIT insert defaults`). Committed PF3-S2 history includes:
  `edb3981` S2-0 PIT gap close, `f2dff8d`, `84d21f2`, `3d751c8` S2-0 repairs, `848a2c1` S2-1 semantic
  schema contract, `6c61a75`/`3542ccf` S2-1 repairs, and `6b72a92` runtime PIT insert defaults.
- After `6b72a92`, full offline DB tests were green (`python -m pytest atx-impl\db\tests -q`) and
  `git diff --check` was green apart from Git LF/CRLF warnings.
- The PF3-S2 worktree is dirty with an uncommitted, interrupted S2-2 patch in exactly:
  `atx-impl/db/migrations.py`, `atx-impl/db/quality.py`,
  `atx-impl/db/tests/test_schema_contract_v2.py`.

### PF3-S2 task status

- **S2-0 - Close PIT gap:** committed on `feat/pf3-s2` with follow-up fixes. Treat as done unless new review
  finds regressions.
- **S2-1 - Semantic column contract:** committed on `feat/pf3-s2` with follow-up fixes. Treat as done unless
  new review finds regressions.
- **S2-2 - Semantic validation gate:** in progress and uncommitted. A worker started implementing
  `semantic_contract_check`, NaN/Inf failures, incomplete semantic declaration failures, disabled-registry
  suppression, and an append-only registry migration. The worker was intentionally shut down before final
  verification. Do not assume the patch is complete.
- **S2-3 - Contract versioning + panel export contract stub:** not started. Watch the migration numbering:
  the interrupted S2-2 patch appears to add a new `0137` migration for semantic-contract quality registration,
  while the sprint plan also reserves `0137` for S2-3 `schema_contract_version` and `panel_contract` work. Before
  committing, reconcile this so PF3-S2 does not burn `0137` on registry-only work and leave no reserved slot for
  S2-3. One likely path is to compose all remaining PF3-S2 append-only items into the final `0137` migration
  before committing.

### Last known S2-2 review findings

- High: S2-2 must not mutate already-committed/checksummed migration `0136`; semantic-contract
  `quality_check_registry` registration must be append-only.
- Medium: disabling the `semantic_contract` row in `quality_check_registry` should suppress the expensive
  semantic scan, not merely filter the result afterward.
- Medium: missing/null semantic declarations (`unit`, `sign`, `scale`, natural-key metadata where applicable)
  must fail the gate rather than being skipped.
- Low: repeated scans per constrained column are a scalability risk, but correctness/governance is the priority.

### Resume instruction

Resume in `C:\atx-wt\pf3-s2` and inspect the uncommitted diff before continuing. Use a fresh SDD implementer or
reviewer for S2-2; the last worker (`019f330f-433c-7db0-b65c-42a853a09761`) was shut down mid-task.

Minimum verification before committing S2-2/S2-3 work:

```
python -m pytest atx-impl\db\tests\test_schema_contract_v2.py -q -n0
python -m pytest atx-impl\db\tests\test_schema_contract.py atx-impl\db\tests\test_schema_contract_v2.py atx-impl\db\tests\test_migration_governance.py -q -n0
python -m pytest atx-impl\db\tests -q
git diff --check
```

Stage only explicit files; never `git add -A`. Use the exact commit trailer:
`Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>`.
