# Lane FIX-2 report: Wave 1 review fixes P-2, P-3, N-1, N-2, N-3, T-1, T-2, T-4

**Status: DONE_WITH_CONCERNS.** Every finding in the brief has a commit. The C++ changes (N-2, T-1, T-4) were written to compile under /W4 /WX but were not built in this lane; root must build and run them. The remaining concerns are listed under "Open risks".

- Branch: `feat/platform-v8-fix2-20260930`, from `b44774d6`.
- Worktree: `C:/atx-wt/pool-7`.
- Head before this report: `edcb597b`.
- Not touched:
  - `scripts/research_cycle.py`
  - `backtest_integrity.py`
  - the era pooling modules
  - anything under `atx-db/`
- No real data was opened. Every test ran on a synthetic fixture.

## Commits (one per finding, in brief order)

| finding | commit | what |
|---|---|---|
| N-2 | `28051c4c` | spo-v3 runs `--capacity-curve` as a report-only pass. Each capacity book is the NAV-multiple tracker on a separate engine, and the main pass is unchanged. `--trade-fraction` (theta) has no effect on spo-v3 and R-9 is undefined on it (Ruling E-37). |
| P-2 | `6471c38f` | The pitch config and scorecard carry every registered criterion part: R-5's "S3 not lower" and R-6's E-14, E-31 and tripwire parts are read from the cell's NAV `summary.json`. R-7's marginal IC is report-only (Ruling E-36). |
| P-3 | `6a90caae` | The ladder refuses a recorded verdict that the item-5 rule contradicts. It also refuses a parent that is rejected, a defect, or not the last accepted cell before the child. |
| N-3 | `4e834a9c`, `c314a12b` | The K6 marginal IC file is bound to the card's role, window, pool and library (Ruling E-36). The card labels it "report-only (rule 8)". The scorecard template names the two K6 pins. |
| N-1 | `6ed5fef8` | The research seal is a `--reuse` input pin of every holdings field. |
| T-1 | `fc83c456` | A new ew-theme-std-v1 test that fails for a wrong rule 1. Its expected values come from the five registered rules written out in the test. Test only. |
| T-2 | `6006fe9d` | The gscore7_lowbm tests now detect which session's me_company is read. Test only; **no defect found**. |
| T-4 | `edcb597b` | Independent references for the spo-v3 solver (two-name closed form, nonzero external_gap) and for gamma's sqrt(252) annualisation. Test only; **no defect found** by reading. The tests were not run. |

No "defect found" stop was triggered (T-2, T-4).

## What was built, per finding

### N-2 (Ruling E-37): `atx-impl/src/strategy_nav_v7.{hpp,cpp}`, `atx-impl/tests/strategy_spo_v3_test.cpp`

The parser:
- now accepts `--rule spo-v3 --capacity-curve`;
- still refuses `--capacity-curve` for spo-v1 and spo-v2 with "spo-v1 does not run the capacity curve".

`State` gains `capacity_engine`. It is built only when `--capacity-curve` is given on a v3 rule, with `void_on_capped = false` (report-only).

In the capacity pass, each capacity book is planned by `capacity_engine`:
- The plan uses NAV `m * nav_post`, where m is the book's capacity multiple (`cost_v2::capacity_multiple`).
- The S2 law stays the primary law.
- A non-capacity book in the capacity pass is refused with a message naming Ruling E-37.

What stays unchanged:
- The main engine's rows, tripwire, summary and timing. The main pass never touches `capacity_engine`.
- `capture()`.

What is added:
- `v7_extras.json` gains `capacity_spo_v3` with these keys:
  - `rule`
  - `calibration`
  - `gamma_equals_main` (a bit compare)
  - `tripwire`
  - `books`
- The declarations gain `capacity_spo_v3`.
- The capacity table is written only when the run is not void.
- The help text names E-37 and says that R-9 is undefined on spo-v3.

New tests:
- `SpoV3.CapacityPassBooksAreTheNavMultipleTrackerAndLeaveTheMainPassAlone`:
  - x1 is bit-identical to the main pass.
  - x4 matches a genuine 4e8 run to the stated tolerances.
  - The main pass is unchanged.
  - The capacity gamma bits equal the main gamma bits.
  - A stray book is refused.
- `SpoV3.TradeFractionHasNoEffectOnTheBook`: theta .25 and .1 give bit-identical days and plan columns, the shadow book moves, and the help text is checked.

`ParseRefusesTheRegisteredConstantsAndRoutesTheImpliedAim` is updated for the new parse behaviour.

### P-2: `atx-impl/tools/mega_report/v8.py`, `docs/plans/mega-alpha-v8-pitch.config.json`, `docs/plans/mega-alpha-scorecard-v8.template.md`

`criterion_eval` gains these check forms:
- `source: summary`, which reads the cell's NAV `summary.json` through the Registry. Both seals apply (path and content).
- `scenario`, read against the parent.
- `{primary}`, which resolves to the primary book.
- An absolute `value` with the ops `le`, `ge` and `eq`.
- `label`.

A manual part with `met` unset is returned as `unread`. `inputs()` lists `v8.cells[K].summary` only for cells whose criterion reads it (R-4, R-5, R-6).

Config changes:
- R-5 adds "S3 not lower".
- R-6 adds:
  - `v7.spo_v3_tripwire.status == "clear"`
  - `v7.spo_v3_books.{primary}.limits_unmet == 0` (E-31)
  - `v7.spo_v3_books.{primary}.aim_correlation_criterion.value >= .9` (E-14)
- R-7's text marks the marginal IC report-only (E-36).

The scorecard template carries the same parts as `NAV[K]` placeholders.

### P-3: `mega_report/v8.py`

`ladder_checks` gains two refusals.

`verdict_rule_checks` refuses three cases:
- a recorded ACCEPTED that the rule rejects ("dSR > 0 AND mechanics AND criterion");
- an ACCEPTED whose criterion has an unread manual part;
- a REJECTED that the rule accepts.

`parent_checks` refuses two cases:
- a parent that is rejected or a defect;
- once every earlier cell is decided, a parent that is not the last accepted cell before the child.

### N-3 (Ruling E-36): `atx-impl/tools/alpha_report_card.py`, `atx-impl/tools/test_horizon_stats.py`

`--marginal-ic` now requires two pins:
- `--marginal-ic-sha256` (already mandatory);
- the new `--marginal-ic-pool-sha256`.

The K6 document must be a complete `atx.marginal-ic/v1` document, and `bind_marginal_ic` checks four things against the card:
- its role manifest;
- the window (`score_begin`, `rows = score_end - score_begin - 22`, first and last decision session);
- the pool's sha256 and the pool's role;
- the library sha256.

Each card's `marginal_ic` carries `use: "report-only (rule 8)"`. The index records the binding.

### N-1: `atx-engine/tools/research_fields_holdings.py`, `atx-engine/tools/test_prepare_research_fields_module_reuse.py`

`seal_pin()` returns `SEAL.isoformat()`. It is used in two places:
- `reuse_inputs` carries it as `seal` for every holdings field.
- Each computed entry records `seal_date`, and `entry_inputs` reads it.

Every holdings field gets the pin because all four kinds (13F quarter clock, FTD, Reg SHO, short volume ext) drop sealed rows before taking a visibility clock. The 13F quarter clock is affected too: a sealed V(P) becomes NEVER.

Two consequences:
- An entry written before this change has no seal, so it is recomputed once.
- Payload bytes are unchanged. The manifest entry gains one key.

New test `ModuleReuse.test_seal_move_recomputes_every_holdings_field`:
- Two seals give two pins; the same seal gives equal pins.
- An old entry (no seal) is not reused.
- A `--reuse` after a seal move recomputes all 9 holdings fields with "inputs differ" and reuses every other field.
- A second run under the moved seal reuses all 63 fields.

One existing assertion is updated for the new key.

### T-1: `atx-impl/tests/strategy_ic_composition_test.cpp`

New test `CompositionV8.RuleOneIsTheWeightedMeanOfSignedMemberRanks`.

Fixture:
- 3 themes with 1, 3 and 3 members, interleaved in library order.
- Mixed signs inside themes b and c.
- Source tiers re-graded by rule 5: `res_mom_12_1` B+ to B-, `ins_opp` B- to C+.
- Within-theme weights proportional to the declared tier scores.
- The rule 4 cap (1/6) binds in two passes. The hand fractions are pinned: 85/528, 11/84 + 11/912, 2/21 + 1/114, 85/924.
- One member is missing for one name, one theme is absent for a name, and two cells are nonmembers.

The reference implements the registration literally:
- `rule_weights` covers rules 5, 1, 3 and 4.
- `rule_blend` covers rules 1 to 3: the weighted mean with W_theme divided out, the re-rank by a literal less/equal count, then W_theme times the rank.

Assertions:
- The runner matches the reference to 1e-15. So does the earlier `v8_ref_blend`.
- The fixture has no near ties: the minimum composite gap is above 1e-4, so the divisor cannot change a rank.
- No theme composite of b or c orders its names as any member's signed ranks do, on any date.
- Each wrong rule 1 moves the blend by more than .2: unsigned, equal within-theme weights, first member only, last member wins. A Python port of the fixture gives .46, .24, .36 and .61.
- Pooled bits equal serial bits.

### T-2: `atx-engine/tools/test_research_fields_v8_quarters.py`

The world now gives line 9 its own price path: its price falls to 30% on role session 60. Its gscore is finite at row 60 and NaN from row 61 on, and this is pinned in `test_values_equal_the_definitions`.

In `test_field_at_t_unchanged_when_rows_after_t_mutate`:
- Line 6's raw_close moves against the other lines from row CUT: x0.5 against x1.9 for the others.
- The test asserts that gscore at CUT is unchanged.
- It also asserts that at CUT + 1 line 6 has left the low-bm tercile (NaN there), while events after CUT are not yet visible.

I ran a probe that patches `gscore_rows` in memory (scratchpad `probe_t2.py`, not committed):

| probe | test_values_equal_the_definitions | test_field_at_t_unchanged_when_rows_after_t_mutate |
|---|---|---|
| reads me_company of session t | fails | fails |
| reads me_company of session t+1 | fails | fails |
| writes NaN everywhere (sanity) | fails | fails |
| unpatched code (t-1) | passes | passes |

**No defect found.**

### T-4: `atx-engine/tests/book/book_target_tracking_test.cpp`, `atx-impl/tests/strategy_spo_v3_test.cpp`

`TargetTracking.TwoNameFactorProblemMatchesItsClosedForm`:
- Setup: intercept + 1 group + 1 style; name 0 is in the group and name 1 is in none. F is dense, costs are zero, and `external_gap` is (.02, -.015, .01).
- The expected optimum is computed in the test from the dense S = B F B' + D, by explicit 2x2 inversion:
  - free: g = -S^-1 B F e;
  - under `net == .01`: the Lagrange closed form.
- The solver must match to 1e-11, with tolerance 1e-13 and a cap of 100000 iterations.
- `tracking_terms` must match the dense variance at the returned book to 1e-12 relative, outside gap included.
- Each wrong reading moves the closed form by more than 5e-4, which is asserted: group column dropped, outside gap mis-signed, outside gap ignored. The numbers come from the Python check in scratchpad `t4_sim.py`.

`SpoV3.GammaIsSPriorOverTheAnnualisedAimVolOfADenseSigma`:
- `last_aim` is bit-equal to L x desired.
- sigma_aim must equal sqrt(252 a' Sigma a) to 1e-12 relative. Sigma = X F X' + D is built densely from the fixture's raw risk files, not through RiskStore.
- The ratio to the daily vol is pinned at 15.874507866387544 (sqrt(252)).
- `v3_sharpe_prior` is pinned at 20 (Ruling E-14), and gamma must equal 20 / sigma_aim.
- The calibration JSON must say the same.

By reading, the code matches both references:
- `tracking_terms` adds `external_gap` to B'(w - a).
- c0 carries gamma B F (B'a - e).
- `calibrate_tracking` uses sqrt(252 x `book_variance`).

**No defect found.** The tests themselves have not been run.

## How root verifies

Build and run with an anchored filter, through the wrapper:

| target | `--gtest_filter` |
|---|---|
| `atx-impl-strategy-target-tests` | `SpoV3.*:SpoPin.*:SpoHook.*:SpoTripwire.*:NavV7Hook.*:CostV2Capacity.*:AdvHold.*` |
| `atx-impl-strategy-ic-tests` | `StrategyIcComposition.*:CompositionV8.*` |
| `atx-engine-book-tests` | `TargetTracking.*` |

The new tests are:
- `SpoV3.CapacityPassBooksAreTheNavMultipleTrackerAndLeaveTheMainPassAlone`
- `SpoV3.TradeFractionHasNoEffectOnTheBook`
- `SpoV3.GammaIsSPriorOverTheAnnualisedAimVolOfADenseSigma`
- `CompositionV8.RuleOneIsTheWeightedMeanOfSignedMemberRanks`
- `TargetTracking.TwoNameFactorProblemMatchesItsClosedForm`

The modified test is `SpoV3.ParseRefusesTheRegisteredConstantsAndRoutesTheImpliedAim`. The spo-v1/v2 digest guard (`SpoPin.*`, and `SpoV3.*` in `strategy_spo_v3_pin_test.cpp`) must still pass unchanged. A quick single-TU compile first:
- `check atx-impl/src/strategy_nav_v7.cpp`
- `check atx-impl/tests/strategy_spo_v3_test.cpp`
- `check atx-impl/tests/strategy_ic_composition_test.cpp`
- `check atx-engine/tests/book/book_target_tracking_test.cpp`

pytest, all on synthetic data. The house interpreter is Python 3.12 with numpy 1.26.4:

| suite | result |
|---|---|
| `atx-impl/tools`: `test_alpha_report_card`, `_store`, `test_book_diagnostics`, `test_horizon_stats`, `test_mega_report_pitch3`, `_seal`, `_sig_corr`, `test_mega_report_v8`, `_v8_render` | 204 passed |
| `atx-engine/tools`: `test_prepare_research_fields_module_reuse`, `test_research_fields_holdings`, `test_research_fields_v8`, `test_research_fields_v8_quarters`, `test_seal_partitions`, `test_research_window` | 55 passed (plus 6 subtests) |
| `scripts/tests/test_research_cycle.py` (untouched; a sanity check of the card argv) | 91 passed, 3 skipped |

## Deviations

- **N-1:** I did not update two things in the builder file `prepare_research_fields.py`, which is outside this lane:
  - The `REUSE_MODULE_RULE` text still names only "the stage manifest SHA-256s, and the SEC identity bridge".
  - The not-reused reason still reads "inputs differ (stage manifest or bridge SHA-256)".

  Both are accurate for the holdings seal only in spirit. A one-line edit there would name the seal.
- **T-1:** I did not strengthen the runner-level rerank-on case (`strategy_ic_runner_test.cpp`, which only asserts on != v1). The new unit test drives the same `IcComposition::add_standardised` / `finish` path the runner calls.
- **T-2:** Besides the point-in-time move the brief asks for, I gave one line its own price path in the world (`close_of`), so the main oracle test also separates t-1 from t and t+1. This was the review's "own drift" fix.
- **T-4:**
  - I used a two-name closed form as the brief says, not the review's n = 6.
  - I did not add the review's "KKT residual with the costs on" check, which the brief does not list.

## Cross-lane edits

- `atx-engine/tools/test_prepare_research_fields_module_reuse.py`: the N-1 test, plus one updated assertion. The brief requires a test; this file is where module reuse is tested.
- `atx-engine/tests/book/book_target_tracking_test.cpp`: T-4, named by the review.

Nothing else outside the brief's file list.

## Open risks

- **Nothing in C++ was compiled or run here.** This covers N-2, T-1 and T-4. In particular:
  - The x4 tolerances in the N-2 capacity test (net return 1e-9 + 1e-6 relative; 4 x traded dollars within 1 + 1e-5) were set by reasoning and have not been run.
  - The T-4 closed form expects the ADMM to reach 1e-11 at tolerance 1e-13. The existing one-name test uses the same settings.
- **N-2 coverage:** the `write_extras` `capacity_spo_v3` block and the void-guarded capacity table are not tested at the CLI level. The engine-level behaviour is tested.
- **N-2 declarations:** the help text and the recipe/summary declarations gain the capacity keys only when the flag is given, so a spo-v3 run without the flag is byte-identical to before.
- **N-3 pool pin:** a manual card run that passes `--marginal-ic` must now add `--marginal-ic-pool-sha256`. `research_cycle.py`'s card step passes no K6, so it is unaffected.
- **P-3:** an n/a rule is refused only when an unread manual part causes it. A missing input stays its own single unavailable block, which keeps the "one owner per missing input" rule.
- **E-31:** it is encoded as an R-6 criterion part (`limits_unmet == 0` on the primary book). It does not make the cell invalid.
- **T-10** (R-1 planned turnover) is not addressed in this lane.
- **N-1 one-time cost:** fields directories built before `6ed5fef8` recompute their 9 holdings fields once on the next `--reuse`. Manifest-level byte comparisons across this change will see the new `seal_date` key; payloads are unchanged.
- **Python environment:** this shell now has `VIRTUAL_ENV=c:\atx\atx-db\.venv` (numpy 2.5.2) first on PATH. Under it, 31 tests of `test_mega_report_v8_render.py` fail. The cause is a fixture problem: `daily_text` writes `repr(np.float64)`, which is "np.float64(...)" under numpy 2, so the daily CSVs do not parse and `fig_exposure` becomes unavailable. This lane did not change `daily_text`. Run these suites with Python 3.12 (numpy 1.26), where they all pass, or fix the fixture to use `float(x)`.
