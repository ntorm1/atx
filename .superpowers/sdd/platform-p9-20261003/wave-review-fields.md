# Wave review: fields (GATE-1 step 8; RESUME 11, 2026-10-15)

- **Range:** BASE `3c9eeb01` .. HEAD `d51d011e`, branch `feat/platform-p9-20261003` (pool-2). Every file was read
  by SHA (`git show d51d011e:<path>`, `git diff 3c9eeb01..d51d011e`). pool-2's own HEAD moved to `475f5d8f` during
  the review; nothing there was read.
- **Area:** fields (precedence 1 of `r11-dispatch.md` "WAVE REVIEW").
- **Verdict: APPROVE.** Required 0 (I 0, M 0). Suggested 6.
- **Reviewer:** fresh, read-only in pool-2. I made no git write, no build and no edit there; the only file written
  in pool-2 is this one, uncommitted (root commits it with `git add -f`). The Python tests ran on a `git archive`
  copy of `d51d011e` in the session scratchpad (`.../scratchpad/wave-fields/tree`). Real-exe tests used pool-2's
  `build-equity/bin/atx-research-fields.exe`, read-only, SHA-256 `d14346e836923cc8dea8ebd05b9769f0449b44e0f9f9150866f31d5a2e875cde`.
  That hash equals `build-equity/mega-p9-3c-receipt.json` (source `bba4c06f`, code dirty 0), so EXE-CUR holds. No
  fields source changes between `bba4c06f` and `d51d011e`; the only change in the area is 2 lines of
  `research_store/make_store_tree.py`.

## Lanes whose code lands in these files

| lane | what is in the range | state of record |
|---|---|---|
| A3 (wave 2, M2a) | the vendor panel, factor-break-v1, the NYSE calendar, the six vendor-panel kinds; the six registry rows flipped to `kind engine` | TRAIN identity 6 / 6 (`root-wave2-merge-report.md:167-200`) |
| AL-SIG (wave 2, M2b) | the 13F and Russell stage readers; the engine-only kinds `gia_13f` and `russell_recon` (DEC-5); two appended registry rows; three registrations | `russell_recon` is not point in time (ALSIG-7) |
| A4 (wave 3, M5) | pre-flip head `6dab6948`: the session clock, the one short-side reader, the FTD and Reg SHO builders and kinds, the run payload, reuse rule 6 and A3-3, the holdings fixture and gtests | the A4 files at `d51d011e` equal `6dab6948` (`git diff --stat` empty). The flips `69141218` / `59d807ea` are not ancestors of the HEAD (A4-P10-FLIPS). The first application `77bb9428` / `abba4b1d` is in history, undone by its reverts `59217cb1` / `ca6e8089`. The fix-1 tests are not in the range (A4-FIX1-P10). |
| T2 (wave 3, M2) | `atx-engine/tools/test_research_fields_holdings.py`: the holdings world re-dated by one year | review APPROVE; the holdings fixture writers come from this file |
| ABS | no file in the area: the diff of merge `898bf120` over the area paths is empty | its field-side behaviour belongs to the IC reviewer. Absent members still load and verify their field payloads (ABS report, P10 option). |
| others, fixtures only | B2 (`eval_tie`, `research_eval_edges`, `research_ledger`), SQL1-SQL3 (`research_store`), D2 (`research_composition/fit_v1.json`) | judged only on (4) and (5) |

## Files reviewed (207)

Two notes on how the area was read:
- The dispatch's glob `atx-engine/include/atx/research/fields*` matches nothing. The headers live under
  `atx-engine/include/atx/engine/research/fields/`, and I reviewed those.
- No field registry row lives under `scripts/specs/`. The registry is `atx-engine/tools/field_registry.json`, so I
  reviewed it, together with AL-SIG's three field registrations under `scripts/specs/p9/`.
- `scripts/specs/p9/kinds/new-field.json` is D3's kind file (W3-8a). It goes to the Python reviewer.
- The `.superpowers/**/a4-evidence`, `m2g-logs` and `m5-logs` holdings logs that match `*holdings*` are records,
  read as evidence only.

| file | +/- in range | lines at HEAD |
|---|---|---|
| `atx-engine/include/atx/engine/research/fields/build_spec.hpp` | +26/-0 | 56 |
| `atx-engine/include/atx/engine/research/fields/gia_13f_field.hpp` | +159/-0 | 159 |
| `atx-engine/include/atx/engine/research/fields/holdings_fields.hpp` | +281/-0 | 281 |
| `atx-engine/include/atx/engine/research/fields/registry.hpp` | +38/-4 | 130 |
| `atx-engine/include/atx/engine/research/fields/research_fields_cli.hpp` | +11/-4 | 99 |
| `atx-engine/include/atx/engine/research/fields/reuse.hpp` | +16/-5 | 85 |
| `atx-engine/include/atx/engine/research/fields/run_payload.hpp` | +45/-0 | 45 |
| `atx-engine/include/atx/engine/research/fields/russell_recon_field.hpp` | +85/-0 | 85 |
| `atx-engine/include/atx/engine/research/fields/session_clock.hpp` | +159/-0 | 159 |
| `atx-engine/include/atx/engine/research/fields/sources/factor_break.hpp` | +98/-0 | 98 |
| `atx-engine/include/atx/engine/research/fields/sources/nyse_calendar.hpp` | +60/-0 | 60 |
| `atx-engine/include/atx/engine/research/fields/sources/parquet_columns.hpp` | +53/-0 | 53 |
| `atx-engine/include/atx/engine/research/fields/sources/russell_proxy.hpp` | +78/-0 | 78 |
| `atx-engine/include/atx/engine/research/fields/sources/short_side.hpp` | +123/-0 | 123 |
| `atx-engine/include/atx/engine/research/fields/sources/stage.hpp` | +114/-0 | 114 |
| `atx-engine/include/atx/engine/research/fields/sources/thirteenf.hpp` | +205/-0 | 205 |
| `atx-engine/include/atx/engine/research/fields/sources/vendor_panel.hpp` | +218/-0 | 218 |
| `atx-engine/include/atx/engine/research/fields/vendor_fields.hpp` | +125/-0 | 125 |
| `atx-engine/src/research/fields/gia_13f_field.cpp` | +334/-0 | 334 |
| `atx-engine/src/research/fields/gia_13f_skill.cpp` | +144/-0 | 144 |
| `atx-engine/src/research/fields/holdings_ftd.cpp` | +349/-0 | 349 |
| `atx-engine/src/research/fields/holdings_regsho.cpp` | +379/-0 | 379 |
| `atx-engine/src/research/fields/holdings_specs.cpp` | +115/-0 | 115 |
| `atx-engine/src/research/fields/registry.cpp` | +141/-1 | 369 |
| `atx-engine/src/research/fields/research_fields_cli.cpp` | +106/-23 | 526 |
| `atx-engine/src/research/fields/reuse.cpp` | +88/-6 | 342 |
| `atx-engine/src/research/fields/run_payload.cpp` | +74/-0 | 74 |
| `atx-engine/src/research/fields/russell_recon_field.cpp` | +244/-0 | 244 |
| `atx-engine/src/research/fields/session_clock.cpp` | +131/-0 | 131 |
| `atx-engine/src/research/fields/sources/factor_break.cpp` | +189/-0 | 189 |
| `atx-engine/src/research/fields/sources/nyse_calendar.cpp` | +171/-0 | 171 |
| `atx-engine/src/research/fields/sources/parquet_columns.cpp` | +252/-0 | 252 |
| `atx-engine/src/research/fields/sources/russell_proxy.cpp` | +120/-0 | 120 |
| `atx-engine/src/research/fields/sources/short_side.cpp` | +275/-0 | 275 |
| `atx-engine/src/research/fields/sources/stage.cpp` | +273/-0 | 273 |
| `atx-engine/src/research/fields/sources/thirteenf_filings.cpp` | +319/-0 | 319 |
| `atx-engine/src/research/fields/sources/thirteenf_holdings.cpp` | +267/-0 | 267 |
| `atx-engine/src/research/fields/sources/vendor_panel.cpp` | +574/-0 | 574 |
| `atx-engine/src/research/fields/vendor_fields.cpp` | +567/-0 | 567 |
| `atx-engine/tests/fixtures/eval_tie/frozen_statistics.py` | +556/-0 | 556 |
| `atx-engine/tests/fixtures/eval_tie/generate_eval_tie.py` | +9/-6 | 411 |
| `atx-engine/tests/fixtures/research_eval_edges/generate_research_eval_edges.py` | +243/-0 | 243 |
| `atx-engine/tests/fixtures/research_eval_edges/test_research_eval_edges_fixture.py` | +81/-0 | 81 |
| `atx-engine/tests/fixtures/research_fields/make_vendor_panel_fixture.py` | +307/-0 | 307 |
| `atx-engine/tests/fixtures/research_fields/test_research_fields_engine_path.py` | +4/-2 | 281 |
| `atx-engine/tests/fixtures/research_fields/test_vendor_engine_path.py` | +265/-0 | 265 |
| `atx-engine/tests/fixtures/research_fields/test_vendor_panel_fixture.py` | +65/-0 | 65 |
| `atx-engine/tests/fixtures/research_fields_al_sig/make_al_sig_fixture.py` | +532/-0 | 532 |
| `atx-engine/tests/fixtures/research_fields_holdings/make_holdings_fixture.py` | +112/-0 | 112 |
| `atx-engine/tests/fixtures/research_fields_holdings/test_holdings_engine_path.py` | +137/-0 | 137 |
| `atx-engine/tests/fixtures/research_fields_holdings/test_holdings_fixture.py` | +52/-0 | 52 |
| `atx-engine/tests/fixtures/research_ledger/generate_research_ledger.py` | +214/-0 | 214 |
| `atx-engine/tests/fixtures/research_ledger/test_research_ledger_fixture.py` | +49/-0 | 49 |
| `atx-engine/tests/fixtures/research_store/digest_oracle.py` | +74/-0 | 74 |
| `atx-engine/tests/fixtures/research_store/make_py_fixture.py` | +85/-0 | 85 |
| `atx-engine/tests/fixtures/research_store/make_store.py` | +87/-0 | 87 |
| `atx-engine/tests/fixtures/research_store/make_store_tree.py` | +338/-0 | 338 |
| `atx-engine/tests/research/research_fields_a4_clock_test.cpp` | +168/-0 | 168 |
| `atx-engine/tests/research/research_fields_a4_reuse_test.cpp` | +189/-0 | 189 |
| `atx-engine/tests/research/research_fields_al_sig_fixture_test.cpp` | +378/-0 | 378 |
| `atx-engine/tests/research/research_fields_al_sig_gia_test.cpp` | +212/-0 | 212 |
| `atx-engine/tests/research/research_fields_al_sig_russell_test.cpp` | +227/-0 | 227 |
| `atx-engine/tests/research/research_fields_al_sig_support.hpp` | +61/-0 | 61 |
| `atx-engine/tests/research/research_fields_al_sig_thirteenf_test.cpp` | +283/-0 | 283 |
| `atx-engine/tests/research/research_fields_holdings_test.cpp` | +709/-0 | 709 |
| `atx-engine/tests/research/research_fields_registry_test.cpp` | +22/-4 | 263 |
| `atx-engine/tests/research/research_fields_test_support.hpp` | +19/-8 | 118 |
| `atx-engine/tests/research/research_fields_vendor_fields_test.cpp` | +298/-0 | 298 |
| `atx-engine/tests/research/research_fields_vendor_fixture_test.cpp` | +326/-0 | 326 |
| `atx-engine/tests/research/research_fields_vendor_panel_test.cpp` | +786/-0 | 786 |
| `atx-engine/tests/research/research_fields_volume_mean_test.cpp` | +29/-12 | 201 |
| `atx-engine/tests/research/research_fields_writer_test.cpp` | +12/-1 | 249 |
| `atx-engine/tools/field_registry.json` | +106/-20 | 3434 |
| `atx-engine/tools/test_research_fields_holdings.py` | +72/-71 | 632 |
| `scripts/specs/p9/blocked/hf_crowd.json` | +37/-0 | 37 |
| `scripts/specs/p9/blocked/lazy_prices.json` | +37/-0 | 37 |
| `scripts/specs/p9/candidates/russell_recon.json` | +43/-0 | 43 |

| fixture data dir (non-code files) | files | binary bytes at HEAD | text lines at HEAD |
|---|---|---|---|
| `atx-engine/tests/fixtures/research_composition/` | 1 | 0 | 7953 |
| `atx-engine/tests/fixtures/research_eval_edges/` | 2 | 0 | 1720 |
| `atx-engine/tests/fixtures/research_fields/` (vendor/) | 16 | 304190 | 706 |
| `atx-engine/tests/fixtures/research_fields_al_sig/` | 30 | 93923 | 282 |
| `atx-engine/tests/fixtures/research_fields_holdings/` | 38 | 130559 | 1548 |
| `atx-engine/tests/fixtures/research_ledger/` | 3 | 0 | 164 |
| `atx-engine/tests/fixtures/research_store/` (schema, goldens, legacy_records, sqlite) | 11 | 90112 | 2962 |
| `atx-engine/tests/fixtures/research_store/tree/` | 29 | 0 | 542 |

In all: 77 code / spec / registry files and 130 fixture data files.

## (1) Point in time and the seal, builder by builder (by reading; identity runs in (2))

The mark is shared by every A4 and AL-SIG builder: `previous_session_marks` (`stage.cpp:102-108`) gives
mark(t) = date(t-1) 22:00 UTC, and `kBeforeAllNs` at session 0. This equals the Python `prev_marks`
(`research_fields_holdings.py:324-330` at `d51d011e`). Visibility is a strict `<` everywhere.

| builder | clock as coded (file:line at `d51d011e`) | seal | verdict |
|---|---|---|---|
| `ftd_shares_ratio21` (A4) | Each date's latest `available_at` (`holdings_ftd.cpp:72-83`), as a running prefix maximum (`:121-126`). Anchor S\*(t) = `lower_bound(prefix, mark) - 1` (`:219-226`). The window is the 21 settlement dates ending at S\* (`:194-207`). Stale if more than 60 d (`:229`). `shares_out` is taken at the last role session on or before S\* (`:238-241`). Year files run from year(first - 120 d) to year(last) (`:262-264`). Same as Python `build_ftd` `:803-889`. | Row drop first (`:90`; a null clock is `kNeverNs`, so sealed). A year that begins on or after the seal is never listed (`short_side.cpp:216-226`). | Holds. Every row of the dates up to S\* has `available_at` <= prefix[S\*] < mark, so no row stamped at or after the mark feeds a value. |
| `regsho_threshold_days63` (A4) | The prefix is the last 65 usable list dates before the role (`holdings_regsho.cpp:69-79`). List visibility = the minimum `available_at` per (market, date) (`:99-100`). A row is late against the visibility before its own file's raises (`:131-133`), and is raised at `end_file` (`:140-146`). The last row of a cell wins (`:147-149`). The window ends at session t-2 (`:219`) and spans 63 dates. A list counts iff its visibility is < mark (`:230`). The latest name row is taken strictly before the mark (`:244`) and must be 45 d old or less (`:250`). Same as Python `build_regsho` `:891-997`. | List rows `:67`, threshold rows `:122`, name rows `:168`. Threshold year files as for FTD. | Holds. Every counted on-list row has `available_at` <= its list's raised visibility < mark. See S-1 on the all-or-nothing raise. |
| clock primitives (A4) | `usable_from`: the first u with instant < mark(u-1) (`session_clock.cpp:79-82`). `LatestRows::before` is strict (`:125-129`). | n/a | Holds. Only `LatestRows` has a production caller in P9 (A4 review S-5). |
| `gia_13f` (AL-SIG) | V(q) = the maximum `available_at` over the filings of q that are on time (`thirteenf_filings.cpp:201-209`). V\*(P) = the running maximum over the quarters with positions (`gia_13f_field.cpp:181`). Row t reads the latest P with V\* < mark, P <= date(t) and an age of 150 d or less (`:221`). The skill window is quarters P-7..P (`:138`). | A quarter is sealed iff V >= seal (`thirteenf_filings.cpp:215`) and is never read (`thirteenf_holdings.cpp:143`). A part whose quarter begins on or after the seal is never opened (`:174`). | Holds. See S-6 on part names. The field feeds no P9 trial (ALSIG-6). |
| `russell_recon` (AL-SIG) | V(R) = max(prior, next) (`russell_recon_field.cpp:109`). Visible iff < mark (`:137`). NaN from the recon day (`:142`). | Row drop (`russell_proxy.cpp:53`). | Declared not point in time (ALSIG-7). The IC admission (`atx-impl/src/strategy_ic_admission.cpp:468`) and the NAV loader (`atx-impl/src/book/nav_load.cpp:61`) refuse it. No consumer at `d51d011e` (`git grep`). See S-4. |
| six vendor-panel fields (A3) | The price fields read axis rows <= prefix + t - 1 (`vendor_fields.cpp:264`, `:292`). The ohlc fields read the bar of session t (`:330`; the declared `ohlc-same-session-v1`, which is Python `LAG_SESSIONS` 0). factor-break-v1 decides from rows <= t only (`factor_break.hpp:16-17`). | A sealed row is dropped by its date first (`vendor_panel.cpp:202`). A group whose statistics start on or after the seal is pruned (`:367`). The window's last day is min(role last, seal - 1) (`:559-560`). The values of a straddling group are decoded and then dropped (OD-A3-SEAL, owner-accepted for P9). | Holds. The TRAIN identity of M2a records the seal counts in the engine receipts. |

**Not exercised in P9 (ruled, not a finding).** The four A4 seal drops have no seal gtest in P9. Three of them
(`holdings_ftd.cpp:90`, `holdings_regsho.cpp:122`, `:168`) meet no sealed row in the P9 fixture:
- The fixture's FTD `rows_sealed` is 0.
- The only sealed fixture row is T2's cboe_bzx list, which reaches the drop at `:67`.
- The late-list raise has `rows_later_than_their_list` 0 in the fixture. It is covered only by the C++ kernel test
  `RegshoLateRuleIsPerFileAcrossBatches`.

A4-FIX1-P10 puts the seal and per-clock probe gtests in P10, and A4-P10-FLIPS keeps Python as the producer of
record. `research_fields_holdings.py`, `prepare_research_fields.py`, `research_window.py` and
`research_window.json` are unchanged in the range (`git diff --stat` empty). So no P9 run reads these engine
builders.

## (2) Correctness against the registry row and the Python reference (re-derived, not taken from the reports)

- **A4.** I re-derived the identity from my own runs:
  - `test_holdings_fixture.py`: the existing Python builder, re-run, reproduces every committed expected byte.
  - `test_holdings_engine_path.py::test_real_executable_identity`, with the p9-3c fields exe: the exe's
    `ftd_shares_ratio21` and `regsho_threshold_days63` payloads equal the Python run's on the committed stages.
    Together, engine = Python on the fixture.
  - Root's log `m5-logs/gt-3c-a4.log` holds the A4 filter 25 / 25 on the p9-3c tests exe. That exe's hash
    `39be2b3d...` equals the receipt and the current bin. The 25 include `FtdIsThePythonField`,
    `RegshoIsThePythonField` and `FormulaFingerprintsAreTheRegistrys`.
  - Operation by operation, the C++ matches the Python's anchor, window, staleness, late and name rules (lines in
    the table above).
  - Registry rows: both stay `kind python`, builder `research_fields_holdings`, with unchanged formula pins
    (`cce14201...`, `7e869d7d...`). Only the owner suffix moved (field_registry.json hunks `@@ -3208`, `@@ -3244`).
- **A3.** The six rows are `kind engine` with builder = the field name; no existing `formula_sha256` changed (diff
  grep). Root's TRAIN identity gives py = engine = the v15 pins (`root-wave2-merge-report.md:183-192`). My own runs
  with the real exe passed `test_vendor_engine_path.py` and `test_vendor_panel_fixture.py`.
- **AL-SIG.** The two engine-only rows are appended, with `russell_recon` `point_in_time false` and a presence
  `non_pit_aspects` entry. The identity is the C++ against the fixture's numpy oracle (gtest, wave-2 logs). I
  regenerated the oracle: 29 / 29 files byte-identical.
- **T2.** The change is a re-date only; no hash and no test meaning moved. I re-derived these boundaries by hand:
  - 13F Q1 V = 2023-05-16 22:00, so the session of 05-17 still reads Q0 and 05-18 reads Q1.
  - Q3 V = 2023-11-15 22:00, so the switch falls between 11-16 and 11-17.
  - FTD staleness: 60 / 61 days after 2023-09-29.
  The file passes on its own (16 / 16).

## (3) House style

- **100 columns:** one C++ line over the limit in the area, `sources/parquet_columns.hpp:5` (103). This is A4
  review S-4, ruled P10 by A4-S.
- **`/W4 /WX`:** root's `mega-p9-3c-build.log` shows 33 TUs and 0 warning lines (UTF-16 log normalized).
- **No hot-path allocation:**
  - The row loops reuse buffers (`holdings_ftd.cpp:281-286`, `holdings_regsho.cpp:300-306`, `vendor_fields.cpp:260-266`,
    `gia_13f_field.cpp:258-263`).
  - Reg SHO's per-row scratch is a stack `std::array` (`holdings_regsho.cpp:226`).
  - The FTD ring holds 22 prefix rows.
- **Test placement:** the tests sit under `atx-engine/tests/research/`, listed in the research-fields target.
  `tests/research` is deliberately not a test group (`atx-engine/tests/CMakeLists.txt:365`).
- **No new Python mirror of a C++ rule:**
  - The new Python files under `tests/fixtures/` are generators and test oracles: `make_al_sig_fixture.py`'s numpy
    oracle, B2's frozen copy `eval_tie/frozen_statistics.py` (accepted in B2's re-reviews), and
    `research_store/digest_oracle.py`.
  - Every importer of them is a test (`git grep`).

## (4) Expected hashes, goldens and pinned values

- **Modified fixture files** (`git diff --diff-filter=MDR` over `atx-engine/tests/fixtures/`): only two files, and
  neither is an expected file.
  - `eval_tie/generate_eval_tie.py` (B2) now reads `frozen_statistics.py`. `eval_tie/expected.json` is unchanged,
    and regenerating writes the committed bytes (own run).
  - `research_fields/test_research_fields_engine_path.py:244-246`: A3 and A4 extended the route pin `ENGINE_FIELDS`
    for their new routes. The assertions on the routed rows are unchanged.
- **Added expected files** (generator output, nothing pre-existing replaced): `research_fields/vendor/expected/*`
  (A3), `research_fields_al_sig/expected/*`, `research_fields_holdings/expected/*`,
  `research_eval_edges/expected.json`, `research_ledger/expected.json`, `research_store/golden_{digests,rows}.json`.
- **Test expectations moved:** `research_fields_volume_mean_test.cpp` (`SumOrderIsNumpys`) and
  `research_fields_writer_test.cpp` (`QuantilesPartitionLikeNumpy`), commit `ef921abd`. Ruling M1a-RED allows this:
  the pre-wave-1 reds were fixed in wave 2 against numpy 1.26.4, with the root cause stated.
- **Registry:** the six A3 kind / builder / owner flips, two A4 owner suffixes and two appended AL-SIG rows. No
  existing `formula_sha256` or `point_in_time` changed.
- **Outside the area, cited:** `atx-engine/tests/alpha/alpha_lit_golden_test.cpp` (S2R, M2c-RED-2) goes to the IC
  reviewer.

## (5) Fixtures: regenerated by their generators only

Each fixture was regenerated from `d51d011e`'s generator in scratch and compared byte for byte:

| fixture | how | result |
|---|---|---|
| `research_fields_holdings/` | `test_holdings_fixture.py` | identical |
| `research_fields/` (base and vendor) | `test_research_fields_fixture.py`, `test_vendor_panel_fixture.py` | identical |
| `research_fields_al_sig/` | own script `scratchpad/wave-fields/alsig_identity.py` | 29 / 29 identical |
| `research_store/tree/`, `legacy_records/` | own script `store_tree_identity.py`, against the blobs | 32 / 32 identical |
| `research_store` (sqlite logical content, digest oracle) | `atx-engine/tools/test_research_store_fixtures.py` | identical |
| `eval_tie/` | `test_eval_tie_fixture.py` | identical |
| `research_eval_edges/` | `test_research_eval_edges_fixture.py` | identical |
| `research_ledger/` | `test_research_ledger_fixture.py` | identical |
| `research_composition/fit_v1.json` | `atx-impl/tools/test_composition_fit_fixture.py` | today's Python |

No fixture differs from its generator's output.

## Required findings

None.

## Suggested

| # | file:line | problem (failing input) | fix |
|---|---|---|---|
| S-1 | `holdings_regsho.cpp:131-146` (= Python `research_fields_holdings.py:929-932`); `field_registry.json` row `regsho_threshold_days63` (`clock`, `non_pit_aspects: []`) | **The late-list raise is all or nothing, and the registry text does not say so.** One line's late on-list row delays its whole (market, date) list. So the count of every other line, and the market's `covered`, can depend on a row stamped after mark(t); the direction is conservative (a count lower, or NaN through `market_lists_incomplete`). Failing input: a list (nasdaq, D), D = session t-2, its status at T0 = D+1 06:00; line Y on-list at T0; line X on-list in the same file at T1 = D+10 d; mark(t) in (T0, T1). Y's day D is not counted, though a reader at mark(t) holds both the list and Y's row. FTD's prefix rule (`holdings_ftd.cpp:121-126`) and the 13F V(P) are the same "complete set" kind of clock, but their clock texts state it. No value stamped at or after the mark is read, the rule is the unchanged Python producer of record's, and the port is faithful. | P10, when the flips land: name the per-list raise in the row's `clock` text (or as a `non_pit_aspects` entry), or adopt a per-row rule under a new formula id. Nothing to do in P9. |
| S-2 | `sources/short_side.cpp:228-256`; `holdings_regsho.cpp:273`, `:294` | **Decode-then-drop on files the owner has not accepted.** The non-partitioned `lists.parquet` and `finra_names.parquet` (and every row group of a year file) are decoded whole before the per-row seal drop: no row-group pruning on `available_at`. OD-A3-SEAL accepted decode-then-drop for the vendor file only. The Python decodes these files whole too, and no C++ run reads real short-side data in P9 (A4-P10-FLIPS). | P10, with the flips: extend OD-A3-SEAL to these files by name, or prune row groups whose `available_at` statistics start on or after the seal (as `vendor_panel.cpp:367` does). |
| S-3 | `tests/fixtures/research_fields_al_sig/make_al_sig_fixture.py`; `tests/fixtures/research_store/make_store_tree.py` | **No committed regenerate-and-compare test for two fixtures.** The AL-SIG expected payloads (the numpy oracle) and the store tree have no pytest that regenerates them and compares bytes. A hand edit of `expected/gia_13f.f64` made to fit the C++ would pass `ResearchFieldsAlSigFixture.*`. My scratch regeneration matched (29 / 29 and 32 / 32). | P10: a `test_the_fixture_is_the_generators_output` beside each, in the form of `test_holdings_fixture.py:26-32`. |
| S-4 | `scripts/specs/p9/candidates/russell_recon.json` `notes` | **A stale note.** The notes say "the field remains a do-no-harm trade-filter input for root". After ALSIG-7 the field is not point in time, and both the NAV loader and the IC admission refuse it. The registration is `dropped` and never pinned, so this is text only. | Leave the file as it is (a dropped registration; PRE-40 form). State in the freeze report that `russell_recon` is no input of any kind while ALSIG-7 stands. |
| S-5 | `holdings_ftd.cpp:25-41`, `holdings_regsho.cpp:25-50`; `sources/parquet_columns.hpp:5`; `session_clock.hpp:43-157` | **A4 review S-3 / S-4 / S-5 are still open in the range** (ruled P10 by A4-S): the duplicated `refused` / `members_of` / `records_json` helpers and the SourceRecord JSON written five times; one line over 100 columns; `SessionCalendar` / `WindowedSums` pinned by hand-written constants with no production caller. | P10 holdings slice, as ruled. Listed here so the freeze report carries them. |
| S-6 | `sources/thirteenf_filings.cpp:130-143`, `thirteenf_holdings.cpp:173-179` (= Python `source_part_is_sealed`) | **Fail-open on odd part names.** A `source_period` that is not `YYYYqN` is "not dated" and its part is opened. Failing input: a filing whose `source_period` is `2024Q1` (upper case) or `2024-q1`; the part is read although its quarter is sealed. The stage manifest still pins the bytes. | P10: refuse a `source_period` of another shape (fail closed), in both the C++ and the Python. |

## Tests run (own; seed 0: `PYTHONHASHSEED=0`, `PYTHONDONTWRITEBYTECODE=1`, `-p no:cacheprovider`, own `--basetemp` under `scratchpad/wave-fields/bt/`, `C:/Program Files/Python312/python.exe`, on the archive of `d51d011e`)

| session | files | result |
|---|---|---|
| holdings (real exe `d14346e8...`) | `research_fields_holdings/test_holdings_fixture.py`, `test_holdings_engine_path.py` | 4 passed, 0 skipped |
| research_fields (real exe) | `test_vendor_panel_fixture.py`, `test_vendor_engine_path.py`, `test_research_fields_engine_path.py`, `test_research_fields_fixture.py` | 26 passed, 0 skipped |
| tools: holdings (T2) | `atx-engine/tools/test_research_fields_holdings.py` | 16 passed |
| tools: registry | `atx-engine/tools/test_field_registry.py` | 37 passed, 8 subtests |
| tools: store fixtures | `atx-engine/tools/test_research_store_fixtures.py` | 8 passed, 18 subtests |
| fixtures | `eval_tie/`, `research_eval_edges/`, `research_ledger/` tests (one session each) | 7, 6, 4 passed |
| composition fixture | `atx-impl/tools/test_composition_fit_fixture.py` | 4 passed |
| generator identity (own scripts) | AL-SIG; store tree | 29 / 29, 32 / 32 identical |

In all: 112 pytest passed, 0 failed, 0 skipped, plus two byte-identity scripts. Every pytest invocation ran in its
own session. Logs are in `scratchpad/wave-fields/logs/`.

C++ claims come from the logs and receipts only, with no gtest exe run by me:
- the A4 filter 25 / 25 on p9-3c (`m5-logs/gt-3c-a4.log`; receipt `mega-p9-3c-receipt.json`; 0 warnings);
- the A4 lane's p9-a4-c / -e logs and teeth 5 / 5 (`a4-evidence/`);
- the first reviewer's p9-a4-g teeth (`task-A4-review.md`);
- A3's and AL-SIG's merge checks (`root-wave2-merge-report.md` "A3", "AL-SIG").

## Mutation (one; TEETH-RESTORE in the TEETH-CRLF form; tool `scratchpad/wave-fields/teeth_regsho_window.py`)

- **W1**, on the scratch copy of `atx-engine/tools/research_fields_holdings.py` (Python is the producer of record
  for `regsho_threshold_days63`). Line `:970` `e = e0 + t - 2` became `e = e0 + t - 1`: the window ends at t-1, a
  one-list-date look-ahead.
- **CAUGHT** by `test_research_fields_holdings.py::HoldingsFields::test_regsho_against_brute_force` (1 failed, 15
  passed) and by `test_holdings_fixture.py::test_the_fixture_is_the_python_builders_output` (1 failed, 1 passed).
- **Restore OK** in the `finally`:
  - raw SHA-256 of the restored file = the kept copy (`48689fd5...`);
  - `git -C pool-2 hash-object --path=<rel>` = `git rev-parse d51d011e:<rel>` = `972703a0...`, with no `-w`.
- Nothing in pool-2 was touched.

## What root must do before any cell

1. **Nothing Required from this area.** At gate step 0, commit this file (`git add -f`).
2. **At gate steps 1-2, for this area (TEST-1, deferred to the gate):**
   - Run the whole `atx-engine-research-fields-tests`, Debug and Release, on the gate's exes. This includes the A4
     filter `ResearchFieldsA4*:ResearchFieldsHoldings*:ResearchFieldsRegistry*` (25), `ResearchFieldsVendor*` /
     `ResearchFieldsAlSig*`, `ResearchFieldsVolumeMean.*` and `ResearchFieldsWriter.*`.
   - Run the fields pytests in their own sessions with the gate's fields exe as `ATX_RESEARCH_FIELDS_EXE`: the
     holdings pair, the four `research_fields/` files, and `tools/test_research_fields_holdings.py` and
     `tools/test_field_registry.py` separately.
   - EXE-CUR: the gate's builds supersede p9-3c; log each exe's SHA-256 beside its run.
3. **Freeze report, P10 list.** Carry S-1 .. S-6 beside A4-P10-FLIPS, A4-FIX1-P10 and A4-RR1-S, and state two
   facts:
   - the engine FTD / Reg SHO builders are merged without seal or per-clock probe gtests, and three of their four
     seal drops meet no sealed fixture row;
   - the Python producers of record are unchanged in `3c9eeb01..d51d011e`.
4. **Nothing to do for SQL4 / M6b** here: its files are IC and store only.

## Records

- **Blindness (disclosed):** one ledger grep (pattern `frozen`) printed `progress.md:138`, a line that carries
  2020-2023 performance figures. Nothing of it is recorded here or used.
- **Root history:** read as one-line subjects only.
- **Data:** no data dated 2024-01-01 or later was opened. The fixtures' sealed-by-design rows are synthetic and
  were only regenerated and compared. `holdout_gate.py` was not run. No lease, `main`, `atx-db/`, pool 8 / 24 or
  pool-23 `build-equity/` was touched.
- **Permission refusals:** none.
- **Slips (not refusals):**
  - The `rtk` hook rewrote a one-line Bash `for` loop into a syntax error. It was re-run as a script file.
  - `tar` read `C:` as a host name. It was re-run with `--force-local`.
- **Scratch:** `scratchpad/wave-fields/`, my own, regenerable (the archive tree, logs, tools).

## Open questions

- S-1: should the Reg SHO all-or-nothing list raise be declared in the registry before the P10 flips, or replaced
  by a per-row rule under a new formula id?
