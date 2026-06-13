#pragma once

// atx::engine::risk — dead-alpha → risk-factor extraction (Kakushadze & Yu, S7-3).
//
// ===========================================================================
//  What this unit is
// ===========================================================================
//  The numeric path that recycles RETIRED (dead) alphas into the risk model as
//  orthogonal risk FACTORS, so the operating book stops re-loading on directions
//  that have already decayed (the "dead-alpha → risk factor" recycling of
//  Kakushadze & Yu, arXiv:1709.06641). It does NOT estimate returns — it only
//  raises the variance the optimizer assigns to a dead alpha's holdings direction,
//  steering the book OFF those directions (the load-bearing R6 proof).
//
//  The pipeline is three FREE FUNCTIONS (no class — §4.3) over two carriers:
//    extract_dead_factors(lib, dead_ids, as_of, M) -> DeadAlphaFactors
//        Build the M×M holdings-overlap matrix X_AB = Σ_{i∈dead} P_iA P_iB over the
//        L1-normalized dead holdings at `as_of`, eigendecompose it (symmetric), fix
//        the sign convention, truncate to the effective rank, and emit the kept
//        eigenvector loadings + eigenvalues (the dead factor variances).
//    augment_factor_model(base, dead) -> FactorModel
//        V_aug = [X | X_dead]·blockdiag(F, diag(var_dead))·[.]ᵀ + D, reusing
//        FactorModel::create (no second covariance apply path).
//    effective_rank(evals) -> usize
//        round(exp(Shannon entropy of the normalized eigenvalue spectrum)) — the
//        Roy-Vetterli / Kakushadze eRank, used to truncate the kept factor count.
//
//  FactorComponents (the (X, F, D, fit_end) base) is defined in factor_model.hpp and
//  produced by FactorModelBuilder::build_components — this header INCLUDES that
//  header and uses the type (the chosen circular-include resolution: the carrier
//  lives next to the builder that returns it).
//
// ===========================================================================
//  Determinism (R1) — the extraction is BIT-REPRODUCIBLE
// ===========================================================================
//  * The overlap accumulation is ORDER-FIXED: dead_ids is iterated in the given
//    order (the caller passes ASCENDING AlphaId) and each rank-1 outer product is
//    summed in that order.
//  * The eigenpairs are SORTED DESCENDING by eigenvalue (a stable order index).
//  * Each eigenvector's sign is PINNED: the component with the largest absolute
//    value is forced POSITIVE (ties → lowest index wins). Eigen leaves the sign of
//    an eigenvector free, so this convention is what makes two identical extracts
//    byte-identical (R1) — without it the loadings could flip sign run to run.
//  * eRank reduces over an order-fixed ascending index.
//  NO RNG anywhere.
//
// ===========================================================================
//  PIT / no-survivorship (R4)
// ===========================================================================
//  A dead alpha's holdings cross-section at `as_of` may carry NaN cells (a name
//  that was delisted / had no opinion). l1_normalize_ignoring_nan maps NaN → 0, so
//  a dead/missing name contributes nothing to the overlap (it is not a fabricated
//  position). The holdings span ALIASES the library store's memtable/segment
//  memory (library::Library::positions' aliasing contract), so it is COPIED OUT
//  into the local VecX before any further store interaction — never held across a
//  store growth.
//
//  COLD path: the eigen-extraction allocates an M×M overlap + the eigensolver's
//  scratch. It runs at recycle cadence (a dead alpha is demoted infrequently), so a
//  per-extraction allocation is acceptable (documented).

#include <cmath>   // std::isnan, std::fabs, std::log, std::exp, std::llround
#include <span>    // std::span (dead-id list)
#include <utility> // std::move

#include <Eigen/Eigenvalues> // Eigen::SelfAdjointEigenSolver, Eigen::Success

#include "atx/core/error.hpp"  // Result, Ok, Err, ErrorCode
#include "atx/core/types.hpp"  // f64, usize, u32

#include "atx/core/linalg/linalg.hpp" // MatX, VecX (column-major Eigen)

#include "atx/engine/combine/store.hpp"      // combine::AlphaId
#include "atx/engine/library/library.hpp"    // library::Library (positions read-passthrough)
#include "atx/engine/risk/factor_model.hpp"  // FactorModel, FactorComponents

namespace atx::engine::risk {

// ===========================================================================
//  DeadAlphaFactors — the extracted dead-alpha risk factors.
//
//  loadings (M×K_dead): the sign-fixed, descending-ordered eigenvector columns of
//      the holdings-overlap matrix kept after eRank truncation. Each column is an
//      orthonormal dead direction in instrument space (a new exposure column for
//      the augmented factor model).
//  variances (K_dead): the corresponding kept eigenvalues — the diagonal of the
//      dead factor covariance block F_dead (the risk assigned to each dead
//      direction). An eigenvalue of the overlap matrix is the energy the dead pool
//      concentrates along that direction, so a heavily-traded dead direction gets a
//      large variance and the optimizer is steered hardest off it.
//  k_dead: round(eRank) of the eigenspectrum, clamped to [0, M].
// ===========================================================================
struct DeadAlphaFactors {
  atx::core::linalg::MatX loadings; // M×K_dead sign-fixed eigenvector columns
  atx::core::linalg::VecX variances; // K_dead kept eigenvalues (F_dead diagonal)
  atx::usize k_dead = 0U;            // = round(eRank), clamped to [0, M]
};

namespace detail {

// Copy a holdings cross-section span into a length-M VecX, mapping NaN → 0, then
// L1-normalize so Σ|v_i| == 1 (a degenerate all-zero / all-NaN holding stays the
// zero vector — no div-by-zero). The span ALIASES store memory, so it is COPIED OUT
// here BEFORE any further store interaction (R4 aliasing discipline). Order-fixed.
[[nodiscard]] inline atx::core::linalg::VecX
l1_normalize_ignoring_nan(std::span<const atx::f64> p, atx::usize m) {
  atx::core::linalg::VecX v = atx::core::linalg::VecX::Zero(static_cast<Eigen::Index>(m));
  const atx::usize n = (p.size() < m) ? p.size() : m; // defensive: never read past M
  atx::f64 s = 0.0;
  for (atx::usize i = 0U; i < n; ++i) {
    const atx::f64 x = p[i];
    const atx::f64 xi = std::isnan(x) ? 0.0 : x; // NaN holding → 0 (no fabricated position)
    v[static_cast<Eigen::Index>(i)] = xi;
    s += std::fabs(xi);
  }
  if (s > 0.0) {
    v /= s; // Σ|v_i| == 1
  }
  return v;
}

} // namespace detail

// ===========================================================================
//  effective_rank — the Roy-Vetterli / Kakushadze eRank of an eigenvalue spectrum.
//
//  round(exp(H)), H = −Σ_a p_a ln p_a the Shannon entropy of the normalized
//  spectrum p_a = max(λ_a,0)/Σ max(λ,0). It measures how many directions the dead
//  pool MEANINGFULLY spans (a pool concentrated on one direction → eRank ≈ 1; a
//  pool spread evenly over r directions → eRank ≈ r), so it is the natural
//  truncation count for the kept dead factors. Negative eigenvalues (numerical
//  noise of a PSD overlap) are floored to 0 and skipped. Order-fixed ascending
//  reduction; returns 0 for an empty / non-positive spectrum.
// ===========================================================================
[[nodiscard]] inline atx::usize effective_rank(const atx::core::linalg::VecX &evals) {
  atx::f64 sum = 0.0;
  for (Eigen::Index i = 0; i < evals.size(); ++i) {
    const atx::f64 e = evals[i];
    if (e > 0.0) {
      sum += e;
    }
  }
  if (sum <= 0.0) {
    return 0U;
  }
  atx::f64 h = 0.0; // Shannon entropy of the normalized spectrum
  for (Eigen::Index i = 0; i < evals.size(); ++i) {
    const atx::f64 e = evals[i] > 0.0 ? evals[i] : 0.0;
    if (e > 0.0) {
      const atx::f64 pa = e / sum;
      h -= pa * std::log(pa);
    }
  }
  const long long r = std::llround(std::exp(h));
  return r <= 0 ? 0U : static_cast<atx::usize>(r);
}

// ===========================================================================
//  extract_dead_factors — holdings-overlap eigen-extraction (§4.3, R1/R4).
//
//  Builds X_AB = Σ_{i∈dead} P_iA P_iB over the L1-normalized dead holdings at
//  `as_of_period` (Kakushadze & Yu 1709.06641), eigendecomposes the symmetric M×M
//  overlap, FIXES the sign convention (largest-|component| positive), truncates to
//  the effective rank, and returns the kept loadings + eigenvalues. `dead_ids` is
//  iterated in the GIVEN order — the caller passes ASCENDING AlphaId so the
//  accumulation is deterministic (R1). An EMPTY dead set yields k_dead == 0 (the
//  boundary). COLD path (allocates the M×M overlap + eigensolver scratch).
// ===========================================================================
[[nodiscard]] inline atx::core::Result<DeadAlphaFactors>
extract_dead_factors(const library::Library &lib, std::span<const combine::AlphaId> dead_ids,
                     atx::usize as_of_period, atx::usize universe_size) {
  if (universe_size == 0U) {
    return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                          "extract_dead_factors: universe_size must be > 0");
  }
  if (dead_ids.empty()) {
    return atx::core::Ok(DeadAlphaFactors{}); // boundary: no dead alphas → no factors
  }

  const Eigen::Index m = static_cast<Eigen::Index>(universe_size);
  atx::core::linalg::MatX overlap = atx::core::linalg::MatX::Zero(m, m);
  for (const combine::AlphaId id : dead_ids) { // ORDER-FIXED (ascending AlphaId, R1)
    // positions() aliases store memory; l1_normalize_ignoring_nan COPIES it out
    // (NaN→0) into `p` BEFORE we touch the store again (R4 aliasing discipline).
    const atx::core::linalg::VecX p =
        detail::l1_normalize_ignoring_nan(lib.positions(id, as_of_period), universe_size);
    overlap.noalias() += p * p.transpose(); // rank-1 outer product (order-fixed sum)
  }

  // Symmetric eigendecomposition (REUSE Eigen — no hand-rolled solver). Eigen
  // returns eigenvalues ASCENDING with orthonormal eigenvectors.
  Eigen::SelfAdjointEigenSolver<atx::core::linalg::MatX> es(overlap);
  if (es.info() != Eigen::Success) {
    return atx::core::Err(atx::core::ErrorCode::Internal,
                          "extract_dead_factors: eigensolver did not converge");
  }
  const atx::core::linalg::VecX &asc_vals = es.eigenvalues();
  const atx::core::linalg::MatX &asc_vecs = es.eigenvectors();

  // Reverse to DESCENDING (a fixed order index) so the kept leftCols are the
  // highest-energy dead directions.
  atx::core::linalg::VecX vals_desc(m);
  atx::core::linalg::MatX vecs_desc(m, m);
  for (Eigen::Index j = 0; j < m; ++j) {
    const Eigen::Index src = m - 1 - j;
    vals_desc[j] = asc_vals[src];
    vecs_desc.col(j) = asc_vecs.col(src);
  }

  // FIXED SIGN convention (R1): flip each eigenvector so its largest-|component| is
  // POSITIVE (ties → lowest index). Eigen leaves the sign free; this pins it so two
  // identical extracts are bit-identical.
  for (Eigen::Index j = 0; j < m; ++j) {
    Eigen::Index pivot = 0;
    atx::f64 best = -1.0;
    for (Eigen::Index r = 0; r < m; ++r) {
      const atx::f64 a = std::fabs(vecs_desc(r, j));
      if (a > best) { // strict > → lowest index wins a tie
        best = a;
        pivot = r;
      }
    }
    if (vecs_desc(pivot, j) < 0.0) {
      vecs_desc.col(j) = -vecs_desc.col(j);
    }
  }

  atx::usize k = effective_rank(vals_desc);
  if (k > universe_size) {
    k = universe_size; // clamp to [0, M]
  }
  const Eigen::Index kk = static_cast<Eigen::Index>(k);
  return atx::core::Ok(
      DeadAlphaFactors{vecs_desc.leftCols(kk), vals_desc.head(kk), k});
}

// ===========================================================================
//  augment_factor_model — V_aug = [X | X_dead]·blockdiag(F, diag(var_dead))·[.]ᵀ + D.
//
//  Hstacks the dead loadings onto the base exposures and blockdiag-extends F with
//  the dead variances, then REUSES FactorModel::create (the same factored Woodbury
//  apply path — no second covariance implementation). Raising a dead direction's
//  variance makes the optimizer assign it more risk, steering the book OFF that
//  direction (R6). A k_dead == 0 dead set is a passthrough (the base model). Err if
//  create rejects the augmented (X, F) (e.g. K_total exceeds the risk() stack bound).
// ===========================================================================
[[nodiscard]] inline atx::core::Result<FactorModel>
augment_factor_model(const FactorComponents &base, const DeadAlphaFactors &dead) {
  if (dead.k_dead == 0U) {
    // Passthrough: no dead factors → the base model verbatim.
    return FactorModel::create(base.X, base.F, base.D, /*fit_begin=*/0U, base.fit_end);
  }
  const Eigen::Index k_base = base.X.cols();
  const Eigen::Index k_dead = static_cast<Eigen::Index>(dead.k_dead);

  // hstack: Xa = [X | X_dead] (M × (K_base + K_dead)).
  atx::core::linalg::MatX xa(base.X.rows(), k_base + k_dead);
  xa << base.X, dead.loadings;

  // blockdiag: Fa = [[F, 0],[0, diag(var_dead)]] ((K_base+K_dead) square).
  atx::core::linalg::MatX fa = atx::core::linalg::MatX::Zero(xa.cols(), xa.cols());
  fa.topLeftCorner(base.F.rows(), base.F.cols()) = base.F;
  for (Eigen::Index j = 0; j < k_dead; ++j) {
    fa(k_base + j, k_base + j) = dead.variances[j];
  }
  return FactorModel::create(std::move(xa), std::move(fa), base.D, /*fit_begin=*/0U, base.fit_end);
}

} // namespace atx::engine::risk
