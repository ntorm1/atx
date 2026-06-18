#pragma once

#include <string>

#include "atx/core/types.hpp"

namespace atx::impl {

// Returns the 16-character lowercase hex representation of a u64,
// zero-padded to exactly 16 chars. Used by emit_digest_line.
[[nodiscard]] std::string to_hex16(atx::u64 value);

} // namespace atx::impl
