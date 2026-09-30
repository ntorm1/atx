#include "atx/engine/data/role_panel.hpp"

#include <cmath>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace atx::engine::data {

// Moved verbatim from the IC runner's dsl_panel (strategy_ic_library.cpp, platform v8 B-3 split).
core::Result<alpha::Panel> overlay_panel(const alpha::Panel &base, std::vector<std::string> extra_names,
                                         std::vector<std::span<const f64>> extra_columns) {
  const auto d = base.dates(), n = base.instruments();
  std::vector<std::string> names;
  std::vector<std::span<const f64>> columns;
  names.reserve(base.num_fields() + extra_names.size());
  columns.reserve(base.num_fields() + extra_names.size());
  for (usize f = 0; f < base.num_fields(); ++f) {
    names.push_back(base.field_name(f));
    columns.push_back(base.field_all(static_cast<alpha::FieldId>(f)));
  }
  for (usize k = 0; k < extra_names.size(); ++k) {
    names.push_back(std::move(extra_names[k]));
    columns.push_back(extra_columns[k]);
  }
  std::vector<u8> presence(d * n);
  for (usize t = 0; t < d; ++t)
    for (usize i = 0; i < n; ++i) presence[t * n + i] = static_cast<u8>(base.in_universe(t, i));
  return alpha::Panel::create_borrowed(d, n, std::move(names), std::move(columns), std::move(presence));
}

// Moved verbatim from the IC runner's guard_for (strategy_ic_runner.cpp).
core::Result<std::vector<u32>> research_return_guard(const StrategyRoleData &role) {
  const auto &p = role.panel;
  const auto d = p.dates(), n = p.instruments();
  ATX_TRY(auto close_id, p.field_id("close"));
  ATX_TRY(auto raw_id, p.field_id("raw_close"));
  const auto close = p.field_all(close_id), raw = p.field_all(raw_id);
  std::vector<u32> out(d * n, 0);
  for (usize t = 1; t < d; ++t)
    for (usize i = 0; i < n; ++i) {
      const auto a = (t - 1) * n + i, b = t * n + i;
      bool bad = false;
      if (p.in_universe(t - 1, i) && p.in_universe(t, i) && std::isfinite(close[a]) &&
          std::isfinite(close[b]) && close[a] > 0 && close[b] > 0) {
        const auto r = std::log(close[b]) - std::log(close[a]);
        bad = std::abs(r) > 1.5;
        if (std::isfinite(raw[a]) && std::isfinite(raw[b]) && raw[a] > 0 && raw[b] > 0)
          bad = bad || std::abs(r) > std::abs(std::log(raw[b]) - std::log(raw[a])) + .10;
      }
      out[b] = out[a] + static_cast<u32>(bad);
    }
  return core::Ok(std::move(out));
}

} // namespace atx::engine::data
