# Sprint 5 progress ledger — Book-Level Gates + Capacity-in-Optimizer + Full Robustness Battery + Synthetic Smoke

**Goal:** make the book-level northstar bars (turnover < 0.20/day, capacity > $100M) **measurable
and enforceable**, and produce the **first real (synthetic-panel) scorecard row**. Five opt-in
levers, each inert-default (byte-identical when off):

1. **S5-1** cross-sleeve-netted book-level turnover as a PER-DAY rate — measured unconditionally
   (surfaced as `book_turnover_per_day` kv), gated opt-in via `--book-turnover-gate`.
2. **S5-2** participation-rate cap INSIDE the optimizer's QP construction (`--participation-cap`).
3. **S5-3** thread the 3 unreachable `eval::RobustnessBattery` checks
   (`sub_universe`/`alt_neutralization`/`param_perturbation`) into the admission-time caller.
4. **S5-4** non-zero borrow/financing debit reaching the vectorized book P&L (`--borrow-bps`).
5. **S5-5** synthetic-panel smoke exercising the whole S1–S5 lever stack → one honest, finite,
   deterministic scorecard row.

- **Worktree:** `C:\atx-wt\p9`
- **Branch:** `feat/p9`
- **Build gate:** `powershell -File <scratch>\p9-build.ps1 -Target atx-impl-tests`
  (and `atx-engine-book-tests` / `atx-engine-factory-tests`) then `p9-ctest.ps1 -R <Suite>`.

One line per clean unit (ROADMAP §141). Newest last.

| Unit | Commit  | Deliverable | Review |
|------|---------|-------------|--------|
| S5-0 | 147ce09 | ledger opened; 6 inert-default `RunConfig` fields (`book_turnover_gate`, `participation_cap`, `borrow_bps`, `robustness_sub_universe`/`_alt_neutralization`/`_param_perturb`) + CLI flags with fail-loud non-negative guards | — |
| S5-1 | d1c42c0 | book-level cross-sleeve-netted turnover as a per-day rate; measured unconditionally (`book_turnover_per_day` kv), gated opt-in via `--book-turnover-gate` | — |
| S5-2 | ca27121 | participation-rate cap inside the optimizer QP construction (`--participation-cap`) | — |
| S5-3 | f590808 | expose the 3 previously-unreachable `eval::RobustnessBattery` checks (`sub_universe`/`alt_neutralization`/`param_perturbation`) at admission, including the S5-3-corrected per-instrument `adv_col` (length `insts`, mean-volume) fix over the plan's flat-field draft | — |
| S5-4 | e82056e | non-zero borrow financing debit reaching `book::accumulate_report`'s realized P&L (`--borrow-bps`) | — |
| S5-5 | 3fc8fa2 | synthetic-panel smoke exercising the whole S1–S5 lever stack → one honest, finite, deterministic scorecard row | SHIP |

**Review (clean, adversarial):** SHIP-WITH-MINORS → closed. No Critical; no frozen-file breach
(`git diff --stat ca27121..HEAD` = `factory/factory.{hpp,cpp}`, `book/report.hpp`, `stage_report.cpp`,
`stage_discover.cpp` + 3 new test files + this ledger only); commit trailers exact; `NsgaSearch.
ScalarRaw_ReproducesGoldenDigest` (0xff95ac12512e0e91) and `FactoryOos.MineIntoOffPathDigestUnchanged`
both green. The `alt_neutralization` wire was the primed "Potemkin" suspect — a direct seed sweep
(0..199) against the real `robustness_battery_passes` rejects **92/200 (~46%)** of a maximally
group-tilted candidate, so the check is genuinely powered, not rigged. `sub_universe` collapses the
illiquid-edge scenario (`restricted_edge ≈ 1.3e-61` vs `base ≈ 1.0`) and admits the broad-edge one —
real discrimination. S5-4 borrow closed-form (`0.5·50·1e-4`) and inert-default byte-identity confirmed;
the pre-existing `book/pipeline.hpp:303` caller has zero diff. Regression: factory **255/255**, book
**35/35**, impl **316/320 (+4 pre-existing skips), 0 failed**. The 2 `RobustPipelineE2E` failures in
`atx-engine-risk-tests` are pre-existing (`robust_size` off-by-one, test file untouched since pre-p9
`830e117`; S5-3's additions are flag-gated inert for that non-battery path) — not attributable to S5.

**Two Important honesty findings, both fixed post-review (docs only, no logic change):**
- `AltNeutralizationRejectsGroupTilt` comment + assertion overclaimed "reject under ANY permutation";
  reworded to the measured ~46% seed-sweep rate and the real `min_survival_ratio=0.5` discriminator.
- **Known limitation (S5-2 participation cap):** at a realistic ~$1e9 NAV over a thin/small universe the
  participation-cap QP goes infeasible/non-convergent and `run_optimize` returns a **fail-loud** `Err`
  (never a wrong-answer path); the QP/elasticity machinery is prior-sprint, not S5's to fix. The S5-5
  smoke deliberately runs at `report_aum=1e6` to stay feasible; now documented at the fixture site and
  here so an operator sizing a real book knows the cap's large-AUM/thin-universe edge.
