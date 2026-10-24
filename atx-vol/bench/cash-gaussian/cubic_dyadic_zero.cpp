#include "cubic_dyadic_zero.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <exception>
#include <ios>
#include <limits>
#include <memory>
#include <new>

#include <boost/multiprecision/cpp_int.hpp>

namespace atx::vol::cash_gaussian {
namespace {
using atx::core::Err;
using Integer = boost::multiprecision::number<boost::multiprecision::cpp_int_backend<
    8192, 8192, boost::multiprecision::signed_magnitude,
    boost::multiprecision::checked, void>, boost::multiprecision::et_off>;
static_assert(std::numeric_limits<Integer>::digits == 8192);
// Fixed precision alone does not exclude Boost's Karatsuba scratch allocation.
// Splitting below at1024 bits bounds every factor by1075 bits. Fail at compile
// time if the installed backend would route these factors into Karatsuba.
constexpr auto limb_bits = std::numeric_limits<boost::multiprecision::limb_type>::digits;
static_assert((1075 + limb_bits-1)/limb_bits <
              boost::multiprecision::backends::karatsuba_cutoff);

struct Workspace {
  std::array<Integer, 6> input;
  Integer width, slope_sum, value_difference, lhs, rhs, numerator, magnitude;
  Integer mask, width_low, width_high, slope_low, slope_high;
  Integer product01, product10, product11;
};
constexpr std::size_t reserved_bytes = sizeof(Workspace)+8*sizeof(Integer)+4096;
static_assert(reserved_bytes <= 65536);
struct WorkRefusal {};
struct ArithmeticRefusal {};

void step(std::size_t &counter, std::size_t cap) {
  if (counter >= cap) throw WorkRefusal{};
  ++counter;
}

std::size_t observe(const Integer &value, Workspace &space,
                    const DyadicZeroBudget &budget, DyadicZeroWork &work) {
  space.magnitude = value;
  if (space.magnitude < 0) {
    space.magnitude = -space.magnitude;
    ++work.negations;
  }
  std::size_t bits = 0;
  if (space.magnitude != 0) {
    step(work.bit_scans, budget.max_bit_scans);
    bits = static_cast<std::size_t>(boost::multiprecision::msb(space.magnitude))+1;
  }
  work.maximum_magnitude_bits = std::max(work.maximum_magnitude_bits, bits);
  if (bits > 4200) throw ArithmeticRefusal{};
  return bits;
}

void left_shift(Integer &magnitude, unsigned shift,
                const DyadicZeroBudget &budget, DyadicZeroWork &work) {
  if (magnitude < 0) throw ArithmeticRefusal{};
  if (magnitude == 0 || shift == 0) return;
  step(work.shifts, budget.max_shifts);
  magnitude <<= shift;
}

void decode(std::uint64_t bits, Integer &value, Workspace &space,
            const DyadicZeroBudget &budget, DyadicZeroWork &work) {
  step(work.operands, budget.max_operands);
  const auto exponent = static_cast<unsigned>((bits >> 52U) & 0x7ffU);
  const auto fraction = bits & 0x000fffffffffffffULL;
  value = exponent == 0 ? fraction : fraction | 0x0010000000000000ULL;
  if (exponent != 0) left_shift(value, exponent-1, budget, work);
  if ((bits >> 63U) != 0 && value != 0) {
    value = -value;
    ++work.negations;
  }
  static_cast<void>(observe(value, space, budget, work));
}

void right_shift(Integer &magnitude, unsigned shift,
                 const DyadicZeroBudget &budget, DyadicZeroWork &work) {
  if (magnitude < 0) throw ArithmeticRefusal{};
  if (magnitude == 0 || shift == 0) return;
  step(work.shifts, budget.max_shifts);
  magnitude >>= shift;
}

void split_product(Workspace &s, const DyadicZeroBudget &budget, DyadicZeroWork &work) {
  s.mask = 1;
  left_shift(s.mask, 1024, budget, work);
  step(work.sums, budget.max_sums);
  s.mask -= 1;
  static_cast<void>(observe(s.mask, s, budget, work));
  step(work.masks, budget.max_masks);
  s.width_low = s.width & s.mask;
  s.width_high = s.width;
  right_shift(s.width_high, 1024, budget, work);
  s.slope_high = s.slope_sum;
  const bool negative = s.slope_high < 0;
  if (negative) { s.slope_high = -s.slope_high; ++work.negations; }
  step(work.masks, budget.max_masks);
  s.slope_low = s.slope_high & s.mask;
  right_shift(s.slope_high, 1024, budget, work);
  for (const auto *value : {&s.width_low, &s.width_high, &s.slope_low, &s.slope_high}) {
    if (observe(*value, s, budget, work) > 1075) throw ArithmeticRefusal{};
  }
  step(work.products, budget.max_products);
  s.rhs = s.width_low*s.slope_low;
  static_cast<void>(observe(s.rhs, s, budget, work));
  step(work.products, budget.max_products);
  s.product01 = s.width_low*s.slope_high;
  static_cast<void>(observe(s.product01, s, budget, work));
  step(work.products, budget.max_products);
  s.product10 = s.width_high*s.slope_low;
  static_cast<void>(observe(s.product10, s, budget, work));
  step(work.products, budget.max_products);
  s.product11 = s.width_high*s.slope_high;
  static_cast<void>(observe(s.product11, s, budget, work));
  step(work.sums, budget.max_sums);
  s.product01 += s.product10;
  static_cast<void>(observe(s.product01, s, budget, work));
  left_shift(s.product01, 1024, budget, work);
  static_cast<void>(observe(s.product01, s, budget, work));
  left_shift(s.product11, 2048, budget, work);
  static_cast<void>(observe(s.product11, s, budget, work));
  step(work.sums, budget.max_sums);
  s.rhs += s.product01;
  static_cast<void>(observe(s.rhs, s, budget, work));
  step(work.sums, budget.max_sums);
  s.rhs += s.product11;
  static_cast<void>(observe(s.rhs, s, budget, work));
  if (negative) { s.rhs = -s.rhs; ++work.negations; }
}

DyadicZeroProof calculate(const DyadicZeroBudget &budget, DyadicZeroWork &work) {
  auto space = std::make_unique<Workspace>();
  auto &s = *space;
  for (std::size_t i = 0; i < s.input.size(); ++i)
    decode(work.input_bits[i], s.input[i], s, budget, work);

  step(work.sums, budget.max_sums);
  s.width = s.input[1]-s.input[0];
  static_cast<void>(observe(s.width, s, budget, work));
  if (s.width <= 0) throw ArithmeticRefusal{};
  step(work.sums, budget.max_sums);
  s.slope_sum = s.input[4]+s.input[5];
  static_cast<void>(observe(s.slope_sum, s, budget, work));
  step(work.sums, budget.max_sums);
  s.value_difference = s.input[3]-s.input[2];
  static_cast<void>(observe(s.value_difference, s, budget, work));

  s.lhs = s.value_difference;
  const bool negative_lhs = s.lhs < 0;
  if (negative_lhs) { s.lhs = -s.lhs; ++work.negations; }
  step(work.products, budget.max_products);
  s.lhs *= 3U;
  static_cast<void>(observe(s.lhs, s, budget, work));
  left_shift(s.lhs, 1074, budget, work);
  static_cast<void>(observe(s.lhs, s, budget, work));
  if (negative_lhs) { s.lhs = -s.lhs; ++work.negations; }

  split_product(s, budget, work);
  step(work.sums, budget.max_sums);
  s.numerator = s.lhs-s.rhs;
  const auto bits = observe(s.numerator, s, budget, work);

  DyadicZeroProof result;
  result.input_bits = work.input_bits;
  result.magnitude_bits = bits;
  result.sign = s.numerator == 0 ? 0 : (s.numerator < 0 ? -1 : 1);
  if (result.sign != 0) {
    step(work.bit_scans, budget.max_bit_scans);
    const auto trailing = boost::multiprecision::lsb(s.magnitude);
    result.trailing_zero_bits = trailing;
    if (trailing != 0) {
      right_shift(s.magnitude, trailing, budget, work);
    }
    ++work.encodings;
    result.odd_magnitude_hex = s.magnitude.str(0, std::ios_base::hex);
  } else {
    ++work.encodings;
    result.odd_magnitude_hex = "0";
  }
  work.status = result.is_zero() ? DyadicZeroStatus::ProvenZero : DyadicZeroStatus::Nonzero;
  return result;
}
} // namespace

std::size_t dyadic_zero_workspace_requirement() noexcept { return reserved_bytes; }

Result<DyadicZeroProof> prove_middle_slope_zero(
    const DyadicZeroRequest &request, const DyadicZeroBudget &budget, DyadicZeroWork &work) {
  work = {};
  const std::array values{request.lower, request.upper, request.value_lower,
      request.value_upper, request.slope_lower, request.slope_upper};
  for (std::size_t i = 0; i < values.size(); ++i)
    work.input_bits[i] = std::bit_cast<std::uint64_t>(values[i]);
  if (!std::all_of(values.begin(), values.end(), [](double v) { return std::isfinite(v); }) ||
      request.lower < 0. || request.upper <= request.lower) {
    work.status = DyadicZeroStatus::InvalidInput;
    return Err(ErrorCode::InvalidArgument, "dyadic proof input domain");
  }
  work.integer_bytes = sizeof(Integer);
  if (budget.max_workspace_bytes < reserved_bytes) {
    work.status = DyadicZeroStatus::WorkspaceRefused;
    return Err(ErrorCode::Unavailable, "dyadic proof workspace budget");
  }
  work.workspace_bytes = reserved_bytes;
  try {
    return calculate(budget, work);
  } catch (const WorkRefusal &) {
    work.status = DyadicZeroStatus::WorkRefused;
    return Err(ErrorCode::Unavailable, "dyadic proof operation budget");
  } catch (const std::bad_alloc &) {
    work.status = DyadicZeroStatus::WorkspaceRefused;
    return Err(ErrorCode::Unavailable, "dyadic proof allocation");
  } catch (const ArithmeticRefusal &) {
    work.status = DyadicZeroStatus::ArithmeticRefused;
    return Err(ErrorCode::OutOfRange, "dyadic proof derived integer bound");
  } catch (const std::exception &) {
    work.status = DyadicZeroStatus::ArithmeticRefused;
    return Err(ErrorCode::OutOfRange, "dyadic proof checked integer operation");
  }
}
} // namespace atx::vol::cash_gaussian
