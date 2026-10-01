# Task FIX-4b report (pool 10, branch `feat/platform-v8-fix4b-20261001`)

Base `43a0447d`. One commit per finding, in the order of the brief. Nothing was built (C++), no real data
was opened, no subagent was used, nothing was pushed. Python was run on synthetic fixtures only.

| finding | commit | kind |
|---|---|---|
| R6B-C-2 (PM4-8, PM4-9, PM4-10) | `f408a6f1` | Python + config + template |
| R6B-C-4 | `cf15052d` | Python |
| R6B-S-1 | `0089b5e8` | C++ tests only |
| R6B-S-2 | `b69317f8` | C++ tests only |

Pytest (this session, after all four commits, `ATX_EQUITY_ROOT` / `ATX_EQUITY_BIN` unset):

- Lane suites: `atx-impl/tools/test_mega_report_v8.py`, `atx-impl/tools/test_mega_report_v8_render.py`,
  `scripts/tests/test_research_spec.py`: **168 passed**.
- Downstream (importers of `mega_report.v8` through `pitch.py`, and every `research_cycle.py` consumer):
  `test_mega_report_pitch3.py`, `test_mega_report_sig_corr.py`, `test_alpha_report_card.py`,
  `scripts/tests/test_research_cycle.py`, `test_research_cycle_roles.py`, `test_research_cycle_label_role.py`,
  `test_research_ledger.py`, `test_cycle_resume.py`, `test_cycle_scoring.py`, `test_cycle_e2e.py`:
  **196 passed, 4 skipped** (env-gated tests).
- Not run (on purpose): `test_nav_summ_v8.py` (reads a real `build-equity` NAV when its env var is set) and
  `test_mega_report_seal.py` (names `build-equity` 2024 paths). Neither imports a file this lane touched.

Command: `"C:/Program Files/Python312/python.exe" -m pytest -q -p no:cacheprovider <files>`.

## R6B-C-2: v8 ladder R-8..R-12 and the E-38 / E-45 branch (`f408a6f1`)

Files: `atx-impl/tools/mega_report/v8.py`, `atx-impl/tools/test_mega_report_v8.py`,
`atx-impl/tools/test_mega_report_v8_render.py`, `docs/plans/mega-alpha-v8-pitch.config.json`,
`docs/plans/mega-alpha-scorecard-v8.template.md`.

What it does (as committed):

- **R-8**: rule 5 plus each TRAIN year's realised S2 net volatility inside [.04, .06] (E-43). It is read from the
  cell's own nav_summ year table through a new `source: years` check (`year_values`, `years_eval`).
- **R-9a..c**: report-only frontier cells. They have no paired test and no rule; an accept or reject recorded on
  one is refused.
- **R-10 / R-11**: rule 5 plus planned turnover per unit gross not higher than the parent (E-44).
- **R-12**: rule 5 plus book turnover not higher than the parent (PM4-9).
- Every recorded verdict is compared with the computed rule, and every parent must be the last accepted cell
  (the P-3 style).
- **Branch**: `defined_if` is read from the recorded verdicts. R-9 exists only if R-6 was rejected; R-10 and R-11
  only if R-6 and R-1 were accepted; R-12 only if R-6 was accepted.
- **Undefined cells**: a verdict "undefined ..." records a cell that could not be formed (PM4-10). Such a cell
  renders as "undefined (ruling id)", reads no input, adds 0, is never a parent and is never an unavailable
  block. A decided verdict on it is refused.

Root verifies: no build. Run `test_mega_report_v8.py` and `test_mega_report_v8_render.py`. Identity: with the new
cells absent the pitch renders the pre-change bytes. The normalised sha256 is pinned in
`test_v8_pitch_without_the_optional_cells_renders_the_pre_pm4_8_bytes` and
`test_config_without_optional_cells_is_unchanged_by_the_branch_machinery`.

Deviations: none known. Cross-lane edits: none. Open risk: the R-8 band [.04, .06] encodes PM4-8's ".8 to 1.2 x
5%" as constants in the config. A change of the 5% target needs a config edit, not a code edit.

## R6B-C-4: the E-27b guard reads the parsed argv (`cf15052d`)

Files: `scripts/research_cycle.py` (`validate_v8_keys` area: new `argparse_values`, `validate_e27b`),
`scripts/tests/test_research_spec.py` (three new tests at the end of the file).

What it does:

- `--protocol` (in `summ.extra`) and `--composition` (in `fit.flags`) are read the way argparse does with
  `allow_abbrev`: the two-token pair, `--option=VALUE`, any accepted prefix, and repeats.
- A v8 spec carrying `ew-theme-aim-v1` in any spelling is refused with the E-27b message at plan, run, lock and
  add-alpha, before any write.
- The cycle's own readers still read one two-token pair. So in a v8 spec, any other spelling of `--protocol` or
  `--composition` is refused at load. No committed spec uses one (every v7 and v8 spec still loads and plans).

Root verifies: `scripts/tests/test_research_spec.py`, in particular these tests:

- `test_e27b_reads_what_the_fitter_and_nav_summ_parse`: each spelling is first shown to parse as v1 / v8 in the
  real `fit_composition_weights` and `nav_summ` parsers.
- `test_e27b_refuses_every_spelling_at_plan_run_and_lock`.
- `test_e27b_refuses_every_spelling_at_add_alpha`.

Deviations: none. Cross-lane edits: none. FIX-4a also edits `research_cycle.py` and `test_research_spec.py`; this
lane stays inside `validate_v8_keys` / `validate_e27b` and its own three tests at the end of the test file, so a
textual merge is expected to be clean. Open risk: the merge order with FIX-4a. Re-run `test_research_spec.py`
after both lanes land.

## R6B-S-1: the E-31a seams outside the engine (`0089b5e8`, tests only)

Files:

- `atx-impl/tests/strategy_spo_v3_test.cpp`: two tests and helpers (`label_of`, `gross_one_desired`,
  `unmet_rows_in_csv`). `primary_label` now calls `label_of` (same value).
- `atx-impl/tests/strategy_spo_cli_fixture.hpp`: **new** shared test header.
- `atx-impl/tests/strategy_spo_test.cpp`: cross-lane edit, see below.

**(a) `SpoV3.TieredRunVoidsOnItsSwapFinancedPrimaryBookOnly`.** Each run is a fresh `ScopedNavExtension` (spo-v3,
`--spo-books all` and `primary`): `begin_run(Main)`, then `run_scenarios(nav_scenario_matrix(true))`. In each run
one book of the tiered matrix plans one decision through `v7::plan` that it cannot restore: the role has volume 0,
so ADV is 0, every trade limit is 0, every name stays at its current .01 and the net stays at .12.

Asserts:

- the engine's primary is `modeled-1bn-stale5-v1+swap-fin-v1`;
- `capture()` voids, with the E-31a reason naming that book, exactly when the planning book is tiered index 1;
- an unmet row on index 3 (S2 x flat-300-v0, the engine's default label) or any other book does not void;
- the capacity pass on the same matrix returns the primary's capacity books, leaves the main engine's primary
  unchanged, and its capacity engine has no primary.

Deleting the `set_primary_book(book_label(primary))` line in `run_scenarios` (`strategy_nav_v7.cpp:585` at this
base) fails the test both ways: index 1 no longer voids and index 3 voids.

**(b) `SpoV3.CliVoidOnPrimaryLimitsUnmetExitsThreeWithTheExtrasOnly`.** It runs `dispatch_nav_replay` with
`--rule spo-v3 --spo-books primary` on a written Role(100, 12, 53) and its risk model, once with
`--specific-ceiling-void on` and once with `off`. Asserts:

- exit 3;
- `<output>` holds exactly `spo_diagnostics.csv`, `v7_extras.json` and `v7_transfer_coefficient.csv`, so there is
  no recipe, summary, daily, events, NAV or return file;
- "run VOID" is on the console and no "net Sharpe" appears on out or err;
- `v7_extras.json` has `status: void` and `voided: limits_unmet`;
- its `limits_unmet` block has `book` = the untiered S2 label and a `rule` that cites E-31a, and its `count` and
  `first_session` equal the `spo_v3.tripwire` record and the primary book's `limits_met = 0` rows in the
  published CSV;
- the tripwire record's `specific_ceiling_void` follows the flag.

Premise: the role's volume is 0 from session 30. The replay's liquidity window is the 63-session default (asserted;
no CLI flag moves it), so from decision 93 every name's ADV is exactly 0. No book can then plan a trade (trade
limit 0, every name pinned) or fill one, and the primary book's drifted net (and beta) cannot be restored within
`tracking_limit_tolerance` (1e-12).

**Shared CLI fixture.** The nav CLI input writer is now shared rather than copied: `write_run_inputs`,
`RunInputs`, `write_json_file` and `read_json_file` moved unchanged from `strategy_spo_test.cpp` into
`strategy_spo_cli_fixture.hpp` (inline, namespace `...spo::fixture`). `strategy_spo_fixture.hpp` (frozen by the
digest pin) is untouched.

Root builds `atx-impl-strategy-target-tests`, which holds `strategy_spo_test.cpp`, `strategy_spo_v3_test.cpp` and
both pin tests. `atx-impl-tests` also compiles them, because it globs every `*_test.cpp`. Gtest filter:
`SpoV3.TieredRunVoidsOnItsSwapFinancedPrimaryBookOnly:SpoV3.CliVoidOnPrimaryLimitsUnmetExitsThreeWithTheExtrasOnly:SpoTripwire.*:SpoPin.*`.
The `SpoTripwire.*` part re-checks the moved helpers in their old user. The v3 pin test's suite is `SpoV3`.

Deviations: (b) drives the void through the real CLI instead of the reviewer's alternative (a reading at merge),
so no source seam was needed.

Cross-lane edits:

- `atx-impl/tests/strategy_spo_test.cpp`: 73 lines of helpers deleted and one `#include` added. No test body
  changed.
- New file `atx-impl/tests/strategy_spo_cli_fixture.hpp`.

Open risks:

- First compile at integration, under `/W4 /WX`. Risk points:
  - (b)'s structured binding `const auto [count, first]` used inside gtest macros (the file already does this
    elsewhere);
  - a local `struct Decided` returned from a lambda;
  - `EXPECT_EQ(json, bool)` (already used in `strategy_live_test.cpp`).
- (b)'s premise is argued, not run. It rests on the drifted book's net after decision 93 being above 1e-12, which
  holds unless the primary book is exactly flat there; the tracker trades from decision 1 at a positive ADV. A
  natural E-31a void earlier on the fixture (daily redrawn exposures) also satisfies the test.
- (b) runs two replays of 100 sessions x 2 books (a few seconds).

## R6B-S-2: E-14a back-fill per book on two cadences (`b69317f8`, tests only)

File: `atx-impl/tests/strategy_spo_v3_test.cpp`. New helper `traded_pearson`; the criterion test's inline lambda
now calls it (same arithmetic as the engine's `traded_correlation`).

**`SpoV3.TradedAfterIsBackFilledPerBookAcrossCadences`.** One spo-v3 `Engine`, two books, driven by
`Engine::plan` with explicit liquidity (ADV 1e9):

- book A (S2 x flat-300-v0) decides every session 2..11;
- book B (S2 x swap-fin-v1) decides every third session: 2, 5, 8, 11;
- on a shared session A plans first, as in the lockstep;
- each book enters a decision with its own previous plan, drifted by a fixed per-name factor.

Asserts:

- 14 rows, each owned by the right book;
- every row's `aim_correlation_traded_after` equals, within 1e-12, the correlation over the union support of the
  book its own book's next decision received with the aim at the row's decision;
- each book's last row is NaN;
- 12 rows are filled.

An engine-wide back-fill fills A's session-2 row from B's flat book (NaN) and pairs B's rows with A's books, so it
fails. A lockstep replay cannot give two books different cadences (the construction grid is refused under the
extension), so the test drives the engine directly.

Root builds `atx-impl-strategy-target-tests` and runs gtest filter
`SpoV3.TradedAfterIsBackFilledPerBookAcrossCadences:SpoV3.CriterionReadsTheTradedBookAfterTheTrades`.

Deviations: the engine is exercised directly, not through `replay_nav_scenarios` (the reviewer's suggestion),
because only a direct drive can give the two books different cadences, as the brief requires. Cross-lane edits:
none. Open risk: first compile at integration (a local aggregate `Book` with default member initialisers, brace
initialised with every field).

## Identity

Every change in S-1 and S-2 is test code; no source byte moved, so every output is unchanged. C-2's identity is
pinned by the render test (above). C-4 changes only the refusals of non-two-token spellings and of E-27b, and every
committed spec still loads and plans.

## Root's build list for this lane

- `powershell scripts\atx-build.ps1 build atx-impl-strategy-target-tests` (`atx-impl-tests` compiles the same files)
- Run filter `SpoV3.*:SpoTripwire.*:SpoPin.*`.
- Python: the lane suites listed above.
