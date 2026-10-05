# Lane AL-COMB re-review 1 (combined round, Ruling ALCOMB-5)

## Verdict

**APPROVE**: 0 Required, 3 Suggested.

- Spec compliance: **PASS**. Checked against brief-AL-COMB, wave2-carry "All lanes" and "AL-COMB", rulings ALCOMB-1..8
  (ALCOMB-4, -5, -5a, -6, -7 item by item), P17, D1-PSCHEMA / CM-5 / D2-1 / D2-2 (the D2 shape).
- Quality: **PASS**. The merge keeps D2 whole and adds only AL-COMB's two rules. S-1, S-3, S-4, S-5 and S-6 are
  fixed with tests that bite. S-2 is documented. No expected value was edited to fit, and no Python mirror was added.

## Reviewed SHA

`efb7ab2e` on `feat/p9-alcomb-20261003` (pool-19).

- History since the last review:
  - lane head `724ab82c`, review `9692a73e`;
  - merge `aca3570d` (parents `9692a73e` and D2's approved head `49af0a52`; D2's own code is not re-reviewed);
  - `97a2d1c3` (S-1..S-6, the only C++ commit after the merge);
  - `6ec102fb` (fixture);
  - `efb7ab2e` (specs and report).
- `97a2d1c3..efb7ab2e` touches no C++. So the head's C++ is exactly the source of build `p9-alcomb-f`: receipt Source
  `97a2d1c3`, DirtyEntries 0, exit 0.
- Not rebuilt. Nothing in this review needed a build to decide.

## Evidence (independent, run by the reviewer in pool-19)

1. **Composition pytests.** Run from `atx-impl/tools`:
   `PYTHONDONTWRITEBYTECODE=1 "C:/Program Files/Python312/python.exe" -m pytest -q -p no:cacheprovider
   --basetemp=<scratch>/bt-comp test_composition_theme_hedge.py test_composition_mom_volman.py
   test_composition_theme_tsmom.py test_composition_two_speed.py test_composition_rules.py test_composition_theme_erc.py
   test_composition_resid.py test_composition_ic_shrink.py test_composition_engine_fit.py test_composition_fit_fixture.py
   test_fit_composition_rule_plugins.py`
   - Exit 0: `132 passed, 1 skipped, 36 subtests passed in 41.05s`.
   - The skip is D2's `RealExecutables`, which needs the exe environment variables.
2. **Guards.** A separate invocation from the pool root: `... -m pytest -q -p no:cacheprovider --basetemp=<scratch>/bt-guard
   -s scripts/tests/test_no_python_mirror.py scripts/tests/test_no_versioned_scripts.py`
   - Exit 0: `11 passed in 3.14s`.
   - It prints `G-P5: 34 mirrored rule function(s) in 13 row(s)`, which is D2's count: AL-COMB adds no mirror.
3. **Fitter suites.** `test_fit_composition_weights{,_pool,_store}.py` (same flags), exit 0: `142 passed in 104.80s`.
4. **Fitter producer fingerprints.** Recomputed with `atx-engine/tools/code_fingerprint.fingerprint` on D2 `49af0a52` and
   on the head:
   - OUTPUT `("fit","fit_prior")`: `d0153b259e951b66` -> `d28a30895ad1393e`;
   - FACTOR `962cd507af511fe3`, CONTEXT `70e1eda51539c353`, AIM `f1fb0d3a5183a723` and AIM_ERA `9a880f97b42fdae6`: equal.

   This confirms the report's figures exactly.
5. **Merge reconstruction.** For every file both sides touched, the reviewer ran `git merge-file -p` (lane / base
   `1239a5ff` / D2) into scratch and compared the result with `aca3570d`.
   - Real conflicts, as the report says:
     - `atx-impl/CMakeLists.txt` (1): both tails kept, markers removed, nothing else changed;
     - `strategy_ic_rules.cpp` (3);
     - `fit_composition_weights.py` (2).
   - `strategy_ic_rules.hpp`, `strategy_ic_runner.cpp` and `test_fit_composition_rule_plugins.py` are the plain text
     merge.
   - `strategy_ic_rules_test.cpp` is the plain merge plus exactly two includes and the two
     `ParamsSchemaConstantsAreTheRegisteredOnes` rows.
   - The fixture at `aca3570d` is the plain text merge, byte for byte (not hand-edited).
   - File counts: 82 files = 51 D2-only + 23 lane-only + 8 both. All 51 D2-only files are byte-equal to D2 at the merge
     and at the head. These include `composition_resid.py` (`parent_check`), `composition_engine_fit.py`,
     `composition_rules.py` and `strategy_marginal_pair_cache.cpp` (the pair-cache pin sources).
   - The lane-only files equal the lane's versions except the three Python adaptations the report lists.
6. **Specs.**
   - `wave_manifest.rule_cell_problems` and `registration_problems` give `[]` for both fragments.
   - The K-P9-11 keys in each fragment equal the registration's.
   - SHA-256 of the committed blob equals the pin: theme-cov-hedge `7c6778bb...dfb15`, mom-volman `7e9b26c1...db6fb`.
     `git ls-files --eol` shows `i/lf w/lf`.
   - `research_cycle.load_spec` resolves both templates. The fit flags are X-5's
     (`--composition ew-theme-std-v1 --theme-erc theme-erc-v1`): no rule flag comes from a template.
7. **Lane logs, read and not re-run.** Build `p9-alcomb-f` receipt. In `r5-gtest-*`:
   - combine CharCovHedge 5/5 and the whole binary 248/248;
   - strategy-ic anchored 111/111. This covers `ListRulesJsonPinned`, `ParamsSchemaConstantsAreTheRegisteredOnes`, the
     four new tests and `MarginalIc.PairCacheSourcesPinned`;
   - strategy-ic whole binary 215/215.
8. **Dry run against root's head `70104732`.** Legacy `git merge-tree 49af0a52 70104732 efb7ab2e`, which writes no
   objects: 6 files are changed on both sides and the output has no conflict markers (see "What root must check").

## Checks

### (a) The merge resolution

- **`strategy_ic_rules.cpp`.** The diff from D2 to the merge only adds lines, except for one replacement: AL-COMB's five
  `*_excludes` arrays replace D2's `resid_excludes{2}` / `resid_ids`. Those arrays were reviewed at `724ab82c`, and the
  new rows force them.
  - D2's `*_params_schema` literals and comment, `exe_capabilities` and every other row are untouched.
  - `volman_params_schema` and `hedge_params_schema` follow D2's K-P9-9 form exactly: rule `const` plus typed `const`
    constants, none required, `additionalProperties: false`. `ParamsSchemaConstantsAreTheRegisteredOnes` pins them to
    the C++ constants (`st::mom_volman_*`, `st::theme_hedge_*`), not to the literals.
  - Dropping `theme` / `sessions` / `sleeve` from mom-volman's schema is what D1-PSCHEMA forces: params are what a
    manifest may restate. The block content is still checked by `composition_volman` (`:76-86`), which is unchanged
    since the last review.
- **`fit_composition_weights.py`.**
  - D2's `compose(engine)` is kept. The two attach blocks sit inside it (`:2610-2618`), after `--two-speed` and before
    `composition_resid.apply`, as before.
  - Both blocks are gated on `getattr(args, ...) is not None`.
  - D2's engine-fit path (`:2634-2656`) is untouched.
- **Adaptations forced by the new shape.**
  - CM-5 / D2-2 `producer_fingerprint` replaces `module_sha256` in `composition_mom_volman`. The closure reaches
    `composition_theme_tsmom`, a `LOCAL_MODULES` member.
  - The `--mom-volman` + `--engine-fit` refusal (ALCOMB-7) is checked before any compute, and the test proves nothing
    is published.
  - `--theme-hedge` is accepted under `--engine-fit`; its block holds constants only. `EngineFit` checks that the spec
    block rides on the C++-valued document and that `check_written` holds.
  - Two pytests are re-pinned to the params form. Their old block-schema assertions cannot hold under D1-PSCHEMA, so
    this is not an edit to fit.
- **Nothing else** of AL-COMB's reviewed behaviour changed. `strategy_ic_mom_volman.cpp`, `strategy_ic_theme_hedge.cpp`
  and the kernel are byte-equal to the lane at the merge.

### (b) The commits after the merge

- **S-1** (`strategy_ic_mom_volman.cpp:38-44`). `scale = target / sigma` and `target` are checked to be finite. The
  refusal names the rule and the decision.
  - The test `TinyVolatilityGivesAFiniteScaleAndOverflowIsRefused` (`:242-276`) has teeth. With r = 3e-162 the squares
    are subnormal, not zero, so sigma is about 3.1e-162 > 0 and passes the old sigma guard. The target is about 6e152,
    so target / sigma is about 2e314, which is inf. On the pre-fix code this case returned Ok, and the `ASSERT_FALSE`
    fails there.
  - The cut-at-253 control proves the refusal belongs to that block.
  - The tiny case (scale about 5e117) is finite, accepted and scheduled.
  - The sum is computed directly for each block (no rolling sum), so the regime switch leaves no residue.
  - ALCOMB-4 holds: the scale is finite, or it is refused, and zero vol is refused.
- **S-3.** Two summary entries are added to each scored role:
  - `composition_hedge_trace` (`strategy_ic_theme_hedge.cpp:205-232`) gives `{rule, themes, semantics, blocks:
    [{decision_index, session_ns, h[t]}]}`. h is the kernel's `trace.ratio` at each block start 253 + 21 b
    (`:100`). By the kernel's step 5, that is the ratio estimated at j_b on the window ending j_b - 2, held to the next
    block.
  - `composition_schedule_trace` (`strategy_ic_mom_volman.cpp:164-187`) gives `{from_session, sigma, target, scale}`,
    the values `mom_volman_blocks` returned, carried in `PinnedWeights`.
  - Both entries are written only inside `if (combined)` (runner `:572-573`), and only when the rule's block parsed
    (`hedge_themes` non-empty / `schedule == mom-volman-v1`).
  - The tests are independent of the code under test. `TheTraceIsTheKernelRatioAtEachBlockStart` reruns the kernel
    itself and checks: bit-equal at each start, held to the next start, at least one non-zero h, the blend bits
    unchanged, a lone output refused, a short role with no block. `SummaryTracePrintsTheHedgeRatiosPerBlock` checks a
    JSON round trip, the Internal refusal and that flag absent prints nothing. `MomVolman.SummaryTracePrintsTheScalePerBlock`
    is bit-equal to `mom_volman_blocks` and prints nothing under tsmom.
  - The traces hold exactly what the templates now promise:
    - hedge: "the theme names and, per block start 253 + 21 b, h per theme";
    - mom-volman: "sigma_b (before scaling), target_b, scale_b".
  - The other printed items are the standard v8 cell figures: turnover per unit gross, cost per traded dollar, net
    Sharpe at 4x. The sleeve vol / Sharpe line is conditional under ALCOMB-6.
  - One prediction still names no producer (S-2 below).
- **S-4.** The report sentence is corrected (2U -> 4U, plus the three v8 rows' `incompatible` lists).
- **S-5.** Both comments are corrected (kernel step 3 and `ClosedForm2Names`). Comments only.
- **S-6.** The hedge template's parent text equals the fitter at this head: `PARENT_RULES = (ew-theme-std-v1,
  ic-shrink-v1)`; with or without `--theme-erc`, `--theme-tsmom` and `--mom-volman`; refused with `--theme-resid` and
  `--two-speed` (`composition_theme_hedge.check_args`); and `ic-shrink-aim-v1` is no longer offered. The mom-volman
  template is a strict subset of what the fitter accepts (S-1 below).
- **ALCOMB-6 and the re-pin.**
  - Field by field, `97a2d1c3 -> efb7ab2e` changes only these:
    - the template's sentence "has no producer ... until the PM rules one" becomes the D3-conditional sentence;
    - one `fixed_choices` line names D3 and the per-cell fit mode (ALCOMB-7);
    - a `requires` line;
    - a `history` row;
    - `template_sha256`.
  - The re-pin follows that template edit (pin = blob SHA, evidence 6). No constant, prediction or rule step moved.

### (c) Fixture commit `6ec102fb`

- It is the only file in that commit.
- It changes only the `params_schema` of `mom-volman-v1` and `theme-cov-hedge-v1`: 18 insertions, 44 deletions.
- No other row moved, and no `recipe_sha256` or `incompatible` list moved.
- Against D2's fixture, the head adds the two rows plus the three v8 rows' `incompatible` entries. D2's v8 rows'
  `params_schema` and `recipe_sha256` are unchanged.
- It is LF and UTF-8 (not a PowerShell UTF-16 redirect). `ListRulesJsonPinned` compared it with the built exe's
  `--list-rules --json` (JSON-equal) and passed.

### (d) Point in time

- **Hedge.** The kernel is unchanged except for one comment.
  - Every loading and the hedge ratio at d use the pairs p <= d - 2. The returns of p = d - 2 are session d, which is
    known after the close of d.
  - `FutureReturnDoesNotChangePast` is present and unchanged, and it bites: it asserts a non-zero ratio at d0, that a
    future perturbation moves nothing at or before d0, and that a perturbation of session d0 moves the output at d0.
  - The trace only reads the ratio the kernel already applied.
- **mom-volman.** The window is j - 127 .. j - 2, pinned by the closed form in `StepVarianceClosedForm` (m =
  clamp(j - 201, 0, 126)).
  - The schedule is fit on the TRAIN sleeve and applied by session.
  - The registration states the limit in `fixed_choices`: "the schedule is fixed on the fit's TRAIN sleeve (Ruling
    ALCOMB-4): a role outside those sessions keeps the parent's masses before the first recorded block and the last
    block's after it".
  - The code matches this: `composition_stages` places each block at the first session >= `from_session`.

### (e) Flag-absent identity

- **IC exe.** With neither block present:
  - `composition_hedge` returns early;
  - `composition_schedule_rules` delegates to v8's `composition_schedule`;
  - `attach_theme_hedge` reads nothing;
  - `finish` takes its old branches;
  - `ic_composition_stage_bytes` returns before `theme_hedge_working_bytes`;
  - both trace functions return before writing.

  `FlagAbsentIdentity` and `StrategyIcRunner.*` pass. Root's X-5 w-argv byte identity is the remaining check.
- **Fitter.** `admission_sha256` moving is legitimate. The input that changed is the fitter's OUTPUT_PRODUCERS AST
  closure, and the chain is:
  1. `fit_prior` gains the two gated attach blocks and the module names.
  2. `fit` reaches the extended `PARENT_CHECKED_MODIFIERS`.
  3. So `admission.json` `inputs.producer_fingerprint` moves (`d0153b...` -> `d28a30...`, evidence 4).
  4. `provenance.admission_sha256`, the SHA-256 of those bytes, follows.

  This is CM-5 working as designed: the producing code changed. `admission.csv` is byte-identical, and the store-record
  fingerprints are equal, so the store is reused. D2's `parent_check` already sets the fitter identity and
  `admission_sha256` aside (`composition_resid.py:230-240`). Root's M2d D2 identity already treats `admission_sha256`
  as a consequence key.

### (f) Registrations, templates and Q5

- **K-P9-11.** `source_sample_end` (2017 / 2016), `predicted_mechanism` (one line) and `data_class: H` are present in
  both files and both fragments.
- **One variant each**, with `params: {}`.
- **Windows and ids.**
  - Windows: 252, 126, 21, and min_pairs 126; lag 2 is a clock, not a window.
  - Ids and names, with their lengths (W1-I1): `theme-cov-hedge-v1` 18, `mom-volman-v1` 13, `p9-theme-cov-hedge` 18,
    `p9-mom-volman` 13.
  - Templates carry no `rules` key and no `change.flags` (ALCOMB-1).
- **No outcome read.** Nothing reads a 2020-2023 outcome. The Q5 spec is unchanged since `724ab82c`. It prints only
  G-2a ex-ante variance shares, `mean_ex_ante_vol_annual`, coverage, uncovered gross and by-year shares (all ex-ante or
  position quantities); it opens no book statistic, and the risk verb's bias verdict is not read.

### (g) Hygiene and C++ §10

- No expected value was edited to fit:
  - the `StageWiringAndRefusals` byte formula follows the new trace model, restated term by term;
  - 2U -> 4U is S-4's documented re-pin;
  - the params-form re-pins are forced by D1-PSCHEMA.
- No Python copy of a C++ rule: sigma, target, scale and h are C++ only (guard green).
- No data opened, and nothing dated >= 2024-01-01.
- §10 on the lane's own C++ diff (`aca3570d..efb7ab2e`):
  - **Bounds.** `starts[b] < dates` by `theme_hedge_block_count`, and `in_force` is sized `dates` exactly when traced.
  - **Kernel trace.** An empty trace span means "not written", which the kernel defines.
  - **Allocation.** New allocations are caught (`bad_alloc` / `length_error`).
  - **Out parameters.** They are replaced only on success, and one without the other is refused before any write.
  - **Overflow.** Overflow is checked in `theme_hedge_working_bytes`.
  - **Attributes.** `[[nodiscard]]` and `noexcept` are where they hold.
  - **Determinism.** No unordered iteration; single-threaded.
  - **Build hygiene.** No changed C++ line exceeds 100 columns, and the build is warning-free (receipt f).
  - **Test names.** Suite names are unique across the tree (`MomVolman`, `ThemeHedgeRule`, `CharCovHedge` are defined
    only in the lane's files).

## Findings

| # | path:line | tag | problem | fix |
|---|---|---|---|---|
| S-1 | `scripts/specs/p9/templates/mom-volman.json:4` (description) and `:12` (root-sets-parent line); `atx-impl/tools/composition_mom_volman.py:50` (`PARENT_ONLY`) | Suggested (minor) | The mom-volman parent list is narrower than what the fitter and the IC exe accept. `--mom-volman`'s `check_args` (`:129`) refuses only `--theme-resid`, `--theme-tsmom`, `--era` and `--engine-fit`. Neither `composition_two_speed.check_args` nor `composition_theme_hedge.check_args` refuses `--mom-volman`, and the C++ `volman_excludes` is {resid, tsmom} (`strategy_ic_rules.cpp:405`). A parent carrying `--two-speed` and/or `--theme-hedge` is therefore accepted, but the template offers only "with or without --theme-erc". The S-6 pin test (`test_composition_theme_hedge.py:101`) covers only the hedge template. The template is stricter, which is the safe direction, so this is not Required. | Either list `--two-speed` / `--theme-hedge` as accepted parent modifiers, or state that the template restricts further on purpose. Add a mom-volman twin of `test_parent_text_is_the_parents_the_fitter_accepts`. Re-pin `template_sha256` if the template changes. |
| S-2 | `scripts/specs/p9/templates/theme-cov-hedge.json:4` ("Predicted before any read: book ex-ante and realised vol ratio to the parent < 1; gross alpha ratio about 1") | Suggested (minor) | The ex-ante half of the prediction names no producer in the cell. Plan §5.3 registers only "P9-H vol ratio < 1". A producer exists: `book_diagnostics.py` G-2a `mean_ex_ante_vol_annual` over `--emit-holdings` plus `risk --book-weights`, the Q5 path. But neither the template nor the registration says it runs for the cell and its parent. This is ALCOMB-5's "no figure without a producer". | PRE round 2: either name G-2a on both books (zero trials, as Q5) as the ex-ante producer, or drop "ex-ante" and keep the realised vol ratio. |
| S-3 | `atx-impl/src/strategy_ic_theme_hedge.cpp:196-201` vs `atx-impl/src/strategy_ic_mom_volman.cpp:92-97` | Suggested (minor) | The "standardised theme names by composition index" loop is now in both parses. Both use `.at()` throws as their error path: the runner's `catch (const std::exception&)` (`strategy_ic_runner.cpp:811`) turns these into InvalidArgument, and the standardise parse validated the names first, so they cannot be reached in practice. Even so, a `Status`-returning parse that relies on throws goes against §4. | P10 tidy, not now, because it would touch reviewed code: one helper returning `Result<std::vector<std::string>>`, used by both parses. |

## What root must check at merge

1. **Conflicts with root's head `70104732`** (S2, D2, COV, AL-SIG, A3, SQL1 / SQL2 merged; E2 not).
   - The legacy dry run `git merge-tree 49af0a52 70104732 efb7ab2e` shows 6 files changed on both sides, and all of
     them merge as text without markers:
     - `strategy_ic_detail.hpp`: S2's edits, plus MR-5's four `PinnedWeights` fields;
     - `strategy_ic_rules.cpp`: S2's `exe_capabilities`, plus AL-COMB's rows, params literals, excludes and `stage_name`;
     - `strategy_ic_runner.cpp`: S2's label-terminal guard and D2, plus MR-2's include, `attach_theme_hedge` and the two
       trace calls at the tail of `score_role`;
     - `atx-impl/tests/CMakeLists.txt`: COV's tail and AL-COMB's tail, keep both;
     - `composition_rules_list.json`: S2's three capabilities (`ic-hac-rule`, `label-terminal`, `min-coverage`) plus
       AL-COMB's two rows and the `incompatible` entries;
     - `strategy_ic_rules_test.cpp`.
   - A clean text merge is not proof. Build, then require `CompositionRules.ListRulesJsonPinned` and a JSON-equality of
     the merged exe's `--list-rules --json` against the merged fixture. If they differ, regenerate the fixture with the
     exe in its own commit, never by hand.
2. **Build and anchored filters.**
   - Targets: `atx-engine-combine-tests`, `atx-impl-strategy-ic-tests`, `atx-equity-strategy-ic` and `atx-impl-tests`.
     The lane never built `atx-impl-tests`.
   - `atx-engine-combine-tests --gtest_filter=CharCovHedge.*`.
   - `atx-impl-strategy-ic-tests --gtest_filter=CharCovHedge.*:ThemeHedgeRule.*:MomVolman.*:CompositionRules.*:`
     `StrategyIcComposition.*:ThemeTsmom*:TwoSpeed*:ThemeResid*:StrategyIcRunner.*:MarginalIc.PairCacheSourcesPinned`.
   - `atx-impl-tests --gtest_filter=ThemeHedgeRule.*:MomVolman.*:CompositionRules.*`.
   - The pytest sets of evidence 1-3. Run guards and `scripts/tests` in separate invocations.
3. **Cross-lane edits.**
   - ALCOMB-3 accepted MR-1..MR-3 as first written.
   - This round extends MR-1 (`IcCompositionResult::hedge_block` / `hedge_ratio`, the third argument of
     `theme_hedge_working_bytes`) and MR-2 (the two trace calls). It also adds MR-5 (D1's `strategy_ic_detail.hpp`).
   - All of these are what ALCOMB-5 S-3 ordered: "the IC exe prints". They are listed in the report and inert when the
     flags are absent. Record root's acceptance.
4. **MR-4 and E2.** E2 is not in root's head. Its unmapped-rule guard (ruling E2-8) and its tests read the real fixture
   (`IC_FIXTURE`). At E2 `558070e5`, three assertions in `scripts/tests/test_wave_kinds.py` depend on AL-COMB being
   absent:
   - `:315`: `abbreviated("--the", ...) == ["--theme-erc", "--theme-resid", "--theme-tsmom"]`. Once MR-4 maps the
     hedge, `--theme-hedge` joins that list.
   - `:318-320`: `unmapped_ic_rules(tables) == []` fails without MR-4. With MR-4, `unmapped_ic_rules(more) ==
     ["theme-cov-hedge-v1"]` fails.
   - `:493-501`: the guard test uses `theme-cov-hedge-v1` as its "unmapped" id.

   What follows from this:
   - **If AL-COMB merges first:** nothing to do at AL-COMB's merge. E2's merge must, in the same commit:
     - add MR-4's two `COMPOSITION_FLAGS` rows (`theme-cov-hedge-v1: {fit: {--theme-hedge: theme-cov-hedge-v1}}`,
       `mom-volman-v1: {fit: {--mom-volman: mom-volman-v1}}`); without them, every v2 wave of every kind stops at
       preflight;
     - move those three assertions to a never-registered id and to a table-derived expectation;
     - then run E2's `scripts/tests` suite on the AL-COMB fixture, beside D2's "E2 flip" item.
   - **If E2 merges first:** the same edits go into AL-COMB's merge commit.
5. **Identity.**
   - X-5's w argv on the merged exe must be byte-identical to root's pre-merge exe output. No `theme_hedge` and no
     mom-volman block means no trace key.
   - X-5's fit argv against root's pre-merge fit: only these differ:
     - `admission.json` `inputs.producer_fingerprint`;
     - weights `provenance.producer_fingerprint`;
     - `provenance.admission_sha256`.

     OUTPUT moves `d0153b259e951b66` -> `d28a30895ad1393e`. FACTOR, CONTEXT, AIM and AIM_ERA are unchanged, so the
     fit store is reused.
   - R-11 parents fitted at D2's head still pass `parent_check`, which sets the fitter identity and `admission_sha256`
     aside.
6. **`classes.json` (SQL2-CLS).** Add the literals `atx.p9-rule-registration/v1` and `atx.p9-diagnostic-spec/v1`, as in
   commits `c0446b19` / `6673f8d1`.
7. **eol=lf.**
   - At `70104732`, root's `.gitattributes` has no `scripts/specs/** text eol=lf`, and `core.autocrlf=true`.
   - `scripts/specs/p9/*` checks out `w/crlf` there today (`git ls-files --eol`). So after the merge checkout, the two
     templates on disk hash differently from the LF pins (`7c6778bb...`, `7e9b26c1...`), and `wave_manifest.file_sha256`
     reads disk bytes.
   - Until E2's R0-7-EOL is in and the files are re-checked-out, verify the pins against the blobs
     (`git show HEAD:<path> | sha256sum`).
   - Re-pin when `parent` is set (and if ALCOMB-6 cuts the line).
8. **W1-MIN M-2 (theme-table seam).** It holds at this head.
   - AL-COMB adds no theme and touches neither `registry.json` nor `builtin_registry_themes`.
   - mom-volman's `price_momentum` is in the 13-name prefix (`strategy_ic_rules.hpp:43-47`).
   - The two 13-theme pins, `test_composition_resid.py` and `test_fit_composition_rule_plugins.py`, are inside
     evidence 1 (green).
   - Re-run them on root's merge.
9. **Q5 diagnostic.** Run `scripts/specs/p9/diagnostics/al-comb-q5-style-variance.json` with zero trials, before the PM
   registers P9-H.
   - Use the parent's NAV holdings; an `--emit-holdings` re-run is checked byte for byte and is not ledgered.
   - Then `risk --book-weights --emit-exposures all` on atx-risk-v1.1, then G-2a under E-18's caps.
   - Print q and q_lit into the prereg. The same G-2a can serve S-2's ex-ante vol ratio.
10. **Prereg items.**
    - ALCOMB-4: the fixed-TRAIN schedule limit goes into the p9-prereg for P9-W and OD-3.
    - ALCOMB-6: at PRE round 2, keep the mom-volman sleeve vol / Sharpe sentence only if D3 delivered its C++ producer;
      otherwise cut it and re-pin.
    - ALCOMB-7: each cell's fit mode is its P9-B0 parent's. `--engine-fit --mom-volman` stays refused until D3.

## Needs a PM ruling

- **Optional; it saves root an edit at merge.** E2 is in a fix round now (`558070e5`, re-review 2 CHANGES_REQUIRED).
  Ordering E2 to replace `theme-cov-hedge-v1` as its "unmapped" example with a never-registered id, and to derive the
  `abbreviated("--the", ...)` expectation from the table, removes the coupling in item 4. Otherwise root edits E2's
  test in whichever of the two merges comes second.
- **Nothing else.** The MR-1 / MR-2 extensions and MR-5 follow from ALCOMB-5 S-3; root records its acceptance (item 3).

## Checked

- [x] `.agents/cpp/agent.md` §10 applied to the lane's C++ diff of this round (above).
- [x] The diff stays inside the brief's scope, P17's three edits, MR-1..MR-3 (ALCOMB-3) and their S-3 extensions plus
  MR-5 (listed, for root), and the CMake tails. There is no `atx-db/`, ledger or `LEDGER.md` edit, and no trial.
- [x] The report's evidence matches its claims. The reviewer re-ran the pytests, the fingerprints, the pins, the specs
  validation and the merge reconstruction, and read the logs and receipt. The lane's build tree is deleted (only
  receipts and logs remain).
