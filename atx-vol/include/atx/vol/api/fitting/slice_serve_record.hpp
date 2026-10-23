#pragma once

// Immutable fitted serving bundle of ONE expiry (lane H0-A, r6 2026-10-23; master plan
// Sprint 5 package H0, deep review F12). Phase A: the record format, its builder from a
// COMPLETED QuoteSliceFit, the tiered serve and the tick-rate implied volatility. Phase B
// (lane H1): the fitter's hand-off at the end of certify_and_serve (QuoteSliceFit::
// serve_handoff), the serve_build stage that builds records off the fit clock, the live
// publication (serve_book.hpp) and the evaluate_quote_slice fast path
// (QuoteSliceFit::serve_record).
//
// ## What a record holds
//
// A SliceServeRecord is built once from a certified fit and is never mutated afterwards:
//   * served_nodes: every node of the fit's AmericanPriceSlice with its call and put price
//     copied BYTE FOR BYTE, the volatility query_quote_slice serves there (lane C0's typed
//     answer: status, price-only reason, interval, convention), the contract's effective
//     yield, how that volatility was accepted (lane K3's RootAcceptance vocabulary), the
//     tier-1 map's price at the node (price_map_delta) and per-Greek reasons;
//   * the expiry's (sigma, q) families (american.hpp AlSigmaQFamily) with the geometries
//     they observe, shared read-only (std::shared_ptr<const>), each with lane K3's error
//     contract (deam_family_certify: the validated envelope, audited by probes);
//   * per node side a spot ladder (tier 0): a Chebyshev plane in (ln S, sigma) sampled
//     from the family at the node's OWN strike and yield;
//   * a clone of the fitted IVolCurve (shared), the version digests, the build clocks,
//     the byte footprint and the support block.
//
// ## Error statements (ServeBoundClass; master plan 3.2, deep review F02 / 5.4)
//
// No number in a record is called a certificate unless its class says what backs it:
//   Exact               the certified slice's own node bytes (error 0 by definition);
//   RefinementValidated lane K3's class: the family's per-point envelope bound
//                       (family_price_bounded) and, for a ladder, the plane's Chebyshev
//                       tail bound verified on refinement and audited at its check points;
//                       an empirical contract, not an interval enclosure;
//   Sampled             a measurement at sampled points (K3's SampledErrorEstimate): never
//                       an acceptance input, never "certified"; answers carry sampled = true;
//   KernelTested        the AlProfile::Fast kernel's tested price bound (a Fast confirm).
// A Sampled ladder serves prices labelled Sampled (ServeRecordPolicy::sampled_ladders); its
// IV root is never Ok.
//
// ## Ownership and lifetime
//
// The record owns everything it reads: the node prices are copied, the curve is cloned,
// and the families are held by a shared_ptr together with the geometries they observe
// (one heap holder whose geometries are destroyed after its families), so a record
// outlives the fit it was built from and every copy of the shared_ptr keeps the families
// and their geometries alive together. Nothing in a record points into the fit.
//
// ## Tiers (ServeTier)
//
//   Ladder (0)    the node's spot ladder: price, delta, gamma, vega and the
//                 continuation-region theta from ONE Chebyshev map (gamma and theta withheld
//                 inside the near-exercise-edge band, kServeEdgeBand); served nodes only,
//                 the spot inside the ladder window and a ladder kept at build.
//   Family (1)    the (sigma, q) family at the contract's own yield: any strike of the
//                 domain, any spot; price and vega (K1's fused pass) and, where the build
//                 carries lane A0's bundle, A0's Greeks with A0's mask, reasons and
//                 convention verbatim. Its bound is K3's per-point bound at the contract.
//   Certified (2) at the fit's own spot the certified slice price (the node's bytes, the
//                 piecewise-linear slice between nodes); at any other spot the family price
//                 confirmed by ONE AlProfile::Fast solve (an explicit, counted cost).
//   Cold (3)      never run inside serve() or implied_vol(): a typed route to today's path
//                 (query_quote_slice / andersen_lake / american_implied_vol on the fit).
//
// A request names the cheapest tier to try and the dearest one it accepts; the record
// walks up from the first and answers from the first tier that can, or returns a typed
// refusal with the route (ServeRoute) a caller takes. No tier hides a cold solve.
//
// ## Prices and Greeks (ruling b, F12)
//
// At the fit's own spot (bit-equal S) a served price is ALWAYS the certified slice price:
// the node's bytes at a node, the piecewise-linear slice price between nodes, on every
// tier. The tier's Greeks then come from a different map; the answer says so
// (price_source != the Greek map), carries price_map_delta = |that map's price - the
// served price| and that map's own bound (map_certificate_usd). At any other spot the
// price and the Greeks come from the same map (price_map_delta = 0), except the Certified
// tier, whose price is the Fast confirm's.
//
// ## Sticky strike
//
// Every tier prices a contract at its served volatility: the node's served volatility at a
// node, the fitted curve's sigma(ln(K / F_fit)) between nodes. A spot move does NOT move
// the smile (sticky strike); only a new fit does.
//
// ## Threads and allocation
//
// Immutable after build: any number of threads may call serve() / implied_vol() on one
// record. Ladder and Family reads allocate nothing (pinned by SliceServeRecordAlloc.*).
// Bytes, construction clocks and latencies are measured in docs/serve-bundle.md and
// docs/live-query.md.

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <span>

#include "atx/vol/api/core/types.hpp" // Side, Result, Status
#include "atx/vol/api/fitting/quote_slice.hpp"

namespace atx::vol {

enum class ServeTier : std::uint8_t { Ladder = 0, Family = 1, Certified = 2, Cold = 3 };
inline constexpr std::size_t kServeTierCount = 4;

// The outcome of one serve() or implied_vol() call (and of a live book query,
// serve_book.hpp, which adds BudgetExhausted and Invalidated).
enum class ServeStatus : std::uint8_t {
  Ok = 0,              // answered by `tier`
  Exercised = 1,       // the family's exercise regime: intrinsic with the exercise Greeks
  PriceOnly = 2,       // the certified price is served; no volatility identifies it (C0)
  OutOfDomain = 3,     // the strike lies outside the certified strike domain
  SigmaOutOfBox = 4,   // the volatility lies outside every allowed tier's box
  TierUnavailable = 5, // no allowed tier has a map for this contract (see support())
  Stale = 6,           // older than the policy's max_age_ns, or no clock to tell (now_ns <= 0)
  NoConvergence = 7,   // (IV) no accepted root within the allowed tiers' budget
  NonMonotoneBand = 8, // (IV) the target lies in the L1c non-monotone band
  Unconfirmed = 9,     // (IV) a family root the build cannot bound: sigma set, not accepted
  InvalidArgument = 10,
  BudgetExhausted = 11, // (book) the async rebuild queue is full: nothing was enqueued
  Invalidated = 12,     // (book) the expiry's geometry no longer applies (expiry or an
                        // event such as an ex-dividend date has passed): Refit
};

// Where a refused (or unconfirmed) contract goes next.
enum class ServeRoute : std::uint8_t {
  None = 0,     // answered
  ColdPath = 1, // tier 3: today's path on the fit (query / andersen_lake / american IV)
  Rebuild = 2,  // rebuild the record from the same fit (stale; async, serve_book.hpp)
  Refit = 3,    // the fit itself must be redone (outside its domain, price-only node,
                // invalidated geometry)
};

enum class ServePriceSource : std::uint8_t {
  None = 0,
  SliceNode = 1,           // the certified node price (bytes)
  SlicePiecewiseLinear = 2, // the certified slice between nodes
  Ladder = 3,
  Family = 4,
  FamilyFastConfirm = 5,   // one AlProfile::Fast solve at the family's volatility
};

enum class ServeGreekSource : std::uint8_t { None = 0, Ladder = 1, Family = 2 };

// What backs an error statement (file header, "Error statements").
enum class ServeBoundClass : std::uint8_t {
  None = 0,                // no statement (NaN)
  Exact = 1,               // the certified node bytes
  RefinementValidated = 2, // lane K3's class (per-point envelope; verified ladder tail)
  Sampled = 3,             // a sampled estimate: never certified, never an acceptance input
  KernelTested = 4,        // the AlProfile::Fast kernel's tested bound
};

enum class ServeGreek : std::uint8_t {
  Delta = 0,
  Gamma = 1,
  Vega = 2,
  Theta = 3,
  Rho = 4,
  Phi = 5,
};
inline constexpr std::size_t kServeGreekCount = 6;

// Why a Greek holds what it holds. Served and Exercised carry a number, every other reason
// carries NaN (NotRequested included): unsupported risk is never 0. Values 0-9 are lane
// A0's AlFamilyGreekReason values (the family bundle's reasons are carried verbatim; 9 is
// A0's fix-round NearExerciseEdge: theta and gamma withheld inside the near-exercise-edge
// band, below); 32 and above are the record's own.
enum class ServeGreekReason : std::uint8_t {
  NotRequested = 0,
  Served = 1,
  Exercised = 2,
  Refused = 3,
  NoCarryBumps = 4,
  CarryBumpRefused = 5,
  NoQAxis = 6,
  NearNodeDegenerate = 7,
  ConventionUnsupported = 8,
  NearExerciseEdge = 9,   // theta / gamma inside the near-exercise-edge band (any tier)
  BundleUnavailable = 32, // this build carries no family Greek bundle (lane A0 seam off)
  TierHasNoGreek = 33,    // the answering tier's map has no such Greek (ladder: rho, phi)
  NoMap = 34,             // the answer has no Greek map (refusal, price-only, slice price)
};
using ServeGreekReasons = std::array<ServeGreekReason, kServeGreekCount>;

// THE NEAR-EXERCISE-EDGE BAND (lane A0 fix round, review-A0 MJ1). edge_sd is a contract's
// distance to the early-exercise edge in standard deviations: ln(S / S*) / (sigma sqrt T)
// for a put, ln(S* / S) / (sigma sqrt T) for a call, S* the edge at the contract's
// (sigma, q); > 0 on the continuation side, +inf where no finite edge exists (the
// European regime). A continuation answer with edge_sd < kServeEdgeBand is IN THE BAND:
// theta and gamma are NaN with NearExerciseEdge on every tier (there the identity theta
// rP - (r - q) S delta - sigma^2 S^2 gamma / 2 -> 0 is a cancellation of O(rK) terms that
// amplifies any map's gamma error: A0 measured it up to 138 % off, wrong sign, 0.007
// sigma sqrt T from the edge). Price, delta and vega stay served. The value equals A0's
// kAlFamilyGreekEdgeBand (static_assert under the A0 seam).
inline constexpr double kServeEdgeBand = 0.25;

struct ServeGreekMask {
  std::uint8_t bits = 0;
  [[nodiscard]] static constexpr std::uint8_t bit(ServeGreek g) noexcept {
    return static_cast<std::uint8_t>(1u << static_cast<unsigned>(g));
  }
  [[nodiscard]] static constexpr ServeGreekMask none() noexcept { return {0}; }
  // delta, gamma, vega, theta
  [[nodiscard]] static constexpr ServeGreekMask four() noexcept {
    return {static_cast<std::uint8_t>(bit(ServeGreek::Delta) | bit(ServeGreek::Gamma) |
                                      bit(ServeGreek::Vega) | bit(ServeGreek::Theta))};
  }
  // four() plus phi and rho
  [[nodiscard]] static constexpr ServeGreekMask all() noexcept {
    return {static_cast<std::uint8_t>(four().bits | bit(ServeGreek::Phi) | bit(ServeGreek::Rho))};
  }
  [[nodiscard]] constexpr bool has(ServeGreek g) const noexcept { return (bits & bit(g)) != 0; }
};

// What the Greeks hold fixed and whose derivatives they are (lane A0's
// AlFamilyGreekConvention where the family bundle answers; the ladder's own otherwise).
enum class ServeGreekModel : std::uint8_t {
  None = 0,
  SigmaQFamily = 1,     // the (sigma, q) family's own map (A0: not the converged model)
  NodeLadder = 2,       // the node ladder's Chebyshev map of that family map
};
struct ServeGreekConvention {
  bool sticky_strike = true; // the served volatility is held when the spot moves
  ServeGreekModel model = ServeGreekModel::None;
  bool theta_pde_identity = true; // theta = rV - (r - q)S delta - sigma^2 S^2 gamma / 2,
                                  // calendar dV/dt per year of the fit's variance clock
  std::uint64_t surface_version = 0; // ServeRecordPolicy::surface_version, echoed
};

// Lane K3's root-acceptance vocabulary (src/pricing/american_family_error.hpp, internal),
// mirrored value for value so a record carries it without exposing the internal header.
enum class ServeAcceptanceKind : std::uint8_t {
  None = 0,
  Confirmed = 1,
  Skipped = 2,
  PriceOnly = 3,
  Interval = 4,
  // A record-tier root on a RefinementValidated ladder: |served - the accurate root| <=
  // (the ladder's validated bound / slope + the Newton distance) <= the policy's
  // iv_tolerance. Never on a Sampled ladder.
  LadderCertificate = 32,
};
enum class ServeAcceptanceReason : std::uint8_t {
  None = 0,
  FamilyEnclosure = 1,
  ConfirmAndFamily = 2,
  ConfirmBothEnds = 3,
  LowVega = 4,
  BudgetExhausted = 5,
  NewtonContract = 6,
  LegacyBandGate = 7,
  LegacyNever = 8,
  FittedCurve = 32, // no root: the served volatility is the fitted curve's value
};
enum class ServeErrorClass : std::uint8_t {
  Uncovered = 0,           // K3 FamilyErrorClass::Uncovered (or the K3 seam is off)
  RefinementValidated = 1, // K3 FamilyErrorClass::RefinementValidated
};
struct ServeRootAcceptance {
  ServeAcceptanceKind kind = ServeAcceptanceKind::None;
  ServeAcceptanceReason reason = ServeAcceptanceReason::None;
  ServeErrorClass error_class = ServeErrorClass::Uncovered;
  double bound_iv = std::numeric_limits<double>::quiet_NaN(); // |served - root| bound (vol)
};

// Per node side: which tiers can serve it. Bits of ServedNodeSide::tiers.
inline constexpr std::uint8_t kServeNodeLadder = 1u;
inline constexpr std::uint8_t kServeNodeFamily = 2u;

struct ServedNodeSide {
  double price = std::numeric_limits<double>::quiet_NaN(); // the certified price, bytes
  // lane C0's typed answer (query_quote_slice) at the node: Success with `sigma` and the
  // convention, or PriceOnly with the reason and the interval where known.
  QuoteSliceEvaluationStatus status = QuoteSliceEvaluationStatus::NoFit;
  QuoteSlicePriceOnlyReason reason = QuoteSlicePriceOnlyReason::None;
  QuoteSliceVolConvention convention = QuoteSliceVolConvention::None;
  double sigma = std::numeric_limits<double>::quiet_NaN();
  double iv_lower = std::numeric_limits<double>::quiet_NaN();
  double iv_upper = std::numeric_limits<double>::quiet_NaN();
  double q = std::numeric_limits<double>::quiet_NaN(); // effective yield, variance clock
  bool moved = false;      // a repair moved this node side (QuoteSliceFit::moved_nodes)
  ServeRootAcceptance acceptance{}; // how `sigma` was accepted
  ServeErrorClass family_class = ServeErrorClass::Uncovered; // the side family's class
  // The family's price at (S_fit, K, sigma, q) minus `price` (NaN: no family price).
  double family_price_delta = std::numeric_limits<double>::quiet_NaN();
  // The ladder's error statement in USD against the cold accurate kernel over its window,
  // classed by ladder_class (NaN and None when the node side has no ladder):
  //   RefinementValidated: the plane's verified Chebyshev tail bound (ladder_plane_tail_usd)
  //     + the largest K3 per-point family bound over the ladder's sample and check points
  //     (ladder_family_bound_usd) + an 8-ulp floor;
  //   Sampled: the plane's largest measured distance to the family at its check points
  //     (ladder_plane_check_usd) + the larger of 10 x K3's probe audit (relative, x max(P,
  //     1)) and the node's measured family error + the floor. An estimate.
  double ladder_certificate_usd = std::numeric_limits<double>::quiet_NaN();
  ServeBoundClass ladder_class = ServeBoundClass::None;
  double ladder_plane_tail_usd = std::numeric_limits<double>::quiet_NaN();
  double ladder_plane_check_usd = std::numeric_limits<double>::quiet_NaN();
  double ladder_family_bound_usd = std::numeric_limits<double>::quiet_NaN();
  std::uint8_t tiers = 0; // kServeNodeLadder | kServeNodeFamily
  // The family tier's per-Greek reasons at the fit's spot for the full mask (A0's bundle's
  // under the A0 seam; else BundleUnavailable except vega).
  ServeGreekReasons family_reasons{};
  // The converged early-exercise edge S* at (tau, sigma, r, q) - exercise_boundary() on
  // the fit's AlOpts - that the ladder's band test uses: 0 (put) / +inf (call) in the
  // European regime; NaN when not located (no ladder candidate, or the solve refused,
  // which makes the side ladder-ineligible).
  double edge_spot = std::numeric_limits<double>::quiet_NaN();
};

struct ServedNode {
  double strike = 0.0;
  ServedNodeSide call{};
  ServedNodeSide put{};
};

// What a record can serve.
enum class ServeUnsupportedReason : std::uint8_t {
  None = 0,
  DatedCash = 1,          // cash dividends: tier 3 only in this phase (lane D1)
  BelowFamilyTau = 2,     // T below kAlSigmaFamilyMinTau: no family
  FamilyRefused = 3,      // the family build (or its K3 certification) refused the side
  FamiliesNotBuilt = 4,   // ServeRecordPolicy::families == None
  NoServedVolatility = 5, // no node side of the side serves a volatility
  HandoffMismatch = 6,    // a handed family did not reproduce the fitter's (H1): refused
};
enum class ServeCashDividends : std::uint8_t { NotPresent = 0, UnsupportedInRecord = 1 };

// How a record's families were obtained.
enum class ServeFamilyOrigin : std::uint8_t {
  None = 0,
  Built = 1,   // the record's own recipe (box from its served sigmas), K3-certified
  Handed = 2,  // the fitter's recipe from QuoteSliceFit::serve_handoff, re-prepared and
               // verified against the fitter's own prices and envelope (lane H1)
  Supplied = 3, // a holder passed to detail::build_serve_record_with
};

struct ServeSideSupport {
  bool family = false;
  ServeFamilyOrigin origin = ServeFamilyOrigin::None;
  std::size_t ladders = 0;            // node sides with a ladder (validated + sampled)
  std::size_t ladders_validated = 0;  // ... whose statement is RefinementValidated
  std::size_t ladders_sampled = 0;    // ... served on a Sampled estimate (labelled)
  std::size_t ladders_ineligible = 0; // ladders refused at build (exercise region, bound)
  ServeUnsupportedReason reason = ServeUnsupportedReason::None;
  double sigma_lo = std::numeric_limits<double>::quiet_NaN(); // the family's sigma box
  double sigma_hi = std::numeric_limits<double>::quiet_NaN();
  double q_lo = std::numeric_limits<double>::quiet_NaN();     // the family's q box
  double q_hi = std::numeric_limits<double>::quiet_NaN();
  // Lane K3's error contract of the side family (deam_family_certify): the envelope's
  // class and status (FamilyEnvelopeStatus value), its quadrature nodes without a verified
  // order (they take box bounds), the build-time probe audit (a SampledErrorEstimate:
  // probes, violations, max |P_family - A| / bound and max relative error), and the
  // envelope's domain-wide price bound at the grid band's strike ends (USD; the fitter's
  // q_bound, reported only - the record certifies with the per-point bound).
  ServeErrorClass family_class = ServeErrorClass::Uncovered;
  std::uint8_t envelope_status = 1; // FamilyEnvelopeStatus::NotPrepared
  std::uint16_t unverified_nodes = 0;
  std::uint16_t audit_probes = 0;
  std::uint16_t audit_violations = 0;
  double audit_max_bound_ratio = std::numeric_limits<double>::quiet_NaN();
  // K3's SampledErrorEstimate: the audit's max |P_family - A| / max(A, 1). Feeds only
  // Sampled ladder statements (x 10), never a validated one.
  double family_bound_rel = std::numeric_limits<double>::infinity();
  double domain_bound_usd = std::numeric_limits<double>::infinity();
  // max |family - slice| over the between-node audit points at the fit's spot, x 2 (Sampled).
  double between_node_bound_usd = std::numeric_limits<double>::quiet_NaN();
};

struct ServeSupport {
  std::array<bool, kServeTierCount> tiers{}; // tiers some contract of the record can use
  ServeSideSupport call{};
  ServeSideSupport put{};
  ServeCashDividends cash_dividends = ServeCashDividends::NotPresent;
  ServeUnsupportedReason reason = ServeUnsupportedReason::None; // record-wide reason
  bool double_continuation = false; // regime without a single exercise boundary
  double tau = 0.0;                 // the fit's variance clock T
  double tau_min_family = 0.0;      // kAlSigmaFamilyMinTau
  // The Greeks each tier serves (ServeGreekMask bits).
  std::array<ServeGreekMask, kServeTierCount> greeks{};
  // The compile-time seams this build carries (review-H0A m14): lane A0's family Greek
  // bundle and lane K3's error envelope. With k3_envelope false every family is Uncovered.
  bool a0_bundle = false;
  bool k3_envelope = false;
};

// Identity of the record: SHA-256 digests of the served state, the build policy and the
// fit's inputs (each over a fixed canonical byte layout, slice_serve_record.cpp), plus
// the build clocks.
using ServeDigest = std::array<std::byte, 32>;
struct ServeRecordVersion {
  ServeDigest fit_sha256{};    // node prices, flags, served rows, curve values at the nodes
  ServeDigest policy_sha256{}; // every ServeRecordPolicy field and the seams
  ServeDigest input_sha256{};  // market, snapshot, carry curve, dividends, quote-level rows
  std::int64_t build_steady_ns = 0; // steady clock at build end
  std::int64_t build_wall_ns = 0;   // Unix ns at build end
  std::int64_t as_of_ns = 0;        // snapshot quote_ns, else build_wall_ns (staleness base)
};

struct ServeRecordBytes {
  std::size_t nodes = 0;
  std::size_t ladders = 0;
  std::size_t families = 0;   // AlSigmaQFamily::state_bytes() of the held families
  std::size_t geometries = 0; // every prepared geometry the holder owns (three per holder)
  std::size_t fixed = 0;      // the record's own fixed state
  [[nodiscard]] std::size_t total() const noexcept {
    return nodes + ladders + families + geometries + fixed;
  }
};

struct ServeBuildClocks {
  double identify_us = 0.0; // query_quote_slice per node side (re-implied nodes invert)
  double family_us = 0.0;   // family builds, their K3 certification, or the hand-off check
  double ladder_us = 0.0;   // ladder sampling and their statements
  double audit_us = 0.0;    // price_map_delta and the between-node audit
  double digest_us = 0.0;
  double total_us = 0.0;
  std::size_t lazy_family_builds = 0; // families this build solved (0: supplied)
  std::size_t family_evaluations = 0; // family price evaluations spent by the build
  std::size_t reimplied_nodes = 0;    // node sides whose served sigma was re-implied
};

enum class ServeFamilyMode : std::uint8_t {
  None = 0,  // no family: tiers 0-1 unavailable (Certified at the fit's spot only)
  Build = 1, // the fit's hand-off when it carries one, else the record's own recipe
};

struct ServeRecordPolicy {
  ServeFamilyMode families = ServeFamilyMode::Build;
  bool ladders = true;
  // Serve a ladder whose validated statement does not qualify on its Sampled estimate,
  // labelled Sampled (never as validated; its IV root is never Ok). false: such a ladder
  // is ineligible.
  bool sampled_ladders = true;
  // The family sigma box: [lo_mult x min, hi_mult x max] of the side's served sigmas,
  // floored at kAlSigmaFamilyMinSigma.
  double sigma_lo_mult = 0.6;
  double sigma_hi_mult = 1.6;
  // Ladder plane: Chebyshev nodes in ln S and sigma; the ln-spot window half-width is
  // clamp(window_sigmas x sigma sqrt(T), window_min, window_max); the sigma window is
  // sigma x [1 - sigma_half_width, 1 + sigma_half_width] inside the family box.
  std::uint8_t ladder_spot_nodes = 12;  // [4, 16]
  std::uint8_t ladder_sigma_nodes = 6;  // [3, 8]
  double ladder_window_sigmas = 2.0;
  double ladder_window_min = 0.0025;
  double ladder_window_max = 0.04;
  double ladder_sigma_half_width = 0.25; // (0, 0.9]
  // A ladder serves only when its statement is at most max(cert_usd, cert_rel x F).
  double ladder_max_cert_usd = 5.0e-4;
  double ladder_max_cert_rel = 5.0e-7;
  std::int64_t max_age_ns = 0;  // > 0: older answers are Stale (and now_ns is required)
  double iv_tolerance = 1.0e-7; // absolute vol: the bound an accepted root must meet
  std::uint64_t surface_version = 0; // echoed in ServeGreekConvention
};

// InvalidArgument for an out-of-range policy field.
[[nodiscard]] Status validate_serve_record_policy(const ServeRecordPolicy &policy);

// The ladder profile whose statements can accept a tick-rate IV root (lane H1): 16 x 8 plane
// nodes over sigma x [0.9, 1.1] and ln S within +/- 2 %, every other field the default. On
// the H0-A fixture its planes sit within ~1e-10 USD of the family (the default 12 x 6 over
// +/- 25 % sigma: ~7e-6 USD, so statement / vega exceeds iv_tolerance and no ladder root is
// accepted). docs/live-query.md has the measured table.
[[nodiscard]] ServeRecordPolicy serve_record_policy_tick() noexcept;

struct ServeRequest {
  double strike = 0.0;
  Side side = Side::Call;
  double spot = 0.0;
  ServeTier tier = ServeTier::Ladder;        // the cheapest tier to try
  ServeTier tier_max = ServeTier::Certified; // the dearest tier accepted (Cold: route only)
  ServeGreekMask greeks = ServeGreekMask::four();
  // Unix ns. Required (> 0) when the policy's max_age_ns > 0: a missing clock refuses
  // (Stale, ColdPath) instead of serving (review-H0A m2).
  std::int64_t now_ns = 0;
  // The Family tier's per-point bound (one more family pass); false leaves the family
  // answer's certificate NaN with class None (the chain path's choice).
  bool family_bound = true;
};

struct ServeAnswer {
  double price = std::numeric_limits<double>::quiet_NaN();
  double delta = std::numeric_limits<double>::quiet_NaN();
  double gamma = std::numeric_limits<double>::quiet_NaN();
  double vega = std::numeric_limits<double>::quiet_NaN();
  double theta = std::numeric_limits<double>::quiet_NaN();
  double rho = std::numeric_limits<double>::quiet_NaN();
  double phi = std::numeric_limits<double>::quiet_NaN();
  ServeGreekReasons reason{};
  double sigma = std::numeric_limits<double>::quiet_NaN(); // the volatility priced
  // |the Greek map's price - price| (0: one map; NaN: no Greek map).
  double price_map_delta = std::numeric_limits<double>::quiet_NaN();
  // The SERVED price's error statement in USD against the cold accurate kernel, classed
  // by certificate_class: 0 (Exact) for a node's bytes; the ladder's statement (its
  // class); K3's per-point bound at the contract on the Family tier (RefinementValidated);
  // the Fast kernel's tested bound for a Fast confirm (KernelTested); NaN (None) for the
  // piecewise-linear slice between nodes, which claims none (review-H0A m3).
  double certificate_usd = std::numeric_limits<double>::quiet_NaN();
  ServeBoundClass certificate_class = ServeBoundClass::None;
  // The Greek map's own statement when the Greeks come from another map than the price
  // (at the fit's spot): the ladder's or the family's per-point bound; NaN / None when the
  // price and the Greeks share one map or there is no Greek map.
  double map_certificate_usd = std::numeric_limits<double>::quiet_NaN();
  ServeBoundClass map_certificate_class = ServeBoundClass::None;
  // True when any statement above is Sampled: such an answer is never "certified".
  bool sampled = false;
  // The distance to the exercise edge in sigma sqrt T that masked theta / gamma (the band
  // above): the ladder's from the node's converged edge (ServedNodeSide::edge_spot), the
  // family's from A0's bundle (its own discretised edge). NaN when the answering map has
  // no edge (no A0 bundle, refusal, slice-only answer); then no theta / gamma is served.
  double edge_sd = std::numeric_limits<double>::quiet_NaN();
  bool near_edge = false; // continuation and edge_sd < kServeEdgeBand: theta, gamma withheld
  std::int64_t age_ns = 0;
  ServeTier tier = ServeTier::Cold;
  ServeStatus status = ServeStatus::TierUnavailable;
  ServeRoute route = ServeRoute::ColdPath;
  ServePriceSource price_source = ServePriceSource::None;
  ServeGreekSource greek_source = ServeGreekSource::None;
  ServeGreekConvention convention{};
};

struct ServeIvRequest {
  double strike = 0.0;
  Side side = Side::Call;
  double spot = 0.0;
  double target_price = 0.0;
  ServeTier tier = ServeTier::Ladder;
  ServeTier tier_max = ServeTier::Certified;
  // Unix ns; required (> 0) when the policy's max_age_ns > 0 (review-H0A m1 / m2).
  std::int64_t now_ns = 0;
};

struct ServeIvAnswer {
  double sigma = std::numeric_limits<double>::quiet_NaN();
  double price_residual = std::numeric_limits<double>::quiet_NaN(); // map(sigma) - target
  double newton_distance_vol = std::numeric_limits<double>::quiet_NaN();
  ServeRootAcceptance acceptance{};
  std::uint8_t evaluations = 0; // map evaluations over every tier tried
  std::int64_t age_ns = 0;
  ServeTier tier = ServeTier::Cold;
  ServeStatus status = ServeStatus::TierUnavailable;
  ServeRoute route = ServeRoute::ColdPath;
};

struct SliceServeRecordAccess; // slice_serve_record.cpp: the only maker of records

class SliceServeRecord {
public:
  struct Impl; // slice_serve_record.cpp

  // Only the builder can make the key, so no record exists without a built Impl
  // (review-H0A m8): every read dereferences a non-null Impl.
  class Key {
    Key() = default;
    friend struct SliceServeRecordAccess;
  };
  SliceServeRecord(Key key, std::unique_ptr<const Impl> impl) noexcept;

  ~SliceServeRecord();
  SliceServeRecord(const SliceServeRecord &) = delete;
  SliceServeRecord &operator=(const SliceServeRecord &) = delete;
  SliceServeRecord(SliceServeRecord &&) = delete;
  SliceServeRecord &operator=(SliceServeRecord &&) = delete;

  [[nodiscard]] std::span<const ServedNode> nodes() const noexcept;
  [[nodiscard]] const ServeSupport &support() const noexcept;
  [[nodiscard]] const ServeRecordVersion &version() const noexcept;
  [[nodiscard]] const ServeRecordBytes &bytes() const noexcept;
  [[nodiscard]] const ServeBuildClocks &clocks() const noexcept;
  [[nodiscard]] const ServeRecordPolicy &policy() const noexcept;
  [[nodiscard]] const IVolCurve &curve() const noexcept;
  [[nodiscard]] double spot() const noexcept;  // the fit's spot
  [[nodiscard]] double tau() const noexcept;   // variance clock T
  [[nodiscard]] double rate() const noexcept;  // effective rate on the variance clock
  [[nodiscard]] double strike_lo() const noexcept;
  [[nodiscard]] double strike_hi() const noexcept;

  // Price and the requested Greeks of (strike, side) at `spot` (see the tier contract in
  // the file header). The Ladder and Family tiers never allocate or throw; the Certified
  // tier's Fast solve at a moved spot is andersen_lake (it may allocate, so std::bad_alloc
  // is the only exception that can leave this call).
  [[nodiscard]] ServeAnswer serve(const ServeRequest &request) const;

  // The volatility at which the tier's map reproduces `target_price` at `spot`, walking
  // from request.tier to request.tier_max as serve() does: Ladder (a RefinementValidated
  // ladder's sigma axis, Newton with a bisection bracket, Ok only when its bound / slope
  // + the Newton distance is within iv_tolerance; a Sampled ladder is never Ok), Family
  // (Newton on the family's fused price and vega inside its box, accepted by K3's
  // enclosure where the envelope covers it, else Unconfirmed), Certified (the family root
  // and one AlProfile::Fast confirm: Newton distance <= iv_tolerance). The L1c
  // non-monotone gate (al_non_monotone_gate) runs first: a target inside the band is
  // NonMonotoneBand, routed to the cold accurate inversion. tier == Cold is a route only:
  // TierUnavailable / ColdPath, no solve (review-H0A m5). Stale like serve(). Allocation
  // and exceptions as serve(): only the Certified tier's Fast confirm can allocate.
  [[nodiscard]] ServeIvAnswer implied_vol(const ServeIvRequest &request) const;

private:
  friend struct SliceServeRecordAccess;
  std::unique_ptr<const Impl> impl_;
};

// Builds the record of a certified fit (fit_status Success with prices and a curve):
// identifies every node side through query_quote_slice, obtains the side families
// (ServeFamilyMode::Build: from the fit's serve_handoff when it carries one, re-prepared
// and verified; otherwise the record's own recipe; both certified through lane K3's
// deam_family_certify; DatedCash, an expiry below kAlSigmaFamilyMinTau and a refused side
// build none and say why in support()), samples the ladders and classes their statements,
// measures price_map_delta and the between-node audit, and stamps the version. Allocates;
// the fit is read only and may be destroyed afterwards.
// @return InvalidArgument for an invalid policy; Unavailable for a fit without a
//         certified slice; the first query error otherwise.
[[nodiscard]] Result<std::shared_ptr<const SliceServeRecord>>
build_serve_record(const QuoteSliceFit &fit, const ServeRecordPolicy &policy = {});

[[nodiscard]] const char *to_string(ServeTier value) noexcept;
[[nodiscard]] const char *to_string(ServeStatus value) noexcept;
[[nodiscard]] const char *to_string(ServeRoute value) noexcept;
[[nodiscard]] const char *to_string(ServeGreekReason value) noexcept;
[[nodiscard]] const char *to_string(ServeUnsupportedReason value) noexcept;
[[nodiscard]] const char *to_string(ServeBoundClass value) noexcept;

} // namespace atx::vol
