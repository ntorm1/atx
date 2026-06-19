#include <cmath>
#include <filesystem>
#include <string>
#include <vector>
#include <gtest/gtest.h>
#include "atx/core/types.hpp"
#include "atx/tsdb/builder.hpp"
#include "atx/engine/alpha/panel.hpp"
#include "atx/engine/regime/store.hpp"
#include "atx/engine/regime/with_regime_fields.hpp"

namespace atxtest_regime_fields {
namespace fs = std::filesystem;
constexpr atx::i64 kDay = 86400LL * 1000000000LL;
using atx::engine::alpha::Panel;
using atx::engine::regime::RegimeStore;
using atx::engine::regime::with_regime_fields;

[[nodiscard]] std::string regime_seg() {
  std::vector<std::string> fields = {"vix"};
  std::vector<std::string> syms = {"MACRO"};
  std::vector<atx::i64> axis = {2 * kDay, 4 * kDay, 6 * kDay};
  atx::tsdb::SegmentBuilder b(fields, syms, axis);
  // set(field, TIME_INDEX, inst, value): 2nd arg indexes `axis`, NOT nanos.
  b.set(0, 0, 0, 15.0); b.set(0, 1, 0, 25.0); b.set(0, 2, 0, 18.0);  // vix @ axis[0..2]
  const std::string p = (fs::temp_directory_path() / "atx_regime_fields.seg").string();
  EXPECT_TRUE(b.write(p, 0).has_value());
  return p;
}

TEST(RegimeFields, BroadcastsAsOfAcrossInstruments) {
  auto store = RegimeStore::open(regime_seg()).value();
  const atx::usize dates = 3, inst = 2;
  std::vector<atx::i64> panel_dates = {2 * kDay, 5 * kDay, 6 * kDay};  // 5 absent in regime
  std::vector<std::string> names = {"close"};
  std::vector<std::vector<atx::f64>> data = {std::vector<atx::f64>(dates * inst, 1.0)};
  std::vector<std::string> req = {"vix"};
  auto p = with_regime_fields(dates, inst, panel_dates, names, data, {}, store, req);
  ASSERT_TRUE(p.has_value()) << (p ? "" : p.error().message());
  auto fid = p.value().field_id("regime_vix");
  ASSERT_TRUE(fid.has_value());
  const auto col = p.value().field_all(fid.value());
  // date 0 (=2): 15 for both instruments; date 1 (=5): as-of -> 25; date 2 (=6): 18
  EXPECT_DOUBLE_EQ(col[0 * inst + 0], 15.0);
  EXPECT_DOUBLE_EQ(col[0 * inst + 1], 15.0);
  EXPECT_DOUBLE_EQ(col[1 * inst + 0], 25.0);
  EXPECT_DOUBLE_EQ(col[2 * inst + 0], 18.0);
}

TEST(RegimeFields, OutOfUniverseCellIsNaN) {
  auto store = RegimeStore::open(regime_seg()).value();
  const atx::usize dates = 1, inst = 2;
  std::vector<atx::i64> panel_dates = {2 * kDay};
  std::vector<std::string> names = {"close"};
  std::vector<std::vector<atx::f64>> data = {{1.0, 1.0}};
  std::vector<std::uint8_t> universe = {1, 0};  // inst 1 out of universe
  auto p = with_regime_fields(dates, inst, panel_dates, names, data, universe, store, {"vix"});
  ASSERT_TRUE(p.has_value());
  auto col = p.value().field_all(p.value().field_id("regime_vix").value());
  EXPECT_DOUBLE_EQ(col[0], 15.0);
  EXPECT_TRUE(std::isnan(col[1]));
}

TEST(RegimeFields, NameCollisionIsError) {
  auto store = RegimeStore::open(regime_seg()).value();
  std::vector<std::string> names = {"close", "regime_vix"};  // already present
  std::vector<std::vector<atx::f64>> data = {{1.0}, {2.0}};
  auto p = with_regime_fields(1, 1, std::vector<atx::i64>{2 * kDay}, names, data, {}, store, {"vix"});
  EXPECT_FALSE(p.has_value());
}
}  // namespace atxtest_regime_fields
