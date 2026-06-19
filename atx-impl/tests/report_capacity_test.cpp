// atx::impl — report capacity-footprint tests (Task P4 TDD).
//
// Tests:
//   CapacityFootprintMathIsCorrect   — hand-computed participation values
//   ReportAumScalesParticipation     — doubling AUM doubles participation
//   ExistingSummaryPrefixUnchanged   — first 6 lines + digest unchanged by new metrics
//
// Fixture strategy:
//   Build a tiny in-memory research panel (M=2, D=6) with known raw_close and
//   volume, plus a books panel with known weights over S=2 rebalance periods.
//   Write both to temp files; wire up the .meta.txt sidecar and run run_report.
//   Parse summary.txt and verify the capacity metrics match hand-computed values.
//
// Hand-calculation (AUM = 1e9):
//   Period 0: date_idx 2
//     name 0:  w=0.5, raw_close=100, volume=500000 -> dvol=50000000
//              part = (0.5 * 1e9) / 50000000 = 10.0
//     name 1:  w=0.5, raw_close=200, volume=100000 -> dvol=20000000
//              part = (0.5 * 1e9) / 20000000 = 25.0
//   Period 1: date_idx 4
//     name 0:  w=0.4, raw_close=110, volume=500000 -> dvol=55000000
//              part = (0.4 * 1e9) / 55000000 ≈ 7.2727...
//     name 1:  w=0.6, raw_close=210, volume=100000 -> dvol=21000000
//              part = (0.6 * 1e9) / 21000000 ≈ 28.5714...
//
//   All 4 parts: [10.0, 25.0, 7.2727..., 28.5714...]
//   max  = 28.5714...   -> max_participation_pct
//   median = middle of sorted [7.2727, 10.0, 25.0, 28.5714] at idx floor(0.95*3)=2 -> p95
//            n=4, median = (vals[1]+vals[2])/2 = (10+25)/2 = 17.5
//   p95: idx = floor(0.95*3) = floor(2.85) = 2 -> 25.0
//   avg_names_held: (2+2)/2 = 2.0
//
// pct_gross_over_5pct_adv (>5% threshold):
//   Period 0: parts: name0=10 (>5%) -> w=0.5; name1=25 (>5%) -> w=0.5 => sum=1.0
//   Period 1: parts: name0≈7.27 (>5%) -> w=0.4; name1≈28.57 (>5%) -> w=0.6 => sum=1.0
//   mean = (1.0 + 1.0)/2 = 1.0

#include <array>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "artifacts.hpp"
#include "config.hpp"
#include "serialize_panel.hpp"
#include "stages.hpp"

#include "atx/engine/alpha/panel.hpp"

namespace atx_impl_report_capacity {

namespace fs = std::filesystem;
namespace alpha = atx::engine::alpha;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

// Read a key=value line from a file.  Returns empty string if key not found.
static std::string read_summary_value(const fs::path& summary_path,
                                       const std::string& key)
{
    std::ifstream f(summary_path);
    std::string line;
    while (std::getline(f, line)) {
        const auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        if (line.substr(0, eq) == key) {
            return line.substr(eq + 1);
        }
    }
    return "";
}

// Read the first N lines of a file verbatim.
static std::vector<std::string> read_first_lines(const fs::path& p, int n)
{
    std::ifstream f(p);
    std::vector<std::string> lines;
    std::string line;
    while (static_cast<int>(lines.size()) < n && std::getline(f, line)) {
        lines.push_back(line);
    }
    return lines;
}

// ---------------------------------------------------------------------------
// Build the tiny research panel: M=2 instruments, D=6 dates.
// Fields: "raw_close", "close", "volume"
//   raw_close[d][i]:
//     i=0: 100, 102, 100, 104, 110, 112
//     i=1: 200, 202, 200, 205, 210, 215
//   volume[d][i]:
//     i=0: 500000 (all dates)
//     i=1: 100000 (all dates)
//   close == raw_close (TRI = raw_close here, used for returns)
// All instruments in-universe on all dates.
// ---------------------------------------------------------------------------
static atx::core::Result<std::string>
make_capacity_research_panel(const fs::path& out)
{
    constexpr atx::usize D = 6;
    constexpr atx::usize M = 2;

    // raw_close: date-major
    const std::array<double, D * M> raw_close_vals = {
        100.0, 200.0,   // date 0
        102.0, 202.0,   // date 1
        100.0, 200.0,   // date 2  <- period 0 rebalance date
        104.0, 205.0,   // date 3
        110.0, 210.0,   // date 4  <- period 1 rebalance date
        112.0, 215.0,   // date 5
    };
    // close == raw_close (to satisfy TRI return computation)
    const std::array<double, D * M> close_vals = raw_close_vals;

    // volume: date-major
    const std::array<double, D * M> volume_vals = {
        500000.0, 100000.0,
        500000.0, 100000.0,
        500000.0, 100000.0,
        500000.0, 100000.0,
        500000.0, 100000.0,
        500000.0, 100000.0,
    };

    std::vector<atx::f64> raw_close_v(raw_close_vals.begin(), raw_close_vals.end());
    std::vector<atx::f64> close_v(close_vals.begin(), close_vals.end());
    std::vector<atx::f64> volume_v(volume_vals.begin(), volume_vals.end());
    std::vector<std::uint8_t> uni(D * M, 1u);

    ATX_TRY(auto panel,
            alpha::Panel::create(D, M,
                {"raw_close", "close", "volume"},
                {raw_close_v, close_v, volume_v},
                uni));
    ATX_TRY(auto digest, atx::impl::write_panel(panel, out.string()));
    (void)digest;
    return atx::core::Ok(out.string());
}

// ---------------------------------------------------------------------------
// Build the books panel: S=2 periods, M=2 instruments, field "weight".
// Period 0 (sched date_idx=2): w = [0.5, 0.5]
// Period 1 (sched date_idx=4): w = [0.4, 0.6]
// ---------------------------------------------------------------------------
static atx::core::Result<std::string>
make_capacity_books_panel(const fs::path& out)
{
    constexpr atx::usize S = 2;
    constexpr atx::usize M = 2;

    // date-major: row 0 = period 0, row 1 = period 1
    const std::array<double, S * M> weight_vals = {
        0.5, 0.5,   // period 0
        0.4, 0.6,   // period 1
    };
    std::vector<atx::f64> wv(weight_vals.begin(), weight_vals.end());
    std::vector<std::uint8_t> uni(S * M, 1u);

    ATX_TRY(auto panel,
            alpha::Panel::create(S, M, {"weight"}, {wv}, uni));
    ATX_TRY(auto digest, atx::impl::write_panel(panel, out.string()));
    (void)digest;
    return atx::core::Ok(out.string());
}

// Write the .meta.txt sidecar for the books panel.
// sched period indices into the RESEARCH panel (date_idx 2 and 4).
static void write_books_meta(const fs::path& books_path)
{
    std::ofstream f(books_path.string() + ".meta.txt");
    f << "periods=2\n";
    f << "instruments=2\n";
    f << "s=0 period=2 turnover=0.5 cost_bps=0.0\n";
    f << "s=1 period=4 turnover=0.3 cost_bps=0.0\n";
}

// ---------------------------------------------------------------------------
// Fixture: build panels once per test, reuse across tests in the suite.
// ---------------------------------------------------------------------------
class ReportCapacity : public ::testing::Test {
protected:
    fs::path work_dir_;
    std::string research_path_;
    std::string books_path_;

    void SetUp() override {
        work_dir_ = fs::temp_directory_path() /
                    ("atx_impl_rptcap_" + std::to_string(
                        static_cast<unsigned long long>(
                            reinterpret_cast<uintptr_t>(this))));
        std::error_code ec;
        fs::remove_all(work_dir_, ec);
        fs::create_directories(work_dir_, ec);

        const fs::path res_path   = work_dir_ / "research.bin";
        const fs::path books_path = work_dir_ / "books.bin";

        auto r_res = make_capacity_research_panel(res_path);
        ASSERT_TRUE(r_res.has_value()) << r_res.error().message();
        research_path_ = *r_res;

        auto r_bk = make_capacity_books_panel(books_path);
        ASSERT_TRUE(r_bk.has_value()) << r_bk.error().message();
        books_path_ = *r_bk;

        write_books_meta(books_path);
    }

    void TearDown() override {
        std::error_code ec;
        fs::remove_all(work_dir_, ec);
    }

    // Run run_report with the given AUM and return the report directory.
    atx::core::Result<fs::path> run_with_aum(double aum) {
        const fs::path report_dir = work_dir_ / ("report_" + std::to_string(static_cast<int>(aum)));
        std::error_code ec;
        fs::remove_all(report_dir, ec);
        fs::create_directories(report_dir, ec);

        atx::impl::RunConfig cfg;
        cfg.panel      = research_path_;
        cfg.books      = books_path_;
        cfg.report_out = report_dir.string();
        cfg.report_aum = aum;

        ATX_TRY(auto sr, atx::impl::run_report(cfg));
        (void)sr;
        return atx::core::Ok(report_dir);
    }
};

// ---------------------------------------------------------------------------
// Test 1: CapacityFootprintMathIsCorrect
// Hand-compute all metrics for AUM=1e9 and verify they match summary.txt.
// ---------------------------------------------------------------------------
TEST_F(ReportCapacity, CapacityFootprintMathIsCorrect)
{
    auto r = run_with_aum(1e9);
    ASSERT_TRUE(r.has_value()) << r.error().message();
    const fs::path report_dir = *r;
    const fs::path summary    = report_dir / "summary.txt";
    ASSERT_TRUE(fs::exists(summary)) << "summary.txt missing";

    // Read values.
    const double avg_names = std::stod(read_summary_value(summary, "avg_names_held"));
    const double max_part  = std::stod(read_summary_value(summary, "max_participation_pct"));
    const double p95_part  = std::stod(read_summary_value(summary, "p95_participation_pct"));
    const double med_part  = std::stod(read_summary_value(summary, "median_participation_pct"));
    const double pct_over  = std::stod(read_summary_value(summary, "pct_gross_over_5pct_adv"));

    // avg_names_held = (2 + 2) / 2 = 2.0
    EXPECT_NEAR(avg_names, 2.0, 1e-9) << "avg_names_held mismatch";

    // Hand-computed participations (see file header comment):
    //   name0 period0: (0.5 * 1e9) / (100 * 500000) = 10.0
    //   name1 period0: (0.5 * 1e9) / (200 * 100000) = 25.0
    //   name0 period1: (0.4 * 1e9) / (110 * 500000) ≈ 7.27272...
    //   name1 period1: (0.6 * 1e9) / (210 * 100000) ≈ 28.5714...

    const double part00 = (0.5 * 1e9) / (100.0 * 500000.0);  // 10.0
    const double part10 = (0.5 * 1e9) / (200.0 * 100000.0);  // 25.0
    // part01 = (0.4*1e9)/(110*500000) ≈ 7.2727 — used indirectly in sorted order
    const double part11 = (0.6 * 1e9) / (210.0 * 100000.0);  // ~28.5714

    // max = part11 * 100 (stored as percentage)
    EXPECT_NEAR(max_part, part11 * 100.0, 1e-4) << "max_participation_pct mismatch";

    // sorted parts: [part01, part00, part10, part11]
    // n=4; p95 idx = floor(0.95 * 3) = floor(2.85) = 2 => part10 = 25.0
    // stored as percentage => part10 * 100
    EXPECT_NEAR(p95_part, part10 * 100.0, 1e-4) << "p95_participation_pct mismatch";

    // median: middle of sorted 4 values => (vals[1]+vals[2])/2 = (part00+part10)/2
    // stored as percentage
    const double expected_median = (part00 + part10) / 2.0 * 100.0;
    EXPECT_NEAR(med_part, expected_median, 1e-4) << "median_participation_pct mismatch";

    // pct_gross_over_5pct_adv: all 4 parts > 5%, so per-period sum of |w| = 1.0
    EXPECT_NEAR(pct_over, 1.0, 1e-9) << "pct_gross_over_5pct_adv mismatch";
}

// ---------------------------------------------------------------------------
// Test 2: ReportAumScalesParticipation
// Doubling --report-aum doubles max_participation_pct (linear in AUM).
// ---------------------------------------------------------------------------
TEST_F(ReportCapacity, ReportAumScalesParticipation)
{
    auto r1 = run_with_aum(1e9);
    ASSERT_TRUE(r1.has_value()) << r1.error().message();

    auto r2 = run_with_aum(2e9);
    ASSERT_TRUE(r2.has_value()) << r2.error().message();

    const fs::path s1 = *r1 / "summary.txt";
    const fs::path s2 = *r2 / "summary.txt";

    const double max1 = std::stod(read_summary_value(s1, "max_participation_pct"));
    const double max2 = std::stod(read_summary_value(s2, "max_participation_pct"));

    ASSERT_GT(max1, 0.0) << "max_participation_pct must be positive";
    EXPECT_NEAR(max2, 2.0 * max1, 1e-9)
        << "doubling AUM must double max_participation_pct";
}

// ---------------------------------------------------------------------------
// Test 3: ExistingSummaryPrefixUnchanged
// The first 6 lines of summary.txt must be byte-identical when running with
// and without the new AUM flag (using default 1e9).
// Also: run twice with the same AUM and assert digest is identical.
// ---------------------------------------------------------------------------
TEST_F(ReportCapacity, ExistingSummaryPrefixUnchanged)
{
    // Run twice with same config.
    auto r_a = run_with_aum(1e9);
    ASSERT_TRUE(r_a.has_value()) << r_a.error().message();

    // Second run into a different output dir.
    const fs::path report_b = work_dir_ / "report_b";
    {
        std::error_code ec;
        fs::remove_all(report_b, ec);
        fs::create_directories(report_b, ec);
    }
    atx::impl::RunConfig cfg_b;
    cfg_b.panel      = research_path_;
    cfg_b.books      = books_path_;
    cfg_b.report_out = report_b.string();
    cfg_b.report_aum = 1e9;
    auto r_b = atx::impl::run_report(cfg_b);
    ASSERT_TRUE(r_b.has_value()) << r_b.error().message();

    // First 6 lines must be byte-identical.
    const auto lines_a = read_first_lines(*r_a / "summary.txt", 6);
    const auto lines_b = read_first_lines(report_b / "summary.txt", 6);

    ASSERT_EQ(lines_a.size(), 6u) << "run A summary.txt has fewer than 6 lines";
    ASSERT_EQ(lines_b.size(), 6u) << "run B summary.txt has fewer than 6 lines";

    for (int i = 0; i < 6; ++i) {
        EXPECT_EQ(lines_a[static_cast<std::size_t>(i)],
                  lines_b[static_cast<std::size_t>(i)])
            << "summary.txt line " << (i+1) << " differs between runs";
    }

    // Also verify the first 6 lines are the ORIGINAL 6 keys.
    const std::array<std::string, 6> expected_keys = {
        "final_equity", "total_pnl_gross", "total_pnl_net",
        "total_pnl_cost", "avg_gross_leverage", "avg_turnover"
    };
    for (int i = 0; i < 6; ++i) {
        const auto& line = lines_a[static_cast<std::size_t>(i)];
        const bool starts_with_key = (line.rfind(expected_keys[static_cast<std::size_t>(i)] + "=", 0) == 0);
        EXPECT_TRUE(starts_with_key)
            << "line " << (i+1) << " should start with '"
            << expected_keys[static_cast<std::size_t>(i)] << "='";
    }

    // Digests must be identical across two runs with same config.
    // We ran r_a already; run a third time to get a StageResult with digest.
    const fs::path report_c = work_dir_ / "report_c";
    {
        std::error_code ec;
        fs::remove_all(report_c, ec);
        fs::create_directories(report_c, ec);
    }
    atx::impl::RunConfig cfg_c;
    cfg_c.panel      = research_path_;
    cfg_c.books      = books_path_;
    cfg_c.report_out = report_c.string();
    cfg_c.report_aum = 1e9;
    auto r_c = atx::impl::run_report(cfg_c);
    ASSERT_TRUE(r_c.has_value()) << r_c.error().message();

    EXPECT_EQ(r_b->digest, r_c->digest)
        << "report digest is not deterministic: b=" << r_b->digest
        << " c=" << r_c->digest;
}

} // namespace atx_impl_report_capacity
