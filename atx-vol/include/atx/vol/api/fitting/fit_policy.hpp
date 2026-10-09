#pragma once

// Unified cold-fit policy selection.  This is the single seam between caller
// intent, observable board features, the seven underlier profiles, fit presets,
// and curve families.  It deliberately performs no de-Americanization or curve
// fitting: high-confidence boards route in O(number of quotes), while ambiguous
// boards ask PricerFitter to run the more expensive held-out selector.

#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string_view>

#include "atx/vol/api/fitting/profile.hpp"
#include "atx/vol/api/fitting/session.hpp"
#include "atx/vol/api/fitting/vol_curve.hpp"

namespace atx::vol {

// Distinct near-money strikes a single expiry must carry before C8's eight free
// parameters are identified on it: eight to pin them plus two so the slice is
// fitted rather than interpolated.
inline constexpr std::uint32_t kC8MinSliceStrikes = 10u;

enum class FitSelectionMode : std::uint8_t {
  Auto = 0,           // profile-first; held-out validation only when ambiguous
  CrossValidated = 1, // always run the held-out curve selector
};

enum class MarketSessionPhase : std::uint8_t {
  Unknown = 0,
  Opening = 1, // opening auction / first minutes: incomplete, rapidly moving board
  Continuous = 2,
  Closing = 3, // final minutes / closing-auction transition
};

enum class EventPhase : std::uint8_t {
  None = 0,
  PreAnnouncement = 1,
  PostAnnouncement = 2,
};

enum class FitDecisionSource : std::uint8_t {
  ProfileOverride = 0,
  TickerPrior = 1,
  BoardFeatures = 2,
  SparseGuard = 3,
  CrossValidation = 4,
};

// Optional facts not recoverable reliably from one OPRA snapshot.  A feed
// handler/calendar service can populate these without changing the fit API.
struct FitContext {
  std::optional<ProfileKind> profile_override{};
  MarketSessionPhase session_phase{MarketSessionPhase::Unknown};
  EventPhase event_phase{EventPhase::None};
  std::optional<std::uint16_t> event_distance_days{};
  std::optional<double> forward_dispersion_bp{};
  std::optional<double> median_q_eff{};
  std::optional<bool> htb{};
  bool vol_product{false};
};

struct FitPolicyConfig {
  FitSelectionMode mode{FitSelectionMode::Auto};
  // A low-confidence classification is validated out of sample instead of being
  // routed on directly.
  //
  // Read against `ProfileVerdict::confidence`, which is ORDINAL: 1.0 when every
  // classifier axis picked the same rung of the liquidity ladder, then 0.75,
  // 0.5, 0.25, 0.0 as the axes spread out over it. 0.70 therefore means
  // "cross-validate once the axes disagree by two rungs or more", and it sits in
  // the middle of the empty band between 0.5 and 0.75 -- any value in (0.5,
  // 0.75] expresses the same policy, so the gate cannot be decided by a
  // hair's-breadth move in one observable.
  //
  // It used to be read against the modal bucket's VOTE SHARE, which on the
  // measured corpus took only the values 0, 1/3, 1/2, 2/3 and 1. 0.70 fell into
  // the 0.667|0.800 gap with 20% of all boards sitting exactly on 0.667, so a
  // single vote decided the route -- and because the fitter substitutes the
  // caller's preset for the profile's own on the cross-validated path, that one
  // vote could change the curve family actually served.
  double min_direct_confidence{0.70};
  bool validate_ambiguous{true};
  // Dead-board backstop for the identifiability demotion: the minimum number of
  // two-sided NEAR-THE-MONEY legs (|ln(K/S)| <= kNearMoneyLogMoneyness) a board
  // must carry before its own vote is trusted.
  //
  // NOTE THE UNIT. This was a floor of 600 on the board's TOTAL two-sided leg
  // count, which is a strike-count test wearing a liquidity test's name: it
  // demoted 49% of the lqbench universe and 8.7% of the S&P 100 -- DUK, KHC,
  // SYK, AMT, CMCSA, T, CL, SO, MO, BMY, dividend-heavy mega caps with short
  // strike ladders. Counted near the money the same names clear it by an order
  // of magnitude (the thinnest S&P 100 board carries 121 near-money nodes), and
  // the structural arm below does the discriminating.
  //
  // Zero disables the demotion outright -- the documented "route on the board's
  // own vote whatever the board looks like" switch.
  std::uint32_t sparse_validation_floor{24};
  // How many expiries must independently identify a smile (each carrying at
  // least `kMinIdentifiableSliceStrikes` distinct near-money strikes) before the
  // board is routed on its own vote. One is enough to fit a slice; the calendar
  // dimension is the fitter's problem, not the classifier's.
  std::uint32_t min_identifiable_expiries{1};
  // Adaptive knot budget for the dense index/ETF HFT route. The cap is a
  // calibration input rather than a curve-local field, so PricerFitter applies it
  // when materializing SessionInputs -- and an explicit PricerConfig
  // ::max_obs_per_slice overrides it.
  std::uint32_t dense_node_cap{48};
};

enum class SurfaceConsumer : std::uint8_t {
  Mark = 0,
  Quote = 1,
  Risk = 2,
};

enum class SurfaceAdmissionReason : std::uint8_t {
  None = 0,
  BuildFailed = 1,
  InsufficientFittedExpiries = 2,
  InsufficientExpiryCoverage = 3,
  InsufficientQuoteCoverage = 4,
  FrontExpiryMissing = 5,
  ConsecutiveExpiryGap = 6,
  NonFiniteDiagnostics = 7,
  CalendarArbitrage = 8,
  QualityBelowFloor = 9,
  ImpossibleEvidence = 10,
  DuplicateMaturity = 11,
  FiniteIvDomain = 12,
  EuropeanPriceBounds = 13,
  StrikeMonotonicity = 14,
  StrikeConvexity = 15,
  CalendarTotalVariance = 16,
  ForwardVariance = 17,
  RequiredTenorBucket = 18,
  DiagnosticsUnavailable = 19,
};

// Default is the Mark-serving contract (WP12 staging): it admits the healthy
// real-world surfaces a mark consumer serves -- dense ETF/index and event boards
// whose LinearVariance route carries genuine (non-arb-free) calendar structure,
// and breadth boards that drop thin expiries -- while STILL rejecting garbage.
// The numerical-sanity gates are consumer-independent and stay on: finite IV
// domain, European price bounds, impossible/self-contradictory evidence, duplicate
// maturities, and non-finite diagnostics. A mark surface serves whatever slices
// fit, so the structural gates (full expiry coverage, front-expiry present, no
// consecutive gaps, calendar no-arbitrage, and the strike/calendar shape
// invariants) are the mark-vs-risk difference and are relaxed here. Strict risk
// admission -- the pre-WP12 contract -- is available verbatim via
// `risk_admission_policy()` and must be requested explicitly.
struct FitAdmissionPolicy {
  bool enabled{true};
  SurfaceConsumer consumer{SurfaceConsumer::Mark};
  std::size_t min_fitted_expiries{1u};
  double min_expiry_coverage{0.0};
  double min_quote_coverage{0.0};
  bool require_front_expiry{false};
  std::size_t max_consecutive_expiry_gaps{std::numeric_limits<std::size_t>::max()};
  bool require_calendar_arb_free{false};
  // Worst per-expiry fraction of scored quotes repriced inside bid/ask that a
  // surface must clear. T6: 0.0 leaves this gate DISARMED, and disarmed is not a
  // low bar -- it is NO bar. The predicate `evaluate_surface_admission` runs is
  // `worst_frac_within_bidask < min_worst_frac_within_bidask`, and
  // `worst_frac_within_bidask` is a FRACTION in [0, 1], so at 0.0 the comparison
  // is `worst < 0.0`: unsatisfiable. `QualityBelowFloor` cannot fire, and no
  // other admission path compares an RMSE to a tolerance either (grep confirms:
  // `rmse` appears nowhere in fit_policy.cpp or this header), while the
  // independent geometry oracle never reads a bid or an ask at all. An arb-free
  // surface that reprices NOTHING inside the spread therefore publishes clean.
  //
  // `fit_quality_floor_armed` below is the predicate a caller reads to find out
  // whether the gate can fire at all, and two static_asserts pin the shipped
  // defaults as disarmed so arming one becomes a deliberate, reviewed edit
  // rather than a silent number change.
  //
  // WHAT A REAL FLOOR WOULD BE (not armed here; arming it changes which
  // production surfaces publish and needs its own measured rollout):
  //   * the only non-zero value anywhere in the repo is populate's 0.35
  //     (`kPopulateMinWorstFracInBand`, src/storage/surface_db_populate.cpp) --
  //     a WORST-EXPIRY floor, so it is already the conservative statistic;
  //   * the floor must key on the consumer. A Mark surface exists to interpolate
  //     quotes and should clear a high fraction; a Risk surface trades in-band
  //     fit for arb-freeness by construction and would be refused by the same
  //     number, so one constant cannot serve both;
  //   * it needs the diagnostic to exist: `fit_admission_consumes_parity`
  //     already makes an armed floor fail closed on missing parity evidence,
  //     which means arming it ALSO turns on the second de-Am scoring pass and
  //     its latency cost on every route that had opted out;
  //   * and it needs a measured distribution of `worst_frac_within_bidask` over
  //     a real board population before a number is chosen -- picking one without
  //     that is how a gate starts refusing surfaces that were always fine.
  double min_worst_frac_within_bidask{0.0};
  bool require_short_tenor{false};
  bool require_medium_tenor{false};
  bool require_long_tenor{false};
};

// T6: whether the bid/ask quality floor can fire AT ALL. A floor of 0.0 makes
// the admission predicate `worst < 0.0`, which no fraction in [0, 1] satisfies,
// so the gate is not "lenient" -- it is absent. Out-of-range floors (negative,
// > 1, non-finite) are rejected by `evaluate_surface_admission` on their own and
// are not "armed" either. Named so a caller can ask the question instead of
// inferring it from a number, and so the two shipped defaults can be pinned.
[[nodiscard]] constexpr bool
fit_quality_floor_armed(const FitAdmissionPolicy &policy) noexcept {
  return policy.enabled && policy.min_worst_frac_within_bidask > 0.0 &&
         policy.min_worst_frac_within_bidask <= 1.0;
}

// Whether admission requires the re-Americanized fit diagnostic pass. Quote
// and Risk fail closed without diagnostics. Mark can omit them only when its
// bid/ask quality floor is disabled; an invalid floor fails independently and
// therefore does not need diagnostic evidence to reject.
[[nodiscard]] constexpr bool
fit_admission_consumes_parity(const FitAdmissionPolicy &policy) noexcept {
  if (!policy.enabled) {
    return false;
  }
  if (policy.consumer != SurfaceConsumer::Mark) {
    return true;
  }
  return fit_quality_floor_armed(policy);
}

// The strict risk-serving contract: every attempted expiry fitted, the front
// expiry present, no consecutive expiry gaps, calendar no-arbitrage, and the full
// Risk-consumer strike/calendar shape invariants (via consumer=Risk in
// evaluate_surface_admission). This is the pre-WP12 default, kept intact for
// callers that need a risk-grade surface; opt in explicitly by assigning it to
// `PricerConfig::admission`.
[[nodiscard]] constexpr FitAdmissionPolicy risk_admission_policy() noexcept {
  FitAdmissionPolicy policy;
  policy.consumer = SurfaceConsumer::Risk;
  policy.min_expiry_coverage = 1.0;
  policy.require_front_expiry = true;
  policy.max_consecutive_expiry_gaps = 0u;
  policy.require_calendar_arb_free = true;
  return policy;
}

// T6: the shipped defaults do NOT arm the bid/ask quality floor. That is a
// recorded fact, not an aspiration -- both surfaces below publish on geometry
// and coverage alone, and neither compares a reprice quality to a tolerance. If
// you are here because one of these fired, you are arming a gate that will start
// REFUSING production surfaces: read `min_worst_frac_within_bidask`'s contract
// above, measure the distribution first, and change the assert deliberately.
// `fit_quality_floor_armed` returns false for an OUT-OF-RANGE floor as well as
// for an unarmed one, so on its own it cannot tell "no floor" from "a floor
// nothing can satisfy". A shipped default of 1.5 (or NaN) would pass both
// asserts below while `evaluate_surface_admission` refused every surface -- the
// exact failure these asserts exist to prevent. Pin the value, not the predicate.
static_assert(FitAdmissionPolicy{}.min_worst_frac_within_bidask == 0.0,
              "the default Mark bid/ask floor must be exactly 0.0 (unarmed); an "
              "out-of-range value reads as 'unarmed' to fit_quality_floor_armed but "
              "refuses every surface at admission");
static_assert(!fit_quality_floor_armed(FitAdmissionPolicy{}),
              "the default Mark admission policy publishes with NO bid/ask quality floor; "
              "arming it changes which production surfaces publish (see T6)");
static_assert(!fit_quality_floor_armed(risk_admission_policy()),
              "risk_admission_policy() publishes with NO bid/ask quality floor; "
              "arming it changes which production surfaces publish (see T6)");

struct SurfaceAdmissionEvidence {
  std::size_t attempted_expiries{0u};
  std::size_t fitted_expiries{0u};
  std::size_t attempted_quotes{0u};
  std::size_t fitted_quotes{0u};
  bool front_expiry_fitted{false};
  std::size_t max_consecutive_expiry_gaps{0u};
  bool finite_diagnostics{false};
  bool calendar_arb_free{false};
  double worst_frac_within_bidask{0.0};
  bool duplicate_maturities{false};
  bool finite_iv_domain{false};
  bool european_price_bounds{false};
  bool strike_monotone{false};
  bool strike_convex{false};
  bool calendar_total_variance{false};
  bool forward_variance_nonnegative{false};
  bool has_short_tenor{false};
  bool has_medium_tenor{false};
  bool has_long_tenor{false};
  double invariant_grid_k_min{0.0};
  double invariant_grid_k_max{0.0};
  std::size_t invariant_grid_points{0u};
  SurfaceAdmissionReason first_invariant_failure{SurfaceAdmissionReason::None};
  std::optional<double> first_failure_maturity{};
  std::optional<double> first_failure_log_moneyness{};
  std::optional<double> first_failure_value{};
  ParityDiagnosticState parity_state{ParityDiagnosticState::NotScored};
};

struct SurfaceAdmissionDecision {
  bool admitted{false};
  SurfaceAdmissionReason primary_reason{SurfaceAdmissionReason::None};
  std::uint32_t failed_checks{0u};
};

[[nodiscard]] constexpr std::uint32_t
surface_admission_reason_mask(SurfaceAdmissionReason reason) noexcept {
  const auto bit = static_cast<std::uint8_t>(reason);
  return bit == 0u || bit > 32u ? 0u : (std::uint32_t{1u} << (bit - 1u));
}

[[nodiscard]] constexpr bool has_admission_failure(const SurfaceAdmissionDecision &decision,
                                                   SurfaceAdmissionReason reason) noexcept {
  return (decision.failed_checks & surface_admission_reason_mask(reason)) != 0u;
}

// Pure family-neutral gate over evidence produced by a completed fit. All failed
// predicates are retained; `primary_reason` follows the enum's stable priority.
[[nodiscard]] SurfaceAdmissionDecision
evaluate_surface_admission(const SurfaceAdmissionEvidence &evidence,
                           const FitAdmissionPolicy &policy) noexcept;

struct FitDecision {
  ClassifierInputs features{};
  ProfileVerdict profile{};
  FitDecisionSource source{FitDecisionSource::BoardFeatures};
  FitPreset preset{FitPreset::Fast};
  CurveConfig curve{};
  CurveConfig primary_curve{};
  bool needs_cross_validation{false};
  bool used_fallback{false};
  // The held-out selector was asked for a family and REFUSED (too thin a
  // held-out sample, no admissible candidate, budget exhausted), so `curve`
  // came from the profile's direct route instead. Cross-validation is advisory
  // among already-admissible families, never a veto on the board -- but the
  // substitution must not be silent, so it is reported here. Distinct from
  // `used_fallback`, which records a fallback-LADDER rung taken after a BUILD
  // failed; the two can both be set on one board.
  bool selector_fallback{false};
};

[[nodiscard]] FitDecision select_fit_policy(const Underlying &under, std::string_view ticker,
                                            const FitContext &context = {},
                                            const FitPolicyConfig &config = {}) noexcept;

} // namespace atx::vol
