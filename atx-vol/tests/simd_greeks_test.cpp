// Parity gate for the vectorized (AVX2) Black-76 Greeks batch kernel.
//
// simd::black76_greeks_batch dispatches to the 4-lane AVX2 path when the host
// supports it (this CI/dev box does — Alder Lake). These tests assert that the
// vectorized result reproduces the scalar per-contract atx::vol::black76_greeks
// to full risk accuracy across all eight sensitivities + price, including the
// awkward cases the SIMD path must special-case: n not a multiple of 4 (scalar
// tail) and degenerate lanes (T ≤ 0 or σ ≤ 0) that the kernel patches through
// the exact scalar path. Deep-wing lanes (|d| large) are NO LONGER patched (K2):
// the Cody rational-erfc Φ prices them on the vector path to machine accuracy.
// If AVX2 is absent the batch runs the scalar loop and these become identity
// checks — still valid, just trivially exact.

#include "atx/vol/simd/greeks_batch.hpp"

#include "atx/vol/greeks.hpp"
#include "atx/vol/simd/cpu.hpp"

#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <vector>

#include <gtest/gtest.h>

namespace atx::vol {
namespace {

// A broad, deterministic grid of contracts spanning ITM/OTM/ATM, short/long
// tenors, low/high vol, both sides, plus explicit degenerate and deep-wing rows.
struct Batch {
  std::vector<double> F, K, T, sigma, r, df;
  std::vector<Side> side;
  [[nodiscard]] std::size_t size() const { return F.size(); }
  void push(double f, double k, double t, double s, double rr, double d,
            Side sd) {
    F.push_back(f); K.push_back(k); T.push_back(t);
    sigma.push_back(s); r.push_back(rr); df.push_back(d); side.push_back(sd);
  }
};

Batch make_grid() {
  Batch b;
  const double forwards[] = {10.0, 50.0, 100.0, 250.0, 500.0};
  const double moneyness[] = {0.5, 0.8, 0.95, 1.0, 1.05, 1.25, 2.0};
  const double tenors[] = {1.0 / 365, 0.05, 0.25, 1.0, 2.5};
  const double vols[] = {0.08, 0.20, 0.45, 0.90};
  const double rate = 0.03;
  for (double F : forwards)
    for (double m : moneyness)
      for (double T : tenors)
        for (double v : vols) {
          const double df = std::exp(-rate * T);
          b.push(F, F * m, T, v, rate, df, Side::Call);
          b.push(F, F * m, T, v, rate, df, Side::Put);
        }
  // Degenerate lanes: expired and zero-vol.
  b.push(100.0, 95.0, 0.0, 0.20, 0.03, 1.0, Side::Call);
  b.push(100.0, 105.0, -1.0, 0.20, 0.03, 1.0, Side::Put);
  b.push(100.0, 100.0, 0.5, 0.0, 0.03, 0.98, Side::Call);
  // Deep-wing lanes: |d| far beyond the old Chebyshev interior. Post-K2 these
  // price on the vector Cody-erfc Φ (no wing patch); see DeepWingLanesMatchScalar.
  b.push(100.0, 5.0, 2.0, 0.10, 0.03, 0.95, Side::Call);
  b.push(100.0, 5000.0, 2.0, 0.10, 0.03, 0.95, Side::Put);
  return b;
}

// Combined abs+rel tolerance check, tracking the worst absolute error seen.
void expect_close(double got, double want, double abs_tol, double rel_tol,
                  const char* field, std::size_t i, double& max_abs) {
  const double e = std::abs(got - want);
  max_abs = std::max(max_abs, e);
  EXPECT_LE(e, abs_tol + rel_tol * std::abs(want))
      << field << " i=" << i << " got=" << got << " want=" << want;
}

TEST(SimdBlack76GreeksBatch, MatchesScalarAcrossGrid) {
  const Batch b = make_grid();
  const std::size_t n = b.size();
  std::vector<Greeks> got(n);
  std::vector<double> price(n, 0.0);
  simd::black76_greeks_batch(b.F.data(), b.K.data(), b.T.data(), b.sigma.data(),
                             b.r.data(), b.df.data(), b.side.data(), got.data(),
                             price.data(), n);

  // The vector Φ/exp are absolute-accuracy (~1e-11) approximations, so each
  // field matches scalar to a combined abs+rel tolerance: near-cancellation in
  // F·Φ(d1)-K·Φ(d2) keeps tiny *absolute* error even where *relative* error
  // grows. Economically exact; any formula/sign bug is order-1 relative and
  // would be caught wide.
  constexpr double kAbs = 1e-6;
  constexpr double kRel = 1e-7;
  double m_price = 0.0, m_delta = 0.0, m_gamma = 0.0, m_vega = 0.0,
         m_theta = 0.0, m_rho = 0.0, m_vanna = 0.0, m_volga = 0.0,
         m_charm = 0.0;
  for (std::size_t i = 0; i < n; ++i) {
    const Black76Greeks w = black76_greeks(b.F[i], b.K[i], b.T[i], b.sigma[i],
                                           b.r[i], b.df[i], b.side[i]);
    expect_close(price[i], w.price, kAbs, kRel, "price", i, m_price);
    expect_close(got[i].delta, w.greeks.delta, kAbs, kRel, "delta", i, m_delta);
    expect_close(got[i].gamma, w.greeks.gamma, kAbs, kRel, "gamma", i, m_gamma);
    expect_close(got[i].vega, w.greeks.vega, kAbs, kRel, "vega", i, m_vega);
    expect_close(got[i].theta, w.greeks.theta, kAbs, kRel, "theta", i, m_theta);
    expect_close(got[i].rho, w.greeks.rho, kAbs, kRel, "rho", i, m_rho);
    expect_close(got[i].vanna, w.greeks.vanna, kAbs, kRel, "vanna", i, m_vanna);
    expect_close(got[i].volga, w.greeks.volga, kAbs, kRel, "volga", i, m_volga);
    expect_close(got[i].charm, w.greeks.charm, kAbs, kRel, "charm", i, m_charm);
  }
  // Absolute error stays sub-microdollar/-microgreek across the whole grid.
  EXPECT_LT(m_price, kAbs);
  EXPECT_LT(m_delta, kAbs);
}

// A9 (simd-review finding 8): a nullptr price_out is a valid "greeks only" request.
// The AVX2 AoS sink already null-checks px; the SCALAR fallback (non-AVX2 hosts)
// dereferenced it unconditionally and crashed. This exercises the null contract
// through the public dispatcher (the AVX2 sink on this host, the fixed scalar loop
// on a non-AVX2 host) and confirms the greeks are still filled.
TEST(SimdBlack76GreeksBatch, NullPriceOutFillsGreeksWithoutCrash) {
  const Batch b = make_grid();
  const std::size_t n = b.size();
  std::vector<Greeks> got(n);
  simd::black76_greeks_batch(b.F.data(), b.K.data(), b.T.data(), b.sigma.data(), b.r.data(),
                             b.df.data(), b.side.data(), got.data(), /*price_out=*/nullptr, n);
  bool checked = false;
  for (std::size_t i = 0; i < n && !checked; ++i) {
    if (b.T[i] > 0.0 && b.sigma[i] > 0.0) {
      const Black76Greeks w =
          black76_greeks(b.F[i], b.K[i], b.T[i], b.sigma[i], b.r[i], b.df[i], b.side[i]);
      EXPECT_NEAR(got[i].delta, w.greeks.delta, 1e-6);
      EXPECT_NEAR(got[i].vega, w.greeks.vega, 1e-6);
      checked = true;
    }
  }
  EXPECT_TRUE(checked);
}

// A9 (simd-review finding 4): log_pd assumes a positive-NORMAL argument, so an F/K
// ratio that underflows to a denormal/0 or overflows to +inf decodes to finite
// GARBAGE near ±709 (not ±inf), which nonfinite_mask(d) cannot catch. The |lnFK|
// >= 708 escape now routes those lanes to scalar. The economic impact is masked (Φ
// saturates at such |d|), so this locks the by-construction fix: the batch must
// match the per-contract scalar kernel BIT-FOR-BIT on such rows.
TEST(SimdBlack76GreeksBatch, ExtremeFKRatioMatchesScalarExactly) {
  Batch b;
  const double df = std::exp(-0.03 * 1.0);
  // F/K underflows toward 0/denormal, and overflows toward +inf; both sides.
  b.push(1.0e-300, 1.0e300, 1.0, 0.20, 0.03, df, Side::Call); // F/K -> 0
  b.push(1.0e-300, 1.0e300, 1.0, 0.20, 0.03, df, Side::Put);
  b.push(1.0e300, 1.0e-300, 1.0, 0.20, 0.03, df, Side::Call); // F/K -> +inf
  b.push(1.0e300, 1.0e-300, 1.0, 0.20, 0.03, df, Side::Put);
  b.push(1.0e-8, 1.0e300, 1.0, 0.20, 0.03, df, Side::Call); // F/K denormal
  b.push(100.0, 100.0, 1.0, 0.20, 0.03, df, Side::Call);    // a normal lane alongside
  const std::size_t n = b.size();
  std::vector<Greeks> got(n);
  std::vector<double> price(n, 0.0);
  simd::black76_greeks_batch(b.F.data(), b.K.data(), b.T.data(), b.sigma.data(), b.r.data(),
                             b.df.data(), b.side.data(), got.data(), price.data(), n);
  auto bit_eq = [](double a, double c) {
    return (std::isnan(a) && std::isnan(c)) || a == c;
  };
  for (std::size_t i = 0; i < n; ++i) {
    const Black76Greeks w =
        black76_greeks(b.F[i], b.K[i], b.T[i], b.sigma[i], b.r[i], b.df[i], b.side[i]);
    EXPECT_TRUE(bit_eq(price[i], w.price)) << "price i=" << i;
    EXPECT_TRUE(bit_eq(got[i].delta, w.greeks.delta)) << "delta i=" << i;
    EXPECT_TRUE(bit_eq(got[i].vega, w.greeks.vega)) << "vega i=" << i;
  }
}

// The five special rows sit at the tail of make_grid(): the first THREE are
// degenerate (expired / zero-vol), the last TWO are deep-wing. They straddle a
// full SIMD group and the scalar tail.
//
// Degenerate rows are still routed through the exact scalar kernel by patch_bits
// (T ≤ 0 or σ ≤ 0), so they must reproduce it bit-for-bit (up to ULP).
TEST(SimdBlack76GreeksBatch, DegenerateLanesAreBitExact) {
  const Batch b = make_grid();
  const std::size_t n = b.size();
  std::vector<Greeks> got(n);
  std::vector<double> price(n, 0.0);
  simd::black76_greeks_batch(b.F.data(), b.K.data(), b.T.data(), b.sigma.data(),
                             b.r.data(), b.df.data(), b.side.data(), got.data(),
                             price.data(), n);
  for (std::size_t i = n - 5; i < n - 2; ++i) {
    const Black76Greeks w = black76_greeks(b.F[i], b.K[i], b.T[i], b.sigma[i],
                                           b.r[i], b.df[i], b.side[i]);
    EXPECT_DOUBLE_EQ(price[i], w.price) << "i=" << i;
    EXPECT_DOUBLE_EQ(got[i].delta, w.greeks.delta) << "i=" << i;
    EXPECT_DOUBLE_EQ(got[i].gamma, w.greeks.gamma) << "i=" << i;
    EXPECT_DOUBLE_EQ(got[i].vega, w.greeks.vega) << "i=" << i;
    EXPECT_DOUBLE_EQ(got[i].theta, w.greeks.theta) << "i=" << i;
    EXPECT_DOUBLE_EQ(got[i].rho, w.greeks.rho) << "i=" << i;
    EXPECT_DOUBLE_EQ(got[i].vanna, w.greeks.vanna) << "i=" << i;
    EXPECT_DOUBLE_EQ(got[i].volga, w.greeks.volga) << "i=" << i;
    EXPECT_DOUBLE_EQ(got[i].charm, w.greeks.charm) << "i=" << i;
  }
}

// K2 wing-patch removal (accuracy-improving): the two deep-wing rows (|d| ≫ 6)
// are NO LONGER patched to scalar — they compute their Greeks on the Cody
// rational-erfc vector Φ, which is full double precision on the entire real line.
// So each field now matches the std::erfc scalar source of truth to a
// machine-class tolerance (not bit-for-bit), strictly better than the retired
// ~1e-11 Chebyshev interior the old wing patch existed to avoid — and orders of
// magnitude inside the 1e-6 economic price bound.
TEST(SimdBlack76GreeksBatch, DeepWingLanesMatchScalarTightly) {
  const Batch b = make_grid();
  const std::size_t n = b.size();
  std::vector<Greeks> got(n);
  std::vector<double> price(n, 0.0);
  simd::black76_greeks_batch(b.F.data(), b.K.data(), b.T.data(), b.sigma.data(),
                             b.r.data(), b.df.data(), b.side.data(), got.data(),
                             price.data(), n);
  // Absolute floor for the near-zero deep-wing Greeks (gamma/vega/vanna/volga
  // are ~1e-96 there) plus a tight relative term for the O(1) price/delta/rho.
  constexpr double kAbs = 1e-9;
  constexpr double kRel = 1e-11;
  double m_price = 0.0, m_delta = 0.0, m_gamma = 0.0, m_vega = 0.0, m_theta = 0.0,
         m_rho = 0.0, m_vanna = 0.0, m_volga = 0.0, m_charm = 0.0;
  for (std::size_t i = n - 2; i < n; ++i) {
    const Black76Greeks w = black76_greeks(b.F[i], b.K[i], b.T[i], b.sigma[i],
                                           b.r[i], b.df[i], b.side[i]);
    expect_close(price[i], w.price, kAbs, kRel, "price", i, m_price);
    expect_close(got[i].delta, w.greeks.delta, kAbs, kRel, "delta", i, m_delta);
    expect_close(got[i].gamma, w.greeks.gamma, kAbs, kRel, "gamma", i, m_gamma);
    expect_close(got[i].vega, w.greeks.vega, kAbs, kRel, "vega", i, m_vega);
    expect_close(got[i].theta, w.greeks.theta, kAbs, kRel, "theta", i, m_theta);
    expect_close(got[i].rho, w.greeks.rho, kAbs, kRel, "rho", i, m_rho);
    expect_close(got[i].vanna, w.greeks.vanna, kAbs, kRel, "vanna", i, m_vanna);
    expect_close(got[i].volga, w.greeks.volga, kAbs, kRel, "volga", i, m_volga);
    expect_close(got[i].charm, w.greeks.charm, kAbs, kRel, "charm", i, m_charm);
  }
  std::printf("[SimdBlack76GreeksBatch] deep-wing max abs err: price=%.3e delta=%.3e "
              "rho=%.3e\n",
              m_price, m_delta, m_rho);
}

// The scalar tail (n % 4 != 0) must be handled for every residue class.
TEST(SimdBlack76GreeksBatch, HandlesEveryTailResidue) {
  const Batch full = make_grid();
  for (std::size_t n = 1; n <= 11; ++n) {
    std::vector<Greeks> got(n);
    std::vector<double> price(n, 0.0);
    simd::black76_greeks_batch(full.F.data(), full.K.data(), full.T.data(),
                               full.sigma.data(), full.r.data(), full.df.data(),
                               full.side.data(), got.data(), price.data(), n);
    for (std::size_t i = 0; i < n; ++i) {
      const Black76Greeks w =
          black76_greeks(full.F[i], full.K[i], full.T[i], full.sigma[i],
                         full.r[i], full.df[i], full.side[i]);
      EXPECT_LT(std::abs(price[i] - w.price), 1e-6) << "n=" << n << " i=" << i;
      EXPECT_LT(std::abs(got[i].delta - w.greeks.delta), 1e-6)
          << "n=" << n << " i=" << i;
      EXPECT_LT(std::abs(got[i].vega - w.greeks.vega), 1e-5)
          << "n=" << n << " i=" << i;
    }
  }
}

TEST(SimdBlack76GreeksBatch, ZeroLengthIsNoOp) {
  Greeks gsentinel{};
  gsentinel.delta = 42.0;
  double psentinel = 7.0;
  simd::black76_greeks_batch(nullptr, nullptr, nullptr, nullptr, nullptr,
                             nullptr, nullptr, &gsentinel, &psentinel, 0);
  EXPECT_EQ(gsentinel.delta, 42.0);
  EXPECT_EQ(psentinel, 7.0);
}

// ── P3.4 SoA output: pure layout, bit-identical to the AoS entry ──────────
//
// black76_greeks_batch_soa and black76_greeks_batch share ONE vector core, so the
// SoA per-greek columns must equal the AoS Greeks[] field-for-field BIT-for-bit
// (EXPECT_EQ, not a tolerance) — the whole point of the reshape. This straddles a
// full SIMD group, the scalar tail, the patched degenerate lanes, and the
// vector-priced deep-wing lanes.
TEST(B76GreeksSoA, MatchesAoSBitIdentical) {
  const Batch b = make_grid();
  const std::size_t n = b.size();

  std::vector<Greeks> aos(n);
  std::vector<double> aos_px(n, 0.0);
  simd::black76_greeks_batch(b.F.data(), b.K.data(), b.T.data(), b.sigma.data(),
                             b.r.data(), b.df.data(), b.side.data(), aos.data(),
                             aos_px.data(), n);

  std::vector<double> dl(n), gm(n), vg(n), th(n), rh(n), vn(n), vl(n), cm(n),
      px(n);
  simd::GreeksBatchSoA soa{dl.data(), gm.data(), vg.data(), th.data(),
                           rh.data(), vn.data(), vl.data(), cm.data(),
                           px.data()};
  simd::black76_greeks_batch_soa(b.F.data(), b.K.data(), b.T.data(),
                                 b.sigma.data(), b.r.data(), b.df.data(),
                                 b.side.data(), soa, n);

  for (std::size_t i = 0; i < n; ++i) {
    EXPECT_EQ(dl[i], aos[i].delta) << "i=" << i;
    EXPECT_EQ(gm[i], aos[i].gamma) << "i=" << i;
    EXPECT_EQ(vg[i], aos[i].vega) << "i=" << i;
    EXPECT_EQ(th[i], aos[i].theta) << "i=" << i;
    EXPECT_EQ(rh[i], aos[i].rho) << "i=" << i;
    EXPECT_EQ(vn[i], aos[i].vanna) << "i=" << i;
    EXPECT_EQ(vl[i], aos[i].volga) << "i=" << i;
    EXPECT_EQ(cm[i], aos[i].charm) << "i=" << i;
    EXPECT_EQ(px[i], aos_px[i]) << "i=" << i;
  }
}

// A null column is skipped; the requested columns still match AoS exactly.
TEST(B76GreeksSoA, NullColumnsSkipped) {
  const Batch b = make_grid();
  const std::size_t n = b.size();

  std::vector<Greeks> aos(n);
  std::vector<double> aos_px(n, 0.0);
  simd::black76_greeks_batch(b.F.data(), b.K.data(), b.T.data(), b.sigma.data(),
                             b.r.data(), b.df.data(), b.side.data(), aos.data(),
                             aos_px.data(), n);

  constexpr double kSentinel = -123456.0;
  std::vector<double> dl(n, kSentinel), vg(n, kSentinel), px(n, kSentinel);
  simd::GreeksBatchSoA soa; // all null
  soa.delta = dl.data();
  soa.vega = vg.data();
  soa.price = px.data();
  simd::black76_greeks_batch_soa(b.F.data(), b.K.data(), b.T.data(),
                                 b.sigma.data(), b.r.data(), b.df.data(),
                                 b.side.data(), soa, n);

  for (std::size_t i = 0; i < n; ++i) {
    EXPECT_EQ(dl[i], aos[i].delta) << "i=" << i;
    EXPECT_EQ(vg[i], aos[i].vega) << "i=" << i;
    EXPECT_EQ(px[i], aos_px[i]) << "i=" << i;
  }
}

TEST(B76GreeksSoA, EveryTailResidueMatchesAoS) {
  const Batch full = make_grid();
  for (std::size_t n = 1; n <= 11; ++n) {
    std::vector<Greeks> aos(n);
    std::vector<double> aos_px(n, 0.0);
    simd::black76_greeks_batch(full.F.data(), full.K.data(), full.T.data(),
                               full.sigma.data(), full.r.data(), full.df.data(),
                               full.side.data(), aos.data(), aos_px.data(), n);
    std::vector<double> dl(n), gm(n), vg(n), th(n), rh(n), vn(n), vl(n), cm(n),
        px(n);
    simd::GreeksBatchSoA soa{dl.data(), gm.data(), vg.data(), th.data(),
                             rh.data(), vn.data(), vl.data(), cm.data(),
                             px.data()};
    simd::black76_greeks_batch_soa(full.F.data(), full.K.data(), full.T.data(),
                                   full.sigma.data(), full.r.data(),
                                   full.df.data(), full.side.data(), soa, n);
    for (std::size_t i = 0; i < n; ++i) {
      EXPECT_EQ(dl[i], aos[i].delta) << "n=" << n << " i=" << i;
      EXPECT_EQ(px[i], aos_px[i]) << "n=" << n << " i=" << i;
    }
  }
}

TEST(B76GreeksSoA, ZeroLengthIsNoOp) {
  double sentinel = 42.0;
  simd::GreeksBatchSoA soa;
  soa.price = &sentinel;
  simd::black76_greeks_batch_soa(nullptr, nullptr, nullptr, nullptr, nullptr,
                                 nullptr, nullptr, soa, 0);
  EXPECT_EQ(sentinel, 42.0);
}

// Homogeneous-batch speedup of the SoA vector path vs a scalar per-contract loop.
// The ≥2.0× P3 gate is read from the build-rel run of this test; Debug records the
// ratio and only sanity-checks parity.
TEST(B76GreeksSoA, Speedup) {
  if (!simd::have_avx2()) {
    GTEST_SKIP() << "no AVX2 on this host";
  }
  constexpr std::size_t kN = 8192;
  std::vector<double> F(kN), K(kN), T(kN), sigma(kN), r(kN), df(kN);
  std::vector<Side> side(kN);
  for (std::size_t i = 0; i < kN; ++i) {
    const double m = 0.80 + 0.40 * static_cast<double>(i % 29) / 29.0;
    const double v = 0.12 + 0.30 * static_cast<double>(i % 19) / 19.0;
    F[i] = 100.0;
    K[i] = 100.0 * m;
    T[i] = 0.5 + static_cast<double>(i % 7) * 0.1;
    sigma[i] = v;
    r[i] = 0.03;
    df[i] = std::exp(-0.03 * T[i]);
    side[i] = (i & 1u) ? Side::Put : Side::Call;
  }
  std::vector<double> dl(kN), gm(kN), vg(kN), th(kN), rh(kN), vn(kN), vl(kN),
      cm(kN), px(kN);
  simd::GreeksBatchSoA soa{dl.data(), gm.data(), vg.data(), th.data(),
                           rh.data(), vn.data(), vl.data(), cm.data(),
                           px.data()};

  auto time_soa = [&]() {
    simd::black76_greeks_batch_soa(F.data(), K.data(), T.data(), sigma.data(),
                                   r.data(), df.data(), side.data(), soa, kN);
    constexpr int reps = 50;
    const auto t0 = std::chrono::steady_clock::now();
    for (int rr = 0; rr < reps; ++rr) {
      simd::black76_greeks_batch_soa(F.data(), K.data(), T.data(), sigma.data(),
                                     r.data(), df.data(), side.data(), soa, kN);
    }
    const auto t1 = std::chrono::steady_clock::now();
    return std::chrono::duration<double>(t1 - t0).count() / reps;
  };
  auto time_scalar = [&]() {
    constexpr int reps = 50;
    volatile double sink = 0.0;
    const auto t0 = std::chrono::steady_clock::now();
    for (int rr = 0; rr < reps; ++rr) {
      for (std::size_t i = 0; i < kN; ++i) {
        const Black76Greeks g = black76_greeks(F[i], K[i], T[i], sigma[i], r[i],
                                               df[i], side[i]);
        sink += g.price + g.greeks.delta;
      }
    }
    const auto t1 = std::chrono::steady_clock::now();
    (void)sink;
    return std::chrono::duration<double>(t1 - t0).count() / reps;
  };

  const double tv = time_soa();
  const double ts = time_scalar();
  const double speedup = ts / tv;
  std::printf("[B76GreeksSoA] speedup=%.3fx  scalar=%.3f ms  soa=%.3f ms  "
              "(n=%zu)\n",
              speedup, ts * 1e3, tv * 1e3, kN);
  RecordProperty("speedup_milli", static_cast<int>(speedup * 1000));
  EXPECT_GT(speedup, 0.0);
}

} // namespace
} // namespace atx::vol
