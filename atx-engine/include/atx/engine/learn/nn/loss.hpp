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

#include <span>   // std::span (row-group labels)
#include <vector> // std::vector (IcLoss row groups)

#include "atx/core/types.hpp" // f64, u8, u32

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

  // Row-group labels for the following value()/grad() calls (W0-L0, L-08):
  // groups[i] is the group (the date) of pred row i. A loss that reduces per group
  // (IcLoss::PerGroupMeanV2) copies and uses them; element-wise losses ignore them.
  // An empty span clears them. The Trainer sets them per minibatch / validation pass
  // when it is given row groups (nn::RowGroups) and clears them otherwise.
  virtual void set_row_groups(std::span<const atx::u32> groups) { static_cast<void>(groups); }
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
//  IcReduction — how IcLoss reduces a batch (W0-L0, L-08).
//
//  PooledV1       : legacy. One Pearson over the whole flattened batch. On a
//                   shuffled batch that mixes dates, this rewards predicting the
//                   DATE-level mean (market timing), not the cross-section.
//  PerGroupMeanV2 : default. L = 1 - mean_g rho_g over the batch's row groups (dates,
//                   set via set_row_groups); a group with < 2 elements or ~zero
//                   variance on either side is excluded. With NO group labels set,
//                   the whole batch is one group (identical to PooledV1).
// ===========================================================================
enum class IcReduction : atx::u8 { PooledV1 = 0, PerGroupMeanV2 = 1 };

// ===========================================================================
//  IcLoss — information-coefficient loss: L = 1 - Pearson(pred, target), so
//  minimising L maximises the cross-sectional correlation between prediction and
//  target. Within a group, pred and target are flattened in ascending (column-major,
//  then row) order and treated as paired samples. Gradient is the analytic
//  d(-corr)/dpred; under PerGroupMeanV2 each included group's rows carry
//  -(1/G) d(rho_g)/dpred and excluded groups' rows carry 0.
//
//  Degenerate guard: if either side has ~zero variance the correlation is
//  undefined; that group is excluded (value() is 1 and grad() zero when no group
//  survives).
//  PRECONDITION (set_row_groups non-empty): groups.size() == pred.rows() at the next
//  value()/grad() call (ATX_CHECK — a mismatch aborts in every build).
// ===========================================================================
class IcLoss final : public Loss {
public:
  explicit IcLoss(atx::f64 eps = 1e-12,
                  IcReduction reduction = IcReduction::PerGroupMeanV2) noexcept
      : eps_{eps}, reduction_{reduction} {}

  [[nodiscard]] atx::f64 value(const lin::MatX &pred, const lin::MatX &target) override;
  [[nodiscard]] lin::MatX grad(const lin::MatX &pred, const lin::MatX &target) override;
  void set_row_groups(std::span<const atx::u32> groups) override;

private:
  atx::f64 eps_;            // variance floor for the degenerate guard
  IcReduction reduction_;   // pooled (legacy) vs per-group mean
  std::vector<atx::u32> groups_; // row -> group for the next value()/grad(); empty = one group
};

} // namespace atx::engine::learn::nn
