// Chain-level batch price / Greeks / implied vol (api/pricing/batch_chain.hpp).
//
// The reference throughout is an independently written per-contract loop: the
// kernel call a caller would make for each contract at the expiry's effective carry.
// The Scalar and PerContract routes must reproduce it bit for bit; the opt-in
// LanedAvx2 and Lockstep routes must stay within their stated bounds; every route
// must give the same bits for every thread count.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "atx/vol/api/pricing/american.hpp"
#include "atx/vol/api/backtest/priced_surface.hpp"
#include "atx/vol/api/fitting/vol_curve.hpp"
#include "atx/vol/api/pricing/american_iv.hpp"
#include "atx/vol/api/pricing/batch_chain.hpp"
#include "atx/vol/api/pricing/pricing_executor.hpp"
#include "support/process_alloc_counter.hpp"

namespace atx::vol {
namespace {

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

TEST(PricedSurfaceGreekAlloc, OrdinaryPartialRefusalsDoNotAllocate) {
  constexpr double S = 772.99, K = 811.6395, T = .0682, sigma = .12;
  constexpr double r = .04496, q = .00162193;
  const double F = S * std::exp((r - q) * T);
  EssviParams params{};
  params.T = T; params.F = F; params.theta = sigma * sigma * T;
  params.phi = params.rho = 0.;
  params.psi = params.p = params.lambda = .5;
  CurveSurface curves;
  curves.push(std::make_unique<EssviCurve>(params, std::exp(-r * T)));
  PricingContext pricing;
  pricing.S = S; pricing.r = r;
  pricing.al_opts = al_fast_profile(std::nullopt);
  pricing.al_opts.profile = AlProfile::Reference;
  auto s = PricedSurface::create(std::move(curves), {{T, F, 0., q, 100u, 0u}}, pricing);
  ASSERT_TRUE(s.has_value());
  using EF = PricedSurface::EvalField;
  const auto warm = s->evaluate(K, T, Side::Put, EF::FirstOrder, true);
  ASSERT_FALSE(warm.status.has_value());
  ASSERT_EQ(warm.greeks.reasons[1], AmericanGreekReason::StencilCrossing);
  const std::array<double, 1> strikes{K}, tenors{T};
  const std::array<Side, 1> sides{Side::Put};
  std::array<double, 1> ivs{}, prices{};
  std::array<AmericanGreeks, 1> greeks{};
  std::array<Status, 1> statuses{};
  bool retained = true;
  const auto allocations = testkit::count_allocations([&] {
    for (const bool analytic : {false, true}) {
      const auto result = s->evaluate(K, T, Side::Put, EF::FirstOrder, analytic);
      retained = retained && !result.status.has_value() && std::isfinite(result.price);
    }
    const auto rc = s->evaluate_batch(strikes, tenors, sides, EF::FirstOrder, true,
        PricedSurface::EvaluationSoA{ivs, prices, greeks, statuses, {}, {}});
    retained = retained && rc.has_value() && !statuses[0].has_value() && std::isfinite(prices[0]);
  });
  EXPECT_TRUE(retained);
  EXPECT_EQ(allocations, 0u);
}

struct Chain {
  double spot = 100.0;
  std::vector<double> T, rate, carry, div_pv;
  std::vector<double> strike, sigma;
  std::vector<std::uint32_t> expiry;
  std::vector<Side> side;

  [[nodiscard]] ChainExpiryColumns exp() const { return {T, rate, carry, div_pv}; }
  [[nodiscard]] ChainContractColumns con() const { return {strike, expiry, side, sigma}; }
  [[nodiscard]] std::size_t size() const { return strike.size(); }
};

// Four expiries (one with a cash dividend PV, one with none), calls and puts
// interleaved strike by strike so every chunk mixes sides.
[[nodiscard]] Chain make_chain(std::size_t strikes = 5) {
  Chain c;
  c.T = {0.02, 0.25, 1.0, 2.0};
  c.rate = {0.04, 0.045, 0.05, 0.05};
  c.carry = {0.01, 0.0, 0.02, 0.015};
  c.div_pv = {0.0, 0.8, 1.5, 0.0};
  for (std::uint32_t e = 0; e < c.T.size(); ++e) {
    const double half = 0.6 * 0.3 * std::sqrt(c.T[e]);
    for (std::size_t j = 0; j < strikes; ++j) {
      const double k =
          -half + 2.0 * half * static_cast<double>(j) / static_cast<double>(strikes - 1u);
      for (const Side sd : {Side::Call, Side::Put}) {
        c.strike.push_back(c.spot * std::exp(k));
        c.sigma.push_back(0.28 - 0.08 * k + 0.25 * k * k);
        c.expiry.push_back(e);
        c.side.push_back(sd);
      }
    }
  }
  return c;
}

// The first n contracts of `c` (expiry columns unchanged).
[[nodiscard]] Chain head(const Chain &c, std::size_t n) {
  Chain h = c;
  h.strike.resize(n);
  h.sigma.resize(n);
  h.expiry.resize(n);
  h.side.resize(n);
  return h;
}

struct Outs {
  std::vector<double> price, delta, gamma, vega, theta, rho;
  std::vector<ChainRowStatus> status;

  explicit Outs(std::size_t n, double fill = -7.0)
      : price(n, fill), delta(n, fill), gamma(n, fill), vega(n, fill), theta(n, fill),
        rho(n, fill), status(n, ChainRowStatus::NoConvergence) {}

  [[nodiscard]] ChainGreekColumns all() { return {price, delta, gamma, vega, theta, rho, status}; }
  [[nodiscard]] ChainGreekColumns price_only() { return {price, {}, {}, {}, {}, {}, status}; }
};

[[nodiscard]] bool same_bits(std::span<const double> a, std::span<const double> b) {
  return a.size() == b.size() &&
         (a.empty() || std::memcmp(a.data(), b.data(), a.size() * sizeof(double)) == 0);
}

[[nodiscard]] bool same_outs(const Outs &a, const Outs &b) {
  return same_bits(a.price, b.price) && same_bits(a.delta, b.delta) &&
         same_bits(a.gamma, b.gamma) && same_bits(a.vega, b.vega) &&
         same_bits(a.theta, b.theta) && same_bits(a.rho, b.rho) && a.status == b.status;
}

// Validity as the batch contract states it, written out independently.
[[nodiscard]] bool row_valid(const Chain &c, std::size_t i, double *q_eff) {
  const std::uint32_t e = c.expiry[i];
  if (e >= c.T.size() || !std::isfinite(c.rate[e])) {
    return false;
  }
  const double D = c.div_pv.empty() ? 0.0 : c.div_pv[e];
  *q_eff = chain_effective_carry(c.spot, c.T[e], c.carry[e], D);
  return std::isfinite(*q_eff) && std::isfinite(c.strike[i]) && c.strike[i] > 0.0 &&
         std::isfinite(c.sigma[i]) && c.sigma[i] > 0.0;
}

void fail_row(Outs &o, std::size_t i, ChainRowStatus st, bool greeks) {
  o.price[i] = kNaN;
  if (greeks) {
    o.delta[i] = o.gamma[i] = o.vega[i] = o.theta[i] = o.rho[i] = kNaN;
  }
  o.status[i] = st;
}

// The per-contract loop: andersen_lake for prices only, else american_greeks_al
// with the requested vega / rho solves, at the expiry's effective carry.
[[nodiscard]] Outs reference_loop(const Chain &c, bool greeks, bool vega = true,
                                  bool rho = true,
                                  const std::optional<AlOpts> &opts = std::nullopt) {
  Outs o(c.size());
  for (std::size_t i = 0; i < c.size(); ++i) {
    double q = 0.0;
    if (!row_valid(c, i, &q)) {
      fail_row(o, i, ChainRowStatus::InvalidInput, greeks);
      continue;
    }
    const std::uint32_t e = c.expiry[i];
    if (!greeks) {
      const Result<double> p = andersen_lake(c.spot, c.strike[i], c.T[e], c.sigma[i], c.rate[e],
                                             q, c.side[i], opts);
      if (p && std::isfinite(*p)) {
        o.price[i] = *p;
        o.status[i] = ChainRowStatus::Ok;
      } else {
        fail_row(o, i, ChainRowStatus::Unpriceable, false);
      }
      continue;
    }
    const Result<AmericanGreeks> g = american_greeks_al(
        c.spot, c.strike[i], c.T[e], c.sigma[i], c.rate[e], q, c.side[i], opts, vega, rho, false);
    const bool fine = g && std::isfinite(g->price) && std::isfinite(g->delta) &&
                      std::isfinite(g->gamma) && std::isfinite(g->theta) &&
                      (!vega || std::isfinite(g->vega)) && (!rho || std::isfinite(g->rho));
    if (!fine) {
      fail_row(o, i, ChainRowStatus::Unpriceable, true);
      continue;
    }
    o.price[i] = g->price;
    o.delta[i] = g->delta;
    o.gamma[i] = g->gamma;
    o.vega[i] = vega ? g->vega : -7.0;
    o.theta[i] = g->theta;
    o.rho[i] = rho ? g->rho : -7.0;
    o.status[i] = ChainRowStatus::Ok;
  }
  return o;
}

[[nodiscard]] Outs run_batch(const Chain &c, ChainBatchOptions opt, bool greeks = true) {
  Outs o(c.size());
  ChainBatchWorkspace ws;
  const Result<std::size_t> r =
      price_chain_batch(c.spot, c.exp(), c.con(), greeks ? o.all() : o.price_only(), opt, ws);
  EXPECT_TRUE(r.has_value());
  if (r) {
    EXPECT_EQ(*r, c.size());
  }
  return o;
}

// ── Shape and boundary validation ────────────────────────────────────────────

TEST(ChainBatch, EmptyChainReturnsZero) {
  Chain c = make_chain();
  c = head(c, 0);
  Outs o(0);
  ChainBatchWorkspace ws;
  const Result<std::size_t> r = price_chain_batch(c.spot, c.exp(), c.con(), o.all(), {}, ws);
  ASSERT_TRUE(r.has_value());
  EXPECT_EQ(*r, 0u);
}

TEST(ChainBatch, RejectsInconsistentShapesAndWritesNothing) {
  const Chain c = make_chain();
  ChainBatchWorkspace ws;
  const auto refused = [&](const ChainExpiryColumns &x, const ChainContractColumns &k,
                           ChainGreekColumns out, double spot) {
    const Result<std::size_t> r = price_chain_batch(spot, x, k, out, {}, ws);
    return !r.has_value() && r.error().code() == ErrorCode::InvalidArgument;
  };
  Outs o(c.size());
  ChainExpiryColumns short_rate = c.exp();
  short_rate.rate = short_rate.rate.first(2);
  EXPECT_TRUE(refused(short_rate, c.con(), o.all(), c.spot));
  ChainContractColumns short_sigma = c.con();
  short_sigma.sigma = short_sigma.sigma.first(3);
  EXPECT_TRUE(refused(c.exp(), short_sigma, o.all(), c.spot));
  ChainGreekColumns short_vega = o.all();
  short_vega.vega = short_vega.vega.first(1);
  EXPECT_TRUE(refused(c.exp(), c.con(), short_vega, c.spot));
  ChainGreekColumns short_status = o.all();
  short_status.status = short_status.status.first(c.size() - 1u);
  EXPECT_TRUE(refused(c.exp(), c.con(), short_status, c.spot));
  Outs fewer(c.size() - 1u);
  EXPECT_TRUE(refused(c.exp(), c.con(), fewer.all(), c.spot));
  EXPECT_TRUE(refused(c.exp(), c.con(), o.all(), 0.0));
  EXPECT_TRUE(refused(c.exp(), c.con(), o.all(), kNaN));
  EXPECT_TRUE(refused(c.exp(), c.con(), o.all(), std::numeric_limits<double>::infinity()));
  for (std::size_t i = 0; i < c.size(); ++i) {
    ASSERT_EQ(o.price[i], -7.0);
    ASSERT_EQ(o.status[i], ChainRowStatus::NoConvergence);
  }
}

TEST(ChainBatch, RejectsAnOutputThatAliasesAnInputOrAnotherOutput) {
  Chain c = make_chain();
  ChainBatchWorkspace ws;
  Outs o(c.size());
  ChainGreekColumns over_strike = o.all();
  over_strike.price = std::span<double>(c.strike); // output written over an input
  const Result<std::size_t> a = price_chain_batch(c.spot, c.exp(), c.con(), over_strike, {}, ws);
  ASSERT_FALSE(a.has_value());
  EXPECT_EQ(a.error().code(), ErrorCode::InvalidArgument);
  ChainGreekColumns twice = o.all();
  twice.delta = twice.gamma; // two outputs share storage
  const Result<std::size_t> b = price_chain_batch(c.spot, c.exp(), c.con(), twice, {}, ws);
  ASSERT_FALSE(b.has_value());
  EXPECT_EQ(b.error().code(), ErrorCode::InvalidArgument);
  EXPECT_EQ(c.strike, make_chain().strike);
}

TEST(ChainBatch, EffectiveCarryFoldsTheDividendPvIntoTheForward) {
  EXPECT_EQ(chain_effective_carry(100.0, 0.5, 0.013, 0.0), 0.013);
  const double S = 100.0, T = 0.75, r = 0.05, q = 0.01, D = 1.7;
  const double qe = chain_effective_carry(S, T, q, D);
  const double fwd_batch = S * std::exp((r - qe) * T);
  const double fwd_escrow = (S - D) * std::exp((r - q) * T);
  EXPECT_NEAR(fwd_batch, fwd_escrow, 1.0e-12 * fwd_escrow);
  EXPECT_TRUE(std::isnan(chain_effective_carry(S, T, q, S)));
  EXPECT_TRUE(std::isnan(chain_effective_carry(S, T, q, -0.1)));
  EXPECT_TRUE(std::isnan(chain_effective_carry(S, 0.0, q, 0.0)));
  EXPECT_TRUE(std::isnan(chain_effective_carry(S, T, kNaN, 0.0)));
  EXPECT_TRUE(std::isnan(chain_effective_carry(0.0, T, q, 0.0)));
}

// ── Scalar route: bit-identity with the per-contract loop ────────────────────

TEST(ChainBatch, ScalarIsBitIdenticalToThePerContractLoop) {
  const Chain c = make_chain();
  const Outs ref = reference_loop(c, /*greeks=*/true);
  const Outs got = run_batch(c, {});
  EXPECT_TRUE(same_outs(ref, got));
  EXPECT_EQ(std::count(got.status.begin(), got.status.end(), ChainRowStatus::Ok),
            static_cast<std::ptrdiff_t>(c.size()));
}

TEST(ChainBatch, ScalarPriceOnlyIsAndersenLakeAndMatchesTheGreekRequestPrice) {
  const Chain c = make_chain();
  const Outs ref = reference_loop(c, /*greeks=*/false);
  const Outs got = run_batch(c, {}, /*greeks=*/false);
  EXPECT_TRUE(same_bits(ref.price, got.price));
  EXPECT_EQ(ref.status, got.status);
  // Greek columns of a price-only request are untouched.
  EXPECT_TRUE(std::all_of(got.delta.begin(), got.delta.end(), [](double v) { return v == -7.0; }));
  // american_greeks_al's price is andersen_lake's: the price column does not move
  // with the Greek request.
  const Outs full = run_batch(c, {});
  EXPECT_TRUE(same_bits(full.price, got.price));
}

TEST(ChainBatch, ScalarGreekSubsetsSkipOnlyTheirOwnSolves) {
  const Chain c = make_chain();
  Outs got(c.size());
  ChainGreekColumns cols{got.price, got.delta, {}, {}, got.theta, {}, got.status};
  ChainBatchWorkspace ws;
  ASSERT_TRUE(price_chain_batch(c.spot, c.exp(), c.con(), cols, {}, ws).has_value());
  const Outs ref = reference_loop(c, true, /*vega=*/false, /*rho=*/false);
  EXPECT_TRUE(same_bits(ref.price, got.price));
  EXPECT_TRUE(same_bits(ref.delta, got.delta));
  EXPECT_TRUE(same_bits(ref.theta, got.theta));
  EXPECT_EQ(ref.status, got.status);
  EXPECT_TRUE(std::all_of(got.vega.begin(), got.vega.end(), [](double v) { return v == -7.0; }));
}

TEST(ChainBatch, ScalarHonoursAnEngagedAlOptsPreset) {
  const Chain c = make_chain(3);
  ChainBatchOptions opt;
  opt.al_opts = al_bulk_opts();
  EXPECT_TRUE(same_outs(reference_loop(c, true, true, true, al_bulk_opts()), run_batch(c, opt)));
}

TEST(ChainBatch, OddSizesAreBitIdenticalInlineAndFannedOut) {
  const Chain big = make_chain(9); // 72 contracts: chunks of 32, 32, 8
  for (const std::size_t n : {1u, 3u, 31u, 32u, 33u, 65u, 72u}) {
    const Chain c = head(big, n);
    const Outs ref = reference_loop(c, true);
    EXPECT_TRUE(same_outs(ref, run_batch(c, {}))) << "n=" << n;
    ChainBatchOptions fan;
    fan.executor = &pricing_executor();
    EXPECT_TRUE(same_outs(ref, run_batch(c, fan))) << "n=" << n << " fanned out";
  }
}

TEST(ChainBatch, StatusPropagatesPerRowAndNeverThrows) {
  Chain c = make_chain(3);
  // Expiry 4: T <= 0. Expiry 5: div_pv >= spot. Expiry 6: the double-continuation
  // put regime q < r <= 0 (the kernel refuses it).
  c.T.insert(c.T.end(), {0.0, 0.5, 0.5});
  c.rate.insert(c.rate.end(), {0.04, 0.04, -0.01});
  c.carry.insert(c.carry.end(), {0.0, 0.0, -0.02});
  c.div_pv.insert(c.div_pv.end(), {0.0, 150.0, 0.0});
  c.strike[1] = 0.0;
  c.strike[2] = kNaN;
  c.sigma[3] = -0.2;
  c.sigma[4] = std::numeric_limits<double>::infinity();
  c.expiry[5] = 99u;
  c.expiry[6] = 4u;
  c.expiry[7] = 5u;
  c.expiry[8] = 6u;
  c.side[8] = Side::Put;
  const Outs got = run_batch(c, {});
  const Outs ref = reference_loop(c, true);
  EXPECT_TRUE(same_outs(ref, got));
  for (const std::size_t i : {1u, 2u, 3u, 4u, 5u, 6u, 7u}) {
    EXPECT_EQ(got.status[i], ChainRowStatus::InvalidInput) << i;
    EXPECT_TRUE(std::isnan(got.price[i]) && std::isnan(got.delta[i]) && std::isnan(got.rho[i]));
  }
  EXPECT_EQ(got.status[8], ChainRowStatus::Unpriceable);
  EXPECT_TRUE(std::isnan(got.price[8]));
  EXPECT_EQ(got.status[0], ChainRowStatus::Ok);
  EXPECT_EQ(got.status[9], ChainRowStatus::Ok);
  ChainBatchOptions laned;
  laned.route = ChainBatchRoute::LanedAvx2;
  const Outs lgot = run_batch(c, laned);
  for (std::size_t i = 0; i < c.size(); ++i) {
    EXPECT_EQ(lgot.status[i], got.status[i]) << i;
  }
}

TEST(ChainBatch, OutputsAreIdenticalForEveryThreadCount) {
  const Chain c = make_chain(9);
  for (const ChainBatchRoute route : {ChainBatchRoute::Scalar, ChainBatchRoute::LanedAvx2}) {
    ChainBatchOptions inline_opt;
    inline_opt.route = route;
    const Outs base = run_batch(c, inline_opt);
    for (const unsigned nt : {1u, 2u, 3u, 0u}) {
      ChainBatchOptions fan = inline_opt;
      fan.executor = &pricing_executor();
      fan.n_threads = nt;
      EXPECT_TRUE(same_outs(base, run_batch(c, fan)))
          << "route=" << static_cast<int>(route) << " n_threads=" << nt;
    }
  }
}

// ── LanedAvx2 route: within the laned kernel's gate ──────────────────────────

TEST(ChainBatch, LanedAvx2StaysWithinItsBoundOfTheScalarRoute) {
  const Chain c = make_chain(9);
  const Outs s = run_batch(c, {});
  ChainBatchOptions laned;
  laned.route = ChainBatchRoute::LanedAvx2;
  const Outs v = run_batch(c, laned);
  double dp = 0.0, dd = 0.0, dg = 0.0, dv = 0.0, dt = 0.0, dr = 0.0;
  for (std::size_t i = 0; i < c.size(); ++i) {
    ASSERT_EQ(s.status[i], ChainRowStatus::Ok);
    ASSERT_EQ(v.status[i], ChainRowStatus::Ok);
    dp = std::max(dp, std::abs(v.price[i] - s.price[i]));
    dd = std::max(dd, std::abs(v.delta[i] - s.delta[i]));
    dg = std::max(dg, std::abs(v.gamma[i] - s.gamma[i]));
    dv = std::max(dv, std::abs(v.vega[i] - s.vega[i]));
    dt = std::max(dt, std::abs(v.theta[i] - s.theta[i]));
    dr = std::max(dr, std::abs(v.rho[i] - s.rho[i]));
  }
  std::printf("[laned-vs-scalar] max|d| price %.3e delta %.3e gamma %.3e vega %.3e theta %.3e "
              "rho %.3e\n",
              dp, dd, dg, dv, dt, dr);
  // The kernel's documented gate is ~1e-6 USD normal; Greeks inherit it through
  // their finite-difference stencils (h_S = 1e-3 S, h_sigma = 1e-3, h_r = 1e-4).
  EXPECT_LE(dp, 1.0e-6);
  EXPECT_LE(dd, 1.0e-5);
  EXPECT_LE(dg, 1.0e-5);
  EXPECT_LE(dv, 1.0e-3);
  EXPECT_LE(dt, 1.0e-3);
  EXPECT_LE(dr, 1.0e-2);
}

// ── Implied vol ──────────────────────────────────────────────────────────────

struct IvOuts {
  std::vector<double> iv;
  std::vector<ChainRowStatus> status;
  explicit IvOuts(std::size_t n) : iv(n, -7.0), status(n, ChainRowStatus::Ok) {}
};

[[nodiscard]] std::vector<double> targets_at(const Chain &c, double bump) {
  std::vector<double> t(c.size(), kNaN);
  for (std::size_t i = 0; i < c.size(); ++i) {
    double q = 0.0;
    if (!row_valid(c, i, &q)) {
      continue;
    }
    const std::uint32_t e = c.expiry[i];
    const Result<double> p = andersen_lake(c.spot, c.strike[i], c.T[e], bump * c.sigma[i],
                                           c.rate[e], q, c.side[i], std::nullopt);
    t[i] = p ? *p : kNaN;
  }
  return t;
}

[[nodiscard]] IvOuts run_iv(const Chain &c, std::span<const double> target, ChainIvOptions opt) {
  IvOuts o(c.size());
  ChainBatchWorkspace ws;
  const Result<std::size_t> r =
      implied_vol_chain_batch(c.spot, c.exp(), c.con(), target, o.iv, o.status, opt, ws);
  EXPECT_TRUE(r.has_value());
  return o;
}

// american_iv.cpp's zero-volatility American value, written out again from its
// formula: the sup over exercise dates t in [0, T] of A e^{-a t} - B e^{-b t}
// (call: A = S, a = q, B = K, b = r; put: the roles swap), floored at 0.
[[nodiscard]] double zero_vol_ref(double S, double K, double T, double r, double q, Side sd) {
  const bool call = sd == Side::Call;
  const double A = call ? S : K;
  const double a = call ? q : r;
  const double B = call ? K : S;
  const double b = call ? r : q;
  const auto pay = [&](double t) { return A * std::exp(-a * t) - B * std::exp(-b * t); };
  const double f0 = pay(0.0);
  const double fT = pay(T);
  if (!std::isfinite(f0) || !std::isfinite(fT)) {
    return kNaN;
  }
  double best = std::fmax(f0, fT);
  const double ts = std::log((a * A) / (b * B)) / (a - b);
  if (std::isfinite(ts) && ts > 0.0 && ts < T) {
    const double fs = pay(ts);
    if (std::isfinite(fs)) {
      best = std::fmax(best, fs);
    }
  }
  return std::fmax(best, 0.0);
}

[[nodiscard]] double ceiling_ref(double S, double K, double T, double r, double q, Side sd) {
  return sd == Side::Call ? S * std::fmax(1.0, std::exp(-q * T))
                          : K * std::fmax(1.0, std::exp(-r * T));
}

[[nodiscard]] double band_ref(double P, double ceiling, const ChainIvOptions &o) {
  return std::max(std::max(o.intrinsic_tol, 1.0e-9) * ceiling, o.price_tol * std::max(1.0, P)) +
         1.0e-12;
}

// The screen as the header's "One implied-vol refusal rule" (1) states it.
[[nodiscard]] ChainRowStatus screen_ref(double P, double S, double K, double T, double r,
                                        double q, Side sd, const ChainIvOptions &o) {
  const double ceiling = ceiling_ref(S, K, T, r, q, sd);
  const double zv = zero_vol_ref(S, K, T, r, q, sd);
  if (!std::isfinite(ceiling) || !std::isfinite(zv)) {
    return ChainRowStatus::Unpriceable;
  }
  const double band = band_ref(P, ceiling, o);
  const double immediate = sd == Side::Call ? std::fmax(S - K, 0.0) : std::fmax(K - S, 0.0);
  if (P > ceiling + band || P < immediate - band) {
    return ChainRowStatus::OutOfBand;
  }
  if (std::abs(P - zv) <= band) {
    return ChainRowStatus::VegaFloor;
  }
  return P < zv - band ? ChainRowStatus::OutOfBand : ChainRowStatus::Ok;
}

[[nodiscard]] double al_ref(double S, double K, double T, double sig, double r, double q, Side sd,
                            const std::optional<AlOpts> &opts = std::nullopt) {
  const Result<double> p = andersen_lake(S, K, T, sig, r, q, sd, opts);
  return p && std::isfinite(*p) ? *p : kNaN;
}

// The rule's (2) cold-map verification of american_implied_vol's answer x: a chord
// to x (1 - 1e-4), then up to 4 secant steps.
[[nodiscard]] ChainRowStatus verify_ref(double P, double S, double K, double T, double r,
                                        double q, Side sd, const ChainIvOptions &o, double &x) {
  const double ptol = o.price_tol * std::max(1.0, P);
  const double vmin = o.vega_floor * K;
  const double xb = x * (1.0 - 1.0e-4);
  double fx = al_ref(S, K, T, x, r, q, sd, o.al_opts) - P;
  const double fb = al_ref(S, K, T, xb, r, q, sd, o.al_opts) - P;
  if (!std::isfinite(fx) || !std::isfinite(fb)) {
    return ChainRowStatus::Unpriceable;
  }
  double slope = (fx - fb) / (x - xb);
  if (!(slope > vmin)) {
    return ChainRowStatus::VegaFloor;
  }
  for (int k = 0;; ++k) {
    if (std::abs(fx) <= ptol && slope > vmin && std::abs(fx) <= o.sigma_tol * slope) {
      return ChainRowStatus::Ok;
    }
    if (k == 4) {
      return ChainRowStatus::NoConvergence;
    }
    const double xn = x - fx / slope;
    if (!(xn > kIvMin)) {
      return ChainRowStatus::VegaFloor;
    }
    if (!(xn < kIvMax)) {
      return ChainRowStatus::OutOfBand;
    }
    if (xn == x) {
      return ChainRowStatus::NoConvergence;
    }
    const double fn = al_ref(S, K, T, xn, r, q, sd, o.al_opts) - P;
    if (!std::isfinite(fn)) {
      return ChainRowStatus::Unpriceable;
    }
    const double span = std::abs(xn - x);
    slope = (fn - fx) / (xn - x);
    x = xn;
    fx = fn;
    if (!(slope > vmin)) {
      return span >= 1.0e-4 * x ? ChainRowStatus::VegaFloor : ChainRowStatus::NoConvergence;
    }
  }
}

// The per-contract IV loop the PerContract route must reproduce: the screen,
// american_implied_vol warm-seeded at the contract's sigma, the verification.
[[nodiscard]] IvOuts reference_iv(const Chain &c, std::span<const double> target,
                                  const ChainIvOptions &o) {
  IvOuts out(c.size());
  for (std::size_t i = 0; i < c.size(); ++i) {
    double q = 0.0;
    const auto put = [&](ChainRowStatus st, double v) {
      out.status[i] = st;
      out.iv[i] = v;
    };
    if (!row_valid(c, i, &q) || !std::isfinite(target[i])) {
      put(ChainRowStatus::InvalidInput, kNaN);
      continue;
    }
    const double K = c.strike[i];
    const std::uint32_t e = c.expiry[i];
    const double T = c.T[e];
    const double r = c.rate[e];
    const ChainRowStatus scr = screen_ref(target[i], c.spot, K, T, r, q, c.side[i], o);
    if (scr != ChainRowStatus::Ok) {
      put(scr, kNaN);
      continue;
    }
    const Result<double> iv = american_implied_vol(target[i], c.spot, K, T, r, q, c.side[i],
                                                   AmericanMethod::AndersenLake, o.sigma_tol,
                                                   o.max_iter, o.al_opts, nullptr, c.sigma[i], 0.0);
    if (!iv) {
      const ErrorCode code = iv.error().code();
      put(code == ErrorCode::OutOfRange    ? ChainRowStatus::OutOfBand
          : code == ErrorCode::Unavailable ? ChainRowStatus::NoConvergence
                                           : ChainRowStatus::Unpriceable,
          kNaN);
      continue;
    }
    double x = *iv;
    if (!(x > kIvMin)) {
      put(ChainRowStatus::VegaFloor, kNaN);
      continue;
    }
    if (!(x < kIvMax)) {
      put(ChainRowStatus::OutOfBand, kNaN);
      continue;
    }
    const ChainRowStatus st = verify_ref(target[i], c.spot, K, T, r, q, c.side[i], o, x);
    put(st, st == ChainRowStatus::Ok ? x : kNaN);
  }
  return out;
}

// Smallest sigma in [kIvMin, kIvMax] with andersen_lake(sigma) >= target (a
// 200-step bisection); NaN when the target is outside the image.
[[nodiscard]] double bisect_root(double P, double S, double K, double T, double r, double q,
                                 Side sd) {
  double lo = kIvMin;
  double hi = kIvMax;
  if (!(al_ref(S, K, T, lo, r, q, sd) < P) || !(al_ref(S, K, T, hi, r, q, sd) >= P)) {
    return kNaN;
  }
  for (int it = 0; it < 200; ++it) {
    const double mid = 0.5 * (lo + hi);
    if (mid <= lo || mid >= hi) {
      break;
    }
    (al_ref(S, K, T, mid, r, q, sd) >= P ? hi : lo) = mid;
  }
  return hi;
}

// A one-expiry chain of single contracts at the given strikes / sides / seeds.
[[nodiscard]] Chain one_expiry(double S, double T, double r, double carry, double div_pv,
                               const std::vector<double> &strikes, const std::vector<Side> &sides,
                               double seed) {
  Chain c;
  c.spot = S;
  c.T = {T};
  c.rate = {r};
  c.carry = {carry};
  c.div_pv = {div_pv};
  for (std::size_t j = 0; j < strikes.size(); ++j) {
    c.strike.push_back(strikes[j]);
    c.side.push_back(sides[j]);
    c.sigma.push_back(seed);
    c.expiry.push_back(0u);
  }
  return c;
}

[[nodiscard]] ChainIvOptions iv_route(ChainIvRoute route) {
  ChainIvOptions o;
  o.route = route;
  return o;
}

TEST(ChainBatchIv, PerContractIsBitIdenticalToTheWarmSeededLoop) {
  const Chain c = make_chain();
  const std::vector<double> target = targets_at(c, 1.05);
  const IvOuts ref = reference_iv(c, target, {});
  const IvOuts got = run_iv(c, target, {});
  EXPECT_TRUE(same_bits(ref.iv, got.iv));
  EXPECT_EQ(ref.status, got.status);
}

TEST(ChainBatchIv, BothRoutesRecoverTheVolAndLockstepTracksPerContract) {
  const Chain c = make_chain();
  const std::vector<double> target = targets_at(c, 1.05);
  const IvOuts pc = run_iv(c, target, {});
  ChainIvOptions lock;
  lock.route = ChainIvRoute::Lockstep;
  const IvOuts ls = run_iv(c, target, lock);
  double worst_true = 0.0, worst_pc = 0.0, worst_pc_true = 0.0;
  std::size_t ok = 0;
  for (std::size_t i = 0; i < c.size(); ++i) {
    EXPECT_EQ(pc.status[i], ls.status[i]) << i;
    if (pc.status[i] != ChainRowStatus::Ok) {
      continue;
    }
    ++ok;
    worst_true = std::max(worst_true, std::abs(ls.iv[i] - 1.05 * c.sigma[i]));
    worst_pc = std::max(worst_pc, std::abs(ls.iv[i] - pc.iv[i]));
    worst_pc_true = std::max(worst_pc_true, std::abs(pc.iv[i] - 1.05 * c.sigma[i]));
  }
  std::printf("[lockstep-iv] ok %zu/%zu max|lockstep - true| %.3e max|lockstep - per-contract| "
              "%.3e max|per-contract - true| %.3e\n",
              ok, c.size(), worst_true, worst_pc, worst_pc_true);
  EXPECT_EQ(ok, c.size());
  // american_implied_vol stops on a 1e-7 sigma bracket with a warm search map
  // (measured 5.2e-7 here); Lockstep converges on the laned cold map in price units.
  EXPECT_LE(worst_pc_true, 1.0e-6);
  EXPECT_LE(worst_true, 1.0e-7);
  EXPECT_LE(worst_pc, 1.0e-6);
}

TEST(ChainBatchIv, IllPosedAndOutOfBandTargetsAreTypedOnBothRoutes) {
  Chain c = make_chain(3);
  std::vector<double> target = targets_at(c, 1.0);
  // Contract 0 / 1: deep ITM put at exactly intrinsic (VegaFloor), and below it.
  c.strike[0] = 160.0;
  c.side[0] = Side::Put;
  target[0] = 60.0;
  c.strike[1] = 160.0;
  c.side[1] = Side::Put;
  target[1] = 59.0;
  ASSERT_EQ(c.side[2], Side::Call);
  target[2] = c.spot + 1.0; // a call above its upper bound S
  target[3] = kNaN;         // no target
  ASSERT_EQ(c.side[4], Side::Call);
  ASSERT_GT(c.strike[4], c.spot);
  target[4] = 0.0;   // an OTM call target at zero intrinsic
  c.sigma[5] = 0.0;  // no seed
  for (const ChainIvRoute route : {ChainIvRoute::PerContract, ChainIvRoute::Lockstep}) {
    ChainIvOptions o;
    o.route = route;
    const IvOuts got = run_iv(c, target, o);
    EXPECT_EQ(got.status[0], ChainRowStatus::VegaFloor);
    EXPECT_EQ(got.status[1], ChainRowStatus::OutOfBand);
    EXPECT_EQ(got.status[2], ChainRowStatus::OutOfBand);
    EXPECT_EQ(got.status[3], ChainRowStatus::InvalidInput);
    EXPECT_EQ(got.status[4], ChainRowStatus::VegaFloor);
    EXPECT_EQ(got.status[5], ChainRowStatus::InvalidInput);
    for (std::size_t i = 0; i < 6u; ++i) {
      EXPECT_TRUE(std::isnan(got.iv[i])) << i;
    }
    EXPECT_EQ(got.status[6], ChainRowStatus::Ok);
  }
}

TEST(ChainBatchIv, RejectsBadOptionsAndShapes) {
  const Chain c = make_chain(3);
  const std::vector<double> target = targets_at(c, 1.0);
  IvOuts o(c.size());
  ChainBatchWorkspace ws;
  ChainIvOptions bad;
  bad.sigma_tol = 0.0;
  EXPECT_FALSE(
      implied_vol_chain_batch(c.spot, c.exp(), c.con(), target, o.iv, o.status, bad, ws));
  bad = {};
  bad.max_iter = 0;
  EXPECT_FALSE(
      implied_vol_chain_batch(c.spot, c.exp(), c.con(), target, o.iv, o.status, bad, ws));
  EXPECT_FALSE(implied_vol_chain_batch(c.spot, c.exp(), c.con(),
                                       std::span<const double>(target).first(2), o.iv, o.status,
                                       {}, ws));
  EXPECT_FALSE(implied_vol_chain_batch(c.spot, c.exp(), c.con(), target,
                                       std::span<double>(o.iv).first(2), o.status, {}, ws));
  EXPECT_TRUE(std::all_of(o.iv.begin(), o.iv.end(), [](double v) { return v == -7.0; }));
}

TEST(ChainBatchIv, OutputsAreIdenticalForEveryThreadCount) {
  const Chain c = make_chain(9);
  const std::vector<double> target = targets_at(c, 0.97);
  for (const ChainIvRoute route : {ChainIvRoute::PerContract, ChainIvRoute::Lockstep}) {
    ChainIvOptions o;
    o.route = route;
    const IvOuts base = run_iv(c, target, o);
    for (const unsigned nt : {1u, 2u, 3u, 4u, 0u}) {
      ChainIvOptions fan = o;
      fan.executor = &pricing_executor();
      fan.n_threads = nt;
      const IvOuts got = run_iv(c, target, fan);
      EXPECT_TRUE(same_bits(base.iv, got.iv)) << static_cast<int>(route) << " nt=" << nt;
      EXPECT_EQ(base.status, got.status);
    }
  }
}

// ── Implied vol: the shared refusal rule (review fe6a516f, fix round 1) ─────

// dev200 TSM row 1295 (K = 185 call, seed 0.30, target = AL(0.309)): the whole time
// value above the zero-vol American value is 8.5e-8 USD, inside the 4.4e-7 band.
// Lockstep used to return Ok 0.306194 on the residual alone (2.8e-3 off the root).
TEST(ChainBatchIv, ZeroVolBandTargetIsVegaFloorOnBothRoutes) {
  const double S = 436.80999755859375;
  const double T = 0.25411561131477356;
  const double r = 0.046193607151508331;
  const double carry = -0.0084914956241846102;
  const double D = 1.0776369571685791;
  const Chain c = one_expiry(S, T, r, carry, D, {185.0}, {Side::Call}, 0.3);
  const double q = chain_effective_carry(S, T, carry, D);
  const double target = al_ref(S, 185.0, T, 0.309, r, q, Side::Call);
  const double zv = zero_vol_ref(S, 185.0, T, r, q, Side::Call);
  const double band = band_ref(target, ceiling_ref(S, 185.0, T, r, q, Side::Call), {});
  ASSERT_GT(target - zv, 1.0e-8); // above the zero-vol value ...
  ASSERT_LT(target - zv, band);   // ... but inside its band
  ASSERT_GT(target, S - 185.0 + 1.0e-9 * 185.0); // and above the old intrinsic screen
  for (const ChainIvRoute route : {ChainIvRoute::PerContract, ChainIvRoute::Lockstep}) {
    const std::vector<double> t{target};
    const IvOuts got = run_iv(c, t, iv_route(route));
    EXPECT_EQ(got.status[0], ChainRowStatus::VegaFloor) << static_cast<int>(route);
    EXPECT_TRUE(std::isnan(got.iv[0]));
  }
}

// Deep-ITM put S=50 K=100 T=1 r=.05 and call S=100 K=40 q=.08, seed 0.3, targets just
// above the zero-vol band inside the flat exercise interval (P = intrinsic for every
// sigma up to the edge, 0.493 / 0.703). PerContract used to return Ok 0.300001 at
// intrinsic + 2e-7 (0.193 vol error), 0.468540 and 0.673506 at + 1e-6.
TEST(ChainBatchIv, FlatExerciseIntervalTargetsNeverReturnAWrongVol) {
  struct Case {
    double S, K, r, q;
    Side sd;
  };
  for (const Case k : {Case{50.0, 100.0, 0.05, 0.0, Side::Put},
                       Case{100.0, 40.0, 0.05, 0.08, Side::Call}}) {
    const Chain c = one_expiry(k.S, 1.0, k.r, k.q, 0.0, {k.K}, {k.sd}, 0.3);
    const double intr = k.sd == Side::Put ? k.K - k.S : k.S - k.K;
    for (const double dt : {2.0e-7, 1.0e-6, 1.0e-4, 1.0e-2}) {
      const std::vector<double> t{intr + dt};
      const double root = bisect_root(t[0], k.S, k.K, 1.0, k.r, k.q, k.sd);
      ASSERT_TRUE(std::isfinite(root));
      for (const ChainIvRoute route : {ChainIvRoute::PerContract, ChainIvRoute::Lockstep}) {
        const IvOuts got = run_iv(c, t, iv_route(route));
        if (got.status[0] == ChainRowStatus::Ok) {
          EXPECT_NEAR(got.iv[0], root, 1.0e-6)
              << "route " << static_cast<int>(route) << " K=" << k.K << " +" << dt;
        } else {
          EXPECT_EQ(got.status[0], ChainRowStatus::VegaFloor) << static_cast<int>(route);
        }
        if (route == ChainIvRoute::PerContract && dt <= 1.0e-6) {
          // american_implied_vol stops inside the flat interval: the verification
          // finds a flat chord there and refuses.
          EXPECT_EQ(got.status[0], ChainRowStatus::VegaFloor) << "K=" << k.K << " +" << dt;
        }
      }
    }
    // A loose (valid) price_tol: Lockstep used to accept the seed 0.300000 on its
    // first step through the Black-76 slope. The target is now within price_tol of
    // the zero-vol value, so both routes refuse.
    ChainIvOptions loose;
    loose.price_tol = 1.0e-6;
    for (const double dt : {1.0e-6, 1.0e-5, 4.0e-5}) {
      const std::vector<double> t{intr + dt};
      for (const ChainIvRoute route : {ChainIvRoute::PerContract, ChainIvRoute::Lockstep}) {
        ChainIvOptions o = loose;
        o.route = route;
        const IvOuts got = run_iv(c, t, o);
        EXPECT_EQ(got.status[0], ChainRowStatus::VegaFloor)
            << "route " << static_cast<int>(route) << " K=" << k.K << " +" << dt;
      }
    }
  }
}

// A loose sigma_tol makes the Lockstep bracket close long before the residual is
// met; Ok must still mean the price is reproduced (it used to accept the bracket
// alone, 1e-2-scale vol errors).
TEST(ChainBatchIv, BracketWidthAloneNeverAcceptsARoot) {
  const Chain c = make_chain();
  const std::vector<double> target = targets_at(c, 1.05);
  for (const ChainIvRoute route : {ChainIvRoute::PerContract, ChainIvRoute::Lockstep}) {
    ChainIvOptions o = iv_route(route);
    o.sigma_tol = 0.25;
    const IvOuts got = run_iv(c, target, o);
    std::size_t ok = 0;
    for (std::size_t i = 0; i < c.size(); ++i) {
      if (got.status[i] != ChainRowStatus::Ok) {
        continue;
      }
      ++ok;
      double q = 0.0;
      ASSERT_TRUE(row_valid(c, i, &q));
      const std::uint32_t e = c.expiry[i];
      const double p = al_ref(c.spot, c.strike[i], c.T[e], got.iv[i], c.rate[e], q, c.side[i]);
      // The laned map matches andersen_lake to ~1e-13 (kernel gate), hence the slack.
      EXPECT_LE(std::abs(p - target[i]), o.price_tol * std::max(1.0, target[i]) + 1.0e-11)
          << "route " << static_cast<int>(route) << " i=" << i;
      EXPECT_NEAR(got.iv[i], 1.05 * c.sigma[i], 1.0e-6) << static_cast<int>(route) << " i=" << i;
    }
    EXPECT_EQ(ok, c.size()) << static_cast<int>(route);
  }
}

// Targets around the zero-vol value: inside the band both routes refuse
// (VegaFloor), below it both refuse (OutOfBand). Just above it they agree except in
// one documented case: an ITM row in the flat exercise interval, where
// american_implied_vol stops at a sigma whose chord is flat (PerContract refuses,
// VegaFloor) while the Lockstep bracket walks on to the root (Ok, checked against
// a bisection root).
TEST(ChainBatchIv, RoutesAgreeOnNearZeroVolTargets) {
  const Chain c = make_chain();
  std::size_t compared = 0;
  std::size_t flat_scalar = 0;
  std::size_t multi_root = 0;
  for (const double m : {-3.0, -0.9, -0.5, 0.0, 0.5, 0.9, 3.0, 30.0}) {
    std::vector<double> t(c.size(), kNaN);
    for (std::size_t i = 0; i < c.size(); ++i) {
      double q = 0.0;
      ASSERT_TRUE(row_valid(c, i, &q));
      const std::uint32_t e = c.expiry[i];
      const double zv = zero_vol_ref(c.spot, c.strike[i], c.T[e], c.rate[e], q, c.side[i]);
      const double band =
          band_ref(zv, ceiling_ref(c.spot, c.strike[i], c.T[e], c.rate[e], q, c.side[i]), {});
      t[i] = zv + m * band;
    }
    const IvOuts pc = run_iv(c, t, iv_route(ChainIvRoute::PerContract));
    const IvOuts ls = run_iv(c, t, iv_route(ChainIvRoute::Lockstep));
    for (std::size_t i = 0; i < c.size(); ++i) {
      ++compared;
      if (m > 1.0 && pc.status[i] == ChainRowStatus::VegaFloor &&
          ls.status[i] == ChainRowStatus::Ok) {
        double q = 0.0;
        ASSERT_TRUE(row_valid(c, i, &q));
        const std::uint32_t e = c.expiry[i];
        const double S = c.spot, K = c.strike[i], T = c.T[e], r = c.rate[e];
        const Result<double> x = american_implied_vol(t[i], S, K, T, r, q, c.side[i],
                                                      AmericanMethod::AndersenLake, 1.0e-7, 64,
                                                      std::nullopt, nullptr, c.sigma[i], 0.0);
        ASSERT_TRUE(x.has_value()) << "m=" << m << " i=" << i;
        const double xb = *x * (1.0 - 1.0e-4);
        const double chord = (al_ref(S, K, T, *x, r, q, c.side[i]) -
                              al_ref(S, K, T, xb, r, q, c.side[i])) /
                             (*x - xb);
        EXPECT_LE(chord, 1.0e-8 * K) << "m=" << m << " i=" << i; // the scalar stopped flat
        EXPECT_LE(std::abs(al_ref(S, K, T, ls.iv[i], r, q, c.side[i]) - t[i]),
                  1.0e-10 * std::max(1.0, t[i]) + 1.0e-11)
            << "m=" << m << " i=" << i; // Lockstep's answer is a root of the cold map
        ++flat_scalar;
        continue;
      }
      EXPECT_EQ(pc.status[i], ls.status[i]) << "m=" << m << " i=" << i;
      if (std::abs(m) < 1.0) {
        EXPECT_EQ(pc.status[i], ChainRowStatus::VegaFloor) << "m=" << m << " i=" << i;
      }
      if (m <= -3.0 && t[i] >= 0.0) {
        EXPECT_EQ(pc.status[i], ChainRowStatus::OutOfBand) << "m=" << m << " i=" << i;
      }
      if (pc.status[i] == ChainRowStatus::Ok && ls.status[i] == ChainRowStatus::Ok &&
          std::abs(pc.iv[i] - ls.iv[i]) > 1.0e-6) {
        // Two different Ok roots: legal only where the cold map is not monotone in
        // sigma (an Andersen-Lake artifact on long-dated deep-ITM puts: P rises up
        // to ~8e-6 USD above intrinsic below the flat edge, then falls back), so it
        // must dip below the target between them. Both must reproduce the target.
        double q = 0.0;
        ASSERT_TRUE(row_valid(c, i, &q));
        const std::uint32_t e = c.expiry[i];
        const double S = c.spot, K = c.strike[i], T = c.T[e], r = c.rate[e];
        const double lo = std::min(pc.iv[i], ls.iv[i]);
        const double hi = std::max(pc.iv[i], ls.iv[i]);
        double dip = std::numeric_limits<double>::infinity();
        for (int g = 1; g < 64; ++g) {
          dip = std::min(dip, al_ref(S, K, T, lo + (hi - lo) * g / 64.0, r, q, c.side[i]));
        }
        EXPECT_LT(dip, t[i] - 1.0e-10) << "m=" << m << " i=" << i;
        const double ptol = 1.0e-10 * std::max(1.0, t[i]) + 1.0e-11;
        EXPECT_LE(std::abs(al_ref(S, K, T, pc.iv[i], r, q, c.side[i]) - t[i]), ptol);
        EXPECT_LE(std::abs(al_ref(S, K, T, ls.iv[i], r, q, c.side[i]) - t[i]), ptol);
        ++multi_root;
      }
    }
  }
  std::printf("[near-zero-vol] %zu rows compared, %zu PerContract VegaFloor / Lockstep Ok "
              "where the scalar stopped on a flat chord, %zu Ok on both at different roots of "
              "a non-monotone map\n",
              compared, flat_scalar, multi_root);
  EXPECT_EQ(compared, 8u * c.size());
}

// A slice of the dev200 TSM chain: the expiry of row 1295 (calls whose zero-vol
// value, the discounted forward intrinsic, sits above the immediate intrinsic) plus
// row 27's expiry, both sides. The routes used to disagree on K = 185.
TEST(ChainBatchIv, Dev200TsmSliceRefusalsAgreeAcrossRoutes) {
  Chain c;
  c.spot = 436.80999755859375;
  c.T = {0.25411561131477356, 0.0093978382647037506};
  c.rate = {0.046193607151508331, 0.044896867126226432};
  c.carry = {-0.0084914956241846102, -0.00115431449376047};
  c.div_pv = {1.0776369571685791, 0.0};
  const std::vector<double> k0 = {85.0,  100.0, 105.0, 110.0, 115.0, 120.0, 125.0, 130.0, 135.0,
                                  140.0, 145.0, 150.0, 155.0, 160.0, 165.0, 170.0, 175.0, 180.0,
                                  185.0, 190.0, 200.0, 250.0, 300.0, 400.0, 430.0, 440.0, 500.0};
  for (const double K : k0) {
    for (const Side sd : {Side::Call, Side::Put}) {
      c.strike.push_back(K);
      c.side.push_back(sd);
      c.sigma.push_back(0.3);
      c.expiry.push_back(0u);
    }
  }
  for (const double K : {260.0, 400.0, 435.0, 440.0}) {
    for (const Side sd : {Side::Call, Side::Put}) {
      c.strike.push_back(K);
      c.side.push_back(sd);
      c.sigma.push_back(0.3);
      c.expiry.push_back(1u);
    }
  }
  const std::vector<double> target = targets_at(c, 1.03); // AL(0.309), as the bench
  const IvOuts pc = run_iv(c, target, iv_route(ChainIvRoute::PerContract));
  const IvOuts ls = run_iv(c, target, iv_route(ChainIvRoute::Lockstep));
  std::size_t disagree = 0;
  std::size_t in_band = 0;
  for (std::size_t i = 0; i < c.size(); ++i) {
    disagree += pc.status[i] != ls.status[i] ? 1u : 0u;
    EXPECT_EQ(pc.status[i], ls.status[i]) << "i=" << i << " K=" << c.strike[i];
    double q = 0.0;
    ASSERT_TRUE(row_valid(c, i, &q));
    const std::uint32_t e = c.expiry[i];
    const double zv = zero_vol_ref(c.spot, c.strike[i], c.T[e], c.rate[e], q, c.side[i]);
    const double band = band_ref(
        target[i], ceiling_ref(c.spot, c.strike[i], c.T[e], c.rate[e], q, c.side[i]), {});
    if (std::abs(target[i] - zv) <= band) {
      ++in_band;
      EXPECT_EQ(pc.status[i], ChainRowStatus::VegaFloor) << "i=" << i;
    }
    if (pc.status[i] == ChainRowStatus::Ok && ls.status[i] == ChainRowStatus::Ok) {
      EXPECT_NEAR(pc.iv[i], 0.309, 1.0e-6) << "i=" << i;
      EXPECT_NEAR(ls.iv[i], 0.309, 1.0e-6) << "i=" << i;
    }
  }
  std::printf("[dev200-slice] %zu rows, %zu in the zero-vol band, %zu status disagreements\n",
              c.size(), in_band, disagree);
  EXPECT_GE(in_band, 16u);
  EXPECT_EQ(disagree, 0u);
}

// ── Implied vol: the non-monotone Andersen-Lake region (L1c, 2026-10-19) ─────
//
// Rule 3 of "One implied-vol refusal rule" (opt-in ChainIvOptions::refuse_non_monotone)
// and the deferred batch-pricing minor N2 (PerContract's 4-step polish runs out on
// long-dated deep-ITM puts that Lockstep solves). Region map and gate constants:
// sprints/2026-10-18-L1c-al-nonmonotone-sprint.md.

// Three long-dated deep-ITM put expiries: the ledgered contract (LEDGER 2026-10-17
// trap: S=100 K=128.989 T=2 r=.05 q=.015) and the scalar trap line's K=140 T=5 r=.05
// q=0 and K=130 T=5 r=q=.05. Rows 0-2: one ambiguous target (zero-vol value + 5e-6,
// three roots: ~0.091 and ~0.105 in the kernel's exercise region, ~0.184 outside it)
// from three seeds; rows 3-5: targets AL(0.14), AL(0.2), AL(0.1); row 6: AL(0.3).
struct NonMonotoneChain {
  Chain c;
  std::vector<double> target;
};

[[nodiscard]] NonMonotoneChain non_monotone_chain() {
  NonMonotoneChain n;
  Chain &c = n.c;
  c.spot = 100.0;
  c.T = {2.0, 5.0, 5.0};
  c.rate = {0.05, 0.05, 0.05};
  c.carry = {0.015, 0.0, 0.05};
  c.div_pv = {0.0, 0.0, 0.0};
  const auto add = [&](std::uint32_t e, double K, double seed, double target) {
    c.strike.push_back(K);
    c.side.push_back(Side::Put);
    c.sigma.push_back(seed);
    c.expiry.push_back(e);
    n.target.push_back(target);
  };
  const auto al = [&](std::uint32_t e, double K, double sig) {
    return al_ref(c.spot, K, c.T[e], sig, c.rate[e], c.carry[e], Side::Put);
  };
  const double bump = zero_vol_ref(100.0, 128.989, 2.0, 0.05, 0.015, Side::Put) + 5.0e-6;
  add(0u, 128.989, 0.30, bump);
  add(0u, 128.989, 0.16, bump);
  add(0u, 128.989, 0.15, bump);
  add(0u, 128.989, 0.30, al(0u, 128.989, 0.14));
  add(1u, 140.0, 0.30, al(1u, 140.0, 0.2));
  add(2u, 130.0, 0.30, al(2u, 130.0, 0.1));
  add(0u, 128.989, 0.30, al(0u, 128.989, 0.3));
  return n;
}

[[nodiscard]] ChainIvOptions refusing(ChainIvRoute route) {
  ChainIvOptions o = iv_route(route);
  o.refuse_non_monotone = true;
  return o;
}

[[nodiscard]] bool is_cold_root(const Chain &c, std::size_t i, double target, double x) {
  const std::uint32_t e = c.expiry[i];
  const double p = al_ref(c.spot, c.strike[i], c.T[e], x, c.rate[e], c.carry[e], c.side[i]);
  return std::abs(p - target) <= 1.0e-10 * std::max(1.0, target) + 1.0e-11;
}

// Flag on vs flag off: every row is bit-identical except the rows the flag turns
// into NonMonotoneRegion, and those had a target inside al_non_monotone_gate.
// Returns the number of refused rows.
[[nodiscard]] std::size_t expect_flag_only_refuses(const Chain &c, std::span<const double> target,
                                                   ChainIvRoute route) {
  const IvOuts off = run_iv(c, target, iv_route(route));
  const IvOuts on = run_iv(c, target, refusing(route));
  std::size_t refused = 0;
  for (std::size_t i = 0; i < c.size(); ++i) {
    EXPECT_NE(off.status[i], ChainRowStatus::NonMonotoneRegion) << i;
    if (on.status[i] == ChainRowStatus::NonMonotoneRegion) {
      ++refused;
      EXPECT_TRUE(std::isnan(on.iv[i])) << i;
      double q = 0.0;
      EXPECT_TRUE(row_valid(c, i, &q));
      const std::uint32_t e = c.expiry[i];
      const double zv = zero_vol_ref(c.spot, c.strike[i], c.T[e], c.rate[e], q, c.side[i]);
      EXPECT_LE(target[i] - zv, al_non_monotone_gate(c.strike[i], c.T[e], std::nullopt))
          << "route " << static_cast<int>(route) << " i=" << i;
      continue;
    }
    EXPECT_EQ(on.status[i], off.status[i]) << "route " << static_cast<int>(route) << " i=" << i;
    EXPECT_TRUE(same_bits(std::span<const double>(&on.iv[i], 1),
                          std::span<const double>(&off.iv[i], 1)))
        << "route " << static_cast<int>(route) << " i=" << i;
  }
  return refused;
}

// N2 (batch-pricing re-review MINOR-2, untested until now): PerContract verifies
// american_implied_vol's answer on the cold map and polishes it with at most 4 secant
// steps; on these long-dated deep-ITM puts the scalar stops 0.02 vol off (row 4:
// 0.219 for 0.2) on a map that is flat-then-steep there, the polish runs out and the
// row is NoConvergence, while Lockstep's bracket walks to a root of the cold map.
TEST(ChainBatchIv, PerContractPolishRunsOutWhereLockstepSolves) {
  const NonMonotoneChain n = non_monotone_chain();
  const IvOuts pc = run_iv(n.c, n.target, iv_route(ChainIvRoute::PerContract));
  const IvOuts ls = run_iv(n.c, n.target, iv_route(ChainIvRoute::Lockstep));
  for (const std::size_t i : {std::size_t{3}, std::size_t{4}}) {
    EXPECT_EQ(pc.status[i], ChainRowStatus::NoConvergence) << i;
    EXPECT_TRUE(std::isnan(pc.iv[i])) << i;
    ASSERT_EQ(ls.status[i], ChainRowStatus::Ok) << i;
    EXPECT_TRUE(is_cold_root(n.c, i, n.target[i], ls.iv[i])) << i;
  }
  // Row 4 is AL(0.2): Lockstep returns the generating vol. Row 3 is AL(0.14), the peak
  // of the ledgered dip: Lockstep returns the root past the exercise edge instead.
  EXPECT_NEAR(ls.iv[4], 0.2, 1.0e-6);
  EXPECT_NEAR(ls.iv[3], 0.1839973, 1.0e-6);
  // The scalar answer PerContract starts its polish from (warm-started at the seed).
  const Result<double> x = american_implied_vol(n.target[4], 100.0, 140.0, 5.0, 0.05, 0.0,
                                                Side::Put, AmericanMethod::AndersenLake, 1.0e-7,
                                                64, std::nullopt, nullptr, 0.3, 0.0);
  ASSERT_TRUE(x.has_value());
  EXPECT_GT(*x - 0.2, 0.01);
  EXPECT_FALSE(is_cold_root(n.c, 4, n.target[4], *x));
}

// Rule 3 surfaces on both routes, through both of its triggers: an Ok vol inside
// al_non_monotone_region (PerContract row 0, Lockstep row 4) and a resolved falling
// chord (row 1: PerContract's verification chord, Lockstep's iterate chord; the
// Lockstep flag-off answer there is outside the exercise region, so only the chord
// can refuse it). Flag off, the ambiguous rows 0-2 return three different Ok roots
// across routes and seeds; that is what the flag refuses.
TEST(ChainBatchIv, NonMonotoneRegionSurfacesOnBothRoutesWhenOptedIn) {
  using St = ChainRowStatus;
  const NonMonotoneChain n = non_monotone_chain();
  const IvOuts pc_off = run_iv(n.c, n.target, iv_route(ChainIvRoute::PerContract));
  const IvOuts ls_off = run_iv(n.c, n.target, iv_route(ChainIvRoute::Lockstep));
  const IvOuts pc_on = run_iv(n.c, n.target, refusing(ChainIvRoute::PerContract));
  const IvOuts ls_on = run_iv(n.c, n.target, refusing(ChainIvRoute::Lockstep));
  const std::vector<St> want_pc_off{St::Ok,        St::VegaFloor, St::Ok, St::NoConvergence,
                                    St::NoConvergence, St::VegaFloor, St::Ok};
  const std::vector<St> want_ls_off{St::Ok, St::Ok, St::Ok, St::Ok, St::Ok, St::Ok, St::Ok};
  const std::vector<St> want_pc_on{St::NonMonotoneRegion, St::NonMonotoneRegion,
                                   St::NonMonotoneRegion, St::NoConvergence,
                                   St::NoConvergence,     St::NonMonotoneRegion,
                                   St::Ok};
  const std::vector<St> want_ls_on{St::Ok,
                                   St::NonMonotoneRegion,
                                   St::NonMonotoneRegion,
                                   St::Ok,
                                   St::NonMonotoneRegion,
                                   St::NonMonotoneRegion,
                                   St::Ok};
  EXPECT_EQ(pc_off.status, want_pc_off);
  EXPECT_EQ(ls_off.status, want_ls_off);
  EXPECT_EQ(pc_on.status, want_pc_on);
  EXPECT_EQ(ls_on.status, want_ls_on);
  // Flag off, the ambiguous target has Ok answers ~0.079 vol apart; all are roots.
  EXPECT_NEAR(pc_off.iv[0], 0.1046251, 1.0e-6);
  EXPECT_NEAR(ls_off.iv[0], 0.1839836, 1.0e-6);
  for (std::size_t i = 0; i < n.c.size(); ++i) {
    if (ls_off.status[i] == St::Ok) {
      EXPECT_TRUE(is_cold_root(n.c, i, n.target[i], ls_off.iv[i])) << i;
    }
  }
  // Rows the flag keeps are bit-identical (rows 0 and 3 on Lockstep: roots past the
  // exercise edge; row 6: well posed).
  EXPECT_TRUE(same_bits(std::span<const double>(&ls_on.iv[0], 1),
                        std::span<const double>(&ls_off.iv[0], 1)));
  EXPECT_TRUE(same_bits(std::span<const double>(&ls_on.iv[3], 1),
                        std::span<const double>(&ls_off.iv[3], 1)));
  EXPECT_NEAR(pc_on.iv[6], 0.3, 1.0e-6);
  EXPECT_EQ(expect_flag_only_refuses(n.c, n.target, ChainIvRoute::PerContract), 4u);
  EXPECT_EQ(expect_flag_only_refuses(n.c, n.target, ChainIvRoute::Lockstep), 4u);
}

// Well-posed chains (the 36-row test chain at two vol bumps) never meet the region:
// with the flag on both routes are bit-identical to the flag off. Targets near the
// zero-vol value of the same chain (its T = 2 deep-ITM puts are the ledgered
// contract's neighbours) only ever change rows into NonMonotoneRegion, inside the gate.
TEST(ChainBatchIv, NonMonotoneFlagOnlyEverTurnsRowsIntoRefusals) {
  const Chain c = make_chain(9);
  for (const double bump : {0.97, 1.05}) {
    const std::vector<double> t = targets_at(c, bump);
    for (const ChainIvRoute route : {ChainIvRoute::PerContract, ChainIvRoute::Lockstep}) {
      EXPECT_EQ(expect_flag_only_refuses(c, t, route), 0u)
          << "bump " << bump << " route " << static_cast<int>(route);
    }
  }
  std::size_t refused_pc = 0;
  std::size_t refused_ls = 0;
  for (const double f : {0.01, 0.1, 0.5, 2.0}) {
    std::vector<double> t(c.size(), kNaN);
    for (std::size_t i = 0; i < c.size(); ++i) {
      double q = 0.0;
      ASSERT_TRUE(row_valid(c, i, &q));
      const std::uint32_t e = c.expiry[i];
      const double zv = zero_vol_ref(c.spot, c.strike[i], c.T[e], c.rate[e], q, c.side[i]);
      t[i] = zv + f * al_non_monotone_gate(c.strike[i], c.T[e], std::nullopt);
    }
    refused_pc += expect_flag_only_refuses(c, t, ChainIvRoute::PerContract);
    refused_ls += expect_flag_only_refuses(c, t, ChainIvRoute::Lockstep);
  }
  std::printf("[non-monotone chain] near-zero-vol targets refused: PerContract %zu, Lockstep "
              "%zu of %zu\n",
              refused_pc, refused_ls, 4u * c.size());
  EXPECT_GT(refused_pc, 0u);
  EXPECT_GT(refused_ls, 0u);
}

// dev200 kernel-input rows 9100-9118 (calls, S = 152.32, T = 0.254, K 60-75, sigma
// 0.83-0.93, seed 0.3; targets 0.43-0.63 USD above the zero-vol value, ~1,000x the
// gate). Lockstep's first iterates pass through the low-vol dip of these deep-ITM
// calls on the way to the one root, so an ungated falling chord refused K = 68-74
// (resume 2026-10-19 count run). Rule 3 never refuses a target above the band.
TEST(ChainBatchIv, NonMonotoneFlagNeverRefusesTargetsAboveTheBand) {
  const double S = 152.31999969482422;
  const double T = 0.25411561131477356;
  const double r = 0.046193607151508331;
  const double q = 0.031131176216856259;
  const std::vector<double> strikes{60.0, 65.0, 68.0, 69.0, 70.0, 71.0, 72.0, 73.0, 74.0, 75.0};
  const std::vector<double> sig{0.93070864145646215, 0.89576923672585029, 0.87547672771706431,
                                0.86881270020774737, 0.86219580086613001, 0.85562436194740155,
                                0.84909677596540301, 0.84261149239938049, 0.83616701462566434,
                                0.82976189705743242};
  const Chain c = one_expiry(S, T, r, q, 0.0, strikes,
                             std::vector<Side>(strikes.size(), Side::Call), 0.3);
  std::vector<double> t(c.size(), kNaN);
  for (std::size_t i = 0; i < c.size(); ++i) {
    t[i] = al_ref(S, strikes[i], T, sig[i], r, q, Side::Call);
    const double zv = zero_vol_ref(S, strikes[i], T, r, q, Side::Call);
    ASSERT_GT(t[i] - zv, 100.0 * al_non_monotone_gate(strikes[i], T, std::nullopt)) << i;
  }
  for (const ChainIvRoute route : {ChainIvRoute::PerContract, ChainIvRoute::Lockstep}) {
    EXPECT_EQ(expect_flag_only_refuses(c, t, route), 0u) << static_cast<int>(route);
    const IvOuts on = run_iv(c, t, refusing(route));
    for (std::size_t i = 0; i < c.size(); ++i) {
      ASSERT_EQ(on.status[i], ChainRowStatus::Ok) << "route " << static_cast<int>(route)
                                                  << " K=" << strikes[i];
      EXPECT_NEAR(on.iv[i], sig[i], 1.0e-6) << "route " << static_cast<int>(route) << " i=" << i;
    }
  }
}

// ── Workspace and allocation ─────────────────────────────────────────────────

TEST(ChainBatchWorkspace, ReservesGrowsAndNeverShrinks) {
  ChainBatchWorkspace ws;
  EXPECT_EQ(ws.expiry_capacity(), 0u);
  ws.reserve(8);
  EXPECT_EQ(ws.expiry_capacity(), 8u);
  ws.reserve(3);
  EXPECT_EQ(ws.expiry_capacity(), 8u);
  const Chain c = make_chain(3);
  ChainBatchWorkspace small;
  Outs o(c.size());
  ASSERT_TRUE(price_chain_batch(c.spot, c.exp(), c.con(), o.all(), {}, small).has_value());
  EXPECT_EQ(small.expiry_capacity(), c.T.size());
}

// A moved-from workspace used to dereference a null Impl in reserve(), impl() and
// expiry_capacity(); it is now empty and usable again.
TEST(ChainBatchWorkspace, MovedFromWorkspaceIsEmptyAndUsable) {
  const Chain c = make_chain(3);
  const std::vector<double> target = targets_at(c, 1.05);
  const Outs ref = run_batch(c, {});
  const IvOuts iv_ref = run_iv(c, target, {});
  ChainBatchWorkspace a;
  a.reserve(c.T.size());
  ChainBatchWorkspace b(std::move(a));
  EXPECT_EQ(b.expiry_capacity(), c.T.size());
  EXPECT_EQ(a.expiry_capacity(), 0u); // NOLINT(bugprone-use-after-move): the contract under test
  Outs o(c.size());
  ASSERT_TRUE(price_chain_batch(c.spot, c.exp(), c.con(), o.all(), {}, a).has_value());
  EXPECT_TRUE(same_outs(ref, o));
  EXPECT_EQ(a.expiry_capacity(), c.T.size());
  ChainBatchWorkspace d;
  d = std::move(a);
  IvOuts iv(c.size());
  ASSERT_TRUE(
      implied_vol_chain_batch(c.spot, c.exp(), c.con(), target, iv.iv, iv.status, {}, a));
  EXPECT_TRUE(same_bits(iv_ref.iv, iv.iv));
  EXPECT_EQ(iv_ref.status, iv.status);
  ChainBatchWorkspace e(std::move(d));
  d.reserve(2); // NOLINT(bugprone-use-after-move)
  EXPECT_EQ(d.expiry_capacity(), 2u);
}

TEST(ChainBatchAlloc, ScalarRouteAddsNoAllocationToTheKernelsOwn) {
  const Chain c = make_chain(3);
  ChainBatchWorkspace ws;
  ws.reserve(c.T.size());
  Outs o(c.size());
  ASSERT_TRUE(price_chain_batch(c.spot, c.exp(), c.con(), o.all(), {}, ws).has_value()); // warm
  const Outs warm = reference_loop(c, true);
  const std::uint64_t loop_allocs =
      testkit::count_allocations([&] { const Outs r = reference_loop(c, true); (void)r; });
  // reference_loop's own Outs vectors: 7 allocations.
  const std::uint64_t batch_allocs = testkit::count_allocations([&] {
    const Result<std::size_t> r = price_chain_batch(c.spot, c.exp(), c.con(), o.all(), {}, ws);
    EXPECT_TRUE(r.has_value());
  });
  std::printf("[alloc] per-contract loop %llu (7 are its output vectors), batch %llu\n",
              static_cast<unsigned long long>(loop_allocs),
              static_cast<unsigned long long>(batch_allocs));
  EXPECT_EQ(batch_allocs + 7u, loop_allocs);
  (void)warm;
}

TEST(ChainBatchAlloc, LockstepIvIsAllocationFreeOnAReservedWorkspace) {
  const Chain c = make_chain(3);
  const std::vector<double> target = targets_at(c, 1.05);
  ChainBatchWorkspace ws;
  ws.reserve(c.T.size());
  ChainIvOptions o;
  o.route = ChainIvRoute::Lockstep;
  IvOuts out(c.size());
  ASSERT_TRUE(
      implied_vol_chain_batch(c.spot, c.exp(), c.con(), target, out.iv, out.status, o, ws));
  const std::uint64_t n = testkit::count_allocations([&] {
    const Result<std::size_t> r =
        implied_vol_chain_batch(c.spot, c.exp(), c.con(), target, out.iv, out.status, o, ws);
    EXPECT_TRUE(r.has_value());
  });
  EXPECT_EQ(n, 0u);
}

// The thread-local counter above cannot see pool workers; this one counts every
// thread. Each execution context first runs all four routes inline once (the
// kernels keep per-thread state they build on first use, e.g. american_implied_vol's
// retained pricer), then every route is measured inline and fanned out.
TEST(ChainBatchAlloc, EveryRouteIsAllocationFreeOnEveryThreadOnceWarm) {
  PricingExecutor &ex = pricing_executor();
  const Chain c = make_chain(9); // 72 contracts: chunks of 32, 32, 8
  const std::vector<double> target = targets_at(c, 1.05);
  const auto run_all = [&](PricingExecutor *exec, ChainBatchWorkspace &ws, Outs &o, IvOuts &iv) {
    for (const ChainBatchRoute route : {ChainBatchRoute::Scalar, ChainBatchRoute::LanedAvx2}) {
      ChainBatchOptions opt;
      opt.route = route;
      opt.executor = exec;
      EXPECT_TRUE(price_chain_batch(c.spot, c.exp(), c.con(), o.all(), opt, ws).has_value());
    }
    for (const ChainIvRoute route : {ChainIvRoute::PerContract, ChainIvRoute::Lockstep}) {
      ChainIvOptions opt = iv_route(route);
      opt.executor = exec;
      EXPECT_TRUE(implied_vol_chain_batch(c.spot, c.exp(), c.con(), target, iv.iv, iv.status, opt,
                                          ws)
                      .has_value());
    }
  };
  // One warm-up per execution context: run_blocks gives every context >= 1 index.
  const std::size_t contexts = static_cast<std::size_t>(ex.size()) + 1u;
  ex.run_blocks(4u * contexts, 0u, [&](std::size_t) {
    ChainBatchWorkspace ws;
    Outs o(c.size());
    IvOuts iv(c.size());
    run_all(nullptr, ws, o, iv);
  });
  ChainBatchWorkspace ws;
  ws.reserve(c.T.size());
  Outs o(c.size());
  IvOuts iv(c.size());
  run_all(&ex, ws, o, iv);
  const std::uint64_t inline_n =
      testkit::count_process_allocations([&] { run_all(nullptr, ws, o, iv); });
  const std::uint64_t fanned_n =
      testkit::count_process_allocations([&] { run_all(&ex, ws, o, iv); });
  std::printf("[alloc] process-wide, 4 routes x 72 contracts: inline %llu, fanned out %llu "
              "(pool workers %u)\n",
              static_cast<unsigned long long>(inline_n),
              static_cast<unsigned long long>(fanned_n), ex.size());
  EXPECT_EQ(inline_n, 0u);
  EXPECT_EQ(fanned_n, 0u);
  EXPECT_TRUE(std::all_of(iv.status.begin(), iv.status.end(),
                          [](ChainRowStatus s) { return s == ChainRowStatus::Ok; }));
}

} // namespace
} // namespace atx::vol
