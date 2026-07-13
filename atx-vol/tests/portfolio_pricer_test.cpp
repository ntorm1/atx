// PortfolioPricer synthetic suite — prices a multi-underlying, multi-kind book
// against PricedSurfaces and validates the Taylor PnL-explain decomposition.
//
// Coverage:
//   * price() rows are bit-identical to direct PricedSurface::greeks (qty*mult
//     scaled), iv matches, totals equal the column sums;
//   * contract dedup (many positions, fewer unique contracts) prices once;
//   * a missing-uid position is ModelUnavailable; a degenerate contract Invalid;
//   * price()/pnl_explain() output is bit-identical across thread counts;
//   * pnl_explain isolates each state axis: a spot-only / rate-only / vol-only /
//     time-only shift lights up exactly the matching Taylor term (coefficient ==
//     base Greek * state move) and leaves the inactive axes exactly zero; the
//     eight components + unexplained sum to the full reprice; a small move is
//     reconstructed to a tight residual.

#include <gtest/gtest.h>

#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <utility>
#include <vector>

#include "atx/vol/black76.hpp"
#include "atx/vol/counters.hpp"
#include "atx/vol/portfolio_pricer.hpp"
#include "atx/vol/priced_surface.hpp"
#include "atx/vol/vol_curve.hpp"
#include "atx/vol/vol_surface.hpp"

using namespace atx::vol;

namespace {

constexpr double kS = 100.0;
constexpr double kR = 0.043;
constexpr std::int64_t kNow = 1700000000000000000LL;

[[nodiscard]] bool bits_equal(double a, double b) noexcept {
  std::uint64_t ba = 0;
  std::uint64_t bb = 0;
  std::memcpy(&ba, &a, sizeof ba);
  std::memcpy(&bb, &b, sizeof bb);
  return ba == bb;
}

// Relative-tolerance closeness for coefficient-wiring checks (avoids fragile
// floating-point associativity assumptions between the pricer and the test).
[[nodiscard]] bool close(double a, double b, double rel = 1e-9) noexcept {
  return std::fabs(a - b) <= rel * (std::fabs(a) + std::fabs(b) + 1e-300);
}

[[nodiscard]] PricingContext make_pricing(std::uint32_t uid, double S = kS, double r = kR,
                                          std::int64_t now = kNow) {
  PricingContext pc;
  pc.S = S;
  pc.r = r;
  pc.now_ts_ns = now;
  pc.method = AmericanMethod::AndersenLake;
  pc.al_opts = al_fast_opts();
  pc.uid = uid;
  return pc;
}

// eSSVI priced surface with knobs: `theta_bump` shifts the whole smile (vol
// axis); the forward F stays at the fixed reference kS across all variants so
// bumping only S / r / now leaves the model IV untouched (clean axis isolation).
[[nodiscard]] PricedSurface make_essvi(std::uint32_t uid, int n, double theta_bump = 0.0,
                                       double S = kS, double r = kR, std::int64_t now = kNow,
                                       double q_eff = 0.02, bool flat_smile = false) {
  CurveSurface cs;
  std::vector<SliceContext> ctx;
  for (int i = 0; i < n; ++i) {
    const double T = 0.05 + 0.10 * static_cast<double>(i);
    const double F = S * std::exp((r - q_eff) * T);
    EssviParams e{};
    e.theta = 0.04 + 0.005 * static_cast<double>(i) + theta_bump;
    e.phi = flat_smile ? 0.0 : 1.5 - 0.05 * static_cast<double>(i);
    e.rho = -0.4 + 0.02 * static_cast<double>(i);
    e.psi = 0.5;
    e.p = 0.5;
    e.lambda = 0.5;
    e.T = T;
    e.F = F;
    e.expiry_id = static_cast<std::uint16_t>(i);
    cs.push(std::make_unique<EssviCurve>(e, std::exp(-r * T)));
    ctx.push_back(SliceContext{T, F, 0.0, q_eff, 250, 7});
  }
  auto ps = PricedSurface::create(std::move(cs), std::move(ctx), make_pricing(uid, S, r, now));
  EXPECT_TRUE(ps.has_value());
  return std::move(*ps);
}

[[nodiscard]] PricedSurface make_svi(std::uint32_t uid, int n) {
  CurveSurface cs;
  std::vector<SliceContext> ctx;
  for (int i = 0; i < n; ++i) {
    const double T = 0.05 + 0.10 * static_cast<double>(i);
    const double F = kS;
    SviParams v{};
    v.a = 0.02 + 0.001 * static_cast<double>(i);
    v.b = 0.10;
    v.rho = -0.3;
    v.m = 0.0;
    v.sigma = 0.15;
    v.T = T;
    v.F = F;
    v.expiry_id = static_cast<std::uint16_t>(i);
    cs.push(std::make_unique<SviCurve>(v, std::exp(-kR * T)));
    ctx.push_back(SliceContext{T, F, 0.0, 0.02, 180, 4});
  }
  auto ps = PricedSurface::create(std::move(cs), std::move(ctx), make_pricing(uid));
  EXPECT_TRUE(ps.has_value());
  return std::move(*ps);
}

[[nodiscard]] PricedSurface make_convex(std::uint32_t uid, int n, int nodes) {
  CurveSurface cs;
  std::vector<SliceContext> ctx;
  for (int i = 0; i < n; ++i) {
    const double T = 0.05 + 0.10 * static_cast<double>(i);
    const double F = kS;
    const double df = std::exp(-kR * T);
    const double sigma = 0.20 + 0.01 * static_cast<double>(i);
    ConvexSliceFit fit;
    fit.T = T;
    fit.F = F;
    fit.df = df;
    fit.rmse_price = 0.25;
    fit.n_obs = static_cast<std::size_t>(nodes);
    fit.n_active = 3;
    fit.u.resize(static_cast<std::size_t>(nodes));
    fit.C.resize(static_cast<std::size_t>(nodes));
    for (int j = 0; j < nodes; ++j) {
      const double K = F * (0.7 + 0.6 * static_cast<double>(j) / static_cast<double>(nodes - 1));
      fit.u[static_cast<std::size_t>(j)] = K;
      fit.C[static_cast<std::size_t>(j)] = atx::vol::black76_price(F, K, T, sigma, df, Side::Call);
    }
    cs.push(std::make_unique<ConvexDenseCurve>(std::move(fit)));
    ctx.push_back(SliceContext{T, F, 0.0, 0.02, static_cast<std::size_t>(nodes), 2});
  }
  auto ps = PricedSurface::create(std::move(cs), std::move(ctx), make_pricing(uid));
  EXPECT_TRUE(ps.has_value());
  return std::move(*ps);
}

[[nodiscard]] SurfaceSet set_of(const std::vector<const PricedSurface *> &v) {
  auto ss = SurfaceSet::create(v);
  EXPECT_TRUE(ss.has_value());
  return std::move(*ss);
}

// A small single-underlying (uid 1) essvi book used across the isolation tests.
[[nodiscard]] std::vector<Position> pnl_book() {
  std::vector<Position> book;
  std::uint64_t id = 0;
  for (double K : {92.0, 98.0, 100.0, 104.0, 110.0}) {
    book.push_back({id++, {1, K, 0.18, Side::Call}, +4.0, 100.0});
    book.push_back({id++, {1, K, 0.30, Side::Put}, -3.0, 100.0});
  }
  return book;
}

// A multi-underlying, multi-kind book with a dedup pair (id12/id14) and a
// no-surface uid (99 -> ModelUnavailable). Priced against {convex 1, essvi 2,
// svi 3}. Used by the in-place / totals-only bit-identity tests.
[[nodiscard]] std::vector<Position> multi_uid_book() {
  return {
      {10, {1, 100.0, 0.18, Side::Call}, +10.0, 100.0},
      {11, {1, 95.0, 0.18, Side::Put}, -5.0, 100.0},
      {12, {2, 105.0, 0.25, Side::Call}, +3.0, 100.0},
      {13, {3, 98.0, 0.15, Side::Put}, +7.0, 100.0},
      {14, {2, 105.0, 0.25, Side::Call}, +2.0, 100.0},  // dup of id12's contract
      {15, {99, 100.0, 0.10, Side::Call}, +1.0, 100.0}, // no surface -> unavailable
  };
}

// Two 70-strike, one-expiry side runs. Each run becomes one full 64-lane
// prepared price tile plus one six-lane tile (one AVX pack + a two-lane tail).
[[nodiscard]] std::vector<Position> tiled_marks_book() {
  std::vector<Position> book;
  book.reserve(140);
  std::uint64_t id = 0;
  for (Side side : {Side::Call, Side::Put}) {
    for (int i = 0; i < 70; ++i) {
      const double K = 65.0 + 0.5 * static_cast<double>(i);
      book.push_back({id++, {1, K, 0.25, side}, (side == Side::Call) ? 2.0 : -1.5, 100.0});
    }
  }
  return book;
}

// Caller-owned backing storage for a PriceFrameView, sized to `n`. The eight
// Greek columns are allocated only under `want_greeks` — mirroring how a
// marks-only caller sizes its buffers and leaves the greek spans empty.
struct FrameStore {
  std::vector<std::uint64_t> id;
  std::vector<std::uint32_t> uid;
  std::vector<double> pv, price, iv;
  std::vector<double> delta, gamma, vega, theta, rho, vanna, volga, charm;
  std::vector<PriceStatus> status;
  PriceTotals total{};

  FrameStore(std::size_t n, bool want_greeks) {
    id.resize(n);
    uid.resize(n);
    pv.resize(n);
    price.resize(n);
    iv.resize(n);
    status.resize(n);
    if (want_greeks) {
      delta.resize(n);
      gamma.resize(n);
      vega.resize(n);
      theta.resize(n);
      rho.resize(n);
      vanna.resize(n);
      volga.resize(n);
      charm.resize(n);
    }
  }

  [[nodiscard]] PriceFrameView view() {
    return PriceFrameView{id,    uid, pv,    price, iv,    delta,  gamma, vega,
                          theta, rho, vanna, volga, charm, status, &total};
  }
};

void expect_totals_bit_identical(const PriceTotals &actual, const PriceTotals &expected) {
  EXPECT_TRUE(bits_equal(actual.pv, expected.pv));
  EXPECT_TRUE(bits_equal(actual.delta, expected.delta));
  EXPECT_TRUE(bits_equal(actual.gamma, expected.gamma));
  EXPECT_TRUE(bits_equal(actual.vega, expected.vega));
  EXPECT_TRUE(bits_equal(actual.theta, expected.theta));
  EXPECT_TRUE(bits_equal(actual.rho, expected.rho));
  EXPECT_TRUE(bits_equal(actual.vanna, expected.vanna));
  EXPECT_TRUE(bits_equal(actual.volga, expected.volga));
  EXPECT_TRUE(bits_equal(actual.charm, expected.charm));
  EXPECT_EQ(actual.n_ok, expected.n_ok);
}

void expect_frame_bit_identical(const PriceFrame &actual, const PriceFrame &expected) {
  ASSERT_EQ(actual.size(), expected.size());
  ASSERT_EQ(actual.greeks_materialized(), expected.greeks_materialized());
  for (std::size_t i = 0; i < actual.size(); ++i) {
    EXPECT_EQ(actual.id[i], expected.id[i]) << i;
    EXPECT_EQ(actual.uid[i], expected.uid[i]) << i;
    EXPECT_EQ(actual.status[i], expected.status[i]) << i;
    EXPECT_TRUE(bits_equal(actual.pv[i], expected.pv[i])) << i;
    EXPECT_TRUE(bits_equal(actual.price[i], expected.price[i])) << i;
    EXPECT_TRUE(bits_equal(actual.iv[i], expected.iv[i])) << i;
    if (actual.greeks_materialized()) {
      EXPECT_TRUE(bits_equal(actual.delta[i], expected.delta[i])) << i;
      EXPECT_TRUE(bits_equal(actual.gamma[i], expected.gamma[i])) << i;
      EXPECT_TRUE(bits_equal(actual.vega[i], expected.vega[i])) << i;
      EXPECT_TRUE(bits_equal(actual.theta[i], expected.theta[i])) << i;
      EXPECT_TRUE(bits_equal(actual.rho[i], expected.rho[i])) << i;
      EXPECT_TRUE(bits_equal(actual.vanna[i], expected.vanna[i])) << i;
      EXPECT_TRUE(bits_equal(actual.volga[i], expected.volga[i])) << i;
      EXPECT_TRUE(bits_equal(actual.charm[i], expected.charm[i])) << i;
    }
  }
  expect_totals_bit_identical(actual.total, expected.total);
}

void expect_frame_bit_identical(const FrameStore &actual, const FrameStore &expected) {
  ASSERT_EQ(actual.id.size(), expected.id.size());
  ASSERT_EQ(actual.delta.empty(), expected.delta.empty());
  for (std::size_t i = 0; i < actual.id.size(); ++i) {
    EXPECT_EQ(actual.id[i], expected.id[i]) << i;
    EXPECT_EQ(actual.uid[i], expected.uid[i]) << i;
    EXPECT_EQ(actual.status[i], expected.status[i]) << i;
    EXPECT_TRUE(bits_equal(actual.pv[i], expected.pv[i])) << i;
    EXPECT_TRUE(bits_equal(actual.price[i], expected.price[i])) << i;
    EXPECT_TRUE(bits_equal(actual.iv[i], expected.iv[i])) << i;
    if (!actual.delta.empty()) {
      EXPECT_TRUE(bits_equal(actual.delta[i], expected.delta[i])) << i;
      EXPECT_TRUE(bits_equal(actual.gamma[i], expected.gamma[i])) << i;
      EXPECT_TRUE(bits_equal(actual.vega[i], expected.vega[i])) << i;
      EXPECT_TRUE(bits_equal(actual.theta[i], expected.theta[i])) << i;
      EXPECT_TRUE(bits_equal(actual.rho[i], expected.rho[i])) << i;
      EXPECT_TRUE(bits_equal(actual.vanna[i], expected.vanna[i])) << i;
      EXPECT_TRUE(bits_equal(actual.volga[i], expected.volga[i])) << i;
      EXPECT_TRUE(bits_equal(actual.charm[i], expected.charm[i])) << i;
    }
  }
  expect_totals_bit_identical(actual.total, expected.total);
}

// Caller-owned backing storage for a PnlFrameView, sized to `n`. P&L has no field
// mask, so all 19 columns always materialize.
struct PnlFrameStore {
  std::vector<std::uint64_t> id;
  std::vector<std::uint32_t> uid;
  std::vector<double> pv_base, pv_target, pnl_total, pnl_delta, pnl_gamma, pnl_vega, pnl_volga,
      pnl_vanna, pnl_theta, pnl_rho, pnl_charm, pnl_unexplained, d_spot, d_vol, d_time, d_rate;
  std::vector<PriceStatus> status;
  PnlTotals total{};

  explicit PnlFrameStore(std::size_t n) {
    id.resize(n);
    uid.resize(n);
    for (std::vector<double> *col :
         {&pv_base, &pv_target, &pnl_total, &pnl_delta, &pnl_gamma, &pnl_vega, &pnl_volga,
          &pnl_vanna, &pnl_theta, &pnl_rho, &pnl_charm, &pnl_unexplained, &d_spot, &d_vol, &d_time,
          &d_rate}) {
      col->resize(n);
    }
    status.resize(n);
  }

  [[nodiscard]] PnlFrameView view() {
    return PnlFrameView{id,        uid,       pv_base,   pv_target,       pnl_total,
                        pnl_delta, pnl_gamma, pnl_vega,  pnl_volga,       pnl_vanna,
                        pnl_theta, pnl_rho,   pnl_charm, pnl_unexplained, d_spot,
                        d_vol,     d_time,    d_rate,    status,          &total};
  }
};

void expect_pnl_totals_bit_identical(const PnlTotals &actual, const PnlTotals &expected) {
  EXPECT_TRUE(bits_equal(actual.pv_base, expected.pv_base));
  EXPECT_TRUE(bits_equal(actual.pv_target, expected.pv_target));
  EXPECT_TRUE(bits_equal(actual.pnl_total, expected.pnl_total));
  EXPECT_TRUE(bits_equal(actual.pnl_delta, expected.pnl_delta));
  EXPECT_TRUE(bits_equal(actual.pnl_gamma, expected.pnl_gamma));
  EXPECT_TRUE(bits_equal(actual.pnl_vega, expected.pnl_vega));
  EXPECT_TRUE(bits_equal(actual.pnl_volga, expected.pnl_volga));
  EXPECT_TRUE(bits_equal(actual.pnl_vanna, expected.pnl_vanna));
  EXPECT_TRUE(bits_equal(actual.pnl_theta, expected.pnl_theta));
  EXPECT_TRUE(bits_equal(actual.pnl_rho, expected.pnl_rho));
  EXPECT_TRUE(bits_equal(actual.pnl_charm, expected.pnl_charm));
  EXPECT_TRUE(bits_equal(actual.pnl_unexplained, expected.pnl_unexplained));
  EXPECT_EQ(actual.n_ok, expected.n_ok);
}

void expect_pnl_frame_bit_identical(const PnlFrameStore &actual, const PnlFrameStore &expected) {
  ASSERT_EQ(actual.id.size(), expected.id.size());
  for (std::size_t i = 0; i < actual.id.size(); ++i) {
    EXPECT_EQ(actual.id[i], expected.id[i]) << i;
    EXPECT_EQ(actual.uid[i], expected.uid[i]) << i;
    EXPECT_EQ(actual.status[i], expected.status[i]) << i;
    EXPECT_TRUE(bits_equal(actual.pv_base[i], expected.pv_base[i])) << i;
    EXPECT_TRUE(bits_equal(actual.pv_target[i], expected.pv_target[i])) << i;
    EXPECT_TRUE(bits_equal(actual.pnl_total[i], expected.pnl_total[i])) << i;
    EXPECT_TRUE(bits_equal(actual.pnl_delta[i], expected.pnl_delta[i])) << i;
    EXPECT_TRUE(bits_equal(actual.pnl_gamma[i], expected.pnl_gamma[i])) << i;
    EXPECT_TRUE(bits_equal(actual.pnl_vega[i], expected.pnl_vega[i])) << i;
    EXPECT_TRUE(bits_equal(actual.pnl_volga[i], expected.pnl_volga[i])) << i;
    EXPECT_TRUE(bits_equal(actual.pnl_vanna[i], expected.pnl_vanna[i])) << i;
    EXPECT_TRUE(bits_equal(actual.pnl_theta[i], expected.pnl_theta[i])) << i;
    EXPECT_TRUE(bits_equal(actual.pnl_rho[i], expected.pnl_rho[i])) << i;
    EXPECT_TRUE(bits_equal(actual.pnl_charm[i], expected.pnl_charm[i])) << i;
    EXPECT_TRUE(bits_equal(actual.pnl_unexplained[i], expected.pnl_unexplained[i])) << i;
    EXPECT_TRUE(bits_equal(actual.d_spot[i], expected.d_spot[i])) << i;
    EXPECT_TRUE(bits_equal(actual.d_vol[i], expected.d_vol[i])) << i;
    EXPECT_TRUE(bits_equal(actual.d_time[i], expected.d_time[i])) << i;
    EXPECT_TRUE(bits_equal(actual.d_rate[i], expected.d_rate[i])) << i;
  }
  expect_pnl_totals_bit_identical(actual.total, expected.total);
}

// A multi-underlying (uids 1/2/3 essvi + a no-surface uid 99), multi-expiry,
// mixed-side book with a dedup pair — exercises the grouped P&L substrate's
// equal-T ladders (three strikes per (uid,side,T) run) and the ModelUnavailable /
// InvalidContract lanes. All-essvi so a shifted set can bump every axis.
[[nodiscard]] std::vector<Position> pnl_multi_book() {
  std::vector<Position> book;
  std::uint64_t id = 0;
  for (std::uint32_t u : {1u, 2u, 3u}) {
    for (double T : {0.15, 0.25, 0.35}) {
      for (double K : {92.0, 100.0, 108.0}) {
        book.push_back({id++, {u, K, T, Side::Call}, +2.0 + 0.1 * static_cast<double>(u), 100.0});
        book.push_back({id++, {u, K, T, Side::Put}, -1.5, 100.0});
      }
    }
  }
  book.push_back({id++, {1u, 92.0, 0.15, Side::Call}, +5.0, 100.0}); // dedup of the first contract
  book.push_back({id++, {99u, 100.0, 0.20, Side::Call}, +1.0, 100.0}); // no surface -> unavailable
  return book;
}

} // namespace

TEST(PortfolioPricer, RetimePreservesDedupAndRejectsDivergentTenors) {
  const std::vector<Position> positions = {
      {1u, {1u, 100.0, 0.25, Side::Call}, 1.0, 100.0},
      {2u, {1u, 100.0, 0.25, Side::Call}, 2.0, 100.0},
  };
  auto portfolio = Portfolio::create(positions);
  ASSERT_TRUE(portfolio.has_value()) << portfolio.error().to_string();
  ASSERT_EQ(portfolio->n_contracts(), 1u);
  const std::array<double, 2> next_t{0.20, 0.20};
  ASSERT_TRUE(portfolio->retime(next_t).has_value());
  EXPECT_TRUE(bits_equal(portfolio->contracts()[0].T, 0.20));
  EXPECT_TRUE(bits_equal(portfolio->positions()[0].contract.T, 0.20));
  EXPECT_TRUE(bits_equal(portfolio->positions()[1].contract.T, 0.20));

  const std::array<double, 2> inconsistent{0.19, 0.18};
  const Status status = portfolio->retime(inconsistent);
  ASSERT_FALSE(status.has_value());
  EXPECT_EQ(status.error().code(), ErrorCode::InvalidArgument);
}

TEST(PortfolioPricer, BitIdenticalRetimePreservesWarmedPreparedSubstrate) {
  using atx::vol::counters::Counter;
  using atx::vol::counters::counters_enabled;
  const PricedSurface surface = make_essvi(1, 5);
  const SurfaceSet surfaces = set_of({&surface});
  const std::vector<Position> positions{
      {1u, {1u, 90.0, 0.15, Side::Call}, +1.0, 100.0},
      {2u, {1u, 100.0, 0.25, Side::Put}, +2.0, 100.0},
  };
  auto portfolio = Portfolio::create(positions);
  ASSERT_TRUE(portfolio.has_value()) << portfolio.error().to_string();
  PortfolioPricer pricer(std::move(*portfolio));
  PortfolioWorkspace workspace;
  FrameStore warmup(positions.size(), /*want_greeks=*/false);
  ASSERT_TRUE(pricer
                  .price_into(surfaces, PriceFieldMask::Marks, warmup.view(), workspace)
                  .has_value());

  if constexpr (counters_enabled()) {
    atx::vol::counters::reset();
  }
  const std::array<double, 2> same_t{0.15, 0.25};
  ASSERT_TRUE(pricer.retime(same_t).has_value());
  FrameStore result(positions.size(), /*want_greeks=*/false);
  ASSERT_TRUE(pricer
                  .price_into(surfaces, PriceFieldMask::Marks, result.view(), workspace)
                  .has_value());
  if constexpr (counters_enabled()) {
    EXPECT_EQ(atx::vol::counters::snapshot().get(Counter::PreparedBuilds), 0u);
  }
}

TEST(PortfolioPricer, Retime_LaterDedupMismatch_LeavesPortfolioAndPricingUnchanged) {
  const PricedSurface surface = make_essvi(1, 5);
  const SurfaceSet surfaces = set_of({&surface});
  const std::vector<Position> positions{
      {1u, {1u, 90.0, 0.15, Side::Call}, +1.0, 100.0},
      {2u, {1u, 100.0, 0.25, Side::Put}, +2.0, 100.0},
      {3u, {1u, 100.0, 0.25, Side::Put}, -3.0, 100.0},
      {4u, {1u, 110.0, 0.35, Side::Call}, +4.0, 100.0},
  };
  auto portfolio = Portfolio::create(positions);
  ASSERT_TRUE(portfolio.has_value()) << portfolio.error().to_string();
  PortfolioPricer pricer(std::move(*portfolio));

  const std::vector<Position> positions_before(pricer.portfolio().positions().begin(),
                                               pricer.portfolio().positions().end());
  const std::vector<OptionContract> contracts_before(pricer.portfolio().contracts().begin(),
                                                     pricer.portfolio().contracts().end());
  const std::vector<std::uint32_t> uids_before(pricer.portfolio().uids().begin(),
                                               pricer.portfolio().uids().end());
  std::vector<std::uint32_t> contract_ix_before;
  contract_ix_before.reserve(pricer.portfolio().n_positions());
  for (std::size_t i = 0; i < pricer.portfolio().n_positions(); ++i) {
    contract_ix_before.push_back(pricer.portfolio().contract_ix(i));
  }
  auto price_before = pricer.price(surfaces, PriceOptions{.n_threads = 1});
  ASSERT_TRUE(price_before.has_value()) << price_before.error().to_string();

  // Contract 0 is valid and appears to retime successfully. The mismatch is in
  // contract 1's deduplicated positions, so an implementation that commits while
  // validating exposes a partially-retimed unique-contract table on this error.
  const std::array<double, 4> inconsistent{0.12, 0.20, 0.19, 0.30};
  const Status status = pricer.retime(inconsistent);
  ASSERT_FALSE(status.has_value());
  EXPECT_EQ(status.error().code(), ErrorCode::InvalidArgument);

  const Portfolio &after = pricer.portfolio();
  ASSERT_EQ(after.n_positions(), positions_before.size());
  ASSERT_EQ(after.n_contracts(), contracts_before.size());
  ASSERT_EQ(after.n_underlyings(), uids_before.size());
  for (std::size_t i = 0; i < positions_before.size(); ++i) {
    const Position &actual = after.positions()[i];
    const Position &expected = positions_before[i];
    EXPECT_EQ(actual.id, expected.id) << i;
    EXPECT_EQ(actual.contract.uid, expected.contract.uid) << i;
    EXPECT_TRUE(bits_equal(actual.contract.K, expected.contract.K)) << i;
    EXPECT_TRUE(bits_equal(actual.contract.T, expected.contract.T)) << i;
    EXPECT_EQ(actual.contract.side, expected.contract.side) << i;
    EXPECT_TRUE(bits_equal(actual.qty, expected.qty)) << i;
    EXPECT_TRUE(bits_equal(actual.multiplier, expected.multiplier)) << i;
    EXPECT_EQ(after.contract_ix(i), contract_ix_before[i]) << i;
  }
  for (std::size_t i = 0; i < contracts_before.size(); ++i) {
    const OptionContract &actual = after.contracts()[i];
    const OptionContract &expected = contracts_before[i];
    EXPECT_EQ(actual.uid, expected.uid) << i;
    EXPECT_TRUE(bits_equal(actual.K, expected.K)) << i;
    EXPECT_TRUE(bits_equal(actual.T, expected.T)) << i;
    EXPECT_EQ(actual.side, expected.side) << i;
  }
  for (std::size_t i = 0; i < uids_before.size(); ++i) {
    EXPECT_EQ(after.uids()[i], uids_before[i]) << i;
  }

  auto price_after = pricer.price(surfaces, PriceOptions{.n_threads = 1});
  ASSERT_TRUE(price_after.has_value()) << price_after.error().to_string();
  expect_frame_bit_identical(*price_after, *price_before);
}

TEST(PortfolioPricer, Retime_NonFirstMaturitiesChange_WarmedWorkspaceMatchesFreshWorkspace) {
  const PricedSurface surface = make_essvi(1, 5);
  const PricedSurface shifted_surface =
      make_essvi(1, 5, 0.002, kS + 1.0, kR + 0.001, kNow + 86'400'000'000'000LL);
  const SurfaceSet surfaces = set_of({&surface});
  const SurfaceSet shifted_surfaces = set_of({&shifted_surface});
  const std::vector<Position> positions{
      {10u, {1u, 90.0, 0.10, Side::Call}, +1.0, 100.0},
      {11u, {1u, 95.0, 0.20, Side::Call}, +2.0, 100.0},
      {12u, {1u, 105.0, 0.30, Side::Call}, -1.0, 100.0},
      {13u, {1u, 110.0, 0.30, Side::Call}, +3.0, 100.0},
      {14u, {1u, 110.0, 0.30, Side::Call}, -0.5, 100.0},
  };
  auto portfolio = Portfolio::create(positions);
  ASSERT_TRUE(portfolio.has_value()) << portfolio.error().to_string();
  PortfolioPricer pricer(std::move(*portfolio));
  ASSERT_EQ(pricer.portfolio().n_contracts(), 4u);

  const std::size_t n = pricer.portfolio().n_positions();
  const PriceOptions opts{.n_threads = 1};
  PortfolioWorkspace warmed_workspace;
  warmed_workspace.reserve(pricer.portfolio().n_contracts(), n);
  FrameStore warmup(n, /*want_greeks=*/true);
  ASSERT_TRUE(
      pricer.price_into(surfaces, PriceFieldMask::FullGreeks, warmup.view(), warmed_workspace, opts)
          .has_value());
  PortfolioWorkspace warmed_pnl_workspace;
  warmed_pnl_workspace.reserve(pricer.portfolio().n_contracts(), n);
  PnlFrameStore pnl_warmup(n);
  ASSERT_TRUE(pricer
                  .pnl_explain_into(surfaces, shifted_surfaces, pnl_warmup.view(),
                                    warmed_pnl_workspace, opts)
                  .has_value());

  // Keep unique contract 0 unchanged (so a first-contract-only fingerprint is
  // unchanged), move contract 1 behind contracts 2/3, and form a raw-bit-equal
  // T=0.20 run whose deterministic order is original contract indices 2 then 3.
  // The duplicate positions for contract 3 receive identical tenors.
  const std::array<double, 5> next_t{0.10, 0.35, 0.20, 0.20, 0.20};
  ASSERT_TRUE(pricer.retime(next_t).has_value());
  EXPECT_TRUE(bits_equal(pricer.portfolio().contracts()[0].T, 0.10));
  EXPECT_TRUE(bits_equal(pricer.portfolio().contracts()[1].T, 0.35));
  EXPECT_TRUE(bits_equal(pricer.portfolio().contracts()[2].T, 0.20));
  EXPECT_TRUE(bits_equal(pricer.portfolio().contracts()[3].T, 0.20));

  FrameStore warmed_result(n, /*want_greeks=*/true);
  ASSERT_TRUE(pricer
                  .price_into(surfaces, PriceFieldMask::FullGreeks, warmed_result.view(),
                              warmed_workspace, opts)
                  .has_value());

  PortfolioWorkspace fresh_into_workspace;
  FrameStore fresh_result(n, /*want_greeks=*/true);
  ASSERT_TRUE(pricer
                  .price_into(surfaces, PriceFieldMask::FullGreeks, fresh_result.view(),
                              fresh_into_workspace, opts)
                  .has_value());
  expect_frame_bit_identical(warmed_result, fresh_result);

  auto warmed_totals =
      pricer.price_totals(surfaces, PriceFieldMask::FullGreeks, warmed_workspace, opts);
  ASSERT_TRUE(warmed_totals.has_value()) << warmed_totals.error().to_string();
  PortfolioWorkspace fresh_totals_workspace;
  auto fresh_totals =
      pricer.price_totals(surfaces, PriceFieldMask::FullGreeks, fresh_totals_workspace, opts);
  ASSERT_TRUE(fresh_totals.has_value()) << fresh_totals.error().to_string();
  expect_totals_bit_identical(*warmed_totals, *fresh_totals);

  PnlFrameStore warmed_pnl(n);
  ASSERT_TRUE(pricer
                  .pnl_explain_into(surfaces, shifted_surfaces, warmed_pnl.view(),
                                    warmed_pnl_workspace, opts)
                  .has_value());
  PortfolioWorkspace fresh_pnl_workspace;
  PnlFrameStore fresh_pnl(n);
  ASSERT_TRUE(
      pricer
          .pnl_explain_into(surfaces, shifted_surfaces, fresh_pnl.view(), fresh_pnl_workspace, opts)
          .has_value());
  expect_pnl_frame_bit_identical(warmed_pnl, fresh_pnl);

  auto warmed_pnl_totals =
      pricer.pnl_totals(surfaces, shifted_surfaces, warmed_pnl_workspace, opts);
  ASSERT_TRUE(warmed_pnl_totals.has_value()) << warmed_pnl_totals.error().to_string();
  PortfolioWorkspace fresh_pnl_totals_workspace;
  auto fresh_pnl_totals =
      pricer.pnl_totals(surfaces, shifted_surfaces, fresh_pnl_totals_workspace, opts);
  ASSERT_TRUE(fresh_pnl_totals.has_value()) << fresh_pnl_totals.error().to_string();
  expect_pnl_totals_bit_identical(*warmed_pnl_totals, *fresh_pnl_totals);
}

// ── Pricing: multi-kind, multi-underlying, dedup, missing uid ────────────────

TEST(PortfolioPricer, Copy_RestartsRevisionWithDistinctWorkspaceIdentity) {
  const PricedSurface surface = make_essvi(1, 5);
  const SurfaceSet surfaces = set_of({&surface});
  const std::vector<Position> positions{
      {10u, {1u, 90.0, 0.10, Side::Call}, +1.0, 100.0},
      {11u, {1u, 95.0, 0.20, Side::Call}, +2.0, 100.0},
      {12u, {1u, 105.0, 0.30, Side::Call}, -1.0, 100.0},
      {13u, {1u, 110.0, 0.30, Side::Call}, +3.0, 100.0},
      {14u, {1u, 110.0, 0.30, Side::Call}, -0.5, 100.0},
  };
  auto created = Portfolio::create(positions);
  ASSERT_TRUE(created.has_value()) << created.error().to_string();
  Portfolio source(std::move(*created));

  // Source reaches revision 1. Its copy starts at revision 0; after one different
  // retime both logical books are at revision 1, so identity must distinguish them.
  const std::array<double, 5> source_t{0.10, 0.25, 0.30, 0.30, 0.30};
  ASSERT_TRUE(source.retime(source_t).has_value());
  Portfolio copied(source);
  PortfolioPricer pricer(std::move(source));

  const std::size_t n = pricer.portfolio().n_positions();
  PortfolioWorkspace workspace;
  FrameStore source_frame(n, /*want_greeks=*/true);
  ASSERT_TRUE(
      pricer.price_into(surfaces, PriceFieldMask::FullGreeks, source_frame.view(), workspace)
          .has_value());

  const std::array<double, 5> copy_t{0.10, 0.35, 0.20, 0.20, 0.20};
  ASSERT_TRUE(copied.retime(copy_t).has_value());
  pricer = PortfolioPricer(std::move(copied));

  FrameStore reused(n, /*want_greeks=*/true);
  ASSERT_TRUE(pricer.price_into(surfaces, PriceFieldMask::FullGreeks, reused.view(), workspace)
                  .has_value());
  PortfolioWorkspace fresh_workspace;
  FrameStore fresh(n, /*want_greeks=*/true);
  ASSERT_TRUE(pricer.price_into(surfaces, PriceFieldMask::FullGreeks, fresh.view(), fresh_workspace)
                  .has_value());
  expect_frame_bit_identical(reused, fresh);
}

TEST(PortfolioPricer, Move_TransfersWorkspaceIdentityAndLeavesSourceValid) {
  using atx::vol::counters::Counter;
  using atx::vol::counters::counters_enabled;
  const PricedSurface surface = make_essvi(1, 5);
  const SurfaceSet surfaces = set_of({&surface});
  auto portfolio = Portfolio::create(pnl_book());
  ASSERT_TRUE(portfolio.has_value()) << portfolio.error().to_string();
  PortfolioPricer source(std::move(*portfolio));

  const std::size_t n = source.portfolio().n_positions();
  PortfolioWorkspace workspace;
  FrameStore warmup(n, /*want_greeks=*/true);
  ASSERT_TRUE(source.price_into(surfaces, PriceFieldMask::FullGreeks, warmup.view(), workspace)
                  .has_value());

  PortfolioPricer moved(std::move(source));
  if constexpr (counters_enabled()) {
    atx::vol::counters::reset();
  }
  FrameStore reused(n, /*want_greeks=*/true);
  ASSERT_TRUE(
      moved.price_into(surfaces, PriceFieldMask::FullGreeks, reused.view(), workspace).has_value());
  if constexpr (counters_enabled()) {
    EXPECT_EQ(atx::vol::counters::snapshot().get(Counter::PreparedBuilds), 0u);
  }

  PortfolioWorkspace fresh_workspace;
  FrameStore fresh(n, /*want_greeks=*/true);
  ASSERT_TRUE(moved.price_into(surfaces, PriceFieldMask::FullGreeks, fresh.view(), fresh_workspace)
                  .has_value());
  expect_frame_bit_identical(reused, fresh);

  // The moved-from pricer owns a fresh empty logical book and remains callable.
  ASSERT_TRUE(source.retime(std::span<const double>{}).has_value());
  auto empty = source.price(surfaces);
  ASSERT_TRUE(empty.has_value()) << empty.error().to_string();
  EXPECT_EQ(empty->size(), 0u);
}

TEST(PortfolioPricer, Price_MultiKind_MultiUnderlying_BitIdenticalToGreeks) {
  const PricedSurface s1 = make_convex(1, 4, 32);
  const PricedSurface s2 = make_essvi(2, 5);
  const PricedSurface s3 = make_svi(3, 4);
  const SurfaceSet surfaces = set_of({&s1, &s2, &s3});

  const std::vector<Position> book{
      {/*id*/ 10, {1, 100.0, 0.18, Side::Call}, +10.0, 100.0},
      {/*id*/ 11, {1, 95.0, 0.18, Side::Put}, -5.0, 100.0},
      {/*id*/ 12, {2, 105.0, 0.25, Side::Call}, +3.0, 100.0},
      {/*id*/ 13, {3, 98.0, 0.15, Side::Put}, +7.0, 100.0},
      {/*id*/ 14, {2, 105.0, 0.25, Side::Call}, +2.0, 100.0},  // dup of id 12's contract
      {/*id*/ 15, {99, 100.0, 0.10, Side::Call}, +1.0, 100.0}, // no surface -> unavailable
  };
  auto pf = Portfolio::create(book);
  ASSERT_TRUE(pf.has_value());
  EXPECT_EQ(pf->n_positions(), 6u);
  EXPECT_EQ(pf->n_contracts(), 5u);   // id12 & id14 share a contract
  EXPECT_EQ(pf->n_underlyings(), 4u); // uids {1,2,3,99}

  const PortfolioPricer pricer(std::move(*pf));
  auto fr = pricer.price(surfaces);
  ASSERT_TRUE(fr.has_value());
  const PriceFrame &f = *fr;
  ASSERT_EQ(f.size(), 6u);

  const std::array<const PricedSurface *, 4> by_uid{nullptr, &s1, &s2, &s3};
  PriceTotals expect_total{};
  for (std::size_t i = 0; i < book.size(); ++i) {
    const Position &p = book[i];
    EXPECT_EQ(f.id[i], p.id);
    EXPECT_EQ(f.uid[i], p.contract.uid);
    if (p.contract.uid == 99) {
      EXPECT_EQ(f.status[i], PriceStatus::ModelUnavailable);
      EXPECT_TRUE(std::isnan(f.pv[i]));
      continue;
    }
    ASSERT_EQ(f.status[i], PriceStatus::Ok);
    const PricedSurface &s = *by_uid[p.contract.uid];
    const auto g = s.greeks(p.contract.K, p.contract.T, p.contract.side);
    ASSERT_TRUE(g.has_value());
    const double w = p.qty * p.multiplier;
    const auto fv = s.fair_value(p.contract.K, p.contract.T, p.contract.side);
    ASSERT_TRUE(fv.has_value());
    EXPECT_TRUE(bits_equal(f.price[i], *fv)); // per-share American mark
    EXPECT_TRUE(bits_equal(f.iv[i], s.iv(p.contract.K, p.contract.T)));
    EXPECT_TRUE(bits_equal(f.pv[i], w * *fv));
    EXPECT_TRUE(bits_equal(f.delta[i], w * g->delta));
    EXPECT_TRUE(bits_equal(f.gamma[i], w * g->gamma));
    EXPECT_TRUE(bits_equal(f.vega[i], w * g->vega));
    EXPECT_TRUE(bits_equal(f.theta[i], w * g->theta));
    EXPECT_TRUE(bits_equal(f.rho[i], w * g->rho));
    EXPECT_TRUE(bits_equal(f.vanna[i], w * g->vanna));
    EXPECT_TRUE(bits_equal(f.volga[i], w * g->volga));
    EXPECT_TRUE(bits_equal(f.charm[i], w * g->charm));
    expect_total.pv += f.pv[i];
    expect_total.delta += f.delta[i];
    expect_total.vega += f.vega[i];
    ++expect_total.n_ok;
  }
  EXPECT_EQ(f.total.n_ok, 5u);
  EXPECT_TRUE(bits_equal(f.total.pv, expect_total.pv));
  EXPECT_TRUE(bits_equal(f.total.delta, expect_total.delta));
  EXPECT_TRUE(bits_equal(f.total.vega, expect_total.vega));

  // id12 and id14 reference one contract: same per-share price/iv, scaled PV.
  EXPECT_TRUE(bits_equal(f.price[2], f.price[4]));
  EXPECT_TRUE(bits_equal(f.iv[2], f.iv[4]));
  EXPECT_TRUE(bits_equal(f.pv[2], 1.5 * f.pv[4])); // qty 3 vs 2
}

TEST(PortfolioPricer, Price_DegenerateContract_Invalid) {
  const PricedSurface s1 = make_essvi(1, 4);
  const SurfaceSet surfaces = set_of({&s1});
  const std::vector<Position> book{
      {1, {1, -5.0, 0.2, Side::Call}, 1.0, 100.0},  // K <= 0
      {2, {1, 100.0, 0.0, Side::Call}, 1.0, 100.0}, // T <= 0
  };
  auto pf = Portfolio::create(book);
  ASSERT_TRUE(pf.has_value());
  const PortfolioPricer pricer(std::move(*pf));
  auto fr = pricer.price(surfaces);
  ASSERT_TRUE(fr.has_value());
  EXPECT_EQ(fr->status[0], PriceStatus::InvalidContract);
  EXPECT_EQ(fr->status[1], PriceStatus::InvalidContract);
  EXPECT_EQ(fr->total.n_ok, 0u);
}

TEST(PortfolioPricer, Price_ThreadCounts_BitIdentical) {
  std::vector<PricedSurface> surfs;
  std::vector<const PricedSurface *> ptrs;
  std::vector<Position> book;
  for (std::uint32_t u = 1; u <= 6; ++u) {
    surfs.push_back((u & 1u) ? make_convex(u, 4, 28) : make_essvi(u, 5));
  }
  for (const PricedSurface &s : surfs) {
    ptrs.push_back(&s);
  }
  const SurfaceSet surfaces = set_of(ptrs);
  std::uint64_t id = 0;
  for (std::uint32_t u = 1; u <= 6; ++u) {
    for (double K : {90.0, 95.0, 100.0, 105.0, 110.0}) {
      book.push_back({id++, {u, K, 0.18, Side::Call}, 1.0 + 0.1 * static_cast<double>(u), 100.0});
      book.push_back({id++, {u, K, 0.28, Side::Put}, -2.0, 100.0});
    }
  }
  auto pf = Portfolio::create(book);
  ASSERT_TRUE(pf.has_value());
  const PortfolioPricer pricer(std::move(*pf));

  auto a = pricer.price(surfaces, PriceOptions{1});
  auto b = pricer.price(surfaces, PriceOptions{8});
  auto c = pricer.price(surfaces, PriceOptions{0}); // hardware concurrency
  ASSERT_TRUE(a.has_value() && b.has_value() && c.has_value());
  ASSERT_EQ(a->size(), b->size());
  for (std::size_t i = 0; i < a->size(); ++i) {
    EXPECT_TRUE(bits_equal(a->pv[i], b->pv[i])) << i;
    EXPECT_TRUE(bits_equal(a->delta[i], b->delta[i])) << i;
    EXPECT_TRUE(bits_equal(a->gamma[i], b->gamma[i])) << i;
    EXPECT_TRUE(bits_equal(a->vega[i], b->vega[i])) << i;
    EXPECT_TRUE(bits_equal(a->pv[i], c->pv[i])) << i;
  }
  EXPECT_TRUE(bits_equal(a->total.pv, b->total.pv));
  EXPECT_TRUE(bits_equal(a->total.delta, b->total.delta));
  EXPECT_TRUE(bits_equal(a->total.vega, b->total.vega));
}

// ── PnL explain: per-axis isolation, coefficient wiring, reconstruction ──────

TEST(PortfolioPricer, PnlExplain_SpotBump_DeltaGammaOnly) {
  const double dS = 0.05; // small spot move -> 2nd-order Taylor is tight
  const PricedSurface base = make_essvi(1, 5, 0.0, kS, kR, kNow, 0.02, true);
  const PricedSurface shifted =
      make_essvi(1, 5, 0.0, kS + dS, kR, kNow, 0.02, true);
  const SurfaceSet bset = set_of({&base});
  const SurfaceSet sset = set_of({&shifted});

  const std::vector<Position> book = pnl_book();
  auto pf = Portfolio::create(book);
  ASSERT_TRUE(pf.has_value());
  const PortfolioPricer pricer(std::move(*pf));
  auto er = pricer.pnl_explain(bset, sset);
  ASSERT_TRUE(er.has_value());
  const PnlFrame &f = *er;

  for (std::size_t i = 0; i < f.size(); ++i) {
    ASSERT_EQ(f.status[i], PriceStatus::Ok) << i;
    EXPECT_NEAR(f.d_spot[i], dS, 1e-12);
    EXPECT_EQ(f.d_vol[i], 0.0) << i; // identical curves -> exact zero
    EXPECT_EQ(f.d_time[i], 0.0) << i;
    EXPECT_EQ(f.d_rate[i], 0.0) << i;
    // Only spot moved: the non-spot Taylor terms are exactly zero.
    EXPECT_EQ(f.pnl_vega[i], 0.0) << i;
    EXPECT_EQ(f.pnl_volga[i], 0.0) << i;
    EXPECT_EQ(f.pnl_vanna[i], 0.0) << i;
    EXPECT_EQ(f.pnl_theta[i], 0.0) << i;
    EXPECT_EQ(f.pnl_rho[i], 0.0) << i;
    EXPECT_EQ(f.pnl_charm[i], 0.0) << i;
    // Coefficient wiring: delta/gamma terms are the base Greeks * move.
    const Position &p = book[i];
    const auto g = base.greeks(p.contract.K, p.contract.T, p.contract.side);
    ASSERT_TRUE(g.has_value());
    const double w = p.qty * p.multiplier;
    EXPECT_TRUE(close(f.pnl_delta[i], w * g->delta * dS)) << i;
    EXPECT_TRUE(close(f.pnl_gamma[i], w * 0.5 * g->gamma * dS * dS)) << i;
    // Components + residual == the full reprice (bookkeeping).
    EXPECT_NEAR(f.pnl_delta[i] + f.pnl_gamma[i] + f.pnl_unexplained[i], f.pnl_total[i],
                1e-6 * (std::fabs(f.pnl_total[i]) + 1.0))
        << i;
    // Small move: delta + gamma explain the bulk.
    // (residual is now the pure higher-order Taylor tail: the early-exercise
    // premium lives inside the American delta/gamma. See TaylorReconstruction_Tight.)
  }
}

TEST(PortfolioPricer, PnlExplain_RateBump_RhoOnly) {
  const double dr = 1e-4; // 1 bp
  const PricedSurface base = make_essvi(1, 5, 0.0, kS, kR, kNow, 0.02, true);
  const PricedSurface shifted =
      make_essvi(1, 5, 0.0, kS, kR + dr, kNow, 0.02, true);
  const SurfaceSet bset = set_of({&base});
  const SurfaceSet sset = set_of({&shifted});

  const std::vector<Position> book = pnl_book();
  auto pf = Portfolio::create(book);
  ASSERT_TRUE(pf.has_value());
  const PortfolioPricer pricer(std::move(*pf));
  auto er = pricer.pnl_explain(bset, sset);
  ASSERT_TRUE(er.has_value());
  const PnlFrame &f = *er;

  for (std::size_t i = 0; i < f.size(); ++i) {
    ASSERT_EQ(f.status[i], PriceStatus::Ok) << i;
    EXPECT_NEAR(f.d_rate[i], dr, 1e-15);
    EXPECT_EQ(f.d_spot[i], 0.0) << i;
    EXPECT_EQ(f.d_vol[i], 0.0) << i;
    EXPECT_EQ(f.d_time[i], 0.0) << i;
    EXPECT_EQ(f.pnl_delta[i], 0.0) << i;
    EXPECT_EQ(f.pnl_gamma[i], 0.0) << i;
    EXPECT_EQ(f.pnl_vega[i], 0.0) << i;
    EXPECT_EQ(f.pnl_theta[i], 0.0) << i;
    EXPECT_EQ(f.pnl_vanna[i], 0.0) << i;
    EXPECT_EQ(f.pnl_charm[i], 0.0) << i;
    const Position &p = book[i];
    const auto g = base.greeks(p.contract.K, p.contract.T, p.contract.side);
    ASSERT_TRUE(g.has_value());
    const double w = p.qty * p.multiplier;
    EXPECT_TRUE(close(f.pnl_rho[i], w * g->rho * dr)) << i;
    EXPECT_NEAR(f.pnl_rho[i] + f.pnl_unexplained[i], f.pnl_total[i],
                1e-6 * (std::fabs(f.pnl_total[i]) + 1.0))
        << i;
    // (residual is now the pure higher-order Taylor tail: the early-exercise
    // premium lives inside the American delta/gamma. See TaylorReconstruction_Tight.)
  }
}

TEST(PortfolioPricer, PnlExplain_VolBump_VegaVolgaOnly) {
  const PricedSurface base = make_essvi(1, 5);
  const PricedSurface shifted = make_essvi(1, 5, /*theta_bump*/ 0.001);
  const SurfaceSet bset = set_of({&base});
  const SurfaceSet sset = set_of({&shifted});

  const std::vector<Position> book = pnl_book();
  auto pf = Portfolio::create(book);
  ASSERT_TRUE(pf.has_value());
  const PortfolioPricer pricer(std::move(*pf));
  auto er = pricer.pnl_explain(bset, sset);
  ASSERT_TRUE(er.has_value());
  const PnlFrame &f = *er;

  for (std::size_t i = 0; i < f.size(); ++i) {
    ASSERT_EQ(f.status[i], PriceStatus::Ok) << i;
    EXPECT_EQ(f.d_spot[i], 0.0) << i;
    EXPECT_EQ(f.d_time[i], 0.0) << i;
    EXPECT_EQ(f.d_rate[i], 0.0) << i;
    EXPECT_NE(f.d_vol[i], 0.0) << i;
    // dS = 0 -> spot/rate/time/cross terms are exactly zero.
    EXPECT_EQ(f.pnl_delta[i], 0.0) << i;
    EXPECT_EQ(f.pnl_gamma[i], 0.0) << i;
    EXPECT_EQ(f.pnl_theta[i], 0.0) << i;
    EXPECT_EQ(f.pnl_rho[i], 0.0) << i;
    EXPECT_EQ(f.pnl_vanna[i], 0.0) << i;
    EXPECT_EQ(f.pnl_charm[i], 0.0) << i;
    // Coefficient wiring: vega/volga terms are base Greeks * dvol.
    const Position &p = book[i];
    const auto g = base.greeks(p.contract.K, p.contract.T, p.contract.side);
    ASSERT_TRUE(g.has_value());
    const double w = p.qty * p.multiplier;
    EXPECT_TRUE(close(f.pnl_vega[i], w * g->vega * f.d_vol[i])) << i;
    EXPECT_TRUE(close(f.pnl_volga[i], w * 0.5 * g->volga * f.d_vol[i] * f.d_vol[i])) << i;
    EXPECT_NEAR(f.pnl_vega[i] + f.pnl_volga[i] + f.pnl_unexplained[i], f.pnl_total[i],
                1e-6 * (std::fabs(f.pnl_total[i]) + 1.0))
        << i;
    // (residual is now the pure higher-order Taylor tail: the early-exercise
    // premium lives inside the American delta/gamma. See TaylorReconstruction_Tight.)
  }
}

TEST(PortfolioPricer, PnlExplain_TimeBump_ThetaAndVolRoll) {
  const std::int64_t one_hour = static_cast<std::int64_t>(3600.0 * 1e9);
  const PricedSurface base = make_essvi(1, 5);
  const PricedSurface shifted = make_essvi(1, 5, 0.0, kS, kR, kNow + one_hour);
  const SurfaceSet bset = set_of({&base});
  const SurfaceSet sset = set_of({&shifted});

  const std::vector<Position> book = pnl_book();
  auto pf = Portfolio::create(book);
  ASSERT_TRUE(pf.has_value());
  const PortfolioPricer pricer(std::move(*pf));
  auto er = pricer.pnl_explain(bset, sset);
  ASSERT_TRUE(er.has_value());
  const PnlFrame &f = *er;

  const double dt_expect = static_cast<double>(one_hour) / kNsPerYear;
  for (std::size_t i = 0; i < f.size(); ++i) {
    ASSERT_EQ(f.status[i], PriceStatus::Ok) << i;
    EXPECT_NEAR(f.d_time[i], dt_expect, 1e-15);
    EXPECT_EQ(f.d_spot[i], 0.0) << i;
    EXPECT_EQ(f.d_rate[i], 0.0) << i;
    // dS = 0 -> delta/gamma/vanna/charm/rho vanish. Vol is measured at the COMMON
    // base maturity, so an identical-curve time roll leaves dvol EXACTLY zero (the
    // term roll now stays inside theta), and vega/volga are inert.
    EXPECT_EQ(f.d_vol[i], 0.0) << i;
    EXPECT_EQ(f.pnl_delta[i], 0.0) << i;
    EXPECT_EQ(f.pnl_gamma[i], 0.0) << i;
    EXPECT_EQ(f.pnl_vanna[i], 0.0) << i;
    EXPECT_EQ(f.pnl_charm[i], 0.0) << i;
    EXPECT_EQ(f.pnl_rho[i], 0.0) << i;
    EXPECT_EQ(f.pnl_vega[i], 0.0) << i;
    EXPECT_EQ(f.pnl_volga[i], 0.0) << i;
    const Position &p = book[i];
    const auto g = base.greeks(p.contract.K, p.contract.T, p.contract.side);
    ASSERT_TRUE(g.has_value());
    const double w = p.qty * p.multiplier;
    EXPECT_TRUE(close(f.pnl_theta[i], w * g->theta * dt_expect)) << i;
    const double sum = f.pnl_theta[i] + f.pnl_unexplained[i];
    EXPECT_NEAR(sum, f.pnl_total[i], 1e-6 * (std::fabs(f.pnl_total[i]) + 1.0)) << i;
    // (residual is now the pure higher-order Taylor tail: the early-exercise
    // premium lives inside the American delta/gamma. See TaylorReconstruction_Tight.)
  }
}

TEST(PortfolioPricer, PnlExplain_CombinedShift_SumsToTotal_And_ThreadDeterministic) {
  const std::int64_t one_hour = static_cast<std::int64_t>(3600.0 * 1e9);
  const PricedSurface base = make_essvi(1, 5);
  // Combined small shift: spot +0.1, rate +5bp, vol +0.0005 theta, +1 hour.
  const PricedSurface shifted = make_essvi(1, 5, 0.0005, kS + 0.1, kR + 0.0005, kNow + one_hour);
  const SurfaceSet bset = set_of({&base});
  const SurfaceSet sset = set_of({&shifted});

  auto pf = Portfolio::create(pnl_book());
  ASSERT_TRUE(pf.has_value());
  const PortfolioPricer pricer(std::move(*pf));
  auto a = pricer.pnl_explain(bset, sset, PriceOptions{1});
  auto b = pricer.pnl_explain(bset, sset, PriceOptions{8});
  ASSERT_TRUE(a.has_value() && b.has_value());
  const PnlFrame &f = *a;

  for (std::size_t i = 0; i < f.size(); ++i) {
    ASSERT_EQ(f.status[i], PriceStatus::Ok) << i;
    const double sum = f.pnl_delta[i] + f.pnl_gamma[i] + f.pnl_vega[i] + f.pnl_volga[i] +
                       f.pnl_vanna[i] + f.pnl_theta[i] + f.pnl_rho[i] + f.pnl_charm[i] +
                       f.pnl_unexplained[i];
    EXPECT_NEAR(sum, f.pnl_total[i], 1e-6 * (std::fabs(f.pnl_total[i]) + 1.0)) << i;
    // Thread determinism.
    EXPECT_TRUE(bits_equal(a->pnl_total[i], b->pnl_total[i])) << i;
    EXPECT_TRUE(bits_equal(a->pnl_delta[i], b->pnl_delta[i])) << i;
    EXPECT_TRUE(bits_equal(a->pnl_vega[i], b->pnl_vega[i])) << i;
    EXPECT_TRUE(bits_equal(a->pnl_unexplained[i], b->pnl_unexplained[i])) << i;
  }
  EXPECT_TRUE(bits_equal(a->total.pnl_total, b->total.pnl_total));
}

// American-consistent Taylor reconstruction. The Greeks are now AMERICAN (cold
// finite differences on american_price), so a GENUINE early-exercise surface
// (q_eff > 0, both sides) reconstructs its American reprice from
// delta/gamma/vega/volga/vanna to a tight residual — the early-exercise premium
// lives inside the coefficients, so the old q_eff = 0 (European == American)
// trick is no longer needed to isolate the pure higher-order Taylor tail.
TEST(PortfolioPricer, PnlExplain_TaylorReconstruction_Tight) {
  const double dS = 0.05;
  const double dvol_bump = 0.0004;
  const PricedSurface base = make_essvi(1, 5);                        // q_eff = 0.02
  const PricedSurface shifted = make_essvi(1, 5, dvol_bump, kS + dS); // spot + vol move
  const SurfaceSet bset = set_of({&base});
  const SurfaceSet sset = set_of({&shifted});

  // A genuine American book: both sides carry an early-exercise premium.
  std::vector<Position> book;
  std::uint64_t id = 0;
  for (double K : {96.0, 100.0, 104.0, 110.0}) {
    book.push_back({id++, {1, K, 0.18, Side::Call}, +4.0, 100.0});
    book.push_back({id++, {1, K, 0.30, Side::Put}, -3.0, 100.0});
  }
  auto pf = Portfolio::create(book);
  ASSERT_TRUE(pf.has_value());
  const PortfolioPricer pricer(std::move(*pf));
  auto er = pricer.pnl_explain(bset, sset);
  ASSERT_TRUE(er.has_value());
  const PnlFrame &f = *er;

  for (std::size_t i = 0; i < f.size(); ++i) {
    ASSERT_EQ(f.status[i], PriceStatus::Ok) << i;
    // delta+gamma+vega+volga+vanna explain the American reprice to a tight residual.
    EXPECT_LT(std::fabs(f.pnl_unexplained[i]), 5e-3 * (std::fabs(f.pnl_total[i]) + 1.0)) << i;
  }
  EXPECT_LT(std::fabs(f.total.pnl_unexplained), 5e-3 * (std::fabs(f.total.pnl_total) + 1.0));
}

TEST(PortfolioPricer, PnlExplain_MissingShiftedSurface_ModelUnavailable) {
  const PricedSurface base = make_essvi(1, 5);
  const PricedSurface other = make_essvi(2, 5);
  const SurfaceSet bset = set_of({&base});
  const SurfaceSet sset = set_of({&other}); // uid 1 absent on the shifted side

  auto pf = Portfolio::create(pnl_book());
  ASSERT_TRUE(pf.has_value());
  const PortfolioPricer pricer(std::move(*pf));
  auto er = pricer.pnl_explain(bset, sset);
  ASSERT_TRUE(er.has_value());
  for (const PriceStatus st : er->status) {
    EXPECT_EQ(st, PriceStatus::ModelUnavailable);
  }
  EXPECT_EQ(er->total.n_ok, 0u);
}

TEST(Portfolio, HundredThousandPositionsUseBoundedDedupHint) {
  constexpr std::size_t kUnique = 128;
  constexpr std::size_t kPositions = 100'000;
  std::vector<Position> positions;
  positions.reserve(kPositions);
  for (std::size_t i = 0; i < kPositions; ++i) {
    const std::size_t j = i % kUnique;
    positions.push_back(Position{
        static_cast<std::uint64_t>(i),
        OptionContract{1u, 80.0 + static_cast<double>(j), 0.25, (j & 1u) ? Side::Call : Side::Put},
        1.0, 100.0});
  }
  auto portfolio =
      Portfolio::create(positions, PortfolioBuildOptions{.expected_unique_contracts = kUnique});
  ASSERT_TRUE(portfolio.has_value());
  EXPECT_EQ(portfolio->n_positions(), kPositions);
  EXPECT_EQ(portfolio->n_contracts(), kUnique);
}

TEST(PortfolioPricer, PricesOnlyPopulatesMarksAndLeavesRiskNaN) {
  const PricedSurface surface = make_essvi(1, 5);
  const SurfaceSet surfaces = set_of({&surface});
  auto pf = Portfolio::create(pnl_book());
  ASSERT_TRUE(pf.has_value());
  const PortfolioPricer pricer(std::move(*pf));
  auto frame = pricer.price(surfaces, PriceOptions{.n_threads = 4, .prices_only = true});
  ASSERT_TRUE(frame.has_value());
  EXPECT_EQ(frame->total.n_ok, frame->size());

  // Under the Marks mask the eight Greek column vectors are left EMPTY (the
  // 64 B/pos saving), not resized-and-NaN-filled. A marks-only caller reads the
  // marks columns and gates any Greek access on greeks_materialized().
  EXPECT_FALSE(frame->greeks_materialized());
  EXPECT_EQ(frame->delta.size(), 0u);
  EXPECT_EQ(frame->gamma.size(), 0u);
  EXPECT_EQ(frame->vega.size(), 0u);
  EXPECT_EQ(frame->theta.size(), 0u);
  EXPECT_EQ(frame->rho.size(), 0u);
  EXPECT_EQ(frame->vanna.size(), 0u);
  EXPECT_EQ(frame->volga.size(), 0u);
  EXPECT_EQ(frame->charm.size(), 0u);
  for (std::size_t i = 0; i < frame->size(); ++i) {
    EXPECT_EQ(frame->status[i], PriceStatus::Ok);
    EXPECT_TRUE(std::isfinite(frame->price[i]));
    EXPECT_TRUE(std::isfinite(frame->pv[i]));
    EXPECT_TRUE(std::isfinite(frame->iv[i]));
  }

  // The aggregate must agree with the columns it aggregates. A finite 0.0 total
  // vega alongside n_ok > 0 is indistinguishable from a genuinely vega-flat book
  // -- exactly the false-flat reading the unpriced-greek accounting exists to
  // prevent. PV stays finite: prices_only computes marks, it only skips risk.
  EXPECT_GT(frame->total.n_ok, 0u);
  EXPECT_TRUE(std::isfinite(frame->total.pv));
  EXPECT_TRUE(std::isnan(frame->total.delta));
  EXPECT_TRUE(std::isnan(frame->total.gamma));
  EXPECT_TRUE(std::isnan(frame->total.vega));
  EXPECT_TRUE(std::isnan(frame->total.theta));
  EXPECT_TRUE(std::isnan(frame->total.rho));
  EXPECT_TRUE(std::isnan(frame->total.vanna));
  EXPECT_TRUE(std::isnan(frame->total.volga));
  EXPECT_TRUE(std::isnan(frame->total.charm));
}

// ── In-place API: price_into / price_totals bit-identity + zero-alloc ────────

TEST(PortfolioPricer, PriceInto_FullGreeks_BitIdenticalToPrice) {
  const PricedSurface s1 = make_convex(1, 4, 32);
  const PricedSurface s2 = make_essvi(2, 5);
  const PricedSurface s3 = make_svi(3, 4);
  const SurfaceSet surfaces = set_of({&s1, &s2, &s3});
  auto pf = Portfolio::create(multi_uid_book());
  ASSERT_TRUE(pf.has_value());
  const PortfolioPricer pricer(std::move(*pf));

  const PriceOptions opts{.n_threads = 4};
  auto ref = pricer.price(surfaces, opts);
  ASSERT_TRUE(ref.has_value());
  ASSERT_TRUE(ref->greeks_materialized());

  FrameStore fs(ref->size(), /*want_greeks=*/true);
  PortfolioWorkspace ws;
  ws.reserve(pricer.portfolio().n_contracts(), pricer.portfolio().n_positions());
  PriceFrameView v = fs.view();
  const Status s = pricer.price_into(surfaces, PriceFieldMask::FullGreeks, v, ws, opts);
  ASSERT_TRUE(s.has_value());

  for (std::size_t i = 0; i < ref->size(); ++i) {
    EXPECT_EQ(fs.id[i], ref->id[i]) << i;
    EXPECT_EQ(fs.uid[i], ref->uid[i]) << i;
    EXPECT_EQ(fs.status[i], ref->status[i]) << i;
    EXPECT_TRUE(bits_equal(fs.iv[i], ref->iv[i])) << i;
    EXPECT_TRUE(bits_equal(fs.pv[i], ref->pv[i])) << i;
    EXPECT_TRUE(bits_equal(fs.price[i], ref->price[i])) << i;
    EXPECT_TRUE(bits_equal(fs.delta[i], ref->delta[i])) << i;
    EXPECT_TRUE(bits_equal(fs.gamma[i], ref->gamma[i])) << i;
    EXPECT_TRUE(bits_equal(fs.vega[i], ref->vega[i])) << i;
    EXPECT_TRUE(bits_equal(fs.theta[i], ref->theta[i])) << i;
    EXPECT_TRUE(bits_equal(fs.rho[i], ref->rho[i])) << i;
    EXPECT_TRUE(bits_equal(fs.vanna[i], ref->vanna[i])) << i;
    EXPECT_TRUE(bits_equal(fs.volga[i], ref->volga[i])) << i;
    EXPECT_TRUE(bits_equal(fs.charm[i], ref->charm[i])) << i;
  }
  EXPECT_EQ(fs.total.n_ok, ref->total.n_ok);
  EXPECT_TRUE(bits_equal(fs.total.pv, ref->total.pv));
  EXPECT_TRUE(bits_equal(fs.total.delta, ref->total.delta));
  EXPECT_TRUE(bits_equal(fs.total.gamma, ref->total.gamma));
  EXPECT_TRUE(bits_equal(fs.total.vega, ref->total.vega));
  EXPECT_TRUE(bits_equal(fs.total.theta, ref->total.theta));
  EXPECT_TRUE(bits_equal(fs.total.rho, ref->total.rho));
  EXPECT_TRUE(bits_equal(fs.total.vanna, ref->total.vanna));
  EXPECT_TRUE(bits_equal(fs.total.volga, ref->total.volga));
  EXPECT_TRUE(bits_equal(fs.total.charm, ref->total.charm));
}

TEST(PortfolioPricer, PriceInto_Marks_MarksBitIdentical_GreeksUntouched) {
  const PricedSurface s1 = make_convex(1, 4, 32);
  const PricedSurface s2 = make_essvi(2, 5);
  const PricedSurface s3 = make_svi(3, 4);
  const SurfaceSet surfaces = set_of({&s1, &s2, &s3});
  auto pf = Portfolio::create(multi_uid_book());
  ASSERT_TRUE(pf.has_value());
  const PortfolioPricer pricer(std::move(*pf));

  auto ref = pricer.price(surfaces, PriceOptions{.n_threads = 4, .prices_only = true});
  ASSERT_TRUE(ref.has_value());
  ASSERT_FALSE(ref->greeks_materialized());

  // A marks-only caller allocates NO greek storage; the greek spans stay empty.
  FrameStore fs(ref->size(), /*want_greeks=*/false);
  PortfolioWorkspace ws;
  PriceFrameView v = fs.view();
  const Status s =
      pricer.price_into(surfaces, PriceFieldMask::Marks, v, ws, PriceOptions{.n_threads = 4});
  ASSERT_TRUE(s.has_value());

  for (std::size_t i = 0; i < ref->size(); ++i) {
    EXPECT_EQ(fs.id[i], ref->id[i]) << i;
    EXPECT_EQ(fs.uid[i], ref->uid[i]) << i;
    EXPECT_EQ(fs.status[i], ref->status[i]) << i;
    EXPECT_TRUE(bits_equal(fs.iv[i], ref->iv[i])) << i;
    EXPECT_TRUE(bits_equal(fs.pv[i], ref->pv[i])) << i;
    EXPECT_TRUE(bits_equal(fs.price[i], ref->price[i])) << i;
  }
  // Greek spans were never touched (empty), matching the returning API's Marks.
  EXPECT_EQ(fs.delta.size(), 0u);
  EXPECT_EQ(fs.charm.size(), 0u);

  // pv total + n_ok bit-identical; greek totals stay NaN (not a clean zero).
  EXPECT_EQ(fs.total.n_ok, ref->total.n_ok);
  EXPECT_TRUE(bits_equal(fs.total.pv, ref->total.pv));
  EXPECT_TRUE(std::isnan(fs.total.delta));
  EXPECT_TRUE(std::isnan(fs.total.vega));
  EXPECT_TRUE(std::isnan(fs.total.charm));
}

TEST(PortfolioPricer, PriceTotals_BitIdenticalToPriceTotal_BothMasks) {
  const PricedSurface s1 = make_convex(1, 4, 32);
  const PricedSurface s2 = make_essvi(2, 5);
  const PricedSurface s3 = make_svi(3, 4);
  const SurfaceSet surfaces = set_of({&s1, &s2, &s3});
  auto pf = Portfolio::create(multi_uid_book());
  ASSERT_TRUE(pf.has_value());
  const PortfolioPricer pricer(std::move(*pf));

  const PriceOptions opts{.n_threads = 4};
  auto ref_full = pricer.price(surfaces, opts);
  auto ref_marks = pricer.price(surfaces, PriceOptions{.n_threads = 4, .prices_only = true});
  ASSERT_TRUE(ref_full.has_value() && ref_marks.has_value());

  PortfolioWorkspace ws;
  auto tf = pricer.price_totals(surfaces, PriceFieldMask::FullGreeks, ws, opts);
  auto tm = pricer.price_totals(surfaces, PriceFieldMask::Marks, ws, opts);
  ASSERT_TRUE(tf.has_value() && tm.has_value());

  // FullGreeks: every total field bit-identical to price().total.
  EXPECT_EQ(tf->n_ok, ref_full->total.n_ok);
  EXPECT_TRUE(bits_equal(tf->pv, ref_full->total.pv));
  EXPECT_TRUE(bits_equal(tf->delta, ref_full->total.delta));
  EXPECT_TRUE(bits_equal(tf->gamma, ref_full->total.gamma));
  EXPECT_TRUE(bits_equal(tf->vega, ref_full->total.vega));
  EXPECT_TRUE(bits_equal(tf->theta, ref_full->total.theta));
  EXPECT_TRUE(bits_equal(tf->rho, ref_full->total.rho));
  EXPECT_TRUE(bits_equal(tf->vanna, ref_full->total.vanna));
  EXPECT_TRUE(bits_equal(tf->volga, ref_full->total.volga));
  EXPECT_TRUE(bits_equal(tf->charm, ref_full->total.charm));

  // Marks: pv + n_ok bit-identical, greek sums NaN.
  EXPECT_EQ(tm->n_ok, ref_marks->total.n_ok);
  EXPECT_TRUE(bits_equal(tm->pv, ref_marks->total.pv));
  EXPECT_TRUE(std::isnan(tm->delta));
  EXPECT_TRUE(std::isnan(tm->vega));
}

TEST(PortfolioPricer, PriceInto_ThreadCounts_TotalsBitIdentical) {
  std::vector<PricedSurface> surfs;
  std::vector<const PricedSurface *> ptrs;
  std::vector<Position> book;
  for (std::uint32_t u = 1; u <= 6; ++u) {
    surfs.push_back((u & 1u) ? make_convex(u, 4, 28) : make_essvi(u, 5));
  }
  for (const PricedSurface &s : surfs) {
    ptrs.push_back(&s);
  }
  const SurfaceSet surfaces = set_of(ptrs);
  std::uint64_t id = 0;
  for (std::uint32_t u = 1; u <= 6; ++u) {
    for (double K : {90.0, 95.0, 100.0, 105.0, 110.0}) {
      book.push_back({id++, {u, K, 0.18, Side::Call}, 1.0 + 0.1 * static_cast<double>(u), 100.0});
      book.push_back({id++, {u, K, 0.28, Side::Put}, -2.0, 100.0});
    }
  }
  auto pf = Portfolio::create(book);
  ASSERT_TRUE(pf.has_value());
  const PortfolioPricer pricer(std::move(*pf));
  const std::size_t n = pricer.portfolio().n_positions();

  PortfolioWorkspace ws;
  PriceTotals totals[4];
  const unsigned thread_counts[4] = {1, 2, 4, 8};
  for (int k = 0; k < 4; ++k) {
    FrameStore fs(n, /*want_greeks=*/true);
    PriceFrameView v = fs.view();
    ASSERT_TRUE(pricer
                    .price_into(surfaces, PriceFieldMask::FullGreeks, v, ws,
                                PriceOptions{.n_threads = thread_counts[k]})
                    .has_value());
    totals[k] = fs.total;
  }
  for (int k = 1; k < 4; ++k) {
    EXPECT_TRUE(bits_equal(totals[k].pv, totals[0].pv)) << k;
    EXPECT_TRUE(bits_equal(totals[k].delta, totals[0].delta)) << k;
    EXPECT_TRUE(bits_equal(totals[k].vega, totals[0].vega)) << k;
    EXPECT_EQ(totals[k].n_ok, totals[0].n_ok) << k;
  }
}

TEST(PortfolioPricer, MarksResolvedRoutesAreThreadDeterministicAndScalarCompatible) {
  const PricedSurface surface = make_essvi(1, 5);
  const SurfaceSet surfaces = set_of({&surface});
  auto pf = Portfolio::create(tiled_marks_book());
  ASSERT_TRUE(pf.has_value());
  const PortfolioPricer pricer(std::move(*pf));
  const std::size_t n = pricer.portfolio().n_positions();
  PortfolioWorkspace ws;
  ws.reserve(pricer.portfolio().n_contracts(), n);
  FrameStore scalar_ref(n, /*want_greeks=*/false);
  FrameStore avx_ref(n, /*want_greeks=*/false);
  const unsigned thread_counts[] = {1, 2, 4, 8};
  const simd::SimdIsa routes[] = {simd::SimdIsa::ForceScalar,
                                  simd::SimdIsa::ForceAvx2};

  for (std::size_t route_ix = 0; route_ix < 2; ++route_ix) {
    FrameStore& route_ref = (route_ix == 0) ? scalar_ref : avx_ref;
    for (std::size_t thread_ix = 0; thread_ix < 4; ++thread_ix) {
      FrameStore actual(n, /*want_greeks=*/false);
      const PriceOptions opts{.n_threads = thread_counts[thread_ix],
                              .resolved_price_isa = routes[route_ix]};
      ASSERT_TRUE(pricer.price_into(surfaces, PriceFieldMask::Marks, actual.view(), ws, opts)
                      .has_value());
      auto totals = pricer.price_totals(surfaces, PriceFieldMask::Marks, ws, opts);
      ASSERT_TRUE(totals.has_value());
      expect_totals_bit_identical(*totals, actual.total);
      if (thread_ix == 0) {
        route_ref = std::move(actual);
      } else {
        expect_frame_bit_identical(actual, route_ref);
      }
    }
  }

  double total_gate = 0.0;
  const auto positions = pricer.portfolio().positions();
  for (std::size_t i = 0; i < n; ++i) {
    EXPECT_EQ(avx_ref.status[i], scalar_ref.status[i]) << i;
    EXPECT_TRUE(bits_equal(avx_ref.iv[i], scalar_ref.iv[i])) << i;
    ASSERT_EQ(scalar_ref.status[i], PriceStatus::Ok) << i;
    EXPECT_LE(std::abs(avx_ref.price[i] - scalar_ref.price[i]), 1.0e-6) << i;
    const double weight = std::abs(positions[i].qty * positions[i].multiplier);
    EXPECT_LE(std::abs(avx_ref.pv[i] - scalar_ref.pv[i]), weight * 1.0e-6) << i;
    total_gate += weight * 1.0e-6;
  }
  EXPECT_LE(std::abs(avx_ref.total.pv - scalar_ref.total.pv), total_gate);
}

// Zero-allocation proof + retained-substrate reuse. The assertions are only
// meaningful under -DATX_VOL_COUNTERS=ON; the OFF build compiles it as a
// disabled-sentinel check so the default suite still exercises the in-place path.
TEST(PortfolioPricer, PriceInto_ZeroAllocation_And_PreparedReuse) {
  using atx::vol::counters::Counter;
  using atx::vol::counters::counters_enabled;
  const PricedSurface surface = make_essvi(1, 5);
  const SurfaceSet surfaces = set_of({&surface});
  auto pf = Portfolio::create(pnl_book());
  ASSERT_TRUE(pf.has_value());
  const PortfolioPricer pricer(std::move(*pf));
  const std::size_t n = pricer.portfolio().n_positions();
  const std::size_t nu = pricer.portfolio().n_contracts();

  PortfolioWorkspace ws;
  ws.reserve(nu, n);
  FrameStore fg(n, /*want_greeks=*/true);
  PriceFrameView vg = fg.view();
  // Warm up: first call builds the retained PreparedPortfolio + sizes the scratch.
  ASSERT_TRUE(pricer.price_into(surfaces, PriceFieldMask::FullGreeks, vg, ws).has_value());

  if constexpr (counters_enabled()) {
    // Second FullGreeks call: reuses the substrate, allocates no frame.
    atx::vol::counters::reset();
    ASSERT_TRUE(pricer.price_into(surfaces, PriceFieldMask::FullGreeks, vg, ws).has_value());
    auto sg = atx::vol::counters::snapshot();
    EXPECT_EQ(sg.get(Counter::FrameAllocations), 0u);
    EXPECT_EQ(sg.get(Counter::FrameBytes), std::uint64_t{101} * n);
    EXPECT_EQ(sg.get(Counter::PreparedBuilds), 0u); // reused, not rebuilt

    // Marks touches 37 B/pos and still allocates nothing. The retained substrate
    // is shared across masks (it depends only on (uid,side,T), not the Greek
    // route), so this Marks call reuses the SAME PreparedPortfolio built above
    // -- no rebuild -- and the reuse is re-measured below.
    FrameStore fm(n, /*want_greeks=*/false);
    PriceFrameView vm = fm.view();
    ASSERT_TRUE(pricer.price_into(surfaces, PriceFieldMask::Marks, vm, ws).has_value());
    atx::vol::counters::reset();
    ASSERT_TRUE(pricer.price_into(surfaces, PriceFieldMask::Marks, vm, ws).has_value());
    auto sm = atx::vol::counters::snapshot();
    EXPECT_EQ(sm.get(Counter::FrameAllocations), 0u);
    EXPECT_EQ(sm.get(Counter::FrameBytes), std::uint64_t{37} * n);
    EXPECT_EQ(sm.get(Counter::PreparedBuilds), 0u);
    EXPECT_GT(sm.get(Counter::ResolvedPriceWrapperCalls), 0u);
    EXPECT_GE(sm.get(Counter::ResolvedPriceWrapperLanes), nu);
    EXPECT_EQ(sm.get(Counter::AmericanAvxPackDispatches), 0u);
    EXPECT_GE(sm.get(Counter::AmericanWrapperKnownScalarLanes), nu);
  } else {
    EXPECT_FALSE(atx::vol::counters::snapshot().enabled);
    SUCCEED();
  }
}

// Exact same-address ABA regression: both books have the same unique-contract
// count and bit-identical first contract, so address/count/first-contract gates
// all collide while the middle of the books differs. The logical-book identity
// must force a rebuild and produce the same result as a fresh workspace.
TEST(PortfolioPricer, PriceInto_WorkspaceReuseAcrossDifferentBooksAtSamePricerAddress) {
  const PricedSurface s1 = make_convex(1, 4, 32);
  const PricedSurface s2 = make_essvi(2, 5);
  const PricedSurface s3 = make_svi(3, 4);
  const SurfaceSet surfaces = set_of({&s1, &s2, &s3});

  // Book A: 6 unique contracts.
  std::vector<Position> book_a;
  std::uint64_t id_a = 0;
  for (double K : {90.0, 95.0, 100.0, 105.0, 110.0, 115.0}) {
    book_a.push_back({id_a++, {1, K, 0.18, Side::Call}, 1.0, 100.0});
  }
  auto pf_a = Portfolio::create(book_a);
  ASSERT_TRUE(pf_a.has_value());
  ASSERT_EQ(pf_a->n_contracts(), 6u);

  // Book B: same count and first contract, different remaining content.
  const std::vector<Position> book_b{
      {100, {1, 90.0, 0.18, Side::Call}, 3.0, 100.0},
      {101, {2, 105.0, 0.25, Side::Call}, 7.0, 100.0},
      {102, {3, 98.0, 0.15, Side::Put}, -2.0, 100.0},
      {103, {1, 102.0, 0.28, Side::Put}, 4.0, 100.0},
      {104, {2, 110.0, 0.35, Side::Put}, -1.0, 100.0},
      {105, {3, 105.0, 0.25, Side::Call}, 5.0, 100.0},
  };
  auto pf_b = Portfolio::create(book_b);
  ASSERT_TRUE(pf_b.has_value());
  ASSERT_EQ(pf_b->n_contracts(), 6u);

  PortfolioWorkspace ws;
  PortfolioPricer pr(std::move(*pf_a));
  const void *addr_before = static_cast<const void *>(&pr);

  {
    const std::size_t na = pr.portfolio().n_positions();
    FrameStore fa(na, /*want_greeks=*/true);
    ASSERT_TRUE(pr.price_into(surfaces, PriceFieldMask::FullGreeks, fa.view(), ws).has_value());
  }

  // Reassign the SAME variable to a different same-sized book. `pr` occupies one
  // stack slot for its whole lifetime, so its (and its `pf_` member's) address
  // is unchanged by the reassignment.
  pr = PortfolioPricer(std::move(*pf_b));
  ASSERT_EQ(static_cast<const void *>(&pr), addr_before);
  const std::size_t nb = pr.portfolio().n_positions();

  FrameStore fb_reused(nb, /*want_greeks=*/true);
  ASSERT_TRUE(
      pr.price_into(surfaces, PriceFieldMask::FullGreeks, fb_reused.view(), ws).has_value());

  // Reference: a FRESH workspace pricing the SAME book B.
  PortfolioWorkspace ws_fresh;
  FrameStore fb_fresh(nb, /*want_greeks=*/true);
  ASSERT_TRUE(
      pr.price_into(surfaces, PriceFieldMask::FullGreeks, fb_fresh.view(), ws_fresh).has_value());

  ASSERT_EQ(fb_reused.status.size(), fb_fresh.status.size());
  for (std::size_t i = 0; i < nb; ++i) {
    EXPECT_EQ(fb_reused.status[i], fb_fresh.status[i]) << i;
    EXPECT_TRUE(bits_equal(fb_reused.pv[i], fb_fresh.pv[i])) << i;
    EXPECT_TRUE(bits_equal(fb_reused.delta[i], fb_fresh.delta[i])) << i;
    EXPECT_TRUE(bits_equal(fb_reused.gamma[i], fb_fresh.gamma[i])) << i;
    EXPECT_TRUE(bits_equal(fb_reused.vega[i], fb_fresh.vega[i])) << i;
    EXPECT_TRUE(bits_equal(fb_reused.theta[i], fb_fresh.theta[i])) << i;
    EXPECT_TRUE(bits_equal(fb_reused.rho[i], fb_fresh.rho[i])) << i;
    EXPECT_TRUE(bits_equal(fb_reused.vanna[i], fb_fresh.vanna[i])) << i;
    EXPECT_TRUE(bits_equal(fb_reused.volga[i], fb_fresh.volga[i])) << i;
    EXPECT_TRUE(bits_equal(fb_reused.charm[i], fb_fresh.charm[i])) << i;
  }
  EXPECT_TRUE(bits_equal(fb_reused.total.pv, fb_fresh.total.pv));
}

// Minor #2 regression: PreparedPortfolio's permutation/groups/oci/aligned
// columns depend only on (uid,side,T), not the Greek route, so alternating
// Marks <-> FullGreeks on one warm workspace must NOT rebuild the substrate --
// exactly one PreparedPortfolio build for the whole sequence (the initial
// warm-up before the counter reset below). Meaningful only under
// -DATX_VOL_COUNTERS=ON; the OFF build checks the disabled-sentinel so the
// default suite still exercises the mask-alternation path.
TEST(PortfolioPricer, PriceInto_AlternatingMasks_NoRebuild) {
  using atx::vol::counters::Counter;
  using atx::vol::counters::counters_enabled;
  const PricedSurface surface = make_essvi(1, 5);
  const SurfaceSet surfaces = set_of({&surface});
  auto pf = Portfolio::create(pnl_book());
  ASSERT_TRUE(pf.has_value());
  const PortfolioPricer pricer(std::move(*pf));
  const std::size_t n = pricer.portfolio().n_positions();

  PortfolioWorkspace ws;
  FrameStore fg(n, /*want_greeks=*/true);
  FrameStore fm(n, /*want_greeks=*/false);
  PriceFrameView vg = fg.view();
  PriceFrameView vm = fm.view();
  const PriceOptions greek_opts{.n_threads = 4,
                                .resolved_price_isa = simd::SimdIsa::ForceScalar};
  const PriceOptions mark_opts{.n_threads = 4,
                               .resolved_price_isa = simd::SimdIsa::ForceAvx2};

  // Warm-up build (the mask does not affect the substrate it builds).
  ASSERT_TRUE(
      pricer.price_into(surfaces, PriceFieldMask::FullGreeks, vg, ws, greek_opts).has_value());

  if constexpr (counters_enabled()) {
    atx::vol::counters::reset();
  }
  ASSERT_TRUE(pricer.price_into(surfaces, PriceFieldMask::Marks, vm, ws, mark_opts).has_value());
  ASSERT_TRUE(
      pricer.price_into(surfaces, PriceFieldMask::FullGreeks, vg, ws, greek_opts).has_value());
  ASSERT_TRUE(pricer.price_into(surfaces, PriceFieldMask::Marks, vm, ws, mark_opts).has_value());
  ASSERT_TRUE(
      pricer.price_into(surfaces, PriceFieldMask::FullGreeks, vg, ws, greek_opts).has_value());
  if constexpr (counters_enabled()) {
    const auto snap = atx::vol::counters::snapshot();
    EXPECT_EQ(snap.get(Counter::PreparedBuilds), 0u); // reused across every mask/ISA flip
  } else {
    EXPECT_FALSE(atx::vol::counters::snapshot().enabled);
  }
}

// P1.4 steady-state proof: the persistent pricing pool creates its worker threads
// ONCE (at first use), so a warmed reprise at n_threads>1 creates NO threads. The
// WorkerLaunches counter now means "pool worker threads actually created"; after a
// warm-up it reads 0 for every subsequent snapshot (mirrors T6's FrameAllocations
// proof). Meaningful only under -DATX_VOL_COUNTERS=ON.
TEST(PortfolioPricer, PriceInto_SteadyState_NoThreadCreation) {
  using atx::vol::counters::Counter;
  using atx::vol::counters::counters_enabled;
  const PricedSurface s1 = make_convex(1, 4, 32);
  const PricedSurface s2 = make_essvi(2, 5);
  const PricedSurface s3 = make_svi(3, 4);
  const SurfaceSet surfaces = set_of({&s1, &s2, &s3});
  auto pf = Portfolio::create(multi_uid_book());
  ASSERT_TRUE(pf.has_value());
  const PortfolioPricer pricer(std::move(*pf));
  const std::size_t n = pricer.portfolio().n_positions();

  PortfolioWorkspace ws;
  ws.reserve(pricer.portfolio().n_contracts(), n);
  FrameStore fs(n, /*want_greeks=*/true);
  PriceFrameView v = fs.view();
  const PriceOptions opts{.n_threads = 4};
  // Warm-up: builds the substrate AND ensures the process pool's workers exist.
  ASSERT_TRUE(pricer.price_into(surfaces, PriceFieldMask::FullGreeks, v, ws, opts).has_value());

  if constexpr (counters_enabled()) {
    atx::vol::counters::reset();
    // Second reprice at n_threads>1: reuses the pool, creates no thread.
    ASSERT_TRUE(pricer.price_into(surfaces, PriceFieldMask::FullGreeks, v, ws, opts).has_value());
    const auto snap = atx::vol::counters::snapshot();
    EXPECT_EQ(snap.get(Counter::WorkerLaunches), 0u); // no threads created in steady state
  } else {
    EXPECT_FALSE(atx::vol::counters::snapshot().enabled);
    SUCCEED();
  }
}

// Repeated concurrent stress: reprice a fixed multi-uid book many times at
// n_threads in {2,4,8} and assert every frame column + total is bit-identical to
// the single-thread reference. A data race on the disjoint-write scatter/solve
// would surface as nondeterminism here. (clang-cl/Windows has no usable TSan, so
// this repeated bit-identity IS the race evidence — see pricing_executor_test.cpp.)
TEST(PortfolioPricer, PriceInto_RepeatedThreadCounts_BitIdentical) {
  const PricedSurface surface = make_essvi(1, 5);
  const SurfaceSet surfaces = set_of({&surface});
  auto pf = Portfolio::create(pnl_book());
  ASSERT_TRUE(pf.has_value());
  const PortfolioPricer pricer(std::move(*pf));
  const std::size_t n = pricer.portfolio().n_positions();

  PortfolioWorkspace ws;
  FrameStore ref(n, /*want_greeks=*/true);
  {
    PriceFrameView rv = ref.view();
    ASSERT_TRUE(
        pricer
            .price_into(surfaces, PriceFieldMask::FullGreeks, rv, ws, PriceOptions{.n_threads = 1})
            .has_value());
  }

  // 2 reps: cross-thread bit-identity is deterministic; soak lives in bench.
  for (int rep = 0; rep < 2; ++rep) {
    for (unsigned nt : {2u, 4u, 8u}) {
      FrameStore fs(n, /*want_greeks=*/true);
      PriceFrameView v = fs.view();
      ASSERT_TRUE(pricer
                      .price_into(surfaces, PriceFieldMask::FullGreeks, v, ws,
                                  PriceOptions{.n_threads = nt})
                      .has_value());
      for (std::size_t i = 0; i < n; ++i) {
        ASSERT_TRUE(bits_equal(fs.pv[i], ref.pv[i])) << "rep=" << rep << " nt=" << nt << " i=" << i;
        ASSERT_TRUE(bits_equal(fs.price[i], ref.price[i])) << rep << " " << nt << " " << i;
        ASSERT_TRUE(bits_equal(fs.delta[i], ref.delta[i])) << rep << " " << nt << " " << i;
        ASSERT_TRUE(bits_equal(fs.gamma[i], ref.gamma[i])) << rep << " " << nt << " " << i;
        ASSERT_TRUE(bits_equal(fs.vega[i], ref.vega[i])) << rep << " " << nt << " " << i;
        ASSERT_TRUE(bits_equal(fs.theta[i], ref.theta[i])) << rep << " " << nt << " " << i;
      }
      ASSERT_TRUE(bits_equal(fs.total.pv, ref.total.pv)) << "rep=" << rep << " nt=" << nt;
      ASSERT_TRUE(bits_equal(fs.total.delta, ref.total.delta)) << "rep=" << rep << " nt=" << nt;
      ASSERT_EQ(fs.total.n_ok, ref.total.n_ok);
    }
  }
}

// An over-sized hint is advisory, not load-bearing: it must not change the dedup
// result, and must not reach reserve() unclamped.
TEST(PortfolioPricer, OversizedUniqueContractHintIsClampedAndHarmless) {
  const auto positions = pnl_book();
  auto exact = Portfolio::create(positions);
  ASSERT_TRUE(exact.has_value());

  auto hinted = Portfolio::create(
      positions, PortfolioBuildOptions{.expected_unique_contracts = 1'000'000'000u});
  ASSERT_TRUE(hinted.has_value());
  EXPECT_EQ(hinted->n_positions(), exact->n_positions());
  EXPECT_EQ(hinted->n_contracts(), exact->n_contracts());
}

// ── In-place P&L API: pnl_explain_into / pnl_totals bit-identity + zero-alloc ─

namespace {

// Base + shifted (combined-bump) essvi surface sets over uids 1/2/3 for the P&L
// in-place tests. The shifted set moves spot +0.1, rate +5bp, vol +0.0005 theta,
// and +1 hour, so every Taylor axis is live. Owns the surfaces; the pointer
// vectors are populated only after both surface vectors stop growing.
struct PnlSurfaces {
  std::vector<PricedSurface> base_s;
  std::vector<PricedSurface> shift_s;
  std::vector<const PricedSurface *> base_p;
  std::vector<const PricedSurface *> shift_p;

  PnlSurfaces() {
    const std::int64_t one_hour = static_cast<std::int64_t>(3600.0 * 1e9);
    for (std::uint32_t u : {1u, 2u, 3u}) {
      base_s.push_back(make_essvi(u, 5));
    }
    for (std::uint32_t u : {1u, 2u, 3u}) {
      shift_s.push_back(make_essvi(u, 5, 0.0005, kS + 0.1, kR + 0.0005, kNow + one_hour));
    }
    for (const PricedSurface &s : base_s) {
      base_p.push_back(&s);
    }
    for (const PricedSurface &s : shift_s) {
      shift_p.push_back(&s);
    }
  }

  [[nodiscard]] SurfaceSet base() const { return set_of(base_p); }
  [[nodiscard]] SurfaceSet shifted() const { return set_of(shift_p); }
};

} // namespace

TEST(PortfolioPricer, PnlExplainInto_BitIdenticalToPnlExplain) {
  const PnlSurfaces surf;
  const SurfaceSet base = surf.base();
  const SurfaceSet shifted = surf.shifted();
  auto pf = Portfolio::create(pnl_multi_book());
  ASSERT_TRUE(pf.has_value());
  const PortfolioPricer pricer(std::move(*pf));

  const PriceOptions opts{.n_threads = 4};
  auto ref = pricer.pnl_explain(base, shifted, opts);
  ASSERT_TRUE(ref.has_value());

  PnlFrameStore fs(ref->size());
  PortfolioWorkspace ws;
  ws.reserve(pricer.portfolio().n_contracts(), pricer.portfolio().n_positions());
  PnlFrameView v = fs.view();
  const Status s = pricer.pnl_explain_into(base, shifted, v, ws, opts);
  ASSERT_TRUE(s.has_value());

  for (std::size_t i = 0; i < ref->size(); ++i) {
    EXPECT_EQ(fs.id[i], ref->id[i]) << i;
    EXPECT_EQ(fs.uid[i], ref->uid[i]) << i;
    EXPECT_EQ(fs.status[i], ref->status[i]) << i;
    EXPECT_TRUE(bits_equal(fs.pv_base[i], ref->pv_base[i])) << i;
    EXPECT_TRUE(bits_equal(fs.pv_target[i], ref->pv_target[i])) << i;
    EXPECT_TRUE(bits_equal(fs.pnl_total[i], ref->pnl_total[i])) << i;
    EXPECT_TRUE(bits_equal(fs.pnl_delta[i], ref->pnl_delta[i])) << i;
    EXPECT_TRUE(bits_equal(fs.pnl_gamma[i], ref->pnl_gamma[i])) << i;
    EXPECT_TRUE(bits_equal(fs.pnl_vega[i], ref->pnl_vega[i])) << i;
    EXPECT_TRUE(bits_equal(fs.pnl_volga[i], ref->pnl_volga[i])) << i;
    EXPECT_TRUE(bits_equal(fs.pnl_vanna[i], ref->pnl_vanna[i])) << i;
    EXPECT_TRUE(bits_equal(fs.pnl_theta[i], ref->pnl_theta[i])) << i;
    EXPECT_TRUE(bits_equal(fs.pnl_rho[i], ref->pnl_rho[i])) << i;
    EXPECT_TRUE(bits_equal(fs.pnl_charm[i], ref->pnl_charm[i])) << i;
    EXPECT_TRUE(bits_equal(fs.pnl_unexplained[i], ref->pnl_unexplained[i])) << i;
    EXPECT_TRUE(bits_equal(fs.d_spot[i], ref->d_spot[i])) << i;
    EXPECT_TRUE(bits_equal(fs.d_vol[i], ref->d_vol[i])) << i;
    EXPECT_TRUE(bits_equal(fs.d_time[i], ref->d_time[i])) << i;
    EXPECT_TRUE(bits_equal(fs.d_rate[i], ref->d_rate[i])) << i;
  }
  EXPECT_EQ(fs.total.n_ok, ref->total.n_ok);
  EXPECT_TRUE(bits_equal(fs.total.pv_base, ref->total.pv_base));
  EXPECT_TRUE(bits_equal(fs.total.pv_target, ref->total.pv_target));
  EXPECT_TRUE(bits_equal(fs.total.pnl_total, ref->total.pnl_total));
  EXPECT_TRUE(bits_equal(fs.total.pnl_delta, ref->total.pnl_delta));
  EXPECT_TRUE(bits_equal(fs.total.pnl_gamma, ref->total.pnl_gamma));
  EXPECT_TRUE(bits_equal(fs.total.pnl_vega, ref->total.pnl_vega));
  EXPECT_TRUE(bits_equal(fs.total.pnl_volga, ref->total.pnl_volga));
  EXPECT_TRUE(bits_equal(fs.total.pnl_vanna, ref->total.pnl_vanna));
  EXPECT_TRUE(bits_equal(fs.total.pnl_theta, ref->total.pnl_theta));
  EXPECT_TRUE(bits_equal(fs.total.pnl_rho, ref->total.pnl_rho));
  EXPECT_TRUE(bits_equal(fs.total.pnl_charm, ref->total.pnl_charm));
  EXPECT_TRUE(bits_equal(fs.total.pnl_unexplained, ref->total.pnl_unexplained));
}

TEST(PortfolioPricer, PnlTotals_BitIdenticalToPnlExplainTotal) {
  const PnlSurfaces surf;
  const SurfaceSet base = surf.base();
  const SurfaceSet shifted = surf.shifted();
  auto pf = Portfolio::create(pnl_multi_book());
  ASSERT_TRUE(pf.has_value());
  const PortfolioPricer pricer(std::move(*pf));

  const PriceOptions opts{.n_threads = 4};
  auto ref = pricer.pnl_explain(base, shifted, opts);
  ASSERT_TRUE(ref.has_value());

  PortfolioWorkspace ws;
  auto t = pricer.pnl_totals(base, shifted, ws, opts);
  ASSERT_TRUE(t.has_value());

  EXPECT_EQ(t->n_ok, ref->total.n_ok);
  EXPECT_TRUE(bits_equal(t->pv_base, ref->total.pv_base));
  EXPECT_TRUE(bits_equal(t->pv_target, ref->total.pv_target));
  EXPECT_TRUE(bits_equal(t->pnl_total, ref->total.pnl_total));
  EXPECT_TRUE(bits_equal(t->pnl_delta, ref->total.pnl_delta));
  EXPECT_TRUE(bits_equal(t->pnl_gamma, ref->total.pnl_gamma));
  EXPECT_TRUE(bits_equal(t->pnl_vega, ref->total.pnl_vega));
  EXPECT_TRUE(bits_equal(t->pnl_volga, ref->total.pnl_volga));
  EXPECT_TRUE(bits_equal(t->pnl_vanna, ref->total.pnl_vanna));
  EXPECT_TRUE(bits_equal(t->pnl_theta, ref->total.pnl_theta));
  EXPECT_TRUE(bits_equal(t->pnl_rho, ref->total.pnl_rho));
  EXPECT_TRUE(bits_equal(t->pnl_charm, ref->total.pnl_charm));
  EXPECT_TRUE(bits_equal(t->pnl_unexplained, ref->total.pnl_unexplained));
}

TEST(PortfolioPricer, PnlExplainInto_ThreadCounts_TotalsBitIdentical) {
  const PnlSurfaces surf;
  const SurfaceSet base = surf.base();
  const SurfaceSet shifted = surf.shifted();
  auto pf = Portfolio::create(pnl_multi_book());
  ASSERT_TRUE(pf.has_value());
  const PortfolioPricer pricer(std::move(*pf));
  const std::size_t n = pricer.portfolio().n_positions();

  PortfolioWorkspace ws;
  PnlTotals totals[4];
  const unsigned thread_counts[4] = {1, 2, 4, 8};
  for (int k = 0; k < 4; ++k) {
    PnlFrameStore fs(n);
    PnlFrameView v = fs.view();
    ASSERT_TRUE(
        pricer.pnl_explain_into(base, shifted, v, ws, PriceOptions{.n_threads = thread_counts[k]})
            .has_value());
    totals[k] = fs.total;
  }
  for (int k = 1; k < 4; ++k) {
    EXPECT_TRUE(bits_equal(totals[k].pnl_total, totals[0].pnl_total)) << k;
    EXPECT_TRUE(bits_equal(totals[k].pnl_delta, totals[0].pnl_delta)) << k;
    EXPECT_TRUE(bits_equal(totals[k].pnl_vega, totals[0].pnl_vega)) << k;
    EXPECT_TRUE(bits_equal(totals[k].pnl_unexplained, totals[0].pnl_unexplained)) << k;
    EXPECT_EQ(totals[k].n_ok, totals[0].n_ok) << k;
  }
}

// Zero-allocation proof + retained-substrate reuse for the P&L path. Meaningful
// only under -DATX_VOL_COUNTERS=ON; the OFF build still exercises the in-place path.
TEST(PortfolioPricer, PnlExplainInto_ZeroAllocation) {
  using atx::vol::counters::Counter;
  using atx::vol::counters::counters_enabled;
  const PnlSurfaces surf;
  const SurfaceSet base = surf.base();
  const SurfaceSet shifted = surf.shifted();
  auto pf = Portfolio::create(pnl_multi_book());
  ASSERT_TRUE(pf.has_value());
  const PortfolioPricer pricer(std::move(*pf));
  const std::size_t n = pricer.portfolio().n_positions();
  const std::size_t nu = pricer.portfolio().n_contracts();

  PortfolioWorkspace ws;
  ws.reserve(nu, n);
  PnlFrameStore fs(n);
  PnlFrameView v = fs.view();
  // Warm up: first call builds the retained PreparedPortfolio + sizes the scratch.
  ASSERT_TRUE(pricer.pnl_explain_into(base, shifted, v, ws).has_value());

  if constexpr (counters_enabled()) {
    atx::vol::counters::reset();
    ASSERT_TRUE(pricer.pnl_explain_into(base, shifted, v, ws).has_value());
    auto s = atx::vol::counters::snapshot();
    EXPECT_EQ(s.get(Counter::FrameAllocations), 0u); // caller-owned spans + reused scratch
    EXPECT_EQ(s.get(Counter::FrameBytes), std::uint64_t{141} * n);
    EXPECT_EQ(s.get(Counter::PreparedBuilds), 0u); // substrate reused, not rebuilt
  } else {
    EXPECT_FALSE(atx::vol::counters::snapshot().enabled);
    SUCCEED();
  }
}

// §4 acceptance gate: the grouped-substrate P&L solve is bit-identical, per
// contract, to the ungrouped per-contract resolves (sb->evaluate + st->fair_value
// + st->iv) the pre-change pnl_explain used. Pins the pre-change frame by
// recomputing it independently on a multi-uid / multi-expiry / mixed-side book.
TEST(PortfolioPricer, PnlExplain_Grouped_BitIdenticalToUngrouped) {
  const PnlSurfaces surf;
  const SurfaceSet base = surf.base();
  const SurfaceSet shifted = surf.shifted();
  const std::vector<Position> book = pnl_multi_book();
  auto pf = Portfolio::create(book);
  ASSERT_TRUE(pf.has_value());
  const PortfolioPricer pricer(std::move(*pf));

  // Grouped substrate output (default opts: analytic_greeks=false, 1 thread).
  auto er = pricer.pnl_explain(base, shifted);
  ASSERT_TRUE(er.has_value());
  const PnlFrame &f = *er;
  ASSERT_EQ(f.size(), book.size());

  using EF = PricedSurface::EvalField;
  for (std::size_t i = 0; i < book.size(); ++i) {
    const OptionContract &c = book[i].contract;
    const double w = book[i].qty * book[i].multiplier;

    PriceStatus st_expect = PriceStatus::Ok;
    AmericanGreeks gb{};
    double price_base = 0.0, price_target = 0.0, dS = 0.0, dvol = 0.0, dt = 0.0, dr = 0.0;
    if (!(std::isfinite(c.K) && c.K > 0.0 && std::isfinite(c.T) && c.T > 0.0)) {
      st_expect = PriceStatus::InvalidContract;
    } else {
      const PricedSurface *sb = base.find(c.uid);
      const PricedSurface *sh = shifted.find(c.uid);
      if (sb == nullptr || sh == nullptr) {
        st_expect = PriceStatus::ModelUnavailable;
      } else {
        dt = static_cast<double>(sh->pricing().now_ts_ns - sb->pricing().now_ts_ns) / kNsPerYear;
        const double T_b = c.T;
        const double T_t = T_b - dt;
        if (!(std::isfinite(T_t) && T_t > 0.0)) {
          st_expect = PriceStatus::InvalidContract;
        } else {
          const PricedSurface::FusedResult fr = sb->evaluate(
              c.K, T_b, c.side, EF::Iv | EF::Price | EF::FirstOrder | EF::SecondOrder, false);
          auto pt = sh->fair_value(c.K, T_t, c.side);
          if (!fr.status.has_value() || !std::isfinite(fr.greeks.price) || !pt.has_value() ||
              !std::isfinite(*pt)) {
            st_expect = PriceStatus::NumericError;
          } else {
            const double sig_b = fr.iv;
            const double sig_t = sh->iv(c.K, T_b);
            if (!(std::isfinite(sig_b) && std::isfinite(sig_t))) {
              st_expect = PriceStatus::NumericError;
            } else {
              gb = fr.greeks;
              price_base = fr.greeks.price;
              price_target = *pt;
              dS = sh->pricing().S - sb->pricing().S;
              dvol = sig_t - sig_b;
              dr = sh->pricing().r - sb->pricing().r;
            }
          }
        }
      }
    }

    EXPECT_EQ(f.status[i], st_expect) << i;
    if (st_expect != PriceStatus::Ok) {
      continue;
    }
    // Recompute the w-scaled decomposition exactly as the scatter does.
    const double pnl_total_ps = price_target - price_base;
    const double pd = gb.delta * dS;
    const double pg = 0.5 * gb.gamma * dS * dS;
    const double pv = gb.vega * dvol;
    const double pvol = 0.5 * gb.volga * dvol * dvol;
    const double pvanna = gb.vanna * dS * dvol;
    const double pth = gb.theta * dt;
    const double prho = gb.rho * dr;
    const double pcharm = gb.charm * dS * dt;
    const double explained = pd + pg + pv + pvol + pvanna + pth + prho + pcharm;
    const double unexpl = pnl_total_ps - explained;
    EXPECT_TRUE(bits_equal(f.pv_base[i], w * price_base)) << i;
    EXPECT_TRUE(bits_equal(f.pv_target[i], w * price_target)) << i;
    EXPECT_TRUE(bits_equal(f.pnl_total[i], w * pnl_total_ps)) << i;
    EXPECT_TRUE(bits_equal(f.pnl_delta[i], w * pd)) << i;
    EXPECT_TRUE(bits_equal(f.pnl_gamma[i], w * pg)) << i;
    EXPECT_TRUE(bits_equal(f.pnl_vega[i], w * pv)) << i;
    EXPECT_TRUE(bits_equal(f.pnl_volga[i], w * pvol)) << i;
    EXPECT_TRUE(bits_equal(f.pnl_vanna[i], w * pvanna)) << i;
    EXPECT_TRUE(bits_equal(f.pnl_theta[i], w * pth)) << i;
    EXPECT_TRUE(bits_equal(f.pnl_rho[i], w * prho)) << i;
    EXPECT_TRUE(bits_equal(f.pnl_charm[i], w * pcharm)) << i;
    EXPECT_TRUE(bits_equal(f.pnl_unexplained[i], w * unexpl)) << i;
    EXPECT_TRUE(bits_equal(f.d_spot[i], dS)) << i;
    EXPECT_TRUE(bits_equal(f.d_vol[i], dvol)) << i;
    EXPECT_TRUE(bits_equal(f.d_time[i], dt)) << i;
    EXPECT_TRUE(bits_equal(f.d_rate[i], dr)) << i;
  }
}
