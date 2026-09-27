#include <atomic>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <utility>

#include <gtest/gtest.h>

#include "atx/engine/eval/trial_epoch.hpp"

namespace {
namespace fs = std::filesystem;
namespace eval = atx::engine::eval;
const std::string genesis(64, '0');

class TrialEpoch : public ::testing::Test {
protected:
    fs::path root;
    void SetUp() override {
        static std::atomic<unsigned> sequence{};
        for (unsigned i = 0; i < 1000; ++i) {
            const auto candidate = fs::temp_directory_path() /
                ("atx_trial_epoch_" + std::to_string(sequence.fetch_add(1)));
            if (fs::create_directory(candidate)) { root = candidate; return; }
        }
        FAIL() << "cannot reserve unique fixture directory";
    }
    void TearDown() override {
        std::error_code ec;
        const auto path = fs::weakly_canonical(root, ec);
        const auto parent = fs::weakly_canonical(fs::temp_directory_path(), ec);
        if (!ec && !root.empty() && path.parent_path() == parent &&
            path.filename().string().starts_with("atx_trial_epoch_")) fs::remove_all(path, ec);
    }
    std::string file() const { return (root / "epoch.bin").string(); }
    eval::TrialEpochAttempt attempt(char token = 'a') const {
        eval::TrialEpochAttempt a;
        a.token = std::string(64, token);
        a.trial_id = std::string("trial-") + token;
        a.prereg_sha256 = std::string(64, 'e');
        a.numerical_recipe_json = "{\"delay\":1,\"input\":\"synthetic\"}";
        eval::TrialEpochCell c;
        c.canonical_dsl = "close";
        c.horizon = 5;
        c.stream_signal_index = 3;
        c.forward_variant = "DropMissingForward";
        c.restriction = "full";
        c.family_sha256 = std::string(64, 'f');
        c.display_alias = "original";
        a.cells.push_back(std::move(c));
        return a;
    }
};
} // namespace

TEST_F(TrialEpoch, DurablePendingReservationExactTokenAndTerminalAreIdempotent) {
    std::string anchor;
    {
        auto catalog = eval::TrialEpochCatalog::open(file(), "E2", genesis);
        ASSERT_TRUE(catalog) << catalog.error().message();
        auto first = catalog->reserve_attempt(attempt());
        ASSERT_TRUE(first) << first.error().message();
        EXPECT_TRUE(first->inserted);
        EXPECT_EQ(first->counts.incomplete, 1U);
        EXPECT_EQ(first->new_unique_cells, 1U);
        anchor = catalog->head_sha256();
        const auto bytes = fs::file_size(file());
        auto repeat = catalog->reserve_attempt(attempt());
        ASSERT_TRUE(repeat) << repeat.error().message();
        EXPECT_FALSE(repeat->inserted);
        EXPECT_EQ(fs::file_size(file()), bytes);
        auto changed = attempt();
        changed.cells[0].sign = -1;
        EXPECT_FALSE(catalog->reserve_attempt(changed));
        EXPECT_EQ(catalog->head_sha256(), anchor);
    }
    auto reopened = eval::TrialEpochCatalog::open(file(), "E2", anchor);
    ASSERT_TRUE(reopened) << reopened.error().message();
    EXPECT_EQ(reopened->counts().incomplete, 1U); // process loss does not erase trials
    ASSERT_TRUE(reopened->finish_attempt(attempt().token, eval::TrialEpochTerminal::Failed,
                                         std::string(64, 'd')));
    EXPECT_EQ(reopened->counts().failed, 1U);
    EXPECT_EQ(reopened->counts().incomplete, 0U);
    const auto bytes = fs::file_size(file());
    EXPECT_TRUE(reopened->finish_attempt(attempt().token, eval::TrialEpochTerminal::Failed,
                                         std::string(64, 'd')));
    EXPECT_EQ(fs::file_size(file()), bytes);
    EXPECT_FALSE(reopened->finish_attempt(attempt().token, eval::TrialEpochTerminal::Completed,
                                          std::string(64, 'd')));
    EXPECT_FALSE(eval::TrialEpochCatalog::open(file(), "E2", anchor)); // stale external anchor
}

TEST_F(TrialEpoch, AliasesDeduplicateButRetainedReferencesMustProveEveryExactCell) {
    auto catalog = eval::TrialEpochCatalog::open(file(), "E2", genesis);
    ASSERT_TRUE(catalog);
    const auto first = attempt();
    ASSERT_TRUE(catalog->reserve_attempt(first));
    auto retained = attempt('b');
    retained.prereg_sha256 = std::string(64, 'c');
    retained.cells[0].display_alias = "renamed";
    retained.cells[0].family_sha256 = std::string(64, 'd');
    retained.cells[0].retained = eval::TrialEpochLineage{
        first.prereg_sha256, first.cells[0].family_sha256, first.trial_id};
    auto bad = retained;
    bad.cells[0].horizon = 21;
    EXPECT_FALSE(catalog->reserve_attempt(bad));
    bad = retained;
    bad.cells[0].retained->prereg_sha256 = std::string(64, '1');
    EXPECT_FALSE(catalog->reserve_attempt(bad));
    bad = retained;
    bad.cells[0].retained->family_sha256 = std::string(64, '2');
    EXPECT_FALSE(catalog->reserve_attempt(bad));
    bad = retained;
    bad.cells[0].retained->trial_id = "unknown";
    EXPECT_FALSE(catalog->reserve_attempt(bad));
    auto accepted = catalog->reserve_attempt(retained);
    ASSERT_TRUE(accepted) << accepted.error().message();
    EXPECT_EQ(accepted->new_unique_cells, 0U);
    EXPECT_EQ(accepted->verified_retained_cells, 1U);
    EXPECT_EQ(accepted->counts.unique_cells, 1U);
    EXPECT_EQ(accepted->counts.declared_cells, 2U);
    EXPECT_EQ(accepted->counts.attempts, 2U);
    auto repeat_new = attempt('c');
    repeat_new.cells[0].display_alias = "another_alias";
    auto repeated = catalog->reserve_attempt(repeat_new);
    ASSERT_TRUE(repeated);
    EXPECT_EQ(repeated->new_unique_cells, 0U); // declaration of 'new' cannot inflate unique N
    EXPECT_EQ(repeated->verified_retained_cells, 0U);
}

TEST_F(TrialEpoch, NumericalRecipeSignStreamAndHorizonChangesAreDistinctTrials) {
    auto catalog = eval::TrialEpochCatalog::open(file(), "E2", genesis);
    ASSERT_TRUE(catalog);
    ASSERT_TRUE(catalog->reserve_attempt(attempt()));
    for (unsigned mutation = 0; mutation < 5; ++mutation) {
        auto a = attempt(static_cast<char>('b' + mutation));
        if (mutation == 0) a.numerical_recipe_json = "{\"delay\":2,\"input\":\"synthetic\"}";
        if (mutation == 1) a.cells[0].sign = -1;
        if (mutation == 2) a.cells[0].horizon = 21;
        if (mutation == 3) a.cells[0].stream_signal_index = 4;
        if (mutation == 4) a.cells[0].stream_horizon_index = 1;
        auto reserved = catalog->reserve_attempt(a);
        ASSERT_TRUE(reserved) << reserved.error().message();
        EXPECT_EQ(reserved->new_unique_cells, 1U);
    }
    EXPECT_EQ(catalog->counts().unique_cells, 6U);
}

TEST_F(TrialEpoch, TornTailAndChangedAnchorsNeverRepairOrEraseAcknowledgedDeclarations) {
    std::string anchor;
    {
        auto catalog = eval::TrialEpochCatalog::open(file(), "E2", genesis);
        ASSERT_TRUE(catalog);
        ASSERT_TRUE(catalog->reserve_attempt(attempt()));
        anchor = catalog->head_sha256();
    }
    EXPECT_FALSE(eval::TrialEpochCatalog::open(file(), "wrong_epoch", anchor));
    EXPECT_FALSE(eval::TrialEpochCatalog::open(file(), "E2", genesis));
    { std::ofstream out(file(), std::ios::binary | std::ios::app); out << "ATXE201\n00000025partial"; }
    const auto damaged_bytes = fs::file_size(file());
    EXPECT_FALSE(eval::TrialEpochCatalog::open(file(), "E2", anchor));
    EXPECT_EQ(fs::file_size(file()), damaged_bytes);
}

TEST_F(TrialEpoch, BoundsUnknownRecipesAndDuplicateCellsRefuseBeforeReservation) {
    eval::TrialEpochLimits limits;
    limits.max_attempts = 1;
    auto catalog = eval::TrialEpochCatalog::open(file(), "E2", genesis, limits);
    ASSERT_TRUE(catalog);
    auto duplicate = attempt();
    duplicate.cells.push_back(duplicate.cells[0]);
    EXPECT_FALSE(catalog->reserve_attempt(duplicate));
    EXPECT_EQ(fs::file_size(file()), 0U);
    auto bad_json = attempt();
    bad_json.numerical_recipe_json = "{\"delay\":1,\"delay\":2}";
    EXPECT_FALSE(catalog->reserve_attempt(bad_json));
    auto bad = attempt();
    bad.cells[0].forward_variant = "unknown";
    EXPECT_FALSE(catalog->reserve_attempt(bad));
    bad = attempt();
    bad.cells[0].canonical_dsl.assign(4097, 'x');
    EXPECT_FALSE(catalog->reserve_attempt(bad));
    ASSERT_TRUE(catalog->reserve_attempt(attempt()));
    EXPECT_FALSE(catalog->reserve_attempt(attempt('b')));
    EXPECT_EQ(catalog->counts().attempts, 1U);
    limits.max_attempts = 4097;
    EXPECT_FALSE(eval::TrialEpochCatalog::open(file(), "E2", catalog->head_sha256(), limits));
}
