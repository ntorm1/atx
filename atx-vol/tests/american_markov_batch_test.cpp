#include "atx/vol/api/pricing/american_markov_batch.hpp"

#include <array>
#include <limits>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

namespace atx::vol {
namespace {

Result<AmericanMarkovModel> batch_model() {
  const std::array intervals{
      AmericanMarkovInterval{0.12, 0.08, -0.02, 0.03, 0.35, 2.0},
      AmericanMarkovInterval{0.28, 0.24, 0.04, 0.0, 0.25, 0.0},
      AmericanMarkovInterval{0.50, 0.38, 0.02, 0.01, 0.30, 1.0}};
  AmericanMarkovSpec spec{100.0, intervals, {}, {}};
  spec.config.grid_nodes = 33;
  return compile_american_markov(spec);
}

TEST(AmericanMarkovBatch, MatchesScalarEverySideExerciseAndExpiryWithWorkspaceReuse) {
  const auto model = batch_model();
  ASSERT_TRUE(model) << model.error().to_string();
  AmericanMarkovWorkspace scalar;
  AmericanMarkovBatchWorkspace batch;
  ASSERT_TRUE(scalar.prepare(*model));
  ASSERT_TRUE(batch.prepare(*model, 71));
  // Odd batch sizes exercise vector tails; order/duplicates have no effect.
  const std::array strikes{100.0, 150.0, 60.0, 101.125, 0.0, 100.0, 70.0};
  std::array<double, strikes.size()> output{};
  for (const auto side : {Side::Call, Side::Put}) {
    for (const auto style : {ExerciseStyle::American, ExerciseStyle::European}) {
      for (std::size_t expiry = 0; expiry < model->expiry_layers().size(); ++expiry) {
        ASSERT_TRUE(price_american_markov_batch(*model, strikes, side, expiry, output, batch, style));
        for (std::size_t i = 0; i < strikes.size(); ++i) {
          const auto expected = price_american_markov(*model, strikes[i], side, expiry, scalar, style);
          ASSERT_TRUE(expected);
          EXPECT_NEAR(output[i], *expected, 2.0e-12);
        }
      }
    }
  }
  std::vector<double> many(71);
  for (std::size_t i = 0; i < many.size(); ++i) many[i] = 50.0 + 1.5 * static_cast<double>(i);
  std::vector<double> all(many.size());
  ASSERT_TRUE(price_american_markov_batch(*model, many, Side::Put, 2, all, batch));
  for (std::size_t i = 0; i < many.size(); ++i) {
    const auto expected = price_american_markov(*model, many[i], Side::Put, 2, scalar);
    ASSERT_TRUE(expected);
    EXPECT_NEAR(all[i], *expected, 2.0e-12);
  }
}

TEST(AmericanMarkovBatch, SupportsOverlappingInputAndOutputWithoutChangingOperator) {
  const auto model = batch_model();
  ASSERT_TRUE(model);
  AmericanMarkovBatchWorkspace batch;
  ASSERT_TRUE(batch.prepare(*model, 4));
  std::array strikes{80.0, 90.0, 100.0, 110.0, -1.0};
  std::array<double, 4> expected{};
  const auto input = std::span<const double>{strikes}.first(4);
  ASSERT_TRUE(price_american_markov_batch(*model, input, Side::Call, 2, expected, batch));
  ASSERT_TRUE(price_american_markov_batch(*model, input, Side::Call, 2,
                                         std::span<double>{strikes}.subspan(1), batch));
  for (std::size_t i = 0; i < expected.size(); ++i) EXPECT_EQ(strikes[i + 1], expected[i]);
}

TEST(AmericanMarkovBatch, InvalidInputsLeaveOutputUntouchedAndMovedWorkspaceFailsClosed) {
  auto model = batch_model();
  ASSERT_TRUE(model);
  AmericanMarkovBatchWorkspace batch;
  std::array strikes{90.0, 100.0, 110.0};
  std::array output{-1.0, -2.0, -3.0};
  const auto original = output;
  EXPECT_FALSE(price_american_markov_batch(*model, strikes, Side::Put, 0, output, batch));
  ASSERT_TRUE(batch.prepare(*model, 3));
  EXPECT_FALSE(price_american_markov_batch(*model, strikes, Side::Put, 3, output, batch));
  strikes[1] = std::numeric_limits<double>::quiet_NaN();
  EXPECT_FALSE(price_american_markov_batch(*model, strikes, Side::Put, 0, output, batch));
  strikes[1] = 100.0;
  EXPECT_FALSE(price_american_markov_batch(*model, strikes, static_cast<Side>(255), 0, output, batch));
  auto owned = std::move(batch);
  EXPECT_FALSE(price_american_markov_batch(*model, strikes, Side::Put, 0, output, batch));
  EXPECT_EQ(output, original);
  EXPECT_FALSE(owned.prepare(*model, kAmericanMarkovBatchMaxOptions + 1));
  EXPECT_FALSE(owned.prepare(*model, 0));
  EXPECT_TRUE(price_american_markov_batch(*model, {}, Side::Call, 0, {}, owned));
  EXPECT_TRUE(price_american_markov_batch(*model, strikes, Side::Put, 0, output, owned));
  auto moved_model = std::move(*model);
  EXPECT_TRUE(price_american_markov_batch(moved_model, strikes, Side::Put, 0, output, owned));
  EXPECT_FALSE(price_american_markov_batch(*model, {}, Side::Call, 0, {}, owned));
}

} // namespace
} // namespace atx::vol
