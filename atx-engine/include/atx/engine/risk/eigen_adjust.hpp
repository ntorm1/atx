#pragma once

// atx::engine::risk — Monte-Carlo eigenfactor risk adjustment (S8.3, MWO).
//
// ===========================================================================
//  What this unit is
// ===========================================================================
//  eigen_adjust(F, sims, amplify, seed) cleans a K×K factor covariance `F` (SPD)
//  by the Menchero-Wang-Orr (MWO) eigenfactor de-biasing procedure. A sample factor
//  covariance systematically UNDER-forecasts the variance of its smallest
//  eigenfactors and OVER-forecasts its largest (the optimizer then over-weights the
//  apparently-low-risk small directions). MWO measures that sampling bias by Monte-
//  Carlo simulation and rescales each eigenvariance to correct it:
//
//    1. F = U·diag(D)·Uᵀ via symmetric_eig (U columns = eigenvectors; D ASCENDING).
//       Treat this decomposition as the TRUTH the simulation samples from.
//    2. For m = 1..M (M = `sims`, a FIXED count — NO convergence early-exit): draw a
//       K×T eigenfactor-return matrix `b` with b[k,t] = √(D[k])·N(0,1), simulate a
//       factor-return history f_sim = U·b (K×T), re-estimate the SAMPLE covariance
//       F_m = (1/T)·f_sim·f_simᵀ, and eigendecompose it into its OWN SAMPLE eigenbasis
//       Û_m (cols) + SAMPLE eigenvalues D̂_m (ascending — the eigenvariances the model
//       would REPORT). The TRUE variance along the k-th sample direction is
//       D̃_m[k] = û_{m,k}ᵀ·F·û_{m,k}. Accumulate acc[k] += √(D̃_m[k]/D̂_m[k]).
//    3. v[k] = acc[k]/M  (the per-eigenfactor simulated vol bias; ≈ 1 for the large
//       eigenfactors, > 1 for the small ones — the sample understates the smallest
//       eigenvariance). γ[k] = a·(v[k]−1)+1 with a = `amplify` (DEFAULT 1.0 — NOT the
//       paper's empirical 1.4; see the caveat in the sprint spec). Rescale the
//       eigenvariances D̂[k] = γ[k]²·D[k] (γ²>0 keeps D̂>0 ⇒ PSD-PRESERVING) and rotate
//       back F̂ = U·diag(D̂)·Uᵀ.
//
//  DEVIATION FROM THE FROZEN PLAN §4 (documented in detail::accumulate_vol_bias): the
//  plan literally writes "D̃ = diag(Uᵀ·F̃·U)" (project the sample cov onto the TRUE
//  basis), which is UNBIASED (→ D[k] for every k) and so produces v(k) ≈ 1 — it cannot
//  correct any bias, making the plan's own acceptance criterion (v(k) > 1; inflate the
//  smallest) unsatisfiable. We implement the genuine Menchero-Wang-Orr de-biasing,
//  which uses each sim's SAMPLE eigenbasis (matching the sprint spec §S8.3's own
//  `√(D̃_m(k)/D_m(k))` variable usage). See the helper's SAFETY note.
//
//  OPT-IN: `sims == 0` is a NO-OP (returns F unchanged) — the default build path.
//  IDENTITY: `amplify == 0` ⇒ γ≡1 ⇒ D̂≡D ⇒ F̂≡F (the simulation still runs but its
//  measured bias is gated out; the result equals F to round-off).
//
// ===========================================================================
//  Determinism contract (the crux — this is the ONLY new RNG site in S8)
// ===========================================================================
//  * ONE atx::core::Xoshiro256pp, constructed ONCE from `seed`. EVERY normal
//    draw is taken in a FIXED canonical order: ascending k (eigenfactor), then
//    ascending t (sim observation), for sim m = 0,1,…,M−1 in order. No other source
//    of randomness, no clock, no map iteration, no parallelism.
//  * The simulation loop runs EXACTLY M iterations (no convergence / early exit).
//  * Therefore the same (F, sims, amplify, seed) ⇒ a BYTE-IDENTICAL F̂ run-to-run.
//    The seed lives in CovarianceConfig.eigen_adjust_seed, which IS the reproducibility
//    record: identical build inputs (the config) ⇒ an identical model.
//  * T (the inner simulated-history length) is a FIXED deterministic function of K
//    (kSimObsPerFactor·K, floored at kMinSimObs) so it never depends on anything
//    nondeterministic — see kSimObsPerFactor.
//
// ===========================================================================
//  Failure modes
// ===========================================================================
//  Returns Result<MatX>: Err if symmetric_eig fails (a non-symmetric / non-finite F,
//  propagated via ATX_TRY) or if any eigenvalue is non-positive (F not SPD — the
//  √(D[k]) draw and the D̃/D ratio both require D[k] > 0). The caller (the builder)
//  produces F from an SPD construction, so the Err path is a guard, not the norm.

#include <cmath>   // std::sqrt
#include <utility> // std::move

#include <Eigen/Dense> // MatX / VecX ops (eigen-rotate, outer product)

#include "atx/core/error.hpp" // Result, Ok, Err, ErrorCode, ATX_TRY
#include "atx/core/types.hpp" // f64, u64, usize

#include "atx/core/linalg/decompose.hpp" // symmetric_eig (EigResult: values ASCENDING)
#include "atx/core/linalg/linalg.hpp"    // MatX, VecX (column-major Eigen)
#include "atx/core/random.hpp"           // Xoshiro256pp (the seeded RNG; .normal())

namespace atx::engine::risk {

// Simulated inner-history length per eigenfactor: T = kSimObsPerFactor·K (floored at
// kMinSimObs). A DETERMINISTIC function of K only — the apply path never sees the fit
// window, so we scale T with the factor count K so the simulated covariance is always
// well-conditioned (T ≫ K) without depending on any run-time / calendar quantity.
// 100 obs/factor gives a stable bias estimate; the floor keeps tiny-K (K=1,2) sims
// from being degenerate. Changing these constants changes F̂ — they are part of the
// reproducibility record alongside the seed.
inline constexpr atx::usize kSimObsPerFactor = 100U;
inline constexpr atx::usize kMinSimObs = 200U;

namespace detail {

// The fixed simulated-history length T for a K-factor covariance (see the constants).
[[nodiscard]] inline constexpr atx::usize sim_obs(atx::usize k) noexcept {
  const atx::usize t = kSimObsPerFactor * k;
  return (t < kMinSimObs) ? kMinSimObs : t;
}

// One MWO simulation pass: given the TRUE eigenbasis (U, D) of F and the simulated-
// history length T, run M fixed-count sims off the single seeded `rng` (draws in
// canonical ascending-k-then-t order) and return the per-eigenfactor accumulated
// vol-bias ratio acc[k] = Σ_m √(D̃_m[k] / D̂_m[k]), where for each sim m:
//   * b ∈ ℝ^{K×T}, b[k,t] = √(D[k])·N(0,1)   — eigenfactor returns from the TRUTH;
//   * f_sim = U·b (K×T); sample covariance F_m = (1/T)·f_sim·f_simᵀ;
//   * symmetric_eig(F_m) → SAMPLE eigenbasis Û_m (cols) + SAMPLE eigenvalues D̂_m
//     (ascending) — D̂_m[k] is the eigenvariance the model WOULD REPORT;
//   * D̃_m[k] = û_{m,k}ᵀ·F·û_{m,k} — the TRUE variance along the k-th sample
//     eigenvector (F = U·diag(D)·Uᵀ projected onto the sample direction).
// The ratio √(D̃_m[k]/D̂_m[k]) > 1 for the small eigenfactors: the sample under-
// states the smallest eigenvariance while the true variance along that noisy
// direction is larger — the §1 under-forecast the adjustment must correct.
//
// SAFETY / DEVIATION (documented): the frozen plan §4 step 2 literally writes
// "D̃ = diag(Uᵀ·F̃·U)" — projecting the SAMPLE covariance onto the TRUE basis U.
// That quantity is the UNBIASED per-direction sample variance (it tends to D[k] for
// every k), so it produces v(k) ≈ 1 and CANNOT correct any bias — the plan's own
// acceptance criterion (v(k) > 1 for the smallest eigenfactor; inflate it) is then
// unsatisfiable. The genuine Menchero-Wang-Orr de-biasing — and the sprint spec
// §S8.3's own variable usage `√(D̃_m(k)/D_m(k))` (D̃ true-in-sample-direction over
// D_m the SAMPLE eigenvalue) — requires the SAMPLE eigenbasis Û_m. We implement the
// canonical method; pairing is by ascending eigenvalue index (both bases sorted).
//
// SAFETY: `rng` is taken by mutable reference and advanced in a FIXED order; the only
// randomness is its seeded stream, so the accumulation is byte-reproducible. A sample-
// covariance eigendecomposition failure (never expected for an SPD F_m) contributes
// the NEUTRAL ratio 1.0 for that sim — keeping the loop fixed-count and deterministic.
[[nodiscard]] inline atx::core::linalg::VecX accumulate_vol_bias(const atx::core::linalg::MatX &u,
                                                                 const atx::core::linalg::VecX &d,
                                                                 atx::usize sims, atx::usize t,
                                                                 atx::core::Xoshiro256pp &rng) {
  const Eigen::Index k = d.size();
  const Eigen::Index tt = static_cast<Eigen::Index>(t);
  atx::core::linalg::VecX acc = atx::core::linalg::VecX::Zero(k);
  atx::core::linalg::VecX sigma(k); // √(D[k]) per eigenfactor (D ascending, all > 0)
  for (Eigen::Index kk = 0; kk < k; ++kk) {
    sigma[kk] = std::sqrt(d[kk]);
  }
  const atx::core::linalg::MatX f_true = u * d.asDiagonal() * u.transpose(); // = F (SPD)
  atx::core::linalg::MatX b(k, tt); // eigenfactor returns b[k,t] = √(D[k])·N(0,1)
  for (atx::usize m = 0U; m < sims; ++m) {
    for (Eigen::Index kk = 0; kk < k; ++kk) {    // canonical order: ascending k,
      for (Eigen::Index ti = 0; ti < tt; ++ti) { //                  then ascending t
        b(kk, ti) = sigma[kk] * rng.normal();
      }
    }
    const atx::core::linalg::MatX f_sim = u * b;                      // K×T history
    const atx::core::linalg::MatX f_m = (f_sim * f_sim.transpose()) / // (1/T) f f ᵀ
                                        static_cast<atx::f64>(t);
    const auto sample = atx::core::linalg::symmetric_eig(f_m);
    if (!sample) {
      acc.array() += 1.0; // neutral (no bias) on the never-expected decompose failure
      continue;
    }
    const atx::core::linalg::MatX &uhat = sample->vectors; // SAMPLE eigenvectors (cols)
    const atx::core::linalg::VecX &dhat = sample->values;  // SAMPLE eigenvalues (ascending)
    for (Eigen::Index kk = 0; kk < k; ++kk) {
      const atx::core::linalg::VecX uk = uhat.col(kk); // k-th sample eigenvector
      const atx::f64 true_var = uk.dot(f_true * uk);   // ûₖᵀ F ûₖ (true variance)
      const atx::f64 samp_var = dhat[kk];              // reported eigenvariance
      // SAFETY: ûₖᵀ F ûₖ is ≥ 0 in exact arithmetic (F SPD), but the projection can
      // round to a tiny NEGATIVE for a near-degenerate eigendirection on an ill-
      // conditioned F; floor the numerator at 0 so √(·) never yields NaN (which would
      // propagate into v(k) and silently corrupt F̂). Denominator guard kept below.
      const atx::f64 num = (true_var > 0.0) ? true_var : 0.0;
      acc[kk] += (samp_var > 0.0) ? std::sqrt(num / samp_var) : 1.0;
    }
  }
  return acc;
}

} // namespace detail

// Monte-Carlo eigenfactor risk adjustment (MWO). Returns the de-biased K×K factor
// covariance F̂. `sims == 0` ⇒ F unchanged (opt-in no-op). `amplify == 0` ⇒ F̂ ≡ F
// (identity). Same (f, sims, amplify, seed) ⇒ byte-identical F̂ (one seeded
// Xoshiro256pp, fixed-count sims, canonical draw order). Err on a non-symmetric F
// (symmetric_eig) or a non-positive eigenvalue (F not SPD).
[[nodiscard]] inline atx::core::Result<atx::core::linalg::MatX>
eigen_adjust(const atx::core::linalg::MatX &f, atx::usize sims, atx::f64 amplify, atx::u64 seed) {
  if (sims == 0U) {
    return atx::core::Ok(f); // opt-in: no simulation requested ⇒ F unchanged
  }
  ATX_TRY(atx::core::linalg::EigResult eig, atx::core::linalg::symmetric_eig(f));
  const atx::core::linalg::MatX &u = eig.vectors; // columns = eigenvectors
  const atx::core::linalg::VecX &d = eig.values;  // eigenvalues, ASCENDING
  const Eigen::Index k = d.size();
  for (Eigen::Index kk = 0; kk < k; ++kk) {
    if (!(d[kk] > 0.0)) { // also catches NaN (every comparison with NaN is false)
      return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                            "eigen_adjust: F must be symmetric positive-definite (D[k] > 0)");
    }
  }

  atx::core::Xoshiro256pp rng{seed}; // the ONE seeded RNG (constructed once)
  const atx::usize t = detail::sim_obs(static_cast<atx::usize>(k));
  const atx::core::linalg::VecX acc = detail::accumulate_vol_bias(u, d, sims, t, rng);

  // v[k] = acc[k]/M; γ[k] = a·(v[k]−1)+1; D̂[k] = γ[k]²·D[k] (PSD-preserving, γ²>0).
  const atx::f64 inv_m = 1.0 / static_cast<atx::f64>(sims);
  atx::core::linalg::VecX d_hat(k);
  for (Eigen::Index kk = 0; kk < k; ++kk) {
    const atx::f64 v = acc[kk] * inv_m;
    const atx::f64 gamma = amplify * (v - 1.0) + 1.0;
    d_hat[kk] = gamma * gamma * d[kk];
  }
  atx::core::linalg::MatX f_hat = u * d_hat.asDiagonal() * u.transpose();
  f_hat = 0.5 * (f_hat + f_hat.transpose()); // re-symmetrize against rotation round-off
  return atx::core::Ok(std::move(f_hat));
}

} // namespace atx::engine::risk
