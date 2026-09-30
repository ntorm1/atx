# Task W0-1 report: research-window-v2, TRAIN 2020-2023, seal 2024-01-01

Lane W0E, pool-3, branch `feat/platform-v8-w0e-20260929`. Nothing built, nothing run on real data.

## What was built

New files
- `atx-impl/strategies/research_window.json`: the brief's JSON verbatim (schema `atx.research-window/v2`, LF).
- `atx-engine/include/atx/engine/data/research_window.hpp`: the brief's header (same values), plus
  `kSealBeginDate = "2024-01-01"` (the date text C++ refusals and manifests print, so no call site spells it) and
  two `static_assert`s (bound order; midnight labels). `is_sealed` split over three lines for the 100-column rule.
- `atx-engine/tools/research_window.py`: `load() -> dict`, `TRAIN_BEGIN_NS`, `TRAIN_END_NS`, `SEAL_NS`, `SEAL_DATE`,
  `is_sealed(ns)` as briefed, plus `WINDOW_ID` (`research-window-v2`, derived from the schema), `TRAIN_BEGIN_DATE`,
  `TRAIN_END_DATE`, `SEAL` (date), `FIRST_SEALED_YEAR` (E-4), `SealError(ValueError)`, `seal_message(what)`,
  `refuse_sealed(ns, what)`, `period_begin`, `partition_is_sealed(year, quarter=None)`, `last_quarter_before_seal()`,
  `window_values`, `current()`, `superseded()` (the research-seal-v1 values of the `supersedes` block) and `bind()`
  (test harnesses only). The JSON is found at `Path(__file__).parents[2]/atx-impl/strategies/research_window.json`.
- `atx-impl/tools/engine_tools.py`: `from engine_tools import research_window as rw` for atx-impl tools. It loads
  the engine file by path as the private module `atx_impl_engine_tools_research_window`; it does not touch `sys.path`.
- `atx-engine/tools/conftest.py`: binds `research_window.superseded()` for the atx-engine/tools pytest session (see
  Deviations 2).
- `atx-engine/tools/test_research_window.py`: `test_json_and_module_agree`, `test_seal_refuses_2024`,
  `test_2023_session_is_train` (the brief's three), plus `test_window_is_the_owner_ruling` (pins the owner values),
  `test_header_matches_json` (parses the .hpp integers and strings), `test_helpers`. Every check of the repository
  window runs in a fresh interpreter (`python -c`), which is what a tool sees.
- `atx-engine/tools/test_seal_partitions.py`: sealed partitions are never opened (PM follow-up, see below).
- `atx-engine/tests/data/data_research_window_test.cpp`: `ResearchWindow.HeaderMatchesJson`,
  `ResearchWindow.IsSealedFromTheSealSessionOn`, `ResearchWindow.RefusesSealedSession` (last session 2024-01-02 and
  the seal day itself; `InvalidArgument`, message names `research-window-v2` and `2024-01-01`),
  `ResearchWindow.Accepts2023` (last session 2023-12-31, score end exactly the seal).
- `atx-engine/tests/data/strategy_role_fixture.hpp`: the synthetic role of `strategy_data_test.cpp`, moved to a
  header (named namespace `atx_test_strategy_role`) with a `first_day` parameter, so both tests share it.

Constant sites replaced by a read of the window
- `atx-engine/src/data/strategy_data.cpp`: `kSeal` removed; `score_end_ns > kSealBeginNs` and `is_sealed(session)`
  refuse with `"strategy role: <score end|session> at or after the research seal 2024-01-01 (research-window-v2)"`.
  The two combined messages were split so the non-seal failures keep their own text.
- `atx-impl/src/strategy_risk_verb.cpp`: `seal_begin_ns = rw::kSealBeginNs`; refusal text, `--help` text and the
  manifest `seal.begin` come from the header.
- `atx-impl/src/strategy_live.hpp/.cpp`: `research_seal_policy = kResearchWindowId.data()`,
  `research_seal_session = kSealBeginDate.data()`, `research_seal_exclusive_ns = kSealBeginNs`; messages built from them.
- `atx-impl/tools/fit_composition_weights.py`: `FIT_BEGIN_NS = rw.TRAIN_BEGIN_NS`, `TRAIN_END_NS = rw.TRAIN_END_NS`,
  `AIM_SEMANTICS` names `[rw.TRAIN_BEGIN_DATE, rw.TRAIN_END_DATE)`; the two role refusals raise
  `TrainWindowError(FitError, rw.SealError)` naming TRAIN end, window id and seal; docstring. `HOLD_BEGIN_NS`
  (2022-01-01) stays: it is the v3-admit-v1 FIT/HOLD split, not a window bound.
- `atx-impl/tools/alpha_report_card.py`: docstring; its check is `fcw.RoleManifest`, so it raises the same error.
- `atx-engine/tools/prepare_recent_research.py`: `SEAL = rw.SEAL`; `--end` default `rw.SEAL_DATE`; seal refusals in
  `prepare_cache` and (new) `cache_receipt` before the payload is hashed; `UNIVERSE_PIT`, `DELISTING_MARK_RULE` text.
- `atx-engine/tools/prepare_research_fields.py`: `SEAL = rw.SEAL`; role refusal `rw.SealError`; spine-year refusal;
  `SIC_STAGE_MAPPING`, `is_common` caveat and manifest `seal.rule` text from the date; docstring.
- `atx-engine/tools/prepare_identity_bridge.py`: `SEAL = rw.SEAL`; `--seal` help and later-seal refusal name the
  window; `read_role` refuses a sealed role; `DEFAULT_ROLES` drops the 2023-2024 validation role (it reaches the seal).
- `atx-engine/tools/research_fields_sec.py`: `SEC_CLOCK` text, docstring; insider seal guard (below).
- `atx-engine/tools/research_fields_holdings.py`: `SEAL = rw.SEAL`, docstring; `Stage.years` and 13F parts guards.
- `atx-engine/tools/build_fundamental_events.py`: `SEAL = rw.SEAL`;
  `LAST_SUB_QUARTER = "{}q{}".format(*rw.last_quarter_before_seal())` (2023q4 now, 2024q4 under v1); docstrings;
  `fundamental_events_schema.md` seal section.

Output strings keep their exact wording; only the date inside them changes (so the superseded binding reproduces the
old bytes, see Verification). Error texts were reworded to name the window and the date.

## PM follow-up (sealed partitions), in this commit
- `LAST_SUB_QUARTER` derived as above (the last quarter that ends before the seal).
- `research_fields_sec.py` `_insider` (current lines 576-582): a `transactions/year=Y/YqN.parquet` quarter with
  `rw.partition_is_sealed(Y, N)` is never opened; counted as `files_not_read_sealed` (key present only when > 0).
  The after-role skip is tested first, so pre-existing counts do not move.
- `research_fields_holdings.py` `Stage.years` (current lines 477-481): a sealed year is never listed, so the FTD,
  Reg SHO threshold and short-volume-ext readers never open it. 13F: `source_part_is_sealed(part)` (lines 251-257)
  skips a `parts/source=YYYYqN` data set that begins on or after the seal (lines 648-650), counted as
  `holdings_parts_not_read_sealed` (present only when > 0). With a year-start seal and a role that ends before it
  these readers never reach a sealed partition; the guards are the second line.
- Other readers checked: earnings calendar and 8-K items are single files (rows filtered by `SEAL_NS`); FINRA short
  interest is one file; short volume reads daily files up to the role's last session; the spine refuses a sealed
  year. No other date-partitioned read found.
- Tests: `test_seal_partitions.py` (holdings year readers with a sealed year written on disk and listed: never
  listed, never opened; 13F part helper; SUB quarter listing; window helpers) and
  `test_insider_sealed_quarter_present_on_disk_is_never_opened` in `test_prepare_research_fields_sec.py`.
  `build_fundamental_events` already had one (`2025q1` written as a non-parquet file under the v1 fixture).

## How root verifies

C++ (targets only; no reconfigure step needed by hand: the data glob is `CONFIGURE_DEPENDS` and the CMakeLists edit
re-runs configure):
- `atx-engine-data-tests`, filter `ResearchWindow.*:StrategyResearchRole.*`.
- `atx-impl-tests`, filter `RiskVerb.RoleReachingTheSealNeedsAnOwnerAndTheManifestNamesTheProducer:StrategyLive.*`.
- Library TUs touched: `atx-engine/src/data/strategy_data.cpp`, `atx-impl/src/strategy_risk_verb.cpp`,
  `atx-impl/src/strategy_live.cpp` (header `strategy_live.hpp` now includes `research_window.hpp`).
- CMake: `atx-engine/tests/CMakeLists.txt` sets `COMPILE_DEFINITIONS ATX_RESEARCH_WINDOW_JSON="<src>/../../atx-impl/
  strategies/research_window.json"` and `SKIP_PRECOMPILE_HEADERS ON` on `data/data_research_window_test.cpp` only
  (a per-source define; the file compiles without the data PCH and is kept out of unity batches by the property).

Python (run here, synthetic only):
- `pytest atx-engine/tools atx-impl/tools` in ONE process: see the test line in the final reply.
- `pytest atx-engine/tools/test_research_window.py`: 6 passed.

Grep gate (`2023-01-01|2025-01-01|1_672_531_200|1'735'689'600` over the brief's paths): production code is clean.
Remaining hits are test fixtures and one comment: `atx-engine/tools/conftest.py:5` (comment citing the superseded
seal), `test_build_fundamental_events.py:170,471` (v1 fixture seal instants, run under the v1 binding),
`test_prepare_research_fields_sec.py:44` (the 2025-01-01 NYSE holiday in a holiday list) and `:608` (a calendar range).

Identity on the existing 3-year role (the role ends 2022-12-30, before both TRAIN ends and both seals):
- C++ research path (`read_strategy_role`, IC runner, NAV, targets): no value or byte change; only refusal texts.
- `fit_composition_weights.py`: weights values unchanged. Bytes that change: `admission.json` `rules.hold_window_ns`
  / `rules.train_window_ns` upper bound (1672531200000000000 -> 1704067200000000000), `inputs.script_sha256` (any
  edit of the file changes it; the file's own contract), and for `ew-theme-aim-v1` the `AIM_SEMANTICS` string and
  `AIM_TAG` (aim work dir `aim-<tag>` recomputes once). So `composition_weights.json` and `admission.json` SHAs change.
  Byte identity with the pre-W0-1 fitter is proven by the five git-history identity tests in
  `test_fit_composition_weights.py`, now run with the fitter bound to the superseded window (`superseded_window`).
- `atx-equity-strategy-risk risk`: manifest `seal.begin` "2023-01-01" -> "2024-01-01" (risk manifest SHA changes).
- `atx-equity-strategy-targets decide`: a deploy manifest must now carry
  `seal {policy "research-window-v2", exclusive_session "2024-01-01"}`; a research-seal-v1 block is refused
  (`pin mismatch seal`). Deploy manifests are hand-written; none is tracked.
- Field and role builders (only when rebuilt): manifest texts carry 2024-01-01 (`seal.exclusive_end`, `seal.rule`,
  `SEC_CLOCK` inside the sec fields' clock, so their `formula_id`s change; `UNIVERSE_PIT`, `DELISTING_MARK_RULE`,
  `SIC_STAGE_MAPPING`, `is_common` caveat), seal-drop counts grow, and `code_sha256` / producer fingerprints change
  (every edit does), so `--reuse` recomputes once.

## SHA pins on constant strings (root identity checks)
- `scripts/specs/v71.json` pins `inputs.reference_admission.sha256` (read as an input, not regenerated) and
  `fields.manifest_sha256` of fields v9 on the 3-year role: a rebuild of those fields would not reproduce that SHA.
- Composition weights / admission SHAs pinned downstream (IC runner `--composition-weights-sha256`, deploy
  `composition.weights_sha256`) change when `fit_composition_weights.py` is re-run.
- Risk manifest SHA (manifest `seal.begin`).
- `recent-projection-v1` (end_exclusive 2025-01-01) is refused by `cache_receipt`/`create_role`: W0-2 step 2 needs a
  projection ending at the seal first, e.g. `prepare_recent_research.py project --source <src> --out
  build-equity/recent-projection-v2 --start 2018-06-01 --end 2024-01-01`.
- Identity bridge: W0-2 step 1 (`--seal 2024-01-01`) is now the default.

## Deviations from the brief (with reasons)
1. gtest path: `atx-engine/tests/data/data_research_window_test.cpp`, not `atx-engine/tests/`. Engine tests are
   globbed per group folder; a file at the tests root is never built.
2. `atx-engine/tools/conftest.py` binds research-seal-v1 for the atx-engine/tools pytest session. With the seal at
   2024-01-01, 86 existing field/role-builder tests (fixtures dated Oct-Dec 2024 and 2025) failed; shifting their
   dates cannot keep weekdays, holidays and month ends at once. Under the binding they run exactly as written (the
   linked-operating golden SHAs still match, proving the refactor is byte-preserving). The repository window is
   pinned in fresh interpreters by `test_research_window.py`. atx-impl tools load their own instance through
   `engine_tools.py`, so a mixed run keeps research-window-v2 there. Lanes C-3 and F-1 (new tests on the same
   fixtures) benefit; a new engine-tools test that needs the repository window in-process must use a subprocess.
3. `kSealBeginDate`, `SealError` and the helpers are additions the call sites need; the brief's names are unchanged.
4. `fit_composition_weights.py` refusals raise `TrainWindowError` (a `FitError` and a `ValueError`) so the CLI still
   refuses cleanly (exit 1) and the brief's `ValueError` holds.
5. `strategy_data_test.cpp` now includes the moved fixture (no logic change).
6. Manifest stat keys `rows_available_on_or_after_2025_dropped` / `rows_on_or_after_2025_skipped` keep their names
   (schema stability; tests key on them) but count rows on or after the current seal. Rename in a later schema bump.

## Cross-lane edits
- `research_fields_sec.py` (lane C-3): lines 21, 36 (import), 75-77 (`SEC_CLOCK`), 576-582 (insider loop).
- `research_fields_holdings.py` (lane C-3): lines 14-15, 36 (`import re`), 43 (import), 49 (`SEAL`), 251-257
  (`source_part_is_sealed`), 477-481 (`Stage.years`), 648-650 (13F parts loop).
- `prepare_research_fields.py` (C-3, F-1): lines 48-49, 93-94 (import), 99 (`SEAL`), 264 (caveat), 661-663 (Role),
  1363 (spine), 1570 (`SIC_STAGE_MAPPING`), 2563-2564 (manifest seal rule).
- `prepare_recent_research.py` (F-1 touches line 71): import at 69, `SEAL` now at 72; 102, 150, 241-243, 367-370,
  684 (comment), 925-926 (`--end`).
- `fit_composition_weights.py` (lane C): lines 20-23, 40, 58, 152-153 (import), 229, 235-237, 268-278, 424-426, 433.
  Lane C adds a `sys.path` insert for atx-engine/tools; it coexists with `engine_tools.py`.
- Tests: `test_fit_composition_weights.py` (`superseded_window` + five identity tests, post-TRAIN refusal),
  `test_prepare_research_fields_sec.py` (insider assertions and one new test), `strategy_live_test.cpp`,
  `strategy_risk_model_test.cpp`.
- `atx-impl/tools/backtest_integrity.py` (TRAIN end constant) is lane EV's, not touched here.

## Open risks
- C++ is written, not compiled (lane rule). Watch `strategy_live.hpp` (`constexpr const char* = sv.data()`), the
  per-source define quoting in CMake, and the moved fixture header.
- Anything that imports `research_window` by its plain name from atx-impl in the same pytest process as
  atx-engine/tools tests would see the v1 binding; use `engine_tools.research_window`.
- The field-reuse fingerprint sees `SEAL = rw.SEAL`, not the date value: a later window change alone does not
  invalidate `--reuse` (the formula ids of sec fields and the manifest seal do change).
