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

} // namespace atxtest_discover
