#pragma once

// atx::engine::learn::nn — Loss base + MseLoss / HuberLoss / IcLoss.
//
// =====================================================================
//  Contract
// =====================================================================
//  value(pred, target)  -> scalar loss L (lower is better).
//  grad(pred, target)   -> dL/dpred, same shape as pred.
//
//  pred and target are samples x outputs (rows = batch). Every reduction (the
//  mean over elements, the Pearson moments) is an ASCENDING SCALAR FOLD (R1),
//  never simd::*. The gradient is checked against value() by finite differences
//  in the test, exactly like the layers.

#include "atx/core/types.hpp" // f64

#include "atx/core/linalg/linalg.hpp" // MatX

namespace atx::engine::learn::nn {

namespace lin = atx::core::linalg;

// ===========================================================================
//  Loss — scalar objective with an analytic dL/dpred.
// ===========================================================================
class Loss {
public:
  Loss() = default;
  Loss(const Loss &) = delete;
  Loss &operator=(const Loss &) = delete;
  Loss(Loss &&) = delete;
  Loss &operator=(Loss &&) = delete;
  virtual ~Loss() = default;

  [[nodiscard]] virtual atx::f64 value(const lin::MatX &pred, const lin::MatX &target) = 0;
  [[nodiscard]] virtual lin::MatX grad(const lin::MatX &pred, const lin::MatX &target) = 0;
};

// ===========================================================================
//  MseLoss — mean of (pred - target)² over ALL elements.
//    L = (1/N) Σ (p - t)² ;  dL/dp = (2/N) (p - t)   (N = element count)
// ===========================================================================
class MseLoss final : public Loss {
public:
  [[nodiscard]] atx::f64 value(const lin::MatX &pred, const lin::MatX &target) override;
  [[nodiscard]] lin::MatX grad(const lin::MatX &pred, const lin::MatX &target) override;
};

// ===========================================================================
//  HuberLoss — quadratic for |r| <= delta, linear beyond (robust to outliers).
//    per element: r = p - t ; |r|<=δ -> 0.5 r² ; else δ(|r| - 0.5 δ)
//    mean over all elements. dL/dp: r if |r|<=δ else δ·sign(r), divided by N.
// ===========================================================================
class HuberLoss final : public Loss {
public:
  explicit HuberLoss(atx::f64 delta = 1.0) noexcept : delta_{delta} {}

  [[nodiscard]] atx::f64 value(const lin::MatX &pred, const lin::MatX &target) override;
  [[nodiscard]] lin::MatX grad(const lin::MatX &pred, const lin::MatX &target) override;

private:
  atx::f64 delta_;
};

// ===========================================================================
//  IcLoss — information-coefficient loss: L = 1 - Pearson(pred, target), so
//  minimising L maximises the cross-sectional correlation between prediction and
//  target. pred and target are flattened in ascending (column-major) order and
//  treated as paired samples. Gradient is the analytic d(-corr)/dpred.
//
//  Degenerate guard: if either side has ~zero variance the correlation is
//  undefined; value() returns 1 (no signal) and grad() returns zeros.
// ===========================================================================
class IcLoss final : public Loss {
public:
  explicit IcLoss(atx::f64 eps = 1e-12) noexcept : eps_{eps} {}

  [[nodiscard]] atx::f64 value(const lin::MatX &pred, const lin::MatX &target) override;
  [[nodiscard]] lin::MatX grad(const lin::MatX &pred, const lin::MatX &target) override;

private:
  atx::f64 eps_; // variance floor for the degenerate guard
};

} // namespace atx::engine::learn::nn
