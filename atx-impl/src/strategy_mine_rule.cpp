#include "strategy_mine_rule.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <span>
#include <vector>

#include "atx/engine/eval/multiple_testing.hpp"
#include "atx/engine/eval/stats_ext.hpp"

namespace atx::impl::strategy {
namespace {
// The factor of the band `count` falls in: the first band whose top is at or above it; NaN for 0
// or a count above the last top.
f64 band_factor(std::span<const MinedFactorBand> bands, u64 count) noexcept {
  if (count == 0U) return std::numeric_limits<f64>::quiet_NaN();
  for (const MinedFactorBand &band : bands)
    if (count <= band.top) return band.factor;
  return std::numeric_limits<f64>::quiet_NaN();
}
} // namespace

f64 mined_hurdle(u64 trials) noexcept {
  if (trials == 0U) return std::numeric_limits<f64>::quiet_NaN();
  return -atx::engine::eval::norm_ppf(kMinedFamilyAlpha / (2.0 * static_cast<f64>(trials)));
}

bool mined_confirm_defined(bool ic_defined, usize ic_dates, usize marginal_dates,
                           usize label_rows) noexcept {
  return ic_defined && label_rows >= kMinedMinConfirmRows && ic_dates >= kMinedMinConfirmRows &&
         marginal_dates == label_rows;
}

f64 mined_overlap_factor(u64 budget) noexcept { return band_factor(kMinedOverlapBands, budget); }

f64 mined_confirm_factor(usize reads) noexcept {
  return band_factor(kMinedConfirmBands, static_cast<u64>(reads));
}

f64 mined_overlap_corrected(f64 t, f64 factor) noexcept { return t / factor; }

std::vector<usize> mined_shortlist(std::span<const MinedRead> reads, f64 hurdle, f64 factor) {
  std::vector<usize> out;
  for (usize i = 0; i < reads.size(); ++i)
    if (std::isfinite(reads[i].f2) && mined_overlap_corrected(reads[i].f2, factor) >= hurdle)
      out.push_back(i);
  std::sort(out.begin(), out.end(), [&reads](usize a, usize b) {
    if (reads[a].f2 != reads[b].f2) return reads[a].f2 > reads[b].f2;
    return reads[a].canon_hash < reads[b].canon_hash;
  });
  return out;
}

std::vector<MinedRho> mined_rho_select(const atx::engine::combine::PairwiseRowCorrelation &rho,
                                       usize fixed_rows, usize candidates, usize min_dates,
                                       usize cap) {
  // Ruling PM5-8: a pair needs a defined rho on at least min_dates dates, and on one date at least.
  const usize min_defined = std::max<usize>(min_dates, 1U);
  std::vector<MinedRho> out(candidates);
  std::vector<usize> kept_rows;
  for (usize k = 0; k < candidates && kept_rows.size() < cap; ++k) {
    const usize row = fixed_rows + k;
    MinedRho r;
    r.read = true;
    const auto meet = [&rho, &r, row, min_defined](usize other) {
      if (rho.dates(row, other) < min_defined) {
        if (r.undefined == kMinedNoRow) r.undefined = other;
        return;
      }
      const f64 v = std::abs(rho.mean(row, other));
      if (std::isfinite(v) && (!std::isfinite(r.max_abs) || v > r.max_abs)) {
        r.max_abs = v;
        r.against = other;
      }
    };
    for (usize m = 0; m < fixed_rows; ++m) meet(m);
    for (const usize other : kept_rows) meet(other);
    r.pass = r.undefined == kMinedNoRow &&
             (!std::isfinite(r.max_abs) || r.max_abs <= kMinedMaxAbsRho);
    if (r.pass) kept_rows.push_back(row);
    out[k] = r;
  }
  return out;
}

std::vector<MinedConfirm> mined_confirm(std::span<const f64> oriented_t) {
  std::vector<MinedConfirm> out(oriented_t.size());
  std::vector<f64> p(oriented_t.size());
  const f64 factor = mined_confirm_factor(oriented_t.size());
  for (usize k = 0; k < oriented_t.size(); ++k) {
    const f64 t = oriented_t[k];
    const f64 corrected = mined_overlap_corrected(t, factor);
    p[k] = std::isfinite(corrected) ? atx::engine::eval::norm_cdf(-corrected) : 1.0;
    out[k].t = t;
    out[k].factor = factor;
    out[k].t_corrected = corrected;
    out[k].p = p[k];
  }
  const std::vector<f64> adjusted = atx::engine::eval::p_adjust_by(p);
  for (usize k = 0; k < out.size(); ++k) {
    out[k].p_by = adjusted[k];
    out[k].confirmed = std::isfinite(out[k].t_corrected) &&
                       out[k].t_corrected >= kMinedConfirmT && adjusted[k] <= kMinedConfirmBy;
  }
  return out;
}

} // namespace atx::impl::strategy
