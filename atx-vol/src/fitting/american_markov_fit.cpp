#include "atx/vol/api/fitting/american_markov_fit.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>
#include <utility>

#include "atx/vol/api/pricing/american_markov_batch.hpp"
#include "fitting/american_markov_fit_controls.hpp"
#include "fitting/american_markov_time_basis.hpp"

namespace atx::vol {
namespace {
using atx::core::Err;
using atx::core::Ok;

constexpr double kMaxWeight = 1.0e6;
bool positive(double x) noexcept { return std::isfinite(x) && x > 0.0; }
bool nonnegative(double x) noexcept { return std::isfinite(x) && x >= 0.0; }
} // namespace

namespace detail {
Status validate_markov_fit_search(const AmericanMarkovFitConfig &c) {
  const bool scaled = c.evaluations_per_parameter != 0;
  if ((c.search != AmericanMarkovFitSearch::CoordinatePattern &&
       c.search != AmericanMarkovFitSearch::LevenbergMarquardt) ||
      c.evaluations_per_parameter > kAmericanMarkovFitMaxEvaluations ||
      (scaled && (c.max_scaled_evaluations < c.max_evaluations ||
                  c.max_scaled_evaluations > kAmericanMarkovFitMaxEvaluations)) ||
      (!scaled && c.max_scaled_evaluations != 0)) {
    return Err(ErrorCode::InvalidArgument, "Markov fit: invalid search or evaluation scaling");
  }
  if (!nonnegative(c.objective_tolerance) || c.objective_tolerance > 1.0 ||
      !nonnegative(c.gradient_tolerance) || c.gradient_tolerance > 1.0e6 ||
      !positive(c.jacobian_log_step) || c.jacobian_log_step < 1.0e-8 ||
      c.jacobian_log_step > 0.5 || !positive(c.initial_damping) ||
      c.initial_damping < 1.0e-12 || c.initial_damping > 1.0e8 ||
      !positive(c.max_damping) || c.max_damping < c.initial_damping || c.max_damping > 1.0e16) {
    return Err(ErrorCode::InvalidArgument, "Markov fit: invalid Levenberg-Marquardt controls");
  }
  return Ok();
}
} // namespace detail

std::size_t american_markov_fit_evaluation_budget(const AmericanMarkovFitConfig &config,
                                                  std::size_t parameters) noexcept {
  if (config.evaluations_per_parameter == 0) return config.max_evaluations;
  const auto cap = config.max_scaled_evaluations;
  const auto scaled = parameters > cap / config.evaluations_per_parameter
      ? cap : parameters * config.evaluations_per_parameter;
  return std::clamp(scaled, std::min(config.max_evaluations, cap), cap);
}

namespace {

Status validate(const AmericanMarkovFitMarket &m, std::span<const AmericanMarkovFitQuote> quotes,
                const AmericanMarkovFitConfig &c) {
  if (!positive(m.spot) || m.intervals.empty() ||
      m.intervals.size() > kAmericanMarkovMaxTransitions || quotes.empty() ||
      c.max_parameters == 0 || c.max_parameters > kAmericanMarkovFitMaxParameters ||
      c.max_quotes == 0 || c.max_quotes > kAmericanMarkovFitMaxQuotes ||
      quotes.size() > c.max_quotes || c.max_evaluations == 0 ||
      c.max_evaluations > kAmericanMarkovFitMaxEvaluations || c.max_sweeps == 0 ||
      c.max_sweeps > kAmericanMarkovFitMaxSweeps || c.batch_options == 0 ||
      c.batch_options > kAmericanMarkovBatchMaxOptions || c.max_batch_values == 0 ||
      c.max_batch_values > kAmericanMarkovBatchMaxValues || c.max_rollback_work == 0 ||
      c.max_rollback_work > kAmericanMarkovFitMaxRollbackWork ||
      (c.time_shape != AmericanMarkovFitTimeShape::Shared &&
       c.time_shape != AmericanMarkovFitTimeShape::PerVarianceInterval &&
       c.time_shape != AmericanMarkovFitTimeShape::VarianceTimeKnots)) {
    return Err(ErrorCode::InvalidArgument, "Markov fit: invalid input or resource configuration");
  }
  if (!positive(c.min_sigma) || !positive(c.max_sigma) || c.min_sigma >= c.max_sigma ||
      c.min_sigma < 1.0e-6 || c.max_sigma > 100.0 ||
      (c.initial_sigmas.empty() && (!positive(c.initial_sigma) ||
          c.initial_sigma < c.min_sigma || c.initial_sigma > c.max_sigma)) ||
      !positive(c.price_scale_floor) || c.price_scale_floor < 1.0e-12 ||
      !positive(c.initial_log_step) || c.initial_log_step > 10.0 ||
      !positive(c.min_log_step) || c.min_log_step > c.initial_log_step) {
    return Err(ErrorCode::InvalidArgument, "Markov fit: invalid sigma, scale or search bounds");
  }
  for (const double weight : {c.midpoint_weight, c.prior_weight, c.strike_roughness_weight,
                              c.time_roughness_weight}) {
    if (!nonnegative(weight) || weight > kMaxWeight) {
      return Err(ErrorCode::InvalidArgument, "Markov fit: invalid objective weight");
    }
  }
  if (c.log_state_knots.size() > c.max_parameters ||
      c.log_state_knots.size() > kAmericanMarkovMaxLocalVolNodes) {
    return Err(ErrorCode::OutOfRange, "Markov fit: local volatility knot limit exceeded");
  }
  double previous = -std::numeric_limits<double>::infinity();
  for (const double knot : c.log_state_knots) {
    if (!std::isfinite(knot) || std::abs(knot) > kAmericanMarkovMaxLocalVolLogState ||
        !(knot > previous)) {
      return Err(ErrorCode::InvalidArgument, "Markov fit: invalid local volatility knot grid");
    }
    previous = knot;
  }
  for (const auto &q : quotes) {
    if (q.expiry_index >= m.intervals.size() || !positive(q.strike) ||
        (q.side != Side::Call && q.side != Side::Put) || !nonnegative(q.bid) ||
        !positive(q.ask) || q.bid > q.ask || !positive(q.weight) || q.weight > kMaxWeight) {
      return Err(ErrorCode::InvalidArgument, "Markov fit: invalid raw quote");
    }
  }
  ATX_TRY_VOID(detail::validate_markov_fit_search(c));
  return detail::validate_markov_time_knots(c);
}

struct Layout {
  std::vector<double> knots;
  std::vector<double> time_knots;
  std::vector<AmericanMarkovFitTimeWeight> time_weights;
  std::optional<AmericanMarkovFitTimeBasisDiagnostics> time_basis;
  std::vector<double> initial;
  std::vector<double> prior;
  std::vector<std::size_t> order;
  std::size_t time_groups = 0;
};

// One owned row per ORIGINAL interval, including paused variance. The compiler
// applies the same schedule predicates; checking here keeps every represented
// midpoint finite before any knot interpolation.
Status map_time_weights(const AmericanMarkovFitMarket &m, const AmericanMarkovFitConfig &c,
                        Layout &out) {
  const bool time_knots = c.time_shape == AmericanMarkovFitTimeShape::VarianceTimeKnots;
  if (time_knots) out.time_groups = out.time_knots.size();
  out.time_weights.reserve(m.intervals.size());
  double calendar = 0.0;
  double variance = 0.0;
  for (const auto &interval : m.intervals) {
    if (!positive(interval.calendar_end) || interval.calendar_end <= calendar ||
        !nonnegative(interval.variance_end) || interval.variance_end < variance ||
        !std::isfinite(interval.rate) || !std::isfinite(interval.continuous_yield) ||
        !nonnegative(interval.cash_dividend_at_end)) {
      return Err(ErrorCode::InvalidArgument, "Markov fit: invalid common interval schedule");
    }
    const bool active = interval.variance_end > variance;
    const double midpoint = std::midpoint(variance, interval.variance_end);
    AmericanMarkovFitTimeWeight mapping{0, 0, 0.0, midpoint, active};
    if (time_knots) {
      ATX_TRY(mapping, detail::markov_time_weight(midpoint, active, out.time_knots));
    } else if (active) {
      if (c.time_shape == AmericanMarkovFitTimeShape::PerVarianceInterval) {
        mapping.left_group = mapping.right_group = out.time_groups++;
      } else {
        out.time_groups = 1;
      }
    }
    out.time_weights.push_back(mapping);
    calendar = interval.calendar_end;
    variance = interval.variance_end;
  }
  return Ok();
}

// VarianceTimeKnots only. Interval-times-state scratch is preflighted against
// the model's own volatility budget. The represented interval log-sigma basis
// must pass its fixed numerical admission over the final quoted prefix; the
// objective's prior/roughness terms are never consulted, so they cannot rescue it.
Status admit_time_knots(const AmericanMarkovFitMarket &m,
                        std::span<const AmericanMarkovFitQuote> quotes, Layout &out) {
  const auto capacity = m.model_config.max_stored_volatility_values;
  if (capacity == 0 || capacity > kAmericanMarkovMaxStoredVolatilityValues ||
      m.intervals.size() > capacity / out.knots.size()) {
    return Err(ErrorCode::OutOfRange,
               "Markov fit: interval sigma scratch exceeds model capacity");
  }
  std::size_t last_quoted_interval = 0;
  for (const auto &quote : quotes) {
    last_quoted_interval = std::max(last_quoted_interval, quote.expiry_index);
  }
  ATX_TRY(auto basis, detail::diagnose_markov_time_basis(
      std::span<const AmericanMarkovFitTimeWeight>(out.time_weights)
          .first(last_quoted_interval + 1), out.time_groups));
  out.time_basis = std::move(basis);
  return Ok();
}

Result<Layout> layout(const AmericanMarkovFitMarket &m,
                      std::span<const AmericanMarkovFitQuote> quotes,
                      const AmericanMarkovFitConfig &c) {
  Layout out;
  out.knots.assign(c.log_state_knots.begin(), c.log_state_knots.end());
  if (out.knots.empty()) out.knots.push_back(0.0);
  out.time_knots.assign(c.variance_time_knots.begin(), c.variance_time_knots.end());
  ATX_TRY_VOID(map_time_weights(m, c, out));
  if (out.time_groups == 0 || out.time_groups > c.max_parameters / out.knots.size()) {
    return Err(ErrorCode::OutOfRange, "Markov fit: unidentifiable variance or parameter limit");
  }
  const auto count = out.time_groups * out.knots.size();
  if (c.time_shape == AmericanMarkovFitTimeShape::VarianceTimeKnots) {
    ATX_TRY_VOID(admit_time_knots(m, quotes, out));
  }
  const auto parameters = [&](std::span<const double> values, std::vector<double> &target) {
    if (!values.empty() && values.size() != count) return false;
    if (!values.empty()) target.assign(values.begin(), values.end());
    return std::all_of(target.begin(), target.end(), [&](double x) {
      return positive(x) && x >= c.min_sigma && x <= c.max_sigma;
    });
  };
  out.initial.assign(count, c.initial_sigma);
  if (!parameters(c.initial_sigmas, out.initial)) {
    return Err(ErrorCode::InvalidArgument, "Markov fit: invalid initial sigma vector");
  }
  out.prior = out.initial;
  if (!parameters(c.prior_sigmas, out.prior)) {
    return Err(ErrorCode::InvalidArgument, "Markov fit: invalid prior sigma vector");
  }
  out.order.resize(quotes.size());
  std::iota(out.order.begin(), out.order.end(), std::size_t{});
  const auto less = [&](std::size_t a, std::size_t b) {
    const auto &x = quotes[a];
    const auto &y = quotes[b];
    if (x.expiry_index != y.expiry_index) return x.expiry_index < y.expiry_index;
    if (x.side != y.side) return x.side < y.side;
    return x.strike < y.strike;
  };
  std::sort(out.order.begin(), out.order.end(), less);
  for (std::size_t i = 1; i < out.order.size(); ++i) {
    if (!less(out.order[i - 1], out.order[i])) {
      return Err(ErrorCode::InvalidArgument, "Markov fit: duplicate expiry/side/strike quote");
    }
  }
  return Ok(std::move(out));
}

struct Objective {
  double total = 0.0;
  double interval = 0.0;
  double midpoint = 0.0;
  double regularization = 0.0;
  double max_error = 0.0;
  double mean_error = 0.0;
  std::size_t inside = 0;
};

Objective objective(std::span<const AmericanMarkovFitQuote> quotes,
                    std::span<const double> prices, std::span<const double> sigmas,
                    const Layout &l, const AmericanMarkovFitConfig &c) {
  Objective out;
  double weights = 0.0;
  for (const auto &q : quotes) weights += q.weight;
  const double root_weights = std::sqrt(weights);
  const double root_midpoint = std::sqrt(c.midpoint_weight);
  for (std::size_t i = 0; i < quotes.size(); ++i) {
    const auto &q = quotes[i];
    // Normalize in square-root space: weight/sum can underflow even when
    // its weighted squared residual is material. Scale distance before its
    // division as well, avoiding an overflowing unweighted residual.
    const double root_weight = std::sqrt(q.weight) / root_weights;
    const double distance = std::max({q.bid - prices[i], prices[i] - q.ask, 0.0});
    const double scale = std::max(0.5 * (q.ask - q.bid), c.price_scale_floor);
    const double residual = (root_weight * distance) / scale;
    out.interval += residual * residual;
    if (q.bid > 0.0 && c.midpoint_weight > 0.0) {
      const double center = 0.5 * q.bid + 0.5 * q.ask;
      const double tie = root_midpoint * ((root_weight * (prices[i] - center)) / scale);
      out.midpoint += tie * tie;
    }
    out.max_error = std::max(out.max_error, distance);
    out.mean_error += distance / static_cast<double>(quotes.size());
    out.inside += distance == 0.0 ? 1U : 0U;
  }
  double prior = 0.0;
  double strike = 0.0;
  double time = 0.0;
  const std::size_t knots = l.knots.size();
  for (std::size_t i = 0; i < sigmas.size(); ++i) {
    const double log_sigma = std::log(sigmas[i]);
    const double d = log_sigma - std::log(l.prior[i]);
    prior += d * d;
    if (i % knots != 0) {
      const double s = log_sigma - std::log(sigmas[i - 1]);
      strike += s * s;
    }
    if (i >= knots) {
      const double t = log_sigma - std::log(sigmas[i - knots]);
      time += t * t;
    }
  }
  out.regularization = (c.prior_weight * prior + c.strike_roughness_weight * strike +
                        c.time_roughness_weight * time) / static_cast<double>(sigmas.size());
  out.total = out.interval + out.midpoint + out.regularization;
  return out;
}

// Log-sigma interpolation at one ORIGINAL interval's represented variance
// midpoint; the caller freezes the sampled value for that whole interval.
double interpolated_sigma(double left, double right, double right_weight) noexcept {
  // Preserve exact controls at knots/tails and an exactly flat term structure.
  // Avoid exp(log(sigma)) perturbing Shared-equivalent rows.
  if (left == right) return left;
  // Exact log-linear interpolation lies between its two controls. The bracket
  // only removes exp/log rounding, so in-bounds controls cannot yield a spurious
  // out-of-bounds node. Interpolation weights themselves are never clipped.
  return std::clamp(std::exp(std::lerp(std::log(left), std::log(right), right_weight)),
                    std::min(left, right), std::max(left, right));
}

// A reusable scratch owner for all trial models. Candidate models own their
// sampled metadata, so none of these interval spans escapes the evaluation.
struct Evaluator {
  const AmericanMarkovFitMarket &market;
  std::span<const AmericanMarkovFitQuote> quotes;
  const AmericanMarkovFitConfig &config;
  const Layout &shape;
  std::vector<AmericanMarkovInterval> intervals;
  std::vector<AmericanMarkovLocalVolNode> nodes;
  AmericanMarkovBatchWorkspace workspace;
  std::vector<double> strikes;
  std::vector<double> values;
  // Honest work ledger across every evaluation of this fit (see diagnostics).
  std::size_t rollback_work = 0;
  std::size_t priced_quotes = 0;
  std::size_t reused_quote_prices = 0;

  Evaluator(const AmericanMarkovFitMarket &m, std::span<const AmericanMarkovFitQuote> q,
            const AmericanMarkovFitConfig &c, const Layout &l)
      : market(m), quotes(q), config(c), shape(l), intervals(m.intervals.size()),
        nodes(c.time_shape == AmericanMarkovFitTimeShape::VarianceTimeKnots
                  ? m.intervals.size() * l.knots.size() : l.initial.size()),
        strikes(std::min(c.batch_options, q.size())),
        values(strikes.size()) {}

  Result<AmericanMarkovModel> compile(std::span<const double> sigmas) {
    const auto knot_count = shape.knots.size();
    const bool time_knots = config.time_shape == AmericanMarkovFitTimeShape::VarianceTimeKnots;
    if (!time_knots) {
      for (std::size_t i = 0; i < sigmas.size(); ++i) {
        nodes[i] = {shape.knots[i % knot_count], sigmas[i]};
      }
    }
    for (std::size_t i = 0; i < intervals.size(); ++i) {
      const auto &in = market.intervals[i];
      const auto &mapping = shape.time_weights[i];
      const auto offset = time_knots ? i * knot_count : mapping.left_group * knot_count;
      if (time_knots) {
        for (std::size_t j = 0; j < knot_count; ++j) {
          const double sigma = interpolated_sigma(sigmas[mapping.left_group * knot_count + j],
                                                  sigmas[mapping.right_group * knot_count + j],
                                                  mapping.right_weight);
          if (!positive(sigma) || sigma < config.min_sigma || sigma > config.max_sigma) {
            return Err(ErrorCode::OutOfRange, "Markov fit: interpolated sigma loses range");
          }
          nodes[offset + j] = {shape.knots[j], sigma};
        }
      }
      intervals[i] = {in.calendar_end, in.variance_end, in.rate, in.continuous_yield, 1.0,
                      in.cash_dividend_at_end,
                      std::span<const AmericanMarkovLocalVolNode>(nodes).subspan(
                          offset, knot_count)};
    }
    return compile_american_markov(
        {market.spot, intervals, market.state_grid, market.model_config});
  }

  // Prices the expiry-major suffix shape.order[first_order..]. Entries before
  // it are owned by the caller (exactly reused prefix prices) and untouched.
  // Only the quotes actually priced are charged against max_rollback_work.
  Status price(const AmericanMarkovModel &model, std::vector<double> &prices,
               std::size_t first_order = 0) {
    const std::size_t states = model.diagnostics().max_states;
    if (strikes.size() > config.max_batch_values / states) {
      return Err(ErrorCode::OutOfRange, "Markov fit: batch workspace value limit exceeded");
    }
    // Price groups roll back different prefixes. Count the actual state-pass-option
    // work before invoking the kernels; no overflow-prone multiplication.
    std::vector<std::size_t> prefix_work(model.layers().size(), 0);
    for (std::size_t i = 0; i < model.transitions().size(); ++i) {
      prefix_work[i + 1] = prefix_work[i] + model.transitions()[i].rollback_work();
    }
    std::size_t work = 0;
    for (std::size_t k = first_order; k < shape.order.size(); ++k) {
      const auto &q = quotes[shape.order[k]];
      const auto quote_work = prefix_work[model.expiry_layers()[q.expiry_index]];
      if (quote_work > config.max_rollback_work - work) {
        return Err(ErrorCode::OutOfRange, "Markov fit: rollback work limit exceeded");
      }
      work += quote_work;
    }
    ATX_TRY_VOID(workspace.prepare(model, strikes.size()));
    // Admitted work is charged before the kernels run, even if one later fails.
    rollback_work += work;
    priced_quotes += shape.order.size() - first_order;
    reused_quote_prices += first_order;
    prices.resize(quotes.size());
    for (std::size_t first = first_order; first < shape.order.size();) {
      const auto &head = quotes[shape.order[first]];
      std::size_t count = 0;
      for (; first + count < shape.order.size() && count < strikes.size(); ++count) {
        const auto &q = quotes[shape.order[first + count]];
        if (q.expiry_index != head.expiry_index || q.side != head.side) break;
        strikes[count] = q.strike;
      }
      ATX_TRY_VOID(price_american_markov_batch(
          model, std::span<const double>(strikes).first(count), head.side, head.expiry_index,
          std::span<double>(values).first(count), workspace));
      for (std::size_t j = 0; j < count; ++j) {
        if (!nonnegative(values[j])) {
          return Err(ErrorCode::OutOfRange, "Markov fit: nonfinite or negative option price");
        }
        prices[shape.order[first + j]] = values[j];
      }
      first += count;
    }
    return Ok();
  }
};

struct Candidate {
  AmericanMarkovModel model;
  std::vector<double> prices;
  Objective loss;
};

// reuse/first_order: exactly unaffected prefix prices (see repriced_suffixes).
Result<Candidate> evaluate(Evaluator &e, std::span<const double> sigmas,
                           std::span<const double> reuse = {}, std::size_t first_order = 0) {
  ATX_TRY(auto model, e.compile(sigmas));
  std::vector<double> prices(reuse.begin(), reuse.end());
  ATX_TRY_VOID(e.price(model, prices, first_order));
  const auto loss = objective(e.quotes, prices, sigmas, e.shape, e.config);
  if (!nonnegative(loss.total) || !nonnegative(loss.max_error) ||
      !nonnegative(loss.mean_error)) {
    return Err(ErrorCode::OutOfRange, "Markov fit: nonfinite objective");
  }
  return Ok(Candidate{std::move(model), std::move(prices), loss});
}

// Mutable state shared by both searches. best/sigmas always hold the lowest
// finite objective evaluated so far and its controls; the stored model is
// this best iterate, never an unevaluated point.
struct SearchState {
  Evaluator &evaluator;
  const Layout &shape;
  const AmericanMarkovFitConfig &config;
  std::size_t budget = 0;
  AmericanMarkovFitDiagnostics &diagnostics;
  std::vector<double> &sigmas;
  Candidate &best;
};

// The historical compass search. The only change is that its evaluation cap
// is the effective budget, which equals max_evaluations unless scaling is on.
double coordinate_search(SearchState &s) {
  const auto &config = s.config;
  auto &diagnostics = s.diagnostics;
  auto &sigmas = s.sigmas;
  double step = config.initial_log_step;
  const double low = std::log(config.min_sigma);
  const double high = std::log(config.max_sigma);
  std::vector<double> trial = sigmas;
  for (std::size_t sweep = 0; sweep < config.max_sweeps; ++sweep) {
    bool improved = false;
    for (std::size_t i = 0; i < sigmas.size(); ++i) {
      const double center = std::log(sigmas[i]);
      for (const double direction : {-1.0, 1.0}) {
        if (diagnostics.evaluations == s.budget) break;
        const double candidate_sigma = std::clamp(
            std::exp(std::clamp(center + direction * step, low, high)),
            config.min_sigma, config.max_sigma);
        if (candidate_sigma == sigmas[i]) continue;
        trial = sigmas;
        trial[i] = candidate_sigma;
        ++diagnostics.evaluations;
        auto candidate = evaluate(s.evaluator, trial);
        if (!candidate) {
          ++diagnostics.failed_evaluations;
          continue;
        }
        if (candidate->loss.total < s.best.loss.total) {
          s.best = std::move(*candidate);
          sigmas = trial;
          improved = true;
          ++diagnostics.accepted_steps;
          diagnostics.best_evaluation = diagnostics.evaluations;
        }
      }
      if (diagnostics.evaluations == s.budget) break;
    }
    if (diagnostics.evaluations == s.budget) {
      diagnostics.termination = AmericanMarkovFitTermination::EvaluationLimit;
      break;
    }
    ++diagnostics.completed_sweeps;
    if (!improved) {
      step *= 0.5;
      if (step < config.min_log_step) {
        diagnostics.termination = AmericanMarkovFitTermination::StepTolerance;
        break;
      }
    }
  }
  return step;
}

// Parameter p is frozen into interval i iff i reads p's time group (either
// interpolation side; paused rows included, conservatively). A quote on an
// earlier expiry keeps a bit-identical price because compilation and rollback
// of an expiry prefix read only prefix intervals. Order is expiry-major, so
// each parameter owns a repriced suffix of shape.order; the prefix is reused.
std::vector<std::size_t> repriced_suffixes(std::span<const AmericanMarkovFitQuote> quotes,
                                           const Layout &l, std::size_t parameters) {
  const auto none = l.time_weights.size();
  std::vector<std::size_t> first_interval(l.time_groups, none);
  for (std::size_t i = 0; i < l.time_weights.size(); ++i) {
    for (const auto group : {l.time_weights[i].left_group, l.time_weights[i].right_group}) {
      if (group < first_interval.size()) {
        first_interval[group] = std::min(first_interval[group], i);
      }
    }
  }
  std::vector<std::size_t> suffix(parameters, 0);
  for (std::size_t p = 0; p < parameters; ++p) {
    const auto start = first_interval[p / l.knots.size()];
    const auto it = std::partition_point(l.order.begin(), l.order.end(), [&](std::size_t index) {
      return quotes[index].expiry_index < start;
    });
    suffix[p] = static_cast<std::size_t>(it - l.order.begin());
  }
  return suffix;
}

// Gauss-Newton normal equations (lower triangle mirrored) of the EXACT
// objective's residual vector at the current prices: interval hinge rows are
// active only outside [bid, ask], midpoint rows only with a bid, and the
// log-sigma prior/roughness rows are linear. jacobian is column-major
// d price / d log sigma. Returns max |d objective / d log sigma| = 2 max |J'r|.
double normal_equations(const Evaluator &e, std::span<const double> prices,
                        std::span<const double> sigmas, std::span<const double> jacobian,
                        std::vector<double> &normal, std::vector<double> &gradient) {
  const auto &c = e.config;
  const auto n = sigmas.size();
  const auto q = e.quotes.size();
  std::fill(normal.begin(), normal.end(), 0.0);
  std::fill(gradient.begin(), gradient.end(), 0.0);
  double weights = 0.0;
  for (const auto &quote : e.quotes) weights += quote.weight;
  const double root_weights = std::sqrt(weights);
  const double root_midpoint = std::sqrt(c.midpoint_weight);
  for (std::size_t i = 0; i < q; ++i) {
    const auto &quote = e.quotes[i];
    // Same square-root normalization and residual formulas as objective().
    const double root_weight = std::sqrt(quote.weight) / root_weights;
    const double scale = std::max(0.5 * (quote.ask - quote.bid), c.price_scale_floor);
    const double unit = root_weight / scale;
    double curvature = 0.0; // Sum of squared row coefficients on d price_i.
    double slope = 0.0;     // Sum of row coefficient times residual.
    if (quote.bid - prices[i] > 0.0) {
      slope -= unit * ((root_weight * (quote.bid - prices[i])) / scale);
      curvature += unit * unit;
    } else if (prices[i] - quote.ask > 0.0) {
      slope += unit * ((root_weight * (prices[i] - quote.ask)) / scale);
      curvature += unit * unit;
    }
    if (quote.bid > 0.0 && c.midpoint_weight > 0.0) {
      const double center = 0.5 * quote.bid + 0.5 * quote.ask;
      const double tie = root_midpoint * ((root_weight * (prices[i] - center)) / scale);
      slope += root_midpoint * unit * tie;
      curvature += c.midpoint_weight * unit * unit;
    }
    if (curvature == 0.0) continue;
    for (std::size_t a = 0; a < n; ++a) {
      const double da = jacobian[a * q + i];
      if (da == 0.0) continue;
      gradient[a] += slope * da;
      for (std::size_t b = 0; b <= a; ++b) {
        normal[a * n + b] += curvature * da * jacobian[b * q + i];
      }
    }
  }
  // objective() divides the regularization sum by the parameter count.
  const double count = static_cast<double>(n);
  const auto pair = [&](std::size_t a, std::size_t b, double weight) { // b < a
    const double coefficient = weight / count;
    const double difference = std::log(sigmas[a]) - std::log(sigmas[b]);
    gradient[a] += coefficient * difference;
    gradient[b] -= coefficient * difference;
    normal[a * n + a] += coefficient;
    normal[b * n + b] += coefficient;
    normal[a * n + b] -= coefficient;
  };
  const auto knots = e.shape.knots.size();
  for (std::size_t i = 0; i < n; ++i) {
    const double prior = c.prior_weight / count;
    gradient[i] += prior * (std::log(sigmas[i]) - std::log(e.shape.prior[i]));
    normal[i * n + i] += prior;
    if (i % knots != 0) pair(i, i - 1, c.strike_roughness_weight);
    if (i >= knots) pair(i, i - knots, c.time_roughness_weight);
  }
  double norm = 0.0;
  for (std::size_t a = 0; a < n; ++a) {
    for (std::size_t b = 0; b < a; ++b) normal[b * n + a] = normal[a * n + b];
    norm = std::max(norm, 2.0 * std::abs(gradient[a]));
  }
  return norm;
}

// Cholesky solve of (A + damping * diag(max(A_jj, floor))) x = -g, with the
// Marquardt floor relative to the largest diagonal. False when the damped
// system is not numerically positive definite or the step is nonfinite.
bool damped_step(std::span<const double> normal, std::span<const double> gradient,
                 double damping, std::vector<double> &system, std::vector<double> &step) {
  const auto n = gradient.size();
  double largest = 0.0;
  for (std::size_t j = 0; j < n; ++j) largest = std::max(largest, normal[j * n + j]);
  const double diagonal_floor = largest > 0.0 ? largest * 1.0e-12 : 1.0;
  std::copy(normal.begin(), normal.end(), system.begin());
  for (std::size_t j = 0; j < n; ++j) {
    system[j * n + j] += damping * std::max(normal[j * n + j], diagonal_floor);
  }
  for (std::size_t j = 0; j < n; ++j) {
    double diagonal = system[j * n + j];
    for (std::size_t k = 0; k < j; ++k) diagonal -= system[j * n + k] * system[j * n + k];
    if (!std::isfinite(diagonal) || !(diagonal > 0.0)) return false;
    const double root = std::sqrt(diagonal);
    system[j * n + j] = root;
    for (std::size_t i = j + 1; i < n; ++i) {
      double value = system[i * n + j];
      for (std::size_t k = 0; k < j; ++k) value -= system[i * n + k] * system[j * n + k];
      system[i * n + j] = value / root;
    }
  }
  for (std::size_t i = 0; i < n; ++i) {
    double value = -gradient[i];
    for (std::size_t k = 0; k < i; ++k) value -= system[i * n + k] * step[k];
    step[i] = value / system[i * n + i];
  }
  for (std::size_t i = n; i-- > 0;) {
    double value = step[i];
    for (std::size_t k = i + 1; k < n; ++k) value -= system[k * n + i] * step[k];
    step[i] = value / system[i * n + i];
    if (!std::isfinite(step[i])) return false;
  }
  return true;
}

// Linear-model decrease ||r||^2 - ||r + J step||^2 = -2 g'step - step'A step.
double predicted_decrease(std::span<const double> normal, std::span<const double> gradient,
                          std::span<const double> step) {
  const auto n = step.size();
  double value = 0.0;
  for (std::size_t a = 0; a < n; ++a) {
    double row = 0.0;
    for (std::size_t b = 0; b < n; ++b) row += normal[a * n + b] * step[b];
    value -= 2.0 * gradient[a] * step[a] + step[a] * row;
  }
  return value;
}

constexpr std::size_t kMaxDampingAttempts = 128; // Damping at least doubles per rejection.
constexpr double kMinDamping = 1.0e-12;

// Damped Gauss-Newton in log sigma (Nielsen damping update), box-projected.
// One iteration costs n Jacobian evaluations plus >= 1 trial evaluation, and
// is attempted only if the remaining budget covers n + 1. The iterate moves
// only on strict decrease; Jacobian probes that happen to beat the stored best
// replace it (they are fully evaluated models) without moving the iterate.
void levenberg_marquardt(SearchState &s) {
  auto &e = s.evaluator;
  const auto &c = s.config;
  auto &d = s.diagnostics;
  const auto n = s.sigmas.size();
  const auto q = e.quotes.size();
  const double low = std::log(c.min_sigma);
  const double high = std::log(c.max_sigma);
  const auto bounded = [&](double log_sigma) {
    return std::clamp(std::exp(std::clamp(log_sigma, low, high)), c.min_sigma, c.max_sigma);
  };
  const auto suffix = repriced_suffixes(e.quotes, s.shape, n);
  std::vector<double> sigmas = s.sigmas;
  std::vector<double> prices = s.best.prices;
  double value = s.best.loss.total;
  std::vector<double> jacobian(n * q), normal(n * n), gradient(n), system(n * n), step(n);
  std::vector<double> trial(n);
  double damping = c.initial_damping;
  double growth = 2.0;
  d.final_damping = damping;
  const auto adopt = [&](Candidate &candidate, std::span<const double> controls) {
    if (!(candidate.loss.total < s.best.loss.total)) return;
    s.best = std::move(candidate);
    s.sigmas.assign(controls.begin(), controls.end());
    d.best_evaluation = d.evaluations;
  };
  const auto grow = [&] {
    damping *= growth;
    growth *= 2.0;
    d.final_damping = damping;
    return damping <= c.max_damping;
  };
  for (std::size_t iteration = 0; iteration < c.max_sweeps; ++iteration) {
    if (s.budget - d.evaluations < n + 1) {
      d.termination = AmericanMarkovFitTermination::EvaluationLimit;
      return;
    }
    for (std::size_t p = 0; p < n; ++p) {
      const auto column = std::span<double>(jacobian).subspan(p * q, q);
      std::fill(column.begin(), column.end(), 0.0);
      const double center = std::log(sigmas[p]);
      double probe = bounded(center + c.jacobian_log_step);
      if (probe == sigmas[p]) probe = bounded(center - c.jacobian_log_step);
      const double delta = std::log(probe) - center;
      trial = sigmas;
      trial[p] = probe;
      ++d.evaluations;
      ++d.jacobian_evaluations;
      auto candidate = evaluate(e, trial, prices, suffix[p]);
      if (!candidate) {
        ++d.failed_evaluations; // Column stays zero: p is frozen this iteration.
        continue;
      }
      if (delta != 0.0) {
        for (std::size_t i = 0; i < q; ++i) column[i] = (candidate->prices[i] - prices[i]) / delta;
      }
      adopt(*candidate, trial);
    }
    d.gradient_norm = normal_equations(e, prices, sigmas, jacobian, normal, gradient);
    if (!(d.gradient_norm > c.gradient_tolerance)) {
      d.termination = AmericanMarkovFitTermination::GradientTolerance;
      return;
    }
    bool accepted = false;
    for (std::size_t attempt = 0; attempt < kMaxDampingAttempts && !accepted; ++attempt) {
      if (!damped_step(normal, gradient, damping, system, step)) {
        if (!grow()) break;
        continue;
      }
      double largest = 0.0;
      for (std::size_t p = 0; p < n; ++p) {
        trial[p] = bounded(std::log(sigmas[p]) + step[p]);
        step[p] = std::log(trial[p]) - std::log(sigmas[p]);
        largest = std::max(largest, std::abs(step[p]));
      }
      d.final_log_step = largest;
      if (largest < c.min_log_step) {
        d.termination = AmericanMarkovFitTermination::StepTolerance;
        return;
      }
      if (d.evaluations >= s.budget) {
        d.termination = AmericanMarkovFitTermination::EvaluationLimit;
        return;
      }
      const double predicted = predicted_decrease(normal, gradient, step);
      ++d.evaluations;
      auto candidate = evaluate(e, trial);
      if (!candidate || !(candidate->loss.total < value)) {
        d.failed_evaluations += candidate ? 0U : 1U;
        ++d.rejected_steps;
        if (!grow()) break;
        continue;
      }
      const double decrease = value - candidate->loss.total;
      const double ratio = predicted > 0.0 ? decrease / predicted : 0.0;
      const double cube = (2.0 * ratio - 1.0) * (2.0 * ratio - 1.0) * (2.0 * ratio - 1.0);
      damping = std::max(damping * std::max(1.0 / 3.0, 1.0 - cube), kMinDamping);
      growth = 2.0;
      d.final_damping = damping;
      const bool converged = decrease <= c.objective_tolerance * value;
      value = candidate->loss.total;
      prices = candidate->prices;
      sigmas = trial;
      ++d.accepted_steps;
      adopt(*candidate, trial);
      accepted = true;
      if (converged) {
        ++d.completed_sweeps;
        d.termination = AmericanMarkovFitTermination::ObjectiveTolerance;
        return;
      }
    }
    if (!accepted) {
      d.termination = AmericanMarkovFitTermination::DampingLimit;
      return;
    }
    ++d.completed_sweeps;
  }
  d.termination = AmericanMarkovFitTermination::SweepLimit;
}
} // namespace

AmericanMarkovFit::AmericanMarkovFit(AmericanMarkovModel model, std::vector<double> sigmas,
                                   std::vector<double> knots, AmericanMarkovFitTimeShape time_shape,
                                   std::vector<double> time_knots,
                                   std::vector<AmericanMarkovFitTimeWeight> time_weights,
                                   std::optional<AmericanMarkovFitTimeBasisDiagnostics> time_basis,
                                   std::vector<double> prices,
                                   AmericanMarkovFitDiagnostics diagnostics)
    : model_(std::move(model)), sigmas_(std::move(sigmas)), knots_(std::move(knots)),
      time_shape_(time_shape), time_knots_(std::move(time_knots)),
      time_weights_(std::move(time_weights)), time_basis_(std::move(time_basis)),
      prices_(std::move(prices)), diag_(diagnostics) {}

Result<AmericanMarkovFit> fit_american_markov_intervals(
    const AmericanMarkovFitMarket &market, std::span<const AmericanMarkovFitQuote> quotes,
    const AmericanMarkovFitConfig &config) {
  ATX_TRY_VOID(validate(market, quotes, config));
  ATX_TRY(auto shape, layout(market, quotes, config));
  Evaluator evaluator{market, quotes, config, shape};
  std::vector<double> sigmas = shape.initial;
  ATX_TRY(auto best, evaluate(evaluator, sigmas));
  AmericanMarkovFitDiagnostics diagnostics;
  diagnostics.evaluations = 1;
  diagnostics.best_evaluation = 1;
  diagnostics.parameter_count = sigmas.size();
  diagnostics.time_groups = shape.time_groups;
  diagnostics.quote_count = quotes.size();
  diagnostics.initial_objective = best.loss.total;
  for (const auto &q : quotes) diagnostics.bidless_quotes += q.bid == 0.0 ? 1U : 0U;
  diagnostics.search = config.search;
  diagnostics.evaluation_budget = american_markov_fit_evaluation_budget(config, sigmas.size());
  SearchState search{evaluator, shape, config, diagnostics.evaluation_budget, diagnostics,
                     sigmas, best};
  if (config.search == AmericanMarkovFitSearch::LevenbergMarquardt) {
    levenberg_marquardt(search);
  } else {
    diagnostics.final_log_step = coordinate_search(search);
  }
  diagnostics.rollback_work = evaluator.rollback_work;
  diagnostics.priced_quotes = evaluator.priced_quotes;
  diagnostics.reused_quote_prices = evaluator.reused_quote_prices;
  diagnostics.objective = best.loss.total;
  diagnostics.interval_loss = best.loss.interval;
  diagnostics.midpoint_loss = best.loss.midpoint;
  diagnostics.regularization_loss = best.loss.regularization;
  diagnostics.quotes_inside_interval = best.loss.inside;
  diagnostics.max_interval_error = best.loss.max_error;
  diagnostics.mean_interval_error = best.loss.mean_error;
  return Ok(AmericanMarkovFit(std::move(best.model), std::move(sigmas), std::move(shape.knots),
                              config.time_shape, std::move(shape.time_knots),
                              std::move(shape.time_weights), std::move(shape.time_basis),
                              std::move(best.prices), diagnostics));
}
} // namespace atx::vol
