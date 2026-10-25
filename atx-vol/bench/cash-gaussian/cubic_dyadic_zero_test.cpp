#include "cubic_dyadic_zero.hpp"

#include <array>
#include <bit>
#include <cstdint>
#include <limits>
#include <string>

#include <gtest/gtest.h>

namespace atx::vol::cash_gaussian {
namespace {
struct ExactCase {
  const char *name;
  std::array<std::uint64_t, 6> bits;
  int sign;
  std::size_t trailing;
  const char *odd_hex;
};
#define DYADIC_CASE(name,a,b,v0,v1,m0,m1,sign,trailing,odd) \
    ExactCase{name, {a,b,v0,v1,m0,m1}, sign, trailing, odd},
constexpr std::array cases{
#include "dyadic_zero_cases.inc"
};
#undef DYADIC_CASE
static_assert(cases.size() == 16);

DyadicZeroRequest request(const ExactCase &c) {
  return {std::bit_cast<double>(c.bits[0]), std::bit_cast<double>(c.bits[1]),
      std::bit_cast<double>(c.bits[2]), std::bit_cast<double>(c.bits[3]),
      std::bit_cast<double>(c.bits[4]), std::bit_cast<double>(c.bits[5])};
}

TEST(CashDyadicZero, MatchesIndependentlyCheckedFractionNumerators) {
  DyadicZeroWork work;
  for (const auto &c : cases) {
    SCOPED_TRACE(c.name);
    auto result = prove_middle_slope_zero(request(c), {}, work);
    ASSERT_TRUE(result) << result.error().message();
    EXPECT_EQ(result->input_bits, c.bits);
    EXPECT_EQ(work.input_bits, c.bits);
    EXPECT_EQ(result->sign, c.sign);
    EXPECT_EQ(result->trailing_zero_bits, c.trailing);
    EXPECT_EQ(result->odd_magnitude_hex, c.odd_hex);
    const std::string odd(c.odd_hex);
    const auto first = static_cast<unsigned>(odd[0] <= '9' ? odd[0]-'0' : odd[0]-'a'+10);
    const auto expected_bits = c.sign == 0 ? 0U :
        4*(odd.size()-1)+std::bit_width(first)+c.trailing;
    EXPECT_EQ(result->magnitude_bits, expected_bits);
    EXPECT_EQ(work.operands, 6U);
    EXPECT_EQ(work.sums, 8U);
    EXPECT_EQ(work.products, 5U);
    EXPECT_EQ(work.masks, 2U);
    EXPECT_LE(work.shifts, 13U);
    EXPECT_LE(work.bit_scans, 32U);
    EXPECT_LE(work.negations, 32U);
    EXPECT_EQ(work.encodings, 1U);
    EXPECT_LE(work.maximum_magnitude_bits, 4200U);
    EXPECT_EQ(work.status, c.sign == 0 ? DyadicZeroStatus::ProvenZero : DyadicZeroStatus::Nonzero);
    EXPECT_EQ(work.workspace_bytes, dyadic_zero_workspace_requirement());
    EXPECT_GE(work.integer_bytes, 1024U);
  }
}

TEST(CashDyadicZero, WorkspaceRefusesBeforeIntegerConstructionOrWork) {
  DyadicZeroWork work;
  auto budget = DyadicZeroBudget{};
  budget.max_workspace_bytes = dyadic_zero_workspace_requirement()-1;
  EXPECT_FALSE(prove_middle_slope_zero(request(cases[0]), budget, work));
  EXPECT_EQ(work.status, DyadicZeroStatus::WorkspaceRefused);
  EXPECT_EQ(work.operands+work.shifts+work.sums+work.products+work.masks+work.bit_scans, 0U);
  EXPECT_EQ(work.workspace_bytes, 0U);
  EXPECT_EQ(work.input_bits, cases[0].bits);
}

TEST(CashDyadicZero, EachBudgetRetainsPartialWorkAndNextCallResets) {
  DyadicZeroWork work;
  for (unsigned slot = 0; slot < 6; ++slot) {
    auto b = DyadicZeroBudget{};
    if (slot == 0) b.max_operands = 2;
    if (slot == 1) b.max_shifts = 0;
    if (slot == 2) b.max_sums = 0;
    if (slot == 3) b.max_products = 0;
    if (slot == 4) b.max_bit_scans = 0;
    if (slot == 5) b.max_masks = 0;
    EXPECT_FALSE(prove_middle_slope_zero(request(cases[0]), b, work));
    EXPECT_EQ(work.status, DyadicZeroStatus::WorkRefused);
    EXPECT_EQ(work.workspace_bytes, dyadic_zero_workspace_requirement());
    EXPECT_LE(work.operands, b.max_operands);
    EXPECT_LE(work.shifts, b.max_shifts);
    EXPECT_LE(work.sums, b.max_sums);
    EXPECT_LE(work.products, b.max_products);
    EXPECT_LE(work.bit_scans, b.max_bit_scans);
    EXPECT_LE(work.masks, b.max_masks);
  }
  ASSERT_TRUE(prove_middle_slope_zero(request(cases[0]), {}, work));
  EXPECT_EQ(work.operands, 6U);
  EXPECT_EQ(work.status, DyadicZeroStatus::ProvenZero);
}

TEST(CashDyadicZero, InvalidInputsNeverBecomeAProof) {
  DyadicZeroWork work;
  auto r = request(cases[0]);
  r.lower = -1.;
  EXPECT_FALSE(prove_middle_slope_zero(r, {}, work));
  EXPECT_EQ(work.status, DyadicZeroStatus::InvalidInput);
  r = request(cases[0]);
  r.upper = r.lower;
  EXPECT_FALSE(prove_middle_slope_zero(r, {}, work));
  r = request(cases[0]);
  r.slope_upper = std::numeric_limits<double>::infinity();
  EXPECT_FALSE(prove_middle_slope_zero(r, {}, work));
  EXPECT_EQ(work.operands, 0U);
  EXPECT_EQ(work.workspace_bytes, 0U);
}
} // namespace
} // namespace atx::vol::cash_gaussian
