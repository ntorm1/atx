#include <array>
#include <bit>
#include <cmath>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "atx/engine/alpha/panel.hpp"
#include "equity_baseline_views.hpp"
#include "panel_artifact.hpp"

namespace atx_test_w0_membership_views {
namespace impl = atx::impl;
constexpr atx::usize kDates = 276;
constexpr atx::usize kBegin = 256;
constexpr atx::usize kJoin = 264;
constexpr atx::usize kNames = 4;
constexpr atx::i64 kDay = 86'400'000'000'000LL;
const std::array<std::string_view, 7> kDsl{
    "rank(close)", "normalize(close)", "group_neutralize(close, sector)",
    "ts_mean(normalize(close), 3)", "rank(ts_mean(close, 5))", "ts_mean(close, 5)",
    "group_neutralize(ts_mean(close, 5), sector)"};
const std::array<std::string_view, 7> kNamesDsl{
    "rank", "normalize", "neutral", "rolling_normalize", "rank_rolling",
    "rolling_price", "neutral_rolling"};

impl::PanelArtifact context(bool perturb_joiner = false) {
  std::vector<double> close(kDates * kNames);
  std::vector<double> raw(kDates * kNames);
  std::vector<double> volume(kDates * kNames, 1e6);
  std::vector<double> sector(kDates * kNames, 1.0);
  std::vector<atx::u8> universe(kDates * kNames, 0);
  for (atx::usize d = 0; d < kDates; ++d) {
    for (atx::usize i = 0; i < kNames; ++i) {
      const auto cell = d * kNames + i;
      close[cell] = 50.0 + static_cast<double>(i * 17 + d * (i + 1));
      if (perturb_joiner && i == 3 && d < kJoin) {
        close[cell] = 1.0 + 0.001 * static_cast<double>(d);
      }
      raw[cell] = close[cell] * 0.5;
      universe[cell] = d >= kBegin ? 1U : 0U;
    }
  }
  auto panel = atx::engine::alpha::Panel::create(kDates, kNames,
      {"close", "raw_close", "volume", "sector"},
      {std::move(close), std::move(raw), std::move(volume), std::move(sector)},
      std::move(universe)).value();
  impl::PanelIdentity identity;
  identity.instrument_namespace = "synthetic.securityID";
  identity.recipe = "synthetic pre-2020 membership fixture";
  identity.parents = {{"fixture", std::string(64, 'a')}};
  for (atx::usize d = 0; d < kDates; ++d) {
    identity.session_keys.push_back(static_cast<atx::i64>(d) * kDay);
  }
  for (atx::usize i = 0; i < kNames; ++i) {
    identity.instrument_ids.push_back(std::to_string(i + 1));
    identity.original_instrument_indices.push_back(i);
  }
  return {std::move(panel), std::move(identity), std::string(64, 'b'), std::string(64, 'c')};
}

impl::EquityBaselineConfig config(bool constant = false, bool legacy = false) {
  impl::EquityBaselineConfig cfg;
  cfg.evaluation = {static_cast<atx::i64>(kBegin) * kDay,
                    static_cast<atx::i64>(kDates) * kDay};
  cfg.observation_basis =
      impl::EquityBaselineObservationBasis::ArchiveRawVolumeAndPointwiseAdjustedCloseV1;
  if (legacy) {
    cfg.membership_rule = impl::EquityMembershipRule::ContextYearUnionV1;
  } else {
    impl::EquityAsOfMembership m;
    m.effective_session_keys = {0};
    m.security_ids = {constant ? std::vector<atx::i64>{1, 2, 3, 4}
                              : std::vector<atx::i64>{1, 2, 3}};
    if (!constant) {
      m.effective_session_keys.push_back(static_cast<atx::i64>(kJoin) * kDay);
      m.security_ids.push_back({1, 2, 3, 4});
    }
    cfg.membership = std::move(m);
  }
  return cfg;
}

TEST(ImplMembershipCrossSection, NonmemberValuesCannotMoveIncumbentFamilies) {
  const auto original = context();
  const auto changed = context(true);
  const auto cfg = config();
  const auto base = impl::evaluate_equity_baseline(original, cfg);
  const auto altered_base = impl::evaluate_equity_baseline(changed, cfg);
  ASSERT_TRUE(base.has_value() && altered_base.has_value());
  const auto a = impl::evaluate_equity_families(original, cfg, *base, kDsl, kNamesDsl);
  const auto b = impl::evaluate_equity_families(changed, cfg, *altered_base, kDsl, kNamesDsl);
  ASSERT_TRUE(a.has_value() && b.has_value());
  for (atx::usize s = 0; s < kDsl.size(); ++s) {
    for (atx::usize row = 0; row < kJoin - kBegin; ++row) {
      for (atx::usize i = 0; i < 3; ++i) {
        const auto cell = row * kNames + i;
        ASSERT_TRUE(std::isfinite(a->signals.alphas[s].values[cell])) << kDsl[s];
        EXPECT_EQ(std::bit_cast<atx::u64>(a->signals.alphas[s].values[cell]),
                  std::bit_cast<atx::u64>(b->signals.alphas[s].values[cell]))
            << kDsl[s] << " row " << row << " name " << i;
      }
      EXPECT_TRUE(std::isnan(a->signals.alphas[s].values[row * kNames + 3]));
    }
  }
  // The old year-union rule sees the perturbed nonmember and changes incumbents.
  const auto old_cfg = config(false, true);
  const auto old_base = impl::evaluate_equity_baseline(original, old_cfg);
  const auto changed_old_base = impl::evaluate_equity_baseline(changed, old_cfg);
  ASSERT_TRUE(old_base.has_value() && changed_old_base.has_value());
  const auto old = impl::evaluate_equity_families(original, old_cfg, *old_base, kDsl, kNamesDsl);
  const auto changed_old = impl::evaluate_equity_families(changed, old_cfg,
      *changed_old_base, kDsl, kNamesDsl);
  ASSERT_TRUE(old.has_value() && changed_old.has_value());
  EXPECT_NE(old->signals.alphas[0].values[0], changed_old->signals.alphas[0].values[0]);
}

TEST(ImplMembershipCrossSection, JoinerKeepsRawHistoryButHasNoPreEntryCsObservations) {
  const auto input = context();
  const auto cfg = config();
  const auto old_cfg = config(false, true);
  const auto base = impl::evaluate_equity_baseline(input, cfg);
  const auto old_base = impl::evaluate_equity_baseline(input, old_cfg);
  ASSERT_TRUE(base.has_value() && old_base.has_value());
  const auto a = impl::evaluate_equity_families(input, cfg, *base, kDsl, kNamesDsl);
  const auto old = impl::evaluate_equity_families(input, old_cfg, *old_base, kDsl, kNamesDsl);
  ASSERT_TRUE(a.has_value() && old.has_value());
  const auto join_cell = (kJoin - kBegin) * kNames + 3;
  ASSERT_TRUE(base->panel.in_universe(kJoin - kBegin, 3));
  ASSERT_TRUE(std::isfinite(a->signals.alphas[5].values[join_cell]));
  EXPECT_DOUBLE_EQ(a->signals.alphas[5].values[join_cell],
                   old->signals.alphas[5].values[join_cell]);
  // Rolling normalized history really starts at entry; raw-price history does not.
  EXPECT_TRUE(std::isnan(a->signals.alphas[3].values[join_cell]));
  EXPECT_TRUE(std::isnan(a->signals.alphas[3].values[join_cell + kNames]));
  EXPECT_TRUE(std::isfinite(a->signals.alphas[3].values[join_cell + 2 * kNames]));
}

TEST(ImplMembershipCrossSection, ConstantMembershipPreservesEveryFamilyBit) {
  const auto input = context();
  const auto cfg = config(true);
  const auto old_cfg = config(false, true);
  const auto base = impl::evaluate_equity_baseline(input, cfg);
  const auto old_base = impl::evaluate_equity_baseline(input, old_cfg);
  ASSERT_TRUE(base.has_value() && old_base.has_value());
  const auto a = impl::evaluate_equity_families(input, cfg, *base, kDsl, kNamesDsl);
  const auto old = impl::evaluate_equity_families(input, old_cfg, *old_base, kDsl, kNamesDsl);
  ASSERT_TRUE(a.has_value() && old.has_value());
  for (atx::usize s = 0; s < kDsl.size(); ++s) {
    ASSERT_EQ(a->signals.alphas[s].values.size(), old->signals.alphas[s].values.size());
    for (atx::usize c = 0; c < a->signals.alphas[s].values.size(); ++c) {
      EXPECT_EQ(std::bit_cast<atx::u64>(a->signals.alphas[s].values[c]),
                std::bit_cast<atx::u64>(old->signals.alphas[s].values[c]));
    }
  }
  auto limited = cfg;
  limited.max_additional_bytes = a->additional_array_bytes - 1;
  EXPECT_FALSE(impl::evaluate_equity_families(input, limited, *base, kDsl, kNamesDsl));
}
} // namespace atx_test_w0_membership_views
