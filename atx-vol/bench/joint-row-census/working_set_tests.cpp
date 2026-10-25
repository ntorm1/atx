#include "fitting/joint_inbox_projection.hpp"
#include "fitting/counters.hpp"
#include "support/alloc_counter.hpp"
#include "../../tools/quote_bench_joint_working_set.hpp"

#include <Eigen/Core>
#include <Eigen/LU>
#include <gtest/gtest.h>

#include <array>
#include <cmath>
#include <cstddef>
#include <span>
#include <limits>

namespace atx::vol::detail {
namespace {
struct Small {
  std::array<AmericanPriceNode, 2> near{{{90.,12.,2.},{110.,3.,13.}}};
  std::array<AmericanPriceNode, 2> far{{{90.,11.,1.},{110.,2.,12.}}};
  const std::array<AmericanPriceBox, 2> boxes{{{10.,15.,0.,5.},{0.,6.,10.,16.}}};
  std::array<JointInboxSlice, 2> slices() {
    return {{{near,boxes,{100.,1.,1.,100.,100.}}, {far,boxes,{100.,1.,1.,100.,100.}}}};
  }
  Eigen::Matrix<double, 8, 1> values() const {
    Eigen::Matrix<double, 8, 1> x;
    x << near[0].call,near[0].put,near[1].call,near[1].put,
         far[0].call,far[0].put,far[1].call,far[1].put;
    return x;
  }
  void set(const Eigen::Matrix<double, 8, 1> &x) {
    near[0].call=x[0];near[0].put=x[1];near[1].call=x[2];near[1].put=x[3];
    far[0].call=x[4];far[0].put=x[5];far[1].call=x[6];far[1].put=x[7];
  }
  JointProjectionResult run(JointConstraintMode mode, JointWorkingSetWork &work,
                            JointInboxDiagnostics &d, JointInboxProfile *p=nullptr,
                            std::size_t budget=100, const JointWorkingSetControl *control=nullptr) {
    auto input=slices();
    return project_joint_in_box(input,budget,&d,p,JointDirectionMode::Original,
                                nullptr,nullptr,mode,&work,control);
  }
};

Eigen::Matrix<double,8,1> independent_witness(const Small &fixture) {
  // Independent dense KKT for the selected active equations: four put-call
  // parities and the two calendar ties. It constructs a feasible witness, not
  // a requirement that the feasibility-stopped native solve find its minimizer.
  Eigen::Matrix<double,6,8> a=Eigen::Matrix<double,6,8>::Zero();
  Eigen::Matrix<double,6,1> b;
  for (int i=0;i<4;++i) {a(i,2*i)=1.;a(i,2*i+1)=-1.;b[i]=(i%2==0)?10.:-10.;}
  a(4,4)=1.;a(4,0)=-1.;b[4]=0.;
  a(5,6)=1.;a(5,2)=-1.;b[5]=0.;
  Eigen::Matrix<double,14,14> kkt=Eigen::Matrix<double,14,14>::Zero();
  kkt.topLeftCorner<8,8>().setIdentity();
  kkt.topRightCorner<8,6>()=a.transpose();kkt.bottomLeftCorner<6,8>()=a;
  Eigen::Matrix<double,14,1> rhs;rhs.head<8>()=fixture.values();rhs.tail<6>()=b;
  const auto factor=kkt.fullPivLu();EXPECT_EQ(factor.rank(),14);
  const Eigen::Matrix<double,14,1> solved=factor.solve(rhs);
  EXPECT_LE((kkt*solved-rhs).template lpNorm<Eigen::Infinity>(),1e-12);
  return solved.head<8>();
}

void independently_feasible(const Eigen::Matrix<double,8,1> &x) {
  constexpr double tol=1e-9; // Full unnormalized tiny-system inequalities.
  for (int s=0;s<2;++s) {
    const int o=4*s;
    EXPECT_GE(x[o],10.-tol);EXPECT_LE(x[o],15.+tol);
    EXPECT_GE(x[o+1],-tol);EXPECT_LE(x[o+1],5.+tol);
    EXPECT_GE(x[o+2],-tol);EXPECT_LE(x[o+2],6.+tol);
    EXPECT_GE(x[o+3],10.-tol);EXPECT_LE(x[o+3],16.+tol);
    EXPECT_NEAR(x[o]-x[o+1],10.,tol);EXPECT_NEAR(x[o+2]-x[o+3],-10.,tol);
    EXPECT_GE(x[o]-x[o+2],-tol);EXPECT_LE(x[o]-x[o+2],20.+tol);
    EXPECT_GE(x[o+3]-x[o+1],-tol);EXPECT_LE(x[o+3]-x[o+1],20.+tol);
  }
  for (int i=0;i<4;++i) EXPECT_GE(x[i+4]-x[i],-tol);
}

TEST(JointWorkingSet, ReducedSelectionRetainsAllVariablesBoundsAndFullFeasibility) {
  ASSERT_TRUE(joint_working_set_compiled());
  Small fixture;independently_feasible(independent_witness(fixture));
  JointWorkingSetWork work;JointInboxDiagnostics d;JointInboxProfile p;
  const auto result=fixture.run(JointConstraintMode::WorkingSet,work,d,&p);
  ASSERT_EQ(result.outcome,JointProjectionOutcome::Feasible);
  independently_feasible(fixture.values());
  EXPECT_TRUE(work.enabled);EXPECT_EQ(work.full_variables,8u);
  EXPECT_EQ(work.full_rows,d.constraints);EXPECT_EQ(work.bound_rows,16u);
  EXPECT_EQ(work.initial_scan_rows,d.constraints);EXPECT_GT(work.initial_violated_rows,0u);
  ASSERT_GT(work.attempts,0u);EXPECT_LE(work.attempts,4u);
  EXPECT_GE(work.solves[0].selected_rows,16u);EXPECT_LT(work.solves[0].selected_rows,d.constraints);
  EXPECT_EQ(work.factorizations,p.factorizations);EXPECT_EQ(work.directions,p.directions);
  EXPECT_EQ(work.iterations,result.sweeps);EXPECT_GE(work.full_scan_rows,2*d.constraints);
}

TEST(JointWorkingSet, FullModeBypassesOptionalWorkAndKeepsOriginalDirections) {
  Small fixture;JointWorkingSetWork work;JointInboxDiagnostics d;JointInboxProfile p;
  ASSERT_EQ(fixture.run(JointConstraintMode::Full,work,d,&p).outcome,
            JointProjectionOutcome::Feasible);
  independently_feasible(fixture.values());
  EXPECT_FALSE(work.enabled);EXPECT_EQ(work.attempts,0u);EXPECT_EQ(work.workspace_bytes,0u);
  EXPECT_GT(p.factorizations,0u);EXPECT_EQ(p.directions,2*p.factorizations);
}

TEST(JointWorkingSet, CompiledAndExplicitUseSameSelectionWithOptionalProfile) {
  Small a,b;JointWorkingSetWork wa,wb;JointInboxDiagnostics da,db;JointInboxProfile p;
  const auto ra=a.run(JointConstraintMode::Compiled,wa,da);
  const auto rb=b.run(JointConstraintMode::WorkingSet,wb,db,&p);
  ASSERT_EQ(ra.outcome,JointProjectionOutcome::Feasible);ASSERT_EQ(rb.outcome,ra.outcome);
  independently_feasible(a.values());independently_feasible(b.values());
  EXPECT_GT(wa.attempts,0u);EXPECT_EQ(wa.attempts,wb.attempts);
  EXPECT_EQ(wa.directions,wb.directions);EXPECT_EQ(wa.full_scan_rows,wb.full_scan_rows);
}

TEST(JointWorkingSet, AlreadyFeasibleDoesNotAllocateOptionalWorkspace) {
  Small fixture;fixture.set(independent_witness(fixture));
  JointWorkingSetWork work;JointInboxDiagnostics d;
  const auto result=fixture.run(JointConstraintMode::WorkingSet,work,d);
  ASSERT_EQ(result.outcome,JointProjectionOutcome::Feasible);
  independently_feasible(fixture.values());
  EXPECT_EQ(result.sweeps,0u);EXPECT_EQ(work.attempts,0u);EXPECT_EQ(work.workspace_bytes,0u);
  EXPECT_EQ(work.initial_scan_rows,d.constraints);EXPECT_EQ(work.initial_violated_rows,0u);
}

TEST(JointWorkingSet, BudgetFailureUsesOneFullFallbackAndDoesNotPublishPartialNodes) {
  Small fixture;const auto before=fixture.values();JointWorkingSetWork work;JointInboxDiagnostics d;
  const auto result=fixture.run(JointConstraintMode::WorkingSet,work,d,nullptr,1);
  EXPECT_EQ(result.outcome,JointProjectionOutcome::NonConverged);
  EXPECT_EQ((fixture.values()-before).squaredNorm(),0.);
  EXPECT_EQ(work.reduced_solves,1u);EXPECT_EQ(work.full_solves,1u);EXPECT_EQ(work.attempts,2u);
  EXPECT_EQ(work.fallback_reason,JointWorkingReason::ReducedNonConverged);
  EXPECT_TRUE(work.solves[1].full_fallback);EXPECT_EQ(work.solves[1].selected_rows,d.constraints);
  EXPECT_LE(work.iterations,2u);EXPECT_EQ(result.sweeps,work.iterations);
}

TEST(JointWorkingSet, InvalidModeAndZeroBudgetAreTransactional) {
  for (const auto mode : {static_cast<JointConstraintMode>(99),JointConstraintMode::WorkingSet}) {
    Small fixture;const auto before=fixture.values();
    JointWorkingSetWork work;JointInboxDiagnostics d;
    const auto result=fixture.run(mode,work,d,nullptr,mode==JointConstraintMode::WorkingSet?0:100);
    EXPECT_EQ(result.outcome,JointProjectionOutcome::InvalidInput);
    EXPECT_EQ((fixture.values()-before).squaredNorm(),0.);EXPECT_EQ(work.attempts,0u);
    EXPECT_EQ(work.workspace_bytes,0u);
  }
}

TEST(JointWorkingSet, AggregateKeepsEveryInvocationAndLabelsOmittedSlotDetails) {
  Small a,b;JointWorkingSetWork wa,wb;JointInboxDiagnostics da,db;
  ASSERT_EQ(a.run(JointConstraintMode::WorkingSet,wa,da).outcome,
            JointProjectionOutcome::Feasible);
  static_cast<void>(b.run(JointConstraintMode::WorkingSet,wb,db,nullptr,1));
  JointWorkingSetAggregate aggregate;
  aggregate.add(wa);aggregate.add(wb);aggregate.add({});
  EXPECT_EQ(aggregate.invocations,2u);EXPECT_EQ(aggregate.slot_detail_invocations_omitted,1u);
  EXPECT_EQ(aggregate.attempts,wa.attempts+wb.attempts);
  EXPECT_EQ(aggregate.directions,wa.directions+wb.directions);
  EXPECT_EQ(aggregate.iterations,wa.iterations+wb.iterations);
  EXPECT_EQ(aggregate.full_scan_rows,wa.full_scan_rows+wb.full_scan_rows);
  EXPECT_EQ(aggregate.full_rows,da.constraints+db.constraints);
  EXPECT_EQ(aggregate.first.full_solves,wa.full_solves);
  EXPECT_EQ(aggregate.first.solves[0].selected_rows,wa.solves[0].selected_rows);
}

TEST(JointWorkingSet, ConservativeSyntheticRefusalsRunOneOriginalFallback) {
  const std::array faults{JointWorkingFault::ReducedRefusal,JointWorkingFault::ScanRefusal,
                         JointWorkingFault::NoProgress,JointWorkingFault::CertificateRefusal};
  const std::array reasons{JointWorkingReason::ReducedNonConverged,
                          JointWorkingReason::NonfiniteScan,
                          JointWorkingReason::NoProgress,JointWorkingReason::CertificateRefused};
  for(std::size_t i=0;i<faults.size();++i) {
    Small fixture;JointWorkingSetWork work;JointInboxDiagnostics d;JointInboxProfile p;
    JointWorkingSetControl control;control.fault=faults[i];
    ASSERT_EQ(fixture.run(JointConstraintMode::WorkingSet,work,d,&p,100,&control).outcome,
              JointProjectionOutcome::Feasible);
    independently_feasible(fixture.values());
    EXPECT_TRUE(work.synthetic_control);EXPECT_EQ(work.injected_refusals,1u);
    EXPECT_EQ(work.fallback_reason,reasons[i]);EXPECT_EQ(work.full_solves,1u);
    EXPECT_GE(work.reduced_solves,1u);EXPECT_LE(work.reduced_solves,3u);
    EXPECT_EQ(work.attempts,work.reduced_solves+1u);
    const auto &last=work.solves[work.attempts-1];
    EXPECT_TRUE(last.full_fallback);EXPECT_EQ(last.selected_rows,d.constraints);
    EXPECT_EQ(last.publication_scan_rows,d.constraints);EXPECT_LE(last.violated_rows,0u);
    EXPECT_EQ(work.directions,p.directions);EXPECT_EQ(work.factorizations,p.factorizations);
  }
}

TEST(JointWorkingSet, ReducedCapacityUsesOriginalSolveWithoutOptionalWorkspace) {
  Small fixture;JointWorkingSetWork work;JointInboxDiagnostics d;
  JointWorkingSetControl control;control.variables_limit=1;
  ASSERT_EQ(fixture.run(JointConstraintMode::WorkingSet,work,d,nullptr,100,&control).outcome,
            JointProjectionOutcome::Feasible);
  independently_feasible(fixture.values());
  EXPECT_TRUE(work.synthetic_control);EXPECT_EQ(work.capacity_refusals,1u);
  EXPECT_EQ(work.fallback_reason,JointWorkingReason::Capacity);
  EXPECT_EQ(work.reduced_solves,0u);EXPECT_EQ(work.full_solves,1u);
  EXPECT_EQ(work.workspace_bytes,0u);EXPECT_EQ(work.solves[0].selected_rows,d.constraints);
}

TEST(JointWorkingSet, RealOptionalAllocationFailurePreservesFullFallback) {
  Small fixture;JointWorkingSetWork work;JointInboxDiagnostics d;
  JointWorkingSetControl control;
  control.before_optional_allocation=[](void *) {
    testkit::alloc_counter_arm();testkit::alloc_counter_fail_once_at(1);
  };
  const auto result=fixture.run(JointConstraintMode::WorkingSet,work,d,nullptr,100,&control);
  const auto allocations=testkit::alloc_counter_disarm();
  ASSERT_EQ(result.outcome,JointProjectionOutcome::Feasible);
  independently_feasible(fixture.values());
  EXPECT_GT(allocations,1u);EXPECT_EQ(work.allocation_refusals,1u);
  EXPECT_EQ(work.fallback_reason,JointWorkingReason::OptionalAllocation);
  EXPECT_EQ(work.reduced_solves,0u);EXPECT_EQ(work.full_solves,1u);
  EXPECT_EQ(work.attempts,1u);EXPECT_GT(work.workspace_bytes,0u);
}

TEST(JointWorkingSet, FixedBoundaryOnlyDataKeepsNoFreeVariablesAndNoWorkspace) {
  std::array<AmericanPriceNode,2> a{{{90.,12.,2.},{110.,3.,13.}}},b=a;
  const std::array<AmericanPriceBox,2> box{{{12.,12.,2.,2.},{3.,3.,13.,13.}}};
  const std::array<JointInboxSlice,2> input{{{a,box,{100.,1.,1.,100.,100.}},
                                         {b,box,{100.,1.,1.,100.,100.}}}};
  JointWorkingSetWork work;JointInboxDiagnostics d;
  const auto result=project_joint_in_box(input,100,&d,nullptr,JointDirectionMode::Original,
                                       nullptr,nullptr,JointConstraintMode::WorkingSet,&work);
  EXPECT_EQ(result.outcome,JointProjectionOutcome::Feasible);EXPECT_TRUE(d.certificate_passed);
  EXPECT_EQ(work.full_variables,0u);EXPECT_EQ(work.full_rows,0u);EXPECT_EQ(work.attempts,0u);
  EXPECT_EQ(work.workspace_bytes,0u);EXPECT_EQ(result.sweeps,0u);
  EXPECT_EQ(a[0].call,12.);EXPECT_EQ(b[1].put,13.);
}

TEST(JointWorkingSet, ActualProjectionCountersChargeOnceAndAccumulateRepeatedCalls) {
  namespace ft=counters::fit_timing;
  ft::ChainRecorder first,other;
  JointWorkingSetWork wa,wb,wc;JointInboxDiagnostics d;
  {
    const ft::ChainScope outer(&first);
    Small a,b,c;
    ASSERT_EQ(a.run(JointConstraintMode::WorkingSet,wa,d).outcome,JointProjectionOutcome::Feasible);
    {
      const ft::ChainScope inner(&other);
      ASSERT_EQ(b.run(JointConstraintMode::Full,wb,d).outcome,JointProjectionOutcome::Feasible);
    }
    ASSERT_EQ(c.run(JointConstraintMode::WorkingSet,wc,d).outcome,JointProjectionOutcome::Feasible);
  }
  EXPECT_EQ(first.counts.joint_working_set.invocations,2u);
  EXPECT_EQ(first.counts.joint_working_set.slot_detail_invocations_omitted,1u);
  EXPECT_EQ(first.counts.joint_working_set.directions,wa.directions+wc.directions);
  EXPECT_EQ(first.counts.joint_working_set.attempts,wa.attempts+wc.attempts);
  EXPECT_EQ(other.counts.joint_working_set.invocations,0u);
}

TEST(JointWorkingSet, InvalidNumericalInputRefusesWithoutMutationOrOptionalWork) {
  Small fixture;fixture.near[0].call=std::numeric_limits<double>::quiet_NaN();
  JointWorkingSetWork work;JointInboxDiagnostics d;
  const auto result=fixture.run(JointConstraintMode::WorkingSet,work,d);
  EXPECT_EQ(result.outcome,JointProjectionOutcome::InvalidInput);
  EXPECT_TRUE(std::isnan(fixture.near[0].call));EXPECT_EQ(fixture.far[0].call,11.);
  EXPECT_EQ(work.attempts,0u);EXPECT_EQ(work.workspace_bytes,0u);
}

Small omitted_rows_fixture() {
  Small fixture;
  Eigen::Matrix<double,8,1> x;
  // Frozen first case of a bounded tiny-fixture authoring search. No SPY data.
  x << 11.515000000000001,4.6699999999999999,2.8919999999999999,12.651999999999999,
       14.955,1.0349999999999999,5.8620000000000001,16.;
  fixture.set(x);
  return fixture;
}

TEST(JointWorkingSet, NaturalOmittedRowsRequireAnotherReducedSolve) {
  auto fixture=omitted_rows_fixture();
  independently_feasible(independent_witness(fixture));
  JointWorkingSetWork work;JointInboxDiagnostics d;
  ASSERT_EQ(fixture.run(JointConstraintMode::WorkingSet,work,d).outcome,
            JointProjectionOutcome::Feasible);
  independently_feasible(fixture.values());
  EXPECT_FALSE(work.synthetic_control);EXPECT_EQ(work.injected_refusals,0u);
  ASSERT_GE(work.reduced_solves,2u);EXPECT_EQ(work.full_solves,0u);
  ASSERT_GT(work.solves[0].violated_rows,0u);ASSERT_GT(work.solves[0].newly_added_rows,0u);
  EXPECT_EQ(work.solves[1].selected_rows,
            work.solves[0].selected_rows+work.solves[0].newly_added_rows);
  EXPECT_EQ(work.solves[work.attempts-1].violated_rows,0u);
  EXPECT_EQ(work.solves[work.attempts-1].publication_scan_rows,d.constraints);
  EXPECT_EQ(work.fallback_reason,JointWorkingReason::None);
}

TEST(JointWorkingSet, LowerTestRoundCapFallsBackAfterActualOmittedRowViolation) {
  auto fixture=omitted_rows_fixture();
  JointWorkingSetWork work;JointInboxDiagnostics d;JointInboxProfile p;
  JointWorkingSetControl control;control.reduced_rounds_limit=1;
  ASSERT_EQ(fixture.run(JointConstraintMode::WorkingSet,work,d,&p,100,&control).outcome,
            JointProjectionOutcome::Feasible);
  independently_feasible(fixture.values());
  EXPECT_TRUE(work.synthetic_control);EXPECT_EQ(work.injected_refusals,0u);
  EXPECT_EQ(work.reduced_solves,1u);EXPECT_EQ(work.full_solves,1u);
  EXPECT_EQ(work.fallback_reason,JointWorkingReason::RoundLimit);
  EXPECT_GT(work.solves[0].violated_rows,0u);EXPECT_GT(work.solves[0].newly_added_rows,0u);
  EXPECT_TRUE(work.solves[1].full_fallback);EXPECT_EQ(work.solves[1].selected_rows,d.constraints);
  EXPECT_EQ(work.solves[1].violated_rows,0u);EXPECT_EQ(work.directions,p.directions);
}

TEST(JointWorkingSet, ExportActualWorkRecordsForIndependentReader) {
  using Json=nlohmann::json;
  Json records=Json::array();
  for(const auto budget : {1u,100u}) {
    auto fixture=omitted_rows_fixture();JointWorkingSetWork work;JointInboxDiagnostics d;
    static_cast<void>(fixture.run(JointConstraintMode::WorkingSet,work,d,nullptr,budget));
    JointWorkingSetAggregate aggregate;aggregate.add(work);
    const auto report=quote_bench_joint_working_set::report(aggregate);
    EXPECT_EQ(report.at("attempts"),work.attempts);
    EXPECT_EQ(report.at("first_invocation").at("solves").size(),work.attempts);
    records.push_back({{"budget",budget},{"work",report}});
  }
  RecordProperty("work_records",records.dump());
}

} // namespace
} // namespace atx::vol::detail
