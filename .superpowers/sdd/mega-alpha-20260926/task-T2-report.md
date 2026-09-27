# Task T2 report: NAV replay of the saved blend ($1bn, costs, causal missing-price policy)

Status: implemented and committed, not compiled or run (per the brief, the root builds).
Worktree `C:/atx-wt/pool-5`, branch `feat/mega-alpha-nav-20260926`, base `5c9cbaed`.
Commit: `8e25992d` feat(strategy): $1bn NAV replay of pinned saved blend [mega T2] (single commit,
6 files, includes the root addendum for pinned-candidate-weights blends). Nothing pushed.

Root's `af8c38ee..b23d463e` do not touch `strategy_target_replay.*` or `tools/equity_strategy_targets.cpp`,
so the commit should cherry-pick cleanly. The only overlap is CMake, which the root owns (see below).

## Files
| File | Change |
|---|---|
| `atx-impl/src/strategy_target_replay_detail.hpp` | NEW. Private seam: `LoadedSavedBlend`, `load_saved_blend`, `validate_replay_input`, `desired_target`, `update_weights`, `calendar_month`. Includes only `strategy_target_replay.hpp`, the core error/types headers and std. No nlohmann. |
| `atx-impl/src/strategy_target_replay.cpp` | Append-only `detail::` forwarding wrappers (:579-630). `SavedBlend::volume`. `admit_saved(..., with_volume=false)` charges 36 B/cell instead of 28 only when volume is requested (:331). `load_prices(..., with_volume=false)` checks `volume_basis == "raw-share-volume"` (:364) and reads `volume.f64` with the engine contract: present means finite and >= 0, absent means NaN (:396-402). Root addendum: `admitted_signal_semantics` (:286-302). `run_target_replay`, `replay_targets` and all arithmetic are unchanged. |
| `atx-impl/src/strategy_nav_replay.hpp` | NEW public (private-to-impl) API: `NavCostRule`, `NavScenario`, `fixed_nav_scenarios()`, `NavReplayConfig`, `NavReplayInput`, `NavReplayDay`, `NavEvent`, `NavBucket`, `NavReplayResult`, pure `replay_nav`, pure `summarize_nav` + `NavSummary`/`NavMonth`/`NavYear`, `run_nav_replay`, `dispatch_nav_replay`. Also constants `nav_adverse_long_return` (-0.55), `nav_adverse_short_return` (+0.30), `nav_sharpe_target` (1.0), `nav_monthly_turnover_target` (0.30), `nav_primary_scenario_index` (1). |
| `atx-impl/src/strategy_nav_replay.cpp` | NEW, about 1060 lines. `replay_cost.hpp`, `hac.hpp`, nlohmann and sha256 are included only here. |
| `atx-impl/tools/equity_strategy_targets.cpp` | `argv[1] == "nav"` calls `dispatch_nav_replay(argc-1, argv+1, ...)`. Anything else goes to the existing `dispatch_target_replay`, unchanged. |
| `atx-impl/tests/strategy_nav_replay_test.cpp` | NEW, 10 fixtures (the 9 in the design plus 1 for the root addendum). |

## Key logic (strategy_nav_replay.cpp line numbers at 8e25992d)
- Input and config validation.
  - `validate_nav_config` (:127). Scenario bounds; scenario id limited to `[a-z0-9-]` because it is used in file names. Refuses a nonzero `one_way_bps` or `annual_borrow_bps`.
  - `validate_nav_input` (:150). Runs the target replay's own `validate_input` through `detail::validate_replay_input`. Requires prices and volume and at least 3 window sessions. Present cells need close and raw finite > 0 and volume finite >= 0; member implies present.
  - Workspace budget (O(N) + days + events at cap) is charged against `max_working_bytes` and excludes caller-owned input.
- Causal liquidity row `liquidity_row` (:210).
  - Window is [t-w, t). ADV = sum of present raw_close × volume / w; absent rows count as 0 and the window is never rescaled.
  - Sigma is the sample SD of adjusted simple returns over adjacent present, unguarded pairs (k-1, k), k in [t-w, t). This is the same pair convention as `strategy_runner.cpp` (up to 63 pairs).
  - Below `min_vol_pairs` pairs, sigma falls back to 0.05/day.
- Guard `guarded_move` (:195): the same predicate as the target replay (|adj log| > 1.5 or > |raw log| + 0.10).
- MARK `mark_session` (:321), with `mark_flat` (:257), `carry_absent` (:277) and `realize` (:292).
  - Borrow is charged on pre-mark short dollars × b × calendar days / 365.
  - Held and present: realize close/p̂ over the gap, emit a gap-resolved event and a guard event.
  - Held and absent: `a_i++`. At `a_i >= K`, write off: W += h·η and C += h(1+η), then zero the holding and cancel its order.
  - Flat and present after a write-off: emit a `reappeared-after-write-off` event with forgone pnl h·(r_adj − η). This never touches NAV.
  - NAVpre_t = NAVpost_{t-1} + P + W − B, so the previous session's fill costs land in r_t.
  - The identity `r = gross − cost − borrow` is checked (hard refusal above 1e-9).
- EXECUTE `execute_orders` (:350).
  - Absent means blocked and the order stays.
  - `ReplayCostModel::cost` on the causal row. A complete fill lands exactly on the target; a capped fill leaves a working residual; an unusable ADV means no fill (`blocked_liquidity`).
  - `unrationed_cost` is recorded for the full request (NaN is counted as unpriced).
  - Linear/impact split, participation histogram. Costs go to cash; `one_way_turnover = traded / NAVpre_t`.
- Extension point `form_desired_target` (:389). This is the only call of `detail::desired_target` in the NAV path. It is called only from `plan_decision`, after the tied-rank target and before the partial/budget planning. A later neutralization step post-processes `s.desired` here. Neutralization is not implemented; the recipe records `"desired_target_postprocess": "none"`.
- DECIDE `plan_decision` (:398).
  - cur = held / NAVpost (stale names at stale marks).
  - Unchanged `detail::update_weights` (v2 budget spent by decision month).
  - Every name with next != cur gets an order at next × NAVpost_d, zero exits included.
  - On rebalance days, members with next == cur cancel their orders; flat nonmembers cancel every day.
- `close_day` (:424): end-of-session exposures, stale $, and cash ratio. Hard check that cash + Σh reconciles to NAVpost (relative 1e-9).
- `report_unresolved` (:446): at the final session, still-stale holdings stay at their stale mark and get an `unresolved-at-end` event.
- `run_book` (:458): sessions [begin, end). MARK for t > begin, EXECUTE for begin < t <= end−2, DECIDE for t < end−2. The final session is valuation only.
- `fixed_nav_scenarios` (:817), exactly as ruled:
  - S1 `linear-6bps-stale5-v1`: FlatBps 6, uncapped, borrow 300, K=5, η=0.
  - S2 `modeled-1bn-stale5-v1` (PRIMARY): SqrtImpact y .6, δ .5, cap .01, commission 1, half spread 5, borrow 300, K=5, η=0.
  - S3 `modeled-1bn-terminal-adverse-v1`: S2 with K=1 and η = −0.55 long / +0.30 short.
- `replay_nav` (:833) is pure, with no I/O. `summarize_nav` (:852) is pure. `run_nav_replay` (:922) and `dispatch_nav_replay` (:988) are the I/O layer.
- Summary per scenario (`scenario_summary` :705, `turnover_json` :638, `risk_json` :665):
  - Returns: observations, net and gross Sharpe (sample SD, √252), HAC t (Bartlett lag 5, `mean_inference(...,5,true,true)`), annualized mean and vol, CAGR, max drawdown, total return, calendar-year returns.
  - Turnover:
    - `turnover_definition` text.
    - Month rows (execution-month turnover, traded $, execution and traded sessions, deployment flag, decision-month planned turnover).
    - `mean_monthly_one_way_turnover` (all execution months) and `mean_monthly_one_way_turnover_ex_deployment_month`, with the two matching max values.
    - `months_le_0.30` counts.
    - Deployment {session, turnover, $}.
    - Planned-vs-actual reconciliation, including the monthly reconciliation error.
  - Costs by component (linear, impact, unrationed, borrow, write-off), guard sensitivity Σh(r_raw − r_adj), missing histogram (gap 1 / 2-4 / 5+ / written off / reappeared / unresolved, each with count, gross $ and pnl $), exposure stats, capacity (fills, capped, unfilled $, liquidity-blocked, fallback fills, participation p95/max).
  - Accounting-check maxima, `meets_*` flags, event count, CSV SHAs.
  - Top level: `source_bindings` (the pinned manifest), `signal_semantics`, `composition_weights_sha256` (null for equal-weight blends), `capacity_qualified: false`, limitations.
- Admission (`nav_reserve_bytes` :810, applied in `run_nav_replay`).
  - A fixed NAV reserve of about 84 MB is carved out of `--max-bytes` before the loader charges its 36 B/cell. The reserve covers 3 results with days and events at cap, per-name state and publication slack. All scenarios are computed before the output directory is created.
  - TRAIN estimate: 64 MiB metadata + 6.50M × 36 B + small ≈ 301 MB, plus the reserve, ≈ 385 MB, under the 512 MiB default.

## Root addendum: pinned-candidate-weights blends
`admitted_signal_semantics` (strategy_target_replay.cpp :286-302) admits exactly the two strings the root's IC runner writes (`strategy_ic_runner.cpp:250-252`):
- `exact-pre-target-composition;equal-family/equal-within;missing-or-unoriented-neutral-fixed-denominator`. This requires that `composition_weights_sha256` is **absent**, matching what the runner writes.
- `exact-pre-target-composition;pinned-candidate-weights;missing-or-unoriented-neutral-fixed-denominator`. This requires `composition_weights_sha256` to be a valid 64-hex string.

Anything else is refused as a "saved blend contract" error.

The weights SHA remains in the manifest, so it appears in `source_bindings` for both the target replay and the NAV summaries. The NAV summary also mirrors it at top level. The 7 existing target-replay fixtures use the equal string with no key, so they are unaffected.

## CMake lines the root must add (not edited by me)
`atx-impl/CMakeLists.txt`:
```cmake
# atx-impl-core source list, next to src/strategy_target_replay.cpp:
    src/strategy_nav_replay.cpp
# Debug /O2 block: append to BOTH lists (set_property ... COMPILE_OPTIONS /O2 /Ob2 /clang:-finline
# and set_source_files_properties ... SKIP_PRECOMPILE_HEADERS ON):
    src/strategy_nav_replay.cpp
```
`atx-impl/tests/CMakeLists.txt`:
```cmake
add_executable(atx-impl-strategy-target-tests EXCLUDE_FROM_ALL
    strategy_target_replay_test.cpp
    strategy_price_exposures_test.cpp   # already on root HEAD
    strategy_nav_replay_test.cpp)
```
Note: the `*_test.cpp` glob of the big `atx-impl-tests` target will also pick up the new test TU, as it already does for `strategy_target_replay_test.cpp`. The test includes `atx/engine/book/replay.hpp` and `replay_cost.hpp` (both reachable through atx-impl-core → atx::engine).

Build / run suggestion: `check atx-impl\src\strategy_nav_replay.cpp`, `check atx-impl\src\strategy_target_replay.cpp`, `build atx-impl-strategy-target-tests atx-equity-strategy-targets`, then `-Ctest -R "StrategyTargetReplay|StrategyNavReplay"` (or run the exe directly with `--gtest_filter`).

CLI:
```
atx-equity-strategy-targets nav --combined PATH --combined-sha256 SHA --role PATH --role-sha256 SHA --output NEWDIR [--rule baseline-v1|monthly-budget-v2] [--cadence 5] [--trade-fraction .25] [--monthly-budget .30] [--max-bytes 536870912]
```
`--one-way-bps` and `--annual-borrow-bps` are rejected (exit 2), and `--role` is required. The command always runs all three scenarios. Outputs:
- `recipe.json` (`atx.dsl-nav-replay/v1`).
- `daily_<S>.csv` and `events_<S>.csv` for each scenario.
- `summary.json` (`atx.dsl-nav-replay-summary/v1`, status complete), written last.

## Fixtures (`atx-impl/tests/strategy_nav_replay_test.cpp`, suite `StrategyNavReplay`)
1. `ConstantPricesNoCostReproducesTargetReplayPlanned` (:279). NAV 1.0, 0 bps, 0 borrow, constant prices, for baseline and v2 (cadence 1, fraction .5, budget .30). Planned turnover, gross, net, forced, applied fraction, month-planned and budget excess are bit-equal to `replay_targets`. traded$ and turnover at d+1 are bit-equal to the plan at d. NAV stays exactly 1.
2. `TwoNameHandComputedDriftCostBorrow` (:315). Thu/Fri/Mon/Tue with 6 bps and 300 bps borrow (3-day weekend accrual). Hand-computed NAV path, r = gross − cost − borrow, cash ratio and summary totals.
3. `InteriorGapCarriesStaleThenRealizesCumulativeReturn` (:362). Checks the zero return while stale, the blocked exit, the +20% realized at the reprint and then the exit at that close, and the gap-resolved event (run 2).
4. `TerminalWriteOffAfterKIsCausal` (:391).
   - S3 write-off at the first absence: −55% long, +30% loss on the short.
   - Reappeared event (run 3, forgone 0.75·h), with zero NAV effect.
   - Rewriting all rows ≥ 5 leaves rows 0-4 and the earlier events bit-identical.
   - S2 on the same panel carries and realizes (run 3) and reports an unresolved stale short at the end.
5. `FutureSignalAndPricesDoNotChangePast` (:441). Random panel with gaps and present nonmembers. Rewriting rows ≥ m leaves rows < m and the earlier events bit-identical, for both rules × S2/S3.
6. `ParticipationCapWorkingOrderAndSqrtCost` (:466).
   - Fills are capped at 1% ADV, and the cost equals `SqrtImpactCost` on the independently rebuilt row. The residual working order completes the next session.
   - The zero-volume name stays liquidity-blocked (unrationed cost unpriced).
   - The name with too few clean pairs uses the 0.05 fallback (counter = 1).
   - Session-6 costs land in the session-7 return. Participation max is 1%.
7. `MonthlyTurnoverDefinitionDeploymentReconciles` (:533).
   - The Jan-31 decision's fill on Feb-3 is booked to February (bit-equal).
   - January's planned turnover includes Jan 31, and the months sum to the total.
   - Execution sessions equal T−2, the deployment month is flagged, and the ex-deployment mean and the `meets_*` flags are consistent.
8. `PinnedRunWithVolumePublishesLastAndRefusesTampering` (:579).
   - Pinned artifact plus role with volume. Checks summary schema, status, primary, recipe SHA, per-scenario CSV SHAs, observations = T−2, the required turnover keys, S2 gap-run-1 = 1 and S3 written-off = 1.
   - Output is exclusive, and the unchanged target replay still runs on the same artifact.
   - A tampered `volume.f64`, a `volume_basis` mismatch and a budget of 1 (OutOfRange) are each refused before any output exists.
   - The CLI rejects `--one-way-bps` and a missing `--role`.
9. `TerminalStressConstantsMatchBookReplay` (:645). Parity with `book::assumed_missing_price_return(Unknown, side)` and the Shumway constants; the fixed scenario numbers.
10. `PinnedCandidateWeightsBlendIsAdmittedAndBound` (:676, root addendum).
    - A pinned manifest runs through both the NAV and target replays, and the weights SHA appears in `source_bindings` (and at the NAV summary top level).
    - Refused before output: pinned semantics without the key, equal semantics with the key, a non-hex SHA, a null SHA, and an unknown semantics string.

The 7 existing `StrategyTargetReplay` fixtures and `strategy_target_replay_test.cpp` are untouched.

## Deviations and decisions to confirm
1. **Daily CSV rows cover [d0, T) (TRAIN 756), not [d0+1, T).** This keeps the first decision's plan visible for reconciliation. `return_observation` marks the 754 return rows [d0+2, T), and the stats use only those.
2. The CSV column names are ASCII (`*_dollars`, `gross_leverage`, `net_leverage`). The per-day `cash_ratio` column feeds `min_cash_ratio` in the summary. I added `planned_gross`/`planned_net` for fixture 1, and `session_index`, `executed`, `unrationed_*` and `fills`.
3. **Participation p95** is the upper edge of a 0.01-decade log histogram (about 2.3% resolution, capped at the exact max). This keeps memory at O(1) instead of storing every fill; the max is exact.
4. S1 (flat) is uncapped (recorded as `"uncapped"`) but its participation diagnostics are still computed against the same causal ADV. `fallback_vol_fills` counts fills in sqrt scenarios only.
5. Sigma pairs are (k-1, k) for k in [t-w, t), mirroring `strategy_runner.cpp:228-239` (63 pairs max). The design text reads as "adjacent pairs within the window" (62 pairs). ADV for t ≤ w (short history) is divided by w and never rescaled; this is irrelevant on real data with a 399-session warmup.
6. `meets_turnover_target_all_months` includes the deployment month (so it will be false in practice). I added `_ex_deployment_month` variants for both the mean and all-months flags. `meets_sharpe_target` is net Sharpe ≥ 1.0.
7. Unresolved-at-end events carry pnl = NaN (written as `nan` in the CSV). Their bucket pnl stays 0 because the stale mark is unrealized.
8. Two **hard refusals** protect accounting: the return identity and the cash + Σh = NAV reconciliation, both at 1e-9 relative. If the real-data run ever hits `Internal: nav replay: ...`, that is a genuine accounting bug to report, not a tolerance to relax. The event cap (262,144 per scenario) refuses with OutOfRange.
9. NAV path planned turnover (and so v2's spent budget) is computed on drifted marked weights, as designed, so it differs from the target replay's no-drift plan. Both appear in the summary (`planned_vs_actual`).
10. The guard only flags; guarded intervals (including gap intervals) are realized at the adjusted return, with sensitivity reported.

## Concerns / open questions
- Not compiled. The code most likely to draw a compiler error:
  - the nlohmann nested initializer lists in `turnover_json`/`risk_json`, and the `json == string-literal` comparisons in `admitted_signal_semantics`;
  - aggregate init of `Ctx` (reference members) and of `EventValues` (brace lists with u32 → usize);
  - `std::make_unique<const bk::FlatBpsCost>(std::move(...))`.
- The exact-fill rule (`held = order` on a complete fill) is what makes fixture 1 bit-exact. The cash book absorbs a sub-ulp difference, which is covered by the 1e-9 reconciliation.
- The runtime of the validation loop (all cells, per scenario, including cells beyond decision_end which never affect results) is O(D·N) × 3. This is negligible next to the SHA-verified load.
