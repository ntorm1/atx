# Task A3 report: one shared vendor panel in C++, price and ohlc builder kinds (P9 wave 2)

| | |
|---|---|
| Outcome | **DONE_WITH_CONCERNS** (concerns below; none blocks the merge of the first five commits) |
| Pool / branch | `C:/atx-wt/pool-12`, `feat/p9-a3-20261003` |
| Base | `1239a5ff` |
| Head (code) | after fix round 1: the re-applied registry flip is the branch head, the one commit after this report's commit; everything below it merges without the flip (see "Fix round 1") |
| Contract | `wave2-lane-dispatch.md`, `brief-A3.md`, `wave2-carry.md` (A3), `resume-rulings.md` (All agents, A3-1..4, A3-RED, W2-BUILD, DISK, DISK-2) |

## Commits (base..head, in order)

| SHA | What | Merge note |
|---|---|---|
| `ef921abd` | M1a-RED: the two `ResearchFields*` gtests corrected (tests only, no code change) | evidence below (A3-RED) |
| `81cef7ba` | C++ library: vendor panel, factor-break-v1, NYSE rule calendar, the six vendor-panel kinds; fixture; first gtests | |
| `83d06664` | handoff at the owner stop | docs |
| `51f8158b` | gtests: leak probes (`ResearchFieldsVendorFields.*`), fixture identity (`ResearchFieldsVendorFixture.*`) | |
| `b07782e6` | A3-1 routing edit in `prepare_research_fields_engine.py`; registry owner suffix; pytests | |
| `27c22eb3` | 100-column reflows (A3 headers; A2's `research_fields_cli.hpp`) | comments only |
| `6262224f` | **registry flip** of the six rows to `kind: engine` | reverted by `2b7af4fa` in fix round 1; net zero below the re-applied flip |
| `f7941ab0` | this report (first version), A3-RED evidence, receipts | docs |
| `56474bbf` | the review (APPROVE, A3-RED confirmed) | docs, the reviewer's |
| `2b7af4fa` | revert of `6262224f` (the flip held back) | fix round 1 |
| `69743545` | S-1: sealed rows of a straddling group reach nothing; `rows_sealed_value_decoded`; new gtest | fix round 1 |
| `165b794b` | S-3: the panel guard fails closed on missing statistics; new pytest | fix round 1 |
| (this commit) | report: fix round 1, S-5 correction | docs |
| branch head | **the registry flip re-applied** (cherry-pick of `6262224f`, same content) | **merge only after root's TRAIN identity run**; everything below it merges first |

## Built and run in the lane (ruling W2-BUILD)

The equity-dev tree of pool-12 was not configured. Configured once through the wrapper with per-worktree deps
(`atx-build.ps1 configure -Preset equity-dev -DFETCHCONTENT_BASE_DIR=C:/atx-wt/pool-12/deps/equity-dev`, exit 0, 643 s),
then three target-scoped builds through `scripts/research-build.ps1`, each admitted by the memory gate, no other build
running at launch. Receipts copied to `a3-build/` beside this report.

| Tag | Source | Targets | Exit | Wall | TUs |
|---|---|---|---|---|---|
| `p9-a3-a` | `83d06664` (clean) | `atx-engine-research-fields, atx-engine-research-fields-tests, atx-research-fields` | 0 | 334 s | 87 |
| `p9-a3-b` | `83d06664` + the two new gtest files | `atx-engine-research-fields-tests` | 0 | 56 s | 3 |
| `p9-a3-c` | `51f8158b` + the reflows / routing (dirty) | the three targets | 0 | 61 s | 13 |

Every C++ file the handoff left uncompiled (`81cef7ba`) compiled first time under clang-cl 18 `/W4 /permissive- /WX`.
No C++ fix was needed; the only C++ edits after the handoff are the two new test files and comment reflows.

## Evidence

gtests (`build-equity\bin\atx-engine-research-fields-tests.exe`, build `p9-a3-c`, whole target):

```
[==========] 65 tests from 13 test suites ran. (3812 ms total)
[  PASSED  ] 65 tests.
```

That covers the 56 tests of the target before this resume (M1a-RED's two included: `ResearchFieldsWriter.
QuantilesPartitionLikeNumpy`, `ResearchFieldsVolumeMean.SumOrderIsNumpys`, `.NegativeVolumeIsNotAccepted`) and the 9 new
ones. The suites of the handoff's uncompiled file pass too: `ResearchFieldsNyseCalendar.*` (3), `ResearchFieldsFactorBreak.
ClosedForm`, `ResearchFieldsVendorPanel.*` (5: `DuplicateKeysAreQuarantinedEverywhere`, `ToF32RoundsAsNumpy`,
`RefusesAFileThatIsNotTheRoles`, `HashOnce`, `SealPushDown`).

pytest (explicit paths, each its own session, `-p no:cacheprovider`; `ATX_RESEARCH_FIELDS_EXE` = the `p9-a3-c`
`atx-research-fields.exe`, so the real-executable tests ran; every exit code 0):

| File | Result |
|---|---|
| `atx-engine/tests/fixtures/research_fields/test_vendor_engine_path.py` (new) | `8 passed` (stand-in only: `5 passed, 3 skipped`) |
| `atx-engine/tests/fixtures/research_fields/test_research_fields_engine_path.py` | `11 passed` |
| `atx-engine/tests/fixtures/research_fields/test_vendor_panel_fixture.py` (new) | `3 passed` |
| `atx-engine/tests/fixtures/research_fields/test_research_fields_fixture.py` | `2 passed` |
| `atx-engine/tools/test_field_registry.py` | `35 passed` |
| `atx-engine/tools/test_no_new_python_builder.py` (DEC-5 guard) | `4 passed` |

End to end, root's flow, on the vendor fixture (scratch, deleted after): `prepare_research_fields.py --registry
atx-engine/tools/field_registry.json --fields ret_overnight,ret_intraday,ceq_iss_5y,open_adj,high_adj,low_adj --role
vendor/role --role-sha256 <sha> --output <dir> --price-source vendor/th.parquet`, once without and once with
`--engine-exe build-equity/bin/atx-research-fields.exe`: both exit 0; the six payloads are identical to each other and to
the committed Python fixture; the manifests are equal once the six producer blocks are removed; the engine made exactly
two calls (`ret_overnight+ret_intraday+ceq_iss_5y`, `open_adj+high_adj+low_adj`).

## A3-RED: why both M1a-RED expectations were wrong (reproducible)

Reproduce (repository root of this branch; synthetic inputs only, nothing read from disk but the code):

```
"C:/Program Files/Python312/python.exe" .superpowers/sdd/platform-p9-20261003/a3-red-evidence.py
```

Output committed as `a3-red-evidence.txt` (numpy 1.26.4, Python 3.12.2). The script drives the real Python reference
functions the C++ ports must equal bit for bit, with in-memory stubs for their host handles only:
`prepare_research_fields.digest_and_quantiles` (the manifest's `member_finite_quantiles`) and
`research_fields_price.volume_mean_rows` (vol_126), the latter with `VOL_WINDOW` / `VOL_MIN_SESSIONS` patched to the
gtest's window and minimum (module globals, read at call time; nothing else is patched).

### (1) `ResearchFieldsWriter.QuantilesPartitionLikeNumpy`, the `{-0.0}` case

- **The failing expectation** (base `1239a5ff`, `research_fields_writer_test.cpp:139`): every quantile of `{-0.0}` is
  `same_bits(x, +0.0)`, with the comment "`_lerp(-0.0, -0.0, gamma >= 1) is +0.0`". Root saw it fail five times.
- **The spec of the value:** the manifest's quantiles are `np.quantile(values, [0.001, 0.01, 0.5, 0.99, 0.999],
  overwrite_input=True)` (`prepare_research_fields.py:179` `QUANTILES`, `:832`), default method `linear`.
- **numpy 1.26.4** (`numpy/lib/function_base.py`, printed by the script):
  - `_get_indexes`: `virtual_indexes >= valid_values_count - 1` sets `previous = next = -1`.
  - `_get_gamma` (linear): `gamma = virtual_index - previous`.
  - `_lerp(a, b, t)`: `a + (b - a) * t`, then overwritten by `b - (b - a) * (1 - t)` where `t >= 0.5`.
- **Computed for one value:** `virtual_index = (n - 1) * p = 0` for all five `p`, so `0 >= n - 1 = 0`, giving
  `previous = next = -1` and `gamma = 0 - (-1) = 1` at every `p`. With `a = b = -0.0`, `diff = b - a = +0.0`. Every `p`
  takes the overwrite branch, `b - diff * (1 - gamma) = -0.0 - (+0.0) = -0.0`. Measured:
  `digest_and_quantiles` gives `-0.0` (signbit 1) at p0.1, p1, p50, p99 and p99.9, and so does `np.quantile` called
  directly. The test's hand reasoning missed the `-1` index, so `gamma` is 1, not "`>= 1` of the clipped index".
- **Two values `{-0.0, -0.0}`:** `gamma = p` = `[0.001, 0.01, 0.5, 0.99, 0.999]`. The kept branch gives
  `-0.0 + 0.0 = +0.0`, the overwrite branch gives `-0.0`. Measured: `[+0.0, +0.0, -0.0, -0.0, -0.0]`. Added as a test,
  so both branches are pinned.
- **The code** (`field_stats.cpp:243-252` `lerp`) returns `-0.0`, and the corrected test passes.
- **Root cause** (corrected in fix round 1, review S-5): the numpy rule was in the code from the start. `eccf6338`
  already had the `gamma >= 0.5` overwrite lerp and marked the above-bounds index -1 before computing gamma
  (`field_stats.cpp:61,72` there: "numpy marks the above-bounds index -1 before it computes gamma"). `e812a0fd` moved
  that lerp into a helper and added the hand-derived `{-0.0} -> +0.0` expectation, which contradicted the existing
  code and its comment. It was written by an uncompiled lane and never ran until T1 registered the executable in
  CTest.
- **Every other expectation of the test is unchanged in `ef921abd`:** the `small`, `large` and `unsorted` cases.

### (2) `ResearchFieldsVolumeMean.SumOrderIsNumpys`

**The spec.** The vol_126 definition (`research_fields_price.py:165-168`, the registry `spec_text`, and C++
`vol_126_spec`) reads: "mean of the role's volume.f64 over the sessions s in t-126..t-1 with present[s] == 1 and a
**finite volume >= 0**". The Python producer is `volume_mean_rows`. At `:623` it sets
`ok = p & np.isfinite(v) & (v >= 0)`, then `ring[slot], seen[slot] = np.where(ok, v, 0.0), ok`, and
`row = np.where(k >= MIN, ring.sum(axis=0) / np.maximum(k, 1), nan)`. The C++ rule is `trailing_mean.cpp:57`,
`accepted = present && isfinite(v) && v >= 0.0`. It was present in the same commit as the test (`eccf6338`).

**The failing expectations.** Base `1239a5ff`, `research_fields_volume_mean_test.cpp:120/133/135`. Each feeds a
`-1e16` volume and expects it summed. The table runs the real `volume_mean_rows` on exactly those inputs:

| Base test line | Inputs (sessions; window, min) | Test expected | `volume_mean_rows` (numpy 1.26.4) | C++ result root saw |
|---|---|---|---|---|
| `:120` | `{5,1},{1e16,1},{1,1},{-1e16,1}`; 3, 1 | `1/3` | `5000000000000000.0` | `5e15` |
| `:133` | one column `1, 1e16, -1e16, 1, 0x5`; 9, 1 | `0.0` | `1250000000000000.0` | `1.25e15` |
| `:135` | the same, plus a column of 1s; 9, 1 | `1/9` | `1250000000000000.0` | `1.25e15` |

The Python reference returns exactly what the C++ returned. The test's values (`1/3`, `0`, `1/9`) are what a rule
that **accepts** negative volumes gives: the script's `hypothetical` ring with `ok = isfinite(v)` returns
`0.3333333333333333`, `0.0` and `0.1111111111111111`. That rule is not the spec. The code is right and the test's
premise was wrong.

**The new inputs.** Every volume is accepted. The inputs still separate slot order from session order (two columns)
and pairwise summation from slot order (one column). Their expected values are `volume_mean_rows` run on them:

| Case | Expected |
|---|---|
| two columns | `{3333333333333334.0, 1.0}` (session order gives `3333333333333333.5`) |
| one column | `1111111111111111.4` |
| two columns, window 9 | `{1111111111111111.1, 1.0}` |
| new `NegativeVolumeIsNotAccepted`: `{4, -1e16, 2}` | `3.0` |

The script prints each value with its hex bits.

**Root cause.** `eccf6338` (YARCH 3, "Not compiled by the lane") shipped the accept rule and a test that contradicts
it. The test first ran when T1 registered the executable.

### Why this is not "editing an expected value to fit"

- **Quantiles.** The new expected value is numpy's measured output for the same input, which the manifest records. The
  old value was a hand derivation that numpy contradicts.
- **Volume mean.** The new inputs fit the spec's domain, and the expected values are the Python reference run on them.
  The old inputs lay outside the spec. On those inputs the Python reference agrees with the code, not with the old
  expectation.
- **Neither commit changed library code** (`ef921abd`: test files only).

## What each carry item became

| Item | Status |
|---|---|
| M1a-RED (required) | **done** in `ef921abd`, its own commit before the flip; root causes above; reviewer re-derives (A3-RED) |
| Field-flip freeze | respected: the flip is the last code commit, separate, after the fix; root merges it only after A3-RED is accepted and the TRAIN identity holds. Fix round 1: `6262224f` reverted (`2b7af4fa`) and re-applied unchanged as the branch head (see "Fix round 1") |
| A1-SHIM (shims route through `--registry`) | **not done** (see Concerns 1) |
| P13 (absent-seal refusal) | untouched (ruling A3-2) |
| A1 merge state (`regenerate()` round-trip) | holds: `test_field_registry.py` 35 passed with the owner suffix and the flip |
| Carried A1 minors | not addressed (A1's files, outside A3's scope): `--engine-exe` Python twins (now moot for the nine routed fields: `engine_path` routes every engine twin), freeze test file names, `first_session` format, root-diff omission |
| Carried A2 minors | **fixed:** `research_fields_cli.hpp:20` 115 columns (`27c22eb3`). Not addressed (A2's files): dev-shared reuse hashes the exe, TRAIN alternative check, `.pending` pin, receipt-before-manifest test, `strategy_live.cpp:424` |
| A3-3 (per-field re-hash under `--reuse`) | unchanged, for wave 3 |
| A3-4 (fixture size) | `vendor/` is 354 KB on disk (accepted at about 320 KB; the difference is the filesystem's rounding) |

## Contracts

- **K-P9-2, source half.** `VendorPanel` (`research/fields/sources/vendor_panel.{hpp,cpp}`) does the following:
  - hashes the file once and checks it against the role's `source_sha256`, with a stamp check before and after;
  - scans the row groups once over the union of requested columns;
  - pushes the seal down to the row-group statistics: a wholly sealed group is never read. A group that straddles
    the seal has its keys decoded and its sealed rows dropped by date; its value chunks are decoded only when a
    pre-seal row survives, and then whole (a parquet column chunk is the unit of decode), sealed rows' values
    included. Those values are never read and reach no observation, matrix, statistic or output;
    `rows_sealed_value_decoded` counts them (corrected in fix round 1, review S-1; the earlier text said a straddling
    group "has only its keys decoded", which holds only when no pre-seal row of it survives);
  - applies the observation contract once;
  - quarantines duplicate keys in every matrix;
  - runs factor-break-v1 once (`sources/factor_break.{hpp,cpp}`), one implementation for the two Python copies.

  Several vendor kinds in one build share the panel (`BuildContext::sources.vendor_panel`, loaded with the union
  request), as `HashOnce` shows.
- **K-P9-1.** The six registry rows are `kind: engine`, with `builder` set to their kind id (`6262224f`). The C++
  registry build (`atx-research-fields --registry`) accepts them, and `ResearchFieldsVendorFixture.
  RegistryBuildEntriesAreThePythonEntries` builds the committed registry with them flipped.
- **K-P9-3.** The wrapper stamps the engine producer block on each routed entry. This is the existing mechanism; the
  receipt's SHA is per call.
- **SQL2-CLS.** There are no new `atx.<name>/v<n>` schema literals.

## Files changed

**Owned (A3), new:**

- `atx-engine/include/atx/engine/research/fields/sources/{vendor_panel,factor_break,nyse_calendar}.hpp`, `.../vendor_fields.hpp`
- `atx-engine/src/research/fields/sources/{vendor_panel,factor_break,nyse_calendar}.cpp`, `.../vendor_fields.cpp`
- `atx-engine/tests/research/research_fields_vendor_{panel,fields,fixture}_test.cpp`
- `atx-engine/tests/fixtures/research_fields/make_vendor_panel_fixture.py`, `vendor/**`,
  `test_vendor_engine_path.py`, `test_vendor_panel_fixture.py`
- `atx-engine/tools/field_registry.json`: the six rows only (owner suffix in `b07782e6`; kind / builder in `6262224f`)

**Cross-lane** (each the minimum; CMake at list tails):

| File | Edit |
|---|---|
| `atx-engine/CMakeLists.txt` | tail `target_sources(atx-engine-research-fields ...)`, four `.cpp` |
| `atx-engine/tests/CMakeLists.txt` | tail `target_sources(atx-engine-research-fields-tests ...)`, three test files |
| `registry.hpp` / `registry.cpp` (A2) | `SharedSources::vendor_panel`, `BuildContext::plans`, the six kinds appended to `kKinds` (3 -> 9), `parse_vendor` / `ensure_vendor_panel` / `build_vendor_kind` |
| `build_spec.hpp` (A2) | `std::optional<path> price_source` |
| `research_fields_cli.{hpp,cpp}` (A2) | optional spec key `price_source`; `BuildContext{..., plans}`; the 115-column comment reflowed |
| `research_fields_test_support.hpp` (A2) | `TinyRole` gains `close`, `raw_close`, `source_sha256`; defaults keep the manifest text unchanged |
| `research_fields_registry_test.cpp` (A2) | `KindLookup` expects 9 kinds, vendor kinds parse with `price_source` |
| `research_fields_writer_test.cpp`, `research_fields_volume_mean_test.cpp` | M1a-RED (in scope by the carry) |
| `atx-engine/tools/prepare_research_fields_engine.py` (A2) | ruling A3-1, the routing edit (below) |
| `atx-engine/tools/test_field_registry.py` (A1) | `test_engine_ported_rows_carry_the_engines_formula` binds every shim module and uses each field's own lag (it computed the bar fields' spec with the plain builder) |
| `atx-engine/tests/fixtures/research_fields/test_research_fields_engine_path.py` (A2) | pins the nine routes; the "no engine route" probe uses `coskew_60m` (now that `ret_overnight` routes) |

**Untouched:** `prepare_research_fields.py`, `research_fields_price.py`, `research_fields_ohlc.py`, the four
`prepare_research_fields_{draft,xdata,ohlc,ydata}.py` shims and `field_registry.py` (`git diff 1239a5ff HEAD` on them
is empty). So no Python code identity or `--reuse` fingerprint moves.

## The A3-1 routing edit (`prepare_research_fields_engine.py`)

The modules still read their own panels, so the manifest's `source_checks` are unchanged. The engine computes the
payloads. The edit has five parts.

**Routed set.** `ENGINE_FIELDS` now holds nine names: the three it already routed plus `PRICE_FIELDS` and `BAR_FIELDS`.

**`build_fields`.** This is `build` generalized to one executable call for several fields, with the spec's
`price_source` passed through. Its receipt and spec files are tagged `a+b+c`, so a single field keeps `<name>.*`. The
formula check uses each field's own module lag (`lag_of`).

**Price hooks.** `price.open_return_rows` and `price.ceq_iss_rows` are routed through one engine call per source panel:

- The open-return call also builds `ceq_iss_5y` when it is routed and wanted (`panel["shares"] is not None`).
- `ceq_iss_rows` reuses that result.

This gives the engine's panel the Python panel's history. That matters because the price fields' bits depend on the
panel's history: a repaired factor step before the role divides both factors of a ratio, and `(o*fa/k)/(cb*fb/k)` is
not bitwise `(o*fa)/(cb*fb)`.

**Bar hooks.** `ohlc.bar_panel` records the vendor path, and `ohlc.bar_rows` routes the bar fields in one call.

**Guards** (fail closed):

- `same_panel` requires every read statistic of the Python panel, apart from its rule text, to equal the engine's.
  This covers the axis (sessions before the role, first session), the rows selected, off-calendar and duplicate rows,
  the sealed count and factor-break-v1.
- The source bytes are checked.
- For the bars, the vendor pin must be the role's `source_sha256`.
- For `ceq_iss_5y`, the engine's declared domain must be the builder's.

A run whose Python panel reaches further back than the engine's own fields ask is refused. Two such runs: `coskew_60m`
without `ceq_iss_5y`, and `ceq_iss_5y` left to Python while the open returns are routed. The real-executable test
`test_real_executable_refuses_a_longer_python_panel` shows the refusal.

## How root verifies

1. **Build** (equity-dev, then Release if wanted):
   `powershell -File scripts\research-build.ps1 -Tag p9-2<letter> -Targets
   "atx-engine-research-fields,atx-engine-research-fields-tests,atx-research-fields"`
2. **gtests:** `build-equity\bin\atx-engine-research-fields-tests.exe --gtest_filter=ResearchFields*`, 65 of 65.
   - Anchored for A3: `ResearchFieldsVendorPanel.*:ResearchFieldsVendorFields.*:ResearchFieldsVendorFixture.*:
     ResearchFieldsNyseCalendar.*:ResearchFieldsFactorBreak.*`.
   - Anchored for M1a-RED: `ResearchFieldsWriter.QuantilesPartitionLikeNumpy:ResearchFieldsVolumeMean.*`.
   - Wave-2 gate: these two must be green with no known-red exception.
3. **pytest**, each file in its own session (the research window), with `ATX_RESEARCH_FIELDS_EXE=<build>\bin\
   atx-research-fields.exe`: the six files in the table above.
4. **Flag-absent identity:**
   - Python builders, modules and shims: byte-unchanged.
   - `test_flag_absent_is_the_plain_builder` passes (the wrapper without `--engine-fields` is the plain builder).
   - The registry entry without `--engine-exe` computes the six engine twins in Python: the same `build_main(argv)`, a
     stderr note only.
   - C++: a spec without `price_source` parses and builds as before (`ResearchFieldsCli.*`, `ResearchFieldsManifest.*`,
     `ResearchFieldsFixture.*` green).
5. **TRAIN identity per moved field** (real data, root only; the brief's check). Run the v15 TRAIN role with v15's own
   field list (so the Python panel has v15's history), through the registry entry:

   ```
   prepare_research_fields.py --registry atx-engine/tools/field_registry.json --fields <v15 list> --role <TRAIN role>
     --role-sha256 <sha> --output <dir> --price-source <its TickerHistory3 parquet> --engine-exe
     build-equity/bin/atx-research-fields.exe <v15's other argv>
   ```

   Each of the six `<name>.f64` SHA-256s must equal v15's manifest `files` entry. A `same_panel` refusal there means
   v15's run shape is one the wrapper cannot reproduce, which is a stop, not a tolerance.

   The C++-only alternative is `atx-research-fields build --spec S --receipt R --registry field_registry.json`, with a
   v2 spec `{schema atx.research-fields-spec/v2, role {dir, manifest_sha256}, output_dir, fields: [the six],
   price_source}`. Its union panel has the six fields' history, which is v15's when v15 built `ceq_iss_5y` (1,261 sessions
   + 490 days).
6. **Build wall before / after:** the six fields through the Python builder against the C++ registry build in step 5.
   The wrapper path is not a speed-up (Concerns 2).

## Tests written (names)

- **gtest `ResearchFieldsVendorFields`** (`research_fields_vendor_fields_test.cpp`):
  - `OpenReturnsLookAheadProbe`, `CeqIssuanceLookAheadProbe`, `OhlcBarsLookAheadProbe`: one planted-leak probe per
    moved builder. The honest builder first changes at t0+1 (lag-1 price fields) or t0 (same-session bars). The same
    row kernel under a clock one session later is caught. The honest kernel is also checked equal to the builder's
    payload, so the probe drives the builder's own arithmetic.
  - `CeqIssuanceSharesLagNinetyDays`: A8, the first change is exactly at t0 + 91.
  - `FormulaFingerprintsAreTheRegistrys`: every ported spec's fingerprint equals the committed registry row's, and so
    does its group.
- **gtest `ResearchFieldsVendorFixture`** (`research_fields_vendor_fixture_test.cpp`):
  - `UnionPanelIsThePythonPricePanel`: stats, axis, the source pin's `row_groups` and `rows`.
  - `BarPanelIsThePythonOhlcPanel`: bars-only stats, plus the three bars' identity on that panel.
  - `FieldsAreByteIdentical`: payload, sha, coverage bit for bit, sources, fingerprint, formula id, min history, every
    extra key and no other.
  - `RegistryBuildEntriesAreThePythonEntries`: the committed registry with the six flipped; every Python entry key is
    equal and the engine adds only `producer`; files are equal; the price fields' source checks equal the Python
    price group's.
- **pytest** (`test_vendor_engine_path.py`):
  - `test_vendor_fields_route_one_call_per_panel[all six | ceq+high]`
  - `test_panel_guard_refuses_another_panel[ret_overnight | low_adj]`
  - `test_repository_registry_routes_the_vendor_rows_to_the_engine`
  - `test_real_executable_identity[all six | ceq+open]`
  - `test_real_executable_refuses_a_longer_python_panel`
- **pytest** (`test_vendor_panel_fixture.py`):
  - `test_the_generator_writes_the_committed_parquet`
  - `test_the_fixture_is_the_python_builders_output`
  - `test_the_fixture_exercises_every_rule`

## Decisions

- **The handoff's decisions stand:**
  - `coskew_60m` and `xrd0_ttm` are not ported.
  - One union panel serves all six fields.
  - A straddling row group is never pruned on the window.
  - The u16 duplicate counters saturate.
  - The engine's source record is `{path, bytes, sha256}`.
- **Routing at the module producers keeps the Python panel.** The Python panel stays for its manifest
  `source_checks`, and the engine call is per panel. This is the smallest edit that is identity-safe; the alternative,
  a new C++ spec key for the history, would widen A2's contract.
- **The leak-probe axis carries the A8 share lookback.** Its prefix is 1,751 consecutive days (1,261 sessions plus 490
  days), so role row 0 already reads two share observations. With a 1,261-only prefix, `ceq_iss_5y` is NaN for
  t < 90, and a close plant at t0 = 5 would never reach the output (no teeth).
- **The brief's test names carry the target's `ResearchFields` prefix** (`ResearchFieldsVendorPanel.HashOnce`,
  `.SealPushDown`, `ResearchFieldsFactorBreak.ClosedForm`), so the documented filter `ResearchFields*` runs them.

## Concerns

1. **A1-SHIM is not done.** The PM's resume scope named the build, the A3-1 routing, and the leak and identity tests;
   A1-SHIM is not marked required in the carry. It changes the argv path of every caller of the four deprecated shims
   (the registry entry binds every module a row names, `--fields` semantics). The recipe is in `handoff-A3.md` item 4.
   It is a small follow-up with its own pytest. **Question for the PM:** wave 3, or an A3 follow-up now?
2. **The wrapper path is identity, not speed.** With `--engine-exe` the Python module still loads its panel (for the
   manifest's source checks and the `same_panel` guard), and the engine loads its own. The build-time win of the
   brief comes from the C++ registry build (`atx-research-fields --registry`), or from a later slice that drops the
   Python panel once root's identity run passes.
3. **Union-panel memory.** The C++ union panel holds every requested matrix on the extended axis. With the bars in the
   union, `bar_open/high/low` also span the roughly 1,600 pre-role rows, which are NaN and unused. On the TRAIN role,
   root should watch RSS. The candidate fix is to keep bars on role rows only (wave 3).
4. **The engine's `source_checks` differ in shape from Python's** in a C++ registry build. They are per field, not per
   group, with no rule text, and a bar field built on the union panel reports the union's counts. Payload identity is
   unaffected. A manifest-level identity of `source_checks` would need a per-group block (wave 3).
5. **DISK-2.** After this report's commit, `build-equity/`'s object tree and `deps/equity-dev/` (both created by this
   lane) are deleted. The `mega-p9-a3-*` receipts and logs are kept; the pre-existing `build-equity/audits/` and
   `build-equity/v8-interim3-pitch-render-run2/` are left alone. A reviewer who wants to re-run the gtests re-configures
   (about 11 minutes, mostly vcpkg) and rebuilds `p9-a3-*`.

## Ledger candidates

1. In numpy 1.26.4, `np.quantile` of a single value returns that value with its sign. An above-bounds index is `-1`,
   so `gamma` is 1 and `_lerp` takes `b - diff*(1-gamma)`. The quantiles of `{-0.0}` are `-0.0`; of `{-0.0, -0.0}` they
   are `[+0, +0, -0, -0, -0]`.
2. The bits of the vendor price fields depend on the panel's history extent: repaired factor steps before the role
   are divided out of both legs of a ratio. A port must build with the Python run's history, and
   `prepare_research_fields_engine.same_panel` refuses otherwise.
3. A fresh-pool equity-dev configure with per-worktree deps took 643 s. The first research-fields build took 334 s for
   87 TUs, at 4 jobs.

## Fix round 1 (PM ruling A3-FIX1, review `task-A3-review.md` at `56474bbf`: APPROVE, A3-RED confirmed)

Three Suggested findings fixed now. S-2 and S-4 are wave 3; S-6 is acknowledged by the PM (`coskew_60m` and
`xrd0_ttm` stay Python).

### Where the flip sits

History is not rewritten. The branch reads, oldest first:

```
... 27c22eb3  6262224f (flip)  f7941ab0  56474bbf  2b7af4fa (revert of the flip)
    69743545 (S-1)  165b794b (S-3)  <this report commit>  <branch head: the flip re-applied>
```

- The re-applied flip is a cherry-pick of `6262224f` with the same content: the six rows go to `kind: engine`, plus
  the pytest that pins them. It is the branch head, the one commit after this report's commit; its SHA is in the
  lane's final message.
- **Root merges the parent of the head first** (everything except the flip). `6262224f` and its revert `2b7af4fa`
  cancel, so that tree has the three A2 engine rows only. Root merges the head (the flip) only after the TRAIN identity
  run.
- The re-apply was checked before this report: it cherry-picks cleanly onto `165b794b`, and on the post-flip tree
  `test_vendor_engine_path.py` gives 9 passed, `test_research_fields_engine_path.py` 11 passed and
  `test_field_registry.py` 35 passed (real executable).

### S-1: sealed values in a straddling row group (`69743545`)

What happens, exactly (the code, its comments, the test header and the fixture generator's docstring now say this):

1. A row group whose tradingDate statistics start on or after the seal is never read.
2. Any other read group has its key columns (tradingDate, securityID) decoded first. `select_rows` drops each sealed
   row first, by its date alone. It counts the row (`rows_sealed_dropped`) and looks at nothing else of it, so a
   sealed row cannot reach `rows_off_calendar` or any later check. Before the fix, a sealed row fell through to the
   window check, which also dropped it (the window ends at `min(role last, seal - 1)`); the drop is now explicit and
   first.
3. If no row of the group survives, its value columns are never decoded (the fixture's straddling group, pinned by
   `SealPushDown`).
4. If a pre-seal row survives, the group's value chunks are decoded whole, because a parquet column chunk is the unit
   of decode. The sealed rows' values are in that decode. Only the surviving rows' indexes are read from it
   (`observation`), and the chunks are released when `scan_row_group` returns. So no sealed value reaches an
   observation, a matrix, a statistic, a message or an output.
5. The new statistic `rows_sealed_value_decoded` (in `VendorScanStats` and the engine's
   `source_checks.<field>.source`) counts the sealed rows decoded this way. It is 0 when no sealed value was decoded at
   all. On the TRAIN run, root reads this count directly (review section 6, item 4) instead of inferring it.

New gtest `ResearchFieldsVendorPanel.SealedValuesOfAStraddlingGroupReachNothing`:

- The test writes three synthetic one-row-group parquets (seal probes, ruling T2-SYN) over a role of four sessions
  (2023-12-26..29, lines 1 and 2).
- **clean:** the role's eight rows only.
- **sealed:** the clean rows plus five poisoned rows dated 2024-01-01..03. The poison is a sealed holiday, a sealed
  key written twice, an id off the role, shares above the A9 ceiling, non-observations and absurd bars. The result:
  - one straddling group with keys and values decoded;
  - `rows_sealed_dropped == rows_sealed_value_decoded == 5`;
  - every other statistic equals the clean file's (rows selected, off calendar, duplicates, A9 rows, C-81 lines,
    repaired and kept-gap steps);
  - every matrix cell is bit-equal to the clean file's (factor, close, shares, open, the three bars, `first_above`);
  - the six payloads are byte-equal to the clean file's.
- **inside (the teeth):** the same poison dated inside the window (2023-12-25, 28, 28, 29) moves everything it
  should:
  - off calendar 1;
  - duplicates 2;
  - A9 rows 3;
  - C-81 lines 2;
  - matrices differ.

`SealPushDown` also pins `rows_sealed_value_decoded == 0` for the fixture.

The test's first run failed on one teeth line only. I had hand-counted 1 duplicate key for the "inside" case, but the
poison row on `(2023-12-29, line 2)` also lands on a clean key, so the true count is 2. I corrected that expectation in
this new, not-yet-passing test, with the reason in its comment, before committing. No code changed, and no existing
expectation was edited.

### S-3: the panel guard fails closed (`165b794b`)

`same_panel` used to refuse only on a differing statistic, so an empty Python panel record compared nothing and
passed. It now refuses in two cases:

- **Python statistics** that are not a dict, or that lack any of `PANEL_KEYS`. These are `rows_scanned`,
  `rows_on_or_after_seal_skipped`, `rows_selected`, `rows_off_calendar` and `duplicate_keys_quarantined`; both Python
  panels always record them.
- **An engine entry without its panel block**, meaning no `source_checks.source` dict, or an empty one.

New pytest `test_panel_guard_fails_closed_on_missing_statistics`:

- Complete, equal price and ohlc statistics pass.
- Each of these Python records is refused with the "recorded no read statistics" message: `{}`, `None`, a record
  missing `rows_selected`, and a record holding only `rule`.
- Each of these engine entries is refused with the "carries no vendor panel statistics" message: `{}`,
  `{"source_checks": {}}`, an empty `source`, and a `None` `source`.

### S-5: root-cause attribution

Corrected in place under "A3-RED" (1). The numpy lerp and the -1 index marking were in `eccf6338`. `e812a0fd` moved
the lerp into a helper and added the hand-derived expectation that contradicted them. The substance of the root cause
is unchanged.

### Builds and tests (fix round 1)

The equity-dev tree had been deleted under DISK-2. I reconfigured it with per-worktree deps
(`atx-build.ps1 configure -Preset equity-dev -DFETCHCONTENT_BASE_DIR=C:/atx-wt/pool-12/deps/equity-dev`, exit 0,
157 s), then ran two builds through `research-build.ps1`, each admitted by the memory gate. Receipts are in
`a3-build/`.

| Tag | Source | Targets | Exit | Wall | TUs | Warnings in log |
|---|---|---|---|---|---|---|
| `p9-a3-d` | `2b7af4fa` + the S-1 / S-3 edits | `atx-engine-research-fields, atx-engine-research-fields-tests, atx-research-fields` | 0 | 52 s | 89 | 0 |
| `p9-a3-e` | the same + the corrected teeth count | `atx-engine-research-fields-tests` | 0 | 17 s | 1 | 0 |

- **gtests**, whole target (`p9-a3-e`): `66 tests from 13 test suites ran ... [ PASSED ] 66 tests`. That is the 65
  from before plus the new straddle test. The anchored M1a-RED and seal filters
  (`ResearchFieldsVendorPanel.Seal*:ResearchFieldsWriter.QuantilesPartitionLikeNumpy:ResearchFieldsVolumeMean.*`)
  are all green.
- **pytest**: explicit paths, each file in its own session, with `ATX_RESEARCH_FIELDS_EXE` set to the `p9-a3-d`
  `atx-research-fields.exe`. Every exit code is 0.

  | File | Pre-flip tree (below the head) | Post-flip trial |
  |---|---|---|
  | `test_vendor_engine_path.py` | 8 passed | 9 passed |
  | `test_research_fields_engine_path.py` | 11 passed | 11 passed |
  | `test_vendor_panel_fixture.py` | 3 passed | (not run) |
  | `test_research_fields_fixture.py` | 2 passed | (not run) |
  | `test_field_registry.py` | 35 passed | 35 passed |
  | `test_no_new_python_builder.py` | 4 passed | (not run) |

  `test_vendor_panel_fixture.py` re-runs the generator and the Python builder; with the docstring edit, every
  committed fixture byte still comes back.

### For root, updated

- **Anchored filter for A3:** `ResearchFieldsVendorPanel.*:ResearchFieldsVendorFields.*:ResearchFieldsVendorFixture.*:
  ResearchFieldsNyseCalendar.*:ResearchFieldsFactorBreak.*`, now 19 tests (19 passed, run here). The whole target is
  66 of 66.
- **Merge order:**
  1. Merge the parent of the branch head; the pre-flip tree is green.
  2. Run the TRAIN identity run.
  3. Merge the head (the flip) only after that run passes.
- **Seal check on the TRAIN run:** read `source_checks.<field>.source.rows_sealed_value_decoded`. 0 means no sealed
  value was decoded at all; a positive count is the number of sealed rows decoded with their chunk and dropped unread.

### DISK-2 (again)

After this report's commit, I delete the object tree I rebuilt and `deps/equity-dev/`, by name. The `mega-p9-a3-*`
receipts and logs and `a3-fix1-configure.log` stay in `build-equity/`; the pre-existing `audits/` and
`v8-interim3-pitch-render-run2/` are left alone.
