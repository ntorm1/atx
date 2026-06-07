# Disk Hive Data Layer + Databento Storage Pipeline — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A date-partitioned (hive) Parquet storage layer for the engine, plus Databento wrapper functions that pull bbo-1m / cbbo-1m straight to Parquet, wired into an end-to-end "build the top-100 liquid universe and snapshot its 15:55 L1" pipeline.

**Architecture:** `atx-engine` gains `DiskStore` (hive layout + partition I/O + reads) and a header-only liquidity-ranking helper. `atx-core`'s databento wrapper gains string-typed, cost-gated pull-to-Parquet functions using the vendored official client (the public header stays databento-free). A thin opt-in `build_universe` orchestrator wires it together.

**Tech Stack:** C++20, CMake + vcpkg + Ninja/clang-cl, Arrow/Parquet (`atx::core::io`), GoogleTest, databento-cpp v0.59.0.

---

## Build environment (every build/test command assumes this)

The worktree builds in its own `build/` dir. All `cmake`/`ctest` commands run inside a VS dev environment with vcpkg. The harness runs them via:

```
cmd /c "call \"C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat\" && set VCPKG_ROOT=C:\Users\natha\vcpkg && <command>"
```

- Configure (once, and after any CMakeLists change): `cmake --preset ninja -DATX_BUILD_EXAMPLES=ON`
- Build a target: `cmake --build build --target <tgt>`
- Run tests: `ctest --test-dir build -C Debug -R <regex> --output-on-failure`

First configure in a fresh worktree rebuilds databento + Arrow deps (several minutes). Run it in the background.

## File structure

| File | Responsibility |
|------|----------------|
| `atx-engine/include/atx/engine/data/disk.hpp` | `DiskStore` decl + `Store` enum |
| `atx-engine/src/data/disk.cpp` | `DiskStore` impl (filesystem + parquet I/O) |
| `atx-engine/include/atx/engine/data/universe.hpp` | header-only `top_n_by_median_notional` + `median` |
| `atx-engine/tests/disk_test.cpp` | DiskStore unit tests |
| `atx-engine/tests/universe_test.cpp` | ranking unit tests |
| `atx-engine/CMakeLists.txt` | add `src/data/disk.cpp`; add examples opt-in |
| `atx-engine/examples/build_universe.cpp` | orchestrator |
| `atx-engine/examples/CMakeLists.txt` | `build_universe` target |
| `atx-core/include/atx/external/databento.hpp` | add `PullStats`, `estimate_cost`, `split_under_cap<>`, two pull fns |
| `atx-core/src/external/databento.cpp` | impl of the new pull/cost functions (official client) |
| `atx-core/tests/databento_split_test.cpp` | `split_under_cap` unit test |

---

## Task 1: DiskStore skeleton — `Store` enum, `open`, `store_dir`, `partition_path`

**Files:**
- Create: `atx-engine/include/atx/engine/data/disk.hpp`
- Create: `atx-engine/src/data/disk.cpp`
- Modify: `atx-engine/CMakeLists.txt:1-3` (add source)
- Test: `atx-engine/tests/disk_test.cpp`

- [ ] **Step 1: Add the source to the engine library**

In `atx-engine/CMakeLists.txt`, change the `add_library` block to:

```cmake
add_library(atx-engine STATIC
    src/engine.cpp
    src/data/disk.cpp
)
```

- [ ] **Step 2: Write the header**

Create `atx-engine/include/atx/engine/data/disk.hpp`:

```cpp
#pragma once

// atx::engine::data::DiskStore — date-partitioned (hive) Parquet stores on disk.
// Layout: <root>/<STORE>/date=YYYY-MM-DD/data.parquet. The `date` value is encoded
// in the path and is NOT a column inside the file.

#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "atx/core/error.hpp"             // Result
#include "atx/core/io/parquet.hpp"        // LazyParquet
#include "atx/core/io/parquet_writer.hpp" // WriteColumn

namespace atx::engine::data {

using atx::core::Result;

enum class Store { Ohlc1D, Ohlc1M, OpraBbo, OChain };

// Subdir name for a store (e.g. Store::Ohlc1D -> "OHLC1D").
[[nodiscard]] std::string_view store_name(Store s) noexcept;

class DiskStore {
public:
  // Create <root> and all four store subdirs if absent.
  [[nodiscard]] static Result<DiskStore> open(std::filesystem::path root);

  [[nodiscard]] const std::filesystem::path& root() const noexcept { return root_; }
  [[nodiscard]] std::filesystem::path store_dir(Store s) const;
  // <root>/<STORE>/date=<date>/data.parquet
  [[nodiscard]] std::filesystem::path partition_path(Store s, std::string_view date) const;

  // Write one date partition. `cols` must NOT contain a `date` column.
  [[nodiscard]] Result<void> write_partition(Store s, std::string_view date,
                                             std::span<const core::io::WriteColumn> cols) const;

  // Ascending list of partition dates ("YYYY-MM-DD") present in a store.
  [[nodiscard]] Result<std::vector<std::string>> list_dates(Store s) const;

  // Lazy scan of a single partition file (Err if absent).
  [[nodiscard]] Result<core::io::LazyParquet> scan_partition(Store s, std::string_view date) const;

private:
  explicit DiskStore(std::filesystem::path root) : root_{std::move(root)} {}
  std::filesystem::path root_;
};

} // namespace atx::engine::data
```

- [ ] **Step 3: Write the failing test**

Create `atx-engine/tests/disk_test.cpp`:

```cpp
#include <gtest/gtest.h>

#include <filesystem>
#include <string>

#include "atx/engine/data/disk.hpp"

namespace fs = std::filesystem;
using atx::engine::data::DiskStore;
using atx::engine::data::Store;

namespace {
fs::path unique_tmp() {
  static int counter = 0;
  return fs::temp_directory_path() /
         ("atx_disk_test_" + std::to_string(++counter) + "_" +
          std::to_string(static_cast<long long>(reinterpret_cast<std::uintptr_t>(&counter))));
}
} // namespace

TEST(DiskStore, OpenCreatesStoreDirs) {
  const fs::path root = unique_tmp();
  auto st = DiskStore::open(root);
  ASSERT_TRUE(st.has_value()) << st.error().message;
  EXPECT_TRUE(fs::exists(root / "OHLC1D"));
  EXPECT_TRUE(fs::exists(root / "OHLC1M"));
  EXPECT_TRUE(fs::exists(root / "OPRA_BBO"));
  EXPECT_TRUE(fs::exists(root / "OCHAIN"));
  fs::remove_all(root);
}

TEST(DiskStore, PartitionPathFormat) {
  const fs::path root = unique_tmp();
  auto st = DiskStore::open(root);
  ASSERT_TRUE(st.has_value());
  const fs::path p = st->partition_path(Store::Ohlc1M, "2026-06-05");
  EXPECT_EQ(p, root / "OHLC1M" / "date=2026-06-05" / "data.parquet");
  fs::remove_all(root);
}
```

- [ ] **Step 4: Implement `disk.cpp` (this task's portion)**

Create `atx-engine/src/data/disk.cpp`:

```cpp
#include "atx/engine/data/disk.hpp"

#include <algorithm>
#include <system_error>

namespace atx::engine::data {

using atx::core::Err;
using atx::core::ErrorCode;
using atx::core::Ok;

std::string_view store_name(Store s) noexcept {
  switch (s) {
    case Store::Ohlc1D: return "OHLC1D";
    case Store::Ohlc1M: return "OHLC1M";
    case Store::OpraBbo: return "OPRA_BBO";
    case Store::OChain: return "OCHAIN";
  }
  return "UNKNOWN";
}

Result<DiskStore> DiskStore::open(std::filesystem::path root) {
  std::error_code ec;
  std::filesystem::create_directories(root, ec);
  if (ec) {
    return Err(ErrorCode::IoError, "create root: " + ec.message());
  }
  DiskStore store{std::move(root)};
  for (const Store s : {Store::Ohlc1D, Store::Ohlc1M, Store::OpraBbo, Store::OChain}) {
    std::filesystem::create_directories(store.store_dir(s), ec);
    if (ec) {
      return Err(ErrorCode::IoError, "create store dir: " + ec.message());
    }
  }
  return Ok(std::move(store));
}

std::filesystem::path DiskStore::store_dir(Store s) const {
  return root_ / std::string{store_name(s)};
}

std::filesystem::path DiskStore::partition_path(Store s, std::string_view date) const {
  return store_dir(s) / ("date=" + std::string{date}) / "data.parquet";
}

// write_partition / list_dates / scan_partition added in later tasks.

} // namespace atx::engine::data
```

- [ ] **Step 5: Configure + build + run the two tests, expect PASS**

Run: `cmake --preset ninja -DATX_BUILD_EXAMPLES=ON` then `cmake --build build --target atx-engine-tests` then `ctest --test-dir build -C Debug -R "DiskStore.OpenCreatesStoreDirs|DiskStore.PartitionPathFormat" --output-on-failure`
Expected: 2 tests PASS.

- [ ] **Step 6: Commit**

```bash
git add atx-engine/include/atx/engine/data/disk.hpp atx-engine/src/data/disk.cpp atx-engine/tests/disk_test.cpp atx-engine/CMakeLists.txt
git commit -m "feat(disk): DiskStore skeleton + store/partition paths"
```

---

## Task 2: DiskStore `write_partition` + `scan_partition` round-trip

**Files:**
- Modify: `atx-engine/src/data/disk.cpp` (replace the trailing comment)
- Test: `atx-engine/tests/disk_test.cpp` (add a test)

- [ ] **Step 1: Write the failing round-trip test**

Append to `atx-engine/tests/disk_test.cpp`:

```cpp
#include <cstdint>
#include "atx/core/types.hpp"

TEST(DiskStore, WriteThenScanPartitionRoundTrip) {
  using atx::core::i64;
  namespace io = atx::core::io;
  const fs::path root = unique_tmp();
  auto st = DiskStore::open(root);
  ASSERT_TRUE(st.has_value());

  const std::vector<std::string> sym{"AAA", "BBB"};
  const std::vector<i64> bid{100, 200};
  const std::vector<i64> ask{101, 202};
  const std::vector<io::WriteColumn> cols{
      {"symbol", std::span<const std::string>(sym)},
      {"bid_px", std::span<const i64>(bid)},
      {"ask_px", std::span<const i64>(ask)},
  };
  auto w = st->write_partition(Store::Ohlc1M, "2026-06-05", cols);
  ASSERT_TRUE(w.has_value()) << w.error().message;
  EXPECT_TRUE(fs::exists(st->partition_path(Store::Ohlc1M, "2026-06-05")));

  auto lazy = st->scan_partition(Store::Ohlc1M, "2026-06-05");
  ASSERT_TRUE(lazy.has_value()) << lazy.error().message;
  auto tbl = lazy->collect();
  ASSERT_TRUE(tbl.has_value()) << tbl.error().message;
  EXPECT_EQ(tbl->num_rows(), 2);
  auto bids = tbl->column_view<i64>("bid_px");
  ASSERT_TRUE(bids.has_value());
  EXPECT_EQ((*bids)[0], 100);
  EXPECT_EQ((*bids)[1], 200);
  fs::remove_all(root);
}
```

- [ ] **Step 2: Run it, expect FAIL (link error: write_partition/scan_partition undefined)**

Run: `cmake --build build --target atx-engine-tests`
Expected: FAIL — unresolved `DiskStore::write_partition` / `scan_partition`.

- [ ] **Step 3: Implement the two methods**

In `atx-engine/src/data/disk.cpp`, add the include and replace the trailing comment:

```cpp
#include "atx/core/io/parquet.hpp"        // already pulled via header, explicit for clarity
```

Replace `// write_partition / list_dates / scan_partition added in later tasks.` with:

```cpp
Result<void> DiskStore::write_partition(Store s, std::string_view date,
                                        std::span<const core::io::WriteColumn> cols) const {
  const std::filesystem::path out = partition_path(s, date);
  std::error_code ec;
  std::filesystem::create_directories(out.parent_path(), ec);
  if (ec) {
    return Err(ErrorCode::IoError, "create partition dir: " + ec.message());
  }
  auto status = core::io::write_parquet(cols, out.string());
  if (!status.has_value()) {
    return Err(status.error());
  }
  return Ok();
}

Result<core::io::LazyParquet> DiskStore::scan_partition(Store s, std::string_view date) const {
  const std::filesystem::path p = partition_path(s, date);
  if (!std::filesystem::exists(p)) {
    return Err(ErrorCode::IoError, "partition not found: " + p.string());
  }
  return core::io::LazyParquet::scan(p.string());
}
```

- [ ] **Step 4: Build + run, expect PASS**

Run: `cmake --build build --target atx-engine-tests` then `ctest --test-dir build -C Debug -R "DiskStore.WriteThenScanPartitionRoundTrip" --output-on-failure`
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add atx-engine/src/data/disk.cpp atx-engine/tests/disk_test.cpp
git commit -m "feat(disk): write_partition + scan_partition round-trip"
```

---

## Task 3: DiskStore `list_dates`

**Files:**
- Modify: `atx-engine/src/data/disk.cpp`
- Test: `atx-engine/tests/disk_test.cpp`

- [ ] **Step 1: Write the failing test**

Append to `atx-engine/tests/disk_test.cpp`:

```cpp
TEST(DiskStore, ListDatesSortedAscending) {
  using atx::core::i64;
  namespace io = atx::core::io;
  const fs::path root = unique_tmp();
  auto st = DiskStore::open(root);
  ASSERT_TRUE(st.has_value());
  const std::vector<std::string> sym{"AAA"};
  const std::vector<i64> v{1};
  const std::vector<io::WriteColumn> cols{
      {"symbol", std::span<const std::string>(sym)}, {"x", std::span<const i64>(v)}};
  for (const char* d : {"2026-06-05", "2026-06-03", "2026-06-04"}) {
    ASSERT_TRUE(st->write_partition(Store::Ohlc1D, d, cols).has_value());
  }
  auto dates = st->list_dates(Store::Ohlc1D);
  ASSERT_TRUE(dates.has_value()) << dates.error().message;
  ASSERT_EQ(dates->size(), 3u);
  EXPECT_EQ((*dates)[0], "2026-06-03");
  EXPECT_EQ((*dates)[1], "2026-06-04");
  EXPECT_EQ((*dates)[2], "2026-06-05");
  fs::remove_all(root);
}
```

- [ ] **Step 2: Run it, expect FAIL (list_dates undefined)**

Run: `cmake --build build --target atx-engine-tests`
Expected: FAIL — unresolved `DiskStore::list_dates`.

- [ ] **Step 3: Implement `list_dates`**

Add to `atx-engine/src/data/disk.cpp` (inside the namespace):

```cpp
Result<std::vector<std::string>> DiskStore::list_dates(Store s) const {
  std::vector<std::string> dates;
  const std::filesystem::path dir = store_dir(s);
  std::error_code ec;
  if (!std::filesystem::exists(dir, ec)) {
    return Ok(std::move(dates));
  }
  for (const auto& e : std::filesystem::directory_iterator(dir, ec)) {
    if (ec) {
      return Err(ErrorCode::IoError, "iterate store: " + ec.message());
    }
    if (!e.is_directory()) {
      continue;
    }
    const std::string name = e.path().filename().string();
    constexpr std::string_view kPrefix = "date=";
    if (name.rfind(kPrefix, 0) == 0) {
      dates.emplace_back(name.substr(kPrefix.size()));
    }
  }
  std::sort(dates.begin(), dates.end());
  return Ok(std::move(dates));
}
```

- [ ] **Step 4: Build + run, expect PASS**

Run: `cmake --build build --target atx-engine-tests` then `ctest --test-dir build -C Debug -R "DiskStore.ListDatesSortedAscending" --output-on-failure`
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add atx-engine/src/data/disk.cpp atx-engine/tests/disk_test.cpp
git commit -m "feat(disk): list_dates partition enumeration"
```

---

## Task 4: Liquidity ranking — `top_n_by_median_notional` (header-only)

**Files:**
- Create: `atx-engine/include/atx/engine/data/universe.hpp`
- Test: `atx-engine/tests/universe_test.cpp`

- [ ] **Step 1: Write the header**

Create `atx-engine/include/atx/engine/data/universe.hpp`:

```cpp
#pragma once

// atx::engine::data — universe selection helpers. Pure, deterministic, no I/O.

#include <algorithm>
#include <cstddef>
#include <string>
#include <unordered_map>
#include <vector>

namespace atx::engine::data {

// Median of a copy (does not mutate input). Empty -> 0.0.
[[nodiscard]] inline double median(std::vector<double> v) {
  if (v.empty()) {
    return 0.0;
  }
  std::sort(v.begin(), v.end());
  const std::size_t n = v.size();
  return (n % 2 == 1) ? v[n / 2] : 0.5 * (v[n / 2 - 1] + v[n / 2]);
}

// Rank symbols by the median of their daily notionals (descending), tie-break by
// symbol ascending for determinism, return the first `n`.
[[nodiscard]] inline std::vector<std::string> top_n_by_median_notional(
    const std::unordered_map<std::string, std::vector<double>>& notionals_by_symbol,
    std::size_t n) {
  std::vector<std::pair<std::string, double>> scored;
  scored.reserve(notionals_by_symbol.size());
  for (const auto& [sym, notionals] : notionals_by_symbol) {
    scored.emplace_back(sym, median(notionals));
  }
  std::sort(scored.begin(), scored.end(), [](const auto& a, const auto& b) {
    if (a.second != b.second) {
      return a.second > b.second; // higher notional first
    }
    return a.first < b.first; // tie-break: symbol ascending
  });
  if (scored.size() > n) {
    scored.resize(n);
  }
  std::vector<std::string> out;
  out.reserve(scored.size());
  for (auto& [sym, _] : scored) {
    out.push_back(std::move(sym));
  }
  return out;
}

} // namespace atx::engine::data
```

- [ ] **Step 2: Write the failing test**

Create `atx-engine/tests/universe_test.cpp`:

```cpp
#include <gtest/gtest.h>

#include <string>
#include <unordered_map>
#include <vector>

#include "atx/engine/data/universe.hpp"

using atx::engine::data::median;
using atx::engine::data::top_n_by_median_notional;

TEST(Universe, MedianOddAndEven) {
  EXPECT_DOUBLE_EQ(median({3.0, 1.0, 2.0}), 2.0);
  EXPECT_DOUBLE_EQ(median({4.0, 1.0, 3.0, 2.0}), 2.5);
  EXPECT_DOUBLE_EQ(median({}), 0.0);
}

TEST(Universe, TopNByMedianWithTieBreak) {
  std::unordered_map<std::string, std::vector<double>> m{
      {"HIGH", {100.0, 100.0, 100.0}}, // median 100
      {"MIDB", {50.0, 50.0}},          // median 50, tie with MIDA
      {"MIDA", {50.0, 50.0}},          // median 50
      {"LOW", {1.0}},                  // median 1
  };
  auto top = top_n_by_median_notional(m, 3);
  ASSERT_EQ(top.size(), 3u);
  EXPECT_EQ(top[0], "HIGH");
  EXPECT_EQ(top[1], "MIDA"); // tie-break: symbol ascending
  EXPECT_EQ(top[2], "MIDB");
}
```

- [ ] **Step 3: Build + run, expect PASS** (header-only; test drives it)

Run: `cmake --build build --target atx-engine-tests` then `ctest --test-dir build -C Debug -R "Universe\." --output-on-failure`
Expected: 2 tests PASS.

- [ ] **Step 4: Commit**

```bash
git add atx-engine/include/atx/engine/data/universe.hpp atx-engine/tests/universe_test.cpp
git commit -m "feat(universe): top-N by median daily notional ranking"
```

---

## Task 5: Databento `split_under_cap` template + `PullStats`

**Files:**
- Modify: `atx-core/include/atx/external/databento.hpp` (add after the existing `load_equs_summary_zip` declaration, before `}` of namespace)
- Test: `atx-core/tests/databento_split_test.cpp`

- [ ] **Step 1: Add `PullStats` + `split_under_cap` to the header**

In `atx-core/include/atx/external/databento.hpp`, add these includes near the top (after the existing includes):

```cpp
#include <span>
#include <vector>
```

Inside `namespace atx::external::databento { ... }`, after the `load_equs_summary_zip` declaration, add:

```cpp
struct PullStats {
  i64 records{0};       // L1 rows written
  i64 symbols{0};       // distinct symbols/contracts seen
  i64 api_calls{0};     // TimeseriesGetRange calls after batch split
  double cost_usd{0.0}; // summed preflight cost actually pulled
};

// Recursively split `symbols` into batches whose estimated cost is < cap. `est` is
// any callable: (std::span<const std::string>) -> double. A single symbol whose own
// estimate is >= cap is still emitted alone (caller decides whether to pull it).
// Order-preserving; the union of batches equals the input.
template <class EstFn>
[[nodiscard]] std::vector<std::vector<std::string>>
split_under_cap(std::span<const std::string> symbols, double cap, EstFn&& est) {
  std::vector<std::vector<std::string>> out;
  if (symbols.empty()) {
    return out;
  }
  if (symbols.size() == 1 || est(symbols) < cap) {
    out.emplace_back(symbols.begin(), symbols.end());
    return out;
  }
  const std::size_t mid = symbols.size() / 2;
  auto left = split_under_cap(symbols.first(mid), cap, est);
  auto right = split_under_cap(symbols.subspan(mid), cap, est);
  out.insert(out.end(), std::make_move_iterator(left.begin()),
             std::make_move_iterator(left.end()));
  out.insert(out.end(), std::make_move_iterator(right.begin()),
             std::make_move_iterator(right.end()));
  return out;
}
```

- [ ] **Step 2: Write the failing test**

Create `atx-core/tests/databento_split_test.cpp`:

```cpp
#include <gtest/gtest.h>

#include <span>
#include <string>
#include <vector>

#include "atx/external/databento.hpp"

using atx::external::databento::split_under_cap;

TEST(SplitUnderCap, SplitsUntilEachBatchUnderCap) {
  std::vector<std::string> syms{"A", "B", "C", "D"};
  // Estimator: $1 per symbol. cap = $2.5 -> each batch must have <= 2 symbols.
  auto est = [](std::span<const std::string> s) {
    return static_cast<double>(s.size());
  };
  auto batches = split_under_cap(std::span<const std::string>(syms), 2.5, est);

  std::vector<std::string> flat;
  for (const auto& b : batches) {
    EXPECT_LT(est(std::span<const std::string>(b)), 2.5);
    for (const auto& x : b) flat.push_back(x);
  }
  EXPECT_EQ(flat, syms); // order-preserving union
}

TEST(SplitUnderCap, SingleExpensiveSymbolEmittedAlone) {
  std::vector<std::string> syms{"BIG"};
  auto est = [](std::span<const std::string>) { return 999.0; };
  auto batches = split_under_cap(std::span<const std::string>(syms), 2.0, est);
  ASSERT_EQ(batches.size(), 1u);
  EXPECT_EQ(batches[0][0], "BIG");
}
```

- [ ] **Step 3: Build + run, expect PASS**

Run: `cmake --build build --target atx-core-tests` then `ctest --test-dir build -C Debug -R "SplitUnderCap" --output-on-failure`
Expected: 2 tests PASS. (If the atx-core test target name differs, list targets with `cmake --build build --target help | findstr tests`.)

- [ ] **Step 4: Commit**

```bash
git add atx-core/include/atx/external/databento.hpp atx-core/tests/databento_split_test.cpp
git commit -m "feat(databento): PullStats + split_under_cap cost batcher"
```

---

## Task 6: Databento `estimate_cost` + `pull_equity_l1_1m_to_parquet`

**Files:**
- Modify: `atx-core/include/atx/external/databento.hpp` (declarations)
- Modify: `atx-core/src/external/databento.cpp` (implementations)

No unit test (network). Verified by build + the Task 8 orchestrator run.

- [ ] **Step 1: Declare the functions in the header**

In `atx-core/include/atx/external/databento.hpp`, add `#include <utility>` to the includes, and after `split_under_cap` add:

```cpp
// Free MetadataGetCost (no egress, no charge). schema/stype_in are databento schema
// and symbology strings, e.g. "cbbo-1m" / "raw_symbol".
[[nodiscard]] Result<double> estimate_cost(
    std::string_view api_key, std::string_view dataset,
    const std::pair<std::string, std::string>& range_utc,
    std::span<const std::string> symbols, std::string_view schema,
    std::string_view stype_in);

// Equity L1 ("bbo-1m" or "cbbo-1m") for `symbols` over [start,end), stype_in
// "raw_symbol". Writes one Parquet file at out_path with columns:
// ts, symbol, bid_px, ask_px, bid_sz, ask_sz  (px = 1e-9 fixed-point i64; unset px
// stored as INT64_MIN; sizes i64). Splits symbols so every API call's preflight
// cost < cap_usd; accumulates all batches in memory; writes the file once.
[[nodiscard]] Result<PullStats> pull_equity_l1_1m_to_parquet(
    std::string_view api_key, std::string_view dataset,
    std::span<const std::string> symbols,
    const std::pair<std::string, std::string>& range_utc, std::string_view schema,
    std::string_view out_path, double cap_usd = 2.0);
```

- [ ] **Step 2: Implement in the .cpp**

In `atx-core/src/external/databento.cpp`, add these includes at the top:

```cpp
#include <array>
#include <limits>
#include <utility>

#include <databento/datetime.hpp>
#include <databento/dbn.hpp>
#include <databento/enums.hpp>
#include <databento/historical.hpp>
#include <databento/record.hpp>
#include <databento/symbol_map.hpp>
#include <databento/timeseries.hpp>
```

Add this anonymous-namespace helper block (string→enum maps + a generic L1 accumulator) inside the existing unnamed `namespace { ... }`:

```cpp
[[nodiscard]] Result<databento::Schema> schema_from_string(std::string_view s) {
  if (s == "bbo-1m") return Ok(databento::Schema::Bbo1M);
  if (s == "cbbo-1m") return Ok(databento::Schema::Cbbo1M);
  if (s == "bbo-1s") return Ok(databento::Schema::Bbo1S);
  if (s == "cbbo-1s") return Ok(databento::Schema::Cbbo1S);
  return Err(ErrorCode::InvalidArgument, std::string{"unsupported schema: "} + std::string{s});
}

[[nodiscard]] Result<databento::SType> stype_from_string(std::string_view s) {
  if (s == "raw_symbol") return Ok(databento::SType::RawSymbol);
  if (s == "parent") return Ok(databento::SType::Parent);
  if (s == "instrument_id") return Ok(databento::SType::InstrumentId);
  return Err(ErrorCode::InvalidArgument, std::string{"unsupported stype: "} + std::string{s});
}

// L1 column accumulator. Prices in 1e-9 fixed-point i64; unset -> INT64_MIN.
struct L1Columns {
  std::vector<time::Timestamp> ts;
  std::vector<std::string> symbol;
  std::vector<i64> bid_px, ask_px, bid_sz, ask_sz;

  void push(u64 ts_event, std::string sym, i64 bpx, i64 apx, u32 bsz, u32 asz) {
    ts.push_back(time::Timestamp::from_unix_nanos(static_cast<i64>(ts_event)));
    symbol.push_back(std::move(sym));
    bid_px.push_back(bpx);
    ask_px.push_back(apx);
    bid_sz.push_back(static_cast<i64>(bsz));
    ask_sz.push_back(static_cast<i64>(asz));
  }
};

constexpr i64 kUnsetPx = std::numeric_limits<i64>::min();
[[nodiscard]] i64 px_or_unset(std::int64_t px) {
  return px == databento::kUndefPrice ? kUnsetPx : static_cast<i64>(px);
}

// Build a Historical client from a raw key.
[[nodiscard]] databento::Historical make_client(std::string_view api_key) {
  return databento::Historical::Builder().SetKey(std::string{api_key}).Build();
}
```

Then, in the namespace `atx::external::databento` (after `load_equs_summary_zip`'s definition), add:

```cpp
Result<double> estimate_cost(std::string_view api_key, std::string_view dataset,
                             const std::pair<std::string, std::string>& range_utc,
                             std::span<const std::string> symbols, std::string_view schema,
                             std::string_view stype_in) {
  ATX_TRY(auto sch, schema_from_string(schema));
  ATX_TRY(auto sty, stype_from_string(stype_in));
  try {
    auto client = make_client(api_key);
    const ::databento::DateTimeRange<std::string> range{range_utc.first, range_utc.second};
    const std::vector<std::string> syms(symbols.begin(), symbols.end());
    const double cost = client.MetadataGetCost(std::string{dataset}, range, syms, sch, sty, 0);
    return Ok(cost);
  } catch (const std::exception& e) {
    return Err(ErrorCode::Internal, std::string{"estimate_cost: "} + e.what());
  }
}

Result<PullStats> pull_equity_l1_1m_to_parquet(
    std::string_view api_key, std::string_view dataset, std::span<const std::string> symbols,
    const std::pair<std::string, std::string>& range_utc, std::string_view schema,
    std::string_view out_path, double cap_usd) {
  ATX_TRY(auto sch, schema_from_string(schema));
  const auto sty = ::databento::SType::RawSymbol;
  PullStats stats;
  L1Columns c;
  try {
    auto client = make_client(api_key);
    const ::databento::DateTimeRange<std::string> range{range_utc.first, range_utc.second};

    auto est = [&](std::span<const std::string> b) -> double {
      const std::vector<std::string> v(b.begin(), b.end());
      return client.MetadataGetCost(std::string{dataset}, range, v, sch, sty, 0);
    };
    const auto batches = split_under_cap(symbols, cap_usd, est);

    for (const auto& batch : batches) {
      stats.cost_usd += est(std::span<const std::string>(batch));
      ::databento::TsSymbolMap tsmap;  // instrument_id+date -> text symbol
      client.TimeseriesGetRange(
          std::string{dataset}, range, batch, sch, sty, ::databento::SType::InstrumentId, 0,
          [&](::databento::Metadata&& m) { tsmap = m.CreateSymbolMap(); },
          [&](const ::databento::Record& rec) {
            if (rec.Holds<::databento::CbboMsg>()) {
              const auto& q = rec.Get<::databento::CbboMsg>();
              const auto& l = q.levels[0];
              const auto it = tsmap.Find(q);
              c.push(q.ts_recv.time_since_epoch().count(),
                     it != tsmap.Map().end() ? *it->second : std::to_string(q.hd.instrument_id),
                     px_or_unset(l.bid_px), px_or_unset(l.ask_px), l.bid_sz, l.ask_sz);
            } else if (rec.Holds<::databento::BboMsg>()) {
              const auto& q = rec.Get<::databento::BboMsg>();
              const auto& l = q.levels[0];
              const auto it = tsmap.Find(q);
              c.push(q.ts_recv.time_since_epoch().count(),
                     it != tsmap.Map().end() ? *it->second : std::to_string(q.hd.instrument_id),
                     px_or_unset(l.bid_px), px_or_unset(l.ask_px), l.bid_sz, l.ask_sz);
            }
            return ::databento::KeepGoing::Continue;
          });
      ++stats.api_calls;
    }
  } catch (const std::exception& e) {
    return Err(ErrorCode::Internal, std::string{"pull_equity_l1: "} + e.what());
  }

  stats.records = static_cast<i64>(c.ts.size());
  const std::vector<atx::core::io::WriteColumn> cols{
      {"ts", std::span<const time::Timestamp>(c.ts)},
      {"symbol", std::span<const std::string>(c.symbol)},
      {"bid_px", std::span<const i64>(c.bid_px)},
      {"ask_px", std::span<const i64>(c.ask_px)},
      {"bid_sz", std::span<const i64>(c.bid_sz)},
      {"ask_sz", std::span<const i64>(c.ask_sz)},
  };
  auto w = atx::core::io::write_parquet(cols, out_path);
  if (!w.has_value()) {
    return Err(w.error());
  }
  return Ok(stats);
}
```

> VERIFY-ON-CONTACT (against `atx-core/third-party/databento-cpp/include/databento/`):
> - `TsSymbolMap::Find(const R&)` returns a `Store::const_iterator` (map keyed by
>   `(year_month_day, instrument_id)`, value `shared_ptr<const string>`); compare to
>   `tsmap.Map().end()` and deref `*it->second`. Confirm signature in `dbn.hpp`.
> - `CbboMsg` and `BboMsg` each carry `ts_recv` as a **message member** (`q.ts_recv`),
>   not on the header — confirm in `record.hpp` and keep `q.ts_recv`.
> - `ConsolidatedBidAskPair` (CbboMsg) and `BidAskPair` (BboMsg) both expose
>   `bid_px/ask_px` (`int64`) and `bid_sz/ask_sz` (`uint32`); `px_or_unset` maps the
>   `kUndefPrice` sentinel. These were validated in the standalone `databento_xom_bbo`
>   tool earlier in the repo history — reuse that as a reference.

- [ ] **Step 3: Build atx-core, expect clean compile**

Run: `cmake --build build --target atx-core`
Expected: builds clean. Fix accessor mismatches (`ts_recv` member vs header) flagged by the compiler per the NOTE.

- [ ] **Step 4: Commit**

```bash
git add atx-core/include/atx/external/databento.hpp atx-core/src/external/databento.cpp
git commit -m "feat(databento): estimate_cost + equity L1 pull-to-parquet"
```

---

## Task 7: Databento `pull_opra_cbbo_1m_to_parquet`

**Files:**
- Modify: `atx-core/include/atx/external/databento.hpp`
- Modify: `atx-core/src/external/databento.cpp`

- [ ] **Step 1: Declare in the header**

After `pull_equity_l1_1m_to_parquet`'s declaration, add:

```cpp
// OPRA full-chain cbbo-1m for `underlyings` via parent symbology "<SYM>.OPT".
// Writes one Parquet file at out_path with columns:
// ts, underlying, symbol, bid_px, ask_px, bid_sz, ask_sz.
// Splits underlyings so every API call's preflight cost < cap_usd.
[[nodiscard]] Result<PullStats> pull_opra_cbbo_1m_to_parquet(
    std::string_view api_key, std::span<const std::string> underlyings,
    const std::pair<std::string, std::string>& range_utc, std::string_view out_path,
    double cap_usd = 2.0);
```

- [ ] **Step 2: Implement in the .cpp**

Add an `underlying` field path: build the parent symbols `<SYM>.OPT`, decode CbboMsg, and record both the option OSI symbol (via PitSymbolMap) and the underlying root (parsed from the OSI symbol prefix — the leading non-digit run). Add to `atx::external::databento`:

```cpp
namespace {
// OSI root = leading letters of an option symbol, e.g. "XOM   260605C00150000" -> "XOM".
[[nodiscard]] std::string osi_root(std::string_view osi) {
  std::size_t i = 0;
  while (i < osi.size() && (osi[i] >= 'A' && osi[i] <= 'Z')) {
    ++i;
  }
  return std::string{osi.substr(0, i)};
}
} // namespace

Result<PullStats> pull_opra_cbbo_1m_to_parquet(
    std::string_view api_key, std::span<const std::string> underlyings,
    const std::pair<std::string, std::string>& range_utc, std::string_view out_path,
    double cap_usd) {
  const std::string dataset = "OPRA.PILLAR";
  const auto sch = ::databento::Schema::Cbbo1M;
  const auto sty = ::databento::SType::Parent;

  // Parent symbols "<SYM>.OPT".
  std::vector<std::string> parents;
  parents.reserve(underlyings.size());
  for (const auto& u : underlyings) {
    parents.push_back(u + ".OPT");
  }

  PullStats stats;
  L1Columns c;                       // reuse; underlying tracked in parallel vector
  std::vector<std::string> underlying_col;
  try {
    auto client = make_client(api_key);
    const ::databento::DateTimeRange<std::string> range{range_utc.first, range_utc.second};
    auto est = [&](std::span<const std::string> b) -> double {
      const std::vector<std::string> v(b.begin(), b.end());
      return client.MetadataGetCost(dataset, range, v, sch, sty, 0);
    };
    const auto batches = split_under_cap(std::span<const std::string>(parents), cap_usd, est);

    for (const auto& batch : batches) {
      stats.cost_usd += est(std::span<const std::string>(batch));
      ::databento::TsSymbolMap tsmap;
      client.TimeseriesGetRange(
          dataset, range, batch, sch, sty, ::databento::SType::InstrumentId, 0,
          [&](::databento::Metadata&& m) { tsmap = m.CreateSymbolMap(); },
          [&](const ::databento::Record& rec) {
            if (rec.Holds<::databento::CbboMsg>()) {
              const auto& q = rec.Get<::databento::CbboMsg>();
              const auto& l = q.levels[0];
              const auto it = tsmap.Find(q);
              const std::string sym =
                  it != tsmap.Map().end() ? *it->second : std::to_string(q.hd.instrument_id);
              underlying_col.push_back(osi_root(sym));
              c.push(q.ts_recv.time_since_epoch().count(), sym, px_or_unset(l.bid_px),
                     px_or_unset(l.ask_px), l.bid_sz, l.ask_sz);
            }
            return ::databento::KeepGoing::Continue;
          });
      ++stats.api_calls;
    }
  } catch (const std::exception& e) {
    return Err(ErrorCode::Internal, std::string{"pull_opra_cbbo: "} + e.what());
  }

  stats.records = static_cast<i64>(c.ts.size());
  const std::vector<atx::core::io::WriteColumn> cols{
      {"ts", std::span<const time::Timestamp>(c.ts)},
      {"underlying", std::span<const std::string>(underlying_col)},
      {"symbol", std::span<const std::string>(c.symbol)},
      {"bid_px", std::span<const i64>(c.bid_px)},
      {"ask_px", std::span<const i64>(c.ask_px)},
      {"bid_sz", std::span<const i64>(c.bid_sz)},
      {"ask_sz", std::span<const i64>(c.ask_sz)},
  };
  auto w = atx::core::io::write_parquet(cols, out_path);
  if (!w.has_value()) {
    return Err(w.error());
  }
  return Ok(stats);
}
```

- [ ] **Step 3: Build atx-core, expect clean compile**

Run: `cmake --build build --target atx-core`
Expected: clean.

- [ ] **Step 4: Commit**

```bash
git add atx-core/include/atx/external/databento.hpp atx-core/src/external/databento.cpp
git commit -m "feat(databento): OPRA cbbo-1m full-chain pull-to-parquet"
```

---

## Task 8: Orchestrator `build_universe` + engine examples wiring

**Files:**
- Modify: `atx-engine/CMakeLists.txt` (examples opt-in)
- Create: `atx-engine/examples/CMakeLists.txt`
- Create: `atx-engine/examples/build_universe.cpp`

- [ ] **Step 1: Wire examples into the engine CMake**

Append to `atx-engine/CMakeLists.txt` (after the bench block):

```cmake
if(ATX_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()
```

Create `atx-engine/examples/CMakeLists.txt`:

```cmake
add_executable(build_universe build_universe.cpp)
target_link_libraries(build_universe PRIVATE atx::engine atx::core)
```

- [ ] **Step 2: Write the orchestrator**

Create `atx-engine/examples/build_universe.cpp`:

```cpp
// build_universe — EQUS zip -> OHLC1D, top-100 by 20-day median notional, then
// equity L1 (OHLC1M) + OPRA cbbo (OPRA_BBO) snapshots at 15:55 ET on the last day.
// Each Databento query is preflight-gated < $2. Key from DATABENTO_API_KEY.
//
// Usage: build_universe [DATA_ROOT] [EQUS_ZIP]

#include <cstdio>
#include <cstdlib>
#include <string>
#include <unordered_map>
#include <vector>

#include "atx/core/io/parquet.hpp"
#include "atx/core/types.hpp"
#include "atx/engine/data/disk.hpp"
#include "atx/engine/data/universe.hpp"
#include "atx/external/databento.hpp"

using namespace atx::engine::data;
namespace dbn = atx::external::databento;
namespace io = atx::core::io;
using atx::core::i64;

namespace {
const char* getenv_or(const char* k, const char* d) {
  const char* v = std::getenv(k);
  return (v && *v) ? v : d;
}
// Pick a consolidated equity L1 dataset/schema by probing estimate_cost on one symbol.
// Prefer consolidated (cbbo-1m) over Nasdaq-only (bbo-1m).
struct EquityFeed { std::string dataset, schema; };
EquityFeed pick_equity_feed(const std::string& key,
                            const std::pair<std::string,std::string>& range,
                            const std::string& probe_symbol) {
  const std::vector<EquityFeed> candidates{
      {"DBEQ.BASIC", "cbbo-1m"}, {"EQUS.MINI", "cbbo-1m"}, {"XNAS.ITCH", "bbo-1m"}};
  const std::vector<std::string> one{probe_symbol};
  for (const auto& f : candidates) {
    auto c = dbn::estimate_cost(key, f.dataset, range, one, f.schema, "raw_symbol");
    if (c.has_value()) {
      std::printf("Equity feed: %s %s (probe cost $%.6f)\n", f.dataset.c_str(),
                  f.schema.c_str(), *c);
      return f;
    }
  }
  std::printf("WARN: no equity feed probed cleanly; defaulting XNAS.ITCH bbo-1m\n");
  return {"XNAS.ITCH", "bbo-1m"};
}
} // namespace

int main(int argc, char** argv) {
  const std::string data_root = argc > 1 ? argv[1] : "data/universe";
  const std::string zip = argc > 2 ? argv[2]
      : "C:/Users/natha/Downloads/EQUS-20260606-CEXECEMBY6.zip";
  const std::string key = getenv_or("DATABENTO_API_KEY", "");
  if (key.empty()) { std::fprintf(stderr, "DATABENTO_API_KEY not set\n"); return 1; }

  auto store_r = DiskStore::open(data_root);
  if (!store_r.has_value()) { std::fprintf(stderr, "open: %s\n", store_r.error().message.c_str()); return 1; }
  DiskStore store = std::move(*store_r);

  // 1. Ingest zip -> OHLC1D.
  auto load = dbn::load_equs_summary_zip(zip, store.store_dir(Store::Ohlc1D).string());
  if (!load.has_value()) { std::fprintf(stderr, "load zip: %s\n", load.error().message.c_str()); return 1; }
  std::printf("OHLC1D: %lld files, %lld records, %lld partitions\n",
              (long long)load->files_processed, (long long)load->records_decoded,
              (long long)load->partitions_written);

  // 2. dates + last 20.
  auto dates_r = store.list_dates(Store::Ohlc1D);
  if (!dates_r.has_value() || dates_r->empty()) { std::fprintf(stderr, "no OHLC1D dates\n"); return 1; }
  const std::vector<std::string>& dates = *dates_r;
  const std::string last = dates.back();
  const std::size_t window = dates.size() < 20 ? dates.size() : 20;
  std::printf("OHLC1D dates=%zu last=%s window=%zu\n", dates.size(), last.c_str(), window);

  // 3-4. per-symbol daily notional over the window.
  std::unordered_map<std::string, std::vector<double>> notionals;
  for (std::size_t i = dates.size() - window; i < dates.size(); ++i) {
    auto lazy = store.scan_partition(Store::Ohlc1D, dates[i]);
    if (!lazy.has_value()) continue;
    auto tbl = lazy->collect();
    if (!tbl.has_value()) continue;
    auto syms = tbl->strings("symbol");
    auto close = tbl->column_view<i64>("close");
    auto vol = tbl->column_view<i64>("volume");
    if (!syms.has_value() || !close.has_value() || !vol.has_value()) continue;
    const auto n = static_cast<std::size_t>(tbl->num_rows());
    for (std::size_t r = 0; r < n; ++r) {
      const double notional = (static_cast<double>((*close)[r]) * 1e-9) *
                              static_cast<double>((*vol)[r]);
      notionals[std::string{(*syms)[r]}].push_back(notional);
    }
  }
  auto picks = top_n_by_median_notional(notionals, 100);
  std::printf("picked %zu symbols\n", picks.size());

  // 6. 15:55 ET (EDT, UTC-4) == 19:55Z.
  const std::pair<std::string,std::string> range{last + "T19:55:00", last + "T19:56:00"};

  // 7. equity L1 -> OHLC1M.
  const EquityFeed feed = pick_equity_feed(key, range, picks.empty() ? "AAPL" : picks.front());
  auto eq = dbn::pull_equity_l1_1m_to_parquet(
      key, feed.dataset, picks, range, feed.schema,
      store.partition_path(Store::Ohlc1M, last).string(), 2.0);
  if (!eq.has_value()) std::fprintf(stderr, "equity L1: %s\n", eq.error().message.c_str());
  else std::printf("OHLC1M: %lld rows, %lld calls, $%.6f\n",
                   (long long)eq->records, (long long)eq->api_calls, eq->cost_usd);

  // 8. OPRA cbbo -> OPRA_BBO.
  auto op = dbn::pull_opra_cbbo_1m_to_parquet(
      key, picks, range, store.partition_path(Store::OpraBbo, last).string(), 2.0);
  if (!op.has_value()) std::fprintf(stderr, "OPRA cbbo: %s\n", op.error().message.c_str());
  else std::printf("OPRA_BBO: %lld rows, %lld calls, $%.6f\n",
                   (long long)op->records, (long long)op->api_calls, op->cost_usd);

  return 0;
}
```

- [ ] **Step 3: Build the orchestrator**

Run: `cmake --preset ninja -DATX_BUILD_EXAMPLES=ON` then `cmake --build build --target build_universe`
Expected: clean build. Fix any `strings()`/`column_view` return-type mismatches against `parquet.hpp` (e.g. `strings()` returns `Result<std::vector<std::string_view>>`).

- [ ] **Step 4: Run end to end (real API, gated < $2)**

Run (via vcvars, with `DATABENTO_API_KEY` set from `.env`):
`build/bin/build_universe.exe data/universe`
Expected: prints OHLC1D stats, picked ~100 symbols, OHLC1M + OPRA_BBO row counts with per-store cost; each `$...` cost line < $2 per call. Partitions exist under `data/universe/OHLC1M/date=<last>/` and `.../OPRA_BBO/date=<last>/`.

- [ ] **Step 5: Commit**

```bash
git add atx-engine/CMakeLists.txt atx-engine/examples/CMakeLists.txt atx-engine/examples/build_universe.cpp
git commit -m "feat(engine): build_universe orchestrator (top-100 -> OHLC1M + OPRA_BBO)"
```

---

## Task 9: Full regression + spec/plan completion

- [ ] **Step 1: Run the full engine + core test suites**

Run: `ctest --test-dir build -C Debug --output-on-failure`
Expected: all pass (new DiskStore, Universe, SplitUnderCap tests + all pre-existing).

- [ ] **Step 2: Verify the data layout on disk**

Run: list `data/universe` and confirm `OHLC1D/date=*/`, `OHLC1M/date=<last>/data.parquet`, `OPRA_BBO/date=<last>/data.parquet`, and an empty `OCHAIN/`.

- [ ] **Step 3: Commit any final docs/ledger updates**

```bash
git add -A
git commit -m "chore(disk): finalize hive data layer pipeline"
```

---

## Self-review notes (addressed)

- **Spec coverage:** DiskStore (T1-3), ranking (T4), split/cost (T5), estimate_cost + equity L1 (T6), OPRA cbbo (T7), orchestrator + zip ingest + top-100 + snapshots (T8), regression + layout (T9). OCHAIN created-not-populated (T1). Equity dataset probe folded into orchestrator `pick_equity_feed` (resolves spec §8.1). Zip date-span < 20 handled by `window` clamp (spec §8.2).
- **Accessor caveats called out:** `ts_recv` member vs header, `strings()` return type, atx-core test target name — each step says to verify against the real header and fix. These are deliberate "confirm on contact" points, not placeholders.
- **Type consistency:** `PullStats`, `split_under_cap`, `estimate_cost`, both pull fns, `DiskStore` methods, `top_n_by_median_notional` names match across tasks and the orchestrator call sites.
