// W0-D0 — history-panel point-in-time invariance and the level-basis tags (D-01).
//
// Suites:
//   DataHistoryPanelFuturePerturb_W0d0 — plan acceptance "future-perturbation
//     invariance": mutating every source row (factors included) dated after t
//     leaves every build_history_panel output row <= t bit-identical, for every
//     field, the universe mask, the level-basis tags, and every Alpha101-derived
//     field (dollar_volume, vwap, adv{d}, returns, cap, IndClass.*).
//   DataLevelBasis_W0d0 — every history-panel field carries a level basis;
//     dollar_volume / adv{d} are built from raw_close x volume, so a vendor
//     re-snapshot of the backward factor (a later split) cannot move them, while
//     the legacy CloseV1 rule scales them by the factor (the D-01 look-ahead).
//
// Fixtures are synthetic ORATS-shaped .seg partitions written in-process with the
// atx-tsdb builder (dated 2016); no real data is read.

#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

#include "atx/core/types.hpp"

#include "atx/engine/alpha/augment.hpp"
#include "atx/engine/alpha/datafields.hpp"
#include "atx/engine/alpha/panel.hpp"
#include "atx/engine/data/history_panel.hpp"
#include "atx/engine/data/orats_history.hpp" // kOratsFields
#include "atx/tsdb/builder.hpp"
#include "atx/tsdb/load_parquet.hpp"

namespace atx_test_w0_d0_future_perturb {

namespace fs = std::filesystem;
namespace alpha = atx::engine::alpha;
using atx::engine::data::build_history_panel;
using atx::engine::data::history_field_level_basis;
using atx::engine::data::HistoryDataConfig;
using atx::engine::data::HistoryPanel;
using atx::engine::data::kOratsFields;
using atx::engine::data::LevelBasis;

namespace {

constexpr atx::usize kDates = 30;
constexpr atx::usize kInsts = 5;
constexpr atx::i64 kFirstDay = 17000; // 2016-07-18 (synthetic; only ordering matters)

[[nodiscard]] atx::i64 day_nanos(atx::i64 day) { return day * 86400LL * 1000000000LL; }

// Source value of ORATS field `field` for (date d, instrument i). `mutated` selects
// a DIFFERENT, still-valid generator — the "future" the invariance test swaps in.
struct SourceModel {
  bool mutated = false;
  atx::f64 factor_scale = 1.0; // multiplies every cumReturnFactor (vendor re-snapshot)

  [[nodiscard]] atx::f64 value(std::string_view field, atx::usize d, atx::usize i) const {
    const atx::f64 dd = static_cast<atx::f64>(d);
    const atx::f64 ii = static_cast<atx::f64>(i);
    const atx::f64 m = mutated ? 1.0 : 0.0;
    // A raw close with drift, a 2:1 split for instrument 1 at d = 12 and a mutated
    // regime that moves every price, volume and share count.
    atx::f64 close = (20.0 + 7.0 * ii) * (1.0 + 0.004 * dd) * (1.0 + 0.37 * m);
    if (i == 1 && d >= 12) {
      close *= 0.5;
    }
    if (field == "close" || field == "closePr" || field == "closeUnadjPr") {
      return close;
    }
    if (field == "open") {
      return close * (0.99 + 0.002 * m);
    }
    if (field == "high") {
      return close * (1.02 + 0.01 * m);
    }
    if (field == "low") {
      return close * (0.97 - 0.01 * m);
    }
    if (field == "volume") {
      return (1.0e5 + 2.5e4 * ii + 1.0e3 * dd) * (1.0 + 3.0 * m);
    }
    if (field == "shares") {
      atx::f64 shares = (1.0e7 + 1.0e6 * ii) * (1.0 + 0.2 * m);
      if (i == 1 && d >= 12) {
        shares *= 2.0;
      }
      return shares;
    }
    if (field == "cumReturnFactor") {
      // Backward factor: instrument 1 splits at d = 12 (0.5 before), instrument 3
      // pays a dividend at d = 8. The mutated future adds a different split.
      atx::f64 f = 1.0;
      if (i == 1 && d < 12) {
        f = 0.5;
      }
      if (i == 3 && d < 8) {
        f = 0.98;
      }
      if (mutated) {
        f *= 0.25;
      }
      return f * factor_scale;
    }
    if (field == "gics") {
      return mutated ? 60.0 : (i % 2 == 0 ? 45.0 : 20.0);
    }
    if (field == "earnFlag") {
      return ((d + i + (mutated ? 1U : 0U)) % 7 == 0) ? 1.0 : 0.0;
    }
    if (field == "atmCenI_21d") {
      return 0.20 + 0.01 * ii + 0.001 * dd + 0.05 * m;
    }
    if (field == "atmCenI_126d") {
      return 0.25 + 0.01 * ii + 0.0005 * dd + 0.03 * m;
    }
    if (field == "nEarnCnt_5d") {
      return static_cast<atx::f64>((d + i) % 3) + 2.0 * m;
    }
    return 1.0 + 0.1 * ii + 0.01 * dd + m; // returnFactor / totalReturn: unused
  }
};

// Write one .seg per date. Rows d <= t come from `past`, rows d > t from `future`.
void write_partition(const fs::path &dir, const SourceModel &past, const SourceModel &future,
                     atx::usize t) {
  fs::remove_all(dir);
  fs::create_directories(dir);
  for (atx::usize d = 0; d < kDates; ++d) {
    const SourceModel &model = (d <= t) ? past : future;
    atx::tsdb::LongColumns cols;
    cols.field_names.assign(kOratsFields.begin(), kOratsFields.end());
    cols.times.assign(kInsts, day_nanos(kFirstDay + static_cast<atx::i64>(d)));
    for (atx::usize i = 0; i < kInsts; ++i) {
      cols.symbols.push_back(std::to_string(101 + i));
    }
    cols.values.assign(kOratsFields.size(), std::vector<atx::f64>(kInsts, 0.0));
    for (atx::usize f = 0; f < kOratsFields.size(); ++f) {
      for (atx::usize i = 0; i < kInsts; ++i) {
        cols.values[f][i] = model.value(kOratsFields[f], d, i);
      }
    }
    const std::string name = "d" + std::to_string(1000 + d) + ".seg";
    ASSERT_TRUE(atx::tsdb::build_from_long(cols, (dir / name).string(), 0).has_value());
  }
}

// A screen that exercises every universe path: causal ADV with a warm-up, an
// enabled ADV floor, a price floor and a top-N cap.
[[nodiscard]] HistoryDataConfig screen_config(const fs::path &dir) {
  HistoryDataConfig cfg;
  cfg.seg_dir = dir.string();
  cfg.universe.adv_window = 3;
  cfg.universe.min_adv_usd = 1.0;
  cfg.universe.min_price = 1.0;
  cfg.universe.top_n_by_adv = 4;
  return cfg;
}

[[nodiscard]] HistoryPanel build(const fs::path &dir, const SourceModel &past,
                                 const SourceModel &future, atx::usize t) {
  write_partition(dir, past, future, t);
  auto built = build_history_panel(screen_config(dir));
  EXPECT_TRUE(built.has_value()) << (built ? "" : built.error().to_string());
  return std::move(built).value();
}

[[nodiscard]] bool same_bits(atx::f64 a, atx::f64 b) {
  return std::bit_cast<atx::u64>(a) == std::bit_cast<atx::u64>(b);
}

// Compare rows [0, t] of every field + the mask of two panels with identical
// shapes and field order. Returns the number of cells compared; records a failure
// per differing cell. `diff_after` counts differing cells in row t + 1.
struct RowCompare {
  atx::usize cells_compared = 0;
  atx::usize diff_after = 0;
};

[[nodiscard]] RowCompare compare_rows(const alpha::Panel &a, const alpha::Panel &b, atx::usize t,
                                      std::string_view what) {
  RowCompare out;
  EXPECT_EQ(a.dates(), b.dates()) << what;
  EXPECT_EQ(a.instruments(), b.instruments()) << what;
  EXPECT_EQ(a.num_fields(), b.num_fields()) << what;
  if (a.dates() != b.dates() || a.instruments() != b.instruments() ||
      a.num_fields() != b.num_fields()) {
    return out;
  }
  const atx::usize n = a.instruments();
  for (atx::usize f = 0; f < a.num_fields(); ++f) {
    const auto fid = static_cast<alpha::FieldId>(f);
    EXPECT_EQ(a.field_name(fid), b.field_name(fid)) << what;
    const std::span<const atx::f64> ca = a.field_all(fid);
    const std::span<const atx::f64> cb = b.field_all(fid);
    for (atx::usize d = 0; d <= t; ++d) {
      for (atx::usize i = 0; i < n; ++i) {
        ++out.cells_compared;
        EXPECT_TRUE(same_bits(ca[d * n + i], cb[d * n + i]))
            << what << ": field " << a.field_name(fid) << " row " << d << " inst " << i
            << " moved when rows > " << t << " changed: " << ca[d * n + i] << " vs "
            << cb[d * n + i];
      }
    }
    if (t + 1 < a.dates()) {
      for (atx::usize i = 0; i < n; ++i) {
        out.diff_after += same_bits(ca[(t + 1) * n + i], cb[(t + 1) * n + i]) ? 0U : 1U;
      }
    }
  }
  for (atx::usize d = 0; d <= t; ++d) {
    for (atx::usize i = 0; i < n; ++i) {
      EXPECT_EQ(a.in_universe(d, i), b.in_universe(d, i))
          << what << ": mask row " << d << " inst " << i;
    }
  }
  return out;
}

[[nodiscard]] std::span<const atx::f64> field(const alpha::Panel &p, std::string_view name) {
  const auto id = p.field_id(name);
  EXPECT_TRUE(id.has_value()) << "missing field " << name;
  return p.field_all(id.value());
}

constexpr std::array<atx::u16, 2> kAdvWindows{2, 5};

} // namespace

// ---------------------------------------------------------------------------
// Acceptance: future-perturbation invariance, for several cut points t.
// ---------------------------------------------------------------------------
TEST(DataHistoryPanelFuturePerturb_W0d0, EveryRowUpToTIsBitIdenticalForAllFields) {
  const fs::path dir_a = fs::temp_directory_path() / "atx_w0d0_perturb_a";
  const fs::path dir_b = fs::temp_directory_path() / "atx_w0d0_perturb_b";
  const SourceModel base{};
  SourceModel future = base;
  future.mutated = true;

  atx::usize total_cells = 0;
  atx::usize total_alpha_cells = 0;
  for (const atx::usize t : {atx::usize{4}, atx::usize{11}, atx::usize{12}, atx::usize{20}}) {
    const HistoryPanel clean = build(dir_a, base, base, t);
    const HistoryPanel perturbed = build(dir_b, base, future, t);
    ASSERT_EQ(clean.panel.dates(), kDates);
    ASSERT_EQ(clean.panel.instruments(), kInsts);
    EXPECT_EQ(clean.instrument_ids, perturbed.instrument_ids);
    EXPECT_EQ(clean.field_basis, perturbed.field_basis) << "basis tags moved";

    const RowCompare base_cmp = compare_rows(clean.panel, perturbed.panel, t, "history panel");
    total_cells += base_cmp.cells_compared;
    // Control: the mutation really changed the future (row t+1 differs).
    EXPECT_GT(base_cmp.diff_after, 0U) << "the future mutation did not take effect at t=" << t;

    // The Alpha101 augmentation (dollar_volume/vwap/adv{d}/returns/...) inherits it.
    auto aug_clean = alpha::with_alpha101_fields(clean.panel, kAdvWindows);
    auto aug_pert = alpha::with_alpha101_fields(perturbed.panel, kAdvWindows);
    ASSERT_TRUE(aug_clean.has_value()) << aug_clean.error().to_string();
    ASSERT_TRUE(aug_pert.has_value()) << aug_pert.error().to_string();
    const RowCompare aug_cmp = compare_rows(*aug_clean, *aug_pert, t, "alpha101 augmented");
    total_alpha_cells += aug_cmp.cells_compared;
    EXPECT_GT(aug_cmp.diff_after, 0U);
  }
  // The augmented panel carries every history field plus the derived ones, so it
  // compares strictly more cells. Counts are printed for the lane report.
  EXPECT_GT(total_cells, 0U);
  EXPECT_GT(total_alpha_cells, total_cells);
  std::printf("[perturb] history cells compared=%zu augmented cells compared=%zu\n",
              static_cast<std::size_t>(total_cells), static_cast<std::size_t>(total_alpha_cells));
  fs::remove_all(dir_a);
  fs::remove_all(dir_b);
}

// The universe screen actually varies across rows in this fixture (warm-up NaN
// ADV, top-N cap), so the invariance above covers a non-trivial mask.
TEST(DataHistoryPanelFuturePerturb_W0d0, FixtureExercisesWarmupAndTopNCap) {
  const fs::path dir = fs::temp_directory_path() / "atx_w0d0_perturb_mask";
  const SourceModel base{};
  const HistoryPanel p = build(dir, base, base, kDates);
  atx::usize in_row0 = 0;
  atx::usize in_row10 = 0;
  for (atx::usize i = 0; i < kInsts; ++i) {
    in_row0 += p.panel.in_universe(0, i) ? 1U : 0U;
    in_row10 += p.panel.in_universe(10, i) ? 1U : 0U;
  }
  EXPECT_EQ(in_row0, 0U) << "ADV warm-up (window 3) must exclude row 0";
  EXPECT_EQ(in_row10, 4U) << "top-N cap (4 of 5) must bind once ADV is warm";
  fs::remove_all(dir);
}

// ---------------------------------------------------------------------------
// DataLevelBasis — tags on every field.
// ---------------------------------------------------------------------------
TEST(DataLevelBasis_W0d0, HistoryPanelTagsEveryFieldWithItsBasis) {
  const fs::path dir = fs::temp_directory_path() / "atx_w0d0_basis_tags";
  const SourceModel base{};
  const HistoryPanel p = build(dir, base, base, kDates);
  ASSERT_EQ(p.field_basis.size(), p.panel.num_fields());
  for (atx::usize f = 0; f < p.panel.num_fields(); ++f) {
    const std::string_view name = p.panel.field_name(static_cast<alpha::FieldId>(f));
    const auto expect = history_field_level_basis(name);
    ASSERT_TRUE(expect.has_value()) << name;
    EXPECT_EQ(p.field_basis[f], *expect) << name;
  }
  const auto basis_of = [&](std::string_view name) {
    return p.field_basis[p.panel.field_id(name).value()];
  };
  for (const auto name : {"close", "open", "high", "low"}) {
    EXPECT_EQ(basis_of(name), LevelBasis::AdjustedLevel) << name;
  }
  for (const auto name : {"raw_close", "volume", "market_cap", "sector", "earnFlag"}) {
    EXPECT_EQ(basis_of(name), LevelBasis::Raw) << name;
  }
  EXPECT_EQ(basis_of("atmCenI_21d"), LevelBasis::Ratio);

  // Every Alpha101-derived field is tagged as well; dollar_volume/adv are Raw.
  auto aug = alpha::with_alpha101_fields(p.panel, kAdvWindows);
  ASSERT_TRUE(aug.has_value());
  for (atx::usize f = 0; f < aug->num_fields(); ++f) {
    const std::string_view name = aug->field_name(static_cast<alpha::FieldId>(f));
    EXPECT_TRUE(history_field_level_basis(name).has_value()) << "untagged field " << name;
  }
  EXPECT_EQ(history_field_level_basis("dollar_volume"), LevelBasis::Raw);
  EXPECT_EQ(history_field_level_basis("adv20"), LevelBasis::Raw);
  EXPECT_EQ(history_field_level_basis("vwap"), LevelBasis::AdjustedLevel);
  EXPECT_EQ(history_field_level_basis("returns"), LevelBasis::Ratio);
  EXPECT_EQ(history_field_level_basis("regime_vix"), LevelBasis::Raw);
  EXPECT_FALSE(history_field_level_basis("no_such_field").has_value());
  EXPECT_FALSE(history_field_level_basis("adv").has_value());
  EXPECT_EQ(atx::engine::data::level_basis_name(LevelBasis::AdjustedLevel), "adjusted_level");
  fs::remove_all(dir);
}

// Fix pass 1: the liquidity tag follows the DollarVolumeBasis the panel was
// augmented under. CloseV1 liquidity is close x volume on the snapshot-factor close,
// so it is AdjustedLevel — measured: it moves under a factor re-snapshot.
TEST(DataLevelBasis_W0d0, LiquidityTagFollowsTheDollarVolumeBasis) {
  using alpha::DollarVolumeBasis;
  const fs::path dir = fs::temp_directory_path() / "atx_w0d0_basis_dvb";
  const SourceModel base{};
  const HistoryPanel p = build(dir, base, base, kDates);
  auto v2 = alpha::with_alpha101_fields(p.panel, kAdvWindows);
  ASSERT_TRUE(v2.has_value());
  atx::usize liquidity_fields = 0;
  for (atx::usize f = 0; f < v2->num_fields(); ++f) {
    const std::string_view name = v2->field_name(static_cast<alpha::FieldId>(f));
    const auto by_name = history_field_level_basis(name);
    ASSERT_TRUE(by_name.has_value()) << name;
    // RawCloseV2 is exactly the name-only tag for every field.
    EXPECT_EQ(history_field_level_basis(name, DollarVolumeBasis::RawCloseV2), by_name) << name;
    atx::u16 w = 0;
    const bool liquidity = name == "dollar_volume" || alpha::datafields::parse_adv_field(name, w);
    liquidity_fields += liquidity ? 1U : 0U;
    // CloseV1 retags only the derived liquidity; every other field keeps its tag.
    EXPECT_EQ(history_field_level_basis(name, DollarVolumeBasis::CloseV1),
              liquidity ? std::optional<LevelBasis>{LevelBasis::AdjustedLevel} : by_name)
        << name;
  }
  EXPECT_EQ(liquidity_fields, 1U + kAdvWindows.size());
  // An unknown enum value fails closed (AdjustedLevel), unknown names stay nullopt.
  const auto bogus = static_cast<DollarVolumeBasis>(0);
  EXPECT_EQ(history_field_level_basis("adv20", bogus), LevelBasis::AdjustedLevel);
  EXPECT_EQ(history_field_level_basis("raw_close", bogus), LevelBasis::Raw);
  EXPECT_FALSE(history_field_level_basis("no_such_field", DollarVolumeBasis::CloseV1).has_value());
  EXPECT_FALSE(history_field_level_basis("adv", DollarVolumeBasis::CloseV1).has_value());

  // with_datafields called directly on a history panel (atx-impl stage_discover's
  // capacity screen) derives close x volume: the same values as CloseV1.
  auto v1 = alpha::with_alpha101_fields(p.panel, kAdvWindows, DollarVolumeBasis::CloseV1);
  ASSERT_TRUE(v1.has_value());
  const atx::usize nf = p.panel.num_fields();
  std::vector<std::string> names;
  std::vector<std::vector<atx::f64>> data;
  for (atx::usize f = 0; f < nf; ++f) {
    names.emplace_back(p.panel.field_name(static_cast<alpha::FieldId>(f)));
    const auto col = p.panel.field_all(static_cast<alpha::FieldId>(f));
    data.emplace_back(col.begin(), col.end());
  }
  std::vector<std::uint8_t> uni(kDates * kInsts, 0);
  for (atx::usize d = 0; d < kDates; ++d) {
    for (atx::usize i = 0; i < kInsts; ++i) {
      uni[d * kInsts + i] = p.panel.in_universe(d, i) ? std::uint8_t{1} : std::uint8_t{0};
    }
  }
  auto direct = alpha::datafields::with_datafields(kDates, kInsts, std::move(names),
                                                    std::move(data), std::move(uni), kAdvWindows);
  ASSERT_TRUE(direct.has_value());
  for (const auto name : {"dollar_volume", "adv2", "adv5"}) {
    const auto x = field(*direct, name);
    const auto y = field(*v1, name);
    ASSERT_EQ(x.size(), y.size());
    for (atx::usize k = 0; k < x.size(); ++k) {
      EXPECT_TRUE(same_bits(x[k], y[k])) << name << " cell " << k;
    }
  }
  fs::remove_all(dir);
}

// dollar_volume = raw_close x volume and adv{d} = its causal mean, bit-for-bit;
// the field order is unchanged from the legacy rule.
TEST(DataLevelBasis_W0d0, DollarVolumeAndAdvAreBuiltFromRawClose) {
  const fs::path dir = fs::temp_directory_path() / "atx_w0d0_dvol_raw";
  const SourceModel base{};
  const HistoryPanel p = build(dir, base, base, kDates);
  auto v2 = alpha::with_alpha101_fields(p.panel, kAdvWindows);
  auto v1 = alpha::with_alpha101_fields(p.panel, kAdvWindows, alpha::DollarVolumeBasis::CloseV1);
  ASSERT_TRUE(v2.has_value() && v1.has_value());
  ASSERT_EQ(v2->num_fields(), v1->num_fields());
  for (atx::usize f = 0; f < v2->num_fields(); ++f) {
    EXPECT_EQ(v2->field_name(static_cast<alpha::FieldId>(f)),
              v1->field_name(static_cast<alpha::FieldId>(f)));
  }
  const auto raw = field(*v2, "raw_close");
  const auto close = field(*v2, "close");
  const auto vol = field(*v2, "volume");
  const auto dv2 = field(*v2, "dollar_volume");
  const auto dv1 = field(*v1, "dollar_volume");
  const auto adv5 = field(*v2, "adv5");
  const std::vector<atx::f64> expect_adv5 = alpha::datafields::detail::rolling_mean(
      std::vector<atx::f64>(dv2.begin(), dv2.end()), kDates, kInsts, 5);
  atx::usize checked = 0;
  atx::usize differs_from_v1 = 0;
  for (atx::usize d = 0; d < kDates; ++d) {
    for (atx::usize i = 0; i < kInsts; ++i) {
      const atx::usize k = d * kInsts + i;
      EXPECT_TRUE(same_bits(adv5[k], expect_adv5[k])) << "adv5 cell " << k;
      if (!v2->in_universe(d, i)) {
        EXPECT_TRUE(std::isnan(dv2[k]));
        continue;
      }
      ++checked;
      EXPECT_TRUE(same_bits(dv2[k], raw[k] * vol[k])) << "dollar_volume cell " << k;
      EXPECT_TRUE(same_bits(dv1[k], close[k] * vol[k])) << "legacy dollar_volume cell " << k;
      differs_from_v1 += same_bits(dv1[k], dv2[k]) ? 0U : 1U;
    }
  }
  EXPECT_GT(checked, 0U);
  // Instrument 1 (factor 0.5 before its split) and 3 (0.98 before a dividend) differ.
  EXPECT_GT(differs_from_v1, 0U);
  // vwap stays the typical price on the close basis (same under both rules).
  const auto vw2 = field(*v2, "vwap");
  const auto vw1 = field(*v1, "vwap");
  for (atx::usize k = 0; k < vw2.size(); ++k) {
    EXPECT_TRUE(same_bits(vw2[k], vw1[k]));
  }
  // A panel with no raw_close keeps close x volume under the default rule.
  auto no_raw = alpha::Panel::create(2, 1, {"close", "volume", "high", "low"},
                                     {{10.0, 11.0}, {5.0, 6.0}, {10.5, 11.5}, {9.5, 10.5}}, {});
  ASSERT_TRUE(no_raw.has_value());
  auto no_raw_aug = alpha::with_alpha101_fields(*no_raw, kAdvWindows);
  ASSERT_TRUE(no_raw_aug.has_value());
  EXPECT_DOUBLE_EQ(field(*no_raw_aug, "dollar_volume")[1], 66.0);
  fs::remove_all(dir);
}

// D-01 look-ahead, measured: a vendor re-snapshot after a later 2:1 split halves
// every stored backward factor. Raw liquidity must not move; the legacy rule
// halves it (the adjusted close carries the future split into the level).
TEST(DataLevelBasis_W0d0, FactorResnapshotLeavesRawLiquidityInvariant) {
  const fs::path dir_a = fs::temp_directory_path() / "atx_w0d0_resnap_a";
  const fs::path dir_b = fs::temp_directory_path() / "atx_w0d0_resnap_b";
  const SourceModel base{};
  SourceModel resnap = base;
  resnap.factor_scale = 0.5; // exact in binary: every product scales exactly
  const HistoryPanel a = build(dir_a, base, base, kDates);
  const HistoryPanel b = build(dir_b, resnap, resnap, kDates);

  auto a2 = alpha::with_alpha101_fields(a.panel, kAdvWindows);
  auto b2 = alpha::with_alpha101_fields(b.panel, kAdvWindows);
  auto a1 = alpha::with_alpha101_fields(a.panel, kAdvWindows, alpha::DollarVolumeBasis::CloseV1);
  auto b1 = alpha::with_alpha101_fields(b.panel, kAdvWindows, alpha::DollarVolumeBasis::CloseV1);
  ASSERT_TRUE(a2 && b2 && a1 && b1);

  // Raw-basis and ratio fields are invariant, bit for bit.
  for (const auto name : {"raw_close", "volume", "market_cap", "dollar_volume", "adv2", "adv5",
                          "returns", "cap"}) {
    const auto x = field(*a2, name);
    const auto y = field(*b2, name);
    for (atx::usize k = 0; k < x.size(); ++k) {
      EXPECT_TRUE(same_bits(x[k], y[k])) << name << " moved under a re-snapshot, cell " << k;
    }
  }
  for (atx::usize d = 0; d < kDates; ++d) {
    for (atx::usize i = 0; i < kInsts; ++i) {
      EXPECT_EQ(a2->in_universe(d, i), b2->in_universe(d, i));
    }
  }
  // The legacy rule's dollar_volume halves: measured ratio exactly 0.5.
  const auto dv_a = field(*a1, "dollar_volume");
  const auto dv_b = field(*b1, "dollar_volume");
  atx::usize halved = 0;
  atx::usize finite = 0;
  for (atx::usize k = 0; k < dv_a.size(); ++k) {
    if (std::isfinite(dv_a[k]) && std::isfinite(dv_b[k])) {
      ++finite;
      halved += (dv_b[k] / dv_a[k] == 0.5) ? 1U : 0U;
    }
  }
  EXPECT_GT(finite, 0U);
  EXPECT_EQ(halved, finite) << "legacy dollar_volume should scale with the snapshot factor";
  std::printf("[resnapshot] CloseV1 dollar_volume cells halved=%zu/%zu; RawCloseV2 moved=0\n",
              static_cast<std::size_t>(halved), static_cast<std::size_t>(finite));
  fs::remove_all(dir_a);
  fs::remove_all(dir_b);
}

} // namespace atx_test_w0_d0_future_perturb
