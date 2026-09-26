#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <vector>

#include <gtest/gtest.h>
#include "atx/engine/data/history_panel.hpp"
#include "atx/engine/data/panel_store.hpp"

namespace {
using namespace atx;
using namespace atx::engine::data;
namespace fs = std::filesystem;
constexpr i64 kDay = 86'400'000'000'000LL;
constexpr i64 kStart = 1'325'462'400'000'000'000LL; // synthetic 2012-01-02
constexpr f64 kNaN = std::numeric_limits<f64>::quiet_NaN();

class DataPanelStoreV2 : public ::testing::Test {
protected:
  fs::path root;
  void SetUp() override {
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    for (int i = 0; i < 32; ++i) {
      auto candidate = fs::temp_directory_path() / ("atx-panel-store-" + std::to_string(stamp) + "-" + std::to_string(i));
      std::error_code ec;
      if (fs::create_directory(candidate, ec)) { root = std::move(candidate); return; }
    }
    FAIL() << "cannot reserve exclusive synthetic test parent";
  }
  void TearDown() override { if (!root.empty()) { std::error_code ec; fs::remove_all(root, ec); } }
  PanelStoreConfig config() {
    PanelStoreConfig c;
    c.session_keys = {kStart, kStart + kDay, kStart + 2 * kDay, kStart + 3 * kDay};
    c.instrument_ids = {17, 100}; c.original_indices = {1, 0};
    c.fields = {{"close", LevelBasis::AdjustedLevel, PanelStorePrecision::Float32V2},
                {"returns", LevelBasis::Ratio, PanelStorePrecision::ExactFloat64V2}};
    c.instrument_namespace = "synthetic.security-id";
    c.recipe = "synthetic-only;fixed-complete-union;source-publication-unverified";
    c.membership_sha256 = std::string(64, 'a'); c.parents = {{"source", std::string(64, 'b')}};
    c.chunk_dates = 2; return c;
  }
  core::Status append(PanelStoreWriter& w, usize t, f64 future_delta = 0) {
    const std::array<f64, 2> close{100.00000002 + static_cast<f64>(t) * .00000001 + (t == 3 ? future_delta : 0), 20.25 + t};
    const std::array<f64, 2> returns{t ? .10000000000000003 : kNaN, t ? -0.0 : kNaN};
    const std::array<std::span<const f64>, 2> fields{close, returns};
    const std::array<u8, 2> present{1, static_cast<u8>(t != 0)};
    const std::array<u8, 2> tradable{1, static_cast<u8>(t >= 2)};
    return w.append_date(t, fields, close, present, tradable, kStart - kDay);
  }
  core::Result<std::string> write(const std::string& name, f64 future_delta = 0) {
    ATX_TRY(auto w, PanelStoreWriter::create((root / name).string(), config()));
    for (usize t = 0; t < 4; ++t) ATX_TRY_VOID(append(w, t, future_delta));
    return w.finish();
  }
};

TEST_F(DataPanelStoreV2, SeparateMasksFixedUnionAndOriginalF64Returns) {
  const auto receipt = write("store"); ASSERT_TRUE(receipt) << receipt.error().message();
  auto s = PanelStore::open((root / "store").string(), *receipt); ASSERT_TRUE(s);
  EXPECT_EQ(s->config().instrument_ids, (std::vector<i64>{17, 100}));
  EXPECT_EQ(s->config().original_indices, (std::vector<u64>{1, 0}));
  auto chunk = s->open_chunk(0); ASSERT_TRUE(chunk);
  std::array<f64, 2> row{};
  ASSERT_TRUE(chunk->read_field_row(0, 1, row)); EXPECT_EQ(row[0], 100.0);
  EXPECT_EQ(row[1], 21.25); // warm-up data is retained while not tradable
  EXPECT_EQ((*chunk->present(1))[1], 1); EXPECT_EQ((*chunk->tradable(1))[1], 0);
  ASSERT_TRUE(chunk->read_field_row(0, 0, row)); EXPECT_TRUE(std::isnan(row[1]));
  ASSERT_TRUE(chunk->read_field_row(1, 1, row));
  EXPECT_EQ(std::bit_cast<u64>(row[0]), std::bit_cast<u64>(.10000000000000003));
  EXPECT_EQ(std::bit_cast<u64>(row[1]), std::bit_cast<u64>(-0.0));
  ASSERT_TRUE(s->forward_returns(0, 3, 4, row));
  const f64 expected = (100.00000002 + 3.0 * .00000001) / 100.00000002 - 1.0;
  EXPECT_EQ(std::bit_cast<u64>(row[0]), std::bit_cast<u64>(expected));
  EXPECT_NE(row[0], 0.0); // f32 prices would have erased this finite return
  EXPECT_TRUE(std::isnan(row[1])); EXPECT_FALSE(s->forward_returns(0, 3, 3, row));
}

TEST_F(DataPanelStoreV2, PublicationLastAndExclusiveNoReplace) {
  auto w = PanelStoreWriter::create((root / "store").string(), config()); ASSERT_TRUE(w);
  EXPECT_FALSE(PanelStoreWriter::create((root / "store").string(), config()));
  ASSERT_TRUE(append(*w, 0)); ASSERT_TRUE(append(*w, 1));
  EXPECT_FALSE(w->finish()); EXPECT_FALSE(PanelStore::open((root / "store").string()));
  ASSERT_TRUE(append(*w, 2)); ASSERT_TRUE(append(*w, 3)); ASSERT_TRUE(w->finish());
  EXPECT_FALSE(w->finish()); EXPECT_FALSE(append(*w, 3));
  EXPECT_TRUE(fs::exists(root / "store" / "manifest.bin"));
}

TEST_F(DataPanelStoreV2, CapturedMappingIntegrityAndExtentValidation) {
  ASSERT_TRUE(write("store"));
  auto s = PanelStore::open((root / "store").string()); ASSERT_TRUE(s);
  { std::fstream f(root / "store" / "chunk-0.bin", std::ios::binary | std::ios::in | std::ios::out);
    ASSERT_TRUE(f); f.seekp(40); const char changed = 'X'; f.write(&changed, 1); }
  EXPECT_FALSE(s->open_chunk(0)); // numeric bytes never exposed before verification
  EXPECT_FALSE(PanelStore::open((root / "store").string(), std::string(64, '0')));
  fs::resize_file(root / "store" / "chunk-1.bin", 32);
  EXPECT_FALSE(PanelStore::open((root / "store").string()));
}

TEST_F(DataPanelStoreV2, SharedMappingBudgetAndViewLifetime) {
  ASSERT_TRUE(write("store"));
  auto s = PanelStore::open((root / "store").string(), {}, 128ULL * 1024 * 1024, 1); ASSERT_TRUE(s);
  auto copy = *s;
  { auto a = s->open_chunk(0); ASSERT_TRUE(a);
    EXPECT_FALSE(copy.open_chunk(1)); // copies share the reservation
    auto borrowed = *a; a = core::Err(core::ErrorCode::InvalidArgument, "discard original");
    std::array<f64, 2> row{}; ASSERT_TRUE(borrowed.read_field_row(0, 1, row)); EXPECT_EQ(row[1], 21.25);
  }
  EXPECT_TRUE(copy.open_chunk(1));
  std::array<f64, 2> row{}; EXPECT_TRUE(s->forward_returns(0, 3, 4, row)); // one live mapping suffices
}

TEST_F(DataPanelStoreV2, BoundsCanonicalAxesAndStrictMaskClock) {
  auto c = config(); c.instrument_ids = {100, 17}; EXPECT_FALSE(preflight_panel_store(c));
  c = config(); c.original_indices = {1, 1}; EXPECT_FALSE(preflight_panel_store(c));
  c = config(); c.max_working_bytes = 1; EXPECT_FALSE(preflight_panel_store(c));
  c = config(); c.fields[1].precision = PanelStorePrecision::Float32V2; EXPECT_FALSE(preflight_panel_store(c));
  auto w = PanelStoreWriter::create((root / "store").string(), config()); ASSERT_TRUE(w);
  std::array<f64, 2> close{10, 20}, returns{0, 0};
  const std::array<std::span<const f64>, 2> fields{close, returns};
  std::array<u8, 2> present{1, 1}, tradable{1, 1};
  EXPECT_FALSE(w->append_date(0, fields, close, present, tradable, kStart));
  present[1] = 0; EXPECT_FALSE(w->append_date(0, fields, close, present, tradable, kStart - 1));
  present[1] = 1; close[0] = std::numeric_limits<f64>::max();
  EXPECT_FALSE(w->append_date(0, fields, close, present, tradable, kStart - 1));
  close[0] = 1e-300; EXPECT_FALSE(w->append_date(0, fields, close, present, tradable, kStart - 1));
  close[0] = 10; ASSERT_TRUE(w->append_date(0, fields, close, present, tradable, kStart - 1));
}

TEST_F(DataPanelStoreV2, FutureMutationLeavesEarlierBytesAndReturnsUnchanged) {
  ASSERT_TRUE(write("prefix")); ASSERT_TRUE(write("mutated", 100));
  auto a = PanelStore::open((root / "prefix").string()); auto b = PanelStore::open((root / "mutated").string());
  ASSERT_TRUE(a); ASSERT_TRUE(b); auto ca = a->open_chunk(0), cb = b->open_chunk(0); ASSERT_TRUE(ca); ASSERT_TRUE(cb);
  std::array<f64, 2> x{}, y{};
  for (usize f = 0; f < 2; ++f) for (usize t = 0; t < 2; ++t) {
    ASSERT_TRUE(ca->read_field_row(f, t, x)); ASSERT_TRUE(cb->read_field_row(f, t, y));
    for (usize i = 0; i < 2; ++i) EXPECT_EQ(std::bit_cast<u64>(x[i]), std::bit_cast<u64>(y[i]));
  }
  ASSERT_TRUE(a->forward_returns(0, 1, 2, x)); ASSERT_TRUE(b->forward_returns(0, 1, 2, y));
  EXPECT_EQ(std::bit_cast<u64>(x[0]), std::bit_cast<u64>(y[0]));
}
} // namespace
