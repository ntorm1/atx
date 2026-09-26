#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

#include "atx/engine/alpha/segment_panel.hpp"
#include "atx/engine/data/history_panel.hpp"
#include "atx/engine/data/orats_history.hpp"   // kOratsFields
#include "atx/tsdb/builder.hpp"
#include "atx/tsdb/load_parquet.hpp"

namespace {
namespace fs = std::filesystem;
namespace alpha = atx::engine::alpha;
using namespace atx::engine::data;

atx::i64 day_nanos(atx::i64 d) { return d * 86400LL * 1000000000LL; }

// Write one date's segment with all 16 ORATS fields for the given securities.
// atmiv21 values are supplied for kOratsFields[13] to prove real data flows through.
void write_day(const fs::path &dir, const char *name, atx::i64 dn,
               std::vector<std::string> syms,
               std::vector<atx::f64> close, std::vector<atx::f64> cumret,
               std::vector<atx::f64> shares,
               std::vector<atx::f64> atmiv21 = {}) {
  atx::tsdb::LongColumns cols;
  cols.field_names.assign(kOratsFields.begin(), kOratsFields.end());
  const size_t r = syms.size();
  cols.times.assign(r, dn);
  cols.symbols = syms;
  cols.values.assign(kOratsFields.size(), std::vector<atx::f64>(r, 0.0));
  // indices into kOratsFields: 3=close, 7=shares, 10=cumReturnFactor, 6=volume
  // 12=earnFlag, 13=atmCenI_21d, 14=atmCenI_126d, 15=nEarnCnt_5d
  cols.values[3] = close;
  cols.values[6] = std::vector<atx::f64>(r, 1e7); // volume
  cols.values[7] = shares;
  cols.values[10] = cumret;
  if (!atmiv21.empty()) {
    cols.values[13] = atmiv21;
  }
  ASSERT_TRUE(atx::tsdb::build_from_long(cols, (dir / name).string(), 0).has_value());
}
} // namespace

class DataHistoryPanelFixedUnion : public ::testing::Test {
protected:
  fs::path root;
  void SetUp() override {
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    for (int i = 0; i < 32; ++i) {
      const auto candidate = fs::temp_directory_path() / ("atx-fixed-history-" + std::to_string(stamp) + "-" + std::to_string(i));
      std::error_code ec;
      if (fs::create_directory(candidate, ec)) { root = candidate; return; }
    }
    FAIL() << "cannot exclusively reserve synthetic history fixture";
  }
  void TearDown() override { if (!root.empty()) { std::error_code ec; fs::remove_all(root, ec); } }
};

TEST_F(DataHistoryPanelFixedUnion, KeepsAbsentAndFutureColumnsWithOriginalF64History) {
  const auto start = day_nanos(15707); // synthetic 2013-01-02
  ASSERT_NO_FATAL_FAILURE(write_day(root, "a.seg", start, {"20", "10"}, {20, 10.00000002}, {1, 1}, {1e8, 1e8}));
  ASSERT_NO_FATAL_FAILURE(write_day(root, "b.seg", start + day_nanos(1), {"10"}, {11.00000002}, {1}, {1e8}));
  EXPECT_FALSE(atx::tsdb::SegmentReader::attach((root / "a.seg").string(), 1));
  EXPECT_TRUE(atx::tsdb::SegmentReader::attach((root / "a.seg").string(), fs::file_size(root / "a.seg")));
  HistoryDataConfig cfg; cfg.seg_dir = root.string(); cfg.window = {start, start + day_nanos(2)};
  cfg.fixed_axis_ids = {10, 20, 30};
  auto fixed = build_history_panel(cfg); ASSERT_TRUE(fixed) << fixed.error().message();
  EXPECT_EQ(fixed->instrument_ids, (std::vector<std::string>{"10", "20", "30"}));
  EXPECT_EQ(fixed->original_instrument_indices, (std::vector<atx::usize>{0, 1, 2}));
  EXPECT_TRUE(fixed->panel.in_universe(0, 0)); EXPECT_FALSE(fixed->panel.in_universe(1, 1));
  EXPECT_FALSE(fixed->panel.in_universe(0, 2)); EXPECT_FALSE(fixed->panel.in_universe(1, 2));
  auto close = fixed->panel.field_id("close"); ASSERT_TRUE(close);
  EXPECT_EQ(std::bit_cast<atx::u64>(fixed->panel.field_cross_section(*close, 1)[0]), std::bit_cast<atx::u64>(11.00000002));
  EXPECT_TRUE(std::isnan(fixed->panel.field_cross_section(*close, 1)[1]));
  cfg.max_working_bytes = 1; EXPECT_FALSE(build_history_panel(cfg));
  cfg.max_working_bytes = 2ULL * 1024 * 1024 * 1024; cfg.compact_to_universe = true;
  EXPECT_FALSE(build_history_panel(cfg));
}

TEST_F(DataHistoryPanelFixedUnion, CapturedIndexRejectsReplacedAxisAndContentBeforeCopy) {
  const auto start = day_nanos(15707);
  ASSERT_NO_FATAL_FAILURE(write_day(root, "a.seg", start, {"10"}, {10}, {1}, {1e8}));
  const auto index = capture_history_sources(root.string(), {start, start + day_nanos(3)});
  ASSERT_TRUE(index) << index.error().message();
  HistoryDataConfig cfg; cfg.seg_dir = root.string(); cfg.window = {start, start + day_nanos(3)};
  cfg.fixed_axis_ids = {10, 20}; cfg.source_index = *index;
  const auto extent = fs::file_size(root / "a.seg");
  // A valid, equal-sized replacement now has an in-window date beyond the
  // captured axis. It used to lower_bound to end and index outside the buffer.
  ASSERT_NO_FATAL_FAILURE(write_day(root, "a.seg", start + day_nanos(1), {"10"}, {10}, {1}, {1e8}));
  EXPECT_EQ(fs::file_size(root / "a.seg"), extent);
  const auto changed_axis = build_history_panel(cfg);
  ASSERT_FALSE(changed_axis);
  EXPECT_NE(changed_axis.error().message().find("time axis changed"), std::string::npos);
  EXPECT_FALSE((*index)->verify_paths());
  // Restoring the original axis does not authorize changed source prices.
  ASSERT_NO_FATAL_FAILURE(write_day(root, "a.seg", start, {"10"}, {99}, {1}, {1e8}));
  const auto changed_content = build_history_panel(cfg);
  ASSERT_FALSE(changed_content);
  EXPECT_NE(changed_content.error().message().find("content changed"), std::string::npos);
}

TEST_F(DataHistoryPanelFixedUnion, CachedChunksSkipDisjointPayloadAndFinalMetadataDetectsNewRelevance) {
  const auto start = day_nanos(15707);
  ASSERT_NO_FATAL_FAILURE(write_day(root, "a.seg", start, {"10"}, {10}, {1}, {1e8}));
  ASSERT_NO_FATAL_FAILURE(write_day(root, "b.seg", start + day_nanos(10), {"10"}, {11}, {1}, {1e8}));
  const auto index = capture_history_sources(root.string(), {start, start + day_nanos(11)});
  ASSERT_TRUE(index) << index.error().message();
  // Corrupt only the later source's seal. A first-date chunk must not attach it.
  {
    std::fstream file(root / "b.seg", std::ios::binary | std::ios::in | std::ios::out);
    ASSERT_TRUE(file);
    file.seekp(-static_cast<std::streamoff>(sizeof(atx::tsdb::SegmentFooter)), std::ios::end);
    file.put('\0');
    ASSERT_TRUE(file);
  }
  HistoryDataConfig cfg; cfg.seg_dir = root.string(); cfg.window = {start, start + day_nanos(1)};
  cfg.fixed_axis_ids = {10}; cfg.source_index = *index;
  const auto first = build_history_panel(cfg);
  ASSERT_TRUE(first) << first.error().message();
  EXPECT_EQ(first->source_segment_paths, (std::vector<std::string>{(root / "a.seg").string()}));
  cfg.window = {start + day_nanos(10), start + day_nanos(11)};
  EXPECT_FALSE(build_history_panel(cfg)); // actually consuming that file validates its CRC/seal
  // A fresh narrow capture reads only disjoint metadata, so the damaged payload
  // does not prevent unrelated assembly. Publication still notices a moved axis.
  const auto narrow = capture_history_sources(root.string(), {start, start + day_nanos(1)});
  ASSERT_TRUE(narrow) << narrow.error().message();
  ASSERT_EQ((*narrow)->files().size(), 2U);
  EXPECT_TRUE((*narrow)->files()[1].sha256.empty());
  ASSERT_NO_FATAL_FAILURE(write_day(root, "b.seg", start, {"20"}, {11}, {1}, {1e8}));
  EXPECT_FALSE((*narrow)->verify_paths());
  ASSERT_NO_FATAL_FAILURE(write_day(root, "b.seg", start + day_nanos(10), {"10"}, {11}, {1}, {1e8}));
  ASSERT_TRUE((*narrow)->verify_paths());
  ASSERT_NO_FATAL_FAILURE(write_day(root, "c.seg", start, {"20"}, {12}, {1}, {1e8}));
  EXPECT_FALSE((*narrow)->verify_paths());
}

TEST_F(DataHistoryPanelFixedUnion, NamedFutureOrMixedSourceRefusedBeforeAnyAttachment) {
  const auto start = day_nanos(15707);
  // Neither entry is a segment: name preflight must win before even parsing the
  // lexically earlier malformed file, proving no CRC/mapping occurred first.
  { std::ofstream file(root / "a.seg", std::ios::binary); file << "not a segment"; }
  const auto bad = root / "prices-2012-2024.seg";
  { std::ofstream file(bad, std::ios::binary); file << "not a segment"; }
  const auto result = capture_history_sources(root.string(), {start, start + day_nanos(1)});
  ASSERT_FALSE(result);
  EXPECT_NE(result.error().message().find("before mapping"), std::string::npos);
}

TEST_F(DataHistoryPanelFixedUnion, UniverseSizeAndBuildDateNamesDoNotAssertPayloadEra) {
  const auto start = day_nanos(15707);
  const auto source = root / "t3000" / "build-20260925";
  ASSERT_TRUE(fs::create_directories(source));
  ASSERT_NO_FATAL_FAILURE(write_day(source, "a.seg", start, {"10"}, {10}, {1}, {1e8}));
  const auto result = capture_history_sources(source.string(), {start, start + day_nanos(1)});
  ASSERT_TRUE(result) << result.error().message();
  EXPECT_EQ((*result)->sessions(), (std::vector<atx::i64>{start}));
  { std::ofstream file(source / "2020-01-02.seg", std::ios::binary); file << "never attach"; }
  const auto forbidden = capture_history_sources(source.string(), {start, start + day_nanos(1)});
  ASSERT_FALSE(forbidden);
  EXPECT_NE(forbidden.error().message().find("before mapping"), std::string::npos);
}

TEST(DataHistoryPanel, DeterministicDigestAndCanonicalFields) {
  const fs::path dir = fs::temp_directory_path() / "atx_hist_panel";
  fs::remove_all(dir);
  fs::create_directories(dir);
  write_day(dir, "2020-01-02.seg", day_nanos(18263), {"33449", "33008"},
            {300.0, 20.0}, {1.0, 0.5}, {4e9, 1e9}, {0.05, 0.07});
  write_day(dir, "2020-01-03.seg", day_nanos(18264), {"33449", "33008"},
            {303.0, 21.0}, {1.0, 0.5}, {4e9, 1e9}, {0.06, 0.08});

  HistoryDataConfig cfg;
  cfg.seg_dir = dir.string();
  cfg.universe.min_adv_usd = 0.0;     // keep both names in the smoke
  cfg.universe.adv_window = 1;

  auto a = build_history_panel(cfg);
  ASSERT_TRUE(a.has_value()) << a.error().to_string();
  auto b = build_history_panel(cfg);
  ASSERT_TRUE(b.has_value()) << b.error().to_string();
  EXPECT_EQ(a->digest, b->digest);                       // byte-reproducible

  const alpha::Panel &p = a->panel;
  EXPECT_EQ(p.dates(), 2u);
  EXPECT_EQ(p.instruments(), 2u);
  auto close = p.field_id(kHistFieldClose);
  auto raw = p.field_id(kHistFieldRawClose);
  ASSERT_TRUE(close.has_value()); ASSERT_TRUE(raw.has_value());
  // close = TRI = raw*cumret; for 33008 (idx1) date0: 20 * 0.5 = 10.
  EXPECT_DOUBLE_EQ(p.field_all(*raw)[0 * 2 + 1], 20.0);
  EXPECT_DOUBLE_EQ(p.field_all(*close)[0 * 2 + 1], 10.0);
  // market_cap present and = shares*raw_close (split-invariant): 1e9*20 = 2e10.
  auto mcap = p.field_id(kHistFieldMarketCap);
  ASSERT_TRUE(mcap.has_value());
  EXPECT_DOUBLE_EQ(p.field_all(*mcap)[0 * 2 + 1], 2.0e10);
}

TEST(DataHistoryPanel, OptionsEarningsFieldsPresentAndFinite) {
  // Verify that the 4 ORATS options/earnings fields (earnFlag, atmCenI_21d,
  // atmCenI_126d, nEarnCnt_5d) are carried into the assembled Panel as fields
  // 8..11, and that atmCenI_21d carries finite (non-NaN) values from the fixture.
  const fs::path dir = fs::temp_directory_path() / "atx_hist_panel_opts";
  fs::remove_all(dir);
  fs::create_directories(dir);
  // Write 2 days; atmCenI_21d (index 13) is non-zero to prove data flows through.
  write_day(dir, "2020-01-02.seg", day_nanos(18263), {"33449", "33008"},
            {300.0, 20.0}, {1.0, 0.5}, {4e9, 1e9}, {0.05, 0.07});
  write_day(dir, "2020-01-03.seg", day_nanos(18264), {"33449", "33008"},
            {303.0, 21.0}, {1.0, 0.5}, {4e9, 1e9}, {0.06, 0.08});

  HistoryDataConfig cfg;
  cfg.seg_dir = dir.string();
  cfg.universe.min_adv_usd = 0.0;
  cfg.universe.adv_window = 1;

  auto res = build_history_panel(cfg);
  ASSERT_TRUE(res.has_value()) << res.error().to_string();
  const alpha::Panel &p = res->panel;

  // All 4 new fields must be present by name.
  auto earnflag  = p.field_id(kHistFieldEarnFlag);
  auto atmiv21   = p.field_id(kHistFieldAtmIv21);
  auto atmiv126  = p.field_id(kHistFieldAtmIv126);
  auto earncnt5  = p.field_id(kHistFieldEarnCnt5);
  ASSERT_TRUE(earnflag.has_value())  << "earnFlag field missing from panel";
  ASSERT_TRUE(atmiv21.has_value())   << "atmCenI_21d field missing from panel";
  ASSERT_TRUE(atmiv126.has_value())  << "atmCenI_126d field missing from panel";
  ASSERT_TRUE(earncnt5.has_value())  << "nEarnCnt_5d field missing from panel";

  // atmCenI_21d must have at least one finite (non-NaN) value in-universe,
  // proving real data flowed through rather than an all-NaN column.
  const std::span<const atx::f64> iv21 = p.field_all(*atmiv21);
  bool found_finite = false;
  for (atx::f64 v : iv21) {
    if (std::isfinite(v)) { found_finite = true; break; }
  }
  EXPECT_TRUE(found_finite) << "atmCenI_21d has no finite value — data did not flow through";
}

namespace {

struct RawHistoryBar {
  atx::f64 open;
  atx::f64 high;
  atx::f64 low;
  atx::f64 close;
  atx::f64 factor;
  atx::f64 volume;
  atx::f64 shares;
};

void write_price_day(const fs::path &dir, const char *name, atx::i64 day,
                     const RawHistoryBar &bar) {
  atx::tsdb::LongColumns cols;
  cols.field_names.assign(kOratsFields.begin(), kOratsFields.end());
  cols.times = {day_nanos(day)};
  cols.symbols = {"7"};
  cols.values.assign(kOratsFields.size(), std::vector<atx::f64>(1, 0.0));
  cols.values[0][0] = bar.open;
  cols.values[1][0] = bar.high;
  cols.values[2][0] = bar.low;
  cols.values[3][0] = bar.close;
  cols.values[6][0] = bar.volume;
  cols.values[7][0] = bar.shares;
  cols.values[10][0] = bar.factor;
  ASSERT_TRUE(atx::tsdb::build_from_long(cols, (dir / name).string(), 0).has_value());
}

HistoryDataConfig price_config(const fs::path &dir) {
  HistoryDataConfig cfg;
  cfg.seg_dir = dir.string();
  cfg.universe.min_adv_usd = 0.0;
  cfg.universe.adv_window = 1;
  return cfg;
}

std::span<const atx::f64> field(const alpha::Panel &panel, std::string_view name) {
  return panel.field_all(panel.field_id(name).value());
}

} // namespace

TEST(DataHistoryPanel, CanonicalOhlcShareFactorWhileRawEconomicsRemain) {
  const fs::path dir = fs::temp_directory_path() / "atx_hist_panel_basis";
  fs::remove_all(dir);
  fs::create_directories(dir);
  write_price_day(dir, "2020-01-02.seg", 18263,
                  RawHistoryBar{96.0, 108.0, 92.0, 100.0, 0.25, 2000.0, 100.0});
  HistoryDataConfig cfg = price_config(dir);
  // All three screens pass on raw economics but would fail on adjusted prices.
  cfg.universe.min_price = 90.0;
  cfg.universe.min_mktcap_usd = 9000.0;
  cfg.universe.min_adv_usd = 150'000.0;
  const auto built = build_history_panel(cfg);
  ASSERT_TRUE(built.has_value()) << built.error().to_string();
  const alpha::Panel &panel = built->panel;
  EXPECT_EQ(panel.num_fields(), 12U);
  EXPECT_TRUE(panel.in_universe(0, 0));
  EXPECT_DOUBLE_EQ(field(panel, "open")[0], 24.0);
  EXPECT_DOUBLE_EQ(field(panel, "high")[0], 27.0);
  EXPECT_DOUBLE_EQ(field(panel, "low")[0], 23.0);
  EXPECT_DOUBLE_EQ(field(panel, "close")[0], 25.0);
  EXPECT_DOUBLE_EQ(field(panel, "raw_close")[0], 100.0);
  EXPECT_DOUBLE_EQ(field(panel, "volume")[0], 2000.0);
  EXPECT_DOUBLE_EQ(field(panel, "market_cap")[0], 10'000.0);

  const std::array<std::string, 6> source_fields{
      "open", "high", "low", "close", "volume", "shares"};
  const auto source = alpha::attach_multi_segment_panel(cfg.seg_dir, cfg.window, source_fields);
  ASSERT_TRUE(source.has_value()) << source.error().to_string();
  EXPECT_DOUBLE_EQ(field(*source, "open")[0], 96.0);
  EXPECT_DOUBLE_EQ(field(*source, "high")[0], 108.0);
  EXPECT_DOUBLE_EQ(field(*source, "low")[0], 92.0);
  EXPECT_DOUBLE_EQ(field(*source, "close")[0], 100.0);
  EXPECT_DOUBLE_EQ(field(*source, "volume")[0], 2000.0);
  EXPECT_DOUBLE_EQ(field(*source, "shares")[0], 100.0);
}

TEST(DataHistoryPanel, SplitContinuityCoversEntireResearchCandle) {
  const fs::path dir = fs::temp_directory_path() / "atx_hist_panel_split";
  fs::remove_all(dir);
  fs::create_directories(dir);
  // Pure four-for-one split with unchanged economic value and candle shape.
  write_price_day(dir, "2020-01-02.seg", 18263,
                  RawHistoryBar{390.0, 420.0, 380.0, 400.0, 0.25, 100.0, 1e6});
  write_price_day(dir, "2020-01-03.seg", 18264,
                  RawHistoryBar{97.5, 105.0, 95.0, 100.0, 1.0, 400.0, 4e6});
  const auto built = build_history_panel(price_config(dir));
  ASSERT_TRUE(built.has_value()) << built.error().to_string();
  const alpha::Panel &panel = built->panel;
  const std::array<std::string_view, 4> names{"open", "high", "low", "close"};
  for (const auto name : names) {
    const auto values = field(panel, name);
    EXPECT_DOUBLE_EQ(values[0], values[1]) << name;
  }
  EXPECT_DOUBLE_EQ(field(panel, "close")[1] / field(panel, "close")[0] - 1.0, 0.0);
  EXPECT_DOUBLE_EQ(field(panel, "volume")[0], 100.0);
  EXPECT_DOUBLE_EQ(field(panel, "volume")[1], 400.0);
  EXPECT_DOUBLE_EQ(field(panel, "market_cap")[0], 4e8);
  EXPECT_DOUBLE_EQ(field(panel, "market_cap")[1], 4e8);
}

TEST(DataHistoryPanel, DividendFactorPreservesCandleRatiosAndRawSharesVolume) {
  const fs::path dir = fs::temp_directory_path() / "atx_hist_panel_dividend";
  fs::remove_all(dir);
  fs::create_directories(dir);
  // One-dollar cash distribution: 125 becomes 124, with no total-return change.
  // The source factor changes by 125/124; no split of shares/volume has occurred.
  write_price_day(dir, "2020-01-02.seg", 18263,
                  RawHistoryBar{124.0, 128.0, 122.0, 125.0, 1.0, 7000.0, 2e6});
  write_price_day(dir, "2020-01-03.seg", 18264,
                  RawHistoryBar{123.0, 127.0, 121.0, 124.0, 125.0 / 124.0, 9000.0, 2e6});
  const auto built = build_history_panel(price_config(dir));
  ASSERT_TRUE(built.has_value()) << built.error().to_string();
  const alpha::Panel &panel = built->panel;
  const atx::f64 close = field(panel, "close")[1];
  EXPECT_NEAR(close, 125.0, 1e-12);
  EXPECT_NEAR(close / field(panel, "close")[0] - 1.0, 0.0, 1e-15);
  EXPECT_NEAR(field(panel, "open")[1] / close, 123.0 / 124.0, 1e-15);
  EXPECT_NEAR(field(panel, "high")[1] / close, 127.0 / 124.0, 1e-15);
  EXPECT_NEAR(field(panel, "low")[1] / close, 121.0 / 124.0, 1e-15);
  EXPECT_DOUBLE_EQ(field(panel, "raw_close")[1], 124.0);
  EXPECT_DOUBLE_EQ(field(panel, "volume")[0], 7000.0);
  EXPECT_DOUBLE_EQ(field(panel, "volume")[1], 9000.0);
  EXPECT_DOUBLE_EQ(field(panel, "market_cap")[0], 250e6);
  EXPECT_DOUBLE_EQ(field(panel, "market_cap")[1], 248e6);
}

TEST(DataHistoryPanel, InvalidFactorsAreGapsForEveryResearchPrice) {
  const fs::path dir = fs::temp_directory_path() / "atx_hist_panel_invalid_factor";
  fs::remove_all(dir);
  fs::create_directories(dir);
  const std::array<atx::f64, 5> factors{
      0.0, -1.0, std::numeric_limits<atx::f64>::infinity(),
      std::numeric_limits<atx::f64>::quiet_NaN(), 2.0};
  const std::array<const char *, 5> dates{
      "2020-01-02.seg", "2020-01-03.seg", "2020-01-04.seg", "2020-01-05.seg",
      "2020-01-06.seg"};
  for (atx::usize i = 0; i < factors.size(); ++i) {
    write_price_day(dir, dates[i], 18263 + static_cast<atx::i64>(i),
                    RawHistoryBar{99.0, 102.0, 98.0, 100.0, factors[i], 2000.0, 100.0});
  }
  const auto built = build_history_panel(price_config(dir));
  ASSERT_TRUE(built.has_value()) << built.error().to_string();
  const std::array<std::string_view, 4> names{"open", "high", "low", "close"};
  for (const auto name : names) {
    const auto prices = field(built->panel, name);
    for (atx::usize i = 0; i < 4; ++i) {
      EXPECT_TRUE(std::isnan(prices[i])) << name << " row=" << i;
    }
    EXPECT_TRUE(std::isfinite(prices[4]));
  }
  EXPECT_DOUBLE_EQ(field(built->panel, "close")[4], 200.0);
  EXPECT_DOUBLE_EQ(field(built->panel, "raw_close")[0], 100.0);
  EXPECT_DOUBLE_EQ(field(built->panel, "volume")[0], 2000.0);
  EXPECT_DOUBLE_EQ(field(built->panel, "market_cap")[0], 10'000.0);
}

TEST(DataHistoryPanel, InvalidPricesAndUnrepresentableProductsNeverBecomeValidPrices) {
  const atx::f64 nan = std::numeric_limits<atx::f64>::quiet_NaN();
  const atx::f64 inf = std::numeric_limits<atx::f64>::infinity();
  const std::array<atx::f64, 7> prices{
      -2.0, 0.0, nan, inf, std::numeric_limits<atx::f64>::max(),
      std::numeric_limits<atx::f64>::denorm_min(), 101.0};
  const std::array<atx::f64, 7> factors{-3.0, 1.0, 1.0, 1.0, 2.0, 0.5, 0.5};
  const auto adjusted = orats_total_return_close(prices, factors);
  ASSERT_EQ(adjusted.size(), prices.size());
  for (atx::usize i = 0; i < 6; ++i) {
    EXPECT_TRUE(std::isnan(adjusted[i])) << "row=" << i;
  }
  EXPECT_DOUBLE_EQ(adjusted.back(), 50.5);
  const auto mismatched = orats_total_return_close(
      prices, std::span<const atx::f64>{factors}.first(1));
  EXPECT_TRUE(mismatched.empty());
}

TEST(IndexedHistoryPanel, ActualTimesDetermineAxesAndSelectedSources) {
  const fs::path dir = fs::temp_directory_path() / "atx_hist_indexed_order";
  fs::remove_all(dir);
  fs::create_directories(dir);
  // Lexical filename order is the reverse of chronological order.
  write_day(dir, "a_later.seg", day_nanos(18264), {"333", "1001001001070"},
            {22.0, 12.0}, {1.0, 1.0}, {100.0, 100.0});
  write_day(dir, "z_earlier.seg", day_nanos(18263), {"1001001001070", "222"},
            {10.0, 20.0}, {1.0, 1.0}, {100.0, 100.0});
  write_day(dir, "outside.seg", day_nanos(18262), {"ignored"}, {1.0}, {1.0}, {1.0});
  const alpha::TimeWindow window{day_nanos(18263), day_nanos(18265)};
  const auto indexed = alpha::attach_indexed_multi_segment_panel(dir.string(), window);
  ASSERT_TRUE(indexed.has_value()) << indexed.error().to_string();
  EXPECT_EQ(indexed->session_keys, (std::vector<atx::i64>{day_nanos(18263), day_nanos(18264)}));
  EXPECT_EQ(indexed->instrument_ids, (std::vector<std::string>{"1001001001070", "222", "333"}));
  EXPECT_EQ(indexed->source_segment_paths,
            (std::vector<std::string>{(dir / "a_later.seg").string(), (dir / "z_earlier.seg").string()}));
  const auto close = field(indexed->panel, "close");
  EXPECT_DOUBLE_EQ(close[0], 10.0);
  EXPECT_DOUBLE_EQ(close[1], 20.0);
  EXPECT_TRUE(std::isnan(close[2]));
  EXPECT_DOUBLE_EQ(close[3], 12.0);
  EXPECT_TRUE(std::isnan(close[4]));
  EXPECT_DOUBLE_EQ(close[5], 22.0);

  const auto legacy = alpha::attach_multi_segment_panel(dir.string(), window);
  ASSERT_TRUE(legacy.has_value()) << legacy.error().to_string();
  const auto legacy_close = field(*legacy, "close");
  for (atx::usize i = 0; i < close.size(); ++i) {
    EXPECT_EQ(std::bit_cast<atx::u64>(close[i]), std::bit_cast<atx::u64>(legacy_close[i]));
  }
}

TEST(IndexedHistoryPanel, DisjointSameTimeCellsMergeWithoutPaddingOverwrite) {
  const fs::path dir = fs::temp_directory_path() / "atx_hist_indexed_padding";
  fs::remove_all(dir);
  fs::create_directories(dir);
  atx::tsdb::SegmentBuilder first({"close"}, {"A", "B"}, {10, 20});
  first.set(0, 0, 0, 10.0); // A on first date
  first.set(0, 1, 1, 21.0); // B on second date
  ASSERT_TRUE(first.write((dir / "a.seg").string(), 0).has_value());
  atx::tsdb::SegmentBuilder second({"close"}, {"B", "A"}, {10, 20});
  second.set(0, 0, 0, 20.0); // B on first date; A is absent padding
  second.set(0, 1, 1, 11.0); // A on second date; B is absent padding
  ASSERT_TRUE(second.write((dir / "b.seg").string(), 0).has_value());
  const auto indexed = alpha::attach_indexed_multi_segment_panel(dir.string());
  ASSERT_TRUE(indexed.has_value()) << indexed.error().to_string();
  EXPECT_EQ(indexed->session_keys, (std::vector<atx::i64>{10, 20}));
  EXPECT_EQ(indexed->instrument_ids, (std::vector<std::string>{"A", "B"}));
  const auto close = field(indexed->panel, "close");
  const std::array<atx::f64, 4> expected{10.0, 20.0, 11.0, 21.0};
  for (atx::usize k = 0; k < expected.size(); ++k) {
    EXPECT_DOUBLE_EQ(close[k], expected[k]);
    EXPECT_TRUE(indexed->panel.in_universe(k / 2, k % 2));
  }
}

TEST(IndexedHistoryPanel, DuplicatePresentCellsRejectEvenWhenValuesMatchAndUniverseIsZero) {
  const fs::path dir = fs::temp_directory_path() / "atx_hist_indexed_duplicate";
  fs::remove_all(dir);
  fs::create_directories(dir);
  atx::tsdb::SegmentBuilder source({"close", "universe"}, {"A"}, {10});
  source.set(0, 0, 0, 100.0);
  source.set(1, 0, 0, 0.0); // source observation exists despite exclusion from universe
  ASSERT_TRUE(source.write((dir / "a.seg").string(), 0).has_value());
  ASSERT_TRUE(source.write((dir / "b.seg").string(), 0).has_value());
  const alpha::UniversePolicy policy{alpha::UniverseKind::Field, "universe"};
  const auto indexed = alpha::attach_indexed_multi_segment_panel(dir.string(), {}, {}, policy);
  ASSERT_FALSE(indexed.has_value());
  EXPECT_EQ(indexed.error().code(), atx::core::ErrorCode::InvalidArgument);
  EXPECT_NE(indexed.error().to_string().find("duplicate cell"), std::string::npos);
  const auto legacy = alpha::attach_multi_segment_panel(dir.string(), {}, {}, policy);
  EXPECT_FALSE(legacy.has_value());
}

TEST(IndexedHistoryPanel, InvalidSourceTimeAxesAreRejectedBeforeSorting) {
  const fs::path dir = fs::temp_directory_path() / "atx_hist_indexed_bad_time";
  fs::remove_all(dir);
  fs::create_directories(dir);
  for (const auto &times : {std::vector<atx::i64>{20, 10}, std::vector<atx::i64>{10, 10}}) {
    atx::tsdb::SegmentBuilder source({"close"}, {"A"}, times);
    source.set(0, 0, 0, 100.0);
    source.set(0, 1, 0, 101.0);
    ASSERT_TRUE(source.write((dir / "invalid.seg").string(), 0).has_value());
    const auto indexed = alpha::attach_indexed_multi_segment_panel(dir.string());
    ASSERT_FALSE(indexed.has_value());
    EXPECT_EQ(indexed.error().code(), atx::core::ErrorCode::InvalidArgument);
  }
}

TEST(DataHistoryPanel, IdentityAndNonPrefixCompactionPreserveLargeSecurityIds) {
  const fs::path dir = fs::temp_directory_path() / "atx_hist_compact_identity";
  fs::remove_all(dir);
  fs::create_directories(dir);
  write_day(dir, "2020-01-02.seg", day_nanos(18263), {"12", "1001001001070", "33"},
            {0.5, 100.0, 0.25}, {1.0, 0.5, 1.0}, {100.0, 100.0, 100.0});
  write_day(dir, "2020-01-03.seg", day_nanos(18264), {"33", "1001001001070", "44"},
            {0.25, 101.0, 200.0}, {1.0, 0.5, 1.0}, {100.0, 100.0, 100.0});
  auto cfg = price_config(dir);
  cfg.universe.min_price = 1.0;
  const auto full = build_history_panel(cfg);
  ASSERT_TRUE(full.has_value()) << full.error().to_string();
  EXPECT_EQ(full->instrument_ids, (std::vector<std::string>{"12", "1001001001070", "33", "44"}));
  EXPECT_EQ(full->original_instrument_indices, (std::vector<atx::usize>{0, 1, 2, 3}));

  cfg.compact_to_universe = true;
  const auto compact = build_history_panel(cfg);
  ASSERT_TRUE(compact.has_value()) << compact.error().to_string();
  EXPECT_EQ(compact->session_keys, (std::vector<atx::i64>{day_nanos(18263), day_nanos(18264)}));
  EXPECT_EQ(compact->instrument_ids, (std::vector<std::string>{"1001001001070", "44"}));
  EXPECT_EQ(compact->original_instrument_indices, (std::vector<atx::usize>{1, 3}));
  EXPECT_EQ(compact->source_segment_paths, full->source_segment_paths);
  ASSERT_EQ(compact->panel.instruments(), 2u);
  // Every field and mask cell follows precisely the retained source column.
  for (atx::usize f = 0; f < compact->panel.num_fields(); ++f) {
    const auto small = compact->panel.field_all(static_cast<alpha::FieldId>(f));
    const auto large = full->panel.field_all(static_cast<alpha::FieldId>(f));
    for (atx::usize d = 0; d < 2; ++d) {
      for (atx::usize i = 0; i < 2; ++i) {
        const auto original = compact->original_instrument_indices[i];
        EXPECT_EQ(std::bit_cast<atx::u64>(small[d * 2 + i]),
                  std::bit_cast<atx::u64>(large[d * 4 + original]));
        EXPECT_EQ(compact->panel.in_universe(d, i), full->panel.in_universe(d, original));
      }
    }
  }
  EXPECT_DOUBLE_EQ(field(compact->panel, "close")[0], 50.0);
  EXPECT_TRUE(std::isnan(field(compact->panel, "close")[1]));
  EXPECT_DOUBLE_EQ(field(compact->panel, "close")[3], 200.0);
}

TEST(DataHistoryPanel, NoncanonicalOrUnrepresentableSecurityIdsFailAtHistoryBoundary) {
  const fs::path dir = fs::temp_directory_path() / "atx_hist_invalid_identity";
  fs::remove_all(dir);
  fs::create_directories(dir);
  for (const char *id : {"0", "-1", "0012", "12x", "9223372036854775808"}) {
    write_day(dir, "2020-01-02.seg", day_nanos(18263), {id}, {100.0}, {1.0}, {100.0});
    const auto built = build_history_panel(price_config(dir));
    ASSERT_FALSE(built.has_value()) << id;
    EXPECT_EQ(built.error().code(), atx::core::ErrorCode::InvalidArgument);
  }
}

TEST(DataHistoryPanel, MembershipAllowListRestrictsColumnsAndEmptyListIsOff) {
  const fs::path dir = fs::temp_directory_path() / "atx_hist_allow_ids";
  fs::remove_all(dir);
  fs::create_directories(dir);
  write_day(dir, "2020-01-02.seg", day_nanos(18263), {"12", "34", "56"},
            {10.0, 20.0, 30.0}, {1.0, 1.0, 1.0}, {100.0, 100.0, 100.0});
  write_day(dir, "2020-01-03.seg", day_nanos(18264), {"12", "34", "56"},
            {11.0, 21.0, 31.0}, {1.0, 1.0, 1.0}, {100.0, 100.0, 100.0});
  auto cfg = price_config(dir);
  cfg.compact_to_universe = true;

  // Baseline: the screen alone keeps all three columns.
  const auto baseline = build_history_panel(cfg);
  ASSERT_TRUE(baseline.has_value()) << baseline.error().to_string();
  EXPECT_EQ(baseline->instrument_ids, (std::vector<std::string>{"12", "34", "56"}));
  EXPECT_EQ(baseline->allow_list_excluded_columns, atx::usize{0});

  // An explicitly EMPTY allow-list is off: same kept count, same digest.
  cfg.allow_ids = {};
  const auto unrestricted = build_history_panel(cfg);
  ASSERT_TRUE(unrestricted.has_value()) << unrestricted.error().to_string();
  EXPECT_EQ(unrestricted->digest, baseline->digest);
  EXPECT_EQ(unrestricted->instrument_ids, baseline->instrument_ids);
  EXPECT_EQ(unrestricted->allow_list_excluded_columns, atx::usize{0});

  // Restricting to {12, 56} drops "34", which the screen alone would have kept.
  // The input is deliberately unsorted and duplicated: order must not matter.
  cfg.allow_ids = {56, 12, 12};
  const auto restricted = build_history_panel(cfg);
  ASSERT_TRUE(restricted.has_value()) << restricted.error().to_string();
  EXPECT_EQ(restricted->allow_list_excluded_columns, atx::usize{1});
  EXPECT_EQ(restricted->instrument_ids, (std::vector<std::string>{"12", "56"}));
  EXPECT_EQ(restricted->original_instrument_indices, (std::vector<atx::usize>{0, 2}));
  ASSERT_EQ(restricted->panel.instruments(), 2u);
  for (atx::usize d = 0; d < 2; ++d) {
    EXPECT_TRUE(restricted->panel.in_universe(d, 0));
    EXPECT_TRUE(restricted->panel.in_universe(d, 1));
  }
  // Retained columns carry precisely their source cells.
  EXPECT_DOUBLE_EQ(field(restricted->panel, "close")[0], 10.0);
  EXPECT_DOUBLE_EQ(field(restricted->panel, "close")[1], 30.0);

  // Without compaction the restriction still removes membership on every date.
  cfg.compact_to_universe = false;
  const auto masked = build_history_panel(cfg);
  ASSERT_TRUE(masked.has_value()) << masked.error().to_string();
  ASSERT_EQ(masked->panel.instruments(), 3u);
  for (atx::usize d = 0; d < 2; ++d) {
    EXPECT_TRUE(masked->panel.in_universe(d, 0));
    EXPECT_FALSE(masked->panel.in_universe(d, 1));
    EXPECT_TRUE(masked->panel.in_universe(d, 2));
  }

  // An allow-list matching no column fails loudly instead of emptying the panel.
  cfg.allow_ids = {999};
  const auto none = build_history_panel(cfg);
  ASSERT_FALSE(none.has_value());
  EXPECT_EQ(none.error().code(), atx::core::ErrorCode::InvalidArgument);
}
