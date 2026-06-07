#include <gtest/gtest.h>
#include <cstddef>
#include <vector>
#include "atx/engine/validation/bias_audit.hpp"
using namespace atx::engine::validation;
TEST(ValidationBiasAudit, NoLookahead_PassesTruncationInvariant) {
  auto causal = [](std::size_t n_visible){ std::vector<double> out; for (std::size_t i=0;i<n_visible;++i) out.push_back(double(i)); return out; };
  EXPECT_TRUE(check_no_lookahead(/*full_n=*/10, /*cut=*/6, causal));
}
TEST(ValidationBiasAudit, NoLookahead_CatchesLeak) {
  auto leaky = [](std::size_t n_visible){ std::vector<double> out; for (std::size_t i=0;i<6;++i) out.push_back(double(n_visible)); return out; };
  EXPECT_FALSE(check_no_lookahead(10, 6, leaky));
}
TEST(ValidationBiasAudit, Survivorship_DelistedPresentAndFrozen) {
  std::vector<double> pnl{0.0, 0.01, 0.02, 0.0, 0.0};
  EXPECT_TRUE(check_survivorship_frozen(pnl, /*delist_period=*/2));
  std::vector<double> bad{0.0, 0.01, 0.02, 0.03, 0.0};
  EXPECT_FALSE(check_survivorship_frozen(bad, 2));
}
TEST(ValidationBiasAudit, Snooping_CatchesPlantedOverfit) {
  EXPECT_TRUE(catches_overfit_synthetic());
}
