#include <chrono>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string>
#include <vector>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "atx/core/sha256.hpp"
#include "atx/engine/data/orats_history.hpp"
#include "config.hpp"
#include "orats_fixture.hpp"
#include "panel_artifact.hpp"
#include "stage_data_provenance.hpp"
#include "stages.hpp"

namespace {
namespace fs = std::filesystem;
using Json = nlohmann::json;
using namespace atx::impl;

class AtxImplDataProvenance : public ::testing::Test {
protected:
    fs::path dir;
    fs::path zip;
    fs::path prep;
    RunConfig load;

    void SetUp() override {
        dir = fs::temp_directory_path() /
              (std::string("atx_data_provenance_") +
               ::testing::UnitTest::GetInstance()->current_test_info()->name() + "_" +
               std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        ASSERT_TRUE(fs::create_directory(dir));
        zip = dir / "accepted.zip";
        prep = dir / "preparation.json";
        std::string body = std::string(atx_impl_test::kHeader) + "\n";
        body += atx_impl_test::make_orats_row(
            "2020-01-02", "9007199254740993", "OLD", "FUTURE", 1.0, 2.0, 1000);
        body += atx_impl_test::make_orats_row(
            "2020-01-02", "42", "SECOND", "FUTURE", 1.0, 1.0, 2000);
        body += atx_impl_test::make_orats_row(
            "2020-01-03", "9007199254740993", "NEW", "FUTURE", 1.0, 2.0, 1000);
        ASSERT_NO_FATAL_FAILURE(atx_impl_test::write_orats_zip(body, zip.string()));
        load.zip = zip.string();
        load.out = (dir / "segments").string();
        load.min_date = "2020-01-01";
        load.preparation_manifest = prep.string();
        const auto hash = atx::core::sha256_file(zip.string());
        ASSERT_TRUE(hash.has_value());
        const Json doc{
            {"status", "complete"}, {"policy_version", "tickerhistory-qa-v1"},
            {"accepted", {{"filename", "accepted.zip"}, {"sha256", *hash},
                           {"size_bytes", fs::file_size(zip)},
                           {"rows_preserved_byte_for_byte", true}}},
            {"source", {{"sha256", std::string(64, 'a')}, {"member", "history.txt"},
                         {"crc_verified", true}}},
            {"window", {{"start_inclusive", "2020-01-02"},
                         {"end_inclusive", "2020-01-03"}}},
            {"counts", {{"source_rows", 10}, {"outside_window_rows", 6},
                         {"selected_rows", 4}, {"accepted_rows", 3}, {"rejected_rows", 1}}}};
        write_json(prep, doc);
    }

    void TearDown() override {
        std::error_code ec;
        const auto parent = fs::weakly_canonical(fs::temp_directory_path(), ec);
        if (ec) return;
        const auto resolved = fs::weakly_canonical(dir, ec);
        if (!ec && resolved.parent_path() == parent &&
            resolved.filename().string().starts_with("atx_data_provenance_")) {
            fs::remove_all(resolved, ec);
        }
    }

    void write_json(const fs::path &path, const Json &value) {
        std::ofstream stream(path, std::ios::binary | std::ios::trunc);
        stream << value.dump(2) << '\n';
        stream.close();
        ASSERT_TRUE(stream.good());
    }

    Json read_json(const fs::path &path) {
        std::ifstream stream(path, std::ios::binary);
        return Json::parse(stream);
    }

    RunConfig panel_config(const char *filename = "panel.bin") const {
        RunConfig cfg;
        cfg.segs = load.out;
        cfg.panel_out = (dir / filename).string();
        cfg.preparation_manifest = prep.string();
        cfg.start = "2020-01-01";
        cfg.end = "2020-01-04";
        return cfg;
    }
};

TEST_F(AtxImplDataProvenance, BoundLoadAndAugmentedPanelPreserveExactAxes) {
    const auto input_before = atx::core::sha256_file(zip.string());
    ASSERT_TRUE(input_before.has_value());
    const auto loaded = run_load(load);
    ASSERT_TRUE(loaded.has_value()) << loaded.error().message();
    const auto receipt = read_json(fs::path(load.out) / "_ingestion.manifest.json");
    EXPECT_EQ(receipt["input"]["sha256"], *input_before);
    EXPECT_EQ(receipt["segments"].size(), 2U);
    EXPECT_FALSE(fs::exists(fs::path(load.out) / "_ingestion.pending"));

    auto cfg = panel_config();
    cfg.augment_panel = true;
    cfg.adv_windows = {2};
    const auto built = run_panel(cfg);
    ASSERT_TRUE(built.has_value()) << built.error().message();
    const auto panel = read_panel_artifact(cfg.panel_out);
    ASSERT_TRUE(panel.has_value()) << panel.error().message();
    EXPECT_EQ(panel->identity.instrument_ids,
              (std::vector<std::string>{"9007199254740993", "42"}));
    EXPECT_EQ(panel->identity.original_instrument_indices, (std::vector<atx::usize>{0, 1}));
    EXPECT_EQ(panel->identity.session_keys,
              (std::vector<atx::i64>{1577923200000000000LL, 1578009600000000000LL}));
    EXPECT_TRUE(panel->panel.field_id("adv2").has_value());
    EXPECT_TRUE(require_panel_parent(panel->identity, "ingestion_input_zip", *input_before));
    EXPECT_TRUE(require_panel_parent(panel->identity, "preparation_declared_original_zip",
                                     std::string(64, 'a')));
    const auto recipe = Json::parse(panel->identity.recipe);
    EXPECT_EQ(recipe["universe"]["adv_window_bars"], 21);
    EXPECT_EQ(recipe["augmentation"]["adv_windows"], Json::array({2}));
    EXPECT_EQ(recipe["augmentation"]["dollar_volume_basis"], "raw_close*raw_volume");
    EXPECT_EQ(recipe["augmentation"]["vwap_rule"], "raw-daily-close-v2");
    EXPECT_EQ(recipe["augmentation"]["vwap_basis"], "raw");
    EXPECT_EQ(recipe["augmentation"]["vwap_kind"], "daily-close-price-proxy-not-intraday-vwap");
    EXPECT_EQ(recipe["historical_availability"], "unknown-archive-snapshot");
    EXPECT_EQ(recipe["historical_vintages_verified"], false);
    EXPECT_EQ(recipe["preparation_content_binding_verified"], true);

    cfg.panel_out = (dir / "repeat.bin").string();
    const auto repeated = run_panel(cfg);
    ASSERT_TRUE(repeated.has_value()) << repeated.error().message();
    const auto other = read_panel_artifact(cfg.panel_out);
    ASSERT_TRUE(other.has_value());
    EXPECT_EQ(other->artifact_id, panel->artifact_id);
    cfg.panel_out = (dir / "legacy.bin").string();
    cfg.vwap_rule = atx::engine::alpha::VwapRule::AdjustedTypicalV1;
    ASSERT_TRUE(run_panel(cfg));
    const auto legacy = read_panel_artifact(cfg.panel_out);
    ASSERT_TRUE(legacy);
    EXPECT_NE(legacy->artifact_id, panel->artifact_id);
    const auto legacy_recipe = Json::parse(legacy->identity.recipe);
    EXPECT_EQ(legacy_recipe["augmentation"]["vwap_rule"], "adjusted-typical-v1");
    EXPECT_EQ(legacy_recipe["augmentation"]["vwap_basis"], "adjusted_level");
    const auto input_after = atx::core::sha256_file(zip.string());
    ASSERT_TRUE(input_after.has_value());
    EXPECT_EQ(*input_after, *input_before);
}

TEST_F(AtxImplDataProvenance, RejectsIncompleteWrongInputAndInconsistentPreparation) {
    const auto original = read_json(prep);
    std::vector<Json> invalid(5, original);
    invalid[0]["status"] = "failed";
    invalid[1]["accepted"]["sha256"] = std::string(64, 'b');
    invalid[2]["accepted"]["filename"] = "different.zip";
    invalid[3]["counts"]["accepted_rows"] = 5;
    invalid[4]["window"]["end_inclusive"] = "2020-02-31";
    for (const auto &doc : invalid) {
        write_json(prep, doc);
        const auto result = begin_ingestion_provenance(zip.string(), load.out, prep.string());
        EXPECT_FALSE(result.has_value());
        EXPECT_FALSE(fs::exists(load.out));
    }
}

TEST_F(AtxImplDataProvenance, ChangedInputLeavesUnpublishedReceiptAndBlocksReuse) {
    const auto input = begin_ingestion_provenance(zip.string(), load.out, prep.string());
    ASSERT_TRUE(input.has_value()) << input.error().message();
    {
        std::ofstream stream(zip, std::ios::binary | std::ios::app);
        stream << 'x';
    }
    const auto result = finish_ingestion_provenance(*input, zip.string(), load.out,
                                                    prep.string(), "recipe", 0, 3);
    EXPECT_FALSE(result.has_value());
    EXPECT_FALSE(fs::exists(fs::path(load.out) / "_ingestion.manifest.json"));
    EXPECT_TRUE(fs::exists(fs::path(load.out) / "_ingestion.pending"));
    EXPECT_FALSE(begin_ingestion_provenance(zip.string(), load.out, "").has_value());
}

TEST_F(AtxImplDataProvenance, DuplicateKeysAndExcessiveDepthFailClosed) {
    std::string duplicate = read_json(prep).dump();
    duplicate.insert(1, "\"status\":\"failed\","); // A last-key-wins parser would accept it.
    const std::string nested = "{\"extra\":" + std::string(70, '[') + "0" +
                               std::string(70, ']') + "}";
    for (const auto &bytes : {duplicate, nested}) {
        std::ofstream stream(prep, std::ios::binary | std::ios::trunc);
        stream << bytes;
        stream.close();
        const auto result = begin_ingestion_provenance(zip.string(), load.out, prep.string());
        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error().code(), atx::core::ErrorCode::ParseError);
        EXPECT_FALSE(fs::exists(load.out));
    }
}

TEST_F(AtxImplDataProvenance, PreparationAllowsOmittedZeroCounterEntries) {
    auto doc = read_json(prep);
    doc["counts"] = {{"source_rows", 3}, {"selected_rows", 3}, {"accepted_rows", 3}};
    write_json(prep, doc);
    const auto loaded = run_load(load);
    ASSERT_TRUE(loaded.has_value()) << loaded.error().message();
    EXPECT_TRUE(fs::exists(fs::path(load.out) / "_ingestion.manifest.json"));
}

TEST_F(AtxImplDataProvenance, RejectsTamperedSelectedSegmentEvenWithValidReceipt) {
    const auto loaded = run_load(load);
    ASSERT_TRUE(loaded.has_value()) << loaded.error().message();
    const auto before = snapshot_panel_sources(load.out, 0, std::numeric_limits<atx::i64>::max());
    ASSERT_TRUE(before.has_value());
    std::vector<std::string> paths;
    for (const auto &file : *before) paths.push_back((fs::path(load.out) / file.filename).string());
    {
        std::ofstream stream(paths.front(), std::ios::binary | std::ios::app);
        stream << 'x';
    }
    EXPECT_FALSE(validate_panel_sources(load.out, *before, paths, prep.string()).has_value());
    // Even a fresh snapshot after tampering cannot inherit the original receipt's parents.
    auto changed = *before;
    const auto hash = atx::core::sha256_file(paths.front());
    ASSERT_TRUE(hash.has_value());
    changed.front().sha256 = *hash;
    changed.front().size_bytes = fs::file_size(paths.front());
    EXPECT_FALSE(validate_panel_sources(load.out, changed, paths, prep.string()).has_value());
}

TEST_F(AtxImplDataProvenance, MissingReceiptAllowsUnknownSourceButNeverUnlinkedPreparation) {
    const auto loaded = run_load(load);
    ASSERT_TRUE(loaded.has_value());
    fs::remove(fs::path(load.out) / "_ingestion.manifest.json");
    auto cfg = panel_config();
    EXPECT_FALSE(run_panel(cfg).has_value());
    EXPECT_FALSE(fs::exists(cfg.panel_out + ".manifest.json"));
    cfg.preparation_manifest.clear();
    const auto built = run_panel(cfg);
    ASSERT_TRUE(built.has_value()) << built.error().message();
    const auto artifact = read_panel_artifact(cfg.panel_out);
    ASSERT_TRUE(artifact.has_value());
    const auto recipe = Json::parse(artifact->identity.recipe);
    EXPECT_EQ(recipe["ingestion_content_binding_verified"], false);
    EXPECT_EQ(recipe["original_source_binding"], "unknown");
}

TEST_F(AtxImplDataProvenance, SelectedWindowBindsOnlyParticipatingSegments) {
    const auto loaded = run_load(load);
    ASSERT_TRUE(loaded.has_value());
    auto cfg = panel_config();
    cfg.start = "2020-01-03";
    const auto built = run_panel(cfg);
    ASSERT_TRUE(built.has_value()) << built.error().message();
    const auto artifact = read_panel_artifact(cfg.panel_out);
    ASSERT_TRUE(artifact.has_value());
    EXPECT_EQ(artifact->identity.instrument_ids,
              (std::vector<std::string>{"9007199254740993"}));
    atx::usize segments = 0;
    for (const auto &parent : artifact->identity.parents) {
        if (parent.role.starts_with("segment:")) {
            ++segments;
            EXPECT_EQ(parent.role, "segment:2020-01-03.seg");
        }
    }
    EXPECT_EQ(segments, 1U);
}

TEST_F(AtxImplDataProvenance, IncompleteIngestionNeverBecomesUnknownSource) {
    const auto loaded = run_load(load);
    ASSERT_TRUE(loaded.has_value());
    fs::remove(fs::path(load.out) / "_ingestion.manifest.json");
    fs::create_directory(fs::path(load.out) / "_ingestion.pending");
    auto cfg = panel_config();
    cfg.preparation_manifest.clear();
    EXPECT_FALSE(run_panel(cfg).has_value());
    EXPECT_FALSE(fs::exists(cfg.panel_out + ".manifest.json"));
}

TEST_F(AtxImplDataProvenance, InvalidConfigurationAndIncrementalFailBeforePublication) {
    auto cfg = panel_config();
    cfg.incremental_panel = true;
    const auto incremental = run_panel(cfg);
    ASSERT_FALSE(incremental.has_value());
    EXPECT_EQ(incremental.error().code(), atx::core::ErrorCode::NotImplemented);
    cfg.incremental_panel = false;
    cfg.min_adv_usd = std::numeric_limits<double>::infinity();
    EXPECT_FALSE(run_panel(cfg).has_value());
    cfg.min_adv_usd = 0.0;
    cfg.augment_panel = true;
    cfg.adv_window = 65536;
    EXPECT_FALSE(run_panel(cfg).has_value());
    EXPECT_FALSE(fs::exists(cfg.panel_out));
}

TEST_F(AtxImplDataProvenance, QaV2TruthfulModifiedRowsLoadAndBindWhileFalseClaimsFail) {
    auto rescued = atx_impl_test::make_orats_row("2016-01-15", "42", "SYN", "SYN", 11.0, 1.0, 1000);
    // Drop only the three synthetic OHL cells (indices 5..7), retaining every other byte.
    std::size_t begin = 0;
    for (int field = 0; field < 5; ++field) begin = rescued.find('\t', begin) + 1;
    auto end = begin;
    for (int field = 0; field < 3; ++field) end = rescued.find('\t', end) + 1;
    rescued.replace(begin, end - begin, "\t\t\t");
    ASSERT_NO_FATAL_FAILURE(atx_impl_test::write_orats_zip(std::string(atx_impl_test::kHeader) + "\n" + rescued, zip.string()));
    const auto sha = atx::core::sha256_file(zip.string());
    ASSERT_TRUE(sha);
    auto doc = read_json(prep);
    doc["policy_version"] = "tickerhistory-qa-v2";
    doc["qa_version"] = "v2";
    doc["window"] = {{"start_inclusive", "2016-01-15"}, {"end_inclusive", "2016-01-15"}};
    doc["counts"] = {{"source_rows", 1}, {"selected_rows", 1}, {"accepted_rows", 1}, {"qa_v2_rescued_rows", 1}};
    doc["accepted"] = {{"filename", "accepted.zip"}, {"sha256", *sha}, {"size_bytes", fs::file_size(zip)},
        {"rows_preserved_byte_for_byte", false}, {"rows_modified_qa_v2", 1}, {"unmodified_rows", 0},
        {"unmodified_rows_preserved_byte_for_byte", true}};
    doc["qa_v2_allowlist_sha256"] = "0589dc9ae5c96e68d183820f4733ade7df94245d805e285e43e1bdf29c0fef60";
    doc["qa_v2_blanked_fields"] = {"open", "high", "low"};
    doc["qa_v2_rescuable_reasons"] = {"ohlc_order_violation"};
    doc["qa_v2_dates"] = {"2016-01-15"};
    doc["qa_v2_daily_counts"] = {{"2016-01-15", {{"accepted_v1", 0}, {"accepted_v2_rescued", 1}, {"accepted", 1}}}};
    std::vector<Json> bad(4, doc);
    bad[0]["accepted"]["rows_preserved_byte_for_byte"] = true;
    bad[1]["qa_v2_allowlist_sha256"] = std::string(64, 'b');
    bad[2]["qa_v2_blanked_fields"] = {"close"};
    bad[3]["qa_v2_daily_counts"]["2016-01-15"]["accepted_v2_rescued"] = 0;
    for (const auto& invalid : bad) {
        write_json(prep, invalid);
        EXPECT_FALSE(begin_ingestion_provenance(zip.string(), load.out, prep.string()));
        EXPECT_FALSE(fs::exists(load.out));
    }
    write_json(prep, doc);
    load.min_date = "2016-01-01";
    const auto loaded = run_load(load);
    ASSERT_TRUE(loaded) << loaded.error().message();
    const auto receipt = read_json(fs::path(load.out) / "_ingestion.manifest.json");
    EXPECT_EQ(receipt["preparation"]["policy_version"], "tickerhistory-qa-v2");
    EXPECT_EQ(receipt["preparation"]["accepted_sha256"], *sha);
}

} // namespace
