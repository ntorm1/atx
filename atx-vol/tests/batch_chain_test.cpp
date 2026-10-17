// Chain-level batch price / Greeks / implied vol (api/pricing/batch_chain.hpp).
//
// The reference throughout is an independently written per-contract loop: the
// kernel call a caller would make for each contract at the expiry's effective carry.
// The Scalar and PerContract routes must reproduce it bit for bit; the opt-in
// LanedAvx2 and Lockstep routes must stay within their stated bounds; every route
// must give the same bits for every thread count.

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <optional>
#include <span>
#include <vector>

#include <gtest/gtest.h>

#include "atx/vol/api/pricing/american.hpp"
#include "atx/vol/api/pricing/american_iv.hpp"
#include "atx/vol/api/pricing/batch_chain.hpp"
#include "atx/vol/api/pricing/pricing_executor.hpp"
#include "support/alloc_counter.hpp"

namespace atx::vol {
namespace {

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

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

// The per-contract IV loop the PerContract route must reproduce.
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
    const bool call = c.side[i] == Side::Call;
    const double intr = call ? std::max(c.spot - K, 0.0) : std::max(K - c.spot, 0.0);
    const double band = o.intrinsic_tol * K;
    if (target[i] < intr - band || target[i] >= (call ? c.spot : K)) {
      put(ChainRowStatus::OutOfBand, kNaN);
      continue;
    }
    if (target[i] <= intr + band) {
      put(ChainRowStatus::VegaFloor, kNaN);
      continue;
    }
    const std::uint32_t e = c.expiry[i];
    const Result<double> iv = american_implied_vol(target[i], c.spot, K, c.T[e], c.rate[e], q,
                                                   c.side[i], AmericanMethod::AndersenLake,
                                                   o.sigma_tol, o.max_iter, o.al_opts, nullptr,
                                                   c.sigma[i], 0.0);
    if (iv) {
      *iv > kIvMin ? put(ChainRowStatus::Ok, *iv) : put(ChainRowStatus::VegaFloor, kNaN);
    } else if (iv.error().code() == ErrorCode::OutOfRange) {
      put(ChainRowStatus::OutOfBand, kNaN);
    } else if (iv.error().code() == ErrorCode::Unavailable) {
      put(ChainRowStatus::NoConvergence, kNaN);
    } else {
      put(ChainRowStatus::Unpriceable, kNaN);
    }
  }
  return out;
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
    for (const unsigned nt : {2u, 3u, 0u}) {
      ChainIvOptions fan = o;
      fan.executor = &pricing_executor();
      fan.n_threads = nt;
      const IvOuts got = run_iv(c, target, fan);
      EXPECT_TRUE(same_bits(base.iv, got.iv)) << static_cast<int>(route) << " nt=" << nt;
      EXPECT_EQ(base.status, got.status);
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

} // namespace
} // namespace atx::vol
