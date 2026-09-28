# Task T30 report: `aim-partial-v5` construction rule (C++)

Status: **DONE_WITH_CONCERNS**. The concern is that nothing was compiled or run, as the lane contract requires. Details are under "Concerns" below.

- Worktree: `C:/atx-wt/pool-3`
- Branch: `feat/mega-alpha-v5-construction-20260927`
- Base: d4ec515d

## Commits

- `34d021dd` feat(nav): aim-partial-v5 construction rule with dust band and aim leverage (T30). This holds the code and fixtures.
- A second commit adds this report and the SHA derivation scripts under `t30-sha/`, force-added. Its subject is the same, with "report" appended.

## Rule, as implemented

`TargetReplayRule::AimPartialV5 = 3`. The config gains `aim_leverage{1.0}` and `dust_multiple{}`, placed after `band_multiple`. On each decision d, with N_d members:

- **Rebalance decision, member i:**
  - aim = L·desired_i and gap = aim − current_i.
  - If |gap| ≤ dust_multiple/N_d, the member is dusted. It keeps its weight and `++construction.banded_names`.
  - Otherwise next = current_i + θ_i·gap, where θ_i = `per_name_rate[i]` if the span is non-empty, else `trade_fraction`.
- **Non-rebalance decision** (cadence > 1, or a skipped neutralize): members keep current exactly. This is equivalent to θ = 0 (R-d), written as `next = current` so no `0*gap` arithmetic is involved.
- **Nonmembers:** always 0.
- **Accumulators:** turnover, forced/discretionary, gross, net, long/short, max_abs, held_names and effective_names are the same fields in the same order as baseline.
- **`applied_fraction`:** set to `trade_fraction` on rebalance decisions, else 0.
- **`monthly_budget`:** ignored.

Code layout in `atx-impl/src/strategy_target_replay.cpp`:

- A file-local `aim_partial_weights(...)` is called from the top of `update_weights` when `rule == AimPartialV5`.
- `update_weights` gains a trailing `std::span<const f64> per_name_rate = {}`. The same parameter is added to the detail declaration, which carries the default, and to the forwarding wrapper, which has no default.
- `members_at(in, d)` is factored out. The band path uses it too; it is the same integer count, and the detail seam exports it for the NAV.

## Controller rulings: how each was applied

- **R-a, amended by the controller mid-task:** there is no θ == 1 special case.
  - Baseline-v1 at fraction 1 computes `current + 1.0*(desired − current)`, which is not always bit-equal to `desired`. Writing `next = aim` would therefore make v5 differ from baseline in the last ulp.
  - The uniform `current + θ·(aim − current)`, with aim = 1.0·desired (exact), runs exactly baseline's IEEE operation sequence.
  - It also matches under FMA contraction, because 1.0·gap is exact.
  - The ThetaOne fixtures therefore assert `bit_cast` equality on every day field and on the weights themselves.
  - This is recorded in a code comment and in the commit message.
- **R-b:** `DustDoesNotBlockEntry` has three checks:
  - Gross lies in [0.9·θG, θG]. θG is taken from a dust-0 replay of the same input, and is also asserted to be about 0.05.
  - held ≥ 90% of members, and held + dusted = members.
  - A name-by-name check through the detail seam: every member with |aim| > dust/N_d holds exactly `0.05*desired` (bit-equal), and every other member stays at 0.
  - The upper bound θG is exact, not approximate: both sums run in the same order over non-negative terms, and IEEE rounding is monotone.
- **R-c:** baseline-v1 and monthly-budget-v2 arithmetic, recipe keys, summary keys and CSVs are unchanged.
  - `construction_on()` is now also true when `rule == AimPartialV5`, so v5 alone gets the construction CSV columns, with `banded_names` as the dust count and no new column, plus the construction summary and the v5 recipe keys.
  - The `rule_name` ternary became an exhaustive `switch` with the same spellings.
- **R-d:** implemented as described above.
- **R-e:** validation uses the file's `co::Err(co::ErrorCode::InvalidArgument, "target replay: …")` idiom.
  - Under v5: band_multiple must be 0, dust in [0, 0.5], L in [1, 2]. The negated ranges also refuse NaN.
  - θ in (0, 1] is the pre-existing trade_fraction check.
  - Under other rules: L must equal 1.0 and dust must be 0.
  - AimPartialV5 was added to the admitted rule set.
- **R-f:** CLI flags `--rule aim-partial-v5`, `--dust-multiple`, `--aim-leverage`; θ is `--trade-fraction`.
  - They were added to **both** the NAV and the target-replay dispatchers, and to both `--help` strings.
  - The NAV summary gets a `construction.v5` object per scenario, v5 only.
  - The T36 seam is `const char* aim_rate(const TargetReplayConfig&)`, which returns `"fixed"`, plus the `per_name_rate` span.

## Recipe and summary keys (v5 only)

**Recipe:** `rule = "aim-partial-v5"`, plus `theta` (= trade_fraction), `dust_multiple`, `aim_leverage`, `rate = "fixed"`. There is one extra descriptive string key, `aim_partial`, following the pattern of the band's `band` key.

**Summary** (target replay top level; NAV per scenario): `construction.v5 = {theta, dust_multiple, aim_leverage, rate, decisions, mean_gross, mean_net, mean_held_share}`.

- `decisions` is extra.
- Means are taken over decision rows; a mean over no decisions is null.
- `mean_held_share` averages held_names/N_d over decisions that have members.
- In the NAV, gross, net and held come from the rule's plan before the locate rule (`planned_gross`, `planned_net`, and the new `planned_held_names`) at decision-NAV weights. The realised book is still in `exposure.mean_gross_leverage`.

**NAV struct change:** `NavReplayDay` gains `usize planned_held_names, decision_members`, set in `plan_decision` for every rule. They are not CSV columns.

- Side effect: sizeof(NavReplayDay) grows by 16 bytes. That raises the NAV admission charge by at most 8 books × 4096 × 16 B = 512 KiB of `nav_reserve_bytes`.
- No output changes. With the `--max-bytes 1073741824` used for v4 there is ample slack.

## Other decisions (small ambiguities; decided and recorded here)

1. **Rule id.** It stays exactly `aim-partial-v5`, with no `+dust`/`+L` suffix, as the brief specifies. The `+neutral-price-risk-v1` suffix still applies if neutralize is combined with v5; that combination is not forbidden.
2. **Per-name rate contract.** The span is either empty or holds exactly `in.instruments` finite rates in [0, 1], checked by the caller (T36).
   - The implementation asserts the size in debug builds. In release, a wrong-size span falls back to the fixed θ and is never indexed out of bounds.
   - Non-v5 rules assert the span is empty.
   - The NAV `plan_decision` and `replay_targets` call sites pass nothing.
3. **Where `construction.v5` is written.** It is emitted in the target-replay summary too, not only the NAV one. That summary computes N_d from the loaded blend.
4. **Dust count scope.** Dust counts only on rebalance decisions. A member with gap exactly 0, such as a middle-rank desired of 0 from flat, counts as dusted, the same way the band counts it.

## `NavV5.RecipeAndSummaryKeys` pre-change bytes: how they were derived

I pinned hand-derived SHA-256s rather than only snapshotting structure, and did both.

- A Python emulation of `nlohmann::json::dump(2)` was validated **byte-exact on 25 committed C++ replay outputs**, including target-replay recipes. The 8 mismatches are hand-written, unsorted recipes plus one known case where Grisu2's output is not shortest; that case does not apply to the fixture values.
- The emulation rebuilds `publication_panel()` + `write_artifact()` payloads and manifests.
- It rebuilds the baseline `nav_recipe` from the source's own string constants, which are regex-extracted from `strategy_nav_replay.cpp`.
- Scripts: `.superpowers/sdd/mega-alpha-20260926/t30-sha/`. Run `fixture_sha.py <dir> <root>`, and `validate_nl.py`.

Pinned values:

| What | SHA-256 |
|---|---|
| role manifest | `17349e657c9ee076c252f46a0688463b416bf55bf2792ae4d2a00f4608e2acdc` |
| combined manifest | `295599523de7b52f5caf46764bb390b53863efe49a72126a52a1f53180b91d91` |
| baseline NAV `recipe.json` | `73cb45f1182f659b1b66bf5adc17bc0539a95c987d191c10c00f846c6c8017c0` |
| baseline target `recipe.json` (same artifact) | `ef16be1716d3d1fed90ad8af9aca3073b1c425e2fb163078ce0d13671f584bdf` |

The test also asserts:

- The baseline recipe key set equals the pre-change 32-key list exactly.
- No v5 key appears under baseline.
- The v5 recipe is exactly those 32 keys plus the 5 v5 keys.

**If a pinned SHA fails at the root:**

- The pins are ordered so the root can tell where a failure comes from: the role and combined pins come first (fixture writers), then the recipe pin.
- A failure means my emulation is wrong, not necessarily the code. Confirm with the pre-change binary at d4ec515d on the same fixture, then replace the constant.
- The structural assertions stand on their own either way.
- The root's T37 real-data byte-stability re-run of the frozen v4.1 cell remains the authoritative check.

## Fixtures (postimplementation; not built, not run)

`atx-impl/tests/strategy_target_replay_test.cpp`. The `Fixture` gains `members_at(t)`. Test-local helpers: `random_fixture`, `constant_signal`, `aim_partial`.

- `TargetReplayV5.AimPartialV5_ThetaOne_MatchesBaseline`: bit equality of every day field and the totals at cadence 1 and 3, with random nonmember cells so forced exits occur. Also the per-decision weight vectors, compared through `detail::update_weights`.
- `TargetReplayV5.AimPartialV5_DustDoesNotBlockEntry`: R-b as described above.
- `TargetReplayV5.AimPartialV5_ConvergesToAimGross`:
  - Gross follows 0.25, then 1−0.75², then reaches 1 (1e-6) after 80 steps.
  - With L = 1.5 it goes 0.375 → 1.5.
  - With dust 0.1 it stops in [0.9, 1], all names end dusted, and turnover is 0.
- `TargetReplayV5.RefusesBandUnderV5`, plus a control that the same config with band 0 runs.
- `TargetReplayV5.RefusesOutOfRangeAndForeignParameters`: L, dust and θ bounds (including NaN), and the v5 parameters under baseline or v2.
- `TargetReplayV5.PinnedRunRecordsRecipeSummaryAndCli`: recipe keys, CSV header, `construction.v5`, CLI exit codes 0, 1 and 2, and that no output directory is created on refusal.

`atx-impl/tests/strategy_nav_replay_test.cpp`:

- `NavV5.ThetaOneMatchesBaselineBooks`: the three fixed scenario books in lockstep, including capped working orders and write-offs, are bit-equal to baseline at cadence 1 and 3.
- `NavV5.DeploysTowardAimLeverage`:
  - planned_gross_j = 1.5(1−0.5^(j+1)).
  - The final gross leverage is 1.5, versus 1.0 for baseline at θ .5.
- `NavV5.RecipeAndSummaryKeys`: as described in the section above.
- `NavV5.CliFlagsAndRefusals`.

## Desk-check list

- **Includes:**
  - `<cassert>` was added to `strategy_target_replay.cpp`. That TU compiles without PCH in Debug (SKIP_PRECOMPILE_HEADERS), so its includes must be complete.
  - `std::span` comes via `strategy_target_replay.hpp`.
  - The detail header uses only `atx::f64`/`usize` and `std::span`, which it already includes.
  - The tests use only headers they already include (`<array>`, `<algorithm>`, `<cmath>`, sha256, nlohmann).
- **Call sites of `update_weights`** (grep over the whole worktree):
  - (1) `replay_targets`, which uses the default span.
  - (2) the detail forwarding wrapper, which forwards the span.
  - (3) NAV `plan_decision` (`strategy_nav_replay.cpp`), which uses the default span.
  - (4) the new tests.
  - There are no other callers.
- **Name lookup:** the unnamed-namespace functions `members_at`/`update_weights` and the `detail::` wrappers follow the file's existing qualified-forwarding pattern. There is no ambiguity, because ADL ignores both the unnamed namespace and `detail`.
- **Exhaustiveness:** the only `switch` on `TargetReplayRule` is `rule_name`, now exhaustive. No other code in the tree switches on or positionally initializes `TargetReplayConfig`, `ConstructionDay` or `NavReplayDay`.
- **Warnings:**
  - No unused parameters: `aim_rate`'s parameter is unnamed, and `per_name_rate` is used on both paths.
  - No shadowing.
  - Float equality (`!= 1.0`, `!= 0`) follows existing file usage, and `-Wfloat-equal` is not enabled.
  - All added lines are ≤ 100 columns.
- **Constness:** new helpers take `const&` or spans of const, and locals are `const` wherever they are not reassigned.

## Root: build targets, test filter, CMake

- **Build:** `atx-impl-strategy-target-tests`, `atx-equity-strategy-targets`.
- **Filter:** `TargetReplayV5.*:NavV5*`.
- **Regression:** also run `StrategyTargetReplay.*:StrategyNavReplay.*`. They are in the same binary, and they are the pre-existing byte-stability and bit-parity suites.
- **CMake:** no change. There are no new translation units; both test files are already in `atx-impl-strategy-target-tests`.

## Concerns

1. Nothing was compiled or run. Compile errors, if any, will first show up in the root build.
2. The four pinned SHA-256s in `NavV5.RecipeAndSummaryKeys` are hand-derived (see above). A mismatch would indicate an emulation error, not a regression, until confirmed with the d4ec515d binary.
3. `construction.v5` means are over decision rows of the rule's plan, which includes the deployment ramp. The pre-registered acceptance check "mean gross ∈ [0.90, 1.05]" should state which gross it means: planned `construction.v5.mean_gross`, or the realised `exposure.mean_gross_leverage`.

## For T36

- Fill `per_name_rate`: `in.instruments` finite rates in [0, 1], one per name; the caller validates.
- Pass the span at the NAV `plan_decision` call and, if wanted, in `replay_targets`.
- Extend `aim_rate()` from `"fixed"` to `"per-name-v1"`, and add the config option plus its validation next to the v5 block in `validate_config`.
- `applied_fraction` currently reports the fixed θ. Decide what it should report under per-name rates.
