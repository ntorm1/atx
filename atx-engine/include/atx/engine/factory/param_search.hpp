#pragma once

// atx::engine::factory — parameter optimizer (S3-3, plan §4.5 / §2.1).
//
// Given a template Genome and the free fractional constants it carries (the
// Window / Scale literals, classified by §0.3), search the constant-space for
// the configuration that maximizes an INJECTED fitness functor. Three methods:
//
//   Grid     — cartesian product of per-dim grids (cfg.lambda_or_gridpoints
//              points/dim), eval each, argmax. Uses NO rng.
//   Random   — cfg-budget seeded uniform-in-bounds samples, argmax.
//   SepCmaEs — separable (diagonal-covariance) CMA-ES, Ros & Hansen 2008.
//
// SAFETY / scope: SepCmaEs is the SEPARABLE variant — the covariance is DIAGONAL
// (per-coordinate step sizes; C is never materialized as a dense matrix and is
// never eigendecomposed). It is well-suited to the low-dimensional fractional-
// constant search and needs no atx-core L7 `eigh`. Full rotation-invariant
// CMA-ES (with the covariance eigendecomposition) is the recorded Pattern-B
// atx-core L7 lift (plan §2.1) — deliberately NOT built here.
//
// Determinism (F1): every random draw comes from the caller-seeded
// `Xoshiro256pp` (a single sequential stream — same seed ⇒ same sequence ⇒
// identical trajectory ⇒ byte-identical `best_x`/`trials`). NEVER seed by
// worker/thread/time. The Grid path touches the rng zero times (the tests pass a
// null rng to the Grid call).
//
// Header-only; COLD path (run once per template, off the VM hot path), so
// std::vector allocation is fine.

#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>
#include <span>
#include <utility>
#include <vector>

#include "atx/core/error.hpp"
#include "atx/core/random.hpp"
#include "atx/core/types.hpp"

#include "atx/engine/factory/genome.hpp"
#include "atx/engine/factory/mutation.hpp"

namespace atx::engine::factory {

using atx::core::Xoshiro256pp;

// =========================================================================
//  ParamSpace — the free-constant search box (plan §4.5).
//
//  One dim per free fractional constant: the genome literal it edits (`id`), its
//  classification (`kind`), and inclusive bounds `[lo, hi]`. A genome-free `box`
//  dim (the raw-optimizer tests) has `id == kNoExpr` and an arbitrary `kind`.
// =========================================================================

struct ParamDim {
  ExprId id{kNoExpr};            // the genome Literal this dim edits (kNoExpr = box dim)
  ConstKind kind{ConstKind::Scale};
  atx::f64 lo{0.0};
  atx::f64 hi{1.0};
};

struct ParamSpace {
  std::vector<ParamDim> dim;

  [[nodiscard]] atx::usize dims() const noexcept { return dim.size(); }
};

// Default rails used by `extract_free_constants` (documented spans):
//   * Window dims are bounded [1, kDefaultMaxLookback] — the same lookback cap
//     jitter_const clamps to (§0.3), so the search never proposes a window the
//     causality rail would reject.
//   * Scale/Hparam dims get a continuous range centered on the current value:
//     [min(0, kScaleSpan*v), max(0, kScaleSpan*v)] — i.e. [0, 2v] for v>0,
//     [2v, 0] for v<0. A near-zero |v| collapses that to a point, so we widen it
//     to a fixed [-kScaleFloor, +kScaleFloor] fallback so the optimizer still has
//     room to move.
inline constexpr atx::f64 kDefaultMaxLookback = 250.0;
inline constexpr atx::f64 kScaleSpan = 2.0;   // half-width factor: span = [0, 2v]
inline constexpr atx::f64 kScaleFloor = 1.0;  // min half-range for near-zero scales

// A genome-free factory: one dim per `{lo, hi}` pair, no genome literal attached
// (`id == kNoExpr`). Used by the raw-optimizer tests against a closed-form
// objective. The dim `kind` is irrelevant for the raw path (no instantiation).
[[nodiscard]] ParamSpace box(const std::vector<std::pair<atx::f64, atx::f64>> &bounds);

// Extract the free fractional constants of `g` as a search box (plan §4.5). The
// free dims are the Window and Scale literals (Hparam included best-effort, since
// a peeled hparam is still a tunable immediate). Reuses `classify_literals` (§0.3)
// so the dim order is canonical (ascending ExprId) and thus deterministic.
[[nodiscard]] ParamSpace extract_free_constants(const Genome &g);

// =========================================================================
//  Config + result.
// =========================================================================

enum class Method : atx::u8 { Grid, Random, SepCmaEs };

struct ParamSearchCfg {
  Method method{Method::SepCmaEs};
  // Grid:    points per dim. Random: total sample budget. SepCmaEs: lambda
  // (offspring per generation).
  atx::usize lambda_or_gridpoints{12};
  // SepCmaEs: number of generations. Ignored by Grid/Random.
  atx::usize generations{1};
  // SepCmaEs initial per-coordinate step as a fraction of each dim's range
  // (sigma_k = sigma0_frac * (hi-lo)); the standard sep-CMA default is 0.3.
  atx::f64 sigma0_frac{0.3};
};

struct ParamResult {
  std::vector<atx::f64> best_x;
  atx::f64 best_fitness{-std::numeric_limits<atx::f64>::infinity()};
  atx::usize trials{0};
};

// =========================================================================
//  instantiate — set the genome's free constants to a concrete point.
//
//  Rebuilds the template genome with every dim's Literal value replaced by the
//  corresponding `x` coordinate (one chained rebuild per dim via `rebuild_with`,
//  funneled through `analyze_into` — the F5 oracle). Box dims (id == kNoExpr) are
//  skipped (the raw path never instantiates). On any rebuild failing analyze, the
//  Err propagates so the genome optimizer can treat that point as worst fitness.
// =========================================================================

[[nodiscard]] atx::core::Result<Genome>
instantiate(const Genome &template_genome, const ParamSpace &space, std::span<const atx::f64> x);

// =========================================================================
//  detail — the three search drivers (raw, fitness over std::span<const f64>).
// =========================================================================

namespace detail {

[[nodiscard]] atx::f64 clamp_dim(atx::f64 v, const ParamDim &d) noexcept;

// Positivity floor for the diagonal covariance entries c_k. Applied BEFORE any
// division by c_k (or 1/sqrt(c_k)) so a collapsed coordinate can never blow up
// the rank-one update; keeps C strictly positive-definite (the sep-CMA invariant).
inline constexpr atx::f64 kCdiagFloor = 1e-20;

// Grid: cartesian product of per-dim grids (`pts` points/dim, evenly spaced
// across [lo, hi]; a single-point dim sits at its midpoint). Touches NO rng.
template <class Fitness>
inline void grid_search(const ParamSpace &space, Fitness &f, atx::usize pts, ParamResult &out) {
  const atx::usize K = space.dims();
  if (pts == 0) {
    return;
  }
  const atx::usize total = static_cast<atx::usize>(std::pow(static_cast<double>(pts),
                                                            static_cast<double>(K)));
  std::vector<atx::f64> x(K);
  for (atx::usize lin = 0; lin < total; ++lin) {
    atx::usize rem = lin;
    for (atx::usize k = 0; k < K; ++k) {
      const atx::usize idx = rem % pts;
      rem /= pts;
      const ParamDim &d = space.dim[k];
      x[k] = (pts == 1) ? 0.5 * (d.lo + d.hi)
                        : d.lo + (d.hi - d.lo) * (static_cast<atx::f64>(idx) /
                                                  static_cast<atx::f64>(pts - 1));
    }
    const atx::f64 fit = f(std::span<const atx::f64>(x));
    ++out.trials;
    if (fit > out.best_fitness) {
      out.best_fitness = fit;
      out.best_x = x;
    }
  }
}

// Random: `budget` seeded uniform-in-bounds samples, argmax.
template <class Fitness>
inline void random_search(const ParamSpace &space, Fitness &f, atx::usize budget,
                          Xoshiro256pp &rng, ParamResult &out) {
  const atx::usize K = space.dims();
  std::vector<atx::f64> x(K);
  for (atx::usize i = 0; i < budget; ++i) {
    for (atx::usize k = 0; k < K; ++k) {
      x[k] = rng.uniform(space.dim[k].lo, space.dim[k].hi);
    }
    const atx::f64 fit = f(std::span<const atx::f64>(x));
    ++out.trials;
    if (fit > out.best_fitness) {
      out.best_fitness = fit;
      out.best_x = x;
    }
  }
}

// Recombination + adaptation constants for a separable CMA-ES with the standard
// (Hansen) weighting. Computed once per run from (K, lambda).
struct SepCmaParams {
  atx::usize mu{0};
  std::vector<atx::f64> w;   // positive recombination weights, sum = 1
  atx::f64 mu_eff{0.0};
  atx::f64 c_sigma{0.0};     // step-size path learning rate
  atx::f64 d_sigma{0.0};     // step-size damping
  atx::f64 c_c{0.0};         // covariance path learning rate
  atx::f64 c_1{0.0};         // rank-one learning rate
  atx::f64 c_mu{0.0};        // rank-mu learning rate
  atx::f64 chi_n{0.0};       // E||N(0,I)||
};

[[nodiscard]] SepCmaParams sep_cma_params(atx::usize K, atx::usize lambda);

// Separable (diagonal-covariance) CMA-ES (Ros & Hansen 2008). Diagonal C held as
// per-coordinate variances `cdiag`; the global step `sigma` scales it. Each of
// the `lambda*generations` fitness evals increments the trial count and updates
// the running best. All N(0,1) draws come from the single seeded rng stream.
template <class Fitness>
inline void sep_cma_es(const ParamSpace &space, Fitness &f, Xoshiro256pp &rng,
                       const ParamSearchCfg &cfg, ParamResult &out) {
  const atx::usize K = space.dims();
  const atx::usize lambda = std::max<atx::usize>(2, cfg.lambda_or_gridpoints);
  const SepCmaParams p = sep_cma_params(K, lambda);

  // mean at the box midpoint; per-coordinate sigma scale & diagonal C = 1.
  std::vector<atx::f64> mean(K);
  std::vector<atx::f64> sigma_scale(K); // sigma0_frac * (hi-lo) per dim
  std::vector<atx::f64> cdiag(K, 1.0);  // diagonal of C (std-dev factor^2)
  std::vector<atx::f64> p_sigma(K, 0.0);
  std::vector<atx::f64> p_c(K, 0.0);
  atx::f64 sigma = 1.0; // global step (the per-dim scale lives in sigma_scale)
  for (atx::usize k = 0; k < K; ++k) {
    const ParamDim &d = space.dim[k];
    mean[k] = 0.5 * (d.lo + d.hi);
    sigma_scale[k] = cfg.sigma0_frac * (d.hi - d.lo);
  }

  std::vector<std::vector<atx::f64>> z(lambda, std::vector<atx::f64>(K)); // raw N(0,I) draws
  std::vector<std::vector<atx::f64>> xs(lambda, std::vector<atx::f64>(K)); // clamped candidates
  std::vector<atx::f64> fit(lambda);
  std::vector<atx::usize> order(lambda);

  for (atx::usize gen = 0; gen < cfg.generations; ++gen) {
    // Sample lambda offspring: x = clamp(mean + sigma * sigma_scale .* sqrt(C) .* z).
    for (atx::usize i = 0; i < lambda; ++i) {
      for (atx::usize k = 0; k < K; ++k) {
        z[i][k] = rng.normal();
        const atx::f64 step = sigma * sigma_scale[k] * std::sqrt(cdiag[k]) * z[i][k];
        xs[i][k] = clamp_dim(mean[k] + step, space.dim[k]);
      }
      fit[i] = f(std::span<const atx::f64>(xs[i]));
      ++out.trials;
      if (fit[i] > out.best_fitness) {
        out.best_fitness = fit[i];
        out.best_x = xs[i];
      }
    }

    // Rank offspring by fitness DESC (maximization). Stable on index ties keeps
    // the trajectory deterministic.
    std::iota(order.begin(), order.end(), atx::usize{0});
    std::stable_sort(order.begin(), order.end(),
                     [&fit](atx::usize i, atx::usize j) { return fit[i] > fit[j]; });

    // Recombine: new mean = sum_i w_i * x_(i:lambda) over the top mu offspring.
    std::vector<atx::f64> mean_old = mean;
    for (atx::usize k = 0; k < K; ++k) {
      atx::f64 mk = 0.0;
      for (atx::usize i = 0; i < p.mu; ++i) {
        mk += p.w[i] * xs[order[i]][k];
      }
      mean[k] = mk;
    }

    // The per-coordinate C^{-1/2}-normalized REALIZED step of the mean:
    //   y_k = (mean_k - mean_old_k) / (sigma * sigma_scale_k * sqrt(c_k)).
    // Using the realized (post-clamp) mean — NOT the raw pre-clamp z — keeps the
    // paths correct when offspring are clamped to the box bounds (without this the
    // search stalls far from a boundary-adjacent optimum). In the separable case
    // C^{-1/2} is exactly per-coordinate 1/sqrt(c_k), so no matrix algebra.
    std::vector<atx::f64> y(K);
    for (atx::usize k = 0; k < K; ++k) {
      const atx::f64 denom = sigma * sigma_scale[k] * std::sqrt(cdiag[k]);
      y[k] = (denom > 0.0) ? (mean[k] - mean_old[k]) / denom : 0.0;
    }

    // Step-size path p_sigma (separable: sqrt(C^-1) is per-coordinate 1/sqrt(c)).
    const atx::f64 cs_disc = std::sqrt(p.c_sigma * (2.0 - p.c_sigma) * p.mu_eff);
    atx::f64 ps_norm_sq = 0.0;
    for (atx::usize k = 0; k < K; ++k) {
      p_sigma[k] = (1.0 - p.c_sigma) * p_sigma[k] + cs_disc * y[k];
      ps_norm_sq += p_sigma[k] * p_sigma[k];
    }
    const atx::f64 ps_norm = std::sqrt(ps_norm_sq);

    // Covariance path p_c (with the standard h_sigma stall guard).
    const atx::f64 gen_next = static_cast<atx::f64>(gen + 1);
    const atx::f64 hsig_thresh =
        (1.4 + 2.0 / (static_cast<atx::f64>(K) + 1.0)) * p.chi_n *
        std::sqrt(1.0 - std::pow(1.0 - p.c_sigma, 2.0 * gen_next));
    const bool h_sigma = ps_norm < hsig_thresh;
    const atx::f64 cc_disc = std::sqrt(p.c_c * (2.0 - p.c_c) * p.mu_eff);
    for (atx::usize k = 0; k < K; ++k) {
      p_c[k] = (1.0 - p.c_c) * p_c[k] + (h_sigma ? cc_disc * y[k] : 0.0);
    }

    // Rank-mu + rank-one diagonal C update (per-coordinate; no matrix algebra).
    // The rank-mu term uses each selected offspring's realized normalized step
    //   y_ik = (xs_ik - mean_old_k) / (sigma * sigma_scale_k * sqrt(c_k))
    // (again realized/clamped, consistent with the mean-path normalization).
    const atx::f64 hsig_corr = (1.0 - (h_sigma ? 1.0 : 0.0)) * p.c_c * (2.0 - p.c_c);
    for (atx::usize k = 0; k < K; ++k) {
      // Apply the positivity floor BEFORE any division by c_k: a prior iteration
      // may have floored cdiag[k] to kCdiagFloor, and dividing p_c^2 (or forming
      // 1/sqrt(c)) by a sub-floor value would blow up to ~1/floor. Use the floored
      // c_k everywhere this iteration, then write the new floored value back.
      const atx::f64 c_k = std::max(cdiag[k], kCdiagFloor);
      const atx::f64 denom = sigma * sigma_scale[k] * std::sqrt(c_k);
      atx::f64 rank_mu = 0.0;
      for (atx::usize i = 0; i < p.mu; ++i) {
        const atx::f64 yik = (denom > 0.0) ? (xs[order[i]][k] - mean_old[k]) / denom : 0.0;
        rank_mu += p.w[i] * yik * yik;
      }
      atx::f64 next = (1.0 - p.c_1 - p.c_mu) * c_k +
                      p.c_1 * (p_c[k] * p_c[k] / c_k + hsig_corr) +
                      p.c_mu * rank_mu;
      cdiag[k] = std::max(next, kCdiagFloor); // C stays positive (>= floor)
    }

    // Step-size control (CSA): sigma *= exp((c_sigma/d_sigma)(||p_sigma||/chi_n - 1)).
    sigma *= std::exp((p.c_sigma / p.d_sigma) * (ps_norm / p.chi_n - 1.0));
  }
}

} // namespace detail

// =========================================================================
//  optimize_params_raw — the genome-free optimizer over a closed-form objective.
//
//  `f` is any callable `std::span<const f64> -> f64` (maximized). Grid uses NO
//  rng; Random/SepCmaEs draw exclusively from `rng`. Returns best_x / best_fitness
//  and the total fitness-eval count.
// =========================================================================

template <class Fitness>
[[nodiscard]] inline ParamResult optimize_params_raw(const ParamSpace &space, Fitness &&f,
                                                     Xoshiro256pp &rng, const ParamSearchCfg &cfg) {
  ParamResult out;
  // Boundary (plan §3): a space with ZERO free constants (e.g. `rank(close)` has
  // no Window/Scale literal). There is nothing to search — every method would be
  // a no-op, and the sep-CMA recurrence divides by K (→ NaN). Score the single
  // degenerate (empty) point ONCE so best_fitness is finite, then return; touches
  // no rng. best_x stays empty (its natural value for a 0-dim space).
  if (space.dims() == 0) {
    out.best_fitness = f(std::span<const atx::f64>{});
    out.trials = 1;
    return out;
  }
  switch (cfg.method) {
  case Method::Grid:
    detail::grid_search(space, f, cfg.lambda_or_gridpoints, out);
    break;
  case Method::Random:
    detail::random_search(space, f, cfg.lambda_or_gridpoints, rng, out);
    break;
  case Method::SepCmaEs:
    detail::sep_cma_es(space, f, rng, cfg, out);
    break;
  }
  if (out.best_x.empty()) {
    out.best_x.assign(space.dims(), 0.0); // boundary: 0 generations / 0 points
  }
  return out;
}

// =========================================================================
//  optimize_params — the genome path: each candidate is instantiated and scored.
//
//  Wraps the raw optimizer with an `instantiate`-then-`fitness_fn` adapter.
//  `fitness_fn` is any callable `const Genome& -> f64` (maximized). On a candidate
//  that fails to instantiate (rebuild/analyze Err), the point is scored as worst
//  fitness (-inf) and SKIPPED from the optimum — F5 guarantees only analyze-valid
//  genomes are ever scored. Each instantiation counts as one trial (via the raw
//  optimizer's eval count).
// =========================================================================

template <class FitnessFn>
[[nodiscard]] inline ParamResult optimize_params(const Genome &template_genome,
                                                 const ParamSpace &space, FitnessFn &&fitness_fn,
                                                 Xoshiro256pp &rng, const ParamSearchCfg &cfg) {
  auto adapter = [&template_genome, &space, &fitness_fn](std::span<const atx::f64> x) -> atx::f64 {
    auto g = instantiate(template_genome, space, x);
    if (!g) {
      return -std::numeric_limits<atx::f64>::infinity(); // un-analyzable: worst, skipped (F5)
    }
    return fitness_fn(*g);
  };
  return optimize_params_raw(space, adapter, rng, cfg);
}

} // namespace atx::engine::factory
