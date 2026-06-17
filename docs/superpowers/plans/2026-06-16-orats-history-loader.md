# ORATS Single-File History Loader → E2E US-Equity Smoke (p3 S3) — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A pure high-performance C++ loader that streams `tbltickerhistory3_10y.zip` into an on-disk atx-tsdb per-date `.seg` partition keyed on `securityID`, wired into the atx-engine data interface (a new multi-segment Panel attach) with corporate actions applied via `cumulReturnFactor`, proven by a full end-to-end smoke of the unchanged p2 pipeline on the US equity universe.

**Architecture:** Three layers, bottom-up. (1) atx-core gains a streaming single-entry zip reader (`io::ZipEntryReader`) wrapping the already-vendored miniz, so the 11 GB plaintext is never fully materialized. (2) atx-engine `data::load_orats_history` parses the projected TSV columns and writes one sealed `.seg` per trading date via `atx::tsdb::build_from_long`. (3) atx-engine `alpha::attach_multi_segment_panel` unions the per-date segments into one owned `alpha::Panel`, and `data::build_history_panel` mirrors `build_real_panel` to apply the `cumulReturnFactor` total-return adjustment + the S1 universe screen + a golden digest, yielding a drop-in Panel the unchanged `factory::RobustResearchDriver` consumes.

**Tech Stack:** C++ (repo standard — do not bump the language version), miniz (vendored, `atx_miniz`), atx-core (`io`, `error`, `from_chars`), atx-tsdb (`build_from_long`, `SegmentReader`), atx-engine (`alpha::Panel`, `data::adjust_total_return`, `data::build_universe`, `data::DatasetCatalog`, `factory::RobustResearchDriver`), GoogleTest.

## Global Constraints

- **Canonical instrument key = `securityID`** (stringified). `ticker_tk` / `todayTicker` are NOT segment fields — they go to a side-car symbology manifest. (securityID is stable across renames/delistings — the survivorship-correct identity.)
- **Date floor:** ingest only rows with `tradingDate ≥ 2020-01-01` (the user constraint). The floor is passed as midnight-UTC unix-nanos.
- **Curated field set (16 segment fields, fixed canonical order):** `open, high, low, close, closePr, closeUnadjPr, volume, shares, returnFactor, totalReturn, cumulReturnFactor, gics, earnFlag, atmCenI_21d, atmCenI_126d, nEarnCnt_5d`. Forward-looking columns (`hEMove, iEMove, shD1, lnD1, wkD1, qtrD1, *EMove`) are **excluded** (leak risk).
- **Corporate-action model:** canonical adjusted `close` = `close × cumulReturnFactor` (`close` = col 9, the as-traded contemporaneous close; NOT `closeUnadjPr`, which is the *lagged prior-day close*). Implemented by reusing `adjust_total_return(close, cumulReturnFactor, zeros)` whose `total_return_index` equals `close × cumulReturnFactor` exactly. Raw `close` retained as `raw_close`.
- **NaN policy (never fabricate):** missing numeric → NaN, never 0; absent sector → `kNoSectorCode (-1)`, never 0; a NaN raw close or NaN factor is a gap (NaN), never zero-filled.
- **Determinism:** the assembled Panel's field order is load-bearing (the digest hashes fields in order). Two builds of identical inputs produce a byte-identical digest.
- **No new third-party dependency** — miniz is already vendored. miniz.h stays confined to `.cpp` TUs (pImpl in the atx-core header).
- **Output partition `data/orats_history_1d/` is gitignored** (`/data/` is in `.gitignore`). Committed tests use tiny fixtures generated at runtime; the 11 GB build + full-universe smoke are an operator data-build recorded in the ledger.
- **Governance:** `.agents/cpp/agent.md` (C++ rules) + `atx-engine/plans/docs/implementation-quality.md` are mandatory for every code unit. Tests are gtest, auto-globbed (`*_test.cpp` — no CMake edit needed for engine tests).
- **Git discipline:** all S3 work in worktree `atx-wt/p3-s3` (branch `feat/p3-s3-orats-loader`, base `main`). Marker + per-unit + close commits; `--no-ff` merge at close; **no push** unless the user asks.

---

## Task 0 (S3-0): Marker — open worktree, ledger, recon

**Files:**
- Create: `atx-engine/plans/p3-impl/sprint-3-progress.md`
- Modify: `atx-engine/plans/p3-impl/ROADMAP.md` (add the S3 row to the sprint arc)
- Commit (move): `docs/superpowers/specs/2026-06-16-orats-history-loader-design.md` + `docs/superpowers/plans/2026-06-16-orats-history-loader.md` (already on disk; first commit in the worktree)

**Interfaces:**
- Produces: the worktree, the sprint ledger, and the recorded decisions every later task assumes (securityID key, TRI = `close × cumulReturnFactor`, post-2020 filter, 16-field set, per-date partition + multi-attach).

- [ ] **Step 1: Create the worktree**

Use the `superpowers:using-git-worktrees` skill. Equivalent native command:
```bash
git worktree add ../atx-wt/p3-s3 -b feat/p3-s3-orats-loader main
```
Run all subsequent steps inside `../atx-wt/p3-s3`.

- [ ] **Step 2: Write the sprint ledger**

Create `atx-engine/plans/p3-impl/sprint-3-progress.md` with the S3 unit table (S3-0…S3-close), the four locked decisions, and a "Recon" section recording the verified facts: the file is `tbltickerhistory3_10y.txt` (11 GB, 71 cols, TSV, date-major), miniz streaming API is `mz_zip_reader_extract_iter_*`, the segment format is `atx-tsdb` v2 (`build_from_long`), and `attach_segment_panel` maps a single segment (the seam being extended). Mirror the structure of `sprint-1-progress.md`.

- [ ] **Step 3: Add the S3 row to the ROADMAP sprint arc**

In `atx-engine/plans/p3-impl/ROADMAP.md`, add an S3 box to the "v4 sprint arc" diagram and a one-line entry noting S3 (single-file ORATS loader) feeds S2, and annotate the S2 outline that it may consume the S3 partition. Keep edits minimal.

- [ ] **Step 4: Commit the marker (incl. spec + plan docs)**

```bash
git add docs/superpowers/specs/2026-06-16-orats-history-loader-design.md \
        docs/superpowers/plans/2026-06-16-orats-history-loader.md \
        atx-engine/plans/p3-impl/sprint-3-progress.md \
        atx-engine/plans/p3-impl/ROADMAP.md
git commit -m "docs(p3-s3): open S3 ORATS history loader sprint — spec, plan, ledger, roadmap"
```

---

## Task 1 (S3-1): atx-core `io::ZipEntryReader` — streaming single-entry inflate

**Files:**
- Create: `atx-core/include/atx/core/io/zip_reader.hpp`
- Create: `atx-core/src/io/zip_reader.cpp`
- Create: `atx-core/tests/zip_reader_test.cpp`
- Modify: `atx-core/CMakeLists.txt` (add `src/io/zip_reader.cpp` to the `atx-core` source list — `atx_miniz` is already linked PRIVATE, line ~106)
- Modify: `atx-core/tests/CMakeLists.txt` only if its test sources are an explicit list (check; if it globs `*_test.cpp`, no edit needed)

**Interfaces:**
- Produces:
  ```cpp
  namespace atx::core::io {
  class ZipEntryReader {
  public:
    // Open `zip_path`; select the first entry whose name CONTAINS `entry_name_substr`
    // (empty => the first non-directory entry). Err(IoError) if the zip cannot be
    // opened; Err(NotFound) if no matching entry; Err(ParseError) on a corrupt central
    // directory or a failed iterator init.
    [[nodiscard]] static atx::core::Result<ZipEntryReader>
    open(std::string_view zip_path, std::string_view entry_name_substr = {});

    ZipEntryReader(ZipEntryReader &&) noexcept;
    ZipEntryReader &operator=(ZipEntryReader &&) noexcept;
    ZipEntryReader(const ZipEntryReader &) = delete;
    ZipEntryReader &operator=(const ZipEntryReader &) = delete;
    ~ZipEntryReader();

    // Fill `dst` with the next decompressed bytes. Returns the count written
    // (0 == end of entry). Err(ParseError) on an inflate/CRC error.
    [[nodiscard]] atx::core::Result<atx::usize> read(std::span<char> dst);

    [[nodiscard]] atx::u64 uncompressed_size() const noexcept;
    [[nodiscard]] std::string_view entry_name() const noexcept;

  private:
    struct Impl;                       // owns mz_zip_archive + iter state (miniz confined here)
    explicit ZipEntryReader(std::unique_ptr<Impl> p) noexcept;
    std::unique_ptr<Impl> p_;
  };
  } // namespace atx::core::io
  ```
- Consumes: miniz (`mz_zip_reader_init_file`, `mz_zip_reader_get_num_files`, `mz_zip_reader_file_stat`, `mz_zip_reader_extract_iter_new/_read/_free`, `mz_zip_reader_end`); `atx::core::{Result, Ok, Err, ErrorCode}`.

- [ ] **Step 1: Write the failing test**

Create `atx-core/tests/zip_reader_test.cpp`. It builds a tiny zip at runtime with the miniz **writer** (so no binary fixture is committed), then reads it back through `ZipEntryReader` in small chunks and reassembles:

```cpp
#include <array>
#include <filesystem>
#include <string>
#include <vector>

#include <gtest/gtest.h>
#include <miniz.h>

#include "atx/core/io/zip_reader.hpp"

namespace {
namespace fs = std::filesystem;
using atx::core::io::ZipEntryReader;

// Write a one-entry zip with known content to a temp path; return that path.
std::string make_zip(const std::string &entry_name, const std::string &content) {
  const fs::path p = fs::temp_directory_path() / "atx_zip_reader_test.zip";
  fs::remove(p);
  mz_zip_archive zip{};
  EXPECT_TRUE(mz_zip_writer_init_file(&zip, p.string().c_str(), 0));
  EXPECT_TRUE(mz_zip_writer_add_mem(&zip, entry_name.c_str(), content.data(), content.size(),
                                    MZ_BEST_SPEED));
  EXPECT_TRUE(mz_zip_writer_finalize_archive(&zip));
  EXPECT_TRUE(mz_zip_writer_end(&zip));
  return p.string();
}

std::string drain(ZipEntryReader &r, atx::usize chunk) {
  std::string out;
  std::vector<char> buf(chunk);
  for (;;) {
    auto n = r.read(std::span<char>(buf.data(), buf.size()));
    ASSERT_TRUE(n.has_value()) << n.error().to_string();
    if (*n == 0) break;
    out.append(buf.data(), *n);
  }
  return out;
}
} // namespace

TEST(IoZipReader, StreamsEntryContentInSmallChunks) {
  // Content larger than the chunk so the streaming loop iterates several times.
  std::string content;
  for (int i = 0; i < 5000; ++i) content += "line" + std::to_string(i) + "\n";
  const std::string zip = make_zip("history.txt", content);

  auto r = ZipEntryReader::open(zip, "history");
  ASSERT_TRUE(r.has_value()) << r.error().to_string();
  EXPECT_EQ(r->uncompressed_size(), content.size());
  EXPECT_NE(r->entry_name().find("history"), std::string_view::npos);

  const std::string got = drain(*r, /*chunk=*/64);
  EXPECT_EQ(got, content);
}

TEST(IoZipReader, MissingEntrySubstringIsNotFound) {
  const std::string zip = make_zip("history.txt", "abc");
  auto r = ZipEntryReader::open(zip, "does-not-exist");
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error().code(), atx::core::ErrorCode::NotFound);
}
```

- [ ] **Step 2: Run the test to verify it fails**

Configure + build the atx-core test target, then run:
```bash
ctest --test-dir build -R IoZipReader -V
```
Expected: FAIL to compile — `atx/core/io/zip_reader.hpp` does not exist.

- [ ] **Step 3: Write the header**

Create `atx-core/include/atx/core/io/zip_reader.hpp` with the exact `ZipEntryReader` declaration from the Interfaces block above. Includes: `<memory>`, `<span>`, `<string>`, `<string_view>`, `"atx/core/error.hpp"`, `"atx/core/types.hpp"`. Do **not** include `miniz.h` here (it stays in the `.cpp`).

- [ ] **Step 4: Write the implementation**

Create `atx-core/src/io/zip_reader.cpp`:

```cpp
#include "atx/core/io/zip_reader.hpp"

#include <cstring>

#include <miniz.h>

namespace atx::core::io {

struct ZipEntryReader::Impl {
  mz_zip_archive zip{};
  mz_zip_reader_extract_iter_state *iter{nullptr};
  mz_zip_archive_file_stat stat{};
  bool zip_open{false};

  ~Impl() {
    if (iter != nullptr) {
      mz_zip_reader_extract_iter_free(iter);
    }
    if (zip_open) {
      mz_zip_reader_end(&zip);
    }
  }
};

ZipEntryReader::ZipEntryReader(std::unique_ptr<Impl> p) noexcept : p_{std::move(p)} {}
ZipEntryReader::ZipEntryReader(ZipEntryReader &&) noexcept = default;
ZipEntryReader &ZipEntryReader::operator=(ZipEntryReader &&) noexcept = default;
ZipEntryReader::~ZipEntryReader() = default;

atx::core::Result<ZipEntryReader> ZipEntryReader::open(std::string_view zip_path,
                                                       std::string_view entry_name_substr) {
  auto impl = std::make_unique<Impl>();
  if (!mz_zip_reader_init_file(&impl->zip, std::string{zip_path}.c_str(), 0)) {
    return atx::core::Err(atx::core::ErrorCode::IoError,
                          std::string{"ZipEntryReader: cannot open "} + std::string{zip_path});
  }
  impl->zip_open = true;

  const mz_uint count = mz_zip_reader_get_num_files(&impl->zip);
  mz_uint chosen = count; // sentinel == not found
  for (mz_uint i = 0; i < count; ++i) {
    mz_zip_archive_file_stat st{};
    if (!mz_zip_reader_file_stat(&impl->zip, i, &st)) {
      continue;
    }
    if (st.m_is_directory != 0) {
      continue;
    }
    const std::string_view name{static_cast<const char *>(st.m_filename)};
    if (entry_name_substr.empty() || name.find(entry_name_substr) != std::string_view::npos) {
      chosen = i;
      impl->stat = st;
      break;
    }
  }
  if (chosen == count) {
    return atx::core::Err(atx::core::ErrorCode::NotFound,
                          std::string{"ZipEntryReader: no entry matching '"} +
                              std::string{entry_name_substr} + "'");
  }

  impl->iter = mz_zip_reader_extract_iter_new(&impl->zip, chosen, 0);
  if (impl->iter == nullptr) {
    return atx::core::Err(atx::core::ErrorCode::ParseError,
                          "ZipEntryReader: failed to start extraction iterator");
  }
  return atx::core::Ok(ZipEntryReader{std::move(impl)});
}

atx::core::Result<atx::usize> ZipEntryReader::read(std::span<char> dst) {
  const size_t got =
      mz_zip_reader_extract_iter_read(p_->iter, dst.data(), dst.size());
  // miniz signals an inflate error by setting iter->status < 0; a short read at EOF
  // simply returns fewer bytes, and a subsequent call returns 0.
  if (p_->iter->status < 0) {
    return atx::core::Err(atx::core::ErrorCode::ParseError,
                          "ZipEntryReader: inflate error mid-stream");
  }
  return atx::core::Ok(static_cast<atx::usize>(got));
}

atx::u64 ZipEntryReader::uncompressed_size() const noexcept {
  return static_cast<atx::u64>(p_->stat.m_uncomp_size);
}

std::string_view ZipEntryReader::entry_name() const noexcept {
  return std::string_view{static_cast<const char *>(p_->stat.m_filename)};
}

} // namespace atx::core::io
```

- [ ] **Step 5: Wire CMake**

In `atx-core/CMakeLists.txt`, add `src/io/zip_reader.cpp` to the `atx-core` library source list (alongside the other `src/io/*.cpp`). `atx_miniz` is already linked PRIVATE to `atx-core`, so no link edit is needed. If `atx-core/tests/CMakeLists.txt` uses an explicit test-source list (not a glob), add `zip_reader_test.cpp`; if it globs `*_test.cpp`, no edit is needed. The test target must already link `atx_miniz` (it uses `mz_zip_writer_*`) — if not, add `atx_miniz` to the atx-core test target's `target_link_libraries`.

- [ ] **Step 6: Run the test to verify it passes**

```bash
ctest --test-dir build -R IoZipReader -V
```
Expected: PASS (both tests).

- [ ] **Step 7: Commit**

```bash
git add atx-core/include/atx/core/io/zip_reader.hpp atx-core/src/io/zip_reader.cpp \
        atx-core/tests/zip_reader_test.cpp atx-core/CMakeLists.txt atx-core/tests/CMakeLists.txt
git commit -m "feat(p3-s3): atx-core io::ZipEntryReader — streaming single-entry zip inflate (S3-1)"
```

---

## Task 2 (S3-2): atx-engine `data::load_orats_history` — TSV → per-date `.seg` partition

**Files:**
- Create: `atx-engine/include/atx/engine/data/orats_history.hpp`
- Create: `atx-engine/src/data/orats_history.cpp`
- Create: `atx-engine/tests/data_orats_history_test.cpp`
- Modify: `atx-engine/CMakeLists.txt` (add `src/data/orats_history.cpp` to the `atx-engine` source list, after `src/data/universe.cpp` ~line 70)

**Interfaces:**
- Produces:
  ```cpp
  namespace atx::engine::data {

  // The 16 segment field names, in canonical (digest-stable) order.
  inline constexpr std::array<std::string_view, 16> kOratsFields = {
      "open", "high", "low", "close", "closePr", "closeUnadjPr", "volume", "shares",
      "returnFactor", "totalReturn", "cumulReturnFactor", "gics", "earnFlag",
      "atmCenI_21d", "atmCenI_126d", "nEarnCnt_5d"};

  struct OratsLoadConfig {
    std::string zip_path;       // .../tbltickerhistory3_10y.zip
    std::string out_dir;        // data/orats_history_1d (created if absent)
    atx::i64 min_date_nanos;    // inclusive floor; rows with tradingDate < this are dropped
    atx::i64 created_at_nanos;  // stamped into every segment header (provenance)
  };

  struct OratsLoadStats {
    atx::i64 rows_read{};          // data rows seen (excludes header)
    atx::i64 rows_kept{};          // rows ≥ min_date with a parseable date+securityID
    atx::i64 rows_filtered{};      // rows dropped by the date floor
    atx::i64 rows_malformed{};     // rows dropped for a bad date / missing securityID
    atx::i64 dates_written{};      // .seg files written
    atx::i64 distinct_securities{}; // unique securityIDs across kept rows
  };

  // Stream the zip, project kOratsFields, filter by date, and write one sealed
  // `<out_dir>/YYYY-MM-DD.seg` per trading date (symbol name = securityID) plus
  // `<out_dir>/_symbology.parquet` (securityID, ticker_tk, todayTicker) and
  // `<out_dir>/_manifest.json`. The input MUST be date-major (non-decreasing
  // tradingDate); a date regression fails closed with Err(InvalidArgument).
  [[nodiscard]] atx::core::Result<OratsLoadStats> load_orats_history(const OratsLoadConfig &cfg);

  namespace detail {
  // "YYYY-MM-DD" -> midnight-UTC unix nanos; std::nullopt on a malformed date.
  [[nodiscard]] std::optional<atx::i64> date_to_nanos(std::string_view ymd);

  // Resolve the projected column indices from the header line (tab-separated names).
  // Err(ParseError) if any required column (tradingDate, securityID, ticker_tk,
  // todayTicker, GICS, or any kOratsFields source) is absent. `gics`/`earnFlag` map
  // to header names "GICS"/"earnFlag"; the rest map by identical name.
  struct ColumnIndex {
    int tradingDate{-1}, securityID{-1}, ticker_tk{-1}, todayTicker{-1};
    std::array<int, 16> field{}; // index in the TSV for each kOratsFields entry
  };
  [[nodiscard]] atx::core::Result<ColumnIndex> resolve_header(std::string_view header_line);
  } // namespace detail
  } // namespace atx::engine::data
  ```
- Consumes: `atx::core::io::ZipEntryReader` (S3-1); `atx::tsdb::build_from_long` + `LongColumns`; `atx::core::io::write_parquet` + `WriteColumn`; `std::from_chars`.

- [ ] **Step 1: Write the failing unit tests for the pure helpers**

Create `atx-engine/tests/data_orats_history_test.cpp`. Start with `date_to_nanos` + `resolve_header` (pure, no zip):

```cpp
#include <array>
#include <filesystem>
#include <optional>
#include <string>

#include <gtest/gtest.h>
#include <miniz.h>

#include "atx/engine/data/orats_history.hpp"
#include "atx/tsdb/segment_reader.hpp"

namespace {
namespace fs = std::filesystem;
using namespace atx::engine::data;

// The real 71-column header (from the file). Only the columns the loader needs are
// asserted; the rest are present so resolve_header sees the true layout.
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
} // namespace

TEST(DataOratsHistory, DateToNanosMidnightUtc) {
  // 2020-01-02 is 18263 days after 1970-01-01.
  const atx::i64 expected = static_cast<atx::i64>(18263) * 86400LL * 1000000000LL;
  auto got = detail::date_to_nanos("2020-01-02");
  ASSERT_TRUE(got.has_value());
  EXPECT_EQ(*got, expected);
  EXPECT_FALSE(detail::date_to_nanos("2020-13-02").has_value()); // bad month
  EXPECT_FALSE(detail::date_to_nanos("not-a-date").has_value());
}

TEST(DataOratsHistory, ResolveHeaderFindsProjectedColumns) {
  auto idx = detail::resolve_header(kHeader);
  ASSERT_TRUE(idx.has_value()) << idx.error().to_string();
  EXPECT_EQ(idx->tradingDate, 0);
  EXPECT_EQ(idx->securityID, 1);
  EXPECT_EQ(idx->ticker_tk, 2);
  EXPECT_EQ(idx->todayTicker, 3);
  // field[0] == "open" is column 5; field[10] == "cumulReturnFactor" is column 65.
  EXPECT_EQ(idx->field[0], 5);
  EXPECT_EQ(kOratsFields[10], "cumulReturnFactor");
  EXPECT_EQ(idx->field[10], 65);
}

TEST(DataOratsHistory, ResolveHeaderRejectsMissingColumn) {
  auto idx = detail::resolve_header("tradingDate\tsecurityID\topen"); // missing most
  ASSERT_FALSE(idx.has_value());
  EXPECT_EQ(idx.error().code(), atx::core::ErrorCode::ParseError);
}
```

- [ ] **Step 2: Run to verify it fails**

```bash
ctest --test-dir build -R DataOratsHistory -V
```
Expected: FAIL to compile — `orats_history.hpp` missing.

- [ ] **Step 3: Write the header**

Create `atx-engine/include/atx/engine/data/orats_history.hpp` with the exact declarations from the Interfaces block. Includes: `<array>`, `<optional>`, `<string>`, `<string_view>`, `"atx/core/error.hpp"`, `"atx/core/types.hpp"`.

- [ ] **Step 4: Implement the pure helpers**

Create `atx-engine/src/data/orats_history.cpp`. Implement `date_to_nanos` (Howard Hinnant `days_from_civil`) and `resolve_header` first:

```cpp
#include "atx/engine/data/orats_history.hpp"

#include <charconv>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

#include "atx/core/io/zip_reader.hpp"
#include "atx/core/io/parquet_writer.hpp"
#include "atx/tsdb/load_parquet.hpp"

namespace atx::engine::data {
namespace detail {
namespace {
// Days from 1970-01-01 to civil (y,m,d), proleptic Gregorian (Howard Hinnant).
constexpr atx::i64 days_from_civil(int y, unsigned m, unsigned d) noexcept {
  y -= static_cast<int>(m <= 2);
  const int era = (y >= 0 ? y : y - 399) / 400;
  const auto yoe = static_cast<unsigned>(y - era * 400);
  const unsigned doy = (153u * (m + (m > 2 ? -3u : 9u)) + 2u) / 5u + d - 1u;
  const unsigned doe = yoe * 365u + yoe / 4u - yoe / 100u + doy;
  return static_cast<atx::i64>(era) * 146097LL + static_cast<atx::i64>(doe) - 719468LL;
}
} // namespace

std::optional<atx::i64> date_to_nanos(std::string_view ymd) {
  // strict "YYYY-MM-DD"
  if (ymd.size() != 10 || ymd[4] != '-' || ymd[7] != '-') return std::nullopt;
  int y = 0, mo = 0, d = 0;
  auto num = [](std::string_view s, int &out) {
    const auto r = std::from_chars(s.data(), s.data() + s.size(), out);
    return r.ec == std::errc{} && r.ptr == s.data() + s.size();
  };
  if (!num(ymd.substr(0, 4), y) || !num(ymd.substr(5, 2), mo) || !num(ymd.substr(8, 2), d))
    return std::nullopt;
  if (mo < 1 || mo > 12 || d < 1 || d > 31) return std::nullopt;
  const atx::i64 days = days_from_civil(y, static_cast<unsigned>(mo), static_cast<unsigned>(d));
  return days * 86400LL * 1000000000LL;
}

atx::core::Result<ColumnIndex> resolve_header(std::string_view header_line) {
  std::unordered_map<std::string_view, int> pos;
  int col = 0;
  atx::usize start = 0;
  while (start <= header_line.size()) {
    const atx::usize tab = header_line.find('\t', start);
    const atx::usize end = (tab == std::string_view::npos) ? header_line.size() : tab;
    pos.emplace(header_line.substr(start, end - start), col++);
    if (tab == std::string_view::npos) break;
    start = end + 1;
  }
  ColumnIndex idx;
  auto need = [&](std::string_view name, int &dst) -> bool {
    const auto it = pos.find(name);
    if (it == pos.end()) return false;
    dst = it->second;
    return true;
  };
  bool ok = need("tradingDate", idx.tradingDate) && need("securityID", idx.securityID) &&
            need("ticker_tk", idx.ticker_tk) && need("todayTicker", idx.todayTicker);
  // kOratsFields map by identical name EXCEPT gics->"GICS", earnFlag->"earnFlag".
  for (atx::usize f = 0; ok && f < kOratsFields.size(); ++f) {
    std::string_view src = kOratsFields[f];
    if (src == "gics") src = "GICS";
    ok = need(src, idx.field[f]);
  }
  if (!ok)
    return atx::core::Err(atx::core::ErrorCode::ParseError,
                          "orats resolve_header: a required column is missing");
  return atx::core::Ok(idx);
}
} // namespace detail
} // namespace atx::engine::data
```
Also add `src/data/orats_history.cpp` to `atx-engine/CMakeLists.txt` now so it builds.

- [ ] **Step 5: Run helper tests — verify pass**

```bash
ctest --test-dir build -R DataOratsHistory -V
```
Expected: PASS (the 3 helper tests).

- [ ] **Step 6: Write the failing end-to-end loader test (tiny zip → segments)**

Append to `data_orats_history_test.cpp`. Build a tiny zip at runtime (full header + a pre-2020 row that must be filtered + two 2020 dates), run `load_orats_history`, then attach a segment and assert values/symbols:

```cpp
namespace {
std::string make_orats_zip() {
  // header + 1 pre-2020 row (filtered) + date A (2 securities) + date B (1 security)
  std::string body = std::string(kHeader) + "\n";
  auto row = [](const char *date, const char *secid, const char *tk, const char *today,
                double close, double cumret, double shares) {
    // 71 tab-separated fields; fill only the ones the loader projects, zeros elsewhere.
    std::array<std::string, 71> f;
    for (auto &x : f) x = "0";
    f[0] = date; f[1] = secid; f[2] = tk; f[3] = today;
    f[5] = "1"; f[6] = "1"; f[7] = "1";              // open/high/low
    f[8] = std::to_string(close);                    // close (col 9, idx 8)
    f[10] = std::to_string(static_cast<long long>(shares)); // volume placeholder
    f[11] = std::to_string(static_cast<long long>(shares)); // shares (idx 11)
    f[62] = "5";                                     // GICS (idx 62)
    f[65] = std::to_string(cumret);                  // cumulReturnFactor (idx 65)
    std::string line;
    for (size_t i = 0; i < f.size(); ++i) { line += f[i]; if (i + 1 < f.size()) line += '\t'; }
    return line + "\n";
  };
  body += row("2019-12-31", "33449", "AAPL", "AAPL", 290.0, 0.9, 4000000000); // FILTERED
  body += row("2020-01-02", "33449", "AAPL", "AAPL", 300.0, 1.0, 4000000000);
  body += row("2020-01-02", "33008", "AA",   "HWM",  20.0,  0.5, 1000000000);
  body += row("2020-01-03", "33449", "AAPL", "AAPL", 303.0, 1.0, 4000000000);

  const fs::path p = fs::temp_directory_path() / "atx_orats_tiny.zip";
  fs::remove(p);
  mz_zip_archive zip{};
  EXPECT_TRUE(mz_zip_writer_init_file(&zip, p.string().c_str(), 0));
  EXPECT_TRUE(mz_zip_writer_add_mem(&zip, "tbltickerhistory3_10y.txt", body.data(), body.size(),
                                    MZ_BEST_SPEED));
  EXPECT_TRUE(mz_zip_writer_finalize_archive(&zip));
  EXPECT_TRUE(mz_zip_writer_end(&zip));
  return p.string();
}
} // namespace

TEST(DataOratsHistory, LoadsTinyZipIntoPerDateSegments) {
  const std::string zip = make_orats_zip();
  const fs::path out = fs::temp_directory_path() / "atx_orats_out";
  fs::remove_all(out);

  OratsLoadConfig cfg;
  cfg.zip_path = zip;
  cfg.out_dir = out.string();
  cfg.min_date_nanos = *detail::date_to_nanos("2020-01-01");
  cfg.created_at_nanos = 0;

  auto st = load_orats_history(cfg);
  ASSERT_TRUE(st.has_value()) << st.error().to_string();
  EXPECT_EQ(st->rows_read, 4);
  EXPECT_EQ(st->rows_filtered, 1);     // the 2019 row
  EXPECT_EQ(st->rows_kept, 3);
  EXPECT_EQ(st->dates_written, 2);     // 2020-01-02, 2020-01-03
  EXPECT_EQ(st->distinct_securities, 2);

  // The 2020-01-02 segment has 2 instruments; close field carries 300 and 20.
  auto rdr = atx::tsdb::SegmentReader::attach((out / "2020-01-02.seg").string());
  ASSERT_TRUE(rdr.has_value()) << rdr.error().to_string();
  EXPECT_EQ(rdr->instrument_count(), 2u);
  EXPECT_EQ(rdr->time_count(), 1u);
  const auto close_fid = rdr->field_index("close");
  ASSERT_TRUE(close_fid.has_value());
  // securityID "33449" interned first -> inst 0.
  EXPECT_EQ(rdr->symbol_name(0), "33449");
  EXPECT_DOUBLE_EQ(rdr->value(*close_fid, 0, 0), 300.0);
  EXPECT_DOUBLE_EQ(rdr->value(*close_fid, 0, 1), 20.0);
}
```

- [ ] **Step 7: Run to verify it fails**

```bash
ctest --test-dir build -R DataOratsHistory.LoadsTinyZip -V
```
Expected: FAIL — `load_orats_history` unimplemented (link error or stub returns Err).

- [ ] **Step 8: Implement the streaming loader**

Add to `orats_history.cpp` a chunked line reader over `ZipEntryReader`, a per-row projector, a per-date flush via `build_from_long`, the symbology + manifest side-cars, and `load_orats_history`. Key logic:

```cpp
namespace atx::engine::data {
namespace {
namespace fs = std::filesystem;

inline atx::f64 parse_f64(std::string_view s) {
  if (s.empty()) return std::numeric_limits<atx::f64>::quiet_NaN();
  atx::f64 v{};
  const auto r = std::from_chars(s.data(), s.data() + s.size(), v);
  if (r.ec != std::errc{}) return std::numeric_limits<atx::f64>::quiet_NaN();
  return v;
}

// Split a line into fields by tab WITHOUT allocating (returns views into `line`).
void split_tabs(std::string_view line, std::vector<std::string_view> &out) {
  out.clear();
  atx::usize start = 0;
  for (;;) {
    const atx::usize tab = line.find('\t', start);
    if (tab == std::string_view::npos) { out.push_back(line.substr(start)); break; }
    out.push_back(line.substr(start, tab - start));
    start = tab + 1;
  }
}

// Accumulates one date's rows, then writes a sealed .seg via build_from_long.
struct DateAccumulator {
  atx::i64 date_nanos{};
  std::string date_str;                       // YYYY-MM-DD for the filename
  std::vector<std::string> symbols;           // securityID per row
  std::vector<std::vector<atx::f64>> values;  // [16][rows]
  DateAccumulator() : values(kOratsFields.size()) {}
  void clear(atx::i64 dn, std::string ds) {
    date_nanos = dn; date_str = std::move(ds);
    symbols.clear();
    for (auto &v : values) v.clear();
  }
  bool empty() const { return symbols.empty(); }
};

atx::core::Status flush_date(DateAccumulator &acc, const std::string &out_dir,
                             atx::i64 created_at_nanos) {
  atx::tsdb::LongColumns cols;
  cols.field_names.assign(kOratsFields.begin(), kOratsFields.end());
  const atx::usize rows = acc.symbols.size();
  cols.times.assign(rows, acc.date_nanos);     // all rows share this date's midnight nanos
  cols.symbols = acc.symbols;
  cols.values = acc.values;
  const std::string path = (fs::path(out_dir) / (acc.date_str + ".seg")).string();
  return atx::tsdb::build_from_long(cols, path, created_at_nanos);
}
} // namespace
```

Then `load_orats_history`:
1. `fs::create_directories(cfg.out_dir)`.
2. `ZipEntryReader::open(cfg.zip_path, "tbltickerhistory")`.
3. Stream fixed-size chunks (e.g. `1 << 20`), maintaining a `std::string carry` for the partial trailing line; for each complete `\n`-terminated line:
   - The **first** line is the header → `resolve_header` (Err on failure).
   - Each data line: `split_tabs`; read `tradingDate` field; `date_to_nanos`; if `nullopt` → `rows_malformed++`, skip; if `< cfg.min_date_nanos` → `rows_filtered++`, skip.
   - `securityID` field empty → `rows_malformed++`, skip.
   - **Date-major guard:** if `date_nanos < current_date_nanos` → `Err(InvalidArgument, "non-monotonic tradingDate")`. If `date_nanos != current` → `flush_date(acc)`, `acc.clear(date_nanos, date_str)`, `dates_written++`.
   - Append `securityID` to `acc.symbols`; for each `f` in 0..15 push `parse_f64(fields[idx.field[f]])` to `acc.values[f]`. Record `securityID→(ticker_tk,todayTicker)` first-seen in a symbology map. `rows_kept++`.
   - `rows_read++` for every non-header data line (before any skip).
4. After the loop: flush the final `acc` (if non-empty) and `dates_written++`.
5. Write `_symbology.parquet` via `write_parquet` (columns: `securityID` i64, `ticker_tk` string, `todayTicker` string) and `_manifest.json` (counts + field list + `min_date_nanos`).
6. Return `OratsLoadStats`.

Counting note: increment `rows_read` once per data line; `rows_filtered`/`rows_malformed`/`rows_kept` are disjoint subsets, so `rows_read == rows_filtered + rows_malformed + rows_kept`.

Write the symbology side-car:
```cpp
  std::vector<atx::i64> sid; std::vector<std::string> tk, today;
  for (const auto &kv : symbology) { sid.push_back(kv.first); tk.push_back(kv.second.first);
                                     today.push_back(kv.second.second); }
  const std::vector<atx::core::io::WriteColumn> man = {
      {"securityID", std::span<const atx::i64>(sid)},
      {"ticker_tk", std::span<const std::string>(tk)},
      {"todayTicker", std::span<const std::string>(today)}};
  ATX_TRY_VOID(atx::core::io::write_parquet(man, (fs::path(cfg.out_dir) / "_symbology.parquet").string()));
```
(Use `securityID` parsed to i64 for the manifest key; the segment symbol name remains the original string form to match `securityID` interning.)

- [ ] **Step 9: Run the E2E loader test — verify pass**

```bash
ctest --test-dir build -R DataOratsHistory -V
```
Expected: PASS (all DataOratsHistory tests).

- [ ] **Step 10: Commit**

```bash
git add atx-engine/include/atx/engine/data/orats_history.hpp atx-engine/src/data/orats_history.cpp \
        atx-engine/tests/data_orats_history_test.cpp atx-engine/CMakeLists.txt
git commit -m "feat(p3-s3): load_orats_history — stream zip TSV into per-date .seg partition (S3-2)"
```

---

## Task 3 (S3-3): Total-return adjusted close via `cumulReturnFactor`

**Files:**
- Modify: `atx-engine/include/atx/engine/data/history_panel.hpp` (declare the helper — created here, fleshed out in Task 5)
- Create: `atx-engine/src/data/history_panel.cpp` (the helper only for now; the orchestrator is Task 5)
- Create: `atx-engine/tests/data_history_tri_test.cpp`
- Modify: `atx-engine/CMakeLists.txt` (add `src/data/history_panel.cpp`)

**Interfaces:**
- Produces:
  ```cpp
  namespace atx::engine::data {
  // Canonical total-return adjusted close = close * cum_return_factor, computed by
  // reusing adjust_total_return(close, cum_return_factor, zeros): its
  // total_return_index equals close*cum_return_factor exactly (dividends are already
  // folded into cum_return_factor, so the dividend input is 0). NaN policy inherited
  // from adjust_total_return: a NaN close OR NaN factor is a gap (NaN), never 0.
  // The two spans must be equal length (one symbol, ascending by date).
  [[nodiscard]] std::vector<atx::f64>
  orats_total_return_close(std::span<const atx::f64> close,
                           std::span<const atx::f64> cum_return_factor);
  } // namespace atx::engine::data
  ```
- Consumes: `atx::engine::data::adjust_total_return` (S1-3, `data/adjust.hpp`).

- [ ] **Step 1: Write the failing test**

Create `atx-engine/tests/data_history_tri_test.cpp`:

```cpp
#include <cmath>
#include <span>
#include <vector>

#include <gtest/gtest.h>

#include "atx/engine/data/history_panel.hpp"

namespace {
using atx::engine::data::orats_total_return_close;
} // namespace

TEST(DataHistoryTri, EqualsCloseTimesCumReturnFactor) {
  const std::vector<atx::f64> close = {606.98, 614.48, 617.62};
  const std::vector<atx::f64> caf = {0.0299354, 0.0299354, 0.0299354};
  const auto tri = orats_total_return_close(close, caf);
  ASSERT_EQ(tri.size(), 3u);
  for (size_t i = 0; i < close.size(); ++i)
    EXPECT_NEAR(tri[i], close[i] * caf[i], 1e-9) << "i=" << i;
  // AAPL 2012-03-26 present-basis adjusted close ≈ 18.16.
  EXPECT_NEAR(tri[0], 18.16, 0.01);
}

TEST(DataHistoryTri, ContinuousAcrossA4For1Split) {
  // A 4:1 split: raw close halves-then-quarters across the ex-date while the
  // cumulative factor steps by 4x, so the adjusted series is continuous (no jump).
  // Pre-split close 400 with factor 0.25; ex-date close 100 with factor 1.0.
  const std::vector<atx::f64> close = {400.0, 100.0, 101.0};
  const std::vector<atx::f64> caf = {0.25, 1.0, 1.0};
  const auto tri = orats_total_return_close(close, caf);
  EXPECT_NEAR(tri[0], 100.0, 1e-9);
  EXPECT_NEAR(tri[1], 100.0, 1e-9);  // no discontinuity at the ex-date
  EXPECT_NEAR(tri[2], 101.0, 1e-9);
}

TEST(DataHistoryTri, NanFactorIsGapNeverZeroFilled) {
  const std::vector<atx::f64> close = {100.0, std::nan(""), 102.0};
  const std::vector<atx::f64> caf = {1.0, std::nan(""), 1.0};
  const auto tri = orats_total_return_close(close, caf);
  EXPECT_DOUBLE_EQ(tri[0], 100.0);
  EXPECT_TRUE(std::isnan(tri[1]));
  EXPECT_DOUBLE_EQ(tri[2], 102.0);
}
```

- [ ] **Step 2: Run to verify it fails**

```bash
ctest --test-dir build -R DataHistoryTri -V
```
Expected: FAIL to compile — `history_panel.hpp` / the helper does not exist.

- [ ] **Step 3: Create the header stub + implement the helper**

Create `atx-engine/include/atx/engine/data/history_panel.hpp` with the `orats_total_return_close` declaration (includes `<span>`, `<vector>`, `"atx/core/types.hpp"`). Create `atx-engine/src/data/history_panel.cpp`:

```cpp
#include "atx/engine/data/history_panel.hpp"

#include <vector>

#include "atx/engine/data/adjust.hpp"

namespace atx::engine::data {

std::vector<atx::f64> orats_total_return_close(std::span<const atx::f64> close,
                                               std::span<const atx::f64> cum_return_factor) {
  // Dividends are already folded into cum_return_factor, so the dividend input is 0:
  // adjust_total_return then yields total_return_index == close * cum_return_factor,
  // with the proven gap/NaN policy and return-invariance contract.
  const std::vector<atx::f64> zero_div(close.size(), 0.0);
  AdjustedSeries adj = adjust_total_return(close, cum_return_factor, zero_div);
  return std::move(adj.total_return_index);
}

} // namespace atx::engine::data
```
Add `src/data/history_panel.cpp` to `atx-engine/CMakeLists.txt`.

- [ ] **Step 4: Run to verify it passes**

```bash
ctest --test-dir build -R DataHistoryTri -V
```
Expected: PASS (3 tests).

- [ ] **Step 5: Commit**

```bash
git add atx-engine/include/atx/engine/data/history_panel.hpp atx-engine/src/data/history_panel.cpp \
        atx-engine/tests/data_history_tri_test.cpp atx-engine/CMakeLists.txt
git commit -m "feat(p3-s3): orats_total_return_close — cumulReturnFactor TRI via adjust reuse (S3-3)"
```

---

## Task 4 (S3-4): `alpha::attach_multi_segment_panel` — the interface expansion

**Files:**
- Modify: `atx-engine/include/atx/engine/alpha/segment_panel.hpp` (add the declaration)
- Modify: `atx-engine/src/alpha/segment_panel.cpp` (add the implementation)
- Create: `atx-engine/tests/alpha_multi_segment_panel_test.cpp`

**Interfaces:**
- Produces:
  ```cpp
  namespace atx::engine::alpha {
  // Span a directory of per-date sealed segments into ONE owned alpha::Panel over
  // [window). Enumerates `*.seg` in `seg_dir`, sorts lexicographically (ISO
  // YYYY-MM-DD names sort chronologically), unions the per-segment securityID
  // instrument axes (join by symbol NAME, first-seen across dates in ascending date
  // order), and MATERIALIZES an owned date-major Panel (each per-date segment is a
  // separate mmap with its own instrument ordering, so a borrowed cross-segment
  // Panel is not possible without a global build-time axis — owned is correct here).
  //
  // `fields` empty => the field set of the first in-window segment, in segment order;
  // otherwise exactly the named fields (Err(NotFound) if any segment lacks one).
  // A cell absent on a date reads NaN; the universe mask follows `universe` (the
  // present-bitmap by default). Err(InvalidArgument) if no .seg files / no in-window
  // dates. Returns an OWNED Panel (the readers are released before return).
  [[nodiscard]] atx::core::Result<Panel>
  attach_multi_segment_panel(const std::string &seg_dir, TimeWindow window = {},
                             std::span<const std::string> fields = {},
                             UniversePolicy universe = {});
  } // namespace atx::engine::alpha
  ```
- Consumes: `atx::tsdb::SegmentReader` (`attach`, `times`, `instrument_count`, `symbol_name`, `field_index`, `field_name`, `field_count`, `value`, `present`); `alpha::Panel::create`.

- [ ] **Step 1: Write the failing test**

Create `atx-engine/tests/alpha_multi_segment_panel_test.cpp`. Build three per-date segments with disjoint+overlapping instrument sets using `atx::tsdb::build_from_long`, then attach and assert the union axis + cell placement + window cutoff:

```cpp
#include <filesystem>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "atx/engine/alpha/panel.hpp"
#include "atx/engine/alpha/segment_panel.hpp"
#include "atx/tsdb/load_parquet.hpp"

namespace {
namespace fs = std::filesystem;
using namespace atx::engine::alpha;

atx::i64 day_nanos(atx::i64 day_index) { return day_index * 86400LL * 1000000000LL; }

// Write a 1-date segment with the given symbols + a single "close" field.
void write_seg(const fs::path &path, atx::i64 dn, std::vector<std::string> syms,
               std::vector<atx::f64> close) {
  atx::tsdb::LongColumns cols;
  cols.field_names = {"close"};
  cols.times.assign(syms.size(), dn);
  cols.symbols = std::move(syms);
  cols.values = {std::move(close)};
  ASSERT_TRUE(atx::tsdb::build_from_long(cols, path.string(), 0).has_value());
}
} // namespace

TEST(AlphaMultiSegmentPanel, UnionsInstrumentAxesAcrossDates) {
  const fs::path dir = fs::temp_directory_path() / "atx_multi_seg";
  fs::remove_all(dir);
  fs::create_directories(dir);
  // day 0: {A, B}; day 1: {B, C}; day 2: {A}.  Union (first-seen) = [A, B, C].
  write_seg(dir / "2020-01-01.seg", day_nanos(18262), {"A", "B"}, {10.0, 20.0});
  write_seg(dir / "2020-01-02.seg", day_nanos(18263), {"B", "C"}, {21.0, 30.0});
  write_seg(dir / "2020-01-03.seg", day_nanos(18264), {"A"}, {11.0});

  auto panel = attach_multi_segment_panel(dir.string());
  ASSERT_TRUE(panel.has_value()) << panel.error().to_string();
  EXPECT_EQ(panel->dates(), 3u);
  EXPECT_EQ(panel->instruments(), 3u);          // A, B, C
  auto fid = panel->field_id("close");
  ASSERT_TRUE(fid.has_value());
  const auto col = panel->field_all(*fid);       // date-major, 3*3
  // (date0): A=10, B=20, C=NaN
  EXPECT_DOUBLE_EQ(col[0 * 3 + 0], 10.0);
  EXPECT_DOUBLE_EQ(col[0 * 3 + 1], 20.0);
  EXPECT_TRUE(std::isnan(col[0 * 3 + 2]));
  // (date1): A=NaN, B=21, C=30
  EXPECT_TRUE(std::isnan(col[1 * 3 + 0]));
  EXPECT_DOUBLE_EQ(col[1 * 3 + 1], 21.0);
  EXPECT_DOUBLE_EQ(col[1 * 3 + 2], 30.0);
  // (date2): A=11, B=NaN, C=NaN
  EXPECT_DOUBLE_EQ(col[2 * 3 + 0], 11.0);
  // universe (present bitmap): A in date0, not date1
  EXPECT_TRUE(panel->in_universe(0, 0));
  EXPECT_FALSE(panel->in_universe(1, 0));
}

TEST(AlphaMultiSegmentPanel, WindowExcludesOutOfRangeDates) {
  const fs::path dir = fs::temp_directory_path() / "atx_multi_seg_win";
  fs::remove_all(dir);
  fs::create_directories(dir);
  write_seg(dir / "2020-01-01.seg", day_nanos(18262), {"A"}, {10.0});
  write_seg(dir / "2020-01-02.seg", day_nanos(18263), {"A"}, {11.0});
  write_seg(dir / "2020-01-03.seg", day_nanos(18264), {"A"}, {12.0});

  TimeWindow w;
  w.start_nanos = day_nanos(18263);   // include day1..
  w.end_nanos = day_nanos(18264);     // ..exclusive of day2
  auto panel = attach_multi_segment_panel(dir.string(), w);
  ASSERT_TRUE(panel.has_value()) << panel.error().to_string();
  EXPECT_EQ(panel->dates(), 1u);
  auto fid = panel->field_id("close");
  ASSERT_TRUE(fid.has_value());
  EXPECT_DOUBLE_EQ(panel->field_all(*fid)[0], 11.0);
}
```

- [ ] **Step 2: Run to verify it fails**

```bash
ctest --test-dir build -R AlphaMultiSegmentPanel -V
```
Expected: FAIL — `attach_multi_segment_panel` undeclared.

- [ ] **Step 3: Declare in the header**

Add the `attach_multi_segment_panel` declaration (from the Interfaces block) to `atx-engine/include/atx/engine/alpha/segment_panel.hpp`, after `attach_segment_panel`.

- [ ] **Step 4: Implement**

Add to `atx-engine/src/alpha/segment_panel.cpp`. Algorithm — two passes, materialize an owned Panel:

```cpp
atx::core::Result<Panel>
attach_multi_segment_panel(const std::string &seg_dir, TimeWindow window,
                           std::span<const std::string> fields, UniversePolicy universe) {
  namespace fs = std::filesystem;
  // 1. Enumerate + sort .seg paths (ISO names sort chronologically).
  std::vector<std::string> paths;
  std::error_code ec;
  for (const auto &e : fs::directory_iterator(seg_dir, ec)) {
    if (e.path().extension() == ".seg") paths.push_back(e.path().string());
  }
  if (paths.empty())
    return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                          "attach_multi_segment_panel: no .seg files in " + seg_dir);
  std::sort(paths.begin(), paths.end());

  // 2. Open readers; collect in-window (segment, local-row, date_nanos) triples and
  //    the global date axis (unique, ascending).
  std::vector<atx::tsdb::SegmentReader> readers;
  readers.reserve(paths.size());
  struct Row { atx::usize seg; atx::usize t; atx::i64 nanos; };
  std::vector<Row> rows;
  for (const auto &p : paths) {
    ATX_TRY(auto rdr, atx::tsdb::SegmentReader::attach(p));
    const auto times = rdr.times();
    for (atx::usize t = 0; t < times.size(); ++t) {
      if (times[t] >= window.start_nanos && times[t] < window.end_nanos)
        rows.push_back({readers.size(), t, times[t]});
    }
    readers.push_back(std::move(rdr));
  }
  if (rows.empty())
    return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                          "attach_multi_segment_panel: window selects no dates");
  // Global date axis (rows already in ascending path/date order; one date per row here).
  std::vector<atx::i64> date_axis;
  for (const auto &r : rows) if (date_axis.empty() || date_axis.back() != r.nanos)
    date_axis.push_back(r.nanos);
  const atx::usize D = date_axis.size();
  std::unordered_map<atx::i64, atx::usize> date_row;
  for (atx::usize d = 0; d < D; ++d) date_row.emplace(date_axis[d], d);

  // 3. Global instrument union (first-seen across rows in ascending date order).
  std::vector<std::string> inst_names;
  std::unordered_map<std::string, atx::usize> inst_of;
  for (const auto &r : rows) {
    const auto &rdr = readers[r.seg];
    for (atx::u32 j = 0; j < rdr.instrument_count(); ++j) {
      const std::string nm{rdr.symbol_name(j)};
      if (inst_of.emplace(nm, inst_names.size()).second) inst_names.push_back(nm);
    }
  }
  const atx::usize N = inst_names.size();

  // 4. Resolve the field list (empty => first reader's fields in order).
  std::vector<std::string> field_names;
  if (fields.empty()) {
    const auto &r0 = readers[rows.front().seg];
    for (atx::u32 f = 0; f < r0.field_count(); ++f) field_names.emplace_back(r0.field_name(f));
  } else {
    field_names.assign(fields.begin(), fields.end());
  }
  const atx::usize F = field_names.size();

  // 5. Allocate owned columns (NaN) + universe mask (0); fill.
  const atx::f64 nan = std::numeric_limits<atx::f64>::quiet_NaN();
  std::vector<std::vector<atx::f64>> data(F, std::vector<atx::f64>(D * N, nan));
  std::vector<std::uint8_t> mask(D * N, 0);
  for (const auto &r : rows) {
    const auto &rdr = readers[r.seg];
    const atx::usize d = date_row.at(r.nanos);
    // Per-field local indices in THIS segment.
    std::vector<std::optional<atx::u32>> fmap(F);
    for (atx::usize f = 0; f < F; ++f) fmap[f] = rdr.field_index(field_names[f]);
    for (atx::u32 j = 0; j < rdr.instrument_count(); ++j) {
      const atx::usize gi = inst_of.at(std::string{rdr.symbol_name(j)});
      const atx::usize cell = d * N + gi;
      bool present_here = false;
      if (universe.kind == UniverseKind::PresentBitmap) present_here = rdr.present(r.t, j);
      for (atx::usize f = 0; f < F; ++f) {
        if (!fmap[f].has_value()) {
          if (!fields.empty())
            return atx::core::Err(atx::core::ErrorCode::NotFound,
                                  "attach_multi_segment_panel: field '" + field_names[f] +
                                      "' absent in a segment");
          continue;
        }
        data[f][cell] = rdr.value(*fmap[f], r.t, j);
      }
      if (universe.kind == UniverseKind::Field) {
        const auto ufid = rdr.field_index(universe.field_name);
        if (ufid.has_value()) {
          const atx::f64 v = rdr.value(*ufid, r.t, j);
          present_here = !std::isnan(v) && v != 0.0;
        }
      }
      if (present_here) mask[cell] = 1;
    }
  }

  // 6. Build the owned Panel; readers drop at scope exit (data is copied).
  return Panel::create(D, N, std::move(field_names), std::move(data), std::move(mask));
}
```
Add the needed includes to the `.cpp`: `<algorithm>`, `<filesystem>`, `<limits>`, `<optional>`, `<unordered_map>`.

- [ ] **Step 5: Run to verify it passes**

```bash
ctest --test-dir build -R AlphaMultiSegmentPanel -V
```
Expected: PASS (both tests).

- [ ] **Step 6: Commit**

```bash
git add atx-engine/include/atx/engine/alpha/segment_panel.hpp atx-engine/src/alpha/segment_panel.cpp \
        atx-engine/tests/alpha_multi_segment_panel_test.cpp
git commit -m "feat(p3-s3): attach_multi_segment_panel — union per-date segments into one Panel (S3-4)"
```

---

## Task 5 (S3-5): `data::build_history_panel` — orchestrator + golden digest

**Files:**
- Modify: `atx-engine/include/atx/engine/data/history_panel.hpp` (add config/result/build + field-order constants)
- Modify: `atx-engine/src/data/history_panel.cpp` (add the orchestrator)
- Create: `atx-engine/tests/data_history_panel_test.cpp`

**Interfaces:**
- Produces:
  ```cpp
  namespace atx::engine::data {

  // Canonical assembled-Panel field order (digest hashes fields in THIS order).
  inline constexpr std::string_view kHistFieldClose = "close";        // = TRI (close*cumret)
  inline constexpr std::string_view kHistFieldRawClose = "raw_close";  // raw as-traded close
  inline constexpr std::string_view kHistFieldVolume = "volume";
  inline constexpr std::string_view kHistFieldHigh = "high";
  inline constexpr std::string_view kHistFieldLow = "low";
  inline constexpr std::string_view kHistFieldOpen = "open";
  inline constexpr std::string_view kHistFieldMarketCap = "market_cap";
  inline constexpr std::string_view kHistFieldSector = "sector";

  struct HistoryDataConfig {
    std::string seg_dir;          // data/orats_history_1d
    alpha::TimeWindow window{};   // [start,end) trading dates (unix-nanos)
    UniverseConfig universe{};    // market-cap / ADV / sector / top-N screen
  };

  struct HistoryPanel {
    alpha::Panel panel;
    atx::u64 digest{};
    std::vector<std::string> lineage;
  };

  // Assemble a deterministic, digest-pinned real-data Panel from the on-disk ORATS
  // partition: multi-segment attach -> TRI close (close*cumulReturnFactor, raw kept)
  // -> S1 universe screen (market_cap = shares*raw_close, causal ADV, GICS sector,
  // in_universe mask) -> Catalog lineage -> final Panel in kHistField* order -> digest.
  // Two calls with identical inputs return an identical digest. Err on: missing
  // partition, an empty window, or a shape mismatch (propagated).
  [[nodiscard]] atx::core::Result<HistoryPanel> build_history_panel(const HistoryDataConfig &cfg);

  } // namespace atx::engine::data
  ```
- Consumes: `attach_multi_segment_panel` (S3-4), `orats_total_return_close` (S3-3), `build_universe` + `UniverseConfig`/`UniverseFields` (S1-4), `Dataset::create` + `DatasetSchema`/`Role::Reference` (S6), `DatasetCatalog` (S6), `corporate_actions.hpp` `kCol*` constants + `kNoSectorCode`/`kNoDate`, and a **shared** `data::digest_panel` helper (extracted in Step 0 below).
- Also creates: `atx-engine/include/atx/engine/data/panel_digest.hpp` (the extracted shared helper).
- Also modifies: `atx-engine/src/data/real_panel.cpp` (use the shared helper instead of its private copy).

**Assembly (mirror `atx-engine/src/data/real_panel.cpp:349-418`, substituting the ORATS source):**

1. **Raw panel** — `attach_multi_segment_panel(cfg.seg_dir, cfg.window, {kOratsFields…}, PresentBitmap)`. This panel's `close` field is the raw as-traded close (the universe step needs raw close + volume). Let `D = panel.dates()`, `N = panel.instruments()`.
2. **Corp-action Dataset (axis-matched, positional)** — build a Reference-role `Dataset` with the canonical 6 columns in order (`kColCumAdjFactor, kColCashDividend, kColSharesOutstanding, kColSharesFiledDate, kColGicsSectorCode, kColSicCode`), `D×N` each:
   - `cum_adj_factor` = the panel's `cumulReturnFactor` column (used only to keep the shape; universe ignores it for mktcap).
   - `cash_dividend` = 0.
   - `shares_outstanding` = the panel's `shares` column.
   - `shares_filed_date` = `kNoDate` (the ORATS row is already PIT-as-published; no separate filing date).
   - `gics_sector_code` = the panel's `gics` column (file's single-digit code; `kNoSectorCode` where NaN/absent — map NaN→-1).
   - `sic_code` = `kNoSectorCode`.
   Axis: `std::vector<DateKey> dates(D)` ascending `0..D-1`, `std::vector<InstKey> insts(N)` `0..N-1` (positional — `build_universe` matches by COUNT/position per the alignment contract, so synthetic ascending keys are correct here). Construct via `Dataset::create(schema, dates, insts, columns, /*mask=*/{}, provenance)`.
3. **Universe** — `build_universe(raw_panel, corp_dataset, cfg.universe)` → `UniverseFields { market_cap, adv_usd, sector_code, in_universe }`. (`market_cap = shares × raw close`; ADV recomputed causally since the raw panel carries no `adv{w}` column.)
4. **TRI close** — per instrument column, gather its `close` and `cumulReturnFactor` time series (stride `N` over the date-major columns), call `orats_total_return_close`, scatter back into a `D×N` `close_tri` column.
5. **Final panel** — assemble in `kHistField*` order:
   `close` = `close_tri`; `raw_close` = raw `close`; `volume`, `high`, `low`, `open` from the raw panel; `market_cap` = `uni.market_cap`; `sector` = `uni.sector_code` widened to f64. Mask = `uni.in_universe`. Build via `alpha::Panel::create(D, N, names, data, mask)`.
6. **Catalog + digest** — register a Reference dataset (a thin lineage record is enough: register `price`/`corp_actions`/`universe` names like `real_panel.cpp` lines 408-412 using the datasets you have, or a minimal lineage of `{"orats_history", "universe"}`), record lineage, then `digest = digest_panel(final_panel)` using the **shared** `data::digest_panel` (extracted in Step 0). Return `HistoryPanel { std::move(panel), digest, lineage }`.

- [ ] **Step 0: Extract the shared `digest_panel` helper (refactor, no behavior change)**

`real_panel.cpp` has a private static `digest_panel(const alpha::Panel&) -> atx::u64`. Move it to a new shared header `atx-engine/include/atx/engine/data/panel_digest.hpp` as `atx::engine::data::digest_panel`, and change `real_panel.cpp` to include + call it (deleting its private copy). The function body is unchanged, so the S1-5 golden digest (`0x2a22a873483d9157`) is byte-identical. Verify by running the existing real-panel E2E test:
```bash
pwsh -File scripts/dev-build.ps1 -Build -Test -TestRegex DataRealPanel
```
Expected: the S1-5 digest test still PASSES (unchanged). Commit this refactor separately:
```bash
git add atx-engine/include/atx/engine/data/panel_digest.hpp atx-engine/src/data/real_panel.cpp
git commit -m "refactor(p3-s3): extract shared data::digest_panel (no digest change) (S3-5)"
```

- [ ] **Step 1: Write the failing determinism test**

Create `atx-engine/tests/data_history_panel_test.cpp`. Build a tiny partition with `build_from_long` (the 16 fields, 2 dates, 2 securities), then assert `build_history_panel` produces a stable digest across two builds and the canonical fields:

```cpp
#include <filesystem>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "atx/engine/data/history_panel.hpp"
#include "atx/engine/data/orats_history.hpp"   // kOratsFields
#include "atx/tsdb/load_parquet.hpp"

namespace {
namespace fs = std::filesystem;
using namespace atx::engine::data;

atx::i64 day_nanos(atx::i64 d) { return d * 86400LL * 1000000000LL; }

// Write one date's segment with all 16 ORATS fields for the given securities.
void write_day(const fs::path &dir, const char *name, atx::i64 dn,
               std::vector<std::string> syms,
               std::vector<atx::f64> close, std::vector<atx::f64> cumret,
               std::vector<atx::f64> shares) {
  atx::tsdb::LongColumns cols;
  cols.field_names.assign(kOratsFields.begin(), kOratsFields.end());
  const size_t r = syms.size();
  cols.times.assign(r, dn);
  cols.symbols = syms;
  cols.values.assign(kOratsFields.size(), std::vector<atx::f64>(r, 0.0));
  // indices into kOratsFields: 3=close, 7=shares, 10=cumulReturnFactor, 6=volume
  cols.values[3] = close;
  cols.values[6] = std::vector<atx::f64>(r, 1e7); // volume
  cols.values[7] = shares;
  cols.values[10] = cumret;
  ASSERT_TRUE(atx::tsdb::build_from_long(cols, (dir / name).string(), 0).has_value());
}
} // namespace

TEST(DataHistoryPanel, DeterministicDigestAndCanonicalFields) {
  const fs::path dir = fs::temp_directory_path() / "atx_hist_panel";
  fs::remove_all(dir);
  fs::create_directories(dir);
  write_day(dir, "2020-01-02.seg", day_nanos(18263), {"33449", "33008"},
            {300.0, 20.0}, {1.0, 0.5}, {4e9, 1e9});
  write_day(dir, "2020-01-03.seg", day_nanos(18264), {"33449", "33008"},
            {303.0, 21.0}, {1.0, 0.5}, {4e9, 1e9});

  HistoryDataConfig cfg;
  cfg.seg_dir = dir.string();
  cfg.universe.min_adv_usd = 0.0;     // keep both names in the smoke
  cfg.universe.adv_window = 1;

  auto a = build_history_panel(cfg);
  ASSERT_TRUE(a.has_value()) << a.error().to_string();
  auto b = build_history_panel(cfg);
  ASSERT_TRUE(b.has_value()) << b.error().to_string();
  EXPECT_EQ(a->digest, b->digest);                       // byte-reproducible

  const alpha::Panel &p = a->panel;
  EXPECT_EQ(p.dates(), 2u);
  EXPECT_EQ(p.instruments(), 2u);
  auto close = p.field_id(kHistFieldClose);
  auto raw = p.field_id(kHistFieldRawClose);
  ASSERT_TRUE(close.has_value()); ASSERT_TRUE(raw.has_value());
  // close = TRI = raw*cumret; for 33008 (idx1) date0: 20 * 0.5 = 10.
  EXPECT_DOUBLE_EQ(p.field_all(*raw)[0 * 2 + 1], 20.0);
  EXPECT_DOUBLE_EQ(p.field_all(*close)[0 * 2 + 1], 10.0);
  // market_cap present and = shares*raw_close (split-invariant): 1e9*20 = 2e10.
  auto mcap = p.field_id(kHistFieldMarketCap);
  ASSERT_TRUE(mcap.has_value());
  EXPECT_DOUBLE_EQ(p.field_all(*mcap)[0 * 2 + 1], 2.0e10);
}
```

- [ ] **Step 2: Run to verify it fails**

```bash
ctest --test-dir build -R DataHistoryPanel -V
```
Expected: FAIL — `build_history_panel` / config types undeclared.

- [ ] **Step 3: Declare the config/result/constants in the header**

Add the `kHistField*` constants, `HistoryDataConfig`, `HistoryPanel`, and `build_history_panel` declaration to `atx-engine/include/atx/engine/data/history_panel.hpp`. Add includes: `<string>`, `"atx/engine/alpha/panel.hpp"`, `"atx/engine/alpha/segment_panel.hpp"` (for `TimeWindow`), `"atx/engine/data/universe.hpp"` (for `UniverseConfig`).

- [ ] **Step 4: Implement the orchestrator**

In `atx-engine/src/data/history_panel.cpp`, implement `build_history_panel` following the 6-step assembly above. Read `atx-engine/src/data/real_panel.cpp:349-418` and `atx-engine/include/atx/engine/data/dataset.hpp` + `dataset_schema.hpp` + `corporate_actions.hpp` for the exact `Dataset::create` / `DatasetSchema` / `DatasetCatalog` / `kCol*` signatures, and `#include "atx/engine/data/panel_digest.hpp"` to call the **shared** `data::digest_panel` (extracted in Step 0). Use `ATX_TRY` for each fallible call. The column-gather/scatter for steps 4 (TRI) and 5 (final assembly) use the date-major stride `cell = d * N + i`:

```cpp
  // Step 4: TRI per instrument column (stride N over date-major columns).
  ATX_TRY(auto close_fid, raw.field_id(kHistFieldClose)); // raw panel's close == raw close
  ATX_TRY(auto cumret_fid, raw.field_id("cumulReturnFactor"));
  const std::span<const atx::f64> rc = raw.field_all(close_fid);
  const std::span<const atx::f64> cr = raw.field_all(cumret_fid);
  std::vector<atx::f64> close_tri(D * N);
  std::vector<atx::f64> one_close(D), one_cr(D), one_tri;
  for (atx::usize i = 0; i < N; ++i) {
    for (atx::usize d = 0; d < D; ++d) { one_close[d] = rc[d * N + i]; one_cr[d] = cr[d * N + i]; }
    one_tri = orats_total_return_close(one_close, one_cr);
    for (atx::usize d = 0; d < D; ++d) close_tri[d * N + i] = one_tri[d];
  }
```
Assemble `data`/`names` in `kHistField*` order, then `Panel::create(D, N, std::move(names), std::move(data), std::vector<std::uint8_t>(uni.in_universe.begin(), uni.in_universe.end()))`, compute `digest_panel`, and return.

- [ ] **Step 5: Run to verify it passes**

```bash
ctest --test-dir build -R DataHistoryPanel -V
```
Expected: PASS.

- [ ] **Step 6: Commit**

```bash
git add atx-engine/include/atx/engine/data/history_panel.hpp atx-engine/src/data/history_panel.cpp \
        atx-engine/tests/data_history_panel_test.cpp
git commit -m "feat(p3-s3): build_history_panel orchestrator + golden digest (S3-5)"
```

---

## Task 6 (S3-6): Full end-to-end smoke on the US-equity universe

**Files:**
- Create: `atx-engine/tests/orats_e2e_smoke_test.cpp`

**Interfaces:**
- Consumes: `data::load_orats_history` (S3-2), `data::build_history_panel` (S3-5), `factory::RobustResearchDriver` (header `atx/engine/factory/robust_pipeline.hpp`).
- Produces: a guarded, deterministic, time-boxed E2E test that runs only when the real partition (or the real zip) is present; skips cleanly otherwise.

**Notes:** mirror the existing `atx-engine/tests/robust_pipeline_e2e_test.cpp` scaffolding for constructing `library::Library`, the `alpha::Library` DSL, `exec::ExecutionSimulator`, `WeightPolicy`, `combine::AlphaGate`, and the `RobustPipelineConfig`. The ONLY substitution is the input Panel: instead of `eval::generate_synthetic_panel`, use the Panel from `build_history_panel`. Use the `find_data_root()` probe pattern from `data_real_panel_e2e_test.cpp:112` (add a `kOratsProbe = "data/orats_history_1d"`).

- [ ] **Step 1: Write the guarded E2E test**

Create `atx-engine/tests/orats_e2e_smoke_test.cpp`. Structure:

```cpp
#include <filesystem>
#include <optional>

#include <gtest/gtest.h>

#include "atx/engine/data/history_panel.hpp"
#include "atx/engine/factory/robust_pipeline.hpp"
// ... the same includes robust_pipeline_e2e_test.cpp uses for Library/DSL/sim/gate ...

namespace {
namespace fs = std::filesystem;

// Mirror data_real_panel_e2e_test.cpp:112 find_data_root(), probing for the ORATS partition.
constexpr const char *kOratsProbe = "data/orats_history_1d";
std::optional<fs::path> find_orats_root() { /* copy find_data_root, swap the probe */ }
} // namespace

TEST(OratsE2ESmoke, RealPartitionRunsUnchangedRobustPipeline) {
  auto root = find_orats_root();
  if (!root) GTEST_SKIP() << "ORATS partition not built; skipping (operator data-build)";
  const std::string seg_dir = (*root / "data" / "orats_history_1d").string();

  atx::engine::data::HistoryDataConfig cfg;
  cfg.seg_dir = seg_dir;
  cfg.window.start_nanos = *atx::engine::data::detail::date_to_nanos("2020-01-02");
  cfg.window.end_nanos   = *atx::engine::data::detail::date_to_nanos("2020-04-01"); // small smoke window
  cfg.universe.min_adv_usd = 5.0e6;   // a liquid subset
  cfg.universe.top_n_by_adv = 200;    // cap for a time-boxed smoke

  auto hp = atx::engine::data::build_history_panel(cfg);
  ASSERT_TRUE(hp.has_value()) << hp.error().to_string();
  EXPECT_GT(hp->panel.instruments(), 0u);
  EXPECT_GT(hp->panel.dates(), 0u);

  // ---- drive the UNCHANGED p2 pipeline (mirror robust_pipeline_e2e_test.cpp) ----
  // library::Library lib{...}; alpha::Library dsl{...};
  // exec::ExecutionSimulator sim{...}; WeightPolicy policy{...}; combine::AlphaGate gate{...};
  // factory::RobustResearchDriver driver{lib, dsl, hp->panel, sim, policy, gate};
  // factory::RobustPipelineConfig pcfg{}; pcfg.research.<small budget>; pcfg.lockbox_frac = 0.2;
  // auto report = driver.run(pcfg);
  // ASSERT_TRUE(report.has_value()) << report.error().to_string();
  // EXPECT_NO_THROW((void)report->research.digest);   // pipeline ran end-to-end
}
```

Fill the commented block by copying the construction lines from `robust_pipeline_e2e_test.cpp` verbatim and pointing the driver at `hp->panel`. Keep the search budget small (a few generations / a low trial count) so the smoke is time-boxed. Expose `detail::date_to_nanos` (already declared in `orats_history.hpp`).

- [ ] **Step 2: Run the guarded test (skips without data)**

```bash
ctest --test-dir build -R OratsE2ESmoke -V
```
Expected: PASS as **SKIPPED** when `data/orats_history_1d` is absent (the committed CI state). The body compiles and links.

- [ ] **Step 3: Commit**

```bash
git add atx-engine/tests/orats_e2e_smoke_test.cpp
git commit -m "feat(p3-s3): guarded E2E smoke — ORATS partition through unchanged p2 pipeline (S3-6)"
```

- [ ] **Step 4: Operator data-build (manual; records the real run)**

Run the real loader + smoke once, locally, and record counts in the ledger (output is gitignored). In a throwaway driver (a `bench/` main or a temporarily un-skipped test):
```
load_orats_history({ zip_path = "~/Downloads/tbltickerhistory3_10y.zip",
                     out_dir = "data/orats_history_1d",
                     min_date_nanos = date_to_nanos("2020-01-01"),
                     created_at_nanos = <wall clock> })
```
Then re-run `OratsE2ESmoke` (now finds the partition) and confirm it drives the pipeline clean within the time budget. Record `OratsLoadStats` (rows_read/kept/filtered, dates_written, distinct_securities), the build wall-time, and the smoke outcome in `sprint-3-progress.md`. Do NOT commit `data/`.

---

## Task 7 (S3-close): Docs, ledger, merge

**Files:**
- Modify: `atx-engine/plans/p3-impl/data-ingestion-reference.md` (new section for the ORATS single-file source)
- Create: `atx-engine/plans/p3-impl/sprint3.md` (user reference)
- Modify: `atx-engine/plans/p3-impl/sprint-3-progress.md` (final status, residuals → backlog)
- Modify: `atx-engine/plans/p3-impl/ROADMAP.md` (mark S3 closed; note the backlog residuals)

**Interfaces:**
- Produces: the closed sprint — docs accurate to the as-built code, ledger baton, `--no-ff` merge.

- [ ] **Step 1: Document the new source**

Add a section to `data-ingestion-reference.md` describing: the ORATS single-file source (path, 71-col schema, the 16-field projection, securityID identity, the `close × cumulReturnFactor` TRI mechanic, the lagged-`closeUnadjPr` note), the `ZipEntryReader` → `load_orats_history` → per-date `.seg` partition path, `attach_multi_segment_panel`, and `build_history_panel` (with its digest pin). Anchor each claim to `file:line` at the merge HEAD.

- [ ] **Step 2: Run the full engine + core suites — verify green**

```bash
ctest --test-dir build -V
```
Expected: all tests pass (new S3 tests + the prior suite; `OratsE2ESmoke` SKIPPED without data). Record the pass count in the ledger.

- [ ] **Step 3: Write `sprint3.md` + close the ledger**

Write the user-facing `sprint3.md` (what shipped, how to run the loader, the digest pin, the data-build command). Fill the final status table in `sprint-3-progress.md`; move residuals to the ROADMAP backlog (e.g. **borrowed/zero-copy multi-segment attach via a global build-time axis**; **parse-parallelism with chunk-boundary stitching**; **IV-surface alpha operators** as p4; **`closePr` semantics** documented if still unverified; **GICS single-digit encoding** mapping table).

- [ ] **Step 4: Commit the close + merge**

```bash
git add atx-engine/plans/p3-impl/
git commit -m "docs(p3-s3): close S3 ORATS loader — sprint3 ref, ledger, data-ingestion doc, backlog"
```
Then merge per sprint discipline (no push unless the user asks):
```bash
git switch main
git merge --no-ff feat/p3-s3-orats-loader -m "merge: p3-s3 — single-file ORATS history loader + E2E smoke"
```

---

## Self-Review

**Spec coverage** (each spec section → task):
- §2 pure C++ loader / streaming → Task 1 (ZipEntryReader) + Task 2 (load_orats_history). ✓
- §2 wire into data interface + expand → Task 4 (attach_multi_segment_panel) + Task 5 (build_history_panel). ✓
- §2 corporate actions via cumulReturnFactor → Task 3 (orats_total_return_close) + Task 5 step 4. ✓
- §2 full E2E smoke on US universe → Task 6. ✓
- §3 16-field projection / forward-col exclusion → Task 2 `kOratsFields` + `resolve_header`. ✓
- §4.3 per-date partition + multi-attach → Tasks 2 + 4. ✓
- §5 TRI = close×cumret, raw retained, adjust reuse → Task 3 + Task 5. ✓
- §6 testing (synthetic fixtures, AAPL split, union, digest, guarded E2E) → Tasks 1-6 tests. ✓
- §7 performance (streaming, from_chars, date-major one-date buffer) → Task 2 step 8 (SPSC producer/consumer is the deferred lever, noted in §7/backlog — single parse thread first per the spec). ✓
- §8 roadmap placement → Task 0 + Task 7. ✓
- §9 defaults (partition root, universe defaults, GICS encoding) → Tasks 2/5/7. ✓
- §10 exit criteria → Tasks 5 (digest), 2 (loader), 4 (interface), 6 (E2E), 3 (adjustment validation), 7 (docs+merge). ✓

**Placeholder scan:** no TBD/TODO; every code step shows code; the E2E (Task 6) names the exact template file + the single substitution (not a placeholder — a precise mirror instruction). The orchestrator (Task 5 step 4) gives the assembly + the novel gather/scatter literal code and pins the `digest_panel` copy source. ✓

**Type consistency:** `kOratsFields` (16, indices used in tests: 3=close, 6=volume, 7=shares, 10=cumulReturnFactor) consistent across Tasks 2/5; `OratsLoadConfig`/`OratsLoadStats`, `HistoryDataConfig`/`HistoryPanel`, `attach_multi_segment_panel` signature, `orats_total_return_close` signature all match between their producing task and consuming tasks; `detail::date_to_nanos` declared in `orats_history.hpp` and reused in Tasks 4/5/6 tests. ✓

> **Note (Task 5 / digest):** `real_panel.cpp`'s `digest_panel` builds an `alpha::SignalSet` from the Panel's fields and calls `parallel::signal_set_digest`. The investigation surfaced two `SignalSet` definitions (the `alpha::` one in `panel.hpp` vs the digest's input), so the implementer must read the real `digest_panel` body before moving it. Per the pre-flight decision, **extract** it to a shared `data::digest_panel` (Task 5 Step 0) used by both `real_panel.cpp` and `history_panel.cpp` — one definition, no duplication, S1-5 digest unchanged (verified by re-running the real-panel E2E test in Step 0).
