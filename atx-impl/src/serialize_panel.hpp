#pragma once

// atx::impl — dependency-free alpha::Panel binary serializer.
//
// Binary layout (see serialize_panel.cpp for detail):
//   payload = header(magic+version+shape) + field-names + field-columns + universe-mask
//   file    = payload + u64 trailer (fnv1a64 of payload, little-endian)
//
// Guarantees:
//   * NaN-transparent: f64 bit patterns are round-tripped via memcpy (no
//     interpretation — NaN cells survive identically).
//   * Little-endian on-disk (portable across LE platforms; not portable to BE).
//   * The fnv1a64 trailer allows fast integrity checking without a separate hash file.

#include <string>

#include "atx/core/error.hpp"
#include "atx/core/types.hpp"

#include "atx/engine/alpha/panel.hpp"

namespace atx::impl {

// Serialize an owned alpha::Panel to `path`. Returns the fnv1a64 digest of the
// serialized payload (header+names+columns+mask), which is also written as an
// 8-byte little-endian trailer for integrity.
// Err(InvalidArgument) if the file cannot be opened for writing.
[[nodiscard]] atx::core::Result<atx::u64>
write_panel(const atx::engine::alpha::Panel& panel, const std::string& path);

// Read back a Panel written by write_panel. Recomputes the payload digest and
// verifies it against the trailer (Err(ParseError) on mismatch / bad magic /
// truncated file). Returns an OWNED Panel (via Panel::create).
[[nodiscard]] atx::core::Result<atx::engine::alpha::Panel>
read_panel(const std::string& path);

} // namespace atx::impl
