#pragma once

// atx::engine::learn — deterministic log-space Baum-Welch HMM + PIT regime
// posterior (S5-5, Pattern-B edge 3, §4.5 + M1/M2/M4/M5).
//
// =====================================================================
//  What this header is
// =====================================================================
//  A self-contained hidden-Markov-model kernel over a RAW observation matrix
//  `obs` (T time steps x d observation dims) — NO Panel/FeatureMatrix dependency.
//  It fits a diagonal-Gaussian-emission HMM by Baum-Welch (EM), entirely in
//  LOG-SPACE (logsumexp), so a long series cannot underflow the alpha/beta
//  recursions. On top of the fitted model sits the load-bearing seam: a
//  POINT-IN-TIME regime posterior — a FORWARD-ONLY filter that reads only obs
//  rows [0..d] and so is truncation-invariant (M2).
//
// =====================================================================
//  Why this is byte-identical across builds/threads (M1)
// =====================================================================
//    * The seeded init draws from a single Xoshiro256pp seeded via
//      seed_for(master_seed, "hmm", 0, 0) in a FIXED order (means, then the
//      global-variance init, then the near-uniform transition / uniform prior).
//      Same seed -> byte-identical init -> byte-identical EM trajectory -> fit.
//    * Every reduction walks a FIXED order: forward over t ascending and s'
//      ascending, backward over t descending and s' ascending, the M-step sums
//      over t ascending. No map iteration, no wall-clock / thread / address
//      entropy, no parallel non-associative float reduction.
//    * logsumexp subtracts the running max (the standard max-shift) so the same
//      inputs always produce the same bit pattern, and the all-(-inf) case
//      returns -inf rather than NaN.
//
// =====================================================================
//  The PIT firewall this unit must honor (M2 / §0.5)
// =====================================================================
//  regime_posterior_at(fitted, obs, d) runs the forward recursion from t=0..d and
//  normalizes log_alpha at row d. The forward recursion at t=d depends ONLY on
//  obs rows [0..d], so the result is identical whether obs has d+1 rows or many
//  more — a fitted model applied forward at d never reads the future.
//
// Header-only; every function is defined inline. Fitting is a COLD path (once per
// training window), so std::vector / Eigen allocation is fine.

#include <cmath>   // std::log, std::exp, std::isfinite
#include <limits>  // std::numeric_limits (the -inf / max-shift sentinels)
#include <span>    // std::span (logsumexp over a contiguous slice)
#include <utility> // std::move
#include <vector>  // std::vector

#include <Eigen/Dense> // Eigen::Index, MatX/VecX

#include "atx/core/macro.hpp"  // ATX_CHECK
#include "atx/core/random.hpp" // atx::core::Xoshiro256pp
#include "atx/core/types.hpp"  // f64, u32, u64, usize

#include "atx/core/linalg/linalg.hpp" // MatX, VecX

#include "atx/engine/learn/train.hpp" // seed_for

namespace atx::engine::learn {

namespace hmm_lin = atx::core::linalg;

// ===========================================================================
//  HmmCfg — the Baum-Welch knobs.
//
//  n_states  : number of hidden regimes.
//  max_iter  : EM iteration cap.
//  tol       : EM stops when |Δloglik| < tol.
//  master_seed : determinism root (the seeded init derives from it via seed_for).
//  var_floor : a floor on every emission variance, so a near-constant dim cannot
//              drive var -> 0 and blow up the Gaussian log-pdf.
// ===========================================================================
struct HmmCfg {
  atx::u32 n_states{3};
  atx::u32 max_iter{100};
  atx::f64 tol{1e-6};
  atx::u64 master_seed{0};
  atx::f64 var_floor{1e-6};
};

// ===========================================================================
//  Gaussian — a diagonal-Gaussian emission for one state: per-dim mean + variance.
// ===========================================================================
struct Gaussian {
  hmm_lin::VecX mean; // d
  hmm_lin::VecX var;  // d (each >= var_floor)
};

// ===========================================================================
//  Hmm — the fitted parameters.
//
//  logA  : n_states x n_states, log transition (each ROW sums to 1 in prob space).
//  emit  : per-state diagonal Gaussian.
//  logpi : n_states, log initial distribution (sums to 1 in prob space).
// ===========================================================================
struct Hmm {
  hmm_lin::MatX logA;
  std::vector<Gaussian> emit;
  hmm_lin::VecX logpi;
  atx::u32 n_states{0};
  atx::u32 n_dims{0};
};

// ===========================================================================
//  ForwardResult — the forward log-alpha (T x n_states) + total log-likelihood.
// ===========================================================================
struct ForwardResult {
  hmm_lin::MatX log_alpha;
  atx::f64 loglik{0.0};
};

namespace hmm_detail {

// Negative infinity in log-space — the log of a zero probability.
[[nodiscard]] inline atx::f64 neg_inf() noexcept {
  return -std::numeric_limits<atx::f64>::infinity();
}

// logsumexp over a span of log-values with the standard max-shift:
//   log(Σ exp(x_i)) = m + log(Σ exp(x_i - m)),  m = max_i x_i.
// Subtracting the max prevents overflow; the all-(-inf) case (m == -inf) returns
// -inf rather than NaN (every term exp(-inf - -inf) is otherwise NaN). The sum is
// an ASCENDING walk over the span -> order-fixed (M1). This is the ONE logsumexp.
[[nodiscard]] inline atx::f64 logsumexp(std::span<const atx::f64> xs) noexcept {
  atx::f64 m = neg_inf();
  for (const atx::f64 x : xs) {
    if (x > m) {
      m = x;
    }
  }
  if (!std::isfinite(m)) {
    return neg_inf(); // all terms are -inf (an impossible event) -> log 0 = -inf
  }
  atx::f64 acc = 0.0;
  for (const atx::f64 x : xs) {
    acc += std::exp(x - m);
  }
  return m + std::log(acc);
}

// log N(x | mean, var) for a DIAGONAL Gaussian (state s at time t), summed over
// dims: Σ_dim [ -0.5*log(2π) - 0.5*log(var) - 0.5*(x-mean)^2/var ]. The variances
// are already floored at fit time, so var > 0 here and the division is safe.
[[nodiscard]] inline atx::f64 diag_gaussian_logpdf(const Gaussian &g, const hmm_lin::MatX &obs,
                                                   atx::usize t) noexcept {
  constexpr atx::f64 k_log_2pi = 1.8378770664093454835606594728112; // log(2π)
  const Eigen::Index d = g.mean.size();
  atx::f64 lp = 0.0;
  for (Eigen::Index j = 0; j < d; ++j) {
    const atx::f64 v = g.var(j);
    const atx::f64 diff = obs(static_cast<Eigen::Index>(t), j) - g.mean(j);
    lp += -0.5 * (k_log_2pi + std::log(v) + (diff * diff) / v);
  }
  return lp;
}

// The (T x n_states) emission log-likelihood matrix: logB(t, s) = log N(obs_t | s).
// Computed once per forward/backward/EM pass (it does not change within a pass).
[[nodiscard]] inline hmm_lin::MatX emission_logp(const Hmm &h, const hmm_lin::MatX &obs) {
  const atx::usize T = static_cast<atx::usize>(obs.rows());
  const Eigen::Index ns = static_cast<Eigen::Index>(h.n_states);
  hmm_lin::MatX logB(static_cast<Eigen::Index>(T), ns);
  for (atx::usize t = 0; t < T; ++t) {
    for (Eigen::Index s = 0; s < ns; ++s) {
      logB(static_cast<Eigen::Index>(t), s) =
          diag_gaussian_logpdf(h.emit[static_cast<atx::usize>(s)], obs, t);
    }
  }
  return logB;
}

// Forward log-alpha over a PREFIX of length `T_use` (T_use <= obs.rows()). Used
// both by forward_log (T_use = T, the full series) and by regime_posterior_at
// (T_use = d+1, the causal prefix) so the two share ONE recursion — the PIT filter
// is literally the forward pass truncated at row d (M2). The recursion at row t
// reads only obs rows [0..t], so a prefix of length T_use is invariant to any
// rows beyond it. Order-fixed: t ascending, the inner logsumexp s' ascending (M1).
[[nodiscard]] inline hmm_lin::MatX forward_alpha_prefix(const Hmm &h, const hmm_lin::MatX &obs,
                                                        atx::usize T_use) {
  const Eigen::Index ns = static_cast<Eigen::Index>(h.n_states);
  const hmm_lin::MatX logB = emission_logp(h, obs);
  hmm_lin::MatX la(static_cast<Eigen::Index>(T_use), ns);
  for (Eigen::Index s = 0; s < ns; ++s) {
    la(0, s) = h.logpi(s) + logB(0, s);
  }
  std::vector<atx::f64> tmp(static_cast<atx::usize>(ns));
  for (atx::usize t = 1; t < T_use; ++t) {
    for (Eigen::Index s = 0; s < ns; ++s) {
      for (Eigen::Index sp = 0; sp < ns; ++sp) {
        tmp[static_cast<atx::usize>(sp)] =
            la(static_cast<Eigen::Index>(t) - 1, sp) + h.logA(sp, s);
      }
      la(static_cast<Eigen::Index>(t), s) =
          logB(static_cast<Eigen::Index>(t), s) + logsumexp(std::span<const atx::f64>{tmp});
    }
  }
  return la;
}

// Backward log-beta over the FULL series, computed INDEPENDENTLY of the forward
// pass (this is what makes the M4 identity a real cross-check). log_beta(T-1,s)=0;
// log_beta(t,s) = logsumexp_s'( logA(s,s') + logB(t+1,s') + log_beta(t+1,s') ).
// Order-fixed: t descending, the inner logsumexp s' ascending (M1).
[[nodiscard]] inline hmm_lin::MatX backward_beta(const Hmm &h, const hmm_lin::MatX &obs,
                                                 const hmm_lin::MatX &logB) {
  const atx::usize T = static_cast<atx::usize>(obs.rows());
  const Eigen::Index ns = static_cast<Eigen::Index>(h.n_states);
  hmm_lin::MatX lb(static_cast<Eigen::Index>(T), ns);
  for (Eigen::Index s = 0; s < ns; ++s) {
    lb(static_cast<Eigen::Index>(T) - 1, s) = 0.0; // log 1
  }
  std::vector<atx::f64> tmp(static_cast<atx::usize>(ns));
  for (atx::usize t = T - 1; t-- > 0;) { // t = T-2 .. 0 (descending, no underflow)
    for (Eigen::Index s = 0; s < ns; ++s) {
      for (Eigen::Index sp = 0; sp < ns; ++sp) {
        tmp[static_cast<atx::usize>(sp)] =
            h.logA(s, sp) + logB(static_cast<Eigen::Index>(t) + 1, sp) +
            lb(static_cast<Eigen::Index>(t) + 1, sp);
      }
      lb(static_cast<Eigen::Index>(t), s) = logsumexp(std::span<const atx::f64>{tmp});
    }
  }
  return lb;
}

// The total log-likelihood from the LAST forward row: logsumexp_s log_alpha(T-1,s).
[[nodiscard]] inline atx::f64 loglik_from_alpha(const hmm_lin::MatX &la) {
  const atx::usize T = static_cast<atx::usize>(la.rows());
  const Eigen::Index ns = la.cols();
  std::vector<atx::f64> last(static_cast<atx::usize>(ns));
  for (Eigen::Index s = 0; s < ns; ++s) {
    last[static_cast<atx::usize>(s)] = la(static_cast<Eigen::Index>(T) - 1, s);
  }
  return logsumexp(std::span<const atx::f64>{last});
}

// ---------------------------------------------------------------------------
//  Seeded init (M1) — a deterministic starting HMM from a single seeded RNG.
// ---------------------------------------------------------------------------

// Per-dim global mean and (population) variance of obs, used to seed the emission
// means (jittered) and variances. Order-fixed ascending sums (M1).
inline void global_stats(const hmm_lin::MatX &obs, hmm_lin::VecX &mean_out,
                         hmm_lin::VecX &var_out) {
  const atx::usize T = static_cast<atx::usize>(obs.rows());
  const Eigen::Index d = obs.cols();
  mean_out = hmm_lin::VecX::Zero(d);
  var_out = hmm_lin::VecX::Zero(d);
  for (atx::usize t = 0; t < T; ++t) {
    for (Eigen::Index j = 0; j < d; ++j) {
      mean_out(j) += obs(static_cast<Eigen::Index>(t), j);
    }
  }
  mean_out /= static_cast<atx::f64>(T);
  for (atx::usize t = 0; t < T; ++t) {
    for (Eigen::Index j = 0; j < d; ++j) {
      const atx::f64 dv = obs(static_cast<Eigen::Index>(t), j) - mean_out(j);
      var_out(j) += dv * dv;
    }
  }
  var_out /= static_cast<atx::f64>(T);
}

// A deterministic starting HMM (M1). Means are the global mean jittered by a
// per-state, per-dim normal scaled by the global per-dim std, so distinct states
// start separated; init var = global per-dim var (floored); A is near-uniform with
// a self-transition bias; pi is uniform. ALL rng draws happen in a FIXED order
// (state-major, dim-minor for the means) so the same seed gives a byte-identical
// init. The matrices are stored in LOG-space.
[[nodiscard]] inline Hmm seeded_init(const hmm_lin::MatX &obs, const HmmCfg &cfg) {
  Hmm h;
  h.n_states = cfg.n_states;
  h.n_dims = static_cast<atx::u32>(obs.cols());
  const Eigen::Index ns = static_cast<Eigen::Index>(cfg.n_states);
  const Eigen::Index d = obs.cols();

  hmm_lin::VecX gmean;
  hmm_lin::VecX gvar;
  global_stats(obs, gmean, gvar);

  atx::core::Xoshiro256pp rng{seed_for(cfg.master_seed, "hmm", 0U, 0U)};

  // Emission means: global mean + jitter ~ std * N(0,1) per (state, dim), drawn in
  // a fixed state-major / dim-minor order. Variances: the floored global variance.
  h.emit.resize(static_cast<atx::usize>(ns));
  for (Eigen::Index s = 0; s < ns; ++s) {
    Gaussian g;
    g.mean = hmm_lin::VecX(d);
    g.var = hmm_lin::VecX(d);
    for (Eigen::Index j = 0; j < d; ++j) {
      const atx::f64 sd = std::sqrt(gvar(j));
      g.mean(j) = gmean(j) + sd * rng.normal();
      const atx::f64 v = gvar(j);
      g.var(j) = (v < cfg.var_floor) ? cfg.var_floor : v;
    }
    h.emit[static_cast<atx::usize>(s)] = std::move(g);
  }

  // Transition: near-uniform with a self-transition bias (regimes persist), stored
  // in log-space (rows sum to 1 in prob space). Off-diagonal mass split evenly.
  h.logA = hmm_lin::MatX(ns, ns);
  const atx::f64 self_p = 0.90;
  const atx::f64 other_p =
      (ns > 1) ? (1.0 - self_p) / static_cast<atx::f64>(ns - 1) : 1.0;
  for (Eigen::Index i = 0; i < ns; ++i) {
    for (Eigen::Index k = 0; k < ns; ++k) {
      h.logA(i, k) = std::log((i == k) ? self_p : other_p);
    }
  }

  // Initial distribution: uniform.
  h.logpi = hmm_lin::VecX(ns);
  const atx::f64 u = 1.0 / static_cast<atx::f64>(ns);
  for (Eigen::Index s = 0; s < ns; ++s) {
    h.logpi(s) = std::log(u);
  }
  return h;
}

// One EM iteration: E-step (log_gamma / log_xi from independent alpha+beta) then
// M-step (re-estimate pi, A, and each Gaussian in prob space via exp of normalized
// logs, order-fixed ascending sums). Returns the data log-likelihood at the
// CURRENT params (computed from alpha) so the caller can test convergence. A dead
// state (near-zero responsibility) keeps its previous emission/row deterministically
// rather than dividing by ~0.
[[nodiscard]] inline atx::f64 em_step(Hmm &h, const hmm_lin::MatX &obs, atx::f64 var_floor) {
  const atx::usize T = static_cast<atx::usize>(obs.rows());
  const Eigen::Index ns = static_cast<Eigen::Index>(h.n_states);
  const Eigen::Index d = obs.cols();
  const hmm_lin::MatX logB = emission_logp(h, obs);

  // Forward / backward (independent recursions); the loglik is from alpha.
  hmm_lin::MatX la = forward_alpha_prefix(h, obs, T);
  const hmm_lin::MatX lb = backward_beta(h, obs, logB);
  const atx::f64 loglik = loglik_from_alpha(la);

  // log_gamma(t,s) = la + lb - loglik. (Normalized per t; logsumexp_s gamma == 0.)
  hmm_lin::MatX log_gamma(static_cast<Eigen::Index>(T), ns);
  for (atx::usize t = 0; t < T; ++t) {
    for (Eigen::Index s = 0; s < ns; ++s) {
      log_gamma(static_cast<Eigen::Index>(t), s) =
          la(static_cast<Eigen::Index>(t), s) + lb(static_cast<Eigen::Index>(t), s) - loglik;
    }
  }

  // --- M-step: pi from gamma_0 ---
  for (Eigen::Index s = 0; s < ns; ++s) {
    h.logpi(s) = log_gamma(0, s);
  }

  // --- M-step: A(i,j) = Σ_t exp(log_xi(t,i,j)) / Σ_t exp(log_gamma(t,i)) ---
  // log_xi(t,i,j) = la(t,i) + logA(i,j) + logB(t+1,j) + lb(t+1,j) - loglik.
  // Accumulate the numerator/denominator in PROB space (exp), order-fixed over t.
  hmm_lin::MatX xi_num = hmm_lin::MatX::Zero(ns, ns);
  hmm_lin::VecX gamma_den_A = hmm_lin::VecX::Zero(ns); // Σ_{t<T-1} gamma(t,i)
  const hmm_lin::MatX logA_prev = h.logA; // the transition used in this E-step
  for (atx::usize t = 0; t + 1 < T; ++t) {
    for (Eigen::Index i = 0; i < ns; ++i) {
      gamma_den_A(i) += std::exp(log_gamma(static_cast<Eigen::Index>(t), i));
      for (Eigen::Index j = 0; j < ns; ++j) {
        const atx::f64 lxi = la(static_cast<Eigen::Index>(t), i) + logA_prev(i, j) +
                             logB(static_cast<Eigen::Index>(t) + 1, j) +
                             lb(static_cast<Eigen::Index>(t) + 1, j) - loglik;
        xi_num(i, j) += std::exp(lxi);
      }
    }
  }
  for (Eigen::Index i = 0; i < ns; ++i) {
    const atx::f64 den = gamma_den_A(i);
    if (den <= 0.0 || !std::isfinite(den)) {
      continue; // dead state: keep the previous row (guards log(0) / div-by-0)
    }
    for (Eigen::Index j = 0; j < ns; ++j) {
      const atx::f64 a = xi_num(i, j) / den;
      h.logA(i, j) = (a > 0.0) ? std::log(a) : neg_inf(); // exact-zero -> -inf
    }
  }

  // --- M-step: per-state diagonal Gaussian from gamma responsibilities ---
  // mean_s = Σ_t γ_ts x_t / Σ_t γ_ts;  var_s = Σ_t γ_ts (x_t-mean_s)^2 / Σ_t γ_ts.
  for (Eigen::Index s = 0; s < ns; ++s) {
    atx::f64 gsum = 0.0;
    hmm_lin::VecX msum = hmm_lin::VecX::Zero(d);
    for (atx::usize t = 0; t < T; ++t) {
      const atx::f64 g = std::exp(log_gamma(static_cast<Eigen::Index>(t), s));
      gsum += g;
      for (Eigen::Index j = 0; j < d; ++j) {
        msum(j) += g * obs(static_cast<Eigen::Index>(t), j);
      }
    }
    if (gsum <= 0.0 || !std::isfinite(gsum)) {
      continue; // dead state: keep its previous emission deterministically
    }
    Gaussian &gauss = h.emit[static_cast<atx::usize>(s)];
    for (Eigen::Index j = 0; j < d; ++j) {
      gauss.mean(j) = msum(j) / gsum;
    }
    hmm_lin::VecX vsum = hmm_lin::VecX::Zero(d);
    for (atx::usize t = 0; t < T; ++t) {
      const atx::f64 g = std::exp(log_gamma(static_cast<Eigen::Index>(t), s));
      for (Eigen::Index j = 0; j < d; ++j) {
        const atx::f64 dv = obs(static_cast<Eigen::Index>(t), j) - gauss.mean(j);
        vsum(j) += g * dv * dv;
      }
    }
    for (Eigen::Index j = 0; j < d; ++j) {
      const atx::f64 v = vsum(j) / gsum;
      gauss.var(j) = (v < var_floor) ? var_floor : v; // floor so var > 0 always
    }
  }
  return loglik;
}

} // namespace hmm_detail

// ===========================================================================
//  baum_welch — EM fit: seeded init, then log-space E/M steps to convergence.
//
//  Iterates em_step until |Δloglik| < cfg.tol or cfg.max_iter is reached. The
//  loglik returned by em_step is at the params BEFORE that step's M-update, so the
//  loop tracks the monotone EM ascent (each accepted step cannot decrease it). The
//  fit is byte-identical for a fixed (obs, cfg) — the init is seeded (M1) and every
//  reduction is order-fixed.
// ===========================================================================
[[nodiscard]] inline Hmm baum_welch(const hmm_lin::MatX &obs, const HmmCfg &cfg) {
  ATX_CHECK(obs.rows() > 0);     // an empty series has no observation to fit
  ATX_CHECK(cfg.n_states > 0U);  // at least one regime
  Hmm h = hmm_detail::seeded_init(obs, cfg);
  atx::f64 prev = hmm_detail::neg_inf();
  for (atx::u32 it = 0; it < cfg.max_iter; ++it) {
    const atx::f64 ll = hmm_detail::em_step(h, obs, cfg.var_floor);
    if (std::isfinite(prev) && std::isfinite(ll) && (ll - prev < cfg.tol) &&
        (prev - ll < cfg.tol)) {
      break; // |Δloglik| < tol -> converged
    }
    prev = ll;
  }
  return h;
}

// ===========================================================================
//  forward_log — forward log-alpha (T x n_states) + total log-likelihood (M4 LHS).
// ===========================================================================
[[nodiscard]] inline ForwardResult forward_log(const Hmm &hmm, const hmm_lin::MatX &obs) {
  ForwardResult out;
  const atx::usize T = static_cast<atx::usize>(obs.rows());
  out.log_alpha = hmm_detail::forward_alpha_prefix(hmm, obs, T);
  out.loglik = hmm_detail::loglik_from_alpha(out.log_alpha);
  return out;
}

// ===========================================================================
//  backward_loglik — total log-likelihood from log-beta (M4 RHS), computed
//  INDEPENDENTLY of forward_log: backward_loglik = logsumexp_s( logpi(s) +
//  logB(0,s) + log_beta(0,s) ). This MUST equal forward_log's loglik (the M4
//  identity), and because it never calls the forward recursion the equality is a
//  genuine cross-check of the two passes.
// ===========================================================================
[[nodiscard]] inline atx::f64 backward_loglik(const Hmm &hmm, const hmm_lin::MatX &obs) {
  const Eigen::Index ns = static_cast<Eigen::Index>(hmm.n_states);
  const hmm_lin::MatX logB = hmm_detail::emission_logp(hmm, obs);
  const hmm_lin::MatX lb = hmm_detail::backward_beta(hmm, obs, logB);
  std::vector<atx::f64> first(static_cast<atx::usize>(ns));
  for (Eigen::Index s = 0; s < ns; ++s) {
    first[static_cast<atx::usize>(s)] = hmm.logpi(s) + logB(0, s) + lb(0, s);
  }
  return hmm_detail::logsumexp(std::span<const atx::f64>{first});
}

// ===========================================================================
//  posterior_decode — in-sample smoothed-posterior regime labels (M4 recovery).
//
//  γ(t,s) ∝ α(t,s) β(t,s); the decoded label at t is argmax_s γ(t,s). Because the
//  per-t log-evidence (loglik) is a constant shift across s, argmax over the
//  SMOOTHED log-gamma equals argmax over (log_alpha + log_beta) — so no
//  normalization is needed for the decode. Ties break to the lower state index
//  (deterministic). Returns length-T state labels.
// ===========================================================================
[[nodiscard]] inline std::vector<atx::u32> posterior_decode(const Hmm &hmm,
                                                            const hmm_lin::MatX &obs) {
  const atx::usize T = static_cast<atx::usize>(obs.rows());
  const Eigen::Index ns = static_cast<Eigen::Index>(hmm.n_states);
  const hmm_lin::MatX logB = hmm_detail::emission_logp(hmm, obs);
  const hmm_lin::MatX la = hmm_detail::forward_alpha_prefix(hmm, obs, T);
  const hmm_lin::MatX lb = hmm_detail::backward_beta(hmm, obs, logB);
  std::vector<atx::u32> labels(T, 0U);
  for (atx::usize t = 0; t < T; ++t) {
    atx::f64 best = hmm_detail::neg_inf();
    atx::u32 arg = 0U;
    for (Eigen::Index s = 0; s < ns; ++s) {
      const atx::f64 g = la(static_cast<Eigen::Index>(t), s) + lb(static_cast<Eigen::Index>(t), s);
      if (g > best) { // strictly-greater -> lower index wins a tie (deterministic)
        best = g;
        arg = static_cast<atx::u32>(s);
      }
    }
    labels[t] = arg;
  }
  return labels;
}

// ===========================================================================
//  regime_posterior_at — PIT regime posterior (M2, the load-bearing seam).
//
//  P(state | obs[0..d]) — a FORWARD-ONLY filter. It runs the forward recursion
//  over the causal prefix obs[0..d] (length d+1) and normalizes log_alpha at the
//  LAST row (t=d) via logsumexp -> exp -> a probability vector summing to 1. The
//  forward recursion at row d depends ONLY on obs rows [0..d], so the result is
//  truncation-invariant: identical whether obs has d+1 rows or many more. It
//  NEVER reads obs rows > d (forward_alpha_prefix is given T_use = d+1, and
//  emission_logp inside it only touches the prefix rows it iterates).
// ===========================================================================
[[nodiscard]] inline hmm_lin::VecX regime_posterior_at(const Hmm &fitted, const hmm_lin::MatX &obs,
                                                       atx::usize d) {
  ATX_CHECK(static_cast<atx::usize>(obs.rows()) > d); // row d must exist in obs
  const Eigen::Index ns = static_cast<Eigen::Index>(fitted.n_states);
  // Forward over the causal prefix [0..d] only (length d+1). Causality is made
  // STRUCTURAL by copying obs[0..d] into a fresh prefix and running the forward
  // recursion over THAT — so emission_logp and the recursion physically cannot
  // touch any row > d. The posterior below reads only la row d.
  hmm_lin::MatX prefix(static_cast<Eigen::Index>(d + 1), obs.cols());
  for (atx::usize t = 0; t <= d; ++t) {
    for (Eigen::Index j = 0; j < obs.cols(); ++j) {
      prefix(static_cast<Eigen::Index>(t), j) = obs(static_cast<Eigen::Index>(t), j);
    }
  }
  const hmm_lin::MatX la = hmm_detail::forward_alpha_prefix(fitted, prefix, d + 1);

  // Normalize the last row (t=d) in log-space, then exp -> a prob vector.
  std::vector<atx::f64> last(static_cast<atx::usize>(ns));
  for (Eigen::Index s = 0; s < ns; ++s) {
    last[static_cast<atx::usize>(s)] = la(static_cast<Eigen::Index>(d), s);
  }
  const atx::f64 lse = hmm_detail::logsumexp(std::span<const atx::f64>{last});
  hmm_lin::VecX post(ns);
  for (Eigen::Index s = 0; s < ns; ++s) {
    post(s) = std::exp(last[static_cast<atx::usize>(s)] - lse);
  }
  return post;
}

} // namespace atx::engine::learn
