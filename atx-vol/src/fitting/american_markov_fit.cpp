#include "atx/vol/api/fitting/american_markov_fit.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>
#include <utility>

#include "atx/vol/api/pricing/american_markov_batch.hpp"
#include "fitting/american_markov_time_basis.hpp"

namespace atx::vol {
namespace {
using atx::core::Err;
using atx::core::Ok;

constexpr double kMaxWeight = 1.0e6;
bool positive(double x) noexcept { return std::isfinite(x) && x > 0.0; }
bool nonnegative(double x) noexcept { return std::isfinite(x) && x >= 0.0; }

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

Result<Layout> layout(const AmericanMarkovFitMarket &m,
                      std::span<const AmericanMarkovFitQuote> quotes,
                      const AmericanMarkovFitConfig &c) {
  Layout out;
  const bool time_knots = c.time_shape == AmericanMarkovFitTimeShape::VarianceTimeKnots;
  out.knots.assign(c.log_state_knots.begin(), c.log_state_knots.end());
  if (out.knots.empty()) out.knots.push_back(0.0);
  out.time_knots.assign(c.variance_time_knots.begin(), c.variance_time_knots.end());
  if (time_knots) out.time_groups = out.time_knots.size();
  out.time_weights.reserve(m.intervals.size());
  double calendar = 0.0;
  double variance = 0.0;
  for (std::size_t i = 0; i < m.intervals.size(); ++i) {
    const auto &interval = m.intervals[i];
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
  if (out.time_groups == 0 || out.time_groups > c.max_parameters / out.knots.size()) {
    return Err(ErrorCode::OutOfRange, "Markov fit: unidentifiable variance or parameter limit");
  }
  const auto count = out.time_groups * out.knots.size();
  if (time_knots) {
    const auto capacity = m.model_config.max_stored_volatility_values;
    if (capacity == 0 || capacity > kAmericanMarkovMaxStoredVolatilityValues ||
        m.intervals.size() > capacity / out.knots.size()) {
      return Err(ErrorCode::OutOfRange, "Markov fit: interval sigma scratch exceeds model capacity");
    }
    std::size_t last_quoted_interval = 0;
    for (const auto &quote : quotes) {
      last_quoted_interval = std::max(last_quoted_interval, quote.expiry_index);
    }
    ATX_TRY(auto basis, detail::diagnose_markov_time_basis(
        std::span<const AmericanMarkovFitTimeWeight>(out.time_weights)
            .first(last_quoted_interval + 1), out.time_groups));
    out.time_basis = std::move(basis);
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
          const double left = sigmas[mapping.left_group * knot_count + j];
          const double right = sigmas[mapping.right_group * knot_count + j];
          // Preserve exact controls at knots/tails and an exactly flat term
          // structure. Avoid exp(log(sigma)) perturbing Shared-equivalent rows.
          const double sigma = left == right ? left : std::exp(std::lerp(
              std::log(left), std::log(right), mapping.right_weight));
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

  Status price(const AmericanMarkovModel &model, std::vector<double> &prices) {
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
    for (const auto &q : quotes) {
      const auto quote_work = prefix_work[model.expiry_layers()[q.expiry_index]];
      if (quote_work > config.max_rollback_work - work) {
        return Err(ErrorCode::OutOfRange, "Markov fit: rollback work limit exceeded");
      }
      work += quote_work;
    }
    ATX_TRY_VOID(workspace.prepare(model, strikes.size()));
    prices.resize(quotes.size());
    for (std::size_t first = 0; first < shape.order.size();) {
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

Result<Candidate> evaluate(Evaluator &e, std::span<const double> sigmas) {
  ATX_TRY(auto model, e.compile(sigmas));
  std::vector<double> prices;
  ATX_TRY_VOID(e.price(model, prices));
  const auto loss = objective(e.quotes, prices, sigmas, e.shape, e.config);
  if (!nonnegative(loss.total) || !nonnegative(loss.max_error) ||
      !nonnegative(loss.mean_error)) {
    return Err(ErrorCode::OutOfRange, "Markov fit: nonfinite objective");
  }
  return Ok(Candidate{std::move(model), std::move(prices), loss});
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
  double step = config.initial_log_step;
  const double low = std::log(config.min_sigma);
  const double high = std::log(config.max_sigma);
  std::vector<double> trial = sigmas;
  for (std::size_t sweep = 0; sweep < config.max_sweeps; ++sweep) {
    bool improved = false;
    for (std::size_t i = 0; i < sigmas.size(); ++i) {
      const double center = std::log(sigmas[i]);
      for (const double direction : {-1.0, 1.0}) {
        if (diagnostics.evaluations == config.max_evaluations) break;
        const double candidate_sigma = std::clamp(
            std::exp(std::clamp(center + direction * step, low, high)),
            config.min_sigma, config.max_sigma);
        if (candidate_sigma == sigmas[i]) continue;
        trial = sigmas;
        trial[i] = candidate_sigma;
        ++diagnostics.evaluations;
        auto candidate = evaluate(evaluator, trial);
        if (!candidate) {
          ++diagnostics.failed_evaluations;
          continue;
        }
        if (candidate->loss.total < best.loss.total) {
          best = std::move(*candidate);
          sigmas = trial;
          improved = true;
          ++diagnostics.accepted_steps;
          diagnostics.best_evaluation = diagnostics.evaluations;
        }
      }
      if (diagnostics.evaluations == config.max_evaluations) break;
    }
    if (diagnostics.evaluations == config.max_evaluations) {
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
  diagnostics.final_log_step = step;
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
