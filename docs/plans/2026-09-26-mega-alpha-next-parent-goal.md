/goal Resume the mega-alpha strategy objective using subagent-driven development: build a combined portfolio of many atx-engine alpha DSL subalphas targeting annualized Sharpe >= 1 after costs, one-way turnover <= 30% per month, a universe of thousands of stocks, and $1 billion NAV, using recent 2020+ data. These are targets to measure honestly, not results to assume. Prioritize core engine runtime, alpha generation, alpha composition and working portfolio construction; defer exhaustive corporate-action registration and detailed realism until the implementation works. Do not use test-driven development.

First read the detailed handoff:
`C:/atx-wt/pool-2/docs/plans/2026-09-26-mega-alpha-parent-handoff.md`.
Then read the active plan and original DAG completion index:
`C:/atx-wt/pool-2/docs/plans/2026-09-26-mega-alpha-strategy.md` and
`C:/atx-wt/pool-2/docs/plans/2026-09-24-alpha-engine-production-swarm.md`.

Work ONLY in the owned integration checkout `C:/atx-wt/pool-2`, branch
`feat/aes-codex-integration-20260925`. Pass this working directory explicitly
to every shell call. Do not mutate/build/switch/stash/commit the separately
owned `C:/atx` checkout. Last implementation commit before handoff docs is
`6df7cc88c2e89d7c339396d594fc4215f0cb0725`. Inspect actual Git state before edits.
The prior parent paused the goal at my request; this prompt authorizes resuming.

Preserve context by assigning bounded implementation/review tasks to child
agents in the established worktrees. Root alone owns builds and real numerical
runs. Pool-3 held evidence/review, pool-4 the fast IC runner, and pool-5 target
replay/review. They were stopped, with exact state in the handoff.

Do not restart from scratch. The fixed48 DSL ensemble has completed TRAIN and
frozen-sign 2023–24 validation on roughly3000 daily names. Validation48+blend
completed110.5seconds/~630MiB; rank IC5/21/63 was0.01803/0.03569/0.05967.
This is not Sharpe. Existing target turnover averages34–35% per month, above
target. Validation blend is saved. Portfolio replay with one fixed30% monthly
budget is integrated but not built. No validated Sharpe or $1bn capacity exists.

Immediate sequence:

1. Import the source-approved but unbuilt/unimported pool-4 VM lifetime fix
   `8527a839f0b4ad1b9a4d16fcc070dd68a35a7e10` and postimplementation fixture
   `23f1541b9da14352ecc4ab7d0ec8262186fa22b7`. They release an old5-slot arena
   before allocating7 slots, avoiding an expected248MiB transient that stopped
   the last TRAIN export. Pool-5 approved source and fixture; runtime is pending.
2. Finish the already-created ignored pool-3 compact evidence archive/report
   without rerunning research. Its exact paths and SHA index are in the handoff.
3. Use the prepared root helper
   `build-equity/build-recent-strategy-targets-v1.ps1` for one focused incremental
   build, then bounded native qualification. Preserve warm PCH/dependencies,
   ccache, clang-cl/LLD, scoped hot-CPP optimization, and RAM-admitted2–4 workers.
   Do not run global builds or long benchmarks.
4. Make one new bounded TRAIN-only export with the existing fixed48 library,
   original TRAIN pins and `--save-combined`. Require original v2 signs and
   metrics to match, retain the new TRAIN blend, and preserve every attempt.
   The last export stopped on RAM; it did not produce a complete TRAIN blend.
5. On that saved blend, compare exactly the preregistered baseline and monthly
   budget policies: cadence5, fraction.25, monthly budget.30, deployment and
   forced exits counted. Require exact baseline target parity. No parameter
   grid or DSL reevaluation. The shared hypothetical scenario is6bps one-way
   cost and300bps annual short borrow, not a $1bn capacity estimate.
6. Apply the already-frozen policy to the saved validation blend without sign,
   weight or parameter tuning. Report turnover alongside exposure, missing
   returns and rough costs. Do not manufacture full-period Sharpe from partial
   observed returns. Continue toward a measured combined net strategy.

Keep2025+ reserved. Use existing caches; no raw rescan is needed. Every real
research workload must be bounded to <=180seconds and RAM guarded. Do not kill
other owners' processes. No pushes, warehouse writes or broker actions.
Record implementation, import and evidence SHAs in the ORIGINAL DAG as elements
finish. Give concise progress updates with concrete results and limits. Do not
claim the objective complete until the combined portfolio actually meets the
declared metrics under a stated credible evaluation.
