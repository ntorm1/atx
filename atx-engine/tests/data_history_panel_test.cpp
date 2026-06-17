#include <filesystem>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "atx/engine/data/history_panel.hpp"
#include "atx/engine/data/orats_history.hpp"   // kOratsFields
#include "atx/tsdb/load_parquet.hpp"

namespace {
namespace fs = std::filesystem;
namespace alpha = atx::engine::alpha;
using namespace atx::engine::data;

atx::i64 day_nanos(atx::i64 d) { return d * 86400LL * 1000000000LL; }

// Write one date's segment with all 16 ORATS fields for the given securities.
void write_day(const fs::path &dir, const char *name, atx::i64 dn,
               std::vector<std::string> syms,
               std::vector<atx::f64> close, std::vector<atx::f64> cumret,
               std::vector<atx::f64> shares) {
  atx::tsdb::LongColumns cols;
  cols.field_names.assign(kOratsFields.begin(), kOratsFields.end());
  const size_t r = syms.size();
  cols.times.assign(r, dn);
  cols.symbols = syms;
  cols.values.assign(kOratsFields.size(), std::vector<atx::f64>(r, 0.0));
  // indices into kOratsFields: 3=close, 7=shares, 10=cumulReturnFactor, 6=volume
  cols.values[3] = close;
  cols.values[6] = std::vector<atx::f64>(r, 1e7); // volume
  cols.values[7] = shares;
  cols.values[10] = cumret;
  ASSERT_TRUE(atx::tsdb::build_from_long(cols, (dir / name).string(), 0).has_value());
}
} // namespace

TEST(DataHistoryPanel, DeterministicDigestAndCanonicalFields) {
  const fs::path dir = fs::temp_directory_path() / "atx_hist_panel";
  fs::remove_all(dir);
  fs::create_directories(dir);
  write_day(dir, "2020-01-02.seg", day_nanos(18263), {"33449", "33008"},
            {300.0, 20.0}, {1.0, 0.5}, {4e9, 1e9});
  write_day(dir, "2020-01-03.seg", day_nanos(18264), {"33449", "33008"},
            {303.0, 21.0}, {1.0, 0.5}, {4e9, 1e9});

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
