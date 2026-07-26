#include <array>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <span>
#include <string_view>

#include <gtest/gtest.h>

#include "atx/core/error.hpp"
#include "atx/core/sha256.hpp"

TEST(Sha256, MatchesPublishedEmptyAndAbcVectors) {
  auto empty = atx::core::sha256_hex(std::string_view{});
  auto abc = atx::core::sha256_hex("abc");
  ASSERT_TRUE(empty);
  ASSERT_TRUE(abc);
  EXPECT_EQ(*empty, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
  EXPECT_EQ(*abc, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
}

TEST(Sha256, IncrementalUpdatesMatchOneShotDigestAndFenceFinalization) {
  constexpr std::string_view message = "the quick brown fox jumps over the lazy dog";
  auto expected = atx::core::sha256_hex(message);
  ASSERT_TRUE(expected);
  atx::core::Sha256 digest;
  ASSERT_TRUE(digest.update(std::as_bytes(std::span{message.data(), 7U})));
  ASSERT_TRUE(digest.update(std::as_bytes(std::span{message.data() + 7, message.size() - 7U})));
  auto finalized = digest.finalize();
  ASSERT_TRUE(finalized);
  constexpr char hex[] = "0123456789abcdef";
  std::string actual;
  for (const auto byte : *finalized) {
    const auto value = std::to_integer<unsigned>(byte);
    actual.push_back(hex[value >> 4U]);
    actual.push_back(hex[value & 0x0fU]);
  }
  EXPECT_EQ(actual, *expected);
  auto repeated = digest.finalize();
  ASSERT_FALSE(repeated);
  EXPECT_EQ(repeated.error().code(), atx::core::ErrorCode::InvalidArgument);
}

TEST(Sha256, FileDigestStreamsTheExactBytes) {
  const auto path = std::filesystem::temp_directory_path() / "atx_sha256_file_test.bin";
  {
    std::ofstream output{path, std::ios::binary | std::ios::trunc};
    ASSERT_TRUE(output);
    output << "abc";
  }
  auto digest = atx::core::sha256_file(path.string());
  ASSERT_TRUE(digest) << digest.error().to_string();
  EXPECT_EQ(*digest, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
  std::error_code ignored;
  std::filesystem::remove(path, ignored);
}
