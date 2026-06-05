#include <gtest/gtest.h>

#include <array>

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
