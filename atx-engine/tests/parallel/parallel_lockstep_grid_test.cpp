#include <gtest/gtest.h>

#include <array>
#include <cmath>
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include "atx/core/error.hpp"
#include "atx/engine/parallel/det_pool.hpp"
#include "atx/engine/parallel/lockstep_grid.hpp"

namespace atxtest_parallel_lockstep_grid_test {

namespace co = atx::core;
namespace par = atx::engine::parallel;

constexpr std::array<std::string_view, 2> kAllowed{"--trade-fraction", "--dust-multiple"};

par::GridRules rules(std::size_t max_variants = 4) {
  return par::GridRules{max_variants, kAllowed};
}

TEST(ParallelLockstepGrid, ValidGridPasses) {
  const std::vector<par::GridVariant> grid{
      {"t05", {{"--trade-fraction", ".05"}}},
      {"t10-d2", {{"--trade-fraction", ".10"}, {"--dust-multiple", ".2"}}},
      {"base", {}}};
  EXPECT_TRUE(par::validate_grid(grid, rules()));
}

TEST(ParallelLockstepGrid, RefusesBadIdsKeysDuplicatesAndCounts) {
  const auto refused = [](const std::vector<par::GridVariant>& grid, std::size_t max = 4) {
    const auto status = par::validate_grid(grid, rules(max));
    return !status && status.error().code() == co::ErrorCode::InvalidArgument;
  };
  EXPECT_TRUE(refused({}));                                           // empty
  EXPECT_TRUE(refused({{"a", {}}, {"b", {}}, {"c", {}}}, 2));         // too many
  EXPECT_TRUE(refused({{"A", {}}}));                                  // upper case
  EXPECT_TRUE(refused({{"../x", {}}}));                               // path characters
  EXPECT_TRUE(refused({{"", {}}}));                                   // empty id
  EXPECT_TRUE(refused({{std::string(65, 'a'), {}}}));                 // too long
  EXPECT_TRUE(refused({{"a", {}}, {"a", {}}}));                       // duplicate id
  EXPECT_TRUE(refused({{"a", {{"--neutralize", "none"}}}}));          // key not allowed
  EXPECT_TRUE(refused({{"a", {{"--dust-multiple", ".1"}, {"--dust-multiple", ".2"}}}}));
  EXPECT_FALSE(refused({{std::string(64, 'a'), {}}}));                // boundary: 64 chars
}

// Sequential (no pool, one worker) and pooled lanes compute the same lane-owned bits, and
// every lane runs exactly once.
TEST(ParallelLockstepGrid, LanesAreBitIdenticalForAnyWorkerCount) {
  constexpr std::size_t lanes = 37;
  const auto run = [&](par::DetPool* pool) {
    std::vector<double> out(lanes, 0.0);
    std::vector<int> calls(lanes, 0);
    const auto status = par::for_each_lane(pool, lanes, [&](std::size_t lane) -> co::Status {
      double x = 0;
      for (std::size_t k = 0; k < 1000; ++k)
        x += std::sin(0.001 * static_cast<double>(k * (lane + 1)));
      out[lane] = x;
      ++calls[lane];
      return co::Ok();
    });
    EXPECT_TRUE(status);
    for (const int c : calls) EXPECT_EQ(c, 1);
    return out;
  };
  par::DetPool one{1}, four{4};
  const auto sequential = run(nullptr);
  EXPECT_EQ(run(&one), sequential);
  EXPECT_EQ(run(&four), sequential);
}

// The reported failure is the lowest failing lane's: the sequential loop stops there, the
// pooled phase runs every lane and still reports it.
TEST(ParallelLockstepGrid, LowestFailingLaneIsReported) {
  constexpr std::size_t lanes = 16;
  const auto fail_at = [](std::size_t lane) -> co::Status {
    if (lane == 5 || lane == 11)
      return co::Err(co::ErrorCode::OutOfRange, "lane " + std::to_string(lane));
    return co::Ok();
  };
  par::DetPool pool{4};
  for (par::DetPool* p : {static_cast<par::DetPool*>(nullptr), &pool}) {
    const auto status = par::for_each_lane(p, lanes, fail_at);
    ASSERT_FALSE(status);
    EXPECT_EQ(status.error().message(), "lane 5");
  }
  std::vector<int> ran(lanes, 0);
  const auto stopped = par::for_each_lane(nullptr, lanes, [&](std::size_t lane) {
    ++ran[lane];
    return fail_at(lane);
  });
  ASSERT_FALSE(stopped);
  EXPECT_EQ(ran[5], 1);
  EXPECT_EQ(ran[6], 0); // sequential: stops at the first failure
  EXPECT_TRUE(par::for_each_lane(&pool, 0, fail_at)); // no lanes: Ok
}

} // namespace atxtest_parallel_lockstep_grid_test
