// track_store.hpp -- Parquet track writer + compactor (Task D2,
// backtest-production-lakehouse sprint).
//
// The brief's Step 1 gate, verbatim: write a 3-track staging set, compact,
// read back via Arrow C++ -- row counts, per-column values (spot-check cells
// vs the source BacktestResult), hive path `underlier=SPY/family=strangle_hedged`,
// one row group per batch, zstd codec asserted from file metadata. That is
// `TrackStoreTest.RoundTripThreeTracksAcrossTwoPartitions` below; the smaller
// tests around it cover shape/meta validation and additive re-compaction.
//
// Only built when ATX_VOL_LAKEHOUSE is ON (tests/CMakeLists.txt) -- the
// library entry points this file exercises do not exist in the OFF build.

#include "atx/vol/research/track_store.hpp"

#include <arrow/array.h>
#include <arrow/io/file.h>
#include <arrow/table.h>
#include <parquet/arrow/reader.h>
#include <parquet/metadata.h>
#include <parquet/types.h>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <system_error>
#include <vector>

#include <gtest/gtest.h>

#include "atx/core/datetime.hpp" // time::days_from_civil (independent date-epoch oracle)
#include "atx/vol/backtest.hpp"  // BacktestResult

using namespace atx::vol;
namespace fs = std::filesystem;

namespace {

constexpr std::int64_t kBaseNow = 1767312000000000000LL; // 2026-01-02T00:00:00Z-ish, exact value unused
constexpr std::int64_t kDayNs = 86400LL * 1000000000LL;

[[nodiscard]] fs::path fresh_dir(const char *tag) {
  const fs::path dir = fs::temp_directory_path() / (std::string("atx-track-store-") + tag);
  std::error_code ec;
  fs::remove_all(dir, ec);
  return dir;
}

[[nodiscard]] TrackKey make_key(std::uint8_t fill_byte) {
  TrackKey k;
  k.sha256.fill(fill_byte);
  return k;
}

// Deterministic, fully-populated-except-for-the-optional-lanes BacktestResult.
// Every one of the 25 frozen series columns gets a distinct, checkable value
// (base_value + <column offset> + 0.001*row). `swap_pv`/`swap_pnl`,
// `gross_vega_abs`/`nav_liquidation`, and `step_pnl_total` are populated only
// when their flag is set -- otherwise left empty, exercising TrackStore's
// empty-lane -> all-NULL contract.
[[nodiscard]] BacktestResult make_result(std::vector<std::string> dates, std::int64_t base_ts_ns,
                                         bool populate_swap, bool populate_gross_vega_nav_liq,
                                         bool populate_step_pnl_total, double base_value) {
  BacktestResult r;
  const std::size_t n = dates.size();
  r.date = std::move(dates);
  r.ts_ns.resize(n);
  for (std::size_t i = 0; i < n; ++i) {
    r.ts_ns[i] = base_ts_ns + static_cast<std::int64_t>(i) * kDayNs;
  }
  const auto fill = [&](double offset) {
    std::vector<double> v(n);
    for (std::size_t i = 0; i < n; ++i) {
      v[i] = base_value + offset + static_cast<double>(i) * 0.001;
    }
    return v;
  };
  r.pnl_total = fill(1.0);
  r.pnl_delta = fill(2.0);
  r.pnl_gamma = fill(3.0);
  r.pnl_vega = fill(4.0);
  r.pnl_vanna = fill(5.0);
  r.pnl_volga = fill(6.0);
  r.pnl_theta = fill(7.0);
  r.pnl_rho = fill(8.0);
  r.pnl_charm = fill(9.0);
  r.pnl_unexplained = fill(10.0);
  r.pnl_settlement = fill(11.0);
  r.pnl_shares = fill(12.0);
  r.financing = fill(13.0);
  r.cost = fill(14.0);
  r.nav = fill(15.0);
  r.cash = fill(16.0);
  r.gross_delta = fill(17.0);
  r.gross_gamma = fill(18.0);
  r.gross_vega = fill(19.0);
  r.gross_theta = fill(20.0);
  r.turnover_notional = fill(21.0);
  r.turnover_vega = fill(22.0);
  r.n_open_lots = fill(23.0);
  r.n_unpriced_lots = fill(24.0);
  r.n_unpriced_greeks = fill(25.0);
  if (populate_swap) {
    r.swap_pv = fill(26.0);
    r.swap_pnl = fill(27.0);
  }
  if (populate_gross_vega_nav_liq) {
    r.gross_vega_abs = fill(28.0);
    r.nav_liquidation = fill(29.0);
  }
  if (populate_step_pnl_total && n >= 1) {
    std::vector<double> step(n - 1);
    for (std::size_t i = 0; i + 1 < n; ++i) {
      step[i] = base_value + 30.0 + static_cast<double>(i) * 0.001;
    }
    r.step_pnl_total = std::move(step);
  }
  return r;
}

struct ReadBatch {
  std::shared_ptr<arrow::Table> table;
  std::shared_ptr<parquet::FileMetaData> meta;
};

[[nodiscard]] ReadBatch read_batch(const std::string &path) {
  auto in = arrow::io::ReadableFile::Open(path);
  EXPECT_TRUE(in.ok()) << (in.ok() ? std::string{} : in.status().ToString());
  auto reader_res = parquet::arrow::OpenFile(*in, arrow::default_memory_pool());
  EXPECT_TRUE(reader_res.ok()) << (reader_res.ok() ? std::string{} : reader_res.status().ToString());
  std::unique_ptr<parquet::arrow::FileReader> reader = *std::move(reader_res);
  std::shared_ptr<parquet::FileMetaData> meta = reader->parquet_reader()->metadata();
  auto table_res = reader->ReadTable();
  EXPECT_TRUE(table_res.ok()) << (table_res.ok() ? std::string{} : table_res.status().ToString());
  std::shared_ptr<arrow::Table> table = *table_res;
  auto combined = table->CombineChunks(arrow::default_memory_pool());
  EXPECT_TRUE(combined.ok());
  return ReadBatch{*combined, meta};
}

[[nodiscard]] double dbl_at(const arrow::Table &table, const std::string &col, std::int64_t row) {
  const int idx = table.schema()->GetFieldIndex(col);
  EXPECT_GE(idx, 0) << col;
  const auto &arr = static_cast<const arrow::DoubleArray &>(*table.column(idx)->chunk(0));
  return arr.Value(row);
}

[[nodiscard]] bool is_null_at(const arrow::Table &table, const std::string &col, std::int64_t row) {
  const int idx = table.schema()->GetFieldIndex(col);
  EXPECT_GE(idx, 0) << col;
  return table.column(idx)->chunk(0)->IsNull(row);
}

[[nodiscard]] std::string str_at(const arrow::Table &table, const std::string &col, std::int64_t row) {
  const int idx = table.schema()->GetFieldIndex(col);
  EXPECT_GE(idx, 0) << col;
  const auto &arr = static_cast<const arrow::StringArray &>(*table.column(idx)->chunk(0));
  return arr.GetString(row);
}

[[nodiscard]] std::int32_t date_at(const arrow::Table &table, std::int64_t row) {
  const int idx = table.schema()->GetFieldIndex("date");
  EXPECT_GE(idx, 0);
  const auto &arr = static_cast<const arrow::Date32Array &>(*table.column(idx)->chunk(0));
  return arr.Value(row);
}

} // namespace

// ── The Step-1 gate: 3 tracks, 2 hive partitions, full round trip ──────────
TEST(TrackStoreTest, RoundTripThreeTracksAcrossTwoPartitions) {
  const fs::path dir = fresh_dir("roundtrip");
  std::error_code mkdir_ec;
  fs::create_directories(dir, mkdir_ec);
  ASSERT_FALSE(mkdir_ec) << mkdir_ec.message();

  const TrackKey key_a = make_key(0x11); // hex "1111...1" -- sorts first
  const TrackKey key_b = make_key(0x22); // hex "2222...2" -- same partition as A, sorts second
  const TrackKey key_c = make_key(0x33); // hex "3333...3" -- different partition

  const BacktestResult track_a =
      make_result({"2026-01-02", "2026-01-03", "2026-01-04", "2026-01-05"}, kBaseNow,
                 /*populate_swap=*/true, /*populate_gross_vega_nav_liq=*/true,
                 /*populate_step_pnl_total=*/true, /*base_value=*/1000.0);
  const BacktestResult track_b =
      make_result({"2026-01-06", "2026-01-07", "2026-01-08"}, kBaseNow + 4 * kDayNs,
                 /*populate_swap=*/false, /*populate_gross_vega_nav_liq=*/false,
                 /*populate_step_pnl_total=*/false, /*base_value=*/2000.0);
  const BacktestResult track_c =
      make_result({"2026-02-02", "2026-02-03", "2026-02-04", "2026-02-05", "2026-02-06"},
                 kBaseNow + 30 * kDayNs, /*populate_swap=*/true, /*populate_gross_vega_nav_liq=*/false,
                 /*populate_step_pnl_total=*/true, /*base_value=*/3000.0);

  TrackStore store(dir.string());
  const TrackMeta meta_spy{"SPY", "strangle_hedged"};
  const TrackMeta meta_aapl{"AAPL", "iron_condor"};

  {
    const Status st = store.write_staging(key_a, track_a, meta_spy);
    ASSERT_TRUE(st.has_value()) << (st.has_value() ? std::string{} : st.error().to_string());
  }
  {
    const Status st = store.write_staging(key_b, track_b, meta_spy);
    ASSERT_TRUE(st.has_value()) << (st.has_value() ? std::string{} : st.error().to_string());
  }
  {
    const Status st = store.write_staging(key_c, track_c, meta_aapl);
    ASSERT_TRUE(st.has_value()) << (st.has_value() ? std::string{} : st.error().to_string());
  }

  // 3 staging files, one per track, before compaction.
  std::error_code count_ec;
  std::size_t staged_count = 0;
  for (const auto &entry : fs::directory_iterator(dir / "staging", count_ec)) {
    (void)entry;
    ++staged_count;
  }
  EXPECT_EQ(staged_count, 3u);

  const Result<CompactStats> compacted = atx::vol::compact(dir.string());
  ASSERT_TRUE(compacted.has_value()) << (compacted.has_value() ? std::string{} : compacted.error().to_string());
  EXPECT_EQ(compacted->tracks_compacted, 3u);
  EXPECT_EQ(compacted->batch_files_written, 2u);
  EXPECT_EQ(compacted->staged_files_deleted, 3u);

  // Staged inputs are gone -- deleted only after their batch's atomic rename
  // landed, which by this point it has.
  std::error_code exists_ec;
  std::size_t remaining_staged = 0;
  for (const auto &entry : fs::directory_iterator(dir / "staging", exists_ec)) {
    (void)entry;
    ++remaining_staged;
  }
  EXPECT_EQ(remaining_staged, 0u);

  // Hive path: underlier=SPY/family=strangle_hedged (brief's literal example).
  const fs::path spy_batch = dir / "tracks" / "underlier=SPY" / "family=strangle_hedged" / "batch-000000.parquet";
  const fs::path aapl_batch = dir / "tracks" / "underlier=AAPL" / "family=iron_condor" / "batch-000000.parquet";
  ASSERT_TRUE(fs::exists(spy_batch)) << spy_batch.string();
  ASSERT_TRUE(fs::exists(aapl_batch)) << aapl_batch.string();

  // ── SPY/strangle_hedged batch: track_a (4 rows) + track_b (3 rows) = 7 ────
  const ReadBatch spy = read_batch(spy_batch.string());
  ASSERT_EQ(spy.table->num_rows(), 7);
  ASSERT_EQ(spy.table->num_columns(), 33) << "track_key + date + ts_ns + 25 series + 5 swap-lane";

  // One row group per batch (brief, Step 1).
  ASSERT_EQ(spy.meta->num_row_groups(), 1);

  // zstd codec, asserted from file metadata (brief, Step 1) -- every column.
  const auto spy_rg0 = spy.meta->RowGroup(0);
  for (int c = 0; c < spy.table->num_columns(); ++c) {
    EXPECT_EQ(spy_rg0->ColumnChunk(c)->compression(), parquet::Compression::ZSTD) << "column " << c;
  }

  // Sorting-column metadata: (track_key, date), both ascending.
  const std::vector<parquet::SortingColumn> spy_sort = spy_rg0->sorting_columns();
  ASSERT_EQ(spy_sort.size(), 2u);
  EXPECT_EQ(spy_sort[0].column_idx, spy.table->schema()->GetFieldIndex("track_key"));
  EXPECT_FALSE(spy_sort[0].descending);
  EXPECT_EQ(spy_sort[1].column_idx, spy.table->schema()->GetFieldIndex("date"));
  EXPECT_FALSE(spy_sort[1].descending);

  // Rows sorted by (track_key, date): track_a's 4 rows (key "111...1") precede
  // track_b's 3 rows (key "222...2"); within each track, date is ascending.
  for (int64_t i = 0; i < spy.table->num_rows() - 1; ++i) {
    const std::string key_i = str_at(*spy.table, "track_key", i);
    const std::string key_next = str_at(*spy.table, "track_key", i + 1);
    const std::int32_t date_i = date_at(*spy.table, i);
    const std::int32_t date_next = date_at(*spy.table, i + 1);
    EXPECT_TRUE(key_i < key_next || (key_i == key_next && date_i < date_next))
        << "row " << i << " not < row " << (i + 1) << " under (track_key, date) order";
  }
  for (int64_t i = 0; i < 4; ++i) {
    EXPECT_EQ(str_at(*spy.table, "track_key", i), key_a.hex());
  }
  for (int64_t i = 4; i < 7; ++i) {
    EXPECT_EQ(str_at(*spy.table, "track_key", i), key_b.hex());
  }

  // ── Spot-check 5+ cells vs the source BacktestResult (bit-exact) ──────────
  // 1. track_a row 0: nav.
  EXPECT_EQ(dbl_at(*spy.table, "nav", 0), track_a.nav[0]);
  // 2. track_a row 2: pnl_vega.
  EXPECT_EQ(dbl_at(*spy.table, "pnl_vega", 2), track_a.pnl_vega[2]);
  // 3. track_a row 2: swap_pv (populated lane) is non-null and exact.
  EXPECT_FALSE(is_null_at(*spy.table, "swap_pv", 2));
  EXPECT_EQ(dbl_at(*spy.table, "swap_pv", 2), track_a.swap_pv[2]);
  // 4. track_b (rows 4..6) has an EMPTY swap lane in the source -> every row
  //    stored NULL, never a fabricated 0.0.
  EXPECT_TRUE(is_null_at(*spy.table, "swap_pv", 4));
  EXPECT_TRUE(is_null_at(*spy.table, "swap_pnl", 5));
  EXPECT_TRUE(is_null_at(*spy.table, "gross_vega_abs", 6));
  // 5. track_b row 1 (combined row 5): gross_theta, exact.
  EXPECT_EQ(dbl_at(*spy.table, "gross_theta", 5), track_b.gross_theta[1]);
  // date32 for track_a row 0 ("2026-01-02"), independent oracle.
  EXPECT_EQ(date_at(*spy.table, 0), atx::core::time::days_from_civil(2026, 1, 2));
  // ts_ns zero-copy round trip.
  EXPECT_EQ(dbl_at(*spy.table, "cash", 1), track_a.cash[1]);

  // ── AAPL/iron_condor batch: track_c (5 rows) ───────────────────────────────
  const ReadBatch aapl = read_batch(aapl_batch.string());
  ASSERT_EQ(aapl.table->num_rows(), 5);
  ASSERT_EQ(aapl.meta->num_row_groups(), 1);
  const auto aapl_rg0 = aapl.meta->RowGroup(0);
  for (int c = 0; c < aapl.table->num_columns(); ++c) {
    EXPECT_EQ(aapl_rg0->ColumnChunk(c)->compression(), parquet::Compression::ZSTD) << "column " << c;
  }

  // step_pnl_total: row 0 (inception) is always NULL; row i>=1 is
  // source.step_pnl_total[i-1], bit-exact. track_c's gross_vega_abs/
  // nav_liquidation lane was left empty -> NULL on every row.
  EXPECT_TRUE(is_null_at(*aapl.table, "step_pnl_total", 0));
  EXPECT_FALSE(is_null_at(*aapl.table, "step_pnl_total", 3));
  EXPECT_EQ(dbl_at(*aapl.table, "step_pnl_total", 3), track_c.step_pnl_total[2]);
  EXPECT_TRUE(is_null_at(*aapl.table, "gross_vega_abs", 0));
  EXPECT_TRUE(is_null_at(*aapl.table, "nav_liquidation", 4));
  EXPECT_EQ(dbl_at(*aapl.table, "cost", 3), track_c.cost[3]);
  for (int64_t i = 0; i < 5; ++i) {
    EXPECT_EQ(str_at(*aapl.table, "track_key", i), key_c.hex());
  }

  std::error_code cleanup_ec;
  fs::remove_all(dir, cleanup_ec);
}

// ── compact() on an absent/empty staging/ is a clean no-op, not an error ───
TEST(TrackStoreTest, CompactWithNoStagingIsANoOp) {
  const fs::path dir = fresh_dir("no-staging");
  std::error_code mkdir_ec;
  fs::create_directories(dir, mkdir_ec);

  const Result<CompactStats> result = atx::vol::compact(dir.string());
  ASSERT_TRUE(result.has_value()) << (result.has_value() ? std::string{} : result.error().to_string());
  EXPECT_EQ(result->tracks_compacted, 0u);
  EXPECT_EQ(result->batch_files_written, 0u);
  EXPECT_EQ(result->staged_files_deleted, 0u);

  std::error_code cleanup_ec;
  fs::remove_all(dir, cleanup_ec);
}

// ── write_staging validates BacktestResult shape before touching disk ──────
TEST(TrackStoreTest, WriteStagingRejectsEmptyResult) {
  const fs::path dir = fresh_dir("empty-result");
  TrackStore store(dir.string());
  const BacktestResult empty{};
  const TrackMeta meta{"SPY", "strangle_hedged"};

  const Status st = store.write_staging(make_key(0x44), empty, meta);
  ASSERT_FALSE(st.has_value());
  EXPECT_EQ(st.error().code(), ErrorCode::InvalidArgument);
  EXPECT_FALSE(fs::exists(dir / "staging"));

  std::error_code ec;
  fs::remove_all(dir, ec);
}

// ── write_staging validates TrackMeta before touching disk ─────────────────
TEST(TrackStoreTest, WriteStagingRejectsHiveUnsafeMeta) {
  const fs::path dir = fresh_dir("bad-meta");
  TrackStore store(dir.string());
  const BacktestResult r = make_result({"2026-01-02"}, kBaseNow, false, false, false, 1.0);

  {
    const TrackMeta bad_underlier{"SP/Y", "strangle_hedged"};
    const Status st = store.write_staging(make_key(0x55), r, bad_underlier);
    ASSERT_FALSE(st.has_value());
    EXPECT_EQ(st.error().code(), ErrorCode::InvalidArgument);
  }
  {
    const TrackMeta empty_family{"SPY", ""};
    const Status st = store.write_staging(make_key(0x66), r, empty_family);
    ASSERT_FALSE(st.has_value());
    EXPECT_EQ(st.error().code(), ErrorCode::InvalidArgument);
  }
  EXPECT_FALSE(fs::exists(dir / "staging"));

  std::error_code ec;
  fs::remove_all(dir, ec);
}

// ── compact() is additive across runs: batch-000001 never clobbers batch-000000 ──
TEST(TrackStoreTest, CompactIsAdditiveAcrossRuns) {
  const fs::path dir = fresh_dir("additive");
  std::error_code mkdir_ec;
  fs::create_directories(dir, mkdir_ec);

  TrackStore store(dir.string());
  const TrackMeta meta{"SPY", "strangle_hedged"};

  const BacktestResult track_d = make_result({"2026-03-02", "2026-03-03"}, kBaseNow, false, false, false, 4000.0);
  ASSERT_TRUE(store.write_staging(make_key(0x77), track_d, meta).has_value());
  const Result<CompactStats> first = atx::vol::compact(dir.string());
  ASSERT_TRUE(first.has_value());
  EXPECT_EQ(first->batch_files_written, 1u);
  const fs::path batch0 = dir / "tracks" / "underlier=SPY" / "family=strangle_hedged" / "batch-000000.parquet";
  ASSERT_TRUE(fs::exists(batch0));
  const auto batch0_size_before = fs::file_size(batch0);

  const BacktestResult track_e = make_result({"2026-03-04", "2026-03-05"}, kBaseNow, false, false, false, 5000.0);
  ASSERT_TRUE(store.write_staging(make_key(0x88), track_e, meta).has_value());
  const Result<CompactStats> second = atx::vol::compact(dir.string());
  ASSERT_TRUE(second.has_value());
  EXPECT_EQ(second->batch_files_written, 1u);
  const fs::path batch1 = dir / "tracks" / "underlier=SPY" / "family=strangle_hedged" / "batch-000001.parquet";
  ASSERT_TRUE(fs::exists(batch1)) << "second compact() must continue the batch index, not overwrite batch-000000";

  // batch-000000 is untouched by the second run.
  EXPECT_EQ(fs::file_size(batch0), batch0_size_before);
  const ReadBatch first_batch = read_batch(batch0.string());
  EXPECT_EQ(first_batch.table->num_rows(), 2);
  const ReadBatch second_batch = read_batch(batch1.string());
  EXPECT_EQ(second_batch.table->num_rows(), 2);

  std::error_code ec;
  fs::remove_all(dir, ec);
}
