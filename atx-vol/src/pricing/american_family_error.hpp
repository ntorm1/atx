#pragma once

// Error contract of the (sigma, q) family route (lane K3, r5 2026-10-22). Internal header
// (src/pricing); not part of the public API.
//
// Three quantities are kept apart (deep review F02 / section 5.4):
//   SampledErrorEstimate   - what probes measure. An estimate, never a bound, and never an
//                            acceptance input.
//   ValidatedErrorEnvelope - a per-family bound on the family's boundary error, built from
//                            the family's own nodes (construction below), with an explicit
//                            domain and a class. Domain-qualified: the interpolation term
//                            rests on a geometric-decay model of the Chebyshev tail that is
//                            VERIFIED on refinement (the order seen on the coarser truncation
//                            must hold on the finer one), not on an interval remainder, so it
//                            is an empirical contract, audited independently (probes at build,
//                            bracketed reference roots offline). There is no rigorous
//                            enclosure class today.
//   RootAcceptance         - how one American IV root of the family route was accepted.
//
// UNITS. Volatility: absolute (0.01 = one vol point). Price: USD of the contract (the
// quote's own price units). y: the Andersen-Lake dimensionless boundary,
// y = ln(xmax / B)^2 (american_boundary.hpp), at the premium quadrature's nodes.
//
// THE TARGET MAP. A(sigma) is the cold accurate kernel the family was built on
// (andersen_lake with the geometry's AlOpts at the strike's own q). The family and A share
// the European leg, the time interpolation and the premium quadrature (same geometry, same
// tables), so the only difference between them is the interpolated boundary. The 1e-7
// root requirement (kIvRootRequirement) is |sigma_served - sigma*| in absolute vol, where
// sigma* is A's root; A's own discretisation error versus the continuous model is model
// error, not root error, and is not part of it.
//
// THE PRICE BOUND (family_price_bounded). The put premium is
// (sqrt(T)/2) sum_i w_i g(z_i), w_i > 0 (Gauss-Legendre), and
// dg/d(ln B) = 2 e^{-r t} phi(d2) (r K - q B) / sigma, so with phi <= 1/sqrt(2 pi) and
// 0 < B <= xmax, |dg/d ln B| <= 2 C / (sigma sqrt(2 pi)), C = r K + max(-q, 0) xmax (the
// internal put's r, q, K). The European leg and the intrinsic / European / zero clamps are
// shared or 1-Lipschitz. Hence
//   |P_family - A| <= sqrt(T) C / (sigma sqrt(2 pi)) sum_i w_i d_i + rounding,
//   d_i >= |ln B_family(tau_i) - ln B_A(tau_i)| = |sqrt(y_f) - sqrt(y_A)|
//        <= min(sqrt(delta_i), delta_i / sqrt(y_f))
// with delta_i = ValidatedErrorEnvelope::bound_y[i] and y_f the family's own boundary at
// that node (computed at the priced point). The bound holds for every strike and spot
// (homogeneity: the family's y is strike-free), every sigma and q of the domain.

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>

#include "atx/vol/api/core/types.hpp"
#include "atx/vol/api/pricing/american.hpp" // AlSigmaQFamily, AlSigmaQFamilyStatus

namespace atx::vol {

// The established IV root requirement, absolute vol (preserved, master plan 3.2).
inline constexpr double kIvRootRequirement = 1.0e-7;

// Envelope allowances (y), each pinned by a test in tests/american_family_error_test.cpp:
//   kFamilyBuildParityY    - node-data noise: an AVX2-built family's node boundaries
//                            against the scalar solve the target map runs (the family
//                            header's AvxBoundary parity, <= ~4e-13), with margin
//                            (AmericanFamilyError.NodeTensorMatchesTheScalarBuild).
//   kFamilyReferenceNoiseY - the target map's own off-node noise: its boundary is a fixed
//                            sweep budget with an early exit, smooth in (sigma, q) while
//                            the sweep count does not switch; the allowance covers the
//                            rounding of an unswitched solve
//                            (AmericanFamilyError.TargetBoundaryIsSmoothBetweenNodes).
inline constexpr double kFamilyBuildParityY = 1.0e-12;
inline constexpr double kFamilyReferenceNoiseY = 1.0e-12;

// Probe measurements of a family against the cold accurate kernel: an ESTIMATE.
struct SampledErrorEstimate {
  double max_abs_usd = std::numeric_limits<double>::quiet_NaN(); // max |P_family - A|
  double max_rel = std::numeric_limits<double>::quiet_NaN();     // max |P_family - A| / max(A, 1)
  // max |P_family - A| / bound_usd over the probes (> 1: a probe violated the envelope)
  double max_bound_ratio = std::numeric_limits<double>::quiet_NaN();
  std::uint16_t probes = 0;
  std::uint16_t violations = 0;
};

// The class of a family's error statement. Only RefinementValidated admits a skipped
// confirmation; Uncovered requires every root to be confirmed.
enum class FamilyErrorClass : std::uint8_t {
  Uncovered = 0,
  RefinementValidated = 1,
};
inline constexpr std::size_t kFamilyErrorClassCount = 2;

// Why a family is Uncovered (Ok: it is covered).
enum class FamilyEnvelopeStatus : std::uint8_t {
  Ok = 0,
  NotPrepared = 1,          // the family is not prepared (or moved-from / re-prepared)
  UnsupportedShape = 2,     // a node count outside the construction's capacity
  DegenerateQBox = 3,       // a collapsed q box of non-zero width (q error not measurable)
  SigmaOrderNotVerified = 4, // the sigma-axis tail does not decay geometrically on refinement
  QOrderNotVerified = 5,    // the q-axis tail does not decay at the a-priori rate
  ProbeViolation = 6,       // an audit probe measured an error above the envelope
};
inline constexpr std::size_t kFamilyEnvelopeStatusCount = 7;

// The domain the envelope holds on: every (S, K) (homogeneity), sigma in [sigma_lo,
// sigma_hi], q in [q_lo, q_hi], the family's expiry T and side.
struct FamilyErrorDomain {
  double sigma_lo = 0.0;
  double sigma_hi = 0.0;
  double q_lo = 0.0;
  double q_hi = 0.0;
  double T = 0.0;
  Side side = Side::Put;
};

// The envelope's contributions, each the worst over the premium quadrature nodes.
struct FamilyErrorContributions {
  double interpolation_y = 0.0; // 2 x (sigma tail + q tail) of the node tensor (y)
  double boundary_solve_y = 0.0; // node-data noise through the interpolant (y)
  double reference_y = 0.0;     // the target map's own off-node noise (y)
  double rounding_y = 0.0;      // floating-point budget of the tensor values (y)
  double quadrature_usd = 0.0;  // 0 by construction: the family and A share the quadrature
  double rounding_rel = 0.0;    // per-price rounding budget, relative to S + K (internal put)
  double sigma_ratio = 0.0;     // worst per-coefficient sigma-tail decay ratio used (< 1)
  double q_ratio = 0.0;         // worst q-tail ratio used: max(a-priori Bernstein, observed)
  // Quadrature nodes whose tail order did not verify: they carry no y bound (+inf) and
  // the price bound uses their trivial box bound instead.
  std::uint16_t unverified_nodes = 0;
};

inline constexpr std::size_t kFamilyEnvelopeMaxQuad = 64;

struct ValidatedErrorEnvelope {
  FamilyErrorClass error_class = FamilyErrorClass::Uncovered;
  FamilyEnvelopeStatus status = FamilyEnvelopeStatus::NotPrepared;
  FamilyErrorDomain domain{};
  // The family it was built for (identity check; observed, not owned) and its node counts.
  const AlSigmaQFamily *family = nullptr;
  std::uint64_t preparation_stamp = 0; // binds this envelope to the exact prepared tensor
  std::uint16_t n_sigma = 0;
  std::uint16_t n_q = 0;
  // bound_y[i]: sup over the domain of |y_family - y_A| at premium quadrature node i (the
  // quadrature's own order, n_quad nodes). +inf when Uncovered.
  std::array<double, kFamilyEnvelopeMaxQuad> bound_y{};
  // zc[i]: node i's time coordinate on the boundary's Chebyshev axis (NaN: the integrand
  // reads no boundary there); y_lo[i]: a lower bound of the target's y over the domain
  // (the interpolant's mean coefficient less every other one, less bound_y[i]).
  std::array<double, kFamilyEnvelopeMaxQuad> zc{};
  std::array<double, kFamilyEnvelopeMaxQuad> y_lo{};
  // The premium quadrature's nodes (z = sqrt(t)) and weights the bound integrates with.
  std::array<double, kFamilyEnvelopeMaxQuad> z{};
  std::array<double, kFamilyEnvelopeMaxQuad> w{};
  std::uint16_t n_quad = 0;
  FamilyErrorContributions contributions{};
  SampledErrorEstimate audit{}; // the build-time probe audit (an estimate; can only veto)

  [[nodiscard]] bool covered() const noexcept {
    return error_class == FamilyErrorClass::RefinementValidated;
  }
};

// The envelope of a prepared family (construction in american_family_error.cpp): reads the
// node tensor through the family's internal accessor, no solve, no allocation. Uncovered
// with the reason when the family is not prepared or refinement does not confirm an order.
[[nodiscard]] ValidatedErrorEnvelope build_family_error_envelope(const AlSigmaQFamily &f) noexcept;

// One family price with its bound against the target map A at the same point. `price` is
// AlSigmaQFamily::price(S, K, sigma, q).price bit for bit; `bound_usd` = +inf when the
// envelope does not cover the family or the point lies outside its domain.
struct FamilyBoundedPrice {
  double price = std::numeric_limits<double>::quiet_NaN();
  double bound_usd = std::numeric_limits<double>::infinity();
  AlSigmaQFamilyStatus status = AlSigmaQFamilyStatus::NotPrepared;
};
[[nodiscard]] FamilyBoundedPrice family_price_bounded(const AlSigmaQFamily &f,
                                                      const ValidatedErrorEnvelope &env, double S,
                                                      double K, double sigma,
                                                      double q) noexcept;

struct FamilyBoundedRequest {
  double S = 0.0;
  double K = 0.0;
  double sigma = 0.0;
  double q = 0.0;
};

struct FamilyBoundedBatchWork {
  std::size_t fused_prices = 0;
  std::size_t scalar_prices = 0;
};

// The same scalar price bits, with a bound no smaller than family_price_bounded's.
// Fuses the accurate premium and sensitivity passes; omitting the per-node minimum
// with the trivial bound can only enlarge the envelope. Uncovered/stale envelopes
// and unsupported arithmetic profiles use the unchanged scalar path. No allocation.
// Unequal lengths return false without writing. Work counts are added, not reset.
[[nodiscard]] bool family_prices_bounded(const AlSigmaQFamily &f,
                                         const ValidatedErrorEnvelope &env,
                                         std::span<const FamilyBoundedRequest> requests,
                                         std::span<FamilyBoundedPrice> out,
                                         FamilyBoundedBatchWork *work = nullptr) noexcept;

// A domain-wide price bound (USD) at (S, K) for every sigma and q of the domain: the
// per-point bound with 1 / sigma_lo, the worst carry of the q box (a negative carry's
// e^{-q t} > 1 at each quadrature node included) and y_lo for y_f. What a
// caller without the priced point's boundary can use (the certification grid's scalar);
// +inf when the envelope is Uncovered.
[[nodiscard]] double family_domain_bound_usd(const ValidatedErrorEnvelope &env, double S, double K,
                                             double rate) noexcept;

// The strike at which spot S sits on `f`'s exercise boundary now (tau = T) at (sigma, q):
// a put is exercised for every K at or above it, a call for every K at or below it. NaN
// when the family refuses the point. (The audit's near-exercise probes.)
[[nodiscard]] double family_exercise_strike(const AlSigmaQFamily &f, double S, double sigma,
                                            double q) noexcept;

// Records the build-time probe audit into `env` (an independent check: a violation makes
// the family Uncovered, a pass changes no bound).
void record_family_probe_audit(ValidatedErrorEnvelope &env,
                               const SampledErrorEstimate &probes) noexcept;

// ── Root acceptance (american_iv.cpp; counters by kind) ──────────────────────────────

// How a family-route root was accepted. Mapping onto C0's identifiability statuses when
// they land: Confirmed / Skipped -> identified IV; PriceOnly -> price_only; Interval ->
// iv_interval. None: the attempt produced no family-route root (refusal or no family).
enum class RootAcceptanceKind : std::uint8_t {
  None = 0,
  Confirmed = 1, // the confirm kernel closed a bracket of A's root no wider than the requirement
  Skipped = 2,   // the family envelope enclosed A's root in such a bracket: no confirm solve
  PriceOnly = 3, // low vega: the requirement is below what the confirm kernel can resolve
  Interval = 4,  // a bracket was found, wider than the requirement, within the budget
};
inline constexpr std::size_t kRootAcceptanceKindCount = 5;

// The evidence that closed (or failed to close) the bracket.
enum class RootAcceptanceReason : std::uint8_t {
  None = 0,
  FamilyEnclosure = 1,   // both ends: family price -/+ envelope (Skipped)
  ConfirmAndFamily = 2,  // one end the confirm kernel, the other the family enclosure
  ConfirmBothEnds = 3,   // both ends the confirm kernel
  LowVega = 4,           // PriceOnly
  BudgetExhausted = 5,   // Interval (or no bracket at all: kind None)
  // Roots outside the bracket rule (counted so a skip without a bound is never silent):
  NewtonContract = 6,    // Confirmed by the established keys-OFF confirm (Newton distance on
                         // the confirm kernel, no bracket): every non-native route
  LegacyBandGate = 7,    // Skipped by the band gate's half-spread statement (a request with
                         // no (sigma, q) family only; the audit counts it)
  LegacyNever = 8,       // Skipped by policy Never (measurement arm; no bound at all)
};
inline constexpr std::size_t kRootAcceptanceReasonCount = 9;

struct RootAcceptance {
  RootAcceptanceKind kind = RootAcceptanceKind::None;
  RootAcceptanceReason reason = RootAcceptanceReason::None;
  FamilyErrorClass error_class = FamilyErrorClass::Uncovered; // the envelope's class
  double bound_iv = std::numeric_limits<double>::quiet_NaN(); // hi - lo (vol): |served - sigma*|
  double lo = std::numeric_limits<double>::quiet_NaN();       // the bracket of A's root (vol)
  double hi = std::numeric_limits<double>::quiet_NaN();
};

// The calling thread's envelope for the family a Premium inversion iterates on (a scoped
// context, like fit_timing's ChainScope): set by the de-Am cache around one inversion, read
// by the root acceptance. RAII, nests (restores the previous one), thread-local; the
// envelope must outlive the scope.
class FamilyEnvelopeScope {
public:
  explicit FamilyEnvelopeScope(const ValidatedErrorEnvelope *env) noexcept;
  ~FamilyEnvelopeScope();
  FamilyEnvelopeScope(const FamilyEnvelopeScope &) = delete;
  FamilyEnvelopeScope &operator=(const FamilyEnvelopeScope &) = delete;
  FamilyEnvelopeScope(FamilyEnvelopeScope &&) = delete;
  FamilyEnvelopeScope &operator=(FamilyEnvelopeScope &&) = delete;

private:
  const ValidatedErrorEnvelope *previous_;
};

// The scoped envelope when it was built for `family` (same object, box and node counts),
// else nullptr: a root without one is confirmed.
[[nodiscard]] const ValidatedErrorEnvelope *
current_family_envelope(const AlSigmaQFamily *family) noexcept;

} // namespace atx::vol
