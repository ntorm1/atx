#pragma once
#include <string_view>

namespace atx::impl {
// Configure-time identity; changing it compiles only the generated implementation.
[[nodiscard]] std::string_view build_engine_git_sha() noexcept;
} // namespace atx::impl
