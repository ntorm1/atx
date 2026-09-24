#include <atomic>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "config.hpp"
#include "dispatch.hpp"

namespace {
namespace fs = std::filesystem;
namespace impl = atx::impl;

auto parse_book(std::vector<std::string> arguments) {
    std::vector<char *> argv;
    for (auto &argument : arguments) argv.push_back(argument.data());
    return impl::parse_args(static_cast<int>(argv.size()), argv.data());
}

class ConfigEquityBook : public ::testing::Test {
protected:
    fs::path root;
    bool owns_root{false};

    void SetUp() override {
        static std::atomic<unsigned> sequence{};
        for (unsigned attempt = 0; attempt < 1000; ++attempt) {
            root = fs::temp_directory_path() /
                ("atx_config_equity_book_" + std::to_string(sequence.fetch_add(1)));
            if (fs::create_directory(root)) {
                owns_root = true;
                return;
            }
        }
        FAIL() << "Cannot reserve fixture directory";
    }

    void TearDown() override {
        if (!owns_root) return;
        std::error_code ec;
        const auto parent = fs::weakly_canonical(fs::temp_directory_path(), ec);
        if (ec) return;
        const auto resolved = fs::weakly_canonical(root, ec);
        if (!ec && resolved.parent_path() == parent &&
            resolved.filename().string().starts_with("atx_config_equity_book_"))
            fs::remove_all(resolved, ec);
    }
};
} // namespace

TEST_F(ConfigEquityBook, FrozenBaselineReferenceLeavesInheritanceOverridesUnset) {
    const auto parsed = parse_book({"atx-impl", "equity-book", "--panel", "context.bin",
        "--baseline-dir", "training baseline", "--out", "constrained-book"});
    ASSERT_TRUE(parsed) << parsed.error().message();
    EXPECT_EQ(parsed->subcommand, "equity-book");
    EXPECT_EQ(parsed->panel, "context.bin");
    EXPECT_EQ(parsed->equity_baseline_dir, "training baseline");
    EXPECT_EQ(parsed->out, "constrained-book");
    EXPECT_TRUE(parsed->equity_evaluation_start.empty());
    EXPECT_TRUE(parsed->equity_evaluation_end.empty());
    EXPECT_TRUE(parsed->set_flags.contains("baseline-dir"));
    EXPECT_FALSE(parsed->set_flags.contains("report-aum"));
    EXPECT_FALSE(parsed->set_flags.contains("replay-trade-bps"));
    EXPECT_FALSE(parsed->set_flags.contains("replay-annual-borrow-bps"));
}

TEST_F(ConfigEquityBook, MissingBaselineDirectoryValueCannotConsumeAnotherFlag) {
    EXPECT_FALSE(parse_book({"atx-impl", "equity-book", "--baseline-dir"}));
    EXPECT_FALSE(parse_book({"atx-impl", "equity-book", "--baseline-dir", ""}));
    EXPECT_FALSE(parse_book({"atx-impl", "equity-book", "--baseline-dir", "--out", "book"}));
}

TEST_F(ConfigEquityBook, ConfigRoundTripPreservesExplicitZeroOverridesAndDispatchMerges) {
    const auto path = root / "book.cfg";
    {
        std::ofstream file(path);
        ASSERT_TRUE(file);
        file << "panel=context.bin\nbaseline-dir=training baseline\nout=book\n"
                "evaluation-start=2013-04-04\nevaluation-end=2014-01-01\n"
                "report-aum=100000000\nreplay-trade-bps=5\nreplay-annual-borrow-bps=365\n";
    }
    const auto from_file = impl::parse_config_file(path.string(), "equity-book");
    ASSERT_TRUE(from_file) << from_file.error().message();
    EXPECT_EQ(from_file->equity_baseline_dir, "training baseline");
    EXPECT_TRUE(from_file->set_flags.contains("baseline-dir"));
    auto from_cli = parse_book({"atx-impl", "equity-book", "--baseline-dir", "CLI baseline",
        "--config", path.string(), "--replay-trade-bps", "0", "--replay-annual-borrow-bps", "0"});
    ASSERT_TRUE(from_cli) << from_cli.error().message();
    const auto merged = impl::merge_config_file(*from_cli, path.string());
    ASSERT_TRUE(merged) << merged.error().message();
    EXPECT_EQ(from_cli->equity_baseline_dir, "CLI baseline");
    EXPECT_EQ(from_cli->equity_evaluation_start, "2013-04-04");
    EXPECT_EQ(from_cli->equity_evaluation_end, "2014-01-01");
    EXPECT_DOUBLE_EQ(from_cli->report_aum, 100000000.0);
    EXPECT_DOUBLE_EQ(from_cli->replay_trade_bps, 0.0);
    EXPECT_DOUBLE_EQ(from_cli->replay_annual_borrow_bps, 0.0);
    EXPECT_TRUE(from_cli->set_flags.contains("replay-trade-bps"));

    const auto invalid_path = root / "invalid.cfg";
    {
        std::ofstream file(invalid_path);
        ASSERT_TRUE(file);
        file << "unknown-equity-book-test=1\n";
    }
    std::vector<std::string> arguments{"atx-impl", "equity-book", "--config", invalid_path.string()};
    std::vector<char *> argv;
    for (auto &argument : arguments) argv.push_back(argument.data());
    std::ostringstream out, error;
    EXPECT_EQ(impl::dispatch(static_cast<int>(argv.size()), argv.data(), out, error), 2);
    EXPECT_NE(error.str().find("unknown flag: --unknown-equity-book-test"), std::string::npos);
}
