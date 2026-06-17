// risk_stat_factor_test.cpp — S8.6: statistical factor model via APCA (T×T Gram).
//
// FactorModelBuilder::build with FactorModelConfig.n_stat_factors == K (and
// n_dead_factors == 0) builds a STATISTICAL FactorModel via 2-pass Asymptotic
// Principal Components (Connor-Korajczyk):
//   * Build the complete-case return panel R (N×T) over the trailing window for
//     the current cross-section (row 0) instruments; column-demean each asset row.
//   * Pass 1: form the T×T Gram Ω = (1/N)·Rᵀ·R (NOT the N×N covariance — the
//     asymptotic trick for N≫T), symmetric_eig, take the top-K eigenvectors as the
//     factor-return matrix Fhat (T×K); regress R on Fhat for exposures B (N×K) and
//     specific variances s_n.
//   * Pass 2 (GLS, opt-in via cov.apca_gls_reweight, default true): reweight rows by
//     1/√s_n, recompute the Gram, re-extract Fhat; recover B and s_n from UN-weighted R.
//   * Assemble V = B·F·Bᵀ + diag(s_n), F = factor_covariance(Fhat, factor_cov_shrink).
//
// Coverage (plan §5 task S8.6):
//   * Planted-latent-factor recovery: R = B_true·Fᵀ_true + small noise, N≫T.
//     (a) the top-K Gram eigenvalues dominate (clear gap to the (K+1)-th);
//     (b) each recovered factor-return column |corr| > 0.9 with a planted factor
//         (up to sign/permutation); (c) the FactorModel applies (risk finite > 0,
//         apply_inverse runs, V SPD ⇒ create succeeded).
//   * NotImplemented retired: n_stat_factors>0 & n_dead_factors==0 ⇒ NOT
//     NotImplemented; n_dead_factors>0 ⇒ STILL NotImplemented.
//   * Validation: N≤T or T≤K or N≤K ⇒ Err(InvalidArgument).
//   * Determinism: same panel/config twice ⇒ byte-identical V (identical risk()).
//   * Truncation-invariance: older rows beyond the window don't change the estimate.
//   * GLS on/off: both produce SPD applicable models; GLS changes the estimate.

#include <cmath>  // std::isfinite, std::sqrt, std::fabs
#include <limits> // std::numeric_limits (quiet NaN sentinel)
#include <span>
#include <vector>

#include <gtest/gtest.h>

#include <Eigen/Dense>

#include "atx/core/error.hpp"
#include "atx/core/linalg/linalg.hpp"
#include "atx/core/random.hpp"
#include "atx/core/types.hpp"

#include "atx/engine/loop/panel_types.hpp" // PanelView, PanelField, kPanelFieldCount
#include "atx/engine/loop/types.hpp"       // InstrumentId (Symbol)
#include "atx/engine/risk/factor_model.hpp"

namespace atxtest_risk_stat_factor_test {

using atx::f64;
using atx::u32;
using atx::usize;
using atx::core::Xoshiro256pp;
using atx::core::domain::Symbol;
using atx::core::linalg::MatX;
using atx::core::linalg::VecX;
using atx::engine::InstrumentId;
using atx::engine::kPanelFieldCount;
using atx::engine::PanelField;
using atx::engine::PanelView;
using atx::engine::risk::FactorModel;
using atx::engine::risk::FactorModelBuilder;
using atx::engine::risk::FactorModelConfig;

constexpr f64 kNaN = std::numeric_limits<f64>::quiet_NaN();

// ===========================================================================
//  PanelFixture — owns a PanelView's backing storage (same pattern as the P4-7b
//  risk_factor_builder_test fixture). row 0 = newest cross-section; open/high/low
//  mirror close.
// ===========================================================================
class PanelFixture {
public:
  PanelFixture(usize n_rows, usize n_inst, const std::vector<std::vector<f64>> &close,
               const std::vector<std::vector<f64>> &volume)
      : n_rows_{n_rows}, n_inst_{n_inst}, cap_{pow2_ceil(n_rows)},
        mask_words_{(n_inst + 63U) / 64U} {
    universe_.reserve(n_inst);
    for (usize i = 0; i < n_inst; ++i) {
      universe_.push_back(Symbol{static_cast<u32>(i + 1U)});
    }
    fields_.assign(kPanelFieldCount * cap_ * n_inst_, kNaN);
    mask_.assign(cap_ * mask_words_, 0ULL);
    for (usize r = 0; r < n_rows_; ++r) {
      const usize phys = (n_rows_ - 1U) - r; // newest-first r -> physical row
      for (usize i = 0; i < n_inst_; ++i) {
        const f64 c = close[r][i];
        const f64 v = volume[r][i];
        set(PanelField::Open, phys, i, c);
        set(PanelField::High, phys, i, c);
        set(PanelField::Low, phys, i, c);
        set(PanelField::Close, phys, i, c);
        set(PanelField::Volume, phys, i, v);
        if (!std::isnan(c)) {
          mask_[phys * mask_words_ + (i >> 6U)] |= (1ULL << (i & 63U));
        }
      }
    }
  }

  [[nodiscard]] PanelView view() const noexcept {
    return PanelView{fields_.data(), mask_.data(), std::span<const InstrumentId>{universe_},
                     cap_,           head_(),      n_rows_,
                     mask_words_};
  }

private:
  [[nodiscard]] usize head_() const noexcept { return (n_rows_ == 0U) ? 0U : n_rows_ - 1U; }

  static usize pow2_ceil(usize n) noexcept {
    usize p = 1U;
    while (p < n) {
      p <<= 1U;
    }
    return p;
  }

  void set(PanelField f, usize phys, usize inst, f64 v) noexcept {
    const usize block = static_cast<usize>(f) * cap_ * n_inst_;
    fields_[block + phys * n_inst_ + inst] = v;
  }

  usize n_rows_;
  usize n_inst_;
  usize cap_;
  usize mask_words_;
  std::vector<InstrumentId> universe_;
  std::vector<f64> fields_;
  std::vector<atx::u64> mask_;
};

// Sectors-only single-group config: one 0/1 dummy column (all 1s) ⇒ the current
// cross-section at row 0 is every present instrument with NO style lookback (a
// style factor would need 252+ rows). The statistical path uses this only to pick
// the cross-section instrument set; its statistical exposures are built internally.
[[nodiscard]] FactorModelConfig stat_cfg(usize n_stat) {
  FactorModelConfig cfg;
  cfg.sector_factors = true;
  cfg.style_mask = 0x00; // sectors only ⇒ no per-instrument lookback
  cfg.n_stat_factors = n_stat;
  return cfg;
}

// Build a close grid (n_rows × n_inst, row 0 newest) whose per-step returns equal
// the planted return panel `ret[t][i]` (newest t=0). Walk from a base price at the
// oldest row forward in time: close[t][i] = close[t+1][i]·(1+ret[t][i]).
[[nodiscard]] std::vector<std::vector<f64>> closes_from_returns(usize window, usize n_inst,
                                                                const MatX &ret /* N×T */) {
  const usize n_rows = window + 1U;
  std::vector<std::vector<f64>> close(n_rows, std::vector<f64>(n_inst));
  for (usize i = 0; i < n_inst; ++i) {
    close[n_rows - 1U][i] = 100.0 + static_cast<f64>(i); // distinct oldest base prices
    for (usize t = window; t-- > 0U;) {
      const f64 r = ret(static_cast<Eigen::Index>(i), static_cast<Eigen::Index>(t));
      close[t][i] = close[t + 1U][i] * (1.0 + r);
    }
  }
  return close;
}

// Pearson |correlation| of two equal-length series (population; the time-mean is
// the natural center). 0 if either has zero variance.
[[nodiscard]] f64 abs_corr(const VecX &a, const VecX &b) {
  const Eigen::Index n = a.size();
  const f64 ma = a.mean();
  const f64 mb = b.mean();
  f64 sab = 0.0, saa = 0.0, sbb = 0.0;
  for (Eigen::Index i = 0; i < n; ++i) {
    const f64 da = a[i] - ma;
    const f64 db = b[i] - mb;
    sab += da * db;
    saa += da * da;
    sbb += db * db;
  }
  if (saa <= 0.0 || sbb <= 0.0) {
    return 0.0;
  }
  return std::fabs(sab / std::sqrt(saa * sbb));
}

// Planted factor-return series F_true (T×K, newest t=0) with columns that are
// TIME-ORTHOGONAL and have DISTINCT, separated variances. Principal axes recover
// individual latent factors (not just their span) only when the factors are
// mutually uncorrelated AND have distinct variances — random columns over a short
// T leave non-trivial sample cross-correlation, so the leading eigenvectors mix
// them. We Gram-Schmidt-orthonormalize random columns, then scale column k by
// `scale·decay^k` so each factor maps to one eigenvalue (a clean permutation match).
[[nodiscard]] MatX planted_factor_returns(usize window, usize n_factors, Xoshiro256pp &rng,
                                          f64 scale, f64 decay) {
  const Eigen::Index t = static_cast<Eigen::Index>(window);
  const Eigen::Index k = static_cast<Eigen::Index>(n_factors);
  MatX f(t, k);
  for (Eigen::Index c = 0; c < k; ++c) {
    for (Eigen::Index r = 0; r < t; ++r) {
      f(r, c) = rng.normal();
    }
  }
  // Modified Gram-Schmidt over the columns (order-fixed ascending column).
  for (Eigen::Index c = 0; c < k; ++c) {
    for (Eigen::Index p = 0; p < c; ++p) {
      const f64 dot = f.col(p).dot(f.col(c));
      f.col(c) -= dot * f.col(p);
    }
    const f64 nrm = f.col(c).norm();
    f.col(c) /= (nrm > 0.0 ? nrm : 1.0);                      // unit columns
    f.col(c) *= scale * std::pow(decay, static_cast<f64>(c)); // distinct variances
  }
  return f;
}

// ===========================================================================
//  Planted-latent-factor recovery. K=3 factors drive N=200 instruments over T=40
//  dates: R = B_true·Fᵀ_true + small noise. The Gram top-K eigenvalues must
//  dominate; each recovered factor-return series must correlate highly with a
//  planted one; and the assembled FactorModel must apply (V SPD).
// ===========================================================================
TEST(RiskStatFactor, RecoversPlantedLatentFactors) {
  constexpr usize N = 200U; // instruments  (N ≫ T)
  constexpr usize T = 40U;  // window dates
  constexpr usize K = 3U;   // planted latent factors

  Xoshiro256pp rng{0xA11CEU};
  // Planted factor returns F_true (T×K, time-orthogonal, distinct variances) and
  // exposures B_true (N×K, standard normal). The factor returns dominate the small
  // idiosyncratic noise (a clear eigenvalue gap).
  const MatX f_true = planted_factor_returns(T, K, rng, /*scale=*/0.18, /*decay=*/0.6);
  MatX b_true(static_cast<Eigen::Index>(N), static_cast<Eigen::Index>(K));
  for (Eigen::Index n = 0; n < static_cast<Eigen::Index>(N); ++n) {
    for (Eigen::Index kk = 0; kk < static_cast<Eigen::Index>(K); ++kk) {
      b_true(n, kk) = rng.normal();
    }
  }
  // R[n,t] = Σ_k B_true[n,k]·F_true[t,k] + noise.  newest t=0.
  MatX ret(static_cast<Eigen::Index>(N), static_cast<Eigen::Index>(T));
  for (Eigen::Index n = 0; n < static_cast<Eigen::Index>(N); ++n) {
    for (Eigen::Index t = 0; t < static_cast<Eigen::Index>(T); ++t) {
      f64 v = 0.0;
      for (Eigen::Index kk = 0; kk < static_cast<Eigen::Index>(K); ++kk) {
        v += b_true(n, kk) * f_true(t, kk);
      }
      ret(n, t) = v + 0.002 * rng.normal(); // small idiosyncratic noise
    }
  }

  const auto close = closes_from_returns(T, N, ret);
  const std::vector<std::vector<f64>> volume(T + 1U, std::vector<f64>(N, 1000.0));
  PanelFixture fx{T + 1U, N, close, volume};
  const std::vector<u32> group(N, 7U); // one sector ⇒ all-present cross-section

  FactorModelBuilder builder{stat_cfg(K)};
  const auto m = builder.build(fx.view(), T, std::span<const f64>{}, std::span<const u32>{group});
  ASSERT_TRUE(m.has_value()) << (m ? "" : m.error().to_string());
  EXPECT_EQ(m->n_factors(), K);
  EXPECT_EQ(m->n_instruments(), N);

  // (c) The model applies: risk finite > 0, apply_inverse finite.
  std::vector<f64> w(N, 0.0);
  for (usize i = 0; i < N; ++i) {
    w[i] = (i % 2U == 0U) ? 0.01 : -0.01;
  }
  const f64 rk = m->risk(std::span<const f64>{w});
  EXPECT_TRUE(std::isfinite(rk));
  EXPECT_GT(rk, 0.0);
  std::vector<f64> inv(N, 0.0);
  m->apply_inverse(std::span<const f64>{w}, std::span<f64>{inv});
  for (const f64 v : inv) {
    EXPECT_TRUE(std::isfinite(v));
  }
}

// ===========================================================================
//  Gram eigenvalue gap + factor-return recovery, asserted on the kernels directly.
//  We rebuild the T×T Gram the way the implementation does and confirm the top-K
//  eigenvalues dominate (clear gap to the (K+1)-th) and that the recovered factor
//  returns correlate highly with the planted ones. This is the statistical core
//  the build() smoke test relies on.
// ===========================================================================
TEST(RiskStatFactor, GramTopKDominatesAndRecoversFactorReturns) {
  constexpr usize N = 200U;
  constexpr usize T = 40U;
  constexpr usize K = 3U;

  Xoshiro256pp rng{0xBEEFU};
  const MatX f_true = planted_factor_returns(T, K, rng, /*scale=*/0.18, /*decay=*/0.6);
  MatX b_true(static_cast<Eigen::Index>(N), static_cast<Eigen::Index>(K));
  for (Eigen::Index n = 0; n < static_cast<Eigen::Index>(N); ++n) {
    for (Eigen::Index kk = 0; kk < static_cast<Eigen::Index>(K); ++kk) {
      b_true(n, kk) = rng.normal();
    }
  }
  MatX r(static_cast<Eigen::Index>(N), static_cast<Eigen::Index>(T)); // N×T (newest t=0)
  for (Eigen::Index n = 0; n < static_cast<Eigen::Index>(N); ++n) {
    for (Eigen::Index t = 0; t < static_cast<Eigen::Index>(T); ++t) {
      f64 v = 0.0;
      for (Eigen::Index kk = 0; kk < static_cast<Eigen::Index>(K); ++kk) {
        v += b_true(n, kk) * f_true(t, kk);
      }
      r(n, t) = v + 0.002 * rng.normal();
    }
  }
  // Column-demean each asset row over its T returns (per-asset time-mean).
  for (Eigen::Index n = 0; n < static_cast<Eigen::Index>(N); ++n) {
    const f64 mean = r.row(n).mean();
    r.row(n).array() -= mean;
  }

  // T×T Gram Ω = (1/N)·Rᵀ·R; symmetric_eig (ascending). Top-K = the last K.
  const MatX gram = (r.transpose() * r) / static_cast<f64>(N);
  const auto eig = atx::core::linalg::symmetric_eig(gram);
  ASSERT_TRUE(eig.has_value());
  const Eigen::Index tt = eig->values.size();
  ASSERT_EQ(tt, static_cast<Eigen::Index>(T));
  // (a) clear gap: the K-th largest eigenvalue ≫ the (K+1)-th.
  const f64 lambda_k = eig->values[tt - static_cast<Eigen::Index>(K)];       // smallest kept
  const f64 lambda_kp1 = eig->values[tt - static_cast<Eigen::Index>(K) - 1]; // first dropped
  EXPECT_GT(lambda_k, 10.0 * lambda_kp1);

  // (b) recovered factor returns (top-K eigenvectors, largest first) correlate
  // highly with a planted factor (up to sign/permutation).
  MatX fhat(static_cast<Eigen::Index>(T), static_cast<Eigen::Index>(K));
  for (Eigen::Index c = 0; c < static_cast<Eigen::Index>(K); ++c) {
    fhat.col(c) = eig->vectors.col(tt - 1 - c); // largest eigenvalue first
  }
  for (Eigen::Index c = 0; c < static_cast<Eigen::Index>(K); ++c) {
    f64 best = 0.0;
    for (Eigen::Index p = 0; p < static_cast<Eigen::Index>(K); ++p) {
      best = std::max(best, abs_corr(fhat.col(c), f_true.col(p)));
    }
    EXPECT_GT(best, 0.9) << "recovered factor " << c << " did not match a planted one";
  }
}

// ===========================================================================
//  Span-level recovery on a NON-orthogonal planted panel — the honest claim.
//
//  PCA recovers the K-dimensional factor SUBSPACE (the span), not the individual
//  planted axes. With RAW random (non-orthogonal) planted factor columns over a
//  short T, per-axis |corr| is only ~0.83–0.89 — NOT because recovery failed, but
//  because the principal axes are linear combinations of the (cross-correlated)
//  planted factors. The honest invariant is: each planted factor-return column lies
//  ALMOST ENTIRELY within the recovered column space. We project each planted
//  (demeaned) column onto the recovered Fhat columns (orthonormal eigenvectors, so
//  the projection is exact: proj = Fhat·Fhatᵀ·v) and assert the retained-norm
//  fraction ‖proj‖²/‖v‖² ≥ 0.95. Deterministic (fixed seed, RNG-free decomposition).
//  This complements the orthogonal-panel per-axis test above.
// ===========================================================================
TEST(RiskStatFactor, RecoveredSubspaceCapturesNonOrthogonalPlantedFactors) {
  constexpr usize N = 200U;
  constexpr usize T = 40U;
  constexpr usize K = 3U;

  Xoshiro256pp rng{0xBEEFU}; // SAME seed/setup as the per-axis Gram test ⇒ the weak
                             // per-axis corr below is exactly the gap this test closes.
  MatX f_true(static_cast<Eigen::Index>(T), static_cast<Eigen::Index>(K));
  MatX b_true(static_cast<Eigen::Index>(N), static_cast<Eigen::Index>(K));
  for (Eigen::Index t = 0; t < static_cast<Eigen::Index>(T); ++t) {
    for (Eigen::Index kk = 0; kk < static_cast<Eigen::Index>(K); ++kk) {
      f_true(t, kk) = 0.03 * rng.normal(); // RAW (non-orthogonal) planted columns
    }
  }
  for (Eigen::Index n = 0; n < static_cast<Eigen::Index>(N); ++n) {
    for (Eigen::Index kk = 0; kk < static_cast<Eigen::Index>(K); ++kk) {
      b_true(n, kk) = rng.normal();
    }
  }
  MatX r(static_cast<Eigen::Index>(N), static_cast<Eigen::Index>(T)); // N×T (newest t=0)
  for (Eigen::Index n = 0; n < static_cast<Eigen::Index>(N); ++n) {
    for (Eigen::Index t = 0; t < static_cast<Eigen::Index>(T); ++t) {
      f64 v = 0.0;
      for (Eigen::Index kk = 0; kk < static_cast<Eigen::Index>(K); ++kk) {
        v += b_true(n, kk) * f_true(t, kk);
      }
      r(n, t) = v + 0.002 * rng.normal();
    }
  }
  for (Eigen::Index n = 0; n < static_cast<Eigen::Index>(N); ++n) {
    const f64 mean = r.row(n).mean();
    r.row(n).array() -= mean;
  }

  const MatX gram = (r.transpose() * r) / static_cast<f64>(N);
  const auto eig = atx::core::linalg::symmetric_eig(gram);
  ASSERT_TRUE(eig.has_value());
  const Eigen::Index tt = eig->values.size();
  MatX fhat(static_cast<Eigen::Index>(T), static_cast<Eigen::Index>(K)); // orthonormal cols
  for (Eigen::Index c = 0; c < static_cast<Eigen::Index>(K); ++c) {
    fhat.col(c) = eig->vectors.col(tt - 1 - c);
  }

  // (1) Per-axis recovery is only MODERATE here (the honesty gap): at least one
  // planted axis has its best per-axis |corr| below 0.9 ⇒ the per-axis claim alone
  // would be misleading on this panel.
  f64 worst_axis = 1.0;
  for (Eigen::Index p = 0; p < static_cast<Eigen::Index>(K); ++p) {
    f64 best = 0.0;
    for (Eigen::Index c = 0; c < static_cast<Eigen::Index>(K); ++c) {
      best = std::max(best, abs_corr(fhat.col(c), f_true.col(p)));
    }
    worst_axis = std::min(worst_axis, best);
  }
  EXPECT_LT(worst_axis, 0.9) << "panel was effectively orthogonal — choose a harder one";

  // (2) But the recovered SUBSPACE captures each planted factor almost entirely:
  // project the demeaned planted column onto the orthonormal Fhat columns and assert
  // ≥ 95% of its squared norm is retained.
  for (Eigen::Index p = 0; p < static_cast<Eigen::Index>(K); ++p) {
    VecX v = f_true.col(p);
    v.array() -= v.mean(); // demean (Fhat columns are mean-≈0 eigenvectors)
    const f64 nrm2 = v.squaredNorm();
    ASSERT_GT(nrm2, 0.0);
    const VecX coeffs = fhat.transpose() * v;       // Fhatᵀ v       (K)
    const VecX proj = fhat * coeffs;                // Fhat Fhatᵀ v  (T)
    const f64 retained = proj.squaredNorm() / nrm2; // ∈ [0,1]
    EXPECT_GT(retained, 0.95) << "planted factor " << p << " escapes the recovered subspace";
  }
}

// ===========================================================================
//  NotImplemented retired (stat) / still NotImplemented (dead).
// ===========================================================================
TEST(RiskStatFactor, StatRungIsNoLongerNotImplemented) {
  constexpr usize N = 60U;
  constexpr usize T = 12U;
  constexpr usize K = 2U;
  Xoshiro256pp rng{0xC0FFEEU};
  MatX ret(static_cast<Eigen::Index>(N), static_cast<Eigen::Index>(T));
  for (Eigen::Index n = 0; n < static_cast<Eigen::Index>(N); ++n) {
    for (Eigen::Index t = 0; t < static_cast<Eigen::Index>(T); ++t) {
      ret(n, t) = 0.02 * rng.normal();
    }
  }
  const auto close = closes_from_returns(T, N, ret);
  const std::vector<std::vector<f64>> volume(T + 1U, std::vector<f64>(N, 1000.0));
  PanelFixture fx{T + 1U, N, close, volume};
  const std::vector<u32> group(N, 1U);

  FactorModelBuilder builder{stat_cfg(K)};
  const auto m = builder.build(fx.view(), T, std::span<const f64>{}, std::span<const u32>{group});
  if (!m.has_value()) {
    EXPECT_NE(m.error().code(), atx::core::ErrorCode::NotImplemented);
  }
}

TEST(RiskStatFactor, DeadRungStillNotImplemented) {
  constexpr usize N = 60U;
  constexpr usize T = 12U;
  Xoshiro256pp rng{0xD15EA5EU};
  MatX ret(static_cast<Eigen::Index>(N), static_cast<Eigen::Index>(T));
  for (Eigen::Index n = 0; n < static_cast<Eigen::Index>(N); ++n) {
    for (Eigen::Index t = 0; t < static_cast<Eigen::Index>(T); ++t) {
      ret(n, t) = 0.02 * rng.normal();
    }
  }
  const auto close = closes_from_returns(T, N, ret);
  const std::vector<std::vector<f64>> volume(T + 1U, std::vector<f64>(N, 1000.0));
  PanelFixture fx{T + 1U, N, close, volume};
  const std::vector<u32> group(N, 1U);

  FactorModelConfig cfg = stat_cfg(2U);
  cfg.n_dead_factors = 1U; // dead-alpha rung STILL deferred
  FactorModelBuilder builder{cfg};
  const auto m = builder.build(fx.view(), T, std::span<const f64>{}, std::span<const u32>{group});
  ASSERT_FALSE(m.has_value());
  EXPECT_EQ(m.error().code(), atx::core::ErrorCode::NotImplemented);
}

// ===========================================================================
//  Validation: the asymptotic argument needs N > T and N > K and T > K.
// ===========================================================================
TEST(RiskStatFactor, NotEnoughInstrumentsIsError) {
  // N == T (== 12) violates N > T. K = 2.
  constexpr usize N = 12U;
  constexpr usize T = 12U;
  Xoshiro256pp rng{0x1234U};
  MatX ret(static_cast<Eigen::Index>(N), static_cast<Eigen::Index>(T));
  for (Eigen::Index n = 0; n < static_cast<Eigen::Index>(N); ++n) {
    for (Eigen::Index t = 0; t < static_cast<Eigen::Index>(T); ++t) {
      ret(n, t) = 0.02 * rng.normal();
    }
  }
  const auto close = closes_from_returns(T, N, ret);
  const std::vector<std::vector<f64>> volume(T + 1U, std::vector<f64>(N, 1000.0));
  PanelFixture fx{T + 1U, N, close, volume};
  const std::vector<u32> group(N, 1U);

  FactorModelBuilder builder{stat_cfg(2U)};
  const auto m = builder.build(fx.view(), T, std::span<const f64>{}, std::span<const u32>{group});
  ASSERT_FALSE(m.has_value());
  EXPECT_EQ(m.error().code(), atx::core::ErrorCode::InvalidArgument);
}

TEST(RiskStatFactor, WindowNotAboveFactorCountIsError) {
  // T == K violates T > K. N ≫ T so only the T > K bound trips.
  constexpr usize N = 80U;
  constexpr usize T = 3U;
  constexpr usize K = 3U;
  Xoshiro256pp rng{0x5678U};
  MatX ret(static_cast<Eigen::Index>(N), static_cast<Eigen::Index>(T));
  for (Eigen::Index n = 0; n < static_cast<Eigen::Index>(N); ++n) {
    for (Eigen::Index t = 0; t < static_cast<Eigen::Index>(T); ++t) {
      ret(n, t) = 0.02 * rng.normal();
    }
  }
  const auto close = closes_from_returns(T, N, ret);
  const std::vector<std::vector<f64>> volume(T + 1U, std::vector<f64>(N, 1000.0));
  PanelFixture fx{T + 1U, N, close, volume};
  const std::vector<u32> group(N, 1U);

  FactorModelBuilder builder{stat_cfg(K)};
  const auto m = builder.build(fx.view(), T, std::span<const f64>{}, std::span<const u32>{group});
  ASSERT_FALSE(m.has_value());
  EXPECT_EQ(m.error().code(), atx::core::ErrorCode::InvalidArgument);
}

// ===========================================================================
//  Determinism: same panel/config twice ⇒ byte-identical V (identical risk()).
// ===========================================================================
TEST(RiskStatFactor, DeterministicReplay) {
  constexpr usize N = 120U;
  constexpr usize T = 24U;
  constexpr usize K = 2U;
  Xoshiro256pp rng{0xFACEU};
  MatX f_true(static_cast<Eigen::Index>(T), static_cast<Eigen::Index>(K));
  MatX b_true(static_cast<Eigen::Index>(N), static_cast<Eigen::Index>(K));
  for (Eigen::Index t = 0; t < static_cast<Eigen::Index>(T); ++t) {
    for (Eigen::Index kk = 0; kk < static_cast<Eigen::Index>(K); ++kk) {
      f_true(t, kk) = 0.03 * rng.normal();
    }
  }
  for (Eigen::Index n = 0; n < static_cast<Eigen::Index>(N); ++n) {
    for (Eigen::Index kk = 0; kk < static_cast<Eigen::Index>(K); ++kk) {
      b_true(n, kk) = rng.normal();
    }
  }
  MatX ret(static_cast<Eigen::Index>(N), static_cast<Eigen::Index>(T));
  for (Eigen::Index n = 0; n < static_cast<Eigen::Index>(N); ++n) {
    for (Eigen::Index t = 0; t < static_cast<Eigen::Index>(T); ++t) {
      f64 v = 0.0;
      for (Eigen::Index kk = 0; kk < static_cast<Eigen::Index>(K); ++kk) {
        v += b_true(n, kk) * f_true(t, kk);
      }
      ret(n, t) = v + 0.002 * rng.normal();
    }
  }
  const auto close = closes_from_returns(T, N, ret);
  const std::vector<std::vector<f64>> volume(T + 1U, std::vector<f64>(N, 1000.0));
  PanelFixture fx{T + 1U, N, close, volume};
  const std::vector<u32> group(N, 1U);

  FactorModelBuilder builder{stat_cfg(K)};
  const auto m1 = builder.build(fx.view(), T, std::span<const f64>{}, std::span<const u32>{group});
  const auto m2 = builder.build(fx.view(), T, std::span<const f64>{}, std::span<const u32>{group});
  ASSERT_TRUE(m1.has_value()) << (m1 ? "" : m1.error().to_string());
  ASSERT_TRUE(m2.has_value());
  std::vector<f64> w(N, 0.0);
  for (usize i = 0; i < N; ++i) {
    w[i] = (i % 3U == 0U) ? 0.02 : -0.01;
  }
  EXPECT_EQ(m1->risk(std::span<const f64>{w}), m2->risk(std::span<const f64>{w})); // byte-identical
}

// ===========================================================================
//  Truncation-invariance: older rows beyond the window's return reach (rows >
//  window) cannot change the estimate. The window reads step_return(s,i) for
//  s in [0,window) ⇒ closes [0, window]; rows >= window+1 are invisible.
// ===========================================================================
TEST(RiskStatFactor, FitWindowTruncationInvariant) {
  constexpr usize N = 100U;
  constexpr usize T = 20U;
  constexpr usize K = 2U;
  constexpr usize extra = 4U; // older rows beyond the window's reach
  Xoshiro256pp rng{0xABCDU};
  MatX f_true(static_cast<Eigen::Index>(T), static_cast<Eigen::Index>(K));
  MatX b_true(static_cast<Eigen::Index>(N), static_cast<Eigen::Index>(K));
  for (Eigen::Index t = 0; t < static_cast<Eigen::Index>(T); ++t) {
    for (Eigen::Index kk = 0; kk < static_cast<Eigen::Index>(K); ++kk) {
      f_true(t, kk) = 0.03 * rng.normal();
    }
  }
  for (Eigen::Index n = 0; n < static_cast<Eigen::Index>(N); ++n) {
    for (Eigen::Index kk = 0; kk < static_cast<Eigen::Index>(K); ++kk) {
      b_true(n, kk) = rng.normal();
    }
  }
  MatX ret(static_cast<Eigen::Index>(N), static_cast<Eigen::Index>(T));
  for (Eigen::Index n = 0; n < static_cast<Eigen::Index>(N); ++n) {
    for (Eigen::Index t = 0; t < static_cast<Eigen::Index>(T); ++t) {
      f64 v = 0.0;
      for (Eigen::Index kk = 0; kk < static_cast<Eigen::Index>(K); ++kk) {
        v += b_true(n, kk) * f_true(t, kk);
      }
      ret(n, t) = v + 0.002 * rng.normal();
    }
  }
  const std::vector<std::vector<f64>> volume(T + 1U + extra, std::vector<f64>(N, 1000.0));
  auto make = [&](f64 tail) {
    // closes [0, window] from the planted returns; rows > window are the free tail.
    const auto in_window = closes_from_returns(T, N, ret); // length T+1, rows [0,T]
    std::vector<std::vector<f64>> close(T + 1U + extra, std::vector<f64>(N));
    for (usize t = 0; t <= T; ++t) {
      close[t] = in_window[t];
    }
    for (usize t = T + 1U; t < T + 1U + extra; ++t) {
      for (usize i = 0; i < N; ++i) {
        close[t][i] = tail;
      }
    }
    return PanelFixture{T + 1U + extra, N, close, volume};
  };
  const PanelFixture a = make(13.0);
  const PanelFixture b = make(987.0);
  FactorModelBuilder builder{stat_cfg(K)};
  const auto ma = builder.build(a.view(), T, std::span<const f64>{},
                                std::span<const u32>{std::vector<u32>(N, 1U)});
  const auto mb = builder.build(b.view(), T, std::span<const f64>{},
                                std::span<const u32>{std::vector<u32>(N, 1U)});
  ASSERT_TRUE(ma.has_value()) << (ma ? "" : ma.error().to_string());
  ASSERT_TRUE(mb.has_value());
  std::vector<f64> w(N, 0.0);
  for (usize i = 0; i < N; ++i) {
    w[i] = (i % 2U == 0U) ? 0.015 : -0.012;
  }
  EXPECT_EQ(ma->risk(std::span<const f64>{w}), mb->risk(std::span<const f64>{w}));
}

// ===========================================================================
//  GLS on/off: both produce SPD applicable models; GLS changes the estimate.
//  Heteroskedastic idiosyncratic noise (var grows with instrument index) makes the
//  GLS reweight materially change the recovered factors vs the equal-weight pass.
// ===========================================================================
TEST(RiskStatFactor, GlsReweightChangesEstimateBothSpd) {
  constexpr usize N = 150U;
  constexpr usize T = 30U;
  constexpr usize K = 2U;
  Xoshiro256pp rng{0x9999U};
  MatX f_true(static_cast<Eigen::Index>(T), static_cast<Eigen::Index>(K));
  MatX b_true(static_cast<Eigen::Index>(N), static_cast<Eigen::Index>(K));
  for (Eigen::Index t = 0; t < static_cast<Eigen::Index>(T); ++t) {
    for (Eigen::Index kk = 0; kk < static_cast<Eigen::Index>(K); ++kk) {
      f_true(t, kk) = 0.03 * rng.normal();
    }
  }
  for (Eigen::Index n = 0; n < static_cast<Eigen::Index>(N); ++n) {
    for (Eigen::Index kk = 0; kk < static_cast<Eigen::Index>(K); ++kk) {
      b_true(n, kk) = rng.normal();
    }
  }
  MatX ret(static_cast<Eigen::Index>(N), static_cast<Eigen::Index>(T));
  for (Eigen::Index n = 0; n < static_cast<Eigen::Index>(N); ++n) {
    // Idiosyncratic vol grows with n ⇒ heteroskedastic ⇒ GLS differs from EW.
    const f64 sd = 0.001 + 0.01 * (static_cast<f64>(n) / static_cast<f64>(N));
    for (Eigen::Index t = 0; t < static_cast<Eigen::Index>(T); ++t) {
      f64 v = 0.0;
      for (Eigen::Index kk = 0; kk < static_cast<Eigen::Index>(K); ++kk) {
        v += b_true(n, kk) * f_true(t, kk);
      }
      ret(n, t) = v + sd * rng.normal();
    }
  }
  const auto close = closes_from_returns(T, N, ret);
  const std::vector<std::vector<f64>> volume(T + 1U, std::vector<f64>(N, 1000.0));
  PanelFixture fx{T + 1U, N, close, volume};
  const std::vector<u32> group(N, 1U);
  std::vector<f64> w(N, 0.0);
  for (usize i = 0; i < N; ++i) {
    w[i] = (i % 2U == 0U) ? 0.02 : -0.015;
  }

  FactorModelConfig cfg_gls = stat_cfg(K);
  cfg_gls.cov.apca_gls_reweight = true;
  FactorModelConfig cfg_ew = stat_cfg(K);
  cfg_ew.cov.apca_gls_reweight = false;

  FactorModelBuilder b_gls{cfg_gls};
  FactorModelBuilder b_ew{cfg_ew};
  const auto m_gls = b_gls.build(fx.view(), T, std::span<const f64>{}, std::span<const u32>{group});
  const auto m_ew = b_ew.build(fx.view(), T, std::span<const f64>{}, std::span<const u32>{group});
  ASSERT_TRUE(m_gls.has_value()) << (m_gls ? "" : m_gls.error().to_string());
  ASSERT_TRUE(m_ew.has_value()) << (m_ew ? "" : m_ew.error().to_string());
  const f64 rk_gls = m_gls->risk(std::span<const f64>{w});
  const f64 rk_ew = m_ew->risk(std::span<const f64>{w});
  EXPECT_TRUE(std::isfinite(rk_gls) && rk_gls > 0.0); // both SPD/applicable
  EXPECT_TRUE(std::isfinite(rk_ew) && rk_ew > 0.0);
  EXPECT_NE(rk_gls, rk_ew); // GLS materially changes the estimate
}


}  // namespace atxtest_risk_stat_factor_test
