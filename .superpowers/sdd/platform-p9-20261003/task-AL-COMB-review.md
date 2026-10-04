# Lane AL-COMB review

## Verdict

**APPROVE** (no Required finding open).

- Spec compliance: **PASS**. Measured against brief-AL-COMB, wave2-carry "All lanes" + "AL-COMB", rulings ALCOMB-1..4 and
  P17. The P17 rebase on D2 is a merge-time obligation; D2 is unmerged, so it cannot be done at this head.
- Task quality: **PASS**, with 6 Suggested findings.

## Reviewed SHA

`724ab82c` on `feat/p9-alcomb-20261003` (pool-19), base `1239a5ff`. The diff `1239a5ff..724ab82c` was read in full: 30
files, +3494 / -29. The lane deleted its build tree (DISK-2), so nothing was rebuilt. The lane's receipts cover the head
C++ exactly: builds a/b/d/e have Source `dc02090a`, and the only C++ change after `dc02090a` is the one-line fixture
fix `58a87d7a`, which build d covers (DirtyEntries 1). `36e1434c` / `724ab82c` add specs and the report only.

## Evidence (independent, run by the reviewer in pool-19)

1. pytest, explicit paths, from `atx-impl/tools`:
   `PYTHONDONTWRITEBYTECODE=1 "C:/Program Files/Python312/python.exe" -m pytest -q -p no:cacheprovider
   --basetemp=<scratchpad> test_composition_theme_hedge.py test_composition_mom_volman.py
   test_fit_composition_rule_plugins.py test_composition_theme_tsmom.py test_composition_rules.py`.
   Exit 0, `49 passed in 32.34s`.
2. pytest from the pool root: `... -m pytest -q -p no:cacheprovider --basetemp=<scratchpad>
   scripts/tests/test_no_python_mirror.py scripts/tests/test_no_versioned_scripts.py`. Exit 0, `11 passed in 6.38s`,
   `G-P5: 37 mirrored rule function(s) in 14 row(s) left for P9 lanes`.
3. Fitter producer fingerprints, base `1239a5ff` vs head, computed by `atx-engine/tools/code_fingerprint.fingerprint`
   on both file versions. All four are equal, so the lane's E9 is confirmed:
   - FACTOR `962cd507af511fe3`
   - CONTEXT `70e1eda51539c353`
   - AIM `f1fb0d3a5183a723`
   - AIM_ERA `9a880f97b42fdae6`
4. Template pins. SHA-256 of the committed blob and of the worktree file (both LF, `git ls-files --eol` i/lf w/lf):
   theme-cov-hedge `095d2d7e432a083f...` and mom-volman `9991895b000b9e9e...`. Both equal the `template_sha256` in
   the registration files.
5. `58a87d7a` changes one line of fixture text: the extra `}` after `"theme_sleeves":{"rule":"two-speed-v1"}`. No
   expected value is touched.
6. The lane's own logs were read (not re-run):
   - `evidence-AL-COMB/gtest-*`: CharCovHedge 5/5, strategy-ic 209/209.
   - `list-rules-vs-fixture.log`: `whole document JSON-equal: True`.
   - `build-equity/mega-p9-alcomb-{a,b,d,e}-receipt.json`: exit 0. The atx-equity-strategy-ic SHA is `b3103707...`.

## Checks

### Point-in-time

**theme-cov-hedge-v1 (`char_cov_hedge.hpp`).**
- At decision d the kernel reads score rows <= d and return rows <= d only. Pair p = d - 2 enters at d, using
  `returns(d)` = (d-1, d]. Pair d - 2 - 252 leaves at d.
- The window is therefore the 252 decisions ending d - 2, as the brief requires.
- The house clock allows reading row d: `strategy_price_exposures.hpp:35-36` says "Decision d is known after the close
  of session d", and `session_interval_returns` "reads sessions t-1 and t only".
- The hedge ratio uses the same window at block starts 253 + 21b. h is 0 until 126 (s, g) pairs exist, so the first
  active estimate falls at decision 274.
- No label enters. The fitter writes only the constants block. The runner builds returns from the scored role's own
  close / raw_close, with geometry equal to the composition's (`cc.dates = role.panel.dates()`, runner.cpp:361).
- `FutureReturnDoesNotChangePast` has teeth: perturbing session d0 moves the output at d0, and nothing earlier moves.
- The TRAIN role carries a warm-up from 2018-06, so the 274-decision start falls before score_begin there.

**mom-volman-v1.**
- sigma_b reads `sleeve[j_b-127 .. j_b-2]`. The value f(j_b-2) is realised at session j_b, and the block applies from
  session j_b. This is causal under the same house clock.
- It uses lag 2, the plan's §2.4 constant. theme-tsmom-v1 chose lag 3 ("one session of slack"). Both are registered and
  both are causal.
- The target is the running mean of sigma_0..sigma_b, the real-time form.
- The parent's in-sample weights inside the sleeve and the fixed-TRAIN schedule are accepted under ALCOMB-4.

### Hedge numerics

- **No matrix inverse.** Every slope is univariate OLS. It is NaN when the centred sum of squares is at most
  1e-12 x the raw sum (`slope`, hpp:117-124).
- **Zero-variance theme sleeve:** beta is NaN, the support is empty, H is undefined and the output is R.
- **Constant or tied R column:** `residualise_in_place` skips it. A spanned beta becomes exactly 0, so H is undefined.
- **Empty theme:** an all-NaN plane gives NaN ranks and adds nothing. `planes.empty()` returns Ok.
- **Degenerate hedge leg g** (no variance): h = 0.
- **Fewer than two present names:** no rank, matching `add_group_rerank` (identical `out += weight * r` expression,
  theme-then-block order).
- **Bit identity before h is first estimated:** `BeforeTheFirstEstimateTheHedgeIsTheParent` pins it, with and without
  a schedule.
- **Rolling sums:** they add and subtract identical re-read values. Drift is at rounding level, and the two-pass
  comparison test passes at 1e-9.
- **Open:** h has no bound (Suggested S-2).

### mom-volman with zero or tiny realised vol (ALCOMB-4)

- **sigma = 0:** refused in `mom_volman_blocks` (`strategy_ic_mom_volman.cpp:33-35`). Tested by the
  `StepVarianceClosedForm` "flat" case and the `SharedParseAndRefusals` "still" case.
- **Tiny sigma > 0:** target <= max sigma, so scale = target / sigma stays finite for any non-pathological sleeve. Only
  a ratio above about 1e308 overflows. The resulting inf mass is then refused by
  `IcComposition::schedule_theme_masses` (`strategy_ic_composition.cpp:416-419`, finite and >= 0 check).
- **Squares that underflow to 0:** refused as zero vol.
- **Non-finite sleeve value:** refused.
- **Verdict:** the unbounded scale is finite, or it is refused. ALCOMB-4 is satisfied. No test pins the tiny-sigma case
  (S-1).

### Flag-absent identity

- **IC runner.** With no `theme_hedge` block and no mom-volman `theme_schedule`, `composition_hedge` returns early.
  `composition_schedule_rules` delegates to v8's `composition_schedule` unchanged. After that:
  - `pinned.rules` is unchanged;
  - `stage_plan` gives the same plan;
  - `ic_composition_stage_bytes` returns the same bytes;
  - `attach_theme_hedge` reads nothing (`FlagAbsentIdentity` passes a role with no price field);
  - `finish` takes the old branch;
  - the manifest keys come only from running rules (runner.cpp:138-143).
- **Fitter.** The two `check_args` return when the flag is None, and both attach blocks are guarded. The fitter
  serialises no argv or args namespace. Output differs only in `script_sha256`, and the fingerprints are unchanged
  (evidence 3).
- **`--list-rules --json`.** Two rows are added, and the `incompatible` lists of theme-resid-v1, theme-tsmom-v1 and
  two-speed-v1 are extended (symmetric pairs). Existing `recipe_sha256` values are unchanged. The exe output is
  JSON-equal to the fixture (lane log). Root regenerates the fixture after P17.

### Cross-lane edits

- MR-1..MR-3 are minimal and inert when the flags are absent. Ruling ALCOMB-3 accepts them.
  - **MR-1:** the enum value is appended last; one `StagePlan` bool plus the sleeves exclusion; `set_theme_hedge`; one
    `finish` branch; additive stage bytes.
  - **MR-2:** one include and one runner line.
  - **MR-3:** one `stage_name` case.
- MR-4 is not edited here (E2 is not in this base).
- The P17 edits (rows, excludes, schema literals, declarations, the tsmom `.parse` hook, the fixture, the
  `TableCoversEveryRule` lists, the fitter and plugin pins) and the CMake tails all match the report's list. No
  unlisted file is touched.

### Other checks

- **No trial, no ledger.** No ledger or trials file is in the diff, and `LEDGER.md` is untouched. The lane's
  `research_cycle.py plan --lines-only` and `--list-rules` read no data.
- **No Python mirror.** Python computes nothing new. The mom-volman sleeve is the existing
  `composition_theme_tsmom.sleeves`; sigma, target and scale are C++. The no-mirror test passes.
- **Spec deliverables:**
  - kernel, runner rule, schedule rule, Q5 spec and templates are present;
  - the Q5 argv names existing verbs (`book_diagnostics.py book-csv`, `run --only`, risk `--book-weights`
    / `--emit-exposures`);
  - registrations carry the K-P9-11 keys;
  - templates have no `rules` key and no `change.flags` (ALCOMB-1);
  - all four brief-named gtests exist.
- **Build hazards.** New and changed C++ lines stay within 100 columns. Includes were checked by hand. The new TUs
  build without PCH in Debug (CMake block). Gtest suite names are unique across the tree.
- **Test quality.**
  - `ClosedFormFiveNames` is a real closed form, hand-checked: residual (10/21)(2, -3, 1), H = (.5, -.5, 0), h = .75, the
    hedged book return is 0, and the block timing is 9 / 13.
  - `WindowSumsAreTheTrailingWindowOls` checks the rolling sums against a two-pass computation.
  - `StepVarianceClosedForm` uses exact binary sums.
  - `ClosedForm2Names` (brief-named) is degenerate: two names are always spanned, so it never exercises an active hedge.
    The five-name test covers that case.

## Findings

| # | path:line | tag | problem | fix |
|---|---|---|---|---|
| S-1 | `atx-impl/src/strategy_ic_mom_volman.cpp:32-38` | Suggested | `scale = target / sigma` is not checked finite. A pathologically small nonzero sigma overflows to inf. It is refused only later, in `create_from_stages` / `schedule_theme_masses`, after the role has loaded, with a message that does not name the rule. No test pins the "unbounded but finite" case ALCOMB-4 asks about. | Add `std::isfinite(target / sigma)` to the block refusal, plus a `MomVolman` case with a tiny nonzero window: for example, values 1e-120 after a 1e-2 regime give a finite scale of about 1e118, accepted. |
| S-2 | `atx-engine/include/atx/engine/combine/char_cov_hedge.hpp:246-247, 288-290` | Suggested | h (the OLS slope of s on g) has no bound, and `R - h H` is not re-standardised. A theme with abs(h) > 1 enters the blend with more cross-sectional dispersion than W_t implies, which loses ew-theme-std-v1's equal-dispersion property for hedged themes. Also, a date whose present names all tie on R while the window has betas yields `-h H`, a pure hedge leg. This is the registered DMRS design (blind) and should not change mid-lane, but it is invisible. | Document both properties in the registration's `fixed_choices`. Give the IC exe a C++ producer for h per theme and block (the kernel already has `trace.ratio`; `add_theme_hedged` passes none). See S-3. |
| S-3 | `scripts/specs/p9/templates/theme-cov-hedge.json:4`, `scripts/specs/p9/templates/mom-volman.json:4`, `atx-impl/src/strategy_ic_theme_hedge.cpp:68-69` | Suggested | Both templates list printed-only items that nothing produces. theme-cov-hedge lists "the hedge ratios per theme and block". mom-volman lists "the scale per block" and the sleeve's vol / Sharpe "after scaling" (plan §5.2 P9-M). No output carries h, sigma, target or scale. A Python re-derivation would be a mirror of the C++ rule (forbidden). | Root rules before P9-H / P9-M: either the IC exe emits them (a C++ trace, PM8-12), or the template lines are cut. |
| S-4 | `.superpowers/sdd/platform-p9-20261003/task-AL-COMB-report.md:59-61`; `atx-impl/tests/strategy_ic_rules_test.cpp:146` | Suggested | The report says "no existing expected value changed", but `theme-resid-v1` incompatible size went from 2U to 4U. The fixture's three v8 rows' `incompatible` lists also changed. The changes are legitimate (they follow the new rows, P17 item 1), but the claim is wrong. | Correct the sentence so root's merge check lists these re-pinned expectations. |
| S-5 | `atx-engine/include/atx/engine/combine/char_cov_hedge.hpp:31-33` (and the `ClosedForm2Names` comment) | Suggested | "with two names it always is [spanned]" is true only when the two R values differ. Two tied names make R constant, so the residual is beta minus its mean and H = +/-0.5 is defined. | Say "two names with distinct ranks". |
| S-6 | `scripts/specs/p9/templates/theme-cov-hedge.json:4` vs `atx-impl/tools/composition_theme_hedge.py:25` | Suggested | The template says the parent may carry ic-shrink-aim-v1. The fitter's `PARENT_RULES` (the house pattern, as for tsmom, two-speed and ERC) refuses `--composition ic-shrink-aim-v1` with `--theme-hedge`, so that parent cannot carry the block. | Align the template text with the fitter's parents. |

## What root must check at merge

1. **P17.**
   - Rebase on D2's head using the report's map. Regenerate `composition_rules_list.json` from the merged exe; do not
     hand-merge it.
   - D2's `ParamsSchemaConstantsAreTheRegisteredOnes` needs the two rows.
   - Re-run the anchored filters: `CharCovHedge.*:ThemeHedgeRule.*:MomVolman.*:CompositionRules.*:`
     `StrategyIcComposition.*:ThemeTsmom*:TwoSpeed*:ThemeResid*:StrategyIcRunner.*`.
   - The lane never built `atx-impl-tests`. Run `ThemeHedgeRule.*:MomVolman.*:CompositionRules.*` there.
   - The rebased head is a new SHA; review it again or have root verify it.
2. **MR-4.** `wave_kinds.COMPOSITION_FLAGS` needs the two entries. Confirm that E2's preflight checks per rule, not
   for table completeness. Otherwise the two new rows without flags would refuse unrelated v2 manifests.
3. **Identity.**
   - X-5 w argv on the new exe: byte-identical.
   - X-5 fit argv: weights / admission equal except `script_sha256`. The fingerprints were confirmed equal above.
4. **Housekeeping.**
   - Add the `atx.p9-rule-registration/v1` and `atx.p9-diagnostic-spec/v1` literals to classes.json (SQL2-CLS).
   - The template pins need E2's `scripts/specs/** text eol=lf`. Re-pin when `parent` is set.
5. **Before the cells.**
   - The Q5 diagnostic runs before P9-H is registered.
   - ALCOMB-4's fixed-TRAIN schedule limit goes into the p9-prereg.
   - S-3 needs a ruling.

## Checked

- [x] `.agents/cpp/agent.md` §10 applied to the diff. Every path into the kernel validates its inputs before writing.
  Refusals write nothing (tested). Scratch growth is caught for `bad_alloc` and `length_error`. There is no UB or
  narrowing found. Single-threaded and deterministic (sorts are on (value, index) keys).
- [x] The diff stays inside the brief's scope plus the listed cross-lane edits (P17's three; MR-1..3 under ALCOMB-3)
  and the CMake tails.
- [x] The report's evidence matches its claims. E9 fingerprints, the template pins and the pytest runs were re-run;
  receipts and logs were read. One inaccuracy: S-4.
