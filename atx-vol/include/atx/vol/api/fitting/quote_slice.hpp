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

struct QuoteSliceConfig {
  CurveConfig curve{.kind = VolCurveKind::Essvi};
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
  InvalidObservation
};
enum class QuoteSliceEvaluationStatus : std::uint8_t {
  Success,
  InvalidContract,
  NoFit,
  InvalidVolatility,
  PricingFailure
};
enum class QuoteSliceFitStatus : std::uint8_t { Success, InsufficientQuotes, FitFailure };

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
};

struct QuoteSliceDiagnostics {
  std::size_t quotes_seen = 0;
  std::size_t iv_attempted = 0;
  std::size_t quotes_admitted = 0;
  std::size_t distinct_strikes = 0;
  std::size_t prices_produced = 0;
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
// Weights use local-forward Black vega as a quote-sensitivity proxy.
//
// DatedCash dividends use CALENDAR tau measured from the same valuation instant
// as calendar_years. The fitter maps tau to the variance clock at a constant
// clock-speed ratio; callers needing intraday/session-varying clocks should map
// those clocks before calling. Other models require an empty event span;
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

// Query an arbitrary new strike using exactly the fitted snapshot's clocks,
// interpolated strike carry and dividend model. Exact carry nodes reproduce
// their supplied yields. Errors never produce fabricated zero values.
[[nodiscard]] Result<QuoteSliceValue> evaluate_quote_slice(const QuoteSliceFit &fit, double strike,
                                                           Side side);

} // namespace atx::vol
