# Task R-6 report: target-tracking optimiser spo-v3 (S-8) -- STOPPED after part 1 of 2

Lane R6, worktree `C:/atx-wt/pool-7`, branch `feat/platform-v8-r6-20260929`. Stopped on the owner's instruction at the
end of the engine-solver unit. Not built and not run (lane rules). Every claim about behaviour below is from reading
the code, or from a numpy port of the same algorithm run on synthetic data (marked [proto]).

| commit | content |
|---|---|
| `75774cd8` | PM addition: v7 side files and the spo tripwire cover scored decisions only under `--warm-start-sessions K` |
| `d695cbd8` | part 1: `atx::engine::book::solve_tracking` (the generic solver) + `TargetTracking.*` gtests |
| (none) | part 2, the atx-impl rule `--rule spo-v3 --spo-alpha implied-aim`: NOT STARTED (see STOPPED HERE) |

## 1. Warm-start side files (`75774cd8`, PM instruction)

- `strategy_nav_v7.cpp` `State::plan`: `observe = pass == Main && d >= x.decision_begin`. A warm-up decision leaves no
  transfer-coefficient record. The rules (aim-partial-v6 cost history, spo) still plan it. A no-rule warm-up decision
  goes straight to `detail::update_weights`: the same call, with the same arguments, that its else-branch made.
- `strategy_spo.cpp` `Engine::Impl::plan`: the diagnostics row is pushed only when `d >= x.decision_begin`. The shadow
  book still moves. So `spo_diagnostics.csv`, the spo summary blocks and `ceiling_tripwire` / `tripwire_json` cover
  scored decisions only.
- K = 0: the replay's first decision is `decision_begin`, so both filters are no-ops and the bytes are unchanged.
- Test `NavV7Hook.SideFilesExcludeWarmUp` (in `strategy_spo_test.cpp`), spo-v2 on `Role(30, 12, 71)` with
  `decision_begin` 10:
  - With K = 10, the TC sessions and the spo row sessions equal those of a flat start at 10.
  - A clamp on warm-up rows 3..7 leaves the tripwire clear. The same clamp on rows 12..16 still voids the run.
- Root verification: gtest filter `NavV7Hook.*:Spo*`. For real data, rerun the recorded spo-v2 cell argv (W1b report,
  step 4) with the new exe. `spo_diagnostics.csv`, `v7_transfer_coefficient.csv` and `v7_extras.json` must be
  byte-identical to the recorded cell's.

## 2. Part 1: the engine solver (`d695cbd8`)

Files:
- `atx-engine/include/atx/engine/book/target_tracking.hpp`: the problem and method stated at the top.
- `atx-engine/src/book/target_tracking.cpp`
- `atx-engine/tests/book/book_target_tracking_test.cpp`
- `atx-engine/CMakeLists.txt`: cross-lane edit, see section 4.

### Problem as coded (one period, n names)

    minimize_w  (gamma/2)(w - a)' Sigma (w - a) + sum_i [s_i|w_i - w0_i| + eta_i|w_i - w0_i|^{3/2}] + sum_i b_i max(-w_i, 0)
    s.t.        lower_i <= w_i <= upper_i,  |w_i - w0_i| <= t_i,  net.lo <= 1'w <= net.hi,  beta.lo <= beta'w <= beta.hi

- Sigma = B F B' + diag(d) is never formed. B's columns are an intercept, one-hot groups and dense styles: exactly
  the atx-risk-v1 layout (market, FF49 slot, 11 styles).
- Positions outside the problem enter as `external_gap` = B'(w - a) over them.
- A box emptied by the trade limit becomes the point w0 -+ t toward the box (spo-v1's rule).

API:
- `TrackingProblem`, `TrackingOptions`, `TrackingSolution`, `validate_tracking_problem`, `tracking_terms`, and
  `solve_tracking(problem, options = {}, warm_dual = {})`.
- The solution reports `iterations`, `converged`, `primal_residual`, `dual_residual`, `limits_met`,
  `limit_violation`, `restore_passes`, `clipped_eigenvalues`, the net and beta multipliers, `terms` (tracking
  variance, trade cost, borrow, objective), `tracking_error`, `no_trade`, `at_trade_limit`, `trade_limit_share` and
  `dual` (for a warm start).

Named constants:

| constant | value |
|---|---|
| `tracking_max_iterations` | 2000 |
| `tracking_tolerance` | 1e-9 (weight units) |
| `tracking_penalty_scale` | 10 (rho_i = 10 gamma d_i) |
| `tracking_relaxation` | 1.6 |
| `tracking_restore_passes` | 16 |
| `tracking_limit_tolerance` | 1e-12 |

### Method and convergence (ten lines)

1. Split: min f(x) + g(z) s.t. x = z. f is the tracking quadratic plus the indicator of the two limit bands. g is
   the separable trading cost, borrow and box.
2. Over-relaxed scaled ADMM (alpha 1.6) in the fixed diagonal metric rho_i = 10 gamma d_i.
3. x-update, exact: (gamma Sigma + R)x = q by Woodbury, with W W' = gamma F_+ (eigen root, negative eigenvalues set
   to 0 and counted). The K x K capacitance I + W'B'Delta^{-1}BW is factored once per solve by Cholesky (SPD,
   eigenvalues >= 1).
4. The two limit rows: an exact KKT active-set choice among at most 9 cases (0-, 1- or 2-row equality solves).
5. z-update, exact and closed form per name: a soft threshold with the 3/2 power, cancellation-free; the borrow kink
   at 0; clip to the box.
6. f and g are closed, proper and convex; the constraints are polyhedral. ADMM with a fixed positive diagonal metric
   and alpha in (0, 2) therefore converges to a KKT point whenever the feasible set is nonempty (Eckstein-Bertsekas
   1992).
7. Stop when ||x - z||_inf <= tol and ||rho(z - z_prev)/(gamma d)||_inf <= tol. The second is the dual residual as a
   weight step in each name's own curvature; a plain ||dz|| flatters large penalties. Otherwise stop at the cap.
8. The returned z is always inside its box. A restoration then puts it on both limits to 1e-12: at most 16 exact
   2 x 2 passes, over names strictly inside their box and off their kinks (w0 and 0).
9. Deterministic: order-fixed loops, fixed metric, no RNG, clock or threads. O(n + K^2) workspace allocated before the
   loop; the loop allocates nothing.
10. Cost per iteration is about 2 x 13n + 3K^2 + 30n flops. [est] About 0.1 ms at n = 1,850 under the Debug /O2
    setting, so about 10 ms per solve at 100 iterations.

[proto]: numpy port, n = 1,000 to 1,800, S2-law costs, the registered gamma.
- 55 to 110 iterations per solve. Objective within 1e-8 relative of a 1e-12 reference; max weight error 3e-10.
- A warm dual cut the next decision from 232 to 105 iterations (at penalty scale 3).
- A line-for-line port (`tt_port.py`, scratchpad) passes every assertion of the new gtests.
- Why ADMM with a dual residual in curvature units: a weight-unit dual residual let rho = 10 gamma d stop early with
  a 5e-7 objective error, and z's net drifted to 5e-7 over 1,800 names. Hence the curvature-unit residual and the
  restoration.

Not reused, with reasons:
- `risk::solve_factor_admm` (+ `TradeCostTerms`): dense M x K `FactorModel` (5x the flops of the 13-nonzero rows),
  an SPD-F requirement (atx-risk-v1 zeroes NaN covariance entries, so only PSD is assured), and fixed-count, polish
  and gate semantics.
- spo-v1's FISTA: it is in atx-impl and needs a nested root per iteration for net and beta.
- The risk loader (`spo::RiskStore`) will be reused as is in part 2.

### Gtests (`atx-engine-book-tests`, filter `TargetTracking.*`)

| test | checks |
|---|---|
| `ZeroCostNoLimitsReturnsTheTargetTo1e8` | default options |
| `DiagonalLinearCostIsTheSoftThreshold` | closed form; dead-zone names exactly w0 |
| `OneNameMatchesAGoldenSectionSearchAtEveryKink` | buy, sale into a short, dead zone, borrow kink at exactly 0 |
| `HigherCostTradesLess` | unit trade cost nonincreasing in kappa (optimality argument) |
| `LimitsAndBoxesHoldToTolerance` | box, locate floor, trade limit; net equality 1e-12; beta on the band edge |
| `TradeLimitShareCountsNamesAtTheirLimit` | share at the trade limit = 1 |
| `IndefiniteCovarianceIsClippedAndCounted` | one clipped eigenvalue, still converges |
| `RepeatsBitForBitAndAWarmDualAgrees` | same bits on repeat; warm dual reaches the same book |
| `RefusesMalformedProblems` | refusals |

Root: `powershell scripts\atx-build.ps1 build atx-engine-book-tests`, then
`build\bin\atx-engine-book-tests.exe --gtest_filter=TargetTracking.*`. No other target is affected: the solver is not
called by anything yet, so every existing output is unchanged by construction.

## 3. Open risks

1. **Registered constants vs. the book's risk structure [proto, synthetic]. Please read before registering the
   cell.**
   - The accepted book's ex-ante variance is about 94% factor (3.58% total against under 1% specific, review S-7).
   - gamma = S_prior / sigma_aim at S_prior 1 gives an implied alpha whose name-specific part, gamma d_i a_i, is
     about 7.5e-6 per session. The amortised linear cost is 3e-5 per session. So the tracker trades names for their
     factor loadings, not for their own signal (research note, failure mode (i)).
   - 60-decision simulation, aim 90% factor, n = 800, H = 20, all from flat:

     | S_prior | corr(w, w_aim) | gross (L = 1.247) | TE / sigma_aim |
     |---|---|---|---|
     | 1 (registered) | .46 at decision 0, falling to .35 | .37 | .31 |
     | 5 | .81 | .77 | .18 |
     | 20 | .96 | 1.09 | .09 |
     | aim-partial reference | .82 | .99 | .24 |

   - This lane changes no constant. Root may want to register a mechanical criterion (the note suggests
     corr(w, w_aim) >= .9) or revisit S_prior before spending the trial. The part-2 diagnostics will record
     corr(w, w_aim).
2. Not compiled: expect one `/W4 /WX` pass.
   - Eigen is reached only through `atx::core::linalg::symmetric_eig`.
   - Watch for `Eigen::Index` conversions and a `-Wmissing-field-initializers` on `TrackingLimit`-typed aggregates.
3. Iteration counts on the real 1,850-name, 62-factor problems are unmeasured. The cap is 2000; unconverged solves are
   reported, not refused.
4. Warm-start commit: a specific-variance clamp inside the warm-up no longer reaches the tripwire (as instructed). The
   risk model's own `diagnostics.csv` still shows it.

## 4. Cross-lane edits

- `atx-engine/CMakeLists.txt`, +8 lines: `src/book/target_tracking.cpp` added to the `atx-engine` source list after
  `src/book/security_transition.cpp`, and a Debug `/O2 /Ob2 /clang:-finline` + `SKIP_PRECOMPILE_HEADERS` block for
  it inside the existing MSVC/Clang/Debug `if`.

## STOPPED HERE

Done: section 1 (committed) and part 1, the engine solver with its gtests (committed).

Remains: part 2, the atx-impl rule. Nothing of it is written. These design decisions were taken so another lane can
pick it up unchanged.

**Files**
- `strategy_spo.{hpp,cpp}`:
  - `Engine::Impl::plan_tracking`, dispatched from `Impl::plan` when `params.version == 3`.
  - `prepare()` skips the v1 alpha and the FISTA metric for v3.
  - Three verbatim extractions from `plan()`, used by both paths: the fixed positions (nonmember exit, unpriced
    hold, fixed exposure / net / beta), the per-name market terms (trade limit, locate guard, impact coefficient,
    financing rates) and the plan-field accumulation. They must stay expression-identical so the SpoPin digests
    hold.
- New `strategy_spo_v3.cpp`: the v3 problem statement, `v3_params`, declaration, parameters/calibration JSON,
  `TrackingRow` CSV, per-book summary and the v3 tripwire. Needs `atx-impl/CMakeLists.txt`: source list + Debug /O2
  list (cross-lane).
- `strategy_nav_v7.{hpp,cpp}`:
  - `spo_rule` gains `spo-v3`; `spo_flag` gains `--spo-alpha`.
  - `write_extras` / `extend_summary` / `capture` call engine-level dispatch functions (`rows_csv`,
    `rows_summary_json`, `rows_tripwire_json`, `rows_tripwire`) that delegate to the existing v1/v2 functions
    unchanged.
- No edit to `atx-impl/tools/equity_strategy_targets.cpp`: `nav` routes through `v7::claims_nav_args`.

**Parameters**, `v3_params()`:

| field | value | note |
|---|---|---|
| version | 3 | |
| H | 20 | fixed, not 1/theta |
| S_prior | 1.0 | annualised |
| gross bound | 2 x `--aim-leverage` | a checked sanity bound, not a solver constraint |
| adv_trade_p | .01 | |
| beta_max | .02 | |
| specific ceiling | 1.0, void on | the v2 tripwire, kept |
| iterations / tolerance | engine constants | |
| alpha | implied-aim | |

- gamma = S_prior / sigma_aim, with sigma_aim = sqrt(252 w_aim' Sigma w_aim) of the whole aim at the first rebalance
  decision. Sigma is daily, so the aim's implied annual Sharpe is S_prior. Refuse (Unavailable) if sigma_aim is not
  finite and positive.

**Problem assembly** per rebalance decision and book:
- w_aim = L x desired on the members.
- Optimised names = members with a risk row. Nonmembers exit per aim-partial-v5; unpriced members hold.
  `external_gap` = their factor exposure.
- trade limit = p ADV/NAV (0 without ADV); lower = min(w0, 0) where the locate rule guards the name, else -inf;
  upper = +inf. No holding cap: it is not in the registration.
- s = (half spread + commission)/H; eta = impact_y sigma sqrt(NAV/ADV)/H (primary S2 law); b = the book's short
  financing per session (v1's `short_rate`). No long-financing term.
- net: 1'w = -net_fixed. beta band: +-.02 minus the fixed beta.
- Warm dual kept per book per instrument.

**CLI**
- `--rule spo-v3`, with optional `--spo-alpha implied-aim` (the only value; refused with v1/v2).
- Refused with v3 (registered constants): `--gamma --ic-book --w-max --adv-cap-q --adv-trade-p --target-vol
  --spo-horizon --alpha-horizon --spo-gross`.
- Allowed with v3: `--risk-model(-sha256) --spo-iters --spo-tol --spo-books --specific-ceiling(-void)`.
- As v1/v2: capacity curve refused, fixed rate required, `--emit-holdings` refused with the void on.

**Diagnostics** (`spo_diagnostics.csv` for v3, its own columns):
- session, book, members, optimized, unpriced_members, fixed_nonmembers, gamma
- iterations, converged, limits_met, primal_residual, dual_residual, limit_violation, clipped_eigenvalues
- tracking_error, tracking_error_current (annualised, whole book), aim_correlation
- objective, trade_cost, amortized_cost, borrow
- gross, aim_gross, net, long, short, abs_beta, turnover
- no_trade, at_trade_limit, trade_limit_share, at_locate_floor, gross_bound_breached, nu, rho, capped_specific
- shadow aim-partial-v5: gross, turnover, trade_cost, tracking_error, aim_correlation

**Tripwire v3**
- Status is void when the void switch is on and either a clamped decision or a gross-bound breach occurred.
- Report-only: TE mean/max, share at the trade limit mean/max, corr(w, w_aim) mean/min, unconverged, limits unmet.

**Tests**
- `strategy_spo_v3_test.cpp`: `SpoV3.ZeroCostNoLimitsReturnsAimTo1e8` (direct `Engine::plan` with a zero-cost S2
  scenario, zero financing, ADV 1e15, beta_max 1), `SpoV3.GrossCapIsSlackOnFixture`,
  `SpoV3.ReportsTrackingErrorAndShareAtTradeLimit`.
- `strategy_spo_v3_pin_test.cpp`: `SpoV3.V1AndV2DigestsUnchanged`. It uses the base API only.
  - v1 digests = the existing SpoPin pins.
  - v2 digests: capture on the pre-R6 base with this one file added, then pin (W1b protocol).
- Both files go in the `atx-impl-strategy-target-tests` list (cross-lane edit, `atx-impl/tests/CMakeLists.txt`).

**Planned root argv after part 2** (not runnable yet):
- Base: the v7.1-cell nav argv with the lo3 role, fields and the 4-year role once W0-2 lands.
- Add: `--rule spo-v3 --spo-alpha implied-aim --risk-model <atx-risk-v1.1 on the 4-year role>
  --risk-model-sha256 <pin> --spo-books primary`.
- Read `v7_extras.json` `spo_v3.tripwire.status == "clear"` before any return.
