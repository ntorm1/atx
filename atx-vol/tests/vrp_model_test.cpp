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
#include "atx/vol/api/core/types.hpp"                   // Result, Status, ErrorCode

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

// Deterministic 3-symbol panel with a PLANTED linear relation in log space:
//   ln(rv_fwd^2) = -0.35 + 0.30 f0 + 0.35 f1 + 0.30 f2 + tiny wiggle
// so the log-HAR baseline on {f0, f1, f2} must beat a train-mean variance
// forecast on QLIKE. Tail rows carry NaN label + NaN rv_fwd (frozen contract);
// a few f4 cells are NaN (iv_fair_63d OutOfRange propagation).
[[nodiscard]] std::string make_synth_panel_tsv() {
  std::string out;
  out += "# schema=vrp_panel_v1\n";
  out += "# horizon_days=21\n";
  out += "symbol\tdate\tentry_ts_ns\tspot\tiv_fair_21d\tiv_fair_63d\trv_fwd_21d\tlabel\t"
         "f0_log_rv1\tf1_log_rv5\tf2_log_rv21\tf3_iv_level\tf4_term_slope\tf5_hv_iv_gap\t"
         "f6_vrp_lag\tf7_ret_21d\tf8_jump_recent\tf9_vov_63d\n";
  for (std::size_t d = 0; d < kSynthDates; ++d) {
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
      const bool tail = d + kSynthTail >= kSynthDates;
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
  // The signal file is deterministic too.
  EXPECT_EQ(read_file_bytes(report_->signal_path), read_file_bytes(report2->signal_path));
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

} // namespace
} // namespace atx::vol
