#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <span>

#include "atx/vol/api/pricing/american_markov.hpp"

namespace atx::vol::detail {

// Trusted compiled factors and prepared buffers. State-major storage retains
// contiguous options in the inner loop. Input/output never overlap.
inline void resolvent_forward_sweep(std::span<const AmericanMarkovResolventFactor> factors,
                                    const double *input, double *output,
                                    std::size_t stride, std::size_t options) noexcept {
  for (std::size_t i = 0; i < factors.size(); ++i) {
    const double alpha = factors[i].forward_input_weight;
    for (std::size_t k = 0; k < options; ++k) {
      const double previous = i == 0 ? 0.0 : output[(i - 1) * stride + k];
      output[i * stride + k] = std::lerp(previous, input[i * stride + k], alpha);
    }
  }
}

inline bool resolvent_backward(std::span<const AmericanMarkovResolventFactor> factors,
                                std::span<const double> input,
                                std::span<double> output) noexcept {
  resolvent_forward_sweep(factors, input.data(), output.data(), 1, 1);
  double next = 0.0;
  bool finite = true;
  for (std::size_t i = factors.size(); i-- > 0;) {
    next = std::lerp(next, output[i], factors[i].backward_input_weight);
    output[i] = next;
    finite &= std::isfinite(next);
  }
  return finite;
}

// Finish applies discount and exercise WITHOUT letting the exercised value
// contaminate the pending linear solve. The consumed input's first option row
// holds raw next-state continuations during the second sweep; no third buffer
// or third state pass is needed.
template <class Finish>
bool resolvent_rollback(std::span<const AmericanMarkovResolventFactor> factors,
                        double *input, double *output, std::size_t stride,
                        std::size_t options, Finish finish) noexcept {
  resolvent_forward_sweep(factors, input, output, stride, options);
  std::fill_n(input, options, 0.0);
  bool finite = true;
  for (std::size_t i = factors.size(); i-- > 0;) {
    const double beta = factors[i].backward_input_weight;
    for (std::size_t k = 0; k < options; ++k) {
      const double continuation = std::lerp(input[k], output[i * stride + k], beta);
      input[k] = continuation;
      const double value = finish(continuation, i, k);
      finite &= std::isfinite(continuation) && std::isfinite(value);
      output[i * stride + k] = value;
    }
  }
  return finite;
}

inline bool resolvent_transpose(std::span<const AmericanMarkovResolventFactor> factors,
                                 std::span<const double> input,
                                 std::span<double> output) noexcept {
  double carry = 0.0;
  for (std::size_t i = 0; i < factors.size(); ++i) {
    const double total = input[i] + carry;
    const double beta = factors[i].backward_input_weight;
    output[i] = beta * total;
    carry = (1.0 - beta) * total;
  }
  carry = 0.0;
  bool finite = true;
  for (std::size_t i = factors.size(); i-- > 0;) {
    const double total = output[i] + carry;
    const double alpha = factors[i].forward_input_weight;
    output[i] = alpha * total;
    carry = (1.0 - alpha) * total;
    finite &= std::isfinite(output[i]) && std::isfinite(carry);
  }
  return finite;
}

} // namespace atx::vol::detail
