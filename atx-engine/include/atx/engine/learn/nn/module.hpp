#pragma once

// atx::engine::learn::nn — the Module base + Sequential / Residual composition.
//
// =====================================================================
//  What this header is (S5-2a framework)
// =====================================================================
//  The pluggable substrate the four deep-learning alphas plug into. A `Module`
//  is anything with a forward / backward / parameter surface; layers (Linear,
//  LayerNorm, …), containers (Sequential, Residual) and whole networks are all
//  Modules. Backward passes are HAND WRITTEN — there is no autodiff library; the
//  finite-difference gradient check in the test is the correctness proof (R3).
//
// =====================================================================
//  The flat-buffer parameter contract (R1 — determinism)
// =====================================================================
//  Optimizers and the determinism digest both consume params()/grads() as ONE
//  FLAT span in ASCENDING order. To make a single contiguous span cover a whole
//  network, ownership of the parameter bytes is centralised:
//
//    - A LEAF module (Linear, LayerNorm) DECLARES how many parameter scalars it
//      needs (param_count()) and, once handed a sub-span of a shared buffer via
//      bind_params(), aliases that storage for its weights/grads. It never owns
//      the bytes itself.
//    - A CONTAINER (Sequential, Residual) owns the single contiguous backing
//      buffer for the whole subtree, lays its children out in child order
//      (ascending), and binds each child to its slice. params()/grads() then
//      return a span over the whole buffer — one flat, ascending view.
//
//  This means a network is built bottom-up (children constructed first) and the
//  ROOT container's params()/grads() is the flat optimizer/serialization surface.
//  Serialization (state_to/state_from) walks the same buffer, so the byte order
//  of the digest equals the ascending param order (R1).
//
//  Column-major note (R1): Eigen MatX storage is column-major. Every leaf that
//  maps a flat slice to a weight matrix MUST pick an explicit, fixed flattening
//  convention and use it for BOTH bind and serialize. Linear uses column-major
//  (the natural Eigen::Map order); see layers.hpp.

#include <memory> // std::unique_ptr
#include <span>
#include <vector>

#include "atx/core/types.hpp" // f64, usize

#include "atx/core/linalg/linalg.hpp" // MatX, VecX

namespace atx::engine::learn::nn {

namespace lin = atx::core::linalg;

// ===========================================================================
//  Module — the polymorphic NN building block.
//
//  Lifecycle: construct (declares param_count) -> bind_params(slice) wires the
//  leaf's weights to shared storage -> [seed-init writes the slice] -> forward /
//  backward / optimizer.step over params()/grads().
//
//  forward caches whatever the matching backward needs (inputs, normalisation
//  stats, dropout mask). backward returns dL/dx and ACCUMULATES dL/dparam into
//  the grad slice (callers zero grads before a fresh accumulation; the Trainer
//  does this each minibatch). All explicit reductions are ascending scalar folds
//  (R1) — never simd::*.
// ===========================================================================
class Module {
public:
  Module() = default;
  Module(const Module &) = delete;
  Module &operator=(const Module &) = delete;
  Module(Module &&) = delete;
  Module &operator=(Module &&) = delete;
  virtual ~Module() = default;

  // Forward pass; caches activations needed by backward. x is samples x features
  // (rows = batch). Returns the layer output (rows = batch).
  [[nodiscard]] virtual lin::MatX forward(const lin::MatX &x) = 0;

  // Backward pass: given dL/dy (grad_out, same shape as forward's output),
  // accumulate dL/dparam into the grad slice and return dL/dx (shape of the
  // forward input). Must be called after a matching forward.
  [[nodiscard]] virtual lin::MatX backward(const lin::MatX &grad_out) = 0;

  // ---- parameter surface (flat, ascending) ----

  // Total number of trainable scalars in this subtree. Pure structural query;
  // valid before bind_params.
  [[nodiscard]] virtual atx::usize param_count() const noexcept = 0;

  // Wire this subtree's parameters and gradients to caller-owned contiguous
  // storage. Both spans MUST be exactly param_count() long. After binding,
  // params()/grads() alias these spans. A container forwards disjoint, ascending
  // sub-spans to its children in child order.
  virtual void bind_params(std::span<atx::f64> params, std::span<atx::f64> grads) = 0;

  // The flat ascending parameter / gradient view (valid after bind_params).
  [[nodiscard]] virtual std::span<atx::f64> params() noexcept = 0;
  [[nodiscard]] virtual std::span<atx::f64> grads() noexcept = 0;

  // Toggle train mode (enables dropout, switches norm to batch behaviour where
  // relevant). Propagates to children.
  virtual void train(bool on) noexcept { training_ = on; }
  [[nodiscard]] bool training() const noexcept { return training_; }

  // Serialize / restore the parameter bytes in ascending order. state_to appends
  // param_count() scalars; state_from consumes exactly param_count(). This is the
  // determinism digest surface (R1) — byte order == ascending param order.
  virtual void state_to(std::vector<atx::f64> &out) const = 0;
  virtual void state_from(std::span<const atx::f64> in) = 0;

protected:
  bool training_ = false;
};

// ===========================================================================
//  Sequential — ordered composition y = f_{n-1}(…f_0(x)…).
//
//  Owns the single contiguous parameter/grad buffer for the whole chain and lays
//  children out in construction order (ascending). forward runs children front to
//  back; backward runs them back to front, threading dL/dx.
// ===========================================================================
class Sequential final : public Module {
public:
  Sequential() = default;

  // Append a child. Children are laid out in append order; call build() once all
  // children are added to allocate + bind the shared buffer.
  Sequential &add(std::unique_ptr<Module> child);

  // Allocate the flat buffer (sized to the sum of child param_counts) and bind
  // each child to its ascending slice. Call exactly once, after all add()s and
  // before forward/optimizer use. Self-owned buffer path (root use).
  void build();

  [[nodiscard]] lin::MatX forward(const lin::MatX &x) override;
  [[nodiscard]] lin::MatX backward(const lin::MatX &grad_out) override;

  [[nodiscard]] atx::usize param_count() const noexcept override;
  void bind_params(std::span<atx::f64> params, std::span<atx::f64> grads) override;
  [[nodiscard]] std::span<atx::f64> params() noexcept override { return params_view_; }
  [[nodiscard]] std::span<atx::f64> grads() noexcept override { return grads_view_; }

  void train(bool on) noexcept override;
  void state_to(std::vector<atx::f64> &out) const override;
  void state_from(std::span<const atx::f64> in) override;

  [[nodiscard]] atx::usize size() const noexcept { return children_.size(); }
  [[nodiscard]] Module &at(atx::usize i) noexcept { return *children_[i]; }

private:
  std::vector<std::unique_ptr<Module>> children_;
  std::vector<atx::f64> owned_params_; // populated only when build() is called
  std::vector<atx::f64> owned_grads_;
  std::span<atx::f64> params_view_; // aliases owned_* (root) or the parent slice
  std::span<atx::f64> grads_view_;
};

// ===========================================================================
//  Residual — y = f(x) + x. Shapes of f(x) and x MUST match (no projection).
//
//  Owns its child's parameter buffer slice through the same bind mechanism.
//  backward: dL/dx = f.backward(dL/dy) + dL/dy (the skip path's local grad is 1).
// ===========================================================================
class Residual final : public Module {
public:
  explicit Residual(std::unique_ptr<Module> body);

  // Allocate + bind the body's buffer (root use; mirrors Sequential::build).
  void build();

  [[nodiscard]] lin::MatX forward(const lin::MatX &x) override;
  [[nodiscard]] lin::MatX backward(const lin::MatX &grad_out) override;

  [[nodiscard]] atx::usize param_count() const noexcept override;
  void bind_params(std::span<atx::f64> params, std::span<atx::f64> grads) override;
  [[nodiscard]] std::span<atx::f64> params() noexcept override { return params_view_; }
  [[nodiscard]] std::span<atx::f64> grads() noexcept override { return grads_view_; }

  void train(bool on) noexcept override;
  void state_to(std::vector<atx::f64> &out) const override;
  void state_from(std::span<const atx::f64> in) override;

private:
  std::unique_ptr<Module> body_;
  std::vector<atx::f64> owned_params_;
  std::vector<atx::f64> owned_grads_;
  std::span<atx::f64> params_view_;
  std::span<atx::f64> grads_view_;
};

} // namespace atx::engine::learn::nn
