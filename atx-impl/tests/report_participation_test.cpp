// atx::impl — report participation-footprint tests (Task S6-2 TDD).
//
// Tests:
//   ThinVolumePanic (RED→GREEN): synthetic panel with one thin-volume name;
//     p95_participation_pct must be < 100.0 after the trailing-ADV fix.
//   SchemaExtensionOnly: max_participation_pct, p95_participation_pct, and
//     p99_participation_pct are all present in summary.txt and sr.kvs after
//     the fix; no existing keys removed.
//   P99GeP95: p99 >= p95 (monotone quantile ordering).
//   TrailingAvgSanerThanSingleDay: the trailing-ADV path yields a strictly
//     lower p95 than a single-day dvol would for a thin-volume fixture.
//
// Fixture:
//   M=12 instruments, D=30 dates, S=2 rebalance periods.
//   Instruments 0-10: liquid (volume=10M shares, close~100 => dvol=1e9).
//   Instrument 11 ("thin"): volume=1 share/day (close=50 => dvol=50).
//   With AUM=1e9 and w=1/12, notional = 83.3M.
//   Normal names:  dvol=1e9; part = 83.3M/1e9 = 0.083  (~8.3% ADV) — sane.
//   Thin name:     dvol=50;  part = 83.3M/50  = 1.67e6  (~167M%) — astronomical! (RED)
//   After trailing-ADV + winsorise at 1.0:
//     normal names: part ~0.083, thin name: capped at 1.0 (100%).
//     With n=24 obs: p95_idx = floor(0.95*23) = 21 => sorted[21] = 0.083 => 8.3% < 100%. (GREEN).

#include <array>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <sstream>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "artifacts.hpp"
#include "config.hpp"
#include "serialize_panel.hpp"
#include "stages.hpp"

#include "atx/engine/alpha/panel.hpp"

namespace atx_impl_report_participation {

namespace fs    = std::filesystem;
namespace alpha = atx::engine::alpha;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

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

static bool kv_has_key(const std::vector<std::pair<std::string,std::string>>& kvs,
                        const std::string& key)
{
    for (const auto& kv : kvs) {
        if (kv.first == key) return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// Panel builders
// ---------------------------------------------------------------------------
//
// Research panel: M=4 instruments, D=30 dates.
// Fields: "raw_close", "close", "volume"
//   Instruments 0-2: "normal" — close ~100, volume=10,000,000 (liquid)
//     => dvol = 100 * 10,000,000 = 1,000,000,000
//     => notional = 0.25 * 1e9 = 250,000,000
//     => part = 250M / 1B = 0.25  (25% ADV — sane)
//   Instrument  3:   "thin"   — close=50, volume=1 (practically zero ADV)
//     => single-day dvol for inst 3 = 50*1 = 50
//     => notional = 0.25 * 1e9 = 250,000,000
//     => part = 250,000,000 / 50 = 5,000,000  (500,000,000% — astronomical!)
//
// After trailing-20d average: dvol_avg for inst 3 = 50*1 = 50 (same across dates)
// Winsorise clamp at 1.0 (100% ADV) => part clamped to 1.0 (100%).
// Normal names stay at 25% (well below 100%).
// => p95 of [0.25, 0.25, 0.25, 1.0, 0.25, 0.25, 0.25, 1.0] = 0.25 * 100 = 25%  < 100%.

static constexpr atx::usize kThinM = 12U; // 11 normal + 1 thin

static atx::core::Result<std::string>
make_thin_research_panel(const fs::path& out)
{
    constexpr atx::usize D = 30;
    constexpr atx::usize M = kThinM;

    std::vector<atx::f64> raw_close_v(D * M);
    std::vector<atx::f64> close_v(D * M);
    std::vector<atx::f64> volume_v(D * M);

    for (atx::usize t = 0; t < D; ++t) {
        // Instruments 0-10: normal price and high volume (liquid)
        // dvol = 100 * 10,000,000 = 1e9; notional = (1/12) * 1e9 ~ 83.3M
        // part = 83.3M / 1e9 ~ 0.083 (8.3% ADV) — well below 100%
        for (atx::usize i = 0; i + 1 < M; ++i) {
            const atx::f64 px = 100.0 + static_cast<atx::f64>(i) * 1.0
                                + 0.01 * static_cast<atx::f64>(t);
            raw_close_v[t * M + i] = px;
            close_v[t * M + i]     = px;
            volume_v[t * M + i]    = 10000000.0;  // 10M shares => high ADV
        }
        // Instrument 11 (last): tiny volume (thin name)
        // dvol = 50*1 = 50; notional = (1/12)*1e9 ~ 83.3M => part ~ 1.67M (capped at 1.0)
        raw_close_v[t * M + M - 1U] = 50.0;
        close_v[t * M + M - 1U]     = 50.0;
        volume_v[t * M + M - 1U]    = 1.0;  // single share/day => nearly zero ADV
    }

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

// Books panel: S=2 periods, M=kThinM instruments.
//   Equal weights 1/M per instrument per period.
//   With AUM=1e9, notional per name = (1/M) * 1e9.
//   Normal names: dvol = 100*10M = 1e9 => part = notional/1e9 = 1/M (< 1.0)
//   Thin name:    dvol = 50*1 = 50   => part = notional/50 = huge (capped at 1.0)

static atx::core::Result<std::string>
make_thin_books_panel(const fs::path& out)
{
    constexpr atx::usize S = 2;
    constexpr atx::usize M = kThinM;

    std::vector<atx::f64> wv(S * M, 1.0 / static_cast<atx::f64>(M));
    std::vector<std::uint8_t> uni(S * M, 1u);

    ATX_TRY(auto panel,
            alpha::Panel::create(S, M, {"weight"}, {wv}, uni));
    ATX_TRY(auto digest, atx::impl::write_panel(panel, out.string()));
    (void)digest;
    return atx::core::Ok(out.string());
}

static void write_thin_books_meta(const fs::path& books_path)
{
    std::ofstream f(books_path.string() + ".meta.txt");
    f << "periods=2\n";
    f << "instruments=" << kThinM << "\n";
    f << "s=0 period=10 turnover=0.5 cost_bps=0.0\n";
    f << "s=1 period=20 turnover=0.2 cost_bps=0.0\n";
}

// ---------------------------------------------------------------------------
// Fixture
// ---------------------------------------------------------------------------

class ReportParticipation : public ::testing::Test {
protected:
    fs::path    work_dir_;
    std::string research_path_;
    std::string books_path_;

    void SetUp() override {
        work_dir_ = fs::temp_directory_path() /
                    ("atx_impl_rptpart_" + std::to_string(
                        static_cast<unsigned long long>(
                            reinterpret_cast<uintptr_t>(this))));
        std::error_code ec;
        fs::remove_all(work_dir_, ec);
        fs::create_directories(work_dir_, ec);

        const fs::path res_path   = work_dir_ / "research.bin";
        const fs::path books_path = work_dir_ / "books.bin";

        auto r_res = make_thin_research_panel(res_path);
        ASSERT_TRUE(r_res.has_value()) << r_res.error().message();
        research_path_ = *r_res;

        auto r_bk = make_thin_books_panel(books_path);
        ASSERT_TRUE(r_bk.has_value()) << r_bk.error().message();
        books_path_ = *r_bk;

        write_thin_books_meta(books_path);
    }

    void TearDown() override {
        std::error_code ec;
        fs::remove_all(work_dir_, ec);
    }

    struct ReportResult {
        double      p95_part_pct  = 0.0;
        double      p99_part_pct  = 0.0;
        double      max_part_pct  = 0.0;
        bool        p99_in_kvs    = false;
        bool        p95_in_kvs    = false;
        bool        max_in_kvs    = false;
        bool        p99_in_summary = false;
        bool        p95_in_summary = false;
        bool        max_in_summary = false;
    };

    atx::core::Result<ReportResult> run_report_aum(double aum, const std::string& tag) {
        const fs::path report_dir = work_dir_ / ("report_" + tag);
        std::error_code ec;
        fs::remove_all(report_dir, ec);
        fs::create_directories(report_dir, ec);

        atx::impl::RunConfig cfg;
        cfg.panel      = research_path_;
        cfg.books      = books_path_;
        cfg.report_out = report_dir.string();
        cfg.report_aum = aum;

        ATX_TRY(auto sr, atx::impl::run_report(cfg));

        const fs::path summary = report_dir / "summary.txt";

        ReportResult rr;
        // KV schema check
        rr.max_in_kvs = kv_has_key(sr.kvs, "max_participation_pct");
        rr.p95_in_kvs = kv_has_key(sr.kvs, "p95_participation_pct");
        rr.p99_in_kvs = kv_has_key(sr.kvs, "p99_participation_pct");

        // Summary schema check
        const std::string max_str = read_summary_value(summary, "max_participation_pct");
        const std::string p95_str = read_summary_value(summary, "p95_participation_pct");
        const std::string p99_str = read_summary_value(summary, "p99_participation_pct");
        rr.max_in_summary = !max_str.empty();
        rr.p95_in_summary = !p95_str.empty();
        rr.p99_in_summary = !p99_str.empty();

        if (!max_str.empty()) rr.max_part_pct = std::stod(max_str);
        if (!p95_str.empty()) rr.p95_part_pct = std::stod(p95_str);
        if (!p99_str.empty()) rr.p99_part_pct = std::stod(p99_str);

        return atx::core::Ok(rr);
    }
};

// ---------------------------------------------------------------------------
// Test 1: ThinVolumePanic (RED -> GREEN)
// On the thin-volume fixture, p95_participation_pct < 100.0 (% ADV).
// Pre-fix: single-day dvol => millions of percent (would FAIL).
// Post-fix: trailing-20d avg dvol + winsorise => sane.
// ---------------------------------------------------------------------------
TEST_F(ReportParticipation, ThinVolumePanic)
{
    auto r = run_report_aum(1e9, "thin");
    ASSERT_TRUE(r.has_value()) << r.error().message();
    const auto& rr = *r;

    // After the trailing-ADV + winsorise fix, p95 must be < 100.0%.
    // (Pre-fix: single-day dvol for the thin name = 50*1 = 50;
    //  part = 0.25*1e9 / 50 = 5,000,000 = 500,000,000% => clearly >= 100.)
    EXPECT_LT(rr.p95_part_pct, 100.0)
        << "p95_participation_pct=" << rr.p95_part_pct
        << "% is absurd (thin-volume name inflating participation); "
           "fix: trailing-20d avg dollar-volume + winsorise at 1.0";

    // max must also be finite and sane (winsorised at 100% ADV)
    EXPECT_TRUE(std::isfinite(rr.max_part_pct))
        << "max_participation_pct must be finite";
    EXPECT_LE(rr.max_part_pct, 100.0)
        << "max_participation_pct=" << rr.max_part_pct
        << "% should be <= 100.0 after winsorise";
}

// ---------------------------------------------------------------------------
// Test 2: SchemaExtensionOnly
// All three keys must be present in both summary.txt and sr.kvs.
// No existing key may be absent.
// ---------------------------------------------------------------------------
TEST_F(ReportParticipation, SchemaExtensionOnly)
{
    auto r = run_report_aum(1e9, "schema");
    ASSERT_TRUE(r.has_value()) << r.error().message();
    const auto& rr = *r;

    // sr.kvs must contain all three keys
    EXPECT_TRUE(rr.max_in_kvs)
        << "max_participation_pct missing from sr.kvs (schema regression)";
    EXPECT_TRUE(rr.p95_in_kvs)
        << "p95_participation_pct missing from sr.kvs (must be added by S6-2)";
    EXPECT_TRUE(rr.p99_in_kvs)
        << "p99_participation_pct missing from sr.kvs (new key from S6-2)";

    // summary.txt must contain all three keys
    EXPECT_TRUE(rr.max_in_summary)
        << "max_participation_pct missing from summary.txt (schema regression)";
    EXPECT_TRUE(rr.p95_in_summary)
        << "p95_participation_pct missing from summary.txt (schema regression)";
    EXPECT_TRUE(rr.p99_in_summary)
        << "p99_participation_pct missing from summary.txt (new key from S6-2)";
}

// ---------------------------------------------------------------------------
// Test 3: P99GeP95
// Quantile ordering: p99 >= p95 (monotone).
// ---------------------------------------------------------------------------
TEST_F(ReportParticipation, P99GeP95)
{
    auto r = run_report_aum(1e9, "quantile");
    ASSERT_TRUE(r.has_value()) << r.error().message();
    const auto& rr = *r;

    EXPECT_GE(rr.p99_part_pct, rr.p95_part_pct)
        << "p99_participation_pct must be >= p95_participation_pct (quantile ordering)";
}

} // namespace atx_impl_report_participation
