// atx::engine::learn — Attention1Head layer + fit_attn alpha (p2 S5-3a) tests.
//
// TDD pins for the ATTENTION-LITE sequence alpha: a single-head causal
// scaled-dot-product attention layer (seq_layers) + the fit_attn alpha that reuses
// the committed fit_seq_alpha core (exactly like TCN/GRU). Suite `LearnAttnAlpha`.
//
// LOAD-BEARING proofs:
//   R3  finite-difference gradient check on Attention1Head for EVERY param
//       (Wq,Wk,Wv,biases) AND dL/dx (rel-err <= ~1e-5, f64, dropout off). The
//       analytic backward — softmax Jacobian + the masked attention adjoint — is
//       the autodiff replacement; FD vs analytic is the only correctness proof.
//       This is the #1 risk (the softmax adjoint).
//   R2  causal structural test: perturbing the INPUT at steps > t leaves the
//       attention output at step t BIT-IDENTICAL (the causal -inf mask guarantees
//       output[t] is independent of future inputs). Tested at d_model > 1.
//   Forward reference: vs a hand-rolled naive single-head causal-attention
//       reference (Q/K/V, masked softmax, A·V) on a tiny fixed window.
//   fit_attn: happy path (Ok, kind Attn, member_states == ensemble_size, non-empty
//       oos_score_series, trial_count > 0, blend_w normalized); R1 two-builds
//       byte-identical; R2 truncation-invariant predict; seed-ensemble mean ==
//       predict_nn; predict_blended dispatch; InvalidArgument guards.
//
// The FD harness mirrors learn_nn_seq_layers_test.cpp; the SequenceTensor fixture
// mirrors learn_tcn_gru_alpha_test.cpp (constructed directly for full byte control).

#include <cmath>   // std::fabs, std::exp, std::sqrt, std::isfinite
#include <cstring> // std::memcmp
#include <limits>  // std::numeric_limits
#include <span>
#include <vector>

#include <gtest/gtest.h>

#include "atx/core/error.hpp"  // ErrorCode
#include "atx/core/random.hpp" // Xoshiro256pp
#include "atx/core/types.hpp"  // f64, u8, u16, u64, usize

#include "atx/core/linalg/linalg.hpp" // MatX

#include "atx/engine/learn/learned_source.hpp"    // LearnedModel, ModelKind, predict_nn, predict_blended
#include "atx/engine/learn/nn/module.hpp"
#include "atx/engine/learn/nn/seq_layers.hpp"     // nn::Attention1Head
#include "atx/engine/learn/sequence_features.hpp" // SequenceTensor
#include "atx/engine/learn/tcn_alpha.hpp"         // fit_attn, AttnAlphaCfg

namespace atxtest_learn_attn_alpha_test {

using atx::f64;
using atx::u16;
using atx::u64;
using atx::u8;
using atx::usize;
using atx::engine::learn::AttnAlphaCfg;
using atx::engine::learn::fit_attn;
using atx::engine::learn::LearnedModel;
using atx::engine::learn::ModelKind;
using atx::engine::learn::predict_blended;
using atx::engine::learn::predict_nn;
using atx::engine::learn::SequenceTensor;
namespace nn = atx::engine::learn::nn;
namespace lin = atx::core::linalg;

// ===========================================================================
//  FD machinery (lifted from learn_nn_seq_layers_test.cpp)
// ===========================================================================

[[nodiscard]] lin::MatX random_mat(Eigen::Index rows, Eigen::Index cols, u64 seed) {
  atx::core::Xoshiro256pp rng{seed};
  lin::MatX m(rows, cols);
  for (Eigen::Index c = 0; c < cols; ++c) {
    for (Eigen::Index r = 0; r < rows; ++r) {
      m(r, c) = rng.normal();
    }
  }
  return m;
}

// L = sum(forward(x) .* gout): dL/d(output) == gout, so backward(gout) yields the
// true dL/dx and dL/dparam that central differences must match.
[[nodiscard]] f64 surrogate_loss(nn::Module &m, const lin::MatX &x, const lin::MatX &gout) {
  const lin::MatX y = m.forward(x);
  return (y.array() * gout.array()).sum();
}

[[nodiscard]] f64 rel_err(f64 a, f64 b) {
  const f64 denom = std::fabs(a) + std::fabs(b) + 1e-8;
  return std::fabs(a - b) / denom;
}

constexpr f64 k_eps = 1e-6;
constexpr f64 k_grad_tol = 1e-5;
// Absolute floor for the FD comparison. The single-head causal attention has an
// EXACTLY-zero analytic gradient for the K-projection bias: bk[o] shifts every
// K[.,o] by a constant, and the softmax row-sum identity Σ_j dS[i,j] == 0 makes
// dbk[o] = Σ_i (Q[i,o]/√d) Σ_{j<=i} dS[i,j] == 0. The matching FD is pure rounding
// noise (~1e-10 off an O(1) surrogate loss), so a relative-error test on two near-
// zeros is ill-conditioned. When BOTH analytic and FD sit below this absolute
// floor the gradient is treated as matched (a standard FD-check refinement; it
// never masks a genuine non-zero gradient, which the relative test still guards).
constexpr f64 k_grad_abs_floor = 1e-7;

void check_param_grads(nn::Module &m, std::span<f64> params, std::span<f64> grads,
                       const lin::MatX &x, const lin::MatX &gout, f64 eps = k_eps) {
  for (f64 &g : grads) {
    g = 0.0;
  }
  static_cast<void>(m.forward(x));
  static_cast<void>(m.backward(gout));
  const std::vector<f64> analytic(grads.begin(), grads.end());

  for (usize i = 0; i < params.size(); ++i) {
    const f64 orig = params[i];
    params[i] = orig + eps;
    const f64 lp = surrogate_loss(m, x, gout);
    params[i] = orig - eps;
    const f64 lm = surrogate_loss(m, x, gout);
    params[i] = orig;
    const f64 fd = (lp - lm) / (2.0 * eps);
    if (std::fabs(analytic[i]) < k_grad_abs_floor && std::fabs(fd) < k_grad_abs_floor) {
      continue; // both effectively zero — see k_grad_abs_floor (exact-zero K-bias).
    }
    EXPECT_LT(rel_err(analytic[i], fd), k_grad_tol)
        << "param grad mismatch at " << i << " analytic=" << analytic[i] << " fd=" << fd;
  }
}

void check_input_grads(nn::Module &m, const lin::MatX &x, const lin::MatX &gout, f64 eps = k_eps) {
  static_cast<void>(m.forward(x));
  const lin::MatX dx = m.backward(gout);

  lin::MatX xp = x;
  for (Eigen::Index c = 0; c < x.cols(); ++c) {
    for (Eigen::Index r = 0; r < x.rows(); ++r) {
      const f64 orig = x(r, c);
      xp(r, c) = orig + eps;
      const f64 lp = surrogate_loss(m, xp, gout);
      xp(r, c) = orig - eps;
      const f64 lm = surrogate_loss(m, xp, gout);
      xp(r, c) = orig;
      const f64 fd = (lp - lm) / (2.0 * eps);
      EXPECT_LT(rel_err(dx(r, c), fd), k_grad_tol)
          << "input grad mismatch at (" << r << "," << c << ")";
    }
  }
}

struct BoundLeaf {
  std::vector<f64> params;
  std::vector<f64> grads;
};

// Bind a leaf to fresh buffers and seed-init params with small normals.
[[nodiscard]] BoundLeaf bind_and_init(nn::Module &m, u64 seed, f64 scale = 0.5) {
  BoundLeaf b;
  b.params.assign(m.param_count(), 0.0);
  b.grads.assign(m.param_count(), 0.0);
  m.bind_params(std::span<f64>(b.params), std::span<f64>(b.grads));
  atx::core::Xoshiro256pp rng{seed};
  for (f64 &p : b.params) {
    p = scale * rng.normal();
  }
  return b;
}

// ===========================================================================
//  Attention1Head — finite-difference gradient checks (R3, the #1 risk).
//  Covers Wq,Wk,Wv,biases AND dL/dx over the masked softmax + A·V adjoint.
// ===========================================================================

TEST(LearnAttnAlpha, Attention_GradCheck_Biased) {
  // B=3, T=5, C_in=2, d_model=4, bias on.
  const usize T = 5, C = 2, D = 4;
  nn::Attention1Head attn{T, C, D, /*bias=*/true};
  EXPECT_EQ(attn.param_count(), 3U * C * D + 3U * D); // 3*Wq/Wk/Wv + 3 biases
  BoundLeaf b = bind_and_init(attn, 71, 0.3);
  const lin::MatX x = random_mat(3, static_cast<Eigen::Index>(T * C), 801);
  const lin::MatX g = random_mat(3, static_cast<Eigen::Index>(T * D), 802);
  check_param_grads(attn, b.params, b.grads, x, g); // softmax + attention adjoint
  check_input_grads(attn, x, g);
}

TEST(LearnAttnAlpha, Attention_GradCheck_NoBias) {
  const usize T = 4, C = 3, D = 3;
  nn::Attention1Head attn{T, C, D, /*bias=*/false};
  EXPECT_EQ(attn.param_count(), 3U * C * D); // no biases
  BoundLeaf b = bind_and_init(attn, 72, 0.3);
  const lin::MatX x = random_mat(2, static_cast<Eigen::Index>(T * C), 803);
  const lin::MatX g = random_mat(2, static_cast<Eigen::Index>(T * D), 804);
  check_param_grads(attn, b.params, b.grads, x, g);
  check_input_grads(attn, x, g);
}

// ===========================================================================
//  Attention1Head — R2 causal structural test (no look-ahead). Perturbing input
//  at steps > t_probe leaves output at every t <= t_probe BIT-IDENTICAL.
// ===========================================================================

TEST(LearnAttnAlpha, Attention_Causal_NoLookAhead) {
  const usize T = 6, C = 2, D = 3; // d_model > 1
  nn::Attention1Head attn{T, C, D, /*bias=*/true};
  BoundLeaf b = bind_and_init(attn, 73, 0.4);
  const lin::MatX x = random_mat(2, static_cast<Eigen::Index>(T * C), 805);
  const lin::MatX y0 = attn.forward(x);

  const usize t_probe = 2;
  lin::MatX xp = x;
  for (usize t = t_probe + 1; t < T; ++t) {
    for (usize ci = 0; ci < C; ++ci) {
      for (Eigen::Index r = 0; r < xp.rows(); ++r) {
        xp(r, static_cast<Eigen::Index>(t * C + ci)) += 9.0; // large future perturbation
      }
    }
  }
  const lin::MatX y1 = attn.forward(xp);
  for (usize t = 0; t <= t_probe; ++t) {
    for (usize o = 0; o < D; ++o) {
      for (Eigen::Index r = 0; r < y0.rows(); ++r) {
        const Eigen::Index col = static_cast<Eigen::Index>(t * D + o);
        EXPECT_EQ(y0(r, col), y1(r, col)) // BIT-identical
            << "future input leaked into attention output at t=" << t;
      }
    }
  }
}

// ===========================================================================
//  Attention1Head — forward reference vs a hand-rolled naive causal single-head
//  attention (Q/K/V projections, masked row-softmax, A·V) on a fixed window.
// ===========================================================================

// Naive reference: Q=X·Wq, K=X·Wk, V=X·Wv (each T×D); S[i,j]=(Q_i·K_j)/√D for j<=i;
// A=softmax(S) row-wise (over j<=i); O=A·V. Weight flat index (ci*D + o), param
// layout [Wq][Wk][Wv][bq][bk][bv], matching the layer's convention.
[[nodiscard]] lin::MatX naive_causal_attention(const lin::MatX &x, std::span<const f64> p,
                                               usize T, usize C, usize D, bool bias) {
  const usize cd = C * D;
  const auto wq = [&](usize ci, usize o) { return p[0 * cd + ci * D + o]; };
  const auto wk = [&](usize ci, usize o) { return p[1 * cd + ci * D + o]; };
  const auto wv = [&](usize ci, usize o) { return p[2 * cd + ci * D + o]; };
  const auto bq = [&](usize o) { return bias ? p[3 * cd + o] : 0.0; };
  const auto bk = [&](usize o) { return bias ? p[3 * cd + D + o] : 0.0; };
  const auto bv = [&](usize o) { return bias ? p[3 * cd + 2 * D + o] : 0.0; };
  const Eigen::Index B = x.rows();
  lin::MatX out(B, static_cast<Eigen::Index>(T * D));
  const f64 inv_scale = 1.0 / std::sqrt(static_cast<f64>(D));
  for (Eigen::Index r = 0; r < B; ++r) {
    // Project Q/K/V.
    std::vector<f64> Q(T * D, 0.0), K(T * D, 0.0), V(T * D, 0.0);
    for (usize t = 0; t < T; ++t) {
      for (usize o = 0; o < D; ++o) {
        f64 q = bq(o), k = bk(o), v = bv(o);
        for (usize ci = 0; ci < C; ++ci) {
          const f64 xv = x(r, static_cast<Eigen::Index>(t * C + ci));
          q += wq(ci, o) * xv;
          k += wk(ci, o) * xv;
          v += wv(ci, o) * xv;
        }
        Q[t * D + o] = q;
        K[t * D + o] = k;
        V[t * D + o] = v;
      }
    }
    // Masked row-softmax attention + A·V.
    for (usize i = 0; i < T; ++i) {
      std::vector<f64> s(i + 1, 0.0);
      f64 smax = -std::numeric_limits<f64>::infinity();
      for (usize j = 0; j <= i; ++j) {
        f64 dotv = 0.0;
        for (usize o = 0; o < D; ++o) {
          dotv += Q[i * D + o] * K[j * D + o];
        }
        s[j] = dotv * inv_scale;
        if (s[j] > smax) {
          smax = s[j];
        }
      }
      f64 denom = 0.0;
      for (usize j = 0; j <= i; ++j) {
        s[j] = std::exp(s[j] - smax);
        denom += s[j];
      }
      for (usize o = 0; o < D; ++o) {
        f64 acc = 0.0;
        for (usize j = 0; j <= i; ++j) {
          acc += (s[j] / denom) * V[j * D + o];
        }
        out(r, static_cast<Eigen::Index>(i * D + o)) = acc;
      }
    }
  }
  return out;
}

TEST(LearnAttnAlpha, Attention_Forward_NaiveReference) {
  const usize T = 4, C = 2, D = 3;
  nn::Attention1Head attn{T, C, D, /*bias=*/true};
  BoundLeaf b = bind_and_init(attn, 74, 0.5);
  const lin::MatX x = random_mat(3, static_cast<Eigen::Index>(T * C), 806);
  const lin::MatX y = attn.forward(x);
  const lin::MatX ref = naive_causal_attention(x, std::span<const f64>{b.params}, T, C, D, true);
  EXPECT_TRUE(y.isApprox(ref, 1e-12)) << "attention forward != naive causal reference";
}

// ===========================================================================
//  fit_attn alpha — fixture (constructed directly, mirroring the TCN/GRU test).
// ===========================================================================

struct SynthCfg {
  usize n_dates;
  usize n_inst;
  usize L;
  usize F;
  usize n_horizons;
  bool planted;
  u64 seed;
};

SequenceTensor make_seq(const SynthCfg &c) {
  SequenceTensor st;
  st.lookback = c.L;
  st.n_features = c.F;
  st.y.assign(c.n_horizons, {});
  atx::core::Xoshiro256pp rng{c.seed};
  for (usize d = c.L - 1; d < c.n_dates; ++d) {
    for (usize i = 0; i < c.n_inst; ++i) {
      std::vector<f64> window(c.L * c.F, 0.0);
      for (usize l = 0; l < c.L; ++l) {
        const usize wd = d - (c.L - 1) + l;
        for (usize f = 0; f < c.F; ++f) {
          const f64 smooth = 0.1 * static_cast<f64>(wd) + 0.3 * static_cast<f64>(i) +
                             0.05 * static_cast<f64>(f);
          window[l * c.F + f] = smooth + 0.2 * rng.normal();
        }
      }
      const f64 trailing0 = window[(c.L - 1) * c.F + 0];
      for (usize h = 0; h < c.n_horizons; ++h) {
        const f64 label = c.planted ? (trailing0 + 0.01 * static_cast<f64>(h)) : rng.normal();
        st.y[h].push_back(label);
      }
      for (usize j = 0; j < window.size(); ++j) {
        st.x.push_back(window[j]);
      }
      st.date_of.push_back(d);
      st.inst_of.push_back(i);
      st.sample_valid.push_back(static_cast<u8>(1));
      ++st.n_samples;
    }
  }
  return st;
}

AttnAlphaCfg tiny_attn_cfg() {
  AttnAlphaCfg cfg;
  cfg.d_model = 6;
  cfg.dropout = 0.0; // deterministic + no per-call RNG divergence in tests
  cfg.cpcv.n_groups = 4;
  cfg.cpcv.n_test_groups = 1;
  cfg.cpcv.embargo = 0.0;
  cfg.horizons = {1, 2};
  cfg.train.epochs = 12;
  cfg.train.batch_size = 16;
  cfg.train.ckpt_every = 4;
  cfg.train.ensemble_size = 2;
  cfg.train.master_seed = 4242;
  return cfg;
}

std::vector<f64> window_of(const SequenceTensor &st, usize sample) {
  const usize wlen = st.lookback * st.n_features;
  std::vector<f64> row(wlen);
  for (usize j = 0; j < wlen; ++j) {
    row[j] = st.x[sample * wlen + j];
  }
  return row;
}

constexpr f64 kBlendTol = 1e-12;

// =====================================================================
//  Happy path — fit_attn returns a well-formed deployed model.
// =====================================================================
TEST(LearnAttnAlpha, FitAttn_PlantedSignal_ReturnsWellFormedModel) {
  const SequenceTensor st = make_seq({10, 4, 3, 2, 2, /*planted=*/true, 11});
  const AttnAlphaCfg cfg = tiny_attn_cfg();

  const auto r = fit_attn(st, cfg);
  ASSERT_TRUE(r.has_value()) << r.error().to_string();
  const LearnedModel &m = *r;

  EXPECT_EQ(m.kind, ModelKind::Attn);
  EXPECT_EQ(m.nn.lookback, st.lookback);
  EXPECT_EQ(m.nn.n_seq_features, st.n_features);
  EXPECT_EQ(m.n_base_features, st.lookback * st.n_features);
  EXPECT_EQ(m.augmented_dim(), st.lookback * st.n_features) << "aug empty => augmented_dim == L*F";
  EXPECT_EQ(m.nn.member_states.size(), cfg.train.ensemble_size);
  EXPECT_FALSE(m.oos_score_series.empty()) << "the gate needs a non-empty OOS series";
  EXPECT_GT(m.trial_count, 0U) << "every fold fit must bump the deflation N";
  // arch payload: {d_model} + {dropout}.
  ASSERT_FALSE(m.nn.arch_dims.empty());
  EXPECT_EQ(m.nn.arch_dims[0], cfg.d_model);
  ASSERT_FALSE(m.nn.arch_params.empty());
  EXPECT_DOUBLE_EQ(m.nn.arch_params[0], cfg.dropout);

  ASSERT_EQ(m.blend_w.size(), cfg.horizons.size());
  f64 sum = 0.0;
  for (const f64 w : m.blend_w) {
    EXPECT_GE(w, 0.0);
    sum += w;
  }
  EXPECT_NEAR(sum, 1.0, kBlendTol);
}

// =====================================================================
//  R1 — two builds, same master_seed -> byte-identical member_states AND identical
//  predict_nn on a fixed window.
// =====================================================================
TEST(LearnAttnAlpha, R1_TwoBuilds_ByteIdenticalStatesAndPredict) {
  const SequenceTensor st = make_seq({10, 4, 3, 2, 2, /*planted=*/true, 17});
  const AttnAlphaCfg cfg = tiny_attn_cfg();

  const auto a = fit_attn(st, cfg);
  const auto b = fit_attn(st, cfg);
  ASSERT_TRUE(a.has_value());
  ASSERT_TRUE(b.has_value());
  ASSERT_EQ(a->nn.member_states.size(), b->nn.member_states.size());
  for (usize mi = 0; mi < a->nn.member_states.size(); ++mi) {
    ASSERT_EQ(a->nn.member_states[mi].size(), b->nn.member_states[mi].size());
    EXPECT_EQ(std::memcmp(a->nn.member_states[mi].data(), b->nn.member_states[mi].data(),
                          a->nn.member_states[mi].size() * sizeof(f64)),
              0)
        << "member " << mi << " not byte-identical across builds";
  }
  const std::vector<f64> row = window_of(st, 0);
  const f64 pa = predict_nn(*a, std::span<const f64>{row});
  const f64 pb = predict_nn(*b, std::span<const f64>{row});
  EXPECT_EQ(pa, pb) << "predict_nn must be byte-deterministic across builds";
}

// =====================================================================
//  R2 — truncation invariance: predict_nn on a shared sample's window is identical
//  whether the window bytes come from the full or the truncated source panel.
// =====================================================================
TEST(LearnAttnAlpha, R2_TruncationInvariant_PredictBitIdentical) {
  const usize L = 3, Fdim = 2, H = 2, t_cut = 5;
  const SequenceTensor full = make_seq({9, 3, L, Fdim, H, /*planted=*/true, 23});
  const SequenceTensor trunc = make_seq({t_cut + 1, 3, L, Fdim, H, /*planted=*/true, 23});

  const usize s_full = 0, s_trunc = 0;
  ASSERT_EQ(full.date_of[s_full], trunc.date_of[s_trunc]);
  ASSERT_EQ(full.inst_of[s_full], trunc.inst_of[s_trunc]);
  const std::vector<f64> wf = window_of(full, s_full);
  const std::vector<f64> wt = window_of(trunc, s_trunc);
  ASSERT_EQ(wf.size(), wt.size());
  for (usize j = 0; j < wf.size(); ++j) {
    ASSERT_EQ(wf[j], wt[j]) << "window byte differs at j=" << j << " — S5-1 trailing invariant broke";
  }

  AttnAlphaCfg cfg = tiny_attn_cfg();
  cfg.cpcv.n_groups = 3;
  const auto r = fit_attn(trunc, cfg);
  ASSERT_TRUE(r.has_value());
  const f64 pf = predict_nn(*r, std::span<const f64>{wf});
  const f64 pt = predict_nn(*r, std::span<const f64>{wt});
  EXPECT_EQ(pf, pt) << "causal predict must be invariant to the (identical) window bytes' source";
}

// =====================================================================
//  Seed-ensemble determinism (R1): predict_nn == ascending mean of the per-member
//  forwards.
// =====================================================================
TEST(LearnAttnAlpha, SeedEnsemble_PredictEqualsAscendingMemberMean) {
  SequenceTensor st = make_seq({10, 4, 3, 2, 2, /*planted=*/true, 29});
  AttnAlphaCfg cfg = tiny_attn_cfg();
  cfg.train.ensemble_size = 3;
  const auto r = fit_attn(st, cfg);
  ASSERT_TRUE(r.has_value());
  const LearnedModel &m = *r;
  ASSERT_EQ(m.nn.member_states.size(), 3U);

  const std::vector<f64> row = window_of(st, 0);
  const f64 ensemble = predict_nn(m, std::span<const f64>{row});

  f64 acc = 0.0;
  for (usize mi = 0; mi < m.nn.member_states.size(); ++mi) {
    LearnedModel one = m;
    one.nn.member_states = {m.nn.member_states[mi]};
    acc += predict_nn(one, std::span<const f64>{row});
  }
  const f64 mean = acc / static_cast<f64>(m.nn.member_states.size());
  EXPECT_NEAR(ensemble, mean, 1e-9) << "ensemble forward must be the ascending-member mean";
}

// =====================================================================
//  predict_blended dispatch: an Attn model routed through predict_blended equals
//  predict_nn (the no-default switch arm works). The augmented row IS the window.
// =====================================================================
TEST(LearnAttnAlpha, PredictBlended_DispatchesToPredictNn) {
  const SequenceTensor st = make_seq({10, 4, 3, 2, 2, /*planted=*/true, 31});
  const auto r = fit_attn(st, tiny_attn_cfg());
  ASSERT_TRUE(r.has_value());
  const std::vector<f64> row = window_of(st, 0);
  EXPECT_EQ(predict_blended(*r, std::span<const f64>{row}),
            predict_nn(*r, std::span<const f64>{row}))
      << "Attn predict_blended must route to predict_nn";
}

// =====================================================================
//  Errors (route through fit_seq_alpha's existing guard — a light check suffices).
// =====================================================================
TEST(LearnAttnAlpha, EmptyTensor_ReturnsInvalidArgument) {
  SequenceTensor st; // n_samples == 0
  st.lookback = 3;
  st.n_features = 2;
  st.y.assign(2, {});
  const auto r = fit_attn(st, tiny_attn_cfg());
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error().code(), atx::core::ErrorCode::InvalidArgument);
}

TEST(LearnAttnAlpha, ZeroFeatures_ReturnsInvalidArgument) {
  SequenceTensor st = make_seq({10, 4, 3, 2, 2, /*planted=*/true, 43});
  st.n_features = 0; // poison F
  const auto r = fit_attn(st, tiny_attn_cfg());
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error().code(), atx::core::ErrorCode::InvalidArgument);
}

TEST(LearnAttnAlpha, ZeroLookback_ReturnsInvalidArgument) {
  SequenceTensor st = make_seq({10, 4, 3, 2, 2, /*planted=*/true, 47});
  st.lookback = 0; // poison L
  const auto r = fit_attn(st, tiny_attn_cfg());
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error().code(), atx::core::ErrorCode::InvalidArgument);
}

}  // namespace atxtest_learn_attn_alpha_test
