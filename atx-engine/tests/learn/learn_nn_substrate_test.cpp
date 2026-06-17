// atx::engine::learn::nn — S5-2a substrate test (suite LearnNnSubstrate).
//
// This file is the LOAD-BEARING proof for the hand-written NN framework:
//
//   R3  finite-difference gradient check for EVERY layer (params AND dL/dx). The
//       analytic backward is the autodiff replacement; FD vs analytic is the only
//       thing that proves it correct. ε-central differences of the scalar
//       L = <forward(x), grad_out> = sum(forward(x) .* grad_out).
//   R1  determinism: two trainings with the same master_seed produce a
//       BYTE-IDENTICAL serialized ensemble; init draws are reproducible; the
//       minibatch shuffle is a pure function of (master_seed, member, epoch).
//   R7  linear-net -> ridge pin: Sequential{Linear(no bias)} + Identity trained
//       full-batch with SGD on an L2-penalised MSE objective converges to
//       ridge(X, y, lambda).beta. Plus a single-feature closed-form sanity case.
//
// Optimizer (Sgd/Adam) and loss (Mse/Huber/Ic) reference + FD-gradient checks
// round out the unit.

#include <cmath>   // std::fabs, std::sqrt
#include <cstring> // std::memcmp
#include <memory>
#include <span>
#include <vector>

#include <gtest/gtest.h>

#include "atx/core/hash.hpp"   // hash_bytes
#include "atx/core/random.hpp" // Xoshiro256pp
#include "atx/core/types.hpp"  // f64, u64, usize

#include "atx/core/linalg/linalg.hpp"     // MatX, VecX, as_matrix, as_vector
#include "atx/core/linalg/regression.hpp" // ridge

#include "atx/engine/learn/nn/layers.hpp"
#include "atx/engine/learn/nn/loss.hpp"
#include "atx/engine/learn/nn/module.hpp"
#include "atx/engine/learn/nn/optimizer.hpp"
#include "atx/engine/learn/nn/trainer.hpp"

namespace atxtest_learn_nn_substrate_test {

using atx::f64;
using atx::u64;
using atx::usize;
namespace nn = atx::engine::learn::nn;
namespace lin = atx::core::linalg;

// ---------------------------------------------------------------------------
//  FD machinery
// ---------------------------------------------------------------------------

// A fixed random matrix (B x C) from a seeded generator (column-major draw order).
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

// The scalar surrogate L = sum(forward(x) .* grad_out). Its gradient wrt every
// forward output element is exactly grad_out, so backward(grad_out) yields the
// true dL/dx and dL/dparam — the quantities FD must match.
[[nodiscard]] f64 surrogate_loss(nn::Module &m, const lin::MatX &x, const lin::MatX &gout) {
  const lin::MatX y = m.forward(x);
  return (y.array() * gout.array()).sum();
}

// Relative error with an absolute floor (so near-zero gradients don't blow up).
[[nodiscard]] f64 rel_err(f64 a, f64 b) {
  const f64 denom = std::fabs(a) + std::fabs(b) + 1e-8;
  return std::fabs(a - b) / denom;
}

constexpr f64 k_eps = 1e-6;     // FD perturbation
constexpr f64 k_grad_tol = 1e-5; // R3 relative-error budget

// Check analytic param-grads against central differences. `m` is bound to
// `params`/`grads` external buffers. The layer must be in eval mode (dropout off).
void check_param_grads(nn::Module &m, std::span<f64> params, std::span<f64> grads,
                       const lin::MatX &x, const lin::MatX &gout) {
  // Analytic: zero grads, one forward+backward accumulates dL/dparam.
  for (f64 &g : grads) {
    g = 0.0;
  }
  static_cast<void>(m.forward(x));
  static_cast<void>(m.backward(gout));
  const std::vector<f64> analytic(grads.begin(), grads.end());

  for (usize i = 0; i < params.size(); ++i) {
    const f64 orig = params[i];
    params[i] = orig + k_eps;
    const f64 lp = surrogate_loss(m, x, gout);
    params[i] = orig - k_eps;
    const f64 lm = surrogate_loss(m, x, gout);
    params[i] = orig;
    const f64 fd = (lp - lm) / (2.0 * k_eps);
    EXPECT_LT(rel_err(analytic[i], fd), k_grad_tol)
        << "param grad mismatch at " << i << " analytic=" << analytic[i] << " fd=" << fd;
  }
}

// Check analytic dL/dx (the backward return) against central differences.
void check_input_grads(nn::Module &m, const lin::MatX &x, const lin::MatX &gout) {
  static_cast<void>(m.forward(x));
  const lin::MatX dx = m.backward(gout);

  lin::MatX xp = x;
  for (Eigen::Index c = 0; c < x.cols(); ++c) {
    for (Eigen::Index r = 0; r < x.rows(); ++r) {
      const f64 orig = x(r, c);
      xp(r, c) = orig + k_eps;
      const f64 lp = surrogate_loss(m, xp, gout);
      xp(r, c) = orig - k_eps;
      const f64 lm = surrogate_loss(m, xp, gout);
      xp(r, c) = orig;
      const f64 fd = (lp - lm) / (2.0 * k_eps);
      EXPECT_LT(rel_err(dx(r, c), fd), k_grad_tol)
          << "input grad mismatch at (" << r << "," << c << ")";
    }
  }
}

// Bind a leaf to fresh buffers and seed-init its params with small normals.
struct BoundLeaf {
  std::vector<f64> params;
  std::vector<f64> grads;
};

[[nodiscard]] BoundLeaf bind_and_init(nn::Module &m, u64 seed) {
  BoundLeaf b;
  b.params.assign(m.param_count(), 0.0);
  b.grads.assign(m.param_count(), 0.0);
  m.bind_params(std::span<f64>(b.params), std::span<f64>(b.grads));
  atx::core::Xoshiro256pp rng{seed};
  for (f64 &p : b.params) {
    p = 0.5 * rng.normal(); // small but non-trivial
  }
  return b;
}

// ---------------------------------------------------------------------------
//  R3 — per-layer finite-difference gradient checks
// ---------------------------------------------------------------------------

TEST(LearnNnSubstrate, Linear_WithBias_GradCheck) {
  nn::Linear lin_layer{4, 3, /*bias=*/true};
  BoundLeaf b = bind_and_init(lin_layer, 11);
  const lin::MatX x = random_mat(5, 4, 101);
  const lin::MatX g = random_mat(5, 3, 202);
  check_param_grads(lin_layer, b.params, b.grads, x, g);
  check_input_grads(lin_layer, x, g);
}

TEST(LearnNnSubstrate, Linear_NoBias_GradCheck) {
  nn::Linear lin_layer{4, 3, /*bias=*/false};
  BoundLeaf b = bind_and_init(lin_layer, 12);
  EXPECT_EQ(lin_layer.param_count(), 12U); // 4*3, no bias
  const lin::MatX x = random_mat(6, 4, 103);
  const lin::MatX g = random_mat(6, 3, 204);
  check_param_grads(lin_layer, b.params, b.grads, x, g);
  check_input_grads(lin_layer, x, g);
}

TEST(LearnNnSubstrate, ReLU_GradCheck) {
  nn::ReLU relu;
  const lin::MatX x = random_mat(5, 4, 301); // normals straddle 0
  const lin::MatX g = random_mat(5, 4, 302);
  check_input_grads(relu, x, g);
}

TEST(LearnNnSubstrate, Tanh_GradCheck) {
  nn::Tanh tanh_layer;
  const lin::MatX x = random_mat(5, 4, 401);
  const lin::MatX g = random_mat(5, 4, 402);
  check_input_grads(tanh_layer, x, g);
}

TEST(LearnNnSubstrate, Identity_GradCheck) {
  nn::Identity id;
  const lin::MatX x = random_mat(4, 3, 501);
  const lin::MatX g = random_mat(4, 3, 502);
  check_input_grads(id, x, g);
}

TEST(LearnNnSubstrate, LayerNorm_GradCheck) {
  nn::LayerNorm ln{4};
  BoundLeaf b = bind_and_init(ln, 21);
  // gamma defaults from init are small; shift them near 1 so the affine is
  // representative (init wrote both gamma and beta as small normals).
  for (usize f = 0; f < 4; ++f) {
    b.params[f] += 1.0; // gamma_f ~ 1
  }
  const lin::MatX x = random_mat(6, 4, 601);
  const lin::MatX g = random_mat(6, 4, 602);
  check_param_grads(ln, b.params, b.grads, x, g);
  check_input_grads(ln, x, g);
}

TEST(LearnNnSubstrate, Dropout_OffPath_IsIdentity_GradCheck) {
  nn::Dropout drop{0.5, /*seed=*/7};
  drop.train(false); // dropout OFF — the path the grad check covers (R3)
  const lin::MatX x = random_mat(5, 4, 701);
  const lin::MatX g = random_mat(5, 4, 702);
  // Eval mode is identity: forward returns x, backward returns grad_out.
  const lin::MatX y = drop.forward(x);
  EXPECT_TRUE(y.isApprox(x));
  check_input_grads(drop, x, g);
}

TEST(LearnNnSubstrate, Sequential_Composed_GradCheck) {
  // Linear(4->5) -> Tanh -> Linear(5->2): a real network's flat param surface.
  auto seq = std::make_unique<nn::Sequential>();
  seq->add(std::make_unique<nn::Linear>(4, 5, true));
  seq->add(std::make_unique<nn::Tanh>());
  seq->add(std::make_unique<nn::Linear>(5, 2, true));
  seq->build();
  // Seed-init the flat buffer with small normals.
  atx::core::Xoshiro256pp rng{31};
  for (f64 &p : seq->params()) {
    p = 0.3 * rng.normal();
  }
  const lin::MatX x = random_mat(6, 4, 801);
  const lin::MatX g = random_mat(6, 2, 802);
  check_param_grads(*seq, seq->params(), seq->grads(), x, g);
  check_input_grads(*seq, x, g);
}

TEST(LearnNnSubstrate, Residual_GradCheck) {
  // Residual over Linear(4->4): y = W x + b + x. Shapes match (4 in, 4 out).
  auto res = std::make_unique<nn::Residual>(std::make_unique<nn::Linear>(4, 4, true));
  res->build();
  atx::core::Xoshiro256pp rng{41};
  for (f64 &p : res->params()) {
    p = 0.3 * rng.normal();
  }
  const lin::MatX x = random_mat(5, 4, 901);
  const lin::MatX g = random_mat(5, 4, 902);
  check_param_grads(*res, res->params(), res->grads(), x, g);
  check_input_grads(*res, x, g);
}

// ---------------------------------------------------------------------------
//  Loss reference + FD-gradient checks
// ---------------------------------------------------------------------------

// FD-check a loss's grad against its value.
void check_loss_grad(nn::Loss &loss, const lin::MatX &pred, const lin::MatX &target) {
  const lin::MatX g = loss.grad(pred, target);
  lin::MatX p = pred;
  for (Eigen::Index c = 0; c < pred.cols(); ++c) {
    for (Eigen::Index r = 0; r < pred.rows(); ++r) {
      const f64 orig = p(r, c);
      p(r, c) = orig + k_eps;
      const f64 lp = loss.value(p, target);
      p(r, c) = orig - k_eps;
      const f64 lm = loss.value(p, target);
      p(r, c) = orig;
      const f64 fd = (lp - lm) / (2.0 * k_eps);
      EXPECT_LT(rel_err(g(r, c), fd), 1e-5) << "loss grad mismatch at (" << r << "," << c << ")";
    }
  }
}

TEST(LearnNnSubstrate, MseLoss_ValueAndGrad) {
  const lin::MatX pred = random_mat(5, 2, 1001);
  const lin::MatX target = random_mat(5, 2, 1002);
  nn::MseLoss mse;
  // Reference value: mean squared error.
  f64 ref = 0.0;
  for (Eigen::Index c = 0; c < 2; ++c) {
    for (Eigen::Index r = 0; r < 5; ++r) {
      const f64 d = pred(r, c) - target(r, c);
      ref += d * d;
    }
  }
  ref /= 10.0;
  EXPECT_NEAR(mse.value(pred, target), ref, 1e-12);
  check_loss_grad(mse, pred, target);
}

TEST(LearnNnSubstrate, HuberLoss_ValueAndGrad) {
  const lin::MatX pred = random_mat(6, 1, 1101);
  const lin::MatX target = random_mat(6, 1, 1102);
  nn::HuberLoss huber{1.0};
  check_loss_grad(huber, pred, target);
  // A residual well inside delta behaves like 0.5 r² (quadratic regime).
  lin::MatX p2(1, 1);
  lin::MatX t2(1, 1);
  p2(0, 0) = 0.1;
  t2(0, 0) = 0.0;
  EXPECT_NEAR(huber.value(p2, t2), 0.5 * 0.1 * 0.1, 1e-12);
}

TEST(LearnNnSubstrate, IcLoss_ValueAndGrad) {
  const lin::MatX pred = random_mat(8, 1, 1201);
  const lin::MatX target = random_mat(8, 1, 1202);
  nn::IcLoss ic;
  check_loss_grad(ic, pred, target);
}

TEST(LearnNnSubstrate, IcLoss_PerfectCorrelation_IsZero) {
  // pred = 2*target + 1 is perfectly correlated => IC loss ~ 0.
  const lin::MatX target = random_mat(10, 1, 1301);
  const lin::MatX pred = (2.0 * target.array() + 1.0).matrix();
  nn::IcLoss ic;
  EXPECT_NEAR(ic.value(pred, target), 0.0, 1e-9);
}

TEST(LearnNnSubstrate, IcLoss_Degenerate_ReturnsOneAndZeroGrad) {
  // Constant prediction => zero variance => no signal: loss 1, grad 0.
  lin::MatX pred = lin::MatX::Constant(6, 1, 0.3);
  const lin::MatX target = random_mat(6, 1, 1401);
  nn::IcLoss ic;
  EXPECT_NEAR(ic.value(pred, target), 1.0, 1e-12);
  EXPECT_TRUE(ic.grad(pred, target).isZero());
}

// ---------------------------------------------------------------------------
//  Optimizer reference checks
// ---------------------------------------------------------------------------

TEST(LearnNnSubstrate, Sgd_PlainStep_Descends) {
  std::vector<f64> p{1.0, -2.0, 3.0};
  const std::vector<f64> g{0.5, 0.5, -1.0};
  nn::Sgd opt{0.1, 0.0};
  opt.step(std::span<f64>(p), std::span<const f64>(g));
  EXPECT_NEAR(p[0], 1.0 - 0.1 * 0.5, 1e-12);
  EXPECT_NEAR(p[1], -2.0 - 0.1 * 0.5, 1e-12);
  EXPECT_NEAR(p[2], 3.0 - 0.1 * -1.0, 1e-12);
}

TEST(LearnNnSubstrate, Sgd_Momentum_AccumulatesVelocity) {
  std::vector<f64> p{0.0};
  const std::vector<f64> g{1.0};
  nn::Sgd opt{0.1, 0.9};
  opt.step(std::span<f64>(p), std::span<const f64>(g)); // v=1, p=-0.1
  EXPECT_NEAR(p[0], -0.1, 1e-12);
  opt.step(std::span<f64>(p), std::span<const f64>(g)); // v=0.9*1+1=1.9, p=-0.1-0.19
  EXPECT_NEAR(p[0], -0.1 - 0.19, 1e-12);
}

TEST(LearnNnSubstrate, Adam_FirstStep_MatchesClosedForm) {
  std::vector<f64> p{0.0};
  const std::vector<f64> g{0.1};
  nn::Adam opt{0.01}; // b1=0.9, b2=0.999, eps=1e-8
  opt.step(std::span<f64>(p), std::span<const f64>(g));
  // t=1: m=0.1*(1-0.9)=0.01 -> mhat=0.01/(1-0.9)=0.1
  //      v=0.01*(1-0.999)=1e-5 -> vhat=1e-5/(1-0.999)=0.01
  //      step = 0.01 * 0.1 / (sqrt(0.01)+1e-8) ~= 0.01*0.1/0.1 = 0.01
  EXPECT_NEAR(p[0], -0.01, 1e-7);
}

TEST(LearnNnSubstrate, Adam_Reset_ClearsState) {
  std::vector<f64> p{0.0};
  const std::vector<f64> g{0.1};
  nn::Adam opt{0.01};
  opt.step(std::span<f64>(p), std::span<const f64>(g));
  const f64 after_first = p[0];
  opt.reset();
  p[0] = 0.0;
  opt.step(std::span<f64>(p), std::span<const f64>(g));
  EXPECT_NEAR(p[0], after_first, 1e-15); // reset => identical first step
}

// ---------------------------------------------------------------------------
//  Trainer + determinism helpers
// ---------------------------------------------------------------------------

// A linear-only factory: Sequential{ Linear(in->out, bias) } + Identity, params
// seeded from `seed` (small normals). Used by R7 + determinism tests.
[[nodiscard]] nn::ModelFactory linear_factory(usize in, usize out, bool bias) {
  return [in, out, bias](u64 seed) -> std::unique_ptr<nn::Module> {
    auto seq = std::make_unique<nn::Sequential>();
    seq->add(std::make_unique<nn::Linear>(in, out, bias));
    seq->add(std::make_unique<nn::Identity>());
    seq->build();
    atx::core::Xoshiro256pp rng{seed};
    for (f64 &p : seq->params()) {
      p = 0.01 * rng.normal(); // small init; the fit drives it
    }
    return seq;
  };
}

// ---------------------------------------------------------------------------
//  R1 — determinism (two builds byte-identical)
// ---------------------------------------------------------------------------

TEST(LearnNnSubstrate, R1_Init_SameSeed_SameParams) {
  auto f = linear_factory(3, 2, true);
  auto a = f(12345);
  auto b = f(12345);
  std::vector<f64> sa;
  std::vector<f64> sb;
  a->state_to(sa);
  b->state_to(sb);
  ASSERT_EQ(sa.size(), sb.size());
  EXPECT_EQ(std::memcmp(sa.data(), sb.data(), sa.size() * sizeof(f64)), 0);
}

TEST(LearnNnSubstrate, R1_Shuffle_PureFunctionOfSeed) {
  // The trainer's order is a pure fn of (master_seed, member, epoch). We prove
  // the underlying seed_for-driven shuffle is reproducible by running the whole
  // trainer twice and comparing the ensemble bytes (covers shuffle + init).
  const lin::MatX x = random_mat(20, 3, 1501);
  const lin::MatX y = random_mat(20, 1, 1502);
  auto f = linear_factory(3, 1, false);
  nn::TrainConfig cfg;
  cfg.epochs = 8;
  cfg.batch_size = 7; // non-divisor => exercises ragged last batch + shuffle
  cfg.ckpt_every = 2;
  cfg.ensemble_size = 2;
  cfg.master_seed = 777;

  nn::Sgd opt1{0.01, 0.0};
  nn::MseLoss loss1;
  const auto e1 = nn::train(f, opt1, loss1, x, y, x, y, cfg);
  nn::Sgd opt2{0.01, 0.0};
  nn::MseLoss loss2;
  const auto e2 = nn::train(f, opt2, loss2, x, y, x, y, cfg);
  ASSERT_TRUE(e1.has_value());
  ASSERT_TRUE(e2.has_value());
  ASSERT_EQ(e1->size(), e2->size());
  for (usize m = 0; m < e1->size(); ++m) {
    ASSERT_EQ((*e1)[m].size(), (*e2)[m].size());
    EXPECT_EQ(std::memcmp((*e1)[m].data(), (*e2)[m].data(), (*e1)[m].size() * sizeof(f64)), 0)
        << "ensemble member " << m << " not byte-identical across builds";
  }
}

TEST(LearnNnSubstrate, R1_TwoBuilds_ByteIdentical_Hash) {
  const lin::MatX x = random_mat(32, 4, 1601);
  const lin::MatX y = random_mat(32, 1, 1602);
  auto f = linear_factory(4, 1, true);
  nn::TrainConfig cfg;
  cfg.epochs = 20;
  cfg.batch_size = 8;
  cfg.ckpt_every = 5;
  cfg.ensemble_size = 3;
  cfg.master_seed = 2024;

  auto run = [&]() {
    nn::Adam opt{0.01};
    nn::MseLoss loss;
    auto e = nn::train(f, opt, loss, x, y, x, y, cfg);
    EXPECT_TRUE(e.has_value());
    std::vector<f64> flat;
    for (const auto &mem : *e) {
      flat.insert(flat.end(), mem.begin(), mem.end());
    }
    return atx::core::hash_bytes(flat.data(), flat.size() * sizeof(f64));
  };
  EXPECT_EQ(run(), run()); // identical digest across two full trainings
}

// ---------------------------------------------------------------------------
//  R7 — linear-net -> ridge pin
//
//  Objective used by the trained net (stated explicitly so the stationary point
//  matches ridge): with full-batch MSE L_data = (1/N)||y - Xb||² the gradient is
//  -(2/N) Xᵀ(y - Xb). Adding an L2 term (lambda/N)||b||² to the loss gradient
//  contributes (2*lambda/N) b. Setting the total gradient to zero gives
//  Xᵀ(y - Xb) = lambda b  <=>  (XᵀX + lambda I) b = Xᵀy — exactly ridge's normal
//  equations. So full-batch GD on MSE + this L2 converges to ridge(X,y,lambda).
//
//  We fold the 2*lambda/N L2 gradient in by hand each step (the substrate's Loss
//  is plain MSE; the penalty is a training-objective choice, per R7).
// ---------------------------------------------------------------------------

TEST(LearnNnSubstrate, R7_LinearNet_ConvergesToRidge) {
  const usize N = 40;
  const usize F = 3;
  const lin::MatX X = random_mat(static_cast<Eigen::Index>(N), static_cast<Eigen::Index>(F), 1701);
  // Ground-truth-ish target with noise so the ridge solution is non-trivial.
  const lin::VecX true_b = (lin::VecX(3) << 1.5, -0.7, 0.4).finished();
  const lin::MatX noise = random_mat(static_cast<Eigen::Index>(N), 1, 1702);
  lin::MatX y(static_cast<Eigen::Index>(N), 1);
  y.col(0) = X * true_b + 0.1 * noise.col(0);

  const f64 lambda = 0.5;
  const auto rr = atx::core::linalg::ridge(X, y.col(0), lambda);
  ASSERT_TRUE(rr.has_value());

  // Train a no-bias Linear by full-batch gradient descent on MSE + L2.
  auto seq = std::make_unique<nn::Sequential>();
  seq->add(std::make_unique<nn::Linear>(F, 1, false));
  seq->add(std::make_unique<nn::Identity>());
  seq->build();
  for (f64 &p : seq->params()) {
    p = 0.0; // start at origin
  }
  nn::MseLoss mse;
  const f64 lr = 0.05;
  const f64 nf = static_cast<f64>(N);
  for (usize it = 0; it < 20000; ++it) {
    std::span<f64> g = seq->grads();
    for (f64 &gi : g) {
      gi = 0.0;
    }
    const lin::MatX pred = seq->forward(X);
    const lin::MatX dpred = mse.grad(pred, y); // (2/N)(pred - y)
    static_cast<void>(seq->backward(dpred));
    // Hand-fold the L2 gradient (2*lambda/N)*b into the param grads.
    std::span<f64> params = seq->params();
    for (usize i = 0; i < params.size(); ++i) {
      g[i] += (2.0 * lambda / nf) * params[i];
      params[i] -= lr * g[i];
    }
  }
  std::span<f64> fit = seq->params();
  for (usize i = 0; i < F; ++i) {
    EXPECT_LT(rel_err(fit[i], rr->beta[static_cast<Eigen::Index>(i)]), 1e-6)
        << "ridge pin mismatch at coef " << i;
  }
}

TEST(LearnNnSubstrate, R7_SingleFeature_ClosedForm) {
  // One feature, no bias: ridge beta = (Σ x y) / (Σ x² + lambda). Closed form.
  const usize N = 16;
  const lin::MatX X = random_mat(static_cast<Eigen::Index>(N), 1, 1801);
  const lin::MatX y = random_mat(static_cast<Eigen::Index>(N), 1, 1802);
  const f64 lambda = 0.3;
  f64 sxy = 0.0;
  f64 sxx = 0.0;
  for (usize i = 0; i < N; ++i) {
    sxy += X(static_cast<Eigen::Index>(i), 0) * y(static_cast<Eigen::Index>(i), 0);
    sxx += X(static_cast<Eigen::Index>(i), 0) * X(static_cast<Eigen::Index>(i), 0);
  }
  const f64 closed = sxy / (sxx + lambda);

  auto seq = std::make_unique<nn::Sequential>();
  seq->add(std::make_unique<nn::Linear>(1, 1, false));
  seq->add(std::make_unique<nn::Identity>());
  seq->build();
  seq->params()[0] = 0.0;
  nn::MseLoss mse;
  const f64 lr = 0.05;
  const f64 nf = static_cast<f64>(N);
  for (usize it = 0; it < 20000; ++it) {
    seq->grads()[0] = 0.0;
    const lin::MatX pred = seq->forward(X);
    const lin::MatX dpred = mse.grad(pred, y);
    static_cast<void>(seq->backward(dpred));
    f64 &g0 = seq->grads()[0];
    f64 &p0 = seq->params()[0];
    g0 += (2.0 * lambda / nf) * p0;
    p0 -= lr * g0;
  }
  EXPECT_LT(rel_err(seq->params()[0], closed), 1e-6);
}

// ---------------------------------------------------------------------------
//  Trainer end-to-end smoke: loss decreases, ensemble mean predicts.
// ---------------------------------------------------------------------------

TEST(LearnNnSubstrate, Trainer_ReducesLoss_AndEnsemblePredicts) {
  const lin::MatX X = random_mat(60, 3, 1901);
  const lin::VecX tb = (lin::VecX(3) << 0.8, -0.5, 0.3).finished();
  lin::MatX y(60, 1);
  y.col(0) = X * tb;

  auto f = linear_factory(3, 1, true);
  nn::TrainConfig cfg;
  cfg.epochs = 100;
  cfg.batch_size = 20;
  cfg.ckpt_every = 10;
  cfg.ensemble_size = 3;
  cfg.master_seed = 5;

  nn::Adam opt{0.05};
  nn::MseLoss loss;
  const auto e = nn::train(f, opt, loss, X, y, X, y, cfg);
  ASSERT_TRUE(e.has_value());
  ASSERT_EQ(e->size(), 3U);

  const auto pred = nn::ensemble_mean_predict(f, *e, X);
  ASSERT_TRUE(pred.has_value());
  // The ensemble should fit a clean linear target well.
  const f64 mse = (pred->array() - y.array()).square().mean();
  EXPECT_LT(mse, 1e-2) << "ensemble failed to fit a clean linear target";
}

TEST(LearnNnSubstrate, Trainer_RejectsBadConfig) {
  const lin::MatX x = random_mat(4, 2, 2001);
  const lin::MatX y = random_mat(4, 1, 2002);
  auto f = linear_factory(2, 1, false);
  nn::MseLoss loss;
  {
    nn::Sgd opt{0.01};
    nn::TrainConfig cfg;
    cfg.batch_size = 0; // invalid
    EXPECT_FALSE(nn::train(f, opt, loss, x, y, x, y, cfg).has_value());
  }
  {
    nn::Sgd opt{0.01};
    nn::TrainConfig cfg;
    cfg.ensemble_size = 0; // invalid
    EXPECT_FALSE(nn::train(f, opt, loss, x, y, x, y, cfg).has_value());
  }
  {
    nn::Sgd opt{0.01};
    nn::TrainConfig cfg;
    const lin::MatX empty;
    EXPECT_FALSE(nn::train(f, opt, loss, empty, empty, x, y, cfg).has_value());
  }
}

}  // namespace atxtest_learn_nn_substrate_test
