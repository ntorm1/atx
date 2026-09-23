// combine_signal_combiner_test.cpp — Lane 5: signal-space zoo combiners.
//
//   * SignalStore z-scores each date row on insert (mean 0 / sd 1 over finite cells).
//   * FamaMacBethRidge recovers the planted weights of a noise-free 3-signal DGP to 1e-6.
//   * Every combiner is truncation-invariant (fit/apply firewall): mutating signal or
//     forward-return rows >= window.end leaves the weights byte-identical.
//   * Grinold-Kahn on orthogonal IC series gives w ∝ E[IC]/Var(IC).
//   * ICIR-EWMA puts ~0 weight on a pure-noise signal (t-stat haircut).
//   * Kakushadze residual orthogonality (to Λ and to the cluster dummies) and the
//     push-through identity between the N>M and N<=M branches.
//   * Acceptance: on a synthetic zoo FMB's OOS IR beats PnL-space ShrinkageMv.
//
// Suite: SignalCombiner

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <span>
#include <vector>

#include <gtest/gtest.h>

#include "atx/engine/combine/combiner.hpp"
#include "atx/engine/combine/metrics.hpp"
#include "atx/engine/combine/signal_combiner.hpp"
#include "atx/engine/combine/signal_store.hpp"
#include "atx/engine/combine/store.hpp"

namespace atx_test_l5_combine_signal_combiner {

using atx::f64;
using atx::usize;
using atx::core::linalg::MatX;
using atx::core::linalg::VecX;
namespace cb = atx::engine::combine;

struct Rng {
  std::uint64_t s;
  f64 uni() {
    s = s * 6364136223846793005ULL + 1442695040888963407ULL;
    return (static_cast<f64>(s >> 11U) + 0.5) / 9007199254740992.0;
  }
  f64 gauss() {
    const f64 u1 = uni();
    const f64 u2 = uni();
    return std::sqrt(-2.0 * std::log(u1)) * std::cos(6.283185307179586 * u2);
  }
};

std::vector<f64> gauss_panel(Rng &g, usize n) {
  std::vector<f64> v(n);
  for (f64 &x : v) {
    x = g.gauss();
  }
  return v;
}

// Noise-free DGP: fwd = Σ beta_a z_a (stored z-scores).
cb::SignalStore planted_store(usize t, usize n, const std::vector<f64> &beta, std::uint64_t seed) {
  auto st = cb::SignalStore::create(t, n);
  EXPECT_TRUE(st.has_value());
  Rng g{seed};
  for (usize a = 0U; a < beta.size(); ++a) {
    EXPECT_TRUE(st->add_signal(gauss_panel(g, t * n)).has_value());
  }
  std::vector<f64> fwd(t * n, 0.0);
  for (usize a = 0U; a < beta.size(); ++a) {
    const auto z = st->signal(a);
    for (usize c = 0U; c < t * n; ++c) {
      fwd[c] += beta[a] * z[c];
    }
  }
  EXPECT_TRUE(st->set_forward_returns(fwd).has_value());
  return std::move(*st);
}

TEST(SignalCombiner, StoreZScoresEachDateRow) {
  auto st = cb::SignalStore::create(3, 5);
  ASSERT_TRUE(st.has_value());
  const std::vector<f64> raw{1, 2, 3, 4, 100, 5, 5, 5, 5, 5, 0, std::nan(""), 2, 4, 6};
  ASSERT_TRUE(st->add_signal(raw).has_value());
  for (usize t = 0U; t < 3U; ++t) {
    const auto row = st->signal_row(0, t);
    f64 s = 0.0;
    f64 ss = 0.0;
    usize n = 0U;
    for (const f64 x : row) {
      if (std::isfinite(x)) {
        s += x;
        ss += x * x;
        ++n;
      }
    }
    EXPECT_NEAR(s, 0.0, 1e-12) << t;
    if (t != 1U) {
      EXPECT_NEAR(ss / static_cast<f64>(n), 1.0, 1e-12) << t;
    } else {
      EXPECT_EQ(ss, 0.0); // flat row carries no information → zeros
    }
  }
  EXPECT_TRUE(std::isnan(st->signal_row(0, 2)[1]));
  EXPECT_FALSE(st->add_signal(std::vector<f64>(4, 0.0)).has_value());
}

TEST(SignalCombiner, FamaMacBethRecoversPlantedWeights) {
  const std::vector<f64> beta{0.5, -0.3, 0.2};
  const cb::SignalStore st = planted_store(30, 50, beta, 11U);
  const auto r = cb::FamaMacBethRidge{}.fit(st, {0, 30});
  ASSERT_TRUE(r.has_value()) << r.error().message();
  ASSERT_EQ(r->w.size(), 3U);
  for (usize a = 0U; a < 3U; ++a) {
    EXPECT_NEAR(r->w[a], beta[a], 1e-6);
  }
  EXPECT_EQ(r->fit_begin, 0U);
  EXPECT_EQ(r->fit_end, 30U);
}

TEST(SignalCombiner, FamaMacBethRidgeShrinksTowardZero) {
  const cb::SignalStore st = planted_store(20, 40, {0.5, -0.3, 0.2}, 12U);
  const auto ols = cb::FamaMacBethRidge{0.0}.fit(st, {0, 20});
  const auto rdg = cb::FamaMacBethRidge{1.0}.fit(st, {0, 20});
  ASSERT_TRUE(ols.has_value());
  ASSERT_TRUE(rdg.has_value());
  f64 a = 0.0;
  f64 b = 0.0;
  for (usize i = 0U; i < 3U; ++i) {
    a += std::abs(ols->w[i]);
    b += std::abs(rdg->w[i]);
  }
  EXPECT_LT(b, a);
}

// Build a noisy 4-signal store; `tail_seed` controls the rows >= cut only.
cb::SignalStore firewall_store(usize cut, std::uint64_t tail_seed) {
  constexpr usize kT = 40;
  constexpr usize kN = 30;
  constexpr usize kK = 4;
  auto st = cb::SignalStore::create(kT, kN);
  EXPECT_TRUE(st.has_value());
  Rng head{7U};
  Rng tail{tail_seed};
  std::vector<std::vector<f64>> sig(kK, std::vector<f64>(kT * kN));
  std::vector<f64> fwd(kT * kN);
  for (usize t = 0U; t < kT; ++t) {
    Rng &g = (t < cut) ? head : tail;
    for (usize i = 0U; i < kN; ++i) {
      f64 r = 0.0;
      for (usize a = 0U; a < kK; ++a) {
        const f64 z = g.gauss();
        sig[a][t * kN + i] = z;
        r += 0.1 * static_cast<f64>(a + 1U) * z;
      }
      fwd[t * kN + i] = r + g.gauss();
    }
  }
  for (usize a = 0U; a < kK; ++a) {
    EXPECT_TRUE(st->add_signal(sig[a]).has_value());
  }
  EXPECT_TRUE(st->set_forward_returns(fwd).has_value());
  return std::move(*st);
}

template <class C> void expect_truncation_invariant(const C &c) {
  const cb::SignalStore a = firewall_store(25, 100U);
  const cb::SignalStore b = firewall_store(25, 999U);
  const auto wa = c.fit(a, {5, 25});
  const auto wb = c.fit(b, {5, 25});
  ASSERT_TRUE(wa.has_value()) << wa.error().message();
  ASSERT_TRUE(wb.has_value());
  ASSERT_EQ(wa->w.size(), wb->w.size());
  for (usize i = 0U; i < wa->w.size(); ++i) {
    EXPECT_EQ(wa->w[i], wb->w[i]) << i; // byte-identical
  }
  // Sanity: the tail really differs (a fit over the full range would change).
  const auto fa = c.fit(a, {5, 40});
  const auto fb = c.fit(b, {5, 40});
  ASSERT_TRUE(fa.has_value());
  ASSERT_TRUE(fb.has_value());
  EXPECT_NE(fa->w, fb->w);
}

TEST(SignalCombiner, TruncationInvarianceAllMethods) {
  expect_truncation_invariant(cb::IcirEwmaCombiner{});
  expect_truncation_invariant(cb::GrinoldKahnCombiner{});
  expect_truncation_invariant(cb::FamaMacBethRidge{});
  expect_truncation_invariant(cb::KakushadzeRegression{});
}

TEST(SignalCombiner, GrinoldKahnOrthogonalIcsGiveMeanOverVariance) {
  // Two IC series with exactly zero sample covariance: col1 depends on parity only
  // through a pattern orthogonal to col0's centered values.
  MatX ic(8, 2);
  const f64 c0[8] = {1, -1, 1, -1, 1, -1, 1, -1};
  const f64 c1[8] = {1, 1, -1, -1, 1, 1, -1, -1};
  for (int r = 0; r < 8; ++r) {
    ic(r, 0) = 0.02 + 0.01 * c0[r];
    ic(r, 1) = 0.03 + 0.03 * c1[r];
  }
  const auto r = cb::grinold_kahn_weights(ic, cb::CovTarget::Sample);
  ASSERT_TRUE(r.has_value());
  const f64 w0 = 0.02 / (0.01 * 0.01);
  const f64 w1 = 0.03 / (0.03 * 0.03);
  EXPECT_NEAR(r->w[0], w0 / (w0 + w1), 1e-12);
  EXPECT_NEAR(r->w[1], w1 / (w0 + w1), 1e-12);
}

TEST(SignalCombiner, IcirEwmaDownWeightsPureNoise) {
  constexpr usize kT = 120;
  constexpr usize kN = 60;
  auto st = cb::SignalStore::create(kT, kN);
  ASSERT_TRUE(st.has_value());
  Rng g{5U};
  const std::vector<f64> good = gauss_panel(g, kT * kN);
  const std::vector<f64> junk = gauss_panel(g, kT * kN);
  std::vector<f64> fwd(kT * kN);
  for (usize c = 0U; c < fwd.size(); ++c) {
    fwd[c] = 0.3 * good[c] + g.gauss();
  }
  ASSERT_TRUE(st->add_signal(good).has_value());
  ASSERT_TRUE(st->add_signal(junk).has_value());
  ASSERT_TRUE(st->set_forward_returns(fwd).has_value());
  const auto r = cb::IcirEwmaCombiner{63.0, 2.0}.fit(*st, {0, kT});
  ASSERT_TRUE(r.has_value());
  EXPECT_GT(r->w[0], 0.9);
  EXPECT_LT(std::abs(r->w[1]), 0.1);
  EXPECT_GT(r->tstat[0], 5.0);
}

TEST(SignalCombiner, KakushadzeResidualIsOrthogonalToLambdaAndClusters) {
  constexpr Eigen::Index kM = 12;
  constexpr Eigen::Index kN = 40;
  Rng g{77U};
  MatX r(kM, kN);
  for (Eigen::Index t = 0; t < kM; ++t) {
    for (Eigen::Index a = 0; a < kN; ++a) {
      r(t, a) = 0.001 * static_cast<f64>(a % 5) + (0.5 + 0.05 * static_cast<f64>(a)) * g.gauss();
    }
  }
  std::vector<f64> e(kN);
  for (Eigen::Index a = 0; a < kN; ++a) {
    e[static_cast<usize>(a)] = r.col(a).mean();
  }
  std::vector<atx::u32> clusters(kN);
  for (Eigen::Index a = 0; a < kN; ++a) {
    clusters[static_cast<usize>(a)] = static_cast<atx::u32>(a % 3) * 10U;
  }
  for (const bool use_clusters : {false, true}) {
    const auto wr = cb::kakushadze_weights(
        r, e, use_clusters ? std::span<const atx::u32>(clusters) : std::span<const atx::u32>{}, 0.0);
    ASSERT_TRUE(wr.has_value()) << wr.error().message();
    const std::vector<f64> &w = *wr;
    // ε_a = w_a σ_a (up to η). Check Σ_a ε_a Λ_as = 0 for each kept date s.
    VecX sigma(kN);
    for (Eigen::Index a = 0; a < kN; ++a) {
      const f64 mu = r.col(a).mean();
      sigma[a] = std::sqrt((r.col(a).array() - mu).square().sum() / static_cast<f64>(kM - 1));
    }
    f64 scale = 0.0;
    for (Eigen::Index a = 0; a < kN; ++a) {
      scale += std::abs(w[static_cast<usize>(a)] * sigma[a]);
    }
    for (Eigen::Index s = 0; s < kM - 1; ++s) {
      f64 ymean = 0.0;
      VecX y(kN);
      for (Eigen::Index a = 0; a < kN; ++a) {
        y[a] = (r(s, a) - r.col(a).mean()) / sigma[a];
        ymean += y[a];
      }
      ymean /= static_cast<f64>(kN);
      f64 dot = 0.0;
      for (Eigen::Index a = 0; a < kN; ++a) {
        dot += w[static_cast<usize>(a)] * sigma[a] * (y[a] - ymean);
      }
      EXPECT_NEAR(dot / scale, 0.0, 1e-9) << "date " << s;
    }
    if (use_clusters) {
      for (atx::u32 c = 0U; c < 3U; ++c) {
        f64 sum = 0.0;
        for (Eigen::Index a = 0; a < kN; ++a) {
          if (clusters[static_cast<usize>(a)] == c * 10U) {
            sum += w[static_cast<usize>(a)] * sigma[a];
          }
        }
        EXPECT_NEAR(sum / scale, 0.0, 1e-9) << "cluster " << c;
      }
    }
  }
}

TEST(SignalCombiner, KakushadzeBranchesAgreeViaPushThrough) {
  // N=10 alphas, M=12 → uses the N×N branch; compare against the explicit (M−1)
  // branch formula computed by hand: ε = Ẽ − L(LᵀL+ρI)⁻¹LᵀẼ.
  constexpr Eigen::Index kM = 12;
  constexpr Eigen::Index kN = 10;
  Rng g{3U};
  MatX r(kM, kN);
  for (Eigen::Index t = 0; t < kM; ++t) {
    for (Eigen::Index a = 0; a < kN; ++a) {
      r(t, a) = 0.01 * static_cast<f64>(a) + g.gauss();
    }
  }
  std::vector<f64> e(kN);
  for (Eigen::Index a = 0; a < kN; ++a) {
    e[static_cast<usize>(a)] = r.col(a).mean();
  }
  const f64 ridge = 0.1;
  const auto wr = cb::kakushadze_weights(r, e, {}, ridge);
  ASSERT_TRUE(wr.has_value());
  const Eigen::RowVectorXd mu = r.colwise().mean();
  VecX sigma = ((r.rowwise() - mu).array().square().colwise().sum() / static_cast<f64>(kM - 1))
                   .sqrt()
                   .transpose();
  MatX l(kN, kM - 1);
  VecX et(kN);
  for (Eigen::Index a = 0; a < kN; ++a) {
    for (Eigen::Index s = 0; s < kM - 1; ++s) {
      l(a, s) = (r(s, a) - mu[a]) / sigma[a];
    }
    et[a] = e[static_cast<usize>(a)] / sigma[a];
  }
  l = l.rowwise() - l.colwise().mean(); // cross-sectional demean per date
  const f64 rho = ridge * (l * l.transpose()).trace() / static_cast<f64>(kN);
  MatX g2 = l.transpose() * l;
  g2.diagonal().array() += rho;
  const VecX eps = et - l * g2.ldlt().solve(l.transpose() * et);
  std::vector<f64> ref(kN);
  f64 gross = 0.0;
  for (Eigen::Index a = 0; a < kN; ++a) {
    ref[static_cast<usize>(a)] = eps[a] / sigma[a];
    gross += std::abs(ref[static_cast<usize>(a)]);
  }
  for (usize a = 0U; a < static_cast<usize>(kN); ++a) {
    EXPECT_NEAR((*wr)[a], ref[a] / gross, 1e-10) << a;
  }
}

TEST(SignalCombiner, RejectsBadWindowsAndEmptyStore) {
  auto empty = cb::SignalStore::create(10, 5);
  ASSERT_TRUE(empty.has_value());
  EXPECT_FALSE(cb::GrinoldKahnCombiner{}.fit(*empty, {0, 10}).has_value());
  const cb::SignalStore st = planted_store(10, 20, {0.1, 0.2}, 1U);
  EXPECT_FALSE(cb::FamaMacBethRidge{}.fit(st, {0, 11}).has_value());
  EXPECT_FALSE(cb::FamaMacBethRidge{}.fit(st, {5, 5}).has_value());
  EXPECT_FALSE(cb::KakushadzeRegression{}.fit(st, {0, 2}).has_value());
  EXPECT_FALSE(cb::FamaMacBethRidge{-1.0}.fit(st, {0, 10}).has_value());
  EXPECT_FALSE(cb::SignalStore::create(0, 5).has_value());
}

// ---------------------------------------------------------------------------
//  Acceptance: synthetic zoo — signal-space FMB vs PnL-space ShrinkageMv.
//  K=20 signals: 4 equal true edges, a CROWDED cluster of 12 noisy copies of edge 0,
//  and 4 junk; N=200 names, T_fit=40 dates, T_oos=200, averaged over 16 seeds.
//  In population PnL-space MV and FMB target the SAME solution (pnl covariance is
//  σ²·ZᵀZ/N²), so the difference is estimation efficiency: FMB reads the signal
//  collinearity exactly from T·N cells and the ridge shrinks the noisy per-date β,
//  while PnL-MV estimates a 20×20 covariance from 40 periods. GK is structurally the
//  PnL-MV estimator in IC units, so it is REPORTED (≈ MV), not asserted to win.
//  HONEST MARGIN: these 16 deterministic seeds give FMB-ridge(0.3) ≈ 0.345 vs MV ≈
//  0.332 IR/day (+4%, per-seed win rate 50%); an independent numpy study over 24
//  other seeds gave 0.338 vs 0.295 (+15%). The gain is real but modest — it is an
//  efficiency gain, not a different estimand.
// ---------------------------------------------------------------------------
struct Zoo {
  cb::SignalStore store;
  cb::AlphaStore pnl_pool;
};

Zoo make_zoo(std::uint64_t seed) {
  constexpr usize kT = 240;
  constexpr usize kN = 200;
  constexpr usize kK = 20;
  Rng g{seed};
  std::vector<std::vector<f64>> f(4, std::vector<f64>(kT * kN));
  for (auto &v : f) {
    for (f64 &x : v) {
      x = g.gauss();
    }
  }
  auto st = cb::SignalStore::create(kT, kN);
  EXPECT_TRUE(st.has_value());
  for (usize a = 0U; a < kK; ++a) {
    std::vector<f64> s(kT * kN);
    for (usize c = 0U; c < s.size(); ++c) {
      if (a < 4U) {
        s[c] = f[a][c] + 0.5 * g.gauss();
      } else if (a < 16U) {
        s[c] = f[0][c] + 1.0 * g.gauss(); // crowded cluster: 12 noisy copies of edge 0
      } else {
        s[c] = g.gauss();
      }
    }
    EXPECT_TRUE(st->add_signal(s).has_value());
  }
  std::vector<f64> fwd(kT * kN);
  for (usize c = 0U; c < fwd.size(); ++c) {
    fwd[c] = 0.02 * (f[0][c] + f[1][c] + f[2][c] + f[3][c]) + g.gauss();
  }
  EXPECT_TRUE(st->set_forward_returns(fwd).has_value());
  // PnL-space pool: pnl_a(t) = mean_i z_a(t,i)·r(t,i).
  cb::AlphaStore pool;
  for (usize a = 0U; a < kK; ++a) {
    std::vector<f64> pnl(kT);
    for (usize t = 0U; t < kT; ++t) {
      pnl[t] = cb::cross_section_slope(st->signal_row(a, t), st->fwd_row(t));
    }
    const std::vector<f64> pos(kT, 0.0);
    EXPECT_TRUE(pool.insert(nullptr, pnl, pos, cb::AlphaMetrics{}).has_value());
  }
  return Zoo{std::move(*st), std::move(pool)};
}

f64 oos_ir(const cb::SignalStore &st, const std::vector<f64> &w, usize begin, usize end) {
  std::vector<f64> c(st.n_instruments());
  std::vector<f64> pnl;
  for (usize t = begin; t < end; ++t) {
    cb::combine_forecast(st, w, t, c);
    f64 gross = 0.0;
    f64 p = 0.0;
    const auto r = st.fwd_row(t);
    for (usize i = 0U; i < c.size(); ++i) {
      gross += std::abs(c[i]);
      p += c[i] * r[i];
    }
    pnl.push_back(gross > 0.0 ? p / gross : 0.0);
  }
  f64 m = 0.0;
  for (const f64 x : pnl) {
    m += x;
  }
  m /= static_cast<f64>(pnl.size());
  f64 v = 0.0;
  for (const f64 x : pnl) {
    v += (x - m) * (x - m);
  }
  return m / std::sqrt(v / static_cast<f64>(pnl.size()));
}

TEST(SignalCombiner, AcceptanceSignalSpaceBeatsPnlShrinkageMvOos) {
  constexpr std::uint64_t kSeeds = 16U;
  f64 ir_fmb = 0.0;
  f64 ir_gk = 0.0;
  f64 ir_mv = 0.0;
  for (std::uint64_t seed = 1U; seed <= kSeeds; ++seed) {
    const Zoo zoo = make_zoo(seed * 1000003U);
    const cb::FitWindow fit{0, 40};
    const auto fmb = cb::FamaMacBethRidge{0.3}.fit(zoo.store, fit);
    const auto gk = cb::GrinoldKahnCombiner{cb::CovTarget::LwIdentity}.fit(zoo.store, fit);
    cb::AlphaCombiner mv;
    mv.cfg.method = cb::CombineMethod::ShrinkageMv;
    const auto mvw = mv.fit(zoo.pnl_pool, 0, 40);
    ASSERT_TRUE(fmb.has_value());
    ASSERT_TRUE(gk.has_value());
    ASSERT_TRUE(mvw.has_value());
    ir_fmb += oos_ir(zoo.store, fmb->w, 40, 240);
    ir_gk += oos_ir(zoo.store, gk->w, 40, 240);
    ir_mv += oos_ir(zoo.store, mvw->weights, 40, 240);
  }
  RecordProperty("ir_fmb", std::to_string(ir_fmb / static_cast<f64>(kSeeds)));
  RecordProperty("ir_gk", std::to_string(ir_gk / static_cast<f64>(kSeeds)));
  RecordProperty("ir_mv", std::to_string(ir_mv / static_cast<f64>(kSeeds)));
  std::printf("[zoo OOS IR/day] FMB=%.4f GK=%.4f PnL-ShrinkageMv=%.4f\n", ir_fmb / static_cast<f64>(kSeeds),
              ir_gk / static_cast<f64>(kSeeds), ir_mv / static_cast<f64>(kSeeds));
  EXPECT_GT(ir_fmb, ir_mv);
  EXPECT_GT(ir_gk, 0.9 * ir_mv); // GK ≈ MV (same estimator in IC units)
}

} // namespace atx_test_l5_combine_signal_combiner
