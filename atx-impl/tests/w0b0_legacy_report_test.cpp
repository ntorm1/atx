// W0-B0 (B-03), stage level: the unidentified (legacy) `report` stage now
// charges each rebalance's book the TRI return compounded over its whole
// holding window, prices a held name that stops printing at its last print
// times (1 + Shumway fallback, flagged) instead of 0, accrues --borrow-bps as an
// annual rate over the sessions held, and annualizes the Sharpe by the mean
// holding length. Synthetic fixture only.

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "config.hpp"
#include "serialize_panel.hpp"
#include "stages.hpp"

#include "atx/engine/alpha/panel.hpp"

namespace atx_test_w0_b0_legacy_report {

namespace fs = std::filesystem;
namespace alpha = atx::engine::alpha;
using atx::f64;
using atx::usize;
constexpr f64 kNaN = std::numeric_limits<f64>::quiet_NaN();
constexpr usize kDates = 11;
constexpr usize kNames = 3;

struct Dir {
    fs::path root;
    explicit Dir(const std::string &tag)
        : root(fs::temp_directory_path() / ("atx_w0b0_legacy_report_" + tag)) {
        std::error_code ec;
        fs::remove_all(root, ec);
        fs::create_directories(root, ec);
    }
    ~Dir() {
        std::error_code ec;
        fs::remove_all(root, ec);
    }
};

// name 0 compounds +1 %/session; name 1 prints 40..46 through session 6 and
// never again (a mid-run delisting with no evidence); name 2 is flat at 20.
std::vector<f64> close_path() {
    std::vector<f64> close;
    f64 p0 = 100.0;
    for (usize t = 0; t < kDates; ++t) {
        close.push_back(p0);
        close.push_back(t <= 6 ? 40.0 + static_cast<f64>(t) : kNaN);
        close.push_back(20.0);
        p0 *= 1.01;
    }
    return close;
}

const std::vector<f64> kBook{0.5, 0.3, -0.2};

void write_fixture(const Dir &dir) {
    const auto close = close_path();
    std::vector<std::uint8_t> uni(kDates * kNames, 1U);
    auto research = alpha::Panel::create(kDates, kNames, {"raw_close", "close"}, {close, close},
                                         uni);
    ASSERT_TRUE(research.has_value()) << research.error().message();
    ASSERT_TRUE(atx::impl::write_panel(*research, (dir.root / "research.bin").string()));
    std::vector<f64> weights;
    for (usize s = 0; s < 2; ++s) weights.insert(weights.end(), kBook.begin(), kBook.end());
    auto books = alpha::Panel::create(2, kNames, {"weight"}, {weights},
                                      std::vector<std::uint8_t>(2 * kNames, 1U));
    ASSERT_TRUE(books.has_value()) << books.error().message();
    ASSERT_TRUE(atx::impl::write_panel(*books, (dir.root / "books.bin").string()));
    std::ofstream meta((dir.root / "books.bin").string() + ".meta.txt");
    meta << "periods=2\ninstruments=3\n";
    meta << "s=0 period=0 turnover=0.0 cost_bps=0.0\n";   // Weekly: sessions 0 and 5.
    meta << "s=1 period=5 turnover=0.0 cost_bps=0.0\n";
}

std::string kv(const atx::impl::StageResult &result, const std::string &key) {
    for (const auto &entry : result.kvs) {
        if (entry.first == key) return entry.second;
    }
    return {};
}

std::string summary_value(const fs::path &path, const std::string &key) {
    std::ifstream in(path);
    std::string line;
    while (std::getline(in, line)) {
        const auto eq = line.find('=');
        if (eq != std::string::npos && line.substr(0, eq) == key) return line.substr(eq + 1);
    }
    return {};
}

// pnl.tsv column 0 (gross) per period, written with shortest round-trip digits.
std::vector<f64> gross_column(const fs::path &path) {
    std::ifstream in(path);
    std::string line;
    std::getline(in, line); // header
    std::vector<f64> gross;
    while (std::getline(in, line)) gross.push_back(std::stod(line.substr(0, line.find('\t'))));
    return gross;
}

TEST(BookLegacyReportStage, WeeklyBookWithADelistingIsChargedTheTruth) {
    const Dir dir{"weekly"};
    write_fixture(dir);
    if (HasFatalFailure()) return;
    atx::impl::RunConfig cfg;
    cfg.allow_unidentified_panels = true; // The legacy numeric report path.
    cfg.panel = (dir.root / "research.bin").string();
    cfg.books = (dir.root / "books.bin").string();
    cfg.report_out = (dir.root / "report").string();
    cfg.borrow_bps = 2520.0; // 25.2 % a year on the 0.2 short.
    const auto result = atx::impl::run_report(cfg);
    ASSERT_TRUE(result.has_value()) << result.error().message();

    const auto close = close_path();
    const auto at = [&](usize t, usize i) { return close[t * kNames + i]; };
    const f64 week0 = 0.5 * (at(5, 0) / at(0, 0) - 1.0) + 0.3 * (at(5, 1) / at(0, 1) - 1.0);
    const f64 delisted = (at(6, 1) / at(5, 1)) * (1.0 - 0.55) - 1.0; // Long, venue unknown.
    const f64 week1 = 0.5 * (at(10, 0) / at(5, 0) - 1.0) + 0.3 * delisted;
    const auto gross = gross_column(fs::path{cfg.report_out} / "pnl.tsv");
    ASSERT_EQ(gross.size(), 2U);
    EXPECT_NEAR(gross[0], week0, 1.0e-15);
    EXPECT_NEAR(gross[1], week1, 1.0e-15);
    // Pre-W0 charged one session per rebalance and read the missing close as 0.
    const f64 v1_week1 = 0.5 * (at(6, 0) / at(5, 0) - 1.0) + 0.3 * (at(6, 1) / at(5, 1) - 1.0);
    std::printf("[measured] pnl_gross week 1: V2 %.6f (delisting priced) vs V1 %.6f\n", gross[1],
                v1_week1);

    EXPECT_EQ(kv(*result, "legacy_report_rule"), "3"); // LegacyReportRule::HoldingIntervalV3
    EXPECT_EQ(kv(*result, "terminal_returns_flagged"), "1");
    EXPECT_EQ(kv(*result, "interior_gap_marks"), "0");
    EXPECT_NEAR(std::stod(kv(*result, "assumed_missing_price_pnl")),
                0.3 * (at(6, 1) / at(5, 1)) * -0.55, 1e-6);
    EXPECT_EQ(kv(*result, "usable_for_alpha_evidence"), "0");
    // 0.2 short * 25.2 %/yr * (5 + 5) sessions / 252 = 0.002.
    EXPECT_NEAR(std::stod(kv(*result, "total_pnl_borrow")), 0.002, 1.0e-12);
    const fs::path summary = fs::path{cfg.report_out} / "summary.txt";
    EXPECT_EQ(summary_value(summary, "legacy_report_rule"), "holding_interval_v3_first_missing");
    EXPECT_EQ(summary_value(summary, "terminal_returns_flagged"), "1");
    // Two weekly books held 5 sessions each: sqrt(252 / 5).
    EXPECT_NEAR(std::stod(summary_value(summary, "annualization_factor")),
                std::sqrt(252.0 / 5.0), 1.0e-6);
    // The pre-existing summary prefix is still first and intact.
    std::ifstream in(summary);
    std::string first;
    std::getline(in, first);
    EXPECT_EQ(first.rfind("final_equity=", 0), 0U);
}

TEST(BookLegacyReportStage, DailyScheduleIsUnchangedByTheHoldingIntervalRule) {
    // A daily schedule holds each book one session, so V2 equals the pre-W0
    // one-session return wherever the next close exists.
    const Dir dir{"daily"};
    const auto close = close_path();
    std::vector<std::uint8_t> uni(kDates * kNames, 1U);
    auto research = alpha::Panel::create(kDates, kNames, {"raw_close", "close"}, {close, close},
                                         uni);
    ASSERT_TRUE(research.has_value());
    ASSERT_TRUE(atx::impl::write_panel(*research, (dir.root / "research.bin").string()));
    std::vector<f64> weights;
    for (usize s = 0; s < 3; ++s) weights.insert(weights.end(), kBook.begin(), kBook.end());
    auto books = alpha::Panel::create(3, kNames, {"weight"}, {weights},
                                      std::vector<std::uint8_t>(3 * kNames, 1U));
    ASSERT_TRUE(books.has_value());
    ASSERT_TRUE(atx::impl::write_panel(*books, (dir.root / "books.bin").string()));
    {
        std::ofstream meta((dir.root / "books.bin").string() + ".meta.txt");
        meta << "periods=3\ninstruments=3\n";
        meta << "s=0 period=1 turnover=0.0 cost_bps=0.0\n";
        meta << "s=1 period=2 turnover=0.0 cost_bps=0.0\n";
        meta << "s=2 period=3 turnover=0.0 cost_bps=0.0\n";
    }
    atx::impl::RunConfig cfg;
    cfg.allow_unidentified_panels = true;
    cfg.panel = (dir.root / "research.bin").string();
    cfg.books = (dir.root / "books.bin").string();
    cfg.report_out = (dir.root / "report").string();
    const auto result = atx::impl::run_report(cfg);
    ASSERT_TRUE(result.has_value()) << result.error().message();
    const auto gross = gross_column(fs::path{cfg.report_out} / "pnl.tsv");
    ASSERT_EQ(gross.size(), 3U);
    for (usize s = 0; s < 3; ++s) {
        const usize d = s + 1;
        f64 old = 0.0; // The pre-W0 one-session arithmetic, name by name.
        for (usize i = 0; i < kNames; ++i) {
            old += kBook[i] * (close[(d + 1) * kNames + i] / close[d * kNames + i] - 1.0);
        }
        EXPECT_EQ(gross[s], old) << "period " << s;
    }
    EXPECT_EQ(kv(*result, "terminal_returns_flagged"), "0");
}

} // namespace atx_test_w0_b0_legacy_report
