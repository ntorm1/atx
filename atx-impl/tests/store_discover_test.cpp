// atx::impl — resumable-discover store wiring tests (Task 7).
//
// Strategy (FULL gated run, with a split-style sink unit test for robustness):
//   1. FingerprintStableAndSensitive — pure unit test of compute_discover_fingerprint
//      (stable across calls; sensitive to seed; order-independent over seed_exprs).
//   2. SinkWritesCheckpointRows — drive a StoreProgressSink with synthetic
//      GenerationSnapshots against a REAL PipelineRecorder over an in-memory StoreDb,
//      asserting the checkpoint/iteration/event rows land. This isolates the sink from
//      the heavy search so a sink regression is caught fast and independently.
//   3. DiscoverOffPathByteIdentical — run the gated discover stage on the existing
//      tiny momentum fixture with NO --run-db, twice -> identical _manifest.txt bytes
//      AND identical stage digest. This is the load-bearing off-path invariant.
//   4. DiscoverWithRunDbWritesProgress — run the gated discover stage with a temp
//      --run-db on the tiny fixture; after it returns, reopen the db and assert one
//      'completed' pipeline_run, COUNT(pipeline_iteration) == generations,
//      COUNT(pipeline_checkpoint) >= 1, and 'started'+'completed' events present.
//   5. DiscoverWithRunDbOffPathByteIdentical — the gated stage's _manifest.txt + stage
//      digest are byte-for-byte identical WITH vs WITHOUT --run-db (the wiring is a
//      pure side-channel; it never perturbs the admitted set or the artifacts).
//
// A full gated discover IS feasible from a unit test (the existing discover_test.cpp
// drives run_discover gated on a 96x6 panel at population 16 / generations 5), so we
// use the full run; the in-memory sink test is kept as the fast, isolated regression.

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "atx/core/types.hpp"

#include "atx/engine/alpha/panel.hpp"
#include "atx/engine/factory/search_progress.hpp" // factory::GenerationSnapshot
#include "atx/engine/store/db.hpp"                 // store::StoreDb
#include "atx/engine/store/pipeline_progress.hpp"  // store::PipelineRecorder, PipelineRunRow

#include "config.hpp"
#include "serialize_panel.hpp"
#include "stages.hpp"
#include "store_progress_sink.hpp"

namespace atxtest_store_discover {

using atx::f64;
using atx::usize;
using atx::engine::alpha::Panel;

namespace store = atx::engine::store;

// ---------------------------------------------------------------------------
// Fixture: the same deterministic noisy-momentum panel as discover_test.cpp.
// ---------------------------------------------------------------------------
struct Lcg {
    std::uint64_t s;
    [[nodiscard]] f64 next() noexcept {
        s = s * 6364136223846793005ULL + 1442695040888963407ULL;
        const std::uint64_t hi = s >> 11U;
        const f64 u = static_cast<f64>(hi) / static_cast<f64>(1ULL << 53U);
        return 2.0 * u - 1.0;
    }
};

static std::vector<f64> noisy_close(usize dates, usize insts, std::uint64_t seed) {
    std::vector<f64> drift(insts);
    for (usize j = 0; j < insts; ++j) {
        drift[j] = 0.006 - 0.0024 * static_cast<f64>(j);
    }
    std::vector<f64> close(dates * insts);
    std::vector<f64> px(insts, 100.0);
    Lcg rng{seed};
    for (usize t = 0; t < dates; ++t) {
        for (usize j = 0; j < insts; ++j) {
            px[j] *= (1.0 + drift[j] + 0.010 * rng.next());
            close[t * insts + j] = px[j];
        }
    }
    return close;
}

static std::optional<Panel> make_momentum_panel(usize dates = 96, usize insts = 6) {
    const std::vector<f64> close = noisy_close(dates, insts, 0xBEEFCAFEULL);
    auto r = Panel::create(dates, insts, {"close"}, {close}, {});
    if (!r.has_value()) {
        ADD_FAILURE() << "panel fixture must build: " << r.error().to_string();
        return std::nullopt;
    }
    return std::move(r.value());
}

static std::vector<std::string> safe_seed_exprs() {
    return {"rank(close)", "ts_mean(close,10)", "delta(close,2)"};
}

static std::string write_panel_tmp(const Panel& panel, const std::string& stem) {
    namespace fs = std::filesystem;
    const std::string path =
        (fs::temp_directory_path() / ("atx_impl_storedisc_" + stem + ".bin")).string();
    auto r = atx::impl::write_panel(panel, path);
    EXPECT_TRUE(r.has_value()) << "write_panel must succeed";
    return path;
}

// A gated RunConfig with a permissive gate so the tiny fixture admits >= 1 alpha.
static atx::impl::RunConfig gated_cfg(const std::string& panel_path,
                                      const std::string& alpha_out) {
    atx::impl::RunConfig cfg;
    cfg.subcommand   = "discover";
    cfg.panel        = panel_path;
    cfg.alpha_out    = alpha_out;
    cfg.seed         = 777ULL;
    cfg.population   = 16;
    cfg.generations  = 5;
    cfg.seed_exprs   = safe_seed_exprs();
    cfg.gated        = true;
    cfg.min_sharpe   = 0.0;
    cfg.min_fitness  = 0.0;
    cfg.max_turnover = 10.0;
    cfg.max_pool_corr = 1.0;
    cfg.min_dsr      = 0.0;
    return cfg;
}

static std::string read_file(const std::string& path) {
    std::ifstream f{path, std::ios::binary};
    return std::string((std::istreambuf_iterator<char>(f)),
                       std::istreambuf_iterator<char>());
}

static atx::i64 count_rows(store::StoreDb& db, const std::string& sql) {
    auto stmt_r = db.db().prepare_cached(sql);
    EXPECT_TRUE(stmt_r.has_value()) << "prepare: " << sql;
    if (!stmt_r.has_value()) return -1;
    auto* stmt = *stmt_r;
    auto step = stmt->step();
    EXPECT_TRUE(step.has_value());
    if (!step.has_value() || *step != atx::core::db::Statement::Step::Row) return -1;
    return stmt->column_int(0);
}

// ---------------------------------------------------------------------------
// 1. FingerprintStableAndSensitive
// ---------------------------------------------------------------------------
TEST(AtxImplStoreDiscover, FingerprintStableAndSensitive) {
    atx::impl::RunConfig cfg = gated_cfg("panel.bin", "out");

    const atx::u64 a = atx::impl::compute_discover_fingerprint(cfg);
    const atx::u64 b = atx::impl::compute_discover_fingerprint(cfg);
    EXPECT_EQ(a, b) << "same config must give same fingerprint";
    EXPECT_NE(a, atx::u64{0}) << "fingerprint must be non-zero";

    // Bump the seed => different fingerprint.
    atx::impl::RunConfig cfg2 = cfg;
    cfg2.seed = cfg.seed + 1ULL;
    EXPECT_NE(atx::impl::compute_discover_fingerprint(cfg2), a)
        << "a different seed must change the fingerprint";

    // Reorder seed_exprs => SAME fingerprint (order-independent: sorted internally).
    atx::impl::RunConfig cfg3 = cfg;
    std::reverse(cfg3.seed_exprs.begin(), cfg3.seed_exprs.end());
    EXPECT_EQ(atx::impl::compute_discover_fingerprint(cfg3), a)
        << "seed_expr order must not change the fingerprint";

    // A different panel path => different fingerprint.
    atx::impl::RunConfig cfg4 = cfg;
    cfg4.panel = "other_panel.bin";
    EXPECT_NE(atx::impl::compute_discover_fingerprint(cfg4), a)
        << "a different panel must change the fingerprint";
}

// ---------------------------------------------------------------------------
// 2. SinkWritesCheckpointRows — isolated sink unit test over in-memory StoreDb.
// ---------------------------------------------------------------------------
TEST(AtxImplStoreDiscover, SinkWritesCheckpointRows) {
    auto db_r = store::StoreDb::open_memory();
    ASSERT_TRUE(db_r.has_value()) << db_r.error().message();
    store::StoreDb db = std::move(*db_r);

    store::PipelineRunRow row;
    row.pipeline_run_id   = "deadbeef00000001";
    row.fingerprint       = 0xDEADBEEF00000001ULL;
    row.stage             = "discover";
    row.master_seed       = 777ULL;
    row.population        = 4;
    row.total_generations = 3;
    row.panel_path        = "mem";
    row.created_at        = atx::impl::now_unix();
    auto rec_r = store::PipelineRecorder::begin(db.db(), row);
    ASSERT_TRUE(rec_r.has_value()) << rec_r.error().message();
    store::PipelineRecorder rec = std::move(*rec_r);

    atx::impl::StoreProgressSink sink{rec};
    for (atx::usize g = 0; g < 3; ++g) {
        atx::engine::factory::GenerationSnapshot snap;
        snap.generation   = g;
        snap.population    = {"rank(close)", "delta(close,2)"};
        snap.best_fitness  = 1.0 + static_cast<f64>(g);
        snap.mean_fitness  = 0.5;
        snap.n_evaluated   = 10 + g;
        snap.n_unique      = 2;
        auto st = sink.on_generation(snap);
        ASSERT_TRUE(st.has_value()) << st.error().message();
    }
    ASSERT_TRUE(rec.complete(atx::impl::now_unix()).has_value());

    EXPECT_EQ(count_rows(db, "SELECT COUNT(*) FROM pipeline_checkpoint"), 3);
    EXPECT_EQ(count_rows(db, "SELECT COUNT(*) FROM pipeline_iteration"), 3);
    EXPECT_EQ(count_rows(db,
                  "SELECT COUNT(*) FROM pipeline_run WHERE status='completed'"), 1);
    // latest blob round-trips through split_population.
    auto blob_r = rec.latest_population_blob();
    ASSERT_TRUE(blob_r.has_value()) << blob_r.error().message();
    auto pop = store::split_population(*blob_r);
    ASSERT_EQ(pop.size(), 2u);
    EXPECT_EQ(pop[0], "rank(close)");
    EXPECT_EQ(pop[1], "delta(close,2)");
}

// ---------------------------------------------------------------------------
// 3. DiscoverOffPathByteIdentical — empty run_db: two runs produce identical
//    _manifest.txt bytes AND identical stage digest (the load-bearing invariant).
// ---------------------------------------------------------------------------
TEST(AtxImplStoreDiscover, DiscoverOffPathByteIdentical) {
    namespace fs = std::filesystem;
    auto panel_opt = make_momentum_panel();
    ASSERT_TRUE(panel_opt.has_value());
    const std::string panel_path = write_panel_tmp(*panel_opt, "offpath");
    const std::string out1 = (fs::temp_directory_path() / "atx_storedisc_off1").string();
    const std::string out2 = (fs::temp_directory_path() / "atx_storedisc_off2").string();

    auto cfg1 = gated_cfg(panel_path, out1);  // run_db empty by default
    auto r1 = atx::impl::run_discover(cfg1);
    ASSERT_TRUE(r1.has_value()) << r1.error().message();

    auto cfg2 = gated_cfg(panel_path, out2);
    auto r2 = atx::impl::run_discover(cfg2);
    ASSERT_TRUE(r2.has_value()) << r2.error().message();

    EXPECT_EQ(r1->digest, r2->digest) << "off-path stage digest must be stable";
    const std::string m1 = read_file((fs::path{out1} / "_manifest.txt").string());
    const std::string m2 = read_file((fs::path{out2} / "_manifest.txt").string());
    EXPECT_FALSE(m1.empty());
    EXPECT_EQ(m1, m2) << "off-path _manifest.txt bytes must be identical";

    std::error_code ec;
    fs::remove(panel_path, ec);
    fs::remove_all(out1, ec);
    fs::remove_all(out2, ec);
}

// ---------------------------------------------------------------------------
// 4. DiscoverWithRunDbWritesProgress — gated run with --run-db persists progress.
// ---------------------------------------------------------------------------
TEST(AtxImplStoreDiscover, DiscoverWithRunDbWritesProgress) {
    namespace fs = std::filesystem;
    auto panel_opt = make_momentum_panel();
    ASSERT_TRUE(panel_opt.has_value());
    const std::string panel_path = write_panel_tmp(*panel_opt, "rundb");
    const std::string out = (fs::temp_directory_path() / "atx_storedisc_rundb_out").string();
    const std::string db_path =
        (fs::temp_directory_path() / "atx_storedisc_progress.db").string();

    std::error_code ec0;
    fs::remove(db_path, ec0);  // fresh db

    auto cfg = gated_cfg(panel_path, out);
    cfg.run_db = db_path;
    auto r = atx::impl::run_discover(cfg);
    ASSERT_TRUE(r.has_value()) << r.error().message();

    // Reopen the db (read-back); StoreDb::open is idempotent on the schema.
    auto db_r = store::StoreDb::open(db_path);
    ASSERT_TRUE(db_r.has_value()) << db_r.error().message();
    store::StoreDb db = std::move(*db_r);

    EXPECT_EQ(count_rows(db,
                  "SELECT COUNT(*) FROM pipeline_run WHERE status='completed'"), 1)
        << "exactly one completed pipeline_run";
    EXPECT_EQ(count_rows(db, "SELECT COUNT(*) FROM pipeline_iteration"),
              static_cast<atx::i64>(cfg.generations))
        << "one iteration row per generation";
    EXPECT_GE(count_rows(db, "SELECT COUNT(*) FROM pipeline_checkpoint"), 1)
        << "at least one checkpoint";
    EXPECT_EQ(count_rows(db,
                  "SELECT COUNT(*) FROM pipeline_event WHERE event_type='started'"), 1)
        << "a 'started' event";
    EXPECT_EQ(count_rows(db,
                  "SELECT COUNT(*) FROM pipeline_event WHERE event_type='completed'"), 1)
        << "a 'completed' event";

    std::error_code ec;
    fs::remove(panel_path, ec);
    fs::remove_all(out, ec);
    fs::remove(db_path, ec);
}

// ---------------------------------------------------------------------------
// 5. DiscoverWithRunDbOffPathByteIdentical — the artifacts (manifest + stage
//    digest) are identical WITH vs WITHOUT --run-db: the store wiring is a pure
//    side-channel and never perturbs the admitted set or the emitted files.
// ---------------------------------------------------------------------------
TEST(AtxImplStoreDiscover, DiscoverWithRunDbOffPathByteIdentical) {
    namespace fs = std::filesystem;
    auto panel_opt = make_momentum_panel();
    ASSERT_TRUE(panel_opt.has_value());
    const std::string panel_path = write_panel_tmp(*panel_opt, "parity");
    const std::string out_off = (fs::temp_directory_path() / "atx_storedisc_par_off").string();
    const std::string out_on  = (fs::temp_directory_path() / "atx_storedisc_par_on").string();
    const std::string db_path =
        (fs::temp_directory_path() / "atx_storedisc_parity.db").string();

    std::error_code ec0;
    fs::remove(db_path, ec0);

    auto cfg_off = gated_cfg(panel_path, out_off);  // no run_db
    auto r_off = atx::impl::run_discover(cfg_off);
    ASSERT_TRUE(r_off.has_value()) << r_off.error().message();

    auto cfg_on = gated_cfg(panel_path, out_on);
    cfg_on.run_db = db_path;  // store wiring active
    auto r_on = atx::impl::run_discover(cfg_on);
    ASSERT_TRUE(r_on.has_value()) << r_on.error().message();

    EXPECT_EQ(r_off->digest, r_on->digest)
        << "stage digest must be identical with vs without --run-db";
    const std::string m_off = read_file((fs::path{out_off} / "_manifest.txt").string());
    const std::string m_on  = read_file((fs::path{out_on} / "_manifest.txt").string());
    EXPECT_FALSE(m_off.empty());
    EXPECT_EQ(m_off, m_on)
        << "_manifest.txt bytes must be identical with vs without --run-db";

    std::error_code ec;
    fs::remove(panel_path, ec);
    fs::remove_all(out_off, ec);
    fs::remove_all(out_on, ec);
    fs::remove(db_path, ec);
}

}  // namespace atxtest_store_discover
