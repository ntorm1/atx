# Task V6-C2 review: industry neutralisation ids, grp_ff12 plumbing, reserve at run geometry

**Reviewer:** task reviewer (read-only; Opus 5.5). **Reviewed:** `720a0066` on `feat/mega-alpha-v6-c2-20260927`
(C:/atx-wt/pool-11, clean), parent `04e9d5bc`. `04e9d5bc` is the brief's `e0dfb8c7` plus the Phase-C
brief docs only, so there is no code delta. **Inputs:** task-V6C2-brief.md, task-V6C2-report.md, review-V6C2.diff,
v6-code-review-exec.md (F6, F7, F9, C3, C4), v4-prereg.md "## v6 revision" C5, `.agents/cpp/agent.md`.
**Method:** I read the diff in full, then the surrounding code at 720a0066 (Cholesky/OLS, windows, NAV run_books,
used_field, load_saved_blend/load_prices), the grp_ff12 producer (`atx-engine/tools/prepare_research_fields.py`), and
a no-worktree `git merge-tree` of 720a0066 against the V6-C1 tip (`5e1c7f6d`). Nothing was built or run.

## Verdict summary

- **Spec compliance: PASS**, with one ruling request (fallback, see S2) and one verification gap the root must close
  (golden (a), see S6).
- **Code quality: PASS.** I found no Critical defect. The FWL composition is exact, v1 bytes are preserved by
  construction, ids are PIT, and NaN/id handling is UB-free.
- **Overall: APPROVED**, conditional on the root gates in section 5. The Important items are root verification and
  merge planning, not code defects in this lane; the implementer's only requested change is the optional I1 test split.

---

## 1. Spec compliance

**S1. ind-v1 = price-risk-v1 regressors + within-FF12 demeaning, in the stated order: PASS.**
The order is documented at `strategy_price_exposures.hpp:140-160` and implemented in `neutralize`
(`strategy_price_exposures.cpp:440-470`) and `fit_residual` (`:407-437`):
- used rows and entry gross come first;
- then z-score and clip over the used rows (`standardize`, unchanged);
- then slots (`assign_slots`, `:241-267`);
- then the target and every clipped z column are demeaned within slot (`demean_within_groups`, `:274-302`);
- then the existing equilibrated-Cholesky OLS on `[1, demeaned z]` with one refinement step;
- then the rescale to the entry gross, with `member&&!ok` rows set to 0.

The regressors are exactly v1's clipped z. The clip is taken against the universe distribution before demeaning,
which is what "price-risk-v1 regressors" means. The amplification cap, the excluded-share cap and the skip
semantics go through `form_desired` unchanged.

**S2. Small-group fallback (< 5): the pooled reading is ACCEPTABLE, and it is deterministic.**
Ruling: pooling every under-5 group into one fallback slot is the regression-consistent meaning of "falls back to
the universe mean". Take the regression with an intercept and dummies for the large groups only. Its base category
is exactly the small-group names, and its fitted level is their pooled mean.

Literally subtracting the universe mean from small-group names is an oblique map, not an orthogonal projection:
- the fitted value of a small name would depend on large-group names;
- the reverse dependence is absent, so P is not symmetric;
- after that step the OLS intercept re-shifts everything, so FWL, the within-group zero sums and the "residual on
  [group indicators, z]" recipe text would all be false.

Determinism:
- slot ids depend only on the ids;
- sums run in ascending row order;
- test (c) pins bit-identity under relabelling ({3,2}→one id, NaN→42).

The root should record this ruling in the ledger/prereg notes, because the brief text said "universe mean".
Degenerate case: see M1.

**S3. NaN ids form one residual group: PASS** (`:247-253`). NaN maps to `kUnknownSlot`. That slot is subject to the
same under-5 pooling, and `unknown_group_names` counts NaN rows whether or not they were pooled. Both behaviours are
documented. ±inf, fractional values and values outside [0, 9999] on a used row are contract errors
(InvalidArgument), and the replay then aborts. Unread rows (nonmembers and `!ok`) are never validated, which test
`WithinGroupsRefusesBadIds…` pins.

**S4. ind-v2 windows via the id only: PASS.**
- `parse_neutralize` (`strategy_target_replay.cpp:810`) sets vol 126 / adv 252. There is no new CLI flag.
- `validate_config` (`:105-110`) refuses any other pair under ind-v2, so a programmatic config cannot relabel.
- Beta stays 252. `block = max(252, 126)`, so the returns block and the `log()` count are unchanged.
- The ADV window of 252 equals the block, so `first_session` is unchanged.

**S5. grp_ff12 goes through load_fields; the reserve is charged at actual geometry; refusals are unchanged: PASS.**
Field loading:
- `load_field` (`strategy_nav_replay.cpp:1482-1493`) reuses `used_field`, so single-entry, PIT true, `<f8`,
  date-major, role shape, receipt and payload SHA are all checked.
- grp_ff12 is loaded only for the industry ids, and it is charged as a third f64 field in both the pre-check and
  `remaining`.

Reserve:
- `role_geometry` (`:1539-1551`) reads names and `score_end - score_begin` from the SHA-pinned role manifest.
- `nav_workspace_reserve_bytes` (`:1613-1621`) has the old formula term for term, with `max_names`/`max_dates`
  replaced by the run's names and sessions.
- Days per book are exactly `end - begin` (`run_books` `:836`, `:889`). After the load, the blend geometry is
  asserted equal to the reserve geometry (`:1951`).
- The refusal is unchanged: `max_working_bytes <= reserve` gives OutOfRange before any payload or output.

The only ordering change: an invalid or unpinned role manifest now fails with InvalidArgument before the budget
check. Previously it failed after the check. This matters only for malformed inputs.

I re-derived the reserve numbers and they check out:
- old: 190.6 MB;
- new, v1: 136.3 MB;
- about 94 MB of that is 5 books × the 262,144-event cap, which C4 does not touch.

**S6. price-risk-v1 output bytes are unchanged: the argument is SOUND by inspection, but the golden is not yet
PROOF.**
The inspection argument:
- v1 and grouped share one body;
- with an empty group span every added branch is skipped;
- the only v1-visible reorder is `e[r] = target[rows[r]]` (`:416`), moved above `factor(normal_matrix(z))`, and
  `factor` reads only z; `target` and `s.residual` never alias;
- the double `stats = {}` collapse is a no-op;
- the strings match: `neutralize_name(PriceRiskV1)` gives the old literals, the v1 method text is byte-identical,
  and `Json` objects are key-sorted, so the new optional keys cannot reorder the v1 ones;
- no NAV output publishes the reserve (grep: no `*reserve*` key in atx-impl outputs).

I agree with this argument.

The golden in test (a) comes from a Python replica. The fixture (`section()`, `normalize`) and v1 use only
+ − × /, sqrt and clamp, so an exact binary64 replica is feasible. `build-equity` is the `equity-dev` preset, which
inherits `dev`: no `/arch`, so no FMA contraction. However, the replica was never checked against a base binary. If
the replica was transcribed from the modified code, then a v1 change would be baked into the golden and pass
silently.

To close the gap, the root must run the golden against a real base build (section 5, gate 2). Note the report's
"cherry-pick the test onto 04e9d5bc" does **not** compile as written: lines `strategy_price_exposures_test.cpp:487-488`
reference `stats.groups` and `stats.fallback_names`, which do not exist at base. See I1.

Test (a) also covers only `neutralize_target`, not `form_desired`, the NAV lockstep, or the recipe/summary strings.
An end-to-end A/B is the stronger gate (section 5, gate 3).

**S7. Deliverables 3 and 5 skipped: ACCEPTED.** `-mkt` is not in the C5 grid and would be +1 trial. The F9 ring
buffer is not needed for the time budget. Both designs in the report are sound. For F9, the plan to keep the ADV
sum recomputed per decision, so the summation order is preserved, is correct.

**S8. Constraints: PASS.** No order, fill or exit logic changed. The nav_replay edits are limited to field loading,
the reserve, the neutralisation dispatch, `validate_nav_input`, and the help and parse lines. v5_train.sh,
v51_train.sh and nav_summ.py are untouched. CRLF is consistent in all 10 files, and no added line exceeds 100 columns
(both checked).

## 2. Code quality (house style)

**Demeaning + OLS composition (FWL): CORRECT.**
- After within-slot demeaning, every demeaned z column and the demeaned target sum to 0 within each slot, and so
  overall.
- The raw intercept column 1 lies in span(D). It is orthogonal to M_D z and to M_D t, so its coefficient is 0 up
  to rounding, and the residual on `[1, M_D z]` equals the residual of `M_D t` on `M_D z`. By FWL, that is the
  residual of t on `[D, z]`.
- There is no double-centering hazard: the global centering in `standardize` is subsumed by the slot demeaning.
- The normal matrix is better conditioned than v1's, with an intercept row of about `[n, ε, ε, ε]`, so the
  equilibrated pivots are unaffected.
- `within ≤ total` always holds, so the spanned-column check (`:300`, `within > 1e-8 × Σz²`) is a correct stand-in
  for the pivot floor, which cannot see a column absorbed by D.

Amplification interaction:
- Entry gross divided by residual gross necessarily rises versus v1, because the industry component is also
  removed, and the same cap of 5 applies. That is compliant.
- The only effect is possibly more `skipped-amplification` decisions for industry-concentrated targets.
- The root should watch `neutralize_amplification_max` and the skip counts in the TRAIN summary. The v1 median
  was 1.19; ind-v1 will be higher.

**Numerics and NaN handling: OK.**
- Slot sums are f64, and the count is never 0 for an occupied slot.
- z and targets on used rows are finite (validated).
- NaN ids are routed explicitly.
- `static_cast<usize>(id)` happens only after the range and floor checks, so there is no float-to-integer UB.
  -0.0 maps to slot 0 correctly.

**Integer group ids as doubles: acceptable.** This matches the producer contract
(`prepare_research_fields.py:394`: FF12 is 1..12 stored as f64, NaN otherwise) and the `<f8` SHA pin.

**Determinism: OK.**
- There is no hashing, no sorting by value, and no threads.
- The lockstep books share one construction.
- Test `IndustryNeutralizeRuns…` checks that lockstep equals single-book replays.

**Look-ahead: none.**
- `form_desired` reads `in.industry.subspan(d*n, n)` (`strategy_target_replay.cpp:339`) at decision d, which is
  after the d 22:00 mark.
- The producer builds `grp_*` at row d from the CIK's latest SIC row with a clock before the mark of session d−1
  (L = 1; `prepare_research_fields.py:28-30`).
- `used_field` refuses `point_in_time != true` (`strategy_nav_replay.cpp:1463-1466`).
- Field rows align with blend rows through the role SHA pins.

**RAM:**
- 1,155 × 5,627 × 8 B = 51,993,480 B = **49.58 MiB**. That is correct, and so is "loaded at full role geometry,
  warm-up included". RSS going from about 338 to about 388 MiB against the 1,536 MiB cap is plausible.
- A narrower type (u8 for FF12, or u16 for `kMaxGroupId = 9999`) would save about 43 MiB of steady-state RSS. Peak
  RSS stays the same unless `read_field` stream-hashes and converts chunk by chunk.
- **Not required for TRAIN.** It becomes relevant under exec-review C4 (VAL/holdout panels of about 1,650 dates,
  plus grp_ff49/mkt_ret, plus 8 books). See M4.

**Default path: unchanged.**
- `neutralize = None` returns before any industry code.
- Recipe, summary and CSV strings are unchanged.
- The admission reserve is ≤ the old one, so every previously admitted run is still admitted, and no output
  depends on the reserve.

**Style:**
- New functions are ≤ about 30 lines, and loops are bounded.
- `[[nodiscard]]` is used throughout, and `neutralize_by_industry` is `constexpr noexcept`.
- The switch in `neutralize_name` is exhaustive and has no default.
- `ATX_TRY_VOID` under a brace-less `if` is safe, because the macro is do-while.
- The multi-statement `ATX_TRY` at `:1529` is braced.

**Tests: meaningful.**
- (b) checks within-group sums, gross, z-moments against the un-demeaned clipped z (correct, since residual ⟂ D and
  residual ⟂ M_D z imply residual ⟂ z), a difference from v1, and stateless scratch reuse.
- (c) pins pooling and relabel invariance.
- The refusal test covers bad ids, geometry and a spanned exposure.
- The target test checks that `form_desired` equals the primitive bit for bit.
- (d) checks the exact per-session increment and the budgets on both sides of the old refusal line.
- The NAV end-to-end test checks the recipe, the binding, the diagnostics and the refusals.

Coverage gaps: see M2 and M3.

## 3. Findings

### Critical
None.

### Important

**I1. Golden (a) is unvalidated against a base binary, and the report's cherry-pick instruction does not compile.**
- Location: `strategy_price_exposures_test.cpp:458-489`, plus report §9.2.
- Failure scenario (hypothetical): the replica mirrors the branch's operation order. A v1 byte drift then passes
  (a), and the "bytes unchanged" claim goes into the ledger unproven. Separately, the root cherry-picks (a) onto
  04e9d5bc and gets a compile error at `:487-488`.
- Fix:
  - Implementer (optional, 1 minute): move the two `stats.groups` / `stats.fallback_names` EXPECTs out of (a), for
    example into (b) or the one-call test, so that (a) is base-compilable verbatim.
  - Root: section 5, gates 2 and 3.

**I2. Merge semantic conflict with V6-C1 on the reserve. The report says "no semantic conflict"; that is incorrect.**
- Location: `strategy_nav_replay.cpp:1613-1621`, against C1's `run_nav_replay` call.
- C1 changed the reserve's per-name-rate argument to `liquidity_cached(base)`, which is true under
  `--liquidity-cache` at a fixed rate. C2 deleted `nav_reserve_bytes` and computes `per_name_rate` from
  `base.rate == PerNameV1`.
- Failure scenario: after a naive resolution that takes the C2 side, `--liquidity-cache` runs under-reserve by
  names × 24 B (about 135 KB on TRAIN). The admission accounting silently omits C1's cache.
- Fix, at merge: in `nav_workspace_reserve_bytes`, use `liquidity_cached(base)`. It is defined earlier in the same
  TU, at trial-merge line 246.

**I3. Cross-lane semantic item: locate-in-aim (C1/C3) composed with ind-v1 (only if a grid cell combines them).**
- Location: the merged `form_desired`. Zeroing happens before the dispatch, and the merge is otherwise clean.
- Failure scenario: zeroing a group's shorts raises that group's mean. Within-group demeaning then gives each zeroed
  name a residual of about −(slot mean) − z·β, which is systematically **negative**. So a short aim re-appears on
  exactly the names that may not be shorted.
- v1 has the same effect, but diluted across the universe intercept. With groups it is concentrated.
- The execution post-block (`block_special_shorts`, swap-fin books) still stops the orders. The unblocked financing
  books (flat-300-v0, engine-tiers-v1) would short them.
- Fix: a controller ruling. Either:
  - exclude `no_short`-zeroed rows from the regression (treat them as `ok = 0`, so they are forced to 0 and count
    toward the excluded share); or
  - accept, and rely on the post-block.

  Add a merged test for ind-v1 + locate-in-aim either way. This is C1 semantics; it is not a C2 defect.

### Minor

- **M1. The fallback pool can itself hold fewer than 5 names** (`assign_slots` `:256-265`).
  - With 1 to 4 fallback names, the pool is demeaned as its own tiny group. A lone fallback name is forced to
    exactly 0, which is the thing the rule exists to prevent.
  - The TRAIN impact is nil: FF12 groups over about 3,000 used names all have ≥ 5 members, and the unknown group
    is about 1,200.
  - It would bite with FF49/SIC2.
  - Fix, before any FF49/SIC2 use: if `0 < pool < kMinGroupNames`, merge the pool into the largest occupied slot
    (lowest slot on ties). That is still an exact projection and deterministic. Pin it with a test, or document it
    in the recipe `fallback` text.
- **M2. No test exercises an ind-v2 decision that actually applies.** The Role(30) fixture only reaches the
  all-skipped early case. The windows flow through the shared `compute_price_exposures`, so the risk is low. Fix:
  use a ≥ 253-session panel and check that ind-v2 equals the within-groups primitive with 126/252.
- **M3.** `expect_same_construction` (base helper) does not compare the three new group fields. The NAV test checks
  them only on the lockstep result. Low risk, because the construction is shared.
- **M4. grp_ff12 is held as f64 over the full role geometry**, although only rows [score_begin, score_end) are ever
  read. That is 49.6 MiB at TRAIN and about 74 MiB at 1,650 dates. Follow-up for exec-review C4: compact to u8/u16
  after the SHA check, restricted to the decision window, and stream the hash if peak RSS matters.
- **M5. The ind-v2 time estimate (+1–2 s) assumes release-speed loops.** `build-equity` is Debug (/Od, checked
  spans), so 1.1e9 ADV multiply-adds is more likely +3–8 s. That is still far inside 180 s.
- **M6. The target-replay CLI error text** (`strategy_target_replay.cpp:1031`) still reads
  `(none|price-risk-v1)`. Cosmetic: the target replay refuses the industry ids anyway.
- **M7. The fields binding key is still `financing_fields` while it also records grp_ff12**, and the NAV geometry
  error reuses "fields/blend geometry" with no fields. Accepted as a schema-stability choice.
- **M8. Domain violations abort late.** A single out-of-domain id on a used row aborts a TRAIN run after minutes.
  The producer domain is 1..12 or NaN, so this is safe today. Optional: validate the finite-value domain once at
  `load_fields`, to fail before compute.
- **M9. The role manifest is now pinned and parsed three times** (`role_geometry`, `load_fields`, `load_prices`).
  Negligible cost.

## 4. V6-C1 merge conflict map

This comes from `git merge-tree --write-tree 720a0066 feat/mega-alpha-v6-c1-20260927` (tip `5e1c7f6d`). Five files
have textual conflicts; the rest auto-merge.

1. **`atx-impl/src/strategy_nav_replay.cpp`: `run_nav_replay` reserve block** (the only conflict in this file;
   trial-merge ~2050-2058). Take C2's `role_geometry` + `nav_workspace_reserve_bytes` call. Keep C1's
   `base.order_basis / locate_in_aim / liquidity_cache` assignments, which sit above the hunk and auto-merged.
   **Semantic: apply I2**, `per_name_rate = liquidity_cached(base)` in `nav_workspace_reserve_bytes`. C1's
   `nav_reserve_bytes` definition is already gone (C2 deleted it); C1 has no other caller.
   - Auto-merged, but check afterwards:
     - `validate_nav_input`: C2's industry check plus C1's `liquidity_cached` budget line;
     - `dispatch_nav_replay`: C2's `--neutralize` help and `parse_neutralize(value, cfg.target)`, plus C1's new
       flags and help text. Read the merged help string once.
2. **`atx-impl/src/strategy_target_replay.hpp`: `ConstructionDay`.** Keep both C2's
   `neutralize_groups / _unknown_group_names / _fallback_names` and C1's `locate_zeroed`. `NavReplayDay` becomes
   about 592 B; the reserve adapts via sizeof. The `TargetReplayConfig` edits (C1 `exit_rate`, C2 doc lines)
   auto-merged.
3. **`atx-impl/src/strategy_target_replay_detail.hpp`: the `form_desired` doc comment.** Concatenate C2's
   industry sentence and C1's `no_short` paragraph. C1's signature change (`std::span<const u8> no_short = {}`)
   auto-merges, and C2's 7-argument test calls stay valid through the default.
4. **`atx-impl/tests/strategy_nav_replay_test.cpp`: tail.** C2 adds 2 NavV6 tests plus the helpers `mib`,
   `panel_industry`, `add_industry` and `industry_daily()`. C1 adds 10 NavV6 tests plus `v6_aim`, `file_bytes`,
   `file_names`, `expect_same_files`, `joined`, and others. Keep both blocks. I checked for name collisions and
   found none, and the suite/test names are distinct.
5. **`atx-impl/tests/strategy_target_replay_test.cpp`: tail.** C2 adds 2 TargetReplayV6 industry tests plus
   `role_industry` and `industry_daily(TargetNeutralize)`. C1 adds 3 ExitRate tests. Keep both; no collisions.

Also auto-merged, verified clean in the trial tree:
- `strategy_target_replay.cpp` `form_desired`: C1's zeroing, then `if (!neutralizing)`, then C2's industry dispatch.
- the target dispatch `parse_neutralize(value, cfg.target)` next to C1's `--exit-rate`.
- `strategy_nav_replay.hpp`: C1's `NavExecutionOptions` / overloads and C2's reserve declaration.

Semantic item: I3.

Post-merge gate: `check` on the three sources, then build `atx-impl-strategy-target-tests`, then run
`StrategyPriceNeutralizeV6.*:TargetReplayV6.*:NavV6.*` plus the regression filter below.

## 5. Root gates to close (in order)

1. Build and run the lane, per the report:
   `check` on `strategy_price_exposures.cpp`, `strategy_target_replay.cpp` and `strategy_nav_replay.cpp`; then
   `build atx-impl-strategy-target-tests`; then
   `build\bin\atx-impl-strategy-target-tests.exe --gtest_filter=StrategyPriceNeutralizeV6.*:TargetReplayV6.*:NavV6.*`
   and the regression set
   `--gtest_filter=StrategyPriceExposures.*:StrategyPriceNeutralize.*:StrategyTargetReplay.*:StrategyNavReplay.*:TargetReplayV5.*:NavV5.*`.
2. **Golden against base.** In a leased pool tree at `04e9d5bc`, add only the (a) test body. Drop the two
   `stats.groups` / `stats.fallback_names` lines (or apply I1 first), and add `#include "atx/core/sha256.hpp"`.
   Build `atx-impl-strategy-target-tests` and run
   `--gtest_filter=StrategyPriceNeutralizeV6.PriceRiskV1BytesArePinnedOnTheExistingFixture`.
   It must pass at base **and** at 720a0066.
3. **End-to-end v1 A/B, the definitive check.**
   - Build `atx-equity-strategy-targets` (the equity-dev preset, `build-equity`) at 720a0066.
   - Re-run the v5 REF nav cell (`v5-ew-t.05-d.1-fixed`) with `--neutralize price-risk-v1` and its exact v51_train.sh
     arguments into a new directory.
   - SHA-256-compare every output file (`recipe.json`, `summary.json`, `daily_*.csv`, `events_*.csv`) against the
     base-binary outputs on disk, or against a fresh base-binary run.
   - They must be byte-identical. Optionally repeat with `--neutralize none`.
   - This covers `form_desired`, the NAV lockstep, the recipe/summary strings and the reserve change, none of which
     (a) reaches.
4. **Before the C5 TRAIN cells**, a one-line data check: `np.unique(grp_ff12[np.isfinite(grp_ff12)]) ⊆ {1..12}` on
   the fields-v6 payload. A violation on a used row aborts the run.
5. At merge: I2 (required) and I3 (a ruling).

## APPROVED

The implementation is correct and spec-compliant. The pooled-fallback reading is accepted, and the root should
record that ruling. The implementer has nothing blocking. Optional: I1's test split (move the two new-field EXPECTs
out of (a) so it cherry-picks onto base verbatim), and M1/M2 before any FF49/SIC2 reuse. The approval holds on the
root closing gates 1-3. The controller must apply I2 at the C1 merge and rule on I3.
