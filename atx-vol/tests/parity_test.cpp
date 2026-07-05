#include <gtest/gtest.h>

#include <cstddef>
#include <vector>

#include "atx/vol/american.hpp"
#include "atx/vol/parity.hpp"
#include "atx/vol/types.hpp"

// Coverage for the American-equity PARITY acceptance metrics. Every synthetic
// mid is generated in-test by `american_price` at a known generating vol, with a
// symmetric bid-ask = mid·(1 ± spread_frac). Because the model re-Americanizes
// the SAME pricer, a model vol equal to the generating vol reproduces each mid
// bit-for-bit, which pins the "perfect model" expectations to exact values.

namespace {

using atx::vol::AmericanMethod;
using atx::vol::chain_parity;
using atx::vol::ErrorCode;
using atx::vol::ParityInputs;
using atx::vol::ParityReport;
using atx::vol::Side;

// Shared market/pricing context for the synthetic chain.
constexpr double kSpot = 100.0;
constexpr double kRate = 0.03;
constexpr double kQeff = 0.01;
constexpr double kT = 0.5;
constexpr double kSigmaGen = 0.25;

struct SyntheticChain {
  std::vector<double> strike;
  std::vector<double> bid;
  std::vector<double> ask;
  std::vector<double> mid;
  std::vector<Side> side;
};

// Build a chain whose mids are American fair values at `sigma_gen`, bracketed by
// a symmetric fractional spread. Uses ASSERT_* so a pricer failure aborts the
// calling test cleanly (helper is void, per GoogleTest's ASSERT contract).
void build_chain(double sigma_gen, double spread_frac, SyntheticChain& out) {
  const std::vector<double> strikes{90.0, 95.0, 100.0, 105.0, 110.0};
  const std::vector<Side> sides{Side::Put, Side::Put, Side::Call, Side::Call,
                                Side::Call};
  for (std::size_t i = 0; i < strikes.size(); ++i) {
    const auto p = atx::vol::american_price(kSpot, strikes[i], kT, sigma_gen,
                                            kRate, kQeff, sides[i]);
    ASSERT_TRUE(p.has_value()) << "pricer failed building chain at strike "
                               << strikes[i];
    const double mid = *p;
    out.strike.push_back(strikes[i]);
    out.side.push_back(sides[i]);
    out.mid.push_back(mid);
    out.bid.push_back(mid * (1.0 - spread_frac));
    out.ask.push_back(mid * (1.0 + spread_frac));
  }
}

// A parallel span filled with one constant vol.
std::vector<double> flat_vol(std::size_t n, double v) {
  return std::vector<double>(n, v);
}

// ── Perfect model ──────────────────────────────────────────────────────────

TEST(Parity, PerfectModel_MatchesGeneratingVol_AllWithinZeroError) {
  SyntheticChain c;
  build_chain(kSigmaGen, /*spread_frac=*/0.02, c);
  const auto model = flat_vol(c.strike.size(), kSigmaGen);
  const auto market = flat_vol(c.strike.size(), kSigmaGen);
  const ParityInputs in{kSpot, kRate, kQeff, kT};

  const auto res = chain_parity(c.strike, c.bid, c.ask, c.mid, c.side, model,
                                market, in);
  ASSERT_TRUE(res.has_value());
  const ParityReport& r = *res;
  EXPECT_EQ(r.n, std::size_t{5});
  EXPECT_EQ(r.n_within, std::size_t{5});
  EXPECT_DOUBLE_EQ(r.frac_fv_within_bidask, 1.0);
  // Model re-prices the identical American call => fair value == mid exactly.
  EXPECT_NEAR(r.rmse_mid_price, 0.0, 1e-9);
  EXPECT_DOUBLE_EQ(r.rmse_mid_vol, 0.0);
  EXPECT_DOUBLE_EQ(r.chi2_reduced, 0.0);
}

// ── Small bias (still inside the spread) ─────────────────────────────────────

TEST(Parity, SmallVolBias_WithinSpread_NonzeroVolRmse) {
  // Wide spread so a small vol bias cannot push the fair value out of the band.
  SyntheticChain c;
  build_chain(kSigmaGen, /*spread_frac=*/0.10, c);
  const double bias = 0.005;  // 0.5 vol pts
  const auto model = flat_vol(c.strike.size(), kSigmaGen + bias);
  const auto market = flat_vol(c.strike.size(), kSigmaGen);
  const ParityInputs in{kSpot, kRate, kQeff, kT};

  const auto res = chain_parity(c.strike, c.bid, c.ask, c.mid, c.side, model,
                                market, in);
  ASSERT_TRUE(res.has_value());
  const ParityReport& r = *res;
  EXPECT_DOUBLE_EQ(r.frac_fv_within_bidask, 1.0);  // still all inside the band
  EXPECT_NEAR(r.rmse_mid_vol, bias, 1e-12);        // flat bias => rmse == bias
  EXPECT_GT(r.rmse_mid_price, 0.0);                // fair value drifts off mid
  EXPECT_GT(r.chi2_reduced, 0.0);
  EXPECT_NEAR(r.mean_edge_vol, bias, 1e-12);       // signed, all positive
}

// ── Large bias (fair values escape the band) ─────────────────────────────────

TEST(Parity, LargeVolBias_TightSpread_FairValuesEscapeBand) {
  // Tight spread + large vol bias => re-Americanized fair values leave [bid,ask].
  SyntheticChain c;
  build_chain(kSigmaGen, /*spread_frac=*/0.01, c);
  const auto model = flat_vol(c.strike.size(), kSigmaGen + 0.05);  // +5 vol pts
  const auto market = flat_vol(c.strike.size(), kSigmaGen);
  const ParityInputs in{kSpot, kRate, kQeff, kT};

  const auto res = chain_parity(c.strike, c.bid, c.ask, c.mid, c.side, model,
                                market, in);
  ASSERT_TRUE(res.has_value());
  const ParityReport& r = *res;
  EXPECT_LT(r.frac_fv_within_bidask, 1.0);
  EXPECT_LT(r.n_within, r.n);
}

// ── Error band ───────────────────────────────────────────────────────────────

TEST(Parity, IdenticalVols_AllWithinEdgeBand) {
  SyntheticChain c;
  build_chain(kSigmaGen, /*spread_frac=*/0.02, c);
  const auto model = flat_vol(c.strike.size(), kSigmaGen);
  const auto market = flat_vol(c.strike.size(), kSigmaGen);
  const ParityInputs in{kSpot, kRate, kQeff, kT};

  const auto res = chain_parity(c.strike, c.bid, c.ask, c.mid, c.side, model,
                                market, in);
  ASSERT_TRUE(res.has_value());
  const ParityReport& r = *res;
  // Zero edge sits strictly inside band_k·err_bar (err_bar >= 1e-4 floor > 0).
  EXPECT_DOUBLE_EQ(r.frac_within_edge_band, 1.0);
  EXPECT_DOUBLE_EQ(r.mean_edge_vol, 0.0);
}

// ── Convenience surface-callable overload ────────────────────────────────────

TEST(Parity, SurfaceCallableOverload_FlatVol_MatchesGeneratingVol) {
  SyntheticChain c;
  build_chain(kSigmaGen, /*spread_frac=*/0.02, c);
  const auto market = flat_vol(c.strike.size(), kSigmaGen);
  const ParityInputs in{kSpot, kRate, kQeff, kT};
  // Flat surface: every log-moneyness maps to the generating vol.
  const auto surface = [](double /*k_log*/, double /*T*/) { return kSigmaGen; };

  const auto res =
      chain_parity(c.strike, c.bid, c.ask, c.mid, c.side, surface, market, in);
  ASSERT_TRUE(res.has_value());
  const ParityReport& r = *res;
  EXPECT_EQ(r.n, std::size_t{5});
  EXPECT_DOUBLE_EQ(r.frac_fv_within_bidask, 1.0);
  EXPECT_DOUBLE_EQ(r.rmse_mid_vol, 0.0);
}

// ── Guards ───────────────────────────────────────────────────────────────────

TEST(Parity, LengthMismatch_ReturnsErr) {
  SyntheticChain c;
  build_chain(kSigmaGen, /*spread_frac=*/0.02, c);
  const auto model = flat_vol(c.strike.size(), kSigmaGen);
  const auto market = flat_vol(c.strike.size() - 1, kSigmaGen);  // one short
  const ParityInputs in{kSpot, kRate, kQeff, kT};

  const auto res = chain_parity(c.strike, c.bid, c.ask, c.mid, c.side, model,
                                market, in);
  ASSERT_FALSE(res.has_value());
  EXPECT_EQ(res.error().code(), ErrorCode::InvalidArgument);
}

TEST(Parity, EmptyChain_ReturnsErr) {
  const std::vector<double> empty{};
  const std::vector<Side> empty_side{};
  const ParityInputs in{kSpot, kRate, kQeff, kT};

  const auto res = chain_parity(empty, empty, empty, empty, empty_side, empty,
                                empty, in);
  ASSERT_FALSE(res.has_value());
  EXPECT_EQ(res.error().code(), ErrorCode::InvalidArgument);
}

}  // namespace
