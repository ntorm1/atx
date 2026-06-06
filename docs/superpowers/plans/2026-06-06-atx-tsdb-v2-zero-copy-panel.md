# atx-tsdb v2 — Zero-Copy Batch Panel for the Alpha VM — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Let the batch alpha VM (`alpha::Engine`) evaluate a compiled `alpha::Program` directly over a sealed atx-tsdb segment with **zero copy and zero conversion** — the segment's f64 field blocks are mapped once (OS page cache) and the VM's `alpha::Panel` reads them in place.

**Architecture:** The segment grid `[field][time][instrument]` is byte-identical to `alpha::Panel`'s date-major field columns. `alpha::Engine` borrows the Panel through a fixed read surface (`field_all`/`field_cross_section`/`in_universe`/`field_id`), so giving `Panel` a *borrowing* column backend needs **zero Engine change**. A new `attach_segment_panel` bridge maps a segment, windows it, derives the universe from the present-bitmap, and returns a move-only `MappedPanel` that owns the mapping and exposes a borrowed `Panel`. The segment format is bumped to v2 (64-byte-aligned field blocks for SIMD); the reader still accepts v1.

**Tech Stack:** C++20, CMake + Ninja + clang-cl, GoogleTest, Google Benchmark, atx-core (`error`, `types`), atx-tsdb (`segment`, `mapping`, `segment_reader`, `builder`), atx-engine `alpha` (`panel`, `vm`, `oracle`, `parser`, `typecheck`, `bytecode`, `registry`).

**Spec:** [docs/superpowers/specs/2026-06-06-atx-tsdb-v2-zero-copy-panel-design.md](../specs/2026-06-06-atx-tsdb-v2-zero-copy-panel-design.md)

---

## Conventions (read once)

- **Build** (from a VS Developer shell with `VCPKG_ROOT` set, or via the helper pattern below; `build/` is already configured — reuse it):
  - Reconfigure only if a `CMakeLists.txt` changed or a new source file must be globbed: `cmake --preset ninja -DATX_BUILD_TESTS=ON -DATX_BUILD_BENCH=ON`
  - Build tsdb tests: `cmake --build build --target atx-tsdb-tests`
  - Build engine tests: `cmake --build build --target atx-engine-tests`
  - Build bench: `cmake --build build --target atx-engine-bench`
  - Run one suite: `ctest --test-dir build -R <SuiteRegex> --output-on-failure` (note: a `|` in the regex must not reach `cmd` unquoted — run the test exe directly with `--gtest_filter` if piping is painful on Windows).
- **MSVC env helper** (this repo builds with clang-cl + Ninja, which need the VS environment). If `ninja`/`VCPKG_ROOT` are absent from your shell, create a one-line batch wrapper and run build commands through it:
  ```bat
  @echo off
  call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
  set "VCPKG_ROOT=C:\Users\natha\vcpkg"
  cd /d "C:\Users\natha\OneDrive\Desktop\atx"
  %*
  ```
  Then e.g. `& .\_build_helper.bat cmake --build build --target atx-tsdb-tests`. Delete the helper when done (it is not part of the deliverable).
- **Gates per unit:** `/W4 /permissive- /WX` build green; tests green; clang-format clean (`clang-format -i <files>`). NOTE: clang-tidy is intentionally disabled repo-wide (`.clang-tidy` is `Checks: '-*'`) — do **not** expect tidy output; the gate is the warning-as-error build + clang-format + tests.
- **Commit style:** `feat(tsdb-vN): …` / `test(tsdb-vN): …` with trailer `Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>`. Stage **explicit pathspecs** (shared branch — never `git add -A`). After each commit verify reachability: `git merge-base --is-ancestor <sha> HEAD`.
- **This is a shared branch** with concurrent Phase-3/Phase-4 work. Only stage the files each task names. Do not touch `.clang-tidy`, `.clangd`, plan/research docs, or `alpha/*` files outside the ones this plan edits.
- **Safety-critical C++:** `Result<T>` not exceptions; `const`/`constexpr`/`noexcept`/`[[nodiscard]]`; functions ≤60 lines; every `reinterpret_cast` over mapped bytes carries a `// SAFETY:` comment and a `NOLINTNEXTLINE` where the existing code does (mirror `segment.hpp`).

---

## File Structure

| Path | Change | Responsibility |
|------|--------|----------------|
| `atx-tsdb/include/atx/tsdb/segment.hpp` | Modify | Bump `kFormatVersion=2`; add `is_supported_version`, `field_block_stride_bytes`, `field_block_view`; make `field_block` stride-aware. |
| `atx-tsdb/tests/segment_test.cpp` | Modify | Stride math, version-policy, and `field_block_view` addressing tests. |
| `atx-tsdb/include/atx/tsdb/segment_reader.hpp` | Modify | Add public `field_block_view(field)` and `field_name(field)` accessors. |
| `atx-tsdb/src/segment_reader.cpp` | Modify | `validate()` uses `is_supported_version`; implement `field_name`. |
| `atx-tsdb/tests/segment_reader_test.cpp` | Modify | v2 round-trip; `field_block_view`/`field_name` accessor tests. |
| `atx-tsdb/src/builder.cpp` | Modify | 64-byte-align `off_field_blocks`; write each field block at a 64-byte-padded stride. |
| `atx-tsdb/tests/builder_test.cpp` | Modify | Field blocks 64-byte aligned; values round-trip through the padded layout. |
| `atx-engine/include/atx/engine/alpha/panel.hpp` | Modify | `alpha::Panel` borrowing backend: `columns_` spans + optional owned `backing_`; add `create_borrowed`. Read surface unchanged. |
| `atx-engine/tests/alpha_panel_test.cpp` | Modify | Borrowed-vs-owned equivalence tests (existing owned tests must still pass). |
| `atx-engine/include/atx/engine/alpha/segment_panel.hpp` | Create | `TimeWindow`, `UniversePolicy`, `MappedPanel`, `attach_segment_panel`. |
| `atx-engine/tests/segment_panel_test.cpp` | Create | Bridge mechanics + the `VM(shm)==VM(owned)==oracle` golden differential. |
| `atx-engine/bench/panel_read_bench.cpp` | Create | GB/s + ns/cell for zero-copy attach+evaluate vs memcpy-load. |

No CMake edits are required: `atx-tsdb/tests`, `atx-engine/tests`, and `atx-engine/bench` all auto-glob (`CONFIGURE_DEPENDS`). A reconfigure picks up the two new files.

---

# Phase P0 — Segment format v2 (64-byte-aligned field blocks)

### Task 1: `segment.hpp` — v2 constants + stride-aware addressing

**Files:**
- Modify: `atx-tsdb/include/atx/tsdb/segment.hpp`
- Test: `atx-tsdb/tests/segment_test.cpp`

- [ ] **Step 1: Write the failing tests**

Append to `atx-tsdb/tests/segment_test.cpp` (before the final `}` if any namespace; these are free `TEST`s at file scope):

```cpp
TEST(Segment_VersionPolicy_AcceptsOneAndTwo, SupportedVersions) {
  EXPECT_FALSE(atx::tsdb::is_supported_version(0U));
  EXPECT_TRUE(atx::tsdb::is_supported_version(1U));
  EXPECT_TRUE(atx::tsdb::is_supported_version(2U));
  EXPECT_FALSE(atx::tsdb::is_supported_version(3U));
}

TEST(Segment_Stride_PacksV1PadsV2, BlockStride) {
  atx::tsdb::SegmentHeader h{};
  h.time_count = 3;       // T
  h.instrument_count = 2; // N  -> packed = 3*2*8 = 48 bytes
  h.format_version = 1U;
  EXPECT_EQ(atx::tsdb::field_block_stride_bytes(h), 48U); // v1: packed
  h.format_version = 2U;
  EXPECT_EQ(atx::tsdb::field_block_stride_bytes(h), 64U); // v2: align_up(48,64)
}

// Lay out a v2 grid by hand at the padded stride and verify field_block_view
// addresses the right block (offsets only — in-memory 64B alignment is a
// builder/mmap property covered in Task 3).
TEST(Segment_FieldBlockView_HonorsV2Stride, ViewAddressing) {
  constexpr atx::u32 F = 2, N = 2;
  constexpr atx::u64 T = 3;                 // packed = 48B -> stride 64B = 8 f64
  constexpr atx::usize kStrideElems = 8;    // 64 / sizeof(f64)
  std::array<atx::f64, F * kStrideElems> buf{};
  // field 1, (t=2, inst=1) -> within-block index t*N+inst = 5; block 1 base = 8.
  buf[1 * kStrideElems + 5] = 42.0;

  atx::tsdb::SegmentHeader h{};
  h.format_version = 2U;
  h.field_count = F;
  h.instrument_count = N;
  h.time_count = T;
  h.off_field_blocks = 0;

  const auto *base = reinterpret_cast<const atx::u8 *>(buf.data());
  const std::span<const atx::f64> b1 = atx::tsdb::field_block_view(base, h, /*field=*/1);
  ASSERT_EQ(b1.size(), static_cast<atx::usize>(T * N)); // view length is T*N (no padding)
  EXPECT_DOUBLE_EQ(b1[2 * N + 1], 42.0);
}
```

Ensure `#include <array>` and `#include <span>` are present at the top of `segment_test.cpp` (add any missing).

- [ ] **Step 2: Run to verify it fails**

Run: `cmake --build build --target atx-tsdb-tests`
Expected: compile error — `is_supported_version` / `field_block_stride_bytes` / `field_block_view` not declared.

- [ ] **Step 3: Edit `segment.hpp`**

(a) Bump the version constant:
```cpp
inline constexpr atx::u32 kFormatVersion = 2U; // v2: 64B-aligned field blocks
```

(b) Immediately after the `kFieldNameLen` line, add:
```cpp
inline constexpr atx::u64 kBlockAlign = 64U; // field-block byte alignment in v2 (cache line / AVX-512)

/// True iff this reader can interpret an on-disk `version`. v1 (packed blocks)
/// and v2 (64B-padded blocks) are both addressable via header offsets.
[[nodiscard]] constexpr bool is_supported_version(atx::u32 version) noexcept {
  return version >= 1U && version <= kFormatVersion;
}
```

(c) In the "Geometry + addressing" section, **before** `field_block`, add the stride helper:
```cpp
/// Bytes from one field block's start to the next. v1 packs blocks (stride ==
/// T*N*8); v2 pads each block up to a 64-byte boundary so the block start is
/// SIMD-aligned in the mapping. The block's live data is always the first T*N
/// f64; the padding is trailing and never read.
[[nodiscard]] constexpr atx::u64 field_block_stride_bytes(const SegmentHeader &h) noexcept {
  const atx::u64 packed = h.time_count * static_cast<atx::u64>(h.instrument_count) * sizeof(atx::f64);
  if (h.format_version >= 2U) {
    return (packed + (kBlockAlign - 1U)) & ~(kBlockAlign - 1U);
  }
  return packed;
}
```

(d) Replace the body of `field_block` so the per-block offset uses the stride:
```cpp
[[nodiscard]] inline const atx::f64 *field_block(const atx::u8 *base, const SegmentHeader &h,
                                                 atx::u32 field) noexcept {
  const atx::u64 byte_off =
      h.off_field_blocks + static_cast<atx::u64>(field) * field_block_stride_bytes(h);
  // SAFETY: byte_off < total_bytes by construction (validated at attach). The
  // section is laid out f64-aligned (64B-aligned in v2), so the reinterpret is
  // well-defined.
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast,cppcoreguidelines-pro-bounds-pointer-arithmetic)
  return reinterpret_cast<const atx::f64 *>(base + byte_off);
}
```

(e) After `field_block`, add the whole-block view helper:
```cpp
/// Non-owning view of field block `field` (length T*N, padding excluded).
[[nodiscard]] inline std::span<const atx::f64>
field_block_view(const atx::u8 *base, const SegmentHeader &h, atx::u32 field) noexcept {
  return {field_block(base, h, field),
          static_cast<atx::usize>(h.time_count * h.instrument_count)};
}
```

- [ ] **Step 4: Run to verify it passes**

Run: `cmake --build build --target atx-tsdb-tests && ctest --test-dir build -R "Segment_" --output-on-failure`
Expected: all `Segment_*` tests PASS (existing + 3 new).

- [ ] **Step 5: Format + commit**

```
clang-format -i atx-tsdb/include/atx/tsdb/segment.hpp atx-tsdb/tests/segment_test.cpp
git add atx-tsdb/include/atx/tsdb/segment.hpp atx-tsdb/tests/segment_test.cpp
git commit -m "feat(tsdb-v2-1): format v2 constants + stride-aware field-block addressing"
```

---

### Task 2: `SegmentReader` — version policy + `field_block_view` / `field_name`

**Files:**
- Modify: `atx-tsdb/include/atx/tsdb/segment_reader.hpp`
- Modify: `atx-tsdb/src/segment_reader.cpp`
- Test: `atx-tsdb/tests/segment_reader_test.cpp`

- [ ] **Step 1: Write the failing tests**

Append to `atx-tsdb/tests/segment_reader_test.cpp` (these reuse the file's existing `make_segment` helper, which builds fields `{"close","volume"}`, symbols `{"AAA","BBB"}`, axis `{100,200,300}` and sets close AAA@t0=10.0, close BBB@t2=20.0):

```cpp
TEST(Reader_FieldName_RoundTrips, NamesByIndex) {
  const std::string path = make_segment("fn1");
  auto r = atx::tsdb::SegmentReader::attach(path);
  ASSERT_TRUE(r.has_value()) << (r ? "" : r.error().to_string());
  EXPECT_EQ(r->field_name(0), "close");
  EXPECT_EQ(r->field_name(1), "volume");
  std::remove(path.c_str());
}

TEST(Reader_FieldBlockView_ExposesWholeBlock, BlockView) {
  const std::string path = make_segment("fb1");
  auto r = atx::tsdb::SegmentReader::attach(path);
  ASSERT_TRUE(r.has_value());
  const atx::u32 close = *r->field_index("close");
  const std::span<const atx::f64> block = r->field_block_view(close);
  ASSERT_EQ(block.size(), r->time_count() * r->instrument_count()); // T*N = 3*2
  // date-major [t][inst]: close AAA@t0 (inst 0, t 0) == 10.0
  EXPECT_DOUBLE_EQ(block[0 * r->instrument_count() + 0], 10.0);
  // close BBB@t2 (inst 1, t 2)
  EXPECT_DOUBLE_EQ(block[2 * r->instrument_count() + 1], 20.0);
  std::remove(path.c_str());
}

TEST(Reader_FieldBlockView_Is64ByteAligned, Aligned) {
  const std::string path = make_segment("fb2");
  auto r = atx::tsdb::SegmentReader::attach(path);
  ASSERT_TRUE(r.has_value());
  for (atx::u32 f = 0; f < r->field_count(); ++f) {
    const auto addr = reinterpret_cast<std::uintptr_t>(r->field_block_view(f).data());
    EXPECT_EQ(addr % 64U, 0U) << "field " << f << " block not 64B-aligned";
  }
  std::remove(path.c_str());
}
```

Ensure `#include <cstdint>` and `#include <span>` are present at the top of `segment_reader_test.cpp`.

- [ ] **Step 2: Run to verify it fails**

Run: `cmake --build build --target atx-tsdb-tests`
Expected: compile error — no member `field_name` / `field_block_view` on `SegmentReader`. (The alignment test will additionally fail until Task 3.)

- [ ] **Step 3: Edit `segment_reader.hpp`**

Add the includes near the top (if missing): `#include <cstdint>`. Then, inside `class SegmentReader`'s public section, after the `times()` method, add:

```cpp
  /// Field name for directory index `field`. PRECONDITION: field < F (ABORTS).
  [[nodiscard]] std::string_view field_name(atx::u32 field) const noexcept;

  /// Whole field block `field` as a non-owning view (length T*N, date-major).
  /// PRECONDITION: field < F (ABORTS in debug; addressing is otherwise UB).
  [[nodiscard]] std::span<const atx::f64> field_block_view(atx::u32 field) const noexcept {
    return atx::tsdb::field_block_view(map_.base(), header(), field);
  }
```

- [ ] **Step 4: Edit `segment_reader.cpp`**

(a) In `validate()`, replace the version check:
```cpp
  if (h.format_version != kFormatVersion) {
    return atx::core::Error{ErrorCode::InvalidArgument, "unsupported format version"};
  }
```
with:
```cpp
  if (!is_supported_version(h.format_version)) {
    return atx::core::Error{ErrorCode::InvalidArgument, "unsupported format version"};
  }
```

(b) Add `#include "atx/core/macro.hpp"` (for `ATX_ASSERT`) and `#include <cstring>` (for `::strnlen`) if not already present. Then add the `field_name` definition (mirrors `field_index`'s directory walk; place it next to `field_index`):
```cpp
std::string_view SegmentReader::field_name(atx::u32 field) const noexcept {
  const SegmentHeader &h = header();
  ATX_ASSERT(field < h.field_count);
  // SAFETY: directory is F contiguous FieldEntry, validated in range; read-only.
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast,cppcoreguidelines-pro-bounds-pointer-arithmetic)
  const auto *dir = reinterpret_cast<const FieldEntry *>(map_.base() + h.off_field_dir);
  // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
  const FieldEntry &e = dir[field];
  const atx::usize len = ::strnlen(e.name, kFieldNameLen);
  return std::string_view{e.name, len};
}
```

- [ ] **Step 5: Run — `field_name`/`field_block_view` pass; alignment test still RED**

Run: `cmake --build build --target atx-tsdb-tests && ctest --test-dir build -R "Reader_FieldName|Reader_FieldBlockView_ExposesWholeBlock" --output-on-failure`
Expected: those two PASS. `Reader_FieldBlockView_Is64ByteAligned` stays RED until Task 3 (the builder doesn't yet 64B-align).

- [ ] **Step 6: Format + commit** (alignment test committed RED here is acceptable — it is the failing test for Task 3; note it in the message)

```
clang-format -i atx-tsdb/include/atx/tsdb/segment_reader.hpp atx-tsdb/src/segment_reader.cpp atx-tsdb/tests/segment_reader_test.cpp
git add atx-tsdb/include/atx/tsdb/segment_reader.hpp atx-tsdb/src/segment_reader.cpp atx-tsdb/tests/segment_reader_test.cpp
git commit -m "feat(tsdb-v2-2): reader accepts v1/v2 + field_name/field_block_view accessors"
```

---

### Task 3: `SegmentBuilder` — 64-byte-aligned field blocks

**Files:**
- Modify: `atx-tsdb/src/builder.cpp`
- Test: `atx-tsdb/tests/builder_test.cpp`

The failing test already exists from Task 2 (`Reader_FieldBlockView_Is64ByteAligned`). Add one more builder-side test, then implement.

- [ ] **Step 1: Write an additional failing test**

Append to `atx-tsdb/tests/builder_test.cpp`:

```cpp
TEST(Builder_V2Layout_AlignsBlocksAndRoundTrips, AlignedValues) {
  // 2 fields x 3 times x 5 instruments -> packed block = 3*5*8 = 120B,
  // padded stride = align_up(120,64) = 128B. The padding must not corrupt reads.
  atx::tsdb::SegmentBuilder b({"a", "b"}, {"S0", "S1", "S2", "S3", "S4"}, {10, 20, 30});
  b.set(/*field=*/0, /*t=*/2, /*inst=*/4, 7.5); // a @ t2,inst4
  b.set(/*field=*/1, /*t=*/0, /*inst=*/0, 9.25); // b @ t0,inst0
  const std::string path = std::string(std::tmpnam(nullptr)) + "v2lay.atxseg";
  ASSERT_TRUE(b.write(path, /*created_at_nanos=*/0).has_value());

  auto r = atx::tsdb::SegmentReader::attach(path);
  ASSERT_TRUE(r.has_value()) << (r ? "" : r.error().to_string());
  const atx::u32 fa = *r->field_index("a");
  const atx::u32 fb = *r->field_index("b");
  EXPECT_DOUBLE_EQ(r->value(fa, 2, 4), 7.5);
  EXPECT_DOUBLE_EQ(r->value(fb, 0, 0), 9.25);
  // block starts 64B-aligned and one padded stride (128B = 16 f64) apart.
  const auto a0 = reinterpret_cast<std::uintptr_t>(r->field_block_view(fa).data());
  const auto b0 = reinterpret_cast<std::uintptr_t>(r->field_block_view(fb).data());
  EXPECT_EQ(a0 % 64U, 0U);
  EXPECT_EQ(b0 % 64U, 0U);
  EXPECT_EQ(b0 - a0, 128U);
  std::remove(path.c_str());
}
```

Ensure `#include <cstdint>` is present in `builder_test.cpp`.

- [ ] **Step 2: Run to verify it fails**

Run: `cmake --build build --target atx-tsdb-tests`
Then: `ctest --test-dir build -R "Builder_V2Layout|Reader_FieldBlockView_Is64ByteAligned" --output-on-failure`
Expected: both FAIL (blocks currently packed at 8B alignment; `b0 - a0 == 120`, not 128, and base offset not 64-aligned).

- [ ] **Step 3: Edit `builder.cpp` — offset computation**

Replace the offsets block (the section from `atx::u64 off = sizeof(SegmentHeader);` down through `const atx::u64 total = off_footer + sizeof(SegmentFooter);`) with:

```cpp
  // --- section offsets -------------------------------------------------------
  atx::u64 off = sizeof(SegmentHeader);

  const atx::u64 off_field_dir = off;
  off += static_cast<atx::u64>(f) * sizeof(FieldEntry);

  const atx::u64 off_symbol_table = off;
  off += static_cast<atx::u64>(n) * sizeof(SymbolEntry);

  const atx::u64 off_string_blob = off;
  off += static_cast<atx::u64>(blob.size());
  off = align_up(off, 8U); // i64 time axis needs 8B alignment

  const atx::u64 off_time_axis = off;
  off += t * sizeof(atx::i64);

  // v2: 64-byte-align the field-block section AND pad each block to a 64-byte
  // stride, so every block start is cache-line / AVX-512 aligned in the mapping.
  off = align_up(off, kBlockAlign);
  const atx::u64 off_field_blocks = off;
  const atx::u64 block_packed = t * static_cast<atx::u64>(n) * sizeof(atx::f64);
  const atx::u64 block_stride = align_up(block_packed, kBlockAlign);
  off += static_cast<atx::u64>(f) * block_stride;

  const atx::u64 off_present_bitmap = off;
  off += t * mw * sizeof(atx::u64);

  const atx::u64 off_footer = off;
  const atx::u64 total = off_footer + sizeof(SegmentFooter);
```

- [ ] **Step 4: Edit `builder.cpp` — per-block write**

Replace the field-blocks write:
```cpp
  // Field blocks (F * T * N f64, row-major within each field).
  if (!blocks_.empty()) {
    put(off_field_blocks, blocks_.data(), blocks_.size() * sizeof(atx::f64));
  }
```
with a per-block copy at the padded stride (the in-RAM `blocks_` stays packed `[field][t][inst]`; the file gets the padding between blocks):
```cpp
  // Field blocks: copy each field's packed T*N f64 to its 64B-padded slot. The
  // inter-block padding stays zero (buf is value-initialised), so content_hash
  // is deterministic.
  for (atx::u32 i = 0; i < f; ++i) {
    const atx::u64 src_elems = t * static_cast<atx::u64>(n);
    if (src_elems == 0U) {
      break; // no data to write (T or N is zero)
    }
    // SAFETY: src is blocks_[i*T*N .. +T*N) (packed in RAM); dst is the i-th
    // 64B-padded block in buf. Both ranges are in bounds by construction.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
    put(off_field_blocks + static_cast<atx::u64>(i) * block_stride,
        blocks_.data() + static_cast<atx::usize>(static_cast<atx::u64>(i) * src_elems),
        static_cast<atx::usize>(src_elems) * sizeof(atx::f64));
  }
```

- [ ] **Step 5: Run to verify it passes (and no tsdb regression)**

Run: `cmake --build build --target atx-tsdb-tests`
Then run the whole suite directly (avoids `|`-in-regex on Windows): `build/bin/atx-tsdb-tests.exe`
Expected: all tests PASS, including `Builder_V2Layout_*` and `Reader_FieldBlockView_Is64ByteAligned`, and the existing determinism/header tests (`Builder_BuildTwice_ByteIdentical`, `Builder_WriteSealedFile_*`, `Reader_*`).

- [ ] **Step 6: Format + commit**

```
clang-format -i atx-tsdb/src/builder.cpp atx-tsdb/tests/builder_test.cpp
git add atx-tsdb/src/builder.cpp atx-tsdb/tests/builder_test.cpp
git commit -m "feat(tsdb-v2-3): builder writes 64B-aligned padded field blocks"
```

---

# Phase P1 — Borrowing `alpha::Panel` backend

### Task 4: `alpha::Panel` — owned-or-borrowed columns

**Files:**
- Modify: `atx-engine/include/atx/engine/alpha/panel.hpp`
- Test: `atx-engine/tests/alpha_panel_test.cpp`

The contract: `create(...)` (owned) stays byte-for-byte identical (many tests + `Engine`/`Oracle` depend on it); add `create_borrowed(...)`; the read surface (`field_all`, `field_cross_section`, `in_universe`, `field_id`, `dates`, `instruments`, `num_fields`, `cells`) is unchanged so `Engine`/`Oracle` need no edit.

- [ ] **Step 1: Write the failing test**

Append to `atx-engine/tests/alpha_panel_test.cpp`:

```cpp
TEST(AlphaPanel_Borrowed_MatchesOwned, BorrowedReadSurface) {
  // Two fields, 2 dates x 3 instruments, date-major columns.
  const std::vector<atx::f64> close{1, 2, 3, 4, 5, 6};   // d0=[1,2,3] d1=[4,5,6]
  const std::vector<atx::f64> volume{10, 20, 30, 40, 50, 60};
  const std::vector<std::uint8_t> uni{1, 0, 1, 1, 1, 0};

  // Borrowed: column spans alias the vectors above (which outlive the panel).
  std::vector<std::span<const atx::f64>> cols{std::span<const atx::f64>{close},
                                              std::span<const atx::f64>{volume}};
  auto bp = Panel::create_borrowed(2, 3, {"close", "volume"}, std::move(cols), uni);
  ASSERT_TRUE(bp.has_value()) << (bp ? "" : bp.error().message());
  const Panel &p = bp.value();

  EXPECT_EQ(p.dates(), 2U);
  EXPECT_EQ(p.instruments(), 3U);
  EXPECT_EQ(p.num_fields(), 2U);
  const auto cid = p.field_id("close");
  ASSERT_TRUE(cid.has_value());
  const auto cs1 = p.field_cross_section(cid.value(), 1); // d1 = [4,5,6]
  ASSERT_EQ(cs1.size(), 3U);
  EXPECT_DOUBLE_EQ(cs1[0], 4.0);
  EXPECT_DOUBLE_EQ(cs1[2], 6.0);
  EXPECT_DOUBLE_EQ(p.field_all(cid.value())[0], 1.0);
  EXPECT_TRUE(p.in_universe(0, 0));
  EXPECT_FALSE(p.in_universe(0, 1));
}

TEST(AlphaPanel_Borrowed_RaggedColumn_Errs, BorrowedValidation) {
  const std::vector<atx::f64> good{1, 2};
  const std::vector<atx::f64> bad{1, 2, 3}; // wrong length for 1x2
  std::vector<std::span<const atx::f64>> cols{std::span<const atx::f64>{good},
                                              std::span<const atx::f64>{bad}};
  auto bp = Panel::create_borrowed(1, 2, {"a", "b"}, std::move(cols), {});
  EXPECT_FALSE(bp.has_value());
}

TEST(AlphaPanel_BorrowedSurvivesPanelMove, BorrowedMoveStable) {
  const std::vector<atx::f64> close{1, 2, 3, 4};
  std::vector<std::span<const atx::f64>> cols{std::span<const atx::f64>{close}};
  auto bp = Panel::create_borrowed(2, 2, {"close"}, std::move(cols), {});
  ASSERT_TRUE(bp.has_value());
  Panel moved = std::move(bp.value()); // borrowed spans point at `close`, unaffected
  EXPECT_DOUBLE_EQ(moved.field_all(moved.field_id("close").value())[3], 4.0);
}
```

Ensure `#include <span>` and `#include <utility>` are present in `alpha_panel_test.cpp`.

- [ ] **Step 2: Run to verify it fails**

Run: `cmake --build build --target atx-engine-tests`
Expected: compile error — no `Panel::create_borrowed`.

- [ ] **Step 3: Edit `panel.hpp` — storage + `create` + `create_borrowed`**

(a) Add `#include <span>` to the include list (alongside the existing `<vector>` etc.).

(b) Replace the private data members:
```cpp
  atx::usize dates_{};
  atx::usize instruments_{};
  std::vector<std::string> field_names_;
  std::vector<std::vector<atx::f64>> field_data_; // one column per field, date-major
  std::vector<std::uint8_t> universe_;            // dates*instruments, 1 == in-universe
```
with:
```cpp
  atx::usize dates_{};
  atx::usize instruments_{};
  std::vector<std::string> field_names_;
  // Access plane: one span per field (date-major, dates*instruments). Spans point
  // either into `backing_` (owned panels) or into caller/mmap memory (borrowed
  // panels). Every read goes through here, so owned and borrowed share one code
  // path with no per-access branch.
  std::vector<std::span<const atx::f64>> columns_;
  // Owned column storage; EMPTY for a borrowed panel. SAFETY: moving a Panel
  // moves this outer vector by buffer transfer (no reallocation), so the inner
  // vectors' data() pointers — and therefore the spans in columns_ that alias
  // them — stay valid across the create()-return-by-value move.
  std::vector<std::vector<atx::f64>> backing_;
  std::vector<std::uint8_t> universe_; // dates*instruments, 1 == in-universe
```

(c) Replace the body of `create(...)` (keep the SAME signature and all validation) so it fills `backing_` then points `columns_` at it. The validation prologue is unchanged; only the assignment epilogue changes:
```cpp
    Panel p;
    p.dates_ = dates;
    p.instruments_ = instruments;
    p.field_names_ = std::move(field_names);
    p.backing_ = std::move(field_data);
    p.columns_.reserve(p.backing_.size());
    for (const std::vector<atx::f64> &col : p.backing_) {
      p.columns_.emplace_back(col.data(), col.size());
    }
    if (universe.empty()) {
      p.universe_.assign(cells, std::uint8_t{1});
    } else {
      p.universe_ = std::move(universe);
    }
    return atx::core::Ok(std::move(p));
```

(d) Add `create_borrowed` right after `create`:
```cpp
  // Build a Panel whose field columns are BORROWED (no copy). Each span in
  // `columns` must have exactly dates*instruments cells and must outlive the
  // Panel (caller's contract — e.g. a mmap held by a MappedPanel). The universe
  // mask is still OWNED (materialized small mask); empty == all-in-universe.
  [[nodiscard]] static atx::core::Result<Panel>
  create_borrowed(atx::usize dates, atx::usize instruments, std::vector<std::string> field_names,
                  std::vector<std::span<const atx::f64>> columns,
                  std::vector<std::uint8_t> universe) {
    if (field_names.size() != columns.size()) {
      return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                            "Panel::create_borrowed: field_names and columns size mismatch");
    }
    const atx::usize cells = dates * instruments;
    for (const std::span<const atx::f64> &col : columns) {
      if (col.size() != cells) {
        return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                              "Panel::create_borrowed: a column is not dates*instruments cells");
      }
    }
    if (!universe.empty() && universe.size() != cells) {
      return atx::core::Err(
          atx::core::ErrorCode::InvalidArgument,
          "Panel::create_borrowed: universe is neither empty nor dates*instruments cells");
    }
    Panel p;
    p.dates_ = dates;
    p.instruments_ = instruments;
    p.field_names_ = std::move(field_names);
    p.columns_ = std::move(columns); // borrowed; backing_ stays empty
    if (universe.empty()) {
      p.universe_.assign(cells, std::uint8_t{1});
    } else {
      p.universe_ = std::move(universe);
    }
    return atx::core::Ok(std::move(p));
  }
```

(e) Replace the two column accessors to read through `columns_`:
```cpp
  [[nodiscard]] std::span<const atx::f64> field_cross_section(FieldId field,
                                                              DateIdx date) const noexcept {
    ATX_ASSERT(field < columns_.size());
    ATX_ASSERT(date < dates_);
    return columns_[field].subspan(date * instruments_, instruments_);
  }

  [[nodiscard]] std::span<const atx::f64> field_all(FieldId field) const noexcept {
    ATX_ASSERT(field < columns_.size());
    return columns_[field];
  }
```

`num_fields()` already returns `field_names_.size()` — unchanged and consistent with `columns_.size()`.

- [ ] **Step 4: Run to verify it passes (and no alpha regression)**

Run: `cmake --build build --target atx-engine-tests`
Then run the panel + VM + oracle suites directly: `build/bin/atx-engine-tests.exe --gtest_filter=AlphaPanel_*:AlphaVm_*:AlphaOracle_*`
Expected: all PASS — the new borrowed tests and every existing owned-Panel test (the owned path is behaviorally identical).

- [ ] **Step 5: Format + commit**

```
clang-format -i atx-engine/include/atx/engine/alpha/panel.hpp atx-engine/tests/alpha_panel_test.cpp
git add atx-engine/include/atx/engine/alpha/panel.hpp atx-engine/tests/alpha_panel_test.cpp
git commit -m "feat(tsdb-v2-4): alpha::Panel borrowing column backend (create_borrowed)"
```

---

# Phase P2/P3 — The segment→Panel bridge

### Task 5: `segment_panel.hpp` — `MappedPanel` + `attach_segment_panel`

**Files:**
- Create: `atx-engine/include/atx/engine/alpha/segment_panel.hpp`
- Test: `atx-engine/tests/segment_panel_test.cpp`

- [ ] **Step 1: Write the failing test**

Create `atx-engine/tests/segment_panel_test.cpp`:

```cpp
// Bridge mechanics: attach_segment_panel produces a borrowed alpha::Panel over a
// sealed segment (window slice, field projection, universe policy, move safety).
// The VM(shm)==VM(owned)==oracle golden differential is added in Task 6.

#include <gtest/gtest.h>

#include <cstdio>
#include <span>
#include <string>
#include <vector>

#include "atx/core/types.hpp"

#include "atx/engine/alpha/segment_panel.hpp"

#include "atx/tsdb/builder.hpp"

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
// NOLINTBEGIN(misc-include-cleaner)
#include <windows.h>
// NOLINTEND(misc-include-cleaner)
#endif

namespace {

std::string temp_path(const std::string &name) {
#if defined(_WIN32)
  // NOLINTBEGIN(misc-include-cleaner)
  // NOLINTBEGIN(cppcoreguidelines-avoid-c-arrays,modernize-avoid-c-arrays)
  wchar_t tmp_dir[MAX_PATH + 1]{};
  GetTempPathW(MAX_PATH + 1, tmp_dir);
  wchar_t tmp_file[MAX_PATH + 1]{};
  GetTempFileNameW(tmp_dir, L"atx", 0, tmp_file);
  // NOLINTEND(cppcoreguidelines-avoid-c-arrays,modernize-avoid-c-arrays)
  std::wstring wpath(tmp_file);
  const std::wstring wsuffix(name.begin(), name.end());
  wpath += wsuffix + L".atxseg";
  const std::string path(wpath.begin(), wpath.end());
  // NOLINTEND(misc-include-cleaner)
#else
  // NOLINTNEXTLINE(cppcoreguidelines-avoid-c-arrays,modernize-avoid-c-arrays)
  char buf[L_tmpnam]{};
  // NOLINTNEXTLINE(cert-msc50-cpp,cert-msc30-c)
  std::tmpnam(buf);
  const std::string path = std::string(buf) + name + ".atxseg";
#endif
  return path;
}

// 3 dates x 2 instruments, fields close/volume; close = date*10+inst, all present
// except (t2, inst1) which is left absent (present bit clear).
std::string make_seg(const std::string &name) {
  atx::tsdb::SegmentBuilder b({"close", "volume"}, {"AAA", "BBB"}, {100, 200, 300});
  for (atx::u64 t = 0; t < 3; ++t) {
    for (atx::u32 i = 0; i < 2; ++i) {
      if (t == 2 && i == 1) {
        continue; // absent cell
      }
      b.set(0, t, i, static_cast<atx::f64>(t * 10 + i)); // close
      b.set(1, t, i, 100.0);                             // volume
    }
  }
  const std::string path = temp_path(name);
  EXPECT_TRUE(b.write(path, 0).has_value());
  return path;
}

using atx::engine::alpha::attach_segment_panel;
using atx::engine::alpha::TimeWindow;
using atx::engine::alpha::UniverseKind;
using atx::engine::alpha::UniversePolicy;

} // namespace

TEST(SegmentPanel_FullWindowAllFields_Mirrors, FullWindow) {
  const std::string path = make_seg("sp1");
  auto mp = attach_segment_panel(path);
  ASSERT_TRUE(mp.has_value()) << (mp ? "" : mp.error().to_string());
  const auto &panel = mp->panel();
  EXPECT_EQ(panel.dates(), 3U);
  EXPECT_EQ(panel.instruments(), 2U);
  EXPECT_EQ(panel.num_fields(), 2U);
  const auto cid = panel.field_id("close");
  ASSERT_TRUE(cid.has_value());
  EXPECT_DOUBLE_EQ(panel.field_cross_section(cid.value(), 1)[0], 10.0); // t1,inst0 = 10
  EXPECT_TRUE(panel.in_universe(0, 0));
  EXPECT_FALSE(panel.in_universe(2, 1)); // absent cell -> out of universe
  std::remove(path.c_str());
}

TEST(SegmentPanel_SubWindow_SlicesDates, SubWindow) {
  const std::string path = make_seg("sp2");
  auto mp = attach_segment_panel(path, TimeWindow{/*start*/ 200, /*end*/ 400}); // dates t1,t2
  ASSERT_TRUE(mp.has_value());
  const auto &panel = mp->panel();
  EXPECT_EQ(panel.dates(), 2U); // {200, 300}
  const auto cid = panel.field_id("close");
  ASSERT_TRUE(cid.has_value());
  EXPECT_DOUBLE_EQ(panel.field_cross_section(cid.value(), 0)[0], 10.0); // first row is t1
  std::remove(path.c_str());
}

TEST(SegmentPanel_FieldProjection_SelectsSubset, Projection) {
  const std::string path = make_seg("sp3");
  const std::vector<std::string> fields{"close"};
  auto mp = attach_segment_panel(path, TimeWindow{}, fields);
  ASSERT_TRUE(mp.has_value());
  EXPECT_EQ(mp->panel().num_fields(), 1U);
  EXPECT_TRUE(mp->panel().field_id("close").has_value());
  EXPECT_FALSE(mp->panel().field_id("volume").has_value());
  std::remove(path.c_str());
}

TEST(SegmentPanel_UnknownField_Errs, UnknownField) {
  const std::string path = make_seg("sp4");
  const std::vector<std::string> fields{"nope"};
  auto mp = attach_segment_panel(path, TimeWindow{}, fields);
  EXPECT_FALSE(mp.has_value());
  std::remove(path.c_str());
}

TEST(SegmentPanel_MovePreservesBorrow, MoveSafe) {
  const std::string path = make_seg("sp5");
  auto mp = attach_segment_panel(path);
  ASSERT_TRUE(mp.has_value());
  auto moved = std::move(mp.value()); // mapping address stable -> spans valid
  EXPECT_DOUBLE_EQ(moved.panel().field_cross_section(moved.panel().field_id("close").value(), 1)[1],
                   11.0); // t1, inst1 = 11
  std::remove(path.c_str());
}
```

- [ ] **Step 2: Run to verify it fails**

Run: `cmake --preset ninja -DATX_BUILD_TESTS=ON -DATX_BUILD_BENCH=ON` (reconfigure to glob the new test), then `cmake --build build --target atx-engine-tests`
Expected: compile error — `atx/engine/alpha/segment_panel.hpp` not found.

- [ ] **Step 3: Create `segment_panel.hpp`**

```cpp
#pragma once

// atx::engine::alpha — the zero-copy bridge from a sealed atx-tsdb segment to a
// borrowed alpha::Panel the batch VM evaluates in place (atx-tsdb v2).
//
// attach_segment_panel maps a segment, slices a [start,end) date window, derives
// the point-in-time universe (present-bitmap by default, or an explicit field),
// and builds an alpha::Panel whose field columns ALIAS the mapped bytes — no
// copy, no f64<->Decimal conversion. The returned MappedPanel OWNS the mapping
// (via SegmentReader) and is MOVE-ONLY, so the borrowed Panel can never outlive
// the bytes it reads. Moving a MappedPanel is safe: the OS mapping address is
// stable across a Mapping move, so the Panel's column spans stay valid.

#include <algorithm> // std::lower_bound
#include <cmath>     // std::isnan
#include <limits>    // std::numeric_limits
#include <span>
#include <string>
#include <utility> // std::move
#include <vector>

#include "atx/core/error.hpp"
#include "atx/core/types.hpp"

#include "atx/engine/alpha/panel.hpp"

#include "atx/tsdb/segment_reader.hpp"

namespace atx::engine::alpha {

/// Half-open unix-nanos window [start_nanos, end_nanos). The default selects the
/// whole segment. end_nanos is the no-look-ahead cutoff (rows >= end are excluded).
struct TimeWindow {
  atx::i64 start_nanos{std::numeric_limits<atx::i64>::min()};
  atx::i64 end_nanos{std::numeric_limits<atx::i64>::max()};
};

/// Where the point-in-time universe mask comes from.
enum class UniverseKind : atx::u8 { PresentBitmap, Field };

struct UniversePolicy {
  UniverseKind kind{UniverseKind::PresentBitmap};
  std::string field_name{"universe"}; // used only when kind == Field (0/NaN => out)
};

/// Owns a sealed segment's mapping (via SegmentReader) plus a borrowed alpha::Panel
/// that aliases it. Move-only: the Panel is reachable only through this owner, so
/// it cannot outlive the mapping. The mapping address is stable across moves.
class MappedPanel {
public:
  MappedPanel(MappedPanel &&) noexcept = default;
  MappedPanel &operator=(MappedPanel &&) noexcept = default;
  MappedPanel(const MappedPanel &) = delete;
  MappedPanel &operator=(const MappedPanel &) = delete;
  ~MappedPanel() = default;

  /// The borrowed batch Panel (pass to alpha::Engine{panel()}).
  [[nodiscard]] const Panel &panel() const noexcept { return panel_; }

  /// The underlying reader (for content_hash, times, etc.).
  [[nodiscard]] const atx::tsdb::SegmentReader &reader() const noexcept { return reader_; }

  /// Make the whole mapping resident (best-effort; window-ranged prefetch is a
  /// deferred refinement — see the plan's Deferred section).
  void prefetch() const noexcept { reader_.prefetch(); }

  // Construction is via the factory below; public so Result/aggregate moves are
  // simple. Misuse (a panel not built from `reader`) is a programmer error.
  MappedPanel(atx::tsdb::SegmentReader reader, Panel panel) noexcept
      : reader_{std::move(reader)}, panel_{std::move(panel)} {}

private:
  // Declared reader_ FIRST so it is destroyed LAST (after panel_): the Panel's
  // borrowed spans must not dangle during its own destruction.
  atx::tsdb::SegmentReader reader_;
  Panel panel_;
};

namespace detail {

/// Materialize a dates*instruments u8 universe over the window [d0, d0+dates).
[[nodiscard]] inline std::vector<std::uint8_t>
universe_from_present(const atx::tsdb::SegmentReader &reader, atx::usize d0, atx::usize dates) {
  const atx::u32 n = reader.instrument_count();
  std::vector<std::uint8_t> uni(dates * n, std::uint8_t{0});
  for (atx::usize d = 0; d < dates; ++d) {
    for (atx::u32 j = 0; j < n; ++j) {
      uni[d * n + j] = reader.present(d0 + d, j) ? std::uint8_t{1} : std::uint8_t{0};
    }
  }
  return uni;
}

/// Materialize a universe from an explicit field column (cell != 0 and non-NaN).
[[nodiscard]] inline std::vector<std::uint8_t>
universe_from_field(const atx::tsdb::SegmentReader &reader, atx::u32 field, atx::usize d0,
                    atx::usize dates) {
  const atx::u32 n = reader.instrument_count();
  const std::span<const atx::f64> col = reader.field_block_view(field);
  std::vector<std::uint8_t> uni(dates * n, std::uint8_t{0});
  for (atx::usize d = 0; d < dates; ++d) {
    for (atx::u32 j = 0; j < n; ++j) {
      const atx::f64 v = col[(d0 + d) * n + j];
      uni[d * n + j] = (!std::isnan(v) && v != 0.0) ? std::uint8_t{1} : std::uint8_t{0};
    }
  }
  return uni;
}

} // namespace detail

/// Attach `path` and build a borrowed batch Panel over the window [start,end).
/// `fields` empty => all segment fields in segment order; otherwise exactly the
/// named fields, in the given order. Err(NotFound) if a named field (or the
/// universe field) is absent; propagates SegmentReader::attach errors.
[[nodiscard]] inline atx::core::Result<MappedPanel>
attach_segment_panel(const std::string &path, TimeWindow window = {},
                     std::span<const std::string> fields = {}, UniversePolicy universe = {}) {
  ATX_TRY(atx::tsdb::SegmentReader reader, atx::tsdb::SegmentReader::attach(path));

  // Resolve the half-open date window [d0, d1) over the ascending time axis.
  const std::span<const atx::i64> axis = reader.times();
  const auto lo = std::lower_bound(axis.begin(), axis.end(), window.start_nanos);
  const auto hi = std::lower_bound(axis.begin(), axis.end(), window.end_nanos);
  const atx::usize d0 = static_cast<atx::usize>(lo - axis.begin());
  const atx::usize d1 = static_cast<atx::usize>(hi - axis.begin());
  const atx::usize dates = d1 - d0;
  const atx::u32 n = reader.instrument_count();
  const atx::usize cells = dates * static_cast<atx::usize>(n);

  // Resolve the field set (names + segment indices).
  std::vector<std::string> names;
  std::vector<atx::u32> field_ids;
  if (fields.empty()) {
    names.reserve(reader.field_count());
    field_ids.reserve(reader.field_count());
    for (atx::u32 f = 0; f < reader.field_count(); ++f) {
      names.emplace_back(reader.field_name(f));
      field_ids.push_back(f);
    }
  } else {
    names.reserve(fields.size());
    field_ids.reserve(fields.size());
    for (const std::string &fn : fields) {
      const auto idx = reader.field_index(fn);
      if (!idx.has_value()) {
        return atx::core::Err(atx::core::ErrorCode::NotFound,
                              "attach_segment_panel: unknown field '" + fn + "'");
      }
      names.push_back(fn);
      field_ids.push_back(idx.value());
    }
  }

  // Build borrowed, windowed column spans (zero-copy: subspan into each block).
  std::vector<std::span<const atx::f64>> columns;
  columns.reserve(field_ids.size());
  for (const atx::u32 fid : field_ids) {
    const std::span<const atx::f64> block = reader.field_block_view(fid); // length T*N
    columns.push_back(block.subspan(d0 * static_cast<atx::usize>(n), cells));
  }

  // Universe mask.
  std::vector<std::uint8_t> uni;
  if (universe.kind == UniverseKind::Field) {
    const auto uidx = reader.field_index(universe.field_name);
    if (!uidx.has_value()) {
      return atx::core::Err(atx::core::ErrorCode::NotFound,
                            "attach_segment_panel: unknown universe field '" +
                                universe.field_name + "'");
    }
    uni = detail::universe_from_field(reader, uidx.value(), d0, dates);
  } else {
    uni = detail::universe_from_present(reader, d0, dates);
  }

  ATX_TRY(Panel panel, Panel::create_borrowed(dates, static_cast<atx::usize>(n), std::move(names),
                                              std::move(columns), std::move(uni)));
  return atx::core::Ok(MappedPanel{std::move(reader), std::move(panel)});
}

} // namespace atx::engine::alpha
```

> **Pre-check before relying on this:** confirm `ATX_TRY` is available transitively (it comes via `atx/core/error.hpp`; the existing `segment_reader.cpp` uses it). Confirm `atx::u8` is the `UniverseKind` underlying type spelled in `atx/core/types.hpp`. If `ATX_TRY` is not visible, add `#include "atx/core/macro.hpp"`.

- [ ] **Step 4: Run to verify it passes**

Run: `cmake --build build --target atx-engine-tests`
Then: `build/bin/atx-engine-tests.exe --gtest_filter=SegmentPanel_*`
Expected: all 5 `SegmentPanel_*` tests PASS.

- [ ] **Step 5: Format + commit**

```
clang-format -i atx-engine/include/atx/engine/alpha/segment_panel.hpp atx-engine/tests/segment_panel_test.cpp
git add atx-engine/include/atx/engine/alpha/segment_panel.hpp atx-engine/tests/segment_panel_test.cpp
git commit -m "feat(tsdb-v2-5): attach_segment_panel bridge + MappedPanel"
```

---

# Phase P4 — Correctness capstone + throughput

### Task 6: Golden differential — `VM(shm) == VM(owned) == Oracle`

**Files:**
- Modify: `atx-engine/tests/segment_panel_test.cpp` (append)

This proves the whole bridge: the VM run over the zero-copy shm Panel produces the exact `SignalSet` of the VM (and the reference oracle) run over an owned Panel built from the same numbers.

- [ ] **Step 1: Write the test**

Add these includes to the top of `segment_panel_test.cpp` (with the existing includes):
```cpp
#include <cmath>     // std::isnan
#include <cstdint>
#include <string_view>

#include "atx/engine/alpha/oracle.hpp"   // evaluate_reference
#include "atx/engine/alpha/panel.hpp"    // Panel::create
#include "atx/engine/alpha/parser.hpp"   // parse_expr
#include "atx/engine/alpha/registry.hpp" // Library
#include "atx/engine/alpha/typecheck.hpp"// analyze
#include "atx/engine/alpha/vm.hpp"       // Engine, compile
```

Append to `segment_panel_test.cpp`:

```cpp
namespace {

using atx::engine::alpha::analyze;
using atx::engine::alpha::compile;
using atx::engine::alpha::Engine;
using atx::engine::alpha::evaluate_reference;
using atx::engine::alpha::Library;
using atx::engine::alpha::Panel;
using atx::engine::alpha::parse_expr;
using atx::engine::alpha::Program;
using atx::engine::alpha::SignalSet;

[[nodiscard]] const Library &shared_lib() {
  static const Library lib;
  return lib;
}

[[nodiscard]] bool same_cell(atx::f64 a, atx::f64 b) noexcept {
  return (std::isnan(a) && std::isnan(b)) || a == b;
}

[[nodiscard]] Program compile_ok(std::string_view src) {
  auto ast = parse_expr(src, shared_lib());
  EXPECT_TRUE(ast.has_value()) << (ast ? "" : ast.error().message());
  auto ana = analyze(ast.value());
  EXPECT_TRUE(ana.has_value()) << (ana ? "" : ana.error().message());
  auto prog = compile(ast.value(), ana.value());
  EXPECT_TRUE(prog.has_value()) << (prog ? "" : prog.error().message());
  return prog.value_or(Program{});
}

void expect_signalsets_equal(const SignalSet &a, const SignalSet &b) {
  ASSERT_EQ(a.alphas.size(), b.alphas.size());
  ASSERT_EQ(a.dates, b.dates);
  ASSERT_EQ(a.instruments, b.instruments);
  for (atx::usize k = 0; k < a.alphas.size(); ++k) {
    ASSERT_EQ(a.alphas[k].values.size(), b.alphas[k].values.size());
    for (atx::usize i = 0; i < a.alphas[k].values.size(); ++i) {
      EXPECT_TRUE(same_cell(a.alphas[k].values[i], b.alphas[k].values[i]))
          << "alpha " << k << " cell " << i;
    }
  }
}

// Build a dense OHLCV segment AND the equivalent owned Panel from identical
// numbers. Field order matches make_panel convention so field_id resolves the
// same in both. close=date*10+inst+1, others derived; all cells present.
struct PairedData {
  std::string path;
  std::vector<std::vector<atx::f64>> cols; // owned-Panel columns (close,open,high,low,volume)
  atx::usize dates;
  atx::usize instruments;
};

[[nodiscard]] PairedData make_paired(const std::string &name, atx::usize dates,
                                     atx::usize instruments) {
  const std::vector<std::string> fnames{"close", "open", "high", "low", "volume"};
  std::vector<std::string> syms(instruments);
  for (atx::usize i = 0; i < instruments; ++i) {
    syms[i] = "S" + std::to_string(i);
  }
  std::vector<atx::i64> axis(dates);
  for (atx::usize d = 0; d < dates; ++d) {
    axis[d] = static_cast<atx::i64>((d + 1) * 100);
  }
  atx::tsdb::SegmentBuilder b(fnames, syms, axis);

  std::vector<std::vector<atx::f64>> cols(5, std::vector<atx::f64>(dates * instruments));
  for (atx::usize d = 0; d < dates; ++d) {
    for (atx::usize i = 0; i < instruments; ++i) {
      const auto idx = d * instruments + i;
      const atx::f64 close = static_cast<atx::f64>(d * 10 + i + 1);
      const atx::f64 open = close - 0.5;
      const atx::f64 high = close + 1.0;
      const atx::f64 low = close - 1.0;
      const atx::f64 vol = 1000.0 + static_cast<atx::f64>(i);
      cols[0][idx] = close;
      cols[1][idx] = open;
      cols[2][idx] = high;
      cols[3][idx] = low;
      cols[4][idx] = vol;
      b.set(0, d, static_cast<atx::u32>(i), close);
      b.set(1, d, static_cast<atx::u32>(i), open);
      b.set(2, d, static_cast<atx::u32>(i), high);
      b.set(3, d, static_cast<atx::u32>(i), low);
      b.set(4, d, static_cast<atx::u32>(i), vol);
    }
  }
  const std::string path = temp_path(name);
  EXPECT_TRUE(b.write(path, 0).has_value());
  return PairedData{path, std::move(cols), dates, instruments};
}

} // namespace

TEST(SegmentPanel_Golden_VmShmEqualsVmOwnedEqualsOracle, Differential) {
  const PairedData pd = make_paired("golden", /*dates=*/8, /*instruments=*/5);

  // Owned Panel from the same numbers (universe = all-present).
  auto owned = Panel::create(pd.dates, pd.instruments, {"close", "open", "high", "low", "volume"},
                             pd.cols, {});
  ASSERT_TRUE(owned.has_value()) << (owned ? "" : owned.error().message());

  // Borrowed Panel straight off the segment.
  auto mp = attach_segment_panel(pd.path);
  ASSERT_TRUE(mp.has_value()) << (mp ? "" : mp.error().to_string());

  const std::vector<std::string_view> exprs{"close - open", "ts_mean(close, 3)",
                                            "rank(close)", "(high - low) / close"};
  for (const std::string_view src : exprs) {
    const Program prog = compile_ok(src);

    Engine eng_owned{owned.value()};
    auto so = eng_owned.evaluate(prog);
    ASSERT_TRUE(so.has_value()) << src << " owned: " << (so ? "" : so.error().message());

    Engine eng_shm{mp->panel()};
    auto ss = eng_shm.evaluate(prog);
    ASSERT_TRUE(ss.has_value()) << src << " shm: " << (ss ? "" : ss.error().message());

    auto ref = evaluate_reference(prog, mp->panel());
    ASSERT_TRUE(ref.has_value()) << src << " oracle: " << (ref ? "" : ref.error().message());

    expect_signalsets_equal(ss.value(), so.value()); // shm == owned
    expect_signalsets_equal(ss.value(), ref.value()); // shm == oracle
  }
  std::remove(pd.path.c_str());
}
```

> **Pre-check:** confirm the four expressions parse against the current registry — `ts_mean`, `rank` are in the operator catalogue (registry.hpp). If any name differs (e.g. `ts_mean` vs `ts_avg`), substitute a name the registry accepts; the differential holds for any compilable expression. Keep at least one cross-sectional (`rank`) and one time-series (`ts_*`) op to exercise both kernel families.

- [ ] **Step 2: Run to verify it fails, then passes**

First run (before any fix): `cmake --build build --target atx-engine-tests` — if an expression name is wrong it fails at `compile_ok`; fix per the pre-check.
Then: `build/bin/atx-engine-tests.exe --gtest_filter=SegmentPanel_Golden_*`
Expected: PASS — shm == owned == oracle for every expression.

- [ ] **Step 3: Format + commit**

```
clang-format -i atx-engine/tests/segment_panel_test.cpp
git add atx-engine/tests/segment_panel_test.cpp
git commit -m "test(tsdb-v2-6): golden VM(shm)==VM(owned)==oracle over segment panel"
```

---

### Task 7: `panel_read_bench` — throughput numbers

**Files:**
- Create: `atx-engine/bench/panel_read_bench.cpp`

Quantifies the win: zero-copy attach+evaluate vs the memcpy-into-owned-Panel path. Uses Google Benchmark (the bench target already links `benchmark::benchmark` and globs `*_bench.cpp`).

- [ ] **Step 1: Create the benchmark**

```cpp
// atx::engine::alpha — panel read throughput: zero-copy shm Panel vs an owned
// Panel built by copying the same field blocks out of the mapping. Reports
// items/s over cells so the ratio is the copy cost the zero-copy path avoids.

#include <string>
#include <vector>

#include <benchmark/benchmark.h>

#include "atx/core/types.hpp"

#include "atx/engine/alpha/panel.hpp"
#include "atx/engine/alpha/segment_panel.hpp"

#include "atx/tsdb/builder.hpp"
#include "atx/tsdb/segment_reader.hpp"

namespace {

// Build a dense F=5 OHLCV segment of `dates` x `instruments` once, return path.
std::string build_seg(atx::usize dates, atx::usize instruments) {
  const std::vector<std::string> fnames{"close", "open", "high", "low", "volume"};
  std::vector<std::string> syms(instruments);
  for (atx::usize i = 0; i < instruments; ++i) {
    syms[i] = "S" + std::to_string(i);
  }
  std::vector<atx::i64> axis(dates);
  for (atx::usize d = 0; d < dates; ++d) {
    axis[d] = static_cast<atx::i64>((d + 1) * 100);
  }
  atx::tsdb::SegmentBuilder b(fnames, syms, axis);
  for (atx::usize d = 0; d < dates; ++d) {
    for (atx::usize i = 0; i < instruments; ++i) {
      for (atx::u32 f = 0; f < 5; ++f) {
        b.set(f, d, static_cast<atx::u32>(i), static_cast<atx::f64>(d + i + f + 1));
      }
    }
  }
  // A fixed path in the temp dir (bench is single-process; overwrite each run).
  const std::string path = std::string("atx_panel_bench.atxseg");
  b.write(path, 0);
  return path;
}

void BM_AttachZeroCopy(benchmark::State &state) {
  const auto dates = static_cast<atx::usize>(state.range(0));
  const auto inst = static_cast<atx::usize>(state.range(1));
  const std::string path = build_seg(dates, inst);
  for (auto _ : state) {
    auto mp = atx::engine::alpha::attach_segment_panel(path);
    benchmark::DoNotOptimize(mp->panel().field_all(0).data());
  }
  state.SetItemsProcessed(static_cast<atx::i64>(state.iterations() * dates * inst * 5));
}

void BM_MemcpyOwned(benchmark::State &state) {
  const auto dates = static_cast<atx::usize>(state.range(0));
  const auto inst = static_cast<atx::usize>(state.range(1));
  const std::string path = build_seg(dates, inst);
  auto reader = atx::tsdb::SegmentReader::attach(path);
  for (auto _ : state) {
    std::vector<std::vector<atx::f64>> cols(reader->field_count());
    for (atx::u32 f = 0; f < reader->field_count(); ++f) {
      const auto block = reader->field_block_view(f);
      cols[f].assign(block.begin(), block.end()); // the copy the zero-copy path avoids
    }
    auto p = atx::engine::alpha::Panel::create(dates, inst,
                                              {"close", "open", "high", "low", "volume"},
                                              std::move(cols), {});
    benchmark::DoNotOptimize(p->field_all(0).data());
  }
  state.SetItemsProcessed(static_cast<atx::i64>(state.iterations() * dates * inst * 5));
}

} // namespace

BENCHMARK(BM_AttachZeroCopy)->Args({2520, 500})->Args({5040, 3000});
BENCHMARK(BM_MemcpyOwned)->Args({2520, 500})->Args({5040, 3000});
```

> **Pre-check:** confirm `bench_main.cpp` provides `BENCHMARK_MAIN()` (it does — the bench target lists it). If the bench main is custom, follow the existing `*_bench.cpp` registration style instead of relying on `BENCHMARK_MAIN`.

- [ ] **Step 2: Build the bench**

Run: `cmake --preset ninja -DATX_BUILD_TESTS=ON -DATX_BUILD_BENCH=ON` then `cmake --build build --target atx-engine-bench`
Expected: links cleanly (new file auto-globbed).

- [ ] **Step 3: Run it (informational, not a pass/fail gate)**

Run: `build/bin/atx-engine-bench.exe --benchmark_filter="BM_AttachZeroCopy|BM_MemcpyOwned"`
Expected: `BM_AttachZeroCopy` items/s ≫ `BM_MemcpyOwned` (zero-copy avoids the per-run copy of `dates*inst*5*8` bytes).

- [ ] **Step 4: Commit**

```
clang-format -i atx-engine/bench/panel_read_bench.cpp
git add atx-engine/bench/panel_read_bench.cpp
git commit -m "bench(tsdb-v2-7): zero-copy attach vs memcpy-owned panel read throughput"
```

---

## Final regression gate (after Task 7)

Run the full suites once more to confirm nothing regressed across tsdb + engine:

```
cmake --build build --target atx-tsdb-tests atx-engine-tests
build/bin/atx-tsdb-tests.exe
build/bin/atx-engine-tests.exe
```
Expected: all green (tsdb suite + engine suite, including the existing alpha VM/oracle/panel tests and the new SegmentPanel + golden tests).

---

## Deferred (NOT in this plan)

- **Window-ranged / per-field prefetch** — `MappedPanel::prefetch()` currently warms the whole mapping; a `Mapping::prefetch_range(off,len)` + per-projected-field warm is a follow-up.
- **Streaming `ShmPanelSource`** — `PanelView` ring→linear generalization for the live event loop (the original v1 §6.3 item); the loop already works via `ShmBarFeed`.
- **Multi-segment virtual concatenation** — one backtest spanning many segment files.
- **Explicit universe as a dedicated bitmap section** — this plan's override path reads a named f64 field; a packed universe bitmap section + format flag is a future option.
- **Per-row SIMD padding** — only block starts are 64B-aligned; padding every date-row is out of scope (would break dense `field_all`).

---

## Self-Review

**1. Spec coverage**

| Spec unit | Task(s) |
|---|---|
| U1 segment format v2 (64B blocks, v1+v2 accept) | Task 1 (segment.hpp), Task 2 (reader version policy), Task 3 (builder alignment) |
| U2 borrowing Panel backend | Task 4 |
| U3 universe (present-bitmap default + field override) | Task 5 (`detail::universe_from_present` / `universe_from_field`) |
| U4 `attach_segment_panel` + `MappedPanel` (window, projection, lifetime) | Task 5 |
| U5 throughput + golden equality | Task 6 (golden), Task 7 (bench) |
| Invariants: determinism | Task 6 (shm==owned==oracle) |
| Invariants: no-look-ahead | Task 5 (`[t0,t1)` window; ts-ops backward-only is the VM's, unchanged) |
| Invariants: survivorship | Task 5 (`SegmentPanel_FullWindow` absent-cell ⇒ out-of-universe) |
| Invariants: read safety | Task 2 (`is_supported_version`; existing crc/magic validation unchanged) |

No gaps.

**2. Placeholder scan:** No "TBD"/"add error handling"/"similar to". Every code step shows complete code. Two `> Pre-check` callouts (segment_panel `ATX_TRY` visibility; golden test expression names against the live registry) are bounded verifications against existing headers, not placeholders.

**3. Type consistency:** `field_block_stride_bytes`, `field_block_view`, `is_supported_version`, `kBlockAlign` are spelled identically at definition (Task 1) and use (Tasks 2/3/5). `SegmentReader::field_name` / `field_block_view` match between header (Task 2) and bridge call sites (Task 5). `Panel::create_borrowed(dates, instruments, field_names, columns, universe)` matches between definition (Task 4) and the single call site (Task 5). `MappedPanel{reader, panel}` ctor and `.panel()`/`.reader()`/`.prefetch()` accessors match between definition (Task 5) and tests (Tasks 5/6/7). `TimeWindow{start_nanos,end_nanos}`, `UniverseKind::{PresentBitmap,Field}`, `UniversePolicy{kind,field_name}` consistent across definition and tests. Engine usage (`Engine{panel}; evaluate(prog)`) and `evaluate_reference(prog, panel)` match the patterns in the existing `alpha_vm_test.cpp`.

**Fix applied during review:** `make_paired` uses a single `SegmentBuilder b` (distinct `S{i}` symbols + ascending axis) that writes the segment AND mirrors the same numbers into `cols` for the owned Panel — so the owned and borrowed panels carry byte-identical field data and the differential is meaningful. No unused variables (clean under `/WX`).
