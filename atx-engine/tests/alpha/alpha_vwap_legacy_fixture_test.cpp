// Independent frozen pre-D0 benchmark input oracle, copied from b185d056440704e7ebcfe2b9395601d7e5264269
// alpha/wq101_battery.hpp and alpha/datafields.hpp. RNG, field order, arithmetic
// order and masks are frozen here; production V1 is called only by the actual side.
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <string>
#include <utility>
#include <vector>
#include <gtest/gtest.h>
#include "atx/engine/alpha/wq101_battery.hpp"
#include "atx/engine/data/panel_digest.hpp"

namespace atx_test_vwap_fixture {
using atx::engine::alpha::Panel;
[[nodiscard]] inline atx::core::Result<Panel> frozen_pre_d0_panel(atx::usize dates,
                                                               atx::usize instruments,
                                                               std::uint64_t seed = 0x5eed) {
  const atx::usize cells = dates * instruments;
  std::uint64_t s = seed | 1U;
  auto next = [&s]() noexcept {
    s = s * 6364136223846793005ULL + 1442695040888963407ULL;
    return static_cast<atx::f64>(s >> 11U) / static_cast<atx::f64>(1ULL << 53U);
  };
  enum Col : atx::usize { kC, kO, kH, kL, kV, kR, kCap, kSec, kInd, kSub, kNCols };
  std::vector<std::vector<atx::f64>> cols(kNCols, std::vector<atx::f64>(cells));
  std::vector<std::uint8_t> universe(cells, 1);
  std::vector<atx::f64> px(instruments);
  for (atx::usize j = 0; j < instruments; ++j) {
    px[j] = 20.0 + next() * 80.0;
  }
  for (atx::usize d = 0; d < dates; ++d) {
    for (atx::usize j = 0; j < instruments; ++j) {
      const atx::usize i = d * instruments + j;
      const atx::f64 prev = px[j];
      px[j] = prev * (1.0 + (next() - 0.5) * 0.04);
      const atx::f64 spread = px[j] * (0.002 + next() * 0.02);
      cols[kC][i] = px[j];
      cols[kO][i] = prev * (1.0 + (next() - 0.5) * 0.01);
      cols[kH][i] = std::fmax(px[j], cols[kO][i]) + spread;
      cols[kL][i] = std::fmin(px[j], cols[kO][i]) - spread;
      cols[kV][i] = 1.0e5 + next() * 9.0e6;
      cols[kR][i] = d == 0 ? std::nan("") : px[j] / prev - 1.0;
      cols[kCap][i] = px[j] * (1.0e6 + static_cast<atx::f64>(j) * 1.0e4);
      cols[kSec][i] = static_cast<atx::f64>(j % 11U);
      cols[kInd][i] = static_cast<atx::f64>(j % 37U);
      cols[kSub][i] = static_cast<atx::f64>(j % 71U);
      universe[i] = next() < 0.01 ? std::uint8_t{0} : std::uint8_t{1};
    }
  }
  std::vector<std::string> names = {"close",   "open", "high",           "low",
                                    "volume",  "returns", "cap",         "IndClass.sector",
                                    "IndClass.industry", "IndClass.subindustry"};
  // Frozen legacy datafields recipe: do not call the production derivation.
  const auto nan = std::numeric_limits<atx::f64>::quiet_NaN();
  std::vector<atx::f64> dvol(cells, nan), vwap(cells, nan);
  for (atx::usize i = 0; i < cells; ++i) {
    if (universe[i] != 0) {
      dvol[i] = cols[kC][i] * cols[kV][i];
      vwap[i] = (cols[kH][i] + cols[kL][i] + cols[kC][i]) / 3.0;
    }
  }
  names.emplace_back("dollar_volume");
  cols.push_back(dvol);
  names.emplace_back("vwap");
  cols.push_back(std::move(vwap));
  constexpr std::array<atx::u16, 7> windows{20, 30, 40, 50, 60, 120, 180};
  for (const auto window : windows) {
    std::vector<atx::f64> adv(cells, nan);
    for (atx::usize j = 0; j < instruments; ++j) {
      for (atx::usize t = 0; t < dates; ++t) {
        if (t + 1 < window) continue;
        atx::f64 sum = 0.0;
        bool valid = true;
        for (atx::usize k = t + 1 - window; k <= t; ++k) {
          const auto value = dvol[k * instruments + j];
          if (std::isnan(value)) { valid = false; break; }
          sum += value;
        }
        if (valid) adv[t * instruments + j] = sum / static_cast<atx::f64>(window);
      }
    }
    names.push_back("adv" + std::to_string(window));
    cols.push_back(std::move(adv));
  }
  return Panel::create(dates, instruments, std::move(names), std::move(cols),
                        std::move(universe));
}

TEST(VwapLegacyFixture, N128MatchesFrozenPreD0InputBytesAndDigest) {
  constexpr atx::usize dates = 2520;
  constexpr atx::usize instruments = 128;
  const auto expected = frozen_pre_d0_panel(dates, instruments);
  const auto actual = atx::engine::alpha::make_wq101_panel(dates, instruments);
  ASSERT_TRUE(expected && actual);
  ASSERT_EQ(actual->dates(), expected->dates());
  ASSERT_EQ(actual->instruments(), expected->instruments());
  ASSERT_EQ(actual->num_fields(), expected->num_fields());
  for (atx::usize f = 0; f < actual->num_fields(); ++f) {
    ASSERT_EQ(actual->field_name(f), expected->field_name(f));
    const auto a = actual->field_all(static_cast<atx::engine::alpha::FieldId>(f));
    const auto e = expected->field_all(static_cast<atx::engine::alpha::FieldId>(f));
    ASSERT_EQ(a.size(), e.size());
    for (atx::usize k = 0; k < a.size(); ++k) {
      ASSERT_EQ(std::bit_cast<std::uint64_t>(a[k]), std::bit_cast<std::uint64_t>(e[k]))
          << "field=" << actual->field_name(f) << " cell=" << k;
    }
  }
  for (atx::usize d = 0; d < dates; ++d) {
    for (atx::usize i = 0; i < instruments; ++i) {
      ASSERT_EQ(actual->in_universe(d, i), expected->in_universe(d, i));
    }
  }
  const auto actual_digest = atx::engine::data::digest_panel(*actual);
  const auto expected_digest = atx::engine::data::digest_panel(*expected);
  EXPECT_EQ(actual_digest, expected_digest);
  std::printf("[vwap-fixture] dates=2520 instruments=128 fields=%zu digest=%016llx\n",
      static_cast<std::size_t>(actual->num_fields()),
      static_cast<unsigned long long>(actual_digest));
}
} // namespace atx_test_vwap_fixture
