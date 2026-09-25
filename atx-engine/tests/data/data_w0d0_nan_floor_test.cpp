// W0-D0 — "a floor of 0 disables that screen" holds for NaN values too (D-09).
//
// Suite: DataUniverseNanFloor_W0d0
//
// The legacy test `value >= floor` failed a NaN even against a disabled floor of 0,
// so a name with no share count (NaN market cap) or an unfilled ADV window was
// excluded although the documentation said the screen was off. An ENABLED floor
// still fails NaN; the legacy rule stays reachable as NanFloorRule::NanFailsV1.

#include <limits>
#include <vector>

#include <gtest/gtest.h>

#include "atx/core/types.hpp"

#include "atx/engine/alpha/panel.hpp"
#include "atx/engine/data/corporate_actions.hpp"
#include "atx/engine/data/dataset.hpp"
#include "atx/engine/data/universe.hpp"

namespace atx_test_w0_d0_nan_floor {

using atx::engine::alpha::Panel;
using atx::engine::data::build_universe;
using atx::engine::data::corp_action_schema;
using atx::engine::data::Dataset;
using atx::engine::data::DatasetProvenance;
using atx::engine::data::DateKey;
using atx::engine::data::InstKey;
using atx::engine::data::kNoSector;
using atx::engine::data::NanFloorRule;
using atx::engine::data::UniverseConfig;
using atx::engine::data::UniverseFields;

namespace {

constexpr atx::f64 kNaN = std::numeric_limits<atx::f64>::quiet_NaN();
constexpr atx::usize kD = 3;
constexpr atx::usize kN = 3;

// Instrument 0: shares known. Instrument 1: shares NaN (no filing). Instrument 2:
// shares known but volume NaN on date 0 (ADV unknown there).
[[nodiscard]] Panel price_panel() {
  std::vector<atx::f64> close(kD * kN, 20.0);
  std::vector<atx::f64> volume(kD * kN, 1.0e6);
  volume[0 * kN + 2] = kNaN;
  auto p = Panel::create(kD, kN, {"close", "volume"}, {close, volume}, {});
  EXPECT_TRUE(p.has_value());
  return std::move(p).value();
}

[[nodiscard]] Dataset corp() {
  const atx::usize cells = kD * kN;
  std::vector<atx::f64> shares(cells, 1.0e6);
  for (atx::usize d = 0; d < kD; ++d) {
    shares[d * kN + 1] = kNaN;
  }
  std::vector<std::vector<atx::f64>> cols = {
      std::vector<atx::f64>(cells, 1.0), std::vector<atx::f64>(cells, 0.0), shares,
      std::vector<atx::f64>(cells, -1.0), std::vector<atx::f64>(cells, kNoSector),
      std::vector<atx::f64>(cells, kNoSector)};
  std::vector<DateKey> dates = {0, 1, 2};
  std::vector<InstKey> insts = {0, 1, 2};
  auto ds = Dataset::create(corp_action_schema(), std::move(dates), std::move(insts),
                            std::move(cols), {}, DatasetProvenance{"test:corp", ""});
  EXPECT_TRUE(ds.has_value());
  return std::move(ds).value();
}

[[nodiscard]] UniverseFields run(const UniverseConfig &cfg) {
  auto u = build_universe(price_panel(), corp(), cfg);
  EXPECT_TRUE(u.has_value());
  return std::move(u).value();
}

} // namespace

TEST(DataUniverseNanFloor_W0d0, DisabledFloorsAdmitNaNValues) {
  UniverseConfig cfg;
  cfg.adv_window = 1;
  cfg.min_adv_usd = 0.0;    // disabled
  cfg.min_mktcap_usd = 0.0; // disabled
  const UniverseFields v2 = run(cfg);
  cfg.nan_floor_rule = NanFloorRule::NanFailsV1;
  const UniverseFields v1 = run(cfg);
  // Instrument 1 (NaN market cap) and instrument 2 on date 0 (NaN ADV).
  for (atx::usize d = 0; d < kD; ++d) {
    EXPECT_EQ(v2.in_universe[d * kN + 1], 1U) << "disabled cap floor excluded a NaN cap, d=" << d;
    EXPECT_EQ(v1.in_universe[d * kN + 1], 0U) << "legacy rule, d=" << d;
  }
  EXPECT_EQ(v2.in_universe[0 * kN + 2], 1U);
  EXPECT_EQ(v1.in_universe[0 * kN + 2], 0U);
  EXPECT_EQ(v2.in_universe[0 * kN + 0], 1U);
}

TEST(DataUniverseNanFloor_W0d0, EnabledFloorsStillFailNaN) {
  UniverseConfig cfg;
  cfg.adv_window = 1;
  cfg.min_adv_usd = 1.0;
  cfg.min_mktcap_usd = 1.0;
  const UniverseFields v2 = run(cfg);
  for (atx::usize d = 0; d < kD; ++d) {
    EXPECT_EQ(v2.in_universe[d * kN + 1], 0U) << d;
    EXPECT_EQ(v2.in_universe[d * kN + 0], 1U) << d;
  }
  EXPECT_EQ(v2.in_universe[0 * kN + 2], 0U);
  EXPECT_EQ(v2.in_universe[1 * kN + 2], 1U);
}

// A cell with no traded price is never a member, even with every floor disabled.
TEST(DataUniverseNanFloor_W0d0, NoTradedPriceIsNeverAMember) {
  std::vector<atx::f64> close = {20.0, kNaN, 0.0};
  std::vector<atx::f64> volume = {1.0e6, 1.0e6, 1.0e6};
  auto p = Panel::create(1, 3, {"close", "volume"}, {close, volume}, {});
  ASSERT_TRUE(p.has_value());
  const atx::usize cells = 3;
  std::vector<std::vector<atx::f64>> cols = {
      std::vector<atx::f64>(cells, 1.0), std::vector<atx::f64>(cells, 0.0),
      std::vector<atx::f64>(cells, 1.0e6), std::vector<atx::f64>(cells, -1.0),
      std::vector<atx::f64>(cells, kNoSector), std::vector<atx::f64>(cells, kNoSector)};
  auto c = Dataset::create(corp_action_schema(), {0}, {0, 1, 2}, std::move(cols), {},
                           DatasetProvenance{"test:corp", ""});
  ASSERT_TRUE(c.has_value());
  UniverseConfig cfg;
  cfg.adv_window = 1;
  cfg.min_adv_usd = 0.0;
  auto u = build_universe(*p, *c, cfg);
  ASSERT_TRUE(u.has_value());
  EXPECT_EQ(u->in_universe, (std::vector<atx::u8>{1, 0, 0}));
}

// With the ADV floor disabled a NaN ADV can reach the top-N cap; it ranks after
// every finite ADV (deterministic, no NaN comparison in the sort).
TEST(DataUniverseNanFloor_W0d0, NaNAdvRanksLastInTopN) {
  UniverseConfig cfg;
  cfg.adv_window = 1;
  cfg.min_adv_usd = 0.0;
  cfg.top_n_by_adv = 2;
  const UniverseFields u = run(cfg);
  // Date 0: instruments 0 and 1 have finite ADV (equal), instrument 2 NaN -> dropped.
  EXPECT_EQ(u.in_universe[0 * kN + 0], 1U);
  EXPECT_EQ(u.in_universe[0 * kN + 1], 1U);
  EXPECT_EQ(u.in_universe[0 * kN + 2], 0U);
  // Date 1: all finite and equal -> ascending id tie-break keeps 0 and 1.
  EXPECT_EQ(u.in_universe[1 * kN + 2], 0U);
}

} // namespace atx_test_w0_d0_nan_floor
