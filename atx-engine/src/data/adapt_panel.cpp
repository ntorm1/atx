// atx::engine::data — adapt_panel (S6.4a): Price Dataset → alpha::Panel bridge.
//
// Implementation of price_to_panel declared in data/adapt_panel.hpp.
// Cold path; every copy here is intentional (std::vector ownership transfer).

#include "atx/engine/data/adapt_panel.hpp"

#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "atx/core/error.hpp"
#include "atx/core/types.hpp"

#include "atx/engine/alpha/datafields.hpp"
#include "atx/engine/alpha/panel.hpp"
#include "atx/engine/data/dataset.hpp"

namespace atx::engine::data {

[[nodiscard]] atx::core::Result<alpha::Panel>
price_to_panel(const Dataset &price, std::span<const atx::u16> adv_windows) {
  const atx::usize nd = price.num_dates();
  const atx::usize ni = price.num_instruments();
  const atx::usize ncols = price.schema().columns.size();

  // 1. Field names — copy in storage order (same as with_datafields expects).
  std::vector<std::string> field_names = price.schema().columns;

  // 2. Field data — materialize each column span into an owned vector.
  std::vector<std::vector<atx::f64>> field_data;
  field_data.reserve(ncols);
  for (atx::usize c = 0; c < ncols; ++c) {
    const std::span<const atx::f64> col = price.column(c);
    field_data.emplace_back(col.begin(), col.end());
  }

  // 3. Universe mask — copy raw stored bytes (empty stays empty, preserving the
  //    "all-in-universe" sentinel that with_datafields interprets the same way).
  const std::span<const std::uint8_t> raw_mask = price.mask();
  std::vector<std::uint8_t> universe(raw_mask.begin(), raw_mask.end());

  // 4. Forward to with_datafields — it validates, derives dollar_volume / vwap /
  //    adv{d}, and builds the Panel.  Do NOT re-derive anything here; that is
  //    what guarantees byte-identity with a direct call to with_datafields.
  return alpha::datafields::with_datafields(nd, ni, std::move(field_names), std::move(field_data),
                                            std::move(universe), adv_windows);
}

} // namespace atx::engine::data
