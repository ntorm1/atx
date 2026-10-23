// Lane A1 (r6, 2026-10-23): European-only outputs on a declared common-model surface
// (api/analytics/common_model_analytics.hpp; deep review F13) - the American quoting surface
// is refused with a typed reason and no number; on the Markov common model the density is
// the compiled chain's own terminal law (it reprices the model's European control to
// rounding), its CDF, the variance strip, the model's own local volatility, the
// declaration and the budgets.
#include "atx/vol/api/analytics/common_model_analytics.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "atx/vol/api/fitting/quote_slice.hpp"
#include "atx/vol/api/fitting/serve_book.hpp"
#include "atx/vol/api/fitting/slice_serve_record.hpp"
#include "atx/vol/api/pricing/american.hpp"
#include "atx/vol/api/pricing/american_markov.hpp"

namespace atx::vol {
namespace {

constexpr double kSpot = 100.0;

// A quoting record of the H0-A / H1 fixture (strikes 70-130, T = 0.25).
std::shared_ptr<const SliceServeRecord> quoting_record() {
  static const auto record = [] {
    const QuoteSliceMarket market{kSpot, 0.25, 0.25, 0.03, 0.01, 0.0};
    const double forward = kSpot * std::exp(0.02 * 0.25);
    std::vector<QuoteSliceQuote> quotes;
    for (int i = 0; i < 25; ++i) {
      const double K = 70.0 + 2.5 * static_cast<double>(i);
      const double k = std::log(K / forward);
      for (const Side side : {Side::Call, Side::Put}) {
        const Result<double> p =
            andersen_lake(kSpot, K, 0.25, 0.22 - 0.10 * k + 0.25 * k * k, 0.03, 0.01, side);
        if (!p) continue;
        const double spread = std::max(0.01, 0.01 * *p);
        quotes.push_back({K, side, std::max(*p - spread, 0.0), *p + spread, 0.01});
      }
    }
    Result<QuoteSliceFit> fit = fit_quote_slice(market, quotes);
    EXPECT_TRUE(fit.has_value());
    if (!fit) return std::shared_ptr<const SliceServeRecord>{};
    ServeRecordPolicy policy;
    policy.ladders = false;
    Result<std::shared_ptr<const SliceServeRecord>> built = build_serve_record(*fit, policy);
    EXPECT_TRUE(built.has_value());
    return built ? *built : std::shared_ptr<const SliceServeRecord>{};
  }();
  return record;
}

// One constant-volatility interval on a fine uniform grid (explicit chain).
Result<AmericanMarkovModel> constant_vol_model(double sigma, double T, double r, double q,
                                               double cash = 0.0) {
  static std::vector<AmericanMarkovInterval> intervals;
  intervals = {AmericanMarkovInterval{T, T, r, q, sigma, cash}};
  AmericanMarkovSpec spec{};
  spec.spot = kSpot;
  spec.intervals = intervals;
  spec.config.grid_nodes = 401;
  spec.config.upper_spot_multiple = 3.0;
  spec.config.max_transitions = 16384;
  spec.config.max_stored_rows = 4000000;
  spec.config.max_transition_work = 4000000;
  return compile_american_markov(spec);
}

double normal_cdf(double x) { return 0.5 * std::erfc(-x / std::sqrt(2.0)); }

// ── Refusals on the quoting surface (F13) ───────────────────────────────────────────────

TEST(EuropeanOutputs, TheQuotingRecordIsRefusedWithATypedReasonAndNoNumber) {
  ASSERT_NE(quoting_record(), nullptr);
  const EuropeanOutputs out =
      european_outputs(EuropeanSurfaceRef::quoting_record(*quoting_record()));
  EXPECT_EQ(out.status, AnalyticsStatus::Refused);
  EXPECT_EQ(out.refusal, EuropeanOutputRefusal::AmericanQuotingSurface);
  EXPECT_EQ(out.declaration.model, AnalyticsModel::QuotingRecord);
  EXPECT_EQ(out.declaration.record_fit_sha256, quoting_record()->version().fit_sha256);
  EXPECT_TRUE(out.density.spot.empty() && out.density.mass.empty() && out.density.cdf.empty());
  EXPECT_TRUE(std::isnan(out.density.total_mass) && std::isnan(out.density.mean));
  EXPECT_TRUE(std::isnan(out.strip.total_variance));
  EXPECT_TRUE(out.local_vol.volatility.empty());
}

TEST(EuropeanOutputs, ABookExpiryIsTheQuotingSurfaceTooAndIsRefused) {
  ServeBookPolicy policy;
  policy.build_workers = 0;
  ServeBook book(policy);
  ServeBookEntry e;
  e.expiry_id = 4;
  e.record = quoting_record();
  ASSERT_TRUE(book.publish_expiry(e).has_value());
  const EuropeanOutputs out = european_outputs(EuropeanSurfaceRef::quoting_book_expiry(book, 4));
  EXPECT_EQ(out.status, AnalyticsStatus::Refused);
  EXPECT_EQ(out.refusal, EuropeanOutputRefusal::AmericanQuotingSurface);
  EXPECT_EQ(out.declaration.model, AnalyticsModel::QuotingRecord);
  EXPECT_EQ(out.declaration.book_version, book.version());
  EXPECT_EQ(out.declaration.expiry_id, 4u);
  EXPECT_TRUE(out.density.mass.empty());
  EXPECT_TRUE(std::isnan(out.strip.total_variance));
}

TEST(EuropeanOutputs, AnEmptyReferenceOrAMissingExpiryIsRefused) {
  const EuropeanOutputs none = european_outputs(EuropeanSurfaceRef{});
  EXPECT_EQ(none.status, AnalyticsStatus::Refused);
  EXPECT_EQ(none.refusal, EuropeanOutputRefusal::NoSurface);
  const Result<AmericanMarkovModel> model = constant_vol_model(0.2, 0.25, 0.03, 0.01);
  ASSERT_TRUE(model.has_value()) << model.error().to_string();
  const EuropeanOutputs missing =
      european_outputs(EuropeanSurfaceRef::common_model(*model, 3));
  EXPECT_EQ(missing.status, AnalyticsStatus::Refused);
  EXPECT_EQ(missing.refusal, EuropeanOutputRefusal::NoExpiryLayer);
  EXPECT_TRUE(missing.density.mass.empty());
}

// ── The common model ─────────────────────────────────────────────────────────────────────

TEST(EuropeanOutputs, TheDensityIsTheChainsTerminalLawAndRepricesItsEuropeanControl) {
  const Result<AmericanMarkovModel> model = constant_vol_model(0.2, 0.25, 0.03, 0.01);
  ASSERT_TRUE(model.has_value()) << model.error().to_string();
  const EuropeanOutputs out = european_outputs(EuropeanSurfaceRef::common_model(*model, 0, 42));
  ASSERT_EQ(out.status, AnalyticsStatus::Ok);
  EXPECT_EQ(out.refusal, EuropeanOutputRefusal::None);
  const CommonModelDensity &d = out.density;
  ASSERT_EQ(d.spot.size(), d.mass.size());
  ASSERT_EQ(d.cdf.size(), d.mass.size());
  ASSERT_GT(d.mass.size(), 100u);
  EXPECT_TRUE(std::is_sorted(d.spot.begin(), d.spot.end()));
  EXPECT_NEAR(d.total_mass, 1.0, 1.0e-12);
  EXPECT_NEAR(d.cdf.back(), d.total_mass, 1.0e-15);
  EXPECT_NEAR(d.mean / d.forward, 1.0, 1.0e-12);
  // A product of one rounded factor per transition: |relative error| <= transitions x eps.
  EXPECT_NEAR(d.discount, std::exp(-0.03 * 0.25),
              static_cast<double>(out.budgets.transitions_applied) * 2.3e-16);
  // The density reprices the model's own European control (backward rollback) to rounding.
  AmericanMarkovWorkspace ws;
  ASSERT_TRUE(ws.prepare(*model).has_value());
  for (const double K : {80.0, 95.0, 100.0, 105.0, 120.0}) {
    for (const Side side : {Side::Call, Side::Put}) {
      const Result<double> control =
          price_american_markov(*model, K, side, 0, ws, ExerciseStyle::European);
      ASSERT_TRUE(control.has_value());
      double value = 0.0;
      for (std::size_t i = 0; i < d.spot.size(); ++i) {
        const double payoff = side == Side::Call ? std::max(d.spot[i] - K, 0.0)
                                                 : std::max(K - d.spot[i], 0.0);
        value += d.mass[i] * payoff;
      }
      EXPECT_NEAR(d.discount * value, *control, 1.0e-10 * std::max(1.0, *control));
    }
  }
}

// Accuracy budget: the compiled constant-vol chain against the lognormal it discretises.
TEST(EuropeanOutputs, AConstantVolChainsCdfAndStripSitOnTheLognormal) {
  const double sigma = 0.2;
  const double T = 0.25;
  const Result<AmericanMarkovModel> model = constant_vol_model(sigma, T, 0.03, 0.01);
  ASSERT_TRUE(model.has_value()) << model.error().to_string();
  const EuropeanOutputs out = european_outputs(EuropeanSurfaceRef::common_model(*model, 0));
  ASSERT_EQ(out.status, AnalyticsStatus::Ok);
  const CommonModelDensity &d = out.density;
  double worst = 0.0;
  for (std::size_t i = 0; i + 1 < d.spot.size(); ++i) {
    // The mass at node i stands for the cell up to the midpoint with node i + 1.
    const double edge = 0.5 * (d.spot[i] + d.spot[i + 1]);
    const double z =
        (std::log(edge / d.forward) + 0.5 * sigma * sigma * T) / (sigma * std::sqrt(T));
    worst = std::max(worst, std::abs(d.cdf[i] - normal_cdf(z)));
  }
  std::printf("[ cdf-vs-lognormal ] max |dCDF| %.3e over %zu nodes\n", worst, d.spot.size());
  EXPECT_LT(worst, 5.0e-4); // measured 1.62e-4 on this grid (401 nodes, 3 x spot)
  const CommonModelVarianceStrip &s = out.strip;
  EXPECT_EQ(out.strip_refusal, EuropeanOutputRefusal::None);
  std::printf("[ strip ] total variance %.9f vs sigma^2 T %.9f\n", s.total_variance,
              sigma * sigma * T);
  EXPECT_NEAR(s.total_variance, sigma * sigma * T, 1.0e-6); // measured 3.0e-7
  EXPECT_NEAR(s.vol_variance_clock, std::sqrt(s.total_variance / T), 1.0e-15);
  EXPECT_EQ(s.variance_years, T);
  EXPECT_EQ(s.calendar_years, T);
}

TEST(EuropeanOutputs, TheLocalVolIsTheModelsOwn) {
  static const std::array<AmericanMarkovLocalVolNode, 3> nodes{
      AmericanMarkovLocalVolNode{-0.2, 1.3}, AmericanMarkovLocalVolNode{0.0, 1.0},
      AmericanMarkovLocalVolNode{0.2, 0.8}};
  static std::vector<AmericanMarkovInterval> intervals;
  intervals = {AmericanMarkovInterval{0.1, 0.1, 0.02, 0.0, 0.25, 0.0, nodes},
               AmericanMarkovInterval{0.3, 0.3, 0.02, 0.0, 0.2, 0.0, nodes}};
  AmericanMarkovSpec spec{};
  spec.spot = kSpot;
  spec.intervals = intervals;
  const Result<AmericanMarkovModel> model = compile_american_markov(spec);
  ASSERT_TRUE(model.has_value()) << model.error().to_string();
  const EuropeanOutputs out = european_outputs(EuropeanSurfaceRef::common_model(*model, 1));
  ASSERT_EQ(out.status, AnalyticsStatus::Ok);
  const CommonModelLocalVol &lv = out.local_vol;
  std::size_t expected = 0;
  for (const AmericanMarkovIntervalData &data : model->interval_data()) {
    expected += data.state_volatilities.size();
  }
  ASSERT_EQ(lv.volatility.size(), expected);
  ASSERT_EQ(lv.spot.size(), expected);
  std::size_t row = 0;
  for (const AmericanMarkovIntervalData &data : model->interval_data()) {
    const AmericanMarkovLayer &layer = model->layers()[data.first_layer];
    for (std::size_t i = 0; i < data.state_volatilities.size(); ++i, ++row) {
      EXPECT_EQ(lv.volatility[row], data.state_volatilities[i]);
      EXPECT_EQ(lv.spot[row], layer.growth * layer.states()[i]);
      EXPECT_EQ(lv.calendar_time[row], layer.calendar_time);
    }
  }
}

TEST(EuropeanOutputs, ACashEventRefusesTheStripButNotTheDensity) {
  static std::vector<AmericanMarkovInterval> intervals;
  intervals = {AmericanMarkovInterval{0.1, 0.1, 0.03, 0.0, 0.2, 2.0},
               AmericanMarkovInterval{0.25, 0.25, 0.03, 0.0, 0.2, 0.0}};
  AmericanMarkovSpec spec{};
  spec.spot = kSpot;
  spec.intervals = intervals;
  const Result<AmericanMarkovModel> model = compile_american_markov(spec);
  ASSERT_TRUE(model.has_value()) << model.error().to_string();
  const EuropeanOutputs out = european_outputs(EuropeanSurfaceRef::common_model(*model, 1));
  ASSERT_EQ(out.status, AnalyticsStatus::Ok);
  EXPECT_FALSE(out.density.mass.empty());
  EXPECT_NEAR(out.density.total_mass, 1.0, 1.0e-12);
  EXPECT_EQ(out.strip_refusal, EuropeanOutputRefusal::CashDividendJump);
  EXPECT_TRUE(std::isnan(out.strip.total_variance));
}

TEST(EuropeanOutputs, TheDeclarationNamesTheCommonModelAndItsBudgets) {
  const Result<AmericanMarkovModel> model = constant_vol_model(0.2, 0.25, 0.03, 0.01);
  ASSERT_TRUE(model.has_value()) << model.error().to_string();
  const EuropeanOutputs out = european_outputs(EuropeanSurfaceRef::common_model(*model, 0, 9));
  ASSERT_EQ(out.status, AnalyticsStatus::Ok);
  const AnalyticsDeclaration &d = out.declaration;
  EXPECT_EQ(d.model, AnalyticsModel::CommonModelEuropean);
  EXPECT_EQ(d.entry_tag, 9u);
  EXPECT_EQ(d.book_version, 0u);
  EXPECT_EQ(d.clock.tau, 0.25);
  const CommonModelBudgets &b = out.budgets;
  EXPECT_LE(b.mass_error, 1.0e-12);
  EXPECT_LE(b.mean_error, 1.0e-12);
  EXPECT_EQ(b.transitions_applied, model->expiry_layers()[0]);
  EXPECT_EQ(b.stored_rows, model->diagnostics().stored_rows);
  EXPECT_EQ(b.max_states, model->diagnostics().max_states);
  EXPECT_GT(b.output_bytes, 0u);
}

} // namespace
} // namespace atx::vol
