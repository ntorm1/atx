#include "atx/engine/research/admission/traded_horizon.hpp"

#include <algorithm>
#include <cmath>
#include <new>
#include <utility>

#include "atx/engine/eval/hac.hpp"

namespace atx::engine::research::admission {

core::Result<std::vector<TradedHorizonRow>>
traded_horizon(usize candidates, usize decisions, std::span<const f64> factors_h21,
               std::span<const u8> train_mask, std::span<const i32> prior_signs) {
  namespace hac = atx::engine::eval::hac;
  if (candidates == 0U || decisions == 0U || factors_h21.size() / candidates != decisions ||
      factors_h21.size() % candidates != 0U || train_mask.size() != decisions ||
      prior_signs.size() != candidates ||
      std::any_of(train_mask.begin(), train_mask.end(), [](u8 m) { return m > 1U; })) {
    return core::Err(core::ErrorCode::InvalidArgument, "traded horizon: inconsistent geometry");
  }
  try {
    std::vector<TradedHorizonRow> rows(candidates);
    std::vector<f64> x;
    x.reserve(decisions);
    for (usize k = 0; k < candidates; ++k) {
      x.clear();
      const f64 sign = static_cast<f64>(prior_signs[k]);
      for (usize d = 0; d < decisions; ++d) {
        const f64 v = factors_h21[d * candidates + k];
        if (train_mask[d] != 0U && std::isfinite(v)) {
          x.push_back(sign * v);
        }
      }
      TradedHorizonRow &row = rows[k];
      row.days = x.size();
      if (prior_signs[k] == 0 || x.empty()) {
        continue;
      }
      row.ic = hac::detail::mean_of(x);
      const hac::MeanInference inference =
          hac::mean_tstat(x, hac::TStatRule::HorizonAwareV3, kTradedHorizonSessions);
      if (inference.defined != 0U) {
        row.hac_t = inference.t;
      }
    }
    return core::Ok(std::move(rows));
  } catch (const std::bad_alloc &) {
    return core::Err(core::ErrorCode::OutOfRange, "traded horizon: allocation failed");
  }
}

} // namespace atx::engine::research::admission
