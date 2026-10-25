// Allocation budget of the batch IV inversion's query loop: ZERO.
//
// K2 (r5, 2026-10-22). Once an InverseScratch has seen a batch of n lanes, invert_many on
// n lanes allocates nothing — on the scalar table path, the scalar family path and the
// AVX2 path, with anchored bid / ask chains, retained brackets and refused lanes in the
// batch. Counted with tests/support/alloc_counter.* (a thread-local armed tally over a
// replaced global operator new), linked ONLY into atx-vol-iv-batch-alloc-tests. The
// family, its table and the scratch are built (allocating) outside the counted block.
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <new>
#include <vector>

#include <gtest/gtest.h>

#include "atx/vol/api/pricing/american.hpp"
#include "atx/vol/api/simd/cpu.hpp"
#include "pricing/american_iv_batch.hpp"
#include "pricing/american_iv_candidate_capture.hpp"
#include "pricing/american_iv_candidate_batch.hpp"
#include "pricing/american_sigma_q_family.hpp"
#include "support/alloc_counter.hpp"

namespace atx::vol {
namespace {

namespace sq = sigma_q_family_detail;
namespace ib = iv_batch;

struct ColdFloorFault {
  std::size_t calls = 0, index = 0;
  static void inject(std::size_t index, void *context) {
    auto &state = *static_cast<ColdFloorFault *>(context);
    ++state.calls;
    state.index = index;
    // Deliberate exception-seam injection, not a numerical floor counterexample.
    throw std::bad_alloc();
  }
};

TEST(IvCandidateBatchAlloc, ColdFloorFailurePreservesOtherLanesAndResumesOnlyFailedProof) {
  if (!simd::use_avx2()) GTEST_SKIP() << "AVX2 unavailable";
  std::array<IvPriceSeedRequest, 5> seeds;
  std::array<IvPriceCandidateRequest, 5> requests;
  for (std::size_t i = 0; i < seeds.size(); ++i) {
    const double strike = 98. + static_cast<double>(i);
    const auto price = andersen_lake(100., strike, .5, .25, .04, .012, Side::Put);
    ASSERT_TRUE(price);
    seeds[i] = {*price, 100., strike, .5, .04, .012, .25, .001, Side::Put};
    requests[i] = {*price, 100., strike, .5, .04, .012, .001, Side::Put};
  }
  std::array<IvPriceCandidateResult, 5> baseline;
  IvPriceCandidateBatchStats baseline_stats;
  ASSERT_TRUE(american_implied_vol_price_candidate_batch(requests, IvOpts{},
      IvPriceSeedKernel::VectorReferenceEmpirical, baseline, baseline_stats));
  ASSERT_EQ(baseline_stats.proof.vector_admitted, 4u);
  ASSERT_EQ(baseline_stats.fast_continuations, 0u);
  const auto &failed = requests[1];
  const auto resumed = american_implied_vol_price_candidate(failed.price, failed.S,
      failed.K, failed.T, failed.r, failed.q, failed.side, IvOpts{}, failed.budget,
      nullptr, IvPriceProposalKernel::Fast);
  ASSERT_TRUE(resumed);
  for (const bool candidate : {false, true}) {
    ColdFloorFault fault;
    const research::IvSeedColdFloorTestHook hook{ColdFloorFault::inject, &fault};
    std::array<IvPriceSeedResult, 5> proofs;
    IvPriceSeedBatchStats proof_stats;
    std::array<IvPriceCandidateResult, 5> results;
    IvPriceCandidateBatchStats stats;
    bool escaped = false, applied = false;
    testkit::alloc_counter_arm();
    // Four allocations prepare the first put's floor. Refuse the second put's
    // cache State (plus three candidate scratch allocations when applicable),
    // then inject at its actual uncached cold-floor call. Lane zero completed.
    testkit::alloc_counter_fail_once_at(candidate ? 8u : 5u);
    try {
      if (candidate)
        applied = american_implied_vol_price_candidate_batch(requests, IvOpts{},
            IvPriceSeedKernel::VectorReferenceEmpirical, results, stats, false,
            IvPriceProposalKernel::ScreenThenFast, &hook);
      else
        applied = american_implied_vol_price_seed_batch(seeds, IvOpts{},
            IvPriceSeedKernel::VectorReferenceEmpirical, proofs, proof_stats, false, &hook);
    } catch (const std::bad_alloc &) {
      escaped = true;
    }
    const auto allocations = testkit::alloc_counter_disarm();
    EXPECT_FALSE(escaped);
    EXPECT_TRUE(applied);
    EXPECT_EQ(fault.calls, 1u);
    EXPECT_EQ(fault.index, 1u);
    EXPECT_GT(allocations, candidate ? 8u : 5u); // later lanes actually ran
    if (escaped) continue;
    if (candidate) {
      EXPECT_EQ(stats.screen_proposals, 5u);
      EXPECT_EQ(stats.screen_seeds, 5u);
      EXPECT_EQ(stats.proof.requests, 5u);
      EXPECT_EQ(stats.proof.allocation_refusals, 1u);
      EXPECT_EQ(stats.fast_continuations, 1u);
      EXPECT_EQ(stats.complete_scalar_fallbacks, 0u);
      EXPECT_TRUE(results[1].screen_proof.allocation_refused);
      EXPECT_FALSE(results[1].stats.screen_accepted);
      for (std::size_t i = 0; i < results.size(); ++i) {
        ASSERT_TRUE(results[i].sigma);
        EXPECT_FALSE(results[i].allocation_refused);
        if (i != 1) EXPECT_EQ(std::bit_cast<std::uint64_t>(*results[i].sigma),
                             std::bit_cast<std::uint64_t>(*baseline[i].sigma));
      }
      EXPECT_EQ(std::bit_cast<std::uint64_t>(*results[1].sigma),
                std::bit_cast<std::uint64_t>(*resumed));
    } else {
      EXPECT_EQ(proof_stats.requests, 5u);
      EXPECT_EQ(proof_stats.allocation_refusals, 1u);
      EXPECT_EQ(proof_stats.vector_admitted, 3u);
      EXPECT_EQ(proof_stats.scalar_fallbacks, 1u); // tail only; no whole-batch replay
      EXPECT_FALSE(proofs[1].sigma);
      EXPECT_TRUE(proofs[1].allocation_refused);
      for (const std::size_t i : {0u, 2u, 3u, 4u}) ASSERT_TRUE(proofs[i].sigma);
    }
  }
}

TEST(IvCandidateBatchAlloc, ScratchAllocationRefusalRunsEveryOriginalCallOnce) {
  const auto price = andersen_lake(773., 780., 1./365., .35, .045, .01, Side::Put);
  ASSERT_TRUE(price);
  const std::array<IvPriceCandidateRequest, 2> requests{{
      {*price, 773., 780., 1./365., .045, .01, .001, Side::Put},
      {10., -100., 100., 1., .05, 0., .001, Side::Put}}};
  const auto expected = american_implied_vol_price_candidate(*price,
      773., 780., 1./365., .045, .01, Side::Put, IvOpts{}, .001);
  ASSERT_TRUE(expected);
  EXPECT_FALSE(american_implied_vol_price_candidate(10., -100., 100., 1., .05,
      0., Side::Put, IvOpts{}, .001));
  for (std::size_t ordinal = 1; ordinal <= 3; ++ordinal) {
    std::array<IvPriceCandidateResult, 2> out;
    IvPriceCandidateBatchStats stats;
    testkit::alloc_counter_arm();
    testkit::alloc_counter_fail_once_at(ordinal);
    const bool applied = american_implied_vol_price_candidate_batch(requests, IvOpts{},
        IvPriceSeedKernel::VectorReferenceEmpirical, out, stats);
    const auto attempts = testkit::alloc_counter_disarm();
    ASSERT_TRUE(applied);
    EXPECT_GE(attempts, ordinal);
    ASSERT_TRUE(out[0].sigma);
    EXPECT_EQ(std::bit_cast<std::uint64_t>(*out[0].sigma),
              std::bit_cast<std::uint64_t>(*expected));
    EXPECT_FALSE(out[1].sigma);
    EXPECT_EQ(stats.requests, 2u);
    EXPECT_EQ(stats.setup_allocation_fallbacks, 1u);
    EXPECT_EQ(stats.complete_scalar_fallbacks, 2u);
    EXPECT_EQ(stats.screen_proposals, 0u); // this is the separately counted full fallback
    EXPECT_EQ(stats.proof.requests, 0u);
  }
}

TEST(IvCandidateBatchAlloc, ScreenAndResumedFastAllocationFailuresRemainPerCall) {
  const std::array<IvPriceCandidateRequest, 1> requests{{
      {10., -100., 100., 1., .05, 0., .001, Side::Put}}};
  EXPECT_FALSE(american_implied_vol_price_candidate(10., -100., 100., 1., .05,
      0., Side::Put, IvOpts{}, .001));
  // Three scratch allocations precede the actual error-message allocations:
  // Screen's error is fourth, and its resumed Fast error is fifth.
  for (const std::size_t ordinal : {4u, 5u}) {
    std::array<IvPriceCandidateResult, 1> out;
    IvPriceCandidateBatchStats stats;
    testkit::alloc_counter_arm();
    testkit::alloc_counter_fail_once_at(ordinal);
    const bool applied = american_implied_vol_price_candidate_batch(requests, IvOpts{},
        IvPriceSeedKernel::VectorReferenceEmpirical, out, stats);
    const auto attempts = testkit::alloc_counter_disarm();
    EXPECT_TRUE(applied);
    EXPECT_EQ(attempts, ordinal);
    EXPECT_FALSE(out[0].sigma);
    EXPECT_TRUE(out[0].allocation_refused);
    EXPECT_TRUE(out[0].stats.screen_attempted);
    EXPECT_EQ(stats.requests, 1u);
    EXPECT_EQ(stats.screen_proposals, 1u);
    EXPECT_EQ(stats.screen_seeds, 0u);
    EXPECT_EQ(stats.allocation_refusals, 1u);
    EXPECT_EQ(stats.fast_continuations, ordinal == 4 ? 0u : 1u);
  }
}

TEST(IvCandidateCaptureAlloc, EscapingAllocationFailureRecordsOnlyAttemptedPhase) {
  research::IvPriceCandidateTrace trace;
  // Register the always-on ledger and exercise the error path before injection.
  EXPECT_FALSE(research::american_implied_vol_price_candidate_traced(
      10., -100., 100., 1., .05, 0., Side::Put, IvOpts{}, .001, trace));
  testkit::alloc_counter_arm();
  testkit::alloc_counter_fail_once_at(1);
  const auto answer = research::american_implied_vol_price_candidate_traced(
      10., -100., 100., 1., .05, 0., Side::Put, IvOpts{}, .001, trace);
  const auto allocations = testkit::alloc_counter_disarm();
  EXPECT_FALSE(answer);
  EXPECT_EQ(allocations, 1u);
  EXPECT_TRUE(trace.complete);
  EXPECT_EQ(trace.outcome, research::IvCandidateOutcome::AllocationFailure);
  EXPECT_TRUE(trace.phases[0].visited);
  EXPECT_TRUE(trace.phases[0].complete);
  EXPECT_EQ(trace.phases[0].outcome, research::IvCandidatePhaseOutcome::AllocationFailure);
  EXPECT_FALSE(trace.phases[0].error);
  for (std::size_t i = 1; i < trace.phases.size(); ++i) {
    EXPECT_FALSE(trace.phases[i].visited);
    EXPECT_FALSE(trace.phases[i].complete);
  }
  EXPECT_EQ(trace.work, (research::IvCandidateWork{}));
}

TEST(IvCandidateCaptureAlloc, BoundedObservationAddsNoAllocation) {
  if (!research::iv_candidate_capture_enabled()) GTEST_SKIP() << "explicit capture build required";
  research::IvPriceCandidateTrace warm;
  EXPECT_FALSE(research::american_implied_vol_price_candidate_traced(
      10., 100., 100., 1., .05, 0., Side::Put, IvOpts{}, 0., warm));
  std::array<research::IvCandidateCaptureRecord, 1> records;
  std::array<research::IvCandidateCacheLifetime, 1> caches;
  research::IvCandidateCapture capture(records, caches);
  const auto allocations = testkit::count_allocations([&] {
    research::IvCandidateCaptureScope scope(capture);
    IvPriceSeedFloorCache cache;
    (void)american_implied_vol_price_candidate(10., 100., 100., 1., .05, 0., Side::Put,
        IvOpts{}, 0., nullptr, IvPriceProposalKernel::ScreenThenFast, &cache);
  });
  EXPECT_EQ(allocations, 0u);
  EXPECT_TRUE(capture.valid());
  ASSERT_EQ(capture.records().size(), 1u);
  EXPECT_EQ(capture.records()[0].trace.outcome, research::IvCandidateOutcome::InvalidControls);
}

TEST(IvCandidateCaptureAlloc, RefusedCacheAllocationRetainsAcceptedColdFloorFallback) {
  const auto price = andersen_lake(773., 780., 1. / 365., .35, .045, .01, Side::Put);
  ASSERT_TRUE(price);
  research::IvPriceCandidateTrace warm;
  const auto expected = research::american_implied_vol_price_candidate_traced(*price,
      773., 780., 1. / 365., .045, .01, Side::Put, IvOpts{}, .001, warm);
  ASSERT_TRUE(expected);
  IvPriceSeedFloorCache cache;
  research::IvPriceCandidateTrace trace;
  testkit::alloc_counter_arm();
  testkit::alloc_counter_fail_once_at(1);
  const auto actual = research::american_implied_vol_price_candidate_traced(*price,
      773., 780., 1. / 365., .045, .01, Side::Put, IvOpts{}, .001, trace,
      IvPriceProposalKernel::ScreenThenFast, &cache);
  const auto allocations = testkit::alloc_counter_disarm();
  ASSERT_TRUE(actual);
  EXPECT_EQ(std::bit_cast<std::uint64_t>(*actual), std::bit_cast<std::uint64_t>(*expected));
  EXPECT_EQ(allocations, 1u); // first cache State allocation fails, then scalar floor prices
  EXPECT_EQ(trace.outcome, research::IvCandidateOutcome::Accepted);
  EXPECT_EQ(trace.phases[1].outcome, research::IvCandidatePhaseOutcome::Value);
  EXPECT_EQ(trace.floor_preparations, 0u);
  EXPECT_EQ(trace.floor_hits, 0u);
  EXPECT_EQ(cache.preparations(), 0u);
  EXPECT_EQ(cache.hits(), 0u);
  for (const auto &slot : trace.floor_after.slots) EXPECT_FALSE(slot.prepared);
  // The optional helper contains the allocation failure; no phase-level
  // AllocationFailure is fabricated for the successful scalar fallback.
  EXPECT_EQ(trace.work, warm.work);
  RecordProperty("cache_allocation_attempts", static_cast<int>(allocations));
  RecordProperty("accepted_with_empty_cache", 1);
  RecordProperty("scalar_proof_boundary_solves", static_cast<int>(trace.phases[1].work[0]));
}

TEST(IvBatchAlloc, QueryLoopAllocatesNothing) {
  constexpr std::size_t n = 30;
  const AlExpiryGeometry g = prepare_al_geometry(0.5, 0.05, 0.02);
  for (const Side side : {Side::Put, Side::Call}) {
    AlSigmaQFamily fam(g, side, 100.0);
    ASSERT_TRUE(fam.prepare(0.1, 0.8, 12, 0.02, 0.021, 4));
    sq::PremiumTable table;
    ASSERT_EQ(sq::build_premium_table(fam, table), sq::FusedStatus::Ok);
    constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
    std::vector<double> K(n), target(n), q(n), seed(n), blo(n, kNaN), bhi(n, kNaN);
    std::vector<std::uint8_t> is_call(n, side == Side::Call ? 1u : 0u);
    std::vector<std::int32_t> anchor(n, -1);
    for (std::size_t i = 0; i < n; ++i) {
      const std::size_t quote = i / 3;
      K[i] = 85.0 + 3.0 * static_cast<double>(quote);
      q[i] = 0.02 + 0.0001 * static_cast<double>(quote);
      const double sigma =
          0.2 + 0.03 * static_cast<double>(quote) + 0.01 * static_cast<double>(i % 3);
      target[i] = sq::price_and_vega(fam, 100.0, K[i], sigma, q[i]).price;
      seed[i] = (i % 3 == 0) ? 1.2 * sigma : kNaN;
      if (i % 3 != 0) anchor[i] = static_cast<std::int32_t>(3 * quote);
    }
    target[7] = -1.0;   // a refused lane
    q[13] = 0.5;        // q outside the box
    seed[3] = 5.0;      // a seed above the box
    blo[6] = 0.15;      // a retained bracket
    bhi[6] = 0.6;
    std::vector<double> sigma(n), olo(n), ohi(n), oplo(n), ophi(n), res(n), vega(n), last(n);
    std::vector<std::uint16_t> evals(n);
    std::vector<ib::InverseStatus> status(n);
    std::vector<std::uint8_t> flags(n);
    std::vector<ib::LaneAcceptance> acc(n);
    const ib::InverseBatchOut out{sigma, olo, ohi, oplo, ophi, res, vega, last, evals, status,
                                  flags, acc};
    ib::InverseScratch scratch;
    scratch.reserve(n);
    for (const int path : {0, 1, 2}) {
      if (path == 2 && !simd::have_avx2()) continue;
      ib::InverseBatchIn in;
      if (path == 1) {
        in.family = &fam;
      } else {
        in.table = &table;
      }
      in.S = 100.0;
      in.isa = path == 2 ? ib::InverseIsa::Avx2 : ib::InverseIsa::Scalar;
      in.K = K;
      in.target = target;
      in.q = q;
      in.is_call = is_call;
      in.seed_sigma = seed;
      in.bracket_lo = blo;
      in.bracket_hi = bhi;
      in.anchor = anchor;
      ib::InverseBatchWork work;
      ib::InverseBatchError e = ib::InverseBatchError::SpanMismatch;
      const std::uint64_t allocs = testkit::count_allocations([&] {
        for (int rep = 0; rep < 3; ++rep) {
          e = ib::invert_many(in, out, scratch, &work);
        }
      });
      ASSERT_EQ(e, ib::InverseBatchError::None);
      EXPECT_EQ(allocs, 0u) << (side == Side::Put ? "put" : "call") << " path " << path;
      EXPECT_EQ(work.lanes, 3u * n);
      EXPECT_EQ(work.by_status[static_cast<std::size_t>(ib::InverseStatus::Converged)] +
                    work.by_status[static_cast<std::size_t>(ib::InverseStatus::InvalidInput)] +
                    work.by_status[static_cast<std::size_t>(ib::InverseStatus::QOutsideBox)],
                3u * n);
      EXPECT_GE(work.by_status[static_cast<std::size_t>(ib::InverseStatus::Converged)],
                3u * (n - 2u));
      EXPECT_GT(work.evaluations, 0u);
    }
  }
}

} // namespace
} // namespace atx::vol
