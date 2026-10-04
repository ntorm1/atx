# Lane AL-COMB report

## Outcome

DONE_WITH_CONCERNS. Lane AL-COMB delivers the composition rules `theme-cov-hedge-v1` and `mom-volman-v1`. They
include the engine kernel, the runner rules and the fitter flags. The rule registrations, two cell templates and the
lit section 8 Q5 zero-trial diagnostic spec are also delivered. The C++ was built in the lane's own pool (Ruling
W2-BUILD). Every gtest in the two built test binaries passes, and so does every pytest suite the lane touches. The
concerns are listed under "Concerns". The main ones:

- The lane has more cross-lane edits than P17's three. They were made before Ruling ALCOMB-2 and are listed below as
  merge requests for root.
- The P17 rebase on D2 is still to do. D2 is WIP in pool-20.

No trial was run and nothing was appended to any trials ledger.

## Branch / SHA

- Branch: `feat/p9-alcomb-20261003` in `C:/atx-wt/pool-19`.
- Base: `1239a5ff` (W2-EARLY).
- Commits on the base, oldest first:
  - `7c5c9a32` wip: the code and tests, written before the owner stop.
  - `dc02090a` the handoff.
  - `58a87d7a` test fixture fix.
  - `36e1434c` specs.
  - This report's commit.
- History was not rewritten (see Decisions). The tree is clean after the report commit.

## Frozen base / lease

- Base: `1239a5ff`, reused pool-19 (no lease call, per the wave-2 dispatch).
- The tree was clean at resume (`git status --porcelain` was empty at `dc02090a`).

## Files changed (vs `1239a5ff`)

Owned (new):

- `atx-engine/include/atx/engine/combine/char_cov_hedge.hpp`: the kernel. It is header-only, as `group_rerank.hpp` is.
- `atx-engine/tests/combine/combine_char_cov_hedge_test.cpp`: globbed into `atx-engine-combine-tests`.
- `atx-impl/src/strategy_ic_theme_hedge.{hpp,cpp}` and `atx-impl/tests/strategy_ic_theme_hedge_test.cpp`.
- `atx-impl/src/strategy_ic_mom_volman.{hpp,cpp}` and `atx-impl/tests/strategy_ic_mom_volman_test.cpp`.
- `atx-impl/tools/composition_theme_hedge.py` and `atx-impl/tools/composition_mom_volman.py`, plus their tests
  `test_composition_theme_hedge.py` and `test_composition_mom_volman.py`.
- `scripts/specs/p9/rules/theme-cov-hedge-v1.json` and `scripts/specs/p9/rules/mom-volman-v1.json`: the rule
  registrations.
- `scripts/specs/p9/templates/theme-cov-hedge.json` and `scripts/specs/p9/templates/mom-volman.json`: the cell
  templates.
- `scripts/specs/p9/diagnostics/al-comb-q5-style-variance.json`: the Q5 diagnostic spec.

Cross-lane, inside P17's three:

1. The row edits in D1's / D2's table:
   - `atx-impl/src/strategy_ic_rules.cpp`:
     - two rows (volman after tsmom, hedge last);
     - the `*_excludes` arrays (resid 2 -> 4 entries; tsmom, volman, sleeves and hedge pairs);
     - the `volman_schema` / `hedge_schema` literals;
     - two includes.
   - `atx-impl/src/strategy_ic_rules.hpp`: two declarations.
   - The K-P9-6 pins that follow the rows: `atx-impl/tests/fixtures/composition_rules_list.json` (two rows added and
     the `incompatible` lists) and `atx-impl/tests/strategy_ic_rules_test.cpp` `TableCoversEveryRule` (lists
     extended). Expected values re-pinned by the new rows (corrected in the combined round, review S-4): in
     `TableCoversEveryRule`, theme-resid-v1's `incompatible` size goes from 2U to 4U; in the fixture, the
     `incompatible` lists of the three v8 rows theme-resid-v1, theme-tsmom-v1 and two-speed-v1 gain the new
     rows' ids. Each follows the new symmetric pairs (P17 item 1); no other existing expected value changed.
2. The plugin entry:
   - `atx-impl/tools/fit_composition_weights.py`: two imports, a `PARENT_CHECKED_MODIFIERS` tail, two attach blocks.
   - `atx-impl/tools/test_fit_composition_rule_plugins.py`: the `MODIFIER_MODULES` pin, the `written` dict and the
     absent-flag loop.
3. The schedule hook: the tsmom row's `.parse` becomes `&composition_schedule_rules`. That dispatcher sends
   `mom-volman-v1` to the volman parse and everything else to v8's `composition_schedule`, unchanged.

CMake tails (P2, allowed by the carry "All lanes"):

- `atx-impl/CMakeLists.txt`: one block (two sources, the Debug /O2 flags as for `strategy_ic_rules.cpp`).
- `atx-impl/tests/CMakeLists.txt`: one `target_sources(atx-impl-strategy-ic-tests ...)` block.

### Merge requests for root (Ruling ALCOMB-2)

These edits go beyond P17's three. They were written before the ruling. They are on the branch because the hedge cannot
run without them (it needs the role's returns inside the composition). Root accepts them or rules otherwise.

| # | file | edit | flag-absent effect | if declined |
|---|---|---|---|---|
| MR-1 | `atx-impl/src/strategy_ic_composition.{hpp,cpp}` | <ul><li>`IcStageKind::hedge` appended last</li><li>`StagePlan.hedge`; a hedge plus sleeves stage plan is refused</li><li>`IcComposition::set_theme_hedge(std::vector<f64>)`</li><li>`finish` branch `else if (p.hedge)` -> `add_theme_hedged`</li><li>`ic_composition_stage_bytes` adds `theme_hedge_working_bytes` iff a hedge stage</li></ul> | None. With no hedge stage every path is the D1 one: `ThemeHedgeRule.FlagAbsentIdentity` and `StrategyIcComposition.*` pass, and the stage bytes equal v8's | `theme-cov-hedge-v1` cannot run. Revert MR-1, MR-2 and the hedge row; mom-volman is unaffected |
| MR-2 | `atx-impl/src/strategy_ic_runner.cpp` | One include plus one line after `composition.emplace(...)`: `ATX_TRY_VOID(attach_theme_hedge(pinned,role,*composition));` | Nothing is read unless the hedge row is running. `FlagAbsentIdentity` passes a role with no price field | as MR-1 |
| MR-3 | `atx-impl/src/strategy_ic_rules.cpp` `stage_name` | `case IcStageKind::hedge: return "hedge";` (follows MR-1) | none | with MR-1 |
| MR-4 | lane E2's `scripts/wave_kinds.py` `COMPOSITION_FLAGS` (not edited here; E2 is not in this base) | add `"theme-cov-hedge-v1": {"fit": {"--theme-hedge": "theme-cov-hedge-v1"}}` and `"mom-volman-v1": {"fit": {"--mom-volman": "mom-volman-v1"}}` | none (new keys) | E2's preflight refuses the two rules' v2 manifests ("the composition row has no fitter flag") |

## Required carry items: what each became

- **Brief (1) theme-cov-hedge-v1.** Kernel `char_cov_hedge.hpp`:
  - per theme, beta_perp on the theme's own sleeve over the trailing 252 decisions ending d-2, orthogonalised to the
    score;
  - h by trailing OLS, re-estimated every 21 decisions from decision 253;
  - `w = R - h H`.

  It is estimated causally at apply time from the role's own guarded session returns (price-risk-v1's
  `session_interval_returns`). The runner rule `strategy_ic_theme_hedge.{hpp,cpp}` is one row of D1's table (block
  `theme_hedge`, stage `hedge`, last). The fitter writes only the constants block.
- **Brief (2) mom-volman-v1.**
  - The price_momentum mass is scaled by target_b / sigma_b. sigma_b is the 126-decision realised volatility ending
    j_b - 2. target_b is the running mean of the sleeve's own estimates.
  - It is a `theme_schedule` rule beside theme-tsmom-v1: shared block, shared dispatch, mutually exclusive.
  - The fitter records the sleeve exactly as tsmom's step 1. The C++ computes sigma, target and scale.
- **Brief (3) Q5 diagnostic.** `scripts/specs/p9/diagnostics/al-comb-q5-style-variance.json`, zero trials. See "Q5"
  below.
- **Brief (4) templates and registrations.**
  - Templates `scripts/specs/p9/templates/{theme-cov-hedge,mom-volman}.json`. Per ALCOMB-1 they are plain
    `atx.research-cycle-template/v1` with no `rules` key and no fitter flag. Each has nominal parent X-5
    (`scripts/specs/v8/x-theme-erc.json`), renames its fit, card, w, nav and monitor outputs, and lists its open
    preconditions in `requires`. Both resolve through `research_cycle.load_spec` (evidence E7).
  - The `rules:` block lives in the v2 wave manifest. Each registration file carries a paste-ready
    `manifest_fragment`: `{schema: atx.research-wave/v2, kind: rule, rule_cell: {template, template_sha256, name,
    K-P9-11 keys}, rules: [{name: <id>, params: {}}]}`.
- **Registration files.** `scripts/specs/p9/rules/<id>.json` holds:
  - status `proposed` with its history;
  - the K-P9-11 keys `source_sample_end`, `predicted_mechanism` and `data_class: "H"`;
  - every constant with why it is fixed;
  - the predictions registered before any read;
  - the fitter flag, as a merge request (MR-4);
  - `requires`.

  `wave_manifest.rule_cell_problems` and `registration_problems` accept both fragments. The template pins equal the
  committed template files (E8).
- **source_sample_end, read from the papers** (the ALSIG-1 method; web read on 2026-10-03):
  - DMRS: `1963/07 - 2017/06` (NBER w24164 rev0, lit [21]'s URL, p35 and every table), so **2017**.
  - mom-volman: Cederburg et al. say "the sample periods end in December 2016" (lit [17]'s URL), so **2016**.
    Barroso-Santa-Clara is 1927:03-2011:12 and is noted as the basis.
  - The handoff's "about 2017, [verify]" for Cederburg was wrong. It is corrected.
- **Carry P17 (rebase on D2's head before merge; three cross-lane edits).**
  - The rebase is NOT done. D2 is WIP in pool-20 (`d61f73c2`, then `a17319fb` handoff) and has not been merged.
  - The three edits are as listed. The edits beyond them are MR-1..MR-3.
  - What the rebase must re-apply is in "P17 rebase map" below.
- **Carry D1 minor (two pytests pin 13 themes).** Not touched. AL-COMB adds no theme, so neither pin moves.
- **Carry: root runs the Q5 diagnostic between wave 2 and the P9 cells.** The spec is ready (either risk store is
  allowed by K-P9-12; see "Q5").

## Contracts

- **K-P9-6.** Two rows with `id`, `version`, `block_key`, `stage`, `params_schema`, `incompatible[]` and
  `recipe_sha256`. On the built exe, `atx-equity-strategy-ic --list-rules --json` is JSON-equal to the fixture (E6).
- **K-P9-9.** The manifest fragments use `rules: [{name, params}]`, with `name` as K-P9-9 states.
- **K-P9-11.** Both registrations and both `rule_cell` fragments carry the three keys, validated with E1's
  `wave_manifest` functions.
- **K-P9-12.** Q5 names atx-risk-v1.1 as the store it reads. The COV store is allowed but has no G-2a reader.

## Tests written (named by the brief, plus pins)

- `CharCovHedge.ClosedForm2Names`, `.FutureReturnDoesNotChangePast` (brief). Also `.ClosedFormFiveNames`,
  `.WindowSumsAreTheTrailingWindowOls` and `.RefusesBadInputAndCountsScratch`.
- `ThemeHedgeRule.FlagAbsentIdentity` (brief). Also `.RegisteredConstantsAndRow`, `.BlockParseAndRefusals`,
  `.BeforeTheFirstEstimateTheHedgeIsTheParent`, `.AddThemeHedgedIsTheMassTimesTheKernel`, `.StageWiringAndRefusals` and
  `.SessionReturnsAreTheGuardedIntervalReturns`.
- `MomVolman.StepVarianceClosedForm` (brief). Also `.RegisteredConstants`, `.ScheduleFromTheWeightsFile` and
  `.SharedParseAndRefusals`.
- pytest: `test_composition_theme_hedge.py` and `test_composition_mom_volman.py` (17 tests). The plugin pins in
  `test_fit_composition_rule_plugins.py` were extended.
- First build: `ThemeHedgeRule.BlockParseAndRefusals` threw a JSON parse error. The cause was one closing brace too
  many in its two-speed fixture text, so it never reached the refusal. Commit `58a87d7a` fixes the fixture text. No
  expected value changed.

## Evidence

All builds were run with `scripts/research-build.ps1` in pool-19, one at a time, after the memory gate admitted them.
First `powershell -NoProfile -ExecutionPolicy Bypass -File scripts\atx-build.ps1 configure -Preset equity-dev` ran
(exit 0, "Configuring done (56.8s)"). The receipts and build logs are kept in `C:/atx-wt/pool-19/build-equity/`
(`mega-p9-alcomb-{a,b,d,e}-*`). The gtest and pytest logs are in `.superpowers/sdd/platform-p9-20261003/evidence-AL-COMB/`.

| tag | targets | exit | TUs | s | exe SHA-256 |
|---|---|---|---|---|---|
| `p9-alcomb-a` | `atx-engine-combine-tests` | 0 | 234 | 555.7 | `5a0be9a7cf3dcd365d99b5131eb0a7958119ac76cfa64de9b048d245e7a3c285` |
| `p9-alcomb-b` | `atx-impl-strategy-ic-tests` | 0 | 105 | 577.0 | `757b1b3b...` (pre-fix) |
| `p9-alcomb-c` | `atx-impl-strategy-ic-tests` | refused by the gate (`memory admission: free=1887 commit=975`), no receipt; waited and retried as `d` | - | - | - |
| `p9-alcomb-d` | `atx-impl-strategy-ic-tests` | 0 | 8 | 20.3 | (after `58a87d7a`'s fix) |
| `p9-alcomb-e` | `atx-equity-strategy-ic` | 0 | 8 | 19.9 | `b31037072cb44b546858dca76294cffaf0ce3a457150246f51918b4da334f2e3` |

There were no compiler warnings from any lane TU. The only warnings were the pre-existing `/MP` unused flag and
databento's `getenv`.

- E1: `build-equity/bin/atx-engine-combine-tests.exe --gtest_filter=CharCovHedge.*`, exit 0:
  `[==========] 5 tests from 1 test suite ran.` / `[  PASSED  ] 5 tests.`
  - The whole binary, exit 0: `[==========] 248 tests from 45 test suites ran.` / `[  PASSED  ] 248 tests.`
- E2: `build-equity/bin/atx-impl-strategy-ic-tests.exe
  --gtest_filter=CharCovHedge.*:ThemeHedgeRule.*:MomVolman.*:CompositionRules.*:StrategyIcComposition.*:ThemeTsmom*:TwoSpeed*:ThemeResid*:StrategyIcRunner.*`,
  exit 0: `[==========] 104 tests from 11 test suites ran.` / `[  PASSED  ] 104 tests.` The suites were CharCovHedge
  5, ThemeHedgeRule 7, MomVolman 4, CompositionRules 6, StrategyIcComposition 7, ThemeTsmom 7, ThemeTsmomRunner 2,
  TwoSpeedRunner 2, ThemeResid 9, ThemeResidRunner 4 and StrategyIcRunner 51.
- E3: `build-equity/bin/atx-impl-strategy-ic-tests.exe` (whole binary), exit 0:
  `[==========] 209 tests from 29 test suites ran.` / `[  PASSED  ] 209 tests.`
- E4: from `atx-impl/tools`, `"C:/Program Files/Python312/python.exe" -m pytest -q -p no:cacheprovider
  --basetemp=<scratch> test_composition_theme_hedge.py test_composition_mom_volman.py
  test_fit_composition_rule_plugins.py test_composition_theme_tsmom.py test_composition_two_speed.py
  test_composition_rules.py test_composition_theme_erc.py test_composition_resid.py test_composition_ic_shrink.py`,
  exit 0: `107 passed, 17 subtests passed in 67.48s`.
- E5: from `atx-impl/tools`, `... -m pytest -q -p no:cacheprovider --basetemp=<scratch> test_fit_composition_weights.py
  test_fit_composition_weights_pool.py test_fit_composition_weights_store.py`, exit 0: `141 passed in 191.90s`.
  - From the pool root, `... -m pytest -q -p no:cacheprovider scripts/tests/test_no_python_mirror.py
    scripts/tests/test_no_versioned_scripts.py`, exit 0: `11 passed in 6.19s`. It also printed `G-P5: 37 mirrored rule
    function(s) in 14 row(s) left for P9 lanes`.
- E6: `atx-equity-strategy-ic.exe --list-rules --json` (exit 0) parsed against
  `atx-impl/tests/fixtures/composition_rules_list.json`:
  - `whole document JSON-equal: True`;
  - ids `within-theme-v1, ew-theme-std-v1, ic-shrink-v1, ic-shrink-aim-v1, theme-erc-v1, theme-resid-v1,
    theme-tsmom-v1, mom-volman-v1, two-speed-v1, theme-cov-hedge-v1`;
  - `theme-cov-hedge-v1 hedge theme_hedge incompatible ['theme-resid-v1', 'two-speed-v1']`;
  - `mom-volman-v1 schedule theme_schedule incompatible ['theme-resid-v1', 'theme-tsmom-v1']`.
- E7: both templates resolve with `research_cycle.load_spec`:
  - `p9-theme-cov-hedge | nav.output build-equity/mega-nav-p9-theme-cov-hedge` and `p9-mom-volman | nav.output
    build-equity/mega-nav-p9-mom-volman`;
  - the fit flags are X-5's unchanged (`--composition ew-theme-std-v1 --theme-erc theme-erc-v1`): no rule flag comes
    from a template.
  - `research_cycle.py plan --lines-only` exits 3 in pool-19 only because X-5's role inputs are not in this tree
    (`input role missing`).
- E8: `wave_manifest.rule_cell_problems` + `registration_problems` give `[]` for both. Each `template_sha256` equals
  its committed file: theme-cov-hedge `095d2d7e...` and mom-volman `9991895b...`. The K-P9-11 keys in each fragment
  equal the file's.
- E9: the fitter's producer fingerprints, base vs head, are equal:
  - FACTOR `962cd507af511fe3`;
  - CONTEXT `70e1eda51539c353`;
  - AIM `f1fb0d3a5183a723`;
  - AIM_ERA `9a880f97b42fdae6`.

  So the fit work store is reused. Only `script_sha256` of the fitter changes.

## How root verifies

- Build (`research-build.ps1`, `equity-dev`): `atx-engine-combine-tests`, `atx-impl-strategy-ic-tests`,
  `atx-equity-strategy-ic` and `atx-impl-tests`. The lane did not build `atx-impl-tests`. Its glob compiles the two
  rule tests, both inside anonymous namespaces with unique suite names.
- Anchored gtests:
  - `atx-engine-combine-tests --gtest_filter=CharCovHedge.*`
  - `atx-impl-strategy-ic-tests
    --gtest_filter=CharCovHedge.*:ThemeHedgeRule.*:MomVolman.*:CompositionRules.*:StrategyIcComposition.*:ThemeTsmom*:TwoSpeed*:ThemeResid*:StrategyIcRunner.*`
  - `atx-impl-tests --gtest_filter=ThemeHedgeRule.*:MomVolman.*:CompositionRules.*`
- `atx-equity-strategy-ic --list-rules --json` must be JSON-equal to the fixture. Re-generate the fixture from the merged
  C++ after the P17 rebase; do not hand-merge it.
- Identity checks with both flags absent, on X-5 (`scripts/specs/v8/x-theme-erc.json`):
  - X-5's w argv re-run on the new exe: every file of its w output byte-identical.
  - X-5's fit argv re-run: `composition_weights.json` and `admission.json` equal except `script_sha256`. The producer
    fingerprints are unchanged (E9).
  - No substitution list is needed: nothing changes when neither block is present.
- The Q5 diagnostic (zero trials) before P9-H is registered: the steps in the spec file.

## Q5 (lit section 8 Q5) diagnostic, in short

The book is the NAV holdings of the parent P9-H will be registered on (nominal X-5). If the cell ran without
`--emit-holdings`, root re-runs it with that flag and checks every file byte-identical; that re-run is not ledgered.
The steps are `book_diagnostics.py book-csv`, then `risk --book-weights --emit-exposures all` (atx-risk-v1.1), then
`book_diagnostics.py run --only G-2a`, under E-18's caps.

Two figures are printed, both from G-2a's `style_share_of_summed_variance` (Euler contributions; per-style values are
printed beside them):

- **q (primary):** the share on the style columns no theme names: size, residual_vol and liquidity. The basis is each
  registry theme text.
- **q_lit (upper bound):** the lit's list, which adds beta and leverage. low_risk's QMJ safety names both.

The spec decides nothing. It prints the arithmetic bound SR factor <= 1 / sqrt(1 - q). Lit's +.05 to +.15 needs q of
about .05 to .15 removed at SR 1.77. The PM rules P9-H registration with q and q_lit printed in the prereg.

## P17 rebase map (for whoever rebases on D2)

D2's WIP (`d61f73c2`) rewrites `params_schema` to the K-P9-9 params form: registered constants, each optional, `const`,
integers typed. In that form the two AL-COMB rows become:

- `theme-cov-hedge-v1`:
  `{"type":"object","additionalProperties":false,"properties":{"lookback":{"type":"integer","const":252},"lag":{"type":"integer","const":2},"step":{"type":"integer","const":21},"min_pairs":{"type":"integer","const":126}}}`
- `mom-volman-v1`:
  `{"type":"object","additionalProperties":false,"properties":{"lookback":{"type":"integer","const":126},"lag":{"type":"integer","const":2},"step":{"type":"integer","const":21}}}`

`ThemeHedgeRule.RegisteredConstantsAndRow` reads `properties.<key>.const` for the four hedge constants, so it holds in
both forms. `MomVolman.RegisteredConstants` does not read the schema. The block content stays checked by each row's
parse. D2's `ParamsSchemaConstantsAreTheRegisteredOnes` will need the two rows added.

Re-apply on D2's form:

- the rows and `*_excludes` in `strategy_ic_rules.cpp`, plus `stage_name` (MR-3);
- the two declarations in `strategy_ic_rules.hpp`;
- the tsmom row's `.parse = &composition_schedule_rules`;
- `composition_schedule_rules` dispatches to D1's `composition_schedule` (in `strategy_ic_theme_tsmom.cpp`). Follow it
  if D2 renames or moves it.
- `strategy_ic_mom_volman.cpp` reads these `PinnedWeights` fields: `std_themes`, `std_theme_count`, `values`,
  `residualise`, `schedule`, `schedule_from`, `schedule_mass` and `schedule_off`. `strategy_ic_theme_hedge.cpp` reads
  `std_themes`, `residualise`, `sleeves`, `running_rules` and `CompositionRule::stage`.
- `composition_mom_volman.py` uses `composition_theme_tsmom.sleeves` and `.SERIES`. If D2 deletes the Python tsmom
  mirror (G-P5), re-point the sleeve source.
- The fixture and the `TableCoversEveryRule` lists.
- The fitter `PARENT_CHECKED_MODIFIERS` tail and the plugin pins.
- The CMake tails: keep both sides.

## Decisions

1. **Rule files go in `scripts/specs/p9/rules/`, not `candidates/`.** `wave_queue.py` validates every file in a
   candidates directory as `atx.wave-candidate/v1` (dsl, theme and tier required), and a rule has none of them.
2. **The templates follow E2 (Ruling ALCOMB-1).** They have no `rules` key and no `change.flags`. The flag comes from
   `COMPOSITION_FLAGS` (MR-4). Each manifest fragment uses `params: {}`: in D2's form, an absent key means the
   registered value, and the cell restates no constant.
3. **mom-volman's `source_sample_end` is 2016 (Cederburg et al.), not 2011 (BSC).** The registered rule is Cederburg's
   real-time form, and 2016 is the last published sample that tested it. BSC is recorded in `source_sample_basis`.
4. **Q5 column classification.** It is read from the registry's theme texts at the base, never from data:
   - beta and leverage count as named, by low_risk's QMJ safety;
   - residual_vol counts as unnamed: low_risk's scaled MAX is per unit of volatility, not a volatility level;
   - liquidity counts as unnamed: price_volume's dollar-liquidity term is a correlation structure, not a level.
5. **History was not rewritten.** The WIP and handoff SHAs stay valid recovery points. The beyond-three edits are
   isolated by listing them as MR-1..MR-4, not by a re-split. Every MR edit sits in its own hunks, and none is mixed
   with a P17 edit except `stage_name` (MR-3) in `strategy_ic_rules.cpp`.
6. **The decisions carried from the handoff are unchanged:**
   - the hedge is a new last stage `IcStageKind::hedge` and is a no-op before decision 253;
   - the hedge excludes theme-resid and two-speed, and stacks with either schedule;
   - volman shares tsmom's block and parse, so they exclude each other;
   - v8 refusal texts are verbatim;
   - the fitter for the hedge writes only the constants block.

## Concerns

1. **ALCOMB-2.** MR-1..MR-3 are cross-lane edits beyond P17's three, already on the branch. The hedge rule needs them.
   Root accepts them or rules a revert: revert MR-1..MR-3 and the hedge row, and mom-volman stands alone.
2. **P17 rebase is not done.** D2 is unmerged WIP. Its `strategy_ic_rules.cpp`, runner, tsmom and fixture changes will
   conflict with this branch. Use the rebase map above, and regenerate the fixture from the merged C++.
3. **mom-volman has no cap.** This is a registered, stated choice (BSC scale without one). A block whose 126-decision
   sigma is small scales the momentum mass up without bound; sigma 0 is refused. Gross matching (PM6-6) holds the
   book's dollar gross, not the momentum share.
4. **mom-volman's schedule is fixed on the fit's TRAIN sleeve.** It is recorded by the fitter, as theme-tsmom-v1's is.
   On a role outside those sessions (an OD-3 history read, P9-W), the blocks are not re-estimated: before the first
   recorded session the parent's masses hold, and after the last recorded block its masses hold.

   theme-cov-hedge-v1 has no such limit: it estimates from each role's own returns. This is a note for P9-W / OD-3.
5. **Template pins assume LF bytes.** `git add` warned "LF will be replaced by CRLF" (`core.autocrlf=true` at this base).
   The pins hold once E2's R0-7-EOL (`scripts/specs/** text eol=lf`, merge slot 1) is in. Root re-pins anyway when it
   sets `parent`.
6. **New schema literals (Ruling SQL2-CLS: root adds them to `classes.json`):** `atx.p9-rule-registration/v1` and
   `atx.p9-diagnostic-spec/v1`.

## Carried minors

- D1's two 13-theme pytests: not touched (no theme added). They break first with whichever lane adds a theme.
- The PM's DISK-2 ruling: after this report is committed, the lane deletes its own `build-equity` object tree by name
  and keeps the `mega-p9-alcomb-*` receipts and logs. It also deletes its scratch files and the two papers' cached PDFs.

## Ledger candidates

1. DMRS (RFS 2020) sample 1963/07-2017/06; Cederburg et al. (JFE 2020) samples end 2016:12; Barroso-Santa-Clara (JFE
   2015) 1927:03-2011:12. These are the K-P9-11 `source_sample_end` facts, read from the papers.
2. In a reused pool, `research-build.ps1` needs `atx-build.ps1 configure -Preset equity-dev` first (57 s). A cold
   `atx-engine-combine-tests` is then 556 s / 234 TUs, and a cold `atx-impl-strategy-ic-tests` 577 s / 105 TUs, at 4
   jobs. Incremental re-links take about 20 s.

## Combined round (ALCOMB-5)

| | |
|---|---|
| Outcome | DONE. Steps (a)-(d) done. Step (c) was first blocked (refused configure) and was finished under Ruling ALCOMB-8: build p9-alcomb-f exit 0, fixture regenerated by the generator, every gtest and pytest named below green |
| Merge commit | `aca3570decb43024b88bd605666378bfdff724b3` (`aca3570d`), parents `9692a73e` (the reviewed lane) and `49af0a52` (D2's approved head); `git merge --no-ff`, no rebase, no history rewrite (ALCOMB-5a) |
| Commits after the merge | `97a2d1c3` S-1..S-6 fixes, specs, this section (written BLOCKED, C++ then unbuilt); `6ec102fb` fixture regenerated from the built exe (alone); then the commit that carries this update (ALCOMB-6 / ALCOMB-7 template and registration lines, the step (c) evidence) |
| Builds | First attempt: `powershell -NoProfile -ExecutionPolicy Bypass -File scripts\atx-build.ps1 configure -Preset equity-dev` was refused by the session's permission system (not retried). Under ALCOMB-8, one try of the documented form from pool-19's root, `powershell scripts\atx-build.ps1 configure -Preset equity-dev`: permitted, "Configuring done (22.9s)", engine_git_sha `97a2d1c3`. Then `powershell scripts\research-build.ps1 -Tag p9-alcomb-f -Targets "atx-engine-combine-tests,atx-impl-strategy-ic-tests,atx-equity-strategy-ic" -Preset equity-dev`: exit 0, 344 TUs, 13 links, 639 s, Jobs 3, Source `97a2d1c3`, DirtyEntries 0; exe SHA-256 combine-tests `61852965...`, strategy-ic-tests `6f89f2ac...`, atx-equity-strategy-ic `39423f90...` (receipt `build-equity/mega-p9-alcomb-f-receipt.json`). Warnings: only the pre-existing `/MP` unused flag and databento's `getenv`; none from a lane TU |
| Trials | 0. No 2020-2023 return / IC / NAV output was read; the trial ledger was not touched |

### The refused configure and ALCOMB-8

The permission system refused the first configure form (and, earlier, a `git checkout --` that would have split the worktree edits; the split was done instead by staging only the merge's files). Nothing was retried around either refusal. Ruling ALCOMB-8 resumed the lane with one try of the documented form; it was permitted, and the resume list of the BLOCKED version of this section was then run in order: configure, build `p9-alcomb-f`, fixture regeneration (committed alone, `6ec102fb`), anchored then whole gtests, pytests. Not built by the lane: `atx-impl-tests` (the glob of every atx-impl test TU; root builds it at merge, as for D1 / D2).

### Step (a): the merge and its conflicts

Three files conflicted; each was resolved by the report's P17 map.

| file | resolution |
|---|---|
| `atx-impl/src/strategy_ic_rules.cpp` | D2's `*_params_schema` literals kept. D1's block-schema literals (and AL-COMB's `volman_schema` / `hedge_schema`) dropped, as D2 dropped them. AL-COMB's `*_excludes` arrays replace D2's `resid_excludes{2}` / `resid_ids` (the new rows add pairs). Two new literals in D2's form: `volman_params_schema` = `{rule, lookback 126, lag 2, step 21}`, `hedge_params_schema` = `{rule, lookback 252, lag 2, step 21, min_pairs 126}`, each a typed `const`, none required, `additionalProperties: false`. Rows wrapped to stay within 100 columns |
| `atx-impl/tools/fit_composition_weights.py` | D2's `compose(engine)` kept. The `--mom-volman` and `--theme-hedge` attach blocks move into it after `--two-speed`, before `composition_resid.apply` (the same order as before). Imports: both sides |
| `atx-impl/CMakeLists.txt` | both tails kept (AL-COMB's two sources, D2's `target_link_libraries(... atx-engine-research-composition)`) |

Auto-merged and checked by reading: `strategy_ic_rules.hpp` (the two declarations), `strategy_ic_runner.cpp`
(MR-2's hook beside D2's runner edits), `strategy_ic_rules_test.cpp` (the extended `TableCoversEveryRule` lists),
`test_fit_composition_rule_plugins.py`. D2 kept `ic_detail::composition_schedule` in `strategy_ic_theme_tsmom.cpp` and
kept `composition_theme_tsmom.sleeves` / `.SERIES` in Python, so the volman dispatcher and the sleeve source are
unchanged.

Adaptations to D2's shape, in the merge commit:

- `ParamsSchemaConstantsAreTheRegisteredOnes` gains the two rows (`st::mom_volman_{lookback,lag,step}`,
  `st::theme_hedge_{lookback,lag,step,min_pairs}`) and the two includes. No existing row changed.
- `params_schema` keys are the numeric registered constants. `mom-volman-v1`'s scaled theme (`price_momentum`) is not a
  params key: it is fixed by the rule id, and the row's parse checks it in the block. That parse also checks
  `sessions` / `sleeve` (the old block schema's array bounds are not in the params form, as for D2's tsmom row).
- CM-5 / D2-2: `composition_mom_volman` writes `provenance.mom_volman.producer_fingerprint` =
  `composition_rules.module_fingerprint(__file__, PRODUCERS)`, `PRODUCERS = ("schedule", "attach")`, in place of the
  whole-file `module_sha256`. The closure reaches `composition_theme_tsmom` (a `LOCAL_MODULES` member), so a sleeve
  edit moves it. `theme-cov-hedge-v1` writes no provenance block.
- `--engine-fit` (D2-1): `composition_mom_volman.check_args` refuses it with `ENGINE_FIT_REFUSED`. The C++ fit
  (`atx-research-composition fit`) records no momentum sleeve, so the sleeve would be a Python number in a file whose
  `provenance.engine_fit.written` says every fitted number is C++'s. `--theme-hedge` is accepted under `--engine-fit`:
  its block is the registered constants only.
- Pytests re-pinned to D2's params form (the old block-schema assertions cannot hold after D1-PSCHEMA):
  `test_composition_mom_volman.py` / `test_composition_theme_hedge.py` `test_block_is_the_rule_tables_row`, and the
  `module_sha256` -> `producer_fingerprint` provenance assertion.

The fit path: D2 moved no AL-COMB code. The hedge has nothing to fit. mom-volman's fit is tsmom's step 1 (the
sleeve), which D2's C++ computes inside `theme_tsmom_schedule` but does not output; hence the `--engine-fit` refusal
(kept by Ruling ALCOMB-7; D3 adds the sleeve to the C++ fit).

### Fixture

- `atx-impl/tests/fixtures/composition_rules_list.json` is the only K-P9-6 fixture this lane pins. In the merge (`aca3570d`) and in `97a2d1c3` it was git's text merge of the two sides, left as is (no build yet; never edited by hand).
- `6ec102fb` regenerates it with the generator: `build-equity/bin/atx-equity-strategy-ic.exe --list-rules --json > atx-impl/tests/fixtures/composition_rules_list.json` (build p9-alcomb-f, exit 0).
- What changed, as predicted: only the `params_schema` of `mom-volman-v1` and `theme-cov-hedge-v1`, from the old block-schema form (`required`; `theme` / `sessions` / `sleeve` for volman; untyped consts) to D2's K-P9-9 form (rule key plus the registered constants, each `const` with its `type`, none required). 18 insertions, 44 deletions; no other row, no `recipe_sha256` and no `incompatible` list moved.
- Why: the merge gave the two rows D2's params form in C++ (`volman_params_schema`, `hedge_params_schema`); the fixture is that table printed.
- No other fixture changed (`atx-engine/tests/fixtures/research_composition/fit_v1.json` is D2's, untouched).

### Step (b): the Suggested items

| # | what changed | where | test | evidence |
|---|---|---|---|---|
| S-1 | `mom_volman_blocks` refuses a block whose `target` or `scale = target / sigma` is not finite, naming the rule and the decision ("mom-volman-v1: the scale target / sigma before decision J must be finite (no cap is registered)"), before the composition's mass check | `strategy_ic_mom_volman.cpp` `mom_volman_blocks`; header comment | new `MomVolman.TinyVolatilityGivesAFiniteScaleAndOverflowIsRefused`: 1e-2 then 1e-120 gives 8 finite blocks, block 253's sigma about 1e-120 and scale in (1e117, 1e119), and the parse schedules W x scale (finite). 1e153 then 3e-162 (positive sigma about 3e-162 under a target about 1e152) is refused at decision 253 with that text, and the same sleeve cut at 253 decisions is accepted | gtest MomVolman 6/6 (p9-alcomb-f) |
| S-2 | documented, not changed (ALCOMB-5): h_t unbounded, `R_t - h_t H_t` not re-standardised (equal dispersion lost for a hedged theme), all-tied date adds `-h_t H_t`; visible through S-3's trace | `scripts/specs/p9/rules/theme-cov-hedge-v1.json` `fixed_choices` + `history` | n/a | `wave_manifest` checks below |
| S-3 | the IC exe prints, in each scored role's `summary.json` entry, `composition_hedge_trace` = `{rule, themes (by composition index), semantics, blocks: [{decision_index, session_ns, h: [per theme]}]}` and `composition_schedule_trace` = `{rule, theme, semantics, blocks: [{from_session, sigma, target, scale}]}`. Both are C++ values. h is the kernel's ratio in force at each block start 253 + 21 b (`add_theme_hedged` optional `block` / `ratio` outputs; `IcCompositionResult::hedge_block` / `hedge_ratio`). The volman values are the parse's `mom_volman_blocks` values (`PinnedWeights::volman_*`). Python derives neither. The keys are absent unless the rule's block is present. `theme_hedge_working_bytes` gains `themes` and counts the trace (dates x 8 B plus blocks x (8 + 8 x themes) B); `theme_hedge_block_count` is new | `strategy_ic_theme_hedge.{hpp,cpp}`, `strategy_ic_mom_volman.cpp`, `strategy_ic_rules.hpp` (2 declarations), `strategy_ic_composition.{hpp,cpp}` (MR-1), `strategy_ic_runner.cpp` (MR-2), `strategy_ic_detail.hpp` (MR-5) | new `ThemeHedgeRule.TheTraceIsTheKernelRatioAtEachBlockStart` (bit-equal to the kernel's per-date ratio, held to the next block, blend bits unchanged, lone output refused unwritten, short role gives no block), `ThemeHedgeRule.SummaryTracePrintsTheHedgeRatiosPerBlock` (runner path, JSON round trip bit-equal, shape refusal Internal, flag absent adds nothing), `MomVolman.SummaryTracePrintsTheScalePerBlock` (bit-equal per block; no key without the block or under theme-tsmom-v1). `StageWiringAndRefusals`' byte formula extended by the trace (the model changed; the formula is restated term by term) | gtest ThemeHedgeRule 9/9, MomVolman 6/6 (p9-alcomb-f) |
| S-4 | the "no existing expected value changed" sentence corrected (TableCoversEveryRule 2U -> 4U; the three v8 fixture rows' `incompatible`) | this report, "Files changed", item 1 | n/a | n/a |
| S-5 | "with two names it always is" -> "two names with distinct ranks always are ...; two tied names leave R constant, so the residual is beta less its mean, which is spanned only when their betas are equal" | `char_cov_hedge.hpp` step 3 comment; `ClosedForm2Names` comment | comment only | CharCovHedge 5/5 |
| S-6 | both templates name exactly the fitter's `PARENT_RULES` (`--composition ew-theme-std-v1` or `ic-shrink-v1`, with the modifiers each accepts); `ic-shrink-aim-v1` is no longer offered. Requires-items updated for the merge | `scripts/specs/p9/templates/{theme-cov-hedge,mom-volman}.json` | new `test_composition_theme_hedge.py::DeclaredRule::test_parent_text_is_the_parents_the_fitter_accepts` | pytest green |

Template and registration pins: both templates changed, so both registrations' `manifest_fragment.rule_cell.template_sha256`
were recomputed from the committed LF bytes: theme-cov-hedge `7c6778bbd6c5759e3c431bae3cf301700240e672ea8406926b549927688dfb15`,
mom-volman `b060a6f3968f829b3b37050de71238dd333431726bef79b2722519810c6406ec`. Each registration gained a `history` row
("no constant, prediction or rule step changed; trials 0") and `fixed_choices` lines (hedge: S-2, S-3; volman: S-1,
ALCOMB-4's TRAIN-schedule limit, the `--engine-fit` refusal, S-3).

The mom-volman template prints sigma_b / target_b / scale_b per block. Its line "the sleeve's realised volatility and
Sharpe before and after scaling" is conditional on lane D3, in one sentence (Ruling ALCOMB-6): printed only if D3 has
made the IC exe write the sleeve's daily series before and after scaling and `atx-research-eval` prints them; PRE
round 2 cuts the sentence if D3 has not delivered. The registration's `requires` line says the same, its
`--engine-fit` line names D3 and the per-cell fit mode (ALCOMB-7), and a `history` row records both; the mom-volman
`template_sha256` is re-pinned to `7e9b26c1bc4a6453447ccbf9fee2612ecb50a293330f239cfc93d035e45db6fb` (theme-cov-hedge's
`7c6778bb...` is unchanged since `97a2d1c3`). No constant, prediction or rule step changed.

### Step (c): evidence

Logs are in `.superpowers/sdd/platform-p9-20261003/evidence-AL-COMB/` (git-ignored). C++ on build p9-alcomb-f (head C++ `97a2d1c3` = this head's C++).

- `r5-gtest-combine-CharCovHedge.log`: `atx-engine-combine-tests --gtest_filter=CharCovHedge.*`, exit 0, `5 tests` / `[  PASSED  ] 5 tests`. `r5-gtest-combine-full.log`: the whole binary, exit 0, `248 tests from 45 test suites`, all passed.
- `r5-gtest-strategy-ic-anchored.log`: `atx-impl-strategy-ic-tests --gtest_filter=CharCovHedge.*:ThemeHedgeRule.*:MomVolman.*:CompositionRules.*:StrategyIcComposition.*:ThemeTsmom*:TwoSpeed*:ThemeResid*:StrategyIcRunner.*:MarginalIc.PairCacheSourcesPinned`, exit 0, `111 tests from 12 test suites`, all passed: CharCovHedge 5, ThemeHedgeRule 9, MomVolman 6, CompositionRules 8 (incl. `ListRulesJsonPinned` on the regenerated fixture and `ParamsSchemaConstantsAreTheRegisteredOnes` with the two new rows), StrategyIcComposition 7, ThemeTsmom 7, ThemeTsmomRunner 2, TwoSpeedRunner 2, ThemeResid 9, ThemeResidRunner 4, StrategyIcRunner 51, MarginalIc 1 (3 pair sources). `r5-gtest-strategy-ic-full.log`: the whole binary, exit 0, `215 tests from 29 test suites`, all passed.
- `r5-pytest-composition-built.log`, from `atx-impl/tools`, `PYTHONDONTWRITEBYTECODE=1 python -m pytest -q -p no:cacheprovider --basetemp=<scratch>` over `test_composition_{theme_hedge,mom_volman,theme_tsmom,two_speed,rules,theme_erc,resid,ic_shrink,engine_fit,fit_fixture}.py` and `test_fit_composition_rule_plugins.py`: exit 0, `132 passed, 1 skipped, 36 subtests passed`. The two `DeclaredRule::test_block_is_the_rule_tables_row` that were red on the text-merged fixture (`r5-pytest-composition{,-head}.log`, `2 failed, 130 passed`) pass on the regenerated one. The skip is D2's `RealExecutables` (needs `atx-research-admission` / `atx-research-composition`, not in this lane's targets). `EngineFit.test_the_spec_block_rides_on_the_engine_document` and the volman `--engine-fit` refusal pass.
- `r5-pytest-fitter.log`: `test_fit_composition_weights{,_pool,_store}.py`, exit 0, `142 passed` (same Python as the head; the fixture and the specs are not read by them).
- `r5-pytest-guards-built.log`, from the pool root, a separate invocation: `scripts/tests/test_no_python_mirror.py scripts/tests/test_no_versioned_scripts.py`, exit 0, `11 passed`; `G-P5: 34 mirrored rule function(s) in 13 row(s)`, D2's count: AL-COMB adds no mirror.
- `wave_manifest.rule_cell_problems` + `registration_problems` give `[]` for both fragments after the ALCOMB-6 edit; each `template_sha256` equals its file's SHA-256; the K-P9-11 keys equal the registration's (E8). `research_cycle.load_spec` resolves both templates (E7).
- `ruff --select F` is clean on the changed Python. The three E501 lines in `test_composition_mom_volman.py` predate this round. No changed C++ line is over 100 columns.

### Flag-absent identity

- **Fitter.** Evidence: `r5-fitter-flag-absent-identity.json`. D2's head fitter (extracted from `49af0a52` into
  scratch) and the merged fitter ran on the same synthetic world (aim_world), with neither AL-COMB flag, for five
  cases: std, std+erc, std+erc+tsmom, ic-shrink, std+two-speed.
  - `admission.csv` is byte-identical in every case.
  - `admission.json` differs only in `inputs.producer_fingerprint`.
  - `composition_weights.json` differs only in `provenance.producer_fingerprint` and in `provenance.admission_sha256`
    (the SHA-256 of that admission.json).
  - Cause: CM-5's `OUTPUT_PRODUCERS = ("fit", "fit_prior")` closure gained the two flags (D2 `d0153b259e951b66` ->
    merged `d28a30895ad1393e`). Any modifier added to the fitter moves it.
  - The store-record fingerprints are unchanged (FACTOR `962cd507af511fe3`, CONTEXT `70e1eda51539c353`, AIM
    `f1fb0d3a5183a723`, AIM_ERA `9a880f97b42fdae6`), so the fit work store is reused. Every D2 composition module is
    byte-unchanged.
- **IC exe.** No X-5 run here: X-5's role inputs are not in pool-19 (the first round's E7: `research_cycle.py plan` exits 3, "input role missing"). On the built exe:
  - `ThemeHedgeRule.FlagAbsentIdentity` (no `theme_hedge` block: parse, running rows, stage list, stage bytes, recipe and composition bits equal the D1 runner's; the role is not read), `SummaryTracePrintsTheHedgeRatiosPerBlock` and `SummaryTracePrintsTheScalePerBlock` (no summary key without the block, nor under theme-tsmom-v1) and `StrategyIcRunner.*` 51/51 all pass;
  - `--list-rules --json` is the regenerated fixture (it is that output) and `ListRulesJsonPinned` passes.
  By construction:
  - the two summary keys are written only when `pinned.hedge_themes` is non-empty (set only by an accepted
    `theme_hedge` block) or `pinned.schedule == "mom-volman-v1"`;
  - the recipe, combined manifest, stage list and stage bytes are unchanged when neither block is present;
  - Root's X-5 w-argv byte-identity on the built exe is the check.

### Updated merge requests for root

| # | edit | flag-absent effect |
|---|---|---|
| MR-1 (extended) | as before, plus `IcCompositionResult::hedge_block` / `hedge_ratio`, `finish` passes them to `add_theme_hedged`, `theme_hedge_working_bytes(dates, names, themes)` at both call sites | none: both vectors stay empty; stage bytes unchanged without a hedge stage |
| MR-2 (extended) | as before, plus two calls after `planned_target_proxy` in `score_role`: `add_theme_hedge_trace(pinned, role.session_keys, *combined, result)`, `add_mom_volman_trace(pinned, result)` | none: both return before writing |
| MR-3 | `stage_name` `hedge` case (unchanged) | none |
| MR-4 | lane E2's `scripts/wave_kinds.py` `COMPOSITION_FLAGS`: `"theme-cov-hedge-v1": {"fit": {"--theme-hedge": "theme-cov-hedge-v1"}}`, `"mom-volman-v1": {"fit": {"--mom-volman": "mom-volman-v1"}}` (E2 is not in this tree) | none (new keys) |
| MR-5 (new) | D1's `strategy_ic_detail.hpp` `PinnedWeights`: `volman_sigma`, `volman_target`, `volman_scale`, `hedge_themes` (S-3) | none: empty unless the rule's block parses |

Root items:

- **Literals** for `classes.json` (SQL2-CLS): `atx.p9-rule-registration/v1`, `atx.p9-diagnostic-spec/v1`.
- **Template pins** assume LF bytes: they need E2's `scripts/specs/** text eol=lf`. Root re-pins when it sets
  `parent`.
- **Q5 diagnostic**: the zero-trial lit section-8 Q5 diagnostic (`scripts/specs/p9/diagnostics/al-comb-q5-style-variance.json`)
  runs between wave 2 and the P9 cells, before P9-H is registered.
- **X-5 fit-argv identity substitution list** after this merge: `inputs.producer_fingerprint`,
  `provenance.producer_fingerprint`, `provenance.admission_sha256`. These are added to D2's four CM-5 / D2-2 keys
  (evidence above).
- **The fixture** was regenerated by the lane (`6ec102fb`); after root's merge, `ListRulesJsonPinned` on root's exe is
  the check (regenerate again only if another lane moved the table).
- **Build** `atx-impl-tests` (never built by the lane) and run `ThemeHedgeRule.*:MomVolman.*:CompositionRules.*` there.
- **PRE round 2 (ALCOMB-6)**: keep the mom-volman template's conditional sentence on the sleeve's vol / Sharpe before
  and after scaling only if D3 delivered the C++ producer (the IC exe writes the sleeve's daily series, `atx-research-eval`
  prints them); else cut it before registration (re-pin `template_sha256`). No Python derivation.
- **PRE round 2 (ALCOMB-7)**: the fit mode of every P9 cell is the fit mode of its P9-B0 parent reference: `--engine-fit`
  only if D3 delivered the C++ sleeve and the wave-3 P9-B0 re-based the parent under engine-fit, else flag-absent.
  `--engine-fit --mom-volman` stays refused until D3 adds the sleeve to the C++ fit.

### Open questions (PM)

Answered by Rulings ALCOMB-6 (the sleeve vol / Sharpe line is conditional on D3; PRE round 2 cuts it otherwise), ALCOMB-7 (the `--engine-fit --mom-volman` refusal stays; D3 adds the sleeve; per-cell fit mode = the P9-B0 parent's) and ALCOMB-8 (the documented configure form; permitted). None open.

### Disk

- After the commit carrying this update, the lane deletes by name its `build-equity` object tree (the directories `atx-core`, `atx-engine`, `atx-impl`, `atx-tsdb`, `bin`, `lib`, `tests`, `CMakeFiles` and the generated top-level files) and keeps the `mega-p9-alcomb-{a,b,d,e,f}-*` receipts / logs and the two pre-session directories (`audits/`, `v8-interim3-pitch-render-run2/`).
- Scratch deleted by name: the `alcomb_*.py` scripts, the `alcomb-*` copies, D2's extracted tree, the identity outputs and the pytest basetemps.
