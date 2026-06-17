// risk_kkt_ldl_test.cpp — S8.2: the deterministic no-pivot quasi-definite LDLᵀ gate.
//
// QuasiDefiniteLdl (kkt_ldl.hpp) replaces the interim Eigen::SimplicialLDLT in the
// ADMM x-update with a hand-rolled QDLDL-style factorization over an AMD ordering.
// This file is the G-DIFF (differential correctness) + G-DET (determinism) gate
// (R5/R11):
//
//   1. Reconstruction (G-DIFF): ‖Pᵀ L D Lᵀ P − K‖∞ ≤ a tight ULP-scaled bound on a
//      battery of representative quasi-definite KKTs (built via the real
//      build_augmented + KKT-assembly path at small M,K AND hand-built synthetic
//      quasi-definite matrices). Inertia cross-check: the count of negative D entries
//      equals the number of constraint rows r — confirms the quasi-definite sign
//      structure survived with no pivoting reordering signs.
//   2. Solve accuracy: ‖K x − rhs‖∞ bounded for random RHS, cross-checked against a
//      dense Eigen::LDLT reference solve on the densified KKT (tight agreement).
//   3. Determinism (G-DET): the same K factored twice ⇒ byte-identical L, D, perm
//      (std::bit_cast<u64> on every stored double). And the full solve output is
//      byte-identical across Eigen thread-count environments {1,2,4,8} — the
//      factorization is purely serial so cross-run bit-identity is expected and
//      asserted.
//
// These are SMALL/FAST (dense reference on small KKTs) — well under a minute total.

#include <algorithm> // std::max
#include <array>     // std::array (sci() format buffer)
#include <bit>       // std::bit_cast (determinism)
#include <cmath>     // std::fabs, std::isfinite
#include <cstdint>   // std::uint64_t
#include <cstdio>    // std::snprintf (sci() formatter)
#include <random>    // std::mt19937_64 (FIXED seed)
#include <span>
#include <string>
#include <utility> // std::move
#include <vector>

#include <gtest/gtest.h>

#include <Eigen/Dense>      // Eigen::LDLT (dense reference oracle — test only)
#include <Eigen/SparseCore> // Eigen::SparseMatrix, Triplet

#include "atx/core/linalg/linalg.hpp"
#include "atx/core/types.hpp"

#include "atx/engine/risk/constraints.hpp"
#include "atx/engine/risk/factor_model.hpp"
#include "atx/engine/risk/kkt_ldl.hpp"
#include "atx/engine/risk/qp_augment.hpp"

namespace atxtest_risk_kkt_ldl_test {

using atx::f64;
using atx::usize;
using atx::core::linalg::MatX;
using atx::core::linalg::VecX;
using atx::engine::risk::AugmentedQp;
using atx::engine::risk::build_augmented;
using atx::engine::risk::ConstraintSet;
using atx::engine::risk::FactorExposure;
using atx::engine::risk::FactorModel;
using atx::engine::risk::MaterializedConstraints;
using atx::engine::risk::PositionCap;
using atx::engine::risk::QuasiDefiniteLdl;
using atx::engine::risk::TurnoverBudget;

using SpMat = Eigen::SparseMatrix<f64>;

// Scientific-notation formatter for RecordProperty. std::to_string(double) uses 6
// fixed decimals, so a ~1e-10 bound rounds to "0.000000" — a dead regression
// record. Format with enough significant digits to actually track drift.
[[nodiscard]] std::string sci(f64 v) {
  std::array<char, 32> buf{};
  std::snprintf(buf.data(), buf.size(), "%.3e", v);
  return std::string(buf.data());
}

// ---------------------------------------------------------------------------
//  KKT assembly mirror — identical structure/order to ConstrainedQpSolver::build_kkt
//  (qp_solver.hpp), reproduced here because that method is private. The KKT is the
//  symmetric quasi-definite  [[P+σI, Ãᵀ], [Ã, −ρ⁻¹I]]  with σ, ρ > 0.
// ---------------------------------------------------------------------------
[[nodiscard]] SpMat build_kkt(const AugmentedQp &aug, f64 sigma, f64 rho) {
  using Trip = Eigen::Triplet<f64>;
  const auto n = static_cast<int>(aug.n_w + aug.n_y + aug.n_aux);
  const int r = static_cast<int>(aug.A_tilde.rows());
  const int dim = n + r;
  const f64 neg_rho_inv = -1.0 / rho;

  std::vector<Trip> trips;
  trips.reserve(static_cast<usize>(aug.P.nonZeros() + 2 * aug.A_tilde.nonZeros() + dim));

  for (int c = 0; c < n; ++c) {
    bool diag_seen = false;
    for (SpMat::InnerIterator it(aug.P, c); it; ++it) {
      f64 v = it.value();
      if (it.row() == c) {
        v += sigma;
        diag_seen = true;
      }
      trips.emplace_back(it.row(), c, v);
    }
    if (!diag_seen) {
      trips.emplace_back(c, c, sigma);
    }
  }
  for (int c = 0; c < n; ++c) {
    for (SpMat::InnerIterator it(aug.A_tilde, c); it; ++it) {
      const int i = it.row();
      const f64 v = it.value();
      trips.emplace_back(n + i, c, v);
      trips.emplace_back(c, n + i, v);
    }
  }
  for (int i = 0; i < r; ++i) {
    trips.emplace_back(n + i, n + i, neg_rho_inv);
  }

  SpMat kkt(dim, dim);
  kkt.setFromTriplets(trips.begin(), trips.end());
  kkt.makeCompressed();
  return kkt;
}

// ---------------------------------------------------------------------------
//  Fixtures — a deterministic random FactorModel with NON-TRIVIAL SPD F. (Mirrors
//  the risk_qp_augment_test fixtures so the KKT battery exercises the real path.)
// ---------------------------------------------------------------------------
[[nodiscard]] MatX make_spd_f(usize k, std::mt19937_64 &rng) {
  const auto ek = static_cast<Eigen::Index>(k);
  std::uniform_real_distribution<f64> u(-0.5, 0.5);
  MatX b(ek, ek);
  for (Eigen::Index i = 0; i < ek; ++i) {
    for (Eigen::Index j = 0; j < ek; ++j) {
      b(i, j) = u(rng);
    }
  }
  MatX f = b * b.transpose();
  f += 0.05 * MatX::Identity(ek, ek);
  return f;
}

[[nodiscard]] FactorModel make_random_model(usize m, usize k, std::mt19937_64 &rng) {
  const auto em = static_cast<Eigen::Index>(m);
  const auto ek = static_cast<Eigen::Index>(k);
  std::uniform_real_distribution<f64> ux(-1.0, 1.0);
  std::uniform_real_distribution<f64> ud(0.05, 0.30);
  MatX x(em, ek);
  for (Eigen::Index i = 0; i < em; ++i) {
    for (Eigen::Index j = 0; j < ek; ++j) {
      x(i, j) = ux(rng);
    }
  }
  const MatX f = make_spd_f(k, rng);
  VecX d(em);
  for (Eigen::Index i = 0; i < em; ++i) {
    d[i] = ud(rng);
  }
  auto r = FactorModel::create(std::move(x), f, std::move(d), 0U, 10U);
  EXPECT_TRUE(r.has_value()) << (r ? "" : r.error().to_string());
  return std::move(*r);
}

// Build a representative quasi-definite KKT from the real augmentation path. Returns
// (kkt, r) where r is the number of constraint rows (= the −ρ⁻¹I block dimension =
// the expected negative inertia).
struct KktCase {
  SpMat kkt;
  usize r; // constraint rows (expected count of negative D pivots)
  usize n; // primal block dimension
};

[[nodiscard]] KktCase make_kkt_from_model(usize m, usize k, bool gross, bool turn, bool box,
                                          bool fexp, unsigned seed, f64 sigma, f64 rho) {
  std::mt19937_64 rng(seed);
  const FactorModel model = make_random_model(m, k, rng);
  std::vector<f64> q(m, 0.0);
  std::vector<f64> w_prev(m, 0.0);

  ConstraintSet cs;
  cs.gross.dollar_neutral = true;
  if (box) {
    cs.pos = PositionCap{0.6};
  }
  if (fexp) {
    const usize nf = k >= 2U ? 2U : 1U;
    std::vector<usize> cols;
    std::vector<f64> bnd;
    for (usize j = 0; j < nf; ++j) {
      cols.push_back(j);
      bnd.push_back(0.8);
    }
    cs.fexp = FactorExposure{std::move(cols), std::move(bnd)};
  }
  if (gross) {
    cs.gross.gross_leverage = 1.5;
  }
  if (turn) {
    cs.turn = TurnoverBudget{1.2};
  }
  auto mcr = cs.materialize(model.exposures(), std::span<const f64>(w_prev), m);
  EXPECT_TRUE(mcr.has_value()) << (mcr ? "" : mcr.error().to_string());
  MaterializedConstraints mc = std::move(*mcr);
  if (!gross) {
    mc.gross_l1_budget = -1.0;
  }

  const AugmentedQp aug = build_augmented(model, 0.6, std::span<const f64>(q), mc);
  KktCase out;
  out.n = aug.n_w + aug.n_y + aug.n_aux;
  out.r = static_cast<usize>(aug.A_tilde.rows());
  out.kkt = build_kkt(aug, sigma, rho);
  return out;
}

// A hand-built synthetic symmetric quasi-definite matrix  [[E, Bᵀ], [B, −G]]  with
// E, G ≻ 0 diagonal-dominant. Returns (kkt, n_pos_block, n_neg_block).
[[nodiscard]] KktCase make_synthetic_qdef(usize np, usize nn, unsigned seed) {
  std::mt19937_64 rng(seed);
  std::uniform_real_distribution<f64> ub(-1.0, 1.0);
  const usize dim = np + nn;
  MatX dense = MatX::Zero(static_cast<Eigen::Index>(dim), static_cast<Eigen::Index>(dim));
  // E block (PD): diagonal 3 + small symmetric off-diagonals.
  for (usize i = 0; i < np; ++i) {
    dense(static_cast<Eigen::Index>(i), static_cast<Eigen::Index>(i)) = 3.0;
    for (usize j = i + 1; j < np; ++j) {
      const f64 v = 0.2 * ub(rng);
      dense(static_cast<Eigen::Index>(i), static_cast<Eigen::Index>(j)) = v;
      dense(static_cast<Eigen::Index>(j), static_cast<Eigen::Index>(i)) = v;
    }
  }
  // −G block (ND): diagonal −2.
  for (usize i = 0; i < nn; ++i) {
    dense(static_cast<Eigen::Index>(np + i), static_cast<Eigen::Index>(np + i)) = -2.0;
  }
  // B off-diagonal coupling (dense-ish, |B_ij| small).
  for (usize i = 0; i < nn; ++i) {
    for (usize j = 0; j < np; ++j) {
      const f64 v = 0.5 * ub(rng);
      dense(static_cast<Eigen::Index>(np + i), static_cast<Eigen::Index>(j)) = v;
      dense(static_cast<Eigen::Index>(j), static_cast<Eigen::Index>(np + i)) = v;
    }
  }
  KktCase out;
  out.n = np;
  out.r = nn;
  out.kkt = dense.sparseView();
  out.kkt.makeCompressed();
  return out;
}

// ‖A‖∞ (max abs entry) of a dense matrix.
[[nodiscard]] f64 max_abs(const MatX &a) {
  f64 m = 0.0;
  for (Eigen::Index i = 0; i < a.rows(); ++i) {
    for (Eigen::Index j = 0; j < a.cols(); ++j) {
      m = std::max(m, std::fabs(a(i, j)));
    }
  }
  return m;
}

// ===========================================================================
//  1. Reconstruction (G-DIFF) + inertia cross-check on the real augmented KKTs.
// ===========================================================================
TEST(RiskKktLdl, ReconstructsAugmentedKktAndInertia) {
  struct Spec {
    usize m, k;
    bool gross, turn, box, fexp;
    unsigned seed;
  };
  const std::vector<Spec> battery = {
      {6U, 2U, false, false, true, false, 101U},
      {8U, 3U, false, false, true, true, 102U},
      {10U, 4U, true, false, true, true, 103U},
      {8U, 3U, false, true, true, false, 104U},
      {12U, 4U, true, true, true, true, 105U},
      {6U, 4U, false, false, false, true, 106U},
  };

  const f64 sigma = 1e-6;
  const f64 rho = 1.0;
  f64 worst_rel = 0.0;

  for (const Spec &s : battery) {
    const KktCase kc = make_kkt_from_model(s.m, s.k, s.gross, s.turn, s.box, s.fexp, s.seed,
                                           sigma, rho);
    QuasiDefiniteLdl ldl;
    ASSERT_TRUE(ldl.factor_symbolic(kc.kkt).has_value());
    ASSERT_TRUE(ldl.factor_numeric(kc.kkt).has_value());

    // Reconstruction: ‖Pᵀ L D Lᵀ P − K‖∞.
    const MatX recon = ldl.reconstruct();
    const MatX dense_k = MatX(kc.kkt);
    const f64 diff = max_abs(recon - dense_k);
    const f64 scale = std::max(1.0, max_abs(dense_k));
    const f64 rel = diff / scale;
    worst_rel = std::max(worst_rel, rel);
    // ULP-scaled reconstruction bound. The real augmented KKT carries σ=1e-6 pivots
    // on the aux diagonal, so κ(K) ~ 1e6 and the LDLᵀ reconstruction error scales
    // with it: the measured battery-wide worst ‖recon−K‖∞/‖K‖∞ is ~2.3e-10. The gate
    // is set at 1e-9 — modest headroom above the achieved bound and far tighter than
    // any solver-meaningful tolerance. (The well-conditioned synthetic battery below
    // reconstructs to 1e-12; this looser bound is purely the σ-conditioning of the
    // regularized KKT, not a factorization defect — the solve residual is ≤1e-9.)
    EXPECT_LE(rel, 1e-9) << "M=" << s.m << " K=" << s.k << " ‖recon−K‖∞/scale=" << rel;

    // Inertia: the count of negative D entries must equal r (the # of constraint
    // rows / the −ρ⁻¹I block). No pivoting ⇒ the sign pattern is exactly preserved.
    EXPECT_EQ(ldl.negative_inertia(), kc.r)
        << "M=" << s.m << " K=" << s.k << " expected " << kc.r << " negative pivots";
  }
  RecordProperty("worst_recon_rel", sci(worst_rel));
}

// ===========================================================================
//  2. Reconstruction + inertia on hand-built synthetic quasi-definite matrices.
// ===========================================================================
TEST(RiskKktLdl, ReconstructsSyntheticQuasiDefinite) {
  struct Spec {
    usize np, nn;
    unsigned seed;
  };
  const std::vector<Spec> battery = {
      {4U, 2U, 201U}, {6U, 3U, 202U}, {3U, 5U, 203U}, {8U, 4U, 204U}, {1U, 1U, 205U},
  };
  for (const Spec &s : battery) {
    const KktCase kc = make_synthetic_qdef(s.np, s.nn, s.seed);
    QuasiDefiniteLdl ldl;
    ASSERT_TRUE(ldl.factor_symbolic(kc.kkt).has_value());
    ASSERT_TRUE(ldl.factor_numeric(kc.kkt).has_value());

    const MatX recon = ldl.reconstruct();
    const MatX dense_k = MatX(kc.kkt);
    const f64 rel = max_abs(recon - dense_k) / std::max(1.0, max_abs(dense_k));
    EXPECT_LE(rel, 1e-12) << "np=" << s.np << " nn=" << s.nn << " rel=" << rel;
    EXPECT_EQ(ldl.negative_inertia(), s.nn)
        << "np=" << s.np << " nn=" << s.nn << " expected " << s.nn << " negative pivots";
  }
}

// ===========================================================================
//  3. Solve accuracy — ‖K x − rhs‖∞ bounded; cross-check vs dense Eigen::LDLT.
// ===========================================================================
TEST(RiskKktLdl, SolveMatchesDenseLdltReference) {
  struct Spec {
    usize m, k;
    bool gross, turn, box, fexp;
    unsigned seed;
  };
  const std::vector<Spec> battery = {
      {6U, 2U, false, false, true, false, 301U},
      {10U, 4U, true, false, true, true, 302U},
      {8U, 3U, false, true, true, true, 303U},
      {12U, 4U, true, true, true, true, 304U},
  };
  const f64 sigma = 1e-6;
  const f64 rho = 1.0;
  f64 worst_resid = 0.0;
  f64 worst_dense_gap = 0.0;

  for (const Spec &s : battery) {
    const KktCase kc = make_kkt_from_model(s.m, s.k, s.gross, s.turn, s.box, s.fexp, s.seed,
                                           sigma, rho);
    const Eigen::Index dim = kc.kkt.rows();

    QuasiDefiniteLdl ldl;
    ASSERT_TRUE(ldl.factor_symbolic(kc.kkt).has_value());
    ASSERT_TRUE(ldl.factor_numeric(kc.kkt).has_value());

    // A deterministic RHS (fixed seed per case).
    std::mt19937_64 rng(static_cast<std::uint64_t>(s.seed) ^ 0x12345u);
    std::uniform_real_distribution<f64> ur(-1.0, 1.0);
    std::vector<f64> rhs(static_cast<usize>(dim));
    for (usize i = 0; i < rhs.size(); ++i) {
      rhs[i] = ur(rng);
    }

    std::vector<f64> x(static_cast<usize>(dim), 0.0);
    ldl.solve(std::span<const f64>(rhs), std::span<f64>(x));

    // Residual ‖K x − rhs‖∞ via the sparse matvec (fixed CSC traversal).
    const VecX xv = Eigen::Map<const VecX>(x.data(), dim);
    const VecX rv = Eigen::Map<const VecX>(rhs.data(), dim);
    const VecX resid = kc.kkt * xv - rv;
    f64 r_inf = 0.0;
    for (Eigen::Index i = 0; i < resid.size(); ++i) {
      r_inf = std::max(r_inf, std::fabs(resid[i]));
    }
    worst_resid = std::max(worst_resid, r_inf);
    EXPECT_LE(r_inf, 1e-9) << "M=" << s.m << " K=" << s.k << " ‖Kx−rhs‖∞=" << r_inf;

    // Dense Eigen::LDLT reference solve on the densified KKT — agree tightly.
    const MatX dense_k = MatX(kc.kkt);
    const VecX xref = dense_k.ldlt().solve(rv);
    f64 gap = 0.0;
    for (Eigen::Index i = 0; i < xref.size(); ++i) {
      gap = std::max(gap, std::fabs(xv[i] - xref[i]));
    }
    worst_dense_gap = std::max(worst_dense_gap, gap);
    EXPECT_LE(gap, 1e-9) << "M=" << s.m << " K=" << s.k << " ‖x − x_dense‖∞=" << gap;
  }
  RecordProperty("worst_resid", sci(worst_resid));
  RecordProperty("worst_dense_gap", sci(worst_dense_gap));
}

// ===========================================================================
//  4. Determinism (G-DET): same K factored twice ⇒ byte-identical L, D, perm.
// ===========================================================================
TEST(RiskKktLdl, FactorIsByteIdenticalAcrossRuns) {
  const KktCase kc = make_kkt_from_model(10U, 4U, true, true, true, true, 401U, 1e-6, 1.0);

  QuasiDefiniteLdl a;
  QuasiDefiniteLdl b;
  ASSERT_TRUE(a.factor_symbolic(kc.kkt).has_value());
  ASSERT_TRUE(a.factor_numeric(kc.kkt).has_value());
  ASSERT_TRUE(b.factor_symbolic(kc.kkt).has_value());
  ASSERT_TRUE(b.factor_numeric(kc.kkt).has_value());

  // perm byte-identical.
  ASSERT_EQ(a.perm().size(), b.perm().size());
  for (usize i = 0; i < a.perm().size(); ++i) {
    EXPECT_EQ(a.perm()[i], b.perm()[i]) << "perm[" << i << "]";
  }
  // L structure + values byte-identical.
  ASSERT_EQ(a.Lp().size(), b.Lp().size());
  for (usize i = 0; i < a.Lp().size(); ++i) {
    EXPECT_EQ(a.Lp()[i], b.Lp()[i]) << "Lp[" << i << "]";
  }
  ASSERT_EQ(a.Li().size(), b.Li().size());
  for (usize i = 0; i < a.Li().size(); ++i) {
    EXPECT_EQ(a.Li()[i], b.Li()[i]) << "Li[" << i << "]";
  }
  ASSERT_EQ(a.Lx().size(), b.Lx().size());
  for (usize i = 0; i < a.Lx().size(); ++i) {
    EXPECT_EQ(std::bit_cast<std::uint64_t>(a.Lx()[i]), std::bit_cast<std::uint64_t>(b.Lx()[i]))
        << "Lx[" << i << "]";
  }
  // D byte-identical.
  ASSERT_EQ(a.diag().size(), b.diag().size());
  for (usize i = 0; i < a.diag().size(); ++i) {
    EXPECT_EQ(std::bit_cast<std::uint64_t>(a.diag()[i]), std::bit_cast<std::uint64_t>(b.diag()[i]))
        << "D[" << i << "]";
  }
}

// Re-factoring numerically over an already-built symbolic (the cache seam) yields a
// byte-identical factor to a cold (symbolic+numeric) factor — proves the
// symbolic/numeric split does not perturb the result.
TEST(RiskKktLdl, WarmSymbolicMatchesColdFactorByteForByte) {
  const KktCase kc = make_kkt_from_model(8U, 3U, true, false, true, true, 402U, 1e-6, 1.0);

  QuasiDefiniteLdl cold;
  ASSERT_TRUE(cold.factor_symbolic(kc.kkt).has_value());
  ASSERT_TRUE(cold.factor_numeric(kc.kkt).has_value());

  QuasiDefiniteLdl warm;
  ASSERT_TRUE(warm.factor_symbolic(kc.kkt).has_value());
  // Re-run numeric a second time over the same symbolic structure.
  ASSERT_TRUE(warm.factor_numeric(kc.kkt).has_value());
  ASSERT_TRUE(warm.factor_numeric(kc.kkt).has_value());

  ASSERT_EQ(cold.Lx().size(), warm.Lx().size());
  for (usize i = 0; i < cold.Lx().size(); ++i) {
    EXPECT_EQ(std::bit_cast<std::uint64_t>(cold.Lx()[i]),
              std::bit_cast<std::uint64_t>(warm.Lx()[i]))
        << "Lx[" << i << "]";
  }
  for (usize i = 0; i < cold.diag().size(); ++i) {
    EXPECT_EQ(std::bit_cast<std::uint64_t>(cold.diag()[i]),
              std::bit_cast<std::uint64_t>(warm.diag()[i]))
        << "D[" << i << "]";
  }
}

// ===========================================================================
//  5. Determinism across Eigen thread-count environments {1,2,4,8} (G-DET).
//     The factorization is purely serial; we still assert the solve output digest
//     is bit-identical regardless of how many threads Eigen is permitted to use.
// ===========================================================================
TEST(RiskKktLdl, SolveByteIdenticalAcrossThreadCounts) {
  const KktCase kc = make_kkt_from_model(12U, 4U, true, true, true, true, 501U, 1e-6, 1.0);
  const Eigen::Index dim = kc.kkt.rows();

  std::mt19937_64 rng(0x515Eu);
  std::uniform_real_distribution<f64> ur(-1.0, 1.0);
  std::vector<f64> rhs(static_cast<usize>(dim));
  for (usize i = 0; i < rhs.size(); ++i) {
    rhs[i] = ur(rng);
  }

  // Digest the solve output under a given Eigen thread count.
  const auto digest_under_threads = [&](int nthreads) -> std::vector<std::uint64_t> {
    Eigen::setNbThreads(nthreads);
    QuasiDefiniteLdl ldl;
    EXPECT_TRUE(ldl.factor_symbolic(kc.kkt).has_value());
    EXPECT_TRUE(ldl.factor_numeric(kc.kkt).has_value());
    std::vector<f64> x(static_cast<usize>(dim), 0.0);
    ldl.solve(std::span<const f64>(rhs), std::span<f64>(x));
    std::vector<std::uint64_t> bits(x.size());
    for (usize i = 0; i < x.size(); ++i) {
      bits[i] = std::bit_cast<std::uint64_t>(x[i]);
    }
    return bits;
  };

  const std::vector<std::uint64_t> ref = digest_under_threads(1);
  for (const int t : {2, 4, 8}) {
    const std::vector<std::uint64_t> got = digest_under_threads(t);
    ASSERT_EQ(ref.size(), got.size());
    for (usize i = 0; i < ref.size(); ++i) {
      EXPECT_EQ(ref[i], got[i]) << "threads=" << t << " element " << i;
    }
  }
  Eigen::setNbThreads(1); // restore a deterministic default for the rest of the suite
}

// ===========================================================================
//  6. Negative path — a non-quasi-definite (indefinite-with-zero-pivot) matrix is
//     reported, not silently mis-factored. A 2×2 [[0,1],[1,0]] hits a zero pivot.
// ===========================================================================
TEST(RiskKktLdl, ZeroPivotIsReported) {
  MatX dense = MatX::Zero(2, 2);
  dense(0, 1) = 1.0;
  dense(1, 0) = 1.0;
  SpMat k = dense.sparseView();
  k.makeCompressed();

  QuasiDefiniteLdl ldl;
  ASSERT_TRUE(ldl.factor_symbolic(k).has_value());
  const auto r = ldl.factor_numeric(k);
  EXPECT_FALSE(r.has_value()) << "a zero leading pivot must be reported, not divided by";
}

} // namespace atxtest_risk_kkt_ldl_test
