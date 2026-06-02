#include "atx/core/hash.hpp"

#include <cstddef>
#include <string_view>

#include <gtest/gtest.h>

using namespace atx;       // NOLINT(google-build-using-namespace)
using namespace atx::core; // NOLINT(google-build-using-namespace)

// ---- hash_bytes -------------------------------------------------------------

TEST(Hash, BytesDeterministic) {
    // Same input must always produce the same digest.
    std::string_view s = "AAPL";
    EXPECT_EQ(hash_bytes(s.data(), s.size()), hash_bytes(s.data(), s.size()));
}

TEST(Hash, BytesEmptyIsStable) {
    // Empty span must not crash and must be deterministic.
    EXPECT_EQ(hash_bytes(nullptr, 0U), hash_bytes(nullptr, 0U));
}

TEST(Hash, BytesDifferentInputsDiffer) {
    // Different byte strings should produce different hashes (high probability).
    std::string_view a = "AAPL";
    std::string_view b = "GOOG";
    EXPECT_NE(hash_bytes(a.data(), a.size()), hash_bytes(b.data(), b.size()));
}

TEST(Hash, BytesLengthSensitive) {
    // "AA" and "AAP" must differ.
    std::string_view full = "AAPL";
    EXPECT_NE(hash_bytes(full.data(), 2U), hash_bytes(full.data(), 3U));
}

// ---- hash_combine -----------------------------------------------------------

TEST(Hash, CombineDiffers) {
    // Seed case from spec: order-sensitive.
    std::size_t a = hash_combine(0U, 1, 2, 3);
    std::size_t b = hash_combine(0U, 3, 2, 1);
    EXPECT_NE(a, b);
}

TEST(Hash, CombineDeterministic) {
    // Same inputs, same seed → same output across two calls.
    EXPECT_EQ(hash_combine(42U, 1, 2, 3), hash_combine(42U, 1, 2, 3));
}

TEST(Hash, CombineSeedMatters) {
    // Different seeds with identical values must (almost always) produce
    // different results.
    EXPECT_NE(hash_combine(0U, 99), hash_combine(1U, 99));
}

TEST(Hash, CombineSingleValue) {
    // Single argument: must return a value that differs from the plain seed.
    std::size_t seed = 0U;
    std::size_t combined = hash_combine(seed, 42);
    // The mixer is non-trivial so the output differs from the seed.
    EXPECT_NE(combined, seed);
}

TEST(Hash, CombineVariadicFolds) {
    // hash_combine(seed, a, b, c) == hash_combine(hash_combine(hash_combine(seed, a), b), c)
    // i.e. folding left must be equivalent.
    int a = 10;
    int b = 20;
    int c = 30;
    std::size_t step1 = hash_combine(0U, a);
    std::size_t step2 = hash_combine(step1, b);
    std::size_t step3 = hash_combine(step2, c);
    EXPECT_EQ(hash_combine(0U, a, b, c), step3);
}

TEST(Hash, CombineZeroSeedZeroValue) {
    // Boundary: seed=0, value=0. Must not crash; result is deterministic.
    std::size_t r1 = hash_combine(0U, 0);
    std::size_t r2 = hash_combine(0U, 0);
    EXPECT_EQ(r1, r2);
}
