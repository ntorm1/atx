#pragma once

#include <string_view>
#include "atx/core/types.hpp"

namespace atx::engine::eval {

enum class PboRule : atx::u8 { LegacyGatherV1 = 1, CachedMomentsV2 = 2 };
[[nodiscard]] constexpr std::string_view pbo_rule_name(PboRule rule) noexcept {
  switch (rule) {
  case PboRule::LegacyGatherV1: return "legacy-gather-v1";
  case PboRule::CachedMomentsV2: return "cached-moments-v2";
  }
  return "unknown";
}

} // namespace atx::engine::eval
