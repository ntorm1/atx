#pragma once

#include <string>

#include "atx/core/types.hpp"

namespace atx::impl {

// Returns the 16-character lowercase hex representation of a u64,
// zero-padded to exactly 16 chars. Used by emit_digest_line.
[[nodiscard]] std::string to_hex16(atx::u64 value);

// FNV-1a 64-bit over a raw byte buffer. Fixed constants => identical output for
// the same bytes across processes and runs on this platform.
// data may be null iff len==0.
[[nodiscard]] atx::u64 fnv1a64(const void* data, atx::usize len) noexcept;

} // namespace atx::impl
