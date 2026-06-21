// atx::impl — sweep stage tests (suite AtxImplSweep, Task M1).
//
// Tests:
//   1. SweepAccumulatesAndByteIdentical  — sweep --sweep-runs 3 accumulates,
//      outputs .dsl + _manifest.{txt,bin}, twice-run byte-identical (digest + artifacts).
//   2. CrossSweepCumulativeN             — R1: cumulative_trials after 3 runs > any single run.
//   3. WalkForwardRotation               — R2: --oos-windows 3 makes 3 runs use rotating windows
//      (admitted sets can differ from single-window sweep); twice-run identical.
//   4. MissingLibraryDirFails            — clean Err(InvalidArgument) if --library-dir unset.
//   5. SweepRunsOneFails                 — --sweep-runs 0 returns Err; 1 => one mine iteration.

#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "atx/core/types.hpp"
#include "atx/engine/library/manifest.hpp"  // library::read_manifest (R1 sidecar readback)

#include "config.hpp"
#include "serialize_panel.hpp"
#include "stages.hpp"

namespace atxtest_sweep {

using atx::f64;
using atx::u64;
using atx::usize;

// ---------------------------------------------------------------------------
// LCG + panel fixture — identical to discover_test.cpp for consistency.
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

static std::optional<atx::engine::alpha::Panel> make_panel(usize dates = 96, usize insts = 6) {
    const auto close = noisy_close(dates, insts, 0xBEEFCAFEULL);
    auto r = atx::engine::alpha::Panel::create(dates, insts, {"close"}, {close}, {});
    if (!r.has_value()) {
        ADD_FAILURE() << "panel fixture must build: " << r.error().to_string();
        return std::nullopt;
    }
    return std::move(r.value());
}

static std::string write_panel_tmp(const atx::engine::alpha::Panel& panel, const std::string& stem) {
    namespace fs = std::filesystem;
    const std::string path =
        (fs::temp_directory_path() / ("atx_sweep_" + stem + ".bin")).string();
    auto r = atx::impl::write_panel(panel, path);
    EXPECT_TRUE(r.has_value()) << "write_panel must succeed";
    return path;
}

static std::vector<std::string> seed_exprs() {
    return {"rank(close)", "ts_mean(close,10)", "delta(close,2)"};
}

// Build a sweep RunConfig with permissive gate for testing.
static atx::impl::RunConfig sweep_cfg(const std::string& panel_path,
                                      const std::string& alpha_out,
                                      const std::string& lib_dir,
                                      unsigned long long seed,
                                      long sweep_runs) {
    atx::impl::RunConfig cfg;
    cfg.subcommand   = "sweep";
    cfg.panel        = panel_path;
    cfg.alpha_out    = alpha_out;
    cfg.seed         = seed;
    cfg.population   = 16;
    cfg.generations  = 5;
    cfg.seed_exprs   = seed_exprs();
    cfg.min_sharpe   = 0.0;
    cfg.min_fitness  = 0.0;
    cfg.max_turnover = 10.0;
    cfg.max_pool_corr= 1.0;
    cfg.min_dsr      = 0.0;
    cfg.library_dir  = lib_dir;
    cfg.sweep_runs   = sweep_runs;
    // NOTE: oos_fraction not set -> R3a auto-default 0.25 kicks in (sweep always accumulates).
    return cfg;
}

// Count .dsl files in a directory.
static int count_dsl(const std::string& dir) {
    namespace fs = std::filesystem;
    int n = 0;
    if (!fs::exists(dir)) return 0;
    for (const auto& e : fs::directory_iterator(dir)) {
        if (e.path().extension() == ".dsl") ++n;
    }
    return n;
}

// Read file content as string.
static std::string read_file(const std::string& path) {
    std::ifstream f{path};
    return std::string{std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
}

// ---------------------------------------------------------------------------
// Test 1: SweepAccumulatesAndByteIdentical
//
// Run sweep --sweep-runs 3 twice (into separate fresh library dirs + alpha_out
// dirs). Assert:
//   - runs <= 3, library_size > 0 in both runs.
//   - .dsl files + _manifest.txt + _manifest.bin written.
//   - digest line (stage_digest) byte-identical across the two runs.
//   - research_digest kv byte-identical.
//   - All .dsl file contents byte-identical.
// ---------------------------------------------------------------------------
TEST(AtxImplSweep, SweepAccumulatesAndByteIdentical) {
    namespace fs = std::filesystem;

    auto panel_opt = make_panel();
    ASSERT_TRUE(panel_opt.has_value());
    const std::string panel_path = write_panel_tmp(*panel_opt, "accum");

    const std::string lib_dir_A = (fs::temp_directory_path() / "atx_sweep_accum_libA").string();
    const std::string alpha_A   = (fs::temp_directory_path() / "atx_sweep_accum_outA").string();
    const std::string lib_dir_B = (fs::temp_directory_path() / "atx_sweep_accum_libB").string();
    const std::string alpha_B   = (fs::temp_directory_path() / "atx_sweep_accum_outB").string();

    std::error_code ec0;
    for (const auto& d : {lib_dir_A, alpha_A, lib_dir_B, alpha_B}) {
        fs::remove_all(d, ec0);
    }

    // Run A.
    auto cfgA = sweep_cfg(panel_path, alpha_A, lib_dir_A, /*seed=*/42ULL, /*sweep_runs=*/3);
    auto rA = atx::impl::run_sweep(cfgA);
    ASSERT_TRUE(rA.has_value()) << "sweep run A failed: " << rA.error().message();

    // Run B (fresh dirs, same config).
    auto cfgB = sweep_cfg(panel_path, alpha_B, lib_dir_B, /*seed=*/42ULL, /*sweep_runs=*/3);
    auto rB = atx::impl::run_sweep(cfgB);
    ASSERT_TRUE(rB.has_value()) << "sweep run B failed: " << rB.error().message();

    // Extract kvs helper.
    auto get_kv = [](const atx::impl::StageResult& sr, const std::string& key) -> std::string {
        for (const auto& p : sr.kvs) {
            if (p.first == key) return p.second;
        }
        return {};
    };

    const int runs_A = std::stoi(get_kv(*rA, "runs"));
    const int runs_B = std::stoi(get_kv(*rB, "runs"));
    EXPECT_LE(runs_A, 3) << "runs must be <= sweep_runs";
    EXPECT_LE(runs_B, 3);
    EXPECT_GE(runs_A, 1) << "at least one run must execute";

    const int lib_size_A = std::stoi(get_kv(*rA, "library_size"));
    EXPECT_GT(lib_size_A, 0) << "library must be non-empty";

    // .dsl files written.
    EXPECT_GT(count_dsl(alpha_A), 0) << "alpha_A must contain .dsl files";
    EXPECT_GT(count_dsl(alpha_B), 0) << "alpha_B must contain .dsl files";

    // _manifest.txt written.
    EXPECT_TRUE(fs::exists((fs::path{alpha_A} / "_manifest.txt").string()));
    EXPECT_TRUE(fs::exists((fs::path{alpha_B} / "_manifest.txt").string()));

    // _manifest.bin (R1 sidecar) written in library dir.
    EXPECT_TRUE(fs::exists((fs::path{lib_dir_A} / "_manifest.bin").string()))
        << "R1 sidecar _manifest.bin must be written to library dir";
    EXPECT_TRUE(fs::exists((fs::path{lib_dir_B} / "_manifest.bin").string()));

    // --- Twice-run byte-identical ---
    // Stage digest.
    EXPECT_EQ(rA->digest, rB->digest) << "stage digest must be byte-identical";
    EXPECT_NE(rA->digest, u64{0});

    // research_digest kv.
    EXPECT_EQ(get_kv(*rA, "research_digest"), get_kv(*rB, "research_digest"))
        << "research_digest (ResearchReport.digest) must be byte-identical";

    // manifest_version_id kv.
    EXPECT_EQ(get_kv(*rA, "manifest_version_id"), get_kv(*rB, "manifest_version_id"))
        << "manifest_version_id must be byte-identical";

    // All .dsl file contents must match.
    const int n = count_dsl(alpha_A);
    ASSERT_EQ(n, count_dsl(alpha_B)) << "same number of .dsl files";
    for (int i = 0; i < n; ++i) {
        std::ostringstream name_ss;
        name_ss << "alpha_" << std::setw(3) << std::setfill('0') << i << ".dsl";
        const std::string name = name_ss.str();
        const std::string cA = read_file((fs::path{alpha_A} / name).string());
        const std::string cB = read_file((fs::path{alpha_B} / name).string());
        EXPECT_EQ(cA, cB) << name << " must be byte-identical across runs";
    }

    // _manifest.txt byte-identical.
    const std::string mfA = read_file((fs::path{alpha_A} / "_manifest.txt").string());
    const std::string mfB = read_file((fs::path{alpha_B} / "_manifest.txt").string());
    EXPECT_EQ(mfA, mfB) << "_manifest.txt must be byte-identical";

    // _manifest.bin byte-identical.
    const std::string binA = read_file((fs::path{lib_dir_A} / "_manifest.bin").string());
    const std::string binB = read_file((fs::path{lib_dir_B} / "_manifest.bin").string());
    EXPECT_EQ(binA, binB) << "_manifest.bin must be byte-identical";

    // Cleanup.
    std::error_code ec;
    fs::remove(panel_path, ec);
    for (const auto& d : {lib_dir_A, alpha_A, lib_dir_B, alpha_B}) {
        fs::remove_all(d, ec);
    }
}

// ---------------------------------------------------------------------------
// Test 2: CrossSweepCumulativeN (R1 integration)
//
// Proves that cumulative_trials from a 3-run sweep is STRICTLY GREATER than
// the cumulative_trials from a 1-run sweep into a separate fresh library
// (same seed/panel/config).  This directly demonstrates that the R1 counter
// accumulates ACROSS runs rather than resetting to 0 on each run.
// The hardcoded ">= 30" floor is removed in favour of the 3-run > 1-run
// comparison, which is robust to any per-run trial count.
// ---------------------------------------------------------------------------
TEST(AtxImplSweep, CrossSweepCumulativeN) {
    namespace fs = std::filesystem;

    auto panel_opt = make_panel();
    ASSERT_TRUE(panel_opt.has_value());
    const std::string panel_path = write_panel_tmp(*panel_opt, "r1_cumN");

    // --- 1-run sweep into its own fresh library dir ---
    const std::string lib_1run  = (fs::temp_directory_path() / "atx_sweep_r1_lib1").string();
    const std::string out_1run  = (fs::temp_directory_path() / "atx_sweep_r1_out1").string();
    std::error_code ec0;
    fs::remove_all(lib_1run, ec0);
    fs::remove_all(out_1run, ec0);

    auto cfg1 = sweep_cfg(panel_path, out_1run, lib_1run, /*seed=*/55ULL, /*sweep_runs=*/1);
    auto r1 = atx::impl::run_sweep(cfg1);
    ASSERT_TRUE(r1.has_value()) << "1-run sweep must succeed: " << r1.error().message();

    const std::string sidecar_1run = (fs::path{lib_1run} / "_manifest.bin").string();
    ASSERT_TRUE(fs::exists(sidecar_1run)) << "_manifest.bin must exist after 1-run sweep";
    auto mf1 = atx::engine::library::read_manifest(sidecar_1run);
    ASSERT_TRUE(mf1.has_value()) << "1-run sidecar must parse: " << mf1.error().message();
    const atx::u64 cum_1run = mf1->cumulative_trials;
    EXPECT_GT(cum_1run, u64{0}) << "1-run sweep must record > 0 trials";

    // --- 3-run sweep into a SEPARATE fresh library dir (same seed) ---
    const std::string lib_3run  = (fs::temp_directory_path() / "atx_sweep_r1_lib3").string();
    const std::string out_3run  = (fs::temp_directory_path() / "atx_sweep_r1_out3").string();
    fs::remove_all(lib_3run, ec0);
    fs::remove_all(out_3run, ec0);

    auto cfg3 = sweep_cfg(panel_path, out_3run, lib_3run, /*seed=*/55ULL, /*sweep_runs=*/3);
    auto r3 = atx::impl::run_sweep(cfg3);
    ASSERT_TRUE(r3.has_value()) << "3-run sweep must succeed: " << r3.error().message();

    const std::string sidecar_3run = (fs::path{lib_3run} / "_manifest.bin").string();
    ASSERT_TRUE(fs::exists(sidecar_3run)) << "_manifest.bin must exist after 3-run sweep";
    auto mf3 = atx::engine::library::read_manifest(sidecar_3run);
    ASSERT_TRUE(mf3.has_value()) << "3-run sidecar must parse: " << mf3.error().message();
    const atx::u64 cum_3run = mf3->cumulative_trials;

    // Core assertion: 3-run sweep must have STRICTLY MORE cumulative trials than
    // 1-run sweep — this proves R1 accumulates across runs rather than resetting.
    EXPECT_GT(cum_3run, cum_1run)
        << "3-run sweep cumulative_trials (" << cum_3run
        << ") must exceed 1-run sweep (" << cum_1run
        << "); R1 must accumulate across runs not reset";

    // Sanity: cumulative trials must also exceed the admitted alpha count (always).
    auto get_kv = [](const atx::impl::StageResult& sr, const std::string& key) -> u64 {
        for (const auto& p : sr.kvs) {
            if (p.first == key) return static_cast<u64>(std::stoul(p.second));
        }
        return 0ULL;
    };
    EXPECT_GT(cum_3run, get_kv(*r3, "library_size"))
        << "cumulative trials must exceed admitted alpha count";

    // Cleanup.
    std::error_code ec;
    fs::remove(panel_path, ec);
    fs::remove_all(lib_1run, ec);
    fs::remove_all(out_1run, ec);
    fs::remove_all(lib_3run, ec);
    fs::remove_all(out_3run, ec);
}

// ---------------------------------------------------------------------------
// Test 3: WalkForwardRotation (R2 integration)
//
// Proves that per-run window rotation has a real observable effect:
//   a) A 3-run sweep with --oos-windows 3 is twice-run byte-identical
//      (rotation is deterministic: window = run % oos_n_windows).
//   b) The research_digest from a 3-run --oos-windows 3 sweep DIFFERS from
//      a 3-run --oos-windows 0 sweep (same seed/panel/config) — rotation has
//      an observable effect on the mine outcomes, proving the window actually
//      changes per run.  NOTE: It is possible (though unlikely with a real-signal
//      panel) that both digests accidentally collide; if this assert is flaky
//      it should be converted to an engine-level test instead.
//   c) The manifest contains oos_fraction= (OOS is active).
// ---------------------------------------------------------------------------
TEST(AtxImplSweep, WalkForwardRotation) {
    namespace fs = std::filesystem;

    auto panel_opt = make_panel(/*dates=*/96, /*insts=*/6);
    ASSERT_TRUE(panel_opt.has_value());
    const std::string panel_path = write_panel_tmp(*panel_opt, "r2_wf");

    const std::string lib_A  = (fs::temp_directory_path() / "atx_sweep_wf_libA").string();
    const std::string out_A  = (fs::temp_directory_path() / "atx_sweep_wf_outA").string();
    const std::string lib_B  = (fs::temp_directory_path() / "atx_sweep_wf_libB").string();
    const std::string out_B  = (fs::temp_directory_path() / "atx_sweep_wf_outB").string();
    // For digest-differs assertion: a separate 3-run oos_windows=0 sweep.
    const std::string lib_C  = (fs::temp_directory_path() / "atx_sweep_wf_libC").string();
    const std::string out_C  = (fs::temp_directory_path() / "atx_sweep_wf_outC").string();
    std::error_code ec0;
    for (const auto& d : {lib_A, out_A, lib_B, out_B, lib_C, out_C}) fs::remove_all(d, ec0);

    // Run A and B: oos_windows=3, twice (for determinism proof).
    auto make_cfg_wf = [&](const std::string& alpha_out, const std::string& lib_dir) {
        auto cfg = sweep_cfg(panel_path, alpha_out, lib_dir, /*seed=*/77ULL, /*sweep_runs=*/3);
        cfg.oos_windows = 3;  // R2: run 0->window 0, run 1->window 1, run 2->window 2
        // A4 — honor the explicit 3 against run_sweep's new set_flags-keyed default-3
        // (faithful to real CLI behavior, where --oos-windows N inserts the flag).
        cfg.set_flags.insert("oos-windows");
        return cfg;
    };
    auto rA = atx::impl::run_sweep(make_cfg_wf(out_A, lib_A));
    ASSERT_TRUE(rA.has_value()) << "walk-forward sweep A: " << rA.error().message();
    auto rB = atx::impl::run_sweep(make_cfg_wf(out_B, lib_B));
    ASSERT_TRUE(rB.has_value()) << "walk-forward sweep B: " << rB.error().message();

    // Run C: same seed + sweep_runs=3 but oos_windows=0 (no rotation).
    auto cfg_c = sweep_cfg(panel_path, out_C, lib_C, /*seed=*/77ULL, /*sweep_runs=*/3);
    // A4 — explicitly pin oos_windows=0 (no-rotation baseline) AND mark the flag
    // explicit so run_sweep's new default-3 does NOT override it back to walk-forward.
    cfg_c.oos_windows = 0;
    cfg_c.set_flags.insert("oos-windows");
    auto rC = atx::impl::run_sweep(cfg_c);
    ASSERT_TRUE(rC.has_value()) << "no-rotation sweep C: " << rC.error().message();

    auto get_kv = [](const atx::impl::StageResult& sr, const std::string& k) {
        for (const auto& p : sr.kvs) { if (p.first == k) return p.second; }
        return std::string{};
    };

    // (a) Twice-run byte-identical with oos_windows=3.
    EXPECT_EQ(rA->digest, rB->digest) << "walk-forward sweep must be twice-run byte-identical";
    EXPECT_NE(rA->digest, u64{0});
    EXPECT_EQ(get_kv(*rA, "research_digest"), get_kv(*rB, "research_digest"));

    // (b) Rotation has an observable effect: oos_windows=3 digest must differ from
    //     oos_windows=0 digest.  The per-run holdout window changes which dates are
    //     withheld, so the admitted set (and thus the engine fingerprint) differs.
    EXPECT_NE(get_kv(*rA, "research_digest"), get_kv(*rC, "research_digest"))
        << "oos_windows=3 research_digest must differ from oos_windows=0 — "
           "window rotation must have a real effect on the admitted alpha set";

    // (c) Manifest must contain oos_fraction (OOS is active via auto-default 0.25).
    const std::string mfA = read_file((fs::path{out_A} / "_manifest.txt").string());
    EXPECT_NE(mfA.find("oos_fraction="), std::string::npos)
        << "walk-forward manifest must contain oos_fraction=";

    // Cleanup.
    std::error_code ec;
    fs::remove(panel_path, ec);
    for (const auto& d : {lib_A, out_A, lib_B, out_B, lib_C, out_C}) fs::remove_all(d, ec);
}

// ---------------------------------------------------------------------------
// Test 4: MissingLibraryDirFails
//
// Without --library-dir, run_sweep must return Err(InvalidArgument).
// ---------------------------------------------------------------------------
TEST(AtxImplSweep, MissingLibraryDirFails) {
    namespace fs = std::filesystem;

    auto panel_opt = make_panel();
    ASSERT_TRUE(panel_opt.has_value());
    const std::string panel_path = write_panel_tmp(*panel_opt, "no_libdir");
    const std::string alpha_out = (fs::temp_directory_path() / "atx_sweep_no_libdir_out").string();

    atx::impl::RunConfig cfg;
    cfg.subcommand   = "sweep";
    cfg.panel        = panel_path;
    cfg.alpha_out    = alpha_out;
    cfg.seed         = 1ULL;
    cfg.population   = 8;
    cfg.generations  = 2;
    cfg.seed_exprs   = seed_exprs();
    cfg.sweep_runs   = 1;
    // library_dir intentionally empty.

    auto r = atx::impl::run_sweep(cfg);
    EXPECT_FALSE(r.has_value()) << "sweep without --library-dir must fail";
    if (!r.has_value()) {
        EXPECT_EQ(r.error().code(), atx::core::ErrorCode::InvalidArgument);
        EXPECT_NE(r.error().message().find("library-dir"), std::string::npos)
            << "error must mention library-dir; got: " << r.error().message();
    }

    std::error_code ec;
    fs::remove(panel_path, ec);
    fs::remove_all(alpha_out, ec);
}

// ---------------------------------------------------------------------------
// Test 5a: SweepRunsZeroFails
//
// --sweep-runs 0 must return Err(InvalidArgument).
// ---------------------------------------------------------------------------
TEST(AtxImplSweep, SweepRunsZeroFails) {
    namespace fs = std::filesystem;

    auto panel_opt = make_panel();
    ASSERT_TRUE(panel_opt.has_value());
    const std::string panel_path = write_panel_tmp(*panel_opt, "runs0");
    const std::string lib_dir = (fs::temp_directory_path() / "atx_sweep_runs0_lib").string();
    const std::string alpha_out = (fs::temp_directory_path() / "atx_sweep_runs0_out").string();

    auto cfg = sweep_cfg(panel_path, alpha_out, lib_dir, /*seed=*/1ULL, /*sweep_runs=*/0);
    auto r = atx::impl::run_sweep(cfg);
    EXPECT_FALSE(r.has_value()) << "sweep-runs 0 must fail";
    if (!r.has_value()) {
        EXPECT_EQ(r.error().code(), atx::core::ErrorCode::InvalidArgument);
    }

    std::error_code ec;
    fs::remove(panel_path, ec);
    fs::remove_all(lib_dir, ec);
    fs::remove_all(alpha_out, ec);
}

// ---------------------------------------------------------------------------
// Test 5b: SweepRunsOneEquivalentToSingleMine
//
// --sweep-runs 1 is equivalent to one gated discover into the library.
// Result: runs==1, library_size>=1, .dsl and _manifest.txt written.
// ---------------------------------------------------------------------------
TEST(AtxImplSweep, SweepRunsOneEquivalentToSingleMine) {
    namespace fs = std::filesystem;

    auto panel_opt = make_panel();
    ASSERT_TRUE(panel_opt.has_value());
    const std::string panel_path = write_panel_tmp(*panel_opt, "runs1");
    const std::string lib_dir = (fs::temp_directory_path() / "atx_sweep_runs1_lib").string();
    const std::string alpha_out = (fs::temp_directory_path() / "atx_sweep_runs1_out").string();
    std::error_code ec0;
    fs::remove_all(lib_dir, ec0);
    fs::remove_all(alpha_out, ec0);

    auto cfg = sweep_cfg(panel_path, alpha_out, lib_dir, /*seed=*/99ULL, /*sweep_runs=*/1);
    auto r = atx::impl::run_sweep(cfg);
    ASSERT_TRUE(r.has_value()) << "sweep-runs 1 must succeed: " << r.error().message();

    auto get_kv = [&](const std::string& key) {
        for (const auto& p : r->kvs) { if (p.first == key) return p.second; }
        return std::string{};
    };

    EXPECT_EQ(get_kv("runs"), "1") << "runs kv must be 1";
    EXPECT_GT(std::stoi(get_kv("library_size")), 0) << "library must be non-empty";
    EXPECT_GT(count_dsl(alpha_out), 0) << "must write .dsl files";
    EXPECT_TRUE(fs::exists((fs::path{alpha_out} / "_manifest.txt").string()));
    EXPECT_TRUE(fs::exists((fs::path{lib_dir} / "_manifest.bin").string()));

    std::error_code ec;
    fs::remove(panel_path, ec);
    fs::remove_all(lib_dir, ec);
    fs::remove_all(alpha_out, ec);
}

// ---------------------------------------------------------------------------
// Helper: parse a key=value line out of summary.txt (empty if absent).
// ---------------------------------------------------------------------------
static std::string read_summary_kv(const std::string& summary, const std::string& key) {
    std::ifstream f{summary};
    std::string line;
    while (std::getline(f, line)) {
        const auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        if (line.substr(0, eq) == key) return line.substr(eq + 1);
    }
    return {};
}

// ---------------------------------------------------------------------------
// Test 6: EndToEndMegaAlphaHasPositiveOosSharpe (A4 — the deliverable's proof)
//
// Runs the REAL pipeline sweep -> combine -> optimize -> report on the
// planted-drift panel and asserts the reported mega-alpha portfolio
// OUT-OF-SAMPLE Sharpe is FINITE AND POSITIVE.  This is the executable proof
// of the whole mega-alpha roadmap: A2b made report emit portfolio_oos_sharpe;
// A4 proves it is finite + positive on data with a real, persistent edge.
//
// Honesty: make_panel plants a persistent per-instrument cross-sectional drift
// (drift[j] = 0.006 - 0.0024*j, ~very high signal/noise vs 1%/day noise). A
// cross-sectional seed like rank(close) captures this stationary edge so its
// Sharpe is strongly positive BOTH in-sample and out-of-sample. The combiner
// fits weights on the IS window (A2a holdout) and report scores the held-out
// [fit_end, np) span (A2b) -> a persistent edge survives -> positive OOS Sharpe.
// ---------------------------------------------------------------------------
TEST(AtxImplSweep, EndToEndMegaAlphaHasPositiveOosSharpe) {
    namespace fs = std::filesystem;

    // 1. Planted-drift panel: 120 dates so the default 3 walk-forward windows fit
    //    feasibly (3*0.25*T holdout + 0.25*T leading train tiles the panel) and the
    //    OOS span has >= 2 rebalance periods.
    auto panel_opt = make_panel(/*dates=*/120, /*insts=*/6);
    ASSERT_TRUE(panel_opt.has_value());
    const std::string panel_path = write_panel_tmp(*panel_opt, "e2e_megaalpha");

    const fs::path work       = fs::temp_directory_path() / "atx_sweep_e2e_work";
    const std::string lib_dir = (fs::temp_directory_path() / "atx_sweep_e2e_lib").string();
    const std::string alpha_o = (fs::temp_directory_path() / "atx_sweep_e2e_alphas").string();
    const std::string report_dir = (fs::temp_directory_path() / "atx_sweep_e2e_report").string();
    std::error_code ec0;
    fs::remove_all(work, ec0);
    fs::remove_all(lib_dir, ec0);
    fs::remove_all(alpha_o, ec0);
    fs::remove_all(report_dir, ec0);
    fs::create_directories(work, ec0);

    // 2. run_sweep: exercise the NEW default 3 walk-forward windows (do NOT set
    //    oos_windows / set_flags). Permissive gate (helper sets the floors to 0).
    auto cfg = sweep_cfg(panel_path, alpha_o, lib_dir, /*seed=*/2024ULL, /*sweep_runs=*/4);
    auto rs = atx::impl::run_sweep(cfg);
    ASSERT_TRUE(rs.has_value()) << "sweep failed: " << rs.error().message();

    auto get_kv = [](const atx::impl::StageResult& sr, const std::string& k) {
        for (const auto& p : sr.kvs) { if (p.first == k) return p.second; }
        return std::string{};
    };
    const int lib_size = std::stoi(get_kv(*rs, "library_size"));
    ASSERT_GE(lib_size, 2) << "need >= 2 admitted alphas for a non-degenerate combine; got "
                           << lib_size;
    // Confirm the new default put 3 walk-forward windows on the telemetry.
    EXPECT_EQ(get_kv(*rs, "oos_windows"), "3")
        << "sweep must default to 3 walk-forward OOS windows";

    // 3. run_combine: holdout-frac 0.25 makes [fit_end, np) genuinely OOS (A2a).
    const std::string combo_path = (work / "combo.bin").string();
    {
        atx::impl::RunConfig c;
        c.subcommand           = "combine";
        c.panel                = panel_path;
        c.library_dir          = lib_dir;
        c.combo_out            = combo_path;
        c.combine_holdout_frac = 0.25;
        c.seed                 = 2024ULL;
        auto rc = atx::impl::run_combine(c);
        ASSERT_TRUE(rc.has_value()) << "combine failed: " << rc.error().message();
        ASSERT_TRUE(fs::exists(combo_path + ".meta"))
            << "combo.bin.meta must exist (combine must record the OOS split boundary)";
    }

    // 4. run_optimize.
    const std::string books_path = (work / "books.bin").string();
    {
        atx::impl::RunConfig c;
        c.subcommand = "optimize";
        c.panel      = panel_path;
        c.combo      = combo_path;
        c.books_out  = books_path;
        c.seed       = 2024ULL;
        auto ro = atx::impl::run_optimize(c);
        ASSERT_TRUE(ro.has_value()) << "optimize failed: " << ro.error().message();
    }

    // 5. run_report: point at combo.bin so it reads combo.bin.meta and splits IS/OOS (A2b).
    {
        atx::impl::RunConfig c;
        c.subcommand = "report";
        c.panel      = panel_path;
        c.books      = books_path;
        c.combo      = combo_path;
        c.report_out = report_dir;
        c.seed       = 2024ULL;
        auto rr = atx::impl::run_report(c);
        ASSERT_TRUE(rr.has_value()) << "report failed: " << rr.error().message();
    }

    // 6. Parse summary.txt and assert the headline: finite AND positive OOS Sharpe.
    const std::string summary = (fs::path{report_dir} / "summary.txt").string();
    ASSERT_TRUE(fs::exists(summary)) << "summary.txt missing at " << summary;
    const std::string all = read_file(summary);

    const std::string oos_s = read_summary_kv(summary, "portfolio_oos_sharpe");
    ASSERT_FALSE(oos_s.empty()) << "summary.txt missing portfolio_oos_sharpe=\n" << all;
    const double portfolio_oos_sharpe = std::stod(oos_s);
    EXPECT_TRUE(std::isfinite(portfolio_oos_sharpe))
        << "portfolio_oos_sharpe must be finite: " << oos_s << "\n" << all;
    EXPECT_GT(portfolio_oos_sharpe, 0.0)
        << "planted persistent edge MUST clear OOS (portfolio_oos_sharpe > 0): "
        << oos_s << "\n" << all;

    const std::string n_oos_s = read_summary_kv(summary, "n_oos_periods");
    ASSERT_FALSE(n_oos_s.empty()) << "summary.txt missing n_oos_periods=\n" << all;
    const long n_oos = std::stol(n_oos_s);
    EXPECT_GE(n_oos, 1) << "a real OOS split must have happened (n_oos_periods>=1)\n" << all;

    const std::string ps_s = read_summary_kv(summary, "portfolio_sharpe");
    ASSERT_FALSE(ps_s.empty()) << "summary.txt missing portfolio_sharpe=\n" << all;
    const double portfolio_sharpe = std::stod(ps_s);
    EXPECT_TRUE(std::isfinite(portfolio_sharpe))
        << "portfolio_sharpe (full series) must be finite: " << ps_s << "\n" << all;
    EXPECT_GT(portfolio_sharpe, 0.0)
        << "planted persistent edge MUST give a positive full-series Sharpe: "
        << ps_s << "\n" << all;

    const std::string hb_s = read_summary_kv(summary, "holdout_begin");
    ASSERT_FALSE(hb_s.empty()) << "summary.txt missing holdout_begin=\n" << all;
    EXPECT_GT(std::stol(hb_s), 0) << "holdout_begin must carve a real interior boundary\n" << all;

    // Surface the observed numbers in the test log for the report.
    std::cerr << "[A4] portfolio_oos_sharpe=" << portfolio_oos_sharpe
              << " portfolio_sharpe=" << portfolio_sharpe
              << " n_oos_periods=" << n_oos
              << " holdout_begin=" << hb_s
              << " library_size=" << lib_size << "\n";

    // Cleanup.
    std::error_code ec;
    fs::remove(panel_path, ec);
    fs::remove_all(work, ec);
    fs::remove_all(lib_dir, ec);
    fs::remove_all(alpha_o, ec);
    fs::remove_all(report_dir, ec);
}

} // namespace atxtest_sweep
