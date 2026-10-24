#pragma once

// Storage-internal ATXVSA2 helpers shared by surface_archive.cpp and surface_db.cpp
// (docs/atxvsa2-format.md). Not part of the public API.

#include <cstdint>
#include <string_view>

#include "atx/vol/api/core/types.hpp"

namespace atx::vol::detail {

// The valuation instant (`now_ts_ns`) of the FIRST record in directory order of the
// archive at `path`, from three positional reads: the header (fully validated, spec
// §4 step 3), the first directory entry and that record's header (magic, size,
// n_slices and symbol bound to the entry). Errors: NotFound (no file), IoError,
// ParseError (malformed framing, or an archive with no surfaces).
[[nodiscard]] Result<std::int64_t> read_archive_v2_first_now_ts(std::string_view path);

} // namespace atx::vol::detail
