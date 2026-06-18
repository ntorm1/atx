#pragma once

#include <initializer_list>
#include <ostream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

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
// Overload 1: initializer_list (used by tests directly).
void emit_digest_line(std::ostream& out,
                      std::string_view stage,
                      atx::u64 digest,
                      std::initializer_list<std::pair<std::string_view, std::string>> kvs = {});

// Overload 2: vector<pair<string,string>> (used by dispatch with StageResult::kvs).
void emit_digest_line(std::ostream& out,
                      std::string_view stage,
                      atx::u64 digest,
                      const std::vector<std::pair<std::string, std::string>>& kvs);

} // namespace atx::impl
