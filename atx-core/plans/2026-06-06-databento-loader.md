# Databento EQUS.SUMMARY Loader — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add `atx::external::databento::load_equs_summary_zip(zip_path, dest_dir)` to atx-core: it unzips a Databento EQUS.SUMMARY batch `.zip`, zstd-decompresses each `.dbn.zst`, decodes the DBN OHLCV records, and writes a date-partitioned Parquet hive on disk.

**Architecture:** Three focused units — a pure DBN parser (`atx/external/dbn.hpp`), a reusable Parquet *writer* in `atx::core::io` (`parquet_writer.hpp`), and the loader orchestration (`atx/external/databento.hpp`) that glues zip + zstd + parser + writer. Two third-party C libs are added: miniz (vendored, PKZIP) and zstd (FetchContent, pinned v1.5.7). The DBN parser is dependency-free and unit-tested on fixture bytes; the loader's integration test builds a DBN→zstd→zip fixture entirely in-process.

**Tech Stack:** C++20, CMake + Ninja + clang-cl, Arrow/Parquet (vcpkg, behind PIMPL), miniz, zstd, GoogleTest. `Result<T>`/`Status` error handling; `/W4 /permissive- /WX`.

**Spec:** `atx-core/plans/databento-loader-design.md`.

---

## Ground-truth reference (verified against databento/dbn + databento-cpp)

DBN byte layout (version 2 and 3 — identical for the fields we read; little-endian throughout):

```
offset 0   : "DBN"                      (3 bytes magic)
offset 3   : version                    (u8; we accept 2 or 3)
offset 4   : frame_len                  (u32; size of the metadata block that follows)
offset 8   : --- fixed metadata block, exactly 100 bytes (METADATA_FIXED_LEN) ---
   +0   dataset           16 bytes (DATASET_CSTR_LEN, null-padded cstr)
   +16  schema            u16
   +18  start             u64 (ns)
   +26  end               u64 (ns)
   +34  limit             u64
   +42  stype_in          u8
   +43  stype_out         u8
   +44  ts_out            u8
   +45  symbol_cstr_len   u16   (call it L)
   +47  reserved          53 bytes (METADATA_RESERVED_LEN)   -> ends at +100
offset 108 : --- variable metadata sections ---
   schema_definition_len  u32   (expected 0; if non-zero, that many bytes follow & are skipped)
   symbols    : count u32, then count*L bytes
   partial    : count u32, then count*L bytes
   not_found  : count u32, then count*L bytes
   mappings   : count u32, then for each:
                  raw_symbol     L bytes (null-padded cstr)
                  interval_count u32
                  intervals[]    : start_date u32 (YYYYMMDD), end_date u32 (YYYYMMDD), symbol L bytes
offset 8 + frame_len : --- record stream begins ---
```

Records: each starts with a 16-byte `RecordHeader { length:u8 (×4 bytes), rtype:u8, publisher_id:u16, instrument_id:u32, ts_event:u64 }`. `OhlcvMsg` (rtype `0x23` ohlcv-1d / `0x24` ohlcv-eod) is 56 bytes (`length`==14): header + `open,high,low,close:i64` (1e-9 fixed-point) + `volume:u64`. Price sentinel `UNDEF = i64::MAX`. Mapping interval `symbol` field holds the instrument_id as a decimal string when `stype_out == instrument_id` (the EQUS.SUMMARY batch case); we invert it to build instrument_id → raw_symbol.

---

## File structure

| File | Responsibility |
|---|---|
| `atx-core/third-party/miniz/{miniz.h,miniz.c,PROVENANCE.md}` | Vendored PKZIP reader/writer (C). |
| `atx-core/include/atx/external/dbn.hpp` | DBN structs, `RType`, `DbnDecoder` (pure, no zip/zstd/Arrow). |
| `atx-core/src/external/dbn.cpp` | `DbnDecoder` implementation (metadata + record parse). |
| `atx-core/include/atx/core/io/parquet_writer.hpp` | `WriteColumn`, `write_parquet`, `write_hive_parquet` (Arrow-free surface). |
| `atx-core/src/io/parquet_writer.cpp` | Arrow table build + Parquet write + hive bucketing. |
| `atx-core/include/atx/external/databento.hpp` | `LoadStats`, `load_equs_summary_zip` (public entry). |
| `atx-core/src/external/databento.cpp` | zip + zstd glue + orchestration (only TU including miniz.h/zstd.h besides tests). |
| `atx-core/tests/dbn_fixture.hpp` | Test-only DBN/zstd/zip fixture builders. |
| `atx-core/tests/dbn_test.cpp` | DBN parser unit tests. |
| `atx-core/tests/parquet_writer_test.cpp` | Writer round-trip tests (read back via existing `LazyParquet`). |
| `atx-core/tests/databento_test.cpp` | End-to-end loader tests. |
| `atx-core/CMakeLists.txt` | Add `atx_miniz`, `find_package(zstd)`, new sources, links. |
| `atx-core/tests/CMakeLists.txt` | Add new test files; link `zstd::libzstd` + `atx_miniz` to tests. |
| `vcpkg.json` (root) | Add explicit `zstd` dependency (already installed transitively via Arrow). |

**Build/test commands** (this repo; an MSVC environment from `vcvars64.bat` must be active in the shell — the project configures with the `ninja` preset, binaryDir `build/`):

- Configure (only after CMake changes): `cmake --preset ninja`
- Build tests: `cmake --build build --target atx-core-tests`
- Run a subset: `build/bin/atx-core-tests.exe --gtest_filter='Dbn*'`
- Run all core tests: `ctest --test-dir build -R atx-core --output-on-failure`
- Format gate: `clang-format -i <changed files>` (then verify `git diff` is clean).

---

## Task 0: Create the isolated worktree

**No files yet** — this sets up the workspace.

- [ ] **Step 1: Create the worktree**

Use the `superpowers:using-git-worktrees` skill. Create a worktree off the current branch with a feature branch:

```bash
git worktree add ../atx-databento -b feat/databento-loader feat/atx-core-stdlib
```

- [ ] **Step 2: Confirm**

Run: `git -C ../atx-databento status`
Expected: on branch `feat/databento-loader`, clean tree. All subsequent work happens in `../atx-databento`.

---

## Task 1: Vendor miniz (PKZIP) as its own static lib

**Files:**
- Create: `atx-core/third-party/miniz/miniz.h`, `atx-core/third-party/miniz/miniz.c`, `atx-core/third-party/miniz/PROVENANCE.md`
- Modify: `atx-core/CMakeLists.txt`

- [ ] **Step 1: Download the miniz amalgamation**

Download the miniz 3.0.2 release amalgamation (the release zip contains pre-amalgamated `miniz.h` + `miniz.c`):

```bash
# from atx-core/third-party/
curl -L -o miniz.zip https://github.com/richgel999/miniz/releases/download/3.0.2/miniz-3.0.2.zip
# extract miniz.h and miniz.c into third-party/miniz/, discard the rest
```

Place exactly `miniz.h` and `miniz.c` under `atx-core/third-party/miniz/`.

- [ ] **Step 2: Record provenance**

Create `atx-core/third-party/miniz/PROVENANCE.md`:

```markdown
# miniz vendoring

- Library: miniz (single-file PKZIP / DEFLATE)
- Version: 3.0.2
- Source: https://github.com/richgel999/miniz/releases/download/3.0.2/miniz-3.0.2.zip
- Files vendored: miniz.h, miniz.c (the release amalgamation; other files discarded)
- SHA256 (miniz.zip): <fill from `Get-FileHash miniz.zip -Algorithm SHA256` at download time>
- License: MIT (see header of miniz.h)
- Why: read PKZIP archives (Databento batch .zip) without a heavy dependency.
```

Compute the hash with `Get-FileHash miniz.zip -Algorithm SHA256` and paste it into the `<...>` slot.

- [ ] **Step 3: Add the static lib to CMake**

In `atx-core/CMakeLists.txt`, immediately after the `atx_sqlite3` block (before `add_library(atx-core ...)`), add:

```cmake
# ---- Vendored miniz (third-party/miniz) ----
# PKZIP reader for the Databento batch .zip. Own static lib so its C source
# never sees /W4 /WX; linked PRIVATE into atx-core (symbols propagate, include
# dir does not -- miniz.h stays confined to the .cpp TUs and the test target).
add_library(atx_miniz STATIC third-party/miniz/miniz.c)
target_include_directories(atx_miniz PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/third-party/miniz>)
if(MSVC)
    target_compile_options(atx_miniz PRIVATE /w)
else()
    target_compile_options(atx_miniz PRIVATE -w)
endif()
```

- [ ] **Step 4: Configure to verify the lib builds**

Run: `cmake --preset ninja && cmake --build build --target atx_miniz`
Expected: `atx_miniz` builds with no errors.

- [ ] **Step 5: Commit**

```bash
git add atx-core/third-party/miniz atx-core/CMakeLists.txt
git commit -m "build(core): vendor miniz 3.0.2 (PKZIP reader) as atx_miniz"
```

---

## Task 2: Wire zstd (vcpkg) and link miniz/zstd

> zstd 1.5.7 is already installed in this repo's vcpkg tree (Arrow depends on it).
> Reusing the vcpkg zstd via `find_package` avoids a second build and links the
> SAME zstd Arrow uses (no symbol duplication). Targets exported by vcpkg:
> `zstd::libzstd` (generic INTERFACE alias) and `zstd::libzstd_shared` (SHARED).

**Files:**
- Modify: `vcpkg.json` (root), `atx-core/CMakeLists.txt`, `atx-core/tests/CMakeLists.txt`

- [ ] **Step 1: Declare zstd explicitly in the manifest**

In the root `vcpkg.json`, add `"zstd"` to `dependencies` (so it does not rely on Arrow transitively pulling it):

```json
  "dependencies": [
    { "name": "arrow", "features": ["parquet"] },
    "zstd"
  ],
```

- [ ] **Step 2: find_package + link into atx-core**

In `atx-core/CMakeLists.txt`, next to the existing Arrow/Parquet `find_package` calls, add:

```cmake
find_package(zstd CONFIG REQUIRED)
```

Then in the `target_link_libraries(atx-core ... PRIVATE ...)` block, add `atx_miniz`
and `zstd::libzstd` alongside `atx_sqlite3`:

```cmake
    PRIVATE
        atx_warnings
        atx_sqlite3
        atx_miniz
        zstd::libzstd
```

- [ ] **Step 3: Give the test target access to zstd + miniz headers**

In `atx-core/tests/CMakeLists.txt`, after the existing `target_link_libraries(... Arrow ...)` lines, add:

```cmake
# DBN/zip/zstd fixtures in the tests build .dbn.zst archives in-process, so the
# test target (first-party) links these directly -- their include dirs propagate
# to the tests only, never to atx-core's public consumers. (zstd::libzstd is the
# same vcpkg zstd Arrow already links; its DLL is copied by the existing
# TARGET_RUNTIME_DLLS post-build step.)
target_link_libraries(atx-core-tests PRIVATE atx_miniz zstd::libzstd)
```

- [ ] **Step 4: Configure**

Run: `cmake --preset ninja`
Expected: configures clean; `zstd::libzstd` target found. No errors.

- [ ] **Step 5: Commit**

```bash
git add vcpkg.json atx-core/CMakeLists.txt atx-core/tests/CMakeLists.txt
git commit -m "build(core): declare zstd (vcpkg) + link miniz/zstd into atx-core"
```

---

## Task 3: DBN parser (`atx/external/dbn`)

**Files:**
- Create: `atx-core/include/atx/external/dbn.hpp`, `atx-core/src/external/dbn.cpp`
- Create: `atx-core/tests/dbn_fixture.hpp` (shared fixture builder)
- Test: `atx-core/tests/dbn_test.cpp`
- Modify: `atx-core/CMakeLists.txt` (add `src/external/dbn.cpp`), `atx-core/tests/CMakeLists.txt` (add `dbn_test.cpp`)

### Subtask 3a: Structs, enum, and the header

- [ ] **Step 1: Write the header**

Create `atx-core/include/atx/external/dbn.hpp`:

```cpp
#pragma once

// atx::external::dbn — pure decoder for Databento Binary Encoding (DBN) v2/v3.
//
// No zip, no zstd, no Arrow: consumes an already-decompressed DBN byte span and
// yields OHLCV records (rtype 0x23 ohlcv-1d, 0x24 ohlcv-eod). Other record types
// are counted and skipped. Symbols resolve from the metadata symbol-mapping
// section -- no JSON sidecar. Layout: see atx-core/plans/2026-06-06-databento-loader.md.
//
// All multi-byte integers are little-endian. Prices are i64 in units of 1e-9;
// the missing-value sentinel is i64::MAX.

#include <cstddef>     // std::byte
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "atx/core/error.hpp"
#include "atx/core/types.hpp"

namespace atx::external::dbn {

using atx::core::Result;

inline constexpr i64 kFixedPriceScale = 1'000'000'000;                       // 1e-9 per unit
inline constexpr i64 kUndefPrice = std::numeric_limits<i64>::max();          // missing price

enum class RType : u8 {
  Ohlcv1S = 0x20,
  Ohlcv1M = 0x21,
  Ohlcv1H = 0x22,
  Ohlcv1D = 0x23,
  OhlcvEod = 0x24,
};

// 16-byte common record header.
struct RecordHeader {
  u8 length{};      // record size in 32-bit words (multiply by 4 for bytes)
  u8 rtype{};
  u16 publisher_id{};
  u32 instrument_id{};
  u64 ts_event{};   // ns since the UNIX epoch
};

// 56-byte OHLCV record (RecordHeader.length == 14).
struct OhlcvMsg {
  RecordHeader hd{};
  i64 open{};
  i64 high{};
  i64 low{};
  i64 close{};
  u64 volume{};
};

struct DbnMetadata {
  u8 version{};
  std::string dataset;
  u16 schema{};
  u16 symbol_cstr_len{};
};

// Streaming OHLCV reader over a contiguous DBN buffer (borrowed; the span must
// outlive the decoder). Move-only.
class DbnDecoder {
public:
  [[nodiscard]] static Result<DbnDecoder> open(std::span<const std::byte> dbn);

  DbnDecoder(DbnDecoder&&) noexcept = default;
  DbnDecoder& operator=(DbnDecoder&&) noexcept = default;
  DbnDecoder(const DbnDecoder&) = delete;
  DbnDecoder& operator=(const DbnDecoder&) = delete;
  ~DbnDecoder() = default;

  [[nodiscard]] const DbnMetadata& metadata() const noexcept { return meta_; }

  // instrument_id -> raw symbol from the metadata mappings; empty if unmapped.
  [[nodiscard]] std::string_view symbol_for(u32 instrument_id) const noexcept;

  // Next OHLCV record, skipping (and counting) unsupported rtypes. nullopt at end.
  [[nodiscard]] Result<std::optional<OhlcvMsg>> next();

  [[nodiscard]] i64 skipped_records() const noexcept { return skipped_; }

private:
  DbnDecoder() = default;

  std::span<const std::byte> buf_{};
  usize cursor_{0};                       // offset of the next record
  DbnMetadata meta_{};
  std::vector<std::pair<u32, std::string>> mappings_; // (instrument_id, raw_symbol)
  i64 skipped_{0};
};

} // namespace atx::external::dbn
```

- [ ] **Step 2: Write the failing struct-size test**

Create `atx-core/tests/dbn_test.cpp`:

```cpp
#include "atx/external/dbn.hpp"

#include <cstdint>

#include <gtest/gtest.h>

namespace dbn = atx::external::dbn;

TEST(DbnLayout, ConstantsMatchSpec) {
  // The wire-relevant constants must match the DBN spec. (Vocabulary types live
  // in namespace atx, not atx::external::dbn, so cast through std::uint8_t.)
  EXPECT_EQ(dbn::kFixedPriceScale, 1'000'000'000);
  EXPECT_EQ(static_cast<std::uint8_t>(dbn::RType::Ohlcv1D), 0x23);
  EXPECT_EQ(static_cast<std::uint8_t>(dbn::RType::OhlcvEod), 0x24);
}
```

- [ ] **Step 3: Add to CMake and run to verify it fails to build, then passes**

Add `src/external/dbn.cpp` to the `add_library(atx-core ...)` source list in `atx-core/CMakeLists.txt`, and `dbn_test.cpp` to `atx-core/tests/CMakeLists.txt`. Create a stub `atx-core/src/external/dbn.cpp`:

```cpp
#include "atx/external/dbn.hpp"
// Implementation added in subtasks 3b-3d.
```

Run: `cmake --preset ninja && cmake --build build --target atx-core-tests`
Expected: builds; `build/bin/atx-core-tests.exe --gtest_filter='DbnLayout.*'` PASSES (this subtask just establishes the header/compile path).

### Subtask 3b: Metadata parsing + the fixture builder

- [ ] **Step 1: Write the fixture builder**

Create `atx-core/tests/dbn_fixture.hpp` (test-only; encodes a valid DBN v2 stream and can zstd-compress / zip it):

```cpp
#pragma once

// Test-only builders: assemble a valid DBN v2 byte stream, zstd-compress it, and
// pack it into an in-memory PKZIP archive. Used by dbn_test and databento_test.

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <miniz.h>
#include <zstd.h>

namespace atx::test {

struct OhlcvRow {
  std::uint32_t instrument_id{};
  std::uint64_t ts_event{};   // ns
  std::int64_t open{}, high{}, low{}, close{};
  std::uint64_t volume{};
  std::uint8_t rtype{0x23};   // OHLCV_1D
};

struct SymMap {
  std::uint32_t instrument_id{};
  std::string raw_symbol;
  std::uint32_t start_date{}; // YYYYMMDD
  std::uint32_t end_date{};   // YYYYMMDD
};

namespace detail {
inline void put_u16(std::vector<std::byte>& b, std::uint16_t v) {
  b.push_back(std::byte(v & 0xFF));
  b.push_back(std::byte((v >> 8) & 0xFF));
}
inline void put_u32(std::vector<std::byte>& b, std::uint32_t v) {
  for (int i = 0; i < 4; ++i) b.push_back(std::byte((v >> (8 * i)) & 0xFF));
}
inline void put_u64(std::vector<std::byte>& b, std::uint64_t v) {
  for (int i = 0; i < 8; ++i) b.push_back(std::byte((v >> (8 * i)) & 0xFF));
}
inline void put_i64(std::vector<std::byte>& b, std::int64_t v) {
  put_u64(b, static_cast<std::uint64_t>(v));
}
inline void put_cstr(std::vector<std::byte>& b, std::string_view s, std::size_t len) {
  for (std::size_t i = 0; i < len; ++i)
    b.push_back(i < s.size() ? std::byte(s[i]) : std::byte(0));
}
} // namespace detail

// Build an uncompressed DBN v2 stream. symbol_cstr_len defaults small (32) to
// keep fixtures compact; the decoder reads it from the header, so any value works.
inline std::vector<std::byte>
build_dbn(const std::vector<SymMap>& maps, const std::vector<OhlcvRow>& rows,
          std::uint16_t symbol_cstr_len = 32) {
  using namespace detail;
  std::vector<std::byte> b;
  // Prefix.
  b.push_back(std::byte('D')); b.push_back(std::byte('B')); b.push_back(std::byte('N'));
  b.push_back(std::byte(2));           // version 2
  put_u32(b, 0);                       // frame_len placeholder (offset 4)
  const std::size_t meta_start = b.size(); // == 8

  // Fixed 100-byte block.
  put_cstr(b, "EQUS.SUMMARY", 16);     // dataset
  put_u16(b, 8);                       // schema = Ohlcv1D
  put_u64(b, 0);                       // start
  put_u64(b, 0);                       // end
  put_u64(b, 0);                       // limit
  b.push_back(std::byte(0));           // stype_in
  b.push_back(std::byte(0));           // stype_out
  b.push_back(std::byte(0));           // ts_out
  put_u16(b, symbol_cstr_len);         // symbol_cstr_len
  b.insert(b.end(), 53, std::byte(0)); // reserved -> fixed block now 100 bytes

  // Variable sections.
  put_u32(b, 0);                       // schema_definition_len
  put_u32(b, 0);                       // symbols count
  put_u32(b, 0);                       // partial count
  put_u32(b, 0);                       // not_found count
  put_u32(b, static_cast<std::uint32_t>(maps.size())); // mappings count
  for (const auto& m : maps) {
    put_cstr(b, m.raw_symbol, symbol_cstr_len);         // raw_symbol (key)
    put_u32(b, 1);                                      // interval_count
    put_u32(b, m.start_date);
    put_u32(b, m.end_date);
    put_cstr(b, std::to_string(m.instrument_id), symbol_cstr_len); // out = iid string
  }

  // Backpatch frame_len.
  const std::uint32_t frame_len = static_cast<std::uint32_t>(b.size() - meta_start);
  std::byte* p = b.data() + 4;
  for (int i = 0; i < 4; ++i) p[i] = std::byte((frame_len >> (8 * i)) & 0xFF);

  // Records.
  for (const auto& r : rows) {
    put_u8_record(b, r);
  }
  return b;
}

inline void put_u8_record(std::vector<std::byte>& b, const OhlcvRow& r) {
  using namespace detail;
  b.push_back(std::byte(14));                  // length (56/4)
  b.push_back(std::byte(r.rtype));             // rtype
  put_u16(b, 0);                               // publisher_id
  put_u32(b, r.instrument_id);
  put_u64(b, r.ts_event);
  put_i64(b, r.open);
  put_i64(b, r.high);
  put_i64(b, r.low);
  put_i64(b, r.close);
  put_u64(b, r.volume);
}

inline std::vector<std::byte> zstd_compress(std::span<const std::byte> in) {
  const std::size_t bound = ZSTD_compressBound(in.size());
  std::vector<std::byte> out(bound);
  const std::size_t n = ZSTD_compress(out.data(), bound, in.data(), in.size(), 3);
  out.resize(ZSTD_isError(n) ? 0 : n);
  return out;
}

// Build a PKZIP archive (STORE) from (name, bytes) entries.
inline std::vector<std::byte>
build_zip(const std::vector<std::pair<std::string, std::vector<std::byte>>>& entries) {
  mz_zip_archive zip{};
  mz_zip_writer_init_heap(&zip, 0, 0);
  for (const auto& [name, bytes] : entries) {
    mz_zip_writer_add_mem(&zip, name.c_str(), bytes.data(), bytes.size(),
                          MZ_NO_COMPRESSION);
  }
  void* buf = nullptr; std::size_t sz = 0;
  mz_zip_writer_finalize_heap_archive(&zip, &buf, &sz);
  std::vector<std::byte> out(static_cast<std::byte*>(buf),
                             static_cast<std::byte*>(buf) + sz);
  mz_free(buf);
  mz_zip_writer_end(&zip);
  return out;
}

} // namespace atx::test
```

> NOTE: declare `put_u8_record` before `build_dbn` (move its definition above, or forward-declare it). Reorder so it compiles; the body is as shown.

- [ ] **Step 2: Write the failing metadata test**

Append to `atx-core/tests/dbn_test.cpp`:

```cpp
#include "dbn_fixture.hpp"
#include <cstring>

TEST(DbnMetadata, ParsesHeaderAndDataset) {
  const auto bytes = atx::test::build_dbn(
      {{101, "AAPL", 20240101, 20250101}}, /*rows=*/{});
  auto dec = dbn::DbnDecoder::open(bytes);
  ASSERT_TRUE(dec.has_value()) << dec.error().to_string();
  EXPECT_EQ(dec->metadata().version, 2);
  EXPECT_EQ(dec->metadata().dataset, "EQUS.SUMMARY");
  EXPECT_EQ(dec->metadata().symbol_cstr_len, 32);
}

TEST(DbnMetadata, RejectsBadMagic) {
  std::vector<std::byte> bad(64, std::byte(0));
  bad[0] = std::byte('X');
  auto dec = dbn::DbnDecoder::open(bad);
  EXPECT_FALSE(dec.has_value());
}

TEST(DbnMetadata, ResolvesSymbol) {
  const auto bytes = atx::test::build_dbn(
      {{101, "AAPL", 20240101, 20250101}, {102, "MSFT", 20240101, 20250101}}, {});
  auto dec = dbn::DbnDecoder::open(bytes);
  ASSERT_TRUE(dec.has_value());
  EXPECT_EQ(dec->symbol_for(101), "AAPL");
  EXPECT_EQ(dec->symbol_for(102), "MSFT");
  EXPECT_TRUE(dec->symbol_for(999).empty());
}
```

- [ ] **Step 3: Run to verify it fails**

Run: `cmake --build build --target atx-core-tests && build/bin/atx-core-tests.exe --gtest_filter='DbnMetadata.*'`
Expected: FAIL — `open` returns a default/empty decoder (no parse yet).

- [ ] **Step 4: Implement metadata parsing**

Replace `atx-core/src/external/dbn.cpp` with:

```cpp
#include "atx/external/dbn.hpp"

#include <charconv>
#include <cstring>

namespace atx::external::dbn {

using atx::core::Err;
using atx::core::ErrorCode;
using atx::core::Ok;

namespace {

// Bounds-checked little-endian cursor over the borrowed buffer.
class Reader {
public:
  explicit Reader(std::span<const std::byte> b, usize off = 0) noexcept : b_{b}, off_{off} {}
  [[nodiscard]] usize pos() const noexcept { return off_; }
  [[nodiscard]] bool ok() const noexcept { return ok_; }
  void seek(usize off) noexcept { off_ = off; }
  void skip(usize n) noexcept { advance(n); }

  [[nodiscard]] u8 u8v() noexcept { return need(1) ? std::to_integer<u8>(b_[off_++]) : 0; }
  // NB: read the bytes in SEPARATE statements -- operands of `|` are unsequenced,
  // so packing two u8v() calls into one expression would read bytes in unspecified
  // order. Each compound assignment below is sequenced.
  [[nodiscard]] u16 u16v() noexcept {
    u16 v = u8v(); v = static_cast<u16>(v | (u16{u8v()} << 8)); return v;
  }
  [[nodiscard]] u32 u32v() noexcept {
    u32 v = u8v(); v |= u32{u8v()} << 8; v |= u32{u8v()} << 16; v |= u32{u8v()} << 24; return v;
  }
  [[nodiscard]] u64 u64v() noexcept {
    u64 v = 0;
    for (int i = 0; i < 8; ++i) v |= u64{u8v()} << (8 * i);
    return v;
  }
  [[nodiscard]] i64 i64v() noexcept { return static_cast<i64>(u64v()); }

  // Read a null-padded fixed-length cstr of `len` bytes.
  [[nodiscard]] std::string cstr(usize len) {
    if (!need(len)) { return {}; }
    const char* p = reinterpret_cast<const char*>(b_.data() + off_); // SAFETY: byte->char alias is permitted; len bytes are in-bounds (checked).
    const usize n = ::strnlen(p, len);
    std::string s(p, n);
    off_ += len;
    return s;
  }

private:
  [[nodiscard]] bool need(usize n) noexcept {
    if (off_ + n > b_.size()) { ok_ = false; return false; }
    return true;
  }
  void advance(usize n) noexcept { if (need(n)) off_ += n; }

  std::span<const std::byte> b_;
  usize off_{0};
  bool ok_{true};
};

constexpr usize kPrefixLen = 8;          // "DBN" + version + u32 frame_len
constexpr usize kFixedMetaLen = 100;     // METADATA_FIXED_LEN (v2/v3)
constexpr usize kDatasetLen = 16;        // DATASET_CSTR_LEN
constexpr usize kReservedLen = 53;       // METADATA_RESERVED_LEN (v2/v3)

} // namespace

Result<DbnDecoder> DbnDecoder::open(std::span<const std::byte> dbn) {
  if (dbn.size() < kPrefixLen) {
    return Err(ErrorCode::ParseError, "DBN: buffer shorter than prefix");
  }
  if (!(dbn[0] == std::byte('D') && dbn[1] == std::byte('B') && dbn[2] == std::byte('N'))) {
    return Err(ErrorCode::ParseError, "DBN: bad magic");
  }
  const u8 version = std::to_integer<u8>(dbn[3]);
  if (version != 2 && version != 3) {
    return Err(ErrorCode::NotImplemented, "DBN: only version 2/3 supported");
  }

  Reader r{dbn, 4};
  const u32 frame_len = r.u32v();                 // metadata length after this field
  const usize records_off = kPrefixLen + frame_len;
  if (frame_len < kFixedMetaLen || records_off > dbn.size()) {
    return Err(ErrorCode::ParseError, "DBN: metadata frame out of range");
  }

  DbnDecoder dec;
  dec.buf_ = dbn;
  dec.meta_.version = version;
  dec.meta_.dataset = r.cstr(kDatasetLen);
  dec.meta_.schema = r.u16v();
  (void)r.u64v();                                 // start
  (void)r.u64v();                                 // end
  (void)r.u64v();                                 // limit
  (void)r.u8v();                                  // stype_in
  (void)r.u8v();                                  // stype_out
  (void)r.u8v();                                  // ts_out
  dec.meta_.symbol_cstr_len = r.u16v();
  r.skip(kReservedLen);                           // reserved -> cursor at offset 108
  const u16 L = dec.meta_.symbol_cstr_len;
  if (L == 0) {
    return Err(ErrorCode::ParseError, "DBN: zero symbol_cstr_len");
  }

  // Variable sections.
  const u32 schema_def_len = r.u32v();
  r.skip(schema_def_len);
  for (int section = 0; section < 3; ++section) {  // symbols, partial, not_found
    const u32 count = r.u32v();
    r.skip(static_cast<usize>(count) * L);
  }
  const u32 map_count = r.u32v();
  for (u32 i = 0; i < map_count; ++i) {
    std::string raw = r.cstr(L);
    const u32 ivl = r.u32v();
    for (u32 j = 0; j < ivl; ++j) {
      (void)r.u32v();                              // start_date
      (void)r.u32v();                              // end_date
      const std::string out = r.cstr(L);           // instrument_id as string
      u32 iid = 0;
      const auto [ptr, ec] = std::from_chars(out.data(), out.data() + out.size(), iid);
      if (ec == std::errc{} && ptr == out.data() + out.size()) {
        dec.mappings_.emplace_back(iid, raw);      // last interval wins per (iid)
      }
    }
  }
  if (!r.ok()) {
    return Err(ErrorCode::ParseError, "DBN: truncated metadata");
  }

  dec.cursor_ = records_off;
  return Ok(std::move(dec));
}

std::string_view DbnDecoder::symbol_for(u32 instrument_id) const noexcept {
  for (const auto& [iid, sym] : mappings_) {
    if (iid == instrument_id) {
      return sym;
    }
  }
  return {};
}

Result<std::optional<OhlcvMsg>> DbnDecoder::next() {
  // Implemented in subtask 3d.
  return Ok(std::optional<OhlcvMsg>{});
}

} // namespace atx::external::dbn
```

- [ ] **Step 5: Run to verify it passes**

Run: `cmake --build build --target atx-core-tests && build/bin/atx-core-tests.exe --gtest_filter='DbnMetadata.*:DbnLayout.*'`
Expected: PASS.

### Subtask 3d: Record iteration + skip

- [ ] **Step 1: Write the failing record test**

Append to `atx-core/tests/dbn_test.cpp`:

```cpp
TEST(DbnRecords, DecodesOhlcvFieldsAndSkipsOthers) {
  using atx::test::OhlcvRow;
  std::vector<OhlcvRow> rows = {
      {101, 1'700'000'000'000'000'000ULL, 1000, 1100, 990, 1050, 12345, 0x23},
      {201, 1'700'000'000'000'000'000ULL, 0, 0, 0, 0, 0, 0x12 /*status: unsupported*/},
      {102, 1'700'086'400'000'000'000ULL, 2000, 2200, 1980, 2100, 6789, 0x24},
  };
  // Note: the 0x12 row is encoded with length 14 too (the builder always writes a
  // 56-byte record); the decoder must skip it by rtype and count it.
  const auto bytes = atx::test::build_dbn({{101, "AAPL", 20240101, 20250101}}, rows);
  auto dec = dbn::DbnDecoder::open(bytes);
  ASSERT_TRUE(dec.has_value());

  auto a = dec->next();
  ASSERT_TRUE(a.has_value()); ASSERT_TRUE(a->has_value());
  EXPECT_EQ((*a)->hd.instrument_id, 101u);
  EXPECT_EQ((*a)->open, 1000);
  EXPECT_EQ((*a)->close, 1050);
  EXPECT_EQ((*a)->volume, 12345u);

  auto b = dec->next();   // 0x12 skipped, returns the 0x24 row
  ASSERT_TRUE(b.has_value()); ASSERT_TRUE(b->has_value());
  EXPECT_EQ((*b)->hd.instrument_id, 102u);
  EXPECT_EQ((*b)->hd.rtype, 0x24);

  auto end = dec->next();
  ASSERT_TRUE(end.has_value());
  EXPECT_FALSE(end->has_value());          // exhausted
  EXPECT_EQ(dec->skipped_records(), 1);    // the 0x12 record
}
```

- [ ] **Step 2: Run to verify it fails**

Run: `cmake --build build --target atx-core-tests && build/bin/atx-core-tests.exe --gtest_filter='DbnRecords.*'`
Expected: FAIL — `next()` always returns nullopt.

- [ ] **Step 3: Implement `next()`**

Replace the stub `next()` in `atx-core/src/external/dbn.cpp` with:

```cpp
Result<std::optional<OhlcvMsg>> DbnDecoder::next() {
  while (cursor_ < buf_.size()) {
    if (cursor_ + 16 > buf_.size()) {
      return Err(ErrorCode::ParseError, "DBN: truncated record header");
    }
    const u8 length = std::to_integer<u8>(buf_[cursor_]);
    if (length == 0) {
      return Err(ErrorCode::ParseError, "DBN: zero-length record");
    }
    const usize rec_len = static_cast<usize>(length) * 4;
    if (cursor_ + rec_len > buf_.size()) {
      return Err(ErrorCode::ParseError, "DBN: record exceeds buffer");
    }
    const u8 rtype = std::to_integer<u8>(buf_[cursor_ + 1]);

    if (rtype == static_cast<u8>(RType::Ohlcv1D) || rtype == static_cast<u8>(RType::OhlcvEod)) {
      if (rec_len < 56) {
        return Err(ErrorCode::ParseError, "DBN: OHLCV record too short");
      }
      Reader r{buf_, cursor_};
      OhlcvMsg m{};
      m.hd.length = r.u8v();
      m.hd.rtype = r.u8v();
      m.hd.publisher_id = r.u16v();
      m.hd.instrument_id = r.u32v();
      m.hd.ts_event = r.u64v();
      m.open = r.i64v();
      m.high = r.i64v();
      m.low = r.i64v();
      m.close = r.i64v();
      m.volume = r.u64v();
      cursor_ += rec_len;
      if (!r.ok()) {
        return Err(ErrorCode::ParseError, "DBN: truncated OHLCV record");
      }
      return Ok(std::optional<OhlcvMsg>{m});
    }

    cursor_ += rec_len;   // skip unsupported rtype
    ++skipped_;
  }
  return Ok(std::optional<OhlcvMsg>{});
}
```

- [ ] **Step 4: Run to verify it passes**

Run: `cmake --build build --target atx-core-tests && build/bin/atx-core-tests.exe --gtest_filter='Dbn*'`
Expected: PASS (all DBN tests).

- [ ] **Step 5: Format + commit**

```bash
clang-format -i atx-core/include/atx/external/dbn.hpp atx-core/src/external/dbn.cpp atx-core/tests/dbn_test.cpp atx-core/tests/dbn_fixture.hpp
git add atx-core/include/atx/external/dbn.hpp atx-core/src/external/dbn.cpp atx-core/tests/dbn_test.cpp atx-core/tests/dbn_fixture.hpp atx-core/CMakeLists.txt atx-core/tests/CMakeLists.txt
git commit -m "feat(core): DBN v2/v3 OHLCV decoder (atx::external::dbn)"
```

---

## Task 4: Parquet writer (`atx::core::io`)

**Files:**
- Create: `atx-core/include/atx/core/io/parquet_writer.hpp`, `atx-core/src/io/parquet_writer.cpp`
- Test: `atx-core/tests/parquet_writer_test.cpp`
- Modify: `atx-core/CMakeLists.txt` (add `src/io/parquet_writer.cpp`), `atx-core/tests/CMakeLists.txt` (add `parquet_writer_test.cpp`)

### Subtask 4a: Flat writer

- [ ] **Step 1: Write the header**

Create `atx-core/include/atx/core/io/parquet_writer.hpp`:

```cpp
#pragma once

// atx::core::io — Parquet WRITE surface (companion to the read-side parquet.hpp).
// Arrow types stay in parquet_writer.cpp; this header includes no Arrow. Columns
// are borrowed spans (must outlive the call) and must all share the same length.

#include <span>
#include <string>
#include <string_view>
#include <variant>

#include "atx/core/datetime.hpp"  // time::Timestamp
#include "atx/core/error.hpp"
#include "atx/core/types.hpp"

namespace atx::core::io {

using atx::core::Result;
using atx::core::Status;

struct WriteColumn {
  std::string name;
  std::variant<std::span<const i64>,
               std::span<const f64>,
               std::span<const std::string>,
               std::span<const time::Timestamp>>
      data;
};

// Write all columns to one Parquet file at `path` (parent dirs created).
[[nodiscard]] Status write_parquet(std::span<const WriteColumn> cols, std::string_view path);

// Hive-partition by string column `partition_col`: bucket rows by its distinct
// values, DROP that column from each file (path-encoded), and write one file per
// bucket at <root>/<partition_col>=<value>/data.parquet. Returns partitions
// written. `partition_col` must name a std::string column in `cols`.
[[nodiscard]] Result<i64> write_hive_parquet(std::span<const WriteColumn> cols,
                                             std::string_view root,
                                             std::string_view partition_col);

} // namespace atx::core::io
```

- [ ] **Step 2: Write the failing round-trip test**

Create `atx-core/tests/parquet_writer_test.cpp`:

```cpp
#include "atx/core/io/parquet_writer.hpp"
#include "atx/core/io/parquet.hpp"

#include <filesystem>
#include <string>
#include <vector>

#include <gtest/gtest.h>

namespace io = atx::core::io;
namespace fs = std::filesystem;

TEST(ParquetWriter, FlatRoundTrip) {
  const std::vector<i64> a = {1, 2, 3};
  const std::vector<std::string> sym = {"AAPL", "MSFT", "NVDA"};
  std::vector<io::WriteColumn> cols = {
      {"v", std::span<const i64>(a)},
      {"sym", std::span<const std::string>(sym)},
  };
  const fs::path out = fs::temp_directory_path() / "atx_pqw_flat.parquet";
  fs::remove(out);
  ASSERT_TRUE(io::write_parquet(cols, out.string()).has_value());
  ASSERT_TRUE(fs::exists(out));

  auto t = io::read_parquet(out.string());
  ASSERT_TRUE(t.has_value());
  EXPECT_EQ(t->num_rows(), 3);
  EXPECT_EQ(t->num_columns(), 2);
}
```

- [ ] **Step 3: Run to verify it fails**

Add `src/io/parquet_writer.cpp` (stub) and `parquet_writer_test.cpp` to CMake. Stub:

```cpp
#include "atx/core/io/parquet_writer.hpp"
namespace atx::core::io {
Status write_parquet(std::span<const WriteColumn>, std::string_view) {
  return atx::core::Err(atx::core::ErrorCode::NotImplemented, "stub");
}
Result<i64> write_hive_parquet(std::span<const WriteColumn>, std::string_view, std::string_view) {
  return atx::core::Err(atx::core::ErrorCode::NotImplemented, "stub");
}
}
```

Run: `cmake --preset ninja && cmake --build build --target atx-core-tests && build/bin/atx-core-tests.exe --gtest_filter='ParquetWriter.FlatRoundTrip'`
Expected: FAIL — `write_parquet` returns NotImplemented.

- [ ] **Step 4: Implement the flat writer**

Replace `atx-core/src/io/parquet_writer.cpp` with:

```cpp
#include "atx/core/io/parquet_writer.hpp"

#include <arrow/builder.h>
#include <arrow/io/file.h>
#include <arrow/result.h>
#include <arrow/status.h>
#include <arrow/table.h>
#include <arrow/type.h>
#include <parquet/arrow/writer.h>

#include <filesystem>
#include <memory>
#include <vector>

namespace atx::core::io {

using atx::core::Error;
using atx::core::ErrorCode;

namespace {

[[nodiscard]] Error from_arrow(const arrow::Status& s, std::string_view ctx) {
  std::string msg{ctx};
  msg += ": ";
  msg += s.ToString();
  if (s.IsIOError()) return Error{ErrorCode::IoError, std::move(msg)};
  if (s.IsInvalid()) return Error{ErrorCode::ParseError, std::move(msg)};
  return Error{ErrorCode::Internal, std::move(msg)};
}

[[nodiscard]] usize column_rows(const WriteColumn& c) noexcept {
  return std::visit([](auto&& s) { return s.size(); }, c.data);
}

// Append one column's rows (at the given indices) into a freshly-built array.
// Returns the field + array, or an arrow error.
[[nodiscard]] arrow::Result<std::shared_ptr<arrow::Array>>
build_array(const WriteColumn& c, std::span<const usize> rows) {
  std::shared_ptr<arrow::Array> out;
  auto append_all = [&](auto& builder, auto fn) -> arrow::Status {
    ARROW_RETURN_NOT_OK(builder.Reserve(static_cast<i64>(rows.size())));
    for (usize idx : rows) ARROW_RETURN_NOT_OK(fn(builder, idx));
    return builder.Finish(&out);
  };
  arrow::Status st;
  std::visit(
      [&](auto&& s) {
        using T = std::remove_const_t<typename std::decay_t<decltype(s)>::value_type>;
        if constexpr (std::is_same_v<T, i64>) {
          arrow::Int64Builder b;
          st = append_all(b, [&](auto& bb, usize i) { return bb.Append(s[i]); });
        } else if constexpr (std::is_same_v<T, f64>) {
          arrow::DoubleBuilder b;
          st = append_all(b, [&](auto& bb, usize i) { return bb.Append(s[i]); });
        } else if constexpr (std::is_same_v<T, std::string>) {
          arrow::StringBuilder b;
          st = append_all(b, [&](auto& bb, usize i) { return bb.Append(s[i]); });
        } else { // time::Timestamp
          arrow::TimestampBuilder b{arrow::timestamp(arrow::TimeUnit::NANO),
                                    arrow::default_memory_pool()};
          st = append_all(b, [&](auto& bb, usize i) { return bb.Append(s[i].unix_nanos()); });
        }
      },
      c.data);
  if (!st.ok()) return st;
  return out;
}

[[nodiscard]] std::shared_ptr<arrow::DataType> arrow_type(const WriteColumn& c) {
  return std::visit(
      [](auto&& s) -> std::shared_ptr<arrow::DataType> {
        using T = std::remove_const_t<typename std::decay_t<decltype(s)>::value_type>;
        if constexpr (std::is_same_v<T, i64>) return arrow::int64();
        else if constexpr (std::is_same_v<T, f64>) return arrow::float64();
        else if constexpr (std::is_same_v<T, std::string>) return arrow::utf8();
        else return arrow::timestamp(arrow::TimeUnit::NANO);
      },
      c.data);
}

// Build an arrow::Table from the columns whose name != skip, taking the given rows.
[[nodiscard]] arrow::Result<std::shared_ptr<arrow::Table>>
build_table(std::span<const WriteColumn> cols, std::span<const usize> rows,
            std::string_view skip) {
  std::vector<std::shared_ptr<arrow::Field>> fields;
  std::vector<std::shared_ptr<arrow::Array>> arrays;
  for (const auto& c : cols) {
    if (c.name == skip) continue;
    fields.push_back(arrow::field(c.name, arrow_type(c), /*nullable=*/false));
    ARROW_ASSIGN_OR_RAISE(auto arr, build_array(c, rows));
    arrays.push_back(std::move(arr));
  }
  return arrow::Table::Make(arrow::schema(fields), arrays,
                            static_cast<i64>(rows.size()));
}

[[nodiscard]] Status write_table(const std::shared_ptr<arrow::Table>& table,
                                 const std::string& path) {
  std::error_code ec;
  std::filesystem::create_directories(std::filesystem::path{path}.parent_path(), ec);
  auto sink_r = arrow::io::FileOutputStream::Open(path);
  if (!sink_r.ok()) return atx::core::Err(from_arrow(sink_r.status(), "open output"));
  auto st = parquet::arrow::WriteTable(*table, arrow::default_memory_pool(),
                                       *sink_r, /*chunk_size=*/1 << 20);
  if (!st.ok()) return atx::core::Err(from_arrow(st, "write parquet"));
  return atx::core::Ok();
}

[[nodiscard]] std::vector<usize> all_rows(usize n) {
  std::vector<usize> v(n);
  for (usize i = 0; i < n; ++i) v[i] = i;
  return v;
}

} // namespace

Status write_parquet(std::span<const WriteColumn> cols, std::string_view path) {
  if (cols.empty()) return atx::core::Err(ErrorCode::InvalidArgument, "no columns");
  const usize n = column_rows(cols.front());
  for (const auto& c : cols) {
    if (column_rows(c) != n) {
      return atx::core::Err(ErrorCode::InvalidArgument, "column length mismatch");
    }
  }
  const auto rows = all_rows(n);
  auto table = build_table(cols, rows, /*skip=*/"");
  if (!table.ok()) return atx::core::Err(from_arrow(table.status(), "build table"));
  return write_table(*table, std::string{path});
}

Result<i64> write_hive_parquet(std::span<const WriteColumn> cols, std::string_view root,
                               std::string_view partition_col) {
  // Implemented in subtask 4b.
  (void)cols; (void)root; (void)partition_col;
  return atx::core::Err(ErrorCode::NotImplemented, "stub");
}

} // namespace atx::core::io
```

- [ ] **Step 5: Run to verify it passes**

Run: `cmake --build build --target atx-core-tests && build/bin/atx-core-tests.exe --gtest_filter='ParquetWriter.FlatRoundTrip'`
Expected: PASS.

### Subtask 4b: Hive writer

- [ ] **Step 1: Write the failing hive test**

Append to `atx-core/tests/parquet_writer_test.cpp`:

```cpp
TEST(ParquetWriter, HivePartitionsByDate) {
  const std::vector<i64> close = {10, 11, 12, 13};
  const std::vector<std::string> date = {"2024-01-02", "2024-01-02", "2024-01-03", "2024-01-03"};
  std::vector<io::WriteColumn> cols = {
      {"close", std::span<const i64>(close)},
      {"date", std::span<const std::string>(date)},
  };
  const fs::path root = fs::temp_directory_path() / "atx_pqw_hive";
  fs::remove_all(root);
  auto n = io::write_hive_parquet(cols, root.string(), "date");
  ASSERT_TRUE(n.has_value()) << n.error().to_string();
  EXPECT_EQ(*n, 2);
  EXPECT_TRUE(fs::exists(root / "date=2024-01-02" / "data.parquet"));
  EXPECT_TRUE(fs::exists(root / "date=2024-01-03" / "data.parquet"));

  // partition column is dropped from the file; only "close" remains.
  auto t = io::read_parquet((root / "date=2024-01-02" / "data.parquet").string());
  ASSERT_TRUE(t.has_value());
  EXPECT_EQ(t->num_rows(), 2);
  EXPECT_EQ(t->num_columns(), 1);
  EXPECT_EQ(t->schema().columns.at(0).name, "close");
}

TEST(ParquetWriter, HiveRejectsMissingPartitionColumn) {
  const std::vector<i64> v = {1};
  std::vector<io::WriteColumn> cols = {{"v", std::span<const i64>(v)}};
  auto n = io::write_hive_parquet(cols, "ignored", "nope");
  EXPECT_FALSE(n.has_value());
}
```

- [ ] **Step 2: Run to verify it fails**

Run: `cmake --build build --target atx-core-tests && build/bin/atx-core-tests.exe --gtest_filter='ParquetWriter.Hive*'`
Expected: FAIL — stub returns NotImplemented.

- [ ] **Step 3: Implement `write_hive_parquet`**

Replace the `write_hive_parquet` stub in `atx-core/src/io/parquet_writer.cpp` with (and add `#include <unordered_map>` at the top):

```cpp
Result<i64> write_hive_parquet(std::span<const WriteColumn> cols, std::string_view root,
                               std::string_view partition_col) {
  if (cols.empty()) return atx::core::Err(ErrorCode::InvalidArgument, "no columns");
  const usize n = column_rows(cols.front());
  for (const auto& c : cols) {
    if (column_rows(c) != n) {
      return atx::core::Err(ErrorCode::InvalidArgument, "column length mismatch");
    }
  }
  // Locate the partition column; it must be a std::string column.
  const WriteColumn* pcol = nullptr;
  for (const auto& c : cols) {
    if (c.name == partition_col) { pcol = &c; break; }
  }
  if (pcol == nullptr) {
    return atx::core::Err(ErrorCode::InvalidArgument, "partition column not found");
  }
  const auto* pvals = std::get_if<std::span<const std::string>>(&pcol->data);
  if (pvals == nullptr) {
    return atx::core::Err(ErrorCode::InvalidArgument, "partition column must be string");
  }

  // Bucket row indices by partition value, preserving first-seen order.
  std::unordered_map<std::string_view, usize> index;       // value -> bucket idx
  std::vector<std::string> values;                          // bucket idx -> value
  std::vector<std::vector<usize>> buckets;
  for (usize i = 0; i < n; ++i) {
    const std::string& v = (*pvals)[i];
    auto it = index.find(v);
    if (it == index.end()) {
      index.emplace(v, buckets.size());
      values.push_back(v);
      buckets.emplace_back();
      buckets.back().push_back(i);
    } else {
      buckets[it->second].push_back(i);
    }
  }

  for (usize b = 0; b < buckets.size(); ++b) {
    auto table = build_table(cols, buckets[b], /*skip=*/partition_col);
    if (!table.ok()) return atx::core::Err(from_arrow(table.status(), "build table"));
    const std::string path = std::string{root} + "/" + std::string{partition_col} + "=" +
                             values[b] + "/data.parquet";
    auto st = write_table(*table, path);
    if (!st.has_value()) return atx::core::Err(st.error());
  }
  return atx::core::Ok(static_cast<i64>(buckets.size()));
}
```

- [ ] **Step 4: Run to verify it passes**

Run: `cmake --build build --target atx-core-tests && build/bin/atx-core-tests.exe --gtest_filter='ParquetWriter.*'`
Expected: PASS.

- [ ] **Step 5: Format + commit**

```bash
clang-format -i atx-core/include/atx/core/io/parquet_writer.hpp atx-core/src/io/parquet_writer.cpp atx-core/tests/parquet_writer_test.cpp
git add atx-core/include/atx/core/io/parquet_writer.hpp atx-core/src/io/parquet_writer.cpp atx-core/tests/parquet_writer_test.cpp atx-core/CMakeLists.txt atx-core/tests/CMakeLists.txt
git commit -m "feat(core): hive Parquet writer (atx::core::io::write_hive_parquet)"
```

---

## Task 5: Loader orchestration (`atx::external::databento`)

**Files:**
- Create: `atx-core/include/atx/external/databento.hpp`, `atx-core/src/external/databento.cpp`
- Test: `atx-core/tests/databento_test.cpp`
- Modify: `atx-core/CMakeLists.txt` (add `src/external/databento.cpp`), `atx-core/tests/CMakeLists.txt` (add `databento_test.cpp`)

### Subtask 5a: Header + happy-path

- [ ] **Step 1: Write the header**

Create `atx-core/include/atx/external/databento.hpp`:

```cpp
#pragma once

// atx::external::databento — ingest a Databento EQUS.SUMMARY batch .zip into a
// date-partitioned Parquet hive on disk.
//
// The zip holds zstd-compressed DBN files (*.dbn.zst). This loader unzips,
// zstd-decompresses, decodes the DBN OHLCV records (see atx::external::dbn), and
// writes <dest_dir>/date=YYYY-MM-DD/data.parquet via atx::core::io. Self-contained:
// no network, no Python, no JSON dependency.
//
// Output schema per file: ts:timestamp[ns], symbol:string, open/high/low/close:i64
// (units of 1e-9), volume:i64. The partition column "date" is encoded in the path.
// Prices use Databento's native 1e-9 fixed-point (divide by 1e9 for dollars).

#include <string_view>

#include "atx/core/error.hpp"
#include "atx/core/types.hpp"

namespace atx::external::databento {

using atx::core::Result;

struct LoadStats {
  i64 files_processed{0};    // .dbn.zst entries decoded
  i64 records_decoded{0};    // OHLCV rows written
  i64 partitions_written{0}; // distinct date= partitions
  i64 records_skipped{0};    // records of unsupported rtype
};

// Decode `zip_path` into <dest_dir>/date=YYYY-MM-DD/data.parquet. Err on missing/
// malformed input or if zero OHLCV records are found.
[[nodiscard]] Result<LoadStats>
load_equs_summary_zip(std::string_view zip_path, std::string_view dest_dir);

} // namespace atx::external::databento
```

- [ ] **Step 2: Write the failing happy-path test**

Create `atx-core/tests/databento_test.cpp`:

```cpp
#include "atx/external/databento.hpp"
#include "atx/core/io/parquet.hpp"
#include "dbn_fixture.hpp"

#include <filesystem>
#include <vector>

#include <gtest/gtest.h>

namespace db = atx::external::databento;
namespace io = atx::core::io;
namespace fs = std::filesystem;

namespace {
// ts in ns for 2024-01-02 and 2024-01-03 at 00:00:00 UTC.
constexpr std::uint64_t kJan02 = 1'704'153'600'000'000'000ULL;
constexpr std::uint64_t kJan03 = 1'704'240'000'000'000'000ULL;

fs::path write_fixture_zip(const std::string& name) {
  using atx::test::OhlcvRow;
  std::vector<OhlcvRow> rows = {
      {101, kJan02, 1000, 1100, 990, 1050, 5000, 0x23},
      {102, kJan02, 2000, 2200, 1980, 2100, 6000, 0x23},
      {101, kJan03, 1050, 1080, 1040, 1075, 5500, 0x23},
  };
  auto dbn = atx::test::build_dbn(
      {{101, "AAPL", 20240101, 20250101}, {102, "MSFT", 20240101, 20250101}}, rows);
  auto zst = atx::test::zstd_compress(dbn);
  auto zip = atx::test::build_zip({{"equs-summary-ohlcv-1d.dbn.zst", zst}});
  const fs::path p = fs::temp_directory_path() / name;
  std::ofstream os(p, std::ios::binary);
  os.write(reinterpret_cast<const char*>(zip.data()),
           static_cast<std::streamsize>(zip.size()));
  return p;
}
} // namespace

TEST(Databento, LoadsZipIntoDatePartitions) {
  const fs::path zip = write_fixture_zip("atx_db_load.zip");
  const fs::path dest = fs::temp_directory_path() / "atx_db_dest";
  fs::remove_all(dest);

  auto stats = db::load_equs_summary_zip(zip.string(), dest.string());
  ASSERT_TRUE(stats.has_value()) << stats.error().to_string();
  EXPECT_EQ(stats->records_decoded, 3);
  EXPECT_EQ(stats->partitions_written, 2);
  EXPECT_EQ(stats->files_processed, 1);

  EXPECT_TRUE(fs::exists(dest / "date=2024-01-02" / "data.parquet"));
  EXPECT_TRUE(fs::exists(dest / "date=2024-01-03" / "data.parquet"));

  auto t = io::read_parquet((dest / "date=2024-01-02" / "data.parquet").string());
  ASSERT_TRUE(t.has_value());
  EXPECT_EQ(t->num_rows(), 2);
  auto syms = t->strings("symbol");
  ASSERT_TRUE(syms.has_value());
  EXPECT_EQ(syms->size(), 2u);
}
```

Add `#include <fstream>` to the test.

- [ ] **Step 3: Run to verify it fails**

Add the stub `atx-core/src/external/databento.cpp` and wire CMake (source + test):

```cpp
#include "atx/external/databento.hpp"
namespace atx::external::databento {
Result<LoadStats> load_equs_summary_zip(std::string_view, std::string_view) {
  return atx::core::Err(atx::core::ErrorCode::NotImplemented, "stub");
}
}
```

Run: `cmake --preset ninja && cmake --build build --target atx-core-tests && build/bin/atx-core-tests.exe --gtest_filter='Databento.LoadsZipIntoDatePartitions'`
Expected: FAIL — NotImplemented.

- [ ] **Step 4: Implement the loader**

Replace `atx-core/src/external/databento.cpp` with:

```cpp
#include "atx/external/databento.hpp"

#include <miniz.h>
#include <zstd.h>

#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#include "atx/core/datetime.hpp"
#include "atx/core/io/parquet_writer.hpp"
#include "atx/external/dbn.hpp"

namespace atx::external::databento {

using atx::core::Err;
using atx::core::ErrorCode;
using atx::core::Ok;
namespace time = atx::core::time;

namespace {

[[nodiscard]] bool ends_with(std::string_view s, std::string_view suf) noexcept {
  return s.size() >= suf.size() && s.substr(s.size() - suf.size()) == suf;
}

// Decompress a single zstd frame (or a buffer of concatenated frames) into bytes.
[[nodiscard]] Result<std::vector<std::byte>> zstd_decompress(std::span<const std::byte> in) {
  ZSTD_DStream* ds = ZSTD_createDStream();
  if (ds == nullptr) return Err(ErrorCode::Internal, "zstd: alloc DStream");
  ZSTD_initDStream(ds);
  ZSTD_inBuffer ib{in.data(), in.size(), 0};
  std::vector<std::byte> out;
  const std::size_t chunk = ZSTD_DStreamOutSize();
  std::vector<std::byte> tmp(chunk);
  while (ib.pos < ib.size) {
    ZSTD_outBuffer ob{tmp.data(), tmp.size(), 0};
    const std::size_t rc = ZSTD_decompressStream(ds, &ob, &ib);
    if (ZSTD_isError(rc)) {
      ZSTD_freeDStream(ds);
      return Err(ErrorCode::ParseError, std::string{"zstd: "} + ZSTD_getErrorName(rc));
    }
    out.insert(out.end(), tmp.begin(), tmp.begin() + ob.pos);
    if (rc == 0 && ib.pos == ib.size) break;
  }
  ZSTD_freeDStream(ds);
  return Ok(std::move(out));
}

// Format ts_event (ns) as a "YYYY-MM-DD" partition value (UTC civil date).
[[nodiscard]] std::string date_string(u64 ts_event_ns) {
  const auto ct = time::to_civil_utc(time::Timestamp::from_unix_nanos(static_cast<i64>(ts_event_ns)));
  char buf[16];
  std::snprintf(buf, sizeof(buf), "%04d-%02u-%02u", ct.date.year, ct.date.month, ct.date.day);
  return std::string{buf};
}

struct Columns {
  std::vector<time::Timestamp> ts;
  std::vector<std::string> symbol;
  std::vector<i64> open, high, low, close, volume;
  std::vector<std::string> date;
};

// Decode one decompressed DBN buffer into the column accumulators.
[[nodiscard]] Result<void> decode_into(std::span<const std::byte> dbn, Columns& c,
                                       i64& decoded, i64& skipped) {
  ATX_TRY(auto dec, dbn::DbnDecoder::open(dbn));
  for (;;) {
    ATX_TRY(auto rec, dec.next());
    if (!rec.has_value()) break;
    const auto& m = *rec;
    c.ts.push_back(time::Timestamp::from_unix_nanos(static_cast<i64>(m.hd.ts_event)));
    std::string_view sym = dec.symbol_for(m.hd.instrument_id);
    c.symbol.push_back(sym.empty() ? std::to_string(m.hd.instrument_id) : std::string{sym});
    c.open.push_back(m.open);
    c.high.push_back(m.high);
    c.low.push_back(m.low);
    c.close.push_back(m.close);
    c.volume.push_back(m.volume > static_cast<u64>(std::numeric_limits<i64>::max())
                           ? std::numeric_limits<i64>::max()
                           : static_cast<i64>(m.volume));
    c.date.push_back(date_string(m.hd.ts_event));
    ++decoded;
  }
  skipped += dec.skipped_records();
  return Ok();
}

} // namespace

Result<LoadStats> load_equs_summary_zip(std::string_view zip_path, std::string_view dest_dir) {
  namespace fs = std::filesystem;
  if (!fs::exists(fs::path{zip_path})) {
    return Err(ErrorCode::IoError, std::string{"zip not found: "} + std::string{zip_path});
  }

  mz_zip_archive zip{};
  if (!mz_zip_reader_init_file(&zip, std::string{zip_path}.c_str(), 0)) {
    return Err(ErrorCode::ParseError, "failed to open zip archive");
  }

  Columns cols;
  LoadStats stats;
  const mz_uint count = mz_zip_reader_get_num_files(&zip);
  for (mz_uint i = 0; i < count; ++i) {
    mz_zip_archive_file_stat st{};
    if (!mz_zip_reader_file_stat(&zip, i, &st)) continue;
    const std::string_view name{st.m_filename};
    const bool is_zst = ends_with(name, ".dbn.zst");
    const bool is_raw = ends_with(name, ".dbn");
    if (!is_zst && !is_raw) continue;

    std::size_t raw_size = 0;
    void* raw = mz_zip_reader_extract_to_heap(&zip, i, &raw_size, 0);
    if (raw == nullptr) {
      mz_zip_reader_end(&zip);
      return Err(ErrorCode::ParseError, std::string{"failed to extract "} + std::string{name});
    }
    std::span<const std::byte> entry{static_cast<const std::byte*>(raw), raw_size};

    Result<void> step = Ok();
    if (is_zst) {
      auto dec = zstd_decompress(entry);
      if (!dec.has_value()) { mz_free(raw); mz_zip_reader_end(&zip); return Err(dec.error()); }
      step = decode_into(*dec, cols, stats.records_decoded, stats.records_skipped);
    } else {
      step = decode_into(entry, cols, stats.records_decoded, stats.records_skipped);
    }
    mz_free(raw);
    if (!step.has_value()) { mz_zip_reader_end(&zip); return Err(step.error()); }
    ++stats.files_processed;
  }
  mz_zip_reader_end(&zip);

  if (cols.ts.empty()) {
    return Err(ErrorCode::InvalidArgument, "no OHLCV records found in archive");
  }

  const std::vector<atx::core::io::WriteColumn> wcols = {
      {"ts", std::span<const time::Timestamp>(cols.ts)},
      {"symbol", std::span<const std::string>(cols.symbol)},
      {"open", std::span<const i64>(cols.open)},
      {"high", std::span<const i64>(cols.high)},
      {"low", std::span<const i64>(cols.low)},
      {"close", std::span<const i64>(cols.close)},
      {"volume", std::span<const i64>(cols.volume)},
      {"date", std::span<const std::string>(cols.date)},
  };
  ATX_TRY(auto nparts, atx::core::io::write_hive_parquet(wcols, dest_dir, "date"));
  stats.partitions_written = nparts;
  return Ok(stats);
}

} // namespace atx::external::databento
```

Add `#include <limits>` and `#include <span>` to the top of the file.

- [ ] **Step 5: Run to verify it passes**

Run: `cmake --build build --target atx-core-tests && build/bin/atx-core-tests.exe --gtest_filter='Databento.LoadsZipIntoDatePartitions'`
Expected: PASS.

### Subtask 5b: Edge cases

- [ ] **Step 1: Write the failing edge-case tests**

Append to `atx-core/tests/databento_test.cpp`:

```cpp
TEST(Databento, MissingZipErrors) {
  auto stats = db::load_equs_summary_zip("c:/no/such/file.zip", "ignored");
  ASSERT_FALSE(stats.has_value());
  EXPECT_EQ(stats.error().code(), atx::core::ErrorCode::IoError);
}

TEST(Databento, ZeroOhlcvErrors) {
  // A DBN with only an unsupported rtype -> zero OHLCV decoded -> error.
  using atx::test::OhlcvRow;
  std::vector<OhlcvRow> rows = {{1, kJan02, 0, 0, 0, 0, 0, 0x12}};
  auto dbn = atx::test::build_dbn({{1, "X", 20240101, 20250101}}, rows);
  auto zst = atx::test::zstd_compress(dbn);
  auto zip = atx::test::build_zip({{"x.dbn.zst", zst}});
  const fs::path p = fs::temp_directory_path() / "atx_db_zero.zip";
  std::ofstream os(p, std::ios::binary);
  os.write(reinterpret_cast<const char*>(zip.data()),
           static_cast<std::streamsize>(zip.size()));
  os.close();

  auto stats = db::load_equs_summary_zip(p.string(), (fs::temp_directory_path() / "atx_db_zero_dest").string());
  ASSERT_FALSE(stats.has_value());
  EXPECT_EQ(stats.error().code(), atx::core::ErrorCode::InvalidArgument);
}

TEST(Databento, SkipsUnsupportedAndCounts) {
  using atx::test::OhlcvRow;
  std::vector<OhlcvRow> rows = {
      {1, kJan02, 10, 11, 9, 10, 100, 0x23},
      {1, kJan02, 0, 0, 0, 0, 0, 0x12},   // skipped
  };
  auto dbn = atx::test::build_dbn({{1, "X", 20240101, 20250101}}, rows);
  auto zst = atx::test::zstd_compress(dbn);
  auto zip = atx::test::build_zip({{"x.dbn.zst", zst}});
  const fs::path p = fs::temp_directory_path() / "atx_db_skip.zip";
  std::ofstream os(p, std::ios::binary);
  os.write(reinterpret_cast<const char*>(zip.data()),
           static_cast<std::streamsize>(zip.size()));
  os.close();

  auto stats = db::load_equs_summary_zip(p.string(), (fs::temp_directory_path() / "atx_db_skip_dest").string());
  ASSERT_TRUE(stats.has_value()) << stats.error().to_string();
  EXPECT_EQ(stats->records_decoded, 1);
  EXPECT_EQ(stats->records_skipped, 1);
}
```

- [ ] **Step 2: Run to verify behavior**

Run: `cmake --build build --target atx-core-tests && build/bin/atx-core-tests.exe --gtest_filter='Databento.*'`
Expected: `MissingZipErrors`, `ZeroOhlcvErrors`, `SkipsUnsupportedAndCounts` all PASS (the implementation from 5a already covers these paths). If any fail, fix the loader, not the test.

- [ ] **Step 3: Format + commit**

```bash
clang-format -i atx-core/include/atx/external/databento.hpp atx-core/src/external/databento.cpp atx-core/tests/databento_test.cpp
git add atx-core/include/atx/external/databento.hpp atx-core/src/external/databento.cpp atx-core/tests/databento_test.cpp atx-core/CMakeLists.txt atx-core/tests/CMakeLists.txt
git commit -m "feat(core): Databento EQUS.SUMMARY zip->parquet loader (atx::external::databento)"
```

---

## Task 6: Full-suite gate

**Files:** none (verification only).

- [ ] **Step 1: Build the whole test target clean under /W4 /WX**

Run: `cmake --build build --target atx-core-tests`
Expected: zero warnings, zero errors (warnings-as-errors).

- [ ] **Step 2: Run the entire core suite**

Run: `ctest --test-dir build -R atx-core --output-on-failure`
Expected: all tests pass (the pre-existing suite plus the new `Dbn*`, `ParquetWriter*`, `Databento*` cases).

- [ ] **Step 3: Verify formatting is idempotent**

Run: `clang-format -i` over all new/changed files, then `git diff --exit-code`
Expected: no diff.

- [ ] **Step 4: Final review commit (if Step 3 produced changes)**

```bash
git add -A
git commit -m "style(core): clang-format databento loader"
```

---

## Notes / deferred (out of scope; see spec)

- v1 DBN (older record_count layout, symbol_cstr_len 22) is rejected with `NotImplemented`; add if a v1 file appears.
- Non-OHLCV schemas (trades, mbp, statistics, status, definition) are skipped+counted.
- The loader buffers all rows before writing; for archives too large to fit in memory, add per-date streaming.
- Prices are stored as raw i64 (1e-9). An f64/Decimal option can be added behind a flag.
- Optional later: vendor a small real `ohlcv-1d.dbn.zst` from databento/dbn test data as an independent (real-encoder) golden fixture under `atx-core/tests/data/`.
