#include "atx/engine/learn/nn/optimizer.hpp"

#include <cmath> // std::sqrt, std::pow

#include "atx/core/macro.hpp" // ATX_ASSERT
#include "atx/core/types.hpp" // f64, usize

namespace atx::engine::learn::nn {

// =====================================================================
//  Sgd — v <- momentum*v + grad ; p <- p - lr*v.
// =====================================================================

Sgd::Sgd(atx::f64 lr, atx::f64 momentum) noexcept : lr_{lr}, momentum_{momentum} {}

void Sgd::step(std::span<atx::f64> params, std::span<const atx::f64> grads) {
  ATX_ASSERT(params.size() == grads.size());
  const atx::usize n = params.size();
  if (momentum_ == 0.0) {
    for (atx::usize i = 0; i < n; ++i) { // ascending (R1)
      params[i] -= lr_ * grads[i];
    }
    return;
  }
  if (velocity_.size() != n) {
    velocity_.assign(n, 0.0); // first step: zero-init state, deterministic
  }
  for (atx::usize i = 0; i < n; ++i) { // ascending (R1)
    velocity_[i] = momentum_ * velocity_[i] + grads[i];
    params[i] -= lr_ * velocity_[i];
  }
}

void Sgd::reset() noexcept { velocity_.clear(); }

// =====================================================================
//  Adam — bias-corrected (Kingma & Ba 2015).
// =====================================================================

Adam::Adam(atx::f64 lr, atx::f64 b1, atx::f64 b2, atx::f64 eps) noexcept
    : lr_{lr}, b1_{b1}, b2_{b2}, eps_{eps} {}

void Adam::step(std::span<atx::f64> params, std::span<const atx::f64> grads) {
  ATX_ASSERT(params.size() == grads.size());
  const atx::usize n = params.size();
  if (m_.size() != n) {
    m_.assign(n, 0.0); // first step: zero-init moments, deterministic
    v_.assign(n, 0.0);
  }
  ++t_;
  // Bias-correction denominators (shared across params; one pow per step).
  const atx::f64 bc1 = 1.0 - std::pow(b1_, static_cast<atx::f64>(t_));
  const atx::f64 bc2 = 1.0 - std::pow(b2_, static_cast<atx::f64>(t_));
  for (atx::usize i = 0; i < n; ++i) { // ascending (R1)
    const atx::f64 g = grads[i];
    m_[i] = b1_ * m_[i] + (1.0 - b1_) * g;
    v_[i] = b2_ * v_[i] + (1.0 - b2_) * g * g;
    const atx::f64 mhat = m_[i] / bc1;
    const atx::f64 vhat = v_[i] / bc2;
    params[i] -= lr_ * mhat / (std::sqrt(vhat) + eps_);
  }
}

void Adam::reset() noexcept {
  m_.clear();
  v_.clear();
  t_ = 0;
}

} // namespace atx::engine::learn::nn
