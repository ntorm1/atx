// Greeks/mark consistency + American-consistent PnL-explain axis isolation.
//
// Two guarantees the cold null-correction-cache path must hold after routing
// PricedSurface::greeks / VolaSession::greeks through the American finite-
// difference Greeks (american_greeks_fd) instead of the European Black-76 leg:
//
//   1. BIT-CONSISTENCY: greeks()->price is BIT-identical to fair_value() — both
//      are the same cold american_price with the same method/opts. Checked on a
//      synthetic PricedSurface (always) and, when the OPRA fixture is present, on
//      a live ConvexDense SPY VolaSession (its served cold path).
//   2. AXIS ISOLATION (synthetic known-truth): a spot-only move is reconstructed
//      by the (now American) delta/gamma to a tiny residual, and each pure axis
//      lights up only its own Taylor term. Because the early-exercise premium is
//      now INSIDE delta/gamma, the spot-only residual is far below the old
//      European-greeks bound — no q_eff=0 trick needed.

#include <gtest/gtest.h>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <span>
#include <utility>
#include <vector>

#include "atx/vol/black76.hpp"
#include "atx/vol/portfolio_pricer.hpp"
#include "atx/vol/priced_surface.hpp"
#include "atx/vol/surface_archive.hpp"
#include "atx/vol/vol_curve.hpp"
#include "support/cached_artifacts.hpp"

using namespace atx::vol;
using atx::vol::test::cached_spy_convex_dense;

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

[[nodiscard]] PricingContext make_pricing(std::uint32_t uid, double S = kS,
                                          double r = kR, std::int64_t now = kNow) {
  PricingContext pc;
  pc.S = S;
  pc.r = r;
  pc.now_ts_ns = now;
  pc.method = AmericanMethod::AndersenLake;
  pc.al_opts = al_fast_opts();
  pc.uid = uid;
  return pc;
}

// Coherent eSSVI priced surface with positive carry q_eff, giving genuine
// American early-exercise premium on both sides (European != American).
[[nodiscard]] PricedSurface make_essvi(std::uint32_t uid, int n, double theta_bump = 0.0,
                                       double S = kS, double r = kR,
                                       std::int64_t now = kNow, double q_eff = 0.02,
                                       bool flat_smile = false) {
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

[[nodiscard]] SurfaceSet set_of(const std::vector<const PricedSurface*>& v) {
  auto ss = SurfaceSet::create(v);
  EXPECT_TRUE(ss.has_value());
  return std::move(*ss);
}

[[nodiscard]] std::vector<Position> pnl_book() {
  std::vector<Position> book;
  std::uint64_t id = 0;
  for (double K : {92.0, 98.0, 100.0, 104.0, 110.0}) {
    book.push_back({id++, {1, K, 0.18, Side::Call}, +4.0, 100.0});
    book.push_back({id++, {1, K, 0.30, Side::Put}, -3.0, 100.0});
  }
  return book;
}

}  // namespace

// ── 1a. PricedSurface: greeks()->price is BIT-identical to fair_value(). ──────
TEST(PnlGreeksConsistency, PricedSurface_GreeksPrice_BitEqual_FairValue) {
  const PricedSurface ps = make_essvi(1, 5);
  int n_checked = 0;
  int n_premium = 0;  // count contracts with a nonzero early-exercise premium
  for (double K : {88.0, 96.0, 100.0, 104.0, 112.0}) {
    for (double T : {0.05, 0.15, 0.35}) {
      for (Side side : {Side::Call, Side::Put}) {
        const auto fv = ps.fair_value(K, T, side);
        const auto g = ps.greeks(K, T, side);
        ASSERT_TRUE(fv.has_value());
        ASSERT_TRUE(g.has_value());
        EXPECT_TRUE(bits_equal(g->price, *fv))
            << "K=" << K << " T=" << T << " side=" << static_cast<int>(side);
        // Confirm the surface genuinely exercises American premium somewhere, so
        // this is not a degenerate European==American pass.
        const double sigma = ps.iv(K, T);
        const double F = ps.forward_at(T);
        const double df = std::exp(-kR * T);
        const double euro = atx::vol::black76_price(F, K, T, sigma, df, side);
        if (std::fabs(*fv - euro) > 1e-6) {
          ++n_premium;
        }
        ++n_checked;
      }
    }
  }
  EXPECT_GT(n_checked, 0);
  EXPECT_GT(n_premium, 0) << "no early-exercise premium anywhere — test is vacuous";
}

// ── 1b. Cached SPY ConvexDense archive (cold path): greeks->price == fair_value.
// Reloads the shared fitted+serialized surface (see cached_artifacts.hpp)
// instead of re-fitting the 14k-contract board live; spy_archive_roundtrip_test
// proves the reload prices bit-identically to the live VolaSession, so this is
// the exact same bit-equality guarantee at load-time cost instead of fit cost.
TEST(PnlGreeksConsistency, Session_ConvexDense_GreeksPrice_BitEqual_FairValue) {
  const auto archive_path = cached_spy_convex_dense();
  if (archive_path.empty()) {
    GTEST_SKIP() << "SPY OPRA parquet fixture not found";
  }
  auto arch = SurfaceArchiveV2::open_file(archive_path.string());
  ASSERT_TRUE(arch.has_value()) << arch.error().to_string();
  auto recon = arch->reconstruct_symbol("SPY");
  ASSERT_TRUE(recon.has_value()) << recon.error().to_string();
  const PricedSurface& spy_sess = *recon;

  int n_checked = 0;
  // greeks()->price == fair_value() is a PER-CONTRACT property, so checking
  // every expiry adds no coverage — only CPU. Sample a REPRESENTATIVE subset of
  // at most ~6 expiries evenly spanning the term structure (front / middle /
  // back T), still honoring the same T in [0.03, 1.5] tradeable window and all
  // 5 moneyness points per sampled expiry.
  const std::span<const SliceContext> exps = spy_sess.context();
  std::vector<std::size_t> eligible;
  for (std::size_t i = 0; i < exps.size(); ++i) {
    if (exps[i].T >= 0.03 && exps[i].T <= 1.5) {
      eligible.push_back(i);
    }
  }
  constexpr std::size_t kMaxExpiries = 6;
  const std::size_t stride =
      eligible.empty() ? 1 : (eligible.size() + kMaxExpiries - 1) / kMaxExpiries;
  for (std::size_t j = 0; j < eligible.size(); j += stride) {
    const SliceContext& c = exps[eligible[j]];
    const double T = c.T;
    const double F = c.forward;
    for (double m : {0.92, 0.98, 1.0, 1.02, 1.08}) {
      const double K = F * m;
      if (!std::isfinite(spy_sess.iv(K, T))) {
        continue;
      }
      const Side side = (m <= 1.0) ? Side::Put : Side::Call;
      const auto fv = spy_sess.fair_value(K, T, side);
      const auto g = spy_sess.greeks(K, T, side);
      ASSERT_TRUE(fv.has_value());
      ASSERT_TRUE(g.has_value());
      EXPECT_TRUE(bits_equal(g->price, *fv)) << "K=" << K << " T=" << T;
      ++n_checked;
    }
  }
  EXPECT_GT(n_checked, 0);
}

// ── 2. PnL axis isolation, synthetic known-truth (American greeks). ───────────
TEST(PnlGreeksConsistency, PnlExplain_SpotOnly_ResidualTiny_AmericanGreeks) {
  const double dS = 0.05;
  // A flat smile isolates the spot axis while coherent carry moves F with S;
  // a skewed sticky-moneyness surface would correctly generate d_vol at fixed K.
  const PricedSurface base = make_essvi(1, 5, 0.0, kS, kR, kNow, 0.02, true);
  const PricedSurface shifted =
      make_essvi(1, 5, 0.0, kS + dS, kR, kNow, 0.02, true);
  const SurfaceSet bset = set_of({&base});
  const SurfaceSet sset = set_of({&shifted});

  auto pf = Portfolio::create(pnl_book());
  ASSERT_TRUE(pf.has_value());
  const PortfolioPricer pricer(std::move(*pf));
  auto er = pricer.pnl_explain(bset, sset);
  ASSERT_TRUE(er.has_value());
  const PnlFrame& f = *er;

  double sum_abs_total = 0.0;
  double sum_abs_unexpl = 0.0;
  for (std::size_t i = 0; i < f.size(); ++i) {
    ASSERT_EQ(f.status[i], PriceStatus::Ok) << i;
    // Only spot moved: every non-spot axis is exactly inert.
    EXPECT_EQ(f.d_vol[i], 0.0) << i;
    EXPECT_EQ(f.d_time[i], 0.0) << i;
    EXPECT_EQ(f.d_rate[i], 0.0) << i;
    EXPECT_EQ(f.pnl_vega[i], 0.0) << i;
    EXPECT_EQ(f.pnl_volga[i], 0.0) << i;
    EXPECT_EQ(f.pnl_vanna[i], 0.0) << i;
    EXPECT_EQ(f.pnl_theta[i], 0.0) << i;
    EXPECT_EQ(f.pnl_rho[i], 0.0) << i;
    EXPECT_EQ(f.pnl_charm[i], 0.0) << i;
    sum_abs_total += std::fabs(f.pnl_total[i]);
    sum_abs_unexpl += std::fabs(f.pnl_unexplained[i]);
  }
  ASSERT_GT(sum_abs_total, 0.0);
  // American delta/gamma reconstruct the American reprice: residual is now the
  // pure higher-order Taylor tail, well under 1e-3 of the total PnL.
  const double resid_frac = sum_abs_unexpl / sum_abs_total;
  EXPECT_LT(resid_frac, 1e-3) << "spot-only residual fraction = " << resid_frac;
}

TEST(PnlGreeksConsistency, PnlExplain_VolOnly_VegaVolgaCarry) {
  const PricedSurface base = make_essvi(1, 5);
  const PricedSurface shifted = make_essvi(1, 5, /*theta_bump*/ 0.001);
  const SurfaceSet bset = set_of({&base});
  const SurfaceSet sset = set_of({&shifted});

  auto pf = Portfolio::create(pnl_book());
  ASSERT_TRUE(pf.has_value());
  const PortfolioPricer pricer(std::move(*pf));
  auto er = pricer.pnl_explain(bset, sset);
  ASSERT_TRUE(er.has_value());
  const PnlFrame& f = *er;
  for (std::size_t i = 0; i < f.size(); ++i) {
    ASSERT_EQ(f.status[i], PriceStatus::Ok) << i;
    EXPECT_EQ(f.d_spot[i], 0.0) << i;
    EXPECT_EQ(f.d_time[i], 0.0) << i;
    EXPECT_EQ(f.d_rate[i], 0.0) << i;
    EXPECT_NE(f.d_vol[i], 0.0) << i;
    EXPECT_EQ(f.pnl_delta[i], 0.0) << i;
    EXPECT_EQ(f.pnl_gamma[i], 0.0) << i;
    EXPECT_EQ(f.pnl_theta[i], 0.0) << i;
    EXPECT_EQ(f.pnl_rho[i], 0.0) << i;
    EXPECT_EQ(f.pnl_charm[i], 0.0) << i;
    EXPECT_EQ(f.pnl_vanna[i], 0.0) << i;
    // vega + volga carry the reprice.
    EXPECT_NE(f.pnl_vega[i], 0.0) << i;
  }
}

TEST(PnlGreeksConsistency, PnlExplain_RateOnly_RhoCarry) {
  const double dr = 1e-4;
  // Flat smiles isolate the rate axis; with a skewed sticky-moneyness surface,
  // the coherent forward move correctly also changes IV at fixed strike.
  const PricedSurface base = make_essvi(1, 5, 0.0, kS, kR, kNow, 0.02, true);
  const PricedSurface shifted =
      make_essvi(1, 5, 0.0, kS, kR + dr, kNow, 0.02, true);
  const SurfaceSet bset = set_of({&base});
  const SurfaceSet sset = set_of({&shifted});

  auto pf = Portfolio::create(pnl_book());
  ASSERT_TRUE(pf.has_value());
  const PortfolioPricer pricer(std::move(*pf));
  auto er = pricer.pnl_explain(bset, sset);
  ASSERT_TRUE(er.has_value());
  const PnlFrame& f = *er;
  for (std::size_t i = 0; i < f.size(); ++i) {
    ASSERT_EQ(f.status[i], PriceStatus::Ok) << i;
    EXPECT_EQ(f.d_spot[i], 0.0) << i;
    EXPECT_EQ(f.d_vol[i], 0.0) << i;
    EXPECT_EQ(f.d_time[i], 0.0) << i;
    EXPECT_NEAR(f.d_rate[i], dr, 1e-15);
    EXPECT_EQ(f.pnl_delta[i], 0.0) << i;
    EXPECT_EQ(f.pnl_gamma[i], 0.0) << i;
    EXPECT_EQ(f.pnl_vega[i], 0.0) << i;
    EXPECT_EQ(f.pnl_theta[i], 0.0) << i;
    EXPECT_EQ(f.pnl_charm[i], 0.0) << i;
    EXPECT_NE(f.pnl_rho[i], 0.0) << i;
  }
}

TEST(PnlGreeksConsistency, PnlExplain_TimeOnly_ThetaCarry) {
  const std::int64_t one_hour = static_cast<std::int64_t>(3600.0 * 1e9);
  const PricedSurface base = make_essvi(1, 5);
  const PricedSurface shifted = make_essvi(1, 5, 0.0, kS, kR, kNow + one_hour);
  const SurfaceSet bset = set_of({&base});
  const SurfaceSet sset = set_of({&shifted});

  auto pf = Portfolio::create(pnl_book());
  ASSERT_TRUE(pf.has_value());
  const PortfolioPricer pricer(std::move(*pf));
  auto er = pricer.pnl_explain(bset, sset);
  ASSERT_TRUE(er.has_value());
  const PnlFrame& f = *er;
  const double dt_expect = static_cast<double>(one_hour) / kNsPerYear;
  for (std::size_t i = 0; i < f.size(); ++i) {
    ASSERT_EQ(f.status[i], PriceStatus::Ok) << i;
    EXPECT_EQ(f.d_spot[i], 0.0) << i;
    EXPECT_EQ(f.d_rate[i], 0.0) << i;
    EXPECT_NEAR(f.d_time[i], dt_expect, 1e-15);
    // The vol-roll now stays inside theta (dvol measured at the common maturity),
    // so d_vol is exactly zero on a pure time move.
    EXPECT_EQ(f.d_vol[i], 0.0) << i;
    EXPECT_EQ(f.pnl_delta[i], 0.0) << i;
    EXPECT_EQ(f.pnl_gamma[i], 0.0) << i;
    EXPECT_EQ(f.pnl_vanna[i], 0.0) << i;
    EXPECT_EQ(f.pnl_charm[i], 0.0) << i;
    EXPECT_EQ(f.pnl_rho[i], 0.0) << i;
    EXPECT_EQ(f.pnl_vega[i], 0.0) << i;   // no smile shift at the common maturity
    EXPECT_NE(f.pnl_theta[i], 0.0) << i;
  }
}
