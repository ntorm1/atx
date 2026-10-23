#include "atx/vol/api/fitting/quote_slice.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <limits>
#include <span>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "atx/vol/api/pricing/american.hpp"

namespace atx::vol {
namespace {

// A single-clock American chain priced by Andersen-Lake at the TRUE carry
// `true_yield`, with a mild skew and proportional spreads.
std::vector<QuoteSliceQuote> american_quotes(double spot, double years, double rate,
                                             double true_yield, double forward) {
  std::vector<QuoteSliceQuote> quotes;
  for (double k = 60.0; k <= 160.0 + 1.0e-9; k += 5.0) {
    const double sigma = 0.30 - 0.08 * std::log(k / forward);
    for (Side side : {Side::Call, Side::Put}) {
      const auto price = andersen_lake(spot, k, years, sigma, rate, true_yield, side);
      if (!price) {
        ADD_FAILURE() << price.error().to_string();
        return {};
      }
      const double half = std::max(0.005, 0.01 * *price);
      quotes.push_back({k, side, std::max(*price - half, 1.0e-3), *price + half});
    }
  }
  return quotes;
}

QuoteSliceConfig implied_config() {
  QuoteSliceConfig cfg;
  cfg.carry.mode = QuoteSliceCarryMode::ImpliedForward;
  cfg.carry.implied.al_opts = std::nullopt; // match the generator exactly
  return cfg;
}

void expect_same_fit(const QuoteSliceFit &a, const QuoteSliceFit &b) {
  ASSERT_EQ(a.fit_status, b.fit_status);
  ASSERT_EQ(a.rows.size(), b.rows.size());
  EXPECT_EQ(std::memcmp(&a.diagnostics.forward, &b.diagnostics.forward, sizeof(double)), 0);
  EXPECT_EQ(a.diagnostics.quotes_admitted, b.diagnostics.quotes_admitted);
  EXPECT_EQ(a.carry_curve.size(), b.carry_curve.size());
  for (std::size_t i = 0; i < a.rows.size(); ++i) {
    EXPECT_EQ(a.rows[i].quote_status, b.rows[i].quote_status) << i;
    EXPECT_EQ(a.rows[i].evaluation_status, b.rows[i].evaluation_status) << i;
    const double pa = a.rows[i].fitted_price;
    const double pb = b.rows[i].fitted_price;
    EXPECT_TRUE((std::isnan(pa) && std::isnan(pb)) || std::memcmp(&pa, &pb, sizeof(double)) == 0)
        << i;
  }
}

TEST(QuoteSliceCarry, DefaultModeIsSuppliedAndRecordsNoResolution) {
  const QuoteSliceConfig cfg;
  EXPECT_EQ(cfg.carry.mode, QuoteSliceCarryMode::Supplied);
  const double forward = 100.0 * std::exp(0.03);
  const QuoteSliceMarket market{100.0, 1.0, 1.0, 0.04, 0.01, 0.0};
  const auto quotes = american_quotes(100.0, 1.0, 0.04, 0.01, forward);
  const auto fit = fit_quote_slice(market, quotes, cfg);
  ASSERT_TRUE(fit) << fit.error().to_string();
  EXPECT_EQ(fit->carry_resolution.mode, QuoteSliceCarryMode::Supplied);
  EXPECT_FALSE(fit->carry_resolution.applied);
  EXPECT_STREQ(to_string(QuoteSliceCarryMode::ImpliedForward), "implied_forward");
}

TEST(QuoteSliceCarry, ImpliedForwardReplacesAWrongSuppliedCarry) {
  const double spot = 100.0;
  const double years = 1.5;
  const double rate = 0.045;
  const double true_yield = 0.012;
  const double forward = spot * std::exp((rate - true_yield) * years);
  // Supplied carry is wrong by 150 bp; per-contract overrides repeat it.
  const QuoteSliceMarket market{spot, years, years, rate, 0.027, 0.0};
  auto quotes = american_quotes(spot, years, rate, true_yield, forward);
  for (auto &quote : quotes) {
    quote.continuous_yield = 0.027;
  }
  const auto fit = fit_quote_slice(market, quotes, implied_config());
  ASSERT_TRUE(fit) << fit.error().to_string();
  const auto &res = fit->carry_resolution;
  ASSERT_TRUE(res.applied) << res.reason;
  EXPECT_EQ(res.fallback, QuoteSliceCarryFallback::None);
  EXPECT_EQ(res.status, ImpliedForwardStatus::Ok);
  EXPECT_NEAR(res.implied_forward / forward - 1.0, 0.0, 5.0e-6);
  EXPECT_NEAR(res.implied_yield, true_yield, 5.0e-6);
  EXPECT_NEAR(res.supplied_forward, spot * std::exp((rate - 0.027) * years), 1.0e-9);
  EXPECT_NEAR(fit->market.continuous_yield, true_yield, 5.0e-6);
  EXPECT_NEAR(fit->diagnostics.forward / forward - 1.0, 0.0, 5.0e-6);
  // One carry per expiry: supplied per-contract yields are dropped.
  EXPECT_TRUE(fit->carry_curve.empty());
  ASSERT_EQ(fit->fit_status, QuoteSliceFitStatus::Success) << fit->fit_error;

  // The implied carry reprices the market better than the wrong supplied one.
  const auto supplied = fit_quote_slice(market, quotes);
  ASSERT_TRUE(supplied);
  ASSERT_EQ(supplied->fit_status, QuoteSliceFitStatus::Success) << supplied->fit_error;
  double err_implied = 0.0;
  double err_supplied = 0.0;
  for (std::size_t i = 0; i < quotes.size(); ++i) {
    const double mid = 0.5 * (quotes[i].bid + quotes[i].ask);
    if (fit->rows[i].evaluation_status == QuoteSliceEvaluationStatus::Success &&
        supplied->rows[i].evaluation_status == QuoteSliceEvaluationStatus::Success) {
      err_implied += std::fabs(fit->rows[i].fitted_price - mid);
      err_supplied += std::fabs(supplied->rows[i].fitted_price - mid);
    }
  }
  EXPECT_LT(err_implied, 0.5 * err_supplied);
}

TEST(QuoteSliceCarry, FundingPresentValueKeepsCashAndMatchesTheImpliedForward) {
  const double spot = 100.0;
  const double years = 1.0;
  const double rate = 0.04;
  const double pv = 2.0;
  // Market truth: funding-PV forward with zero residual yield.
  const double forward = spot * std::exp(rate * years) - pv * std::exp(rate * years);
  const double q_eff = rate - std::log(forward / spot) / years;
  std::vector<QuoteSliceQuote> quotes;
  for (double k = 60.0; k <= 150.0 + 1.0e-9; k += 5.0) {
    const double sigma = 0.28 - 0.06 * std::log(k / forward);
    for (Side side : {Side::Call, Side::Put}) {
      const auto price = andersen_lake(spot, k, years, sigma, rate, q_eff, side);
      ASSERT_TRUE(price);
      const double half = std::max(0.005, 0.01 * *price);
      quotes.push_back({k, side, std::max(*price - half, 1.0e-3), *price + half});
    }
  }
  const QuoteSliceMarket market{spot, years, years, rate, 0.02, pv};
  auto cfg = implied_config();
  cfg.dividend_model = QuoteSliceDividendModel::AggregateFundingPresentValue;
  const auto fit = fit_quote_slice(market, quotes, cfg);
  ASSERT_TRUE(fit) << fit.error().to_string();
  ASSERT_TRUE(fit->carry_resolution.applied) << fit->carry_resolution.reason;
  EXPECT_DOUBLE_EQ(fit->market.dividend_pv, pv);
  EXPECT_NEAR(fit->market.continuous_yield, 0.0, 5.0e-6);
  EXPECT_NEAR(fit->diagnostics.forward / forward - 1.0, 0.0, 5.0e-6);
  EXPECT_NEAR(fit->diagnostics.effective_yield, q_eff, 5.0e-6);
}

TEST(QuoteSliceCarry, RefusalFallsBackBitIdenticallyToSuppliedCarry) {
  const double forward = 100.0 * std::exp(0.03);
  const QuoteSliceMarket market{100.0, 1.0, 1.0, 0.04, 0.01, 0.0};
  auto quotes = american_quotes(100.0, 1.0, 0.04, 0.01, forward);
  for (auto &quote : quotes) {
    quote.continuous_yield = 0.01;
  }
  auto cfg = implied_config();
  cfg.carry.implied.min_pairs = 32; // more than the chain can supply near the money
  cfg.carry.implied.max_pairs = 32;
  const auto refused = fit_quote_slice(market, quotes, cfg);
  const auto supplied = fit_quote_slice(market, quotes);
  ASSERT_TRUE(refused && supplied);
  EXPECT_FALSE(refused->carry_resolution.applied);
  EXPECT_EQ(refused->carry_resolution.fallback, QuoteSliceCarryFallback::EstimateRefused);
  EXPECT_EQ(refused->carry_resolution.status, ImpliedForwardStatus::InsufficientPairs);
  EXPECT_EQ(refused->carry_resolution.mode, QuoteSliceCarryMode::ImpliedForward);
  EXPECT_DOUBLE_EQ(refused->market.continuous_yield, 0.01);
  expect_same_fit(*refused, *supplied);
}

TEST(QuoteSliceCarry, DatedCashModelIsNotImpliedAndFallsBack) {
  const double forward = 100.0 * std::exp(0.03);
  const QuoteSliceMarket market{100.0, 1.0, 1.0, 0.04, 0.01, 0.0};
  const auto quotes = american_quotes(100.0, 1.0, 0.04, 0.01, forward);
  auto cfg = implied_config();
  cfg.dividend_model = QuoteSliceDividendModel::DatedCash;
  cfg.discrete_div_steps = 51;
  const auto fit = fit_quote_slice(market, quotes, cfg);
  ASSERT_TRUE(fit) << fit.error().to_string();
  EXPECT_FALSE(fit->carry_resolution.applied);
  EXPECT_EQ(fit->carry_resolution.fallback, QuoteSliceCarryFallback::DatedCash);
  EXPECT_STREQ(to_string(fit->carry_resolution.fallback), "dated_cash");
  EXPECT_DOUBLE_EQ(fit->market.continuous_yield, 0.01);
  auto supplied_cfg = cfg;
  supplied_cfg.carry.mode = QuoteSliceCarryMode::Supplied;
  const auto supplied = fit_quote_slice(market, quotes, supplied_cfg);
  ASSERT_TRUE(supplied);
  expect_same_fit(*fit, *supplied);
}

TEST(QuoteSliceCarry, MalformedMarketStillReportsTheSuppliedError) {
  const QuoteSliceMarket market{-1.0, 1.0, 1.0, 0.04, 0.01, 0.0};
  const std::vector<QuoteSliceQuote> quotes{{100.0, Side::Call, 1.0, 1.1}};
  const auto implied = fit_quote_slice(market, quotes, implied_config());
  const auto supplied = fit_quote_slice(market, quotes);
  ASSERT_FALSE(implied);
  ASSERT_FALSE(supplied);
  EXPECT_EQ(implied.error().code(), supplied.error().code());
}

TEST(QuoteSliceCarry, MalformedQuoteIsRejectedBeforeCarryResolution) {
  // The implied-carry path replaces every contract yield, so a non-finite
  // supplied yield must be refused before carry resolution, exactly as the
  // supplied path refuses it.
  const double forward = 100.0 * std::exp(0.03);
  const QuoteSliceMarket market{100.0, 1.0, 1.0, 0.04, 0.01, 0.0};
  auto quotes = american_quotes(100.0, 1.0, 0.04, 0.01, forward);
  quotes[3].continuous_yield = std::numeric_limits<double>::quiet_NaN();
  const auto implied = fit_quote_slice(market, quotes, implied_config());
  const auto supplied = fit_quote_slice(market, quotes);
  ASSERT_FALSE(supplied);
  ASSERT_FALSE(implied);
  EXPECT_EQ(implied.error().code(), ErrorCode::InvalidArgument);
  EXPECT_EQ(implied.error().code(), supplied.error().code());
  // A malformed shared configuration is refused the same way.
  auto config = implied_config();
  config.min_quotes = 1;
  quotes[3].continuous_yield = 0.01;
  const auto bad_config = fit_quote_slice(market, quotes, config);
  ASSERT_FALSE(bad_config);
  EXPECT_EQ(bad_config.error().code(), ErrorCode::InvalidArgument);
}

// ── Post-estimate fallback causes (cleanup review 3465221c, MINOR) ───────────
//
// NoModelYield and SliceRefused arrive with estimator status Ok, so only the
// typed fallback cause explains them (quote_slice_carry.hpp). Each test drives
// the public fit_quote_slice to that cause and checks the documented outcome:
// the supplied carry is kept and the fit is bit-identical to a Supplied fit.

// NoModelYield: the calendar clock is the smallest subnormal year fraction, so
// the implied forward's calendar yield r - ln(F / S) / T_cal overflows to +inf.
// validate() admits the clock (positive, finite) and the estimator runs on the
// variance clock, where the funding rate rescales to rate * T_cal / T_var == 0,
// so the chain is priced at r = 0 on that clock and the estimate is Ok.
TEST(QuoteSliceCarry, ForwardWithoutAFiniteModelYieldFallsBackAsNoModelYield) {
  const double calendar = std::numeric_limits<double>::denorm_min();
  const double true_yield = 0.02;
  const double forward = 100.0 * std::exp(-true_yield);
  const auto quotes = american_quotes(100.0, 1.0, 0.0, true_yield, forward);
  const QuoteSliceMarket market{100.0, 1.0, calendar, 0.04, 0.01, 0.0};
  const auto fit = fit_quote_slice(market, quotes, implied_config());
  const auto supplied = fit_quote_slice(market, quotes);
  ASSERT_TRUE(fit) << fit.error().to_string();
  ASSERT_TRUE(supplied) << supplied.error().to_string();
  const auto &res = fit->carry_resolution;
  EXPECT_FALSE(res.applied);
  EXPECT_EQ(res.mode, QuoteSliceCarryMode::ImpliedForward);
  EXPECT_EQ(res.fallback, QuoteSliceCarryFallback::NoModelYield);
  EXPECT_STREQ(to_string(res.fallback), "no_model_yield");
  EXPECT_STREQ(res.reason, "implied forward has no yield under the dividend model");
  // The estimate itself succeeded and is recorded beside the cause.
  EXPECT_EQ(res.status, ImpliedForwardStatus::Ok);
  EXPECT_NEAR(res.implied_forward / forward - 1.0, 0.0, 5.0e-6);
  EXPECT_FALSE(std::isfinite(res.implied_yield));
  EXPECT_DOUBLE_EQ(fit->market.continuous_yield, 0.01);
  expect_same_fit(*fit, *supplied);
}

// Unit-spot American chain (strikes 0.60 .. 1.60) priced by Andersen-Lake at
// `true_yield`; spreads are proportional with a 5e-5 floor.
std::vector<QuoteSliceQuote> unit_spot_quotes(double years, double rate, double true_yield) {
  const double forward = std::exp((rate - true_yield) * years);
  std::vector<QuoteSliceQuote> quotes;
  for (int i = 0; i <= 20; ++i) {
    const double k = 0.60 + 0.05 * i;
    const double sigma = 0.30 - 0.08 * std::log(k / forward);
    for (Side side : {Side::Call, Side::Put}) {
      const auto price = andersen_lake(1.0, k, years, sigma, rate, true_yield, side);
      if (!price) {
        ADD_FAILURE() << price.error().to_string();
        return {};
      }
      const double half = std::max(5.0e-5, 0.01 * *price);
      quotes.push_back({k, side, std::max(*price - half, 1.0e-5), *price + half});
    }
  }
  return quotes;
}

// SliceRefused: AggregateFundingPresentValue keeps the supplied PV and restates
// F as q = -ln((F e^{-r T} + PV) / S) / T. With PV = 1e16 (ulp 2) and
// F e^{-r T} < 1, the sum rounds to PV, and at unit spot and T == 1 the slice's
// retained-cash fraction PV e^{q T} / S is then EXACTLY 1 (ln S == 0, and q T ==
// -ln PV cancels ln PV), so prepare_fit refuses the derived carry. The supplied
// q = -37.5 leaves a fraction of about 0.52, which the slice represents, so the
// fit falls back to it.
TEST(QuoteSliceCarry, CarryTheSliceCannotRepresentFallsBackAsSliceRefused) {
  const double rate = 0.04;
  const double pv = 1.0e16;
  const auto quotes = unit_spot_quotes(1.0, rate, rate); // F == 1
  ASSERT_FALSE(quotes.empty());
  const QuoteSliceMarket market{1.0, 1.0, 1.0, rate, -37.5, pv};
  auto cfg = implied_config();
  cfg.dividend_model = QuoteSliceDividendModel::AggregateFundingPresentValue;
  cfg.carry.implied.spread_floor = 1.0e-4; // price units, scaled to the unit spot
  auto supplied_cfg = cfg;
  supplied_cfg.carry.mode = QuoteSliceCarryMode::Supplied;
  const auto fit = fit_quote_slice(market, quotes, cfg);
  const auto supplied = fit_quote_slice(market, quotes, supplied_cfg);
  ASSERT_TRUE(fit) << fit.error().to_string();
  ASSERT_TRUE(supplied) << supplied.error().to_string();
  const auto &res = fit->carry_resolution;
  EXPECT_FALSE(res.applied);
  EXPECT_EQ(res.fallback, QuoteSliceCarryFallback::SliceRefused);
  EXPECT_STREQ(to_string(res.fallback), "slice_refused");
  EXPECT_STREQ(res.reason, "slice refused the implied carry");
  EXPECT_EQ(res.status, ImpliedForwardStatus::Ok);
  EXPECT_NEAR(res.implied_forward - 1.0, 0.0, 1.0e-4);
  EXPECT_EQ(res.implied_yield, -std::log(pv)); // finite: not NoModelYield
  EXPECT_DOUBLE_EQ(fit->market.continuous_yield, -37.5);
  expect_same_fit(*fit, *supplied);
  // Premise: the slice refuses exactly that carry on its own.
  QuoteSliceMarket implied = market;
  implied.continuous_yield = res.implied_yield;
  const auto direct = fit_quote_slice(implied, quotes, supplied_cfg);
  ASSERT_FALSE(direct);
  EXPECT_EQ(direct.error().code(), ErrorCode::OutOfRange);
}

// InvalidMarket cannot reach a caller: fit_quote_slice validates the market
// before carry resolution (quote_slice.cpp, fit_quote_slice), the carry guard's
// predicate (spot, both clocks, rate) is a subset of validate()'s, and inside the
// carry seam the cause would ride only on the Supplied fit's error, which carries
// no resolution (fit_supplied records it on success only). What a caller CAN
// observe is the documented contract, "the Supplied fit reports it", for every
// field the guard covers.
TEST(QuoteSliceCarry, EveryMalformedMarketFieldReportsTheSuppliedError) {
  const double nan = std::numeric_limits<double>::quiet_NaN();
  const double inf = std::numeric_limits<double>::infinity();
  const QuoteSliceMarket good{100.0, 1.0, 1.0, 0.04, 0.01, 0.0};
  std::vector<QuoteSliceMarket> bad;
  for (const double v : {0.0, -1.0, nan, inf}) {
    for (double QuoteSliceMarket::*field : {&QuoteSliceMarket::spot,
                                           &QuoteSliceMarket::variance_years,
                                           &QuoteSliceMarket::calendar_years}) {
      QuoteSliceMarket m = good;
      m.*field = v;
      bad.push_back(m);
    }
  }
  for (const double v : {nan, inf, -inf}) {
    QuoteSliceMarket m = good;
    m.rate = v;
    bad.push_back(m);
  }
  const std::vector<QuoteSliceQuote> quotes{{100.0, Side::Call, 1.0, 1.1}};
  for (std::size_t i = 0; i < bad.size(); ++i) {
    const auto implied = fit_quote_slice(bad[i], quotes, implied_config());
    const auto supplied = fit_quote_slice(bad[i], quotes);
    ASSERT_FALSE(implied) << i;
    ASSERT_FALSE(supplied) << i;
    EXPECT_EQ(implied.error().code(), ErrorCode::InvalidArgument) << i;
    EXPECT_EQ(implied.error().to_string(), supplied.error().to_string()) << i;
  }
  EXPECT_EQ(bad.size(), 15u);
}

// ── One authoritative spot per snapshot (lane C0) ─────────────────────────────

// The forward fit_quote_slice derives from `market` under `model` (prepare_fit), read
// off a fit of no quotes: the slice computes it before it admits anything.
double fitted_forward(const QuoteSliceMarket &market, QuoteSliceDividendModel model,
                      std::span<const CashDividend> dividends = {}) {
  QuoteSliceConfig cfg;
  cfg.dividend_model = model;
  const auto fit = fit_quote_slice(market, {}, cfg, dividends);
  if (!fit) {
    ADD_FAILURE() << fit.error().to_string();
    return std::numeric_limits<double>::quiet_NaN();
  }
  return fit->diagnostics.forward;
}

TEST(QuoteSliceSpotRebase, EveryAggregateModelKeepsItsForwardOnTheCalendarClock) {
  // A 2-day chain whose own spot sits 2 cents above the snapshot spot (the SPY
  // per-chain drift), on separate clocks: the shift lives on the calendar clock.
  const double snapshot = 772.98;
  struct Case {
    QuoteSliceDividendModel model;
    double pv;
  };
  for (const Case c : {Case{QuoteSliceDividendModel::ContinuousYield, 0.0},
                       Case{QuoteSliceDividendModel::AggregateFundingPresentValue, 1.75},
                       Case{QuoteSliceDividendModel::AggregatePresentValue, 1.75}}) {
    const QuoteSliceMarket market{773.00, 2.0 / 252.0, 2.0 / 365.0, 0.045, 0.012, c.pv};
    const auto rebased = rebase_quote_slice_spot(market, snapshot, c.model);
    ASSERT_TRUE(rebased) << to_string(c.model) << ": " << rebased.error().to_string();
    EXPECT_EQ(rebased->market.spot, snapshot);
    EXPECT_EQ(rebased->market.variance_years, market.variance_years);
    EXPECT_EQ(rebased->market.calendar_years, market.calendar_years);
    EXPECT_EQ(rebased->market.rate, market.rate);
    EXPECT_EQ(rebased->market.dividend_pv, market.dividend_pv);
    const double held = c.model == QuoteSliceDividendModel::AggregatePresentValue ? c.pv : 0.0;
    const double expected =
        std::log((snapshot - held) / (market.spot - held)) / market.calendar_years;
    // The closed form through a rounded ratio carries ~1e-16 / (shift T) relative error.
    EXPECT_NEAR(rebased->yield_shift, expected, 1.0e-10 * std::abs(expected));
    EXPECT_EQ(rebased->market.continuous_yield, market.continuous_yield + rebased->yield_shift);
    // One cent on a 2-day chain is ~24 bp of calendar yield: the shift is not small.
    EXPECT_LT(rebased->yield_shift, -0.004);
    const double before = fitted_forward(market, c.model);
    const double after = fitted_forward(rebased->market, c.model);
    EXPECT_NEAR(after, before, 1.0e-12 * before) << to_string(c.model);
    EXPECT_NEAR(rebased->forward, before, 1.0e-12 * before) << to_string(c.model);
  }
}

TEST(QuoteSliceSpotRebase, DatedCashSolvesTheLatticeForwardNotTheContinuousFormula) {
  const QuoteSliceMarket market{100.0, 0.5, 0.5, 0.04, 0.01, 0.0};
  const std::array<CashDividend, 2> dividends{{{0.1, 1.5}, {0.35, 1.5}}};
  const double snapshot = 100.30;
  const auto rebased = rebase_quote_slice_spot(market, snapshot,
                                               QuoteSliceDividendModel::DatedCash, dividends);
  ASSERT_TRUE(rebased) << rebased.error().to_string();
  const double before = fitted_forward(market, QuoteSliceDividendModel::DatedCash, dividends);
  const double after =
      fitted_forward(rebased->market, QuoteSliceDividendModel::DatedCash, dividends);
  EXPECT_NEAR(after, before, 1.0e-13 * before);
  // The continuous-yield identity would move the dividends' forward value too.
  const double continuous = std::log(snapshot / market.spot) / market.calendar_years;
  EXPECT_GT(std::abs(rebased->yield_shift - continuous), 5.0e-5);
  QuoteSliceMarket naive = market;
  naive.spot = snapshot;
  naive.continuous_yield += continuous;
  const double wrong = fitted_forward(naive, QuoteSliceDividendModel::DatedCash, dividends);
  EXPECT_GT(std::abs(wrong - before), 1.0e-3);
}

TEST(QuoteSliceSpotRebase, PreservesTheForwardNotTheAmericanPrice) {
  // FORWARD-preserving only: a deep in-the-money American put at its exercise value
  // moves with the spot itself, one for one, whatever the carry does.
  const QuoteSliceMarket market{100.02, 0.25, 0.25, 0.04, 0.0, 0.0};
  const auto rebased =
      rebase_quote_slice_spot(market, 100.00, QuoteSliceDividendModel::ContinuousYield);
  ASSERT_TRUE(rebased) << rebased.error().to_string();
  const auto raw = andersen_lake(market.spot, 140.0, 0.25, 0.2, 0.04, 0.0, Side::Put);
  const auto moved = andersen_lake(100.00, 140.0, 0.25, 0.2, 0.04,
                                   rebased->market.continuous_yield, Side::Put);
  ASSERT_TRUE(raw && moved);
  EXPECT_NEAR(*raw, 140.0 - 100.02, 1.0e-9);
  EXPECT_NEAR(*moved - *raw, 0.02, 1.0e-6);
}

TEST(QuoteSliceSpotRebase, RefusesInputsNoCarryCanReconcile) {
  const QuoteSliceMarket market{100.0, 0.5, 0.5, 0.04, 0.01, 2.0};
  EXPECT_EQ(rebase_quote_slice_spot(market, 1.5, QuoteSliceDividendModel::AggregatePresentValue)
                .error()
                .code(),
            ErrorCode::OutOfRange);
  for (const double bad : {0.0, -1.0, std::numeric_limits<double>::quiet_NaN()}) {
    EXPECT_EQ(rebase_quote_slice_spot(market, bad, QuoteSliceDividendModel::ContinuousYield)
                  .error()
                  .code(),
              ErrorCode::InvalidArgument);
  }
  const std::array<CashDividend, 1> dividends{{{0.1, 1.0}}};
  EXPECT_EQ(rebase_quote_slice_spot(market, 100.1,
                                    QuoteSliceDividendModel::AggregateFundingPresentValue,
                                    dividends)
                .error()
                .code(),
            ErrorCode::InvalidArgument);
  EXPECT_EQ(rebase_quote_slice_spot(market, 100.1, QuoteSliceDividendModel::DatedCash, dividends)
                .error()
                .code(),
            ErrorCode::InvalidArgument); // dated cash cannot also take an aggregate PV
  EXPECT_EQ(rebase_quote_slice_spot(market, 100.1, static_cast<QuoteSliceDividendModel>(9))
                .error()
                .code(),
            ErrorCode::InvalidArgument);
}

} // namespace
} // namespace atx::vol
