// library_corr_index_test.cpp — S4-3: SimHash corr-neighbor index + incremental
// corr-to-pool gate.
//
// CorrNeighborIndex is the o(N) accelerator for the corr-to-pool re-gate: instead
// of scanning the ENTIRE admitted pool with combine::pairwise_complete_corr
// (O(N) per candidate, O(N^2) to re-gate a generation), it SimHash-LSH-buckets
// each admitted alpha's demeaned PnL vector and returns only the candidate's
// approximate near-neighbors. online_corr_to_pool then computes the EXACT
// pairwise_complete_corr over JUST those candidates and returns the max |corr|.
//
// The dominant risk of any corr-accelerator is MISSED RECALL: if a true
// high-correlation neighbor is not bucketed near the candidate, the gate
// silently admits a correlated alpha. The differential test below is the
// load-bearing guard — it asserts that the approximate max |corr| equals the
// brute-force exact max |corr| on >= 90% of queries (i.e. the true argmax
// neighbor IS recalled at least 90% of the time), with the neighbor set staying
// well under the pool size (the speedup).
//
// Tests:
//   * MatchesExactMaxCorrWithinRecallBound (LOAD-BEARING): approx == exact on
//     >= 90% of perturbed-near-duplicate queries (recall).
//   * NeighborSetMuchSmallerThanPool: median neighbor count < 0.2*N (the o(N)).
//   * IdenticalStreamIsItsOwnNeighborCorrOne: a stream's corr-to-pool is 1.0.
//   * SameSeedSameSignatures: seeded hyperplanes reproduce signatures (L7).

#include <algorithm>  // std::sort
#include <cmath>      // std::abs
#include <filesystem> // per-test temp directory (LibraryStore catalog)
#include <span>       // std::span (brute-force reference)
#include <string>
#include <system_error> // std::error_code
#include <vector>

#include <gtest/gtest.h>

#include "atx/core/random.hpp" // Xoshiro256pp (deterministic fixtures)
#include "atx/core/types.hpp"  // f64, u32, u64, usize

#include "atx/engine/combine/correlation.hpp" // pairwise_complete_corr (exact ref)
#include "atx/engine/combine/metrics.hpp"     // combine::AlphaMetrics
#include "atx/engine/library/corr_index.hpp"  // the unit under test
#include "atx/engine/library/store.hpp"       // LibraryStore, Provenance

namespace atxtest_library_corr_index_test {

using atx::f64;
using atx::u32;
using atx::u64;
using atx::usize;
using atx::core::Xoshiro256pp;
using atx::engine::combine::AlphaId;
using atx::engine::combine::AlphaMetrics;
using atx::engine::combine::pairwise_complete_corr;

namespace lib = atx::engine::library;

// --- fixtures --------------------------------------------------------------

// A per-test unique temp directory (LibraryStore needs a writable dir for its
// sqlite catalog; we never flush, so reads come from the live memtable).
[[nodiscard]] std::string tmpdir(const std::string &tag = "") {
  const ::testing::TestInfo *info = ::testing::UnitTest::GetInstance()->current_test_info();
  std::string base = std::string(info != nullptr ? info->test_suite_name() : "S4") + "_" +
                     std::string(info != nullptr ? info->name() : "t") + "_" + tag;
  const std::filesystem::path dir = std::filesystem::temp_directory_path() / "atx_s4_corr" / base;
  std::error_code ec;
  std::filesystem::remove_all(dir, ec);
  std::filesystem::create_directories(dir, ec);
  return dir.string();
}

// A deterministic Xoshiro-seeded pool of N f64 streams of length T. To make the
// differential MEANINGFUL (a vacuous all-uncorrelated pool would pass trivially),
// roughly one in eight streams is a NEAR-DUPLICATE of an earlier stream: a clone
// scaled + shifted with tiny additive noise, so its exact corr to its base is
// genuinely high (~0.97-0.999). The rest are independent N(0,1) walks.
[[nodiscard]] std::vector<std::vector<f64>> random_pnl_pool(usize N, usize T, u64 seed) {
  Xoshiro256pp rng(seed);
  std::vector<std::vector<f64>> streams;
  streams.reserve(N);
  for (usize a = 0; a < N; ++a) {
    std::vector<f64> s(T);
    // Every 8th stream (after the first few) clones an earlier one + tiny noise.
    if (a >= 8 && (a % 8) == 0) {
      const usize base = a - 8;
      for (usize t = 0; t < T; ++t) {
        s[t] = streams[base][t] + 0.02 * rng.normal(); // tiny perturbation
      }
    } else {
      for (usize t = 0; t < T; ++t) {
        s[t] = rng.normal();
      }
    }
    streams.push_back(std::move(s));
  }
  return streams;
}

[[nodiscard]] AlphaMetrics default_metrics() {
  return AlphaMetrics{0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0};
}

[[nodiscard]] lib::Provenance default_prov() { return lib::Provenance{}; }

// Build a LibraryStore in a fresh tmpdir and stage every stream (no flush — the
// memtable read path is sufficient and keeps store.pnl(id) returning each stream
// exactly). Positions are EMPTY (n_instruments == 0): the corr index only reads
// pnl, and AlphaStore::insert accepts a zero-length positions_flat when T*0 == 0.
// `streams` must outlive the returned store (the staged pnl is copied into the
// memtable, so the store is self-contained after staging).
[[nodiscard]] lib::LibraryStore staged(const std::vector<std::vector<f64>> &streams,
                                       const std::string &tag) {
  lib::LibraryStore store(tmpdir(tag));
  const std::vector<f64> no_pos; // n_instruments == 0
  const auto m = default_metrics();
  const auto p = default_prov();
  for (const auto &s : streams) {
    const bool ok = store.stage(nullptr, s, no_pos, m, p).has_value();
    EXPECT_TRUE(ok);
  }
  return store;
}

void add_all(lib::CorrNeighborIndex &idx, const lib::LibraryStore &store) {
  for (u32 a = 0; a < store.n_alphas(); ++a) {
    idx.add(AlphaId{a}, store.pnl(AlphaId{a}));
  }
}

// m perturbed-near-duplicate queries: clone a spread-out subset of pool members
// with tiny noise. A query is NOT in the store (so the exact max-corr is the corr
// to the query's own base in the pool, which is high but < 1.0) — this makes the
// differential test whether the SimHash actually recalls that true near-neighbor
// rather than trivially matching the query's own self-corr.
[[nodiscard]] std::vector<std::vector<f64>>
query_set(const std::vector<std::vector<f64>> &streams, usize m, u64 seed) {
  Xoshiro256pp rng(seed);
  const usize N = streams.size();
  const usize T = streams.empty() ? 0 : streams[0].size();
  std::vector<std::vector<f64>> qs;
  qs.reserve(m);
  for (usize i = 0; i < m; ++i) {
    const usize base = (i * (N / (m + 1) + 1)) % N; // spread across the pool
    std::vector<f64> q(T);
    for (usize t = 0; t < T; ++t) {
      q[t] = streams[base][t] + 0.02 * rng.normal();
    }
    qs.push_back(std::move(q));
  }
  return qs;
}

// O(N) brute-force reference: MAX over ALL store ids of |pairwise_complete_corr|.
[[nodiscard]] f64 exact_max_abs_corr(std::span<const f64> q, const lib::LibraryStore &store) {
  f64 worst = 0.0;
  for (u32 a = 0; a < store.n_alphas(); ++a) {
    const f64 c = std::abs(pairwise_complete_corr(q, store.pnl(AlphaId{a})));
    worst = (c > worst) ? c : worst;
  }
  return worst;
}

[[nodiscard]] f64 median_neighbor_count(const lib::CorrNeighborIndex &idx,
                                        const lib::LibraryStore &store) {
  std::vector<f64> counts;
  const u32 n = static_cast<u32>(store.n_alphas());
  const u32 step = (n / 64u) == 0u ? 1u : (n / 64u); // ~64-query sample
  for (u32 a = 0; a < n; a += step) {
    counts.push_back(static_cast<f64>(idx.neighbors(store.pnl(AlphaId{a})).size()));
  }
  std::sort(counts.begin(), counts.end());
  return counts.empty() ? 0.0 : counts[counts.size() / 2];
}

// --- tests -----------------------------------------------------------------

TEST(LibraryCorrIndex, MatchesExactMaxCorrWithinRecallBound) { // LOAD-BEARING
  const usize N = 512;
  const usize T = 256;
  const auto streams = random_pnl_pool(N, T, /*seed*/ 3);
  lib::LibraryStore store = staged(streams, "rt");
  lib::CorrNeighborIndex idx(/*seed*/ 9, /*T*/ T, /*K*/ 64);
  add_all(idx, store);

  int hits = 0;
  int Q = 0;
  for (const auto &q : query_set(streams, /*m*/ 64, /*seed*/ 17)) {
    const f64 approx = lib::online_corr_to_pool(q, store, idx);
    const f64 exact = exact_max_abs_corr(q, store);
    if (std::abs(approx - exact) < 1e-12) { // approx is EXACT when the argmax is recalled
      ++hits;
    }
    ++Q;
  }
  ASSERT_GT(Q, 0);
  // MEASURED on this fixture (K=64, b=8, L=8, probes=1): recall = 64/64 = 1.00.
  EXPECT_GE(static_cast<double>(hits) / Q, 0.90) // documented recall bound
      << "recall = " << hits << "/" << Q;
}

TEST(LibraryCorrIndex, NeighborSetMuchSmallerThanPool) { // the speedup
  const auto streams = random_pnl_pool(2000, 128, /*seed*/ 1);
  lib::LibraryStore store = staged(streams, "small");
  lib::CorrNeighborIndex idx(/*seed*/ 1, /*T*/ 128, /*K*/ 64);
  add_all(idx, store);
  // MEASURED on this fixture: median neighbors = 62 of N = 2000 -> ratio = 0.031
  // (3.1% of the pool; the o(N) speedup), well under the 0.2*N bound.
  const double med = median_neighbor_count(idx, store);
  EXPECT_LT(med, 0.2 * static_cast<double>(store.n_alphas()));
}

TEST(LibraryCorrIndex, IdenticalStreamIsItsOwnNeighborCorrOne) {
  std::vector<std::vector<f64>> one = random_pnl_pool(1, 128, /*seed*/ 5);
  lib::LibraryStore store = staged(one, "ident");
  lib::CorrNeighborIndex idx(/*seed*/ 7, /*T*/ 128, /*K*/ 64);
  idx.add(AlphaId{0}, store.pnl(AlphaId{0}));
  EXPECT_NEAR(lib::online_corr_to_pool(one[0], store, idx), 1.0, 1e-9);
}

TEST(LibraryCorrIndex, SameSeedSameSignatures) { // determinism (L7)
  std::vector<std::vector<f64>> one = random_pnl_pool(1, 64, /*seed*/ 11);
  lib::CorrNeighborIndex a(/*seed*/ 42, /*T*/ 64, /*K*/ 32);
  lib::CorrNeighborIndex b(/*seed*/ 42, /*T*/ 64, /*K*/ 32);
  EXPECT_EQ(a.signature(one[0]), b.signature(one[0]));
}


}  // namespace atxtest_library_corr_index_test
