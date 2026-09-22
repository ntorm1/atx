# Tier-1 parity program — handoff (2026-09-20, session 2)

Supersedes `2026-09-19-tier1-parity-handoff.md` (still valid for S1/S2 detail).
Branch: `feat/tier1-parity` (base `main` a79f8371). HEAD at write time: e3f52a9e (+ any
commits landed by in-flight agents after this file — check `git log` first).

## 1. TL;DR
- S1 foundation: GATE CLEAN (e01ba95d). Merge to main still awaiting user OK.
- S2 standardization: core tasks T1-T7, T9 complete (T8 industry templates, T10 fixtures
  deferred by scope ruling). S2 gate review folded into the S3 gate.
- S3 derived engine: T1-T6, T9, T10 complete and reviewed. T7 (factor projection + parity
  harness) IN FLIGHT at stop (files on disk, uncommitted). T8 (retire 18 modules) NOT started
  (needs T7's parity evidence).
- S4 quality/universe/publication: T1, T2, T6, T9 complete. T3 and T4 landed, each in a fix
  round at stop. T7 (0307 schemas/SLOs) and T8 (0308 publication) IN FLIGHT (files on disk,
  uncommitted). T10 (retirement wave 2), T11 (docs/CI truth pass) NOT started. T5 deferred.
- Live warehouse activation run4 (child PID 22840) was in `companyfacts_load` at stop, after
  `submissions_load` completed (3,045,440 rows / 47,869 CIKs, 5,702 s). Prices already loaded
  (31.18M bars / 34,803 securities). Migrations 0300-0302 + 0301 applied to the live DB;
  0303-0308 are NOT yet applied (the next ladder start applies them under governed migrate).
- USER ACTION: `stash@{0}` holds 96 tracked-file edits from the user's other session
  (atx-engine/atx-impl/CMake/scripts/atx-build.ps1 + the 09-19 next-goal doc) that an agent's
  bare `git stash` stripped from disk; the controller's restore was denied by permission
  policy. Run `git stash apply stash@{0}` (the parity program never touched those files) or
  drop it deliberately. `stash@{1}` is stale S2-era seed WIP (already landed) — safe to drop.

## 2. Commits this session (aeb9a7e2..e3f52a9e, all on feat/tier1-parity)
aeb9a7e2 S2 T7 fix (cross-seed alias conflicts) · 7c1b0f09 S3 T5 engine · dffb609b S3 T4 rsst
sign fix · 8b4fdd75 S4 T1 0304 · 0825cf40 S4 T6 identity checks · 8cfaeba5 S3 T6 market_daily
· 3087db84 S3 T5 dense grid fix (Critical, re-reviewed clean) · 49008619 0302 pin relax ·
bde5a1db S4 T2 universe builder · f97e0001 S4 T6 fix1 · fd866224 S4 T1 fix1 (checks_universe;
re-reviewed clean) · e3855734 S3 T10 ladder wiring · 580b827e S3 T6 fix1 · d71b7905 S4 T2 fix1
· 43a355ce S4 T6 fix2 · ed8fd5f1 S3 T9 panel export + schemas + 0303 · ac17b67f S4 T9 data
dictionary · 2c0890bf S3 T9 fix1 · c1cbc3bd S4 T4 Shumway 0306 · ce3b81db S3 T6 fix2 ·
e3f52a9e S4 T3 delisting evidence 0305.

## 3. In-flight at stop (reconcile FIRST)
Uncommitted on disk (owner → files → what to do):
- S3 T7 (agent impl-s3-t7): `src/atx_db/derived_factor_projection.py`,
  `src/atx_db/seeds/derived_factor_projections.csv` (needs `git add -f`),
  `tests/test_derived_parity.py`, snapshot line "derived_factor_projection". Was in its test
  phase for 100+ min; controller ruled: land with explicit parity skips (reason per module).
  If no commit landed: run `pytest tests/test_derived_parity.py -n 0 -q`, convert remaining
  mismatches to explicit skips, commit with pathspec, then review (sonnet).
- S4 T7 (impl-s4-t7): `migrations/bodies_0307.py`, `api/catalog.py`, `provider_coverage.py`,
  `tests/test_provider_coverage.py`, `tests/test_provider_coverage_slos.py`, registry/__init__
  0307 lines.
- S4 T8 (impl-s4-t8): `migrations/bodies_0308.py`, `publication.py`, `scripts/publish_release.py`,
  `cli.py`, `tests/test_publication.py`, registry/__init__ 0308 lines, snapshot "publication".
- S4 T3 fix1 (impl-s4-t3): delisting_evidence.py — newest-snapshot bankruptcy overlay,
  source-salted evidence ids, real as_of_date bound, is_latest_revision filters.
- S4 T4 fix1 (impl-s4-t4): delisting.py + bodies_0306.py — per-exchange Shumway dispatch
  (−0.55 Nasdaq / −0.30 other; PIT exchange via universe_us_listed_membership →
  exchange_listings → nasdaq directory), reconciliation families, module-boundary import fix,
  runbook sentence.

Rule for reconciling: `git status --short -- atx-db`; for each owner group run its focused
tests, ruff, mypy --strict on new src files, commit with an explicit pathspec (seeds need -f).
Never `git stash / checkout -- / reset / restore / clean` on this shared tree.
registry.py / migrations/__init__.py must only be committed together with the bodies_NNNN.py
files they import (HEAD was unimportable once this session because of that).

## 4. Remaining program (in order)
1. Reconcile §3. Verify `python -c "import atx_db"` and
   `pytest tests/test_module_boundaries.py tests/test_schema_contract_v2.py -n 0 -q`.
2. S3 T8: retire the 18 covered per-metric modules once test_derived_parity has no raw-value
   skips that matter (brief: `.superpowers/sdd/2026-09-19-tier1-s3-derived-engine/task-8-brief.md`;
   migration number is the next free one — 0303 was taken by S3 T9, 0305-0308 by S4).
3. S4 T10 retirement wave 2 (abnormal_capex + operating_leverage only); S4 T11 docs/CI truth
   pass (README, PRODUCTION_RUNBOOK, PARITY docs, CI staleness gate for DATA_DICTIONARY.md —
   regenerate after T8 lands: `python scripts/generate_data_dictionary.py`).
4. Live warehouse: let run4 finish (`.superpowers/sdd/tier1-parity/activation-run4.log`); then
   rerun `python scripts/warehouse_activate.py --db-path data/warehouse.duckdb --memory-limit 6GB
   --threads 8 --shards 8 --run-id activation-run5 --start-stage statement_points --force`
   with `ATX_SEC_USER_AGENT="atx-db/0.1 atx-research@example.com"` (dummy email per user
   ruling) so the new stages (derived_metrics, market_daily, delisting_evidence,
   universe_us_listed, provider_coverage) run over the full universe. Governed migrate applies
   0303-0308 first (backup ~4 GB each; the two stale `.pre-migrate.*.bak` files from 09-19/09-20
   can be deleted with user OK). Check `activation_stage_runs` for stale 'running' rows first.
5. Measure: `scripts/measure_item_coverage.py`, `refresh_provider_coverage`, quality checks
   (identities, shares, universe, survivorship); publish numbers in docs; flip schema
   conditions only on measured thresholds; publish a first full-universe release.
6. Sprint gates: full non-slow suite once per gate, opus whole-branch review, then ask the
   user before merging to main.

## 5. Standing rulings (full list in program.md)
- Speed ruling: focused tests once, one review pass, re-review only for Critical.
- Scope ruling: full production DB + core metrics over niche coverage (S2 T8/T10, S4 T5 deferred).
- Shared tree: pathspec-only commits; no stash/checkout/reset/restore/clean; never commit
  registry lines for modules you did not write; snapshot-line bleed is the only allowed
  cross-task inconsistency.
- PIT: available_at = max of inputs; end-of-day = as_of + 22h; no clock reads in library code.
- Derived grid: dense quarter buckets (floor((y*12+m-1+(d>=15))/3)); windows count buckets.
- Market daily: row-rank lookback ≤300 sessions for scoped reruns; negative denominators keep
  signed ratios (Compustat convention).
- Shumway ON by default; opt-out via None; per-exchange dispatch (T4 fix1).
- User email never sent to external services; SEC UA uses the dummy contact.

## 6. Ledgers
- Program: `.superpowers/sdd/tier1-parity/program.md`
- S2: `.superpowers/sdd/2026-09-19-tier1-s2-standardization/progress.md`
- S3: `.superpowers/sdd/2026-09-19-tier1-s3-derived-engine/progress.md` (task-N-brief/report/review, fix reports)
- S4: `.superpowers/sdd/2026-09-19-tier1-s4-quality-universe-publication/progress.md`
  (briefs = plan line ranges; reports/reviews per task)
- Activation logs: `.superpowers/sdd/tier1-parity/activation-run{1..4}.log`
