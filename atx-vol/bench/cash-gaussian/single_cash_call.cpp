#include "single_cash_call.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <exception>
#include <ios>
#include <limits>
#include <new>
#include <utility>

#include <boost/math/special_functions/erf.hpp>
#include <boost/multiprecision/cpp_bin_float.hpp>

namespace atx::vol::cash_gaussian {
namespace {
using atx::core::Err;
constexpr std::size_t max_knots = 8192, max_bytes = 64 * 1024 * 1024;
struct Refusal { ErrorCode code; const char *message; };
template <unsigned Digits>
using Real = boost::multiprecision::number<boost::multiprecision::cpp_bin_float<Digits>>;

template <class R>
bool finite(const R &x) {
  return x == x && x != std::numeric_limits<R>::infinity() &&
      x != -std::numeric_limits<R>::infinity();
}

template <unsigned Digits>
bool reserve(std::size_t endpoints, const SingleCashCallBudget &budget,
             SingleCashCallWork &work) {
  // Bounds are checked before these products; no caller-sized unbounded storage.
  if (endpoints > max_knots || endpoints > budget.max_knots ||
      endpoints > budget.owner.max_knots || 3*endpoints+1 >
          std::min<std::size_t>(budget.max_endpoint_projections, 65536)) return false;
  // Two tuple vectors coexist during StockCubic::make. String capacity covers
  // ten endpoint strings and four strings per saved boundary bracket. Scalar
  // scratch and owner construction scratch coexist and are charged separately.
  const auto bytes = endpoints*(sizeof(CashCallEndpoint)+12*(Digits+32)+128) +
      290*(sizeof(CashRootStep)+4*(Digits+32)+128) +
      2*endpoints*sizeof(HermiteCell) + 256*sizeof(Real<Digits>)+32768;
  if (bytes > std::min(budget.max_workspace_bytes, max_bytes)) return false;
  work.workspace_bytes = bytes;
  return true;
}

template <unsigned Digits>
class Builder {
 public:
  using R = Real<Digits>;
  Builder(const SingleCashCallModel &model, const SingleCashCallBudget &budget,
          SingleCashCallWork &work, SingleCashCallCurve &output)
      : m_(model), budget_(budget), work_(work), out_(output), k_(model.strike),
        tau_(R(model.maturity)-R(model.dividend_time)), sigma_(model.sigma),
        rate_(model.rate), cash_(model.dividend) {
    if (model.dividend >= model.strike) return; // No post-event continuation exists.
    ++work_.elementary_functions;
    root_tau_ = sqrt(tau_);
    a_ = sigma_*root_tau_;
    if (!finite(a_) || a_ <= 0 || abs(rate_*tau_) > 32768)
      throw Refusal{ErrorCode::OutOfRange, "cash call Black parameter domain"};
    ++work_.elementary_functions;
    discount_ = exp(-rate_*tau_);
    ++work_.elementary_functions;
    inverse_sqrt_two_ = 1/sqrt(R(2));
  }

  std::string encode(const R &x) const { return x.str(Digits, std::ios_base::scientific); }

  std::pair<R, R> black(const R &y) {
    if (work_.black_evaluations >= 8490)
      throw Refusal{ErrorCode::Unavailable, "cash call function budget"};
    ++work_.black_evaluations;
    if (y == 0) return {R(0), R(0)};
    if (y < 0) throw Refusal{ErrorCode::OutOfRange, "cash call negative post-event stock"};
    ++work_.elementary_functions;
    const R d1 = (log(y/k_)+rate_*tau_)/a_+a_/2, d2 = d1-a_;
    if (!finite(d1) || !finite(d2) || abs(d1) > 256 || abs(d2) > 256)
      throw Refusal{ErrorCode::OutOfRange, "cash call normal domain"};
    const R delta = normal(d1), value = y*delta-k_*discount_*normal(d2);
    if (!finite(value) || value <= 0 || value > y || delta <= 0 || delta > 1)
      throw Refusal{ErrorCode::OutOfRange, "cash call Black arithmetic"};
    return {value, delta};
  }

  double boundary() {
    work_.phase = CashCallPhase::Boundary;
    if (cash_ <= k_*(1-discount_))
      throw Refusal{ErrorCode::InvalidArgument, "cash call has no finite exercise boundary"};
    R lo = k_, hi = 2*k_, fl = equation(lo), fh = equation(hi);
    save(lo, hi, fl, fh);
    const auto cap = std::min<std::size_t>(budget_.max_bracket_expansions, 32);
    for (; fh > 0 && work_.bracket_expansions < cap;) {
      ++work_.bracket_expansions;
      hi *= 2;
      fh = equation(hi);
      save(lo, hi, fl, fh);
    }
    if (fl <= 0 || fh > 0)
      throw Refusal{ErrorCode::Unavailable, "cash call bracket budget"};
    const auto iterations = std::min<std::size_t>(budget_.max_bisections, 256);
    for (; hi-lo > ldexp(hi, -192) && work_.bisections < iterations;) {
      ++work_.bisections;
      const R mid = (lo+hi)/2, fm = equation(mid);
      if (mid == lo || mid == hi)
        throw Refusal{ErrorCode::OutOfRange, "cash call root stagnation"};
      if (fm > 0) { lo = mid; fl = fm; }
      else { hi = mid; fh = fm; }
      save(lo, hi, fl, fh);
    }
    if (hi-lo > ldexp(hi, -192))
      throw Refusal{ErrorCode::Unavailable, "cash call bisection budget"};
    out_.root_lower = encode(lo); out_.root_upper = encode(hi);
    double projected = hi.template convert_to<double>();
    if (R(projected) < hi)
      projected = std::nextafter(projected, std::numeric_limits<double>::infinity());
    if (!std::isfinite(projected) || R(projected) < hi)
      throw Refusal{ErrorCode::OutOfRange, "cash call boundary projection"};
    const R residual = equation(R(projected));
    if (residual > 0)
      throw Refusal{ErrorCode::OutOfRange, "cash call projected boundary sign"};
    out_.boundary_residual = encode(residual);
    return projected;
  }

  void endpoint(const R &unrounded, bool boundary, bool analytical) {
    CashCallEndpoint node;
    node.stock = project(unrounded);
    const R x(node.stock.projected);
    if (!out_.endpoints.empty() && node.stock.projected <= out_.endpoints.back().stock.projected)
      throw Refusal{ErrorCode::OutOfRange, "cash call collapsed knots"};
    const auto continuation = analytical || x <= cash_ ? std::pair{R(0), R(0)} : black(x-cash_);
    node.continuation_value = encode(continuation.first);
    node.continuation_delta = encode(continuation.second);
    const R value = boundary ? x-k_ : continuation.first;
    node.value = project(value);
    node.left_slope = project(continuation.second);
    node.right_slope = boundary ? project(R(1)) : node.left_slope;
    out_.endpoints.push_back(std::move(node));
    ++work_.constructed_knots;
  }

  void allowances(bool analytical) {
    work_.phase = CashCallPhase::Allowance;
    R value_bound = 0, delta_bound = 0;
    for (std::size_t i = 1; i < out_.endpoints.size(); ++i) {
      const auto &left = out_.endpoints[i-1], &right = out_.endpoints[i];
      const R h = R(right.stock.projected)-R(left.stock.projected);
      // For the analytical hinge, the left branch is exactly zero at K.
      const R ev0 = R(left.value.projected)-R(left.continuation_value);
      const R ev1 = analytical ? R(0) :
          R(right.value.projected)-R(right.continuation_value);
      const R em0 = R(left.right_slope.projected)-R(left.continuation_delta);
      const R em1 = R(right.left_slope.projected)-R(right.continuation_delta);
      const R vb = std::max(R(abs(ev0)), R(abs(ev1))) +
          4*h*(abs(em0)+abs(em1))/27;
      const R db = 3*(abs(ev0)+abs(ev1))/(2*h)+abs(em0)+abs(em1);
      value_bound = std::max(value_bound, vb); delta_bound = std::max(delta_bound, db);
    }
    const auto &tail = out_.endpoints.back();
    value_bound = std::max(value_bound,
        R(abs(R(tail.value.projected)-(R(out_.boundary)-k_))));
    if (abs(rate_*R(m_.dividend_time)) > 32768)
      throw Refusal{ErrorCode::OutOfRange, "cash call discount domain"};
    ++work_.elementary_functions;
    const R before_discount = exp(-rate_*R(m_.dividend_time));
    R root_value = 0, root_delta = 0;
    if (!analytical) {
      const R lower(out_.root_lower), upper(out_.boundary);
      root_value = before_discount*(upper-lower);
      if (m_.stock > 0 && upper > lower) {
        work_.elementary_functions += 3;
        const R t(m_.dividend_time), a = sigma_*sqrt(t), s(m_.stock);
        const R drift = (rate_+sigma_*sigma_/2)*t;
        const R zl = (log(lower/s)-drift)/a, zu = (log(upper/s)-drift)/a;
        if (abs(zl) > 256 || abs(zu) > 256)
          throw Refusal{ErrorCode::OutOfRange, "cash call root moment domain"};
        root_delta = zu <= 0 ? normal(zu)-normal(zl) : normal(-zl)-normal(-zu);
        if (!finite(root_delta) || root_delta <= 0)
          throw Refusal{ErrorCode::OutOfRange, "cash call root moment arithmetic"};
      }
    }
    value_bound *= before_discount;
    auto &out = out_.allowances;
    out.projection_value = encode(value_bound); out.projection_delta = encode(delta_bound);
    out.root_value = encode(root_value); out.root_delta = encode(root_delta);
    const R combined_value = value_bound+root_value, combined_delta = delta_bound+root_delta;
    out.combined_value = encode(combined_value); out.combined_delta = encode(combined_delta);
    if (!finite(combined_value) || !finite(combined_delta) ||
        combined_value > R("1e-8") || combined_delta > R("1e-8"))
      throw Refusal{ErrorCode::OutOfRange, "cash call root/projection allowance"};
  }

 private:
  R normal(const R &z) {
    ++work_.normal_functions;
    return boost::math::erfc(-z*inverse_sqrt_two_)/2;
  }
  R equation(const R &x) { return black(x-cash_).first-(x-k_); }
  void save(const R &lo, const R &hi, const R &fl, const R &fh) {
    if (work_.root_steps.size() == 290)
      throw Refusal{ErrorCode::Unavailable, "cash call root record budget"};
    work_.root_steps.push_back({encode(lo), encode(hi), encode(fl), encode(fh)});
  }
  CashEndpointProjection project(const R &value) {
    if (work_.endpoint_projections >=
        std::min<std::size_t>(budget_.max_endpoint_projections, 65536))
      throw Refusal{ErrorCode::Unavailable, "cash call projection budget"};
    ++work_.endpoint_projections;
    const double projected = value.template convert_to<double>();
    if (!std::isfinite(projected))
      throw Refusal{ErrorCode::OutOfRange, "cash call endpoint projection"};
    return {encode(value), encode(R(projected)-value), projected};
  }
  const SingleCashCallModel &m_;
  const SingleCashCallBudget &budget_;
  SingleCashCallWork &work_;
  SingleCashCallCurve &out_;
  R k_, tau_, sigma_, rate_, cash_, root_tau_, a_, discount_, inverse_sqrt_two_;
};

template <unsigned Digits>
Result<SingleCashCallCurve> build(const SingleCashCallModel &m, std::size_t n,
    WeightPrecision precision, const SingleCashCallBudget &budget, SingleCashCallWork &work) {
  const bool analytical = m.dividend >= m.strike;
  const std::size_t endpoints = analytical ? 2u : n+2;
  if (!reserve<Digits>(endpoints, budget, work))
    return Err(ErrorCode::Unavailable, "cash call setup reservation");
  SingleCashCallCurve out;
  out.model = m; out.precision = precision; out.continuation_cells = analytical ? 0u : n;
  out.endpoints.reserve(endpoints); work.root_steps.reserve(290);
  Builder<Digits> math(m, budget, work, out);
  using R = typename Builder<Digits>::R;
  work.route = analytical ? CashCallRoute::ExerciseAtEvent : CashCallRoute::FiniteBoundary;
  out.boundary = analytical ? m.strike : math.boundary();
  work.phase = CashCallPhase::Endpoints;
  math.endpoint(R(0), false, analytical);
  if (!analytical) {
    math.endpoint(R(m.dividend), false, false);
    for (std::size_t j = 1; j < n; ++j) {
      math.endpoint(R(m.dividend)+(R(out.boundary)-R(m.dividend))*R(j)/R(n), false, false);
    }
  }
  math.endpoint(R(out.boundary), true, analytical);
  std::vector<HermiteCell> cells;
  cells.reserve(endpoints-1);
  for (std::size_t i = 1; i < endpoints; ++i) {
    const auto &a = out.endpoints[i-1], &b = out.endpoints[i];
    cells.push_back({a.stock.projected, b.stock.projected, a.value.projected, b.value.projected,
        a.right_slope.projected, b.left_slope.projected});
  }
  work.phase = CashCallPhase::Owner;
  const auto &last = out.endpoints.back();
  auto owner_budget = budget.owner;
  owner_budget.max_workspace_bytes = std::min(owner_budget.max_workspace_bytes,
      std::min(budget.max_workspace_bytes, max_bytes));
  auto curve = StockCubic::make(cells,
      {last.stock.projected, last.value.projected, last.right_slope.projected},
      owner_budget, work.owner);
  if (!curve) return Err(curve.error());
  out.curve = std::move(*curve);
  math.allowances(analytical);
  work.phase = CashCallPhase::Complete;
  return out;
}
} // namespace

Result<SingleCashCallCurve> build_single_cash_call(const SingleCashCallModel &m,
    std::size_t n, WeightPrecision precision, const SingleCashCallBudget &budget,
    SingleCashCallWork &work) {
  work = {};
  const std::array fields{m.stock, m.strike, m.maturity, m.sigma, m.rate, m.yield,
      m.dividend_time, m.dividend};
  if (!std::all_of(fields.begin(), fields.end(), [](double x) { return std::isfinite(x); }) ||
      m.stock < 0 || m.strike <= 0 || m.maturity <= 0 || m.sigma <= 0 || m.rate < 0 ||
      m.yield != 0 || m.dividend_time <= 0 || m.dividend_time >= m.maturity ||
      m.dividend <= 0 || n == 0 || n > max_knots-2)
    return Err(ErrorCode::InvalidArgument, "cash call model or grid");
  try {
    switch (precision) {
      case WeightPrecision::Decimal100: return build<100>(m, n, precision, budget, work);
      case WeightPrecision::Decimal200: return build<200>(m, n, precision, budget, work);
    }
    return Err(ErrorCode::InvalidArgument, "cash call precision");
  } catch (const Refusal &error) {
    return Err(error.code, error.message);
  } catch (const std::bad_alloc &) {
    return Err(ErrorCode::Unavailable, "cash call allocation");
  } catch (const std::exception &) {
    return Err(ErrorCode::OutOfRange, "cash call arithmetic exception");
  }
}
} // namespace atx::vol::cash_gaussian
