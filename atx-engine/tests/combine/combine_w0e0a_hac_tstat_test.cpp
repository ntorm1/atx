// atx-engine/tests/combine/combine_w0e0a_hac_tstat_test.cpp
//
// W0-E0a at the combine sites:
//   E-03  HAC t-statistics in column_tstats (GK / FMB / Kakushadze), ICIR-EWMA and
//         marginal_ic — the IID t on an overlapping MA(h-1) IC series is inflated ~sqrt(h);
//   E-15  SignalStore winsorizes after standardizing (values stay within ±winsor) and the
//         Pearson IC of ic_matrix uses winsorized returns.
// Suites: CombineHacTstat, CombineHacTstatStoreWinsor. Synthetic data only.

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <span>
#include <vector>

#include "atx/core/linalg/linalg.hpp"
#include "atx/core/random.hpp"
#include "atx/core/types.hpp"
#include "atx/engine/combine/orthogonalize.hpp"
#include "atx/engine/combine/signal_combiner.hpp"
#include "atx/engine/combine/signal_store.hpp"
#include "atx/engine/eval/hac.hpp"

namespace atx_test_w0_e0a_hac_tstat {

namespace cb = atx::engine::combine;
namespace hac = atx::engine::eval::hac;
using atx::core::linalg::MatX;

constexpr std::size_t kH = 21U; // forward-return horizon -> MA(20) IC overlap

// An equal-weight MA(20) series of length n with the given mean.
[[nodiscard]] std::vector<double> ma20(atx::core::Xoshiro256pp &rng, std::size_t n, double mean,
                                       double scale) {
  std::vector<double> eps(n + kH - 1U);
  for (double &e : eps) {
    e = rng.normal();
  }
  std::vector<double> out(n);
  for (std::size_t t = 0U; t < n; ++t) {
    double w = 0.0;
    for (std::size_t j = 0U; j < kH; ++j) {
      w += eps[t + j];
    }
    out[t] = mean + scale * w;
  }
  return out;
}

// A store whose per-date IC is overlapping: a static cross-sectional signal z_i and daily
// returns r(u, i) = c_u z_i + noise, with the forward return the h-day sum from u = t + 1.
[[nodiscard]] cb::SignalStore overlapping_store(std::size_t t_n, std::size_t n, double edge,
                                                atx::u64 seed, std::vector<double> *z_out) {
  auto st = cb::SignalStore::create(t_n, n);
  EXPECT_TRUE(st.has_value());
  atx::core::Xoshiro256pp rng{seed};
  std::vector<double> z(n);
  for (double &v : z) {
    v = rng.normal();
  }
  std::vector<double> panel(t_n * n);
  for (std::size_t t = 0U; t < t_n; ++t) {
    for (std::size_t i = 0U; i < n; ++i) {
      panel[t * n + i] = z[i];
    }
  }
  EXPECT_TRUE(st->add_signal(panel).has_value());
  const std::size_t days = t_n + kH;
  std::vector<double> daily(days * n);
  for (std::size_t u = 0U; u < days; ++u) {
    const double c = edge + 0.2 * rng.normal();
    for (std::size_t i = 0U; i < n; ++i) {
      daily[u * n + i] = 0.01 * (c * z[i] + rng.normal());
    }
  }
  std::vector<double> fwd(t_n * n, 0.0);
  for (std::size_t t = 0U; t < t_n; ++t) {
    for (std::size_t i = 0U; i < n; ++i) {
      double acc = 0.0;
      for (std::size_t u = t + 1U; u <= t + kH; ++u) {
        acc += daily[u * n + i];
      }
      fwd[t * n + i] = acc;
    }
  }
  EXPECT_TRUE(st->set_forward_returns(fwd).has_value());
  if (z_out != nullptr) {
    *z_out = z;
  }
  return std::move(*st);
}

// ===========================================================================
//  E-03 — column_tstats (Grinold-Kahn, Kakushadze, Fama-MacBeth share it).
// ===========================================================================
TEST(CombineHacTstat, ColumnTstats_Ma20NullRejectionFallsFromIidToNearNominal) {
  constexpr std::size_t kReps = 300U;
  constexpr std::size_t kT = 500U;
  atx::core::Xoshiro256pp rng{0xC01ULL};
  std::size_t reject_hac = 0U;
  std::size_t reject_iid = 0U;
  std::size_t total = 0U;
  for (std::size_t rep = 0U; rep < kReps; ++rep) {
    const std::vector<double> a = ma20(rng, kT, 0.0, 0.01);
    const std::vector<double> b = ma20(rng, kT, 0.0, 0.01);
    MatX ic(static_cast<Eigen::Index>(kT), 2);
    for (std::size_t t = 0U; t < kT; ++t) {
      ic(static_cast<Eigen::Index>(t), 0) = a[t];
      ic(static_cast<Eigen::Index>(t), 1) = b[t];
    }
    const auto r = cb::grinold_kahn_weights(ic, cb::CovTarget::Sample);
    ASSERT_TRUE(r.has_value());
    for (std::size_t c = 0U; c < 2U; ++c) {
      const std::vector<double> &col = (c == 0U) ? a : b;
      const hac::MeanInference want = hac::mean_tstat(col, hac::kDefaultTStatRule);
      ASSERT_EQ(want.defined, 1U);
      EXPECT_EQ(r->tstat[c], want.t); // the site IS the versioned kernel on the column
      const double iid = hac::mean_tstat(col, hac::TStatRule::IidV1).t;
      reject_hac += (std::fabs(r->tstat[c]) > 1.96) ? 1U : 0U;
      reject_iid += (std::fabs(iid) > 1.96) ? 1U : 0U;
      ++total;
    }
  }
  const double rate_hac = static_cast<double>(reject_hac) / static_cast<double>(total);
  const double rate_iid = static_cast<double>(reject_iid) / static_cast<double>(total);
  std::printf("[CombineHacTstat] MA(20) null, T=%zu, %zu columns: |t|>1.96 rate NW-auto %.4f, "
              "IID %.4f (nominal 0.05)\n",
              kT, total, rate_hac, rate_iid);
  EXPECT_GT(rate_iid, 0.50);  // E-03: the IID t rejects a true null most of the time
  EXPECT_LT(rate_hac, 0.15);  // HAC is close to nominal
}

TEST(CombineHacTstat, KakushadzeAndFamaMacBethTstats_AreHacOfTheirColumns) {
  const cb::SignalStore st = overlapping_store(120U, 40U, 0.05, 17U, nullptr);
  const cb::FitWindow win{0U, 120U};
  // Kakushadze: column_tstats over the (complete) alpha-return matrix.
  const auto ky = cb::KakushadzeRegression{}.fit(st, win);
  ASSERT_TRUE(ky.has_value()) << ky.error().message();
  const std::vector<double> ar = cb::alpha_return_matrix(st, win);
  const hac::MeanInference want = hac::mean_tstat(ar, hac::kDefaultTStatRule);
  ASSERT_EQ(want.defined, 1U);
  EXPECT_EQ(ky->tstat[0], want.t);
  const double iid = hac::mean_tstat(ar, hac::TStatRule::IidV1).t;
  std::printf("[CombineHacTstat] Kakushadze alpha-return t: NW-auto %.3f vs IID %.3f\n",
              ky->tstat[0], iid);
  EXPECT_LT(std::fabs(ky->tstat[0]), std::fabs(iid));
  // Fama-MacBeth: one signal, the per-date beta series.
  const auto fmb = cb::FamaMacBethRidge{}.fit(st, win);
  ASSERT_TRUE(fmb.has_value()) << fmb.error().message();
  ASSERT_EQ(fmb->tstat.size(), 1U);
  EXPECT_TRUE(std::isfinite(fmb->tstat[0]));
}

// ===========================================================================
//  E-03 — ICIR-EWMA.
// ===========================================================================
TEST(CombineHacTstat, IcirEwma_TstatIsIidOverSqrtVifAndFarBelowIid) {
  constexpr std::size_t kT = 400U;
  const cb::SignalStore st = overlapping_store(kT, 80U, 0.02, 23U, nullptr);
  const cb::FitWindow win{0U, kT};
  const cb::IcirEwmaCombiner comb{0.0, 2.0}; // equal row weights
  const auto r = comb.fit(st, win);
  ASSERT_TRUE(r.has_value()) << r.error().message();
  // Rebuild the site's numbers from the same IC matrix.
  const std::vector<double> ic = cb::ic_matrix(st, win);
  std::vector<double> xs;
  for (const double v : ic) {
    if (std::isfinite(v)) {
      xs.push_back(v);
    }
  }
  ASSERT_EQ(xs.size(), kT);
  const std::vector<double> ws(xs.size(), 1.0);
  double sw = 0.0;
  double sw2 = 0.0;
  double swx = 0.0;
  for (std::size_t k = 0U; k < xs.size(); ++k) {
    sw += ws[k];
    sw2 += ws[k] * ws[k];
    swx += ws[k] * xs[k];
  }
  const double mean = swx / sw;
  double swv = 0.0;
  for (std::size_t k = 0U; k < xs.size(); ++k) {
    swv += ws[k] * (xs[k] - mean) * (xs[k] - mean);
  }
  const double n_eff = (sw * sw) / sw2;
  const double icir = mean / std::sqrt(std::max(swv / sw, 1e-24));
  const double t_iid = icir * std::sqrt(n_eff); // the pre-W0 site value (TStatRule::IidV1)
  const double vif = hac::ewma_variance_inflation(xs, ws, mean, hac::kDefaultTStatRule);
  EXPECT_EQ(hac::ewma_variance_inflation(xs, ws, mean, hac::TStatRule::IidV1), 1.0);
  EXPECT_NEAR(r->tstat[0], icir * std::sqrt(n_eff / vif), 1e-12 * std::fabs(t_iid));
  std::printf("[CombineHacTstat] ICIR-EWMA on an h=21 overlapping IC: t IID %.3f -> HAC %.3f "
              "(VIF %.2f)\n",
              t_iid, r->tstat[0], vif);
  EXPECT_GT(vif, 4.0);
  EXPECT_LT(std::fabs(r->tstat[0]), 0.5 * std::fabs(t_iid));
}

// ===========================================================================
//  E-03 — marginal_ic.
// ===========================================================================
TEST(CombineHacTstat, MarginalIc_TstatIsHacOfTheMarginalIcSeries) {
  constexpr std::size_t kT = 300U;
  constexpr std::size_t kN = 60U;
  std::vector<double> z;
  const cb::SignalStore st = overlapping_store(kT, kN, 0.03, 29U, &z);
  const std::span<const double> cand = st.signal(0U);
  const std::span<const double> fwd = st.forward_returns();
  const cb::PanelView cv{cand, kT, kN};
  const cb::PanelView fv{fwd, kT, kN};
  const auto mic = cb::marginal_ic(cv, {}, fv);
  ASSERT_TRUE(mic.has_value()) << mic.error().message();
  ASSERT_EQ(mic->n_dates, kT);
  // Rebuild the per-date series: with an empty pool the residual is the demeaned signal.
  std::vector<double> ics;
  std::vector<double> resid(kN);
  for (std::size_t t = 0U; t < kT; ++t) {
    double m = 0.0;
    for (std::size_t i = 0U; i < kN; ++i) {
      m += cand[t * kN + i];
    }
    m /= static_cast<double>(kN);
    for (std::size_t i = 0U; i < kN; ++i) {
      resid[i] = cand[t * kN + i] - m;
    }
    ics.push_back(cb::cross_section_corr(resid, fwd.subspan(t * kN, kN)));
  }
  const hac::MeanInference hac_t = hac::mean_tstat(ics, hac::kDefaultTStatRule);
  const hac::MeanInference iid_t = hac::mean_tstat(ics, hac::TStatRule::IidV1);
  std::printf("[CombineHacTstat] marginal_ic t: site %.4f, NW-auto on rebuilt series %.4f, "
              "IID %.4f\n",
              mic->tstat, hac_t.t, iid_t.t);
  // Rounding in the COD residual differs from the direct demeaning at the 1e-15 level, so
  // the rebuilt series matches to tolerance rather than bit for bit.
  EXPECT_NEAR(mic->tstat, hac_t.t, 1e-8 * std::fabs(hac_t.t));
  EXPECT_LT(std::fabs(mic->tstat), 0.5 * std::fabs(iid_t.t));
  // Contract unchanged: fewer than two usable dates -> 0.
  const auto one = cb::marginal_ic(cv, {}, fv, 5U, 6U);
  ASSERT_TRUE(one.has_value());
  EXPECT_EQ(one->tstat, 0.0);
}

// ===========================================================================
//  E-15 — winsorize after standardizing; Pearson IC on winsorized returns.
// ===========================================================================

// The pre-W0 zscore_row, restated verbatim (z, clip, re-standardize).
void pre_w0_zscore_row(std::span<double> row, double winsor) {
  const auto moments = [&row]() {
    double sum = 0.0;
    std::size_t n = 0U;
    for (const double x : row) {
      if (std::isfinite(x)) {
        sum += x;
        ++n;
      }
    }
    const double mean = (n == 0U) ? 0.0 : sum / static_cast<double>(n);
    double ss = 0.0;
    for (const double x : row) {
      if (std::isfinite(x)) {
        ss += (x - mean) * (x - mean);
      }
    }
    const double sd = (n < 2U) ? 0.0 : std::sqrt(ss / static_cast<double>(n));
    return std::pair<double, double>{mean, sd};
  };
  for (int pass = 0; pass < 2; ++pass) {
    const auto [mean, sd] = moments();
    for (double &x : row) {
      if (!std::isfinite(x)) {
        x = cb::kSignalNaN;
        continue;
      }
      if (sd <= 0.0) {
        x = 0.0;
        continue;
      }
      const double z = (x - mean) / sd;
      x = (pass == 0) ? std::clamp(z, -winsor, winsor) : z;
    }
  }
}

TEST(CombineHacTstatStoreWinsor, ZScore_StaysWithinTheLimit_V1ReproducesTheOldRow) {
  constexpr std::size_t kN = 20U;
  std::vector<double> raw(kN, 0.0);
  raw[3] = 1000.0; // z = sqrt(18) = 4.24 over the 19 finite cells, before clipping
  raw[7] = std::nan("");
  auto st = cb::SignalStore::create(1U, kN);
  ASSERT_TRUE(st.has_value());
  ASSERT_TRUE(st->add_signal(raw).has_value());                                     // default
  ASSERT_TRUE(st->add_signal(raw, cb::SignalNormalize::ZScoreRestandardizeV1).has_value());
  const std::span<const double> v2 = st->signal_row(0U, 0U);
  const std::span<const double> v1 = st->signal_row(1U, 0U);
  double v2_max = 0.0;
  double v1_max = 0.0;
  for (std::size_t i = 0U; i < kN; ++i) {
    if (std::isfinite(v2[i])) {
      v2_max = std::max(v2_max, std::fabs(v2[i]));
      v1_max = std::max(v1_max, std::fabs(v1[i]));
    }
  }
  std::printf("[CombineHacTstatStoreWinsor] winsor 3: max |z| ZScore %.4f, "
              "ZScoreRestandardizeV1 %.4f\n",
              v2_max, v1_max);
  EXPECT_LE(v2_max, 3.0);
  EXPECT_EQ(v2[3], 3.0);  // the outlier sits exactly on the limit
  EXPECT_GT(v1_max, 3.0); // E-15: the old re-standardization broke the limit
  EXPECT_TRUE(std::isnan(v2[7]));
  // V1 is the pre-W0 row, bit for bit.
  std::vector<double> old = raw;
  pre_w0_zscore_row(old, 3.0);
  for (std::size_t i = 0U; i < kN; ++i) {
    if (std::isfinite(old[i])) {
      EXPECT_EQ(v1[i], old[i]) << i;
    } else {
      EXPECT_TRUE(std::isnan(v1[i])) << i;
    }
  }
  // A row that needs no clipping is unchanged by the fix: mean 0, unit sd.
  const std::vector<double> mild{1.0, 2.0, 3.0, 4.0, 5.0};
  auto st2 = cb::SignalStore::create(1U, 5U);
  ASSERT_TRUE(st2.has_value());
  ASSERT_TRUE(st2->add_signal(mild).has_value());
  ASSERT_TRUE(st2->add_signal(mild, cb::SignalNormalize::ZScoreRestandardizeV1).has_value());
  for (std::size_t i = 0U; i < 5U; ++i) {
    EXPECT_NEAR(st2->signal_row(0U, 0U)[i], st2->signal_row(1U, 0U)[i], 1e-15);
  }
}

TEST(CombineHacTstatStoreWinsor, IcMatrix_WinsorizedReturnsTameAnOutlier_RawV1Reproduces) {
  constexpr std::size_t kN = 400U;
  atx::core::Xoshiro256pp rng{0x5EED'15ULL};
  std::vector<double> sig(kN);
  std::vector<double> fwd(kN);
  for (std::size_t i = 0U; i < kN; ++i) {
    sig[i] = rng.normal();
    fwd[i] = 0.5 * sig[i] + rng.normal();
  }
  std::vector<double> dirty = fwd;
  // One extreme return on a strongly negative-signal name dominates the raw Pearson IC.
  std::size_t worst = 0U;
  for (std::size_t i = 1U; i < kN; ++i) {
    worst = (sig[i] < sig[worst]) ? i : worst;
  }
  dirty[worst] = 200.0;
  auto st = cb::SignalStore::create(1U, kN);
  ASSERT_TRUE(st.has_value());
  ASSERT_TRUE(st->add_signal(sig).has_value());
  ASSERT_TRUE(st->set_forward_returns(dirty).has_value());
  const cb::FitWindow win{0U, 1U};
  const double clean = cb::cross_section_corr(st->signal_row(0U, 0U), fwd);
  const double raw_ic = cb::ic_matrix(st.value(), win, cb::IcReturnTreatment::RawV1)[0];
  const double win_ic = cb::ic_matrix(st.value(), win)[0];
  std::printf("[CombineHacTstatStoreWinsor] IC clean %.4f, raw-with-outlier %.4f, "
              "winsorized %.4f\n",
              clean, raw_ic, win_ic);
  // RawV1 is the pre-W0 IC exactly.
  EXPECT_EQ(raw_ic, cb::cross_section_corr(st->signal_row(0U, 0U), st->fwd_row(0U)));
  EXPECT_GT(clean, 0.3);
  EXPECT_LT(raw_ic, 0.0); // the outlier flips the sign of the raw IC
  EXPECT_GT(win_ic, 0.0); // the clip at mean + 3 sd restores it
  EXPECT_LT(std::fabs(win_ic - clean), std::fabs(raw_ic - clean));
}

TEST(CombineInferenceConfig, PublicCombinersRestoreLegacyInferenceAndRawReturns) {
  constexpr std::size_t kT = 160U;
  const auto store = overlapping_store(kT, 40U, 0.02, 731U, nullptr);
  const cb::FitWindow win{0U, kT};
  const cb::SignalInferenceConfig legacy{hac::TStatRule::IidV1, 21U,
                                          cb::IcReturnTreatment::RawV1};
  const auto raw_ic = cb::ic_matrix(store, win, cb::IcReturnTreatment::RawV1);
  double mean = 0.0;
  for (double v : raw_ic) {
    mean += v;
  }
  mean /= static_cast<double>(kT);
  double ss = 0.0;
  for (double v : raw_ic) {
    ss += (v - mean) * (v - mean);
  }
  const double legacy_t = mean / (std::sqrt(ss / static_cast<double>(kT - 1U)) /
                                   std::sqrt(static_cast<double>(kT)));
  cb::GrinoldKahnCombiner gk;
  gk.inference = legacy;
  const auto gr = gk.fit(store, win);
  ASSERT_TRUE(gr.has_value());
  EXPECT_EQ(gr->tstat[0], legacy_t);

  cb::IcirEwmaCombiner ewma{0.0, 0.0};
  ewma.inference = legacy;
  const auto er = ewma.fit(store, win);
  ASSERT_TRUE(er.has_value());
  // The historical EWMA estimator uses the population weighted variance.
  EXPECT_NEAR(er->tstat[0], mean / std::sqrt(ss / static_cast<double>(kT)) *
                               std::sqrt(static_cast<double>(kT)), 1e-12);

  cb::KakushadzeRegression ky;
  ky.inference = legacy;
  const auto kr = ky.fit(store, win);
  ASSERT_TRUE(kr.has_value());
  const auto ar = cb::alpha_return_matrix(store, win);
  EXPECT_EQ(kr->tstat[0], hac::mean_tstat(ar, hac::TStatRule::IidV1).t);
  cb::FamaMacBethRidge fmb;
  fmb.inference = legacy;
  const auto fr = fmb.fit(store, win);
  ASSERT_TRUE(fr.has_value());
  EXPECT_NEAR(fr->tstat[0], kr->tstat[0], 1e-11);

  const auto mr = cb::marginal_ic({store.signal(0U), kT, 40U}, {},
      {store.forward_returns(), kT, 40U}, 0U, kT, hac::TStatRule::IidV1, 21U);
  ASSERT_TRUE(mr.has_value());
  EXPECT_NEAR(mr->tstat, legacy_t, 1e-11);
}

TEST(CombineInferenceConfig, DeclaredHorizonCalibratesOverlappingNull) {
  constexpr std::size_t kReps = 2000U;
  constexpr std::size_t kT = 1750U;
  atx::core::Xoshiro256pp rng{0xC01ULL};
  cb::SignalInferenceConfig cfg;
  cfg.label_horizon = kH;
  std::size_t rejected = 0U;
  for (std::size_t rep = 0U; rep < kReps; ++rep) {
    const auto series = ma20(rng, kT, 0.0, 0.01);
    MatX ic(static_cast<Eigen::Index>(kT), 1);
    for (std::size_t t = 0U; t < kT; ++t) {
      ic(static_cast<Eigen::Index>(t), 0) = series[t];
    }
    const auto fit = cb::grinold_kahn_weights(ic, cb::CovTarget::Sample, cfg);
    ASSERT_TRUE(fit.has_value());
    rejected += std::abs(fit->tstat[0]) > 1.96 ? 1U : 0U;
  }
  const double rate = static_cast<double>(rejected) / static_cast<double>(kReps);
  std::printf("[CombineInferenceConfig] MA(20) null, n=%zu, reps=%zu, rejection=%.4f\n",
              kT, kReps, rate);
  EXPECT_GE(rate, 0.03);
  EXPECT_LE(rate, 0.07);
}

TEST(CombineInferenceConfig, HorizonChangesWeightedHaircutAndValidatesBoundary) {
  const auto store = overlapping_store(400U, 80U, 0.02, 23U, nullptr);
  cb::IcirEwmaCombiner daily{0.0, 2.0};
  cb::IcirEwmaCombiner overlap = daily;
  overlap.inference.label_horizon = 21U;
  const auto dr = daily.fit(store, {0U, 400U});
  const auto hr = overlap.fit(store, {0U, 400U});
  ASSERT_TRUE(dr.has_value());
  ASSERT_TRUE(hr.has_value());
  EXPECT_LT(std::abs(hr->tstat[0]), std::abs(dr->tstat[0]));
  overlap.inference.label_horizon = 0U;
  EXPECT_FALSE(overlap.fit(store, {0U, 400U}).has_value());
  overlap.inference.label_horizon = 1U;
  overlap.inference.tstat_rule = hac::TStatRule::Unknown;
  EXPECT_FALSE(overlap.fit(store, {0U, 400U}).has_value());
  overlap.inference.tstat_rule = hac::TStatRule::HorizonAwareV3;
  overlap.inference.return_treatment = static_cast<cb::IcReturnTreatment>(255U);
  EXPECT_FALSE(overlap.fit(store, {0U, 400U}).has_value());
  EXPECT_FALSE(cb::marginal_ic({store.signal(0U), 400U, 80U}, {},
      {store.forward_returns(), 400U, 80U}, 0U, 400U,
      hac::TStatRule::HorizonAwareV3, 0U).has_value());
}

TEST(CombineInferenceConfig, UnsupportedOverlapDoesNotCreateInfiniteSignificance) {
  const std::array<double, 5> x{.037921444273233657, .0333917854612267,
      .032102668894305145, .03600658924381094, .039304876836250696};
  EXPECT_EQ(hac::mean_tstat(x, hac::TStatRule::HorizonAwareV3, 5U).defined, 0U);
  EXPECT_EQ(hac::mean_tstat(x, hac::TStatRule::HorizonAwareV3, 99U).defined, 0U);
  const auto guarded = hac::mean_inference(x, hac::Kernel::UniformV1, 4U, true, true);
  EXPECT_EQ(guarded.defined, 1U);
  EXPECT_EQ(guarded.fell_back, 1U);
  EXPECT_EQ(guarded.kernel, hac::Kernel::BartlettV1);
}

TEST(CombineInferenceConfig, UnequalDecayWeightsUseDirectSandwichVariance) {
  constexpr std::size_t kT = 300U;
  constexpr double kHalfLife = 17.0;
  const auto store = overlapping_store(kT, 80U, 0.02, 517U, nullptr);
  cb::IcirEwmaCombiner comb{kHalfLife, 0.0};
  comb.inference.label_horizon = 21U;
  const auto fit = comb.fit(store, {0U, kT});
  ASSERT_TRUE(fit.has_value());
  const auto ic = cb::ic_matrix(store, {0U, kT});
  std::vector<double> w(kT);
  double sw = 0.0;
  double mean = 0.0;
  for (std::size_t t = 0U; t < kT; ++t) {
    w[t] = std::pow(0.5, static_cast<double>(kT - 1U - t) / kHalfLife);
    sw += w[t];
    mean += w[t] * ic[t];
  }
  mean /= sw;
  double s0 = 0.0;
  for (std::size_t t = 0U; t < kT; ++t) {
    const double u = w[t] * (ic[t] - mean);
    s0 += u * u;
  }
  const std::size_t lag = std::max(std::size_t{20U}, hac::newey_west_auto_lag(ic));
  double sandwich = s0;
  double bartlett = s0;
  for (std::size_t j = 1U; j <= lag; ++j) {
    double pair_sum = 0.0;
    for (std::size_t t = j; t < kT; ++t) {
      pair_sum += w[t] * (ic[t] - mean) * w[t-j] * (ic[t-j] - mean);
    }
    sandwich += 2.0 * pair_sum;
    bartlett += 2.0 * (1.0 - static_cast<double>(j) / static_cast<double>(lag+1U)) * pair_sum;
  }
  if (!(sandwich > 0.0)) sandwich = bartlett;
  ASSERT_GT(sandwich, 0.0);
  EXPECT_NEAR(fit->tstat[0], mean * sw / std::sqrt(sandwich), 1e-11);
}

TEST(CombineInferenceConfig, LegacyRawTwoAlphaWeightsMatchClosedForm) {
  constexpr std::size_t kT = 160U;
  constexpr std::size_t kN = 40U;
  auto store = overlapping_store(kT, kN, 0.02, 731U, nullptr);
  atx::core::Xoshiro256pp rng{47U};
  std::vector<double> second(kT * kN);
  for (std::size_t i = 0U; i < second.size(); ++i) {
    second[i] = 0.3 * store.signal(0U)[i] + rng.normal();
  }
  ASSERT_TRUE(store.add_signal(second).has_value());
  cb::GrinoldKahnCombiner comb;
  comb.target = cb::CovTarget::Sample;
  comb.inference = {hac::TStatRule::IidV1, 21U, cb::IcReturnTreatment::RawV1};
  const auto fit = comb.fit(store, {0U, kT});
  ASSERT_TRUE(fit.has_value());
  const auto ic = cb::ic_matrix(store, {0U, kT}, cb::IcReturnTreatment::RawV1);
  double m0 = 0.0, m1 = 0.0;
  for (std::size_t t = 0U; t < kT; ++t) {
    m0 += ic[2U*t] / static_cast<double>(kT);
    m1 += ic[2U*t+1U] / static_cast<double>(kT);
  }
  double c00 = 0.0, c01 = 0.0, c11 = 0.0;
  for (std::size_t t = 0U; t < kT; ++t) {
    const double a = ic[2U*t] - m0;
    const double b = ic[2U*t+1U] - m1;
    c00 += a*a; c01 += a*b; c11 += b*b;
  }
  const double b0 = c11*m0 - c01*m1;
  const double b1 = c00*m1 - c01*m0;
  const double gross = std::abs(b0) + std::abs(b1);
  ASSERT_GT(gross, 0.0);
  EXPECT_NEAR(fit->w[0], b0/gross, 1e-11);
  EXPECT_NEAR(fit->w[1], b1/gross, 1e-11);
  EXPECT_GT(std::abs(fit->w[1]), 1e-4);
}

} // namespace atx_test_w0_e0a_hac_tstat
