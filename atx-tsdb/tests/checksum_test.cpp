#include <gtest/gtest.h>

#include <array>
#include <string_view>

#include "atx/core/types.hpp"
#include "atx/tsdb/checksum.hpp"

// Known CRC-32 (zlib) of ASCII "123456789" is 0xCBF43926 (industry check value).
TEST(Crc32_CheckString_MatchesKnownVector, Check) {
  constexpr std::string_view s = "123456789";
  EXPECT_EQ(atx::tsdb::crc32(s.data(), s.size()), 0xCBF43926U);
}

TEST(Crc32_EmptyInput_IsZero, Empty) { EXPECT_EQ(atx::tsdb::crc32(nullptr, 0), 0U); }

TEST(Crc32_DifferentBytes_DifferentDigest, Sensitivity) {
  const std::array<atx::u8, 3> a{1, 2, 3};
  const std::array<atx::u8, 3> b{1, 2, 4};
  EXPECT_NE(atx::tsdb::crc32(a.data(), a.size()), atx::tsdb::crc32(b.data(), b.size()));
}
