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
  EXPECT_NE(bytes.find("# n_symbols_fully_rejected=0\n"), std::string::npos);
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
      const double sd = fs.label_sd[s];
      const double edge = sd == 0.0 ? 0.0 : (y[0] - fs.label_mean[s]) / sd;
      EXPECT_NEAR(edge, srow.pred_edge_norm, 1e-9);
      found = true;
    }
  }
  EXPECT_TRUE(found);

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
  const double f_inproc =
      vrp::detail::baseline_forecast_var(report_->panel, stz, refit, row);
  EXPECT_NEAR(f_file, f_inproc, 1e-9 * std::max(1.0, f_inproc));
}

} // namespace
} // namespace atx::vol
