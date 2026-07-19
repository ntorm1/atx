#include <gtest/gtest.h>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "atx/vol/american.hpp"
#include "atx/vol/american_iv.hpp"
#include "atx/vol/counters.hpp" // counters::ledger — always-on AL boundary-solve gate
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

using atx::vol::al_default_opts;
using atx::vol::al_fast_opts;
using atx::vol::american_implied_vol;
using atx::vol::american_price;
using atx::vol::AmericanMethod;
using atx::vol::audit_european_equiv_iv;
using atx::vol::Chain;
using atx::vol::chain_index;
using atx::vol::de_americanize_chain;
using atx::vol::DeAmOptions;
using atx::vol::DeAmResult;
using atx::vol::DividendEvent;
using atx::vol::ErrorCode;
using atx::vol::european_equiv_iv;
using atx::vol::hybrid_forward;
using atx::vol::HybridDivParams;
using atx::vol::imply_term_borrow;
using atx::vol::otm_side;
using atx::vol::resolve_chain_forward;
using atx::vol::Side;

// Year-fraction → epoch-ns (365.25-day year, matching hybrid_forward).
constexpr double kYearNs = 365.25 * 86400.0 * 1.0e9;
[[nodiscard]] std::int64_t years_to_ns(double y) { return static_cast<std::int64_t>(y * kYearNs); }

double value_or_fail(const atx::core::Result<double> &r) {
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
[[nodiscard]] double true_sigma(double k_log) noexcept { return 0.20 + 0.15 * k_log * k_log; }

// Build a fully-populated Chain whose per-side mids are American prices at the
// per-strike true smile, on the forward implied by `b_true`. Bids/asks straddle
// each mid by ±1% so every leg is quotable.
[[nodiscard]] Chain make_synthetic_chain(const Scenario &sc, double b_true,
                                         const std::vector<double> &strikes) {
  const double F =
      hybrid_forward(sc.S, sc.r, b_true, sc.T, sc.divs, sc.expiry_ns, sc.now_ns, sc.hyb);
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

} // namespace

// ── Chain round-trip ─────────────────────────────────────────────────────

TEST(DeAmer, RoundTripSyntheticChain_RecoversSmileAndForward) {
  const Scenario sc;
  const double b_true = 0.0175;
  const std::vector<double> strikes{80.0, 90.0, 100.0, 110.0, 120.0};
  const Chain chain = make_synthetic_chain(sc, b_true, strikes);

  const double f_true =
      hybrid_forward(sc.S, sc.r, b_true, sc.T, sc.divs, sc.expiry_ns, sc.now_ns, sc.hyb);

  DeAmOptions opts;
  opts.hyb = sc.hyb;
  opts.imply_borrow = true;
  opts.n_atm = 3;

  const auto res = de_americanize_chain(chain, sc.S, sc.r, sc.divs, sc.now_ns, opts);
  ASSERT_TRUE(res.has_value()) << (res ? std::string{} : res.error().to_string());
  const DeAmResult &out = *res;

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
    EXPECT_NEAR(out.iv[i], true_sigma(std::log(strikes[i] / f_true)), 1e-4) << "K=" << strikes[i];
    EXPECT_GT(out.weight[i], 0.0);
  }
}

TEST(DeAmer, DefaultCarryBudgetUsesFiveFastAndersenLakePairs) {
  const DeAmOptions opts;
  const auto fast = al_fast_opts();
  ASSERT_TRUE(opts.carry_al_opts.has_value());
  EXPECT_EQ(opts.max_borrow_pairs, std::size_t{5});
  EXPECT_EQ(opts.carry_al_opts->n_collocation, fast.n_collocation);
  EXPECT_EQ(opts.carry_al_opts->n_quadrature, fast.n_quadrature);
  EXPECT_EQ(opts.carry_al_opts->max_newton_iter, fast.max_newton_iter);
  EXPECT_DOUBLE_EQ(opts.carry_al_opts->tol, fast.tol);
}

// ── Per-term borrow ──────────────────────────────────────────────────────

TEST(DeAmer, ImplyTermBorrow_RecoversInjectedBorrow) {
  const Scenario sc;
  const double b_true = 0.0225;
  const double K = 100.0; // ATM

  const double F =
      hybrid_forward(sc.S, sc.r, b_true, sc.T, sc.divs, sc.expiry_ns, sc.now_ns, sc.hyb);
  const double q_eff = sc.r - std::log(F / sc.S) / sc.T;
  const double sig = 0.25;
  const double call = value_or_fail(
      american_price(sc.S, K, sc.T, sig, sc.r, q_eff, Side::Call, AmericanMethod::AndersenLake));
  const double put = value_or_fail(
      american_price(sc.S, K, sc.T, sig, sc.r, q_eff, Side::Put, AmericanMethod::AndersenLake));

  const auto res =
      imply_term_borrow(call, put, sc.S, K, sc.T, sc.r, sc.divs, sc.expiry_ns, sc.now_ns, sc.hyb);
  ASSERT_TRUE(res.has_value()) << (res ? std::string{} : res.error().to_string());
  EXPECT_NEAR(res->borrow, b_true, 1e-4);
  EXPECT_NEAR(res->forward, F, 1e-2);
  EXPECT_LT(res->rmse_pcp, 1e-4);
}

// R-06: deamer.cpp codifies the nested tolerance ladder
// (kPcpTol < kBorrowFpTol < kInnerIvTol) as a compile-time static_assert. This
// pins its ECONOMIC consequence: because the borrow fixed point (kBorrowFpTol =
// 1e-8) resolves far below the inner-IV economic bound (kInnerIvTol = 1e-4), the
// reported PCP residual stays inside its 1e-4 acceptance contract across borrow
// regimes and strikes. An edit that inverted the ladder (letting inner-IV noise
// masquerade as an unconverged map) would surface here as an rmse_pcp blow-up.
TEST(DeAmer, ToleranceLadderKeepsPcpResidualWithinEconomicBound) {
  const Scenario sc;
  constexpr double kEconomicBound = 1.0e-4; // == kInnerIvTol (deamer.cpp)
  for (const double b_true : {0.005, 0.02, 0.035}) {
    for (const double K : {90.0, 100.0, 110.0}) {
      const double F =
          hybrid_forward(sc.S, sc.r, b_true, sc.T, sc.divs, sc.expiry_ns, sc.now_ns, sc.hyb);
      const double q_eff = sc.r - std::log(F / sc.S) / sc.T;
      const double sig = 0.25;
      const double call = value_or_fail(american_price(sc.S, K, sc.T, sig, sc.r, q_eff, Side::Call,
                                                       AmericanMethod::AndersenLake));
      const double put = value_or_fail(american_price(sc.S, K, sc.T, sig, sc.r, q_eff, Side::Put,
                                                      AmericanMethod::AndersenLake));
      const auto res = imply_term_borrow(call, put, sc.S, K, sc.T, sc.r, sc.divs, sc.expiry_ns,
                                         sc.now_ns, sc.hyb);
      ASSERT_TRUE(res.has_value())
          << "b_true=" << b_true << " K=" << K << ": "
          << (res ? std::string{} : res.error().to_string());
      EXPECT_LT(res->rmse_pcp, kEconomicBound) << "b_true=" << b_true << " K=" << K;
      EXPECT_NEAR(res->borrow, b_true, kEconomicBound) << "b_true=" << b_true << " K=" << K;
    }
  }
}

TEST(DeAmer, CarrySolveUsesItsOwnAndersenLakePreset) {
  const Scenario sc;
  const double b_true = 0.025;
  Chain chain = make_synthetic_chain(sc, b_true, {100.0});

  DeAmOptions opts;
  opts.hyb = sc.hyb;
  opts.n_atm = 1;
  opts.max_borrow_pairs = 1;
  opts.al_opts = al_default_opts();
  opts.carry_al_opts = al_fast_opts();
  const auto resolved = resolve_chain_forward(chain, sc.S, sc.r, sc.divs, sc.now_ns, opts);
  ASSERT_TRUE(resolved.has_value()) << (resolved ? std::string{} : resolved.error().to_string());

  const auto expected = imply_term_borrow(
      chain.mids[chain_index(0u, Side::Call)], chain.mids[chain_index(0u, Side::Put)], sc.S,
      chain.strikes[0], chain.T, sc.r, sc.divs, chain.expiry_ns, sc.now_ns, sc.hyb,
      AmericanMethod::AndersenLake, opts.carry_al_opts);
  ASSERT_TRUE(expected.has_value()) << (expected ? std::string{} : expected.error().to_string());
  EXPECT_NEAR(resolved->borrow, expected->borrow, 1.0e-12);
  EXPECT_NEAR(resolved->forward, expected->forward, 1.0e-10);
}

// R-27: default-constructed DeAmOptions moved every library caller to the
// fast-AL / 5-pair carry solve, and carry is unaudited by design. This pins that
// even in a HIGH-DIVIDEND regime (F/S ~= 0.90, driven by a large mid-life cash
// dividend so the ATM co-terminal pairs are deep-ITM puts / OTM calls) the
// implied borrow stays within the economic 1e-4 bound under BOTH the fast carry
// preset (the default) and the accurate one — so the fast default is not silently
// trading carry accuracy away where the dividend load is heaviest.
TEST(DeAmer, HighDividendCarryHoldsFastVsAccurateWithinEconomicBound) {
  Scenario sc;
  sc.divs = {{years_to_ns(0.5), 17.0}}; // ~17% mid-life cash div -> F/S ~= 0.90
  const double b_true = 0.02;
  const std::vector<double> strikes{85.0, 90.0, 95.0, 100.0, 105.0};
  const Chain chain = make_synthetic_chain(sc, b_true, strikes);

  const double f_true =
      hybrid_forward(sc.S, sc.r, b_true, sc.T, sc.divs, sc.expiry_ns, sc.now_ns, sc.hyb);
  ASSERT_LT(f_true / sc.S, 0.95) << "fixture must exercise a high-dividend (F/S << 1) regime";

  const auto resolve_with = [&](const std::optional<atx::vol::AlOpts> &carry_preset) {
    DeAmOptions opts;
    opts.hyb = sc.hyb;
    opts.n_atm = 3;
    opts.max_borrow_pairs = strikes.size();
    opts.carry_al_opts = carry_preset;
    return resolve_chain_forward(chain, sc.S, sc.r, sc.divs, sc.now_ns, opts);
  };

  const auto fast = resolve_with(al_fast_opts());      // the default carry preset
  const auto accurate = resolve_with(std::nullopt);    // cold accurate Andersen-Lake
  ASSERT_TRUE(fast.has_value()) << (fast ? std::string{} : fast.error().to_string());
  ASSERT_TRUE(accurate.has_value()) << (accurate ? std::string{} : accurate.error().to_string());

  EXPECT_NEAR(fast->borrow, b_true, 1e-4);
  EXPECT_NEAR(accurate->borrow, b_true, 1e-4);
  EXPECT_NEAR(fast->forward, f_true, 1e-4 * sc.S);
  EXPECT_NEAR(accurate->forward, f_true, 1e-4 * sc.S);
  // Fast and accurate presets must agree with each other, not just with truth.
  EXPECT_NEAR(fast->borrow, accurate->borrow, 1e-4);
}

// P2 (perf F2): the robust carry solve's cross-pair warm start + skip-redundant-
// final now ship ON by default (DeAmOptions::warm_start_carry). The proof is the
// DETERMINISTIC solve ledger (G-COUNTER), NOT wall-clock: warm start must cut the
// AL boundary solves the multi-pair carry solve spends, while leaving the
// converged borrow/forward within the fixed-point tolerance (< kBorrowFpTol=1e-8).
// The ledger is always-on (compiled into rel/rel-avx2), so this gate runs on the
// shipping binary with no special counters build.
TEST(DeAmer, WarmStartCarryCutsBoundarySolvesConvergedRootUnchanged) {
  namespace led = atx::vol::counters::ledger;

  const Scenario sc;
  const double b_true = 0.021;
  // A full near-ATM ladder so the ascending-|K-S| chain has neighbours to warm
  // from (single-pair solves share the first pair's cold seed and can't chain).
  const std::vector<double> strikes{92.0, 94.0,  96.0,  98.0, 100.0,
                                    102.0, 104.0, 106.0, 108.0};
  const Chain chain = make_synthetic_chain(sc, b_true, strikes);

  const auto resolve_with = [&](bool warm) {
    DeAmOptions opts;
    opts.hyb = sc.hyb;
    opts.n_atm = strikes.size();
    opts.max_borrow_pairs = strikes.size();
    opts.warm_start_carry = warm;
    return resolve_chain_forward(chain, sc.S, sc.r, sc.divs, sc.now_ns, opts);
  };

  // The P2 flip: a freshly constructed DeAmOptions carries warm start ON.
  EXPECT_TRUE(DeAmOptions{}.warm_start_carry);

  led::reset();
  const auto off = resolve_with(false);
  const std::uint64_t solves_off = led::snapshot().get(led::Solve::AlBoundarySolves);

  led::reset();
  const auto on = resolve_with(true);
  const std::uint64_t solves_on = led::snapshot().get(led::Solve::AlBoundarySolves);

  ASSERT_TRUE(off.has_value()) << (off ? std::string{} : off.error().to_string());
  ASSERT_TRUE(on.has_value()) << (on ? std::string{} : on.error().to_string());

  // Converged root unchanged: borrow (load-bearing) moves by < the fixed-point
  // tolerance the review bounds it to; the forward is hybrid_forward(borrow) so it
  // tracks that shift scaled by ~F·T (∂F/∂borrow).
  EXPECT_NEAR(on->borrow, off->borrow, 1e-8);
  EXPECT_NEAR(on->forward, off->forward, 1e-8 * sc.S * sc.T * 2.0);

  // The gate: warm start spends strictly fewer AL boundary solves per slice.
  EXPECT_GT(solves_off, 0u);
  EXPECT_LT(solves_on, solves_off) << "warm start must cut per-slice boundary solves";

  std::printf("[deamer-carry] AL boundary solves/slice: OFF=%llu ON=%llu (ratio %.3f); "
              "borrow OFF=%.12f ON=%.12f |d|=%.2e; forward |d|=%.2e\n",
              static_cast<unsigned long long>(solves_off),
              static_cast<unsigned long long>(solves_on),
              solves_off ? static_cast<double>(solves_on) / static_cast<double>(solves_off) : 0.0,
              off->borrow, on->borrow, std::fabs(on->borrow - off->borrow),
              std::fabs(on->forward - off->forward));
}

TEST(DeAmer, RobustCarryRejectsOneBadAtmPairAndReportsSensitivity) {
  const Scenario sc;
  const double b_true = 0.031;
  const std::vector<double> strikes{94.0, 96.0, 98.0, 100.0, 102.0, 104.0, 106.0};
  Chain chain = make_synthetic_chain(sc, b_true, strikes);

  // Keep a valid/tight quote but dislocate one pair's call mid. A simple mean
  // moves materially; the robust strip should identify and discard this pair.
  const std::size_t bad = chain_index(3u, Side::Call);
  chain.mids[bad] += 1.00;
  chain.bids[bad] = chain.mids[bad] - 0.01;
  chain.asks[bad] = chain.mids[bad] + 0.01;

  DeAmOptions opts;
  opts.hyb = sc.hyb;
  opts.n_atm = strikes.size();
  opts.max_borrow_pairs = strikes.size();
  opts.require_carry_confidence = true;

  const auto result = resolve_chain_forward(chain, sc.S, sc.r, sc.divs, sc.now_ns, opts);
  ASSERT_TRUE(result.has_value()) << (result ? std::string{} : result.error().to_string());
  const double expected =
      hybrid_forward(sc.S, sc.r, b_true, sc.T, sc.divs, sc.expiry_ns, sc.now_ns, sc.hyb);
  EXPECT_NEAR(result->forward, expected, 0.05);
  EXPECT_TRUE(result->carry.confident);
  EXPECT_GE(result->carry.n_solved, 6u);
  EXPECT_TRUE(result->carry.n_solved < result->carry.n_attempted ||
              result->carry.n_retained < result->carry.n_solved);
  EXPECT_LT(result->carry.max_leave_one_out_shift, opts.max_carry_leave_one_out);
  EXPECT_GE(result->carry.confidence_half_width, 0.0);
}

TEST(DeAmer, CarryConfidenceGateRejectsSinglePair) {
  const Scenario sc;
  Chain chain = make_synthetic_chain(sc, 0.02, {100.0});
  DeAmOptions opts;
  opts.hyb = sc.hyb;
  opts.n_atm = 1;
  opts.max_borrow_pairs = 1;
  opts.min_confident_borrow_pairs = 3;
  opts.require_carry_confidence = true;

  const auto result = resolve_chain_forward(chain, sc.S, sc.r, sc.divs, sc.now_ns, opts);
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().code(), ErrorCode::Unavailable);
}

TEST(DeAmer, RobustCarrySupportsHardToBorrowStrip) {
  const Scenario sc;
  const double b_true = 0.15;
  Chain chain = make_synthetic_chain(sc, b_true, {94.0, 96.0, 98.0, 100.0, 102.0, 104.0});
  DeAmOptions opts;
  opts.hyb = sc.hyb;
  opts.n_atm = 6;
  opts.max_borrow_pairs = 6;
  opts.require_carry_confidence = true;

  const auto result = resolve_chain_forward(chain, sc.S, sc.r, sc.divs, sc.now_ns, opts);
  ASSERT_TRUE(result.has_value()) << (result ? std::string{} : result.error().to_string());
  EXPECT_NEAR(result->borrow, b_true, 1e-4);
  EXPECT_TRUE(result->carry.confident);
  EXPECT_GE(result->carry.effective_pair_count, 3.0);
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

TEST(DeAmer, AccurateRepricingAuditCertifiesKnownSigma) {
  const double S = 100.0, K = 95.0, T = 0.5, r = 0.05, q = 0.01;
  const double sigma = 0.24;
  const double mid =
      value_or_fail(american_price(S, K, T, sigma, r, q, Side::Put, AmericanMethod::AndersenLake));
  const auto audit = audit_european_equiv_iv(mid, 0.04, sigma, S, K, T, r, q, Side::Put, 0.25);
  ASSERT_TRUE(audit.has_value()) << (audit ? std::string{} : audit.error().to_string());
  EXPECT_TRUE(audit->passed);
  EXPECT_LT(audit->residual_half_spreads, 1.0e-6);
}

// ── OTM-side selection rule ──────────────────────────────────────────────

TEST(DeAmer, OtmSide_PicksCallAboveForwardPutBelow) {
  EXPECT_EQ(otm_side(0.30), Side::Call); // k > 0: OTM call
  EXPECT_EQ(otm_side(0.00), Side::Call); // ATM: Call by convention
  EXPECT_EQ(otm_side(-0.30), Side::Put); // k < 0: OTM put
}

// A deep-ITM strike must be inverted through its OTM opposite leg. Poison the
// ITM leg (crossed quote) so the strike survives ONLY if the OTM leg is chosen.
TEST(DeAmer, DeepItmStrike_InvertsViaOtmOppositeSide) {
  const Scenario sc;
  const double b_true = 0.0;
  const std::vector<double> strikes{60.0, 100.0, 140.0};
  Chain chain = make_synthetic_chain(sc, b_true, strikes);

  const double F =
      hybrid_forward(sc.S, sc.r, b_true, sc.T, sc.divs, sc.expiry_ns, sc.now_ns, sc.hyb);

  // K=60 is a deep-ITM CALL (k<0 → OTM side is the PUT): cross the call leg.
  const std::size_t c60 = chain_index(0u, Side::Call);
  chain.bids[c60] = 50.0;
  chain.asks[c60] = 1.0; // crossed → invalid
  // K=140 is a deep-ITM PUT (k>0 → OTM side is the CALL): cross the put leg.
  const std::size_t p140 = chain_index(2u, Side::Put);
  chain.bids[p140] = 50.0;
  chain.asks[p140] = 1.0; // crossed → invalid

  DeAmOptions opts;
  opts.hyb = sc.hyb;
  opts.imply_borrow = false; // fix borrow so the poisoned wings don't feed it
  opts.borrow_fixed = b_true;

  const auto res = de_americanize_chain(chain, sc.S, sc.r, sc.divs, sc.now_ns, opts);
  ASSERT_TRUE(res.has_value()) << (res ? std::string{} : res.error().to_string());
  const DeAmResult &out = *res;

  // All three survive because each was inverted through its clean OTM leg.
  EXPECT_EQ(out.n_used, 3u);
  EXPECT_EQ(out.n_dropped, 0u);
  for (std::size_t i = 0; i < out.iv.size(); ++i) {
    EXPECT_NEAR(out.iv[i], true_sigma(std::log(strikes[i] / F)), 1e-4) << "K=" << strikes[i];
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
  opts.imply_borrow = false; // borrow not under test here
  opts.borrow_fixed = b_true;

  const auto res = de_americanize_chain(chain, sc.S, sc.r, sc.divs, sc.now_ns, opts);
  ASSERT_TRUE(res.has_value()) << (res ? std::string{} : res.error().to_string());
  const DeAmResult &out = *res;

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
