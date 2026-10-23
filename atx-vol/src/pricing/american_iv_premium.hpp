#pragma once

// De-Americanised (early-exercise-premium) American IV inversion with per-contract
// memory (lane L2e, 2026-10-19). Internal header (src/pricing); the public entry is
// IvOpts::strategy = IvStrategy::Premium in american_iv.hpp, whose contract this is.
//
// A caller that inverts several prices of ONE contract (the de-Am IV cache inverts a
// quote's mid, bid and ask) may pass an IvPremiumMemory: the cold Andersen-Lake points
// of an earlier inversion fix the remainder D = P_am - A of the seed leg and its sigma
// chord, so a later price of the same contract usually needs one cold solve (seed from
// the chord, accept with the chord of that solve and the remembered point). The memory
// only moves the seed: acceptance, tolerance and fallback are the same, so an answer
// differs by at most the accepted tolerance with or without it. Callers that need
// order-independent answers pass each later price a COPY of one fixed earlier
// inversion's memory (the cache: the mid's). It must only be shared between calls
// with identical S, K, T, r, q, side, AlOpts and seed_profile.

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>

#include "atx/vol/api/core/types.hpp"
#include "atx/vol/api/pricing/american_iv.hpp"
#include "pricing/american_family_error.hpp" // RootAcceptance (lane K3)
#include "pricing/american_iv_batch_status.hpp" // K2: the native attempt's typed outcome

namespace atx::vol {

// Why a Premium inversion handed the price to the Search route (None: accepted).
enum class IvPremiumFallback : std::uint8_t {
  None = 0,           // the premium root was accepted
  NotApplicable = 1,  // route Premium does not cover, or inputs the Search route refuses
  RangeEdge = 2,      // price at the identifiable-range edges or in the non-monotone band
  NoEuropeanSeed = 3, // no European IV for the quote or for its de-Americanised target
  NotIncreasing = 4,  // a cold-map chord that is not a usable increasing slope
  VolBound = 5,       // an iterate or the root at/below kIvMin or above the search ceiling
  PricerFailure = 6,  // the Andersen-Lake kernel refused at an iterate
  NoConvergence = 7,  // not accepted within kIvPremiumMaxSolves cold solves
};
inline constexpr std::size_t kIvPremiumFallbackCount = 8;

// K4 (r6, 2026-10-23): the typed outcome of one request on the native fit route
// (IvPremiumGate::native_route). The route never runs Search: every request ends in exactly
// one of these. The non-identified outcomes map onto C0's identifiability statuses
// (quote_slice.hpp QuoteSlicePriceOnlyReason): PriceOnlyPlateau -> flat_exercise_region,
// PriceOnlyFloor -> floor_vol, PriceOnlyLowVega -> low_vega, Interval ->
// unidentified_interval; BudgetExhausted / AboveRange / Unresolved are typed refusals.
enum class IvRouteOutcome : std::uint8_t {
  None = 0,             // not on the native route
  NativeRoot = 1,       // a family-native root the K3 acceptance closed (Skipped / Confirmed)
  ReferenceRoot = 2,    // a root the bounded reference route certified (bracket <= the width)
  LegRoot = 3,          // a request with no family of its side: the seed-leg route's root
  PriceOnlyPlateau = 4, // the price lies on the exercise plateau (C1 ExercisePlateau class)
  PriceOnlyFloor = 5,   // at or below the kIvMin price outside the plateau
  PriceOnlyLowVega = 6, // the slope cannot resolve the width (K3 PriceOnly, K2 LowVega)
  Interval = 7,         // a certified bracket wider than the requirement (K3 Interval)
  BudgetExhausted = 8,  // the chain's reference budget is spent: refused and counted
  AboveRange = 9,       // at or above the identifiable range's top
  Unresolved = 10,      // the reference kernel refused, or no bracket closed within its cap
};
inline constexpr std::size_t kIvRouteOutcomeCount = 11;

// The reference route's budget of one chain cache (QuoteSliceChannelConfig
// deam_reference_max_calls / _max_evaluations), spent by the requests that reach the route
// in the order the fit asks for them (deterministic). Not thread-safe: one chain cache is
// filled by one thread at a time (rung_memo.hpp).
struct IvReferenceBudget {
  std::uint32_t max_calls = 0;
  std::uint32_t max_evaluations = 0;
  std::uint32_t calls = 0;
  std::uint32_t evaluations = 0;
  std::uint32_t refused = 0; // requests refused because the budget was spent
};

// Kernel evaluations one reference request may spend (its own cap inside the budget).
inline constexpr std::uint16_t kIvReferenceMaxEvaluations = 16;

// The native fit route's outcome of a family-native root the K3 acceptance left unaccepted
// (review-K3 For K4): PriceOnly -> PriceOnlyLowVega and Interval -> Interval, typed and NOT
// re-solved (C0's low_vega / iv_interval); None (no bracket closed) -> None, which sends the
// root to the bounded reference route. Confirmed / Skipped are accepted roots (None here).
[[nodiscard]] constexpr IvRouteOutcome native_unaccepted_outcome(RootAcceptanceKind kind) noexcept {
  switch (kind) {
  case RootAcceptanceKind::PriceOnly:
    return IvRouteOutcome::PriceOnlyLowVega;
  case RootAcceptanceKind::Interval:
    return IvRouteOutcome::Interval;
  case RootAcceptanceKind::None:
  case RootAcceptanceKind::Confirmed:
  case RootAcceptanceKind::Skipped:
    break;
  }
  return IvRouteOutcome::None;
}

// Cold iterate solves one Premium attempt may spend before it falls back (plus one
// accurate confirming solve when IvOpts::seed_profile selects another kernel), and the
// extra solves it may add one at a time while the Newton distance keeps shrinking by at
// least kIvPremiumShrink per solve (SPY graded-v1: 392 of the 395 budget fallbacks were
// still converging geometrically at the 4th solve).
inline constexpr std::uint32_t kIvPremiumMaxSolves = 4;
inline constexpr std::uint32_t kIvPremiumExtraSolves = 2;
inline constexpr double kIvPremiumShrink = 0.25;
// Confirming solves of a seed-tier root on the confirm kernel (SPY1s Phase 1): the first
// confirms when its Newton distance is <= tol; otherwise its Newton step is taken and one
// more solve confirms (a Screen root sits up to 7.8e-4 max(P, 1) off the accurate map).
inline constexpr std::uint32_t kIvPremiumConfirmSolves = 2;
// Fused family evaluations (price + vega) one family-native attempt
// (IvPremiumGate::qfamily_native) may spend before it hands the price, with its typed
// outcome and retained bracket, to the seed-leg path (K2 r5: the batch seam's lane cap,
// iv_batch::kInverseMaxEvaluations; was 12 family prices with a chord counting two).
inline constexpr std::uint32_t kIvPremiumNativeEvaluations = 16;

// K2 (r5, 2026-10-22): what a family-native attempt learned about ONE contract on the
// family map — the bracket [lo, hi] with the family prices at its ends (NaN: an
// unevaluated end), its last evaluated point and the fused vega there. Written by EVERY
// native attempt, accepted or not, and never by the seed-leg path, so a quote's bid and ask
// start from the mid's bracket and a Newton seed off its last point even when the mid's
// attempt handed the price to another route. stream_id: the recorded request that wrote it
// (pricing/american_iv_stream.hpp; 0xFFFFFFFF = none, when no stream is armed).
struct IvPremiumNativeCarry {
  bool valid = false;
  double lo = std::numeric_limits<double>::quiet_NaN();
  double hi = std::numeric_limits<double>::quiet_NaN();
  double p_lo = std::numeric_limits<double>::quiet_NaN();
  double p_hi = std::numeric_limits<double>::quiet_NaN();
  double sigma = std::numeric_limits<double>::quiet_NaN();
  double price = std::numeric_limits<double>::quiet_NaN();
  double vega = std::numeric_limits<double>::quiet_NaN();
  std::uint32_t stream_id = 0xFFFFFFFFu;
};

// The cold Andersen-Lake points of one contract: sigma and the remainder D = P_am - A
// of the seed leg A (Barone-Adesi-Whaley where the contract has an exercise region,
// Black-76 elsewhere; american_iv.cpp `Leg`). `native` (SPY1s Phase 3): the points are
// family-native ones (IvPremiumGate::qfamily_native), whose remainder is the family price
// itself (no leg); a reader of the other kind ignores them.
struct IvPremiumMemory {
  std::array<double, 2> sigma{};
  std::array<double, 2> rest{};
  std::uint8_t points = 0; // valid entries, most recent first
  bool native = false;
  IvPremiumNativeCarry carry{}; // K2: the family-native attempts' retained bracket
};

struct IvPremiumStats {
  std::uint32_t premium_solves = 0; // cold Andersen-Lake solves of the Premium attempt
  IvPremiumFallback fallback = IvPremiumFallback::NotApplicable;
  // Accepted roots: |P_am - price| (USD) at the last accurate solve before the final
  // Newton correction, an upper bound on the served root's own residual.
  double last_residual_usd = 0.0;
  // Seed-tier wiring (IvOpts::seed_profile; SPY1s Phase 1, 2026-10-20). Of
  // premium_solves: the confirming solves on the confirm kernel (at most
  // kIvPremiumConfirmSolves; lane K3's bracket confirm spends up to three) and the band
  // gate's chord solve on the seed tier (0 or 1); the rest are iterates. seed_nodes: the
  // boundary node count of the scheme the iterates ran on (al_screen_opts() 7; the
  // accurate preset 12). confirm_profile: the kernel profile of the confirming solve when
  // one ran. seed_routed: the price lay inside the seed tier's non-monotone gate, so the
  // iterates ran on the accurate kernel and no confirm was needed. confirm_skipped: the
  // band gate (IvPremiumGate) accepted the seed-tier root without a confirm.
  // gate_bound_vol: the vol bound a skipped root carries (NaN otherwise): the band gate's
  // (eps_A + eps_r) / v_min, or (lane K3) the width of the bracket of the target map's root
  // the family envelope enclosed (acceptance.bound_iv).
  std::uint32_t confirm_solves = 0;
  std::uint32_t gate_solves = 0;
  std::uint16_t seed_nodes = 0;
  AlProfile confirm_profile = AlProfile::Reference;
  bool seed_routed = false;
  bool confirm_skipped = false;
  double gate_bound_vol = std::numeric_limits<double>::quiet_NaN();
  // Family iterates (IvPremiumGate::family; SPY1s Phase 2): iterates whose cold point was
  // the family D-model (no Andersen-Lake solve; not in premium_solves), and whether a
  // family refusal sent the rest of the attempt's iterates to the seed kernel.
  std::uint32_t family_prices = 0;
  bool family_refused = false;
  // K2 (r5, 2026-10-22): the family-native attempt (IvPremiumGate::qfamily_native) through
  // the batch seam (pricing/american_iv_batch.hpp): its typed outcome (Inactive: it did not
  // run; Converged with the root confirmed as before; any other value is WHY the price went
  // to the seed-leg path - converged without a usable slope (ConvergedNoSlope, review-K2
  // m1), low vega, exercise-flat, sigma below / above the box, q outside the box,
  // nonconverged, invalid input), its fused evaluations (also counted in
  // family_prices), its iv_batch::lane_flag bits, and whether it started from the
  // contract's retained carry (a bid / ask behind its mid).
  iv_batch::InverseStatus native_status = iv_batch::InverseStatus::Inactive;
  std::uint16_t native_evaluations = 0;
  std::uint8_t native_flags = 0;
  bool native_carried = false;
  // How the root was accepted (lane K3; american_family_error.hpp): a typed record for every
  // root of the (sigma, q) family route and for a root the legacy band / never policies skip
  // (kind None otherwise). native_partner_span: on the family-native route, the sigma span of
  // the slope the iterate accepted with (R1-A MJ1: a secant that may be far from local;
  // acceptance no longer uses it) - 0 since K2's batch seam (the fused vega at the lane's
  // last point, a tangent) - NaN elsewhere.
  RootAcceptance acceptance{};
  double native_partner_span = std::numeric_limits<double>::quiet_NaN();
  // K4 (r6): the native fit route's typed outcome (None off the route), the bounded
  // reference route's kernel evaluations (also in premium_solves), the bracket or interval
  // that typed outcome carries (NaN: an unknown end; [route_lo, route_hi] of a
  // ReferenceRoot is its certified bracket, of an Interval its certified bracket, of a
  // PriceOnly outcome the volatilities consistent with the price where known), and the
  // exercise class the reference kernel saw at its last evaluation (american_region.hpp;
  // meaningful only when reference_evaluations > 0).
  IvRouteOutcome route = IvRouteOutcome::None;
  std::uint16_t reference_evaluations = 0;
  double route_lo = std::numeric_limits<double>::quiet_NaN();
  double route_hi = std::numeric_limits<double>::quiet_NaN();
  std::uint8_t reference_class = 0;
};

// Tested price bounds of the seed tiers versus the accurate Reference kernel (american.hpp
// al_screen_opts() / AlProfile::Fast; pinned by AlScreenProfile.* and AlFastProfile.* in
// tests/american_test.cpp): |dP| <= bound * max(P, 1). Screen: 7.8e-4 on the whole
// grid, 4.6e-6 for T <= 0.1. Fast: 1e-12. Reference: 0 (it IS the accurate kernel).
inline constexpr double kAlScreenPriceBound = 7.8e-4;
inline constexpr double kAlScreenShortDatedPriceBound = 4.6e-6;
inline constexpr double kAlScreenShortDatedYears = 0.1;
inline constexpr double kAlFastPriceBound = 1.0e-12;
inline constexpr std::uint16_t kAlAccurateCollocation = 12; // the nullopt preset's nodes

[[nodiscard]] inline double al_profile_price_bound(AlProfile profile, double price,
                                                   double years) noexcept {
  const double scale = price > 1.0 ? price : 1.0;
  switch (profile) {
  case AlProfile::Reference:
    return 0.0;
  case AlProfile::Fast:
    return kAlFastPriceBound * scale;
  case AlProfile::Screen:
    return (years <= kAlScreenShortDatedYears ? kAlScreenShortDatedPriceBound
                                              : kAlScreenPriceBound) *
           scale;
  }
  return kAlScreenPriceBound * scale; // unreachable: every enumerator returns above
}

// Confirm policy of a seed-tier Premium root (research R2 / gap report 5a; SPY1s Phase
// 1). Always: every accepted root is confirmed by one solve on the confirm kernel (the
// historical seed_profile behaviour). Band: the confirm is skipped only when the
// acceptance statement (eps_A + eps_r) / v_min < half_spread_usd / k holds, with eps_A
// the seed tier's tested price bound at this contract, eps_r the root's own residual on
// the seed map, v_min the seed map's chord slope at the root less its chord bound,
// floored at min_relative_slope * max(price, 1); a non-positive v_min, a price inside
// the seed tier's non-monotone gate, a non-positive half-spread or a root at the
// volatility bounds always confirms. Never: the seed-tier root is accepted as is
// (measurement arm only; never a candidate default).
enum class IvConfirmPolicy : std::uint8_t { Always = 0, Band = 1, Never = 2 };

struct IvPremiumGate {
  IvConfirmPolicy policy = IvConfirmPolicy::Always;
  double k = 10.0;                 // > 0
  double half_spread_usd = 0.0;    // the quote's half-spread in price (<= 0: confirm)
  double min_relative_slope = 0.0; // floor on v_min relative to max(price, 1)
  // The confirming solve's kernel: nullopt = opts.al_opts (the accurate kernel), else
  // this scheme (the R3 measurement arm passes al_fast_profile(opts.al_opts)).
  std::optional<AlOpts> confirm_opts = std::nullopt;
  // Seed-tier routing gate (SPY1s Phase 2, opt-in): > 0 routes a price to the accurate
  // iterates only when price - zero_vol <= seed_gate_kappa * K * max(1, T^2) instead of
  // inside the seed tier's own non-monotone gate (al_non_monotone_gate of the seed opts;
  // Screen 4e-4, about 100x the dip L2f measured, 4.3e-6). 0 (the default): the seed
  // tier's own gate, bit for bit. Every root outside the gate is still confirmed (or
  // gated) on the confirm kernel exactly as before; only which iterates run changes.
  double seed_gate_kappa = 0.0;
  // Kernel-free family iterates (SPY1s Phase 2, opt-in; policy Always only - the band
  // gate's bound is the seed TIER's, not the family's). When `family` is set (the
  // contract's side, built at yield `family_q`), every seed-tier iterate's cold point is
  // the D-model A(sigma; q) + D(sigma), D = P_family(sigma; family_q) - A(sigma;
  // family_q), A the seed leg (Barone-Adesi-Whaley / Black-76) - the leg carries the
  // contract's own q, the family the de-Americanisation remainder at the expiry's q. A
  // family refusal (sigma outside the box or below the guard, an unprepared family) runs
  // that attempt's remaining iterates on the seed kernel. The confirm is unchanged: a
  // root is served only after the confirm kernel at the contract's own q accepts it.
  // Observed, not owned: the family outlives the call.
  const AlSigmaFamily *family = nullptr;
  double family_q = std::numeric_limits<double>::quiet_NaN();
  // (sigma, q) family iterates (SPY1s Phase 2 step 5, opt-in; takes precedence over
  // `family`): every seed-tier iterate's cold point is qfamily->price(S, K, sigma, q) at
  // the strike's OWN q (L2h-b AlSigmaQFamily) - no D-model. Policy Always: confirmed as
  // above. Policy Band (the B-family arm; lane K3): the confirm is skipped only when the
  // family's validated envelope (american_family_error.hpp, the scoped
  // current_family_envelope of qfamily) encloses the root in a bracket no wider than
  // kIvRootRequirement; never by the band gate's half-spread statement. A refusal restarts
  // the attempt on the seed kernel, and its root is then confirmed (no band gate: the
  // request carried a family). qfamily_bound: the certification grid's scalar
  // (deam_family_certify: the envelope's domain-wide USD bound, which the grid scales by
  // max(P, 1), a conservative reading); not read by the acceptance.
  const AlSigmaQFamily *qfamily = nullptr;
  double qfamily_bound = std::numeric_limits<double>::infinity();
  // Family-native iterates (SPY1s Phase 3 lever 1, opt-in; with qfamily under Always): the
  // family's own map P_family(sigma; q) at the strike's own q is iterated WITHOUT the seed
  // leg (no Barone-Adesi-Whaley / Black-76 model). K2 (r5): one lane of the batch seam
  // (iv_batch::invert_many) - a bounded bracketed Newton on the fused price + vega, started
  // from the memory's native carry (the mid's bracket and point behind a bid / ask), else
  // from the Black-76 IV of the price (an upper bound of the American IV; a failed or
  // out-of-box seed starts from the bracket), at most kIvPremiumNativeEvaluations
  // evaluations. Its typed outcome is IvPremiumStats::native_status; a non-converged
  // outcome hands the price to the seed-leg path (which keeps the contract's leg memory)
  // and the carry keeps the bracket for the contract's next price.
  // Acceptance (lane K3, replacing the secant-slope confirm of R1-A MJ1): the root is
  // Skipped when the validated envelope encloses the target map's root in a bracket no
  // wider than kIvRootRequirement (two family prices), else Confirmed by the confirm kernel
  // closing such a bracket - every end a CERTIFIED side of the target's root: a confirm
  // price beyond the kernel's tested bound, or a family price beyond its envelope bound (a
  // local slope only places the far end) - else handed to Search (PriceOnly when the
  // kernel's bound cannot resolve the width at the root's slope, Interval / None when the
  // bracket did not close).
  bool qfamily_native = false;
  // The native fit route (K4 r6, opt-in; with qfamily_native): no request reaches Search.
  // A price inside the accurate non-monotone edge band, a refused family-native attempt and
  // a native root the acceptance leaves open (kind None) go to the bounded reference route
  // (american_iv.cpp reference_root: a certified bracketed root on the confirm kernel under
  // AlExerciseRegion::Intrinsic, classified per evaluation, started from the native
  // attempt's retained bracket / last point / fused vega); PriceOnly and Interval
  // acceptances are typed outcomes (IvPremiumStats::route), not re-solved. A request whose
  // side carries no family keeps the seed-leg route, its fallback the reference route.
  // The seed tier's routing gate never diverts a family request (the family is built on
  // the accurate kernel). `reference_budget` (nullptr: unbounded) is the chain's budget.
  bool native_route = false;
  IvReferenceBudget *reference_budget = nullptr;
};

// The family enclosure of a (sigma, q) family root (lane K3; american_iv.cpp, the Skipped
// rule of the root acceptance; declared here for its tests). Two family prices with their
// envelope bounds at the ends a = x - w/2, b = x + w/2 enclose the target map's root when
// P_f(a) + eps(a) <= price <= P_f(b) - eps(b), eps the per-point bound of `env`
// (family_price_bounded); `root` is then the falsi point clamped into [lo, hi] = [a, b].
// When that misses only because `price` lies outside [P_f(a), P_f(b)] while the bounded
// prices leave room, one re-centred attempt on the local chord's root. Without a covering
// envelope (`env` nullptr or Uncovered) nothing is enclosed and only `slope` is measured:
// the chord over the last attempt's bracket (NaN when a price refused), which places and
// never bounds. stats.family_prices counts the prices spent.
struct FamilyEnclosure {
  bool enclosed = false;
  double root = std::numeric_limits<double>::quiet_NaN();
  double lo = std::numeric_limits<double>::quiet_NaN();
  double hi = std::numeric_limits<double>::quiet_NaN();
  double slope = std::numeric_limits<double>::quiet_NaN();
};
[[nodiscard]] FamilyEnclosure family_enclosure(double price, double S, double K, double q, double x,
                                               double w, const AlSigmaQFamily &f,
                                               const ValidatedErrorEnvelope *env,
                                               IvPremiumStats &stats) noexcept;

// The top of the Premium route's edge band (K4): the zero-volatility American value plus
// max(al_non_monotone_gate(K, T, opts), the ceiling's rounding band) - a price at or below it
// is not inverted on the monotone branch (premium_root: RangeEdge; on the native fit route
// it is classified and goes to the reference route). NaN for invalid inputs.
[[nodiscard]] double iv_premium_edge_band_top(double S, double K, double T, double r, double q,
                                              Side side,
                                              const std::optional<AlOpts> &opts) noexcept;

// The seed-tier routing gate of a Premium attempt: gate->seed_gate_kappa when set, else
// the seed tier's own non-monotone gate (NaN for an invalid K or T, which routes).
[[nodiscard]] double iv_premium_seed_gate(double K, double T,
                                          const std::optional<AlOpts> &seed_opts,
                                          const IvPremiumGate *gate) noexcept;

// IvOpts-typed inversion under the Premium strategy with optional per-contract memory
// (nullptr: none). `opts.strategy` is ignored (this IS Premium); every other field
// means what it means in american_implied_vol(..., const IvOpts &), including
// refuse_non_monotone. stats.fallback says whether the Premium root was served; on a
// fallback the result equals the Search route's bit for bit. `gate` (nullptr: Always on
// opts.al_opts) governs the confirm of a seed-tier root (IvPremiumGate); it is ignored
// when seed_profile is unset or al_opts' own profile (nothing to confirm).
[[nodiscard]] IvResult american_implied_vol_premium(double price, double S, double K, double T,
                                                    double r, double q, Side side,
                                                    const IvOpts &opts, IvPremiumMemory *memory,
                                                    IvPremiumStats &stats,
                                                    const IvPremiumGate *gate = nullptr) noexcept;

} // namespace atx::vol
