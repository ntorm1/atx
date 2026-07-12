#include "atx/vol/fit_policy.hpp"

#include <algorithm>
#include <cmath>

namespace atx::vol {

namespace {

[[nodiscard]] bool is_event_window(const FitContext &context) noexcept {
  if (context.event_phase != EventPhase::None) {
    return true;
  }
  return context.event_distance_days.has_value() && *context.event_distance_days <= 7u;
}

// C8 spends eight free parameters on one slice. A board too thin to support an
// even/odd holdout is also too thin to identify them, whatever the classifier's
// provenance -- a ticker prior tells us WHICH underlier this is, not that today's
// snapshot carries enough quotes to fit it. Fall back to the five-parameter eSSVI
// backbone, which is C8's own seed family.
[[nodiscard]] bool board_supports_c8(const FitDecision &out,
                                     const FitPolicyConfig &config) noexcept {
  return out.features.n_live_quotes >= config.sparse_validation_floor;
}

void configure_direct_route(FitDecision &out, const FitContext &context,
                            const FitPolicyConfig &config) noexcept {
  switch (out.profile.kind) {
  case ProfileKind::IndexEtfUltraLiquid:
    out.preset = FitPreset::Hft;
    out.curve.kind = VolCurveKind::LinearVariance;
    break;
  case ProfileKind::MegaCapEvent:
    if (is_event_window(context) && out.features.n_live_quotes >= 1500u) {
      // A dense event board already contains the W-shape in its market nodes;
      // direct variance preserves it exactly and is materially faster than an
      // eight-parameter fit. C8 remains the event route below dense-board scale.
      out.preset = FitPreset::Hft;
      out.curve.kind = VolCurveKind::LinearVariance;
    } else {
      out.preset = FitPreset::Fast;
      out.curve.kind = is_event_window(context) && board_supports_c8(out, config)
                           ? VolCurveKind::C8
                           : VolCurveKind::Essvi;
    }
    break;
  case ProfileKind::LiquidSingleName:
    out.preset = FitPreset::Fast;
    out.curve.kind = VolCurveKind::Essvi;
    break;
  case ProfileKind::OrdinarySingleName:
    out.preset = FitPreset::Robust;
    out.curve.kind = VolCurveKind::Essvi;
    break;
  case ProfileKind::IlliquidSmallCap:
    out.preset = FitPreset::Fast;
    out.curve.kind = VolCurveKind::Svi;
    break;
  case ProfileKind::HtbDividendName:
    out.preset = FitPreset::Accurate;
    out.curve.kind = VolCurveKind::Svi;
    break;
  case ProfileKind::VolProduct:
    out.preset = FitPreset::Fast;
    out.curve.kind = VolCurveKind::Svi;
    break;
  }

  // In the first minutes, boards are often incomplete and unstable; keep the
  // low-dimensional family even when a mega-cap event prior exists. Only the
  // MegaCapEvent route ever reaches C8, so no profile test is needed here.
  if (context.session_phase == MarketSessionPhase::Opening && out.curve.kind == VolCurveKind::C8 &&
      (out.features.n_live_quotes < 500u || out.features.median_spread_pct > 0.25)) {
    out.curve.kind = VolCurveKind::Essvi;
  }
}

} // namespace

SurfaceAdmissionDecision evaluate_surface_admission(const SurfaceAdmissionEvidence &evidence,
                                                    const FitAdmissionPolicy &policy) noexcept {
  SurfaceAdmissionDecision out;
  if (!policy.enabled) {
    out.admitted = true;
    return out;
  }

  const auto fail = [&out](SurfaceAdmissionReason reason) {
    out.failed_checks |= surface_admission_reason_mask(reason);
    if (out.primary_reason == SurfaceAdmissionReason::None) {
      out.primary_reason = reason;
    }
  };
  const double expiry_coverage = evidence.attempted_expiries > 0u
                                     ? static_cast<double>(evidence.fitted_expiries) /
                                           static_cast<double>(evidence.attempted_expiries)
                                     : 0.0;
  const double quote_coverage = evidence.attempted_quotes > 0u
                                    ? static_cast<double>(evidence.fitted_quotes) /
                                          static_cast<double>(evidence.attempted_quotes)
                                    : 0.0;

  if (evidence.fitted_expiries > evidence.attempted_expiries ||
      evidence.fitted_quotes > evidence.attempted_quotes) {
    fail(SurfaceAdmissionReason::ImpossibleEvidence);
  }

  if (evidence.fitted_expiries < policy.min_fitted_expiries) {
    fail(SurfaceAdmissionReason::InsufficientFittedExpiries);
  }
  if (!std::isfinite(policy.min_expiry_coverage) || policy.min_expiry_coverage < 0.0 ||
      policy.min_expiry_coverage > 1.0 || expiry_coverage < policy.min_expiry_coverage) {
    fail(SurfaceAdmissionReason::InsufficientExpiryCoverage);
  }
  if (!std::isfinite(policy.min_quote_coverage) || policy.min_quote_coverage < 0.0 ||
      policy.min_quote_coverage > 1.0 || quote_coverage < policy.min_quote_coverage) {
    fail(SurfaceAdmissionReason::InsufficientQuoteCoverage);
  }
  if (policy.require_front_expiry && !evidence.front_expiry_fitted) {
    fail(SurfaceAdmissionReason::FrontExpiryMissing);
  }
  if (evidence.max_consecutive_expiry_gaps > policy.max_consecutive_expiry_gaps) {
    fail(SurfaceAdmissionReason::ConsecutiveExpiryGap);
  }
  if (evidence.duplicate_maturities) {
    fail(SurfaceAdmissionReason::DuplicateMaturity);
  }
  if (!evidence.finite_diagnostics || !std::isfinite(evidence.worst_frac_within_bidask)) {
    fail(SurfaceAdmissionReason::NonFiniteDiagnostics);
  }
  if (policy.require_calendar_arb_free && !evidence.calendar_arb_free) {
    fail(SurfaceAdmissionReason::CalendarArbitrage);
  }
  if (!std::isfinite(policy.min_worst_frac_within_bidask) ||
      policy.min_worst_frac_within_bidask < 0.0 || policy.min_worst_frac_within_bidask > 1.0 ||
      evidence.worst_frac_within_bidask < policy.min_worst_frac_within_bidask) {
    fail(SurfaceAdmissionReason::QualityBelowFloor);
  }
  if (!evidence.finite_iv_domain) {
    fail(SurfaceAdmissionReason::FiniteIvDomain);
  }
  if (!evidence.european_price_bounds) {
    fail(SurfaceAdmissionReason::EuropeanPriceBounds);
  }
  if (policy.consumer == SurfaceConsumer::Quote || policy.consumer == SurfaceConsumer::Risk) {
    if (!evidence.strike_monotone) {
      fail(SurfaceAdmissionReason::StrikeMonotonicity);
    }
    if (!evidence.strike_convex) {
      fail(SurfaceAdmissionReason::StrikeConvexity);
    }
  }
  if (policy.consumer == SurfaceConsumer::Risk) {
    if (!evidence.calendar_total_variance) {
      fail(SurfaceAdmissionReason::CalendarTotalVariance);
    }
    if (!evidence.forward_variance_nonnegative) {
      fail(SurfaceAdmissionReason::ForwardVariance);
    }
  }
  if ((policy.require_short_tenor && !evidence.has_short_tenor) ||
      (policy.require_medium_tenor && !evidence.has_medium_tenor) ||
      (policy.require_long_tenor && !evidence.has_long_tenor)) {
    fail(SurfaceAdmissionReason::RequiredTenorBucket);
  }
  out.admitted = out.failed_checks == 0u;
  return out;
}

FitDecision select_fit_policy(const Underlying &under, std::string_view ticker,
                              const FitContext &context, const FitPolicyConfig &config) noexcept {
  FitDecision out;
  out.features = classifier_inputs_from_underlier(under);
  if (context.event_distance_days.has_value()) {
    out.features.event_distance_days = *context.event_distance_days;
  }
  if (context.forward_dispersion_bp.has_value()) {
    out.features.forward_dispersion_bp = *context.forward_dispersion_bp;
  }
  if (context.median_q_eff.has_value()) {
    out.features.median_q_eff = *context.median_q_eff;
  }
  if (context.htb.has_value()) {
    out.features.htb_flag = *context.htb;
  }
  out.features.vol_product = context.vol_product;

  ProfileKind seed_kind{};
  if (context.profile_override.has_value()) {
    out.profile = {*context.profile_override, 1.0};
    out.source = FitDecisionSource::ProfileOverride;
  } else if (ticker_seed_profile(ticker, seed_kind)) {
    // Ask the seed table directly rather than inferring provenance from the
    // confidence a seeded verdict happens to carry. An unseeded ticker falls
    // through to board voting, which classifies the features we just enriched
    // with the caller's context.
    out.profile = {seed_kind, kTickerSeedConfidence};
    out.source = FitDecisionSource::TickerPrior;
  } else {
    out.profile = classify_profile(out.features);
    out.source = FitDecisionSource::BoardFeatures;
  }

  configure_direct_route(out, context, config);
  out.primary_curve = out.curve;

  if (config.mode == FitSelectionMode::CrossValidated) {
    out.needs_cross_validation = true;
    out.source = FitDecisionSource::CrossValidation;
  } else if (out.features.n_live_quotes < config.sparse_validation_floor &&
             out.source == FitDecisionSource::BoardFeatures) {
    // Scoped to board voting on purpose: a thin board makes the VOTE unreliable,
    // so reclassifying it as a small cap recovers information. A ticker prior or
    // an operator override still knows which underlier this is, and demoting SPY
    // to IlliquidSmallCap because of one thin snapshot would throw that away. The
    // model-capacity risk those sources do carry is handled where it belongs, by
    // board_supports_c8() in configure_direct_route.
    out.needs_cross_validation = false;
    out.source = FitDecisionSource::SparseGuard;
    out.profile = {ProfileKind::IlliquidSmallCap, std::max(out.profile.confidence, 0.80)};
    configure_direct_route(out, context, config);
    out.primary_curve = out.curve;
  } else if (config.validate_ambiguous && out.profile.confidence < config.min_direct_confidence) {
    out.needs_cross_validation = true;
    out.source = FitDecisionSource::CrossValidation;
  }
  return out;
}

} // namespace atx::vol
