#pragma once

// Library identity for atx-vol.

#include <string_view>

namespace atx::vol {

inline constexpr int kVersionMajor = 0;
inline constexpr int kVersionMinor = 1;
inline constexpr int kVersionPatch = 0;

// Semantic version string, e.g. "0.1.0". Stable for the process lifetime.
[[nodiscard]] std::string_view version() noexcept;

} // namespace atx::vol
