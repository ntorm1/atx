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
// cached through the immutable curve. Between nodes, invert the certified
// piecewise-linear price using the snapshot's local carry and American model;
// this keeps returned price and IV consistent. Outside-domain requests fail.
// For allocation-free PRICE-ONLY serving, call fit.prices->price(K, side).
// A slice enforces strike constraints; publish several expiries together only
// after check_american_calendar has accepted their overlapping price domains.
[[nodiscard]] Result<QuoteSliceValue> evaluate_quote_slice(const QuoteSliceFit &fit, double strike,
                                                           Side side);

} // namespace atx::vol
