#include <gtest/gtest.h>

#include <span>
#include <string>
#include <vector>

#include "atx/external/databento.hpp"

using atx::external::databento::split_under_cap;

TEST(SplitUnderCap, SplitsUntilEachBatchUnderCap) {
  std::vector<std::string> syms{"A", "B", "C", "D"};
  // Estimator: $1 per symbol. cap = $2.5 -> each batch must have <= 2 symbols.
  auto est = [](std::span<const std::string> s) {
    return static_cast<double>(s.size());
  };
  auto batches = split_under_cap(std::span<const std::string>(syms), 2.5, est);

  std::vector<std::string> flat;
  for (const auto& b : batches) {
    EXPECT_LT(est(std::span<const std::string>(b)), 2.5);
    for (const auto& x : b) flat.push_back(x);
  }
  EXPECT_EQ(flat, syms); // order-preserving union
}

TEST(SplitUnderCap, SingleExpensiveSymbolEmittedAlone) {
  std::vector<std::string> syms{"BIG"};
  auto est = [](std::span<const std::string>) { return 999.0; };
  auto batches = split_under_cap(std::span<const std::string>(syms), 2.0, est);
  ASSERT_EQ(batches.size(), 1U);
  EXPECT_EQ(batches[0][0], "BIG");
}
