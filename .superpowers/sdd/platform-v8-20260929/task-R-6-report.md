# Task R-6 report: target-tracking optimiser spo-v3 (S-8) -- parts 1 and 2 built

Lane R6, worktree `C:/atx-wt/pool-7`, branch `feat/platform-v8-r6-20260929`. Not built and not run (lane rules).
Every claim about behaviour below is from reading the code, or from a numpy port run on synthetic data ([proto]).

| commit | content |
|---|---|
| `75774cd8` | PM addition: v7 side files and the spo tripwire cover scored decisions only under `--warm-start-sessions K` |
| `d695cbd8` | part 1: `atx::engine::book::solve_tracking` (the generic solver) + `TargetTracking.*` gtests |
| `a7a26df1` | step 0: merge of root `feat/platform-v8-20260929` (41ac94fd, integration 3) into the lane; no conflict |
| `3a59c5bc` | part 2: the atx-impl rule `--rule spo-v3 --spo-alpha implied-aim` + `SpoV3.*` gtests (section 5) |

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

## 3. Open risks of part 1

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

   - RESOLVED by PM Ruling E-14 (2026-09-30, pre-read amendment): S_prior = 20, and the cell gains the
     mechanical criterion "mean correlation of the traded book with the aim over scored decisions >= .9". Part 2
     codes S_prior 20 as `spo::v3_sharpe_prior` and reports the correlation (section 5).
2. Not compiled: expect one `/W4 /WX` pass.
   - Eigen is reached only through `atx::core::linalg::symmetric_eig`.
   - Watch for `Eigen::Index` conversions and a `-Wmissing-field-initializers` on `TrackingLimit`-typed aggregates.
3. Iteration counts on the real 1,850-name, 62-factor problems are unmeasured. The cap is 2000; unconverged solves are
   reported, not refused.
4. Warm-start commit: a specific-variance clamp inside the warm-up no longer reaches the tripwire (as instructed). The
   risk model's own `diagnostics.csv` still shows it.

## 4. Cross-lane edits of part 1

- `atx-engine/CMakeLists.txt`, +8 lines: `src/book/target_tracking.cpp` added to the `atx-engine` source list after
  `src/book/security_transition.cpp`, and a Debug `/O2 /Ob2 /clang:-finline` + `SKIP_PRECOMPILE_HEADERS` block for
  it inside the existing MSVC/Clang/Debug `if`.

## 5. Part 2: the atx-impl rule `--rule spo-v3 --spo-alpha implied-aim` (`3a59c5bc`)

Built to the design this report recorded at the stop, with PM Ruling E-14 (S_prior = 20) and the deviations listed
at the end of this section.

### Files

- `atx-impl/src/strategy_spo_v3.hpp` (new): the problem statement and the registered constants:

  | constant | value | note |
  |---|---|---|
  | `v3_horizon` | 20 | H, sessions: fixed, not 1 / theta |
  | `v3_sharpe_prior` | 20 | S_prior. The comment cites "Ruling E-14" (pre-read amendment; 1.0 was the first declaration) |
  | `v3_adv_trade_p` | .01 | trade limit p ADV / NAV |
  | `v3_beta_max` | .02 | |
  | `v3_specific_ceiling` | 1.0 | void on: a clamp is a tripwire |
  | `v3_gross_bound_multiple` | 2 | breach: planned gross > 2 x `--aim-leverage` |

  - Also declares `v3_params()` and the published blocks: `tracking_declaration`, `tracking_parameters_json`,
    `tracking_calibration_json`, `tracking_csv`, `tracking_units_json`, `tracking_summary_json`,
    `tracking_tripwire`, `tracking_tripwire_json`.
- `atx-impl/src/strategy_spo_v3.cpp` (new): those functions, plus the Engine's rule-level dispatch members (they
  use only the Engine's public accessors).
- `atx-impl/src/strategy_spo.hpp`:
  - `SpoParams::sharpe_prior` (NaN; v3 only).
  - `TrackingRow`.
  - New `Engine` members:
    - `tracking_rows()`.
    - `rule_declaration()`, `rule_parameters_json()`, `rule_calibration_json()`.
    - `rows_csv()`, `rows_units_json()`, `rows_summary_json()`, `rows_tripwire_json()`, `rows_tripwire()`.
  - Under spo-v1/v2 each dispatch member calls the old function with the old arguments.
- `atx-impl/src/strategy_spo.cpp`:
  - `validate_params` accepts version 3 and checks S_prior in (0, 1e3]. `rule_name` / `json_key` give
    `spo-v3` / `spo_v3`.
  - `Impl::plan` returns `plan_tracking` when `version == 3`. The dispatch comes after the geometry and S2-law
    checks and after H is set.
  - `prepare()` skips the alpha and the FISTA metric under v3.
  - Three extractions from `plan()`, now used by both paths, with the expressions copied unchanged:
    - `Impl::fixed_positions`: nonmember exit, unpriced hold, fixed exposure, net, gross and beta.
    - `Impl::market_terms`: holding cap, trade limit, locate guard and floor, impact coefficient, financing rates.
    - free `accumulate_plan`: the plan fields.
  - spo-v3: `calibrate_tracking`, `tracking_problem`, `tracking_row`, `plan_tracking`, and a free
    `tracking_error`.
- `atx-impl/src/strategy_nav_v7.{hpp,cpp}`:
  - `spo_rule` gains `spo-v3`; `spo_flag` gains `--spo-alpha`; new `refused_with_v3`.
  - The parser: v3 defaults, then the refusals, then `--spo-alpha`.
  - `declarations` / `write_extras` / `extend_summary` / `capture` go through the Engine members.
  - `--help` and the header comment document v3.
- Tests:
  - New: `strategy_spo_v3_test.cpp`, `strategy_spo_v3_pin_test.cpp`, `strategy_spo_digest.hpp`.
  - `strategy_spo_pin_test.cpp` is refactored onto the shared digest header.

### The problem as coded (per rebalance decision and book)

- w_aim = L x desired on the members. Optimized names are the members with a risk row. Nonmembers exit per
  aim-partial-v5 and unpriced members hold (`fixed_positions`). `external_gap` is the fixed positions' factor
  exposure: their aim is 0, and unpriced names have no risk row.
- Per-name terms:
  - s = (half spread + commission) / H and eta = impact_y sigma sqrt(NAV / ADV) / H (primary S2 law).
  - b = the book's short financing per session. No long term.
  - trade limit p ADV / NAV (0 without ADV).
  - lower = min(w0, 0) where guarded, else -inf; upper = +inf (no holding cap).
- Limits: net 1'w = -net_fixed (an equality band); beta in +-.02 - beta_fixed.
- Solver: `tt::solve_tracking(problem, {--spo-iters, --spo-tol}, warm)`. Defaults 2000 / 1e-9 = the engine
  constants.
- Warm dual: kept per book per instrument. A name new to the problem starts at 0. `begin_run` clears it (every
  pass starts cold).
- gamma = S_prior / sigma_aim, set at the first plan call. sigma_aim = sqrt(252 x `book_variance(slice, aim)`)
  over the whole aim. Unavailable if sigma_aim is not finite and positive. The calibration records session, gamma,
  sigma_aim (`aim_vol`), the aim's gross and the name count.
- `out.construction.banded_names` = the solver's no-trade count, as v1/v2.
- Scored decisions only (`d >= decision_begin`) leave a row, as v1/v2. The shadow and the warm dual still move in
  the warm-up.

### `spo_diagnostics.csv` under spo-v3 (41 columns)

`session, book, members, optimized, unpriced_members, fixed_nonmembers, gamma, iterations, converged, limits_met,
primal_residual, dual_residual, limit_violation, clipped_eigenvalues, tracking_error, tracking_error_current,
aim_correlation, objective, trade_cost, amortized_cost, borrow, gross, aim_gross, net, long, short, abs_beta, turnover,
no_trade, at_trade_limit, trade_limit_share, at_locate_floor, gross_bound_breached, nu, rho, capped_specific,
gross_shadow, turnover_shadow, trade_cost_shadow, tracking_error_shadow, aim_correlation_shadow`

- tracking_error*: annualised, whole book, priced names.
- aim_correlation*: Pearson correlation of the planned (shadow) and the aim weights over the optimized names. It is
  NaN when either has no dispersion, e.g. a flat plan.
- Units are in `diagnostics_units`.

### Tripwire and report (`v7_extras.json` `spo_v3.tripwire`, `summary.json` `v7.spo_v3_tripwire`)

- `status`:
  - `void` when `--specific-ceiling-void` is on (the default) and any scored decision clamped a specific variance,
    or any book planned gross above 2 x L. The run then exits 3 with diagnostics only, as spo-v2.
  - `tripped (not voiding ...)` when the void is off.
  - `clear` otherwise.
- Also recorded: `capped_specific_decisions`, `capped_specific_names_max`, `gross_bound_multiple`,
  `gross_bound_breaches` (book decisions) and `max_gross`.
- `report_only.<book>`: `decisions`, `tracking_error {mean, max, n}`, `trade_limit_share {mean, max, n}`,
  `aim_correlation {mean, min, n}`, `unconverged`, `limits_unmet`.
  - The same block heads each book's entry of `spo_v3.books` / `v7.spo_v3_books`.
  - Those entries add the mean iterations and residuals, mean current TE, cost, gross and turnover, the holding
    period, breaches, clamps, the shadow (cost, gross, turnover, TE, aim correlation) and `trade_cost_ratio`.
- **E-14 criterion** (report-only; the PM judges it): the primary book's
  `spo_v3.tripwire.report_only["modeled-1bn-stale5-v1+<financing id>"].aim_correlation.mean >= .9`. NaN rows are
  excluded; `n` counts the rows that were averaged.

### CLI

- `--rule spo-v3` loads `v3_params()`. `--spo-alpha implied-aim` is optional. It is refused with spo-v1/v2, for any
  other value, and twice.
- Refused with v3, wherever they stand on the line: `--gamma --ic-book --w-max --adv-cap-q --adv-trade-p
  --target-vol --spo-horizon --alpha-horizon --spo-gross`.
- Allowed: `--risk-model(-sha256) --spo-iters --spo-tol --spo-books --specific-ceiling(-void)`.
- As v1/v2: no capacity curve, fixed rate only, no `--emit-holdings` with the void on, one `--rule` only.

### Tests (`atx-impl-strategy-target-tests`; the glob also puts them in `atx-impl-tests`)

| test | checks |
|---|---|
| `SpoV3.ZeroCostNoLimitsReturnsAimTo1e8` | direct `Engine::plan`: zero-cost S2 law, zero borrow, ADV 1e15, beta band +-1, flat, all names optimized. Plan = L x desired to 1e-8; converged, limits met, TE < 1e-6, correlation 1; gamma = 20 / sigma_aim; TE_current = sigma_aim; H 20; bound 2L |
| `SpoV3.GrossCapIsSlackOnFixture` | fixture replay under `v3_params`: no breach, every gross < 2L, max < .75 x 2L, tripwire clear. A forced breach voids with the void on and is `tripped` with it off |
| `SpoV3.ReportsTrackingErrorAndShareAtTradeLimit` | first row's TE_current = sigma_aim. The first trading decision from flat converges, cuts TE and has names at the trade limit. share = at / optimized; correlation in [-1, 1] (NaN only on flat rows); net/beta held where limits were met; CSV rows and 41 columns; summary and tripwire report equal the rows' mean / max / min / counts |
| `SpoV3.ParseRefusesTheRegisteredConstantsAndRoutesTheImpliedAim` | v3 defaults (S_prior 20), allowed overrides, the nine refusals (before or after `--rule`), `--spo-alpha` rules, capacity / rate / holdings refusals, the `spo_v3` recipe block and the relabelled rule |
| `SpoV3.V1AndV2DigestsUnchanged` | spo-v1 weights and replay digests against SpoPin's pins. spo-v2 (`v2_params`, w_max .5, Role(30, 12, 71), model seed 9, every CSV column) against its pins, which are a **0 placeholder**: the test SKIPS after the v1 checks until root captures them (section 6) |

### Deviations from the stop-time design

1. S_prior is 20 (Ruling E-14), not 1.0.
2. Dispatch covers the declaration, the parameters and the calibration too (`rule_*`), so nav_v7 never branches on
   the version. The members are defined in `strategy_spo_v3.cpp`.
3. The SpoPin digest procedures moved into `strategy_spo_digest.hpp` rather than being copied into the new pin test.
   The fold order and the `[spo-pin]` prints are unchanged, and the pins are untouched.
4. The v2 digest fixture differs from SpoPin's. Under v2 defaults (w_max .01), SpoPin's Role(40, 12, 53) cannot
   reach the vol target, so v2 is refused there. v2 uses the spo-v2 hook tests' fixture instead.
5. The report is keyed per book (`report_only.<book>`) because the criterion is the primary book's.

## 6. How root verifies

1. Compile each TU first:
   - `powershell scripts\atx-build.ps1 check atx-impl\src\strategy_spo.cpp`
   - the same for `atx-impl\src\strategy_spo_v3.cpp`, `atx-impl\src\strategy_nav_v7.cpp` and
     `atx-engine\src\book\target_tracking.cpp`.
2. Build: `powershell scripts\atx-build.ps1 build atx-engine-book-tests atx-impl-strategy-target-tests`. Add
   `atx-equity-strategy-targets` for the real-data runs.
3. Run the gtests:
   - `build\bin\atx-engine-book-tests.exe --gtest_filter=TargetTracking.*`
   - `build\bin\atx-impl-strategy-target-tests.exe --gtest_filter=Spo*:NavV7Hook.*`. SpoPin must pass with its old
     pins; this proves the digest refactor. `SpoV3.V1AndV2DigestsUnchanged` passes its v1 checks, then SKIPS until
     step 4.
4. **spo-v2 pin capture** (never invent a digest):
   1. Take a pool tree on the integration head without R6 (`feat/platform-v8-20260929` at 41ac94fd, or its
      successor before the R6 merge).
   2. Copy `atx-impl/tests/strategy_spo_digest.hpp` and `atx-impl/tests/strategy_spo_v3_pin_test.cpp` from
      `3a59c5bc`. Both use only pre-R6 API.
   3. Append `strategy_spo_v3_pin_test.cpp` to the `atx-impl-strategy-target-tests` list in
      `atx-impl/tests/CMakeLists.txt`.
   4. Build that target and run `--gtest_filter=SpoV3.V1AndV2DigestsUnchanged`. The v1 checks pass, then the test
      skips. Read the line `[spo-v3-pin] v2 weights=0x... replay=0x...`.
   5. On the R6 head, set `pinned_v2_weights` / `pinned_v2_replay` in `strategy_spo_v3_pin_test.cpp` to those two
      values. Rebuild. The test must print the same values and pass.
5. **Identity runs on real data, with the flag off:**
   - (a) The v7.1 cell (aim-partial-v5, no spo rule): rerun its recorded nav argv with the new exe. Every output file
     must be byte-identical. No-rule runs never enter the spo code.
   - (b) spo-v2: rerun the recorded spo-v2 cell argv (W1b report, step 4). Byte-identical: `spo_diagnostics.csv`,
     `v7_transfer_coefficient.csv`, `v7_extras.json`, `recipe.json`, `summary.json` and the NAV / daily / events
     files. The warm-start commit is a no-op at K = 0.
   - (c) spo-v1: the SpoPin digests. A recorded spo-v1 cell may be rerun the same way as (b).
6. **Trial argv delta** for the spo-v3 cell, after W0-2's 4-year role and atx-risk-v1.1 on it with its manifest pin:
   - Start from the v7.1-cell nav argv on the registered role.
   - REPLACE `--rule aim-partial-v5` with `--rule spo-v3 --spo-alpha implied-aim`. The hook rewrites it to
     aim-partial-v5; a second `--rule` is refused.
   - Keep the aim-partial-v5 flags (`--trade-fraction`, `--dust-multiple`, `--aim-leverage`, `--exit-rate`,
     cadence). They drive the exits, the shadow and L. H stays 20.
   - ADD `--risk-model <dir> --risk-model-sha256 <pin> --spo-books primary`.
   - The line must carry: fixed rate, no `--capacity-curve`, no `--emit-holdings`, `--book-workers 1` (the hook is
     thread-local), and the cell's `--warm-start-sessions K`.
   - Before any return: a tripped run exits 3 with status `void` and no NAV file. Otherwise read `v7_extras.json`
     `spo_v3.tripwire.status == "clear"`, then the E-14 criterion from the report block (section 5).

## 7. Cross-lane edits of part 2

- `atx-impl/CMakeLists.txt`: `src/strategy_spo_v3.cpp` in the `atx-impl-core` source list (after
  `strategy_spo.cpp`) and in both Debug `/O2 /Ob2` + `SKIP_PRECOMPILE_HEADERS` lists.
- `atx-impl/tests/CMakeLists.txt`: `strategy_spo_v3_test.cpp` and `strategy_spo_v3_pin_test.cpp` appended to
  `atx-impl-strategy-target-tests`.
- `atx-impl/tests/strategy_spo_pin_test.cpp`: its digest procedures moved to `strategy_spo_digest.hpp`. Pins and
  prints are unchanged.
- The step-0 merge (`a7a26df1`) had no textual conflict. Nothing was resolved by hand, and no window or digest
  constant was touched.

## 8. Open risks of part 2

1. **Not compiled.** Expect a `/W4 /WX` pass. Points to watch:
   - The namespace alias `tt` (engine book) is declared in `strategy_spo.cpp`'s anonymous namespace and used in the
     `Engine::Impl` member declarations.
   - `tt::TrackingLimit{lo, hi}` / `tt::TrackingOptions{iters, tol}` aggregates.
   - `ATX_TRY(const auto sol, tt::solve_tracking(p, options, warm))`, where `warm` is a `std::vector` converted to a
     span.
   - The nlohmann `Json` comparisons in the tests.
2. **The v2 pins are a placeholder** (the test SKIPS) until the capture in section 6, step 4.
3. **Beta feasibility under the one-session trade limit.** When a book's beta jumps more than one session's trade
   limits can repair, the solve runs to the 2000-iteration cap. It is reported (`limits_met` 0, `unconverged`) and
   never refused.
   - The fixture redraws style exposures daily, so the fixture tests check net and beta only on rows that met their
     limits, and check the counts.
   - On real data, each such decision costs the iteration cap in runtime. Read `unconverged` and `limits_unmet` in the
     report.
4. `GrossCapIsSlackOnFixture` asserts max planned gross < .75 x 2L = 1.8. That threshold is reasoned, not measured:
   aim gross 1.2 plus the hedge of one decaying nonmember. If it fails while no row breaches, the threshold is wrong,
   not the rule.
5. On a flat start, a decision with an empty liquidity window (the fixture's first) has ADV 0, so its trade limit is
   0 and nothing trades, as in v1/v2. Its aim correlation is NaN and falls out of the criterion's `n`.
6. Iteration count and runtime at 1,850 names are still unmeasured (part 1, risk 3).
7. Cosmetic: the `summary.json` `v7.extras` string still reads "spo_diagnostics.csv (spo-v1/v2)" under v3. It was
   kept so the v1/v2 bytes stay identical.
