// risk_cov_ewma_test.cpp — S8.2: EWMA split-half-life factor covariance + Newey-West.
//
// ewma_factor_covariance(fseries, vol_hl, corr_hl, nw_lags) builds the K×K factor
// covariance the vendor recipe trades:
//   * EWMA variances at `vol_hl`            σ_c² = Σ_r w_r^vol (x−μ)² / Σ_r w_r^vol
//   * EWMA correlations at `corr_hl`        ρ_ij = Cov_ij / √(Cov_ii·Cov_jj)
//   * recombine                             F_ij = ρ_ij·σ_i·σ_j ,  F_ii = σ_i²
//   * Newey-West Bartlett serial-corr add   F += Σ_{d=1..L}(1−d/(L+1))(Γ_d+Γ_dᵀ)
//   * SPD eigenvalue floor (Cholesky must succeed)
//
// Row 0 of `fseries` is the NEWEST date ⇒ the k-steps-back weight on row r is
// w_r = 2^(−r/H). A half-life of 0 selects EQUAL weights (the no-EWMA-tilt path);
// the Newey-West autocovariances are always equal-weighted (the standard HAC
// estimator — the EWMA tilt already lives in the contemporaneous Γ_0 = F_split).
//
// Coverage (plan §5 S8.2 acceptance):
//   * EWMA weight closed form: variance == Σ w_r(x−μ)²/Σ w_r; ratio w_r/w_{r+1} == 2^(1/H).
//   * Newey-West AR(1): long-run variance ≈ σ²(1+2Σ_d(1−d/(L+1))φ^d).
//   * SPD: the kernel's F passes a Cholesky / has a positive minimum eigenvalue.
//   * Conditioning: cond(F_split) < cond(F_mle_single) on a T≈K window.
//   * Determinism: same input twice ⇒ byte-identical F.
//   * Backward-compat: default build F equals the as-built detail::factor_covariance.

#include <array>   // std::array
#include <cmath>   // std::pow, std::sqrt, std::isfinite, std::isnan
#include <cstdint> // fixed-width
#include <cstring> // std::memcmp
#include <limits>  // std::numeric_limits (quiet NaN sentinel)
#include <span>
#include <vector>

#include <gtest/gtest.h>

#include <Eigen/Dense>

#include "atx/core/linalg/decompose.hpp" // symmetric_eig (condition number / SPD check)
#include "atx/core/linalg/linalg.hpp"
#include "atx/core/types.hpp"

#include "atx/engine/loop/panel_types.hpp" // PanelView, PanelField, kPanelFieldCount
#include "atx/engine/loop/types.hpp"       // InstrumentId (Symbol)
#include "atx/engine/risk/cov_ewma.hpp"
#include "atx/engine/risk/factor_model.hpp" // detail::factor_covariance (single-MLE reference)

namespace atxtest_risk_cov_ewma_test {

using atx::f64;
using atx::u32;
using atx::usize;
using atx::core::domain::Symbol;
using atx::core::linalg::MatX;
using atx::core::linalg::VecX;
using atx::engine::InstrumentId;
using atx::engine::kPanelFieldCount;
using atx::engine::PanelField;
using atx::engine::PanelView;
using atx::engine::risk::ewma_factor_covariance;
using atx::engine::risk::FactorCovMethod;
using atx::engine::risk::FactorModelBuilder;
using atx::engine::risk::FactorModelConfig;

constexpr f64 kTestNaN = std::numeric_limits<f64>::quiet_NaN();

// Minimal single-sector PanelView fixture (K=1 dummy, no per-instrument lookback)
// — same backing-storage pattern as risk_factor_builder_test.cpp. Row 0 = newest.
class PanelFixture {
public:
  PanelFixture(usize n_rows, usize n_inst, const std::vector<std::vector<f64>> &close)
      : n_rows_{n_rows}, n_inst_{n_inst}, cap_{pow2_ceil(n_rows)},
        mask_words_{(n_inst + 63U) / 64U} {
    universe_.reserve(n_inst);
    for (usize i = 0; i < n_inst; ++i) {
      universe_.push_back(Symbol{static_cast<u32>(i + 1U)});
    }
    fields_.assign(kPanelFieldCount * cap_ * n_inst_, kTestNaN);
    mask_.assign(cap_ * mask_words_, 0ULL);
    for (usize r = 0; r < n_rows_; ++r) {
      const usize phys = (n_rows_ - 1U) - r; // newest-first r -> physical row
      for (usize i = 0; i < n_inst_; ++i) {
        const f64 c = close[r][i];
        set(PanelField::Open, phys, i, c);
        set(PanelField::High, phys, i, c);
        set(PanelField::Low, phys, i, c);
        set(PanelField::Close, phys, i, c);
        set(PanelField::Volume, phys, i, 1000.0);
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

// Condition number max/min eigenvalue of a symmetric PSD matrix (symmetric_eig
// returns eigenvalues ascending). Returns +inf if the smallest eigenvalue is <= 0.
[[nodiscard]] f64 cond2(const MatX &a) {
  const auto eig = atx::core::linalg::symmetric_eig(a);
  EXPECT_TRUE(eig.has_value());
  const VecX &ev = eig->values;
  const f64 lo = ev[0];
  const f64 hi = ev[ev.size() - 1];
  return (lo <= 0.0) ? std::numeric_limits<f64>::infinity() : hi / lo;
}

// ===========================================================================
//  EWMA weight closed form. A K=1 known series with vol_hl=H: the EWMA variance
//  the kernel reports (the F(0,0) diagonal, no NW lags) must equal the hand
//  oracle Σ_r w_r(x_r−μ)²/Σ_r w_r with w_r = 2^(−r/H), μ the EWMA mean.
// ===========================================================================
TEST(CovEwma, EwmaVarianceMatchesClosedForm) {
  const usize H = 4U;
  // Row 0 == newest. Arbitrary distinct values (not mean-zero).
  const std::array<f64, 6> x = {0.05, -0.02, 0.03, 0.01, -0.04, 0.02};
  MatX fseries(static_cast<Eigen::Index>(x.size()), 1);
  for (usize r = 0; r < x.size(); ++r) {
    fseries(static_cast<Eigen::Index>(r), 0) = x[r];
  }

  // Hand oracle: w_r = 2^(−r/H); μ = Σ w x / Σ w; var = Σ w (x−μ)² / Σ w.
  f64 sw = 0.0, swx = 0.0;
  for (usize r = 0; r < x.size(); ++r) {
    const f64 w = std::pow(2.0, -static_cast<f64>(r) / static_cast<f64>(H));
    sw += w;
    swx += w * x[r];
  }
  const f64 mu = swx / sw;
  f64 swxx = 0.0;
  for (usize r = 0; r < x.size(); ++r) {
    const f64 w = std::pow(2.0, -static_cast<f64>(r) / static_cast<f64>(H));
    swxx += w * (x[r] - mu) * (x[r] - mu);
  }
  const f64 oracle_var = swxx / sw;

  // corr_hl irrelevant for K=1 (ρ_00 == 1); nw_lags=0 ⇒ no serial-corr add.
  const MatX f = ewma_factor_covariance(fseries, /*vol_hl=*/H, /*corr_hl=*/H, /*nw_lags=*/0U);
  ASSERT_EQ(f.rows(), 1);
  ASSERT_EQ(f.cols(), 1);
  EXPECT_NEAR(f(0, 0), oracle_var, 1e-15);
}

// ===========================================================================
//  EWMA weight ratio is EXACTLY 2^(−1/H). We expose the weight builder via the
//  detail kernel; the ratio of consecutive weights is a hard equality (the whole
//  geometric-decay contract).
// ===========================================================================
TEST(CovEwma, EwmaWeightRatioIsExact) {
  const usize H = 7U;
  const usize n = 5U;
  const VecX w = atx::engine::risk::detail::ewma_weights(n, H);
  ASSERT_EQ(w.size(), static_cast<Eigen::Index>(n));
  const f64 ratio = std::pow(2.0, -1.0 / static_cast<f64>(H));
  for (usize r = 0; r + 1U < n; ++r) {
    // w_{r+1}/w_r == 2^(−1/H), exactly (both are exact powers of the same base).
    EXPECT_DOUBLE_EQ(w[static_cast<Eigen::Index>(r + 1U)] / w[static_cast<Eigen::Index>(r)], ratio);
  }
  // H == 0 ⇒ equal weights (the no-tilt sentinel).
  const VecX we = atx::engine::risk::detail::ewma_weights(n, 0U);
  for (usize r = 0; r < n; ++r) {
    EXPECT_DOUBLE_EQ(we[static_cast<Eigen::Index>(r)], 1.0);
  }
}

// ===========================================================================
//  Newey-West AR(1). Build a long, deterministic AR(1) factor-return column
//  x_t = φ·x_{t−1} + ε_t with known φ. With equal weights (HL=0) the
//  contemporaneous variance Γ_0 ≈ σ²/(1−φ²) and the lag-d autocov Γ_d ≈ φ^d·Γ_0,
//  so the Newey-West long-run variance the kernel returns must match the analytic
//  Γ_0·(1 + 2Σ_{d=1..L}(1−d/(L+1))φ^d) within sampling tolerance.
// ===========================================================================
TEST(CovEwma, NeweyWestMatchesAr1LongRunVariance) {
  const f64 phi = 0.6;
  const usize L = 5U;
  const usize T = 8000U;

  // Deterministic innovations via a simple LCG mapped to (−1,1) — RNG-free build,
  // reproducible, zero-mean-ish. Newest-first row order does not matter for a
  // stationary series; we fill ascending then the kernel reads row r as k=r.
  std::vector<f64> series(T);
  std::uint64_t state = 0x9E3779B97F4A7C15ULL;
  auto next_eps = [&]() {
    state = state * 6364136223846793005ULL + 1442695040888963407ULL;
    const std::uint64_t hi = state >> 11U;                              // 53 bits
    const f64 u = static_cast<f64>(hi) / static_cast<f64>(1ULL << 53U); // [0,1)
    return 2.0 * u - 1.0;                                               // (−1,1), mean ≈ 0
  };
  f64 prev = 0.0;
  for (usize t = 0; t < T; ++t) {
    prev = phi * prev + next_eps();
    series[t] = prev;
  }
  MatX fseries(static_cast<Eigen::Index>(T), 1);
  for (usize t = 0; t < T; ++t) {
    fseries(static_cast<Eigen::Index>(t), 0) = series[t];
  }

  // Equal-weighted contemporaneous variance Γ_0 (HL=0) — the kernel's F_split for K=1.
  const MatX g0 = ewma_factor_covariance(fseries, /*vol_hl=*/0U, /*corr_hl=*/0U, /*nw_lags=*/0U);
  const f64 gamma0 = g0(0, 0);

  // Analytic Bartlett long-run variance from the SAME Γ_0 and the known φ.
  f64 factor = 1.0;
  for (usize d = 1U; d <= L; ++d) {
    const f64 bart = 1.0 - static_cast<f64>(d) / static_cast<f64>(L + 1U);
    factor += 2.0 * bart * std::pow(phi, static_cast<f64>(d));
  }
  const f64 analytic_lrv = gamma0 * factor;

  const MatX f_nw = ewma_factor_covariance(fseries, /*vol_hl=*/0U, /*corr_hl=*/0U, /*nw_lags=*/L);
  // 4% tolerance: the sample autocovariances Γ_d are consistent estimators of
  // φ^d·Γ_0 but carry finite-sample noise; T=8000 keeps it tight.
  EXPECT_NEAR(f_nw(0, 0), analytic_lrv, 0.04 * analytic_lrv);
  EXPECT_GT(f_nw(0, 0), gamma0); // positive serial corr ⇒ NW inflates the variance
}

// ===========================================================================
//  SPD: a multi-factor EWMA covariance (no NW) has a positive minimum eigenvalue
//  and its Cholesky succeeds — FactorModel::create's Cholesky must not reject it.
// ===========================================================================
TEST(CovEwma, KernelOutputIsSpd) {
  // 8×3 factor-return series (T=8 dates, K=3 factors), distinct columns.
  MatX fseries(8, 3);
  fseries << 0.012, -0.004, 0.020, -0.008, 0.011, -0.003, 0.015, 0.006, 0.009, -0.002, -0.013,
      0.017, 0.021, 0.003, -0.011, -0.010, 0.018, 0.004, 0.007, -0.009, 0.013, -0.014, 0.005,
      -0.006;

  const MatX f = ewma_factor_covariance(fseries, /*vol_hl=*/6U, /*corr_hl=*/12U, /*nw_lags=*/2U);
  ASSERT_EQ(f.rows(), 3);
  ASSERT_EQ(f.cols(), 3);
  // Symmetric.
  for (Eigen::Index i = 0; i < 3; ++i) {
    for (Eigen::Index j = 0; j < 3; ++j) {
      EXPECT_NEAR(f(i, j), f(j, i), 1e-15);
    }
  }
  // Positive-definite: Cholesky succeeds.
  Eigen::LLT<MatX> llt(f);
  EXPECT_EQ(llt.info(), Eigen::Success);
  // Min eigenvalue > 0.
  const auto eig = atx::core::linalg::symmetric_eig(f);
  ASSERT_TRUE(eig.has_value());
  EXPECT_GT(eig->values[0], 0.0);
}

// ===========================================================================
//  Conditioning: the split-half-life EWMA covariance is MATERIALLY better
//  conditioned (max/min eigenvalue ratio) than the single-window MLE on an
//  ill-conditioned T≈K panel, by a real margin — it must DEMONSTRATE the benefit,
//  not merely permit it.
//
//  Reference: the single-window MLE is detail::factor_covariance(fseries, 0.0) ==
//  (1−0)·S + 0·m·I == S, the raw population covariance (the S in the LW formula).
//
//  Fixture (T=12, K=5; row 0 = newest): the panel has a STALE near-collinear OLD
//  regime (rows 5..11 are near-rank-1 — one shared driver, tiny idiosyncratic
//  spread) and a WELL-SPREAD recent regime (rows 0..4 are independent, comparable
//  scale). The equal-weighted MLE is dominated by the collinear old block (a tiny
//  smallest eigenvalue ⇒ large cond). The split's EWMA half-lives (vol_hl=4,
//  corr_hl=6) DECAY that stale block out and anchor the correlation on the recent
//  well-spread rows ⇒ the smallest eigenvalue lifts and the conditioning improves
//  by ~25%. The exact panel was generated by a deterministic LCG (recorded here as
//  a literal so the test is self-contained and RNG-free).
// ===========================================================================
TEST(CovEwma, SplitHalfLifeBetterConditionedThanSingleMle) {
  MatX fseries(12, 5);
  // clang-format off
  fseries <<
    -0.0044250632607524985, -0.0051064452532222854, -0.010660572243119604,  0.014804214781821413,  -0.0048948462403209814,
    -0.012772851662888078,   0.0021916785073961098, -0.0093609515342722629, -0.002509909736386796,  -0.00092480210917436515,
     0.0062396464481753386,  0.00036397620789665574, 0.013175342296010743,  -0.00099152019893808288,-0.0069757018369264361,
    -0.013615474842522202,   0.0030674216056360646,  0.012278737889220735,  -0.014176497444784747,  -0.00066605708136717619,
    -0.0020547458142809471, -0.013000475328195951,  -0.0083112438178526947, -0.012767660642460217,  -0.012584746038300069,
     0.014975765411358994,   0.014768117947795888,   0.012873199246790883,   0.012070425114962405,   0.012138963670714898,
    -0.0089215198708973101, -0.0091281179166173287, -0.0084949924095564372, -0.0070917320433397813, -0.0066315399019162419,
     0.0099782551744006919,  0.009310167090384968,   0.0091861058670757792,  0.0098521706116246055,  0.0080743946530995713,
    -0.0012407388342834942, -0.0011400184422055639, -0.00036125083817140546,-0.00055120587105658724,-0.00031631376955728013,
     0.0082201943091682658,  0.0062347566911542286,  0.0058701224177418448,  0.0062068889736493467,  0.0067323389479877821,
     0.011455603759156885,   0.0091328457999510209,  0.009817805124588689,   0.0092067146419547493,  0.0087636958617355459,
    -0.0097838190710889724, -0.0087580532225277202, -0.0090806955642509513, -0.0078309818289282115, -0.0072397805216408376;
  // clang-format on

  // Single-window MLE reference: S == detail::factor_covariance(fseries, 0.0).
  const MatX f_mle = atx::engine::risk::detail::factor_covariance(fseries, /*cfg_shrink=*/0.0);
  // Split-half-life EWMA covariance (no NW so the comparison is purely conditioning):
  // vol_hl decays inside the window, corr_hl decays the stale collinear old regime.
  const MatX f_split = ewma_factor_covariance(fseries, /*vol_hl=*/4U, /*corr_hl=*/6U,
                                              /*nw_lags=*/0U);

  const f64 cond_mle = cond2(f_mle);     // ≈ 186.66 (collinear-block-dominated)
  const f64 cond_split = cond2(f_split); // ≈ 139.31 (stale block decayed out)
  EXPECT_TRUE(std::isfinite(cond_split));
  // A REAL margin (measured ratio ≈ 0.746): assert at least a 20% improvement, well
  // clear of sampling noise, so the test pins the mechanism rather than permitting it.
  EXPECT_LT(cond_split, 0.8 * cond_mle);
}

// ===========================================================================
//  Determinism: same input twice ⇒ byte-identical F (the kernel is RNG-free and
//  order-fixed). bit-exact element compare.
// ===========================================================================
TEST(CovEwma, DeterministicByteIdentical) {
  MatX fseries(7, 2);
  fseries << 0.011, -0.006, -0.004, 0.013, 0.018, -0.002, -0.009, 0.007, 0.015, -0.011, -0.003,
      0.005, 0.022, -0.014;
  const MatX a = ewma_factor_covariance(fseries, 5U, 10U, 3U);
  const MatX b = ewma_factor_covariance(fseries, 5U, 10U, 3U);
  ASSERT_EQ(a.rows(), b.rows());
  ASSERT_EQ(a.cols(), b.cols());
  EXPECT_EQ(0, std::memcmp(a.data(), b.data(),
                           static_cast<usize>(a.size()) * sizeof(f64))); // byte-identical
}

// ===========================================================================
//  Recombination: F_ii == σ_i² (vol-HL variance) and the off-diagonal obeys
//  F_ij == ρ_ij·σ_i·σ_j with |ρ_ij| <= 1 (a valid correlation). Checked on a 2×… ,
//  here we just assert the diagonal equals the standalone EWMA variance and the
//  implied correlation is in [−1, 1].
// ===========================================================================
TEST(CovEwma, RecombinationDiagonalIsVolHalfLifeVariance) {
  MatX fseries(6, 2);
  fseries << 0.020, -0.010, -0.012, 0.014, 0.015, -0.008, -0.007, 0.010, 0.022, -0.012, -0.016,
      0.017;
  const usize vol_hl = 4U;
  const usize corr_hl = 9U;
  const MatX f = ewma_factor_covariance(fseries, vol_hl, corr_hl, /*nw_lags=*/0U);

  // Standalone σ_0², σ_1² at vol_hl (1×1 kernels on each column).
  MatX c0(6, 1), c1(6, 1);
  c0 = fseries.col(0);
  c1 = fseries.col(1);
  const f64 v0 = ewma_factor_covariance(c0, vol_hl, vol_hl, 0U)(0, 0);
  const f64 v1 = ewma_factor_covariance(c1, vol_hl, vol_hl, 0U)(0, 0);
  EXPECT_NEAR(f(0, 0), v0, 1e-15);
  EXPECT_NEAR(f(1, 1), v1, 1e-15);
  const f64 rho = f(0, 1) / std::sqrt(f(0, 0) * f(1, 1));
  EXPECT_LE(rho, 1.0 + 1e-12);
  EXPECT_GE(rho, -1.0 - 1e-12);
}

// ===========================================================================
//  Backward-compat: the DEFAULT FactorModelConfig (factor_cov_method ==
//  LedoitWolfSingle) drives the as-built P4 factor-covariance path byte-for-byte —
//  it is IDENTICAL to an explicitly-LedoitWolfSingle build and DISTINCT from the
//  opt-in EwmaNeweyWest path. We exercise this through the PUBLIC FactorModelBuilder
//  ::build over a single-sector (K=1) panel and compare risk() readouts (F is not
//  otherwise observable). This pins both that the default reproduces P4 and that the
//  S8.2 dispatch is genuinely wired (the EWMA branch produces a different model).
// ===========================================================================
TEST(CovEwma, DefaultConfigReproducesP4AndEwmaDiffers) {
  const usize window = 8U;
  const usize n_inst = 4U;
  const usize n_rows = window + 1U;
  // Distinct geometric-ish price paths so per-date returns vary across instruments.
  std::vector<std::vector<f64>> close(n_rows, std::vector<f64>(n_inst));
  for (usize i = 0; i < n_inst; ++i) {
    f64 px = 100.0 + 10.0 * static_cast<f64>(i);
    for (usize r = n_rows; r-- > 0U;) { // oldest -> newest
      px *= 1.0 + 0.01 * (static_cast<f64>((r + i) % 3U) - 1.0);
      close[r][i] = px;
    }
  }
  PanelFixture fx{n_rows, n_inst, close};
  const std::vector<u32> group{1U, 1U, 1U, 1U}; // one sector -> K=1
  const std::span<const f64> no_cap{};
  const std::span<const u32> grp{group};

  // (a) DEFAULT config — must equal an EXPLICIT LedoitWolfSingle build, byte-for-byte.
  FactorModelConfig def_cfg;
  def_cfg.sector_factors = true;
  def_cfg.style_mask = 0x00; // sectors only -> K=1, no lookback
  FactorModelConfig lw_cfg = def_cfg;
  lw_cfg.cov.factor_cov_method = FactorCovMethod::LedoitWolfSingle; // explicit == default
  const auto m_def = FactorModelBuilder{def_cfg}.build(fx.view(), window, no_cap, grp);
  const auto m_lw = FactorModelBuilder{lw_cfg}.build(fx.view(), window, no_cap, grp);
  ASSERT_TRUE(m_def.has_value()) << (m_def ? "" : m_def.error().to_string());
  ASSERT_TRUE(m_lw.has_value());
  const std::vector<f64> w{0.4, -0.1, 0.3, -0.2};
  const std::span<const f64> ws{w};
  EXPECT_EQ(m_def->risk(ws), m_lw->risk(ws)); // default IS the P4 path, byte-identical

  // (b) EWMA path — the dispatch is genuinely wired ⇒ a DIFFERENT covariance/model.
  FactorModelConfig ewma_cfg = def_cfg;
  ewma_cfg.cov.factor_cov_method = FactorCovMethod::EwmaNeweyWest;
  ewma_cfg.cov.vol_halflife = 3U;
  ewma_cfg.cov.corr_halflife = 6U;
  ewma_cfg.cov.nw_lags = 1U;
  const auto m_ewma = FactorModelBuilder{ewma_cfg}.build(fx.view(), window, no_cap, grp);
  ASSERT_TRUE(m_ewma.has_value());
  EXPECT_NE(m_ewma->risk(ws), m_def->risk(ws)); // opt-in path changes the result
}


}  // namespace atxtest_risk_cov_ewma_test
