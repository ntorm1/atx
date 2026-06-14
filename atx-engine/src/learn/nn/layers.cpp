#include "atx/engine/learn/nn/layers.hpp"

#include "atx/engine/learn/nn/module.hpp"

#include <cmath>   // std::sqrt, std::tanh
#include <utility> // std::move

#include <Eigen/Dense> // Eigen::Index

#include "atx/core/macro.hpp"  // ATX_ASSERT
#include "atx/core/random.hpp" // Xoshiro256pp
#include "atx/core/types.hpp"  // f64, usize

namespace atx::engine::learn::nn {

// Casts kept local so every size_t<->Eigen::Index boundary is explicit (/Wconversion).
namespace {
[[nodiscard]] Eigen::Index idx(atx::usize n) noexcept { return static_cast<Eigen::Index>(n); }
[[nodiscard]] atx::usize sz(Eigen::Index n) noexcept { return static_cast<atx::usize>(n); }
} // namespace

// =====================================================================
//  Module composition: Sequential
// =====================================================================

Sequential &Sequential::add(std::unique_ptr<Module> child) {
  children_.push_back(std::move(child));
  return *this;
}

atx::usize Sequential::param_count() const noexcept {
  atx::usize n = 0;
  for (const auto &c : children_) {
    n += c->param_count(); // ascending child-order fold (R1)
  }
  return n;
}

void Sequential::bind_params(std::span<atx::f64> params, std::span<atx::f64> grads) {
  ATX_ASSERT(params.size() == param_count());
  ATX_ASSERT(grads.size() == param_count());
  params_view_ = params;
  grads_view_ = grads;
  atx::usize off = 0;
  for (const auto &c : children_) { // ascending: child 0 gets the lowest slice
    const atx::usize n = c->param_count();
    c->bind_params(params.subspan(off, n), grads.subspan(off, n));
    off += n;
  }
}

void Sequential::build() {
  const atx::usize n = param_count();
  owned_params_.assign(n, 0.0);
  owned_grads_.assign(n, 0.0);
  bind_params(std::span<atx::f64>(owned_params_), std::span<atx::f64>(owned_grads_));
}

lin::MatX Sequential::forward(const lin::MatX &x) {
  lin::MatX h = x;
  for (const auto &c : children_) { // front to back
    h = c->forward(h);
  }
  return h;
}

lin::MatX Sequential::backward(const lin::MatX &grad_out) {
  lin::MatX g = grad_out;
  for (atx::usize i = children_.size(); i-- > 0;) { // back to front
    g = children_[i]->backward(g);
  }
  return g;
}

void Sequential::train(bool on) noexcept {
  training_ = on;
  for (const auto &c : children_) {
    c->train(on);
  }
}

void Sequential::state_to(std::vector<atx::f64> &out) const {
  for (const auto &c : children_) { // ascending child order == ascending param order
    c->state_to(out);
  }
}

void Sequential::state_from(std::span<const atx::f64> in) {
  ATX_ASSERT(in.size() == param_count());
  atx::usize off = 0;
  for (const auto &c : children_) {
    const atx::usize n = c->param_count();
    c->state_from(in.subspan(off, n));
    off += n;
  }
}

// =====================================================================
//  Module composition: Residual (y = f(x) + x)
// =====================================================================

Residual::Residual(std::unique_ptr<Module> body) : body_{std::move(body)} {}

atx::usize Residual::param_count() const noexcept { return body_->param_count(); }

void Residual::bind_params(std::span<atx::f64> params, std::span<atx::f64> grads) {
  params_view_ = params;
  grads_view_ = grads;
  body_->bind_params(params, grads);
}

void Residual::build() {
  const atx::usize n = param_count();
  owned_params_.assign(n, 0.0);
  owned_grads_.assign(n, 0.0);
  bind_params(std::span<atx::f64>(owned_params_), std::span<atx::f64>(owned_grads_));
}

lin::MatX Residual::forward(const lin::MatX &x) { return body_->forward(x) + x; }

lin::MatX Residual::backward(const lin::MatX &grad_out) {
  // y = f(x) + x  =>  dL/dx = f.backward(dL/dy) + dL/dy (skip path local grad = 1).
  return body_->backward(grad_out) + grad_out;
}

void Residual::train(bool on) noexcept {
  training_ = on;
  body_->train(on);
}

void Residual::state_to(std::vector<atx::f64> &out) const { body_->state_to(out); }
void Residual::state_from(std::span<const atx::f64> in) { body_->state_from(in); }

// =====================================================================
//  Linear
// =====================================================================

Linear::Linear(atx::usize in_features, atx::usize out_features, bool bias)
    : in_{in_features}, out_{out_features}, bias_{bias},
      n_params_{in_features * out_features + (bias ? out_features : 0)} {}

void Linear::bind_params(std::span<atx::f64> params, std::span<atx::f64> grads) {
  ATX_ASSERT(params.size() == n_params_);
  ATX_ASSERT(grads.size() == n_params_);
  params_ = params;
  grads_ = grads;
}

lin::MatMap Linear::weight() noexcept {
  return lin::MatMap(params_.data(), idx(in_), idx(out_)); // column-major in x out
}
lin::MatMapConst Linear::weight() const noexcept {
  return lin::MatMapConst(params_.data(), idx(in_), idx(out_));
}
lin::MatMap Linear::weight_grad() noexcept {
  return lin::MatMap(grads_.data(), idx(in_), idx(out_));
}

lin::MatX Linear::forward(const lin::MatX &x) {
  x_cache_ = x; // B x in, needed for dL/dW
  lin::MatX y = x * weight(); // (B x in)(in x out) = B x out
  if (bias_) {
    // bias span follows the weight block: params_[in*out .. in*out+out)
    const lin::VecMapConst b(params_.data() + in_ * out_, idx(out_));
    y.rowwise() += b.transpose(); // broadcast bias over rows
  }
  return y;
}

lin::MatX Linear::backward(const lin::MatX &grad_out) {
  // dL/dW += xᵀ · grad_out (matmul, allowed) ; dL/dx = grad_out · Wᵀ.
  weight_grad().noalias() += x_cache_.transpose() * grad_out;
  if (bias_) {
    // dL/db_o += Σ_rows grad_out(:, o). Explicit ascending scalar fold per output
    // (R1: a reduction we write is an ordered loop, not an Eigen colwise sum).
    lin::VecMap bg(grads_.data() + in_ * out_, idx(out_));
    for (Eigen::Index o = 0; o < grad_out.cols(); ++o) {
      atx::f64 acc = 0.0;
      for (Eigen::Index r = 0; r < grad_out.rows(); ++r) {
        acc += grad_out(r, o);
      }
      bg(o) += acc;
    }
  }
  return grad_out * weight().transpose(); // B x in
}

void Linear::state_to(std::vector<atx::f64> &out) const {
  out.insert(out.end(), params_.begin(), params_.end()); // ascending == bind order
}
void Linear::state_from(std::span<const atx::f64> in) {
  ATX_ASSERT(in.size() == n_params_);
  for (atx::usize i = 0; i < n_params_; ++i) {
    params_[i] = in[i];
  }
}

// =====================================================================
//  Activations
// =====================================================================

lin::MatX Identity::forward(const lin::MatX &x) { return x; }
lin::MatX Identity::backward(const lin::MatX &grad_out) { return grad_out; }

lin::MatX ReLU::forward(const lin::MatX &x) {
  mask_ = (x.array() > 0.0).cast<atx::f64>(); // 1 where positive else 0
  return x.cwiseMax(0.0);
}
lin::MatX ReLU::backward(const lin::MatX &grad_out) {
  return grad_out.cwiseProduct(mask_); // gate by forward sign
}

lin::MatX Tanh::forward(const lin::MatX &x) {
  y_cache_ = x.array().tanh().matrix();
  return y_cache_;
}
lin::MatX Tanh::backward(const lin::MatX &grad_out) {
  // d/dx tanh = 1 - tanh² = 1 - y².
  const lin::MatX deriv = (1.0 - y_cache_.array().square()).matrix();
  return grad_out.cwiseProduct(deriv);
}

// =====================================================================
//  Dropout (inverted, seeded mask)
// =====================================================================

Dropout::Dropout(atx::f64 p, atx::u64 seed) : p_{p}, keep_{1.0 - p}, seed_{seed} {}

lin::MatX Dropout::forward(const lin::MatX &x) {
  if (!training_ || p_ <= 0.0) {
    // Eval / no-drop: identity. R6 — zero-alloc: flag the pass so backward returns
    // grad_out untouched, instead of materialising an all-ones mask every forward.
    identity_pass_ = true;
    return x;
  }
  identity_pass_ = false;
  // Per-forward seed: base seed mixed with the call counter so successive
  // minibatches get distinct-but-reproducible masks (pure fn of seed_, calls_).
  atx::core::Xoshiro256pp rng{seed_ ^ (calls_ * 0x9E3779B97F4A7C15ULL)};
  ++calls_;
  mask_.resize(x.rows(), x.cols());
  const atx::f64 inv_keep = 1.0 / keep_;
  // Column-major ascending draw order (R1): walk col then row, the Map order.
  for (Eigen::Index c = 0; c < x.cols(); ++c) {
    for (Eigen::Index r = 0; r < x.rows(); ++r) {
      mask_(r, c) = rng.bernoulli(keep_) ? inv_keep : 0.0;
    }
  }
  return x.cwiseProduct(mask_);
}

lin::MatX Dropout::backward(const lin::MatX &grad_out) {
  if (identity_pass_) {
    return grad_out; // eval/no-drop forward had no mask (R6 zero-alloc path)
  }
  return grad_out.cwiseProduct(mask_); // same mask as forward
}

// =====================================================================
//  LayerNorm (per-row over features)
// =====================================================================

LayerNorm::LayerNorm(atx::usize features, atx::f64 eps)
    : features_{features}, eps_{eps}, n_params_{2 * features} {}

void LayerNorm::bind_params(std::span<atx::f64> params, std::span<atx::f64> grads) {
  ATX_ASSERT(params.size() == n_params_);
  ATX_ASSERT(grads.size() == n_params_);
  params_ = params;
  grads_ = grads;
}

lin::MatX LayerNorm::forward(const lin::MatX &x) {
  const Eigen::Index B = x.rows();
  const Eigen::Index F = x.cols();
  ATX_ASSERT(sz(F) == features_);
  const lin::VecMapConst gamma(params_.data(), F);
  const lin::VecMapConst beta(params_.data() + features_, F);
  const atx::f64 invF = 1.0 / static_cast<atx::f64>(F);

  xhat_cache_.resize(B, F);
  inv_std_cache_.resize(B);
  lin::MatX y(B, F);
  for (Eigen::Index i = 0; i < B; ++i) {
    // Per-row mean / variance as ascending scalar folds (R1).
    atx::f64 mu = 0.0;
    for (Eigen::Index f = 0; f < F; ++f) {
      mu += x(i, f);
    }
    mu *= invF;
    atx::f64 var = 0.0;
    for (Eigen::Index f = 0; f < F; ++f) {
      const atx::f64 d = x(i, f) - mu;
      var += d * d;
    }
    var *= invF;
    const atx::f64 inv_std = 1.0 / std::sqrt(var + eps_);
    inv_std_cache_(i) = inv_std;
    for (Eigen::Index f = 0; f < F; ++f) {
      const atx::f64 xh = (x(i, f) - mu) * inv_std;
      xhat_cache_(i, f) = xh;
      y(i, f) = gamma(f) * xh + beta(f);
    }
  }
  return y;
}

lin::MatX LayerNorm::backward(const lin::MatX &grad_out) {
  const Eigen::Index B = grad_out.rows();
  const Eigen::Index F = grad_out.cols();
  const lin::VecMapConst gamma(params_.data(), F);
  lin::VecMap dgamma(grads_.data(), F);
  lin::VecMap dbeta(grads_.data() + features_, F);
  const atx::f64 invF = 1.0 / static_cast<atx::f64>(F);

  lin::MatX dx(B, F);
  for (Eigen::Index i = 0; i < B; ++i) {
    // dL/dxhat = grad_out ⊙ gamma. Accumulate gamma/beta grads (ascending fold).
    // For the input grad we need the two per-row means:
    //   m1 = mean_f dxhat ; m2 = mean_f (dxhat * xhat)
    atx::f64 m1 = 0.0;
    atx::f64 m2 = 0.0;
    for (Eigen::Index f = 0; f < F; ++f) {
      const atx::f64 xh = xhat_cache_(i, f);
      const atx::f64 go = grad_out(i, f);
      const atx::f64 dxhat = go * gamma(f);
      dgamma(f) += go * xh; // dL/dgamma_f += grad_out * xhat
      dbeta(f) += go;       // dL/dbeta_f  += grad_out
      m1 += dxhat;
      m2 += dxhat * xh;
    }
    m1 *= invF;
    m2 *= invF;
    const atx::f64 r = inv_std_cache_(i);
    for (Eigen::Index f = 0; f < F; ++f) {
      const atx::f64 xh = xhat_cache_(i, f);
      const atx::f64 dxhat = grad_out(i, f) * gamma(f);
      // Standard LayerNorm input grad: r * (dxhat - m1 - xhat * m2).
      dx(i, f) = r * (dxhat - m1 - xh * m2);
    }
  }
  return dx;
}

void LayerNorm::state_to(std::vector<atx::f64> &out) const {
  out.insert(out.end(), params_.begin(), params_.end());
}
void LayerNorm::state_from(std::span<const atx::f64> in) {
  ATX_ASSERT(in.size() == n_params_);
  for (atx::usize i = 0; i < n_params_; ++i) {
    params_[i] = in[i];
  }
}

} // namespace atx::engine::learn::nn
