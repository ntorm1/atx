#pragma once

// Project vocabulary types. Fixed-width, explicit, no ambiguity about width or
// signedness at any boundary. Prefer these over bare `int`/`unsigned`/`long`.
//
// Rationale (agent profile §2): never assume `int` width; fixed-width integers
// at boundaries/ABI; signed/unsigned mixing is a CERT finding.

#include <cstddef>
#include <cstdint>

namespace atx {

// ---- Signed integers ----
using i8 = std::int8_t;
using i16 = std::int16_t;
using i32 = std::int32_t;
using i64 = std::int64_t;

// ---- Unsigned integers ----
using u8 = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;

// ---- Sizes & pointers ----
using usize = std::size_t;    // index/length; matches container ::size_type
using isize = std::ptrdiff_t; // signed distance between pointers
using uptr = std::uintptr_t;  // integer wide enough to hold a pointer

// ---- Floating point ----
using f32 = float;
using f64 = double;

// ---- Bytes & C strings ----
using byte = std::byte;    // raw memory; not an arithmetic type
using cstr = const char *; // non-owning, NUL-terminated C string

// ---- Width guarantees (compile-time contract) ----
static_assert(sizeof(i8) == 1 && sizeof(u8) == 1, "8-bit width");
static_assert(sizeof(i16) == 2 && sizeof(u16) == 2, "16-bit width");
static_assert(sizeof(i32) == 4 && sizeof(u32) == 4, "32-bit width");
static_assert(sizeof(i64) == 8 && sizeof(u64) == 8, "64-bit width");
static_assert(sizeof(f32) == 4, "f32 is 32-bit");
static_assert(sizeof(f64) == 8, "f64 is 64-bit");

} // namespace atx
