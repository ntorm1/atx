// cluster_panel_store_test.cpp — S11-4 persisted cluster-panel artifact.
//
// Covers the external binary artifact + SQLite registry that caches a built
// atx::engine::alpha::ClusterPanel: register/lookup/locate over the new
// cluster_panel table (mirrors segment_index), the deterministic binary
// save/load round-trip, the FNV-1a-64 params-hash key (replay-stable +
// field-sensitive), content_hash corruption detection, and the Dev->UAT->PROD
// promotion copy that dual-writes a cluster_panel_built alpha_event.
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "atx/engine/alpha/cluster_panel.hpp"   // ClusterPanel (the built result)
#include "atx/engine/store/cluster_panel.hpp"    // ClusterPanelRecord + registry
#include "atx/engine/store/db.hpp"
#include "atx/engine/store/promotion.hpp"

namespace atxtest_cluster_panel_store_test {

using atx::engine::store::ClusterPanelRecord;
using atx::engine::store::StoreDb;
namespace cp = atx::engine::store::cluster_panel;
namespace pr = atx::engine::store::promotion;

// A scratch directory unique to the running test case, cleared on entry.
[[nodiscard]] std::string tmpdir() {
  const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
  const std::filesystem::path dir = std::filesystem::temp_directory_path() / "atx_cluster_panel" /
      (std::string(info->test_suite_name()) + "_" + info->name());
  std::error_code e; std::filesystem::remove_all(dir, e); std::filesystem::create_directories(dir, e);
  return dir.string();
}

// A non-trivial built panel: two snapshots, kUnclustered cells, distinct labels.
[[nodiscard]] atx::engine::alpha::ClusterPanel sample_panel() {
  using atx::engine::alpha::ClusterPanel;
  ClusterPanel p;
  p.instruments = 4;
  ClusterPanel::Snapshot s0;
  s0.date = 5;
  s0.cluster_id = {0, 1, ClusterPanel::kUnclustered, 0};
  s0.n_labels = 2;
  ClusterPanel::Snapshot s1;
  s1.date = 10;
  s1.cluster_id = {0, 0, 1, 1};
  s1.n_labels = 2;
  p.snapshots = {s0, s1};
  return p;
}

// A representative record whose binary_path points at a freshly written artifact.
[[nodiscard]] ClusterPanelRecord sample_record(const std::string& binary_path) {
  ClusterPanelRecord rec;
  rec.panel_id = "panel-1";
  rec.universe_id = "univ-A";
  rec.window_start = 0;
  rec.window_end = 10;
  rec.recluster_every = 5;
  rec.params_hash = 0xABCDull;
  rec.asof_date = "2026-06-19";
  rec.binary_path = binary_path;
  rec.content_hash = 0x1234ull;
  rec.algo = "hierarchical";
  rec.k = 2;
  rec.created_at = "2026-06-19T00:00:00Z";
  rec.created_by_run_id = "run-1";
  return rec;
}

// The canonical params used to key a panel — folded into compute_params_hash.
[[nodiscard]] cp::ParamsKey sample_key() {
  cp::ParamsKey key;
  key.universe_id = "univ-A";
  key.universe_content_hash = 0xAAAAull;
  key.source_content_hash = 0xBBBBull;
  key.window = 60;
  key.recluster_every = 5;
  key.k = 8;
  key.residualize = "CAPM";
  key.return_field = "ret";
  key.algo = "hierarchical";
  return key;
}

// --- 1. register -> lookup round-trip --------------------------------------
TEST(ClusterPanelStore, RegisterLookupRoundTrip) {
  const std::string dir = tmpdir();
  auto s = StoreDb::open_memory(); ASSERT_TRUE(s.has_value());
  auto& db = s->db();
  const auto rec = sample_record(dir + "/panel-1.atxcp");
  ASSERT_TRUE(cp::register_panel(db, rec).has_value());

  auto got = cp::lookup(db, rec.universe_id, rec.asof_date, rec.params_hash);
  ASSERT_TRUE(got.has_value());
  ASSERT_TRUE(got->has_value());
  const ClusterPanelRecord& r = **got;
  EXPECT_EQ(r.panel_id, rec.panel_id);
  EXPECT_EQ(r.universe_id, rec.universe_id);
  EXPECT_EQ(r.window_start, rec.window_start);
  EXPECT_EQ(r.window_end, rec.window_end);
  EXPECT_EQ(r.recluster_every, rec.recluster_every);
  EXPECT_EQ(r.params_hash, rec.params_hash);
  EXPECT_EQ(r.asof_date, rec.asof_date);
  EXPECT_EQ(r.binary_path, rec.binary_path);
  EXPECT_EQ(r.content_hash, rec.content_hash);
  EXPECT_EQ(r.algo, rec.algo);
  EXPECT_EQ(r.k, rec.k);
  EXPECT_EQ(r.created_at, rec.created_at);
  EXPECT_EQ(r.created_by_run_id, rec.created_by_run_id);
}

// A miss returns Ok(nullopt), not Err.
TEST(ClusterPanelStore, LookupMissReturnsNullopt) {
  auto s = StoreDb::open_memory(); ASSERT_TRUE(s.has_value());
  auto got = cp::lookup(s->db(), "nobody", "2026-01-01", 0x9999ull);
  ASSERT_TRUE(got.has_value());
  EXPECT_FALSE(got->has_value());
}

// register is idempotent on panel_id (INSERT OR REPLACE), so a re-register of the
// same id does not duplicate the row.
TEST(ClusterPanelStore, RegisterIdempotentOnPanelId) {
  const std::string dir = tmpdir();
  auto s = StoreDb::open_memory(); ASSERT_TRUE(s.has_value());
  auto& db = s->db();
  const auto rec = sample_record(dir + "/panel-1.atxcp");
  ASSERT_TRUE(cp::register_panel(db, rec).has_value());
  ASSERT_TRUE(cp::register_panel(db, rec).has_value());
  auto st = db.prepare("SELECT COUNT(*) FROM cluster_panel WHERE panel_id = ?1");
  ASSERT_TRUE(st.has_value());
  ASSERT_TRUE(st->bind(1, rec.panel_id).has_value());
  auto step = st->step(); ASSERT_TRUE(step.has_value());
  ASSERT_EQ(*step, atx::core::db::Statement::Step::Row);
  EXPECT_EQ(st->column_int(0), 1);
}

// locate returns the binary_path for a registered (universe, asof, params) key.
TEST(ClusterPanelStore, LocateReturnsBinaryPath) {
  const std::string dir = tmpdir();
  auto s = StoreDb::open_memory(); ASSERT_TRUE(s.has_value());
  auto& db = s->db();
  const auto rec = sample_record(dir + "/panel-1.atxcp");
  ASSERT_TRUE(cp::register_panel(db, rec).has_value());
  auto loc = cp::locate(db, rec.universe_id, rec.asof_date, rec.params_hash);
  ASSERT_TRUE(loc.has_value());
  EXPECT_EQ(*loc, rec.binary_path);
  // Unmapped key -> Err(NotFound).
  auto missing = cp::locate(db, rec.universe_id, rec.asof_date, 0x0ull);
  EXPECT_FALSE(missing.has_value());
}

// --- 2. binary save -> load round-trip -------------------------------------
TEST(ClusterPanelStore, BinarySaveLoadRoundTrip) {
  const std::string dir = tmpdir();
  const std::string path = dir + "/panel.atxcp";
  const auto panel = sample_panel();
  ASSERT_TRUE(cp::save_binary(path, panel).has_value());

  auto loaded = cp::load_binary(path);
  ASSERT_TRUE(loaded.has_value());
  EXPECT_EQ(loaded->instruments, panel.instruments);
  ASSERT_EQ(loaded->snapshots.size(), panel.snapshots.size());
  for (std::size_t i = 0; i < panel.snapshots.size(); ++i) {
    EXPECT_EQ(loaded->snapshots[i].date, panel.snapshots[i].date);
    EXPECT_EQ(loaded->snapshots[i].n_labels, panel.snapshots[i].n_labels);
    EXPECT_EQ(loaded->snapshots[i].cluster_id, panel.snapshots[i].cluster_id);
  }
}

// save -> load -> save reproduces byte-identical artifacts AND content_hash.
TEST(ClusterPanelStore, BinarySaveIsByteStable) {
  const std::string dir = tmpdir();
  const std::string path_a = dir + "/a.atxcp";
  const std::string path_b = dir + "/b.atxcp";
  const auto panel = sample_panel();
  auto ha = cp::save_binary(path_a, panel); ASSERT_TRUE(ha.has_value());
  auto loaded = cp::load_binary(path_a); ASSERT_TRUE(loaded.has_value());
  auto hb = cp::save_binary(path_b, *loaded); ASSERT_TRUE(hb.has_value());
  // save_binary returns the content_hash of the bytes it wrote.
  EXPECT_EQ(*ha, *hb);
  // The two files are byte-identical.
  const auto read_all = [](const std::string& p) {
    std::ifstream f(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
  };
  EXPECT_EQ(read_all(path_a), read_all(path_b));
}

// --- 3. params_hash determinism + field sensitivity (mirror fingerprint_test)
TEST(ClusterPanelStore, ParamsHashIdenticalInputsSameHash) {
  EXPECT_EQ(cp::compute_params_hash(sample_key()), cp::compute_params_hash(sample_key()));
}

TEST(ClusterPanelStore, ParamsHashTwoRunsEqual) {
  // Two independent constructions of the same key hash identically (replay).
  const auto a = sample_key();
  const auto b = sample_key();
  EXPECT_EQ(cp::compute_params_hash(a), cp::compute_params_hash(b));
}

TEST(ClusterPanelStore, ParamsHashDiffersOnEveryField) {
  const atx::u64 base = cp::compute_params_hash(sample_key());
  { auto x = sample_key(); x.universe_id = "univ-B";          EXPECT_NE(base, cp::compute_params_hash(x)); }
  { auto x = sample_key(); x.universe_content_hash = 0xCCCCull; EXPECT_NE(base, cp::compute_params_hash(x)); }
  { auto x = sample_key(); x.source_content_hash = 0xDDDDull;  EXPECT_NE(base, cp::compute_params_hash(x)); }
  { auto x = sample_key(); x.window = 61;                      EXPECT_NE(base, cp::compute_params_hash(x)); }
  { auto x = sample_key(); x.recluster_every = 6;             EXPECT_NE(base, cp::compute_params_hash(x)); }
  { auto x = sample_key(); x.k = 9;                            EXPECT_NE(base, cp::compute_params_hash(x)); }
  { auto x = sample_key(); x.residualize = "None";            EXPECT_NE(base, cp::compute_params_hash(x)); }
  { auto x = sample_key(); x.return_field = "logret";         EXPECT_NE(base, cp::compute_params_hash(x)); }
  { auto x = sample_key(); x.algo = "sponge";                 EXPECT_NE(base, cp::compute_params_hash(x)); }
}

// --- 4. content_hash mismatch detection ------------------------------------
TEST(ClusterPanelStore, ContentHashDetectsCorruption) {
  const std::string dir = tmpdir();
  const std::string path = dir + "/panel.atxcp";
  const auto panel = sample_panel();
  auto h = cp::save_binary(path, panel); ASSERT_TRUE(h.has_value());
  // Recomputing the hash over the untouched bytes matches.
  auto h2 = cp::content_hash_of_file(path); ASSERT_TRUE(h2.has_value());
  EXPECT_EQ(*h2, *h);
  // Flip one byte in the payload; the recomputed hash must differ.
  {
    std::fstream f(path, std::ios::in | std::ios::out | std::ios::binary);
    ASSERT_TRUE(f.good());
    f.seekg(0, std::ios::end);
    const auto sz = f.tellg();
    ASSERT_GT(sz, 0);
    f.seekp(sz - std::streamoff(1));
    char b; f.seekg(sz - std::streamoff(1)); f.read(&b, 1);
    char flipped = static_cast<char>(b ^ 0xFF);
    f.seekp(sz - std::streamoff(1)); f.write(&flipped, 1);
  }
  auto h3 = cp::content_hash_of_file(path); ASSERT_TRUE(h3.has_value());
  EXPECT_NE(*h3, *h);
}

// --- 5. promotion copies the row + writes cluster_panel_built alpha_event ---
TEST(ClusterPanelStore, PromotionCopiesRowAndWritesEvent) {
  const std::string dir = tmpdir();
  const std::string dev = dir + "/atx_dev.sqlite";
  const std::string uat = dir + "/atx_uat.sqlite";
  const auto rec = sample_record(dir + "/panel-1.atxcp");
  // Seed dev with a registered panel.
  { auto d = StoreDb::open(dev); ASSERT_TRUE(d.has_value());
    ASSERT_TRUE(cp::register_panel(d->db(), rec).has_value()); }
  // Create the dest env so its schema exists.
  { auto u = StoreDb::open(uat); ASSERT_TRUE(u.has_value()); }

  auto d = StoreDb::open(dev); ASSERT_TRUE(d.has_value());
  pr::PromotionRequest req{0xABCull, "dev", "uat", "run-1", "nathan", /*ts*/20, uat};
  ASSERT_TRUE(pr::promote(d->db(), req).has_value());

  auto u = StoreDb::open(uat); ASSERT_TRUE(u.has_value());
  // The cluster_panel row is now present in uat.
  auto got = cp::lookup(u->db(), rec.universe_id, rec.asof_date, rec.params_hash);
  ASSERT_TRUE(got.has_value());
  ASSERT_TRUE(got->has_value());
  EXPECT_EQ((*got)->panel_id, rec.panel_id);
  // A cluster_panel_built alpha_event was dual-written into uat.
  auto st = u->db().prepare(
      "SELECT COUNT(*) FROM alpha_event WHERE event_type = 'cluster_panel_built'");
  ASSERT_TRUE(st.has_value());
  auto step = st->step(); ASSERT_TRUE(step.has_value());
  ASSERT_EQ(*step, atx::core::db::Statement::Step::Row);
  EXPECT_GE(st->column_int(0), 1);
}

}  // namespace atxtest_cluster_panel_store_test
