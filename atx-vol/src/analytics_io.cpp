// CSV serializers for the analytic bundles, following the house report style
// (`# key=value` meta header + deterministic rows; snprintf, not iostream).
// Depends only on the output struct definitions (analytics.hpp).

#include "atx/vol/analytics.hpp"

namespace atx::vol {

Status write_surface_analytics_csv(const SurfaceAnalytics&, std::string_view) {
  return Err(ErrorCode::NotImplemented, "write_surface_analytics_csv");
}

Status write_surface_diff_csv(const SurfaceDiff&, std::string_view) {
  return Err(ErrorCode::NotImplemented, "write_surface_diff_csv");
}

Status write_rnd_csv(const RiskNeutralDensity&, std::string_view) {
  return Err(ErrorCode::NotImplemented, "write_rnd_csv");
}

}  // namespace atx::vol
