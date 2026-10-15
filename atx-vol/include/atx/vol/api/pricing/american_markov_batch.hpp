#pragma once

#include <cstddef>
#include <span>
#include <vector>

#include "atx/vol/api/pricing/american_markov.hpp"

namespace atx::vol {

inline constexpr std::size_t kAmericanMarkovBatchMaxOptions = 65536;
inline constexpr std::size_t kAmericanMarkovBatchMaxValues = 16777216;

class AmericanMarkovBatchWorkspace;

// Price one side/expiry over arbitrary ordered strikes using the SAME operator
// as scalar rollback. States are rows, strikes contiguous within each row so
// the inner loop can vectorize. No padding, strike-dependent tree or resampling.
//
// Successful calls allocate nothing; one workspace per concurrent caller.
// Output is written only on success. Strikes/output may overlap each other,
// but neither may alias the private workspace. Empty batches are valid.
[[nodiscard]] Status price_american_markov_batch(
    const AmericanMarkovModel &model, std::span<const double> strikes, Side side,
    std::size_t expiry_index, std::span<double> output,
    AmericanMarkovBatchWorkspace &workspace,
    ExerciseStyle exercise = ExerciseStyle::American);

class AmericanMarkovBatchWorkspace {
public:
  AmericanMarkovBatchWorkspace() = default;
  AmericanMarkovBatchWorkspace(const AmericanMarkovBatchWorkspace &) = delete;
  AmericanMarkovBatchWorkspace &operator=(const AmericanMarkovBatchWorkspace &) = delete;
  AmericanMarkovBatchWorkspace(AmericanMarkovBatchWorkspace &&) noexcept = default;
  AmericanMarkovBatchWorkspace &operator=(AmericanMarkovBatchWorkspace &&) noexcept = default;

  // Each of the two work buffers holds max_states * max_options doubles.
  // The hard value cap is PER BUFFER. prepare is the sole allocation path.
  [[nodiscard]] Status prepare(const AmericanMarkovModel &model, std::size_t max_options);
  [[nodiscard]] std::size_t option_capacity() const noexcept { return option_capacity_; }
  [[nodiscard]] std::size_t state_capacity() const noexcept { return state_capacity_; }

private:
  friend Status price_american_markov_batch(
      const AmericanMarkovModel &, std::span<const double>, Side, std::size_t,
      std::span<double>, AmericanMarkovBatchWorkspace &, ExerciseStyle);
  std::size_t option_capacity_{};
  std::size_t state_capacity_{};
  std::vector<double> first_;
  std::vector<double> second_;
};

} // namespace atx::vol
