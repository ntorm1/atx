#include <filesystem>
#include <fstream>
#include <string>
#include <vector>
#include <gtest/gtest.h>
#include "atx/engine/regime/loader.hpp"
#include "atx/engine/regime/store.hpp"

namespace atxtest_regime_loader {
namespace fs = std::filesystem;

std::string write_csv(const fs::path &dir, const std::string &name,
                      const std::string &body) {
  const fs::path p = dir / name;
  std::ofstream(p, std::ios::binary) << body;
  return p.string();
}

[[nodiscard]] bool files_equal(const std::string &a, const std::string &b) {
  std::ifstream fa(a, std::ios::binary), fb(b, std::ios::binary);
  std::string sa((std::istreambuf_iterator<char>(fa)), {});
  std::string sb((std::istreambuf_iterator<char>(fb)), {});
  return sa == sb;
}

using namespace atx::engine::regime;

[[nodiscard]] RegimeLoadConfig make_cfg(const fs::path &dir, const std::string &out) {
  write_csv(dir, "dgs2.csv", "DATE,VALUE\n2020-01-02,1.0\n2020-01-06,1.5\n");
  write_csv(dir, "dgs10.csv", "DATE,VALUE\n2020-01-02,2.0\n2020-01-06,2.5\n");
  RegimeLoadConfig cfg;
  cfg.staging_dir = dir.string();
  cfg.out_path = out;
  cfg.series = {{"dgs2", "dgs2.csv", CsvFormat::Fred, "VALUE"},
                {"dgs10", "dgs10.csv", CsvFormat::Fred, "VALUE"}};
  cfg.derived = {"t10y2y = dgs10 - dgs2"};
  cfg.created_at_nanos = 12345;
  return cfg;
}

TEST(RegimeLoader, LoadsAndDerivesCorrectly) {
  const fs::path dir = fs::temp_directory_path() / "atx_regime_loader_a";
  fs::create_directories(dir);
  const std::string out = (dir / "regime.seg").string();
  auto st = load_regime_history(make_cfg(dir, out));
  ASSERT_TRUE(st.has_value()) << (st ? "" : st.error().message());
  EXPECT_EQ(st.value().series_count, 3);     // dgs2, dgs10, t10y2y
  EXPECT_EQ(st.value().dates_written, 2);

  auto store = RegimeStore::open(out).value();
  const atx::i64 d02 = date_to_nanos("2020-01-02").value();
  const atx::i64 d06 = date_to_nanos("2020-01-06").value();
  EXPECT_DOUBLE_EQ(store.value("dgs2", d02), 1.0);
  EXPECT_DOUBLE_EQ(store.value("dgs10", d02), 2.0);
  EXPECT_DOUBLE_EQ(store.value("t10y2y", d02), 1.0);  // 2.0 - 1.0
  EXPECT_DOUBLE_EQ(store.value("t10y2y", d06), 1.0);  // 2.5 - 1.5
}

TEST(RegimeLoader, ByteIdenticalAcrossRuns) {
  const fs::path dir = fs::temp_directory_path() / "atx_regime_loader_b";
  fs::create_directories(dir);
  const std::string out1 = (dir / "r1.seg").string();
  const std::string out2 = (dir / "r2.seg").string();
  ASSERT_TRUE(load_regime_history(make_cfg(dir, out1)).has_value());
  ASSERT_TRUE(load_regime_history(make_cfg(dir, out2)).has_value());
  EXPECT_TRUE(files_equal(out1, out2));
}
}  // namespace atxtest_regime_loader
