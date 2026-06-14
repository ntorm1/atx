#include "atx/engine/data/adapt_factor.hpp"

#include <cmath>   // std::isnan
#include <limits>  // std::numeric_limits
#include <utility> // std::move

#include "atx/engine/risk/factor_model.hpp" // risk::FactorModel::create (full def)

namespace atx::engine::data {

atx::core::Result<atx::engine::risk::FactorModel>
artifact_to_factor_model(const FactorModelArtifact &a) {
  // Copy matrices from the const artifact and forward to the single validation +
  // assembly point. No independent checks here: create is the source of truth.
  return atx::engine::risk::FactorModel::create(a.X, a.F, a.D, a.fit_begin, a.fit_end);
}

atx::core::Result<RefSpans> reference_spans(const Dataset &reference, const Dataset &price,
                                             DateKey as_of_date, atx::u32 default_group) {
  // Validate that the required columns are present in the reference dataset.
  auto cap_col_r = reference.column_by_name("market_cap");
  if (!cap_col_r.has_value()) {
    return atx::core::Err(atx::core::ErrorCode::NotFound,
                          "reference_spans: reference dataset missing column 'market_cap'");
  }
  auto grp_col_r = reference.column_by_name("group_id");
  if (!grp_col_r.has_value()) {
    return atx::core::Err(atx::core::ErrorCode::NotFound,
                          "reference_spans: reference dataset missing column 'group_id'");
  }

  const std::span<const atx::f64> cap_col = cap_col_r.value();
  const std::span<const atx::f64> grp_col = grp_col_r.value();

  const atx::usize price_ni = price.num_instruments();
  const atx::usize ref_ni = reference.num_instruments();
  const std::span<const InstKey> price_insts = price.instruments();
  const std::span<const InstKey> ref_insts = reference.instruments();
  const std::span<const DateKey> ref_dates = reference.dates();

  // Resolve the as-of row index once (same for all instruments on this date).
  const std::optional<atx::usize> maybe_row = as_of_index(ref_dates, as_of_date);

  RefSpans out;
  out.market_cap.resize(price_ni, std::numeric_limits<atx::f64>::quiet_NaN());
  out.group_id.resize(price_ni, default_group);

  for (atx::usize i = 0; i < price_ni; ++i) {
    const InstKey inst = price_insts[i];

    // Linear scan for the instrument in the reference universe (cold path; small N).
    atx::usize ref_idx = ref_ni; // sentinel: not found
    for (atx::usize j = 0; j < ref_ni; ++j) {
      if (ref_insts[j] == inst) {
        ref_idx = j;
        break;
      }
    }

    if (ref_idx == ref_ni) {
      // Instrument absent from reference -> NaN / default_group (already set).
      continue;
    }

    if (!maybe_row.has_value()) {
      // as_of_date precedes all reference dates -> NaN / default_group (already set).
      continue;
    }

    const atx::usize row = maybe_row.value();
    const atx::usize flat = row * ref_ni + ref_idx;

    out.market_cap[i] = cap_col[flat];

    const atx::f64 grp_f64 = grp_col[flat];
    if (std::isnan(grp_f64)) {
      out.group_id[i] = default_group;
    } else {
      out.group_id[i] = static_cast<atx::u32>(grp_f64);
    }
  }

  return atx::core::Ok(std::move(out));
}

} // namespace atx::engine::data
