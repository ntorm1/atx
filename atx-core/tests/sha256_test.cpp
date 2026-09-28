#include <algorithm>
#include <array>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <random>
#include <span>
#include <string>
#include <string_view>
#include <vector>

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

// ---- Accelerated backends (platform v7 L1): digests are backend-independent ----
namespace {
using atx::core::Sha256;
using atx::core::Sha256Backend;

std::string hex_digest(const std::array<std::byte, 32> &digest) {
  constexpr char hex[] = "0123456789abcdef";
  std::string out;
  for (const auto byte : digest) {
    const auto value = std::to_integer<unsigned>(byte);
    out.push_back(hex[value >> 4U]);
    out.push_back(hex[value & 0x0fU]);
  }
  return out;
}

// Scalar always; the SHA-NI kernel too when this host can run it.
std::vector<Sha256Backend> runnable_backends() {
  std::vector<Sha256Backend> out{Sha256Backend::Scalar};
  if (atx::core::sha256_backend_available(Sha256Backend::X86ShaNi)) {
    out.push_back(Sha256Backend::X86ShaNi);
  }
  return out;
}

// Digest of `bytes` fed in `chunk`-sized updates through `backend`.
std::string digest_with(Sha256Backend backend, std::span<const std::byte> bytes, std::size_t chunk) {
  Sha256 digest(backend);
  EXPECT_EQ(digest.backend(), backend);
  for (std::size_t offset = 0; offset < bytes.size(); offset += chunk) {
    EXPECT_TRUE(digest.update(bytes.subspan(offset, std::min(chunk, bytes.size() - offset))));
  }
  auto finalized = digest.finalize();
  EXPECT_TRUE(finalized);
  return finalized ? hex_digest(*finalized) : std::string{};
}

std::vector<std::byte> random_bytes(std::size_t size, std::uint64_t seed) {
  std::mt19937_64 engine{seed};
  std::vector<std::byte> out(size);
  for (auto &byte : out) {
    byte = std::byte{static_cast<unsigned char>(engine() >> 56U)};
  }
  return out;
}
} // namespace

TEST(Sha256, ScalarIsAlwaysAvailableAndAnUnavailableBackendFallsBackToIt) {
  EXPECT_TRUE(atx::core::sha256_backend_available(Sha256Backend::Scalar));
  const auto chosen = atx::core::sha256_default_backend();
  EXPECT_TRUE(atx::core::sha256_backend_available(chosen));
  EXPECT_EQ(Sha256{}.backend(), chosen);
  EXPECT_EQ(atx::core::sha256_backend_name(Sha256Backend::Scalar), "scalar");
  EXPECT_EQ(atx::core::sha256_backend_name(Sha256Backend::X86ShaNi), "x86-sha-ni");
  const bool shani = atx::core::sha256_backend_available(Sha256Backend::X86ShaNi);
  // The fastest runnable backend is the default; a pinned one never runs unsupported code.
  EXPECT_EQ(chosen, shani ? Sha256Backend::X86ShaNi : Sha256Backend::Scalar);
  EXPECT_EQ(Sha256{Sha256Backend::X86ShaNi}.backend(),
            shani ? Sha256Backend::X86ShaNi : Sha256Backend::Scalar);
  RecordProperty("sha256_default_backend", std::string(atx::core::sha256_backend_name(chosen)));
}

TEST(Sha256, EveryBackendMatchesTheNistVectorsForEveryUpdateSplit) {
  struct Vector {
    std::string message;
    std::string_view digest;
  };
  const std::array<Vector, 5> vectors{{
      {"", "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"},
      {"abc", "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"},
      {"abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq",
       "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"},
      {"abcdefghbcdefghicdefghijdefghijkefghijklfghijklmghijklmnhijklmnoijklmnopjklmnopqklmnopqrlmnopqrs"
       "mnopqrstnopqrstu",
       "cf5b16a778af8380036ce59e7b0492370b249b11e8f07a51afac45037afee9d1"},
      {std::string(1'000'000, 'a'), "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0"},
  }};
  for (const auto backend : runnable_backends()) {
    SCOPED_TRACE(std::string(atx::core::sha256_backend_name(backend)));
    for (const auto &vector : vectors) {
      const auto bytes = std::as_bytes(std::span{vector.message.data(), vector.message.size()});
      // Byte-at-a-time only for the short vectors (a million single-byte updates is slow in Debug).
      const std::size_t smallest = bytes.size() > 4096 ? 64 : 1;
      for (const std::size_t chunk : {smallest, std::size_t{63}, std::size_t{64}, std::size_t{65},
                                      std::size_t{1000}, std::max<std::size_t>(bytes.size(), 1)}) {
        EXPECT_EQ(digest_with(backend, bytes, chunk), vector.digest)
            << bytes.size() << " bytes in chunks of " << chunk;
      }
    }
  }
}

TEST(Sha256, AcceleratedDigestsEqualScalarOnMultiMebibyteRandomBuffersAndEveryTailLength) {
  // 5 MiB + 37 B: many whole-block runs through the kernel plus an odd tail.
  const auto data = random_bytes((std::size_t{5} << 20U) + 37U, 0x6a09e667f3bcc908ULL);
  const auto expected = digest_with(Sha256Backend::Scalar, data, data.size());
  ASSERT_EQ(expected.size(), 64U);
  for (const auto backend : runnable_backends()) {
    SCOPED_TRACE(std::string(atx::core::sha256_backend_name(backend)));
    for (const std::size_t chunk : {data.size(), std::size_t{1} << 20U, std::size_t{4099},
                                    std::size_t{65535}}) {
      EXPECT_EQ(digest_with(backend, data, chunk), expected) << "chunk " << chunk;
    }
  }
  auto one_shot = atx::core::sha256_hex(std::span<const std::byte>{data});
  ASSERT_TRUE(one_shot);
  EXPECT_EQ(*one_shot, expected);
  // Every padding case: lengths 0..300 cross the 55/56/63/64-byte finalization edges.
  const auto small = random_bytes(300, 0xbb67ae8584caa73bULL);
  for (std::size_t length = 0; length <= small.size(); ++length) {
    const auto prefix = std::span<const std::byte>{small}.first(length);
    const auto scalar = digest_with(Sha256Backend::Scalar, prefix, std::max<std::size_t>(length, 1));
    for (const auto backend : runnable_backends()) {
      EXPECT_EQ(digest_with(backend, prefix, 7), scalar) << "length " << length;
    }
  }
}

TEST(Sha256, FileDigestOverSeveralReadChunksEqualsTheInMemoryScalarDigest) {
  const auto data = random_bytes((std::size_t{3} << 20U) + 5U, 0x3c6ef372fe94f82bULL);
  const auto path = std::filesystem::temp_directory_path() / "atx_sha256_multichunk_test.bin";
  {
    std::ofstream output{path, std::ios::binary | std::ios::trunc};
    ASSERT_TRUE(output);
    output.write(reinterpret_cast<const char *>(data.data()),
                 static_cast<std::streamsize>(data.size()));
    ASSERT_TRUE(output);
  }
  auto digest = atx::core::sha256_file(path.string());
  ASSERT_TRUE(digest) << digest.error().to_string();
  EXPECT_EQ(*digest, digest_with(Sha256Backend::Scalar, data, data.size()));
  std::error_code ignored;
  std::filesystem::remove(path, ignored);
}
