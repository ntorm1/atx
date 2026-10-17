#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "atx/vol/api/fitting/vol_curve.hpp"
#include "atx/vol/api/fitting/american_price_slice.hpp"
#include "atx/vol/api/fitting/quote_slice_carry.hpp"
#include "atx/vol/api/pricing/american.hpp"

namespace atx::vol {

// A single expiry, with separate variance and funding clocks. Volatility is
// annualized on variance_years; rate and continuous_yield on calendar_years.
// continuous_yield excludes any separately supplied cash dividends.
struct QuoteSliceMarket {
  double spot = 0.0;
  double variance_years = 0.0;
  double calendar_years = 0.0;
  double rate = 0.0;
  double continuous_yield = 0.0;
  double dividend_pv = 0.0;
};

struct QuoteSliceQuote {
  double strike = 0.0;
  Side side = Side::Call;
  double bid = 0.0;
  double ask = 0.0;
  // Optional contract carry, annualized on the CALENDAR clock and excluding
  // separately supplied cash dividends. Missing means the market reference
  // yield. Matched calls/puts at one strike must resolve to the same yield.
  std::optional<double> continuous_yield{};
};

enum class QuoteSliceDividendModel : std::uint8_t {
  ContinuousYield,
  // APPROXIMATION: replace aggregate cash PV with an equivalent continuous
  // yield. This preserves (spot - PV)*exp((rate-yield)*calendar_years), but
  // cannot preserve exercise decisions at unknown ex-dividend dates.
  AggregatePresentValue,
  // APPROXIMATION with a FUNDING-discounted cash PV: preserve
  // spot*exp((rate-yield)*calendar_years) - PV*exp(rate*calendar_years).
  // Equivalently, terminal prepaid stock is spot*exp(-yield*T)-PV.
  // This differs from escrowed AggregatePresentValue when yield != 0.
  // As with that model, missing ex-dates prevent exact exercise modeling.
  AggregateFundingPresentValue,
  // Use the discrete-cash lattice and the supplied event dates for BOTH IV
  // inversion and repricing. dividend_pv must be zero to avoid double counting.
  DatedCash
};

enum class QuoteSliceSidePolicy : std::uint8_t {
  All,
  // Puts below their local forward, calls above; both exactly at the forward. Optional
  // atm_log_band admits both sides within |log(K/F)| <= atm_log_band.
  OutOfTheMoney
};

// Policy for point observations, applied to every curve family. A point target
// always requires 0 < bid < ask: a missing bid is an upper-bound observation,
// not evidence that the fair price is half the ask. Such contracts are still
// repriced from an available fit. Bound-constrained fitting is not provided.
struct QuoteSliceQualityConfig {
  double min_bid = 0.0; // additional absolute floor; a bid must still be positive
  double max_spread_to_mid = 0.60; // full raw spread / midpoint; 0 disables
  double max_spread_vol = 0.05; // full floored spread / local Black vega; 0 disables
  double min_vega_weight = 1.0e-6; // (local Black vega / floored spread)^2; 0 disables
  // Upper clip on inverse total-variance uncertainty, matching CalibOpts.
  // This controls absolute weight scale, including spline roughness penalties.
  double max_weight = 1.0e3;
};

enum class QuoteSliceFitMethod : std::uint8_t {
  // Admitted point mids fitted by `curve` (the historical default).
  PointCurve,
  // Channel objective: every valid contract's bid/ask volatility channel (both
  // sides, bidless upper bounds included), a spread-normalised mid anchor and a
  // flat default-shape prior, solved by `channel.family` (default: the knot
  // spline on the standardized grid, served as SplineVol). `curve.kind` and
  // `min_quotes` are not used by the fit; `min_quotes` only grades evidence.
  ChannelSpline
};

// Channel knot-spline controls (src/fitting/slice_channel.hpp has the
// objective). The mid anchor is in channel half-widths; the channel penalty is
// in the fixed volatility unit min_half_width_vol.
struct QuoteSliceChannelConfig {
  double channel_weight = 100.0;      // steep penalty outside [bid, ask]; > 0
  double mid_weight = 1.0;            // spread-normalised mid anchor
  double itm_mid_weight = 1.0;        // extra factor on in-the-money legs' mid term
  double default_shape_weight = 0.05; // flat default shape when no term prior
  double prior_scale = 0.10;          // relative volatility unit of the prior penalty
  double roughness = 0.02;            // second-difference penalty on knot vols
  double min_half_width_vol = 0.0025; // floor on a channel half-width
  double mult_floor = 0.05;
  double mult_ceil = 3.0;
  std::uint16_t max_iterations = 50;
  std::size_t min_channel_quotes = 2; // usable channels required to fit
  // Curve family that solves the channel objective. Unset: the native knot
  // spline (slice_channel). Otherwise any point family except LinearVariance,
  // fitted by bounded active-set reweighting around its own weighted fitter
  // (fit_slice_curve): mid anchor, currently violated bounds and the prior
  // become weighted point targets; at most `family_iterations` refits.
  std::optional<VolCurveKind> family{};
  std::uint16_t family_iterations = 12;
  // Bounded-move convexity repair, applied only when the strict price
  // certificate fails: each side's prices are replaced by their greatest convex
  // minorant if every moved node moves, FROM ITS FITTED PRICE, by at most this
  // many of its own quote half-spreads (bidless: half the ask; a side without a
  // quote uses the other side's). This bounds the move; it does not keep a
  // price inside [bid, ask]. The unchanged certificate then runs again; moved
  // nodes are flagged in QuoteSliceFit::moved_nodes. 0 disables the repair.
  double repair_half_spreads = 0.0;
  // Opt-in channel dominance under graded admission (fit_quote_surface): a
  // pass-A POINT fit whose served prices lie outside [bid, ask] on more than
  // this fraction of the expiry's strictly two-sided quotes is no pass-A
  // evidence; the expiry is refitted on this channel objective like any other
  // candidate, and the point fit is served (grade B, `point_fallback`) only when
  // no channel refit is. Per profile, so it can target wide markets. Unset: off.
  // In [0, 1].
  std::optional<double> point_max_outside_fraction{};
};

// InvalidArgument unless every channel control is finite and in range
// (channel_weight > 0, mult_ceil > mult_floor > 0, a supported family, ...).
// fit_quote_slice_channel and fit_quote_surface apply it before any fit.
[[nodiscard]] Status validate_quote_slice_channel(const QuoteSliceChannelConfig &config);

// Opt-in repair inside the bid/ask channel. Off is the historical behaviour.
enum class QuoteSliceRepairMode : std::uint8_t { Off, Channel };

// Channel repair, applied only after the strict price certificate has failed
// (and after the bounded-move convexity repair, when that is enabled, has not
// fixed it). First only the nodes more than the certificate's allowance above
// their side's lower convex envelope are lowered onto it (every other node
// keeps its fitted price); if that is not accepted, each side's node prices are
// projected (least squares, Dykstra) onto the set the certificate's strike
// checks describe: convex in strike, call slopes in [-discount_max, 0],
// put slopes in [0, discount_max], intrinsic and upper price bounds, and every
// node inside its own quote's [bid, ask] channel widened to contain its fitted
// price, so no node can end farther outside its channel than it started
// (bidless: [0, ask]; a side without a quote is held fixed). Nodes whose
// constraints never bind keep their fitted price exactly; a residual violation
// above the certificate allowance is lowered onto its envelope. A repair is
// accepted only when (a) the channel rule holds, (b) at most `max_nodes`
// strikes moved by more than `immaterial_price_move` (a floor at pricing
// accuracy: per-strike carry noise can leave many nodes a few 1e-5 above their
// envelope, and such moves are flagged and re-implied but do not count), and
// (c) every moved node-side WITHOUT its own quote (where no channel protects
// the price) moved by at most `max_vol_move` in American IV from the fitted
// curve. The IV change of every moved node-side is recorded either way. The
// unchanged certificate then runs again; nothing about certification is
// relaxed. Moved nodes are flagged in QuoteSliceFit::moved_nodes and re-implied
// as for the other repairs.
struct QuoteSliceRepairConfig {
  QuoteSliceRepairMode mode = QuoteSliceRepairMode::Off;
  std::size_t max_nodes = 16;            // >= 1
  double max_vol_move = 0.01;            // > 0, volatility units
  double immaterial_price_move = 5.0e-4; // >= 0, price units
};

// InvalidArgument unless the mode is known and every budget is finite and in
// range. fit_quote_slice and fit_quote_surface apply it before any fit.
[[nodiscard]] Status validate_quote_slice_repair(const QuoteSliceRepairConfig &config);

enum class QuoteSliceRepairOutcome : std::uint8_t {
  NotAttempted, // certificate passed, mode Off, or no repair needed
  Applied,      // accepted and certified
  NoMove,       // the certificate failed but the projection moved no node
  Infeasible,   // no convex price set inside the channels (or no settled projection)
  LeftChannel,  // a moved node-side would end farther outside its channel
  NodeBudget,   // more than max_nodes strikes would move materially
  VolBudget,    // an unquoted node-side's volatility would move more than max_vol_move
  NotCertified  // within budget, but the certificate (or the calendar clamp) still fails
};

[[nodiscard]] const char *to_string(QuoteSliceRepairOutcome value) noexcept;

// What one channel repair did (or would have done before it was refused).
struct QuoteSliceRepairRecord {
  QuoteSliceRepairOutcome outcome = QuoteSliceRepairOutcome::NotAttempted;
  std::size_t nodes = 0;          // strikes whose call or put price moved
  std::size_t material_nodes = 0; // of which moved by more than immaterial_price_move
  double max_price_move = 0.0;     // largest absolute node-price move
  double max_vol_move = 0.0;       // largest |IV change| over identifiable moved node-sides
  double max_unquoted_vol_move = 0.0; // the same over node-sides without a quote (budgeted)
  std::size_t unidentified = 0;    // moved node-sides whose new price has no American IV
};

struct QuoteSliceConfig {
  CurveConfig curve{.kind = VolCurveKind::Essvi};
  QuoteSliceQualityConfig quality{};
  QuoteSliceDividendModel dividend_model = QuoteSliceDividendModel::ContinuousYield;
  QuoteSliceSidePolicy side_policy = QuoteSliceSidePolicy::OutOfTheMoney;
  std::optional<AlOpts> al_opts{};
  std::size_t min_quotes = 6;
  double atm_log_band = 0.0;
  double spread_floor = 0.01;
  double min_time_value = 1.0e-5;
  double iv_tolerance = 1.0e-7;
  std::uint16_t max_iv_iterations = 64;
  int discrete_div_steps = kDiscreteDivDefaultSteps;
  // Opt-in carry resolution (quote_slice_carry.hpp). The default Supplied mode
  // is the historical path, unchanged.
  QuoteSliceCarryConfig carry{};
  QuoteSliceFitMethod method = QuoteSliceFitMethod::PointCurve;
  QuoteSliceChannelConfig channel{};
  // Opt-in channel repair of a failed certificate (default Off).
  QuoteSliceRepairConfig repair{};
};

enum class QuoteSliceQuoteStatus : std::uint8_t {
  Admitted,
  InvalidContract,
  InvalidQuote,
  ExcludedSide,
  UnidentifiableVolatility,
  ImpliedVolatilityFailure,
  InvalidObservation,
  BidlessQuote,
  BelowMinimumBid,
  WideSpreadToMid,
  WideSpreadVol,
  LowVegaWeight
};
enum class QuoteSliceEvaluationStatus : std::uint8_t {
  Success,
  InvalidContract,
  NoFit,
  InvalidVolatility,
  PricingFailure
};
enum class QuoteSliceFitStatus : std::uint8_t {
  Success, InsufficientQuotes, FitFailure, ArbitrageFailure
};

[[nodiscard]] const char *to_string(QuoteSliceDividendModel value) noexcept;
[[nodiscard]] const char *to_string(QuoteSliceQuoteStatus value) noexcept;
[[nodiscard]] const char *to_string(QuoteSliceEvaluationStatus value) noexcept;
[[nodiscard]] const char *to_string(QuoteSliceFitStatus value) noexcept;

struct QuoteSliceRow {
  QuoteSliceQuoteStatus quote_status = QuoteSliceQuoteStatus::InvalidQuote;
  QuoteSliceEvaluationStatus evaluation_status = QuoteSliceEvaluationStatus::NoFit;
  bool admitted = false;
  double market_iv = std::numeric_limits<double>::quiet_NaN();
  double fitted_iv = std::numeric_limits<double>::quiet_NaN();
  double fitted_price = std::numeric_limits<double>::quiet_NaN();
  // Full floored spread in local volatility units, once IV and vega exist.
  double spread_vol = std::numeric_limits<double>::quiet_NaN();
  double weight_w = 0.0; // stored fit weight; zero when not admitted
  bool weight_capped = false;
};

// Populated only by the channel method.
struct QuoteSliceChannelDiagnostics {
  bool used = false;
  std::size_t channel_quotes = 0; // usable bid/ask volatility channels
  std::size_t mid_quotes = 0;     // of which carry a mid volatility
  std::size_t mid_strikes = 0;    // distinct strikes among the mid quotes
  std::size_t violations = 0;     // fitted volatility outside its channel
  double max_violation_half_widths = 0.0;
  double prior_strength = 0.0;
  bool term_prior = false;        // false: flat default shape
  bool converged = false;
  std::size_t iterations = 0;     // Newton iterations (knot spline) or family refits
  double solve_seconds = 0.0;     // wall time of the channel solve alone (diagnostic)
  // Convexity repair: required shift measured whenever the certificate failed,
  // nodes moved only when the repair was within tolerance and applied.
  std::size_t repaired_nodes = 0;
  double repair_max_price = 0.0;
  double repair_max_half_spreads = 0.0;
};

struct QuoteSliceDiagnostics {
  std::size_t quotes_seen = 0;
  std::size_t iv_attempted = 0;
  std::size_t quotes_admitted = 0;
  std::size_t distinct_strikes = 0;
  std::size_t prices_produced = 0;
  std::size_t weights_capped = 0;
  double forward = 0.0;
  double discount = 0.0;
  double effective_rate = 0.0;
  double effective_yield = 0.0;
  FitDiag curve_fit{};
  QuoteSliceChannelDiagnostics channel{};
  // Channel repair of this slice's own certificate (QuoteSliceConfig::repair).
  QuoteSliceRepairRecord repair{};
};

struct QuoteSliceCarryPoint {
  double strike = 0.0;
  double continuous_yield = 0.0; // calendar annualized, excluding cash dividends
};

// An immutable serving snapshot when published by the caller. The curve owns
// its fitted state, and the schedule is copied, so query lifetime is independent
// of every input span and CurveConfig (including a caller's spline-knot span).
struct QuoteSliceFit {
  QuoteSliceFitStatus fit_status = QuoteSliceFitStatus::InsufficientQuotes;
  std::string fit_error;
  QuoteSliceDiagnostics diagnostics;
  std::unique_ptr<IVolCurve> curve;
  // The accepted serving representation. Its owned piecewise-linear American
  // prices enforce strike shape, exercise bounds and conservative cross-side
  // bounds throughout its finite domain. The intermediate IV curve is NOT a
  // certified serving representation. Absence means the fit cannot be served.
  std::optional<AmericanPriceSlice> prices;
  // Empty unless a repair moved served prices off the curve. Otherwise one entry
  // per prices->nodes() entry: value 1 = call moved, 2 = put moved (OR'd). At
  // such a node the curve's volatility is not the served price's.
  std::vector<std::uint8_t> moved_nodes;
  std::vector<QuoteSliceRow> rows; // same order and length as input quotes
  QuoteSliceMarket market;
  QuoteSliceDividendModel dividend_model = QuoteSliceDividendModel::ContinuousYield;
  std::optional<AlOpts> al_opts;
  int discrete_div_steps = kDiscreteDivDefaultSteps;
  std::vector<CashDividend> variance_clock_dividends;
  // Empty when no quote supplies a yield override. Otherwise owns one
  // resolved carry per valid contract strike, including rejected bid/ask rows.
  // Strictly increasing strikes; interpolation is linear in log strike, with
  // flat edges. Conflicting same-strike carries are refused during fitting.
  std::vector<QuoteSliceCarryPoint> carry_curve;
  // How the expiry carry was resolved (quote_slice_carry.hpp). Under an applied
  // implied forward, `market` holds the implied yield the fit actually used.
  QuoteSliceCarryResolution carry_resolution{};
};

struct QuoteSliceValue {
  double volatility = 0.0;
  double price = 0.0;
};

// Fits raw American bid/ask quotes, then reprices every valid contract with the
// fitted smile, including quotes excluded from fitting. No reference fair
// values or reference volatilities are accepted by this API.
// Per-contract yields are used for IV extraction and repricing. The fitted
// volatility curve retains the COMMON market-reference forward coordinate;
// European observation prices/spreads are transformed to that coordinate.
// Weights use local-forward Black vega as a quote-sensitivity proxy. Quality
// gates act before curve fitting, using QuoteSliceConfig::quality for ALL curve
// kinds; CurveConfig::parametric observation-builder gates do not run here.
//
// DatedCash dividends use CALENDAR tau measured from the same valuation instant
// as calendar_years. The fitter assumes a constant variance/calendar clock-speed
// ratio at every intermediate exercise time. Two maturity totals cannot encode
// session-dependent clocks; those require an event/time-dependent model.
// Other models require an empty event span;
// ContinuousYield also requires dividend_pv == 0.
//
// Shared malformed inputs return Err. Quote-level failures are retained in rows;
// insufficient data or a curve failure returns Ok with a non-Success fit_status.
// Non-finite yield overrides and conflicting resolved carries at the same
// valid contract strike are shared InvalidArgument errors, never averaged.
// The cold fitting path allocates O(quotes + dividends) scratch. Serving reuses
// the immutable curve; the discrete-cash engine retains its own lattice cost.
[[nodiscard]] Result<QuoteSliceFit>
fit_quote_slice(const QuoteSliceMarket &market, std::span<const QuoteSliceQuote> quotes,
                const QuoteSliceConfig &config = {},
                std::span<const CashDividend> calendar_dividends = {});

// Query inside the certified price domain. At a grid node the fitted IV is
// cached through the immutable curve, except at a node a repair moved off the
// curve (moved_nodes), which is inverted like a point between nodes. Between
// nodes, invert the certified piecewise-linear price using the snapshot's local
// carry and American model; this keeps returned price and IV consistent.
// Outside-domain requests fail.
// For allocation-free PRICE-ONLY serving, call fit.prices->price(K, side).
// A slice enforces strike constraints; publish several expiries together only
// after check_american_calendar has accepted their overlapping price domains.
[[nodiscard]] Result<QuoteSliceValue> evaluate_quote_slice(const QuoteSliceFit &fit, double strike,
                                                           Side side);

// A smile prior for one expiry: an ATM volatility and a shape as multiples
// sigma/atm_vol on an increasing standardized grid z = ln(K/F)/(atm_vol*sqrt(T))
// (variance clock), with the effective strength already applied. Spans are
// borrowed for the duration of the call only.
struct QuoteSlicePrior {
  double atm_vol = 0.0;
  std::span<const double> z;
  std::span<const double> multiples;
  double strength = 0.0;
};

// Channel knot-spline fit of one expiry (QuoteSliceFitMethod::ChannelSpline),
// optionally regularized toward `prior` instead of the flat default shape.
// Every valid contract contributes its bid/ask channel, inverted with the SAME
// American model and per-contract carry as the point path; quote statuses and
// admission provenance are reported exactly as by the point path. Final prices
// pass the same mandatory AmericanPriceSlice certification. Fewer than
// `channel.min_channel_quotes` usable channels returns InsufficientQuotes; a
// numerical failure of the spline (or of `channel.family`'s first refit)
// returns FitFailure. Requires supplied carry (config.carry.mode == Supplied);
// fit_quote_slice resolves carry before its prior-free channel fit.
[[nodiscard]] Result<QuoteSliceFit>
fit_quote_slice_channel(const QuoteSliceMarket &market, std::span<const QuoteSliceQuote> quotes,
                        const QuoteSliceConfig &config, const QuoteSlicePrior *prior,
                        std::span<const CashDividend> calendar_dividends = {});

// Bounded-move calendar repair. Moves the certified node prices of `fit` into
// [max over `earlier` slices, min over `later` slices] wherever a node lies
// inside their domains, restores convexity with the greatest convex minorant,
// and accepts only when every node moved by at most `half_spreads` of its own
// quote half-spread (bidless: half the ask) FROM ITS PREVIOUS PRICE, the clamp
// remains satisfied and the unchanged AmericanPriceSlice certificate passes.
// The bound limits the move; it does not keep a price inside [bid, ask]. On
// success replaces fit.prices, flags moved nodes in fit.moved_nodes, refreshes
// row prices (moved nodes report the volatility of the served price, or
// InvalidVolatility with NaN price and volatility when that price has no
// identifiable volatility) and returns true; otherwise leaves `fit` unchanged. The largest required move, in
// half-spreads, is written to `required` when non-null. The caller must still
// check the result against every accepted slice.
[[nodiscard]] bool repair_quote_slice_calendar(QuoteSliceFit &fit,
                                               std::span<const QuoteSliceQuote> quotes,
                                               std::span<const AmericanPriceSlice *const> earlier,
                                               std::span<const AmericanPriceSlice *const> later,
                                               double half_spreads, double *required = nullptr);

// Calendar repair inside the bid/ask channel: the certified node prices of
// `fit` are projected exactly as by the channel repair (QuoteSliceRepairConfig),
// with every node's box further intersected with [max over `earlier`, min over
// `later`] wherever it lies inside their domains, and acceptance follows the
// same channel, node and volatility budgets (`budget.mode` is ignored) instead
// of a half-spread bound. On success replaces fit.prices, ORs the moved nodes
// into fit.moved_nodes, refreshes row prices and volatilities exactly as
// repair_quote_slice_calendar does and returns true; otherwise leaves `fit`
// unchanged. `record` (when non-null) receives what the repair did or why it
// was refused. The caller must still check the result against every accepted
// slice.
[[nodiscard]] bool repair_quote_slice_calendar_in_channel(
    QuoteSliceFit &fit, std::span<const QuoteSliceQuote> quotes,
    std::span<const AmericanPriceSlice *const> earlier,
    std::span<const AmericanPriceSlice *const> later, const QuoteSliceRepairConfig &budget,
    QuoteSliceRepairRecord *record = nullptr);

// ── Graded admission over one valuation surface ──────────────────────────────

// Confidence grade carried by every served expiry.
//  A: the expiry's own admitted point quotes satisfy the profile minimum
//     (min_quotes admitted quotes and distinct strikes) and pass A served it with
//     the profile's own method (fit_quote_slice: the point fitter, or for a
//     ChannelSpline profile the channel fit with only the flat default shape),
//     with no term prior and no served price moved by a repair.
//  B: a channel refit (under the leave-one-out term prior when another expiry
//     supplies a node, else the flat default shape) with at least
//     `grade_b_min_mid_strikes` distinct mid strikes, or a pass-A slice whose
//     served prices a convexity or calendar repair moved. Opt-in rungs are B as
//     well: a pass-A fit served only after the channel repair (repair rung), and
//     a pass-A point fit that failed the channel-dominance gate but is served
//     because no channel refit was (point fallback). A repaired slice is never A.
//  C: a channel refit (same prior rule) with fewer mid strikes (dominated by
//     bid/ask bounds and the prior or default shape).
enum class QuoteSliceGrade : std::uint8_t { A, B, C, Refused };

[[nodiscard]] const char *to_string(QuoteSliceGrade value) noexcept;

struct QuoteSurfaceExpiry {
  QuoteSliceMarket market;
  std::span<const QuoteSliceQuote> quotes;
  std::span<const CashDividend> calendar_dividends{};
};

struct QuoteSurfaceConfig {
  double term_prior_weight = 1.0;     // scale of the neighbour prior's strength
  double prior_decay_log_time = 1.0;  // confidence e-folding in |ln(T/T_nearest)|
  double prior_quote_halflife = 6.0;  // strength * n0 / (n0 + own two-sided strikes)
  double channel_node_weight = 0.5;   // prior-node weight of a channel fit vs a point fit
  double calendar_retry_factor = 4.0; // prior strength multiplier on a calendar retry
  std::size_t grade_b_min_mid_strikes = 4;
  // Bounded-move calendar repair for a slice that crosses accepted ones
  // (repair_quote_slice_calendar); 0 disables it. A repaired pass-A slice is
  // graded B because its served prices are no longer its fit's.
  double calendar_repair_half_spreads = 0.0;
};

struct QuoteSurfaceSlice {
  // For a refused expiry the fit keeps its diagnostics and row provenance but
  // never carries certified prices or fitted values.
  QuoteSliceFit fit;
  QuoteSliceGrade grade = QuoteSliceGrade::Refused;
  // Empty when served. Otherwise one of: invalid_input, insufficient_quotes,
  // fit_failure, arbitrage_failure, calendar_arbitrage.
  std::string refusal;
  std::string error;                  // detail for invalid_input / fit failures
  QuoteSliceFitStatus point_status = QuoteSliceFitStatus::InsufficientQuotes;
  bool channel_refit = false;         // served or last attempted by the channel fitter
  bool calendar_retry = false;        // a calendar retry was attempted
  bool calendar_repaired = false;     // served after a bounded-move calendar repair
  // For a calendar refusal: the largest price crossing against the accepted
  // slices (earlier minus later, price units) and its strike.
  double calendar_gap = 0.0;
  double calendar_gap_strike = 0.0;
  // Largest node move (in half-spreads) the calendar repair required, over its
  // attempts; infinite when a moved node had no quote. Zero when never tried.
  double calendar_repair_required = 0.0;
  // Opt-in provenance; every field below keeps its default when the options are
  // off, except point_error (the pass-A fit_error, a diagnostic).
  std::string point_error;
  // Repair rung: served by the pass-A fit refitted with the channel repair, and
  // that refit's repair record (also when it was refused). calendar_repaired is
  // also set after a channel calendar repair.
  bool repair_rung = false;
  QuoteSliceRepairRecord rung_repair{};
  // Served after a channel calendar repair; the last such attempt's record.
  bool calendar_channel_repaired = false;
  QuoteSliceRepairRecord calendar_channel_repair{};
  // Dominance gate (QuoteSliceChannelConfig::point_max_outside_fraction): the
  // pass-A point fit's share of two-sided quotes outside [bid, ask], whether it
  // failed the gate, and whether it was served anyway because no channel refit
  // was.
  double point_outside_fraction = 0.0;
  bool point_gated = false;
  bool point_fallback = false;
};

// Graded admission for every expiry of ONE underlier/deliverable at ONE valuation
// instant (the caller owns identity). Pass A runs the profile's own fit,
// unchanged (fit_quote_slice: the point fitter, or the term-prior-free channel
// fit for a ChannelSpline profile); expiries it cannot serve with enough own point
// evidence are refitted on the channel objective under a leave-one-out term
// prior built from the other fitted expiries (two stages, so snapshots without
// any pass-A fit still borrow shape from their neighbours). Calendar
// consistency is enforced on the final American prices: slices are accepted
// greedily in confidence order; a slice that crosses an accepted one is first
// given a bounded-move repair (when calendar_repair_half_spreads > 0), then
// refitted once with a stronger prior (and that refit repaired), and otherwise
// refused. No certification is relaxed. With config.repair.mode == Channel the
// channel repair runs only where a certificate or calendar check has already
// failed, so every fit that certifies without it is unchanged: pass A runs
// without it; channel refits (B1, B2 and the calendar retry) run with it after
// their bounded-move repair; an expiry still refused for a failed certificate
// whose pass-A fit failed its certificate too gets that pass-A fit again with
// the repair (grade B, `repair_rung`); and a crossing that neither the
// bounded-move repair nor the retry fixes is offered
// repair_quote_slice_calendar_in_channel on the expiry's own prices, then on
// the retry's. A newly served expiry still feeds the B2 prior and the greedy
// calendar order, so it can change other expiries' channel refits and displace
// a later candidate (the benchmark reports this on the intersection rows).
// Distinct calendar maturities are
// required; at most kQuoteSurfaceMaxExpiries expiries; carry must be supplied
// (resolved carry is not yet composed with the term prior). Shared malformed
// configuration (including config.channel and config.repair) is Err;
// per-expiry input errors are
// refusals with refusal == "invalid_input".
inline constexpr std::size_t kQuoteSurfaceMaxExpiries = 128;

[[nodiscard]] Result<std::vector<QuoteSurfaceSlice>>
fit_quote_surface(std::span<const QuoteSurfaceExpiry> expiries, const QuoteSliceConfig &config,
                  const QuoteSurfaceConfig &surface = {});

} // namespace atx::vol
