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
// It ALSO proves the dual S6-5 invariant for the DISCOVER digest:
// ConfigJsonNotInDiscoverDigest runs the gated discover stage TWICE on the same
// tiny fixture, differing ONLY in a config field that IS serialized into config_json
// but is NOT folded into the discover fingerprint NOR read by the (typed_fields-off)
// search — so the discover stage digest + _manifest.txt stay byte-identical while the
// persisted config_json strings differ (non-vacuous).
//
// No production code here. If a future refactor ever wires a provenance field into
// the panel serializer or the discover digest, these tripwires break.

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "atx/core/types.hpp"

#include "atx/engine/alpha/panel.hpp"        // alpha::Panel (discover fixture)
#include "atx/engine/data/orats_history.hpp" // kOratsFields
#include "atx/engine/store/db.hpp"           // store::StoreDb (read back config_json)
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

// ===========================================================================
// ConfigJsonNotInDiscoverDigest (S6-5, discover side) — the dual of the panel
// tripwires above. The panel tests prove config_json never reaches panel.bin;
// this proves it never reaches the DISCOVER stage digest either.
//
// Two gated discover runs on the SAME tiny momentum fixture differ ONLY in
// `field_cardinality_max`. That field is:
//   (a) serialized into config_json (build_config_json, kv_i "field_cardinality_max"),
//   (b) NOT folded into compute_discover_fingerprint (store_progress_sink.cpp folds
//       only panel/seed/population/generations/seed_exprs + gate & oos floors), and
//   (c) only READ when --typed-fields is set; with typed_fields=false (the default
//       here) the search never reads it, so it cannot perturb the admitted set,
//       the stage digest, or _manifest.txt.
// We pick this field precisely because the assertion is digest-EQUALITY: a book-
// scoring field (weight_transform/winsorize_limit/gross_leverage) WOULD change the
// WeightPolicy and thus the digest, so it cannot serve as a digest-invariant probe.
// The test is non-vacuous: it asserts the two persisted config_json strings DIFFER
// while the discover digest + manifest are byte-identical.
// ===========================================================================
namespace store = atx::engine::store;
using atx::engine::alpha::Panel;

// Deterministic noisy-momentum panel (mirrors store_discover_test.cpp's fixture).
struct DiscLcg {
  std::uint64_t s;
  [[nodiscard]] atx::f64 next() noexcept {
    s = s * 6364136223846793005ULL + 1442695040888963407ULL;
    const std::uint64_t hi = s >> 11U;
    const atx::f64 u = static_cast<atx::f64>(hi) / static_cast<atx::f64>(1ULL << 53U);
    return 2.0 * u - 1.0;
  }
};

std::optional<Panel> make_momentum_panel(atx::usize dates = 96, atx::usize insts = 6) {
  std::vector<atx::f64> drift(insts);
  for (atx::usize j = 0; j < insts; ++j) {
    drift[j] = 0.006 - 0.0024 * static_cast<atx::f64>(j);
  }
  std::vector<atx::f64> close(dates * insts);
  std::vector<atx::f64> px(insts, 100.0);
  DiscLcg rng{0xBEEFCAFEULL};
  for (atx::usize t = 0; t < dates; ++t) {
    for (atx::usize j = 0; j < insts; ++j) {
      px[j] *= (1.0 + drift[j] + 0.010 * rng.next());
      close[t * insts + j] = px[j];
    }
  }
  auto r = Panel::create(dates, insts, {"close"}, {close}, {});
  if (!r.has_value()) {
    ADD_FAILURE() << "momentum panel fixture must build: " << r.error().to_string();
    return std::nullopt;
  }
  return std::move(r.value());
}

// A permissive gated RunConfig so the tiny fixture admits >= 1 alpha (mirrors
// store_discover_test.cpp::gated_cfg). typed_fields stays at its default (false).
atx::impl::RunConfig disc_gated_cfg(const std::string &panel_path,
                                    const std::string &alpha_out) {
  atx::impl::RunConfig cfg;
  cfg.subcommand = "discover";
  cfg.panel = panel_path;
  cfg.alpha_out = alpha_out;
  cfg.seed = 777ULL;
  cfg.population = 16;
  cfg.generations = 5;
  cfg.seed_exprs = {"rank(close)", "ts_mean(close,10)", "delta(close,2)"};
  cfg.gated = true;
  cfg.min_sharpe = 0.0;
  cfg.min_fitness = 0.0;
  cfg.max_turnover = 10.0;
  cfg.max_pool_corr = 1.0;
  cfg.min_dsr = 0.0;
  return cfg;
}

std::string read_run_config_json(store::StoreDb &db) {
  auto stmt_r = db.db().prepare_cached("SELECT config_json FROM pipeline_run LIMIT 1");
  EXPECT_TRUE(stmt_r.has_value());
  if (!stmt_r.has_value()) return "";
  auto *stmt = *stmt_r;
  auto step = stmt->step();
  EXPECT_TRUE(step.has_value());
  if (!step.has_value() || *step != atx::core::db::Statement::Step::Row) return "";
  return std::string{stmt->column_text(0)};
}

TEST(AtxImplProvenanceDigest, ConfigJsonNotInDiscoverDigest) {
  auto panel = make_momentum_panel();
  ASSERT_TRUE(panel.has_value());
  const std::string panel_path =
      (fs::temp_directory_path() / "atx_provdig_disc_panel.bin").string();
  ASSERT_TRUE(atx::impl::write_panel(*panel, panel_path).has_value());

  // Run the gated discover stage once with a temp run-db; return {digest, manifest,
  // config_json}. Only `field_cardinality_max` differs between the two invocations.
  struct DiscOut {
    atx::u64 digest{};
    std::string manifest;
    std::string config_json;
  };
  auto run_once = [&](const char *tag, int fcm) -> DiscOut {
    DiscOut out;
    const std::string alpha_out =
        (fs::temp_directory_path() / (std::string("atx_provdig_disc_out_") + tag)).string();
    const std::string db_path =
        (fs::temp_directory_path() / (std::string("atx_provdig_disc_") + tag + ".db")).string();
    std::error_code ec0;
    fs::remove_all(alpha_out, ec0);
    fs::remove(db_path, ec0);

    atx::impl::RunConfig cfg = disc_gated_cfg(panel_path, alpha_out);
    cfg.run_db = db_path;
    cfg.field_cardinality_max = fcm; // serialized into config_json; NOT in the digest
    auto r = atx::impl::run_discover(cfg);
    EXPECT_TRUE(r.has_value()) << (r ? "" : r.error().message());
    if (r.has_value()) out.digest = r->digest;

    out.manifest = read_bytes((fs::path{alpha_out} / "_manifest.txt").string());
    auto db_r = store::StoreDb::open(db_path);
    EXPECT_TRUE(db_r.has_value());
    if (db_r.has_value()) {
      store::StoreDb db = std::move(*db_r);
      out.config_json = read_run_config_json(db);
    }

    fs::remove_all(alpha_out, ec0);
    fs::remove(db_path, ec0);
    return out;
  };

  const DiscOut a = run_once("a", 12);  // default
  const DiscOut b = run_once("b", 999); // different config_json only

  // The probe field must actually be serialized AND must actually differ (non-vacuous).
  EXPECT_FALSE(a.config_json.empty()) << "config_json must be populated";
  EXPECT_NE(a.config_json.find("\"field_cardinality_max\":12"), std::string::npos)
      << "probe field must be serialized into config_json";
  EXPECT_NE(b.config_json.find("\"field_cardinality_max\":999"), std::string::npos)
      << "probe field must be serialized into config_json";
  EXPECT_NE(a.config_json, b.config_json)
      << "config_json must differ across the two runs (non-vacuous tripwire)";

  // Yet the discover stage digest + manifest are byte-identical: config_json (and the
  // un-fingerprinted, typed_fields-off field_cardinality_max) never enter the digest.
  EXPECT_NE(a.digest, atx::u64{0});
  EXPECT_EQ(a.digest, b.digest)
      << "discover stage digest must be invariant to config_json-only fields";
  EXPECT_FALSE(a.manifest.empty());
  EXPECT_EQ(a.manifest, b.manifest)
      << "_manifest.txt must be byte-identical across config_json-only differences";

  std::error_code ec;
  fs::remove(panel_path, ec);
}

} // namespace atxtest_provenance_digest
