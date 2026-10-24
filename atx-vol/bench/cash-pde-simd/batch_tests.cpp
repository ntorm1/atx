#include "batch.hpp"

#include <gtest/gtest.h>

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <limits>
#include <string>
#include <vector>

#include "atx/vol/api/simd/cpu.hpp"
#include "support/ddiv_matrix_cases.hpp"

namespace {
namespace ref = atx::vol::ddiv_ref;
using namespace ref::research;
using atx::core::ErrorCode;

void same_result(const ref::PdeResult &a, const ref::PdeResult &b) {
  EXPECT_EQ(a.space_intervals, b.space_intervals);
  EXPECT_EQ(a.time_steps, b.time_steps);
  EXPECT_EQ(a.max_policy_iterations, b.max_policy_iterations);
  EXPECT_EQ(a.lcp_rounding_ties, b.lcp_rounding_ties);
  for (const auto field : {&ref::PdeResult::price, &ref::PdeResult::delta,
                          &ref::PdeResult::gamma, &ref::PdeResult::max_policy_time,
                          &ref::PdeResult::max_lcp_violation}) {
    EXPECT_EQ(std::bit_cast<std::uint64_t>(a.*field),
              std::bit_cast<std::uint64_t>(b.*field));
  }
}

void same_work(const ref::PdeDiagnostics &a, const ref::PdeDiagnostics &b) {
  EXPECT_EQ(a.thomas_sweeps, b.thomas_sweeps);
  EXPECT_EQ(a.fresh_seed_sweeps, b.fresh_seed_sweeps);
  EXPECT_EQ(a.forward_rows, b.forward_rows);
  EXPECT_EQ(a.continuation_rows, b.continuation_rows);
  EXPECT_EQ(a.identity_rows, b.identity_rows);
  EXPECT_EQ(a.continuation_runs, b.continuation_runs);
  EXPECT_EQ(a.exact_key_coefficient_rows, b.exact_key_coefficient_rows);
  EXPECT_EQ(a.continuation_run_lengths, b.continuation_run_lengths);
  ASSERT_EQ(a.coefficient_steps.size(), b.coefficient_steps.size());
  for (std::size_t i = 0; i < a.coefficient_steps.size(); ++i) {
    EXPECT_EQ(a.coefficient_steps[i].coefficient_bits, b.coefficient_steps[i].coefficient_bits);
    EXPECT_EQ(a.coefficient_steps[i].thomas_sweeps, b.coefficient_steps[i].thomas_sweeps);
    EXPECT_EQ(a.coefficient_steps[i].continuation_rows, b.coefficient_steps[i].continuation_rows);
    EXPECT_EQ(a.coefficient_steps[i].max_run_length, b.coefficient_steps[i].max_run_length);
  }
}

std::vector<ref::Problem> small_problems() {
  std::vector<ref::Problem> p(7, ref::Problem{.S = 100., .K = 100., .T = 1.,
      .sigma = .25, .r = .04, .q = .01, .dividends = {}});
  for (std::size_t i = 0; i < p.size(); ++i) {
    p[i].K += 3. * static_cast<double>(i);
    p[i].sigma += .01 * static_cast<double>(i);
    p[i].side = (i % 2u == 0) ? atx::vol::Side::Put : atx::vol::Side::Call;
  }
  return p;
}

TEST(CashPdeBatch, HeterogeneousModelsAndRetiredLanesMatchOriginal) {
  auto p = small_problems();
  const ref::PdeGrid grid{.space_base = 64, .time_base = 24};
  BatchDiagnostics work;
  const auto out = pde_price_batch(p, grid, &solve_scalar, &work);
  ASSERT_EQ(out.size(), p.size());
  ASSERT_EQ(work.lanes.size(), p.size());
  for (std::size_t i = 0; i < p.size(); ++i) {
    ref::PdeDiagnostics original_work;
    const auto original = ref::pde_price(p[i], grid, &original_work);
    ASSERT_TRUE(original); ASSERT_TRUE(out[i]) << i;
    same_result(*out[i], *original);
    same_work(work.lanes[i], original_work);
  }
  EXPECT_EQ(work.bundles, 2u);
  EXPECT_GT(work.tail_slots, 0u);
  EXPECT_GT(work.retired_slots, 0u);
  EXPECT_GT(work.mask_disagreement_rows, 0u);
  EXPECT_EQ(4u * work.vector_sweeps,
      work.active_lane_sweeps + work.tail_slots + work.retired_slots);
}

TEST(CashPdeBatch, PerInputRefusalsAndNullCallbackKeepEveryPosition) {
  auto p = small_problems();
  p[1].S = std::numeric_limits<double>::quiet_NaN();
  p[3].sigma = .005; p[3].r = .1; p[3].q = 0.;
  const ref::PdeGrid grid{.space_base = 64, .time_base = 24};
  BatchDiagnostics work;
  const auto out = pde_price_batch(p, grid, &solve_scalar, &work);
  ASSERT_EQ(out.size(), p.size());
  ASSERT_EQ(work.lanes.size(), p.size());
  for (std::size_t i = 0; i < p.size(); ++i) {
    const auto original = ref::pde_price(p[i], grid);
    ASSERT_EQ(out[i].has_value(), original.has_value()) << i;
    if (original) same_result(*out[i], *original);
    else EXPECT_EQ(out[i].error().code(), original.error().code());
  }
  const auto null = pde_price_batch(p, grid, nullptr, &work);
  ASSERT_EQ(null.size(), p.size());
  for (const auto &v : null) {
    ASSERT_FALSE(v); EXPECT_EQ(v.error().code(), ErrorCode::InvalidArgument);
  }
  EXPECT_EQ(work.bundles, 0u);
  EXPECT_TRUE(pde_price_batch({}, grid, &solve_scalar, &work).empty());
  EXPECT_TRUE(work.lanes.empty());
}

Statuses refuse_second(ThomasPack &p, std::uint8_t lanes) noexcept {
  auto status = solve_scalar(p, lanes);
  if ((lanes & 2u) != 0) status[1] = LaneStatus::NonfiniteResult;
  return status;
}

TEST(CashPdeBatch, KernelRefusalRetiresOnlyItsOwnLane) {
  auto p = small_problems(); p.resize(4);
  const ref::PdeGrid grid{.space_base = 64, .time_base = 24};
  BatchDiagnostics work;
  const auto out = pde_price_batch(p, grid, &refuse_second, &work);
  ASSERT_EQ(out.size(), 4u);
  ASSERT_EQ(work.lanes.size(), 4u);
  ASSERT_FALSE(out[1]);
  EXPECT_EQ(out[1].error().code(), ErrorCode::Internal);
  EXPECT_EQ(work.lanes[1].thomas_sweeps, 1u);
  for (const std::size_t i : {0u, 2u, 3u}) {
    const auto original = ref::pde_price(p[i], grid);
    ASSERT_TRUE(original); ASSERT_TRUE(out[i]); same_result(*out[i], *original);
  }
}

TEST(CashPdeBatch, ExactCalendarsAndExerciseStylesAreSeparateGroups) {
  auto p = small_problems(); p.resize(4);
  const std::array<atx::vol::CashDividend, 1> first{{{.tau = .5, .amount = 2.}}};
  const std::array<atx::vol::CashDividend, 1> second{{{.tau = .6, .amount = 2.}}};
  p[0].dividends = first; p[1].dividends = second;
  p[2].exercise = atx::vol::ExerciseStyle::European;
  const ref::PdeGrid grid{.space_base = 64, .time_base = 24};
  BatchDiagnostics work;
  const auto out = pde_price_batch(p, grid, &solve_scalar, &work);
  ASSERT_EQ(out.size(), 4u);
  EXPECT_EQ(work.bundles, 4u);
  for (std::size_t i = 0; i < p.size(); ++i) {
    const auto original = ref::pde_price(p[i], grid);
    ASSERT_TRUE(original); ASSERT_TRUE(out[i]); same_result(*out[i], *original);
  }
}

TEST(CashPdeBatch, CompleteFiftyCaseThreeGridResultsAndPoliciesMatchOriginal) {
  if (!atx::vol::simd::have_avx2()) GTEST_SKIP() << "AVX2+FMA required";
  const auto cases = atx::vol::test::ddiv_matrix::matrix();
  ASSERT_EQ(cases.size(), 50u);
  std::vector<ref::Problem> p;
  for (const auto &c : cases) {
    p.push_back({.S = c.S, .K = c.K, .T = c.T, .sigma = c.sigma,
        .r = c.r, .q = c.q, .side = c.side, .exercise = atx::vol::ExerciseStyle::American,
        .process = c.process, .dividends = c.divs});
  }
  for (int level = 0; level < 3; ++level) {
    const ref::PdeGrid grid{.space_base = 400 << level, .time_base = 200 << level};
    BatchDiagnostics work;
    const auto out = pde_price_batch(p, grid, &solve_avx2, &work);
    const auto null = pde_price_batch(p, grid, &solve_scalar);
    ASSERT_EQ(out.size(), 50u); ASSERT_EQ(null.size(), 50u);
    ASSERT_EQ(work.lanes.size(), 50u);
    std::uint64_t sweeps = 0;
    for (std::size_t i = 0; i < p.size(); ++i) {
      SCOPED_TRACE(cases[i].id + " grid=" + std::to_string(level));
      ref::PdeDiagnostics original_work;
      const auto original = ref::pde_price(p[i], grid, &original_work);
      ASSERT_TRUE(original); ASSERT_TRUE(out[i]); ASSERT_TRUE(null[i]);
      same_result(*out[i], *original); same_result(*null[i], *original);
      same_work(work.lanes[i], original_work);
      sweeps += original_work.thomas_sweeps;
    }
    // Seventeen calendar classes need 21 fixed-width bundles in input order.
    EXPECT_EQ(work.bundles, 21u);
    EXPECT_EQ(sweeps, work.active_lane_sweeps);
    EXPECT_EQ(4u * work.vector_sweeps,
        work.active_lane_sweeps + work.tail_slots + work.retired_slots);
    std::cout << "cash-pde-batch grid=" << grid.space_base << " cases=50 bundles="
        << work.bundles << " vector_sweeps=" << work.vector_sweeps
        << " useful=" << work.active_lane_sweeps << " tails=" << work.tail_slots
        << " retired=" << work.retired_slots << '\n';
  }
}

TEST(CashPdeBatch, ImplicitMatrixRefusalOfAnchorDoesNotChangeOtherLanes) {
  auto p = small_problems(); p.resize(4);
  p[0].sigma = .2; p[0].r = -300.; p[0].q = -300.02;
  p[2].r = -.02; p[2].q = -.04;
  const ref::PdeGrid grid;
  BatchDiagnostics work;
  const auto out = pde_price_batch(p, grid, &solve_scalar, &work);
  ASSERT_EQ(out.size(), 4u); ASSERT_EQ(work.lanes.size(), 4u);
  EXPECT_EQ(work.bundles, 1u);
  for (std::size_t i = 0; i < p.size(); ++i) {
    ref::PdeDiagnostics original_work;
    const auto original = ref::pde_price(p[i], grid, &original_work);
    ASSERT_EQ(out[i].has_value(), original.has_value()) << i;
    if (original) same_result(*out[i], *original);
    else {
      EXPECT_EQ(out[i].error().code(), original.error().code());
      EXPECT_NE(out[i].error().message().find("implicit rows"), std::string::npos);
    }
    same_work(work.lanes[i], original_work);
  }
  ASSERT_FALSE(out[0]);
}

TEST(CashPdeBatch, CanonicalSchedulesGroupWithoutQuantizingTimesOrCash) {
  auto p = small_problems(); p.resize(4);
  const std::array<atx::vol::CashDividend, 1> one{{{.tau = .5, .amount = 2.}}};
  const std::array<atx::vol::CashDividend, 3> split{{
      {.tau = .5, .amount = .25}, {.tau = .5, .amount = .75},
      {.tau = .7, .amount = 0.}}};
  const std::array<atx::vol::CashDividend, 1> later{{
      {.tau = std::bit_cast<double>(std::bit_cast<std::uint64_t>(.5) + 1u), .amount = 2.}}};
  p[0].dividends = one; p[1].dividends = split;
  p[2].dividends = later; p[3].dividends = one;
  p[3].process = ref::CashProcess::Escrowed;
  const ref::PdeGrid grid{.space_base = 64, .time_base = 24};
  BatchDiagnostics work;
  const auto out = pde_price_batch(p, grid, &solve_scalar, &work);
  ASSERT_EQ(out.size(), 4u);
  EXPECT_EQ(work.bundles, 2u);
  for (std::size_t i = 0; i < p.size(); ++i) {
    const auto original = ref::pde_price(p[i], grid);
    ASSERT_TRUE(original); ASSERT_TRUE(out[i]); same_result(*out[i], *original);
  }
}

} // namespace
