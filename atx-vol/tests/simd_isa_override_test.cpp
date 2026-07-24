// W4 (simd-review finding 2): EVERY batch dispatcher must honor the process-global
// SIMD ISA override / MathMode, not just the American boundary/greeks batches.
//
// Before W4 the black76 / greeks / eSSVI / P&L batch entry points gated directly on
// have_avx2(), so set_simd_isa_override(ForceScalar) (equivalently
// set_math_mode(Reference)) was silently ignored: Reference mode still returned the
// FastDeterministic AVX2 numbers. W4 rewires them to use_avx2(), which observes the
// override.
//
// Proof strategy (per entry point): with ForceScalar installed, the batch output must
// be BIT-IDENTICAL to the per-element scalar source of truth. On an AVX2 host that is
// only possible if the override actually diverted the call off the AVX2 fast path —
// the vector kernels differ from scalar libm (Cody-erfc Φ, FMA grouping) — so each
// assertion below is RED before the fix and GREEN after. A companion Auto-path check
// on the black76 family confirms the fast path is genuinely live (the bit-identity has
// teeth). The whole suite is skipped on a non-AVX2 host, where use_avx2() is always
// false and there is nothing to divert.

#include "atx/vol/batch.hpp"
#include "atx/vol/black76.hpp"
#include "atx/vol/greeks.hpp"
#include "atx/vol/surface.hpp"     // EssviSlice, essvi_w
#include "atx/vol/types.hpp"
#include "atx/vol/vol_surface.hpp" // EssviParams, SviParams, essvi_backbone_w, svi_total_w, essvi_w_grad3

#include "atx/vol/simd/black76_batch.hpp"
#include "atx/vol/simd/cpu.hpp"
#include "atx/vol/simd/essvi_batch.hpp"
#include "atx/vol/simd/greeks_batch.hpp"
#include "atx/vol/simd/math_mode.hpp"
#include "atx/vol/simd/pnl_batch.hpp"

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdlib> // std::getenv / getenv_s (ForceScalar env leg)
#include <span>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

namespace atx::vol {
namespace {

// Restores the process-global override to Auto no matter how the test exits, so a
// forced route never leaks into a sibling test (the override is process-wide).
struct IsaGuard {
  ~IsaGuard() { simd::set_simd_isa_override(simd::SimdIsa::Auto); }
};

// ── Shared input grids (all finite, in-domain: no degenerate/NaN lanes, so the
//    scalar reference is exact and EXPECT_EQ is a legitimate bit test). ────────

struct B76Grid {
  std::vector<double> F, K, T, sigma, r, df;
  std::vector<Side> side;
  [[nodiscard]] std::size_t size() const { return F.size(); }
};

// 84 interior lanes (n % 4 == 0 so full vector blocks, no scalar tail), spanning
// ITM/OTM, several tenors and vols, both sides.
B76Grid make_b76_grid() {
  B76Grid g;
  const double moneyness[] = {0.7, 0.85, 0.95, 1.0, 1.05, 1.2, 1.5};
  const double tenors[] = {0.05, 0.25, 1.0, 2.0};
  const double vols[] = {0.12, 0.30, 0.60};
  constexpr double rate = 0.03;
  for (double m : moneyness)
    for (double T : tenors)
      for (double v : vols) {
        constexpr double F = 100.0;
        g.F.push_back(F);
        g.K.push_back(F * m);
        g.T.push_back(T);
        g.sigma.push_back(v);
        g.r.push_back(rate);
        g.df.push_back(std::exp(-rate * T));
        g.side.push_back((g.F.size() & 1u) ? Side::Put : Side::Call);
      }
  return g;
}

// A log-moneyness grid for the eSSVI/SVI kernels (>= 16 so the public essvi_w_batch
// clears kEssviAvx2MinBatch and takes the vector path).
std::vector<double> make_k_grid() {
  std::vector<double> k;
  for (int i = -30; i <= 30; ++i) {
    k.push_back(0.05 * static_cast<double>(i));
  }
  return k;
}

// Count lanes where two equal-length buffers differ bit-for-bit.
[[nodiscard]] std::size_t bit_diffs(const std::vector<double>& a,
                                    const std::vector<double>& b) {
  std::size_t d = 0;
  for (std::size_t i = 0; i < a.size(); ++i) {
    if (a[i] != b[i]) {
      ++d;
    }
  }
  return d;
}

// ── Public span layer (atx::vol, batch.cpp) ──────────────────────────────────

TEST(SimdIsaOverride, PublicBlack76PriceBatchHonorsForceScalar) {
  if (!simd::have_avx2()) {
    GTEST_SKIP() << "override divergence is only observable on an AVX2 host";
  }
  IsaGuard guard;
  const B76Grid g = make_b76_grid();
  const std::size_t n = g.size();

  std::vector<double> ref(n);
  for (std::size_t i = 0; i < n; ++i) {
    ref[i] = black76_price(g.F[i], g.K[i], g.T[i], g.sigma[i], g.df[i], g.side[i]);
  }

  // Auto → AVX2 on this host: must differ from scalar on >= 1 lane (teeth).
  simd::set_simd_isa_override(simd::SimdIsa::Auto);
  std::vector<double> got_auto(n, 0.0);
  ASSERT_TRUE(
      black76_price_batch(g.F, g.K, g.T, g.sigma, g.df, g.side, std::span<double>(got_auto))
          .has_value());
  EXPECT_GT(bit_diffs(got_auto, ref), 0u) << "Auto did not take the AVX2 fast path";

  // ForceScalar → bit-identical to the scalar reference on every lane.
  simd::set_simd_isa_override(simd::SimdIsa::ForceScalar);
  std::vector<double> got_scalar(n, 0.0);
  ASSERT_TRUE(
      black76_price_batch(g.F, g.K, g.T, g.sigma, g.df, g.side, std::span<double>(got_scalar))
          .has_value());
  for (std::size_t i = 0; i < n; ++i) {
    EXPECT_EQ(got_scalar[i], ref[i]) << "lane " << i;
  }
}

TEST(SimdIsaOverride, PublicBlack76ValueVegaBatchHonorsForceScalar) {
  if (!simd::have_avx2()) {
    GTEST_SKIP() << "override divergence is only observable on an AVX2 host";
  }
  IsaGuard guard;
  const B76Grid g = make_b76_grid();
  const std::size_t n = g.size();
  constexpr double kT = 0.5; // shared-T entry
  const double sqrt_t = std::sqrt(kT);

  std::vector<double> ref_v(n), ref_vg(n);
  for (std::size_t i = 0; i < n; ++i) {
    const Black76ValueVega vv =
        black76_value_and_vega(g.F[i], g.K[i], kT, g.sigma[i], g.df[i], g.side[i], sqrt_t);
    ref_v[i] = vv.price;
    ref_vg[i] = vv.vega;
  }

  simd::set_simd_isa_override(simd::SimdIsa::Auto);
  std::vector<double> auto_v(n, 0.0), auto_vg(n, 0.0);
  ASSERT_TRUE(black76_value_and_vega_batch(g.F, g.K, kT, g.sigma, g.df, g.side,
                                           std::span<double>(auto_v),
                                           std::span<double>(auto_vg), sqrt_t)
                  .has_value());
  EXPECT_GT(bit_diffs(auto_v, ref_v) + bit_diffs(auto_vg, ref_vg), 0u)
      << "Auto did not take the AVX2 fast path";

  simd::set_simd_isa_override(simd::SimdIsa::ForceScalar);
  std::vector<double> sc_v(n, 0.0), sc_vg(n, 0.0);
  ASSERT_TRUE(black76_value_and_vega_batch(g.F, g.K, kT, g.sigma, g.df, g.side,
                                           std::span<double>(sc_v),
                                           std::span<double>(sc_vg), sqrt_t)
                  .has_value());
  for (std::size_t i = 0; i < n; ++i) {
    EXPECT_EQ(sc_v[i], ref_v[i]) << "value lane " << i;
    EXPECT_EQ(sc_vg[i], ref_vg[i]) << "vega lane " << i;
  }
}

TEST(SimdIsaOverride, PublicBlack76GreeksBatchHonorsForceScalar) {
  if (!simd::have_avx2()) {
    GTEST_SKIP() << "override divergence is only observable on an AVX2 host";
  }
  IsaGuard guard;
  const B76Grid g = make_b76_grid();
  const std::size_t n = g.size();

  std::vector<Greeks> ref(n);
  std::vector<double> ref_px(n);
  for (std::size_t i = 0; i < n; ++i) {
    const Black76Greeks w =
        black76_greeks(g.F[i], g.K[i], g.T[i], g.sigma[i], g.r[i], g.df[i], g.side[i]);
    ref[i] = w.greeks;
    ref_px[i] = w.price;
  }

  simd::set_simd_isa_override(simd::SimdIsa::Auto);
  std::vector<Greeks> auto_g(n);
  std::vector<double> auto_px(n, 0.0);
  ASSERT_TRUE(black76_greeks_batch(g.F, g.K, g.T, g.sigma, g.r, g.df, g.side,
                                   std::span<Greeks>(auto_g), std::span<double>(auto_px))
                  .has_value());
  std::size_t auto_diffs = 0;
  for (std::size_t i = 0; i < n; ++i) {
    if (auto_g[i].delta != ref[i].delta || auto_px[i] != ref_px[i]) {
      ++auto_diffs;
    }
  }
  EXPECT_GT(auto_diffs, 0u) << "Auto did not take the AVX2 fast path";

  simd::set_simd_isa_override(simd::SimdIsa::ForceScalar);
  std::vector<Greeks> sc_g(n);
  std::vector<double> sc_px(n, 0.0);
  ASSERT_TRUE(black76_greeks_batch(g.F, g.K, g.T, g.sigma, g.r, g.df, g.side,
                                   std::span<Greeks>(sc_g), std::span<double>(sc_px))
                  .has_value());
  for (std::size_t i = 0; i < n; ++i) {
    EXPECT_EQ(sc_px[i], ref_px[i]) << "price lane " << i;
    EXPECT_EQ(sc_g[i].delta, ref[i].delta) << "delta lane " << i;
    EXPECT_EQ(sc_g[i].gamma, ref[i].gamma) << "gamma lane " << i;
    EXPECT_EQ(sc_g[i].vega, ref[i].vega) << "vega lane " << i;
    EXPECT_EQ(sc_g[i].theta, ref[i].theta) << "theta lane " << i;
    EXPECT_EQ(sc_g[i].rho, ref[i].rho) << "rho lane " << i;
    EXPECT_EQ(sc_g[i].vanna, ref[i].vanna) << "vanna lane " << i;
    EXPECT_EQ(sc_g[i].volga, ref[i].volga) << "volga lane " << i;
    EXPECT_EQ(sc_g[i].charm, ref[i].charm) << "charm lane " << i;
  }
}

TEST(SimdIsaOverride, PublicEssviWBatchHonorsForceScalar) {
  if (!simd::have_avx2()) {
    GTEST_SKIP() << "override divergence is only observable on an AVX2 host";
  }
  IsaGuard guard;
  EssviSlice slice{};
  slice.theta = 0.04;
  slice.phi = 1.2;
  slice.rho = -0.3;
  slice.T = 0.25;
  const std::vector<double> k = make_k_grid();
  const std::size_t n = k.size();

  std::vector<double> ref(n);
  for (std::size_t i = 0; i < n; ++i) {
    ref[i] = essvi_w(slice, k[i]);
  }

  simd::set_simd_isa_override(simd::SimdIsa::ForceScalar);
  std::vector<double> sc(n, 0.0);
  ASSERT_TRUE(essvi_w_batch(slice, k, std::span<double>(sc)).has_value());
  for (std::size_t i = 0; i < n; ++i) {
    EXPECT_EQ(sc[i], ref[i]) << "lane " << i;
  }
}

// ── Raw-pointer simd layer (atx::vol::simd) ──────────────────────────────────

TEST(SimdIsaOverride, RawBlack76PriceBatchHonorsForceScalar) {
  if (!simd::have_avx2()) {
    GTEST_SKIP() << "override divergence is only observable on an AVX2 host";
  }
  IsaGuard guard;
  const B76Grid g = make_b76_grid();
  const std::size_t n = g.size();

  std::vector<double> ref(n);
  for (std::size_t i = 0; i < n; ++i) {
    ref[i] = black76_price(g.F[i], g.K[i], g.T[i], g.sigma[i], g.df[i], g.side[i]);
  }

  simd::set_simd_isa_override(simd::SimdIsa::Auto);
  std::vector<double> got_auto(n, 0.0);
  simd::black76_price_batch(g.F.data(), g.K.data(), g.T.data(), g.sigma.data(), g.df.data(),
                            g.side.data(), got_auto.data(), n);
  EXPECT_GT(bit_diffs(got_auto, ref), 0u) << "Auto did not take the AVX2 fast path";

  simd::set_simd_isa_override(simd::SimdIsa::ForceScalar);
  std::vector<double> got_scalar(n, 0.0);
  simd::black76_price_batch(g.F.data(), g.K.data(), g.T.data(), g.sigma.data(), g.df.data(),
                            g.side.data(), got_scalar.data(), n);
  for (std::size_t i = 0; i < n; ++i) {
    EXPECT_EQ(got_scalar[i], ref[i]) << "lane " << i;
  }
}

TEST(SimdIsaOverride, RawBlack76ValueVegaBatchHonorsForceScalar) {
  if (!simd::have_avx2()) {
    GTEST_SKIP() << "override divergence is only observable on an AVX2 host";
  }
  IsaGuard guard;
  const B76Grid g = make_b76_grid();
  const std::size_t n = g.size();

  std::vector<double> ref_v(n), ref_vg(n);
  for (std::size_t i = 0; i < n; ++i) {
    const Black76ValueVega vv =
        black76_value_and_vega(g.F[i], g.K[i], g.T[i], g.sigma[i], g.df[i], g.side[i]);
    ref_v[i] = vv.price;
    ref_vg[i] = vv.vega;
  }

  simd::set_simd_isa_override(simd::SimdIsa::ForceScalar);
  std::vector<double> sc_v(n, 0.0), sc_vg(n, 0.0);
  simd::black76_value_vega_batch(g.F.data(), g.K.data(), g.T.data(), g.sigma.data(), g.df.data(),
                                 g.side.data(), sc_v.data(), sc_vg.data(), n);
  for (std::size_t i = 0; i < n; ++i) {
    EXPECT_EQ(sc_v[i], ref_v[i]) << "value lane " << i;
    EXPECT_EQ(sc_vg[i], ref_vg[i]) << "vega lane " << i;
  }
}

TEST(SimdIsaOverride, RawBlack76GreeksBatchAoSAndSoAHonorForceScalar) {
  if (!simd::have_avx2()) {
    GTEST_SKIP() << "override divergence is only observable on an AVX2 host";
  }
  IsaGuard guard;
  const B76Grid g = make_b76_grid();
  const std::size_t n = g.size();

  std::vector<Greeks> ref(n);
  std::vector<double> ref_px(n);
  for (std::size_t i = 0; i < n; ++i) {
    const Black76Greeks w =
        black76_greeks(g.F[i], g.K[i], g.T[i], g.sigma[i], g.r[i], g.df[i], g.side[i]);
    ref[i] = w.greeks;
    ref_px[i] = w.price;
  }

  simd::set_simd_isa_override(simd::SimdIsa::ForceScalar);

  // AoS entry.
  std::vector<Greeks> aos(n);
  std::vector<double> aos_px(n, 0.0);
  simd::black76_greeks_batch(g.F.data(), g.K.data(), g.T.data(), g.sigma.data(), g.r.data(),
                             g.df.data(), g.side.data(), aos.data(), aos_px.data(), n);

  // SoA entry (shares the vector core; must equally divert to scalar).
  std::vector<double> dl(n), gm(n), vg(n), th(n), rh(n), vn(n), vl(n), cm(n), px(n);
  simd::GreeksBatchSoA soa{dl.data(), gm.data(), vg.data(), th.data(), rh.data(),
                           vn.data(), vl.data(), cm.data(), px.data()};
  simd::black76_greeks_batch_soa(g.F.data(), g.K.data(), g.T.data(), g.sigma.data(), g.r.data(),
                                 g.df.data(), g.side.data(), soa, n);

  for (std::size_t i = 0; i < n; ++i) {
    EXPECT_EQ(aos_px[i], ref_px[i]) << "aos price lane " << i;
    EXPECT_EQ(aos[i].delta, ref[i].delta) << "aos delta lane " << i;
    EXPECT_EQ(aos[i].vega, ref[i].vega) << "aos vega lane " << i;
    EXPECT_EQ(aos[i].charm, ref[i].charm) << "aos charm lane " << i;
    EXPECT_EQ(px[i], ref_px[i]) << "soa price lane " << i;
    EXPECT_EQ(dl[i], ref[i].delta) << "soa delta lane " << i;
    EXPECT_EQ(vg[i], ref[i].vega) << "soa vega lane " << i;
    EXPECT_EQ(cm[i], ref[i].charm) << "soa charm lane " << i;
  }
}

TEST(SimdIsaOverride, RawEssviAndSviBatchesHonorForceScalar) {
  if (!simd::have_avx2()) {
    GTEST_SKIP() << "override divergence is only observable on an AVX2 host";
  }
  IsaGuard guard;
  EssviParams es{};
  es.theta = 0.04;
  es.phi = 1.2;
  es.rho = -0.3;
  es.T = 0.25;
  es.F = 100.0;
  SviParams sv{};
  sv.a = 0.02;
  sv.b = 0.15;
  sv.rho = -0.4;
  sv.m = 0.0;
  sv.sigma = 0.1;
  sv.T = 0.25;
  sv.F = 100.0;

  const std::vector<double> k = make_k_grid();
  const std::size_t n = k.size();

  // Scalar references.
  std::vector<double> ref_w(n), ref_dth(n), ref_dphi(n), ref_drho(n), ref_svi(n), ref_sig(n);
  std::vector<double> ref_qu(n), ref_qv(n);
  constexpr double kInvSqrt2 = 0.70710678118654752440;
  constexpr double kQeM = 0.05;
  constexpr double kQeSigma = 0.10;
  for (std::size_t i = 0; i < n; ++i) {
    ref_w[i] = essvi_backbone_w(es, k[i]);
    const std::array<double, 3> gr = essvi_w_grad3(es, k[i]);
    ref_dth[i] = gr[0];
    ref_dphi[i] = gr[1];
    ref_drho[i] = gr[2];
    ref_svi[i] = svi_total_w(sv, k[i]);
    ref_sig[i] = std::sqrt(std::max(essvi_backbone_w(es, k[i]), 0.0) / es.T);
    const double y = (k[i] - kQeM) / kQeSigma;
    const double z = std::sqrt(y * y + 1.0);
    ref_qu[i] = (y + z) * kInvSqrt2;
    ref_qv[i] = (z - y) * kInvSqrt2;
  }

  simd::set_simd_isa_override(simd::SimdIsa::ForceScalar);

  std::vector<double> w(n, 0.0);
  simd::essvi_backbone_w_batch(es, k.data(), w.data(), n);

  std::vector<double> gw(n, 0.0), gdth(n, 0.0), gdphi(n, 0.0), gdrho(n, 0.0);
  simd::essvi_backbone_w_grad_batch(es, k.data(), gw.data(), gdth.data(), gdphi.data(),
                                    gdrho.data(), n);

  std::vector<double> svi(n, 0.0);
  simd::svi_total_w_batch(sv, k.data(), svi.data(), n);

  std::vector<double> sig(n, 0.0);
  simd::essvi_backbone_sigma_batch(es, k.data(), sig.data(), n);

  std::vector<double> qu(n, 0.0), qv(n, 0.0);
  simd::svi_qe_basis_batch(kQeM, kQeSigma, k.data(), qu.data(), qv.data(), n);

  for (std::size_t i = 0; i < n; ++i) {
    EXPECT_EQ(w[i], ref_w[i]) << "essvi_w lane " << i;
    EXPECT_EQ(gw[i], ref_w[i]) << "grad.w lane " << i;
    EXPECT_EQ(gdth[i], ref_dth[i]) << "grad.dtheta lane " << i;
    EXPECT_EQ(gdphi[i], ref_dphi[i]) << "grad.dphi lane " << i;
    EXPECT_EQ(gdrho[i], ref_drho[i]) << "grad.drho lane " << i;
    EXPECT_EQ(svi[i], ref_svi[i]) << "svi_total lane " << i;
    EXPECT_EQ(sig[i], ref_sig[i]) << "essvi_sigma lane " << i;
    // svi_qe_basis forbids FMA for scalar bit-parity, so AVX2 already equals scalar;
    // the override is still required to route the scalar loop, and equality holds.
    EXPECT_EQ(qu[i], ref_qu[i]) << "qe.u lane " << i;
    EXPECT_EQ(qv[i], ref_qv[i]) << "qe.v lane " << i;
  }
}

TEST(SimdIsaOverride, RawPnlTaylorBatchHonorsForceScalar) {
  if (!simd::have_avx2()) {
    GTEST_SKIP() << "override divergence is only observable on an AVX2 host";
  }
  IsaGuard guard;

  // A small deterministic book (n % 4 == 0), every column non-trivial.
  constexpr std::size_t n = 40;
  std::vector<double> delta(n), gamma(n), vega(n), volga(n), vanna(n), theta(n), rho(n), charm(n);
  std::vector<double> qty(n), dS(n), dSigma(n), dt(n), dr(n);
  for (std::size_t i = 0; i < n; ++i) {
    const double p = static_cast<double>(i);
    delta[i] = -0.9 + 0.05 * p;
    gamma[i] = 0.01 + 0.002 * p;
    vega[i] = 2.0 + 0.5 * p;
    volga[i] = -30.0 + 1.5 * p;
    vanna[i] = -12.0 + 0.6 * p;
    theta[i] = -20.0 + 0.9 * p;
    rho[i] = -40.0 + 2.0 * p;
    charm[i] = -4.0 + 0.2 * p;
    qty[i] = (i % 3 == 0) ? -100.0 : 75.0;
    dS[i] = -8.0 + 0.4 * p;
    dSigma[i] = -0.2 + 0.01 * p;
    dt[i] = -0.03 + 0.001 * p;
    dr[i] = -0.01 + 0.0004 * p;
  }

  // Scalar reference: the exact op order of pnl_taylor_explain_batch_scalar.
  std::vector<double> ref_total(n), ref_gamma(n), ref_vanna(n);
  for (std::size_t i = 0; i < n; ++i) {
    const double w = qty[i];
    const double pd = delta[i] * dS[i];
    const double pg = 0.5 * gamma[i] * dS[i] * dS[i];
    const double pv = vega[i] * dSigma[i];
    const double pvol = 0.5 * volga[i] * dSigma[i] * dSigma[i];
    const double pvanna = vanna[i] * dS[i] * dSigma[i];
    const double pth = theta[i] * dt[i];
    const double prho = rho[i] * dr[i];
    const double pcharm = charm[i] * dS[i] * dt[i];
    const double explained = pd + pg + pv + pvol + pvanna + pth + prho + pcharm;
    ref_total[i] = w * explained;
    ref_gamma[i] = w * pg;
    ref_vanna[i] = w * pvanna;
  }

  simd::PnlExplainInputs in{};
  in.delta = delta.data();
  in.gamma = gamma.data();
  in.vega = vega.data();
  in.volga = volga.data();
  in.vanna = vanna.data();
  in.theta = theta.data();
  in.rho = rho.data();
  in.charm = charm.data();
  in.qty = qty.data();
  in.dS = dS.data();
  in.dSigma = dSigma.data();
  in.dt = dt.data();
  in.dr = dr.data();

  std::vector<double> o_delta(n), o_gamma(n), o_vega(n), o_volga(n), o_vanna(n), o_theta(n),
      o_rho(n), o_charm(n), o_total(n);
  simd::PnlExplainOutputs out{o_delta.data(), o_gamma.data(), o_vega.data(),
                              o_volga.data(), o_vanna.data(), o_theta.data(),
                              o_rho.data(),   o_charm.data(), o_total.data()};

  simd::set_simd_isa_override(simd::SimdIsa::ForceScalar);
  simd::pnl_taylor_explain_batch(in, out, n);
  for (std::size_t i = 0; i < n; ++i) {
    EXPECT_EQ(o_total[i], ref_total[i]) << "total lane " << i;
    EXPECT_EQ(o_gamma[i], ref_gamma[i]) << "gamma lane " << i;
    EXPECT_EQ(o_vanna[i], ref_vanna[i]) << "vanna lane " << i;
  }
}

// The MathMode alias must divert the same way (Reference == ForceScalar): a
// representative entry (public black76 price) is checked through set_math_mode.
TEST(SimdIsaOverride, MathModeReferenceRoutesScalar) {
  if (!simd::have_avx2()) {
    GTEST_SKIP() << "override divergence is only observable on an AVX2 host";
  }
  IsaGuard guard;
  const B76Grid g = make_b76_grid();
  const std::size_t n = g.size();

  std::vector<double> ref(n);
  for (std::size_t i = 0; i < n; ++i) {
    ref[i] = black76_price(g.F[i], g.K[i], g.T[i], g.sigma[i], g.df[i], g.side[i]);
  }

  simd::set_math_mode(simd::MathMode::Reference);
  ASSERT_EQ(simd::active_math_mode(), simd::MathMode::Reference);
  std::vector<double> got(n, 0.0);
  ASSERT_TRUE(black76_price_batch(g.F, g.K, g.T, g.sigma, g.df, g.side, std::span<double>(got))
                  .has_value());
  for (std::size_t i = 0; i < n; ++i) {
    EXPECT_EQ(got[i], ref[i]) << "lane " << i;
  }

  // FastDeterministic returns to the AVX2 numbers (teeth on the alias too).
  simd::set_math_mode(simd::MathMode::FastDeterministic);
  std::vector<double> fast(n, 0.0);
  ASSERT_TRUE(black76_price_batch(g.F, g.K, g.T, g.sigma, g.df, g.side, std::span<double>(fast))
                  .has_value());
  EXPECT_GT(bit_diffs(fast, ref), 0u) << "FastDeterministic did not take the AVX2 fast path";
}

// PR-C2 non-AVX2 test leg: proves the ATX_SIMD_ISA env override seeds the
// process-global scalar ISA at load. Meaningful only under the
// `atx_vol_pricing_forcescalar` ctest leg (tests/CMakeLists.txt), which launches
// the binary with ATX_SIMD_ISA=ForceScalar and a filter that runs NO override-
// mutating suite; a normal run leaves the var unset and skips. On an AVX2 host
// use_avx2() is TRUE by default, so asserting FALSE here confirms the env actually
// diverted the whole process onto the scalar path.
TEST(ScalarLegEnv, ForceScalarEnvSeedsScalarOverride) {
#if defined(_WIN32)
  std::size_t sz = 0;
  char buf[16] = {};
  const bool set = (getenv_s(&sz, buf, sizeof(buf), "ATX_SIMD_ISA") == 0 && sz != 0);
  const bool force_scalar = set && std::string_view{buf} == "ForceScalar";
#else
  const char *e = std::getenv("ATX_SIMD_ISA");
  const bool force_scalar = (e != nullptr && std::string_view{e} == "ForceScalar");
#endif
  if (!force_scalar) {
    GTEST_SKIP() << "ATX_SIMD_ISA != ForceScalar; env leg not active";
  }
  EXPECT_EQ(simd::simd_isa_override(), simd::SimdIsa::ForceScalar);
  EXPECT_FALSE(simd::use_avx2());
}

} // namespace
} // namespace atx::vol
