// atx::impl — combine stage tests (suite AtxImplCombine, S4 TDD).
//
// TDD order: CombinedEqualsWeightedSum first (pins the core semantics —
// that the combined mega-alpha is the exact NaN-aware weighted sum of
// the per-alpha TARGET-WEIGHT (position) streams, NOT the raw signals).
// Remaining tests verify structure, determinism, and field coverage.
//
// Panel fixture: 96 dates x 6 instruments, "close" field (required by
// extract_streams) built via a deterministic LCG random walk with drift.
// Uses method="equal" (w=1/N, closed-form) for correctness tests so there
// is no numerical solver that could fail on a tiny fixture.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "atx/core/types.hpp"

#include "atx/engine/alpha/bytecode.hpp"  // alpha::compile_batch
#include "atx/engine/alpha/panel.hpp"     // alpha::Panel, alpha::SignalSet
#include "atx/engine/alpha/parser.hpp"    // alpha::Library
#include "atx/engine/alpha/streams.hpp"   // alpha::extract_streams (position streams)
#include "atx/engine/alpha/vm.hpp"        // alpha::Engine
#include "atx/engine/loop/weight_policy.hpp" // engine::WeightPolicy

#include "config.hpp"
#include "research_sim.hpp"               // frictionless_sim
#include "serialize_panel.hpp"
#include "stages.hpp"

namespace atxtest_combine {

using atx::f64;
using atx::usize;
using atx::engine::alpha::Engine;
using atx::engine::alpha::Library;
using atx::engine::alpha::Panel;
using atx::engine::alpha::SignalSet;

// ---------------------------------------------------------------------------
// Deterministic LCG (same idiom as discover_test.cpp).
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

// Build a panel with the given field names and columns (all same dates/insts).
// All cells in universe (empty universe mask = all in).
static std::optional<Panel> make_panel(
    usize dates, usize insts,
    const std::vector<std::string>& field_names,
    const std::vector<std::vector<f64>>& columns)
{
    auto r = Panel::create(dates, insts, field_names, columns, {});
    if (!r.has_value()) {
        ADD_FAILURE() << "panel fixture must build: " << r.error().to_string();
        return std::nullopt;
    }
    return std::move(r.value());
}

// Build the standard 96x6 single-field "close" momentum panel.
static std::optional<Panel> make_momentum_panel(usize dates = 96, usize insts = 6) {
    const std::vector<f64> close = noisy_close(dates, insts, 0xDEADBEEFULL);
    return make_panel(dates, insts, {"close"}, {close});
}

// Write panel to a temp file and return path.
static std::string write_panel_tmp(const Panel& panel, const std::string& stem) {
    namespace fs = std::filesystem;
    const std::string path =
        (fs::temp_directory_path() / ("atx_impl_combine_" + stem + ".bin")).string();
    auto r = atx::impl::write_panel(panel, path);
    EXPECT_TRUE(r.has_value()) << "write_panel must succeed";
    return path;
}

// Write DSL files into a temp directory and return the dir path.
static std::string write_alpha_dir(
    const std::string& stem,
    const std::vector<std::string>& dsls)
{
    namespace fs = std::filesystem;
    const std::string dir =
        (fs::temp_directory_path() / ("atx_impl_combine_alphas_" + stem)).string();
    fs::create_directories(dir);
    for (usize i = 0; i < dsls.size(); ++i) {
        std::ostringstream name;
        name << "alpha_" << i << ".dsl";
        std::ofstream f{(fs::path{dir} / name.str()).string()};
        EXPECT_TRUE(f.is_open());
        f << dsls[i] << '\n';
    }
    return dir;
}

// Three verified-safe DSL expressions for a {"close"} panel.
static std::vector<std::string> safe_dsls() {
    return {"rank(close)", "ts_mean(close,10)", "delta(close,2)"};
}

// ---------------------------------------------------------------------------
// Test 2 (TDD FIRST): CombinedEqualsWeightedSum
// With method="equal" (w=1/3 each), the combined mega-alpha is the weighted sum
// of the per-alpha TARGET-WEIGHT (position) streams — NOT the raw signals.
// Positions are the winsorized/ranked/dollar-neutral/gross-normalized books each
// alpha's metrics were validated on (streams.hpp contract). Recompute the
// position streams independently via the same extract_streams path run_combine
// uses, and verify combined[d,i] == (p0+p1+p2)/3 at in-universe probe cells.
// ---------------------------------------------------------------------------
TEST(AtxImplCombine, CombinedEqualsWeightedSum) {
    namespace fs = std::filesystem;

    auto panel_opt = make_momentum_panel();
    ASSERT_TRUE(panel_opt.has_value());
    const Panel& panel = *panel_opt;
    const std::string panel_path = write_panel_tmp(panel, "weighted_sum");

    const std::vector<std::string> dsls = safe_dsls();
    const std::string alphas_dir  = write_alpha_dir("weighted_sum", dsls);
    const std::string combo_out   =
        (fs::temp_directory_path() / "atx_impl_combine_weighted_sum.bin").string();

    atx::impl::RunConfig cfg;
    cfg.subcommand = "combine";
    cfg.panel      = panel_path;
    cfg.alphas     = alphas_dir;
    cfg.combo_out  = combo_out;
    cfg.method     = "equal";
    cfg.fit_begin  = 0;
    cfg.fit_end    = 0; // -> defaults to dates

    auto r = atx::impl::run_combine(cfg);
    ASSERT_TRUE(r.has_value()) << r.error().message();

    // Read back the combined panel.
    auto cpanel_r = atx::impl::read_panel(combo_out);
    ASSERT_TRUE(cpanel_r.has_value()) << cpanel_r.error().message();
    const Panel& cpanel = *cpanel_r;

    // Recompute the per-alpha POSITION streams independently via the SAME engine
    // + extract_streams path run_combine uses (default WeightPolicy, frictionless
    // sim). The combined mega-alpha must equal the equal-weighted sum of these
    // target-weight streams.
    Library lib{};
    std::vector<std::string_view> views(dsls.begin(), dsls.end());
    auto prog_r = atx::engine::alpha::compile_batch(
        std::span<const std::string_view>{views}, lib);
    ASSERT_TRUE(prog_r.has_value()) << prog_r.error().message();

    Engine engine{panel};
    auto ss_r = engine.evaluate(*prog_r);
    ASSERT_TRUE(ss_r.has_value()) << ss_r.error().message();
    const SignalSet& ss = *ss_r;
    ASSERT_EQ(ss.alphas.size(), 3u);

    atx::engine::WeightPolicy policy{};
    auto sim = atx::impl::frictionless_sim();
    auto streams_r = atx::engine::alpha::extract_streams(ss, policy, panel, sim);
    ASSERT_TRUE(streams_r.has_value()) << streams_r.error().message();
    const auto& streams = *streams_r;

    const usize D  = panel.dates();
    const usize N  = panel.instruments();

    // Retrieve the combined field span.
    auto fid_r = cpanel.field_id("alpha");
    ASSERT_TRUE(fid_r.has_value()) << "combined panel must have field 'alpha'";
    const auto combined_span = cpanel.field_all(*fid_r);

    usize probes_checked = 0;
    usize nonzero_seen   = 0;
    for (usize d = 10; d < D; ++d) {     // skip first 10 (warm-up region)
        const auto p0 = streams.positions(0, d);
        const auto p1 = streams.positions(1, d);
        const auto p2 = streams.positions(2, d);
        for (usize i = 0; i < N; ++i) {
            const usize c = d * N + i;
            if (!panel.in_universe(d, i)) {
                continue; // not a tradable name on this date -> left NaN
            }
            const f64 expected = (p0[i] + p1[i] + p2[i]) / 3.0;
            const f64 actual   = combined_span[c];
            EXPECT_FALSE(std::isnan(actual))
                << "combined cell [" << d << "," << i << "] should not be NaN";
            EXPECT_NEAR(actual, expected, 1e-9)
                << "combined[" << d << "," << i << "] = " << actual
                << " expected " << expected;
            if (std::abs(actual) > 1e-12) { ++nonzero_seen; }
            ++probes_checked;
            if (probes_checked >= 20) { goto done; } // enough probe cells
        }
    }
done:
    EXPECT_GT(probes_checked, 0u) << "no in-universe probe cells found";
    EXPECT_GT(nonzero_seen, 0u)
        << "combined positions must be non-trivially non-zero";

    // Cleanup.
    std::error_code ec;
    fs::remove(panel_path, ec);
    fs::remove_all(alphas_dir, ec);
    fs::remove(combo_out, ec);
    fs::remove(combo_out + ".weights.txt", ec);
}

// ---------------------------------------------------------------------------
// Test 1: CombinesEqualWeight
// Basic happy-path: panel 96x6, 3 alphas, method="equal"; combo.bin exists,
// has 1 field named "alpha", correct dates/instruments; weights sidecar exists.
// ---------------------------------------------------------------------------
TEST(AtxImplCombine, CombinesEqualWeight) {
    namespace fs = std::filesystem;

    auto panel_opt = make_momentum_panel();
    ASSERT_TRUE(panel_opt.has_value());
    const Panel& panel = *panel_opt;
    const std::string panel_path = write_panel_tmp(panel, "equal_weight");

    const std::string alphas_dir = write_alpha_dir("equal_weight", safe_dsls());
    const std::string combo_out  =
        (fs::temp_directory_path() / "atx_impl_combine_equal_weight.bin").string();

    atx::impl::RunConfig cfg;
    cfg.subcommand = "combine";
    cfg.panel      = panel_path;
    cfg.alphas     = alphas_dir;
    cfg.combo_out  = combo_out;
    cfg.method     = "equal";
    cfg.fit_begin  = 0;
    cfg.fit_end    = 0; // -> defaults to dates (96)

    auto r = atx::impl::run_combine(cfg);
    ASSERT_TRUE(r.has_value()) << r.error().message();

    // combo.bin must exist.
    EXPECT_TRUE(fs::exists(combo_out)) << "combo.bin not created";

    // Read back and verify shape.
    auto cpanel_r = atx::impl::read_panel(combo_out);
    ASSERT_TRUE(cpanel_r.has_value()) << cpanel_r.error().message();
    const Panel& cpanel = *cpanel_r;
    EXPECT_EQ(cpanel.num_fields(),   1u)  << "combined panel must have 1 field";
    EXPECT_EQ(cpanel.dates(),        96u) << "combined panel must have 96 dates";
    EXPECT_EQ(cpanel.instruments(),  6u)  << "combined panel must have 6 instruments";

    // Field must be named "alpha".
    auto fid_r = cpanel.field_id("alpha");
    EXPECT_TRUE(fid_r.has_value()) << "combined panel must have field named 'alpha'";

    // Weights sidecar must exist.
    EXPECT_TRUE(fs::exists(combo_out + ".weights.txt")) << "weights sidecar not created";

    // Stage result kvs sanity.
    const auto& kvs = r->kvs;
    auto find_kv = [&](const std::string& k) -> std::string {
        for (const auto& p : kvs) if (p.first == k) return p.second;
        return "";
    };
    EXPECT_EQ(find_kv("alphas"), "3");
    EXPECT_EQ(find_kv("method"), "equal");

    // Cleanup.
    std::error_code ec;
    fs::remove(panel_path, ec);
    fs::remove_all(alphas_dir, ec);
    fs::remove(combo_out, ec);
    fs::remove(combo_out + ".weights.txt", ec);
}

// ---------------------------------------------------------------------------
// Test 3: DeterministicAcrossRuns (R1)
// run_combine twice into two combo files, same inputs; assert equal non-zero
// stage digests AND byte-identical combo.bin files.
// ---------------------------------------------------------------------------
TEST(AtxImplCombine, DeterministicAcrossRuns) {
    namespace fs = std::filesystem;

    auto panel_opt = make_momentum_panel();
    ASSERT_TRUE(panel_opt.has_value());
    const Panel& panel = *panel_opt;
    const std::string panel_path = write_panel_tmp(panel, "deterministic");

    const std::string alphas_dir = write_alpha_dir("deterministic", safe_dsls());
    const std::string combo1 =
        (fs::temp_directory_path() / "atx_impl_combine_det1.bin").string();
    const std::string combo2 =
        (fs::temp_directory_path() / "atx_impl_combine_det2.bin").string();

    atx::impl::RunConfig cfg;
    cfg.subcommand = "combine";
    cfg.panel      = panel_path;
    cfg.alphas     = alphas_dir;
    cfg.method     = "equal";
    cfg.fit_begin  = 0;
    cfg.fit_end    = 0;

    cfg.combo_out = combo1;
    auto r1 = atx::impl::run_combine(cfg);
    ASSERT_TRUE(r1.has_value()) << r1.error().message();

    cfg.combo_out = combo2;
    auto r2 = atx::impl::run_combine(cfg);
    ASSERT_TRUE(r2.has_value()) << r2.error().message();

    // Digests must be equal and non-zero.
    EXPECT_EQ(r1->digest, r2->digest) << "digests must be equal across runs";
    EXPECT_NE(r1->digest, atx::u64{0}) << "digest must be non-zero";

    // Files must be byte-identical.
    auto read_bytes = [](const std::string& path) -> std::vector<char> {
        std::ifstream f{path, std::ios::binary};
        return std::vector<char>{std::istreambuf_iterator<char>(f),
                                 std::istreambuf_iterator<char>()};
    };
    const auto bytes1 = read_bytes(combo1);
    const auto bytes2 = read_bytes(combo2);
    EXPECT_FALSE(bytes1.empty()) << "combo1 must not be empty";
    EXPECT_EQ(bytes1, bytes2) << "combo.bin files must be byte-identical";

    // Cleanup.
    std::error_code ec;
    fs::remove(panel_path, ec);
    fs::remove_all(alphas_dir, ec);
    fs::remove(combo1, ec);
    fs::remove(combo1 + ".weights.txt", ec);
    fs::remove(combo2, ec);
    fs::remove(combo2 + ".weights.txt", ec);
}

// ---------------------------------------------------------------------------
// Test 4: MarketCapResolves (plan R2)
// Panel {"close","market_cap"}; one alpha "rank(market_cap)"; method="equal";
// proves the research-panel re-eval resolves market_cap (which the loop's
// OHLCV-only VmSignalSource could not).
// ---------------------------------------------------------------------------
TEST(AtxImplCombine, MarketCapResolves) {
    namespace fs = std::filesystem;

    const usize dates = 96;
    const usize insts = 6;
    const std::vector<f64> close      = noisy_close(dates, insts, 0xDEADBEEFULL);
    const std::vector<f64> market_cap = noisy_close(dates, insts, 0xFEEDFACEULL);

    // Make all market_cap values positive (they need to be for rank to work well).
    std::vector<f64> mc_pos = market_cap;
    for (auto& v : mc_pos) { v = std::abs(v) + 1.0; }

    auto panel_opt = make_panel(dates, insts,
                                {"close", "market_cap"}, {close, mc_pos});
    ASSERT_TRUE(panel_opt.has_value());
    const Panel& panel = *panel_opt;
    const std::string panel_path = write_panel_tmp(panel, "market_cap");

    const std::string alphas_dir = write_alpha_dir("market_cap", {"rank(market_cap)"});
    const std::string combo_out  =
        (fs::temp_directory_path() / "atx_impl_combine_market_cap.bin").string();

    atx::impl::RunConfig cfg;
    cfg.subcommand = "combine";
    cfg.panel      = panel_path;
    cfg.alphas     = alphas_dir;
    cfg.combo_out  = combo_out;
    cfg.method     = "equal";
    cfg.fit_begin  = 0;
    cfg.fit_end    = 0;

    auto r = atx::impl::run_combine(cfg);
    ASSERT_TRUE(r.has_value()) << r.error().message();

    // Read back and verify it's a valid 1-field panel.
    auto cpanel_r = atx::impl::read_panel(combo_out);
    ASSERT_TRUE(cpanel_r.has_value()) << cpanel_r.error().message();
    EXPECT_EQ(cpanel_r->num_fields(), 1u);

    // Cleanup.
    std::error_code ec;
    fs::remove(panel_path, ec);
    fs::remove_all(alphas_dir, ec);
    fs::remove(combo_out, ec);
    fs::remove(combo_out + ".weights.txt", ec);
}

// ---------------------------------------------------------------------------
// Test 5: MissingArgsFails
// empty panel / empty alphas dir path -> Err(InvalidArgument).
// ---------------------------------------------------------------------------
TEST(AtxImplCombine, MissingArgsFails) {
    // Empty panel path.
    {
        atx::impl::RunConfig cfg;
        cfg.subcommand = "combine";
        cfg.panel      = "";
        cfg.alphas     = "/some/dir";
        cfg.combo_out  = "/some/out.bin";
        auto r = atx::impl::run_combine(cfg);
        EXPECT_FALSE(r.has_value());
        EXPECT_EQ(r.error().code(), atx::core::ErrorCode::InvalidArgument);
    }

    // Empty alphas path.
    {
        atx::impl::RunConfig cfg;
        cfg.subcommand = "combine";
        cfg.panel      = "/some/panel.bin";
        cfg.alphas     = "";
        cfg.combo_out  = "/some/out.bin";
        auto r = atx::impl::run_combine(cfg);
        EXPECT_FALSE(r.has_value());
        EXPECT_EQ(r.error().code(), atx::core::ErrorCode::InvalidArgument);
    }

    // Empty combo_out.
    {
        atx::impl::RunConfig cfg;
        cfg.subcommand = "combine";
        cfg.panel      = "/some/panel.bin";
        cfg.alphas     = "/some/dir";
        cfg.combo_out  = "";
        auto r = atx::impl::run_combine(cfg);
        EXPECT_FALSE(r.has_value());
        EXPECT_EQ(r.error().code(), atx::core::ErrorCode::InvalidArgument);
    }
}

// ---------------------------------------------------------------------------
// Test 6: SectorNeutralCombinedBookIsPerSectorNeutral
// With --sector-neutral and a 2-sector panel {"close","sector"} (sectors {0,1}
// split 3+3), the combined mega-alpha must be dollar-neutral WITHIN each sector
// (per-sector sum == 0) for every date past the warm-up window.
// ---------------------------------------------------------------------------
TEST(AtxImplCombine, SectorNeutralCombinedBookIsPerSectorNeutral) {
  namespace fs = std::filesystem;
  const usize D = 96, N = 6;
  const std::vector<f64> close = noisy_close(D, N, 0xDEADBEEFULL);
  std::vector<f64> sect(D * N);
  for (usize t = 0; t < D; ++t) for (usize i = 0; i < N; ++i) sect[t*N+i] = (i < 3 ? 0.0 : 1.0);
  auto panel_opt = make_panel(D, N, {"close","sector"}, {close, sect});
  ASSERT_TRUE(panel_opt.has_value());
  const Panel& panel = *panel_opt;
  const std::string panel_path = write_panel_tmp(panel, "sector_neutral");
  const std::string alphas_dir = write_alpha_dir("sector_neutral", safe_dsls());
  const std::string combo_out = (fs::temp_directory_path() / "atx_impl_combine_sector_neutral.bin").string();

  atx::impl::RunConfig cfg;
  cfg.subcommand = "combine"; cfg.panel = panel_path; cfg.alphas = alphas_dir;
  cfg.combo_out = combo_out; cfg.method = "equal"; cfg.sector_neutral = true;
  auto r = atx::impl::run_combine(cfg);
  ASSERT_TRUE(r.has_value()) << r.error().message();

  auto cpanel = atx::impl::read_panel(combo_out).value();
  auto fid = cpanel.field_id("alpha").value();
  usize dates_checked = 0;
  for (usize d = 12; d < D; ++d) {
    auto cs = cpanel.field_cross_section(fid, d);
    f64 g0 = 0.0, g1 = 0.0; usize n0 = 0, n1 = 0;
    for (usize i = 0; i < N; ++i) {
      if (std::isnan(cs[i])) continue;
      if (i < 3) { g0 += cs[i]; ++n0; } else { g1 += cs[i]; ++n1; }
    }
    if (n0 > 0) EXPECT_NEAR(g0, 0.0, 1e-9);  // sector 0 neutral when populated
    if (n1 > 0) EXPECT_NEAR(g1, 0.0, 1e-9);  // sector 1 neutral when populated
    if (n0 > 0 && n1 > 0) ++dates_checked;
  }
  // Guard against a vacuous pass: at least one date must have BOTH sectors
  // populated so the per-sector neutrality was actually exercised on real data.
  EXPECT_GT(dates_checked, 0u) << "no date had both sectors populated";
  std::error_code ec; fs::remove(panel_path, ec); fs::remove_all(alphas_dir, ec);
  fs::remove(combo_out, ec); fs::remove(combo_out + ".weights.txt", ec);
}

// ---------------------------------------------------------------------------
// 8.B Test: CombineFromLibraryMatchesDslPath
// Discover (gated) accumulates N alphas into a --library-dir AND writes the same
// N as alpha_NNN.dsl into alpha_out. Combine is then run TWICE on the same panel
// + method: once over the loose .dsl dir (--alphas) and once over the persistent
// library (--library-dir). Both enumerate the SAME N alphas in the SAME (AlphaId)
// order with the SAME stored expression source, so the combine math is identical
// and the two combo panels are BYTE-IDENTICAL (equal digests). The library path
// is also run twice to confirm it is itself deterministic.
// ---------------------------------------------------------------------------
TEST(AtxImplCombine, CombineFromLibraryMatchesDslPath) {
    namespace fs = std::filesystem;

    auto panel_opt = make_momentum_panel(/*dates=*/96, /*insts=*/6);
    ASSERT_TRUE(panel_opt.has_value());
    const Panel& panel = *panel_opt;
    const std::string panel_path = write_panel_tmp(panel, "lib_combine");

    const std::string lib_dir =
        (fs::temp_directory_path() / "atx_impl_combine_libdir").string();
    const std::string alpha_out =
        (fs::temp_directory_path() / "atx_impl_combine_lib_alpha_out").string();
    std::error_code ec0;
    fs::remove_all(lib_dir, ec0);
    fs::remove_all(alpha_out, ec0);

    // --- Discover (gated) into the library dir; writes .dsl into alpha_out too. ---
    {
        atx::impl::RunConfig cfg;
        cfg.subcommand   = "discover";
        cfg.panel        = panel_path;
        cfg.alpha_out    = alpha_out;
        cfg.library_dir  = lib_dir;
        cfg.seed         = 909ULL;
        cfg.population   = 16;
        cfg.generations  = 5;
        cfg.seed_exprs   = safe_dsls(); // valid {"close"} expressions
        cfg.gated        = true;
        cfg.min_sharpe   = 0.0;
        cfg.min_fitness  = 0.0;
        cfg.max_turnover = 10.0;
        cfg.max_pool_corr= 1.0;
        cfg.min_dsr      = 0.0;
        auto rd = atx::impl::run_discover(cfg);
        ASSERT_TRUE(rd.has_value()) << rd.error().message();
    }
    // Sanity: the library wrote >= 1 .dsl into alpha_out.
    int n_dsl = 0;
    for (const auto& e : fs::directory_iterator(alpha_out)) {
        if (e.path().extension() == ".dsl") ++n_dsl;
    }
    ASSERT_GE(n_dsl, 1) << "discover must have admitted >= 1 alpha";

    auto run_combine_into = [&](const std::string& tag, bool from_lib)
        -> atx::core::Result<atx::impl::StageResult> {
        atx::impl::RunConfig cfg;
        cfg.subcommand = "combine";
        cfg.panel      = panel_path;
        cfg.combo_out  =
            (fs::temp_directory_path() / ("atx_impl_combine_lib_" + tag + ".bin")).string();
        cfg.method     = "equal";
        if (from_lib) {
            cfg.library_dir = lib_dir;     // 8.B library-backed input
        } else {
            cfg.alphas      = alpha_out;   // loose .dsl input
        }
        return atx::impl::run_combine(cfg);
    };

    // --- Combine from the loose .dsl dir (backward-compat path). ---
    auto r_dsl = run_combine_into("dsl", /*from_lib=*/false);
    ASSERT_TRUE(r_dsl.has_value()) << r_dsl.error().message();

    // --- Combine from the persistent library (8.B). ---
    auto r_lib = run_combine_into("lib", /*from_lib=*/true);
    ASSERT_TRUE(r_lib.has_value()) << r_lib.error().message();

    // --- Combine from the library AGAIN (twice-run determinism). ---
    auto r_lib2 = run_combine_into("lib2", /*from_lib=*/true);
    ASSERT_TRUE(r_lib2.has_value()) << r_lib2.error().message();

    // PARITY: the library path and the .dsl path produce a byte-identical combo
    // (same N alphas, same order, same expressions => same combine math).
    EXPECT_EQ(r_lib->digest, r_dsl->digest)
        << "library-backed combine must byte-match the .dsl path on the same alpha set";
    // DETERMINISM: the library path is stable across re-runs.
    EXPECT_EQ(r_lib->digest, r_lib2->digest)
        << "library-backed combine must be deterministic (twice-run identical)";
    EXPECT_NE(r_lib->digest, atx::u64{0});

    // The "alphas" kv must report the same count on both paths.
    auto alphas_kv = [](const atx::impl::StageResult& sr) -> int {
        for (const auto& p : sr.kvs) {
            if (p.first == "alphas") return std::stoi(p.second);
        }
        return -1;
    };
    EXPECT_EQ(alphas_kv(*r_lib), alphas_kv(*r_dsl)) << "same N alphas on both paths";
    EXPECT_EQ(alphas_kv(*r_lib), n_dsl);

    // Cleanup.
    std::error_code ec;
    fs::remove(panel_path, ec);
    fs::remove_all(lib_dir, ec);
    fs::remove_all(alpha_out, ec);
    for (const char* tag : {"dsl", "lib", "lib2"}) {
        const std::string co =
            (fs::temp_directory_path() / ("atx_impl_combine_lib_" + std::string(tag) + ".bin")).string();
        fs::remove(co, ec);
        fs::remove(co + ".weights.txt", ec);
    }
}

// ---------------------------------------------------------------------------
// 9.2 Test: CorrPenaltyZeroIsByteIdenticalToDefault
// The opt-in crowding de-correlation (--corr-penalty / --capacity-floor) defaults
// to 0.0, which is the engine's EXACT-passthrough rail. A combine run with
// corr_penalty == 0 (explicit) and capacity_floor == 0 must be BYTE-IDENTICAL to a
// combine run with no de-correlation knobs at all: same combo.bin digest AND same
// file bytes. This pins "the no-flag path is unchanged".
// ---------------------------------------------------------------------------
TEST(AtxImplCombine, CorrPenaltyZeroIsByteIdenticalToDefault) {
    namespace fs = std::filesystem;

    auto panel_opt = make_momentum_panel();
    ASSERT_TRUE(panel_opt.has_value());
    const Panel& panel = *panel_opt;
    const std::string panel_path = write_panel_tmp(panel, "corr0");

    const std::string alphas_dir = write_alpha_dir("corr0", safe_dsls());
    const std::string combo_default =
        (fs::temp_directory_path() / "atx_impl_combine_corr_default.bin").string();
    const std::string combo_zero =
        (fs::temp_directory_path() / "atx_impl_combine_corr_zero.bin").string();

    atx::impl::RunConfig cfg;
    cfg.subcommand = "combine";
    cfg.panel      = panel_path;
    cfg.alphas     = alphas_dir;
    cfg.method     = "shrinkage-mv"; // exercise the real fitted weights (not equal)
    cfg.fit_begin  = 0;
    cfg.fit_end    = 0;

    // (a) Default: no de-correlation knobs set (both stay 0.0 by struct default).
    cfg.combo_out = combo_default;
    auto r_default = atx::impl::run_combine(cfg);
    ASSERT_TRUE(r_default.has_value()) << r_default.error().message();

    // (b) Explicit corr_penalty == 0, capacity_floor == 0 -> exact passthrough.
    cfg.corr_penalty   = 0.0;
    cfg.capacity_floor = 0.0;
    cfg.combo_out      = combo_zero;
    auto r_zero = atx::impl::run_combine(cfg);
    ASSERT_TRUE(r_zero.has_value()) << r_zero.error().message();

    EXPECT_EQ(r_default->digest, r_zero->digest)
        << "corr_penalty 0 must be byte-identical to the default (no-knob) path";
    EXPECT_NE(r_default->digest, atx::u64{0});

    auto read_bytes = [](const std::string& path) -> std::vector<char> {
        std::ifstream f{path, std::ios::binary};
        return std::vector<char>{std::istreambuf_iterator<char>(f),
                                 std::istreambuf_iterator<char>()};
    };
    const auto b_default = read_bytes(combo_default);
    const auto b_zero    = read_bytes(combo_zero);
    EXPECT_FALSE(b_default.empty());
    EXPECT_EQ(b_default, b_zero) << "default and corr_penalty-0 combo.bin must match";

    std::error_code ec;
    fs::remove(panel_path, ec);
    fs::remove_all(alphas_dir, ec);
    for (const std::string& co : {combo_default, combo_zero}) {
        fs::remove(co, ec);
        fs::remove(co + ".weights.txt", ec);
    }
}

// ---------------------------------------------------------------------------
// 9.2 Test: CorrPenaltyPositiveChangesWeightsAndIsDeterministic
// With --corr-penalty > 0 on a pool of correlated alphas (rank/ts_mean/delta of
// the same close panel share non-trivial PnL correlation), the de-correlation
// SHRINKS the crowded weights, so the combined panel must DIFFER from the
// passthrough (corr_penalty 0) combo. And the de-correlated path must itself be
// twice-run BYTE-IDENTICAL (decorrelate_weights is pure / no RNG).
// ---------------------------------------------------------------------------
TEST(AtxImplCombine, CorrPenaltyPositiveChangesWeightsAndIsDeterministic) {
    namespace fs = std::filesystem;

    auto panel_opt = make_momentum_panel();
    ASSERT_TRUE(panel_opt.has_value());
    const Panel& panel = *panel_opt;
    const std::string panel_path = write_panel_tmp(panel, "corrpos");

    const std::string alphas_dir = write_alpha_dir("corrpos", safe_dsls());
    const std::string combo_off =
        (fs::temp_directory_path() / "atx_impl_combine_corr_off.bin").string();
    const std::string combo_on1 =
        (fs::temp_directory_path() / "atx_impl_combine_corr_on1.bin").string();
    const std::string combo_on2 =
        (fs::temp_directory_path() / "atx_impl_combine_corr_on2.bin").string();

    atx::impl::RunConfig cfg;
    cfg.subcommand = "combine";
    cfg.panel      = panel_path;
    cfg.alphas     = alphas_dir;
    cfg.method     = "shrinkage-mv";
    cfg.fit_begin  = 0;
    cfg.fit_end    = 0;

    // (a) De-correlation OFF (passthrough).
    cfg.corr_penalty = 0.0;
    cfg.combo_out    = combo_off;
    auto r_off = atx::impl::run_combine(cfg);
    ASSERT_TRUE(r_off.has_value()) << r_off.error().message();

    // (b) De-correlation ON, run #1.
    cfg.corr_penalty = 1.0;
    cfg.combo_out    = combo_on1;
    auto r_on1 = atx::impl::run_combine(cfg);
    ASSERT_TRUE(r_on1.has_value()) << r_on1.error().message();

    // (c) De-correlation ON, run #2 (same inputs) -> must be byte-identical to #1.
    cfg.combo_out = combo_on2;
    auto r_on2 = atx::impl::run_combine(cfg);
    ASSERT_TRUE(r_on2.has_value()) << r_on2.error().message();

    // De-correlation must actually change the combined book (the alphas are
    // mutually correlated, so the crowded weights shrink).
    EXPECT_NE(r_on1->digest, r_off->digest)
        << "corr_penalty > 0 must change the combined panel vs passthrough";
    // Determinism: the de-correlated path is twice-run byte-identical.
    EXPECT_EQ(r_on1->digest, r_on2->digest)
        << "de-correlated combine must be deterministic (twice-run identical)";

    auto read_bytes = [](const std::string& path) -> std::vector<char> {
        std::ifstream f{path, std::ios::binary};
        return std::vector<char>{std::istreambuf_iterator<char>(f),
                                 std::istreambuf_iterator<char>()};
    };
    EXPECT_EQ(read_bytes(combo_on1), read_bytes(combo_on2))
        << "de-correlated combo.bin must be byte-identical across runs";

    std::error_code ec;
    fs::remove(panel_path, ec);
    fs::remove_all(alphas_dir, ec);
    for (const std::string& co : {combo_off, combo_on1, combo_on2}) {
        fs::remove(co, ec);
        fs::remove(co + ".weights.txt", ec);
    }
}

// ---------------------------------------------------------------------------
// A2a helper: read a `key=value\n` sidecar (combo.meta) into a small map.
// ---------------------------------------------------------------------------
static std::map<std::string, std::string> read_kv_file(const std::string& path) {
    std::map<std::string, std::string> kv;
    std::ifstream f{path};
    std::string line;
    while (std::getline(f, line)) {
        const auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        kv[line.substr(0, eq)] = line.substr(eq + 1);
    }
    return kv;
}

// ---------------------------------------------------------------------------
// A2a Test: HoldoutFracZeroIsByteIdenticalToDefault
// The new --holdout-frac defaults to 0.0 (off). A combine run with the flag
// ABSENT must produce a BYTE-IDENTICAL combo.bin / digest to a run with an
// explicit --holdout-frac 0 (both resolve fit_end == np). The combo.meta sidecar
// is a SEPARATE file, NOT part of the panel digest. With holdout off, combo.meta
// records fit_end == np and holdout_begin == np (no OOS window).
// ---------------------------------------------------------------------------
TEST(AtxImplCombine, HoldoutFracZeroIsByteIdenticalToDefault) {
    namespace fs = std::filesystem;

    auto panel_opt = make_momentum_panel();
    ASSERT_TRUE(panel_opt.has_value());
    const Panel& panel = *panel_opt;
    const std::string panel_path = write_panel_tmp(panel, "holdout0");
    const usize np = panel.dates(); // n_periods == panel dates for this fixture

    const std::string alphas_dir = write_alpha_dir("holdout0", safe_dsls());
    const std::string combo_default =
        (fs::temp_directory_path() / "atx_impl_combine_holdout_default.bin").string();
    const std::string combo_zero =
        (fs::temp_directory_path() / "atx_impl_combine_holdout_zero.bin").string();

    atx::impl::RunConfig cfg;
    cfg.subcommand = "combine";
    cfg.panel      = panel_path;
    cfg.alphas     = alphas_dir;
    cfg.method     = "shrinkage-mv"; // exercise the real fitted weights (not equal)
    cfg.fit_begin  = 0;
    cfg.fit_end    = 0;

    // (a) Default: no holdout flag set (combine_holdout_frac stays 0.0).
    cfg.combo_out = combo_default;
    auto r_default = atx::impl::run_combine(cfg);
    ASSERT_TRUE(r_default.has_value()) << r_default.error().message();

    // (b) Explicit --holdout-frac 0 -> identical fit window (fit_end == np).
    cfg.combine_holdout_frac = 0.0;
    cfg.combo_out            = combo_zero;
    auto r_zero = atx::impl::run_combine(cfg);
    ASSERT_TRUE(r_zero.has_value()) << r_zero.error().message();

    // Digest equality and byte equality vs the no-flag baseline.
    EXPECT_EQ(r_default->digest, r_zero->digest)
        << "holdout_frac 0 must be byte-identical to the default (no-flag) path";
    EXPECT_NE(r_default->digest, atx::u64{0});

    auto read_bytes = [](const std::string& path) -> std::vector<char> {
        std::ifstream f{path, std::ios::binary};
        return std::vector<char>{std::istreambuf_iterator<char>(f),
                                 std::istreambuf_iterator<char>()};
    };
    const auto b_default = read_bytes(combo_default);
    const auto b_zero    = read_bytes(combo_zero);
    EXPECT_FALSE(b_default.empty());
    EXPECT_EQ(b_default, b_zero) << "default and holdout-0 combo.bin must match byte-for-byte";

    // combo.meta records the no-OOS boundary: fit_end == np, holdout_begin == np.
    const auto meta = read_kv_file(combo_default + ".meta");
    EXPECT_EQ(meta.at("n_periods"),     std::to_string(np));
    EXPECT_EQ(meta.at("fit_end"),       std::to_string(np));
    EXPECT_EQ(meta.at("holdout_begin"), std::to_string(np));
    // StageResult exposes holdout_begin too.
    auto find_kv = [&](const atx::impl::StageResult& sr, const std::string& k) {
        for (const auto& p : sr.kvs) if (p.first == k) return p.second;
        return std::string{};
    };
    EXPECT_EQ(find_kv(*r_default, "holdout_begin"), std::to_string(np));

    std::error_code ec;
    fs::remove(panel_path, ec);
    fs::remove_all(alphas_dir, ec);
    for (const std::string& co : {combo_default, combo_zero}) {
        fs::remove(co, ec);
        fs::remove(co + ".weights.txt", ec);
        fs::remove(co + ".meta", ec);
    }
}

// ---------------------------------------------------------------------------
// A2a Test: HoldoutFracPositiveShrinksFitWindowAndWritesMeta
// With --holdout-frac 0.25 on a fixture with np periods, the fit window shrinks to
// [fit_begin, np - floor(0.25*np)) and combo.meta records that boundary:
// fit_end == np - floor(0.25*np), holdout_begin == fit_end, fit_end < np. combo.bin
// is still written, and the run is twice-run byte-identical (incl. combo.meta).
// ---------------------------------------------------------------------------
TEST(AtxImplCombine, HoldoutFracPositiveShrinksFitWindowAndWritesMeta) {
    namespace fs = std::filesystem;

    auto panel_opt = make_momentum_panel();
    ASSERT_TRUE(panel_opt.has_value());
    const Panel& panel = *panel_opt;
    const std::string panel_path = write_panel_tmp(panel, "holdoutpos");
    const usize np = panel.dates();
    const usize expected_oos_n  = static_cast<usize>(std::floor(0.25 * static_cast<f64>(np)));
    const usize expected_fitend = np - expected_oos_n;
    ASSERT_LT(expected_fitend, np) << "fixture must actually shrink the fit window";

    const std::string alphas_dir = write_alpha_dir("holdoutpos", safe_dsls());
    const std::string combo1 =
        (fs::temp_directory_path() / "atx_impl_combine_holdout_pos1.bin").string();
    const std::string combo2 =
        (fs::temp_directory_path() / "atx_impl_combine_holdout_pos2.bin").string();

    atx::impl::RunConfig cfg;
    cfg.subcommand           = "combine";
    cfg.panel                = panel_path;
    cfg.alphas               = alphas_dir;
    cfg.method               = "shrinkage-mv";
    cfg.fit_begin            = 0;
    cfg.fit_end              = 0;     // NOT explicitly set -> holdout governs
    cfg.combine_holdout_frac = 0.25;

    cfg.combo_out = combo1;
    auto r1 = atx::impl::run_combine(cfg);
    ASSERT_TRUE(r1.has_value()) << r1.error().message();
    cfg.combo_out = combo2;
    auto r2 = atx::impl::run_combine(cfg);
    ASSERT_TRUE(r2.has_value()) << r2.error().message();

    // combo.bin written and twice-run byte-identical.
    EXPECT_TRUE(fs::exists(combo1));
    EXPECT_EQ(r1->digest, r2->digest) << "holdout combine must be deterministic";
    EXPECT_NE(r1->digest, atx::u64{0});

    // combo.meta records the shrunk boundary (non-vacuous: exact value).
    const auto meta = read_kv_file(combo1 + ".meta");
    EXPECT_EQ(meta.at("n_periods"),     std::to_string(np));
    EXPECT_EQ(meta.at("fit_end"),       std::to_string(expected_fitend));
    EXPECT_EQ(meta.at("holdout_begin"), std::to_string(expected_fitend));
    EXPECT_LT(std::stoul(meta.at("fit_end")), np)
        << "holdout must shrink fit_end below n_periods";

    // combo.meta itself is twice-run identical.
    auto read_text = [](const std::string& p) {
        std::ifstream f{p};
        return std::string{std::istreambuf_iterator<char>(f),
                           std::istreambuf_iterator<char>()};
    };
    EXPECT_EQ(read_text(combo1 + ".meta"), read_text(combo2 + ".meta"))
        << "combo.meta must be deterministic across runs";

    // StageResult reports holdout_begin == fit_end.
    auto find_kv = [&](const atx::impl::StageResult& sr, const std::string& k) {
        for (const auto& p : sr.kvs) if (p.first == k) return p.second;
        return std::string{};
    };
    EXPECT_EQ(find_kv(*r1, "holdout_begin"), std::to_string(expected_fitend));
    EXPECT_EQ(find_kv(*r1, "fit_end"),       std::to_string(expected_fitend));

    std::error_code ec;
    fs::remove(panel_path, ec);
    fs::remove_all(alphas_dir, ec);
    for (const std::string& co : {combo1, combo2}) {
        fs::remove(co, ec);
        fs::remove(co + ".weights.txt", ec);
        fs::remove(co + ".meta", ec);
    }
}

// ---------------------------------------------------------------------------
// A2a Test: HoldoutFracTooLargeErrors
// A holdout fraction that leaves < 2 in-sample periods (or < 1 OOS period) must
// return Err(InvalidArgument). With np=96, holdout_frac 0.99 -> oos_n = 95 ->
// in-sample = 1 (< 2) -> error. (frac >= 1.0 -> oos_n == np -> 0 in-sample.)
// ---------------------------------------------------------------------------
TEST(AtxImplCombine, HoldoutFracTooLargeErrors) {
    namespace fs = std::filesystem;

    auto panel_opt = make_momentum_panel();
    ASSERT_TRUE(panel_opt.has_value());
    const Panel& panel = *panel_opt;
    const std::string panel_path = write_panel_tmp(panel, "holdoutbig");

    const std::string alphas_dir = write_alpha_dir("holdoutbig", safe_dsls());
    const std::string combo_out =
        (fs::temp_directory_path() / "atx_impl_combine_holdout_big.bin").string();

    atx::impl::RunConfig cfg;
    cfg.subcommand           = "combine";
    cfg.panel                = panel_path;
    cfg.alphas               = alphas_dir;
    cfg.combo_out            = combo_out;
    cfg.method               = "equal";
    cfg.combine_holdout_frac = 0.99; // leaves only 1 in-sample period of 96 -> error

    auto r = atx::impl::run_combine(cfg);
    EXPECT_FALSE(r.has_value()) << "holdout that starves the in-sample window must fail";
    if (!r.has_value()) {
        EXPECT_EQ(r.error().code(), atx::core::ErrorCode::InvalidArgument);
    }

    std::error_code ec;
    fs::remove(panel_path, ec);
    fs::remove_all(alphas_dir, ec);
    fs::remove(combo_out, ec);
    fs::remove(combo_out + ".weights.txt", ec);
    fs::remove(combo_out + ".meta", ec);
}

// ---------------------------------------------------------------------------
// A2a Fix-round-1 Test: HoldoutFracOutOfRangeRejected
// A --holdout-frac >= 1.0 value must be rejected at config-parse time with
// Err(InvalidArgument) — it must NOT silently degrade to a full-history fit.
// Two sub-cases: frac == 1.0 (exact boundary) and frac == 1.5 (well out of range).
// Both are tested via apply_flag (config-parse level) to verify the validation
// fires before run_combine is ever reached. The stage-level guard (oos_n >= np)
// is confirmed separately via run_combine with a programmatic cfg (bypassing parse)
// set to combine_holdout_frac = 1.5.
// ---------------------------------------------------------------------------
TEST(AtxImplCombine, HoldoutFracOutOfRangeRejected) {
    namespace fs = std::filesystem;

    // --- Config-parse level rejection (apply_flag path) ---
    // frac == 1.0: exact lower boundary of the out-of-range region -> Err.
    {
        atx::impl::RunConfig cfg{};
        const char* argv[] = {"atx", "combine",
                              "--holdout-frac", "1.0",
                              "--panel", "p.bin",
                              "--alphas", "/a",
                              "--combo-out", "/o.bin"};
        const int argc = static_cast<int>(sizeof(argv) / sizeof(argv[0]));
        auto r = atx::impl::parse_args(argc, const_cast<char**>(argv));
        EXPECT_FALSE(r.has_value())
            << "--holdout-frac 1.0 must be rejected at parse time";
        if (!r.has_value()) {
            EXPECT_EQ(r.error().code(), atx::core::ErrorCode::InvalidArgument)
                << "parse rejection must be InvalidArgument, got: "
                << r.error().message();
        }
    }

    // frac == 1.5: well above 1.0 -> also Err.
    {
        const char* argv[] = {"atx", "combine",
                              "--holdout-frac", "1.5",
                              "--panel", "p.bin",
                              "--alphas", "/a",
                              "--combo-out", "/o.bin"};
        const int argc = static_cast<int>(sizeof(argv) / sizeof(argv[0]));
        auto r = atx::impl::parse_args(argc, const_cast<char**>(argv));
        EXPECT_FALSE(r.has_value())
            << "--holdout-frac 1.5 must be rejected at parse time";
        if (!r.has_value()) {
            EXPECT_EQ(r.error().code(), atx::core::ErrorCode::InvalidArgument)
                << "parse rejection must be InvalidArgument, got: "
                << r.error().message();
        }
    }

    // --- Stage-level guard: programmatic cfg bypasses config parse ---
    // combine_holdout_frac = 1.5 injected directly -> the oos_n >= np guard must fire.
    {
        auto panel_opt = make_momentum_panel();
        ASSERT_TRUE(panel_opt.has_value());
        const Panel& panel = *panel_opt;
        const std::string panel_path = write_panel_tmp(panel, "oor_panel");
        const std::string alphas_dir = write_alpha_dir("oor_alphas", safe_dsls());
        const std::string combo_out =
            (fs::temp_directory_path() / "atx_impl_combine_holdout_oor.bin").string();

        atx::impl::RunConfig cfg;
        cfg.subcommand            = "combine";
        cfg.panel                 = panel_path;
        cfg.alphas                = alphas_dir;
        cfg.combo_out             = combo_out;
        cfg.method                = "equal";
        cfg.combine_holdout_frac  = 1.5; // bypass config parse — stage guard must catch it

        // Ensure no leftover from a prior run so the existence check is meaningful.
        { std::error_code ec0; fs::remove(combo_out, ec0); }

        auto r = atx::impl::run_combine(cfg);
        EXPECT_FALSE(r.has_value())
            << "run_combine with combine_holdout_frac=1.5 must return Err, not silently produce a full-history fit";
        if (!r.has_value()) {
            EXPECT_EQ(r.error().code(), atx::core::ErrorCode::InvalidArgument)
                << "stage guard must be InvalidArgument, got: " << r.error().message();
        }
        // combo.bin must NOT have been created (no misleading output).
        EXPECT_FALSE(fs::exists(combo_out))
            << "combo.bin must not be created when holdout_frac is out of range";

        std::error_code ec;
        fs::remove(panel_path, ec);
        fs::remove_all(alphas_dir, ec);
        fs::remove(combo_out, ec);
        fs::remove(combo_out + ".weights.txt", ec);
        fs::remove(combo_out + ".meta", ec);
    }
}

// ---------------------------------------------------------------------------
// D1.2 Test 1: ConvictionAbsentIsDefaultPath
// Asserts cfg.conviction defaults to false and that a run without --conviction
// produces the same combo.bin digest as the default path. The pre-existing
// DeterministicAcrossRuns test is the primary byte-identical proof; this test
// pins the field default and confirms the no-conviction path is unperturbed.
// ---------------------------------------------------------------------------
TEST(AtxImplCombine, ConvictionAbsentIsDefaultPath) {
    namespace fs = std::filesystem;

    // (a) cfg.conviction must default to false (field default).
    atx::impl::RunConfig default_cfg;
    EXPECT_FALSE(default_cfg.conviction)
        << "cfg.conviction must default to false";

    auto panel_opt = make_momentum_panel();
    ASSERT_TRUE(panel_opt.has_value());
    const Panel& panel = *panel_opt;
    const std::string panel_path = write_panel_tmp(panel, "cvt_absent");
    const std::string alphas_dir = write_alpha_dir("cvt_absent", safe_dsls());
    const std::string combo_default =
        (fs::temp_directory_path() / "atx_impl_combine_cvt_absent_default.bin").string();
    const std::string combo_false =
        (fs::temp_directory_path() / "atx_impl_combine_cvt_absent_false.bin").string();

    atx::impl::RunConfig cfg;
    cfg.subcommand = "combine";
    cfg.panel      = panel_path;
    cfg.alphas     = alphas_dir;
    cfg.method     = "shrinkage-mv";
    cfg.fit_begin  = 0;
    cfg.fit_end    = 0;

    // (b) Default (conviction not set) run.
    cfg.combo_out = combo_default;
    auto r_default = atx::impl::run_combine(cfg);
    ASSERT_TRUE(r_default.has_value()) << r_default.error().message();

    // (c) Explicit conviction == false run — must be byte-identical to (b).
    cfg.conviction = false;
    cfg.combo_out  = combo_false;
    auto r_false = atx::impl::run_combine(cfg);
    ASSERT_TRUE(r_false.has_value()) << r_false.error().message();

    EXPECT_EQ(r_default->digest, r_false->digest)
        << "explicit conviction=false must be byte-identical to the default (no-flag) path";
    EXPECT_NE(r_default->digest, atx::u64{0});

    auto read_bytes = [](const std::string& path) -> std::vector<char> {
        std::ifstream f{path, std::ios::binary};
        return std::vector<char>{std::istreambuf_iterator<char>(f),
                                 std::istreambuf_iterator<char>()};
    };
    EXPECT_EQ(read_bytes(combo_default), read_bytes(combo_false))
        << "combo.bin files must be byte-identical when conviction is absent vs false";

    std::error_code ec;
    fs::remove(panel_path, ec);
    fs::remove_all(alphas_dir, ec);
    for (const std::string& co : {combo_default, combo_false}) {
        fs::remove(co, ec);
        fs::remove(co + ".weights.txt", ec);
        fs::remove(co + ".meta", ec);
    }
}

// ---------------------------------------------------------------------------
// D1.2 Test 2: ConvictionScalesWeightsWhenEnabled
// Run combine TWICE on the same 3-alpha fixture: once with conviction=false,
// once with conviction=true. Assert:
//   (a) Both succeed.
//   (b) Σ|w| == 1 (within 1e-9) for the conviction run (renorm held).
//   (c) The conviction weight vector DIFFERS from the default vector
//       (the transform is non-vacuous on a fixture with 3 alphas of
//       clearly different PnL quality from the noisy-close panel).
// The fixture uses shrinkage-mv so the base weights are non-uniform.
// ---------------------------------------------------------------------------
TEST(AtxImplCombine, ConvictionScalesWeightsWhenEnabled) {
    namespace fs = std::filesystem;

    auto panel_opt = make_momentum_panel();
    ASSERT_TRUE(panel_opt.has_value());
    const Panel& panel = *panel_opt;
    const std::string panel_path = write_panel_tmp(panel, "cvt_scales");
    const std::string alphas_dir = write_alpha_dir("cvt_scales", safe_dsls());
    const std::string combo_off =
        (fs::temp_directory_path() / "atx_impl_combine_cvt_off.bin").string();
    const std::string combo_on =
        (fs::temp_directory_path() / "atx_impl_combine_cvt_on.bin").string();

    atx::impl::RunConfig cfg;
    cfg.subcommand = "combine";
    cfg.panel      = panel_path;
    cfg.alphas     = alphas_dir;
    cfg.method     = "shrinkage-mv";
    cfg.fit_begin  = 0;
    cfg.fit_end    = 0;

    // (a) conviction OFF.
    cfg.conviction = false;
    cfg.combo_out  = combo_off;
    auto r_off = atx::impl::run_combine(cfg);
    ASSERT_TRUE(r_off.has_value()) << r_off.error().message();

    // (b) conviction ON.
    cfg.conviction = true;
    cfg.combo_out  = combo_on;
    auto r_on = atx::impl::run_combine(cfg);
    ASSERT_TRUE(r_on.has_value()) << r_on.error().message();

    // Verify Σ|w| == 1 for the conviction run by reading the weights sidecar.
    // The sidecar format is "w[a]=<value> <label>\n".
    {
        const std::string wpath = combo_on + ".weights.txt";
        std::ifstream wf{wpath};
        ASSERT_TRUE(wf.is_open()) << "weights sidecar must exist for conviction run";
        f64 gross = 0.0;
        int n_weights = 0;
        std::string line;
        while (std::getline(wf, line)) {
            const std::string prefix = "w[";
            if (line.rfind(prefix, 0) == 0) {
                const auto eq = line.find('=');
                const auto sp = line.find(' ', eq);
                const std::string val_str = line.substr(eq + 1, sp - eq - 1);
                const f64 w = std::stod(val_str);
                gross += std::abs(w);
                ++n_weights;
            }
        }
        EXPECT_EQ(n_weights, 3) << "fixture has 3 alphas";
        // The sidecar uses default stream precision (~6 sig figs), so round-tripping
        // through text introduces ~1e-6 error. A tolerance of 1e-4 is tight enough
        // to confirm renorm ran (a non-renormed vector could easily be off by 0.3+)
        // while allowing for the text-serialization rounding.
        EXPECT_NEAR(gross, 1.0, 1e-4) << "Σ|w| must == 1 after conviction renorm";
    }

    // The conviction run must produce a DIFFERENT combined panel from the default.
    EXPECT_NE(r_on->digest, r_off->digest)
        << "conviction=true must change the combined panel vs the default (non-vacuous transform)";

    std::error_code ec;
    fs::remove(panel_path, ec);
    fs::remove_all(alphas_dir, ec);
    for (const std::string& co : {combo_off, combo_on}) {
        fs::remove(co, ec);
        fs::remove(co + ".weights.txt", ec);
        fs::remove(co + ".meta", ec);
    }
}

// ---------------------------------------------------------------------------
// D1.2 Test 3: ConvictionRunIsDeterministic
// Run combine --conviction TWICE on the same fixture -> byte-identical combo
// output (proves the conviction math is deterministic: no map iteration order,
// no UB, no NaN-ordering, no RNG).
// ---------------------------------------------------------------------------
TEST(AtxImplCombine, ConvictionRunIsDeterministic) {
    namespace fs = std::filesystem;

    auto panel_opt = make_momentum_panel();
    ASSERT_TRUE(panel_opt.has_value());
    const Panel& panel = *panel_opt;
    const std::string panel_path = write_panel_tmp(panel, "cvt_det");
    const std::string alphas_dir = write_alpha_dir("cvt_det", safe_dsls());
    const std::string combo1 =
        (fs::temp_directory_path() / "atx_impl_combine_cvt_det1.bin").string();
    const std::string combo2 =
        (fs::temp_directory_path() / "atx_impl_combine_cvt_det2.bin").string();

    atx::impl::RunConfig cfg;
    cfg.subcommand = "combine";
    cfg.panel      = panel_path;
    cfg.alphas     = alphas_dir;
    cfg.method     = "shrinkage-mv";
    cfg.fit_begin  = 0;
    cfg.fit_end    = 0;
    cfg.conviction = true;

    cfg.combo_out = combo1;
    auto r1 = atx::impl::run_combine(cfg);
    ASSERT_TRUE(r1.has_value()) << r1.error().message();

    cfg.combo_out = combo2;
    auto r2 = atx::impl::run_combine(cfg);
    ASSERT_TRUE(r2.has_value()) << r2.error().message();

    EXPECT_EQ(r1->digest, r2->digest)
        << "conviction combine must be deterministic (twice-run identical digest)";
    EXPECT_NE(r1->digest, atx::u64{0});

    auto read_bytes = [](const std::string& path) -> std::vector<char> {
        std::ifstream f{path, std::ios::binary};
        return std::vector<char>{std::istreambuf_iterator<char>(f),
                                 std::istreambuf_iterator<char>()};
    };
    const auto bytes1 = read_bytes(combo1);
    const auto bytes2 = read_bytes(combo2);
    EXPECT_FALSE(bytes1.empty()) << "conviction combo1 must not be empty";
    EXPECT_EQ(bytes1, bytes2) << "conviction combo.bin files must be byte-identical";

    std::error_code ec;
    fs::remove(panel_path, ec);
    fs::remove_all(alphas_dir, ec);
    for (const std::string& co : {combo1, combo2}) {
        fs::remove(co, ec);
        fs::remove(co + ".weights.txt", ec);
        fs::remove(co + ".meta", ec);
    }
}

// ---------------------------------------------------------------------------
// D3a Test 1: BreadthTelemetryEmitted
// Run combine on the 3-alpha fixture; assert sr.kvs contains the three breadth
// keys with finite, in-range values:
//   - breadth_effective_n: finite, in [0.0, na] (na = 3 alphas), >= 1.0 for >=2 alphas
//   - breadth_realized_ir: finite
//   - breadth_implied_ic:  finite
// This pins the "always-recorded" contract: no flag needed, always present in kvs.
// ---------------------------------------------------------------------------
TEST(AtxImplCombine, BreadthTelemetryEmitted) {
    namespace fs = std::filesystem;

    auto panel_opt = make_momentum_panel();
    ASSERT_TRUE(panel_opt.has_value());
    const Panel& panel = *panel_opt;
    const std::string panel_path = write_panel_tmp(panel, "breadth_emitted");
    const std::string alphas_dir = write_alpha_dir("breadth_emitted", safe_dsls());
    const std::string combo_out =
        (fs::temp_directory_path() / "atx_impl_combine_breadth_emitted.bin").string();

    atx::impl::RunConfig cfg;
    cfg.subcommand = "combine";
    cfg.panel      = panel_path;
    cfg.alphas     = alphas_dir;
    cfg.combo_out  = combo_out;
    cfg.method     = "shrinkage-mv";
    cfg.fit_begin  = 0;
    cfg.fit_end    = 0;

    auto r = atx::impl::run_combine(cfg);
    ASSERT_TRUE(r.has_value()) << r.error().message();

    const auto& kvs = r->kvs;
    auto find_kv = [&](const std::string& k) -> std::optional<std::string> {
        for (const auto& p : kvs) {
            if (p.first == k) return p.second;
        }
        return std::nullopt;
    };

    // All three breadth keys must be present.
    const auto eff_n_str = find_kv("breadth_effective_n");
    const auto ir_str    = find_kv("breadth_realized_ir");
    const auto ic_str    = find_kv("breadth_implied_ic");
    ASSERT_TRUE(eff_n_str.has_value()) << "breadth_effective_n missing from kvs";
    ASSERT_TRUE(ir_str.has_value())    << "breadth_realized_ir missing from kvs";
    ASSERT_TRUE(ic_str.has_value())    << "breadth_implied_ic missing from kvs";

    const f64 eff_n  = std::stod(*eff_n_str);
    const f64 ir     = std::stod(*ir_str);
    const f64 ic     = std::stod(*ic_str);

    // Print observed values (for D3a-report.md).
    std::cout << "[D3a] breadth_effective_n=" << eff_n
              << " breadth_realized_ir=" << ir
              << " breadth_implied_ic=" << ic << "\n";

    // effective_n must be finite and in [0, na=3].
    EXPECT_TRUE(std::isfinite(eff_n))  << "breadth_effective_n must be finite";
    EXPECT_GE(eff_n, 0.0)              << "breadth_effective_n must be >= 0";
    EXPECT_LE(eff_n, 3.0)              << "breadth_effective_n must be <= alpha count (3)";
    // For a 3-alpha fixture with distinct PnL streams, effective_n >= 1.
    EXPECT_GE(eff_n, 1.0)             << "breadth_effective_n must be >= 1 for >= 2 distinct alphas";

    // realized_ir and implied_ic must be finite.
    EXPECT_TRUE(std::isfinite(ir))  << "breadth_realized_ir must be finite";
    EXPECT_TRUE(std::isfinite(ic))  << "breadth_implied_ic must be finite";

    // Fundamental law consistency: if effective_n > 0 and ir is finite, ic = ir / sqrt(eff_n).
    // Tolerance 1e-4: std::to_string emits ~6 significant digits, so parsing the kvs string
    // introduces ~1e-6 relative error; the magnitudes here are O(10), giving ~1e-5 absolute
    // error — 1e-4 is a tight confirmation while allowing for serialization rounding.
    if (eff_n > 0.0 && std::isfinite(ir)) {
        const f64 expected_ic = ir / std::sqrt(eff_n);
        EXPECT_NEAR(ic, expected_ic, 1e-4) << "implied_ic must equal ir / sqrt(effective_n)";
    }

    std::error_code ec;
    fs::remove(panel_path, ec);
    fs::remove_all(alphas_dir, ec);
    fs::remove(combo_out, ec);
    fs::remove(combo_out + ".weights.txt", ec);
    fs::remove(combo_out + ".meta", ec);
}

// ---------------------------------------------------------------------------
// D3a Test 2: BreadthDeterministicAndDigestUnchanged
// Run the same combine config TWICE; assert:
//   (a) the three breadth values are BYTE-IDENTICAL across runs (same string),
//   (b) combo.bin (panel digest) is BYTE-IDENTICAL across runs.
// This proves the breadth telemetry is deterministic AND did not perturb the
// hashed artifact (the new numbers live only in kvs, not in combo.bin).
// ---------------------------------------------------------------------------
TEST(AtxImplCombine, BreadthDeterministicAndDigestUnchanged) {
    namespace fs = std::filesystem;

    auto panel_opt = make_momentum_panel();
    ASSERT_TRUE(panel_opt.has_value());
    const Panel& panel = *panel_opt;
    const std::string panel_path = write_panel_tmp(panel, "breadth_det");
    const std::string alphas_dir = write_alpha_dir("breadth_det", safe_dsls());
    const std::string combo1 =
        (fs::temp_directory_path() / "atx_impl_combine_breadth_det1.bin").string();
    const std::string combo2 =
        (fs::temp_directory_path() / "atx_impl_combine_breadth_det2.bin").string();

    atx::impl::RunConfig cfg;
    cfg.subcommand = "combine";
    cfg.panel      = panel_path;
    cfg.alphas     = alphas_dir;
    cfg.method     = "shrinkage-mv";
    cfg.fit_begin  = 0;
    cfg.fit_end    = 0;

    cfg.combo_out = combo1;
    auto r1 = atx::impl::run_combine(cfg);
    ASSERT_TRUE(r1.has_value()) << r1.error().message();

    cfg.combo_out = combo2;
    auto r2 = atx::impl::run_combine(cfg);
    ASSERT_TRUE(r2.has_value()) << r2.error().message();

    auto find_kv = [](const atx::impl::StageResult& sr, const std::string& k) -> std::string {
        for (const auto& p : sr.kvs) if (p.first == k) return p.second;
        return "";
    };

    // (a) Breadth values must be byte-identical across runs (same string representation).
    EXPECT_EQ(find_kv(*r1, "breadth_effective_n"), find_kv(*r2, "breadth_effective_n"))
        << "breadth_effective_n must be deterministic across runs";
    EXPECT_EQ(find_kv(*r1, "breadth_realized_ir"), find_kv(*r2, "breadth_realized_ir"))
        << "breadth_realized_ir must be deterministic across runs";
    EXPECT_EQ(find_kv(*r1, "breadth_implied_ic"), find_kv(*r2, "breadth_implied_ic"))
        << "breadth_implied_ic must be deterministic across runs";

    // (b) combo.bin digest and file bytes must be byte-identical (breadth did not perturb it).
    EXPECT_EQ(r1->digest, r2->digest) << "combo.bin digest must be identical across runs";
    EXPECT_NE(r1->digest, atx::u64{0}) << "digest must be non-zero";

    auto read_bytes = [](const std::string& path) -> std::vector<char> {
        std::ifstream f{path, std::ios::binary};
        return std::vector<char>{std::istreambuf_iterator<char>(f),
                                 std::istreambuf_iterator<char>()};
    };
    const auto bytes1 = read_bytes(combo1);
    const auto bytes2 = read_bytes(combo2);
    EXPECT_FALSE(bytes1.empty()) << "combo1 must not be empty";
    EXPECT_EQ(bytes1, bytes2)    << "combo.bin files must be byte-identical across runs";

    std::error_code ec;
    fs::remove(panel_path, ec);
    fs::remove_all(alphas_dir, ec);
    for (const std::string& co : {combo1, combo2}) {
        fs::remove(co, ec);
        fs::remove(co + ".weights.txt", ec);
        fs::remove(co + ".meta", ec);
    }
}

// ---------------------------------------------------------------------------
// D3b Test 1: WalkForwardOffIsByteIdentical
// combine WITHOUT --walk-forward (cfg.walk_forward default 0): assert combo.bin
// bytes are identical to the baseline run AND no walk_forward_* keys are present.
// Also assert cfg.walk_forward defaults to 0.
// ---------------------------------------------------------------------------
TEST(AtxImplCombine, WalkForwardOffIsByteIdentical) {
    namespace fs = std::filesystem;

    // (a) cfg.walk_forward must default to 0 (field default).
    atx::impl::RunConfig default_cfg;
    EXPECT_EQ(default_cfg.walk_forward, 0L) << "cfg.walk_forward must default to 0";

    auto panel_opt = make_momentum_panel();
    ASSERT_TRUE(panel_opt.has_value());
    const Panel& panel = *panel_opt;
    const std::string panel_path  = write_panel_tmp(panel, "wf_off");
    const std::string alphas_dir  = write_alpha_dir("wf_off", safe_dsls());
    const std::string combo_base  =
        (fs::temp_directory_path() / "atx_impl_combine_wf_off_base.bin").string();
    const std::string combo_zero  =
        (fs::temp_directory_path() / "atx_impl_combine_wf_off_zero.bin").string();

    atx::impl::RunConfig cfg;
    cfg.subcommand = "combine";
    cfg.panel      = panel_path;
    cfg.alphas     = alphas_dir;
    cfg.method     = "shrinkage-mv";
    cfg.fit_begin  = 0;
    cfg.fit_end    = 0;

    // (b) Baseline: walk_forward not set (stays 0 by default).
    cfg.combo_out = combo_base;
    auto r_base = atx::impl::run_combine(cfg);
    ASSERT_TRUE(r_base.has_value()) << r_base.error().message();

    // (c) Explicit walk_forward == 0 — must be byte-identical to baseline.
    cfg.walk_forward = 0;
    cfg.combo_out    = combo_zero;
    auto r_zero = atx::impl::run_combine(cfg);
    ASSERT_TRUE(r_zero.has_value()) << r_zero.error().message();

    // Digest equality + byte-identity.
    EXPECT_EQ(r_base->digest, r_zero->digest)
        << "walk_forward=0 must be byte-identical to the default (no-flag) path";
    EXPECT_NE(r_base->digest, atx::u64{0});

    auto read_bytes = [](const std::string& path) -> std::vector<char> {
        std::ifstream f{path, std::ios::binary};
        return std::vector<char>{std::istreambuf_iterator<char>(f),
                                 std::istreambuf_iterator<char>()};
    };
    EXPECT_EQ(read_bytes(combo_base), read_bytes(combo_zero))
        << "combo.bin files must be byte-identical when walk_forward is absent vs 0";

    // No walk_forward_* keys in default path kvs.
    auto has_kv = [](const atx::impl::StageResult& sr, const std::string& k) {
        for (const auto& p : sr.kvs) if (p.first == k) return true;
        return false;
    };
    EXPECT_FALSE(has_kv(*r_base, "walk_forward_folds"))
        << "walk_forward_folds must NOT appear in default (k==0) path";
    EXPECT_FALSE(has_kv(*r_base, "walk_forward_oos_sharpe_mean"))
        << "walk_forward_oos_sharpe_mean must NOT appear in default (k==0) path";
    EXPECT_FALSE(has_kv(*r_base, "walk_forward_oos_sharpe"))
        << "walk_forward_oos_sharpe must NOT appear in default (k==0) path";

    std::error_code ec;
    fs::remove(panel_path, ec);
    fs::remove_all(alphas_dir, ec);
    for (const std::string& co : {combo_base, combo_zero}) {
        fs::remove(co, ec);
        fs::remove(co + ".weights.txt", ec);
        fs::remove(co + ".meta", ec);
    }
}

// ---------------------------------------------------------------------------
// D3b Test 2: WalkForwardRecordsOosFolds
// combine with cfg.walk_forward = 2 on a 96-period fixture (span=96, seg=32,
// so 2 folds with >= 2 test periods each). Assert:
//   (a) kvs has walk_forward_folds == "2".
//   (b) walk_forward_oos_sharpe_mean is finite.
//   (c) walk_forward_oos_sharpe parses to 2 finite values.
//   (d) combo.bin is BYTE-IDENTICAL to the same run with WF off (harness must
//       not change the shipped weights).
// ---------------------------------------------------------------------------
TEST(AtxImplCombine, WalkForwardRecordsOosFolds) {
    namespace fs = std::filesystem;

    auto panel_opt = make_momentum_panel(); // 96x6
    ASSERT_TRUE(panel_opt.has_value());
    const Panel& panel = *panel_opt;
    const std::string panel_path = write_panel_tmp(panel, "wf_folds");
    const std::string alphas_dir = write_alpha_dir("wf_folds", safe_dsls());
    const std::string combo_wf_off =
        (fs::temp_directory_path() / "atx_impl_combine_wf_folds_off.bin").string();
    const std::string combo_wf_on =
        (fs::temp_directory_path() / "atx_impl_combine_wf_folds_on.bin").string();

    atx::impl::RunConfig cfg;
    cfg.subcommand = "combine";
    cfg.panel      = panel_path;
    cfg.alphas     = alphas_dir;
    cfg.method     = "shrinkage-mv";
    cfg.fit_begin  = 0;
    cfg.fit_end    = 0;

    // Run without WF (baseline combo.bin).
    cfg.walk_forward = 0;
    cfg.combo_out    = combo_wf_off;
    auto r_off = atx::impl::run_combine(cfg);
    ASSERT_TRUE(r_off.has_value()) << r_off.error().message();

    // Run with k=2 (fixture np=96, fit_begin=0, span=96, K+1=3, seg=32 >= 2).
    cfg.walk_forward = 2;
    cfg.combo_out    = combo_wf_on;
    auto r_on = atx::impl::run_combine(cfg);
    ASSERT_TRUE(r_on.has_value()) << r_on.error().message();

    auto find_kv = [](const atx::impl::StageResult& sr, const std::string& k) -> std::optional<std::string> {
        for (const auto& p : sr.kvs) if (p.first == k) return p.second;
        return std::nullopt;
    };

    // (a) walk_forward_folds == "2".
    const auto folds_str = find_kv(*r_on, "walk_forward_folds");
    ASSERT_TRUE(folds_str.has_value()) << "walk_forward_folds missing from kvs";
    EXPECT_EQ(*folds_str, "2");

    // (b) walk_forward_oos_sharpe_mean is finite.
    const auto mean_str = find_kv(*r_on, "walk_forward_oos_sharpe_mean");
    ASSERT_TRUE(mean_str.has_value()) << "walk_forward_oos_sharpe_mean missing from kvs";
    const f64 mean_val = std::stod(*mean_str);
    EXPECT_TRUE(std::isfinite(mean_val)) << "walk_forward_oos_sharpe_mean must be finite";

    // (c) walk_forward_oos_sharpe parses to 2 finite values.
    const auto sharpes_str = find_kv(*r_on, "walk_forward_oos_sharpe");
    ASSERT_TRUE(sharpes_str.has_value()) << "walk_forward_oos_sharpe missing from kvs";
    // Parse comma-separated.
    {
        std::vector<f64> parsed;
        std::string tok;
        std::istringstream ss(*sharpes_str);
        while (std::getline(ss, tok, ',')) {
            parsed.push_back(std::stod(tok));
        }
        EXPECT_EQ(parsed.size(), 2u) << "walk_forward_oos_sharpe must have 2 values for k=2";
        for (usize i = 0; i < parsed.size(); ++i) {
            EXPECT_TRUE(std::isfinite(parsed[i]))
                << "fold " << i << " OOS Sharpe must be finite, got " << parsed[i];
        }
        // Print observed values for report.
        std::cout << "[D3b] walk_forward k=2 fold_sharpes=" << *sharpes_str
                  << " mean=" << *mean_str << "\n";
    }

    // (d) combo.bin BYTE-IDENTICAL to WF-off run (shipped weights unchanged).
    EXPECT_EQ(r_on->digest, r_off->digest)
        << "combo.bin digest must be IDENTICAL with WF on vs off (shipped weights unchanged)";

    auto read_bytes = [](const std::string& path) -> std::vector<char> {
        std::ifstream f{path, std::ios::binary};
        return std::vector<char>{std::istreambuf_iterator<char>(f),
                                 std::istreambuf_iterator<char>()};
    };
    EXPECT_EQ(read_bytes(combo_wf_on), read_bytes(combo_wf_off))
        << "combo.bin must be byte-identical WF-on vs WF-off";

    std::error_code ec;
    fs::remove(panel_path, ec);
    fs::remove_all(alphas_dir, ec);
    for (const std::string& co : {combo_wf_off, combo_wf_on}) {
        fs::remove(co, ec);
        fs::remove(co + ".weights.txt", ec);
        fs::remove(co + ".meta", ec);
    }
}

// ---------------------------------------------------------------------------
// D3b Test 3: WalkForwardDeterministic
// Run cfg.walk_forward = 2 TWICE -> identical WF telemetry strings AND identical
// combo.bin (proves WF scoring is pure / no RNG, and the shipped combo is stable).
// ---------------------------------------------------------------------------
TEST(AtxImplCombine, WalkForwardDeterministic) {
    namespace fs = std::filesystem;

    auto panel_opt = make_momentum_panel(); // 96x6
    ASSERT_TRUE(panel_opt.has_value());
    const Panel& panel = *panel_opt;
    const std::string panel_path = write_panel_tmp(panel, "wf_det");
    const std::string alphas_dir = write_alpha_dir("wf_det", safe_dsls());
    const std::string combo1 =
        (fs::temp_directory_path() / "atx_impl_combine_wf_det1.bin").string();
    const std::string combo2 =
        (fs::temp_directory_path() / "atx_impl_combine_wf_det2.bin").string();

    atx::impl::RunConfig cfg;
    cfg.subcommand   = "combine";
    cfg.panel        = panel_path;
    cfg.alphas       = alphas_dir;
    cfg.method       = "shrinkage-mv";
    cfg.fit_begin    = 0;
    cfg.fit_end      = 0;
    cfg.walk_forward = 2;

    cfg.combo_out = combo1;
    auto r1 = atx::impl::run_combine(cfg);
    ASSERT_TRUE(r1.has_value()) << r1.error().message();

    cfg.combo_out = combo2;
    auto r2 = atx::impl::run_combine(cfg);
    ASSERT_TRUE(r2.has_value()) << r2.error().message();

    auto find_kv = [](const atx::impl::StageResult& sr, const std::string& k) -> std::string {
        for (const auto& p : sr.kvs) if (p.first == k) return p.second;
        return "";
    };

    // WF telemetry must be byte-identical across runs (same string representation).
    EXPECT_EQ(find_kv(*r1, "walk_forward_folds"),           find_kv(*r2, "walk_forward_folds"))
        << "walk_forward_folds must be deterministic across runs";
    EXPECT_EQ(find_kv(*r1, "walk_forward_oos_sharpe_mean"), find_kv(*r2, "walk_forward_oos_sharpe_mean"))
        << "walk_forward_oos_sharpe_mean must be deterministic across runs";
    EXPECT_EQ(find_kv(*r1, "walk_forward_oos_sharpe"),      find_kv(*r2, "walk_forward_oos_sharpe"))
        << "walk_forward_oos_sharpe must be deterministic across runs";

    // combo.bin must be identical (the WF loop doesn't touch shipped weights).
    EXPECT_EQ(r1->digest, r2->digest)
        << "combo.bin digest must be deterministic across WF runs";
    EXPECT_NE(r1->digest, atx::u64{0});

    auto read_bytes = [](const std::string& path) -> std::vector<char> {
        std::ifstream f{path, std::ios::binary};
        return std::vector<char>{std::istreambuf_iterator<char>(f),
                                 std::istreambuf_iterator<char>()};
    };
    const auto bytes1 = read_bytes(combo1);
    const auto bytes2 = read_bytes(combo2);
    EXPECT_FALSE(bytes1.empty()) << "combo1 must not be empty";
    EXPECT_EQ(bytes1, bytes2)    << "combo.bin must be byte-identical across WF runs";

    std::error_code ec;
    fs::remove(panel_path, ec);
    fs::remove_all(alphas_dir, ec);
    for (const std::string& co : {combo1, combo2}) {
        fs::remove(co, ec);
        fs::remove(co + ".weights.txt", ec);
        fs::remove(co + ".meta", ec);
    }
}

// ---------------------------------------------------------------------------
// T7 NEW-1 Test 1: WalkForwardConvictionReflectsShippedBook
// With BOTH --walk-forward and --conviction set, each WF fold must re-apply the
// per-fold conviction transform (over the fold's TRAIN window) before scoring
// the OOS Sharpe — so walk_forward_oos_sharpe becomes an HONEST OOS estimator of
// the shipped (conviction-weighted) book, NOT the base combiner fit. Assert:
//   (a) walk_forward_oos_sharpe DIFFERS from the base-fit WF telemetry (same
//       panel/flags MINUS --conviction) — proving the per-fold conviction
//       transform is actually applied in the WF loop.
//   (b) combo.bin (shipped weights) is BYTE-IDENTICAL to the conviction-on /
//       WF-off run — the WF harness never touches the shipped book regardless of
//       conviction. This pins NEW-1's determinism guarantee: WF only changes the
//       walk_forward_oos_sharpe* TELEMETRY, never the shipped combo.bin.
// Fixture: make_momentum_panel (96x6), k=2 (span=96, seg=32, 2 folds).
// ---------------------------------------------------------------------------
TEST(AtxImplCombine, WalkForwardConvictionReflectsShippedBook) {
    namespace fs = std::filesystem;

    auto panel_opt = make_momentum_panel(); // 96x6
    ASSERT_TRUE(panel_opt.has_value());
    const Panel& panel = *panel_opt;
    const std::string panel_path = write_panel_tmp(panel, "wf_cvt");
    const std::string alphas_dir = write_alpha_dir("wf_cvt", safe_dsls());
    // combo.bin sinks: WF+conviction, conviction-only (WF off), base WF (no conviction).
    const std::string combo_wf_cvt =
        (fs::temp_directory_path() / "atx_impl_combine_wf_cvt_on.bin").string();
    const std::string combo_cvt_only =
        (fs::temp_directory_path() / "atx_impl_combine_wf_cvt_cvtonly.bin").string();
    const std::string combo_wf_base =
        (fs::temp_directory_path() / "atx_impl_combine_wf_cvt_base.bin").string();

    atx::impl::RunConfig cfg;
    cfg.subcommand = "combine";
    cfg.panel      = panel_path;
    cfg.alphas     = alphas_dir;
    cfg.method     = "shrinkage-mv";
    cfg.fit_begin  = 0;
    cfg.fit_end    = 0;

    // (1) Base WF: --walk-forward 2, conviction OFF (today's base-fit folds).
    cfg.walk_forward = 2;
    cfg.conviction   = false;
    cfg.combo_out    = combo_wf_base;
    auto r_wf_base = atx::impl::run_combine(cfg);
    ASSERT_TRUE(r_wf_base.has_value()) << r_wf_base.error().message();

    // (2) WF + conviction: --walk-forward 2 AND --conviction (per-fold conviction).
    cfg.walk_forward = 2;
    cfg.conviction   = true;
    cfg.combo_out    = combo_wf_cvt;
    auto r_wf_cvt = atx::impl::run_combine(cfg);
    ASSERT_TRUE(r_wf_cvt.has_value()) << r_wf_cvt.error().message();

    // (3) Conviction only (WF off): the shipped conviction book without the WF harness.
    cfg.walk_forward = 0;
    cfg.conviction   = true;
    cfg.combo_out    = combo_cvt_only;
    auto r_cvt_only = atx::impl::run_combine(cfg);
    ASSERT_TRUE(r_cvt_only.has_value()) << r_cvt_only.error().message();

    auto find_kv = [](const atx::impl::StageResult& sr, const std::string& k) -> std::optional<std::string> {
        for (const auto& p : sr.kvs) if (p.first == k) return p.second;
        return std::nullopt;
    };

    // (a) walk_forward_oos_sharpe DIFFERS between conviction-on and base-fit WF.
    const auto sharpes_base = find_kv(*r_wf_base, "walk_forward_oos_sharpe");
    const auto sharpes_cvt  = find_kv(*r_wf_cvt,  "walk_forward_oos_sharpe");
    ASSERT_TRUE(sharpes_base.has_value()) << "base WF walk_forward_oos_sharpe missing";
    ASSERT_TRUE(sharpes_cvt.has_value())  << "conviction WF walk_forward_oos_sharpe missing";
    EXPECT_NE(*sharpes_base, *sharpes_cvt)
        << "walk_forward_oos_sharpe must DIFFER when --conviction is on (per-fold conviction "
           "applied), proving WF reflects the shipped book. base=" << *sharpes_base
        << " conviction=" << *sharpes_cvt;
    std::cout << "[T7 NEW-1] base_wf_sharpes=" << *sharpes_base
              << " conviction_wf_sharpes=" << *sharpes_cvt << "\n";

    // (b) combo.bin BYTE-IDENTICAL: WF+conviction shipped book == conviction-only book.
    //     The WF harness must NOT touch the shipped combo.bin even with conviction on.
    EXPECT_EQ(r_wf_cvt->digest, r_cvt_only->digest)
        << "combo.bin digest must be IDENTICAL for WF+conviction vs conviction-only "
           "(WF never ships its scratch folds)";
    EXPECT_NE(r_wf_cvt->digest, atx::u64{0});

    auto read_bytes = [](const std::string& path) -> std::vector<char> {
        std::ifstream f{path, std::ios::binary};
        return std::vector<char>{std::istreambuf_iterator<char>(f),
                                 std::istreambuf_iterator<char>()};
    };
    EXPECT_EQ(read_bytes(combo_wf_cvt), read_bytes(combo_cvt_only))
        << "combo.bin must be byte-identical for WF+conviction vs conviction-only";

    std::error_code ec;
    fs::remove(panel_path, ec);
    fs::remove_all(alphas_dir, ec);
    for (const std::string& co : {combo_wf_cvt, combo_cvt_only, combo_wf_base}) {
        fs::remove(co, ec);
        fs::remove(co + ".weights.txt", ec);
        fs::remove(co + ".meta", ec);
    }
}

// ===========================================================================
// T6 — real per-alpha capacity wired into the combine de-correlation step.
//
// Today --capacity-floor was a documented NO-OP (a constant-1.0 capacity vector
// went into decorrelate_weights, so cap_scale was uniform and washed out by the
// combined book's gross-normalization). T6 computes a REAL per-alpha capacity
// AUM (the $-AUM at which that alpha's last-period book's √-impact erodes its
// gross edge to zero, under the engine DEFAULT impact model — NOT frictionless)
// and passes it in, so a positive --capacity-floor (in $-AUM) fades the alphas
// whose capacity AUM falls below the floor.
//
// These tests prove (a) the default + corr-only paths stay byte-identical (the
// existing CorrPenalty* tests already pin the close-only fixture; these add the
// {close,volume} fixture), and (b) the capacity path is NON-DEGENERATE: the
// per-alpha capacity AUM is FINITE and VARIES across alphas (so the impact sim
// is real, not frictionless), the combo.bin DIFFERS from capacity-off, the
// illiquid-heavy alpha is faded to a smaller weight, and the path is
// twice-run deterministic.
// ===========================================================================

// Build a {close, volume} panel where instruments span a WIDE liquidity range:
// instrument 0 is highly liquid (huge volume), instrument I-1 is thin (tiny
// volume). This is what makes per-alpha capacity vary: an alpha that concentrates
// weight on the thin names has a far lower capacity AUM than one on the liquid
// names. Returns close (momentum walk) + a per-instrument constant-ish volume.
static std::optional<Panel> make_liquidity_panel(usize dates = 96, usize insts = 6) {
    const std::vector<f64> close = noisy_close(dates, insts, 0xDEADBEEFULL);
    // Volume decays geometrically by instrument index: name 0 trades ~1e9
    // shares/day, the last name ~1e9 / 4^(I-1) — a multi-decade liquidity spread
    // so the per-alpha capacity AUM is clearly different across names.
    std::vector<f64> volume(dates * insts);
    for (usize t = 0; t < dates; ++t) {
        for (usize j = 0; j < insts; ++j) {
            const f64 base = 1.0e9 / std::pow(4.0, static_cast<f64>(j));
            // A mild deterministic per-day wobble keeps ADV well-defined and finite
            // without changing the per-name ordering.
            volume[t * insts + j] = base * (1.0 + 0.01 * static_cast<f64>((t + j) % 5));
        }
    }
    return make_panel(dates, insts, {"close", "volume"}, {close, volume});
}

// Parse the comma-joined per-alpha capacity telemetry kv into a vector of doubles.
// "+inf"/"inf" parse to +infinity (so the test can assert finiteness explicitly).
static std::vector<f64> parse_capacity_csv(const std::string& csv) {
    std::vector<f64> out;
    std::stringstream ss{csv};
    std::string tok;
    while (std::getline(ss, tok, ',')) {
        if (tok.empty()) continue;
        if (tok == "inf" || tok == "+inf") {
            out.push_back(std::numeric_limits<f64>::infinity());
        } else {
            out.push_back(std::stod(tok));
        }
    }
    return out;
}

static std::string find_kv_combine(const atx::impl::StageResult& sr, const std::string& k) {
    for (const auto& p : sr.kvs) if (p.first == k) return p.second;
    return "";
}

// ---------------------------------------------------------------------------
// T6 Test 1: CapacityFloorZeroOnVolumePanelIsByteIdenticalToDefault
// On the {close,volume} fixture, --corr-penalty 1.0 --capacity-floor 0 (capacity
// OFF) must be byte-identical to the same run with --capacity-floor unset. The
// gate requires capacity_floor>0 for real capacity, so floor==0 keeps the
// constant-1.0 stub and the corr-only path is unchanged. This pins "wiring real
// capacity does not perturb the corr-only path".
// ---------------------------------------------------------------------------
TEST(AtxImplCombine, CapacityFloorZeroOnVolumePanelIsByteIdenticalToDefault) {
    namespace fs = std::filesystem;

    auto panel_opt = make_liquidity_panel();
    ASSERT_TRUE(panel_opt.has_value());
    const Panel& panel = *panel_opt;
    const std::string panel_path = write_panel_tmp(panel, "cap_corr0");
    const std::string alphas_dir = write_alpha_dir("cap_corr0", safe_dsls());
    const std::string combo_default =
        (fs::temp_directory_path() / "atx_impl_combine_cap_corr_default.bin").string();
    const std::string combo_floor0 =
        (fs::temp_directory_path() / "atx_impl_combine_cap_corr_floor0.bin").string();

    atx::impl::RunConfig cfg;
    cfg.subcommand = "combine";
    cfg.panel      = panel_path;
    cfg.alphas     = alphas_dir;
    cfg.method     = "shrinkage-mv";
    cfg.fit_begin  = 0;
    cfg.fit_end    = 0;

    // (a) corr-only, capacity_floor unset (stays 0.0 by default).
    cfg.corr_penalty = 1.0;
    cfg.combo_out    = combo_default;
    auto r_default = atx::impl::run_combine(cfg);
    ASSERT_TRUE(r_default.has_value()) << r_default.error().message();

    // (b) corr-only, capacity_floor EXPLICITLY 0, target_aum set but unused.
    cfg.capacity_floor = 0.0;
    cfg.target_aum     = 1.0e9; // present but inert: the gate requires floor>0 too
    cfg.combo_out      = combo_floor0;
    auto r_floor0 = atx::impl::run_combine(cfg);
    ASSERT_TRUE(r_floor0.has_value()) << r_floor0.error().message();

    EXPECT_EQ(r_default->digest, r_floor0->digest)
        << "capacity_floor 0 must be byte-identical to the corr-only path (capacity unused)";
    EXPECT_NE(r_default->digest, atx::u64{0});

    auto read_bytes = [](const std::string& path) -> std::vector<char> {
        std::ifstream f{path, std::ios::binary};
        return std::vector<char>{std::istreambuf_iterator<char>(f),
                                 std::istreambuf_iterator<char>()};
    };
    EXPECT_EQ(read_bytes(combo_default), read_bytes(combo_floor0))
        << "capacity_floor-0 combo.bin must match the corr-only path byte-for-byte";

    // No capacity telemetry on the capacity-off path (additive-only contract).
    EXPECT_EQ(find_kv_combine(*r_default, "capacity_min_alpha_aum"), "");
    EXPECT_EQ(find_kv_combine(*r_floor0, "capacity_min_alpha_aum"), "");

    std::error_code ec;
    fs::remove(panel_path, ec);
    fs::remove_all(alphas_dir, ec);
    for (const std::string& co : {combo_default, combo_floor0}) {
        fs::remove(co, ec);
        fs::remove(co + ".weights.txt", ec);
        fs::remove(co + ".meta", ec);
    }
}

// ---------------------------------------------------------------------------
// T6 Test 2 (LOAD-BEARING): CapacityActivatesAndIsNonDegenerate
// With --capacity-floor>0 --target-aum>0 on the wide-liquidity {close,volume}
// fixture, the computed per-alpha capacity AUM must be (a) FINITE for every alpha
// (proves the impact sim is REAL — a frictionless Y=0 sim would yield +inf for
// all) and (b) VARY across alphas (min != max — proves the per-name liquidity
// arithmetic actually bites). The capacity-on combo.bin must DIFFER from the
// capacity-off run, AND the alpha with the SMALLEST capacity (most illiquid-heavy)
// must receive a SMALLER |weight| than it does with capacity off (the fade). This
// is the test that proves the D3c no-op trap is avoided.
// ---------------------------------------------------------------------------
TEST(AtxImplCombine, CapacityActivatesAndIsNonDegenerate) {
    namespace fs = std::filesystem;

    auto panel_opt = make_liquidity_panel();
    ASSERT_TRUE(panel_opt.has_value());
    const Panel& panel = *panel_opt;
    const std::string panel_path = write_panel_tmp(panel, "cap_active");
    const std::string alphas_dir = write_alpha_dir("cap_active", safe_dsls());
    const std::string combo_off =
        (fs::temp_directory_path() / "atx_impl_combine_cap_off.bin").string();
    const std::string combo_on =
        (fs::temp_directory_path() / "atx_impl_combine_cap_on.bin").string();

    // Helper to read the per-alpha |weights| from the sidecar (in AlphaId order).
    auto read_abs_weights = [](const std::string& combo) -> std::vector<f64> {
        std::vector<f64> w;
        std::ifstream wf{combo + ".weights.txt"};
        std::string line;
        while (std::getline(wf, line)) {
            if (line.rfind("w[", 0) == 0) {
                const auto eq = line.find('=');
                const auto sp = line.find(' ', eq);
                w.push_back(std::abs(std::stod(line.substr(eq + 1, sp - eq - 1))));
            }
        }
        return w;
    };

    atx::impl::RunConfig cfg;
    cfg.subcommand = "combine";
    cfg.panel      = panel_path;
    cfg.alphas     = alphas_dir;
    cfg.method     = "shrinkage-mv";
    cfg.fit_begin  = 0;
    cfg.fit_end    = 0;

    // (a) capacity OFF (no knobs) — the baseline book.
    cfg.combo_out = combo_off;
    auto r_off = atx::impl::run_combine(cfg);
    ASSERT_TRUE(r_off.has_value()) << r_off.error().message();
    const std::vector<f64> w_off = read_abs_weights(combo_off);

    // (b) PROBE run: capacity ON with a tiny floor so the telemetry is emitted but
    // (since every alpha's capacity AUM >> the tiny floor) NO alpha is faded — we
    // only read the per-alpha capacities here to CALIBRATE the real floor below.
    // This makes the test robust to the fixture's exact dollar magnitudes: we do
    // not hand-pick a floor, we derive it from the observed capacity distribution.
    const std::string combo_probe =
        (fs::temp_directory_path() / "atx_impl_combine_cap_probe.bin").string();
    cfg.target_aum     = 1.0e8;
    cfg.capacity_floor = 1.0; // $1 floor: telemetry emitted; no meaningful fade
    cfg.combo_out      = combo_probe;
    auto r_probe = atx::impl::run_combine(cfg);
    ASSERT_TRUE(r_probe.has_value()) << r_probe.error().message();

    // ---- (1) Per-alpha capacity is FINITE and VARIES (the non-degeneracy proof). ----
    const std::string cap_csv = find_kv_combine(*r_probe, "capacity_alpha_aum");
    ASSERT_FALSE(cap_csv.empty())
        << "capacity-on run must emit per-alpha capacity telemetry (capacity_alpha_aum)";
    const std::vector<f64> caps = parse_capacity_csv(cap_csv);
    ASSERT_EQ(caps.size(), 3u) << "fixture has 3 alphas";
    for (usize a = 0; a < caps.size(); ++a) {
        EXPECT_TRUE(std::isfinite(caps[a]))
            << "alpha " << a << " capacity must be FINITE (proves a real impact sim, "
            << "not frictionless Y=0 which yields +inf): got " << caps[a];
        EXPECT_GT(caps[a], 0.0) << "alpha " << a << " capacity must be positive";
    }
    const f64 cmin = *std::min_element(caps.begin(), caps.end());
    const f64 cmax = *std::max_element(caps.begin(), caps.end());
    EXPECT_GT(cmax - cmin, 0.0)
        << "per-alpha capacity must VARY across alphas (min=" << cmin << " max=" << cmax
        << ") — a uniform capacity is the washed-out no-op T6 exists to avoid";

    // (c) REAL capacity run: set the floor to the MIDPOINT of [cmin, cmax] so the
    // min-capacity alpha is strictly BELOW the floor (faded) while the max-capacity
    // alpha is strictly AT/ABOVE it (held) — a guaranteed non-degenerate fade
    // regardless of the absolute dollar scale.
    const f64 calibrated_floor = 0.5 * (cmin + cmax);
    cfg.capacity_floor = calibrated_floor;
    cfg.combo_out      = combo_on;
    auto r_on = atx::impl::run_combine(cfg);
    ASSERT_TRUE(r_on.has_value()) << r_on.error().message();
    const std::vector<f64> w_on = read_abs_weights(combo_on);

    // ---- (2) The combined book must CHANGE vs capacity off. ----
    EXPECT_NE(r_on->digest, r_off->digest)
        << "capacity-floor>0 with real per-alpha capacity must change the combined book";

    // ---- (3) The lowest-capacity alpha is faded (smaller |weight| than off). ----
    // Identify the alpha with the smallest capacity AUM; under a floor above it,
    // its cap_scale < 1 so its |weight| must shrink relative to the off run. (A
    // >=-floor alpha holds full size, so the fade is the discriminating signal.)
    const usize a_min =
        static_cast<usize>(std::min_element(caps.begin(), caps.end()) - caps.begin());
    ASSERT_LT(caps[a_min], calibrated_floor)
        << "the midpoint floor must put the min-capacity alpha BELOW it so it is faded";
    EXPECT_LT(w_on[a_min], w_off[a_min] - 1e-12)
        << "the lowest-capacity alpha (idx " << a_min << ", cap=" << caps[a_min]
        << ") must be faded to a smaller |weight| under the capacity floor ("
        << w_on[a_min] << " < " << w_off[a_min] << ")";

    // ---- (4) Book-level capacity telemetry present and sane. ----
    const std::string min_aum = find_kv_combine(*r_on, "capacity_min_alpha_aum");
    EXPECT_FALSE(min_aum.empty()) << "capacity_min_alpha_aum kv must be emitted";
    EXPECT_NEAR(std::stod(min_aum), cmin, std::abs(cmin) * 1e-6 + 1e-6)
        << "capacity_min_alpha_aum must equal the min over the per-alpha capacities";

    std::error_code ec;
    fs::remove(panel_path, ec);
    fs::remove_all(alphas_dir, ec);
    for (const std::string& co : {combo_off, combo_probe, combo_on}) {
        fs::remove(co, ec);
        fs::remove(co + ".weights.txt", ec);
        fs::remove(co + ".meta", ec);
    }
}

// ---------------------------------------------------------------------------
// T6 Test 3: CapacityRunIsDeterministic
// The capacity-on combine (--capacity-floor>0 --target-aum>0) must be twice-run
// byte-identical (the capacity arithmetic is pure / no RNG / order-fixed).
// ---------------------------------------------------------------------------
TEST(AtxImplCombine, CapacityRunIsDeterministic) {
    namespace fs = std::filesystem;

    auto panel_opt = make_liquidity_panel();
    ASSERT_TRUE(panel_opt.has_value());
    const Panel& panel = *panel_opt;
    const std::string panel_path = write_panel_tmp(panel, "cap_det");
    const std::string alphas_dir = write_alpha_dir("cap_det", safe_dsls());
    const std::string combo1 =
        (fs::temp_directory_path() / "atx_impl_combine_cap_det1.bin").string();
    const std::string combo2 =
        (fs::temp_directory_path() / "atx_impl_combine_cap_det2.bin").string();

    atx::impl::RunConfig cfg;
    cfg.subcommand     = "combine";
    cfg.panel          = panel_path;
    cfg.alphas         = alphas_dir;
    cfg.method         = "shrinkage-mv";
    cfg.fit_begin      = 0;
    cfg.fit_end        = 0;
    cfg.target_aum     = 1.0e8;
    cfg.capacity_floor = 5.0e8;

    cfg.combo_out = combo1;
    auto r1 = atx::impl::run_combine(cfg);
    ASSERT_TRUE(r1.has_value()) << r1.error().message();
    cfg.combo_out = combo2;
    auto r2 = atx::impl::run_combine(cfg);
    ASSERT_TRUE(r2.has_value()) << r2.error().message();

    EXPECT_EQ(r1->digest, r2->digest)
        << "capacity-on combine must be deterministic (twice-run identical digest)";
    EXPECT_NE(r1->digest, atx::u64{0});
    EXPECT_EQ(find_kv_combine(*r1, "capacity_alpha_aum"),
              find_kv_combine(*r2, "capacity_alpha_aum"))
        << "per-alpha capacity telemetry must be deterministic across runs";

    auto read_bytes = [](const std::string& path) -> std::vector<char> {
        std::ifstream f{path, std::ios::binary};
        return std::vector<char>{std::istreambuf_iterator<char>(f),
                                 std::istreambuf_iterator<char>()};
    };
    EXPECT_EQ(read_bytes(combo1), read_bytes(combo2))
        << "capacity-on combo.bin must be byte-identical across runs";

    std::error_code ec;
    fs::remove(panel_path, ec);
    fs::remove_all(alphas_dir, ec);
    for (const std::string& co : {combo1, combo2}) {
        fs::remove(co, ec);
        fs::remove(co + ".weights.txt", ec);
        fs::remove(co + ".meta", ec);
    }
}

} // namespace atxtest_combine
