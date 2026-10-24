#include "cubic_expectation.hpp"
#include "cubic_point_detail.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <exception>
#include <ios>
#include <limits>
#include <new>
#include <type_traits>

#include <boost/math/special_functions/erf.hpp>
#include <boost/multiprecision/cpp_bin_float.hpp>

namespace atx::vol::cash_gaussian {
namespace {
using atx::core::Err;
template <unsigned Digits>
using Real = boost::multiprecision::number<boost::multiprecision::cpp_bin_float<Digits>>;
struct Refusal { ErrorCode code; const char *message; };
constexpr std::size_t workspace_cap = 64*1024*1024;
constexpr std::size_t cell_workspace_cap = 1048576;
constexpr std::size_t cell_conversion_reservation = 128;
constexpr std::size_t cell_term_reservation = 16;

template <class R>
bool finite(const R &x) {
  return x == x && x != std::numeric_limits<R>::infinity() &&
      x != -std::numeric_limits<R>::infinity();
}

void require_room(std::size_t used, std::size_t amount, std::size_t cap) {
  if (used > cap || amount > cap-used)
    throw Refusal{ErrorCode::Unavailable, "cubic expectation work budget"};
}

void add(std::size_t &used, std::size_t amount, std::size_t cap) {
  require_room(used, amount, cap);
  used += amount;
}

std::size_t remaining(std::size_t used, std::size_t cap) {
  require_room(used, 0, cap);
  return cap-used;
}

template <unsigned Digits>
class Context {
 public:
  using R = Real<Digits>;
  Context(const StockCubic &curve, const CubicExpectationBudget &budget,
          CubicExpectationWork &work, std::size_t frame)
      : curve_(curve), budget_(budget), work_(work), frame_(frame) {}

  static std::size_t reserve(const StockCubic &curve, const CubicExpectationBudget &budget,
                            CubicExpectationWork &work, bool weighted) {
    work.phase = CubicExpectationPhase::Preflight;
    if (curve.cells().size() > 8191 ||
        curve.cells().size()+1 > std::min<std::size_t>(budget.owner.max_knots, 8192))
      throw Refusal{ErrorCode::Unavailable, "cubic expectation knot budget"};
    const auto storage = curve.cells().size()*sizeof(HermiteCell)+sizeof(CubicAffineTail);
    // Global context/ledger and the maximum sequential child are simultaneously
    // live. Strings have bounded precision; allocator metadata is not a claim.
    const auto frame = storage+256*sizeof(R)+16384+sizeof(CubicExpectationWork);
    const auto point_frame = storage+128*sizeof(R)+8192;
    std::size_t child = point_frame;
    if (weighted && !curve.cells().empty()) {
      const auto cell = std::min(budget.owner.cell.max_workspace_bytes, cell_workspace_cap);
      const auto proof = budget.owner.exact_zero.max_workspace_bytes >=
          dyadic_zero_workspace_requirement() ? dyadic_zero_workspace_requirement() : 0;
      child += std::max(cell, proof);
    }
    const auto cap = std::min(budget.owner.max_workspace_bytes, workspace_cap);
    require_room(frame, child, cap);
    work.workspace_bytes = frame+child;
    work.decimal_digits = Digits;
    return frame;
  }

  R convert(double x) { charge_conversion(); return R(x); }
  R convert(const std::string &x) { charge_conversion(); return R(x); }
  std::string encode(const R &x) {
    charge_conversion();
    return x.str(Digits, std::ios_base::scientific);
  }
  void charge_conversion() {
    add(work_.conversions, 1, conversion_cap());
  }
  void coefficient(std::size_t n) {
    add(work_.coefficient_terms, n, coefficient_cap());
  }
  void accumulate(std::size_t n) {
    add(work_.accumulation_terms, n, accumulation_cap());
  }
  void elementary() {
    add(work_.elementary_functions, 1,
        std::min<std::size_t>(budget_.max_elementary_functions, 64));
  }
  R exponential(const R &exponent) {
    if (!finite(exponent) || abs(exponent) > 32768)
      throw Refusal{ErrorCode::OutOfRange, "cubic expectation exponential domain"};
    elementary();
    const R value = exp(exponent);
    positive(value, "cubic expectation exponential underflow/overflow");
    return value;
  }
  void positive(const R &x, const char *message) {
    if (!finite(x) || !(x > 0)) throw Refusal{ErrorCode::OutOfRange, message};
  }
  R condition(const R &value, const R &absolute_sum) {
    if (!finite(value) || !finite(absolute_sum) || absolute_sum < 0)
      throw Refusal{ErrorCode::OutOfRange, "cubic expectation nonfinite contraction"};
    if (value == 0) {
      if (absolute_sum == 0) return R(0);
      throw Refusal{ErrorCode::OutOfRange, "cubic expectation unresolved zero cancellation"};
    }
    const R ratio = absolute_sum/abs(value);
    if (!finite(ratio) || ratio > limit_)
      throw Refusal{ErrorCode::OutOfRange, "cubic expectation contraction conditioning"};
    maximum_condition = std::max(maximum_condition, ratio);
    return ratio;
  }

  void reserve_owner_counts(std::size_t conversions = cell_conversion_reservation) {
    require_room(work_.conversions, conversions, conversion_cap());
    require_room(work_.coefficient_terms, cell_term_reservation, coefficient_cap());
    require_room(work_.accumulation_terms, cell_term_reservation, accumulation_cap());
  }

  CubicOwnerBudget child_budget() const {
    auto out = budget_.owner;
    out.max_workspace_bytes = std::min(out.max_workspace_bytes, workspace_cap)-frame_;
    out.max_finite_cells = remaining(work_.finite_attempts,
        std::min<std::size_t>(budget_.owner.max_finite_cells, 65536));
    out.max_moments = remaining(work_.moment_attempts, moment_cap());
    out.cell.moments.max_moments = std::min(out.cell.moments.max_moments, out.max_moments);
    out.cell.moments.max_normal_functions = std::min(out.cell.moments.max_normal_functions,
        remaining(work_.normal_functions, normal_cap()));
    out.cell.max_selection_checks = std::min(out.cell.max_selection_checks,
        remaining(work_.selection_checks, selection_cap()));
    out.cell.max_coefficient_products = std::min(out.cell.max_coefficient_products,
        remaining(work_.coefficient_products, product_cap()));
    out.cell.max_local_functions = std::min(out.cell.max_local_functions,
        remaining(work_.local_functions, function_cap()));
    return out;
  }

  void absorb_child() {
    const auto &child = work_.last_cell;
    add(work_.conversions, child.conversions, conversion_cap());
    add(work_.coefficient_terms, child.coefficient_terms, coefficient_cap());
    add(work_.accumulation_terms, child.accumulation_terms, accumulation_cap());
    add(work_.moment_attempts, child.weights.moments.moment_attempts, moment_cap());
    add(work_.normal_functions, child.weights.moments.normal_functions, normal_cap());
    add(work_.selection_checks, child.weights.selection_checks, selection_cap());
    add(work_.coefficient_products, child.weights.coefficient_products, product_cap());
    add(work_.local_functions, child.weights.local_functions, function_cap());
    add(work_.polynomial_terms, child.weights.moments.polynomial_terms,
        std::numeric_limits<std::size_t>::max());
    if (child.dyadic_zero.status != DyadicZeroStatus::NotAttempted) {
      ++work_.exact_proof_attempts;
      if (child.dyadic_zero.status == DyadicZeroStatus::ProvenZero) ++work_.exact_zeros;
      else if (child.dyadic_zero.status != DyadicZeroStatus::Nonzero) ++work_.exact_proof_refusals;
    }
    auto &sum = work_.dyadic;
    const auto &d = child.dyadic_zero;
    const auto cap = std::numeric_limits<std::size_t>::max();
    add(sum.operands, d.operands, cap); add(sum.shifts, d.shifts, cap);
    add(sum.sums, d.sums, cap); add(sum.products, d.products, cap);
    add(sum.masks, d.masks, cap); add(sum.bit_scans, d.bit_scans, cap);
    add(sum.negations, d.negations, cap); add(sum.encodings, d.encodings, cap);
    sum.maximum_magnitude_bits = std::max(sum.maximum_magnitude_bits, d.maximum_magnitude_bits);
    sum.workspace_bytes = std::max(sum.workspace_bytes, d.workspace_bytes);
    sum.integer_bytes = std::max(sum.integer_bytes, d.integer_bytes);
  }

  std::size_t moment_cap() const {
    return std::min<std::size_t>(budget_.owner.max_moments, 262144);
  }
  std::size_t normal_cap() const {
    return std::min<std::size_t>(budget_.max_normal_functions, 524288);
  }
  R maximum_condition = 1;

 private:
  std::size_t conversion_cap() const {
    return std::min<std::size_t>(budget_.max_conversions, 1048576);
  }
  std::size_t coefficient_cap() const {
    return std::min<std::size_t>(budget_.max_coefficient_terms, 524288);
  }
  std::size_t accumulation_cap() const {
    return std::min<std::size_t>(budget_.max_accumulation_terms, 524288);
  }
  std::size_t selection_cap() const {
    return std::min<std::size_t>(budget_.max_selection_checks, 262144);
  }
  std::size_t product_cap() const {
    return std::min<std::size_t>(budget_.max_coefficient_products, 262144);
  }
  std::size_t function_cap() const {
    return std::min<std::size_t>(budget_.max_local_functions, 262144);
  }
  const StockCubic &curve_;
  const CubicExpectationBudget &budget_;
  CubicExpectationWork &work_;
  std::size_t frame_ = 0;
  const R limit_{Digits == 100 ? "1e70" : "1e170"};
};

template <unsigned Digits>
CubicContribution tail_contribution(const CubicAffineTail &tail,
    const CubicExpectationRequest &request, Context<Digits> &math,
    const CubicExpectationBudget &budget, CubicExpectationWork &work) {
  using R = Real<Digits>;
  math.reserve_owner_counts();
  ++work.tail_attempts;
  const R a = math.convert(tail.lower), v = math.convert(tail.value);
  const R m = math.convert(tail.slope), s = math.convert(request.stock);
  const R t = math.convert(request.dt), sigma = math.convert(request.sigma);
  const R r = math.convert(request.rate), q = math.convert(request.yield);
  const R variance = sigma*sigma*t;
  R sd = 0, log_ratio = 0, sqrt_two = 0;
  if (a > 0) {
    math.positive(variance, "cubic tail variance");
    math.elementary(); sd = sqrt(variance);
    math.elementary(); log_ratio = log(a/s);
    math.elementary(); sqrt_two = sqrt(R(2));
    if (!finite(log_ratio)) throw Refusal{ErrorCode::OutOfRange, "cubic tail logarithm"};
  }
  std::array<R, 2> moments;
  for (std::size_t k = 0; k < moments.size(); ++k) {
    require_room(work.tail_moment_attempts, 1, std::min<std::size_t>(budget.max_tail_moments, 2));
    require_room(work.moment_attempts, 1, math.moment_cap());
    ++work.tail_moment_attempts; ++work.moment_attempts;
    const R exponent = k == 0 ? R(-r*t) : R(-q*t);
    work.tail.prefactor_exponents[k] = math.encode(exponent);
    R probability = 1;
    if (a > 0) {
      const R z = (log_ratio-(r-q-sigma*sigma/2)*t-R(k)*variance)/sd;
      work.tail.thresholds[k] = math.encode(z);
      if (!finite(z) || abs(z) > 256)
        throw Refusal{ErrorCode::OutOfRange, "cubic tail standardized threshold"};
      add(work.normal_functions, 1, math.normal_cap());
      probability = boost::math::erfc(z/sqrt_two)/2;
      math.positive(probability, "cubic tail survival underflow/overflow");
    }
    moments[k] = math.exponential(exponent)*probability;
    if (k == 1) moments[k] *= s;
    math.positive(moments[k], "cubic tail moment underflow/overflow");
    work.tail.moments[k] = math.encode(moments[k]);
  }
  const R a_mass = a*moments[0];
  const R excess = a == 0 ? moments[1] : R(moments[1]-a_mass);
  math.coefficient(a == 0 ? 1 : 2);
  const R excess_abs = a == 0 ? R(abs(moments[1])) : R(abs(moments[1])+abs(a_mass));
  work.tail.excess = math.encode(excess);
  work.tail.excess_absolute_sum = math.encode(excess_abs);
  math.positive(excess, "cubic tail rounded nonpositive excess");
  work.tail.excess_condition = math.encode(math.condition(excess, excess_abs));
  const R value = v*moments[0]+m*excess;
  // Keep cancellation inside the excess visible in the full-curve ledger too.
  const R value_abs = abs(v*moments[0])+abs(m)*excess_abs;
  const R delta = m*moments[1]/s;
  const R delta_abs = abs(delta);
  math.accumulate(3);
  math.condition(value, value_abs); math.condition(delta, delta_abs);
  work.tail.value_absolute_sum = math.encode(value_abs);
  work.tail.delta_absolute_sum = math.encode(delta_abs);
  CubicContribution result{math.encode(value), math.encode(delta), math.encode(value_abs),
      math.encode(delta_abs), math.encode(math.maximum_condition)};
  ++work.tail_completions;
  return result;
}

template <unsigned Digits>
Result<CubicExpectation> expectation(const StockCubic &curve,
    const CubicExpectationRequest &request, WeightPrecision precision,
    const CubicExpectationBudget &budget, CubicExpectationWork &work) {
  using R = Real<Digits>;
  const bool stochastic = request.dt > 0 && request.stock > 0 && request.sigma > 0;
  const auto frame = Context<Digits>::reserve(curve, budget, work, stochastic);
  Context<Digits> math(curve, budget, work, frame);
  if (!stochastic) {
    work.phase = CubicExpectationPhase::Point;
    work.route = request.dt == 0 ? CubicExpectationRoute::ZeroTime :
        (request.stock == 0 ? CubicExpectationRoute::ZeroStock :
                             CubicExpectationRoute::ZeroVolatility);
    // Decimal point lookup can visit every stored knot; reserve its worst
    // conversion count before calling the existing transactional helper.
    math.reserve_owner_counts(128+2*curve.cells().size());
    auto child_budget = math.child_budget();
    R value_discount = 1, delta_discount = 1;
    Result<CubicPoint> point = Err(ErrorCode::Unavailable, "cubic point not evaluated");
    if (request.dt == 0) {
      point = cubic_point(curve, request.stock, precision, child_budget, work.last_cell);
    } else {
      const R dt = math.convert(request.dt), r = math.convert(request.rate);
      const R q = math.convert(request.yield);
      value_discount = math.exponential(R(-r*dt));
      delta_discount = math.exponential(R(-q*dt));
      if (request.stock == 0 || request.rate == request.yield) {
        point = cubic_point(curve, request.stock, precision, child_budget, work.last_cell);
      } else {
        const R x = math.convert(request.stock)*math.exponential(R((r-q)*dt));
        math.positive(x, "cubic deterministic transported stock");
        point = detail::cubic_decimal_point(curve, math.encode(x), precision,
                                           child_budget, work.last_cell);
      }
    }
    math.absorb_child();
    work.finite_attempts = work.last_cell.finite_cells;
    if (!point) return Err(point.error());
    work.finite_completions = work.finite_attempts;
    const R value = value_discount*math.convert(point->value);
    const R right = delta_discount*math.convert(point->right_derivative);
    const R left = point->left_derivative.empty() ? R(0) :
        R(delta_discount*math.convert(point->left_derivative));
    const R value_abs = value_discount*math.convert(point->value_absolute_sum);
    const R delta_abs = delta_discount*math.convert(point->derivative_absolute_sum);
    math.condition(value, value_abs);
    // The point's derivative ledger is the maximum across two one-sided
    // answers, not a signed sum between those answers.
    math.condition(R(1), math.convert(point->maximum_condition));
    if (!finite(left) || !finite(right) || !finite(delta_abs))
      throw Refusal{ErrorCode::OutOfRange, "cubic transported derivative"};
    work.phase = CubicExpectationPhase::Output;
    return CubicExpectation{math.encode(value), point->left_derivative.empty() ?
        std::string{} : math.encode(left), math.encode(right), point->slope_kind,
        math.encode(value_abs), math.encode(delta_abs), math.encode(math.maximum_condition)};
  }

  work.route = CubicExpectationRoute::Stochastic;
  R value = 0, delta = 0, value_abs = 0, delta_abs = 0;
  auto combine = [&](const CubicContribution &piece) {
    const R part_value = math.convert(piece.value), part_delta = math.convert(piece.delta);
    const R part_value_abs = math.convert(piece.value_absolute_sum);
    const R part_delta_abs = math.convert(piece.delta_absolute_sum);
    math.condition(R(1), math.convert(piece.maximum_condition));
    math.accumulate(4);
    value += part_value; delta += part_delta;
    value_abs += part_value_abs; delta_abs += part_delta_abs;
  };
  for (std::size_t i = 0; i < curve.cells().size(); ++i) {
    work.phase = CubicExpectationPhase::FiniteCell;
    work.component_index = i;
    math.reserve_owner_counts();
    auto child_budget = math.child_budget();
    require_room(work.finite_attempts, 1,
        std::min<std::size_t>(budget.owner.max_finite_cells, 65536));
    ++work.finite_attempts;
    const auto &cell = curve.cells()[i];
    const WeightRequest r{cell.lower, cell.upper, request.stock, request.dt,
        request.sigma, request.rate, request.yield};
    auto piece = cubic_cell_contribution(curve, i, r, precision, child_budget, work.last_cell);
    math.absorb_child();
    if (!piece) return Err(piece.error());
    ++work.finite_completions;
    combine(*piece);
  }
  work.phase = CubicExpectationPhase::Tail;
  work.component_index = curve.cells().size();
  combine(tail_contribution(curve.tail(), request, math, budget, work));
  math.condition(value, value_abs); math.condition(delta, delta_abs);
  work.phase = CubicExpectationPhase::Output;
  const auto delta_text = math.encode(delta);
  return CubicExpectation{math.encode(value), delta_text, delta_text,
      CubicSlopeKind::Smooth, math.encode(value_abs), math.encode(delta_abs),
      math.encode(math.maximum_condition)};
}
} // namespace

Result<CubicExpectation> cubic_expectation(const StockCubic &curve,
    const CubicExpectationRequest &request, WeightPrecision precision,
    const CubicExpectationBudget &budget, CubicExpectationWork &work) {
  work = {};
  if (!std::isfinite(request.stock) || !std::isfinite(request.dt) ||
      !std::isfinite(request.sigma) || !std::isfinite(request.rate) ||
      !std::isfinite(request.yield) || request.stock < 0 || request.dt < 0 || request.sigma < 0)
    return Err(ErrorCode::InvalidArgument, "cubic expectation request");
  try {
    switch (precision) {
      case WeightPrecision::Decimal100:
        return expectation<100>(curve, request, precision, budget, work);
      case WeightPrecision::Decimal200:
        return expectation<200>(curve, request, precision, budget, work);
    }
    return Err(ErrorCode::InvalidArgument, "cubic expectation precision");
  } catch (const Refusal &error) {
    return Err(error.code, error.message);
  } catch (const std::bad_alloc &) {
    return Err(ErrorCode::Unavailable, "cubic expectation allocation");
  } catch (const std::exception &) {
    return Err(ErrorCode::OutOfRange, "cubic expectation arithmetic exception");
  }
}
} // namespace atx::vol::cash_gaussian
