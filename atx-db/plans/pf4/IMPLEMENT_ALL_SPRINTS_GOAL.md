# GOAL PROMPT — Implement the pf4 Production Quant-Product & Warehouse Activation series

> Paste below the line into a fresh agent. Detail lives in the plans; this is the driver.

---

Take `atx-impl/db` from a **fixture-proven, data-empty, un-activated pf3 factor warehouse** to a
**state-of-the-art, production-ready US-equity fundamental factor _product_ for downstream quant teams**:
close pf3's two unbuilt sprints (signal evaluation + production capstone), harden the S1–S10 code and re-green
the offline suite, make the data **survivorship-safe** and **dense** across **multiple governed universes**,
package it as **immutable semver'd releases (Parquet + Arrow)** fronted by a `pip install`-able **`atx-panel`
Python SDK** with a **generated data dictionary + docs + notebooks** and a **served read tier**, all made
**operationally trustworthy** (orchestrator-gated panel quality + factor observability) and **reproducibly
activatable** via a documented, operator-gated backfill runbook. Implement all 11 sprints **PF4-S1…PF4-S11**
end-to-end. This is the successor to pf1 (spine), pf2 (depth + production surface), and pf3 (signal-ready
factor store on backfill rails) and **assumes pf1 PF-S1…S8, pf2 PF2-S1…S10, and pf3 PF3-S1…S10 have landed**.

**Use `superpowers:subagent-driven-development`** — you are the controller; fresh subagents implement + review
(implementers use TDD + verification-before-completion; final whole-branch review + finish-branch at the end).
Do not write code yourself.

**One fresh git worktree per sprint, merged back at sprint end.** Each sprint (PF4-S1…PF4-S11) is implemented
in its own isolated worktree so schema churn never touches the primary tree and the git-ignored multi-GB
`atx_impl.duckdb` is never copied. Use `atx-impl/scripts/new_db_worktree.sh`:

```
scripts/new_db_worktree.sh new    pf4-s1     # branch feat/pf4-s1 off the mainline, worktree at C:/atx-wt/pf4-s1
#   ... run the sprint's SDD implementers/reviewers INSIDE that worktree; offline tests + commits there ...
scripts/new_db_worktree.sh finish pf4-s1     # merge feat/pf4-s1 back into the mainline, remove the worktree
```

- The worktree branches from and merges back into the **integration mainline** — the branch you launch from.
  Set `BASE_BRANCH` only if you deliberately target a different branch.
- Offline `python -m pytest atx-impl/db/tests -q` runs fully inside the worktree (in-memory / template
  DuckDB). **Run pytest from `atx-impl/`, never from `db/`** — `db/calendar.py` shadows stdlib `calendar` and
  breaks collection when cwd is `db/`. Operator live-DB smoke runs against the shared DB in the primary tree
  (`--db-path <repo>/atx-impl/db/atx_impl.duckdb`), backed up first (clause F). Never run live smoke, a live
  migration apply, or a **live backfill** from inside a throwaway worktree.
- `finish` refuses to merge a worktree with uncommitted changes and keeps the branch for history. Only run it
  once the sprint is green (offline suite passes) and its review is clean.
- Respect the ROADMAP overlap rule: two sprints that share the `fundamental_*`/pricing/universe surfaces
  (S4/S5/S6) or the panel export/read/serve path (S7/S8/S10) must not have concurrent live worktrees — merge
  one back before the next. Disjoint-module sprints may have concurrent worktrees.

**Read `atx-impl/plans/pf4/ROADMAP.md` first** — it holds the shared PIT/determinism + production contract
(clauses A–L), ownership, reserved migration ranges (`0176–0204`), sequencing, and the north-star acceptance.
Each `sprint-N-*.md` is the plan file for that sprint; its `S{N}-0, S{N}-1, …` tasks are the task list (one
implementer + one review each). Follow the ROADMAP sequencing (S1→S2→S3 → S4→S5→S6 → S7→S8→S9→S10 → S11;
serialize any two sprints sharing `fundamental_*`/pricing/universe or the panel export/read/serve path). Track
progress in `.superpowers/sdd/progress-pf4.md`; resume from the first incomplete task.

**Live-backfill posture — the standing gate (scope decision #1).** pf4 builds all code + a reproducible
activation runbook; **the actual live migration/backfill against the 14 GB DB is executed ONLY on explicit
per-step operator go.** No sprint autonomously applies migrations to, or backfills, the live production DB, and
no sprint performs multi-hour SEC/vendor network pulls. Content sprints ship injectable loaders + engines +
offline fixtures; the operator runs the archive on approval and records live counts in the ledger.

**Non-negotiables** (also in ROADMAP §Shared contract — put them in every implementer + reviewer block):
- Offline tests only (no SEC/FRED/FINRA/OpenFIGI/GLEIF/vendor network in pytest; live connectors are injectable
  `--*-file`/`--*-zip`, smoke is operator-run + recorded in the ledger).
- Bitemporal PIT, no lookahead (clause A); panel PIT-safety (clause I): cross-sectional operators rank only
  within the as-of cross-section; universe membership applied as-of; a lookahead test gates the export and the
  signal-eval forward-return joins.
- Append-only idempotent migrations in the sprint's reserved range; catalog every new table/view in the same
  migration; split schema vs index for WAL safety; back up before every live apply (clause F).
- Schema-as-contract (E) + semantic contract (J): no table lands without a contract row + `table_catalog`
  entry + PIT-column assertion; every fact/metric/factor column declares unit + sign + scale.
- Backfill-safe (H): re-running a completed window is a no-op; a partial window resumes without duplication.
- Quality-gated (G): load-bearing invariants (incl. factor leakage/coverage, survivorship bias, release
  immutability, client/view parity) are registered as gated checks wired into the orchestrator where
  applicable, not just written to `data_quality_checks`.
- **Release immutability (K):** a published panel release is content-addressed + checksummed; re-publishing
  identical inputs is a no-op; a pinned release is never mutated or pruned.
- **Client/view parity (L):** the `atx-panel` SDK read path returns rows identical to the contracted view read
  for the same `(as_of, universe, factors, release)`; a parity test gates the client.
- The signal-eval surface **reads** the S10 `v_factor_panel` read-only and must not perturb the export
  contract or `schema_sha256`; the S7 release engine and S8 SDK **extend** `db/lake.py`/`db/factor_panel.py`
  surfaces (not greenfield); S3 hardening must keep the `module_boundaries` public-API contract intact
  (deliberately re-pin the snapshot only where an intended change requires it).
- Don't touch C++/CMake or the parked pf5 domains (ESG / IBES licensed estimates / international-IFRS /
  supply-chain graph).
- `python -m pytest atx-impl\db\tests -q` green before each commit; never `git add -A`; commit trailer
  `Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>`; per sprint, append a
  `WAREHOUSE_PARITY_TRANCHES.md` row + update `PARITY_GAP.md`.

**Autonomy:** run for hours; don't check in between tasks/sprints. Stop only on unresolvable BLOCKED, genuine
ambiguity, destructive risk (incl. any live-DB apply/backfill — that is always operator-gated per decision #1),
or all 11 done. Done = the ROADMAP §North star acceptance.

---

## Implementation status (initialized 2026-07-06, America/New_York)

**Overall: PF4-S1…PF4-S11 are all NOT STARTED.** pf4 planning infrastructure (design spec, ROADMAP, this
driver, and the eleven `sprint-N-*.md` plan files) is authored and ready for an SDD controller.

### Preconditions verified (2026-07-06)
- pf3 landed **S1–S10** only; **S11 + S12 are unbuilt** (migrations stop at 0167; `db/signal_eval.py` absent).
  pf4 absorbs the S11/S12 intent as **PF4-S1** (signal eval) and **PF4-S2 + PF4-S11** (gating/observability +
  capstone).
- Every pf3 sprint is `OPERATOR-PENDING`: migrations `0132–0167` are **not applied to the live DB**; factor
  tables are data-empty. pf4 reserved range `0176–0204` is unclaimed.
- S1–S10 code review: 1 High (EV PIT period-selection), 5 Med, ~6 Low → **PF4-S3**.
- Offline suite (2026-07-06): ~8 failures (date-sensitive time-bombs + snapshot drift) → **PF4-S3** re-greens.

### Resume instruction
Begin at **PF4-S1** in a fresh worktree (`scripts/new_db_worktree.sh new pf4-s1`). Create
`.superpowers/sdd/progress-pf4.md` before first commit. Follow `sprint-1-signal-evaluation.md` task-by-task.
