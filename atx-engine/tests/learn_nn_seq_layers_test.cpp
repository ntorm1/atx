// atx::engine::learn::nn — S5-2b-i sequence-layer test (suite LearnNnSeqLayers).
//
// LOAD-BEARING proofs for the hand-written sequence layers:
//
//   R3  finite-difference gradient check on EVERY new layer, for BOTH params AND
//       dL/dx (rel-err <= ~1e-5, f64, dropout OFF). The analytic backward is the
//       autodiff replacement; FD vs analytic is the only correctness proof. The
//       GRU/MGU checks exercise the BPTT adjoints over the whole window.
//   R2  causal structural test: perturbing a Conv1dCausal input at times > t
//       leaves output[t] BIT-IDENTICAL (no look-ahead). Same end-to-end through a
//       TcnResidualBlock (a stack of causal convs is causal).
//   Forward reference checks: Conv1dCausal vs a hand-rolled naive causal conv
//       (dilation 1 and 2); GruCell/MguCell vs a scalar recurrence reference on a
//       2-step window; SeqLastStep returns exactly the t=T-1 step.
//   R1  determinism: same seed -> byte-identical params (the existing init path);
//       no simd::*.
//
// The FD machinery mirrors learn_nn_substrate_test.cpp (the substrate's harness),
// adapted for the time-major sequence encoding row = [t*C + c].

#include <cmath>   // std::fabs
#include <cstring> // std::memcmp
#include <span>
#include <vector>

#include <gtest/gtest.h>

#include "atx/core/random.hpp" // Xoshiro256pp
#include "atx/core/types.hpp"  // f64, u64, usize

#include "atx/core/linalg/linalg.hpp" // MatX

#include "atx/engine/learn/nn/module.hpp"
#include "atx/engine/learn/nn/seq_layers.hpp"

namespace {

using atx::f64;
using atx::u64;
using atx::usize;
namespace nn = atx::engine::learn::nn;
namespace lin = atx::core::linalg;

// ---------------------------------------------------------------------------
//  FD machinery (lifted from learn_nn_substrate_test.cpp)
// ---------------------------------------------------------------------------

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

// L = sum(forward(x) .* gout): its gradient wrt every output element is gout, so
// backward(gout) yields the true dL/dx and dL/dparam that FD must match.
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

// Most checks use the tight default (eps=1e-6). The deep TcnResidualBlock has a
// dominant identity skip, so some early-conv weights carry a near-zero TRUE
// gradient (O(1e-6..1e-7)); there central differences are rounding-floor limited
// (the surrogate loss is O(1), so lp-lm is O(2*eps*g) ~ 1e-13, near f64 epsilon).
// A slightly larger eps lifts that rounding floor without changing the analytic
// gradient under test, so those tests pass eps=1e-4 explicitly. (The analytic
// backward is identical to the channel-mismatch variant, which passes at 1e-6.)
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

// Bind a leaf/block to fresh buffers and seed-init params with small normals.
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
//  Conv1dCausal — gradient checks (R3)
// ===========================================================================

TEST(LearnNnSeqLayers, Conv1d_Dilation1_GradCheck) {
  // T=5, C_in=2, C_out=3, K=3, dil=1, bias.
  nn::Conv1dCausal conv{5, 2, 3, 3, 1, /*bias=*/true};
  BoundLeaf b = bind_and_init(conv, 11);
  EXPECT_EQ(conv.param_count(), 3U * 2U * 3U + 3U); // K*Cin*Cout + Cout
  const lin::MatX x = random_mat(4, 5 * 2, 101);    // B=4, T*C_in
  const lin::MatX g = random_mat(4, 5 * 3, 202);    // B=4, T*C_out
  check_param_grads(conv, b.params, b.grads, x, g);
  check_input_grads(conv, x, g);
}

TEST(LearnNnSeqLayers, Conv1d_Dilation2_GradCheck) {
  nn::Conv1dCausal conv{6, 3, 2, 2, 2, /*bias=*/true};
  BoundLeaf b = bind_and_init(conv, 12);
  const lin::MatX x = random_mat(3, 6 * 3, 103);
  const lin::MatX g = random_mat(3, 6 * 2, 204);
  check_param_grads(conv, b.params, b.grads, x, g);
  check_input_grads(conv, x, g);
}

TEST(LearnNnSeqLayers, Conv1d_NoBias_GradCheck) {
  nn::Conv1dCausal conv{4, 2, 2, 2, 1, /*bias=*/false};
  BoundLeaf b = bind_and_init(conv, 13);
  EXPECT_EQ(conv.param_count(), 2U * 2U * 2U); // no bias
  const lin::MatX x = random_mat(3, 4 * 2, 105);
  const lin::MatX g = random_mat(3, 4 * 2, 206);
  check_param_grads(conv, b.params, b.grads, x, g);
  check_input_grads(conv, x, g);
}

// ===========================================================================
//  Conv1dCausal — forward reference (hand-computed naive causal conv)
// ===========================================================================

// Naive reference: y[t,co] = b[co] + Σ_k Σ_ci W[k,ci,co]*x[t-k*dil, ci], x[neg]=0.
// Weight flat index ((k*Cin + ci)*Cout + co), matching the layer's convention.
[[nodiscard]] lin::MatX naive_causal_conv(const lin::MatX &x, std::span<const f64> w,
                                          std::span<const f64> bias, usize T, usize cin,
                                          usize cout, usize K, usize dil) {
  const Eigen::Index B = x.rows();
  lin::MatX y(B, static_cast<Eigen::Index>(T * cout));
  for (Eigen::Index r = 0; r < B; ++r) {
    for (usize t = 0; t < T; ++t) {
      for (usize co = 0; co < cout; ++co) {
        f64 acc = bias.empty() ? 0.0 : bias[co];
        for (usize k = 0; k < K; ++k) {
          const usize back = k * dil;
          if (back > t) {
            continue; // left zero-pad
          }
          const usize ts = t - back;
          for (usize ci = 0; ci < cin; ++ci) {
            const f64 wv = w[(k * cin + ci) * cout + co];
            acc += wv * x(r, static_cast<Eigen::Index>(ts * cin + ci));
          }
        }
        y(r, static_cast<Eigen::Index>(t * cout + co)) = acc;
      }
    }
  }
  return y;
}

TEST(LearnNnSeqLayers, Conv1d_Forward_RefDilation1) {
  const usize T = 4;
  const usize cin = 2;
  const usize cout = 2;
  const usize K = 2;
  nn::Conv1dCausal conv{T, cin, cout, K, 1, true};
  BoundLeaf b = bind_and_init(conv, 21);
  const std::span<const f64> w{b.params.data(), K * cin * cout};
  const std::span<const f64> bias{b.params.data() + K * cin * cout, cout};
  const lin::MatX x = random_mat(3, static_cast<Eigen::Index>(T * cin), 301);
  const lin::MatX y = conv.forward(x);
  const lin::MatX ref = naive_causal_conv(x, w, bias, T, cin, cout, K, 1);
  EXPECT_TRUE(y.isApprox(ref, 1e-12)) << "conv forward != naive reference (dil=1)";
}

TEST(LearnNnSeqLayers, Conv1d_Forward_RefDilation2) {
  const usize T = 6;
  const usize cin = 2;
  const usize cout = 3;
  const usize K = 3;
  nn::Conv1dCausal conv{T, cin, cout, K, 2, true};
  BoundLeaf b = bind_and_init(conv, 22);
  const std::span<const f64> w{b.params.data(), K * cin * cout};
  const std::span<const f64> bias{b.params.data() + K * cin * cout, cout};
  const lin::MatX x = random_mat(2, static_cast<Eigen::Index>(T * cin), 302);
  const lin::MatX y = conv.forward(x);
  const lin::MatX ref = naive_causal_conv(x, w, bias, T, cin, cout, K, 2);
  EXPECT_TRUE(y.isApprox(ref, 1e-12)) << "conv forward != naive reference (dil=2)";
}

// Tiny fully hand-computed window (no reference helper): single sample, Cin=Cout=1,
// K=2, dil=1, no bias, W=[w0 (k=0), w1 (k=1)]. y[t] = w0*x[t] + w1*x[t-1].
TEST(LearnNnSeqLayers, Conv1d_Forward_HandComputed) {
  nn::Conv1dCausal conv{3, 1, 1, 2, 1, /*bias=*/false};
  std::vector<f64> p{2.0, -1.0}; // w0=2 (current), w1=-1 (one step back)
  std::vector<f64> g(2, 0.0);
  conv.bind_params(std::span<f64>(p), std::span<f64>(g));
  lin::MatX x(1, 3);
  x(0, 0) = 1.0;
  x(0, 1) = 3.0;
  x(0, 2) = 5.0;
  const lin::MatX y = conv.forward(x);
  // y[0]=2*1 - 1*0 = 2 ; y[1]=2*3 - 1*1 = 5 ; y[2]=2*5 - 1*3 = 7.
  EXPECT_NEAR(y(0, 0), 2.0, 1e-12);
  EXPECT_NEAR(y(0, 1), 5.0, 1e-12);
  EXPECT_NEAR(y(0, 2), 7.0, 1e-12);
}

// ===========================================================================
//  R2 — causal structural test (no look-ahead)
// ===========================================================================

TEST(LearnNnSeqLayers, Conv1d_Causal_NoLookAhead) {
  const usize T = 6;
  const usize cin = 2;
  const usize cout = 2;
  const usize K = 3;
  const usize dil = 1;
  nn::Conv1dCausal conv{T, cin, cout, K, dil, true};
  BoundLeaf b = bind_and_init(conv, 31);
  const lin::MatX x = random_mat(2, static_cast<Eigen::Index>(T * cin), 401);
  const lin::MatX y0 = conv.forward(x);

  // Perturb EVERY input at time steps > t_probe; assert output at all t <= t_probe
  // is bit-identical (independent of future inputs).
  const usize t_probe = 2;
  lin::MatX xp = x;
  for (usize t = t_probe + 1; t < T; ++t) {
    for (usize ci = 0; ci < cin; ++ci) {
      for (Eigen::Index r = 0; r < xp.rows(); ++r) {
        xp(r, static_cast<Eigen::Index>(t * cin + ci)) += 7.5; // large perturbation
      }
    }
  }
  const lin::MatX y1 = conv.forward(xp);
  for (usize t = 0; t <= t_probe; ++t) {
    for (usize co = 0; co < cout; ++co) {
      for (Eigen::Index r = 0; r < y0.rows(); ++r) {
        const Eigen::Index col = static_cast<Eigen::Index>(t * cout + co);
        EXPECT_EQ(y0(r, col), y1(r, col)) // BIT-identical
            << "future input leaked into output at t=" << t;
      }
    }
  }
}

TEST(LearnNnSeqLayers, Tcn_Causal_NoLookAhead) {
  // A stack of causal convs (the TCN block) must also be causal end-to-end.
  const usize T = 6;
  const usize C = 2;
  nn::TcnResidualBlock blk{T, C, C, 3, 1, /*dropout=*/0.0, /*seed=*/5};
  BoundLeaf b = bind_and_init(blk, 32, 0.3);
  blk.train(false); // eval (no dropout) so forward is a pure function
  const lin::MatX x = random_mat(2, static_cast<Eigen::Index>(T * C), 402);
  const lin::MatX y0 = blk.forward(x);

  const usize t_probe = 3;
  lin::MatX xp = x;
  for (usize t = t_probe + 1; t < T; ++t) {
    for (usize c = 0; c < C; ++c) {
      for (Eigen::Index r = 0; r < xp.rows(); ++r) {
        xp(r, static_cast<Eigen::Index>(t * C + c)) += 4.0;
      }
    }
  }
  const lin::MatX y1 = blk.forward(xp);
  for (usize t = 0; t <= t_probe; ++t) {
    for (usize c = 0; c < C; ++c) {
      for (Eigen::Index r = 0; r < y0.rows(); ++r) {
        const Eigen::Index col = static_cast<Eigen::Index>(t * C + c);
        EXPECT_EQ(y0(r, col), y1(r, col)) << "future leaked through TCN at t=" << t;
      }
    }
  }
}

// ===========================================================================
//  TcnResidualBlock — gradient checks (R3), channel-match AND mismatch skip
// ===========================================================================

// Nudge the two LayerNorm gammas in a TCN flat param buffer toward 1.0 so the
// affine is representative (init writes small normals; small gammas push the
// output near-degenerate, where the early-conv gradients are ~1e-6 and central
// differences can't resolve them to a 1e-5 relative budget). Mirrors the
// substrate LayerNorm_GradCheck practice. Layout: conv1(W+b), ln1(gamma,beta),
// conv2(W+b), ln2(gamma,beta), [proj].
void nudge_tcn_gammas(std::span<f64> params, usize K, usize cin, usize cout) {
  const usize conv1 = K * cin * cout + cout;  // W + bias
  const usize ln_block = 2 * cout;            // gamma + beta
  const usize conv2 = K * cout * cout + cout; // stage-2 conv (cout->cout)
  const usize ln1_gamma = conv1;
  const usize ln2_gamma = conv1 + ln_block + conv2;
  for (usize c = 0; c < cout; ++c) {
    params[ln1_gamma + c] += 1.0;
    params[ln2_gamma + c] += 1.0;
  }
}

TEST(LearnNnSeqLayers, Tcn_ChannelMatch_GradCheck) {
  const usize T = 5;
  const usize C = 2;
  const usize K = 2;
  nn::TcnResidualBlock blk{T, C, C, K, 1, /*dropout=*/0.0, /*seed=*/7};
  BoundLeaf b = bind_and_init(blk, 41, 0.3);
  nudge_tcn_gammas(b.params, K, C, C);
  blk.train(false);
  const lin::MatX x = random_mat(3, static_cast<Eigen::Index>(T * C), 501);
  const lin::MatX g = random_mat(3, static_cast<Eigen::Index>(T * C), 502);
  // eps=1e-4: see check_param_grads note (dominant identity skip => near-zero
  // early-conv gradients are rounding-floor limited at eps=1e-6).
  check_param_grads(blk, b.params, b.grads, x, g, /*eps=*/1e-4);
  check_input_grads(blk, x, g, /*eps=*/1e-4);
}

TEST(LearnNnSeqLayers, Tcn_ChannelMismatch_GradCheck) {
  // in=2, out=3 => 1x1 causal conv on the skip path.
  const usize T = 5;
  const usize cin = 2;
  const usize cout = 3;
  const usize K = 2;
  nn::TcnResidualBlock blk{T, cin, cout, K, 1, /*dropout=*/0.0, /*seed=*/9};
  EXPECT_TRUE(blk.projects_skip());
  BoundLeaf b = bind_and_init(blk, 42, 0.3);
  nudge_tcn_gammas(b.params, K, cin, cout);
  blk.train(false);
  const lin::MatX x = random_mat(3, static_cast<Eigen::Index>(T * cin), 503);
  const lin::MatX g = random_mat(3, static_cast<Eigen::Index>(T * cout), 504);
  check_param_grads(blk, b.params, b.grads, x, g);
  check_input_grads(blk, x, g);
}

// ===========================================================================
//  GruCell / MguCell — BPTT gradient checks (R3) and forward references
// ===========================================================================

TEST(LearnNnSeqLayers, Gru_GradCheck_BPTT) {
  const usize T = 4;
  const usize C = 2;
  const usize H = 3;
  nn::GruCell gru{T, C, H};
  EXPECT_EQ(gru.param_count(), 3U * (C * H + H * H + H));
  BoundLeaf b = bind_and_init(gru, 51, 0.3);
  const lin::MatX x = random_mat(2, static_cast<Eigen::Index>(T * C), 601);
  const lin::MatX g = random_mat(2, static_cast<Eigen::Index>(H), 602);
  check_param_grads(gru, b.params, b.grads, x, g); // exercises gate BPTT adjoints
  check_input_grads(gru, x, g);
}

TEST(LearnNnSeqLayers, Mgu_GradCheck_BPTT) {
  const usize T = 5;
  const usize C = 2;
  const usize H = 3;
  nn::MguCell mgu{T, C, H};
  EXPECT_EQ(mgu.param_count(), 2U * (C * H + H * H + H));
  BoundLeaf b = bind_and_init(mgu, 52, 0.3);
  const lin::MatX x = random_mat(2, static_cast<Eigen::Index>(T * C), 603);
  const lin::MatX g = random_mat(2, static_cast<Eigen::Index>(H), 604);
  check_param_grads(mgu, b.params, b.grads, x, g);
  check_input_grads(mgu, x, g);
}

// Scalar-recurrence reference for a single sample over a 2-step window.
// Hidden size 1, input channels 1, to keep the closed form readable.
TEST(LearnNnSeqLayers, Gru_Forward_ScalarRecurrenceRef) {
  const usize T = 2;
  const usize C = 1;
  const usize H = 1;
  nn::GruCell gru{T, C, H};
  // Param order: W_xz,W_hz,b_z, W_xr,W_hr,b_r, W_xn,W_hn,b_n (each 1 scalar here).
  std::vector<f64> p{0.5, 0.2, 0.1,   // z gate
                     -0.3, 0.4, 0.0,  // r gate
                     0.6, -0.2, 0.2}; // candidate
  std::vector<f64> grads(p.size(), 0.0);
  gru.bind_params(std::span<f64>(p), std::span<f64>(grads));
  lin::MatX x(1, 2);
  x(0, 0) = 0.7; // x_0
  x(0, 1) = -0.4; // x_1
  const lin::MatX y = gru.forward(x);

  auto sigm = [](f64 v) { return 1.0 / (1.0 + std::exp(-v)); };
  f64 h = 0.0;
  for (usize t = 0; t < T; ++t) {
    const f64 xt = x(0, static_cast<Eigen::Index>(t));
    const f64 z = sigm(0.5 * xt + 0.2 * h + 0.1);
    const f64 r = sigm(-0.3 * xt + 0.4 * h + 0.0);
    const f64 n = std::tanh(0.6 * xt + (-0.2) * (r * h) + 0.2);
    h = (1.0 - z) * n + z * h;
  }
  EXPECT_NEAR(y(0, 0), h, 1e-12);
}

TEST(LearnNnSeqLayers, Mgu_Forward_ScalarRecurrenceRef) {
  const usize T = 2;
  const usize C = 1;
  const usize H = 1;
  nn::MguCell mgu{T, C, H};
  // Param order: W_xf,W_hf,b_f, W_xn,W_hn,b_n.
  std::vector<f64> p{0.4, 0.3, -0.1, 0.7, 0.2, 0.05};
  std::vector<f64> grads(p.size(), 0.0);
  mgu.bind_params(std::span<f64>(p), std::span<f64>(grads));
  lin::MatX x(1, 2);
  x(0, 0) = 0.6;
  x(0, 1) = -0.5;
  const lin::MatX y = mgu.forward(x);

  auto sigm = [](f64 v) { return 1.0 / (1.0 + std::exp(-v)); };
  f64 h = 0.0;
  for (usize t = 0; t < T; ++t) {
    const f64 xt = x(0, static_cast<Eigen::Index>(t));
    const f64 f = sigm(0.4 * xt + 0.3 * h - 0.1);
    const f64 n = std::tanh(0.7 * xt + 0.2 * (f * h) + 0.05);
    h = (1.0 - f) * h + f * n;
  }
  EXPECT_NEAR(y(0, 0), h, 1e-12);
}

// ===========================================================================
//  SeqLastStep — forward exactness + input grad (R3)
// ===========================================================================

TEST(LearnNnSeqLayers, SeqLastStep_ReturnsLastStep) {
  const usize T = 4;
  const usize C = 3;
  nn::SeqLastStep pool{T, C};
  const lin::MatX x = random_mat(3, static_cast<Eigen::Index>(T * C), 701);
  const lin::MatX y = pool.forward(x);
  ASSERT_EQ(y.cols(), static_cast<Eigen::Index>(C));
  for (Eigen::Index r = 0; r < x.rows(); ++r) {
    for (usize c = 0; c < C; ++c) {
      const Eigen::Index src = static_cast<Eigen::Index>((T - 1) * C + c);
      EXPECT_EQ(y(r, static_cast<Eigen::Index>(c)), x(r, src));
    }
  }
}

TEST(LearnNnSeqLayers, SeqLastStep_GradCheck) {
  nn::SeqLastStep pool{4, 3};
  const lin::MatX x = random_mat(3, 4 * 3, 702);
  const lin::MatX g = random_mat(3, 3, 703);
  check_input_grads(pool, x, g);
}

TEST(LearnNnSeqLayers, SeqLastStep_BackwardScattersToLastStep) {
  const usize T = 4;
  const usize C = 2;
  nn::SeqLastStep pool{T, C};
  const lin::MatX x = random_mat(2, static_cast<Eigen::Index>(T * C), 704);
  static_cast<void>(pool.forward(x));
  const lin::MatX g = random_mat(2, static_cast<Eigen::Index>(C), 705);
  const lin::MatX dx = pool.backward(g);
  ASSERT_EQ(dx.cols(), static_cast<Eigen::Index>(T * C));
  for (Eigen::Index r = 0; r < dx.rows(); ++r) {
    for (usize t = 0; t < T; ++t) {
      for (usize c = 0; c < C; ++c) {
        const Eigen::Index col = static_cast<Eigen::Index>(t * C + c);
        if (t == T - 1) {
          EXPECT_EQ(dx(r, col), g(r, static_cast<Eigen::Index>(c)));
        } else {
          EXPECT_EQ(dx(r, col), 0.0); // earlier steps get zero gradient
        }
      }
    }
  }
}

// ===========================================================================
//  R1 — determinism: same seed -> byte-identical params
// ===========================================================================

TEST(LearnNnSeqLayers, R1_Init_SameSeed_SameParams) {
  // Two independent inits of the same layer with the same seed match byte-for-byte
  // (the existing seeded-init path; no simd::*).
  nn::Conv1dCausal a{5, 2, 3, 3, 1, true};
  nn::Conv1dCausal b{5, 2, 3, 3, 1, true};
  BoundLeaf ba = bind_and_init(a, 123456);
  BoundLeaf bb = bind_and_init(b, 123456);
  std::vector<f64> sa;
  std::vector<f64> sb;
  a.state_to(sa);
  b.state_to(sb);
  ASSERT_EQ(sa.size(), sb.size());
  EXPECT_EQ(std::memcmp(sa.data(), sb.data(), sa.size() * sizeof(f64)), 0);
}

} // namespace
