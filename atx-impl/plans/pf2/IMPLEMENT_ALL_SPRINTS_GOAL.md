# GOAL PROMPT — Implement the pf2 Fundamentals-Depth + Production series

> Paste below the line into a fresh agent. Detail lives in the plans; this is the driver.

---

Bring `atx-impl/db` to point-in-time parity with FactSet / S&P GMI Compustat for **US equity
fundamentals**: every metric, ratio, and derived ratio, linked through a strong schema, driven by real
job management **and production-level database and schema management**. Implement all 10 sprints
**PF2-S1…PF2-S10** end-to-end. This is the successor to pf1 (the fundamentals spine) and **assumes
pf1 PF-S1…PF-S8 have landed**.

**Use `superpowers:subagent-driven-development`** — you are the controller; fresh subagents implement +
review (implementers use TDD + verification-before-completion; final whole-branch review +
finish-branch at the end). Do not write code yourself.

**One fresh git worktree per sprint, merged back at sprint end.** Each sprint (PF2-S1…PF2-S10) is
implemented in its own isolated worktree so the schema churn never touches the primary tree and the
14 GB git-ignored `atx_impl.duckdb` is never copied. Use the helper `atx-impl/scripts/new_db_worktree.sh`:

```
scripts/new_db_worktree.sh new    pf2-s1     # branch feat/pf2-s1 off the mainline, worktree at C:/atx-wt/pf2-s1
#   ... run the sprint's SDD implementers/reviewers INSIDE that worktree; offline tests + commits there ...
scripts/new_db_worktree.sh finish pf2-s1     # merge feat/pf2-s1 back into the mainline, remove the worktree
```

- The worktree branches from and merges back into the **integration mainline** — the branch you launch
  from (`feat/warehouse-parity` today, which carries pf1). That is "local main." Set `BASE_BRANCH` only
  if you deliberately target a different branch.
- Offline `python -m pytest atx-impl/db/tests -q` runs fully inside the worktree (in-memory/template
  DuckDB, no live DB). Operator live-DB smoke runs against the shared DB in the primary tree
  (`--db-path <repo>/atx-impl/db/atx_impl.duckdb`), backed up first (clause F). Never run live smoke or
  a live migration apply from inside a throwaway worktree.
- `finish` refuses to merge a worktree with uncommitted changes and keeps the branch for history. Only
  run it once the sprint is green (offline suite passes) and its review is clean.
- Respect the ROADMAP overlap rule: two sprints that share `fundamental_ratios.py` or
  `fundamental_statements.py` must not have concurrent live worktrees — merge one back before starting
  the next. Disjoint-module sprints (S7, S8 vs the S5→S6 chain) may have concurrent worktrees.

**Read `atx-impl/plans/pf2/ROADMAP.md` first** — it holds the shared PIT/determinism + production
contract (clauses A–G), ownership, reserved migration ranges (`0097–0131`), sequencing, and the
north-star acceptance. Each `sprint-N-*.md` is the plan file for that sprint; its `S{N}-0, S{N}-1, …`
tasks are the task list (one implementer + one review each). Follow the ROADMAP sequencing
(S1→S2 → S3→S4 → S5→S6 ‖ S7 ‖ S8 → S9 → S10; serialize any two sprints sharing `fundamental_ratios.py`
or `fundamental_statements.py`). Track progress in `.superpowers/sdd/progress.md`; resume from the first
incomplete task.

**Non-negotiables** (also in ROADMAP §Shared contract — put them in every implementer + reviewer block):
- Offline tests only (no SEC/FRED/FINRA/OpenFIGI/GLEIF/vendor network in pytest; live connectors are
  injectable `--*-file`/`--*-zip`, smoke is operator-run + recorded in the ledger).
- Data posture: ship loader + engine + fixtures, then an operator-run **~1yr recent proof slice** with
  live counts in the ledger. No large historical backfills.
- Bitemporal PIT, no lookahead: `available_at = max(input.available_at)`; as-of readers gate on valid
  window **and** `available_at ≤ as_of_ts`.
- Append-only idempotent migrations in the sprint's reserved range; catalog every new table/view in the
  same migration; split schema vs index for WAL safety; **back up before every live apply** (clause F).
- **Schema-as-contract (clause E):** no table lands without a contract row + `table_catalog` entry +
  PIT-column assertion; the drift check stays green.
- **Quality-gated (clause G):** load-bearing invariants are registered as gated checks wired into the
  orchestrator, not just written to `data_quality_checks`.
- The standardization refactor (PF2-S3) must keep the existing ratio rebuild **reconcilable** to pf1
  outputs (regression test gates any intended change).
- Don't touch C++/CMake or the non-fundamental domains (13F/insider/macro/short-interest/off-exchange).
- `python -m pytest atx-impl\db\tests -q` green before each commit; never `git add -A`; commit trailer
  `Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>`; per sprint, append a
  `WAREHOUSE_PARITY_TRANCHES.md` row + update `PARITY_GAP.md`.

**Autonomy:** run for hours; don't check in between tasks/sprints. Stop only on unresolvable BLOCKED,
genuine ambiguity, destructive risk, or all 10 done. Done = the ROADMAP §North star acceptance.

---

## Implementation status (updated 2026-07-04, America/New_York)

Controller: `superpowers:subagent-driven-development`. Durable ledger: `.superpowers/sdd/progress.md`
(PF2 section at the bottom — authoritative; this block is a summary).

**Overall: PF2-S1 in progress (3 of 4 tasks complete). PF2-S2…PF2-S10 not started.**

### PF2-S1 — Schema-as-contract (worktree `C:/atx-wt/pf2-s1`, branch `feat/pf2-s1`, base `636e82b`)
Task order S1-0 → S1-2 → S1-1 → S1-3.

- **S1-0 complete** (`636e82b..f7a83a2`, review clean) — NEW `db/schema_contract.py`: declarative `CONTRACT`
  manifest + `detect_schema_drift` + `schema_contract_sha256`; migrations **0097** (`schema_contract` table
  + catalog) / **0098** (indexes).
- **S1-2 complete** (`f7a83a2..a276aba`, review clean after 2 fix passes) — `build_contract_manifest`
  reconciles both schema paths (schema.py core + every MIGRATIONS table); `is_pit_column` partition derived
  from temporal-marker presence (not hardcoded); zero-residual coverage test.
- **S1-1 complete** (`a276aba..5df7d59`, review clean/Approved) — two `severity=critical` gated checks in
  `db/quality.py`: catalog-completeness + PIT-column-presence, both manifest-driven, planted-fixture tests
  assert RED. Full suite green.
  - KNOWN GAP (adjudicated, honest): `pit_column_presence` correctly FIRES on **56** real pre-existing fact
    tables missing ≥1 PIT column (mostly `is_latest_revision`: equity_daily_bars, corporate_actions,
    fundamental_points, security_identifier_history). S1-1's scope is authoring the gate, NOT backfilling.
    Offender set pinned as a `<=` ratchet. Must be resolved (backfill / exempt / threshold-tune) when
    PF2-S10 wires critical checks to halt. Log in `PARITY_GAP.md` at S1 closeout.
  - Deferred to final review (non-blocking): `quality.py` imports private `_fetch_live_tables`/
    `_fetch_catalogued_tables` from `schema_contract.py` (encapsulation coupling — justified to avoid
    skiplist drift; promoting to public wrappers is schema_contract.py's lane).
  - Deferred: `scripts/verify_quant_warehouse.py` has no severity-awareness (exits 1 on any failed); S10
    gating must add severity filtering.
- **S1-3 PENDING** — warehouse data-catalog as-of reader + CLI. Brief written
  (`scratchpad/pf2/s1-3-brief.md`): migration **0099** `v_warehouse_catalog` (table_catalog + field_catalog
  + v_formula_registry lineage, catalogued view), `warehouse_catalog_asof(...)` in `db/asof.py` (gates on
  `updated_at <= effective_ts` — NO-lookahead, differs from formula_registry_asof which no-ops as_of_ts),
  `__init__` export, `python -m db.asof warehouse-catalog` CLI, tests extend
  `db/tests/test_schema_contract.py`. **Implementer dispatch not yet run — paused here by operator.**

### PF2-S1 closeout remaining (after S1-3 review clean)
Update `PARITY_GAP.md` (clause E defined/enforced + log the 56-table PIT gap), append
`WAREHOUSE_PARITY_TRANCHES.md` row, operator-run live-DB drift smoke (pre/post undeclared/uncatalogued/
missing-PIT counts + `schema_contract_sha256` + run_id, backed up first — clause F), then
`scripts/new_db_worktree.sh finish pf2-s1` to merge `feat/pf2-s1` back into `feat/warehouse-parity`.

### PF2-S2…PF2-S10 — not started
Sequencing S1→S2 → S3→S4 → (S5→S6 ‖ S7 ‖ S8) → S9 → S10. Each: fresh worktree, SDD task loop
(implementer + review per task), sprint review, `finish`. Final whole-branch review (most-capable model) +
`superpowers:finishing-a-development-branch`.

### Resume instruction
Read `.superpowers/sdd/progress.md` (PF2 section) first — resume at PF2-S1 **S1-3** by dispatching the
implementer with `scratchpad/pf2/s1-3-brief.md`. Do NOT re-dispatch S1-0/S1-2/S1-1 (complete). All PF2
worktree/PIT/append-only/offline-test/commit-trailer non-negotiables above still bind.
