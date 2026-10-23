#pragma once

// Served analytics (lane A1, r6 2026-10-23; master plan Sprint 5 package A1, Gate G6; deep
// review F11 / F13): hedge Greeks, bounded Taylor scenarios next to exact repricing, P&L
// attribution and deterministic portfolio aggregation on the LIVE serving book
// (serve_book.hpp) - every output declares the model, the version and the convention it
// is made of. docs/analytics-contracts.md is the contract; this header is its API.
//
// ## One declared model and version per answer
//
// Every output carries an AnalyticsDeclaration: the book version that answered (the
// ServeBookVersion number), the expiry's record identity (ServeRecordVersion digests), the
// model that produced the numbers (AnalyticsModel), the map the Greeks differentiate
// (ServeGreekModel), the hedge convention and the clock (now, the record's as-of instant,
// its variance clock T and the entry's expiry / event instants). Price and Greeks of one
// answer come from one map, or price_map_delta says how far apart the two maps are (H0-A).
// A portfolio answer (scenarios, attribution) reads ONE book version for every leg: when a
// publication lands mid-run the whole computation runs again once, and a second move is
// the typed AnalyticsStatus::VersionMoved (no totals).
//
// ## Certified inputs (H1's statements, review-H1 F1 / F2)
//
// classify_served_answer() decides what a served answer may feed:
//   Certified  status Ok or Exercised, not Sampled, and a statement backs the price
//              (Exact node bytes, K3's RefinementValidated bound, the Fast kernel's
//              KernelTested bound);
//   Estimate   answered, but Sampled or without a statement (class None, e.g. the
//              piecewise-linear slice between nodes): NEVER a certified input. Its numbers
//              are kept with AnalyticsStatus::Estimate and summed apart from certified
//              totals, never into them;
//   PriceOnly  C0's price-only node: the price and its typed reason, no Greek map;
//   Refused    no number (the record's or the book's refusal, with its route).
// No output reports AnalyticsStatus::Ok on an input that is not Certified.
//
// ## Hedge conventions (F11)
//
// What a hedge Greek holds fixed when the spot moves (values equal A0's
// AlFamilyGreekHeldFixed):
//   ContractSigma         the contract's served volatility: the record's own Greeks;
//   StickyStrike          sigma(K) at the strike: on a record (sticky strike by
//                         construction) these are the ContractSigma numbers;
//   StickyDeltaMoneyness  the smile is a function of ln(K / F) and moves with the spot
//                         (sticky delta and sticky moneyness coincide when the smile of
//                         one expiry is a function of K / F): delta gains vega x
//                         dsigma/dS with dsigma/dS = -sigma'(k) / S, sigma'(k) the fitted
//                         curve's slope at k = ln(K / F_fit) by a central difference
//                         (SmileSlopeClass::FittedCurveCentralDifference: a model
//                         statement of the smile, no certificate). Its gamma needs
//                         vanna, volga and sigma'' terms no tier serves: NaN with
//                         NeedsVannaVolga. Vega and theta are the sticky-strike numbers;
//   RefittedSurface       a refit per bump is the fitter's job, not a read: every Greek NaN
//                         with RequiresRefit (the price is still served).
// Theta is the record's: the continuation-region identity, per year of the record's
// variance clock (ServeGreekConvention::theta_pde_identity); a roll through expiry, an
// earnings date or an ex-dividend date is an EVENT operation, never theta.
//
// ## Scenarios (F13)
//
// A scenario matrix is spot_rel x vol_abs x time_years (row-major in that order). Every
// unique contract is answered ONCE at the base spot (one served answer: price, delta,
// gamma, vega, theta) and every cell is evaluated two ways, labelled and never mixed:
//   Taylor  delta dS + gamma dS^2 / 2 + vega dsigma + theta dt: declared order (spot 2,
//           vol 1, time 1, no cross terms), declared validity box (TaylorBox; a cell
//           outside it is excluded, OutsideTaylorBox) and a remainder estimate (class
//           Sampled, never a certificate): spot  dS^2 / 2 x max |gamma(S (1 +/- b)) -
//           gamma(S)| (the record's gamma at the box ends; exact when gamma is monotone
//           on each side), vol and time  the first-order residual measured on the cold
//           kernel at the box edge, scaled by (shock / edge)^2. Cross terms (vanna,
//           charm) are outside the estimate; the A1 row measures them against exact.
//           Every term is gated on its own shock (a zero shock never reads its Greek).
//   Exact   ONE map per cell: a pure spot cell is the record at S' (route RecordTier,
//           the answer's statement class) minus the record at S; any cell with a vol or
//           time shock is the cold accurate kernel (andersen_lake at the served sigma +
//           dsigma, the record's rate, the node's yield, T - dt; route ColdKernel) minus
//           the same kernel at the base. A roll through expiry (dt >= T) is the payoff at
//           S' (route EventSettlement); a roll through the entry's next event (ex-
//           dividend, earnings: ServeBookEntry::next_event_ns) is an event the record
//           cannot price (EventRequiresRefit) and Taylor never steps across an event
//           (EventOperation). dt is on the record's variance clock; an event instant is
//           located by mapping dt linearly onto [now_ns, expiry_ns].
// Every cell reports, per exclusion reason, the legs excluded and their base premium
// (|quantity x multiplier x base price|), for Taylor and Exact separately: a finite total
// never hides what it left out.
//
// ## Aggregation
//
// Unique contracts are evaluated in parallel (ScenarioSpec::workers std::jthread workers,
// never the pricing pool; one ServeBookReader per worker); every total is a Neumaier
// (improved Kahan) compensated sum over the portfolio's legs in INPUT order on the calling
// thread, so a result is bit-identical for any worker count. Units: per-share values x
// quantity (contracts, signed) x multiplier (shares per contract) = USD. A leg's Taylor
// value is its four position-scaled terms summed left to right (delta + gamma + vega +
// theta, each x quantity x multiplier) - the attribution's `explained`, bit for bit - and
// its exact value is the per-share exact P&L x quantity x multiplier.
//
// ## Dated cash dividends (D1)
//
// A record does not serve dated cash (ServeCashDividends::UnsupportedInRecord): such a
// contract runs on D1's production lattice (cash_dividend_scenarios), whose time roll
// across an ex-date is D1's event operation (the cash comes off the stock and off the
// schedule), never a Taylor step; D1's unsupported Greeks keep D1's reasons.
//
// ## Threads
//
// hedge_greeks uses the caller's reader (one per thread). served_scenarios /
// served_attribution take the book (const) and make one reader per worker. Pure
// otherwise: concurrent calls on one book are safe.

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <vector>

#include "atx/vol/api/core/types.hpp" // Side, Result, Status
#include "atx/vol/api/fitting/serve_book.hpp"
#include "atx/vol/api/fitting/slice_serve_record.hpp"
#include "atx/vol/api/pricing/american.hpp" // AlOpts, AlFamilyGreekHeldFixed, CashDividend
#include "atx/vol/api/pricing/american_discrete_div_kernel.hpp" // D1's production lattice

namespace atx::vol {

// ── Declarations ─────────────────────────────────────────────────────────────────────────

// The model that produced an output's numbers.
enum class AnalyticsModel : std::uint8_t {
  None = 0,
  QuotingRecord = 1,       // a SliceServeRecord answer (the American quoting surface)
  ColdKernel = 2,          // andersen_lake at the record's served sigma, rate and yield
  EventSettlement = 3,     // a roll through expiry: the payoff
  DiscreteDividend = 4,    // D1's production lattice (dated cash)
  SessionCurve = 5,        // a curve session's cold route (session_serve_book.hpp)
  CommonModelEuropean = 6, // the Markov common model (common_model_analytics.hpp)
};

enum class HedgeConvention : std::uint8_t {
  ContractSigma = 0,
  StickyStrike = 1,
  StickyDeltaMoneyness = 2,
  RefittedSurface = 3,
};

struct AnalyticsClock {
  std::int64_t now_ns = 0;        // the request's clock (Unix ns)
  std::int64_t as_of_ns = 0;      // the record's ServeRecordVersion::as_of_ns
  std::int64_t expiry_ns = 0;     // the book entry's instants (0: none)
  std::int64_t next_event_ns = 0;
  double tau = std::numeric_limits<double>::quiet_NaN(); // the record's variance clock T
};

struct AnalyticsDeclaration {
  std::uint64_t book_version = 0; // ServeBookVersion::number that answered (0: no book)
  std::uint64_t expiry_id = 0;
  std::uint64_t entry_tag = 0;    // ServeBookEntry::tag, echoed
  ServeDigest record_fit_sha256{};    // the record's version id (ServeRecordVersion)
  ServeDigest record_policy_sha256{};
  ServeDigest record_input_sha256{};
  AnalyticsModel model = AnalyticsModel::None;
  ServeGreekModel greek_model = ServeGreekModel::None; // the map the Greeks differentiate
  HedgeConvention convention = HedgeConvention::ContractSigma;
  AnalyticsClock clock{};
  // |the Greek map's price - the served price| (0: one map; NaN: no Greek map).
  double price_map_delta = std::numeric_limits<double>::quiet_NaN();
};

enum class AnalyticsInputClass : std::uint8_t {
  Certified = 0,
  Estimate = 1,
  PriceOnly = 2,
  Refused = 3,
};

enum class AnalyticsStatus : std::uint8_t {
  Ok = 0,              // every input Certified
  Estimate = 1,        // numbers from an uncertified answer: never Ok
  PriceOnly = 2,       // C0's price-only answer: price and reason, no Greeks
  Refused = 3,         // the book or the record refused (serve_status and route say why)
  VersionMoved = 4,    // the book published twice while a portfolio answer ran
  InvalidArgument = 5, // the request (or the convention for this entry) is not admissible
};

// The input class of a served answer / a served chain statement (see the file header).
[[nodiscard]] AnalyticsInputClass classify_served_answer(const ServeAnswer &answer) noexcept;
[[nodiscard]] AnalyticsInputClass classify_served_statement(const ServedChainStatement &statement,
                                                            double price) noexcept;

// ── Hedge Greeks ─────────────────────────────────────────────────────────────────────────

inline constexpr std::size_t kHedgeGreekCount = 4; // delta, gamma, vega, theta

// Why a hedge Greek holds what it holds: the record's ServeGreekReason values verbatim
// (0-34: Served, Exercised, NearExerciseEdge, TierHasNoGreek, NoMap, ...) and A1's own
// from 64. Served and Exercised carry a number; every other reason carries NaN.
enum class HedgeGreekReason : std::uint8_t {
  NotRequested = 0,
  Served = 1,
  Exercised = 2,
  Refused = 3,
  NoCarryBumps = 4,
  CarryBumpRefused = 5,
  NoQAxis = 6,
  NearNodeDegenerate = 7,
  ConventionUnsupported = 8,
  NearExerciseEdge = 9,
  BundleUnavailable = 32,
  TierHasNoGreek = 33,
  NoMap = 34,
  SmileSlopeUnavailable = 64, // StickyDeltaMoneyness: no finite curve slope at the strike
  NeedsVannaVolga = 65,       // StickyDeltaMoneyness gamma: second-order smile terms
  RequiresRefit = 66,         // RefittedSurface: a refit per bump
};

enum class SmileSlopeClass : std::uint8_t {
  None = 0,                         // no slope term (refused, or no curve slope)
  ZeroByConvention = 1,             // ContractSigma / StickyStrike: dsigma/dS = 0
  FittedCurveCentralDifference = 2, // StickyDeltaMoneyness: the fitted curve's slope
};
// The central-difference step in k = ln(K / F) for the fitted curve's slope.
inline constexpr double kSmileSlopeStep = 1.0e-4;

struct HedgeGreeksRequest {
  std::uint64_t expiry_id = 0;
  double strike = 0.0;
  Side side = Side::Call;
  double spot = 0.0;
  std::int64_t now_ns = 0;
  HedgeConvention convention = HedgeConvention::StickyStrike;
  ServeTier tier = ServeTier::Ladder;
  ServeTier tier_max = ServeTier::Family;
};

struct HedgeGreeks {
  double price = std::numeric_limits<double>::quiet_NaN();
  double delta = std::numeric_limits<double>::quiet_NaN();
  double gamma = std::numeric_limits<double>::quiet_NaN();
  double vega = std::numeric_limits<double>::quiet_NaN();
  double theta = std::numeric_limits<double>::quiet_NaN();
  std::array<HedgeGreekReason, kHedgeGreekCount> reason{}; // delta, gamma, vega, theta
  double sigma = std::numeric_limits<double>::quiet_NaN(); // the served volatility
  // The convention's smile term: dsigma/dS (0 by convention for ContractSigma and
  // StickyStrike) and sigma'(k) of the fitted curve it came from.
  double dsigma_dspot = std::numeric_limits<double>::quiet_NaN();
  double curve_slope = std::numeric_limits<double>::quiet_NaN();
  SmileSlopeClass slope_class = SmileSlopeClass::None;
  // The served answer's statement (H1), carried verbatim.
  double certificate_usd = std::numeric_limits<double>::quiet_NaN();
  ServeBoundClass certificate_class = ServeBoundClass::None;
  bool sampled = false;
  QuoteSlicePriceOnlyReason price_only_reason = QuoteSlicePriceOnlyReason::None;
  double iv_lower = std::numeric_limits<double>::quiet_NaN();
  double iv_upper = std::numeric_limits<double>::quiet_NaN();
  double edge_sd = std::numeric_limits<double>::quiet_NaN();
  bool near_edge = false;
  ServeStatus serve_status = ServeStatus::TierUnavailable;
  ServeRoute route = ServeRoute::ColdPath;
  ServeTier tier = ServeTier::Cold;
  AnalyticsInputClass input_class = AnalyticsInputClass::Refused;
  AnalyticsStatus status = AnalyticsStatus::Refused;
  AnalyticsDeclaration declaration{};
};

// One served answer (reader.serve, Greeks four, family_bound true) turned into the
// convention's hedge Greeks. Never throws past std::bad_alloc from a Certified tier;
// InvalidArgument status for a non-finite / non-positive strike or spot.
[[nodiscard]] HedgeGreeks hedge_greeks(ServeBookReader &reader, const HedgeGreeksRequest &request);

// ── Portfolios and scenarios ─────────────────────────────────────────────────────────────

struct ServedPosition {
  std::uint64_t leg_id = 0;
  std::uint64_t expiry_id = 0;
  double strike = 0.0;
  Side side = Side::Call;
  double quantity = 0.0;     // contracts, signed
  double multiplier = 100.0; // shares per contract
};

// The Taylor validity box: |spot_rel| <= spot_rel, |vol_abs| <= vol_abs and
// 0 <= time_years <= time_years. A cell outside it is excluded from Taylor.
struct TaylorBox {
  double spot_rel = 0.03;
  double vol_abs = 0.03;
  double time_years = 7.0 / 365.0;
};

// The declared Taylor order (no cross terms: no tier serves vanna or charm).
struct TaylorOrder {
  std::uint8_t spot = 2;
  std::uint8_t vol = 1;
  std::uint8_t time = 1;
  bool cross_terms = false;
};

enum class ScenarioMethod : std::uint8_t { Taylor = 0, Exact = 1, Both = 2 };

enum class ScenarioExactRoute : std::uint8_t {
  None = 0,
  RecordTier = 1,      // the record at S' minus the record at S
  ColdKernel = 2,      // andersen_lake at the shocked inputs minus at the base
  EventSettlement = 3, // a roll through expiry: payoff at S' minus the base price
  DiscreteDividend = 4 // D1's production lattice (cash_dividend_scenarios)
};

enum class ScenarioExclusion : std::uint8_t {
  None = 0,
  Refused = 1,            // the base answer refused (no number)
  PriceOnly = 2,          // the base answer is price-only (C0)
  Estimate = 3,           // an uncertified input (Sampled / no statement): summed apart
  MissingGreek = 4,       // Taylor: a Greek the cell's shocks need is NaN (its reason)
  OutsideTaylorBox = 5,   // Taylor: the cell is outside the validity box
  RemainderUnbounded = 6, // Taylor: a gamma at the box ends is NaN (band, refusal)
  EventOperation = 7,     // Taylor: the time shock crosses expiry or an event
  EventRequiresRefit = 8, // Exact: the time shock crosses the entry's next event
  ExactRefused = 9,       // Exact: the shocked reprice refused
  OffNode = 10,           // Exact (cold kernel): no node yield for this strike
};
inline constexpr std::size_t kScenarioExclusionCount = 11;

struct ScenarioSpec {
  std::vector<double> spot_rel{0.0};   // S' = S (1 + x)
  std::vector<double> vol_abs{0.0};    // sigma' = sigma + y, floored at kScenarioSigmaFloor
  std::vector<double> time_years{0.0}; // dt >= 0 on the record's variance clock
  double spot = 0.0;                   // the base spot (> 0)
  std::int64_t now_ns = 0;             // required (> 0) when an entry carries instants
  ScenarioMethod method = ScenarioMethod::Both;
  TaylorBox box{};
  // The cold kernel's preset for exact cells with a vol or time shock (nullopt: the
  // accurate preset - the kernel the records' statements are measured against).
  std::optional<AlOpts> cold_opts = std::nullopt;
  ServeTier tier = ServeTier::Ladder; // the cheapest tier (Family: one map at every spot)
  ServeTier tier_max = ServeTier::Family;
  unsigned workers = 1;    // std::jthread workers over unique contracts (1: inline)
  bool keep_legs = false;  // keep the per-(unique, cell) values (ScenarioResult::cells)
  // Scenarios hold the served volatility (ContractSigma / StickyStrike); the other two
  // conventions are InvalidArgument here.
  HedgeConvention convention = HedgeConvention::StickyStrike;
};
inline constexpr double kScenarioSigmaFloor = 1.0e-4;

// What the run evaluated, fixed before it ran (the A1 row's workload line).
struct ServedWorkload {
  std::size_t contracts = 0;        // portfolio legs
  std::size_t unique_contracts = 0; // distinct (expiry_id, strike, side)
  ServeGreekMask greek_mask = ServeGreekMask::four();
  std::size_t n_spot = 0;
  std::size_t n_vol = 0;
  std::size_t n_time = 0;
  std::size_t n_cells = 0;
  std::size_t expiries = 0;         // distinct expiry ids
  std::size_t events_in_horizon = 0; // expiries whose expiry / next event lies inside the
                                     // largest time shock
};

// Per-leg provenance: what the base answer was and which version / record it came from.
struct ServedLegProvenance {
  std::uint64_t leg_id = 0;
  std::size_t unique_index = 0;
  double base_price = std::numeric_limits<double>::quiet_NaN(); // per share, served
  double cold_base_price = std::numeric_limits<double>::quiet_NaN(); // the cold kernel's
  double delta = std::numeric_limits<double>::quiet_NaN();
  double gamma = std::numeric_limits<double>::quiet_NaN();
  double vega = std::numeric_limits<double>::quiet_NaN();
  double theta = std::numeric_limits<double>::quiet_NaN();
  std::array<HedgeGreekReason, kHedgeGreekCount> reason{};
  double sigma = std::numeric_limits<double>::quiet_NaN();
  double edge_sd = std::numeric_limits<double>::quiet_NaN();
  double certificate_usd = std::numeric_limits<double>::quiet_NaN();
  ServeBoundClass certificate_class = ServeBoundClass::None;
  bool sampled = false;
  QuoteSlicePriceOnlyReason price_only_reason = QuoteSlicePriceOnlyReason::None;
  ServeStatus serve_status = ServeStatus::TierUnavailable;
  ServeRoute route = ServeRoute::ColdPath;
  ServeTier tier = ServeTier::Cold;
  AnalyticsInputClass input_class = AnalyticsInputClass::Refused;
  AnalyticsDeclaration declaration{};
};

// One (unique contract, cell): per-share values, kept when ScenarioSpec::keep_legs.
struct ScenarioLegCell {
  double taylor = std::numeric_limits<double>::quiet_NaN();    // Taylor P&L per share
  double taylor_delta = std::numeric_limits<double>::quiet_NaN(); // its terms, per share
  double taylor_gamma = std::numeric_limits<double>::quiet_NaN();
  double taylor_vega = std::numeric_limits<double>::quiet_NaN();
  double taylor_theta = std::numeric_limits<double>::quiet_NaN();
  double remainder = std::numeric_limits<double>::quiet_NaN(); // its estimate (Sampled)
  double exact = std::numeric_limits<double>::quiet_NaN();     // exact P&L per share
  double exact_certificate_usd = std::numeric_limits<double>::quiet_NaN();
  ServeBoundClass exact_class = ServeBoundClass::None;
  ScenarioExactRoute exact_route = ScenarioExactRoute::None;
  ScenarioExclusion taylor_exclusion = ScenarioExclusion::None;
  ScenarioExclusion exact_exclusion = ScenarioExclusion::None;
};

using ScenarioExclusionCounts = std::array<std::uint32_t, kScenarioExclusionCount>;
using ScenarioExclusionUsd = std::array<double, kScenarioExclusionCount>;

struct ScenarioCell {
  double spot_rel = 0.0;
  double vol_abs = 0.0;
  double time_years = 0.0;
  // Taylor: certified legs only, and the four terms (position-scaled USD).
  double taylor_pnl = 0.0;
  double taylor_delta = 0.0;
  double taylor_gamma = 0.0;
  double taylor_vega = 0.0;
  double taylor_theta = 0.0;
  double taylor_remainder = 0.0; // sum of |leg remainder estimate| (Sampled)
  // Exact: certified legs only.
  double exact_pnl = 0.0;
  double exact_certificate_usd = 0.0; // sum of |leg statement| over RecordTier legs
  // Estimate legs (uncertified inputs), summed apart - never into the totals above.
  double estimate_taylor_pnl = 0.0;
  double estimate_exact_pnl = 0.0;
  // Like for like: the legs in BOTH the Taylor and the exact totals only. The two totals
  // above may hold different legs (a leg excluded from one method only, e.g. a missing
  // gamma or an unbounded remainder on the Taylor side), so taylor_pnl - exact_pnl is not
  // a Taylor error; matched_taylor_pnl - matched_exact_pnl is, against matched_remainder.
  double matched_taylor_pnl = 0.0;
  double matched_exact_pnl = 0.0;
  double matched_remainder = 0.0;
  std::uint32_t taylor_legs = 0;
  std::uint32_t exact_legs = 0;
  std::uint32_t matched_legs = 0;
  std::array<std::uint32_t, 5> exact_routes{}; // legs per ScenarioExactRoute
  ScenarioExclusionCounts taylor_excluded{};
  ScenarioExclusionUsd taylor_excluded_usd{}; // base premium of the excluded legs
  ScenarioExclusionCounts exact_excluded{};
  ScenarioExclusionUsd exact_excluded_usd{};
};

struct ScenarioResult {
  AnalyticsStatus status = AnalyticsStatus::Ok;
  std::uint64_t book_version = 0; // the ONE version every leg read
  ServedWorkload workload{};
  TaylorOrder order{};
  TaylorBox box{};
  HedgeConvention convention = HedgeConvention::StickyStrike;
  std::size_t n_spot = 0;
  std::size_t n_vol = 0;
  std::size_t n_time = 0;
  std::vector<ScenarioCell> cell; // row-major [spot][vol][time]
  std::vector<ServedLegProvenance> legs;     // portfolio order
  std::vector<ScenarioLegCell> cells;        // [unique][cell] when keep_legs
  std::uint32_t attempts = 0;                // runs (2: one publication landed mid-run)
  std::size_t base_serves = 0;               // reader.serve calls for the base answers
  std::size_t exact_record_serves = 0;
  std::size_t cold_solves = 0;
  [[nodiscard]] std::size_t index(std::size_t i_spot, std::size_t i_vol,
                                  std::size_t i_time) const noexcept {
    return (i_spot * n_vol + i_vol) * n_time + i_time;
  }
};

// The scenario matrix of `portfolio` on `book` (file header). InvalidArgument for an empty
// axis, a non-finite shock, a negative time shock, a non-positive spot, a non-finite
// quantity / multiplier or an unsupported convention.
[[nodiscard]] Result<ScenarioResult> served_scenarios(const ServeBook &book,
                                                      std::span<const ServedPosition> portfolio,
                                                      const ScenarioSpec &spec);

// ── P&L attribution ──────────────────────────────────────────────────────────────────────

// The move a portfolio is attributed over: one scenario cell.
struct ServedMove {
  double spot_rel = 0.0;
  double vol_abs = 0.0;
  double time_years = 0.0;
};

struct ServedLegAttribution {
  std::uint64_t leg_id = 0;
  double delta = std::numeric_limits<double>::quiet_NaN(); // position-scaled USD terms
  double gamma = std::numeric_limits<double>::quiet_NaN();
  double vega = std::numeric_limits<double>::quiet_NaN();
  double theta = std::numeric_limits<double>::quiet_NaN();
  double explained = std::numeric_limits<double>::quiet_NaN(); // the Taylor sum
  double actual = std::numeric_limits<double>::quiet_NaN();    // the exact P&L
  double unexplained = std::numeric_limits<double>::quiet_NaN(); // actual - explained
  ScenarioExactRoute route = ScenarioExactRoute::None;
  ScenarioExclusion taylor_exclusion = ScenarioExclusion::None;
  ScenarioExclusion exact_exclusion = ScenarioExclusion::None;
};

struct ServedAttribution {
  AnalyticsStatus status = AnalyticsStatus::Ok;
  ServedMove move{};
  std::vector<ServedLegAttribution> legs; // portfolio order
  // Totals over legs where both Taylor and Exact are included (Neumaier, leg order).
  double delta = 0.0;
  double gamma = 0.0;
  double vega = 0.0;
  double theta = 0.0;
  double explained = 0.0;
  double actual = 0.0;
  double unexplained = 0.0;
  std::uint32_t attributed_legs = 0;
  ScenarioCell cell{};             // the scenario cell it was computed from
  ScenarioResult scenario{};       // provenance, workload, version
};

// The attribution of `move` over `portfolio` (one ScenarioSpec cell; spec's axes are
// replaced by the move). Errors as served_scenarios.
[[nodiscard]] Result<ServedAttribution>
served_attribution(const ServeBook &book, std::span<const ServedPosition> portfolio,
                   const ServedMove &move, const ScenarioSpec &spec);

// ── Deterministic aggregation ────────────────────────────────────────────────────────────

// Neumaier (improved Kahan) compensated sum in the given order. The aggregation every
// total above uses.
class NeumaierSum {
public:
  void add(double value) noexcept;
  [[nodiscard]] double value() const noexcept { return sum_ + compensation_; }

private:
  double sum_ = 0.0;
  double compensation_ = 0.0;
};

// ── Dated cash dividends (D1's production lattice) ───────────────────────────────────────

struct CashDividendContract {
  std::uint64_t leg_id = 0;
  double spot = 0.0;
  double strike = 0.0;
  double tau = 0.0; // years to expiry (D1's clock)
  double sigma = 0.0;
  double rate = 0.0;
  double yield = 0.0; // residual continuous yield on top of the cash
  Side side = Side::Call;
  ExerciseStyle exercise = ExerciseStyle::American;
  std::vector<CashDividend> dividends;
  double quantity = 1.0;
  double multiplier = 100.0;
};

struct CashDividendScenarioSpec {
  std::vector<double> spot_rel{0.0};
  std::vector<double> vol_abs{0.0};
  std::vector<double> time_years{0.0};
  TaylorBox box{};
  DiscreteDivProductionOpts opts{};
};

struct CashDividendLegCell {
  double taylor = std::numeric_limits<double>::quiet_NaN();
  double remainder = std::numeric_limits<double>::quiet_NaN();
  double exact = std::numeric_limits<double>::quiet_NaN();
  int events_crossed = 0;
  double cash_crossed = 0.0;
  ScenarioExactRoute exact_route = ScenarioExactRoute::None;
  ScenarioExclusion taylor_exclusion = ScenarioExclusion::None;
  ScenarioExclusion exact_exclusion = ScenarioExclusion::None;
};

struct CashDividendLeg {
  std::uint64_t leg_id = 0;
  DiscreteDivProductionGreeks base{}; // D1's bundle at the base (its reasons verbatim)
  AnalyticsDeclaration declaration{};
  std::vector<CashDividendLegCell> cells; // row-major [spot][vol][time]
};

struct CashDividendScenarioResult {
  std::size_t n_spot = 0;
  std::size_t n_vol = 0;
  std::size_t n_time = 0;
  TaylorOrder order{};
  TaylorBox box{};
  std::vector<CashDividendLeg> legs;
  std::vector<ScenarioCell> cell; // totals, as ScenarioResult::cell
  std::size_t lattice_solves = 0;
};

// The scenario matrix of dated-cash contracts on D1's production lattice (file header).
// InvalidArgument as served_scenarios; a contract D1 refuses is excluded (Refused).
[[nodiscard]] Result<CashDividendScenarioResult>
cash_dividend_scenarios(std::span<const CashDividendContract> contracts,
                        const CashDividendScenarioSpec &spec);

[[nodiscard]] const char *to_string(AnalyticsModel value) noexcept;
[[nodiscard]] const char *to_string(HedgeConvention value) noexcept;
[[nodiscard]] const char *to_string(AnalyticsInputClass value) noexcept;
[[nodiscard]] const char *to_string(AnalyticsStatus value) noexcept;
[[nodiscard]] const char *to_string(HedgeGreekReason value) noexcept;
[[nodiscard]] const char *to_string(ScenarioExactRoute value) noexcept;
[[nodiscard]] const char *to_string(ScenarioExclusion value) noexcept;

} // namespace atx::vol
