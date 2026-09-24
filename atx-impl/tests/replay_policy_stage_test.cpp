#include <algorithm>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "atx/core/sha256.hpp"
#include "panel_artifact.hpp"
#include "replay_report.hpp"
#include "serialize_panel.hpp"

namespace {
namespace fs = std::filesystem;
namespace impl = atx::impl;
using atx::core::Err;
using atx::core::ErrorCode;
using atx::core::Ok;
using atx::core::Result;
using atx::engine::alpha::Panel;
constexpr atx::i64 kDay = 86'400'000'000'000LL;

struct PolicyInputs {
    impl::RunConfig config;
    impl::ReplayReportPolicy policy;
};

class ReplayPolicyStage : public ::testing::Test {
protected:
    fs::path root;

    void SetUp() override {
        static std::atomic<unsigned> sequence{};
        for (unsigned attempt = 0; attempt < 1000; ++attempt) {
            root = fs::temp_directory_path() /
                ("atx_replay_policy_stage_" + std::to_string(sequence.fetch_add(1)));
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
            resolved.filename().string().starts_with("atx_replay_policy_stage_")) {
            fs::remove_all(resolved, ec);
        }
    }

    Result<PolicyInputs> inputs(const std::string &name, double scale, double planning_cost = 0) {
        const auto directory = root / name;
        if (!fs::create_directory(directory)) return Err(ErrorCode::IoError, "fixture directory");
        const auto path = [&](const char *file) { return (directory / file).string(); };
        impl::PanelIdentity identity;
        identity.session_keys = {0, kDay, 2 * kDay};
        identity.instrument_namespace = "synthetic.securityID";
        identity.instrument_ids = {"17"};
        identity.original_instrument_indices = {9};
        identity.recipe = "bounded policy wrapper fixture";
        identity.parents = {{"fixture", std::string(64, 'a')}};
        const std::vector<double> close{100 * scale, 120 * scale, 120 * scale};
        ATX_TRY(auto panel, Panel::create(3, 1, {"close", "raw_close", "volume"},
            {close, close, {1000, 1000, 1000}}, {1, 1, 1}));
        ATX_TRY(auto research, impl::write_panel_artifact(panel, path("research.bin"), identity));
        {
            std::ofstream meta(path("combo.bin.meta"), std::ios::binary);
            meta << "n_periods=3\nfit_begin=0\nfit_end=0\nholdout_begin=0\n";
        }
        ATX_TRY(auto fit_hash, atx::core::sha256_file(path("combo.bin.meta")));
        identity.parents = {{"research", research.artifact_id}, {"fit-boundary", fit_hash}};
        ATX_TRY(auto combo_panel, Panel::create(3, 1, {"alpha"}, {{0.5, 0.5, 0.0}}, {1, 1, 1}));
        ATX_TRY(auto combo, impl::write_panel_artifact(combo_panel, path("combo.bin"), identity));
        {
            std::ofstream meta(path("books.bin.meta.txt"), std::ios::binary);
            meta << "periods=2\ninstruments=1\ns=0 period=0 turnover=0 cost_bps="
                 << planning_cost << "\ns=1 period=2 turnover=0 cost_bps=0\n";
        }
        ATX_TRY(auto schedule_hash, atx::core::sha256_file(path("books.bin.meta.txt")));
        identity.session_keys = {0, 2 * kDay};
        identity.parents = {{"research", research.artifact_id}, {"combo", combo.artifact_id},
                            {"book-schedule", schedule_hash}};
        ATX_TRY(auto book_panel, Panel::create(2, 1, {"weight"}, {{0.5, 0.0}}, {1, 1}));
        ATX_TRY(auto books, impl::write_panel_artifact(book_panel, path("books.bin"), identity));
        PolicyInputs result;
        result.config.panel = path("research.bin");
        result.config.books = path("books.bin");
        result.config.combo = path("combo.bin");
        result.config.report_out = path("report");
        result.config.report_aum = 100;
        result.config.replay_execution_delay = 0;
        result.policy.callback = [](const atx::engine::book::ReplayAllocationState &state) {
            return Ok(std::vector<double>(state.preference_weights.begin(),
                                           state.preference_weights.end()));
        };
        result.policy.recipe = "{\"model\":\"identity\"}";
        result.policy.expected_research_artifact_id = research.artifact_id;
        result.policy.expected_books_artifact_id = books.artifact_id;
        result.policy.expected_combo_artifact_id = combo.artifact_id;
        result.policy.max_input_payload_bytes = static_cast<atx::u64>(std::max({
            fs::file_size(result.config.panel), fs::file_size(result.config.books),
            fs::file_size(result.config.combo)}));
        return Ok(std::move(result));
    }
};

TEST_F(ReplayPolicyStage, BoundedIdentifiedWrapperRetainsScheduleAndExplicitFeeValidation) {
    auto fixture = inputs("admitted", 1.0, 5.0);
    ASSERT_TRUE(fixture.has_value()) << fixture.error().message();
    auto &cfg = fixture->config;
    auto &policy = fixture->policy;
    atx::usize calls = 0;
    policy.callback = [&](const atx::engine::book::ReplayAllocationState &state) {
        ++calls;
        return Ok(std::vector<double>(state.preference_weights.begin(),
                                       state.preference_weights.end()));
    };
    for (const auto cap : {atx::u64{0}, policy.max_input_payload_bytes - 1}) {
        auto invalid = policy;
        invalid.max_input_payload_bytes = cap;
        EXPECT_FALSE(impl::run_policy_replay_report(cfg, invalid).has_value());
        EXPECT_FALSE(fs::exists(cfg.report_out));
    }
    auto invalid = policy;
    invalid.expected_research_artifact_id.clear();
    EXPECT_FALSE(impl::run_policy_replay_report(cfg, invalid).has_value());
    invalid = policy;
    invalid.expected_books_artifact_id = std::string(64, 'A');
    EXPECT_FALSE(impl::run_policy_replay_report(cfg, invalid).has_value());
    invalid = policy;
    invalid.expected_combo_artifact_id = "invalid";
    EXPECT_FALSE(impl::run_policy_replay_report(cfg, invalid).has_value());
    auto no_combo = cfg;
    no_combo.combo.clear();
    EXPECT_FALSE(impl::run_policy_replay_report(no_combo, policy).has_value());

    auto research = impl::read_panel(cfg.panel);
    ASSERT_TRUE(research.has_value());
    const auto legacy_path = (root / "legacy.bin").string();
    ASSERT_TRUE(impl::write_panel(*research, legacy_path).has_value());
    auto legacy = cfg;
    legacy.panel = legacy_path;
    legacy.allow_unidentified_panels = true;
    EXPECT_FALSE(impl::run_policy_replay_report(legacy, policy).has_value());
    EXPECT_EQ(calls, 0U);
    EXPECT_FALSE(fs::exists(cfg.report_out));

    const auto implicit_fee = impl::run_policy_replay_report(cfg, policy);
    ASSERT_FALSE(implicit_fee.has_value());
    EXPECT_NE(implicit_fee.error().message().find("choose --replay-trade-bps explicitly"),
              std::string::npos);
    EXPECT_EQ(calls, 0U);
    EXPECT_FALSE(fs::exists(cfg.report_out));
    cfg.set_flags.insert("replay-trade-bps");
    const auto accepted = impl::run_policy_replay_report(cfg, policy);
    ASSERT_TRUE(accepted.has_value()) << accepted.error().message();
    EXPECT_EQ(calls, 1U);
    EXPECT_TRUE(fs::exists(fs::path(cfg.report_out) / "allocations.csv"));
    EXPECT_TRUE(fs::exists(fs::path(cfg.report_out) / "manifest.json"));
}

TEST_F(ReplayPolicyStage, CoherentReplacementGraphMustMatchEveryAdmittedArtifactIdentity) {
    const auto admitted = inputs("admitted", 1.0);
    const auto replacement = inputs("replacement", 2.0);
    ASSERT_TRUE(admitted.has_value());
    ASSERT_TRUE(replacement.has_value());
    const auto &cfg = replacement->config;
    auto policy = admitted->policy;
    atx::usize calls = 0;
    policy.callback = [&](const atx::engine::book::ReplayAllocationState &state) {
        ++calls;
        return Ok(std::vector<double>(state.preference_weights.begin(),
                                       state.preference_weights.end()));
    };
    const auto assert_rejected_at = [&](const std::string &path) {
        const auto result = impl::run_policy_replay_report(cfg, policy);
        ASSERT_FALSE(result.has_value());
        EXPECT_NE(result.error().message().find("differs from admitted identity: " + path),
                  std::string::npos);
        EXPECT_EQ(calls, 0U);
        EXPECT_FALSE(fs::exists(cfg.report_out));
    };
    assert_rejected_at(cfg.panel);
    policy.expected_research_artifact_id = replacement->policy.expected_research_artifact_id;
    assert_rejected_at(cfg.books);
    policy.expected_books_artifact_id = replacement->policy.expected_books_artifact_id;
    assert_rejected_at(cfg.combo);
    policy.expected_combo_artifact_id = replacement->policy.expected_combo_artifact_id;
    const auto accepted = impl::run_policy_replay_report(cfg, policy);
    ASSERT_TRUE(accepted.has_value()) << accepted.error().message();
    EXPECT_EQ(calls, 1U);
}

} // namespace
