# Task A3 review: vendor panel, factor-break-v1, six vendor-panel builder kinds (P9 wave 2)

| | |
|---|---|
| Lane / pool / branch | A3, `C:/atx-wt/pool-12`, `feat/p9-a3-20261003` |
| Base / head reviewed | `1239a5ff` .. `f7941ab0` (last code commit `6262224f`) |
| Contract | `wave2-review-dispatch.md`, `briefs/review-template.md`, `brief-A3.md`, `wave2-carry.md` (All lanes, A3), `resume-rulings.md` (A3-1..4, A3-RED, A3-SHIM via progress), progress `M1a-RED` |
| Reviewer | task reviewer, read-only. No lane file edited. C++ not rebuilt (reason under A3-RED). |
| **Overall** | **APPROVE** |
| Spec verdict | **PASS**: every required carry item met (table below) |
| Quality verdict | **PASS**: 0 Required, 6 Suggested |
| **A3-RED** | **Confirmed independently.** The new expectations are numpy 1.26.4's values and the spec's, bit for bit. Neither old expectation could ever have been right. |

The review dispatch says "APPROVE / BLOCK" with blocker / major / minor. Here, Required = blocker or major, and
Suggested = minor. There are no Required findings, so APPROVE is valid.

## 1. A3-RED: independent re-derivation (Ruling A3-RED)

### Method

- **Environment.** `C:/Program Files/Python312/python.exe`, Python 3.12.2, **numpy 1.26.4** (the reference)
  (`C:\Users\natha\AppData\Roaming\Python\Python312\site-packages\numpy`).
- **Independence.** I wrote my own script and ran it before opening the lane's `a3-red-evidence.py` / `.txt`. It
  imports numpy only: no lane script and no repository module.
- **Reference rules, typed from the spec:**
  - Manifest quantiles: `np.quantile(values, [.001, .01, .5, .99, .999], overwrite_input=True)`, default `linear`
    (`prepare_research_fields.py:179` `QUANTILES`, `:832`).
  - vol_126: "mean of volume over s in t-W..t-1 with present[s] == 1 and a finite volume >= 0; NaN when fewer than
    MIN such sessions" (`research_fields_price.py:165-168`). The ring is a `(W, n)` float64 array reduced by
    `ring.sum(axis=0)`, the producer's reduction (`:613-625`).
- **numpy source read directly** in the installed `numpy/lib/function_base.py`:
  - `_lerp` (`:4641-4662`) writes `a + diff*t` and overwrites it with `b - diff*(1-t)` where `t >= 0.5`.
  - `_get_indexes` (`:4744-4748`): `virtual_index >= n-1` sets `previous = next = -1`.

### Script core (the full script was a scratch file, deleted after this review)

```python
q = lambda v: np.quantile(np.array(v, dtype=np.float64), [.001, .01, .5, .99, .999], overwrite_input=True)
def vol_rule(sessions, window, min_count, accept_negative=False):     # row after the last push (row t reads < t)
    s = np.asarray(sessions, float); s = s[:, None] if s.ndim == 1 else s
    ring, seen = np.zeros((window, s.shape[1])), np.zeros((window, s.shape[1]), bool)
    for t, v in enumerate(s):
        ok = np.isfinite(v) if accept_negative else np.isfinite(v) & (v >= 0)
        ring[t % window], seen[t % window] = np.where(ok, v, 0.0), ok
    k = seen.sum(axis=0)
    return np.where(k >= min_count, ring.sum(axis=0) / np.maximum(k, 1), np.nan)
```

Each test literal is parsed with `float(text)` (correctly rounded, as clang parses a literal) and compared to numpy's
output by its 64-bit pattern.

### Results (numpy 1.26.4)

| Test case (head `f7941ab0`) | New expectation | numpy 1.26.4 (bits) | Equal |
|---|---|---|---|
| writer `{-0.0}`, all 5 p (`research_fields_writer_test.cpp:140-144`) | `-0.0` x5 | `-0.0` x5 (signbit 1) | yes |
| writer `{-0.0,-0.0}` (`:145-151`) | `+0,+0,-0,-0,-0` | `+0,+0,-0,-0,-0` | yes |
| writer unchanged siblings: small / large / unsorted | (unchanged) | `[+0,+0,-0,1,1]` / `[-2.5,-2.5,-0,3,3]` / `[1.003,1.03,2.5,3.9699999999999998,3.997]` | yes |
| volume two columns, W3 (`research_fields_volume_mean_test.cpp:123-124`) | `3333333333333334.0`, `1.0` | `4327af4c4a80aaac`, `3ff0...` | yes |
| volume one column, W9 (`:137`) | `1111111111111111.4` | `430f9465b8ab8e3b` | yes |
| volume two columns, W9 (`:139-140`) | `1111111111111111.1`, `1.0` | `430f9465b8ab8e39`, `3ff0...` | yes |
| `NegativeVolumeIsNotAccepted` `{4,-1e16,2}` W3 (`:145-154`) | `3.0` | `4008000000000000` | yes |

The lane's `a3-red-evidence.txt` reports the same values in hex-float form (e.g. `0x1.7af4c4a80aaacp+51` =
`4327af4c4a80aaac`). Two independent derivations agree. Theirs drives the real `volume_mean_rows` /
`digest_and_quantiles`; mine is a re-typed rule.

### The new inputs still test what the test is named for

The rewritten `SumOrderIsNumpys` still separates the orders it names:

- Two columns, W3: session order gives `3333333333333333.5`; numpy's slot order gives `...334.0`.
- One column, W9: slot order gives `1111111111111111.1`; numpy's pairwise sum gives `...111.4`.
- Two columns, W9, same data: `...111.1`, which pins the column-count dispatch.

Every new value therefore differs from the value of the alternative order it rules out. The new inputs are inside the
spec's domain (every volume >= 0). On the old inputs, with the -1e16 rejected, slot order and session order both give
`1e16`. So the old test separated no orders under the real rule.

### Could the OLD expectations ever have been right? No.

- **Quantile `{-0.0}` -> `+0.0`.** For n = 1, every one of the five p has `virtual_index = 0 >= n-1`. That gives
  `previous = next = -1` and gamma = 0 - (-1) = **exactly 1**, so the overwrite branch computes
  `-0.0 - (+0.0 * 0) = -0.0`.
  - The old comment ("`gamma >= 1` is +0.0") holds only for gamma **> 1**. My hand check: t = 2 gives `+0.0`, t = 1
    gives `-0.0`.
  - numpy reaches gamma > 1 only at q = 1.0 with n >= 2, which is not one of the manifest's five probabilities.
  - The C++ already marked the above-bounds index -1 when that expectation was written: `eccf6338` `field_stats.cpp:61`,
    "numpy marks the above-bounds index -1 before it computes gamma". The expectation added by `e812a0fd`
    contradicted numpy and the code's own comment from the start.
- **Volume `1/3, 0, 1/9`.** My numpy gives exactly these bits only under a *hypothetical* rule that accepts negative
  volumes (`ok = isfinite(v)`). The spec rule gives `5e15, 1.25e15, 1.25e15`, the values root saw. No version of the
  rule ever accepted negatives:
  - the Python producer has had `ok = p & np.isfinite(v) & (v >= 0)` since the module's first commit (`1e44f90a`,
    `research_fields_price.py:593` there);
  - the C++ `v >= 0.0` (`trailing_mean.cpp:57`) arrived in the same commit as the test (`eccf6338`);
  - the same test file, in the same commit, asserts "NaN, negative and absent sessions are not accepted"
    (`ClosedFormWindowOfThree`, `eccf6338` line 96).

### The C++ gives the new values (code trace, deterministic)

- **Quantiles.** `field_stats.cpp`: `neighbours` (`:261-273`) gives previous = -1, then `interpolate` (`:275-281`)
  gives gamma = 1, then `lerp` (`:244-253`) computes `b - diff*0 = -0.0`. `numpy_quantiles` returns
  `interpolate(values, at[k])` with no special case (`:355-357`). For n = 2 the kept branch gives `+0` (p < .5) and
  the overwrite branch gives `-0`.
- **Volume.** `trailing_mean.cpp`:
  - `push` rejects `v < 0` (`:57`);
  - `value` sums slots in slot order from `0.0` for two or more columns, and with `numpy_pairwise_sum` for one
    (`field_stats.cpp:285-319`: 8 accumulators, then the tail);
  - each new case's arithmetic, traced by hand, gives exactly the values above.
- **No contraction-sensitive expression.** Every product and sum is its own statement and nothing transcendental is
  involved, so Debug and Release cannot differ.
- **Why I did not rebuild.** I judged a run unnecessary, given the trace and the build provenance:
  - the last lane build `p9-a3-c` was `51f8158b` + 11 dirty entries;
  - those 11 are exactly `b07782e6` (6 files, no C++) + `27c22eb3` (5 files);
  - the non-comment lines of `27c22eb3` are 3 re-wrapped statements;
  - all three build logs have 0 "warning" lines.

  Root's merge gate runs the anchored filters (section 6).

**Conclusion:** the new expectations are the reference behaviour (numpy 1.26.4 + the vol_126 spec). This was not "an
expected value edited to fit": the code never changed (`ef921abd` touches test files only), and the old values match
no rule the code or the spec ever had. **A3-RED is accepted. The M1a-RED pair's root causes are as the lane states**,
with one provenance imprecision (S-5).

## 2. Spec compliance (brief + carry + rulings)

| Item | Result |
|---|---|
| **M1a-RED (required):** both gtests fixed, root cause stated, no expected value edited to fit | **Met.** Independently re-derived (section 1). Its own commit `ef921abd`, first on the branch, before any flip. |
| **Field-flip freeze:** fix before the flip; flip in its own commit | **Met.** `6262224f` is the last code commit (the registry rows and their pinning pytest only). The pre-flip tree stands alone: I extracted `27c22eb3` with `git archive` and ran 3 suites there, all exit 0 with 3 engine rows (section 5). The C++ fixture test flips the rows in-test (`research_fields_vendor_fixture_test.cpp:189-204`), so it is independent of the flip commit. |
| `VendorPanel`: hash once, one scan over the union, seal pushed down, observation contract once | **Met.** `vendor_panel.cpp:530-562` hashes and stamps before and after. One pass over row groups (`:348-374`). Sealed groups are pruned on statistics (`:356`). Keys are decoded first, values only for surviving rows (`:291-314`). Contract in `Assembler::add` (`:436-467`). Caveat S-1 on "sealed rows never decoded". |
| factor-break-v1 in C++ once per run, one implementation | **Met.** `sources/factor_break.cpp`, run in `Assembler::finish` (`vendor_panel.cpp:469-503`). The Python copies stay (Forbidden list; rule 4). |
| Builder kinds for the price-module and ohlc fields | **Met for 6 fields.** `coskew_60m` / `xrd0_ttm` stay Python with a stated reason (`vendor_fields.hpp:18-20`). The scope cut needs the PM's acknowledgement (S-6). |
| Registry rows `kind: engine` | **Met** (`6262224f`). Formula SHA is unchanged. Owner suffix `engine-shim:` (`b07782e6`). The `builder` = kind id agrees with `kKinds` (`registry.cpp:228-240`). |
| Gtests `HashOnce`, `SealPushDown`, `FactorBreak.ClosedForm`, a leak probe per builder, fixture byte identity | **Met.** Names carry the target prefix (`ResearchFieldsVendorPanel.*`, `ResearchFieldsFactorBreak.ClosedForm`, `ResearchFieldsVendorFields.*`, `ResearchFieldsVendorFixture.*`). |
| A3-1: minimal routing edit, cross-lane, listed | **Met** (section 3). Listed in the report. |
| A3-2 P13 untouched / A3-3 re-hash under reuse / A3-4 fixture size | Accepted per ruling. Fixture is 354 KB on disk. |
| A1-SHIM | Moved to wave 3 by Ruling A3-SHIM; not required. |
| A1 merge state (`regenerate()` round trip) | Holds: `test_field_registry.py` 35 passed (section 5). |
| Carried A1 / A2 minors | A2 `research_fields_cli.hpp:20` (115 columns) fixed. The rest are explained as other lanes' files. |
| Forbidden: Python builders | **Respected.** `git diff 1239a5ff f7941ab0` is empty on `prepare_research_fields.py`, `research_fields_price.py`, `research_fields_ohlc.py` and the shims. |
| Files in scope / cross-lane edits listed | Yes. Every file outside `sources/**`, new builder files, tests, fixtures and registry rows is in the report's cross-lane table. CMake edits are appended at the list tails (`atx-engine/CMakeLists.txt:365-372`, `tests/CMakeLists.txt:441-446`). |
| SQL2-CLS | No new schema literals. |
| Seal / blind | No real data. The fixture's 2023-12 / 2024 rows are synthetic seal probes (allowed). `kSpecialClosures` "2025-01-09" is a rule constant ported from Python, not data. |

## 3. Quality

### Point in time, per builder

| Builder | Clock | Code check | Probe check |
|---|---|---|---|
| ret_overnight | lag 1 | `a = prefix + t - 1` (`vendor_fields.cpp:263`). Reads rows `a`, `a-1` and kept gaps ending in `(a-1, a]` (`:431`). | `OpenReturnsLookAheadProbe` (`research_fields_vendor_fields_test.cpp:238-246`): the builder's first changed row is pinned **EXPECT_EQ t0+1**, and the honest kernel must equal the builder's payload. |
| ret_intraday | lag 1 | Reads row `a` only. | Same probe and pin as ret_overnight. |
| ceq_iss_5y | lag 1 | `a`, `b = a-1260` (`:291`, `:501`). C-81 withholding only once an above-ceiling row dated <= day(a) is known (`:513`). Kept gaps in `(o_b, a]`. | `CeqIssuanceLookAheadProbe` pinned EXPECT_EQ t0+1. |
| ceq_iss_5y A8 share lag | 90 days | `ShareIndex::lagged` (`:477-490`): last obs <= day(s) - 90 and >= day(s) - 490. | `CeqIssuanceSharesLagNinetyDays` (`:258-264`) pins EXPECT_EQ t0+91 on a consecutive-day axis (row a = t-1 needs day(a) - 90 >= day(plant)). An 89-day lag fails it. |
| open / high / low_adj | same session | `bar_row = prefix + t` (`:329`), plus the role's close / raw_close / present of row t. | `OhlcBarsLookAheadProbe` pins EXPECT_EQ t0. |

- **The probes have teeth.** Each probe:
  - asserts that the plant reaches the output;
  - pins the builder's own first-changed row with exact equality, so a builder one session leaky fails;
  - checks that the leaky kernel moves earlier, so the probe method can see a shift.
- **The panel window is point in time too.** It ends at `min(role last, seal-1)` (`vendor_panel.cpp:549`, `:202`),
  so no row after the role or the seal is loaded.
- **factor-break-v1 is point in time.** Each step's class reads rows <= its end row t, and each repair divides rows
  >= t (`:498`). So row r's chained factor uses only steps ending <= r.

### The A3-1 routing edit (`prepare_research_fields_engine.py`)

- **Minimal.**
  - `build` became `build_fields`, needed so that one call per panel gives the engine the Python panel's history.
  - For a single field the spec bytes, the tag (`<name>.*`), the error text and the formula lag are unchanged, so
    the three A2 fields behave as before. `test_research_fields_engine_path.py` gives 10 passed.
  - `lag_of` exists for the ohlc lag; it equals `price.LAG_SESSIONS` for the old three.
  - Hooks are installed only when the routed names meet `PRICE_FIELDS` / `BAR_FIELDS` and are restored in `finally`;
    the pytest asserts they are undone.
  - The edit holds no copy of any C++ rule.
- **`same_panel` fails closed** (`:255-271`).
  - Every Python panel statistic except `rule` must equal the engine's.
  - A missing engine `source_checks` block makes `got = {}`, so every non-None statistic raises.
  - The one-directional skip applies only to a Python `None`.
  - Bars also require the vendor SHA = the role's `source_sha256` and the role-file pins. ceq also requires the
    declared domain.
  - The refusal is demonstrated by `test_panel_guard_refuses_another_panel` (stand-in, run here) and
    `test_real_executable_refuses_a_longer_python_panel` (root, real exe).
  - The `pending` hand-off from `open_return_rows` to `ceq_iss_rows` is sound: `compute` loads shares iff
    `ceq_iss_5y` is wanted, and calls `ceq_iss_rows` in exactly that case (`research_fields_price.py:757-764`).
  - Residual: it passes vacuously on an empty stats dict (S-3).
- **No live caller passes `--engine-exe`.** `git grep` over `scripts/`, `atx-impl/` and `atx-engine/tools` (tests
  excluded) finds none, so the flip changes behaviour only for explicit `--engine-exe` runs (root's identity run).

### Flag-absent / registry-absent identity

- **C++.** `price_source` is optional; a spec without it parses and builds as before (`research_fields_cli.cpp:324-327`).
  - `BuildContext::plans` defaults to `{}`.
  - The receipt's `engine.fields` is `spec.fields`, not the kind table, so growing the table from 3 to 9 changes no
    existing receipt.
  - Nothing outside tests lists `engine_field_names()`.
  - `TinyRole`'s defaults keep the existing role manifest bytes: same file order, same zero SHA.
- **Python.** With no `--engine-fields`, the wrapper is the plain builder.
  - The registry entry without `--engine-exe` now prints a stderr note for the six and calls the same `build_main(rest)`.
  - Module binding order is unchanged by the flip. The ohlc rows now bind through the owner `shim:` ->
    `DRAFT_MODULES = (research_fields_ohlc,)` at the same row (75). The price rows are builder-owned; the builder
    binds `research_fields_price` itself either way.

### Build, style, safety

- **/W4 /WX.** Three wrapper builds, exit 0; 0 "warning" lines in `mega-p9-a3-{a,b,c}-build.log`. No line of any
  changed `.cpp` / `.hpp` exceeds 100 columns at head.
- **Lifetime.** `ShareIndex` holds a non-owning pointer to a panel that outlives it. `Scan` / `ScanWindow` references
  are scoped to `scan_file`. `BuildContext::plans` points at the caller's vector.
- **Error paths.** Arrow statuses are mapped. `load` catches `std::exception`. `chunk_of` checks the type id before
  each `static_cast`. Null dates and ids are dropped by key.
- **Determinism.** No unordered containers; JSON is `nlohmann::json` (sorted keys); `set_use_threads(false)`.
- **Tests pin the contract, not the implementation.**
  - Fixture identity is against the EXISTING Python builder's output, regenerated by a pytest (`test_vendor_panel_fixture.py`).
  - `HashOnce` moves the file away after the first kind; a fresh context must then fail.
  - `SealPushDown` overwrites sealed chunks with 0xFF and checks that a decoding reader fails on them.
  - `FactorBreak.ClosedForm` is hand-derived.

## 4. Findings

| # | path:line | Tag | Problem | Fix |
|---|---|---|---|---|
| S-1 | `atx-engine/src/research/fields/sources/vendor_panel.cpp:306` | Suggested (minor) | **Sealed values are decoded in a straddling row group that has a surviving pre-seal row.** That group's value chunks are read whole. `SealPushDown` covers only a straddling group with **no** surviving row (the fixture's 2023-12-27/28 rows lie after the role). So these claims overstate: the report (`task-A3-report.md:177-178`, "a straddling group has only its keys decoded"), the test header (`research_fields_vendor_panel_test.cpp:4`, "a sealed row's values are never decoded") and the fixture doc (`make_vendor_panel_fixture.py:16`). The values never reach an output, the header comment `vendor_panel.hpp:16-19` is accurate, and this matches the gold reader's pushdown level. K-P9-2 asks for a "seal-filtered column scan", which this is. | Correct the three claims. Optionally add a counter (for example `rows_sealed_in_value_decoded_groups`) so the TRAIN receipt shows whether any sealed value was decoded; or read value pages only up to the last surviving row of a date-sorted group. Root check: section 6, item 4. |
| S-2 | `atx-engine/src/research/fields/research_fields_cli.cpp:384` (with `prepare_research_fields.py:2576`) | Suggested (note, wave 3) | The price fields' bits depend on the panel history, which is set by the co-requested fields (ledger candidate 2). Under `--reuse` only `to_build` forms the union request, so rebuilding `ret_overnight` while `ceq_iss_5y` is reused uses a 2-session history: same `formula_sha256`, possibly different last bits from a fresh build. Python does the same: `compute` gets the non-reused fields only. This is a pre-existing property that A3 faithfully ports, not a defect. | Wave 3: put the history extent (or the co-requested price set) into the reuse key, or build the union from all requested plans. No A3 change required. |
| S-3 | `atx-engine/tools/prepare_research_fields_engine.py:264` | Suggested (minor) | `same_panel` passes vacuously when the Python panel's `stats` is empty or missing keys. | `if not stats: raise EngineError(...)` before the loop. |
| S-4 | `atx-engine/src/research/fields/vendor_fields.cpp:268,296` | Suggested (minor) | `session_calendar_pin()` (the NYSE sessions from 1970 to the seal, plus SHA-256) is recomputed for each of the three price fields in a build. | Compute it once (a function-local static, or on the panel). |
| S-5 | `.superpowers/sdd/platform-p9-20261003/task-A3-report.md:107` | Suggested (minor) | Root-cause provenance: `e812a0fd` did not add the numpy lerp. The `gamma >= 0.5` lerp and the `-1` index marking were already in `eccf6338` (`field_stats.cpp:61,72`); `e812a0fd` moved the lerp into a helper and added the hand-derived `{-0.0}` expectation, which contradicted that existing comment. The substance of the root cause stands. | One-line correction in the report or the ledger entry. |
| S-6 | `atx-engine/include/atx/engine/research/fields/vendor_fields.hpp:18-20` | Suggested (PM acknowledgement) | The brief says "builder kinds for the price-module ... fields". `coskew_60m` (a DuckDB Kahan `fsum`, order-dependent) and `xrd0_ttm` (reads other fields) stay Python. The reasons are sound, but no ruling records the scope cut. | PM records the two as wave-3 / Python-kept. |

There are no Required findings.

## 5. Evidence (re-run in `C:/atx-wt/pool-12` at `f7941ab0`)

All runs used `PYTHONDONTWRITEBYTECODE=1`, `-p no:cacheprovider`, explicit paths, each file in its own session, and
`ATX_RESEARCH_FIELDS_EXE` unset, so the real-exe tests skip. Base temp was a scratch dir, deleted afterwards.

| Command (`"C:/Program Files/Python312/python.exe" -m pytest -q -p no:cacheprovider <file>`) | Exit | Tail |
|---|---|---|
| `atx-engine/tests/fixtures/research_fields/test_vendor_engine_path.py` | 0 | `5 passed, 3 skipped in 11.57s` (skips: `set ATX_RESEARCH_FIELDS_EXE (root)`) |
| `atx-engine/tests/fixtures/research_fields/test_research_fields_engine_path.py` | 0 | `10 passed, 1 skipped in 36.33s` |
| `atx-engine/tests/fixtures/research_fields/test_vendor_panel_fixture.py` | 0 | `3 passed in 3.36s` (re-runs the Python builder and reproduces every committed byte) |
| `atx-engine/tests/fixtures/research_fields/test_research_fields_fixture.py` | 0 | `2 passed in 3.99s` |
| `atx-engine/tools/test_field_registry.py` | 0 | `35 passed in 34.10s` |
| `atx-engine/tools/test_no_new_python_builder.py` | 0 | `4 passed in 0.71s` |
| **Pre-flip tree** (`git archive 27c22eb3` of `atx-engine/tools`, `atx-engine/tests/fixtures/research_fields`, `atx-impl/strategies/research_window.json`; 3 `"kind": "engine"` rows): `test_vendor_engine_path.py` / `test_research_fields_engine_path.py` / `test_field_registry.py` | 0 / 0 / 0 | `4 passed, 3 skipped` / `10 passed, 1 skipped` / `35 passed` |
| A3-RED numpy script (section 1) | 0 | every new literal `bit-equal=True`; old literals `bit-equal=False` under the spec rule, `True` only under the accept-negative rule |

My first pytest pass errored in fixture setup: I gave `--basetemp` a parent directory that did not exist (`WinError 3`).
I created the directory and re-ran; the results above are the re-run. No test failed.

Scratch I created and deleted afterwards: `a3-review-diff.txt`, `a3-review-numpy.py`, `a3-review-pytest-tmp/`,
`a3-review-preflip/`. I started no build and left no process running.

## 6. What root must check at merge

1. **Build:** `scripts\research-build.ps1 -Targets "atx-engine-research-fields,atx-engine-research-fields-tests,atx-research-fields"`.
   Then run:
   - the M1a-RED anchors: `--gtest_filter=ResearchFieldsWriter.QuantilesPartitionLikeNumpy:ResearchFieldsVolumeMean.*`;
   - the A3 suites: `ResearchFieldsVendorPanel.*:ResearchFieldsVendorFields.*:ResearchFieldsVendorFixture.*:ResearchFieldsNyseCalendar.*:ResearchFieldsFactorBreak.*`;
   - the whole target: 65 / 65.

   The wave-2 gate is 0 failed. The M1a-RED arithmetic is contraction-free, so a Release run should agree; run it
   too if the gate follows the C2-RED precedent.
2. **pytest with `ATX_RESEARCH_FIELDS_EXE=<build>\bin\atx-research-fields.exe`.** Run the 4 real-exe tests skipped
   here, each file in its own session.
3. **Merge order.** Merge `ef921abd..27c22eb3` with the pre-flip tree green. Merge **`6262224f` (the flip) only after
   root's TRAIN identity run**: each of the six `<name>.f64` SHA-256 values equals v15's. A3-RED is accepted by this
   review, so the M1a-RED freeze no longer blocks it.
4. **Seal (S-1), from the TRAIN run's `source_checks.*.source` counts only (no data read).**
   - If `rows_sealed_dropped == rows_in_row_groups_pruned_sealed`, every sealed row sat in a pruned group: none was
     decoded.
   - If it is larger, some sealed rows were in a read group. Check whether that group also had value-decoded rows,
     and decide on S-1's counter.
5. **RSS on TRAIN.** The union panel carries about 1,600 pre-role rows of bars (lane Concern 3).
6. **AL-SIG merges after A3.** Expect conflicts on `kKinds` / `registry.cpp`, the CMake tails and `ENGINE_FIELDS`
   (ALSIG-3). Keep both sides.
7. **With the flip, `--registry ... --engine-exe` fails closed in two field-list shapes, by design:** a list with
   `coskew_60m` and an open return but not `ceq_iss_5y`; or a list with `ceq_iss_5y` not routed while the open
   returns are. If v15's field list contains `ceq_iss_5y`, the identity run is unaffected.
