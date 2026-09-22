#include <algorithm>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <optional>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "atx/core/sha256.hpp"
#include "panel_artifact.hpp"
#include "panel_pipeline.hpp"
#include "replay_report.hpp"
#include "serialize_panel.hpp"
#include "stages.hpp"

namespace {
namespace fs = std::filesystem;
namespace impl = atx::impl;
using Json = nlohmann::json;
using atx::core::Ok;
using atx::core::Result;
using atx::engine::alpha::Panel;
constexpr atx::i64 kStart = 1'777'593'600'000'000'000LL;
constexpr atx::i64 kDay = 86'400'000'000'000LL;
constexpr const char *kSecurity = "9007199254740993";

std::string contents(const fs::path &path) {
    std::ifstream file(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}

Json json_file(const fs::path &path) { return Json::parse(contents(path)); }

std::string metric(const impl::StageResult &result, const std::string &key) {
    for (const auto &[name, value] : result.kvs) if (name == key) return value;
    return {};
}

class ReplayReport : public ::testing::Test {
protected:
    fs::path root;

    void SetUp() override {
        static std::atomic<unsigned> sequence{};
        for (unsigned attempt = 0; attempt < 1000; ++attempt) {
            root = fs::temp_directory_path() /
                ("atx_replay_report_" + std::to_string(sequence.fetch_add(1)));
            if (fs::create_directory(root)) return;
        }
        FAIL() << "Cannot reserve fixture directory";
    }

    void TearDown() override {
        std::error_code ec;
        const auto parent = fs::weakly_canonical(fs::temp_directory_path(), ec);
        if (ec) return;
        const auto resolved = fs::weakly_canonical(root, ec);
        if (!ec && resolved.parent_path() == parent &&
            resolved.filename().string().starts_with("atx_replay_report_")) {
            fs::remove_all(resolved, ec);
        }
    }

    std::string path(const std::string &name) const { return (root / name).string(); }

    Result<impl::StageResult> policy_report(const impl::RunConfig &cfg,
        const std::vector<atx::usize> &decisions, const impl::ReplayReportPolicy &policy) {
        ATX_TRY(auto research, impl::read_pipeline_panel(cfg.panel, false));
        ATX_TRY(auto books, impl::read_pipeline_panel(cfg.books, false));
        const std::vector<double> costs(decisions.size(), 0.0);
        return impl::run_identified_replay_report(
            cfg, research, books, decisions, costs, nullptr, &policy);
    }

    Result<impl::RunConfig> inputs(
        const std::vector<double> &close, const std::vector<atx::usize> &decisions,
        const std::vector<double> &weights, double planning_bps = 0.0,
        std::optional<atx::usize> fit_boundary = std::nullopt,
        std::vector<atx::i64> sessions = {}, const std::string &entry_extra = "") {
        const auto dates = close.size();
        if (sessions.empty()) {
            for (atx::usize d = 0; d < dates; ++d) {
                sessions.push_back(kStart + static_cast<atx::i64>(d) * kDay);
            }
        }
        ATX_TRY(auto panel, Panel::create(dates, 1, {"close", "raw_close", "volume"},
            {close, close, std::vector<double>(dates, 1000.0)},
            std::vector<std::uint8_t>(dates, 1)));
        impl::PanelIdentity identity;
        identity.instrument_namespace = "synthetic.securityID";
        identity.session_keys = sessions;
        identity.instrument_ids = {kSecurity};
        identity.original_instrument_indices = {42};
        identity.recipe = "analytical replay fixture; historical availability unknown";
        identity.parents = {{"fixture", std::string(64, 'a')}};
        ATX_TRY(auto research, impl::write_panel_artifact(panel, path("research.bin"), identity));

        std::string combo_id;
        if (fit_boundary) {
            std::ofstream metadata(path("combo.bin.meta"), std::ios::binary);
            metadata << "n_periods=" << dates << "\nfit_begin=0\nfit_end=" << *fit_boundary
                     << "\nholdout_begin=" << *fit_boundary << "\n";
            metadata.close();
            ATX_TRY(auto hash, atx::core::sha256_file(path("combo.bin.meta")));
            auto combo_identity = identity;
            combo_identity.parents = {{"research", research.artifact_id}, {"fit-boundary", hash}};
            ATX_TRY(auto combo, Panel::create(dates, 1, {"alpha"},
                {std::vector<double>(dates, 0.5)}, std::vector<std::uint8_t>(dates, 1)));
            ATX_TRY(auto receipt, impl::write_panel_artifact(combo, path("combo.bin"), combo_identity));
            combo_id = receipt.artifact_id;
        }

        std::ofstream schedule(path("books.bin.meta.txt"), std::ios::binary);
        schedule << "periods=" << decisions.size() << "\ninstruments=1\n";
        for (atx::usize s = 0; s < decisions.size(); ++s) {
            schedule << "s=" << s << " period=" << decisions[s]
                     << " turnover=0 cost_bps=" << planning_bps << entry_extra << '\n';
        }
        schedule.close();
        ATX_TRY(auto schedule_hash, atx::core::sha256_file(path("books.bin.meta.txt")));
        identity.session_keys.clear();
        for (auto d : decisions) identity.session_keys.push_back(sessions.at(d));
        identity.recipe = "analytical target weights";
        identity.parents = {{"research", research.artifact_id}, {"book-schedule", schedule_hash}};
        if (!combo_id.empty()) identity.parents.push_back({"combo", combo_id});
        ATX_TRY(auto books, Panel::create(decisions.size(), 1, {"weight"}, {weights},
            std::vector<std::uint8_t>(decisions.size(), 1)));
        ATX_TRY(auto book_receipt, impl::write_panel_artifact(books, path("books.bin"), identity));
        (void)book_receipt;
        impl::RunConfig cfg;
        cfg.panel = path("research.bin");
        cfg.books = path("books.bin");
        cfg.report_out = path("report");
        cfg.report_aum = 100.0;
        if (fit_boundary) cfg.combo = path("combo.bin");
        return Ok(std::move(cfg));
    }
};

TEST_F(ReplayReport, WeeklyCoverageDriftExactAxesAndDeterministicManifest) {
    auto input = inputs({100, 110, 121, 121, 121, 121}, {0, 5}, {0.5, 0.0});
    ASSERT_TRUE(input.has_value()) << input.error().message();
    auto cfg = *input;
    cfg.replay_execution_delay = 0;
    auto result = impl::run_report(cfg);
    ASSERT_TRUE(result.has_value()) << result.error().message();
    const auto summary = json_file(root / "report/summary.json");
    EXPECT_NEAR(summary["full"]["final_nav"].get<double>(), 110.5, 1e-12);
    EXPECT_EQ(summary["full"]["observed_intervals"], 5);
    EXPECT_EQ(summary["effective_rebalances"], 1);
    EXPECT_EQ(summary["unexecuted_decisions"], 1);
    EXPECT_EQ(summary["actual_trade_count"], 1);
    EXPECT_EQ(summary["strategy_capacity"]["status"], "unavailable");
    EXPECT_EQ(metric(*result, "covered_intervals"), "5");
    EXPECT_NEAR(std::stod(metric(*result, "total_pnl_dollars")), 10.5, 1e-12);
    const auto ledger = contents(root / "report/ledger.csv");
    EXPECT_EQ(std::count(ledger.begin(), ledger.end(), '\n'), 6);
    auto manifest = json_file(root / "report/manifest.json");
    EXPECT_EQ(manifest["axes"]["instrument_ids"][0], kSecurity);
    EXPECT_EQ(manifest["axes"]["session_keys"][0], std::to_string(kStart));
    EXPECT_EQ(manifest["axes"]["original_instrument_indices"][0], "42");
    EXPECT_FALSE(manifest["recipe"]["historical_vintages_verified"].get<bool>());
    EXPECT_FALSE(fs::exists(root / "report/.pending"));
    for (const auto &file : manifest["files"]) {
        auto hash = atx::core::sha256_file((root / "report" / file["filename"].get<std::string>()).string());
        ASSERT_TRUE(hash.has_value());
        EXPECT_EQ(*hash, file["sha256"].get<std::string>());
    }
    const auto report_id = manifest["report_id"].get<std::string>();
    manifest.erase("report_id");
    auto expected_id = atx::core::sha256_hex("atx-replay-report-v1\n" + manifest.dump());
    ASSERT_TRUE(expected_id.has_value());
    EXPECT_EQ(*expected_id, report_id);
    EXPECT_EQ(report_id, metric(*result, "report_id"));
    cfg.report_out = path("repeat");
    auto repeat = impl::run_report(cfg);
    ASSERT_TRUE(repeat.has_value()) << repeat.error().message();
    EXPECT_EQ(repeat->digest, result->digest);
    EXPECT_EQ(contents(root / "repeat/manifest.json"), contents(root / "report/manifest.json"));
}

TEST_F(ReplayReport, DefaultDelayUsesNextObservationAndNeverTradesTerminalDecision) {
    auto input = inputs({100, 110, 121}, {0, 2}, {0.5, 0.0});
    ASSERT_TRUE(input.has_value());
    auto result = impl::run_report(*input);
    ASSERT_TRUE(result.has_value()) << result.error().message();
    const auto summary = json_file(root / "report/summary.json");
    EXPECT_NEAR(summary["full"]["final_nav"].get<double>(), 105.0, 1e-12);
    EXPECT_EQ(summary["full"]["observed_intervals"], 2);
    EXPECT_EQ(summary["unexecuted_decisions"], 1);
    EXPECT_EQ(summary["actual_trade_count"], 1);
    const auto trades = contents(root / "report/trades.csv");
    EXPECT_NE(trades.find("\n1," + std::to_string(kStart + kDay) + ",0,"), std::string::npos);
    EXPECT_NE(trades.find(kSecurity), std::string::npos);
    EXPECT_NE(trades.find("insufficient-prior-observations"), std::string::npos);
    EXPECT_EQ(json_file(root / "report/manifest.json")["recipe"]["execution_delay_observations"], "1");
}

TEST_F(ReplayReport, DollarTradeFeesAndActualCalendarBorrowReconcileCashAndAssets) {
    auto input = inputs({100, 100, 100}, {0, 2}, {-0.5, 0.0}, 0.0, std::nullopt,
                         {kStart, kStart + 3 * kDay, kStart + 4 * kDay});
    ASSERT_TRUE(input.has_value());
    auto cfg = *input;
    cfg.replay_execution_delay = 0;
    cfg.replay_trade_bps = 10;
    cfg.replay_annual_borrow_bps = 365;
    auto result = impl::run_report(cfg);
    ASSERT_TRUE(result.has_value()) << result.error().message();
    const auto summary = json_file(root / "report/summary.json");
    EXPECT_NEAR(summary["full"]["trade_cost_dollars"].get<double>(), 0.05, 1e-12);
    EXPECT_NEAR(summary["full"]["borrow_cost_dollars"].get<double>(), 0.02, 1e-12);
    EXPECT_NEAR(summary["full"]["final_nav"].get<double>(), 99.93, 1e-12);
    EXPECT_NEAR(summary["final_cash"].get<double>(), 149.93, 1e-12);
    EXPECT_NEAR(summary["final_assets"].get<double>(), -50.0, 1e-12);
    EXPECT_NEAR(summary["full"]["absolute_trade_dollars"].get<double>(), 50.0, 1e-12);
    EXPECT_EQ(summary["actual_trade_count"], 1);
}

TEST_F(ReplayReport, RejectsIgnoredLegacyRatesAndRequiresExplicitFeeForCostlyBooks) {
    auto input = inputs({100, 100, 100}, {0}, {0.5}, 5.0);
    ASSERT_TRUE(input.has_value());
    auto cfg = *input;
    auto implicit_zero = impl::run_report(cfg);
    ASSERT_FALSE(implicit_zero.has_value());
    EXPECT_NE(implicit_zero.error().message().find("choose --replay-trade-bps explicitly"), std::string::npos);
    EXPECT_FALSE(fs::exists(root / "report"));
    cfg.set_flags.insert("replay-trade-bps");
    cfg.borrow_bps = 1;
    EXPECT_FALSE(impl::run_report(cfg).has_value());
    cfg.borrow_bps = 0;
    cfg.cost_bps = 1;
    EXPECT_FALSE(impl::run_report(cfg).has_value());
    cfg.cost_bps = 0;
    cfg.replay_day_basis = 252;
    EXPECT_FALSE(impl::run_report(cfg).has_value());
    cfg.replay_day_basis = 365;
    auto explicit_zero = impl::run_report(cfg);
    ASSERT_TRUE(explicit_zero.has_value()) << explicit_zero.error().message();
    EXPECT_EQ(json_file(root / "report/summary.json")["full"]["trade_cost_dollars"], 0.0);
    EXPECT_TRUE(json_file(root / "report/manifest.json")["recipe"]["replay_trade_bps_explicit"].get<bool>());

    auto research = impl::read_panel(cfg.panel);
    auto books = impl::read_panel(cfg.books);
    ASSERT_TRUE(research.has_value());
    ASSERT_TRUE(books.has_value());
    ASSERT_TRUE(impl::write_panel(*research, path("legacy-research.bin")).has_value());
    ASSERT_TRUE(impl::write_panel(*books, path("legacy-books.bin")).has_value());
    ASSERT_TRUE(fs::copy_file(path("books.bin.meta.txt"), path("legacy-books.bin.meta.txt")));
    cfg.panel = path("legacy-research.bin");
    cfg.books = path("legacy-books.bin");
    cfg.report_out = path("legacy-report");
    cfg.allow_unidentified_panels = true;
    auto legacy_explicit_zero = impl::run_report(cfg);
    ASSERT_FALSE(legacy_explicit_zero.has_value());
    EXPECT_NE(legacy_explicit_zero.error().message().find("require identified"), std::string::npos);
    cfg.set_flags.clear();
    cfg.replay_trade_bps = 5;
    EXPECT_FALSE(impl::run_report(cfg).has_value());
    cfg.replay_trade_bps = 0;
    cfg.replay_execution_delay = 0;
    EXPECT_FALSE(impl::run_report(cfg).has_value());
    EXPECT_FALSE(fs::exists(root / "legacy-report"));
}

TEST_F(ReplayReport, MissingHeldPriceMapsExactSecurityAndDateWithoutCompletePublication) {
    auto input = inputs({100, 110, std::numeric_limits<double>::quiet_NaN()}, {0}, {0.5});
    ASSERT_TRUE(input.has_value());
    auto result = impl::run_report(*input);
    ASSERT_FALSE(result.has_value());
    EXPECT_NE(result.error().message().find("period=2"), std::string::npos);
    EXPECT_NE(result.error().message().find("security_id=" + std::string(kSecurity)), std::string::npos);
    EXPECT_NE(result.error().message().find("session_key_ns=" + std::to_string(kStart + 2 * kDay)),
              std::string::npos);
    EXPECT_TRUE(fs::exists(root / "report/.pending"));
    EXPECT_FALSE(fs::exists(root / "report/manifest.json"));
    EXPECT_FALSE(fs::exists(root / "report/summary.json"));
}

TEST_F(ReplayReport, ScheduleBytesAreBoundAndDuplicateCostCannotDefaultToFreeReplay) {
    auto input = inputs({100, 110, 121}, {0}, {0.5}, 5.0, std::nullopt, {}, " cost_bps=0");
    ASSERT_TRUE(input.has_value());
    auto cfg = *input;
    cfg.replay_trade_bps = 5;
    auto duplicate = impl::run_report(cfg);
    ASSERT_FALSE(duplicate.has_value());
    EXPECT_NE(duplicate.error().message().find("duplicate schedule cost"), std::string::npos);
    EXPECT_FALSE(fs::exists(root / "report"));
    std::ofstream changed(path("books.bin.meta.txt"), std::ios::binary);
    changed << "periods=1\ninstruments=1\ns=0 period=0 turnover=0 cost_bps=0\n";
    changed.close();
    auto mutation = impl::run_report(cfg);
    ASSERT_FALSE(mutation.has_value());
    EXPECT_FALSE(fs::exists(root / "report"));
}

TEST_F(ReplayReport, PostFitStartsAtNextEffectiveDecisionAndLiquidityRemainsExplicitlyUnknown) {
    auto input = inputs({100, 110, 121, 133.1, 146.41, 161.051}, {0, 3, 5}, {0.5, 0.5, 0.0}, 0.0, 2);
    ASSERT_TRUE(input.has_value());
    auto result = impl::run_report(*input);
    ASSERT_TRUE(result.has_value()) << result.error().message();
    const auto summary = json_file(root / "report/summary.json");
    EXPECT_EQ(summary["first_post_fit_effective_observation"], 4);
    EXPECT_EQ(summary["post_fit"]["observed_intervals"], 1);
    EXPECT_NEAR(summary["post_fit"]["initial_nav"].get<double>(), 116.55, 1e-11);
    EXPECT_NEAR(summary["post_fit"]["total_return"].get<double>(), 0.05, 1e-12);
    EXPECT_EQ(summary["trade_liquidity"]["known_count"], 0);
    EXPECT_EQ(summary["trade_liquidity"]["unknown_count"], 2);
    EXPECT_TRUE(summary["trade_liquidity"]["max_participation"].is_null());
    EXPECT_TRUE(summary["post_fit"]["sharpe_252"].is_null());
    const auto manifest = json_file(root / "report/manifest.json");
    const auto combo = impl::read_panel_artifact(path("combo.bin"));
    ASSERT_TRUE(combo.has_value());
    bool bound_combo = false;
    for (const auto &parent : manifest["parents"]) {
        if (parent["role"] == "combo") bound_combo = parent["sha256"] == combo->artifact_id;
    }
    EXPECT_TRUE(bound_combo);
    auto changed = *input;
    changed.report_out = path("mutated-fit");
    std::ofstream mutation(path("combo.bin.meta"), std::ios::app);
    mutation << "holdout_begin=0\n";
    mutation.close();
    EXPECT_FALSE(impl::run_report(changed).has_value());
    EXPECT_FALSE(fs::exists(root / "mutated-fit"));
}

TEST_F(ReplayReport, EmptyDirectoryAllowedButCompletedAndPartialOutputsNeverOverwrite) {
    auto input = inputs({100, 100, 100}, {0}, {0.5});
    ASSERT_TRUE(input.has_value());
    ASSERT_TRUE(fs::create_directory(root / "report"));
    auto result = impl::run_report(*input);
    ASSERT_TRUE(result.has_value()) << result.error().message();
    const auto manifest = contents(root / "report/manifest.json");
    const auto ledger = contents(root / "report/ledger.csv");
    EXPECT_FALSE(impl::run_report(*input).has_value());
    EXPECT_EQ(contents(root / "report/manifest.json"), manifest);
    EXPECT_EQ(contents(root / "report/ledger.csv"), ledger);
    EXPECT_FALSE(fs::exists(root / "report/.pending"));
    auto partial = *input;
    partial.report_out = path("partial");
    ASSERT_TRUE(fs::create_directories(root / "partial/.pending"));
    std::ofstream marker(root / "partial/ledger.csv.partial", std::ios::binary);
    marker << "failure evidence\n";
    marker.close();
    EXPECT_FALSE(impl::run_report(partial).has_value());
    EXPECT_EQ(contents(root / "partial/ledger.csv.partial"), "failure evidence\n");
    EXPECT_FALSE(fs::exists(root / "partial/manifest.json"));
}

TEST_F(ReplayReport, PolicyAllocationsBindActualDollarRowsAndModelProvenance) {
    const std::vector<atx::usize> decisions{0, 1, 3};
    auto input = inputs({100, 120, 120, 120}, decisions, {0.5, 0.5, 0.0});
    ASSERT_TRUE(input.has_value());
    auto cfg = *input;
    cfg.replay_execution_delay = 0;
    impl::ReplayReportPolicy policy;
    bool mutate_descriptor = true;
    policy.callback = [&](const atx::engine::book::ReplayAllocationState &state) {
        if (mutate_descriptor) {
            policy.recipe = "{\"mutation\":\"after-snapshot\"}";
            policy.parents[0].sha256 = std::string(64, 'd');
            mutate_descriptor = false;
        }
        const auto weight = state.schedule_index == 0
            ? state.preference_weights[0] / 2.0 : state.marked_dollars[0] / state.pretrade_nav;
        return Ok(std::vector<double>{weight});
    };
    policy.recipe = Json{{"model", "half-entry-then-hold"}, {"version", 1}}.dump();
    policy.parents = {{"risk-context", std::string(64, 'b')}};
    const auto frozen_recipe = policy.recipe;
    const auto result = policy_report(cfg, decisions, policy);
    ASSERT_TRUE(result.has_value()) << result.error().message();
    EXPECT_EQ(metric(*result, "report_mode"), "identified-policy-holdings-replay-v1");
    const auto summary = json_file(root / "report/summary.json");
    EXPECT_DOUBLE_EQ(summary["full"]["final_nav"].get<double>(), 105.0);
    EXPECT_EQ(summary["actual_trade_count"], 1);
    EXPECT_EQ(summary["accepted_allocation_count"], 2);
    EXPECT_EQ(summary["books_role"], "frozen-decision-preferences");
    const auto csv = contents(root / "report/allocations.csv");
    EXPECT_EQ(std::count(csv.begin(), csv.end(), '\n'), 3);
    EXPECT_NE(csv.find("\n0,0,0," + std::to_string(kStart) + "," + std::to_string(kStart) +
                       ",0," + kSecurity + ",0.25,0,25,100,100,100,75,25,0\n"),
              std::string::npos);
    EXPECT_NE(csv.find(",30,30,105,105,75,75,0,0\n"), std::string::npos);
    auto manifest = json_file(root / "report/manifest.json");
    EXPECT_EQ(manifest["recipe"]["allocation_policy"], Json::parse(frozen_recipe));
    EXPECT_EQ(manifest["recipe"]["books_role"], "frozen-decision-preferences");
    EXPECT_EQ(manifest["recipe"]["allocation_constraints"],
              "policy-defined-not-certified-by-generic-report");
    EXPECT_TRUE(std::any_of(manifest["parents"].begin(), manifest["parents"].end(),
        [&](const Json &parent) {
            return parent["role"] == "risk-context" && parent["sha256"] == std::string(64, 'b');
        }));
    bool found_allocations = false;
    for (const auto &file : manifest["files"]) {
        const auto filename = file["filename"].get<std::string>();
        const auto hash = atx::core::sha256_file((root / "report" / filename).string());
        ASSERT_TRUE(hash.has_value());
        EXPECT_EQ(*hash, file["sha256"].get<std::string>());
        found_allocations = found_allocations || filename == "allocations.csv";
    }
    EXPECT_TRUE(found_allocations);
    const auto report_id = manifest["report_id"].get<std::string>();
    manifest.erase("report_id");
    const auto expected_id = atx::core::sha256_hex("atx-replay-report-v1\n" + manifest.dump());
    ASSERT_TRUE(expected_id.has_value());
    EXPECT_EQ(*expected_id, report_id);
    policy.recipe = frozen_recipe;
    policy.parents[0].sha256 = std::string(64, 'b');
    cfg.report_out = path("repeat-policy");
    const auto repeat = policy_report(cfg, decisions, policy);
    ASSERT_TRUE(repeat.has_value()) << repeat.error().message();
    EXPECT_EQ(repeat->digest, result->digest);
    EXPECT_EQ(contents(root / "repeat-policy/manifest.json"),
              contents(root / "report/manifest.json"));
    cfg.report_out = path("other-recipe");
    policy.recipe = Json{{"model", "same-callback-distinct-declared-recipe"}}.dump();
    const auto other_recipe = policy_report(cfg, decisions, policy);
    ASSERT_TRUE(other_recipe.has_value()) << other_recipe.error().message();
    EXPECT_NE(metric(*other_recipe, "report_id"), report_id);
    EXPECT_EQ(contents(root / "other-recipe/allocations.csv"), csv);
    cfg.report_out = path("other-parent");
    policy.parents[0].sha256 = std::string(64, 'c');
    const auto other_parent = policy_report(cfg, decisions, policy);
    ASSERT_TRUE(other_parent.has_value()) << other_parent.error().message();
    EXPECT_NE(metric(*other_parent, "report_id"), metric(*other_recipe, "report_id"));
    cfg.report_out = path("fixed");
    const auto fixed = impl::run_report(cfg);
    ASSERT_TRUE(fixed.has_value()) << fixed.error().message();
    EXPECT_DOUBLE_EQ(json_file(root / "fixed/summary.json")["full"]["final_nav"].get<double>(),
                     110.0);
    EXPECT_FALSE(fs::exists(root / "fixed/allocations.csv"));
    EXPECT_FALSE(json_file(root / "fixed/manifest.json")["recipe"].contains("allocation_policy"));
    EXPECT_EQ(metric(*fixed, "report_mode"), "identified-holdings-replay-v1");
}

TEST_F(ReplayReport, ExplicitIntentReportBindsHoldCloseAndActualCashFlows) {
    namespace book = atx::engine::book;
    const std::vector<atx::usize> decisions{0, 1, 2};
    const auto input = inputs({100, 120, 120, 120}, decisions, {0.25, 0.25, 0.25});
    ASSERT_TRUE(input.has_value());
    auto cfg = *input;
    cfg.replay_execution_delay = 0;
    cfg.replay_trade_bps = 5;
    impl::ReplayReportPolicy policy;
    policy.recipe = Json{{"model", "entry-exact-hold-close"}, {"version", 1}}.dump();
    policy.intent_callback = [](const book::ReplayAllocationState &state) {
        const book::ReplayTargetIntent intent = state.schedule_index == 0
            ? book::ReplayTargetIntent{book::ReplayTargetAction::TargetWeight, 0.25}
            : book::ReplayTargetIntent{state.schedule_index == 1
                ? book::ReplayTargetAction::HoldCurrent : book::ReplayTargetAction::Close, 0.0};
        return Ok(std::vector<book::ReplayTargetIntent>{intent});
    };
    const auto result = policy_report(cfg, decisions, policy);
    ASSERT_TRUE(result.has_value()) << result.error().message();
    const auto summary = json_file(root / "report/summary.json");
    EXPECT_EQ(summary["allocation_model"], "explicit-intents-against-marked-holdings-v2");
    EXPECT_EQ(summary["actual_trade_count"], 2);
    EXPECT_EQ(summary["accepted_allocation_count"], 3);
    EXPECT_NEAR(summary["full"]["final_nav"].get<double>(), 104.9725, 1e-12);
    const auto csv = contents(root / "report/allocations.csv");
    EXPECT_EQ(std::count(csv.begin(), csv.end(), '\n'), 4);
    EXPECT_NE(csv.find("resolved_intent_pretrade_nav_weight"), std::string::npos);
    EXPECT_NE(csv.find("target_action,intent_weight_payload\n"), std::string::npos);
    EXPECT_NE(csv.find(",target-weight,0.25\n"), std::string::npos);
    EXPECT_NE(csv.find(",0,0,hold-current,0\n"), std::string::npos);
    EXPECT_NE(csv.find(",close,0\n"), std::string::npos);
    auto manifest = json_file(root / "report/manifest.json");
    EXPECT_EQ(manifest["recipe"]["allocation_policy"], Json::parse(policy.recipe));
    EXPECT_EQ(manifest["recipe"]["hold_instruction"],
              "copy-held-TRI-units-and-marked-dollars-exactly");
    const auto csv_hash = atx::core::sha256_hex(csv);
    ASSERT_TRUE(csv_hash.has_value());
    bool bound = false;
    for (const auto &file : manifest["files"]) {
        if (file["filename"] == "allocations.csv") {
            EXPECT_EQ(file["sha256"].get<std::string>(), *csv_hash);
            bound = true;
        }
    }
    EXPECT_TRUE(bound);
}

TEST_F(ReplayReport, InvalidPolicyDescriptorsFailBeforeOutputReservation) {
    const std::vector<atx::usize> decisions{0};
    const auto input = inputs({100, 100, 100}, decisions, {0.5});
    ASSERT_TRUE(input.has_value());
    atx::usize calls = 0;
    impl::ReplayReportPolicy valid;
    valid.callback = [&](const atx::engine::book::ReplayAllocationState &) {
        ++calls;
        return Ok(std::vector<double>{0.5});
    };
    valid.recipe = "{}";
    std::vector<impl::ReplayReportPolicy> invalid;
    auto candidate = valid;
    candidate.callback = {};
    invalid.push_back(candidate);
    candidate = valid;
    candidate.intent_callback = [](const atx::engine::book::ReplayAllocationState &) {
        return Ok(std::vector<atx::engine::book::ReplayTargetIntent>{
            {atx::engine::book::ReplayTargetAction::Close, 0.0}});
    };
    invalid.push_back(candidate); // Ambiguous callback types reject before reservation.
    for (const auto &recipe : std::vector<std::string>{
            "", "{", "[]", "{ }", "{\"x\":1,\"x\":1}",
            std::string(1024U * 1024U + 1U, ' '),
            "{\"x\":" + std::string(66, '[') + "0" + std::string(66, ']') + "}"}) {
        candidate = valid;
        candidate.recipe = recipe;
        invalid.push_back(candidate);
    }
    for (const auto &parents : std::vector<std::vector<impl::PanelParent>>{
            {{"research", std::string(64, 'a')}}, {{"books", std::string(64, 'a')}},
            {{"book-schedule", std::string(64, 'a')}}, {{"combo", std::string(64, 'a')}},
            {{"fit-boundary", std::string(64, 'a')}}, {{"", std::string(64, 'a')}},
            {{"risk\ncontext", std::string(64, 'a')}},
            {{std::string(257, 'r'), std::string(64, 'a')}},
            {{"risk", std::string(64, 'A')}}, {{"risk", "abc"}},
            {{"risk", std::string(64, 'a')}, {"risk", std::string(64, 'b')}},
            std::vector<impl::PanelParent>(257, {"risk", std::string(64, 'a')})}) {
        candidate = valid;
        candidate.parents = parents;
        invalid.push_back(candidate);
    }
    for (atx::usize i = 0; i < invalid.size(); ++i) {
        auto cfg = *input;
        cfg.report_out = path("invalid-policy-" + std::to_string(i));
        const auto result = policy_report(cfg, decisions, invalid[i]);
        EXPECT_FALSE(result.has_value()) << i;
        EXPECT_FALSE(fs::exists(cfg.report_out)) << i;
    }
    EXPECT_EQ(calls, 0U);
}

TEST_F(ReplayReport, LaterPolicyFailurePublishesNoAcceptedAllocationOrCompleteManifest) {
    const std::vector<atx::usize> decisions{0, 1};
    const auto input = inputs({100, 100, 100}, decisions, {0.5, 0.5});
    ASSERT_TRUE(input.has_value());
    auto cfg = *input;
    cfg.replay_execution_delay = 0;
    atx::usize calls = 0;
    impl::ReplayReportPolicy policy;
    policy.recipe = "{\"model\":\"intentional-failure\"}";
    policy.callback = [&](const atx::engine::book::ReplayAllocationState &)
        -> Result<std::vector<double>> {
        if (++calls == 1) return Ok(std::vector<double>{0.5});
        return atx::core::Err(atx::core::ErrorCode::Unavailable, "declared infeasible allocation");
    };
    const auto result = policy_report(cfg, decisions, policy);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().message(), "declared infeasible allocation");
    EXPECT_EQ(calls, 2U);
    EXPECT_TRUE(fs::exists(root / "report/.pending"));
    for (const auto *name : {"allocations.csv", "ledger.csv", "trades.csv", "summary.json",
                             "manifest.json"}) {
        EXPECT_FALSE(fs::exists(root / "report" / name));
    }
}

} // namespace
