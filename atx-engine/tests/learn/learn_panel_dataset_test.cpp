#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <limits>
#include <numeric>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "atx/engine/data/panel_store.hpp"
#include "atx/engine/learn/feature_matrix.hpp"
#include "atx/engine/learn/linear_alpha.hpp"
#include "atx/engine/learn/panel_dataset.hpp"

namespace {
using namespace atx;
namespace learn = atx::engine::learn;
namespace data = atx::engine::data;
namespace alpha = atx::engine::alpha;
namespace combine = atx::engine::combine;
namespace fs = std::filesystem;
constexpr i64 kStart = 1'357'084'800'000'000'000LL;
constexpr i64 kDay = 86'400'000'000'000LL;
constexpr u64 kBudget = 512ULL * 1024 * 1024;
constexpr f64 kNaN = std::numeric_limits<f64>::quiet_NaN();

struct SyntheticSource final : learn::PanelDatasetSource {
  usize dates, names, features;
  std::vector<f64> raw, close;
  std::vector<u8> present, member;
  std::vector<i64> clocks;
  usize fail_at{std::numeric_limits<usize>::max()};
  explicit SyntheticSource(usize t = 32, usize n = 10, usize f = 5)
      : dates(t), names(n), features(f), raw(t * f * n), close(t * n), present(t * n, 1), member(t * n, 1), clocks(t) {
    for (usize d = 0; d < t; ++d) {
      clocks[d] = kStart + static_cast<i64>(d) * kDay - 1;
      for (usize i = 0; i < n; ++i) {
        close[d * n + i] = 100 + static_cast<f64>(i) + .2 * static_cast<f64>(d) +
            2 * std::sin(static_cast<f64>(d) * .7 + static_cast<f64>(i) * .3);
        for (usize j = 0; j < f; ++j) raw[(d * f + j) * n + i] = (i + j) % 5 < 2 ? kNaN :
            static_cast<f64>(i % 4) + .1 * static_cast<f64>(d + j);
      }
    }
    if (t > 6 && n > 3) { present[5 * n + 2] = 0; close[5 * n + 2] = kNaN; member[6 * n + 3] = 0; }
  }
  core::Status read_features(usize d, std::span<f64> out, std::span<u8> p, std::span<u8> m, i64& clock) override {
    if (d == fail_at) return core::Err(core::ErrorCode::Unavailable, "synthetic source failure");
    std::copy_n(raw.begin() + static_cast<std::ptrdiff_t>(d * features * names), out.size(), out.begin());
    std::copy_n(present.begin() + static_cast<std::ptrdiff_t>(d * names), names, p.begin());
    std::copy_n(member.begin() + static_cast<std::ptrdiff_t>(d * names), names, m.begin()); clock = clocks[d]; return core::Ok();
  }
  core::Status read_close(usize d, std::span<f64> out) override {
    if (d >= dates) return core::Err(core::ErrorCode::InvalidArgument, "synthetic close bound");
    std::copy_n(close.begin() + static_cast<std::ptrdiff_t>(d * names), names, out.begin()); return core::Ok();
  }
  learn::PanelDatasetConfig config(usize count = 0) const {
    learn::PanelDatasetConfig c; if (!count) count = dates;
    for (usize d = 0; d < count; ++d) c.session_keys.push_back(kStart + static_cast<i64>(d) * kDay);
    for (usize i = 0; i < names; ++i) c.instrument_ids.push_back(static_cast<i64>(100 + i));
    for (usize f = 0; f < features; ++f) c.feature_names.push_back("feature_" + std::to_string(f));
    c.instrument_namespace = "synthetic-security-id"; c.source_sha256.assign(64, 'a');
    c.source_recipe = "synthetic;original-f64-close;independent-verified-prior-membership;not-market-evidence";
    c.holding_horizons = {1, 2, 4}; c.execution_delay = 1; c.volatility_window = 3; c.volatility_min_observations = 2;
    return c;
  }
};

class LearnPanelDataset : public ::testing::Test {
protected:
  fs::path root;
  void SetUp() override {
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    for (int i = 0; i < 32; ++i) {
      const auto candidate = fs::temp_directory_path() / ("atx-l1-dataset-" + std::to_string(stamp) + "-" + std::to_string(i));
      std::error_code ec; if (fs::create_directory(candidate, ec)) { root = candidate; return; }
    }
    FAIL() << "exclusive fixture directory unavailable";
  }
  void TearDown() override { if (!root.empty()) { std::error_code ec; fs::remove_all(root, ec); } }
};

TEST_F(LearnPanelDataset, FortyPercentMissingRetainsMembersAndLabelsMatchIndependentDateOracle) {
  SyntheticSource source;
  const auto built = learn::build_panel_dataset(source, source.config(), (root / "dataset").string());
  ASSERT_TRUE(built) << built.error().message();
  EXPECT_EQ(built->member_rows, source.dates * source.names - 1);
  const auto dataset = learn::PanelDataset::open((root / "dataset").string(), built->manifest_sha256);
  ASSERT_TRUE(dataset);
  const auto fm = learn::read_dataset_features(*dataset, 0, source.dates, source.dates - 1, kBudget);
  ASSERT_TRUE(fm) << fm.error().message();
  EXPECT_EQ(fm->n_rows(), built->member_rows); EXPECT_EQ(fm->n_features, source.features * 2);
  EXPECT_EQ(fm->label_horizons, (std::vector<u16>{2, 3, 5}));
  EXPECT_TRUE(std::all_of(fm->row_valid.begin(), fm->row_valid.end(), [](u8 v) { return v == 1; }));
  const auto absent = fm->row_of(5, 2); EXPECT_EQ(fm->row_present[absent], 0);
  for (usize f = 0; f < source.features; ++f) {
    EXPECT_EQ(fm->X[absent * fm->n_features + f], 0);
    EXPECT_EQ(fm->X[absent * fm->n_features + source.features + f], 1);
  }
  // At date0/feature0 the finite member IDs are2,3,4,7,8,9: values2,3,0,3,0,1.
  const std::array<f64, 10> ranks{0, 0, .1, .4, -.4, 0, 0, .4, -.4, -.1};
  for (usize i = 0; i < source.names; ++i) EXPECT_NEAR(fm->X[fm->row_of(0, i) * fm->n_features], ranks[i], 3e-8);
  constexpr usize date = 7, horizon = 1;
  std::vector<f64> independent;
  for (usize i = 0; i < source.names; ++i) {
    std::vector<f64> returns;
    for (usize d = date - 3; d < date; ++d) {
      const auto a = source.close[(d - 1) * source.names + i], b = source.close[d * source.names + i];
      if (std::isfinite(a) && std::isfinite(b)) returns.push_back(b / a - 1);
    }
    if (returns.size() < 2) { independent.push_back(kNaN); continue; }
    const auto mean = std::accumulate(returns.begin(), returns.end(), 0.0) / static_cast<f64>(returns.size());
    f64 squares = 0; for (auto value : returns) squares += (value - mean) * (value - mean);
    const auto sd = std::max(source.config().daily_volatility_floor, std::sqrt(squares / static_cast<f64>(returns.size() - 1)));
    independent.push_back((source.close[(date + 1 + horizon) * source.names + i] / source.close[(date + 1) * source.names + i] - 1) / sd);
  }
  f64 mean = 0; usize count = 0; for (auto value : independent) if (std::isfinite(value)) { mean += value; ++count; }
  mean /= static_cast<f64>(count);
  f64 actual_mean = 0;
  for (usize i = 0; i < source.names; ++i) {
    const auto actual = fm->Y[0][fm->row_of(date, i)];
    if (std::isfinite(independent[i])) { EXPECT_NEAR(actual, independent[i] - mean, 1e-11); actual_mean += actual; }
    else EXPECT_TRUE(std::isnan(actual));
  }
  EXPECT_NEAR(actual_mean / static_cast<f64>(count), 0, 1e-12);
  EXPECT_TRUE(std::isnan(fm->Y[0][fm->row_of(2, 0)])); // insufficient strictly-prior returns
  EXPECT_GT(built->finite_labels[0], 0U);
}

TEST_F(LearnPanelDataset, FutureMutationTruncationAndExactEndpointMaturity) {
  SyntheticSource full, changed = full;
  for (usize d = 18; d < changed.dates; ++d) {
    for (usize i = 0; i < changed.names; ++i) changed.close[d * changed.names + i] *= 2 + .1 * static_cast<f64>(i);
    for (usize f = 0; f < changed.features; ++f) for (usize i = 0; i < changed.names; ++i)
      changed.raw[(d * changed.features + f) * changed.names + i] = -static_cast<f64>(i);
  }
  ASSERT_TRUE(learn::build_panel_dataset(full, full.config(), (root / "full").string()));
  ASSERT_TRUE(learn::build_panel_dataset(changed, changed.config(), (root / "changed").string()));
  ASSERT_TRUE(learn::build_panel_dataset(full, full.config(18), (root / "truncated").string()));
  const auto a = learn::PanelDataset::open((root / "full").string());
  const auto b = learn::PanelDataset::open((root / "changed").string());
  const auto c = learn::PanelDataset::open((root / "truncated").string());
  ASSERT_TRUE(a); ASSERT_TRUE(b); ASSERT_TRUE(c);
  const auto fa = learn::read_dataset_features(*a, 0, 18, 17, kBudget);
  const auto fb = learn::read_dataset_features(*b, 0, 18, 17, kBudget);
  const auto fc = learn::read_dataset_features(*c, 0, 18, 17, kBudget);
  ASSERT_TRUE(fa); ASSERT_TRUE(fb); ASSERT_TRUE(fc);
  EXPECT_EQ(fa->X, fb->X); EXPECT_EQ(fa->X, fc->X);
  for (usize h = 0; h < fa->Y.size(); ++h) for (usize r = 0; r < fa->n_rows(); ++r) {
    EXPECT_EQ(std::bit_cast<u64>(fa->Y[h][r]), std::bit_cast<u64>(fb->Y[h][r]));
    EXPECT_EQ(std::bit_cast<u64>(fa->Y[h][r]), std::bit_cast<u64>(fc->Y[h][r]));
  }
  const auto before = learn::read_dataset_features(*a, 7, 8, 8, kBudget);
  const auto at = learn::read_dataset_features(*a, 7, 8, 9, kBudget);
  ASSERT_TRUE(before); ASSERT_TRUE(at);
  EXPECT_TRUE(std::isnan(before->Y[0][0])); EXPECT_TRUE(std::isfinite(at->Y[0][0]));
  EXPECT_FALSE(learn::read_dataset_features(*a, 0, 18, 16, kBudget)); // future feature rows cannot enter a fit
}

TEST_F(LearnPanelDataset, MappingLifetimeBudgetsChangedExtentAndPublishLastFailure) {
  SyntheticSource source;
  const auto built = learn::build_panel_dataset(source, source.config(), (root / "ok").string());
  ASSERT_TRUE(built);
  const auto dataset = learn::PanelDataset::open((root / "ok").string(), built->manifest_sha256, 1024 * 1024, 1);
  ASSERT_TRUE(dataset);
  {
    const auto block = dataset->open_block(0); ASSERT_TRUE(block);
    EXPECT_FALSE(dataset->open_block(1));
    EXPECT_TRUE(block->feature(0)); EXPECT_FALSE(block->feature(10));
  }
  EXPECT_TRUE(dataset->open_block(1));
  const auto orphan = [&]() -> core::Result<learn::PanelDatasetBlock> {
    ATX_TRY(auto reopened, learn::PanelDataset::open((root / "ok").string())); return reopened.open_block(0);
  }();
  ASSERT_TRUE(orphan); EXPECT_EQ(orphan->member().size(), 80U);
  EXPECT_FALSE(learn::PanelDataset::open((root / "ok").string(), std::string(64, 'b')));
  EXPECT_FALSE(learn::read_dataset_features(*dataset, 0, 8, 10, 1));
  { std::ofstream changed(root / "ok" / "block-1.bin", std::ios::binary | std::ios::app); changed.put('x'); }
  EXPECT_FALSE(dataset->open_block(1)); // captured handle extent must be admitted before mapping
  source.fail_at = 5;
  EXPECT_FALSE(learn::build_panel_dataset(source, source.config(), (root / "failed").string()));
  EXPECT_FALSE(fs::exists(root / "failed" / "manifest.bin"));
  EXPECT_FALSE(learn::PanelDataset::open((root / "failed").string()));
  source.fail_at = std::numeric_limits<usize>::max(); source.clocks[0] = kStart;
  EXPECT_FALSE(learn::build_panel_dataset(source, source.config(), (root / "clock").string()));
}

TEST_F(LearnPanelDataset, PanelAndD6ProducersPreserveExplicitLegacyAndIndependentMasks) {
  SyntheticSource source;
  auto cfg = source.config(); std::vector<std::string> names{"close"};
  names.insert(names.end(), cfg.feature_names.begin(), cfg.feature_names.end());
  std::vector<std::vector<f64>> values(names.size()); values[0] = source.close;
  values[0][5 * source.names + 2] = 777; // absent but finite backing cell must never become a label/volatility price
  for (usize f = 0; f < source.features; ++f) for (usize d = 0; d < source.dates; ++d)
    values[f + 1].insert(values[f + 1].end(), source.raw.begin() + static_cast<std::ptrdiff_t>((d * source.features + f) * source.names),
        source.raw.begin() + static_cast<std::ptrdiff_t>((d * source.features + f + 1) * source.names));
  const auto panel = alpha::Panel::create(source.dates, source.names, names, values, source.present);
  ASSERT_TRUE(panel);
  const combine::AlphaStore pool;
  learn::FeatureSpec spec; spec.raw_fields = cfg.feature_names; spec.horizons = cfg.holding_horizons;
  const auto legacy = learn::build_features(*panel, pool, spec); ASSERT_TRUE(legacy);
  EXPECT_EQ(legacy->label_horizons, spec.horizons);
  EXPECT_EQ(std::count(legacy->row_valid.begin(), legacy->row_valid.end(), u8{1}), 0);
  ASSERT_TRUE(learn::build_panel_dataset_from_panel(*panel, pool, spec, cfg, source.member, source.clocks, (root / "panel").string()));
  data::PanelStoreConfig sc; sc.session_keys = cfg.session_keys; sc.instrument_ids = cfg.instrument_ids;
  sc.original_indices.resize(source.names); std::iota(sc.original_indices.begin(), sc.original_indices.end(), u64{0});
  sc.instrument_namespace = cfg.instrument_namespace; sc.recipe = "synthetic D6 parent"; sc.membership_sha256.assign(64, 'b');
  sc.parents = {{"synthetic", std::string(64, 'a')}}; sc.chunk_dates = 8;
  for (usize f = 0; f < names.size(); ++f) sc.fields.push_back({names[f], static_cast<data::LevelBasis>(f == 0 ? 2 : 1), data::PanelStorePrecision::Float32V2});
  auto writer = data::PanelStoreWriter::create((root / "store").string(), sc); ASSERT_TRUE(writer);
  std::vector<std::span<const f64>> rows;
  for (usize d = 0; d < source.dates; ++d) {
    rows.clear(); for (const auto& field : values) rows.push_back(std::span<const f64>(field).subspan(d * source.names, source.names));
    ASSERT_TRUE(writer->append_date(d, rows, rows[0], std::span<const u8>(source.present).subspan(d * source.names, source.names),
        std::span<const u8>(source.member).subspan(d * source.names, source.names), source.clocks[d]));
  }
  const auto sha = writer->finish(); ASSERT_TRUE(sha);
  const auto store = data::PanelStore::open((root / "store").string(), *sha); ASSERT_TRUE(store);
  cfg.source_sha256 = *sha;
  ASSERT_TRUE(learn::build_panel_dataset_from_store(*store, cfg, (root / "from-store").string()));
  const auto a = learn::PanelDataset::open((root / "panel").string());
  const auto b = learn::PanelDataset::open((root / "from-store").string());
  ASSERT_TRUE(a); ASSERT_TRUE(b);
  const auto fa = learn::read_dataset_features(*a, 0, 20, 23, kBudget);
  const auto fb = learn::read_dataset_features(*b, 0, 20, 23, kBudget);
  ASSERT_TRUE(fa); ASSERT_TRUE(fb);
  EXPECT_EQ(fa->row_present, fb->row_present); EXPECT_EQ(fa->row_date, fb->row_date); EXPECT_EQ(fa->X, fb->X);
  for (usize h = 0; h < fa->Y.size(); ++h) for (usize r = 0; r < fa->n_rows(); ++r)
    EXPECT_EQ(std::bit_cast<u64>(fa->Y[h][r]), std::bit_cast<u64>(fb->Y[h][r]));
}

TEST_F(LearnPanelDataset, MetadataScaleAdmissionAndBoundedCheckedLinearConsumer) {
  SyntheticSource source;
  auto big = source.config(); big.session_keys.resize(3000); big.instrument_ids.resize(1750); big.feature_names.resize(500);
  for (usize d = 0; d < big.session_keys.size(); ++d) big.session_keys[d] = 1'167'609'600'000'000'000LL + static_cast<i64>(d) * kDay;
  for (usize i = 0; i < big.instrument_ids.size(); ++i) big.instrument_ids[i] = static_cast<i64>(100 + i);
  for (usize f = 0; f < big.feature_names.size(); ++f) big.feature_names[f] = "f" + std::to_string(f);
  const auto size = learn::preflight_panel_dataset(big); ASSERT_TRUE(size);
  EXPECT_LT(size->working_bytes, 512ULL * 1024 * 1024); // formula admission ONLY; not a measured RSS run
  big.max_working_bytes = 1; EXPECT_FALSE(learn::preflight_panel_dataset(big));
  ASSERT_TRUE(learn::build_panel_dataset(source, source.config(), (root / "dataset").string()));
  const auto dataset = learn::PanelDataset::open((root / "dataset").string()); ASSERT_TRUE(dataset);
  learn::LinearAlphaCfg cfg{}; cfg.en = {.02, .5, 20, 1e-6}; cfg.use_ridge_baseline = true; cfg.master_seed = 17;
  cfg.horizons = {2, 3, 5}; cfg.cpcv.n_groups = 3; cfg.cpcv.n_test_groups = 1;
  const learn::LatentAugmentation aug;
  EXPECT_FALSE(learn::fit_linear_dataset(*dataset, 0, 24, 31, kBudget, aug, cfg)); // explicit V1 refused on new bridge
  cfg.cpcv.rule = atx::engine::eval::CpcvRule::DateV2;
  const auto model = learn::fit_linear_dataset(*dataset, 0, 24, 31, kBudget, aug, cfg);
  ASSERT_TRUE(model) << model.error().message();
  EXPECT_EQ(model->dataset_manifest_sha256, dataset->manifest_sha256());
  EXPECT_EQ(model->model.n_base_features, 10U); EXPECT_EQ(model->model.horizons, cfg.horizons);
  cfg.horizons = {1, 2, 4}; EXPECT_FALSE(learn::fit_linear_dataset(*dataset, 0, 24, 31, kBudget, aug, cfg));
  cfg.horizons = {2, 3, 5}; cfg.cpcv.max_working_bytes = 1;
  EXPECT_FALSE(learn::fit_linear_dataset(*dataset, 0, 24, 31, kBudget, aug, cfg));
}
} // namespace
