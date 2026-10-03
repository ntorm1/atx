// atx::engine::combine — marginal rank IC bodies (platform v8 F-2). Contracts live in
// combine/marginal_rank_ic.hpp.

#include "atx/engine/combine/marginal_rank_ic.hpp"

#include <algorithm> // std::adjacent_find, std::clamp, std::fill, std::sort
#include <array>     // std::array
#include <cmath>     // std::isfinite, std::sqrt
#include <span>      // std::span
#include <utility>   // std::pair
#include <vector>    // std::vector

#include "atx/core/error.hpp"
#include "atx/core/types.hpp"
#include "atx/engine/combine/orthogonalize.hpp" // residualize_signal, PanelView, ExposureView
#include "atx/engine/eval/hac.hpp"             // mean_inference

namespace atx::engine::combine {

using atx::f64;
using atx::u8;
using atx::usize;

namespace {

using Ranked = std::pair<f64, usize>;

// A residual whose norm is below this fraction of the candidate's own dispersion is the
// round-off of an exact projection (the candidate lies in the regressors' span).
constexpr f64 kSpannedTolerance = 1e-10;

// Ascending by value; the name breaks ties, so the order (and every rank) is deterministic.
void sort_ranked(std::vector<Ranked> &sorted) { std::sort(sorted.begin(), sorted.end()); }

// Tie-averaged 0-based ranks of values[i] over the ascending name list `names`, written to
// out[i]; other cells of `out` are left as they are. Every values[i] on `names` is finite.
void average_ranks(std::span<const f64> values, std::span<const usize> names, std::span<f64> out,
                   std::vector<Ranked> &sorted) {
  sorted.clear();
  for (const usize i : names) {
    sorted.emplace_back(values[i], i);
  }
  sort_ranked(sorted);
  for (usize b = 0U; b < sorted.size();) {
    usize e = b + 1U;
    while (e < sorted.size() && sorted[e].first == sorted[b].first) {
      ++e;
    }
    const f64 rank = (static_cast<f64>(b) + static_cast<f64>(e - 1U)) * 0.5;
    for (usize j = b; j < e; ++j) {
      out[sorted[j].second] = rank;
    }
    b = e;
  }
}

// Pearson correlation of x and y over `names` (both finite there); NaN with fewer than 3 names
// or no dispersion on either side. Two passes, so large rank levels never cancel.
[[nodiscard]] f64 pearson_on(std::span<const f64> x, std::span<const f64> y,
                             std::span<const usize> names) noexcept {
  if (names.size() < 3U) {
    return kMarginalNaN;
  }
  f64 sx = 0.0;
  f64 sy = 0.0;
  for (const usize i : names) {
    sx += x[i];
    sy += y[i];
  }
  const f64 count = static_cast<f64>(names.size());
  const f64 mx = sx / count;
  const f64 my = sy / count;
  f64 sxx = 0.0;
  f64 syy = 0.0;
  f64 sxy = 0.0;
  for (const usize i : names) {
    const f64 dx = x[i] - mx;
    const f64 dy = y[i] - my;
    sxx += dx * dx;
    syy += dy * dy;
    sxy += dx * dy;
  }
  if (!(sxx > 0.0) || !(syy > 0.0)) {
    return kMarginalNaN;
  }
  const f64 rho = sxy / std::sqrt(sxx * syy);
  return std::isfinite(rho) ? std::clamp(rho, -1.0, 1.0) : kMarginalNaN;
}

void grow(std::vector<f64> &buffer, usize n) {
  if (buffer.size() < n) {
    buffer.resize(n);
  }
}

// One date of one pair (v8 add_date's body): the clamped Pearson correlation of x and y over
// their jointly finite names, or NaN when fewer than `min_names` are joint or it is undefined.
[[nodiscard]] f64 pair_value(std::span<const f64> x, std::span<const f64> y,
                             usize min_names) noexcept {
  usize joint = 0U;
  f64 sx = 0.0;
  f64 sy = 0.0;
  f64 sxx = 0.0;
  f64 syy = 0.0;
  f64 sxy = 0.0;
  for (usize i = 0U; i < x.size(); ++i) {
    const f64 xi = x[i];
    const f64 yi = y[i];
    if (!std::isfinite(xi) || !std::isfinite(yi)) {
      continue;
    }
    ++joint;
    sx += xi;
    sy += yi;
    sxx += xi * xi;
    syy += yi * yi;
    sxy += xi * yi;
  }
  if (joint < min_names) {
    return kMarginalNaN;
  }
  const f64 count = static_cast<f64>(joint);
  const f64 vx = sxx - sx * sx / count;
  const f64 vy = syy - sy * sy / count;
  const f64 cov = sxy - sx * sy / count;
  if (!(vx > 0.0) || !(vy > 0.0)) {
    return kMarginalNaN;
  }
  const f64 rho = cov / std::sqrt(vx * vy);
  if (!std::isfinite(rho)) {
    return kMarginalNaN;
  }
  return std::clamp(rho, -1.0, 1.0);
}

} // namespace

atx::engine::BuildFlavor marginal_rank_ic_build_flavor() noexcept {
  return ATX_ENGINE_BUILD_FLAVOR;
}

atx::core::Status centred_tied_ranks(std::span<const f64> values, std::span<const u8> eligible,
                                     std::span<f64> out, std::vector<Ranked> &sorted) {
  if (out.size() != values.size() || (!eligible.empty() && eligible.size() != values.size())) {
    return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                          "centred_tied_ranks: row size mismatch");
  }
  std::fill(out.begin(), out.end(), kMarginalNaN);
  sorted.clear();
  for (usize i = 0U; i < values.size(); ++i) {
    if ((eligible.empty() || eligible[i] != u8{0}) && std::isfinite(values[i])) {
      sorted.emplace_back(values[i], i);
    }
  }
  if (sorted.size() < 2U) {
    return atx::core::Ok();
  }
  sort_ranked(sorted);
  // The composition's arithmetic (strategy_ic_composition.cpp each_centered_rank), term for term.
  const f64 denominator = 2.0 * static_cast<f64>(sorted.size() - 1U);
  for (usize b = 0U; b < sorted.size();) {
    usize e = b + 1U;
    while (e < sorted.size() && sorted[e].first == sorted[b].first) {
      ++e;
    }
    const f64 rank = (static_cast<f64>(b) + static_cast<f64>(e - 1U)) / denominator - 0.5;
    for (usize j = b; j < e; ++j) {
      out[sorted[j].second] = rank;
    }
    b = e;
  }
  return atx::core::Ok();
}

atx::core::Result<MarginalRankIcDay>
marginal_rank_ic_day(std::span<const f64> candidate,
                     std::span<const std::span<const f64>> regressors, std::span<const f64> label,
                     usize min_names, MarginalRankIcScratch &scratch) {
  const usize n = candidate.size();
  if (n == 0U || label.size() != n || regressors.size() > kMaxMarginalRegressors ||
      min_names < 3U) {
    return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                          "marginal_rank_ic_day: row shape, regressor bound or min_names");
  }
  std::array<PanelView, kMaxMarginalRegressors> pool{};
  for (usize j = 0U; j < regressors.size(); ++j) {
    if (regressors[j].size() != n) {
      return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                            "marginal_rank_ic_day: regressor row width");
    }
    pool[j] = PanelView{regressors[j], 1U, n};
  }
  if (scratch.ones.size() != n) {
    scratch.ones.assign(n, 1.0);
  }
  const ExposureView intercept{scratch.ones, 1U, n, 1U};
  const std::span<const PanelView> pool_views(pool.data(), regressors.size());
  auto residualized = residualize_signal(PanelView{candidate, 1U, n}, intercept, {}, pool_views);
  if (!residualized) {
    return atx::core::Err(residualized.error());
  }
  const std::vector<f64> &residual = *residualized;

  // Paired names: the residual (so the candidate and every regressor) and the label finite.
  auto &names = scratch.names;
  names.clear();
  usize support = 0U;
  f64 candidate_sum = 0.0;
  for (usize i = 0U; i < n; ++i) {
    if (!std::isfinite(residual[i])) {
      continue;
    }
    ++support;
    candidate_sum += candidate[i];
    if (std::isfinite(label[i])) {
      names.push_back(i);
    }
  }
  MarginalRankIcDay out;
  out.names = names.size();
  if (names.size() < min_names) {
    return atx::core::Ok(out);
  }
  const f64 candidate_mean = candidate_sum / static_cast<f64>(support);
  f64 residual_ss = 0.0;
  f64 candidate_ss = 0.0;
  for (usize i = 0U; i < n; ++i) {
    if (!std::isfinite(residual[i])) {
      continue;
    }
    residual_ss += residual[i] * residual[i];
    const f64 deviation = candidate[i] - candidate_mean;
    candidate_ss += deviation * deviation;
  }
  grow(scratch.x_rank, n);
  grow(scratch.y_rank, n);
  const std::span<f64> x_rank(scratch.x_rank.data(), n);
  const std::span<f64> y_rank(scratch.y_rank.data(), n);
  average_ranks(label, names, y_rank, scratch.sorted);
  average_ranks(candidate, names, x_rank, scratch.sorted);
  out.raw_ic = pearson_on(x_rank, y_rank, names);
  if (!(std::sqrt(residual_ss) > kSpannedTolerance * std::sqrt(candidate_ss))) {
    out.spanned = u8{1};
    out.marginal_ic = std::isfinite(out.raw_ic) ? 0.0 : kMarginalNaN;
    return atx::core::Ok(out);
  }
  // Linear in the residual: re-ranking it would let a tiny ordered remainder of the projection
  // decide the order of every name the regressors explain, and report a near-copy of the book as
  // strongly additive (seen at IC .28 on a synthetic book + 1% noise).
  out.marginal_ic = pearson_on(residual, y_rank, names);
  return atx::core::Ok(out);
}

RankIcSummary summarize_rank_ic(std::span<const f64> daily, usize hac_lag,
                                std::vector<f64> &compact) {
  compact.clear();
  for (const f64 value : daily) {
    if (std::isfinite(value)) {
      compact.push_back(value);
    }
  }
  RankIcSummary out;
  out.dates = compact.size();
  if (compact.empty()) {
    return out;
  }
  const eval::hac::MeanInference inference =
      eval::hac::mean_inference(compact, eval::hac::Kernel::BartlettV1, hac_lag, true);
  out.mean = inference.mean;
  out.hac_t = (inference.defined != u8{0}) ? inference.t : kMarginalNaN;
  return out;
}

PairwiseRowCorrelation::PairwiseRowCorrelation(usize rows, usize min_names)
    : rows_{rows}, min_names_{min_names < 3U ? usize{3} : min_names}, sum_(rows * rows, 0.0),
      count_(rows * rows, 0U), compute_(rows * rows, u8{0}) {
  for (usize a = 0U; a < rows_; ++a) {
    for (usize b = a + 1U; b < rows_; ++b) {
      compute_[a * rows_ + b] = u8{1};
    }
  }
  collect_computed();
}

void PairwiseRowCorrelation::collect_computed() {
  computed_.clear();
  for (usize a = 0U; a < rows_; ++a) {
    for (usize b = a + 1U; b < rows_; ++b) {
      if (compute_[a * rows_ + b] != u8{0}) {
        computed_.emplace_back(a, b);
      }
    }
  }
}

atx::core::Status PairwiseRowCorrelation::restrict_to(std::span<const u8> listed) {
  if (started_ || listed.size() != rows_) {
    return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                          "PairwiseRowCorrelation: restrict_to needs one flag per row, before "
                          "the first date");
  }
  for (usize a = 0U; a < rows_; ++a) {
    for (usize b = a + 1U; b < rows_; ++b) {
      if (listed[a] == u8{0} && listed[b] == u8{0}) {
        compute_[a * rows_ + b] = u8{0};
      }
    }
  }
  collect_computed();
  return atx::core::Ok();
}

atx::core::Status PairwiseRowCorrelation::seed(std::span<const PairSeed> seeds) {
  if (started_) {
    return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                          "PairwiseRowCorrelation: seed before the first date");
  }
  // Every seed is checked before any is applied, so an Err leaves the object unchanged.
  std::vector<usize> slots;
  slots.reserve(seeds.size());
  for (const PairSeed &s : seeds) {
    const usize lo = (s.a < s.b) ? s.a : s.b;
    const usize hi = (s.a < s.b) ? s.b : s.a;
    if (lo == hi || hi >= rows_ || compute_[lo * rows_ + hi] == u8{0} || !std::isfinite(s.sum) ||
        std::abs(s.sum) > static_cast<f64>(s.dates)) {
      return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                            "PairwiseRowCorrelation: seed of a pair not computed, or its sum");
    }
    slots.push_back(lo * rows_ + hi);
  }
  std::vector<usize> order(slots);
  std::sort(order.begin(), order.end());
  if (std::adjacent_find(order.begin(), order.end()) != order.end()) {
    return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                          "PairwiseRowCorrelation: one pair seeded twice");
  }
  for (usize k = 0U; k < seeds.size(); ++k) {
    compute_[slots[k]] = u8{0};
    sum_[slots[k]] = seeds[k].sum;
    count_[slots[k]] = seeds[k].dates;
  }
  collect_computed();
  return atx::core::Ok();
}

atx::core::Status
PairwiseRowCorrelation::check_rows(std::span<const std::span<const f64>> rows) const {
  if (rows.size() != rows_) {
    return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                          "PairwiseRowCorrelation: row count differs from construction");
  }
  if (rows_ == 0U) {
    return atx::core::Ok();
  }
  const usize n = rows[0].size();
  for (const auto &row : rows) {
    if (row.size() != n) {
      return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                            "PairwiseRowCorrelation: row width mismatch");
    }
  }
  return atx::core::Ok();
}

atx::core::Status PairwiseRowCorrelation::add_date(std::span<const std::span<const f64>> rows) {
  ATX_TRY_VOID(check_rows(rows));
  started_ = true;
  for (const auto &[a, b] : computed_) {
    const f64 value = pair_value(rows[a], rows[b], min_names_);
    if (std::isnan(value)) {
      continue;
    }
    sum_[a * rows_ + b] += value;
    ++count_[a * rows_ + b];
  }
  return atx::core::Ok();
}

atx::core::Status PairwiseRowCorrelation::day_values(std::span<const std::span<const f64>> rows,
                                                    std::span<f64> out) const {
  ATX_TRY_VOID(check_rows(rows));
  if (out.size() != computed_.size()) {
    return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                          "PairwiseRowCorrelation: one day value per computed pair");
  }
  for (usize p = 0U; p < computed_.size(); ++p) {
    out[p] = pair_value(rows[computed_[p].first], rows[computed_[p].second], min_names_);
  }
  return atx::core::Ok();
}

atx::core::Status PairwiseRowCorrelation::accumulate(std::span<const f64> values) {
  if (values.size() != computed_.size()) {
    return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                          "PairwiseRowCorrelation: one day value per computed pair");
  }
  started_ = true;
  for (usize p = 0U; p < computed_.size(); ++p) {
    if (std::isnan(values[p])) {
      continue;
    }
    const usize at = computed_[p].first * rows_ + computed_[p].second;
    sum_[at] += values[p];
    ++count_[at];
  }
  return atx::core::Ok();
}

f64 PairwiseRowCorrelation::sum(usize a, usize b) const noexcept {
  if (a == b || a >= rows_ || b >= rows_) {
    return 0.0;
  }
  const usize lo = (a < b) ? a : b;
  const usize hi = (a < b) ? b : a;
  return sum_[lo * rows_ + hi];
}

f64 PairwiseRowCorrelation::mean(usize a, usize b) const noexcept {
  if (a == b || a >= rows_ || b >= rows_) {
    return kMarginalNaN;
  }
  const usize lo = (a < b) ? a : b;
  const usize hi = (a < b) ? b : a;
  const usize count = count_[lo * rows_ + hi];
  return (count == 0U) ? kMarginalNaN : sum_[lo * rows_ + hi] / static_cast<f64>(count);
}

usize PairwiseRowCorrelation::dates(usize a, usize b) const noexcept {
  if (a == b || a >= rows_ || b >= rows_) {
    return 0U;
  }
  const usize lo = (a < b) ? a : b;
  const usize hi = (a < b) ? b : a;
  return count_[lo * rows_ + hi];
}

} // namespace atx::engine::combine
