#include "corrector.hpp"

#include <array>
#include <cmath>
#include <cstddef>
#include <limits>
#include <span>

#include <Eigen/Core>
#include <Eigen/LU>
#include <gtest/gtest.h>

namespace atx::vol::research {
namespace {

struct Fixture {
  std::array<double, 1> s{1.}, z{1.}, dx{-2.}, ds{-2.}, dz{0.};
  std::array<double, 1> rc{}, cx{}, cs{}, cz{};
  CentralityWork work;
  std::size_t calls = 0;
  double first_rc = 0.;

  CentralityDirection direction() { return {dx, ds, dz}; }
  CentralityScratch scratch() { return {rc, {cx, cs, cz}}; }

  static bool dense_kkt(void *context, std::span<const double> residual,
                        CentralityDirection output) {
    auto &state = *static_cast<Fixture *>(context);
    if (state.calls++ == 0) state.first_rc = residual[0];
    // Independent full KKT system, in (ddx,dds,ddz) order. No reduced-H
    // factor or candidate trial-product arithmetic is reused by this control.
    Eigen::Matrix3d kkt;
    kkt << 1., 0., -1., 1., -1., 0., 0., 1., 1.;
    const Eigen::Vector3d rhs(0., 0., -residual[0]);
    const Eigen::Vector3d delta = kkt.fullPivLu().solve(rhs);
    EXPECT_LT((kkt * delta - rhs).lpNorm<Eigen::Infinity>(), 1e-14);
    output.x[0] = delta[0];
    output.s[0] = delta[1];
    output.z[0] = delta[2];
    return true;
  }

  static bool worsening(void *context, std::span<const double>, CentralityDirection output) {
    ++static_cast<Fixture *>(context)->calls;
    output.x[0] = 8.;
    output.s[0] = -1.;
    output.z[0] = 0.;
    return true;
  }

  static bool nonfinite(void *context, std::span<const double>, CentralityDirection output) {
    ++static_cast<Fixture *>(context)->calls;
    output.x[0] = std::numeric_limits<double>::infinity();
    output.s[0] = 1.;
    output.z[0] = 1.;
    return true;
  }

  static bool weaker_dual(void *context, std::span<const double>, CentralityDirection output) {
    ++static_cast<Fixture *>(context)->calls;
    output.x[0] = 8.;
    output.s[0] = 1.;
    output.z[0] = -3.;
    return true;
  }

  static bool first_then_refused(void *context, std::span<const double> residual,
                                 CentralityDirection output) {
    auto &state = *static_cast<Fixture *>(context);
    if (state.calls == 0) return dense_kkt(context, residual, output);
    ++state.calls;
    output.x[0] = 999.; // A failed solve must not publish even finite partial scratch.
    return false;
  }

  bool run(HomogeneousSolve solve = dense_kkt, double target = .1) {
    return improve_centrality(s, z, target, direction(), scratch(), solve, this, work);
  }
};

TEST(JointCentrality, HomogeneousKktSignAndThreeSolveBound) {
  Fixture f;
  ASSERT_TRUE(f.run());
  // ap=.5, ad=1, trial product=-.2, lower target=.01 => rc=-.21.
  EXPECT_NEAR(f.first_rc, -.21, 1e-14);
  EXPECT_EQ(f.work.attempts, 3u);
  EXPECT_EQ(f.work.solves, 3u);
  EXPECT_EQ(f.calls, 3u);
  EXPECT_EQ(f.work.accepted, 3u);
  EXPECT_GT(-1. / f.ds[0], .53);
  EXPECT_EQ(f.dx[0], f.ds[0]);
  EXPECT_NEAR(f.dz[0], f.dx[0] + 2., 1e-14);
  EXPECT_EQ(f.s[0], 1.);
  EXPECT_EQ(f.z[0], 1.); // Current iterate is never the trial point.
}

struct CoupledFixture {
  std::array<double, 3> s{1., 1., 1.}, z{1., 1., 1.};
  std::array<double, 2> dx{-2., 0.}, cx{};
  std::array<double, 3> ds{-2., 0., -2.}, dz{}, rc{}, cs{}, cz{};
  std::size_t calls = 0;

  static Eigen::Matrix<double, 3, 2> constraints() {
    Eigen::Matrix<double, 3, 2> a;
    a << 1., 0., 0., 1., 1., -1.;
    return a;
  }

  static bool dense_kkt(void *context, std::span<const double> residual,
                        CentralityDirection output) {
    auto &state = *static_cast<CoupledFixture *>(context);
    const auto a = constraints();
    // Independently solve all eight primal, constraint and complementarity
    // equations. Unequal dimensions and coupling exercise every span join.
    Eigen::Matrix<double, 8, 8> kkt = Eigen::Matrix<double, 8, 8>::Zero();
    kkt.topLeftCorner<2, 2>().setIdentity();
    kkt.block<2, 3>(0, 5) = -a.transpose();
    kkt.block<3, 2>(2, 0) = a;
    kkt.block<3, 3>(2, 2) = -Eigen::Matrix3d::Identity();
    kkt.block<3, 3>(5, 2).setIdentity();
    kkt.bottomRightCorner<3, 3>().setIdentity();
    Eigen::Matrix<double, 8, 1> rhs = Eigen::Matrix<double, 8, 1>::Zero();
    rhs.tail<3>() = -Eigen::Map<const Eigen::Vector3d>(residual.data());
    const Eigen::Matrix<double, 8, 1> delta = kkt.fullPivLu().solve(rhs);
    EXPECT_LT((kkt * delta - rhs).lpNorm<Eigen::Infinity>(), 1e-14);
    if (state.calls++ == 0) {
      EXPECT_NEAR(residual[0], -.21, 1e-14);
      EXPECT_DOUBLE_EQ(residual[1], 0.);
      EXPECT_NEAR(residual[2], -.21, 1e-14);
      EXPECT_NEAR(delta[0], .13125, 1e-14);
      EXPECT_NEAR(delta[1], -.02625, 1e-14);
    }
    Eigen::Map<Eigen::Vector2d> x(output.x.data());
    Eigen::Map<Eigen::Vector3d> s(output.s.data()), z(output.z.data());
    x = delta.head<2>();
    s = delta.segment<3>(2);
    z = delta.tail<3>();
    return true;
  }
};

TEST(JointCentrality, CoupledUnequalDimensionsPreserveHomogeneousPrimalAndDualResiduals) {
  CoupledFixture f;
  CentralityWork work;
  ASSERT_TRUE(improve_centrality(f.s, f.z, .1, {f.dx, f.ds, f.dz},
      {f.rc, {f.cx, f.cs, f.cz}}, CoupledFixture::dense_kkt, &f, work));
  EXPECT_GT(work.accepted, 0u);
  EXPECT_LE(work.solves, 3u);
  EXPECT_EQ(f.calls, work.solves);
  const Eigen::Map<const Eigen::Vector2d> dx(f.dx.data());
  const Eigen::Map<const Eigen::Vector3d> ds(f.ds.data()), dz(f.dz.data());
  const auto a = CoupledFixture::constraints();
  EXPECT_LT((a * dx - ds).lpNorm<Eigen::Infinity>(), 1e-14);
  EXPECT_LT((dx - a.transpose() * dz - Eigen::Vector2d(-2., 0.)).
      lpNorm<Eigen::Infinity>(), 1e-14);
  const std::array<double, 3> original{1., 1., 1.};
  EXPECT_EQ(f.s, original);
  EXPECT_EQ(f.z, original);
}

TEST(JointCentrality, RejectingOneStepDiscardsTheWholeDirection) {
  Fixture f;
  ASSERT_TRUE(f.run(Fixture::worsening));
  EXPECT_EQ(f.calls, 1u);
  EXPECT_EQ(f.work.solves, 1u);
  EXPECT_EQ(f.work.accepted, 0u);
  EXPECT_EQ(f.work.nonimproving, 1u);
  EXPECT_EQ(f.dx[0], -2.);
  EXPECT_EQ(f.ds[0], -2.);
  EXPECT_EQ(f.dz[0], 0.);
}

TEST(JointCentrality, SaturatedOrAlreadyCentralTrialsNeedNoSolve) {
  Fixture saturated;
  saturated.ds[0] = -.5;
  ASSERT_TRUE(saturated.run());
  EXPECT_EQ(saturated.calls, 0u);
  EXPECT_EQ(saturated.work.attempts, 0u);
  EXPECT_EQ(saturated.ds[0], -.5);
  Fixture central;
  central.dz[0] = -2.; // trial product=.04 lies inside [.01,1].
  ASSERT_TRUE(central.run());
  EXPECT_EQ(central.calls, 0u);
  EXPECT_EQ(central.work.zero_target, 1u);
  EXPECT_EQ(central.ds[0], -2.);
  EXPECT_EQ(central.dz[0], -2.);
}

TEST(JointCentrality, NonfiniteCorrectionCannotPartiallyReplaceTheDirection) {
  Fixture f;
  ASSERT_TRUE(f.run(Fixture::nonfinite));
  EXPECT_EQ(f.work.solves, 1u);
  EXPECT_EQ(f.work.nonfinite, 1u);
  EXPECT_EQ(f.work.accepted, 0u);
  EXPECT_EQ(f.dx[0], -2.);
  EXPECT_EQ(f.ds[0], -2.);
  EXPECT_EQ(f.dz[0], 0.);
}

TEST(JointCentrality, ImprovingOnlyPrimalStillRejectsTheWholeCandidate) {
  Fixture f;
  ASSERT_TRUE(f.run(Fixture::weaker_dual));
  EXPECT_EQ(f.work.nonimproving, 1u);
  EXPECT_EQ(f.work.accepted, 0u);
  EXPECT_EQ(f.dx[0], -2.);
  EXPECT_EQ(f.ds[0], -2.);
  EXPECT_EQ(f.dz[0], 0.);
}

TEST(JointCentrality, FailedLaterSolveRetainsThePreviouslyAcceptedWholeDirection) {
  Fixture f;
  ASSERT_TRUE(f.run(Fixture::first_then_refused));
  EXPECT_EQ(f.work.attempts, 2u);
  EXPECT_EQ(f.work.solves, 2u);
  EXPECT_EQ(f.work.accepted, 1u);
  EXPECT_EQ(f.work.solve_refused, 1u);
  EXPECT_NEAR(f.dx[0], -1.895, 1e-14);
  EXPECT_EQ(f.ds[0], f.dx[0]);
  EXPECT_NEAR(f.dz[0], .105, 1e-14);
}

TEST(JointCentrality, UnrepresentableTargetsAndTrialProductsKeepTheOriginalDirection) {
  for (const double target : {0., -1., std::numeric_limits<double>::quiet_NaN(),
                              std::numeric_limits<double>::infinity(), 1e308,
                              std::numeric_limits<double>::denorm_min()}) {
    Fixture f;
    ASSERT_TRUE(f.run(Fixture::dense_kkt, target));
    EXPECT_EQ(f.work.invalid_target, 1u);
    EXPECT_EQ(f.work.solves, 0u);
    EXPECT_EQ(f.calls, 0u);
    EXPECT_EQ(f.dx[0], -2.);
    EXPECT_EQ(f.ds[0], -2.);
    EXPECT_EQ(f.dz[0], 0.);
  }
  Fixture f;
  f.s[0] = f.z[0] = 1e200;
  f.ds[0] = -2e200;
  ASSERT_TRUE(f.run());
  EXPECT_EQ(f.work.nonfinite, 1u);
  EXPECT_EQ(f.work.attempts, 1u);
  EXPECT_EQ(f.work.solves, 0u);
  EXPECT_EQ(f.calls, 0u);
  EXPECT_EQ(f.ds[0], -2e200);
}

TEST(JointCentrality, InvalidShapesOrIteratesRefuseBeforeTheSolverAndResetWork) {
  Fixture f;
  f.work.accepted = 11;
  auto malformed = f.scratch();
  malformed.residual = {};
  EXPECT_FALSE(improve_centrality(f.s, f.z, .1, f.direction(), malformed,
      Fixture::dense_kkt, &f, f.work));
  EXPECT_EQ(f.work.accepted, 0u);
  EXPECT_EQ(f.calls, 0u);
  EXPECT_FALSE(improve_centrality(f.s, f.z, .1, f.direction(), f.scratch(),
      nullptr, &f, f.work));
  for (const double value : {0., -1., std::numeric_limits<double>::infinity(),
                             std::numeric_limits<double>::quiet_NaN()}) {
    f.s[0] = value;
    EXPECT_FALSE(f.run());
    EXPECT_EQ(f.calls, 0u);
  }
  f.s[0] = 1.;
  f.dx[0] = std::numeric_limits<double>::quiet_NaN();
  EXPECT_FALSE(f.run());
  EXPECT_EQ(f.calls, 0u);
  EXPECT_EQ(f.ds[0], -2.);
  EXPECT_EQ(f.dz[0], 0.);
}

} // namespace
} // namespace atx::vol::research
