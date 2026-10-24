#include "stock_cubic.hpp"
#include "cubic_point_detail.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <exception>
#include <ios>
#include <limits>
#include <new>
#include <type_traits>
#include <utility>

#include <boost/multiprecision/cpp_bin_float.hpp>

namespace atx::vol::cash_gaussian {
namespace {
using atx::core::Err;
template <unsigned Digits>
using Real = boost::multiprecision::number<boost::multiprecision::cpp_bin_float<Digits>>;
struct NumericRefusal { const char *message; };
constexpr std::size_t maximum_cells = 8191;
constexpr std::size_t maximum_workspace = 64 * 1024 * 1024;

bool same_bits(double a, double b) noexcept {
  return std::bit_cast<std::uint64_t>(a) == std::bit_cast<std::uint64_t>(b);
}

template <class R>
bool finite(const R &x) {
  return x == x && x != std::numeric_limits<R>::infinity() &&
      x != -std::numeric_limits<R>::infinity();
}

template <unsigned Digits>
class Arithmetic {
 public:
  using R = Real<Digits>;
  explicit Arithmetic(CubicOwnerWork &work) : work_(work) {}

  R convert(double x) { ++work_.conversions; return R(x); }
  R convert(const std::string &x) { ++work_.conversions; return R(x); }
  std::string encode(const R &x) {
    ++work_.conversions;
    return x.str(Digits, std::ios_base::scientific);
  }
  // Identical input subtraction is an exact algebraic zero, established before
  // arithmetic. Other zero sums retain their nonzero absolute-term ledger.
  R difference(const R &a, const R &b, CubicCoefficient kind) {
    work_.coefficient_terms += 2;
    if (a == b) {
      record(kind, R(0), R(0), true);
      return R(0);
    }
    const R result = a-b;
    record(kind, result, R(abs(a)+abs(b)), false);
    condition(result, R(abs(a)+abs(b)));
    return result;
  }
  R coefficient_sum(const std::array<R, 3> &terms) {
    R value = 0, absolute_sum = 0;
    for (const auto &term : terms) {
      ++work_.coefficient_terms;
      value += term;
      absolute_sum += abs(term);
    }
    record(CubicCoefficient::MiddleSlope, value, absolute_sum, false);
    condition(value, absolute_sum);
    return value;
  }
  R proved_middle_zero() {
    // Symbolic elimination precedes the floating coefficient sum. The separate
    // exact proof retains the original six input bit patterns and numerator.
    record(CubicCoefficient::MiddleSlope, R(0), R(0), false);
    work_.coefficients[work_.coefficient_records-1].exact_dyadic_zero = true;
    return R(0);
  }
  template <std::size_t N>
  R sum(const std::array<R, N> &terms, R &absolute_sum) {
    R value = 0;
    absolute_sum = 0;
    for (const auto &term : terms) {
      ++work_.accumulation_terms;
      value += term;
      absolute_sum += abs(term);
    }
    condition(value, absolute_sum);
    return value;
  }
  void condition(const R &value, const R &absolute_sum) {
    if (!finite(value) || !finite(absolute_sum) || absolute_sum < 0)
      throw NumericRefusal{"cubic owner nonfinite arithmetic"};
    if (value == 0) {
      if (absolute_sum == 0) return;
      throw NumericRefusal{"cubic owner zero from cancelling nonzero terms"};
    }
    const R ratio = absolute_sum/abs(value);
    if (!finite(ratio) || ratio > limit_)
      throw NumericRefusal{"cubic owner conditioning"};
    if (ratio > maximum_condition) maximum_condition = ratio;
  }
  R maximum_condition = 1;

 private:
  void record(CubicCoefficient kind, const R &value, const R &absolute_sum, bool identity) {
    if (work_.coefficient_records == work_.coefficients.size())
      throw NumericRefusal{"cubic owner coefficient ledger bound"};
    auto &out = work_.coefficients[work_.coefficient_records++];
    out.kind = kind;
    out.result = encode(value);
    out.absolute_sum = encode(absolute_sum);
    out.identical_input_zero = identity;
    out.condition = value == 0 ? (absolute_sum == 0 ? "0" : "unbounded") :
        encode(R(absolute_sum/abs(value)));
  }
  CubicOwnerWork &work_;
  const R limit_{Digits == 100 ? "1e70" : "1e170"};
};

template <unsigned Digits>
std::size_t scalar_reservation(const StockCubic &curve) {
  return curve.cells().size()*sizeof(HermiteCell)+sizeof(CubicAffineTail)+
      128*sizeof(Real<Digits>)+8192;
}

template <unsigned Digits>
bool reserve(const StockCubic &curve, const CubicOwnerBudget &budget,
             CubicOwnerWork &work, bool weighted) {
  if (curve.cells().size() > maximum_cells ||
      curve.cells().size()+1 > std::min<std::size_t>(budget.max_knots, 8192) ||
      (curve.cells().empty() && curve.tail().lower != 0.)) return false;
  // Fixed precision objects, bounded strings, and the owner's existing tuples.
  // Weighted evaluation adds the reviewed cell workspace reservation sequentially.
  const auto bytes = scalar_reservation<Digits>(curve);
  const auto cell_bytes = weighted ? std::min<std::size_t>(budget.cell.max_workspace_bytes,
                                                        1048576) : 0;
  const auto cap = std::min(budget.max_workspace_bytes, maximum_workspace);
  if (bytes > cap || cell_bytes > cap-bytes) return false;
  work.workspace_bytes = bytes+cell_bytes;
  work.decimal_digits = Digits;
  return true;
}

template <class Output, class F>
Result<Output> precision_call(WeightPrecision precision, F &&function) {
  try {
    switch (precision) {
      case WeightPrecision::Decimal100:
        return function(std::integral_constant<unsigned, 100>{});
      case WeightPrecision::Decimal200:
        return function(std::integral_constant<unsigned, 200>{});
    }
    return Err(ErrorCode::InvalidArgument, "cubic owner precision");
  } catch (const NumericRefusal &error) {
    return Err(ErrorCode::OutOfRange, error.message);
  } catch (const std::bad_alloc &) {
    return Err(ErrorCode::Unavailable, "cubic owner allocation");
  } catch (const std::exception &) {
    return Err(ErrorCode::OutOfRange, "cubic owner arithmetic exception");
  }
}

template <unsigned Digits>
CubicPoint direct_cell(const HermiteCell &cell, const Real<Digits> &u,
                      Arithmetic<Digits> &math, CubicOwnerWork &work) {
  using R = Real<Digits>;
  ++work.finite_cells;
  const R a = math.convert(cell.lower), b = math.convert(cell.upper);
  const R v0 = math.convert(cell.value_lower), v1 = math.convert(cell.value_upper);
  const R m0 = math.convert(cell.slope_lower), m1 = math.convert(cell.slope_upper);
  const R h = math.difference(b, a, CubicCoefficient::Width);
  const R delta = math.difference(v1, v0, CubicCoefficient::ValueIncrement);
  R value, derivative, value_abs, derivative_abs;
  if (u == 0 || u == 1) {
    value = u == 0 ? v0 : v1;
    derivative = u == 0 ? m0 : m1;
    value_abs = abs(value);
    derivative_abs = abs(derivative);
  } else {
    const R u2 = u*u, complement = 1-u;
    const R h01 = u2*(3-2*u), h10 = u*complement*complement, h11 = -u2*complement;
    value = math.sum(std::array<R, 4>{v0, delta*h01, h*m0*h10, h*m1*h11}, value_abs);
    derivative = math.sum(std::array<R, 3>{(delta/h)*6*u*complement,
        m0*complement*(1-3*u), m1*u*(3*u-2)}, derivative_abs);
  }
  return {math.encode(value), math.encode(derivative), math.encode(derivative),
      CubicSlopeKind::Smooth, math.encode(value_abs), math.encode(derivative_abs),
      math.encode(math.maximum_condition)};
}

template <unsigned Digits>
CubicPoint knot(double value, double left, double right, bool at_zero,
                Arithmetic<Digits> &math) {
  using R = Real<Digits>;
  const R v = math.convert(value), l = math.convert(left), r = math.convert(right);
  return {math.encode(v), at_zero ? std::string{} : math.encode(l), math.encode(r),
      at_zero ? CubicSlopeKind::RightLimit :
      (left == right ? CubicSlopeKind::Smooth : CubicSlopeKind::Kink),
      math.encode(R(abs(v))), math.encode(R(std::max(R(abs(l)), R(abs(r))))),
      math.encode(math.maximum_condition)};
}

template <unsigned Digits>
Result<CubicPoint> point(const StockCubic &curve, double stock,
                        const CubicOwnerBudget &budget, CubicOwnerWork &work) {
  using R = Real<Digits>;
  if (!reserve<Digits>(curve, budget, work, false))
    return Err(ErrorCode::Unavailable, "cubic owner workspace/knots budget");
  Arithmetic<Digits> math(work);
  const auto cells = curve.cells();
  for (std::size_t i = 0; i < cells.size(); ++i) {
    ++work.cells_examined;
    const auto &cell = cells[i];
    if (stock == cell.lower)
      return knot(cell.value_lower, i == 0 ? cell.slope_lower : cells[i-1].slope_upper,
                  cell.slope_lower, stock == 0., math);
    if (stock < cell.upper) {
      if (budget.max_finite_cells == 0)
        return Err(ErrorCode::Unavailable, "cubic owner cell budget");
      const R x = math.convert(stock), a = math.convert(cell.lower), b = math.convert(cell.upper);
      const R offset = math.difference(x, a, CubicCoefficient::CoordinateOffset);
      const R width = math.difference(b, a, CubicCoefficient::Width);
      const R u = offset/width;
      if (!(u > 0 && u < 1))
        throw NumericRefusal{"cubic owner rounded interior coordinate"};
      return direct_cell(cell, u, math, work);
    }
  }
  const auto &tail = curve.tail();
  if (stock == tail.lower)
    return knot(tail.value, cells.empty() ? tail.slope : cells.back().slope_upper,
                tail.slope, stock == 0., math);
  const R x = math.convert(stock), a = math.convert(tail.lower);
  const R v = math.convert(tail.value), slope = math.convert(tail.slope);
  R absolute_sum;
  const R offset = math.difference(x, a, CubicCoefficient::TailOffset);
  const R value = math.sum(std::array<R, 2>{v, slope*offset}, absolute_sum);
  return CubicPoint{math.encode(value), math.encode(slope), math.encode(slope),
      CubicSlopeKind::Smooth, math.encode(absolute_sum), math.encode(R(abs(slope))),
      math.encode(math.maximum_condition)};
}

template <unsigned Digits>
Result<CubicContribution> contribution(const StockCubic &curve, std::size_t index,
    const WeightRequest &request, const CubicOwnerBudget &budget, CubicOwnerWork &work) {
  using R = Real<Digits>;
  if (!reserve<Digits>(curve, budget, work, true) || budget.max_finite_cells == 0)
    return Err(ErrorCode::Unavailable, "cubic owner workspace/cell budget");
  auto cell_budget = budget.cell;
  cell_budget.moments.max_moments = std::min({cell_budget.moments.max_moments,
      budget.max_moments, std::size_t{262144}});
  ++work.finite_cells;
  auto weights = cubic_local_weight_control(request,
      Digits == 100 ? WeightPrecision::Decimal100 : WeightPrecision::Decimal200,
      cell_budget, work.weights);
  if (!weights) return Err(weights.error());
  Arithmetic<Digits> math(work);
  const auto &cell = curve.cells()[index];
  const R a = math.convert(cell.lower), b = math.convert(cell.upper);
  const R v0 = math.convert(cell.value_lower), v1 = math.convert(cell.value_upper);
  const R m0 = math.convert(cell.slope_lower), m1 = math.convert(cell.slope_upper);
  const R h = math.difference(b, a, CubicCoefficient::Width);
  const R delta = math.difference(v1, v0, CubicCoefficient::ValueIncrement);
  const auto &w = weights->weights;
  math.condition(R(1), math.convert(w.maximum_condition));
  const R mass = math.convert(w.mass), w1 = math.convert(w.value[1]);
  const R w2 = math.convert(w.value[2]), w3 = math.convert(w.value[3]);
  const R d0 = math.convert(w.derivative[0]), d1 = math.convert(w.derivative[1]);
  const R d2 = math.convert(w.derivative[2]);
  const DyadicZeroRequest exact_request{cell.lower, cell.upper, cell.value_lower,
      cell.value_upper, cell.slope_lower, cell.slope_upper};
  auto proof_budget = budget.exact_zero;
  const auto frame = scalar_reservation<Digits>(curve);
  const auto remaining = std::min(budget.max_workspace_bytes, maximum_workspace)-frame;
  proof_budget.max_workspace_bytes = std::min(proof_budget.max_workspace_bytes, remaining);
  const auto proof_bytes = dyadic_zero_workspace_requirement();
  if (proof_budget.max_workspace_bytes >= proof_bytes)
    work.workspace_bytes = std::max(work.workspace_bytes, frame+proof_bytes);
  auto proof = prove_middle_slope_zero(exact_request, proof_budget, work.dyadic_zero);
  if (proof) work.middle_proof = std::move(*proof);
  const std::array expected_bits{std::bit_cast<std::uint64_t>(cell.lower),
      std::bit_cast<std::uint64_t>(cell.upper), std::bit_cast<std::uint64_t>(cell.value_lower),
      std::bit_cast<std::uint64_t>(cell.value_upper),
      std::bit_cast<std::uint64_t>(cell.slope_lower),
      std::bit_cast<std::uint64_t>(cell.slope_upper)};
  const bool proved_zero = work.middle_proof && work.middle_proof->is_zero() &&
      work.middle_proof->input_bits == expected_bits &&
      work.dyadic_zero.status == DyadicZeroStatus::ProvenZero;
  const R middle = proved_zero ? math.proved_middle_zero() :
      math.coefficient_sum(std::array<R, 3>{3*delta/h, -m0, -m1});
  R value_abs, delta_abs;
  const R value = math.sum(std::array<R, 4>{v0*mass, delta*(w2+w3),
      (h/3)*m0*w1, -(h/3)*m1*w2}, value_abs);
  const R derivative = math.sum(std::array<R, 3>{m0*d0, middle*d1, m1*d2}, delta_abs);
  return CubicContribution{math.encode(value), math.encode(derivative),
      math.encode(value_abs), math.encode(delta_abs), math.encode(math.maximum_condition)};
}

template <unsigned Digits>
Result<CubicPoint> decimal_point(const StockCubic &curve, const std::string &stock,
    const CubicOwnerBudget &budget, CubicOwnerWork &work) {
  using R = Real<Digits>;
  if (!reserve<Digits>(curve, budget, work, false))
    return Err(ErrorCode::Unavailable, "cubic decimal point workspace/knots budget");
  Arithmetic<Digits> math(work);
  const R x = math.convert(stock);
  if (!finite(x) || x < 0)
    return Err(ErrorCode::InvalidArgument, "cubic decimal point domain");
  // This entry is only for nonidentity exponential transport. No rounded
  // equality may be interpreted as an authoritative knot identity.
  auto location = [&](const R &knot_value) {
    const R difference = x-knot_value;
    if (difference == 0)
      throw NumericRefusal{"cubic uncertain transported knot"};
    math.condition(difference, R(abs(x)+abs(knot_value)));
  };
  for (const auto &cell : curve.cells()) {
    ++work.cells_examined;
    const R a = math.convert(cell.lower), b = math.convert(cell.upper);
    location(a); location(b);
    if (x < b) {
      if (budget.max_finite_cells == 0)
        return Err(ErrorCode::Unavailable, "cubic decimal point cell budget");
      const R offset = math.difference(x, a, CubicCoefficient::CoordinateOffset);
      const R width = math.difference(b, a, CubicCoefficient::Width);
      const R u = offset/width;
      if (!(u > 0 && u < 1))
        throw NumericRefusal{"cubic decimal point rounded coordinate"};
      return direct_cell(cell, u, math, work);
    }
  }
  const auto &tail = curve.tail();
  const R a = math.convert(tail.lower), v = math.convert(tail.value);
  const R slope = math.convert(tail.slope);
  location(a);
  R absolute_sum;
  const R offset = math.difference(x, a, CubicCoefficient::TailOffset);
  const R value = math.sum(std::array<R, 2>{v, slope*offset}, absolute_sum);
  return CubicPoint{math.encode(value), math.encode(slope), math.encode(slope),
      CubicSlopeKind::Smooth, math.encode(absolute_sum), math.encode(R(abs(slope))),
      math.encode(math.maximum_condition)};
}
} // namespace

Result<StockCubic> StockCubic::make(std::span<const HermiteCell> cells, CubicAffineTail tail,
    const CubicOwnerBudget &budget, CubicOwnerWork &work) {
  work = {};
  if (cells.size() > maximum_cells || cells.size()+1 > budget.max_knots)
    return Err(ErrorCode::Unavailable, "cubic owner knot budget");
  if (!std::isfinite(tail.lower) || !std::isfinite(tail.value) ||
      !std::isfinite(tail.slope) || tail.lower < 0.)
    return Err(ErrorCode::InvalidArgument, "cubic owner tail domain");
  for (std::size_t i = 0; i < cells.size(); ++i) {
    ++work.cells_examined;
    const auto &cell = cells[i];
    if (!std::isfinite(cell.lower) || !std::isfinite(cell.upper) ||
        !std::isfinite(cell.value_lower) || !std::isfinite(cell.value_upper) ||
        !std::isfinite(cell.slope_lower) || !std::isfinite(cell.slope_upper) ||
        cell.lower < 0. || cell.upper <= cell.lower ||
        (i == 0 && cell.lower != 0.) ||
        (i > 0 && (!same_bits(cells[i-1].upper, cell.lower) ||
                   !same_bits(cells[i-1].value_upper, cell.value_lower))))
      return Err(ErrorCode::InvalidArgument, "cubic owner cell continuity/domain");
  }
  if ((cells.empty() && tail.lower != 0.) ||
      (!cells.empty() && (!same_bits(cells.back().upper, tail.lower) ||
                         !same_bits(cells.back().value_upper, tail.value))))
    return Err(ErrorCode::InvalidArgument, "cubic owner tail continuity");
  const auto bytes = cells.size()*sizeof(HermiteCell)+sizeof(CubicAffineTail);
  if (bytes > std::min(budget.max_workspace_bytes, maximum_workspace))
    return Err(ErrorCode::Unavailable, "cubic owner storage budget");
  work.workspace_bytes = bytes;
  try {
    StockCubic out;
    out.cells_.assign(cells.begin(), cells.end());
    work.cells_copied = cells.size();
    out.tail_ = tail;
    return out;
  } catch (const std::bad_alloc &) {
    return Err(ErrorCode::Unavailable, "cubic owner allocation");
  }
}

Result<CubicPoint> cubic_cell_coordinate(const StockCubic &curve, std::size_t cell, double u,
    WeightPrecision precision, const CubicOwnerBudget &budget, CubicOwnerWork &work) {
  work = {};
  if (cell >= curve.cells().size() || !std::isfinite(u) || u < 0. || u > 1.)
    return Err(ErrorCode::InvalidArgument, "cubic owner cell coordinate");
  return precision_call<CubicPoint>(precision, [&](auto digits) -> Result<CubicPoint> {
    constexpr unsigned n = decltype(digits)::value;
    if (u == 0. || u == 1.)
      return point<n>(curve, u == 0. ? curve.cells()[cell].lower : curve.cells()[cell].upper,
                      budget, work);
    if (!reserve<n>(curve, budget, work, false) || budget.max_finite_cells == 0)
      return Err(ErrorCode::Unavailable, "cubic owner workspace/cell budget");
    Arithmetic<n> math(work);
    return direct_cell(curve.cells()[cell], math.convert(u), math, work);
  });
}

Result<CubicPoint> cubic_point(const StockCubic &curve, double stock, WeightPrecision precision,
    const CubicOwnerBudget &budget, CubicOwnerWork &work) {
  work = {};
  if (!std::isfinite(stock) || stock < 0.)
    return Err(ErrorCode::InvalidArgument, "cubic owner stock");
  return precision_call<CubicPoint>(precision, [&](auto digits) {
    return point<decltype(digits)::value>(curve, stock, budget, work);
  });
}

Result<CubicContribution> cubic_cell_contribution(const StockCubic &curve, std::size_t cell,
    const WeightRequest &request, WeightPrecision precision,
    const CubicOwnerBudget &budget, CubicOwnerWork &work) {
  work = {};
  if (cell >= curve.cells().size() || !same_bits(request.lower, curve.cells()[cell].lower) ||
      !same_bits(request.upper, curve.cells()[cell].upper))
    return Err(ErrorCode::InvalidArgument, "cubic owner weight bounds");
  return precision_call<CubicContribution>(precision, [&](auto digits) {
    return contribution<decltype(digits)::value>(curve, cell, request, budget, work);
  });
}

Result<CubicPoint> detail::cubic_decimal_point(const StockCubic &curve,
    const std::string &stock, WeightPrecision precision,
    const CubicOwnerBudget &budget, CubicOwnerWork &work) {
  work = {};
  if (stock.empty() || stock.size() > 256)
    return Err(ErrorCode::InvalidArgument, "cubic decimal point encoding length");
  return precision_call<CubicPoint>(precision, [&](auto digits) {
    return decimal_point<decltype(digits)::value>(curve, stock, budget, work);
  });
}
} // namespace atx::vol::cash_gaussian
