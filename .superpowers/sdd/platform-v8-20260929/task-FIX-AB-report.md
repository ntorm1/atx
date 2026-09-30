# Lane FIX-AB report: Wave 1 review fixes, areas A and B

Worktree `C:/atx-wt/pool-7`, branch `feat/platform-v8-fixab-20260930` from `81abfa12`. One commit per finding, in the
brief's order. Nothing was built and no C++ test was run (lane rule). Python: pytest was run on the touched files.

| finding | commit | verdict before the fix |
|---|---|---|
| B-2 | `c7c16599` | CONFIRMED, partly (see below) |
| B-3 | `dd9cbcf2` | (not marked unverified) |
| B-4 | `931c655a` | CONFIRMED |
| A-2 | `f1a928f9` | part 1 CONFIRMED; part 2 NOT A DEFECT on the stores the risk verb writes (fixed anyway, as the brief requires) |
| A-3 | `8e5ea802` | (not marked unverified; the review's "unverified" part is data, not code) |
| A-4 | `98b0a0e8` | (not marked unverified) |
| A-1 | `f425e4ec` | (not marked unverified) |
| B-1 | `b900b387` | (not marked unverified) |

## Verifications

**B-2: CONFIRMED, partly.**
- What is confirmed:
  - At base, `strategy_marginal_ic.cpp:194-197` read only `theme_redistribution.themes`.
  - The theme regressor was the plain sum `w*s*r` (`stream_rows`, `:453-460`).
  - The R-1 blend re-ranks each theme's sum over the names that have a present member (`strategy_ic_composition.cpp:328-330`, `composition_rules.py:221,249`).
  - So under ew-theme-std-v1 the marginal statistic was not the one computed inside the theme.
- What does not occur for generated libraries: the grouping mismatch the review describes.
  - `generate_library.py:286-287` writes `theme == family ==` the registry theme.
  - `fit_composition_weights.py:469-478` takes the weights' themes from the same rows.
- So the real defect is the missing re-rank and the ignored `theme_standardise` block.

**B-4: CONFIRMED.**
- Where the limits are:
  - The 1 MiB limit is `strategy_ic_library.cpp:44` (`metadata_text`). The fields manifest reaches it through `pinned_json` at `strategy_ic_admission.cpp:326`.
  - The 1,024-row cap is `strategy_ic_detail.hpp:56`.
- The builder writes `json.dumps(sort_keys=True, indent=2) + "\n"` (`prepare_research_fields.py:620-622`).
- Measured sizes:
  - fields-v9 (TRAIN 2020-2022, lo1) is 483,031 B for 63 rows: 7.7 KB per row on average, 12.3 KB at the widest (`sv_ratio126`).
- **A 73-row manifest as the builder writes it** (v9 plus its 10 widest rows again) **is 605,601 B: 57.8% of the old 1,048,576 B limit.**
- Other sizes at the widest row width:

  | rows | bytes | against 1 MiB |
  |---|---|---|
  | 73 | 927,292 | 88.4% |
  | 82 | about 1 MiB | the most the old limit held |
  | 100 | 1,258,447 | refused before this fix |
  | 1,024 | 12,591,307 | the row cap could never be reached |

- The new bound is 16 MiB (16,777,216 B), the risk verb's manifest bound.
- The review's specific failure at 70-75 rows does not reproduce at v9 row widths. The limit was still wrong, because the row cap could never be reached.

**A-2 part 1: CONFIRMED.** Calibration ran at the first `plan` call:
- `strategy_spo.cpp:1239` for v1/v2 (`calibrate`).
- `:1459` for v3 (`calibrate_tracking` in `plan_tracking`).
- Under a warm start that call is row `score_begin - K`, so `calibration.session` fell before the scored window.

**A-2 part 2 (spo-v3 stops for lack of risk rows): NOT A DEFECT on the stores the risk verb writes.**
- The verb writes every role row and sets forecast = 1 once specific forecasts exist (`strategy_risk_model.cpp:1041-1059`). That needs `structural_history` 252 (`:978`).
- The `v7-w1-risk-all` manifest has forecasts on 840 of 1,155 dates. The first is 2019-09-03 (row 315), and they are contiguous from there.
- The TRAIN role `recent-fast-train-2020-2022-v2-lo1` has `score_begin` 399. With K = 60, warm-up rows 339-398 are all forecast.
- It is fixed anyway: the brief requires that spo-v3 does not stop on such a store. The fix below covers that by construction.

## A-2: the choice

I chose to calibrate gamma on the first scored decision and to run the warm-up decisions as aim-partial-v5, reading no risk row:
- **It makes the declared text true without new wording.** The registration and the recipe say gamma comes from "the first decision". A reader who recomputes `sigma_aim` at `score_begin` now gets the published gamma, and no flag-off byte changes.
- **The alternative keeps the defect and needs a fallback.** Calibrating the warm-up from the risk rows that exist leaves `calibration.session` before the scored window. It would also need a second rule for a store with no early rows, which is exactly the case the brief says must not stop.
- **The spo cell starts from its parent's book.** aim-partial-v5 is the rule every spo rule is rewritten to on the command line, and the shadow book's rule. The spo cell therefore starts its scored window from the same book as its aim-partial-v5 warm-start parent, bit for bit. The test asserts this for row `score_begin`'s book, its EXECUTE and its gross.
- **The cost is confined to unscored rows.** The warm-up book is not an spo steady state. It is the accepted rule's steady state, and only rows the run does not score are affected.

## What was built, per finding, and how root verifies it

Test targets:
- `atx-impl-strategy-ic-tests`
- `atx-impl-strategy-tests` (it compiles `atx-engine/tests/data/strategy_data_test.cpp`; `atx-engine-data-tests` has it too)
- `atx-impl-strategy-target-tests`

### B-2 `c7c16599` (marginal IC themes from the weights block)
- **Theme reader.** New public `ic_weights_themes(text)` (`strategy_ic_runner.hpp`). It uses the runner's own block checks, which are factored out in `strategy_ic_admission.cpp` with the same refusal texts: exclusivity of the two blocks, block shape, theme names.
- **Marginal verb.**
  - It reads `theme_standardise` or `theme_redistribution` through that reader.
  - A weighted member missing from the block is refused, as the runner refuses it.
  - Under `rerank` it re-ranks each theme composite inside the theme with the composition's own kernels (`group_rerank.hpp`), so the marginal statistic is computed inside the theme.
  - The method text and `inputs.themes` (`grouping`, `rerank`) change only under `theme_standardise`.
- **Verify:** `atx-impl-strategy-ic-tests --gtest_filter=MarginalIc.*:CompositionV8.*`. The new test is `MarginalIc.StandardisedThemesComeFromTheWeightsBlock`: two themes, rerank and plain.
- **Identity:** a weights file without `theme_standardise` takes the unchanged path. The earlier `MarginalIc` tests are unchanged apart from the fixture refactor.

### B-3 `dd9cbcf2` (delisting-returns role refused as a signal role)
- **Engine functions** (`atx/engine/data/strategy_data.hpp`):
  - `role_delisting_returns_applied(text)`.
  - `refuse_delisting_returns_signal_role(text, path)`. The message names the manifest path, the flag `--delisting-returns` and the field `universe.delisting.returns_applied`.
- **Callers:** the IC runner's `admit()` (before any payload or output) and the marginal verb (right after the pool is read).
- **For R45:** the refusal is a free function R45 can call; its label-role path is not touched.
- **Pin:** `dsl_vm_sources_sha256` is repinned to `ad6c4ca7…` because `strategy_data.hpp/.cpp` are pinned sources. There is no semantics bump: the VM is unchanged, so candidate-cache identities are unchanged.
- **Verify:**
  - `atx-impl-strategy-tests --gtest_filter=StrategyResearchRole.*`
  - `atx-impl-strategy-ic-tests --gtest_filter=StrategyIcRunner.DelistingReturnsRoleIsRefusedBeforeAnyPayloadOrOutput:StrategyIcRunner.VmSourcesPinnedToSemanticsVersion:MarginalIc.DelistingReturnsRoleIsRefused`
- **Identity:** a role without `universe.delisting`, or with `returns_applied` false, is admitted as before.

### B-4 `931c655a` (fields-manifest byte bound)
- **The constant:** `ic_fields_manifest_max_bytes = 16 MiB` (`strategy_ic_runner.hpp`), with the sizes above in its comment.
- **Where it is used:**
  - IC admission (`bind_fields`)
  - the legacy fields manifests of the signal cache
  - the marginal verb's field manifest
- Every other metadata file keeps 1 MiB. The 1 MiB message text is unchanged, and the refusal names both bounds.
- **Verify:** `atx-impl-strategy-ic-tests --gtest_filter=FieldCaps.*:StrategyIcRunner.*`. The new test is `FieldCaps.AdmitsA100RowManifestOfPublishedRowWidth`:
  - 100 rows of 12,300-character definitions, more than 1 MiB, plan OK.
  - One more row of 16 MiB is refused, and the refusal names both bounds.
- **Identity:** every manifest under 1 MiB (all published ones) behaves exactly as before.

### A-2 `f1a928f9` (spo gamma on the first scored decision)
- **Engine:** `Engine::Impl::plan` sends `d < decision_begin` to `warm_up_step`: aim-partial-v5's move of the book and of the shadow book, with no risk read, solve, calibration or row.
- **Calibration blocks:** `Calibration::warm_up` adds a `"warm_up"` entry to both calibration blocks, and only when a warm-up ran.
- **Verify:** `atx-impl-strategy-target-tests --gtest_filter=SpoV3.*:SpoPin.*:NavV7Hook.SideFilesExcludeWarmUp:SpoHook.*`. The new test is `SpoV3.WarmStartCalibratesOnTheFirstScoredDecision`. On a store with no forecast before `decision_begin` the warm start runs, and:
  - gamma is bit-equal to the flat start's at `decision_begin`
  - the rows are the scored ones only
  - the scored window starts from the plain aim-partial-v5 warm start's book, bit for bit
- **Identity:**
  - Without a warm start every `d >= decision_begin`, so every byte is unchanged.
  - `SpoPin.*` and `SpoV3.V1AndV2DigestsUnchanged` hold the spo-v1/v2 digests.

### A-3 `8e5ea802` (inert warm start refused; score_begin row and gross recorded)
- **Refusal.**
  - Condition: right after row `score_begin` is closed, every book has gross leverage 0 after that row's EXECUTE.
  - Result: InvalidArgument whose message names:
    - `--warm-start-sessions K`
    - the warm-up role rows
    - the first scored session (date, `session_ns`, role row `= score_begin`)
  - It uses the post-EXECUTE gross, so K = 1 (whose only fills land at `score_begin`) is not refused automatically.
- **summary.json:** `warm_start` adds `first_decision_row`, `score_begin_row` and `score_begin_gross_leverage`: per book label, the daily CSV's `gross_leverage` of row `score_begin`.
- **Holdings manifest:** with `--emit-holdings`, `manifest.json` adds `warm_start {warm_start_sessions, score_begin_row}`.
- **Recipe:** `recipe.json` already records `warm_start_sessions` and is unchanged. The live deploy pin recomputes the recipe, and the role row is bound by `role_sha256`.
- **Verify:** `atx-impl-strategy-target-tests --gtest_filter=NavWarmStart.*:StrategyLive.RecipePinBackwardCompatibleWithNewConstructionFields`.
  - New test `NavWarmStart.WarmStartThatBuildsNoBookIsRefused`.
  - Extended test `NavWarmStart.RefusesMoreThanTheRolesHistoryAndRecordsTheWarmStart`: the rows are recorded, and each book's gross equals the CSV.
- **Identity:** without a warm start there is no boundary, so nothing is checked or written. `NavWarmStart.FlagOffIsByteIdentical` pins the recipe SHAs `73cb45f1…` and `d53f0c09…`.

### A-4 `98b0a0e8` (E-14 criterion on the traded book)
- **New value:** `TrackingRow::aim_correlation_traded`. It is the Pearson correlation of the holdings the book's DECIDE read at `d` (fills, caps, blocks and drift of earlier decisions; nonmember exits and unpriced members included) with the aim, over every name either holds. It is NaN from a flat book.
- **Outputs:**
  - `spo_diagnostics.csv` adds the column after `aim_correlation`, giving 42 columns.
  - The per-book report (summary and tripwire `report_only`) adds `aim_correlation_traded` (mean/min) and `aim_correlation_criterion`: it reads `aim_correlation_traded.mean` against `v3_aim_correlation_min = .9`, with `value` and `met`.
  - The plan's `aim_correlation` is kept unchanged.
- **Verify:** `atx-impl-strategy-target-tests --gtest_filter=SpoV3.CriterionReadsTheTradedBook:SpoV3.ReportsTrackingErrorAndShareAtTradeLimit`. The new test recomputes the traded correlation from the holdings stream's held weights and the tracker's aim, at every decision.
- **Identity:** spo-v3 only. spo-v1/v2 rows and all flag-off outputs are untouched.

### A-1 `f425e4ec` (grid ADV cap at each variant's own leverage)
- **How it works:** with `adv_hold_q > 0`, `replay_books` forms lockstep groups by distinct `--aim-leverage` (in grid order) and runs one `run_books` per group on the same pool. Each group's shared construction is capped at its own leverage. The variants' results are reassembled in grid order.
- **Other outputs:**
  - `grid_manifest.json` gets `leverage_groups` (rule and ids) only when there is more than one group.
  - The stage timers' exposures seconds now accumulate across groups; with one call this is the same as before.
- **Verify:** `atx-impl-strategy-target-tests --gtest_filter=AdvHold.GridCapsEachVariantAtItsOwnLeverage:ConstructionGrid.*:HoldBand.GridSharesTheBandAndOneCadence:NavTimers.*`.
  - API test: variants at L 1.0 / 1.5 / 1.0 each equal their standalone replay, bit for bit, and the caps differ.
  - CLI test `ConstructionGrid.AdvCapGroupsVariantsByLeverage`: each `<id>/` is byte for byte its standalone run, and the manifest's groups are `[["l15","l15-t25"],["l10"]]`.
  - `ConstructionGrid.EachVariantEqualsItsStandaloneRun` now also asserts that there is no `leverage_groups` key without the cap.
- **Identity:** without the cap, or with a single leverage, the old single-lockstep code path runs unchanged.

### B-1 `b900b387` (price-field reuse pins the session calendar)
- **The pin:** `research_fields_price.session_calendar()` gives `{rule nyse-rule-v1, first 1970-01-01, last = the day before the seal (research_window), sessions, sha256}`. The SHA-256 is over the rule id and the sessions' epoch days (int64 LE).
- **Which fields:** those on the extended axis (`CALENDAR_GROUPS`: `ret_overnight`, `ret_intraday`, `ceq_iss_5y`, `coskew_60m`). Their entries record it, and `reuse_inputs` / `entry_inputs` compare it. `vol_126` and `xrd0_ttm` read role rows only and pin nothing.
- **Payloads:** unchanged.
- **Verify:** `"C:/Program Files/Python312/python.exe" -m pytest -q -p no:cacheprovider test_research_fields_price.py` in `atx-engine/tools`. New test `test_reuse_pins_the_session_calendar`:
  - One closure added to `research_fields_sec.NYSE_SPECIAL_CLOSURES` before the role recomputes the four fields ("inputs differ").
  - `vol_126` is still copied.
  - The unchanged calendar copies every field.
- **Identity:** a build without price fields is unchanged. With price fields, only the manifest entries gain `session_calendar`.

## pytest

| files | result |
|---|---|
| `test_research_fields_price.py` | 9 passed |
| `test_prepare_research_fields_module_reuse.py`, `test_research_fields_v8.py`, `test_research_fields_v8_quarters.py` | 27 passed |

## Deviations

- **A-3, which file is "the NAV manifest".** The brief says "the NAV manifest records `warm_start_sessions` and the row index of `score_begin`".
  - `recipe.json` already records `warm_start_sessions`. Adding the row there would change a warm-start recipe hash that the live deploy pin recomputes without the role, so it is not changed.
  - The row index went into `summary.json`'s `warm_start`.
  - With `--emit-holdings`, both values also went into the holdings `manifest.json`.
- **A-3, the refusal reads the gross after row `score_begin`'s EXECUTE,** not the pre-mark gross the review suggested. The pre-mark gross is always 0 for K = 1.
- **A-3, one existing test changed its fixture.** `StrategyLive.RecipePinBackwardCompatibleWithNewConstructionFields` used an inert warm start: price-risk-v1 exposures need 126 return pairs, and the role scored from row 20. It now uses `PinBench(140)` (a new optional score_begin; the default stays 20).
- **A-4, the timing of "the traded book".** It is the book at DECIDE of `d`, the result of earlier decisions, against the aim at `d`, as the review suggests. It is not the fills of `d`'s own plan, which would need a one-decision look-ahead. Both values are written, and the criterion reads the traded one.
- **A-1, grouping instead of refusal.** The brief asks that each variant use its own leverage. The review's smallest fix, refusing a grid whose leverages differ, would not satisfy that.
- **B-1, a data digest over a role-independent span, not the role's own extended axis.** The reuse interface (`reuse_inputs(name, options)`) receives no role. The role's sessions are already bound by `load_prior`.
  - Consequence 1: the first `--reuse` after this change recomputes those four fields of an older prior once, because its entries have no pin.
  - Consequence 2: re-dating the seal recomputes them as well.
- **Local history rewrite.** The A-2 test (the parent-book comparison) and the A-3 fixture change were folded into their finding commits. The rewrite used a branch and cherry-picks, with no interactive rebase. The branch was never pushed.

## Cross-lane edits

- None of FIX-C's files were touched.
- `atx-impl/tests/strategy_live_test.cpp` was edited: the A-1 test, and the A-3 `PinBench` score_begin parameter. It is a gtest file of the owned sources.
- The engine files `atx-engine/include/atx/engine/data/strategy_data.hpp` and `src/data/strategy_data.cpp` were edited for B-3, the signal role loader the finding names.

## Open risks

- **Nothing is compiled.** It was written for clang-cl `/W4 /WX` and read twice, but root must build the three targets above.
- **Merge with R45 (E-25).** Expect conflicts in `strategy_nav_replay.cpp` (A-1, A-3) and possibly `strategy_target_replay.cpp`. The B-3 refusal function is ready for R45's label-role path to call.
- **B-3 coverage.** The refusal is not added to the legacy `strategy_runner.cpp` or to the Python builders.
- **B-4 memory.** A 16 MiB fields manifest is parsed inside the IC/marginal fixed slack. The admission reserve does not charge the parse (well below 1 MiB today).
- **A-3 partial output.** An inert warm start is refused only after the warm-up and row `score_begin` have run. With `--emit-holdings`, the holdings directory is left partial, as for any replay error.
- **A-3 blocks some warm starts by design.** It now refuses any warm start whose rows cannot trade before `score_begin`, for example price-risk exposures that are not yet available.
- **A-2 changes warm-start spo outputs.** Warm-start spo-v1/v2/v3 runs now have a different warm-up book and gamma than before. No accepted spo cell ran with a warm start.
- **A-4 changes spo-v3 output shape.** The spo-v3 CSV and JSON shapes changed. No accepted spo-v3 cell exists, and no Python reads them.
- **B-1 does not cover other first-party imports.** The review's general note stands: `code_fingerprint` is blind to first-party imports. Only the price module's calendar is pinned. `research_fields_v8.py` imports only `_Host`, which carries no values.

## Minor findings left untouched (not inside an edited line)

- A-5: the admission budget grew with the flags off (ring, holdings row, band 0).
- A-6: warm-up events count against the event cap.
- A-7: the objective is reported with the unclipped F.
- A-8: a tiny specific variance slows convergence.
- A-9: the gross bound voids instead of capping.
- A-10: the exposures verb leaves a partial output directory.
- B-5 to B-9 (see `review-w1-B.md`).
- B-10: the pytest seal binding.
- B-11: the seal value is in no producer fingerprint.
  - Incidentally, B-1's calendar pin now includes the seal for the four extended-axis price fields.
- B-12: the price panel extent depends on which fields are requested together.
