#pragma once

// atx::engine::learn::nn — generic layers (S5-2a): Linear, activations, Dropout,
// LayerNorm. Each is a Module with a forward and a HAND-WRITTEN backward; the
// finite-difference gradient check in the test is the correctness proof (R3).
//
// =====================================================================
//  Conventions shared by every layer here
// =====================================================================
//  - Designs are samples x features (rows = batch B, cols = in/out features).
//  - Leaves DO NOT own their parameter bytes: param_count() declares the scalar
//    budget; bind_params(p, g) aliases caller-owned contiguous storage; the
//    weight/grad Eigen::Maps view that storage. The flattening convention is
//    fixed and COLUMN-MAJOR (Eigen's native Map order) so serialize == bind order
//    (R1). Linear's flat layout is: [W (in*out, col-major), b (out)] when biased.
//  - forward caches exactly what backward needs and nothing the steady state
//    must reallocate (R6). backward ACCUMULATES into the grad slice (the Trainer
//    zeroes grads each minibatch).
//  - Every explicit reduction (norm mean/var, grad sums) is an ascending scalar
//    fold, never simd::* (R1). Matmuls use Eigen `*` (deterministic, single
//    thread in the digest path).

#include <span>
#include <vector>

#include "atx/core/types.hpp" // f64, u64, usize

#include "atx/core/linalg/linalg.hpp" // MatX, VecX

#include "atx/engine/learn/nn/module.hpp" // Module, lin

namespace atx::engine::learn::nn {

// ===========================================================================
//  Linear — y = x · W (+ b). W is in_features x out_features.
//
//  Flat param layout (ascending, column-major): W column 0, W column 1, …, then
//  bias (if present). Initialisation is the caller's job (the Trainer seeds it);
//  bind_params only wires storage. grads accumulate dL/dW and dL/db.
// ===========================================================================
class Linear final : public Module {
public:
  Linear(atx::usize in_features, atx::usize out_features, bool bias);

  [[nodiscard]] lin::MatX forward(const lin::MatX &x) override;
  [[nodiscard]] lin::MatX backward(const lin::MatX &grad_out) override;

  [[nodiscard]] atx::usize param_count() const noexcept override { return n_params_; }
  void bind_params(std::span<atx::f64> params, std::span<atx::f64> grads) override;
  [[nodiscard]] std::span<atx::f64> params() noexcept override { return params_; }
  [[nodiscard]] std::span<atx::f64> grads() noexcept override { return grads_; }

  void state_to(std::vector<atx::f64> &out) const override;
  void state_from(std::span<const atx::f64> in) override;

  [[nodiscard]] atx::usize in_features() const noexcept { return in_; }
  [[nodiscard]] atx::usize out_features() const noexcept { return out_; }
  [[nodiscard]] bool has_bias() const noexcept { return bias_; }

private:
  // Weight / grad Eigen views over the bound slice (column-major, in x out).
  [[nodiscard]] lin::MatMap weight() noexcept;
  [[nodiscard]] lin::MatMapConst weight() const noexcept;
  [[nodiscard]] lin::MatMap weight_grad() noexcept;

  atx::usize in_;
  atx::usize out_;
  bool bias_;
  atx::usize n_params_;
  std::span<atx::f64> params_;
  std::span<atx::f64> grads_;
  lin::MatX x_cache_; // last forward input (B x in), for backward
};

// ===========================================================================
//  Activations — elementwise, parameter-free. forward caches what backward
//  needs (the input for ReLU/Tanh slope, the output for Tanh's 1 - y²).
// ===========================================================================

// Identity: y = x. The R7 linear-net pin uses this as the output activation.
class Identity final : public Module {
public:
  [[nodiscard]] lin::MatX forward(const lin::MatX &x) override;
  [[nodiscard]] lin::MatX backward(const lin::MatX &grad_out) override;
  [[nodiscard]] atx::usize param_count() const noexcept override { return 0; }
  void bind_params(std::span<atx::f64>, std::span<atx::f64>) override {}
  [[nodiscard]] std::span<atx::f64> params() noexcept override { return {}; }
  [[nodiscard]] std::span<atx::f64> grads() noexcept override { return {}; }
  void state_to(std::vector<atx::f64> &) const override {}
  void state_from(std::span<const atx::f64>) override {}
};

// ReLU: y = max(x, 0). backward gates dL/dy by the forward sign mask.
class ReLU final : public Module {
public:
  [[nodiscard]] lin::MatX forward(const lin::MatX &x) override;
  [[nodiscard]] lin::MatX backward(const lin::MatX &grad_out) override;
  [[nodiscard]] atx::usize param_count() const noexcept override { return 0; }
  void bind_params(std::span<atx::f64>, std::span<atx::f64>) override {}
  [[nodiscard]] std::span<atx::f64> params() noexcept override { return {}; }
  [[nodiscard]] std::span<atx::f64> grads() noexcept override { return {}; }
  void state_to(std::vector<atx::f64> &) const override {}
  void state_from(std::span<const atx::f64>) override {}

private:
  lin::MatX mask_; // 1 where x > 0 else 0 (cached from forward)
};

// Tanh: y = tanh(x). backward multiplies dL/dy by (1 - y²).
class Tanh final : public Module {
public:
  [[nodiscard]] lin::MatX forward(const lin::MatX &x) override;
  [[nodiscard]] lin::MatX backward(const lin::MatX &grad_out) override;
  [[nodiscard]] atx::usize param_count() const noexcept override { return 0; }
  void bind_params(std::span<atx::f64>, std::span<atx::f64>) override {}
  [[nodiscard]] std::span<atx::f64> params() noexcept override { return {}; }
  [[nodiscard]] std::span<atx::f64> grads() noexcept override { return {}; }
  void state_to(std::vector<atx::f64> &) const override {}
  void state_from(std::span<const atx::f64>) override {}

private:
  lin::MatX y_cache_; // forward output, for (1 - y²)
};

// ===========================================================================
//  Dropout — inverted dropout with a SEEDED per-forward mask (R1).
//
//  Train mode: draw a Bernoulli(keep) mask from a generator seeded by
//  (master_seed, "nn-drop", call_index); scale kept units by 1/keep so the
//  expected activation is unchanged. Eval mode (training()==false): identity, no
//  RNG draw. The deterministic gradient check runs with dropout OFF.
// ===========================================================================
class Dropout final : public Module {
public:
  // p is the DROP probability in [0, 1). seed is the per-layer base seed; each
  // forward in train mode advances a call counter so successive minibatches get
  // distinct-but-reproducible masks.
  Dropout(atx::f64 p, atx::u64 seed);

  [[nodiscard]] lin::MatX forward(const lin::MatX &x) override;
  [[nodiscard]] lin::MatX backward(const lin::MatX &grad_out) override;
  [[nodiscard]] atx::usize param_count() const noexcept override { return 0; }
  void bind_params(std::span<atx::f64>, std::span<atx::f64>) override {}
  [[nodiscard]] std::span<atx::f64> params() noexcept override { return {}; }
  [[nodiscard]] std::span<atx::f64> grads() noexcept override { return {}; }
  void state_to(std::vector<atx::f64> &) const override {}
  void state_from(std::span<const atx::f64>) override {}

private:
  atx::f64 p_;        // drop probability
  atx::f64 keep_;     // 1 - p
  atx::u64 seed_;     // base seed for the mask stream
  atx::u64 calls_{0}; // forward counter, mixed into the per-call seed
  bool identity_pass_{false}; // last forward was eval/no-drop: skip the mask (R6)
  lin::MatX mask_;    // last applied scaled mask (kept => 1/keep, dropped => 0)
};

// ===========================================================================
//  LayerNorm — per-row (per-sample) normalisation over the feature axis, with a
//  learned affine (gamma, beta), each of length `features`.
//
//    mu_i  = mean_f x_if ;  var_i = mean_f (x_if - mu_i)²
//    xhat  = (x_if - mu_i) / sqrt(var_i + eps)
//    y_if  = gamma_f * xhat_if + beta_f
//
//  Flat param layout (ascending): gamma (features), then beta (features). The
//  backward is the standard LayerNorm vector-Jacobian; mean/var are ascending
//  scalar folds (R1).
// ===========================================================================
class LayerNorm final : public Module {
public:
  explicit LayerNorm(atx::usize features, atx::f64 eps = 1e-5);

  [[nodiscard]] lin::MatX forward(const lin::MatX &x) override;
  [[nodiscard]] lin::MatX backward(const lin::MatX &grad_out) override;

  [[nodiscard]] atx::usize param_count() const noexcept override { return n_params_; }
  void bind_params(std::span<atx::f64> params, std::span<atx::f64> grads) override;
  [[nodiscard]] std::span<atx::f64> params() noexcept override { return params_; }
  [[nodiscard]] std::span<atx::f64> grads() noexcept override { return grads_; }

  void state_to(std::vector<atx::f64> &out) const override;
  void state_from(std::span<const atx::f64> in) override;

  [[nodiscard]] atx::usize features() const noexcept { return features_; }

private:
  atx::usize features_;
  atx::f64 eps_;
  atx::usize n_params_;
  std::span<atx::f64> params_; // [gamma (F), beta (F)]
  std::span<atx::f64> grads_;
  lin::MatX xhat_cache_;   // normalised input (B x F)
  lin::VecX inv_std_cache_; // 1/sqrt(var_i + eps) per row (length B)
};

} // namespace atx::engine::learn::nn
