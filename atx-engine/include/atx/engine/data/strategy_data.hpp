#pragma once

#include <string>
#include <vector>

#include "atx/core/error.hpp"
#include "atx/core/types.hpp"
#include "atx/engine/alpha/panel.hpp"

namespace atx::engine::data {

// Explicit research snapshot, not verified historical publication/common-stock
// evidence. Panel mask is physical presence; membership is independent and uses
// strictly prior-session liquidity. Missing marks never become zero returns.
struct StrategyRoleData {
  alpha::Panel panel;
  std::vector<i64> session_keys, mark_times_ns, decision_times_ns;
  std::vector<u64> instrument_ids;
  std::vector<u8> decision_member;
  usize score_begin{}, score_end{};
  std::string source_sha256, membership_recipe, clock_recipe, manifest_sha256;
};

// Owns only one bounded role. SHA checks stream through the same captured file
// handle that fills each final vector; no full duplicate payload buffer. Caller
// pins the returned manifest SHA externally before treating a role as frozen.
[[nodiscard]] core::Result<StrategyRoleData> read_strategy_role(
    const std::string& manifest_path, u64 max_bytes);

} // namespace atx::engine::data
