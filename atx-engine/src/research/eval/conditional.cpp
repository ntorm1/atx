#include "atx/engine/research/eval/conditional.hpp"

#include <cmath>
#include <utility>

namespace atx::engine::research::eval {
namespace {

constexpr f64 kSqrt1_2 = 0.70710678118654752440;
constexpr f64 kLogSqrt2Pi = 0.91893853320467274178; // log(sqrt(2 pi))
constexpr f64 kTailSwitch = 5.0;
constexpr int kMillsTerms = 200;
constexpr int kBisections = 200;
constexpr int kExpansions = 40;
constexpr f64 kLowStart = 10.0; // first lower bracket: c - 10 se
constexpr f64 kHighReach = 40.0; // upper bracket: x + 40 se

// F_mu(x) in standardized units: zx = (x - mu)/se, zc = (c - mu)/se, zx > zc.
[[nodiscard]] f64 conditional_cdf(f64 zx, f64 zc) noexcept {
  return -std::expm1(log_upper_tail(zx) - log_upper_tail(zc));
}

// mu (in se units, relative to x = 0 after standardizing by the caller) with F_mu = target.
// Returns nullopt when the lower bracket cannot be found (clamped).
[[nodiscard]] std::optional<f64> solve(f64 x, f64 c, f64 target) noexcept {
  f64 hi = x + kHighReach;
  f64 lo = c - kLowStart;
  f64 reach = kLowStart;
  bool bracketed = conditional_cdf(x - lo, c - lo) > target;
  for (int k = 0; k < kExpansions && !bracketed; ++k) {
    reach *= 2.0;
    lo = c - reach;
    bracketed = conditional_cdf(x - lo, c - lo) > target;
  }
  if (!bracketed || !(conditional_cdf(x - hi, c - hi) < target)) {
    return std::nullopt;
  }
  for (int k = 0; k < kBisections; ++k) {
    const f64 mid = 0.5 * (lo + hi);
    if (mid == lo || mid == hi) {
      break;
    }
    if (conditional_cdf(x - mid, c - mid) > target) {
      lo = mid;
    } else {
      hi = mid;
    }
  }
  return 0.5 * (lo + hi);
}

} // namespace

f64 log_upper_tail(f64 z) noexcept {
  if (z < kTailSwitch) {
    return std::log(0.5 * std::erfc(z * kSqrt1_2));
  }
  // Q(z) = phi(z) R(z), R(z) = 1 / (z + 1 / (z + 2 / (z + 3 / (z + ...)))), evaluated backwards.
  f64 r = z;
  for (int k = kMillsTerms; k >= 1; --k) {
    r = z + static_cast<f64>(k) / r;
  }
  return -0.5 * z * z - kLogSqrt2Pi - std::log(r);
}

core::Result<ConditionalEstimate> conditional_estimate(f64 raw, f64 se, f64 threshold,
                                                       f64 alpha) {
  if (!std::isfinite(raw) || !std::isfinite(threshold) || !std::isfinite(se) || !(se > 0.0) ||
      !(alpha > 0.0) || !(alpha < 1.0)) {
    return core::Err(core::ErrorCode::InvalidArgument,
                     "conditional: raw, threshold finite; se > 0; alpha in (0, 1)");
  }
  ConditionalEstimate out;
  out.raw = raw;
  out.se = se;
  out.threshold = threshold;
  out.alpha = alpha;
  const f64 zc0 = threshold / se;
  out.null_bias = se * std::exp(-0.5 * zc0 * zc0 - kLogSqrt2Pi - log_upper_tail(zc0));
  out.selected = raw > threshold;
  if (!out.selected) {
    return core::Ok(std::move(out));
  }
  const f64 x = raw / se;
  const f64 c = threshold / se;
  const std::optional<f64> mid = solve(x, c, 0.5);
  const std::optional<f64> lo = solve(x, c, 1.0 - alpha / 2.0);
  const std::optional<f64> hi = solve(x, c, alpha / 2.0);
  out.clamped = !mid || !lo || !hi;
  if (mid) {
    out.median_unbiased = *mid * se;
  }
  if (lo) {
    out.ci_lo = *lo * se;
  }
  if (hi) {
    out.ci_hi = *hi * se;
  }
  return core::Ok(std::move(out));
}

core::Result<CumulativeConditional> cumulative_conditional(std::span<const LineageCell> cells,
                                                           f64 alpha) {
  CumulativeConditional out;
  f64 adjusted = 0.0;
  bool complete = true;
  for (const LineageCell &cell : cells) {
    ATX_TRY(ConditionalEstimate row,
            conditional_estimate(cell.raw, cell.se, cell.threshold, alpha));
    if (row.selected) {
      ++out.cells;
      out.raw_sum += cell.raw;
      out.null_bias_sum += row.null_bias;
      if (row.median_unbiased) {
        adjusted += *row.median_unbiased;
      } else {
        complete = false;
      }
    }
    out.rows.push_back(std::move(row));
  }
  if (complete) {
    out.adjusted_sum = adjusted;
  }
  return core::Ok(std::move(out));
}

} // namespace atx::engine::research::eval
