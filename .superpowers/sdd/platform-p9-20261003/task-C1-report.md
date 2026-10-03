# Lane C1 report

## Outcome
DONE_WITH_CONCERNS: the lane covers all six INFRA-C deliverables plus the leverage half of K-P9-7. About 2,000 lines of C++ were written but never compiled (lane rule). The tests were written but not run. The NAV re-pin substitution list below needs root's ruling before the X-5 identity run.

## Branch / SHA
feat/p9-c1-20261003 @ ef75dfa0. All work is committed and the leased tree is clean (`git status --short` is empty).
- Task A (sqrt, DEC-11): dd925b7f.
- Tasks B+C (per-book leverage rule, capacity lockstep, summary binding, adv-hold per multiple, truncation test, `--list-rules --json` leverage rows): ef75dfa0. B and C are one commit because they edit the same hunks of strategy_nav_{replay,v7}.cpp, and interactive staging is not available.

## Frozen base / lease
- base_sha=d7c1c520c3caa162ef453349b256669ce8de3c80
- worktree=C:\atx-wt\pool-15; lease_name=pool-15
- lease_run_id=p9-c1-20261003; heartbeat_id=p9-c1-hb; keeper_pid=20868 (pools.md)

Root leased the pool before dispatch. This lane did not lease or release it.

## Acquisition receipt
Root performed it; see pools.md row C1. The lane ran no acquire.

## Files changed
- atx-engine/src/book/replay_cost.cpp: `sqrt` where the exponent is .5; cbrt untouched (P10).
- atx-engine/tests/book/book_replay_cost_test.cpp: ReplayCostSqrt.* (3 tests).
- atx-impl/src/strategy_cost_v2.cpp: `sqrt` in the capacity law and the S2 marginal cost.
- atx-impl/src/strategy_risk_target.{hpp,cpp}:
  - `Record` = NavLeverageRecord.
  - BookScaler (one book's engine state) and BookLeverage (scaler + scaled config + two-speed carry).
  - Scaler is now a label → BookLeverage map, used by the v7 seam and the tests.
  - leverage_rule / options_of; leverage_rules_json.
- atx-impl/src/strategy_nav_replay.hpp:
  - NavLeverageLaw / NavLeverageRule, and `NavReplayConfig::leverage`.
  - NavLeverageRecord and NavTransferRecord; NavReplayResult::{leverage, transfer}.
  - `nav_rules_schema`; docs updated.
- atx-impl/src/strategy_nav_replay.cpp:
  - Each book owns its BookLeverage and v7 BookState. Planning goes through the leverage rule, then v7::plan_book.
  - Leverage and capacity books are grouped by (L, m).
  - Records are merged back in (session, book) order.
  - Book cap is 1..16.
  - Book workers and the grid are allowed under the scalers; spo is still refused.
  - The decide path refuses leverage rules.
  - Publish order: recipe → main CSVs → capacity/ → v7_extras.json → summary.json last.
  - summary.json gets `producer {engine_git_sha, build_type, definition}`.
  - `--list-rules --json` returns the `atx.nav-rules/v1` envelope.
- atx-impl/src/strategy_nav_v7.{hpp,cpp}:
  - Per-book BookState replaces the State members (c_history, costs, decision, scaled, lambda, carry).
  - capacity_book / nav_multiple / lockstep_scenarios.
  - Capacity books run in the main lockstep, and the argv re-dispatch is deleted.
  - CapacityPublication; publish_extras binds `capacity/summary.json`.
  - The recipe's adv_hold_rule reads each multiple's NAV under `--capacity-curve`.
  - Help text updated.
- atx-impl/tests/strategy_cost_v2_test.cpp: ReplayCostSqrt.CapacityLawAndS2MarginalCostBitsArePinned; NavCapacityLockstep.{OneLockstepEqualsTheSeparatePassesBookForBook, LockstepBooksAndTheirCap}. The NavV7Hook capacity test's base book is relabelled.
- atx-impl/tests/strategy_live_test.cpp:
  - AdvHoldCapacityPerMultiple.CapReadsEachMultiplesNav.
  - NavSummaryBinding.SummaryIsLastAndBindsTheExtrasAndTheCapacitySummary.
  - NavCapacityLockstep.CliPublishesTheCapacityDirectoryFromOneLockstep.
  - AdvHold.CapacityCurveCarriesTheCapInBothPasses re-pinned. An in-test comment gives the reason: Ruling E-15's initial-NAV cap at every multiple is retired; x4 now caps at 4×NAV.
- atx-impl/tests/strategy_vol_target_test.cpp: NavBookRule.* (5 tests) and VolTarget.TruncationInvariant.

## Evidence
No build and no test run: lane rule, owner directive. Mechanical checks only:

```
$ git -C C:/atx-wt/pool-15 log --oneline d7c1c520..HEAD        exit_code=0
ef75dfa0 feat(atx-impl): per-book leverage rule, capacity books in main lockstep, summary binding (P9 C1)
dd925b7f fix(nav): sqrt for the square-root impact law, Debug/Release probe (P9 C1, DEC-11)

$ git diff --stat d7c1c520..HEAD                                 exit_code=0
 12 files changed, 2037 insertions(+), 367 deletions(-)

$ git diff -U0 d7c1c520..HEAD | awk '/^\+[^+]/ && length($0) > 101 {n++} END {print "added lines over 100 columns: " n+0}'
added lines over 100 columns: 0                                  exit_code=0
```

- Symbol closure checks: grep-checked; there are no exit-code-zero receipts for them.
  - Every anonymous-namespace helper in strategy_nav_v7.cpp has ≥1 use.
  - The removed `v7_extension_installed` has no remaining references.
  - No test reads the removed v7::State members.
  - `build_engine_git_sha` lives in atx-impl-core, the same library as strategy_nav_replay.cpp.
- Summary-hash check: no test pins a summary.json SHA. Grep for 64-hex literals near `summary.json` in atx-impl/tests returned nothing.

## How root verifies

### 1. Build
Build these three targets, each through the wrapper:
```
powershell scripts\atx-build.ps1 build atx-engine-w1-cost-tests
powershell scripts\atx-build.ps1 build atx-impl-strategy-target-tests
powershell scripts\atx-build.ps1 build atx-equity-strategy-targets
```

### 2. New tests (anchored filters)
- `atx-engine-w1-cost-tests --gtest_filter=ReplayCostSqrt.*`
- `atx-impl-strategy-target-tests --gtest_filter=ReplayCostSqrt.*:NavBookRule.*:NavCapacityLockstep.*:NavSummaryBinding.*:VolTarget.TruncationInvariant:AdvHoldCapacityPerMultiple.*`

### 3. Regression (same exe)
Run `RiskTarget.*:VolTarget.*:NavV7Hook.*:CostV2*:AdvHold*:ConstructionGrid.*:NavBookWorkers.*:Spo*:TwoSpeed.*:NavLabelRole.*:InvVol.*:NavReplay*:Nav*`. That covers every suite in strategy_{cost_v2,live,vol_target,risk_target,spo,spo_v3,two_speed,nav_replay}_test.cpp.

These four suites test behaviour the commit changed on purpose:
- NavV7Hook capacity tests: capacity books now sit in the main lockstep.
- AdvHold capacity: re-pinned.
- Grid refusals: the grid is now allowed under the scalers.
- Book-workers refusals.

If any of them fails on its message substring, read the test before treating it as a regression. The new refusal texts are:
- "nav replay: --book-workers above 1 needs a fixed rate and no spo rule (its engines hold every book's state)"
- "nav grid: not with an spo rule ..."
- "nav replay: 1..16 scenarios per replay"
- "nav replay: a leverage rule (risk-target-v1, vol-target-v1) needs aim-partial-v5, ..."
- "nav decide: a leverage rule ..."

### 4. X-5 identity run (Release, at the merged SHA)
Use the argv of `build-equity/v8-i16d-x5-nav-run/receipt.json` with only `--output` changed:
```
atx-equity-strategy-targets.exe nav --combined build-equity/mega-v8xw-train-theme-erc-1/train_combined.json
  --combined-sha256 2a442f560982749b74283f16ef3060656f270d6130628c64fcd4bde77259cb86
  --role build-equity/train-2020-2023-lo3/manifest.json
  --role-sha256 e1c6710104594b4777616714195e5ecc78f22fed7820577692b6423612d395f4
  --fields build-equity/train-2020-2023-lo3-fields-v13/manifest.json
  --fields-sha256 e5f7f28c465a92885d55d55a439eb2f217b9f5f15d51017b4293ebc55935e9b2
  --output <new dir> --rule aim-partial-v5 --cadence 1 --trade-fraction .05 --dust-multiple .1
  --aim-leverage 1.1720 --daily-turnover-mean-max .20 --daily-turnover-p95-max .30
  --neutralize price-risk-v1 --max-bytes 1073741824 --order-basis delta --exit-rate .05
  --locate-in-aim --liquidity-cache --warm-start-sessions 60 --capacity-curve
  --label-role build-equity/train-2020-2023-lo3-dlret/manifest.json
  --label-role-sha256 95e16cfe3ad0e0c9e7cf94acb6df4004e6f286ae59dff7c0e725ea33aace5069
```

### NAV re-pin substitution list (root must rule before the identity run; sqrt moves bytes)

| File | Expected vs old X-5 output | Reason |
|---|---|---|
| `recipe.json`, `capacity/recipe.json` | Byte-identical | X-5 has no adv-hold and no spo. The adv_hold_rule override only applies when the key exists. |
| S1-book daily/events CSVs (no square-root impact) | Byte-identical | |
| S2-family daily/events (4 main books); `capacity/` daily, events, summary.json; `capacity_curve.csv`; `v7_transfer_coefficient.csv` | May move | Only where `sqrt(x)` ≠ CRT `pow(x, .5)` (DEC-11), plus whatever follows from it. Capacity books being in the lockstep is value-preserving per book (NavCapacityLockstep test). |
| `v7_extras.json` | New bytes | Gains `files["capacity/summary.json"]` and its SHA. |
| `summary.json` | New bytes, by design | New v7 extras sentence (it names the binding), new `v7.files` {v7_extras.json, capacity/summary.json}, new `producer`, and the SHAs that follow. Cross-build comparisons must drop `producer`. |

### Release expectation (NV-3 vs DS-1)
sqrt removes the CRT `pow(x, .5)` cause of the 1-ULP Release/Debug difference in the cost columns. Release/Debug differences can still come from these, all untouched by the lane:
- log/log10/pow(10) in participation p95
- the cagr `pow`
- the guard `log`
- `cbrt` (KO)
- the v6 band `pow`

## Deviations from brief
1. **Exe identity goes in summary.json, not recipe.json.** The decide path recomputes the recipe SHA, and tests pin it (strategy_nav_replay_test ~1645/1999/2150/2861). The producer key is `{engine_git_sha, build_type, definition}` and appears in every NAV run's summary.json, not only v7 runs.
2. **The argv SHA-256 is not in the published bytes.** Directory-identity tests compare runs with different argv. The run receipt (E1 / K-P9-10) carries it.
3. **Capacity books are identified by capacity id under `--capacity-curve` (or pass Capacity), not by capacity id alone.** strategy_spo_v3_test's replay_v3 shortcut replays a capacity id without the curve and must stay a main book.
4. **`capacity/summary.json` keeps the old capacity-pass bytes**: pass "capacity", the unbound extras sentence, no producer and no stage_seconds. The main summary binds it by SHA.
5. **Warm-start inert check runs separately for main and capacity books.** With adv-hold and several multiples, it runs once per multiple's group.
6. **Decision liquidity is computed on every rebalance decision whenever v7 books exist,** including the warm-up. Values are unchanged; this costs a small amount of time.
7. **The construction grid is now allowed with the v7 seam (except spo).** Each variant publishes its own v7 files.
8. **adv_hold_rule recipe text is overridden in v7::extend_recipe,** so strategy_target_replay.cpp was not edited.
9. **Tasks B and C share one commit** (see Branch / SHA).

## Cross-lane edits
None. No CMake, scripts/** or atx-db edits; no new test files (P2).

## Open risks
- **First compile happens at root.** clang-cl /W4 /WX may flag something; the likeliest spots are the new `merged` template and the `std::span<const Record>*` parameter in strategy_nav_v7.cpp.
- **Admission under `--max-bytes 1 GiB`.** The 5 capacity books now share the main lockstep's reserve and validate_nav_input budget: about +5 × (1 MiB + 5922×192 B + per-day and event buffers), roughly +110 MB. If X-5 is refused, raising `--max-bytes` changes recipe bytes; root rules.
- **spo still refuses book workers and the grid** (shared engine state). Out of scope for C1.

## Ledger candidates
- NAV P9 C1: a capacity id under `--capacity-curve` makes a capacity book that runs in the main lockstep (max 16 books). The argv re-dispatch is gone. A capacity id without the curve (spo-v3 replay shortcut) stays a main book.
- NAV summary.json carries `producer {engine_git_sha, build_type, definition}`, and cross-build byte comparisons must drop it. recipe.json carries no exe identity, because the decide path recomputes the recipe SHA.
- DEC-11: the square-root impact law uses `sqrt` (IEEE-exact). CRT `pow(x, .5)` differed by 1 ULP between Release and Debug. `cbrt` and the other exponents keep `pow`.
