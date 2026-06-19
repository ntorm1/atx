#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "atx/core/error.hpp"
#include "atx/core/types.hpp"

#include "atx/engine/alpha/panel.hpp"
#include "atx/engine/regime/store.hpp"

namespace atx::engine::regime {

// Append one broadcast column per requested series to a panel's field set.
// For each requested series s and cell (date d, instrument i):
//   value = store.value(s, panel_dates[d])   if instrument i is in-universe at d
//         = NaN                               otherwise
// Field name = kRegimePrefix + s (e.g. "regime_vix"). A collision with an
// existing field name is Err(AlreadyExists). An unknown series yields an all-NaN
// column (store.value returns NaN). panel_dates.size() must equal `dates`.
[[nodiscard]] atx::core::Result<alpha::Panel>
with_regime_fields(atx::usize dates, atx::usize instruments,
                   std::span<const atx::i64> panel_dates, std::vector<std::string> field_names,
                   std::vector<std::vector<atx::f64>> field_data,
                   std::vector<std::uint8_t> universe, const RegimeStore &store,
                   const std::vector<std::string> &requested_series);

}  // namespace atx::engine::regime
