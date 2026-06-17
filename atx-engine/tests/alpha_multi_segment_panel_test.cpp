#include <filesystem>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "atx/engine/alpha/panel.hpp"
#include "atx/engine/alpha/segment_panel.hpp"
#include "atx/tsdb/load_parquet.hpp"

namespace {
namespace fs = std::filesystem;
using namespace atx::engine::alpha;

atx::i64 day_nanos(atx::i64 day_index) { return day_index * 86400LL * 1000000000LL; }

// Write a 1-date segment with the given symbols + a single "close" field.
void write_seg(const fs::path &path, atx::i64 dn, std::vector<std::string> syms,
               std::vector<atx::f64> close) {
  atx::tsdb::LongColumns cols;
  cols.field_names = {"close"};
  cols.times.assign(syms.size(), dn);
  cols.symbols = std::move(syms);
  cols.values = {std::move(close)};
  ASSERT_TRUE(atx::tsdb::build_from_long(cols, path.string(), 0).has_value());
}
} // namespace

TEST(AlphaMultiSegmentPanel, UnionsInstrumentAxesAcrossDates) {
  const fs::path dir = fs::temp_directory_path() / "atx_multi_seg";
  fs::remove_all(dir);
  fs::create_directories(dir);
  // day 0: {A, B}; day 1: {B, C}; day 2: {A}.  Union (first-seen) = [A, B, C].
  write_seg(dir / "2020-01-01.seg", day_nanos(18262), {"A", "B"}, {10.0, 20.0});
  write_seg(dir / "2020-01-02.seg", day_nanos(18263), {"B", "C"}, {21.0, 30.0});
  write_seg(dir / "2020-01-03.seg", day_nanos(18264), {"A"}, {11.0});

  auto panel = attach_multi_segment_panel(dir.string());
  ASSERT_TRUE(panel.has_value()) << panel.error().to_string();
  EXPECT_EQ(panel->dates(), 3u);
  EXPECT_EQ(panel->instruments(), 3u);          // A, B, C
  auto fid = panel->field_id("close");
  ASSERT_TRUE(fid.has_value());
  const auto col = panel->field_all(*fid);       // date-major, 3*3
  // (date0): A=10, B=20, C=NaN
  EXPECT_DOUBLE_EQ(col[0 * 3 + 0], 10.0);
  EXPECT_DOUBLE_EQ(col[0 * 3 + 1], 20.0);
  EXPECT_TRUE(std::isnan(col[0 * 3 + 2]));
  // (date1): A=NaN, B=21, C=30
  EXPECT_TRUE(std::isnan(col[1 * 3 + 0]));
  EXPECT_DOUBLE_EQ(col[1 * 3 + 1], 21.0);
  EXPECT_DOUBLE_EQ(col[1 * 3 + 2], 30.0);
  // (date2): A=11, B=NaN, C=NaN
  EXPECT_DOUBLE_EQ(col[2 * 3 + 0], 11.0);
  // universe (present bitmap): A in date0, not date1
  EXPECT_TRUE(panel->in_universe(0, 0));
  EXPECT_FALSE(panel->in_universe(1, 0));
}

TEST(AlphaMultiSegmentPanel, WindowExcludesOutOfRangeDates) {
  const fs::path dir = fs::temp_directory_path() / "atx_multi_seg_win";
  fs::remove_all(dir);
  fs::create_directories(dir);
  write_seg(dir / "2020-01-01.seg", day_nanos(18262), {"A"}, {10.0});
  write_seg(dir / "2020-01-02.seg", day_nanos(18263), {"A"}, {11.0});
  write_seg(dir / "2020-01-03.seg", day_nanos(18264), {"A"}, {12.0});

  TimeWindow w;
  w.start_nanos = day_nanos(18263);   // include day1..
  w.end_nanos = day_nanos(18264);     // ..exclusive of day2
  auto panel = attach_multi_segment_panel(dir.string(), w);
  ASSERT_TRUE(panel.has_value()) << panel.error().to_string();
  EXPECT_EQ(panel->dates(), 1u);
  auto fid = panel->field_id("close");
  ASSERT_TRUE(fid.has_value());
  EXPECT_DOUBLE_EQ(panel->field_all(*fid)[0], 11.0);
}
