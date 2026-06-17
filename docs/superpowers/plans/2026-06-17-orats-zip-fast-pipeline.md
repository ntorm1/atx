# ORATS Zip → atx-tsdb Fast Concurrent Pipeline — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make `load_orats_history` (zip TSV → per-date `.seg` partition) as fast as possible by removing serial-path waste and overlapping the inherently-serial inflate+parse front with a parallel per-date build+write back end — while keeping byte-reproducible output.

**Architecture:** The input is a single deflate stream (`tbltickerhistory`), so decompression cannot be parallelized across threads. We therefore (1) cut copies/allocations on the serial path, then (2) split the loader into a single ordered **producer** (inflate → frame lines → classify/route by trading date, single-threaded, deterministic) feeding a bounded queue of per-date jobs to a pool of **writer workers** (pivot via `build_from_long` + write `.seg`, embarrassingly parallel since each date's segment is fully self-determined). Symbology + manifest stay producer-side so output is bit-identical to the serial loader. Faster-inflate (zlib-ng/ISA-L) and parallel-parse are decision-gated on Phase 0 measurements.

**Tech Stack:** C++20, miniz (zip central-dir + inflate), `atx::tsdb::SegmentBuilder`/`build_from_long`, `atx::core::Result/Status` (tl::expected), `std::thread` + `std::condition_variable` bounded queue, GoogleTest, google-benchmark.

## Global Constraints

- C++20; warnings gate `/W4 /WX` (`atx_warnings`) applies to every atx TU. Third-party C (miniz) is isolated in `atx_miniz` with `/w` — do **not** route new warnings through it.
- Error handling: expected-failures travel in `Result<T>`/`Status`; never throw across the API. Use `ATX_TRY` / `ATX_TRY_VOID`. `Result` is `[[nodiscard]]`.
- **Determinism is a hard requirement.** The existing loader produces byte-reproducible output (sorted symbology, per-date segments with integrity CRC + content hash). Every change here MUST preserve byte-identical output vs. the current serial loader. This is verified by an explicit equivalence test.
- The input contract is unchanged: TSV MUST be date-major (non-decreasing `tradingDate`); a regression fails closed with `Err(InvalidArgument)`.
- miniz include (`<miniz.h>`) stays confined to `.cpp` TUs and test targets (it is not in any public umbrella header).
- Public signatures `load_orats_history(const OratsLoadConfig&)` and `OratsLoadStats` MUST NOT change (callers: `orats_e2e_smoke_test.cpp`). Internal restructuring only.
- No new heavyweight dependency without an explicit gate (Phase 4 zlib-ng/ISA-L is opt-in behind a CMake option, default OFF).

## Files

- Modify: `atx-engine/src/data/orats_history.cpp` — serial-path fixes; producer/worker split; env-gated profiling.
- Modify: `atx-engine/CMakeLists.txt` — link `Threads::Threads` (needed by the writer pool).
- Modify: `atx-tsdb/src/load_parquet.cpp:40-48` — single-timestamp time-axis fast path.
- Create: `atx-engine/bench/orats_history_bench.cpp` — synthetic large-fixture generator + throughput benchmark (auto-globbed into `atx-engine-bench`).
- Modify: `atx-engine/tests/data/data_orats_history_test.cpp` — add a serial-vs-parallel byte-equivalence test and a multi-date stress fixture. (Confirm the file's current path first — the suite was reorganized into `tests/<group>/`; the data group globs `tests/data/*_test.cpp`.)
- Reference only (read, do not edit unless a task says so): `atx-core/src/io/zip_reader.cpp`, `atx-core/include/atx/core/concurrent/mpmc_queue.hpp`, `atx-tsdb/include/atx/tsdb/builder.hpp`.

### Worktree

Execute in an isolated worktree (the repo's `new-worktree.ps1` convention). Build the data test group + bench only:

```
cmake --preset ninja -DATX_TEST_GROUPS=data -DATX_BUILD_BENCH=ON
ninja atx-engine-data-tests atx-engine-bench
```

---

## Phase 0 — Measure first (no behavior change)

**Rationale:** Inflate, parse, and build+write are the three costs. We do not yet know the split. Everything after Phase 0 is gated on these numbers — do not optimize blind.

### Task 0.1: Env-gated profiling inside the loader

**Files:**
- Modify: `atx-engine/src/data/orats_history.cpp`

**Interfaces:**
- Produces: nothing public. Adds internal timing logged to `stderr` only when `ATX_ORATS_PROFILE` is set. `OratsLoadStats` and the public signature are unchanged.

- [ ] **Step 1: Add a profiling accumulator and read helper at the top of the anonymous namespace in `orats_history.cpp`**

```cpp
#include <chrono>
#include <cstdlib>
#include <cstdio>

namespace {
// Coarse wall-clock split of the serial pipeline, emitted to stderr only when
// ATX_ORATS_PROFILE is set (any non-empty value). Off => zero overhead beyond a
// few steady_clock reads, which are negligible vs. the work they bracket.
struct LoadProfile {
  using clock = std::chrono::steady_clock;
  clock::duration inflate{}, parse{}, build_write{};
  bool enabled{false};
  LoadProfile() {
    const char *e = std::getenv("ATX_ORATS_PROFILE");
    enabled = (e != nullptr && e[0] != '\0');
  }
  static double ms(clock::duration d) {
    return std::chrono::duration<double, std::milli>(d).count();
  }
  void report(const OratsLoadStats &s) const {
    if (!enabled) return;
    std::fprintf(stderr,
                 "[orats-profile] inflate=%.1fms parse=%.1fms build_write=%.1fms "
                 "rows_kept=%lld dates=%lld\n",
                 ms(inflate), ms(parse), ms(build_write),
                 static_cast<long long>(s.rows_kept),
                 static_cast<long long>(s.dates_written));
  }
};
} // namespace
```

- [ ] **Step 2: Bracket the three regions in `load_orats_history`**

Around `reader.read(...)` accumulate into `prof.inflate`; around the per-line `process_line` loop accumulate into `prof.parse`; around `flush_date(...)` accumulate into `prof.build_write`. Example for the inflate region:

```cpp
LoadProfile prof;
// ... inside the read loop:
const auto t0 = LoadProfile::clock::now();
auto read_res = reader.read(std::span<char>(buf.data(), buf.size()));
prof.inflate += LoadProfile::clock::now() - t0;
```

Wrap the line-processing inner `while (pos < chunk.size())` body's `process_line` call with `prof.parse` timing, and the two `flush_date` call sites with `prof.build_write` timing. Call `prof.report(stats);` immediately before `return atx::core::Ok(stats);`.

- [ ] **Step 3: Build the data test group to confirm it still compiles**

Run: `ninja atx-engine-data-tests`
Expected: clean build (no `/WX` failures).

- [ ] **Step 4: Run the existing tiny-zip test to confirm no behavior change**

Run: `ctest --test-dir build -R DataOratsHistory --output-on-failure`
Expected: PASS (all `DataOratsHistory.*` green; profiling silent because `ATX_ORATS_PROFILE` is unset).

- [ ] **Step 5: Commit**

```bash
git add atx-engine/src/data/orats_history.cpp
git commit -m "perf(orats): env-gated inflate/parse/build_write profiling"
```

### Task 0.2: Synthetic large-fixture benchmark

**Files:**
- Create: `atx-engine/bench/orats_history_bench.cpp`
- Modify: `atx-engine/CMakeLists.txt` (add `Threads::Threads` to `atx-engine` — needed now so later phases link cleanly; bench already links Threads)

**Interfaces:**
- Consumes: `atx::engine::data::load_orats_history`, `OratsLoadConfig`, `detail::date_to_nanos`, `kOratsFields`; miniz writer (`mz_zip_writer_*`).
- Produces: `atx-engine-bench` target gains an `orats_history_bench.cpp` (auto-globbed). A benchmark `BM_OratsLoad`.

- [ ] **Step 1: Add `Threads::Threads` to the engine library**

In `atx-engine/CMakeLists.txt`, after the existing `target_link_libraries(atx-engine ...)` block (around line 109-115), add:

```cmake
# The ORATS loader's writer pool uses std::thread; link the platform thread lib.
find_package(Threads REQUIRED)
target_link_libraries(atx-engine PUBLIC Threads::Threads)
```

- [ ] **Step 2: Write the fixture generator + benchmark**

`atx-engine/bench/orats_history_bench.cpp` — generates a realistic date-major TSV once into a temp zip, then times the full load. The header MUST be the real 71-column ORATS layout (copy `kHeader` from `data_orats_history_test.cpp`) so `resolve_header` succeeds.

```cpp
// atx-engine — ORATS loader throughput benchmark. Generates a synthetic
// date-major ORATS TSV (N_DATES x N_SYMBOLS) into a temp .zip ONCE, then times
// load_orats_history end-to-end. Set ATX_ORATS_PROFILE=1 to also print the
// inflate/parse/build_write split to stderr.
#include <benchmark/benchmark.h>
#include <miniz.h>

#include <filesystem>
#include <string>

#include "atx/engine/data/orats_history.hpp"

namespace {
namespace fs = std::filesystem;

// Real 71-column header (mirrors data_orats_history_test.cpp::kHeader).
constexpr const char *kHeader =
    "tradingDate\tsecurityID\tticker_tk\ttodayTicker\tdn\topen\thigh\tlow\tclose\tclosePr\t"
    "volume\tshares\tearnFlag\tccVar\thlVar\trvVar\texpiryCount\thEMove\tiEMove\tshD1\tlnD1\t"
    "atmCenI_decay\tatmCenI_st\tatmCenI_lt\tatmCenI_5d\tatmCenI_21d\tatmCenI_42d\tatmCenI_63d\t"
    "atmCenI_84d\tatmCenI_105d\tatmCenI_126d\tatmCenI_189d\tatmCenI_252d\tatmCenI_378d\t"
    "atmCenI_504d\tatmCenH_st\tatmCenH_lt\tatmCenH_decay\tatmCenH_5d\tatmCenH_21d\tatmCenH_42d\t"
    "atmCenH_63d\tatmCenH_84d\tatmCenH_105d\tatmCenH_126d\tatmCenH_189d\tatmCenH_252d\t"
    "atmCenH_378d\tatmCenH_504d\tnEarnCnt\tnEarnCnt_5d\tnEarnCnt_21d\tnEarnCnt_42d\tnEarnCnt_63d\t"
    "nEarnCnt_84d\tnEarnCnt_105d\tnEarnCnt_126d\tnEarnCnt_189d\tnEarnCnt_252d\tnEarnCnt_378d\t"
    "nEarnCnt_504d\tGICS\tcloseUnadjPr\treturnFactor\ttotalReturn\tcumulReturnFactor\twkD1\t"
    "atmCenI_10d\tatmCenH_10d\tnEarnCnt_10d\tqtrD1";

// Build the TSV body: kDates dates (advancing 1 day from 2020-01-02), each with
// kSymbols rows. Only the projected columns carry non-zero values; the rest are
// "0" so the row width matches the header.
std::string make_body(int kDates, int kSymbols) {
  std::string body;
  body.reserve(static_cast<size_t>(kDates) * kSymbols * 160);
  body += kHeader;
  body += '\n';
  // 2020-01-02 is unix-day 18263.
  for (int d = 0; d < kDates; ++d) {
    // Render YYYY-MM-DD by stepping a simple counter; the loader only needs a
    // valid, non-decreasing date string. Use 2020 + month/day arithmetic via a
    // fixed base; here we keep it inside a single year window for the bench
    // (kDates <= 360 keeps us within 2020).
    const int month = 1 + (d / 28);
    const int day = 1 + (d % 28);
    char date[11];
    std::snprintf(date, sizeof(date), "2020-%02d-%02d", month, day);
    for (int s = 0; s < kSymbols; ++s) {
      const int secid = 10001 + s;
      // tradingDate, securityID, ticker_tk, todayTicker
      body += date; body += '\t';
      body += std::to_string(secid); body += "\tT"; body += std::to_string(secid);
      body += "\tT"; body += std::to_string(secid); body += '\t';
      // remaining 67 columns: put a price in close (idx 8) and volume (idx 10),
      // zeros elsewhere. Columns 4..70 (0-based) after the first 4.
      for (int c = 4; c < 71; ++c) {
        if (c == 8) body += "123.45";
        else if (c == 10) body += "1000000";
        else body += '0';
        if (c < 70) body += '\t';
      }
      body += '\n';
    }
  }
  return body;
}

std::string write_zip_once(int kDates, int kSymbols) {
  static std::string cached;
  if (!cached.empty()) return cached;
  const fs::path p = fs::temp_directory_path() / "atx_orats_bench.zip";
  std::error_code ec;
  fs::remove(p, ec);
  const std::string body = make_body(kDates, kSymbols);
  mz_zip_archive zip{};
  mz_zip_writer_init_file(&zip, p.string().c_str(), 0);
  mz_zip_writer_add_mem(&zip, "tbltickerhistory3_10y.txt", body.data(), body.size(),
                        MZ_DEFAULT_LEVEL);
  mz_zip_writer_finalize_archive(&zip);
  mz_zip_writer_end(&zip);
  cached = p.string();
  return cached;
}

void BM_OratsLoad(benchmark::State &state) {
  const int kDates = static_cast<int>(state.range(0));
  const int kSymbols = static_cast<int>(state.range(1));
  const std::string zip = write_zip_once(kDates, kSymbols);
  const auto min_date = atx::engine::data::detail::date_to_nanos("2020-01-01");
  for (auto _ : state) {
    const fs::path out = fs::temp_directory_path() / "atx_orats_bench_out";
    std::error_code ec; fs::remove_all(out, ec);
    atx::engine::data::OratsLoadConfig cfg;
    cfg.zip_path = zip;
    cfg.out_dir = out.string();
    cfg.min_date_nanos = *min_date;
    cfg.created_at_nanos = 0;
    auto st = atx::engine::data::load_orats_history(cfg);
    if (!st) state.SkipWithError(st.error().to_string().c_str());
    benchmark::DoNotOptimize(st);
  }
  state.SetLabel(std::to_string(kDates) + "d x " + std::to_string(kSymbols) + "sym");
}
BENCHMARK(BM_OratsLoad)
    ->Args({240, 3000})   // ~720k rows, ~240 .seg files
    ->Unit(benchmark::kMillisecond)
    ->Iterations(3);
} // namespace
```

- [ ] **Step 3: Configure + build the bench**

Run: `cmake --preset ninja -DATX_BUILD_BENCH=ON -DATX_TEST_GROUPS=data && ninja atx-engine-bench`
Expected: clean build.

- [ ] **Step 4: Capture the baseline number (this is the reference all later phases beat)**

Run: `ATX_ORATS_PROFILE=1 ./build/bin/atx-engine-bench --benchmark_filter=BM_OratsLoad`
Expected: prints the benchmark wall-clock AND the `[orats-profile] inflate=… parse=… build_write=…` split. **Record these three numbers in the commit message** — they decide whether Phase 3 (parallel parse) or Phase 4 (faster inflate) is worth doing.

- [ ] **Step 5: Commit**

```bash
git add atx-engine/bench/orats_history_bench.cpp atx-engine/CMakeLists.txt
git commit -m "bench(orats): synthetic large-fixture throughput benchmark + baseline

baseline (240d x 3000sym): inflate=<X>ms parse=<Y>ms build_write=<Z>ms total=<T>ms"
```

---

## Phase 1 — Serial-path quick wins (no concurrency)

**Rationale:** Remove copies/allocations that the serial loop pays on every chunk and every date. Low risk, measurable, and they reduce the work the producer does in Phase 2.

### Task 1.1: Move (not copy) the accumulator into the segment build

**Files:**
- Modify: `atx-engine/src/data/orats_history.cpp` (`DateAccumulator`, `flush_date`)

**Interfaces:**
- Consumes: `atx::tsdb::build_from_long(const LongColumns&, ...)` (unchanged).
- Produces: `flush_date` now consumes the accumulator by move; `DateAccumulator::clear` re-initializes `values` to `kOratsFields.size()` empty vectors.

- [ ] **Step 1: Add a failing equivalence guard test**

In `atx-engine/tests/data/data_orats_history_test.cpp`, add a test that loads the existing tiny zip and asserts the close values (proving the move-refactor did not corrupt accumulation):

```cpp
TEST(DataOratsHistory, MoveFlushPreservesValues) {
  const std::string zip = make_orats_zip();
  const fs::path out = fs::temp_directory_path() / "atx_orats_move_out";
  fs::remove_all(out);
  OratsLoadConfig cfg;
  cfg.zip_path = zip;
  cfg.out_dir = out.string();
  cfg.min_date_nanos = *detail::date_to_nanos("2020-01-01");
  cfg.created_at_nanos = 0;
  auto st = load_orats_history(cfg);
  ASSERT_TRUE(st.has_value()) << st.error().to_string();
  auto rdr = atx::tsdb::SegmentReader::attach((out / "2020-01-02.seg").string());
  ASSERT_TRUE(rdr.has_value()) << rdr.error().to_string();
  const auto close_fid = rdr->field_index("close");
  ASSERT_TRUE(close_fid.has_value());
  EXPECT_DOUBLE_EQ(rdr->value(*close_fid, 0, 0), 300.0);
  EXPECT_DOUBLE_EQ(rdr->value(*close_fid, 0, 1), 20.0);
}
```

- [ ] **Step 2: Run it to confirm it passes on the CURRENT code (it is a regression guard, not red-first here)**

Run: `ctest --test-dir build -R DataOratsHistory.MoveFlushPreservesValues --output-on-failure`
Expected: PASS (baseline behavior before refactor).

- [ ] **Step 3: Change `flush_date` to move and fix `DateAccumulator::clear`**

Replace the copy lines in `flush_date`:

```cpp
atx::core::Status flush_date(DateAccumulator &acc, const std::string &out_dir,
                              atx::i64 created_at_nanos) {
  atx::tsdb::LongColumns cols;
  cols.field_names.assign(kOratsFields.begin(), kOratsFields.end());
  const atx::usize rows = acc.symbols.size();
  cols.times.assign(rows, acc.date_nanos);
  cols.symbols = std::move(acc.symbols);   // was: cols.symbols = acc.symbols;
  cols.values = std::move(acc.values);     // was: cols.values = acc.values;
  const std::string path = (fs::path(out_dir) / (acc.date_str + ".seg")).string();
  return atx::tsdb::build_from_long(cols, path, created_at_nanos);
}
```

And fix `clear` so the moved-from outer `values` vector is rebuilt to 16 empty columns (a moved-from `std::vector` is empty, so the old `for (auto &v : values) v.clear();` would leave it size 0 and the next `values[f]` would be out of bounds):

```cpp
void clear(atx::i64 dn, std::string ds) {
  date_nanos = dn;
  date_str = std::move(ds);
  symbols.clear();
  values.assign(kOratsFields.size(), {});  // rebuild 16 empty columns (move-safe)
}
```

- [ ] **Step 4: Run the full data-group ORATS tests**

Run: `ninja atx-engine-data-tests && ctest --test-dir build -R DataOratsHistory --output-on-failure`
Expected: PASS (all `DataOratsHistory.*`, including the new guard).

- [ ] **Step 5: Commit**

```bash
git add atx-engine/src/data/orats_history.cpp atx-engine/tests/data/data_orats_history_test.cpp
git commit -m "perf(orats): move accumulator into flush_date instead of copying"
```

### Task 1.2: Rolling read buffer — eliminate the per-chunk full copy

**Files:**
- Modify: `atx-engine/src/data/orats_history.cpp` (the read loop in `load_orats_history`)

**Interfaces:**
- Consumes: `ZipEntryReader::read(std::span<char>)` (unchanged).
- Produces: the read loop no longer allocates/copies a `combined` string per chunk; it keeps one growable buffer and `memmove`s only the trailing partial line.

- [ ] **Step 1: Replace the `carry`/`combined` scheme with a single rolling buffer**

The current loop (`orats_history.cpp:253-300`) builds a `std::string combined` whenever `carry` is non-empty, copying the whole chunk. Replace with: keep all bytes in one `std::vector<char> buf`; track `data_len` (valid bytes) and `scan_pos`; after consuming complete lines, `memmove` the unconsumed tail to the front and append the next inflate read after it. Full replacement for the loop body:

```cpp
constexpr atx::usize kChunk = 1u << 22; // 4 MiB inflate reads
std::vector<char> buf(kChunk);
atx::usize fill = 0;        // valid bytes currently in buf[0, fill)
bool eof = false;

while (!eof) {
  // Ensure room to read: if buf is full of an unconsumed partial line, grow it
  // (a single line longer than the buffer is pathological but handled).
  if (fill == buf.size()) buf.resize(buf.size() * 2);

  const auto t0 = LoadProfile::clock::now();
  auto read_res = reader.read(std::span<char>(buf.data() + fill, buf.size() - fill));
  prof.inflate += LoadProfile::clock::now() - t0;
  if (!read_res.has_value()) return tl::unexpected(read_res.error());
  const atx::usize n = *read_res;
  fill += n;
  if (n == 0) eof = true;

  std::string_view chunk(buf.data(), fill);
  atx::usize pos = 0;
  for (;;) {
    const atx::usize nl = chunk.find('\n', pos);
    if (nl == std::string_view::npos) break; // no more complete lines this fill
    atx::usize end = nl;
    if (end > pos && chunk[end - 1] == '\r') --end;
    const std::string_view line = chunk.substr(pos, end - pos);
    pos = nl + 1;

    if (!header_parsed) {
      ATX_TRY(auto resolved, detail::resolve_header(line));
      idx = resolved;
      header_parsed = true;
      continue;
    }
    if (line.empty()) continue;
    const auto tp = LoadProfile::clock::now();
    ATX_TRY_VOID(process_line(line, st));
    prof.parse += LoadProfile::clock::now() - tp;
  }

  // Compact: keep the unconsumed tail [pos, fill) at the front of buf.
  if (pos > 0) {
    const atx::usize tail = fill - pos;
    std::memmove(buf.data(), buf.data() + pos, tail);
    fill = tail;
  }
}

// Final line with no trailing newline lives in buf[0, fill).
if (fill > 0 && header_parsed) {
  std::string_view line(buf.data(), fill);
  if (!line.empty() && line.back() == '\r') line = line.substr(0, line.size() - 1);
  if (!line.empty()) {
    const auto tp = LoadProfile::clock::now();
    ATX_TRY_VOID(process_line(line, st));
    prof.parse += LoadProfile::clock::now() - tp;
  }
}
```

Delete the now-dead `std::string carry;` declaration and the old `combined`/`carry` block. Note: `process_line`'s `st.fields` holds `string_view`s into `buf`; they are consumed fully within the `process_line` call (it copies `securityID`/ticker strings into the accumulator), so the later `memmove` invalidating those views is safe — no view outlives its iteration.

- [ ] **Step 2: Run the ORATS tests (line framing across the buffer boundary is the risk)**

Run: `ninja atx-engine-data-tests && ctest --test-dir build -R DataOratsHistory --output-on-failure`
Expected: PASS. The `LoadsTinyZipIntoPerDateSegments` and `RejectsNonMonotonicDates` cases exercise multi-line framing.

- [ ] **Step 3: Add a buffer-boundary stress test**

Add to `data_orats_history_test.cpp` a test that builds a zip with enough rows that lines straddle the 4 MiB read boundary, and asserts `rows_kept` matches the row count:

```cpp
TEST(DataOratsHistory, FramingAcrossBufferBoundary) {
  std::string body = std::string(kHeader) + "\n";
  // ~50k rows on one date >> any single inflate read won't align to line ends.
  constexpr int kRows = 50000;
  for (int i = 0; i < kRows; ++i) {
    body += make_orats_row("2020-01-02", std::to_string(20000 + i).c_str(),
                           "T", "T", 100.0 + i, 1.0, 1000000);
  }
  const std::string zip = write_orats_zip(body, "atx_orats_boundary.zip");
  const fs::path out = fs::temp_directory_path() / "atx_orats_boundary_out";
  fs::remove_all(out);
  OratsLoadConfig cfg;
  cfg.zip_path = zip;
  cfg.out_dir = out.string();
  cfg.min_date_nanos = *detail::date_to_nanos("2020-01-01");
  cfg.created_at_nanos = 0;
  auto st = load_orats_history(cfg);
  ASSERT_TRUE(st.has_value()) << st.error().to_string();
  EXPECT_EQ(st->rows_kept, kRows);
  EXPECT_EQ(st->dates_written, 1);
}
```

- [ ] **Step 4: Run the boundary test**

Run: `ctest --test-dir build -R DataOratsHistory.FramingAcrossBufferBoundary --output-on-failure`
Expected: PASS (`rows_kept == 50000`).

- [ ] **Step 5: Commit**

```bash
git add atx-engine/src/data/orats_history.cpp atx-engine/tests/data/data_orats_history_test.cpp
git commit -m "perf(orats): rolling read buffer (4 MiB) — drop per-chunk full copy"
```

### Task 1.3: Cheap allocation + hashing cuts

**Files:**
- Modify: `atx-engine/src/data/orats_history.cpp`

**Interfaces:**
- Produces: `DateAccumulator` reserves per-date row capacity; symbology insert uses one hash (`try_emplace`).

- [ ] **Step 1: Reserve accumulator capacity on `clear`**

ORATS dates carry thousands of symbols; reserve to cut `push_back` reallocations. In `DateAccumulator::clear`, after the `values.assign(...)`:

```cpp
constexpr atx::usize kRowsPerDateHint = 8192;
symbols.reserve(kRowsPerDateHint);
for (auto &v : values) v.reserve(kRowsPerDateHint);
```

(The `reserve` after `assign` is fine; `assign` set size 0 with capacity 0, `reserve` then grows capacity without changing size.)

- [ ] **Step 2: Collapse the symbology double-hash to `try_emplace`**

Replace the `find`/`emplace` pair in `process_line` (`orats_history.cpp:220-223`):

```cpp
st.symbology.try_emplace(secid_i64,
                         std::string(field_at(st.idx.ticker_tk)),
                         std::string(field_at(st.idx.todayTicker)));
```

`try_emplace` is a no-op if the key exists (preserving first-seen) and hashes once.

- [ ] **Step 3: Build + run the ORATS tests**

Run: `ninja atx-engine-data-tests && ctest --test-dir build -R DataOratsHistory --output-on-failure`
Expected: PASS (first-seen symbology behavior unchanged — `distinct_securities == 2` in the tiny-zip test).

- [ ] **Step 4: Commit**

```bash
git add atx-engine/src/data/orats_history.cpp
git commit -m "perf(orats): reserve per-date row capacity; try_emplace symbology"
```

### Task 1.4: Single-timestamp time-axis fast path in `build_from_long`

**Files:**
- Modify: `atx-tsdb/src/load_parquet.cpp:40-48`

**Interfaces:**
- Consumes/Produces: `build_from_long` semantics unchanged; only the time-axis construction is short-circuited when every timestamp is identical (the ORATS per-date case: all rows share one midnight-nanos).

- [ ] **Step 1: Add a failing/guard test in the tsdb suite**

In `atx-tsdb/tests/load_parquet_test.cpp`, add (or confirm) a case where all `times` are equal and assert the built segment has `time_count() == 1` and correct values — this guards the fast path:

```cpp
TEST(BuildFromLong, SingleTimestampAxisCollapsesToOneRow) {
  atx::tsdb::LongColumns cols;
  cols.field_names = {"close"};
  cols.times = {1000, 1000, 1000};
  cols.symbols = {"A", "B", "C"};
  cols.values = {{10.0, 20.0, 30.0}};
  const std::string path = (std::filesystem::temp_directory_path() / "axis1.seg").string();
  auto st = atx::tsdb::build_from_long(cols, path, 0);
  ASSERT_TRUE(st.has_value()) << st.error().to_string();
  auto rdr = atx::tsdb::SegmentReader::attach(path);
  ASSERT_TRUE(rdr.has_value());
  EXPECT_EQ(rdr->time_count(), 1u);
  EXPECT_EQ(rdr->instrument_count(), 3u);
}
```

- [ ] **Step 2: Run it against current code to confirm it already passes (regression guard)**

Run: `ninja atx-tsdb-tests && ctest --test-dir build -R BuildFromLong.SingleTimestampAxisCollapsesToOneRow --output-on-failure`
Expected: PASS (the sort+unique path already yields a 1-element axis; this guards the optimization).

- [ ] **Step 3: Short-circuit the axis build when all timestamps are equal**

Replace lines 40-48 of `load_parquet.cpp` (the `// time axis` block) with:

```cpp
  // --- time axis: unique sorted timestamps -> row index --------------------
  // Fast path: a single-date segment (the ORATS loader's per-date flush) has
  // every timestamp identical — skip the O(R log R) sort + the R hash inserts.
  std::vector<atx::i64> axis;
  std::unordered_map<atx::i64, atx::u64> time_to_row;
  const bool all_equal =
      rows > 0 && std::all_of(cols.times.begin(), cols.times.end(),
                              [&](atx::i64 t) { return t == cols.times.front(); });
  if (all_equal) {
    axis.push_back(cols.times.front());
    time_to_row.emplace(cols.times.front(), 0);
  } else {
    axis = cols.times;
    std::sort(axis.begin(), axis.end());
    axis.erase(std::unique(axis.begin(), axis.end()), axis.end());
    time_to_row.reserve(axis.size());
    for (atx::u64 i = 0; i < axis.size(); ++i) time_to_row.emplace(axis[i], i);
  }
```

(`<algorithm>` is already included for `std::sort`/`std::unique`.)

- [ ] **Step 4: Run the tsdb suite**

Run: `ninja atx-tsdb-tests && ctest --test-dir build -R BuildFromLong --output-on-failure`
Expected: PASS (both the equal-time and the multi-time existing cases).

- [ ] **Step 5: Re-run the bench to quantify Phase 1**

Run: `ninja atx-engine-bench && ATX_ORATS_PROFILE=1 ./build/bin/atx-engine-bench --benchmark_filter=BM_OratsLoad`
Expected: total wall-clock lower than the Phase 0 baseline; record the new split.

- [ ] **Step 6: Commit**

```bash
git add atx-tsdb/src/load_parquet.cpp atx-tsdb/tests/load_parquet_test.cpp
git commit -m "perf(tsdb): single-timestamp time-axis fast path in build_from_long

phase-1 bench (240d x 3000sym): inflate=<X> parse=<Y> build_write=<Z> total=<T>"
```

---

## Phase 2 — Parallel build+write (producer → writer pool)

**Rationale:** Each date's `.seg` is fully determined by that date's rows, independent of all other dates. So `build_from_long` + file write can run on a worker pool while the single ordered producer keeps inflating + parsing the next date. This overlaps the build/serialize/disk-IO latency behind the serial front. Symbology + manifest stay producer-side, so output is byte-identical to the serial loader.

**Determinism invariant (must hold):** every `<date>.seg` is written by exactly one worker from one date's rows; segment contents do not depend on inter-date ordering or on how workers interleave. `_symbology.parquet` (sorted by securityID, first-seen ticker resolved in producer date-order) and `_manifest.json` (additive counts) are produced single-threaded after the pool drains. Therefore the partition is bit-identical regardless of worker count.

### Task 2.1: Extract a `DateJob` and a bounded blocking queue

**Files:**
- Modify: `atx-engine/src/data/orats_history.cpp`

**Interfaces:**
- Produces (internal, anonymous namespace):
  - `struct DateJob { std::string out_path; atx::i64 date_nanos; atx::i64 created_at_nanos; std::vector<std::string> symbols; std::vector<std::vector<atx::f64>> values; };`
  - `template <class T> class BoundedQueue { void push(T); bool pop(T&); void close(); };`

- [ ] **Step 1: Add the includes**

At the top of `orats_history.cpp`:

```cpp
#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
```

- [ ] **Step 2: Add `DateJob` and `BoundedQueue` to the anonymous namespace**

```cpp
// One trading date's projected columns, sealed and ready to pivot+write.
struct DateJob {
  std::string out_path;                      // <out_dir>/YYYY-MM-DD.seg
  atx::i64 date_nanos{};
  atx::i64 created_at_nanos{};
  std::vector<std::string> symbols;          // securityID per row
  std::vector<std::vector<atx::f64>> values; // [16][rows]
};

// Bounded, blocking MPMC handoff. Coarse granularity (one job == one date,
// thousands of rows), so a mutex/condvar queue is the right tool — the
// lock-free ring buffers in atx-core/concurrent target per-event hot paths.
template <class T>
class BoundedQueue {
public:
  explicit BoundedQueue(std::size_t cap) : cap_{cap} {}

  // Returns false if the queue was closed before this push could be accepted.
  bool push(T v) {
    std::unique_lock lk(m_);
    not_full_.wait(lk, [&] { return q_.size() < cap_ || closed_; });
    if (closed_) return false;
    q_.push_back(std::move(v));
    lk.unlock();
    not_empty_.notify_one();
    return true;
  }

  // Returns false once the queue is closed AND drained.
  bool pop(T &out) {
    std::unique_lock lk(m_);
    not_empty_.wait(lk, [&] { return !q_.empty() || closed_; });
    if (q_.empty()) return false;
    out = std::move(q_.front());
    q_.pop_front();
    lk.unlock();
    not_full_.notify_one();
    return true;
  }

  void close() {
    {
      std::lock_guard lk(m_);
      closed_ = true;
    }
    not_empty_.notify_all();
    not_full_.notify_all();
  }

private:
  std::size_t cap_;
  std::deque<T> q_;
  std::mutex m_;
  std::condition_variable not_full_, not_empty_;
  bool closed_{false};
};
```

- [ ] **Step 3: Build to confirm it compiles (no use yet)**

Run: `ninja atx-engine-data-tests`
Expected: clean build (unused types are fine until wired in 2.2).

- [ ] **Step 4: Commit**

```bash
git add atx-engine/src/data/orats_history.cpp
git commit -m "perf(orats): add DateJob + bounded blocking queue scaffolding"
```

### Task 2.2: Wire the writer pool into `load_orats_history`

**Files:**
- Modify: `atx-engine/src/data/orats_history.cpp`

**Interfaces:**
- Consumes: `DateJob`, `BoundedQueue<DateJob>`, `build_from_long`.
- Produces: `load_orats_history` runs `W = clamp(hardware_concurrency-1, 1, 8)` writer threads; the parse loop seals each completed date into a `DateJob` and pushes it; workers pop, pivot, and write. First worker error aborts the load and is returned.

- [ ] **Step 1: Add a worker-error sink and a job-sealing helper**

Add to the anonymous namespace:

```cpp
// Holds the first worker error (if any) under a mutex; workers race to set it.
struct WorkerError {
  std::mutex m;
  std::atomic<bool> failed{false};
  atx::core::Error err;
  void set(atx::core::Error e) {
    if (failed.exchange(true)) return; // keep only the first
    std::lock_guard lk(m);
    err = std::move(e);
  }
};

// Pivot one job's columns and write its .seg. Mirrors the old flush_date body
// but operates on an owned (moved) DateJob — safe to run on any thread because
// SegmentBuilder is build-once/instance-local and each job writes a distinct file.
atx::core::Status run_job(DateJob &job) {
  atx::tsdb::LongColumns cols;
  cols.field_names.assign(kOratsFields.begin(), kOratsFields.end());
  const atx::usize rows = job.symbols.size();
  cols.times.assign(rows, job.date_nanos);
  cols.symbols = std::move(job.symbols);
  cols.values = std::move(job.values);
  return atx::tsdb::build_from_long(cols, job.out_path, job.created_at_nanos);
}
```

- [ ] **Step 2: Replace `flush_date` calls with job-sealing**

`flush_date` currently builds + writes inline. Change the date-boundary flush and the final flush to instead seal a `DateJob` and push it to the queue. Add a sealing lambda in `load_orats_history` and route both flush sites through it. The producer must STILL count `dates_written` (it knows each sealed date), and must stop early if a worker has already failed.

The `LoadState` / `process_line` flush site (`orats_history.cpp:198-205`) calls `flush_date`. Refactor so `process_line` does NOT write directly; instead, when it detects a date boundary it hands the *previous* accumulator off via a callback the loader supplies. Simplest concrete approach: keep the date-boundary detection in `process_line`, but replace the `flush_date(...)` call with a call through a `std::function<atx::core::Status(DateAccumulator&)>` stored in `LoadState` (call it `st.flush`). In `load_orats_history`, define:

```cpp
BoundedQueue<DateJob> jobs{/*cap=*/ static_cast<std::size_t>(2 * W)};

auto seal = [&](DateAccumulator &a) -> atx::core::Status {
  if (a.empty()) return atx::core::Ok();
  if (werr.failed.load(std::memory_order_relaxed)) {
    std::lock_guard lk(werr.m);
    return atx::core::Err(werr.err);          // surface a worker failure promptly
  }
  DateJob job;
  job.out_path = (fs::path(cfg.out_dir) / (a.date_str + ".seg")).string();
  job.date_nanos = a.date_nanos;
  job.created_at_nanos = cfg.created_at_nanos;
  job.symbols = std::move(a.symbols);
  job.values = std::move(a.values);
  jobs.push(std::move(job));
  ++stats.dates_written;
  return atx::core::Ok();
};
```

Set `st.flush = seal;` (add `std::function<atx::core::Status(DateAccumulator&)> flush;` to `LoadState`, and in `process_line` replace `ATX_TRY_VOID(flush_date(st.acc, st.cfg.out_dir, st.cfg.created_at_nanos)); ++st.stats.dates_written;` with `ATX_TRY_VOID(st.flush(st.acc));`). The `++dates_written` now lives in `seal`, so remove it from `process_line`.

- [ ] **Step 3: Spawn the pool before the read loop and join after the final flush**

In `load_orats_history`, before the read loop:

```cpp
const unsigned hw = std::thread::hardware_concurrency();
const std::size_t W = std::clamp<std::size_t>(hw == 0 ? 1 : hw - 1, 1, 8);
WorkerError werr;
// (declare jobs{...} and seal{...} here, as in Step 2, using W)

std::vector<std::thread> pool;
pool.reserve(W);
for (std::size_t i = 0; i < W; ++i) {
  pool.emplace_back([&jobs, &werr] {
    DateJob job;
    while (jobs.pop(job)) {
      auto s = run_job(job);
      if (!s.has_value()) {
        werr.set(s.error());
        // keep draining so the producer's push() never deadlocks on a full queue;
        // further jobs are popped and dropped cheaply once failed is set.
      }
    }
  });
}
```

After the final accumulated-date flush (`orats_history.cpp:310-314`, now routed through `seal`), close the queue and join:

```cpp
jobs.close();
for (auto &t : pool) t.join();
if (werr.failed.load(std::memory_order_acquire)) {
  std::lock_guard lk(werr.m);
  return atx::core::Err(werr.err);
}
```

Delete the now-unused free function `flush_date` (its body moved into `run_job`/`seal`).

- [ ] **Step 4: Keep symbology + manifest exactly where they are**

The symbology map build, sort, `write_parquet`, and `_manifest.json` write (`orats_history.cpp:316-374`) run AFTER the join, on the producer thread, unchanged. `stats.distinct_securities = symbology.size()` is computed there as before. Confirm these blocks are below the join.

- [ ] **Step 5: Build + run the existing ORATS tests**

Run: `ninja atx-engine-data-tests && ctest --test-dir build -R DataOratsHistory --output-on-failure`
Expected: PASS. Critically, `LoadsTinyZipIntoPerDateSegments` (2 dates, 2 securities) and `RejectsNonMonotonicDates` must still pass — the monotonic guard runs in the producer before any push, so a regression still fails closed with `InvalidArgument` (the producer returns before closing the queue; ensure the early-return path still `close()`s + joins to avoid detached threads — see Step 6).

- [ ] **Step 6: Make every error path join the pool (no detached threads / no deadlock)**

The producer has several early `return tl::unexpected(...)` paths (inflate error, header parse error, non-monotonic date). Each must `jobs.close(); for (auto&t:pool) t.join();` before returning, else worker threads block forever on `pop` and `std::thread` destructors call `std::terminate`. Wrap the read loop + flush in a small helper that returns a `Status`, then have `load_orats_history` always `close()`+`join()` after it (RAII guard preferred):

```cpp
struct PoolJoiner {
  BoundedQueue<DateJob> &q;
  std::vector<std::thread> &p;
  ~PoolJoiner() { q.close(); for (auto &t : p) if (t.joinable()) t.join(); }
} joiner{jobs, pool};
```

Declare `joiner` right after the pool is spawned; its destructor runs on every return path (normal or error), guaranteeing clean shutdown. After the explicit post-loop join you can leave the guard as a no-op safety net (joining an already-joined `std::thread` is skipped via `joinable()`).

- [ ] **Step 7: Run the boundary + move tests again under the threaded path**

Run: `ctest --test-dir build -R DataOratsHistory --output-on-failure`
Expected: PASS (`FramingAcrossBufferBoundary`, `MoveFlushPreservesValues`, all originals).

- [ ] **Step 8: Commit**

```bash
git add atx-engine/src/data/orats_history.cpp
git commit -m "perf(orats): parallel per-date build+write via writer pool

producer stays single-threaded + ordered (inflate/parse/route + symbology);
each date's .seg pivoted+written on a worker. RAII pool join on all paths."
```

### Task 2.3: Serial-vs-parallel byte-equivalence test (determinism gate)

**Files:**
- Modify: `atx-engine/tests/data/data_orats_history_test.cpp`

**Interfaces:**
- Consumes: `load_orats_history`, `atx::tsdb::SegmentReader`.
- Produces: a test proving the multi-threaded loader yields identical segment content + identical symbology to a known-good reference, across a multi-date fixture.

- [ ] **Step 1: Add a multi-date determinism test**

Because the loader is internally threaded, we cannot toggle worker count from the public API — instead we assert the partition is internally consistent and stable across two independent runs (run twice → identical segment digests + identical byte content of `_symbology.parquet`). Identical output across repeated threaded runs is the determinism guarantee.

```cpp
TEST(DataOratsHistory, ParallelOutputIsDeterministicAcrossRuns) {
  // Multi-date fixture: 40 dates x 500 symbols — enough to exercise the queue
  // and multiple workers concurrently.
  std::string body = std::string(kHeader) + "\n";
  for (int d = 0; d < 40; ++d) {
    char date[11];
    std::snprintf(date, sizeof(date), "2020-%02d-%02d", 1 + d / 28, 1 + d % 28);
    for (int s = 0; s < 500; ++s) {
      body += make_orats_row(date, std::to_string(30000 + s).c_str(), "T", "T",
                             100.0 + s + d, 1.0, 1000000);
    }
  }
  const std::string zip = write_orats_zip(body, "atx_orats_det.zip");

  auto run = [&](const char *tag) {
    const fs::path out = fs::temp_directory_path() / (std::string("atx_orats_det_") + tag);
    fs::remove_all(out);
    OratsLoadConfig cfg;
    cfg.zip_path = zip;
    cfg.out_dir = out.string();
    cfg.min_date_nanos = *detail::date_to_nanos("2020-01-01");
    cfg.created_at_nanos = 0;
    auto st = load_orats_history(cfg);
    EXPECT_TRUE(st.has_value()) << st.error().to_string();
    return out;
  };

  const fs::path a = run("a");
  const fs::path b = run("b");

  // Every .seg and the symbology parquet must be byte-identical across runs.
  for (const auto &e : fs::directory_iterator(a)) {
    const fs::path rel = e.path().filename();
    const fs::path bp = b / rel;
    ASSERT_TRUE(fs::exists(bp)) << "missing in run b: " << rel.string();
    ASSERT_EQ(fs::file_size(e.path()), fs::file_size(bp)) << "size differs: " << rel.string();
    std::ifstream fa(e.path(), std::ios::binary), fb(bp, std::ios::binary);
    std::string sa((std::istreambuf_iterator<char>(fa)), {});
    std::string sb((std::istreambuf_iterator<char>(fb)), {});
    EXPECT_EQ(sa, sb) << "content differs: " << rel.string();
  }
}
```

- [ ] **Step 2: Run it**

Run: `ninja atx-engine-data-tests && ctest --test-dir build -R DataOratsHistory.ParallelOutputIsDeterministicAcrossRuns --output-on-failure`
Expected: PASS — identical bytes across both threaded runs (segment content hash + integrity CRC are pure functions of the date's rows; symbology is producer-ordered).

- [ ] **Step 3: Re-run the benchmark to quantify Phase 2**

Run: `ninja atx-engine-bench && ATX_ORATS_PROFILE=1 ./build/bin/atx-engine-bench --benchmark_filter=BM_OratsLoad`
Expected: total wall-clock improves toward `max(inflate, parse, build_write/W)`. Record the new total.

- [ ] **Step 4: Commit**

```bash
git add atx-engine/tests/data/data_orats_history_test.cpp
git commit -m "test(orats): determinism gate — threaded output byte-identical across runs

phase-2 bench (240d x 3000sym): total=<T> (baseline was <T0>)"
```

---

## Phase 3 — Parallel parse (DECISION-GATED)

**Go/no-go:** Only do this if Phase 0/1/2 numbers show **parse** is the dominant remaining cost (parse ≳ inflate and parse ≳ build_write/W). If inflate dominates, skip to Phase 4.

**Design (to be expanded into bite-sized tasks once the gate is met):**

The producer currently does the 16× `from_chars` f64 parse per row. To parallelize it, move parsing into the workers:

- Producer stops parsing field values. For each kept row it appends the row's *raw bytes* to the current date's contiguous byte buffer (one `std::string` per date) and still extracts only `tradingDate` (for routing/guard) and `securityID`+`ticker_tk`+`todayTicker` (for producer-side symbology, which must stay single-threaded for determinism). The cheap part (newline scan + a few `find('\t')` to the needed columns) stays serial; the expensive part (16 `from_chars` × millions of rows) moves to workers.
- `DateJob` carries the raw byte buffer + the resolved `ColumnIndex` (copied once, it is 20 ints). The worker re-splits each line and runs the 16 `from_chars`, building `LongColumns` directly, then `build_from_long` + write — exactly as in Phase 2 but with the parse cost now parallel.
- Determinism unchanged: symbology + counts still producer-side; per-date segments still order-independent.

**Risk:** the producer must still scan each line far enough to find `securityID`/`ticker` columns; if those are late columns this re-scan cost partly offsets the win. Measure the column positions in the real header (from `data_orats_history_test.cpp::kHeader`: tradingDate=0, securityID=1, ticker_tk=2, todayTicker=3 — all early, so the producer scan is cheap). This makes Phase 3 favorable **if** parse dominates.

**Acceptance:** same determinism test (Task 2.3) passes; bench total drops toward `max(inflate, build_write_and_parse/W)`.

---

## Phase 4 — Faster inflate (DECISION-GATED)

**Go/no-go:** Do this if Phase 0 shows **inflate** is the dominant cost (the likely case for a multi-GB single-entry zip). miniz inflate is ~80–150 MB/s; zlib-ng / ISA-L igzip are 2–4× faster.

**Design (to be expanded once the gate is met):**

- Keep miniz for zip central-directory parsing and CRC validation. Replace only the inflate step behind the existing `ZipEntryReader` interface in `atx-core/src/io/zip_reader.cpp` — the loader does not change at all (it only calls `reader.read(span)`).
- Add an opt-in CMake option `ATX_FAST_INFLATE` (default OFF) that vendors/links zlib-ng (vcpkg `zlib-ng`) or ISA-L. When ON, `zip_reader.cpp` feeds the entry's raw compressed bytes (obtained from miniz's central directory: offset + compressed size) to the faster inflater in streaming mode and validates CRC32 against the central-directory value.
- When OFF, the current miniz `mz_zip_reader_extract_iter_*` path is used verbatim — zero behavior change, no new dependency in the default build.

**Risk:** zlib-ng is a streaming `inflate()` (drop-in); ISA-L `isal_inflate` is faster but lower-level. zstd (already vendored) is **not** applicable — the input is deflate, not zstd. Prefer zlib-ng for the smallest, lowest-risk swap.

**Acceptance:** `ZipEntryReader` unit tests (`atx-core/tests/zip_reader_test.cpp`) pass under both `ATX_FAST_INFLATE` ON and OFF; CRC mismatch still returns `Err(ParseError)`; bench inflate term drops ≥2×.

---

## Self-Review

**Spec coverage:**
- "Review the code" → done in the conversation (findings folded into Phase 1 tasks 1.1–1.4 and the `try_emplace` fix).
- "Efficiently and concurrently process the zipped text into atx-tsdb segments" → Phase 2 (parallel per-date build+write), Phase 3 (parallel parse, gated), Phase 4 (faster inflate, gated).
- "All the performance/optimization tricks" → rolling buffer (no copy), move-not-copy, capacity reserves, single-hash insert, time-axis fast path, lock-light coarse queue, faster inflate lib.
- "See how fast you can make this" → Phase 0 baseline + per-phase bench re-runs quantify each step.
- "First review the code and build a plan" → this document.

**Placeholder scan:** Phases 0–2 contain complete code + exact commands. Phases 3–4 are explicitly DECISION-GATED roadmaps, not placeholders — they will be expanded into bite-sized tasks only if their go/no-go criterion is met (parse-dominant / inflate-dominant); writing full step-by-step code now would be speculative since it depends on measured numbers.

**Type consistency:** `DateJob`, `BoundedQueue<T>` (`push`/`pop`/`close`), `WorkerError` (`set`/`failed`/`err`), `run_job`, `seal` are consistently named across Tasks 2.1–2.2. `DateAccumulator::clear` rebuilds `values` via `assign` then `reserve` (Tasks 1.1 + 1.3 are compatible: 1.1 changes `clear`, 1.3 adds reserves after the `assign`). `load_orats_history` / `OratsLoadConfig` / `OratsLoadStats` signatures are unchanged throughout.

**Known follow-ups to verify during execution:**
- Confirm the actual path of the ORATS test file (`tests/data/data_orats_history_test.cpp` after the suite reorg) before editing.
- Confirm `atx-tsdb-tests` target name in the local build (`tests/CMakeLists.txt` of atx-tsdb).
- `MZ_DEFAULT_LEVEL` in the bench produces compressible output; if the synthetic body compresses too well (unrealistically fast inflate), raise entropy in the value columns.
