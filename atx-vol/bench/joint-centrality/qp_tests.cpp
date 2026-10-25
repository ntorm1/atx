#include "qp_control.hpp"
#include "fitting/joint_inbox_projection.hpp"

#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>

#include <Eigen/Core>
#include <Eigen/LU>
#include <gtest/gtest.h>

namespace atx::vol::research {
namespace {

struct HomogeneousFixture {
  std::array<double, 6> a{1., 0., 0., 1., 1., -1.};
  std::array<double, 3> s{2., .5, 3.}, z{.25, 2., 1.5}, rc{-.3, 0., 0.};
  std::array<double, 2> dx{17., -0.};
  std::array<double, 3> ds{23., -29., 31.}, dz{-37., 41., -43.};
  JointHomogeneousWork work{7, 9};

  JointHomogeneousInput input() const { return {2, a, s, z, rc}; }
  CentralityDirection output() { return {dx, ds, dz}; }
};

template <std::size_t N>
void expect_same_bits(const std::array<double, N> &actual,
                      const std::array<double, N> &expected) {
  for (std::size_t i = 0; i < N; ++i) {
    EXPECT_EQ(std::bit_cast<std::uint64_t>(actual[i]),
              std::bit_cast<std::uint64_t>(expected[i])) << "slot " << i;
  }
}

void expect_validation_refusal(HomogeneousFixture &f, const JointHomogeneousInput &input,
                               CentralityDirection output) {
  const auto dx = f.dx;
  const auto ds = f.ds;
  const auto dz = f.dz;
  EXPECT_FALSE(solve_joint_homogeneous_control(input, output, f.work));
  EXPECT_EQ(f.work.factors, 0u);
  EXPECT_EQ(f.work.directions, 0u);
  expect_same_bits(f.dx, dx);
  expect_same_bits(f.ds, ds);
  expect_same_bits(f.dz, dz);
}

// Independent unreduced KKT equations in (dx,ds,dz) order. This control does
// not reuse NewtonSystem, its reduced Hessian, or a candidate solve callback.
Eigen::VectorXd independent_kkt(const JointHomogeneousInput &input) {
  const auto n = static_cast<Eigen::Index>(input.variables);
  const auto m = static_cast<Eigen::Index>(input.s.size());
  const auto dimension = n + 2 * m;
  Eigen::MatrixXd kkt = Eigen::MatrixXd::Zero(dimension, dimension);
  Eigen::VectorXd rhs = Eigen::VectorXd::Zero(dimension);
  kkt.topLeftCorner(n, n).setIdentity();
  for (Eigen::Index row = 0; row < m; ++row) {
    const auto i = static_cast<std::size_t>(row);
    for (Eigen::Index col = 0; col < n; ++col) {
      const auto j = static_cast<std::size_t>(col);
      const double coefficient = input.a[i * input.variables + j];
      kkt(col, n + m + row) = -coefficient;
      kkt(n + row, col) = coefficient;
    }
    kkt(n + row, n + row) = -1.;
    kkt(n + m + row, n + row) = input.z[i];
    kkt(n + m + row, n + m + row) = input.s[i];
    rhs[n + m + row] = -input.rc[i];
  }
  const Eigen::FullPivLU<Eigen::MatrixXd> lu(kkt);
  EXPECT_EQ(lu.rank(), dimension);
  const Eigen::VectorXd expected = lu.solve(rhs);
  EXPECT_LT((kkt * expected - rhs).lpNorm<Eigen::Infinity>(), 1e-12);
  return expected;
}

void expect_kkt_solution(const JointHomogeneousInput &input, CentralityDirection output) {
  const auto expected = independent_kkt(input);
  const auto n = input.variables;
  const auto m = input.s.size();
  for (std::size_t j = 0; j < n; ++j) {
    EXPECT_NEAR(output.x[j], expected[static_cast<Eigen::Index>(j)], 1e-12);
    double dual = output.x[j];
    for (std::size_t i = 0; i < m; ++i) dual -= input.a[i * n + j] * output.z[i];
    EXPECT_NEAR(dual, 0., 1e-12);
  }
  for (std::size_t i = 0; i < m; ++i) {
    EXPECT_NEAR(output.s[i], expected[static_cast<Eigen::Index>(n + i)], 1e-12);
    EXPECT_NEAR(output.z[i], expected[static_cast<Eigen::Index>(n + m + i)], 1e-12);
    double primal = -output.s[i];
    for (std::size_t j = 0; j < n; ++j) primal += input.a[i * n + j] * output.x[j];
    EXPECT_NEAR(primal, 0., 1e-12);
    EXPECT_NEAR(input.z[i] * output.s[i] + input.s[i] * output.z[i],
                -input.rc[i], 1e-12);
  }
}

TEST(JointHomogeneousControl, CoupledNonunitIteratesMatchFullKktAndRationalPrimalDirection) {
  HomogeneousFixture f;
  const auto original = f;
  ASSERT_TRUE(solve_joint_homogeneous_control(f.input(), f.output(), f.work));
  EXPECT_EQ(f.work.factors, 1u);
  EXPECT_EQ(f.work.directions, 1u);
  EXPECT_NEAR(f.dx[0], 66. / 695., 1e-14);
  EXPECT_NEAR(f.dx[1], 6. / 695., 1e-14);
  expect_kkt_solution(f.input(), f.output());
  expect_same_bits(f.a, original.a);
  expect_same_bits(f.s, original.s);
  expect_same_bits(f.z, original.z);
  expect_same_bits(f.rc, original.rc);
}

TEST(JointHomogeneousControl, OppositeResidualNegatesAllThreeDirections) {
  HomogeneousFixture positive, negative;
  for (double &value : negative.rc) value = -value;
  ASSERT_TRUE(solve_joint_homogeneous_control(positive.input(), positive.output(), positive.work));
  ASSERT_TRUE(solve_joint_homogeneous_control(negative.input(), negative.output(), negative.work));
  for (std::size_t i = 0; i < positive.dx.size(); ++i) {
    EXPECT_NEAR(negative.dx[i], -positive.dx[i], 1e-14);
  }
  for (std::size_t i = 0; i < positive.ds.size(); ++i) {
    EXPECT_NEAR(negative.ds[i], -positive.ds[i], 1e-14);
    EXPECT_NEAR(negative.dz[i], -positive.dz[i], 1e-14);
  }
  expect_kkt_solution(negative.input(), negative.output());
}

TEST(JointHomogeneousControl, ZeroResidualPublishesCompleteZeroDirectionAndResetsWork) {
  HomogeneousFixture f;
  f.rc.fill(0.);
  ASSERT_TRUE(solve_joint_homogeneous_control(f.input(), f.output(), f.work));
  EXPECT_EQ(f.work.factors, 1u);
  EXPECT_EQ(f.work.directions, 1u);
  for (const double value : f.dx) EXPECT_EQ(value, 0.);
  for (const double value : f.ds) EXPECT_EQ(value, 0.);
  for (const double value : f.dz) EXPECT_EQ(value, 0.);
}

TEST(JointHomogeneousControl, DeclaredMaximumDimensionsMatchIndependentFullSystem) {
  std::array<double, 128> a{};
  std::array<double, 32> s{}, z{}, rc{}, ds{}, dz{};
  std::array<double, 4> dx{};
  for (std::size_t i = 0; i < s.size(); ++i) {
    s[i] = .5 + static_cast<double>(i % 7) * .25;
    z[i] = .25 + static_cast<double>(i % 5) * .5;
    rc[i] = (static_cast<double>(i % 3) - 1.) * .125;
    for (std::size_t j = 0; j < dx.size(); ++j) {
      a[i * dx.size() + j] = (static_cast<double>((i + 2 * j) % 5) - 2.) * .25;
    }
  }
  const JointHomogeneousInput input{dx.size(), a, s, z, rc};
  const CentralityDirection output{dx, ds, dz};
  JointHomogeneousWork work{17, 19};
  ASSERT_TRUE(solve_joint_homogeneous_control(input, output, work));
  EXPECT_EQ(work.factors, 1u);
  EXPECT_EQ(work.directions, 1u);
  expect_kkt_solution(input, output);
}

TEST(JointHomogeneousControl, MalformedInputShapesRefuseWithoutPublishingOrNumericalWork) {
  for (std::size_t mutation = 0; mutation < 10; ++mutation) {
    HomogeneousFixture f;
    auto input = f.input();
    switch (mutation) {
    case 0: input.variables = 0; break;
    case 1: input.variables = 5; break;
    case 2: input.variables = std::numeric_limits<std::size_t>::max(); break;
    case 3: input.a = input.a.first(5); break;
    case 4: input.a = {}; break;
    case 5: input.s = {}; break;
    case 6: input.s = input.s.first(2); break;
    case 7: input.z = input.z.first(2); break;
    case 8: input.rc = input.rc.first(2); break;
    case 9: input.z = {}; break;
    default: FAIL() << "unhandled input mutation";
    }
    SCOPED_TRACE(mutation);
    expect_validation_refusal(f, input, f.output());
  }
  HomogeneousFixture f;
  const std::array<double, 33> too_many{};
  auto input = f.input();
  input.s = too_many;
  input.z = too_many;
  input.rc = too_many;
  expect_validation_refusal(f, input, f.output());
}

TEST(JointHomogeneousControl, MalformedOutputShapesPreserveEveryOutputByte) {
  for (std::size_t mutation = 0; mutation < 6; ++mutation) {
    HomogeneousFixture f;
    auto output = f.output();
    switch (mutation) {
    case 0: output.x = {}; break;
    case 1: output.x = output.x.first(1); break;
    case 2: output.s = {}; break;
    case 3: output.s = output.s.first(2); break;
    case 4: output.z = {}; break;
    case 5: output.z = output.z.first(2); break;
    default: FAIL() << "unhandled output mutation";
    }
    SCOPED_TRACE(mutation);
    expect_validation_refusal(f, f.input(), output);
  }
}

TEST(JointHomogeneousControl, NonpositiveAndNonfiniteIteratesRefuseBeforeFactoring) {
  const std::array<double, 5> invalid{0., -1.,
      std::numeric_limits<double>::infinity(), -std::numeric_limits<double>::infinity(),
      std::numeric_limits<double>::quiet_NaN()};
  for (const double value : invalid) {
    for (std::size_t i = 0; i < 3; ++i) {
      HomogeneousFixture slack, dual;
      slack.s[i] = value;
      dual.z[i] = value;
      expect_validation_refusal(slack, slack.input(), slack.output());
      expect_validation_refusal(dual, dual.input(), dual.output());
    }
  }
}

TEST(JointHomogeneousControl, NonfiniteMatrixAndResidualRefuseBeforeFactoring) {
  const std::array<double, 3> invalid{std::numeric_limits<double>::infinity(),
      -std::numeric_limits<double>::infinity(), std::numeric_limits<double>::quiet_NaN()};
  for (const double value : invalid) {
    for (std::size_t i = 0; i < 6; ++i) {
      HomogeneousFixture f;
      f.a[i] = value;
      expect_validation_refusal(f, f.input(), f.output());
    }
    for (std::size_t i = 0; i < 3; ++i) {
      HomogeneousFixture f;
      f.rc[i] = value;
      expect_validation_refusal(f, f.input(), f.output());
    }
  }
}

TEST(JointHomogeneousControl, FiniteMatrixOverflowRefusesWithoutPublishingAnyDirection) {
  HomogeneousFixture f;
  f.a[0] = 1e200; // Finite input; A^T diag(z/s) A cannot be represented in binary64.
  const auto before = f;
  EXPECT_FALSE(solve_joint_homogeneous_control(f.input(), f.output(), f.work));
  EXPECT_EQ(f.work.factors, 1u); // Assembly refusal is an attempted numerical factor call.
  EXPECT_LE(f.work.directions, 1u);
  expect_same_bits(f.dx, before.dx);
  expect_same_bits(f.ds, before.ds);
  expect_same_bits(f.dz, before.dz);
  expect_same_bits(f.a, before.a);
  expect_same_bits(f.s, before.s);
  expect_same_bits(f.z, before.z);
  expect_same_bits(f.rc, before.rc);
}

constexpr AmericanPriceBounds kQpCarry{100., 1., 1., 98., 100.};

struct CrossingQpFixture {
  std::array<AmericanPriceNode, 3> near{{{90., 12., 2.}, {100., 7., 6.}, {110., 2., 12.}}};
  std::array<AmericanPriceNode, 3> far{{{90., 11., 3.}, {100., 5., 6.}, {110., 1., 13.}}};
  std::array<AmericanPriceBox, 3> boxes{{{10.5, 13., 1.5, 4.}, {4., 7., 4., 8.},
                                       {.5, 3., 11.5, 14.}}};

  JointProjectionResult run(detail::JointDirectionMode mode, detail::JointInboxProfile *profile,
                            detail::JointInboxDiagnostics *diagnostics = nullptr,
                            std::size_t budget = 100) {
    const std::array<detail::JointInboxSlice, 2> slices{{
        {near, boxes, kQpCarry}, {far, boxes, kQpCarry}}};
    return detail::project_joint_in_box(slices, budget, diagnostics, profile, mode);
  }
};

void expect_node_bits(const CrossingQpFixture &actual, const CrossingQpFixture &expected) {
  for (std::size_t i = 0; i < actual.near.size(); ++i) {
    const auto check = [](const AmericanPriceNode &a, const AmericanPriceNode &b) {
      expect_same_bits(std::array{a.strike, a.call, a.put},
                        std::array{b.strike, b.call, b.put});
    };
    check(actual.near[i], expected.near[i]);
    check(actual.far[i], expected.far[i]);
  }
}

void expect_result_equal(const JointProjectionResult &a, const JointProjectionResult &b) {
  EXPECT_EQ(a.outcome, b.outcome);
  EXPECT_EQ(a.sweeps, b.sweeps);
  EXPECT_EQ(a.conflict.kind, b.conflict.kind);
  EXPECT_EQ(a.conflict.slice, b.conflict.slice);
  EXPECT_EQ(a.conflict.node, b.conflict.node);
  EXPECT_EQ(a.conflict.other, b.conflict.other);
  expect_same_bits(std::array{a.conflict.strike, a.conflict.excess},
                    std::array{b.conflict.strike, b.conflict.excess});
}

auto centrality_counts(const detail::JointInboxProfile &p) {
  return std::array{p.centrality_attempts, p.centrality_solves, p.centrality_accepted,
      p.centrality_zero_target, p.centrality_nonfinite, p.centrality_nonimproving,
      p.centrality_solve_refused, p.centrality_invalid_target, p.centrality_input_refusals,
      p.centrality_setup_refusals, p.centrality_allocation_refusals};
}

void expect_no_centrality_work(const detail::JointInboxProfile &profile) {
  for (const auto count : centrality_counts(profile)) EXPECT_EQ(count, 0u);
  EXPECT_EQ(profile.centrality_workspace_bytes, 0u);
  EXPECT_EQ(profile.centrality_setup_ns, 0u);
  EXPECT_EQ(profile.centrality_ns, 0u);
}

void expect_certified_boxes_and_calendar(const CrossingQpFixture &f) {
  const auto near = AmericanPriceSlice::create(f.near, kQpCarry);
  const auto far = AmericanPriceSlice::create(f.far, kQpCarry);
  ASSERT_TRUE(near.has_value());
  ASSERT_TRUE(far.has_value());
  EXPECT_TRUE(check_american_calendar(*near, *far).has_value());
  for (std::size_t i = 0; i < f.boxes.size(); ++i) {
    for (const AmericanPriceNode node : {f.near[i], f.far[i]}) {
      EXPECT_GE(node.call, f.boxes[i].call_lo);
      EXPECT_LE(node.call, f.boxes[i].call_hi);
      EXPECT_GE(node.put, f.boxes[i].put_lo);
      EXPECT_LE(node.put, f.boxes[i].put_hi);
    }
  }
}

TEST(JointCentralityQp, LinkedModeAndOptionalProfilePreserveExactResultAndPrices) {
  ASSERT_TRUE(detail::joint_centrality_compiled());
  for (const std::size_t budget : {1u, 100u}) {
    SCOPED_TRACE(budget);
    CrossingQpFixture compiled, explicit_mode, unprofiled;
    detail::JointInboxProfile a, b;
    detail::JointInboxDiagnostics da, db, dc;
    const auto ra = compiled.run(detail::JointDirectionMode::Compiled, &a, &da, budget);
    const auto rb = explicit_mode.run(detail::JointDirectionMode::Centrality, &b, &db, budget);
    const auto rc = unprofiled.run(detail::JointDirectionMode::Centrality, nullptr, &dc, budget);
    expect_result_equal(ra, rb);
    expect_result_equal(ra, rc);
    expect_node_bits(compiled, explicit_mode);
    expect_node_bits(compiled, unprofiled);
    expect_same_bits(std::array{da.primal_residual, da.dual_residual, da.duality_gap},
                      std::array{db.primal_residual, db.dual_residual, db.duality_gap});
    expect_same_bits(std::array{da.primal_residual, da.dual_residual, da.duality_gap},
                      std::array{dc.primal_residual, dc.dual_residual, dc.duality_gap});
    EXPECT_EQ(da.variables, db.variables);
    EXPECT_EQ(da.constraints, db.constraints);
    EXPECT_EQ(da.certificate_passed, db.certificate_passed);
    EXPECT_TRUE(a.centrality_enabled);
    EXPECT_TRUE(b.centrality_enabled);
    EXPECT_EQ(centrality_counts(a), centrality_counts(b));
    EXPECT_EQ(a.centrality_workspace_bytes, b.centrality_workspace_bytes);
    EXPECT_EQ(a.factorizations, b.factorizations);
    EXPECT_EQ(a.directions, b.directions);
    EXPECT_EQ(ra.outcome, budget == 1 ? JointProjectionOutcome::NonConverged
                                    : JointProjectionOutcome::Feasible);
  }
}

TEST(JointCentralityQp, OriginalModeBypassesAllOptionalWorkAndRetainsFeasibleProjection) {
  CrossingQpFixture f;
  detail::JointInboxProfile profile;
  profile.centrality_attempts = 999;
  profile.centrality_workspace_bytes = 999;
  profile.centrality_enabled = true;
  detail::JointInboxDiagnostics diagnostics;
  ASSERT_EQ(f.run(detail::JointDirectionMode::Original, &profile, &diagnostics).outcome,
             JointProjectionOutcome::Feasible);
  EXPECT_FALSE(profile.centrality_enabled);
  expect_no_centrality_work(profile);
  EXPECT_GT(profile.factorizations, 0u);
  EXPECT_EQ(profile.directions, 2u * profile.factorizations);
  EXPECT_TRUE(diagnostics.certificate_passed);
  expect_certified_boxes_and_calendar(f);
}

TEST(JointCentralityQp, CorrectionsUseBoundedExtraSolvesAndPreserveEveryCertificate) {
  CrossingQpFixture f;
  detail::JointInboxProfile profile;
  detail::JointInboxDiagnostics diagnostics;
  ASSERT_EQ(f.run(detail::JointDirectionMode::Centrality, &profile, &diagnostics).outcome,
             JointProjectionOutcome::Feasible);
  EXPECT_TRUE(profile.centrality_enabled);
  EXPECT_GT(profile.centrality_workspace_bytes, 0u);
  EXPECT_GT(profile.centrality_attempts, 0u);
  EXPECT_LE(profile.centrality_attempts, 3u * profile.factorizations);
  EXPECT_LE(profile.centrality_solves, profile.centrality_attempts);
  EXPECT_LE(profile.centrality_accepted, profile.centrality_solves);
  EXPECT_EQ(profile.directions, 2u * profile.factorizations + profile.centrality_solves);
  EXPECT_LE(profile.directions, 5u * profile.factorizations);
  EXPECT_EQ(profile.centrality_allocation_refusals, 0u);
  EXPECT_EQ(profile.centrality_input_refusals, 0u);
  EXPECT_TRUE(diagnostics.certificate_passed);
  expect_certified_boxes_and_calendar(f);
}

TEST(JointCentralityQp, FeasibleAndZeroIterationEarlyReturnsAllocateNoCorrectionWorkspace) {
  for (const auto mode : {detail::JointDirectionMode::Original,
                          detail::JointDirectionMode::Centrality}) {
    std::array<AmericanPriceNode, 3> nodes{{{90., 12., 3.}, {100., 7., 8.}, {110., 2., 13.}}};
    const std::array<AmericanPriceBox, 3> boxes{{{11., 13., 2., 4.}, {6., 8., 7., 9.},
                                               {1., 3., 12., 14.}}};
    const std::array<detail::JointInboxSlice, 1> slices{{{nodes, boxes, kQpCarry}}};
    for (const std::size_t budget : {0u, 1u}) {
      const auto before = nodes;
      detail::JointInboxProfile profile;
      const auto result = detail::project_joint_in_box(slices, budget, nullptr, &profile, mode);
      EXPECT_EQ(result.outcome, budget == 0 ? JointProjectionOutcome::InvalidInput
                                          : JointProjectionOutcome::Feasible);
      EXPECT_EQ(result.sweeps, 0u);
      EXPECT_EQ(profile.factorizations, 0u);
      EXPECT_EQ(profile.directions, 0u);
      expect_no_centrality_work(profile);
      for (std::size_t i = 0; i < nodes.size(); ++i) {
        expect_same_bits(std::array{nodes[i].strike, nodes[i].call, nodes[i].put},
                          std::array{before[i].strike, before[i].call, before[i].put});
      }
    }
  }
}

TEST(JointCentralityQp, ExhaustedBudgetPreservesEveryOriginalNodeInBothModes) {
  for (const auto mode : {detail::JointDirectionMode::Original,
                          detail::JointDirectionMode::Centrality}) {
    CrossingQpFixture f;
    const auto before = f;
    detail::JointInboxProfile profile;
    detail::JointInboxDiagnostics diagnostics;
    const auto result = f.run(mode, &profile, &diagnostics, 1);
    EXPECT_EQ(result.outcome, JointProjectionOutcome::NonConverged);
    EXPECT_EQ(result.sweeps, 1u);
    EXPECT_FALSE(diagnostics.certificate_passed);
    EXPECT_EQ(profile.factorizations, 1u);
    expect_node_bits(f, before);
  }
}

TEST(JointCentralityQp, InvalidModeRefusesTransactionallyAndResetsDiagnostics) {
  CrossingQpFixture f;
  const auto before = f;
  detail::JointInboxProfile profile;
  profile.centrality_enabled = true;
  profile.centrality_attempts = 123;
  profile.centrality_workspace_bytes = 123;
  profile.factorizations = 123;
  profile.directions = 123;
  detail::JointInboxDiagnostics diagnostics{123, 123, 1., 2., 3., true};
  const auto result = f.run(static_cast<detail::JointDirectionMode>(999), &profile, &diagnostics);
  EXPECT_EQ(result.outcome, JointProjectionOutcome::InvalidInput);
  EXPECT_EQ(result.sweeps, 0u);
  expect_node_bits(f, before);
  EXPECT_FALSE(profile.centrality_enabled);
  expect_no_centrality_work(profile);
  EXPECT_EQ(profile.factorizations, 0u);
  EXPECT_EQ(profile.directions, 0u);
  EXPECT_EQ(diagnostics.variables, 0u);
  EXPECT_EQ(diagnostics.constraints, 0u);
  EXPECT_EQ(diagnostics.primal_residual, 0.);
  EXPECT_EQ(diagnostics.dual_residual, 0.);
  EXPECT_EQ(diagnostics.duality_gap, 0.);
  EXPECT_FALSE(diagnostics.certificate_passed);
}

} // namespace
} // namespace atx::vol::research
