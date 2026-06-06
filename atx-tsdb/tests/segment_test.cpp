#include <gtest/gtest.h>

#include <array>
#include <span>

#include "atx/core/types.hpp"
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
  constexpr atx::u32 F = 2;
  constexpr atx::u32 N = 2;
  constexpr atx::u64 T = 3;
  std::array<atx::f64, F * T * N> blocks{};
  // field 1 (second block), t=2, inst=1  ==>  index = 1*(T*N) + (2*N + 1) = 6+5 = 11
  blocks[1 * (T * N) + (2 * N + 1)] = 42.0;

  SegmentHeader h{};
  h.field_count = F;
  h.instrument_count = N;
  h.time_count = T;
  h.off_field_blocks = 0;

  // SAFETY: reinterpret_cast from f64* to u8* to simulate a file mapping;
  // blocks is trivially copyable and f64-aligned, read-only in accessors.
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
  const auto *base = reinterpret_cast<const atx::u8 *>(blocks.data());
  EXPECT_DOUBLE_EQ(atx::tsdb::cell_value(base, h, /*field=*/1, /*t=*/2, /*inst=*/1), 42.0);
  EXPECT_DOUBLE_EQ(atx::tsdb::cell_value(base, h, /*field=*/0, /*t=*/0, /*inst=*/0), 0.0);
}

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
  constexpr atx::u64 T = 3;              // packed = 48B -> stride 64B = 8 f64
  constexpr atx::usize kStrideElems = 8; // 64 / sizeof(f64)
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
