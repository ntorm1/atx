# atx-tsdb — Shared-Memory In-Memory TSDB — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A new monorepo binary project, `atx-tsdb`, that loads an immutable historical dataset once from parquet into a sealed file, which many backtest reader processes `mmap` read-only (page-cache-shared, zero-copy), plus an `IDataHandler` adapter (`ShmBarFeed`) that lets the existing engine backtest loop run unchanged off shared memory.

**Architecture:** Dense panel grid `[field][time][instrument]` of f64 + a present-bitmap, written to a position-independent sealed file (offsets-only, never raw pointers). Readers map it read-only and address cells with pure arithmetic. Determinism, no-look-ahead, and no-survivorship-bias are preserved; the file-backed mapping is shared across processes by the OS page cache.

**Tech Stack:** C++20, CMake + Ninja + clang-cl, GoogleTest, atx-core (`error`, `types`, `hash`, `io/parquet`, `domain`, `datetime`), Win32 (`CreateFileMapping`/`MapViewOfFile`) + POSIX (`mmap`) behind one `Mapping` seam.

**Spec:** [docs/superpowers/specs/2026-06-04-atx-tsdb-shm-engine-design.md](../specs/2026-06-04-atx-tsdb-shm-engine-design.md)

---

## Conventions (read once)

- **Namespace:** `atx::tsdb`. Headers under `atx-tsdb/include/atx/tsdb/`, sources under `atx-tsdb/src/`, tests under `atx-tsdb/tests/*_test.cpp` (auto-globbed), tools under `atx-tsdb/tools/`.
- **Build commands** (from a VS Developer shell with `VCPKG_ROOT` set; a `build/` is usually already configured — reuse it):
  - Configure (only if `build/` missing or a `CMakeLists.txt` changed): `cmake --preset ninja -DATX_BUILD_TESTS=ON`
  - Build tests: `cmake --build build --target atx-tsdb-tests`
  - Build a tool: `cmake --build build --target atx-tsdb-load`
  - Run one suite: `ctest --test-dir build -R <SuiteName> --output-on-failure`
- **Gates per unit:** CLI clang-tidy clean (`clang-tidy -p build --header-filter=".*atx/tsdb/.*" <file>`), clang-format clean (`clang-format -i <file>`), `/W4 /permissive- /WX` build green, tests green. TDD always — failing test first.
- **Commit style:** `feat(tsdb-N): …` with trailer `Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>`. Stage **explicit pathspecs** (shared-branch landmine — never `git add -A`). After each commit verify it is reachable: `git -C <root> merge-base --is-ancestor <sha> HEAD`.
- **Safety-critical C++ (per `.agents/cpp/agent.md`):** `Result<T>` not exceptions, `const`/`constexpr`/`noexcept`/`[[nodiscard]]`, exhaustive switches, functions ≤60 lines, no UB, validate inputs at the boundary. Every `reinterpret_cast` over mapped bytes carries a `// SAFETY:` comment and a `NOLINTNEXTLINE` where clang-tidy flags pointer arithmetic / unions / reinterpret (mirror the `panel_types.hpp` precedent).

---

## File Structure

| Path | Responsibility |
|------|----------------|
| `atx-tsdb/CMakeLists.txt` | `atx-tsdb` STATIC lib (globs `src/*.cpp`), `atx::tsdb` alias, links `atx::core`; adds `tests/` and `tools/`. |
| `atx-tsdb/include/atx/tsdb/checksum.hpp` | `crc32(data,len)` — constexpr-table CRC-32 (zlib poly). Deterministic, portable, byte-wise. **No new dep.** |
| `atx-tsdb/include/atx/tsdb/segment.hpp` | The on-disk **contract**: magic/version constants, `SegmentHeader`/`FieldEntry`/`SymbolEntry`/`SegmentFooter` PODs, `mask_words()` + addressing free functions. Shared by builder + reader. |
| `atx-tsdb/include/atx/tsdb/mapping.hpp` | `Mapping` RAII seam: `map_file_ro(path) → Result<Mapping>`, `base()`, `size()`, `prefetch()`. |
| `atx-tsdb/src/mapping.cpp` | Win32 + POSIX impls behind `#if defined(_WIN32)`. |
| `atx-tsdb/include/atx/tsdb/builder.hpp` | `SegmentBuilder`: declare fields/symbols/time axis, `set(field,t,inst,value)`, `write(path,created_at) → Status`. |
| `atx-tsdb/src/builder.cpp` | Layout/offset computation, NaN-fill, present-bit set, crc + seal, file write. |
| `atx-tsdb/include/atx/tsdb/segment_reader.hpp` | `SegmentReader`: `attach(path) → Result<SegmentReader>` (validate), typed accessors, `cutoff_index(now)`. |
| `atx-tsdb/src/segment_reader.cpp` | Validation + accessor bodies. |
| `atx-tsdb/include/atx/tsdb/load_parquet.hpp` | `load_parquet(...) → Status` — parquet (long format) → `SegmentBuilder`. |
| `atx-tsdb/src/load_parquet.cpp` | Arrow-backed read via atx-core `io::LazyParquet`; pivot long rows into the dense grid. |
| `atx-tsdb/tools/load_main.cpp` | `atx-tsdb-load` CLI. |
| `atx-tsdb/tools/stat_main.cpp` | `atx-tsdb-stat` CLI. |
| `atx-tsdb/tests/*_test.cpp` | Per-unit GoogleTest suites (auto-globbed). |
| `atx-engine/include/atx/engine/data/shm_bar_feed.hpp` | **P2:** `ShmBarFeed : IDataHandler` over `SegmentReader`. |
| `atx-engine/tests/shm_bar_feed_test.cpp` | **P2:** determinism cross-check vs `InMemoryBarFeed` + no-look-ahead. |
| `CMakeLists.txt` (root) | Add `add_subdirectory(atx-tsdb)` **before** `atx-engine`. |
| `atx-engine/CMakeLists.txt` | **P2:** link `atx::tsdb`. |

**Note (deviation from spec §5.1):** the two platform mapping files are consolidated into a single `mapping.cpp` guarded by `#if defined(_WIN32)`, because the test CMake globs `src/*.cpp` and would otherwise compile the POSIX file on Windows. Functionally identical seam.

---

# Phase P0 — Scaffold

### Task 0: Create the `atx-tsdb` project skeleton and wire it into the build

**Files:**
- Create: `atx-tsdb/CMakeLists.txt`
- Create: `atx-tsdb/tests/CMakeLists.txt`
- Create: `atx-tsdb/tools/CMakeLists.txt`
- Create: `atx-tsdb/include/atx/tsdb/version.hpp`
- Create: `atx-tsdb/src/version.cpp`
- Create: `atx-tsdb/tests/scaffold_test.cpp`
- Modify: `CMakeLists.txt` (root) — add subdirectory

- [ ] **Step 1: Write the failing test**

`atx-tsdb/tests/scaffold_test.cpp`:
```cpp
#include <gtest/gtest.h>

#include "atx/tsdb/version.hpp"

TEST(Scaffold_VersionString_NonEmpty, ReturnsName) {
  EXPECT_EQ(atx::tsdb::version(), "atx-tsdb 0.1.0");
}
```

- [ ] **Step 2: Create the minimal library sources**

`atx-tsdb/include/atx/tsdb/version.hpp`:
```cpp
#pragma once

// atx::tsdb — shared-memory in-memory time-series store (build-smoke unit).

#include <string_view>

namespace atx::tsdb {

/// Library version string (smoke-test anchor; replaced by real units below).
[[nodiscard]] std::string_view version() noexcept;

} // namespace atx::tsdb
```

`atx-tsdb/src/version.cpp`:
```cpp
#include "atx/tsdb/version.hpp"

namespace atx::tsdb {

std::string_view version() noexcept { return "atx-tsdb 0.1.0"; }

} // namespace atx::tsdb
```

- [ ] **Step 3: Write the CMake wiring**

`atx-tsdb/CMakeLists.txt`:
```cmake
# atx-tsdb: immutable shared-memory time-series store. Sources are globbed so a
# new unit only drops a src/*.cpp; CONFIGURE_DEPENDS re-globs on change.
file(GLOB ATX_TSDB_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp")

add_library(atx-tsdb STATIC ${ATX_TSDB_SOURCES})
add_library(atx::tsdb ALIAS atx-tsdb)

target_include_directories(atx-tsdb
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

target_compile_features(atx-tsdb PUBLIC cxx_std_20)

target_link_libraries(atx-tsdb
    PUBLIC
        atx::core
    PRIVATE
        atx_warnings
)

if(ATX_BUILD_TESTS)
    add_subdirectory(tests)
endif()

add_subdirectory(tools)
```

`atx-tsdb/tests/CMakeLists.txt`:
```cmake
file(GLOB ATX_TSDB_TEST_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/*_test.cpp")

add_executable(atx-tsdb-tests ${ATX_TSDB_TEST_SOURCES})

target_link_libraries(atx-tsdb-tests
    PRIVATE
        atx::tsdb
        GTest::gtest_main
        atx_warnings
)

find_package(Threads REQUIRED)
target_link_libraries(atx-tsdb-tests PRIVATE Threads::Threads)

include(GoogleTest)
gtest_discover_tests(atx-tsdb-tests)
```

`atx-tsdb/tools/CMakeLists.txt`:
```cmake
add_executable(atx-tsdb-load load_main.cpp)
target_link_libraries(atx-tsdb-load PRIVATE atx::tsdb atx_warnings)

add_executable(atx-tsdb-stat stat_main.cpp)
target_link_libraries(atx-tsdb-stat PRIVATE atx::tsdb atx_warnings)
```

Create placeholder tool mains so the `tools/` subdir configures (replaced in Task 8):

`atx-tsdb/tools/load_main.cpp`:
```cpp
int main() { return 0; }
```

`atx-tsdb/tools/stat_main.cpp`:
```cpp
int main() { return 0; }
```

- [ ] **Step 4: Wire into the root build**

In root `CMakeLists.txt`, replace the trailing two lines:
```cmake
add_subdirectory(atx-core)
add_subdirectory(atx-engine)
```
with (atx-tsdb must come **after** atx-core and **before** atx-engine, which will depend on it in P2):
```cmake
add_subdirectory(atx-core)
add_subdirectory(atx-tsdb)
add_subdirectory(atx-engine)
```

- [ ] **Step 5: Configure, build, run — verify PASS**

Run:
```
cmake --preset ninja -DATX_BUILD_TESTS=ON
cmake --build build --target atx-tsdb-tests
ctest --test-dir build -R Scaffold_VersionString --output-on-failure
```
Expected: 1 test, PASS.

- [ ] **Step 6: Commit**

```
git add atx-tsdb/CMakeLists.txt atx-tsdb/tests/CMakeLists.txt atx-tsdb/tools/CMakeLists.txt atx-tsdb/include/atx/tsdb/version.hpp atx-tsdb/src/version.cpp atx-tsdb/tests/scaffold_test.cpp atx-tsdb/tools/load_main.cpp atx-tsdb/tools/stat_main.cpp CMakeLists.txt
git commit -m "feat(tsdb-0): scaffold atx-tsdb project + build wiring"
```

---

# Phase P1 — Standalone TSDB (no engine dependency)

### Task 1: CRC-32 checksum (deterministic integrity + content hash)

**Files:**
- Create: `atx-tsdb/include/atx/tsdb/checksum.hpp`
- Test: `atx-tsdb/tests/checksum_test.cpp`

- [ ] **Step 1: Write the failing test**

`atx-tsdb/tests/checksum_test.cpp`:
```cpp
#include <gtest/gtest.h>

#include <array>
#include <string_view>

#include "atx/tsdb/checksum.hpp"

// Known CRC-32 (zlib) of ASCII "123456789" is 0xCBF43926 (industry check value).
TEST(Crc32_CheckString_MatchesKnownVector, Check) {
  constexpr std::string_view s = "123456789";
  EXPECT_EQ(atx::tsdb::crc32(s.data(), s.size()), 0xCBF43926U);
}

TEST(Crc32_EmptyInput_IsZero, Empty) {
  EXPECT_EQ(atx::tsdb::crc32(nullptr, 0), 0U);
}

TEST(Crc32_DifferentBytes_DifferentDigest, Sensitivity) {
  const std::array<atx::u8, 3> a{1, 2, 3};
  const std::array<atx::u8, 3> b{1, 2, 4};
  EXPECT_NE(atx::tsdb::crc32(a.data(), a.size()), atx::tsdb::crc32(b.data(), b.size()));
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cmake --build build --target atx-tsdb-tests`
Expected: compile error — `atx/tsdb/checksum.hpp` not found.

- [ ] **Step 3: Write the implementation**

`atx-tsdb/include/atx/tsdb/checksum.hpp`:
```cpp
#pragma once

// atx::tsdb::crc32 — CRC-32 (IEEE 802.3 / zlib polynomial 0xEDB88320), table
// generated at compile time. Byte-wise, so the digest is endianness-independent
// and stable across processes/platforms — the property the segment integrity
// check and content_hash require (atx-core hash_bytes disclaims cross-restart
// stability, so it is unsuitable here).

#include <array>

#include "atx/core/types.hpp"

namespace atx::tsdb {

namespace detail {

/// Build the 256-entry CRC-32 lookup table at compile time.
[[nodiscard]] constexpr std::array<atx::u32, 256> make_crc32_table() noexcept {
  std::array<atx::u32, 256> table{};
  for (atx::u32 n = 0; n < 256U; ++n) {
    atx::u32 c = n;
    for (atx::u32 k = 0; k < 8U; ++k) {
      c = ((c & 1U) != 0U) ? (0xEDB88320U ^ (c >> 1U)) : (c >> 1U);
    }
    table[n] = c;
  }
  return table;
}

inline constexpr std::array<atx::u32, 256> kCrc32Table = make_crc32_table();

} // namespace detail

/// CRC-32 of a byte buffer. `data` may be nullptr iff `len == 0`.
[[nodiscard]] inline atx::u32 crc32(const void *data, atx::usize len) noexcept {
  // SAFETY: byte cursor over the buffer; len bounds the read. No write, no alias.
  // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
  const auto *p = static_cast<const atx::u8 *>(data);
  atx::u32 c = 0xFFFFFFFFU;
  for (atx::usize i = 0; i < len; ++i) {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
    c = detail::kCrc32Table[(c ^ p[i]) & 0xFFU] ^ (c >> 8U);
  }
  return c ^ 0xFFFFFFFFU;
}

} // namespace atx::tsdb
```

- [ ] **Step 4: Run test to verify it passes**

Run: `cmake --build build --target atx-tsdb-tests && ctest --test-dir build -R Crc32 --output-on-failure`
Expected: 3 tests, PASS.

- [ ] **Step 5: Lint + format, then commit**

```
clang-format -i atx-tsdb/include/atx/tsdb/checksum.hpp atx-tsdb/tests/checksum_test.cpp
git add atx-tsdb/include/atx/tsdb/checksum.hpp atx-tsdb/tests/checksum_test.cpp
git commit -m "feat(tsdb-1): constexpr-table CRC-32 for segment integrity"
```

---

### Task 2: Segment format — the on-disk contract

**Files:**
- Create: `atx-tsdb/include/atx/tsdb/segment.hpp`
- Test: `atx-tsdb/tests/segment_test.cpp`

- [ ] **Step 1: Write the failing test**

`atx-tsdb/tests/segment_test.cpp`:
```cpp
#include <gtest/gtest.h>

#include <array>
#include <bit>

#include "atx/tsdb/segment.hpp"

using atx::tsdb::SegmentHeader;

TEST(Segment_Magic_IsAscii, MagicBytes) {
  // "ATXSEG01" packed little-endian: byte 0 == 'A'.
  EXPECT_EQ(atx::tsdb::kMagic & 0xFFULL, static_cast<atx::u64>('A'));
}

TEST(Segment_MaskWords_RoundsUp, MaskWords) {
  EXPECT_EQ(atx::tsdb::mask_words(0), 0U);
  EXPECT_EQ(atx::tsdb::mask_words(1), 1U);
  EXPECT_EQ(atx::tsdb::mask_words(64), 1U);
  EXPECT_EQ(atx::tsdb::mask_words(65), 2U);
}

// Addressing math: lay out a tiny 2-field x 3-time x 2-inst grid by hand in a
// byte buffer, point a header at it, and verify the free-function accessors hit
// the right cells (position-independent: offsets only, no stored pointers).
TEST(Segment_Addressing_HitsCorrectCell, CellMath) {
  constexpr atx::u32 F = 2, N = 2;
  constexpr atx::u64 T = 3;
  std::array<atx::f64, F * T * N> blocks{};
  // field 1 (second block), t=2, inst=1  ==>  index = 1*(T*N) + (2*N + 1) = 6+5 = 11
  blocks[1 * (T * N) + (2 * N + 1)] = 42.0;

  SegmentHeader h{};
  h.field_count = F;
  h.instrument_count = N;
  h.time_count = T;
  h.off_field_blocks = 0;

  const auto *base = reinterpret_cast<const atx::u8 *>(blocks.data());
  EXPECT_DOUBLE_EQ(atx::tsdb::cell_value(base, h, /*field=*/1, /*t=*/2, /*inst=*/1), 42.0);
  EXPECT_DOUBLE_EQ(atx::tsdb::cell_value(base, h, /*field=*/0, /*t=*/0, /*inst=*/0), 0.0);
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cmake --build build --target atx-tsdb-tests`
Expected: compile error — `atx/tsdb/segment.hpp` not found.

- [ ] **Step 3: Write the implementation**

`atx-tsdb/include/atx/tsdb/segment.hpp`:
```cpp
#pragma once

// atx::tsdb segment format — THE on-disk contract shared by builder and reader.
//
// One contiguous file. Every internal reference is a byte OFFSET from the base
// of the mapping (never a raw pointer), so a reader can map the file at any
// virtual address. Sections, in order:
//   SegmentHeader | FieldDirectory | SymbolTable | StringBlob | TimeAxis |
//   FieldBlocks (F x [T][N] f64, row-major) | PresentBitmap | SegmentFooter.
//
// Endianness: the raw f64/i64 grids are written/read in native byte order. Both
// supported targets are little-endian x86_64; the static_assert below pins that
// assumption so a big-endian port fails loudly rather than silently corrupting.

#include <bit>
#include <span>

#include "atx/core/types.hpp"

namespace atx::tsdb {

static_assert(std::endian::native == std::endian::little,
              "atx-tsdb segment format assumes a little-endian target");

/// Pack an 8-char tag into a u64 (little-endian: s[0] is the low byte).
[[nodiscard]] consteval atx::u64 tag8(const char (&s)[9]) noexcept {
  atx::u64 v = 0;
  for (atx::u32 i = 0; i < 8U; ++i) {
    v |= static_cast<atx::u64>(static_cast<atx::u8>(s[i])) << (8U * i);
  }
  return v;
}

inline constexpr atx::u64 kMagic = tag8("ATXSEG01");
inline constexpr atx::u64 kSealMarker = tag8("SEALED!!");
inline constexpr atx::u32 kFormatVersion = 1U;
inline constexpr atx::u32 kFlagSealed = 1U << 0U;
inline constexpr atx::u32 kFieldNameLen = 16U; // NUL-padded field name capacity

// ---------------------------------------------------------------------------
//  POD records (trivially copyable; memcpy'd to/from the file verbatim).
// ---------------------------------------------------------------------------

/// Fixed header at file offset 0. All section offsets are bytes from base.
struct SegmentHeader {
  atx::u64 magic;            // == kMagic
  atx::u32 format_version;   // == kFormatVersion
  atx::u32 flags;            // bit0 = kFlagSealed
  atx::u64 total_bytes;      // full file size
  atx::u32 field_count;      // F
  atx::u32 instrument_count; // N
  atx::u64 time_count;       // T
  atx::i64 created_at_nanos; // wall clock at build (supplied by caller)
  atx::u64 content_hash;     // crc32 over data sections (zero-extended)
  atx::u64 off_field_dir;
  atx::u64 off_symbol_table;
  atx::u64 off_string_blob;
  atx::u64 off_time_axis;
  atx::u64 off_field_blocks;
  atx::u64 off_present_bitmap;
  atx::u64 off_footer;
};

/// One field-directory entry: NUL-padded name + its block index.
struct FieldEntry {
  char name[kFieldNameLen]; // e.g. "close"
  atx::u32 field_index;
  atx::u32 reserved; // pad to 8-byte alignment; written 0
};

/// One symbol-table entry: slice into the string blob (offsets from blob start).
struct SymbolEntry {
  atx::u32 name_off; // byte offset within StringBlob
  atx::u32 name_len; // byte length
};

/// Trailer: seal marker + integrity CRC over [header .. present-bitmap end].
struct SegmentFooter {
  atx::u64 seal_marker;   // == kSealMarker
  atx::u32 integrity_crc; // crc32 of everything before the footer
  atx::u32 reserved;      // written 0
};

static_assert(std::is_trivially_copyable_v<SegmentHeader>);
static_assert(std::is_trivially_copyable_v<FieldEntry>);
static_assert(std::is_trivially_copyable_v<SymbolEntry>);
static_assert(std::is_trivially_copyable_v<SegmentFooter>);

// ---------------------------------------------------------------------------
//  Geometry + addressing (pure functions over a header and a mapping base).
// ---------------------------------------------------------------------------

/// u64 words needed to hold an N-bit present-bitmap row.
[[nodiscard]] constexpr atx::u64 mask_words(atx::u32 n_inst) noexcept {
  return (static_cast<atx::u64>(n_inst) + 63U) / 64U;
}

/// Pointer to the first f64 of field block `field`.
[[nodiscard]] inline const atx::f64 *field_block(const atx::u8 *base, const SegmentHeader &h,
                                                 atx::u32 field) noexcept {
  const atx::u64 block_elems = h.time_count * h.instrument_count;
  const atx::u64 byte_off =
      h.off_field_blocks + static_cast<atx::u64>(field) * block_elems * sizeof(atx::f64);
  // SAFETY: byte_off < total_bytes by construction (validated at attach). The
  // section is laid out f64-aligned, so the reinterpret is well-defined.
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast,cppcoreguidelines-pro-bounds-pointer-arithmetic)
  return reinterpret_cast<const atx::f64 *>(base + byte_off);
}

/// Value at (field, t, inst). No bounds check (caller guarantees in range).
[[nodiscard]] inline atx::f64 cell_value(const atx::u8 *base, const SegmentHeader &h, atx::u32 field,
                                         atx::u64 t, atx::u32 inst) noexcept {
  // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
  return field_block(base, h, field)[t * h.instrument_count + inst];
}

/// True iff cell (t, inst) was present at build time.
[[nodiscard]] inline bool cell_present(const atx::u8 *base, const SegmentHeader &h, atx::u64 t,
                                       atx::u32 inst) noexcept {
  const atx::u64 mw = mask_words(h.instrument_count);
  // SAFETY: bitmap section is u64-aligned and sized T*mw; index is in range by
  // caller contract. Read-only mapped bytes.
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
  const auto *bm = reinterpret_cast<const atx::u64 *>(base + h.off_present_bitmap);
  // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
  const atx::u64 word = bm[t * mw + (inst >> 6U)];
  return ((word >> (inst & 63U)) & 1ULL) != 0ULL;
}

/// The time axis (T ascending unix-nanos) as a non-owning span.
[[nodiscard]] inline std::span<const atx::i64> time_axis(const atx::u8 *base,
                                                         const SegmentHeader &h) noexcept {
  // SAFETY: axis section is i64-aligned, sized T. Read-only mapped bytes.
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
  const auto *p = reinterpret_cast<const atx::i64 *>(base + h.off_time_axis);
  return {p, static_cast<atx::usize>(h.time_count)};
}

} // namespace atx::tsdb
```

- [ ] **Step 4: Run test to verify it passes**

Run: `cmake --build build --target atx-tsdb-tests && ctest --test-dir build -R Segment_ --output-on-failure`
Expected: 3 tests, PASS.

- [ ] **Step 5: Lint + format, then commit**

```
clang-format -i atx-tsdb/include/atx/tsdb/segment.hpp atx-tsdb/tests/segment_test.cpp
git add atx-tsdb/include/atx/tsdb/segment.hpp atx-tsdb/tests/segment_test.cpp
git commit -m "feat(tsdb-2): segment format contract + addressing helpers"
```

---

### Task 3: `Mapping` — cross-platform read-only file mapping

**Files:**
- Create: `atx-tsdb/include/atx/tsdb/mapping.hpp`
- Create: `atx-tsdb/src/mapping.cpp`
- Test: `atx-tsdb/tests/mapping_test.cpp`

- [ ] **Step 1: Write the failing test**

`atx-tsdb/tests/mapping_test.cpp`:
```cpp
#include <gtest/gtest.h>

#include <array>
#include <cstdio>
#include <fstream>
#include <string>

#include "atx/tsdb/mapping.hpp"

namespace {

// Write bytes to a unique temp file; return its path. (No Date.now/rand in
// engine code — but tests may use the OS temp dir + the test name for a path.)
std::string write_temp(const std::string &name, const std::string &bytes) {
  const std::string path = std::string(std::tmpnam(nullptr)) + name + ".bin";
  std::ofstream out(path, std::ios::binary);
  out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
  return path;
}

} // namespace

TEST(Mapping_MapExistingFile_ExposesBytes, MapsContent) {
  const std::string path = write_temp("map_ok", "hello-mmap");
  auto m = atx::tsdb::Mapping::map_file_ro(path);
  ASSERT_TRUE(m.has_value()) << (m ? "" : m.error().to_string());
  ASSERT_EQ(m->size(), 10U);
  // SAFETY: base() points at the mapped bytes; size() bounds the read.
  EXPECT_EQ(std::string(reinterpret_cast<const char *>(m->base()), m->size()), "hello-mmap");
  std::remove(path.c_str());
}

TEST(Mapping_MissingFile_ReturnsErr, RejectsMissing) {
  auto m = atx::tsdb::Mapping::map_file_ro("definitely-not-a-real-path.bin");
  EXPECT_FALSE(m.has_value());
}

TEST(Mapping_MoveLeavesSourceEmpty_NoDoubleUnmap, MoveSafe) {
  const std::string path = write_temp("map_move", "abc");
  auto m = atx::tsdb::Mapping::map_file_ro(path);
  ASSERT_TRUE(m.has_value());
  atx::tsdb::Mapping moved = std::move(*m);
  EXPECT_EQ(moved.size(), 3U);
  EXPECT_EQ(m->base(), nullptr); // moved-from is empty; dtor must be a no-op
  std::remove(path.c_str());
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cmake --build build --target atx-tsdb-tests`
Expected: compile error — `atx/tsdb/mapping.hpp` not found.

- [ ] **Step 3: Write the header**

`atx-tsdb/include/atx/tsdb/mapping.hpp`:
```cpp
#pragma once

// atx::tsdb::Mapping — RAII read-only file mapping (the cross-platform seam).
//
// map_file_ro(path) maps an existing file PROT_READ / FILE_MAP_READ and shares
// its pages via the OS page cache: every process that maps the same file sees
// one physical copy. Move-only (owns OS handles). Read-only by construction, so
// a buggy reader cannot corrupt the dataset for sibling processes.

#include <string>

#include "atx/core/error.hpp"
#include "atx/core/types.hpp"

namespace atx::tsdb {

class Mapping {
public:
  /// Map `path` read-only. Err(IoError) if the file cannot be opened/mapped,
  /// Err(InvalidArgument) if it is empty (an empty mapping is never valid here).
  [[nodiscard]] static atx::core::Result<Mapping> map_file_ro(const std::string &path);

  Mapping() noexcept = default;
  ~Mapping();

  Mapping(Mapping &&other) noexcept;
  Mapping &operator=(Mapping &&other) noexcept;
  Mapping(const Mapping &) = delete;
  Mapping &operator=(const Mapping &) = delete;

  [[nodiscard]] const atx::u8 *base() const noexcept { return base_; }
  [[nodiscard]] atx::usize size() const noexcept { return size_; }

  /// Hint the OS to make the mapping resident (madvise WILLNEED /
  /// PrefetchVirtualMemory). Best-effort; never fails the caller.
  void prefetch() const noexcept;

private:
  void reset() noexcept; // unmap + close handles; idempotent

  const atx::u8 *base_{nullptr};
  atx::usize size_{0};
#if defined(_WIN32)
  void *file_handle_{nullptr};    // HANDLE from CreateFileW
  void *mapping_handle_{nullptr}; // HANDLE from CreateFileMappingW
#else
  int fd_{-1};
#endif
};

} // namespace atx::tsdb
```

- [ ] **Step 4: Write the platform implementation**

`atx-tsdb/src/mapping.cpp`:
```cpp
#include "atx/tsdb/mapping.hpp"

#include <utility>

#include "atx/core/error.hpp"

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <memoryapi.h>
#else
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace atx::tsdb {

using atx::core::Err;
using atx::core::ErrorCode;
using atx::core::Result;

Mapping::~Mapping() { reset(); }

Mapping::Mapping(Mapping &&other) noexcept
    : base_{other.base_}, size_{other.size_}
#if defined(_WIN32)
      ,
      file_handle_{other.file_handle_}, mapping_handle_{other.mapping_handle_}
#else
      ,
      fd_{other.fd_}
#endif
{
  other.base_ = nullptr;
  other.size_ = 0;
#if defined(_WIN32)
  other.file_handle_ = nullptr;
  other.mapping_handle_ = nullptr;
#else
  other.fd_ = -1;
#endif
}

Mapping &Mapping::operator=(Mapping &&other) noexcept {
  if (this != &other) {
    reset();
    base_ = other.base_;
    size_ = other.size_;
#if defined(_WIN32)
    file_handle_ = other.file_handle_;
    mapping_handle_ = other.mapping_handle_;
    other.file_handle_ = nullptr;
    other.mapping_handle_ = nullptr;
#else
    fd_ = other.fd_;
    other.fd_ = -1;
#endif
    other.base_ = nullptr;
    other.size_ = 0;
  }
  return *this;
}

#if defined(_WIN32)

void Mapping::reset() noexcept {
  if (base_ != nullptr) {
    UnmapViewOfFile(base_);
    base_ = nullptr;
  }
  if (mapping_handle_ != nullptr) {
    CloseHandle(mapping_handle_);
    mapping_handle_ = nullptr;
  }
  if (file_handle_ != nullptr) {
    CloseHandle(file_handle_);
    file_handle_ = nullptr;
  }
  size_ = 0;
}

Result<Mapping> Mapping::map_file_ro(const std::string &path) {
  const int wlen = MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, nullptr, 0);
  std::wstring wpath(static_cast<std::size_t>(wlen), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, wpath.data(), wlen);

  HANDLE file = CreateFileW(wpath.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                            FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) {
    return Err(ErrorCode::IoError, "CreateFileW failed: " + path);
  }
  LARGE_INTEGER fsize{};
  if (GetFileSizeEx(file, &fsize) == 0 || fsize.QuadPart == 0) {
    CloseHandle(file);
    return Err(ErrorCode::InvalidArgument, "empty or unsizable file: " + path);
  }
  HANDLE mapping = CreateFileMappingW(file, nullptr, PAGE_READONLY, 0, 0, nullptr);
  if (mapping == nullptr) {
    CloseHandle(file);
    return Err(ErrorCode::IoError, "CreateFileMappingW failed: " + path);
  }
  void *view = MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 0);
  if (view == nullptr) {
    CloseHandle(mapping);
    CloseHandle(file);
    return Err(ErrorCode::IoError, "MapViewOfFile failed: " + path);
  }
  Mapping m;
  m.base_ = static_cast<const atx::u8 *>(view);
  m.size_ = static_cast<atx::usize>(fsize.QuadPart);
  m.file_handle_ = file;
  m.mapping_handle_ = mapping;
  return m;
}

void Mapping::prefetch() const noexcept {
  if (base_ == nullptr) {
    return;
  }
  WIN32_MEMORY_RANGE_ENTRY range{const_cast<atx::u8 *>(base_), size_};
  PrefetchVirtualMemory(GetCurrentProcess(), 1, &range, 0);
}

#else // POSIX

void Mapping::reset() noexcept {
  if (base_ != nullptr) {
    ::munmap(const_cast<atx::u8 *>(base_), size_);
    base_ = nullptr;
  }
  if (fd_ >= 0) {
    ::close(fd_);
    fd_ = -1;
  }
  size_ = 0;
}

Result<Mapping> Mapping::map_file_ro(const std::string &path) {
  const int fd = ::open(path.c_str(), O_RDONLY);
  if (fd < 0) {
    return Err(ErrorCode::IoError, "open failed: " + path);
  }
  struct stat st{};
  if (::fstat(fd, &st) != 0 || st.st_size == 0) {
    ::close(fd);
    return Err(ErrorCode::InvalidArgument, "empty or unstatable file: " + path);
  }
  const auto bytes = static_cast<atx::usize>(st.st_size);
  void *addr = ::mmap(nullptr, bytes, PROT_READ, MAP_SHARED, fd, 0);
  if (addr == MAP_FAILED) {
    ::close(fd);
    return Err(ErrorCode::IoError, "mmap failed: " + path);
  }
  Mapping m;
  m.base_ = static_cast<const atx::u8 *>(addr);
  m.size_ = bytes;
  m.fd_ = fd;
  return m;
}

void Mapping::prefetch() const noexcept {
  if (base_ != nullptr) {
    ::madvise(const_cast<atx::u8 *>(base_), size_, MADV_WILLNEED);
  }
}

#endif

} // namespace atx::tsdb
```

- [ ] **Step 5: Run test to verify it passes**

Run: `cmake --build build --target atx-tsdb-tests && ctest --test-dir build -R Mapping_ --output-on-failure`
Expected: 3 tests, PASS.

- [ ] **Step 6: Lint + format, then commit**

```
clang-format -i atx-tsdb/include/atx/tsdb/mapping.hpp atx-tsdb/src/mapping.cpp atx-tsdb/tests/mapping_test.cpp
git add atx-tsdb/include/atx/tsdb/mapping.hpp atx-tsdb/src/mapping.cpp atx-tsdb/tests/mapping_test.cpp
git commit -m "feat(tsdb-3): cross-platform read-only file Mapping seam"
```

---

### Task 4: `SegmentBuilder` — build and write a sealed segment

**Files:**
- Create: `atx-tsdb/include/atx/tsdb/builder.hpp`
- Create: `atx-tsdb/src/builder.cpp`
- Test: `atx-tsdb/tests/builder_test.cpp`

- [ ] **Step 1: Write the failing test**

`atx-tsdb/tests/builder_test.cpp`:
```cpp
#include <gtest/gtest.h>

#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

#include "atx/tsdb/builder.hpp"
#include "atx/tsdb/checksum.hpp"
#include "atx/tsdb/segment.hpp"

namespace {
std::string temp_path(const std::string &name) {
  return std::string(std::tmpnam(nullptr)) + name + ".atxseg";
}
} // namespace

TEST(Builder_WriteSealedFile_HeaderIsValid, WritesHeader) {
  atx::tsdb::SegmentBuilder b({"close", "volume"}, {"AAA", "BBB"}, {100, 200, 300});
  b.set(/*field=*/0, /*t=*/1, /*inst=*/0, 42.5);
  const std::string path = temp_path("b1");
  ASSERT_TRUE(b.write(path, /*created_at_nanos=*/123).has_value());

  std::ifstream in(path, std::ios::binary);
  atx::tsdb::SegmentHeader h{};
  in.read(reinterpret_cast<char *>(&h), sizeof(h));
  EXPECT_EQ(h.magic, atx::tsdb::kMagic);
  EXPECT_EQ(h.format_version, atx::tsdb::kFormatVersion);
  EXPECT_NE(h.flags & atx::tsdb::kFlagSealed, 0U);
  EXPECT_EQ(h.field_count, 2U);
  EXPECT_EQ(h.instrument_count, 2U);
  EXPECT_EQ(h.time_count, 3U);
  EXPECT_EQ(h.created_at_nanos, 123);
  std::remove(path.c_str());
}

TEST(Builder_BuildTwice_ByteIdentical, Deterministic) {
  const auto build = [](const std::string &p) {
    atx::tsdb::SegmentBuilder b({"close"}, {"AAA"}, {10, 20});
    b.set(0, 0, 0, 1.0);
    b.set(0, 1, 0, 2.0);
    return b.write(p, /*created_at_nanos=*/0).has_value();
  };
  const std::string p1 = temp_path("det1");
  const std::string p2 = temp_path("det2");
  ASSERT_TRUE(build(p1));
  ASSERT_TRUE(build(p2));

  std::ifstream f1(p1, std::ios::binary), f2(p2, std::ios::binary);
  const std::string s1((std::istreambuf_iterator<char>(f1)), {});
  const std::string s2((std::istreambuf_iterator<char>(f2)), {});
  EXPECT_EQ(s1, s2); // identical inputs (incl. created_at) => identical bytes
  std::remove(p1.c_str());
  std::remove(p2.c_str());
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cmake --build build --target atx-tsdb-tests`
Expected: compile error — `atx/tsdb/builder.hpp` not found.

- [ ] **Step 3: Write the header**

`atx-tsdb/include/atx/tsdb/builder.hpp`:
```cpp
#pragma once

// atx::tsdb::SegmentBuilder — assemble a dense panel grid in RAM and write it as
// a sealed, position-independent segment file. Single-threaded; build-once.
//
// Cells default to NaN with present-bit clear; set() writes a value and sets the
// present bit. write() lays out every section, computes content_hash + integrity
// crc, sets the SEALED flag, and writes the whole file in one pass.

#include <string>
#include <vector>

#include "atx/core/error.hpp"
#include "atx/core/types.hpp"

namespace atx::tsdb {

class SegmentBuilder {
public:
  /// Declare the grid shape: F field names (column order == field index), N
  /// symbol names (column order == instrument index), and the T ascending
  /// unix-nanos time axis. All field blocks start NaN / not-present.
  SegmentBuilder(std::vector<std::string> field_names, std::vector<std::string> symbols,
                 std::vector<atx::i64> time_axis);

  /// Set cell (field, t, inst) to `value` and mark it present.
  /// PRECONDITION: field < F, t < T, inst < N (ABORTS in debug).
  void set(atx::u32 field, atx::u64 t, atx::u32 inst, atx::f64 value) noexcept;

  /// Lay out + write the sealed segment to `path`. `created_at_nanos` is stamped
  /// into the header verbatim (caller supplies the clock). Err(IoError) if the
  /// file cannot be opened/written.
  [[nodiscard]] atx::core::Status write(const std::string &path,
                                        atx::i64 created_at_nanos) const;

private:
  std::vector<std::string> field_names_;
  std::vector<std::string> symbols_;
  std::vector<atx::i64> time_axis_;
  std::vector<atx::f64> blocks_;     // F*T*N, NaN-initialised
  std::vector<atx::u64> present_;    // T*mask_words(N), zero-initialised
};

} // namespace atx::tsdb
```

- [ ] **Step 4: Write the implementation**

`atx-tsdb/src/builder.cpp`:
```cpp
#include "atx/tsdb/builder.hpp"

#include <cmath>     // std::nan
#include <cstring>   // std::memcpy, std::memset
#include <fstream>
#include <limits>

#include "atx/core/error.hpp"
#include "atx/core/macro.hpp"
#include "atx/tsdb/checksum.hpp"
#include "atx/tsdb/segment.hpp"

namespace atx::tsdb {

using atx::core::Err;
using atx::core::ErrorCode;
using atx::core::Ok;
using atx::core::Status;

namespace {
[[nodiscard]] atx::u64 align_up(atx::u64 v, atx::u64 a) noexcept { return (v + a - 1U) & ~(a - 1U); }
} // namespace

SegmentBuilder::SegmentBuilder(std::vector<std::string> field_names,
                               std::vector<std::string> symbols, std::vector<atx::i64> time_axis)
    : field_names_{std::move(field_names)}, symbols_{std::move(symbols)},
      time_axis_{std::move(time_axis)} {
  const atx::u64 f = field_names_.size();
  const atx::u64 n = symbols_.size();
  const atx::u64 t = time_axis_.size();
  // NaN-fill every cell so an unset cell reads as "absent" even if its present
  // bit were ever misread; present_ is the authoritative absence signal.
  blocks_.assign(static_cast<atx::usize>(f * t * n),
                 std::numeric_limits<atx::f64>::quiet_NaN());
  present_.assign(static_cast<atx::usize>(t * mask_words(static_cast<atx::u32>(n))), 0ULL);
}

void SegmentBuilder::set(atx::u32 field, atx::u64 t, atx::u32 inst, atx::f64 value) noexcept {
  const atx::u64 f = field_names_.size();
  const atx::u64 n = symbols_.size();
  const atx::u64 nt = time_axis_.size();
  ATX_ASSERT(field < f && t < nt && inst < n);
  blocks_[static_cast<atx::usize>(static_cast<atx::u64>(field) * nt * n + t * n + inst)] = value;
  const atx::u64 mw = mask_words(static_cast<atx::u32>(n));
  present_[static_cast<atx::usize>(t * mw + (inst >> 6U))] |= (1ULL << (inst & 63U));
}

Status SegmentBuilder::write(const std::string &path, atx::i64 created_at_nanos) const {
  const atx::u32 f = static_cast<atx::u32>(field_names_.size());
  const atx::u32 n = static_cast<atx::u32>(symbols_.size());
  const atx::u64 t = time_axis_.size();
  const atx::u64 mw = mask_words(n);

  // --- string blob: concatenate symbol names; record (off,len) per symbol -----
  std::vector<SymbolEntry> sym_entries(n);
  std::string blob;
  for (atx::u32 i = 0; i < n; ++i) {
    sym_entries[i].name_off = static_cast<atx::u32>(blob.size());
    sym_entries[i].name_len = static_cast<atx::u32>(symbols_[i].size());
    blob += symbols_[i];
  }

  // --- section offsets (each 8-byte aligned for the f64/i64/u64 grids) --------
  atx::u64 off = sizeof(SegmentHeader);
  const atx::u64 off_field_dir = off;
  off += static_cast<atx::u64>(f) * sizeof(FieldEntry);
  const atx::u64 off_symbol_table = off;
  off += static_cast<atx::u64>(n) * sizeof(SymbolEntry);
  const atx::u64 off_string_blob = off;
  off += blob.size();
  off = align_up(off, 8U);
  const atx::u64 off_time_axis = off;
  off += t * sizeof(atx::i64);
  const atx::u64 off_field_blocks = off;
  off += static_cast<atx::u64>(f) * t * n * sizeof(atx::f64);
  const atx::u64 off_present_bitmap = off;
  off += t * mw * sizeof(atx::u64);
  const atx::u64 off_footer = off;
  const atx::u64 total = off_footer + sizeof(SegmentFooter);

  // --- assemble the whole file in one buffer ---------------------------------
  std::vector<atx::u8> buf(static_cast<atx::usize>(total), atx::u8{0});
  const auto put = [&buf](atx::u64 at, const void *src, atx::usize len) {
    std::memcpy(buf.data() + at, src, len);
  };

  SegmentHeader h{};
  h.magic = kMagic;
  h.format_version = kFormatVersion;
  h.flags = kFlagSealed;
  h.total_bytes = total;
  h.field_count = f;
  h.instrument_count = n;
  h.time_count = t;
  h.created_at_nanos = created_at_nanos;
  h.off_field_dir = off_field_dir;
  h.off_symbol_table = off_symbol_table;
  h.off_string_blob = off_string_blob;
  h.off_time_axis = off_time_axis;
  h.off_field_blocks = off_field_blocks;
  h.off_present_bitmap = off_present_bitmap;
  h.off_footer = off_footer;

  for (atx::u32 i = 0; i < f; ++i) {
    FieldEntry e{};
    const std::string &name = field_names_[i];
    const atx::usize cap = kFieldNameLen - 1U; // keep a NUL terminator
    std::memcpy(e.name, name.data(), name.size() < cap ? name.size() : cap);
    e.field_index = i;
    put(off_field_dir + static_cast<atx::u64>(i) * sizeof(FieldEntry), &e, sizeof(e));
  }
  if (n > 0) {
    put(off_symbol_table, sym_entries.data(), sym_entries.size() * sizeof(SymbolEntry));
  }
  if (!blob.empty()) {
    put(off_string_blob, blob.data(), blob.size());
  }
  if (t > 0) {
    put(off_time_axis, time_axis_.data(), t * sizeof(atx::i64));
  }
  if (!blocks_.empty()) {
    put(off_field_blocks, blocks_.data(), blocks_.size() * sizeof(atx::f64));
  }
  if (!present_.empty()) {
    put(off_present_bitmap, present_.data(), present_.size() * sizeof(atx::u64));
  }

  // content_hash: crc over the data sections [off_time_axis, off_footer).
  h.content_hash = crc32(buf.data() + off_time_axis, static_cast<atx::usize>(off_footer - off_time_axis));
  put(0, &h, sizeof(h));

  // integrity crc: over everything before the footer (header now finalised).
  SegmentFooter footer{};
  footer.seal_marker = kSealMarker;
  footer.integrity_crc = crc32(buf.data(), static_cast<atx::usize>(off_footer));
  put(off_footer, &footer, sizeof(footer));

  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  if (!out) {
    return Err(ErrorCode::IoError, "cannot open for write: " + path);
  }
  out.write(reinterpret_cast<const char *>(buf.data()), static_cast<std::streamsize>(buf.size()));
  if (!out) {
    return Err(ErrorCode::IoError, "write failed: " + path);
  }
  return Ok();
}

} // namespace atx::tsdb
```

- [ ] **Step 5: Run test to verify it passes**

Run: `cmake --build build --target atx-tsdb-tests && ctest --test-dir build -R Builder_ --output-on-failure`
Expected: 2 tests, PASS.

- [ ] **Step 6: Lint + format, then commit**

```
clang-format -i atx-tsdb/include/atx/tsdb/builder.hpp atx-tsdb/src/builder.cpp atx-tsdb/tests/builder_test.cpp
git add atx-tsdb/include/atx/tsdb/builder.hpp atx-tsdb/src/builder.cpp atx-tsdb/tests/builder_test.cpp
git commit -m "feat(tsdb-4): SegmentBuilder writes sealed deterministic segment"
```

---

### Task 5: `SegmentReader` — attach, validate, read

**Files:**
- Create: `atx-tsdb/include/atx/tsdb/segment_reader.hpp`
- Create: `atx-tsdb/src/segment_reader.cpp`
- Test: `atx-tsdb/tests/segment_reader_test.cpp`

- [ ] **Step 1: Write the failing test**

`atx-tsdb/tests/segment_reader_test.cpp`:
```cpp
#include <gtest/gtest.h>

#include <cstdio>
#include <fstream>
#include <string>

#include "atx/tsdb/builder.hpp"
#include "atx/tsdb/segment_reader.hpp"

namespace {
std::string temp_path(const std::string &name) {
  return std::string(std::tmpnam(nullptr)) + name + ".atxseg";
}
std::string make_segment(const std::string &name) {
  atx::tsdb::SegmentBuilder b({"close", "volume"}, {"AAA", "BBB"}, {100, 200, 300});
  b.set(/*field=*/0, /*t=*/0, /*inst=*/0, 10.0); // close AAA @ t0
  b.set(/*field=*/0, /*t=*/2, /*inst=*/1, 20.0); // close BBB @ t2
  const std::string path = temp_path(name);
  EXPECT_TRUE(b.write(path, 0).has_value());
  return path;
}
} // namespace

TEST(Reader_AttachValid_ExposesGrid, ReadsCells) {
  const std::string path = make_segment("r1");
  auto r = atx::tsdb::SegmentReader::attach(path);
  ASSERT_TRUE(r.has_value()) << (r ? "" : r.error().to_string());
  EXPECT_EQ(r->time_count(), 3U);
  EXPECT_EQ(r->instrument_count(), 2U);
  EXPECT_EQ(r->symbol_name(1), "BBB");
  ASSERT_TRUE(r->field_index("close").has_value());
  const atx::u32 close = *r->field_index("close");
  EXPECT_TRUE(r->present(0, 0));
  EXPECT_DOUBLE_EQ(r->value(close, 0, 0), 10.0);
  EXPECT_FALSE(r->present(1, 0)); // unset cell
  std::remove(path.c_str());
}

TEST(Reader_CutoffIndex_RespectsVisibility, NoLookAhead) {
  const std::string path = make_segment("r2");
  auto r = atx::tsdb::SegmentReader::attach(path);
  ASSERT_TRUE(r.has_value());
  EXPECT_FALSE(r->cutoff_index(99).has_value());  // before first row
  EXPECT_EQ(r->cutoff_index(100), 0U);            // exact hit on t0
  EXPECT_EQ(r->cutoff_index(250), 1U);            // between t1 and t2
  EXPECT_EQ(r->cutoff_index(10'000), 2U);         // after last row
  std::remove(path.c_str());
}

TEST(Reader_BadMagic_Rejected, ValidatesMagic) {
  const std::string path = make_segment("r3");
  { // corrupt the first byte of the magic
    std::fstream f(path, std::ios::binary | std::ios::in | std::ios::out);
    f.seekp(0);
    const char junk = 'Z';
    f.write(&junk, 1);
  }
  EXPECT_FALSE(atx::tsdb::SegmentReader::attach(path).has_value());
  std::remove(path.c_str());
}

TEST(Reader_TwoIndependentMappings_SameContent, SharedReadProxy) {
  // In-process proxy for the cross-process value prop: two independent mappings
  // of the same file (possibly at different addresses) must agree on content.
  const std::string path = make_segment("r4");
  auto a = atx::tsdb::SegmentReader::attach(path);
  auto b = atx::tsdb::SegmentReader::attach(path);
  ASSERT_TRUE(a.has_value() && b.has_value());
  EXPECT_EQ(a->content_hash(), b->content_hash());
  EXPECT_DOUBLE_EQ(a->value(*a->field_index("close"), 2, 1),
                   b->value(*b->field_index("close"), 2, 1));
  std::remove(path.c_str());
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cmake --build build --target atx-tsdb-tests`
Expected: compile error — `atx/tsdb/segment_reader.hpp` not found.

- [ ] **Step 3: Write the header**

`atx-tsdb/include/atx/tsdb/segment_reader.hpp`:
```cpp
#pragma once

// atx::tsdb::SegmentReader — attach a sealed segment file read-only and read its
// dense grid with O(1) addressing. attach() validates magic/version/SEALED/crc
// before exposing one byte; a bad file returns Err, never UB. Owns the Mapping.

#include <optional>
#include <span>
#include <string>
#include <string_view>

#include "atx/core/error.hpp"
#include "atx/core/types.hpp"

#include "atx/tsdb/mapping.hpp"
#include "atx/tsdb/segment.hpp"

namespace atx::tsdb {

class SegmentReader {
public:
  /// Map + validate `path`. Err(InvalidArgument) on bad magic/version/unsealed/
  /// short file; Err(IoError) if it cannot be mapped (propagated from Mapping);
  /// Err(Internal) on integrity-crc mismatch.
  [[nodiscard]] static atx::core::Result<SegmentReader> attach(const std::string &path);

  [[nodiscard]] atx::u64 time_count() const noexcept { return header().time_count; }
  [[nodiscard]] atx::u32 instrument_count() const noexcept { return header().instrument_count; }
  [[nodiscard]] atx::u32 field_count() const noexcept { return header().field_count; }
  [[nodiscard]] atx::u64 content_hash() const noexcept { return header().content_hash; }

  /// The ascending unix-nanos time axis (T entries).
  [[nodiscard]] std::span<const atx::i64> times() const noexcept {
    return time_axis(map_.base(), header());
  }

  /// Field index for `name`, or nullopt if absent. O(F) over the directory.
  [[nodiscard]] std::optional<atx::u32> field_index(std::string_view name) const noexcept;

  /// Symbol name for instrument column `inst`. PRECONDITION: inst < N (ABORTS).
  [[nodiscard]] std::string_view symbol_name(atx::u32 inst) const noexcept;

  /// Cell value. PRECONDITION: field < F, t < T, inst < N (ABORTS in debug).
  [[nodiscard]] atx::f64 value(atx::u32 field, atx::u64 t, atx::u32 inst) const noexcept {
    return cell_value(map_.base(), header(), field, t, inst);
  }

  /// Cell presence. PRECONDITION: t < T, inst < N (ABORTS in debug).
  [[nodiscard]] bool present(atx::u64 t, atx::u32 inst) const noexcept {
    return cell_present(map_.base(), header(), t, inst);
  }

  /// Newest row index whose time <= now_nanos, or nullopt if none is visible.
  /// This is the no-look-ahead cutoff: a reader must never address past it.
  [[nodiscard]] std::optional<atx::u64> cutoff_index(atx::i64 now_nanos) const noexcept;

  /// Make the mapping resident (best-effort).
  void prefetch() const noexcept { map_.prefetch(); }

private:
  explicit SegmentReader(Mapping map) noexcept : map_{std::move(map)} {}

  [[nodiscard]] const SegmentHeader &header() const noexcept {
    // SAFETY: attach() verified size >= sizeof(SegmentHeader) and the magic, so
    // the first bytes are a valid header.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    return *reinterpret_cast<const SegmentHeader *>(map_.base());
  }

  Mapping map_;
};

} // namespace atx::tsdb
```

- [ ] **Step 4: Write the implementation**

`atx-tsdb/src/segment_reader.cpp`:
```cpp
#include "atx/tsdb/segment_reader.hpp"

#include <algorithm> // std::upper_bound
#include <cstring>   // std::memcmp, std::strnlen

#include "atx/core/error.hpp"
#include "atx/core/macro.hpp"
#include "atx/tsdb/checksum.hpp"

namespace atx::tsdb {

using atx::core::Err;
using atx::core::ErrorCode;

namespace {

// Validate that a mapping holds a well-formed, sealed, intact segment. Returns
// an Error (by value, via Result-style) or std::nullopt on success.
[[nodiscard]] std::optional<atx::core::Error> validate(const Mapping &m) {
  if (m.size() < sizeof(SegmentHeader) + sizeof(SegmentFooter)) {
    return atx::core::Error{ErrorCode::InvalidArgument, "file too small for a segment"};
  }
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
  const auto &h = *reinterpret_cast<const SegmentHeader *>(m.base());
  if (h.magic != kMagic) {
    return atx::core::Error{ErrorCode::InvalidArgument, "bad magic (not an atx segment)"};
  }
  if (h.format_version != kFormatVersion) {
    return atx::core::Error{ErrorCode::InvalidArgument, "unsupported format version"};
  }
  if ((h.flags & kFlagSealed) == 0U) {
    return atx::core::Error{ErrorCode::InvalidArgument, "segment is not sealed"};
  }
  if (h.total_bytes != m.size() || h.off_footer + sizeof(SegmentFooter) != m.size()) {
    return atx::core::Error{ErrorCode::InvalidArgument, "size/offset mismatch"};
  }
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast,cppcoreguidelines-pro-bounds-pointer-arithmetic)
  const auto &f = *reinterpret_cast<const SegmentFooter *>(m.base() + h.off_footer);
  if (f.seal_marker != kSealMarker) {
    return atx::core::Error{ErrorCode::InvalidArgument, "missing seal marker"};
  }
  if (f.integrity_crc != crc32(m.base(), static_cast<atx::usize>(h.off_footer))) {
    return atx::core::Error{ErrorCode::Internal, "integrity crc mismatch (corrupt segment)"};
  }
  return std::nullopt;
}

} // namespace

atx::core::Result<SegmentReader> SegmentReader::attach(const std::string &path) {
  ATX_TRY(Mapping m, Mapping::map_file_ro(path));
  if (auto err = validate(m)) {
    return atx::core::Err(std::move(*err));
  }
  return SegmentReader{std::move(m)};
}

std::optional<atx::u32> SegmentReader::field_index(std::string_view name) const noexcept {
  const SegmentHeader &h = header();
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast,cppcoreguidelines-pro-bounds-pointer-arithmetic)
  const auto *dir = reinterpret_cast<const FieldEntry *>(map_.base() + h.off_field_dir);
  for (atx::u32 i = 0; i < h.field_count; ++i) {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
    const FieldEntry &e = dir[i];
    const atx::usize len = ::strnlen(e.name, kFieldNameLen);
    if (std::string_view{e.name, len} == name) {
      return e.field_index;
    }
  }
  return std::nullopt;
}

std::string_view SegmentReader::symbol_name(atx::u32 inst) const noexcept {
  const SegmentHeader &h = header();
  ATX_ASSERT(inst < h.instrument_count);
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast,cppcoreguidelines-pro-bounds-pointer-arithmetic)
  const auto *tbl = reinterpret_cast<const SymbolEntry *>(map_.base() + h.off_symbol_table);
  // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
  const SymbolEntry &e = tbl[inst];
  // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
  const auto *chars = reinterpret_cast<const char *>(map_.base() + h.off_string_blob + e.name_off);
  return std::string_view{chars, e.name_len};
}

std::optional<atx::u64> SegmentReader::cutoff_index(atx::i64 now_nanos) const noexcept {
  const std::span<const atx::i64> axis = times();
  // First element strictly greater than now; the one before it is the cutoff.
  const auto it = std::upper_bound(axis.begin(), axis.end(), now_nanos);
  if (it == axis.begin()) {
    return std::nullopt; // nothing visible yet
  }
  return static_cast<atx::u64>((it - axis.begin()) - 1);
}

} // namespace atx::tsdb
```

- [ ] **Step 5: Run test to verify it passes**

Run: `cmake --build build --target atx-tsdb-tests && ctest --test-dir build -R Reader_ --output-on-failure`
Expected: 4 tests, PASS.

- [ ] **Step 6: Lint + format, then commit**

```
clang-format -i atx-tsdb/include/atx/tsdb/segment_reader.hpp atx-tsdb/src/segment_reader.cpp atx-tsdb/tests/segment_reader_test.cpp
git add atx-tsdb/include/atx/tsdb/segment_reader.hpp atx-tsdb/src/segment_reader.cpp atx-tsdb/tests/segment_reader_test.cpp
git commit -m "feat(tsdb-5): SegmentReader attach/validate + cutoff_index"
```

---

### Task 6: `load_parquet` — pivot a long-format parquet into the grid

This loader assumes a **long-format** parquet: one row per (timestamp, symbol) with a timestamp column, a symbol column, and one numeric column per field. It pivots those rows into the dense grid. Columns are read via atx-core `io::LazyParquet`.

**Files:**
- Create: `atx-tsdb/include/atx/tsdb/load_parquet.hpp`
- Create: `atx-tsdb/src/load_parquet.cpp`
- Test: `atx-tsdb/tests/load_parquet_test.cpp`

- [ ] **Step 1: Write the failing test**

This test first writes a tiny parquet via Arrow is heavy; instead drive the loader through a seam that accepts already-extracted columns, so the pivot logic is unit-tested without an Arrow fixture. The thin parquet-file entry point is covered by the `atx-tsdb-load` CLI smoke in Task 8.

`atx-tsdb/tests/load_parquet_test.cpp`:
```cpp
#include <gtest/gtest.h>

#include <cstdio>
#include <string>
#include <vector>

#include "atx/tsdb/load_parquet.hpp"
#include "atx/tsdb/segment_reader.hpp"

namespace {
std::string temp_path(const std::string &name) {
  return std::string(std::tmpnam(nullptr)) + name + ".atxseg";
}
} // namespace

TEST(LoadPivot_LongRows_BuildsDenseGrid, Pivots) {
  // Long rows: (t, symbol, close). Two timestamps x two symbols, one gap.
  atx::tsdb::LongColumns cols;
  cols.field_names = {"close"};
  cols.times = {100, 100, 200};         // row time
  cols.symbols = {"AAA", "BBB", "AAA"}; // row symbol
  cols.values = {{1.0, 2.0, 3.0}};      // values[field][row]

  const std::string path = temp_path("pivot1");
  ASSERT_TRUE(atx::tsdb::build_from_long(cols, path, /*created_at_nanos=*/0).has_value());

  auto r = atx::tsdb::SegmentReader::attach(path);
  ASSERT_TRUE(r.has_value());
  EXPECT_EQ(r->time_count(), 2U);       // {100, 200} deduped + sorted
  EXPECT_EQ(r->instrument_count(), 2U); // {AAA, BBB}
  const atx::u32 close = *r->field_index("close");
  // AAA is interned first (row 0) => inst 0; BBB => inst 1.
  EXPECT_DOUBLE_EQ(r->value(close, 0, 0), 1.0); // t=100, AAA
  EXPECT_DOUBLE_EQ(r->value(close, 0, 1), 2.0); // t=100, BBB
  EXPECT_DOUBLE_EQ(r->value(close, 1, 0), 3.0); // t=200, AAA
  EXPECT_FALSE(r->present(1, 1));               // t=200, BBB absent (the gap)
  std::remove(path.c_str());
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cmake --build build --target atx-tsdb-tests`
Expected: compile error — `atx/tsdb/load_parquet.hpp` not found.

- [ ] **Step 3: Write the header**

`atx-tsdb/include/atx/tsdb/load_parquet.hpp`:
```cpp
#pragma once

// atx::tsdb parquet loader — pivot a LONG-format table (one row per
// (timestamp, symbol), one numeric column per field) into a sealed dense grid.
//
// The pivot core (build_from_long) takes already-extracted columns so it is unit
// testable without an Arrow fixture. load_parquet() is the thin file entry point
// that pulls those columns out of a parquet via atx-core io::LazyParquet.

#include <string>
#include <vector>

#include "atx/core/error.hpp"
#include "atx/core/types.hpp"

namespace atx::tsdb {

/// Extracted long-format columns. Row r contributes (times[r], symbols[r],
/// values[field][r]) for each field. All inner vectors share the row count.
struct LongColumns {
  std::vector<std::string> field_names;     // F field names
  std::vector<atx::i64> times;              // R row timestamps (unix nanos)
  std::vector<std::string> symbols;         // R row symbols
  std::vector<std::vector<atx::f64>> values; // [F][R] values
};

/// Pivot `cols` into a dense grid (dedup+sort time axis, intern symbols in
/// first-seen order) and write a sealed segment to `path`. Err(InvalidArgument)
/// on ragged column lengths.
[[nodiscard]] atx::core::Status build_from_long(const LongColumns &cols, const std::string &path,
                                                atx::i64 created_at_nanos);

/// Read `parquet_path`, project (time_col, symbol_col, field_cols) into
/// LongColumns, and build a sealed segment at `out_path`.
[[nodiscard]] atx::core::Status load_parquet(const std::string &parquet_path,
                                             const std::string &out_path,
                                             const std::string &time_col,
                                             const std::string &symbol_col,
                                             const std::vector<std::string> &field_cols,
                                             atx::i64 created_at_nanos);

} // namespace atx::tsdb
```

- [ ] **Step 4: Write the implementation**

`atx-tsdb/src/load_parquet.cpp`:
```cpp
#include "atx/tsdb/load_parquet.hpp"

#include <algorithm>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "atx/core/datetime.hpp"
#include "atx/core/error.hpp"
#include "atx/core/io/parquet.hpp"
#include "atx/core/series/frame.hpp"

#include "atx/tsdb/builder.hpp"

namespace atx::tsdb {

using atx::core::Err;
using atx::core::ErrorCode;
using atx::core::Ok;
using atx::core::Status;

Status build_from_long(const LongColumns &cols, const std::string &path,
                       atx::i64 created_at_nanos) {
  const atx::usize rows = cols.times.size();
  if (cols.symbols.size() != rows) {
    return Err(ErrorCode::InvalidArgument, "times/symbols length mismatch");
  }
  for (const auto &v : cols.values) {
    if (v.size() != rows) {
      return Err(ErrorCode::InvalidArgument, "field-values length mismatch");
    }
  }
  if (cols.values.size() != cols.field_names.size()) {
    return Err(ErrorCode::InvalidArgument, "field_names/values count mismatch");
  }

  // --- time axis: unique sorted timestamps -> row index --------------------
  std::vector<atx::i64> axis = cols.times;
  std::sort(axis.begin(), axis.end());
  axis.erase(std::unique(axis.begin(), axis.end()), axis.end());
  std::unordered_map<atx::i64, atx::u64> time_to_row;
  time_to_row.reserve(axis.size());
  for (atx::u64 i = 0; i < axis.size(); ++i) {
    time_to_row.emplace(axis[i], i);
  }

  // --- symbols: first-seen interning order -> instrument index -------------
  std::vector<std::string> symbols;
  std::unordered_map<std::string_view, atx::u32> sym_to_inst;
  for (const std::string &s : cols.symbols) {
    if (sym_to_inst.find(s) == sym_to_inst.end()) {
      const auto idx = static_cast<atx::u32>(symbols.size());
      symbols.push_back(s);
      sym_to_inst.emplace(symbols.back(), idx);
    }
  }

  SegmentBuilder builder(cols.field_names, symbols, axis);
  for (atx::usize r = 0; r < rows; ++r) {
    const atx::u64 t = time_to_row.at(cols.times[r]);
    const atx::u32 inst = sym_to_inst.at(cols.symbols[r]);
    for (atx::u32 fld = 0; fld < cols.field_names.size(); ++fld) {
      builder.set(fld, t, inst, cols.values[fld][r]);
    }
  }
  return builder.write(path, created_at_nanos);
}

Status load_parquet(const std::string &parquet_path, const std::string &out_path,
                    const std::string &time_col, const std::string &symbol_col,
                    const std::vector<std::string> &field_cols, atx::i64 created_at_nanos) {
  ATX_TRY(auto table, atx::core::io::read_parquet(parquet_path));

  LongColumns cols;
  cols.field_names = field_cols;

  // Timestamps: read the Timestamp column, project to unix nanos.
  ATX_TRY(auto ts, table.column_view<atx::core::time::Timestamp>(time_col));
  cols.times.reserve(ts.size());
  for (const auto &t : ts) {
    cols.times.push_back(t.unix_nanos());
  }

  // Symbols: string column.
  ATX_TRY(auto syms, table.strings(symbol_col));
  cols.symbols.reserve(syms.size());
  for (std::string_view s : syms) {
    cols.symbols.emplace_back(s);
  }

  // Field values: each numeric column as f64.
  cols.values.resize(field_cols.size());
  for (atx::usize f = 0; f < field_cols.size(); ++f) {
    ATX_TRY(auto col, table.column_view<atx::f64>(field_cols[f]));
    cols.values[f].assign(col.begin(), col.end());
  }

  return build_from_long(cols, out_path, created_at_nanos);
}

} // namespace atx::tsdb
```

> **Note on parquet dtypes:** `column_view<atx::f64>` requires the field columns to be Float64 in the parquet. If a real dataset stores prices as Decimal128 or Int, extend `load_parquet` with a per-dtype branch (read `table.schema()`, dispatch on `ColumnInfo::dtype`). Out of scope for this task — the f64 path is the spec's primary case; record any other dtype need as a follow-up.

- [ ] **Step 5: Run test to verify it passes**

Run: `cmake --build build --target atx-tsdb-tests && ctest --test-dir build -R LoadPivot_ --output-on-failure`
Expected: 1 test, PASS.

- [ ] **Step 6: Lint + format, then commit**

```
clang-format -i atx-tsdb/include/atx/tsdb/load_parquet.hpp atx-tsdb/src/load_parquet.cpp atx-tsdb/tests/load_parquet_test.cpp
git add atx-tsdb/include/atx/tsdb/load_parquet.hpp atx-tsdb/src/load_parquet.cpp atx-tsdb/tests/load_parquet_test.cpp
git commit -m "feat(tsdb-6): long-format parquet pivot into dense grid"
```

---

### Task 7: `atx-tsdb-load` and `atx-tsdb-stat` CLIs

**Files:**
- Modify: `atx-tsdb/tools/load_main.cpp`
- Modify: `atx-tsdb/tools/stat_main.cpp`

(No new unit test — these are thin arg-parsing shells over tested units. Manual smoke commands are given below.)

- [ ] **Step 1: Write the loader CLI**

`atx-tsdb/tools/load_main.cpp`:
```cpp
// atx-tsdb-load — pivot a long-format parquet into a sealed segment file.
//
// Usage:
//   atx-tsdb-load --in IN.parquet --out OUT.atxseg --time TS_COL --symbol SYM_COL
//                 --fields close,volume[,...]
//
// created_at_nanos is stamped 0 here (deterministic output); a wrapper that
// wants a real wall clock can pass it in a future flag.

#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>

#include "atx/tsdb/load_parquet.hpp"

namespace {

[[nodiscard]] std::string arg_after(int argc, char **argv, std::string_view flag) {
  for (int i = 1; i + 1 < argc; ++i) {
    if (flag == argv[i]) {
      return argv[i + 1];
    }
  }
  return {};
}

[[nodiscard]] std::vector<std::string> split_csv(const std::string &s) {
  std::vector<std::string> out;
  std::string cur;
  for (const char c : s) {
    if (c == ',') {
      if (!cur.empty()) {
        out.push_back(cur);
      }
      cur.clear();
    } else {
      cur.push_back(c);
    }
  }
  if (!cur.empty()) {
    out.push_back(cur);
  }
  return out;
}

} // namespace

int main(int argc, char **argv) {
  const std::string in = arg_after(argc, argv, "--in");
  const std::string out = arg_after(argc, argv, "--out");
  const std::string time_col = arg_after(argc, argv, "--time");
  const std::string sym_col = arg_after(argc, argv, "--symbol");
  const std::vector<std::string> fields = split_csv(arg_after(argc, argv, "--fields"));

  if (in.empty() || out.empty() || time_col.empty() || sym_col.empty() || fields.empty()) {
    std::fprintf(stderr,
                 "usage: atx-tsdb-load --in IN.parquet --out OUT.atxseg "
                 "--time TS_COL --symbol SYM_COL --fields f1,f2,...\n");
    return 2;
  }

  const auto status =
      atx::tsdb::load_parquet(in, out, time_col, sym_col, fields, /*created_at_nanos=*/0);
  if (!status) {
    std::fprintf(stderr, "load failed: %s\n", status.error().to_string().c_str());
    return 1;
  }
  std::printf("wrote %s\n", out.c_str());
  return 0;
}
```

- [ ] **Step 2: Write the stat CLI**

`atx-tsdb/tools/stat_main.cpp`:
```cpp
// atx-tsdb-stat — print header + geometry of a sealed segment (and verify crc).
//
// Usage: atx-tsdb-stat SEGMENT.atxseg

#include <cstdio>
#include <string>

#include "atx/tsdb/segment_reader.hpp"

int main(int argc, char **argv) {
  if (argc != 2) {
    std::fprintf(stderr, "usage: atx-tsdb-stat SEGMENT.atxseg\n");
    return 2;
  }
  auto reader = atx::tsdb::SegmentReader::attach(argv[1]);
  if (!reader) {
    std::fprintf(stderr, "attach failed: %s\n", reader.error().to_string().c_str());
    return 1;
  }
  std::printf("segment: %s\n", argv[1]);
  std::printf("  fields:      %u\n", reader->field_count());
  std::printf("  instruments: %u\n", reader->instrument_count());
  std::printf("  time rows:   %llu\n", static_cast<unsigned long long>(reader->time_count()));
  std::printf("  content_hash:%llu\n", static_cast<unsigned long long>(reader->content_hash()));
  return 0;
}
```

- [ ] **Step 3: Build both tools**

Run: `cmake --build build --target atx-tsdb-load atx-tsdb-stat`
Expected: both link cleanly.

- [ ] **Step 4: Manual smoke (if a sample parquet is available)**

Run (substitute a real long-format parquet path and its columns):
```
build/bin/atx-tsdb-load --in sample.parquet --out sample.atxseg --time ts --symbol symbol --fields close,volume
build/bin/atx-tsdb-stat sample.atxseg
```
Expected: `wrote sample.atxseg`, then a stat dump with non-zero geometry.

- [ ] **Step 5: Commit**

```
git add atx-tsdb/tools/load_main.cpp atx-tsdb/tools/stat_main.cpp
git commit -m "feat(tsdb-7): atx-tsdb-load + atx-tsdb-stat CLIs"
```

**P1 exit criterion met:** parquet → seal → re-attach → verify hash works on this platform; the same flow compiles on the other platform via the `#ifdef` mapping seam.

---

# Phase P2 — Engine integration (`ShmBarFeed`)

### Task 8: `ShmBarFeed` — `IDataHandler` over a `SegmentReader`

`ShmBarFeed` replaces `InMemoryBarFeed`: instead of k-way-merging caller-owned `BarRow` spans, it walks the segment's time axis row by row and, for each present instrument at that row, reconstructs a `domain::Bar` (OHLCV f64 → `Decimal` via `Decimal::from_double`, the deterministic spec §4.4 bridge) and publishes a Market event. The clock advances to the row's timestamp **before** publishing — same no-look-ahead discipline as `InMemoryBarFeed`.

**Files:**
- Create: `atx-engine/include/atx/engine/data/shm_bar_feed.hpp`
- Modify: `atx-engine/CMakeLists.txt` (link `atx::tsdb`)
- Test: `atx-engine/tests/shm_bar_feed_test.cpp`

- [ ] **Step 1: Link `atx::tsdb` into the engine**

In `atx-engine/CMakeLists.txt`, change the `target_link_libraries(atx-engine ...)` `PUBLIC` list from:
```cmake
target_link_libraries(atx-engine
    PUBLIC
        atx::core
    PRIVATE
        atx_warnings
)
```
to:
```cmake
target_link_libraries(atx-engine
    PUBLIC
        atx::core
        atx::tsdb
    PRIVATE
        atx_warnings
)
```

- [ ] **Step 2: Write the failing test**

`atx-engine/tests/shm_bar_feed_test.cpp`:
```cpp
#include <gtest/gtest.h>

#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "atx/core/domain/symbol.hpp"

#include "atx/engine/bus/event_bus.hpp"
#include "atx/engine/clock/sim_clock.hpp"
#include "atx/engine/data/market.hpp"
#include "atx/engine/data/shm_bar_feed.hpp"
#include "atx/engine/event/event.hpp"

#include "atx/tsdb/builder.hpp"
#include "atx/tsdb/segment_reader.hpp"

namespace {
std::string temp_path(const std::string &name) {
  return std::string(std::tmpnam(nullptr)) + name + ".atxseg";
}

// Build a 2-time x 1-instrument OHLCV segment: AAA has a bar at t=100 only.
std::string make_ohlcv(const std::string &name) {
  atx::tsdb::SegmentBuilder b({"open", "high", "low", "close", "volume"}, {"AAA"}, {100, 200});
  b.set(0, 0, 0, 10.0); // open
  b.set(1, 0, 0, 12.0); // high
  b.set(2, 0, 0, 9.0);  // low
  b.set(3, 0, 0, 11.0); // close
  b.set(4, 0, 0, 1000.0); // volume
  // t=200 left absent for AAA (present bit clear) -> no event there.
  const std::string path = temp_path(name);
  EXPECT_TRUE(b.write(path, 0).has_value());
  return path;
}
} // namespace

TEST(ShmBarFeed_StepPublishesPresentBars_NoLookAhead, PublishesBars) {
  const std::string path = make_ohlcv("feed1");
  auto reader = atx::tsdb::SegmentReader::attach(path);
  ASSERT_TRUE(reader.has_value());

  atx::core::domain::SymbolTable symbols;
  atx::engine::SimClock clock;
  auto bus = std::make_unique<atx::engine::EventBus<>>();

  atx::engine::data::ShmBarFeed feed(*reader, symbols, clock, *bus);

  // Step 1: frontier t=100, AAA present -> exactly one Market event, clock@100.
  ASSERT_TRUE(feed.step());
  EXPECT_EQ(clock.now().unix_nanos(), 100);
  int seen = 0;
  atx::core::domain::Bar got{};
  bus->drain_in_order([&](const atx::engine::event::Event &e) {
    ++seen;
    EXPECT_LE(e.knowledge_ts.unix_nanos(), clock.now().unix_nanos()); // no look-ahead
    got = e.payload_as<atx::engine::data::MarketPayload>().as_bar();
  });
  EXPECT_EQ(seen, 1);
  EXPECT_DOUBLE_EQ(got.close.to_decimal().to_double(), 11.0);

  // Step 2: frontier t=200, AAA absent -> clock advances, zero events.
  ASSERT_TRUE(feed.step());
  EXPECT_EQ(clock.now().unix_nanos(), 200);
  int seen2 = 0;
  bus->drain_in_order([&](const atx::engine::event::Event &) { ++seen2; });
  EXPECT_EQ(seen2, 0);

  // Step 3: EOF.
  EXPECT_FALSE(feed.step());
  std::remove(path.c_str());
}
```

> **Pre-check:** confirm the exact `SimClock::now()`, `EventBus::drain_in_order`, and `Event::payload_as`/`knowledge_ts` member names against `sim_clock.hpp`, `event_bus.hpp`, `event.hpp` (they are used as in `replay_determinism_test.cpp`). Also confirm `Decimal::to_double()` exists for the assertion; if not, compare via `Decimal::from_double(11.0)` equality instead.

- [ ] **Step 3: Run test to verify it fails**

Run: `cmake --build build --target atx-engine-tests`
Expected: compile error — `atx/engine/data/shm_bar_feed.hpp` not found.

- [ ] **Step 4: Write the implementation**

`atx-engine/include/atx/engine/data/shm_bar_feed.hpp`:
```cpp
#pragma once

// atx::engine::data::ShmBarFeed — an IDataHandler that streams a sealed atx-tsdb
// segment straight out of shared memory, with no per-process re-parse or data
// copy. It walks the segment's time axis; for each row it advances the clock to
// that row's timestamp (BEFORE publishing, preserving no-look-ahead) and emits a
// Market event for every instrument present at that row.
//
// f64 -> Decimal: the grid is f64 (the alpha/panel path); a Bar's prices are the
// exact Decimal money type. Reconstruction uses Decimal::from_double (rounds
// half-away-from-zero to the nano grid) — the deterministic bridge from spec
// §4.4. A present cell is always finite, so from_double never errs here; an
// unexpected non-finite value falls back to a zero Decimal (defensive).

#include "atx/core/datetime.hpp"      // time::Timestamp
#include "atx/core/decimal.hpp"       // Decimal::from_double
#include "atx/core/domain/domain.hpp" // domain::Bar, Price, Quantity
#include "atx/core/domain/symbol.hpp" // domain::SymbolTable, Symbol
#include "atx/core/macro.hpp"         // ATX_ASSERT
#include "atx/core/types.hpp"         // u32, u64, f64

#include "atx/engine/bus/event_bus.hpp"   // EventBus
#include "atx/engine/clock/sim_clock.hpp" // SimClock
#include "atx/engine/data/data_handler.hpp" // IDataHandler
#include "atx/engine/data/market.hpp"       // make_market_bar
#include "atx/engine/event/event.hpp"       // event::Event

#include "atx/tsdb/segment_reader.hpp" // tsdb::SegmentReader

namespace atx::engine::data {

class ShmBarFeed final : public IDataHandler {
public:
  /// Build a feed over `reader` (a sealed OHLCV segment), publishing onto `bus`
  /// and advancing `clock`. Symbol columns are interned into `symbols` once at
  /// construction, building the inst-index -> Symbol map. NON-OWNING: `reader`,
  /// `symbols`, `clock`, and `bus` must outlive this feed.
  ///
  /// PRECONDITION: the segment carries the five OHLCV fields (ABORTS in debug if
  /// any is missing — a non-OHLCV segment is a wiring error, not a runtime case).
  ShmBarFeed(const atx::tsdb::SegmentReader &reader, atx::core::domain::SymbolTable &symbols,
             SimClock &clock, EventBus<> &bus)
      : reader_{&reader}, clock_{&clock}, bus_{&bus} {
    open_ = require_field(reader, "open");
    high_ = require_field(reader, "high");
    low_ = require_field(reader, "low");
    close_ = require_field(reader, "close");
    volume_ = require_field(reader, "volume");
    syms_.reserve(reader.instrument_count());
    for (atx::u32 i = 0; i < reader.instrument_count(); ++i) {
      syms_.push_back(symbols.intern(reader.symbol_name(i)));
    }
  }

  /// Advance one time-axis row: move the clock to that row's timestamp, publish a
  /// Market event for every present instrument, return true. False at EOF.
  [[nodiscard]] bool step() override {
    if (row_ >= reader_->time_count()) {
      return false;
    }
    const atx::i64 ts_nanos = reader_->times()[static_cast<atx::usize>(row_)];
    const auto ts = atx::core::time::Timestamp::from_unix_nanos(ts_nanos);

    // Clock advances BEFORE publishing (no-look-ahead, as in InMemoryBarFeed).
    clock_->advance_to(ts);
    publish_row(row_, ts);
    ++row_;
    return true;
  }

private:
  [[nodiscard]] static atx::u32 require_field(const atx::tsdb::SegmentReader &r,
                                              std::string_view name) {
    const auto idx = r.field_index(name);
    ATX_ASSERT(idx.has_value());
    return idx.value_or(0U);
  }

  [[nodiscard]] static atx::core::domain::Price price_of(atx::f64 v) noexcept {
    const auto d = atx::core::Decimal::from_double(v);
    return atx::core::domain::Price::from_decimal(d.value_or(atx::core::Decimal{}));
  }

  [[nodiscard]] static atx::core::domain::Quantity qty_of(atx::f64 v) noexcept {
    const auto d = atx::core::Decimal::from_double(v);
    return atx::core::domain::Quantity::from_decimal(d.value_or(atx::core::Decimal{}));
  }

  void publish_row(atx::u64 row, atx::core::time::Timestamp ts) {
    for (atx::u32 inst = 0; inst < reader_->instrument_count(); ++inst) {
      if (!reader_->present(row, inst)) {
        continue; // absent cell -> no event (ragged history / survivorship)
      }
      atx::core::domain::Bar bar{};
      bar.ts = ts;
      bar.open = price_of(reader_->value(open_, row, inst));
      bar.high = price_of(reader_->value(high_, row, inst));
      bar.low = price_of(reader_->value(low_, row, inst));
      bar.close = price_of(reader_->value(close_, row, inst));
      bar.volume = qty_of(reader_->value(volume_, row, inst));

      atx::i64 seq = 0;
      event::Event &slot = bus_->claim_slot(seq);
      // knowledge_ts == bar.ts (release-at-close; spec §4.2 v1 assumption).
      slot = make_market_bar(syms_[inst], bar, ts, /*delisted_final=*/false);
      bus_->publish(seq);
    }
  }

  const atx::tsdb::SegmentReader *reader_; // non-owning; outlives the feed
  SimClock *clock_;                        // non-owning
  EventBus<> *bus_;                        // non-owning
  std::vector<atx::core::domain::Symbol> syms_; // inst index -> interned Symbol
  atx::u32 open_{0}, high_{0}, low_{0}, close_{0}, volume_{0};
  atx::u64 row_{0}; // next time-axis row to publish
};

} // namespace atx::engine::data
```

- [ ] **Step 5: Run test to verify it passes**

Run: `cmake --build build --target atx-engine-tests && ctest --test-dir build -R ShmBarFeed_ --output-on-failure`
Expected: 1 test, PASS. (If `Decimal::to_double` is absent, adjust the assertion per the Step 2 pre-check note, rebuild, re-run.)

- [ ] **Step 6: Lint + format, then commit**

```
clang-format -i atx-engine/include/atx/engine/data/shm_bar_feed.hpp atx-engine/tests/shm_bar_feed_test.cpp
git add atx-engine/include/atx/engine/data/shm_bar_feed.hpp atx-engine/tests/shm_bar_feed_test.cpp atx-engine/CMakeLists.txt
git commit -m "feat(tsdb-8): ShmBarFeed IDataHandler over shared-memory segment"
```

---

### Task 9: Determinism cross-check — `ShmBarFeed` ≡ `InMemoryBarFeed`

Prove the spec's headline invariant: a backtest fed from shared memory produces the **same event stream** as one fed from the in-memory feed on identical data. Reuse the hashing style from `replay_determinism_test.cpp`.

**Files:**
- Test: `atx-engine/tests/shm_determinism_test.cpp`

- [ ] **Step 1: Write the test**

`atx-engine/tests/shm_determinism_test.cpp`:
```cpp
#include <gtest/gtest.h>

#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "atx/core/datetime.hpp"
#include "atx/core/domain/domain.hpp"
#include "atx/core/domain/symbol.hpp"
#include "atx/core/hash.hpp"

#include "atx/engine/bus/event_bus.hpp"
#include "atx/engine/clock/sim_clock.hpp"
#include "atx/engine/data/data_handler.hpp"
#include "atx/engine/data/market.hpp"
#include "atx/engine/data/shm_bar_feed.hpp"

#include "atx/tsdb/builder.hpp"
#include "atx/tsdb/segment_reader.hpp"

namespace {

std::string temp_path(const std::string &name) {
  return std::string(std::tmpnam(nullptr)) + name + ".atxseg";
}

// Fold every drained event's salient fields into a running hash. Identical
// streams -> identical digest.
atx::u64 drain_hash(atx::engine::EventBus<> &bus) {
  atx::u64 h = 0;
  bus.drain_in_order([&](const atx::engine::event::Event &e) {
    const auto bar = e.payload_as<atx::engine::data::MarketPayload>();
    h = atx::core::hash_combine(static_cast<std::size_t>(h),
                                e.knowledge_ts.unix_nanos(), e.event_ts.unix_nanos(),
                                bar.symbol.id, bar.as_bar().close.to_decimal().to_string());
  });
  return h;
}

} // namespace

TEST(ShmDeterminism_ShmFeedEqualsInMemory_ByteStream, SameDigest) {
  // --- shared data: two symbols, two timestamps, dense -----------------------
  atx::core::domain::SymbolTable shm_syms;
  atx::core::domain::SymbolTable mem_syms;

  // Build the segment (shm side).
  atx::tsdb::SegmentBuilder b({"open", "high", "low", "close", "volume"}, {"AAA", "BBB"},
                              {100, 200});
  const double px[2][2] = {{11.0, 21.0}, {12.0, 22.0}}; // [t][inst] close
  for (atx::u64 t = 0; t < 2; ++t) {
    for (atx::u32 i = 0; i < 2; ++i) {
      b.set(0, t, i, px[t][i]);       // open
      b.set(1, t, i, px[t][i] + 1.0); // high
      b.set(2, t, i, px[t][i] - 1.0); // low
      b.set(3, t, i, px[t][i]);       // close
      b.set(4, t, i, 100.0);          // volume
    }
  }
  const std::string path = temp_path("det");
  ASSERT_TRUE(b.write(path, 0).has_value());
  auto reader = atx::tsdb::SegmentReader::attach(path);
  ASSERT_TRUE(reader.has_value());

  // --- run the shm feed ------------------------------------------------------
  atx::engine::SimClock shm_clock;
  auto shm_bus = std::make_unique<atx::engine::EventBus<>>();
  atx::engine::data::ShmBarFeed shm_feed(*reader, shm_syms, shm_clock, *shm_bus);
  atx::u64 shm_digest = 0;
  while (shm_feed.step()) {
    shm_digest = atx::core::hash_combine(static_cast<std::size_t>(shm_digest), drain_hash(*shm_bus));
  }

  // --- run the in-memory feed over the SAME logical data ---------------------
  using atx::engine::data::BarRow;
  const auto ts100 = atx::core::time::Timestamp::from_unix_nanos(100);
  const auto ts200 = atx::core::time::Timestamp::from_unix_nanos(200);
  const auto sym_aaa = mem_syms.intern("AAA");
  const auto sym_bbb = mem_syms.intern("BBB");
  const auto mk = [](atx::core::domain::Symbol s, atx::core::time::Timestamp ts, double close) {
    atx::core::domain::Bar bar{};
    bar.ts = ts;
    bar.open = atx::core::domain::Price::from_decimal(*atx::core::Decimal::from_double(close));
    bar.high = atx::core::domain::Price::from_decimal(*atx::core::Decimal::from_double(close + 1.0));
    bar.low = atx::core::domain::Price::from_decimal(*atx::core::Decimal::from_double(close - 1.0));
    bar.close = atx::core::domain::Price::from_decimal(*atx::core::Decimal::from_double(close));
    bar.volume = atx::core::domain::Quantity::from_decimal(*atx::core::Decimal::from_double(100.0));
    return BarRow{s, bar, ts, false};
  };
  // Each source must be knowledge_ts-sorted; one source per symbol.
  const std::vector<BarRow> src_aaa{mk(sym_aaa, ts100, 11.0), mk(sym_aaa, ts200, 12.0)};
  const std::vector<BarRow> src_bbb{mk(sym_bbb, ts100, 21.0), mk(sym_bbb, ts200, 22.0)};
  const std::vector<std::span<const BarRow>> sources{src_aaa, src_bbb};

  atx::engine::SimClock mem_clock;
  auto mem_bus = std::make_unique<atx::engine::EventBus<>>();
  atx::engine::data::InMemoryBarFeed mem_feed(sources, mem_clock, *mem_bus);
  atx::u64 mem_digest = 0;
  while (mem_feed.step()) {
    mem_digest = atx::core::hash_combine(static_cast<std::size_t>(mem_digest), drain_hash(*mem_bus));
  }

  EXPECT_EQ(shm_digest, mem_digest);
  std::remove(path.c_str());
}
```

> **Pre-check:** the digest only matches if both feeds emit the same (symbol, timestamp) order. `ShmBarFeed` emits per row in instrument-column order (AAA, BBB); `InMemoryBarFeed` emits per frontier in (knowledge_ts, source_idx) order, so source 0 = AAA, source 1 = BBB. Symbol ids match because both tables intern AAA then BBB (id 0, 1). If `Decimal::to_string`/`Symbol::id` member spellings differ, align `drain_hash` accordingly (verify against `replay_determinism_test.cpp`).

- [ ] **Step 2: Run the test**

Run: `cmake --build build --target atx-engine-tests && ctest --test-dir build -R ShmDeterminism_ --output-on-failure`
Expected: 1 test, PASS (shm digest == in-memory digest).

- [ ] **Step 3: Run the whole engine + tsdb suite (regression gate)**

Run:
```
cmake --build build --target atx-engine-tests atx-tsdb-tests
ctest --test-dir build -R "atx-tsdb|ShmBarFeed|ShmDeterminism|ReplayDeterminism" --output-on-failure
```
Expected: all green — the shm path matches the in-memory path and nothing in Phase-1/2 regressed.

- [ ] **Step 4: Commit**

```
git add atx-engine/tests/shm_determinism_test.cpp
git commit -m "test(tsdb-9): ShmBarFeed == InMemoryBarFeed event-stream determinism"
```

**P2 exit criterion met:** a backtest fed from shared memory produces a byte-identical event stream to the in-memory feed, with no-look-ahead enforced by the clock-before-publish order and the present-bitmap.

---

## Deferred (NOT in this plan — spec §10 P3)

- `PanelView` ring→linear addressing generalization, then `ShmPanelSource` (zero-copy `PanelView` straight over shm). Compile-guard the contract like `VmSignalSource` until built.
- True bitemporal restatements (`knowledge_ts > event_ts`) via a correction overlay.
- Per-dtype parquet field columns (Decimal128/int) in `load_parquet`.
- Optional scaled-integer price block for bit-exact input prices.

---

## Self-Review

**1. Spec coverage**

| Spec section | Task(s) |
|---|---|
| §3.1 file-backed mmap | Task 3 (Mapping) |
| §3.3 position-independence (offsets only) | Task 2 (segment), Task 5 (`Reader_TwoIndependentMappings`) |
| §4 segment format + addressing | Task 2 |
| §4.2 no-look-ahead `cutoff_index` | Task 5, Task 8 (clock-before-publish), Task 9 |
| §4.3 ragged via present-bitmap | Task 4 (set), Task 6 (gap test), Task 8 (absent → no event) |
| §4.4 f64↔Decimal bridge | Task 8 (`price_of` via `Decimal::from_double`) |
| §5.1 components | Tasks 1–7 |
| §5.2 ShmBarFeed | Task 8 |
| §5.3 tools | Task 7 |
| §6.1 loader path | Task 6 |
| §6.2 reader path / determinism cross-check | Task 9 |
| §7 invariants | Determinism (Task 4 byte-identical, Task 9), no-look-ahead (Task 5/8/9), survivorship (Task 6/8 absent cells), read-safety (Task 5 validation) |
| §8 testing | every task is TDD; cross-process proxy = Task 5 two-mappings |
| §9 build | Task 0 |
| §10 phasing | P0/P1/P2 sections; P3 deferred list |
| §11 non-goals | Deferred section |

No gaps.

**2. Placeholder scan:** No "TBD"/"implement later"/"add error handling" — every code step shows complete code. Two explicit `> Pre-check`/`> Note` callouts flag *verification* steps against existing engine headers (member-name confirmation) and an out-of-scope dtype branch; both are bounded and named, not placeholders.

**3. Type consistency:** `SegmentHeader` field names are identical across `segment.hpp`, `builder.cpp`, `segment_reader.cpp`. `mask_words`, `cell_value`, `cell_present`, `time_axis` signatures match between definition (Task 2) and use (Tasks 4/5). `SegmentBuilder::set`/`write`, `SegmentReader::attach`/`value`/`present`/`cutoff_index`/`field_index`/`symbol_name`/`content_hash`/`times`, `LongColumns`/`build_from_long`/`load_parquet`, and `ShmBarFeed(reader, symbols, clock, bus)` are spelled identically at definition and call sites. `crc32` used consistently for both content_hash and integrity_crc.
