#pragma once

// Private Stage A representation/control. No option rollback or production dispatch.
#include "cubic_local_weights.hpp"

#include <array>
#include <cstddef>
#include <span>
#include <string>
#include <vector>

namespace atx::vol::cash_gaussian {

struct HermiteCell {
  double lower = 0., upper = 0.;
  double value_lower = 0., value_upper = 0.;
  double slope_lower = 0., slope_upper = 0.;
};

struct CubicAffineTail { double lower = 0., value = 0., slope = 0.; };

struct CubicOwnerBudget {
  std::size_t max_knots = 8192;
  std::size_t max_finite_cells = 65536;
  std::size_t max_moments = 262144;
  std::size_t max_workspace_bytes = 64 * 1024 * 1024;
  LocalWeightBudget cell;
};

enum class CubicCoefficient { Width, ValueIncrement, CoordinateOffset, TailOffset, MiddleSlope };

struct CubicCoefficientRecord {
  CubicCoefficient kind = CubicCoefficient::Width;
  std::string result, absolute_sum, condition;
  bool identical_input_zero = false;
};

struct CubicOwnerWork {
  std::size_t cells_examined = 0, cells_copied = 0, finite_cells = 0;
  std::size_t conversions = 0, coefficient_terms = 0, accumulation_terms = 0;
  // Reservation for owned tuples and bounded arithmetic/string working objects;
  // excludes allocator metadata. It is not measured resident memory.
  std::size_t workspace_bytes = 0;
  LocalWeightWork weights;
  // Fixed bound: point location has two differences, cell evaluation has two.
  std::array<CubicCoefficientRecord, 4> coefficients;
  std::size_t coefficient_records = 0;
  unsigned decimal_digits = 0;
};

class StockCubic {
 public:
  [[nodiscard]] static Result<StockCubic> make(std::span<const HermiteCell> cells,
      CubicAffineTail tail, const CubicOwnerBudget &budget, CubicOwnerWork &work);
  [[nodiscard]] std::span<const HermiteCell> cells() const noexcept { return cells_; }
  [[nodiscard]] const CubicAffineTail &tail() const noexcept { return tail_; }

 private:
  std::vector<HermiteCell> cells_;
  CubicAffineTail tail_;
};

enum class CubicSlopeKind { Smooth, RightLimit, Kink };

struct CubicPoint {
  std::string value, left_derivative, right_derivative;
  CubicSlopeKind slope_kind = CubicSlopeKind::Smooth;
  std::string value_absolute_sum, derivative_absolute_sum, maximum_condition;
};

struct CubicContribution {
  // delta is the interior E[f'(X)*X/S] contribution on this cell. It omits
  // moving-boundary terms; only a complete continuous partition can sum these
  // into a full derivative. It is not the derivative of the truncated value.
  std::string value, delta;
  std::string value_absolute_sum, delta_absolute_sum, maximum_condition;
};

// Exact zero with zero absolute-term sum is valid. Zero from nonzero cancelling
// terms refuses; so does condition >10^(digits-30). Output is transactional.
// cell_coordinate evaluates the actual endpoint tuple at u in [0,1], allowing
// adjacent binary64 knots to have a represented interior at either precision.
[[nodiscard]] Result<CubicPoint> cubic_cell_coordinate(const StockCubic &curve,
    std::size_t cell, double u, WeightPrecision precision,
    const CubicOwnerBudget &budget, CubicOwnerWork &work);

// At a knot, returns the two authoritative one-sided slopes. At stock zero the
// left derivative is empty and RightLimit is explicit. No epsilon snapping.
[[nodiscard]] Result<CubicPoint> cubic_point(const StockCubic &curve, double stock,
    WeightPrecision precision, const CubicOwnerBudget &budget, CubicOwnerWork &work);

// Finite-cell contribution only. Bounds come from the owner; request.lower/upper
// must equal them. Positive stochastic time/stock/volatility only. No tail is
// omitted by a full-curve routine: full-domain integration is not implemented here.
[[nodiscard]] Result<CubicContribution> cubic_cell_contribution(const StockCubic &curve,
    std::size_t cell, const WeightRequest &request, WeightPrecision precision,
    const CubicOwnerBudget &budget, CubicOwnerWork &work);

} // namespace atx::vol::cash_gaussian
