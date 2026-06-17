// atx::engine::learn — log-space Baum-Welch HMM + PIT regime posterior tests (S5-5).
//
// Suite Hmm — the load-bearing semantics of the deterministic, log-space
// Baum-Welch HMM (Pattern-B edge 3):
//
//   1. ForwardLogLik_EqualsBackwardLogLik (M4 — the algorithm's own identity) —
//      after a fit on a toy 2-state series, forward_log's total log-likelihood
//      equals backward_loglik to ~1e-8. NON-VACUOUS: the two are computed by
//      INDEPENDENT recursions (forward alpha vs backward beta), so the equality
//      is a real cross-check, not a tautology.
//   2. RecoversPlantedRegimes_AboveChance (M4 — vs a known generator) — a planted
//      2-regime series (regime A: mean 0 low-var; regime B: mean +3 higher-var,
//      with persistent runs); a fit + posterior decode agrees with the planted
//      truth > 0.8 under a permutation-robust match (HMM state labels are
//      arbitrary, so both label assignments are tried and the max taken).
//   3. Posterior_FitOnTrailing_Causal_TruncationInvariant (M2 — the load-bearing
//      one) — the PIT posterior at t=149 is byte-identical whether obs has 150
//      rows or 300, proving regime_posterior_at reads ONLY obs rows [0..149].
//   4. SameSeed_ByteIdenticalParams (M1) — two fits on the same obs + same seed
//      produce an identical param hash (logA + each Gaussian mean/var + logpi).
//      NON-VACUOUS: any param byte change shifts the digest.
//
// Fixtures are MatX (T x d) observation matrices built directly. The fixture
// noise uses a test-local deterministic LCG (mirrors gbt_test / linear_alpha_test),
// NOT std::rand. Naming: Subject_Condition_Expected.

#include <algorithm> // std::max
#include <vector>

#include <Eigen/Dense> // Eigen::Index, MatX/VecX

#include <gtest/gtest.h>

#include "atx/core/hash.hpp"          // atx::core::hash_bytes
#include "atx/core/linalg/linalg.hpp" // MatX, VecX
#include "atx/core/types.hpp"         // f64, u32, u64, usize

#include "atx/engine/learn/hmm.hpp" // HmmCfg, Hmm, baum_welch, forward_log, backward_loglik, ...

namespace atxtest_hmm_test {

using atx::f64;
using atx::u32;
using atx::u64;
using atx::usize;
using atx::core::linalg::MatX;
using atx::core::linalg::VecX;
using atx::engine::learn::Hmm;
using atx::engine::learn::HmmCfg;
namespace learn = atx::engine::learn;

// A small deterministic LCG so fixtures carry reproducible "noise" without an RNG
// dependency; pure function of its state (test-local). Mirrors the S5-3/S5-4 fixture.
struct Lcg {
  u64 s;
  [[nodiscard]] f64 next() noexcept {
    s = s * 6364136223846793005ULL + 1442695040888963407ULL;
    const u64 top = s >> 11U;
    const f64 u = static_cast<f64>(top) / static_cast<f64>(1ULL << 53U);
    return 2.0 * u - 1.0; // [-1, 1)
  }
  // Approx standard normal via a sum of 12 uniforms (Irwin-Hall, mean 0, var 1).
  [[nodiscard]] f64 normal() noexcept {
    f64 acc = 0.0;
    for (usize k = 0; k < 12U; ++k) {
      acc += 0.5 * (next() + 1.0); // each uniform in [0, 1)
    }
    return acc - 6.0;
  }
};

// A toy 2-state, 1-dim observation series of length T: alternating runs of a
// low-mean and a high-mean Gaussian, with light noise. Used to exercise the
// forward==backward identity (it just needs to be a non-degenerate fit target).
[[nodiscard]] MatX two_state_toy_observations(usize T, u64 seed) {
  MatX obs(static_cast<Eigen::Index>(T), 1);
  Lcg rng{seed};
  for (usize t = 0; t < T; ++t) {
    const bool high = ((t / 20U) % 2U) == 1U; // runs of length 20
    const f64 mean = high ? 2.0 : 0.0;
    obs(static_cast<Eigen::Index>(t), 0) = mean + 0.3 * rng.normal();
  }
  return obs;
}

// A planted 2-regime, 1-dim series with PERSISTENT runs, parameterized by the
// regime-B mean/sd so the SAME generator drives both an easy (well-separated) and
// a hard (weakly-separated) recovery fixture. Regime A is fixed at mean 0, sd 0.3;
// regime B is mean_b / sd_b. Persistence is encoded by switching regime only at
// fixed run boundaries (deterministic) — what an HMM's self-transition models —
// so the planted truth stays crisp regardless of separation.
[[nodiscard]] MatX planted_two_regime_series_sep(usize T, u64 seed, f64 mean_b, f64 sd_b,
                                                 std::vector<u32> &truth_out) {
  MatX obs(static_cast<Eigen::Index>(T), 1);
  truth_out.assign(T, 0U);
  Lcg rng{seed};
  const usize run = 25U;
  for (usize t = 0; t < T; ++t) {
    const bool b = ((t / run) % 2U) == 1U;
    truth_out[t] = b ? 1U : 0U;
    const f64 mean = b ? mean_b : 0.0;
    const f64 sd = b ? sd_b : 0.3;
    obs(static_cast<Eigen::Index>(t), 0) = mean + sd * rng.normal();
  }
  return obs;
}

// The well-separated default fixture (mean +3, sd 0.6 ≈ 5σ gap) used by the PIT
// and easy-recovery tests.
[[nodiscard]] MatX planted_two_regime_series(usize T, u64 seed, std::vector<u32> &truth_out) {
  return planted_two_regime_series_sep(T, seed, /*mean_b=*/3.0, /*sd_b=*/0.6, truth_out);
}

// Permutation-robust agreement between a decode and the planted truth: HMM state
// labels are arbitrary, so try BOTH assignments (identity and swapped) and take
// the max fraction. Both vectors are length T over {0, 1}.
[[nodiscard]] f64 regime_agreement(const std::vector<u32> &decode, const std::vector<u32> &truth) {
  const usize n = decode.size();
  usize same = 0U;
  for (usize i = 0; i < n; ++i) {
    if (decode[i] == truth[i]) {
      ++same;
    }
  }
  const f64 frac_id = static_cast<f64>(same) / static_cast<f64>(n);
  const f64 frac_swap = 1.0 - frac_id; // {0,1} two-state: swapped agreement is the complement
  return std::max(frac_id, frac_swap);
}

// The top-N rows of a (T x d) matrix as a fresh (N x d) matrix (the truncation a
// causal posterior must be invariant to). N <= T required by the caller.
[[nodiscard]] MatX top_rows(const MatX &m, usize n) {
  MatX out(static_cast<Eigen::Index>(n), m.cols());
  for (usize i = 0; i < n; ++i) {
    for (Eigen::Index j = 0; j < m.cols(); ++j) {
      out(static_cast<Eigen::Index>(i), j) = m(static_cast<Eigen::Index>(i), j);
    }
  }
  return out;
}

// Hash a fitted HMM's parameters (logA + each Gaussian mean/var + logpi) into one
// digest. Non-vacuous: any param byte change shifts the buffer and the digest.
[[nodiscard]] u64 hash_hmm(const Hmm &h) {
  std::vector<f64> buf;
  buf.push_back(static_cast<f64>(h.n_states));
  buf.push_back(static_cast<f64>(h.n_dims));
  for (Eigen::Index i = 0; i < h.logA.rows(); ++i) {
    for (Eigen::Index j = 0; j < h.logA.cols(); ++j) {
      buf.push_back(h.logA(i, j));
    }
  }
  for (const auto &g : h.emit) {
    for (Eigen::Index i = 0; i < g.mean.size(); ++i) {
      buf.push_back(g.mean(i));
    }
    for (Eigen::Index i = 0; i < g.var.size(); ++i) {
      buf.push_back(g.var(i));
    }
  }
  for (Eigen::Index i = 0; i < h.logpi.size(); ++i) {
    buf.push_back(h.logpi(i));
  }
  // SAFETY: std::vector<f64> stores doubles contiguously; buf.data() points at
  // buf.size()*sizeof(f64) live bytes for the call's duration.
  return atx::core::hash_bytes(buf.data(), buf.size() * sizeof(f64));
}

[[nodiscard]] u64 hash_vec(const VecX &v) {
  // SAFETY: Eigen VecX stores doubles contiguously; v.data() points at
  // size()*sizeof(f64) live bytes for the vector's lifetime.
  return atx::core::hash_bytes(v.data(), static_cast<usize>(v.size()) * sizeof(f64));
}

[[nodiscard]] HmmCfg standard_cfg() {
  HmmCfg cfg;
  cfg.n_states = 2U;
  cfg.max_iter = 100U;
  cfg.tol = 1e-7;
  cfg.master_seed = 42ULL;
  return cfg;
}

// ---- 1: M4 forward==backward log-likelihood identity --------------------------

TEST(Hmm, ForwardLogLik_EqualsBackwardLogLik) {
  const MatX obs = two_state_toy_observations(/*T=*/200U, /*seed=*/13ULL);
  const Hmm hmm = learn::baum_welch(obs, standard_cfg());

  const f64 fwd = learn::forward_log(hmm, obs).loglik;
  const f64 bwd = learn::backward_loglik(hmm, obs);
  // The two are computed by INDEPENDENT recursions (alpha vs beta), so equality is
  // a genuine cross-check of the algorithm, not a tautology.
  EXPECT_NEAR(fwd, bwd, 1e-8) << "forward loglik=" << fwd << " backward loglik=" << bwd;
}

// ---- 2: M4 recovery vs a known generator (permutation-robust) -----------------

TEST(Hmm, RecoversPlantedRegimes_AboveChance) {
  std::vector<u32> truth;
  const MatX obs = planted_two_regime_series(/*T=*/300U, /*seed=*/7ULL, truth);
  const Hmm hmm = learn::baum_welch(obs, standard_cfg());
  const std::vector<u32> decode = learn::posterior_decode(hmm, obs);
  ASSERT_EQ(decode.size(), truth.size());

  // Well-separated (~5σ) fixture: a correct fit recovers the planted runs nearly
  // perfectly. The bar is set high (0.95, not a loose 0.8) so a kernel that only
  // half-works — or decodes near chance — genuinely FAILS this test.
  const f64 agree = regime_agreement(decode, truth);
  EXPECT_GT(agree, 0.95) << "fitted regimes must recover the planted truth: agreement=" << agree;
}

// A HARDER recovery fixture: regime B is only mean +1.2 / sd 0.6 over regime A
// (mean 0 / sd 0.3) — a ~2σ overlap where a single-threshold classifier struggles
// but the HMM's temporal self-transition prior should still pull decode well above
// chance. This makes the recovery claim load-bearing on a non-trivial separation,
// not just the auto-passing wide-gap case.
TEST(Hmm, RecoversWeaklySeparatedRegimes_AboveChance) {
  std::vector<u32> truth;
  const MatX obs =
      planted_two_regime_series_sep(/*T=*/400U, /*seed=*/7ULL, /*mean_b=*/1.2, /*sd_b=*/0.6, truth);
  const Hmm hmm = learn::baum_welch(obs, standard_cfg());
  const std::vector<u32> decode = learn::posterior_decode(hmm, obs);
  ASSERT_EQ(decode.size(), truth.size());

  const f64 agree = regime_agreement(decode, truth);
  EXPECT_GT(agree, 0.75) << "weakly-separated regimes must still recover above chance: agreement="
                         << agree;
}

// ---- 3: M2 PIT posterior — causal + truncation-invariant ----------------------

TEST(Hmm, Posterior_FitOnTrailing_Causal_TruncationInvariant) {
  std::vector<u32> truth;
  const MatX full = planted_two_regime_series(/*T=*/300U, /*seed=*/7ULL, truth);
  // Fit on a trailing window (first 200 rows) — the fitted params are what the PIT
  // filter consumes; the truncation invariance is about the FORWARD filter, which
  // depends only on obs rows [0..d].
  const MatX fit_obs = top_rows(full, 200U);
  const Hmm hmm = learn::baum_welch(fit_obs, standard_cfg());

  const usize d = 149U;
  const MatX truncated = top_rows(full, 150U); // exactly d+1 rows
  const VecX p_trunc = learn::regime_posterior_at(hmm, truncated, d);
  const VecX p_full = learn::regime_posterior_at(hmm, full, d);

  ASSERT_EQ(p_trunc.size(), p_full.size());
  // Byte-identical: the posterior at d=149 reads only rows [0..149], so adding the
  // 150..299 rows cannot change a single bit.
  EXPECT_EQ(hash_vec(p_trunc), hash_vec(p_full))
      << "PIT posterior at d must be invariant to obs rows > d";

  // It is a genuine probability vector (sums to 1, non-negative).
  f64 sum = 0.0;
  for (Eigen::Index i = 0; i < p_trunc.size(); ++i) {
    EXPECT_GE(p_trunc(i), 0.0);
    sum += p_trunc(i);
  }
  EXPECT_NEAR(sum, 1.0, 1e-9);
}

// ---- 4: M1 determinism — same seed -> byte-identical params -------------------

TEST(Hmm, SameSeed_ByteIdenticalParams) {
  const MatX obs = two_state_toy_observations(/*T=*/200U, /*seed=*/21ULL);
  const HmmCfg cfg = standard_cfg();

  const Hmm a = learn::baum_welch(obs, cfg);
  const Hmm b = learn::baum_welch(obs, cfg);
  EXPECT_EQ(hash_hmm(a), hash_hmm(b)) << "same seed must produce byte-identical HMM params";
}


}  // namespace atxtest_hmm_test
