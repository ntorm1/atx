#include <cmath>
#include <filesystem>
#include <string>
#include <vector>
#include <gtest/gtest.h>
#include "atx/core/types.hpp"
#include "atx/tsdb/builder.hpp"
#include "atx/engine/regime/store.hpp"

namespace atxtest_regime_store {
constexpr atx::i64 kDay = 86400LL * 1000000000LL;

[[nodiscard]] std::string write_fixture() {
  std::vector<std::string> fields = {"vix", "move"};
  std::vector<std::string> syms = {"MACRO"};
  std::vector<atx::i64> axis = {2 * kDay, 4 * kDay, 6 * kDay};
  atx::tsdb::SegmentBuilder b(fields, syms, axis);
  // set(field, TIME_INDEX, inst, value): 2nd arg indexes `axis`, it is NOT nanos.
  b.set(0, 0, 0, 15.0); b.set(0, 1, 0, 25.0); b.set(0, 2, 0, 18.0);   // vix  @ axis[0..2]
  b.set(1, 0, 0, 80.0); b.set(1, 1, 0, 90.0); b.set(1, 2, 0, 100.0);  // move @ axis[0..2]
  const std::string path =
      (std::filesystem::temp_directory_path() / "atx_regime_store_fixture.seg").string();
  EXPECT_TRUE(b.write(path, /*created_at_nanos=*/0).has_value());
  return path;
}

using atx::engine::regime::RegimeStore;

TEST(RegimeStore, OpensAndExposesSeriesAndAxis) {
  const std::string path = write_fixture();
  auto s = RegimeStore::open(path);
  ASSERT_TRUE(s.has_value()) << (s ? "" : s.error().message());
  EXPECT_EQ(s.value().series_names().size(), 2u);
  EXPECT_EQ(s.value().date_axis().size(), 3u);
}

TEST(RegimeStore, AsOfLookup_ExactAndBetweenAndBefore) {
  const std::string path = write_fixture();
  auto s = RegimeStore::open(path).value();
  EXPECT_DOUBLE_EQ(s.value("vix", 4 * kDay), 25.0);        // exact date
  EXPECT_DOUBLE_EQ(s.value("vix", 5 * kDay), 25.0);        // between -> last <= 5
  EXPECT_DOUBLE_EQ(s.value("vix", 100 * kDay), 18.0);      // past last -> carry
  EXPECT_TRUE(std::isnan(s.value("vix", 1 * kDay)));       // before first axis date
  EXPECT_TRUE(std::isnan(s.value("nope", 4 * kDay)));      // unknown series
}
}  // namespace atxtest_regime_store
