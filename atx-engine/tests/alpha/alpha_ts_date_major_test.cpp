#include <array>
#include <bit>
#include <cmath>
#include <limits>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "atx/core/types.hpp"
#include "atx/engine/alpha/panel.hpp"
#include "atx/engine/parallel/det_pool.hpp"
#include "atx/engine/alpha/vm.hpp"

namespace atxtest_alpha_ts_date_major {
using namespace atx::engine::alpha;

TEST(AlphaTsDateMajor_Lookback, FullAndUnalignedRangesPreserveIndependentCellOracle) {
  constexpr atx::usize dates = 13;
  constexpr atx::usize names = 133; // two tiles, SIMD tail and unaligned partitions
  constexpr atx::usize cells = dates * names;
  std::vector<atx::f64> x(cells);
  for (atx::usize i = 0; i < cells; ++i) x[i] = static_cast<atx::f64>(i) / 16.0;
  x[3] = -0.0;
  x[7] = std::bit_cast<atx::f64>(atx::u64{0x7ff8000000001234});
  x[12] = std::numeric_limits<atx::f64>::infinity();
  auto panel = Panel::create(dates, names, {"close"}, {x}, {});
  ASSERT_TRUE(panel);
  for (const EvalMode mode : {EvalMode::AuditExact, EvalMode::ResearchFast}) {
    Engine engine{*panel};
    engine.set_eval_mode(mode);
    for (const OpCode op : {OpCode::TsDelay, OpCode::TsDelta}) {
      for (const atx::usize d : {atx::usize{0}, atx::usize{1}, atx::usize{5}, dates, dates + 1}) {
        SCOPED_TRACE(static_cast<unsigned>(op));
        SCOPED_TRACE(d);
        std::vector<atx::f64> window(cells, static_cast<atx::f64>(d));
        std::vector<atx::f64> full(cells, -333.0), cut(cells, -333.0);
        Instr in{};
        in.op = op;
        in.src[0] = 0;
        in.src[1] = 1;
        in.dst = 2;
        std::array<ExtSlot, 3> slots{{{x.data(), nullptr, cells},
                                    {window.data(), nullptr, cells},
                                    {full.data(), full.data(), cells}}};
        ASSERT_TRUE(engine.execute_range(in, slots, 0, names));
        slots[2] = {cut.data(), cut.data(), cells};
        ASSERT_TRUE(engine.execute_range(in, slots, 0, 3));
        ASSERT_TRUE(engine.execute_range(in, slots, 3, 69));
        ASSERT_TRUE(engine.execute_range(in, slots, 69, names));
        for (atx::usize t = 0; t < dates; ++t) {
          for (atx::usize j = 0; j < names; ++j) {
            const atx::usize i = t * names + j;
            ASSERT_EQ(std::bit_cast<atx::u64>(full[i]), std::bit_cast<atx::u64>(cut[i]));
            if (d == 0 || t < d) {
              EXPECT_TRUE(std::isnan(full[i]));
            } else {
              const atx::f64 prior = x[(t - d) * names + j];
              const atx::f64 expected = op == OpCode::TsDelay ? prior : x[i] - prior;
              if (std::isnan(expected) && op == OpCode::TsDelta) EXPECT_TRUE(std::isnan(full[i]));
              else EXPECT_EQ(std::bit_cast<atx::u64>(full[i]), std::bit_cast<atx::u64>(expected));
            }
          }
        }
      }
    }
  }
}

TEST(AlphaTsDateMajor_Lookback, BlockOverlapCopiesBeforeWarmupAndPartialOverlapRejects) {
  constexpr atx::usize dates = 4, names = 3, cells = dates * names;
  const std::vector<atx::f64> original{1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12};
  auto panel = Panel::create(dates, names, {"close"}, {original}, {});
  ASSERT_TRUE(panel);
  Engine engine{*panel};
  auto x = original;
  std::vector<atx::f64> window(cells, 1.0);
  Instr in{};
  in.op = OpCode::TsDelay;
  in.src[0] = 0;
  in.src[1] = 1;
  in.dst = 2;
  std::array<ExtSlot, 3> slots{{{x.data(), nullptr, cells},
                              {window.data(), nullptr, cells}, {x.data(), x.data(), cells}}};
  ASSERT_TRUE(engine.execute_range(in, slots, 0, names));
  for (atx::usize i = 0; i < names; ++i) EXPECT_TRUE(std::isnan(x[i]));
  for (atx::usize i = names; i < cells; ++i) EXPECT_EQ(x[i], original[i - names]);
  const auto before = x;
  EXPECT_FALSE(engine.execute_range(in, slots, 1, names));
  in.op = OpCode::TsDelta;
  EXPECT_FALSE(engine.execute_range(in, slots, 0, names));
  for (atx::usize i = 0; i < cells; ++i)
    EXPECT_EQ(std::bit_cast<atx::u64>(x[i]), std::bit_cast<atx::u64>(before[i]));
}

TEST(AlphaTsDateMajor_Sum, TiledSimdAndWorkerCutsMatchScalarCompensatedStateBits) {
  constexpr atx::usize dates = 73, names = 133, cells = dates * names;
  std::vector<atx::f64> x(cells);
  for (atx::usize i = 0; i < cells; ++i)
    x[i] = (i % 7 == 0 ? -1.0 : 1.0) * (1.0e8 + static_cast<atx::f64>(i % 43) / 32.0);
  x[10 * names + 2] = std::numeric_limits<atx::f64>::quiet_NaN();
  x[12 * names + 64] = std::numeric_limits<atx::f64>::infinity();
  x[13 * names + 132] = -std::numeric_limits<atx::f64>::infinity();
  auto panel = Panel::create(dates, names, {"close"}, {x}, {});
  ASSERT_TRUE(panel);
  atx::engine::parallel::DetPool pool{2};
  Engine engine{*panel};
  engine.set_eval_mode(EvalMode::ResearchFast);
  for (const OpCode op : {OpCode::TsSum, OpCode::TsMean}) {
    for (const atx::usize d : {atx::usize{1}, atx::usize{17}, dates + 1}) {
      std::vector<atx::f64> expected(cells), actual(cells), cut(cells);
      std::vector<atx::f64> window(cells, static_cast<atx::f64>(d));
      for (atx::usize j = 0; j < names; ++j)
        detail::ts_online_sum_family(op, x, expected, dates, j, d, names, true);
      Instr in{};
      in.op = op;
      in.src[0] = 0;
      in.src[1] = 1;
      in.dst = 2;
      std::array<ExtSlot, 3> slots{{{x.data(), nullptr, cells},
                                  {window.data(), nullptr, cells},
                                  {actual.data(), actual.data(), cells}}};
      engine.set_ts_pool(&pool);
      ASSERT_TRUE(engine.execute_range(in, slots, 0, names));
      slots[2] = {cut.data(), cut.data(), cells};
      engine.set_ts_pool(nullptr);
      ASSERT_TRUE(engine.execute_range(in, slots, 0, 3));
      ASSERT_TRUE(engine.execute_range(in, slots, 3, 69));
      ASSERT_TRUE(engine.execute_range(in, slots, 69, names));
      for (atx::usize i = 0; i < cells; ++i) {
        ASSERT_EQ(std::bit_cast<atx::u64>(actual[i]), std::bit_cast<atx::u64>(expected[i])) << i;
        ASSERT_EQ(std::bit_cast<atx::u64>(cut[i]), std::bit_cast<atx::u64>(expected[i])) << i;
      }
    }
  }
}

TEST(AlphaTsDateMajor_UnarySliding, TiledWorkerDispatchPreservesEachExistingLaneBits) {
  constexpr atx::usize dates = 83, names = 131, cells = dates * names, d = 17;
  std::vector<atx::f64> x(cells);
  for (atx::usize i = 0; i < cells; ++i)
    x[i] = 100.0 + std::sin(static_cast<atx::f64>(i) * 0.31);
  x[21 * names + 64] = std::numeric_limits<atx::f64>::quiet_NaN();
  x[32 * names + 130] = std::numeric_limits<atx::f64>::infinity();
  auto panel = Panel::create(dates, names, {"close"}, {x}, {});
  ASSERT_TRUE(panel);
  atx::engine::parallel::DetPool pool{2};
  Engine engine{*panel};
  engine.set_eval_mode(EvalMode::ResearchFast);
  engine.set_ts_pool(&pool);
  for (const OpCode op : {OpCode::TsDecayLinear, OpCode::TsWma, OpCode::TsSlope,
                          OpCode::TsRsquare, OpCode::TsResid}) {
    std::vector<atx::f64> expected(cells), actual(cells), window(cells, static_cast<atx::f64>(d));
    for (atx::usize j = 0; j < names; ++j)
      detail::tsv_welford_dispatch(op, x, expected, dates, j, d, names);
    Instr in{};
    in.op = op;
    in.src[0] = 0;
    in.src[1] = 1;
    in.dst = 2;
    const std::array<ExtSlot, 3> slots{{{x.data(), nullptr, cells},
                                      {window.data(), nullptr, cells},
                                      {actual.data(), actual.data(), cells}}};
    ASSERT_TRUE(engine.execute_range(in, slots, 0, names));
    for (atx::usize i = 0; i < cells; ++i)
      ASSERT_EQ(std::bit_cast<atx::u64>(actual[i]), std::bit_cast<atx::u64>(expected[i])) << i;
  }
}
} // namespace atxtest_alpha_ts_date_major
