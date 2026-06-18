# Regime / Macro Data Module Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add an `atx::engine::regime` submodule that ingests offline-staged public macro CSVs (VIX, 2s10s, MOVE, credit spreads, …) into a sealed tsdb segment, and surfaces those series into the simulation panel as broadcast `regime_*` fields the alpha DSL reads via existing ops.

**Architecture:** Phase 1 is a self-contained data module: CSV source adapters → align onto a master date axis with forward-fill → derive composites (e.g. `t10y2y = dgs10 - dgs2`) → pivot to `atx::tsdb::LongColumns` → `build_from_long` writes a sealed segment + a hand-written JSON manifest; a `RegimeStore` reads it back with as-of (`<=`) lookup. Phase 2 adds `regime::with_regime_fields` (mirrors `alpha::datafields::with_datafields`) that broadcasts each series across all in-universe instruments per date, plus atx-impl config flags + a `regime` subcommand. No VM/opcode changes: regime conditioning is an authored/evolved expression like `regime_vix < 20 ? <expr> : 0`.

**Tech Stack:** C++20, `atx::core` (types/Result/error), `atx::tsdb` (SegmentBuilder / build_from_long / segment read accessors), `atx::engine::alpha` (Panel, parser/typecheck/VM), GoogleTest, CMake (explicit source lists).

## Global Constraints

- **Namespace:** `atx::engine::regime` (headers under `include/atx/engine/regime/`, sources under `src/regime/`).
- **Types:** use `atx::i64`, `atx::f64`, `atx::u8`, `atx::u32`, `atx::usize`. Errors via `atx::core::Result<T>` / `atx::core::Status`, `atx::core::Ok(...)`, `atx::core::Err(ErrorCode, msg)`, and the `ATX_TRY(...)` macro.
- **No network in C++.** Ingest reads local files only (offline-staged CSV). Networking lives in a Python staging script outside the build.
- **Determinism (byte-reproducible):** sorted+deduped master date axis; canonical series ordering; fixed `from_chars` parse; NaN sentinel for missing; segment digest is the `SegmentBuilder` CRC32. Re-running the loader on the same staged snapshot ⇒ byte-identical `.seg`.
- **PIT / causality:** forward-fill carries the last observation forward only (NaN before first obs); panel join uses as-of (`<=`) lookup. No look-ahead.
- **Reserved field namespace:** broadcast columns are named `regime_<series>`; a collision with an existing panel field is a hard error (fail-closed).
- **No-regression:** with `--regime-segs` absent, the `build_real_panel` path and its digest are unchanged.
- **CMake:** `atx-engine/CMakeLists.txt` uses an EXPLICIT source list (not glob). Every new `src/regime/*.cpp` must be added by name. New deps: none (reuse `atx::core` + `atx::tsdb`, already linked).
- **Spec:** `docs/superpowers/specs/2026-06-18-regime-macro-data-module-design.md`.

---

## File Structure

**Phase 1 (data module):**
- Create `atx-engine/include/atx/engine/regime/series.hpp` — canonical series names, `kRegimePrefix`, derived-spec parse.
- Create `atx-engine/include/atx/engine/regime/source_csv.hpp` + `src/regime/source_csv.cpp` — CSV adapters → `(date_nanos, value)`.
- Create `atx-engine/include/atx/engine/regime/align.hpp` + `src/regime/align.cpp` — master axis, forward-fill, derived application (pure functions).
- Create `atx-engine/include/atx/engine/regime/loader.hpp` + `src/regime/loader.cpp` — orchestrate → `LongColumns` → `build_from_long` → manifest.
- Create `atx-engine/include/atx/engine/regime/store.hpp` + `src/regime/store.cpp` — sealed-segment read API with as-of lookup.
- Create `atx-engine/include/atx/engine/regime/README.md` — field dictionary + RUNBOOK pointer.
- Modify `atx-engine/CMakeLists.txt` — register the four `src/regime/*.cpp`.
- Modify `atx-engine/tests/CMakeLists.txt` (+ create `atx-engine/tests/regime/*`) — register the regime test group.

**Phase 2 (surface):**
- Create `atx-engine/include/atx/engine/regime/with_regime_fields.hpp` + `src/regime/with_regime_fields.cpp` — panel augmenter.
- Modify `atx-impl/src/config.hpp` + `config.cpp` — `--regime-segs`, `--regime-fields`, `regime` subcommand.
- Create `scripts/regime/fetch_regime.py` — offline staging (non-build).

---

## Task 1: Module skeleton, CMake wiring, and `series.hpp`

**Files:**
- Create: `atx-engine/include/atx/engine/regime/series.hpp`
- Modify: `atx-engine/CMakeLists.txt` (add the regime source block after line 94, before the closing `)`)
- Modify: `atx-engine/tests/CMakeLists.txt` (register a `regime` test group — follow the existing per-module group pattern)
- Test: `atx-engine/tests/regime/regime_series_test.cpp`

**Interfaces:**
- Produces:
  - `atx::engine::regime::kRegimePrefix` (`std::string_view` == `"regime_"`).
  - `atx::engine::regime::kCanonicalSeries` (`std::span<const std::string_view>`) — the documented v1 series names.
  - `struct DerivedSpec { std::string name; std::string lhs; char op; std::string rhs; };`
  - `parse_derived_spec(std::string_view) -> atx::core::Result<DerivedSpec>` — parses `"t10y2y = dgs10 - dgs2"`; `op ∈ {'+','-','*','/'}`.

- [ ] **Step 1: Create the CMake source block.** In `atx-engine/CMakeLists.txt`, immediately after line 94 (`src/fund/meta_book.cpp`) and before the closing `)` on line 95, add:

```cmake
    # regime (macro/regime data: offline CSV -> sealed segment + panel augmenter)
    src/regime/source_csv.cpp
    src/regime/align.cpp
    src/regime/loader.cpp
    src/regime/store.cpp
    src/regime/with_regime_fields.cpp
```

(All five are declared now; their `.cpp` files are created by later tasks. Create empty-but-compiling stubs as each is referenced — see Step 2. CMake will not configure until the files exist, so create the stub files in the same commit.)

- [ ] **Step 2: Create compiling stubs** so the build configures. Create each of these with just an include + namespace open/close:

`atx-engine/src/regime/source_csv.cpp`:
```cpp
#include "atx/engine/regime/source_csv.hpp"
namespace atx::engine::regime {}  // impls land in Task 2
```
Create the analogous one-line stubs for `align.cpp`, `loader.cpp`, `store.cpp`, `with_regime_fields.cpp`, each including its (to-be-created) header. For Task 1, also create minimal headers so the stubs compile: `source_csv.hpp`, `align.hpp`, `loader.hpp`, `store.hpp`, `with_regime_fields.hpp` may start as `#pragma once` + namespace block; later tasks fill them.

- [ ] **Step 3: Register the test group.** In `atx-engine/tests/CMakeLists.txt`, add a `regime` group mirroring an existing module group (e.g. how `alpha` tests are registered). Point it at `tests/regime/*.cpp`. Create the directory `atx-engine/tests/regime/`.

- [ ] **Step 4: Write the failing test** `atx-engine/tests/regime/regime_series_test.cpp`:

```cpp
#include <string>
#include <gtest/gtest.h>
#include "atx/engine/regime/series.hpp"

namespace atxtest_regime_series {
using atx::engine::regime::parse_derived_spec;

TEST(RegimeSeries, PrefixIsRegimeUnderscore) {
  EXPECT_EQ(atx::engine::regime::kRegimePrefix, "regime_");
}

TEST(RegimeSeries, ParseDerivedSpec_Subtraction) {
  auto r = parse_derived_spec("t10y2y = dgs10 - dgs2");
  ASSERT_TRUE(r.has_value()) << (r ? "" : r.error().message());
  EXPECT_EQ(r.value().name, "t10y2y");
  EXPECT_EQ(r.value().lhs, "dgs10");
  EXPECT_EQ(r.value().op, '-');
  EXPECT_EQ(r.value().rhs, "dgs2");
}

TEST(RegimeSeries, ParseDerivedSpec_RejectsMissingOperator) {
  EXPECT_FALSE(parse_derived_spec("t10y2y = dgs10 dgs2").has_value());
  EXPECT_FALSE(parse_derived_spec("garbage").has_value());
}
}  // namespace atxtest_regime_series
```

- [ ] **Step 5: Run the test, verify it fails to compile/link** (`series.hpp` lacks the symbols).
Run: build the `atx-engine` regime test target.
Expected: FAIL — `parse_derived_spec` / `kRegimePrefix` not declared.

- [ ] **Step 6: Implement `series.hpp`:**

```cpp
#pragma once

#include <array>
#include <span>
#include <string>
#include <string_view>

#include "atx/core/error.hpp"

namespace atx::engine::regime {

// Field-name prefix reserved for broadcast regime columns in a Panel.
inline constexpr std::string_view kRegimePrefix = "regime_";

// Documented v1 series (config can request a subset/superset; this is the
// canonical naming reference, mirrored in regime/README.md).
inline constexpr std::array<std::string_view, 11> kCanonicalSeriesArr = {
    "vix", "vvix", "move",                     // vol complex
    "dgs2", "dgs10", "t10y2y", "t3m10y",       // rates / curve
    "hy_oas", "ig_oas", "nfci",                // credit / liquidity
    "spx_dist_200dma"};                        // breadth / trend
[[nodiscard]] inline std::span<const std::string_view> kCanonicalSeries() noexcept {
  return kCanonicalSeriesArr;
}

// A composite series defined as `name = lhs OP rhs` over two other series.
struct DerivedSpec {
  std::string name;
  std::string lhs;
  char op{'-'};
  std::string rhs;
};

// Parse "name = lhs OP rhs" (whitespace-insensitive; OP in {+,-,*,/}).
// Err(InvalidArgument) on any shape mismatch.
[[nodiscard]] inline atx::core::Result<DerivedSpec> parse_derived_spec(std::string_view s) {
  auto trim = [](std::string_view v) {
    while (!v.empty() && (v.front() == ' ' || v.front() == '\t')) v.remove_prefix(1);
    while (!v.empty() && (v.back() == ' ' || v.back() == '\t')) v.remove_suffix(1);
    return v;
  };
  const std::size_t eq = s.find('=');
  if (eq == std::string_view::npos) {
    return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                          "parse_derived_spec: missing '='");
  }
  const std::string_view name = trim(s.substr(0, eq));
  std::string_view rhs_expr = trim(s.substr(eq + 1));
  std::size_t op_pos = std::string_view::npos;
  char op = 0;
  for (std::size_t i = 0; i < rhs_expr.size(); ++i) {
    const char c = rhs_expr[i];
    if (i > 0 && (c == '+' || c == '-' || c == '*' || c == '/')) {
      op_pos = i;
      op = c;
      break;
    }
  }
  if (name.empty() || op_pos == std::string_view::npos) {
    return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                          "parse_derived_spec: expected 'name = lhs OP rhs'");
  }
  const std::string_view lhs = trim(rhs_expr.substr(0, op_pos));
  const std::string_view rhs = trim(rhs_expr.substr(op_pos + 1));
  if (lhs.empty() || rhs.empty()) {
    return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                          "parse_derived_spec: empty operand");
  }
  return atx::core::Ok(DerivedSpec{std::string{name}, std::string{lhs}, op, std::string{rhs}});
}

}  // namespace atx::engine::regime
```

- [ ] **Step 7: Run the test, verify it passes.** Expected: 3 tests PASS.

- [ ] **Step 8: Commit.**
```bash
git add atx-engine/include/atx/engine/regime/ atx-engine/src/regime/ atx-engine/tests/regime/ atx-engine/CMakeLists.txt atx-engine/tests/CMakeLists.txt
git commit -m "feat(regime): module skeleton + series.hpp (names, prefix, derived-spec parse)"
```

---

## Task 2: CSV source adapters (`source_csv`)

**Files:**
- Modify: `atx-engine/include/atx/engine/regime/source_csv.hpp`
- Modify: `atx-engine/src/regime/source_csv.cpp`
- Test: `atx-engine/tests/regime/regime_source_csv_test.cpp`

**Interfaces:**
- Consumes: nothing from prior tasks.
- Produces:
  - `enum class CsvFormat : atx::u8 { Fred, Cboe, Yahoo };`
  - `parse_series_content(std::string_view content, CsvFormat fmt, std::string_view value_column) -> atx::core::Result<std::vector<std::pair<atx::i64,atx::f64>>>` — pure, no disk; sorted-ascending by date, deduped (last wins), NaN for `.`/empty.
  - `parse_series_csv(const std::string& path, CsvFormat fmt, std::string_view value_column) -> atx::core::Result<std::vector<std::pair<atx::i64,atx::f64>>>` — reads file, delegates to `parse_series_content`.
  - `date_to_nanos(std::string_view ymd) -> atx::core::Result<atx::i64>` — `"YYYY-MM-DD"` → unix nanos (UTC midnight).

- [ ] **Step 1: Write the failing test** `atx-engine/tests/regime/regime_source_csv_test.cpp`:

```cpp
#include <cmath>
#include <string>
#include <utility>
#include <vector>
#include <gtest/gtest.h>
#include "atx/engine/regime/source_csv.hpp"

namespace atxtest_regime_csv {
using atx::engine::regime::CsvFormat;
using atx::engine::regime::date_to_nanos;
using atx::engine::regime::parse_series_content;

TEST(RegimeCsv, DateToNanos_KnownEpochDay) {
  auto r = date_to_nanos("1970-01-02");
  ASSERT_TRUE(r.has_value());
  EXPECT_EQ(r.value(), static_cast<atx::i64>(86400) * 1000000000LL);
}

TEST(RegimeCsv, Fred_ParsesValueColumn_AndDotIsNaN) {
  const std::string csv =
      "DATE,VALUE\n"
      "2020-01-02,12.5\n"
      "2020-01-03,.\n"
      "2020-01-06,13.0\n";
  auto r = parse_series_content(csv, CsvFormat::Fred, "VALUE");
  ASSERT_TRUE(r.has_value()) << (r ? "" : r.error().message());
  const auto& v = r.value();
  ASSERT_EQ(v.size(), 3u);
  EXPECT_EQ(v[0].first, date_to_nanos("2020-01-02").value());
  EXPECT_DOUBLE_EQ(v[0].second, 12.5);
  EXPECT_TRUE(std::isnan(v[1].second));
  EXPECT_DOUBLE_EQ(v[2].second, 13.0);
}

TEST(RegimeCsv, Cboe_ReadsNamedCloseColumn) {
  const std::string csv =
      "DATE,OPEN,HIGH,LOW,CLOSE\n"
      "2021-03-01,10,11,9,10.5\n";
  auto r = parse_series_content(csv, CsvFormat::Cboe, "CLOSE");
  ASSERT_TRUE(r.has_value()) << (r ? "" : r.error().message());
  ASSERT_EQ(r.value().size(), 1u);
  EXPECT_DOUBLE_EQ(r.value()[0].second, 10.5);
}

TEST(RegimeCsv, SortsAscending_AndDedupsLastWins) {
  const std::string csv =
      "DATE,VALUE\n"
      "2020-01-06,2\n"
      "2020-01-02,1\n"
      "2020-01-06,9\n";   // duplicate date -> last wins
  auto r = parse_series_content(csv, CsvFormat::Fred, "VALUE");
  ASSERT_TRUE(r.has_value());
  ASSERT_EQ(r.value().size(), 2u);
  EXPECT_EQ(r.value()[0].first, date_to_nanos("2020-01-02").value());
  EXPECT_DOUBLE_EQ(r.value()[1].second, 9.0);
}

TEST(RegimeCsv, MissingValueColumn_IsError) {
  const std::string csv = "DATE,NOPE\n2020-01-02,1\n";
  EXPECT_FALSE(parse_series_content(csv, CsvFormat::Fred, "VALUE").has_value());
}
}  // namespace atxtest_regime_csv
```

- [ ] **Step 2: Run, verify it fails** (symbols undeclared). Expected: compile FAIL.

- [ ] **Step 3: Implement the header** `source_csv.hpp`:

```cpp
#pragma once

#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "atx/core/error.hpp"
#include "atx/core/types.hpp"

namespace atx::engine::regime {

enum class CsvFormat : atx::u8 { Fred, Cboe, Yahoo };

// "YYYY-MM-DD" -> unix nanos at UTC midnight. Err(ParseError) on bad shape.
[[nodiscard]] atx::core::Result<atx::i64> date_to_nanos(std::string_view ymd);

// Parse CSV TEXT (no disk). Header row names columns; the date column is the
// first column for all formats. `value_column` selects the numeric column by
// header name. Empty / "." cells -> NaN. Output is sorted-ascending by date and
// deduped (later row wins). Err(ParseError) if the header lacks `value_column`.
[[nodiscard]] atx::core::Result<std::vector<std::pair<atx::i64, atx::f64>>>
parse_series_content(std::string_view content, CsvFormat fmt, std::string_view value_column);

// Read `path`, delegate to parse_series_content. Err(IoError) if unreadable.
[[nodiscard]] atx::core::Result<std::vector<std::pair<atx::i64, atx::f64>>>
parse_series_csv(const std::string &path, CsvFormat fmt, std::string_view value_column);

}  // namespace atx::engine::regime
```

- [ ] **Step 4: Implement `source_csv.cpp`:**

```cpp
#include "atx/engine/regime/source_csv.hpp"

#include <algorithm>
#include <charconv>
#include <fstream>
#include <limits>
#include <sstream>

namespace atx::engine::regime {

namespace {

constexpr atx::f64 kNaN = std::numeric_limits<atx::f64>::quiet_NaN();

// Howard Hinnant days_from_civil (proleptic Gregorian), matching the engine's
// date math. y/m/d are 1-based month/day.
[[nodiscard]] atx::i64 days_from_civil(atx::i64 y, unsigned m, unsigned d) noexcept {
  y -= (m <= 2);
  const atx::i64 era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = static_cast<unsigned>(y - era * 400);
  const unsigned doy = (153u * (m + (m > 2 ? -3 : 9)) + 2u) / 5u + (d - 1u);
  const unsigned doe = yoe * 365u + yoe / 4u - yoe / 100u + doy;
  return era * 146097LL + static_cast<atx::i64>(doe) - 719468LL;
}

[[nodiscard]] std::vector<std::string_view> split(std::string_view line, char sep) {
  std::vector<std::string_view> out;
  std::size_t start = 0;
  for (std::size_t i = 0; i <= line.size(); ++i) {
    if (i == line.size() || line[i] == sep) {
      out.push_back(line.substr(start, i - start));
      start = i + 1;
    }
  }
  return out;
}

[[nodiscard]] std::string_view rstrip_cr(std::string_view v) {
  if (!v.empty() && v.back() == '\r') v.remove_suffix(1);
  return v;
}

}  // namespace

atx::core::Result<atx::i64> date_to_nanos(std::string_view ymd) {
  // Expect exactly "YYYY-MM-DD".
  if (ymd.size() != 10 || ymd[4] != '-' || ymd[7] != '-') {
    return atx::core::Err(atx::core::ErrorCode::ParseError,
                          std::string{"date_to_nanos: bad date '"} + std::string{ymd} + "'");
  }
  int y = 0, m = 0, d = 0;
  auto ok = [](std::from_chars_result r, const char *end) {
    return r.ec == std::errc{} && r.ptr == end;
  };
  const char *p = ymd.data();
  if (!ok(std::from_chars(p, p + 4, y), p + 4) ||
      !ok(std::from_chars(p + 5, p + 7, m), p + 7) ||
      !ok(std::from_chars(p + 8, p + 10, d), p + 10) || m < 1 || m > 12 || d < 1 || d > 31) {
    return atx::core::Err(atx::core::ErrorCode::ParseError,
                          std::string{"date_to_nanos: non-numeric '"} + std::string{ymd} + "'");
  }
  const atx::i64 days = days_from_civil(y, static_cast<unsigned>(m), static_cast<unsigned>(d));
  return atx::core::Ok(days * 86400LL * 1000000000LL);
}

atx::core::Result<std::vector<std::pair<atx::i64, atx::f64>>>
parse_series_content(std::string_view content, CsvFormat /*fmt*/, std::string_view value_column) {
  std::vector<std::pair<atx::i64, atx::f64>> rows;
  std::size_t pos = 0;
  bool header_seen = false;
  std::size_t date_col = 0;
  std::size_t val_col = std::string_view::npos;

  while (pos <= content.size()) {
    const std::size_t nl = content.find('\n', pos);
    const std::string_view raw =
        rstrip_cr(content.substr(pos, nl == std::string_view::npos ? std::string_view::npos
                                                                   : nl - pos));
    pos = (nl == std::string_view::npos) ? content.size() + 1 : nl + 1;
    if (raw.empty()) {
      if (nl == std::string_view::npos) break;
      continue;
    }
    const std::vector<std::string_view> cols = split(raw, ',');
    if (!header_seen) {
      header_seen = true;
      for (std::size_t i = 0; i < cols.size(); ++i) {
        if (cols[i] == value_column) val_col = i;
      }
      if (val_col == std::string_view::npos) {
        return atx::core::Err(atx::core::ErrorCode::ParseError,
                              std::string{"parse_series_content: header lacks column '"} +
                                  std::string{value_column} + "'");
      }
      continue;
    }
    if (date_col >= cols.size() || val_col >= cols.size()) continue;  // ragged row -> skip
    auto dn = date_to_nanos(cols[date_col]);
    if (!dn.has_value()) continue;  // unparseable date -> skip the row
    atx::f64 value = kNaN;
    const std::string_view cell = cols[val_col];
    if (!cell.empty() && cell != ".") {
      double parsed = 0.0;
      const std::from_chars_result r =
          std::from_chars(cell.data(), cell.data() + cell.size(), parsed);
      value = (r.ec == std::errc{} && r.ptr == cell.data() + cell.size()) ? parsed : kNaN;
    }
    rows.emplace_back(dn.value(), value);
  }

  std::stable_sort(rows.begin(), rows.end(),
                   [](const auto &a, const auto &b) { return a.first < b.first; });
  // Dedup keeping the LAST occurrence of each date (stable_sort preserved order).
  std::vector<std::pair<atx::i64, atx::f64>> deduped;
  for (const auto &row : rows) {
    if (!deduped.empty() && deduped.back().first == row.first) {
      deduped.back().second = row.second;  // later wins
    } else {
      deduped.push_back(row);
    }
  }
  return atx::core::Ok(std::move(deduped));
}

atx::core::Result<std::vector<std::pair<atx::i64, atx::f64>>>
parse_series_csv(const std::string &path, CsvFormat fmt, std::string_view value_column) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    return atx::core::Err(atx::core::ErrorCode::IoError,
                          std::string{"parse_series_csv: cannot open '"} + path + "'");
  }
  std::stringstream ss;
  ss << in.rdbuf();
  const std::string content = ss.str();
  return parse_series_content(content, fmt, value_column);
}

}  // namespace atx::engine::regime
```

> Note: `CsvFormat` is currently informational (the date column is column 0 and the value column is selected by header name for all three formats). It is retained so config stays declarative and format-specific quirks can be added later without an interface change.

- [ ] **Step 5: Run, verify all tests pass.** Expected: 5 PASS.

- [ ] **Step 6: Commit.**
```bash
git add atx-engine/include/atx/engine/regime/source_csv.hpp atx-engine/src/regime/source_csv.cpp atx-engine/tests/regime/regime_source_csv_test.cpp
git commit -m "feat(regime): CSV source adapters + date_to_nanos (FRED/CBOE/Yahoo)"
```

---

## Task 3: Alignment — master axis, forward-fill, derived series (`align`)

**Files:**
- Modify: `atx-engine/include/atx/engine/regime/align.hpp`
- Modify: `atx-engine/src/regime/align.cpp`
- Test: `atx-engine/tests/regime/regime_align_test.cpp`

**Interfaces:**
- Consumes: `DerivedSpec` (Task 1); `(date_nanos,value)` series (Task 2).
- Produces:
  - `struct NamedSeries { std::string name; std::vector<std::pair<atx::i64,atx::f64>> obs; };`
  - `build_master_axis(std::span<const NamedSeries>, atx::i64 min_date_nanos) -> std::vector<atx::i64>` — sorted-unique union of all observed dates `>= min_date_nanos`.
  - `forward_fill(std::span<const std::pair<atx::i64,atx::f64>> obs, std::span<const atx::i64> axis) -> std::vector<atx::f64>` — value carried forward; NaN before first obs.
  - `apply_derived(std::vector<std::string>& names, std::vector<std::vector<atx::f64>>& cols, const DerivedSpec&) -> atx::core::Status` — append `name` column computed elementwise from existing `lhs`/`rhs` columns (NaN propagates; div-by-zero → NaN). Err(NotFound) if an operand column is absent.

- [ ] **Step 1: Write the failing test** `atx-engine/tests/regime/regime_align_test.cpp`:

```cpp
#include <cmath>
#include <string>
#include <utility>
#include <vector>
#include <gtest/gtest.h>
#include "atx/engine/regime/align.hpp"
#include "atx/engine/regime/series.hpp"

namespace atxtest_regime_align {
using atx::engine::regime::apply_derived;
using atx::engine::regime::build_master_axis;
using atx::engine::regime::DerivedSpec;
using atx::engine::regime::forward_fill;
using atx::engine::regime::NamedSeries;

constexpr atx::i64 kDay = 86400LL * 1000000000LL;

TEST(RegimeAlign, MasterAxis_UnionSortedDedupedFloored) {
  std::vector<NamedSeries> s = {
      {"a", {{2 * kDay, 1.0}, {5 * kDay, 2.0}}},
      {"b", {{1 * kDay, 9.0}, {5 * kDay, 8.0}}},
  };
  auto axis = build_master_axis(s, /*min_date_nanos=*/2 * kDay);
  ASSERT_EQ(axis.size(), 2u);   // 1*kDay dropped by floor
  EXPECT_EQ(axis[0], 2 * kDay);
  EXPECT_EQ(axis[1], 5 * kDay);
}

TEST(RegimeAlign, ForwardFill_CarriesAndLeavesNaNBeforeFirst) {
  std::vector<std::pair<atx::i64, atx::f64>> obs = {{2 * kDay, 10.0}, {4 * kDay, 20.0}};
  std::vector<atx::i64> axis = {1 * kDay, 2 * kDay, 3 * kDay, 4 * kDay, 5 * kDay};
  auto col = forward_fill(obs, axis);
  ASSERT_EQ(col.size(), 5u);
  EXPECT_TRUE(std::isnan(col[0]));     // before first obs
  EXPECT_DOUBLE_EQ(col[1], 10.0);
  EXPECT_DOUBLE_EQ(col[2], 10.0);      // carried
  EXPECT_DOUBLE_EQ(col[3], 20.0);
  EXPECT_DOUBLE_EQ(col[4], 20.0);      // carried past last obs
}

TEST(RegimeAlign, ApplyDerived_SubtractionElementwise) {
  std::vector<std::string> names = {"dgs2", "dgs10"};
  std::vector<std::vector<atx::f64>> cols = {{1.0, 2.0}, {3.0, 5.0}};
  DerivedSpec spec{"t10y2y", "dgs10", '-', "dgs2"};
  auto st = apply_derived(names, cols, spec);
  ASSERT_TRUE(st.has_value()) << (st ? "" : st.error().message());
  ASSERT_EQ(names.size(), 3u);
  EXPECT_EQ(names[2], "t10y2y");
  EXPECT_DOUBLE_EQ(cols[2][0], 2.0);   // 3 - 1
  EXPECT_DOUBLE_EQ(cols[2][1], 3.0);   // 5 - 2
}

TEST(RegimeAlign, ApplyDerived_MissingOperand_IsError) {
  std::vector<std::string> names = {"dgs10"};
  std::vector<std::vector<atx::f64>> cols = {{1.0}};
  DerivedSpec spec{"t10y2y", "dgs10", '-', "dgs2"};
  EXPECT_FALSE(apply_derived(names, cols, spec).has_value());
}
}  // namespace atxtest_regime_align
```

- [ ] **Step 2: Run, verify it fails** (symbols undeclared).

- [ ] **Step 3: Implement `align.hpp`:**

```cpp
#pragma once

#include <span>
#include <string>
#include <utility>
#include <vector>

#include "atx/core/error.hpp"
#include "atx/core/types.hpp"

#include "atx/engine/regime/series.hpp"

namespace atx::engine::regime {

struct NamedSeries {
  std::string name;
  std::vector<std::pair<atx::i64, atx::f64>> obs;  // sorted-ascending by date
};

// Sorted-unique union of every series' observation dates, keeping dates >= floor.
[[nodiscard]] std::vector<atx::i64> build_master_axis(std::span<const NamedSeries> series,
                                                      atx::i64 min_date_nanos);

// Reindex `obs` onto `axis` with forward-fill: axis[i] takes the value of the
// latest obs whose date <= axis[i]; NaN before the first obs.
[[nodiscard]] std::vector<atx::f64>
forward_fill(std::span<const std::pair<atx::i64, atx::f64>> obs, std::span<const atx::i64> axis);

// Append a derived column `spec.name = lhs OP rhs` computed elementwise from the
// existing columns named spec.lhs / spec.rhs. NaN propagates; '/' by 0 -> NaN.
// Err(NotFound) if an operand name is absent; Err(InvalidArgument) on bad op.
[[nodiscard]] atx::core::Status apply_derived(std::vector<std::string> &names,
                                              std::vector<std::vector<atx::f64>> &cols,
                                              const DerivedSpec &spec);

}  // namespace atx::engine::regime
```

- [ ] **Step 4: Implement `align.cpp`:**

```cpp
#include "atx/engine/regime/align.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace atx::engine::regime {

namespace {
constexpr atx::f64 kNaN = std::numeric_limits<atx::f64>::quiet_NaN();

[[nodiscard]] atx::usize index_of(const std::vector<std::string> &names, const std::string &n) {
  for (atx::usize i = 0; i < names.size(); ++i) {
    if (names[i] == n) return i;
  }
  return static_cast<atx::usize>(-1);
}
}  // namespace

std::vector<atx::i64> build_master_axis(std::span<const NamedSeries> series,
                                        atx::i64 min_date_nanos) {
  std::vector<atx::i64> all;
  for (const NamedSeries &s : series) {
    for (const auto &kv : s.obs) {
      if (kv.first >= min_date_nanos) all.push_back(kv.first);
    }
  }
  std::sort(all.begin(), all.end());
  all.erase(std::unique(all.begin(), all.end()), all.end());
  return all;
}

std::vector<atx::f64> forward_fill(std::span<const std::pair<atx::i64, atx::f64>> obs,
                                   std::span<const atx::i64> axis) {
  std::vector<atx::f64> out(axis.size(), kNaN);
  atx::usize oi = 0;
  atx::f64 last = kNaN;
  bool have = false;
  for (atx::usize i = 0; i < axis.size(); ++i) {
    while (oi < obs.size() && obs[oi].first <= axis[i]) {
      last = obs[oi].second;
      have = true;
      ++oi;
    }
    out[i] = have ? last : kNaN;
  }
  return out;
}

atx::core::Status apply_derived(std::vector<std::string> &names,
                                std::vector<std::vector<atx::f64>> &cols, const DerivedSpec &spec) {
  const atx::usize li = index_of(names, spec.lhs);
  const atx::usize ri = index_of(names, spec.rhs);
  if (li == static_cast<atx::usize>(-1) || ri == static_cast<atx::usize>(-1)) {
    return atx::core::Err(atx::core::ErrorCode::NotFound,
                          std::string{"apply_derived: missing operand for '"} + spec.name + "'");
  }
  const std::vector<atx::f64> &lhs = cols[li];
  const std::vector<atx::f64> &rhs = cols[ri];
  std::vector<atx::f64> out(lhs.size(), kNaN);
  for (atx::usize i = 0; i < lhs.size(); ++i) {
    const atx::f64 a = lhs[i];
    const atx::f64 b = rhs[i];
    switch (spec.op) {
      case '+': out[i] = a + b; break;
      case '-': out[i] = a - b; break;
      case '*': out[i] = a * b; break;
      case '/': out[i] = (b == 0.0) ? kNaN : a / b; break;
      default:
        return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                              std::string{"apply_derived: bad op for '"} + spec.name + "'");
    }
  }
  names.push_back(spec.name);
  cols.push_back(std::move(out));
  return atx::core::Ok();
}

}  // namespace atx::engine::regime
```

- [ ] **Step 5: Run, verify all tests pass.** Expected: 4 PASS.

- [ ] **Step 6: Commit.**
```bash
git add atx-engine/include/atx/engine/regime/align.hpp atx-engine/src/regime/align.cpp atx-engine/tests/regime/regime_align_test.cpp
git commit -m "feat(regime): master axis + forward-fill + derived-series application"
```

---

## Task 4: Read API (`store`)

> Built before the loader so the loader's integration test can read back what it writes.

**Files:**
- Modify: `atx-engine/include/atx/engine/regime/store.hpp`
- Modify: `atx-engine/src/regime/store.cpp`
- Test: `atx-engine/tests/regime/regime_store_test.cpp`

**Interfaces:**
- Consumes: a sealed `.seg` written by `atx::tsdb` (fields = series names, instruments = `["MACRO"]`, times = master axis).
- Produces:
  - `class RegimeStore` with `static Result<RegimeStore> open(const std::string& seg_path)`, `std::span<const std::string> series_names() const`, `std::span<const atx::i64> date_axis() const`, `atx::f64 value(std::string_view series, atx::i64 t_nanos) const` (as-of `<=`; NaN if unknown series or before first axis date).

- [ ] **Step 1: Inspect the tsdb read surface.** Read `atx-tsdb/include/atx/tsdb/segment.hpp` (or `mapping.hpp` / `segment_reader.hpp`) to confirm how to memory-map a `.seg` and call the read accessors: the header type, `time_axis(base, header)`, `field_block_view(base, header, field)`, `cell_value(base, header, field, t, inst)`, and the field/symbol name tables. Use the same opener the engine uses (e.g. `atx::tsdb::SegmentReader` / `Mapping`). Record the exact spellings; the code below names them `atx::tsdb::SegmentReader` — adjust to the real type if it differs.

- [ ] **Step 2: Write the failing integration test** `atx-engine/tests/regime/regime_store_test.cpp`. It first writes a tiny segment with `atx::tsdb::SegmentBuilder` (two series, three dates, one instrument `MACRO`), then opens it via `RegimeStore`:

```cpp
#include <cmath>
#include <filesystem>
#include <string>
#include <vector>
#include <gtest/gtest.h>
#include "atx/core/types.hpp"
#include "atx/tsdb/builder.hpp"
#include "atx/engine/regime/store.hpp"

namespace atxtest_regime_store {
constexpr atx::i64 kDay = 86400LL * 1000000000LL;

[[nodiscard]] std::string write_fixture() {
  // fields: vix, move ; instrument: MACRO ; dates: 2,4,6 (in kDay units)
  std::vector<std::string> fields = {"vix", "move"};
  std::vector<std::string> syms = {"MACRO"};
  std::vector<atx::i64> axis = {2 * kDay, 4 * kDay, 6 * kDay};
  atx::tsdb::SegmentBuilder b(fields, syms, axis);
  // vix = 15, 25, 18 ; move = 80, 90, 100
  b.set(0, 2 * kDay, 0, 15.0); b.set(0, 4 * kDay, 0, 25.0); b.set(0, 6 * kDay, 0, 18.0);
  b.set(1, 2 * kDay, 0, 80.0); b.set(1, 4 * kDay, 0, 90.0); b.set(1, 6 * kDay, 0, 100.0);
  const std::string path =
      (std::filesystem::temp_directory_path() / "atx_regime_store_fixture.seg").string();
  const auto st = b.write(path, /*created_at_nanos=*/0);
  EXPECT_TRUE(st.has_value());
  return path;
}

using atx::engine::regime::RegimeStore;

TEST(RegimeStore, OpensAndExposesSeriesAndAxis) {
  const std::string path = write_fixture();
  auto s = RegimeStore::open(path);
  ASSERT_TRUE(s.has_value()) << (s ? "" : s.error().message());
  EXPECT_EQ(s.value().series_names().size(), 2u);
  EXPECT_EQ(s.value().date_axis().size(), 3u);
}

TEST(RegimeStore, AsOfLookup_ExactAndBetweenAndBefore) {
  const std::string path = write_fixture();
  auto s = RegimeStore::open(path).value();
  EXPECT_DOUBLE_EQ(s.value("vix", 4 * kDay), 25.0);        // exact date
  EXPECT_DOUBLE_EQ(s.value("vix", 5 * kDay), 25.0);        // between -> last <= 5
  EXPECT_DOUBLE_EQ(s.value("vix", 100 * kDay), 18.0);      // past last -> carry
  EXPECT_TRUE(std::isnan(s.value("vix", 1 * kDay)));       // before first axis date
  EXPECT_TRUE(std::isnan(s.value("nope", 4 * kDay)));      // unknown series
}
}  // namespace atxtest_regime_store
```

- [ ] **Step 3: Run, verify it fails** (`RegimeStore` undeclared).

- [ ] **Step 4: Implement `store.hpp`:**

```cpp
#pragma once

#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "atx/core/error.hpp"
#include "atx/core/types.hpp"

namespace atx::engine::regime {

// Read-only view over a sealed regime segment (one synthetic instrument
// "MACRO"; fields are series names; the time axis is the master date axis).
class RegimeStore {
public:
  [[nodiscard]] static atx::core::Result<RegimeStore> open(const std::string &seg_path);

  [[nodiscard]] std::span<const std::string> series_names() const noexcept { return names_; }
  [[nodiscard]] std::span<const atx::i64> date_axis() const noexcept { return axis_; }

  // As-of (<=) lookup: value of `series` at the latest axis date <= t_nanos.
  // NaN if `series` is unknown or t_nanos precedes the first axis date.
  [[nodiscard]] atx::f64 value(std::string_view series, atx::i64 t_nanos) const noexcept;

private:
  std::vector<std::string> names_;     // field/series names (field index order)
  std::vector<atx::i64> axis_;         // ascending master dates
  std::vector<std::vector<atx::f64>> cols_;  // [series][date] dense f64 (NaN-filled)
};

}  // namespace atx::engine::regime
```

- [ ] **Step 5: Implement `store.cpp`.** Materialize the segment's columns into `cols_` at open (the regime grid is tiny — a few series × a few thousand dates). Use the tsdb reader confirmed in Step 1:

```cpp
#include "atx/engine/regime/store.hpp"

#include <algorithm>
#include <limits>

#include "atx/tsdb/segment.hpp"        // adjust include to the real reader header
// If the engine opens segments via a reader/mapping wrapper, include that too.

namespace atx::engine::regime {

namespace {
constexpr atx::f64 kNaN = std::numeric_limits<atx::f64>::quiet_NaN();
}  // namespace

atx::core::Result<RegimeStore> RegimeStore::open(const std::string &seg_path) {
  // Open + mmap the sealed segment via the tsdb reader. The exact API is the one
  // confirmed in Task 4 Step 1; the shape below assumes:
  //   * a reader R that owns the mapping and exposes header() + base pointer,
  //   * free accessors time_axis(base,h), field name table, cell_value(base,h,field,t,inst).
  // Replace the three TODO-marked lines with the confirmed calls.
  RegimeStore store;

  // --- BEGIN tsdb-reader-specific section (fill from Step 1 findings) ---
  // auto reader = atx::tsdb::SegmentReader::open(seg_path);  // -> Result<...>
  // if (!reader.has_value()) return atx::core::Err(reader.error());
  // const auto &h = reader.value().header();
  // const atx::u8 *base = reader.value().base();
  // store.names_ : copy field names from the field directory.
  // store.axis_  : copy time_axis(base, h) into a vector<i64>.
  // for each field f, for each date index t: store.cols_[f][t] =
  //     cell_value(base, h, f, axis_[t], /*inst=*/0);
  // --- END tsdb-reader-specific section ---

  if (store.names_.empty() || store.axis_.empty()) {
    return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                          "RegimeStore::open: empty/invalid segment");
  }
  return atx::core::Ok(std::move(store));
}

atx::f64 RegimeStore::value(std::string_view series, atx::i64 t_nanos) const noexcept {
  atx::usize fi = static_cast<atx::usize>(-1);
  for (atx::usize i = 0; i < names_.size(); ++i) {
    if (names_[i] == series) { fi = i; break; }
  }
  if (fi == static_cast<atx::usize>(-1)) return kNaN;
  // largest axis index with axis_[idx] <= t_nanos
  const auto it = std::upper_bound(axis_.begin(), axis_.end(), t_nanos);
  if (it == axis_.begin()) return kNaN;  // before first date
  const atx::usize di = static_cast<atx::usize>((it - axis_.begin()) - 1);
  return cols_[fi][di];
}

}  // namespace atx::engine::regime
```

> The tsdb-reader-specific block is the only part to fill from Step 1. The mapping report (`atx-tsdb/include/atx/tsdb/segment.hpp` accessors: `time_axis`, `field_block_view`, `cell_value`, header) lists the exact functions; the `field_block_view(base,h,f)` span is the whole `[T][N]` block, so with N==1 the per-date values are contiguous — copy them directly into `cols_[f]` instead of per-cell `cell_value` if preferred.

- [ ] **Step 6: Run, verify both tests pass.**

- [ ] **Step 7: Commit.**
```bash
git add atx-engine/include/atx/engine/regime/store.hpp atx-engine/src/regime/store.cpp atx-engine/tests/regime/regime_store_test.cpp
git commit -m "feat(regime): RegimeStore sealed-segment reader with as-of lookup"
```

---

## Task 5: Loader (`loader`) + manifest + determinism

**Files:**
- Modify: `atx-engine/include/atx/engine/regime/loader.hpp`
- Modify: `atx-engine/src/regime/loader.cpp`
- Test: `atx-engine/tests/regime/regime_loader_test.cpp`

**Interfaces:**
- Consumes: `CsvFormat`/`parse_series_csv` (Task 2), `NamedSeries`/`build_master_axis`/`forward_fill`/`apply_derived` (Task 3), `parse_derived_spec`/`DerivedSpec` (Task 1), `atx::tsdb::LongColumns`/`build_from_long`, `RegimeStore` (Task 4, test only).
- Produces:
  - `struct SeriesSpec { std::string name; std::string file; CsvFormat format{CsvFormat::Fred}; std::string value_column; };`
  - `struct RegimeLoadConfig { std::string staging_dir; std::string out_path; std::vector<SeriesSpec> series; std::vector<std::string> derived; atx::i64 min_date_nanos{0}; atx::i64 created_at_nanos{0}; };`
  - `struct RegimeLoadStats { atx::i64 series_count; atx::i64 dates_written; atx::i64 first_date_nanos; atx::i64 last_date_nanos; };`
  - `load_regime_history(const RegimeLoadConfig&) -> atx::core::Result<RegimeLoadStats>`.

- [ ] **Step 1: Write the failing test** `atx-engine/tests/regime/regime_loader_test.cpp`. It writes two FRED CSVs to a temp staging dir, loads, then (a) reopens via `RegimeStore` and checks values + a derived series, and (b) loads a second time and asserts the two `.seg` files are byte-identical:

```cpp
#include <cmath>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>
#include <gtest/gtest.h>
#include "atx/engine/regime/loader.hpp"
#include "atx/engine/regime/store.hpp"

namespace atxtest_regime_loader {
namespace fs = std::filesystem;
constexpr atx::i64 kDay = 86400LL * 1000000000LL;

[[nodiscard]] std::string write_csv(const fs::path &dir, const std::string &name,
                                    const std::string &body) {
  const fs::path p = dir / name;
  std::ofstream(p, std::ios::binary) << body;
  return p.string();
}

[[nodiscard]] bool files_equal(const std::string &a, const std::string &b) {
  std::ifstream fa(a, std::ios::binary), fb(b, std::ios::binary);
  std::string sa((std::istreambuf_iterator<char>(fa)), {});
  std::string sb((std::istreambuf_iterator<char>(fb)), {});
  return sa == sb;
}

using namespace atx::engine::regime;

[[nodiscard]] RegimeLoadConfig make_cfg(const fs::path &dir, const std::string &out) {
  write_csv(dir, "dgs2.csv", "DATE,VALUE\n2020-01-02,1.0\n2020-01-06,1.5\n");
  write_csv(dir, "dgs10.csv", "DATE,VALUE\n2020-01-02,2.0\n2020-01-06,2.5\n");
  RegimeLoadConfig cfg;
  cfg.staging_dir = dir.string();
  cfg.out_path = out;
  cfg.series = {{"dgs2", "dgs2.csv", CsvFormat::Fred, "VALUE"},
                {"dgs10", "dgs10.csv", CsvFormat::Fred, "VALUE"}};
  cfg.derived = {"t10y2y = dgs10 - dgs2"};
  cfg.created_at_nanos = 12345;
  return cfg;
}

TEST(RegimeLoader, LoadsAndDerivesCorrectly) {
  const fs::path dir = fs::temp_directory_path() / "atx_regime_loader_a";
  fs::create_directories(dir);
  const std::string out = (dir / "regime.seg").string();
  auto st = load_regime_history(make_cfg(dir, out));
  ASSERT_TRUE(st.has_value()) << (st ? "" : st.error().message());
  EXPECT_EQ(st.value().series_count, 3);     // dgs2, dgs10, t10y2y
  EXPECT_EQ(st.value().dates_written, 2);

  auto store = RegimeStore::open(out).value();
  EXPECT_DOUBLE_EQ(store.value("dgs2", 2 * kDay + 86400LL * 1000000000LL * 0), 1.0);  // 2020-01-02
  EXPECT_DOUBLE_EQ(store.value("t10y2y", 1577923200000000000LL), 0.5);  // 2020-01-02: 2.0-1.0
  EXPECT_DOUBLE_EQ(store.value("t10y2y", 1578268800000000000LL), 1.0);  // 2020-01-06: 2.5-1.5
}

TEST(RegimeLoader, ByteIdenticalAcrossRuns) {
  const fs::path dir = fs::temp_directory_path() / "atx_regime_loader_b";
  fs::create_directories(dir);
  const std::string out1 = (dir / "r1.seg").string();
  const std::string out2 = (dir / "r2.seg").string();
  ASSERT_TRUE(load_regime_history(make_cfg(dir, out1)).has_value());
  ASSERT_TRUE(load_regime_history(make_cfg(dir, out2)).has_value());
  EXPECT_TRUE(files_equal(out1, out2));
}
}  // namespace atxtest_regime_loader
```

> The two epoch-nanos literals are the unix-nanos for 2020-01-02 and 2020-01-06 at UTC midnight (compute once with `date_to_nanos` while writing the test if you prefer — `date_to_nanos("2020-01-02").value()` etc. — and drop the magic numbers).

- [ ] **Step 2: Run, verify it fails** (loader symbols undeclared).

- [ ] **Step 3: Implement `loader.hpp`:**

```cpp
#pragma once

#include <string>
#include <vector>

#include "atx/core/error.hpp"
#include "atx/core/types.hpp"

#include "atx/engine/regime/source_csv.hpp"

namespace atx::engine::regime {

struct SeriesSpec {
  std::string name;          // canonical series name -> becomes a segment field
  std::string file;          // CSV filename relative to staging_dir
  CsvFormat format{CsvFormat::Fred};
  std::string value_column;  // header name of the numeric column
};

struct RegimeLoadConfig {
  std::string staging_dir;             // dir holding the staged CSVs
  std::string out_path;                // output .seg path
  std::vector<SeriesSpec> series;      // raw series to load
  std::vector<std::string> derived;    // derived defs, e.g. "t10y2y = dgs10 - dgs2"
  atx::i64 min_date_nanos{0};          // inclusive floor
  atx::i64 created_at_nanos{0};        // stamped into the segment header
};

struct RegimeLoadStats {
  atx::i64 series_count{};      // raw + derived
  atx::i64 dates_written{};     // master axis length
  atx::i64 first_date_nanos{};
  atx::i64 last_date_nanos{};
};

// Read staged CSVs -> align onto a master axis (forward-filled) -> derive
// composites -> pivot to a sealed segment at cfg.out_path + a JSON manifest at
// cfg.out_path + ".manifest.json". Deterministic: same staged snapshot ->
// byte-identical segment.
[[nodiscard]] atx::core::Result<RegimeLoadStats>
load_regime_history(const RegimeLoadConfig &cfg);

}  // namespace atx::engine::regime
```

- [ ] **Step 4: Implement `loader.cpp`:**

```cpp
#include "atx/engine/regime/loader.hpp"

#include <filesystem>
#include <fstream>

#include "atx/tsdb/load_parquet.hpp"   // LongColumns + build_from_long

#include "atx/engine/regime/align.hpp"
#include "atx/engine/regime/series.hpp"

namespace atx::engine::regime {

atx::core::Result<RegimeLoadStats> load_regime_history(const RegimeLoadConfig &cfg) {
  namespace fs = std::filesystem;

  // 1. Parse each raw series CSV (preserve config order for determinism).
  std::vector<NamedSeries> raw;
  raw.reserve(cfg.series.size());
  for (const SeriesSpec &spec : cfg.series) {
    const std::string path = (fs::path(cfg.staging_dir) / spec.file).string();
    ATX_TRY(auto obs, parse_series_csv(path, spec.format, spec.value_column));
    raw.push_back(NamedSeries{spec.name, std::move(obs)});
  }
  if (raw.empty()) {
    return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                          "load_regime_history: no series configured");
  }

  // 2. Master axis = sorted-unique union of observed dates >= floor.
  const std::vector<atx::i64> axis = build_master_axis(raw, cfg.min_date_nanos);
  if (axis.empty()) {
    return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                          "load_regime_history: master axis is empty (check min_date)");
  }

  // 3. Forward-fill each series onto the axis.
  std::vector<std::string> names;
  std::vector<std::vector<atx::f64>> cols;
  names.reserve(raw.size());
  cols.reserve(raw.size());
  for (const NamedSeries &s : raw) {
    names.push_back(s.name);
    cols.push_back(forward_fill(s.obs, axis));
  }

  // 4. Derived series (after alignment, so operands share the axis).
  for (const std::string &def : cfg.derived) {
    ATX_TRY(const DerivedSpec spec, parse_derived_spec(def));
    ATX_TRY_STATUS(apply_derived(names, cols, spec));  // see note below on the macro
  }

  // 5. Pivot to LongColumns: one synthetic instrument "MACRO"; row r = (axis[t],
  //    "MACRO", values[field][t]) for every (field, date). build_from_long
  //    dedups+sorts the time axis and writes the sealed segment.
  atx::tsdb::LongColumns lc;
  lc.field_names = names;
  const atx::usize T = axis.size();
  const atx::usize F = names.size();
  lc.times.reserve(F * T);
  lc.symbols.assign(F * T, "MACRO");
  lc.values.assign(1, {});  // build_from_long expects values[field][row]? -> see note
  // NOTE: LongColumns is row-oriented: times[r], symbols[r], values[field][r] all
  // share the SAME row count R. So build ONE row per (date) and F value vectors:
  lc.times.clear();
  lc.symbols.clear();
  lc.values.assign(F, {});
  for (atx::usize t = 0; t < T; ++t) {
    lc.times.push_back(axis[t]);
    lc.symbols.push_back("MACRO");
    for (atx::usize f = 0; f < F; ++f) {
      lc.values[f].push_back(cols[f][t]);
    }
  }

  ATX_TRY_STATUS(atx::tsdb::build_from_long(lc, cfg.out_path, cfg.created_at_nanos));

  // 6. Hand-written JSON manifest (no JSON lib dependency).
  {
    std::ofstream m(cfg.out_path + ".manifest.json", std::ios::binary);
    m << "{\n  \"dates\": " << T << ",\n  \"series\": [";
    for (atx::usize f = 0; f < F; ++f) {
      m << (f ? ", " : "") << "\"" << names[f] << "\"";
    }
    m << "],\n  \"first_date_nanos\": " << axis.front()
      << ",\n  \"last_date_nanos\": " << axis.back()
      << ",\n  \"created_at_nanos\": " << cfg.created_at_nanos << "\n}\n";
  }

  RegimeLoadStats stats;
  stats.series_count = static_cast<atx::i64>(F);
  stats.dates_written = static_cast<atx::i64>(T);
  stats.first_date_nanos = axis.front();
  stats.last_date_nanos = axis.back();
  return atx::core::Ok(stats);
}

}  // namespace atx::engine::regime
```

> **Macro note:** if the codebase has no `ATX_TRY_STATUS` for `Status` (no value), unwrap by hand: `{ auto _s = expr; if (!_s.has_value()) return atx::core::Err(_s.error()); }`. Confirm the project's `Status` unwrap idiom from existing sources (e.g. `src/data/orats_history.cpp`) and use that exact form. Delete the dead `lc.values.assign(1, {})` scaffolding line — it is replaced two lines later; it is shown only to flag the row-orientation pitfall.

- [ ] **Step 5: Clean up the loader** — remove the flagged dead `lc.values.assign(1, {})` line and the duplicate `lc.times`/`lc.symbols` initialization, leaving only the single row-per-date fill loop. Re-read the final `loader.cpp` to confirm it is coherent.

- [ ] **Step 6: Run, verify both loader tests pass** (values + byte-identity).

- [ ] **Step 7: Commit.**
```bash
git add atx-engine/include/atx/engine/regime/loader.hpp atx-engine/src/regime/loader.cpp atx-engine/tests/regime/regime_loader_test.cpp
git commit -m "feat(regime): offline-CSV loader -> sealed segment + manifest (deterministic)"
```

---

## Task 6: Panel augmenter (`with_regime_fields`)

**Files:**
- Modify: `atx-engine/include/atx/engine/regime/with_regime_fields.hpp`
- Modify: `atx-engine/src/regime/with_regime_fields.cpp`
- Test: `atx-engine/tests/regime/regime_with_fields_test.cpp`

**Interfaces:**
- Consumes: `RegimeStore` (Task 4); `atx::engine::alpha::Panel` (`Panel::create`), `kRegimePrefix` (Task 1).
- Produces:
  - `with_regime_fields(atx::usize dates, atx::usize instruments, std::span<const atx::i64> panel_dates, std::vector<std::string> field_names, std::vector<std::vector<atx::f64>> field_data, std::vector<std::uint8_t> universe, const RegimeStore& store, std::span<const std::string> requested_series) -> atx::core::Result<alpha::Panel>`.

- [ ] **Step 1: Write the failing test** `atx-engine/tests/regime/regime_with_fields_test.cpp`. Build a tiny regime segment (vix at dates 2,4,6), then augment a 3-date × 2-instrument panel whose dates are 2,5,6 (date 5 absent from the regime axis → as-of carries date-4 value):

```cpp
#include <cmath>
#include <filesystem>
#include <string>
#include <vector>
#include <gtest/gtest.h>
#include "atx/core/types.hpp"
#include "atx/tsdb/builder.hpp"
#include "atx/engine/alpha/panel.hpp"
#include "atx/engine/regime/store.hpp"
#include "atx/engine/regime/with_regime_fields.hpp"

namespace atxtest_regime_fields {
namespace fs = std::filesystem;
constexpr atx::i64 kDay = 86400LL * 1000000000LL;
using atx::engine::alpha::Panel;
using atx::engine::regime::RegimeStore;
using atx::engine::regime::with_regime_fields;

[[nodiscard]] std::string regime_seg() {
  std::vector<std::string> fields = {"vix"};
  std::vector<std::string> syms = {"MACRO"};
  std::vector<atx::i64> axis = {2 * kDay, 4 * kDay, 6 * kDay};
  atx::tsdb::SegmentBuilder b(fields, syms, axis);
  b.set(0, 2 * kDay, 0, 15.0); b.set(0, 4 * kDay, 0, 25.0); b.set(0, 6 * kDay, 0, 18.0);
  const std::string p = (fs::temp_directory_path() / "atx_regime_fields.seg").string();
  EXPECT_TRUE(b.write(p, 0).has_value());
  return p;
}

TEST(RegimeFields, BroadcastsAsOfAcrossInstruments) {
  auto store = RegimeStore::open(regime_seg()).value();
  const atx::usize dates = 3, inst = 2;
  std::vector<atx::i64> panel_dates = {2 * kDay, 5 * kDay, 6 * kDay};  // 5 absent in regime
  std::vector<std::string> names = {"close"};
  std::vector<std::vector<atx::f64>> data = {std::vector<atx::f64>(dates * inst, 1.0)};
  std::vector<std::string> req = {"vix"};
  auto p = with_regime_fields(dates, inst, panel_dates, names, data, {}, store, req);
  ASSERT_TRUE(p.has_value()) << (p ? "" : p.error().message());
  auto fid = p.value().field_id("regime_vix");
  ASSERT_TRUE(fid.has_value());
  const auto col = p.value().field_all(fid.value());
  // date 0 (=2): 15 for both instruments; date 1 (=5): as-of -> 25; date 2 (=6): 18
  EXPECT_DOUBLE_EQ(col[0 * inst + 0], 15.0);
  EXPECT_DOUBLE_EQ(col[0 * inst + 1], 15.0);
  EXPECT_DOUBLE_EQ(col[1 * inst + 0], 25.0);
  EXPECT_DOUBLE_EQ(col[2 * inst + 0], 18.0);
}

TEST(RegimeFields, OutOfUniverseCellIsNaN) {
  auto store = RegimeStore::open(regime_seg()).value();
  const atx::usize dates = 1, inst = 2;
  std::vector<atx::i64> panel_dates = {2 * kDay};
  std::vector<std::string> names = {"close"};
  std::vector<std::vector<atx::f64>> data = {{1.0, 1.0}};
  std::vector<std::uint8_t> universe = {1, 0};  // inst 1 out of universe
  auto p = with_regime_fields(dates, inst, panel_dates, names, data, universe, store, {"vix"});
  ASSERT_TRUE(p.has_value());
  auto col = p.value().field_all(p.value().field_id("regime_vix").value());
  EXPECT_DOUBLE_EQ(col[0], 15.0);
  EXPECT_TRUE(std::isnan(col[1]));
}

TEST(RegimeFields, NameCollisionIsError) {
  auto store = RegimeStore::open(regime_seg()).value();
  std::vector<std::string> names = {"close", "regime_vix"};  // already present
  std::vector<std::vector<atx::f64>> data = {{1.0}, {2.0}};
  auto p = with_regime_fields(1, 1, std::vector<atx::i64>{2 * kDay}, names, data, {}, store, {"vix"});
  EXPECT_FALSE(p.has_value());
}
}  // namespace atxtest_regime_fields
```

- [ ] **Step 2: Run, verify it fails.**

- [ ] **Step 3: Implement `with_regime_fields.hpp`:**

```cpp
#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "atx/core/error.hpp"
#include "atx/core/types.hpp"

#include "atx/engine/alpha/panel.hpp"
#include "atx/engine/regime/store.hpp"

namespace atx::engine::regime {

// Append one broadcast column per requested series to a panel's field set.
// For each requested series s and cell (date d, instrument i):
//   value = store.value(s, panel_dates[d])   if instrument i is in-universe at d
//         = NaN                               otherwise
// Field name = kRegimePrefix + s (e.g. "regime_vix"). A collision with an
// existing field name is Err(AlreadyExists). An unknown series yields an all-NaN
// column (store.value returns NaN). panel_dates.size() must equal `dates`.
[[nodiscard]] atx::core::Result<alpha::Panel>
with_regime_fields(atx::usize dates, atx::usize instruments,
                   std::span<const atx::i64> panel_dates, std::vector<std::string> field_names,
                   std::vector<std::vector<atx::f64>> field_data,
                   std::vector<std::uint8_t> universe, const RegimeStore &store,
                   std::span<const std::string> requested_series);

}  // namespace atx::engine::regime
```

- [ ] **Step 4: Implement `with_regime_fields.cpp`:**

```cpp
#include "atx/engine/regime/with_regime_fields.hpp"

#include <limits>
#include <string>

#include "atx/engine/regime/series.hpp"

namespace atx::engine::regime {

namespace {
constexpr atx::f64 kNaN = std::numeric_limits<atx::f64>::quiet_NaN();

[[nodiscard]] bool in_universe(const std::vector<std::uint8_t> &u, atx::usize idx) noexcept {
  return u.empty() || u[idx] != 0;  // empty == all in-universe (matches with_datafields)
}
[[nodiscard]] bool has_name(const std::vector<std::string> &names, const std::string &n) noexcept {
  for (const std::string &x : names) {
    if (x == n) return true;
  }
  return false;
}
}  // namespace

atx::core::Result<alpha::Panel>
with_regime_fields(atx::usize dates, atx::usize instruments,
                   std::span<const atx::i64> panel_dates, std::vector<std::string> field_names,
                   std::vector<std::vector<atx::f64>> field_data,
                   std::vector<std::uint8_t> universe, const RegimeStore &store,
                   std::span<const std::string> requested_series) {
  if (panel_dates.size() != dates) {
    return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                          "with_regime_fields: panel_dates length != dates");
  }
  const atx::usize cells = dates * instruments;
  for (const std::string &s : requested_series) {
    const std::string fname = std::string{kRegimePrefix} + s;
    if (has_name(field_names, fname)) {
      return atx::core::Err(atx::core::ErrorCode::AlreadyExists,
                            std::string{"with_regime_fields: field '"} + fname + "' already present");
    }
    std::vector<atx::f64> col(cells, kNaN);
    for (atx::usize d = 0; d < dates; ++d) {
      const atx::f64 v = store.value(s, panel_dates[d]);
      for (atx::usize i = 0; i < instruments; ++i) {
        const atx::usize idx = d * instruments + i;
        col[idx] = in_universe(universe, idx) ? v : kNaN;
      }
    }
    field_names.push_back(fname);
    field_data.push_back(std::move(col));
  }
  return alpha::Panel::create(dates, instruments, std::move(field_names), std::move(field_data),
                              std::move(universe));
}

}  // namespace atx::engine::regime
```

- [ ] **Step 5: Run, verify all three tests pass.**

- [ ] **Step 6: Commit.**
```bash
git add atx-engine/include/atx/engine/regime/with_regime_fields.hpp atx-engine/src/regime/with_regime_fields.cpp atx-engine/tests/regime/regime_with_fields_test.cpp
git commit -m "feat(regime): with_regime_fields panel augmenter (broadcast + as-of + universe NaN)"
```

---

## Task 7: End-to-end regime-masked alpha (no new VM ops)

**Files:**
- Test: `atx-engine/tests/regime/regime_e2e_mask_test.cpp`

**Interfaces:**
- Consumes: `with_regime_fields` (Task 6); the alpha eval harness (`parse_expr`/`analyze`/`compile`/`Engine::evaluate`).
- Produces: nothing — this task proves the load-bearing claim that `regime_vix < 20 ? <expr> : 0` masks the signal, using only existing DSL ops (ternary `Select` + `CmpLt`).

- [ ] **Step 1: Write the test** `atx-engine/tests/regime/regime_e2e_mask_test.cpp`. A 2-date × 1-instrument panel; vix = 15 on date 0 (low) and 25 on date 1 (high). The masked alpha `regime_vix < 20 ? close : 0` must equal `close` on date 0 and exactly `0` on date 1:

```cpp
#include <cmath>
#include <filesystem>
#include <string>
#include <vector>
#include <gtest/gtest.h>
#include "atx/core/types.hpp"
#include "atx/tsdb/builder.hpp"
#include "atx/engine/alpha/bytecode.hpp"
#include "atx/engine/alpha/panel.hpp"
#include "atx/engine/alpha/parser.hpp"
#include "atx/engine/alpha/registry.hpp"
#include "atx/engine/alpha/typecheck.hpp"
#include "atx/engine/alpha/vm.hpp"
#include "atx/engine/regime/store.hpp"
#include "atx/engine/regime/with_regime_fields.hpp"

namespace atxtest_regime_e2e {
namespace fs = std::filesystem;
constexpr atx::i64 kDay = 86400LL * 1000000000LL;
using namespace atx::engine::alpha;
using atx::engine::regime::RegimeStore;
using atx::engine::regime::with_regime_fields;

[[nodiscard]] const Library &lib() { static const Library l; return l; }

[[nodiscard]] std::vector<atx::f64> eval(std::string_view expr, const Panel &p) {
  auto ast = parse_expr(expr, lib());
  EXPECT_TRUE(ast.has_value()) << (ast ? "" : ast.error().message());
  auto ana = analyze(ast.value());
  EXPECT_TRUE(ana.has_value()) << (ana ? "" : ana.error().message());
  auto prog = compile(ast.value(), ana.value());
  EXPECT_TRUE(prog.has_value()) << (prog ? "" : prog.error().message());
  Engine e{p};
  auto out = e.evaluate(prog.value_or(Program{}));
  EXPECT_TRUE(out.has_value()) << (out ? "" : out.error().message());
  return (out.has_value() && !out.value().alphas.empty()) ? out.value().alphas[0].values
                                                          : std::vector<atx::f64>{};
}

[[nodiscard]] std::string regime_seg() {
  std::vector<std::string> f = {"vix"}; std::vector<std::string> s = {"MACRO"};
  std::vector<atx::i64> axis = {0 * kDay, 1 * kDay};
  atx::tsdb::SegmentBuilder b(f, s, axis);
  b.set(0, 0 * kDay, 0, 15.0);  // low vol
  b.set(0, 1 * kDay, 0, 25.0);  // high vol
  const std::string p = (fs::temp_directory_path() / "atx_regime_e2e.seg").string();
  EXPECT_TRUE(b.write(p, 0).has_value());
  return p;
}

TEST(RegimeE2E, TernaryMaskGatesSignalOnVix) {
  auto store = RegimeStore::open(regime_seg()).value();
  const atx::usize dates = 2, inst = 1;
  std::vector<atx::i64> panel_dates = {0 * kDay, 1 * kDay};
  std::vector<std::string> names = {"close"};
  std::vector<std::vector<atx::f64>> data = {{10.0, 20.0}};  // close = 10 (d0), 20 (d1)
  auto p = with_regime_fields(dates, inst, panel_dates, names, data, {}, store, {"vix"});
  ASSERT_TRUE(p.has_value()) << (p ? "" : p.error().message());

  const std::vector<atx::f64> v = eval("regime_vix < 20 ? close : 0", p.value());
  ASSERT_EQ(v.size(), dates * inst);
  EXPECT_DOUBLE_EQ(v[0], 10.0);  // vix 15 < 20 -> close
  EXPECT_DOUBLE_EQ(v[1], 0.0);   // vix 25 !< 20 -> 0 (gated off)
}
}  // namespace atxtest_regime_e2e
```

- [ ] **Step 2: Run, verify it passes.** If `parse_expr`/`compile` reject the ternary or comparison, STOP and re-check the DSL surface (`registry.hpp` confirms `OpCode::Select` via `?:` and `OpCode::CmpLt` via infix `<`); do not add new opcodes — fix the expression spelling.

- [ ] **Step 3: Commit.**
```bash
git add atx-engine/tests/regime/regime_e2e_mask_test.cpp
git commit -m "test(regime): e2e proof that 'regime_vix < 20 ? expr : 0' gates the signal"
```

---

## Task 8: atx-impl config flags + `regime` subcommand

**Files:**
- Modify: `atx-impl/src/config.hpp` (RunConfig fields; `kSubcommands`)
- Modify: `atx-impl/src/config.cpp` (`apply_flag_value` cases)
- Modify: the atx-impl dispatch/stage file that routes subcommands (the `load` handler's file — follow its pattern). Add `stage_regime`.
- Test: extend the existing atx-impl config test (find it under `atx-impl/tests/`); add cases for the new flags.

**Interfaces:**
- Consumes: `load_regime_history` / `RegimeLoadConfig` (Task 5).
- Produces: CLI surface `regime --staging-dir <d> --regime-out <p> [--min-date <YYYY-MM-DD>]`, and panel-stage flags `--regime-segs <path>`, `--regime-fields <csv>`.

- [ ] **Step 1: Read the config + dispatch pattern.** Read `atx-impl/src/config.cpp` `apply_flag_value` (string-flag cases like `zip`/`out`) and the dispatch routing that maps `cfg.subcommand` to a stage function (the `load` branch). Read how `stage_load` builds an `OratsLoadConfig` from `RunConfig` and calls the loader — `stage_regime` mirrors it.

- [ ] **Step 2: Write/extend the failing config test.** In the existing atx-impl config test, add:

```cpp
TEST(Config, ParsesRegimeFlags) {
  const char* argv[] = {"atx", "panel", "--regime-segs", "r.seg",
                        "--regime-fields", "vix,t10y2y"};
  auto cfg = atx::impl::parse_args(6, const_cast<char**>(argv));
  ASSERT_TRUE(cfg.has_value()) << (cfg ? "" : cfg.error().message());
  EXPECT_EQ(cfg.value().regime_segs, "r.seg");
  EXPECT_EQ(cfg.value().regime_fields, "vix,t10y2y");
}

TEST(Config, AcceptsRegimeSubcommand) {
  const char* argv[] = {"atx", "regime", "--staging-dir", "d", "--regime-out", "o.seg"};
  auto cfg = atx::impl::parse_args(6, const_cast<char**>(argv));
  ASSERT_TRUE(cfg.has_value()) << (cfg ? "" : cfg.error().message());
  EXPECT_EQ(cfg.value().subcommand, "regime");
  EXPECT_EQ(cfg.value().staging_dir, "d");
  EXPECT_EQ(cfg.value().regime_out, "o.seg");
}
```

- [ ] **Step 3: Run, verify it fails** (fields/subcommand absent).

- [ ] **Step 4: Add RunConfig fields** in `config.hpp` (after the `-- panel --` block):

```cpp
    // -- regime (macro/regime data) --
    std::string staging_dir;   // --staging-dir (regime subcommand: dir of staged CSVs)
    std::string regime_out;    // --regime-out  (regime subcommand: output .seg)
    std::string regime_segs;   // --regime-segs (panel stage: regime .seg to broadcast)
    std::string regime_fields; // --regime-fields (panel stage: comma-separated series)
```

And extend `kSubcommands` to include `"regime"` (bump the array size from 7 to 8):

```cpp
inline constexpr std::array<std::string_view, 8> kSubcommands = {
    "load", "panel", "discover", "combine", "optimize", "report", "run", "regime"};
```

- [ ] **Step 5: Add `apply_flag_value` cases** in `config.cpp` (alongside the other string flags):

```cpp
    if (flag == "staging-dir")   { cfg.staging_dir = value;   return atx::core::Ok(); }
    if (flag == "regime-out")    { cfg.regime_out = value;    return atx::core::Ok(); }
    if (flag == "regime-segs")   { cfg.regime_segs = value;   return atx::core::Ok(); }
    if (flag == "regime-fields") { cfg.regime_fields = value; return atx::core::Ok(); }
```

- [ ] **Step 6: Run, verify the config tests pass.**

- [ ] **Step 7: Add the `regime` stage handler.** Create `stage_regime` mirroring `stage_load`: build a `RegimeLoadConfig` from `RunConfig` (map `staging_dir`→`staging_dir`, `regime_out`→`out_path`, `min_date`→`min_date_nanos` via `regime::date_to_nanos`, and a DEFAULT series list = the v1 SeriesSpecs for the staged files), call `load_regime_history`, print stats (respect `cfg.quiet`). Wire it into the dispatch if-chain: `if (cfg.subcommand == "regime") return stage_regime(cfg);`. Link: the stage TU includes `atx/engine/regime/loader.hpp`; `atx-impl` already links `atx::engine`.

```cpp
// stage_regime.cpp (new), mirroring stage_load:
#include "atx/engine/regime/loader.hpp"
#include "atx/engine/regime/source_csv.hpp"
// ... in the handler:
atx::engine::regime::RegimeLoadConfig rc;
rc.staging_dir = cfg.staging_dir;
rc.out_path = cfg.regime_out;
if (!cfg.min_date.empty()) {
  ATX_TRY(rc.min_date_nanos, atx::engine::regime::date_to_nanos(cfg.min_date));
}
rc.series = {  // v1 default staged-file mapping (FRED unless noted)
  {"vix",    "vix.csv",    atx::engine::regime::CsvFormat::Fred, "VALUE"},
  {"vvix",   "vvix.csv",   atx::engine::regime::CsvFormat::Cboe, "CLOSE"},
  {"move",   "move.csv",   atx::engine::regime::CsvFormat::Yahoo, "Adj Close"},
  {"dgs2",   "dgs2.csv",   atx::engine::regime::CsvFormat::Fred, "VALUE"},
  {"dgs10",  "dgs10.csv",  atx::engine::regime::CsvFormat::Fred, "VALUE"},
  {"hy_oas", "hy_oas.csv", atx::engine::regime::CsvFormat::Fred, "VALUE"},
  {"ig_oas", "ig_oas.csv", atx::engine::regime::CsvFormat::Fred, "VALUE"},
  {"nfci",   "nfci.csv",   atx::engine::regime::CsvFormat::Fred, "VALUE"},
};
rc.derived = {"t10y2y = dgs10 - dgs2"};
ATX_TRY(const auto stats, atx::engine::regime::load_regime_history(rc));
// print stats unless cfg.quiet
```

Register the new `stage_regime.cpp` in `atx-impl/CMakeLists.txt` (follow how `stage_load` is listed).

- [ ] **Step 8: Build atx-impl; run a manual smoke** with a couple of hand-written CSVs in a temp staging dir:
Run: `atx-impl regime --staging-dir <tmp> --regime-out <tmp>/regime.seg`
Expected: writes `regime.seg` + `regime.seg.manifest.json`; prints series/date counts.

- [ ] **Step 9: Commit.**
```bash
git add atx-impl/src/config.hpp atx-impl/src/config.cpp atx-impl/src/stage_regime.cpp atx-impl/CMakeLists.txt atx-impl/tests/
git commit -m "feat(regime): atx-impl 'regime' subcommand + panel-stage regime flags"
```

---

## Task 9: Wire regime broadcast into the panel stage

**Files:**
- Modify: the panel-assembly path that calls `price_to_panel` (the `build_real_panel` orchestrator — `atx-engine/src/data/real_panel.cpp`, and/or the atx-impl `stage_panel`).
- Test: extend the panel-stage test (or add `atx-engine/tests/data/real_panel_regime_test.cpp`) proving regime fields appear when `--regime-segs` is set and are ABSENT (digest unchanged) when it is not.

**Interfaces:**
- Consumes: `RegimeStore` + `with_regime_fields` (Tasks 4, 6); the panel date axis (the assembled panel's `times`/date axis); `cfg.regime_segs` / `cfg.regime_fields` (Task 8).
- Produces: an augmented `alpha::Panel` carrying `regime_*` columns when regime is enabled.

- [ ] **Step 1: Read `build_real_panel`.** Read `atx-engine/src/data/real_panel.cpp` to find (a) where the `alpha::Panel` is finalized after `price_to_panel`/`with_datafields`, (b) how the panel's date axis (unix-nanos per date row) is available, and (c) where the digest is pinned. The regime step goes AFTER `with_datafields` and BEFORE the digest pin, and only runs when a regime segment path is supplied.

- [ ] **Step 2: Write the failing test.** Build a tiny price panel + a regime segment; assemble with regime enabled and assert `field_id("regime_vix")` resolves; assemble with regime disabled and assert it does not (and that the field list/digest matches the pre-regime baseline). Use the same builder helpers as Task 6. (Mirror the existing real_panel test's construction; if a full `build_real_panel` fixture is heavy, test the smaller seam: a helper `augment_with_regime(panel, panel_dates, store, fields)` that wraps `with_regime_fields` using the panel's own field arrays.)

- [ ] **Step 3: Implement the augmentation seam.** Add a thin helper (in `real_panel.cpp` or a small `regime_panel` adapter) that, given the finalized panel's `dates`, `instruments`, date axis, field names/data, universe, opens the `RegimeStore` and calls `with_regime_fields` with the requested series; thread `regime_segs` + parsed `regime_fields` (comma-split) from config to it. When `regime_segs` is empty, skip entirely (return the panel unchanged).

- [ ] **Step 4: Run, verify the enabled/disabled tests pass.**

- [ ] **Step 5: Confirm no-regression.** Run the existing real_panel / panel-stage digest test with no regime config; confirm the digest is byte-identical to the pre-change baseline.
Run: the data/real_panel test group.
Expected: PASS, digest unchanged.

- [ ] **Step 6: Commit.**
```bash
git add atx-engine/src/data/real_panel.cpp atx-engine/tests/data/
git commit -m "feat(regime): broadcast regime fields into build_real_panel when --regime-segs set"
```

---

## Task 10: Offline staging script + README field dictionary + seed exprs

**Files:**
- Create: `scripts/regime/fetch_regime.py`
- Modify: `atx-engine/include/atx/engine/regime/README.md`
- Modify (optional): the discover-stage seed list / a documented example, adding a regime-masked seed expr.

**Interfaces:** none (tooling + docs).

- [ ] **Step 1: Write `scripts/regime/fetch_regime.py`.** A standalone (non-build) script that downloads the v1 series to a staging dir using only the Python stdlib (`urllib.request`) — no third-party deps. FRED CSV endpoint pattern: `https://fred.stlouisfed.org/graph/fredgraph.csv?id=<SERIES_ID>`. Map: `vix→VIXCLS`, `dgs2→DGS2`, `dgs10→DGS10`, `hy_oas→BAMLH0A0HYM2`, `ig_oas→BAMLC0A0CM`, `nfci→NFCI`. Write each to `<staging>/<name>.csv` with the original header. For `vvix`/`move` (CBOE/Yahoo), document the manual/URL source in a comment if no stable stdlib-fetchable endpoint exists. Print one line per series fetched. Include a top docstring: "Offline staging for the atx regime loader. Run before `atx-impl regime`."

```python
#!/usr/bin/env python3
"""Offline staging for the atx regime loader.

Downloads public macro series to a staging dir as CSVs that
`atx-impl regime --staging-dir <dir>` ingests. Stdlib only; no deps.
The staged snapshot is the reproducible input (re-running the loader on the
same snapshot yields a byte-identical .seg).
"""
import sys, urllib.request, pathlib

FRED = {
    "vix": "VIXCLS", "dgs2": "DGS2", "dgs10": "DGS10",
    "hy_oas": "BAMLH0A0HYM2", "ig_oas": "BAMLC0A0CM", "nfci": "NFCI",
}

def fetch_fred(series_id: str) -> bytes:
    url = f"https://fred.stlouisfed.org/graph/fredgraph.csv?id={series_id}"
    with urllib.request.urlopen(url, timeout=60) as r:  # noqa: S310
        return r.read()

def main(out_dir: str) -> int:
    out = pathlib.Path(out_dir); out.mkdir(parents=True, exist_ok=True)
    for name, sid in FRED.items():
        (out / f"{name}.csv").write_bytes(fetch_fred(sid))
        print(f"staged {name} <- FRED {sid}")
    print("NOTE: vvix (CBOE) and move (Yahoo/ICE) must be staged manually; "
          "see regime/README.md for source URLs.")
    return 0

if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1] if len(sys.argv) > 1 else "data/regime_staging"))
```

- [ ] **Step 2: Write `README.md`** — the field dictionary (every `regime_<series>` name, its meaning, and its public source/FRED id), the forward-fill + as-of semantics, the `regime_vix < 20 ? <expr> : 0` usage example, and a RUNBOOK snippet: stage (`python scripts/regime/fetch_regime.py data/regime_staging`) → load (`atx-impl regime --staging-dir data/regime_staging --regime-out data/regime.seg`) → use (`atx-impl panel ... --regime-segs data/regime.seg --regime-fields vix,t10y2y`).

- [ ] **Step 3 (optional): Add a regime-masked seed expr** to the documented discover seed list so search starts with a regime-conditional example, e.g. `regime_vix < 20 ? ts_zscore(-ts_delta(close, 5), 20) : 0`. Only do this where seed exprs are configured (a config/docs list), not by hardcoding into engine source.

- [ ] **Step 4: Commit.**
```bash
git add scripts/regime/fetch_regime.py atx-engine/include/atx/engine/regime/README.md
git commit -m "docs(regime): offline staging script + field dictionary + RUNBOOK"
```

---

## Self-Review

**1. Spec coverage:**
- §5.1 offline staging → Task 10 (script) + Task 8 (subcommand). ✓
- §5.2 source adapters → Task 2. ✓
- §5.3 derived series → Task 1 (parse) + Task 3 (apply) + Task 5 (wire). ✓
- §5.4 loader → Task 5. ✓
- §5.5 forward-fill → Task 3. ✓
- §5.6 read API → Task 4. ✓
- §5.7 determinism → Task 5 (byte-identity test). ✓
- §6.1 with_regime_fields → Task 6. ✓
- §6.2 pipeline wiring + config flags → Tasks 8, 9. ✓
- §6.3 DSL usage (no new ops) → Task 7. ✓
- §6.4 no-regression digest → Task 9 Step 5. ✓
- §8 testing → each task ships its tests; e2e mask → Task 7. ✓
- §9 phasing → Tasks 1–5 (Phase 1), 6–10 (Phase 2). ✓

**2. Placeholder scan:** The only deferred detail is the tsdb reader API in Task 4 Step 5, explicitly bounded to a Step-1 inspection of `segment.hpp` with the exact accessor names to substitute; the surrounding logic (as-of lookup, materialization) is complete. The atx-impl dispatch routing (Task 8 Step 7) references the existing `stage_load` pattern with concrete field mappings shown. No "TBD"/"add error handling"/"similar to" placeholders elsewhere.

**3. Type consistency:** `RegimeStore::value(string_view, i64)`, `with_regime_fields(...)`, `RegimeLoadConfig`/`RegimeLoadStats`, `NamedSeries`, `DerivedSpec`, `CsvFormat`, `parse_series_content`/`parse_series_csv`, `date_to_nanos`, `build_master_axis`/`forward_fill`/`apply_derived`, `load_regime_history` — names/signatures are identical across the tasks that define and consume them. `LongColumns` is row-oriented (times[r]/symbols[r]/values[field][r]) — Task 5 fills it that way and flags the pitfall.

## Open implementation notes (resolve during execution, not blockers)
- **tsdb reader type** (Task 4): confirm whether the engine opens a sealed `.seg` via `atx::tsdb::SegmentReader`, a `Mapping`, or the `ShmBarFeed` path; `atx-engine/src/data/segment_panel.cpp` and `shm_bar_feed.cpp` are existing consumers to copy.
- **`Status` unwrap idiom** (Task 5): use the project's existing macro (grep `ATX_TRY` usage on a `Status`-returning call in `src/data/orats_history.cpp`); if none, unwrap by hand as shown.
- **atx-impl test location** (Task 8): confirm the config test file path under `atx-impl/tests/` and its registration.
