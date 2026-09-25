// W0-I0b: publication order (I-17), the same-close delay guard in the equity stages
// and at the identified replay seam (B-02 impl), and mandatory report costs (I-11).
// Synthetic two-name fixture (the stage_equity_baseline_test shape); no market data.
#include <atomic>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "atx/core/error.hpp"
#include "atx/engine/data/orats_history.hpp"
#include "equity_baseline_views.hpp"
#include "panel_artifact.hpp"
#include "stage_equity_baseline.hpp"
#include "stage_equity_book.hpp"
#include "stages.hpp"

namespace atx_test_w0_i0b_pending {
namespace fs = std::filesystem;
namespace impl = atx::impl;
using Json = nlohmann::json;
using atx::core::Err;
using atx::core::ErrorCode;
using atx::core::Ok;
using atx::engine::alpha::Panel;
constexpr atx::i64 kDay = 86'400'000'000'000LL;

std::string contents(const fs::path &path) {
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

class Fixture : public ::testing::Test {
protected:
    fs::path root;

    void SetUp() override {
        static std::atomic<unsigned> sequence{};
        for (unsigned attempt = 0; attempt < 1000; ++attempt) {
            root = fs::temp_directory_path() /
                   ("atx_w0i0b_pending_" + std::to_string(sequence.fetch_add(1)));
            if (fs::create_directory(root)) return;
        }
        FAIL() << "Cannot reserve fixture root";
    }
    void TearDown() override {
        std::error_code ec;
        fs::remove_all(root, ec);
    }

    // The checkpoint-14 baseline shape: 264 sessions, two names, 8 evaluated.
    impl::RunConfig baseline_input() {
        constexpr atx::usize dates = 264;
        constexpr atx::usize names = 2;
        const auto evaluation = *atx::engine::data::detail::date_to_nanos("2013-04-04");
        const auto first = evaluation - 256 * kDay;
        std::vector<double> close(dates * names);
        std::vector<atx::u8> mask(dates * names, 0);
        for (atx::usize d = 0; d < dates; ++d) {
            close[d * names] = 100 + static_cast<double>(d);
            close[d * names + 1] = 500 - 0.25 * static_cast<double>(d);
            if (d >= 256) mask[d * names] = mask[d * names + 1] = 1;
        }
        std::vector<double> raw = close;
        for (auto &value : raw) value *= 2;
        auto panel = Panel::create(dates, names, {"close", "raw_close", "volume"},
            {close, raw, std::vector<double>(dates * names, 1e6)}, std::move(mask));
        EXPECT_TRUE(panel.has_value());
        impl::PanelIdentity identity;
        identity.instrument_namespace = impl::kSpiderRockSecurityIdNamespace;
        identity.instrument_ids = {"1", "2"};
        identity.original_instrument_indices = {0, 1};
        for (atx::usize d = 0; d < dates; ++d) {
            identity.session_keys.push_back(first + static_cast<atx::i64>(d) * kDay);
        }
        identity.parents = {{"synthetic-fixture", std::string(64, 'a')}};
        identity.recipe = Json{{"version", "tickerhistory-panel-v2-identified"},
            {"start_inclusive_nanos", std::to_string(first)},
            {"end_exclusive_nanos", std::to_string(first + static_cast<atx::i64>(dates) * kDay)},
            {"research_ohlc", "raw-OHLC*cumulReturnFactor-pointwise"},
            {"raw_close", "unadjusted-as-traded"},
            {"volume", "raw-reported-volume-no-total-return-factor-rescaling"},
            {"universe", {{"adv_basis", "raw_close*raw_volume"},
                {"top_n_tie_break", "original-instrument-index"}, {"top_n_by_adv", 1000},
                {"min_adv_usd", 20e6}, {"min_raw_price_exclusive", 5.0}, {"adv_window_bars", 21},
                {"min_mktcap_usd", 0.0}, {"require_sector", false},
                {"current_session_data_included", true},
                {"adv_missing", "full-trailing-window-any-NaN-invalid"}}},
            {"compact_to_universe", true},
            {"compaction", "keep-original-order-if-ever-in-universe-in-selected-window"},
            {"augmentation", {{"enabled", false}, {"adv_windows", Json::array()}}},
            {"historical_availability", "unknown-archive-snapshot"},
            {"fixture", "synthetic-only-no-market-or-performance-evidence"}}.dump();
        EXPECT_TRUE(impl::write_panel_artifact(*panel, (root / "context.bin").string(), identity)
                        .has_value());
        impl::RunConfig cfg;
        cfg.panel = (root / "context.bin").string();
        cfg.out = (root / "baseline").string();
        cfg.equity_evaluation_start = "2013-04-04";
        cfg.equity_evaluation_end = "2013-04-12";
        cfg.report_aum = 1000;
        return cfg;
    }

    impl::RunConfig book_input(const std::string &out) const {
        impl::RunConfig cfg;
        cfg.panel = (root / "context.bin").string();
        cfg.equity_baseline_dir = (root / "baseline").string();
        cfg.out = (root / out).string();
        return cfg;
    }

    // A report over the published baseline's own identified research/combo/books.
    impl::RunConfig report_input(const std::string &out) const {
        impl::RunConfig cfg;
        cfg.panel = (root / "baseline/evaluation.bin").string();
        cfg.combo = (root / "baseline/combo.bin").string();
        cfg.books = (root / "baseline/books.bin").string();
        cfg.report_out = (root / out).string();
        cfg.report_aum = 1000;
        return cfg;
    }
};

class ImplPendingOrder_Stages : public Fixture {};
class ImplDelayGuard_Stages : public Fixture {};

// --- I-17: manifest first, `.pending` released last -------------------------------

TEST_F(ImplPendingOrder_Stages, AFailedManifestWriteLeavesThePendingMarker) {
    const auto dir = root / "pub";
    fs::create_directories(dir / ".pending");
    bool called = false;
    const auto failed = impl::publish_manifest_then_release_pending(dir, [&]() -> atx::core::Status {
        called = true;
        return Err(ErrorCode::IoError, "injected manifest failure");
    });
    ASSERT_FALSE(failed);
    EXPECT_TRUE(called);
    EXPECT_NE(failed.error().message().find("injected"), std::string::npos);
    EXPECT_TRUE(fs::exists(dir / ".pending")) << "I-17: a failed manifest must stay pending";

    const auto ok = impl::publish_manifest_then_release_pending(dir, [&]() -> atx::core::Status {
        EXPECT_TRUE(fs::exists(dir / ".pending")) << "manifest must be written BEFORE release";
        std::ofstream(dir / "manifest.json") << "{}\n";
        return Ok();
    });
    ASSERT_TRUE(ok) << ok.error().message();
    EXPECT_FALSE(fs::exists(dir / ".pending"));
    EXPECT_TRUE(fs::exists(dir / "manifest.json"));

    bool second = false;
    const auto no_marker = impl::publish_manifest_then_release_pending(
        dir, [&]() -> atx::core::Status { second = true; return Ok(); });
    EXPECT_FALSE(no_marker);
    EXPECT_FALSE(second) << "nothing is published into a directory that was never reserved";
}

TEST_F(ImplPendingOrder_Stages, BaselineAndBookPublishCompleteManifestsAndReleaseLast) {
    const auto baseline = impl::run_equity_baseline(baseline_input());
    ASSERT_TRUE(baseline.has_value()) << baseline.error().message();
    EXPECT_TRUE(fs::exists(root / "baseline/manifest.json"));
    EXPECT_FALSE(fs::exists(root / "baseline/.pending"));
    EXPECT_EQ(Json::parse(contents(root / "baseline/manifest.json")).at("status"), "complete");
    const auto book = impl::run_equity_book(book_input("book"));
    ASSERT_TRUE(book.has_value()) << book.error().message();
    EXPECT_TRUE(fs::exists(root / "book/manifest.json"));
    EXPECT_FALSE(fs::exists(root / "book/.pending"));
}

// --- B-02 (impl): same-close fills need the explicit opt-in ------------------------

TEST_F(ImplDelayGuard_Stages, DelayGuardInBaselineAndInheritedByTheBook) {
    auto cfg = baseline_input();
    cfg.replay_execution_delay = 0;
    cfg.set_flags = {"replay-execution-delay"};
    const auto refused = impl::run_equity_baseline(cfg);
    ASSERT_FALSE(refused.has_value());
    EXPECT_NE(refused.error().message().find("--allow-same-close"), std::string::npos);
    EXPECT_FALSE(fs::exists(root / "baseline")) << "refused before reserving the output";

    cfg.allow_same_close = true;
    cfg.set_flags.insert("allow-same-close");
    const auto allowed = impl::run_equity_baseline(cfg);
    ASSERT_TRUE(allowed.has_value()) << allowed.error().message();
    EXPECT_EQ(Json::parse(contents(root / "baseline/manifest.json"))
                  .at("recipe").at("execution_delay_observations"), "0");

    // The book inherits delay 0 from that baseline: refused unless it opts in too.
    const auto inherited = impl::run_equity_book(book_input("book_refused"));
    ASSERT_FALSE(inherited.has_value());
    EXPECT_NE(inherited.error().message().find("--allow-same-close"), std::string::npos);
    auto opted = book_input("book_allowed");
    opted.allow_same_close = true;
    opted.set_flags = {"allow-same-close"};
    const auto book = impl::run_equity_book(opted);
    ASSERT_TRUE(book.has_value()) << book.error().message();
}

TEST_F(ImplDelayGuard_Stages, ReplaySeamRefusesSameCloseAndSilentCosts) {
    ASSERT_TRUE(impl::run_equity_baseline(baseline_input()).has_value());

    // I-11: neither rate chosen -> refused; one chosen -> still refused.
    auto silent = report_input("r_silent");
    const auto no_costs = impl::run_report(silent);
    ASSERT_FALSE(no_costs.has_value());
    EXPECT_NE(no_costs.error().message().find("report costs are mandatory"), std::string::npos)
        << no_costs.error().message();
    EXPECT_FALSE(fs::exists(root / "r_silent"));
    auto half = report_input("r_half");
    half.set_flags = {"replay-trade-bps"};
    EXPECT_FALSE(impl::run_report(half).has_value());

    // An explicit zero is a deliberate choice and runs; nonzero values count as chosen.
    auto zero = report_input("r_zero");
    zero.set_flags = {"replay-trade-bps", "replay-annual-borrow-bps"};
    const auto frictionless = impl::run_report(zero);
    ASSERT_TRUE(frictionless.has_value()) << frictionless.error().message();
    auto costed = report_input("r_costed");
    costed.replay_trade_bps = 5;
    costed.replay_annual_borrow_bps = 365;
    const auto with_costs = impl::run_report(costed);
    ASSERT_TRUE(with_costs.has_value()) << with_costs.error().message();
    const auto z = Json::parse(contents(root / "r_zero/summary.json"));
    const auto c = Json::parse(contents(root / "r_costed/summary.json"));
    EXPECT_EQ(z.at("full").at("trade_cost_dollars"), 0.0);
    EXPECT_GT(c.at("full").at("trade_cost_dollars").get<double>(), 0.0);
    std::printf("[ImplDelayGuard] replay seam: explicit-zero trade cost %.6f, 5bps/365bps trade "
                "cost %.6f borrow %.6f\n", z.at("full").at("trade_cost_dollars").get<double>(),
                c.at("full").at("trade_cost_dollars").get<double>(),
                c.at("full").at("borrow_cost_dollars").get<double>());

    // B-02: delay 0 at the replay seam needs the opt-in, even programmatically.
    auto same_close = report_input("r_delay0");
    same_close.set_flags = {"replay-trade-bps", "replay-annual-borrow-bps"};
    same_close.replay_execution_delay = 0;
    const auto refused = impl::run_report(same_close);
    ASSERT_FALSE(refused.has_value());
    EXPECT_NE(refused.error().message().find("--allow-same-close"), std::string::npos);
    same_close.allow_same_close = true;
    EXPECT_TRUE(impl::run_report(same_close).has_value());
}

} // namespace atx_test_w0_i0b_pending
