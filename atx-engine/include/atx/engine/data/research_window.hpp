#pragma once
#include <cstdint>
#include <string_view>

namespace atx::engine::data {
// Generated values; the JSON file is the source and a test pins the two together.
inline constexpr std::int64_t kTrainBeginNs = 1'577'836'800'000'000'000LL;        // 2020-01-01T00:00Z
inline constexpr std::int64_t kTrainEndExclusiveNs = 1'704'067'200'000'000'000LL; // 2024-01-01T00:00Z
inline constexpr std::int64_t kSealBeginNs = 1'704'067'200'000'000'000LL;         // 2024-01-01T00:00Z
inline constexpr std::string_view kResearchWindowId = "research-window-v2";
[[nodiscard]] constexpr bool is_sealed(std::int64_t session_ns) noexcept { return session_ns >= kSealBeginNs; }
}  // namespace atx::engine::data
