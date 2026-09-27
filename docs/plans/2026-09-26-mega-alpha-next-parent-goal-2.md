/goal Resume the mega-alpha strategy objective using subagent-driven development with Opus 5.5 child
agents. The goal is a combined portfolio of many atx-engine alpha-DSL subalphas targeting:

- annualized NET Sharpe >= 1 after costs;
- one-way turnover <= 30% per calendar month;
- a universe of ~3000 stocks per day;
- $1bn NAV;
- 2020+ data.

These are targets to measure honestly, not results to assume. Prioritize core runtime, alpha
generation, composition and working portfolio construction. Defer exhaustive corporate-action
realism. Do not use test-driven development; write focused postimplementation fixtures.

## Read first

1. The detailed handoff: `C:/atx-wt/pool-2/docs/plans/2026-09-26-mega-alpha-parent-handoff-2.md`.
2. The top section of the ledger: `C:/atx-wt/pool-2/.superpowers/sdd/mega-alpha-20260926/progress.md`.
3. The active plan: `C:/atx-wt/pool-2/docs/plans/2026-09-26-mega-alpha-strategy.md`.
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
- Child agents implement in their own pool worktrees (see the handoff §3) and never build. Root
  cherry-picks, registers CMake, compiles, tests, and dispatches a task review per task.
- Select everything on TRAIN 2020-2022 only. Validation 2023-2024 is used once per frozen
  configuration. 2025+ stays reserved.
- No pushes, warehouse writes or broker actions. Do not kill other owners' processes.
- Record implementation, import and evidence SHAs in the original DAG and in the ledger.

## Immediate sequence

1. **T2 NAV replay.** Fix the one failing fixture, `StrategyNavReplay.ConstantPricesNoCost...`, then
   rebuild, retest (25 cases) and review. Run NAV on the TRAIN v6 blend with baseline-v1 and
   monthly-budget-v2, then on the frozen VAL v3 blend. Report the S2 ($1bn modeled) net Sharpe, HAC t,
   drawdown, yearly returns, execution-month turnover, stale/write-off events and capacity. These are
   the first honest NAV numbers.
2. **T4.** Dispatch it: neutralize + no-trade band in the target/NAV replay.
3. **T6 and T7.** Run T6 on the validation role and review T6. Restart T7 (runner extra fields).
4. **T5 and T1 reviews.** Review and import T5 fix round 1 (pool-7 `f2d5fb97`). Redo the T1 review.
5. **T9.** Restart it (the `mv-shrink-0.9-nonneg-v1` weight fitter).
6. **Library v3.** Add short-interest, implied-vol and size families to the fixed v2. Freeze it before
   measurement.
7. **TRAIN.** Run with the cache, fit the pinned weights, save the blend, then run NAV with
   neutralize + band. Choose among a small preregistered set of construction settings, including an
   SI-tiered borrow scenario declared before viewing results next to the flat 300 bps scenario.
8. **Validation.** Freeze, then run validation once.

Give concise progress updates with concrete numbers and limits. Do not claim the objective complete
until the combined portfolio meets the declared metrics under the stated NAV evaluation.
