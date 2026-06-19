#include "atx/engine/regime/with_regime_fields.hpp"

#include <limits>
#include <string>

#include "atx/engine/regime/series.hpp"

namespace atx::engine::regime {

namespace {
constexpr atx::f64 kNaN = std::numeric_limits<atx::f64>::quiet_NaN();

[[nodiscard]] bool in_universe(const std::vector<std::uint8_t> &u, atx::usize idx) noexcept {
  return u.empty() || u[idx] != 0;  // empty == all in-universe (matches with_datafields)
}
[[nodiscard]] bool has_name(const std::vector<std::string> &names, const std::string &n) noexcept {
  for (const std::string &x : names) {
    if (x == n) return true;
  }
  return false;
}
}  // namespace

atx::core::Result<alpha::Panel>
with_regime_fields(atx::usize dates, atx::usize instruments,
                   std::span<const atx::i64> panel_dates, std::vector<std::string> field_names,
                   std::vector<std::vector<atx::f64>> field_data,
                   std::vector<std::uint8_t> universe, const RegimeStore &store,
                   const std::vector<std::string> &requested_series) {
  if (panel_dates.size() != dates) {
    return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                          "with_regime_fields: panel_dates length != dates");
  }
  const atx::usize cells = dates * instruments;
  for (const std::string &s : requested_series) {
    const std::string fname = std::string{kRegimePrefix} + s;
    if (has_name(field_names, fname)) {
      return atx::core::Err(atx::core::ErrorCode::AlreadyExists,
                            std::string{"with_regime_fields: field '"} + fname + "' already present");
    }
    std::vector<atx::f64> col(cells, kNaN);
    for (atx::usize d = 0; d < dates; ++d) {
      const atx::f64 v = store.value(s, panel_dates[d]);
      for (atx::usize i = 0; i < instruments; ++i) {
        const atx::usize idx = d * instruments + i;
        col[idx] = in_universe(universe, idx) ? v : kNaN;
      }
    }
    field_names.push_back(fname);
    field_data.push_back(std::move(col));
  }
  return alpha::Panel::create(dates, instruments, std::move(field_names), std::move(field_data),
                              std::move(universe));
}

}  // namespace atx::engine::regime
