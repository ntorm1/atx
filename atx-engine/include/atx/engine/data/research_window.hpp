#pragma once
#include <cstdint>
#include <string_view>

// The research window (platform v8 W0-1, research-window-v2): TRAIN is the decision sessions in
// [kTrainBeginNs, kTrainEndExclusiveNs); nothing at or after kSealBeginNs is read. The source is
// atx-impl/strategies/research_window.json (Python: atx-engine/tools/research_window.py); no C++
// file hard-codes a TRAIN end or a seal date again.
namespace atx::engine::data {
// Generated values; the JSON file is the source and a test pins the two together.
inline constexpr std::int64_t kTrainBeginNs = 1'577'836'800'000'000'000LL;        // 2020-01-01Z
inline constexpr std::int64_t kTrainEndExclusiveNs = 1'704'067'200'000'000'000LL; // 2024-01-01Z
inline constexpr std::int64_t kSealBeginNs = 1'704'067'200'000'000'000LL;         // 2024-01-01Z
inline constexpr std::string_view kResearchWindowId = "research-window-v2";
// kSealBeginNs as the JSON writes it (seal_begin): named in refusals and output manifests.
inline constexpr std::string_view kSealBeginDate = "2024-01-01";
[[nodiscard]] constexpr bool is_sealed(std::int64_t session_ns) noexcept {
  return session_ns >= kSealBeginNs;
}

static_assert(kTrainBeginNs < kTrainEndExclusiveNs && kTrainEndExclusiveNs <= kSealBeginNs,
              "research window: train_begin < train_end_exclusive <= seal_begin");
static_assert(kTrainBeginNs % 86'400'000'000'000LL == 0 &&
                  kTrainEndExclusiveNs % 86'400'000'000'000LL == 0 &&
                  kSealBeginNs % 86'400'000'000'000LL == 0,
              "research window: every bound is a midnight UTC session label");
}  // namespace atx::engine::data
