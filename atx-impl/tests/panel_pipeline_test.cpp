#include <atomic>
#include <filesystem>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "config.hpp"
#include "panel_pipeline.hpp"
#include "serialize_panel.hpp"
#include "stages.hpp"
#include "atx/core/sha256.hpp"

namespace {
namespace fs = std::filesystem;
namespace impl = atx::impl;
using atx::engine::alpha::Panel;

class PanelPipeline : public ::testing::Test {
protected:
    fs::path root;

    void SetUp() override {
        static std::atomic<unsigned> sequence{};
        for (unsigned attempt = 0; attempt < 1000; ++attempt) {
            root = fs::temp_directory_path() /
                ("atx_panel_pipeline_" + std::to_string(sequence.fetch_add(1)));
            if (fs::create_directory(root)) return;
        }
        FAIL() << "Cannot reserve fixture directory";
    }

    void TearDown() override {
        std::error_code ec;
        const auto parent = fs::weakly_canonical(fs::temp_directory_path(), ec);
        const auto resolved = fs::weakly_canonical(root, ec);
        if (!ec && resolved.parent_path() == parent &&
            resolved.filename().string().starts_with("atx_panel_pipeline_")) {
            fs::remove_all(resolved, ec);
        }
    }

    std::string path(const std::string& name) const { return (root / name).string(); }

    static atx::core::Result<Panel> research_panel() {
        std::vector<double> close(36);
        for (atx::usize d = 0; d < 12; ++d) {
            for (atx::usize i = 0; i < 3; ++i) {
                close[d * 3 + i] = 100.0 + static_cast<double>(d * (i + 1));
            }
        }
        return Panel::create(12, 3, {"close", "raw_close", "volume"},
            {close, close, std::vector<double>(36, 1'000'000.0)},
            std::vector<std::uint8_t>(36, 1));
    }

    static impl::PanelIdentity identity() {
        impl::PanelIdentity id;
        id.instrument_namespace = "synthetic.securityID";
        for (atx::i64 d = 0; d < 12; ++d) {
            id.session_keys.push_back(1'777'593'600'000'000'000LL + d * 86'400'000'000'000LL);
        }
        id.instrument_ids = {"1", "2", "1001001001070"};
        id.original_instrument_indices = {2, 5, 9};
        id.recipe = "synthetic fixture; availability unknown";
        id.parents = {{"fixture", std::string(64, 'a')}};
        return id;
    }

    atx::core::Status write_inputs(bool swap_ids = false, bool wrong_parent = false) {
        ATX_TRY(auto panel, research_panel());
        auto id = identity();
        ATX_TRY(auto receipt, impl::write_panel_artifact(panel, path("research.bin"), id));
        std::vector<double> alpha(36);
        for (atx::usize d = 0; d < 12; ++d) {
            alpha[d * 3] = 0.5;
            alpha[d * 3 + 1] = -0.5;
        }
        ATX_TRY(auto combo, Panel::create(12, 3, {"alpha"}, {alpha},
                                         std::vector<std::uint8_t>(36, 1)));
        if (swap_ids) std::swap(id.instrument_ids[0], id.instrument_ids[1]);
        id.parents = {{"research", wrong_parent ? std::string(64, 'b') : receipt.artifact_id}};
        id.recipe = "synthetic target weights";
        ATX_TRY(auto written, impl::write_panel_artifact(combo, path("combo.bin"), id));
        (void)written;
        return atx::core::Ok();
    }

    impl::RunConfig optimize_config() const {
        impl::RunConfig cfg;
        cfg.panel = path("research.bin");
        cfg.combo = path("combo.bin");
        cfg.books_out = path("books.bin");
        cfg.position_mode = true;
        cfg.rebalance = "weekly";
        cfg.gross = 1.0;
        cfg.name_cap = 0.5;
        return cfg;
    }
};

TEST_F(PanelPipeline, MissingManifestRequiresExplicitLegacyDiagnosticMode) {
    ASSERT_FALSE(impl::RunConfig{}.allow_unidentified_panels);
    auto panel = research_panel();
    ASSERT_TRUE(panel.has_value());
    ASSERT_TRUE(impl::write_panel(*panel, path("legacy.bin")).has_value());
    EXPECT_FALSE(impl::read_pipeline_panel(path("legacy.bin"), false).has_value());
    auto legacy = impl::read_pipeline_panel(path("legacy.bin"), true);
    ASSERT_TRUE(legacy.has_value());
    EXPECT_FALSE(legacy->identity.has_value());
    char program[] = "atx-impl";
    char command[] = "report";
    char flag[] = "--allow-unidentified-panels";
    char invalid_value[] = "yes";
    char valid_value[] = "false";
    char* invalid[] = {program, command, flag, invalid_value};
    EXPECT_FALSE(impl::parse_args(4, invalid).has_value());
    char* valid[] = {program, command, flag, valid_value};
    auto config = impl::parse_args(4, valid);
    ASSERT_TRUE(config.has_value());
    EXPECT_FALSE(config->allow_unidentified_panels);
}

TEST_F(PanelPipeline, OptimizationRejectsEqualShapeWithSwappedSecurityIdentities) {
    ASSERT_TRUE(write_inputs(true).has_value());
    auto result = impl::run_optimize(optimize_config());
    EXPECT_FALSE(result.has_value());
    EXPECT_FALSE(fs::exists(path("books.bin")));
}

TEST_F(PanelPipeline, OptimizationRejectsSameAxesWithWrongResearchParent) {
    ASSERT_TRUE(write_inputs(false, true).has_value());
    EXPECT_FALSE(impl::run_optimize(optimize_config()).has_value());
}

TEST_F(PanelPipeline, OptimizationRejectsEqualShapeWithShiftedSessionDates) {
    ASSERT_TRUE(write_inputs().has_value());
    auto combo = impl::read_panel_artifact(path("combo.bin"));
    ASSERT_TRUE(combo.has_value());
    for (auto& key : combo->identity.session_keys) ++key;
    ASSERT_TRUE(impl::write_panel_artifact(combo->panel, path("shifted.bin"),
                                          combo->identity).has_value());
    auto cfg = optimize_config();
    cfg.combo = path("shifted.bin");
    EXPECT_FALSE(impl::run_optimize(cfg).has_value());
}

TEST_F(PanelPipeline, MixedKnownAndUnknownInputsNeverJoin) {
    ASSERT_TRUE(write_inputs().has_value());
    auto combo = impl::read_panel(path("combo.bin"));
    ASSERT_TRUE(combo.has_value());
    ASSERT_TRUE(impl::write_panel(*combo, path("legacy_combo.bin")).has_value());
    auto cfg = optimize_config();
    cfg.combo = path("legacy_combo.bin");
    cfg.allow_unidentified_panels = true;
    EXPECT_FALSE(impl::run_optimize(cfg).has_value());
}

TEST_F(PanelPipeline, WeeklyBooksCarryExactDatesAndReportRejectsScheduleMutation) {
    ASSERT_TRUE(write_inputs().has_value());
    auto cfg = optimize_config();
    auto optimized = impl::run_optimize(cfg);
    ASSERT_TRUE(optimized.has_value()) << optimized.error().message();
    auto books = impl::read_panel_artifact(path("books.bin"));
    ASSERT_TRUE(books.has_value());
    const auto expected = identity();
    EXPECT_EQ(books->identity.instrument_ids, expected.instrument_ids);
    EXPECT_EQ(books->identity.session_keys,
        (std::vector<atx::i64>{expected.session_keys[0], expected.session_keys[5],
                               expected.session_keys[10]}));
    auto research_input = impl::read_pipeline_panel(cfg.panel, false);
    auto books_input = impl::read_pipeline_panel(cfg.books_out, false);
    ASSERT_TRUE(research_input.has_value());
    ASSERT_TRUE(books_input.has_value());
    std::swap(books_input->identity->original_instrument_indices[0],
              books_input->identity->original_instrument_indices[1]);
    const std::vector<atx::usize> periods{0, 5, 10};
    EXPECT_FALSE(impl::require_book_schedule(*books_input, *research_input, periods).has_value());
    impl::RunConfig report;
    report.panel = cfg.panel;
    report.books = cfg.books_out;
    report.report_out = path("report");
    // W0-I0b / I-11: report costs are mandatory; frictionless is an explicit choice here.
    report.set_flags = {"replay-trade-bps", "replay-annual-borrow-bps"};
    auto result = impl::run_report(report);
    ASSERT_TRUE(result.has_value()) << result.error().message();
    std::ofstream corrupt(path("books.bin.meta.txt"), std::ios::app);
    corrupt << "s=3 period=0 turnover=0 cost_bps=0\n";
    corrupt.close();
    report.report_out = path("rejected-report");
    EXPECT_FALSE(impl::run_report(report).has_value());
}

TEST_F(PanelPipeline, StageReservationPreservesPublishedCompanionBytes) {
    ASSERT_TRUE(write_inputs().has_value());
    const auto cfg = optimize_config();
    ASSERT_TRUE(impl::run_optimize(cfg).has_value());
    auto before = atx::core::sha256_file(path("books.bin.meta.txt"));
    ASSERT_TRUE(before.has_value());
    EXPECT_FALSE(impl::run_optimize(cfg).has_value());
    auto after = atx::core::sha256_file(path("books.bin.meta.txt"));
    ASSERT_TRUE(after.has_value());
    EXPECT_EQ(*before, *after);
    auto guard = impl::reserve_pipeline_output(path("new.bin"), true);
    ASSERT_TRUE(guard.has_value());
    EXPECT_FALSE(impl::reserve_pipeline_output(path("new.bin"), true).has_value());
    guard->reset();
    std::ofstream partial(path("new.bin.partial"));
    partial << "interrupted payload";
    partial.close();
    EXPECT_FALSE(impl::reserve_pipeline_output(path("new.bin"), true).has_value());
    EXPECT_FALSE(impl::reserve_pipeline_output(path("new.bin"), false).has_value());
}

TEST_F(PanelPipeline, OversizedScheduleIntegerReturnsErrorWithoutThrowing) {
    auto research = research_panel();
    ASSERT_TRUE(research.has_value());
    ASSERT_TRUE(impl::write_panel(*research, path("research.bin")).has_value());
    auto books = Panel::create(1, 3, {"weight"}, {{0.5, -0.5, 0.0}},
                               std::vector<std::uint8_t>(3, 1));
    ASSERT_TRUE(books.has_value());
    ASSERT_TRUE(impl::write_panel(*books, path("books.bin")).has_value());
    std::ofstream meta(path("books.bin.meta.txt"));
    meta << "periods=1\ninstruments=3\ns=0 period=" << std::string(100, '9')
         << " turnover=1 cost_bps=0\n";
    meta.close();
    impl::RunConfig cfg;
    cfg.allow_unidentified_panels = true;
    cfg.panel = path("research.bin");
    cfg.books = path("books.bin");
    cfg.report_out = path("invalid-report");
    EXPECT_NO_THROW({
        const auto result = impl::run_report(cfg);
        EXPECT_FALSE(result.has_value());
    });
}

} // namespace
