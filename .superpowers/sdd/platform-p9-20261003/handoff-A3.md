# Handoff: P9 lane A3 (vendor panel and price / ohlc builders in C++, shim retirement)

The owner stopped the lane, so this is a work-in-progress handoff. Nothing has been built or run in C++. Lanes never
build C++, so every C++ file here is unverified until root compiles it.

| | |
|---|---|
| Pool | `C:/atx-wt/pool-12` (reused) |
| Branch | `feat/p9-a3-20261003` |
| Base | `1239a5ff` (P9 integration, after the D1 merge) |
| Head | `81cef7ba` `wip(A3): ...`, on top of `ef921abd` (M1a-RED) |
| Contract | `C:/atx-wt/pool-2/.superpowers/sdd/platform-p9-20261003/wave2-lane-dispatch.md` (LANE = A3), `brief-A3.md`, `wave2-carry.md` |
| Estimate | about 60% done |

## Status by item

### Done

**M1a-RED, the two `ResearchFields*` gtest fixes.** Committed on their own in `ef921abd`. In both cases the test was
wrong and the code was right. No library code changed.

- `research_fields_writer_test.cpp` (quantiles of `{-0.0}`). The hand-reasoned expectation of `+0.0` is wrong.
  numpy 1.26.4 `_lerp` computes `a + diff*t`, then overwrites the result with `b - diff*(1-t)` wherever `t >= 0.5`.
  So `np.quantile([-0.0], q)` is `-0.0` at all five quantiles. For the two values `[-0.0, -0.0]` the result is
  `[+0, +0, -0, -0, -0]`. Both values were measured with numpy, and the two-value case was added as a test.
- `research_fields_volume_mean_test.cpp` (`SumOrderIsNumpys`). The test fed `-1e16` volumes. The vol_126 spec accepts
  only finite volume `>= 0`, so the code was right to reject them.
  - The test now uses accepted volumes, with expectations computed by numpy. numpy sums in slot order for two or
    more columns and pairwise for one column.
  - New test `NegativeVolumeIsNotAccepted`.

**No registry flip.** `field_registry.json` is untouched. Every row of the six fields is still `kind: python`.

**C++ library (in `81cef7ba`).** Every file below is new.

- `include/.../research/fields/sources/nyse_calendar.hpp` and `src/.../sources/nyse_calendar.cpp`:
  - rule nyse-rule-v1, including the special closures;
  - `extended_axis`, a port of `research_fields_price.extended_days`;
  - `session_calendar_pin`.
- `sources/factor_break.{hpp,cpp}`:
  - `factor_breaks_v1`, a port of `prepare_research_fields.factor_breaks`;
  - `KeptGaps::crosses`, which matches the Python `Gaps.crosses`.
- `sources/vendor_panel.{hpp,cpp}`, the `VendorPanel` class. `load()` works in this order:
  1. hash the file once and check it against the role's `source_sha256`;
  2. check the file stamp before and after;
  3. check the column types;
  4. scan the row groups once over the union of requested columns, with the seal pushed down to row-group
     statistics:
     - a group whose minimum date is on or after the seal is never read;
     - a group that straddles the seal has only its keys decoded; its sealed rows are counted and its values are
       never decoded;
     - a group wholly outside the window and before the seal is pruned;
  5. apply the observation contract once;
  6. quarantine duplicate keys in every matrix;
  7. run factor-break-v1 once and divide each repaired step out of the factor.

  `VendorPanel::Assembler` is a public back end, so tests can build panels from rows. `to_f32` narrows f64 to f32
  the way numpy `astype(float32)` does.

  In the stats, `rows_sealed_dropped` counts every sealed row: the rows of pruned sealed groups plus the rows dropped
  by key. It equals Python's `rows_on_or_after_seal_skipped`.
- `vendor_fields.{hpp,cpp}` ports six fields:
  - from `research_fields_price.py`: `ret_overnight`, `ret_intraday`, `ceq_iss_5y`;
  - from `research_fields_ohlc.py`: `open_adj`, `high_adj`, `low_adj`.

  The row kernels are public (`open_return_row`, `ShareIndex`, `ceq_issuance_row`, `ohlc_bar_row`) so that the
  planted-leak probes can drive them with a shifted clock. The spec texts are verbatim copies of the registry
  `spec_text`. The Python fingerprints are checked by hand:

  | Field | Fingerprint prefix |
  |---|---|
  | `ret_overnight` | `e5248a03` |
  | `ret_intraday` | `63f2284a` |
  | `ceq_iss_5y` | `ad3068dd` |
  | `open_adj` | `2853374e` |
  | `high_adj` | `9a4e3fba` |
  | `low_adj` | `00069d88` |

**Builder-kind wiring (cross-lane, in `81cef7ba`).**

- `registry.hpp`:
  - `SharedSources::vendor_panel`;
  - `BuildContext::plans`, the span of every plan in the run, used to build the union request;
  - an updated comment.
- `registry.cpp`: `parse_vendor`, `ensure_vendor_panel` (loads the panel once with the union of all vendor plans),
  `build_vendor_kind`, and `kKinds` grown from 3 to 9 entries, with the six new kinds appended.
- `build_spec.hpp`: `std::optional<path> price_source`.
- `research_fields_cli.{hpp,cpp}`: a new optional spec key `price_source`, and `BuildContext ctx{spec, role, {}, plans}`.
- `atx-engine/CMakeLists.txt`: a tail `target_sources(atx-engine-research-fields ...)` with the four new `.cpp` files.

**Fixture (in `81cef7ba`).**

- Generator: `atx-engine/tests/fixtures/research_fields/make_vendor_panel_fixture.py`.
- Data: `.../research_fields/vendor/{th.parquet, role/*, expected/*}`, about 320 KB.
- The generator ran twice, into scratch and into the repo, and produced identical bytes.
- It runs the existing Python builder `run(..., price_source=th)` with the ohlc module bound under
  `mock.patch.dict(ALL_FIELDS)` / `FIELD_MODULES`.
- Python values recorded in the fixture's `source_checks.price.source`:

  | Stat | Value |
  |---|---|
  | `rows_scanned` | 5210 |
  | `rows_on_or_after_seal_skipped` | 31 |
  | `rows_selected` | 5130 (ohlc-only: 2450) |
  | `rows_off_calendar` | 1 |
  | `duplicate_keys_quarantined` | 1 |
  | `extended_sessions` | 1641 |
  | `sessions_before_role` | 1600 |
  | `first_session` | 2014-07-25 |
  | mass sessions | `[2021-01-04]` |
  | repaired steps | 56 |
  | kept_gap steps | 1 |
  | shares rows above the A9 ceiling | 1 |
  | lines withheld under C-81 | 1 |

- Row groups:

  | Group | Rows | Dates |
  |---|---|---|
  | 0 | 10 | 2010 |
  | 1-3 | main rows | 2014-06-02 .. 2022-06-03 (group size 2048) |
  | 4 | 12 | 2023-12-27 .. 2024-01-03 (straddles the seal) |
  | 5 | 25 | 2024-02 (wholly sealed) |

  The 2024 rows are synthetic seal probes.

**Test edits (in `81cef7ba`).**

- `research_fields_test_support.hpp` (cross-lane): `TinyRole` gains `close`, `raw_close` (written only when non-empty)
  and `source_sha256` (defaults to 64 zeros). The manifest text is unchanged when the defaults are used.
- `research_fields_registry_test.cpp` `KindLookup` (cross-lane): expects 9 kinds, and the vendor kinds parse with
  `price_source` set.
- `tests/CMakeLists.txt`: a tail `target_sources(atx-engine-research-fields-tests ... research_fields_vendor_panel_test.cpp)`.

### Partial: gtests

`research/research_fields_vendor_panel_test.cpp` is written but not compiled. It holds these tests:

- `ResearchFieldsNyseCalendar.RuleSessions`
- `ResearchFieldsNyseCalendar.ExtendedAxisIsThePythonAxis`
- `ResearchFieldsNyseCalendar.SessionCalendarPinIsThePythonPin`
- `ResearchFieldsFactorBreak.ClosedForm`
- `ResearchFieldsVendorPanel.DuplicateKeysAreQuarantinedEverywhere`
- `ResearchFieldsVendorPanel.ToF32RoundsAsNumpy`
- `ResearchFieldsVendorPanel.RefusesAFileThatIsNotTheRoles`
- `ResearchFieldsVendorPanel.HashOnce`
- `ResearchFieldsVendorPanel.SealPushDown`

The brief's names `VendorPanel.HashOnce`, `VendorPanel.SealPushDown` and `FactorBreak.ClosedForm` carry the
`ResearchFields` prefix here, because the target's documented filter is `ResearchFields*`.

### Not started

1. **`research/research_fields_vendor_fields_test.cpp`: planted-leak probes, one per moved builder, each with teeth.**
   - Design: one panel built through the Assembler, with request price + shares + price_open + bars,
     `pre_sessions = 1261`, `ExtendedAxis{days, 1261}`, 100 role days of consecutive calendar days from 2021-01-04,
     and 2 lines.
     - factor 1.0, close 10, open 10.1, high 11, low 9, volume 1;
     - shares at every row, valued `1000 + row`;
     - a `TinyRole` whose days equal the panel tail, with ids {1, 2}, member and present all 1, close and raw_close
       10.
   - Probe: the first differing role row between the base and planted outputs, with t0 = 5.

     | Field(s) | Plant at axis row `P + t0` | Honest first change | Leaky clock | Leaky first change |
     |---|---|---|---|---|
     | `ret_overnight` | open × 1.01 | t0 + 1 | `open_return_row` with `a = P + t` | ≤ t0 |
     | `ret_intraday` | open × 1.01 | t0 + 1 | `open_return_row` with `a = P + t` | ≤ t0 |
     | `ceq_iss_5y` | close NaN | t0 + 1 | `a = P + t` | ≤ t0 |
     | bars | open 10.5, high 11.5, low 8.5 | t0 | `bar_row = P + t + 1` | < t0 |

   - A8 property for `ceq_iss_5y`: a shares change at row `P + t0` first changes role row `t0 + 91`.
   - Formula fingerprints: `formula_sha256(*vendor_field_spec(n))` must equal the `formula_sha256` of `n`'s row in
     `fixture_dir()/../../../tools/field_registry.json`.
2. **`research/research_fields_vendor_fixture_test.cpp`: byte identity against the Python builder.** Follow the
   pattern in `research_fields_fixture_test.cpp` (`expect_coverage` / `expect_payload` / `expect_sources`), with the
   fixture at `vendor_dir()`.
   - Load the union panel and compare its stats with the manifest `source_checks.price.source`:
     - `rows_in_file == rows_scanned`
     - `rows_sealed_dropped == rows_on_or_after_seal_skipped`
     - `rows_selected`
     - `rows_off_calendar`
     - `duplicate_keys_quarantined`
     - `rows() == extended_sessions`
     - `prefix() == sessions_before_role`
     - `first_session`
     - the factor_break block
     - the two shares counters
     - `row_groups == sources[0].row_groups`
   - Load an ohlc-only panel (`bars`) and compare it with `source_checks.ohlc.source`.
   - For each of the six fields:
     - payload bytes, coverage and sources;
     - `formula_sha256`, `formula_id`, `min_history`, `lag_sessions`;
     - `guarded_member_cells`, `outside_domain_member_cells`, `domain`, `order_violation_member_cells`,
       `missing_bar_member_cells`;
     - the `session_calendar` block.

     Read these from `VendorField.extra`; Python keeps them as top-level entry keys.
   - A registry build. Take the committed registry JSON and, for the six rows, set `kind: engine` and
     `builder: <name>`. Parse it with `parse_field_registry`, then call `build_registry_fields(spec+price_source,
     reg, sha, ProducerIdentity{...})`. The manifest entries' sha256 must equal Python's. This is also the
     verification flow for root.
   - Then add both files to the tail `target_sources` in `tests/CMakeLists.txt`. They were removed from it at the stop
     so that the tree stays buildable.
3. **Fixture pytest**, `atx-engine/tests/fixtures/research_fields/test_vendor_panel_fixture.py`:
   - a freshly written parquet equals the committed one;
   - `build(tmp, vendor=committed parquet)` reproduces every committed byte under `role/` and `expected/`.

   Pattern: `test_research_fields_fixture.py`. The DEC-5 guard forbids new Python files that match
   `research_fields_*.py`; the new names are fine.
4. **A1-SHIM shim retirement** (`wave2-carry`).
   - In each of `prepare_research_fields_{draft,xdata,ohlc,ydata}.py`, make `main` route through the registry:
     `builder.main(argv if field_registry.wants_registry(argv) else ["--registry", str(field_registry.DEFAULT_PATH), *argv])`.
   - Keep `register`, `DRAFT_MODULES` and the docstring word "DEPRECATED". `test_field_registry.Registration` checks
     for these.
   - Check first that `entry` accepts the shims' argv: `--fields` is required with `--registry`.
   - Add a pytest that mocks the argv of all four shims, then run `test_field_registry.py` etc. with explicit paths.
5. **Self-review.** Lines over 100 columns, includes, and `/W4` traps in all new C++. Then the report
   `task-A3-report.md`, added with `git add -f`.

## Decisions

- **`coskew_60m` is not ported.** Its EW market is a DuckDB `fsum` (a Kahan sum in DuckDB's own row order), so the
  engine cannot reproduce its bits.
- **`xrd0_ttm` is not ported.** It reads other fields of the same run, not the panel. Both stay Python. This is
  recorded in the `vendor_fields.hpp` header.
- **Union request.** One panel serves all six fields with the union request. With `ceq_iss_5y` present this is
  `pre = 1261` and lookback 490, which matches Python's price axis. The bar fields use the role rows of that axis.
  The panel's `rows_off_calendar` and `rows_selected` are relative to the union axis. With bars only they equal
  Python's ohlc counts.
- **Straddling row group.** A group that straddles the seal is never pruned on the window. Its keys are decoded so
  that the sealed count is exact. The scan change is in `vendor_panel.cpp` `scan_file`.
- **Duplicate counters.** The u16 duplicate counters saturate, where Python's `np.add.at` on u16 wraps. This only
  matters at 65,536 or more copies.
- **Source records.** The engine's source record has `{path, bytes, sha256}` only. Python's price source pin also
  carries `row_groups` and `rows`, which appear in the engine's `source_checks` instead.

## Open questions for the PM

1. Registry flip of the six fields, done later by root or the PM.
   - The C++ side is ready.
   - On the Python side, `CommittedRegistry` requires engine twins ⊆ `prepare_research_fields_engine.ENGINE_FIELDS`.
     The flip therefore also needs engine-shim routing, which is outside A3's brief.
2. P13 absent-seal refusal: not touched. It is enabled only after root confirms.
3. Reuse cost. A2's `reuse.cpp` re-hashes `price_source` once per vendor field when `--reuse` is used. It could be
   memoized per run but was not changed.
4. The fixture is about 320 KB, larger than other fixtures (the largest is 240 KB). It can be cut further if needed.

## Carried minors (A1, A2)

None were addressed. All were deferred by the stop:

- `--engine-exe` builds twins in Python;
- the freeze test checks file names only;
- the `first_session` format;
- `research_fields_cli.hpp:20` is 115 columns;
- dev-shared reuse hashes the exe;
- the TRAIN alternative check;
- the `.pending` pin;
- the receipt-before-manifest test;
- `strategy_live.cpp:424` `code_sha256`.

## Traps for the next agent

- **Never build C++** (lane rule). The fixture generator is the only executable step. Run it with
  `"C:/Program Files/Python312/python.exe" make_vendor_panel_fixture.py [--out DIR]`. Run pytest with explicit paths
  only, with `-p no:cacheprovider`.
- **Line endings.** `core.autocrlf=true`. The fixture dir `.gitattributes` has `* -text`, which also covers
  `vendor/`, and `field_registry.json` is `-text`. The role manifest bytes are pinned by SHA, so never let an editor
  rewrite them.
- **Shell.** Heredocs break in the shell hook except for `git commit -F -`. Create and edit files with Write/Edit.
  `rtk` mangles some `git` / `grep` output: use `rtk proxy git ...` and the Grep tool.
- **Name the `-Wshadow` / `/W4` risks.** In `vendor_fields.cpp`'s `build_bar` the locals are named `close_rows`,
  `raw_rows` and `present_rows` on purpose. In `vendor_panel.cpp` the free helper is `chunk_of`, not `column_of`.
- **Arrow 24.** Use the Result-returning `ReadRowGroup` and `OpenFile`. The out-parameter overloads are deprecated
  and trip `/WX`.
- **Python stats.** `rows_on_or_after_seal_skipped` counts every sealed row of the file. The engine's
  `rows_sealed_dropped` must equal it, and the straddle fix above guarantees that.
- **Fixture values.** `research_fields_vendor_panel_test.cpp` reads its expected values from
  `vendor/expected/manifest.normalized.json`. If the fixture is regenerated, those tests follow it, except the
  hard-coded calendar counts, which are rule constants.
