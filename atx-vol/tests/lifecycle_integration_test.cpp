// Canonical library lifecycle — one end-to-end integration test, no external data.
//
// Locks the ONE blessed atx-vol path and proves every hand-off in a single TU:
//
//   OptionChain::from_frame            (deterministic SPY known-truth fixture)
//     -> PricerFitter::fit             (ConvexDense — the index recipe)
//        -> FittedSurface::session
//           -> VolaSession::to_priced_surface()          (PricedSurface snapshot)
//              -> write_surface_archive_v2 / SurfaceArchiveV2::open /
//                 reconstruct_symbol (serialize -> reload, BIT-identical theo)
//                 -> SurfaceSet + Portfolio + PortfolioPricer::price
//                    -> PortfolioPricer::pnl_explain      (Taylor spot-move explain)
//
// It is FIXTURE-FREE: the source board is the deterministic `make_spy_synthetic_
// panel` known-truth surface (spy_fixture.hpp), so this test runs everywhere with
// NO parquet and is NOT GTEST_SKIP-gated — it is the mandatory acceptance gate for
// the lifecycle. (A real-OPRA variant that GTEST_SKIPs when the fixture is absent
// lives in spy_archive_roundtrip_test / spy_portfolio_pnl_test.)
//
// The load-bearing guarantees, in order:
//   3. round-trip integrity — the reloaded PricedSurface reproduces the LIVE fit
//      bit-for-bit (fair_value AND every Greek), so the archive loses nothing;
//   4. portfolio fidelity   — each priced row is the reloaded surface's own
//      fair_value / Greeks scaled by qty*multiplier, bit-for-bit, and the frame
//      totals equal the in-order column sums over the Ok lanes;
//   5. PnL bookkeeping      — a pure spot bump isolates delta/gamma (vol/time/rate
//      terms exactly inert) and delta+gamma+unexplained reconstruct the full
//      American reprice (American cold-FD Greeks carry the early-exercise premium).

#include <gtest/gtest.h>

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <utility>
#include <vector>

#include "atx/vol/american.hpp"          // AmericanGreeks
#include "atx/vol/chain.hpp"             // OptionChain
#include "atx/vol/data.hpp"              // iso_to_ns
#include "atx/vol/market_env.hpp"        // MarketEnv
#include "atx/vol/panel.hpp"             // make_synthetic_american_panel, SynthPanelSpec
#include "atx/vol/portfolio_pricer.hpp"  // Portfolio, SurfaceSet, PortfolioPricer, ...
#include "atx/vol/pricer_fitter.hpp"     // PricerFitter, PricerConfig, FittedSurface
#include "atx/vol/priced_surface.hpp"    // PricedSurface, PricingContext, SliceContext
#include "atx/vol/session.hpp"           // VolaSession, FitPreset
#include "support/spy_fixture.hpp"       // make_spy_synthetic_spec
#include "atx/vol/surface_archive.hpp"   // write_surface_archive_v2, SurfaceArchiveV2
#include "atx/vol/types.hpp"             // Side
#include "atx/vol/vol_curve.hpp"         // CurveConfig, VolCurveKind, CurveSurface
#include "support/isa_golden_tol.hpp"    // golden_accum_close (per-ISA FMA band)

using namespace atx::vol;

namespace {

// Bit-for-bit double equality via the raw uint64 pattern — the round-trip and
// dedup guarantees are BIT-identical, not merely close.
[[nodiscard]] bool bits_equal(double a, double b) noexcept {
  std::uint64_t ba = 0;
  std::uint64_t bb = 0;
  std::memcpy(&ba, &a, sizeof ba);
  std::memcpy(&bb, &b, sizeof bb);
  return ba == bb;
}

// A shifted surface with IDENTICAL fitted curves but a bumped spot — the pure,
// Taylor-friendly market move for pnl_explain (dvol = dt = drate = 0).
[[nodiscard]] PricedSurface bump_spot(const PricedSurface& base, double dS) {
  CurveSurface c = base.surface().clone();
  std::vector<SliceContext> ctx(base.context().begin(), base.context().end());
  PricingContext pc = base.pricing();
  pc.S += dS;
  auto ps = PricedSurface::create(std::move(c), std::move(ctx), pc);
  EXPECT_TRUE(ps.has_value());
  return std::move(*ps);
}

}  // namespace

TEST(LifecycleIntegration, ChainToFitToArchiveToPortfolioToPnl) {
  // ── Stage 1: chain -> fit (ConvexDense index recipe). ─────────────────────
  // The deterministic known-truth SPY board; carry the two cash divs into the
  // chain's MarketEnv (the flat overload would otherwise drop them).
  const SynthPanelSpec spec = make_spy_synthetic_spec();
  auto panel = make_synthetic_american_panel(spec);
  ASSERT_TRUE(panel.has_value()) << panel.error().to_string();

  auto chain_res = OptionChain::from_frame(
      panel->frame,
      MarketEnv::flat(spec.spot, spec.r, iso_to_ns(spec.snapshot_iso), spec.cash_divs));
  ASSERT_TRUE(chain_res.has_value()) << chain_res.error().to_string();
  const OptionChain& chain = *chain_res;
  ASSERT_FALSE(chain.ids().empty());

  PricerConfig cfg;
  cfg.preset = FitPreset::Fast;
  cfg.curve = CurveConfig{};  // default is ConvexDense — pin it (index recipe)
  cfg.curve->kind = VolCurveKind::ConvexDense;
  cfg.curve->convex.node_cap = 40;
  cfg.n_threads = 1;
  PricerFitter fitter{cfg};
  ASSERT_TRUE(fitter.fit(chain).has_value());
  ASSERT_TRUE(fitter.fitted());
  const VolaSession& live = fitter.surface()->session();
  ASSERT_GT(live.expiries().size(), 0u);

  // ── Stage 2: to_priced_surface() -> archive -> reload. ────────────────────
  auto live_ps = live.to_priced_surface();
  ASSERT_TRUE(live_ps.has_value()) << live_ps.error().to_string();

  const std::array<SurfaceArchiveItem, 1> items{SurfaceArchiveItem{"SPY", &*live_ps}};
  auto built = write_surface_archive_v2(items);
  ASSERT_TRUE(built.has_value()) << built.error().to_string();

  auto opened = SurfaceArchiveV2::open(std::move(*built));
  ASSERT_TRUE(opened.has_value()) << opened.error().to_string();
  // reconstruct_symbol (owned), not map_symbol (borrowed view): the rest of this
  // test hands `reloaded` to a SurfaceSet of `const PricedSurface*`.
  auto recon_res = opened->reconstruct_symbol("SPY");
  ASSERT_TRUE(recon_res.has_value()) << recon_res.error().to_string();
  const PricedSurface& reloaded = *recon_res;

  ASSERT_EQ(reloaded.n_slices(), live.expiries().size());
  EXPECT_EQ(reloaded.kind_at(0), VolCurveKind::ConvexDense);

  // ── Assertion 3: reloaded surface reproduces the LIVE fit BIT-IDENTICALLY. ─
  // Walk a (K, T, side) grid straddling each slice forward and require the
  // reloaded PricedSurface's iv / fair_value / every Greek to be bit-for-bit the
  // live VolaSession's cold served theo (ConvexDense serves cold, so the snapshot
  // is exact). This is the archive round-trip integrity gate.
  std::size_t n_iv = 0;
  std::size_t n_fv = 0;
  std::size_t n_greeks = 0;
  for (const SliceContext& c : reloaded.context()) {
    const double T = c.T;
    const double F = c.forward;
    for (const double m : {0.90, 0.95, 0.98, 1.0, 1.02, 1.05, 1.10}) {
      const double K = F * m;
      const Side side = (m <= 1.0) ? Side::Put : Side::Call;

      const double iv_live = live.iv(K, T);
      const double iv_recon = reloaded.iv(K, T);
      EXPECT_TRUE(bits_equal(iv_live, iv_recon)) << "iv K=" << K << " T=" << T;
      ++n_iv;
      if (!std::isfinite(iv_live)) {
        continue;
      }

      const auto fv_live = live.fair_value(K, T, side);
      const auto fv_recon = reloaded.fair_value(K, T, side);
      ASSERT_EQ(fv_live.has_value(), fv_recon.has_value()) << "fv K=" << K << " T=" << T;
      if (fv_live.has_value()) {
        EXPECT_TRUE(bits_equal(*fv_live, *fv_recon)) << "fv K=" << K << " T=" << T;
        ++n_fv;
      }

      const auto g_live = live.greeks(K, T, side);
      const auto g_recon = reloaded.greeks(K, T, side);
      ASSERT_EQ(g_live.has_value(), g_recon.has_value()) << "greeks K=" << K << " T=" << T;
      if (g_live.has_value()) {
        const AmericanGreeks& a = *g_live;
        const AmericanGreeks& b = *g_recon;
        EXPECT_TRUE(bits_equal(a.price, b.price)) << "price K=" << K << " T=" << T;
        EXPECT_TRUE(bits_equal(a.delta, b.delta)) << "delta K=" << K << " T=" << T;
        EXPECT_TRUE(bits_equal(a.gamma, b.gamma)) << "gamma K=" << K << " T=" << T;
        EXPECT_TRUE(bits_equal(a.vega, b.vega)) << "vega K=" << K << " T=" << T;
        EXPECT_TRUE(bits_equal(a.theta, b.theta)) << "theta K=" << K << " T=" << T;
        EXPECT_TRUE(bits_equal(a.rho, b.rho)) << "rho K=" << K << " T=" << T;
        EXPECT_TRUE(bits_equal(a.vanna, b.vanna)) << "vanna K=" << K << " T=" << T;
        EXPECT_TRUE(bits_equal(a.volga, b.volga)) << "volga K=" << K << " T=" << T;
        EXPECT_TRUE(bits_equal(a.charm, b.charm)) << "charm K=" << K << " T=" << T;
        // greeks().price is itself the fair_value (the P0 cold-FD invariant).
        EXPECT_TRUE(bits_equal(b.price, *fv_recon)) << "greeks.price==fv K=" << K;
        ++n_greeks;
      }
    }
  }
  ASSERT_GT(n_iv, 20u);
  ASSERT_GT(n_fv, 20u) << "too few priced grid points to be a meaningful gate";
  ASSERT_GT(n_greeks, 20u);

  // ── Stage 3 + Assertion 4: Portfolio over the reloaded surface. ───────────
  // A listed book straddling each slice forward, both sides, carrying the
  // reloaded surface's own uid so SurfaceSet resolves it.
  const std::uint32_t uid = reloaded.uid();
  std::vector<Position> book;
  std::uint64_t id = 0;
  for (const SliceContext& c : reloaded.context()) {
    const double T = c.T;
    const double F = c.forward;
    for (const double m : {0.94, 0.98, 1.0, 1.02, 1.06}) {
      const double K = F * m;
      if (!std::isfinite(reloaded.iv(K, T))) {
        continue;
      }
      const Side side = (m <= 1.0) ? Side::Put : Side::Call;
      const double qty = (side == Side::Call) ? 3.0 : -2.0;
      book.push_back(Position{id++, OptionContract{uid, K, T, side}, qty, 100.0});
    }
  }
  ASSERT_GT(book.size(), 20u);

  auto pf = Portfolio::create(book);
  ASSERT_TRUE(pf.has_value());
  EXPECT_EQ(pf->n_underlyings(), 1u);
  const PortfolioPricer pricer(std::move(*pf));

  const std::array<const PricedSurface*, 1> set_ptrs{&reloaded};
  auto set = SurfaceSet::create(set_ptrs);
  ASSERT_TRUE(set.has_value());

  auto fr = pricer.price(*set, PriceOptions{0});  // hardware concurrency
  ASSERT_TRUE(fr.has_value());
  const PriceFrame& f = *fr;
  ASSERT_EQ(f.size(), book.size());

  // Each Ok row is the reloaded surface's own fair_value / Greeks scaled by
  // qty*multiplier, bit-for-bit; accumulate the in-order Ok-lane column sums for
  // the totals check.
  double sum_pv = 0.0;
  double sum_delta = 0.0;
  double sum_gamma = 0.0;
  double sum_vega = 0.0;
  std::size_t n_ok = 0;
  for (std::size_t i = 0; i < book.size(); ++i) {
    if (f.status[i] != PriceStatus::Ok) {
      continue;
    }
    ++n_ok;
    const Position& p = book[i];
    const auto fv = reloaded.fair_value(p.contract.K, p.contract.T, p.contract.side);
    const auto g = reloaded.greeks(p.contract.K, p.contract.T, p.contract.side);
    ASSERT_TRUE(fv.has_value() && g.has_value());
    const double w = p.qty * p.multiplier;
    EXPECT_TRUE(bits_equal(f.price[i], *fv)) << i;      // per-share American mark
    EXPECT_TRUE(bits_equal(g->price, *fv)) << i;        // greeks.price == fair_value
    EXPECT_TRUE(bits_equal(f.pv[i], w * *fv)) << i;     // qty*mult scaled PV
    EXPECT_TRUE(bits_equal(f.delta[i], w * g->delta)) << i;
    EXPECT_TRUE(bits_equal(f.gamma[i], w * g->gamma)) << i;
    EXPECT_TRUE(bits_equal(f.vega[i], w * g->vega)) << i;
    EXPECT_TRUE(bits_equal(f.theta[i], w * g->theta)) << i;
    EXPECT_TRUE(bits_equal(f.vanna[i], w * g->vanna)) << i;
    sum_pv += f.pv[i];
    sum_delta += f.delta[i];
    sum_gamma += f.gamma[i];
    sum_vega += f.vega[i];
  }
  ASSERT_GT(n_ok, 15u);
  EXPECT_EQ(f.total.n_ok, n_ok);
  // Frame totals == the in-order Ok-lane column sums. The pricer reduces serially
  // in input order, so on the SSE2 reference ISA this is a bit-identical match
  // (golden_accum_close is byte-exact there). infra / test-tolerance (WS-0): under
  // rel-avx2 (/arch:AVX2) the pricer's reduction FMA-contracts differently than
  // this serial re-sum, so the *totals* drift a few ULP while every per-lane mark
  // above stays byte-exact — a reduction-order contraction telltale, not an
  // economics change. golden_accum_close opens a relative economic band under FMA
  // only. See support/isa_golden_tol.hpp.
  EXPECT_TRUE(atx::vol::test::golden_accum_close(f.total.pv, sum_pv))
      << "pv got=" << f.total.pv << " sum=" << sum_pv << " d=" << (f.total.pv - sum_pv);
  EXPECT_TRUE(atx::vol::test::golden_accum_close(f.total.delta, sum_delta))
      << "delta got=" << f.total.delta << " sum=" << sum_delta << " d=" << (f.total.delta - sum_delta);
  EXPECT_TRUE(atx::vol::test::golden_accum_close(f.total.gamma, sum_gamma))
      << "gamma got=" << f.total.gamma << " sum=" << sum_gamma << " d=" << (f.total.gamma - sum_gamma);
  EXPECT_TRUE(atx::vol::test::golden_accum_close(f.total.vega, sum_vega))
      << "vega got=" << f.total.vega << " sum=" << sum_vega << " d=" << (f.total.vega - sum_vega);

  // ── Assertion 5: pnl_explain over a pure spot bump. ───────────────────────
  // Same fitted curves, spot +0.2% (dvol = dt = drate = 0): a pure spot move
  // isolates delta/gamma, and delta+gamma+unexplained reconstruct the full
  // American reprice (American cold-FD Greeks — the P0 invariant).
  const double dS = 0.002 * reloaded.pricing().S;
  const PricedSurface shifted = bump_spot(reloaded, dS);
  const std::array<const PricedSurface*, 1> base_ptrs{&reloaded};
  const std::array<const PricedSurface*, 1> shift_ptrs{&shifted};
  auto base_set = SurfaceSet::create(base_ptrs);
  auto shift_set = SurfaceSet::create(shift_ptrs);
  ASSERT_TRUE(base_set.has_value() && shift_set.has_value());

  auto er = pricer.pnl_explain(*base_set, *shift_set, PriceOptions{0});
  ASSERT_TRUE(er.has_value());
  const PnlFrame& e = *er;
  ASSERT_EQ(e.size(), book.size());

  double sum_abs_total = 0.0;
  double sum_abs_unexpl = 0.0;
  std::size_t n_pnl_ok = 0;
  for (std::size_t i = 0; i < e.size(); ++i) {
    if (e.status[i] != PriceStatus::Ok) {
      continue;
    }
    ++n_pnl_ok;
    // Pure spot move — every non-spot axis is exactly inert.
    EXPECT_EQ(e.d_vol[i], 0.0) << i;
    EXPECT_EQ(e.d_time[i], 0.0) << i;
    EXPECT_EQ(e.d_rate[i], 0.0) << i;
    EXPECT_EQ(e.pnl_vega[i], 0.0) << i;
    EXPECT_EQ(e.pnl_volga[i], 0.0) << i;
    EXPECT_EQ(e.pnl_vanna[i], 0.0) << i;
    EXPECT_EQ(e.pnl_theta[i], 0.0) << i;
    EXPECT_EQ(e.pnl_rho[i], 0.0) << i;
    EXPECT_EQ(e.pnl_charm[i], 0.0) << i;
    EXPECT_NE(e.d_spot[i], 0.0) << i;
    // Components + residual == the full American reprice (exact bookkeeping).
    const double recon = e.pnl_delta[i] + e.pnl_gamma[i] + e.pnl_unexplained[i];
    EXPECT_NEAR(recon, e.pnl_total[i], 1e-6 * (std::fabs(e.pnl_total[i]) + 1.0)) << i;
    sum_abs_total += std::fabs(e.pnl_total[i]);
    sum_abs_unexpl += std::fabs(e.pnl_unexplained[i]);
  }
  ASSERT_GT(n_pnl_ok, 15u);
  ASSERT_GT(sum_abs_total, 0.0);
  // American delta+gamma reconstruct the American reprice: the residual is only
  // the higher-order Taylor tail (early-exercise premium is inside the Greeks).
  const double resid_frac = sum_abs_unexpl / sum_abs_total;
  EXPECT_LT(resid_frac, 1e-3) << "spot-only residual fraction = " << resid_frac;

  std::printf(
      "[lifecycle] slices=%zu iv=%zu fv=%zu greeks=%zu | positions=%zu ok=%zu | "
      "PV total=%.2f delta=%.2f | PnL total=%.2f (delta=%.2f gamma=%.2f) resid=%.4f%%\n",
      reloaded.n_slices(), n_iv, n_fv, n_greeks, book.size(), n_ok, f.total.pv,
      f.total.delta, e.total.pnl_total, e.total.pnl_delta, e.total.pnl_gamma,
      100.0 * resid_frac);
}
