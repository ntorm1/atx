#include <gtest/gtest.h>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "atx/vol/api/pricing/american.hpp"
#include "atx/vol/api/pricing/american_iv.hpp"
#include "atx/vol/api/pricing/black76.hpp"
#include "fitting/counters.hpp" // counters::ledger — always-on AL boundary-solve gate
#include "atx/vol/api/fitting/deamer.hpp"
#include "atx/vol/api/pricing/dividend.hpp"
#include "atx/vol/api/pricing/rates_curve.hpp"
#include "atx/vol/api/core/types.hpp"
#include "atx/vol/api/marketdata/universe.hpp"

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
using atx::vol::audit_european_equiv_iv_batch;
using atx::vol::carry_moneyness_bounded;
using atx::vol::Chain;
using atx::vol::chain_index;
using atx::vol::de_americanize_chain;
using atx::vol::deam_inversion_well_posed;
using atx::vol::DeAmOptions;
using atx::vol::DeAmResult;
using atx::vol::DividendEvent;
using atx::vol::ErrorCode;
using atx::vol::european_equiv_iv;
using atx::vol::ExerciseStyle;
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
      const double put = value_or_fail(
          american_price(sc.S, K, sc.T, sig, sc.r, q_eff, Side::Put, AmericanMethod::AndersenLake));
      const auto res = imply_term_borrow(call, put, sc.S, K, sc.T, sc.r, sc.divs, sc.expiry_ns,
                                         sc.now_ns, sc.hyb);
      ASSERT_TRUE(res.has_value()) << "b_true=" << b_true << " K=" << K << ": "
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

TEST(DeAmer, EuropeanChainForwardUsesRawParityWithoutAmericanBoundarySolves) {
  namespace led = atx::vol::counters::ledger;

  constexpr double spot = 100.0;
  constexpr double rate = 0.02;
  constexpr double maturity = 0.25;
  constexpr double forward = 101.25;
  constexpr double sigma = 0.22;
  const double df = std::exp(-rate * maturity);

  Chain chain;
  chain.exercise_style = ExerciseStyle::European;
  chain.T = maturity;
  chain.expiry_ns = years_to_ns(maturity);
  chain.strikes = {96.0, 98.0, 100.0, 102.0, 104.0};
  const std::size_t two_n = 2u * chain.strikes.size();
  chain.bids.assign(two_n, 0.0);
  chain.asks.assign(two_n, 0.0);
  chain.mids.assign(two_n, 0.0);
  for (std::size_t i = 0; i < chain.strikes.size(); ++i) {
    for (const Side side : {Side::Call, Side::Put}) {
      const std::size_t index = chain_index(static_cast<std::uint16_t>(i), side);
      const double mid =
          atx::vol::black76_price(forward, chain.strikes[i], maturity, sigma, df, side);
      chain.mids[index] = mid;
      chain.bids[index] = mid - 0.01;
      chain.asks[index] = mid + 0.01;
    }
  }

  DeAmOptions opts;
  opts.n_atm = chain.strikes.size();
  opts.max_borrow_pairs = chain.strikes.size();
  led::reset();
  const auto resolved = resolve_chain_forward(chain, spot, rate, {}, 0, opts);
  const std::uint64_t boundary_solves = led::snapshot().get(led::Solve::AlBoundarySolves);

  ASSERT_TRUE(resolved.has_value()) << (resolved ? std::string{} : resolved.error().to_string());
  EXPECT_NEAR(resolved->forward, forward, 1.0e-10);
  EXPECT_EQ(boundary_solves, std::uint64_t{0});
  EXPECT_EQ(resolved->carry.n_solved, chain.strikes.size());
}

TEST(DeAmer, EuropeanChainDeAmericanizationIsRawBlack76PassThrough) {
  namespace led = atx::vol::counters::ledger;

  constexpr double spot = 100.0;
  constexpr double rate = 0.02;
  constexpr double maturity = 0.25;
  constexpr double forward = 101.25;
  constexpr double sigma = 0.22;
  const double df = std::exp(-rate * maturity);

  Chain chain;
  chain.exercise_style = ExerciseStyle::European;
  chain.T = maturity;
  chain.expiry_ns = years_to_ns(maturity);
  chain.strikes = {96.0, 98.0, 100.0, 102.0, 104.0};
  const std::size_t two_n = 2u * chain.strikes.size();
  chain.bids.assign(two_n, 0.0);
  chain.asks.assign(two_n, 0.0);
  chain.mids.assign(two_n, 0.0);
  for (std::size_t i = 0; i < chain.strikes.size(); ++i) {
    for (const Side side : {Side::Call, Side::Put}) {
      const std::size_t index = chain_index(static_cast<std::uint16_t>(i), side);
      const double mid = black76_price(forward, chain.strikes[i], maturity, sigma, df, side);
      chain.mids[index] = mid;
      chain.bids[index] = mid - 0.01;
      chain.asks[index] = mid + 0.01;
    }
  }

  DeAmOptions opts;
  opts.n_atm = chain.strikes.size();
  opts.max_borrow_pairs = chain.strikes.size();
  led::reset();
  const auto result = de_americanize_chain(chain, spot, rate, {}, 0, opts);
  const std::uint64_t boundary_solves = led::snapshot().get(led::Solve::AlBoundarySolves);

  ASSERT_TRUE(result.has_value()) << (result ? std::string{} : result.error().to_string());
  EXPECT_EQ(boundary_solves, std::uint64_t{0});
  EXPECT_EQ(result->n_used, chain.strikes.size());
  ASSERT_EQ(result->iv.size(), chain.strikes.size());
  for (const double recovered : result->iv) {
    EXPECT_NEAR(recovered, sigma, 1.0e-8);
  }
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

  const auto fast = resolve_with(al_fast_opts());   // the default carry preset
  const auto accurate = resolve_with(std::nullopt); // cold accurate Andersen-Lake
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
  const std::vector<double> strikes{92.0, 94.0, 96.0, 98.0, 100.0, 102.0, 104.0, 106.0, 108.0};
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

// P3 (perf F3): the per-row de-Am audit reprice (audit_european_equiv_iv — ONE
// ACCURATE-preset cold Andersen-Lake solve per audited row) is batched per
// (expiry, side) through the σ-boundary interpolant slice route
// (audit_european_equiv_iv_batch). Two claims, proven here without wall-clock:
//   (1) EQUIVALENCE — the batched verdict matches the per-row verdict on every
//       row (identical pass/fail set; residual within the Task-11 σ-interpolant
//       gap of 3.8e-5/share, far under the half-spread budget the audit scores
//       against). The audit still certifies IV-inversion consistency; it never
//       claimed boundary-PATH independence, so the σ-interpolant is admissible.
//   (2) G-COUNTER — the always-on solve ledger shows the audited side's AL
//       boundary solves drop from O(strikes) to O(n_σ)=8.
TEST(DeAmer, AuditBatchMatchesPerRowVerdictAndCutsBoundarySolves) {
  namespace led = atx::vol::counters::ledger;
  using atx::vol::IvRepricingAudit;

  // One (expiry, side) slice: fixed (S, T, r, q_eff), many strikes each carrying
  // its own smile σ. More strikes than σ-nodes so the interpolant is built (a
  // ≤ n_σ side stays per-row cold, which is bit-identical anyway).
  const double S = 100.0, T = 0.25, r = 0.03, q_eff = 0.01;
  const Side side = Side::Put;
  const double budget = 0.25; // DeAmOptions default half-spread budget
  std::vector<double> strikes, sigmas, mids, spreads;
  for (int i = 0; i < 24; ++i) {
    const double K = 70.0 + 2.5 * static_cast<double>(i); // 70 .. 127.5
    const double k = std::log(K / S);
    double audit_sigma = 0.20 + 0.35 * k * k; // convex smile => non-degenerate σ box
    // The mid is the American price at the true smile σ, so a row audited at that
    // σ passes with ~0 residual. Two rows are audited at a badly wrong σ so their
    // reprice misses the mid by dollars — a COMFORTABLE fail on both paths, which
    // proves the pass/fail SET (not just the pass rows) is preserved.
    const double mid = value_or_fail(
        american_price(S, K, T, audit_sigma, r, q_eff, side, AmericanMethod::AndersenLake));
    if (i == 5 || i == 15) {
      audit_sigma *= 1.6; // deliberate mispricing => residual >> budget
    }
    strikes.push_back(K);
    sigmas.push_back(audit_sigma);
    mids.push_back(mid);
    spreads.push_back(std::fmax(0.02, 0.02 * mid)); // realistic strictly-positive spread
  }
  const std::size_t n = strikes.size();

  // Per-row audit (the pre-P3 path): one ACCURATE cold solve per row.
  std::vector<atx::core::Result<IvRepricingAudit>> per_row;
  per_row.reserve(n);
  led::reset();
  for (std::size_t i = 0; i < n; ++i) {
    per_row.push_back(audit_european_equiv_iv(mids[i], spreads[i], sigmas[i], S, strikes[i], T, r,
                                              q_eff, side, budget));
  }
  const std::uint64_t solves_per_row = led::snapshot().get(led::Solve::AlBoundarySolves);

  // Batched audit (P3): one σ-interpolant shared by the whole side.
  std::vector<atx::core::Result<IvRepricingAudit>> batch(n);
  led::reset();
  const auto st = audit_european_equiv_iv_batch(S, T, r, q_eff, side, strikes, sigmas, mids,
                                                spreads, budget, batch);
  const std::uint64_t solves_batch = led::snapshot().get(led::Solve::AlBoundarySolves);
  ASSERT_TRUE(st.has_value()) << (st ? std::string{} : st.error().to_string());

  // (1) EQUIVALENCE — identical ok/err and pass/fail; residual within the σ gap.
  std::size_t n_pass = 0, n_fail = 0;
  for (std::size_t i = 0; i < n; ++i) {
    ASSERT_EQ(per_row[i].has_value(), batch[i].has_value()) << "row " << i;
    if (per_row[i].has_value()) {
      EXPECT_EQ(per_row[i]->passed, batch[i]->passed) << "verdict flip at row " << i;
      per_row[i]->passed ? ++n_pass : ++n_fail;
      // σ-interpolant price gap ≤ 3.8e-5/share => residual-half-spread gap ≤ that
      // over half the spread. Stay comfortably under that ceiling on every row.
      const double gap =
          std::fabs(per_row[i]->residual_half_spreads - batch[i]->residual_half_spreads);
      EXPECT_LT(gap, 3.8e-5 / (0.5 * spreads[i]) + 1.0e-9) << "row " << i;
    }
  }
  EXPECT_EQ(n_fail, 2u) << "the two deliberately-mispriced rows must fail on both paths";
  EXPECT_EQ(n_pass, n - 2u);

  // (2) G-COUNTER — audited side O(strikes) -> O(n_σ)=8.
  EXPECT_EQ(solves_per_row, static_cast<std::uint64_t>(n)); // one cold solve per row
  EXPECT_LE(solves_batch, 8u);                              // n_σ interpolant node solves
  EXPECT_LT(solves_batch, solves_per_row);
  std::printf("[deamer-audit-batch] AL boundary solves/side: per-row=%llu batch=%llu (ratio %.3f); "
              "pass=%zu fail=%zu\n",
              static_cast<unsigned long long>(solves_per_row),
              static_cast<unsigned long long>(solves_batch),
              solves_per_row
                  ? static_cast<double>(solves_batch) / static_cast<double>(solves_per_row)
                  : 0.0,
              n_pass, n_fail);
}

// P3 integration: de_americanize_chain routes EVERY approximate-proposal row's
// audit through audit_european_equiv_iv_batch. With the fast preset every OTM leg
// is an audited row, and a > n_σ-per-side board builds the σ-interpolant — the
// wired batched path. The round-trip must still recover the injected smile, every
// row must survive (its batched verdict passes, exactly as the per-row audit
// would on exact-American mids), and the audited-row count must equal the used
// count.
TEST(DeAmer, ChainAuditBatchRoundTripsFastPresetOverEightPerSide) {
  const Scenario sc;
  const double b_true = 0.019;
  // 24 strikes around F≈100 so BOTH OTM sides clear n_σ=8 and take the interpolant.
  std::vector<double> strikes;
  for (int i = 0; i < 24; ++i) {
    strikes.push_back(78.0 + 2.0 * static_cast<double>(i)); // 78 .. 124
  }
  const Chain chain = make_synthetic_chain(sc, b_true, strikes);
  const double f_true =
      hybrid_forward(sc.S, sc.r, b_true, sc.T, sc.divs, sc.expiry_ns, sc.now_ns, sc.hyb);

  DeAmOptions opts;
  opts.hyb = sc.hyb;
  opts.imply_borrow = true;
  opts.n_atm = 6;
  opts.max_borrow_pairs = 6;
  opts.al_opts = al_fast_opts(); // fast inversion => every OTM leg is audited

  const auto res = de_americanize_chain(chain, sc.S, sc.r, sc.divs, sc.now_ns, opts);
  ASSERT_TRUE(res.has_value()) << (res ? std::string{} : res.error().to_string());
  const DeAmResult &out = *res;

  // Every strike survived: each row's batched audit verdict passed.
  EXPECT_EQ(out.n_used, strikes.size());
  EXPECT_EQ(out.n_dropped, 0u);
  EXPECT_EQ(out.n_iv_audited, strikes.size()); // fast preset audits every row
  EXPECT_EQ(out.n_iv_fallback, 0u);            // exact mids => no verdict misses
  ASSERT_EQ(out.iv.size(), strikes.size());

  // Round-trip: the batched-audit path recovers the injected European smile.
  for (std::size_t i = 0; i < strikes.size(); ++i) {
    EXPECT_NEAR(out.iv[i], true_sigma(std::log(strikes[i] / f_true)), 1e-3) << "K=" << strikes[i];
  }
  // The audit reprice only scores a verdict; the served residual stays far inside
  // the half-spread budget on every audited row.
  EXPECT_LT(out.max_iv_residual_half_spreads, opts.max_iv_residual_half_spreads);
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

// ── Deep-ITM well-posedness (T5 item 1) ──────────────────────────────────

// Burkovska, Glau, Mahlstedt & Wohlmuth Remark 4.1, verbatim: S0 = 100, K = 120,
// r = 1%, an American put at P_Am = 20.00 — exactly intrinsic — admits two roots
// whose European equivalents are 18.81 and 19.69. Which one comes back is a
// property of the bracket, so the inversion must be REFUSED, not answered.
TEST(DeAmer, DeepItmQuoteWithinOnePercentOfIntrinsicIsRefusedAsIllPosed) {
  constexpr double S = 100.0, K = 120.0, T = 1.0, r = 0.01, q = 0.0;
  constexpr double intrinsic = K - S; // 20.0, the American put's intrinsic

  EXPECT_FALSE(deam_inversion_well_posed(intrinsic, S, K, Side::Put));
  EXPECT_FALSE(deam_inversion_well_posed(intrinsic * 1.005, S, K, Side::Put));
  EXPECT_TRUE(deam_inversion_well_posed(intrinsic * 1.02, S, K, Side::Put));

  // The seam refuses rather than answering. Before this guard
  // `american_implied_vol` CLAMPED a price at/below intrinsic to the 0.5% vol
  // floor and returned Ok — a confident-looking answer to an unanswerable
  // question, which then repriced its own mid exactly and passed every audit.
  const auto at_intrinsic = european_equiv_iv(intrinsic, S, K, T, r, q, Side::Put);
  ASSERT_FALSE(at_intrinsic.has_value());
  EXPECT_EQ(at_intrinsic.error().code(), ErrorCode::OutOfRange);

  const auto inside_margin = european_equiv_iv(intrinsic * 1.005, S, K, T, r, q, Side::Put);
  ASSERT_FALSE(inside_margin.has_value());
  EXPECT_EQ(inside_margin.error().code(), ErrorCode::OutOfRange);

  // Comfortably above intrinsic the problem is well posed and still inverts —
  // the guard must not amputate the ITM population, only its degenerate tail.
  const double priced =
      value_or_fail(american_price(S, K, T, 0.25, r, q, Side::Put, AmericanMethod::AndersenLake,
                                   std::nullopt));
  ASSERT_GT(priced, intrinsic * 1.01);
  const auto recovered = european_equiv_iv(priced, S, K, T, r, q, Side::Put);
  ASSERT_TRUE(recovered.has_value()) << recovered.error().to_string();
  EXPECT_NEAR(*recovered, 0.25, 1.0e-4);
}

// An out-of-the-money quote has no intrinsic to hide behind, so the guard is a
// strict no-op there — the OTM legs the de-Am strip normally uses are untouched.
TEST(DeAmer, WellPosednessGuardIsANoOpOnOutOfTheMoneyLegs) {
  EXPECT_TRUE(deam_inversion_well_posed(0.01, 100.0, 120.0, Side::Call));
  EXPECT_TRUE(deam_inversion_well_posed(0.01, 100.0, 80.0, Side::Put));
  EXPECT_TRUE(deam_inversion_well_posed(1.0, 100.0, 100.0, Side::Call));
  // A non-finite or negative-margin request is refused, never silently accepted.
  EXPECT_FALSE(deam_inversion_well_posed(std::nan(""), 100.0, 120.0, Side::Put));
  EXPECT_FALSE(deam_inversion_well_posed(25.0, 100.0, 120.0, Side::Put, -1.0));
}

// ── Carry uncertainty in the unit the fit consumes (T5c) ─────────────────

// The rate-unit carry statistics are restated as standard-deviation moneyness
// by exactly `sqrt(T) / atm_sigma`: a borrow error `db` moves every observation's
// `k = ln(K/F)` by `db*T`, which is `db*sqrt(T)/sigma` slice standard deviations.
// `atm_sigma` is the retained pairs' own near-ATM level, so the identity is
// checkable against the smile the chain was generated from.
TEST(DeAmer, CarryUncertaintyIsRestatedInStandardDeviationMoneyness) {
  const Scenario sc;
  Chain chain = make_synthetic_chain(sc, 0.031, {96.0, 98.0, 100.0, 102.0, 104.0});
  DeAmOptions opts;
  opts.hyb = sc.hyb;
  opts.n_atm = 5;
  opts.max_borrow_pairs = 5;

  const auto result = resolve_chain_forward(chain, sc.S, sc.r, sc.divs, sc.now_ns, opts);
  ASSERT_TRUE(result.has_value()) << (result ? std::string{} : result.error().to_string());
  const auto &carry = result->carry;
  ASSERT_GT(carry.n_retained, 0u);

  // The near-ATM level the pairs converged on IS the generating smile's ATM vol.
  EXPECT_NEAR(carry.atm_sigma, true_sigma(0.0), 0.02);

  const double scale = std::sqrt(sc.T) / carry.atm_sigma;
  EXPECT_DOUBLE_EQ(carry.dispersion_moneyness, carry.dispersion * scale);
  EXPECT_DOUBLE_EQ(carry.max_leave_one_out_moneyness, carry.max_leave_one_out_shift * scale);
  EXPECT_DOUBLE_EQ(carry.confidence_half_width_moneyness, carry.confidence_half_width * scale);
}

// A one-pair solve reports dispersion 0 and leave-one-out 0 only because nothing
// disputes it (de-Am review D6). It must never present as a bounded carry, no
// matter how reassuring those two numbers look.
TEST(DeAmer, SinglePairCarryIsNeverMoneynessBounded) {
  const Scenario sc;
  Chain chain = make_synthetic_chain(sc, 0.02, {100.0});
  DeAmOptions opts;
  opts.hyb = sc.hyb;
  opts.n_atm = 1;
  opts.max_borrow_pairs = 1;
  opts.min_confident_borrow_pairs = 3;
  opts.require_carry_confidence = false; // want the diagnostics, not the Err

  const auto result = resolve_chain_forward(chain, sc.S, sc.r, sc.divs, sc.now_ns, opts);
  ASSERT_TRUE(result.has_value()) << (result ? std::string{} : result.error().to_string());
  EXPECT_EQ(result->carry.n_retained, 1u);
  EXPECT_DOUBLE_EQ(result->carry.dispersion, 0.0);
  EXPECT_DOUBLE_EQ(result->carry.max_leave_one_out_shift, 0.0);
  EXPECT_FALSE(result->carry.confident);
  EXPECT_FALSE(carry_moneyness_bounded(result->carry, opts));
}

// The second tier is a MEASUREMENT, not a waiver: a carry whose moneyness shift
// exceeds the budget stays unbounded, and one inside it is bounded while still
// reporting `confident == false` under a rate gate it misses.
TEST(DeAmer, MoneynessBoundedTracksTheBudgetAndNeverImpliesConfidence) {
  const Scenario sc;
  Chain chain = make_synthetic_chain(sc, 0.031, {96.0, 98.0, 100.0, 102.0, 104.0});
  DeAmOptions opts;
  opts.hyb = sc.hyb;
  opts.n_atm = 5;
  opts.max_borrow_pairs = 5;
  // A rate gate this tight cannot be met by any real solve, so the expiry is
  // NOT confident — exactly the production shape this tier exists for.
  opts.max_carry_leave_one_out = 1.0e-12;

  const auto result = resolve_chain_forward(chain, sc.S, sc.r, sc.divs, sc.now_ns, opts);
  ASSERT_TRUE(result.has_value()) << (result ? std::string{} : result.error().to_string());
  ASSERT_FALSE(result->carry.confident);
  ASSERT_GE(result->carry.n_retained, opts.min_confident_borrow_pairs);

  // Budget straddling the measured shift: bounded above it, not below it.
  const double measured = result->carry.max_leave_one_out_moneyness;
  ASSERT_TRUE(std::isfinite(measured));
  DeAmOptions generous = opts;
  generous.max_carry_moneyness_shift = std::fmax(measured * 2.0, 1.0e-9);
  EXPECT_TRUE(carry_moneyness_bounded(result->carry, generous));

  DeAmOptions strict = opts;
  strict.max_carry_moneyness_shift = measured * 0.5;
  EXPECT_FALSE(carry_moneyness_bounded(result->carry, strict));

  // A non-positive budget disables the tier outright.
  DeAmOptions disabled = opts;
  disabled.max_carry_moneyness_shift = 0.0;
  EXPECT_FALSE(carry_moneyness_bounded(result->carry, disabled));
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
