// FitRiskProbe (lane F, wave 3 of the risk-engine sprint 2026-10-24): the C++ probe that
// prices and Greeks every served row of a quote-slice fit run through the risk engine at
// the fitted vol (tools/fit_risk_probe.hpp). The tests pin: CSV parsing of the adapter's
// quotes.csv and the native predictions; the shadow clock against a REAL fit_quote_surface
// call (bit-equal carry); the reprice identity on the cold route; that price-only and
// refused rows are counted but never evaluated; the sanity classifier on injected
// violations; route / availability accounting; thread-count bit-identity; label
// governance; and the schema-v1 JSON round trip. FitRiskProbeReal.* is env-gated on
// ATX_VOL_UNIVERSE_FIT_DIR (a real panel bucket) and is not part of the 9-case gate.

#include <gtest/gtest.h>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <limits>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <vector>

#include "atx/vol/api/core/vol_time.hpp"
#include "atx/vol/api/fitting/quote_slice.hpp"
#include "atx/vol/api/pricing/american.hpp"
#include "fit_risk_probe.hpp"

using namespace atx::vol;
namespace fp = atx::vol::fit_probe;

namespace {

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
constexpr std::int64_t kSnapshotNs = 1791988200000000000; // 2026-10-14 10:30 ET

[[nodiscard]] bool same_bits(double a, double b) noexcept {
  return std::bit_cast<std::uint64_t>(a) == std::bit_cast<std::uint64_t>(b);
}

struct ExpirySpec {
  double variance_years;
  double calendar_years;
};

struct Synthetic {
  std::vector<fp::QuoteRow> quotes;
  std::vector<fp::PredictionRow> predictions;
};

// A smile with skew; strictly positive on the strikes used.
[[nodiscard]] double smile(double strike, double spot, double T) noexcept {
  const double k = std::log(strike / spot);
  return 0.28 - 0.10 * k + 0.25 * k * k + 0.02 * std::sqrt(T);
}

// One underlier's chains: every expiry x strike x side, every row served at the smile vol
// with the American price on the fitter's basis (variance clock, converted carry, accurate
// Andersen-Lake preset) — what a quote-bench run publishes for a fully served chain.
void add_underlier(Synthetic &out, const std::string &name, double spot, double pv,
                   double sdiv, std::span<const ExpirySpec> expiries,
                   std::span<const double> strikes, std::uint64_t &next_row,
                   std::uint64_t chain_base) {
  const double rate = 0.045;
  std::int32_t expiry_day = 20750;
  std::uint64_t chain = chain_base;
  for (const ExpirySpec &e : expiries) {
    ++chain;
    expiry_day += 30;
    const QuoteSliceMarket market{spot, e.variance_years, e.calendar_years, rate, sdiv, pv};
    const double r_v = fp::shadow_rate(market);
    const double q_v =
        fp::shadow_yield(market, sdiv, QuoteSliceDividendModel::AggregateFundingPresentValue);
    for (const double K : strikes) {
      for (const bool call : {true, false}) {
        const double sigma = smile(K, spot, e.variance_years);
        const auto price = andersen_lake(spot, K, e.variance_years, sigma, r_v, q_v,
                                         call ? Side::Call : Side::Put);
        ASSERT_TRUE(price.has_value()) << price.error().to_string();
        fp::QuoteRow q;
        q.row_id = next_row++;
        q.chain_id = chain;
        q.snapshot_ns = kSnapshotNs;
        q.underlier = name;
        q.root = name;
        q.expiry_days = expiry_day;
        q.strike = K;
        q.is_call = call;
        q.exercise = "American";
        q.spot = spot;
        q.bid = std::max(0.0, *price - 0.05);
        q.ask = *price + 0.05;
        q.years = e.variance_years;
        q.years_calendar = e.calendar_years;
        q.rate = rate;
        q.sdiv = sdiv;
        q.ddiv_pv = pv;
        q.input_status = "ok";
        out.quotes.push_back(q);
        out.predictions.push_back(fp::PredictionRow{q.row_id, chain, *price, sigma, "ok"});
      }
    }
  }
}

[[nodiscard]] Synthetic three_expiry_chain() {
  Synthetic s;
  const std::vector<ExpirySpec> expiries{{0.06, 0.062}, {0.25, 0.258}, {0.75, 0.77}};
  const std::vector<double> strikes{80.0, 90.0, 95.0, 100.0, 105.0, 110.0, 120.0};
  std::uint64_t row = 1000;
  add_underlier(s, "SYN", 100.0, 0.8, 0.012, expiries, strikes, row, 500);
  return s;
}

[[nodiscard]] fp::ProbeOptions options(fp::ProbeKernel kernel, unsigned threads = 1) {
  fp::ProbeOptions o;
  o.kernel = kernel;
  o.threads = threads;
  return o;
}

[[nodiscard]] fp::ProbeReport run_or_die(const Synthetic &s, const fp::ProbeOptions &o,
                                         std::vector<fp::ProbeRow> *rows = nullptr,
                                         std::span<const fp::LabelRow> labels = {}) {
  auto report = fp::run_fit_risk_probe(s.quotes, s.predictions, labels, {}, o, rows);
  EXPECT_TRUE(report.has_value()) << (report ? "" : report.error().to_string());
  return report ? *report : fp::ProbeReport{};
}

[[nodiscard]] std::uint64_t state_count(const fp::ProbeCounts &c, fp::RowState s) {
  return c.state[static_cast<std::size_t>(s)];
}

[[nodiscard]] std::uint64_t route_count(const fp::ProbeCounts &c, RiskRoute r) {
  return c.route[static_cast<std::size_t>(r)];
}

} // namespace

TEST(FitRiskProbe, ParsesQuotesAndPredictions) {
  // pyarrow's CSV writer quotes the header and every string; the native predictions are
  // unquoted with blanks for missing numbers; CRLF must parse too.
  const std::string quotes =
      "\"row_id\",\"chain_id\",\"snapshot_ns\",\"observation_ns\",\"underlier\",\"root\","
      "\"expiry_days\",\"strike\",\"is_call\",\"exercise\",\"spot_bid\",\"spot_ask\",\"spot\","
      "\"bid\",\"ask\",\"bid_size\",\"ask_size\",\"years\",\"years_calendar\",\"rate\",\"sdiv\","
      "\"ddiv\",\"ddiv_pv\",\"split\",\"input_status\"\r\n"
      "430336,15768868996136256924,1791988200000000000,1791988207305903000,\"ALKS\",\"ALKS\","
      "20742,32,1,\"American\",39.939998626708984,40.0099983215332,39.974998474121094,"
      "6.800000190734863,10,8,7,0.00939783826470375,0.00939783826470375,"
      "0.044896867126226425,-0.1280701607465744,0,0,\"development\",\"ok\"\r\n"
      "430352,15768868996136256924,1791988200000000000,1791988207305903000,\"ALKS\",\"ALKS\","
      "20742,32,0,\"American\",39.939998626708984,40.0099983215332,39.974998474121094,"
      "0,0.05000000074505806,0,26,0.00939783826470375,0.00939783826470375,"
      "0.044896867126226425,-0.1280701607465744,0,1.25,\"development\",\"invalid_option_quote\"\r\n";
  const auto parsed = fp::parse_quotes_csv(quotes);
  ASSERT_TRUE(parsed.has_value()) << parsed.error().to_string();
  ASSERT_EQ(parsed->size(), 2u);
  const fp::QuoteRow &a = (*parsed)[0];
  EXPECT_EQ(a.row_id, 430336u);
  EXPECT_EQ(a.chain_id, 15768868996136256924ull);
  EXPECT_EQ(a.snapshot_ns, 1791988200000000000);
  EXPECT_EQ(a.underlier, "ALKS");
  EXPECT_EQ(a.root, "ALKS");
  EXPECT_EQ(a.expiry_days, 20742);
  EXPECT_TRUE(same_bits(a.strike, 32.0));
  EXPECT_TRUE(a.is_call);
  EXPECT_EQ(a.exercise, "American");
  EXPECT_TRUE(same_bits(a.spot, 39.974998474121094));
  EXPECT_TRUE(same_bits(a.years, 0.00939783826470375));
  EXPECT_TRUE(same_bits(a.sdiv, -0.1280701607465744));
  EXPECT_EQ(a.input_status, "ok");
  EXPECT_FALSE((*parsed)[1].is_call);
  EXPECT_TRUE(same_bits((*parsed)[1].ddiv_pv, 1.25));
  EXPECT_EQ((*parsed)[1].input_status, "invalid_option_quote");

  const std::string predictions =
      "row_id,predicted_price,predicted_vol,status,chain_id,quote_status,vol_convention\n"
      "430336,8.0410812733348482,0.88777978444305228,ok,15768868996136256924,excluded_side,"
      "reimplied_repaired\n"
      "430352,,,insufficient_quotes,15768868996136256924,bidless_quote,\n";
  const auto pred = fp::parse_predictions_csv(predictions);
  ASSERT_TRUE(pred.has_value()) << pred.error().to_string();
  ASSERT_EQ(pred->size(), 2u);
  EXPECT_TRUE(same_bits((*pred)[0].price, 8.0410812733348482));
  EXPECT_TRUE(same_bits((*pred)[0].vol, 0.88777978444305228));
  EXPECT_EQ((*pred)[0].status, "ok");
  EXPECT_TRUE(std::isnan((*pred)[1].price) && std::isnan((*pred)[1].vol));
  EXPECT_EQ((*pred)[1].status, "insufficient_quotes");

  // Structural failures are typed, never silently dropped rows.
  const auto missing = fp::parse_predictions_csv("row_id,predicted_price,status\n1,2,ok\n");
  ASSERT_FALSE(missing.has_value());
  EXPECT_EQ(missing.error().code(), ErrorCode::ParseError);
  const auto ragged =
      fp::parse_predictions_csv("row_id,predicted_price,predicted_vol,status,chain_id\n1,2\n");
  ASSERT_FALSE(ragged.has_value());
  EXPECT_EQ(ragged.error().code(), ErrorCode::ParseError);
  const auto dup = fp::parse_predictions_csv(
      "row_id,predicted_price,predicted_vol,status,chain_id\n1,2,0.2,ok,9\n1,2,0.2,ok,9\n");
  ASSERT_FALSE(dup.has_value());
  EXPECT_EQ(dup.error().code(), ErrorCode::InvalidArgument);
  const auto bad_number = fp::parse_predictions_csv(
      "row_id,predicted_price,predicted_vol,status,chain_id\n1,2x,0.2,ok,9\n");
  ASSERT_FALSE(bad_number.has_value());
  EXPECT_EQ(bad_number.error().code(), ErrorCode::ParseError);

  const auto basis = fp::parse_basis_csv("chain_id,spot\n15768868996136256924,39.96\n");
  ASSERT_TRUE(basis.has_value()) << basis.error().to_string();
  ASSERT_EQ(basis->size(), 1u);
  EXPECT_EQ((*basis)[0].chain_id, 15768868996136256924ull);
  const auto labels = fp::parse_labels_csv(
      "row_id,reference_price,reference_vol,vendor_delta,vendor_vega\n7,1.5,0.3,0.42,\n");
  ASSERT_TRUE(labels.has_value()) << labels.error().to_string();
  EXPECT_TRUE(same_bits((*labels)[0].vendor_delta, 0.42));
  EXPECT_TRUE(std::isnan((*labels)[0].vendor_vega));
}

TEST(FitRiskProbe, ShadowClockReproducesFitCarry) {
  // A REAL fit on two clocks with an aggregate cash PV: the probe's restated conversion
  // must equal the fitter's own effective carry bit for bit, under every aggregate model.
  const double spot = 100.0;
  const double Tv = 0.30;
  const double Tc = 0.3125;
  const double rate = 0.045;
  const double yield = 0.012;
  const double pv = 1.5;
  for (const auto model : {QuoteSliceDividendModel::AggregateFundingPresentValue,
                           QuoteSliceDividendModel::AggregatePresentValue,
                           QuoteSliceDividendModel::ContinuousYield}) {
    const QuoteSliceMarket market{spot, Tv, Tc, rate, yield, pv};
    const double r_v = fp::shadow_rate(market);
    const double q_v = fp::shadow_yield(market, yield, model);
    std::vector<QuoteSliceQuote> quotes;
    for (double K = 70.0; K <= 130.0 + 1.0e-9; K += 5.0) {
      for (const Side side : {Side::Call, Side::Put}) {
        const auto price = andersen_lake(spot, K, Tv, smile(K, spot, Tv), r_v, q_v, side);
        ASSERT_TRUE(price.has_value());
        const double half = std::max(0.01, 0.02 * *price);
        quotes.push_back({K, side, std::max(*price - half, 0.0), *price + half});
      }
    }
    QuoteSliceConfig config;
    config.dividend_model = model;
    const std::vector<QuoteSurfaceExpiry> expiries{QuoteSurfaceExpiry{market, quotes}};
    const auto fit = fit_quote_surface(expiries, config);
    ASSERT_TRUE(fit.has_value()) << fit.error().to_string();
    ASSERT_EQ(fit->size(), 1u);
    const QuoteSliceDiagnostics &diag = (*fit)[0].fit.diagnostics;
    EXPECT_TRUE(same_bits(q_v, diag.effective_yield))
        << static_cast<int>(model) << ": " << q_v << " vs " << diag.effective_yield;
    EXPECT_TRUE(same_bits(r_v, diag.effective_rate)) << r_v << " vs " << diag.effective_rate;
    EXPECT_NE(diag.effective_yield, 0.0);
  }
  // A PV at or above the funding-discounted spot has no finite equivalent yield.
  EXPECT_TRUE(std::isnan(fp::shadow_yield(QuoteSliceMarket{2.0, 0.5, 0.5, 0.04, 0.0, 2.5}, 0.0,
                                          QuoteSliceDividendModel::AggregateFundingPresentValue)));
  // The synthetic Calendar365 expiry reproduces T_v to far below 1e-12 years, up to 10 y.
  for (const double T : {0.9440 / 365.0, 0.0026, 0.3, 2.5, 9.8}) {
    for (const std::int64_t offset : {std::int64_t{0}, std::int64_t{35}}) {
      const std::int64_t expiry = fp::shadow_expiry_ns(kSnapshotNs, T, offset);
      const auto back = time_to_expiry_years(kSnapshotNs, expiry, TimeSpec{});
      ASSERT_TRUE(back.has_value());
      EXPECT_LE(std::fabs(*back - T), fp::kShadowClockTolerance) << T << " " << offset;
    }
  }
}

TEST(FitRiskProbe, RepricesServedRowsAtFittedVol) {
  const Synthetic s = three_expiry_chain();
  std::vector<fp::ProbeRow> rows;
  const fp::ProbeReport report = run_or_die(s, options(fp::ProbeKernel::Cold), &rows);
  ASSERT_EQ(rows.size(), s.quotes.size());
  ASSERT_EQ(report.chains.size(), 3u);
  EXPECT_EQ(report.provenance.identity_kernel, "cold");
  std::size_t checked = 0;
  for (std::size_t i = 0; i < rows.size(); ++i) {
    const fp::ProbeRow &r = rows[i];
    ASSERT_EQ(r.state, fp::RowState::Evaluated) << i;
    ASSERT_EQ(r.status, RiskLaneStatus::Ok) << i;
    EXPECT_TRUE(r.route == RiskRoute::ColdAnalytic || r.route == RiskRoute::Exercised) << i;
    EXPECT_LE(std::fabs(r.identity_price - r.predicted_price),
              1.0e-8 * std::max(1.0, r.predicted_price))
        << "row " << r.row_id << " K " << r.strike << " T " << r.T;
    ++checked;
  }
  EXPECT_EQ(checked, s.quotes.size());
  EXPECT_EQ(report.totals.reprice_checked, s.quotes.size());
  EXPECT_EQ(report.totals.reprice_mismatch, 0u);
  EXPECT_LE(report.totals.reprice_max_rel, 1.0e-8);
}

TEST(FitRiskProbe, PriceOnlyAndRefusedRowsNeverEvaluated) {
  Synthetic s = three_expiry_chain();
  // Row 0: price-only (a price, no vol); row 1: refused by the fit; row 2: no prediction
  // at all; row 3: "ok" with an unusable vol; row 4: rejected at input.
  s.predictions[0].status = "ok_price_only";
  s.predictions[0].vol = kNaN;
  s.predictions[1].status = "insufficient_quotes";
  s.predictions[1].price = kNaN;
  s.predictions[1].vol = kNaN;
  s.predictions.erase(s.predictions.begin() + 2);
  s.predictions[2].vol = kNaN; // row 3 after the erase
  s.quotes[4].input_status = "invalid_option_quote";
  s.predictions[3].status = "input_rejected";
  s.predictions[3].price = kNaN;
  s.predictions[3].vol = kNaN;
  std::vector<fp::ProbeRow> rows;
  const fp::ProbeReport report = run_or_die(s, options(fp::ProbeKernel::Family), &rows);
  ASSERT_EQ(rows.size(), s.quotes.size());
  EXPECT_EQ(rows[0].state, fp::RowState::PriceOnly);
  EXPECT_EQ(rows[1].state, fp::RowState::NotServed);
  EXPECT_EQ(rows[2].state, fp::RowState::NotServed);
  EXPECT_EQ(rows[3].state, fp::RowState::InvalidInput);
  EXPECT_EQ(rows[4].state, fp::RowState::NotServed);
  for (std::size_t i = 0; i < 5; ++i) {
    EXPECT_EQ(rows[i].route, RiskRoute::None) << i;
    EXPECT_TRUE(std::isnan(rows[i].price)) << i;
    EXPECT_TRUE(std::isnan(rows[i].reprice_abs)) << i;
  }
  const fp::ProbeCounts &t = report.totals;
  EXPECT_EQ(t.rows, s.quotes.size());
  EXPECT_EQ(state_count(t, fp::RowState::PriceOnly), 1u);
  EXPECT_EQ(state_count(t, fp::RowState::NotServed), 3u);
  EXPECT_EQ(state_count(t, fp::RowState::InvalidInput), 1u);
  EXPECT_EQ(state_count(t, fp::RowState::Evaluated), s.quotes.size() - 5u);
  std::uint64_t routed = 0;
  for (const std::uint64_t n : t.route) routed += n;
  EXPECT_EQ(routed, state_count(t, fp::RowState::Evaluated));
  EXPECT_EQ(route_count(t, RiskRoute::None), 0u);
  EXPECT_TRUE(std::isfinite(t.reprice_max_abs));
  // Per-chain blocks partition the totals.
  std::uint64_t chain_rows = 0;
  for (const fp::ChainProbe &c : report.chains) chain_rows += c.counts.rows;
  EXPECT_EQ(chain_rows, t.rows);
}

TEST(FitRiskProbe, GreekSanityFlagsInjectedViolations) {
  using fp::SanityBit;
  EXPECT_EQ(fp::greek_sanity(Side::Call, 0.55, 0.03, 0.2), 0u);
  EXPECT_EQ(fp::greek_sanity(Side::Put, -0.45, 0.03, 0.2), 0u);
  EXPECT_EQ(fp::greek_sanity(Side::Call, 1.2, 0.03, 0.2), fp::sanity_mask(SanityBit::DeltaRange));
  EXPECT_EQ(fp::greek_sanity(Side::Put, 0.1, 0.03, 0.2), fp::sanity_mask(SanityBit::DeltaRange));
  EXPECT_EQ(fp::greek_sanity(Side::Call, 0.5, -1.0e-3, 0.2),
            fp::sanity_mask(SanityBit::GammaNeg));
  EXPECT_EQ(fp::greek_sanity(Side::Put, -0.5, 0.01, -0.2), fp::sanity_mask(SanityBit::VegaNeg));
  EXPECT_EQ(fp::greek_sanity(Side::Call, -0.1, -0.1, -0.1),
            fp::sanity_mask(SanityBit::DeltaRange) | fp::sanity_mask(SanityBit::GammaNeg) |
                fp::sanity_mask(SanityBit::VegaNeg));
  EXPECT_EQ(fp::greek_sanity(Side::Call, kNaN, 0.01, 0.2), 0u); // availability's job
  EXPECT_FALSE(fp::cp_delta_violation(0.55, -0.44, 0.01, 0.5));
  EXPECT_TRUE(fp::cp_delta_violation(0.9, -0.9, 0.0, 0.5));
  EXPECT_TRUE(fp::cp_delta_violation(0.1, -0.1, 0.0, 0.5));

  // Injected rows fold into the counts: one per sanity kind, one non-finite Greek, a
  // reprice mismatch, and a non-Ok lane that must not count as a sanity failure.
  fp::ProbeCounts counts;
  fp::ProbeRow ok_row;
  ok_row.state = fp::RowState::Evaluated;
  ok_row.status = RiskLaneStatus::Ok;
  ok_row.route = RiskRoute::Family;
  ok_row.side = Side::Call;
  ok_row.delta = 0.5;
  ok_row.gamma = 0.02;
  ok_row.vega = 0.3;
  ok_row.predicted_price = 2.0;
  ok_row.identity_price = 2.0;
  ok_row.reprice_abs = 0.0;
  ok_row.reprice_vol_bp = 0.0;
  ok_row.vendor_delta = kNaN;
  ok_row.vendor_vega = kNaN;
  fp::accumulate(counts, ok_row);
  for (const SanityBit bit : {SanityBit::DeltaRange, SanityBit::GammaNeg, SanityBit::VegaNeg,
                              SanityBit::CpDelta}) {
    fp::ProbeRow bad = ok_row;
    bad.sanity = fp::sanity_mask(bit);
    fp::accumulate(counts, bad);
  }
  fp::ProbeRow nonfinite = ok_row;
  nonfinite.gamma = kNaN;
  fp::accumulate(counts, nonfinite);
  fp::ProbeRow mismatch = ok_row;
  mismatch.identity_price = 2.0 + 1.0e-4;
  mismatch.reprice_abs = 1.0e-4;
  mismatch.reprice_vol_bp = 1.0e-4 / 0.3 * 1.0e4;
  fp::accumulate(counts, mismatch);
  fp::ProbeRow refused = ok_row;
  refused.status = RiskLaneStatus::Refused;
  refused.route = RiskRoute::Refused;
  refused.sanity = fp::sanity_mask(SanityBit::DeltaRange);
  refused.reprice_abs = kNaN;
  fp::accumulate(counts, refused);
  EXPECT_EQ(counts.rows, 8u);
  for (std::size_t k = 0; k < fp::kSanityKinds; ++k) EXPECT_EQ(counts.sanity_fail[k], 1u) << k;
  EXPECT_EQ(counts.nonfinite_greek, 1u);
  EXPECT_EQ(counts.reprice_mismatch, 1u);
  EXPECT_EQ(counts.reprice_checked, 7u); // every Ok row with a finite identity; not the refused
  EXPECT_EQ(counts.lane[static_cast<std::size_t>(RiskLaneStatus::Refused)], 1u);
  EXPECT_EQ(route_count(counts, RiskRoute::Refused), 1u);
  EXPECT_DOUBLE_EQ(counts.reprice_max_abs, 1.0e-4);
}

TEST(FitRiskProbe, RoutesAndAvailabilityCounted) {
  // A chain below the family's tau floor (1 vol-day: cold fallback) and deep ITM puts
  // across the exercise boundary (near edge / exercised) next to ordinary contracts.
  Synthetic s;
  const std::vector<ExpirySpec> expiries{{1.0 / 365.0, 1.0 / 365.0}, {0.25, 0.25}};
  const std::vector<double> strikes{60.0, 95.0, 100.0, 105.0, 130.0, 150.0, 170.0};
  std::uint64_t row = 1;
  add_underlier(s, "EDGE", 100.0, 0.0, 0.0, expiries, strikes, row, 900);
  std::vector<fp::ProbeRow> rows;
  const fp::ProbeReport report = run_or_die(s, options(fp::ProbeKernel::Family), &rows);
  const fp::ProbeCounts &t = report.totals;
  const std::uint64_t evaluated = state_count(t, fp::RowState::Evaluated);
  EXPECT_EQ(evaluated, s.quotes.size());
  std::uint64_t routed = 0;
  for (const std::uint64_t n : t.route) routed += n;
  std::uint64_t laned = 0;
  for (const std::uint64_t n : t.lane) laned += n;
  EXPECT_EQ(routed, evaluated);
  EXPECT_EQ(laned, evaluated);
  EXPECT_GT(route_count(t, RiskRoute::Family), 0u);
  EXPECT_GT(t.cold_fallback, 0u);
  EXPECT_GT(t.near_edge + route_count(t, RiskRoute::Exercised), 0u);
  std::uint64_t flagged_fallback = 0;
  for (const fp::ProbeRow &r : rows) {
    if (has_flag(r.flags, RiskFlag::ColdFallback)) {
      ++flagged_fallback;
      EXPECT_TRUE(r.route == RiskRoute::ColdAnalytic || r.route == RiskRoute::ColdFd ||
                  r.route == RiskRoute::Exercised || r.route == RiskRoute::Refused)
          << r.row_id;
    }
    // Every 1-vol-day row sits below the family's tau floor: never served by a family.
    if (r.T < 3.0 / 365.0) EXPECT_NE(r.route, RiskRoute::Family) << r.row_id;
  }
  EXPECT_EQ(flagged_fallback, t.cold_fallback);
  // Chain blocks carry the same accounting.
  std::uint64_t chain_family = 0;
  for (const fp::ChainProbe &c : report.chains) chain_family += route_count(c.counts, RiskRoute::Family);
  EXPECT_EQ(chain_family, route_count(t, RiskRoute::Family));
}

TEST(FitRiskProbe, ThreadCountBitIdentical) {
  Synthetic s;
  const std::vector<ExpirySpec> expiries{{0.04, 0.041}, {0.2, 0.205}, {0.6, 0.61}};
  const std::vector<double> strikes{85.0, 95.0, 100.0, 105.0, 115.0};
  std::uint64_t row = 1;
  for (int u = 0; u < 6; ++u) {
    add_underlier(s, "U" + std::to_string(u), 80.0 + 10.0 * u, 0.3 * u, 0.005 * u, expiries,
                  strikes, row, 100u * static_cast<std::uint64_t>(u + 1));
  }
  std::vector<fp::ProbeRow> base_rows;
  const std::string base =
      fp::probe_report_to_json(run_or_die(s, options(fp::ProbeKernel::Both, 1), &base_rows));
  for (const unsigned threads : {2u, 8u}) {
    std::vector<fp::ProbeRow> rows;
    fp::ProbeReport report = run_or_die(s, options(fp::ProbeKernel::Both, threads), &rows);
    report.provenance.threads = 1; // the only field allowed to differ
    EXPECT_EQ(fp::probe_report_to_json(report), base) << threads;
    ASSERT_EQ(rows.size(), base_rows.size());
    for (std::size_t i = 0; i < rows.size(); ++i) {
      EXPECT_TRUE(same_bits(rows[i].price, base_rows[i].price)) << threads << " " << i;
      EXPECT_TRUE(same_bits(rows[i].delta, base_rows[i].delta)) << threads << " " << i;
      EXPECT_TRUE(same_bits(rows[i].gamma, base_rows[i].gamma)) << threads << " " << i;
      EXPECT_TRUE(same_bits(rows[i].vega, base_rows[i].vega)) << threads << " " << i;
      EXPECT_TRUE(same_bits(rows[i].identity_price, base_rows[i].identity_price)) << i;
      EXPECT_EQ(rows[i].route, base_rows[i].route) << i;
      EXPECT_EQ(rows[i].flags, base_rows[i].flags) << i;
    }
  }
}

TEST(FitRiskProbe, LabelsRefusedOutsideDevelopmentSplit) {
  EXPECT_TRUE(fp::labels_allowed(R"({"schema_version":1,"rows_by_split":{"development":72348}})")
                  .has_value());
  for (const char *manifest :
       {R"({"rows_by_split":{"universe":2082476}})",
        R"({"rows_by_split":{"development":5,"validation":3}})",
        R"({"rows_by_split":{"validation":3}})", R"({"rows_by_split":{}})",
        R"({"rows_by_split":{"development":0}})"}) {
    const Status st = fp::labels_allowed(manifest);
    ASSERT_FALSE(st.has_value()) << manifest;
    EXPECT_EQ(st.error().code(), ErrorCode::PermissionDenied) << manifest;
  }
  for (const char *broken : {"{", R"({"rows":1})", "[]"}) {
    const Status st = fp::labels_allowed(broken);
    ASSERT_FALSE(st.has_value()) << broken;
    EXPECT_EQ(st.error().code(), ErrorCode::ParseError) << broken;
  }

  const Synthetic s = three_expiry_chain();
  std::vector<fp::LabelRow> labels;
  for (const fp::QuoteRow &q : s.quotes) {
    labels.push_back(fp::LabelRow{q.row_id, 1.0, 0.3, q.is_call ? 0.5 : -0.5, 20.0});
  }
  const auto refused = fp::run_fit_risk_probe(s.quotes, s.predictions, labels, {},
                                              options(fp::ProbeKernel::Family), nullptr);
  ASSERT_FALSE(refused.has_value());
  EXPECT_EQ(refused.error().code(), ErrorCode::PermissionDenied);

  fp::ProbeOptions allowed = options(fp::ProbeKernel::Family);
  allowed.labels_allowed = true;
  std::vector<fp::ProbeRow> rows;
  const auto report = fp::run_fit_risk_probe(s.quotes, s.predictions, labels, {}, allowed, &rows);
  ASSERT_TRUE(report.has_value()) << report.error().to_string();
  EXPECT_TRUE(report->provenance.labels);
  EXPECT_EQ(report->totals.vendor_n, s.quotes.size());
  EXPECT_TRUE(std::isfinite(fp::vendor_delta_mae(report->totals)));
  // Vendor vega arrives per vol point: 20.0 per point is 2000 per unit vol.
  EXPECT_DOUBLE_EQ(rows[0].vendor_vega, 2000.0);
  // Without labels the vendor block is absent (NaN), never zero.
  const fp::ProbeReport plain = run_or_die(s, options(fp::ProbeKernel::Family));
  EXPECT_FALSE(plain.provenance.labels);
  EXPECT_EQ(plain.totals.vendor_n, 0u);
  EXPECT_TRUE(std::isnan(fp::vendor_delta_mae(plain.totals)));
}

TEST(FitRiskProbe, JsonSchemaV1RoundTrip) {
  Synthetic s = three_expiry_chain();
  s.predictions[0].status = "ok_price_only";
  s.predictions[0].vol = kNaN;
  const fp::ProbeReport report = run_or_die(s, options(fp::ProbeKernel::Both));
  const std::string json = fp::probe_report_to_json(report);
  EXPECT_NE(json.find("\"schema\":\"atx-vol-fit-risk-probe\""), std::string::npos);
  EXPECT_NE(json.find("\"schema_version\":1"), std::string::npos);
  const auto back = fp::probe_report_from_json(json);
  ASSERT_TRUE(back.has_value()) << back.error().to_string();
  EXPECT_EQ(fp::probe_report_to_json(*back), json);
  ASSERT_EQ(back->chains.size(), report.chains.size());
  EXPECT_EQ(back->chains[0].chain_id, report.chains[0].chain_id);
  EXPECT_EQ(back->chains[0].underlier, "SYN");
  EXPECT_EQ(back->totals.rows, report.totals.rows);
  EXPECT_EQ(back->provenance.kernel, "both");
  // A 64-bit chain id survives the round trip exactly.
  fp::ProbeReport big = *back;
  big.chains[0].chain_id = 18446744073709551557ull;
  const auto big_back = fp::probe_report_from_json(fp::probe_report_to_json(big));
  ASSERT_TRUE(big_back.has_value());
  EXPECT_EQ(big_back->chains[0].chain_id, 18446744073709551557ull);

  std::string v2 = json;
  v2.replace(v2.find("\"schema_version\":1"), 18, "\"schema_version\":2");
  const auto wrong_version = fp::probe_report_from_json(v2);
  ASSERT_FALSE(wrong_version.has_value());
  EXPECT_EQ(wrong_version.error().code(), ErrorCode::InvalidArgument);
  std::string foreign = json;
  foreign.replace(foreign.find("atx-vol-fit-risk-probe"), 22, "atx-vol-other-schema00");
  const auto wrong_schema = fp::probe_report_from_json(foreign);
  ASSERT_FALSE(wrong_schema.has_value());
  EXPECT_EQ(wrong_schema.error().code(), ErrorCode::InvalidArgument);
  const auto truncated = fp::probe_report_from_json(json.substr(0, json.size() / 2));
  ASSERT_FALSE(truncated.has_value());
  EXPECT_EQ(truncated.error().code(), ErrorCode::ParseError);
}

namespace {

[[nodiscard]] std::string universe_fit_dir() {
#if defined(_WIN32)
  char *value = nullptr;
  std::size_t size = 0U;
  std::string root;
  if (::_dupenv_s(&value, &size, "ATX_VOL_UNIVERSE_FIT_DIR") == 0 && value != nullptr) {
    root = value;
  }
  std::free(value);
  return root;
#else
  const char *value = std::getenv("ATX_VOL_UNIVERSE_FIT_DIR");
  return value != nullptr ? std::string(value) : std::string();
#endif
}

[[nodiscard]] std::string slurp(const std::string &path) {
  std::ifstream in(path, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

} // namespace

// Env-gated: <ATX_VOL_UNIVERSE_FIT_DIR>/smoke/{quotes.csv,predictions.csv} is one real
// panel bucket (1000) shard. Every row must be accounted for and some evaluated.
TEST(FitRiskProbeReal, PanelBucket1000Smoke) {
  const std::string dir = universe_fit_dir();
  if (dir.empty()) GTEST_SKIP() << "ATX_VOL_UNIVERSE_FIT_DIR not set";
  const auto quotes = fp::parse_quotes_csv(slurp(dir + "/smoke/quotes.csv"));
  ASSERT_TRUE(quotes.has_value()) << quotes.error().to_string();
  const auto predictions = fp::parse_predictions_csv(slurp(dir + "/smoke/predictions.csv"));
  ASSERT_TRUE(predictions.has_value()) << predictions.error().to_string();
  fp::ProbeOptions o;
  o.kernel = fp::ProbeKernel::Family;
  const auto report = fp::run_fit_risk_probe(*quotes, *predictions, {}, {}, o, nullptr);
  ASSERT_TRUE(report.has_value()) << report.error().to_string();
  std::uint64_t states = 0;
  for (const std::uint64_t n : report->totals.state) states += n;
  EXPECT_EQ(states, quotes->size());
  EXPECT_GT(report->totals.state[static_cast<std::size_t>(fp::RowState::Evaluated)], 0u);
}
