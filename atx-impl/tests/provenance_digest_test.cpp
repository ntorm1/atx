// atx::impl — determinism tripwire: provenance must NOT enter panel.bin (p7 S6-5).
//
// The S6-3 (wall_ms) and S6-4 (config_json / engine_git_sha) provenance lives in
// the discover run DB (PipelineRunRow / pipeline_iteration), NOT in any binary
// output on the deterministic path. This test-only unit is the permanent
// regression tripwire: it proves the panel.bin fnv1a64 trailer is a pure function
// of the panel's contents and is unaffected by (a) re-running, (b) wall-clock time
// passing, or (c) varying discover-stage config fields that feed config_json but
// have nothing to do with panel construction.
//
// No production code here. If a future refactor ever wires a provenance field into
// the panel serializer, ConfigJsonNotInPanelDigest / WallMsNotInPanelDigest break.

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "atx/core/types.hpp"

#include "atx/engine/data/orats_history.hpp" // kOratsFields
#include "atx/tsdb/load_parquet.hpp"         // build_from_long, LongColumns

#include "config.hpp"
#include "serialize_panel.hpp"
#include "stages.hpp"

namespace atxtest_provenance_digest {

namespace fs = std::filesystem;

constexpr atx::i64 kDayNanos = 86400LL * 1'000'000'000LL;
constexpr atx::i64 kDay0 = 18263LL * kDayNanos;

void write_seg_day(const fs::path &dir, int day_index, atx::i64 dn, int n_instr,
                   atx::f64 base_close) {
  const auto r = static_cast<atx::usize>(n_instr);
  atx::tsdb::LongColumns cols;
  cols.field_names.assign(atx::engine::data::kOratsFields.begin(),
                          atx::engine::data::kOratsFields.end());
  cols.times.assign(r, dn);
  cols.symbols.reserve(r);
  for (int i = 0; i < n_instr; ++i) {
    cols.symbols.push_back(std::to_string(10001 + i));
  }
  cols.values.assign(atx::engine::data::kOratsFields.size(), std::vector<atx::f64>(r, 0.0));
  for (int i = 0; i < n_instr; ++i) {
    cols.values[3][static_cast<atx::usize>(i)] = base_close + static_cast<atx::f64>(i);
  }
  cols.values[6].assign(r, 1.0e6); // volume
  cols.values[7].assign(r, 2.0e8); // shares
  cols.values[10].assign(r, 1.0);  // cumReturnFactor
  auto ok = atx::tsdb::build_from_long(
      cols, (dir / ("day_" + std::to_string(1000 + day_index) + ".seg")).string(), 0);
  ASSERT_TRUE(ok.has_value()) << "write_seg_day failed for day " << day_index;
}

// Build a fresh synthetic seg partition under a unique dir; returns the dir path.
fs::path make_partition(const char *tag, int dates = 10, int instr = 5) {
  const fs::path dir = fs::temp_directory_path() / (std::string("atx_provdig_") + tag);
  std::error_code ec;
  fs::remove_all(dir, ec);
  fs::create_directories(dir, ec);
  for (int d = 0; d < dates; ++d) {
    const atx::i64 dn = kDay0 + static_cast<atx::i64>(d) * kDayNanos;
    write_seg_day(dir, d, dn, instr, 70.0 + static_cast<atx::f64>(d) * 0.2);
  }
  return dir;
}

std::string read_bytes(const std::string &path) {
  std::ifstream f{path, std::ios::binary};
  return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

// Run run_panel over `seg_dir` into a tagged temp file; return {digest, path}.
struct PanelOut {
  atx::u64 digest{};
  std::string path;
};

PanelOut run_panel_into(const std::string &seg_dir, const atx::impl::RunConfig &base,
                        const char *tag) {
  PanelOut out;
  out.path =
      (fs::temp_directory_path() / (std::string("atx_provdig_out_") + tag + ".bin")).string();
  fs::remove(fs::path(out.path));
  fs::remove(fs::path(out.path + ".meta.txt"));
  atx::impl::RunConfig cfg = base;
  cfg.segs = seg_dir;
  cfg.panel_out = out.path;
  auto r = atx::impl::run_panel(cfg);
  EXPECT_TRUE(r.has_value()) << (r ? "" : r.error().message());
  if (r.has_value()) {
    out.digest = r->digest;
  }
  return out;
}

void cleanup(const PanelOut &p) {
  std::error_code ec;
  fs::remove(fs::path(p.path), ec);
  fs::remove(fs::path(p.path + ".meta.txt"), ec);
}

// ---------------------------------------------------------------------------
// PanelDigestUnchangedByProvenance — two run_panel calls on the same fixture
// produce identical panel.bin bytes (the deterministic baseline).
// ---------------------------------------------------------------------------
TEST(AtxImplProvenanceDigest, PanelDigestUnchangedByProvenance) {
  const fs::path seg_dir = make_partition("baseline");
  atx::impl::RunConfig base;
  base.min_adv_usd = 0.0;
  base.top_n_by_adv = 0;

  const PanelOut a = run_panel_into(seg_dir.string(), base, "base_a");
  const PanelOut b = run_panel_into(seg_dir.string(), base, "base_b");

  EXPECT_NE(a.digest, atx::u64{0});
  EXPECT_EQ(a.digest, b.digest) << "panel.bin digest must be reproducible";
  EXPECT_EQ(read_bytes(a.path), read_bytes(b.path)) << "panel.bin bytes must be identical";

  cleanup(a);
  cleanup(b);
  std::error_code ec;
  fs::remove_all(seg_dir, ec);
}

// ---------------------------------------------------------------------------
// WallMsNotInPanelDigest — wall-clock time passing between two runs does not
// change the panel digest. wall_ms is captured by the discover sink, never by the
// panel serializer; this is the explicit proof it cannot reach panel.bin.
// ---------------------------------------------------------------------------
TEST(AtxImplProvenanceDigest, WallMsNotInPanelDigest) {
  const fs::path seg_dir = make_partition("wallms");
  atx::impl::RunConfig base;
  base.min_adv_usd = 0.0;
  base.top_n_by_adv = 0;

  const PanelOut a = run_panel_into(seg_dir.string(), base, "wall_a");
  std::this_thread::sleep_for(std::chrono::milliseconds(5)); // advance the wall clock
  const PanelOut b = run_panel_into(seg_dir.string(), base, "wall_b");

  EXPECT_EQ(a.digest, b.digest) << "panel digest must not depend on elapsed wall time";
  EXPECT_EQ(read_bytes(a.path), read_bytes(b.path));

  cleanup(a);
  cleanup(b);
  std::error_code ec;
  fs::remove_all(seg_dir, ec);
}

// ---------------------------------------------------------------------------
// ConfigJsonNotInPanelDigest — varying the config fields that feed config_json
// (seed + gate floors, which have NOTHING to do with panel construction) leaves
// the panel.bin digest identical. Proves config_json never reaches the serializer.
// ---------------------------------------------------------------------------
TEST(AtxImplProvenanceDigest, ConfigJsonNotInPanelDigest) {
  const fs::path seg_dir = make_partition("cfgjson");

  atx::impl::RunConfig cfg1;
  cfg1.min_adv_usd = 0.0;
  cfg1.top_n_by_adv = 0;
  // Provenance-affecting (discover) fields — irrelevant to the panel build.
  cfg1.seed = 1ULL;
  cfg1.min_sharpe = 0.10;
  cfg1.min_dsr = 0.20;
  cfg1.max_turnover = 0.5;

  atx::impl::RunConfig cfg2 = cfg1;
  cfg2.seed = 999ULL;        // different config_json
  cfg2.min_sharpe = 0.90;    // different config_json
  cfg2.min_dsr = 0.80;       // different config_json
  cfg2.max_turnover = 9.9;   // different config_json

  const PanelOut a = run_panel_into(seg_dir.string(), cfg1, "cfg_a");
  const PanelOut b = run_panel_into(seg_dir.string(), cfg2, "cfg_b");

  EXPECT_NE(a.digest, atx::u64{0});
  EXPECT_EQ(a.digest, b.digest)
      << "panel digest must be invariant to discover-config (config_json) fields";
  EXPECT_EQ(read_bytes(a.path), read_bytes(b.path));

  cleanup(a);
  cleanup(b);
  std::error_code ec;
  fs::remove_all(seg_dir, ec);
}

} // namespace atxtest_provenance_digest
