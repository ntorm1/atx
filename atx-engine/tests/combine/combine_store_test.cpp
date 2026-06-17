// combine_store_test.cpp — P4-1: AlphaStore + AlphaRecord (the alpha pool).
//
// AlphaStore is the append-only, deterministically-keyed pool that owns each
// evaluated alpha's realized PnL stream (+ optional position stream) and a
// non-owning, re-evaluable source handle. It is the data substrate the gate
// (P4-3) and combiner (P4-4) read. This unit WRAPS the as-built Phase-3
// alpha::AlphaStreams (it ingests pre-computed streams) — it does NOT recompute
// PnL (the §0-A wrap-vs-rebuild directive).
//
// Coverage (plan §8 P4-1):
//   * insert / size / get round-trip.
//   * AlphaId stable across growth (insert 3 -> ids 0,1,2, unchanged).
//   * pnl_matrix() 2x3 fixture reads back exact alpha-major row/col layout.
//   * period-count mismatch on 2nd insert -> Err (Result, not abort).
//   * empty store: size 0, pnl_matrix empty, n_periods 0.
//   * single alpha (boundary).
//   * all-NaN stream stored verbatim (reads back NaN, not 0).
//   * ingest_streams round-trip against a hand-built alpha::AlphaStreams.

#include <cmath>  // std::isnan
#include <limits> // std::numeric_limits (NaN fixture)
#include <span>   // std::span
#include <vector> // std::vector (fixture storage)

#include <gtest/gtest.h>

#include "atx/core/error.hpp" // Result, ErrorCode
#include "atx/core/types.hpp" // f64, u32, usize

#include "atx/engine/alpha/streams.hpp"   // alpha::AlphaStreams (ingest_streams source)
#include "atx/engine/combine/metrics.hpp" // combine::AlphaMetrics
#include "atx/engine/combine/store.hpp"   // combine::AlphaId, AlphaRecord, AlphaStore

namespace atxtest_combine_store_test {

using atx::f64;
using atx::u32;
using atx::usize;
using atx::core::ErrorCode;
using atx::engine::alpha::AlphaStreams;
using atx::engine::combine::AlphaId;
using atx::engine::combine::AlphaMetrics;
using atx::engine::combine::AlphaStore;

constexpr f64 kNaN = std::numeric_limits<f64>::quiet_NaN();

// A distinct, easy-to-recognize metrics value per field so a get() round-trip
// can assert the POD travelled byte-intact.
[[nodiscard]] AlphaMetrics sample_metrics(f64 base) {
  return AlphaMetrics{/*sharpe*/ base + 0.1,      /*turnover*/ base + 0.2,
                      /*returns*/ base + 0.3,     /*drawdown*/ base + 0.4,
                      /*margin*/ base + 0.5,      /*fitness*/ base + 0.6,
                      /*holding_days*/ base + 0.7};
}

TEST(AlphaStore, EmptyStoreHasNoPeriodsNoAlphasNoMatrix) {
  const AlphaStore store;
  EXPECT_EQ(store.size(), 0U);
  EXPECT_EQ(store.n_alphas(), 0U);
  EXPECT_EQ(store.n_periods(), 0U);
  EXPECT_TRUE(store.pnl_matrix().empty());
}

TEST(AlphaStore, InsertSizeGetRoundTrip) {
  AlphaStore store;
  const std::vector<f64> pnl{0.0, 1.5, -2.0};
  const std::vector<f64> pos{0.0, 0.0, 0.5, -0.5, 0.25, -0.25}; // 3 periods x 2 inst
  const AlphaMetrics m = sample_metrics(1.0);

  const auto id = store.insert(nullptr, pnl, pos, m);
  ASSERT_TRUE(id.has_value());

  EXPECT_EQ(store.size(), 1U);
  EXPECT_EQ(store.n_periods(), 3U);
  EXPECT_EQ((*id).value, 0U);

  const auto &rec = store.get(*id);
  EXPECT_EQ(rec.id.value, 0U);
  EXPECT_EQ(rec.source, nullptr);
  EXPECT_DOUBLE_EQ(rec.metrics.sharpe, m.sharpe);
  EXPECT_DOUBLE_EQ(rec.metrics.fitness, m.fitness);
  EXPECT_DOUBLE_EQ(rec.metrics.holding_days, m.holding_days);

  const std::span<const f64> row = store.pnl(*id);
  ASSERT_EQ(row.size(), 3U);
  EXPECT_DOUBLE_EQ(row[0], 0.0);
  EXPECT_DOUBLE_EQ(row[1], 1.5);
  EXPECT_DOUBLE_EQ(row[2], -2.0);
}

TEST(AlphaStore, AlphaIdStableAcrossGrowth) {
  AlphaStore store;
  const std::vector<f64> pnl{0.0, 1.0, 2.0};
  const std::vector<f64> pos(6, 0.0);

  const auto id0 = store.insert(nullptr, pnl, pos, sample_metrics(0.0));
  const auto id1 = store.insert(nullptr, pnl, pos, sample_metrics(1.0));
  const auto id2 = store.insert(nullptr, pnl, pos, sample_metrics(2.0));
  ASSERT_TRUE(id0.has_value() && id1.has_value() && id2.has_value());

  EXPECT_EQ((*id0).value, 0U);
  EXPECT_EQ((*id1).value, 1U);
  EXPECT_EQ((*id2).value, 2U);
  EXPECT_EQ(store.size(), 3U);

  // The earliest id still resolves to the first-inserted record after growth.
  EXPECT_EQ(store.get(*id0).id.value, 0U);
  EXPECT_DOUBLE_EQ(store.get(*id0).metrics.sharpe, sample_metrics(0.0).sharpe);
  EXPECT_DOUBLE_EQ(store.get(*id2).metrics.sharpe, sample_metrics(2.0).sharpe);
}

// pnl_matrix() is alpha-major row-major [n_alphas x n_periods]: row a (length
// n_periods) is alpha a's stream, laid out contiguously. A 2x3 fixture pins the
// exact flat byte order so the combiner can index it deterministically.
TEST(AlphaStore, PnlMatrixIsAlphaMajorRowMajor2x3) {
  AlphaStore store;
  const std::vector<f64> pos2(6, 0.0); // 3 periods x 2 inst (shape irrelevant here)

  const std::vector<f64> a0{10.0, 11.0, 12.0};
  const std::vector<f64> a1{20.0, 21.0, 22.0};
  ASSERT_TRUE(store.insert(nullptr, a0, pos2, sample_metrics(0.0)).has_value());
  ASSERT_TRUE(store.insert(nullptr, a1, pos2, sample_metrics(1.0)).has_value());

  EXPECT_EQ(store.n_alphas(), 2U);
  EXPECT_EQ(store.n_periods(), 3U);

  const std::span<const f64> mat = store.pnl_matrix();
  ASSERT_EQ(mat.size(), 6U); // 2 alphas * 3 periods

  // Alpha-major: [a0[0], a0[1], a0[2], a1[0], a1[1], a1[2]].
  EXPECT_DOUBLE_EQ(mat[0], 10.0);
  EXPECT_DOUBLE_EQ(mat[1], 11.0);
  EXPECT_DOUBLE_EQ(mat[2], 12.0);
  EXPECT_DOUBLE_EQ(mat[3], 20.0);
  EXPECT_DOUBLE_EQ(mat[4], 21.0);
  EXPECT_DOUBLE_EQ(mat[5], 22.0);

  // Row a == matrix slice [a*n_periods, +n_periods); equals pnl(id).
  const std::span<const f64> r1 = store.pnl(AlphaId{1});
  ASSERT_EQ(r1.size(), 3U);
  EXPECT_DOUBLE_EQ(r1[0], 20.0);
  EXPECT_DOUBLE_EQ(r1[2], 22.0);
}

TEST(AlphaStore, PeriodCountMismatchOnSecondInsertReturnsErr) {
  AlphaStore store;
  const std::vector<f64> pnl3{0.0, 1.0, 2.0};
  const std::vector<f64> pos3(6, 0.0);
  ASSERT_TRUE(store.insert(nullptr, pnl3, pos3, sample_metrics(0.0)).has_value());

  // A second stream of a different period count must be rejected (all streams
  // share n_periods, §4 contract) — as a Result Err, not an abort.
  const std::vector<f64> pnl4{0.0, 1.0, 2.0, 3.0};
  const std::vector<f64> pos4(8, 0.0);
  const auto bad = store.insert(nullptr, pnl4, pos4, sample_metrics(1.0));
  ASSERT_FALSE(bad.has_value());
  EXPECT_EQ(bad.error().code(), ErrorCode::InvalidArgument);

  // The store is unchanged by the rejected insert.
  EXPECT_EQ(store.size(), 1U);
  EXPECT_EQ(store.n_periods(), 3U);
}

TEST(AlphaStore, SingleAlphaBoundary) {
  AlphaStore store;
  const std::vector<f64> pnl{42.0};          // 1 period
  const std::vector<f64> pos{0.0, 0.0, 0.0}; // 1 period x 3 inst
  const auto id = store.insert(nullptr, pnl, pos, sample_metrics(3.0));
  ASSERT_TRUE(id.has_value());

  EXPECT_EQ(store.size(), 1U);
  EXPECT_EQ(store.n_periods(), 1U);
  EXPECT_EQ(store.n_alphas(), 1U);
  const std::span<const f64> mat = store.pnl_matrix();
  ASSERT_EQ(mat.size(), 1U);
  EXPECT_DOUBLE_EQ(mat[0], 42.0);
}

TEST(AlphaStore, AllNaNStreamStoredVerbatim) {
  AlphaStore store;
  const std::vector<f64> pnl{kNaN, kNaN, kNaN}; // pre-first-valid periods are NaN (§3.3)
  const std::vector<f64> pos(6, 0.0);
  const auto id = store.insert(nullptr, pnl, pos, sample_metrics(0.0));
  ASSERT_TRUE(id.has_value());

  const std::span<const f64> row = store.pnl(*id);
  ASSERT_EQ(row.size(), 3U);
  // NaN must round-trip as NaN, NOT be coerced to 0.
  EXPECT_TRUE(std::isnan(row[0]));
  EXPECT_TRUE(std::isnan(row[1]));
  EXPECT_TRUE(std::isnan(row[2]));
}

// ingest_streams collapses a batch alpha::AlphaStreams (the §0-A "one
// extract_streams call" path) into the pool, one insert() per alpha row in id
// order. AlphaStreams is directly aggregate-constructible (public members), so a
// 2-alpha x 3-period x 2-instrument fixture is cheap to hand-build here without
// the full extract_streams pipeline.
TEST(AlphaStore, IngestStreamsRoundTrip) {
  AlphaStreams streams;
  streams.n_alphas_ = 2;
  streams.n_periods_ = 3;
  streams.n_instruments_ = 2;
  // pnl_flat is alpha-major [2 x 3].
  streams.pnl_flat = {0.0, 1.0, 2.0, 0.0, 3.0, 4.0};
  // pos_flat is [2 alphas x 3 periods x 2 inst]; alpha 1's period 0 is {0.9,0.8}.
  streams.pos_flat = {0.0, 0.0, 0.1, 0.2, 0.3, 0.4, 0.9, 0.8, 0.5, 0.6, 0.7, 0.8};

  // ISignalSource* sources are non-owning; nullptr is the documented "no re-eval"
  // sentinel for a unit test (re-eval is exercised in P4-5's integration test).
  const std::vector<atx::engine::ISignalSource *> sources{nullptr, nullptr};
  const std::vector<AlphaMetrics> metrics{sample_metrics(0.0), sample_metrics(1.0)};

  AlphaStore store;
  const auto status = store.ingest_streams(streams, sources, metrics);
  ASSERT_TRUE(status.has_value());

  EXPECT_EQ(store.size(), 2U);
  EXPECT_EQ(store.n_periods(), 3U);

  // Each alpha row copied in id order, matching streams.pnl(a).
  const std::span<const f64> r0 = store.pnl(AlphaId{0});
  const std::span<const f64> r1 = store.pnl(AlphaId{1});
  ASSERT_EQ(r0.size(), 3U);
  ASSERT_EQ(r1.size(), 3U);
  EXPECT_DOUBLE_EQ(r0[1], 1.0);
  EXPECT_DOUBLE_EQ(r1[2], 4.0);

  // Position stream copied per period: alpha 1, period 0 == {0.9, 0.8}.
  const std::span<const f64> p10 = store.positions(AlphaId{1}, 0);
  ASSERT_EQ(p10.size(), 2U);
  EXPECT_DOUBLE_EQ(p10[0], 0.9);
  EXPECT_DOUBLE_EQ(p10[1], 0.8);

  EXPECT_DOUBLE_EQ(store.get(AlphaId{1}).metrics.sharpe, sample_metrics(1.0).sharpe);
}


}  // namespace atxtest_combine_store_test
