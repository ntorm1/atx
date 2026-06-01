#include "atx/core/types.hpp"

#include <cstddef>
#include <type_traits>

#include <gtest/gtest.h>

static_assert(sizeof(atx::i32) == 4, "i32 width");
static_assert(sizeof(atx::u64) == 8, "u64 width");
static_assert(sizeof(atx::f32) == 4, "f32 width");
static_assert(sizeof(atx::f64) == 8, "f64 width");
static_assert(std::is_same_v<atx::usize, std::size_t>, "usize == size_t");
static_assert(std::is_signed_v<atx::i32>, "i32 signed");
static_assert(std::is_unsigned_v<atx::u32>, "u32 unsigned");

// Width/signedness are compile-time contracts; this body only anchors the TU.
TEST(TypesTest, CompileTimeContractsHold) { SUCCEED(); }
