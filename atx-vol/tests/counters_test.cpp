// counters_test.cpp — the ATX_VOL_COUNTERS facility (P0.2) contract.
//
// The default test build compiles with ATX_VOL_COUNTERS OFF, so the primary
// assertions here are the OFF-path guarantees: the macros compile to nothing, the
// snapshot API returns the "disabled" sentinel, and counters_enabled() is a
// constexpr false. When the suite is (re)built with -DATX_VOL_COUNTERS=ON the same
// test flips to verifying the counters actually count.

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <thread>
#include <vector>

#include "atx/vol/counters.hpp"

namespace {

using atx::vol::counters::Counter;
using atx::vol::counters::counters_enabled;
using atx::vol::counters::snapshot;

// Compile-time zero-cost proof for the default build: when the definition is
// absent, counters_enabled() folds to false, so every `if constexpr` counter
// block (and the header's atomic storage) is discarded.
#if !defined(ATX_VOL_COUNTERS)
static_assert(!counters_enabled(), "OFF build must report counters disabled (zero-cost sentinel)");
#endif

// The macros must be valid statements even in an unbraced `if` — the OFF
// expansion is `((void)0)`, the ON expansion a function call; both parse here.
TEST(Counters, MacrosAreStatementSafe) {
  int taken = 0;
  if (taken == 0)
    ATX_VOL_COUNT(BoundarySolves);
  else
    ATX_VOL_COUNT_N(FrameBytes, 7);
  for (int i = 0; i < 2; ++i)
    ATX_VOL_COUNT(NormCdfCalls);
  SUCCEED();
}

TEST(Counters, SnapshotContractMatchesBuildMode) {
  if constexpr (!counters_enabled()) {
    // OFF: disabled sentinel, and the macros never touch any state.
    const auto s = snapshot();
    EXPECT_FALSE(s.enabled);
    ATX_VOL_COUNT(BoundarySolves);
    ATX_VOL_COUNT_N(FrameBytes, 999);
    const auto s2 = snapshot();
    EXPECT_FALSE(s2.enabled);
    EXPECT_EQ(s2.get(Counter::BoundarySolves), 0u);
    EXPECT_EQ(s2.get(Counter::FrameBytes), 0u);
  } else {
    // ON: the counters actually accumulate and the snapshot is enabled.
    atx::vol::counters::reset();
    ATX_VOL_COUNT(BoundarySolves);
    ATX_VOL_COUNT(BoundarySolves);
    ATX_VOL_COUNT_N(FrameBytes, 40);
    const auto s = snapshot();
    EXPECT_TRUE(s.enabled);
    EXPECT_EQ(s.get(Counter::BoundarySolves), 2u);
    EXPECT_EQ(s.get(Counter::FrameBytes), 40u);
    atx::vol::counters::reset();
    const auto s2 = snapshot();
    EXPECT_EQ(s2.get(Counter::BoundarySolves), 0u);
  }
}

TEST(LightweightCounters, RouteSamplesAreExclusiveAndDeltaIsCoherent) {
  namespace lw = atx::vol::counters::lightweight;
  lw::reset();

  std::uint32_t sampled_queries = 0u;
  for (std::uint32_t i = 0; i < 3u * lw::kSamplePeriod; ++i) {
    lw::QuerySample sample{true};
    if (sample.sampled()) {
      if (sampled_queries == 0u) {
        sample.record_cache_hit(true);
      } else if (sampled_queries == 1u) {
        sample.record_cache_hit(false);
      }
      ++sampled_queries;
    }
    // The third sampled query deliberately records no hit, so its scope
    // classifies it as a cold fallback.
  }

  const lw::Snapshot before = lw::snapshot();
  EXPECT_EQ(before.sample_period, lw::kSamplePeriod);
  EXPECT_EQ(before.representative_hit_samples, 1u);
  EXPECT_EQ(before.other_cache_hit_samples, 1u);
  EXPECT_EQ(before.cold_fallback_samples, 1u);
  EXPECT_EQ(before.query_attempt_samples(), 3u);
  EXPECT_EQ(before.cache_hit_samples(), 2u);
  EXPECT_EQ(before.estimated_query_attempts(), 3u * lw::kSamplePeriod);
  EXPECT_DOUBLE_EQ(before.cache_hit_rate(), 2.0 / 3.0);
  EXPECT_DOUBLE_EQ(before.cold_fallback_rate(), 1.0 / 3.0);

  for (std::uint32_t i = 0; i < lw::kSamplePeriod; ++i) {
    lw::QuerySample sample{true};
    sample.record_cache_hit(true);
  }
  const lw::Snapshot change = lw::delta(before, lw::snapshot());
  EXPECT_EQ(change.query_attempt_samples(), 1u);
  EXPECT_EQ(change.representative_hit_samples, 1u);
  EXPECT_EQ(change.other_cache_hit_samples, 0u);
  EXPECT_EQ(change.cold_fallback_samples, 0u);
}

TEST(LightweightCounters, InversionSampleBatchesKernelWork) {
  namespace lw = atx::vol::counters::lightweight;
  lw::reset();

  for (std::uint32_t i = 0; i < lw::kSamplePeriod; ++i) {
    lw::AmericanIvSample sample;
    for (std::uint32_t residual = 0; residual < 11u; ++residual) {
      lw::record_residual_evaluation();
    }
    lw::record_boundary_solves(3u);
    lw::record_exp_calls(17u);
  }

  const lw::Snapshot measured = lw::snapshot();
  EXPECT_EQ(measured.american_iv_samples, 1u);
  EXPECT_EQ(measured.residual_evaluations_in_sampled_iv, 11u);
  EXPECT_EQ(measured.boundary_solves_in_sampled_iv, 3u);
  EXPECT_EQ(measured.exp_calls_in_sampled_iv, 17u);
  EXPECT_EQ(measured.estimated_american_iv_inversions(), lw::kSamplePeriod);
  EXPECT_EQ(measured.estimated_residual_evaluations(), 11u * lw::kSamplePeriod);
  EXPECT_DOUBLE_EQ(measured.residual_evaluations_per_inversion(), 11.0);
  EXPECT_DOUBLE_EQ(measured.boundary_solves_per_inversion(), 3.0);
  EXPECT_DOUBLE_EQ(measured.exp_calls_per_inversion(), 17.0);
}

TEST(LightweightCounters, ConcurrentWritersRetainEverySample) {
  namespace lw = atx::vol::counters::lightweight;
  lw::reset();
  constexpr std::uint32_t kWorkers = 8u;
  std::vector<std::thread> workers;
  workers.reserve(kWorkers);
  for (std::uint32_t worker = 0; worker < kWorkers; ++worker) {
    workers.emplace_back([worker] {
      for (std::uint32_t i = 0; i < lw::kSamplePeriod; ++i) {
        lw::QuerySample sample{true};
        if ((worker % 2u) == 0u) {
          sample.record_cache_hit(true);
        } else {
          sample.record_cache_hit(false);
        }
      }
      for (std::uint32_t i = 0; i < lw::kSamplePeriod; ++i) {
        lw::AmericanIvSample sample;
        lw::record_residual_evaluation();
        lw::record_boundary_solves(2u);
        lw::record_exp_calls(5u);
      }
    });
  }
  for (std::thread &worker : workers) {
    worker.join();
  }

  const lw::Snapshot measured = lw::snapshot();
  EXPECT_EQ(measured.representative_hit_samples, kWorkers / 2u);
  EXPECT_EQ(measured.other_cache_hit_samples, kWorkers / 2u);
  EXPECT_EQ(measured.cold_fallback_samples, 0u);
  EXPECT_EQ(measured.query_attempt_samples(), kWorkers);
  EXPECT_EQ(measured.american_iv_samples, kWorkers);
  EXPECT_EQ(measured.residual_evaluations_in_sampled_iv, kWorkers);
  EXPECT_EQ(measured.boundary_solves_in_sampled_iv, 2u * kWorkers);
  EXPECT_EQ(measured.exp_calls_in_sampled_iv, 5u * kWorkers);
}

// R-21: a nested inversion under an UNSAMPLED outer must not re-sample itself as
// a second root. White-box: force the american_iv sampler to miss on the next
// scope (position 0 != target), then confirm only the root advanced it.
TEST(LightweightCounters, NestedUnderUnsampledOuterIsNotASecondRoot) {
  namespace lw = atx::vol::counters::lightweight;
  namespace det = atx::vol::counters::lightweight::detail;
  lw::reset();
  det::t_state.american_iv.position = 0u;
  det::t_state.american_iv.target = 5u; // next scope at position 0 will NOT sample
  {
    lw::AmericanIvSample outer; // depth 0 -> root, but misses the sample here
    EXPECT_EQ(det::t_state.inversion_depth, 1u);
    {
      lw::AmericanIvSample nested; // depth 1 -> must never take its own sample
      EXPECT_EQ(det::t_state.inversion_depth, 2u);
      lw::record_residual_evaluation();
    }
    EXPECT_EQ(det::t_state.inversion_depth, 1u);
  }
  EXPECT_EQ(det::t_state.inversion_depth, 0u);
  // Only the root touched the sampler (advance by 1, not 2) — the old
  // active_inversion==nullptr gate would have let the nested scope sample too.
  EXPECT_EQ(det::t_state.american_iv.position, 1u);
  const lw::Snapshot s = lw::snapshot();
  EXPECT_EQ(s.american_iv_samples, 0u); // neither scope was sampled
  EXPECT_EQ(s.residual_evaluations_in_sampled_iv, 0u);
}

// The sampled root absorbs its nested scope's kernel work (not a separate root).
TEST(LightweightCounters, NestedWorkAttributesToTheSampledRoot) {
  namespace lw = atx::vol::counters::lightweight;
  namespace det = atx::vol::counters::lightweight::detail;
  lw::reset();
  det::t_state.american_iv.position = 0u;
  det::t_state.american_iv.target = 0u; // next scope samples (position == target)
  {
    lw::AmericanIvSample outer;
    lw::record_residual_evaluation(); // 1 in the outer
    {
      lw::AmericanIvSample nested;       // not a root; work flows to the outer
      lw::record_residual_evaluation();  // 2nd residual -> outer accumulator
      lw::record_boundary_solves(2u);
    }
  }
  const lw::Snapshot s = lw::snapshot();
  EXPECT_EQ(s.american_iv_samples, 1u);                // exactly one root
  EXPECT_EQ(s.residual_evaluations_in_sampled_iv, 2u); // outer + nested
  EXPECT_EQ(s.boundary_solves_in_sampled_iv, 2u);
}

// R-20: each thread seeds its samplers distinctly and rolls the target before
// first use, so the sampling phase decorrelates across threads instead of every
// thread locking to target==0 (which always observed the first event).
TEST(LightweightCounters, PerThreadSeedDecorrelatesSamplingPhase) {
  namespace det = atx::vol::counters::lightweight::detail;
  constexpr int kThreads = 16;
  std::vector<std::uint32_t> query_targets(kThreads, 0u);
  std::vector<std::thread> ts;
  ts.reserve(kThreads);
  for (int i = 0; i < kThreads; ++i) {
    ts.emplace_back([&query_targets, i] {
      // First touch of t_state on this thread runs the ThreadState ctor, which
      // mixes a per-thread seed and rolls the initial target.
      query_targets[static_cast<std::size_t>(i)] = det::t_state.query.target;
    });
  }
  for (std::thread &t : ts) {
    t.join();
  }
  const std::uint32_t first = query_targets.front();
  const bool all_equal = std::all_of(query_targets.begin(), query_targets.end(),
                                     [first](std::uint32_t v) { return v == first; });
  EXPECT_FALSE(all_equal) << "per-thread seeding must decorrelate the sampling phase";
}

} // namespace
