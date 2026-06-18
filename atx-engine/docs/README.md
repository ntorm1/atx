# ORATS zip → atx-tsdb partition: build & run guide

How the atx-engine turns a single ORATS history `.zip` into a per-trading-date
atx-tsdb segment partition, and how to build and run it. Written for future
agents picking this up cold.

---

## 1. What it does

`load_orats_history` streams one entry of an ORATS history zip (a multi-GB,
single deflate stream of date-major TSV) and writes one sealed atx-tsdb segment
per trading date, plus two side-cars.

```
tbltickerhistory3_10y.zip          (3.3 GB, 1 deflate entry, date-major TSV)
        │
        ▼   load_orats_history(cfg)
  ┌─────────────────────────────────────────────┐
  │ producer:  inflate → frame lines → route by  │   single thread, ordered
  │            date, extract key cols, symbology │
  │ writer pool: parse 16 value cols → pivot →   │   W = clamp(cores-1, 1, 8)
  │            build_from_long → write <date>.seg│
  └─────────────────────────────────────────────┘
        │
        ▼   <out_dir>/  (== data/orats_history_1d in production)
   2020-01-02.seg  2020-01-03.seg  …  (one per trading date; symbol = securityID)
   _symbology.parquet                 (securityID → ticker_tk, todayTicker)
   _manifest.json                     (row counts, fields, min_date)
```

Downstream, `build_history_panel` attaches that partition into a research Panel
(see §9). The loader is the *only* zip-aware step; everything after reads `.seg`.

**Public API** — [atx-engine/include/atx/engine/data/orats_history.hpp](../include/atx/engine/data/orats_history.hpp):

```cpp
struct OratsLoadConfig {
  std::string zip_path;       // .../tbltickerhistory3_10y.zip
  std::string out_dir;        // output partition dir (created if absent)
  atx::i64    min_date_nanos; // inclusive floor; rows with tradingDate < this are dropped
  atx::i64    created_at_nanos;// stamped into every segment header (provenance)
};
struct OratsLoadStats {       // rows_read == rows_filtered + rows_malformed + rows_kept
  atx::i64 rows_read, rows_kept, rows_filtered, rows_malformed;
  atx::i64 dates_written, distinct_securities;
};
atx::core::Result<OratsLoadStats> load_orats_history(const OratsLoadConfig& cfg);
```

Implementation: [atx-engine/src/data/orats_history.cpp](../src/data/orats_history.cpp).

---

## 2. Quick start

From the repo root, in a VS Developer environment (the helper script sources it
for you). PowerShell:

```powershell
# Build the data test group + the load operator (Debug, default preset)
scripts\atx-build.ps1 configure -Groups data
scripts\atx-build.ps1 build atx-engine-data-tests

# Run the loader on a real zip via the operator test (env-gated):
$env:ATX_ORATS_ZIP   = "C:/path/to/tbltickerhistory3_10y.zip"
$env:ATX_DATA_DIR    = "C:/path/to/data"     # optional: writes <ATX_DATA_DIR>/orats_history_1d
$env:ATX_ORATS_PROFILE = "1"                  # optional: print the inflate/parse/build split
build\bin\atx-engine-data-tests.exe --gtest_filter=OratsE2ESmoke.OperatorOratsZip
```

For real throughput build **Release** with the faster inflater on — see §3 and §8.

---

## 3. Build setup

**Prereqs**
- Visual Studio 2022 (clang-cl + the Windows SDK) — the toolchain is clang-cl
  with the Ninja generator.
- vcpkg, with `VCPKG_ROOT` set. Deps come two ways: vcpkg manifest
  ([vcpkg.json](../../vcpkg.json): arrow+parquet, zstd, zlib-ng, openssl, gtest) and
  FetchContent (spdlog, eigen, etc., cached under `ATX_DEPS_DIR`).
- `ninja.exe` ships inside the VS install; the build helper puts it on PATH.

**Build helper** — [scripts/atx-build.ps1](../../scripts/atx-build.ps1). It sources
`vcvars64.bat` and adds VS-bundled Ninja to PATH, then forwards to cmake/ctest:

```powershell
scripts\atx-build.ps1 configure -Groups data [-Bench]   # cmake --preset ninja (+ test group, +bench)
scripts\atx-build.ps1 build <target>                    # cmake --build build --target <target>
scripts\atx-build.ps1 -Ctest -R <regex>                 # ctest in build/  (regex-safe; bypasses cmd)
```

**Presets** — [CMakePresets.json](../../CMakePresets.json). All are **Debug**:
`ninja` (default, clang-cl + PCH), `hygiene` (PCH off, CI), `dev` (sccache +
shared deps), `vs` (MSBuild). There is **no Release preset** — for a release
build, configure a separate dir manually (see §8).

**Relevant targets**
- `atx-engine-data-tests` — the data test group; contains the load operator test.
- `atx-core-tests` — contains the zip-reader unit tests.
- `atx-engine-bench` — google-benchmark suite incl. `BM_OratsLoad` (synthetic).

### `ATX_FAST_INFLATE` (faster inflate, opt-in)

The real load is **inflate-bound** (~86% of wall on the 3.3 GB zip). The option
`ATX_FAST_INFLATE` (default **OFF**) makes `ZipEntryReader` decompress DEFLATE
entries with **zlib-ng** instead of the vendored miniz. Default OFF → miniz path
verbatim, zlib-ng not linked, zero behavior change.

```powershell
cmake -B build-rel ... -DATX_FAST_INFLATE=ON   # see §8 for the full release configure
```

Measured: inflate **166 s → 54 s (3.07×)**, total zip→tsdb **211 s → 97 s (2.18×)**,
byte-identical output. Implementation: [atx-core/src/io/zip_reader.cpp](../../atx-core/src/io/zip_reader.cpp);
wiring in [atx-core/CMakeLists.txt](../../atx-core/CMakeLists.txt) and [CMakeLists.txt](../../CMakeLists.txt).

---

## 4. Running the load

**Option A — the operator test (recommended for one-off builds).**
`OratsE2ESmoke.OperatorOratsZip` in
[atx-engine/tests/data/orats_e2e_smoke_test.cpp](../tests/data/orats_e2e_smoke_test.cpp)
calls `load_orats_history` then drives a small downstream panel. It **skips
cleanly** unless `ATX_ORATS_ZIP` is set:

```powershell
$env:ATX_ORATS_ZIP = "C:/.../tbltickerhistory3_10y.zip"
$env:ATX_DATA_DIR  = "C:/.../data"          # out_dir = <ATX_DATA_DIR>/orats_history_1d; else %TEMP%
build\bin\atx-engine-data-tests.exe --gtest_filter=OratsE2ESmoke.OperatorOratsZip
```

Floor is hard-coded to `2020-01-01` in that test; edit it there to change the window.

**Option B — call the API directly** (in your own tool/target that links `atx::engine`):

```cpp
#include "atx/engine/data/orats_history.hpp"
using namespace atx::engine::data;

OratsLoadConfig cfg;
cfg.zip_path         = "C:/.../tbltickerhistory3_10y.zip";
cfg.out_dir          = "C:/.../data/orats_history_1d";
cfg.min_date_nanos   = *detail::date_to_nanos("2020-01-01"); // midnight UTC
cfg.created_at_nanos = 0;
auto stats = load_orats_history(cfg);              // Result<OratsLoadStats>
if (!stats) { /* stats.error().to_string() */ }
```

---

## 5. Output layout

Written to `cfg.out_dir`:

| file | content |
|------|---------|
| `YYYY-MM-DD.seg` | one atx-tsdb segment per trading date; one row per security; symbol name = `securityID`; time axis collapses to that date's midnight-UTC nanos |
| `_symbology.parquet` | `securityID` (i64, sorted asc) → `ticker_tk`, `todayTicker`; first-seen per security |
| `_manifest.json` | `rows_read/kept/filtered/malformed`, `dates_written`, `distinct_securities`, `min_date_nanos`, and the 16 `fields` |

The 16 segment fields are NaN-filled where the TSV value is empty/unparseable.

---

## 6. Projected fields & header mapping

The 16 segment field names (canonical, digest-stable order;
[orats_history.hpp](../include/atx/engine/data/orats_history.hpp) `kOratsFields`):

```
open high low close closePr closeUnadjPr volume shares
returnFactor totalReturn cumReturnFactor gics earnFlag
atmCenI_21d atmCenI_126d nEarnCnt_5d
```

Header → segment-name special cases (see `resolve_header`): TSV `GICS` → `gics`;
TSV `cumulReturnFactor` (17 chars) → `cumReturnFactor` (15-char segment limit).
Required columns: `tradingDate`, `securityID`, `ticker_tk`, `todayTicker`, plus
every mapped field — a missing one fails the load with `Err(ParseError)`.

---

## 7. Pipeline architecture

A single deflate stream is inherently serial to decompress, so parallelism comes
from the per-date independence of the output segments.

**Producer thread** (ordered, single-threaded): rolling 4 MiB inflate reads →
frame lines → for each row: parse `tradingDate` (route/guard/floor), extract
`securityID` + tickers (symbology), append the row's **raw bytes** to the current
date's buffer. On a date boundary it seals the previous date into a `DateJob` and
pushes it onto a bounded blocking queue.

**Writer pool** (`W = clamp(hardware_concurrency-1, 1, 8)` threads): pop a
`DateJob`, run the 16 `from_chars` float parses, pivot to columns,
`build_from_long`, write `<date>.seg`. The serial per-date build+write **and**
the per-row float parse run in parallel, hidden behind the producer's inflate wall.

**Determinism (hard requirement).** Output is byte-identical across runs and
across worker counts: each `.seg` is a pure function of its date's rows in input
order (`symbols[i]` pairs with `values[f][i]`); the symbology map, all stat
counts, and the manifest are produced single-threaded on the producer **after**
the pool joins. `build_from_long` also length-validates as a fail-closed backstop.
Regression test: `DataOratsHistory.ParallelOutputIsDeterministicAcrossRuns`.

History of the optimization (all on local `main`):
- **Phase 0–2** — profiling, serial micro-opts (rolling buffer, move-not-copy,
  capacity reserves), parallel per-date build+write pool.
- **Phase 3** — parallel per-row parse (move the 16× `from_chars` into the pool).
- **Phase 4** — `ATX_FAST_INFLATE` zlib-ng inflate (the dominant lever on real data).

Full plan + rationale: [docs/superpowers/plans/2026-06-17-orats-zip-fast-pipeline.md](../../docs/superpowers/plans/2026-06-17-orats-zip-fast-pipeline.md).

---

## 8. Profiling & measured performance

Set `ATX_ORATS_PROFILE=1` (any non-empty value) to print a stderr split:

```
[orats-profile] inflate=…ms producer_route=…ms worker_parse_build_write(workers,summed)=…ms rows_kept=… dates=…
```

- `inflate` — miniz/zlib-ng decompression on the producer (serial; the bottleneck).
- `producer_route` — framing + key-column extract + symbology (serial).
- `worker_*` — parse+build+write **summed across the pool**, overlapped behind the
  producer wall (≈0 marginal wall, not added to the total).

**Release, real 3.3 GB zip** (31.6M rows → 17.0M kept → 1621 segs, 20,413 securities):

| stage | miniz (OFF) | zlib-ng (ON) |
|-------|-------------|--------------|
| inflate | 166 s | 54 s (**3.07×**) |
| total zip→tsdb | 211 s | 97 s (**2.18×**) |

Debug builds are noise/heap-contention dominated and **not** representative —
always measure perf in Release.

### Configuring a Release build (no preset exists)

```powershell
$mt = '-DCMAKE_MT="C:/Program Files (x86)/Windows Kits/10/bin/<ver>/x64/mt.exe"'  # if clang-cl can't find mt
cmake -G Ninja -B build-rel -DCMAKE_BUILD_TYPE=Release `
  -DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl $mt `
  -DCMAKE_TOOLCHAIN_FILE=$env:VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake `
  -DVCPKG_TARGET_TRIPLET=x64-windows `
  -DATX_USE_PCH=ON -DATX_BUILD_BENCH=ON -DATX_TEST_GROUPS=data `
  -DATX_FAST_INFLATE=ON
# build via the same vcvars-sourced shell scripts/atx-build.ps1 uses, e.g. cmake --build build-rel --target atx-engine-data-tests
```

Note: the repo does not currently build clean under `/W4 /WX` in Release
(pre-existing unrelated warnings); add `-DATX_WERROR=OFF` for a throwaway perf build.

---

## 9. Downstream: building a research Panel

The segment partition is consumed by `build_history_panel`
([atx-engine/include/atx/engine/data/history_panel.hpp](../include/atx/engine/data/history_panel.hpp)):

```cpp
HistoryDataConfig hcfg;
hcfg.seg_dir = "C:/.../data/orats_history_1d";   // the load's out_dir
hcfg.window  = {start_nanos, end_nanos};         // [start, end) trading dates
hcfg.universe.min_adv_usd = 5.0e6;               // market-cap / ADV / sector / top-N screen
auto hp = build_history_panel(hcfg);             // Result<HistoryPanel> (digest-pinned)
```

It multi-attaches the `.seg` files, computes total-return close
(`close * cumReturnFactor`, raw kept), runs the universe screen, and emits a
deterministic Panel in `kHistField*` order. Two calls with identical inputs
return an identical digest.

---

## 10. Invariants & gotchas

- **Input must be date-major** (non-decreasing `tradingDate`). A regression fails
  closed with `Err(InvalidArgument)` — it is treated as malformed input, never a
  silent drop. Sub-floor rows are still guarded.
- **Counting invariant:** `rows_read == rows_filtered + rows_malformed + rows_kept`.
- **`min_date_nanos` is an inclusive floor**; earlier rows count as `rows_filtered`.
- **Determinism is non-negotiable** — keep symbology/manifest/counts on the
  producer post-join; never let workers share mutable state across dates.
- **`OratsE2ESmoke` tests skip without data** — `RealPartitionRuns…` needs
  `data/orats_history_1d` present; `OperatorOratsZip` needs `ATX_ORATS_ZIP`. The
  `Synthetic…` variant always runs (no data dependency).
- **`ATX_FAST_INFLATE` is OFF by default** — the default `ninja` preset uses
  miniz. Turn it ON for real loads.

---

## 11. Tests

```powershell
scripts\atx-build.ps1 -Ctest -R DataOratsHistory   # loader unit + determinism gate
scripts\atx-build.ps1 -Ctest -R IoZipReader        # zip reader (miniz + zlib-ng paths)
scripts\atx-build.ps1 -Ctest -R OratsE2ESmoke      # synthetic always-runs; real/operator gated
```

Key tests: `DataOratsHistory.*`
([data_orats_history_test.cpp](../tests/data/data_orats_history_test.cpp)) —
counts, framing across buffer boundaries, determinism across runs;
`IoZipReader.*` ([zip_reader_test.cpp](../../atx-core/tests/zip_reader_test.cpp)) —
streaming round-trip and corrupted-stream rejection (both inflate paths fail closed).

---

## 12. File map

| path | role |
|------|------|
| [atx-engine/include/atx/engine/data/orats_history.hpp](../include/atx/engine/data/orats_history.hpp) | public API: `load_orats_history`, `OratsLoadConfig/Stats`, `kOratsFields` |
| [atx-engine/src/data/orats_history.cpp](../src/data/orats_history.cpp) | the loader: producer + writer pool + framing + profiling |
| [atx-core/include/atx/core/io/zip_reader.hpp](../../atx-core/include/atx/core/io/zip_reader.hpp) | `ZipEntryReader` interface (`open`/`read`) |
| [atx-core/src/io/zip_reader.cpp](../../atx-core/src/io/zip_reader.cpp) | miniz + opt-in zlib-ng inflate, CRC32 validation |
| [atx-tsdb/src/load_parquet.cpp](../../atx-tsdb/src/load_parquet.cpp) | `build_from_long` — pivots long columns into a `.seg` |
| [atx-engine/include/atx/engine/data/history_panel.hpp](../include/atx/engine/data/history_panel.hpp) | `build_history_panel` — segs → research Panel |
| [atx-engine/bench/orats_history_bench.cpp](../bench/orats_history_bench.cpp) | `BM_OratsLoad` synthetic throughput bench |
| [scripts/atx-build.ps1](../../scripts/atx-build.ps1) | configure/build/test helper (sources vcvars + Ninja) |
