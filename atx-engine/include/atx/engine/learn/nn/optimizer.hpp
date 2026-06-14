#pragma once

// atx::engine::learn::nn — Optimizer base + Sgd (momentum) + Adam.
//
// =====================================================================
//  Contract (R1 — determinism)
// =====================================================================
//  step(params, grads) updates params in place from the parallel grad span. The
//  iteration is ALWAYS ascending over the flat parameter index — the same order
//  Module::params() exposes. There is NO per-step RNG: an optimizer is a pure
//  function of its hyperparameters, its accumulated state, params and grads.
//
//  Stateful optimizers (Sgd with momentum, Adam) lazily size their per-parameter
//  state to params.size() on the FIRST step (zero-initialised, deterministic) and
//  require params.size() to stay fixed thereafter. All updates are elementwise
//  scalar arithmetic in ascending order (no simd::*).

#include <span>
#include <vector>

#include "atx/core/types.hpp" // f64, usize

namespace atx::engine::learn::nn {

// ===========================================================================
//  Optimizer — applies one parameter update from a gradient.
// ===========================================================================
class Optimizer {
public:
  Optimizer() = default;
  Optimizer(const Optimizer &) = delete;
  Optimizer &operator=(const Optimizer &) = delete;
  Optimizer(Optimizer &&) = delete;
  Optimizer &operator=(Optimizer &&) = delete;
  virtual ~Optimizer() = default;

  // Update params in place from grads (parallel spans, equal length). Ascending
  // index order, no RNG.
  virtual void step(std::span<atx::f64> params, std::span<const atx::f64> grads) = 0;

  // Reset accumulated state (momentum / moment estimates) and step counter, so a
  // fresh training run from this optimizer instance is deterministic.
  virtual void reset() noexcept = 0;
};

// ===========================================================================
//  Sgd — vanilla / momentum SGD. v <- momentum*v + grad ; p <- p - lr*v.
//  momentum == 0 is plain gradient descent (no state used).
// ===========================================================================
class Sgd final : public Optimizer {
public:
  explicit Sgd(atx::f64 lr, atx::f64 momentum = 0.0) noexcept;

  void step(std::span<atx::f64> params, std::span<const atx::f64> grads) override;
  void reset() noexcept override;

private:
  atx::f64 lr_;
  atx::f64 momentum_;
  std::vector<atx::f64> velocity_; // sized on first step when momentum != 0
};

// ===========================================================================
//  Adam — Kingma & Ba (2015), bias-corrected. Per-parameter first/second moment
//  estimates m, v sized to params.size() on first step (zero-init):
//    m <- b1*m + (1-b1)*g ; v <- b2*v + (1-b2)*g²
//    mhat = m/(1-b1^t) ; vhat = v/(1-b2^t)
//    p <- p - lr * mhat / (sqrt(vhat) + eps)
//  t is the global step counter (shared across all params), incremented once per
//  step() call. Ascending elementwise; no RNG.
// ===========================================================================
class Adam final : public Optimizer {
public:
  explicit Adam(atx::f64 lr, atx::f64 b1 = 0.9, atx::f64 b2 = 0.999, atx::f64 eps = 1e-8) noexcept;

  void step(std::span<atx::f64> params, std::span<const atx::f64> grads) override;
  void reset() noexcept override;

private:
  atx::f64 lr_;
  atx::f64 b1_;
  atx::f64 b2_;
  atx::f64 eps_;
  atx::u64 t_{0}; // step counter (for bias correction)
  std::vector<atx::f64> m_;
  std::vector<atx::f64> v_;
};

} // namespace atx::engine::learn::nn
