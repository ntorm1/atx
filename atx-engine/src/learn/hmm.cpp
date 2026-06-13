#include "atx/engine/learn/hmm.hpp"

#include <utility> // std::move

#include "atx/core/random.hpp" // atx::core::Xoshiro256pp

#include "atx/engine/learn/train.hpp" // seed_for

namespace atx::engine::learn {

namespace hmm_detail {

hmm_lin::MatX backward_beta(const Hmm &h, const hmm_lin::MatX &obs,
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

void global_stats(const hmm_lin::MatX &obs, hmm_lin::VecX &mean_out,
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

Hmm seeded_init(const hmm_lin::MatX &obs, const HmmCfg &cfg) {
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

atx::f64 em_step(Hmm &h, const hmm_lin::MatX &obs, atx::f64 var_floor) {
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

Hmm baum_welch(const hmm_lin::MatX &obs, const HmmCfg &cfg) {
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

atx::f64 backward_loglik(const Hmm &hmm, const hmm_lin::MatX &obs) {
  const Eigen::Index ns = static_cast<Eigen::Index>(hmm.n_states);
  const hmm_lin::MatX logB = hmm_detail::emission_logp(hmm, obs);
  const hmm_lin::MatX lb = hmm_detail::backward_beta(hmm, obs, logB);
  std::vector<atx::f64> first(static_cast<atx::usize>(ns));
  for (Eigen::Index s = 0; s < ns; ++s) {
    first[static_cast<atx::usize>(s)] = hmm.logpi(s) + logB(0, s) + lb(0, s);
  }
  return hmm_detail::logsumexp(std::span<const atx::f64>{first});
}

std::vector<atx::u32> posterior_decode(const Hmm &hmm,
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

} // namespace atx::engine::learn
