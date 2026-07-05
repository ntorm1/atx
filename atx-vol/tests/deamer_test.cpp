#include <gtest/gtest.h>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "atx/vol/american.hpp"
#include "atx/vol/american_iv.hpp"
#include "atx/vol/curve.hpp"
#include "atx/vol/deamer.hpp"
#include "atx/vol/dividend.hpp"
#include "atx/vol/types.hpp"
#include "atx/vol/universe.hpp"

// De-Americanization pipeline coverage (atx/vol/deamer.hpp). The forward map is
// the SAME American pricer used everywhere else, so a synthetic chain generated
// at a known (borrow, cash-div, per-strike smile) round-trips: de_americanize_
// chain must recover the injected borrow, forward, and per-strike vols. The
// remaining tests pin the single-quote consistency with american_implied_vol,
// the OTM-side selection rule, and the dropped-quote accounting.

namespace {

using atx::vol::american_implied_vol;
using atx::vol::american_price;
using atx::vol::AmericanMethod;
using atx::vol::Chain;
using atx::vol::chain_index;
using atx::vol::DeAmOptions;
using atx::vol::DeAmResult;
using atx::vol::de_americanize_chain;
using atx::vol::DividendEvent;
using atx::vol::ErrorCode;
using atx::vol::european_equiv_iv;
using atx::vol::hybrid_forward;
using atx::vol::HybridDivParams;
using atx::vol::imply_term_borrow;
using atx::vol::otm_side;
using atx::vol::Side;

// Year-fraction → epoch-ns (365.25-day year, matching hybrid_forward).
constexpr double kYearNs = 365.25 * 86400.0 * 1.0e9;
[[nodiscard]] std::int64_t years_to_ns(double y) {
  return static_cast<std::int64_t>(y * kYearNs);
}

double value_or_fail(const atx::core::Result<double>& r) {
  EXPECT_TRUE(r.has_value()) << (r ? std::string{} : r.error().to_string());
  return r ? *r : std::nan("");
}

// Canonical scenario: spot 100, 3% rate, 1y expiry, one 1.20 cash dividend
// ex-6-months, hybrid blend 0.4, 2% proportional yield.
struct Scenario {
  double S = 100.0;
  double r = 0.03;
  double T = 1.0;
  std::int64_t now_ns = 0;
  std::int64_t expiry_ns = years_to_ns(1.0);
  std::vector<DividendEvent> divs{{years_to_ns(0.5), 1.20}};
  HybridDivParams hyb{/*prop_div_yield=*/0.02, /*blend=*/0.4};
};

// A gentle smile: base 20% vol with a mild convex wing in log-moneyness.
[[nodiscard]] double true_sigma(double k_log) noexcept {
  return 0.20 + 0.15 * k_log * k_log;
}

// Build a fully-populated Chain whose per-side mids are American prices at the
// per-strike true smile, on the forward implied by `b_true`. Bids/asks straddle
// each mid by ±1% so every leg is quotable.
[[nodiscard]] Chain make_synthetic_chain(const Scenario& sc, double b_true,
                                         const std::vector<double>& strikes) {
  const double F = hybrid_forward(sc.S, sc.r, b_true, sc.T, sc.divs, sc.expiry_ns,
                                  sc.now_ns, sc.hyb);
  const double q_eff = sc.r - std::log(F / sc.S) / sc.T;

  Chain chain;
  chain.T = sc.T;
  chain.expiry_ns = sc.expiry_ns;
  chain.strikes = strikes;
  const std::size_t two_n = 2u * strikes.size();
  chain.bids.assign(two_n, 0.0);
  chain.asks.assign(two_n, 0.0);
  chain.mids.assign(two_n, 0.0);

  for (std::size_t i = 0; i < strikes.size(); ++i) {
    const double K = strikes[i];
    const double sig = true_sigma(std::log(K / F));
    for (const Side side : {Side::Call, Side::Put}) {
      const double p = value_or_fail(
          american_price(sc.S, K, sc.T, sig, sc.r, q_eff, side, AmericanMethod::AndersenLake));
      const std::size_t idx = chain_index(static_cast<std::uint16_t>(i), side);
      chain.mids[idx] = p;
      chain.bids[idx] = p * 0.99;
      chain.asks[idx] = p * 1.01;
    }
  }
  return chain;
}

}  // namespace

// ── Chain round-trip ─────────────────────────────────────────────────────

TEST(DeAmer, RoundTripSyntheticChain_RecoversSmileAndForward) {
  const Scenario sc;
  const double b_true = 0.0175;
  const std::vector<double> strikes{80.0, 90.0, 100.0, 110.0, 120.0};
  const Chain chain = make_synthetic_chain(sc, b_true, strikes);

  const double f_true = hybrid_forward(sc.S, sc.r, b_true, sc.T, sc.divs,
                                       sc.expiry_ns, sc.now_ns, sc.hyb);

  DeAmOptions opts;
  opts.hyb = sc.hyb;
  opts.imply_borrow = true;
  opts.n_atm = 3;

  const auto res =
      de_americanize_chain(chain, sc.S, sc.r, sc.divs, sc.now_ns, opts);
  ASSERT_TRUE(res.has_value()) << (res ? std::string{} : res.error().to_string());
  const DeAmResult& out = *res;

  EXPECT_NEAR(out.borrow, b_true, 1e-4);
  EXPECT_NEAR(out.forward, f_true, 1e-2);
  EXPECT_EQ(out.n_used, strikes.size());
  EXPECT_EQ(out.n_dropped, 0u);
  ASSERT_EQ(out.iv.size(), strikes.size());
  ASSERT_EQ(out.k_log.size(), strikes.size());
  ASSERT_EQ(out.weight.size(), strikes.size());

  for (std::size_t i = 0; i < strikes.size(); ++i) {
    // Log-moneyness is definitionally consistent with the returned forward.
    EXPECT_NEAR(out.k_log[i], std::log(strikes[i] / out.forward), 1e-12);
    // Recovered European-equivalent vol matches the injected smile.
    EXPECT_NEAR(out.iv[i], true_sigma(std::log(strikes[i] / f_true)), 1e-4)
        << "K=" << strikes[i];
    EXPECT_GT(out.weight[i], 0.0);
  }
}

// ── Per-term borrow ──────────────────────────────────────────────────────

TEST(DeAmer, ImplyTermBorrow_RecoversInjectedBorrow) {
  const Scenario sc;
  const double b_true = 0.0225;
  const double K = 100.0;  // ATM

  const double F = hybrid_forward(sc.S, sc.r, b_true, sc.T, sc.divs, sc.expiry_ns,
                                  sc.now_ns, sc.hyb);
  const double q_eff = sc.r - std::log(F / sc.S) / sc.T;
  const double sig = 0.25;
  const double call = value_or_fail(american_price(sc.S, K, sc.T, sig, sc.r,
                                                   q_eff, Side::Call, AmericanMethod::AndersenLake));
  const double put = value_or_fail(american_price(sc.S, K, sc.T, sig, sc.r,
                                                  q_eff, Side::Put, AmericanMethod::AndersenLake));

  const auto res = imply_term_borrow(call, put, sc.S, K, sc.T, sc.r, sc.divs,
                                     sc.expiry_ns, sc.now_ns, sc.hyb);
  ASSERT_TRUE(res.has_value()) << (res ? std::string{} : res.error().to_string());
  EXPECT_NEAR(res->borrow, b_true, 1e-4);
  EXPECT_NEAR(res->forward, F, 1e-2);
  EXPECT_LT(res->rmse_pcp, 1e-6);
}

// ── Single-quote consistency ─────────────────────────────────────────────

TEST(DeAmer, EuropeanEquivIv_EqualsAmericanImpliedVol) {
  const double S = 100.0, K = 105.0, T = 0.75, r = 0.04, q_eff = 0.015;
  const double sig = 0.28;
  const double p = value_or_fail(
      american_price(S, K, T, sig, r, q_eff, Side::Call, AmericanMethod::AndersenLake));

  const auto a = american_implied_vol(p, S, K, T, r, q_eff, Side::Call);
  const auto b = european_equiv_iv(p, S, K, T, r, q_eff, Side::Call);
  ASSERT_TRUE(a.has_value());
  ASSERT_TRUE(b.has_value());
  EXPECT_DOUBLE_EQ(*a, *b);
  EXPECT_NEAR(*b, sig, 1e-5);
}

// ── OTM-side selection rule ──────────────────────────────────────────────

TEST(DeAmer, OtmSide_PicksCallAboveForwardPutBelow) {
  EXPECT_EQ(otm_side(0.30), Side::Call);   // k > 0: OTM call
  EXPECT_EQ(otm_side(0.00), Side::Call);   // ATM: Call by convention
  EXPECT_EQ(otm_side(-0.30), Side::Put);   // k < 0: OTM put
}

// A deep-ITM strike must be inverted through its OTM opposite leg. Poison the
// ITM leg (crossed quote) so the strike survives ONLY if the OTM leg is chosen.
TEST(DeAmer, DeepItmStrike_InvertsViaOtmOppositeSide) {
  const Scenario sc;
  const double b_true = 0.0;
  const std::vector<double> strikes{60.0, 100.0, 140.0};
  Chain chain = make_synthetic_chain(sc, b_true, strikes);

  const double F = hybrid_forward(sc.S, sc.r, b_true, sc.T, sc.divs, sc.expiry_ns,
                                  sc.now_ns, sc.hyb);

  // K=60 is a deep-ITM CALL (k<0 → OTM side is the PUT): cross the call leg.
  const std::size_t c60 = chain_index(0u, Side::Call);
  chain.bids[c60] = 50.0;
  chain.asks[c60] = 1.0;  // crossed → invalid
  // K=140 is a deep-ITM PUT (k>0 → OTM side is the CALL): cross the put leg.
  const std::size_t p140 = chain_index(2u, Side::Put);
  chain.bids[p140] = 50.0;
  chain.asks[p140] = 1.0;  // crossed → invalid

  DeAmOptions opts;
  opts.hyb = sc.hyb;
  opts.imply_borrow = false;  // fix borrow so the poisoned wings don't feed it
  opts.borrow_fixed = b_true;

  const auto res =
      de_americanize_chain(chain, sc.S, sc.r, sc.divs, sc.now_ns, opts);
  ASSERT_TRUE(res.has_value()) << (res ? std::string{} : res.error().to_string());
  const DeAmResult& out = *res;

  // All three survive because each was inverted through its clean OTM leg.
  EXPECT_EQ(out.n_used, 3u);
  EXPECT_EQ(out.n_dropped, 0u);
  for (std::size_t i = 0; i < out.iv.size(); ++i) {
    EXPECT_NEAR(out.iv[i], true_sigma(std::log(strikes[i] / F)), 1e-4)
        << "K=" << strikes[i];
  }
}

// ── Dropped-quote accounting ─────────────────────────────────────────────

TEST(DeAmer, CrossedAndZeroQuotes_CountedNotInverted) {
  const Scenario sc;
  const double b_true = 0.01;
  const std::vector<double> strikes{80.0, 90.0, 100.0, 110.0, 120.0};
  Chain chain = make_synthetic_chain(sc, b_true, strikes);

  // Poison the OTM leg of two strikes so they must drop.
  // K=120 (k>0 → OTM call): zero the call bid.
  const std::size_t c120 = chain_index(4u, Side::Call);
  chain.bids[c120] = 0.0;
  // K=80 (k<0 → OTM put): cross the put quote.
  const std::size_t p80 = chain_index(0u, Side::Put);
  chain.bids[p80] = 30.0;
  chain.asks[p80] = 1.0;

  DeAmOptions opts;
  opts.hyb = sc.hyb;
  opts.imply_borrow = false;  // borrow not under test here
  opts.borrow_fixed = b_true;

  const auto res =
      de_americanize_chain(chain, sc.S, sc.r, sc.divs, sc.now_ns, opts);
  ASSERT_TRUE(res.has_value()) << (res ? std::string{} : res.error().to_string());
  const DeAmResult& out = *res;

  EXPECT_EQ(out.n_dropped, 2u);
  EXPECT_EQ(out.n_used, strikes.size() - 2u);
  EXPECT_EQ(out.iv.size(), out.n_used);
  EXPECT_EQ(out.k_log.size(), out.n_used);
  // Every surviving vol is a sane recovery (no NaN leaked through).
  for (double v : out.iv) {
    EXPECT_TRUE(std::isfinite(v));
    EXPECT_GT(v, 0.0);
  }
}

// ── Input guards ─────────────────────────────────────────────────────────

TEST(DeAmer, EmptyChain_ReturnsInvalidArgument) {
  Chain chain;
  chain.T = 1.0;
  chain.expiry_ns = years_to_ns(1.0);
  DeAmOptions opts;
  const auto res = de_americanize_chain(chain, 100.0, 0.03, {}, 0, opts);
  ASSERT_FALSE(res.has_value());
  EXPECT_EQ(res.error().code(), ErrorCode::InvalidArgument);
}
