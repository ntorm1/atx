#include <cmath>
#include <filesystem>
#include <string>
#include <vector>
#include <gtest/gtest.h>
#include "atx/core/types.hpp"
#include "atx/tsdb/builder.hpp"
#include "atx/engine/alpha/panel.hpp"
#include "atx/engine/data/real_panel.hpp"

namespace atxtest_real_panel_regime {
namespace fs = std::filesystem;
constexpr atx::i64 kNsPerDay = 86'400LL * 1'000'000'000LL;
using atx::engine::data::finalize_panel_with_regime;

[[nodiscard]] std::string regime_seg() {
  std::vector<std::string> fields = {"vix"};
  std::vector<std::string> syms = {"MACRO"};
  std::vector<atx::i64> axis = {0, kNsPerDay};   // two dates: day 0 and day 1
  atx::tsdb::SegmentBuilder b(fields, syms, axis);
  b.set(0, 0, 0, 15.0);  // axis[0] (index 0) -> vix 15
  b.set(0, 1, 0, 25.0);  // axis[1] (index 1) -> vix 25
  const std::string p = (fs::temp_directory_path() / "atx_real_panel_regime.seg").string();
  EXPECT_TRUE(b.write(p, 0).has_value());
  return p;
}

TEST(RealPanelRegime, RegimeOff_NoRegimeColumns_NoRegression) {
  std::vector<std::string> names = {"close"};
  std::vector<std::vector<atx::f64>> data = {{10.0, 20.0}};
  std::vector<atx::i64> panel_dates = {0, kNsPerDay};
  auto p = finalize_panel_with_regime(2, 1, panel_dates, names, data, {}, "", {});
  ASSERT_TRUE(p.has_value()) << (p ? "" : p.error().message());
  EXPECT_FALSE(p.value().field_id("regime_vix").has_value());  // no regime field
  EXPECT_TRUE(p.value().field_id("close").has_value());        // base field intact
}

TEST(RealPanelRegime, RegimeOn_BroadcastsAsOf) {
  const std::string seg = regime_seg();
  std::vector<std::string> names = {"close"};
  std::vector<std::vector<atx::f64>> data = {{10.0, 20.0}};
  std::vector<atx::i64> panel_dates = {0, kNsPerDay};
  std::vector<std::string> req = {"vix"};
  auto p = finalize_panel_with_regime(2, 1, panel_dates, names, data, {}, seg, req);
  ASSERT_TRUE(p.has_value()) << (p ? "" : p.error().message());
  auto fid = p.value().field_id("regime_vix");
  ASSERT_TRUE(fid.has_value());
  const auto col = p.value().field_all(fid.value());
  EXPECT_DOUBLE_EQ(col[0], 15.0);  // date 0 -> vix 15
  EXPECT_DOUBLE_EQ(col[1], 25.0);  // date 1 -> vix 25
}

TEST(RealPanelRegime, RegimeOn_OutOfUniverseIsNaN) {
  const std::string seg = regime_seg();
  std::vector<std::string> names = {"close"};
  std::vector<std::vector<atx::f64>> data = {{10.0, 20.0}};  // 1 date x 2 inst
  std::vector<atx::i64> panel_dates = {0};
  std::vector<std::uint8_t> universe = {1, 0};  // inst 1 out of universe
  std::vector<std::string> req = {"vix"};
  auto p = finalize_panel_with_regime(1, 2, panel_dates, names, data, universe, seg, req);
  ASSERT_TRUE(p.has_value()) << (p ? "" : p.error().message());
  const auto col = p.value().field_all(p.value().field_id("regime_vix").value());
  EXPECT_DOUBLE_EQ(col[0], 15.0);
  EXPECT_TRUE(std::isnan(col[1]));
}
}  // namespace atxtest_real_panel_regime
