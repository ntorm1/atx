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

#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
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
  for (usize d = 12; d < D; ++d) {
    auto cs = cpanel.field_cross_section(fid, d);
    f64 g0 = 0.0, g1 = 0.0; bool any = false;
    for (usize i = 0; i < N; ++i) {
      if (std::isnan(cs[i])) continue;
      (i < 3 ? g0 : g1) += cs[i]; any = true;
    }
    if (any) { EXPECT_NEAR(g0, 0.0, 1e-9); EXPECT_NEAR(g1, 0.0, 1e-9); }
  }
  std::error_code ec; fs::remove(panel_path, ec); fs::remove_all(alphas_dir, ec);
  fs::remove(combo_out, ec); fs::remove(combo_out + ".weights.txt", ec);
}

} // namespace atxtest_combine
