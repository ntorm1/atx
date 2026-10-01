# Review 6b-comb: signal-combination rules (FIX-3 round 1, COMB2)

Read-only review. Nothing built, run or edited in any worktree. No file under `build-equity/` and no data file opened.

| lane | worktree | range reviewed |
|---|---|---|
| A, FIX-3 round 1 | `C:/atx-wt/pool-10` (`feat/platform-v8-fix3-20260930`) | `b6899ed7..75acb091` (code commit `00e710ec`); the aim-v1 path also against `fd2ff7a8` |
| B, COMB2 | `C:/atx-wt/pool-11` (`feat/platform-v8-comb2-20260930`, head `1e8af5b8`) | `fd2ff7a8..1e8af5b8` (`fd2ff7a8` is the merge base with `48c625fc`) |

I read the rulings E-27, E-27a, E-27b, E-28, E-35, E-35a, E-38, E-44 and E-45 in `pool-2/.../progress.md`, the R-10 registration and its Round 1, the FIX-3 Round 1 section, plan R-1 and R-3, and prereg rules 5 and 8. For the cross-lane checks I also looked at the integration tree `pool-2`. It moved during this review, from `8ea2e7bf` to `486aa4f3`.

## 1. Per-rule verdicts

| rule | matches registration | basis |
|---|---|---|
| `ew-theme-aim-v2` (E-27a/b) | **yes** | w_k = (1/T) g_k / sum_theme g, then the member cap 1/(2T). The code is shared with `ew-theme-std-aim-v1`: `pool-10 composition_rules.py:218-226` (`theme_gain_weights` = `tier_weights` 183-189, then `member_cap` 192-215) and `ew_theme_aim_v2` 236-254. The fitter dispatch is at `fit_composition_weights.py:2170-2174`. `ew-theme-aim-v1` is back to the bytes of `fd2ff7a8`: `git diff fd2ff7a8 75acb091` shows only added lines plus one changed error-message string (2035-2036). The v8 refusal is at `research_cycle.py:400-404`. |
| `ic-shrink-v1` (R-10 §1) | **yes** | Steps run in the registered order: the IC is `rows[k]["train_mean"]` (`pool-11 fit_composition_weights.py:2168-2172`), then shrinkage, floor, share and the equal-share fallback (`group_shrink.hpp:74-85`; Python `composition_ic_shrink.py:97-116`). Then share/T (`strategy_ic_shrink.cpp:36-37`; Python 141), then the 1e-12 relative cap with excess pro rata to the other themes' unfrozen members, repeated to a fixed point (`group_shrink.hpp:109-134`; Python `member_cap`). Infeasible input is refused. The runner re-check uses a 1e-12 absolute tolerance (`strategy_ic_admission.cpp:552`). |
| `ic-shrink-aim-v1` (E-44) | **yes** | It uses the same shares as `ic-shrink-v1`, multiplies them by the gains, renormalises inside each theme with the same expressions in the same order as `tier_weights` (`strategy_ic_shrink.cpp:41-49`; Python 144), then applies the cap after (`:51`; Python 145-146). The gains are the parent's (`fit_composition_weights.py:2170`, from `ensure_records(aim=True)` because the id is in `AIM_RULES`, line 217). |

## 2. Hand-worked examples, compared with the code and the fixtures

**ew-theme-aim-v2, example 1** (the FIX-3 unit test `test_aim_v2_weights_within_theme_then_capped`). Gains are a = (1, .25, .25) and b = (.1, .1), T = 2, cap 1/4.
- Within each theme: a = (1/3, 1/12, 1/12), b = (1/4, 1/4).
- Pass 1: a0 is capped. Its 1/12 goes to b0 and b1, which become 7/24 each.
- Pass 2: b0 and b1 are capped. Their 1/12 goes to a1 and a2, which become 1/8 each.
- Result {1/4, 1/8, 1/8, 1/4, 1/4}, the test's value. The v5 rule gives theme a 5/6 of the weight, and the test asserts that too.

**ew-theme-aim-v2, example 2** (my own). A has gains (.6, .3, .3) and B has (.9, .1).
- Uncapped: {.25, .125, .125 | .45, .05}.
- Pass 1: b0 is capped. The .2 excess goes to A, which is scaled by 1.4: {.35, .175, .175}.
- Pass 2: a0 is capped. The .1 excess goes to b1, which becomes .15.
- Result {.25, .175, .175 | .25, .15}, summing to 1. `theme_gain_weights` follows exactly this path. The v5 rule would give {.222, .111, .111, .5, .056}.

**ic-shrink-v1**: I re-derived the shared fixture `ic_shrink_v1.json` (T = 4, cap 1/8).
- value: shrunk {11/3000, 1/600, -1/750}, shares {11/16, 5/16, 0}.
- momentum shares {5/12, 1/4, 1/3}. quality takes the equal share 1/3 each. flow shares {5/18, 7/18, 1/3}.
- a1 at 11/64 is capped. Its excess 3/64 scales the other nine members by 17/16, in one pass. Every fraction in the file is right.

The runner fixture (`strategy_ic_runner_test.cpp`):
- liquidity shares {7/18, 5/18, 6/18}; size shares {1/4, 3/4, 0}.
- volume_lag_2 at 3/8 is capped. The liquidity members are scaled by 5/4, giving {35/144, 25/144, 30/144 | 1/8, 1/4, 0}. Right.

My own example: A has ICs {.010, .000} and B has three ICs of .002.
- Shares are {.75, .25} for A and 1/3 each for B, so w = {.375, .125 | 1/6 ×3}.
- a0 is capped. The .125 excess scales B by 1.25, giving {.25, .125, 5/24, 5/24, 5/24}. This is the code's path.

**ic-shrink-aim-v1**: I re-derived `ic_shrink_aim_v1.json`.
- Shares × gains: {1/4, 1/8, 4/15 | 9/16, 3/8, 0}. Theme sums are 77/120 and 15/16.
- Before the cap: {15/77, 15/154, 16/77 | 3/10, 1/5, 0}.
- b1 is capped. The value theme is scaled by 11/10, giving {3/14, 3/28, 8/35 | 1/4, 1/5, 0}.
- Without gains: {15/64, 9/64, 3/16 | 1/4, 3/16, 0}. Every value is right.

The runner fixture with gains {.5, 1, 1, .5 | .2, .9}:
- liquidity before the cap: 7/46, 10/46, 6/46. size: 5/16, 3/16, 0.
- volume_rank is capped. Liquidity is scaled by 9/8, giving {63/368, 45/184, 27/184 | 1/4, 3/16, 0}. Right.

The comment's gain list is ordered by library position, not by theme as it is labelled; the values match the struct.

**Python and C++ agreement.**
- Same expressions: shrinkage, floor, share, the in-theme renormalisation, and the cap's excess × base / receivers.
- Different orders:
  - The fitter passes members in admission-rank order (`active`, lines 2062 and 2123); the runner uses library order (`strategy_ic_admission.cpp:531-542`).
  - The cap visits themes in sorted-name order in Python and in first-appearance order in C++.
  - These differences move only the last bits, except at the measure-zero boundaries in R6B-C-8.
- What protects the result: the runner refuses any mismatch above 1e-12, so a disagreement fails loudly. It can never pass silently.

## 3. Answers to the seven questions

**Q1 (rule code = registration).** Yes for all three rules (section 1). The order is theme share, then renormalisation, floor, fallback and cap.
- Floor before share: `group_shrink.hpp:78` then `:84`.
- The fallback applies only when the theme's floored mass is 0 (`:81`).
- The cap comes after the 1/T theme share in both implementations.
- The excess goes pro rata by current weight to unfrozen members of the other themes. A zero-weight member receives nothing, and receivers with no weight are refused.
- Tolerances: the cap uses 1e-12 relative (`ic_shrink_cap_tolerance`, `CAP_TOLERANCE`); the runner uses 1e-12 absolute.
- There are no disagreements beyond 1e-12 apart from the measure-zero boundaries in R6B-C-8.

**Q2 (information set).**
- `ic-shrink-*` reads only `rows[k]["train_mean"]` (2170) and, for the aim variant, the gains (`aims[k]["gain"]`, 2171). These come from the same `screen_v4` call that writes `admission.json` (1477).
  - `train_mask` = [`rw.TRAIN_BEGIN_NS`, `rw.TRAIN_END_NS`) (275-277, 2039).
  - Under `--era`, the pooled decisions are used.
- No new window and no new store kind. The gains are the parent's: same records, same `AIM_SEMANTICS` TRAIN window. The fitter test asserts that the gains in the file equal the `ew-theme-std-aim-v1` fit's gains.
- The runner cannot re-derive the ICs. It trusts the SHA-pinned file, which is bound to `library_sha256` and `train_manifest_sha256` (`strategy_ic_admission.cpp:634-658`).
- No path accepts an IC from another role or library: the weights file and the record store keys bind both.
- One thing remains open: the fields-manifest binding of a TRAIN w pass was not re-checked here (see section 6).

**Q3 (refusals).** Each refusal fires before any output is written.
- In C++, `composition_weights` runs before payloads, and the tests remove `close.f64` and assert no output and an empty log.
- In Python, every refusal fires before `publish_directory`.

| case | where it is refused |
|---|---|
| wrong parent composition | the r10/r3 maps refuse at load |
| missing or non-finite IC or gain | Python 130-134; C++ 525-529 and `strategy_ic_shrink.cpp:25-29` |
| theme with no positive value | the equal-share fallback, as registered (not a refusal) |
| fewer than 2T members / infeasible cap | refused |
| rerank off | `standardise_block` 585-587 |

Two refusal gaps:
- A weights file whose `provenance.rule` and block rule disagree is **not** refused (R6B-C-5).
- `ew-theme-aim-v1` in a v8 spec is refused by template map, verdict, `--protocol v8` and add-alpha (`research_add_alpha.py:352` before any write). It is not refused when it is spelled `--composition=…` or abbreviated, when `--protocol=v8` is used, or when the fitter is called directly (R6B-C-4).

**Q4 (identity with the new rules unselected).**
- C++:
  - Row 0 of the table is exactly the old check: `ew-theme-std-v1`, rerank off allowed, no verify.
  - `pinned.standardise` and `standardise_rule()` produce the old strings, so the recipe `composition_standardise`, the combined manifest key and the summary bytes are unchanged.
  - A rerank-off block keeps both keys absent, as before (`detail.hpp:157-159`, `runner.cpp:497-498`, `admission.cpp:70-76`).
  - The only call of `method_recipe` with three arguments still uses the defaults (admission.cpp:161).
- Python:
  - The new `elif` branches come after the old ones and `attach` is guarded by `RULES`. `test_flag_absent_paths_never_touch_the_rule` mocks the module to raise, and the std, std-aim and v1 fits stay byte-identical.
  - The script and module SHA fields change, which is the expected exception.
  - The fingerprints of the producers of the store records are untouched.
  - FIX-3's `V1BytesUnchangedByV6` pins `ew-theme-aim-v1` again against `04e9d5bc`.
- Caches: the VM source pin (`strategy_ic_signal_cache.cpp:41-74`) checks only the include closure of the listed engine files, so `group_shrink.hpp` does not move the cache keys.

**Q5 (templates).**
- r3 (`pool-10 r3-aim-gain.json:16`) and r10 (`pool-11 r10.json:18`) map the parent's value of `--composition`.
- The parent is resolved first, including a template parent (`research_spec.resolve` → `load`), so a derived composition is seen correctly.
- An add-alpha parent copies the fit flags, runner phases and `ic.w_flags` (`research_add_alpha.py:180-183`), so the maps apply.
- An unmapped or `=`-spelled value refuses at load. Load sits under plan, run and lock.
- Caps: R-1, R-3-on-R-1 and every chain allowed by E-45 carry 3072/"3072". r10 also forces them itself (R6B-C-7).
- The integration tree has already deleted the two non-std keys (`8ea2e7bf`), as E-45 requires. I do not report them.

**Q6 (can the tests fail on a wrong rule).** Yes. Every new test compares against exact independent fractions or an independent loop port (`ref_cap`, `written_shares`).
- `ic-shrink-v1` alternatives that would fail:
  - equal shares;
  - intensity 0 or 1;
  - no floor;
  - floor after normalisation (it changes the value theme: {11/12, 5/12, 0} against {11/16, 5/16, 0});
  - no fallback;
  - no cap.
- aim alternatives that would fail:
  - no gains;
  - no in-theme renormalisation;
  - global renormalisation;
  - gains after the cap.
- aim-v2 alternatives that would fail: the v5 global rule; bit-equality with std-aim at equal tiers; equal gains.

Coverage gaps, which do not make any test vacuous:
- No fixture has two themes spilling in one pass, so the Python/C++ theme-order difference is not exercised.
- The aim fixtures have no equal-share theme. The shared kernel covers it.

**Q7 (compile risk).** Section 5.

## 4. Findings

| id | sev | file:line | scenario | smallest fix |
|---|---|---|---|---|
| R6B-C-1 | M | `pool-2 fit_composition_weights.py:238-243` (whitelist), check at about 1923; registration `task-R-10-report.md:19-21`, `composition_ic_shrink.py:8-10` | After integration, `POOLED_COMPOSITIONS` excludes `ic-shrink-v1` and `ic-shrink-aim-v1`. The R-10 registration claims the pooled era decisions under `--era`. **Scenario:** R-10 is accepted and V8-F carries `--composition ic-shrink-v1` → the OD-3 history read (pooled or one-era) refuses by name → the read is blocked at the freeze gate. This is exactly what E-35 ruled must not happen for v8 compositions. At the lane head (`pool-11` 1817, 1867, 2033), ic-shrink-v1 pools but `ic-shrink-aim-v1` already refuses, because `aims=None` reaches `require(aim == …)`. | Add both ids to `POOLED_COMPOSITIONS`. Feed the aim variant the pooled aim records as std-aim does. Add a one-era equality test against the single window. Needed before any OD-3 read of a V8-F that descends from R-10. |
| R6B-C-2 | M | `pool-2 docs/plans/mega-alpha-v8-pitch.config.json` (the `v8.cells` ladder stops at R-7); `r10.json` (criterion in prose only) | E-44's mechanical criterion for R-10 (planned turnover per unit gross not higher than the parent) is not computed or checked anywhere in code. **Scenario:** R-10 has dSR > 0 and passes mechanics, but turnover per gross is higher than the parent's → nothing flags it, and the P-3 ladder check (recorded verdict against the computed rule) cannot run because R-10 has no row. The same gap applies to R-11 (and to R-8, R-9, R-12). | Before R-10's verdict, add ladder rows for R-10 and R-11: parent = the last accepted cell, check `tau_gmv_mean per mean_gross_leverage_all_rows le`, copied from R-1's row. Cross-lane (report config). |
| R6B-C-3 | M (**already fixed at integration `393910ed`**) | ORTH `composition_resid.py` `resid_block` (it required `rule == ew-theme-std-v1`) against COMB2's new block ids | E-44 runs slots 49-51 on the last accepted parent, and E-45 defines R-11 on any rerank-true `theme_standardise`. **Scenario:** R-10 is accepted → the R-11 fit refuses `--theme-resid`, while the C++ runner (`strategy_ic_theme_resid.cpp:118`) would accept it. I read the `393910ed` diff (`STANDARDISE_RULES` = std-v1 plus both ic-shrink ids); it closes this. | None left beyond confirming `393910ed` merges with its test. |
| R6B-C-4 | m | `pool-10 research_cycle.py:400-404, 420-423, 426-430`; the fitter argparse (`fit_composition_weights.py` around line 2345, `allow_abbrev` defaults to True) | The E-27b guard is lexical. **Scenario:** the v8 spec has `fit.flags: ["--composition=ew-theme-aim-v1"]` (or `["--compo", "ew-theme-aim-v1"]`, or two `--composition` pairs with v1 last) → `option_value` sees no v1 → the spec validates → argparse fits the v5 rule. The same happens with `summ.extra: ["--protocol=v8"]` and no `verdict`: `summ_protocol` returns None, so the spec is treated as v7. Direct fitter calls are unguarded (FIX-3 risk 1). | In `validate_v8_keys`, refuse `--composition=`/`--protocol=` forms and duplicate `--composition` in a v8 spec. Set `allow_abbrev=False` in the fitter parser. Optionally, after the fit, check that `provenance.rule != ew-theme-aim-v1` for v8 specs. |
| R6B-C-5 | m | `pool-11 strategy_ic_admission.cpp:609-621` (only the block's rule is read) | The rule recorded in the file is not bound to its block. **Scenario:** a weights file has `provenance.rule: ic-shrink-v1` but `theme_standardise.rule: ew-theme-std-v1` → the runner accepts it, the ic-shrink verification is skipped, and the recipe and manifest record `ew-theme-std-v1`. The fitter cannot write this (disjoint attach branches at 2239-2242), so this is defence-in-depth only. | When `provenance.rule` is one of the two ic-shrink ids, require `block.rule == provenance.rule` (std-aim-v1 maps to the std-v1 block). |
| R6B-C-6 | m | `group_shrink.hpp:129-131`, `composition_rules.py:211`; registration "Infeasible cap: refused" | Floored members cannot receive excess, so R-10 can be infeasible where R-1 was feasible. **Scenario:** T = 3. A has one positive member (two floored), B has one positive member (one floored), C has {.5, .5}. Pass 1 caps a1 and b1 and C rises to 1/3 each. Pass 2 caps C and no unfrozen member with weight remains → the fit is refused. The tier shares of std-v1 are all positive and stay feasible on the same library. The same applies to aim-v2 on a B0c with many single-member themes, which FIX-3 risk 3 and ERA also note. What a refused fit means for the ledger (undefined, so no trial; or a failed cell) is not ruled. | A one-line PM ruling before R-3 (if R-1 is rejected) and before R-10. |
| R6B-C-7 | m | `pool-11 r10.json` `change.set` (`runner.phases.w.max_rss_mib: 3072`, `ic.w_flags.--max-memory-mib: "3072"`) | The set overwrites rather than raises. **Scenario:** a ruling raises the w-pass cap above 3072 on the chain, for example for a larger library from R-2 or R-7 → R-10 lowers it back to 3072 → the IC exe's admission refuses the w pass or the runner kills it. Under E-45 every R-10 parent chain already holds R-1's 3072, so these two keys can only lower a cap, never raise one. | Delete the two `set` keys and inherit from the chain. Update `test_r10_derives_its_rule…` to match. |
| R6B-C-8 | m | `pool-11 fit_composition_weights.py:2062, 2123, 2168-2170` (members in admission-rank order) against `strategy_ic_admission.cpp:531-542` (library order); cap theme order (sorted names against first appearance); report claim at `task-R-10-report.md:45-46` | The two sides sum in different orders, so they agree to the last bit only away from the discontinuities. **Scenario:** a theme with ICs {.001, -.002, -.002}: the mean is -.001, so shrunk_a = 0 up to rounding → one order gives mass 0 and equal shares of 1/3, the other gives about 1e-19 and shares {1, 0, 0} → the weights differ by 2/(3T) → the runner refuses the w pass. The failure is loud, and on real data it is measure-zero. | Pass the members to `ic_shrink` in library order (sort `active` by library index) and visit the cap's themes in first-appearance order. Python then equals C++ bit for bit. |

Counts: I 0, M 3 (C-3 already fixed at integration), m 5.

## 5. Compile risk (clang-cl `/W4 /permissive- /WX`, which is `-Wall -Wextra` with no `-Wshadow`, `-Wconversion` or `-Wsign-conversion`; `CMakeLists.txt:265-275`)

**Lines I am confident will fail: none.**

What I checked:
- `strategy_ic_shrink.{hpp,cpp}`: includes complete; `co`/`cb` aliases in the unnamed namespace are visible to the function; string ↔ string_view conversions are explicit where needed; every `Result` is consumed; no unused variables or parameters.
- `group_shrink.hpp`: `assign(groups, 0U)` and `count(groups, 0U)` pick the size constructors; `co::Ok(pass)` deduces `Result<usize>`; the `[[nodiscard]]` results are used.
- `strategy_ic_admission.cpp`: the `constexpr std::array<StandardiseRule,3>` takes a `string_view` from the `inline constexpr const char*` (constexpr `char_traits::length`); the verify functions are declared at 480-481 in the same unnamed namespace and defined at 560-565; `std::set::contains` is C++20; `ATX_TRY_VOID` under an `if` is a do/while statement; `theme_name`, `quiet_nan` and `Library` are in scope; `<array>` was added.
- `method_recipe`, `save_combined_artifact`, `score_role`: the bool-to-`string_view` parameter change is applied at every call site (admission.cpp:161 uses the defaults; runner.cpp:654, 693-695 and 497-498). No other caller exists in src or tests.
- Tests: all helpers exist (`run_named`, `file_sha`, `text_of`, `combined_rows`, `member_rows`, `weights_v2`, `ATX_IMPL_TESTS_DIR`); aggregates are fully initialised (no `-Wmissing-field-initializers`); the range-for over `{"train","validation"}` is an existing idiom; unsigned/unsigned `EXPECT_EQ` comparisons; no duplicate suite names.
- CMake:
  - `strategy_ic_shrink.cpp` is in `atx-impl-core` and in both IC-TU property lists.
  - `strategy_ic_shrink_test.cpp` is listed in `atx-impl-strategy-ic-tests` and also globbed into `atx-impl-tests`, which is fine.
  - `combine_group_shrink_test.cpp` is listed in `atx-impl-strategy-ic-tests` and globbed into `atx-engine-combine-tests`, the same pattern `combine_marginal_rank_ic_test.cpp` already uses.

Low-confidence watch items, none expected to fail:
- gtest printing of `std::string_view` in `EXPECT_EQ(st::ic_shrink_rule, "ic-shrink-v1")`.
- The integration merge with ORTH changes the `method_recipe` signature again (it adds `bool residualised`). That is the integrator's merge, not these lanes.

## 6. What I did not read

- The full `strategy_ic_runner.cpp` (only the diff hunks), `strategy_ic_composition.cpp` (the per-date standardise path is unchanged and was not re-read), and `strategy_ic_theme_resid.cpp` (only lines 104-148).
- The `ensure_records` / `RecordStore` key code beyond the comments: whether a store record can leak across roles or fields builds is taken from the C-1 design, not re-verified. The same applies to the fields-manifest binding of a TRAIN w pass.
- Whether the label `fwd=r[d+2]` of the last TRAIN decisions can touch 2024 sessions. That depends on the role seal (bridge sealed 2024-01-01, role to 2023-12-29), which I did not open.
- ERA's pooled aim code after integration (`dbfc09bc`), apart from the whitelist, and the integration commits `8ea2e7bf`, `f6288685` and `486aa4f3`, apart from their messages and the `393910ed` diff.
- `mega_report/v8.py` beyond the pitch config's cell list; `cycle_verdict.py` and `cycle_resume.py` beyond grep hits; the test helpers' bodies in `strategy_ic_runner_test.cpp` (signatures only).
- The FIX-3 phase-0 findings (F-1, F-8, F-9, F-14 and the minors). Only round 1 is in scope.
- No build, test or data run (lane rule).
