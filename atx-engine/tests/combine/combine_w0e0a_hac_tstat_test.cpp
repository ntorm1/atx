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

} // namespace atx_test_w0_e0a_hac_tstat
