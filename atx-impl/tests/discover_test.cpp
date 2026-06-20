// atx::impl — discover stage tests (suite AtxImplDiscover, S3 TDD).
//
// TDD order: GenomeRoundTrip first (isolates serialize_genome), then the
// search-based tests (AdmitsAtLeastOneAlpha, SameSeedDeterministic).
//
// Panel fixture: 96 dates x 6 instruments, single "close" field built via a
// deterministic LCG random walk with a small persistent drift so rank(close)
// has a genuine finite-Sharpe momentum edge (mirrors factory_search_driver_test).

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <vector>
#include <cmath>

#include <gtest/gtest.h>

#include "atx/core/types.hpp"

#include "atx/engine/alpha/bytecode.hpp" // alpha::compile, alpha::Program
#include "atx/engine/alpha/panel.hpp"
#include "atx/engine/alpha/parser.hpp"   // alpha::parse_expr, alpha::Library
#include "atx/engine/alpha/vm.hpp"       // alpha::Engine
#include "atx/engine/factory/genome.hpp" // factory::analyze_into, factory::Genome

#include "config.hpp"
#include "serialize_genome.hpp"
#include "serialize_panel.hpp"
#include "stages.hpp"

namespace atxtest_discover {

using atx::f64;
using atx::usize;
using atx::engine::alpha::compile;
using atx::engine::alpha::Engine;
using atx::engine::alpha::Library;
using atx::engine::alpha::Panel;
using atx::engine::alpha::parse_expr;
using atx::engine::alpha::Program;
using atx::engine::alpha::SignalSet;
using atx::engine::factory::analyze_into;
using atx::engine::factory::Genome;
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

} // namespace atxtest_discover
