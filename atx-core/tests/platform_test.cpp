#include "atx/core/platform.hpp"

#include <cstddef>
#include <cstdint>

#include <gtest/gtest.h>

// ---- compile-time contracts ------------------------------------------------

static_assert(atx::core::kCacheLineSize == 64u,
              "x86-64 cache line must be 64 bytes");

// ---- CacheLineSize ---------------------------------------------------------

TEST(Platform, CacheLineSizeIs64) {
    static_assert(atx::core::kCacheLineSize == 64u);
    EXPECT_EQ(atx::core::kCacheLineSize, 64u);
}

TEST(Platform, CacheAlignedTypeIsAligned) {
    struct alignas(atx::core::kCacheLineSize) Padded {
        int x;
    };
    EXPECT_EQ(alignof(Padded), atx::core::kCacheLineSize);
}

// ---- ATX_CACHE_ALIGNED macro -----------------------------------------------

TEST(Platform, CacheAlignedMacroProducesCorrectAlignment) {
    // Verify the macro expands to an alignas that the compiler enforces.
    struct ATX_CACHE_ALIGNED MacroPadded {
        int x;
    };
    EXPECT_EQ(alignof(MacroPadded), atx::core::kCacheLineSize);
}

// ---- prefetch smoke test ---------------------------------------------------

TEST(Platform, PrefetchOnStackIntDoesNotCrash) {
    // prefetch() is a hint — no observable side-effect, but it must compile
    // and not crash when called on a valid address.
    const int value{42};
    atx::core::prefetch(&value);
    EXPECT_EQ(value, 42); // unchanged; just proves we return safely
}

TEST(Platform, PrefetchNullptrIsDefinedByFallback) {
    // On platforms where prefetch is a no-op (void)p fallback, nullptr must
    // also be safe (no dereference occurs; it is merely passed as a hint).
    // SAFETY: The intrinsic/builtin treats the address as a hint only;
    //         no load is performed, so nullptr is not UB here.
    atx::core::prefetch(nullptr);
    SUCCEED();
}
