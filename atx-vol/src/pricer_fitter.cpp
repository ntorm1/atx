#include "atx/vol/pricer_fitter.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <future>
#include <limits>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "atx/core/error.hpp"
#include "atx/vol/american_iv.hpp"  // american_implied_vol
#include "atx/vol/calib.hpp"        // build_observations_european
#include "atx/vol/correction.hpp"   // AmericanCorrectionCaches (cached inversion hot path)
#include "atx/vol/deamer.hpp"       // resolve_chain_forward
#include "atx/vol/parallel_for.hpp" // parallel_for (shared block-partition fan-out)
#include "atx/vol/risk_surface_validation.hpp"

namespace atx::vol {

using atx::core::Err;
using atx::core::ErrorCode;
using atx::core::Ok;

// Ordered fallback rungs for a primary curve family that failed to build. A
// profile is a latency prior, not permission to drop an underlier: every
// auto-routed board descends toward the minimally identified direct-variance
// curve, and the direct curve itself falls back to the parsimonious eSSVI
// backbone (a board too degenerate for market nodes may still admit a smooth
// five-parameter slice). Switching on the enum with no `default:` lets
// -Wswitch -Werror reject a new kind that forgets to declare its rungs.
std::span<const VolCurveKind> fallback_curve_rungs(VolCurveKind primary) noexcept {
  static constexpr VolCurveKind kFromC8[]{VolCurveKind::Essvi, VolCurveKind::LinearVariance};
  static constexpr VolCurveKind kFromEssvi[]{VolCurveKind::Svi, VolCurveKind::LinearVariance};
  static constexpr VolCurveKind kFromSvi[]{VolCurveKind::Essvi};
  static constexpr VolCurveKind kFromConvex[]{VolCurveKind::Svi, VolCurveKind::Essvi};
  static constexpr VolCurveKind kFromLinear[]{VolCurveKind::Essvi};
  switch (primary) {
  case VolCurveKind::C8:
    return kFromC8;
  case VolCurveKind::Essvi:
    return kFromEssvi;
  case VolCurveKind::Svi:
    return kFromSvi;
  case VolCurveKind::ConvexDense:
    return kFromConvex;
  case VolCurveKind::LinearVariance:
    return kFromLinear;
  }
  return {};
}

[[nodiscard]] RiskSurfaceValidationConfig
risk_validation_config(FitQualityMode quality_mode) noexcept;

// Non-geometric failure context carried by the session diagnostics: carry
// confidence, inversion certification, and expiry-coverage gaps (carry-gate
// skips + audit-starved slices). Merged into the oracle digest by BOTH fit()'s
// candidate validation and refit_risk_slice, so a successful incremental
// publish cannot launder a fit-time Degraded reason into clean Healthy while
// the expiry is still missing from the served surface (§5.2). Namespace-scope
// (declared in pricer_fitter.hpp) so the seam → admission contract is
// directly testable; strictly OR-only / additive-only either way.
void merge_session_failure_context(const SessionDiagnostics &diagnostics,
                                   ValidationDigest &digest) noexcept {
  if (!diagnostics.carry_confident) {
    digest.failures |= ValidationFailure::InsufficientData;
  }
  if (!diagnostics.inversion_certified) {
    digest.failures |= ValidationFailure::InversionResidual;
  }
  if (diagnostics.n_carry_skipped_expiries > 0 ||
      diagnostics.n_audit_starved_expiries > 0) {
    // §5.2: expiries dropped by the carry gate or starved by the fit audit
    // must be surfaced. CarryGap is the one publish-with-Degraded reason
    // (decide_risk_surface_admission); combined with any other failure it
    // still rejects.
    digest.failures |= ValidationFailure::CarryGap;
  }
  if (diagnostics.n_price_bound_violations > 0) {
    // Oracle finding I-2: the geometric oracle only reconstructs prices from
    // w via Black, which is always in-bounds by construction and cannot see
    // a served ConvexDense call_price() the fit clamped into range before
    // forming w. This self-report (arb_check_price_bounds over the session's
    // own served surface) is the one exception, exactly like CarryGap: it
    // may only ADD PriceBounds — and ADD the clamp count into the digest's
    // violation tally (saturating; never decremented) so a clamp-triggered
    // rejection reports how many samples breached, not a bare bit — never
    // clear anything the geometric oracle already found.
    digest.failures |= ValidationFailure::PriceBounds;
    const std::uint64_t merged =
        static_cast<std::uint64_t>(digest.n_price_bound_violations) +
        static_cast<std::uint64_t>(diagnostics.n_price_bound_violations);
    digest.n_price_bound_violations = static_cast<std::uint32_t>(std::min<std::uint64_t>(
        merged, std::numeric_limits<std::uint32_t>::max()));
  }
}

std::optional<std::size_t> ChainValuation::row_of(OptionId id) const {
  for (std::size_t i = 0; i < ids.size(); ++i) {
    if (ids[i] == id) {
      return i;
    }
  }
  return std::nullopt;
}

Status PricerFitter::fit(const OptionChain &chain) {
  using Clock = std::chrono::steady_clock;
  struct MarkBuildResult {
    Result<VolaSession> built;
    double elapsed_ms{};
  };
  const auto fit_start = Clock::now();
  const auto elapsed_ms = [](Clock::time_point start) noexcept {
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
  };

  timings_ = {};
  selection_.reset();
  decision_.reset();
  if (candidate_generation_ == std::numeric_limits<std::uint64_t>::max()) {
    return Err(ErrorCode::OutOfRange, "surface generation counter exhausted");
  }
  ++candidate_generation_;

  // One-release compatibility: legacy presets route through the single §9
  // mapping table (`map_legacy_fit_preset`, via effective_request()) so this
  // seam cannot drift from the documented product policy. An otherwise-default
  // HFT request — implicit or with the documented explicit LinearVariance pin —
  // remains the legacy mark-only surface. New callers express a low-latency
  // dual request with quality_mode=Latency and explicit outputs.
  const EffectiveRequest request = effective_request();
  const SurfaceOutputs requested_outputs = request.outputs;
  const FitQualityMode quality_mode = request.quality_mode;

  const auto configure_common = [&](SessionInputs &in) {
    if (chain.env().yield.size() > 0u) {
      in.expiry_rate_T.clear();
      in.expiry_rates.clear();
      in.expiry_rate_T.reserve(chain.underlying().chains.size());
      in.expiry_rates.reserve(chain.underlying().chains.size());
      for (const Chain &expiry : chain.underlying().chains) {
        in.expiry_rate_T.push_back(expiry.T);
        in.expiry_rates.push_back(chain.env().rate_at(expiry.T));
      }
    }
    in.cash_divs = cfg_.cash_divs.empty() ? chain.env().cash_divs : cfg_.cash_divs;
    if (cfg_.use_correction_cache.has_value()) {
      in.use_correction_cache = *cfg_.use_correction_cache;
    }
    if (cfg_.use_deam_cache_for_fit.has_value()) {
      in.use_deam_cache_for_fit = *cfg_.use_deam_cache_for_fit;
    }
    if (cfg_.max_obs_per_slice.has_value()) {
      in.calib.max_obs_per_slice = *cfg_.max_obs_per_slice;
    }
    if (cfg_.max_otm_shortcut_premium_spread_frac.has_value()) {
      in.calib.max_otm_shortcut_premium_spread_frac =
          *cfg_.max_otm_shortcut_premium_spread_frac;
    }
    if (!in.expiry_rates.empty()) {
      in.use_correction_cache = false;
      in.use_deam_cache_for_fit = false;
    }
  };

  std::optional<std::future<MarkBuildResult>> mark_future;
  if (has_output(requested_outputs, SurfacePurpose::MarketMark)) {
    SessionInputs mark_in =
        make_session_inputs(FitPreset::Hft, chain.spot(), chain.rate(), chain.now_ns());
    configure_common(mark_in);
    mark_in.curve.kind = VolCurveKind::LinearVariance;
    const Underlying &underlying = chain.underlying();
    mark_future.emplace(std::async(
        std::launch::async,
        [&underlying, mark_in = std::move(mark_in)]() mutable -> MarkBuildResult {
          const auto mark_start = Clock::now();
          Result<VolaSession> built = VolaSession::build(underlying, mark_in);
          const double duration =
              std::chrono::duration<double, std::milli>(Clock::now() - mark_start).count();
          return MarkBuildResult{std::move(built), duration};
        }));
  }

  const auto finalize_mark = [&]() -> Status {
    if (!mark_future.has_value()) {
      return Ok();
    }
    MarkBuildResult result = mark_future->get();
    mark_future.reset();
    timings_.market_mark_build_ms = result.elapsed_ms;
    if (result.built.has_value()) {
      market_mark_surface_.reset(new FittedSurface(std::move(*result.built),
                                                   SurfacePurpose::MarketMark, quality_mode,
                                                   candidate_generation_));
      market_mark_health_ = SurfaceHealth{
          .purpose = SurfacePurpose::MarketMark,
          .quality_mode = quality_mode,
          .state = SurfaceState::Healthy,
          .reasons = ValidationFailure::None,
          .candidate_generation = candidate_generation_,
          .served_generation = candidate_generation_,
      };
      return Ok();
    }
    if (market_mark_surface_ != nullptr && cfg_.fallback == SurfaceFallback::LastKnownGood) {
      market_mark_health_ = SurfaceHealth{
          .purpose = SurfacePurpose::MarketMark,
          .quality_mode = quality_mode,
          .state = SurfaceState::Stale,
          .reasons = ValidationFailure::InsufficientData,
          .candidate_generation = candidate_generation_,
          .served_generation = market_mark_surface_->generation(),
          .fallback_generation = market_mark_surface_->generation(),
      };
      return Ok();
    }
    if (cfg_.fallback == SurfaceFallback::None) {
      market_mark_surface_.reset();
    }
    market_mark_health_ = SurfaceHealth{
        .purpose = SurfacePurpose::MarketMark,
        .quality_mode = quality_mode,
        .state = SurfaceState::Rejected,
        .reasons = ValidationFailure::InsufficientData,
        .candidate_generation = candidate_generation_,
    };
    return Err(std::move(result.built).error());
  };

  if (has_output(requested_outputs, SurfacePurpose::Risk) &&
      (cfg_.risk_admission != RiskAdmission::Required ||
       (cfg_.enforce_calendar_floor.has_value() && !*cfg_.enforce_calendar_floor) ||
       (cfg_.score_parity.has_value() && !*cfg_.score_parity) ||
       (cfg_.curve.has_value() && cfg_.curve->kind == VolCurveKind::LinearVariance))) {
    ValidationDigest rejected;
    rejected.failures = ValidationFailure::InvalidDomain;
    const std::uint64_t prior = risk_surface_ != nullptr ? risk_surface_->generation() : 0u;
    risk_health_ = decide_risk_surface_admission(rejected, quality_mode, candidate_generation_,
                                                 prior, cfg_.fallback)
                       .health;
    if (cfg_.fallback == SurfaceFallback::None) {
      risk_surface_.reset();
      served_decision_.reset();
      served_selection_.reset();
    }
    // §5.6: the requested mark is still built and published on its own
    // contract; only the risk output is refused. The caller must still learn
    // the risk config was invalid, so the policy error outranks mark status.
    (void)finalize_mark();
    timings_.total_ms = elapsed_ms(fit_start);
    return Err(ErrorCode::InvalidArgument, "invalid correctness policy for requested risk surface");
  }

  if (!has_output(requested_outputs, SurfacePurpose::Risk)) {
    Status mark_status = finalize_mark();
    timings_.total_ms = elapsed_ms(fit_start);
    if (!mark_status.has_value()) {
      return mark_status;
    }
    return Ok();
  }
  const FitPreset risk_preset = quality_mode == FitQualityMode::Latency
                                    ? FitPreset::Fast
                                    : quality_mode == FitQualityMode::Accuracy
                                          ? FitPreset::Accurate
                                          : FitPreset::Robust;
  const auto risk_start = Clock::now();
  SessionInputs in = make_session_inputs(risk_preset, chain.spot(), chain.rate(), chain.now_ns());
  configure_common(in);

  const auto apply_risk_policy = [&] {
    in.score_parity = true;
    in.enforce_calendar_floor = true;
    in.calendar_repair = CalendarRepair::Project;
    in.deam.require_carry_confidence = true;
    // §8.1: every served risk fit must run audited inversions — including the
    // eSSVI fallback rung, whose aligned-obs fit path audits only under this
    // flag (the curve-driver primaries audit unconditionally). Without it the
    // rung's certificate would describe a diagnostic re-run, not the fit.
    in.deam.audit_fit_inversions = true;
    // Curve-override risk sessions deliberately serve the accurate cold pricer;
    // building a scalar-carry correction cache here adds hundreds of ms and is
    // then never used by the served path. Fast/cache IV proposals remain
    // separately audited in calibration, so skipping this dead cache is both
    // faster and semantically cleaner.
    in.use_correction_cache = false;
    // Every risk mode pins the ACCURATE Andersen-Lake reference preset
    // EXPLICITLY. An unset al_opts is the legacy-compat signal that lets
    // VolaSession::build substitute the fast preset, a loosened iv_tol, and a
    // single-pair carry floor — which would silently undo every per-mode carry
    // and inversion budget set below.
    if (quality_mode == FitQualityMode::Accuracy) {
      in.deam.al_opts = al_default_opts();
      in.use_correction_cache = false;
      in.use_deam_cache_for_fit = false;
    }
    switch (quality_mode) {
    case FitQualityMode::Latency:
      // A fast proposal plus mandatory cold audit costs more than solving the
      // smaller Latency node set accurately once. Latency comes from bounded
      // work and narrower validation, never from publishing an unaudited IV.
      in.deam.al_opts = al_default_opts();
      in.deam.n_atm = 3;
      in.deam.max_borrow_pairs = 6;
      in.calib.max_obs_per_slice = cfg_.max_obs_per_slice.value_or(40u);
      in.calib.max_otm_shortcut_premium_spread_frac =
          cfg_.max_otm_shortcut_premium_spread_frac.value_or(0.50);
      break;
    case FitQualityMode::Balanced:
      in.deam.n_atm = 8;
      in.deam.max_borrow_pairs = 12;
      // Certified fast proposals frequently require a cold fallback on dense
      // boards, paying for both paths. The direct accurate reference is faster
      // in that regime and is the correctness-first Balanced default.
      in.deam.al_opts = al_default_opts();
      in.calib.max_obs_per_slice = cfg_.max_obs_per_slice.value_or(60u);
      in.calib.max_otm_shortcut_premium_spread_frac =
          cfg_.max_otm_shortcut_premium_spread_frac.value_or(0.0);
      break;
    case FitQualityMode::Accuracy:
      in.deam.n_atm = 12;
      in.deam.max_borrow_pairs = 12;
      in.calib.max_obs_per_slice = cfg_.max_obs_per_slice.value_or(80u);
      in.calib.max_otm_shortcut_premium_spread_frac =
          cfg_.max_otm_shortcut_premium_spread_frac.value_or(0.0);
      break;
    }
  };
  apply_risk_policy();

  if (!cfg_.curve.has_value()) {
    FitDecision d =
        select_fit_policy(chain.underlying(), chain.underlying().ticker, cfg_.context, cfg_.policy);
    d.preset = risk_preset;
    if (d.curve.kind == VolCurveKind::LinearVariance) {
      d.curve.kind = VolCurveKind::ConvexDense;
      d.primary_curve = d.curve;
    }
    decision_ = std::move(d);
  }

  if (decision_.has_value()) {
    const auto profile = profile_lookup(decision_->profile.kind);
    if (profile.has_value()) {
      in.calib = profile.value()->calib;
      apply_fit_preset(in, risk_preset);
      configure_common(in);
      apply_risk_policy();
    }
  }

  if (cfg_.curve.has_value()) {
    in.curve = *cfg_.curve;
  } else if (decision_.has_value() && !decision_->needs_cross_validation) {
    decision_->curve.parametric = in.calib;
    if (decision_->curve.kind == VolCurveKind::ConvexDense) {
      decision_->curve.convex.node_cap = in.calib.max_obs_per_slice;
    }
    in.curve = decision_->curve;
  } else {
    SurfaceParityInputs sp;
    sp.S = in.S;
    sp.r = in.r;
    sp.expiry_rate_T = in.expiry_rate_T;
    sp.expiry_rates = in.expiry_rates;
    sp.cash_divs = in.cash_divs;
    sp.now_ts_ns = in.now_ts_ns;
    sp.deam = in.deam;
    sp.calib = in.calib;
    sp.band_k = in.band_k;
    sp.repair = in.calendar_repair;
    sp.score_parity = in.score_parity;
    sp.enforce_calendar_floor = in.enforce_calendar_floor;
    sp.use_deam_cache_for_fit = in.use_deam_cache_for_fit;
    Result<SelectorResult> selected = select_curve(chain.underlying(), sp, cfg_.selector);
    if (selected.has_value()) {
      SelectorResult chosen = std::move(*selected);
      chosen.chosen.parametric = in.calib;
      if (chosen.chosen.kind == VolCurveKind::LinearVariance) {
        chosen.chosen.kind = VolCurveKind::ConvexDense;
        chosen.chosen.convex.node_cap = in.calib.max_obs_per_slice;
      }
      in.curve = chosen.chosen;
      if (decision_.has_value()) {
        decision_->curve = chosen.chosen;
        decision_->preset = risk_preset;
      }
      selection_ = std::move(chosen);
    } else if (decision_.has_value()) {
      // Cross-validation is advisory among already admissible families. If its
      // held-out sample is too thin, use the profile's safe primary rather than
      // returning through an un-stamped early-exit path.
      in.curve = decision_->curve;
      in.curve.parametric = in.calib;
      if (in.curve.kind == VolCurveKind::LinearVariance) {
        in.curve.kind = VolCurveKind::ConvexDense;
      }
    }
  }

  Result<VolaSession> built = VolaSession::build(chain.underlying(), in);
  const bool auto_routed = decision_.has_value() && !cfg_.curve.has_value();
  if (!built.has_value() && auto_routed) {
    const CurveConfig primary_curve = in.curve;
    for (const VolCurveKind rung : fallback_curve_rungs(primary_curve.kind)) {
      if (rung == VolCurveKind::LinearVariance) {
        continue;
      }
      in.curve.kind = rung;
      Result<VolaSession> retry = VolaSession::build(chain.underlying(), in);
      if (!retry.has_value()) {
        continue;
      }
      decision_->primary_curve = primary_curve;
      decision_->curve = in.curve;
      decision_->used_fallback = true;
      if (selection_.has_value()) {
        // The selector's candidate could not be built; re-stamp the served
        // choice so provenance names the family actually fit (the scores stay
        // as the selector's audit trail).
        selection_->chosen = in.curve;
      }
      built = std::move(retry);
      break;
    }
  }
  timings_.risk_build_ms = elapsed_ms(risk_start);
  if (!built.has_value()) {
    ValidationDigest failed;
    failed.failures = ValidationFailure::InsufficientData;
    const std::uint64_t prior = risk_surface_ != nullptr ? risk_surface_->generation() : 0u;
    risk_health_ = decide_risk_surface_admission(failed, quality_mode, candidate_generation_, prior,
                                                 cfg_.fallback)
                       .health;
    (void)finalize_mark();
    timings_.total_ms = elapsed_ms(fit_start);
    if (risk_surface_ != nullptr && risk_health_.using_fallback()) {
      return Ok();
    }
    if (cfg_.fallback == SurfaceFallback::None) {
      risk_surface_.reset();
      served_decision_.reset();
      served_selection_.reset();
    }
    return Err(std::move(built).error());
  }

  VolaSession sess = std::move(*built);
  const RiskSurfaceValidationConfig validation_config =
      risk_validation_config(quality_mode);
  const auto validate_candidate = [&](const VolaSession &candidate) {
    const auto validation_start = Clock::now();
    Result<ValidationDigest> checked =
        validate_risk_surface(candidate, validation_config);
    timings_.risk_validation_ms += elapsed_ms(validation_start);
    ValidationDigest result;
    if (checked.has_value()) {
      result = *checked;
    } else {
      result.failures = ValidationFailure::InvalidDomain;
    }
    merge_session_failure_context(candidate.diagnostics(), result);
    finalize_validation_digest(result, validation_config);
    return result;
  };

  ValidationDigest digest = validate_candidate(sess);
  const std::uint64_t prior = risk_surface_ != nullptr ? risk_surface_->generation() : 0u;
  AdmissionDecision admission = decide_risk_surface_admission(
      digest, quality_mode, candidate_generation_, prior, cfg_.fallback);

  // A policy curve is only a prior. Validation rejection walks the same safe
  // model ladder as a construction failure; each rung must independently pass
  // the complete admission contract before it can replace the candidate.
  if (!admission.publish_candidate && auto_routed) {
    const CurveConfig rejected_curve = in.curve;
    for (const VolCurveKind rung : fallback_curve_rungs(rejected_curve.kind)) {
      if (rung == VolCurveKind::LinearVariance) continue;
      SessionInputs retry_inputs = in;
      retry_inputs.curve.kind = rung;
      const auto retry_start = Clock::now();
      Result<VolaSession> retry =
          VolaSession::build(chain.underlying(), retry_inputs);
      timings_.risk_build_ms += elapsed_ms(retry_start);
      if (!retry.has_value()) continue;
      ValidationDigest retry_digest = validate_candidate(*retry);
      AdmissionDecision retry_admission = decide_risk_surface_admission(
          retry_digest, quality_mode, candidate_generation_, prior, cfg_.fallback);
      if (!retry_admission.publish_candidate) continue;
      in = std::move(retry_inputs);
      sess = std::move(*retry);
      digest = retry_digest;
      admission = retry_admission;
      if (decision_.has_value()) {
        // Served provenance must name the admitted family: the policy curve
        // was rejected by independent admission and a fallback rung is being
        // published — the same record the construction-failure ladder keeps.
        // The first rejected primary of this fit stays authoritative if both
        // ladders fired.
        if (!decision_->used_fallback) {
          decision_->primary_curve = rejected_curve;
        }
        decision_->used_fallback = true;
        decision_->curve = in.curve;
      }
      if (selection_.has_value()) {
        // The selector's chosen candidate did not survive admission; re-stamp
        // the served choice so persisted provenance names the served model.
        selection_->chosen = in.curve;
      }
      break;
    }
  }
  risk_health_ = admission.health;
  if (!admission.publish_candidate) {
    (void)finalize_mark();
    timings_.total_ms = elapsed_ms(fit_start);
    if (risk_surface_ != nullptr && risk_health_.using_fallback()) {
      return Ok();
    }
    if (cfg_.fallback == SurfaceFallback::None) {
      risk_surface_.reset();
      served_decision_.reset();
      served_selection_.reset();
    }
    const SessionDiagnostics &session_diagnostics = sess.diagnostics();
    return Err(ErrorCode::Unavailable,
               "risk surface rejected: model=" + std::string(to_string(sess.inputs().curve.kind)) +
                   " mask=" +
                   std::to_string(static_cast<std::uint32_t>(digest.failures)) +
                   " butterfly=" + std::to_string(digest.n_butterfly_violations) +
                   " butterfly_slack=" + std::to_string(digest.max_butterfly_slack) +
                   " butterfly_k=" + std::to_string(digest.first_butterfly_k) +
                   " butterfly_slice=" + std::to_string(digest.first_butterfly_slice) +
                   " slopes=" + std::to_string(digest.first_butterfly_slope_left) + "/" +
                   std::to_string(digest.first_butterfly_slope_right) +
                   " calendar=" + std::to_string(digest.n_calendar_violations) +
                   " calendar_slack=" + std::to_string(digest.max_calendar_slack) +
                   " calendar_k=" + std::to_string(digest.first_calendar_k) +
                   " calendar_slice=" + std::to_string(digest.first_calendar_long_slice) +
                   " calendar_w=" + std::to_string(digest.first_calendar_previous_w) + "/" +
                   std::to_string(digest.first_calendar_current_w) +
                   " finite=" + std::to_string(digest.n_non_finite) +
                   " first_k=" + std::to_string(digest.first_non_finite_k) +
                   " first_slice=" + std::to_string(digest.first_non_finite_slice) +
                   " carry=" + (session_diagnostics.carry_confident ? "ok" : "failed") +
                   " inversion=" +
                   (session_diagnostics.inversion_certified ? "ok" : "failed"));
  }

  risk_surface_.reset(new FittedSurface(std::move(sess), SurfacePurpose::Risk, quality_mode,
                                        candidate_generation_));
  served_decision_ = decision_;
  served_selection_ = selection_;
  (void)finalize_mark();
  timings_.total_ms = elapsed_ms(fit_start);
  return Ok();
}

[[nodiscard]] RiskSurfaceValidationConfig
risk_validation_config(FitQualityMode quality_mode) noexcept {
  RiskSurfaceValidationConfig config;
  switch (quality_mode) {
  case FitQualityMode::Latency:
    config.k_min = -0.35;
    config.k_max = 0.35;
    config.strike_grid_points = 65;
    config.calendar_grid_points = 33;
    break;
  case FitQualityMode::Balanced:
    config.k_min = -0.50;
    config.k_max = 0.50;
    config.strike_grid_points = 97;
    config.calendar_grid_points = 65;
    break;
  case FitQualityMode::Accuracy:
    // Calendar projection is certified over the common production risk band.
    // Accuracy spends more samples/tighter fit work inside that contract; it
    // does not claim unprojected far-wing calendar safety.
    config.k_min = -0.60;
    config.k_max = 0.60;
    config.strike_grid_points = 257;
    config.calendar_grid_points = 129;
    break;
  }
  return config;
}

Result<FitDiag> PricerFitter::refit_risk_slice(const OptionChain &chain,
                                               std::size_t slice_idx) {
  using Clock = std::chrono::steady_clock;
  const auto incremental_start = Clock::now();
  const auto elapsed_ms = [](Clock::time_point start) {
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
  };
  timings_.incremental_input_ms = 0.0;
  timings_.incremental_refit_ms = 0.0;
  timings_.incremental_validation_ms = 0.0;
  timings_.incremental_publish_ms = 0.0;
  timings_.incremental_total_ms = 0.0;
  if (risk_surface_ == nullptr) {
    return Err(ErrorCode::Unavailable,
               "PricerFitter::refit_risk_slice: no admitted risk surface");
  }
  if (candidate_generation_ == std::numeric_limits<std::uint64_t>::max()) {
    return Err(ErrorCode::OutOfRange, "surface generation counter exhausted");
  }
  ++candidate_generation_;

  const FittedSurface &served = *risk_surface_;
  const VolaSession &live_session = served.session();
  const std::span<const SliceContext> contexts = live_session.expiries();
  if (slice_idx >= contexts.size()) {
    return Err(ErrorCode::InvalidArgument,
               "PricerFitter::refit_risk_slice: slice_idx out of range");
  }

  const FitQualityMode quality_mode = served.quality_mode();
  const std::uint64_t prior_generation = served.generation();
  const auto retain_prior = [&](ValidationFailure failure) {
    ValidationDigest digest;
    digest.failures = failure;
    const RiskSurfaceValidationConfig validation_config =
        risk_validation_config(quality_mode);
    finalize_validation_digest(digest, validation_config);
    risk_health_ = decide_risk_surface_admission(
                       digest, quality_mode, candidate_generation_, prior_generation,
                       SurfaceFallback::LastKnownGood)
                       .health;
  };

  const SliceContext &context = contexts[slice_idx];
  const Chain *updated_chain = nullptr;
  for (const Chain &candidate_chain : chain.underlying().chains) {
    if (std::fabs(candidate_chain.T - context.T) <=
        1.0e-10 * std::max(1.0, context.T)) {
      updated_chain = &candidate_chain;
      break;
    }
  }
  if (updated_chain == nullptr) {
    retain_prior(ValidationFailure::StaleInput);
    return Err(ErrorCode::NotFound,
               "PricerFitter::refit_risk_slice: fitted expiry is absent from updated chain");
  }

  const SessionInputs &inputs = live_session.inputs();
  const double rate = live_session.rate_at(context.T);
  Result<std::vector<FitObs>> observation_rows =
      live_session.cached_refit_observations(*updated_chain, slice_idx);
  if (!observation_rows.has_value()) {
    Result<ChainForward> carry = resolve_chain_forward(
        *updated_chain, inputs.S, rate, inputs.cash_divs, inputs.now_ts_ns,
        inputs.deam);
    if (!carry.has_value() || !carry->carry.confident) {
      retain_prior(ValidationFailure::InsufficientData);
      if (!carry.has_value()) {
        return Err(std::move(carry).error());
      }
      return Err(ErrorCode::Unavailable,
                 "PricerFitter::refit_risk_slice: updated carry is not certified");
    }

    // A local curve owns its original forward coordinate. Moving that
    // coordinate without rebuilding the whole term structure would silently
    // change every k, so promote the update to the cold fit path.
    const double forward_shift =
        std::fabs(std::log(carry->forward / context.forward));
    if (!std::isfinite(forward_shift) || forward_shift > 1.0e-8) {
      retain_prior(ValidationFailure::StaleInput);
      return Err(ErrorCode::Unavailable,
                 "PricerFitter::refit_risk_slice: carry moved; full surface fit required");
    }

    const double df = std::exp(-rate * context.T);
    const AmericanCorrectionCaches deam_caches =
        inputs.use_deam_cache_for_fit ? live_session.correction_caches()
                                     : AmericanCorrectionCaches{};
    Result<ObsSet> observations = build_observations_european(
        *updated_chain, inputs.S, rate, context.forward, context.T, df,
        inputs.calib, deam_caches, inputs.deam.al_opts, inputs.deam.iv_tol,
        inputs.deam.iv_max_iter, inputs.deam.method);
    if (!observations.has_value()) {
      retain_prior(ValidationFailure::InsufficientData);
      return Err(std::move(observations).error());
    }
    // Fail-closed for EVERY method: a non-AndersenLake method has no audit and
    // can never certify (deam_inversion_certified). Node drops within the
    // configured cap are tolerated; beyond it the refit is refused.
    if (!deam_inversion_certified(
            observations->deam_audit,
            inputs.calib.max_certified_deam_drop_fraction)) {
      retain_prior(ValidationFailure::InversionResidual);
      return Err(ErrorCode::Unavailable,
                 "PricerFitter::refit_risk_slice: price-to-IV inversion audit failed");
    }
    observation_rows = Ok(std::move(observations->obs));
  }
  timings_.incremental_input_ms = elapsed_ms(incremental_start);

  const auto refit_start = Clock::now();
  VolaSession candidate = live_session.clone();
  Result<FitDiag> refit = candidate.refit_slice(slice_idx, *observation_rows);
  timings_.incremental_refit_ms = elapsed_ms(refit_start);
  if (!refit.has_value()) {
    retain_prior(ValidationFailure::InsufficientData);
    return refit;
  }

  const RiskSurfaceValidationConfig validation_config =
      risk_validation_config(quality_mode);
  const auto validation_start = Clock::now();
  Result<ValidationDigest> checked = validate_risk_surface(candidate, validation_config);
  timings_.incremental_validation_ms = elapsed_ms(validation_start);
  if (!checked.has_value()) {
    retain_prior(ValidationFailure::InvalidDomain);
    return Err(std::move(checked).error());
  }
  ValidationDigest digest = *checked;
  // Review I-1: the geometric oracle knows nothing about carry coverage. Merge
  // the candidate's non-geometric failure context (identical seam to fit()) so
  // a still-gapped surface re-admits as Degraded+CarryGap, never as a clean
  // Healthy with the expiry silently missing (§5.2).
  merge_session_failure_context(candidate.diagnostics(), digest);
  finalize_validation_digest(digest, validation_config);
  const AdmissionDecision admission = decide_risk_surface_admission(
      digest, quality_mode, candidate_generation_, prior_generation,
      SurfaceFallback::LastKnownGood);
  risk_health_ = admission.health;
  if (!admission.publish_candidate) {
    return Err(ErrorCode::Unavailable,
               "PricerFitter::refit_risk_slice: candidate failed independent admission");
  }

  const auto publish_start = Clock::now();
  risk_surface_.reset(new FittedSurface(std::move(candidate), SurfacePurpose::Risk,
                                        quality_mode, candidate_generation_));
  timings_.incremental_publish_ms = elapsed_ms(publish_start);
  timings_.incremental_total_ms = elapsed_ms(incremental_start);
  return refit;
}

PricerFitter::EffectiveRequest PricerFitter::effective_request() const noexcept {
  // One-release compatibility seam (§9): legacy presets route through the
  // single map_legacy_fit_preset table while the v2 policy fields sit at their
  // defaults, so the preset->mode/purpose mapping has one source of truth. Hft
  // is a market-mark request, never an implicit risk request — for both the
  // implicit spelling (no pinned curve) and the documented explicit pin of the
  // legacy LinearVariance mark curve. Explicitly-configured requests
  // (non-default quality/outputs, or a pinned non-mark curve) pass through.
  const LegacyPresetMapping legacy = map_legacy_fit_preset(cfg_.preset);
  const bool default_request = cfg_.quality_mode == FitQualityMode::Balanced &&
                               cfg_.outputs == SurfaceOutputs::MarketMarkAndRisk;
  if (!default_request) {
    return EffectiveRequest{cfg_.outputs, cfg_.quality_mode};
  }
  if (legacy.purpose == SurfacePurpose::MarketMark) {
    if (!cfg_.curve.has_value() || cfg_.curve->kind == VolCurveKind::LinearVariance) {
      return EffectiveRequest{SurfaceOutputs::MarketMark, legacy.quality_mode};
    }
    // Hft with a pinned non-mark curve is an explicit dual request; the v2
    // policy fields (Balanced, MarkAndRisk) stand.
    return EffectiveRequest{cfg_.outputs, cfg_.quality_mode};
  }
  return EffectiveRequest{cfg_.outputs, legacy.quality_mode};
}

bool PricerFitter::fitted() const noexcept { return surface() != nullptr; }

const FittedSurface *PricerFitter::surface() const noexcept {
  // Fail-closed default-purpose serving (§3, §5.6): a config that requested a
  // Risk output is answered by the admitted risk surface or not at all — the
  // unconstrained LinearVariance market mark is never silently substituted.
  // Mark-only requests keep serving their market surface.
  if (has_output(effective_request().outputs, SurfacePurpose::Risk)) {
    return risk_surface_.get();
  }
  return market_mark_surface_.get();
}

Result<ChainValuation> PricerFitter::value_chain(const OptionChain &chain, OutputField fields,
                                                 unsigned n_threads) const {
  // Fail-closed purpose default: mirrors surface(). A caller that wants the
  // mark interpolant while risk is requested/unserved states so explicitly via
  // the SurfacePurpose::MarketMark overload.
  const SurfacePurpose purpose =
      has_output(effective_request().outputs, SurfacePurpose::Risk)
          ? SurfacePurpose::Risk
          : SurfacePurpose::MarketMark;
  return value_chain(chain, fields, purpose, n_threads);
}

Result<ChainValuation> PricerFitter::value_chain(const OptionChain &chain, OutputField fields,
                                                 SurfacePurpose purpose,
                                                 unsigned n_threads) const {
  const FittedSurface *served = purpose == SurfacePurpose::Risk ? risk_surface_.get()
                                                                : market_mark_surface_.get();
  if (served == nullptr) {
    if (purpose == SurfacePurpose::Risk) {
      // Name the risk rejection so a tick-loop caller cannot mistake this for
      // a transient "not fitted yet" and quietly re-route to the mark.
      return Err(ErrorCode::Unavailable,
                 "PricerFitter::value_chain: risk surface unserved (state=" +
                     std::string(to_string(risk_health_.state)) + " reasons=" +
                     std::to_string(static_cast<std::uint32_t>(risk_health_.reasons)) +
                     "); pass SurfacePurpose::MarketMark to price the mark explicitly");
    }
    return Err(ErrorCode::Unavailable,
               "PricerFitter::value_chain: requested surface purpose is unavailable");
  }
  const VolaSession &sess = served->session();
  const double S = chain.spot();
  const double nan = std::numeric_limits<double>::quiet_NaN();

  ChainValuation val;
  ChainSnapshot snap = chain.snapshot();
  val.ids = std::move(snap.ids);
  val.filled = fields;
  const std::size_t n = val.ids.size();

  if (has(fields, OutputField::ModelPrice)) {
    val.model_price.assign(n, nan);
  }
  if (has(fields, OutputField::ModelIV)) {
    val.model_iv.assign(n, nan);
  }
  if (has(fields, OutputField::BidIV)) {
    val.bid_iv.assign(n, nan);
  }
  if (has(fields, OutputField::AskIV)) {
    val.ask_iv.assign(n, nan);
  }
  if (has(fields, OutputField::MidIV)) {
    val.mid_iv.assign(n, nan);
  }
  if (has(fields, OutputField::Greeks)) {
    val.greeks.assign(n, AmericanGreeks{});
  }

  const unsigned nt = n_threads ? n_threads : cfg_.n_threads;
  const bool want_bands = has(fields, OutputField::BidIV) || has(fields, OutputField::AskIV) ||
                          has(fields, OutputField::MidIV);

  // The per-side correction caches the fit built. Routing the bid/ask/mid IV
  // inversions through them replaces the cold per-residual Andersen-Lake solve
  // (12 BAW root-finds + sweeps + quadrature + cold polish) with the cached hot
  // path (Black-76 + one Chebyshev evaluation) — the SOTA American-IV method (a
  // fast surrogate in the root-find, not a pricer). A null cache for a side
  // transparently falls back to the cold path (bit-identical, just slower).
  const AmericanCorrectionCaches caches = sess.correction_caches();

  const auto eval = [&](std::size_t i) {
    const double K = snap.strike[i];
    const double T = snap.T[i];
    const Side side = snap.side[i];
    if (!(K > 0.0) || !(T > 0.0)) {
      return; // decode failed or degenerate expiry — leave the row NaN
    }
    const double q = sess.q_eff_at(T);
    const double rate = sess.rate_at(T);
    if (has(fields, OutputField::ModelIV)) {
      val.model_iv[i] = sess.iv(K, T);
    }
    if (has(fields, OutputField::ModelPrice)) {
      const auto fv = sess.fair_value(K, T, side);
      val.model_price[i] = fv.has_value() ? *fv : nan;
    }
    if (has(fields, OutputField::Greeks)) {
      const auto g = sess.greeks(K, T, side);
      if (g.has_value()) {
        val.greeks[i] = *g;
      } else {
        val.greeks[i].price = nan;
      }
    }
    if (!want_bands) {
      return;
    }
    // Parallel American-IV band inversions through the cached hot path. The
    // surface's own IV at (K, T) seeds all three (bid/ask/mid vols sit within a
    // spread's width of it), so each is 1-2 Newton steps.
    const CorrectionCache *cc = caches.for_side(side);
    const double miv = sess.iv(K, T);
    const double ws = (std::isfinite(miv) && miv > 0.0) ? miv : 0.0;
    const auto invert = [&](double px) {
      return american_implied_vol(px, S, K, T, rate, q, side, AmericanMethod::AndersenLake, 1.0e-7,
                                  64, std::nullopt, cc, ws);
    };
    if (has(fields, OutputField::BidIV) && snap.bid[i] > 0.0) {
      const auto iv = invert(snap.bid[i]);
      val.bid_iv[i] = iv.has_value() ? *iv : nan;
    }
    if (has(fields, OutputField::AskIV) && snap.ask[i] > 0.0) {
      const auto iv = invert(snap.ask[i]);
      val.ask_iv[i] = iv.has_value() ? *iv : nan;
    }
    if (has(fields, OutputField::MidIV) && snap.mid[i] > 0.0) {
      const auto iv = invert(snap.mid[i]);
      val.mid_iv[i] = iv.has_value() ? *iv : nan;
    }
  };

  parallel_for(n, nt, eval);
  return Ok(std::move(val));
}

} // namespace atx::vol
