#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

#include "atx/vol/research_validation.hpp"

namespace {

using namespace atx::vol;

constexpr std::int64_t kDayNs = 86'400'000'000'000LL;

[[nodiscard]] ArchiveContentIdentity source_identity() {
  return ArchiveContentIdentity{4096u, 123456789u, 0x1234u, 0x5678u};
}

[[nodiscard]] ResearchObservation observation(std::size_t day, std::uint32_t uid = 1u,
                                              double signal = 1.0, double pnl = 0.01) {
  const std::int64_t base = static_cast<std::int64_t>(day) * kDayNs;
  ResearchObservation out;
  out.uid = uid;
  out.observed_ts_ns = base + 1;
  out.available_ts_ns = base + 2;
  out.decision_ts_ns = base + 3;
  out.execution_ts_ns = base + 4;
  out.label_end_ts_ns = base + kDayNs;
  out.signal = signal;
  out.forward_pnl = pnl;
  out.lagged_capital = 1.0;
  out.source_identity = source_identity();
  return out;
}

[[nodiscard]] ResearchSelectionAdjustment adjustment(std::uint64_t attempted_trials,
                                                     double sharpe_variance = 0.0) {
  ResearchSelectionAdjustment out;
  out.family_sealed = true;
  out.attempted_trials = attempted_trials;
  out.successful_trial_sharpe_variance = sharpe_variance;
  return out;
}

TEST(ResearchObservationValidation, SameCloseRejectedAndNextSessionAccepted) {
  ResearchObservation same_close = observation(0u);
  same_close.execution_ts_ns = same_close.decision_ts_ns;
  EXPECT_FALSE(canonicalize_research_observations({&same_close, 1u}));

  ResearchObservation next_session = observation(0u);
  next_session.execution_ts_ns += kDayNs;
  next_session.label_end_ts_ns += kDayNs;
  auto accepted = canonicalize_research_observations({&next_session, 1u});
  ASSERT_TRUE(accepted) << accepted.error().to_string();
  ASSERT_EQ(accepted->size(), 1u);
  EXPECT_EQ(accepted->front(), next_session);
}

TEST(ResearchObservationValidation, RejectsNonFiniteAndDuplicatePanelKeys) {
  ResearchObservation bad = observation(0u);
  bad.signal = std::numeric_limits<double>::quiet_NaN();
  EXPECT_FALSE(canonicalize_research_observations({&bad, 1u}));

  const ResearchObservation first = observation(0u, 7u);
  ResearchObservation duplicate = first;
  duplicate.signal = 2.0;
  const std::vector<ResearchObservation> panel{duplicate, first};
  EXPECT_FALSE(canonicalize_research_observations(panel));
}

TEST(ResearchParameterGrid, AxisPermutationHasCanonicalOrderAndStableIdentity) {
  ResearchParameterGrid left;
  left.max_trials = 16u;
  left.axes = {
      ResearchParameterAxis{"zeta", {std::int64_t{2}, std::int64_t{1}}},
      ResearchParameterAxis{"alpha", {true, false}},
  };
  ResearchParameterGrid right;
  right.max_trials = 16u;
  right.axes = {
      ResearchParameterAxis{"alpha", {false, true}},
      ResearchParameterAxis{"zeta", {std::int64_t{1}, std::int64_t{2}}},
  };

  auto a = enumerate_research_parameter_grid(left);
  auto b = enumerate_research_parameter_grid(right);
  ASSERT_TRUE(a) << a.error().to_string();
  ASSERT_TRUE(b) << b.error().to_string();
  EXPECT_EQ(*a, *b);
  ASSERT_EQ(a->size(), 4u);
  for (const ResearchParameterSet &set : *a) {
    EXPECT_NE(set.identity, 0u);
  }

  left.max_trials = 3u;
  EXPECT_FALSE(enumerate_research_parameter_grid(left));
}

TEST(ResearchWalkForward, PurgesOutcomeOverlapEmbargoesGapAndKeepsPanelTogether) {
  std::vector<ResearchObservation> observations;
  for (std::size_t day = 0; day < 8u; ++day) {
    observations.push_back(observation(day, 1u));
    observations.push_back(observation(day, 2u));
  }
  // Fold 0 tests days 4-5. Day 2's outcome reaches into that test; day 3 is
  // otherwise admissible but lies inside the explicit one-day pre-test embargo.
  for (ResearchObservation &row : observations) {
    const std::size_t day = static_cast<std::size_t>(row.decision_ts_ns / kDayNs);
    if (day == 2u) {
      row.label_end_ts_ns = 5 * kDayNs;
    }
  }
  auto canonical = canonicalize_research_observations(observations);
  ASSERT_TRUE(canonical) << canonical.error().to_string();

  ResearchWalkForwardSpec spec;
  spec.kind = ResearchWalkForwardKind::Anchored;
  spec.min_train_groups = 4u;
  spec.test_groups = 2u;
  spec.step_groups = 2u;
  spec.embargo_ns = kDayNs;
  auto plan = make_purged_walk_forward_plan(*canonical, spec);
  ASSERT_TRUE(plan) << plan.error().to_string();
  ASSERT_EQ(plan->folds.size(), 2u);

  const ResearchValidationFold &fold = plan->folds.front();
  ASSERT_EQ(fold.test_indices.size(), 4u);
  EXPECT_EQ((*canonical)[fold.test_indices.front()].decision_ts_ns / kDayNs, 4);
  EXPECT_EQ((*canonical)[fold.test_indices.back()].decision_ts_ns / kDayNs, 5);
  ASSERT_EQ(fold.purged_indices.size(), 2u);
  ASSERT_EQ(fold.embargoed_indices.size(), 2u);
  for (const std::size_t i : fold.purged_indices) {
    EXPECT_EQ((*canonical)[i].decision_ts_ns / kDayNs, 2);
  }
  for (const std::size_t i : fold.embargoed_indices) {
    EXPECT_EQ((*canonical)[i].decision_ts_ns / kDayNs, 3);
  }
  EXPECT_TRUE(validate_research_plan_no_leakage(*canonical, *plan));
}

TEST(ResearchWalkForward, PurgesWholePanelWhenOnlyOneLabelOverlaps) {
  std::vector<ResearchObservation> observations;
  for (std::size_t day = 0; day < 7u; ++day) {
    observations.push_back(observation(day, 1u));
    observations.push_back(observation(day, 2u));
  }
  // Only uid 1 has an overlapping label at day 2. The decision-time panel is
  // the indivisible sampling unit, so uid 2 must be purged with it.
  for (ResearchObservation &row : observations) {
    if (row.uid == 1u && row.decision_ts_ns / kDayNs == 2) {
      row.label_end_ts_ns = 5 * kDayNs;
    }
  }
  auto canonical = canonicalize_research_observations(observations);
  ASSERT_TRUE(canonical) << canonical.error().to_string();

  ResearchWalkForwardSpec spec;
  spec.min_train_groups = 4u;
  spec.test_groups = 2u;
  spec.step_groups = 2u;
  auto plan = make_purged_walk_forward_plan(*canonical, spec);
  ASSERT_TRUE(plan) << plan.error().to_string();
  const ResearchValidationFold &fold = plan->folds.front();

  std::size_t purged_day_two = 0u;
  for (const std::size_t index : fold.purged_indices) {
    if ((*canonical)[index].decision_ts_ns / kDayNs == 2) {
      ++purged_day_two;
    }
  }
  EXPECT_EQ(purged_day_two, 2u);
  for (const std::size_t index : fold.train_indices) {
    EXPECT_NE((*canonical)[index].decision_ts_ns / kDayNs, 2);
  }
}

TEST(ResearchSignalEvaluation, MutatingTestValuesCannotAlterTrainTransform) {
  std::vector<ResearchObservation> observations;
  for (std::size_t day = 0; day < 10u; ++day) {
    observations.push_back(
        observation(day, 1u, static_cast<double>(day + 1u), (day % 2u == 0u) ? 0.01 : -0.01));
  }
  auto canonical = canonicalize_research_observations(observations);
  ASSERT_TRUE(canonical) << canonical.error().to_string();

  ResearchWalkForwardSpec split;
  split.min_train_groups = 6u;
  split.test_groups = 2u;
  split.step_groups = 2u;
  auto plan = make_purged_walk_forward_plan(*canonical, split);
  ASSERT_TRUE(plan) << plan.error().to_string();

  ResearchSignalCandidate candidate;
  candidate.id = "rolling-z";
  candidate.transform = ResearchSignalTransform::RollingZScore;
  candidate.lookback = 3u;
  candidate.lag = 0u;
  candidate.direction = ResearchSignalDirection::LongHigh;

  auto before =
      evaluate_research_signal_candidate(*canonical, *plan, candidate, 1u, adjustment(1u));
  ASSERT_TRUE(before) << before.error().to_string();

  std::vector<ResearchObservation> mutated = *canonical;
  for (const std::size_t i : plan->folds.front().test_indices) {
    mutated[i].signal *= 1.0e6;
  }
  auto after = evaluate_research_signal_candidate(mutated, *plan, candidate, 1u, adjustment(1u));
  ASSERT_TRUE(after) << after.error().to_string();
  EXPECT_EQ(before->in_sample_returns, after->in_sample_returns);
}

TEST(ResearchStatistics, PositiveAutocorrelationWidensNeweyWestMeanError) {
  std::vector<ResearchReturnObservation> returns;
  for (std::size_t i = 0; i < 40u; ++i) {
    const double value = ((i / 5u) % 2u == 0u) ? 0.02 : -0.015;
    returns.push_back(ResearchReturnObservation{static_cast<std::int64_t>(i), value, 1.0, value});
  }
  auto iid = compute_research_return_stats(returns, 0u, adjustment(1u));
  auto hac = compute_research_return_stats(returns, 4u, adjustment(1u));
  ASSERT_TRUE(iid) << iid.error().to_string();
  ASSERT_TRUE(hac) << hac.error().to_string();
  EXPECT_GT(hac->hac_mean_standard_error, iid->hac_mean_standard_error);
  EXPECT_LT(std::fabs(hac->hac_t_statistic), std::fabs(iid->hac_t_statistic));
}

TEST(ResearchStatistics, DeflatedSharpeFallsAsSealedAttemptedTrialCountGrows) {
  std::vector<ResearchReturnObservation> returns;
  const std::vector<double> values{0.01,   0.02,  -0.004, 0.015, 0.006, 0.018,
                                   -0.002, 0.011, 0.009,  0.013, 0.004, 0.016};
  for (std::size_t i = 0; i < values.size(); ++i) {
    returns.push_back(
        ResearchReturnObservation{static_cast<std::int64_t>(i), values[i], 1.0, values[i]});
  }
  auto one = compute_research_return_stats(returns, 1u, adjustment(1u, 0.04));
  auto many = compute_research_return_stats(returns, 1u, adjustment(100u, 0.04));
  ASSERT_TRUE(one) << one.error().to_string();
  ASSERT_TRUE(many) << many.error().to_string();
  EXPECT_LT(many->deflated_sharpe_probability, one->deflated_sharpe_probability);
  EXPECT_EQ(many->attempted_trials, 100u);
}

TEST(ResearchStatistics, RejectsOverflowedComputedMoments) {
  const std::vector<ResearchReturnObservation> returns{
      ResearchReturnObservation{0, 1.0e308, 1.0, 1.0e308},
      ResearchReturnObservation{1, 1.0e308, 1.0, 1.0e308},
  };
  EXPECT_FALSE(compute_research_return_stats(returns, 0u, adjustment(1u)));
}

TEST(ResearchMultipleTesting, RawPValueCanPassWhileAdjustedFamilyFails) {
  const std::vector<double> raw{0.01, 0.20, 0.30, 0.40, 0.50, 0.60, 0.70, 0.80, 0.90, 0.95};
  auto holm = holm_adjusted_p_values(raw);
  auto by = benjamini_yekutieli_adjusted_p_values(raw);
  ASSERT_TRUE(holm) << holm.error().to_string();
  ASSERT_TRUE(by) << by.error().to_string();
  EXPECT_LT(raw.front(), 0.05);
  EXPECT_GT(holm->front(), 0.05);
  EXPECT_GT(by->front(), 0.05);
}

TEST(ResearchMining, OutOfSampleSelectionRejectsInSampleOnlyWinner) {
  std::vector<ResearchObservation> observations;
  const std::vector<double> signal{1, -1, 1, -1, 1, -1, 6, 5, 4, 3, 2, 1};
  for (std::size_t day = 0; day < signal.size(); ++day) {
    const double pnl = day < 6u ? ((signal[day] > 0.0) ? 0.01 : -0.01) : -0.01;
    observations.push_back(observation(day, 1u, signal[day], pnl));
  }
  auto canonical = canonicalize_research_observations(observations);
  ASSERT_TRUE(canonical) << canonical.error().to_string();

  ResearchWalkForwardSpec split;
  split.min_train_groups = 6u;
  split.test_groups = 3u;
  split.step_groups = 3u;
  auto plan = make_purged_walk_forward_plan(*canonical, split);
  ASSERT_TRUE(plan) << plan.error().to_string();

  ResearchSignalCandidate identity;
  identity.id = "identity";
  identity.transform = ResearchSignalTransform::Identity;
  identity.direction = ResearchSignalDirection::LongHigh;

  ResearchSignalCandidate difference;
  difference.id = "difference";
  difference.transform = ResearchSignalTransform::Difference;
  difference.lookback = 1u;
  difference.direction = ResearchSignalDirection::LongHigh;

  auto mined = mine_research_signal_candidates(*canonical, *plan, {identity, difference}, 1u,
                                               ResearchTrialFamily{true, 2u});
  ASSERT_TRUE(mined) << mined.error().to_string();
  ASSERT_EQ(mined->evaluations.size(), 2u);
  EXPECT_EQ(mined->selected_candidate_id, "difference");

  const auto identity_eval = std::find_if(
      mined->evaluations.begin(), mined->evaluations.end(),
      [](const ResearchCandidateEvaluation &value) { return value.candidate.id == "identity"; });
  const auto difference_eval = std::find_if(
      mined->evaluations.begin(), mined->evaluations.end(),
      [](const ResearchCandidateEvaluation &value) { return value.candidate.id == "difference"; });
  ASSERT_NE(identity_eval, mined->evaluations.end());
  ASSERT_NE(difference_eval, mined->evaluations.end());
  EXPECT_GT(identity_eval->in_sample_stats.mean, difference_eval->in_sample_stats.mean);
  EXPECT_LT(identity_eval->oos_stats.mean, difference_eval->oos_stats.mean);
}

TEST(ResearchMining, RejectsDuplicateIdsEvenWhenIdentitySortSeparatesThem) {
  std::vector<ResearchObservation> observations;
  for (std::size_t day = 0; day < 14u; ++day) {
    observations.push_back(observation(day, 1u, static_cast<double>(day), 0.01));
  }
  auto canonical = canonicalize_research_observations(observations);
  ASSERT_TRUE(canonical) << canonical.error().to_string();

  ResearchWalkForwardSpec split;
  split.min_train_groups = 8u;
  split.test_groups = 3u;
  split.step_groups = 3u;
  auto plan = make_purged_walk_forward_plan(*canonical, split);
  ASSERT_TRUE(plan) << plan.error().to_string();

  // With the stable identity hash, "middle-9" sorts between these two
  // differently-parameterized "duplicate" candidates. An adjacent-only ID
  // check therefore misses the duplicate.
  const ResearchSignalCandidate duplicate_level{"duplicate", ResearchSignalTransform::Identity, 0u,
                                                0u, ResearchSignalDirection::LongHigh};
  const ResearchSignalCandidate middle{"middle-9", ResearchSignalTransform::Identity, 9u, 0u,
                                       ResearchSignalDirection::LongHigh};
  const ResearchSignalCandidate duplicate_difference{
      "duplicate", ResearchSignalTransform::Difference, 0u, 1u, ResearchSignalDirection::LongHigh};
  const std::vector<ResearchSignalCandidate> candidates{duplicate_level, middle,
                                                        duplicate_difference};

  EXPECT_FALSE(mine_research_signal_candidates(*canonical, *plan, candidates, 1u,
                                               ResearchTrialFamily{true, 3u}));
}

TEST(ResearchPromotion, AnyMandatoryGateFailureBlocksPromotion) {
  ResearchPromotionEvidence evidence;
  evidence.source_lineage_complete = true;
  evidence.family_sealed = true;
  evidence.independent_validation_passed = true;
  evidence.holdout_consumed = true;
  evidence.cost_stress_passed = false;
  evidence.concentration_passed = true;
  evidence.oos_stats.n_observations = 1000u;
  evidence.oos_stats.hac_t_statistic = 4.0;
  evidence.oos_stats.deflated_sharpe_probability = 0.99;
  evidence.oos_stats.max_drawdown = 0.10;
  evidence.adjusted_p_value = 0.01;

  ResearchPromotionGateSpec gates;
  gates.min_oos_observations = 500u;
  gates.min_hac_t_statistic = 3.0;
  gates.min_deflated_sharpe_probability = 0.95;
  gates.max_adjusted_p_value = 0.05;
  gates.max_drawdown = 0.25;
  auto decision = evaluate_research_promotion(evidence, gates);
  ASSERT_TRUE(decision) << decision.error().to_string();
  EXPECT_FALSE(decision->promoted);
  const auto failed = std::find_if(decision->gates.begin(), decision->gates.end(),
                                   [](const ResearchPromotionGateResult &gate) {
                                     return gate.code == ResearchPromotionGateCode::CostStress;
                                   });
  ASSERT_NE(failed, decision->gates.end());
  EXPECT_FALSE(failed->passed);
}

} // namespace
