#include "atx/vol/risk_surface_validation.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <limits>
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
  return RiskSurfaceView{&surface, curve_slice_count, curve_maturity, curve_total_variance};
}

RiskSurfaceView make_risk_surface_view(const VolaSession &surface) noexcept {
  return RiskSurfaceView{&surface, session_slice_count, session_maturity, session_total_variance};
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

  const auto sample_k = [&](std::uint32_t point, std::uint32_t n_points) noexcept {
    const double fraction = static_cast<double>(point) / static_cast<double>(n_points - 1u);
    return config.k_min + fraction * (config.k_max - config.k_min);
  };

  std::vector<double> prices(config.strike_grid_points);
  std::vector<double> strikes(config.strike_grid_points);
  std::vector<double> variances(config.strike_grid_points);
  std::vector<bool> valid(config.strike_grid_points);

  for (std::size_t slice = 0; slice < n_slices; ++slice) {
    const double maturity = maturities[slice];
    std::fill(valid.begin(), valid.end(), false);

    for (std::uint32_t point = 0; point < config.strike_grid_points; ++point) {
      const double k = sample_k(point, config.strike_grid_points);
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

    for (std::uint32_t point = 1; point < config.strike_grid_points; ++point) {
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

    for (std::uint32_t point = 1; point + 1 < config.strike_grid_points; ++point) {
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
          out.first_butterfly_k = sample_k(point, config.strike_grid_points);
          out.first_butterfly_slice = static_cast<std::uint32_t>(slice);
          out.first_butterfly_slope_left = slope_left;
          out.first_butterfly_slope_right = slope_right;
        }
        ++out.n_butterfly_violations;
        out.failures |= ValidationFailure::Butterfly;
        out.max_butterfly_slack = std::max(out.max_butterfly_slack, slack);
      }
    }

    const auto check_wing = [&](std::uint32_t left, std::uint32_t right) {
      if (!valid[left] || !valid[right]) {
        return;
      }
      const double k_left = sample_k(left, config.strike_grid_points);
      const double k_right = sample_k(right, config.strike_grid_points);
      const double slope = (variances[right] - variances[left]) / (k_right - k_left);
      const double excess = std::abs(slope) - config.max_abs_wing_total_variance_slope;
      if (excess > config.wing_slope_tolerance) {
        ++out.n_wing_violations;
        out.failures |= ValidationFailure::Wing;
        out.max_wing_slope_excess = std::max(out.max_wing_slope_excess, excess);
      }
    };
    check_wing(0, 1);
    check_wing(config.strike_grid_points - 2u, config.strike_grid_points - 1u);
  }

  if (n_slices > 1) {
    for (std::uint32_t point = 0; point < config.calendar_grid_points; ++point) {
      const double k = sample_k(point, config.calendar_grid_points);
      double previous = surface.total_variance(surface.context, 0, k);
      for (std::size_t slice = 1; slice < n_slices; ++slice) {
        const double current = surface.total_variance(surface.context, slice, k);
        ++out.n_calendar_samples;
        if (!std::isfinite(previous) || !(previous > 0.0) || !std::isfinite(current) ||
            !(current > 0.0)) {
          previous = current;
          continue; // already reported on the strike grid
        }
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
        previous = current;
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
