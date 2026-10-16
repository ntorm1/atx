#include "atx/vol/api/pricing/american_markov_batch.hpp"

#include <algorithm>
#include <cmath>
#include <new>
#include <utility>

namespace atx::vol {
namespace {
using atx::core::Err;
using atx::core::Ok;

// Compile side/exercise out of the inner pricing loop. Three independently
// indexed source rows remain contiguous, including repeated boundary indices.
template <bool Call, bool American>
bool rollback(const AmericanMarkovModel &model, std::span<const double> strikes,
              std::size_t terminal, std::size_t stride,
              std::vector<double> *&values, std::vector<double> *&scratch) noexcept {
  const auto &end = model.layers()[terminal];
  const auto end_states = end.states();
  for (std::size_t i = 0; i < end_states.size(); ++i) {
    const double stock = end.growth * end_states[i];
    double *const row = values->data() + i * stride;
    for (std::size_t k = 0; k < strikes.size(); ++k) {
      row[k] = std::max(Call ? stock - strikes[k] : strikes[k] - stock, 0.0);
    }
  }
  for (std::size_t n = terminal; n > 0; --n) {
    const auto &transition = model.transitions()[n - 1];
    const auto &layer = model.layers()[n - 1];
    const auto rows = transition.rows();
    const auto states = layer.states();
    bool finite = true;
    for (std::size_t i = 0; i < rows.size(); ++i) {
      const auto &row = rows[i];
      const double *const a = values->data() + row.destination[0] * stride;
      const double *const b = values->data() + row.destination[1] * stride;
      const double *const c = values->data() + row.destination[2] * stride;
      double *const out = scratch->data() + i * stride;
      const double p0 = row.probability[0];
      const double p1 = row.probability[1];
      const double p2 = row.probability[2];
      const double discount = transition.discount;
      const double stock = layer.growth * states[i];
      for (std::size_t k = 0; k < strikes.size(); ++k) {
        // Preserve scalar accumulation order with precise floating-point flags.
        const double continuation = ((p0 * a[k] + p1 * b[k]) + p2 * c[k]) * discount;
        finite &= std::isfinite(continuation);
        if constexpr (American) {
          out[k] = std::max(continuation,
                            std::max(Call ? stock - strikes[k] : strikes[k] - stock, 0.0));
        } else {
          out[k] = continuation;
        }
      }
    }
    if (!finite) return false;
    std::swap(values, scratch);
  }
  return true;
}
} // namespace

Status AmericanMarkovBatchWorkspace::prepare(const AmericanMarkovModel &model,
                                             std::size_t max_options) {
  if (model.layers().empty() || max_options == 0 ||
      max_options > kAmericanMarkovBatchMaxOptions ||
      model.diagnostics().max_states == 0 ||
      max_options > kAmericanMarkovBatchMaxValues / model.diagnostics().max_states) {
    return Err(ErrorCode::InvalidArgument, "American Markov batch: invalid workspace capacity");
  }
  const auto states = model.diagnostics().max_states;
  try {
    first_.resize(states * max_options);
    second_.resize(states * max_options);
  } catch (const std::bad_alloc &) {
    option_capacity_ = 0;
    state_capacity_ = 0;
    return Err(ErrorCode::Unavailable, "American Markov batch: workspace allocation failed");
  }
  option_capacity_ = max_options;
  state_capacity_ = states;
  return Ok();
}

Status price_american_markov_batch(const AmericanMarkovModel &model,
                                   std::span<const double> strikes, Side side,
                                   std::size_t expiry_index, std::span<double> output,
                                   AmericanMarkovBatchWorkspace &workspace,
                                   ExerciseStyle exercise) {
  if (model.layers().empty() || expiry_index >= model.expiry_layers().size() ||
      (side != Side::Call && side != Side::Put) ||
      (exercise != ExerciseStyle::American && exercise != ExerciseStyle::European) ||
      output.size() != strikes.size()) {
    return Err(ErrorCode::InvalidArgument, "American Markov batch: invalid option batch");
  }
  if (strikes.empty()) return Ok();
  if (workspace.option_capacity_ < strikes.size() ||
      workspace.state_capacity_ < model.diagnostics().max_states ||
      workspace.first_.size() < workspace.option_capacity_ * workspace.state_capacity_ ||
      workspace.second_.size() < workspace.option_capacity_ * workspace.state_capacity_) {
    return Err(ErrorCode::InvalidArgument, "American Markov batch: workspace is not prepared");
  }
  if (!std::all_of(strikes.begin(), strikes.end(),
                   [](double k) { return std::isfinite(k) && k >= 0.0; })) {
    return Err(ErrorCode::InvalidArgument, "American Markov batch: invalid strike");
  }
  auto *values = &workspace.first_;
  auto *scratch = &workspace.second_;
  const auto terminal = model.expiry_layers()[expiry_index];
  const auto stride = workspace.option_capacity_;
  bool finite = false;
  if (side == Side::Call) {
    finite = exercise == ExerciseStyle::American
                 ? rollback<true, true>(model, strikes, terminal, stride, values, scratch)
                 : rollback<true, false>(model, strikes, terminal, stride, values, scratch);
  } else {
    finite = exercise == ExerciseStyle::American
                 ? rollback<false, true>(model, strikes, terminal, stride, values, scratch)
                 : rollback<false, false>(model, strikes, terminal, stride, values, scratch);
  }
  if (!finite) {
    return Err(ErrorCode::OutOfRange, "American Markov batch: option value overflow");
  }
  const double *const result = values->data() + model.initial_state_index() * stride;
  std::copy_n(result, strikes.size(), output.begin());
  return Ok();
}

} // namespace atx::vol
