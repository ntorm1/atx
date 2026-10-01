#include "strategy_mine_rule.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <span>
#include <vector>

#include "atx/engine/eval/multiple_testing.hpp"
#include "atx/engine/eval/stats_ext.hpp"

namespace atx::impl::strategy {

f64 mined_hurdle(u64 trials) noexcept {
  if (trials == 0U) return std::numeric_limits<f64>::quiet_NaN();
  return -atx::engine::eval::norm_ppf(kMinedFamilyAlpha / (2.0 * static_cast<f64>(trials)));
}

bool mined_confirm_defined(bool ic_defined, usize ic_dates, usize marginal_dates,
                           usize label_rows) noexcept {
  return ic_defined && label_rows >= kMinedMinConfirmRows && ic_dates >= kMinedMinConfirmRows &&
         marginal_dates == label_rows;
}

std::vector<usize> mined_shortlist(std::span<const MinedRead> reads, f64 hurdle, usize cap) {
  std::vector<usize> out;
  for (usize i = 0; i < reads.size(); ++i)
    if (std::isfinite(reads[i].f2) && reads[i].f2 >= hurdle) out.push_back(i);
  std::sort(out.begin(), out.end(), [&reads](usize a, usize b) {
    if (reads[a].f2 != reads[b].f2) return reads[a].f2 > reads[b].f2;
    return reads[a].canon_hash < reads[b].canon_hash;
  });
  if (out.size() > cap) out.resize(cap);
  return out;
}

std::vector<MinedRho> mined_rho_select(const atx::engine::combine::PairwiseRowCorrelation &rho,
                                       usize pool_rows, usize candidates) {
  std::vector<MinedRho> out(candidates);
  std::vector<usize> kept_rows;
  for (usize k = 0; k < candidates; ++k) {
    const usize row = pool_rows + k;
    MinedRho r;
    const auto meet = [&rho, &r, row](usize other) {
      const f64 v = std::abs(rho.mean(row, other));
      if (std::isfinite(v) && (!std::isfinite(r.max_abs) || v > r.max_abs)) {
        r.max_abs = v;
        r.against = other;
      }
    };
    for (usize m = 0; m < pool_rows; ++m) meet(m);
    for (const usize other : kept_rows) meet(other);
    r.pass = !std::isfinite(r.max_abs) || r.max_abs <= kMinedMaxAbsRho;
    if (r.pass) kept_rows.push_back(row);
    out[k] = r;
  }
  return out;
}

std::vector<MinedConfirm> mined_confirm(std::span<const f64> oriented_t) {
  std::vector<MinedConfirm> out(oriented_t.size());
  std::vector<f64> p(oriented_t.size());
  for (usize k = 0; k < oriented_t.size(); ++k) {
    const f64 t = oriented_t[k];
    p[k] = std::isfinite(t) ? atx::engine::eval::norm_cdf(-t) : 1.0;
    out[k].t = t;
    out[k].p = p[k];
  }
  const std::vector<f64> adjusted = atx::engine::eval::p_adjust_by(p);
  for (usize k = 0; k < out.size(); ++k) {
    out[k].p_by = adjusted[k];
    out[k].confirmed = std::isfinite(out[k].t) && out[k].t >= kMinedConfirmT &&
                       adjusted[k] <= kMinedConfirmBy;
  }
  return out;
}

} // namespace atx::impl::strategy
