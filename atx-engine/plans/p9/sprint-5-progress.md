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
| S5-5 | (this)  | synthetic-panel smoke exercising the whole S1–S5 lever stack → one honest, finite, deterministic scorecard row | — |
