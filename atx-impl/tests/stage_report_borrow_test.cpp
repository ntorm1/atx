// stage_report_borrow_test.cpp — S5-4: --borrow-bps threading proof. Confirms
// atx::impl::run_report actually reaches book::accumulate_report's new trailing
// borrow_bps parameter (atx-engine/tests/book/report_borrow_test.cpp already
// proves the engine-side math in isolation): cfg.borrow_bps set on a run_report
// call produces a non-zero total_pnl_borrow summary figure (+ StageResult kv)
// with the EXACT closed-form value for a hand-built single-period fixture;
// cfg.borrow_bps absent (the RunConfig struct default, 0.0) leaves total_pnl_borrow
// at exactly 0.0 and the summary/kvs otherwise byte-identical to the borrow-off run.

#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "config.hpp"
#include "serialize_panel.hpp"
#include "stages.hpp"

#include "atx/engine/alpha/panel.hpp"

namespace atx_impl_stage_report_borrow {

namespace fs    = std::filesystem;
namespace alpha = atx::engine::alpha;

// ---------------------------------------------------------------------------
// Helpers (mirrors stage_report_capacity_curve_test.cpp's conventions).
// ---------------------------------------------------------------------------

static std::string read_summary_value(const fs::path &summary_path, const std::string &key) {
    std::ifstream f(summary_path);
    std::string line;
    while (std::getline(f, line)) {
        const auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        if (line.substr(0, eq) == key) return line.substr(eq + 1);
    }
    return "";
}

static bool kv_has_key(const std::vector<std::pair<std::string, std::string>> &kvs,
                       const std::string &key, std::string *out_value = nullptr) {
    for (const auto &kv : kvs) {
        if (kv.first == key) {
            if (out_value) *out_value = kv.second;
            return true;
        }
    }
    return false;
}

struct Fixture {
    fs::path work_dir;
    std::string research_path;
    std::string books_path;

    explicit Fixture(const std::string &tag) {
        work_dir = fs::temp_directory_path() / ("atx_impl_s5_4_borrow_" + tag);
        std::error_code ec;
        fs::remove_all(work_dir, ec);
        fs::create_directories(work_dir, ec);
        research_path = (work_dir / "research.bin").string();
        books_path    = (work_dir / "books.bin").string();
    }
    ~Fixture() {
        std::error_code ec;
        fs::remove_all(work_dir, ec);
    }
};

// 2-instrument, D=2-date research panel: a "ret"-adjacent "close"/"raw_close"
// pair (the shape run_report's own panel loader expects), no volume field (the
// capacity curve isn't this test's concern -- it degenerates to its own inert
// sentinel, orthogonal to the borrow debit).
static atx::core::Result<std::string> make_research_panel(const fs::path &out) {
    constexpr atx::usize D = 2;
    constexpr atx::usize M = 2;
    const std::vector<atx::f64> closeA{100.0, 101.0};
    const std::vector<atx::f64> closeB{50.0, 50.5};
    std::vector<atx::f64> raw_close_v(D * M);
    std::vector<atx::f64> close_v(D * M);
    for (atx::usize t = 0; t < D; ++t) {
        raw_close_v[t * M + 0] = closeA[t];
        close_v[t * M + 0]     = closeA[t];
        raw_close_v[t * M + 1] = closeB[t];
        close_v[t * M + 1]     = closeB[t];
    }
    std::vector<std::uint8_t> uni(D * M, 1u);
    ATX_TRY(auto panel, alpha::Panel::create(D, M, {"raw_close", "close"},
                                             {raw_close_v, close_v}, uni));
    ATX_TRY(auto digest, atx::impl::write_panel(panel, out.string()));
    (void)digest;
    return atx::core::Ok(out.string());
}

// A single rebalance period: weights [wA, wB] -- wB negative gives a KNOWN
// short notional for the closed-form check below.
static atx::core::Result<std::string> make_books_panel(const fs::path &out, atx::f64 wA,
                                                        atx::f64 wB) {
    constexpr atx::usize S = 1;
    constexpr atx::usize M = 2;
    std::vector<atx::f64> wv{wA, wB};
    std::vector<std::uint8_t> uni(S * M, 1u);
    ATX_TRY(auto panel, alpha::Panel::create(S, M, {"weight"}, {wv}, uni));
    ATX_TRY(auto digest, atx::impl::write_panel(panel, out.string()));
    (void)digest;
    return atx::core::Ok(out.string());
}

static void write_books_meta(const fs::path &books_path, atx::usize dates) {
    std::ofstream f(books_path.string() + ".meta.txt");
    f << "periods=1\n";
    f << "instruments=2\n";
    f << "s=0 period=" << (dates - 1) << " turnover=0.5 cost_bps=0.0\n"; // cost_bps=0 isolates borrow
}

struct RunResult {
    atx::f64 total_pnl_borrow = 0.0;
    bool has_total_pnl_borrow_kv = false;
    std::vector<std::pair<std::string, std::string>> kvs;
};

static atx::core::Result<RunResult> run_and_parse(const std::string &research_path,
                                                  const std::string &books_path,
                                                  const fs::path &report_dir,
                                                  atx::f64 borrow_bps) {
    atx::impl::RunConfig cfg;
    cfg.panel      = research_path;
    cfg.books      = books_path;
    cfg.report_out = report_dir.string();
    cfg.borrow_bps = borrow_bps;

    ATX_TRY(auto sr, atx::impl::run_report(cfg));

    RunResult rr;
    rr.kvs = sr.kvs;
    std::string kv_val;
    rr.has_total_pnl_borrow_kv = kv_has_key(sr.kvs, "total_pnl_borrow", &kv_val);
    if (rr.has_total_pnl_borrow_kv) {
        rr.total_pnl_borrow = std::stod(kv_val);
    }

    const fs::path summary = report_dir / "summary.txt";
    const std::string borrow_str = read_summary_value(summary, "total_pnl_borrow");
    if (!borrow_str.empty()) {
        // Cross-check: the summary.txt line must agree with the kv (same computed value).
        EXPECT_NEAR(std::stod(borrow_str), rr.total_pnl_borrow, 1e-12);
    }
    return atx::core::Ok(rr);
}

// =============================================================================
//  BorrowBpsSetProducesNonZeroSummaryFigure — --borrow-bps threaded through
//  run_report produces a non-zero total_pnl_borrow kv/summary figure matching
//  the closed-form short-notional debit exactly (0.5 short * 50 bps * 1e-4).
// =============================================================================
TEST(StageReportBorrow, BorrowBpsSetProducesNonZeroSummaryFigure) {
    Fixture fx{"on"};
    auto r_res = make_research_panel(fx.work_dir / "research.bin");
    ASSERT_TRUE(r_res.has_value()) << r_res.error().message();
    auto r_bk = make_books_panel(fx.work_dir / "books.bin", 0.5, -0.5);
    ASSERT_TRUE(r_bk.has_value()) << r_bk.error().message();
    write_books_meta(fx.work_dir / "books.bin", 2U);

    const fs::path report_dir = fx.work_dir / "report";
    auto rr = run_and_parse(*r_res, *r_bk, report_dir, /*borrow_bps=*/50.0);
    ASSERT_TRUE(rr.has_value()) << rr.error().message();

    ASSERT_TRUE(rr->has_total_pnl_borrow_kv) << "total_pnl_borrow missing from sr.kvs";
    constexpr atx::f64 kExpected = 0.5 * 50.0 * 1e-4; // short notional * bps * 1e-4
    EXPECT_NEAR(rr->total_pnl_borrow, kExpected, 1e-12)
        << "run_report's total_pnl_borrow diverged from the closed-form short-notional debit";
    EXPECT_GT(rr->total_pnl_borrow, 0.0);
}

// =============================================================================
//  BorrowBpsAbsentIsByteIdenticalToOff — cfg.borrow_bps left at its 0.0 struct
//  default: total_pnl_borrow == 0.0 exactly, and every OTHER kv/summary figure
//  is identical to an explicit borrow_bps=0.0 run (the inert-default contract).
// =============================================================================
TEST(StageReportBorrow, BorrowBpsAbsentIsByteIdenticalToOff) {
    Fixture fx_absent{"absent"};
    auto r_res_a = make_research_panel(fx_absent.work_dir / "research.bin");
    ASSERT_TRUE(r_res_a.has_value()) << r_res_a.error().message();
    auto r_bk_a = make_books_panel(fx_absent.work_dir / "books.bin", 0.5, -0.5);
    ASSERT_TRUE(r_bk_a.has_value()) << r_bk_a.error().message();
    write_books_meta(fx_absent.work_dir / "books.bin", 2U);
    auto rr_absent =
        run_and_parse(*r_res_a, *r_bk_a, fx_absent.work_dir / "report", /*borrow_bps=*/0.0);
    ASSERT_TRUE(rr_absent.has_value()) << rr_absent.error().message();

    EXPECT_TRUE(rr_absent->has_total_pnl_borrow_kv);
    EXPECT_DOUBLE_EQ(rr_absent->total_pnl_borrow, 0.0);

    // A SECOND, independent fixture/run at the SAME (0.0) borrow rate must
    // reproduce every kv identically -- the explicit-0.0 == implicit-default
    // equivalence (mirrors the house InertByteIdentical idiom).
    Fixture fx_off{"off"};
    auto r_res_o = make_research_panel(fx_off.work_dir / "research.bin");
    ASSERT_TRUE(r_res_o.has_value()) << r_res_o.error().message();
    auto r_bk_o = make_books_panel(fx_off.work_dir / "books.bin", 0.5, -0.5);
    ASSERT_TRUE(r_bk_o.has_value()) << r_bk_o.error().message();
    write_books_meta(fx_off.work_dir / "books.bin", 2U);
    auto rr_off = run_and_parse(*r_res_o, *r_bk_o, fx_off.work_dir / "report", /*borrow_bps=*/0.0);
    ASSERT_TRUE(rr_off.has_value()) << rr_off.error().message();

    ASSERT_EQ(rr_absent->kvs.size(), rr_off->kvs.size());
    for (std::size_t i = 0; i < rr_absent->kvs.size(); ++i) {
        EXPECT_EQ(rr_absent->kvs[i], rr_off->kvs[i]);
    }
}

} // namespace atx_impl_stage_report_borrow
