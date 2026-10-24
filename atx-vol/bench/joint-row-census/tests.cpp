#include "fitting/joint_row_census.hpp"
#include "fitting/raw_price_draft.hpp"
#include "support/alloc_counter.hpp"
#include "../../tools/quote_bench_joint_row_census.hpp"

#include <array>
#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <span>
#include <string>
#include <thread>
#include <type_traits>
#include <vector>

#include <gtest/gtest.h>

namespace atx::vol::research {
namespace {
struct Fixture {
  std::array<AmericanPriceNode, 3> near{{{90., 12., 2.}, {100., 7., 6.}, {110., 2., 12.}}};
  std::array<AmericanPriceNode, 3> far{{{90., 11., 3.}, {100., 5., 6.}, {110., 1., 13.}}};
  std::array<AmericanPriceNode, 3> raw_near{{{90., 11.75, 2.}, {100., 6.75, 6.}, {110., 2., 12.}}};
  std::array<AmericanPriceNode, 3> raw_far{{{90., 10.75, 3.}, {100., 4.75, 6.}, {110., 1., 13.}}};
  std::array<AmericanPriceBox, 3> boxes{{{10.5, 13., 1.5, 4.}, {4., 7., 4., 8.},
                                       {.5, 3., 11.5, 14.}}};
  static std::span<const AmericanPriceNode> raw(const void *p, std::size_t i) noexcept {
    const auto &self = *static_cast<const Fixture *>(p);
    return i == 0 ? std::span(self.raw_near) : std::span(self.raw_far);
  }
  JointProjectionResult run(JointRowObserver *observer, detail::JointInboxProfile *profile,
                            detail::JointInboxDiagnostics *diagnostics = nullptr,
                            std::size_t budget = 100, bool raw_enabled = true) {
    const AmericanPriceBounds bounds{100., 1., 1., 98., 100.};
    const std::array<detail::JointInboxSlice, 2> slices{{
        {near, boxes, bounds}, {far, boxes, bounds}}};
    const JointRowRawSource raw_source{this, raw};
    return detail::project_joint_in_box(slices, budget, diagnostics, profile,
        detail::JointDirectionMode::Original, observer, raw_enabled ? &raw_source : nullptr);
  }
};
struct Storage {
  std::array<JointCapturedSlice, 2> slices;
  std::array<JointCapturedNode, 6> nodes;
  std::array<JointCapturedCoordinate, 12> coordinates;
  std::array<JointCapturedVariable, 12> variables;
  std::array<JointCapturedRow, 100> rows;
  JointRowStorage view() { return {slices, nodes, coordinates, variables, rows}; }
};
void maybe_export(const JointRowRecorder &recorder, const char *suffix) {
  char *output = nullptr;
  std::size_t output_size = 0;
  ASSERT_EQ(_dupenv_s(&output, &output_size, "ATX_VOL_JOINT_CENSUS_FIXTURE_OUTPUT"), 0);
  const std::string output_path = output ? output : "";
  std::free(output);
  if (!output_path.empty())
    quote_bench_joint_row_census::write(output_path + suffix,
                                      quote_bench_joint_row_census::report(recorder));
}
void same_double(double a, double b) {
  EXPECT_EQ(std::bit_cast<std::uint64_t>(a), std::bit_cast<std::uint64_t>(b));
}
void same_nodes(const Fixture &a, const Fixture &b) {
  std::size_t differences = 0;
  const auto check = [&](double x, double y) {
    differences += std::bit_cast<std::uint64_t>(x) != std::bit_cast<std::uint64_t>(y);
    EXPECT_NEAR(x, y, .001); // All fixture spreads exceed .10; selected economic cap.
  };
  for (std::size_t i = 0; i < a.near.size(); ++i) {
    check(a.near[i].call, b.near[i].call);
    check(a.near[i].put, b.near[i].put);
    check(a.far[i].call, b.far[i].call);
    check(a.far[i].put, b.far[i].put);
  }
  ::testing::Test::RecordProperty("price_bit_differences_diagnostic", std::to_string(differences));
  const AmericanPriceBounds bounds{100., 1., 1., 98., 100.};
  const auto near = AmericanPriceSlice::create(a.near, bounds);
  const auto far = AmericanPriceSlice::create(a.far, bounds);
  ASSERT_TRUE(near); ASSERT_TRUE(far);
  EXPECT_TRUE(check_american_calendar(*near, *far));
}
TEST(JointRowCensus, FullRowsTargetsAndCertifiedPublicationAreOwnedAfterReturn) {
  ASSERT_TRUE(joint_row_census_compiled());
  Storage storage;
  JointRowRecorder recorder(storage.view());
  Fixture input;
  const Fixture before = input;
  detail::JointInboxDiagnostics diagnostics;
  detail::JointInboxProfile profile;
  {
    JointRowCaptureScope scope(recorder.observer());
    const auto result = input.run(nullptr, &profile, &diagnostics);
    EXPECT_EQ(result.outcome, JointProjectionOutcome::Feasible);
    EXPECT_GT(result.sweeps, 1u);
  }
  ASSERT_TRUE(recorder.observer().valid());
  const auto data = recorder.data();
  ASSERT_EQ(data.slices.size(), 2u);
  ASSERT_EQ(data.nodes.size(), 6u);
  ASSERT_EQ(data.coordinates.size(), 12u);
  ASSERT_EQ(data.variables.size(), diagnostics.variables);
  ASSERT_EQ(data.rows.size(), diagnostics.constraints);
  EXPECT_EQ(recorder.iteration_budget(), 100u);
  same_double(recorder.tolerance(), 1e-10);
  EXPECT_TRUE(recorder.diagnostics().certificate_passed);
  EXPECT_TRUE(recorder.has_profile());
  EXPECT_EQ(recorder.profile().factorizations, profile.factorizations);
  for (std::size_t i = 0; i < 3; ++i) {
    same_double(data.nodes[i].selected_call, before.near[i].call);
    same_double(data.nodes[i].raw_call, before.raw_near[i].call);
    same_double(data.nodes[i].published_call, input.near[i].call);
    same_double(data.nodes[i+3].selected_put, before.far[i].put);
    same_double(data.nodes[i+3].raw_put, before.raw_far[i].put);
    same_double(data.nodes[i+3].published_put, input.far[i].put);
  }
  for (std::size_t j = 0; j < data.variables.size(); ++j) {
    EXPECT_EQ(data.coordinates[j].variable, static_cast<int>(j));
    same_double(data.variables[j].initial, data.variables[j].target);
    const auto &lo = data.rows[data.rows.size() - 2 * data.variables.size() + 2 * j];
    const auto &hi = data.rows[data.rows.size() - 2 * data.variables.size() + 2 * j + 1];
    EXPECT_EQ(lo.count, 1u);
    EXPECT_EQ(lo.index[0], static_cast<int>(j));
    same_double(lo.coefficient[0], 1.);
    same_double(lo.rhs, data.variables[j].lower);
    same_double(hi.coefficient[0], -1.);
    same_double(hi.rhs, -data.variables[j].upper);
  }
  const double retained = data.nodes[0].selected_call;
  input = {};
  same_double(data.nodes[0].selected_call, retained);
}

TEST(JointRowCensus, NullProfileAndCapturePreservePriceContractWorkAndAllocations) {
  Fixture plain, profiled, captured;
  detail::JointInboxProfile a, b;
  detail::JointInboxDiagnostics da, db;
  const auto result_plain = plain.run(nullptr, nullptr);
  JointProjectionResult result_a, result_b;
  const auto allocations_a = testkit::count_allocations([&] {
    result_a = profiled.run(nullptr, &a, &da);
  });
  Storage storage;
  JointRowRecorder recorder(storage.view());
  std::uint64_t allocations_b = 0;
  {
    JointRowCaptureScope scope(recorder.observer());
    allocations_b = testkit::count_allocations([&] {
      result_b = captured.run(nullptr, &b, &db);
    });
  }
  EXPECT_TRUE(recorder.observer().valid());
  EXPECT_EQ(result_plain.outcome, JointProjectionOutcome::Feasible);
  EXPECT_EQ(result_a.outcome, result_plain.outcome);
  EXPECT_EQ(result_b.outcome, result_plain.outcome);
  EXPECT_EQ(result_a.sweeps, result_plain.sweeps);
  EXPECT_EQ(result_b.sweeps, result_plain.sweeps);
  same_nodes(plain, profiled);
  same_nodes(plain, captured);
  EXPECT_EQ(allocations_a, allocations_b);
  EXPECT_EQ(a.factorizations, b.factorizations);
  EXPECT_EQ(a.directions, b.directions);
  EXPECT_EQ(a.matrix_nonzeros, b.matrix_nonzeros);
  EXPECT_EQ(a.factor_nonzeros, b.factor_nonzeros);
  EXPECT_EQ(a.row_arities, b.row_arities);
  same_double(da.primal_residual, db.primal_residual);
  same_double(da.dual_residual, db.dual_residual);
  same_double(da.duality_gap, db.duality_gap);
}

TEST(JointRowCensus, CapacityFailureInvalidatesOnlyObservation) {
  Fixture expected, observed;
  const auto baseline = expected.run(nullptr, nullptr);
  Storage storage;
  auto short_storage = storage.view();
  short_storage.rows = short_storage.rows.first(1);
  JointRowRecorder recorder(short_storage);
  JointProjectionResult answer;
  {
    JointRowCaptureScope scope(recorder.observer());
    answer = observed.run(nullptr, nullptr);
  }
  EXPECT_FALSE(recorder.observer().valid());
  EXPECT_EQ(recorder.observer().error(), JointRowCaptureError::Capacity);
  EXPECT_EQ(answer.outcome, baseline.outcome);
  EXPECT_EQ(answer.sweeps, baseline.sweeps);
  same_nodes(observed, expected);
}

TEST(JointRowCensus, EveryStorageCapacityAndMissingRawSourceRefuseOnlyCapture) {
  Fixture expected;
  const auto baseline = expected.run(nullptr, nullptr);
  for (unsigned field = 0; field < 6; ++field) {
    Fixture observed;
    Storage storage;
    auto capacity = storage.view();
    if (field == 0) capacity.slices = {};
    if (field == 1) capacity.nodes = {};
    if (field == 2) capacity.coordinates = {};
    if (field == 3) capacity.variables = {};
    if (field == 4) capacity.rows = {};
    JointRowRecorder recorder(capacity);
    JointProjectionResult answer;
    {
      JointRowCaptureScope scope(recorder.observer());
      answer = observed.run(nullptr, nullptr, nullptr, 100, field != 5);
    }
    EXPECT_FALSE(recorder.observer().valid());
    EXPECT_EQ(recorder.observer().error(), field == 5 ? JointRowCaptureError::MissingRawTargets
                                                     : JointRowCaptureError::Capacity);
    EXPECT_EQ(answer.outcome, baseline.outcome);
    same_nodes(observed, expected);
  }
}

TEST(JointRowCensus, CallbackExceptionsAreContainedAtBothPhases) {
  static_assert(std::is_const_v<typename decltype(JointRowSliceView{}.nodes)::element_type>);
  Fixture expected;
  expected.run(nullptr, nullptr);
  for (const bool fail_built : {false, true}) {
    Fixture observed;
    bool built_phase = fail_built;
    JointRowObserver observer(&built_phase,
        [](void *p, const JointRowBuiltView &) {
          if (*static_cast<bool *>(p)) throw 17; // synthetic callback exception, no allocation.
          return JointRowCaptureError::None;
        }, [](void *p, const JointRowPublishedView &) {
          if (!*static_cast<bool *>(p)) throw 23;
          return JointRowCaptureError::None;
        });
    {
      JointRowCaptureScope scope(observer);
      EXPECT_EQ(observed.run(nullptr, nullptr).outcome, JointProjectionOutcome::Feasible);
    }
    EXPECT_FALSE(observer.valid());
    EXPECT_EQ(observer.error(), JointRowCaptureError::CallbackException);
    same_nodes(observed, expected);
  }
}

TEST(JointRowCensus, ReentrantCallbackCannotRecurseOrChangeEitherFit) {
  struct Context { Fixture nested; std::size_t callbacks = 0; } context;
  JointRowObserver observer(&context, [](void *p, const JointRowBuiltView &) {
    auto &c = *static_cast<Context *>(p);
    ++c.callbacks;
    const auto answer = c.nested.run(nullptr, nullptr);
    EXPECT_EQ(answer.outcome, JointProjectionOutcome::Feasible);
    return JointRowCaptureError::None;
  }, [](void *, const JointRowPublishedView &) { return JointRowCaptureError::None; });
  Fixture observed, expected;
  expected.run(nullptr, nullptr);
  {
    JointRowCaptureScope scope(observer);
    EXPECT_EQ(observed.run(nullptr, nullptr).outcome, JointProjectionOutcome::Feasible);
  }
  EXPECT_EQ(context.callbacks, 1u);
  EXPECT_EQ(observer.attempts(), 2u);
  EXPECT_EQ(observer.error(), JointRowCaptureError::Reentrant);
  EXPECT_FALSE(observer.valid());
  same_nodes(observed, expected);
  same_nodes(context.nested, expected);
}

TEST(JointRowCensus, NestedAndReusedScopesFailWithoutReplacingOwner) {
  Storage first, second;
  JointRowRecorder a(first.view()), b(second.view());
  Fixture input;
  {
    JointRowCaptureScope scope(a.observer());
    ASSERT_TRUE(scope.attached());
    JointRowCaptureScope nested(b.observer());
    EXPECT_FALSE(nested.attached());
    EXPECT_EQ(input.run(nullptr, nullptr).outcome, JointProjectionOutcome::Feasible);
  }
  EXPECT_EQ(a.observer().error(), JointRowCaptureError::NestedScope);
  EXPECT_EQ(b.observer().error(), JointRowCaptureError::NestedScope);
  EXPECT_FALSE(a.observer().valid());
  EXPECT_FALSE(b.observer().valid());
  Storage fresh;
  JointRowRecorder once(fresh.view());
  {
    JointRowCaptureScope scope(once.observer());
    Fixture valid;
    valid.run(nullptr, nullptr);
  }
  ASSERT_TRUE(once.observer().valid());
  JointRowCaptureScope again(once.observer());
  EXPECT_FALSE(again.attached());
  EXPECT_EQ(once.observer().error(), JointRowCaptureError::ReusedScope);
}

TEST(JointRowCensus, ForeignProjectionInvalidatesReceiptWithoutTouchingOwnerStorage) {
  Storage storage;
  JointRowRecorder recorder(storage.view());
  Fixture local, foreign, expected;
  expected.run(nullptr, nullptr);
  JointProjectionResult foreign_answer;
  {
    JointRowCaptureScope scope(recorder.observer());
    std::thread worker([&] { foreign_answer = foreign.run(nullptr, nullptr); });
    worker.join();
    EXPECT_EQ(local.run(nullptr, nullptr).outcome, JointProjectionOutcome::Feasible);
  }
  EXPECT_EQ(foreign_answer.outcome, JointProjectionOutcome::Feasible);
  EXPECT_EQ(recorder.observer().attempts(), 1u); // The foreign thread never touches this counter.
  EXPECT_EQ(recorder.observer().error(), JointRowCaptureError::ForeignThread);
  EXPECT_FALSE(recorder.observer().valid());
  same_nodes(local, expected);
  same_nodes(foreign, expected);
}

TEST(JointRowCensus, ExtraProjectionAndMissingPublicationFailClosed) {
  for (const std::size_t budget : {std::size_t{0}, std::size_t{1}, std::size_t{100}}) {
    Fixture input, baseline;
    const auto expected = baseline.run(nullptr, nullptr, nullptr, budget);
    Storage storage;
    JointRowRecorder recorder(storage.view());
    {
      JointRowCaptureScope scope(recorder.observer());
      const auto answer = input.run(nullptr, nullptr, nullptr, budget);
      EXPECT_EQ(answer.outcome, expected.outcome);
      if (budget == 100) {
        Fixture extra;
        EXPECT_EQ(extra.run(nullptr, nullptr).outcome, JointProjectionOutcome::Feasible);
      }
    }
    EXPECT_FALSE(recorder.observer().valid());
    EXPECT_EQ(recorder.observer().error(), budget == 100 ? JointRowCaptureError::WrongPhase
                                                        : JointRowCaptureError::Incomplete);
    for (std::size_t i = 0; i < input.near.size(); ++i) {
      same_double(input.near[i].call, baseline.near[i].call);
      same_double(input.far[i].put, baseline.far[i].put);
    }
  }
}

TEST(JointRowCensus, FixedCoordinatesAndAlreadyFeasibleTargetsRemainExplicit) {
  Fixture input;
  input.boxes[0].put_lo = input.boxes[0].put_hi = 2.;
  Storage storage;
  JointRowRecorder recorder(storage.view());
  {
    JointRowCaptureScope scope(recorder.observer());
    ASSERT_EQ(input.run(nullptr, nullptr).outcome, JointProjectionOutcome::Feasible);
  }
  ASSERT_TRUE(recorder.observer().valid());
  EXPECT_EQ(recorder.data().variables.size(), 10u);
  EXPECT_EQ(recorder.data().coordinates[1].variable, -1);
  EXPECT_EQ(recorder.data().coordinates[7].variable, -1);
  same_double(recorder.data().coordinates[1].fixed, 2.);
  same_double(recorder.data().nodes[0].published_put, 2.);
  same_double(recorder.data().nodes[3].published_put, 2.);
  maybe_export(recorder, ".fixed.json");
  Storage second;
  JointRowRecorder feasible(second.view());
  detail::JointInboxProfile profile;
  {
    JointRowCaptureScope scope(feasible.observer());
    ASSERT_EQ(input.run(nullptr, &profile).outcome, JointProjectionOutcome::Feasible);
  }
  EXPECT_TRUE(feasible.observer().valid());
  EXPECT_EQ(profile.factorizations, 0u);
  EXPECT_EQ(profile.directions, 0u);
  EXPECT_EQ(feasible.sweeps(), 0u);
  maybe_export(feasible, ".feasible.json");
}

TEST(JointRowCensus, RawDraftAccessorPreservesSortedCallerRawAndSelectedRecords) {
  Fixture fixture;
  std::array<detail::RawPriceDraft, 2> drafts;
  for (std::size_t i = 0; i < 2; ++i) {
    const auto &nodes = i == 0 ? fixture.far : fixture.near;
    const auto &raw = i == 0 ? fixture.raw_far : fixture.raw_near;
    drafts[i].nodes.assign(nodes.begin(), nodes.end());
    drafts[i].raw_targets.assign(raw.begin(), raw.end());
    drafts[i].boxes.assign(fixture.boxes.begin(), fixture.boxes.end());
    drafts[i].bounds = {100., 1., 1., 98., 100.};
    drafts[i].prepared.market.calendar_years = i == 0 ? .5 : .25;
    drafts[i].prepared.diagnostics.discount = 1.;
    drafts[i].terminal_prepaid.assign(3, 99.);
  }
  Storage storage;
  JointRowRecorder recorder(storage.view());
  {
    JointRowCaptureScope scope(recorder.observer());
    EXPECT_EQ(detail::project_raw_price_drafts(drafts, 100, nullptr, nullptr).outcome,
              JointProjectionOutcome::Feasible);
  }
  ASSERT_TRUE(recorder.observer().valid());
  same_double(recorder.data().nodes[0].raw_call, fixture.raw_near[0].call);
  same_double(recorder.data().nodes[0].selected_call, fixture.near[0].call);
  same_double(recorder.data().nodes[3].raw_call, fixture.raw_far[0].call);
  EXPECT_TRUE(recorder.data().slices[0].has_discount);
  EXPECT_TRUE(recorder.data().slices[0].has_prepaid);
  same_double(recorder.data().nodes[0].prepaid, 99.);
  same_double(recorder.data().nodes[0].published_call, drafts[1].nodes[0].call);
  maybe_export(recorder, ".raw.json");
}

TEST(JointRowCensus, ExporterPreservesExactObservedBitsAndCompleteCounts) {
  Fixture input;
  Storage storage;
  JointRowRecorder recorder(storage.view());
  detail::JointInboxProfile profile;
  {
    JointRowCaptureScope scope(recorder.observer());
    ASSERT_EQ(input.run(nullptr, &profile).outcome, JointProjectionOutcome::Feasible);
  }
  ASSERT_TRUE(recorder.observer().valid());
  const auto report = quote_bench_joint_row_census::report(recorder);
  EXPECT_EQ(report.at("schema"), "atx-joint-row-census-v1");
  EXPECT_TRUE(report.at("valid").get<bool>());
  EXPECT_FALSE(report.at("timing_qualifying").get<bool>());
  EXPECT_EQ(report.at("counts").at("rows").get<std::size_t>(), recorder.data().rows.size());
  EXPECT_EQ(report.at("variables").size(), recorder.data().variables.size());
  EXPECT_EQ(report.at("nodes")[0].at("raw")[0],
            quote_bench_joint_row_census::bits(input.raw_near[0].call));
  EXPECT_EQ(report.at("nodes")[0].at("published")[0],
            quote_bench_joint_row_census::bits(input.near[0].call));
  EXPECT_EQ(quote_bench_joint_row_census::bits(-0.), "8000000000000000");
  maybe_export(recorder, "");
}

TEST(JointRowCensus, ExporterRetainsUnionGridAndNondividendRows) {
  std::array<std::array<AmericanPriceNode, 3>, 3> nodes;
  const std::array<std::array<double, 3>, 3> strikes{{{90.,100.,110.}, {92.,101.,108.}, {91.,99.,109.}}};
  std::array<std::array<AmericanPriceBox, 3>, 3> boxes;
  std::array<detail::JointInboxSlice, 3> slices;
  for (std::size_t s = 0; s < nodes.size(); ++s) {
    for (std::size_t i = 0; i < 3; ++i) {
      const double k = strikes[s][i], call = 55. - .5*k, put = .5*k - 45.;
      nodes[s][i] = {k, call, put};
      boxes[s][i] = {call-.25, call+.25, put-.25, put+.25};
    }
    slices[s] = {nodes[s], boxes[s], {100.,1.,1.,100.,100.}};
  }
  const JointRowRawSource raw{&nodes, [](const void *p, std::size_t i) noexcept {
    return std::span<const AmericanPriceNode>((*static_cast<const decltype(nodes) *>(p))[i]);
  }};
  quote_bench_joint_row_census::Owner owner;
  detail::JointInboxProfile profile;
  {
    JointRowCaptureScope scope(owner.recorder->observer());
    ASSERT_EQ(detail::project_joint_in_box(slices, 100, nullptr, &profile,
        detail::JointDirectionMode::Original, nullptr, &raw).outcome,
        JointProjectionOutcome::Feasible);
  }
  ASSERT_TRUE(owner.recorder->observer().valid());
  EXPECT_EQ(owner.recorder->data().slices.size(), 3u);
  maybe_export(*owner.recorder, ".union.json");
}

TEST(JointRowCensus, ToolOwnerAllocationFailureDoesNotPreventTheNumericalFit) {
  std::optional<quote_bench_joint_row_census::Owner> owner;
  testkit::alloc_counter_arm();
  testkit::alloc_counter_fail_once_at(1);
  owner.emplace();
  const auto allocations = testkit::alloc_counter_disarm();
  EXPECT_EQ(allocations, 1u);
  ASSERT_TRUE(owner->recorder.has_value());
  EXPECT_EQ(owner->recorder->observer().error(), research::JointRowCaptureError::SetupAllocation);
  Fixture input, baseline;
  baseline.run(nullptr, nullptr);
  {
    JointRowCaptureScope scope(owner->recorder->observer());
    EXPECT_EQ(input.run(nullptr, nullptr).outcome, JointProjectionOutcome::Feasible);
  }
  EXPECT_FALSE(owner->recorder->observer().valid());
  same_nodes(input, baseline);
}
} // namespace
} // namespace atx::vol::research
