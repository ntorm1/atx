// Per-expiry calibration record (api/fitting/expiry_record.hpp) and the record
// tick route (api/pricing/chain_tick.hpp). Pricing hot-path audit 2026-10-16,
// item 2. Accuracy is measured against the cold accurate Andersen-Lake solver
// (andersen_lake / american_greeks_al with the nullopt preset).

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <optional>
#include <vector>

#include "atx/vol/api/fitting/expiry_record.hpp"
#include "atx/vol/api/pricing/american.hpp"
#include "atx/vol/api/pricing/black76.hpp"
#include "atx/vol/api/pricing/chain_tick.hpp"
#include "pricing/american_boundary.hpp"
#include "support/alloc_counter.hpp"

namespace {

using namespace atx::vol;

constexpr double kS0 = 100.0;
constexpr double kR = 0.043;
constexpr double kQ = 0.015;

[[nodiscard]] bool bits_equal(double a, double b) noexcept {
  std::uint64_t ba = 0;
  std::uint64_t bb = 0;
  std::memcpy(&ba, &a, sizeof ba);
  std::memcpy(&bb, &b, sizeof bb);
  return ba == bb;
}

[[nodiscard]] double smile(double k) noexcept { return 0.25 + 0.10 * std::abs(k); }

// One record for (T, side) over a box covering +/-3 sigma sqrt(T) strikes plus
// a 5% spot move, sigma in [0.6, 1.6] x the smile range.
[[nodiscard]] ExpiryRecordSpec spec_for(double T, Side side) {
  const double half = 3.0 * 0.25 * std::sqrt(T);
  ExpiryRecordSpec s;
  s.expiry = T;
  s.carry = ExpiryCarry{kR, kQ, {}};
  s.side = side;
  s.k_min = -half - 0.05;
  s.k_max = half + 0.05;
  s.sigma_min = 0.6 * smile(0.0);
  s.sigma_max = 1.6 * smile(half);
  return s;
}

struct Built {
  ExpiryRecordSpec spec;
  ExpiryRecord rec;
};

[[nodiscard]] const std::vector<Built> &records() {
  static const std::vector<Built> recs = [] {
    std::vector<Built> out;
    for (const double T : {0.02, 0.25, 1.5}) {
      for (const Side sd : {Side::Put, Side::Call}) {
        const ExpiryRecordSpec s = spec_for(T, sd);
        Result<ExpiryRecord> r = ExpiryRecord::build(s);
        EXPECT_TRUE(r.has_value()) << "T=" << T;
        if (r) {
          out.push_back(Built{s, *r});
        }
      }
    }
    return out;
  }();
  return recs;
}

// ── Accuracy against the cold solver ─────────────────────────────────────────

TEST(ExpiryRecord, CertifiedErrorIsSmallAndCoversTheGrid) {
  ASSERT_EQ(records().size(), 6u);
  for (const Built &b : records()) {
    const double err = b.rec.certified_max_abs_error_fwd();
    std::printf("[record] T=%.3f side=%s edge=%d cert_err_fwd=%.3e points=%u\n", b.spec.expiry,
                b.spec.side == Side::Put ? "put" : "call", b.rec.has_exercise_edge() ? 1 : 0, err,
                b.rec.certified_points());
    EXPECT_TRUE(std::isfinite(err));
    EXPECT_GT(b.rec.certified_points(), 150u);
    // Measured 2026-10-16: <= 1.3e-8 forward units on these six records.
    EXPECT_LT(err, 1.0e-7) << "T=" << b.spec.expiry;
  }
}

TEST(ExpiryRecord, PricesMatchColdAndersenLakeAcrossSpotMovesStrikesAndSides) {
  double worst = 0.0;
  for (const Built &b : records()) {
    const double T = b.spec.expiry;
    const double half = 3.0 * 0.25 * std::sqrt(T);
    for (const double dS : {-0.04, -0.01, 0.0, 0.02, 0.045}) {
      const double S = kS0 * std::exp(dS);
      for (int j = 0; j <= 8; ++j) {
        const double k = -half + 2.0 * half * j / 8.0;
        const double K = kS0 * std::exp((kR - kQ) * T) * std::exp(k);
        const double sig = smile(k);
        const Result<double> cold =
            andersen_lake(S, K, T, sig, kR, kQ, b.spec.side, std::nullopt);
        ASSERT_TRUE(cold.has_value());
        const double got = b.rec.price(S, K, sig);
        const double err = std::abs(got - *cold);
        worst = std::max(worst, err);
        // Measured worst 9.4e-7 $ at S ~ 100 (2026-10-16).
        EXPECT_LT(err, 1.0e-5) << "T=" << T << " K=" << K << " S=" << S;
      }
    }
  }
  std::printf("[record] worst |price - cold| over spot/strike/side grid: %.3e $ (S~100)\n",
              worst);
}

TEST(ExpiryRecord, GreeksMatchColdAnalyticGreeksWithinEconomicTolerance) {
  double wd = 0.0, wg = 0.0, wv = 0.0, wt = 0.0;
  for (const Built &b : records()) {
    const double T = b.spec.expiry;
    const double half = 3.0 * 0.25 * std::sqrt(T);
    for (const double dS : {-0.02, 0.0, 0.03}) {
      const double S = kS0 * std::exp(dS);
      for (int j = 0; j <= 6; ++j) {
        const double k = -half + 2.0 * half * j / 6.0;
        const double K = kS0 * std::exp((kR - kQ) * T) * std::exp(k);
        const double sig = smile(k);
        const Result<AmericanGreeks> ref = american_greeks_al(
            S, K, T, sig, kR, kQ, b.spec.side, std::nullopt, true, false, false);
        ASSERT_TRUE(ref.has_value());
        const RecordGreeks g = b.rec.greeks(S, K, sig);
        EXPECT_TRUE(g.in_box);
        wd = std::max(wd, std::abs(g.delta - ref->delta));
        wg = std::max(wg, std::abs(g.gamma - ref->gamma));
        wv = std::max(wv, std::abs(g.vega - ref->vega));
        wt = std::max(wt, std::abs(g.theta - ref->theta));
        // The reference is itself a finite difference (hS = 1e-3 S, hv = 1e-3), so
        // these bound the pair, not the record alone. Measured worst 2026-10-16:
        // delta 3.3e-5, gamma 7.5e-6, vega 7.7e-4, theta 2.3e-3 per year.
        EXPECT_NEAR(g.delta, ref->delta, 5.0e-4) << "T=" << T << " K=" << K;
        EXPECT_NEAR(g.gamma, ref->gamma, 1.0e-4) << "T=" << T << " K=" << K;
        EXPECT_NEAR(g.vega, ref->vega, 1.0e-2) << "T=" << T << " K=" << K;
        EXPECT_NEAR(g.theta, ref->theta, 2.0e-2) << "T=" << T << " K=" << K;
      }
    }
  }
  std::printf("[record] worst greek error: delta %.3e gamma %.3e vega %.3e theta %.3e\n", wd, wg,
              wv, wt);
}

// ── The exercise edge ────────────────────────────────────────────────────────

TEST(ExpiryRecord, DeepInTheMoneyPutPastTheEdgeIsIntrinsicWithUnitDelta) {
  const Built *put_long = nullptr;
  for (const Built &b : records()) {
    if (b.spec.side == Side::Put && b.spec.expiry == 1.5) {
      put_long = &b;
    }
  }
  ASSERT_NE(put_long, nullptr);
  ASSERT_TRUE(put_long->rec.has_exercise_edge());
  const double sig = 0.25;
  const double edge = put_long->rec.exercise_edge(sig);
  ASSERT_LT(edge, put_long->spec.k_max);
  const double k = 0.5 * (edge + put_long->spec.k_max);
  const double F = kS0 * put_long->rec.forward_factor();
  const double K = F * std::exp(k);
  const RecordGreeks g = put_long->rec.greeks(kS0, K, sig);
  EXPECT_TRUE(g.exercised);
  EXPECT_DOUBLE_EQ(g.price, K - kS0);
  EXPECT_EQ(g.delta, -1.0);
  EXPECT_EQ(g.gamma, 0.0);
  EXPECT_EQ(g.theta, 0.0);
  const Result<double> cold = andersen_lake(kS0, K, 1.5, sig, kR, kQ, Side::Put, std::nullopt);
  ASSERT_TRUE(cold.has_value());
  EXPECT_NEAR(*cold, K - kS0, 1.0e-9 * K) << "cold solver disagrees that this put is exercised";
}

TEST(ExpiryRecord, PriceIsContinuousAcrossTheExerciseEdge) {
  for (const Built &b : records()) {
    if (!b.rec.has_exercise_edge()) {
      continue;
    }
    const double sig = 0.5 * (b.spec.sigma_min + b.spec.sigma_max);
    const double edge = b.rec.exercise_edge(sig);
    if (!(edge > b.spec.k_min && edge < b.spec.k_max)) {
      continue;
    }
    const double F = kS0 * b.rec.forward_factor();
    const double in = b.rec.price(kS0, F * std::exp(edge - 1.0e-7), sig);
    const double out = b.rec.price(kS0, F * std::exp(edge + 1.0e-7), sig);
    EXPECT_NEAR(in, out, 1.0e-3) << "T=" << b.spec.expiry;
  }
}

// ── Box handling, regimes, refusals ──────────────────────────────────────────

TEST(ExpiryRecord, BoxCornersAreInsideAndOutsideIsFlaggedNotRefused) {
  const Built &b = records().front();
  EXPECT_TRUE(b.rec.contains(b.spec.k_min, b.spec.sigma_min));
  EXPECT_TRUE(b.rec.contains(b.spec.k_max, b.spec.sigma_max));
  EXPECT_FALSE(b.rec.contains(b.spec.k_max + 1.0e-9, b.spec.sigma_max));
  EXPECT_FALSE(b.rec.contains(b.spec.k_min, b.spec.sigma_min * 0.999));
  const double F = kS0 * b.rec.forward_factor();
  const RecordGreeks far = b.rec.greeks(kS0, F * std::exp(b.spec.k_min - 0.5), 0.25);
  EXPECT_FALSE(far.in_box);
  EXPECT_TRUE(std::isfinite(far.price));
}

TEST(ExpiryRecord, EuropeanRegimeIsExactlyBlack76) {
  ExpiryRecordSpec s = spec_for(0.25, Side::Call);
  s.carry = ExpiryCarry{kR, 0.0, {}}; // call with q = 0: never early-exercised
  const Result<ExpiryRecord> r = ExpiryRecord::build(s);
  ASSERT_TRUE(r.has_value());
  EXPECT_FALSE(r->american());
  EXPECT_EQ(r->certified_max_abs_error_fwd(), 0.0);
  const double F = kS0 * std::exp(kR * 0.25);
  for (const double K : {90.0, 100.0, 112.0}) {
    EXPECT_TRUE(bits_equal(r->price(kS0, K, 0.3),
                           black76_price(F, K, 0.25, 0.3, std::exp(-kR * 0.25), Side::Call)));
  }
}

TEST(ExpiryRecord, DatedDividendsAreRefusedUntilSupported) {
  ExpiryRecordSpec s = spec_for(0.25, Side::Put);
  const std::array<CashDividend, 1> divs{CashDividend{0.1, 0.5}};
  s.carry.dividends = divs;
  const Result<ExpiryRecord> r = ExpiryRecord::build(s);
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error().code(), ErrorCode::NotImplemented);
}

TEST(ExpiryRecord, InvalidSpecsAreRefused) {
  for (int which = 0; which < 4; ++which) {
    ExpiryRecordSpec s = spec_for(0.25, Side::Put);
    if (which == 0) s.expiry = 0.0;
    if (which == 1) s.k_max = s.k_min;
    if (which == 2) s.sigma_min = 0.0;
    if (which == 3) s.carry.rate = std::numeric_limits<double>::quiet_NaN();
    const Result<ExpiryRecord> r = ExpiryRecord::build(s);
    ASSERT_FALSE(r.has_value()) << which;
    EXPECT_EQ(r.error().code(), ErrorCode::InvalidArgument) << which;
  }
  ExpiryRecordSpec dc = spec_for(0.25, Side::Put);
  dc.carry = ExpiryCarry{-0.01, -0.03, {}}; // put q < r < 0: double continuation
  const Result<ExpiryRecord> r = ExpiryRecord::build(dc);
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error().code(), ErrorCode::NotImplemented);
}

TEST(ExpiryRecord, UnpopulatedRecordReturnsNaN) {
  const ExpiryRecord empty;
  EXPECT_TRUE(std::isnan(empty.price(100.0, 100.0, 0.2)));
  EXPECT_TRUE(std::isnan(empty.greeks(100.0, 100.0, 0.2).delta));
}

TEST(ExpiryRecordBank, CapacityIsFixedAndInsertPastItIsRefused) {
  ExpiryRecordBank bank(2);
  ASSERT_TRUE(bank.insert(records()[0].rec).has_value());
  ASSERT_TRUE(bank.insert(records()[1].rec).has_value());
  const ExpiryRecord *first = bank.at(0);
  const Result<std::size_t> third = bank.insert(records()[2].rec);
  ASSERT_FALSE(third.has_value());
  EXPECT_EQ(third.error().code(), ErrorCode::OutOfRange);
  EXPECT_EQ(bank.at(0), first) << "a refused insert must not move stored records";
  EXPECT_EQ(bank.find(records()[1].spec.expiry, records()[1].spec.side), bank.at(1));
  EXPECT_EQ(bank.find(9.9, Side::Put), nullptr);
  EXPECT_FALSE(bank.insert(ExpiryRecord{}).has_value());
}

TEST(ExpiryRecord, QueriesAllocateNothing) {
  const Built &b = records()[2];
  double sink = 0.0;
  const std::uint64_t n = atx::vol::testkit::count_allocations([&] {
    for (int i = 0; i < 50; ++i) {
      const double K = 90.0 + 0.4 * i;
      sink += b.rec.price(100.0, K, 0.27);
      double v = 0.0;
      sink += b.rec.price_and_vega(100.0, K, 0.27, &v) + v;
      const RecordGreeks g = b.rec.greeks(100.0, K, 0.27);
      sink += g.delta + g.gamma + g.vega + g.theta;
    }
  });
  EXPECT_EQ(n, 0u);
  EXPECT_TRUE(std::isfinite(sink));
}

// ── The seam forwarders reproduce the price path they were split from ────────

TEST(ExpiryRecordSeam, PremiumPlusEuropeanIsThePriceFromBoundaryBitForBit) {
  const amer::AlScheme sch = amer::scheme_from_opts(std::nullopt);
  amer::AlBoundary bnd;
  amer::AlWorkspace ws;
  ASSERT_EQ(amer::al_solve_put_boundary(100.0, 0.5, 0.3, kR, kQ, sch, bnd, ws),
            amer::AlSolveStatus::Ok);
  const double b_now = amer::al_boundary_now(bnd);
  EXPECT_GT(b_now, 0.0);
  EXPECT_LT(b_now, 100.0);
  for (const double S : {b_now * 1.02, 95.0, 100.0, 110.0}) {
    const double px = amer::al_put_price_from_boundary(bnd, ws, S, 100.0, 0.5, 0.3, kR, kQ);
    const double prem = amer::al_put_premium_from_boundary(bnd, ws, S, 0.3, kR, kQ);
    const double F = S * std::exp((kR - kQ) * 0.5);
    const double euro = black76_price(F, 100.0, 0.5, 0.3, std::exp(-kR * 0.5), Side::Put);
    EXPECT_NEAR(euro + prem, px, 1.0e-12 * 100.0) << "S=" << S;
    const Result<double> cold = andersen_lake(S, 100.0, 0.5, 0.3, kR, kQ, Side::Put, std::nullopt);
    ASSERT_TRUE(cold.has_value());
    EXPECT_TRUE(bits_equal(px, *cold)) << "S=" << S;
  }
  // Below the boundary the cold solver returns intrinsic: the edge is real.
  const Result<double> below =
      andersen_lake(b_now * 0.97, 100.0, 0.5, 0.3, kR, kQ, Side::Put, std::nullopt);
  ASSERT_TRUE(below.has_value());
  EXPECT_DOUBLE_EQ(*below, 100.0 - b_now * 0.97);
}

// ── The record tick route ────────────────────────────────────────────────────

struct SmallChain {
  std::vector<double> strike, expiry, sigma, rate, carry;
  std::vector<Side> side;
  [[nodiscard]] ChainContractsView view() const {
    return ChainContractsView{strike, expiry, sigma, rate, carry, side};
  }
};

[[nodiscard]] SmallChain make_chain() {
  SmallChain c;
  for (const double T : {0.05, 0.5}) {
    const double half = 3.0 * 0.25 * std::sqrt(T);
    for (const Side sd : {Side::Call, Side::Put}) {
      for (int j = 0; j <= 6; ++j) {
        const double k = -half + 2.0 * half * j / 6.0;
        c.strike.push_back(kS0 * std::exp((kR - kQ) * T + k));
        c.expiry.push_back(T);
        c.sigma.push_back(smile(k));
        c.rate.push_back(kR);
        c.carry.push_back(kQ);
        c.side.push_back(sd);
      }
    }
  }
  return c;
}

struct Outputs {
  std::vector<double> price, delta, gamma, vega, theta;
  std::vector<TickStatus> status;
  explicit Outputs(std::size_t n)
      : price(n), delta(n), gamma(n), vega(n), theta(n), status(n, TickStatus::Unpriceable) {}
  [[nodiscard]] ChainTickOutputs view() {
    return ChainTickOutputs{price, delta, gamma, vega, theta, status};
  }
};

TEST(ChainTickRecords, RouteEqualsPerContractRecordGreeksAndTracksColdPrices) {
  const SmallChain c = make_chain();
  const Result<ChainRecords> recs = ChainRecords::build(kS0, c.view());
  ASSERT_TRUE(recs.has_value());
  EXPECT_EQ(recs->bank().size(), 4u);
  EXPECT_LT(recs->worst_certified_error_fwd(), 1.0e-7);
  Outputs out(c.strike.size());
  for (const double S : {98.0, 100.0, 102.5}) {
    ASSERT_TRUE(refresh_chain_records(S, c.view(), *recs, out.view()).has_value());
    for (std::size_t i = 0; i < c.strike.size(); ++i) {
      const ExpiryRecord *rec = recs->bank().at(recs->record_of_contract()[i]);
      ASSERT_NE(rec, nullptr);
      const RecordGreeks g = rec->greeks(S, c.strike[i], c.sigma[i]);
      EXPECT_TRUE(bits_equal(out.price[i], g.price)) << i;
      EXPECT_TRUE(bits_equal(out.theta[i], g.theta)) << i;
      EXPECT_NE(out.status[i], TickStatus::Unpriceable) << i;
      const Result<double> cold = andersen_lake(S, c.strike[i], c.expiry[i], c.sigma[i], kR, kQ,
                                                c.side[i], std::nullopt);
      ASSERT_TRUE(cold.has_value());
      EXPECT_NEAR(out.price[i], *cold, 1.0e-5) << i;
    }
  }
}

TEST(ChainTickRecords, SpotFarOutsideTheBoxIsFlaggedFallback) {
  const SmallChain c = make_chain();
  ChainRecordPolicy pol;
  pol.spot_move = 0.01;
  pol.certify = false;
  const Result<ChainRecords> recs = ChainRecords::build(kS0, c.view(), pol);
  ASSERT_TRUE(recs.has_value());
  EXPECT_TRUE(std::isnan(recs->worst_certified_error_fwd()));
  Outputs out(c.strike.size());
  ASSERT_TRUE(refresh_chain_records(kS0 * 1.3, c.view(), *recs, out.view()).has_value());
  std::size_t fallback = 0;
  for (const TickStatus st : out.status) {
    fallback += (st == TickStatus::Fallback) ? 1u : 0u;
  }
  EXPECT_GT(fallback, 0u);
}

TEST(ChainTickRecords, ShapeErrorsAreRefusedAndTheTickAllocatesNothing) {
  const SmallChain c = make_chain();
  ChainRecordPolicy pol;
  pol.certify = false;
  const Result<ChainRecords> recs = ChainRecords::build(kS0, c.view(), pol);
  ASSERT_TRUE(recs.has_value());
  Outputs short_out(c.strike.size() - 1u);
  EXPECT_FALSE(refresh_chain_records(kS0, c.view(), *recs, short_out.view()).has_value());
  Outputs out(c.strike.size());
  const ChainContractsView v = c.view();
  const ChainTickOutputs ov = out.view();
  const std::uint64_t n = atx::vol::testkit::count_allocations(
      [&] { (void)refresh_chain_records(101.0, v, *recs, ov); });
  EXPECT_EQ(n, 0u);
}

} // namespace
