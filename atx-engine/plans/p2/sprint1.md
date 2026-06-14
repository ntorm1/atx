# p2 · S1 — Constrained Multi-Horizon Portfolio Optimization (user reference)

**Status:** ✅ shipped on branch `feat/p2-s1-multi-horizon` (off `main` @ `f85f3d3`), pending your merge. 70 new tests, full engine suite green, `/W4 /permissive- /WX` + strict-FP clean.

## What this sprint built — in one paragraph
`p1` S7 already trades a *receding-horizon* book: each rebalance it runs a single-period solve, executes only the first
move, threads the realized book forward. S1 turns that into a **true multi-horizon optimizer** without rewriting it.
Signals now carry an explicit **decay horizon** (fast vs slow); the optimizer projects each signal forward, blends the
horizons into a **Gârleanu-Pedersen "aim" portfolio**, and trades toward it under a **full constraint algebra**
(factor-exposure, sector/group, beta-neutral, gross/net, position caps, turnover budgets) enforced **exactly** by a new
**deterministic fixed-iteration QP/ADMM solver**. It trades `p1` S8's cleaned covariance `V` and prices `p1` S6's
calibrated cost. It still executes only the first move (look-ahead-safe), and — the load-bearing guarantee — **collapses
bit-for-bit back to S7** on the boundary.

## The four new pieces (all under `atx-engine/include/atx/engine/risk/`)
| File | What it does |
|---|---|
| `constraints.hpp` | Composable constraint descriptors (`GrossNet`, `PositionCap`, `FactorExposure`, `GroupCap`, `BetaNeutral`, `TurnoverBudget`) that materialize into the QP's exact `l ≤ A·w ≤ u` form. |
| `qp_solver.hpp` | `ConstrainedQpSolver` — a deterministic, fixed-iteration OSQP-class ADMM. Never densifies `V` (routes `P=2λV` through a new matrix-free `FactorModel::apply`). Returns `Err` on an infeasible set, never a quietly-violated book. |
| `horizon.hpp` | The signal-horizon taxonomy: each signal's forecast decays at its own half-life; `forecast_trajectory` projects the current cross-section forward `α_t … α_{t+H}` (point-in-time, no peeking). |
| `multi_horizon.hpp` | `MultiHorizonOptimizer` — reuses S7's schedule walk, builds the GP aim portfolio from the trajectory, solves the constrained QP toward it, executes the first move. |

Plus a 2-method additive accessor on `risk/factor_model.hpp` (`apply` = forward `V·w`, `specific_var` = the diagonal) — the only engine-source touch outside the new headers.

## The guarantees it proves (the four gates, all in one integration test)
- **Deterministic** — two identical runs produce a byte-identical book schedule (the ADMM runs a fixed iteration count, no convergence early-exit).
- **No look-ahead** — the forward trajectory is a pure projection of today's data; truncating the future leaves past books unchanged.
- **Constraints exact** — every claimed constraint is satisfied at every period's realized book; infeasible ⇒ a real error.
- **Reduces to S7** — with one (identity-decay) horizon, the minimal constraint set, and full trade-rate, the new optimizer's book schedule is **byte-identical** to `p1` S7's `MultiPeriodOptimizer`.

## What's deliberately deferred (recorded in the ledger)
- A dedicated `atx-core` `qp_admm` kernel (warm-start, Ruiz equilibration, infeasibility certificates) — the engine ships a correct fixed-iteration fallback; the optimized kernel is the lift.
- The full O(N·H) "stacked-MPC" QP — the O(N) GP aim-collapse is the production path (the stacked form is a benchable flag, currently `Err(NotImplemented)`).
- Estimating each signal's horizon from realized IC-decay — that's the S2 sleeve's job (S1 takes horizons as config).
- A sparse constraint matrix + higher default ADMM iterations for very dense augmented constraint sets at the 3–5k-name scale.

## How to build / test it
From a VS Developer shell (or sourcing `vcvars64.bat`) with `VCPKG_ROOT` set, in the worktree:
```
cmake --preset ninja
cmake --build --preset ninja --target atx-engine-tests
ctest  --preset ninja -R "RiskConstraints|RiskQpSolver|RiskHorizon|RiskMultiHorizon|MultiHorizonIntegration"
```

## Next
**S2 — multi-strategy meta-book:** wrap each `MultiHorizonOptimizer` as a *sleeve* (universe × horizon × signal-family) and net the sleeve books into one fund under a cross-sleeve risk budget. S1 is the spine S2/S4/S8 all route through.
