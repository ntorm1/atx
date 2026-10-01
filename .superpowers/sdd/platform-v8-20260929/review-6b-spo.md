# Review 6B-S: FIX-2 round 1 (spo-v3: E-31a / SPO-1 / SPO-5, SPO-4, E-14a / SPO-2)

Worktree `C:/atx-wt/pool-7`, head `de32b9ad`, range `d43949b7..de32b9ad` (ad50e574, dd1b923e, 8ca8326e, f1cd0e43).
Read-only: nothing was built, run or edited, and no data or `build-equity/` path was opened. Line numbers are at `de32b9ad`.
Rules applied: E-14, E-14a, E-15 / PM4-5, E-26, E-29, E-31, E-31a, E-37 (`pool-2/.../progress.md`), review-spo.md.

Severity: I invalidates a result or breaks a registered rule; M must fix before R-6; m minor.

## Verdicts

| item | verdict | why (evidence) |
|---|---|---|
| SPO-1 (E-31a) | **ADDRESSED** | `tracking_tripwire` checks the primary book's `limits_met == false` rows first, for either value of the void flag (`strategy_spo_v3.cpp:350-359`). `capture` returns that error (`strategy_nav_v7.cpp:547-552`) after `summarize_nav` and before `publish_nav` creates the directory (`strategy_nav_replay.cpp:3143-3150`, `:3167`). The only Sharpe line is inside `publish_scenario` (`:2660`). The dispatcher writes `v7_extras.json` (`status: void`, `voided: limits_unmet`, `limits_unmet {count, first_session, book, rule}`) and exits 3 (`strategy_nav_v7.cpp:313-324`, `:893-906`). The capacity pass cannot start (`:893-919`). The primary label comes from the run's own matrix (`:510`). |
| SPO-2 (E-14a) | **ADDRESSED** | `aim_correlation_traded_after` of row d = corr(the book the same book's next rebalance DECIDE reads, aim_d), over the names either one holds. It is back-filled per book (`strategy_spo.cpp:984-993`, `:1514-1516`, `:1557-1561`) and is NaN on the last row (`strategy_spo.hpp:332`). The criterion, report, CSV, units, pitch check, scorecard cell and r6 text all read the same `Stats` mean (`strategy_spo_v3.cpp:92-114`, `:314-317`). The plan value `aim_correlation` stays beside it. |
| SPO-4 | **ADDRESSED** | Capacity books plan only on `capacity_engine` (`strategy_nav_v7.cpp:396-406`), which has an empty primary label and the void flag off (`:36-39`, `:52-54`). `capture` and the main extras read only `engine` (`:547`, `:346-357`). Hazard (b): the trade limit and impact use `in.nav_post` = m x NAV_post (`strategy_spo.cpp:1218`, `:1230`, `:1238`). E-15 / PM4-5 is unchanged (declaration `:165-175`). |
| SPO-5 | **ADDRESSED** | `--spo-iters` and `--spo-tol` are in `refused_with_v3` (`strategy_nav_v7.cpp:112-117`). They are refused in either argv position and before value conversion (`:775-781`). spo-v1 and spo-v2 still accept them (test `:862`). |
| SPO-3, SPO-6 | open (known) | Not touched by this round. SPO-3 now also matters to E-31a: a NaN iterate reads as converged with limits met, so it would not void (reachable only by overflow). |

## Answers

**Q1: can a voided primary book still publish a return, NAV or Sharpe?** No path found. Checked:

- **Writers.** Only `publish_nav` and `publish_holdings` write returns. `publish_nav` runs after `capture` (`strategy_nav_replay.cpp:3150` < `:3167`). `--emit-holdings` is refused for spo-v3 whatever the void flag (`strategy_nav_v7.cpp:842-846`), and no spec or runner passes it.
- **Console.** Nothing before `capture` prints a return. The error path prints only the tripwire text (`strategy_nav_replay.cpp:3437`, `strategy_nav_v7.cpp:903`).
- **Side files on a void.** `v7_transfer_coefficient.csv`, `spo_diagnostics.csv` and the `spo_v3` block hold no return quantity.
- **Capacity pass.** It runs only after a clear main pass (`:893-907`). Its `capture` re-reads the unchanged main rows.
- **Grid and book workers.** Both are refused with the extension (`strategy_nav_replay.cpp:3185-3186`, `:2273-2276`).
- **Scenario matrices.** For tiered matrices, `--spo-books primary` and `--cost-v2`: S2 stays at index 1 and the label is taken before the resize (`strategy_nav_v7.cpp:507-514`, `strategy_nav_replay.cpp:2158-2169`).
- **Other entry points.** No resume exists in the replay. The extension is built only by `dispatch_nav_v7` (`strategy_nav_v7.cpp:891`). A runner rerun HARD-STOPs on the existing output (`research_cycle.py:1586-1587`, `:1610-1612`).
- **Which decisions are scored.** Rows exist only for rebalance decisions with d >= `decision_begin` (`strategy_spo.cpp:1271`, `:1557`). Warm-up decisions take `warm_up_step`, which writes no row and never touches `row_before` or the dual, so warm-up rows can neither void a run nor hide a void. With cadence 1 every scored decision is a rebalance decision, so every scored decision has a primary row.

**Q2: E-14a.**

- **Inputs and timing.** The value uses `current` at the next rebalance DECIDE of the same book. Under MARK -> EXECUTE -> DECIDE (`strategy_nav_replay.hpp:22-23`) that book holds d's fills at d+1's close plus one session's drift. aim_d is a copy taken at d (`strategy_spo.cpp:1559`).
- **Look-ahead.** No decision input reads the value, so there is no look-ahead. It is published on the row of session d but carries d+1 prices (see note N1).
- **Alignment.** `row_before` is per book and indexes the append-only `tracking_rows`. `begin_run` clears it (`strategy_spo.cpp:1579`), so a pass never back-fills another pass's row.
- **Name set.** The union of the support of aim_d and the support of the next book, so nonmember exits are included. This is the same definition as A-4 (`:929-937`).
- **Last decision.** The value is NaN, and `Stats` skips it, so n = decisions - 1 (`strategy_spo_v3.cpp:42-46`).
- **Cadence > 1.** The value includes the intermediate non-rebalance exits and k sessions of drift. Every v8 base is cadence 1 (`base-lo1.json:95`, `base-lo3.json:105`), so no v8 cell is affected.
- **Names entering or leaving.** Both are handled by the union.
- **Readers.**
  - The C++ criterion `value` and `report.aim_correlation_traded_after.mean` are the same `mean()`.
  - The pitch config (`mega-alpha-v8-pitch.config.json` R-6 checks), the scorecard R-6 row and the r6 text all read `v7.spo_v3_books.<primary>.aim_correlation_traded_after.mean` from summary.json. summary.json uses the same `report()` (`strategy_spo_v3.cpp:314-317`).
  - Same field, same rows.

**Q3: SPO-4.**

- **Rows.** No capacity row can reach `engine->tracking_rows()`, the main tripwire, `report_only`, the counts or E-31a. The main pass never sees `capacity_engine`, and the capacity pass routes `plan` and `hold` to `capacity_engine` before either call (`strategy_nav_v7.cpp:396-412`).
- **Primary series.** `--capacity-curve` on or off does not change a byte of the primary series:
  - the main pass runs first with identical replay argv, because `--capacity-curve` is consumed by the v7 parser (`:710-713`);
  - `run_scenarios` does not read `o.capacity` on the main pass;
  - `capacity_engine` has its own state and shares only the const `RiskStore`.
- **What does differ** is the v7 declaration block in recipe.json and summary.json (`capacity_curve`, `capacity_spo_v3`). This was already true of aim-partial-v5's curve.

**Q4: flag off.** With spo-v3 not selected, every changed path is unchanged:

- `refused_with_v3` and the `--emit-holdings` refusal apply only when the version is 3.
- The new `write_extras` block requires `version == 3`.
- `run_scenarios` now calls `set_primary_book` on spo-v1/v2 engines too, but no v1/v2 output reads it (`ceiling_tripwire` and `tripwire_json` are unchanged).
- `Engine::Impl` gains `primary_book`, built from `default_primary_book()`, for every version. This has no output effect.
- `traded_correlation` is a verbatim extraction of the old inline loop and is called only under v3.
- `append_help` text changed (console only). The asserted substrings remain: `strategy_cost_v2_test.cpp:365-368`, `strategy_spo_v3_test.cpp:1281-1286`.
- aim-partial-v5/v6 have no engine, so nothing on their path is reached.

spo-v3's own bytes do change: CSV column 43, tripwire and parameter keys, declaration text. No v3 pin exists; `strategy_spo_v3_pin_test.cpp` pins v1/v2 only.

**Q5: tests.**

- These tests can fail on a wrong rule: `PrimaryBookLimitsUnmetVoidsTheRunWhateverTheVoidFlag` (book filter, flag independence, count, first session, `voided` key), `EngineVoidsOnItsPrimaryBooksLimitsUnmet` (the premise is asserted), the parse test, and the SPO-4 x4 versus NAV-4V comparison (`:1200-1214`).
- **The "fixture premise" is not a hole for the tripwire rule.** The two deterministic tests drive both branches whatever the fixture does, and the replay-level `expect_clear_or_limits_void` checks label, count and status consistency.
- **The real holes are elsewhere** (R6B-S-1, R6B-S-2):
  - The label seam that binds E-31a on the registered tiered run is untested.
  - The CLI void path is untested.
  - Every spo-v3 replay test is single-book: `replay_nav` runs `cfg.scenario` only (`strategy_nav_replay.hpp:357-362`), so per-book state under lockstep interleaving is never exercised.

**Q6: compile risk.** See below. No line is a confident failure.

## Findings

| id | sev | file:line | scenario | smallest fix |
|---|---|---|---|---|
| R6B-S-1 | m | `strategy_nav_v7.cpp:510`; `:313-324`, `:893-906` | **The E-31a seams outside the engine are untested.** (a) Delete or reorder the `set_primary_book(book_label(primary))` call: every test stays green, because the tests replay via `replay_nav` with the default untiered label. On the registered R-6 run (tiered fields, `--spo-books primary`) the engine would then compare rows against `modeled-1bn-stale5-v1+flat-300-v0` (`strategy_nav_replay.hpp:71`). No row carries that label, so E-31a would silently never fire. With `--spo-books all` it would bind the S2 x flat-300 stress book (tiered index 3) instead. (b) No test runs `write_extras`' `voided` / `limits_unmet` block or exit 3. The code is correct by reading. | Unit test: build a `ScopedNavExtension` with spo-v3 options (a null risk store is fine for this), call `begin_run(Main)` and `run_scenarios(nav_scenario_matrix(true))`, and assert `spo_engine()->primary_book() == "modeled-1bn-stale5-v1+swap-fin-v1"`. Then call `begin_run(Capacity)` and `run_scenarios` again, and assert the label is unchanged and the capacity engine's label is empty. For (b), the integrator confirms the exit-3 extras once on a fixture (strategy_live_test's `nav_cli` bench, forced via the unrestorable-net premise) or by reading at merge. |
| R6B-S-2 | m | `strategy_spo_v3_test.cpp:713-796` (via `replay_v3` `:128`, `:134`) | **E-14a's per-book back-fill is tested on one book only.** An implementation keeping one engine-wide `aim_before` / `row_before` passes the test. On the real 3-to-5-book lockstep run, rows interleave S1/S2/S3 per decision, so S2's row d would then be paired with S3's book and S1's aim. The current code is per book (`strategy_spo.cpp:984-993`), so this is a test gap, not a defect. | Run the recorder test through `replay_nav_scenarios` with two scenarios and observed index 1. Assert the observed book's `aim_correlation_traded_after` as now, and that the other book's rows are back-filled from its own book (its values differ). |

Counts: I 0, M 0, m 2.

## Compile risk (clang-cl /W4 /permissive-, /WX)

clang-cl /W4 is `-Wall -Wextra`; `-Wshadow` and `-Wconversion` are only on the non-MSVC branch (`CMakeLists.txt:265-282`). I checked each item below and **found no line I am confident will fail**:

- Header/definition signatures match:
  - `tracking_tripwire(_json)` is declared at `strategy_spo_v3.hpp:108-118` and defined at `.cpp:350`, `:375`. No other caller of the old 2-argument form remains.
  - `default_primary_book` is declared at `.hpp:83` and defined at `.cpp:344`. `fixed_nav_scenarios` and `nav_primary_scenario_index` are reachable through `strategy_spo.hpp` -> `strategy_nav_replay.hpp`.
  - `set_primary_book` / `primary_book` are declared at `strategy_spo.hpp:429-430` and defined at `strategy_spo.cpp:1590-1591`.
  - `strategy_spo.cpp` includes `strategy_spo_v3.hpp` (`:22`) for the member initializer at `:1024`.
- Includes and types:
  - `<string_view>` is added to the v3 header. The test adds `<tuple>` and `<string_view>`.
  - `BookDecision::book` is a `std::string_view` (`strategy_spo.hpp:279`), so the test's `label` passes as is.
  - json compared with `std::string` in `EXPECT_EQ` already compiles elsewhere (`strategy_nav_replay_test.cpp:1013`).
  - `static constexpr usize no_row` inside the nested `BookState` is fine.
- No unused locals or parameters in the new code: every structured binding is read, every new anonymous-namespace symbol is used, and the `Unmet` / `Stats` fields are all read.
- No missing-field-initializers: `BookDecision` gets all 10 fields at `test:485`.
- No sign-compare: comparisons are `usize` vs `usize`, `i64` vs `i64`, or `version == 3` (a non-negative literal).
- gtest names are unique within the file.

Residual uncertainty: the test's structured bindings from a `[&]` lambda returning `std::tuple<bool, bool, json>` (`:480-495`, `:498-511`) are standard but were never compiled. I rate them low risk.

## Notes for the PM (not findings)

- **N1 (E-14a wording).** The ruling's "(both known at d's close)" cannot hold under MARK -> EXECUTE -> DECIDE: d's orders fill at d+1's close (`strategy_nav_replay.hpp:22-23`). The recorded value is the earliest observable after-trade book, and it carries one session of d+1 drift. This is the pairing review SPO-2 proposed, and the lane disclosed it. The row labelled session d therefore holds d+1 prices: a diagnostic only, no input reads it.
- **N2 (E-31a and the B0c warm start).** The first scored decision plans from the aim-partial-v5 warm-up book. That book has no constraint on the risk model's beta, so its ex-ante beta can sit outside +-.02. If the 1% ADV trade limits cannot bring it inside in one session, R-6 voids at its first scored decision. That is per the rule, but the void would come from the warm start rather than from tracking. Expect this as a possible first-run void cause.
- **N3 (E-29 / E-37 on the template).** `r6-spo-v3.json:13-14` still sets `--capacity-curve: false`, and its description says "spo refuses it". This contradicts E-37, but F-14 (FIX-3) owns the fix. The lane edited the same description line, so the integrator must merge both.
- **N4.** The capacity engine's tripwire record carries `primary_book: ""` and `limits_unmet_primary.count: 0` by construction. Its per-book `limits_unmet` lives in `report_only`, so read capacity books there.

## Not read

- `atx-engine/src/book/target_tracking.cpp`: how `limits_met` is computed (relied on review-spo Q4).
- `strategy_spo.cpp` 1-900 and 1100-1195: prepare, the v1/v2 solver, fixed positions above `:1196`.
- The `v7::plan` call site and its book loop (`strategy_nav_replay.cpp` ~880-1000).
- `strategy_spo_v3_test.cpp` 260-350 and 940-1095 (the E-26 shaping tests; untouched except by the header).
- `mega_report_v8.py`'s `{primary}` resolver and template renderer (only their tests' diffs were read).
- `strategy_spo_test.cpp`, `strategy_spo_pin_test.cpp`; `strategy_spo_v3_pin_test.cpp` beyond a grep.
- The full `base-b0c.json` (grep only), and `cycle_resume`.
