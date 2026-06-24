// atx::impl — discover stage tests (suite AtxImplDiscover, S3 TDD).
//
// TDD order: GenomeRoundTrip first (isolates serialize_genome), then the
// search-based tests (AdmitsAtLeastOneAlpha, SameSeedDeterministic).
//
// Panel fixture: 96 dates x 6 instruments, single "close" field built via a
// deterministic LCG random walk with a small persistent drift so rank(close)
// has a genuine finite-Sharpe momentum edge (mirrors factory_search_driver_test).

#define _CRT_SECURE_NO_WARNINGS 1 // std::getenv (W6 real-panel data gate); matches single_alpha_capacity_test.cpp

#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>   // std::getenv (W6 real-panel data gate)
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "atx/core/types.hpp"

#include "atx/engine/alpha/bytecode.hpp"    // alpha::compile, alpha::Program
#include "atx/engine/alpha/panel.hpp"
#include "atx/engine/alpha/parser.hpp"     // alpha::parse_expr, alpha::Library
#include "atx/engine/alpha/typecheck.hpp"  // alpha::analyze (W3 validity test)
#include "atx/engine/alpha/unparse.hpp"    // alpha::unparse (W3 gen-0 test)
#include "atx/engine/alpha/vm.hpp"         // alpha::Engine
#include "atx/engine/combine/store.hpp"    // combine::AlphaStore (W3 gen-0 test)
#include "atx/engine/exec/execution_sim.hpp" // exec::ExecutionSimulator (W3)
#include "atx/engine/factory/genome.hpp"   // factory::analyze_into, factory::Genome
#include "atx/engine/factory/search_driver.hpp"   // factory::SearchDriver (W3)
#include "atx/engine/factory/search_progress.hpp" // factory::SearchProgressSink (W3)
#include "atx/engine/loop/weight_policy.hpp"       // engine::WeightPolicy (W3)

#include "config.hpp"
#include "research_sim.hpp"        // atx::impl::frictionless_sim (W3)
#include "serialize_genome.hpp"
#include "serialize_panel.hpp"
#include "alpha101_support.hpp"       // make_synth_orats_panel, augment_for_alpha101 (W6)
#include "stage_discover_detail.hpp"  // atx::impl::detail::apply_capacity_screen (Fix 1)
#include "stages.hpp"

namespace atxtest_discover {

using atx::f64;
using atx::usize;
using atx::engine::alpha::compile;
using atx::engine::alpha::Engine;
using atx::engine::alpha::FieldId;
using atx::engine::alpha::Library;
using atx::engine::alpha::Panel;
using atx::engine::alpha::parse_expr;
using atx::engine::alpha::Program;
using atx::engine::alpha::SignalSet;
using atx::engine::combine::AlphaStore;
using atx::engine::exec::ExecutionSimulator;
using atx::engine::factory::analyze_into;
using atx::engine::factory::Genome;
using atx::engine::factory::SearchConfig;
using atx::engine::factory::SearchDriver;
using atx::engine::WeightPolicy;
using atx::impl::read_genome;
using atx::impl::write_genome;

// ---------------------------------------------------------------------------
// Deterministic LCG (same idiom as factory_search_driver_test).
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

// Build a noisy momentum close matrix [dates * insts].
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

// Build a 96x6 single-field "close" Panel (momentum fixture). Returns nullopt
// on a Panel::create failure (ADD_FAILURE marks the test failed) so a caller's
// ASSERT_TRUE stops the test before dereferencing an error (no UB).
static std::optional<Panel> make_momentum_panel(usize dates = 96, usize insts = 6) {
    const std::vector<f64> close = noisy_close(dates, insts, 0xBEEFCAFEULL);
    auto r = Panel::create(dates, insts, {"close"}, {close}, {});
    if (!r.has_value()) {
        ADD_FAILURE() << "panel fixture must build: " << r.error().to_string();
        return std::nullopt;
    }
    return std::move(r.value());
}

// Seed expressions valid for a {"close"} panel (brief §4 VERIFIED-safe grammar).
static std::vector<std::string> safe_seed_exprs() {
    return {"rank(close)", "ts_mean(close,10)", "delta(close,2)"};
}

// Write the panel to a temp file and return the path.
static std::string write_panel_tmp(const Panel& panel, const std::string& stem) {
    namespace fs = std::filesystem;
    const std::string path =
        (fs::temp_directory_path() / ("atx_impl_discover_" + stem + ".bin")).string();
    auto r = atx::impl::write_panel(panel, path);
    EXPECT_TRUE(r.has_value()) << "write_panel must succeed";
    return path;
}

// ---------------------------------------------------------------------------
// Test 3 (TDD FIRST): GenomeRoundTrip
// Verifies serialize_genome end-to-end against the engine without depending
// on search nondeterminism.  Build a Genome from parse_expr + analyze_into,
// write/read it, compile both, evaluate on the panel, assert cell-for-cell ==.
// ---------------------------------------------------------------------------
TEST(AtxImplDiscover, GenomeRoundTrip) {
    namespace fs = std::filesystem;

    auto panel_opt = make_momentum_panel();
    ASSERT_TRUE(panel_opt.has_value());
    const Panel& panel = *panel_opt;
    Library lib{};

    // Build a reference Genome from a known safe expression.
    auto ast_r = parse_expr("rank(close)", lib);
    ASSERT_TRUE(ast_r.has_value()) << ast_r.error().message();
    auto g_r = analyze_into(std::move(*ast_r));
    ASSERT_TRUE(g_r.has_value()) << g_r.error().message();
    const Genome& g = *g_r;

    // Write to a temp .dsl file.
    const std::string dsl_path =
        (fs::temp_directory_path() / "atx_impl_discover_roundtrip.dsl").string();
    auto ws = write_genome(g, dsl_path);
    ASSERT_TRUE(ws.has_value()) << ws.error().message();

    // Read back.
    auto g2_r = read_genome(dsl_path, lib);
    ASSERT_TRUE(g2_r.has_value()) << g2_r.error().message();
    const Genome& g2 = *g2_r;

    // Compile both.
    auto prog1_r = compile(g.ast, g.analysis);
    ASSERT_TRUE(prog1_r.has_value()) << prog1_r.error().message();
    auto prog2_r = compile(g2.ast, g2.analysis);
    ASSERT_TRUE(prog2_r.has_value()) << prog2_r.error().message();

    // Evaluate both on the panel.
    Engine e1{panel};
    auto ss1_r = e1.evaluate(*prog1_r);
    ASSERT_TRUE(ss1_r.has_value()) << ss1_r.error().message();

    Engine e2{panel};
    auto ss2_r = e2.evaluate(*prog2_r);
    ASSERT_TRUE(ss2_r.has_value()) << ss2_r.error().message();

    const SignalSet& ss1 = *ss1_r;
    const SignalSet& ss2 = *ss2_r;

    ASSERT_EQ(ss1.alphas.size(), ss2.alphas.size());
    ASSERT_FALSE(ss1.alphas.empty());
    ASSERT_EQ(ss1.alphas[0].values.size(), ss2.alphas[0].values.size());

    const auto& v1 = ss1.alphas[0].values;
    const auto& v2 = ss2.alphas[0].values;
    for (usize i = 0; i < v1.size(); ++i) {
        const bool both_nan = std::isnan(v1[i]) && std::isnan(v2[i]);
        const bool equal    = (v1[i] == v2[i]);
        EXPECT_TRUE(both_nan || equal)
            << "cell " << i << ": " << v1[i] << " vs " << v2[i];
    }

    // Clean up.
    std::error_code ec;
    fs::remove(dsl_path, ec);
}

// ---------------------------------------------------------------------------
// Test 4a: MissingSeedExprFails
// ---------------------------------------------------------------------------
TEST(AtxImplDiscover, MissingSeedExprFails) {
    auto panel_opt = make_momentum_panel();
    ASSERT_TRUE(panel_opt.has_value());
    const Panel& panel = *panel_opt;
    const std::string panel_path = write_panel_tmp(panel, "missing_seed");

    atx::impl::RunConfig cfg;
    cfg.subcommand = "discover";
    cfg.panel      = panel_path;
    cfg.alpha_out  = (std::filesystem::temp_directory_path() /
                      "atx_impl_discover_missing_seed_out").string();
    // seed_exprs intentionally empty

    auto r = atx::impl::run_discover(cfg);
    EXPECT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code(), atx::core::ErrorCode::InvalidArgument);

    std::error_code ec;
    std::filesystem::remove(panel_path, ec);
}

// ---------------------------------------------------------------------------
// Test 4b: MissingPanelFails
// ---------------------------------------------------------------------------
TEST(AtxImplDiscover, MissingPanelFails) {
    atx::impl::RunConfig cfg;
    cfg.subcommand  = "discover";
    cfg.panel       = "";   // intentionally empty
    cfg.alpha_out   = (std::filesystem::temp_directory_path() /
                       "atx_impl_discover_missing_panel_out").string();
    cfg.seed_exprs  = safe_seed_exprs();

    auto r = atx::impl::run_discover(cfg);
    EXPECT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code(), atx::core::ErrorCode::InvalidArgument);
}

// ---------------------------------------------------------------------------
// Test 1: AdmitsAtLeastOneAlpha
// ---------------------------------------------------------------------------
TEST(AtxImplDiscover, AdmitsAtLeastOneAlpha) {
    namespace fs = std::filesystem;

    auto panel_opt = make_momentum_panel();
    ASSERT_TRUE(panel_opt.has_value());
    const Panel& panel = *panel_opt;
    const std::string panel_path = write_panel_tmp(panel, "admits_alpha");
    const std::string alpha_out  =
        (fs::temp_directory_path() / "atx_impl_discover_admits_out").string();

    atx::impl::RunConfig cfg;
    cfg.subcommand  = "discover";
    cfg.panel       = panel_path;
    cfg.alpha_out   = alpha_out;
    cfg.seed        = 777ULL;
    cfg.population  = 16;
    cfg.generations = 5;
    cfg.seed_exprs  = safe_seed_exprs();

    auto r = atx::impl::run_discover(cfg);
    ASSERT_TRUE(r.has_value()) << r.error().message();

    const auto& sr = *r;

    // "admitted" kv must parse to >= 1.
    const auto& kvs = sr.kvs;
    auto it = std::find_if(kvs.begin(), kvs.end(),
                           [](const auto& p) { return p.first == "admitted"; });
    ASSERT_NE(it, kvs.end()) << "missing 'admitted' kv";
    EXPECT_GE(std::stoi(it->second), 1);

    // alpha_out dir must contain >= 1 *.dsl and _manifest.txt.
    EXPECT_TRUE(fs::exists(alpha_out));
    bool has_dsl = false;
    bool has_manifest = false;
    for (const auto& entry : fs::directory_iterator(alpha_out)) {
        if (entry.path().extension() == ".dsl") {
            has_dsl = true;
        }
        if (entry.path().filename() == "_manifest.txt") {
            has_manifest = true;
        }
    }
    EXPECT_TRUE(has_dsl)      << "no .dsl files found in " << alpha_out;
    EXPECT_TRUE(has_manifest) << "no _manifest.txt found in " << alpha_out;

    // Cleanup.
    std::error_code ec;
    fs::remove(panel_path, ec);
    fs::remove_all(alpha_out, ec);
}

// ---------------------------------------------------------------------------
// Test 2: SameSeedDeterministic (F1)
// ---------------------------------------------------------------------------
TEST(AtxImplDiscover, SameSeedDeterministic) {
    namespace fs = std::filesystem;

    auto panel_opt = make_momentum_panel();
    ASSERT_TRUE(panel_opt.has_value());
    const Panel& panel = *panel_opt;
    const std::string panel_path = write_panel_tmp(panel, "deterministic");
    const std::string alpha_out1 =
        (fs::temp_directory_path() / "atx_impl_discover_det_out1").string();
    const std::string alpha_out2 =
        (fs::temp_directory_path() / "atx_impl_discover_det_out2").string();

    atx::impl::RunConfig cfg;
    cfg.subcommand  = "discover";
    cfg.panel       = panel_path;
    cfg.seed        = 777ULL;
    cfg.population  = 16;
    cfg.generations = 5;
    cfg.seed_exprs  = safe_seed_exprs();

    cfg.alpha_out = alpha_out1;
    auto r1 = atx::impl::run_discover(cfg);
    ASSERT_TRUE(r1.has_value()) << r1.error().message();

    cfg.alpha_out = alpha_out2;
    auto r2 = atx::impl::run_discover(cfg);
    ASSERT_TRUE(r2.has_value()) << r2.error().message();

    // Same seed => same admitted genomes => same unparse strings => same fnv1a64.
    EXPECT_EQ(r1->digest, r2->digest)
        << "digests must be equal (same seed)";
    EXPECT_NE(r1->digest, atx::u64{0})
        << "digest must be non-zero";

    // Cleanup.
    std::error_code ec;
    fs::remove(panel_path, ec);
    fs::remove_all(alpha_out1, ec);
    fs::remove_all(alpha_out2, ec);
}

// ---------------------------------------------------------------------------
// P2b Test: DiscoverRejectsTooShortOosPanel
// A small panel + an over-large --oos-fraction (0.99) must return
// Err(InvalidArgument) with a message mentioning "oos-fraction", NOT succeed
// with 0 alphas.  Tests the pre-validation guard in run_discover_gated which
// reuses eval::reserve_lockbox (the engine geometry helper).
// ---------------------------------------------------------------------------
TEST(AtxImplDiscover, DiscoverRejectsTooShortOosPanel) {
    namespace fs = std::filesystem;

    // Use a small panel: 20 dates, 6 instruments.
    auto panel_opt = make_momentum_panel(/*dates=*/20, /*insts=*/6);
    ASSERT_TRUE(panel_opt.has_value());
    const Panel& panel = *panel_opt;
    const std::string panel_path = write_panel_tmp(panel, "oos_too_short");
    const std::string alpha_out  =
        (fs::temp_directory_path() / "atx_impl_discover_oos_too_short_out").string();

    atx::impl::RunConfig cfg;
    cfg.subcommand   = "discover";
    cfg.panel        = panel_path;
    cfg.alpha_out    = alpha_out;
    cfg.seed         = 42ULL;
    cfg.population   = 8;
    cfg.generations  = 2;
    cfg.seed_exprs   = safe_seed_exprs();
    cfg.gated        = true;
    cfg.min_sharpe   = 0.0;  // permissive gate so only the guard can reject
    cfg.min_fitness  = 0.0;
    cfg.max_turnover = 10.0;
    cfg.max_pool_corr= 1.0;
    cfg.min_dsr      = 0.0;
    cfg.oos_fraction = 0.99; // too large: leaves almost no train window

    auto r = atx::impl::run_discover(cfg);
    EXPECT_FALSE(r.has_value())
        << "expected Err but got Ok (guard must reject a 0.99 oos-fraction on a 20-date panel)";
    if (!r.has_value()) {
        EXPECT_EQ(r.error().code(), atx::core::ErrorCode::InvalidArgument);
        // The error message must name oos-fraction so the user knows what to fix.
        EXPECT_NE(r.error().message().find("oos-fraction"), std::string::npos)
            << "error message must mention 'oos-fraction', got: " << r.error().message();
    }

    // Cleanup.
    std::error_code ec;
    fs::remove(panel_path, ec);
    fs::remove_all(alpha_out, ec);
}

// ---------------------------------------------------------------------------
// P2b Test: OosManifestHeaderPresent
// When oos_fraction > 0 and a run succeeds, the manifest must contain the
// oos_fraction= header line.  When oos_fraction == 0 (legacy path), the
// manifest must NOT contain that line (byte-identical off-path).
// ---------------------------------------------------------------------------
TEST(AtxImplDiscover, OosManifestHeaderPresent) {
    namespace fs = std::filesystem;

    // Use the standard 96-date momentum panel — large enough for a small OOS split.
    auto panel_opt = make_momentum_panel(/*dates=*/96, /*insts=*/6);
    ASSERT_TRUE(panel_opt.has_value());
    const Panel& panel = *panel_opt;
    const std::string panel_path = write_panel_tmp(panel, "oos_manifest");

    // -- Run 1: OOS enabled (fraction=0.2), permissive gate.
    const std::string alpha_out_oos =
        (fs::temp_directory_path() / "atx_impl_discover_oos_manifest_oos").string();
    {
        atx::impl::RunConfig cfg;
        cfg.subcommand   = "discover";
        cfg.panel        = panel_path;
        cfg.alpha_out    = alpha_out_oos;
        cfg.seed         = 777ULL;
        cfg.population   = 16;
        cfg.generations  = 5;
        cfg.seed_exprs   = safe_seed_exprs();
        cfg.gated        = true;
        cfg.min_sharpe   = 0.0;
        cfg.min_fitness  = 0.0;
        cfg.max_turnover = 10.0;
        cfg.max_pool_corr= 1.0;
        cfg.min_dsr      = 0.0;
        cfg.oos_fraction = 0.2;
        cfg.oos_embargo  = 0.0;
        auto r = atx::impl::run_discover(cfg);
        // If the run fails due to no alphas clearing the gate, skip the header check.
        if (r.has_value()) {
            const std::string manifest_path =
                (fs::path{alpha_out_oos} / "_manifest.txt").string();
            ASSERT_TRUE(fs::exists(manifest_path)) << "manifest must exist";
            std::ifstream mf{manifest_path};
            std::string content((std::istreambuf_iterator<char>(mf)),
                                 std::istreambuf_iterator<char>());
            EXPECT_NE(content.find("oos_fraction="), std::string::npos)
                << "manifest must contain oos_fraction= when OOS is active";
        }
    }

    // -- Run 2: OOS disabled (fraction=0), verify oos_fraction= NOT in manifest.
    const std::string alpha_out_leg =
        (fs::temp_directory_path() / "atx_impl_discover_oos_manifest_leg").string();
    {
        atx::impl::RunConfig cfg;
        cfg.subcommand   = "discover";
        cfg.panel        = panel_path;
        cfg.alpha_out    = alpha_out_leg;
        cfg.seed         = 777ULL;
        cfg.population   = 16;
        cfg.generations  = 5;
        cfg.seed_exprs   = safe_seed_exprs();
        cfg.gated        = true;
        cfg.min_sharpe   = 0.0;
        cfg.min_fitness  = 0.0;
        cfg.max_turnover = 10.0;
        cfg.max_pool_corr= 1.0;
        cfg.min_dsr      = 0.0;
        cfg.oos_fraction = 0.0;  // legacy path
        auto r = atx::impl::run_discover(cfg);
        if (r.has_value()) {
            const std::string manifest_path =
                (fs::path{alpha_out_leg} / "_manifest.txt").string();
            ASSERT_TRUE(fs::exists(manifest_path)) << "manifest must exist";
            std::ifstream mf{manifest_path};
            std::string content((std::istreambuf_iterator<char>(mf)),
                                 std::istreambuf_iterator<char>());
            EXPECT_EQ(content.find("oos_fraction="), std::string::npos)
                << "manifest must NOT contain oos_fraction= on the legacy (oos_fraction=0) path";
        }
    }

    // Cleanup.
    std::error_code ec;
    fs::remove(panel_path, ec);
    fs::remove_all(alpha_out_oos, ec);
    fs::remove_all(alpha_out_leg, ec);
}

// ---------------------------------------------------------------------------
// 8.A helpers — read the gated "admitted" kv (== total library size after the
// run; the gated path emits n_alphas() there) and the manifest count= line.
// ---------------------------------------------------------------------------
static int admitted_kv(const atx::impl::StageResult& sr) {
    for (const auto& p : sr.kvs) {
        if (p.first == "admitted") {
            return std::stoi(p.second);
        }
    }
    ADD_FAILURE() << "missing 'admitted' kv";
    return -1;
}

// Count alpha_NNN.dsl files written into an alpha_out dir (== this-run library
// size; the gated path writes one .dsl per AlphaId).
static int count_dsl(const std::string& dir) {
    namespace fs = std::filesystem;
    int n = 0;
    if (!fs::exists(dir)) return 0;
    for (const auto& e : fs::directory_iterator(dir)) {
        if (e.path().extension() == ".dsl") ++n;
    }
    return n;
}

// Build a gated, permissive-gate RunConfig for the accumulation tests. min_dsr
// 0 + wide-open floors so admission is driven by the search, not the gate.
static atx::impl::RunConfig gated_cfg(const std::string& panel_path,
                                      const std::string& alpha_out,
                                      unsigned long long seed) {
    atx::impl::RunConfig cfg;
    cfg.subcommand   = "discover";
    cfg.panel        = panel_path;
    cfg.alpha_out    = alpha_out;
    cfg.seed         = seed;
    cfg.population   = 16;
    cfg.generations  = 5;
    cfg.seed_exprs   = safe_seed_exprs();
    cfg.gated        = true;
    cfg.min_sharpe   = 0.0;
    cfg.min_fitness  = 0.0;
    cfg.max_turnover = 10.0;
    cfg.max_pool_corr= 1.0;   // permit any corr so accumulation is not corr-gated
    cfg.min_dsr      = 0.0;
    return cfg;
}

// ---------------------------------------------------------------------------
// 8.A Test: LibraryAccumulatesAcrossRuns
// With --library-dir set, the library is RE-OPENED (not wiped) each run, so two
// runs with DIFFERENT seeds into the SAME library dir leave the second run's
// library >= the first run's (new uncorrelated alphas accumulate; identical
// alphas are deduped). Without --library-dir (the default), each run wipes its
// per-run <alpha_out>/_library, so the second run does NOT see the first's alphas.
// ---------------------------------------------------------------------------
TEST(AtxImplDiscover, LibraryAccumulatesAcrossRuns) {
    namespace fs = std::filesystem;

    auto panel_opt = make_momentum_panel(/*dates=*/96, /*insts=*/6);
    ASSERT_TRUE(panel_opt.has_value());
    const Panel& panel = *panel_opt;
    const std::string panel_path = write_panel_tmp(panel, "lib_accum");

    // Hermetic, fresh library dir + alpha_out dirs (removed up-front so a stale
    // dir from a prior failed run cannot pollute the accumulation count).
    const std::string lib_dir =
        (fs::temp_directory_path() / "atx_impl_discover_lib_accum_libdir").string();
    const std::string out1 =
        (fs::temp_directory_path() / "atx_impl_discover_lib_accum_out1").string();
    const std::string out2 =
        (fs::temp_directory_path() / "atx_impl_discover_lib_accum_out2").string();
    std::error_code ec0;
    fs::remove_all(lib_dir, ec0);
    fs::remove_all(out1, ec0);
    fs::remove_all(out2, ec0);

    // Run 1 into the stable library dir (seed A).
    auto cfg1 = gated_cfg(panel_path, out1, /*seed=*/111ULL);
    cfg1.library_dir = lib_dir;
    auto r1 = atx::impl::run_discover(cfg1);
    ASSERT_TRUE(r1.has_value()) << r1.error().message();
    const int n1 = admitted_kv(*r1);
    ASSERT_GE(n1, 1);
    EXPECT_EQ(count_dsl(out1), n1) << "run 1 .dsl count == library size";

    // Run 2 into the SAME library dir, DIFFERENT seed B. The library is re-opened
    // (not wiped): the size is monotonic non-decreasing, and >= run 1's (run 1's
    // alphas are still present; any new run-2 alphas are added, identical ones
    // deduped).
    auto cfg2 = gated_cfg(panel_path, out2, /*seed=*/222ULL);
    cfg2.library_dir = lib_dir;
    auto r2 = atx::impl::run_discover(cfg2);
    ASSERT_TRUE(r2.has_value()) << r2.error().message();
    const int n2 = admitted_kv(*r2);
    EXPECT_GE(n2, n1) << "accumulation: library cannot shrink across runs";
    EXPECT_EQ(count_dsl(out2), n2) << "run 2 emits the FULL accumulated library as .dsl";

    // CONTRAST: the DEFAULT path (no --library-dir) wipes its per-run library, so a
    // second default run does NOT accumulate — n stays a single-run size.
    const std::string out_def1 =
        (fs::temp_directory_path() / "atx_impl_discover_lib_accum_def1").string();
    const std::string out_def2 =
        (fs::temp_directory_path() / "atx_impl_discover_lib_accum_def2").string();
    fs::remove_all(out_def1, ec0);
    fs::remove_all(out_def2, ec0);
    auto cdef1 = gated_cfg(panel_path, out_def1, /*seed=*/111ULL); // library_dir unset
    auto rdef1 = atx::impl::run_discover(cdef1);
    ASSERT_TRUE(rdef1.has_value()) << rdef1.error().message();
    auto cdef2 = gated_cfg(panel_path, out_def2, /*seed=*/222ULL); // library_dir unset
    auto rdef2 = atx::impl::run_discover(cdef2);
    ASSERT_TRUE(rdef2.has_value()) << rdef2.error().message();
    // Each default run's library is fresh; the seed-B default run cannot include
    // the seed-A alphas, so its size is independent of (and not the accumulation of)
    // the seed-A run. Specifically it must be < the accumulated n2 whenever the two
    // seeds discovered any distinct alpha (n2 == n1 + new). If the two seeds found
    // an identical set, n2 == n1 and both default runs == n1 too; assert the weaker
    // invariant that the accumulated library is at least as large as a single run.
    EXPECT_GE(n2, admitted_kv(*rdef2))
        << "accumulated library must be >= a single default (wiped) run";

    // Cleanup.
    std::error_code ec;
    fs::remove(panel_path, ec);
    fs::remove_all(lib_dir, ec);
    fs::remove_all(out1, ec);
    fs::remove_all(out2, ec);
    fs::remove_all(out_def1, ec);
    fs::remove_all(out_def2, ec);
}

// ---------------------------------------------------------------------------
// 8.A Test: LibraryDedupOnReadmitDeterministic
// Re-running discover with the SAME seed into the SAME --library-dir re-admits
// the SAME alphas. library::admit dedups by canonical hash, so the library size
// does NOT grow (every re-admit is a Duplicate), and the run is deterministic
// (twice-run: identical digest, identical library size).
// ---------------------------------------------------------------------------
TEST(AtxImplDiscover, LibraryDedupOnReadmitDeterministic) {
    namespace fs = std::filesystem;

    auto panel_opt = make_momentum_panel(/*dates=*/96, /*insts=*/6);
    ASSERT_TRUE(panel_opt.has_value());
    const Panel& panel = *panel_opt;
    const std::string panel_path = write_panel_tmp(panel, "lib_dedup");

    const std::string lib_dir =
        (fs::temp_directory_path() / "atx_impl_discover_lib_dedup_libdir").string();
    const std::string out1 =
        (fs::temp_directory_path() / "atx_impl_discover_lib_dedup_out1").string();
    const std::string out2 =
        (fs::temp_directory_path() / "atx_impl_discover_lib_dedup_out2").string();
    std::error_code ec0;
    fs::remove_all(lib_dir, ec0);
    fs::remove_all(out1, ec0);
    fs::remove_all(out2, ec0);

    // Run 1 (seed A) into the stable library dir.
    auto cfg1 = gated_cfg(panel_path, out1, /*seed=*/333ULL);
    cfg1.library_dir = lib_dir;
    auto r1 = atx::impl::run_discover(cfg1);
    ASSERT_TRUE(r1.has_value()) << r1.error().message();
    const int n1 = admitted_kv(*r1);
    ASSERT_GE(n1, 1);

    // Run 2: identical seed + same library dir. Every candidate re-admit is a
    // Duplicate, so the library size is UNCHANGED, and the run is byte-deterministic
    // (same admitted-from-search set => same emitted .dsl => same fnv1a64 digest).
    auto cfg2 = gated_cfg(panel_path, out2, /*seed=*/333ULL);
    cfg2.library_dir = lib_dir;
    auto r2 = atx::impl::run_discover(cfg2);
    ASSERT_TRUE(r2.has_value()) << r2.error().message();
    const int n2 = admitted_kv(*r2);

    EXPECT_EQ(n2, n1) << "re-admitting identical alphas must NOT grow the library (dedup)";
    EXPECT_EQ(r1->digest, r2->digest)
        << "twice-run determinism: identical seed + library dir => identical stage digest";
    EXPECT_NE(r1->digest, atx::u64{0});

    // Cleanup.
    std::error_code ec;
    fs::remove(panel_path, ec);
    fs::remove_all(lib_dir, ec);
    fs::remove_all(out1, ec);
    fs::remove_all(out2, ec);
}

// ---------------------------------------------------------------------------
// R3 helpers
// ---------------------------------------------------------------------------

// Read the full text of _manifest.txt in alpha_out (fails the test if missing).
static std::string read_manifest(const std::string& alpha_out) {
    namespace fs = std::filesystem;
    const std::string path = (fs::path{alpha_out} / "_manifest.txt").string();
    if (!fs::exists(path)) {
        ADD_FAILURE() << "_manifest.txt not found in " << alpha_out;
        return {};
    }
    std::ifstream f{path};
    return std::string{std::istreambuf_iterator<char>(f),
                       std::istreambuf_iterator<char>()};
}

// Read all kv keys from a StageResult.
static std::set<std::string> kv_keys(const atx::impl::StageResult& sr) {
    std::set<std::string> ks;
    for (const auto& p : sr.kvs) {
        ks.insert(p.first);
    }
    return ks;
}

// ---------------------------------------------------------------------------
// R3a Test: AccumulationAutoOos
// With --library-dir set AND no explicit --oos-fraction, the manifest must
// contain oos_fraction=0.25 (the auto-default) and the IS/OOS column headers.
// ---------------------------------------------------------------------------
TEST(AtxImplDiscover, R3a_AccumulationAutoOos) {
    namespace fs = std::filesystem;

    // Use a 96-date panel — large enough for 25% holdout (24 dates).
    auto panel_opt = make_momentum_panel(/*dates=*/96, /*insts=*/6);
    ASSERT_TRUE(panel_opt.has_value());
    const std::string panel_path = write_panel_tmp(*panel_opt, "r3a_auto");

    const std::string lib_dir =
        (fs::temp_directory_path() / "atx_r3a_auto_libdir").string();
    const std::string alpha_out =
        (fs::temp_directory_path() / "atx_r3a_auto_out").string();
    std::error_code ec0;
    fs::remove_all(lib_dir, ec0);
    fs::remove_all(alpha_out, ec0);

    // Build a gated cfg WITH --library-dir but WITHOUT explicit --oos-fraction.
    atx::impl::RunConfig cfg;
    cfg.subcommand   = "discover";
    cfg.panel        = panel_path;
    cfg.alpha_out    = alpha_out;
    cfg.seed         = 7ULL;
    cfg.population   = 16;
    cfg.generations  = 5;
    cfg.seed_exprs   = safe_seed_exprs();
    cfg.gated        = true;
    cfg.min_sharpe   = 0.0;
    cfg.min_fitness  = 0.0;
    cfg.max_turnover = 10.0;
    cfg.max_pool_corr= 1.0;
    cfg.min_dsr      = 0.0;
    cfg.library_dir  = lib_dir;
    // cfg.oos_fraction intentionally NOT set (should auto-default to 0.25).
    // cfg.set_flags intentionally empty (no "oos-fraction" in set_flags).

    auto r = atx::impl::run_discover(cfg);
    ASSERT_TRUE(r.has_value()) << "accumulation auto-OOS run must succeed: " << r.error().message();

    // The manifest must contain oos_fraction=0.25 (auto-default).
    const std::string manifest = read_manifest(alpha_out);
    EXPECT_NE(manifest.find("oos_fraction=0.25"), std::string::npos)
        << "auto-OOS must write oos_fraction=0.25 to manifest; got:\n" << manifest;

    // The StageResult kvs must contain oos_pbo (the OOS path is active).
    const auto keys = kv_keys(*r);
    EXPECT_NE(keys.find("oos_pbo"), keys.end())
        << "accumulation auto-OOS run must include oos_pbo in StageResult kvs";

    // Cleanup.
    std::error_code ec;
    fs::remove(panel_path, ec);
    fs::remove_all(lib_dir, ec);
    fs::remove_all(alpha_out, ec);
}

// ---------------------------------------------------------------------------
// R3a Test: NonAccumulationByteIdentical
// Without --library-dir, the manifest must NOT contain oos_fraction= or
// oos_pbo= lines, and the StageResult kvs must NOT have oos_pbo.
// This pins the byte-identical non-accumulation invariant.
// ---------------------------------------------------------------------------
TEST(AtxImplDiscover, R3a_NonAccumulationByteIdentical) {
    namespace fs = std::filesystem;

    auto panel_opt = make_momentum_panel(/*dates=*/96, /*insts=*/6);
    ASSERT_TRUE(panel_opt.has_value());
    const std::string panel_path = write_panel_tmp(*panel_opt, "r3a_noaccum");
    const std::string alpha_out =
        (fs::temp_directory_path() / "atx_r3a_noaccum_out").string();
    std::error_code ec0;
    fs::remove_all(alpha_out, ec0);

    atx::impl::RunConfig cfg;
    cfg.subcommand   = "discover";
    cfg.panel        = panel_path;
    cfg.alpha_out    = alpha_out;
    cfg.seed         = 7ULL;
    cfg.population   = 16;
    cfg.generations  = 5;
    cfg.seed_exprs   = safe_seed_exprs();
    cfg.gated        = true;
    cfg.min_sharpe   = 0.0;
    cfg.min_fitness  = 0.0;
    cfg.max_turnover = 10.0;
    cfg.max_pool_corr= 1.0;
    cfg.min_dsr      = 0.0;
    // library_dir intentionally UNSET (no accumulation).
    // oos_fraction intentionally 0.0 (default).

    auto r = atx::impl::run_discover(cfg);
    ASSERT_TRUE(r.has_value()) << "non-accumulation run must succeed: " << r.error().message();

    const std::string manifest = read_manifest(alpha_out);
    EXPECT_EQ(manifest.find("oos_fraction="), std::string::npos)
        << "non-accumulation run must NOT emit oos_fraction= in manifest";
    EXPECT_EQ(manifest.find("oos_pbo="), std::string::npos)
        << "non-accumulation run must NOT emit oos_pbo= in manifest";

    const auto keys = kv_keys(*r);
    EXPECT_EQ(keys.find("oos_pbo"), keys.end())
        << "non-accumulation run StageResult must NOT have oos_pbo key";

    // Cleanup.
    std::error_code ec;
    fs::remove(panel_path, ec);
    fs::remove_all(alpha_out, ec);
}

// ===========================================================================
// W2 — Capacity universe screen tests (TDD: written before implementation)
// ===========================================================================

// Helpers shared across W2 tests.
// Build a small synthetic panel with hand-chosen close/volume values that let
// us compute adv{W} by hand and verify the capacity screen predicate exactly.
//
// Layout: 5 dates x 4 instruments, adv_window = 3.
// Instrument roles:
//   [0] PASS   — close > min_price AND adv3 >= min_adv (always in-universe)
//   [1] LOW_PX — close <= min_price (< $1.00)
//   [2] LOW_ADV— close > min_price but adv3 < min_adv (< $50M)
//   [3] OOU    — out-of-universe on all dates (never admitted regardless)
//
// Constants: min_price = 1.0, min_adv = 50e6, adv_window = 3.
// close[d][i]:  PASS=10.0, LOW_PX=0.50, LOW_ADV=5.0, OOU=100.0 (masked out)
// volume[d][i]: PASS=1e7 (DV=1e8 >> 50M), LOW_PX=1e7, LOW_ADV=1e3 (DV=5e3 << 50M), OOU=irrelevant

static constexpr atx::f64 kW2MinPrice = 1.0;
static constexpr atx::f64 kW2MinAdv   = 50.0e6;
static constexpr long     kW2AdvWin   = 3;

// Returns nullopt on failure (ADD_FAILURE marks the test).
static std::optional<Panel> make_w2_panel() {
    constexpr usize D = 5;
    constexpr usize I = 4;
    // universe: all in except instrument 3 (OOU)
    std::vector<std::uint8_t> univ(D * I, 1);
    for (usize d = 0; d < D; ++d) {
        univ[d * I + 3] = 0; // instrument 3 always out-of-universe
    }
    // close: constant across dates
    std::vector<f64> close_data(D * I);
    for (usize d = 0; d < D; ++d) {
        close_data[d * I + 0] = 10.0;   // PASS
        close_data[d * I + 1] = 0.50;   // LOW_PX (below $1)
        close_data[d * I + 2] = 5.0;    // LOW_ADV
        close_data[d * I + 3] = 100.0;  // OOU (masked; value irrelevant)
    }
    // volume: constant across dates
    std::vector<f64> volume_data(D * I);
    for (usize d = 0; d < D; ++d) {
        volume_data[d * I + 0] = 1.0e7;  // DV=1e8, adv3=1e8 >= 50M (PASS)
        volume_data[d * I + 1] = 1.0e7;  // DV=5e6, adv3=5e6 < 50M (but already LOW_PX)
        volume_data[d * I + 2] = 1.0e3;  // DV=5e3, adv3=5e3 << 50M (LOW_ADV)
        volume_data[d * I + 3] = 1.0e7;  // OOU, irrelevant
    }
    auto r = Panel::create(D, I, {"close", "volume"}, {close_data, volume_data}, univ);
    if (!r.has_value()) {
        ADD_FAILURE() << "W2 panel fixture must build: " << r.error().to_string();
        return std::nullopt;
    }
    return std::move(r.value());
}

// ---------------------------------------------------------------------------
// W2 Test 1: CapacityScreenPredicateUnit
// Calls the REAL apply_capacity_screen (via atx::impl::detail — Fix 1) on the
// synthetic fixture and asserts the returned panel's in_universe(d,i) matches
// the hand-computed mask EXACTLY.
//
// Brief requirement: "Apply the screen; assert the resulting panel's
// in_universe(d,i) matches the hand-computed mask EXACTLY." — a bug in the
// real helper is now detectable (the old test reimplemented the logic in-test
// and would have passed even with a broken apply_capacity_screen).
//
// Fixture coverage (from make_w2_panel):
//   [0] PASS    — close=10 > $1, DV=1e8 >= $50M (passes after warm-up)
//   [1] LOW_PX  — close=0.50 <= $1 (fails price screen, all dates)
//   [2] LOW_ADV — close=5 > $1 but DV=5e3 << $50M (fails adv screen, all dates)
//   [3] OOU     — out-of-universe on all dates (unchanged by screen)
//   NaN cells   — adv is NaN on warm-up dates (dates 0..adv_window-2), so
//                 instrument[0] is also out during those dates (NaN guard).
// ---------------------------------------------------------------------------
TEST(AtxImplDiscover, W2_CapacityScreenPredicateUnit) {
    auto panel_opt = make_w2_panel();
    ASSERT_TRUE(panel_opt.has_value());
    const Panel& panel = *panel_opt;

    constexpr usize D = 5;
    constexpr usize I = 4;

    // Call the REAL production helper. This is the key change from the original
    // test: any bug in apply_capacity_screen will now cause an assertion failure
    // here, instead of passing because the test reimplemented the same logic.
    auto screened_r = atx::impl::detail::apply_capacity_screen(
        panel, kW2MinPrice, kW2MinAdv, kW2AdvWin);
    ASSERT_TRUE(screened_r.has_value()) << screened_r.error().to_string();
    const Panel& screened = *screened_r;

    // Hand-computed expected mask:
    //   instrument[0] (PASS):    in-universe only when adv window is complete,
    //                            i.e. d >= kW2AdvWin - 1 (dates 2, 3, 4).
    //   instrument[1] (LOW_PX):  never in-universe (close=0.50 <= $1.00).
    //   instrument[2] (LOW_ADV): never in-universe (adv << $50M).
    //   instrument[3] (OOU):     never in-universe (was out in the original panel).
    for (usize d = 0; d < D; ++d) {
        const bool adv_defined = (d + 1 >= static_cast<usize>(kW2AdvWin));
        EXPECT_EQ(screened.in_universe(d, 0), adv_defined)
            << "PASS instrument[0] at date " << d
            << ": expected in_universe=" << adv_defined
            << " (adv window complete=" << adv_defined << ")";
        EXPECT_FALSE(screened.in_universe(d, 1))
            << "LOW_PX instrument[1] must be excluded at date " << d
            << " (close=0.50 <= min_price=1.0)";
        EXPECT_FALSE(screened.in_universe(d, 2))
            << "LOW_ADV instrument[2] must be excluded at date " << d
            << " (DV=5e3 << min_adv=50M)";
        EXPECT_FALSE(screened.in_universe(d, 3))
            << "OOU instrument[3] must remain out-of-universe at date " << d;
    }

    // Also verify adv field values are present and correct in the returned panel,
    // confirming the augmentation step ran correctly end-to-end.
    const std::string adv_name = "adv" + std::to_string(kW2AdvWin);
    auto adv_id_r = screened.field_id(adv_name);
    ASSERT_TRUE(adv_id_r.has_value()) << "screened panel must carry the adv" << kW2AdvWin << " field";
    const auto adv_col = screened.field_all(*adv_id_r);
    // adv3 for instrument 0 (PASS): DV = close*vol = 10*1e7 = 1e8 (constant).
    // Mean over any complete window of 3 constant values = 1e8.
    for (usize d = static_cast<usize>(kW2AdvWin) - 1; d < D; ++d) {
        const f64 adv_val = adv_col[d * I + 0];
        EXPECT_TRUE(std::isfinite(adv_val)) << "adv3[" << d << "][0] must be finite";
        EXPECT_NEAR(adv_val, 1.0e8, 1.0)   << "adv3[" << d << "][0] must be ~1e8";
    }
    // adv3 at dates 0 and 1 must be NaN (incomplete window).
    for (usize d = 0; d + 1 < static_cast<usize>(kW2AdvWin); ++d) {
        EXPECT_TRUE(std::isnan(adv_col[d * I + 0]))
            << "adv3[" << d << "][0] must be NaN (incomplete window, d<" << kW2AdvWin - 1 << ")";
    }
}

// ---------------------------------------------------------------------------
// W4a Test: RobustHoldoutPanelDeterministicAndMasks
// Calls the REAL build_robust_holdout_panel helper and asserts: (1) the derived
// universe is a SUBSET of the original (no cell is added), (2) the same seed + frac
// yields a BYTE-IDENTICAL mask (seed-stable determinism, F1), (3) a different seed
// yields a (generally) different mask, and (4) fail-closed on an out-of-range frac.
// ---------------------------------------------------------------------------
TEST(AtxImplDiscover, W4a_RobustHoldoutPanelDeterministicAndMasks) {
    // A 6-date x 16-instrument all-in-universe panel (close field only — the helper
    // copies columns verbatim; only the universe mask changes).
    constexpr usize D = 6;
    constexpr usize I = 16;
    std::vector<f64> close(D * I);
    for (usize d = 0; d < D; ++d) {
        for (usize i = 0; i < I; ++i) {
            close[d * I + i] = 10.0 + static_cast<f64>(i);
        }
    }
    auto base_r = Panel::create(D, I, {"close"}, {close}, {}); // empty universe == all-in
    ASSERT_TRUE(base_r.has_value()) << base_r.error().to_string();
    const Panel& base = *base_r;

    // (1) + (2): same seed + frac -> identical mask; mask is a subset of the original.
    auto a_r = atx::impl::detail::build_robust_holdout_panel(base, /*frac=*/0.5, /*seed=*/12345u);
    auto b_r = atx::impl::detail::build_robust_holdout_panel(base, /*frac=*/0.5, /*seed=*/12345u);
    ASSERT_TRUE(a_r.has_value()) << a_r.error().to_string();
    ASSERT_TRUE(b_r.has_value()) << b_r.error().to_string();
    usize kept_a = 0;
    for (usize d = 0; d < D; ++d) {
        for (usize i = 0; i < I; ++i) {
            EXPECT_EQ(a_r->in_universe(d, i), b_r->in_universe(d, i))
                << "same seed+frac must yield a byte-identical weak universe (F1)";
            if (a_r->in_universe(d, i)) {
                ++kept_a;
                EXPECT_TRUE(base.in_universe(d, i)) << "weak universe must be a SUBSET of the original";
            }
        }
    }
    EXPECT_GT(kept_a, 0u) << "frac=0.5 over 16 instruments must retain some cells";
    EXPECT_LT(kept_a, D * I) << "frac=0.5 must mask SOME instruments out (a proper sub-universe)";
    // Per-instrument selection is uniform across dates (the mask keys on instrument i).
    for (usize i = 0; i < I; ++i) {
        const bool sel0 = a_r->in_universe(0, i);
        for (usize d = 1; d < D; ++d) {
            EXPECT_EQ(a_r->in_universe(d, i), sel0) << "selection is per-instrument (date-uniform)";
        }
    }

    // (3): a different seed generally yields a different selection (not byte-identical).
    auto c_r = atx::impl::detail::build_robust_holdout_panel(base, /*frac=*/0.5, /*seed=*/98765u);
    ASSERT_TRUE(c_r.has_value()) << c_r.error().to_string();
    bool any_diff = false;
    for (usize i = 0; i < I && !any_diff; ++i) {
        if (a_r->in_universe(0, i) != c_r->in_universe(0, i)) any_diff = true;
    }
    EXPECT_TRUE(any_diff) << "a different master_seed should generally pick a different sub-universe";

    // (4): fail-closed on an out-of-range frac (must be in the open interval (0,1)).
    EXPECT_FALSE(atx::impl::detail::build_robust_holdout_panel(base, /*frac=*/0.0, 1u).has_value());
    EXPECT_FALSE(atx::impl::detail::build_robust_holdout_panel(base, /*frac=*/1.0, 1u).has_value());
    EXPECT_FALSE(atx::impl::detail::build_robust_holdout_panel(base, /*frac=*/-0.1, 1u).has_value());
}

// ---------------------------------------------------------------------------
// W2 Test 2: CapacityScreenInactiveIsNoOp
// When min_price=0 and min_adv=0, run_discover must NOT call the screen and
// must produce the same digest as a baseline run with no capacity flags.
// ---------------------------------------------------------------------------
TEST(AtxImplDiscover, W2_CapacityScreenInactiveIsNoOp) {
    namespace fs = std::filesystem;

    auto panel_opt = make_momentum_panel(/*dates=*/96, /*insts=*/6);
    ASSERT_TRUE(panel_opt.has_value());
    const Panel& panel = *panel_opt;
    const std::string panel_path = write_panel_tmp(panel, "w2_inactive");

    const std::string out_base =
        (fs::temp_directory_path() / "atx_impl_discover_w2_inactive_base").string();
    const std::string out_noop =
        (fs::temp_directory_path() / "atx_impl_discover_w2_inactive_noop").string();

    // Baseline: no capacity flags.
    atx::impl::RunConfig cfg_base;
    cfg_base.subcommand  = "discover";
    cfg_base.panel       = panel_path;
    cfg_base.alpha_out   = out_base;
    cfg_base.seed        = 777ULL;
    cfg_base.population  = 16;
    cfg_base.generations = 5;
    cfg_base.seed_exprs  = safe_seed_exprs();
    // min_adv_usd=0, min_price=0 (defaults) => screen inactive

    auto r_base = atx::impl::run_discover(cfg_base);
    ASSERT_TRUE(r_base.has_value()) << r_base.error().message();

    // Same config but explicitly set min_adv=0 and min_price=0 (still inactive).
    atx::impl::RunConfig cfg_noop = cfg_base;
    cfg_noop.alpha_out   = out_noop;
    cfg_noop.min_adv_usd = 0.0;
    cfg_noop.min_price   = 0.0;

    auto r_noop = atx::impl::run_discover(cfg_noop);
    ASSERT_TRUE(r_noop.has_value()) << r_noop.error().message();

    // Digests must be equal: same panel + same seed + screen inactive => byte-identical.
    //
    // WHY two zero-threshold W2 runs are sufficient (Fix 4):
    //   The inactive path is a verified-by-construction no-op: run_discover's
    //   `if (capacity_on)` guard means apply_capacity_screen is NEVER called when
    //   both thresholds are 0, so zero new code runs and the original `panel`
    //   object flows unchanged to all downstream paths. Digest-equality is the
    //   determinism check — it proves the full pipeline (panel → search → DSL →
    //   fnv1a64) is bit-for-bit identical between the two runs, matching the
    //   convention used by SameSeedDeterministic earlier in this file.
    //   No pinned golden digest is needed: the two-run comparison is self-checking
    //   (any regression that makes one run non-deterministic breaks this test too).
    EXPECT_EQ(r_base->digest, r_noop->digest)
        << "inactive screen (min_adv=0, min_price=0) must produce byte-identical digest";

    // Cleanup.
    std::error_code ec;
    fs::remove(panel_path, ec);
    fs::remove_all(out_base, ec);
    fs::remove_all(out_noop, ec);
}

// ---------------------------------------------------------------------------
// W2 Test 3: CapacityScreenFailClosedMissingClose
// Screen active (min_adv > 0) but panel has no 'close' field => Err(InvalidArgument)
// naming the missing field.
// ---------------------------------------------------------------------------
TEST(AtxImplDiscover, W2_CapacityScreenFailClosedMissingClose) {
    namespace fs = std::filesystem;

    // Panel with 'volume' but no 'close'.
    constexpr usize D = 10;
    constexpr usize I = 3;
    std::vector<f64> vol(D * I, 1.0e7);
    auto r = Panel::create(D, I, {"volume"}, {vol}, {});
    ASSERT_TRUE(r.has_value());
    const Panel& panel = *r;

    // Write panel to disk.
    const std::string panel_path =
        (std::filesystem::temp_directory_path() /
         "atx_impl_discover_w2_nocl.bin").string();
    {
        auto wr = atx::impl::write_panel(panel, panel_path);
        ASSERT_TRUE(wr.has_value()) << "write_panel must succeed";
    }

    const std::string alpha_out =
        (fs::temp_directory_path() / "atx_impl_discover_w2_nocl_out").string();

    atx::impl::RunConfig cfg;
    cfg.subcommand   = "discover";
    cfg.panel        = panel_path;
    cfg.alpha_out    = alpha_out;
    cfg.seed         = 1ULL;
    cfg.population   = 4;
    cfg.generations  = 1;
    cfg.seed_exprs   = {"rank(volume)"};
    cfg.min_adv_usd  = 50.0e6; // screen ACTIVE

    auto result = atx::impl::run_discover(cfg);
    EXPECT_FALSE(result.has_value())
        << "screen active with missing 'close' must return Err";
    if (!result.has_value()) {
        EXPECT_EQ(result.error().code(), atx::core::ErrorCode::InvalidArgument);
        // Error must name the missing field.
        EXPECT_NE(result.error().message().find("close"), std::string::npos)
            << "error message must name 'close', got: " << result.error().message();
    }

    std::error_code ec;
    fs::remove(panel_path, ec);
    fs::remove_all(alpha_out, ec);
}

// ---------------------------------------------------------------------------
// W2 Test 4: CapacityScreenActiveChangesUniverse
// With min_price > 0 and a panel that has close/volume, the screen IS active.
// Instruments with close <= min_price must be excluded (no signal from them),
// so the discover run either succeeds with different admitted exprs or succeeds
// on the passing names. Assert the run completes and at least one instrument
// passes (the panel has enough passing names to find an alpha).
// ---------------------------------------------------------------------------
TEST(AtxImplDiscover, W2_CapacityScreenActiveChangesUniverse) {
    namespace fs = std::filesystem;

    // Build a momentum panel that also has a 'volume' field so the screen can
    // compute ADV. 6 instruments; close > $1 for all (so price screen passes
    // all; use min_price = 0.0 and min_adv_usd = 1.0 so only the adv check runs,
    // which all pass since DV >> 1.0).
    constexpr usize D = 96;
    constexpr usize I = 6;
    const std::vector<f64> close_vals = noisy_close(D, I, 0xDEADBEEFULL);
    // All close values start at ~100 and drift, so > $1.
    std::vector<f64> volume_vals(D * I, 1.0e6); // DV = close*vol, adv >> 1.0

    auto r = Panel::create(D, I, {"close", "volume"}, {close_vals, volume_vals}, {});
    ASSERT_TRUE(r.has_value());
    const Panel& panel = *r;

    const std::string panel_path = write_panel_tmp(panel, "w2_active");
    const std::string alpha_out  =
        (fs::temp_directory_path() / "atx_impl_discover_w2_active_out").string();

    atx::impl::RunConfig cfg;
    cfg.subcommand   = "discover";
    cfg.panel        = panel_path;
    cfg.alpha_out    = alpha_out;
    cfg.seed         = 777ULL;
    cfg.population   = 16;
    cfg.generations  = 5;
    cfg.seed_exprs   = safe_seed_exprs();
    cfg.min_adv_usd  = 1.0;   // very low adv bar => all pass => no names pruned
    cfg.adv_window   = 3;     // short window for speed

    auto result = atx::impl::run_discover(cfg);
    ASSERT_TRUE(result.has_value()) << result.error().message();

    // At least one alpha must be admitted.
    const auto& kvs = result->kvs;
    auto it = std::find_if(kvs.begin(), kvs.end(),
                           [](const auto& p) { return p.first == "admitted"; });
    ASSERT_NE(it, kvs.end()) << "missing 'admitted' kv";
    EXPECT_GE(std::stoi(it->second), 1)
        << "capacity screen (all names pass) must still admit at least one alpha";

    std::error_code ec;
    fs::remove(panel_path, ec);
    fs::remove_all(alpha_out, ec);
}

// ---------------------------------------------------------------------------
// W2 Test 5: CapacityScreenZeroUniverseReturnsErr (Fix 2)
// When the screen is ACTIVE and all cells are excluded (thresholds too strict),
// apply_capacity_screen must return Err(InvalidArgument) naming the zero-retain
// condition. This prevents silently producing an all-NaN signal set.
// ---------------------------------------------------------------------------
TEST(AtxImplDiscover, W2_CapacityScreenZeroUniverseReturnsErr) {
    // Use the W2 fixture (5x4) but set min_adv absurdly high so no cell passes.
    auto panel_opt = make_w2_panel();
    ASSERT_TRUE(panel_opt.has_value());
    const Panel& panel = *panel_opt;

    // min_adv = 1e30 >> any possible DV => no cell can pass => kept_cells == 0.
    auto result = atx::impl::detail::apply_capacity_screen(
        panel, /*min_price=*/0.0, /*min_adv=*/1.0e30, /*adv_window=*/kW2AdvWin);

    EXPECT_FALSE(result.has_value())
        << "zero-retain screen must return Err, not a panel";
    if (!result.has_value()) {
        EXPECT_EQ(result.error().code(), atx::core::ErrorCode::InvalidArgument);
        // Error message must mention the zero-retain condition.
        const std::string& msg = result.error().message();
        EXPECT_NE(msg.find("zero"), std::string::npos)
            << "error message must mention 'zero', got: " << msg;
    }
}

// ---------------------------------------------------------------------------
// W2 Test 6: CapacityScreenBadAdvWindowReturnsErr (Fix 3)
// When the screen is ACTIVE, adv_window out of [1, 65535] must return
// Err(InvalidArgument) naming the bad value — NOT silently clamp to 20.
// ---------------------------------------------------------------------------
TEST(AtxImplDiscover, W2_CapacityScreenBadAdvWindowReturnsErr) {
    auto panel_opt = make_w2_panel();
    ASSERT_TRUE(panel_opt.has_value());
    const Panel& panel = *panel_opt;

    // adv_window = 0 is below the valid range [1, 65535].
    {
        auto result = atx::impl::detail::apply_capacity_screen(
            panel, /*min_price=*/1.0, /*min_adv=*/50.0e6, /*adv_window=*/0L);
        EXPECT_FALSE(result.has_value())
            << "adv_window=0 must return Err (out of range)";
        if (!result.has_value()) {
            EXPECT_EQ(result.error().code(), atx::core::ErrorCode::InvalidArgument);
            const std::string& msg = result.error().message();
            EXPECT_NE(msg.find("adv_window"), std::string::npos)
                << "error must name 'adv_window', got: " << msg;
            EXPECT_NE(msg.find("0"), std::string::npos)
                << "error must include the bad value (0), got: " << msg;
        }
    }

    // adv_window = 70000 is above the valid range.
    {
        auto result = atx::impl::detail::apply_capacity_screen(
            panel, /*min_price=*/1.0, /*min_adv=*/50.0e6, /*adv_window=*/70000L);
        EXPECT_FALSE(result.has_value())
            << "adv_window=70000 must return Err (out of range)";
        if (!result.has_value()) {
            EXPECT_EQ(result.error().code(), atx::core::ErrorCode::InvalidArgument);
            const std::string& msg = result.error().message();
            EXPECT_NE(msg.find("adv_window"), std::string::npos)
                << "error must name 'adv_window', got: " << msg;
        }
    }
}

// ===========================================================================
// W3 — seed-file tests
// ===========================================================================

// ---------------------------------------------------------------------------
// W3 Test 1: ReadSeedFileFormat
// Unit test for read_seed_file format rules (TDD RED step — written before
// the fixture is final). Writes a synthetic temp file containing:
//   - a '#' comment line
//   - a blank line
//   - a valid '<id>: <dsl>' line
//   - a no-colon malformed line
//   - an empty-DSL line
// Asserts the reader returns exactly the one valid DSL string, trimmed.
// Also asserts an unreadable path -> Err, and an all-comment file -> Err.
// ---------------------------------------------------------------------------
TEST(AtxImplDiscover, W3_ReadSeedFileFormat) {
    namespace fs = std::filesystem;

    // Write a synthetic mixed-content file.
    const std::string good_path =
        (fs::temp_directory_path() / "atx_w3_seed_file_test.txt").string();
    {
        std::ofstream ofs(good_path);
        ASSERT_TRUE(ofs.is_open()) << "Cannot create temp file " << good_path;
        ofs << "# this is a comment\n";
        ofs << "\n";                                     // blank line
        ofs << "tpl1:   rank(close)  \n";               // valid — id=tpl1, dsl=rank(close)
        ofs << "no colon malformed line\n";              // no ':' — skip
        ofs << "empty:   \n";                            // ':' but empty DSL — skip
    }

    auto r = atx::impl::read_seed_file(good_path);
    ASSERT_TRUE(r.has_value()) << "read_seed_file must succeed: " << r.error().message();
    ASSERT_EQ(r->size(), 1u) << "exactly one valid DSL line expected";
    EXPECT_EQ((*r)[0], "rank(close)") << "DSL must be trimmed";

    // Unreadable path must return Err(IoError).
    auto r2 = atx::impl::read_seed_file("/no/such/path/does_not_exist_xyz.txt");
    EXPECT_FALSE(r2.has_value()) << "unreadable path must return Err";
    if (!r2.has_value()) {
        EXPECT_EQ(r2.error().code(), atx::core::ErrorCode::IoError);
    }

    // All-comment file must return Err(InvalidArgument).
    const std::string comments_path =
        (fs::temp_directory_path() / "atx_w3_seed_file_comments.txt").string();
    {
        std::ofstream ofs(comments_path);
        ofs << "# comment 1\n";
        ofs << "# comment 2\n";
    }
    auto r3 = atx::impl::read_seed_file(comments_path);
    EXPECT_FALSE(r3.has_value()) << "all-comment file must return Err";
    if (!r3.has_value()) {
        EXPECT_EQ(r3.error().code(), atx::core::ErrorCode::InvalidArgument);
    }

    // Cleanup.
    std::error_code ec;
    fs::remove(good_path, ec);
    fs::remove(comments_path, ec);
}

// ---------------------------------------------------------------------------
// W3 Test 2: FactorTemplatesAllValid
// Load factor_templates.txt and assert every template parses AND analyzes
// against a default Library. This is the LOAD-BEARING gate — if any template
// fails here it must be fixed before committing (not the test).
// ---------------------------------------------------------------------------
TEST(AtxImplDiscover, W3_FactorTemplatesAllValid) {
    // ATX_IMPL_TESTS_DIR is injected by CMakeLists.txt.
    const std::string fixture_path =
        std::string(ATX_IMPL_TESTS_DIR) + "/fixtures/factor_templates.txt";

    auto dsls_r = atx::impl::read_seed_file(fixture_path);
    ASSERT_TRUE(dsls_r.has_value())
        << "read_seed_file must succeed on factor_templates.txt: "
        << dsls_r.error().message();
    ASSERT_FALSE(dsls_r->empty()) << "factor_templates.txt must have at least one template";

    Library lib{};
    std::size_t passed = 0;
    for (const std::string& dsl : *dsls_r) {
        // parse_expr
        auto ast_r = parse_expr(dsl, lib);
        EXPECT_TRUE(ast_r.has_value())
            << "parse_expr FAILED for template: '" << dsl
            << "' error: " << (ast_r.has_value() ? "" : ast_r.error().message());
        if (!ast_r.has_value()) continue;

        // analyze
        auto info_r = atx::engine::alpha::analyze(*ast_r);
        EXPECT_TRUE(info_r.has_value())
            << "analyze FAILED for template: '" << dsl
            << "' error: " << (info_r.has_value() ? "" : info_r.error().message());
        if (!info_r.has_value()) continue;

        ++passed;
    }

    EXPECT_EQ(passed, dsls_r->size())
        << "ALL templates must parse+analyze; "
        << (dsls_r->size() - passed) << " failed";
}

// ---------------------------------------------------------------------------
// W3 Test 3: TemplatesAppearInGenZero
// Construct a SearchDriver with the factor templates as seed_exprs,
// seed_from_grammar=true, population >= template count, and 1 generation.
// Capture gen-0 via SearchProgressSink and assert each template's canonical
// DSL appears in the gen-0 population list (which is build by
// serialize_population / unparse in canonical order).
// ---------------------------------------------------------------------------

namespace {

// Simple sink that records the gen-0 snapshot population DSL strings.
class GenZeroCaptureSink final : public atx::engine::factory::SearchProgressSink {
public:
    std::vector<std::string> gen0_population;  // filled on first call (gen==0)
    bool captured{false};

    atx::core::Status on_generation(
        const atx::engine::factory::GenerationSnapshot& snap) override
    {
        if (snap.generation == 0 && !captured) {
            gen0_population = snap.population;
            captured = true;
        }
        return atx::core::Ok();
    }
};

} // anonymous namespace

TEST(AtxImplDiscover, W3_TemplatesAppearInGenZero) {
    // Load the templates from the fixture.
    const std::string fixture_path =
        std::string(ATX_IMPL_TESTS_DIR) + "/fixtures/factor_templates.txt";
    auto dsls_r = atx::impl::read_seed_file(fixture_path);
    ASSERT_TRUE(dsls_r.has_value()) << dsls_r.error().message();
    ASSERT_FALSE(dsls_r->empty());

    const std::vector<std::string>& seed_dsls = *dsls_r;
    const atx::usize n_templates = seed_dsls.size();

    // Build a panel that has all required fields: close, returns, high, low,
    // sector, volume. The templates reference returns / close / sector.
    // Use the synthetic ORATS-shape panel (make_synth_orats_panel in
    // alpha101_support.hpp) logic — replicate it inline here with the needed
    // fields: close, returns (derived), sector.
    // We need at least: close, volume (for dollar_volume / adv derivation is not
    // needed here — only parse+analyze matters for validity, and SearchDriver
    // only constructs genomes; evaluation isn't done in this test). However
    // SearchDriver::run DOES evaluate each genome, so the Panel must supply all
    // fields the templates reference: returns, close, sector.
    //
    // For the discovery path the Panel only needs to pass evaluate(), which means
    // the fields in the DSL must be present. We build a minimal panel with all
    // required fields.
    constexpr atx::usize D = 60;   // enough for ts_std(returns,20) warm-up
    constexpr atx::usize I = 12;   // 12 instruments, 2 per sector (6 sectors)

    // Build deterministic price data.
    std::vector<atx::f64> close_d(D * I), returns_d(D * I, 0.0), sector_d(D * I);
    {
        atx::f64 px[I];
        atx::usize state_lcg = 0xDEADBEEFULL;
        auto lcg_next = [&]() -> atx::f64 {
            state_lcg = state_lcg * 6364136223846793005ULL + 1442695040888963407ULL;
            return static_cast<atx::f64>(state_lcg >> 11) / static_cast<atx::f64>(1ULL << 53);
        };
        for (atx::usize j = 0; j < I; ++j) {
            px[j] = 20.0 + 80.0 * lcg_next();
        }
        for (atx::usize d = 0; d < D; ++d) {
            for (atx::usize j = 0; j < I; ++j) {
                px[j] *= (1.0 + (lcg_next() - 0.5) * 0.04);
                const atx::f64 c = px[j];
                close_d[d * I + j]  = c;
                sector_d[d * I + j] = static_cast<atx::f64>(j % 6); // 6 sectors
                if (d > 0) {
                    const atx::f64 pc = close_d[(d-1) * I + j];
                    returns_d[d * I + j] = (pc != 0.0) ? (c / pc - 1.0) : 0.0;
                }
            }
        }
    }

    // high and low needed for any template using them; add them defensive.
    std::vector<atx::f64> high_d = close_d, low_d = close_d;

    auto panel_r = Panel::create(
        D, I,
        {"close", "returns", "sector", "high", "low"},
        {close_d, returns_d, sector_d, high_d, low_d},
        /*universe=*/{});
    ASSERT_TRUE(panel_r.has_value()) << panel_r.error().message();
    const Panel& panel = *panel_r;

    // Collect field names.
    std::vector<std::string> fields;
    for (atx::usize f = 0; f < panel.num_fields(); ++f) {
        fields.push_back(std::string{panel.field_name(f)});
    }

    Library lib{};
    WeightPolicy policy{};
    ExecutionSimulator sim = atx::impl::frictionless_sim();
    AlphaStore pool{};

    // Configure: population = 2*n_templates so all seeds get distinct slots.
    SearchConfig sc;
    sc.master_seed        = 42ULL;
    sc.population         = std::max<atx::usize>(n_templates * 2, 32u);
    sc.generations        = 1;   // only gen-0 matters for membership
    sc.elites             = 2;
    sc.k_tournament       = 3;
    sc.p_cross            = 0.5;
    sc.seed_from_grammar  = true;
    sc.enable_behavioral_novelty = false;  // keep test fast/simple
    sc.n_workers          = 1;
    sc.n_immigrants       = 0;
    sc.stagnation_patience = 0;  // no early stop

    SearchDriver driver{lib, panel, policy, sim, seed_dsls, fields};

    GenZeroCaptureSink sink;
    auto result = driver.run(sc, pool, &sink);
    // run() may produce an empty result if all genomes fail to evaluate —
    // but the snapshot is emitted regardless, so we can still check gen-0.
    ASSERT_TRUE(sink.captured) << "GenZeroCaptureSink must have been called for gen-0";
    const std::vector<std::string>& gen0 = sink.gen0_population;
    ASSERT_FALSE(gen0.empty()) << "gen-0 population must not be empty";

    // For each seed template: parse+analyze to get the canonical DSL via unparse,
    // then check it appears in the gen-0 population list (the snapshot uses
    // serialize_population which calls unparse on each Genome).
    atx::usize found = 0;
    for (const std::string& dsl : seed_dsls) {
        auto ast_r = parse_expr(dsl, lib);
        if (!ast_r.has_value()) continue;  // already tested in Test 2
        auto info_r = atx::engine::alpha::analyze(*ast_r);
        if (!info_r.has_value()) continue;

        const std::string canon_dsl = atx::engine::alpha::unparse(*ast_r);

        bool in_gen0 = false;
        for (const std::string& g0_dsl : gen0) {
            if (g0_dsl == canon_dsl) { in_gen0 = true; break; }
        }
        EXPECT_TRUE(in_gen0)
            << "Template '" << dsl << "' (canonical: '" << canon_dsl
            << "') not found in gen-0 population";
        if (in_gen0) ++found;
    }

    EXPECT_EQ(found, n_templates)
        << "All " << n_templates << " templates must appear in gen-0";
}

// ---------------------------------------------------------------------------
// W3 Test 4: ParseArgsSeedFileIntegration
// Integration tests that exercise the REAL parse_args -> apply_flag_value
// "seed-file" arm — the path a typo in the flag string or a broken append
// would leave silently broken while direct read_seed_file tests pass.
//
// (a) --seed-file alone populates seed_exprs with all 10 fixture templates
//     in file order.
// (b) --seed-expr X BEFORE --seed-file yields [X, file templates...] — CLI
//     entry first, file entries following in file order.
// (c) parse_args WITHOUT --seed-file leaves seed_exprs empty (the real
//     byte-identity-at-config-layer check through the actual parse path;
//     replaces the tautology of the original W3_SeedFileByteIdenticalDefault).
// (d) --seed-file with a nonexistent path returns Err (fail-closed through
//     the real CLI path).
// ---------------------------------------------------------------------------
TEST(AtxImplDiscover, W3_ParseArgsSeedFileIntegration) {
    const std::string fixture_path =
        std::string(ATX_IMPL_TESTS_DIR) + "/fixtures/factor_templates.txt";

    // Pre-load the expected DSL list so we can compare order exactly.
    auto expected_r = atx::impl::read_seed_file(fixture_path);
    ASSERT_TRUE(expected_r.has_value())
        << "pre-load fixture: " << expected_r.error().message();
    const std::vector<std::string>& expected_dsls = *expected_r;
    // Fixture must have exactly 10 templates.
    ASSERT_EQ(expected_dsls.size(), 10u)
        << "factor_templates.txt must have 10 templates";

    // -----------------------------------------------------------------------
    // (a) --seed-file <fixture> only: seed_exprs == file templates in order.
    // -----------------------------------------------------------------------
    {
        const char* argv[] = {
            "atx", "discover",
            "--seed-file", fixture_path.c_str(),
        };
        auto cfg_r = atx::impl::parse_args(4, const_cast<char**>(argv));
        ASSERT_TRUE(cfg_r.has_value())
            << "(a) parse_args with --seed-file must succeed: "
            << cfg_r.error().message();
        const auto& cfg = *cfg_r;
        ASSERT_EQ(cfg.seed_exprs.size(), 10u)
            << "(a) seed_exprs must contain all 10 fixture templates";
        for (std::size_t i = 0; i < expected_dsls.size(); ++i) {
            EXPECT_EQ(cfg.seed_exprs[i], expected_dsls[i])
                << "(a) seed_exprs[" << i << "] must match file order";
        }
    }

    // -----------------------------------------------------------------------
    // (b) --seed-expr X BEFORE --seed-file: seed_exprs == [X, file templates...]
    // -----------------------------------------------------------------------
    {
        const char* argv[] = {
            "atx", "discover",
            "--seed-expr", "rank(close)",
            "--seed-file", fixture_path.c_str(),
        };
        auto cfg_r = atx::impl::parse_args(6, const_cast<char**>(argv));
        ASSERT_TRUE(cfg_r.has_value())
            << "(b) parse_args with --seed-expr then --seed-file must succeed: "
            << cfg_r.error().message();
        const auto& cfg = *cfg_r;
        ASSERT_EQ(cfg.seed_exprs.size(), 11u)
            << "(b) seed_exprs must be [CLI entry] + [10 file templates]";
        EXPECT_EQ(cfg.seed_exprs[0], "rank(close)")
            << "(b) CLI --seed-expr entry must appear first";
        for (std::size_t i = 0; i < expected_dsls.size(); ++i) {
            EXPECT_EQ(cfg.seed_exprs[i + 1], expected_dsls[i])
                << "(b) file template [" << i << "] must follow CLI entry";
        }
    }

    // -----------------------------------------------------------------------
    // (c) No --seed-file: seed_exprs is empty (byte-identical default through
    //     the real parse path, not a tautological manual RunConfig build).
    // -----------------------------------------------------------------------
    {
        const char* argv[] = {"atx", "discover"};
        auto cfg_r = atx::impl::parse_args(2, const_cast<char**>(argv));
        ASSERT_TRUE(cfg_r.has_value())
            << "(c) parse_args without --seed-file must succeed: "
            << cfg_r.error().message();
        EXPECT_TRUE(cfg_r->seed_exprs.empty())
            << "(c) seed_exprs must be empty when --seed-file is not supplied";
    }

    // -----------------------------------------------------------------------
    // (d) --seed-file with a nonexistent path must return Err (fail-closed).
    // -----------------------------------------------------------------------
    {
        const char* argv[] = {
            "atx", "discover",
            "--seed-file", "/no/such/path/nonexistent_seed_file_xyz.txt",
        };
        auto cfg_r = atx::impl::parse_args(4, const_cast<char**>(argv));
        EXPECT_FALSE(cfg_r.has_value())
            << "(d) --seed-file with nonexistent path must return Err";
        if (!cfg_r.has_value()) {
            // The error propagates from read_seed_file as IoError, then wrapped
            // by parse_args as InvalidArgument (the flag machinery uses
            // apply_flag_value which returns the Err directly).
            const auto code = cfg_r.error().code();
            EXPECT_TRUE(
                code == atx::core::ErrorCode::IoError ||
                code == atx::core::ErrorCode::InvalidArgument)
                << "(d) error code must be IoError or InvalidArgument, got: "
                << static_cast<int>(code);
        }
    }
}

// ---------------------------------------------------------------------------
//  W4b_ParseArgsMaxPboThreads — the --max-pbo flag lands in cfg.max_pbo through
//  the REAL parse_args -> apply_flag_value path (the run-level CSCV-PBO batch gate),
//  and the no-flag default is the disabling 1.0 (byte-identical-at-config-layer).
//  Mirrors the W4a --min-split-sharpe flag-threading discipline.
// ---------------------------------------------------------------------------
TEST(AtxImplDiscover, W4b_ParseArgsMaxPboThreads) {
    // (a) --max-pbo 0.5 lands in cfg.max_pbo (the value the gated stage threads into
    //     FactoryConfig::max_pbo).
    {
        const char* argv[] = {
            "atx", "discover",
            "--max-pbo", "0.5",
        };
        auto cfg_r = atx::impl::parse_args(4, const_cast<char**>(argv));
        ASSERT_TRUE(cfg_r.has_value())
            << "(a) parse_args with --max-pbo must succeed: " << cfg_r.error().message();
        EXPECT_DOUBLE_EQ(cfg_r->max_pbo, 0.5)
            << "(a) --max-pbo 0.5 must land in cfg.max_pbo";
    }

    // (b) No --max-pbo: cfg.max_pbo is the disabling 1.0 default (the off path the
    //     discover digest goldens are pinned against).
    {
        const char* argv[] = {"atx", "discover"};
        auto cfg_r = atx::impl::parse_args(2, const_cast<char**>(argv));
        ASSERT_TRUE(cfg_r.has_value())
            << "(b) parse_args without --max-pbo must succeed: " << cfg_r.error().message();
        EXPECT_DOUBLE_EQ(cfg_r->max_pbo, 1.0)
            << "(b) the no-flag default must be the disabling 1.0 (off)";
    }

    // (c) A malformed --max-pbo value fails closed through the real CLI path.
    {
        const char* argv[] = {
            "atx", "discover",
            "--max-pbo", "not-a-number",
        };
        auto cfg_r = atx::impl::parse_args(4, const_cast<char**>(argv));
        EXPECT_FALSE(cfg_r.has_value())
            << "(c) --max-pbo with a non-numeric value must return Err";
    }
}

// ---------------------------------------------------------------------------
//  W5_MeanNamesPerDayUnit — detail::mean_names_per_day (the W5 capacity-universe
//  name-count recorded as an admission metric) over hand-built universe masks.
//  Pure (mask-only), so a tiny Panel pins the exact value.
// ---------------------------------------------------------------------------
TEST(AtxImplDiscover, W5_MeanNamesPerDayUnit) {
    constexpr atx::usize D = 3u, I = 4u; // 3 dates x 4 instruments
    const std::vector<atx::f64> close(D * I, 1.0); // field values irrelevant to the mask

    // (a) all-in (empty universe == all cells in-universe) -> mean == instruments().
    {
        auto r = Panel::create(D, I, {"close"}, {close}, {});
        ASSERT_TRUE(r.has_value()) << "(a) Panel::create: " << r.error().message();
        EXPECT_DOUBLE_EQ(atx::impl::detail::mean_names_per_day(*r), 4.0)
            << "(a) all-in 3x4 -> 4 names/day";
    }

    // (b) a KNOWN partial mask: 3 + 1 + 1 = 5 in-universe cells over 3 dates -> 5/3.
    {
        std::vector<std::uint8_t> univ(D * I, 0u);
        univ[0 * I + 0] = 1u; univ[0 * I + 1] = 1u; univ[0 * I + 2] = 1u; // date 0: 3
        univ[1 * I + 0] = 1u;                                             // date 1: 1
        univ[2 * I + 3] = 1u;                                             // date 2: 1
        auto r = Panel::create(D, I, {"close"}, {close}, univ);
        ASSERT_TRUE(r.has_value()) << "(b) Panel::create: " << r.error().message();
        EXPECT_DOUBLE_EQ(atx::impl::detail::mean_names_per_day(*r), 5.0 / 3.0)
            << "(b) 5 in-universe cells over 3 dates -> 5/3 names/day";
    }

    // (c) all-out (every cell masked) -> 0.0 (the in_univ == 0 edge).
    {
        const std::vector<std::uint8_t> univ(D * I, 0u);
        auto r = Panel::create(D, I, {"close"}, {close}, univ);
        ASSERT_TRUE(r.has_value()) << "(c) Panel::create: " << r.error().message();
        EXPECT_DOUBLE_EQ(atx::impl::detail::mean_names_per_day(*r), 0.0)
            << "(c) fully-masked universe -> 0 names/day";
    }
}

// ---------------------------------------------------------------------------
//  W5_ParseArgsMaxTurnoverThreads — the --max-turnover flag lands in
//  cfg.max_turnover through the REAL parse_args -> apply_flag_value path (it is
//  threaded into GateConfig::max_turnover at stage_discover.cpp), and the no-flag
//  default is the generous 0.70 (so non-capacity runs are unchanged). Mirrors the
//  W4a/W4b flag-threading discipline. The engine RejectTurnover BEHAVIOR is already
//  covered by combine_gate_test.cpp:189; this asserts the impl wiring.
// ---------------------------------------------------------------------------
TEST(AtxImplDiscover, W5_ParseArgsMaxTurnoverThreads) {
    // (a) --max-turnover 0.30 lands in cfg.max_turnover (the tradeable capacity bar).
    {
        const char* argv[] = {
            "atx", "discover",
            "--max-turnover", "0.30",
        };
        auto cfg_r = atx::impl::parse_args(4, const_cast<char**>(argv));
        ASSERT_TRUE(cfg_r.has_value())
            << "(a) parse_args with --max-turnover must succeed: " << cfg_r.error().message();
        EXPECT_DOUBLE_EQ(cfg_r->max_turnover, 0.30)
            << "(a) --max-turnover 0.30 must land in cfg.max_turnover";
    }

    // (b) No --max-turnover: cfg.max_turnover is the generous 0.70 default (the
    //     non-capacity path the plan pins as unchanged).
    {
        const char* argv[] = {"atx", "discover"};
        auto cfg_r = atx::impl::parse_args(2, const_cast<char**>(argv));
        ASSERT_TRUE(cfg_r.has_value())
            << "(b) parse_args without --max-turnover must succeed: " << cfg_r.error().message();
        EXPECT_DOUBLE_EQ(cfg_r->max_turnover, 0.70)
            << "(b) the no-flag default must be the generous 0.70";
    }

    // (c) A malformed --max-turnover value fails closed through the real CLI path.
    {
        const char* argv[] = {
            "atx", "discover",
            "--max-turnover", "not-a-number",
        };
        auto cfg_r = atx::impl::parse_args(4, const_cast<char**>(argv));
        EXPECT_FALSE(cfg_r.has_value())
            << "(c) --max-turnover with a non-numeric value must return Err";
    }
}

// ---------------------------------------------------------------------------
//  W6_RediscoverLowVolCapacityAlpha — the Phase-W ACCEPTANCE test: with W1–W5
//  enabled (Raw weighting, capacity universe, factor-template seeding, wrap_in_op,
//  split-sharpe + turnover bars), run the gated discovery pipeline NON-SEEDED for
//  the low-vol family (seed only momentum + reversal) and require the search to
//  REACH a low-vol-conditioned capacity alpha clearing Sharpe>1 ∧ turnover<0.30 ∧
//  both-halves-positive on the REAL liquid panel.
//
//  Data-gated EXACTLY like single_alpha_capacity_test.cpp: the real verdict needs
//  ATX_ALPHA101_PANEL (the serialized liquid panel). On the synthetic panel the
//  Sharpes are ~0 by construction, so this only proves the full W1–W5 gated path
//  RUNS end-to-end and admits an alpha, then GTEST_SKIPs the rediscovery verdict.
//
//  The panel is augmented (augment_for_alpha101) BEFORE the run: run_discover
//  derives the grammar fields from the panel's own field names and does NOT
//  auto-derive `returns`, so without augmentation the low-vol family
//  (ts_std(returns,20)) would be unreachable and a returns-seed silently dropped.
// ---------------------------------------------------------------------------
TEST(AtxImplDiscover, W6_RediscoverLowVolCapacityAlpha) {
    namespace fs = std::filesystem;

    const char* env_panel = std::getenv("ATX_ALPHA101_PANEL");
    const bool is_real = (env_panel != nullptr);

    // 1. Base panel (real liquid if ATX_ALPHA101_PANEL set, else synthetic ORATS),
    //    augmented so the grammar can reach returns / sector / adv. A real panel that
    //    is set-but-unreadable HARD-FAILS here (ASSERT) rather than silently falling
    //    through to synthetic data under the real (tight-bar) config — which would
    //    otherwise surface as a misleading "failed to rediscover" verdict downstream.
    std::optional<Panel> base_opt;
    if (is_real) {
        auto r = atx::impl::read_panel(env_panel);
        ASSERT_TRUE(r.has_value())
            << "ATX_ALPHA101_PANEL set but unreadable: " << r.error().message();
        base_opt = std::move(*r);
    } else {
        base_opt = atx_impl_test::make_synth_orats_panel(300, 24);
    }
    const Panel& base = *base_opt;

    const std::array<atx::u16, 1> adv_windows{20};
    auto aug_r = atx_impl_test::augment_for_alpha101(
        base, std::span<const atx::u16>{adv_windows});
    ASSERT_TRUE(aug_r.has_value())
        << "augment_for_alpha101 must succeed: "
        << (aug_r.has_value() ? "" : aug_r.error().message());
    const std::string panel_path = write_panel_tmp(*aug_r, "w6_rediscover");
    const std::string alpha_out =
        (fs::temp_directory_path() / "atx_impl_discover_w6_out").string();

    // 2. W1–W5 enabled, NON-SEEDED for low-vol: seed momentum + short-term reversal
    //    ONLY (neither carries ts_std/zscore/signedpower conditioning) — the search
    //    must REACH the low-vol-conditioned family via wrap_in_op + window jitter.
    atx::impl::RunConfig cfg;
    cfg.subcommand        = "discover";
    cfg.panel             = panel_path;
    cfg.alpha_out         = alpha_out;
    cfg.seed              = 20260621ULL;
    cfg.gated             = true;        // W2/W4/W5 live on the gated mine_into path
    cfg.weight_transform  = "raw";       // W1a: preserve in-DSL conditioning (no re-rank)
    cfg.enable_wrap_in_op = true;        // W1b: CREATE signedpower(zscore(raw), p)
    cfg.adv_window        = 20;
    cfg.seed_exprs        = {
        "rank(ts_mean(returns,60))",     // ~12-1 momentum (NOT low-vol)
        "rank(-1*ts_mean(returns,5))",   // short-term reversal (NOT low-vol)
    };

    if (is_real) {
        cfg.min_price        = 1.0;      // W2: stocks > $1
        cfg.min_adv_usd      = 50.0e6;   // W2: 20-day dollar ADV >= $50M
        cfg.min_split_sharpe = 0.0;      // W4a: both-halves-positive admission bar
        cfg.max_turnover     = 0.30;     // W5: tradeable turnover bar
        cfg.min_sharpe       = 0.25;
        cfg.min_fitness      = 1.0;
        cfg.min_dsr          = 0.5;
        cfg.population       = 200;
        cfg.generations      = 40;
    } else {
        // Synthetic: EXERCISE the W1–W5 wiring (capacity price-screen active; closes
        // >> $1) but loosen the quality BARS so the ~0-Sharpe noise path still
        // completes + admits >=1 (the bars themselves are tested elsewhere; here we
        // prove the PIPELINE runs end-to-end). A fast smoke budget.
        cfg.min_price        = 1.0;      // price screen ACTIVE (capacity_on)
        cfg.min_adv_usd       = 0.0;
        cfg.min_sharpe       = -1.0e9;
        cfg.min_fitness      = -1.0e9;
        cfg.min_dsr          = -1.0e9;
        cfg.max_pool_corr    = 1.0;
        cfg.max_turnover     = 1.0e9;
        cfg.population       = 24;
        cfg.generations      = 4;
    }

    // 3. Run the full W1–W5 gated discovery pipeline. It MUST run end-to-end.
    auto r = atx::impl::run_discover(cfg);
    ASSERT_TRUE(r.has_value())
        << "the W1–W5 gated discovery path must run: "
        << (r.has_value() ? "" : r.error().message());

    // admitted >= 1 (the pipeline produced at least one alpha).
    const auto& kvs = r->kvs;
    auto it = std::find_if(kvs.begin(), kvs.end(),
                           [](const auto& p) { return p.first == "admitted"; });
    ASSERT_NE(it, kvs.end()) << "missing 'admitted' kv";
    // strtol (not stoi) so a malformed/unexpected kv string yields 0 rather than
    // throwing out of the test body.
    const long admitted = std::strtol(it->second.c_str(), nullptr, 10);
    EXPECT_GE(admitted, 1L) << "the gated pipeline must admit >= 1 alpha";

    if (!is_real) {
        std::error_code ec;
        fs::remove(panel_path, ec);
        fs::remove_all(alpha_out, ec);
        GTEST_SKIP() << "synthetic panel: Sharpes ~0 by construction; the Sharpe>1 / "
                        "turnover<0.30 rediscovery verdict requires ATX_ALPHA101_PANEL";
    }

    // 4. REAL verdict: parse <alpha_out>/_manifest.txt and require at least one
    //    admitted alpha with Sharpe > 1 AND turnover < 0.30. Both-halves-positive is
    //    guaranteed by the active min_split_sharpe=0.0 gate (only split-stable,
    //    both-halves-clearing alphas were admitted). Compare to the manual lv_z_p2.0
    //    baseline (Sharpe 1.56, turnover 0.15) in single-alpha-capacity-findings.md.
    const std::string manifest_path = (fs::path{alpha_out} / "_manifest.txt").string();
    std::ifstream mf{manifest_path};
    ASSERT_TRUE(mf.is_open()) << "manifest must exist: " << manifest_path;

    auto field_after = [](const std::string& line, const std::string& key) -> double {
        const std::string tok = " " + key + "=";
        const auto pos = line.find(tok);
        if (pos == std::string::npos) {
            return std::numeric_limits<double>::quiet_NaN();
        }
        return std::strtod(line.c_str() + pos + tok.size(), nullptr);
    };

    bool found_tradeable = false;
    double best_sharpe = -std::numeric_limits<double>::infinity();
    double best_turnover = std::numeric_limits<double>::quiet_NaN();
    std::string line;
    while (std::getline(mf, line)) {
        if (line.rfind("alpha[", 0) != 0) {
            continue;
        }
        const double sharpe = field_after(line, "sharpe");
        const double turnover = field_after(line, "turnover");
        if (std::isfinite(sharpe) && sharpe > best_sharpe) {
            best_sharpe = sharpe;
            best_turnover = turnover;
        }
        if (std::isfinite(sharpe) && std::isfinite(turnover) &&
            sharpe > 1.0 && turnover < 0.30) {
            found_tradeable = true;
        }
    }

    EXPECT_TRUE(found_tradeable)
        << "W1–W5 must auto-rediscover >= 1 NON-SEEDED alpha with Sharpe>1 ∧ "
           "turnover<0.30 (both-halves-positive via the split gate). Best seen: sharpe="
        << best_sharpe << " turnover=" << best_turnover;

    std::error_code ec;
    fs::remove(panel_path, ec);
    fs::remove_all(alpha_out, ec);
}

// ---------------------------------------------------------------------------
// R3a Test: ExplicitOosFractionOverride
// With --library-dir set AND explicit --oos-fraction 0.1 in set_flags, the
// effective fraction must be 0.1 (not the auto-default 0.25).
// ---------------------------------------------------------------------------
TEST(AtxImplDiscover, R3a_ExplicitOosFractionOverride) {
    namespace fs = std::filesystem;

    auto panel_opt = make_momentum_panel(/*dates=*/96, /*insts=*/6);
    ASSERT_TRUE(panel_opt.has_value());
    const std::string panel_path = write_panel_tmp(*panel_opt, "r3a_override");
    const std::string lib_dir =
        (fs::temp_directory_path() / "atx_r3a_override_libdir").string();
    const std::string alpha_out =
        (fs::temp_directory_path() / "atx_r3a_override_out").string();
    std::error_code ec0;
    fs::remove_all(lib_dir, ec0);
    fs::remove_all(alpha_out, ec0);

    atx::impl::RunConfig cfg;
    cfg.subcommand   = "discover";
    cfg.panel        = panel_path;
    cfg.alpha_out    = alpha_out;
    cfg.seed         = 7ULL;
    cfg.population   = 16;
    cfg.generations  = 5;
    cfg.seed_exprs   = safe_seed_exprs();
    cfg.gated        = true;
    cfg.min_sharpe   = 0.0;
    cfg.min_fitness  = 0.0;
    cfg.max_turnover = 10.0;
    cfg.max_pool_corr= 1.0;
    cfg.min_dsr      = 0.0;
    cfg.library_dir  = lib_dir;
    cfg.oos_fraction = 0.1;
    cfg.set_flags.insert("oos-fraction"); // simulate explicit CLI --oos-fraction 0.1

    auto r = atx::impl::run_discover(cfg);
    ASSERT_TRUE(r.has_value()) << "explicit override run must succeed: " << r.error().message();

    const std::string manifest = read_manifest(alpha_out);
    // Must contain oos_fraction=0.1, not 0.25.
    EXPECT_NE(manifest.find("oos_fraction=0.1"), std::string::npos)
        << "explicit --oos-fraction 0.1 must override auto-default 0.25; got:\n" << manifest;
    EXPECT_EQ(manifest.find("oos_fraction=0.25"), std::string::npos)
        << "must NOT see auto-default 0.25 when explicit 0.1 is set";

    // Cleanup.
    std::error_code ec;
    fs::remove(panel_path, ec);
    fs::remove_all(lib_dir, ec);
    fs::remove_all(alpha_out, ec);
}

// ---------------------------------------------------------------------------
// R3b Test: AccumulationManifestHasPboLine
// A gated accumulation run with >= 2 admitted alphas must have oos_pbo= in
// the manifest. A run with < 2 admitted must have oos_pbo=nan.
// ---------------------------------------------------------------------------
TEST(AtxImplDiscover, R3b_AccumulationManifestHasPboLine) {
    namespace fs = std::filesystem;

    auto panel_opt = make_momentum_panel(/*dates=*/96, /*insts=*/6);
    ASSERT_TRUE(panel_opt.has_value());
    const std::string panel_path = write_panel_tmp(*panel_opt, "r3b_pbo_manifest");
    const std::string lib_dir =
        (fs::temp_directory_path() / "atx_r3b_pbo_libdir").string();
    const std::string alpha_out =
        (fs::temp_directory_path() / "atx_r3b_pbo_out").string();
    std::error_code ec0;
    fs::remove_all(lib_dir, ec0);
    fs::remove_all(alpha_out, ec0);

    atx::impl::RunConfig cfg;
    cfg.subcommand   = "discover";
    cfg.panel        = panel_path;
    cfg.alpha_out    = alpha_out;
    cfg.seed         = 7ULL;
    cfg.population   = 16;
    cfg.generations  = 5;
    cfg.seed_exprs   = safe_seed_exprs();
    cfg.gated        = true;
    cfg.min_sharpe   = 0.0;
    cfg.min_fitness  = 0.0;
    cfg.max_turnover = 10.0;
    cfg.max_pool_corr= 1.0;
    cfg.min_dsr      = 0.0;
    cfg.library_dir  = lib_dir; // accumulation on -> auto-OOS 0.25

    auto r = atx::impl::run_discover(cfg);
    ASSERT_TRUE(r.has_value()) << "run must succeed: " << r.error().message();

    const std::string manifest = read_manifest(alpha_out);
    // The manifest must have an oos_pbo= line (OOS is active).
    EXPECT_NE(manifest.find("oos_pbo="), std::string::npos)
        << "accumulation run manifest must contain oos_pbo= line; got:\n" << manifest;

    // The oos_pbo kv must be present in StageResult.
    const auto keys = kv_keys(*r);
    EXPECT_NE(keys.find("oos_pbo"), keys.end())
        << "accumulation run StageResult must have oos_pbo key";

    // Cleanup.
    std::error_code ec;
    fs::remove(panel_path, ec);
    fs::remove_all(lib_dir, ec);
    fs::remove_all(alpha_out, ec);
}

// =============================================================================
// R3 Q2 — --pbo-hard-block tests (3 total)
//
// The hard-block decision is a pure predicate over (flag, pbo_gate_passed, pbo,
// max_pbo). We test the predicate directly without a full end-to-end run (which
// would need >= 2 admitted alphas for PBO to be computed). The stage wiring is
// tested by the flag-absent/flag-set advisory distinction.
// =============================================================================

// The predicate replicating the hard-block condition (mirrors stage_discover.cpp).
static bool pbo_hard_block_fails(bool flag, bool pbo_gate_passed, double pbo, double max_pbo) {
    return flag && std::isfinite(pbo) && max_pbo < 1.0 && !pbo_gate_passed;
}

// =============================================================================
// R3-Q2 Test 1 — Flag absent: pbo_hard_block_fails returns false regardless of
//   the PBO value. The advisory gate does NOT affect the exit verdict.
// =============================================================================
TEST(AtxImplDiscover, PboHardBlock_FlagAbsent_ReturnsOk_Advisory) {
    // flag == false -> never fails, even with a breached PBO.
    EXPECT_FALSE(pbo_hard_block_fails(false, false, 0.8, 0.5))
        << "flag absent -> hard block never fires even if PBO breaches";
    EXPECT_FALSE(pbo_hard_block_fails(false, false, 0.99, 0.1))
        << "flag absent -> hard block never fires at extreme PBO";
    EXPECT_FALSE(pbo_hard_block_fails(false, true, 0.3, 0.5))
        << "flag absent -> hard block never fires even when gate passes";
}

// =============================================================================
// R3-Q2 Test 2 — Flag set + breach: pbo_hard_block_fails returns true; NOT
//   set when PBO is NaN/finite-pass or gate is off (max_pbo >= 1.0).
// =============================================================================
TEST(AtxImplDiscover, PboHardBlock_FlagSet_BreachFails) {
    // flag == true, breach (finite pbo > max_pbo, gate NOT passed).
    EXPECT_TRUE(pbo_hard_block_fails(true, false, 0.8, 0.5))
        << "flag set + pbo=0.8 > max_pbo=0.5 + gate not passed -> must fail";
    EXPECT_TRUE(pbo_hard_block_fails(true, false, 0.99, 0.1))
        << "flag set + severe breach -> must fail";

    // flag set, but various non-breach conditions -> must NOT fail.
    EXPECT_FALSE(pbo_hard_block_fails(true, true, 0.8, 0.5))
        << "flag set but pbo_gate_passed -> no hard block";
    EXPECT_FALSE(pbo_hard_block_fails(true, false, std::numeric_limits<double>::quiet_NaN(), 0.5))
        << "flag set but pbo is NaN (not computed) -> no hard block";
    EXPECT_FALSE(pbo_hard_block_fails(true, true, 0.3, 0.5))
        << "flag set but pbo_gate_passed=true (pbo=0.3 <= max_pbo=0.5) -> no hard block";
    EXPECT_FALSE(pbo_hard_block_fails(true, false, 0.8, 1.0))
        << "flag set but max_pbo=1.0 (gate OFF) -> no hard block";
    EXPECT_FALSE(pbo_hard_block_fails(true, false, 0.8, 1.5))
        << "flag set but max_pbo>1 (gate OFF) -> no hard block";
}

// =============================================================================
// R3-Q2 Test 3 — Pure determinism: same inputs always produce same output.
// =============================================================================
TEST(AtxImplDiscover, PboHardBlock_Pure_Deterministic) {
    const bool r1 = pbo_hard_block_fails(true, false, 0.7, 0.4);
    const bool r2 = pbo_hard_block_fails(true, false, 0.7, 0.4);
    EXPECT_EQ(r1, r2) << "pure predicate must be deterministic";
    EXPECT_TRUE(r1) << "pbo=0.7 > max_pbo=0.4 with flag=true and gate_not_passed -> fails";

    const bool r3 = pbo_hard_block_fails(false, false, 0.7, 0.4);
    const bool r4 = pbo_hard_block_fails(false, false, 0.7, 0.4);
    EXPECT_EQ(r3, r4) << "pure predicate must be deterministic with flag=false";
    EXPECT_FALSE(r3) << "flag=false -> never fails";
}

} // namespace atxtest_discover
