# Lane A3 re-review 1 (fix round 1, PM ruling A3-FIX1)

## Verdict
**APPROVE.** S-1, S-3 and S-5 are RESOLVED. The flip placement is exact. The fix diff adds no Required finding. There
are two new Suggested findings (wording; one guard edge case nothing can reach) and three notes for root.

## Reviewed range and method
- **Branch.** `feat/p9-a3-20261003` in `C:/atx-wt/pool-12`. HEAD was `50cb1571` and the tree was clean before and
  after.
- **Previous review.** `56474bbf`, at `f7941ab0`.
- **Fix range** `f7941ab0..50cb1571`:
  - `56474bbf`: the review;
  - `2b7af4fa`: revert of the flip `6262224f`;
  - `69743545`: S-1;
  - `165b794b`: S-3;
  - `a217fd7e`: the report;
  - `50cb1571`: the flip, re-applied.
- **Method.**
  - Read the code diff (`git diff f7941ab0 50cb1571 -- atx-engine`, written to a scratch file in this directory and
    deleted afterwards) and the whole of `vendor_panel.{hpp,cpp}` at head.
  - Read the receipts `p9-a3-d/-e` and the UTF-16 build logs `build-equity/mega-p9-a3-{d,e}-build.log`, decoded.
  - Ran the pytests listed in "Evidence" on synthetic fixtures only.
  - Built nothing (the lane's tree is gone). Opened no real data. Edited no lane file.
- **Commit position.** This file is committed **on top of the flip commit `50cb1571`**. Root merges by SHA:
  - first `a217fd7e`;
  - then `50cb1571`, only after the TRAIN identity run.

  This review commit is docs only. Root can merge it with the flip or cherry-pick it. Merging "the branch tip" before
  the TRAIN run would bring the flip in early.

## S-1 (sealed values in a straddling row group): RESOLVED

### Every path, at head
| Path | What a sealed row reaches | Evidence |
|---|---|---|
| Pruned group (date statistics start at or after the seal) | Nothing is read. Only metadata row counts are used. | `vendor_panel.cpp:367-371` |
| Read group: keys | Only the date is examined. A sealed row is counted (`rows_sealed_dropped`, local `sealed`) and dropped by `continue` before its id, the window, the role line or the calendar are looked at. | `:201-206` |
| Read group with no survivor | Value columns are never decoded. | `:309-311` |
| Read group with a survivor | The value chunks decode whole, sealed rows included. `rows_sealed_value_decoded += sealed` (`:316`). `value_columns` checks only type and length (`chunk_of` `:159-161`, whose error text names a column only). `observation` reads only `Selected::index` of surviving rows (`:262-285`). The `values` / `columns` locals are released on return. | `:316-324` |
| Assembler / finish (matrices, A9, C-81, duplicates, factor-break) | These see only surviving observations. `first_above` uses `axis_.days[o.row]` of a surviving row. | `:447-529` |
| Receipt | `scan_json` emits counts, the axis and the source pins. No cell value. | `vendor_fields.cpp:204-249` |
| Error text / prints | `refused()` / `from_arrow()` carry column names or an Arrow status. A failed decode aborts the load. The CLI prints only error messages and field counts (`research_fields_cli.cpp:229-284`). There is no debug print or log anywhere in `research/fields`. | grep of `cerr`/`cout`/`printf`/log over `src/research/fields` |
| Other C++ readers of the vendor file | None. `ReadRowGroup` / `OpenFile` occur only in `vendor_panel.cpp:145,346`. | grep |

- **Hash.** The SHA-256 pass reads the raw bytes, sealed bytes included. That is a byte hash, not a decode, and it is
  the role's pin. Python does the same.
- **Python parity.** Python's `rows_on_or_after_seal_skipped` counts `d >= seal` over every row and selects
  `[first, last]` with `last <= seal - 1`. The explicit `continue` changes no count:
  - `research_fields_price.py:312-313`;
  - `research_fields_ohlc.py:141-142`.
- **Conclusion.** No decoded sealed value reaches a computation, statistic, message or output on any path.

### The new gtest `SealedValuesOfAStraddlingGroupReachNothing` has teeth
- **One straddling group.** Each case is a single row group (`WriteTable` chunk `1 << 20`). The case is stable-sorted
  by day, so the five sealed rows are indexes 8-12 and the clean rows 0-7 sit at the clean file's indexes. The test
  pins `row_groups == 1`, `pruned_sealed == 0`, keys and values decoded once, and
  `rows_sealed_dropped == rows_sealed_value_decoded == 5` (1 + 3 + 1 rows dated 2024-01-01/02/03, seal `2024-01-01`
  `research_window.hpp:13`).
- **Any misread changes a compared quantity.** The comparison covers bit-equal matrices (factor f64, close, shares,
  price_open, three bars, `first_above`), five statistics, repaired / kept-gap steps and six payload files. A read of
  any sealed index into a clean cell would change at least one of them:
  - indexes 8-10 carry factor 1e300, close -5, volume -1 and shares 2e8, all above the 1e8 A9 ceiling
    (`vendor_panel.hpp:57`);
  - index 11 has open 10.0 against the clean 10.1;
  - index 12 has shares 2e8.
- **The "inside" control is real.** It shows that the instrument sees this poison: union_request carries ceq's 1,260+
  pre-sessions, so 2023-12-25 lies inside the window and off the NYSE calendar.
- **Limit (not a defect).** The role ends 2023-12-29, so the window clamp `min(role last, seal - 1)` would also drop
  the sealed rows. The test therefore pins the end-to-end property and the counters, not the explicit `continue`
  alone. The report says this honestly: the pre-fix code also dropped them, via the window.

### The 1 -> 2 duplicate-key correction is right
Inside poison, `poison(2023-12-25, 2023-12-28, 2023-12-29)`, on the clean rows (ids 1 and 2, 2023-12-26..29):

| Poison row | Fate | Cell count |
|---|---|---|
| (12-25, id 1) bad | off calendar (Christmas): `rows_off_calendar` 1, never added | none |
| (12-28, id 1) bad x2 | added | (12-28, id 1) = clean + 2 = **3** |
| (12-28, id 3) | id not on the role, dropped uncounted | none |
| (12-29, id 2) shares 2e8 | added | (12-29, id 2) = clean + 1 = **2** |

- **Duplicate keys:** 2 cells with a count > 1 (`finish` `:482-497`), so **2**. A count of 1 misses (12-29, id 2).
- **A9 rows:** 3 (the two copies and the 12-29 row).
- **C-81 lines:** 2.

  Both match the test.
- **No old expectation edited.** In the pre-existing tests the diff changes only the `SealPushDown` comment and adds
  `EXPECT_EQ(rows_sealed_value_decoded, 0U)`.
- **The receipts agree.** `p9-a3-e` recompiled exactly one TU, `research_fields_vendor_panel_test.cpp`: the
  test-only correction between `d` and `e`.

### Claims at head
These are literally true:
- `vendor_panel.cpp:192-193`, `:312-315`;
- `vendor_panel.hpp:97-103` (both counters);
- the test header bullets 2-3;
- `make_vendor_panel_fixture.py:13-17`;
- report items 1-5 of "S-1".

Three phrasings are broader than the code. None says that a sealed value goes undecoded when it is decoded: S-R1.

## S-3 (the panel guard passes on nothing): RESOLVED
- **What `same_panel` now refuses** (`prepare_research_fields_engine.py:268-274`):
  - non-dict Python statistics;
  - Python statistics missing any `PANEL_KEYS` key;
  - an engine entry whose `source_checks` or `source` is missing, `None`, non-dict or empty.
- **Partial engine statistics.** A partial engine block with complete Python statistics is refused by the loop,
  because `got.get(k)` is `None` and the Python value is an int.
- **Every engine field passes through the guard.** `engine_call` (`:293-296`) and `bar_rows` (`:356-358`) call it per
  built entry. The reused `pending` entry was guarded when it was built. A field missing from the receipt is refused
  earlier by `build_fields`, which requires the names to match.
- **Probe** (synthetic dicts, my scratch script):
  - complete and equal: passes;
  - engine missing `rows_selected`: refused;
  - engine block non-empty but unrelated: refused;
  - `entry=None`: `AttributeError`, which still fails closed.
- **One input shape still passes** (S-R2): every `PANEL_KEYS` value `None`, with a non-empty engine block that lacks
  them. Both Python panels initialise all five keys to int 0 and only add to them (`research_fields_price.py:306-349`,
  `research_fields_ohlc.py:135-164`), so no real run produces it.
- **The pytest.** `test_panel_guard_fails_closed_on_missing_statistics` covers `{}`, `None`, partial and rule-only
  Python records, four empty engine shapes, and the price and ohlc accept cases.

## S-5: RESOLVED
`task-A3-report.md:113-118` now attributes the lerp and the -1 marking to `eccf6338` and the contradicting expectation
to `e812a0fd`. This matches review section 1.

## Flip placement: VERIFIED
- **Registry blob.** `atx-engine/tools/field_registry.json` has the same blob (`5d2954ec`) at `6262224f`, `f7941ab0`
  and `50cb1571`. `git diff 6262224f 50cb1571 -- field_registry.json` is empty (0 bytes).
- **Patch identity.** `git patch-id --stable` gives `6262224f` = `50cb1571` = `b6dbcf84...`, so the content is
  identical, pin pytest included.
- **Pre-flip state at `a217fd7e`.**
  - The registry blob is `793a3081`, the same as `27c22eb3` (pre-flip), with 3 engine rows: `si_shares`, `si_dtc`,
    `vol_126`.
  - `test_vendor_engine_path.py` at `2b7af4fa` has the same blob as at `27c22eb3`.
  - `git diff a217fd7e 50cb1571` touches only the registry and the pin test.
  - `git diff 27c22eb3 a217fd7e -- atx-engine/tools atx-engine/tests/fixtures` holds only the S-1 / S-3 edits.
- **Conclusion.** Root can merge `a217fd7e`, then `50cb1571` after the TRAIN run.

## Findings
| # | path:line | Tag | Problem | Fix |
|---|---|---|---|---|
| S-R1 | `atx-engine/tests/research/research_fields_vendor_panel_test.cpp:4`, `:767`; `atx-engine/include/atx/engine/research/fields/sources/vendor_panel.hpp:19`, `:22`; `task-A3-report.md:441` | Suggested (wording) | Four claims are broader than the code. **(a)** `:4` says "a row group whose dates all fall on or after the seal is never read". The code prunes on date **statistics** (`vendor_panel.cpp:366-367`), so a statistics-less, wholly sealed group has its keys decoded (never its values). The header `:15` and the report `:416` say this precisely. **(b)** `:767` and the report `:441` say "every other statistic ... is the clean file's"; commit `69743545` says "every statistic but the sealed counts". `rows_in_file` and `rows_keys_decoded` are 13 against 8 by design: they count the file's rows and the decoded keys. **(c)** hpp `:22` says "dropped at decode". The values are decoded with the chunk and held until `scan_row_group` returns; hpp `:101` and cpp `:312-315` say this correctly. **(d)** hpp `:19` says "(and counted)" (pre-existing): rows off the window or off the role's lines are dropped uncounted. | (a) "whose tradingDate statistics start on or after the seal". (b) "every value-derived statistic". (c) "decoded with the chunk and released unread". (d) Count only sealed and off-calendar rows in the claim. Next touch of these files (wave 3 / A4). Not merge-blocking. |
| S-R2 | `atx-engine/tools/prepare_research_fields_engine.py:268` | Suggested (minor) | The precondition checks that `PANEL_KEYS` are present, not that they are non-`None`. Python statistics with all five `None`, plus a non-empty engine block without them, pass through the `None` skip at `:277` (shown with synthetic dicts). No real run produces this: both panels initialise the five to int 0. | `any(stats.get(k) is None for k in PANEL_KEYS)`. Wave 3. |

There are no Required findings.

## Notes for root (not counted against this round)
- **N-1. Pre-existing, outside the fix range; fails closed.** Any `--engine-exe` run that routes an open return while
  the same Python `compute` does not want `ceq_iss_5y` is refused by the guard's unchanged loop:
  - The Python price panel records `shares_rows_above_a9_ceiling: 0` even without shares
    (`research_fields_price.py:306-307`; shares are loaded iff `ceq_iss_5y` is wanted, `:757-758`).
  - The engine emits that key only when shares are requested (`vendor_fields.cpp:227`), so the guard compares 0 with
    `None`. My synthetic probe confirms the refusal.
  - This widens review-1 root item 7 to any list with an open return but no `ceq_iss_5y`. Under `--reuse`, rebuilding
    an open return while `ceq_iss_5y` is reused (the S-2 shape) is refused too.
  - The identity run is unaffected if v15's list contains `ceq_iss_5y`. For A4: make the comparison `None`-aware for
    the shares keys.
- **N-2. Build logs are UTF-16.** A byte grep finds 0 "warning" lines. Decoded, `mega-p9-a3-d-build.log` (89 TUs) has
  11:
  - 7 clang-cl "`/MP` unused";
  - 2 `databento-cpp` `getenv` deprecations;
  - 2 "1 warning generated".

  None is in an A3 TU, so "0" in the report means 0 in lane code. `mega-p9-a3-a-build.log` carries the same 11, and
  review 1's "0 warning lines" for a/b/c should be read the same way.
- **N-3. Root's merge build is binding.** Receipts `p9-a3-d` / `-e` are `2b7af4fa` plus 7 dirty entries: exactly the
  7 files of `69743545` + `165b794b`. The committed head was never itself built, and the lane gtest log (66/66) was
  not kept.
  - The source agrees with 66 tests in 13 suites (19 under the A3 filter), counted from the target's 11 test files.
  - Root rebuilds from `a217fd7e` and runs the review-1 section 6 filters.
  - On the TRAIN run, root reads `source_checks.<field>.source.rows_sealed_value_decoded`.

## Evidence (re-run here, no `ATX_RESEARCH_FIELDS_EXE`, `PYTHONDONTWRITEBYTECODE=1`, `-p no:cacheprovider`, scratch basetemp)
| Command | Tree | Result |
|---|---|---|
| `pytest atx-engine/tests/fixtures/research_fields/test_vendor_engine_path.py` | head `50cb1571` (post-flip) | 6 passed, 3 skipped (5 + the new S-3 test; skips = real exe) |
| `pytest .../test_vendor_panel_fixture.py` | head | 3 passed (the generator, with the docstring edit, reproduces every committed byte) |
| `pytest atx-engine/tools/test_field_registry.py` | head | 35 passed |
| `pytest .../test_vendor_engine_path.py` | `git archive a217fd7e` (pre-flip, 3 engine rows) | 5 passed, 3 skipped |
| `pytest atx-engine/tools/test_field_registry.py` | `git archive a217fd7e` | 35 passed |
| `same_panel` probe (synthetic dicts) | head | as listed under S-3 / S-R2 / N-1 |

- **Scratch I created and deleted.** `a3-rereview1-diff.txt` (this directory), plus my scratchpad folder
  `a3rr1/` (archive, basetemps, probe script).
- **Left alone.** The `.mypy_cache/` directories in the tree predate this review and are not mine.
- **Builds and processes.** I started no build and left no process running.
