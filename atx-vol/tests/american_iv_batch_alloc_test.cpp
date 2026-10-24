// Allocation budget of the batch IV inversion's query loop: ZERO.
//
// K2 (r5, 2026-10-22). Once an InverseScratch has seen a batch of n lanes, invert_many on
// n lanes allocates nothing — on the scalar table path, the scalar family path and the
// AVX2 path, with anchored bid / ask chains, retained brackets and refused lanes in the
// batch. Counted with tests/support/alloc_counter.* (a thread-local armed tally over a
// replaced global operator new), linked ONLY into atx-vol-iv-batch-alloc-tests. The
// family, its table and the scratch are built (allocating) outside the counted block.
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

#include <gtest/gtest.h>

#include "atx/vol/api/pricing/american.hpp"
#include "atx/vol/api/simd/cpu.hpp"
#include "pricing/american_iv_batch.hpp"
#include "pricing/american_iv_candidate_capture.hpp"
#include "pricing/american_sigma_q_family.hpp"
#include "support/alloc_counter.hpp"

namespace atx::vol {
namespace {

namespace sq = sigma_q_family_detail;
namespace ib = iv_batch;

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
