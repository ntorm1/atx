#include "atx/vol/detail/risk_surface_validation.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <limits>
#include <span>
#include <vector>

#include "atx/core/error.hpp"
#include "atx/vol/black76.hpp"
#include "atx/vol/session.hpp"
#include "atx/vol/vol_curve.hpp"
#include "atx/vol/vol_surface.hpp"

namespace atx::vol {
namespace {

constexpr std::uint64_t kFnvOffset = 14695981039346656037ull;
constexpr std::uint64_t kFnvPrime = 1099511628211ull;

void hash_u64(std::uint64_t &hash, std::uint64_t value) noexcept {
  for (unsigned i = 0; i < 8; ++i) {
    hash ^= (value >> (i * 8u)) & 0xffu;
    hash *= kFnvPrime;
  }
}

void hash_double(std::uint64_t &hash, double value) noexcept {
  hash_u64(hash, std::bit_cast<std::uint64_t>(value));
}

[[nodiscard]] bool finite_nonnegative(double value) noexcept {
  return std::isfinite(value) && value >= 0.0;
}

[[nodiscard]] double sample_k(const RiskSurfaceValidationConfig &config, std::uint32_t point,
                              std::uint32_t n_points) noexcept {
  return detail::validation_grid_k(config, point, n_points);
}

// Defensive caps on the per-slice grid densification (oracle finding I-3):
// at most this many of the slice's own node k's are read from the adapter,
// and the deduplicated uniform-grid + node-k union is truncated to at most
// this many total samples. Both are generous multiples of the largest
// configured strike_grid_points (257, Accuracy) and node_cap (40, plus a
// handful of required_k calendar knots), so neither bound is expected to
// bind in practice — they exist only to keep a pathological adapter from
// making validation cost unbounded.
constexpr std::size_t kMaxNodeKsPerSlice = 1024;
constexpr std::size_t kMaxGridPointsPerSlice = 4096;

// Build slice `slice`'s validation k-grid: config.strike_grid_points uniform
// points across [k_min,k_max], UNIONED with that slice's own served node
// log-moneyness locations (when the adapter exposes them), ascending and
// deduplicated at a tight relative tolerance. Deterministic regardless of
// worker/thread scheduling: the uniform component is config-derived and the
// node component is read once, in the adapter's own (already deterministic)
// order, then sorted.
[[nodiscard]] std::vector<double> build_slice_grid(const RiskSurfaceView &surface,
                                                   std::size_t slice,
                                                   const RiskSurfaceValidationConfig &config) {
  std::vector<double> ks;
  ks.reserve(static_cast<std::size_t>(config.strike_grid_points) + 8u);
  for (std::uint32_t point = 0; point < config.strike_grid_points; ++point) {
    ks.push_back(sample_k(config, point, config.strike_grid_points));
  }
  if (surface.node_ks != nullptr) {
    std::vector<double> node_buf(kMaxNodeKsPerSlice);
    const std::size_t n_written =
        surface.node_ks(surface.context, slice, std::span<double>(node_buf));
    const std::size_t n = std::min(n_written, node_buf.size());
    for (std::size_t i = 0; i < n; ++i) {
      const double k = node_buf[i];
      if (std::isfinite(k) && k >= config.k_min && k <= config.k_max) {
        ks.push_back(k);
      }
    }
  }
  std::sort(ks.begin(), ks.end());
  ks.erase(std::unique(ks.begin(), ks.end(),
                       [](double a, double b) noexcept {
                         return std::fabs(a - b) <=
                                1.0e-9 * std::max({1.0, std::fabs(a), std::fabs(b)});
                       }),
           ks.end());
  if (ks.size() > kMaxGridPointsPerSlice) {
    ks.resize(kMaxGridPointsPerSlice); // deterministic ascending-prefix truncation
  }
  return ks;
}

[[nodiscard]] bool valid_config(const RiskSurfaceValidationConfig &cfg) noexcept {
  return std::isfinite(cfg.k_min) && std::isfinite(cfg.k_max) && cfg.k_max > cfg.k_min &&
         cfg.strike_grid_points >= 5 && cfg.calendar_grid_points >= 2 &&
         finite_nonnegative(cfg.price_bound_tolerance) &&
         finite_nonnegative(cfg.strike_monotonicity_tolerance) &&
         finite_nonnegative(cfg.convexity_slope_tolerance) &&
         finite_nonnegative(cfg.calendar_total_variance_tolerance) &&
         std::isfinite(cfg.max_abs_wing_total_variance_slope) &&
         cfg.max_abs_wing_total_variance_slope > 0.0 &&
         finite_nonnegative(cfg.wing_slope_tolerance);
}

void stamp_validation_id(ValidationDigest &out, const RiskSurfaceValidationConfig &cfg) noexcept {
  std::uint64_t hash = kFnvOffset;
  hash_double(hash, cfg.k_min);
  hash_double(hash, cfg.k_max);
  hash_u64(hash, cfg.strike_grid_points);
  hash_u64(hash, cfg.calendar_grid_points);
  hash_double(hash, cfg.price_bound_tolerance);
  hash_double(hash, cfg.strike_monotonicity_tolerance);
  hash_double(hash, cfg.convexity_slope_tolerance);
  hash_double(hash, cfg.calendar_total_variance_tolerance);
  hash_double(hash, cfg.max_abs_wing_total_variance_slope);
  hash_double(hash, cfg.wing_slope_tolerance);
  hash_u64(hash, static_cast<std::uint32_t>(out.failures));
  hash_u64(hash, out.n_slices);
  hash_u64(hash, out.n_strike_samples);
  hash_u64(hash, out.n_calendar_samples);
  hash_u64(hash, out.n_non_finite);
  hash_u64(hash, out.n_price_bound_violations);
  hash_u64(hash, out.n_strike_monotonicity_violations);
  hash_u64(hash, out.n_butterfly_violations);
  hash_u64(hash, out.n_calendar_violations);
  hash_u64(hash, out.n_wing_violations);
  hash_double(hash, out.max_price_bound_slack);
  hash_double(hash, out.max_strike_monotonicity_slack);
  hash_double(hash, out.max_butterfly_slack);
  hash_double(hash, out.max_calendar_slack);
  hash_double(hash, out.max_wing_slope_excess);
  hash_double(hash, out.first_non_finite_k);
  hash_u64(hash, out.first_non_finite_slice);
  hash_double(hash, out.first_butterfly_k);
  hash_u64(hash, out.first_butterfly_slice);
  hash_double(hash, out.first_calendar_k);
  hash_u64(hash, out.first_calendar_long_slice);
  hash_double(hash, out.first_butterfly_slope_left);
  hash_double(hash, out.first_butterfly_slope_right);
  hash_double(hash, out.first_calendar_previous_w);
  hash_double(hash, out.first_calendar_current_w);
  out.validation_id = hash;
}

std::size_t curve_slice_count(const void *ctx) noexcept {
  return static_cast<const CurveSurface *>(ctx)->n_slices();
}

double curve_maturity(const void *ctx, std::size_t idx) noexcept {
  const auto slices = static_cast<const CurveSurface *>(ctx)->slices();
  return idx < slices.size() ? slices[idx]->T() : std::numeric_limits<double>::quiet_NaN();
}

double curve_total_variance(const void *ctx, std::size_t idx, double k) noexcept {
  const auto slices = static_cast<const CurveSurface *>(ctx)->slices();
  return idx < slices.size() ? slices[idx]->w(k) : std::numeric_limits<double>::quiet_NaN();
}

// Node log-moneyness locations for one IVolCurve slice. Only the dense convex
// fit has a discrete node grid finer than the uniform validation grid (oracle
// I-3); every other curve family (a smooth parametric backbone) returns 0, so
// the validator falls back to the uniform grid alone for that slice.
std::size_t convex_node_ks(const IVolCurve &curve, std::span<double> out) noexcept {
  const auto *dense = dynamic_cast<const ConvexDenseCurve *>(&curve);
  if (dense == nullptr) {
    return 0;
  }
  const ConvexSliceFit &fit = dense->fit();
  if (!(fit.F > 0.0)) {
    return 0;
  }
  const std::size_t n = std::min(fit.u.size(), out.size());
  for (std::size_t i = 0; i < n; ++i) {
    out[i] = std::log(fit.u[i] / fit.F);
  }
  return n;
}

std::size_t curve_node_ks(const void *ctx, std::size_t idx, std::span<double> out) noexcept {
  const auto slices = static_cast<const CurveSurface *>(ctx)->slices();
  return idx < slices.size() ? convex_node_ks(*slices[idx], out) : 0;
}

std::size_t session_slice_count(const void *ctx) noexcept {
  return static_cast<const VolaSession *>(ctx)->expiries().size();
}

double session_maturity(const void *ctx, std::size_t idx) noexcept {
  const auto expiries = static_cast<const VolaSession *>(ctx)->expiries();
  return idx < expiries.size() ? expiries[idx].T : std::numeric_limits<double>::quiet_NaN();
}

double session_total_variance(const void *ctx, std::size_t idx, double k) noexcept {
  const auto &session = *static_cast<const VolaSession *>(ctx);
  const auto expiries = session.expiries();
  if (idx >= expiries.size()) {
    return std::numeric_limits<double>::quiet_NaN();
  }
  const double maturity = expiries[idx].T;
  const double forward = session.forward_at(maturity);
  const double strike = forward * std::exp(k);
  return session.total_variance(strike, maturity);
}

std::size_t session_node_ks(const void *ctx, std::size_t idx, std::span<double> out) noexcept {
  const auto &session = *static_cast<const VolaSession *>(ctx);
  const CurveSurface *curve = session.curve_override();
  if (curve == nullptr) {
    return 0;
  }
  const auto slices = curve->slices();
  return idx < slices.size() ? convex_node_ks(*slices[idx], out) : 0;
}

std::size_t vol_slice_count(const void *ctx) noexcept {
  return static_cast<const VolSurface *>(ctx)->n_slices();
}

double vol_maturity(const void *ctx, std::size_t idx) noexcept {
  const auto &surface = *static_cast<const VolSurface *>(ctx);
  if (surface.param() == Parametrization::Essvi) {
    const auto slices = surface.essvi_slices();
    return idx < slices.size() ? slices[idx].T : std::numeric_limits<double>::quiet_NaN();
  }
  if (surface.param() == Parametrization::Svi || surface.param() == Parametrization::SviMm) {
    const auto slices = surface.svi_slices();
    return idx < slices.size() ? slices[idx].T : std::numeric_limits<double>::quiet_NaN();
  }
  return std::numeric_limits<double>::quiet_NaN();
}

double vol_total_variance(const void *ctx, std::size_t idx, double k) noexcept {
  const auto &surface = *static_cast<const VolSurface *>(ctx);
  const double maturity = vol_maturity(ctx, idx);
  return std::isfinite(maturity) ? surface.w(k, maturity)
                                 : std::numeric_limits<double>::quiet_NaN();
}

} // namespace

RiskSurfaceView make_risk_surface_view(const CurveSurface &surface) noexcept {
  return RiskSurfaceView{&surface, curve_slice_count, curve_maturity, curve_total_variance,
                         curve_node_ks};
}

RiskSurfaceView make_risk_surface_view(const VolaSession &surface) noexcept {
  return RiskSurfaceView{&surface, session_slice_count, session_maturity, session_total_variance,
                         session_node_ks};
}

RiskSurfaceView make_risk_surface_view(const VolSurface &surface) noexcept {
  return RiskSurfaceView{&surface, vol_slice_count, vol_maturity, vol_total_variance};
}

Result<ValidationDigest> validate_risk_surface(RiskSurfaceView surface,
                                               const RiskSurfaceValidationConfig &config) {
  if (!surface.valid()) {
    return atx::core::Err(ErrorCode::InvalidArgument,
                          "validate_risk_surface: incomplete sampling view");
  }
  if (!valid_config(config)) {
    return atx::core::Err(ErrorCode::InvalidArgument,
                          "validate_risk_surface: invalid validation contract");
  }

  const std::size_t n_slices = surface.slice_count(surface.context);
  if (n_slices == 0 || n_slices > std::numeric_limits<std::uint32_t>::max()) {
    return atx::core::Err(ErrorCode::InvalidArgument,
                          "validate_risk_surface: surface must have slices");
  }

  ValidationDigest out;
  out.n_slices = static_cast<std::uint32_t>(n_slices);

  std::vector<double> maturities(n_slices);
  for (std::size_t slice = 0; slice < n_slices; ++slice) {
    const double maturity = surface.maturity(surface.context, slice);
    maturities[slice] = maturity;
    if (!std::isfinite(maturity) || !(maturity > 0.0) ||
        (slice > 0 && !(maturity > maturities[slice - 1]))) {
      out.failures |= ValidationFailure::InvalidDomain;
    }
  }

  std::vector<double> prices;
  std::vector<double> strikes;
  std::vector<double> variances;
  std::vector<bool> valid;

  for (std::size_t slice = 0; slice < n_slices; ++slice) {
    const double maturity = maturities[slice];
    // I-3: union the uniform grid with this slice's own served node k's (when
    // the adapter exposes them) so a node-level kink cannot alias between two
    // uniform samples on the optimization grid's own admission pass.
    const std::vector<double> grid_k = build_slice_grid(surface, slice, config);
    const std::size_t n_points = grid_k.size();
    prices.assign(n_points, 0.0);
    strikes.assign(n_points, 0.0);
    variances.assign(n_points, 0.0);
    valid.assign(n_points, false);

    for (std::size_t point = 0; point < n_points; ++point) {
      const double k = grid_k[point];
      const double strike = std::exp(k); // K/F; normalized forward is one.
      const double w = surface.total_variance(surface.context, slice, k);
      ++out.n_strike_samples;
      strikes[point] = strike;
      variances[point] = w;
      if (!std::isfinite(maturity) || !(maturity > 0.0) || !std::isfinite(strike) ||
          !std::isfinite(w) || !(w > 0.0)) {
        if (out.n_non_finite == 0u) {
          out.first_non_finite_k = k;
          out.first_non_finite_slice = static_cast<std::uint32_t>(slice);
        }
        ++out.n_non_finite;
        out.failures |= ValidationFailure::NonFinite;
        continue;
      }

      const double sigma = std::sqrt(w / maturity);
      const double price = black76_price(1.0, strike, maturity, sigma, 1.0, Side::Call);
      prices[point] = price;
      if (!std::isfinite(price)) {
        ++out.n_non_finite;
        out.failures |= ValidationFailure::NonFinite;
        continue;
      }
      valid[point] = true;

      const double lower = std::max(1.0 - strike, 0.0);
      const double lower_slack = lower - price;
      const double upper_slack = price - 1.0;
      const double slack = std::max(lower_slack, upper_slack);
      if (slack > config.price_bound_tolerance) {
        ++out.n_price_bound_violations;
        out.failures |= ValidationFailure::PriceBounds;
        out.max_price_bound_slack = std::max(out.max_price_bound_slack, slack);
      }
    }

    for (std::size_t point = 1; point < n_points; ++point) {
      if (!valid[point - 1] || !valid[point]) {
        continue;
      }
      const double slack = prices[point] - prices[point - 1];
      if (slack > config.strike_monotonicity_tolerance) {
        ++out.n_strike_monotonicity_violations;
        out.failures |= ValidationFailure::StrikeMonotonicity;
        out.max_strike_monotonicity_slack = std::max(out.max_strike_monotonicity_slack, slack);
      }
    }

    for (std::size_t point = 1; point + 1 < n_points; ++point) {
      if (!valid[point - 1] || !valid[point] || !valid[point + 1]) {
        continue;
      }
      const double slope_left =
          (prices[point] - prices[point - 1]) / (strikes[point] - strikes[point - 1]);
      const double slope_right =
          (prices[point + 1] - prices[point]) / (strikes[point + 1] - strikes[point]);
      const double slack = slope_left - slope_right;
      if (slack > config.convexity_slope_tolerance) {
        if (out.n_butterfly_violations == 0u) {
          out.first_butterfly_k = grid_k[point];
          out.first_butterfly_slice = static_cast<std::uint32_t>(slice);
          out.first_butterfly_slope_left = slope_left;
          out.first_butterfly_slope_right = slope_right;
        }
        ++out.n_butterfly_violations;
        out.failures |= ValidationFailure::Butterfly;
        out.max_butterfly_slack = std::max(out.max_butterfly_slack, slack);
      }
    }

    const auto check_wing = [&](std::size_t left, std::size_t right) {
      if (!valid[left] || !valid[right]) {
        return;
      }
      const double k_left = grid_k[left];
      const double k_right = grid_k[right];
      const double slope = (variances[right] - variances[left]) / (k_right - k_left);
      const double excess = std::abs(slope) - config.max_abs_wing_total_variance_slope;
      if (excess > config.wing_slope_tolerance) {
        ++out.n_wing_violations;
        out.failures |= ValidationFailure::Wing;
        out.max_wing_slope_excess = std::max(out.max_wing_slope_excess, excess);
      }
    };
    check_wing(0, 1);
    check_wing(n_points - 2u, n_points - 1u);
  }

  if (n_slices > 1) {
    for (std::uint32_t point = 0; point < config.calendar_grid_points; ++point) {
      const double k = sample_k(config, point, config.calendar_grid_points);
      // I-1: check EVERY calendar-grid sample for finiteness/positivity here,
      // not just pairwise deltas. The calendar grid is not guaranteed to be a
      // subset of the strike grid (e.g. Balanced: 65 calendar pts vs 97
      // strike pts), so a NaN/zero sliver at a calendar-only k must set
      // NonFinite itself instead of being silently skipped on the (false)
      // assumption it was "already reported on the strike grid".
      const auto check_calendar_sample = [&](std::size_t slice, double w) noexcept {
        if (std::isfinite(w) && w > 0.0) {
          return true;
        }
        if (out.n_non_finite == 0u) {
          out.first_non_finite_k = k;
          out.first_non_finite_slice = static_cast<std::uint32_t>(slice);
        }
        ++out.n_non_finite;
        out.failures |= ValidationFailure::NonFinite;
        return false;
      };
      double previous = surface.total_variance(surface.context, 0, k);
      bool previous_valid = check_calendar_sample(0, previous);
      for (std::size_t slice = 1; slice < n_slices; ++slice) {
        const double current = surface.total_variance(surface.context, slice, k);
        ++out.n_calendar_samples;
        const bool current_valid = check_calendar_sample(slice, current);
        if (previous_valid && current_valid) {
          const double slack = previous - current;
          if (slack > config.calendar_total_variance_tolerance) {
            if (out.n_calendar_violations == 0u) {
              out.first_calendar_k = k;
              out.first_calendar_long_slice = static_cast<std::uint32_t>(slice);
              out.first_calendar_previous_w = previous;
              out.first_calendar_current_w = current;
            }
            ++out.n_calendar_violations;
            out.failures |= ValidationFailure::Calendar;
            out.max_calendar_slack = std::max(out.max_calendar_slack, slack);
          }
        }
        previous = current;
        previous_valid = current_valid;
      }
    }
  }

  stamp_validation_id(out, config);
  return out;
}

void finalize_validation_digest(ValidationDigest &digest,
                                const RiskSurfaceValidationConfig &config) noexcept {
  stamp_validation_id(digest, config);
}

} // namespace atx::vol
