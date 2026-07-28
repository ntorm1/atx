#include "atx/vol/backtest_db.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "atx/vol/backtest_series_columns.hpp"

namespace atx::vol {
namespace {

[[nodiscard]] std::filesystem::path test_root(std::string_view stem) {
  static std::atomic<std::uint64_t> sequence{0};
  const std::filesystem::path root =
      std::filesystem::temp_directory_path() /
      ("atx_backtest_db_" + std::string(stem) + "_" + std::to_string(sequence.fetch_add(1)));
  std::error_code ec;
  std::filesystem::remove_all(root, ec);
  return root;
}

[[nodiscard]] BacktestStrategyTemplate make_template(std::string id) {
  auto made = make_40_delta_3_calendar_month_strangle_template();
  EXPECT_TRUE(made.has_value()) << (made ? "" : made.error().to_string());
  BacktestStrategyTemplate value = made ? std::move(*made) : BacktestStrategyTemplate{};
  value.id = std::move(id);
  value.name = "Daily delta-hedged 40d 3m strangle";
  return value;
}

[[nodiscard]] BacktestSeriesData make_series(std::uint32_t uid, std::string symbol = "AAPL") {
  BacktestSeriesData data;
  BacktestResult &result = data.backtest;
  result.date = {"2026-01-02", "2026-01-05", "2026-01-06"};
  result.ts_ns = {100, 200, 300};
  const std::vector<double> values{0.0, 1.0, 2.0};
  result.pnl_total = values;
  result.pnl_delta = values;
  result.pnl_gamma = values;
  result.pnl_vega = values;
  result.pnl_vanna = values;
  result.pnl_volga = values;
  result.pnl_theta = values;
  result.pnl_rho = values;
  result.pnl_charm = values;
  result.pnl_unexplained = values;
  result.pnl_settlement = values;
  result.pnl_shares = values;
  result.financing = values;
  result.cost = values;
  result.nav = values;
  result.cash = values;
  result.gross_delta = values;
  result.gross_gamma = values;
  result.gross_vega = values;
  result.gross_theta = values;
  result.turnover_notional = values;
  result.turnover_vega = values;
  result.n_open_lots = values;
  result.n_unpriced_lots = values;
  result.n_unpriced_greeks = values;
  result.pnl_total = {0.0, 1.25, -0.25};
  result.nav = {0.0, 1.25, 1.0};
  result.cash = {0.0, 10.0, 11.0};
  result.n_open_lots = {1.0, 1.0, 1.0};
  result.gross_vega_abs = {25.0, 24.0, 23.0};
  result.nav_liquidation = result.nav;
  result.step_pnl_total = {1.25, -0.25};
  result.signals = {{"entry_count", {1.0, 0.0, 0.0}}};

  Lot lot;
  lot.id = 1;
  lot.contract = OptionContract{uid, 100.0, 0.20, Side::Call};
  lot.qty = 1.0;
  lot.multiplier = 100.0;
  lot.expiry_ts_ns = 1'000;
  lot.cohort = 0;
  lot.entry_price = 2.5;
  data.checkpoint.base_ts_ns = result.ts_ns.back();
  data.checkpoint.completed_step_index = result.size() - 1;
  data.checkpoint.next_lot_id = 2;
  data.checkpoint.portfolio.lots = {lot};
  data.checkpoint.hedge_shares = {{uid, -12.5}};
  data.checkpoint.cash = result.cash.back();
  data.checkpoint.nav = result.nav.back();
  data.checkpoint.cumulative_noncash_financing = 0.125;
  data.next_cohort = 1;
  for (std::size_t i = 0; i < result.size(); ++i) {
    data.sources.push_back(BacktestSourcePartition{
        result.date[i],
        ArchiveContentIdentity{1'000 + i, 10 + i, static_cast<std::uint32_t>(20 + i),
                               static_cast<std::uint32_t>(30 + i)}});
  }
  (void)symbol;
  return data;
}

TEST(BacktestDbCatalog, CreateOpenAndRegisterTemplateRoundTrip) {
  const std::filesystem::path root = test_root("catalog");
  auto db = BacktestDb::create(root.string());
  ASSERT_TRUE(db.has_value()) << (db ? "" : db.error().to_string());
  EXPECT_EQ(db->generation(), 1u);
  const BacktestStrategyTemplate value = make_template("strangle");
  ASSERT_TRUE(db->register_template(value).has_value());
  const std::uint64_t generation = db->generation();
  ASSERT_TRUE(db->register_template(value).has_value());
  EXPECT_EQ(db->generation(), generation) << "idempotent registration must not rewrite";

  auto reopened = BacktestDb::open(root.string());
  ASSERT_TRUE(reopened.has_value()) << (reopened ? "" : reopened.error().to_string());
  auto stored = reopened->find_template("strangle");
  ASSERT_TRUE(stored.has_value());
  EXPECT_EQ(fingerprint_backtest_template(*stored), fingerprint_backtest_template(value));
  EXPECT_EQ(stored->name, value.name);
  std::filesystem::remove_all(root);
}

TEST(BacktestDbCatalog, ChangedEconomicsUnderExistingIdRejected) {
  const std::filesystem::path root = test_root("changed_id");
  auto db = BacktestDb::create(root.string());
  ASSERT_TRUE(db.has_value());
  BacktestStrategyTemplate value = make_template("same-id");
  ASSERT_TRUE(db->register_template(value).has_value());
  value.legs.front().quantity = 2.0;
  const Status changed = db->register_template(value);
  ASSERT_FALSE(changed.has_value());
  EXPECT_EQ(changed.error().code(), ErrorCode::AlreadyExists);
  std::filesystem::remove_all(root);
}

TEST(BacktestDbCatalog, EconomicAliasesUseDistinctPartitionFilenames) {
  const std::filesystem::path root = test_root("aliases");
  auto db = BacktestDb::create(root.string());
  ASSERT_TRUE(db.has_value());
  const BacktestStrategyTemplate first = make_template("alias-one");
  const BacktestStrategyTemplate second = make_template("alias-two");
  ASSERT_EQ(fingerprint_backtest_template(first), fingerprint_backtest_template(second));
  ASSERT_TRUE(db->register_template(first).has_value());
  ASSERT_TRUE(db->register_template(second).has_value());
  ASSERT_TRUE(db->write_series(first.id, "AAPL", 101, make_series(101)).has_value());
  ASSERT_TRUE(db->write_series(second.id, "AAPL", 101, make_series(101)).has_value());
  auto first_info = db->find_series(first.id, "AAPL");
  auto second_info = db->find_series(second.id, "AAPL");
  ASSERT_TRUE(first_info.has_value());
  ASSERT_TRUE(second_info.has_value());
  EXPECT_NE(first_info->partition_filename, second_info->partition_filename);
  EXPECT_TRUE(db->load_series(first.id, "AAPL").has_value());
  EXPECT_TRUE(db->load_series(second.id, "AAPL").has_value());
  std::filesystem::remove_all(root);
}

TEST(BacktestDbSeries, SeriesAndCheckpointRoundTrip) {
  const std::filesystem::path root = test_root("series");
  auto db = BacktestDb::create(root.string());
  ASSERT_TRUE(db.has_value());
  const BacktestStrategyTemplate value = make_template("strangle");
  ASSERT_TRUE(db->register_template(value).has_value());
  const BacktestSeriesData input = make_series(101);
  ASSERT_TRUE(db->write_series(value.id, "aapl", 101, input).has_value());

  auto info = db->find_series(value.id, "AAPL");
  ASSERT_TRUE(info.has_value());
  EXPECT_EQ(info->row_count, 3u);
  EXPECT_EQ(info->run_identity_hash,
            backtest_series_identity(info->template_fingerprint, info->uid, input.sources));
  auto loaded = db->load_series(value.id, "AAPL");
  ASSERT_TRUE(loaded.has_value()) << (loaded ? "" : loaded.error().to_string());
  EXPECT_EQ(loaded->backtest.date, input.backtest.date);
  EXPECT_EQ(loaded->backtest.pnl_total, input.backtest.pnl_total);
  EXPECT_EQ(loaded->backtest.step_pnl_total, input.backtest.step_pnl_total);
  EXPECT_EQ(loaded->backtest.gross_vega_abs, input.backtest.gross_vega_abs);
  EXPECT_EQ(loaded->backtest.nav_liquidation, input.backtest.nav_liquidation);
  ASSERT_EQ(loaded->backtest.signals.size(), 1u);
  EXPECT_EQ(loaded->backtest.signals.front(), input.backtest.signals.front());
  ASSERT_EQ(loaded->checkpoint.portfolio.lots.size(), 1u);
  EXPECT_EQ(loaded->checkpoint.portfolio.lots.front().id, 1u);
  ASSERT_EQ(loaded->checkpoint.hedge_shares.size(), 1u);
  EXPECT_EQ(loaded->checkpoint.hedge_shares.front().uid, 101u);
  EXPECT_DOUBLE_EQ(loaded->checkpoint.hedge_shares.front().shares, -12.5);
  EXPECT_EQ(loaded->next_cohort, 1u);
  EXPECT_EQ(loaded->sources, input.sources);
  std::filesystem::remove_all(root);
}

TEST(BacktestDbSeries, CallerMetadataIsAnOptimisticConcurrencyToken) {
  const std::filesystem::path root = test_root("caller_metadata");
  auto db = BacktestDb::create(root.string());
  ASSERT_TRUE(db.has_value());
  const BacktestStrategyTemplate value = make_template("strangle");
  ASSERT_TRUE(db->register_template(value).has_value());
  const BacktestSeriesData original = make_series(101);
  ASSERT_TRUE(db->write_series(value.id, "AAPL", 101, original).has_value());
  auto original_info = db->find_series(value.id, "AAPL");
  ASSERT_TRUE(original_info.has_value());

  BacktestSeriesData revised = original;
  revised.backtest.pnl_delta[1] += 0.5;
  ASSERT_TRUE(db->write_series(*original_info, revised).has_value());
  auto revised_info = db->find_series(value.id, "AAPL");
  ASSERT_TRUE(revised_info.has_value());
  EXPECT_NE(revised_info->partition_filename, original_info->partition_filename);
  EXPECT_NE(revised_info->partition_identity, original_info->partition_identity);

  const Status stale = db->write_series(*original_info, original);
  ASSERT_FALSE(stale.has_value());
  EXPECT_EQ(stale.error().code(), ErrorCode::InvalidArgument);
  auto loaded = db->load_series(value.id, "AAPL");
  ASSERT_TRUE(loaded.has_value());
  EXPECT_EQ(loaded->backtest.pnl_delta, revised.backtest.pnl_delta);
  std::filesystem::remove_all(root);
}

TEST(BacktestDbSeries, VersionedWriteLeavesOldManifestSeriesLoadable) {
  const std::filesystem::path root = test_root("crash_safe_update");
  const std::filesystem::path manifest = root / kBacktestDbManifestName;
  const std::filesystem::path saved_manifest = root / "manifest.before-update";
  const BacktestStrategyTemplate value = make_template("strangle");
  const BacktestSeriesData original = make_series(101);
  std::string original_filename;
  {
    auto db = BacktestDb::create(root.string());
    ASSERT_TRUE(db.has_value());
    ASSERT_TRUE(db->register_template(value).has_value());
    ASSERT_TRUE(db->write_series(value.id, "AAPL", 101, original).has_value());
    auto original_info = db->find_series(value.id, "AAPL");
    ASSERT_TRUE(original_info.has_value());
    original_filename = original_info->partition_filename;
    std::error_code ec;
    ASSERT_TRUE(std::filesystem::copy_file(manifest, saved_manifest,
                                           std::filesystem::copy_options::overwrite_existing, ec))
        << ec.message();

    BacktestSeriesData revised = original;
    revised.backtest.pnl_delta[1] += 0.5;
    ASSERT_TRUE(db->write_series(*original_info, revised).has_value());
    auto revised_info = db->find_series(value.id, "AAPL");
    ASSERT_TRUE(revised_info.has_value());
    ASSERT_NE(revised_info->partition_filename, original_filename);
    EXPECT_TRUE(std::filesystem::exists(root / kBacktestDbPartitionDir / original_filename));
  }

  // Model a crash after publishing the new immutable partition but before
  // publishing its manifest: restore the prior manifest snapshot. Its indexed
  // partition must still exist and pass the complete load validation.
  {
    std::error_code ec;
    ASSERT_TRUE(std::filesystem::copy_file(saved_manifest, manifest,
                                           std::filesystem::copy_options::overwrite_existing, ec))
        << ec.message();
  }
  auto recovered = BacktestDb::open(root.string());
  ASSERT_TRUE(recovered.has_value()) << (recovered ? "" : recovered.error().to_string());
  auto loaded = recovered->load_series(value.id, "AAPL");
  ASSERT_TRUE(loaded.has_value()) << (loaded ? "" : loaded.error().to_string());
  EXPECT_EQ(loaded->backtest.pnl_delta, original.backtest.pnl_delta);
  EXPECT_EQ(recovered->find_series(value.id, "AAPL")->partition_filename, original_filename);
  std::filesystem::remove_all(root);
}

TEST(BacktestDbSeries, VacuumRemovesOnlyUnindexedDatabasePartitions) {
  const std::filesystem::path root = test_root("vacuum");
  const std::filesystem::path partition_dir = root / kBacktestDbPartitionDir;
  auto db = BacktestDb::create(root.string());
  ASSERT_TRUE(db.has_value());
  const BacktestStrategyTemplate value = make_template("strangle");
  ASSERT_TRUE(db->register_template(value).has_value());
  const BacktestSeriesData original = make_series(101);
  ASSERT_TRUE(db->write_series(value.id, "AAPL", 101, original).has_value());
  auto original_info = db->find_series(value.id, "AAPL");
  ASSERT_TRUE(original_info.has_value());

  BacktestSeriesData revised = original;
  revised.backtest.pnl_delta[1] += 0.5;
  ASSERT_TRUE(db->write_series(*original_info, revised).has_value());
  auto current_info = db->find_series(value.id, "AAPL");
  ASSERT_TRUE(current_info.has_value());
  ASSERT_NE(current_info->partition_filename, original_info->partition_filename);

  std::string orphan_filename = current_info->partition_filename;
  ASSERT_GE(orphan_filename.size(), 71u);
  orphan_filename.replace(55, 16, "ffffffffffffffff");
  const std::filesystem::path orphan = partition_dir / orphan_filename;
  std::error_code ec;
  ASSERT_TRUE(std::filesystem::copy_file(partition_dir / current_info->partition_filename, orphan,
                                         std::filesystem::copy_options::overwrite_existing, ec))
      << ec.message();

  const std::filesystem::path unrelated = partition_dir / "operator-notes.txt";
  const std::filesystem::path unknown_archive = partition_dir / "unrecognized.atxrun";
  const std::filesystem::path writer_temp =
      partition_dir / (current_info->partition_filename + ".tmp");
  {
    std::ofstream file(unrelated);
    ASSERT_TRUE(file.is_open());
    file << "keep";
  }
  {
    std::ofstream file(unknown_archive);
    ASSERT_TRUE(file.is_open());
    file << "keep";
  }
  {
    std::ofstream file(writer_temp);
    ASSERT_TRUE(file.is_open());
    file << "keep";
  }

  auto removed = db->vacuum_unindexed_partitions();
  ASSERT_TRUE(removed.has_value()) << (removed ? "" : removed.error().to_string());
  EXPECT_EQ(*removed, 2u);
  EXPECT_FALSE(std::filesystem::exists(partition_dir / original_info->partition_filename));
  EXPECT_FALSE(std::filesystem::exists(orphan));
  EXPECT_TRUE(std::filesystem::exists(partition_dir / current_info->partition_filename));
  EXPECT_TRUE(std::filesystem::exists(unrelated));
  EXPECT_TRUE(std::filesystem::exists(unknown_archive));
  EXPECT_TRUE(std::filesystem::exists(writer_temp));
  EXPECT_TRUE(db->load_series(value.id, "AAPL").has_value());

  auto no_more = db->vacuum_unindexed_partitions();
  ASSERT_TRUE(no_more.has_value());
  EXPECT_EQ(*no_more, 0u);
  std::filesystem::remove_all(root);
}

TEST(BacktestDbSeries, MappedViewOwnsArchiveLifetime) {
  const std::filesystem::path root = test_root("mapped");
  MappedBacktestView mapped;
  {
    auto db = BacktestDb::create(root.string());
    ASSERT_TRUE(db.has_value());
    const BacktestStrategyTemplate value = make_template("strangle");
    ASSERT_TRUE(db->register_template(value).has_value());
    ASSERT_TRUE(db->write_series(value.id, "AAPL", 101, make_series(101)).has_value());
    auto opened = db->map_backtest(value.id, "AAPL");
    ASSERT_TRUE(opened.has_value());
    mapped = std::move(*opened);
  }
  EXPECT_EQ(mapped->n_rows(), 3u);
  const auto nav = mapped->f64_col("nav");
  ASSERT_EQ(nav.size(), 3u);
  EXPECT_DOUBLE_EQ(nav.back(), 1.0);
  mapped = {};
  std::filesystem::remove_all(root);
}

TEST(BacktestDbSeries, CorruptedPartitionRejected) {
  const std::filesystem::path root = test_root("corrupt");
  auto db = BacktestDb::create(root.string());
  ASSERT_TRUE(db.has_value());
  const BacktestStrategyTemplate value = make_template("strangle");
  ASSERT_TRUE(db->register_template(value).has_value());
  ASSERT_TRUE(db->write_series(value.id, "AAPL", 101, make_series(101)).has_value());
  auto info = db->find_series(value.id, "AAPL");
  ASSERT_TRUE(info.has_value());
  const std::filesystem::path partition = root / kBacktestDbPartitionDir / info->partition_filename;
  {
    std::fstream file(partition, std::ios::in | std::ios::out | std::ios::binary);
    ASSERT_TRUE(file.is_open());
    file.seekg(-1, std::ios::end);
    char byte = 0;
    file.read(&byte, 1);
    byte ^= static_cast<char>(0x5a);
    file.seekp(-1, std::ios::end);
    file.write(&byte, 1);
  }
  const auto loaded = db->load_series(value.id, "AAPL");
  ASSERT_FALSE(loaded.has_value());
  EXPECT_EQ(loaded.error().code(), ErrorCode::ParseError);
  std::filesystem::remove_all(root);
}

TEST(BacktestDbManifest, RefreshObservesAdvancedGeneration) {
  const std::filesystem::path root = test_root("refresh");
  auto reader = BacktestDb::create(root.string());
  ASSERT_TRUE(reader.has_value());
  auto writer = BacktestDb::open(root.string());
  ASSERT_TRUE(writer.has_value());
  ASSERT_TRUE(writer->register_template(make_template("new-template")).has_value());
  EXPECT_EQ(reader->generation(), 1u);
  ASSERT_TRUE(reader->refresh().has_value());
  EXPECT_EQ(reader->generation(), writer->generation());
  EXPECT_TRUE(reader->find_template("new-template").has_value());
  std::filesystem::remove_all(root);
}

TEST(BacktestDbManifest, TemplateAndSeriesListingsAreDeterministic) {
  const std::filesystem::path root = test_root("ordering");
  auto db = BacktestDb::create(root.string());
  ASSERT_TRUE(db.has_value());
  ASSERT_TRUE(db->register_template(make_template("zeta")).has_value());
  ASSERT_TRUE(db->register_template(make_template("alpha")).has_value());
  const auto templates = db->templates();
  ASSERT_EQ(templates.size(), 2u);
  EXPECT_EQ(templates[0].id, "alpha");
  EXPECT_EQ(templates[1].id, "zeta");
  ASSERT_TRUE(db->write_series("zeta", "MSFT", 202, make_series(202)).has_value());
  ASSERT_TRUE(db->write_series("alpha", "AAPL", 101, make_series(101)).has_value());
  ASSERT_TRUE(db->write_series("alpha", "MSFT", 202, make_series(202)).has_value());
  const auto series = db->series();
  ASSERT_EQ(series.size(), 3u);
  EXPECT_EQ(std::pair(series[0].template_id, series[0].symbol),
            std::pair(std::string("alpha"), std::string("AAPL")));
  EXPECT_EQ(std::pair(series[1].template_id, series[1].symbol),
            std::pair(std::string("alpha"), std::string("MSFT")));
  EXPECT_EQ(std::pair(series[2].template_id, series[2].symbol),
            std::pair(std::string("zeta"), std::string("MSFT")));
  std::filesystem::remove_all(root);
}

} // namespace
} // namespace atx::vol
