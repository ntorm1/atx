#include <cmath>
#include <filesystem>
#include <span>
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
