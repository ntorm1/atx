#pragma once

#include <initializer_list>
#include <ostream>
#include <string>
#include <string_view>
#include <utility>

#include "atx/core/types.hpp"

namespace atx::impl {

// Route CLI arguments to the appropriate stage function.
// Streams injected so tests can capture output without spawning a process.
// Returns a process exit code: 0 = success, 1 = stage error, 2 = parse error.
[[nodiscard]] int dispatch(int argc, char** argv,
                           std::ostream& out, std::ostream& err);

// Emit a structured digest line to `out`:
//   "[atx-impl] stage=<stage> digest=<hex16> k=v ...\n"
// digest is printed as 16 lowercase hex chars, zero-padded.
// kvs are appended in order as space-separated k=v pairs.
void emit_digest_line(std::ostream& out,
                      std::string_view stage,
                      atx::u64 digest,
                      std::initializer_list<std::pair<std::string_view, std::string>> kvs = {});

} // namespace atx::impl
