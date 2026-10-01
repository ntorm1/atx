# Review 6B: lane ORTH, composition rule `theme-resid-v1` (R-11)

Reviewed: `C:/atx-wt/pool-4`, branch `feat/platform-v8-orth-20260930`, head `c1cc57ce`, against the merge base with
`48c625fc` = `fd2ff7a8` (lane commits `19cc08ef`, `75534a36`, `121bfb15`, `c1cc57ce`). Read only. Nothing built or run.
The registration is from `task-R-11-report.md` section 1. The rulings are E-38, E-44, E-45 and PM4-4 in pool-2's
`progress.md`, plus rules 5 and 8 of `v8-prereg.md` and the plan's R-1 text.

## Verdict

**Does the code match the registration? Yes on every arithmetic step. Two domain gaps would stop the cell, and the
rule's re-rank step needs a ruling (O-3).**

| registered step | matches? | evidence |
|---|---|---|
| Theme order = `PRIOR_THEMES` order, limited to themes with a weighted member; any other theme refused | yes, in Python (`composition_resid.py:46-48,79-97`). The C++ runner accepts any permutation from the file (O-4). The frozen tuple has no slot for `filing_events` (O-2). | |
| z_t = parent's standardised composite; a lone present name gets z = 0; absent stays NaN | yes | `strategy_ic_theme_resid.cpp:23-34`; `composition_resid.py:189-194` |
| Theme t >= 1: least squares of z_t on an intercept and the earlier **composites**, over t's support, with an absent earlier theme = 0 | yes | `strategy_ic_theme_resid.cpp:47-67` copies `planes[j]` (the composites, never overwritten) and maps NaN to 0. The kernel `group_residualise.hpp:57-65,85-114` removes the mean first (the intercept). |
| Tolerance 1e-10 relative; a dependent regressor is dropped; a spanned residual becomes exactly 0 and adds nothing | yes | `group_residualise.hpp:103,109-112`. In `strategy_ic_theme_resid.cpp:68` a spanned theme adds nothing. |
| Residual re-ranked: centred tied rank in [-.5, .5] over the same names | yes (see O-3 on ties) | `strategy_ic_theme_resid.cpp:69-71` |
| First theme bit for bit | yes | Theme 0 adds `mass[0]*z` with the same rank expression and the same per-cell accumulation order as `add_group_rerank` (`:52-54` against `group_rerank.hpp:83`). |
| Blend uses the parent's shares, weights, signs and cap | yes | `std_mass` is the sum of pinned weights over the remapped theme index (`strategy_ic_composition.cpp:189-191`). The weights file is the parent's fit, unchanged. |
| Domain: defined only on a rerank-true `theme_standardise`, refused before any output (E-45) | partly | Non-standardised parents are refused before any output. An ic-shrink parent, which does carry rerank-true `theme_standardise`, is also refused (O-1). |
| E-44: accepts an aim parent and records the parent's rule | yes, untested | The `ew-theme-std-aim-v1` block rule is `ew-theme-std-v1` (`composition_rules.py:250`), so it is accepted. `provenance.resid.parent_composition` records `provenance.rule` (`composition_resid.py:110`). No test covers it (O-6). |

**Hand-worked check of the whole fixture.** I recomputed all 23 finite cells of `ThemeResid.ThreeThemesEqualTheRegisteredRule` by hand,
with rational arithmetic. The fixture has 3 themes and 8 names. Theme b is absent for name 1 on date 1, theme c is
absent for name 2 on date 0, and on date 2 theme b equals theme a. Every cell equals the pinned value:
- Date 0: -337/840 ... 23/420.
- Date 1: -53/140 ... 13/60.
- Date 2: 1/15 ... 13/60.

Path for one cell (date 0, theme c, support excluding name 2):
- The means of z_a and z_b over that support are -3/98 and -4/98, so the intercept is active.
- In integer-scaled form the centred normal equations are S11 = 7728, S22 = 7140, S12 = 1190, S1y = 336 and S2y = 7.
- That gives the residual ranks i3 < i0 < i7 < i1 < i5 < i6 < i4.
- So c adds .25 x [-1/12, 0, ., -1/8, 1/8, 1/24, 1/12, -1/24].

Date 2 exercises the dependent-column drop: z_b equals z_a, so b is spanned and c's z_b column is dropped. The
expected vectors are therefore correct for the registered rule. They do not come from the code: the lane's "loop
port" is a transcription of the code, but the numpy `lstsq` reference and this hand computation are independent of it.

## Answers by question

1. **Is the rule computed exactly?** Yes (table above).
   - Which names enter: theme t's support only.
   - Absent earlier theme: 0 (`:64`).
   - Intercept: a mean removal before every projection pass (`group_residualise.hpp:59,71`).
   - Rank deficiency: a column is dropped when `remaining <= 1e-10 * centred` (`:103`). That covers constant, all-zero
     (absent everywhere), duplicate or affine columns, and columns beyond n-1.
   - Spanned: `y_remaining <= 1e-10 * y_centred` sets y to exactly +0 and the theme adds nothing.
   - Re-rank: sort by (value, index), tie blocks get the average rank, range [-.5, .5], mean 0.
   - Final blend: per cell, `0 + W_0 z_0 + W_1 rr_1 + ...` in registered order.
2. **Look-ahead.** None found.
   - The work is per session and cross-sectional (`add_theme_residualised` loops `d`, `strategy_ic_theme_resid.cpp:95-99`).
   - The inputs are only the same session's theme planes, built from the same session's candidate signals. No labels,
     no returns, no other session.
   - The fitter adds no statistic, window or read; it only attaches the block (`fit_composition_weights.py:2232`).
   - The theme order is a frozen literal tuple (`composition_resid.py:46-48`). The only data-dependent part is which
     themes are weighted, and that is the parent's TRAIN admission, already counted.
3. **Determinism and stability.**
   - Worker count: the planes are bit-identical at any worker count (one writer per cell, candidates in library order),
     and `finish()` residualises serially. The pooled run equals the serial bits (test at `strategy_ic_theme_resid_test.cpp:208-217`).
   - Name order: name order changes dot-product rounding at about 1e-16. That matters only through O-3.
   - Solver: modified Gram-Schmidt with re-orthogonalisation is stable here.
   - Drop rule: on rank-grid composites, a column that is not exactly dependent keeps a relative remainder of at least
     about 1/(2(n-1))/||z||, roughly 1e-5 at n near 6,000. An exactly dependent column comes out near 1e-16. 1e-10
     sits between the two, so tiny perturbations cannot flip which regressor is dropped.
   - The remaining discontinuity is in the re-rank of tie blocks (O-3). There, the sign of a near-zero or analytically
     zero coefficient orders whole tie blocks, and MGS and `lstsq` can disagree in sign.
4. **Parent handling.**
   - A parent without a rerank-true block is refused before any output:
     - Python: `fit` at `:1804` for non-prior fits; `attach` raises before `publish_directory` (`:2232` < `:2239`).
     - C++: `composition_weights`, before any payload (test `ThemeResidRunner.BlockRefusalsPrecedeAnyPayloadOrOutput`).
   - Aim parent: accepted and recorded (untested).
   - ic-shrink parent: refused (O-1).
   - The runner does not check the weights against the parent's rule or the parent's weights file. R-11's weights are a
     deterministic re-fit of the parent's argv. Nothing mechanical compares them with the parent cell's file; only the
     report's manual identity run 2 does (O-5).
5. **Identity with the flag absent.** Byte-identical by reading.
   - `theme_planes` is rewritten but gives the same values for `redistribute`/`standardise`
     (`strategy_ic_composition.cpp:42-44`).
   - `standardise = rule != redistribute` is the same for the existing values.
   - The extra working bytes apply only under `residualise` (`:95-97`). The `else` branch in `finish()` is the old loop
     verbatim (`:339-343`).
   - The new recipe, manifest and summary keys are added only when the flag is on (`strategy_ic_admission.cpp:74,590`;
     `strategy_ic_runner.cpp:131`).
   - No `switch` over `IcThemeRule` exists, so `-Wswitch` has nothing to flag.
   - The IC result cache and VM source pins do not list the touched files (`strategy_ic_result_cache.cpp:29-39`,
     `strategy_ic_signal_cache.cpp:41-74`).
   - Fitter: `apply` returns early when the flag is absent, and no `vars(args)` is serialised. `script_sha256` changes
     (the report discloses this).
   - Runner test additions are new functions and tests only. CMake adds the source to the core library, to both /O2
     lists and to `atx-impl-strategy-ic-tests`. There is precedent for an engine test compiled into two executables
     (`ic_screen_test.cpp`).
   - There is no rule table in this lane; COMB2 owns it. ORTH's rule is a separate block and needs no row.
6. **Python against C++.**
   - The fitter never computes the blend; it only writes `{rule, order}`. Both sides derive "weighted themes" the same
     way: std-block themes with weight > 0.
   - The numpy reference matches the C++ in rank formula, the lone-name 0, absent = 0, the spanned criterion,
     W_t = sum of positive weights, and skipping sign-0 members.
   - Dropped regressors: `lstsq` (SVD cutoff) and MGS (1e-10) differ only for relative remainders between about 1e-15
     and 1e-10. That band is unreachable on rank grids (point 3).
   - Two places can disagree:
     - O-3's analytically zero coefficient, where the tie-block order comes from rounding sign.
     - O-1: the Python domain check is narrower than the C++ one once COMB2 merges, because C++ accepts any rerank-true
       row of the rule table.
7. **Tests.** Results for each plausible wrong rule (my hand computations):

   | plausible wrong rule | caught? |
   |---|---|
   | wrong theme order | yes: swap test (>.05) and exact values |
   | absent treated as missing | yes: date 1, name 1 |
   | no re-rank, or rank before residualising | yes: raw residuals are off the grid |
   | dependent column mishandled | yes: date 2 |
   | regressing on residuals (pre-rank) | **no** (O-7): every rank is identical on all three dates |
   | regressing on re-ranked residuals | **no** (O-7): every rank is identical on all three dates |
   | omitting the intercept in the composition | **no** (O-7): every rank is identical on all three dates |

   Kernel intercept omission is caught by `GroupResidualise.TwoRegressorsMatchCramerAndAreOrthogonal` (non-zero means).
   It is not caught by `TwoGroupsMatchTheClosedFormAndAreOrthogonal`, because kX and kY both have mean 0.

   With two themes, the runner test cannot catch an inverted position map, because a 2-permutation is its own inverse.
8. **Compile risk.** No line I am confident will fail (list below).

## Findings

| id | sev | file:line | scenario | smallest fix |
|---|---|---|---|---|
| R6B-O-1 | M | `atx-impl/tools/composition_resid.py:91-94`; `fit_composition_weights.py:2232`; COMB2 `composition_ic_shrink.py:166` (block `rule` = `ic-shrink-v1` / `ic-shrink-aim-v1`) | E-38 runs R-10 before R-11. E-44 says both run on the last accepted parent whatever its composition, and E-45 defines them on any rerank-true `theme_standardise`. If R-10 is accepted, R-11's fit argv is `--composition ic-shrink-v1 --theme-resid theme-resid-v1`. The block carries `rule: ic-shrink-v1, rerank: true`, but `resid_block` requires `rule == "ew-theme-std-v1"` and raises `PRIOR_ONLY` ("needs ... theme_standardise with rerank true"). The cell stops, and the message falsely says the parent is undefined under E-45, inviting a wrong "skip as undefined". The C++ runner (after the COMB2 merge) would accept it. Merge hazard: COMB2's `composition_ic_shrink.attach` lands on the same line as `composition_resid.apply`. If `apply` ends up before `attach`, every ic-shrink parent is refused for lack of a block. | Accept any rerank-true block whose rule is in the runner's table (`ew-theme-std-v1`, `ic-shrink-v1`, `ic-shrink-aim-v1`). Put `composition_resid.apply` after both attach calls. Add a fitter test on an ic-shrink and an ic-shrink-aim parent that asserts `parent_composition`. |
| R6B-O-2 | M | `composition_resid.py:46-48,79-86`; `test_composition_resid.py:94-100`; library-v8-draft E7 (`:552`); progress E-42 (`:667`); `r7-lib-v81.json` gate lists `nonreliance_402` | E7 adds theme `filing_events` to the registry and to the fitter's theme list at v8.1 (R-7, before R-11). E-42 puts `exch_switch` (R-12, v8.2) in the same theme. `REGISTERED_THEME_ORDER` is a frozen 10-tuple without it, and `theme_order` refuses any theme outside it. (a) R-7 accepted with `nonreliance_402` weighted: R-11's fit raises "themes outside the registered order" and the cell cannot run. (b) R-11 accepted, then R-12's child fit inherits `--theme-resid`: it refuses once `exch_switch` is weighted. (c) Even unweighted, the pin test fails once E7 edits the registry or `PRIOR_THEMES`. The registration's two definitions ("= `PRIOR_THEMES` registry order" and "frozen constant") then disagree. | A PM ruling before any read on where `filing_events` sits. The natural reading of "registry file order" is last, after `ownership_flow`. Then extend the tuple and the pin test, or derive the order from `PRIOR_THEMES` with a frozen prefix check. |
| R6B-O-3 | M (rule; the code is faithful) | `strategy_ic_theme_resid.cpp:67-71`; `composition_resid.py:203-211`; registration report line 20 | Re-ranking the residual spreads every tie block of z_t. Inside a block, e_t = const - sum_j beta_j z_j, so the block's names are ordered by the fitted earlier-theme combination with sign -beta, across the block's whole rank range. The parent gives them one common value. Example: theme t's composite is tied over a fraction f of its support, such as a sparse flag member (ownership_flow where only `ins_opp` is present and net opportunistic trades are 0, if that field is 0-filled), or `filing_events` (`k8_item402_63`, `exch_up_365d`). With beta_value = +0.003, those names receive W_t x a rank spread of +-f/2 ordered by -z_value: a near full-dispersion short-value bet from a statistically meaningless beta. With beta = -0.002 the next session, the order reverses, which raises planned turnover (the E-44 criterion). At an analytically zero coefficient, the order comes from the sign of a 1e-17 rounding residue, so MGS and `lstsq` and different name orders can disagree. The re-ranked "residual" then correlates with the earlier themes, against the registered hypothesis ("uncorrelated alpha"). | Needs a ruling before the cell (the rule text is silent on ties). Options: (i) treat as declared and disclose; (ii) keep z_t ties tied, by re-ranking the residual's within-tie-block mean, so each block gets one value; (iii) skip the re-rank and scale the residual to the composite's dispersion. Whichever is ruled, add a fixture with a tied composite. |
| R6B-O-4 | m | `strategy_ic_theme_resid.cpp:131-143` | The runner accepts any permutation in `order` and records only `theme-resid-v1`. A weights file with `order: ["price_momentum","value"]`, from a hand edit or a future fitter that sorts themes, runs a different rule under the same id. The weights SHA pins the bytes, but no reader of the recipe or manifest can see the order. | Check `order` against a C++ copy of the registered list, limited to the weighted themes. Or at least record `order` in the recipe and the combined manifest. |
| R6B-O-5 | m | `composition_resid.py:100-120`; `scripts/specs/v8/r11.json` | R-11's weights are a re-fit of the parent's argv. Nothing compares them with the parent cell's `composition_weights.json`, and `provenance.resid` names the parent's rule but not its weights SHA. If any fitter-affecting code lands between the parent's fit and R-11's fit (PM4-2 forbids it, but nothing enforces it), R-11 silently measures two changes. Only the report's manual identity run 2 would catch it. | Have the fitter take the parent's weights path and SHA under `--theme-resid`, and require (weights file minus block and `provenance.resid`) == parent file except `script_sha256`. Record the parent SHA. |
| R6B-O-6 | m | `test_composition_resid.py:171-245` | E-44 requires acceptance of an aim parent and a record of its rule. No test fits `ew-theme-std-aim-v1` with `--theme-resid`, so a future block-rule change for the aim variant would pass silently. | One `FitterEndToEnd` case on `ew-theme-std-aim-v1` asserting the block, the order and `parent_composition == "ew-theme-std-aim-v1"`. |
| R6B-O-7 | m | `strategy_ic_theme_resid_test.cpp:194-226`; `test_composition_resid.py:119-139`; `strategy_ic_runner_test.cpp:3079-3101` | I computed every case by hand. The 8-name fixture gives the same final ranks under the registered rule and under (a) regressing on earlier pre-rank residuals, (b) regressing on earlier re-ranked residuals and (c) omitting the intercept. Date 0: theme b's support covers all names, so the spans are equal. Date 1: rank orders are unchanged; the closest pair is i1 2.43 < i6 2.79. Date 2: b is spanned. So nothing pins the registered "composites, not their residuals" choice, which the lane itself flags as deviation 2. The runner test's two themes cannot catch an inverted position map. | Add a direct `add_theme_residualised` case with small n (4-5 names) and planes chosen so that (a), (b) and (c) flip a rank. Add a three-theme runner order using a 3-cycle. |

Integration notes (merge with COMB2 `1e8af5b8`; not lane defects):
- These conflict textually: `method_recipe` (bool vs `std::string_view standardised`), `save_combined_artifact`, and
  `PinnedWeights::theme_rule()`.
- If COMB2's 2-value `theme_rule()` is kept, a `theme_residualise` file runs `standardise` while the recipe and
  summary record `theme-resid-v1`.
- If COMB2's `(themed && rule==IcThemeRule::standardise)?std_rule:{}` is kept, the residualised run's manifest loses
  `composition_standardise`.
- `ThemeResidRunner.ResidualisesInTheBlockOrderAndRecordsTheRule` catches all three, so run it after the merge.
- Python: `composition_resid.apply` must follow `composition_ic_shrink.attach` (O-1).

Other notes:
- PM4-4: the r11 template has no machine-read acceptance field, and nothing in `scripts/` reads a turnover criterion.
  `parent: null` plus the cell-time parent matches E-44. The only gap is the description text, which is excluded.
- The K6 marginal verb and the G diagnostics treat a resid parent's themes as ew-theme-std-v1 terms (the report
  discloses this). Under rule 8 they gate nothing; R-12's marginal-IC use is outside this lane.

## Compile risk (clang-cl /W4 /WX; never compiled)

Confident failures: **0**.

Checked and expected to compile:
- `group_residualise.hpp`:
  - The implicit `span<f64>` to `span<const f64>` conversions are valid.
  - The structured bindings are used.
  - `Ok(fit)` and `Err` convert to `Result<ResidualFit>`.
- `strategy_ic_theme_resid.cpp`:
  - The `Ranked` alias matches the header's `pair<f64,usize>`.
  - The definition signature matches the declaration (`.hpp:25-28`), and `composition_residualise` matches
    `strategy_ic_detail.hpp:284`, both in `ic_detail`.
  - `ATX_TRY`/`ATX_TRY_VOID` are used in Status-returning functions.
  - `std::vector<usize>(n, n)` resolves to the count and value constructor.
  - nlohmann `!=` against a `const char*` is the existing idiom.
  - All includes are explicit (the TU is in `SKIP_PRECOMPILE_HEADERS`).
- Composition, admission and runner edits:
  - The default arguments are consistent with the `strategy_ic_detail.hpp:265-266` declaration.
  - No `switch` over the enum, so `-Wswitch` has nothing to warn about.
- Tests:
  - The gtest suite names are unique.
  - The helper names do not collide (`resid_block` is defined once).
  - `DetPool(usize)`, `IcCompositionCandidate{id, family}` and `EXPECT_EQ(u64, unsigned)` (no sign-compare) are valid.
  - The `bit_cast` includes are present.
  - `CHILD_NULLS` exists for the Python spec test.
- CMake: the source is in `atx-impl-core` and in both Debug /O2 property lists. Both test files are in
  `atx-impl-strategy-ic-tests`. The engine combine group picks up the kernel test by glob.

Residual risk (low, not a compile error): `-Wshadow` and `-Wconversion` are not enabled under clang-cl /W4
(`CMakeLists.txt:265-282`), so local `width`/`n` names and the int-to-`usize` literals (`add_theme_residualised(..., 4, ...)`)
are fine.

## What I did not read, and method

- Read in full or in the relevant hunks:
  - Lane: the full diff, `group_residualise.hpp`, `strategy_ic_theme_resid.{hpp,cpp}`, `strategy_ic_composition.{hpp,cpp}`,
    the admission and runner hunks, all new tests, `composition_resid.py`, `test_composition_resid.py`, `r11.json`, the
    `test_research_spec.py` hunks, and `fit_composition_weights.py` around `fit`, `fit_prior` and the themes.
  - Shared code: `composition_rules.ew_theme_std`, and `group_rerank.hpp`.
  - COMB2 hunks (pool-11 `1e8af5b8`): only the rule table, `method_recipe`, the runner, the fitter, and
    `composition_ic_shrink.py` block lines.
  - Plan and rulings: the plan's R-1, prereg rules 5 and 8, the rulings named, and library-v8-draft E7.
- Not read:
  - FIX-2, FIX-3 and ERA code.
  - `research_cycle.py` template application beyond key checks.
  - `test_fit_composition_weights.py` fixtures (`aim_world`).
  - The marginal verb, `nav_summ` and `book_diagnostics` beyond the grep.
  - The full LIB2 report.
  - Any data, and anything under `build-equity/`.
- Method notes:
  - Besides read-only git, Read and Grep, I ran one `python -c` to parse `atx-impl/strategies/alphas/registry.json`
    from `git show`. That is a spec file in the repo: its `themes` table and the DSLs of the small themes. I ran no
    tests, builds or data jobs.
  - The fixture's rule values and the variant comparisons in O-7 are hand computations at about 1e-4 precision. The
    closest pairs differ by at least 0.0097, so the orderings stand.
  - Whether `ins_opp` produces large tie blocks (O-3) depends on its field's zero-fill and on admission, which I did
    not read.
