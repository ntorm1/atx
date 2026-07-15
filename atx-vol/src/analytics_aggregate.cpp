// Aggregators — the full single-surface analytic bundle, the session/fitted/
// fitter convenience overloads, the earnings implied-move solver, and the
// two-surface change bundle. Orchestrates the primitives and density TUs
// (see analytics.hpp). Implemented last (depends on the other TUs' public fns).

#include "atx/vol/analytics.hpp"

namespace atx::vol {

Result<double> earnings_implied_move(const PricedSurface&, const EventContext&) {
  return Err(ErrorCode::NotImplemented, "earnings_implied_move");
}

Result<SurfaceAnalytics> compute_surface_analytics(const PricedSurface&, const AnalyticsConfig&,
                                                   const EventContext*) {
  return Err(ErrorCode::NotImplemented, "compute_surface_analytics(PricedSurface)");
}

Result<SurfaceAnalytics> compute_surface_analytics(const VolaSession&, const AnalyticsConfig&) {
  return Err(ErrorCode::NotImplemented, "compute_surface_analytics(VolaSession)");
}

Result<SurfaceAnalytics> compute_surface_analytics(const FittedSurface&, const AnalyticsConfig&) {
  return Err(ErrorCode::NotImplemented, "compute_surface_analytics(FittedSurface)");
}

Result<SurfaceAnalytics> compute_surface_analytics(const PricerFitter&, const AnalyticsConfig&) {
  return Err(ErrorCode::NotImplemented, "compute_surface_analytics(PricerFitter)");
}

Result<SurfaceDiff> compute_surface_diff(const PricedSurface&, const PricedSurface&,
                                         const AnalyticsConfig&) {
  return Err(ErrorCode::NotImplemented, "compute_surface_diff");
}

}  // namespace atx::vol
