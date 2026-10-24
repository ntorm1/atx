#pragma once

// Private exact-arithmetic identity control. No numerical tolerance is used.
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

#include "atx/vol/api/core/types.hpp"

namespace atx::vol::cash_gaussian {

struct DyadicZeroRequest {
  double lower = 0., upper = 0., value_lower = 0., value_upper = 0.;
  double slope_lower = 0., slope_upper = 0.;
};

struct DyadicZeroBudget {
  std::size_t max_operands = 6, max_shifts = 13, max_sums = 8, max_products = 5;
  std::size_t max_masks = 2;
  std::size_t max_bit_scans = 32, max_workspace_bytes = 65536;
};

enum class DyadicZeroStatus {
  NotAttempted, InvalidInput, WorkspaceRefused, WorkRefused, ArithmeticRefused,
  ProvenZero, Nonzero
};

struct DyadicZeroWork {
  std::array<std::uint64_t, 6> input_bits{};
  DyadicZeroStatus status = DyadicZeroStatus::NotAttempted;
  std::size_t operands = 0, shifts = 0, sums = 0, products = 0;
  std::size_t masks = 0, bit_scans = 0, negations = 0, encodings = 0;
  std::size_t integer_bytes = 0, workspace_bytes = 0, maximum_magnitude_bits = 0;
};

struct DyadicZeroProof {
  std::array<std::uint64_t, 6> input_bits{};
  // N_integer = [3*(v1-v0)-(b-a)*(m0+m1)] * 2^2148.
  // Its canonical form is sign * odd_magnitude_hex * 2^trailing_zero_bits.
  int sign = 0;
  std::size_t magnitude_bits = 0, trailing_zero_bits = 0;
  std::string odd_magnitude_hex;
  [[nodiscard]] bool is_zero() const noexcept { return sign == 0; }
};

[[nodiscard]] std::size_t dyadic_zero_workspace_requirement() noexcept;

// Resets work, retains partial attempted work, and never returns a partial proof.
// Inputs must be finite with 0<=lower<upper. All integer arithmetic uses fixed
// checked8192-bit signed storage; shifts operate on nonnegative magnitudes only.
[[nodiscard]] Result<DyadicZeroProof> prove_middle_slope_zero(
    const DyadicZeroRequest &request, const DyadicZeroBudget &budget, DyadicZeroWork &work);

} // namespace atx::vol::cash_gaussian
