#pragma once

// atx::tsdb — shared-memory in-memory time-series store (build-smoke unit).

#include <string_view>

namespace atx::tsdb {

/// Library version string (smoke-test anchor; replaced by real units below).
[[nodiscard]] std::string_view version() noexcept;

} // namespace atx::tsdb
