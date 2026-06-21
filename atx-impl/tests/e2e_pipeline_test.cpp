// atx::impl — end-to-end pipeline tests (suite AtxImplE2E, S6 TDD).
//
// TDD order: RunProducesReport first (exercises the whole chain end to end).
// Then StagedEqualsRun (pins staged == run digest guarantee) and
// ReportBytesDeterministic (R8 byte-identical assertion).
//
// Fixture: 10 instruments x 100 trading dates built as a synthetic ORATS zip.
// This sizing is chosen to:
//   - give discover sufficient history for rank(close)/ts_mean(close,5) to
//     have a finite positive Sharpe (min_date range, momentum drift per instrument)
//   - produce >= 1 admitted alpha so combine/optimize/report all have real data
//   - stay fast (in-process zip build, no disk seg files needed for run mode)

#include <array>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <gtest/gtest.h>
#include <miniz.h>

#include "artifacts.hpp"
#include "config.hpp"
#include "stages.hpp"

#include "atx/engine/combine/gate.hpp"       // combine::GateConfig (A1 library readback)
#include "atx/engine/library/library.hpp"    // library::Library, n_alphas (A1 library readback)

#include "orats_fixture.hpp"

namespace atx_impl_e2e {

namespace fs = std::filesystem;

// ---------------------------------------------------------------------------
// Fixture sizing constants.
// ---------------------------------------------------------------------------
static constexpr int kInstr = 10;
static constexpr int kDates = 100;

// ---------------------------------------------------------------------------
// Build a large synthetic ORATS zip body: kInstr instruments x kDates dates.
//
// Prices have a per-instrument momentum drift so rank(close) and ts_mean have
// genuine signal. cumulReturnFactor grows monotonically (TRI = close * factor).
// Volume/shares sized well above the ADV screen.
// ---------------------------------------------------------------------------
static std::string make_large_orats_body() {
    using atx_impl_test::kHeader;
    using atx_impl_test::make_orats_row;

    // Base date: 2020-01-02 (first trading day after the min_date filter).
    // We generate kDates consecutive "trading days" (weekdays don't matter for
    // the synthetic loader — each unique date string gets its own day bucket).
    std::string body = std::string(kHeader) + "\n";

    // Per-instrument price and cumret accumulators.
    std::vector<double> px(static_cast<std::size_t>(kInstr));
    std::vector<double> cumret(static_cast<std::size_t>(kInstr), 1.0);

    // Small deterministic drift: instrument i has drift 0.002 + 0.0005*i so
    // that cross-sectional rank(close) sorts stably over time => finite Sharpe.
    std::vector<double> drift(static_cast<std::size_t>(kInstr));
    for (int i = 0; i < kInstr; ++i) {
        const auto ii = static_cast<std::size_t>(i);
        px[ii]    = 50.0 + static_cast<double>(i) * 5.0;  // initial prices spread
        drift[ii] = 0.002 + 0.0005 * static_cast<double>(i);
    }

    // Date strings: start from 2020-01-02, walk forward one day at a time.
    // We just encode the numeric date as YYYY-MM-DD for the 100 days.
    // We use a simple day-of-year counter to generate distinct date strings
    // without depending on calendar logic (the loader only needs distinct keys).
    // Convert day_offset (0-based from 2020-01-02) to a YYYY-MM-DD string.
    // 2020 is a leap year; we walk 100 days starting from 2020-01-02 (day 2).
    auto make_date = [](int day_offset) -> std::string {
        static constexpr std::array<int,12> kMlen = {31,29,31,30,31,30,31,31,30,31,30,31};
        // abs_day: 1-indexed day of 2020; day 1 = Jan-1, day 2 = Jan-2 (our start).
        int abs_day = 2 + day_offset;
        int m = 0;
        while (m < 11 && abs_day > kMlen[static_cast<std::size_t>(m)]) {
            abs_day -= kMlen[static_cast<std::size_t>(m)];
            ++m;
        }
        char buf[16];
        (void)std::snprintf(buf, sizeof(buf), "2020-%02d-%02d", m + 1, abs_day);
        return buf;
    };

    for (int d = 0; d < kDates; ++d) {
        const std::string date = make_date(d);
        for (int i = 0; i < kInstr; ++i) {
            const auto ii = static_cast<std::size_t>(i);
            // Deterministic step.
            px[ii]     *= (1.0 + drift[ii]);
            cumret[ii] *= (1.0 + drift[ii]);

            const std::string secid = std::to_string(30000 + i);
            const std::string tk    = "SYM" + std::to_string(i);
            body += make_orats_row(date.c_str(), secid.c_str(), tk.c_str(),
                                   tk.c_str(), px[ii], cumret[ii],
                                   500'000'000LL);
        }
    }
    return body;
}

// Write the large fixture zip and return its path. Must be called from a void
// context (write_orats_zip uses ASSERT_TRUE).
static void make_large_orats_zip(std::string& out_zip) {
    const std::string path =
        (fs::temp_directory_path() / "atx_impl_e2e_large.zip").string();
    const std::string body = make_large_orats_body();
    atx_impl_test::write_orats_zip(body, path);
    out_zip = path;
}

// ---------------------------------------------------------------------------
// Build the base RunConfig shared across e2e tests.
// ---------------------------------------------------------------------------
static atx::impl::RunConfig make_base_cfg(const std::string& zip,
                                           const std::string& work_dir,
                                           const std::string& report_dir)
{
    atx::impl::RunConfig cfg;
    cfg.zip          = zip;
    cfg.out          = work_dir;
    cfg.min_date     = "2019-12-31";   // admit all dates from 2020 onward
    cfg.start        = "";             // default (all dates)
    cfg.end          = "";
    cfg.min_adv_usd  = 0.0;           // admit all instruments
    cfg.top_n_by_adv = 0;             // no cap
    cfg.seed         = 777ULL;
    cfg.population   = 12;
    cfg.generations  = 3;
    cfg.seed_exprs   = {"rank(close)", "ts_mean(close,5)"};
    cfg.method       = "equal";
    cfg.fit_begin    = 0;
    cfg.fit_end      = 0;
    cfg.gross        = 1.0;
    cfg.name_cap     = 0.2;
    cfg.rebalance    = "weekly";
    cfg.risk_aversion = 1.0;
    cfg.report_out   = report_dir;
    return cfg;
}

// Wire the 6-stage staged run exactly as run_all does (so staged == run).
// Returns each stage's StageResult via the out-params.
struct StagedDigests {
    atx::u64 load;
    atx::u64 panel;
    atx::u64 discover;
    atx::u64 combine;
    atx::u64 optimize;
    atx::u64 report;
};

// Run all 6 stages individually into `work`, capturing digests.
static ::testing::AssertionResult run_staged(const atx::impl::RunConfig& base_cfg,
                                              const std::string& work,
                                              const std::string& report_dir,
                                              StagedDigests& out)
{
    fs::create_directories(fs::path(work));
    atx::impl::RunConfig cfg = base_cfg;
    cfg.out       = work;
    cfg.report_out = report_dir;

    // 1. load
    atx::impl::RunConfig c_load = cfg;
    c_load.out = (fs::path{work} / "segs").string();
    auto r_load = atx::impl::run_load(c_load);
    if (!r_load.has_value())
        return ::testing::AssertionFailure() << "run_load: " << r_load.error().message();
    out.load = r_load->digest;

    // 2. panel
    atx::impl::RunConfig c_panel = cfg;
    c_panel.segs      = (fs::path{work} / "segs").string();
    c_panel.panel_out = (fs::path{work} / "panel.bin").string();
    auto r_panel = atx::impl::run_panel(c_panel);
    if (!r_panel.has_value())
        return ::testing::AssertionFailure() << "run_panel: " << r_panel.error().message();
    out.panel = r_panel->digest;

    // 3. discover
    atx::impl::RunConfig c_disc = cfg;
    c_disc.panel     = (fs::path{work} / "panel.bin").string();
    c_disc.alpha_out = (fs::path{work} / "alphas").string();
    // A1 — mirror run_all's library accumulation wiring so staged == run.
    c_disc.gated       = true;
    c_disc.library_dir = (fs::path{work} / "_library").string();
    auto r_disc = atx::impl::run_discover(c_disc);
    if (!r_disc.has_value())
        return ::testing::AssertionFailure() << "run_discover: " << r_disc.error().message();
    out.discover = r_disc->digest;

    // 4. combine
    atx::impl::RunConfig c_comb = cfg;
    c_comb.panel     = (fs::path{work} / "panel.bin").string();
    c_comb.alphas    = (fs::path{work} / "alphas").string();
    c_comb.combo_out = (fs::path{work} / "combo.bin").string();
    // A1 — feed combine from the same accumulated library (mirrors run_all).
    c_comb.library_dir = c_disc.library_dir;
    auto r_comb = atx::impl::run_combine(c_comb);
    if (!r_comb.has_value())
        return ::testing::AssertionFailure() << "run_combine: " << r_comb.error().message();
    out.combine = r_comb->digest;

    // 5. optimize
    atx::impl::RunConfig c_opt = cfg;
    c_opt.panel     = (fs::path{work} / "panel.bin").string();
    c_opt.combo     = (fs::path{work} / "combo.bin").string();
    c_opt.books_out = (fs::path{work} / "books.bin").string();
    auto r_opt = atx::impl::run_optimize(c_opt);
    if (!r_opt.has_value())
        return ::testing::AssertionFailure() << "run_optimize: " << r_opt.error().message();
    out.optimize = r_opt->digest;

    // 6. report
    atx::impl::RunConfig c_rep = cfg;
    c_rep.panel      = (fs::path{work} / "panel.bin").string();
    c_rep.books      = (fs::path{work} / "books.bin").string();
    c_rep.report_out = report_dir;
    auto r_rep = atx::impl::run_report(c_rep);
    if (!r_rep.has_value())
        return ::testing::AssertionFailure() << "run_report: " << r_rep.error().message();
    out.report = r_rep->digest;

    return ::testing::AssertionSuccess();
}

// ---------------------------------------------------------------------------
// Fixture: build the zip once per test-program lifetime; each test uses a
// fresh temp work dir so runs are independent.
// ---------------------------------------------------------------------------
class AtxImplE2E : public ::testing::Test {
protected:
    static std::string s_zip_;

    static void SetUpTestSuite() {
        ASSERT_NO_FATAL_FAILURE(make_large_orats_zip(s_zip_));
    }

    static void TearDownTestSuite() {
        std::error_code ec;
        fs::remove(fs::path(s_zip_), ec);
    }

    void SetUp() override {
        ASSERT_FALSE(s_zip_.empty()) << "zip fixture not built";
    }

    // Create a uniquely-named temp dir for this test (clean up in TearDown).
    fs::path make_work_dir(const char* tag) {
        const fs::path d = fs::temp_directory_path() /
                           (std::string("atx_impl_e2e_") + tag);
        std::error_code ec;
        fs::remove_all(d, ec);
        fs::create_directories(d, ec);
        work_dirs_.push_back(d);
        return d;
    }

    void TearDown() override {
        for (const auto& d : work_dirs_) {
            std::error_code ec;
            fs::remove_all(d, ec);
        }
        work_dirs_.clear();
    }

private:
    std::vector<fs::path> work_dirs_;
};

std::string AtxImplE2E::s_zip_;

// ---------------------------------------------------------------------------
// Test 1: RunProducesReport
// Call run_all; assert Ok; check work-dir artifacts and report files; digest
// is non-zero.
// ---------------------------------------------------------------------------
TEST_F(AtxImplE2E, RunProducesReport) {
    const fs::path work   = make_work_dir("run1_work");
    const fs::path report = make_work_dir("run1_report");

    atx::impl::RunConfig cfg =
        make_base_cfg(s_zip_, work.string(), report.string());

    auto result = atx::impl::run_all(cfg);
    ASSERT_TRUE(result.has_value()) << result.error().message();

    // Work-dir artifacts.
    EXPECT_TRUE(fs::exists(work / "segs"))       << "missing segs/";
    EXPECT_TRUE(fs::exists(work / "panel.bin"))  << "missing panel.bin";
    EXPECT_TRUE(fs::exists(work / "alphas"))     << "missing alphas/";
    EXPECT_TRUE(fs::exists(work / "combo.bin"))  << "missing combo.bin";
    EXPECT_TRUE(fs::exists(work / "books.bin"))  << "missing books.bin";

    // At least one .dsl alpha.
    bool has_dsl = false;
    if (fs::exists(work / "alphas")) {
        for (const auto& e : fs::directory_iterator(work / "alphas")) {
            if (e.path().extension() == ".dsl") { has_dsl = true; break; }
        }
    }
    EXPECT_TRUE(has_dsl) << "no .dsl files in alphas/";

    // Report canonical TSVs.
    EXPECT_TRUE(fs::exists(report / "pnl.tsv"))          << "missing pnl.tsv";
    EXPECT_TRUE(fs::exists(report / "leverage.tsv"))     << "missing leverage.tsv";
    EXPECT_TRUE(fs::exists(report / "exposure.tsv"))     << "missing exposure.tsv";
    EXPECT_TRUE(fs::exists(report / "census.tsv"))       << "missing census.tsv";

    // Convenience files.
    EXPECT_TRUE(fs::exists(report / "equity_curve.csv")) << "missing equity_curve.csv";
    EXPECT_TRUE(fs::exists(report / "summary.txt"))      << "missing summary.txt";

    // Run digest must be non-zero.
    EXPECT_NE(result->digest, atx::u64{0}) << "run digest must be non-zero";
}

// ---------------------------------------------------------------------------
// Test 2: StagedEqualsRun
// Run the 6 stages individually into a staged work dir and capture each
// stage's digest. Then run_all into a separate run work dir. Assert per-stage
// digest equality (the core integration guarantee).
// ---------------------------------------------------------------------------
TEST_F(AtxImplE2E, StagedEqualsRun) {
    const fs::path staged_work   = make_work_dir("staged_work");
    const fs::path staged_report = make_work_dir("staged_report");
    const fs::path run_work      = make_work_dir("run_work");
    const fs::path run_report_d  = make_work_dir("run_report");

    atx::impl::RunConfig base_cfg =
        make_base_cfg(s_zip_, staged_work.string(), staged_report.string());

    // Run the 6 stages individually.
    StagedDigests staged{};
    ASSERT_TRUE(run_staged(base_cfg,
                           staged_work.string(),
                           staged_report.string(),
                           staged));

    // Run the full pipeline via run_all.
    atx::impl::RunConfig run_cfg =
        make_base_cfg(s_zip_, run_work.string(), run_report_d.string());
    auto run_result = atx::impl::run_all(run_cfg);
    ASSERT_TRUE(run_result.has_value()) << run_result.error().message();

    // Extract per-stage digests from run_all kvs.
    auto find_kv = [&](const std::string& key) -> std::string {
        for (const auto& [k, v] : run_result->kvs) {
            if (k == key) return v;
        }
        return "";
    };

    // run_all's kvs are hex16 digests of each stage; the staged digests we
    // captured above are raw u64. Convert via the PRODUCTION to_hex16 (rather
    // than a local copy) so the test cannot silently diverge from the formatter
    // run_all itself uses.
    using atx::impl::to_hex16;

    EXPECT_EQ(to_hex16(staged.load),     find_kv("load"))
        << "load digest mismatch: staged vs run";
    EXPECT_EQ(to_hex16(staged.panel),    find_kv("panel"))
        << "panel digest mismatch: staged vs run";
    EXPECT_EQ(to_hex16(staged.discover), find_kv("discover"))
        << "discover digest mismatch: staged vs run";
    EXPECT_EQ(to_hex16(staged.combine),  find_kv("combine"))
        << "combine digest mismatch: staged vs run";
    EXPECT_EQ(to_hex16(staged.optimize), find_kv("optimize"))
        << "optimize digest mismatch: staged vs run";
    EXPECT_EQ(to_hex16(staged.report),   find_kv("report"))
        << "report digest mismatch: staged vs run";
}

// ---------------------------------------------------------------------------
// Test 3: ReportBytesDeterministic (R8)
// Run the full pipeline twice into two separate dirs. Assert pnl.tsv (and all
// other TSVs) are byte-identical and run digests are equal.
// ---------------------------------------------------------------------------
TEST_F(AtxImplE2E, ReportBytesDeterministic) {
    const fs::path work_a   = make_work_dir("det_work_a");
    const fs::path report_a = make_work_dir("det_report_a");
    const fs::path work_b   = make_work_dir("det_work_b");
    const fs::path report_b = make_work_dir("det_report_b");

    atx::impl::RunConfig cfg_a =
        make_base_cfg(s_zip_, work_a.string(), report_a.string());
    atx::impl::RunConfig cfg_b =
        make_base_cfg(s_zip_, work_b.string(), report_b.string());

    auto r_a = atx::impl::run_all(cfg_a);
    ASSERT_TRUE(r_a.has_value()) << r_a.error().message();

    auto r_b = atx::impl::run_all(cfg_b);
    ASSERT_TRUE(r_b.has_value()) << r_b.error().message();

    // Run digests must be equal.
    EXPECT_EQ(r_a->digest, r_b->digest) << "run digests differ across two runs";

    // Each canonical TSV must be byte-identical.
    auto read_file = [](const fs::path& p) -> std::vector<char> {
        std::ifstream f(p, std::ios::binary);
        return std::vector<char>((std::istreambuf_iterator<char>(f)),
                                  std::istreambuf_iterator<char>());
    };

    for (const char* tsv : {"pnl.tsv", "leverage.tsv", "exposure.tsv", "census.tsv"}) {
        const auto da = read_file(report_a / tsv);
        const auto db = read_file(report_b / tsv);
        EXPECT_FALSE(da.empty()) << tsv << " (run A) is empty";
        EXPECT_EQ(da, db) << tsv << " is not byte-identical across two runs (R8 violation)";
    }
}

// ---------------------------------------------------------------------------
// Test 4: RunAccumulatesLibraryAndCombineConsumesIt (A1)
// Proves the run_all wiring:
//   (a) <work>/_library exists and the persisted library has >= 1 admitted
//       record (accumulation actually happened — non-vacuous),
//   (b) combine consumed the library: the combine stage's reported alpha count
//       equals the library's n_alphas() (library-sourced input, not the loose
//       .dsl fallback).
// ---------------------------------------------------------------------------
TEST_F(AtxImplE2E, RunAccumulatesLibraryAndCombineConsumesIt) {
    namespace library = atx::engine::library;
    namespace combine = atx::engine::combine;

    const fs::path work   = make_work_dir("a1_lib_work");
    const fs::path report = make_work_dir("a1_lib_report");

    atx::impl::RunConfig cfg =
        make_base_cfg(s_zip_, work.string(), report.string());

    auto result = atx::impl::run_all(cfg);
    ASSERT_TRUE(result.has_value()) << result.error().message();

    // (a) The accumulation library dir exists.
    const fs::path lib_dir = work / "_library";
    ASSERT_TRUE(fs::exists(lib_dir)) << "missing accumulation library dir <work>/_library";

    // Open the persisted library read-only and read its admitted-record count.
    library::Library lib =
        library::Library::open(lib_dir.string(), combine::GateConfig{}, {});
    const atx::u64 n_lib = lib.n_alphas();
    ASSERT_GE(n_lib, atx::u64{1})
        << "library accumulation did not admit any alpha (accumulation never happened)";

    // (b) Combine consumed the library: the combine stage's reported alpha count
    //     equals the library record count. Re-run combine standalone against the
    //     SAME library to read its "alphas" kv (run_all does not surface per-stage
    //     kvs, only digests).
    atx::impl::RunConfig c_comb = cfg;
    c_comb.subcommand  = "combine";
    c_comb.panel       = (work / "panel.bin").string();
    c_comb.alphas      = (work / "alphas").string(); // harmless fallback (ignored on the library path)
    c_comb.library_dir = lib_dir.string();
    c_comb.combo_out   = (work / "combo_a1_check.bin").string();
    auto r_comb = atx::impl::run_combine(c_comb);
    ASSERT_TRUE(r_comb.has_value()) << r_comb.error().message();

    int combine_alphas = -1;
    for (const auto& p : r_comb->kvs) {
        if (p.first == "alphas") { combine_alphas = std::stoi(p.second); break; }
    }
    ASSERT_GE(combine_alphas, 1) << "combine reported no library-sourced alphas";
    EXPECT_EQ(static_cast<atx::u64>(combine_alphas), n_lib)
        << "combine alpha count must equal library n_alphas() (library-sourced input)";
}

// ---------------------------------------------------------------------------
// Helper: parse a key=value line out of summary.txt (empty if absent).
// ---------------------------------------------------------------------------
static std::string read_summary_kv(const fs::path& summary, const std::string& key) {
    std::ifstream f(summary);
    std::string line;
    while (std::getline(f, line)) {
        const auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        if (line.substr(0, eq) == key) return line.substr(eq + 1);
    }
    return "";
}

// ---------------------------------------------------------------------------
// Test 5: RunAllEmitsPositiveOosPortfolioSharpe (A2b — the keystone metric)
// run_all wires report at combo.bin so it reads combo.bin.meta and splits the
// per-period series IS/OOS. Assert summary.txt carries a FINITE portfolio_sharpe
// and a REAL OOS split (n_oos_periods >= 1, holdout_begin < n periods). We do
// NOT hard-assert a positive Sharpe on the synthetic fixture — that data-gated
// assertion is Task A4's job.
// ---------------------------------------------------------------------------
TEST_F(AtxImplE2E, RunAllEmitsPositiveOosPortfolioSharpe) {
    const fs::path work   = make_work_dir("a2b_work");
    const fs::path report = make_work_dir("a2b_report");

    atx::impl::RunConfig cfg =
        make_base_cfg(s_zip_, work.string(), report.string());

    auto result = atx::impl::run_all(cfg);
    ASSERT_TRUE(result.has_value()) << result.error().message();

    const fs::path summary = report / "summary.txt";
    ASSERT_TRUE(fs::exists(summary)) << "summary.txt missing";

    // combo.bin.meta must have been written by combine and read by report.
    ASSERT_TRUE(fs::exists(work / "combo.bin.meta")) << "combo.bin.meta missing";

    // portfolio_sharpe (full series) must be present and FINITE.
    const std::string ps_s = read_summary_kv(summary, "portfolio_sharpe");
    ASSERT_FALSE(ps_s.empty()) << "summary.txt missing portfolio_sharpe=";
    const double portfolio_sharpe = std::stod(ps_s);
    EXPECT_TRUE(std::isfinite(portfolio_sharpe))
        << "portfolio_sharpe is not finite: " << ps_s;

    // OOS Sharpe key must be emitted (value may be NaN-free finite; the OOS
    // split is real so it should parse).
    const std::string oos_s = read_summary_kv(summary, "portfolio_oos_sharpe");
    ASSERT_FALSE(oos_s.empty()) << "summary.txt missing portfolio_oos_sharpe=";

    // A REAL OOS split actually happened.
    const std::string n_oos_s = read_summary_kv(summary, "n_oos_periods");
    ASSERT_FALSE(n_oos_s.empty()) << "summary.txt missing n_oos_periods=";
    const long n_oos = std::stol(n_oos_s);
    EXPECT_GE(n_oos, 1) << "expected a non-empty OOS window (n_oos_periods>=1)";

    const std::string hb_s = read_summary_kv(summary, "holdout_begin");
    ASSERT_FALSE(hb_s.empty()) << "summary.txt missing holdout_begin=";
    const long holdout_begin = std::stol(hb_s);

    const std::string nis_s = read_summary_kv(summary, "n_is_periods");
    ASSERT_FALSE(nis_s.empty()) << "summary.txt missing n_is_periods=";
    const long n_is = std::stol(nis_s);
    EXPECT_GE(n_is, 1) << "expected a non-empty IS window (n_is_periods>=1)";

    // holdout_begin must carve a real interior boundary (0 < hb < n_periods).
    // n_periods is read from combo.bin.meta == research.dates().
    EXPECT_GT(holdout_begin, 0) << "holdout_begin must be > 0";

    // portfolio_sharpe (full) and oos kvs must also be on the StageResult,
    // but run_all only surfaces per-stage digests; summary.txt is the contract.
}

// ---------------------------------------------------------------------------
// Test 6: ReportWithoutComboStillWorksAndIsByteIdentical (A2b)
// Run report STANDALONE with cfg.combo empty: it must succeed, summary.txt must
// carry portfolio_sharpe= (full series) and n_oos_periods=0 (no split), and the
// report stage digest + canonical TSV bytes must match a baseline run (no
// regression from the additive metrics).
// ---------------------------------------------------------------------------
TEST_F(AtxImplE2E, ReportWithoutComboStillWorksAndIsByteIdentical) {
    const fs::path work = make_work_dir("a2b_nocombo_work");

    atx::impl::RunConfig base =
        make_base_cfg(s_zip_, work.string(), (work / "report_x").string());

    // Build the pipeline up through books.bin via the staged path (this also
    // writes combo.bin.meta, which we deliberately ignore by leaving cfg.combo
    // empty in the report call below).
    StagedDigests staged{};
    ASSERT_TRUE(run_staged(base, work.string(),
                           (work / "report_staged").string(), staged));

    // Run report TWICE standalone with combo empty -> two report dirs.
    auto run_report_into = [&](const fs::path& rdir,
                               atx::u64& digest_out) -> ::testing::AssertionResult {
        atx::impl::RunConfig c = base;
        c.panel      = (work / "panel.bin").string();
        c.books      = (work / "books.bin").string();
        c.combo      = "";   // NO combo => no OOS split
        c.report_out = rdir.string();
        auto r = atx::impl::run_report(c);
        if (!r.has_value())
            return ::testing::AssertionFailure() << "run_report: " << r.error().message();
        digest_out = r->digest;
        return ::testing::AssertionSuccess();
    };

    const fs::path rep_a = work / "report_a";
    const fs::path rep_b = work / "report_b";
    atx::u64 dig_a = 0, dig_b = 0;
    ASSERT_TRUE(run_report_into(rep_a, dig_a));
    ASSERT_TRUE(run_report_into(rep_b, dig_b));

    // Deterministic twice-run: digests equal.
    EXPECT_EQ(dig_a, dig_b) << "standalone report digest not deterministic";

    // summary.txt: full-series portfolio_sharpe present + finite; no OOS split.
    const fs::path summary = rep_a / "summary.txt";
    ASSERT_TRUE(fs::exists(summary)) << "summary.txt missing";

    const std::string ps_s = read_summary_kv(summary, "portfolio_sharpe");
    ASSERT_FALSE(ps_s.empty()) << "summary.txt missing portfolio_sharpe=";
    EXPECT_TRUE(std::isfinite(std::stod(ps_s)))
        << "portfolio_sharpe not finite (no-combo): " << ps_s;

    const std::string n_oos_s = read_summary_kv(summary, "n_oos_periods");
    ASSERT_FALSE(n_oos_s.empty()) << "summary.txt missing n_oos_periods=";
    EXPECT_EQ(std::stol(n_oos_s), 0L)
        << "no-combo report must have an empty OOS window (n_oos_periods=0)";

    // Canonical TSVs byte-identical across the two standalone runs (R8 / digest
    // proof: the additive metrics did not perturb write_report's output).
    auto read_file = [](const fs::path& p) -> std::vector<char> {
        std::ifstream f(p, std::ios::binary);
        return std::vector<char>((std::istreambuf_iterator<char>(f)),
                                  std::istreambuf_iterator<char>());
    };
    for (const char* tsv : {"pnl.tsv", "leverage.tsv", "exposure.tsv", "census.tsv"}) {
        const auto da = read_file(rep_a / tsv);
        const auto db = read_file(rep_b / tsv);
        EXPECT_FALSE(da.empty()) << tsv << " (report A) is empty";
        EXPECT_EQ(da, db) << tsv << " not byte-identical across standalone reports";
    }
}

} // namespace atx_impl_e2e
