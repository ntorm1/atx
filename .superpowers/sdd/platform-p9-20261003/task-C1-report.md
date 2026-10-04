# Lane C1 report

## Outcome
DONE_WITH_CONCERNS: the lane covers all six INFRA-C deliverables plus the leverage half of K-P9-7. About 2,000 lines of C++ were written but never compiled (lane rule). The tests were written but not run. The two-stage X-5 identity procedure and its re-pin list (§4) need root's ruling before the identity runs. Fix round 1 (review BLOCK: M1, M2, and C1-SPO) is closed below.

## Branch / SHA
feat/p9-c1-20261003 @ the `docs(sdd): ... C1 fix round 1` commit, with code head a275088b. All work is committed and the leased tree is clean (`git status --short` is empty).
- Task A (sqrt, DEC-11): dd925b7f.
- Tasks B+C (per-book leverage rule, capacity lockstep, summary binding, adv-hold per multiple, truncation test, `--list-rules --json` leverage rows): ef75dfa0. B and C are one commit because they edit the same hunks of strategy_nav_{replay,v7}.cpp, and interactive staging is not available.
- Fix round 1 code and gtest (C1-SPO): a275088b. Report: the commit that follows it.

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
- atx-impl/tests/strategy_spo_v3_test.cpp (fix round 1): SpoV3.CapacityDeclarationWithoutAdvHoldKeepsTheBaseBytes.

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
- Fix round 1, mechanical checks (exit_code=0; outputs below):
  - `cmp` of base d7c1c520 strategy_nav_v7.cpp lines 196-206 against head lines 204-214: `BASE_TEXT_IDENTICAL`.
  - The literal pinned in `SpoV3.CapacityDeclarationWithoutAdvHoldKeepsTheBaseBytes`, compared to the same base lines: `TEST_LITERAL_MATCHES_BASE`.
  - Added lines over 100 columns in the fix diff: `over100: 0`.
  - Receipt fields read (receipt only, no output): source_sha d7c1c520c3caa162ef453349b256669ce8de3c80, git "clean in the code pathspec", outcome completed, exit 0.

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
- `atx-impl-strategy-target-tests --gtest_filter=ReplayCostSqrt.*:NavBookRule.*:NavCapacityLockstep.*:NavSummaryBinding.*:VolTarget.TruncationInvariant:AdvHoldCapacityPerMultiple.*:SpoV3.CapacityDeclarationWithoutAdvHoldKeepsTheBaseBytes`

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

### 4. X-5 identity: two Debug stages, then Release against Debug (fix round 1, M1 + M2)
The reference is `build-equity/v8-i16d-x5-nav`. Its receipt (`build-equity/v8-i16d-x5-nav-run/receipt.json`) says it came from source_sha d7c1c520 (this lane's base, tree clean in the code pathspec) on the `build-equity` (equity-dev, Debug) exe.

Every run uses the receipt's argv, changing only `--output`:
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

#### X-5 books (from code: `nav_scenario_matrix(tiered)`, `cost_v2::capacity_scenarios`)
Main books (labels `<trading id>+<financing id>`; files `daily_<label>.csv` and `events_<label>.csv`):

| Index | Label | Book family | Cost law |
|---|---|---|---|
| [0] | `linear-6bps-stale5-v1+swap-fin-v1` | S1 | FlatBpsV1, no square-root term |
| [1] | `modeled-1bn-stale5-v1+swap-fin-v1` | S2 (primary) | SqrtImpactV1, delta .5 |
| [2] | `modeled-1bn-terminal-adverse-v1+swap-fin-v1` | S2 family | SqrtImpactV1, delta .5 |
| [3] | `modeled-1bn-stale5-v1+flat-300-v0` | S2 family | SqrtImpactV1, delta .5 |
| [4] | `modeled-1bn-stale5-v1+engine-tiers-v1` | S2 family | SqrtImpactV1, delta .5 |

Capacity books in `capacity/` are the S2 law at m x NAV: `capacity-x0p5-v1`, `capacity-x1-v1`, `capacity-x2-v1`, `capacity-x4-v1` and `capacity-x8-v1`, each `+swap-fin-v1`. X-5 has no `--cost-v2`, so no KO/FIM books and no `cbrt`; no v6, spo or adv-hold.

#### Run (a): the new Debug build (equity-dev) against the old X-5 output, in two stages
Each stage builds the commit in its own pool on `-Preset equity-dev` and compares only by file SHA-256 and JSON path (DEC-20).

**Stage 1: dd925b7f (sqrt only, old flow) against the old X-5 output.**

Only these files may differ:
- Main directory: `daily_` and `events_` CSVs of the four S2-family labels [1]-[4].
- `capacity/`: the `daily_` and `events_` CSVs of all five capacity labels.
- `capacity/summary.json`, `capacity_curve.csv`, `v7_transfer_coefficient.csv`.
- `summary.json` and `v7_extras.json`, but only at the JSON paths listed below.

These must be byte-identical:
- The `daily_`/`events_` CSVs of `linear-6bps-stale5-v1+swap-fin-v1`.
- `recipe.json`.
- `capacity/recipe.json`. The capacity laws' impact_y = 0.6 x m^.5 is unchanged: review C1 recomputed pow(m, .5) == sqrt(m) for m in {.5, 1, 2, 4, 8}.

JSON paths sqrt may move:
- `summary.json`:
  - `scenarios[1]`, `scenarios[2]`, `scenarios[3]`, `scenarios[4]` (whole entries, including their CSV SHAs and `construction.v5`).
  - `warm_start.score_begin_gross_leverage["<label [1]-[4]>"]`.
  - `v7.books["<label [1]-[4]>"]`.
  - Nothing else: `scenarios[0]`, `recipe_sha256`, `locate_in_aim` and the rest are identical.
- `capacity/summary.json`:
  - `scenarios[0..4]` (every capacity book).
  - `warm_start.score_begin_gross_leverage[*]`.
  - `v7.books["<label [1]-[4]>"]`. The capacity summary carries the main books' records; label [0] is identical.
- `v7_extras.json`:
  - `files["v7_transfer_coefficient.csv"]`, `files["capacity_curve.csv"]`.
  - `capacity[0..4]`.
  - `capacity_x1_equals_primary_bit_for_bit` stays `true`.

Stage 1 may legitimately show zero differences: the Debug CRT's pow(x, .5) may already round like sqrt.

**Stage 2: a275088b (lane code head; fix 1 touches only spo-v3 capacity sentences, inert in X-5) against the stage-1 output.**

Byte-identical:
- Every `daily_*.csv` and `events_*.csv`, in the main directory and in `capacity/`.
- `recipe.json`, `capacity/recipe.json`, `capacity/summary.json`.
- `capacity_curve.csv`, `v7_transfer_coefficient.csv`.

Allowed differences:
- `summary.json` differs only at `v7.extras` (the bound sentence), `v7.files` (new: SHAs of `v7_extras.json` and `capacity/summary.json`) and `producer` (new; PM ruling C1-PROD).
- `v7_extras.json` differs only at `files["capacity/summary.json"]` (new key).

No file is non-identical by design beyond these three paths. `capacity/summary.json` keeps the old capacity pass's bytes: pass "capacity", the unbound sentence, no producer, and the main books' records, as the old accumulating seam had. Any other difference is a defect in the structural change (per-book BookState/BookLeverage, the capacity lockstep, book_groups, the record merge), not sqrt.

#### Run (b): Release (equity-rel) at the lane head against the stage-2 Debug output
Build the same commit on `-Preset equity-rel` and run the same argv.

Expected result after dropping `summary.producer` (build_type differs by design): every file byte-identical, including `capacity/summary.json`, which has no producer.

A difference beyond that is build noise from the CRT calls the sqrt fix did not touch, not a C1 defect (stage 2 already compared Debug against Debug):
- `guarded_move`'s `std::log`, which runs in every book's MARK and can flip a guard decision, so even the S1 CSVs can move.
- log/log10/pow(10) in participation p95.
- The cagr `pow`.

If Release still differs, plan §2.4 C1 applies: NAV stays Debug and G-P3's NAV half is reported unmet. Root records the first differing file and column.

**What the probe covers.** The Debug/Release probe (`ReplayCostSqrt.*`) covers only the sqrt path: cost_fraction, the capped and unrationed fill costs, the capacity law and marginal_cost_s2, on fixed inputs. Root runs it on both presets of atx-engine-w1-cost-tests and atx-impl-strategy-target-tests. It says nothing about `guarded_move`'s `std::log` or any other CRT call.

### NAV re-pin list for root to rule on (DEC-20; summary of run (a))

| File | Stage 1: dd925b7f vs old X-5 | Stage 2: head vs stage 1 |
|---|---|---|
| `recipe.json`, `capacity/recipe.json` | identical | identical |
| `daily_`/`events_` of `linear-6bps-stale5-v1+swap-fin-v1` | identical | identical |
| `daily_`/`events_` of labels [1]-[4] | may differ (sqrt) | identical |
| `capacity/daily_*`, `capacity/events_*` | may differ (sqrt) | identical |
| `capacity/summary.json` | may differ at the paths above | identical |
| `capacity_curve.csv`, `v7_transfer_coefficient.csv` | may differ (sqrt) | identical |
| `v7_extras.json` | may differ at the paths above | only `files["capacity/summary.json"]` |
| `summary.json` | may differ at the paths above | only `v7.extras`, `v7.files`, `producer` (C1-PROD) |

Cross-build comparisons drop `summary.producer`.

## Fix round 1
Review: `task-C1-review.md` @ f6cd387d, BLOCK (2 major). FIX_BASE f6cd387d.

The code passed review. Only these three findings are closed; the other minors stay deferred as the coordinator directed:
- validate_nav_config: leverage store and params.
- book_groups wall time.
- aim_leverage `required`.

| Finding | Closed by |
|---|---|
| **M1** (report:111): the identity run was stated as "Release, at the merged SHA", which contradicts brief-C1 and plan §2.4 C1 | §4 now has two runs. **(a)** The new Debug (`-Preset equity-dev`) build against the old X-5 output (itself an equity-dev d7c1c520 output per its receipt), under the re-pin list. **(b)** Release (`-Preset equity-rel`) against that Debug output: expected byte-identical after dropping `summary.producer`; any other difference is CRT build noise and triggers the plan's rule (NAV stays Debug, G-P3 NAV half unmet). §4 states that the Debug/Release probe (`ReplayCostSqrt.*`) covers only the sqrt path; `guarded_move`'s `std::log` (every MARK), log/log10/pow(10) in participation p95 and the cagr `pow` remain. Report only. |
| **M2** (report:134): the substitution list excused whole files "plus whatever follows", so it could not catch a structural defect on 9 of 10 books | Replaced by a two-stage identity. **Stage 1:** dd925b7f (sqrt only) against the old X-5, with the files sqrt may move named exactly: the `daily_`/`events_` CSVs of labels [1]-[4] and of the five `capacity-x*-v1+swap-fin-v1` books, `capacity_curve.csv`, `v7_transfer_coefficient.csv`, and `capacity/summary.json`, `summary.json` and `v7_extras.json` only at listed JSON paths (`scenarios[1..4]`, `warm_start.score_begin_gross_leverage[<S2 labels>]`, `v7.books[<S2 labels>]`, `capacity[0..4]`, the two `files` SHAs). **Stage 2:** head a275088b against the dd925b7f output: every CSV (main and `capacity/`), both recipes, `capacity/summary.json`, `capacity_curve.csv` and `v7_transfer_coefficient.csv` byte-identical; `summary.json` only at `v7.extras`, `v7.files` and `producer`; `v7_extras.json` only at `files["capacity/summary.json"]`. Nothing is non-identical by design beyond those three paths. Report only. |
| **C1-SPO** (PM ruling; strategy_nav_v7.cpp:208): `capacity_spo_v3_declaration` was reworded unconditionally, so flag-absent spo-v3 capacity recipes moved | a275088b restores the base sentence byte for byte and adds `capacity_spo_v3_adv_hold_declaration`, the per-multiple-NAV sentence, used only with `--adv-hold-q`. Which signal picks it: the recipe uses its own `adv_hold_rule` key, so the decide path, which runs no replay, agrees. The summary, holdings manifest and `v7_extras.json` use `State::adv_hold`, set by `v7::configure` from the replay's configs. New gtest `SpoV3.CapacityDeclarationWithoutAdvHoldKeepsTheBaseBytes` in strategy_spo_v3_test.cpp (written, not run) pins the base literal in the recipe and the holdings manifest without adv-hold, the adv-hold sentence with it, and the return to base. Filter: `atx-impl-strategy-target-tests --gtest_filter=SpoV3.CapacityDeclarationWithoutAdvHoldKeepsTheBaseBytes`. |
| **C1-PROD** (PM ruling, for information) | Accepted: exe identity in summary.json `producer` (plan §1.2 NV row). It stays in the re-pin list: stage 2 `producer`, and run (b) drops it. |

Files touched in fix round 1:
- atx-impl/src/strategy_nav_v7.{hpp,cpp}
- atx-impl/tests/strategy_spo_v3_test.cpp
- this report

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
