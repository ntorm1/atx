#include "atx/vol/pricer_fitter.hpp"

#include <algorithm>
#include <cassert>
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
#include "atx/vol/american_iv.hpp"      // american_implied_vol
#include "atx/vol/black76.hpp"          // black76_price (independent publication oracle)
#include "atx/vol/calib.hpp"            // build_observations_european
#include "atx/vol/correction.hpp"       // AmericanCorrectionCaches (cached inversion hot path)
#include "atx/vol/deamer.hpp"           // resolve_chain_forward
#include "atx/vol/detail/convex_recovery.hpp"  // strict-recovery bridge (admission-rejection rung)
#include "atx/vol/detail/counters.hpp"         // ATX_VOL_COUNT (RiskStrictRecoveryRounds/Admitted)
#include "atx/vol/detail/parallel_for.hpp"     // atx_auto_worker_count
#include "atx/vol/detail/prepared_fitting.hpp" // prepare_expiry (canonical refit preparation)
#include "atx/vol/detail/pricing_executor.hpp" // persistent whole-chain task fan-out
#include "atx/vol/detail/risk_surface_validation.hpp"

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
  // Svi descends to the direct-variance rung like every other family — the
  // ladder's own contract above. It was the one list without it, which was
  // invisible while the calendar level-projection silently "repaired" any SVI
  // candidate into admissibility; with that fabrication refused (fidelity
  // budget, arb.hpp), a board whose SVI and eSSVI candidates both carry
  // in-band calendar crossings must reach the dense model to serve at all.
  static constexpr VolCurveKind kFromSvi[]{VolCurveKind::Essvi, VolCurveKind::LinearVariance};
  static constexpr VolCurveKind kFromConvex[]{VolCurveKind::Svi, VolCurveKind::Essvi};
  static constexpr VolCurveKind kFromLinear[]{VolCurveKind::Essvi};
  // SplineVol is not in default_selector_candidates() v1 (task-3 constraint),
  // so this rung is not exercised by the auto-routed path today; it still
  // needs a progressing ladder for any caller that pins SplineVol explicitly.
  static constexpr VolCurveKind kFromSpline[]{VolCurveKind::LinearVariance};
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
  case VolCurveKind::SplineVol:
    return kFromSpline;
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
  // Decision B: a term-structure-fallback carry is honestly NOT confident, but a
  // board whose only carry shortfall is fallback-admitted expiries is still
  // ADMISSIBLE — it publishes Degraded via CarryGap below, NOT a hard reject.
  // InsufficientData is reserved for a genuine carry deficiency: a board that is
  // not confident AND has no fallback carry to account for the shortfall (e.g. a
  // committed slice with no usable carry at all). A fully-confident board keeps
  // its exact prior behaviour (n_carry_fallback_expiries == 0).
  if (!diagnostics.carry_confident && diagnostics.n_carry_fallback_expiries == 0) {
    digest.failures |= ValidationFailure::InsufficientData;
  }
  if (!diagnostics.inversion_certified) {
    digest.failures |= ValidationFailure::InversionResidual;
  }
  if (diagnostics.n_carry_skipped_expiries > 0 || diagnostics.n_audit_starved_expiries > 0 ||
      diagnostics.n_carry_fallback_expiries > 0) {
    // §5.2 + Decision B: expiries dropped by the carry gate, starved by the fit
    // audit, OR admitted with a term-structure-fallback carry all surface as the
    // one publish-with-Degraded reason. CarryGap (decide_risk_surface_admission);
    // combined with any other failure it still rejects.
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
    const std::uint64_t merged = static_cast<std::uint64_t>(digest.n_price_bound_violations) +
                                 static_cast<std::uint64_t>(diagnostics.n_price_bound_violations);
    digest.n_price_bound_violations = static_cast<std::uint32_t>(
        std::min<std::uint64_t>(merged, std::numeric_limits<std::uint32_t>::max()));
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

namespace detail {

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
  attempt.evidence.parity_state = diagnostics.parity_state;
  attempt.evidence.calendar_arb_free = diagnostics.calendar_arb_free;
  attempt.evidence.worst_frac_within_bidask = diagnostics.worst_frac_within_bidask;
  attempt.evidence.finite_diagnostics = diagnostics.parity_state == ParityDiagnosticState::Valid &&
                                        std::isfinite(diagnostics.worst_frac_within_bidask) &&
                                        std::isfinite(diagnostics.mean_frac_within_bidask) &&
                                        std::isfinite(diagnostics.mean_chi2_reduced) &&
                                        std::isfinite(diagnostics.mean_rmse_vol);

  const std::span<const SliceContext> fitted = session.expiries();
  // Per-expiry re-Americanized parity, parallel to `fitted`: n = admitted
  // (two-sided, de-Am-survived) quotes SCORED for this slice, n_within = those
  // the built surface reprices inside [bid, ask]. This IS the serve-check that
  // produced worst_frac_within_bidask; the F2 quote-coverage numerator/denominator
  // are its board-level count.
  const std::span<const ParityReport> parity = session.parity();
  std::vector<bool> consumed(fitted.size(), false);
  std::size_t consecutive_gaps = 0u;
  for (std::size_t i = 0u; i < under.chains.size(); ++i) {
    const Chain &chain = under.chains[i];
    if (!std::isfinite(chain.T) || !(chain.T > 0.0)) {
      continue;
    }
    ++attempt.evidence.attempted_expiries;
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
    // F2 SERVED-quote coverage (fit_policy.hpp InsufficientQuoteCoverage): the
    // floor's intent is the fraction of admitted-universe quotes the BUILT surface
    // reprices in-band — NOT the node-capped fit-observation count, which on a
    // wide board a dense fit caps far below the strikes it actually serves. Count
    // this expiry's scored quotes (attempted_quotes) and those the surface serves
    // in-band (fitted_quotes) from its parity report. One metric, every family: an
    // eSSVI slice's n_within is scored the same way. When parity was not scored
    // (score_parity off — only ever with the serve floor disabled), fall back to
    // the fit-observation count on both sides (an unmeasured serve cannot gate).
    if (context_index < parity.size() && parity[context_index].n > 0u) {
      attempt.evidence.attempted_quotes += parity[context_index].n;
      attempt.evidence.fitted_quotes += parity[context_index].n_within;
    } else {
      attempt.evidence.attempted_quotes += context->n_used;
      attempt.evidence.fitted_quotes += context->n_used;
    }
    attempt.expiries.push_back(
        ExpiryBuildReport{i, chain.T, ExpiryBuildOutcome::Fitted, context->n_used});
  }
  // Fail loud in debug if the served/attempted invariant is ever violated: served
  // (in-band) quotes must not exceed the admitted quotes scored. In release this
  // is the ImpossibleEvidence admission check's job (fail safe).
  assert(attempt.evidence.fitted_quotes <= attempt.evidence.attempted_quotes &&
         "completed_attempt_report: served (in-band) quotes exceeded admitted scored quotes");
  evaluate_independent_invariants(session, attempt.evidence);
  attempt.admission = evaluate_surface_admission(attempt.evidence, policy);
  return attempt;
}

[[nodiscard]] SurfaceParityInputs refit_preparation_inputs(const VolaSession &session) {
  const SessionInputs &stored = session.inputs();
  SurfaceParityInputs inputs;
  inputs.S = stored.S;
  inputs.r = stored.r;
  inputs.expiry_rate_T = stored.expiry_rate_T;
  inputs.expiry_rates = stored.expiry_rates;
  inputs.cash_divs = stored.cash_divs;
  inputs.now_ts_ns = stored.now_ts_ns;
  inputs.deam = stored.deam;
  inputs.deam.caches = session.correction_caches();
  inputs.calib = stored.calib;
  inputs.band_k = stored.band_k;
  inputs.repair = stored.calendar_repair;
  // The legacy eSSVI cold driver always scores parity, even when the generic
  // score_parity optimization is disabled.
  inputs.score_parity = true;
  inputs.enforce_calendar_floor = stored.enforce_calendar_floor;
  inputs.use_deam_cache_for_fit = stored.use_deam_cache_for_fit;
  return inputs;
}

} // namespace detail

using detail::completed_attempt_report;
using detail::duplicate_maturity_report;
using detail::failed_attempt_report;
using detail::refit_preparation_inputs;

Status PricerFitter::fit(const OptionChain &chain,
                         const std::function<void(SessionInputs &)> &session_overlay) {
  using Clock = std::chrono::steady_clock;
  struct MarkBuildResult {
    Result<VolaSession> built;
    double elapsed_ms{};
  };
  const auto fit_start = Clock::now();
  const auto elapsed_ms = [](Clock::time_point start) noexcept {
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
  };

  // Fail-closed input validation BEFORE any served state mutates: a board with
  // duplicate maturities cannot produce a coherent term structure for either
  // purpose. The failure is recorded transactionally (last_attempt_report_
  // only); every published artifact is left untouched.
  const CurveConfig validation_curve = cfg_.curve.value_or(CurveConfig{});
  if (std::optional<SurfaceBuildAttemptReport> duplicate =
          duplicate_maturity_report(chain.underlying(), validation_curve);
      duplicate.has_value()) {
    const atx::core::Error failure = *duplicate->failure;
    SurfaceBuildReport report;
    report.primary_curve = validation_curve;
    report.retained_last_known_good = surface() != nullptr;
    report.attempts.push_back(std::move(*duplicate));
    last_attempt_report_ = std::move(report);
    return Err(failure);
  }

  // Product intent alone selects legacy versus v2 routing. A per-symbol session
  // overlay layers numerical inputs inside the selected branch; it must never
  // demote an explicit v2 Risk/Mark request to the legacy single-surface path.
  if (!is_v2_request()) {
    // ── Legacy / main single-surface transactional fit (default v2 fields) ──
    // The caller did not opt into the v2 dual mark/risk API, so serve ONE
    // surface admitted by FitAdmissionPolicy (mark consumer by default; the
    // strict risk consumer via risk_admission_policy()). No forced Project
    // repair -- the preset owns calendar policy -- so refit_expiry can consume
    // it. Published into the market_mark slot; effective_request() resolves
    // reads there for a legacy request.
    timings_ = {};
    if (candidate_generation_ == std::numeric_limits<std::uint64_t>::max()) {
      return Err(ErrorCode::OutOfRange, "surface generation counter exhausted");
    }
    std::optional<SelectorResult> next_selection;
    std::optional<FitDecision> next_decision;
    FitPreset effective_preset = cfg_.preset;
    const bool pinned_hft = !cfg_.curve.has_value() && cfg_.preset == FitPreset::Hft;
    if (!cfg_.curve.has_value() && !pinned_hft) {
      FitDecision d = select_fit_policy(chain.underlying(), chain.underlying().ticker, cfg_.context,
                                        cfg_.policy);
      effective_preset = d.needs_cross_validation ? cfg_.preset : d.preset;
      next_decision = std::move(d);
    }

    SessionInputs in =
        make_session_inputs(effective_preset, chain.spot(), chain.rate(), chain.now_ns());
    in.fit_workers = cfg_.fit_workers;
    in.collect_stage_timings = cfg_.collect_stage_timings;
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
    in.query_pricing_tier = cfg_.query_pricing_tier;
    if (cfg_.score_parity.has_value()) {
      in.score_parity = *cfg_.score_parity;
    } else if (fit_admission_consumes_parity(cfg_.admission)) {
      in.score_parity = true;
    } else if (cfg_.admission.consumer == SurfaceConsumer::Mark) {
      // A floor-free Mark admission consumes the fitted curve, not the
      // re-Americanized quality report. Diagnostic-dependent Mark policies and
      // Quote/Risk remain scored by default.
      in.score_parity = false;
    }
    if (cfg_.enforce_calendar_floor.has_value()) {
      in.enforce_calendar_floor = *cfg_.enforce_calendar_floor;
    }
    if (cfg_.use_deam_cache_for_fit.has_value()) {
      in.use_deam_cache_for_fit = *cfg_.use_deam_cache_for_fit;
    }
    if (cfg_.fit_prep_policy)
      in.fit_prep_policy = *cfg_.fit_prep_policy;
    if (cfg_.audit_fit_inversions)
      in.deam.audit_fit_inversions = *cfg_.audit_fit_inversions;
    if (cfg_.warm_start_carry)
      in.deam.warm_start_carry = *cfg_.warm_start_carry;
    if (cfg_.max_obs_per_slice.has_value()) {
      in.calib.max_obs_per_slice = *cfg_.max_obs_per_slice;
    }
    if (cfg_.max_deam_strikes_per_expiry.has_value()) {
      in.calib.max_deam_strikes_per_expiry = *cfg_.max_deam_strikes_per_expiry;
    }
    if (cfg_.max_otm_shortcut_premium_spread_frac.has_value()) {
      in.calib.max_otm_shortcut_premium_spread_frac = *cfg_.max_otm_shortcut_premium_spread_frac;
    }
    if (!in.expiry_rates.empty()) {
      // CorrectionCache is built at one scalar (r, q) pair. A term-rate board
      // must stay on the cold pricer until the cache itself becomes term-aware.
      if (in.query_pricing_tier == QueryPricingTier::LegacyCompatible ||
          in.query_pricing_tier == QueryPricingTier::ColdReference) {
        in.use_correction_cache = false;
      }
      in.use_deam_cache_for_fit = false;
    }

    // Curve config: pinned, profile-direct, or held-out selected for this board.
    if (cfg_.curve.has_value()) {
      in.curve = *cfg_.curve;
      in.curve_pinned = true;
    } else if (pinned_hft) {
      // Hft's preset-pinned direct market curve avoids both selector candidate
      // fits and the per-expiry dense QP on penny-dense index boards.
      in.curve.kind = VolCurveKind::LinearVariance;
      in.curve_pinned = true;
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
      sp.fit_workers = in.fit_workers;
      sp.fit_prep_policy = in.fit_prep_policy;
      sp.score_parity = in.score_parity;
      sp.enforce_calendar_floor = in.enforce_calendar_floor;
      sp.use_deam_cache_for_fit = in.use_deam_cache_for_fit;
      Result<SelectorResult> selected = select_curve(chain.underlying(), sp, cfg_.selector);
      if (!selected.has_value()) {
        SurfaceBuildReport report;
        report.primary_curve = in.curve;
        report.retained_last_known_good = market_mark_surface_ != nullptr;
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
    const FitAdmissionPolicy publication_admission =
        next_selection.has_value()
            ? detail::selector_served_admission_policy(cfg_.admission, cfg_.selector)
            : cfg_.admission;
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
          completed_attempt_report(under, in.curve, *built, publication_admission);
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
            completed_attempt_report(under, in.curve, *retry, publication_admission);
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
      report.retained_last_known_good = market_mark_surface_ != nullptr;
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
    const std::uint64_t legacy_generation = ++candidate_generation_;
    const SurfacePurpose legacy_purpose = cfg_.admission.consumer == SurfaceConsumer::Risk
                                              ? SurfacePurpose::Risk
                                              : SurfacePurpose::MarketMark;
    const FitQualityMode legacy_quality = map_legacy_fit_preset(cfg_.preset).quality_mode;
    std::shared_ptr<const FittedSurface> next_surface(
        new FittedSurface(std::move(sess), legacy_purpose, legacy_quality, legacy_generation));
    std::optional<SurfaceBuildReport> next_published{report};
    std::optional<SurfaceBuildReport> next_attempt{std::move(report)};
    std::optional<SelectorResult> next_served_selection{next_selection};
    std::optional<FitDecision> next_served_decision{next_decision};
    FitSnapshotProvenance provenance;
    provenance.chain_instance_id = chain.instance_id();
    provenance.board_revision = chain.quote_revision();
    provenance.uid = chain.uid();
    provenance.expiry_revisions.assign(chain.expiry_quote_revisions().begin(),
                                       chain.expiry_quote_revisions().end());
    std::optional<FitSnapshotProvenance> next_provenance{std::move(provenance)};
    const SurfaceHealth next_health{
        .purpose = legacy_purpose,
        .quality_mode = legacy_quality,
        .state = SurfaceState::Healthy,
        .reasons = ValidationFailure::None,
        .candidate_generation = legacy_generation,
        .served_generation = legacy_generation,
    };

    // Transaction boundary: admitted state and its provenance become current
    // together. Every earlier failure leaves the last-known-good publication.
    market_mark_surface_ = std::move(next_surface);
    market_mark_provenance_ = std::move(next_provenance);
    market_mark_health_ = next_health;
    selection_ = std::move(next_selection);
    served_selection_ = std::move(next_served_selection);
    decision_ = std::move(next_decision);
    served_decision_ = std::move(next_served_decision);
    published_report_ = std::move(next_published);
    last_attempt_report_ = std::move(next_attempt);
    timings_.total_ms = elapsed_ms(fit_start);
    return Ok();
  }

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
    in.fit_workers = cfg_.fit_workers;
    in.collect_stage_timings = cfg_.collect_stage_timings;
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
    in.query_pricing_tier = cfg_.query_pricing_tier;
    if (cfg_.score_parity.has_value()) {
      in.score_parity = *cfg_.score_parity;
    } else if (fit_admission_consumes_parity(cfg_.admission)) {
      in.score_parity = true;
    } else if (cfg_.admission.consumer == SurfaceConsumer::Mark) {
      in.score_parity = false;
    }
    if (cfg_.use_deam_cache_for_fit.has_value()) {
      in.use_deam_cache_for_fit = *cfg_.use_deam_cache_for_fit;
    }
    if (cfg_.fit_prep_policy)
      in.fit_prep_policy = *cfg_.fit_prep_policy;
    if (cfg_.audit_fit_inversions)
      in.deam.audit_fit_inversions = *cfg_.audit_fit_inversions;
    if (cfg_.warm_start_carry)
      in.deam.warm_start_carry = *cfg_.warm_start_carry;
    if (cfg_.max_obs_per_slice.has_value()) {
      in.calib.max_obs_per_slice = *cfg_.max_obs_per_slice;
    }
    if (cfg_.max_deam_strikes_per_expiry.has_value()) {
      in.calib.max_deam_strikes_per_expiry = *cfg_.max_deam_strikes_per_expiry;
    }
    if (cfg_.max_otm_shortcut_premium_spread_frac.has_value()) {
      in.calib.max_otm_shortcut_premium_spread_frac = *cfg_.max_otm_shortcut_premium_spread_frac;
    }
    if (!in.expiry_rates.empty()) {
      if (in.query_pricing_tier == QueryPricingTier::LegacyCompatible ||
          in.query_pricing_tier == QueryPricingTier::ColdReference) {
        in.use_correction_cache = false;
      }
      in.use_deam_cache_for_fit = false;
    }
  };

  // Snapshot provenance is stamped at the transaction boundary of whichever
  // purpose publishes (mark-only or risk); every earlier failure leaves the
  // last-known-good publication and its provenance untouched.
  const auto snapshot_provenance = [&]() {
    FitSnapshotProvenance provenance;
    provenance.chain_instance_id = chain.instance_id();
    provenance.board_revision = chain.quote_revision();
    provenance.uid = chain.uid();
    provenance.expiry_revisions.assign(chain.expiry_quote_revisions().begin(),
                                       chain.expiry_quote_revisions().end());
    return provenance;
  };

  // ── Mark-only request (legacy HFT mapping or explicit outputs) ────────────
  //
  // Synchronous, admission-gated, transactional publish: the mark serves under
  // the family-neutral FitAdmissionPolicy (default = the WP12 Mark-serving
  // contract, which admits healthy real-world marks; the strict risk policy is
  // an explicit opt-in). Risk-purpose admission below additionally requires the
  // independent risk oracle — a mark gate can never substitute for it.
  if (!has_output(requested_outputs, SurfacePurpose::Risk)) {
    SessionInputs mark_in =
        make_session_inputs(FitPreset::Hft, chain.spot(), chain.rate(), chain.now_ns());
    configure_common(mark_in);
    mark_in.curve.kind = VolCurveKind::LinearVariance;
    mark_in.curve_pinned = true;
    if (session_overlay) {
      session_overlay(mark_in);
    }
    const Underlying &under = chain.underlying();
    SurfaceBuildReport report;
    report.primary_curve = mark_in.curve;
    const auto mark_start = Clock::now();
    Result<VolaSession> built = VolaSession::build(under, mark_in);
    timings_.market_mark_build_ms = elapsed_ms(mark_start);
    const auto retain_or_reject = [&](ValidationFailure reason) -> bool {
      // true => last-known-good mark retained (health Stale); false => nothing
      // is served (health Rejected; fallback None drops the stale surface).
      if (market_mark_surface_ != nullptr && cfg_.fallback == SurfaceFallback::LastKnownGood) {
        market_mark_health_ = SurfaceHealth{
            .purpose = SurfacePurpose::MarketMark,
            .quality_mode = quality_mode,
            .state = SurfaceState::Stale,
            .reasons = reason,
            .candidate_generation = candidate_generation_,
            .served_generation = market_mark_surface_->generation(),
            .fallback_generation = market_mark_surface_->generation(),
        };
        return true;
      }
      if (cfg_.fallback == SurfaceFallback::None) {
        market_mark_surface_.reset();
        market_mark_provenance_.reset();
      }
      market_mark_health_ = SurfaceHealth{
          .purpose = SurfacePurpose::MarketMark,
          .quality_mode = quality_mode,
          .state = SurfaceState::Rejected,
          .reasons = reason,
          .candidate_generation = candidate_generation_,
      };
      return false;
    };
    if (!built.has_value()) {
      report.retained_last_known_good = market_mark_surface_ != nullptr;
      report.attempts.push_back(failed_attempt_report(under, mark_in.curve, built.error()));
      last_attempt_report_ = std::move(report);
      const bool retained = retain_or_reject(ValidationFailure::InsufficientData);
      timings_.total_ms = elapsed_ms(fit_start);
      if (retained) {
        return Ok();
      }
      return Err(std::move(built).error());
    }
    SurfaceBuildAttemptReport attempt =
        completed_attempt_report(under, mark_in.curve, *built, cfg_.admission);
    const bool mark_admitted = attempt.admission.admitted;
    report.attempts.push_back(std::move(attempt));
    if (!mark_admitted) {
      report.retained_last_known_good = market_mark_surface_ != nullptr;
      last_attempt_report_ = std::move(report);
      (void)retain_or_reject(ValidationFailure::InvalidDomain);
      timings_.total_ms = elapsed_ms(fit_start);
      return Err(ErrorCode::Unavailable,
                 "PricerFitter::fit: mark candidate failed surface admission");
    }
    report.published = true;
    report.published_curve = mark_in.curve;
    report.attempts.back().stage = SurfaceBuildStage::Publication;
    std::shared_ptr<const FittedSurface> next_surface(new FittedSurface(
        std::move(*built), SurfacePurpose::MarketMark, quality_mode, candidate_generation_));
    std::optional<FitSnapshotProvenance> next_provenance{snapshot_provenance()};
    const SurfaceHealth next_health{
        .purpose = SurfacePurpose::MarketMark,
        .quality_mode = quality_mode,
        .state = SurfaceState::Healthy,
        .reasons = ValidationFailure::None,
        .candidate_generation = candidate_generation_,
        .served_generation = candidate_generation_,
    };
    std::optional<SurfaceBuildReport> next_published{report};
    std::optional<SurfaceBuildReport> next_attempt{std::move(report)};
    market_mark_surface_ = std::move(next_surface);
    market_mark_provenance_ = std::move(next_provenance);
    market_mark_health_ = next_health;
    published_report_ = std::move(next_published);
    last_attempt_report_ = std::move(next_attempt);
    timings_.total_ms = elapsed_ms(fit_start);
    return Ok();
  }

  // ── Dual request: the mark builds concurrently with the risk pipeline ─────
  std::optional<std::future<MarkBuildResult>> mark_future;
  if (has_output(requested_outputs, SurfacePurpose::MarketMark)) {
    SessionInputs mark_in =
        make_session_inputs(FitPreset::Hft, chain.spot(), chain.rate(), chain.now_ns());
    configure_common(mark_in);
    mark_in.curve.kind = VolCurveKind::LinearVariance;
    mark_in.curve_pinned = true;
    if (session_overlay) {
      session_overlay(mark_in);
    }
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
      std::optional<FitSnapshotProvenance> next_provenance{snapshot_provenance()};
      std::shared_ptr<const FittedSurface> next_surface(
          new FittedSurface(std::move(*result.built), SurfacePurpose::MarketMark, quality_mode,
                            candidate_generation_));
      const SurfaceHealth next_health{
          .purpose = SurfacePurpose::MarketMark,
          .quality_mode = quality_mode,
          .state = SurfaceState::Healthy,
          .reasons = ValidationFailure::None,
          .candidate_generation = candidate_generation_,
          .served_generation = candidate_generation_,
      };
      // Mark publication is independent of risk admission. Publish its surface,
      // health, and chain identity as one no-fail state transition after every
      // allocating operation above has succeeded.
      market_mark_surface_ = std::move(next_surface);
      market_mark_provenance_ = std::move(next_provenance);
      market_mark_health_ = next_health;
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
      market_mark_provenance_.reset();
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

  if (cfg_.risk_admission != RiskAdmission::Required ||
      (cfg_.enforce_calendar_floor.has_value() && !*cfg_.enforce_calendar_floor) ||
      (cfg_.score_parity.has_value() && !*cfg_.score_parity) ||
      (cfg_.curve.has_value() && cfg_.curve->kind == VolCurveKind::LinearVariance)) {
    ValidationDigest rejected;
    rejected.failures = ValidationFailure::InvalidDomain;
    const std::uint64_t prior = risk_surface_ != nullptr ? risk_surface_->generation() : 0u;
    risk_health_ = decide_risk_surface_admission(rejected, quality_mode, candidate_generation_,
                                                 prior, cfg_.fallback)
                       .health;
    if (cfg_.fallback == SurfaceFallback::None) {
      risk_surface_.reset();
      risk_provenance_.reset();
      served_decision_.reset();
      served_selection_.reset();
    }
    atx::core::Error policy_error{ErrorCode::InvalidArgument,
                                  "invalid correctness policy for requested risk surface"};
    SurfaceBuildReport report;
    report.primary_curve = validation_curve;
    report.retained_last_known_good = risk_surface_ != nullptr;
    report.attempts.push_back(failed_attempt_report(
        chain.underlying(), validation_curve, policy_error, SurfaceBuildStage::InputValidation));
    last_attempt_report_ = std::move(report);
    // §5.6: the requested mark is still built and published on its own
    // contract; only the risk output is refused. The caller must still learn
    // the risk config was invalid, so the policy error outranks mark status.
    (void)finalize_mark();
    timings_.total_ms = elapsed_ms(fit_start);
    return Err(std::move(policy_error));
  }

  const FitPreset risk_preset = quality_mode == FitQualityMode::Latency    ? FitPreset::Fast
                                : quality_mode == FitQualityMode::Accuracy ? FitPreset::Accurate
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
    // Every risk mode pins an EXPLICIT Andersen-Lake de-Am preset. An unset
    // al_opts is the legacy-compat signal that lets VolaSession::build substitute
    // the fast preset, a loosened iv_tol, and a single-pair carry floor — which
    // would silently undo every per-mode carry and inversion budget set below.
    //
    // F1 (C3 tier honesty, docs/al-preset-ladder.md §5-6): the bulk `Populate`
    // preset is authorized to run the CHEAPER Andersen-Lake block (`al_fast_opts`,
    // the specialized (7,16,4) FP block) on the de-Am inversion lane — the exact
    // lever the C3 sprint created it for, defeated until now because this policy
    // re-pinned `al_default_opts` AFTER the Populate overlay applied it. Keyed on
    // the PRESET, never the quality mode: it lowers ONLY the American boundary
    // precision (< ~1e-3 IV, well inside the ~1e-2 surface RMSE and the quote
    // half-spread), while the risk GEOMETRY — audited inversions
    // (audit_fit_inversions), calendar Project, require_carry_confidence, the
    // per-mode carry anchors (n_atm/max_borrow_pairs) and the validation oracle —
    // stays fully accurate. Accuracy mode never permits it (its contract IS the
    // reference AL). Non-Populate presets are byte-identical to the prior pin.
    //
    // Perf 2b: FitPreset::Bulk is Populate's tier with the cheaper `ql_fast` rung
    // (session.hpp). It takes the SAME authorization on the same grounds and, like
    // Populate, only where the quality mode is not Accuracy. It additionally pins
    // the BAKED serve rung back to al_fast_opts, because al_bulk_opts uses the
    // decoupled `n_quad_price` axis and no AlOpts record format persists it — see
    // DeAmOptions::serve_al_opts. Every other preset is byte-identical to the
    // prior pin.
    const bool cheap_deam_authorized = quality_mode != FitQualityMode::Accuracy;
    const auto risk_deam_al =
        (cfg_.preset == FitPreset::Bulk && cheap_deam_authorized)     ? al_bulk_opts()
        : (cfg_.preset == FitPreset::Populate && cheap_deam_authorized) ? al_fast_opts()
                                                                       : al_default_opts();
    if (cfg_.preset == FitPreset::Bulk && cheap_deam_authorized) {
      // The CARRY rung as well, and it is the one that carries the win: Phase-2b
      // step-1 measured ~66% of a `populate` date's fast-tier AL boundary solves
      // inside the PCP borrow fixed point, not the per-strike de-Am inversion, so
      // lowering only the de-Am rung caps the speedup at ~1.1x. Set HERE and not only
      // in apply_fit_preset because this policy runs AFTER
      // apply_fit_preset(risk_preset = Robust), which re-pins carry_al_opts
      // unconditionally. Why the carry tolerates it: session.cpp's FitPreset::Bulk.
      in.deam.carry_al_opts = al_bulk_opts();
      in.deam.serve_al_opts = al_fast_opts();
    }
    if (quality_mode == FitQualityMode::Accuracy) {
      in.deam.al_opts = risk_deam_al; // == al_default_opts() for Accuracy
      in.use_correction_cache = false;
      in.use_deam_cache_for_fit = false;
    }
    switch (quality_mode) {
    case FitQualityMode::Latency:
      // A fast proposal plus mandatory cold audit costs more than solving the
      // smaller Latency node set accurately once. Latency comes from bounded
      // work and narrower validation, never from publishing an unaudited IV.
      in.deam.al_opts = risk_deam_al;
      in.deam.n_atm = 3;
      in.deam.max_borrow_pairs = 5;
      in.calib.max_obs_per_slice = cfg_.max_obs_per_slice.value_or(40u);
      in.calib.max_otm_shortcut_premium_spread_frac =
          cfg_.max_otm_shortcut_premium_spread_frac.value_or(0.50);
      break;
    case FitQualityMode::Balanced:
      in.deam.n_atm = 8;
      in.deam.max_borrow_pairs = 5;
      // Certified fast proposals frequently require a cold fallback on dense
      // boards, paying for both paths. The direct accurate reference is faster
      // in that regime and is the correctness-first Balanced default; the
      // Populate preset instead honors the cheaper de-Am block via risk_deam_al.
      in.deam.al_opts = risk_deam_al;
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
    in.curve_pinned = true;
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
    sp.fit_workers = in.fit_workers;
    sp.fit_prep_policy = in.fit_prep_policy;
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

  // Transactional attempt history for the risk pipeline (primary + every
  // ladder rung); moves to published_report_ atomically with the admitted
  // surface, or to last_attempt_report_ alone on failure.
  const Underlying &under = chain.underlying();
  SurfaceBuildReport report;
  report.primary_curve = in.curve;

  // `session_overlay` layers per-symbol config onto the EXACT inputs this fit
  // uses, once, immediately before the first build; a fallback-ladder retry
  // does not re-invoke it (main contract).
  //
  // MERGE: main's overlay seam and the branch's risk pipeline met here for the
  // first time, and the overlay ran LAST — so a per-symbol config
  // (apply_symbol_config -> apply_fit_preset) silently overwrote the resolved
  // risk family and every mandatory risk budget: calendar_repair Project->None,
  // score_parity / enforce_calendar_floor, the pinned accurate Andersen-Lake
  // reference, require_carry_confidence and the per-mode carry/observation
  // floors. Observed effect: every populate board's risk build collapsed to
  // InsufficientData. It is also a fail-OPEN hole — the config-level equivalents
  // (score_parity=false, enforce_calendar_floor=false) are hard-rejected as an
  // "invalid correctness policy for requested risk surface" above, so the
  // overlay must not be able to smuggle them in. The overlay's own knobs
  // (band_k, al_override, caches, a pinned curve via cfg_.curve) still reach the
  // fit; the mandatory risk contract is re-asserted on top of them.
  if (session_overlay) {
    const CurveConfig resolved_curve = in.curve;
    session_overlay(in);
    apply_risk_policy();
    in.curve = resolved_curve;
  }

  Result<VolaSession> built = VolaSession::build(under, in);
  const bool auto_routed = decision_.has_value() && !cfg_.curve.has_value();
  if (!built.has_value()) {
    report.attempts.push_back(failed_attempt_report(under, in.curve, built.error()));
  }
  if (!built.has_value() && auto_routed) {
    const CurveConfig primary_curve = in.curve;
    for (const VolCurveKind rung : fallback_curve_rungs(primary_curve.kind)) {
      if (rung == VolCurveKind::LinearVariance) {
        continue;
      }
      in.curve.kind = rung;
      Result<VolaSession> retry = VolaSession::build(under, in);
      if (!retry.has_value()) {
        report.attempts.push_back(failed_attempt_report(under, in.curve, retry.error()));
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
    report.retained_last_known_good = risk_surface_ != nullptr;
    last_attempt_report_ = std::move(report);
    (void)finalize_mark();
    timings_.total_ms = elapsed_ms(fit_start);
    if (risk_surface_ != nullptr && risk_health_.using_fallback()) {
      return Ok();
    }
    if (cfg_.fallback == SurfaceFallback::None) {
      risk_surface_.reset();
      risk_provenance_.reset();
      served_decision_.reset();
      served_selection_.reset();
    }
    return Err(std::move(built).error());
  }

  VolaSession sess = std::move(*built);
  const RiskSurfaceValidationConfig validation_config = risk_validation_config(quality_mode);
  const auto validate_candidate = [&](const VolaSession &candidate) {
    const auto validation_start = Clock::now();
    Result<ValidationDigest> checked = validate_risk_surface(candidate, validation_config);
    timings_.risk_validation_ms += elapsed_ms(validation_start);
    ValidationDigest result;
    if (checked.has_value()) {
      result = *checked;
    } else {
      result.failures = ValidationFailure::InvalidDomain;
    }
    merge_session_failure_context(candidate.diagnostics(), result);
    return result;
  };
  // Candidate admission requires BOTH gates, fail-closed: the independent risk
  // oracle (geometry + certification, decide_risk_surface_admission) AND the
  // family-neutral FitAdmissionPolicy publication gate (`cfg_.admission`;
  // default = the WP12 Mark-serving numerical floor, strict risk policy is the
  // caller's opt-in). The policy verdict is folded into the digest BEFORE the
  // oracle decision so served health can never disagree with publication.
  //
  // F2 (R-02): SERVED-BREADTH FLOOR on the risk rebuild, all routes. The mark
  // consumer raises its quote-coverage floor to the selector's served breadth
  // (`selector_served_admission_policy`) ONLY when a selector routed the board
  // (fit(), :640). A risk surface is the safety-critical consumer, so here we
  // apply that same floor on EVERY route — selector-routed, caller-pinned, or a
  // fallback rung — and UNCONDITIONALLY, making it a strict superset of the mark
  // path. Without it a narrow-coverage rebuild that the mark gate would reject
  // under a selector could still slip into the risk surface through a
  // non-selector route. min_quote_coverage becomes
  // max(cfg_.admission.min_quote_coverage, cfg_.selector.min_served_quote_coverage).
  const FitAdmissionPolicy risk_admission_policy =
      detail::selector_served_admission_policy(cfg_.admission, cfg_.selector);
  const auto admission_attempt = [&](const VolaSession &candidate, ValidationDigest &digest) {
    SurfaceBuildAttemptReport attempt =
        completed_attempt_report(under, in.curve, candidate, risk_admission_policy);
    if (!attempt.admission.admitted) {
      digest.failures |= ValidationFailure::InvalidDomain;
    }
    finalize_validation_digest(digest, validation_config);
    return attempt;
  };

  ValidationDigest digest = validate_candidate(sess);
  SurfaceBuildAttemptReport attempt = admission_attempt(sess, digest);
  const std::uint64_t prior = risk_surface_ != nullptr ? risk_surface_->generation() : 0u;
  AdmissionDecision admission = decide_risk_surface_admission(
      digest, quality_mode, candidate_generation_, prior, cfg_.fallback);
  report.attempts.push_back(std::move(attempt));
  // Hoisted above the validation-rejection ladder (which mutates `in` on every
  // successfully-built rung, admitted or not — see `in = std::move(retry_inputs)`
  // below) so this always names the TRUE first-rejected primary, for both the
  // ladder's own provenance stamp and the strict-recovery rung further below.
  const CurveConfig rejected_primary_curve = in.curve;

  // A policy curve is only a prior. Validation rejection walks the same safe
  // model ladder as a construction failure; each rung must independently pass
  // the complete admission contract before it can replace the candidate.
  if (!admission.publish_candidate && auto_routed) {
    for (const VolCurveKind rung0 : fallback_curve_rungs(rejected_primary_curve.kind)) {
      // The risk path serves ConvexDense as its dense model, so its fallback
      // ladder must REACH the dense fit where the generic ladder names
      // LinearVariance — the same substitution routing already applies
      // (pricer_fitter.cpp:~1109/1156). Without it a parametric primary that a
      // real board serves poorly (e.g. an eSSVI whose in-band served coverage
      // trips the F2 floor) dead-ends at the skipped LinearVariance rung instead
      // of falling to the dense model that fits the same board. Never re-fit the
      // rejected primary's own family.
      const VolCurveKind rung =
          (rung0 == VolCurveKind::LinearVariance) ? VolCurveKind::ConvexDense : rung0;
      if (rung == rejected_primary_curve.kind)
        continue;
      SessionInputs retry_inputs = in;
      retry_inputs.curve.kind = rung;
      if (rung == VolCurveKind::ConvexDense)
        retry_inputs.curve.convex.node_cap = retry_inputs.calib.max_obs_per_slice;
      const auto retry_start = Clock::now();
      Result<VolaSession> retry = VolaSession::build(chain.underlying(), retry_inputs);
      timings_.risk_build_ms += elapsed_ms(retry_start);
      if (!retry.has_value()) {
        report.attempts.push_back(failed_attempt_report(under, retry_inputs.curve, retry.error()));
        continue;
      }
      in = std::move(retry_inputs);
      ValidationDigest retry_digest = validate_candidate(*retry);
      SurfaceBuildAttemptReport retry_attempt = admission_attempt(*retry, retry_digest);
      AdmissionDecision retry_admission = decide_risk_surface_admission(
          retry_digest, quality_mode, candidate_generation_, prior, cfg_.fallback);
      report.attempts.push_back(std::move(retry_attempt));
      if (!retry_admission.publish_candidate)
        continue;
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
          decision_->primary_curve = rejected_primary_curve;
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

  // Strict convex-dense recovery — the rung after the last rung. Reached only
  // when the candidate was rejected and the digest names pure geometry
  // (Butterfly/Calendar, optionally CarryGap), AND the caller either let the
  // fitter auto-route (`auto_routed`) or explicitly pinned ConvexDense itself.
  // A pin on a NON-dense family stays exactly what its contract promises —
  // "pin (one attempt, no recovery)" (surface_db_build.cpp) / "skip both
  // fallback ladders" (surface_db_seed.hpp) — this rung must not silently
  // substitute ConvexDense underneath it. A ConvexDense pin, by contrast, is
  // this rung's OWN family: it is a same-family strict refit (tighter repair
  // + exact-node promotion of THAT candidate), never a family substitution,
  // so it stays eligible under the pin. Root cause (2026-08 SPY backfill): the
  // dense repair loop's fixed lattice + 1e-7 acceptance are strictly looser
  // than this oracle's grid + 1e-8, so marginal sub-vol-tick crossings pass
  // repair and die here. The refit pins repair to the oracle's exact calendar
  // grid at 0.1x its tolerance and promotes the digest's reported violation
  // k's to exact QP nodes; each round's new firsts feed the next round.
  // Admitted fits never reach this block, so the hot path is unchanged.
  if (!admission.publish_candidate &&
      (auto_routed || rejected_primary_curve.kind == VolCurveKind::ConvexDense) &&
      detail::should_attempt_strict_recovery(digest.failures)) {
    constexpr int kMaxStrictRecoveryRounds = 3;
    ConvexRepairSpec spec = detail::make_strict_repair_spec(validation_config);
    ValidationDigest round_digest = digest;
    for (int round = 0; round < kMaxStrictRecoveryRounds; ++round) {
      const std::vector<double> promoted =
          detail::strict_promotion_ks(round_digest, validation_config);
      const std::size_t before = spec.extra_node_ks.size();
      spec.extra_node_ks.insert(spec.extra_node_ks.end(), promoted.begin(), promoted.end());
      std::sort(spec.extra_node_ks.begin(), spec.extra_node_ks.end());
      spec.extra_node_ks.erase(
          std::unique(spec.extra_node_ks.begin(), spec.extra_node_ks.end()),
          spec.extra_node_ks.end());
      if (round > 0 && spec.extra_node_ks.size() == before) {
        break; // no new violation k's — an identical refit cannot converge
      }
      SessionInputs strict_inputs = in;
      strict_inputs.curve.kind = VolCurveKind::ConvexDense;
      strict_inputs.curve.convex.node_cap = strict_inputs.calib.max_obs_per_slice;
      strict_inputs.curve.convex_repair = spec;
      ATX_VOL_COUNT(RiskStrictRecoveryRounds);
      const auto strict_start = Clock::now();
      Result<VolaSession> strict = VolaSession::build(chain.underlying(), strict_inputs);
      timings_.risk_build_ms += elapsed_ms(strict_start);
      if (!strict.has_value()) {
        report.attempts.push_back(
            failed_attempt_report(under, strict_inputs.curve, strict.error()));
        break; // the strict QP itself failed — nothing further to promote
      }
      // Mirror the ladder above (`in = std::move(retry_inputs);` immediately
      // after a successful build, before validate/admission): admission_attempt
      // captures `in` by reference and stamps `in.curve` into the attempt
      // report, so `in` must already name THIS round's strict candidate before
      // validate_candidate/admission_attempt run, or every strict attempt's
      // report entry — including the one that ends up published — records the
      // stale pre-recovery curve instead of the curve actually measured.
      in = std::move(strict_inputs);
      ValidationDigest strict_digest = validate_candidate(*strict);
      SurfaceBuildAttemptReport strict_attempt = admission_attempt(*strict, strict_digest);
      AdmissionDecision strict_admission = decide_risk_surface_admission(
          strict_digest, quality_mode, candidate_generation_, prior, cfg_.fallback);
      report.attempts.push_back(std::move(strict_attempt));
      if (strict_admission.publish_candidate) {
        ATX_VOL_COUNT(RiskStrictRecoveryAdmitted);
        // Same provenance contract as the ladder adoption above: the first
        // rejected primary of this fit stays authoritative.
        sess = std::move(*strict);
        digest = strict_digest;
        admission = strict_admission;
        if (decision_.has_value()) {
          if (!decision_->used_fallback) {
            decision_->primary_curve = rejected_primary_curve;
          }
          decision_->used_fallback = true;
          decision_->curve = in.curve;
        }
        if (selection_.has_value()) {
          selection_->chosen = in.curve;
        }
        break;
      }
      if (!detail::should_attempt_strict_recovery(strict_digest.failures)) {
        break; // the strict refit surfaced a non-geometric failure — stop
      }
      round_digest = strict_digest;
    }
  }
  risk_health_ = admission.health;
  if (!admission.publish_candidate) {
    report.retained_last_known_good = risk_surface_ != nullptr;
    last_attempt_report_ = std::move(report);
    (void)finalize_mark();
    timings_.total_ms = elapsed_ms(fit_start);
    if (risk_surface_ != nullptr && risk_health_.using_fallback()) {
      return Ok();
    }
    if (cfg_.fallback == SurfaceFallback::None) {
      risk_surface_.reset();
      risk_provenance_.reset();
      served_decision_.reset();
      served_selection_.reset();
    }
    const SessionDiagnostics &session_diagnostics = sess.diagnostics();
    return Err(ErrorCode::Unavailable,
               "risk surface rejected: model=" + std::string(to_string(sess.inputs().curve.kind)) +
                   " mask=" + std::to_string(static_cast<std::uint32_t>(digest.failures)) +
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
                   " inversion=" + (session_diagnostics.inversion_certified ? "ok" : "failed"));
  }

  // Transaction boundary: the admitted risk surface, its provenance records,
  // and the served decision/selection become current together.
  report.published = true;
  report.published_curve = in.curve;
  report.attempts.back().stage = SurfaceBuildStage::Publication;
  std::optional<FitSnapshotProvenance> next_provenance{snapshot_provenance()};
  std::optional<FitDecision> next_served_decision{decision_};
  std::optional<SelectorResult> next_served_selection{selection_};
  std::optional<SurfaceBuildReport> next_published{report};
  std::optional<SurfaceBuildReport> next_attempt{std::move(report)};
  std::shared_ptr<const FittedSurface> next_surface(new FittedSurface(
      std::move(sess), SurfacePurpose::Risk, quality_mode, candidate_generation_));

  risk_surface_ = std::move(next_surface);
  risk_provenance_ = std::move(next_provenance);
  served_decision_ = std::move(next_served_decision);
  served_selection_ = std::move(next_served_selection);
  published_report_ = std::move(next_published);
  last_attempt_report_ = std::move(next_attempt);
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

Result<ExpiryRefitDiagnostics> PricerFitter::refit_expiry(const OptionChain &chain,
                                                          ExpiryId expiry_id) {
  // The transactional expiry refit operates on the config's default-purpose
  // surface and its matching provenance. When that surface is the RISK surface,
  // publication additionally requires the independent risk oracle below — the
  // mark-grade FitAdmissionPolicy alone can never republish a risk surface
  // (fail-closed, §5.2).
  const bool risk_purpose = has_output(effective_request().outputs, SurfacePurpose::Risk);
  const FittedSurface *served = surface();
  const std::optional<FitSnapshotProvenance> &published_provenance =
      risk_purpose ? risk_provenance_ : market_mark_provenance_;
  CurveConfig published_curve{};
  if (served != nullptr) {
    published_curve = served->session().inputs().curve;
  }
  const auto fail = [&](atx::core::Error error,
                        SurfaceBuildStage stage =
                            SurfaceBuildStage::Build) -> Result<ExpiryRefitDiagnostics> {
    SurfaceBuildReport report;
    report.primary_curve = published_curve;
    report.refit_expiry = expiry_id;
    report.source_quote_revision = chain.quote_revision();
    report.retained_last_known_good = served != nullptr;
    report.attempts.push_back(
        failed_attempt_report(chain.underlying(), published_curve, error, stage));
    last_attempt_report_ = std::move(report);
    return Err(std::move(error));
  };

  if (served == nullptr || !published_provenance.has_value()) {
    return fail(atx::core::Error{ErrorCode::Unavailable,
                                 "PricerFitter::refit_expiry: no published surface"});
  }
  const FitSnapshotProvenance &published = *published_provenance;
  if (chain.instance_id() != published.chain_instance_id || chain.uid() != published.uid) {
    return fail(atx::core::Error{ErrorCode::InvalidArgument,
                                 "PricerFitter::refit_expiry: chain instance differs from fit"},
                SurfaceBuildStage::InputValidation);
  }
  const Underlying &under = chain.underlying();
  if (expiry_id >= under.chains.size() || under.chains[expiry_id].expiry_id != expiry_id) {
    return fail(
        atx::core::Error{ErrorCode::NotFound, "PricerFitter::refit_expiry: expiry id not found"},
        SurfaceBuildStage::InputValidation);
  }
  const std::span<const std::uint64_t> current_revisions = chain.expiry_quote_revisions();
  if (current_revisions.size() != published.expiry_revisions.size()) {
    return fail(atx::core::Error{ErrorCode::InvalidArgument,
                                 "PricerFitter::refit_expiry: expiry topology changed"},
                SurfaceBuildStage::InputValidation);
  }
  for (std::size_t index = 0u; index < current_revisions.size(); ++index) {
    if (index != expiry_id && current_revisions[index] != published.expiry_revisions[index]) {
      return fail(
          atx::core::Error{ErrorCode::InvalidArgument,
                           "PricerFitter::refit_expiry: a non-target expiry has newer quotes"},
          SurfaceBuildStage::InputValidation);
    }
  }
  if (published_curve.kind != VolCurveKind::Essvi) {
    return fail(atx::core::Error{ErrorCode::NotImplemented,
                                 "PricerFitter::refit_expiry: only eSSVI is supported"},
                SurfaceBuildStage::InputValidation);
  }
  switch (served->session().inputs().calendar_repair) {
  case CalendarRepair::None:
    break;
  case CalendarRepair::MonotoneFit:
  case CalendarRepair::Project:
    return fail(
        atx::core::Error{ErrorCode::NotImplemented,
                         "PricerFitter::refit_expiry: configured calendar repair is unsupported"},
        SurfaceBuildStage::InputValidation);
  }
  if (candidate_generation_ == std::numeric_limits<std::uint64_t>::max()) {
    return fail(atx::core::Error{ErrorCode::OutOfRange, "surface generation counter exhausted"},
                SurfaceBuildStage::InputValidation);
  }

  const Chain &target_chain = under.chains[expiry_id];
  const std::span<const SliceContext> fitted_expiries = served->session().expiries();
  std::optional<std::size_t> fitted_index;
  for (std::size_t index = 0u; index < fitted_expiries.size(); ++index) {
    if (fitted_expiries[index].T != target_chain.T) {
      continue;
    }
    if (fitted_index.has_value()) {
      return fail(atx::core::Error{ErrorCode::InvalidArgument,
                                   "PricerFitter::refit_expiry: maturity is ambiguous"},
                  SurfaceBuildStage::InputValidation);
    }
    fitted_index = index;
  }
  if (!fitted_index.has_value()) {
    return fail(
        atx::core::Error{ErrorCode::NotFound, "PricerFitter::refit_expiry: expiry was not fitted"},
        SurfaceBuildStage::InputValidation);
  }

  Result<CanonicalPreparedExpiry> prepared =
      prepare_expiry(target_chain, static_cast<std::uint32_t>(expiry_id),
                     refit_preparation_inputs(served->session()),
                     PreparedObservationPolicy::LegacyEssviCompatibility);
  if (!prepared.has_value()) {
    return fail(prepared.error());
  }

  VolaSession candidate = served->session().clone_for_refit();
  Result<FitDiag> fit_diag = candidate.apply_prepared_essvi_refit(*fitted_index, *prepared);
  if (!fit_diag.has_value()) {
    return fail(fit_diag.error());
  }

  SurfaceBuildAttemptReport attempt =
      completed_attempt_report(under, published_curve, candidate, cfg_.admission);
  SurfaceBuildReport report;
  report.primary_curve = published_curve;
  report.published_curve = published_curve;
  report.refit_expiry = expiry_id;
  report.source_quote_revision = chain.quote_revision();
  report.warm_started = true;
  const SurfaceAdmissionDecision admission = attempt.admission;
  if (!admission.admitted) {
    report.retained_last_known_good = true;
    report.attempts.push_back(std::move(attempt));
    last_attempt_report_ = std::move(report);
    return Err(ErrorCode::Unavailable,
               "PricerFitter::refit_expiry: candidate failed surface admission");
  }
  ++candidate_generation_;
  if (risk_purpose) {
    // Rule: a risk surface is NEVER republished on the mark-grade policy gate
    // alone — the candidate must independently clear the same oracle contract
    // fit() enforces (geometry + certification + §5.2 coverage merge).
    const FitQualityMode quality_mode = served->quality_mode();
    const RiskSurfaceValidationConfig validation_config = risk_validation_config(quality_mode);
    Result<ValidationDigest> checked = validate_risk_surface(candidate, validation_config);
    ValidationDigest digest;
    if (checked.has_value()) {
      digest = *checked;
    } else {
      digest.failures = ValidationFailure::InvalidDomain;
    }
    merge_session_failure_context(candidate.diagnostics(), digest);
    finalize_validation_digest(digest, validation_config);
    const AdmissionDecision risk_admission =
        decide_risk_surface_admission(digest, quality_mode, candidate_generation_,
                                      served->generation(), SurfaceFallback::LastKnownGood);
    risk_health_ = risk_admission.health;
    if (!risk_admission.publish_candidate) {
      report.retained_last_known_good = true;
      report.attempts.push_back(std::move(attempt));
      last_attempt_report_ = std::move(report);
      return Err(ErrorCode::Unavailable,
                 "PricerFitter::refit_expiry: candidate failed independent risk admission");
    }
  }

  attempt.stage = SurfaceBuildStage::Publication;
  report.attempts.push_back(std::move(attempt));
  report.published = true;
  const SliceContext refreshed_context = candidate.expiries()[*fitted_index];
  const FitQualityMode published_quality = served->quality_mode();
  std::shared_ptr<const FittedSurface> next_surface(new FittedSurface(
      std::move(candidate), served->purpose(), published_quality, candidate_generation_));
  std::optional<SurfaceBuildReport> next_published{report};
  std::optional<SurfaceBuildReport> next_attempt{std::move(report)};
  FitSnapshotProvenance provenance;
  provenance.chain_instance_id = chain.instance_id();
  provenance.board_revision = chain.quote_revision();
  provenance.uid = chain.uid();
  provenance.expiry_revisions.assign(current_revisions.begin(), current_revisions.end());
  std::optional<FitSnapshotProvenance> next_provenance{std::move(provenance)};

  if (risk_purpose) {
    risk_surface_ = std::move(next_surface);
    risk_provenance_ = std::move(next_provenance);
  } else {
    market_mark_surface_ = std::move(next_surface);
    market_mark_provenance_ = std::move(next_provenance);
    market_mark_health_.state = SurfaceState::Healthy;
    market_mark_health_.reasons = ValidationFailure::None;
    market_mark_health_.candidate_generation = candidate_generation_;
    market_mark_health_.served_generation = candidate_generation_;
  }
  published_report_ = std::move(next_published);
  last_attempt_report_ = std::move(next_attempt);

  return Ok(ExpiryRefitDiagnostics{expiry_id, chain.quote_revision(), published_curve.kind, true,
                                   refreshed_context.n_used, refreshed_context.n_dropped,
                                   std::optional<FitDiag>{*fit_diag}, admission});
}

Result<FitDiag> PricerFitter::refit_risk_slice(const OptionChain &chain, std::size_t slice_idx) {
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
    return Err(ErrorCode::Unavailable, "PricerFitter::refit_risk_slice: no admitted risk surface");
  }
  if (!risk_provenance_.has_value()) {
    return Err(ErrorCode::Unavailable,
               "PricerFitter::refit_risk_slice: fitted-chain provenance is unavailable");
  }
  if (chain.instance_id() != risk_provenance_->chain_instance_id ||
      chain.uid() != risk_provenance_->uid) {
    return Err(ErrorCode::InvalidArgument,
               "PricerFitter::refit_risk_slice: chain instance differs from fit");
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
    const RiskSurfaceValidationConfig validation_config = risk_validation_config(quality_mode);
    finalize_validation_digest(digest, validation_config);
    risk_health_ = decide_risk_surface_admission(digest, quality_mode, candidate_generation_,
                                                 prior_generation, SurfaceFallback::LastKnownGood)
                       .health;
  };

  const SliceContext &context = contexts[slice_idx];
  const Chain *updated_chain = nullptr;
  for (const Chain &candidate_chain : chain.underlying().chains) {
    if (std::fabs(candidate_chain.T - context.T) <= 1.0e-10 * std::max(1.0, context.T)) {
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
        *updated_chain, inputs.S, rate, inputs.cash_divs, inputs.now_ts_ns, inputs.deam);
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
    const double forward_shift = std::fabs(std::log(carry->forward / context.forward));
    if (!std::isfinite(forward_shift) || forward_shift > 1.0e-8) {
      retain_prior(ValidationFailure::StaleInput);
      return Err(ErrorCode::Unavailable,
                 "PricerFitter::refit_risk_slice: carry moved; full surface fit required");
    }

    const double df = std::exp(-rate * context.T);
    const AmericanCorrectionCaches deam_caches = inputs.use_deam_cache_for_fit
                                                     ? live_session.correction_caches_at(context.T)
                                                     : AmericanCorrectionCaches{};
    Result<ObsSet> observations = build_observations_european(
        *updated_chain, inputs.S, rate, context.forward, context.T, df, inputs.calib, deam_caches,
        inputs.deam.al_opts, inputs.deam.iv_tol, inputs.deam.iv_max_iter, inputs.deam.method);
    if (!observations.has_value()) {
      retain_prior(ValidationFailure::InsufficientData);
      return Err(std::move(observations).error());
    }
    // Fail-closed for EVERY method: a non-AndersenLake method has no audit and
    // can never certify (deam_inversion_certified). Node drops within the
    // configured cap are tolerated; beyond it the refit is refused.
    if (!deam_inversion_certified(observations->deam_audit,
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

  const RiskSurfaceValidationConfig validation_config = risk_validation_config(quality_mode);
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
  const AdmissionDecision admission =
      decide_risk_surface_admission(digest, quality_mode, candidate_generation_, prior_generation,
                                    SurfaceFallback::LastKnownGood);
  risk_health_ = admission.health;
  if (!admission.publish_candidate) {
    return Err(ErrorCode::Unavailable,
               "PricerFitter::refit_risk_slice: candidate failed independent admission");
  }

  const auto publish_start = Clock::now();
  FitSnapshotProvenance provenance;
  provenance.chain_instance_id = chain.instance_id();
  provenance.board_revision = chain.quote_revision();
  provenance.uid = chain.uid();
  provenance.expiry_revisions.assign(chain.expiry_quote_revisions().begin(),
                                     chain.expiry_quote_revisions().end());
  std::optional<FitSnapshotProvenance> next_provenance{std::move(provenance)};
  std::shared_ptr<const FittedSurface> next_surface(new FittedSurface(
      std::move(candidate), SurfacePurpose::Risk, quality_mode, candidate_generation_));
  risk_surface_ = std::move(next_surface);
  risk_provenance_ = std::move(next_provenance);
  timings_.incremental_publish_ms = elapsed_ms(publish_start);
  timings_.incremental_total_ms = elapsed_ms(incremental_start);
  return refit;
}

bool PricerFitter::is_v2_request() const noexcept {
  // Route between two coexisting fit contracts:
  //   * BRANCH dual mark/risk pipeline (independent risk oracle, refit_risk_slice)
  //   * MAIN single-surface transactional fit (FitAdmissionPolicy, refit_expiry)
  //
  // MERGE routing call. The two APIs collided on the *value* {Balanced,
  // MarketMarkAndRisk}: it was both the v2 default AND the shape of every legacy
  // PricerConfig, so a legacy `preset` (Fast/Robust/Accurate -> purpose Risk in
  // map_legacy_fit_preset) silently promoted main's mark-grade consumers
  // (corpus/populate/dispersion) into fail-closed RISK requests. That inverts
  // da718f7 (WP12), whose contract is explicit: the default serves a MARK and
  // strict risk admission is the opt-in.
  //
  // Resolution: the v2 request is opt-in *by naming it*. `quality_mode` and
  // `outputs` are optional; engaging either is the v2 signal. A legacy preset
  // never implicitly requests Risk — it supplies the work budget of whichever
  // field the v2 caller left unnamed (effective_request, below). A config that
  // names neither is main's single-surface world: preset-budgeted, selector-
  // routed, admitted by cfg_.admission (strict only via risk_admission_policy()),
  // served + incrementally refit through refit_expiry.
  //
  // The fail-closed risk contract is untouched: any request that DOES resolve to
  // a Risk output is still served only through the independent oracle, and still
  // refuses to substitute the market mark (surface() / value_chain, below).
  return cfg_.quality_mode.has_value() || cfg_.outputs.has_value();
}

PricerFitter::EffectiveRequest PricerFitter::effective_request() const noexcept {
  const LegacyPresetMapping legacy = map_legacy_fit_preset(cfg_.preset);
  // Legacy (v2-unnamed) request: ONE surface, published into the market_mark
  // slot and admitted by FitAdmissionPolicy — so reads (surface()/value_chain)
  // resolve there regardless of the mark-vs-risk admission consumer. The preset
  // keeps its §9 work budget.
  if (!is_v2_request()) {
    return EffectiveRequest{SurfaceOutputs::MarketMark, legacy.quality_mode};
  }
  // v2 request. An unnamed field falls back to the legacy preset's §9 mapping:
  // Hft is a market-mark request (never an implicit risk request), every other
  // preset's dual bundle carries that preset's quality budget.
  const SurfaceOutputs outputs = cfg_.outputs.value_or(legacy.purpose == SurfacePurpose::MarketMark
                                                           ? SurfaceOutputs::MarketMark
                                                           : SurfaceOutputs::MarketMarkAndRisk);
  return EffectiveRequest{outputs, cfg_.quality_mode.value_or(legacy.quality_mode)};
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

const std::optional<FitSnapshotProvenance> &PricerFitter::published_provenance() const noexcept {
  const SurfacePurpose purpose = has_output(effective_request().outputs, SurfacePurpose::Risk)
                                     ? SurfacePurpose::Risk
                                     : SurfacePurpose::MarketMark;
  return published_provenance(purpose);
}

const std::optional<FitSnapshotProvenance> &
PricerFitter::published_provenance(SurfacePurpose purpose) const noexcept {
  return purpose == SurfacePurpose::Risk ? risk_provenance_ : market_mark_provenance_;
}

Result<ChainValuation> PricerFitter::value_chain(const OptionChain &chain, OutputField fields,
                                                 unsigned n_threads) const {
  // Fail-closed purpose default: mirrors surface(). A caller that wants the
  // mark interpolant while risk is requested/unserved states so explicitly via
  // the SurfacePurpose::MarketMark overload.
  const SurfacePurpose purpose = has_output(effective_request().outputs, SurfacePurpose::Risk)
                                     ? SurfacePurpose::Risk
                                     : SurfacePurpose::MarketMark;
  return value_chain(chain, fields, purpose, n_threads);
}

Result<ChainValuation> PricerFitter::value_chain(const OptionChain &chain,
                                                 std::span<const OptionId> selected_ids,
                                                 OutputField fields, unsigned n_threads) const {
  const SurfacePurpose purpose = has_output(effective_request().outputs, SurfacePurpose::Risk)
                                     ? SurfacePurpose::Risk
                                     : SurfacePurpose::MarketMark;
  return value_chain(chain, selected_ids, fields, purpose, n_threads);
}

Result<ChainValuation> PricerFitter::value_chain(const OptionChain &chain, OutputField fields,
                                                 SurfacePurpose purpose, unsigned n_threads) const {
  return value_snapshot(chain, chain.snapshot(), fields, purpose, n_threads);
}

Result<ChainValuation> PricerFitter::value_chain(const OptionChain &chain,
                                                 std::span<const OptionId> selected_ids,
                                                 OutputField fields, SurfacePurpose purpose,
                                                 unsigned n_threads) const {
  ATX_TRY(ChainSnapshot snapshot, chain.snapshot(selected_ids));
  return value_snapshot(chain, std::move(snapshot), fields, purpose, n_threads);
}

Result<ChainValuation> PricerFitter::value_snapshot(const OptionChain &chain, ChainSnapshot snap,
                                                    OutputField fields, SurfacePurpose purpose,
                                                    unsigned n_threads) const {
  const FittedSurface *served =
      purpose == SurfacePurpose::Risk ? risk_surface_.get() : market_mark_surface_.get();
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
  const std::optional<FitSnapshotProvenance> &provenance = published_provenance(purpose);
  if (!provenance.has_value()) {
    return Err(ErrorCode::Unavailable,
               "PricerFitter::value_chain: fitted-chain provenance is unavailable");
  }
  const FitSnapshotProvenance &published = *provenance;
  if (chain.instance_id() != published.chain_instance_id || chain.uid() != published.uid) {
    return Err(ErrorCode::InvalidArgument,
               "PricerFitter::value_chain: chain instance differs from fit");
  }
  const VolaSession &sess = served->session();
  const double S = chain.spot();
  const double nan = std::numeric_limits<double>::quiet_NaN();

  ChainValuation val;
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
  const bool want_model = has(fields, OutputField::ModelIV) ||
                          has(fields, OutputField::ModelPrice) || has(fields, OutputField::Greeks);
  const bool want_side_bands = has(fields, OutputField::BidIV) || has(fields, OutputField::AskIV);

  // The per-side correction caches the fit built. Routing the bid/ask/mid IV
  // inversions through them replaces the cold per-residual Andersen-Lake solve
  // (12 BAW root-finds + sweeps + quadrature + cold polish) with the cached hot
  // path (Black-76 + one Chebyshev evaluation) — the SOTA American-IV method (a
  // fast surrogate in the root-find, not a pricer). A null cache for a side
  // transparently falls back to the cold path (bit-identical, just slower).
  struct alignas(64) LocalCounts {
    std::size_t bid_unset{0};
    std::size_t ask_unset{0};
    std::size_t bid_iv_fail{0};
    std::size_t ask_iv_fail{0};
  };
  // Resolve auto exactly once and use the same upper bound for per-worker state
  // and dispatch. The fixed executor may clamp this value downward to its pool
  // size, but never produces a worker id beyond this pre-sized local array.
  unsigned resolved_nt = nt == 0u ? atx_auto_worker_count() : nt;
  if (resolved_nt > n) {
    resolved_nt = static_cast<unsigned>(n);
  }
  // A canonical whole-chain snapshot is expiry-major; a selected-id snapshot
  // may contain shorter or single-row runs. Split each consecutive equal-T run
  // into bounded chunks: each task still hoists its carry/cache lookup, while a
  // dense OPRA expiry no longer becomes one indivisible straggler that idles the
  // other value-chain workers.
  constexpr std::size_t kValuationChunkRows = 128u;
  struct ValuationChunk {
    std::size_t begin{0u};
    std::size_t end{0u};
    double T{0.0};
  };
  std::vector<ValuationChunk> work_chunks;
  work_chunks.reserve(n);
  for (std::size_t begin = 0u; begin < n;) {
    const double T = snap.T[begin];
    std::size_t run_end = begin + 1u;
    while (run_end < n && snap.T[run_end] == T) {
      ++run_end;
    }
    for (std::size_t chunk_begin = begin; chunk_begin < run_end;
         chunk_begin += kValuationChunkRows) {
      work_chunks.push_back(
          ValuationChunk{chunk_begin, std::min(run_end, chunk_begin + kValuationChunkRows), T});
    }
    begin = run_end;
  }
  if (resolved_nt > work_chunks.size()) {
    resolved_nt = static_cast<unsigned>(work_chunks.size());
  }
  std::vector<LocalCounts> local_counts(want_side_bands ? std::max(1u, resolved_nt) : 0u);
  std::vector<double> band_model_iv;
  if (want_bands && !has(fields, OutputField::ModelIV)) {
    band_model_iv.assign(n, nan);
  }
  if ((want_model || want_bands) && !work_chunks.empty()) {
    pricing_executor().run_dynamic(
        work_chunks.size(), resolved_nt, [&](std::size_t chunk_index, unsigned worker_id) {
          const ValuationChunk &range = work_chunks[chunk_index];
          LocalCounts *counts = want_side_bands ? &local_counts[worker_id] : nullptr;
          if (!(range.T > 0.0) || !std::isfinite(range.T)) {
            for (std::size_t i = range.begin; i < range.end; ++i) {
              if (counts != nullptr && has(fields, OutputField::BidIV)) {
                ++counts->bid_unset;
              }
              if (counts != nullptr && has(fields, OutputField::AskIV)) {
                ++counts->ask_unset;
              }
            }
            return;
          }
          const std::size_t count = range.end - range.begin;
          std::span<double> iv_out;
          if (has(fields, OutputField::ModelIV)) {
            iv_out = std::span<double>{val.model_iv}.subspan(range.begin, count);
          } else if (want_bands) {
            iv_out = std::span<double>{band_model_iv}.subspan(range.begin, count);
          }
          const std::span<double> price_out =
              has(fields, OutputField::ModelPrice)
                  ? std::span<double>{val.model_price}.subspan(range.begin, count)
                  : std::span<double>{};
          const std::span<AmericanGreeks> greeks_out =
              has(fields, OutputField::Greeks)
                  ? std::span<AmericanGreeks>{val.greeks}.subspan(range.begin, count)
                  : std::span<AmericanGreeks>{};
          const Status model_status = sess.evaluate_ladder(
              range.T, std::span<const double>{snap.strike}.subspan(range.begin, count),
              std::span<const Side>{snap.side}.subspan(range.begin, count), iv_out, price_out,
              greeks_out);
          assert(model_status.has_value());
          (void)model_status;
          if (!want_bands) {
            return;
          }

          const VolaSession::ForwardCarry carry = sess.interp_forward(range.T);
          const CorrectionBlend call_correction = sess.correction_blend_at(range.T, Side::Call);
          const CorrectionBlend put_correction = sess.correction_blend_at(range.T, Side::Put);
          for (std::size_t i = range.begin; i < range.end; ++i) {
            const double K = snap.strike[i];
            const Side side = snap.side[i];
            if (!(K > 0.0) || !std::isfinite(K)) {
              if (counts != nullptr && has(fields, OutputField::BidIV)) {
                ++counts->bid_unset;
              }
              if (counts != nullptr && has(fields, OutputField::AskIV)) {
                ++counts->ask_unset;
              }
              continue;
            }
            const double model_iv =
                has(fields, OutputField::ModelIV) ? val.model_iv[i] : band_model_iv[i];
            const double model_seed = std::isfinite(model_iv) && model_iv > 0.0 ? model_iv : 0.0;
            const CorrectionBlend &correction =
                side == Side::Call ? call_correction : put_correction;
            const auto invert = [&](double price, double warm_start) {
              return american_implied_vol(price, S, K, range.T, carry.rate, carry.q_eff, side,
                                          correction, AmericanMethod::AndersenLake,
                                          sess.inputs().deam.iv_tol, sess.inputs().deam.iv_max_iter,
                                          std::nullopt, warm_start);
            };
            double band_seed = model_seed;
            // When all bands are requested the mid is the closest observed price to
            // the model mark. Solve it first and reuse its converged IV for both
            // spread-adjacent sides. If mid is absent/fails, retain the model seed;
            // do not synthesize mid work for a BidIV/AskIV-only request.
            if (has(fields, OutputField::MidIV)) {
              const double mid = snap.mid[i];
              if (mid > 0.0 && std::isfinite(mid)) {
                const auto iv = invert(mid, model_seed);
                if (iv.has_value()) {
                  val.mid_iv[i] = *iv;
                  band_seed = *iv;
                } else {
                  val.mid_iv[i] = nan;
                }
              }
            }
            if (has(fields, OutputField::BidIV)) {
              const double bid = snap.bid[i];
              if (!(bid > 0.0) || !std::isfinite(bid)) {
                ++counts->bid_unset;
              } else {
                const auto iv = invert(bid, band_seed);
                if (iv.has_value()) {
                  val.bid_iv[i] = *iv;
                } else {
                  ++counts->bid_iv_fail;
                }
              }
            }
            if (has(fields, OutputField::AskIV)) {
              const double ask = snap.ask[i];
              if (!(ask > 0.0) || !std::isfinite(ask)) {
                ++counts->ask_unset;
              } else {
                const auto iv = invert(ask, band_seed);
                if (iv.has_value()) {
                  val.ask_iv[i] = *iv;
                } else {
                  ++counts->ask_iv_fail;
                }
              }
            }
          }
        });
  }

  for (const LocalCounts &counts : local_counts) {
    val.n_bid_unset += counts.bid_unset;
    val.n_ask_unset += counts.ask_unset;
    val.n_bid_iv_fail += counts.bid_iv_fail;
    val.n_ask_iv_fail += counts.ask_iv_fail;
  }
  return Ok(std::move(val));
}

} // namespace atx::vol
