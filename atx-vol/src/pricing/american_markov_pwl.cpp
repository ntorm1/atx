#include "atx/vol/api/pricing/american_markov_pwl.hpp"

#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <new>
#include <string>
#include <utility>

namespace atx::vol {
namespace {

using atx::core::Err;
using atx::core::Ok;

[[nodiscard]] Status validate(const AmericanMarkovModel &model, double lo, double hi,
                              const AmericanMarkovPwlLimits &limits) {
  if (model.layers().empty() || model.expiry_layers().empty()) {
    return Err(ErrorCode::InvalidArgument, "American Markov PWL: empty or moved-from model");
  }
  if (model.diffusion_method() != AmericanMarkovDiffusionMethod::ExplicitEuler) {
    return Err(ErrorCode::Unavailable, "American Markov PWL: variance resolvent is unsupported");
  }
  if (!std::isfinite(lo) || !std::isfinite(hi) || !(lo > 0.0) || !(hi > lo)) {
    return Err(ErrorCode::InvalidArgument, "American Markov PWL: invalid strike domain");
  }
  if (limits.max_expiries == 0u || limits.max_expiries > kAmericanMarkovPwlMaxExpiries ||
      limits.max_live_nodes < 2u || limits.max_live_nodes > kAmericanMarkovPwlMaxLiveNodes ||
      limits.max_generated_nodes < 2u ||
      limits.max_generated_nodes > kAmericanMarkovPwlMaxGeneratedNodes ||
      limits.max_input_nodes < 2u || limits.max_input_nodes > kAmericanMarkovPwlMaxInputNodes ||
      limits.max_operations == 0u || limits.max_operations > kAmericanMarkovPwlMaxOperations) {
    return Err(ErrorCode::InvalidArgument, "American Markov PWL: invalid aggregate resource limits");
  }
  if (model.expiry_layers().size() > limits.max_expiries) {
    return Err(ErrorCode::Unavailable, "American Markov PWL: expiry limit exceeded");
  }
  const auto &algebra = limits.algebra;
  if (algebra.max_nodes < 2u || algebra.max_nodes > kPwlStrikeMaxNodes ||
      algebra.max_terms == 0u || algebra.max_terms > kPwlStrikeMaxTerms ||
      algebra.max_input_nodes < 2u || algebra.max_input_nodes > kPwlStrikeMaxInputNodes ||
      algebra.max_workspace_nodes < 2u ||
      algebra.max_workspace_nodes > kPwlStrikeMaxWorkspaceNodes) {
    return Err(ErrorCode::InvalidArgument, "American Markov PWL: invalid algebra resource limits");
  }
  return Ok();
}

struct Budget {
  const AmericanMarkovPwlLimits &limits;
  AmericanMarkovPwlDiagnostics diagnostics;
  std::size_t live_nodes{};

  [[nodiscard]] Result<PwlStrikeLimits> begin(std::size_t input_nodes) {
    if (diagnostics.algebra_operations >= limits.max_operations ||
        input_nodes > limits.max_input_nodes - diagnostics.input_nodes) {
      return Err(ErrorCode::Unavailable, "American Markov PWL: aggregate work limit exceeded");
    }
    ++diagnostics.algebra_operations;
    diagnostics.input_nodes += input_nodes;
    auto allowed = limits.algebra;
    allowed.max_nodes = std::min({allowed.max_nodes, limits.max_live_nodes - live_nodes,
                                  limits.max_generated_nodes - diagnostics.generated_nodes});
    if (allowed.max_nodes < 2u) {
      return Err(ErrorCode::Unavailable, "American Markov PWL: live or generated node limit exceeded");
    }
    return Ok(allowed);
  }

  void acquired(const ConvexPwlStrikeFunction &function) noexcept {
    const std::size_t count = function.nodes().size();
    assert(count <= limits.max_live_nodes - live_nodes);
    assert(count <= limits.max_generated_nodes - diagnostics.generated_nodes);
    live_nodes += count;
    diagnostics.generated_nodes += count;
    diagnostics.peak_live_nodes = std::max(diagnostics.peak_live_nodes, live_nodes);
    diagnostics.max_function_nodes = std::max(diagnostics.max_function_nodes, count);
  }

  void released(std::size_t count) noexcept {
    assert(count <= live_nodes);
    live_nodes -= count;
  }
};

[[nodiscard]] Result<ConvexPwlStrikeFunction>
rollback_side(const AmericanMarkovModel &model, std::size_t terminal, double lo, double hi,
               Side side, Budget &budget) {
  std::vector<ConvexPwlStrikeFunction> values;
  std::vector<ConvexPwlStrikeFunction> previous;
  values.reserve(model.diagnostics().max_states);
  previous.reserve(model.diagnostics().max_states);
  const auto &end = model.layers()[terminal];
  for (const double state : end.states()) {
    ATX_TRY(auto allowed, budget.begin(0u));
    ATX_TRY(auto function,
             ConvexPwlStrikeFunction::payoff(end.growth * state, side, lo, hi, allowed));
    budget.acquired(function);
    values.push_back(std::move(function));
  }
  for (std::size_t n = terminal; n > 0u; --n) {
    const auto &transition = model.transitions()[n - 1u];
    const auto &layer = model.layers()[n - 1u];
    const auto rows = transition.rows();
    const auto states = layer.states();
    for (std::size_t i = 0u; i < rows.size(); ++i) {
      const auto &row = rows[i];
      std::array<PwlStrikeTerm, 3> terms;
      std::size_t count = 0u;
      std::size_t inputs = 0u;
      for (std::size_t j = 0u; j < row.probability.size(); ++j) {
        if (row.probability[j] == 0.0) continue;
        const double weight = transition.discount * row.probability[j];
        if (!std::isfinite(weight) || !(weight > 0.0)) {
          return Err(ErrorCode::OutOfRange, "American Markov PWL: discounted weight loses range");
        }
        const auto &child = values[row.destination[j]];
        inputs += child.nodes().size();
        terms[count++] = {&child, weight};
      }
      ATX_TRY(auto sum_limits, budget.begin(inputs));
      ATX_TRY(auto continuation, ConvexPwlStrikeFunction::weighted_sum(
                                  std::span<const PwlStrikeTerm>(terms).first(count), sum_limits));
      budget.acquired(continuation);
      ATX_TRY(auto exercise_limits, budget.begin(continuation.nodes().size()));
      ATX_TRY(auto exercised,
               continuation.max_intrinsic(layer.growth * states[i], side, exercise_limits));
      budget.acquired(exercised);
      budget.released(continuation.nodes().size());
      previous.push_back(std::move(exercised));
    }
    for (const auto &function : values) budget.released(function.nodes().size());
    values.clear();
    values.swap(previous);
  }
  auto result = std::move(values[model.initial_state_index()]);
  for (const auto &function : values) budget.released(function.nodes().size());
  return Ok(std::move(result));
}

} // namespace

AmericanMarkovPwlSurface::AmericanMarkovPwlSurface(
    AmericanMarkovModel model, std::vector<AmericanMarkovPwlSlice> slices,
    AmericanMarkovPwlDiagnostics diagnostics, double min_strike, double max_strike) noexcept
    : model_(std::move(model)), slices_(std::move(slices)), diagnostics_(diagnostics),
      min_strike_(min_strike), max_strike_(max_strike) {}

Result<AmericanMarkovPwlSurface>
compile_american_markov_pwl(AmericanMarkovModel model, double min_strike, double max_strike,
                            AmericanMarkovPwlLimits limits) {
  ATX_TRY_VOID(validate(model, min_strike, max_strike, limits));
  Budget budget{limits, {}, 0u};
  try {
    std::vector<AmericanMarkovPwlSlice> slices;
    slices.reserve(model.expiry_layers().size());
    for (std::size_t expiry = 0u; expiry < model.expiry_layers().size(); ++expiry) {
      const std::size_t terminal = model.expiry_layers()[expiry];
      const auto &end = model.layers()[terminal];
      auto call = rollback_side(model, terminal, min_strike, max_strike, Side::Call, budget);
      if (!call) {
        return Err(call.error().code(), "American Markov PWL: call expiry " +
                    std::to_string(expiry) + ": " + call.error().message());
      }
      auto put = rollback_side(model, terminal, min_strike, max_strike, Side::Put, budget);
      if (!put) {
        return Err(put.error().code(), "American Markov PWL: put expiry " +
                    std::to_string(expiry) + ": " + put.error().message());
      }
      budget.diagnostics.output_nodes += call->nodes().size() + put->nodes().size();
      budget.diagnostics.max_output_crossing_resolution_error = std::max(
          {budget.diagnostics.max_output_crossing_resolution_error,
           call->crossing_resolution_error_bound(), put->crossing_resolution_error_bound()});
      slices.push_back({expiry, terminal, end.calendar_time, end.variance_time,
                         std::move(*call), std::move(*put)});
    }
    assert(budget.live_nodes == budget.diagnostics.output_nodes);
    return Ok(AmericanMarkovPwlSurface(std::move(model), std::move(slices), budget.diagnostics,
                                       min_strike, max_strike));
  } catch (const std::bad_alloc &) {
    return Err(ErrorCode::Unavailable, "American Markov PWL: allocation failed");
  }
}

std::optional<double> AmericanMarkovPwlSurface::price(double strike, Side side,
                                                     std::size_t expiry_index) const noexcept {
  if (expiry_index >= slices_.size() || (side != Side::Call && side != Side::Put)) {
    return std::nullopt;
  }
  const auto &slice = slices_[expiry_index];
  return side == Side::Call ? slice.call.value(strike) : slice.put.value(strike);
}

} // namespace atx::vol
