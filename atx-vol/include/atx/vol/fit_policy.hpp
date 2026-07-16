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

#include "atx/vol/profile.hpp"
#include "atx/vol/session.hpp"
#include "atx/vol/vol_curve.hpp"

namespace atx::vol {

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
  // A low-confidence liquid/ordinary classification is validated out of sample.
  double min_direct_confidence{0.70};
  bool validate_ambiguous{true};
  // Below this many live quote legs a board is "thin", with two consequences: a
  // board-voted verdict cannot support a useful even/odd holdout (route it
  // directly to the parsimonious SVI family instead of paying for a doomed
  // validation pass), and no board -- however confidently classified -- can
  // identify C8's eight free parameters.
  std::uint32_t sparse_validation_floor{600};
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
  double min_worst_frac_within_bidask{0.0};
  bool require_short_tenor{false};
  bool require_medium_tenor{false};
  bool require_long_tenor{false};
};

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
  return policy.min_worst_frac_within_bidask > 0.0 && policy.min_worst_frac_within_bidask <= 1.0;
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
};

[[nodiscard]] FitDecision select_fit_policy(const Underlying &under, std::string_view ticker,
                                            const FitContext &context = {},
                                            const FitPolicyConfig &config = {}) noexcept;

} // namespace atx::vol
