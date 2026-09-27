#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <limits>
#include <numeric>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "atx/engine/learn/gbt.hpp"
#include "atx/engine/learn/panel_dataset.hpp"

namespace {
using namespace atx;
namespace learn = atx::engine::learn;
namespace eval = atx::engine::eval;
namespace lin = atx::core::linalg;
namespace fs = std::filesystem;
constexpr f64 kNaN = std::numeric_limits<f64>::quiet_NaN();

learn::GbtCfg config() {
  learn::GbtCfg cfg;
  cfg.rule = learn::GbtRule::ColumnBinsV2;
  cfg.n_trees = 3;
  cfg.max_depth = 2;
  cfg.n_bins = 8;
  cfg.learning_rate = .5;
  cfg.l2 = 0;
  cfg.min_split_gain = 0;
  cfg.row_subsample = cfg.feature_subsample = 1;
  cfg.demean_loss_by_date = false;
  cfg.master_seed = 29;
  return cfg;
}

// Compare explicitly serialized primitive values, never indeterminate struct padding.
std::vector<u64> forest_bits(const learn::GbtForest &forest) {
  std::vector<u64> result{std::bit_cast<u64>(forest.base), forest.trees.size()};
  for (const auto &tree : forest.trees) {
    result.push_back(tree.nodes.size());
    for (const auto &node : tree.nodes) {
      result.push_back(node.feature);
      result.push_back(std::bit_cast<u64>(node.threshold));
      result.push_back(std::bit_cast<u64>(node.leaf_value));
      result.push_back(static_cast<u64>(node.left));
      result.push_back(static_cast<u64>(node.right));
      result.push_back(node.is_leaf);
    }
  }
  return result;
}

TEST(GbtColumnV2, MissingRightAndFirstFeatureTieMatchIndependentSquaredErrorStump) {
  lin::MatX x(8, 2);
  lin::VecX y(8);
  const std::array<f64, 8> values{0, 0, 1, 1, 2, 2, kNaN, kNaN};
  for (Eigen::Index i = 0; i < 8; ++i) {
    x(i, 0) = x(i, 1) = values[static_cast<usize>(i)];
    y(i) = i < 4 ? 0 : 4;
  }
  f64 best_loss = std::numeric_limits<f64>::infinity(), threshold = 0;
  usize best_feature = 0;
  // Independent direct residual-square enumeration; NaN naturally takes right.
  for (usize feature = 0; feature < 2; ++feature)
    for (f64 cut : {0., 1., 2.}) {
      std::array<f64, 2> sum{};
      std::array<usize, 2> count{};
      for (Eigen::Index row = 0; row < 8; ++row) {
        const usize side = x(row, static_cast<Eigen::Index>(feature)) < cut ? 0 : 1;
        sum[side] += y(row);
        ++count[side];
      }
      if (!count[0] || !count[1])
        continue;
      f64 loss = 0;
      for (Eigen::Index row = 0; row < 8; ++row) {
        const usize side = x(row, static_cast<Eigen::Index>(feature)) < cut ? 0 : 1;
        const auto residual = y(row) - sum[side] / static_cast<f64>(count[side]);
        loss += residual * residual;
      }
      if (loss < best_loss) {
        best_loss = loss;
        threshold = cut;
        best_feature = feature;
      }
    }
  auto cfg = config();
  cfg.n_trees = 1;
  cfg.max_depth = 1;
  cfg.learning_rate = 1;
  learn::GbtFitDiagnostics stats;
  const auto fitted = learn::fit_gbt_forest_checked(x, y, cfg, 7, {}, &stats);
  ASSERT_TRUE(fitted) << fitted.error().message();
  ASSERT_EQ(fitted->trees.size(), 1U);
  ASSERT_EQ(fitted->trees[0].nodes.size(), 3U);
  EXPECT_EQ(fitted->trees[0].nodes[0].feature, best_feature);
  EXPECT_DOUBLE_EQ(fitted->trees[0].nodes[0].threshold, threshold);
  EXPECT_EQ(best_feature, 0U);
  EXPECT_DOUBLE_EQ(threshold, 2);
  EXPECT_DOUBLE_EQ(best_loss, 0);
  for (usize i = 0; i < values.size(); ++i) {
    const std::array<f64, 2> row{values[i], values[i]};
    EXPECT_DOUBLE_EQ(learn::gbt_forest_predict(*fitted, row), y(static_cast<Eigen::Index>(i)));
  }
  EXPECT_EQ(stats.bin_bytes, 16U);
}

TEST(GbtColumnV2, WorkerCountsPreserveForestBitsAndImportanceWithSubtraction) {
  lin::MatX x(128, 3);
  lin::VecX y(128);
  for (Eigen::Index i = 0; i < x.rows(); ++i) {
    x(i, 0) = static_cast<f64>(i % 16);
    x(i, 1) = static_cast<f64>((i * 7) % 19);
    x(i, 2) = i % 13 == 0 ? kNaN : static_cast<f64>((i * 11) % 23);
    y(i) = (i % 16 < 8 ? -2. : 3.) + ((i * 7) % 19 < 10 ? 1. : -1.);
  }
  auto cfg = config();
  cfg.n_trees = 4;
  cfg.max_depth = 3;
  cfg.n_bins = 16;
  cfg.row_subsample = .85;
  cfg.feature_subsample = .8;
  learn::GbtFitDiagnostics one, four;
  const auto a = learn::fit_gbt_forest_checked(x, y, cfg, 17, {}, &one);
  cfg.workers = 4;
  const auto b = learn::fit_gbt_forest_checked(x, y, cfg, 17, {}, &four);
  ASSERT_TRUE(a) << a.error().message();
  ASSERT_TRUE(b) << b.error().message();
  EXPECT_EQ(forest_bits(*a), forest_bits(*b));
  EXPECT_EQ(one.gain_importance, four.gain_importance);
  EXPECT_EQ(one.histogram_rows, four.histogram_rows);
  EXPECT_EQ(one.histogram_subtractions, four.histogram_subtractions);
  EXPECT_GT(one.histogram_subtractions, 0U);
  EXPECT_EQ(one.bin_bytes, 128U * 3U);
  EXPECT_LT(one.bin_bytes, 128U * 3U * sizeof(usize));
  ASSERT_EQ(one.gain_importance.size(), 3U);
  for (auto gain : one.gain_importance)
    EXPECT_GE(gain, 0);
  EXPECT_NEAR(std::accumulate(one.gain_importance.begin(), one.gain_importance.end(), 0.), 1.,
              1e-14);
}

TEST(GbtColumnV2, DateCenteredLossIgnoresCommonDateOffsetsAndRequiresOriginalClock) {
  lin::MatX x(32, 1);
  lin::VecX y(32), shifted(32);
  std::vector<usize> dates(32);
  for (usize r = 0; r < dates.size(); ++r) {
    dates[r] = r / 4;
    x(static_cast<Eigen::Index>(r), 0) = static_cast<f64>(r % 4);
    y(static_cast<Eigen::Index>(r)) = r % 4 < 2 ? -1 : 1;
    shifted(static_cast<Eigen::Index>(r)) =
        y(static_cast<Eigen::Index>(r)) + 1024 * static_cast<f64>(1 + dates[r]);
  }
  auto cfg = config();
  cfg.demean_loss_by_date = true;
  const auto a = learn::fit_gbt_forest_checked(x, y, cfg, 3, dates);
  const auto b = learn::fit_gbt_forest_checked(x, shifted, cfg, 3, dates);
  ASSERT_TRUE(a);
  ASSERT_TRUE(b);
  EXPECT_EQ(forest_bits(*a), forest_bits(*b));
  EXPECT_DOUBLE_EQ(a->base, 0);
  EXPECT_FALSE(learn::fit_gbt_forest_checked(x, y, cfg, 3));
  dates[10] = 0;
  EXPECT_FALSE(learn::fit_gbt_forest_checked(x, y, cfg, 3, dates));
}

TEST(GbtColumnV2, InvalidDataAndResourceBoundsRefuseWithoutInventingImportance) {
  lin::MatX x = lin::MatX::Constant(8, 2, kNaN);
  lin::VecX y = lin::VecX::Ones(8);
  auto cfg = config();
  learn::GbtFitDiagnostics stats;
  const auto constant = learn::fit_gbt_forest_checked(x, y, cfg, 2, {}, &stats);
  ASSERT_TRUE(constant);
  EXPECT_DOUBLE_EQ(stats.split_gain_total, 0);
  EXPECT_EQ(stats.gain_importance, (std::vector<f64>{0, 0}));
  cfg.n_bins = 256;
  EXPECT_FALSE(learn::fit_gbt_forest_checked(x, y, cfg, 2));
  cfg = config();
  cfg.workers = 0;
  EXPECT_FALSE(learn::fit_gbt_forest_checked(x, y, cfg, 2));
  cfg = config();
  cfg.max_working_bytes = 1;
  EXPECT_FALSE(learn::fit_gbt_forest_checked(x, y, cfg, 2));
  cfg = config();
  x(0, 0) = std::numeric_limits<f64>::infinity();
  EXPECT_FALSE(learn::fit_gbt_forest_checked(x, y, cfg, 2));
  x(0, 0) = 0;
  y(0) = kNaN;
  EXPECT_FALSE(learn::fit_gbt_forest_checked(x, y, cfg, 2));
  EXPECT_FALSE(learn::fit_gbt_forest_checked(x, lin::VecX::Ones(7), cfg, 2));
}

TEST(GbtColumnV2, ExplicitLegacyDispatchMatchesFrozenPrimitivePathAndIgnoresV2Knobs) {
  lin::MatX x(32, 2);
  lin::VecX y(32);
  for (Eigen::Index r = 0; r < 32; ++r) {
    x(r, 0) = static_cast<f64>(r);
    x(r, 1) = static_cast<f64>(r % 7);
    y(r) = r < 16 ? -1 : 2;
  }
  auto cfg = config();
  cfg.rule = learn::GbtRule::LegacyV1;
  const auto edges = learn::gbt_detail::fit_bin_edges(x, cfg.n_bins);
  const auto expected = learn::gbt_detail::fit_forest(x, y, edges, cfg, 19);
  const auto a = learn::fit_gbt_forest_checked(x, y, cfg, 19);
  ASSERT_TRUE(a);
  cfg.workers = 0;
  cfg.max_working_bytes = 0;
  cfg.demean_loss_by_date = true;
  const auto b = learn::fit_gbt_forest_checked(x, y, cfg, 19);
  ASSERT_TRUE(b);
  EXPECT_EQ(forest_bits(*a), forest_bits(expected));
  EXPECT_EQ(forest_bits(*b), forest_bits(expected));
}

constexpr i64 kStart = 1'357'084'800'000'000'000LL;
constexpr i64 kDay = 86'400'000'000'000LL;
struct SmallSource final : learn::PanelDatasetSource {
  static constexpr usize dates = 24, names = 6, features = 2;
  std::vector<f64> close = std::vector<f64>(dates * names);
  SmallSource() {
    for (usize d = 0; d < dates; ++d)
      for (usize i = 0; i < names; ++i)
        close[d * names + i] = 100 + static_cast<f64>(i) + .2 * static_cast<f64>(d) +
                               2 * std::sin(static_cast<f64>(d) * .7 + static_cast<f64>(i) * .3);
  }
  core::Status read_features(usize d, std::span<f64> out, std::span<u8> present,
                             std::span<u8> member, i64 &clock) override {
    for (usize f = 0; f < features; ++f)
      for (usize i = 0; i < names; ++i)
        out[f * names + i] = (i + f + d) % 5 == 0 ? kNaN : static_cast<f64>((i + f + d) % 4);
    std::fill(present.begin(), present.end(), u8{1});
    std::fill(member.begin(), member.end(), u8{1});
    clock = kStart + static_cast<i64>(d) * kDay - 1;
    return core::Ok();
  }
  core::Status read_close(usize d, std::span<f64> out) override {
    std::copy_n(close.begin() + static_cast<std::ptrdiff_t>(d * names), names, out.begin());
    return core::Ok();
  }
  learn::PanelDatasetConfig data_config() const {
    learn::PanelDatasetConfig c;
    for (usize d = 0; d < dates; ++d)
      c.session_keys.push_back(kStart + static_cast<i64>(d) * kDay);
    for (usize i = 0; i < names; ++i)
      c.instrument_ids.push_back(static_cast<i64>(100 + i));
    c.feature_names = {"a", "b"};
    c.instrument_namespace = "synthetic-id";
    c.source_sha256.assign(64, 'a');
    c.source_recipe = "synthetic original-f64 close; prior membership; no market evidence";
    c.holding_horizons = {1};
    c.execution_delay = 1;
    c.volatility_window = 3;
    c.volatility_min_observations = 2;
    c.block_dates = 4;
    c.max_mapped_bytes = 1024 * 1024;
    return c;
  }
};
class GbtDatasetV2 : public ::testing::Test {
protected:
  fs::path root;
  void SetUp() override {
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    for (int i = 0; i < 32; ++i) {
      const auto path = fs::temp_directory_path() /
                        ("atx-l2-gbt-" + std::to_string(stamp) + "-" + std::to_string(i));
      std::error_code ec;
      if (fs::create_directory(path, ec)) {
        root = path;
        return;
      }
    }
    FAIL() << "exclusive fixture directory unavailable";
  }
  void TearDown() override {
    if (!root.empty()) {
      std::error_code ec;
      fs::remove_all(root, ec);
    }
  }
  learn::GbtCfg fit_config() const {
    auto cfg = config();
    cfg.n_trees = 2;
    cfg.horizons = {2};
    cfg.demean_loss_by_date = true;
    cfg.cpcv.rule = eval::CpcvRule::DateV2;
    cfg.cpcv.n_groups = 3;
    cfg.cpcv.n_test_groups = 1;
    cfg.cpcv.max_working_bytes = 4 * 1024 * 1024;
    return cfg;
  }
};

TEST_F(GbtDatasetV2, ActualConsumerBindsMaturityAndRefusesUnsupportedRecipesBeforeFit) {
  SmallSource source, changed = source;
  for (usize d = 19; d < SmallSource::dates; ++d)
    for (usize i = 0; i < SmallSource::names; ++i)
      changed.close[d * SmallSource::names + i] *= 2 + static_cast<f64>(i);
  ASSERT_TRUE(learn::build_panel_dataset(source, source.data_config(), (root / "a").string()));
  ASSERT_TRUE(learn::build_panel_dataset(changed, changed.data_config(), (root / "b").string()));
  const auto a = learn::PanelDataset::open((root / "a").string());
  const auto b = learn::PanelDataset::open((root / "b").string());
  ASSERT_TRUE(a);
  ASSERT_TRUE(b);
  auto cfg = fit_config();
  const learn::LatentAugmentation aug;
  constexpr u64 budget = 64 * 1024 * 1024;
  const auto fit = learn::fit_gbt_dataset(*a, 0, 16, 18, budget, aug, cfg);
  const auto future = learn::fit_gbt_dataset(*b, 0, 16, 18, budget, aug, cfg);
  ASSERT_TRUE(fit) << fit.error().message();
  ASSERT_TRUE(future) << future.error().message();
  EXPECT_EQ(fit->model.n_base_features, 4U);
  ASSERT_EQ(fit->model.forests.size(), 1U);
  EXPECT_EQ(fit->model.forests[0].trees.size(), 2U);
  EXPECT_EQ(forest_bits(fit->model.forests[0]), forest_bits(future->model.forests[0]));
  EXPECT_EQ(fit->dataset_manifest_sha256, a->manifest_sha256());
  EXPECT_NE(fit->dataset_manifest_sha256, future->dataset_manifest_sha256);
  EXPECT_FALSE(fit->dataset_window_recipe.empty());
  EXPECT_NE(fit->algorithm_recipe.find("missing255-fixed-right"), std::string::npos);
  EXPECT_NE(fit->algorithm_recipe.find("early-stop=none"), std::string::npos);
  EXPECT_GT(fit->diagnostics.forest_fits, 1U);
  EXPECT_GT(fit->diagnostics.bin_bytes, 0U);
  cfg.rule = learn::GbtRule::LegacyV1;
  EXPECT_FALSE(learn::fit_gbt_dataset(*a, 0, 16, 18, budget, aug, cfg));
  cfg = fit_config();
  cfg.cpcv.rule = eval::CpcvRule::ObservationV1;
  EXPECT_FALSE(learn::fit_gbt_dataset(*a, 0, 16, 18, budget, aug, cfg));
  cfg = fit_config();
  cfg.horizons = {1};
  EXPECT_FALSE(learn::fit_gbt_dataset(*a, 0, 16, 18, budget, aug, cfg));
  cfg = fit_config();
  cfg.max_working_bytes = 1;
  EXPECT_FALSE(learn::fit_gbt_dataset(*a, 0, 16, 18, budget, aug, cfg));
  cfg = fit_config();
  learn::LatentAugmentation future_pca;
  future_pca.pca.emplace();
  future_pca.pca->fit_upto_date = 19;
  EXPECT_FALSE(learn::fit_gbt_dataset(*a, 0, 16, 18, budget, future_pca, cfg));
}

TEST_F(GbtDatasetV2, FullFitIdentityBindsAugmentationButNotWorkerScheduling) {
  SmallSource source;
  ASSERT_TRUE(learn::build_panel_dataset(source, source.data_config(), (root / "data").string()));
  const auto dataset = learn::PanelDataset::open((root / "data").string());
  ASSERT_TRUE(dataset);
  auto cfg = fit_config();
  cfg.n_trees = 1;
  learn::LatentAugmentation aug;
  aug.interactions = {{0, 1}};
  aug.interactions_fixed = true;
  constexpr u64 budget = 64 * 1024 * 1024;
  const auto first = learn::fit_gbt_dataset(*dataset, 0, 16, 18, budget, aug, cfg);
  ASSERT_TRUE(first);
  cfg.workers = 2;
  const auto threaded = learn::fit_gbt_dataset(*dataset, 0, 16, 18, budget, aug, cfg);
  ASSERT_TRUE(threaded);
  EXPECT_EQ(first->algorithm_recipe, threaded->algorithm_recipe);
  EXPECT_EQ(forest_bits(first->model.forests[0]), forest_bits(threaded->model.forests[0]));
  aug.interactions_fixed = false;
  const auto selected = learn::fit_gbt_dataset(*dataset, 0, 16, 18, budget, aug, cfg);
  ASSERT_TRUE(selected);
  EXPECT_NE(first->algorithm_recipe, selected->algorithm_recipe);
  aug.interactions_fixed = true;
  aug.interactions = {{0, 2}};
  const auto pair = learn::fit_gbt_dataset(*dataset, 0, 16, 18, budget, aug, cfg);
  ASSERT_TRUE(pair);
  EXPECT_NE(first->algorithm_recipe, pair->algorithm_recipe);
  aug.pca.emplace();
  aug.pca->fit_upto_date = 17; // disabled basis still has explicit provenance
  const auto pca = learn::fit_gbt_dataset(*dataset, 0, 16, 18, budget, aug, cfg);
  ASSERT_TRUE(pca);
  EXPECT_NE(pair->algorithm_recipe, pca->algorithm_recipe);
  aug.pca->fit_upto_date = 18;
  const auto clock = learn::fit_gbt_dataset(*dataset, 0, 16, 18, budget, aug, cfg);
  ASSERT_TRUE(clock);
  EXPECT_NE(pca->algorithm_recipe, clock->algorithm_recipe);
  aug.pca->k = 1;
  aug.pca->model.mean = lin::VecX::Zero(4);
  aug.pca->model.components = lin::MatX::Zero(4, 1);
  aug.pca->model.components(0, 0) = 1;
  aug.pca->model.explained_variance = lin::VecX::Ones(1);
  aug.pca->model.explained_ratio = lin::VecX::Ones(1);
  const auto basis = learn::fit_gbt_dataset(*dataset, 0, 16, 18, budget, aug, cfg);
  ASSERT_TRUE(basis);
  aug.pca->model.components(0, 0) = -1;
  const auto rotated = learn::fit_gbt_dataset(*dataset, 0, 16, 18, budget, aug, cfg);
  ASSERT_TRUE(rotated);
  EXPECT_NE(basis->algorithm_recipe, rotated->algorithm_recipe);
}
} // namespace
