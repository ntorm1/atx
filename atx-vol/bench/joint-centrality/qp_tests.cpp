#include "qp_control.hpp"

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

} // namespace
} // namespace atx::vol::research
