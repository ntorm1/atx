// Real-OPRA multi-underlying portfolio pricing + Taylor PnL-explain end-to-end.
//
// Builds a book spanning TWO real underlyings from the committed OPRA boards —
// SPY (index, ConvexDense fit) and XOM (single-name) — installed into ONE
// universe so they carry distinct uids. Chain:
//
//   OPRA boards -> VolaSession per name -> to_priced_surface() (uid 1 / uid 2)
//               -> Portfolio (positions across both) -> PortfolioPricer
//
// Two guarantees:
//   1. PRICE: every priced row is BIT-IDENTICAL to the underlying
//      PricedSurface::greeks (qty*mult scaled) — the multi-underlying pricer
//      reproduces the served theo the archive already proved equals the live
//      session (99.49% pxCLN on SPY).
//   2. PnL-EXPLAIN: against a controlled shifted surface (same fitted curves,
//      spot + rate bumped via CurveSurface::clone) the Taylor delta/gamma/rho
//      terms reconstruct the full reprice to a tight aggregate residual, with the
//      vol/time axes exactly inert (dvol = dt = 0).
//
// GTEST_SKIPs cleanly when either parquet fixture is absent.

#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <span>
#include <utility>
#include <vector>

#include "atx/vol/api/marketdata/data.hpp"             // data_install
#include "atx/vol/api/backtest/portfolio_pricer.hpp"
#include "atx/vol/api/backtest/priced_surface.hpp"
#include "atx/vol/api/fitting/session.hpp"
#include "atx/vol/api/storage/surface_archive.hpp"
#include "atx/vol/api/marketdata/universe.hpp"
#include "atx/vol/api/fitting/vol_curve.hpp"
#include "support/cached_artifacts.hpp"
#include "support/opra_fixture.hpp"

using namespace atx::vol;
using atx::vol::test::cached_spy_convex_dense;
using atx::vol::testkit::load_opra_board;

namespace {

[[nodiscard]] bool bits_equal(double a, double b) noexcept {
  std::uint64_t ba = 0;
  std::uint64_t bb = 0;
  std::memcpy(&ba, &a, sizeof ba);
  std::memcpy(&bb, &b, sizeof bb);
  return ba == bb;
}

// A shifted surface with IDENTICAL fitted curves but bumped spot / rate — a
// controlled, Taylor-friendly market move on the real surface (dvol = dt = 0).
[[nodiscard]] PricedSurface bump_scalars(const PricedSurface& base, double dS, double dr) {
  CurveSurface c = base.surface().clone();
  std::vector<SliceContext> ctx(base.context().begin(), base.context().end());
  PricingContext pc = base.pricing();
  pc.S += dS;
  pc.r += dr;
  auto ps = PricedSurface::create(std::move(c), std::move(ctx), pc);
  EXPECT_TRUE(ps.has_value());
  return std::move(*ps);
}

// Build a liquid listed-contract book over `sess`'s expiries: a few strikes
// straddling the forward per expiry, both sides. Contracts carry `uid`.
// Templated so it runs identically off a live `VolaSession` (XOM, fit live
// below) or a reloaded `PricedSurface` (SPY, off the cached archive) — the two
// types expose the same `iv()`; only the expiry-span accessor's name differs
// (`expiries()` vs `context()`), branched via `if constexpr`.
template <typename Surface>
void append_book(const Surface& sess, std::uint32_t uid, double qty_scale,
                 std::vector<Position>& book, std::uint64_t& next_id) {
  const auto expiries = [&sess]() -> std::span<const SliceContext> {
    if constexpr (requires { sess.expiries(); }) {
      return sess.expiries();
    } else {
      return sess.context();
    }
  }();
  for (const auto& c : expiries) {
    const double T = c.T;
    if (T < 0.03 || T > 1.5) {
      continue;
    }
    const double F = c.forward;
    for (const double m : {0.92, 0.98, 1.0, 1.02, 1.08}) {
      const double K = F * m;
      const double iv = sess.iv(K, T);
      if (!std::isfinite(iv)) {
        continue;
      }
      const Side side = (m <= 1.0) ? Side::Put : Side::Call;
      book.push_back(Position{next_id++, OptionContract{uid, K, T, side},
                              qty_scale * (side == Side::Call ? 1.0 : -1.0), 100.0});
    }
  }
}

}  // namespace

TEST(SpyPortfolioPnl, MultiUnderlying_Price_And_ControlledExplain) {
  auto spy_board = load_opra_board("spy", "SPY");
  auto xom_board = load_opra_board("xom", "XOM");
  const auto spy_archive_path = cached_spy_convex_dense();
  if (!spy_board.has_value() || !xom_board.has_value() || spy_archive_path.empty()) {
    GTEST_SKIP() << "SPY/XOM OPRA parquet fixtures not found";
  }

  // Install both boards into ONE universe -> distinct uids (SPY=1, XOM=2). SPY
  // is still installed (though no longer fit live below) purely to reserve its
  // uid slot ahead of XOM: universe.hpp guarantees a fresh single-symbol
  // Universe always assigns uid 1 to its sole ticker, so cached_spy_convex_dense's
  // OWN (single-symbol) install stamped the archived surface with uid=1 too --
  // installing SPY first here keeps the two uids in lock-step (checked below).
  Universe uni;
  auto spy_uid = data_install(uni, spy_board->panel.frame);
  auto xom_uid = data_install(uni, xom_board->panel.frame);
  ASSERT_TRUE(spy_uid.has_value() && xom_uid.has_value());
  ASSERT_NE(*spy_uid, *xom_uid);
  const Underlying& xom_u = *uni.get_underlying(*xom_uid).value();

  // SPY: reload the shared cached ConvexDense archive (the canary
  // PnlGreeksConsistency.Session_ConvexDense_GreeksPrice_BitEqual_FairValue and
  // spy_archive_roundtrip_test both prove the reload prices bit-identically to
  // a live session). XOM: single-name default, still fit live (21 KB board,
  // cheap -- no cache needed).
  auto arch = SurfaceArchiveV2::open_file(spy_archive_path.string());
  ASSERT_TRUE(arch.has_value()) << arch.error().to_string();
  auto spy_ps = arch->reconstruct_symbol("SPY");
  ASSERT_TRUE(spy_ps.has_value()) << spy_ps.error().to_string();
  ASSERT_EQ(spy_ps->uid(), *spy_uid) << "archived SPY uid must match this test's reserved slot";

  SessionInputs xom_in = make_session_inputs(FitPreset::Fast, xom_board->spot(),
                                             xom_board->r, xom_board->now_ns());
  xom_in.cash_divs = xom_board->panel.frame.divs;

  auto xom_sess = VolaSession::build(xom_u, xom_in);
  ASSERT_TRUE(xom_sess.has_value()) << xom_sess.error().to_string();

  auto xom_ps = xom_sess->to_priced_surface();
  ASSERT_TRUE(xom_ps.has_value()) << xom_ps.error().to_string();
  ASSERT_NE(spy_ps->uid(), xom_ps->uid());

  const std::vector<const PricedSurface*> base_ptrs{&*spy_ps, &*xom_ps};
  auto base_set = SurfaceSet::create(base_ptrs);
  ASSERT_TRUE(base_set.has_value());

  // Multi-underlying book across both names.
  std::vector<Position> book;
  std::uint64_t id = 0;
  append_book(*spy_ps, spy_ps->uid(), 10.0, book, id);
  append_book(*xom_sess, xom_ps->uid(), 25.0, book, id);
  ASSERT_GT(book.size(), 40u);

  auto pf = Portfolio::create(book);
  ASSERT_TRUE(pf.has_value());
  EXPECT_EQ(pf->n_underlyings(), 2u);
  const PortfolioPricer pricer(std::move(*pf));

  // ── 1. Price: rows bit-identical to the PricedSurface Greeks. ──────────────
  auto fr = pricer.price(*base_set, PriceOptions{0});  // hardware concurrency
  ASSERT_TRUE(fr.has_value());
  const PriceFrame& f = *fr;
  const PricedSurface* by_uid[3] = {nullptr, &*spy_ps, &*xom_ps};

  std::size_t n_ok = 0;
  for (std::size_t i = 0; i < book.size(); ++i) {
    if (f.status[i] != PriceStatus::Ok) {
      continue;
    }
    ++n_ok;
    const Position& p = book[i];
    const PricedSurface& s = *by_uid[p.contract.uid];
    const auto fv = s.fair_value(p.contract.K, p.contract.T, p.contract.side);
    const auto g = s.greeks(p.contract.K, p.contract.T, p.contract.side);
    ASSERT_TRUE(fv.has_value() && g.has_value());
    const double w = p.qty * p.multiplier;
    EXPECT_TRUE(bits_equal(f.pv[i], w * *fv)) << i;   // American mark
    EXPECT_TRUE(bits_equal(f.price[i], *fv)) << i;
    EXPECT_TRUE(bits_equal(g->price, *fv)) << i;             // greeks price == fair_value
    EXPECT_TRUE(bits_equal(f.delta[i], w * g->delta)) << i;  // American (cold-FD) Greeks
    EXPECT_TRUE(bits_equal(f.gamma[i], w * g->gamma)) << i;
    EXPECT_TRUE(bits_equal(f.vega[i], w * g->vega)) << i;
    EXPECT_TRUE(bits_equal(f.theta[i], w * g->theta)) << i;
    EXPECT_TRUE(bits_equal(f.vanna[i], w * g->vanna)) << i;
  }
  ASSERT_GT(n_ok, 40u);

  // ── 2. Controlled PnL-explain: spot +0.2% on both names (dvol=dt=dr=0). ─────
  // A pure spot move isolates delta/gamma. The Greeks are now AMERICAN (cold-FD),
  // so the early-exercise premium's spot sensitivity is captured inside
  // delta/gamma and the residual is only the higher-order Taylor tail.
  const PricedSurface spy_shift = bump_scalars(*spy_ps, 0.002 * spy_ps->pricing().S, 0.0);
  const PricedSurface xom_shift = bump_scalars(*xom_ps, 0.002 * xom_ps->pricing().S, 0.0);
  const std::vector<const PricedSurface*> shift_ptrs{&spy_shift, &xom_shift};
  auto shift_set = SurfaceSet::create(shift_ptrs);
  ASSERT_TRUE(shift_set.has_value());

  auto er = pricer.pnl_explain(*base_set, *shift_set, PriceOptions{0});
  ASSERT_TRUE(er.has_value());
  const PnlFrame& e = *er;

  double sum_abs_total = 0.0;
  double sum_abs_unexpl = 0.0;
  for (std::size_t i = 0; i < e.size(); ++i) {
    if (e.status[i] != PriceStatus::Ok) {
      continue;
    }
    // Pure spot move: vol / time / rate axes are exactly inert.
    EXPECT_EQ(e.d_vol[i], 0.0) << i;
    EXPECT_EQ(e.d_time[i], 0.0) << i;
    EXPECT_EQ(e.d_rate[i], 0.0) << i;
    EXPECT_EQ(e.pnl_vega[i], 0.0) << i;
    EXPECT_EQ(e.pnl_volga[i], 0.0) << i;
    EXPECT_EQ(e.pnl_vanna[i], 0.0) << i;
    EXPECT_EQ(e.pnl_theta[i], 0.0) << i;
    EXPECT_EQ(e.pnl_rho[i], 0.0) << i;
    EXPECT_EQ(e.pnl_charm[i], 0.0) << i;
    // Components + residual == the full American reprice (bookkeeping, exact).
    const double recon = e.pnl_delta[i] + e.pnl_gamma[i] + e.pnl_unexplained[i];
    EXPECT_NEAR(recon, e.pnl_total[i], 1e-6 * (std::fabs(e.pnl_total[i]) + 1.0)) << i;
    sum_abs_total += std::fabs(e.pnl_total[i]);
    sum_abs_unexpl += std::fabs(e.pnl_unexplained[i]);
  }
  // American delta + gamma reconstruct the American reprice: with the early-
  // exercise premium now inside the coefficients, the residual collapses from the
  // old European-greeks ~2% to a tiny higher-order tail on the mixed real book.
  ASSERT_GT(sum_abs_total, 0.0);
  const double resid_frac = sum_abs_unexpl / sum_abs_total;
  EXPECT_LT(resid_frac, 2e-4);  // measured ~4e-5; margin for the mixed SPY+XOM book

  std::printf(
      "[SPY+XOM portfolio] positions=%zu contracts=%zu underlyings=%zu ok=%zu | "
      "PV total=%.2f delta=%.2f vega=%.2f | PnL total=%.2f (delta=%.2f gamma=%.2f) "
      "residual=%.4f%%\n",
      pricer.portfolio().n_positions(), pricer.portfolio().n_contracts(),
      pricer.portfolio().n_underlyings(), n_ok, f.total.pv, f.total.delta, f.total.vega,
      e.total.pnl_total, e.total.pnl_delta, e.total.pnl_gamma, 100.0 * resid_frac);
}
