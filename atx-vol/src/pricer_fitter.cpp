#include "atx/vol/pricer_fitter.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <span>
#include <utility>
#include <vector>

#include "atx/core/error.hpp"
#include "atx/vol/american_iv.hpp"  // american_implied_vol
#include "atx/vol/black76.hpp"      // black76_price (independent publication oracle)
#include "atx/vol/correction.hpp"   // AmericanCorrectionCaches (cached inversion hot path)
#include "atx/vol/parallel_for.hpp" // parallel_for (shared block-partition fan-out)

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
  static constexpr VolCurveKind kFromSvi[]{VolCurveKind::LinearVariance};
  static constexpr VolCurveKind kFromConvex[]{VolCurveKind::LinearVariance};
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

std::optional<std::size_t> ChainValuation::row_of(OptionId id) const {
  for (std::size_t i = 0; i < ids.size(); ++i) {
    if (ids[i] == id) {
      return i;
    }
  }
  return std::nullopt;
}

namespace {

[[nodiscard]] SurfaceBuildAttemptReport
failed_attempt_report(const Underlying &under, const CurveConfig &curve,
                      const atx::core::Error &failure,
                      SurfaceBuildStage stage = SurfaceBuildStage::Build) {
  SurfaceBuildAttemptReport attempt;
  attempt.curve = curve;
  attempt.stage = stage;
  attempt.failure = failure;
  attempt.admission.primary_reason = SurfaceAdmissionReason::BuildFailed;
  attempt.admission.failed_checks =
      surface_admission_reason_mask(SurfaceAdmissionReason::BuildFailed);
  for (std::size_t i = 0u; i < under.chains.size(); ++i) {
    const Chain &chain = under.chains[i];
    if (std::isfinite(chain.T) && chain.T > 0.0) {
      attempt.evidence.attempted_quotes += chain.n_strikes();
      attempt.expiries.push_back(ExpiryBuildReport{i, chain.T, ExpiryBuildOutcome::Missing, 0u});
    }
  }
  attempt.evidence.attempted_expiries = attempt.expiries.size();
  return attempt;
}

void evaluate_independent_invariants(const VolaSession &session,
                                     SurfaceAdmissionEvidence &evidence) {
  constexpr std::size_t kGrid = 97u;
  constexpr double kMin = -0.60;
  constexpr double kMax = 0.60;
  const std::span<const SliceContext> expiries = session.expiries();
  const auto record_failure = [&evidence](SurfaceAdmissionReason reason, double T, double k,
                                          double value) {
    if (evidence.first_invariant_failure == SurfaceAdmissionReason::None) {
      evidence.first_invariant_failure = reason;
      evidence.first_failure_maturity = T;
      evidence.first_failure_log_moneyness = k;
      evidence.first_failure_value = value;
    }
  };
  if (expiries.empty()) {
    record_failure(SurfaceAdmissionReason::FiniteIvDomain, 0.0, 0.0, 0.0);
    return;
  }

  std::vector<bool> common_finite(kGrid, true);
  for (const SliceContext &context : expiries) {
    const double T = context.T;
    const double F = context.forward;
    evidence.has_short_tenor = evidence.has_short_tenor || T <= 0.125;
    evidence.has_medium_tenor = evidence.has_medium_tenor || (T > 0.125 && T < 0.50);
    evidence.has_long_tenor = evidence.has_long_tenor || T >= 0.50;
    if (!(T > 0.0) || !(F > 0.0) || !std::isfinite(T) || !std::isfinite(F)) {
      record_failure(SurfaceAdmissionReason::FiniteIvDomain, T, 0.0, F);
      return;
    }
    for (std::size_t grid_index = 0u; grid_index < kGrid; ++grid_index) {
      const double alpha = static_cast<double>(grid_index) / static_cast<double>(kGrid - 1u);
      const double k = kMin + (kMax - kMin) * alpha;
      const double K = F * std::exp(k);
      const double iv = session.iv(K, T);
      common_finite[grid_index] = common_finite[grid_index] && K > 0.0 && std::isfinite(K) &&
                                  iv > 0.0 && std::isfinite(iv) && std::isfinite(iv * iv * T);
    }
  }

  constexpr std::size_t kAtm = (kGrid - 1u) / 2u;
  if (!common_finite[kAtm]) {
    record_failure(SurfaceAdmissionReason::FiniteIvDomain, expiries.front().T, 0.0,
                   session.iv(expiries.front().forward, expiries.front().T));
    return;
  }
  std::size_t first_grid = kAtm;
  while (first_grid > 0u && common_finite[first_grid - 1u]) {
    --first_grid;
  }
  std::size_t last_grid = kAtm;
  while (last_grid + 1u < kGrid && common_finite[last_grid + 1u]) {
    ++last_grid;
  }
  evidence.invariant_grid_points = last_grid - first_grid + 1u;
  evidence.invariant_grid_k_min =
      kMin + (kMax - kMin) * static_cast<double>(first_grid) / static_cast<double>(kGrid - 1u);
  evidence.invariant_grid_k_max =
      kMin + (kMax - kMin) * static_cast<double>(last_grid) / static_cast<double>(kGrid - 1u);
  if (evidence.invariant_grid_points < 5u ||
      evidence.invariant_grid_k_max - evidence.invariant_grid_k_min < 0.05) {
    record_failure(SurfaceAdmissionReason::FiniteIvDomain, expiries.front().T, 0.0,
                   static_cast<double>(evidence.invariant_grid_points));
    return;
  }

  evidence.finite_iv_domain = true;
  evidence.european_price_bounds = true;
  evidence.strike_monotone = true;
  evidence.strike_convex = true;
  evidence.calendar_total_variance = true;
  evidence.forward_variance_nonnegative = true;
  std::vector<double> previous_w(kGrid, 0.0);
  double previous_T = 0.0;
  for (std::size_t expiry_index = 0u; expiry_index < expiries.size(); ++expiry_index) {
    const SliceContext &context = expiries[expiry_index];
    const double T = context.T;
    const double F = context.forward;
    const double rate = session.rate_at(T);
    const double df = std::exp(-rate * T);
    if (!std::isfinite(rate) || !(df > 0.0) || !std::isfinite(df)) {
      evidence.finite_iv_domain = false;
      evidence.european_price_bounds = false;
      evidence.strike_monotone = false;
      evidence.strike_convex = false;
      evidence.calendar_total_variance = false;
      evidence.forward_variance_nonnegative = false;
      record_failure(SurfaceAdmissionReason::FiniteIvDomain, T, 0.0, rate);
      return;
    }

    double previous_strike = 0.0;
    double previous_price = 0.0;
    double previous_slope = 0.0;
    bool have_previous = false;
    bool have_slope = false;
    for (std::size_t grid_index = first_grid; grid_index <= last_grid; ++grid_index) {
      const double alpha = static_cast<double>(grid_index) / static_cast<double>(kGrid - 1u);
      const double k = kMin + (kMax - kMin) * alpha;
      const double K = F * std::exp(k);
      const double iv = session.iv(K, T);
      const double w = iv * iv * T;
      if (!(K > 0.0) || !std::isfinite(K) || !(iv > 0.0) || !std::isfinite(iv) ||
          !std::isfinite(w)) {
        evidence.finite_iv_domain = false;
        record_failure(SurfaceAdmissionReason::FiniteIvDomain, T, k, iv);
        continue;
      }

      const double call = black76_price(F, K, T, iv, df, Side::Call);
      const double lower = df * std::max(F - K, 0.0);
      const double upper = df * F;
      const double price_tolerance = 1.0e-10 * std::max({1.0, std::abs(lower), std::abs(upper)});
      if (!std::isfinite(call) || call < lower - price_tolerance ||
          call > upper + price_tolerance) {
        evidence.european_price_bounds = false;
        record_failure(SurfaceAdmissionReason::EuropeanPriceBounds, T, k, call);
      }
      if (have_previous) {
        const double slope = (call - previous_price) / (K - previous_strike);
        constexpr double slope_tolerance = 1.0e-10;
        if (!std::isfinite(slope) || slope > slope_tolerance) {
          evidence.strike_monotone = false;
          record_failure(SurfaceAdmissionReason::StrikeMonotonicity, T, k, slope);
        }
        if (have_slope && slope + slope_tolerance < previous_slope) {
          evidence.strike_convex = false;
          record_failure(SurfaceAdmissionReason::StrikeConvexity, T, k, slope - previous_slope);
        }
        previous_slope = slope;
        have_slope = true;
      }
      previous_strike = K;
      previous_price = call;
      have_previous = true;

      if (expiry_index > 0u) {
        const double variance_tolerance =
            1.0e-10 * std::max({1.0, std::abs(previous_w[grid_index]), std::abs(w)});
        if (w + variance_tolerance < previous_w[grid_index]) {
          evidence.calendar_total_variance = false;
          record_failure(SurfaceAdmissionReason::CalendarTotalVariance, T, k,
                         w - previous_w[grid_index]);
        }
        const double forward_variance = (w - previous_w[grid_index]) / (T - previous_T);
        if (!std::isfinite(forward_variance) || forward_variance < -variance_tolerance) {
          evidence.forward_variance_nonnegative = false;
          record_failure(SurfaceAdmissionReason::ForwardVariance, T, k, forward_variance);
        }
      }
      previous_w[grid_index] = w;
    }
    previous_T = T;
  }
}

[[nodiscard]] std::optional<SurfaceBuildAttemptReport>
duplicate_maturity_report(const Underlying &under, const CurveConfig &curve) {
  std::vector<std::pair<double, std::size_t>> maturities;
  maturities.reserve(under.chains.size());
  for (std::size_t i = 0u; i < under.chains.size(); ++i) {
    const double T = under.chains[i].T;
    if (std::isfinite(T) && T > 0.0) {
      maturities.emplace_back(T, i);
    }
  }
  std::sort(maturities.begin(), maturities.end());
  std::vector<bool> duplicate(under.chains.size(), false);
  bool found = false;
  for (std::size_t i = 1u; i < maturities.size(); ++i) {
    if (maturities[i - 1u].first == maturities[i].first) {
      duplicate[maturities[i - 1u].second] = true;
      duplicate[maturities[i].second] = true;
      found = true;
    }
  }
  if (!found) {
    return std::nullopt;
  }

  SurfaceBuildAttemptReport attempt;
  attempt.curve = curve;
  attempt.stage = SurfaceBuildStage::InputValidation;
  attempt.failure =
      atx::core::Error{ErrorCode::InvalidArgument, "PricerFitter::fit: duplicate valid maturities"};
  attempt.evidence.duplicate_maturities = true;
  attempt.admission.primary_reason = SurfaceAdmissionReason::DuplicateMaturity;
  attempt.admission.failed_checks =
      surface_admission_reason_mask(SurfaceAdmissionReason::DuplicateMaturity);
  for (const auto &[T, index] : maturities) {
    ++attempt.evidence.attempted_expiries;
    attempt.evidence.attempted_quotes += under.chains[index].n_strikes();
    attempt.expiries.push_back(ExpiryBuildReport{
        index, T,
        duplicate[index] ? ExpiryBuildOutcome::DuplicateMaturity : ExpiryBuildOutcome::Missing,
        0u});
  }
  return attempt;
}

[[nodiscard]] SurfaceBuildAttemptReport completed_attempt_report(const Underlying &under,
                                                                 const CurveConfig &curve,
                                                                 const VolaSession &session,
                                                                 const FitAdmissionPolicy &policy) {
  SurfaceBuildAttemptReport attempt;
  attempt.curve = curve;
  attempt.build_succeeded = true;
  attempt.stage = SurfaceBuildStage::Admission;
  const SessionDiagnostics &diagnostics = session.diagnostics();
  attempt.evidence.calendar_arb_free = diagnostics.calendar_arb_free;
  attempt.evidence.worst_frac_within_bidask = diagnostics.worst_frac_within_bidask;
  attempt.evidence.finite_diagnostics = std::isfinite(diagnostics.worst_frac_within_bidask) &&
                                        std::isfinite(diagnostics.mean_frac_within_bidask) &&
                                        std::isfinite(diagnostics.mean_chi2_reduced) &&
                                        std::isfinite(diagnostics.mean_rmse_vol);

  const std::span<const SliceContext> fitted = session.expiries();
  std::vector<bool> consumed(fitted.size(), false);
  std::size_t consecutive_gaps = 0u;
  for (std::size_t i = 0u; i < under.chains.size(); ++i) {
    const Chain &chain = under.chains[i];
    if (!std::isfinite(chain.T) || !(chain.T > 0.0)) {
      continue;
    }
    ++attempt.evidence.attempted_expiries;
    attempt.evidence.attempted_quotes += chain.n_strikes();
    const auto context = std::find_if(fitted.begin(), fitted.end(), [&](const SliceContext &slice) {
      const std::size_t index = static_cast<std::size_t>(&slice - fitted.data());
      return !consumed[index] && slice.T == chain.T;
    });
    if (context == fitted.end()) {
      ++consecutive_gaps;
      attempt.evidence.max_consecutive_expiry_gaps =
          std::max(attempt.evidence.max_consecutive_expiry_gaps, consecutive_gaps);
      attempt.expiries.push_back(ExpiryBuildReport{i, chain.T, ExpiryBuildOutcome::Missing, 0u});
      continue;
    }
    if (attempt.expiries.empty()) {
      attempt.evidence.front_expiry_fitted = true;
    }
    consecutive_gaps = 0u;
    const std::size_t context_index = static_cast<std::size_t>(context - fitted.begin());
    consumed[context_index] = true;
    ++attempt.evidence.fitted_expiries;
    attempt.evidence.fitted_quotes += context->n_used;
    attempt.expiries.push_back(
        ExpiryBuildReport{i, chain.T, ExpiryBuildOutcome::Fitted, context->n_used});
  }
  evaluate_independent_invariants(session, attempt.evidence);
  attempt.admission = evaluate_surface_admission(attempt.evidence, policy);
  return attempt;
}

} // namespace

Status PricerFitter::fit(const OptionChain &chain,
                         const std::function<void(SessionInputs &)> &session_overlay) {
  std::optional<SelectorResult> next_selection;
  std::optional<FitDecision> next_decision;

  const CurveConfig validation_curve = cfg_.curve.value_or(CurveConfig{});
  if (std::optional<SurfaceBuildAttemptReport> duplicate =
          duplicate_maturity_report(chain.underlying(), validation_curve);
      duplicate.has_value()) {
    const atx::core::Error failure = *duplicate->failure;
    SurfaceBuildReport report;
    report.primary_curve = validation_curve;
    report.retained_last_known_good = surface_ != nullptr;
    report.attempts.push_back(std::move(*duplicate));
    last_attempt_report_ = std::move(report);
    return Err(failure);
  }

  FitPreset effective_preset = cfg_.preset;
  const bool pinned_hft = !cfg_.curve.has_value() && cfg_.preset == FitPreset::Hft;
  if (!cfg_.curve.has_value() && !pinned_hft) {
    FitDecision d =
        select_fit_policy(chain.underlying(), chain.underlying().ticker, cfg_.context, cfg_.policy);
    effective_preset = d.needs_cross_validation ? cfg_.preset : d.preset;
    next_decision = std::move(d);
  }

  SessionInputs in =
      make_session_inputs(effective_preset, chain.spot(), chain.rate(), chain.now_ns());
  if (chain.env().yield.size() > 0u) {
    in.expiry_rate_T.reserve(chain.underlying().chains.size());
    in.expiry_rates.reserve(chain.underlying().chains.size());
    for (const Chain &expiry : chain.underlying().chains) {
      in.expiry_rate_T.push_back(expiry.T);
      in.expiry_rates.push_back(chain.env().rate_at(expiry.T));
    }
  }
  // Apply the selected profile's existing quote/calibration policy through the
  // same SessionInputs consumed by every curve family. Reapply the preset after
  // copying the profile so latency/fidelity controls (HFT knot cap, shortcut,
  // de-Am options) remain authoritative.
  if (next_decision.has_value() && effective_preset != FitPreset::Hft) {
    const auto profile = profile_lookup(next_decision->profile.kind);
    if (profile.has_value()) {
      in.calib = profile.value()->calib;
      apply_fit_preset(in, effective_preset);
    }
  }
  // Dividends: the chain's MarketEnv supplies the schedule; a non-empty config
  // value overrides it.
  in.cash_divs = cfg_.cash_divs.empty() ? chain.env().cash_divs : cfg_.cash_divs;
  if (cfg_.use_correction_cache.has_value()) {
    in.use_correction_cache = *cfg_.use_correction_cache;
  }
  if (cfg_.score_parity.has_value()) {
    in.score_parity = *cfg_.score_parity;
  }
  if (cfg_.enforce_calendar_floor.has_value()) {
    in.enforce_calendar_floor = *cfg_.enforce_calendar_floor;
  }
  if (cfg_.use_deam_cache_for_fit.has_value()) {
    in.use_deam_cache_for_fit = *cfg_.use_deam_cache_for_fit;
  }
  if (cfg_.max_obs_per_slice.has_value()) {
    in.calib.max_obs_per_slice = *cfg_.max_obs_per_slice;
  }
  if (cfg_.max_otm_shortcut_premium_spread_frac.has_value()) {
    in.calib.max_otm_shortcut_premium_spread_frac = *cfg_.max_otm_shortcut_premium_spread_frac;
  }
  if (!in.expiry_rates.empty()) {
    // CorrectionCache is built at one scalar (r, q) pair. A term-rate board
    // must stay on the cold pricer until the cache itself becomes term-aware.
    in.use_correction_cache = false;
    in.use_deam_cache_for_fit = false;
  }

  // Curve config: pinned, profile-direct, or held-out selected for this board.
  if (cfg_.curve.has_value()) {
    in.curve = *cfg_.curve;
  } else if (pinned_hft) {
    // Hft's preset-pinned direct market curve avoids both selector candidate
    // fits and the per-expiry dense QP on penny-dense index boards.
    in.curve.kind = VolCurveKind::LinearVariance;
  } else if (next_decision.has_value() && !next_decision->needs_cross_validation) {
    // The adaptive knot budget is a policy default, so an explicit
    // cfg_.max_obs_per_slice (already applied above) must win -- its documented
    // contract is "nullopt => use the preset default". Apply the cap before
    // mirroring calib into the curve so the two never disagree.
    if (next_decision->curve.kind == VolCurveKind::LinearVariance &&
        !cfg_.max_obs_per_slice.has_value() && cfg_.policy.dense_node_cap > 0) {
      in.calib.max_obs_per_slice = cfg_.policy.dense_node_cap;
    }
    next_decision->curve.parametric = in.calib;
    in.curve = next_decision->curve;
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
    if (!selected.has_value()) {
      SurfaceBuildReport report;
      report.primary_curve = in.curve;
      report.retained_last_known_good = surface_ != nullptr;
      report.attempts.push_back(failed_attempt_report(
          chain.underlying(), in.curve, selected.error(), SurfaceBuildStage::Selection));
      last_attempt_report_ = std::move(report);
      return Err(std::move(selected).error());
    }
    SelectorResult chosen = std::move(*selected);
    // Parametric candidates inherit the selected profile's calibration policy;
    // the held-out selector chooses family/curve-local knobs, not a second quote
    // filtering policy.
    chosen.chosen.parametric = in.calib;
    in.curve = chosen.chosen;
    if (next_decision.has_value()) {
      next_decision->curve = chosen.chosen;
      next_decision->preset = effective_preset;
    }
    next_selection = std::move(chosen);
  }

  if (session_overlay) {
    session_overlay(in);
  }
  if (next_decision.has_value()) {
    next_decision->primary_curve = in.curve;
    next_decision->curve = in.curve;
    next_decision->used_fallback = false;
  }

  const Underlying &under = chain.underlying();
  const CurveConfig primary_curve = in.curve;
  SurfaceBuildReport report;
  report.primary_curve = primary_curve;
  std::optional<VolaSession> admitted_session;
  std::optional<atx::core::Error> primary_failure;

  Result<VolaSession> built = VolaSession::build(under, in);
  if (!built.has_value()) {
    primary_failure = built.error();
    report.attempts.push_back(failed_attempt_report(under, in.curve, built.error()));
  } else {
    SurfaceBuildAttemptReport attempt =
        completed_attempt_report(under, in.curve, *built, cfg_.admission);
    const bool admitted = attempt.admission.admitted;
    report.attempts.push_back(std::move(attempt));
    if (admitted) {
      admitted_session.emplace(std::move(*built));
    }
  }
  // A profile is a fast prior, not permission to drop an underlier: walk the
  // fallback ladder for anything the policy routed, including a board whose curve
  // came from the held-out selector. A curve the CALLER pinned (cfg_.curve, or the
  // preset-pinned Hft dense route) is an explicit instruction and is never
  // silently substituted.
  const bool auto_routed = next_decision.has_value() && !cfg_.curve.has_value() && !pinned_hft;
  if (!admitted_session.has_value() && auto_routed) {
    for (const VolCurveKind rung : fallback_curve_rungs(primary_curve.kind)) {
      in.curve = primary_curve;
      in.curve.kind = rung;
      Result<VolaSession> retry = VolaSession::build(under, in);
      if (!retry.has_value()) {
        report.attempts.push_back(failed_attempt_report(under, in.curve, retry.error()));
        continue;
      }
      SurfaceBuildAttemptReport attempt =
          completed_attempt_report(under, in.curve, *retry, cfg_.admission);
      const bool admitted = attempt.admission.admitted;
      report.attempts.push_back(std::move(attempt));
      if (!admitted) {
        continue;
      }
      admitted_session.emplace(std::move(*retry));
      report.used_fallback = true;
      next_decision->primary_curve = primary_curve;
      next_decision->curve = in.curve;
      next_decision->used_fallback = true;
      break;
    }
  }
  if (!admitted_session.has_value()) {
    report.retained_last_known_good = surface_ != nullptr;
    last_attempt_report_ = std::move(report);
    if (primary_failure.has_value()) {
      return Err(std::move(*primary_failure));
    }
    return Err(ErrorCode::Unavailable, "PricerFitter::fit: every built surface failed admission");
  }
  report.published = true;
  report.published_curve = in.curve;
  report.attempts.back().stage = SurfaceBuildStage::Publication;
  VolaSession sess = std::move(*admitted_session);
  // FittedSurface's ctor is private (friend PricerFitter), so make_unique cannot
  // reach it — construct explicitly.
  std::unique_ptr<FittedSurface> next_surface(new FittedSurface(std::move(sess)));
  std::optional<SurfaceBuildReport> next_published{report};
  std::optional<SurfaceBuildReport> next_attempt{std::move(report)};

  // Transaction boundary: admitted state and its provenance become current
  // together. Every earlier failure leaves the last-known-good publication.
  surface_ = std::move(next_surface);
  selection_ = std::move(next_selection);
  decision_ = std::move(next_decision);
  published_report_ = std::move(next_published);
  last_attempt_report_ = std::move(next_attempt);
  return Ok();
}

Result<ChainValuation> PricerFitter::value_chain(const OptionChain &chain, OutputField fields,
                                                 unsigned n_threads) const {
  if (surface_ == nullptr) {
    return Err(ErrorCode::Unavailable,
               "PricerFitter::value_chain: no fitted surface; call fit() first");
  }
  const VolaSession &sess = surface_->session();
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
