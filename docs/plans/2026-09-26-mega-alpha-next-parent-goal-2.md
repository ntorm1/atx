/goal Resume the mega-alpha objective using subagent-driven development with Opus 5.5 child agents.

The deliverable is a **real out-of-sample NAV backtest of a mega-alpha portfolio**. Many high-quality
atx-engine alpha-DSL subalphas are admitted, signed and weighted on TRAIN only. They are combined into
one market-neutral book, rebalanced daily and simulated at $1bn NAV with declared costs and borrow. The
configuration is then frozen and run once on validation (2023-2024). Judge every task by whether it
moves that run closer.

Targets, measured on the frozen validation NAV run (primary scenario S2):

- annualized NET Sharpe >= 1 on excess returns after S2 trading costs and **`swap-fin-v1` financing**
  (252 sessions/yr). This is PB portfolio-swap financing: long leg 40 bps, short leg 20 bps plus a
  per-name borrow tier (GC 30 / warm 100 / special 500 bps), ACT/360, and no new shorts in special-tier
  names. The flat 300 bps borrow is demoted to a stress scenario (handoff §2b);
- **daily rebalancing**: a decision every session (`--cadence 1`), executed next session. US equities
  are efficient and our alpha decays within days. Monthly or weekly cadences are not candidates; they
  may appear only as labeled TRAIN diagnostics;
- **combined-book turnover**: mean daily one-way turnover <= 20% of GMV and p95 <= 30% of GMV
  (sum|fills| / pre-trade long+short $, deployment excluded);
- **individual alphas** may run up to 70% of GMV per day standalone, because combining nets opposing
  trades. Measure that: report the netting ratio `tau_book / sum_k w_k tau_k` on TRAIN;
- ~3000 stocks/day; $1bn NAV; 2020+ data.

The old 30%/month turnover target is **retired**: it is not a constraint, a selection criterion or a
pass/fail flag. Keep reporting monthly turnover for continuity only. The daily limits are declared now;
do not relax or tighten them after seeing results. These are targets to measure honestly, not results
to assume.

Priorities, in order: alpha quality, TRAIN-fit composition, daily construction (neutralize + no-trade
band), and the evaluator/runtime that makes them measurable. Defer exhaustive corporate-action realism.
Do not use test-driven development; write focused postimplementation fixtures.

## Read first

1. The detailed handoff: `C:/atx-wt/pool-2/docs/plans/2026-09-26-mega-alpha-parent-handoff-2.md`.
   §2 and §2a give the revised objective, the turnover research and the definitions. §6 gives the
   critical path.
2. The top section of the ledger: `C:/atx-wt/pool-2/.superpowers/sdd/mega-alpha-20260926/progress.md`.
3. The active plan: `C:/atx-wt/pool-2/docs/plans/2026-09-26-mega-alpha-strategy.md`. Its turnover line
   is superseded by the owner ruling.
4. The original DAG index: `C:/atx-wt/pool-2/docs/plans/2026-09-24-alpha-engine-production-swarm.md`.

## Working rules

- Work only in the integration checkout `C:/atx-wt/pool-2`, branch
  `feat/aes-codex-integration-20260925`. Pass this directory explicitly to every shell call.
- Never mutate, build, switch or commit in `C:/atx`; read-only inspection is okay. Inspect the actual
  Git state before any edit.
- Root alone builds, using `build-equity/mega-build.ps1`: RAM-admitted, Jobs 2-4, target-scoped,
  keeping warm PCH/ccache.
- Root alone runs real data, under `scripts/run_bounded_research.py` with Python312 (it has psutil).
  Limits: <= 180 s, RSS 1536 MiB, free floor 512 MiB.
- Do not let RAM slow progress. If a run is blocked, make it more efficient or more incremental; the
  candidate cache already lets stopped runs resume.
- Child agents implement in their own pool worktrees (handoff §3) and never build. Root cherry-picks,
  registers CMake, compiles, tests, and dispatches a task review per task.
- Select everything (admission, signs, weights, construction) on TRAIN 2020-2022 only. Validation
  2023-2024 is used once, on the frozen daily mega-alpha. 2025+ stays reserved until the owner
  authorizes a final test. Count every admission, composition and construction trial in the ledger.
- No pushes, warehouse writes or broker actions. Do not kill other owners' processes.
- Record implementation, import and evidence SHAs in the original DAG and in the ledger.

## Critical path (handoff §6 has the detail)

1. **T2 NAV replay.** Fix the one failing fixture, `StrategyNavReplay.ConstantPricesNoCost...`, then
   rebuild, retest (25 cases) and review. Then run NAV once, on TRAIN only:
   `--rule baseline-v1 --cadence 1 --trade-fraction 1` on the v6 blend. It is the evaluator smoke test
   and the first S2 read at daily cadence. **Do not spend validation on the v3 blend, monthly-budget-v2
   or any cadence > 1.**
2. **In parallel once T2 is fixed:**
   - T4 (revised brief): neutralize + no-trade band at cadence 1, GMV-denominated daily turnover
     stats, declared daily-ceiling flags. Then T10 (new brief `task-T10-brief.md`), with the same owner
     and files, run after T4: `swap-fin-v1` financing, tiered borrow from the T6 fields, locate block,
     and financing stresses;
   - review and import T5 fix round 1 (pool-7 `f2d5fb97`);
   - restart T7 (runner extra fields);
   - run T6 on the validation role and review T6;
   - restart T9 (`mv-shrink-0.9-nonneg-v1` weight fitter, plus per-alpha `tau_k` and the netting-ratio
     input);
   - redo the T1 review.
3. **Library v3 + quality admission.** Add short-interest, implied-vol and size families to the fixed
   v2. Freeze the library and the TRAIN-only admission screen before measurement:
   - standalone `tau_k` <= 70%/day (smooth faster alphas in DSL);
   - oriented neutralized Sharpe > 0 on 2020-21, with the same sign on 2022;
   - |rho| <= 0.7 against admitted alphas.
4. **TRAIN mega-alpha.** Run with the cache, fit the pinned weights and save the blend. Run NAV at
   cadence 1 with neutralize + band in {0, 0.5, 1, 2}/N. Choose by S2 net Sharpe subject to the daily
   turnover limits, and report the netting ratio. If the limits are breached, use the preregistered
   lever order: band, then partial fraction, then turnover-suppressed weights. Financing is already
   declared: primary `swap-fin-v1`; stresses `flat-300-v0` and `engine-tiers-v1`. Then freeze.
5. **Validation once: the deliverable.** Produce the validation fields, then run the frozen runner,
   saved blend and NAV. Report S2 net Sharpe, HAC t, drawdown, yearly returns, daily turnover mean/p95
   against the limits, netting ratio, capacity, financing by leg and tier, and both financing stresses,
   pass or fail.
6. If validation misses Sharpe 1, the next lever is alpha quality: fundamentals and industry via CIK,
   then a new library version. That requires a fresh TRAIN freeze. Each new frozen configuration gets
   one validation run, disclosed as an additional validation trial. Never tune on validation.

Give concise progress updates with concrete numbers and limits. Do not claim the objective complete
until the frozen daily mega-alpha meets the declared metrics on the validation NAV run.
