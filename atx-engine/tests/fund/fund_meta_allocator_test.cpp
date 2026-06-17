// fund_meta_allocator_test.cpp — P2-S2-2: the Meta-Allocator (risk-budget +
// portfolio-of-books Kelly → per-sleeve capital weights).
//
// MetaAllocator::allocate lifts the scalar book::size_book to N sleeves: given a
// trailing sleeve-return covariance Ω (S×S), per-sleeve vols and per-sleeve
// capacity box, it computes a risk-budget weight vector w_rb (InverseVol /
// EqualRiskContribution log-barrier / HierarchicalRiskParity), then applies a
// fund-level fractional-Kelly leverage with a gross-clipped vol-target to produce
// the capital weights c_s = clip(k·w_rb_s, 0, caps_s) with Σ|c| ≤ max_gross.
//
// Coverage (the S2-2 contract):
//   1. ERC exactness (R5): RC_s = w_s·(Ωw)_s equal across s for equal budget.
//   2. CCD vs equicorrelation closed form (R8): constant-ρ Ω ⇒ w_rb ∝ 1/σ.
//   3. Determinism (R1): identical inputs ⇒ byte-identical c; fixed solve_iters
//      (1-iter vs 64-iter differ ⇒ no early-exit shortcut).
//   4. InverseVol: c ∝ 1/σ (pre-Kelly proportionality).
//   5. HRP: w_rb all > 0, Σ = 1, finite, sane 2-block diversification.
//   6. Capacity box (§0.3): binding caps clip c_s; Σ|c| ≤ max_gross always.
//   7. Vol-target (A5): pre-cap fund vol sqrt(cᵀΩc) ≈ σ* when no cap binds.
//   8. s=0 fallback (§0.8): degenerate Ω ⇒ inverse_vol fallback, Ok (no throw);
//      empty Ω ⇒ Ok empty.
//   9. Single sleeve: S=1 ⇒ w_rb=[1], c=[k].
//  10. Errors: shape mismatch / bad risk_budget ⇒ Err(InvalidArgument).

#include <bit>    // std::bit_cast (byte-identical determinism check)
#include <cmath>  // std::sqrt, std::fabs, std::isfinite
#include <cstdint>// std::uint64_t
#include <limits> // std::numeric_limits (NaN poison fixtures)
#include <vector>

#include <gtest/gtest.h>

#include "atx/core/error.hpp"         // ErrorCode
#include "atx/core/linalg/linalg.hpp" // MatX, VecX
#include "atx/core/types.hpp"         // f64, usize

#include "atx/engine/fund/meta_allocator.hpp" // the unit under test

namespace atxtest_fund_meta_allocator_test {

using atx::f64;
using atx::usize;
using atx::core::ErrorCode;
using atx::core::linalg::MatX;
using atx::engine::fund::CapitalWeights;
using atx::engine::fund::MetaAllocator;
using atx::engine::fund::MetaAllocatorConfig;
using atx::engine::fund::RiskBudgetMethod;

// ---------------------------------------------------------------------------
//  Fixtures
// ---------------------------------------------------------------------------

// A non-trivial SPD 3×3 covariance: build Ω = ρ·σσᵀ-style with given vols and a
// modest correlation structure, made SPD by construction (diagonally dominant).
[[nodiscard]] MatX spd_3() {
  MatX omega(3, 3);
  omega << 0.04, 0.006, 0.002, //
      0.006, 0.09, 0.012,      //
      0.002, 0.012, 0.16;      //
  return omega;
}

// Constant-correlation Ω: ρ equal off-diagonal, given per-sleeve vols σ_i, so
// Ω_ij = ρ·σ_i·σ_j (i≠j), Ω_ii = σ_i². For equicorrelation the ERC closed form is
// w_s ∝ 1/σ_s.
[[nodiscard]] MatX const_corr(const std::vector<f64> &sig, f64 rho) {
  const auto s = static_cast<Eigen::Index>(sig.size());
  MatX omega(s, s);
  for (Eigen::Index i = 0; i < s; ++i) {
    for (Eigen::Index j = 0; j < s; ++j) {
      const auto si = sig[static_cast<usize>(i)];
      const auto sj = sig[static_cast<usize>(j)];
      omega(i, j) = (i == j) ? si * si : rho * si * sj;
    }
  }
  return omega;
}

// sqrt(diag Ω) — the per-sleeve vols the allocator consumes.
[[nodiscard]] std::vector<f64> diag_vol(const MatX &omega) {
  std::vector<f64> v(static_cast<usize>(omega.rows()));
  for (Eigen::Index i = 0; i < omega.rows(); ++i) {
    v[static_cast<usize>(i)] = std::sqrt(omega(i, i));
  }
  return v;
}

// A config whose Kelly/cap composition is the IDENTITY on w_rb: fractional_kelly
// = 1, no vol-target, huge gross cap. Combined with huge caps this makes c == w_rb
// so a test can read the risk-budget stage directly off the returned c.
[[nodiscard]] MetaAllocatorConfig identity_kelly(RiskBudgetMethod m) {
  MetaAllocatorConfig cfg;
  cfg.method = m;
  cfg.fractional_kelly = 1.0;
  cfg.target_vol = 0.0;
  cfg.max_gross = 1e9;
  return cfg;
}

// caps so large they never bind.
[[nodiscard]] std::vector<f64> huge_caps(usize s) { return std::vector<f64>(s, 1e9); }

// Risk contribution RC_s = w_s·(Ωw)_s (un-normalized; equal across s ⇔ ERC).
[[nodiscard]] std::vector<f64> risk_contributions(const MatX &omega, const std::vector<f64> &w) {
  const auto s = static_cast<Eigen::Index>(w.size());
  std::vector<f64> rc(static_cast<usize>(s));
  for (Eigen::Index i = 0; i < s; ++i) {
    f64 ow = 0.0;
    for (Eigen::Index j = 0; j < s; ++j) {
      ow += omega(i, j) * w[static_cast<usize>(j)];
    }
    rc[static_cast<usize>(i)] = w[static_cast<usize>(i)] * ow;
  }
  return rc;
}

[[nodiscard]] f64 gross(const std::vector<f64> &c) {
  f64 g = 0.0;
  for (const f64 v : c) {
    g += std::fabs(v);
  }
  return g;
}

[[nodiscard]] f64 quad_form(const MatX &omega, const std::vector<f64> &c) {
  const auto s = static_cast<Eigen::Index>(c.size());
  f64 q = 0.0;
  for (Eigen::Index i = 0; i < s; ++i) {
    for (Eigen::Index j = 0; j < s; ++j) {
      q += c[static_cast<usize>(i)] * omega(i, j) * c[static_cast<usize>(j)];
    }
  }
  return q;
}

// ===========================================================================
//  1. ERC exactness (R5)
// ===========================================================================
TEST(FundMetaAllocator, EqualRiskContribution_SpdOmega_EqualizesRiskContributions) {
  const MatX omega = spd_3();
  const auto vol = diag_vol(omega);
  MetaAllocator alloc{identity_kelly(RiskBudgetMethod::EqualRiskContribution)};

  const auto res = alloc.allocate(omega, vol, huge_caps(3));
  ASSERT_TRUE(res.has_value());
  const auto &c = res->c;
  ASSERT_EQ(c.size(), 3U);

  // c == w_rb here (identity Kelly, no cap). Equal-budget ERC ⇒ all RC_s equal.
  const auto rc = risk_contributions(omega, c);
  for (usize i = 1; i < rc.size(); ++i) {
    EXPECT_NEAR(rc[i], rc[0], 1e-7 * rc[0]) << "RC mismatch at sleeve " << i;
  }
}

TEST(FundMetaAllocator, EqualRiskContribution_CustomBudget_MatchesBudgetShares) {
  const MatX omega = spd_3();
  const auto vol = diag_vol(omega);
  MetaAllocatorConfig cfg = identity_kelly(RiskBudgetMethod::EqualRiskContribution);
  cfg.risk_budget = {0.5, 0.3, 0.2}; // Σ=1, all > 0
  MetaAllocator alloc{cfg};

  const auto res = alloc.allocate(omega, vol, huge_caps(3));
  ASSERT_TRUE(res.has_value());
  const auto rc = risk_contributions(omega, res->c);
  const f64 total = rc[0] + rc[1] + rc[2];
  // Stationarity: RC_s / Σ RC = b_s.
  EXPECT_NEAR(rc[0] / total, 0.5, 1e-6);
  EXPECT_NEAR(rc[1] / total, 0.3, 1e-6);
  EXPECT_NEAR(rc[2] / total, 0.2, 1e-6);
}

// ===========================================================================
//  2. CCD vs equicorrelation closed form (R8)
// ===========================================================================
TEST(FundMetaAllocator, EqualRiskContribution_ConstantCorrelation_MatchesInverseVol) {
  const std::vector<f64> sig{0.10, 0.20, 0.30, 0.15};
  const MatX omega = const_corr(sig, 0.3);
  MetaAllocator alloc{identity_kelly(RiskBudgetMethod::EqualRiskContribution)};

  const auto res = alloc.allocate(omega, diag_vol(omega), huge_caps(4));
  ASSERT_TRUE(res.has_value());
  const auto &c = res->c;

  // Closed form: w_s ∝ 1/σ_s. Compare normalized ratios against c (∝ w_rb).
  f64 inv_sum = 0.0;
  for (const f64 s : sig) {
    inv_sum += 1.0 / s;
  }
  f64 c_sum = 0.0;
  for (const f64 v : c) {
    c_sum += v;
  }
  for (usize i = 0; i < sig.size(); ++i) {
    const f64 want = (1.0 / sig[i]) / inv_sum;
    const f64 got = c[i] / c_sum;
    EXPECT_NEAR(got, want, 1e-6) << "equicorr ERC ≠ inverse-vol at " << i;
  }
}

// ===========================================================================
//  3. Determinism (R1)
// ===========================================================================
TEST(FundMetaAllocator, Allocate_IdenticalInputs_ByteIdenticalWeights) {
  const MatX omega = spd_3();
  const auto vol = diag_vol(omega);
  MetaAllocator alloc{identity_kelly(RiskBudgetMethod::EqualRiskContribution)};

  const auto a = alloc.allocate(omega, vol, huge_caps(3));
  const auto b = alloc.allocate(omega, vol, huge_caps(3));
  ASSERT_TRUE(a.has_value() && b.has_value());
  ASSERT_EQ(a->c.size(), b->c.size());
  for (usize i = 0; i < a->c.size(); ++i) {
    EXPECT_EQ(std::bit_cast<std::uint64_t>(a->c[i]), std::bit_cast<std::uint64_t>(b->c[i]))
        << "non-bit-identical c at " << i;
  }
}

TEST(FundMetaAllocator, SolveIters_OneVsManyOnHardOmega_DifferProvingNoEarlyExit) {
  // A strongly-coupled Ω where CCD needs several sweeps to converge: 1 sweep and
  // 64 sweeps must give DIFFERENT w_rb (proving the count is honored, not an
  // early-exit that would make them identical).
  const std::vector<f64> sig{0.10, 0.40, 0.25};
  const MatX omega = const_corr(sig, 0.6);
  const auto vol = diag_vol(omega);

  MetaAllocatorConfig one = identity_kelly(RiskBudgetMethod::EqualRiskContribution);
  one.solve_iters = 1;
  MetaAllocatorConfig many = identity_kelly(RiskBudgetMethod::EqualRiskContribution);
  many.solve_iters = 64;

  const auto r1 = MetaAllocator{one}.allocate(omega, vol, huge_caps(3));
  const auto r64 = MetaAllocator{many}.allocate(omega, vol, huge_caps(3));
  ASSERT_TRUE(r1.has_value() && r64.has_value());

  bool any_diff = false;
  for (usize i = 0; i < r1->c.size(); ++i) {
    if (std::fabs(r1->c[i] - r64->c[i]) > 1e-9) {
      any_diff = true;
    }
  }
  EXPECT_TRUE(any_diff) << "1-iter == 64-iter ⇒ an early-exit shortcut, not a fixed count";
}

// ===========================================================================
//  4. InverseVol method
// ===========================================================================
TEST(FundMetaAllocator, InverseVol_AnyOmega_WeightsProportionalToInverseVol) {
  const std::vector<f64> sig{0.05, 0.10, 0.20};
  const MatX omega = const_corr(sig, 0.1);
  MetaAllocator alloc{identity_kelly(RiskBudgetMethod::InverseVol)};

  const auto res = alloc.allocate(omega, diag_vol(omega), huge_caps(3));
  ASSERT_TRUE(res.has_value());
  const auto &c = res->c;

  // c_i / c_j == (1/σ_i)/(1/σ_j) = σ_j/σ_i.
  EXPECT_NEAR(c[0] / c[1], sig[1] / sig[0], 1e-9);
  EXPECT_NEAR(c[1] / c[2], sig[2] / sig[1], 1e-9);
}

// ===========================================================================
//  5. HRP
// ===========================================================================
TEST(FundMetaAllocator, Hrp_TwoBlockCorrelated_PositiveNormalizedDiversified) {
  // Two correlated blocks {0,1} and {2,3}; within-block ρ=0.8, cross ρ=0.05.
  const std::vector<f64> sig{0.10, 0.12, 0.20, 0.18};
  MatX omega(4, 4);
  auto fill = [&](Eigen::Index i, Eigen::Index j, f64 rho) {
    const f64 si = sig[static_cast<usize>(i)];
    const f64 sj = sig[static_cast<usize>(j)];
    omega(i, j) = (i == j) ? si * si : rho * si * sj;
    omega(j, i) = omega(i, j);
  };
  for (Eigen::Index i = 0; i < 4; ++i) {
    omega(i, i) = sig[static_cast<usize>(i)] * sig[static_cast<usize>(i)];
  }
  fill(0, 1, 0.8);
  fill(2, 3, 0.8);
  fill(0, 2, 0.05);
  fill(0, 3, 0.05);
  fill(1, 2, 0.05);
  fill(1, 3, 0.05);

  MetaAllocator alloc{identity_kelly(RiskBudgetMethod::HierarchicalRiskParity)};
  const auto res = alloc.allocate(omega, diag_vol(omega), huge_caps(4));
  ASSERT_TRUE(res.has_value());
  const auto &c = res->c;
  ASSERT_EQ(c.size(), 4U);

  f64 sum = 0.0;
  for (const f64 v : c) {
    EXPECT_GT(v, 0.0);               // all strictly positive
    EXPECT_TRUE(std::isfinite(v));   // finite
    sum += v;
  }
  // c == w_rb here (identity Kelly): HRP normalizes to Σ = 1.
  EXPECT_NEAR(sum, 1.0, 1e-9);

  // --- structural HRP assertions (an impl with NO hierarchy would fail these) ---
  // (a) Intra-cluster split is INVERSE-VARIANCE: within each correlated block the two
  // members' weight ratio tracks (1/σ_i²)/(1/σ_j²) = σ_j²/σ_i². Both members of a block
  // share the SAME cluster-level split factor, so that factor cancels in the ratio —
  // making this a refactor-robust ratio check, not a magic constant.
  const f64 v0 = sig[0] * sig[0];
  const f64 v1 = sig[1] * sig[1];
  const f64 v2 = sig[2] * sig[2];
  const f64 v3 = sig[3] * sig[3];
  EXPECT_NEAR(c[0] / c[1], v1 / v0, 1e-9) << "block {0,1} intra-split not inverse-variance";
  EXPECT_NEAR(c[2] / c[3], v3 / v2, 1e-9) << "block {2,3} intra-split not inverse-variance";

  // (b) The HIGHER-variance block receives LESS total weight: block {2,3} (σ≈0.18–0.20)
  // is riskier than block {0,1} (σ≈0.10–0.12), so HRP's top-level inverse-variance
  // bisection tilts capital toward the lower-variance block.
  EXPECT_LT(c[2] + c[3], c[0] + c[1]) << "higher-variance block not down-weighted";
}

// ===========================================================================
//  6. Capacity box (§0.3) + gross cap
// ===========================================================================
TEST(FundMetaAllocator, CapacityBox_BindingCap_ClipsSleeveWeight) {
  const MatX omega = spd_3();
  const auto vol = diag_vol(omega);
  MetaAllocatorConfig cfg = identity_kelly(RiskBudgetMethod::InverseVol);
  cfg.max_gross = 1e9;
  MetaAllocator alloc{cfg};

  std::vector<f64> caps{1e9, 1e9, 1e9};
  caps[0] = 0.01; // bind sleeve 0 hard
  const auto res = alloc.allocate(omega, vol, caps);
  ASSERT_TRUE(res.has_value());
  EXPECT_LE(res->c[0], 0.01 + 1e-12);
}

TEST(FundMetaAllocator, MaxGross_Binding_ScalesGrossDownWithinCap) {
  const MatX omega = spd_3();
  const auto vol = diag_vol(omega);
  MetaAllocatorConfig cfg = identity_kelly(RiskBudgetMethod::EqualRiskContribution);
  cfg.fractional_kelly = 10.0; // push gross well past the cap
  cfg.max_gross = 1.5;
  MetaAllocator alloc{cfg};

  const auto res = alloc.allocate(omega, vol, huge_caps(3));
  ASSERT_TRUE(res.has_value());
  EXPECT_LE(gross(res->c), 1.5 + 1e-9);
}

TEST(FundMetaAllocator, ZeroAndNegativeCap_HardZeroSleeveOthersWithinGross) {
  const MatX omega = spd_3();
  const auto vol = diag_vol(omega);
  MetaAllocatorConfig cfg = identity_kelly(RiskBudgetMethod::InverseVol);
  cfg.max_gross = 2.0;
  MetaAllocator alloc{cfg};

  // caps_0 == 0 ⇒ that sleeve is hard-zeroed; caps_1 < 0 is treated as 0 (the
  // apply_kelly_caps negative/zero-cap branch). caps_2 is unbound.
  std::vector<f64> caps{0.0, -5.0, 1e9};
  const auto res = alloc.allocate(omega, vol, caps);
  ASSERT_TRUE(res.has_value());
  EXPECT_EQ(res->c[0], 0.0); // binding zero cap hard-zeros the sleeve
  EXPECT_EQ(res->c[1], 0.0); // negative cap clamped to 0
  EXPECT_GT(res->c[2], 0.0); // the unbound sleeve still gets capital
  EXPECT_LE(gross(res->c), 2.0 + 1e-9); // Σ|c| ≤ max_gross holds throughout
}

// ===========================================================================
//  7. Vol-target (A5)
// ===========================================================================
TEST(FundMetaAllocator, VolTarget_NoCapBinds_RealizesTargetFundVol) {
  const MatX omega = spd_3();
  const auto vol = diag_vol(omega);
  MetaAllocatorConfig cfg;
  cfg.method = RiskBudgetMethod::EqualRiskContribution;
  cfg.fractional_kelly = 1.0;
  cfg.target_vol = 0.05; // σ* = 5%
  cfg.max_gross = 1e9;
  MetaAllocator alloc{cfg};

  const auto res = alloc.allocate(omega, vol, huge_caps(3));
  ASSERT_TRUE(res.has_value());
  const f64 fund_vol = std::sqrt(quad_form(omega, res->c));
  EXPECT_NEAR(fund_vol, 0.05, 1e-6);
}

// ===========================================================================
//  8. Degenerate / empty fallback (§0.8)
// ===========================================================================
TEST(FundMetaAllocator, DegenerateOmega_NonFinite_FallsBackToInverseVolNoThrow) {
  MatX omega = spd_3();
  omega(1, 1) = std::numeric_limits<f64>::quiet_NaN(); // poison the diagonal
  std::vector<f64> vol{0.2, 0.3, 0.4};                 // usable vols for the fallback
  MetaAllocator alloc{identity_kelly(RiskBudgetMethod::EqualRiskContribution)};

  const auto res = alloc.allocate(omega, vol, huge_caps(3));
  ASSERT_TRUE(res.has_value()); // Ok, no throw, no peek
  // Inverse-vol fallback ⇒ c ∝ 1/σ.
  EXPECT_NEAR(res->c[0] / res->c[1], vol[1] / vol[0], 1e-9);
}

TEST(FundMetaAllocator, DegenerateOmega_ZeroDiagonal_FallsBackNoThrow) {
  MatX omega = spd_3();
  omega(2, 2) = 0.0; // a non-positive variance — ERC cannot proceed
  std::vector<f64> vol{0.2, 0.3, 0.4};
  MetaAllocator alloc{identity_kelly(RiskBudgetMethod::EqualRiskContribution)};
  const auto res = alloc.allocate(omega, vol, huge_caps(3));
  ASSERT_TRUE(res.has_value());
}

TEST(FundMetaAllocator, EmptyOmega_ZeroSleeves_ReturnsOkEmpty) {
  const MatX omega(0, 0);
  MetaAllocator alloc{identity_kelly(RiskBudgetMethod::EqualRiskContribution)};
  const auto res = alloc.allocate(omega, {}, {});
  ASSERT_TRUE(res.has_value());
  EXPECT_TRUE(res->c.empty());
}

// ===========================================================================
//  9. Single sleeve
// ===========================================================================
TEST(FundMetaAllocator, SingleSleeve_ErcTrivial_CapitalEqualsKelly) {
  MatX omega(1, 1);
  omega(0, 0) = 0.04; // σ = 0.2
  std::vector<f64> vol{0.2};
  MetaAllocatorConfig cfg = identity_kelly(RiskBudgetMethod::EqualRiskContribution);
  cfg.fractional_kelly = 0.3; // k = c·1 = 0.3 (no vol-target, no cap)
  MetaAllocator alloc{cfg};

  const auto res = alloc.allocate(omega, vol, huge_caps(1));
  ASSERT_TRUE(res.has_value());
  ASSERT_EQ(res->c.size(), 1U);
  // w_rb = [1] ⇒ c = clip(k·1, 0, cap) = 0.3.
  EXPECT_NEAR(res->c[0], 0.3, 1e-9);
}

// ===========================================================================
//  10. Errors
// ===========================================================================
TEST(FundMetaAllocator, Allocate_VolSizeMismatch_ReturnsInvalidArgument) {
  const MatX omega = spd_3();
  std::vector<f64> bad_vol{0.2, 0.3}; // size 2 ≠ S=3
  MetaAllocator alloc{identity_kelly(RiskBudgetMethod::EqualRiskContribution)};
  const auto res = alloc.allocate(omega, bad_vol, huge_caps(3));
  ASSERT_FALSE(res.has_value());
  EXPECT_EQ(res.error().code(), ErrorCode::InvalidArgument);
}

TEST(FundMetaAllocator, Allocate_RiskBudgetWrongSize_ReturnsInvalidArgument) {
  const MatX omega = spd_3();
  const auto vol = diag_vol(omega);
  MetaAllocatorConfig cfg = identity_kelly(RiskBudgetMethod::EqualRiskContribution);
  cfg.risk_budget = {0.5, 0.5}; // size 2 ≠ S=3
  MetaAllocator alloc{cfg};
  const auto res = alloc.allocate(omega, vol, huge_caps(3));
  ASSERT_FALSE(res.has_value());
  EXPECT_EQ(res.error().code(), ErrorCode::InvalidArgument);
}

TEST(FundMetaAllocator, Allocate_RiskBudgetNegative_ReturnsInvalidArgument) {
  const MatX omega = spd_3();
  const auto vol = diag_vol(omega);
  MetaAllocatorConfig cfg = identity_kelly(RiskBudgetMethod::EqualRiskContribution);
  cfg.risk_budget = {0.7, 0.6, -0.3}; // sums to 1 but has a negative
  MetaAllocator alloc{cfg};
  const auto res = alloc.allocate(omega, vol, huge_caps(3));
  ASSERT_FALSE(res.has_value());
  EXPECT_EQ(res.error().code(), ErrorCode::InvalidArgument);
}

TEST(FundMetaAllocator, Allocate_RiskBudgetNotSummingToOne_ReturnsInvalidArgument) {
  const MatX omega = spd_3();
  const auto vol = diag_vol(omega);
  MetaAllocatorConfig cfg = identity_kelly(RiskBudgetMethod::EqualRiskContribution);
  cfg.risk_budget = {0.5, 0.5, 0.5}; // Σ = 1.5
  MetaAllocator alloc{cfg};
  const auto res = alloc.allocate(omega, vol, huge_caps(3));
  ASSERT_FALSE(res.has_value());
  EXPECT_EQ(res.error().code(), ErrorCode::InvalidArgument);
}

// Config-scalar boundary checks: a negative gross/Kelly/vol-target or solve_iters==0
// each breaks a documented contract (notably max_gross < 0 would silently skip the
// gross cap) ⇒ Err(InvalidArgument). One test, four invalid configs.
TEST(FundMetaAllocator, Allocate_InvalidConfigScalars_ReturnInvalidArgument) {
  const MatX omega = spd_3();
  const auto vol = diag_vol(omega);

  const auto reject = [&](const MetaAllocatorConfig &cfg) {
    const auto res = MetaAllocator{cfg}.allocate(omega, vol, huge_caps(3));
    ASSERT_FALSE(res.has_value());
    EXPECT_EQ(res.error().code(), ErrorCode::InvalidArgument);
  };

  MetaAllocatorConfig neg_gross = identity_kelly(RiskBudgetMethod::EqualRiskContribution);
  neg_gross.max_gross = -1.0;
  reject(neg_gross);

  MetaAllocatorConfig neg_kelly = identity_kelly(RiskBudgetMethod::EqualRiskContribution);
  neg_kelly.fractional_kelly = -0.1;
  reject(neg_kelly);

  MetaAllocatorConfig neg_vol = identity_kelly(RiskBudgetMethod::EqualRiskContribution);
  neg_vol.target_vol = -0.05;
  reject(neg_vol);

  MetaAllocatorConfig zero_iters = identity_kelly(RiskBudgetMethod::EqualRiskContribution);
  zero_iters.solve_iters = 0U;
  reject(zero_iters);
}


}  // namespace atxtest_fund_meta_allocator_test
