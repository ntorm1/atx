# Lane 6 (l6-optim) status — Optimizer: cost terms, ADMM schedule, discretize, GP Riccati, MPC

- Worktree: `C:/atx-wt/pool-7`  branch `feat/qps-l6-optim`  base `334a7939`  (synced with integration at 82df1513)
- head `1cf59cb7`
- Session 3 (review-fix round) DONE. Everything is committed. Not merged.

## Commits (lane)
- 10e52630 cost_terms (kappa L1, 3/2-power rotated cones, borrow split + locate box)
- fe3894ea admm_schedule (per-row rho, pow2 adaptive rho at fixed iters, over-relaxation, warm start, early exit)
- 53269259 discretize (max-names, min-trade, re-solve, round lots) + gp_riccati (dense Riccati + factor-space closed form)
- a876b932 mpc_stack.hpp: true stacked H<=3 MPC QP through the augmented solver (w_1..w_H jointly)
- 46fe3195 RiskConstraintDispatchMH (every descriptor alone moves the book or fails typed)
- 8c6695a9 RiskQpAugmentFast / RiskQpAugmentNightly split; WarmStart::rho carry-over (0 => cfg.rho, byte-identical)
- b8aa37f9 bench/optimizer_production_bench.cpp
- 88d56408 review fixes: discretize max_names invariant (+ schedule/warm re-solve), GpPolicy step/aim size checks
  (Result), MH dispatch cone tests assert the cone bound
- d72d96fe qp_factor_admm.hpp factor-space ADMM (QpConfig::factor_space, opt-in) — the 100 ms target; bench modes 6/7
- 02161952 MultiHorizonOptimizer cfg.true_mpc -> solve_mpc_stack (production entry point)
- 1f456451 cost/optimizer_cost_terms.hpp: Lane 8 ReplayCostModel + S6-5 borrow -> TradeCostTerms (one calibration)
- 1cf59cb7 doc: polish acceptance rule

## Review findings (session 3)
- [FIXED] major perf: warm M=3000 K=64 = 57.7 ms (target <100 ms) via factor-space ADMM
- [FIXED] major discretize max_names: excluded names always pinned flat; Err(Internal) guard; 2 regression tests
- [FIXED] major wiring: MH true_mpc mode (solve_mpc_stack), cost helpers from Lane 8 coefficients.
  PortfolioOptimizer hooks for solve_with_costs/discretize still NOT wired (deferred, see Next)
- [FIXED] minor discretize re-solve schedule/warm: DiscretizeCfg::schedule + ::warm
- [FIXED] minor gp_riccati size checks: step/aim return Result
- [FIXED] minor MH dispatch cone asserts (sector sigma, tracking TE, robust ||X'w|| vs augmented nominal)
- [FIXED] minor doc drift: bench header recipe; Nightly has ONE M=200 case (seed 51), prior report was wrong;
  30 s Fast budget applies to Release (1.9 s); Debug is ~33 s under ctest on the shared box

## Tests (Debug, equity-dev, groups=risk)
- full atx-engine-risk-tests.exe: 405 passed, 1 skipped (Nightly by design), exit 0, 57 s
- anchored ctest (lane + must-stay-green: RiskFactorAdmm|RiskMhTrueMpc|RiskOptimizerCostTerms|RiskMpcStack|
  RiskConstraintDispatchMH|RiskAdmmSchedule|RiskCostTerms|RiskDiscretize|RiskGpRiccati|RiskQpAugment*|
  RiskConstraintDispatch|RiskKktLdl): 102/102 passed, Nightly skipped

## Bench (Release equity-rel, shared box)
Build: configure -Preset equity-rel -Groups risk -Bench; build atx-engine-bench AND atx-shm-worker.
Book: dollar-neutral, gross<=1, |w|<=10/M, 2 fexp bounds, beta band, K=64. eps 2e-7, cap 3000.
- factor-space warm (mode 6): M=1000 8.3 ms/100 it, M=3000 57.7 ms/250 it, M=5000 171 ms/400 it; all polished,
  book == cold eps-1e-11 reference (max diff 0)
- factor-space cold (mode 7): M=1000 15.8 ms, M=3000 106 ms, M=5000 134 ms
- augmented warm early-exit (mode 4): M=1000 278 ms, M=3000 1424 ms, M=5000 2596 ms (unpolished; its book
  exceeds the gross budget by 1.1e-4 / 2.5e-4 — the per-split-row gate lets that through)

## Next / deferred
- PortfolioOptimizer integration of solve_with_costs / discretize_and_resolve (minimal prox optimizer; different
  algorithm — integration step)
- LEDGER.md line at integration (lane rule: no appends here)
- ACCIDENT to report: an EMPTY untracked file C:\atx\atx-engine\tests\risk\risk_qp_factor_admm_test.cpp was
  created by a mis-rooted PowerShell WriteAllText; deleting it was denied by the permission classifier. The user
  must delete it (it will block merging this branch into C:\atx: "untracked file would be overwritten").
