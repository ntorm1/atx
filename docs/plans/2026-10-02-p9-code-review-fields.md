# P9 code review — field builders stage (sub-reviewer, pool-2 @ d7c1c520, read-only)

Supplement to 2026-10-02-p9-code-review.md.

FIELD BUILDERS stage review: worktree C:/atx-wt/pool-2 @ d7c1c520. This was read-only. Nothing was edited, built or run.

Sizes (wc -l):
- Python: builder 3,199; 13 modules 7,674; 5 shims 409; support files 408 (research_window 140, record_store 78, code_fingerprint 190).
- Python tests: 9,772 lines in 22 tools/test_* files, plus 411 lines of fixture tests.
- C++: 783 hpp + 2,305 cpp + 1,148 test.
- The audit's line base for migration 2 is out of date. audit:194 uses "8,891" lines and "eight modules"; it is now 13 modules and 10,873 lines. The +2,050 lines from YDATA were already flagged as p9-code-review F-6 (p9:42-44).
- record_store.py is not used by the fields pipeline. It serves the fitter and the report card (record_store.py:10-11).

## 1. Pipeline map (all Python unless marked C++)
- **Registration** (four mechanisms):
  - (a) Inline dicts: FIELDS prep:194-278, ISSUER_FIELDS 411-440, SV_FIELDS 480-504, ALL_FIELDS 507, FIELD_PRODUCERS 537-539.
  - (b) FIELD_MODULES `bind()`: sec prep:511-513; price and v8 prep:3188-3196; v9, xdata, gold, ohlc, ivshape, mgr13f, divevent, deals and connected only through the shims (draft:30-44, xdata:28-42, ohlc:23-37, ydata:45-59).
  - (c) Holdings wraps `run`/`main`/`publish` (prep:3186-3187 → holdings:1397-1475).
  - (d) The engine shim swaps `finra_field` and `price.volume_mean_rows` at runtime (engine.py:146-160).
  - C++: hard-coded `kEngineFields` (research_fields_cli.cpp:28) and name dispatch `build_one` (cli.cpp:124-133).
- **Builder:** group functions finra 885, th 1019, lake 1355, role/mkt_ret 1469, issuer 1874, sv 2322, dispatched in run() (prep:2496-2547). Module `compute` follows (prep:2545-2547, e.g. price:738-784). Holdings computes inside the `publish` hook (holdings:1421-1447). C++: build_finra_field (finra_asof_field.cpp:148-227) and build_vol_126 (volume_mean_field.cpp:46-70).
- **Payload:** FieldWriter, date-major `<f8` with canonical NaN (prep:686-742). C++ equivalent is field_writer.cpp.
- **Manifest:** entries prep:2548-2579; manifest prep:2582-2605 (code identity of prepare_research_fields.py only, 2603); fsync, then `os.link` publish-last (620-630). C++ writes only a receipt (cli.cpp:224-233). Migration slice 3's "publish-last manifest, reuse decision" in the exe (migration:97) is not implemented.
- **Reuse:** `reuse_fields` prep:2934-3137 and `reuse_module_fields` 2780-2862. They check formula_id (2639-2641), the AST closure (2721 → code_fingerprint.py:165-182), the role binding (load_prior 2916-2931) and a re-hash of sources (2982-3018).
- **Consumer:** research_cycle fields_step (research_cycle.py:1210-1262; one builder path, no engine flags); IC runner `--*-fields` (strategy_ic_runner.cpp:816); NAV (strategy_nav_replay.cpp:1975-2012); risk (strategy_risk_verb.cpp:173); marginal (strategy_marginal_ic.cpp:48).
- **Tests:**
  - pytest: test_prepare_research_fields{,_draft,_module_reuse,_reuse,_sec,_sic,_sv}.py, test_research_fields_{connected,deals,divevent,gold,holdings,holdings_xsw,ivshape,mgr13f,ohlc,price,v8,v8_quarters,v9_earn,v9_nt,xdata}.py, test_research_window.py; fixtures/research_fields/test_research_fields_{fixture,engine_path}.py.
  - gtest: `atx-engine-research-fields-tests` (tests/CMakeLists.txt:368-380), suites ResearchFields{Clock,Writer,VolumeMean,FinraAsof,Fixture,Cli}.
  - It is deliberately not registered with CTest (tests/CMakeLists.txt:364-367).
  - code_fingerprint.py has no test of its own; only test_research_fields_v8.py:227 touches it.
  - engine.py:13 cites `test_prepare_research_fields_engine.py`, which does not exist. The real test is fixtures/.../test_research_fields_engine_path.py.

## 2. Adding a new field today
- **No spec-driven path.** Every field needs Python code.
- **Policy forbids editing the builder** (draft:3-5). An edit there changes `code_sha256` in every manifest.
- **Module route** — a new research_fields_X.py containing:
  - FIELDS (or `_spec`), PRODUCERS, HOST_HANDLES;
  - `bind`, `producer_group`, `field_spec`, `reuse_inputs`, `entry_inputs`;
  - `imported_code` plus an IMPORTS list if it imports another module;
  - an XFieldModule with `add_arguments`/`check`/`compute`;
  - then a shim or one more tuple element in a shim, and tests;
  - promotion later needs a hook line at prep:3188-3196.
  - Example: divevent is a 250-line module for one field; about 80 of those lines are registration/reuse boilerplate (divevent:94-110, 179-250).
- **Engine route:**
  - a new .cpp/.hpp with the spec text copied verbatim (finra_asof_field.cpp:25-63; volume_mean_field.cpp:13-37);
  - `kEngineFields` plus dispatch (cli.cpp:28, 124-133);
  - Python ENGINE_FIELDS (engine.py:38);
  - a hand-written producer interceptor (engine.py:116-143).
- **Nearest thing to spec-driven:** gold's `declare()` (gold:95), still inside a Python module.
- **Shims: 5** (draft 53, xdata 51, ohlc 46, ydata 68, engine 191). Four of them carry a byte-identical `register()`/`main()` (draft:30-53, xdata:28-51, ohlc:23-46, ydata:45-68).
  - I agree with P9 F-12 that this is the copy pattern, with a refinement: the shims fork no builder logic (they import it), unlike generate_fund_ic_v4..v71.
  - The larger copy surface is the per-module boilerplate repeated across 11 modules, plus version-named modules: research_fields_v8.py, research_fields_v9.py, and `DRAFT_VERSION="v13"` (draft:25).
  - No single entry point builds a v13 list. It takes a hand one-liner `d.register(vars(b)); x.register(...); o.register(...); b.main(argv)` (ohlc:11-12).
  - I disagree with the migration plan on one point: it put `--engine-fields` on the builder (migration:81-82); the implementation added a fifth shim instead (engine.py:15-16).

## 3. Correctness
**Look-ahead: no leak found in the paths I traced.**
- FINRA: strict `available_day < d` (prep:920 searchsorted left −1) and age > 45 → NaN (924). C++ is identical (asof_series.cpp:53-60).
- Issuer: `clock < mark(t−L)` (prep:1866-1871, 1944-1949). Links: `avail ≤ mark(t)` (1712-1713).
- Spine: formation strictly before the session (1424). Presence is non-PIT, flagged opt-in (248, 280).
- sv: files d−126..d−1 (2347-2357). Ticker map: rows ≤ file date (2267-2272).
- shares_out: lag ≤ d−90, and a factor-break k is applied only when t ≤ day (1204-1216). C-81 is point-in-time (1225).
- price: rows ≤ t−1 (price:477, 527); a repaired step is divided out from t onward (price:357).
- Holdings: `available_at < mark(t−1)` (holdings:82). SEC: `usable_from` (sec:318-321).

Look-ahead risks:
- Five hand-written spellings of the same clock rule.
- Probes with a planted-leak counter-test exist in only 6 Python test files (ohlc, ivshape, xdata, gold, connected, mgr13f). The builder's own groups (test_prepare_research_fields.py:956, 1370), price (:275), divevent, deals, sec, v8, v9 and holdings have probes or visibility tests without teeth. C++ has teeth (finra_asof_test:113, volume_mean_test:138).

**Seal enforcement:**
- Writer side: Role refuses `days[-1] ≥ SEAL` (prep:670-672; C++ role_axes.cpp:160-161). Per-source drops at prep:902-904, 1371-1372/1386, 1681, 1743, 1842; holdings:632/822/899/928/946/1027/1146; sec:526; gold:169/209-218. C++ drops sealed FINRA rows in finra_asof_field.cpp:101.
- Reader side: no consumer reads the manifest's `seal.exclusive_end` (prep:2592; zero hits for `exclusive_end` in atx-impl/src). load_prior does not compare the prior's seal (prep:2916-2931).
- The TH readers decode sealed rows' value columns before dropping them (prep:1059-1063; price:308-312; ohlc:137-142; ivshape:163-168). This contradicts "never opened" (research_window.py:8); gold pushes the filter down (gold:212-214).
- The whole tools pytest suite runs under the superseded seal 2025-01-01 (conftest.py:15), so every Python seal or refusal test checks the wrong boundary.
- The stats key `rows_available_on_or_after_2025_dropped` (prep:948, 1689, 1754; cli.cpp:116) mislabels the 2024-01-01 seal.

**Hash-pin gaps:**
- (a) code_fingerprint hashes an import statement, not the imported module (code_fingerprint.py:36-37, 106-116); patched per module with hand-kept IMPORTS (divevent:54, v9:163-165) and data pins (session_calendar price:203-214, SIC table prep:1813, seal_pin holdings:1224); nothing verifies IMPORTS is complete.
- (b) Library and interpreter versions (numpy, pyarrow, duckdb, python) recorded nowhere. vol_126 depends on numpy's axis-0 slot-order sum (price:619; C++ copies it trailing_mean.cpp:40-46); coskew on DuckDB `fsum` (price:402).
- (c) Fingerprints are of source on disk, not executed code: runtime monkeypatches escape (holdings `publish` holdings:1450; engine.py:153-156).
- (d) Handle reads with a non-constant key not followed (code_fingerprint.py:159).
- (e) Manifest pins only prepare_research_fields.py (prep:2603); shim identity and registration order unrecorded, and that order sets manifest field order (prep:2444).
- (f) Absolute paths in manifest bytes (prep:2584, sources) so the pinned manifest SHA differs per worktree.

**Nondeterminism:** none found (single-threaded reads, DuckDB threads=1 price:409, mkt_ret math.fsum prep:1510, index tie-breaks prep:1747-1748). Hazard: reduction order tied to numpy version (b).

**Memory:** Budget cooperative, polled at boundaries only (prep:585-617); default cap 700 MiB (prep:588) vs 1,536 from the cycle (research_cycle.py:1239); whole-panel matrices built per module, not shared (prep:1047-1053, price:300-304, ohlc:133-134, ivshape:157-159, divevent:117-125, gold:258-259, link matrix prep:1710-1711); mgr13f reads the whole shares_out payload to hash it (mgr13f:293); DuckDB RSS not polled (price:406-416); engine child has no RSS cap (engine.py:83-84) and loads the CSV whole (finra_asof_field.cpp:20, 162).

## 4. Modularity

Copy-paste: TickerHistory3 panel reader x5 (prep:1059-1131, price:308-353, ohlc:137-164, ivshape:163-200, xdata:241); `verify_*source` x3 (+2 inline); `imported_code` x6; shim `register()` x4; `group_median` (holdings:359-369 vs v8:435-441, column_median prep:788); `average_ranks` two algorithms under one name (v9:412-416 O(m^2) vs mgr13f:229-235); helpers holdings:289-294/453-467 duplicate prep:550-582 (`date_of` returns dt.date vs str; `_sha_file` no budget polling); issuer selection prep:1944-1958 vs v8 LatestRows 247-267; "read this run's payload" x6.
Implicit coupling: absolute default paths incl. a user Downloads path (prep:185-187); atx_db imported via path walk (prep:1787-1794); cross-module CLI option dependencies (connected after mgr13f ydata:34-35; v9 reads SEC/builder options v9:517-519); fields depend on each other through files in the output dir (prep:1194, 1928; price:641); holdings computes a carrier mkt_ret then deletes it (holdings:1424-1427).
Flags bypassing the registry: `--engine-fields` sidesteps code identity (F1); `--sic-events` changes data behind grp_* with unchanged formula_id, guarded only by reuse roots (prep:2477).

## 5. C++ fields library

Ported: si_shares, si_dtc, vol_126. Exe atx-research-fields EXCLUDE_FROM_ALL (CMakeLists.txt:281-282). `--engine-fields` wired only through prepare_research_fields_engine.py, not the builder or research_cycle (research_cycle.py:1224-1239). Proven closed-form: AsofRuleClosedForm (finra_asof_test:73), ClosedFormWindowOfThree (volume_mean_test:76), planted-leak probes (:113, :138), numpy-exact sum/quantile/min/max (writer_test:24-140), seal refusal (writer_test:171), formula SHA = Python's (clock_test:89-114), receipt = Python manifest (cli_test:99). Fixture: byte identity on 8 x 300 (fixture_test:141-200; test_research_fields_fixture.py). NOT proven: gtest outside CTest; engine-path test runs a Python stand-in not the exe (:46-82, 117-128); real-exe test skipped unless ATX_RESEARCH_FIELDS_EXE; no committed TRAIN identity run (migration:83-84). Primitives generic (AsofSeries, TrailingMean, FieldWriter, RoleAxes, RoleRowReader, field_stats); builders one .cpp each with spec text copied verbatim and name dispatch; planned registry.hpp (migration:25) does not exist.

## 6. Performance

Fields v13 44.0 s, v14 72.8 s (p9:62); one-field build 59-121 s before reuse. No stage timings logged (Budget.report prints RSS only, prep:615-617). TickerHistory3 dominates: up to 7 full SHA-256 passes over one file (prep:1032, 2164, price:268, xdata:561, divevent:232, ohlc:113, ivshape:131; th_known cache reaches only sv); ~10 full parquet scans (prep:1059, 2203, 2232, price:308, DuckDB price:385, xdata:241, 569, divevent:233, ohlc:137, ivshape:163); factor_breaks rerun per source_panel call (prep:1133, price:354). Reuse still re-hashes TH and every CNMS window file (prep:3010, 2965-2971).

## 7. Top 5 findings (impact x effort)

1. **Engine path records C++ payloads as Python-produced (S)** — engine.py:7-13, 98-105; prep:2603; cli.cpp:224-233. Manifest byte-identical to the Python path's; receipt has no exe identity; guards check only self-consistency; nothing compares the payload with Python's output -> a C++ regression publishes under the Python identity and `--reuse` (prep:3024-3077) carries it forward. Fix: entry `producer: {engine, exe_sha256, git_sha, receipt_sha256}`; identity claim on payload + coverage only; key reuse of engine entries on exe identity.
2. **Reuse fingerprint blind to libraries and imports (S-M)** — code_fingerprint.py:36-37, 106-116; price:619; trailing_mean.cpp:40-46. Fix: record versions in manifest and reuse key; test that each PRODUCERS closure's cross-module names are in IMPORTS; register the gtest with CTest under a label.
3. **Five TickerHistory3 readers, 7 hashes per build (M)** — dominant wall time; observation / duplicate rules drift (ohlc:157-158 stores bars without price's observation contract). Fix: one shared vendor-panel object per run (hash once, one scan over the union of columns, factor_breaks once) passed through the host; becomes C++ fields/sources in slice 4.
4. **Four registration mechanisms, no single entry (M)** — prep:194-544, 511-513, 3186-3196; holdings:1397-1475; engine.py:146-160; ohlc:11-12; research_cycle.py:1224. Fix: one JSON field registry (name -> module/kind, spec text, requires, options) read by one builder CLI and by C++; holdings onto FIELD_MODULES (reuse interface exists holdings:1216-1245); delete the four shims (P9 F-12 extended).
5. **Seal discipline gaps (S / M)** — conftest.py:15 binds tests to the superseded 2025 seal; stats key mislabels the boundary (prep:948/1689/1754; cli.cpp:116); no consumer reads `seal.exclusive_end` (prep:2592); load_prior ignores the prior's seal (prep:2916-2931); TH readers decode sealed rows before dropping (prep:1059-1063). Fix: regenerate fixtures under the repository window, drop the conftest bind; rename key to `rows_sealed_dropped`; consumers and load_prior refuse when seal != kSealBeginDate; push `tradingDate < seal` filters down.

## Disagreements with YARCH

- Reuse is not just glue: the AST-based reuse decision (prep:2616-3137, ~520 lines) cannot cover C++ producers; redesign (spec + exe identity) in slice 3, not kept as Python glue.
- Manifest identity is the bug: "flag absent = byte-identical" is fine; byte-identical manifests with the flag PRESENT cause F1.
- Freeze before slice 4: registry first (F4) and a freeze on new Python builders before more porting; otherwise each lane keeps adding modules (p9:42-44). Audit line base out of date: migration 2 is now 13 modules / 10,873 lines, not 8 / 8,891.
