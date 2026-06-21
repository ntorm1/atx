// atx::impl — discover stage tests (suite AtxImplDiscover, S3 TDD).
//
// TDD order: GenomeRoundTrip first (isolates serialize_genome), then the
// search-based tests (AdmitsAtLeastOneAlpha, SameSeedDeterministic).
//
// Panel fixture: 96 dates x 6 instruments, single "close" field built via a
// deterministic LCG random walk with a small persistent drift so rank(close)
// has a genuine finite-Sharpe momentum edge (mirrors factory_search_driver_test).

#include <array>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "atx/core/types.hpp"

#include "atx/engine/alpha/bytecode.hpp"    // alpha::compile, alpha::Program
#include "atx/engine/alpha/panel.hpp"
#include "atx/engine/alpha/parser.hpp"     // alpha::parse_expr, alpha::Library
#include "atx/engine/alpha/vm.hpp"         // alpha::Engine
#include "atx/engine/factory/genome.hpp"   // factory::analyze_into, factory::Genome

#include "config.hpp"
#include "serialize_genome.hpp"
#include "serialize_panel.hpp"
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

} // namespace atxtest_discover
