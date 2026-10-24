#include "thomas.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

#include "atx/vol/api/simd/cpu.hpp"
#include "pricing/american_discrete_div_reference.hpp"
#include "support/ddiv_matrix_cases.hpp"

namespace {
namespace ref = atx::vol::ddiv_ref;
using namespace ref::research;

struct CapturedSweep {
  Rows rows;
  int step = 0;
  bool fresh = false;
  std::vector<double> rhs, obstacle, cp, dp, work;
  std::vector<unsigned char> active;
};

struct Capture {
  unsigned segments = 0;
  bool need_policy = false;
  std::vector<CapturedSweep> sweeps;

  static void observe(const ref::PdeThomasObservation &o, void *opaque) {
    auto &c = *static_cast<Capture *>(opaque);
    if (o.fresh_data) {
      ++c.segments;
      c.need_policy = c.segments <= 2;
      if (!c.need_policy) return;
    } else {
      if (!c.need_policy) return;
      c.need_policy = false;
    }
    c.sweeps.push_back({{o.lo, o.di, o.up, o.bottom, o.top}, o.implicit_step, o.fresh_data,
        {o.rhs.begin(), o.rhs.end()}, {o.obstacle.begin(), o.obstacle.end()},
        {o.cp.begin(), o.cp.end()}, {o.dp.begin(), o.dp.end()}, {o.work.begin(), o.work.end()},
        {o.active.begin(), o.active.end()}});
  }
};

void verify_pack(const std::vector<CapturedSweep> &captures, std::size_t begin) {
  const std::size_t count = std::min(std::size_t{4}, captures.size() - begin);
  const auto size = captures[begin].rhs.size();
  ThomasPack p;
  p.rhs.resize(size); p.obstacle.resize(size); p.active.resize(size);
  p.cp.resize(size); p.dp.resize(size); p.work.resize(size);
  for (std::size_t lane = 0; lane < count; ++lane) {
    const auto &c = captures[begin + lane];
    ASSERT_EQ(c.rhs.size(), size);
    p.rows[lane] = c.rows;
    for (std::size_t i = 0; i < size; ++i) {
      p.rhs[i][lane] = c.rhs[i];
      p.obstacle[i][lane] = c.obstacle[i];
      p.active[i][lane] = c.active[i];
    }
  }
  auto scalar = p;
  const auto mask = static_cast<std::uint8_t>((1u << count) - 1u);
  const auto packed_status = solve_avx2(p, mask);
  const auto scalar_status = solve_scalar(scalar, mask);
  for (std::size_t lane = 0; lane < count; ++lane) {
    ASSERT_EQ(packed_status[lane], LaneStatus::Success);
    ASSERT_EQ(scalar_status[lane], LaneStatus::Success);
    const auto &c = captures[begin + lane];
    for (std::size_t i = 0; i < size; ++i) {
      for (std::size_t field = 0; field < 3; ++field) {
        const auto *expected = field == 0 ? &c.cp : field == 1 ? &c.dp : &c.work;
        const auto *packed = field == 0 ? &p.cp : field == 1 ? &p.dp : &p.work;
        const auto *control = field == 0 ? &scalar.cp : field == 1 ? &scalar.dp : &scalar.work;
        const auto bits = std::bit_cast<std::uint64_t>((*expected)[i]);
        ASSERT_EQ(std::bit_cast<std::uint64_t>((*packed)[i][lane]), bits)
            << "capture=" << begin + lane << " row=" << i << " field=" << field;
        ASSERT_EQ(std::bit_cast<std::uint64_t>((*control)[i][lane]), bits);
      }
    }
  }
}

TEST(CashThomasReplay, OriginalFiftyCaseThreeGridSweepsPreserveAllIntermediateBits) {
  if (!atx::vol::simd::have_avx2()) GTEST_SKIP() << "AVX2+FMA required";
  const auto cases = atx::vol::test::ddiv_matrix::matrix();
  ASSERT_EQ(cases.size(), 50u);
  std::size_t total = 0;
  for (int level = 0; level < 3; ++level) {
    std::vector<CapturedSweep> captures;
    for (const auto &c : cases) {
      SCOPED_TRACE(c.id + " grid=" + std::to_string(level));
      const ref::Problem problem{.S = c.S, .K = c.K, .T = c.T, .sigma = c.sigma,
          .r = c.r, .q = c.q, .side = c.side, .exercise = atx::vol::ExerciseStyle::American,
          .process = c.process, .dividends = c.divs};
      ref::PdeGrid grid;
      grid.space_base = 400 << level;
      grid.time_base = 200 << level;
      grid.width_sd = 8.;
      grid.time_grading = 2.;
      grid.rannacher_steps = 2;
      Capture captured;
      const ref::PdeThomasObserver observer{&Capture::observe, &captured};
      const auto actual = ref::pde_price(problem, grid, nullptr, &observer);
      ASSERT_TRUE(actual);
      const auto original = ref::pde_price(problem, grid);
      ASSERT_TRUE(original);
      EXPECT_EQ(actual->space_intervals, original->space_intervals);
      EXPECT_EQ(actual->time_steps, original->time_steps);
      EXPECT_EQ(actual->max_policy_iterations, original->max_policy_iterations);
      EXPECT_EQ(actual->lcp_rounding_ties, original->lcp_rounding_ties);
      for (const auto member : {&ref::PdeResult::price, &ref::PdeResult::delta,
                               &ref::PdeResult::gamma, &ref::PdeResult::max_policy_time,
                               &ref::PdeResult::max_lcp_violation}) {
        EXPECT_EQ(std::bit_cast<std::uint64_t>((*actual).*member),
                  std::bit_cast<std::uint64_t>((*original).*member));
      }
      const bool interior = !ref::event_schedule(c.T, c.divs).interior.empty();
      ASSERT_EQ(captured.sweeps.size(), interior ? 4u : 2u);
      ASSERT_TRUE(captured.sweeps[0].fresh);
      ASSERT_FALSE(captured.sweeps[1].fresh);
      ASSERT_EQ(captured.sweeps[0].step, captured.sweeps[1].step);
      for (auto &sweep : captured.sweeps) captures.push_back(std::move(sweep));
    }
    for (std::size_t begin = 0; begin < captures.size(); begin += 4u) {
      verify_pack(captures, begin);
    }
    total += captures.size();
    std::cout << "cash-thomas-replay grid=" << (400 << level)
              << " cases=50 captures=" << captures.size() << '\n';
  }
  EXPECT_GT(total, 0u);
}

} // namespace
