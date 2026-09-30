#pragma once

// atx::engine::data — research-role adapters shared by the strategy verbs (platform v8 H-3).
//
// Lifted from the IC runner (atx-impl strategy_ic_library.cpp dsl_panel, strategy_ic_runner.cpp
// guard_for) so the runner and the miner build their DSL panel and research IC return guard with
// one piece of code. The bodies moved verbatim; the runner's outputs are unchanged by the move.

#include <span>
#include <string>
#include <vector>

#include "atx/core/error.hpp"
#include "atx/core/types.hpp"
#include "atx/engine/alpha/panel.hpp"
#include "atx/engine/data/strategy_data.hpp"

namespace atx::engine::data {

// The DSL panel of a research role: `base`'s columns and the extra field columns, all BORROWED
// (no copy of any column), resolved by name. Only the 1 B/cell presence mask is owned, copied from
// `base`, so a field load NaNs the extras exactly where it NaNs the base fields. The result must
// not outlive `base` or the extra columns. Each extra column holds base.cells() values; Err on a
// ragged or duplicate input (Panel::create_borrowed's contract).
[[nodiscard]] core::Result<alpha::Panel> overlay_panel(const alpha::Panel &base,
                                                       std::vector<std::string> extra_names,
                                                       std::vector<std::span<const f64>> extra_columns);

// The research IC return guard of a role: date-major cumulative counts, per instrument, of the
// excluded one-day returns (prepare_research_ic's bad_return_prefix). A return between two
// observed sessions is excluded when |log close ratio| > 1.5, or when it exceeds the raw close's
// log ratio by more than .10 in absolute value (a split or dividend adjustment gone wrong). Needs
// the role's close and raw_close fields.
[[nodiscard]] core::Result<std::vector<u32>> research_return_guard(const StrategyRoleData &role);

} // namespace atx::engine::data
