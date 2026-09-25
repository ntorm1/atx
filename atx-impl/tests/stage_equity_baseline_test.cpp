#include <algorithm>
#include <atomic>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <string>
#include <vector>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "atx/core/sha256.hpp"
#include "atx/engine/data/orats_history.hpp"
#include "panel_artifact.hpp"
#include "stage_equity_baseline.hpp"
#include "stage_equity_book.hpp"

namespace {
namespace fs = std::filesystem;
namespace impl = atx::impl;
using Json = nlohmann::json;
using atx::core::Ok;
using atx::core::Result;
using atx::engine::alpha::Panel;
constexpr atx::i64 kDay = 86'400'000'000'000LL;
constexpr const char *kId = "9007199254740993";

std::string contents(const fs::path &path) {
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

Json json_file(const fs::path &path) { return Json::parse(contents(path)); }

class StageEquityBaseline : public ::testing::Test {
protected:
    fs::path root;

    void SetUp() override {
        static std::atomic<unsigned> sequence{};
        for (unsigned attempt = 0; attempt < 1000; ++attempt) {
            root = fs::temp_directory_path() /
                ("atx_equity_baseline_" + std::to_string(sequence.fetch_add(1)));
            if (fs::create_directory(root)) return;
        }
        FAIL() << "Cannot reserve fixture root";
    }

    void TearDown() override {
        std::error_code ec;
        const auto parent = fs::weakly_canonical(fs::temp_directory_path(), ec);
        if (ec) return;
        const auto resolved = fs::weakly_canonical(root, ec);
        if (!ec && resolved.parent_path() == parent &&
            resolved.filename().string().starts_with("atx_equity_baseline_")) {
            fs::remove_all(resolved, ec);
        }
    }

    Result<impl::RunConfig> input(bool held_gap = false, bool unavailable_short_history = false,
                                  bool bad_source_basis = false, bool unheld_execution_gap = false) {
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
        if (held_gap) {
            close[259 * names] = std::numeric_limits<double>::quiet_NaN();
            mask[259 * names] = 0;
        }
        if (unheld_execution_gap) {
            close[257 * names] = std::numeric_limits<double>::quiet_NaN();
            mask[257 * names] = 0;
        }
        if (unavailable_short_history) {
            // The 126-lag signal is ready, while the 252-lag signal needs this
            // first observation at the first evaluation row. Both must be gated.
            close[4 * names] = std::numeric_limits<double>::quiet_NaN();
        }
        std::vector<double> raw = close;
        for (auto &value : raw) value *= 2;
        ATX_TRY(auto panel, Panel::create(dates, names, {"close", "raw_close", "volume"},
            {close, raw, std::vector<double>(dates * names, 1e6)}, std::move(mask)));
        impl::PanelIdentity identity;
        identity.instrument_namespace = impl::kSpiderRockSecurityIdNamespace;
        identity.instrument_ids = {kId, "2"};
        identity.original_instrument_indices = {42, 7};
        for (atx::usize d = 0; d < dates; ++d) identity.session_keys.push_back(first + static_cast<atx::i64>(d) * kDay);
        identity.parents = {{"synthetic-fixture", std::string(64, 'a')}};
        identity.recipe = Json{{"version", "tickerhistory-panel-v2-identified"},
            {"start_inclusive_nanos", std::to_string(first)},
            {"end_exclusive_nanos", std::to_string(first + static_cast<atx::i64>(dates) * kDay)},
            {"research_ohlc", bad_source_basis ? "unadjusted" : "raw-OHLC*cumulReturnFactor-pointwise"},
            {"raw_close", "unadjusted-as-traded"}, {"volume", "raw-reported-volume-no-total-return-factor-rescaling"},
            {"universe", {{"adv_basis", "raw_close*raw_volume"}, {"top_n_tie_break", "original-instrument-index"},
                {"top_n_by_adv", 1000}, {"min_adv_usd", 20e6}, {"min_raw_price_exclusive", 5.0},
                {"adv_window_bars", 21}, {"min_mktcap_usd", 0.0}, {"require_sector", false},
                {"current_session_data_included", true}, {"adv_missing", "full-trailing-window-any-NaN-invalid"}}},
            {"compact_to_universe", true}, {"compaction", "keep-original-order-if-ever-in-universe-in-selected-window"},
            {"augmentation", {{"enabled", false}, {"adv_windows", Json::array()}}},
            {"historical_availability", "unknown-archive-snapshot"},
            {"fixture", "synthetic-only-no-market-or-performance-evidence"}}.dump();
        ATX_TRY(auto receipt, impl::write_panel_artifact(panel, (root / "context.bin").string(), identity));
        (void)receipt;
        impl::RunConfig cfg;
        cfg.panel = (root / "context.bin").string();
        cfg.out = (root / "baseline").string();
        cfg.equity_evaluation_start = "2013-04-04";
        cfg.equity_evaluation_end = "2013-04-12";
        cfg.report_aum = 1000;
        return Ok(std::move(cfg));
    }
};

TEST_F(StageEquityBaseline, FixedUnfitBlendReplaysOnlyEvaluationAndBindsAncestry) {
    const auto cfg = input();
    ASSERT_TRUE(cfg.has_value()) << cfg.error().message();
    const auto result = impl::run_equity_baseline(*cfg);
    ASSERT_TRUE(result.has_value()) << result.error().message();
    const auto directory = root / "baseline";
    const auto summary = json_file(directory / "summary.json");
    EXPECT_EQ(summary["readiness"]["observations"], 8);
    EXPECT_EQ(summary["readiness"]["evaluation_begin_context_row"], 256);
    EXPECT_EQ(summary["readiness"]["admitted_evaluation_cells"], 16);
    EXPECT_EQ(summary["replay"]["observed_intervals"], 7);
    EXPECT_GT(summary["replay"]["absolute_trade_dollars"].get<double>(), 20);
    EXPECT_GT(summary["replay"]["trade_cost_dollars"].get<double>(), 0);
    EXPECT_GT(summary["replay"]["borrow_cost_dollars"].get<double>(), 0);
    EXPECT_EQ(summary["qualification"], "unknown");
    EXPECT_EQ(summary["strategy_capacity"], "unavailable");
    EXPECT_NEAR(summary["target_exposures"]["max_target_gross"].get<double>(), 0.02, 1e-12);
    EXPECT_NEAR(summary["target_exposures"]["max_abs_target_net"].get<double>(), 0, 1e-12);
    const auto evaluation = impl::read_panel_artifact((directory / "evaluation.bin").string());
    const auto combo = impl::read_panel_artifact((directory / "combo.bin").string());
    const auto books = impl::read_panel_artifact((directory / "books.bin").string());
    ASSERT_TRUE(evaluation.has_value()); ASSERT_TRUE(combo.has_value()); ASSERT_TRUE(books.has_value());
    EXPECT_EQ(evaluation->identity.instrument_ids[0], kId);
    EXPECT_EQ(evaluation->identity.original_instrument_indices, (std::vector<atx::usize>{42, 7}));
    EXPECT_TRUE(impl::require_panel_parent(combo->identity, "research", evaluation->artifact_id));
    EXPECT_TRUE(impl::require_panel_parent(books->identity, "combo", combo->artifact_id));
    ASSERT_EQ(books->identity.session_keys.size(), 2U);
    EXPECT_EQ(books->identity.session_keys[0], evaluation->identity.session_keys[0]);
    EXPECT_EQ(books->identity.session_keys[1], evaluation->identity.session_keys[5]);
    EXPECT_NE(contents(directory / "combo.bin.meta").find("fit_kind=unfit-constant-weights"), std::string::npos);
    const auto manifest = json_file(directory / "manifest.json");
    EXPECT_EQ(manifest["recipe"]["trade_bps_per_absolute_dollar"], 5);
    EXPECT_EQ(manifest["recipe"]["annual_borrow_bps"], 365);
    EXPECT_EQ(manifest["recipe"]["execution_delay_observations"], "1");
    EXPECT_EQ(manifest["recipe"]["raw_data_economics"], "unverified");
    EXPECT_FALSE(fs::exists(directory / ".pending"));
    const auto report = json_file(directory / "report/summary.json");
    EXPECT_EQ(report["effective_rebalances"], 2);
    EXPECT_EQ(report["actual_trade_count"], 4);
    EXPECT_EQ(summary["usable_for_alpha_evidence"], false);
    EXPECT_EQ(summary["performance_evidence_eligibility"], "unverified-research-diagnostic");
    EXPECT_EQ(summary["assumed_liquidation_count"], 0);
    EXPECT_EQ(summary["assumed_liquidation_pnl_dollars"], 0);
    const auto ledger = contents(directory / "report/ledger.csv");
    // The first complete interval is all cash under the default one-observation delay.
    EXPECT_NE(ledger.find(",1000,1000,0,1000,0,0,0,0,0,0,0\n"), std::string::npos);
}

TEST_F(StageEquityBaseline, ExplicitZeroCostsDelayAndGlobalDefaultAumArePreserved) {
    auto cfg = input();
    ASSERT_TRUE(cfg.has_value());
    cfg->replay_execution_delay = 0;
    cfg->replay_trade_bps = 0;
    cfg->replay_annual_borrow_bps = 0;
    cfg->report_aum = 1e9;
    cfg->allow_same_close = true; // W0-I0b / B-02: delay 0 is an explicit opt-in now
    cfg->set_flags = {"replay-execution-delay", "replay-trade-bps", "replay-annual-borrow-bps",
                      "report-aum", "allow-same-close"};
    auto result = impl::run_equity_baseline(*cfg);
    ASSERT_TRUE(result.has_value()) << result.error().message();
    const auto manifest = json_file(root / "baseline/manifest.json");
    EXPECT_EQ(manifest["recipe"]["initial_nav"], 1e9);
    EXPECT_EQ(manifest["recipe"]["execution_delay_observations"], "0");
    const auto summary = json_file(root / "baseline/summary.json");
    EXPECT_EQ(summary["replay"]["trade_cost_dollars"], 0);
    EXPECT_EQ(summary["replay"]["borrow_cost_dollars"], 0);
}

TEST_F(StageEquityBaseline, CommonReadinessGatesBothStreamsBeforeRanking) {
    auto cfg = input(false, true);
    ASSERT_TRUE(cfg.has_value());
    auto result = impl::run_equity_baseline(*cfg);
    ASSERT_TRUE(result.has_value()) << result.error().message();
    const auto evaluation = impl::read_panel_artifact((root / "baseline/evaluation.bin").string());
    const auto combo = impl::read_panel_artifact((root / "baseline/combo.bin").string());
    ASSERT_TRUE(evaluation.has_value()); ASSERT_TRUE(combo.has_value());
    EXPECT_FALSE(evaluation->panel.in_universe(0, 0));
    EXPECT_TRUE(std::isnan(combo->panel.field_all(0)[0]));
    // The one remaining eligible instrument has no cross-sectional long-short signal.
    EXPECT_DOUBLE_EQ(combo->panel.field_all(0)[1], 0);
}

TEST_F(StageEquityBaseline, MissingHeldMarkPreservesBoundFailureAndNoCompleteManifest) {
    auto cfg = input(true);
    ASSERT_TRUE(cfg.has_value());
    cfg->replay_delisting_policy = impl::ReplayDelistingPolicy::AbortV1;
    cfg->set_flags.insert("replay-delisting-policy");
    auto result = impl::run_equity_baseline(*cfg);
    ASSERT_FALSE(result.has_value());
    EXPECT_NE(result.error().message().find("required close"), std::string::npos);
    EXPECT_NE(result.error().message().find(kId), std::string::npos);
    EXPECT_FALSE(fs::exists(root / "baseline/manifest.json"));
    EXPECT_FALSE(fs::exists(root / "baseline/report/manifest.json"));
    EXPECT_TRUE(fs::exists(root / "baseline/.pending"));
    const auto failure = json_file(root / "baseline/failure.json");
    EXPECT_EQ(failure["status"], "failed");
    EXPECT_EQ(failure["source_context_artifact_id"].get<std::string>().size(), 64U);
    EXPECT_EQ(failure["recipe"]["evaluation_end_exclusive"], "2013-04-12");
    EXPECT_TRUE(fs::exists(root / "baseline/books.bin.manifest.json"));
}

TEST_F(StageEquityBaseline, CorrectedDefaultCompletesOriginalWindowAndConstrainedBook) {
    auto baseline = input(true);
    ASSERT_TRUE(baseline);
    baseline->set_flags.insert("replay-delisting-policy");
    const auto result = impl::run_equity_baseline(*baseline);
    ASSERT_TRUE(result) << result.error().message();
    const auto summary = json_file(root / "baseline/report/summary.json");
    EXPECT_EQ(summary["full"]["observed_intervals"], 7);
    EXPECT_EQ(summary["flagged_delistings"], 1);
    EXPECT_EQ(summary["gap_carry_count"], 0);
    EXPECT_EQ(json_file(root / "baseline/manifest.json")["recipe"]["replay_delisting_policy"],
              "terminal-return");
    impl::RunConfig cfg;
    cfg.panel = baseline->panel;
    cfg.equity_baseline_dir = baseline->out;
    cfg.out = (root / "book_causal").string();
    cfg.set_flags.insert("replay-delisting-policy");
    const auto constrained = impl::run_equity_book(cfg);
    ASSERT_TRUE(constrained) << constrained.error().message();
    const auto book_summary = json_file(root / "book_causal/report/summary.json");
    EXPECT_EQ(book_summary["full"]["observed_intervals"], 7);
    EXPECT_EQ(book_summary["flagged_delistings"], 1);
    EXPECT_EQ(json_file(root / "book_causal/report/manifest.json")
        ["recipe"]["replay_delisting_policy"], "terminal-return");
    for (const auto &directory : {root / "baseline", root / "book_causal"}) {
        const auto outer = json_file(directory / "summary.json");
        const auto nested = json_file(directory / "report/summary.json");
        EXPECT_EQ(outer["replay"], nested["full"]);
        for (const char *key : {"usable_for_alpha_evidence", "performance_evidence_eligibility",
                 "terminal_liquidation_count", "evidenced_delisting_count", "flagged_delistings",
                 "flagged_short_delistings", "flagged_short_pnl_dollars",
                 "assumed_liquidation_count",
                 "assumed_liquidation_pnl_dollars", "gap_carry_count"}) {
            EXPECT_EQ(outer.at(key), nested.at(key)) << directory << " " << key;
        }
        EXPECT_EQ(outer["usable_for_alpha_evidence"], false);
        EXPECT_EQ(outer["assumed_liquidation_count"], 1);
        EXPECT_EQ(outer["evidenced_delisting_count"], 0);
        EXPECT_LT(outer["assumed_liquidation_pnl_dollars"].get<double>(), 0);
        EXPECT_EQ(outer["qualification"], "failed");
        EXPECT_EQ(outer["performance_evidence_eligibility"],
                  "ineligible-assumed-missing-price-liquidation");
        const auto &reasons = outer.at("qualification_reasons");
        EXPECT_NE(std::find(reasons.begin(), reasons.end(),
            "ineligible-assumed-missing-price-liquidation"), reasons.end());
        const auto manifest = json_file(directory / "manifest.json");
        EXPECT_EQ(manifest["qualification"], "failed");
        const auto sha = atx::core::sha256_file((directory / "summary.json").string());
        ASSERT_TRUE(sha);
        bool bound = false;
        for (const auto &file : manifest.at("files")) {
            if (file.at("filename") == "summary.json") {
                bound = true;
                EXPECT_EQ(file.at("sha256"), *sha);
            }
        }
        EXPECT_TRUE(bound) << directory;
    }
}

TEST_F(StageEquityBaseline, RejectsUnsupportedModesSealedDatesCoverageAndMemoryFloor) {
    auto cfg = input();
    ASSERT_TRUE(cfg.has_value());
    auto unsupported = *cfg; unsupported.set_flags.insert("method");
    EXPECT_FALSE(impl::run_equity_baseline(unsupported).has_value());
    auto sealed = *cfg; sealed.equity_evaluation_end = "2023-01-01";
    EXPECT_FALSE(impl::run_equity_baseline(sealed).has_value());
    auto too_small = *cfg; too_small.equity_max_working_bytes = 512'000'001;
    EXPECT_FALSE(impl::run_equity_baseline(too_small).has_value());
    EXPECT_TRUE(fs::exists(root / "baseline/failure.json"));
    auto coverage = *cfg; coverage.out = (root / "coverage").string();
    coverage.equity_evaluation_end = "2013-12-31";
    const auto rejected = impl::run_equity_baseline(coverage);
    ASSERT_FALSE(rejected.has_value());
    EXPECT_NE(rejected.error().message().find("coverage"), std::string::npos);
    EXPECT_FALSE(fs::exists(root / "coverage/evaluation.bin"));
}

TEST_F(StageEquityBaseline, RejectsUnknownPriceBasisBeforeNumericFeatureEvaluation) {
    const auto cfg = input(false, false, true);
    ASSERT_TRUE(cfg.has_value());
    const auto result = impl::run_equity_baseline(*cfg);
    ASSERT_FALSE(result.has_value());
    EXPECT_NE(result.error().message().find("recipe"), std::string::npos);
    EXPECT_FALSE(fs::exists(root / "baseline/evaluation.bin"));
}

TEST_F(StageEquityBaseline, PublicationHashesDeterminismAndNoOverwrite) {
    auto cfg = input();
    ASSERT_TRUE(cfg.has_value());
    const auto first = impl::run_equity_baseline(*cfg);
    ASSERT_TRUE(first.has_value()) << first.error().message();
    auto manifest = json_file(root / "baseline/manifest.json");
    for (const auto &file : manifest.at("files")) {
        const auto hash = atx::core::sha256_file((root / "baseline" / file.at("filename").get<std::string>()).string());
        ASSERT_TRUE(hash.has_value());
        EXPECT_EQ(*hash, file.at("sha256").get<std::string>());
    }
    const auto id = manifest.at("baseline_id").get<std::string>();
    manifest.erase("baseline_id");
    const auto hash = atx::core::sha256_hex("atx-equity-baseline-v1\n" + manifest.dump());
    ASSERT_TRUE(hash.has_value()); EXPECT_EQ(*hash, id);
    EXPECT_FALSE(impl::run_equity_baseline(*cfg).has_value());
    EXPECT_EQ(json_file(root / "baseline/manifest.json")["baseline_id"].get<std::string>(), id);
    cfg->out = (root / "repeat").string();
    const auto repeated = impl::run_equity_baseline(*cfg);
    ASSERT_TRUE(repeated.has_value()) << repeated.error().message();
    EXPECT_EQ(first->digest, repeated->digest);
    EXPECT_EQ(json_file(root / "repeat/manifest.json")["baseline_id"].get<std::string>(), id);
}

TEST_F(StageEquityBaseline, ConstrainedBookPublishesActualAllocationsAndContextAncestry) {
    const auto baseline = input();
    ASSERT_TRUE(baseline.has_value());
    const auto prepared = impl::run_equity_baseline(*baseline);
    ASSERT_TRUE(prepared.has_value()) << prepared.error().message();
    impl::RunConfig cfg;
    cfg.panel = baseline->panel;
    cfg.equity_baseline_dir = baseline->out;
    cfg.out = (root / "book").string();
    cfg.report_aum = baseline->report_aum;
    const auto result = impl::run_equity_book(cfg);
    ASSERT_TRUE(result.has_value()) << result.error().message();

    const auto context = impl::read_panel_artifact(cfg.panel);
    ASSERT_TRUE(context.has_value());
    const auto directory = root / "book";
    const auto manifest = json_file(directory / "manifest.json");
    EXPECT_EQ(manifest["schema"], "atx-equity-book-v1");
    const auto id = manifest.at("book_id").get<std::string>();
    EXPECT_EQ(id.size(), 64U);
    EXPECT_TRUE(manifest.at("recipe").is_object());
    EXPECT_FALSE(manifest.at("files").empty());
    bool context_bound = false;
    for (const auto &parent : manifest.at("parents")) {
        if (parent.at("role") == "source-context")
            context_bound = parent.at("sha256") == context->artifact_id;
    }
    EXPECT_TRUE(context_bound);
    EXPECT_FALSE(fs::exists(directory / ".pending"));

    const auto report = json_file(directory / "report/manifest.json");
    EXPECT_EQ(report["recipe"]["initial_nav"], 1000);
    EXPECT_EQ(report["recipe"]["execution_delay_observations"], "1");
    EXPECT_EQ(report["recipe"]["trade_bps_per_absolute_dollar"], 5);
    EXPECT_EQ(report["recipe"]["annual_borrow_bps"], 365);
    const auto certificates = json_file(directory / "allocation_certificates.json");
    const auto &decisions = certificates.at("decisions");
    ASSERT_TRUE(decisions.is_array());
    ASSERT_EQ(decisions.size(), 2U);
    const auto period = [](const Json &value) -> atx::usize {
        return value.is_string() ? static_cast<atx::usize>(std::stoull(value.get<std::string>()))
                                 : value.get<atx::usize>();
    };
    for (atx::usize i = 0; i < decisions.size(); ++i) {
        const auto &decision = decisions[i];
        EXPECT_TRUE(decision.at("solver_used").get<bool>());
        EXPECT_EQ(decision.at("solver_certificate_scope"), "continuous-weights-before-representation");
        EXPECT_EQ(decision.at("continuous_weights_canonical_order").size(), 2U);
        EXPECT_EQ(decision.at("proposed_intents_canonical_order").size(), 2U);
        EXPECT_LE(decision.at("representation_l1_change").get<double>(),
                  decision.at("representation_l1_budget").get<double>());
        EXPECT_EQ(period(decision.at("decision_period")), i * 5);
        EXPECT_EQ(period(decision.at("execution_period")), i * 5 + 1);
        EXPECT_GT(decision.at("actual_turnover").get<double>(), 0);
        EXPECT_LE(decision.at("actual_turnover").get<double>(), 0.2 + 1e-8);
        EXPECT_LE(std::abs(decision.at("postfee_net").get<double>()), 1e-8);
        EXPECT_GT(decision.at("postfee_gross").get<double>(), 0);
        EXPECT_LE(decision.at("postfee_gross").get<double>(), 1.0 + 1e-8);
        EXPECT_LE(decision.at("postfee_max_name").get<double>(), 0.01 + 1e-8);
    }
    const auto allocations = contents(directory / "report/allocations.csv");
    EXPECT_NE(allocations.find("pretrade_marked_dollars,posttrade_marked_dollars"), std::string::npos);
    EXPECT_NE(allocations.find(kId), std::string::npos);
    EXPECT_EQ(std::count(allocations.begin(), allocations.end(), '\n'), 5);
    EXPECT_EQ(json_file(directory / "report/summary.json")["accepted_allocation_count"], 2);
    EXPECT_FALSE(impl::run_equity_book(cfg).has_value());
    EXPECT_EQ(json_file(directory / "manifest.json")["book_id"].get<std::string>(), id);
    auto hashed_manifest = manifest;
    hashed_manifest.erase("book_id");
    const auto computed_id = atx::core::sha256_hex("atx-equity-book-v1\n" + hashed_manifest.dump());
    ASSERT_TRUE(computed_id);
    EXPECT_EQ(*computed_id, id);
    for (const auto &file : manifest.at("files")) {
        const auto sha = atx::core::sha256_file((directory / file.at("filename").get<std::string>()).string());
        ASSERT_TRUE(sha);
        EXPECT_EQ(*sha, file.at("sha256").get<std::string>());
    }
    cfg.out = (root / "book_repeat").string();
    const auto repeated = impl::run_equity_book(cfg);
    ASSERT_TRUE(repeated) << repeated.error().message();
    EXPECT_EQ(json_file(root / "book_repeat/manifest.json")["book_id"].get<std::string>(), id);
}

TEST_F(StageEquityBaseline, ConstrainedBookPreservesMissingHeldMarkFailureOnOriginalWindow) {
    auto baseline = input(true);
    ASSERT_TRUE(baseline.has_value());
    baseline->replay_delisting_policy = impl::ReplayDelistingPolicy::AbortV1;
    ASSERT_FALSE(impl::run_equity_baseline(*baseline).has_value());
    ASSERT_TRUE(fs::exists(root / "baseline/evaluation.bin.manifest.json"));
    ASSERT_TRUE(fs::exists(root / "baseline/combo.bin.manifest.json"));
    ASSERT_TRUE(fs::exists(root / "baseline/books.bin.manifest.json"));
    impl::RunConfig cfg;
    cfg.panel = baseline->panel;
    cfg.equity_baseline_dir = baseline->out;
    cfg.out = (root / "book_failed").string();
    cfg.report_aum = baseline->report_aum;
    const auto result = impl::run_equity_book(cfg);
    ASSERT_FALSE(result.has_value());
    EXPECT_NE(result.error().message().find("required close"), std::string::npos);
    EXPECT_NE(result.error().message().find(kId), std::string::npos);
    const auto directory = root / "book_failed";
    EXPECT_TRUE(fs::exists(directory / ".pending"));
    EXPECT_FALSE(fs::exists(directory / "manifest.json"));
    EXPECT_FALSE(fs::exists(directory / "report/manifest.json"));
    EXPECT_EQ(json_file(directory / "failure.json")["status"], "failed");
    const auto certificates = json_file(directory / "allocation_certificates.json");
    ASSERT_TRUE(certificates.at("decisions").is_array());
    EXPECT_EQ(certificates.at("decisions").size(), 1U);
    // The missing held mark fails before any second callback. Preserve the
    // earlier proposal context without inventing a failed candidate or callback.
    const auto &last = certificates.at("last_callback_attempt");
    EXPECT_EQ(last.at("status"), "certified-proposal-returned");
    EXPECT_EQ(last.at("decision_period"), 0);
    EXPECT_EQ(last.at("execution_period"), 1);
    EXPECT_TRUE(last.at("raw_candidate_captured").get<bool>());
    EXPECT_FALSE(json_file(directory / "failure.json").at("callback_failure_observed").get<bool>());
    // The failed original evidence remains present; no successful baseline is fabricated.
    EXPECT_TRUE(fs::exists(root / "baseline/failure.json"));
    EXPECT_FALSE(fs::exists(root / "baseline/manifest.json"));
}

TEST_F(StageEquityBaseline, ObservedCloseEntryConstraintBindsAvailabilityWithoutShrinkingUnion) {
    auto baseline = input(false, false, false, true);
    ASSERT_TRUE(baseline.has_value());
    baseline->replay_delisting_policy = impl::ReplayDelistingPolicy::AbortV1;
    // Strict fixed-weight replay cannot price the first entry, but its frozen
    // evaluation/preferences remain valid input to a separately identified book.
    const auto strict = impl::run_equity_baseline(*baseline);
    ASSERT_FALSE(strict.has_value());
    EXPECT_NE(strict.error().message().find("required close"), std::string::npos);
    ASSERT_TRUE(fs::exists(root / "baseline/books.bin.manifest.json"));
    impl::RunConfig cfg;
    cfg.panel = baseline->panel;
    cfg.equity_baseline_dir = baseline->out;
    cfg.out = (root / "observed_close_book").string();
    cfg.report_aum = baseline->report_aum;
    const auto result = impl::run_equity_book(cfg);
    ASSERT_TRUE(result.has_value()) << result.error().message();
    const auto directory = root / "observed_close_book";
    const auto manifest = json_file(directory / "manifest.json");
    const auto &recipe = manifest.at("recipe");
    EXPECT_EQ(recipe.at("profile"), "constrained-preference-weekly-observed-close-v3");
    const auto &policy = recipe.at("execution_availability");
    EXPECT_EQ(policy.at("model"), "ObservedCloseEntryConstraintV1");
    EXPECT_EQ(policy.at("available_by_order_submission"), "unverified");
    EXPECT_FALSE(policy.at("current_volume_used").get<bool>());
    EXPECT_FALSE(policy.at("future_observations_used").get<bool>());
    EXPECT_FALSE(policy.at("permanent_exclusion").get<bool>());
    EXPECT_EQ(recipe.at("required_zero_reason_bits").at("execution-close-unavailable"), 4);
    const auto report = json_file(directory / "report/manifest.json");
    EXPECT_EQ(report.at("recipe").at("allocation_policy"), recipe);

    const auto evidence = json_file(directory / "allocation_certificates.json");
    ASSERT_EQ(evidence.at("decisions").size(), 2U);
    const auto &first = evidence.at("decisions")[0];
    EXPECT_EQ(first.at("union_instruments"), 2);
    EXPECT_EQ(first.at("execution_unavailable_count"), 1);
    EXPECT_EQ(first.at("execution_fixed_zero_count"), 1);
    EXPECT_EQ(first.at("required_zero_count"), 1);
    EXPECT_EQ(first.at("fixed_zero_count"), 0);
    EXPECT_EQ(first.at("required_zero_reasons_canonical_order"), Json::array({4, 0}));
    const auto reason_hash = atx::core::sha256_hex("atx-equity-required-zero-reasons-v1\n" +
        first.at("required_zero_reasons_canonical_order").dump());
    ASSERT_TRUE(reason_hash);
    EXPECT_EQ(*reason_hash, first.at("required_zero_reasons_sha256").get<std::string>());
    EXPECT_EQ(first.at("proposed_intents_canonical_order")[0].at("action"), "close");
    EXPECT_DOUBLE_EQ(first.at("proposed_weights_canonical_order")[0].get<double>(), 0.0);
    EXPECT_NEAR(first.at("postfee_net").get<double>(), 0.0, 1e-8);
    // Net zero must be reoptimized with the equality. Clipping only the first
    // original target would leave an unwanted short in the second instrument.
    EXPECT_NEAR(first.at("postfee_gross").get<double>(), 0.0, 1e-8);
    const auto &availability = first.at("execution_availability");
    EXPECT_EQ(availability.at("held_count"), 0);
    EXPECT_TRUE(availability.at("held_current_marks_verified").get<bool>());
    ASSERT_EQ(availability.at("invalid_unheld_marks").size(), 1U);
    const auto &invalid = availability.at("invalid_unheld_marks")[0];
    EXPECT_EQ(invalid.at("instrument_index"), 0);
    EXPECT_EQ(invalid.at("security_id"), kId);
    EXPECT_EQ(invalid.at("reason"), "execution-close-unavailable");
    EXPECT_EQ(invalid.at("mark_classification"), "nan");
    EXPECT_TRUE(invalid.at("observed_close").is_null());
    const auto evaluation = impl::read_panel_artifact((root / "baseline/evaluation.bin").string());
    ASSERT_TRUE(evaluation);
    EXPECT_TRUE(evaluation->panel.in_universe(0, 0)); // Original decision eligibility survives.
    EXPECT_EQ(availability.at("source_artifact_id"), evaluation->artifact_id);
    const auto close_id = evaluation->panel.field_id("close");
    ASSERT_TRUE(close_id);
    const auto marked = evaluation->panel.field_cross_section(*close_id, 1);
    const Json observation{{"source_artifact_id", evaluation->artifact_id}, {"execution_period", 1},
        {"execution_session_key_ns", std::to_string(evaluation->identity.session_keys[1])},
        {"marks_canonical_order", Json::array({Json::array({"nan", nullptr}),
            Json::array({"finite-positive", marked[1]})})}};
    const auto mark_hash = atx::core::sha256_hex("atx-equity-observed-closes-v1\n" + observation.dump());
    ASSERT_TRUE(mark_hash);
    EXPECT_EQ(*mark_hash, availability.at("observed_closes_sha256").get<std::string>());
    for (const auto &file : manifest.at("files")) {
        const auto sha = atx::core::sha256_file(
            (directory / file.at("filename").get<std::string>()).string());
        ASSERT_TRUE(sha);
        EXPECT_EQ(*sha, file.at("sha256").get<std::string>());
    }
    auto identity = manifest;
    identity.erase("book_id");
    const auto book_hash = atx::core::sha256_hex("atx-equity-book-v1\n" + identity.dump());
    ASSERT_TRUE(book_hash);
    EXPECT_EQ(*book_hash, manifest.at("book_id").get<std::string>());
    EXPECT_TRUE(fs::exists(root / "baseline/failure.json")); // The strict attempt stays failed.
}

TEST_F(StageEquityBaseline, ConstrainedBookRejectsExplicitEvaluationWindowOverride) {
    const auto baseline = input();
    ASSERT_TRUE(baseline.has_value());
    const auto prepared = impl::run_equity_baseline(*baseline);
    ASSERT_TRUE(prepared.has_value()) << prepared.error().message();
    impl::RunConfig cfg;
    cfg.panel = baseline->panel;
    cfg.equity_baseline_dir = baseline->out;
    cfg.out = (root / "book_override").string();
    cfg.report_aum = baseline->report_aum;
    cfg.equity_evaluation_start = "2013-04-05";
    cfg.set_flags.insert("evaluation-start");
    EXPECT_FALSE(impl::run_equity_book(cfg).has_value());
    EXPECT_FALSE(fs::exists(root / "book_override/manifest.json"));
    EXPECT_FALSE(fs::exists(root / "book_override/report/manifest.json"));
}
} // namespace
