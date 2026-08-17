// VRP model-layer suite (vrp-model lane): the schema-v2 IFairVolModel seam
// (linear/elastic-net TSV + flat-array GBT scorer, byte-stable file round
// trips) and the walk-forward trainer pipeline in tools/vrp_train.hpp
// (frozen vrp_panel_v1 parsing, purged/embargoed walk-forward, train-fold-only
// per-asset standardization, QLIKE in variance levels, retransform + insanity
// clip, deterministic model files, frozen vrp_signal_v1 output).
//
// Suites: VrpModel.* / VrpTrain*.* (seam / trainer). Neither needs a fitted
// PricedSurface -- everything here is file-level and panel-level, so the suite
// stays light and lands in the atx_vol_fast label.

#include "pricing/theo.hpp"
#include "vrp_train.hpp" // tools/ include root (granted to atx-vol-tests)

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <limits>
#include <memory>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include <gtest/gtest.h>

#include "atx/vol/api/backtest/research_validation.hpp" // validate_research_plan_no_leakage
#include "atx/vol/api/backtest/vol_edge.hpp" // the UNMODIFIED frozen vrp_signal_v1 loader
#include "atx/vol/api/core/types.hpp"        // Result, Status, ErrorCode

namespace atx::vol {
namespace {

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

// ── shared temp-path helpers (mirrors theo_test.cpp's ScopedTempFile) ───────

[[nodiscard]] std::filesystem::path unique_temp_path(std::string_view label,
                                                     std::string_view extension) {
  static std::atomic<std::uint64_t> sequence{0};
  const auto tick = std::chrono::steady_clock::now().time_since_epoch().count();
  return std::filesystem::temp_directory_path() /
         ("atx_vrp_" + std::string(label) + "_" + std::to_string(tick) + "_" +
          std::to_string(sequence.fetch_add(1, std::memory_order_relaxed)) +
          std::string(extension));
}

class ScopedTempFile {
public:
  ScopedTempFile(std::string_view label, std::string_view content)
      : path_(unique_temp_path(label, ".tsv")) {
    std::ofstream out(path_, std::ios::binary | std::ios::trunc);
    out.write(content.data(), static_cast<std::streamsize>(content.size()));
  }

  ~ScopedTempFile() {
    std::error_code ec;
    std::filesystem::remove(path_, ec);
  }

  ScopedTempFile(const ScopedTempFile &) = delete;
  ScopedTempFile &operator=(const ScopedTempFile &) = delete;
  ScopedTempFile(ScopedTempFile &&) = delete;
  ScopedTempFile &operator=(ScopedTempFile &&) = delete;

  [[nodiscard]] std::string path_string() const { return path_.string(); }

private:
  std::filesystem::path path_;
};

[[nodiscard]] std::string read_file_bytes(const std::filesystem::path &path) {
  std::ifstream in(path, std::ios::binary);
  std::ostringstream buf;
  buf << in.rdbuf();
  return buf.str();
}

// ── VrpModel: schema lookup ─────────────────────────────────────────────────

TEST(VrpModel, FairVolFeatureCountLookupKnowsBothSchemasAndFailsClosed) {
  EXPECT_EQ(fair_vol_feature_count(kFairVolFeatureSchemaV1), kFairVolFeatureCount);
  EXPECT_EQ(fair_vol_feature_count(kVrpFeatureSchemaV1), kVrpFeatureCount);
  EXPECT_EQ(kVrpFeatureSchemaV1, 2u);
  EXPECT_EQ(kVrpFeatureCount, std::size_t{10});
  EXPECT_EQ(fair_vol_feature_count(0), std::size_t{0});
  EXPECT_EQ(fair_vol_feature_count(99), std::size_t{0});
}

// ── VrpModel: linear v2 (elastic-net coefficient TSV) ───────────────────────

[[nodiscard]] LinearFairVolParams make_linear_v2_params() {
  LinearFairVolParams p;
  p.feature_schema = kVrpFeatureSchemaV1;
  p.intercept = -0.25;
  p.coefficients = {0.3, 0.35, 0.3, 0.0, -0.1, 0.05, 0.0, 0.0, 0.0, 0.125};
  return p;
}

TEST(VrpModel, LinearV2SaveLoadSaveRoundTripIsByteStable) {
  const LinearFairVolParams params = make_linear_v2_params();
  const std::filesystem::path p1 = unique_temp_path("lin_rt1", ".tsv");
  const std::filesystem::path p2 = unique_temp_path("lin_rt2", ".tsv");
  ASSERT_TRUE(save_linear_fair_vol_params(params, p1.string()).has_value());
  const auto loaded = load_linear_fair_vol_params(p1.string());
  ASSERT_TRUE(loaded.has_value()) << loaded.error().to_string();
  EXPECT_EQ(loaded->feature_schema, kVrpFeatureSchemaV1);
  ASSERT_TRUE(save_linear_fair_vol_params(*loaded, p2.string()).has_value());
  EXPECT_EQ(read_file_bytes(p1), read_file_bytes(p2));
  EXPECT_FALSE(read_file_bytes(p1).empty());
  std::error_code ec;
  std::filesystem::remove(p1, ec);
  std::filesystem::remove(p2, ec);
}

TEST(VrpModel, LinearV2PredictMatchesHandComputation) {
  const LinearFairVolParams params = make_linear_v2_params();
  auto model = make_linear_fair_vol_model(params);
  ASSERT_TRUE(model.has_value()) << model.error().to_string();
  EXPECT_EQ((*model)->feature_schema(), kVrpFeatureSchemaV1);

  const std::array<double, 10> x{-3.1, -3.0, -2.9, 0.2, 0.01, -0.03, 0.001, 0.02, 0.0, 0.12};
  double expected = params.intercept;
  for (std::size_t i = 0; i < x.size(); ++i) {
    expected += params.coefficients[i] * x[i];
  }
  std::array<double, 1> y{};
  const Status st = (*model)->predict(x, 1, y);
  ASSERT_TRUE(st.has_value()) << st.error().to_string();
  EXPECT_NEAR(y[0], expected, 1e-12);
}

TEST(VrpModel, LinearLoaderAcceptsSchemaTwoWidthTenFile) {
  // Ten coefficients + intercept under "# schema=2": the generalized loader
  // accepts it and the model reports the v2 schema.
  const ScopedTempFile f("lin_v2", "# schema=2\n0.5\t1 2 3 4 5 6 7 8 9 10\n");
  const auto model = load_linear_fair_vol_model(f.path_string());
  ASSERT_TRUE(model.has_value()) << model.error().to_string();
  EXPECT_EQ((*model)->feature_schema(), kVrpFeatureSchemaV1);
  std::array<double, 10> x{};
  x[9] = 2.0;
  std::array<double, 1> y{};
  ASSERT_TRUE((*model)->predict(x, 1, y).has_value());
  EXPECT_NEAR(y[0], 0.5 + 10.0 * 2.0, 1e-12);
}

TEST(VrpModel, LinearLoaderRejectsUnknownSchemaFailClosed) {
  const ScopedTempFile f("lin_unknown", "# schema=99\n0 0 0 0 0 0 0 0 0\n");
  const auto model = load_linear_fair_vol_model(f.path_string());
  ASSERT_FALSE(model.has_value());
  EXPECT_EQ(model.error().code(), ErrorCode::ParseError);
}

TEST(VrpModel, LinearLoaderRejectsSchemaTwoWithV1Width) {
  // A "# schema=2" file carrying only 8 coefficients (the v1 width) must be
  // refused: the schema decides the width, fail closed on the mismatch.
  const ScopedTempFile f("lin_v2_short", "# schema=2\n0 0 0 0 0 0 0 0 0\n");
  const auto model = load_linear_fair_vol_model(f.path_string());
  ASSERT_FALSE(model.has_value());
  EXPECT_EQ(model.error().code(), ErrorCode::ParseError);
}

TEST(VrpModel, MakeLinearModelRejectsWidthSchemaMismatch) {
  LinearFairVolParams p = make_linear_v2_params();
  p.coefficients.resize(kFairVolFeatureCount); // 8 coefs under a width-10 schema
  const auto model = make_linear_fair_vol_model(p);
  ASSERT_FALSE(model.has_value());
  EXPECT_EQ(model.error().code(), ErrorCode::InvalidArgument);
}

TEST(VrpModel, OverlayStaysV1OnlyAndRefusesSchemaTwoModels) {
  // make_fair_vol_model_overlay assembles the v1 feature layout and must keep
  // refusing schema-2 models (the brief's "overlay stays v1-only" contract).
  auto model = make_linear_fair_vol_model(make_linear_v2_params());
  ASSERT_TRUE(model.has_value()) << model.error().to_string();
  std::shared_ptr<const IFairVolModel> shared = std::move(*model);
  const auto overlay = make_fair_vol_model_overlay(shared);
  ASSERT_FALSE(overlay.has_value());
  EXPECT_EQ(overlay.error().code(), ErrorCode::InvalidArgument);
}

// ── VrpModel: flat-array GBT scorer ─────────────────────────────────────────

// A hand-built 2-tree fixture on the v2 schema.
//
// tree 0 (nodes 0..2): split f3 < 0.5 -> leaf +1.0 else leaf -2.0
// tree 1 (nodes 3..7): node 3 splits f0 < -3.0; LEFT -> node 4 (splits
//   f9 < 0.1 -> leaf 6 (+0.25) : leaf 7 (+0.5)); RIGHT -> node 5 (leaf
//   -0.125). base = 0.75.
[[nodiscard]] GbtFairVolModelData make_two_tree_fixture() {
  GbtFairVolModelData d;
  d.feature_schema = kVrpFeatureSchemaV1;
  d.base = 0.75;
  d.tree_first_node = {0, 3};
  d.feature_idx = {3, 0, 0, 0, 9, 0, 0, 0};
  d.threshold = {0.5, 0.0, 0.0, -3.0, 0.1, 0.0, 0.0, 0.0};
  d.left = {1, -1, -1, 4, 6, -1, -1, -1};
  d.right = {2, -1, -1, 5, 7, -1, -1, -1};
  d.leaf_value = {0.0, 1.0, -2.0, 0.0, 0.0, -0.125, 0.25, 0.5};
  return d;
}

[[nodiscard]] double two_tree_expected(const std::array<double, 10> &x) {
  const double t0 = (x[3] < 0.5) ? 1.0 : -2.0;
  double t1 = -0.125;
  if (x[0] < -3.0) {
    t1 = (x[9] < 0.1) ? 0.25 : 0.5;
  }
  return 0.75 + t0 + t1;
}

TEST(VrpModel, GbtPredictMatchesHandBuiltTwoTreeFixture) {
  auto model = make_gbt_fair_vol_model(make_two_tree_fixture());
  ASSERT_TRUE(model.has_value()) << model.error().to_string();
  EXPECT_EQ((*model)->feature_schema(), kVrpFeatureSchemaV1);

  const std::array<std::array<double, 10>, 4> rows{{
      {-3.5, 0, 0, 0.2, 0, 0, 0, 0, 0, 0.05},
      {-3.5, 0, 0, 0.7, 0, 0, 0, 0, 0, 0.2},
      {-2.0, 0, 0, 0.4, 0, 0, 0, 0, 0, 0.0},
      {-4.0, 0, 0, 0.6, 0, 0, 0, 0, 0, 0.15},
  }};
  std::array<double, 40> flat{};
  for (std::size_t r = 0; r < rows.size(); ++r) {
    std::copy(rows[r].begin(), rows[r].end(), flat.begin() + static_cast<std::ptrdiff_t>(r * 10));
  }
  std::array<double, 4> y{};
  const Status st = (*model)->predict(flat, rows.size(), y);
  ASSERT_TRUE(st.has_value()) << st.error().to_string();
  for (std::size_t r = 0; r < rows.size(); ++r) {
    EXPECT_NEAR(y[r], two_tree_expected(rows[r]), 1e-12) << "row " << r;
  }
}

TEST(VrpModel, GbtNaNFeatureRoutesToRightChild) {
  // IEEE: NaN < threshold is false, so a NaN feature deterministically takes
  // the right branch -- pinned so the scorer's behavior cannot silently drift.
  auto model = make_gbt_fair_vol_model(make_two_tree_fixture());
  ASSERT_TRUE(model.has_value()) << model.error().to_string();
  std::array<double, 10> x{-2.0, 0, 0, kNaN, 0, 0, 0, 0, 0, 0.0};
  std::array<double, 1> y{};
  ASSERT_TRUE((*model)->predict(x, 1, y).has_value());
  // tree0: NaN f3 -> right leaf (-2.0); tree1: f0 = -2.0 -> right leaf -0.125.
  EXPECT_NEAR(y[0], 0.75 - 2.0 - 0.125, 1e-12);
}

TEST(VrpModel, GbtSaveLoadSaveRoundTripIsByteStable) {
  const GbtFairVolModelData d = make_two_tree_fixture();
  const std::filesystem::path p1 = unique_temp_path("gbt_rt1", ".tsv");
  const std::filesystem::path p2 = unique_temp_path("gbt_rt2", ".tsv");
  ASSERT_TRUE(save_gbt_fair_vol_model_data(d, p1.string()).has_value());
  const auto loaded = load_gbt_fair_vol_model_data(p1.string());
  ASSERT_TRUE(loaded.has_value()) << loaded.error().to_string();
  ASSERT_TRUE(save_gbt_fair_vol_model_data(*loaded, p2.string()).has_value());
  EXPECT_EQ(read_file_bytes(p1), read_file_bytes(p2));
  EXPECT_FALSE(read_file_bytes(p1).empty());

  // And the loaded data predicts identically to the original fixture.
  auto model = load_gbt_fair_vol_model(p1.string());
  ASSERT_TRUE(model.has_value()) << model.error().to_string();
  const std::array<double, 10> x{-3.5, 0, 0, 0.2, 0, 0, 0, 0, 0, 0.05};
  std::array<double, 1> y{};
  ASSERT_TRUE((*model)->predict(x, 1, y).has_value());
  EXPECT_NEAR(y[0], two_tree_expected(x), 1e-12);
  std::error_code ec;
  std::filesystem::remove(p1, ec);
  std::filesystem::remove(p2, ec);
}

TEST(VrpModel, GbtLoadRejectsSchemaMismatchFailClosed) {
  const GbtFairVolModelData d = make_two_tree_fixture();
  const std::filesystem::path p1 = unique_temp_path("gbt_schema", ".tsv");
  ASSERT_TRUE(save_gbt_fair_vol_model_data(d, p1.string()).has_value());
  // Rewrite the schema comment to an unknown id.
  std::string bytes = read_file_bytes(p1);
  const std::size_t pos = bytes.find("# schema=2");
  ASSERT_NE(pos, std::string::npos);
  bytes.replace(pos, std::string_view{"# schema=2"}.size(), "# schema=7");
  {
    std::ofstream out(p1, std::ios::binary | std::ios::trunc);
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
  }
  const auto model = load_gbt_fair_vol_model(p1.string());
  ASSERT_FALSE(model.has_value());
  EXPECT_EQ(model.error().code(), ErrorCode::ParseError);
  std::error_code ec;
  std::filesystem::remove(p1, ec);
}

TEST(VrpModel, GbtLoadMissingFileIsIoError) {
  const auto model = load_gbt_fair_vol_model("this/path/does/not/exist.gbt.tsv");
  ASSERT_FALSE(model.has_value());
  EXPECT_EQ(model.error().code(), ErrorCode::IoError);
}

// ── VrpModel: GBT loader fail-closed grammar (counts / truncation / trailing) ─

// Saves the two-tree fixture (trees\t2, nodes\t8), applies one byte-level
// find/replace mutation, and returns the loader's Result. Every mutation below
// must fail closed with Err(ParseError) -- never an exception escaping the
// Result-returning loader.
[[nodiscard]] Result<GbtFairVolModelData> load_gbt_fixture_with_mutation(
    std::string_view label, std::string_view find, std::string_view replace) {
  const GbtFairVolModelData d = make_two_tree_fixture();
  const std::filesystem::path p = unique_temp_path(label, ".tsv");
  EXPECT_TRUE(save_gbt_fair_vol_model_data(d, p.string()).has_value());
  std::string bytes = read_file_bytes(p);
  const std::size_t pos = bytes.find(find);
  EXPECT_NE(pos, std::string::npos) << "fixture no longer contains '" << find << "'";
  if (pos != std::string::npos) {
    bytes.replace(pos, find.size(), replace);
  }
  {
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
  }
  auto loaded = load_gbt_fair_vol_model_data(p.string());
  std::error_code ec;
  std::filesystem::remove(p, ec);
  return loaded;
}

TEST(VrpModel, GbtLoadRejectsOverdeclaredTreeCountWithoutReserving) {
  // 'trees\t400000000000' parses into size_t; the loader must bound it by the
  // content lines actually present BEFORE any vector::reserve, so a corrupt
  // count is Err(ParseError) instead of bad_alloc/length_error escaping.
  const auto loaded = load_gbt_fixture_with_mutation("gbt_trees_huge", "trees\t2\n",
                                                     "trees\t400000000000\n");
  ASSERT_FALSE(loaded.has_value());
  EXPECT_EQ(loaded.error().code(), ErrorCode::ParseError);
}

TEST(VrpModel, GbtLoadRejectsOverdeclaredNodeCountWithoutReserving) {
  const auto loaded = load_gbt_fixture_with_mutation("gbt_nodes_huge", "nodes\t8\n",
                                                     "nodes\t400000000000\n");
  ASSERT_FALSE(loaded.has_value());
  EXPECT_EQ(loaded.error().code(), ErrorCode::ParseError);
}

TEST(VrpModel, GbtLoadRejectsTruncatedNodeSection) {
  // Declares one more node than the file carries.
  const auto loaded =
      load_gbt_fixture_with_mutation("gbt_nodes_trunc", "nodes\t8\n", "nodes\t9\n");
  ASSERT_FALSE(loaded.has_value());
  EXPECT_EQ(loaded.error().code(), ErrorCode::ParseError);
}

TEST(VrpModel, GbtLoadRejectsTruncatedNodeLine) {
  // Final node line loses its leaf field (5 -> 4 fields).
  const auto loaded =
      load_gbt_fixture_with_mutation("gbt_node_4field", "\t-1\t-1\t0.5\n", "\t-1\t-1\n");
  ASSERT_FALSE(loaded.has_value());
  EXPECT_EQ(loaded.error().code(), ErrorCode::ParseError);
}

TEST(VrpModel, GbtLoadRejectsTrailingContentAfterDeclaredNodes) {
  // An extra node-shaped line after the declared count must fail closed --
  // deleting the loader's 'at != content.size()' trailing check would turn
  // this into a silent Ok.
  const auto loaded = load_gbt_fixture_with_mutation(
      "gbt_trailing", "\t-1\t-1\t0.5\n", "\t-1\t-1\t0.5\n0\t0\t-1\t-1\t0.125\n");
  ASSERT_FALSE(loaded.has_value());
  EXPECT_EQ(loaded.error().code(), ErrorCode::ParseError);
}

TEST(VrpModel, GbtMakeRejectsChildIndexNotStrictlyIncreasing) {
  GbtFairVolModelData d = make_two_tree_fixture();
  d.left[0] = 0; // self-cycle: a child must be strictly greater than its parent
  const auto model = make_gbt_fair_vol_model(d);
  ASSERT_FALSE(model.has_value());
  EXPECT_EQ(model.error().code(), ErrorCode::InvalidArgument);
}

TEST(VrpModel, GbtMakeRejectsHalfLeafNode) {
  GbtFairVolModelData d = make_two_tree_fixture();
  d.right[1] = 2; // left == -1 but right != -1: neither leaf nor split
  const auto model = make_gbt_fair_vol_model(d);
  ASSERT_FALSE(model.has_value());
  EXPECT_EQ(model.error().code(), ErrorCode::InvalidArgument);
}

TEST(VrpModel, GbtMakeRejectsFeatureIndexOutOfSchemaWidth) {
  GbtFairVolModelData d = make_two_tree_fixture();
  d.feature_idx[0] = 10; // width is exactly 10 for schema 2
  const auto model = make_gbt_fair_vol_model(d);
  ASSERT_FALSE(model.has_value());
  EXPECT_EQ(model.error().code(), ErrorCode::InvalidArgument);
}

// ── VrpTrain: synthetic vrp_panel_v1 fixture ────────────────────────────────

constexpr std::size_t kSynthDates = 180;
constexpr std::size_t kSynthTail = 21;
constexpr std::array<std::string_view, 3> kSynthSymbols{"AAA", "BBB", "CCC"};
constexpr std::int64_t kSynthBaseTs = 1'600'000'000'000'000'000LL;
constexpr std::int64_t kSynthDayNs = 86'400'000'000'000LL; // one day in ns

[[nodiscard]] std::string synth_date_string(std::size_t d) {
  // A synthetic-but-sortable date label; the trainer treats it as opaque text.
  char buf[16];
  std::snprintf(buf, sizeof buf, "2020-%03zu", d);
  return std::string(buf);
}

[[nodiscard]] std::string fmt_num(double v) {
  if (std::isnan(v)) {
    return "nan";
  }
  char buf[64];
  const int len = std::snprintf(buf, sizeof buf, "%.17g", v);
  return std::string(buf, static_cast<std::size_t>(len > 0 ? len : 0));
}

inline constexpr std::string_view kSynthPanelHeader =
    "symbol\tdate\tentry_ts_ns\tspot\tiv_fair_21d\tiv_fair_63d\trv_fwd_21d\tlabel\t"
    "f0_log_rv1\tf1_log_rv5\tf2_log_rv21\tf3_iv_level\tf4_term_slope\tf5_hv_iv_gap\t"
    "f6_vrp_lag\tf7_ret_21d\tf8_jump_recent\tf9_vov_63d\n";

// Deterministic 3-symbol panel with a PLANTED linear relation in log space:
//   ln(rv_fwd^2) = -0.35 + 0.30 f0 + 0.35 f1 + 0.30 f2 + tiny wiggle
// so the log-HAR baseline on {f0, f1, f2} must beat a train-mean variance
// forecast on QLIKE. Tail rows carry NaN label + NaN rv_fwd (frozen contract);
// a few f4 cells are NaN (iv_fair_63d OutOfRange propagation). `n_dates`
// stretches/shrinks the session axis (round-2 fold auto-scaling fixtures).
[[nodiscard]] std::string make_synth_panel_tsv(std::size_t n_dates = kSynthDates) {
  std::string out;
  out += "# schema=vrp_panel_v1\n";
  out += "# horizon_days=21\n";
  out += kSynthPanelHeader;
  for (std::size_t d = 0; d < n_dates; ++d) {
    for (std::size_t s = 0; s < kSynthSymbols.size(); ++s) {
      const double ds = static_cast<double>(d);
      const double ss = static_cast<double>(s);
      const double level = 0.15 + 0.05 * ss;
      const double wig = 0.25 * std::sin(0.11 * ds + 2.1 * ss) //
                         + 0.15 * std::sin(0.023 * ds + 0.7 * ss);
      const double f0 = 2.0 * std::log(level) + wig + 0.08 * std::sin(0.9 * ds + ss);
      const double f1 = 2.0 * std::log(level) + 0.8 * wig;
      const double f2 = 2.0 * std::log(level) + 0.6 * wig;
      const double y_log =
          -0.35 + 0.30 * f0 + 0.35 * f1 + 0.30 * f2 + 0.02 * std::sin(3.7 * ds + 1.3 * ss);
      const bool tail = d + kSynthTail >= n_dates;
      const double rv_fwd = tail ? kNaN : std::sqrt(std::exp(y_log));
      const double iv21 = std::sqrt(std::exp(2.0 * std::log(level) + 0.5 * wig)) * 1.05;
      const bool iv63_missing = (s == 2 && d % 37 == 5);
      const double iv63 = iv63_missing ? kNaN : iv21 * 1.03;
      const double label = tail ? kNaN : (rv_fwd * rv_fwd - iv21 * iv21) * (21.0 / 252.0);
      const double f3 = iv21;
      const double f4 = iv63_missing ? kNaN : iv63 - iv21;
      const double f5 = std::sqrt(std::exp(f2)) - iv21;
      const double f6 = (std::exp(f2) - iv21 * iv21) * (21.0 / 252.0);
      const double f7 = 0.02 * std::sin(0.5 * ds + ss);
      const double f8 = (d % 29 == 3) ? 1.0 : 0.0;
      const double f9 = 0.10 + 0.03 * std::sin(0.07 * ds + ss);
      out += std::string(kSynthSymbols[s]) + '\t' + synth_date_string(d) + '\t' +
             std::to_string(kSynthBaseTs + static_cast<std::int64_t>(d) * kSynthDayNs) + '\t' +
             fmt_num(100.0 + ss) + '\t' + fmt_num(iv21) + '\t' + fmt_num(iv63) + '\t' +
             fmt_num(rv_fwd) + '\t' + fmt_num(label) + '\t' + fmt_num(f0) + '\t' + fmt_num(f1) +
             '\t' + fmt_num(f2) + '\t' + fmt_num(f3) + '\t' + fmt_num(f4) + '\t' + fmt_num(f5) +
             '\t' + fmt_num(f6) + '\t' + fmt_num(f7) + '\t' + fmt_num(f8) + '\t' + fmt_num(f9) +
             '\n';
    }
  }
  return out;
}

// Textual row surgery on a generated panel TSV: applies `fn` to each DATA
// row's 18 fields (mutable, in place); a false return DROPS the row. Comment
// lines and the header pass through untouched -- the fixtures for interior
// surface holes, mid-sample NaN labels, and NaN-f9 warmups are all built
// this way instead of via new generators.
[[nodiscard]] std::string
transform_panel_rows(const std::string &tsv,
                     const std::function<bool(std::vector<std::string> &)> &fn) {
  std::string out;
  bool header_seen = false;
  std::size_t start = 0;
  while (start < tsv.size()) {
    std::size_t end = tsv.find('\n', start);
    if (end == std::string::npos) {
      end = tsv.size();
    }
    const std::string line = tsv.substr(start, end - start);
    start = end + 1;
    if (line.empty() || line.front() == '#' || !header_seen) {
      if (!line.empty() && line.front() != '#') {
        header_seen = true;
      }
      out += line;
      out += '\n';
      continue;
    }
    std::vector<std::string> fields;
    std::size_t p = 0;
    while (true) {
      const std::size_t tab = line.find('\t', p);
      if (tab == std::string::npos) {
        fields.push_back(line.substr(p));
        break;
      }
      fields.push_back(line.substr(p, tab - p));
      p = tab + 1;
    }
    if (fn(fields)) {
      for (std::size_t i = 0; i < fields.size(); ++i) {
        if (i != 0) {
          out += '\t';
        }
        out += fields[i];
      }
      out += '\n';
    }
  }
  return out;
}

// Regime-break fixture for the GBT insanity-clip test (round-2 item 4): one
// symbol, 140 sessions; iv 0.5 while every train row forms vs 0.05 in the
// test window, rv ~ 0.1 throughout. The GBT learns a strongly negative
// label (~ -0.02 total-variance units) from the train era, so its implied
// test-row variance forecast label/H + iv^2 ~ -0.24 + 0.0025 goes NEGATIVE
// -- exactly the shape that exploded round-1 SP100 QLIKE through the 1e-10
// floor. f3 is constant over every train row (sd 0 -> z 0), so the regime
// switch cannot leak in as a feature.
[[nodiscard]] std::string make_regime_break_panel_tsv() {
  constexpr std::size_t kDates = 140;
  std::string out;
  out += "# schema=vrp_panel_v1\n";
  out += "# horizon_days=21\n";
  out += kSynthPanelHeader;
  for (std::size_t d = 0; d < kDates; ++d) {
    const double ds = static_cast<double>(d);
    const double rv = 0.1 + 0.01 * std::sin(0.37 * ds);
    const double iv = d < 95 ? 0.5 : 0.05;
    const bool tail = d + kSynthTail >= kDates;
    const double rv_fwd = tail ? kNaN : rv;
    const double label = tail ? kNaN : (rv * rv - iv * iv) * (21.0 / 252.0);
    const double f_rv = std::log(rv * rv);
    out += "AAA\t" + synth_date_string(d) + '\t' +
           std::to_string(kSynthBaseTs + static_cast<std::int64_t>(d) * kSynthDayNs) +
           "\t100\t" + fmt_num(iv) + '\t' + fmt_num(iv * 1.03) + '\t' + fmt_num(rv_fwd) + '\t' +
           fmt_num(label) + '\t' + fmt_num(f_rv) + '\t' + fmt_num(f_rv) + '\t' + fmt_num(f_rv) +
           '\t' + fmt_num(std::log(iv * iv)) + "\t0.01\t-0.05\t0.001\t0.02\t0\t0.1\n";
  }
  return out;
}

// Sparse-EMITTED-rows fixture (DUK-class sparsity): CCC thinned to every
// other session. Its 21-emitted-rows label window spans 42 pooled sessions
// -- exactly AT the default span cap, so its rows stay admitted and its
// 42-session span becomes the embargo source. label_end must be the
// EMITTED-AXIS end (a provable upper bound on the true bar-axis t+21 end):
// the fix-2 review demonstrated that the pooled-axis "true horizon" claim
// is false for bar-holey symbols and admitted leaking train rows.
[[nodiscard]] std::string make_sparse_ccc_panel_tsv() {
  return transform_panel_rows(make_synth_panel_tsv(), [](std::vector<std::string> &f) {
    if (f[0] != "CCC") {
      return true;
    }
    return std::stoi(f[1].substr(5)) % 2 == 0; // keep even sessions only
  });
}

// Bar-holey fixture (the fix-2 review's demonstrated-leak attack shape):
// HHH's LABEL-GENERATION (bar) axis is SPARSER than the pooled axis --
// present two sessions of every three (d % 3 != 2) while AAA/BBB/CCC keep
// the pooled axis dense. Emitted rows == bars for HHH, so bar q sits at
// session d(q) = 3*(q/2) + (q%2) and the TRUE label end of bar q is bar
// q+21 at session d(q) + 31 (q even) or + 32 (q odd) -- STRICTLY LATER
// than pooled session d(q)+21. A pooled-axis label_end understates every
// such row's true window (SP100: HD/AMT class, 77 of 102 names carried
// >= 1 in-span bar hole).
[[nodiscard]] std::size_t bar_holey_session_of(std::size_t q) {
  return 3 * (q / 2) + (q % 2);
}

[[nodiscard]] std::string make_bar_holey_panel_tsv() {
  std::string out = make_synth_panel_tsv();
  constexpr std::size_t kBars = 120; // sessions 0..178, d % 3 != 2
  for (std::size_t q = 0; q < kBars; ++q) {
    const std::size_t d = bar_holey_session_of(q);
    const bool tail = q + kSynthTail >= kBars;
    out += "HHH\t" + synth_date_string(d) + '\t' +
           std::to_string(kSynthBaseTs + static_cast<std::int64_t>(d) * kSynthDayNs) +
           (tail ? "\t100\t0.2\t0.206\tnan\tnan" : "\t100\t0.2\t0.206\t0.25\t0.001875") +
           "\t-3\t-3\t-3\t0.2\t0.01\t-0.05\t0.001\t0.02\t0\t0.1\n";
  }
  return out;
}

// Ultra-sparse fixture for the span cap: SSS present every 3rd session only
// (60 bars over 180 sessions), so each 21-emitted-row window spans 63
// pooled sessions -- past the default 42-session cap (2x horizon).
[[nodiscard]] std::string make_span_cap_sss_panel_tsv() {
  std::string out = make_synth_panel_tsv();
  constexpr std::size_t kBars = 60; // sessions 0, 3, ..., 177
  for (std::size_t q = 0; q < kBars; ++q) {
    const std::size_t d = 3 * q;
    const bool tail = q + kSynthTail >= kBars;
    out += "SSS\t" + synth_date_string(d) + '\t' +
           std::to_string(kSynthBaseTs + static_cast<std::int64_t>(d) * kSynthDayNs) +
           (tail ? "\t100\t0.2\t0.206\tnan\tnan" : "\t100\t0.2\t0.206\t0.25\t0.001875") +
           "\t-3\t-3\t-3\t0.2\t0.01\t-0.05\t0.001\t0.02\t0\t0.1\n";
  }
  return out;
}

[[nodiscard]] vrp::VrpTrainConfig make_synth_config(const std::string &panel_path,
                                                    const std::string &out_dir) {
  vrp::VrpTrainConfig cfg;
  cfg.panel_path = panel_path;
  cfg.out_dir = out_dir;
  cfg.master_seed = 42;
  cfg.walk.min_train_sessions = 90;
  cfg.walk.test_sessions = 20;
  cfg.walk.step_sessions = 20;
  cfg.en_lambda = 1e-3;
  cfg.en_alpha = 0.5;
  return cfg;
}

// One end-to-end trainer run shared by every VrpTrain pipeline assertion --
// run_vrp_train is deterministic, so a single SetUpTestSuite run is the
// arrange step for all of them.
class VrpTrainPipelineTest : public ::testing::Test {
protected:
  static void SetUpTestSuite() {
    panel_file_ = new ScopedTempFile("pipeline_panel", make_synth_panel_tsv());
    out_dir_ = unique_temp_path("pipeline_out", "");
    auto report =
        vrp::run_vrp_train(make_synth_config(panel_file_->path_string(), out_dir_.string()));
    ASSERT_TRUE(report.has_value()) << report.error().to_string();
    report_ = new vrp::VrpTrainReport(std::move(*report));
  }

  static void TearDownTestSuite() {
    delete report_;
    report_ = nullptr;
    delete panel_file_;
    panel_file_ = nullptr;
    std::error_code ec;
    std::filesystem::remove_all(out_dir_, ec);
  }

  static ScopedTempFile *panel_file_;
  static std::filesystem::path out_dir_;
  static vrp::VrpTrainReport *report_;
};

ScopedTempFile *VrpTrainPipelineTest::panel_file_ = nullptr;
std::filesystem::path VrpTrainPipelineTest::out_dir_{};
vrp::VrpTrainReport *VrpTrainPipelineTest::report_ = nullptr;

// ── VrpTrain: panel loader ──────────────────────────────────────────────────

TEST(VrpTrainLoader, ParsesFrozenPanelContract) {
  const ScopedTempFile f("loader_ok", make_synth_panel_tsv());
  const auto panel = vrp::load_vrp_panel(f.path_string());
  ASSERT_TRUE(panel.has_value()) << panel.error().to_string();
  EXPECT_EQ(panel->rows.size(), kSynthDates * kSynthSymbols.size());
  EXPECT_EQ(panel->symbols.size(), kSynthSymbols.size());
  // Canonical (entry_ts_ns, symbol) order.
  for (std::size_t i = 1; i < panel->rows.size(); ++i) {
    const auto &a = panel->rows[i - 1];
    const auto &b = panel->rows[i];
    EXPECT_TRUE(a.entry_ts_ns < b.entry_ts_ns ||
                (a.entry_ts_ns == b.entry_ts_ns && a.symbol < b.symbol));
  }
  // Tail rows keep NaN label + NaN rv_fwd; leading rows are labeled.
  const auto &first = panel->rows.front();
  EXPECT_TRUE(std::isfinite(first.label));
  EXPECT_TRUE(std::isfinite(first.rv_fwd_21d));
  const auto &last = panel->rows.back();
  EXPECT_TRUE(std::isnan(last.label));
  EXPECT_TRUE(std::isnan(last.rv_fwd_21d));
  // The planted NaN f4 cells survive parsing as NaN (not dropped, not zeroed).
  bool saw_nan_f4 = false;
  for (const auto &row : panel->rows) {
    if (std::isnan(row.f[4])) {
      saw_nan_f4 = true;
      EXPECT_TRUE(std::isnan(row.iv_fair_63d));
    }
  }
  EXPECT_TRUE(saw_nan_f4);
}

TEST(VrpTrainLoader, RejectsSchemaCommentMismatchFailClosed) {
  std::string tsv = make_synth_panel_tsv();
  const std::size_t pos = tsv.find("vrp_panel_v1");
  ASSERT_NE(pos, std::string::npos);
  tsv.replace(pos, std::string_view{"vrp_panel_v1"}.size(), "vrp_panel_v9");
  const ScopedTempFile f("loader_schema", tsv);
  const auto panel = vrp::load_vrp_panel(f.path_string());
  ASSERT_FALSE(panel.has_value());
  EXPECT_EQ(panel.error().code(), ErrorCode::ParseError);
}

TEST(VrpTrainLoader, RejectsReorderedHeaderColumns) {
  std::string tsv = make_synth_panel_tsv();
  const std::size_t pos = tsv.find("symbol\tdate");
  ASSERT_NE(pos, std::string::npos);
  tsv.replace(pos, std::string_view{"symbol\tdate"}.size(), "date\tsymbol");
  const ScopedTempFile f("loader_header", tsv);
  const auto panel = vrp::load_vrp_panel(f.path_string());
  ASSERT_FALSE(panel.has_value());
  EXPECT_EQ(panel.error().code(), ErrorCode::ParseError);
}

TEST(VrpTrainLoader, MissingFileIsIoError) {
  const auto panel = vrp::load_vrp_panel("no/such/panel.tsv");
  ASSERT_FALSE(panel.has_value());
  EXPECT_EQ(panel.error().code(), ErrorCode::IoError);
}

TEST(VrpTrainLoader, RejectsNegativeVovFeatureFailClosed) {
  // f9_vov_63d is a sample stdev, so a finite negative value is a panel
  // contract violation. Validated at the boundary (review minor, round 2):
  // letting it through would either emit a negative vov_63d the frozen
  // signal loader fail-closes on (raw pass-through) or drag the per-asset
  // imputation mean negative.
  std::string tsv = make_synth_panel_tsv();
  tsv = transform_panel_rows(tsv, [](std::vector<std::string> &f) {
    if (f[0] == "AAA" && f[1] == "2020-010") {
      f[17] = "-0.05"; // f9_vov_63d
    }
    return true;
  });
  const ScopedTempFile file("neg_vov", tsv);
  const auto panel = vrp::load_vrp_panel(file.path_string());
  ASSERT_FALSE(panel.has_value());
  EXPECT_EQ(panel.error().code(), ErrorCode::ParseError);
  EXPECT_NE(panel.error().to_string().find("f9_vov_63d"), std::string::npos);
}

// ── VrpTrain: F1 per-row t+21 rejection (round 2) ───────────────────────────

TEST(VrpTrainLoader, InteriorHolesRejectOnlyTheUnusableRows) {
  // Drop CCC sessions 158..162 -- surface holes straddling the labeled/tail
  // boundary. The labeled CCC rows at dates 154..157 lose their t+21 emitted
  // successors (successor counts 17..20 < 21) and must be rejected ONE BY
  // ONE; every other CCC row, and both other symbols, stay usable. Round 1
  // failed the whole run here.
  std::string tsv = make_synth_panel_tsv();
  tsv = transform_panel_rows(tsv, [](std::vector<std::string> &f) {
    return !(f[0] == "CCC" && f[1] >= "2020-158" && f[1] <= "2020-162");
  });
  const ScopedTempFile file("f1_holes", tsv);
  const auto panel = vrp::load_vrp_panel(file.path_string());
  ASSERT_TRUE(panel.has_value()) << panel.error().to_string();
  const auto obs = vrp::build_vrp_observations(*panel);
  ASSERT_TRUE(obs.has_value()) << obs.error().to_string();
  // AAA/BBB: 159 labeled rows each; CCC: 158 (date 158 was dropped).
  EXPECT_EQ(obs->n_labeled_rows, 159u + 159u + 158u);
  EXPECT_EQ(obs->n_rows_rejected_no_t21, 4u);
  EXPECT_EQ(obs->n_symbols_fully_rejected, 0u);
  EXPECT_EQ(obs->obs.size(), 159u + 159u + 154u);
  // And the panel still trains end-to-end, surfacing the counts in the
  // metrics meta lines (the brief's "rejection counts surfaced in metrics").
  const auto out = unique_temp_path("f1_holes_out", "");
  const auto report =
      vrp::run_vrp_train(make_synth_config(file.path_string(), out.string()));
  ASSERT_TRUE(report.has_value()) << report.error().to_string();
  const std::string metrics = read_file_bytes(report->metrics_path);
  EXPECT_NE(metrics.find("# n_rows_rejected_no_t21=4\n"), std::string::npos);
  EXPECT_NE(metrics.find("# n_symbols_fully_rejected=0\n"), std::string::npos);
  std::error_code ec;
  std::filesystem::remove_all(out, ec);
}

TEST(VrpTrainLoader, FullyRejectedSymbolIsCountedNotFatal) {
  // ZZZ carries 5 labeled rows and nothing after them: every ZZZ row lacks a
  // t+21 successor, so the SYMBOL contributes zero observations (counted as
  // fully rejected) while the run and the other symbols are untouched.
  std::string tsv = make_synth_panel_tsv();
  for (std::size_t i = 0; i < 5; ++i) {
    tsv += "ZZZ\t" + synth_date_string(i) + '\t' +
           std::to_string(kSynthBaseTs + static_cast<std::int64_t>(i) * kSynthDayNs) +
           "\t100\t0.2\t0.206\t0.25\t0.001875\t-3\t-3\t-3\t0.2\t0.01\t-0.05\t0.001\t0.02\t0\t"
           "0.1\n";
  }
  const ScopedTempFile file("f1_zzz", tsv);
  const auto panel = vrp::load_vrp_panel(file.path_string());
  ASSERT_TRUE(panel.has_value()) << panel.error().to_string();
  const auto obs = vrp::build_vrp_observations(*panel);
  ASSERT_TRUE(obs.has_value()) << obs.error().to_string();
  EXPECT_EQ(obs->n_rows_rejected_no_t21, 5u);
  EXPECT_EQ(obs->n_symbols_fully_rejected, 1u);
  EXPECT_EQ(obs->obs.size(), 159u * 3);
}

TEST(VrpTrainLoader, NonTailUnlabeledRowFailsClosed) {
  // A mid-sample NaN-label row (its t+21 successor EXISTS) is a panel
  // contract violation: scoring it with the final fold's models would hand
  // it a hindsight prediction. Fail closed, never skip (review minor).
  std::string tsv = make_synth_panel_tsv();
  tsv = transform_panel_rows(tsv, [](std::vector<std::string> &f) {
    if (f[0] == "AAA" && f[1] == "2020-050") {
      f[6] = "nan"; // rv_fwd_21d
      f[7] = "nan"; // label
    }
    return true;
  });
  const ScopedTempFile file("f1_midnan", tsv);
  const auto panel = vrp::load_vrp_panel(file.path_string());
  ASSERT_TRUE(panel.has_value()) << panel.error().to_string();
  const auto obs = vrp::build_vrp_observations(*panel);
  ASSERT_FALSE(obs.has_value());
  EXPECT_EQ(obs.error().code(), ErrorCode::InvalidArgument);
  EXPECT_NE(obs.error().to_string().find("non-tail unlabeled row"), std::string::npos);
}

// ── VrpTrain: label_end on the EMITTED axis (fix-2 review blocker revert) ───

TEST(VrpTrainLoader, SparseSymbolLabelEndIsConservativeEmittedAxisEnd) {
  const ScopedTempFile file("sparse_ccc", make_sparse_ccc_panel_tsv());
  const auto panel = vrp::load_vrp_panel(file.path_string());
  ASSERT_TRUE(panel.has_value()) << panel.error().to_string();
  const auto obs = vrp::build_vrp_observations(*panel);
  ASSERT_TRUE(obs.has_value()) << obs.error().to_string();
  // Labeled rows: AAA/BBB 159 each, CCC even sessions 0..158 = 80. The
  // per-row ADMISSION rule (21 same-symbol EMITTED rows) is unchanged:
  // CCC even sessions 0..136 = 69 admitted, 11 rejected-and-counted.
  EXPECT_EQ(obs->n_labeled_rows, 159u + 159u + 80u);
  EXPECT_EQ(obs->n_rows_rejected_no_t21, 11u);
  EXPECT_EQ(obs->n_symbols_fully_rejected, 0u);
  EXPECT_EQ(obs->obs.size(), 159u + 159u + 69u);
  // CCC's date-0 observation (uid = sorted symbol index + 1 = 3) records
  // the EMITTED-AXIS end: its own 21st emitted row (date 42 under the
  // every-other-session thinning), NOT the pooled-axis timestamp 21
  // sessions later. Emitted rows are a subset of the symbol's label-
  // generation bars, so this end is >= the true bar-axis t+21 end BY
  // CONSTRUCTION -- it may over-purge, it can never understate (the fix-2
  // review's demonstrated leak).
  bool found = false;
  for (const ResearchObservation &ob : obs->obs) {
    if (ob.uid == 3u && ob.decision_ts_ns == kSynthBaseTs) {
      EXPECT_EQ(ob.label_end_ts_ns, kSynthBaseTs + 42 * kSynthDayNs);
      found = true;
    }
  }
  EXPECT_TRUE(found);
  // CCC's 42-session span sits exactly AT the default cap (inclusive):
  // admitted, and it becomes the max span the embargo derives from.
  std::int64_t max_span = 0;
  for (const ResearchObservation &ob : obs->obs) {
    max_span = std::max(max_span, ob.label_end_ts_ns - ob.decision_ts_ns);
  }
  EXPECT_EQ(max_span, 42 * kSynthDayNs);
}

TEST(VrpTrainLoader, SparsePanelTrainsLeakFreeAndOverlappingRowsStayRejected) {
  const ScopedTempFile file("sparse_ccc_e2e", make_sparse_ccc_panel_tsv());
  const auto out = unique_temp_path("sparse_ccc_out", "");
  const auto report =
      vrp::run_vrp_train(make_synth_config(file.path_string(), out.string()));
  ASSERT_TRUE(report.has_value()) << report.error().to_string();
  ASSERT_GE(report->folds.size(), 1u);
  // Embargo = max ADMITTED emitted-axis span: CCC's 42 sessions.
  EXPECT_EQ(report->plan.spec.embargo_ns, 42 * kSynthDayNs);
  const auto &obs = report->observations.obs;
  // Leak-conservatism preserved: the independent audit passes, and no
  // admitted train row's TRUE label window overlaps its fold's test window.
  const Status audit = validate_research_plan_no_leakage(
      std::span<const ResearchObservation>{obs}, report->plan);
  EXPECT_TRUE(audit.has_value()) << audit.error().to_string();
  for (const auto &fold : report->plan.folds) {
    std::int64_t test_min = std::numeric_limits<std::int64_t>::max();
    for (const std::size_t t : fold.test_indices) {
      test_min = std::min(test_min, obs[t].decision_ts_ns);
    }
    for (const std::size_t i : fold.train_indices) {
      EXPECT_LE(obs[i].label_end_ts_ns, test_min) << "fold " << fold.id;
    }
    // A genuinely-overlapping row -- decided one session before the test
    // window, so its [t, t+21] label window crosses it -- is still kept out
    // of train (purged or embargoed, never admitted).
    bool overlap_seen = false;
    for (std::size_t i = 0; i < obs.size(); ++i) {
      if (obs[i].decision_ts_ns == test_min - kSynthDayNs) {
        overlap_seen = true;
        EXPECT_EQ(std::count(fold.train_indices.begin(), fold.train_indices.end(), i), 0)
            << "fold " << fold.id << " obs " << i;
      }
    }
    EXPECT_TRUE(overlap_seen) << "fold " << fold.id;
  }
  std::error_code ec;
  std::filesystem::remove_all(out, ec);
}

TEST(VrpTrainLoader, BarHoleySymbolLabelEndNeverUnderstatesTrueBarAxisEnd) {
  const ScopedTempFile file("bar_holey", make_bar_holey_panel_tsv());
  const auto panel = vrp::load_vrp_panel(file.path_string());
  ASSERT_TRUE(panel.has_value()) << panel.error().to_string();
  const auto obs = vrp::build_vrp_observations(*panel);
  ASSERT_TRUE(obs.has_value()) << obs.error().to_string();
  // HHH = sorted symbol index 3 -> uid 4. For every admitted HHH row the
  // recorded label_end must NOT understate the true bar-axis t+21 end (the
  // fixture's emitted list IS the bar axis, so bar q's true end is bar
  // q+21's timestamp). The pooled-axis end sat 10-11 sessions EARLY here --
  // the review's attack shape; here it is exact, and > pooled t+21.
  std::size_t n_hhh = 0;
  for (const ResearchObservation &ob : obs->obs) {
    if (ob.uid != 4u) {
      continue;
    }
    const auto d =
        static_cast<std::size_t>((ob.decision_ts_ns - kSynthBaseTs) / kSynthDayNs);
    const std::size_t q = 2 * (d / 3) + (d % 3); // d % 3 is 0 or 1 by construction
    const std::int64_t true_end =
        kSynthBaseTs +
        static_cast<std::int64_t>(bar_holey_session_of(q + 21)) * kSynthDayNs;
    EXPECT_EQ(ob.label_end_ts_ns, true_end) << "HHH bar " << q;
    EXPECT_GT(ob.label_end_ts_ns, ob.decision_ts_ns + 21 * kSynthDayNs) << "HHH bar " << q;
    ++n_hhh;
  }
  // All 99 labeled HHH bars have 21 emitted successors and 31/32-session
  // spans -- inside the default cap, so every one is admitted.
  EXPECT_EQ(n_hhh, 99u);
}

TEST(VrpTrainLoader, BarHoleySymbolCannotAdmitALeakingTrainRow) {
  const ScopedTempFile file("bar_holey_e2e", make_bar_holey_panel_tsv());
  const auto out = unique_temp_path("bar_holey_out", "");
  const auto report =
      vrp::run_vrp_train(make_synth_config(file.path_string(), out.string()));
  ASSERT_TRUE(report.has_value()) << report.error().to_string();
  ASSERT_GE(report->folds.size(), 1u);
  const auto &obs = report->observations.obs;
  // The review's acceptance check in miniature: every admitted HHH train
  // row's TRUE bar-axis label end stays <= its fold's earliest test
  // decision. The pooled-axis semantics admitted HHH rows decided within
  // (test_min - 32, test_min - 21) sessions whose true windows crossed the
  // test boundary -- exactly the demonstrated SP100 leak.
  for (const auto &fold : report->plan.folds) {
    std::int64_t test_min = std::numeric_limits<std::int64_t>::max();
    for (const std::size_t t : fold.test_indices) {
      test_min = std::min(test_min, obs[t].decision_ts_ns);
    }
    for (const std::size_t i : fold.train_indices) {
      if (obs[i].uid != 4u) {
        continue;
      }
      const auto d =
          static_cast<std::size_t>((obs[i].decision_ts_ns - kSynthBaseTs) / kSynthDayNs);
      const std::size_t q = 2 * (d / 3) + (d % 3);
      const std::int64_t true_end =
          kSynthBaseTs +
          static_cast<std::int64_t>(bar_holey_session_of(q + 21)) * kSynthDayNs;
      EXPECT_LE(true_end, test_min) << "fold " << fold.id << " HHH bar " << q;
    }
  }
  // Embargo derives from the max ADMITTED emitted-axis span -- HHH's 32
  // sessions -- never the pooled-axis fiction of 21.
  EXPECT_EQ(report->plan.spec.embargo_ns, 32 * kSynthDayNs);
  std::error_code ec;
  std::filesystem::remove_all(out, ec);
}

// ── VrpTrain: span-cap reject-and-count (fix-2 remedy, trainability leg) ────

TEST(VrpTrainLoader, SpanCapRejectsAndCountsUltraSparseRows) {
  const ScopedTempFile file("span_cap", make_span_cap_sss_panel_tsv());
  const auto panel = vrp::load_vrp_panel(file.path_string());
  ASSERT_TRUE(panel.has_value()) << panel.error().to_string();
  const auto obs = vrp::build_vrp_observations(*panel);
  ASSERT_TRUE(obs.has_value()) << obs.error().to_string();
  // SSS: 39 labeled rows, every one WITH a t+21 emitted successor -- but
  // each window spans 63 pooled sessions > the default 42 cap: rejected
  // one by one and counted, so the symbol cannot poison the global embargo
  // (round-1's DUK shape: one sparse symbol embargoed ~70% of the corpus).
  EXPECT_EQ(obs->n_labeled_rows, 477u + 39u);
  EXPECT_EQ(obs->n_rows_rejected_no_t21, 0u);
  EXPECT_EQ(obs->n_rows_rejected_span_cap, 39u);
  EXPECT_EQ(obs->n_symbols_fully_rejected, 1u);
  EXPECT_EQ(obs->obs.size(), 477u);
  std::int64_t max_span = 0;
  for (const ResearchObservation &ob : obs->obs) {
    max_span = std::max(max_span, ob.label_end_ts_ns - ob.decision_ts_ns);
  }
  EXPECT_EQ(max_span, 21 * kSynthDayNs);
  // Cap boundary is inclusive: raising it to exactly 63 admits SSS whole.
  const auto obs63 = vrp::build_vrp_observations(*panel, 63);
  ASSERT_TRUE(obs63.has_value()) << obs63.error().to_string();
  EXPECT_EQ(obs63->n_rows_rejected_span_cap, 0u);
  EXPECT_EQ(obs63->obs.size(), 477u + 39u);
  // A cap below the 21-session horizon would reject every row: fail closed.
  const auto bad = vrp::build_vrp_observations(*panel, 20);
  ASSERT_FALSE(bad.has_value());
  EXPECT_EQ(bad.error().code(), ErrorCode::InvalidArgument);
  EXPECT_NE(bad.error().to_string().find("below the 21-session horizon"), std::string::npos);
}

TEST(VrpTrainLoader, SpanCapCounterIsPersistedAndEmbargoFollowsTheCap) {
  const ScopedTempFile file("span_cap_e2e", make_span_cap_sss_panel_tsv());
  const auto out = unique_temp_path("span_cap_out", "");
  const auto report =
      vrp::run_vrp_train(make_synth_config(file.path_string(), out.string()));
  ASSERT_TRUE(report.has_value()) << report.error().to_string();
  EXPECT_EQ(report->observations.n_rows_rejected_span_cap, 39u);
  // Every admitted symbol is dense: the embargo stays the bare horizon.
  EXPECT_EQ(report->plan.spec.embargo_ns, 21 * kSynthDayNs);
  const std::string metrics = read_file_bytes(report->metrics_path);
  EXPECT_NE(metrics.find("# n_rows_rejected_span_cap=39\n"), std::string::npos);
  // Raising the cap via config admits SSS and the embargo follows the max
  // ADMITTED span: the cap bounds the embargo, and the embargo bounds the
  // purge/embargo attrition that zeroed the SP100 folds in round 1.
  vrp::VrpTrainConfig cfg = make_synth_config(
      file.path_string(), unique_temp_path("span_cap_out63", "").string());
  cfg.max_label_span_sessions = 63;
  const auto wide = vrp::run_vrp_train(cfg);
  ASSERT_TRUE(wide.has_value()) << wide.error().to_string();
  EXPECT_EQ(wide->observations.n_rows_rejected_span_cap, 0u);
  EXPECT_EQ(wide->plan.spec.embargo_ns, 63 * kSynthDayNs);
  std::error_code ec;
  std::filesystem::remove_all(out, ec);
  std::filesystem::remove_all(cfg.out_dir, ec);
}

// ── VrpTrain: QLIKE in variance levels (hand values) ────────────────────────

TEST(VrpTrainMath, QlikeHandValues) {
  // L(F, P) = P/F - ln(P/F) - 1, F = forecast VARIANCE, P = proxy VARIANCE.
  EXPECT_NEAR(vrp::vrp_qlike(1.0, 1.0), 0.0, 1e-15);
  EXPECT_NEAR(vrp::vrp_qlike(2.0, 1.0), 0.5 - std::log(0.5) - 1.0, 1e-15);
  EXPECT_NEAR(vrp::vrp_qlike(1.0, 2.0), 2.0 - std::log(2.0) - 1.0, 1e-15);
  EXPECT_NEAR(vrp::vrp_qlike(0.04, 0.09), 2.25 - std::log(2.25) - 1.0, 1e-12);
  // Asymmetric: an under-forecast (F < P) costs more than the mirrored
  // over-forecast -- the property that makes QLIKE the vol-forecast loss.
  EXPECT_GT(vrp::vrp_qlike(1.0, 2.0), vrp::vrp_qlike(2.0, 1.0));
}

TEST(VrpTrainMath, RetransformAppliesLognormalCorrectionAndClips) {
  // exp(s^2/2) retransformation: ln F = mu, residual var s2 -> F = exp(mu + s2/2).
  const double mu = std::log(0.04);
  EXPECT_NEAR(vrp::vrp_retransform_clip(mu, 0.0, 0.0, 1.0), 0.04, 1e-15);
  EXPECT_NEAR(vrp::vrp_retransform_clip(mu, 0.5, 0.0, 1.0), 0.04 * std::exp(0.25), 1e-12);
  // Insanity filter: the forecast never leaves the train-window label range.
  EXPECT_DOUBLE_EQ(vrp::vrp_retransform_clip(std::log(9.0), 0.0, 0.01, 0.16), 0.16);
  EXPECT_DOUBLE_EQ(vrp::vrp_retransform_clip(std::log(1e-9), 0.0, 0.01, 0.16), 0.01);
}

// ── VrpTrain: fold-plan auto-scaling (round 2, item 3) ──────────────────────

TEST(VrpTrainMath, DeriveWalkForwardKeepsProductionPlanWhenDeep) {
  const vrp::VrpWalkForwardCfg deep = vrp::derive_vrp_walk_forward(400);
  EXPECT_EQ(deep.min_train_sessions, 252u);
  EXPECT_EQ(deep.test_sessions, 63u);
  EXPECT_EQ(deep.step_sessions, 63u);
  // Boundary: exactly min_train + test groups still carries the full plan.
  const vrp::VrpWalkForwardCfg edge = vrp::derive_vrp_walk_forward(315);
  EXPECT_EQ(edge.min_train_sessions, 252u);
  EXPECT_EQ(edge.test_sessions, 63u);
}

TEST(VrpTrainMath, DeriveWalkForwardScalesToThinHistoryAndKeepsFailClosedLine) {
  // The 244-session panel shape: 223 labeled groups -> 84/37/37.
  const vrp::VrpWalkForwardCfg thin = vrp::derive_vrp_walk_forward(223);
  EXPECT_EQ(thin.min_train_sessions, 84u);
  EXPECT_EQ(thin.test_sessions, 37u);
  EXPECT_EQ(thin.step_sessions, 37u);
  // Floors 84/21/21: the smallest trainable depth is exactly 105 groups.
  const vrp::VrpWalkForwardCfg floor = vrp::derive_vrp_walk_forward(105);
  EXPECT_EQ(floor.min_train_sessions, 84u);
  EXPECT_EQ(floor.test_sessions, 21u);
  EXPECT_EQ(floor.step_sessions, 21u);
  // One group below the floor no longer fits: auto-scaling moves the
  // fail-closed line, it never removes it (make_vrp_plan rejects the plan).
  const vrp::VrpWalkForwardCfg below = vrp::derive_vrp_walk_forward(104);
  EXPECT_GT(below.min_train_sessions + below.test_sessions, 104u);
}

// ── VrpTrain: per-asset standardization is train-fold-only ──────────────────

TEST_F(VrpTrainPipelineTest, StandardizationStatsComeFromTrainFoldRowsOnly) {
  ASSERT_FALSE(report_->folds.empty());
  const auto &fold = report_->folds.front();
  ASSERT_FALSE(fold.train_rows.empty());
  ASSERT_FALSE(fold.test_rows.empty());

  const vrp::VrpStandardization stz =
      vrp::compute_asset_standardization(report_->panel, fold.train_rows);

  // Perturb a TEST-fold row's features; recompute -> train stats + z unchanged.
  vrp::VrpPanel perturbed = report_->panel;
  for (double &v : perturbed.rows[fold.test_rows.front()].f) {
    v = std::isnan(v) ? v : v + 100.0;
  }
  const vrp::VrpStandardization stz2 =
      vrp::compute_asset_standardization(perturbed, fold.train_rows);

  for (std::size_t s = 0; s < stz.mean.size(); ++s) {
    for (std::size_t f = 0; f < kVrpFeatureCount; ++f) {
      EXPECT_EQ(stz.mean[s][f], stz2.mean[s][f]) << "sym " << s << " feat " << f;
      EXPECT_EQ(stz.sd[s][f], stz2.sd[s][f]) << "sym " << s << " feat " << f;
    }
  }
  for (const std::size_t r : fold.train_rows) {
    for (std::size_t f = 0; f < kVrpFeatureCount; ++f) {
      EXPECT_EQ(vrp::standardized_feature(report_->panel, stz, r, f),
                vrp::standardized_feature(perturbed, stz2, r, f));
    }
  }
  // And perturbing a TRAIN row does change its own asset's stats (the check
  // above is not vacuously true).
  vrp::VrpPanel perturbed_train = report_->panel;
  perturbed_train.rows[fold.train_rows.front()].f[0] += 100.0;
  const vrp::VrpStandardization stz3 =
      vrp::compute_asset_standardization(perturbed_train, fold.train_rows);
  const std::size_t sym = report_->panel.row_symbol[fold.train_rows.front()];
  EXPECT_NE(stz.mean[sym][0], stz3.mean[sym][0]);
}

// ── VrpTrain: purge/embargo structural guarantee ────────────────────────────

TEST_F(VrpTrainPipelineTest, NoTrainLabelIntervalOverlapsAnyTestObservation) {
  const auto &obs = report_->observations.obs;
  ASSERT_FALSE(report_->plan.folds.empty());
  for (const auto &fold : report_->plan.folds) {
    ASSERT_FALSE(fold.train_indices.empty());
    ASSERT_FALSE(fold.test_indices.empty());
    std::int64_t test_min_decision = std::numeric_limits<std::int64_t>::max();
    for (const std::size_t t : fold.test_indices) {
      test_min_decision = std::min(test_min_decision, obs[t].decision_ts_ns);
    }
    for (const std::size_t i : fold.train_indices) {
      // The train row's [t, t+21] label interval must be fully resolved before
      // the earliest test decision -- so it cannot overlap ANY test
      // observation's interval (test intervals start at their decisions).
      EXPECT_LE(obs[i].label_end_ts_ns, test_min_decision)
          << "fold " << fold.id << " train obs " << i;
    }
  }
  // Independent audit: the stored plan is exactly the canonical leak-free plan.
  const Status audit =
      validate_research_plan_no_leakage(std::span<const ResearchObservation>{obs}, report_->plan);
  EXPECT_TRUE(audit.has_value()) << audit.error().to_string();
}

// ── VrpTrain: planted linear relation is recovered ──────────────────────────

TEST_F(VrpTrainPipelineTest, BaselineBeatsMeanForecastOnQlike) {
  ASSERT_FALSE(report_->folds.empty());
  double baseline_sum = 0.0;
  double mean_sum = 0.0;
  for (const auto &fold : report_->folds) {
    EXPECT_TRUE(std::isfinite(fold.qlike_baseline)) << "fold " << fold.fold_id;
    EXPECT_TRUE(std::isfinite(fold.qlike_mean_forecast)) << "fold " << fold.fold_id;
    baseline_sum += fold.qlike_baseline;
    mean_sum += fold.qlike_mean_forecast;
  }
  EXPECT_LT(baseline_sum, mean_sum);
}

TEST_F(VrpTrainPipelineTest, FoldMetricsAreFiniteAndPopulated) {
  for (const auto &fold : report_->folds) {
    EXPECT_GT(fold.n_train, 0u);
    EXPECT_GT(fold.n_test, 0u);
    EXPECT_TRUE(std::isfinite(fold.qlike_gbt));
    EXPECT_TRUE(std::isfinite(fold.ic_baseline));
    EXPECT_TRUE(std::isfinite(fold.ic_gbt));
    EXPECT_GT(fold.train_var_max, fold.train_var_min);
    // The scored GBT variance forecast stays inside the insanity-clip range
    // (and therefore strictly positive) on EVERY fold, thin or not.
    EXPECT_GE(fold.gbt_test_forecast_min, fold.train_var_min);
    EXPECT_LE(fold.gbt_test_forecast_max, fold.train_var_max);
    EXPECT_GT(fold.gbt_test_forecast_min, 0.0);
  }
}

// ── VrpTrain: retransform + insanity clip on the baseline ───────────────────

TEST_F(VrpTrainPipelineTest, BaselineForecastNeverExceedsTrainWindowLabelMax) {
  for (const auto &fold : report_->folds) {
    EXPECT_TRUE(std::isfinite(fold.baseline_test_forecast_max));
    EXPECT_LE(fold.baseline_test_forecast_max, fold.train_var_max) << "fold " << fold.fold_id;
    EXPECT_GE(fold.baseline_test_forecast_max, fold.train_var_min) << "fold " << fold.fold_id;
  }
}

// ── VrpTrain: determinism -- identical model files across two runs ──────────

TEST_F(VrpTrainPipelineTest, FixedMasterSeedProducesIdenticalModelFileBytes) {
  const std::filesystem::path out2 = unique_temp_path("determinism_out", "");
  const auto report2 =
      vrp::run_vrp_train(make_synth_config(panel_file_->path_string(), out2.string()));
  ASSERT_TRUE(report2.has_value()) << report2.error().to_string();
  const std::string gbt1 = read_file_bytes(report_->gbt_model_path);
  const std::string gbt2 = read_file_bytes(report2->gbt_model_path);
  ASSERT_FALSE(gbt1.empty());
  EXPECT_EQ(gbt1, gbt2);
  const std::string lin1 = read_file_bytes(report_->baseline_model_path);
  const std::string lin2 = read_file_bytes(report2->baseline_model_path);
  ASSERT_FALSE(lin1.empty());
  EXPECT_EQ(lin1, lin2);
  // The signal, metrics, and fold-stats sidecar files are deterministic too.
  EXPECT_EQ(read_file_bytes(report_->signal_path), read_file_bytes(report2->signal_path));
  EXPECT_EQ(read_file_bytes(report_->metrics_path), read_file_bytes(report2->metrics_path));
  EXPECT_EQ(read_file_bytes(report_->fold_stats_path),
            read_file_bytes(report2->fold_stats_path));
  std::error_code ec;
  std::filesystem::remove_all(out2, ec);
}

// ── VrpTrain: serialized models load back through the seam ──────────────────

TEST_F(VrpTrainPipelineTest, SerializedModelsRoundTripThroughTheSeamLoaders) {
  auto gbt = load_gbt_fair_vol_model(report_->gbt_model_path.string());
  ASSERT_TRUE(gbt.has_value()) << gbt.error().to_string();
  EXPECT_EQ((*gbt)->feature_schema(), kVrpFeatureSchemaV1);

  auto lin = load_linear_fair_vol_model(report_->baseline_model_path.string());
  ASSERT_TRUE(lin.has_value()) << lin.error().to_string();
  EXPECT_EQ((*lin)->feature_schema(), kVrpFeatureSchemaV1);

  // Both predict finite values on a z-scored-shaped input row.
  const std::array<double, 10> x{0.1, -0.2, 0.3, 0.0, 0.5, -0.5, 0.0, 0.1, 0.0, 0.2};
  std::array<double, 1> y{};
  ASSERT_TRUE((*gbt)->predict(x, 1, y).has_value());
  EXPECT_TRUE(std::isfinite(y[0]));
  ASSERT_TRUE((*lin)->predict(x, 1, y).has_value());
  EXPECT_TRUE(std::isfinite(y[0]));
}

// ── VrpTrain: frozen vrp_signal_v1 output contract ──────────────────────────

TEST_F(VrpTrainPipelineTest, SignalFileFollowsFrozenContract) {
  std::ifstream in(report_->signal_path);
  ASSERT_TRUE(in.is_open());
  std::string line;
  ASSERT_TRUE(std::getline(in, line));
  EXPECT_EQ(line, "# schema=vrp_signal_v1");
  ASSERT_TRUE(std::getline(in, line));
  EXPECT_EQ(line, "symbol\tdate\tpred_label\tpred_edge_norm\tvov_63d");
  std::size_t n_rows = 0;
  while (std::getline(in, line)) {
    if (line.empty()) {
      continue;
    }
    ++n_rows;
    // Exactly five tab-separated fields per row.
    std::size_t tabs = 0;
    for (const char c : line) {
      tabs += (c == '\t') ? 1u : 0u;
    }
    EXPECT_EQ(tabs, 4u) << line;
  }
  // Every test-fold observation appears exactly once, plus the NaN-label tail
  // rows (predict-time rows scored by the final fold's models).
  std::size_t n_test_obs = 0;
  for (const auto &fold : report_->plan.folds) {
    n_test_obs += fold.test_indices.size();
  }
  std::size_t n_tail_rows = 0;
  for (const auto &row : report_->panel.rows) {
    n_tail_rows += std::isnan(row.label) ? 1u : 0u;
  }
  EXPECT_EQ(n_rows, n_test_obs + n_tail_rows);
}

TEST_F(VrpTrainPipelineTest, MetricsFileCarriesRejectionMetaLines) {
  // Hole-free shared fixture: counters present and zero (the F1 fixture test
  // asserts the nonzero path).
  const std::string bytes = read_file_bytes(report_->metrics_path);
  EXPECT_NE(bytes.find("# n_labeled_rows=477\n"), std::string::npos);
  EXPECT_NE(bytes.find("# n_rows_rejected_no_t21=0\n"), std::string::npos);
  EXPECT_NE(bytes.find("# n_rows_rejected_span_cap=0\n"), std::string::npos);
  EXPECT_NE(bytes.find("# n_symbols_fully_rejected=0\n"), std::string::npos);
}

TEST_F(VrpTrainPipelineTest, MetricsFileCarriesPerFoldClipAndPurgeMetaLines) {
  // Round-2 review major 3: the GBT QLIKE-path clip count + post-clip
  // extrema -- and the purge/embargo train-row losses (major 2's counter
  // ask) -- are persisted per fold via the same `# key=value` mechanism as
  // the F1 counters, so a reader of the artifacts can tell a saturated clip
  // from a healthy forecaster. Values must match the in-memory report.
  const std::string bytes = read_file_bytes(report_->metrics_path);
  ASSERT_EQ(report_->plan.folds.size(), report_->folds.size());
  for (std::size_t i = 0; i < report_->folds.size(); ++i) {
    const auto &m = report_->folds[i];
    const auto &pf = report_->plan.folds[i];
    const std::string p = "# fold_" + std::to_string(m.fold_id) + "_";
    const auto has = [&](const std::string &line) {
      EXPECT_NE(bytes.find(line), std::string::npos) << line;
    };
    has(p + "n_train_purged=" + std::to_string(pf.purged_indices.size()) + "\n");
    has(p + "n_train_embargoed=" + std::to_string(pf.embargoed_indices.size()) + "\n");
    has(p + "n_gbt_forecast_clipped=" + std::to_string(m.n_gbt_forecast_clipped) + "\n");
    has(p + "gbt_test_forecast_min=" + vrp::detail::fmt_double(m.gbt_test_forecast_min) +
        "\n");
    has(p + "gbt_test_forecast_max=" + vrp::detail::fmt_double(m.gbt_test_forecast_max) +
        "\n");
  }
}

// ── VrpTrain: F2 -- emitted vov_63d is ALWAYS finite (round 2) ──────────────

TEST_F(VrpTrainPipelineTest, WarmupNaNVovImputesTrainFoldMeanAndParsesUnderFrozenLoader) {
  // Plant NaN f9 inside a test window (dates 095..100, fold 0 tests 090..109
  // under the 90/20/20 synth walk) AND on tail rows (dates 170+): round 1
  // passed f9 through raw, so these rows made the whole signal file
  // unloadable (the frozen loader fail-closes on non-finite vov_63d).
  std::string tsv = make_synth_panel_tsv();
  tsv = transform_panel_rows(tsv, [](std::vector<std::string> &f) {
    if ((f[1] >= "2020-095" && f[1] <= "2020-100") || f[1] >= "2020-170") {
      f[17] = "nan"; // f9_vov_63d
    }
    return true;
  });
  const ScopedTempFile file("f2_vov", tsv);
  const auto out = unique_temp_path("f2_out", "");
  const auto report =
      vrp::run_vrp_train(make_synth_config(file.path_string(), out.string()));
  ASSERT_TRUE(report.has_value()) << report.error().to_string();

  // Parse under the UNMODIFIED frozen loader (vol_edge.hpp, owned by the
  // vol-edge lane): it enforces finite vov_63d >= 0 on every row.
  const auto rows = load_vrp_signal_v1(report->signal_path.string());
  ASSERT_TRUE(rows.has_value()) << rows.error().to_string();
  ASSERT_FALSE(rows->empty());
  for (const auto &row : *rows) {
    EXPECT_TRUE(std::isfinite(row.vov_63d));
    EXPECT_GE(row.vov_63d, 0.0);
  }

  // A tail row's imputed value is exactly the FINAL fold's per-asset
  // train-window f9 mean (symbols are sorted, so AAA is index 0).
  const vrp::VrpStandardization stz = vrp::compute_asset_standardization(
      report->panel, std::span<const std::size_t>{report->folds.back().train_rows});
  bool found = false;
  for (const auto &row : *rows) {
    if (row.symbol == "AAA" && row.date == "2020-179") {
      EXPECT_DOUBLE_EQ(row.vov_63d, stz.mean[0][9]);
      found = true;
    }
  }
  EXPECT_TRUE(found);
  std::error_code ec;
  std::filesystem::remove_all(out, ec);
}

TEST_F(VrpTrainPipelineTest, AllNaNVovSymbolEmitsDocumentedZeroFallback) {
  // A symbol with ZERO finite f9 observations in every scoring fold's train
  // window: the per-asset imputation mean is the DOCUMENTED 0.0 fallback --
  // finite and >= 0, so the frozen loader accepts it, and the vov_floor at
  // sizing owns the degenerate value downstream (review minor, round 2).
  std::string tsv = make_synth_panel_tsv();
  tsv = transform_panel_rows(tsv, [](std::vector<std::string> &f) {
    if (f[0] == "AAA") {
      f[17] = "nan"; // f9_vov_63d
    }
    return true;
  });
  const ScopedTempFile file("allnan_vov", tsv);
  const auto out = unique_temp_path("allnan_vov_out", "");
  const auto report =
      vrp::run_vrp_train(make_synth_config(file.path_string(), out.string()));
  ASSERT_TRUE(report.has_value()) << report.error().to_string();
  const auto rows = load_vrp_signal_v1(report->signal_path.string());
  ASSERT_TRUE(rows.has_value()) << rows.error().to_string();
  bool saw_aaa = false;
  for (const auto &row : *rows) {
    if (row.symbol == "AAA") {
      saw_aaa = true;
      EXPECT_EQ(row.vov_63d, 0.0);
    }
  }
  EXPECT_TRUE(saw_aaa);
  std::error_code ec;
  std::filesystem::remove_all(out, ec);
}

// ── VrpTrain: fold-plan auto-scaling end-to-end (round 2, item 3) ───────────

TEST_F(VrpTrainPipelineTest, AutoScaledDefaultsTrainA244SessionPanelEndToEnd) {
  // 244 sessions -> 223 labeled groups. The production 252/63/63 defaults
  // cannot fit; with walk_auto (the CLI default when no walk flag is given)
  // the derived 84/37/37 plan must yield >= 1 valid purged fold and a report
  // (library-level twin of "CLI with DEFAULT flags exits 0").
  const ScopedTempFile file("thin244", make_synth_panel_tsv(244));
  const auto out = unique_temp_path("thin244_out", "");
  vrp::VrpTrainConfig cfg = make_synth_config(file.path_string(), out.string());
  cfg.walk = vrp::VrpWalkForwardCfg{
      .min_train_sessions = 252, .test_sessions = 63, .step_sessions = 63};
  cfg.walk_auto = true;
  const auto report = vrp::run_vrp_train(cfg);
  ASSERT_TRUE(report.has_value()) << report.error().to_string();
  EXPECT_GE(report->folds.size(), 1u);
  EXPECT_EQ(report->plan.spec.min_train_groups, 84u);
  EXPECT_EQ(report->plan.spec.test_groups, 37u);
  const Status audit = validate_research_plan_no_leakage(
      std::span<const ResearchObservation>{report->observations.obs}, report->plan);
  EXPECT_TRUE(audit.has_value()) << audit.error().to_string();

  // The SAME panel with walk_auto off still fails closed on the requested
  // production plan -- auto-scaling is opt-out via any explicit walk flag.
  vrp::VrpTrainConfig strict = cfg;
  strict.walk_auto = false;
  strict.out_dir = unique_temp_path("thin244_strict", "").string();
  const auto strict_r = vrp::run_vrp_train(strict);
  ASSERT_FALSE(strict_r.has_value());
  EXPECT_EQ(strict_r.error().code(), ErrorCode::InvalidArgument);
  std::error_code ec;
  std::filesystem::remove_all(out, ec);
}

TEST_F(VrpTrainPipelineTest, GenuinelyUnusablePanelStillFailsClosedUnderAutoScaling) {
  // 70 sessions -> 49 labeled groups: below the 84+21 floor, so even the
  // scaled plan cannot fit and the make_vrp_plan fail-closed error survives
  // (the CLI maps it to exit 1).
  const ScopedTempFile file("thin70", make_synth_panel_tsv(70));
  const auto out = unique_temp_path("thin70_out", "");
  vrp::VrpTrainConfig cfg = make_synth_config(file.path_string(), out.string());
  cfg.walk = vrp::VrpWalkForwardCfg{
      .min_train_sessions = 252, .test_sessions = 63, .step_sessions = 63};
  cfg.walk_auto = true;
  const auto report = vrp::run_vrp_train(cfg);
  ASSERT_FALSE(report.has_value());
  EXPECT_EQ(report.error().code(), ErrorCode::InvalidArgument);
  EXPECT_NE(report.error().to_string().find("insufficient decision groups"),
            std::string::npos);
}

// ── VrpTrain: GBT insanity clip on a thin regime-break fold (item 4) ────────

TEST_F(VrpTrainPipelineTest, ThinFoldGbtForecastIsClippedAndQlikeStaysFinite) {
  const ScopedTempFile file("regime_break", make_regime_break_panel_tsv());
  const auto out = unique_temp_path("regime_out", "");
  vrp::VrpTrainConfig cfg = make_synth_config(file.path_string(), out.string());
  const auto report = vrp::run_vrp_train(cfg);
  ASSERT_TRUE(report.has_value()) << report.error().to_string();
  ASSERT_FALSE(report->folds.empty());
  const auto &fold = report->folds.front();
  // The raw GBT-implied variance forecast goes <= 0 on the regime-broken
  // test rows; the insanity clip must fire, keep every scored forecast
  // inside the train label range (hence > 0), and keep QLIKE sane -- round 1
  // floored at 1e-10 and reported QLIKE in the 1e6..3e7 range here.
  EXPECT_GE(fold.n_gbt_forecast_clipped, 1u);
  EXPECT_GE(fold.gbt_test_forecast_min, fold.train_var_min);
  EXPECT_LE(fold.gbt_test_forecast_max, fold.train_var_max);
  EXPECT_GT(fold.gbt_test_forecast_min, 0.0);
  EXPECT_TRUE(std::isfinite(fold.qlike_gbt));
  EXPECT_LT(fold.qlike_gbt, 100.0);
  // The NONZERO clip count is persisted in the metrics meta lines (round-2
  // review major 3) -- a reader of the artifacts alone sees the saturation.
  const std::string metrics = read_file_bytes(report->metrics_path);
  EXPECT_NE(metrics.find("# fold_0_n_gbt_forecast_clipped=" +
                         std::to_string(fold.n_gbt_forecast_clipped) + "\n"),
            std::string::npos);
  std::error_code ec;
  std::filesystem::remove_all(out, ec);
}

// ── VrpTrain: per-fold stats sidecar (round 2, item 5) ──────────────────────

TEST_F(VrpTrainPipelineTest, FoldStatsSidecarRoundTripsByteStable) {
  ASSERT_FALSE(report_->fold_stats.empty());
  ASSERT_EQ(report_->fold_stats.size(), report_->folds.size());
  const auto loaded = vrp::load_vrp_fold_stats(report_->fold_stats_path);
  ASSERT_TRUE(loaded.has_value()) << loaded.error().to_string();
  EXPECT_EQ(*loaded, report_->fold_stats);
  const std::filesystem::path p2 = unique_temp_path("sidecar_rt", ".tsv");
  ASSERT_TRUE(vrp::save_vrp_fold_stats(std::span<const vrp::VrpFoldStats>{*loaded}, p2)
                  .has_value());
  EXPECT_EQ(read_file_bytes(report_->fold_stats_path), read_file_bytes(p2));
  std::error_code ec;
  std::filesystem::remove(p2, ec);
}

TEST_F(VrpTrainPipelineTest, SidecarLoaderFailsClosedOnMutations) {
  const std::string bytes = read_file_bytes(report_->fold_stats_path);
  const auto reject = [&](std::string_view find, std::string_view replace) {
    std::string mutated = bytes;
    const std::size_t pos = mutated.find(find);
    ASSERT_NE(pos, std::string::npos) << find;
    mutated.replace(pos, find.size(), replace);
    const ScopedTempFile f("sidecar_bad", mutated);
    const auto loaded = vrp::load_vrp_fold_stats(f.path_string());
    ASSERT_FALSE(loaded.has_value()) << "mutation '" << find << "' -> '" << replace
                                     << "' was accepted";
    EXPECT_EQ(loaded.error().code(), ErrorCode::ParseError);
  };
  reject("# schema=vrp_fold_stats_v1", "# schema=vrp_fold_stats_v9");
  reject("\tbaseline_s2\t", "\tbaseline_zz\t");   // fold field renamed
  reject("\tf9_sd\t", "\tf9_zz\t");               // asset block truncated field
}

// Fail-closed probe shared by the sidecar-loader reject tests below (review
// minor, round 2: structural mutations no prior test would catch).
void expect_sidecar_parse_error(const std::string &content, std::string_view needle) {
  const ScopedTempFile f("sidecar_reject", content);
  const auto loaded = vrp::load_vrp_fold_stats(f.path_string());
  ASSERT_FALSE(loaded.has_value()) << "accepted a sidecar that should fail: " << needle;
  EXPECT_EQ(loaded.error().code(), ErrorCode::ParseError);
  EXPECT_NE(loaded.error().to_string().find(needle), std::string::npos)
      << loaded.error().to_string();
}

// Newline split (no trailing empty line) for sidecar text surgery.
[[nodiscard]] std::vector<std::string> split_lines(const std::string &bytes) {
  std::vector<std::string> lines;
  std::size_t start = 0;
  while (start < bytes.size()) {
    std::size_t end = bytes.find('\n', start);
    if (end == std::string::npos) {
      end = bytes.size();
    }
    lines.push_back(bytes.substr(start, end - start));
    start = end + 1;
  }
  return lines;
}

TEST_F(VrpTrainPipelineTest, SidecarLoaderRejectsHeaderOnlyFile) {
  expect_sidecar_parse_error(
      "# schema=vrp_fold_stats_v1\nfold_id\tkind\tsymbol\tname\tvalue\n", "no data rows");
}

TEST_F(VrpTrainPipelineTest, SidecarLoaderRejectsFieldCountMismatch) {
  std::string bytes = read_file_bytes(report_->fold_stats_path);
  bytes += "9\tfold\t-\tn_train\n"; // 4 fields, not the canonical 5
  expect_sidecar_parse_error(bytes, "expected 5 fields");
}

TEST_F(VrpTrainPipelineTest, SidecarLoaderRejectsTruncatedAssetBlock) {
  const std::string bytes = read_file_bytes(report_->fold_stats_path);
  // Drop the final data row (the last fold's last symbol loses f9_sd).
  ASSERT_GT(bytes.size(), 2u);
  const std::size_t cut = bytes.find_last_of('\n', bytes.size() - 2);
  ASSERT_NE(cut, std::string::npos);
  expect_sidecar_parse_error(bytes.substr(0, cut + 1), "incomplete asset block");
}

TEST_F(VrpTrainPipelineTest, SidecarLoaderRejectsDescendingFoldIds) {
  ASSERT_GE(report_->fold_stats.size(), 2u);
  const std::vector<std::string> lines = split_lines(read_file_bytes(report_->fold_stats_path));
  // Reassemble with fold 1's whole block BEFORE fold 0's.
  std::string mutated = lines[0] + "\n" + lines[1] + "\n";
  for (const std::string_view prefix : {std::string_view{"1\t"}, std::string_view{"0\t"}}) {
    for (std::size_t i = 2; i < lines.size(); ++i) {
      if (lines[i].starts_with(prefix)) {
        mutated += lines[i];
        mutated += '\n';
      }
    }
  }
  expect_sidecar_parse_error(mutated, "fold ids not ascending");
}

TEST_F(VrpTrainPipelineTest, SidecarLoaderRejectsDuplicateFoldIdBlock) {
  ASSERT_GE(report_->fold_stats.size(), 2u);
  std::string bytes = read_file_bytes(report_->fold_stats_path);
  // Relabel every fold-1 row as fold 0: a second fold-0 block right after
  // the first one -- fails closed (the loader sees a 'fold' row where only
  // 'asset' rows may continue the open block).
  std::size_t pos = 0;
  while ((pos = bytes.find("\n1\t", pos)) != std::string::npos) {
    bytes[pos + 1] = '0';
    ++pos;
  }
  expect_sidecar_parse_error(bytes, "asset rows out of canonical order");
}

TEST_F(VrpTrainPipelineTest, SidecarLoaderRejectsFoldBlockWithoutAssetRows) {
  const std::vector<std::string> lines = split_lines(read_file_bytes(report_->fold_stats_path));
  // Drop every fold-0 asset row, keeping its 7 fold-field rows.
  std::string mutated;
  for (const std::string &line : lines) {
    if (line.starts_with("0\tasset\t")) {
      continue;
    }
    mutated += line;
    mutated += '\n';
  }
  expect_sidecar_parse_error(mutated, "without asset rows");
}

TEST_F(VrpTrainPipelineTest, SidecarLoaderRejectsOutOfOrderSymbols) {
  const std::vector<std::string> lines = split_lines(read_file_bytes(report_->fold_stats_path));
  // Swap fold 0's AAA and BBB asset blocks (BBB first breaks the strictly
  // ascending symbol order within the fold).
  std::string mutated;
  std::vector<std::string> aaa;
  bool aaa_flushed = false;
  for (const std::string &line : lines) {
    if (line.starts_with("0\tasset\tAAA\t")) {
      aaa.push_back(line);
      continue;
    }
    if (!aaa_flushed && line.starts_with("0\tasset\tCCC\t")) {
      for (const std::string &a : aaa) {
        mutated += a;
        mutated += '\n';
      }
      aaa_flushed = true;
    }
    mutated += line;
    mutated += '\n';
  }
  ASSERT_TRUE(aaa_flushed);
  expect_sidecar_parse_error(mutated, "asset rows out of canonical order");
}

TEST_F(VrpTrainPipelineTest, RawPanelRowScoresFromModelFilePlusSidecarAlone) {
  // The live-path contract: a consumer holding ONLY {model files + sidecar}
  // reproduces the trainer's predictions for a RAW panel row -- per-asset
  // standardization from the sidecar, GBT pred_label + pred_edge_norm
  // matching the emitted signal row, and the clipped baseline variance
  // forecast from the linear file + sidecar retransform state (s2 + clip
  // bounds -- review minor: baseline s2 persistence).
  const auto sidecar = vrp::load_vrp_fold_stats(report_->fold_stats_path);
  ASSERT_TRUE(sidecar.has_value()) << sidecar.error().to_string();
  const vrp::VrpFoldStats &fs = sidecar->back();
  ASSERT_EQ(fs.fold_id, report_->folds.back().fold_id);

  // Last AAA tail row: scored by the final fold's models (matching the
  // serialized final-fold model files).
  std::size_t row = report_->panel.rows.size();
  for (std::size_t r = 0; r < report_->panel.rows.size(); ++r) {
    const auto &pr = report_->panel.rows[r];
    if (pr.symbol == "AAA" && std::isnan(pr.label)) {
      row = r;
    }
  }
  ASSERT_LT(row, report_->panel.rows.size());
  const vrp::VrpPanelRow &pr = report_->panel.rows[row];

  // Standardize the RAW features from the sidecar alone (NaN / degenerate
  // sd -> z = 0, the trainer's own imputation).
  const auto sym_it = std::lower_bound(fs.symbols.begin(), fs.symbols.end(), pr.symbol);
  ASSERT_TRUE(sym_it != fs.symbols.end() && *sym_it == pr.symbol);
  const auto s = static_cast<std::size_t>(sym_it - fs.symbols.begin());
  std::array<double, kVrpFeatureCount> z{};
  for (std::size_t f = 0; f < kVrpFeatureCount; ++f) {
    const double x = pr.f[f];
    const double sd = fs.feat_sd[s][f];
    z[f] = (std::isfinite(x) && sd > 0.0) ? (x - fs.feat_mean[s][f]) / sd : 0.0;
  }

  auto gbt = load_gbt_fair_vol_model(report_->gbt_model_path.string());
  ASSERT_TRUE(gbt.has_value()) << gbt.error().to_string();
  std::array<double, 1> y{};
  ASSERT_TRUE((*gbt)->predict(z, 1, y).has_value());

  const auto signal = load_vrp_signal_v1(report_->signal_path.string());
  ASSERT_TRUE(signal.has_value()) << signal.error().to_string();
  bool found = false;
  for (const auto &srow : *signal) {
    if (srow.symbol == pr.symbol && srow.date == pr.date) {
      EXPECT_NEAR(y[0], srow.pred_label, 1e-9);
      found = true;
    }
  }
  EXPECT_TRUE(found);
  // ROUND-4 F1 SCOPE NOTE: pred_LABEL is still reproducible from
  // {model file + sidecar} alone, and that is the live-path contract this test
  // pins. pred_EDGE_NORM is not, under the cross-section default: it is a
  // WITHIN-DATE transform, so it needs the whole date's cross-section, not one
  // row's per-asset stats. The sidecar's label_mean/label_sd reproduce it only
  // under --edge-norm per-symbol, which
  // PerSymbolEdgeNormIsReproducibleFromTheSidecarAlone pins separately.

  // Baseline: the linear file scores ln(rv^2) pre-retransform; the sidecar's
  // s2 + clip bounds complete the variance forecast. Compare against the
  // in-process forecast from a deterministic refit of the final fold.
  auto lin = load_linear_fair_vol_model(report_->baseline_model_path.string());
  ASSERT_TRUE(lin.has_value()) << lin.error().to_string();
  std::array<double, 1> mu{};
  ASSERT_TRUE((*lin)->predict(z, 1, mu).has_value());
  const double f_file = std::clamp(std::exp(mu[0] + 0.5 * fs.baseline_s2), fs.train_var_min,
                                   fs.train_var_max);
  const vrp::VrpStandardization stz = vrp::compute_asset_standardization(
      report_->panel, std::span<const std::size_t>{report_->folds.back().train_rows});
  const vrp::detail::BaselineFit refit = vrp::detail::fit_vrp_baseline(
      report_->panel, stz, std::span<const std::size_t>{report_->folds.back().train_rows},
      make_synth_config(panel_file_->path_string(), out_dir_.string()));
  EXPECT_DOUBLE_EQ(fs.baseline_s2, refit.s2);
  const double f_inproc = vrp::detail::baseline_forecast_var(
      report_->panel, stz, refit, row, vrp::VrpRetransformMode::Jensen);
  EXPECT_NEAR(f_file, f_inproc, 1e-9 * std::max(1.0, f_inproc));
}

// ── VrpTrain: round-3 isotonic recalibration + retransform (digest Q4) ──────

// Reads the double value of a `# key=value` metrics meta line; NaN when the
// key is absent (asserted separately by the callers).
[[nodiscard]] double meta_double(const std::string &bytes, const std::string &key) {
  const std::string tag = "# " + key + "=";
  const std::size_t at = bytes.find(tag);
  if (at == std::string::npos) {
    return kNaN;
  }
  const std::size_t end = bytes.find('\n', at);
  return std::stod(bytes.substr(at + tag.size(), end - at - tag.size()));
}

TEST(VrpTrainMath, IsotonicPavaMatchesHandExample) {
  // x = {1,2,3,4}, y = {1,3,2,4}: the (3,2) violator pools with (2,3) into a
  // 2.5 block, giving fitted values {1, 2.5, 2.5, 4} (classic PAVA example).
  const auto map = vrp::fit_vrp_isotonic({1.0, 2.0, 3.0, 4.0}, {1.0, 3.0, 2.0, 4.0});
  ASSERT_TRUE(map.has_value()) << map.error().to_string();
  EXPECT_DOUBLE_EQ(vrp::vrp_isotonic_eval(*map, 1.0), 1.0);
  EXPECT_DOUBLE_EQ(vrp::vrp_isotonic_eval(*map, 2.0), 2.5);
  EXPECT_DOUBLE_EQ(vrp::vrp_isotonic_eval(*map, 3.0), 2.5);
  EXPECT_DOUBLE_EQ(vrp::vrp_isotonic_eval(*map, 4.0), 4.0);
  // Piecewise-linear between fitted points, flat inside a pooled block.
  EXPECT_DOUBLE_EQ(vrp::vrp_isotonic_eval(*map, 1.5), 1.75);
  EXPECT_DOUBLE_EQ(vrp::vrp_isotonic_eval(*map, 2.5), 2.5);
  EXPECT_DOUBLE_EQ(vrp::vrp_isotonic_eval(*map, 3.5), 3.25);
  // Constant extrapolation outside the fitted range: recalibrated levels are
  // bounded by observed calibration targets (the point of the exercise).
  EXPECT_DOUBLE_EQ(vrp::vrp_isotonic_eval(*map, -10.0), 1.0);
  EXPECT_DOUBLE_EQ(vrp::vrp_isotonic_eval(*map, 99.0), 4.0);
}

TEST(VrpTrainMath, IsotonicPoolsTiedInputsToTheirMean) {
  const auto map = vrp::fit_vrp_isotonic({1.0, 1.0, 2.0}, {1.0, 3.0, 5.0});
  ASSERT_TRUE(map.has_value()) << map.error().to_string();
  EXPECT_DOUBLE_EQ(vrp::vrp_isotonic_eval(*map, 1.0), 2.0); // mean(1, 3)
  EXPECT_DOUBLE_EQ(vrp::vrp_isotonic_eval(*map, 2.0), 5.0);
  EXPECT_DOUBLE_EQ(vrp::vrp_isotonic_eval(*map, 1.5), 3.5);
}

TEST(VrpTrainMath, IsotonicEvalIsMonotoneNonDecreasingOnAWigglyFit) {
  // A broadly increasing but locally violating relation: after PAVA the
  // evaluated map must be globally non-decreasing (the rank-preservation
  // property everything downstream rests on).
  std::vector<double> x;
  std::vector<double> y;
  for (std::size_t i = 0; i < 200; ++i) {
    const double xi = 0.05 * static_cast<double>(i);
    x.push_back(xi);
    y.push_back(0.8 * xi + 0.9 * std::sin(2.3 * xi));
  }
  const auto map = vrp::fit_vrp_isotonic(std::move(x), std::move(y));
  ASSERT_TRUE(map.has_value()) << map.error().to_string();
  double prev = -std::numeric_limits<double>::infinity();
  for (std::size_t k = 0; k <= 1200; ++k) {
    const double q = -1.0 + 0.01 * static_cast<double>(k);
    const double v = vrp::vrp_isotonic_eval(*map, q);
    EXPECT_LE(prev, v) << "q=" << q;
    prev = v;
  }
}

TEST(VrpTrainMath, IsotonicFitFailsClosedOnBadInputs) {
  EXPECT_FALSE(vrp::fit_vrp_isotonic({}, {}).has_value());
  EXPECT_FALSE(vrp::fit_vrp_isotonic({1.0}, {1.0, 2.0}).has_value());
  // Non-finite pairs carry no level information and are excluded; an
  // all-non-finite input fails closed instead of yielding an empty map.
  EXPECT_FALSE(vrp::fit_vrp_isotonic({kNaN}, {1.0}).has_value());
  const auto one_good = vrp::fit_vrp_isotonic({kNaN, 2.0}, {7.0, 3.0});
  ASSERT_TRUE(one_good.has_value()) << one_good.error().to_string();
  EXPECT_DOUBLE_EQ(vrp::vrp_isotonic_eval(*one_good, -5.0), 3.0);
  EXPECT_DOUBLE_EQ(vrp::vrp_isotonic_eval(*one_good, 5.0), 3.0);
}

TEST(VrpTrainMath, MincerZarnowitzRecoversPlantedAffineRelation) {
  const std::vector<double> f{0.1, 0.2, 0.4, 0.8};
  std::vector<double> r;
  for (const double v : f) {
    r.push_back(0.5 + 2.0 * v);
  }
  const vrp::VrpMzFit fit = vrp::vrp_mincer_zarnowitz(std::span<const double>{f},
                                                      std::span<const double>{r});
  EXPECT_NEAR(fit.slope, 2.0, 1e-12);
  EXPECT_NEAR(fit.intercept, 0.5, 1e-12);
  // Degenerate (constant) forecasts: slope/intercept are NaN, never faked.
  const std::vector<double> flat{0.3, 0.3};
  const std::vector<double> real{1.0, 2.0};
  const vrp::VrpMzFit deg = vrp::vrp_mincer_zarnowitz(std::span<const double>{flat},
                                                      std::span<const double>{real});
  EXPECT_TRUE(std::isnan(deg.slope));
  EXPECT_TRUE(std::isnan(deg.intercept));
}

TEST(VrpTrainMath, SmearingFactorAndRetransformMatchHandComputation) {
  // Duan smearing: factor = mean(exp(residual)) -- 0.5 and 2.0 average 1.25.
  const std::vector<double> resid{std::log(0.5), std::log(2.0)};
  EXPECT_DOUBLE_EQ(vrp::vrp_smearing_factor(std::span<const double>{resid}), 1.25);
  // forecast = exp(mu) * factor, then the SAME insanity clip as Jensen.
  EXPECT_DOUBLE_EQ(vrp::vrp_smearing_retransform_clip(std::log(0.04), 1.25, 0.0, 1.0), 0.05);
  EXPECT_DOUBLE_EQ(vrp::vrp_smearing_retransform_clip(std::log(0.04), 1.25, 0.0, 0.045),
                   0.045);
}

// One shared isotonic-mode trainer run (deterministic, same panel content as
// the flag-off VrpTrainPipelineTest fixture, walk 90/20/20, window 21).
class VrpTrainRecalPipelineTest : public ::testing::Test {
protected:
  static void SetUpTestSuite() {
    panel_file_ = new ScopedTempFile("recal_panel", make_synth_panel_tsv());
    out_dir_ = unique_temp_path("recal_out", "");
    vrp::VrpTrainConfig cfg = make_synth_config(panel_file_->path_string(), out_dir_.string());
    cfg.recalibrate = vrp::VrpRecalMode::Isotonic;
    cfg.recalib_window_sessions = 21;
    auto report = vrp::run_vrp_train(cfg);
    ASSERT_TRUE(report.has_value()) << report.error().to_string();
    report_ = new vrp::VrpTrainReport(std::move(*report));
  }

  static void TearDownTestSuite() {
    delete report_;
    report_ = nullptr;
    delete panel_file_;
    panel_file_ = nullptr;
    std::error_code ec;
    std::filesystem::remove_all(out_dir_, ec);
  }

  static ScopedTempFile *panel_file_;
  static std::filesystem::path out_dir_;
  static vrp::VrpTrainReport *report_;
};

ScopedTempFile *VrpTrainRecalPipelineTest::panel_file_ = nullptr;
std::filesystem::path VrpTrainRecalPipelineTest::out_dir_{};
vrp::VrpTrainReport *VrpTrainRecalPipelineTest::report_ = nullptr;

TEST_F(VrpTrainRecalPipelineTest, IsotonicRecalibrationPreservesTestRankOrder) {
  // The monotone map may repair the LEVEL but must never reorder the ranks:
  // raw_i < raw_j => recal_i <= recal_j (ties from pooled blocks allowed),
  // and equal raw forecasts stay equal. Rank IC may lose only tie
  // granularity, so before/after IC must stay close.
  ASSERT_FALSE(report_->folds.empty());
  for (const auto &fold : report_->folds) {
    EXPECT_TRUE(fold.recal_applied);
    ASSERT_EQ(fold.test_pred_raw.size(), fold.test_rows.size());
    ASSERT_EQ(fold.test_pred_recal.size(), fold.test_rows.size());
    bool any_diff = false;
    for (std::size_t i = 0; i < fold.test_pred_raw.size(); ++i) {
      any_diff = any_diff || fold.test_pred_recal[i] != fold.test_pred_raw[i];
      for (std::size_t j = i + 1; j < fold.test_pred_raw.size(); ++j) {
        if (fold.test_pred_raw[i] < fold.test_pred_raw[j]) {
          EXPECT_LE(fold.test_pred_recal[i], fold.test_pred_recal[j]);
        } else if (fold.test_pred_raw[i] == fold.test_pred_raw[j]) {
          EXPECT_EQ(fold.test_pred_recal[i], fold.test_pred_recal[j]);
        } else {
          EXPECT_GE(fold.test_pred_recal[i], fold.test_pred_recal[j]);
        }
      }
    }
    EXPECT_TRUE(any_diff); // the level actually moved somewhere
    EXPECT_NEAR(fold.ic_gbt, fold.ic_gbt_recal, 0.25);
  }
}

TEST_F(VrpTrainRecalPipelineTest, IsotonicFitWindowStaysStrictlyBeforeEachFoldsTestStart) {
  // THE leak-safety pin: every isotonic fit row is an ADMITTED TRAIN row
  // from the trailing window, so its decision is strictly before the fold's
  // test start AND its recorded emitted-axis label end (a provable UPPER
  // bound on the true end) never crosses the earliest test decision -- the
  // fit uses only data strictly before the test window, which is why the
  // leak adjudicator stays PASS with recalibration on.
  std::vector<std::int64_t> label_end_of(report_->panel.rows.size(), -1);
  for (std::size_t i = 0; i < report_->observations.obs.size(); ++i) {
    label_end_of[report_->observations.row_of[i]] =
        report_->observations.obs[i].label_end_ts_ns;
  }
  ASSERT_FALSE(report_->folds.empty());
  for (const auto &fold : report_->folds) {
    ASSERT_TRUE(fold.recal_applied);
    ASSERT_GT(fold.recal_n_fit, 0u);
    ASSERT_EQ(fold.recal_fit_rows.size(), fold.recal_n_fit);
    std::int64_t test_min = std::numeric_limits<std::int64_t>::max();
    for (const std::size_t r : fold.test_rows) {
      test_min = std::min(test_min, report_->panel.rows[r].entry_ts_ns);
    }
    for (const std::size_t r : fold.recal_fit_rows) {
      EXPECT_LT(report_->panel.rows[r].entry_ts_ns, test_min);
      ASSERT_NE(label_end_of[r], -1);
      EXPECT_LE(label_end_of[r], test_min);
      EXPECT_TRUE(std::find(fold.train_rows.begin(), fold.train_rows.end(), r) !=
                  fold.train_rows.end());
    }
    // Window accounting: the fit sessions are exactly the trailing
    // recal_window_effective distinct admitted train sessions.
    std::vector<std::int64_t> train_ts;
    for (const std::size_t r : fold.train_rows) {
      train_ts.push_back(report_->panel.rows[r].entry_ts_ns);
    }
    train_ts.erase(std::unique(train_ts.begin(), train_ts.end()), train_ts.end());
    std::vector<std::int64_t> fit_ts;
    for (const std::size_t r : fold.recal_fit_rows) {
      fit_ts.push_back(report_->panel.rows[r].entry_ts_ns);
    }
    fit_ts.erase(std::unique(fit_ts.begin(), fit_ts.end()), fit_ts.end());
    EXPECT_EQ(fit_ts.size(), fold.recal_window_effective);
    EXPECT_EQ(fold.recal_window_effective, 21u); // min(21, n_train_sessions/2)
    ASSERT_LE(fit_ts.size(), train_ts.size());
    const std::vector<std::int64_t> tail(train_ts.end() -
                                             static_cast<std::ptrdiff_t>(fit_ts.size()),
                                         train_ts.end());
    EXPECT_EQ(fit_ts, tail);
  }
}

TEST_F(VrpTrainRecalPipelineTest, RecalibratedValuesFlowOnlyBehindTheFlagAndRawPathIsUntouched) {
  const auto out = unique_temp_path("recal_off_cmp", "");
  const auto off =
      vrp::run_vrp_train(make_synth_config(panel_file_->path_string(), out.string()));
  ASSERT_TRUE(off.has_value()) << off.error().to_string();
  ASSERT_EQ(off->folds.size(), report_->folds.size());
  for (std::size_t k = 0; k < off->folds.size(); ++k) {
    const auto &a = off->folds[k];
    const auto &b = report_->folds[k];
    // The flag adds a post-hoc map: fold plan and raw production forecasts
    // must be bit-identical to the flag-off run (extra calibration fits use
    // their own seeded state and cannot perturb the production model).
    EXPECT_EQ(a.train_rows, b.train_rows);
    EXPECT_EQ(a.test_rows, b.test_rows);
    EXPECT_EQ(a.test_pred_raw, b.test_pred_raw);
    EXPECT_DOUBLE_EQ(a.qlike_gbt, b.qlike_gbt);
    EXPECT_DOUBLE_EQ(a.ic_gbt, b.ic_gbt);
    // Flag off: the recalibrated vector collapses onto the raw one.
    EXPECT_EQ(a.test_pred_raw, a.test_pred_recal);
    EXPECT_FALSE(a.recal_applied);
  }
  // Recalibrated values flow into pred_label only behind the flag; the file
  // stays SCHEMA byte-compatible (same schema + header lines) and parses
  // under the UNMODIFIED frozen vrp_signal_v1 loader.
  const std::string sig_off = read_file_bytes(off->signal_path);
  const std::string sig_on = read_file_bytes(report_->signal_path);
  EXPECT_NE(sig_off, sig_on);
  const auto head = [](const std::string &bytes) {
    const std::size_t first = bytes.find('\n');
    return bytes.substr(0, bytes.find('\n', first + 1));
  };
  EXPECT_EQ(head(sig_off), head(sig_on));
  const auto loaded = load_vrp_signal_v1(report_->signal_path.string());
  ASSERT_TRUE(loaded.has_value()) << loaded.error().to_string();
  // Spot-check: an emitted test row carries the fold's recalibrated value
  // (fmt_double is shortest-round-trip, so the parse is exact).
  const auto &fold0 = report_->folds.front();
  ASSERT_FALSE(fold0.test_rows.empty());
  const vrp::VrpPanelRow &pr = report_->panel.rows[fold0.test_rows.front()];
  bool found = false;
  for (const auto &srow : *loaded) {
    if (srow.symbol == pr.symbol && srow.date == pr.date) {
      EXPECT_DOUBLE_EQ(srow.pred_label, fold0.test_pred_recal.front());
      found = true;
    }
  }
  EXPECT_TRUE(found);
  std::error_code ec;
  std::filesystem::remove_all(out, ec);
}

TEST_F(VrpTrainRecalPipelineTest, MetricsMetaLinesCarryBeforeAfterAndWindowAccounting) {
  const std::string bytes = read_file_bytes(report_->metrics_path);
  EXPECT_NE(bytes.find("# recalibrate=isotonic\n"), std::string::npos);
  EXPECT_NE(bytes.find("# recalib_window=21\n"), std::string::npos);
  EXPECT_NE(bytes.find("# retransform=jensen\n"), std::string::npos);
  for (const auto &m : report_->folds) {
    const std::string p = "# fold_" + std::to_string(m.fold_id) + "_";
    const auto has = [&](const std::string &line) {
      EXPECT_NE(bytes.find(line), std::string::npos) << line;
    };
    has(p + "mz_slope_raw=" + vrp::detail::fmt_double(m.mz_slope_raw) + "\n");
    has(p + "mz_intercept_raw=" + vrp::detail::fmt_double(m.mz_intercept_raw) + "\n");
    has(p + "smear_factor=" + vrp::detail::fmt_double(m.smear_factor) + "\n");
    has(p + "recal_applied=1\n");
    has(p + "recal_window_effective=" + std::to_string(m.recal_window_effective) + "\n");
    has(p + "recal_n_fit=" + std::to_string(m.recal_n_fit) + "\n");
    has(p + "qlike_gbt_recal=" + vrp::detail::fmt_double(m.qlike_gbt_recal) + "\n");
    has(p + "mz_slope_recal=" + vrp::detail::fmt_double(m.mz_slope_recal) + "\n");
    has(p + "mz_intercept_recal=" + vrp::detail::fmt_double(m.mz_intercept_recal) + "\n");
    has(p + "ic_gbt_recal=" + vrp::detail::fmt_double(m.ic_gbt_recal) + "\n");
    EXPECT_TRUE(std::isfinite(m.mz_slope_raw));
    EXPECT_TRUE(std::isfinite(m.qlike_gbt_recal));
  }
}

TEST(VrpTrainRecal, OversizedWindowShrinksToHalfTheAdmittedTrainSessions) {
  const ScopedTempFile panel("recal_big_win", make_synth_panel_tsv());
  const auto out = unique_temp_path("recal_big_win_out", "");
  vrp::VrpTrainConfig cfg = make_synth_config(panel.path_string(), out.string());
  cfg.recalibrate = vrp::VrpRecalMode::Isotonic;
  cfg.recalib_window_sessions = 500;
  const auto report = vrp::run_vrp_train(cfg);
  ASSERT_TRUE(report.has_value()) << report.error().to_string();
  for (const auto &fold : report->folds) {
    std::vector<std::int64_t> train_ts;
    for (const std::size_t r : fold.train_rows) {
      train_ts.push_back(report->panel.rows[r].entry_ts_ns);
    }
    train_ts.erase(std::unique(train_ts.begin(), train_ts.end()), train_ts.end());
    EXPECT_EQ(fold.recal_window_effective, train_ts.size() / 2);
    EXPECT_TRUE(fold.recal_applied);
  }
  std::error_code ec;
  std::filesystem::remove_all(out, ec);
}

TEST(VrpTrainRecal, ZeroRecalWindowFailsClosed) {
  const ScopedTempFile panel("recal_zero_win", make_synth_panel_tsv());
  vrp::VrpTrainConfig cfg =
      make_synth_config(panel.path_string(), unique_temp_path("recal_zero_out", "").string());
  cfg.recalibrate = vrp::VrpRecalMode::Isotonic;
  cfg.recalib_window_sessions = 0;
  const auto report = vrp::run_vrp_train(cfg);
  ASSERT_FALSE(report.has_value());
  EXPECT_NE(report.error().to_string().find("recalib-window"), std::string::npos);
}

TEST(VrpTrainRecal, IsotonicModeIsByteDeterministicAcrossRuns) {
  const ScopedTempFile panel("recal_det", make_synth_panel_tsv());
  const auto out_a = unique_temp_path("recal_det_a", "");
  const auto out_b = unique_temp_path("recal_det_b", "");
  vrp::VrpTrainConfig cfg_a = make_synth_config(panel.path_string(), out_a.string());
  cfg_a.recalibrate = vrp::VrpRecalMode::Isotonic;
  cfg_a.recalib_window_sessions = 21;
  vrp::VrpTrainConfig cfg_b = cfg_a;
  cfg_b.out_dir = out_b.string();
  const auto a = vrp::run_vrp_train(cfg_a);
  const auto b = vrp::run_vrp_train(cfg_b);
  ASSERT_TRUE(a.has_value()) << a.error().to_string();
  ASSERT_TRUE(b.has_value()) << b.error().to_string();
  EXPECT_EQ(read_file_bytes(a->signal_path), read_file_bytes(b->signal_path));
  EXPECT_EQ(read_file_bytes(a->metrics_path), read_file_bytes(b->metrics_path));
  EXPECT_EQ(read_file_bytes(a->gbt_model_path), read_file_bytes(b->gbt_model_path));
  EXPECT_EQ(read_file_bytes(a->baseline_model_path), read_file_bytes(b->baseline_model_path));
  EXPECT_EQ(read_file_bytes(a->fold_stats_path), read_file_bytes(b->fold_stats_path));
  std::error_code ec;
  std::filesystem::remove_all(out_a, ec);
  std::filesystem::remove_all(out_b, ec);
}

TEST_F(VrpTrainPipelineTest, MetricsFileCarriesMzHonestyMetaLinesWithFlagOff) {
  // Feature 3 (metrics honesty) is NOT gated on the flag: the raw GBT's
  // Mincer-Zarnowitz level diagnostics and the baseline smearing factor are
  // reported per fold in every mode (QLIKE alone can favor positively
  // biased forecasts, digest [15]); recal lines appear only behind the flag.
  const std::string bytes = read_file_bytes(report_->metrics_path);
  EXPECT_NE(bytes.find("# recalibrate=off\n"), std::string::npos);
  EXPECT_NE(bytes.find("# retransform=jensen\n"), std::string::npos);
  EXPECT_EQ(bytes.find("# recalib_window="), std::string::npos);
  EXPECT_EQ(bytes.find("recal_applied="), std::string::npos);
  EXPECT_EQ(bytes.find("mz_slope_recal="), std::string::npos);
  for (const auto &m : report_->folds) {
    const std::string p = "# fold_" + std::to_string(m.fold_id) + "_";
    EXPECT_NE(bytes.find(p + "mz_slope_raw=" + vrp::detail::fmt_double(m.mz_slope_raw) + "\n"),
              std::string::npos);
    EXPECT_NE(bytes.find(p + "mz_intercept_raw=" +
                         vrp::detail::fmt_double(m.mz_intercept_raw) + "\n"),
              std::string::npos);
    EXPECT_NE(bytes.find(p + "smear_factor=" + vrp::detail::fmt_double(m.smear_factor) + "\n"),
              std::string::npos);
    EXPECT_TRUE(std::isfinite(m.mz_slope_raw));
    EXPECT_TRUE(std::isfinite(m.mz_intercept_raw));
    // Flag off: recal vectors collapse onto raw, nothing recal-side applied.
    EXPECT_FALSE(m.recal_applied);
    EXPECT_EQ(m.test_pred_raw, m.test_pred_recal);
  }
}

TEST_F(VrpTrainPipelineTest, SmearingRetransformChangesOnlyTheBaselinePath) {
  // Duan smearing (digest [16][17]) replaces exp(s2/2) with mean(exp(resid))
  // in the baseline retransform, behind its own flag: the GBT path (raw
  // preds, QLIKE clip) must stay bit-identical, and the factor is reported.
  const auto out = unique_temp_path("smear_out", "");
  vrp::VrpTrainConfig cfg = make_synth_config(panel_file_->path_string(), out.string());
  cfg.retransform = vrp::VrpRetransformMode::Smearing;
  const auto smear = vrp::run_vrp_train(cfg);
  ASSERT_TRUE(smear.has_value()) << smear.error().to_string();
  ASSERT_EQ(smear->folds.size(), report_->folds.size());
  for (std::size_t k = 0; k < smear->folds.size(); ++k) {
    const auto &a = report_->folds[k];
    const auto &b = smear->folds[k];
    EXPECT_NE(a.qlike_baseline, b.qlike_baseline); // level path actually moved
    EXPECT_DOUBLE_EQ(a.qlike_gbt, b.qlike_gbt);
    EXPECT_EQ(a.test_pred_raw, b.test_pred_raw);
    EXPECT_TRUE(std::isfinite(b.smear_factor));
    EXPECT_GT(b.smear_factor, 0.0);
  }
  const std::string bytes = read_file_bytes(smear->metrics_path);
  EXPECT_NE(bytes.find("# retransform=smearing\n"), std::string::npos);
  EXPECT_NE(bytes.find("# fold_0_smear_factor="), std::string::npos);
  EXPECT_TRUE(std::isfinite(meta_double(bytes, "fold_0_smear_factor")));
  std::error_code ec;
  std::filesystem::remove_all(out, ec);
}

// ── ROUND 4 F3: honest-metric primitives ────────────────────────────────────

TEST(VrpTrainMath, PearsonAndSpearmanAreUndefinedNotZeroOnDegenerateInput) {
  // A zero correlation is a MEASUREMENT; an unmeasurable one must not
  // impersonate one. The engine kernel's all-zero convention is right for
  // feature screening and wrong for a reported IC, so the degenerate cases
  // are intercepted before it ever runs.
  const std::array<double, 1> one{1.0};
  EXPECT_TRUE(std::isnan(vrp::vrp_pearson(one, one)));
  EXPECT_TRUE(std::isnan(vrp::vrp_spearman(one, one)));
  const std::array<double, 4> flat{2.0, 2.0, 2.0, 2.0};
  const std::array<double, 4> vary{1.0, 2.0, 3.0, 4.0};
  EXPECT_TRUE(std::isnan(vrp::vrp_pearson(flat, vary)));
  EXPECT_TRUE(std::isnan(vrp::vrp_pearson(vary, flat)));
  EXPECT_TRUE(std::isnan(vrp::vrp_spearman(flat, vary)));
  // Size mismatch is a caller bug, not a zero.
  EXPECT_TRUE(std::isnan(vrp::vrp_pearson(one, vary)));
  // Non-finite pairs drop out; the surviving pairs decide the answer.
  const std::array<double, 5> ax{1.0, 2.0, kNaN, 3.0, 4.0};
  const std::array<double, 5> ay{2.0, 4.0, 9.0, 6.0, 8.0};
  EXPECT_NEAR(vrp::vrp_pearson(ax, ay), 1.0, 1e-12);
  EXPECT_NEAR(vrp::vrp_spearman(ax, ay), 1.0, 1e-12);
  // Perfect monotone but non-linear: Spearman 1, Pearson strictly below --
  // the exact substitution Grinold's alpha = IC*sigma_y*z must never make.
  const std::array<double, 5> bx{1.0, 2.0, 3.0, 4.0, 5.0};
  const std::array<double, 5> by{1.0, 4.0, 9.0, 16.0, 100.0};
  EXPECT_NEAR(vrp::vrp_spearman(bx, by), 1.0, 1e-12);
  EXPECT_LT(vrp::vrp_pearson(bx, by), 0.95);
  EXPECT_DOUBLE_EQ(vrp::vrp_corr(bx, by, vrp::VrpCorrKind::Pearson), vrp::vrp_pearson(bx, by));
  EXPECT_DOUBLE_EQ(vrp::vrp_corr(bx, by, vrp::VrpCorrKind::Spearman),
                   vrp::vrp_spearman(bx, by));
}

TEST(VrpTrainMath, StatAggTStatsMatchHandComputationAndPayTheOverlapHaircut) {
  // x = {1,2,3,4,5}: mean 3, unbiased sd sqrt(2.5), se = sqrt(2.5/5),
  // t_iid = 3/sqrt(0.5) = 4.2426406871192848.
  const std::array<double, 5> x{1.0, 2.0, 3.0, 4.0, 5.0};
  const vrp::VrpStatAgg a = vrp::vrp_aggregate_series(x, 0);
  EXPECT_DOUBLE_EQ(a.mean, 3.0);
  EXPECT_EQ(a.n, 5u);
  EXPECT_NEAR(a.t_iid, 3.0 / std::sqrt(0.5), 1e-12);
  // At lag 0 the Bartlett sum is just gamma_0 (the POPULATION second moment),
  // so t_nw = mean / sqrt(g0/n) = 3 / sqrt(2/5).
  EXPECT_NEAR(a.t_nw, 3.0 / std::sqrt(2.0 / 5.0), 1e-12);
  // A trending (positively autocorrelated) series is exactly the overlap
  // shape: the HAC t must be strictly SMALLER than the naive one.
  const vrp::VrpStatAgg lagged = vrp::vrp_aggregate_series(x, 2);
  EXPECT_LT(lagged.t_nw, lagged.t_iid);
  // Non-finite entries are dropped, not counted.
  const std::array<double, 4> holey{1.0, kNaN, 3.0, 5.0};
  const vrp::VrpStatAgg h = vrp::vrp_aggregate_series(holey, 0);
  EXPECT_EQ(h.n, 3u);
  EXPECT_DOUBLE_EQ(h.mean, 3.0);
  // Degenerate: empty -> NaN mean; constant -> a mean but no t (no sampling
  // error to divide by), never a fabricated infinity.
  const vrp::VrpStatAgg empty = vrp::vrp_aggregate_series(std::span<const double>{}, 0);
  EXPECT_TRUE(std::isnan(empty.mean));
  EXPECT_EQ(empty.n, 0u);
  const std::array<double, 3> konst{2.0, 2.0, 2.0};
  const vrp::VrpStatAgg k = vrp::vrp_aggregate_series(konst, 0);
  EXPECT_DOUBLE_EQ(k.mean, 2.0);
  EXPECT_TRUE(std::isnan(k.t_iid));
  EXPECT_TRUE(std::isnan(k.t_nw));
}

TEST(VrpTrainMath, MseMatchesHandComputationOverFinitePairsOnly) {
  const std::array<double, 4> f{1.0, 2.0, 3.0, kNaN};
  const std::array<double, 4> r{1.5, 1.0, 5.0, 0.0};
  // (0.25 + 1 + 4) / 3
  EXPECT_NEAR(vrp::vrp_mse(f, r), 5.25 / 3.0, 1e-12);
  const std::array<double, 2> none{kNaN, kNaN};
  const std::array<double, 2> some{1.0, 2.0};
  EXPECT_TRUE(std::isnan(vrp::vrp_mse(none, some)));
}

TEST(VrpTrainMath, DecileStatsRecoverPlantedTailsAndCarryTStats) {
  // Two dates, 20 names each, realized == score: a perfectly monotone
  // cross-section. Deciles must be increasing, rho = +1, and the top-minus-
  // bottom spread must be positive with a defined t-stat -- the harvestability
  // test of audit-gross-negative S1 run inside the trainer.
  std::vector<std::int64_t> ts;
  std::vector<double> score;
  std::vector<double> real;
  for (int d = 0; d < 2; ++d) {
    for (int i = 0; i < 20; ++i) {
      ts.push_back(d);
      score.push_back(static_cast<double>(i));
      real.push_back(static_cast<double>(i));
    }
  }
  const vrp::VrpDecileStats up = vrp::vrp_decile_stats(ts, score, real);
  EXPECT_EQ(up.n_dates, 2u);
  EXPECT_NEAR(up.rho, 1.0, 1e-12);
  EXPECT_NEAR(up.spread, 18.0, 1e-12); // mean{18,19} - mean{0,1}
  EXPECT_TRUE(std::isfinite(up.ic_traded));
  EXPECT_NEAR(up.ic_traded, 1.0, 1e-12); // tails only, still perfectly linear
  // Both dates realize the SAME spread, so the per-date series is constant:
  // there is no sampling error to divide by and the t-stat is undefined, not
  // infinite and not zero.
  EXPECT_TRUE(std::isnan(up.spread_t));
  EXPECT_TRUE(std::isnan(up.spread_t_nw));

  // A genuinely varying per-date spread does produce a t-stat, and the
  // overlap-adjusted one is the smaller of the two on a trending series.
  std::vector<std::int64_t> vts;
  std::vector<double> vscore;
  std::vector<double> vreal;
  for (int d = 0; d < 6; ++d) {
    for (int i = 0; i < 20; ++i) {
      vts.push_back(d);
      vscore.push_back(static_cast<double>(i));
      vreal.push_back(static_cast<double>(i) * (1.0 + 0.1 * static_cast<double>(d)));
    }
  }
  const vrp::VrpDecileStats varying = vrp::vrp_decile_stats(vts, vscore, vreal);
  EXPECT_EQ(varying.n_dates, 6u);
  EXPECT_GT(varying.spread, 0.0);
  EXPECT_GT(varying.spread_t, 0.0);
  EXPECT_TRUE(std::isfinite(varying.spread_t_nw));
  EXPECT_LT(varying.spread_t_nw, varying.spread_t);
  EXPECT_NEAR(varying.rho, 1.0, 1e-12);
  // INVERTED tails with a flat-to-positive pooled correlation is exactly the
  // pred_edge_norm pathology (+0.042 pooled IC, -2.5 vol pts in the tails):
  // flip the realized value only in the two extreme deciles.
  std::vector<double> inverted = real;
  for (std::size_t i = 0; i < inverted.size(); ++i) {
    const double s = score[i];
    if (s >= 18.0) {
      inverted[i] = -100.0;
    } else if (s <= 1.0) {
      inverted[i] = 100.0;
    }
  }
  const vrp::VrpDecileStats bad = vrp::vrp_decile_stats(ts, score, inverted);
  EXPECT_LT(bad.spread, 0.0);
  EXPECT_LT(bad.ic_traded, 0.0);
  // A date thinner than one name per decile contributes NOTHING rather than a
  // fabricated tail.
  const std::vector<std::int64_t> thin_ts{7, 7, 7};
  const std::vector<double> thin{1.0, 2.0, 3.0};
  const vrp::VrpDecileStats thin_stats = vrp::vrp_decile_stats(thin_ts, thin, thin);
  EXPECT_EQ(thin_stats.n_dates, 0u);
  EXPECT_TRUE(std::isnan(thin_stats.spread));
  EXPECT_TRUE(std::isnan(thin_stats.rho));
}

TEST(VrpTrainMath, FmtDoubleCanonicalizesEveryNanSpelling) {
  // UCRT/MSVC to_chars prints the x87 "indefinite" quiet NaN (what 0.0/0.0
  // yields) as "-nan(ind)". A TSV whose bytes depend on which instruction
  // produced an undefined value is not reproducible.
  EXPECT_EQ(vrp::detail::fmt_double(kNaN), "nan");
  EXPECT_EQ(vrp::detail::fmt_double(-kNaN), "nan");
  volatile double zero = 0.0;
  EXPECT_EQ(vrp::detail::fmt_double(zero / zero), "nan");
  EXPECT_EQ(vrp::detail::fmt_double(std::numeric_limits<double>::infinity()), "inf");
  EXPECT_EQ(vrp::detail::fmt_double(1.5), "1.5");
}

// ── ROUND 4 F2: the benchmark gate verdict ──────────────────────────────────

namespace {
// The verdict reads ONLY the rv_fwd_21d axis, so that is what these fixtures
// carry unless a test deliberately says otherwise.
[[nodiscard]] vrp::VrpScoreReport make_score(std::string name, vrp::VrpScoreKind kind,
                                             double pearson, double spearman) {
  vrp::VrpScoreReport s;
  s.name = std::move(name);
  s.kind = kind;
  s.target = vrp::VrpTargetAxis::RvFwd;
  s.ic_pearson = pearson;
  s.ic_spearman = spearman;
  return s;
}

[[nodiscard]] vrp::VrpScoreReport make_score_on(std::string name, vrp::VrpScoreKind kind,
                                                vrp::VrpTargetAxis axis, double pearson,
                                                double spearman) {
  vrp::VrpScoreReport s = make_score(std::move(name), kind, pearson, spearman);
  s.target = axis;
  return s;
}

[[nodiscard]] vrp::VrpPnlReport make_pnl(std::string name, vrp::VrpScoreKind kind,
                                         double iv_neutral_excess) {
  vrp::VrpPnlReport p;
  p.name = std::move(name);
  p.kind = kind;
  p.iv_neutral.excess = iv_neutral_excess;
  return p;
}

[[nodiscard]] vrp::VrpPnlFloor make_floor(double mean) {
  vrp::VrpPnlFloor f;
  f.mean = mean;
  return f;
}

// The measured SP100 floor: shorting the whole cross-section blind earns
// +3.706 vol pts / 1u gross vega / cycle.
constexpr double kMeasuredFloor = 3.706;
} // namespace

TEST(VrpTrainGate, VerdictFailsWhenAFreeBenchmarkBeatsTheModel) {
  // The measured SP100 state on the CORRECTED target. Against rv_fwd_21d the
  // GBT scores +0.0467 and the free hv_iv_gap +0.0730, and hv_iv_gap earns
  // +4.998 IV-neutralised vol pts against the GBT's +2.157 -- against a
  // short-everything floor of +3.706, so the model does not even clear doing
  // nothing. That must read FAIL.
  const std::vector<vrp::VrpScoreReport> scores{
      make_score("gbt", vrp::VrpScoreKind::Model, 0.0671, 0.0467),
      make_score("baseline_log_har", vrp::VrpScoreKind::Baseline, 0.30, 0.55),
      make_score("bench_hv_iv_gap", vrp::VrpScoreKind::Benchmark, 0.1674, 0.0730)};
  const std::vector<vrp::VrpPnlReport> pnl{
      make_pnl("gbt", vrp::VrpScoreKind::Model, 2.157 - kMeasuredFloor),
      make_pnl("bench_hv_iv_gap", vrp::VrpScoreKind::Benchmark, 4.998 - kMeasuredFloor)};
  const vrp::VrpGateVerdict v =
      vrp::vrp_gate_verdict(scores, pnl, make_floor(kMeasuredFloor));
  EXPECT_FALSE(v.pass);
  EXPECT_EQ(v.model, "gbt");
  EXPECT_EQ(v.best_benchmark, "bench_hv_iv_gap");
  EXPECT_EQ(v.n_benchmarks, 1u);
  EXPECT_DOUBLE_EQ(v.model_ic_spearman, 0.0467);
  EXPECT_DOUBLE_EQ(v.best_benchmark_ic_spearman, 0.0730);
  EXPECT_DOUBLE_EQ(v.pnl_floor, kMeasuredFloor);
  EXPECT_NEAR(v.model_pnl_excess, -1.549, 1e-12);
  EXPECT_NEAR(v.best_benchmark_pnl_excess, 1.292, 1e-12);
  // The FITTED baseline outscoring the model is reported but never the bar:
  // only zero-parameter benchmarks decide the verdict.
  const std::vector<vrp::VrpScoreReport> beats_benchmarks{
      make_score("gbt", vrp::VrpScoreKind::Model, 0.40, 0.60),
      make_score("baseline_log_har", vrp::VrpScoreKind::Baseline, 0.90, 0.90),
      make_score("bench_hv_iv_gap", vrp::VrpScoreKind::Benchmark, 0.20, 0.30)};
  const std::vector<vrp::VrpPnlReport> beats_pnl{
      make_pnl("gbt", vrp::VrpScoreKind::Model, 2.0),
      make_pnl("baseline_log_har", vrp::VrpScoreKind::Baseline, 9.0),
      make_pnl("bench_hv_iv_gap", vrp::VrpScoreKind::Benchmark, 1.0)};
  EXPECT_TRUE(
      vrp::vrp_gate_verdict(beats_benchmarks, beats_pnl, make_floor(kMeasuredFloor)).pass);
}

TEST(VrpTrainGate, ContaminatedNegIvFairCanNeverDecideTheVerdict) {
  // -iv_fair_21d is a PERFECT rank transform of the composite label's own
  // implied leg (IC exactly +1.0000, by algebra, since iv_fair_21d > 0) and a
  // strong ANTI-forecaster of realized vol (-0.6128, t_nw -22.93 on SP100).
  // Round 4 counted it as a zero-parameter benchmark and every verdict it
  // issued is void. Plant it here beating the model on every axis by a mile:
  // the verdict must not move, and it must not be counted as a benchmark.
  const std::vector<vrp::VrpScoreReport> without{
      make_score("gbt", vrp::VrpScoreKind::Model, 0.40, 0.60),
      make_score("bench_hv_iv_gap", vrp::VrpScoreKind::Benchmark, 0.20, 0.30)};
  const std::vector<vrp::VrpPnlReport> pnl_without{
      make_pnl("gbt", vrp::VrpScoreKind::Model, 2.0),
      make_pnl("bench_hv_iv_gap", vrp::VrpScoreKind::Benchmark, 1.0)};
  const vrp::VrpGateVerdict base =
      vrp::vrp_gate_verdict(without, pnl_without, make_floor(kMeasuredFloor));
  ASSERT_TRUE(base.pass);

  std::vector<vrp::VrpScoreReport> with = without;
  with.push_back(
      make_score("contaminated_neg_iv_fair_21d", vrp::VrpScoreKind::Contaminated, 0.99, 0.99));
  std::vector<vrp::VrpPnlReport> pnl_with = pnl_without;
  pnl_with.push_back(
      make_pnl("contaminated_neg_iv_fair_21d", vrp::VrpScoreKind::Contaminated, 99.0));
  const vrp::VrpGateVerdict v =
      vrp::vrp_gate_verdict(with, pnl_with, make_floor(kMeasuredFloor));
  EXPECT_TRUE(v.pass);
  EXPECT_EQ(v.n_benchmarks, base.n_benchmarks);
  EXPECT_EQ(v.best_benchmark, "bench_hv_iv_gap");
  EXPECT_DOUBLE_EQ(v.best_benchmark_ic_spearman, 0.30);
}

TEST(VrpTrainGate, OnlyTheRvFwdAxisDecidesTheVerdict) {
  // The composite label's rank ordering is anti-correlated with realized-vol
  // forecasting skill, so it is reported and NEVER gated. A model that loses
  // catastrophically on the label axis while winning on the realized leg must
  // still pass; that is the whole correction.
  std::vector<vrp::VrpScoreReport> scores{
      make_score("gbt", vrp::VrpScoreKind::Model, 0.40, 0.60),
      make_score_on("gbt", vrp::VrpScoreKind::Model, vrp::VrpTargetAxis::Label, -0.90, -0.90),
      make_score("bench_hv_iv_gap", vrp::VrpScoreKind::Benchmark, 0.20, 0.30),
      make_score_on("bench_hv_iv_gap", vrp::VrpScoreKind::Benchmark,
                    vrp::VrpTargetAxis::Label, 0.95, 0.95),
      make_score_on("bench_hv_iv_gap", vrp::VrpScoreKind::Benchmark,
                    vrp::VrpTargetAxis::VolChg, 0.95, 0.95)};
  const std::vector<vrp::VrpPnlReport> pnl{
      make_pnl("gbt", vrp::VrpScoreKind::Model, 2.0),
      make_pnl("bench_hv_iv_gap", vrp::VrpScoreKind::Benchmark, 1.0)};
  const vrp::VrpGateVerdict v =
      vrp::vrp_gate_verdict(scores, pnl, make_floor(kMeasuredFloor));
  EXPECT_TRUE(v.pass);
  EXPECT_EQ(v.n_benchmarks, 1u); // one benchmark, not one per axis
  EXPECT_DOUBLE_EQ(v.model_ic_spearman, 0.60);
  EXPECT_DOUBLE_EQ(v.best_benchmark_ic_spearman, 0.30);
}

TEST(VrpTrainGate, VerdictFailsClosedOnTiesMissingBenchmarksMissingMoneyAndNaN) {
  const auto pnl = [](double model_excess, double bench_excess) {
    return std::vector<vrp::VrpPnlReport>{
        make_pnl("gbt", vrp::VrpScoreKind::Model, model_excess),
        make_pnl("bench_hv_iv_gap", vrp::VrpScoreKind::Benchmark, bench_excess)};
  };
  const vrp::VrpPnlFloor floor = make_floor(kMeasuredFloor);
  // A tie is not a win -- on ICs...
  const std::vector<vrp::VrpScoreReport> tie{
      make_score("gbt", vrp::VrpScoreKind::Model, 0.20, 0.30),
      make_score("bench_hv_iv_gap", vrp::VrpScoreKind::Benchmark, 0.20, 0.30)};
  EXPECT_FALSE(vrp::vrp_gate_verdict(tie, pnl(2.0, 1.0), floor).pass);
  // ...and on money.
  const std::vector<vrp::VrpScoreReport> wins_ic{
      make_score("gbt", vrp::VrpScoreKind::Model, 0.40, 0.60),
      make_score("bench_hv_iv_gap", vrp::VrpScoreKind::Benchmark, 0.20, 0.30)};
  EXPECT_FALSE(vrp::vrp_gate_verdict(wins_ic, pnl(1.0, 1.0), floor).pass);
  // Winning both ICs while earning LESS money than the free rule is not a win:
  // hv_iv_gap outearned the model IV-neutralised on SP100 and that is the fact
  // the gate has to be able to see.
  EXPECT_FALSE(vrp::vrp_gate_verdict(wins_ic, pnl(0.5, 1.0), floor).pass);
  // Beating every benchmark while still earning LESS per unit of gross vega
  // than shorting the universe blind is not selection: the floor is absolute.
  EXPECT_FALSE(vrp::vrp_gate_verdict(wins_ic, pnl(-0.5, -1.0), floor).pass);
  EXPECT_TRUE(vrp::vrp_gate_verdict(wins_ic, pnl(0.5, -1.0), floor).pass);
  // A run with no money measured at all is ungraded, therefore not passing.
  EXPECT_FALSE(vrp::vrp_gate_verdict(wins_ic, {}, floor).pass);
  // Winning on rank while losing on level is the state that produced a book
  // with rank skill and no currency edge. Both must clear.
  const std::vector<vrp::VrpScoreReport> rank_only{
      make_score("gbt", vrp::VrpScoreKind::Model, 0.05, 0.90),
      make_score("bench_hv_iv_gap", vrp::VrpScoreKind::Benchmark, 0.20, 0.30)};
  EXPECT_FALSE(vrp::vrp_gate_verdict(rank_only, pnl(2.0, 1.0), floor).pass);
  // An ungraded run must never read as a passing one.
  const std::vector<vrp::VrpScoreReport> no_bench{
      make_score("gbt", vrp::VrpScoreKind::Model, 0.90, 0.90)};
  EXPECT_FALSE(vrp::vrp_gate_verdict(no_bench, pnl(2.0, 1.0), floor).pass);
  EXPECT_EQ(vrp::vrp_gate_verdict(no_bench, pnl(2.0, 1.0), floor).n_benchmarks, 0u);
  const std::vector<vrp::VrpScoreReport> no_model{
      make_score("bench_hv_iv_gap", vrp::VrpScoreKind::Benchmark, 0.20, 0.30)};
  EXPECT_FALSE(vrp::vrp_gate_verdict(no_model, pnl(2.0, 1.0), floor).pass);
  EXPECT_TRUE(vrp::vrp_gate_verdict(no_model, pnl(2.0, 1.0), floor).model.empty());
  // NaN on either side is unmeasurable, therefore not won.
  const std::vector<vrp::VrpScoreReport> nan_model{
      make_score("gbt", vrp::VrpScoreKind::Model, kNaN, 0.90),
      make_score("bench_hv_iv_gap", vrp::VrpScoreKind::Benchmark, 0.20, 0.30)};
  EXPECT_FALSE(vrp::vrp_gate_verdict(nan_model, pnl(2.0, 1.0), floor).pass);
  const std::vector<vrp::VrpScoreReport> nan_bench{
      make_score("gbt", vrp::VrpScoreKind::Model, 0.90, 0.90),
      make_score("bench_hv_iv_gap", vrp::VrpScoreKind::Benchmark, kNaN, kNaN)};
  EXPECT_FALSE(vrp::vrp_gate_verdict(nan_bench, pnl(2.0, 1.0), floor).pass);
  EXPECT_FALSE(vrp::vrp_gate_verdict(wins_ic, pnl(kNaN, 1.0), floor).pass);
  EXPECT_FALSE(vrp::vrp_gate_verdict(wins_ic, pnl(2.0, kNaN), floor).pass);
  // A MEASURED benchmark must be named as the bar even when an unmeasurable
  // one arrives first -- otherwise a leading NaN latches and the report names
  // the wrong thing for the model to answer for.
  const std::vector<vrp::VrpScoreReport> nan_first{
      make_score("gbt", vrp::VrpScoreKind::Model, 0.90, 0.90),
      make_score("bench_unmeasurable", vrp::VrpScoreKind::Benchmark, kNaN, kNaN),
      make_score("bench_hv_iv_gap", vrp::VrpScoreKind::Benchmark, 0.20, 0.30)};
  EXPECT_EQ(vrp::vrp_gate_verdict(nan_first, pnl(2.0, 1.0), floor).best_benchmark,
            "bench_hv_iv_gap");
  EXPECT_FALSE(vrp::vrp_gate_verdict(nan_first, pnl(2.0, 1.0), floor).pass);
}

// ── ROUND 5: money, and the floor every candidate must clear ────────────────

TEST(VrpTrainMath, PpvAndTheShortEverythingFloorMatchHandComputation) {
  // ppv = 100 * (rv^2 - iv^2) / (2 * iv), the hold-to-horizon carry of a
  // daily-delta-hedged ATM straddle carrying 1 unit of vega.
  EXPECT_DOUBLE_EQ(vrp::vrp_ppv_raw(0.30, 0.20), 12.5);
  EXPECT_DOUBLE_EQ(vrp::vrp_ppv_raw(0.10, 0.20), -7.5);
  EXPECT_DOUBLE_EQ(vrp::vrp_ppv_raw(0.10, 0.50), -24.0);
  EXPECT_DOUBLE_EQ(vrp::vrp_ppv_raw(1.00, 0.50), 75.0);
  // A row that cannot be priced is undefined, never 0.0 -- iv_fair is the
  // denominator and a tail row carries no realized leg at all.
  EXPECT_TRUE(std::isnan(vrp::vrp_ppv_raw(kNaN, 0.20)));
  EXPECT_TRUE(std::isnan(vrp::vrp_ppv_raw(0.30, 0.0)));
  EXPECT_TRUE(std::isnan(vrp::vrp_ppv_raw(0.30, -0.20)));

  const std::vector<double> rv{0.30, 0.10, 0.10, 1.00, kNaN};
  const std::vector<double> iv{0.20, 0.20, 0.50, 0.50, 0.20};
  const vrp::VrpPpvSeries s = vrp::vrp_build_ppv(rv, iv);
  ASSERT_EQ(s.ppv.size(), 5u);
  EXPECT_EQ(s.n_priced, 4u);
  // The +16465-vol-point unadjusted-split rows are the reason the cap exists,
  // and the count is published rather than hidden.
  EXPECT_EQ(s.n_winsorized, 1u);
  EXPECT_DOUBLE_EQ(s.ppv[3], vrp::kVrpPpvWinsorAbs);
  EXPECT_TRUE(std::isnan(s.ppv[4]));

  // Shorting earns -ppv, so the floor is the negated per-date cross-sectional
  // mean: date 1 = -(12.5 - 7.5 - 24.0)/3, date 2 = -60.
  const std::vector<std::int64_t> ts{1, 1, 1, 2, 2};
  const vrp::VrpPnlFloor floor = vrp::vrp_short_everything_floor(ts, s.ppv);
  ASSERT_EQ(floor.per_date.size(), 2u);
  EXPECT_NEAR(floor.per_date[0], 19.0 / 3.0, 1e-12);
  EXPECT_DOUBLE_EQ(floor.per_date[1], -60.0);
  EXPECT_NEAR(floor.mean, (19.0 / 3.0 - 60.0) / 2.0, 1e-12);
  EXPECT_EQ(floor.n_dates, 2u);
}

TEST(VrpTrainMath, PnlExcessIsThePairedPerDateDifferenceFromTheFloor) {
  // The excess is a PAIRED per-date statistic, not (mean book - mean floor):
  // a date the book could not trade must not contribute its floor either.
  const std::vector<double> book{2.0, 4.0, kNaN};
  const std::vector<double> floor{1.0, 1.0, 5.0};
  const vrp::VrpPnlAgg a = vrp::vrp_pnl_agg(book, floor);
  EXPECT_DOUBLE_EQ(a.mean, 3.0);
  EXPECT_EQ(a.n_dates, 2u);
  EXPECT_DOUBLE_EQ(a.excess, 2.0);
  // The naive difference of the two means would have been +0.667. It is not
  // the same number, and the paired one is the honest one.
  EXPECT_NE(a.excess, 3.0 - (1.0 + 1.0 + 5.0) / 3.0);
  // Misaligned inputs are undefined, never faked.
  const std::vector<double> shorter{1.0};
  EXPECT_TRUE(std::isnan(vrp::vrp_pnl_agg(book, shorter).excess));
}

TEST(VrpTrainMath, IvNeutralisationStripsAStaticVolLevelTiltFromTheBook) {
  // Ten names on one date. iv rises 0.10 -> 0.55; the score is -iv (the
  // contaminated rule: long the cheap names, short the expensive ones); and
  // ppv is a pure function of the IV QUINTILE, constant inside each pair.
  //
  // The decile book therefore harvests the whole IV-level gradient, while the
  // IV-neutralised book -- which can only compare names inside one quintile --
  // must read EXACTLY ZERO. That is the audit's finding reproduced in a
  // closed-form fixture: 90% of -iv_fair_21d's P&L was a static vol-level tilt.
  std::vector<std::int64_t> ts(10, 7);
  std::vector<double> iv;
  std::vector<double> score;
  const std::vector<double> ppv{14.0, 14.0, 4.0, 4.0, -6.0, -6.0, -16.0, -16.0, -26.0, -26.0};
  for (std::size_t i = 0; i < 10; ++i) {
    iv.push_back(0.10 + 0.05 * static_cast<double>(i));
    score.push_back(-iv.back());
  }
  const vrp::VrpPnlFloor floor = vrp::vrp_short_everything_floor(ts, ppv);
  ASSERT_EQ(floor.per_date.size(), 1u);
  EXPECT_DOUBLE_EQ(floor.per_date[0], 6.0); // -mean(ppv) = -(-6) = +6

  // Decile book, halved because the quoted spread costs 2u of gross vega.
  const std::vector<double> dec = vrp::vrp_decile_book_per_date(ts, score, ppv);
  ASSERT_EQ(dec.size(), 1u);
  EXPECT_DOUBLE_EQ(dec[0], 0.5 * (14.0 - (-26.0)));

  const std::vector<double> ivn = vrp::vrp_iv_neutral_book_per_date(ts, score, ppv, iv);
  ASSERT_EQ(ivn.size(), 1u);
  EXPECT_DOUBLE_EQ(ivn[0], 0.0);

  // And the number that decides anything is the excess over doing nothing:
  // +20 raw becomes +14, and the neutralised book is BELOW the floor at -6.
  EXPECT_DOUBLE_EQ(vrp::vrp_pnl_agg(dec, floor.per_date).excess, 14.0);
  EXPECT_DOUBLE_EQ(vrp::vrp_pnl_agg(ivn, floor.per_date).excess, -6.0);

  // A score that genuinely selects INSIDE each IV quintile survives the
  // transform: flip the sign of ppv dispersion within pairs and neutralise.
  const std::vector<double> ppv_within{20.0, 8.0, 10.0, -2.0, 0.0, -12.0,
                                       -10.0, -22.0, -20.0, -32.0};
  const std::vector<double> ivn2 =
      vrp::vrp_iv_neutral_book_per_date(ts, score, ppv_within, iv);
  ASSERT_EQ(ivn2.size(), 1u);
  // Inside every quintile the higher score (lower iv) carries +12 more ppv.
  EXPECT_DOUBLE_EQ(ivn2[0], 6.0);

  // Dates thinner than two names per quintile contribute NOTHING rather than a
  // fabricated tail -- the same contract as the decile floor.
  const std::vector<std::int64_t> thin_ts(9, 7);
  const std::vector<double> thin(9, 1.0);
  const std::vector<double> thin_out =
      vrp::vrp_iv_neutral_book_per_date(thin_ts, thin, thin, thin);
  ASSERT_EQ(thin_out.size(), 1u);
  EXPECT_TRUE(std::isnan(thin_out[0]));
}

// ── ROUND 4 F4: feature lagging ─────────────────────────────────────────────

TEST(VrpTrainMath, FeatureLagShiftsToTheKthSameSymbolPredecessorAndCountsWarmup) {
  const ScopedTempFile panel("lag_unit", make_synth_panel_tsv());
  const auto loaded = vrp::load_vrp_panel(panel.path_string());
  ASSERT_TRUE(loaded.has_value()) << loaded.error().to_string();
  const vrp::VrpPanel original = *loaded;
  vrp::VrpPanel lagged = *loaded;

  // The synthetic panel plants NaN f4 cells, and NaN != NaN, so identity has
  // to be checked bit-for-bit rather than with operator==.
  const auto same_features = [](const std::array<double, kVrpFeatureCount> &a,
                                const std::array<double, kVrpFeatureCount> &b) {
    for (std::size_t f = 0; f < kVrpFeatureCount; ++f) {
      if (!((std::isnan(a[f]) && std::isnan(b[f])) || a[f] == b[f])) {
        return false;
      }
    }
    return true;
  };

  // Lag 0 is the identity and reports no attrition.
  vrp::VrpPanel identity = original;
  EXPECT_EQ(vrp::apply_vrp_feature_lag(identity, 0), 0u);
  for (std::size_t r = 0; r < identity.rows.size(); ++r) {
    EXPECT_TRUE(same_features(identity.rows[r].f, original.rows[r].f)) << r;
  }

  constexpr std::size_t kLag = 2;
  const std::size_t blanked = vrp::apply_vrp_feature_lag(lagged, kLag);
  // Exactly `lag` warmup rows per symbol have no k-th predecessor.
  EXPECT_EQ(blanked, kLag * kSynthSymbols.size());

  // Per symbol, row i must now carry the features of row i-lag, and the
  // TARGET side must be untouched.
  std::vector<std::vector<std::size_t>> by_symbol(original.symbols.size());
  for (std::size_t r = 0; r < original.rows.size(); ++r) {
    by_symbol[original.row_symbol[r]].push_back(r);
  }
  for (const auto &rows : by_symbol) {
    for (std::size_t i = 0; i < rows.size(); ++i) {
      const auto &out = lagged.rows[rows[i]];
      const auto &in = original.rows[rows[i]];
      EXPECT_EQ(out.symbol, in.symbol);
      EXPECT_EQ(out.date, in.date);
      EXPECT_EQ(out.entry_ts_ns, in.entry_ts_ns);
      EXPECT_DOUBLE_EQ(out.iv_fair_21d, in.iv_fair_21d);
      EXPECT_TRUE((std::isnan(out.label) && std::isnan(in.label)) || out.label == in.label);
      if (i < kLag) {
        for (const double v : out.f) {
          EXPECT_TRUE(std::isnan(v));
        }
      } else {
        for (std::size_t f = 0; f < kVrpFeatureCount; ++f) {
          const double src = original.rows[rows[i - kLag]].f[f];
          EXPECT_TRUE((std::isnan(out.f[f]) && std::isnan(src)) || out.f[f] == src);
        }
      }
    }
  }
}

TEST(VrpTrainMath, FeatureLagPastTheCapFailsClosed) {
  const ScopedTempFile panel("lag_cap", make_synth_panel_tsv());
  vrp::VrpTrainConfig cfg =
      make_synth_config(panel.path_string(), unique_temp_path("lag_cap_out", "").string());
  cfg.feature_lag = vrp::kVrpMaxFeatureLag + 1;
  const auto report = vrp::run_vrp_train(cfg);
  ASSERT_FALSE(report.has_value());
  EXPECT_NE(report.error().to_string().find("feature-lag"), std::string::npos);
}

TEST(VrpTrainPipeline, FeatureLagLeavesTheFoldPlanIdenticalAndMovesTheForecasts) {
  // Targets are never shifted, so the observation set and the fold plan are
  // the SAME at every lag -- which is what makes a lag-to-lag comparison a
  // comparison rather than two different experiments. The forecasts do move.
  const ScopedTempFile panel("lag_pipe", make_synth_panel_tsv());
  const auto out0 = unique_temp_path("lag_pipe_0", "");
  const auto out2 = unique_temp_path("lag_pipe_2", "");
  vrp::VrpTrainConfig cfg0 = make_synth_config(panel.path_string(), out0.string());
  vrp::VrpTrainConfig cfg2 = cfg0;
  cfg2.out_dir = out2.string();
  cfg2.feature_lag = 2;
  const auto a = vrp::run_vrp_train(cfg0);
  const auto b = vrp::run_vrp_train(cfg2);
  ASSERT_TRUE(a.has_value()) << a.error().to_string();
  ASSERT_TRUE(b.has_value()) << b.error().to_string();
  EXPECT_EQ(a->feature_lag_rows_unavailable, 0u);
  EXPECT_EQ(b->feature_lag_rows_unavailable, 2u * kSynthSymbols.size());
  ASSERT_EQ(a->folds.size(), b->folds.size());
  bool any_pred_moved = false;
  for (std::size_t k = 0; k < a->folds.size(); ++k) {
    EXPECT_EQ(a->folds[k].train_rows, b->folds[k].train_rows);
    EXPECT_EQ(a->folds[k].test_rows, b->folds[k].test_rows);
    any_pred_moved = any_pred_moved || a->folds[k].test_pred_raw != b->folds[k].test_pred_raw;
  }
  EXPECT_TRUE(any_pred_moved);
  const std::string bytes = read_file_bytes(b->metrics_path);
  EXPECT_NE(bytes.find("# feature_lag=2\n"), std::string::npos);
  EXPECT_NE(bytes.find("# feature_lag_rows_unavailable=" +
                       std::to_string(b->feature_lag_rows_unavailable) + "\n"),
            std::string::npos);
  EXPECT_NE(read_file_bytes(a->metrics_path).find("# feature_lag=0\n"), std::string::npos);
  std::error_code ec;
  std::filesystem::remove_all(out0, ec);
  std::filesystem::remove_all(out2, ec);
}

// ── ROUND 4 F1: the ranking column ──────────────────────────────────────────

TEST_F(VrpTrainPipelineTest, CrossSectionEdgeNormPreservesWithinDateOrderOfPredLabel) {
  // THE point of the default: within each date the ranking column must order
  // the names exactly as pred_label does, so the book ranks on the axis the
  // IC is measured on. Ranking on the per-symbol z-score instead was measured
  // at -1.63 vol pts/cycle against pred_label's +1.74.
  const auto signal = load_vrp_signal_v1(report_->signal_path.string());
  ASSERT_TRUE(signal.has_value()) << signal.error().to_string();
  ASSERT_FALSE(signal->empty());
  // Group the emitted rows by date (the signal file is written in canonical
  // panel-row order, so equal dates are contiguous).
  std::size_t begin = 0;
  std::size_t dates_checked = 0;
  std::size_t pairs_checked = 0;
  while (begin < signal->size()) {
    std::size_t end = begin;
    while (end < signal->size() && (*signal)[end].date == (*signal)[begin].date) {
      ++end;
    }
    if (end - begin >= 2) {
      ++dates_checked;
      for (std::size_t i = begin; i < end; ++i) {
        for (std::size_t j = i + 1; j < end; ++j) {
          const auto &a = (*signal)[i];
          const auto &b = (*signal)[j];
          ++pairs_checked;
          if (a.pred_label < b.pred_label) {
            EXPECT_LT(a.pred_edge_norm, b.pred_edge_norm) << a.date << ' ' << a.symbol;
          } else if (a.pred_label > b.pred_label) {
            EXPECT_GT(a.pred_edge_norm, b.pred_edge_norm) << a.date << ' ' << a.symbol;
          } else {
            EXPECT_DOUBLE_EQ(a.pred_edge_norm, b.pred_edge_norm);
          }
        }
      }
      // The column really is a z-score: mean ~ 0 and population sd ~ 1.
      double sum = 0.0;
      for (std::size_t i = begin; i < end; ++i) {
        sum += (*signal)[i].pred_edge_norm;
      }
      const auto n = static_cast<double>(end - begin);
      const double mean = sum / n;
      double sq = 0.0;
      for (std::size_t i = begin; i < end; ++i) {
        const double d = (*signal)[i].pred_edge_norm - mean;
        sq += d * d;
      }
      EXPECT_NEAR(mean, 0.0, 1e-9);
      EXPECT_NEAR(std::sqrt(sq / n), 1.0, 1e-9);
    }
    begin = end;
  }
  EXPECT_GT(dates_checked, 0u);
  EXPECT_GT(pairs_checked, 0u);
  const std::string bytes = read_file_bytes(report_->metrics_path);
  EXPECT_NE(bytes.find("# edge_norm=cross_section\n"), std::string::npos);
}

TEST_F(VrpTrainPipelineTest, PerSymbolEdgeNormIsReproducibleFromTheSidecarAlone) {
  // The round-1..3 column, now behind --edge-norm per-symbol so the round-3
  // artifacts stay byte-reproducible: pred_edge_norm is exactly
  // (pred_label - label_mean[sym]) / label_sd[sym] from the fold sidecar, and
  // the two modes genuinely disagree (otherwise the flag would be theatre).
  const auto out = unique_temp_path("per_symbol_out", "");
  vrp::VrpTrainConfig cfg = make_synth_config(panel_file_->path_string(), out.string());
  cfg.edge_norm = vrp::VrpEdgeNormMode::PerSymbol;
  const auto ps = vrp::run_vrp_train(cfg);
  ASSERT_TRUE(ps.has_value()) << ps.error().to_string();

  // Everything except the ranking column is untouched by the flag.
  EXPECT_EQ(read_file_bytes(ps->gbt_model_path), read_file_bytes(report_->gbt_model_path));
  EXPECT_EQ(read_file_bytes(ps->baseline_model_path),
            read_file_bytes(report_->baseline_model_path));
  EXPECT_EQ(read_file_bytes(ps->fold_stats_path), read_file_bytes(report_->fold_stats_path));
  EXPECT_NE(read_file_bytes(ps->signal_path), read_file_bytes(report_->signal_path));

  const auto sidecar = vrp::load_vrp_fold_stats(ps->fold_stats_path);
  ASSERT_TRUE(sidecar.has_value()) << sidecar.error().to_string();
  const vrp::VrpFoldStats &fs = sidecar->back();
  const auto signal = load_vrp_signal_v1(ps->signal_path.string());
  ASSERT_TRUE(signal.has_value()) << signal.error().to_string();

  // Check the tail rows, which the FINAL fold's stats score.
  std::size_t checked = 0;
  for (const auto &srow : *signal) {
    const auto it = std::find_if(report_->panel.rows.begin(), report_->panel.rows.end(),
                                 [&](const vrp::VrpPanelRow &pr) {
                                   return pr.symbol == srow.symbol && pr.date == srow.date;
                                 });
    ASSERT_NE(it, report_->panel.rows.end());
    if (!std::isnan(it->label)) {
      continue; // fold rows use their own fold's stats, not the last fold's
    }
    const auto sym_it = std::lower_bound(fs.symbols.begin(), fs.symbols.end(), srow.symbol);
    ASSERT_TRUE(sym_it != fs.symbols.end() && *sym_it == srow.symbol);
    const auto s = static_cast<std::size_t>(sym_it - fs.symbols.begin());
    const double sd = fs.label_sd[s];
    const double expected = sd == 0.0 ? 0.0 : (srow.pred_label - fs.label_mean[s]) / sd;
    EXPECT_NEAR(expected, srow.pred_edge_norm, 1e-9) << srow.symbol << ' ' << srow.date;
    ++checked;
  }
  EXPECT_GT(checked, 0u);
  EXPECT_NE(read_file_bytes(ps->metrics_path).find("# edge_norm=per_symbol\n"),
            std::string::npos);
  // The break the gate now makes visible: under per-symbol the column the book
  // RANKS on is a different axis from the column the IC is measured on, so its
  // rank IC diverges from the model's on EVERY target. Under cross-section the
  // two coincide (pinned by GateScoresEveryColumnOnAllThreeTargetAxes).
  constexpr std::size_t kAxes = vrp::kVrpTargetAxes.size();
  constexpr std::size_t kRankedCol = 5;
  ASSERT_EQ(ps->gate.pooled.size(), vrp::kVrpGateColumnCount * kAxes);
  for (std::size_t a = 0; a < kAxes; ++a) {
    const auto &ranked = ps->gate.pooled[kRankedCol * kAxes + a];
    EXPECT_EQ(ranked.name, "ranked_pred_edge_norm");
    EXPECT_NE(ranked.ic_spearman, ps->gate.pooled[a].ic_spearman);
  }
  std::error_code ec;
  std::filesystem::remove_all(out, ec);
}

TEST(VrpTrainMath, CrossSectionEdgeNormEmitsZeroOnDegenerateDatesAndNeverNonFinite) {
  // The frozen vrp_signal_v1 loader fail-closes on a non-finite column, so a
  // date with no dispersion (or a lone name) must emit 0.0, matching the
  // per-symbol path's sd == 0 convention -- never NaN, never inf.
  vrp::VrpPanel panel;
  const auto add = [&panel](std::string sym, std::int64_t ts) {
    vrp::VrpPanelRow row;
    row.symbol = std::move(sym);
    row.date = std::to_string(ts);
    row.entry_ts_ns = ts;
    panel.rows.push_back(std::move(row));
  };
  add("AAA", 1); // date 1: a single name
  add("AAA", 2); // date 2: two names, identical forecasts
  add("BBB", 2);
  add("AAA", 3); // date 3: two names, one forecast non-finite
  add("BBB", 3);
  add("AAA", 4); // date 4: genuine dispersion
  add("BBB", 4);
  panel.symbols = {"AAA", "BBB"};
  panel.row_symbol = {0, 0, 1, 0, 1, 0, 1};
  std::vector<vrp::detail::SignalEntry> e(panel.rows.size());
  for (std::size_t i = 0; i < e.size(); ++i) {
    e[i].panel_row = i;
  }
  e[0].pred_label = 5.0;
  e[1].pred_label = 3.0;
  e[2].pred_label = 3.0;
  e[3].pred_label = kNaN;
  e[4].pred_label = 2.0;
  e[5].pred_label = -1.0;
  e[6].pred_label = 1.0;
  vrp::detail::apply_cross_section_edge_norm(panel, std::span<vrp::detail::SignalEntry>{e});
  for (const auto &entry : e) {
    EXPECT_TRUE(std::isfinite(entry.pred_edge_norm));
  }
  EXPECT_DOUBLE_EQ(e[0].pred_edge_norm, 0.0); // lone name
  EXPECT_DOUBLE_EQ(e[1].pred_edge_norm, 0.0); // zero dispersion
  EXPECT_DOUBLE_EQ(e[2].pred_edge_norm, 0.0);
  EXPECT_DOUBLE_EQ(e[3].pred_edge_norm, 0.0); // fewer than two finite forecasts
  EXPECT_DOUBLE_EQ(e[4].pred_edge_norm, 0.0);
  // date 4: mean 0, population sd 1 -> exactly -1 and +1.
  EXPECT_DOUBLE_EQ(e[5].pred_edge_norm, -1.0);
  EXPECT_DOUBLE_EQ(e[6].pred_edge_norm, 1.0);
}

// ── ROUND 4 F2/F3: the gate in the artifacts ────────────────────────────────

TEST_F(VrpTrainPipelineTest, GateScoresEveryColumnOnEveryTargetAxis) {
  const auto &gate = report_->gate;
  constexpr std::size_t kAxes = vrp::kVrpTargetAxes.size();
  constexpr std::size_t kCols = vrp::kVrpGateColumnCount;
  constexpr std::size_t kRankedCol = 5;
  ASSERT_EQ(gate.per_fold.size(), report_->folds.size());
  ASSERT_EQ(gate.pooled.size(), kCols * kAxes);
  ASSERT_EQ(gate.pooled_pnl.size(), kCols);
  ASSERT_EQ(gate.pooled_vega.size(), kCols);
  // Column order and kinds: the model on trial, the fitted baseline, the three
  // surviving zero-parameter benchmarks, the column the BOOK actually ranks on,
  // and -- last, and structurally non-gating -- the two contaminated appendices.
  const std::array<std::pair<std::string, vrp::VrpScoreKind>, kCols> expect_cols{
      std::pair{std::string{"gbt"}, vrp::VrpScoreKind::Model},
      std::pair{std::string{"baseline_log_har"}, vrp::VrpScoreKind::Baseline},
      std::pair{std::string{"bench_hv_iv_gap"}, vrp::VrpScoreKind::Benchmark},
      std::pair{std::string{"bench_neg_iv_atmf_21d"}, vrp::VrpScoreKind::Benchmark},
      std::pair{std::string{"bench_term_slope"}, vrp::VrpScoreKind::Benchmark},
      std::pair{std::string{"ranked_pred_edge_norm"}, vrp::VrpScoreKind::Ranked},
      std::pair{std::string{"contaminated_neg_iv_fair_21d"},
                vrp::VrpScoreKind::Contaminated},
      std::pair{std::string{"contaminated_iv63_minus_iv_atmf21"},
                vrp::VrpScoreKind::Contaminated}};
  const std::array<vrp::VrpTargetAxis, kAxes> expect_axes{
      vrp::VrpTargetAxis::RvFwd, vrp::VrpTargetAxis::VolChg, vrp::VrpTargetAxis::Label,
      vrp::VrpTargetAxis::IvChgRaw, vrp::VrpTargetAxis::IvChgRoll};
  for (std::size_t c = 0; c < kCols; ++c) {
    EXPECT_EQ(gate.pooled_pnl[c].name, expect_cols[c].first);
    EXPECT_EQ(gate.pooled_pnl[c].kind, expect_cols[c].second);
    EXPECT_EQ(gate.pooled_vega[c].name, expect_cols[c].first);
    EXPECT_EQ(gate.pooled_vega[c].kind, expect_cols[c].second);
    for (std::size_t a = 0; a < kAxes; ++a) {
      const auto &s = gate.pooled[c * kAxes + a];
      EXPECT_EQ(s.name, expect_cols[c].first);
      EXPECT_EQ(s.kind, expect_cols[c].second);
      EXPECT_EQ(s.target, expect_axes[a]);
    }
  }
  // THREE benchmarks. Neither contaminated column counts: -iv_fair_21d is
  // +1.0000 correlated with the label's own implied leg by algebra, and
  // iv_fair_63d - iv_atmf_21d carries the iv-change target's own entry mark.
  EXPECT_EQ(gate.verdict.n_benchmarks, 3u);
  EXPECT_EQ(gate.vega_verdict.n_benchmarks, 3u);
  // The primary axis is the REALIZED leg, and it is a different number from
  // the contaminated composite -- which is the entire point of grading both.
  EXPECT_NE(gate.pooled[0].ic_spearman, gate.pooled[2].ic_spearman);
  EXPECT_DOUBLE_EQ(gate.verdict.model_ic_spearman, gate.pooled[0].ic_spearman);
  EXPECT_DOUBLE_EQ(gate.verdict.model_ic_pearson, gate.pooled[0].ic_pearson);
  // Under the cross-section default the ranking column is an order-preserving
  // map of pred_label WITHIN each date, so its per-date rank IC and its decile
  // tails must coincide with the model's -- the round-1..3 gap between the
  // scored column and the traded one closes to zero by construction. Holds on
  // every MEASURABLE axis, because the transform is on the SCORE side. This
  // fixture is a vrp_panel_v1 file, so the two iv-change axes carry no ATM-
  // forward leg at all -- asserted below rather than skipped silently.
  constexpr std::size_t kMeasurableAxes = 3;
  for (std::size_t a = 0; a < kMeasurableAxes; ++a) {
    EXPECT_NEAR(gate.pooled[kRankedCol * kAxes + a].ic_spearman, gate.pooled[a].ic_spearman,
                1e-9);
    // Pearson is invariant to a positive affine map, so the PER-DATE Pearson
    // IC coincides too. The pooled-across-dates Pearson deliberately does NOT:
    // the z-score is date-specific, which is exactly why a pooled-row
    // correlation is the wrong statistic for a per-date cross-sectional book.
    EXPECT_NEAR(gate.pooled[kRankedCol * kAxes + a].ic_pearson, gate.pooled[a].ic_pearson,
                1e-9);
    EXPECT_NE(gate.pooled[kRankedCol * kAxes + a].ic_pearson_pooled,
              gate.pooled[a].ic_pearson_pooled);
  }
  // A v1 panel has no ATM-forward leg, so BOTH iv-change axes and the whole
  // vega book are undefined -- never quietly built on iv_fair_21d, which is the
  // variance-swap strip strike and sits ~4.24 vol points above the ATMF point.
  // The vega gate therefore FAILS, because an unmeasurable run is not a passing
  // one.
  for (std::size_t c = 0; c < kCols; ++c) {
    for (std::size_t a = kMeasurableAxes; a < kAxes; ++a) {
      const auto &s = gate.pooled[c * kAxes + a];
      EXPECT_TRUE(std::isnan(s.ic_pearson)) << c << ',' << a;
      EXPECT_TRUE(std::isnan(s.ic_spearman)) << c << ',' << a;
      EXPECT_EQ(s.n_dates, 0u) << c << ',' << a;
    }
    EXPECT_TRUE(std::isnan(gate.pooled_vega[c].decile.net.mean)) << c;
    EXPECT_TRUE(std::isnan(gate.pooled_vega[c].iv_neutral.excess)) << c;
  }
  EXPECT_TRUE(std::isnan(gate.pooled_vega_floor.long_all.mean));
  EXPECT_TRUE(std::isnan(gate.pooled_vega_floor.short_all.mean));
  EXPECT_FALSE(gate.vega_verdict.pass);
  EXPECT_EQ(gate.n_iv_chg_rows_no_exit + gate.n_iv_chg_rows,
            report_->panel.rows.size());
  // This fixture carries 3 names per date, below the one-name-per-decile floor
  // AND below the two-names-per-IV-quintile floor, so every tail and every
  // book is UNDEFINED rather than fabricated from three names. The floor
  // itself needs only one priced name per date, so it IS measurable -- pinned
  // because "the book is unmeasurable" and "doing nothing pays nothing" are
  // different statements.
  EXPECT_TRUE(std::isnan(gate.pooled[0].decile_spread));
  EXPECT_TRUE(std::isnan(gate.pooled[0].decile_rho));
  EXPECT_TRUE(std::isnan(gate.pooled_pnl[0].decile.mean));
  EXPECT_TRUE(std::isnan(gate.pooled_pnl[0].iv_neutral.mean));
  EXPECT_TRUE(std::isnan(gate.pooled_pnl[0].iv_neutral.excess));
  EXPECT_TRUE(std::isfinite(gate.pooled_floor.mean));
  EXPECT_GT(gate.pooled_floor.n_dates, 0u);
  EXPECT_DOUBLE_EQ(gate.verdict.pnl_floor, gate.pooled_floor.mean);
  // An unmeasurable book cannot clear the floor, so the run cannot pass.
  EXPECT_FALSE(gate.verdict.pass);
  // Every row that carries a realized leg is priced, and this clean fixture
  // trips the winsorization cap on nothing.
  EXPECT_EQ(gate.n_ppv_rows_priced, gate.pooled[0].n_rows);
  EXPECT_EQ(gate.n_ppv_rows_winsorized, 0u);
  // Every fold scores the same columns on that fold's own test rows.
  for (std::size_t i = 0; i < gate.per_fold.size(); ++i) {
    ASSERT_EQ(gate.per_fold[i].scores.size(), kCols * kAxes);
    ASSERT_EQ(gate.per_fold[i].pnl.size(), kCols);
    EXPECT_EQ(gate.per_fold[i].fold_id, report_->folds[i].fold_id);
    EXPECT_TRUE(std::isfinite(gate.per_fold[i].floor.mean));
    ASSERT_EQ(gate.per_fold[i].vega.size(), kCols);
    for (std::size_t k = 0; k < gate.per_fold[i].scores.size(); ++k) {
      const auto &s = gate.per_fold[i].scores[k];
      EXPECT_EQ(s.n_rows, report_->folds[i].n_test);
      // On this v1 fixture nothing that touches iv_atmf_21d is measurable:
      // neither the two iv-change AXES nor the two COLUMNS built from that
      // leg (bench_neg_iv_atmf_21d, contaminated_iv63_minus_iv_atmf21). Both
      // kinds of hole are asserted, not skipped -- an undefined statistic that
      // silently reads as zero is the failure mode this gate exists to stop.
      const std::size_t col = k / kAxes;
      const bool col_needs_atmf = (col == 3 || col == 7);
      const bool axis_needs_atmf = (k % kAxes) >= kMeasurableAxes;
      if (col_needs_atmf || axis_needs_atmf) {
        EXPECT_EQ(s.n_dates, 0u) << k;
      } else {
        EXPECT_GT(s.n_dates, 0u) << k;
      }
    }
    // MSE / Mincer-Zarnowitz are LABEL-unit claims: populated for the model
    // and the baseline ON THE LABEL AXIS ONLY. A level loss against
    // rv_fwd_21d or against a log vol ratio is a category error, and it is
    // refused rather than fabricated.
    EXPECT_TRUE(std::isfinite(gate.per_fold[i].scores[2].mse));
    EXPECT_TRUE(std::isfinite(gate.per_fold[i].scores[2].mz_slope));
    EXPECT_TRUE(std::isnan(gate.per_fold[i].scores[0].mse));
    EXPECT_TRUE(std::isnan(gate.per_fold[i].scores[1].mse));
    EXPECT_TRUE(std::isfinite(gate.per_fold[i].scores[1 * kAxes + 2].mse));
    // The benchmark, the ranking z-score and the contaminated column make no
    // level claim on any axis.
    for (std::size_t c = 2; c < kCols; ++c) {
      for (std::size_t a = 0; a < kAxes; ++a) {
        EXPECT_TRUE(std::isnan(gate.per_fold[i].scores[c * kAxes + a].mse));
        EXPECT_TRUE(std::isnan(gate.per_fold[i].scores[c * kAxes + a].mz_slope));
      }
    }
  }
  // Pooled covers every fold's test rows exactly once.
  std::size_t n_test_total = 0;
  for (const auto &m : report_->folds) {
    n_test_total += m.n_test;
  }
  EXPECT_EQ(gate.pooled[0].n_rows, n_test_total);
  // The gate's model column is the SHIPPED forecast: with recalibration off
  // its LABEL-axis rank IC must equal the per-fold number the round-1..3 path
  // already reported (that path scores the composite label).
  EXPECT_NEAR(gate.per_fold.front().scores[2].ic_spearman, report_->folds.front().ic_gbt,
              1e-9);
  // The corpus is named, so a clean-25 number can never be quoted for an
  // SP100 book again by accident.
  EXPECT_FALSE(gate.corpus.empty());
  // Coverage: the tail rows are counted and the fraction is published.
  EXPECT_GT(gate.n_signal_rows_unlabeled, 0u);
  EXPECT_LT(gate.n_signal_rows_unlabeled, gate.n_signal_rows);
  EXPECT_NEAR(gate.frac_unlabeled(),
              static_cast<double>(gate.n_signal_rows_unlabeled) /
                  static_cast<double>(gate.n_signal_rows),
              1e-12);
}

TEST_F(VrpTrainPipelineTest, MetricsFileCarriesTheGateVerdictBenchmarkTableAndCoverage) {
  const std::string bytes = read_file_bytes(report_->metrics_path);
  const auto &gate = report_->gate;
  const auto has = [&](const std::string &line) {
    EXPECT_NE(bytes.find(line), std::string::npos) << line;
  };
  has(std::string("# gate_verdict=") + (gate.verdict.pass ? "PASS" : "FAIL") + "\n");
  has("# gate_model=gbt\n");
  has("# gate_n_benchmarks=3\n");
  // The round-5 vega gate is emitted beside the variance one, with its own
  // rule, its own target axis, and the two honesty lines that keep the raw
  // iv-change axis and the surface-read EIV channel from being read as edge.
  has("# vega_gate_verdict=");
  has("# vega_gate_primary_target=iv_chg_21d_roll\n");
  has("# vega_target_raw_axis_is_not_a_pnl=");
  has("# vega_target_eiv_caveat=");
  has("# vega_cost_one_way_frac_of_premium=" +
      vrp::detail::fmt_double(vrp::kVrpVegaOneWayCostFracOfPremium) + "\n");
  has("# vega_short_haircut=" + vrp::detail::fmt_double(vrp::kVrpDefaultShortVegaHaircut) +
      "\n");
  has("# gate_pooled_vega_floor_long_everything_net_vol_pts=");
  has("# gate_pooled_vega_floor_short_everything_net_vol_pts=");
  has("# gate_pooled_vega_floor_binding=");
  // Gross, cost and net travel together, and the leg split is never optional.
  for (const std::string &stat :
       {std::string{"gross_vol_pts"}, std::string{"cost_vol_pts"},
        std::string{"net_vol_pts"}, std::string{"excess_over_vega_floor"},
        std::string{"long_leg_net_vol_pts"}, std::string{"long_leg_net_vol_pts_t_nw"},
        std::string{"short_leg_net_vol_pts"}, std::string{"short_leg_net_vol_pts_t_nw"},
        std::string{"long_leg_excess_over_long_everything"},
        std::string{"short_leg_excess_over_short_everything"},
        std::string{"haircut_objective_vol_pts"}}) {
    has("# gate_pooled_gbt_vega_decile_" + stat + "=");
    has("# gate_pooled_gbt_vega_iv_neutral_" + stat + "=");
  }
  has("# gate_best_benchmark=" + gate.verdict.best_benchmark + "\n");
  // The corrections are stamped on the artifact so a stale reader cannot
  // mistake this file for a round-4 one, and so the reason -iv_fair_21d is
  // gone travels with the numbers rather than living only in a report.
  has("# gate_primary_target=rv_fwd_21d\n");
  EXPECT_NE(bytes.find("# gate_deleted_benchmark=neg_iv_fair_21d_rank_ic_exactly_plus_"
                       "1.0000_vs_the_labels_own_implied_leg_and_minus_0.6128"),
            std::string::npos);
  has("# gate_deleted_benchmark_reference=.superpowers/sdd/2026-08-15-vrp-ml/"
      "audit-benchmark-contamination.md\n");
  EXPECT_NE(bytes.find("# gate_contaminated_target=label_composite"), std::string::npos);
  // Money, and the floor it must clear, on every P&L claim.
  has("# gate_model_pnl_iv_neutral_excess_over_floor=" +
      vrp::detail::fmt_double(gate.verdict.model_pnl_excess) + "\n");
  has("# gate_best_benchmark_pnl_iv_neutral_excess_over_floor=" +
      vrp::detail::fmt_double(gate.verdict.best_benchmark_pnl_excess) + "\n");
  has("# gate_pnl_floor_short_everything=" +
      vrp::detail::fmt_double(gate.verdict.pnl_floor) + "\n");
  has("# pnl_ppv_winsor_abs=" + vrp::detail::fmt_double(vrp::kVrpPpvWinsorAbs) + "\n");
  has("# pnl_n_rows_priced=" + std::to_string(gate.n_ppv_rows_priced) + "\n");
  has("# pnl_n_rows_winsorized=" + std::to_string(gate.n_ppv_rows_winsorized) + "\n");
  has("# gate_pooled_pnl_floor_short_everything_vol_pts_gross_vega=" +
      vrp::detail::fmt_double(gate.pooled_floor.mean) + "\n");
  has("# gate_pooled_pnl_floor_short_everything_vol_pts_gross_vega_t_nw=" +
      vrp::detail::fmt_double(gate.pooled_floor.t_nw) + "\n");
  for (const auto &p : gate.pooled_pnl) {
    // Raw carry and excess-over-floor share a key stem and are emitted
    // together: quoting one without the other is how short-vol beta read as
    // selection skill for three rounds.
    for (const auto &book : {std::pair{std::string{"decile"}, p.decile},
                             std::pair{std::string{"iv_neutral"}, p.iv_neutral}}) {
      const std::string stem = "# gate_pooled_pnl_" + p.name + "_" + book.first + "_";
      has(stem + "vol_pts_gross_vega=" + vrp::detail::fmt_double(book.second.mean) + "\n");
      has(stem + "vol_pts_gross_vega_t_nw=" + vrp::detail::fmt_double(book.second.t_nw) +
          "\n");
      has(stem + "excess_over_floor=" + vrp::detail::fmt_double(book.second.excess) + "\n");
      has(stem + "excess_over_floor_t_nw=" +
          vrp::detail::fmt_double(book.second.excess_t_nw) + "\n");
    }
  }
  for (const auto &f : gate.per_fold) {
    has("# gate_fold_" + std::to_string(f.fold_id) +
        "_pnl_floor_short_everything_vol_pts_gross_vega=" +
        vrp::detail::fmt_double(f.floor.mean) + "\n");
  }
  // The deleted benchmark's key must not survive anywhere in the artifact.
  EXPECT_EQ(bytes.find("bench_neg_iv_fair_21d"), std::string::npos);
  has("# gate_model_ic_pearson=" + vrp::detail::fmt_double(gate.verdict.model_ic_pearson) +
      "\n");
  has("# gate_model_ic_spearman=" + vrp::detail::fmt_double(gate.verdict.model_ic_spearman) +
      "\n");
  has("# corpus=" + gate.corpus + "\n");
  // QLIKE is retained for round-3 comparability and explicitly disowned: it is
  // undefined on a signed variance spread and the gate never reads it.
  has("# qlike_status=deprecated_undefined_on_signed_label_not_scored_by_gate\n");
  // Coverage honesty (27% of the round-2 run was unvalidated and reported as
  // if it were not).
  has("# n_signal_rows=" + std::to_string(gate.n_signal_rows) + "\n");
  has("# n_signal_rows_unlabeled_tail=" + std::to_string(gate.n_signal_rows_unlabeled) + "\n");
  has("# frac_signal_rows_unlabeled_tail=" + vrp::detail::fmt_double(gate.frac_unlabeled()) +
      "\n");
  // Every score column, on every target axis, pooled AND per fold, with its
  // t-stats and its tail statement -- an IC without a t-stat is how t = -0.96
  // shipped three times, and an IC without a NAMED TARGET is how a +1.0000
  // algebraic identity read as a forecast for a whole round.
  for (const auto &s : gate.pooled) {
    const std::string p = "# gate_pooled_" + s.name + "_" +
                          std::string{vrp::vrp_target_axis_key(s.target)} + "_";
    has(p + "ic_pearson=" + vrp::detail::fmt_double(s.ic_pearson) + "\n");
    has(p + "ic_pearson_t=" + vrp::detail::fmt_double(s.ic_pearson_t) + "\n");
    has(p + "ic_pearson_t_nw=" + vrp::detail::fmt_double(s.ic_pearson_t_nw) + "\n");
    has(p + "ic_spearman=" + vrp::detail::fmt_double(s.ic_spearman) + "\n");
    has(p + "ic_spearman_t_nw=" + vrp::detail::fmt_double(s.ic_spearman_t_nw) + "\n");
    has(p + "ic_pearson_traded=" + vrp::detail::fmt_double(s.ic_pearson_traded) + "\n");
    has(p + "decile_spread=" + vrp::detail::fmt_double(s.decile_spread) + "\n");
    has(p + "decile_spread_t_nw=" + vrp::detail::fmt_double(s.decile_spread_t_nw) + "\n");
    has(p + "decile_rho=" + vrp::detail::fmt_double(s.decile_rho) + "\n");
    has(p + "n_rows=" + std::to_string(s.n_rows) + "\n");
  }
  for (const auto &f : gate.per_fold) {
    for (const auto &s : f.scores) {
      const std::string p = "# gate_fold_" + std::to_string(f.fold_id) + "_" + s.name + "_" +
                            std::string{vrp::vrp_target_axis_key(s.target)} + "_";
      has(p + "ic_pearson=" + vrp::detail::fmt_double(s.ic_pearson) + "\n");
      has(p + "ic_spearman=" + vrp::detail::fmt_double(s.ic_spearman) + "\n");
      has(p + "ic_spearman_t_nw=" + vrp::detail::fmt_double(s.ic_spearman_t_nw) + "\n");
    }
  }
}

// ── ROUND 5: the tradeable vol-change target and the VEGA book ──────────────

namespace {

// Build a tiny panel in memory: `n_dates` sessions x `syms` symbols, with the
// caller filling iv_atmf / iv_fair / iv63 per (date, symbol). Everything the
// iv-change target reads is target-side, so no feature plumbing is needed.
struct MiniPanelSpec {
  std::size_t n_dates{0};
  std::size_t n_syms{0};
  std::function<double(std::size_t, std::size_t)> iv_atmf;
  std::function<double(std::size_t, std::size_t)> iv_fair;
  std::function<double(std::size_t, std::size_t)> iv63;
  // Return false to DROP the (date, symbol) row, which is how a surface gap --
  // and therefore a missing exit mark -- is expressed.
  std::function<bool(std::size_t, std::size_t)> keep;
};

[[nodiscard]] vrp::VrpPanel make_mini_panel(const MiniPanelSpec &spec) {
  vrp::VrpPanel p;
  for (std::size_t d = 0; d < spec.n_dates; ++d) {
    for (std::size_t s = 0; s < spec.n_syms; ++s) {
      if (spec.keep && !spec.keep(d, s)) {
        continue;
      }
      vrp::VrpPanelRow r;
      r.symbol = "S" + std::to_string(s);
      r.date = "D" + std::to_string(d);
      r.entry_ts_ns = static_cast<std::int64_t>(d) * 86400000000000LL;
      r.spot = 100.0;
      r.iv_atmf_21d = spec.iv_atmf ? spec.iv_atmf(d, s) : 0.30;
      r.iv_fair_21d = spec.iv_fair ? spec.iv_fair(d, s) : 0.31;
      r.iv_fair_63d = spec.iv63 ? spec.iv63(d, s) : 0.31;
      r.rv_fwd_21d = 0.30;
      r.label = 0.0;
      r.f.fill(0.0);
      p.rows.push_back(std::move(r));
    }
  }
  std::sort(p.rows.begin(), p.rows.end(),
            [](const vrp::VrpPanelRow &a, const vrp::VrpPanelRow &b) {
              if (a.entry_ts_ns != b.entry_ts_ns) {
                return a.entry_ts_ns < b.entry_ts_ns;
              }
              return a.symbol < b.symbol;
            });
  for (const vrp::VrpPanelRow &r : p.rows) {
    p.symbols.push_back(r.symbol);
  }
  std::sort(p.symbols.begin(), p.symbols.end());
  p.symbols.erase(std::unique(p.symbols.begin(), p.symbols.end()), p.symbols.end());
  for (const vrp::VrpPanelRow &r : p.rows) {
    const auto it = std::lower_bound(p.symbols.begin(), p.symbols.end(), r.symbol);
    p.row_symbol.push_back(static_cast<std::size_t>(it - p.symbols.begin()));
  }
  return p;
}

} // namespace

// The target is a horizon-matched forward MARK, not a smoothed one: the exit
// row must exist at exactly +H pooled sessions or the row is undefined.
TEST(VrpTrainVega, IvChgTargetIsTheExactHorizonForwardMarkMinusItsRollLeg) {
  MiniPanelSpec spec;
  spec.n_dates = 30;
  spec.n_syms = 2;
  // S0: iv rises 0.001/session. S1: iv falls 0.002/session.
  spec.iv_atmf = [](std::size_t d, std::size_t s) {
    return s == 0 ? 0.20 + 0.001 * static_cast<double>(d)
                  : 0.50 - 0.002 * static_cast<double>(d);
  };
  spec.iv_fair = [](std::size_t, std::size_t s) { return s == 0 ? 0.22 : 0.52; };
  spec.iv63 = [](std::size_t, std::size_t s) { return s == 0 ? 0.26 : 0.50; };
  // Punch a hole in S1's session 25 -- so S1's session 4 loses its exit mark.
  spec.keep = [](std::size_t d, std::size_t s) { return !(s == 1 && d == 25); };
  const vrp::VrpPanel panel = make_mini_panel(spec);

  const vrp::VrpIvChgTargets t = vrp::vrp_build_iv_chg(panel, 21);

  // S0 @ d=0 -> d=21: 100*(0.221 - 0.200) = 2.1 vol pts raw.
  // roll leg = 100*(0.26 - 0.22)/2 = 2.0  =>  roll-adjusted 0.1.
  std::size_t s0d0 = panel.rows.size();
  std::size_t s1d4 = panel.rows.size();
  for (std::size_t i = 0; i < panel.rows.size(); ++i) {
    if (panel.rows[i].symbol == "S0" && panel.rows[i].date == "D0") {
      s0d0 = i;
    }
    if (panel.rows[i].symbol == "S1" && panel.rows[i].date == "D4") {
      s1d4 = i;
    }
  }
  ASSERT_LT(s0d0, panel.rows.size());
  ASSERT_LT(s1d4, panel.rows.size());
  EXPECT_NEAR(t.raw[s0d0], 2.1, 1e-9);
  EXPECT_NEAR(t.roll[s0d0], 0.1, 1e-9);
  // S1 @ d=4 wanted the dropped session 25: UNDEFINED, never interpolated to
  // the neighbouring session, which would silently shorten the horizon.
  EXPECT_TRUE(std::isnan(t.raw[s1d4]));
  EXPECT_TRUE(std::isnan(t.roll[s1d4]));
  // S0 keeps 30 rows and 9 of them (d=0..8) reach d+21 <= 29, so 21 do not.
  // S1 keeps 29 rows (session 25 dropped) and 8 reach an exit -- d=4 is the one
  // that loses it -- so 21 do not. The dropped row is not a row and is not
  // counted anywhere: a gap costs the rows that POINT at it, nothing else.
  EXPECT_EQ(t.n_rows_with_exit, static_cast<std::size_t>(9 + 8));
  EXPECT_EQ(t.n_rows_no_exit, static_cast<std::size_t>(21 + 21));
}

// A v1 panel carries no ATM-forward leg, so every iv-change axis must be
// UNDEFINED rather than silently substituting the variance-swap strip strike.
TEST(VrpTrainVega, AV1PanelLeavesEveryIvChangeAxisUndefined) {
  MiniPanelSpec spec;
  spec.n_dates = 25;
  spec.n_syms = 2;
  spec.iv_atmf = [](std::size_t, std::size_t) {
    return std::numeric_limits<double>::quiet_NaN(); // what load_vrp_panel leaves on v1
  };
  const vrp::VrpPanel panel = make_mini_panel(spec);
  const vrp::VrpIvChgTargets t = vrp::vrp_build_iv_chg(panel, 21);
  EXPECT_GT(t.n_rows_with_exit, 0u); // the JOIN succeeded
  for (std::size_t i = 0; i < t.raw.size(); ++i) {
    EXPECT_TRUE(std::isnan(t.raw[i])) << i; // the VALUE did not
    EXPECT_TRUE(std::isnan(t.roll[i])) << i;
  }
}

// THE CONTAMINATION CHECK, as an executable contract.
//
// -iv_atmf_21d[t] is a PERFECT rank transform of the target's KNOWN leg -- the
// same +1.0000 algebraic relation -iv_fair_21d had to the old label's implied
// leg. That much is unavoidable for any difference target and is not by itself
// disqualifying. What WOULD disqualify it is the same +1.0000 against the
// TARGET, and that is a measurement, not an identity: it depends entirely on
// how the forward leg moves, so a fixture can drive it to +1, to -1, or to 0
// with the entry leg held fixed. This test pins all three.
TEST(VrpTrainVega, NegIvAtmfIsAnIdentityOnTheKnownLegButNotOnTheTarget) {
  const std::vector<double> iv_t{0.20, 0.30, 0.40, 0.50};
  const std::vector<double> neg_iv_t{-0.20, -0.30, -0.40, -0.50};
  // The known leg: E_t[iv_fwd] - iv_t. The date constant cannot change ranks.
  std::vector<double> known_leg;
  for (const double v : iv_t) {
    known_leg.push_back(0.35 - v);
  }
  EXPECT_NEAR(vrp::vrp_spearman(neg_iv_t, known_leg), 1.0, 1e-12);

  // Forward leg case A -- perfect mean reversion (everything to 0.35):
  // the target IS the known leg, so the identity carries through.
  std::vector<double> tgt_a;
  for (const double v : iv_t) {
    tgt_a.push_back(0.35 - v);
  }
  EXPECT_NEAR(vrp::vrp_spearman(neg_iv_t, tgt_a), 1.0, 1e-12);
  // Case B -- a cross-sectional random walk: iv_fwd = iv_t + an idiosyncratic
  // shift whose ranks (2,4,1,3) are orthogonal to the entry level's. The SAME
  // entry leg now scores EXACTLY ZERO. This is the case the whole check turns
  // on: if implied vol did not mean-revert cross-sectionally, -iv_atmf_21d
  // would carry no information about the target at all.
  const std::vector<double> tgt_b{-0.01, 0.03, -0.02, 0.01};
  EXPECT_NEAR(vrp::vrp_spearman(neg_iv_t, tgt_b), 0.0, 1e-12);
  // Case C -- momentum (high implied goes higher): the SAME entry leg now
  // scores -1. A quantity that can be -1 is not an algebraic identity.
  std::vector<double> tgt_c;
  for (const double v : iv_t) {
    tgt_c.push_back(0.5 * v);
  }
  EXPECT_NEAR(vrp::vrp_spearman(neg_iv_t, tgt_c), -1.0, 1e-12);
}

// THE ROLL LEG EARNS ITS PLACE: under a frozen (sticky-expiry) surface the raw
// axis IS the term slope by construction, and the roll adjustment removes it
// exactly. A gate on the raw axis would be grading a carry identity.
TEST(VrpTrainVega, RollAdjustmentRemovesAStickyExpiryTermStructureRoll) {
  // Sticky-expiry, linear-in-tenor: iv_atmf_21d(t+21) == iv_42d(t) ==
  // iv_21d(t) + (iv63(t)-iv21(t))/2 for every name, with a different slope per
  // name so the slope has cross-sectional dispersion to rank on.
  const std::size_t n_syms = 12;
  MiniPanelSpec spec;
  spec.n_dates = 43;
  spec.n_syms = n_syms;
  const auto slope = [](std::size_t s) {
    return -0.06 + 0.01 * static_cast<double>(s); // spans backwardation..contango
  };
  spec.iv_fair = [](std::size_t, std::size_t s) {
    return 0.25 + 0.005 * static_cast<double>(s);
  };
  spec.iv63 = [&slope](std::size_t, std::size_t s) {
    return 0.25 + 0.005 * static_cast<double>(s) + slope(s);
  };
  // The ATMF point rides the entry-date curve forward and nothing else moves.
  spec.iv_atmf = [&slope](std::size_t d, std::size_t s) {
    return 0.25 + 0.005 * static_cast<double>(s) +
           0.5 * slope(s) * static_cast<double>(d) / 21.0;
  };
  const vrp::VrpPanel panel = make_mini_panel(spec);
  const vrp::VrpIvChgTargets t = vrp::vrp_build_iv_chg(panel, 21);

  // The gate's statistic is CROSS-SECTIONAL (per date), so the fixture is read
  // one date at a time. Pooling would tie every date's term slope for a given
  // name against raw values that differ in their last bits, which is a
  // tie-handling artifact rather than a statement about the roll.
  std::vector<double> term_slope;
  std::vector<double> raw;
  std::vector<double> roll;
  const std::int64_t first_ts = panel.rows.front().entry_ts_ns;
  for (std::size_t i = 0; i < panel.rows.size(); ++i) {
    if (panel.rows[i].entry_ts_ns != first_ts || !std::isfinite(t.roll[i])) {
      continue;
    }
    term_slope.push_back(panel.rows[i].iv_fair_63d - panel.rows[i].iv_fair_21d);
    raw.push_back(t.raw[i]);
    roll.push_back(t.roll[i]);
  }
  ASSERT_EQ(raw.size(), n_syms);
  // On the RAW axis the free term slope is a PERFECT predictor -- +1.0000, by
  // carry accounting, on any dataset in which the surface does not move...
  EXPECT_NEAR(vrp::vrp_spearman(term_slope, raw), 1.0, 1e-12);
  // ...and on the roll-adjusted axis there is nothing left of it at all.
  for (const double v : roll) {
    EXPECT_NEAR(v, 0.0, 1e-9);
  }
}

// The vega book charges a round trip on BOTH legs, splits its P&L by vega
// sign, and quotes the pair per 1u GROSS vega (i.e. halved).
TEST(VrpTrainVega, VegaBookChargesARoundTripOnBothLegsAndSplitsByVegaSign) {
  // 10 names on one date so the decile buckets hold exactly one name each.
  std::vector<std::int64_t> ts(10, 1000);
  std::vector<double> score;
  std::vector<double> target;
  std::vector<double> iv(10, 0.40);
  for (std::size_t i = 0; i < 10; ++i) {
    score.push_back(static_cast<double>(i));
    target.push_back(static_cast<double>(i) - 4.5); // -4.5 .. +4.5 vol pts
  }
  std::vector<std::size_t> sym(10);
  for (std::size_t i = 0; i < 10; ++i) {
    sym[i] = i;
  }
  const vrp::VrpVegaLegs legs = vrp::vrp_vega_book_per_date(
      ts, score, target, iv, sym, vrp::kVrpDefaultCrossingFraction, true);
  ASSERT_EQ(legs.net.size(), 1u);
  // Round-trip cost at iv = 0.40: 2 * 0.03205 * 40 = 2.564 vol pts per leg.
  const double rt = 2.0 * vrp::vrp_vega_cost_one_way(0.40);
  EXPECT_NEAR(rt, 2.564, 1e-12);
  EXPECT_NEAR(legs.long_leg[0], 4.5 - rt, 1e-12);  // long the top name
  EXPECT_NEAR(legs.short_leg[0], 4.5 - rt, 1e-12); // short the bottom (-4.5)
  EXPECT_NEAR(legs.gross[0], 4.5, 1e-12);          // 0.5*(4.5 + 4.5)
  EXPECT_NEAR(legs.cost[0], rt, 1e-12);
  EXPECT_NEAR(legs.net[0], 4.5 - rt, 1e-12);
  // Costs off reproduces the gross figure exactly, so the charge is visible.
  const vrp::VrpVegaLegs free_legs = vrp::vrp_vega_book_per_date(
      ts, score, target, iv, sym, vrp::kVrpDefaultCrossingFraction, false);
  EXPECT_NEAR(free_legs.net[0], 4.5, 1e-12);
  EXPECT_NEAR(free_legs.cost[0], 0.0, 1e-12);
}

// A vega book is NOT structurally short vol, so its zero-selection bar is the
// better of long-everything and short-everything -- picked ONCE from the whole
// sample and then applied to every date, never per-date hindsight.
TEST(VrpTrainVega, VegaFloorIsTheBetterOfLongAndShortEverythingChosenOnce) {
  std::vector<std::int64_t> ts;
  std::vector<double> target;
  std::vector<double> iv;
  // Two dates: implied rises 10 vol pts on the first, falls 4 on the second.
  for (const double m : {10.0, -4.0}) {
    for (std::size_t i = 0; i < 4; ++i) {
      ts.push_back(m > 0.0 ? 1 : 2);
      target.push_back(m);
      iv.push_back(0.40);
    }
  }
  const vrp::VrpVegaFloor f =
      vrp::vrp_vega_floor(ts, target, iv, vrp::kVrpDefaultCrossingFraction, true);
  const double rt = 2.0 * vrp::vrp_vega_cost_one_way(0.40);
  ASSERT_EQ(f.per_date_long.size(), 2u);
  EXPECT_NEAR(f.per_date_long[0], 10.0 - rt, 1e-12);
  EXPECT_NEAR(f.per_date_long[1], -4.0 - rt, 1e-12);
  EXPECT_NEAR(f.per_date_short[0], -10.0 - rt, 1e-12);
  EXPECT_NEAR(f.per_date_short[1], 4.0 - rt, 1e-12);
  // Long everything means +3 - rt; short everything means -3 - rt. Long wins,
  // and the SAME choice is applied to date 1 where long lost money.
  EXPECT_TRUE(f.best_is_long);
  EXPECT_NEAR(f.long_all.mean, 3.0 - rt, 1e-12);
  EXPECT_NEAR(f.short_all.mean, -3.0 - rt, 1e-12);
  EXPECT_NEAR(f.per_date_best[1], -4.0 - rt, 1e-12);
}

// THE ASYMMETRIC OBJECTIVE. A positive short-vega edge is discounted; a
// short-vega LOSS is not -- otherwise the haircut would flatter a losing short
// book by shrinking its loss.
TEST(VrpTrainVega, ShortVegaHaircutDiscountsOnlyAPositiveShortLegEdge) {
  vrp::VrpVegaLegs legs;
  legs.gross = {6.0, 6.0};
  legs.cost = {0.0, 0.0};
  legs.net = {6.0, 6.0};
  legs.long_leg = {2.0, 2.0};
  legs.short_leg = {10.0, 10.0};
  vrp::VrpVegaFloor floor;
  floor.per_date_long = {0.0, 0.0};
  floor.per_date_short = {0.0, 0.0};
  floor.per_date_best = {0.0, 0.0};
  const vrp::VrpVegaAgg a = vrp::vrp_vega_agg(legs, floor, 0.5);
  EXPECT_NEAR(a.short_multiplier, 0.5, 1e-12);
  EXPECT_NEAR(a.net.mean, 6.0, 1e-12);
  EXPECT_NEAR(a.objective.mean, 0.5 * (2.0 + 0.5 * 10.0), 1e-12); // 3.5, not 6
  EXPECT_NEAR(a.long_leg.mean, 2.0, 1e-12);
  EXPECT_NEAR(a.short_leg.mean, 10.0, 1e-12);
  EXPECT_NEAR(a.excess, 6.0, 1e-12);
  EXPECT_NEAR(a.long_excess, 2.0, 1e-12);
  EXPECT_NEAR(a.short_excess, 10.0, 1e-12);

  // Flip the short leg negative: the haircut must NOT apply.
  legs.short_leg = {-10.0, -10.0};
  legs.net = {-4.0, -4.0};
  const vrp::VrpVegaAgg b = vrp::vrp_vega_agg(legs, floor, 0.5);
  EXPECT_NEAR(b.short_multiplier, 1.0, 1e-12);
  EXPECT_NEAR(b.objective.mean, 0.5 * (2.0 - 10.0), 1e-12); // -4, undiscounted
  // Haircut 0 is the undiscounted book on either sign.
  const vrp::VrpVegaAgg c = vrp::vrp_vega_agg(legs, floor, 0.0);
  EXPECT_NEAR(c.short_multiplier, 1.0, 1e-12);
  EXPECT_NEAR(c.objective.mean, -4.0, 1e-12);
}

// ── ROUND 6: the cost model ─────────────────────────────────────────────────

// The crossing fraction is REACHABLE and multiplicative, and the DEFAULT
// returns the measured Christoffersen effective charge bit-exactly -- so
// making the constant reachable is not a silent re-pricing of round 5.
TEST(VrpTrainCost, CrossingFractionIsReachableAndTheDefaultIsTheMeasuredEffectiveSpread) {
  // 6.41% of premium ATM effective, halved one-way, x 100 x iv = vol points.
  const double one_way_at_40 = vrp::vrp_vega_cost_one_way(0.40);
  EXPECT_NEAR(one_way_at_40, 1.282, 1e-12); // 0.03205 * 40
  // BIT-EXACT, not merely close: the default must not move a single ulp of any
  // round-5 figure, or "unchanged at default" would be a guess.
  EXPECT_EQ(vrp::vrp_vega_cost_one_way(0.40, vrp::kVrpDefaultCrossingFraction), one_way_at_40);
  EXPECT_EQ(vrp::kVrpDefaultCrossingFraction, 0.55);
  // The quoted width is DERIVED, so re-crossing it at 1.00 charges the full
  // quoted spread: 0.03205 / 0.55 = 5.827% of premium one-way.
  EXPECT_NEAR(vrp::vrp_vega_cost_one_way(0.40, 1.0),
              vrp::kVrpVegaQuotedOneWayFracOfPremium * 100.0 * 0.40, 1e-12);
  EXPECT_NEAR(vrp::kVrpVegaQuotedOneWayFracOfPremium, 0.0582727272727, 1e-12);
  // Strictly multiplicative in the crossing fraction, so a sensitivity grid is
  // a scaling of one measured number rather than five unrelated calibrations.
  EXPECT_NEAR(vrp::vrp_vega_cost_one_way(0.40, 0.275), 0.5 * one_way_at_40, 1e-12);
  // THE TRAP THIS TEST EXISTS TO PIN: the ORATS complex-order 0.53 applied on
  // top of the already-effective 3.205% would cut the charge by 47%. Under the
  // corrected parameterisation 0.53 lands within 4% of the measured effective
  // charge instead, because it is crossed against the QUOTED width.
  EXPECT_NEAR(vrp::vrp_vega_cost_one_way(0.40, 0.53) / one_way_at_40, 0.53 / 0.55, 1e-12);
  EXPECT_GT(vrp::vrp_vega_cost_one_way(0.40, 0.53), 0.9 * one_way_at_40);
  // Cost scales with the name's own IV (research digest Q2.1), so a high-vol
  // name is NOT cheaper in vol terms.
  EXPECT_NEAR(vrp::vrp_vega_cost_one_way(0.80), 2.0 * one_way_at_40, 1e-12);
  // NaN-hostile on both arguments: an unpriceable leg is not a free one, and a
  // nonsense knob never trades free.
  EXPECT_TRUE(
      std::isnan(vrp::vrp_vega_cost_one_way(std::numeric_limits<double>::quiet_NaN(), 0.55)));
  EXPECT_TRUE(std::isnan(vrp::vrp_vega_cost_one_way(0.40, -0.1)));
  EXPECT_TRUE(
      std::isnan(vrp::vrp_vega_cost_one_way(0.40, std::numeric_limits<double>::quiet_NaN())));
}

// ── ROUND 7: the LIQUIDITY-VARYING cost model ──────────────────────────────

// THE REGRESSION ANCHOR. Off is off: the multiplier is EXACTLY 1.0 for every
// input a caller can present -- including the ones that are NaN-hostile when
// the model is on -- so a disabled run cannot move one ulp of any round-5/6
// figure. This is the test that keeps the flat-cost artifacts reproducible.
TEST(VrpTrainCost, LiquidityMultiplierIsExactlyOneWhenDisabled) {
  const vrp::VrpLiquidityCost off{}; // default-constructed == disabled
  EXPECT_FALSE(off.enabled);
  const double nan = std::numeric_limits<double>::quiet_NaN();
  for (const double h : {0.0, -1.0, 1e-9, 0.001, 0.0509, 0.5, 1e9, nan,
                         std::numeric_limits<double>::infinity()}) {
    EXPECT_EQ(vrp::vrp_liquidity_cost_multiplier(h, off), 1.0)
        << "disabled multiplier moved at half-spread " << h;
  }
  // And the charge itself is BIT-IDENTICAL to the flat one, not merely near.
  for (const double iv : {0.10, 0.25, 0.40, 0.83, 1.75}) {
    for (const double h : {0.001, 0.0509, 0.42, nan}) {
      EXPECT_EQ(vrp::vrp_vega_cost_one_way(iv, vrp::kVrpDefaultCrossingFraction, h, off),
                vrp::vrp_vega_cost_one_way(iv, vrp::kVrpDefaultCrossingFraction))
          << "disabled path re-priced iv=" << iv << " h=" << h;
    }
  }
}

// Monotone in LIQUIDITY: a wider quoted market is never cheaper. Stated on the
// half-spread axis (which is the INVERSE of liquidity), the multiplier is
// non-decreasing in the width and therefore non-increasing in liquidity.
TEST(VrpTrainCost, LiquidityMultiplierIsMonotoneNonIncreasingInLiquidity) {
  vrp::VrpLiquidityCost m;
  m.enabled = true;
  double prev = -1.0;
  // Bounded sweep across four orders of magnitude of quoted width.
  for (int i = 0; i <= 400; ++i) {
    const double h = 1e-4 * std::pow(10.0, 3.0 * static_cast<double>(i) / 400.0);
    const double mult = vrp::vrp_liquidity_cost_multiplier(h, m);
    ASSERT_TRUE(std::isfinite(mult)) << "non-finite multiplier at h=" << h;
    EXPECT_GE(mult, prev) << "multiplier fell as the quoted width widened at h=" << h;
    prev = mult;
  }
  // Strictly increasing inside the unclamped band, so the model actually
  // discriminates rather than merely failing to invert.
  const double a = vrp::vrp_liquidity_cost_multiplier(1.2 * m.ref_half_spread_frac, m);
  const double b = vrp::vrp_liquidity_cost_multiplier(2.4 * m.ref_half_spread_frac, m);
  EXPECT_GT(b, a);
  EXPECT_NEAR(b / a, 2.0, 1e-12);
}

// The anchor is a UNITS IDENTITY, not a fitted curve: a name quoting exactly
// the reference width pays exactly the flat charge, and one quoting k times
// that width pays k times it, until the cap.
TEST(VrpTrainCost, LiquidityMultiplierAnchorsAtOneOnTheReferenceWidthAndClamps) {
  vrp::VrpLiquidityCost m;
  m.enabled = true;
  EXPECT_EQ(m.ref_half_spread_frac, vrp::kVrpLiquidityReferenceHalfSpreadFrac);
  EXPECT_NEAR(vrp::vrp_liquidity_cost_multiplier(m.ref_half_spread_frac, m), 1.0, 1e-12);
  EXPECT_NEAR(vrp::vrp_liquidity_cost_multiplier(2.0 * m.ref_half_spread_frac, m), 2.0, 1e-12);
  // FLOOR: the default deliberately refuses to claim a SAVING on names tighter
  // than the reference. The flat charge's LEVEL is a 1996-2015 EFFECTIVE spread
  // and this field is a 2025-26 QUOTED one -- the cross-sectional ratio is
  // comparable, the level is not -- so the default prices illiquidity only.
  EXPECT_EQ(m.floor_multiplier, 1.0);
  EXPECT_EQ(vrp::vrp_liquidity_cost_multiplier(0.01 * m.ref_half_spread_frac, m), 1.0);
  // CAP: past it the calibration is unsupported by the measurement, and the
  // honest response is to screen the name out, not to extrapolate a charge.
  EXPECT_EQ(m.cap_multiplier, vrp::kVrpLiquidityDefaultCapMultiplier);
  EXPECT_EQ(vrp::vrp_liquidity_cost_multiplier(50.0 * m.ref_half_spread_frac, m),
            m.cap_multiplier);
  // Releasing the floor gives the symmetric two-sided reading on request.
  m.floor_multiplier = 0.0;
  EXPECT_NEAR(vrp::vrp_liquidity_cost_multiplier(0.5 * m.ref_half_spread_frac, m), 0.5, 1e-12);
}

// NaN-hostile when ON, exactly like the crossing fraction: a leg whose market
// was never measured is not a leg that trades at the liquid-universe charge.
TEST(VrpTrainCost, LiquidityCostIsNaNHostileWhenEnabled) {
  vrp::VrpLiquidityCost m;
  m.enabled = true;
  const double nan = std::numeric_limits<double>::quiet_NaN();
  EXPECT_TRUE(std::isnan(vrp::vrp_liquidity_cost_multiplier(nan, m)));
  EXPECT_TRUE(std::isnan(vrp::vrp_liquidity_cost_multiplier(0.0, m)));
  EXPECT_TRUE(std::isnan(vrp::vrp_liquidity_cost_multiplier(-0.02, m)));
  EXPECT_TRUE(std::isnan(
      vrp::vrp_liquidity_cost_multiplier(std::numeric_limits<double>::infinity(), m)));
  // A nonsense knob never trades free either.
  vrp::VrpLiquidityCost bad = m;
  bad.ref_half_spread_frac = 0.0;
  EXPECT_TRUE(std::isnan(vrp::vrp_liquidity_cost_multiplier(0.05, bad)));
  bad = m;
  bad.cap_multiplier = -1.0;
  EXPECT_TRUE(std::isnan(vrp::vrp_liquidity_cost_multiplier(0.05, bad)));
  bad = m;
  bad.floor_multiplier = 5.0; // floor ABOVE the 4.0 cap is incoherent
  EXPECT_TRUE(std::isnan(vrp::vrp_liquidity_cost_multiplier(0.05, bad)));
  // ...but a floor merely BELOW the cap is a legitimate configuration.
  vrp::VrpLiquidityCost banded = m;
  banded.floor_multiplier = 3.0;
  EXPECT_EQ(vrp::vrp_liquidity_cost_multiplier(0.05, banded), 3.0);
  // The charge inherits it: NaN liquidity propagates to a NaN cost.
  EXPECT_TRUE(std::isnan(
      vrp::vrp_vega_cost_one_way(0.40, vrp::kVrpDefaultCrossingFraction, nan, m)));
}

// The two factors compose multiplicatively and independently: crossing scales
// the width, liquidity scales the name. Neither silently absorbs the other.
TEST(VrpTrainCost, LiquidityAndCrossingComposeMultiplicatively) {
  vrp::VrpLiquidityCost m;
  m.enabled = true;
  const double h = 2.0 * m.ref_half_spread_frac;
  const double flat = vrp::vrp_vega_cost_one_way(0.40, vrp::kVrpDefaultCrossingFraction);
  EXPECT_NEAR(vrp::vrp_vega_cost_one_way(0.40, vrp::kVrpDefaultCrossingFraction, h, m),
              2.0 * flat, 1e-12);
  // Halving the crossing fraction halves the liquidity-adjusted charge too.
  EXPECT_NEAR(vrp::vrp_vega_cost_one_way(0.40, 0.275, h, m), flat, 1e-12);
  // And it still scales with the name's own IV.
  EXPECT_NEAR(vrp::vrp_vega_cost_one_way(0.80, vrp::kVrpDefaultCrossingFraction, h, m),
              4.0 * flat, 1e-12);
}

// A book that cannot be held is not a strategy: turnover, names/day and the
// drawdown path are hand-computed against a fixture whose membership is known.
TEST(VrpTrainVega, VegaBookReportsTurnoverNamesPerDayAndMaxDrawdown) {
  // Two dates, 10 names each. Date 1 ranks symbols 0..9 ascending; date 2
  // REVERSES the score, so the long and short deciles swap symbol entirely.
  std::vector<std::int64_t> ts;
  std::vector<double> score;
  std::vector<double> target;
  std::vector<double> iv;
  std::vector<std::size_t> sym;
  for (std::size_t d = 0; d < 2; ++d) {
    for (std::size_t i = 0; i < 10; ++i) {
      ts.push_back(static_cast<std::int64_t>(d) + 1);
      sym.push_back(i);
      iv.push_back(0.40);
      score.push_back(d == 0 ? static_cast<double>(i) : -static_cast<double>(i));
      // Flat within each date, so gross is 0 and the cost charge is visible.
      target.push_back(0.0);
    }
  }
  const vrp::VrpVegaLegs legs = vrp::vrp_vega_book_per_date(
      ts, score, target, iv, sym, vrp::kVrpDefaultCrossingFraction, true);
  ASSERT_EQ(legs.net.size(), 2u);
  // One name per decile bucket on each side: 2 names a day.
  EXPECT_NEAR(legs.names[0], 2.0, 1e-12);
  EXPECT_NEAR(legs.names[1], 2.0, 1e-12);
  // Turnover is UNDEFINED on the first formation date -- there is no previous
  // book, and calling that "zero turnover" would understate every cost.
  EXPECT_TRUE(std::isnan(legs.turnover[0]));
  // Date 1 holds {+sym9, -sym0}; date 2 holds {+sym0, -sym9}. All of it moved.
  EXPECT_NEAR(legs.turnover[1], 1.0, 1e-12);

  vrp::VrpVegaFloor floor;
  floor.per_date_long = {0.0, 0.0};
  floor.per_date_short = {0.0, 0.0};
  floor.per_date_best = {0.0, 0.0};
  const vrp::VrpVegaAgg a = vrp::vrp_vega_agg(legs, floor, 0.0);
  EXPECT_NEAR(a.names_per_day.mean, 2.0, 1e-12);
  // The mean turnover is over the ONE date that has a predecessor.
  EXPECT_EQ(a.turnover.n, 1u);
  EXPECT_NEAR(a.turnover.mean, 1.0, 1e-12);
  const double rt = 2.0 * vrp::vrp_vega_cost_one_way(0.40);
  EXPECT_NEAR(legs.net[0], -rt, 1e-12);
  EXPECT_NEAR(legs.net[1], -rt, 1e-12);
  // Cumulative curve: -rt then -2rt. Peak is 0 at the start, so maxDD = 2rt.
  EXPECT_NEAR(a.max_drawdown, 2.0 * rt, 1e-12);
  // Each LEG carries its own drawdown against its own zero-selection
  // alternative: a long-only reading of this book is a different instrument and
  // must not borrow the pair's risk statistics.
  EXPECT_NEAR(a.long_max_drawdown, 2.0 * rt, 1e-12);
  EXPECT_NEAR(a.short_max_drawdown, 2.0 * rt, 1e-12);
  EXPECT_NEAR(a.floor_long_max_drawdown, 0.0, 1e-12);
  // A monotonically rising curve has NO drawdown, and an all-NaN one has no
  // MEASURED drawdown rather than a comforting zero.
  const std::array<double, 3> up{1.0, 2.0, 3.0};
  EXPECT_NEAR(vrp::detail::max_drawdown(std::span<const double>{up}), 0.0, 1e-12);
  const std::array<double, 2> none{std::numeric_limits<double>::quiet_NaN(),
                                   std::numeric_limits<double>::quiet_NaN()};
  EXPECT_TRUE(std::isnan(vrp::detail::max_drawdown(std::span<const double>{none})));
}

// Turnover is computed on SIGNED weights, so a name that stays in the book but
// FLIPS SIDE is full turnover on that name, not zero.
TEST(VrpTrainVega, TurnoverCountsASideFlipAsAFullReplacementOfThatName) {
  const std::map<std::size_t, double> a{{0, 0.5}, {1, -0.5}};
  const std::map<std::size_t, double> b{{0, -0.5}, {1, 0.5}};
  EXPECT_NEAR(vrp::detail::weight_turnover(a, a), 0.0, 1e-12);
  EXPECT_NEAR(vrp::detail::weight_turnover(a, b), 1.0, 1e-12);
  // Half the long leg rotates to a new name: a quarter of gross moves.
  const std::map<std::size_t, double> c{{0, 0.25}, {1, -0.5}, {2, 0.25}};
  EXPECT_NEAR(vrp::detail::weight_turnover(a, c), 0.25, 1e-12);
}

// ── ROUND 6: the EIV guards on the columns actually gated ───────────────────

// The free Vasquez term-slope rule must MOVE under --feature-lag. Round 5 read
// it off the target-side row, so its "lag 2" figure still saw session t, and a
// free rule cannot be gated on a lag it never took.
TEST(VrpTrainVega, TermSlopeBenchmarkIsReadFromTheLaggedFeatureNotTheTargetSideRow) {
  vrp::VrpPanel panel;
  panel.symbols = {"AAA"};
  for (std::size_t i = 0; i < 4; ++i) {
    vrp::VrpPanelRow r;
    r.symbol = "AAA";
    r.entry_ts_ns = static_cast<std::int64_t>(i) + 1;
    r.iv_fair_21d = 0.20;
    r.iv_fair_63d = 0.20 + 0.01 * static_cast<double>(i);
    r.f.fill(0.0);
    r.f[vrp::kVrpFeatTermSlope] = r.iv_fair_63d - r.iv_fair_21d;
    panel.rows.push_back(r);
    panel.row_symbol.push_back(0);
  }
  // The feature slot IS the target-side difference, so at lag 0 the two reads
  // agree exactly -- which is why this fix leaves every round-5 number alone.
  for (const vrp::VrpPanelRow &r : panel.rows) {
    EXPECT_NEAR(r.f[vrp::kVrpFeatTermSlope], r.iv_fair_63d - r.iv_fair_21d, 1e-15);
  }
  const std::size_t unavailable = vrp::apply_vrp_feature_lag(panel, 2);
  EXPECT_EQ(unavailable, 2u);
  // After lagging, the FEATURE has moved back two sessions while the
  // target-side columns have not. Row 3 now carries row 1's slope.
  EXPECT_NEAR(panel.rows[3].f[vrp::kVrpFeatTermSlope], 0.01, 1e-15);
  EXPECT_NEAR(panel.rows[3].iv_fair_63d - panel.rows[3].iv_fair_21d, 0.03, 1e-15);
  EXPECT_TRUE(std::isnan(panel.rows[0].f[vrp::kVrpFeatTermSlope]));
}

// The EIV target rebuild: the ENTRY leg moves back, the EXIT leg does not, and
// a row without a lagged entry mark is UNDEFINED rather than silently unlagged.
TEST(VrpTrainVega, EivTargetEntryLagMovesOnlyTheEntryLegAndNeverSubstitutes) {
  vrp::VrpPanel panel;
  panel.symbols = {"AAA"};
  // 6 sessions, iv_atmf rising 1 vol point per session, flat 63d/21d so the
  // roll leg is identically zero and the raw axis equals the roll axis.
  for (std::size_t i = 0; i < 6; ++i) {
    vrp::VrpPanelRow r;
    r.symbol = "AAA";
    r.entry_ts_ns = static_cast<std::int64_t>(i) + 1;
    r.iv_fair_21d = 0.20;
    r.iv_fair_63d = 0.20;
    r.iv_atmf_21d = 0.20 + 0.01 * static_cast<double>(i);
    r.f.fill(0.0);
    panel.rows.push_back(r);
    panel.row_symbol.push_back(0);
  }
  // Horizon 3 sessions, no entry lag: row 0 marks 0.23 against 0.20 => +3.
  const vrp::VrpIvChgTargets t0 = vrp::vrp_build_iv_chg(panel, 3, 0);
  EXPECT_NEAR(t0.raw[0], 3.0, 1e-9);
  EXPECT_NEAR(t0.roll[0], 3.0, 1e-9);
  EXPECT_NEAR(t0.raw[2], 3.0, 1e-9);
  // Entry lag 2: row 2's entry mark comes from row 0 (0.20) while its exit is
  // still row 5 (0.25), so the target lengthens to +5 -- reported as the
  // diagnostic it is, never as a tradeable hold.
  const vrp::VrpIvChgTargets t2 = vrp::vrp_build_iv_chg(panel, 3, 2);
  EXPECT_NEAR(t2.raw[2], 5.0, 1e-9);
  // Rows 0 and 1 have no 2nd predecessor: UNDEFINED, not the unlagged value.
  EXPECT_TRUE(std::isnan(t2.raw[0]));
  EXPECT_TRUE(std::isnan(t2.raw[1]));
  EXPECT_TRUE(std::isnan(t2.roll[0]));
  // The exit leg is untouched: the same rows still have no exit at all.
  EXPECT_EQ(t2.n_rows_no_exit, t0.n_rows_no_exit);
}

namespace {

[[nodiscard]] vrp::VrpScoreReport make_iv_score(std::string name, vrp::VrpScoreKind kind,
                                                double pearson, double spearman) {
  vrp::VrpScoreReport s;
  s.name = std::move(name);
  s.kind = kind;
  s.target = vrp::VrpTargetAxis::IvChgRoll;
  s.ic_pearson = pearson;
  s.ic_spearman = spearman;
  return s;
}

[[nodiscard]] vrp::VrpVegaReport make_iv_vega(std::string name, double iv_neutral_excess) {
  vrp::VrpVegaReport v;
  v.name = std::move(name);
  v.iv_neutral.excess = iv_neutral_excess;
  return v;
}

} // namespace

// The vega gate is exactly as strict as the variance gate, is gated on the
// ROLL-ADJUSTED axis only, and fails closed on everything the variance gate
// fails closed on.
TEST(VrpTrainVega, VegaGateIsStrictOnTheRollAxisAndFailsClosed) {
  vrp::VrpVegaFloor floor;
  floor.long_all.mean = 2.1;
  floor.best_is_long = true;

  // Model beats the one benchmark on both ICs and on money, and its own excess
  // is positive: PASS.
  std::vector<vrp::VrpScoreReport> s{
      make_iv_score("gbt", vrp::VrpScoreKind::Model, 0.20, 0.20),
      make_iv_score("bench_term_slope", vrp::VrpScoreKind::Benchmark, 0.10, 0.10)};
  std::vector<vrp::VrpVegaReport> v{make_iv_vega("gbt", 1.5),
                                    make_iv_vega("bench_term_slope", 0.5)};
  EXPECT_TRUE(vrp::vrp_vega_gate_verdict(s, v, floor).pass);

  // Losing on money alone is a FAIL, however far ahead the ICs are.
  v[0] = make_iv_vega("gbt", 0.4);
  EXPECT_FALSE(vrp::vrp_vega_gate_verdict(s, v, floor).pass);

  // A positive relative win with a NEGATIVE own excess is a FAIL: earning less
  // than the zero-selection alternative is not selection.
  v[0] = make_iv_vega("gbt", -0.1);
  v[1] = make_iv_vega("bench_term_slope", -0.9);
  EXPECT_FALSE(vrp::vrp_vega_gate_verdict(s, v, floor).pass);

  // NaN anywhere is a FAIL.
  v[0] = make_iv_vega("gbt", kNaN);
  v[1] = make_iv_vega("bench_term_slope", 0.5);
  EXPECT_FALSE(vrp::vrp_vega_gate_verdict(s, v, floor).pass);
  v[0] = make_iv_vega("gbt", 1.5);
  s[0].ic_spearman = kNaN;
  EXPECT_FALSE(vrp::vrp_vega_gate_verdict(s, v, floor).pass);

  // No benchmark at all is a FAIL: an ungraded run never reads as a passing one.
  s = {make_iv_score("gbt", vrp::VrpScoreKind::Model, 0.20, 0.20)};
  EXPECT_FALSE(vrp::vrp_vega_gate_verdict(s, v, floor).pass);

  // A CONTAMINATED column cannot decide the verdict however well it scores --
  // iv_fair_63d - iv_atmf_21d carries the target's own entry mark.
  s = {make_iv_score("gbt", vrp::VrpScoreKind::Model, 0.05, 0.05),
       make_iv_score("bench_term_slope", vrp::VrpScoreKind::Benchmark, 0.01, 0.01),
       make_iv_score(std::string{vrp::kVrpScoreContamIvSlope},
                     vrp::VrpScoreKind::Contaminated, 0.90, 0.90)};
  v = {make_iv_vega("gbt", 1.5), make_iv_vega("bench_term_slope", 0.5),
       make_iv_vega(std::string{vrp::kVrpScoreContamIvSlope}, 9.9)};
  const vrp::VrpVegaGateVerdict got = vrp::vrp_vega_gate_verdict(s, v, floor);
  EXPECT_TRUE(got.pass);
  EXPECT_EQ(got.n_benchmarks, 1u);
  EXPECT_EQ(got.best_benchmark, "bench_term_slope");

  // The RV-axis model entry is invisible here: only IvChgRoll gates the vega.
  std::vector<vrp::VrpScoreReport> wrong_axis{
      make_iv_score("gbt", vrp::VrpScoreKind::Model, 0.20, 0.20),
      make_iv_score("bench_term_slope", vrp::VrpScoreKind::Benchmark, 0.10, 0.10)};
  wrong_axis[0].target = vrp::VrpTargetAxis::IvChgRaw;
  EXPECT_FALSE(vrp::vrp_vega_gate_verdict(wrong_axis, v, floor).pass);
}

} // namespace
} // namespace atx::vol
