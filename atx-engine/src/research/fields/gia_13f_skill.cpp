// gia_13f's arithmetic: holdings returns, centered ranks and the holders' skill score (declared in
// gia_13f_field.hpp). Sums run in the stated orders so an independent implementation can follow
// them (pairs in (filer, security) order; filer partial sums in filer order).
#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>

#include "atx/engine/research/fields/gia_13f_field.hpp"

namespace atx::engine::research::fields {
namespace {

constexpr f64 kNaN = std::numeric_limits<f64>::quiet_NaN();

// r_q(i) of every security of prev.price_sid (NaN without one).
[[nodiscard]] std::vector<f64> security_returns(const QuarterPositions &prev,
                                                const QuarterPositions &cur,
                                                const GiaParams &params) {
  const std::vector<f64> basis = basis_factors(prev, cur, prev.price_sid);
  std::vector<f64> out(prev.price_sid.size(), kNaN);
  for (usize i = 0; i < out.size(); ++i) {
    const f64 before = prev.price[i];
    const f64 after = price_of(cur, prev.price_sid[i]);
    if (!(before > 0.0) || !(after > 0.0)) {
      continue;
    }
    const f64 ratio = basis[i] * after / before;
    if (std::isfinite(ratio) && ratio <= params.max_return_ratio &&
        ratio >= 1.0 / params.max_return_ratio) {
      out[i] = ratio - 1.0;
    }
  }
  return out;
}

} // namespace

HoldingsReturns holdings_returns(const QuarterPositions &prev, const QuarterPositions &cur,
                                 const GiaParams &params) {
  const std::vector<f64> r = security_returns(prev, cur, params);
  HoldingsReturns out;
  out.filer.assign(prev.filers.size(), kNaN);
  f64 all_sum = 0.0;
  f64 all_covered = 0.0;
  f64 all_total = 0.0;
  usize p = 0;
  for (usize k = 0; k < prev.filers.size(); ++k) {
    f64 sum = 0.0;
    f64 covered = 0.0;
    for (; p < prev.pair_filer.size() && prev.pair_filer[p] == prev.filers[k]; ++p) {
      const f64 v = prev.pair_value[p];
      const auto at =
          std::lower_bound(prev.price_sid.begin(), prev.price_sid.end(), prev.pair_sid[p]);
      if (!(v > 0.0) || at == prev.price_sid.end() || *at != prev.pair_sid[p]) {
        continue;
      }
      const f64 ri = r[static_cast<usize>(at - prev.price_sid.begin())];
      if (std::isnan(ri)) {
        continue;
      }
      sum += v * ri;
      covered += v;
    }
    const f64 total = prev.filer_value[k];
    if (covered > 0.0 && covered >= params.min_covered * total) {
      out.filer[k] = sum / covered;
    }
    all_sum += sum;
    all_covered += covered;
    all_total += total;
  }
  out.aggregate = all_covered > 0.0 && all_covered >= params.min_covered * all_total
                      ? all_sum / all_covered
                      : kNaN;
  return out;
}

std::vector<f64> centered_ranks(std::span<const f64> values) {
  const usize m = values.size();
  std::vector<usize> order(m);
  std::iota(order.begin(), order.end(), usize{0});
  std::stable_sort(order.begin(), order.end(),
                   [values](usize a, usize b) { return values[a] < values[b]; });
  std::vector<f64> out(m, kNaN);
  const f64 centre = (static_cast<f64>(m) + 1.0) / 2.0;
  usize start = 0;
  while (start < m) {
    usize end = start + 1;
    while (end < m && values[order[end]] == values[order[start]]) {
      ++end;
    }
    const f64 rank = static_cast<f64>(start) + (static_cast<f64>(end - start) + 1.0) / 2.0;
    for (usize k = start; k < end; ++k) {
      out[order[k]] = (rank - centre) / static_cast<f64>(m);
    }
    start = end;
  }
  return out;
}

SecurityScores holder_scores(const QuarterPositions &positions, std::span<const f64> filer_score,
                             const GiaParams &params) {
  SecurityScores out;
  out.sid = positions.pair_sid;
  std::sort(out.sid.begin(), out.sid.end());
  out.sid.erase(std::unique(out.sid.begin(), out.sid.end()), out.sid.end());
  const usize n = out.sid.size();
  std::vector<f64> weighted(n, 0.0);
  std::vector<f64> weight(n, 0.0);
  std::vector<usize> holders(n, 0);
  usize k = 0;
  for (usize p = 0; p < positions.pair_filer.size(); ++p) {
    while (k + 1 < positions.filers.size() && positions.filers[k] != positions.pair_filer[p]) {
      ++k; // pairs are ordered by filer, and every filer holds a pair
    }
    if (positions.filers[k] != positions.pair_filer[p] || k >= filer_score.size()) {
      continue;
    }
    const f64 u = filer_score[k];
    const f64 total = positions.filer_value[k];
    if (!std::isfinite(u) || !(total > 0.0)) {
      continue;
    }
    const f64 w = positions.pair_value[p] / total;
    if (!(w > 0.0)) {
      continue;
    }
    const auto at = static_cast<usize>(
        std::lower_bound(out.sid.begin(), out.sid.end(), positions.pair_sid[p]) - out.sid.begin());
    weighted[at] += w * u;
    weight[at] += w;
    ++holders[at];
  }
  out.value.assign(n, kNaN);
  for (usize i = 0; i < n; ++i) {
    if (holders[i] >= params.min_holders && weight[i] > 0.0) {
      out.value[i] = weighted[i] / weight[i];
    }
  }
  return out;
}

} // namespace atx::engine::research::fields
