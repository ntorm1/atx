#pragma once

#include <string_view>
#include "atx/core/types.hpp"

namespace atx::engine::eval {

enum class CpcvRule : atx::u8 { ObservationV1 = 1, DateV2 = 2 };

[[nodiscard]] constexpr std::string_view cpcv_rule_name(CpcvRule rule) noexcept {
  switch (rule) {
  case CpcvRule::ObservationV1: return "observation-v1";
  case CpcvRule::DateV2: return "date-v2";
  }
  return "invalid";
}

struct CpcvConfig {
  atx::usize n_groups = 6;
  atx::usize n_test_groups = 2;
  atx::f64 embargo = 0.01;
  // V1 ignores these appended knobs. V2 ignores the fractional embargo above.
  CpcvRule rule{CpcvRule::ObservationV1};
  atx::usize embargo_dates{0};
  // Ceiling for each plan/index workspace, not overall training RSS. Learn
  // separately caps row-fold expansion and retained path metadata at this limit;
  // caller feature matrices, model state and optional traces are excluded.
  atx::u64 max_working_bytes{64ULL * 1024ULL * 1024ULL};
};

} // namespace atx::engine::eval
