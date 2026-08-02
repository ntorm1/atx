#include "atx/vol/detail/convex_recovery.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace atx::vol::detail {

bool should_attempt_strict_recovery(ValidationFailure failures) noexcept {
  const auto bits = static_cast<std::uint32_t>(failures);
  constexpr auto geometric = static_cast<std::uint32_t>(ValidationFailure::Butterfly) |
                             static_cast<std::uint32_t>(ValidationFailure::Calendar);
  constexpr auto recoverable = geometric | static_cast<std::uint32_t>(ValidationFailure::CarryGap);
  return (bits & geometric) != 0u && (bits & ~recoverable) == 0u;
}

ConvexRepairSpec make_strict_repair_spec(const RiskSurfaceValidationConfig &config) {
  ConvexRepairSpec spec;
  spec.k_min = config.k_min;
  spec.k_max = config.k_max;
  spec.grid_points = config.calendar_grid_points;
  spec.tolerance = 0.1 * config.calendar_total_variance_tolerance;
  return spec;
}

std::vector<double> strict_promotion_ks(const ValidationDigest &digest,
                                        const RiskSurfaceValidationConfig &config) {
  std::vector<double> ks;
  if (digest.n_calendar_violations > 0u && std::isfinite(digest.first_calendar_k)) {
    ks.push_back(digest.first_calendar_k);
  }
  if (digest.n_butterfly_violations > 0u && std::isfinite(digest.first_butterfly_k)) {
    const double k = digest.first_butterfly_k;
    ks.push_back(k);
    const std::uint32_t n = config.strike_grid_points;
    const double span = config.k_max - config.k_min;
    if (n >= 2u && span > 0.0 && k >= config.k_min && k <= config.k_max) {
      const double step = span / static_cast<double>(n - 1u);
      const auto idx = static_cast<std::int64_t>(std::floor((k - config.k_min) / step));
      for (std::int64_t i = idx - 1; i <= idx + 2; ++i) {
        if (i >= 0 && i < static_cast<std::int64_t>(n)) {
          ks.push_back(validation_grid_k(config, static_cast<std::uint32_t>(i), n));
        }
      }
    }
  }
  std::sort(ks.begin(), ks.end());
  ks.erase(std::unique(ks.begin(), ks.end()), ks.end());
  return ks;
}

} // namespace atx::vol::detail
