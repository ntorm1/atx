// atx::impl — T1 transaction-cost model tests (TDD).
//
// Tests:
//   CostBpsPositionMode_CostPositive      — cost_bps>0 -> total_pnl_cost>0 (position-mode)
//   CostBpsMvo_CostPositive               — cost_bps>0 -> total_pnl_cost>0 (MVO path)
//   CostBpsNetSharpeBelow Gross_PosMode   — net Sharpe < gross Sharpe when cost_bps>0
//   CostBpsHigherTurnoverLargerDrag       — higher turnover -> larger cost drag
//   CostBpsZero_ByteIdenticalPanel        — cost_bps=0 -> book panel digest unchanged
//   CostBpsZero_TotalCostIsZero           — cost_bps=0 -> total_pnl_cost == 0
//   CostBpsParsedFromCLI                  — --cost-bps parsed correctly; 0 default
//   CostBpsUnsetIsByteIdenticalPanel      — absent --cost-bps flag -> panel byte-identical
//                                           to default (set_flags pattern)

#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <numeric>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "artifacts.hpp"
#include "config.hpp"
#include "serialize_panel.hpp"
#include "stages.hpp"

#include "atx/engine/alpha/panel.hpp"

namespace atx_impl_cost_bps {

namespace fs    = std::filesystem;
namespace alpha = atx::engine::alpha;

// ---------------------------------------------------------------------------
// Panel builders
// ---------------------------------------------------------------------------

// Small research panel (M instruments, D dates, field "close").
// Non-NaN, non-zero prices with per-instrument drift so return variance is
// well above the risk-model floor.
static atx::core::Result<std::string>
make_research_panel(const fs::path& out, atx::usize M, atx::usize D)
{
    std::vector<atx::f64> close_data;
    close_data.reserve(D * M);
    for (atx::usize t = 0; t < D; ++t) {
        for (atx::usize i = 0; i < M; ++i) {
            const atx::f64 drift =
                0.0002 * (1.0 + static_cast<atx::f64>(i) * 0.1);
            close_data.push_back(
                100.0 * std::exp(drift * static_cast<atx::f64>(t)));
        }
    }
    std::vector<std::uint8_t> uni(D * M, 1u);
    ATX_TRY(auto panel,
            alpha::Panel::create(D, M, {"close"}, {close_data}, uni));
    ATX_TRY(auto digest, atx::impl::write_panel(panel, out.string()));
    (void)digest;
    return atx::core::Ok(out.string());
}

// "Whippy" combo panel: alpha cross-section flips sign every period.
// This maximises turnover so transaction costs have a measurable impact.
static atx::core::Result<std::string>
make_whippy_combo_panel(const fs::path& out, atx::usize M, atx::usize D)
{
    std::vector<atx::f64> alpha_data;
    alpha_data.reserve(D * M);
    for (atx::usize t = 0; t < D; ++t) {
        const atx::f64 sign = (t % 2 == 0) ? 1.0 : -1.0;
        for (atx::usize i = 0; i < M; ++i) {
            alpha_data.push_back(
                sign * (static_cast<atx::f64>(i) -
                        static_cast<atx::f64>(M) / 2.0));
        }
    }
    std::vector<std::uint8_t> uni(D * M, 1u);
    ATX_TRY(auto panel,
            alpha::Panel::create(D, M, {"alpha"}, {alpha_data}, uni));
    ATX_TRY(auto digest, atx::impl::write_panel(panel, out.string()));
    (void)digest;
    return atx::core::Ok(out.string());
}

// Steady combo panel: alpha is a fixed cross-sectional rank (low turnover).
// Used for the "higher turnover => larger drag" comparative test.
static atx::core::Result<std::string>
make_steady_combo_panel(const fs::path& out, atx::usize M, atx::usize D)
{
    std::vector<atx::f64> alpha_data;
    alpha_data.reserve(D * M);
    for (atx::usize t = 0; t < D; ++t) {
        for (atx::usize i = 0; i < M; ++i) {
            // Same signal every period => near-zero turnover after s=0.
            alpha_data.push_back(
                static_cast<atx::f64>(i) - static_cast<atx::f64>(M) / 2.0);
        }
    }
    std::vector<std::uint8_t> uni(D * M, 1u);
    ATX_TRY(auto panel,
            alpha::Panel::create(D, M, {"alpha"}, {alpha_data}, uni));
    ATX_TRY(auto digest, atx::impl::write_panel(panel, out.string()));
    (void)digest;
    return atx::core::Ok(out.string());
}

// ---------------------------------------------------------------------------
// Sidecar helpers
// ---------------------------------------------------------------------------

// Parse "total_pnl_cost=..." from summary.txt.
static double read_summary_double(const fs::path& summary, const std::string& key)
{
    std::ifstream f(summary);
    std::string line;
    while (std::getline(f, line)) {
        const auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        if (line.substr(0, eq) == key)
            return std::stod(line.substr(eq + 1));
    }
    return std::numeric_limits<double>::quiet_NaN();
}

// ---------------------------------------------------------------------------
// Fixture
// ---------------------------------------------------------------------------

class CostBpsTest : public ::testing::Test {
protected:
    static constexpr atx::usize kM = 20;
    static constexpr atx::usize kD = 80;

    fs::path     work_dir_;
    std::string  research_path_;
    std::string  whippy_combo_path_;  // high-turnover
    std::string  steady_combo_path_;  // low-turnover

    void SetUp() override {
        work_dir_ = fs::temp_directory_path() / "atx_impl_cost_bps_test";
        std::error_code ec;
        fs::remove_all(work_dir_, ec);
        fs::create_directories(work_dir_, ec);

        auto rr = make_research_panel(work_dir_ / "research.bin", kM, kD);
        ASSERT_TRUE(rr.has_value()) << rr.error().message();
        research_path_ = *rr;

        auto wr = make_whippy_combo_panel(work_dir_ / "whippy.bin", kM, kD);
        ASSERT_TRUE(wr.has_value()) << wr.error().message();
        whippy_combo_path_ = *wr;

        auto sr = make_steady_combo_panel(work_dir_ / "steady.bin", kM, kD);
        ASSERT_TRUE(sr.has_value()) << sr.error().message();
        steady_combo_path_ = *sr;
    }

    void TearDown() override {
        std::error_code ec;
        fs::remove_all(work_dir_, ec);
    }

    // Run optimize + report and return total_pnl_cost and portfolio_oos_sharpe.
    // `tag` is used to name unique output dirs so concurrent runs don't collide.
    struct RunResult {
        double total_pnl_cost  = 0.0;
        double total_pnl_net   = 0.0;
        double portfolio_sharpe = 0.0;
        atx::u64 books_digest  = 0;
    };

    atx::core::Result<RunResult> run_pipeline(
        const std::string& combo_path,
        double             cost_bps_val,
        bool               set_cost_flag,
        bool               position_mode,
        const std::string& tag)
    {
        const fs::path books_path  = work_dir_ / (tag + "_books.bin");
        const fs::path report_path = work_dir_ / (tag + "_report");
        std::error_code ec;
        fs::create_directories(report_path, ec);

        // --- Optimize ---
        atx::impl::RunConfig opt_cfg;
        opt_cfg.panel         = research_path_;
        opt_cfg.combo         = combo_path;
        opt_cfg.books_out     = books_path.string();
        opt_cfg.gross         = 1.0;
        opt_cfg.name_cap      = 0.5;
        opt_cfg.rebalance     = "weekly";
        opt_cfg.position_mode = position_mode;
        opt_cfg.cost_bps      = cost_bps_val;
        if (set_cost_flag) opt_cfg.set_flags.emplace("cost-bps");
        if (!position_mode) {
            opt_cfg.risk_aversion = 1.0;
            opt_cfg.set_flags.emplace("risk-aversion");
        }

        ATX_TRY(auto opt_sr, atx::impl::run_optimize(opt_cfg));
        const atx::u64 books_digest = opt_sr.digest;

        // --- Report ---
        atx::impl::RunConfig rep_cfg;
        rep_cfg.panel      = research_path_;
        rep_cfg.books      = books_path.string();
        rep_cfg.report_out = report_path.string();
        rep_cfg.report_aum = 1e9;

        ATX_TRY(auto rep_sr, atx::impl::run_report(rep_cfg));
        (void)rep_sr;

        const fs::path summary = report_path / "summary.txt";
        RunResult res;
        res.books_digest    = books_digest;
        res.total_pnl_cost  = read_summary_double(summary, "total_pnl_cost");
        res.total_pnl_net   = read_summary_double(summary, "total_pnl_net");
        res.portfolio_sharpe = read_summary_double(summary, "portfolio_sharpe");
        return atx::core::Ok(res);
    }
};

// ---------------------------------------------------------------------------
// Test 1 (position-mode): cost_bps > 0 => total_pnl_cost > 0
// ---------------------------------------------------------------------------
TEST_F(CostBpsTest, CostBpsPositionMode_CostPositive)
{
    // High cost so the drag is measurable even on short synthetic series.
    auto r = run_pipeline(whippy_combo_path_, /*cost_bps=*/50.0,
                          /*set_cost_flag=*/true,
                          /*position_mode=*/true,
                          "pm_cost50");
    ASSERT_TRUE(r.has_value()) << r.error().message();
    EXPECT_GT(r->total_pnl_cost, 0.0)
        << "position-mode cost_bps=50 must yield total_pnl_cost > 0; got "
        << r->total_pnl_cost;
}

// ---------------------------------------------------------------------------
// Test 2 (MVO path): cost_bps > 0 => total_pnl_cost > 0
// ---------------------------------------------------------------------------
TEST_F(CostBpsTest, CostBpsMvo_CostPositive)
{
    auto r = run_pipeline(whippy_combo_path_, /*cost_bps=*/50.0,
                          /*set_cost_flag=*/true,
                          /*position_mode=*/false,
                          "mvo_cost50");
    ASSERT_TRUE(r.has_value()) << r.error().message();
    EXPECT_GT(r->total_pnl_cost, 0.0)
        << "MVO cost_bps=50 must yield total_pnl_cost > 0; got "
        << r->total_pnl_cost;
}

// ---------------------------------------------------------------------------
// Test 3: net Sharpe < gross Sharpe when cost_bps > 0 (position-mode).
//
// portfolio_sharpe in summary.txt is computed from pnl_net (which embeds the
// cost drag).  We compare cost_bps=0 run (gross ≈ net) against cost_bps>0
// run (net < gross) on the same whippy (high-turnover) combo.
// ---------------------------------------------------------------------------
TEST_F(CostBpsTest, CostBpsNetSharpeBelowGross_PosMode)
{
    auto r0 = run_pipeline(whippy_combo_path_, /*cost_bps=*/0.0,
                           /*set_cost_flag=*/true,
                           /*position_mode=*/true,
                           "pm_sharpe_gross");
    ASSERT_TRUE(r0.has_value()) << r0.error().message();

    auto r50 = run_pipeline(whippy_combo_path_, /*cost_bps=*/50.0,
                            /*set_cost_flag=*/true,
                            /*position_mode=*/true,
                            "pm_sharpe_net50");
    ASSERT_TRUE(r50.has_value()) << r50.error().message();

    // Net Sharpe (high cost) must be strictly below gross-Sharpe (zero cost).
    EXPECT_LT(r50->portfolio_sharpe, r0->portfolio_sharpe)
        << "Expected net Sharpe (" << r50->portfolio_sharpe
        << ") < gross Sharpe (" << r0->portfolio_sharpe << ")";
}

// ---------------------------------------------------------------------------
// Test 4: higher turnover => larger cost drag (same cost_bps, same panel).
//
// The whippy combo (sign-flipping every period) produces maximum turnover.
// The steady combo produces minimum turnover AFTER period 0: once the book
// is set at s=0 the signal never changes, so turnover[s] == 0 for s >= 1.
// At s=0 both combos start from prev=0 so turnover[0] is equal.
// Over 16 weekly periods (D=80), whippy has 16x that of steady.
// We assert on the SIDECAR turnover directly to verify the setup, then
// verify that total_pnl_cost scales proportionally.
//
// "Truly steady": all alpha values identical across all dates so shape_book
// produces the same book every period => turnover[s]=0 for s>=1.
TEST_F(CostBpsTest, CostBpsHigherTurnoverLargerDrag)
{
    const double kBps = 50.0;

    // Parse total turnover from the sidecar .meta.txt produced by optimize.
    auto parse_total_turnover = [](const std::string& meta_path) -> double {
        std::ifstream f(meta_path);
        if (!f.is_open()) return -1.0;
        double total = 0.0;
        std::string line;
        while (std::getline(f, line)) {
            if (line.rfind("s=", 0) != 0) continue;
            const auto pos = line.find("turnover=");
            if (pos == std::string::npos) continue;
            const auto end = line.find(' ', pos);
            const std::string val = (end == std::string::npos)
                ? line.substr(pos + 9)
                : line.substr(pos + 9, end - pos - 9);
            total += std::stod(val);
        }
        return total;
    };

    // Run whippy (high-turnover).
    const fs::path whippy_books = work_dir_ / "drag_whippy_books.bin";
    {
        atx::impl::RunConfig cfg;
        cfg.panel         = research_path_;
        cfg.combo         = whippy_combo_path_;
        cfg.books_out     = whippy_books.string();
        cfg.gross         = 1.0;
        cfg.name_cap      = 0.5;
        cfg.rebalance     = "weekly";
        cfg.position_mode = true;
        cfg.cost_bps      = kBps;
        cfg.set_flags.emplace("cost-bps");
        auto r = atx::impl::run_optimize(cfg);
        ASSERT_TRUE(r.has_value()) << r.error().message();
    }
    // Run steady (low-turnover).
    const fs::path steady_books = work_dir_ / "drag_steady_books.bin";
    {
        atx::impl::RunConfig cfg;
        cfg.panel         = research_path_;
        cfg.combo         = steady_combo_path_;
        cfg.books_out     = steady_books.string();
        cfg.gross         = 1.0;
        cfg.name_cap      = 0.5;
        cfg.rebalance     = "weekly";
        cfg.position_mode = true;
        cfg.cost_bps      = kBps;
        cfg.set_flags.emplace("cost-bps");
        auto r = atx::impl::run_optimize(cfg);
        ASSERT_TRUE(r.has_value()) << r.error().message();
    }

    const double tv_whippy = parse_total_turnover(whippy_books.string() + ".meta.txt");
    const double tv_steady = parse_total_turnover(steady_books.string() + ".meta.txt");

    ASSERT_GE(tv_whippy, 0.0) << "could not parse whippy turnover";
    ASSERT_GE(tv_steady, 0.0) << "could not parse steady turnover";

    // Whippy must have strictly more total turnover than steady.
    EXPECT_GT(tv_whippy, tv_steady)
        << "whippy total turnover (" << tv_whippy
        << ") must exceed steady (" << tv_steady << ")";

    // With the same cost_bps, higher turnover => larger total_pnl_cost.
    // Run the full optimize+report pipeline for each and compare.
    auto r_whippy = run_pipeline(whippy_combo_path_, kBps,
                                 /*set_cost_flag=*/true,
                                 /*position_mode=*/true,
                                 "drag2_whippy");
    ASSERT_TRUE(r_whippy.has_value()) << r_whippy.error().message();

    auto r_steady = run_pipeline(steady_combo_path_, kBps,
                                 /*set_cost_flag=*/true,
                                 /*position_mode=*/true,
                                 "drag2_steady");
    ASSERT_TRUE(r_steady.has_value()) << r_steady.error().message();

    EXPECT_GT(r_whippy->total_pnl_cost, r_steady->total_pnl_cost)
        << "High-turnover (whippy) combo must have larger cost drag than "
           "low-turnover (steady) combo at the same cost_bps";
}

// ---------------------------------------------------------------------------
// Test 5: cost_bps=0 (default) => book panel digest unchanged.
//
// A run with cost_bps=0 (set_cost_flag=true) must produce the SAME books
// digest as a run where the flag is absent (set_cost_flag=false). The weight
// panel is determined by weights only, not by cost_bps, so both must agree.
// ---------------------------------------------------------------------------
TEST_F(CostBpsTest, CostBpsZero_ByteIdenticalPanel)
{
    // Run A: cost_bps explicitly 0, flag present.
    auto r_set = run_pipeline(whippy_combo_path_, /*cost_bps=*/0.0,
                              /*set_cost_flag=*/true,
                              /*position_mode=*/true,
                              "panel_zero_set");
    ASSERT_TRUE(r_set.has_value()) << r_set.error().message();

    // Run B: flag absent (default cost_bps=0).
    auto r_unset = run_pipeline(whippy_combo_path_, /*cost_bps=*/0.0,
                                /*set_cost_flag=*/false,
                                /*position_mode=*/true,
                                "panel_zero_unset");
    ASSERT_TRUE(r_unset.has_value()) << r_unset.error().message();

    EXPECT_EQ(r_set->books_digest, r_unset->books_digest)
        << "cost_bps=0 run must produce the same book panel digest as the "
           "default (flag-absent) run";

    // Also verify a cost_bps=50 run has the SAME digest (cost only flows into
    // the sidecar / pnl_net, not into the weight panel itself).
    auto r_50 = run_pipeline(whippy_combo_path_, /*cost_bps=*/50.0,
                             /*set_cost_flag=*/true,
                             /*position_mode=*/true,
                             "panel_bps50");
    ASSERT_TRUE(r_50.has_value()) << r_50.error().message();

    EXPECT_EQ(r_set->books_digest, r_50->books_digest)
        << "cost_bps=50 must NOT change the book panel digest (cost flows "
           "only into sidecar/pnl_net)";
}

// ---------------------------------------------------------------------------
// Test 6: cost_bps=0 => total_pnl_cost == 0 (exactly).
// ---------------------------------------------------------------------------
TEST_F(CostBpsTest, CostBpsZero_TotalCostIsZero)
{
    auto r = run_pipeline(whippy_combo_path_, /*cost_bps=*/0.0,
                          /*set_cost_flag=*/true,
                          /*position_mode=*/true,
                          "zero_cost_check");
    ASSERT_TRUE(r.has_value()) << r.error().message();
    EXPECT_NEAR(r->total_pnl_cost, 0.0, 1e-12)
        << "cost_bps=0 must yield total_pnl_cost == 0; got "
        << r->total_pnl_cost;
}

// ---------------------------------------------------------------------------
// Test 7: --cost-bps parsed from CLI correctly; default is 0.
// ---------------------------------------------------------------------------
TEST(CostBpsConfig, ParsedFromCLI)
{
    // Explicit value.
    {
        const char* argv[] = {"atx", "optimize", "--cost-bps", "25.5"};
        auto r = atx::impl::parse_args(4, const_cast<char**>(argv));
        ASSERT_TRUE(r.has_value()) << r.error().message();
        EXPECT_NEAR(r->cost_bps, 25.5, 1e-12) << "--cost-bps 25.5 not parsed";
        EXPECT_TRUE(r->set_flags.count("cost-bps"))
            << "cost-bps must be in set_flags after CLI parse";
    }
    // Default (flag absent).
    {
        const char* argv[] = {"atx", "optimize"};
        auto r = atx::impl::parse_args(2, const_cast<char**>(argv));
        ASSERT_TRUE(r.has_value()) << r.error().message();
        EXPECT_NEAR(r->cost_bps, 0.0, 1e-12) << "default cost_bps must be 0.0";
        EXPECT_FALSE(r->set_flags.count("cost-bps"))
            << "cost-bps must NOT be in set_flags when absent";
    }
    // Zero explicit.
    {
        const char* argv[] = {"atx", "optimize", "--cost-bps", "0"};
        auto r = atx::impl::parse_args(4, const_cast<char**>(argv));
        ASSERT_TRUE(r.has_value()) << r.error().message();
        EXPECT_NEAR(r->cost_bps, 0.0, 1e-12);
        EXPECT_TRUE(r->set_flags.count("cost-bps"))
            << "cost-bps must be in set_flags for explicit 0";
    }
    // Negative value (invalid cost).
    {
        const char* argv[] = {"atx", "optimize", "--cost-bps", "-1"};
        auto r = atx::impl::parse_args(4, const_cast<char**>(argv));
        EXPECT_FALSE(r.has_value())
            << "--cost-bps -1 should be rejected";
        if (!r.has_value()) {
            EXPECT_EQ(r.error().code(), atx::core::ErrorCode::InvalidArgument);
        }
    }
}

} // namespace atx_impl_cost_bps
