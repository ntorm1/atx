// Checkpoint 14 / T6, design §9.4 item 1. This file asserts ONLY what the
// codebase actually does (ruling N-8): per-subcommand allowed-flag guards exist
// in exactly two places today — stage_equity_baseline.cpp:69 and
// stage_equity_book.cpp:100 — and `equity-ic` adopts the same discipline. The
// other ten subcommands have no per-flag guard, so they are asserted to be
// UNAFFECTED, never to reject anything: writing that assertion would produce a
// test that cannot pass and would silently reinterpret the requirement.
#include <array>
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

#include "atx/core/error.hpp"
#include "config.hpp"
#include "stage_equity_baseline.hpp"
#include "stage_equity_book.hpp"
#include "stage_equity_ic.hpp"

namespace {
namespace impl = atx::impl;
using atx::core::ErrorCode;

auto parse(std::vector<std::string> arguments) {
    std::vector<char *> argv;
    for (auto &argument : arguments) argv.push_back(argument.data());
    return impl::parse_args(static_cast<int>(argv.size()), argv.data());
}

// A config that clears resolve()'s value guards, so the only thing that can fail
// is the allowed-flag check under test. No path is ever opened.
impl::RunConfig ic_config() {
    impl::RunConfig cfg;
    cfg.subcommand = "equity-ic";
    cfg.panel = "context.bin";
    cfg.equity_baseline_dir = "baseline";
    cfg.out = "ic-out";
    cfg.equity_evaluation_start = "2013-04-04";
    cfg.equity_evaluation_end = "2014-01-01";
    return cfg;
}
} // namespace

TEST(ConfigEquityIc, DocumentedFlagsParseAndLeaveEveryOtherFieldUnset) {
    const auto parsed = parse({"atx-impl", "equity-ic", "--panel", "context.bin",
        "--baseline-dir", "training baseline", "--out", "ic", "--evaluation-start", "2013-04-04",
        "--evaluation-end", "2014-01-01", "--max-working-bytes", "3000000000",
        "--trial-ledger", "atx-engine/reviews/trial-ledger.jsonl"});
    ASSERT_TRUE(parsed) << parsed.error().message();
    EXPECT_EQ(parsed->subcommand, "equity-ic");
    EXPECT_EQ(parsed->panel, "context.bin");
    EXPECT_EQ(parsed->equity_baseline_dir, "training baseline");
    EXPECT_EQ(parsed->out, "ic");
    EXPECT_EQ(parsed->equity_evaluation_start, "2013-04-04");
    EXPECT_EQ(parsed->equity_evaluation_end, "2014-01-01");
    EXPECT_EQ(parsed->equity_trial_ledger, "atx-engine/reviews/trial-ledger.jsonl");
    EXPECT_EQ(parsed->equity_max_working_bytes, 3'000'000'000ULL);
    EXPECT_TRUE(parsed->set_flags.contains("trial-ledger"));
    EXPECT_FALSE(parsed->set_flags.contains("config"));
}

TEST(ConfigEquityIc, SubcommandIsAppendedLastSoNoExistingIndexMoves) {
    ASSERT_EQ(impl::kSubcommands.size(), 14U); EXPECT_EQ(impl::kSubcommands[12], "equity-ic");
    EXPECT_EQ(impl::kSubcommands.back(), "equity-universe"); // cp15 DR15-7: appended LAST
    const std::array<std::string_view, 12> before{"load", "panel", "discover", "combine",
        "optimize", "report", "run", "regime", "sweep", "metabook", "equity-baseline",
        "equity-book"};
    for (std::size_t i = 0; i < before.size(); ++i) EXPECT_EQ(impl::kSubcommands[i], before[i]);
}

TEST(ConfigEquityIc, MissingTrialLedgerValueCannotConsumeAnotherFlag) {
    EXPECT_FALSE(parse({"atx-impl", "equity-ic", "--trial-ledger"}));
    EXPECT_FALSE(parse({"atx-impl", "equity-ic", "--trial-ledger", ""}));
    EXPECT_FALSE(parse({"atx-impl", "equity-ic", "--trial-ledger", "--out", "ic"}));
}

TEST(ConfigEquityIc, StageRejectsEveryFlagOutsideItsAllowedSetIncludingConfig) {
    for (const std::string flag : {"config", "gross", "rebalance", "report-aum",
             "replay-trade-bps", "combo", "books", "method"}) {
        auto cfg = ic_config();
        cfg.set_flags = {flag};
        const auto result = impl::run_equity_ic(cfg);
        ASSERT_FALSE(result) << "equity-ic accepted --" << flag;
        EXPECT_EQ(result.error().code(), ErrorCode::InvalidArgument);
        EXPECT_NE(result.error().message().find("unsupported flag --" + flag), std::string::npos);
    }
}

TEST(ConfigEquityIc, StageRejectsAProgrammaticConfigFileEvenWithoutTheFlag) {
    auto cfg = ic_config();
    cfg.config_file = "book.cfg";
    const auto result = impl::run_equity_ic(cfg);
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code(), ErrorCode::InvalidArgument);
}

TEST(ConfigEquityIc, BothEvaluationDatesAreRequired) {
    auto missing_start = ic_config();
    missing_start.equity_evaluation_start.clear();
    EXPECT_FALSE(impl::run_equity_ic(missing_start));
    auto missing_end = ic_config();
    missing_end.equity_evaluation_end.clear();
    EXPECT_FALSE(impl::run_equity_ic(missing_end));
    auto missing_baseline = ic_config();
    missing_baseline.equity_baseline_dir.clear();
    EXPECT_FALSE(impl::run_equity_ic(missing_baseline));
}

TEST(ConfigEquityIc, EvaluationWindowOutsideTrainingIsRejectedLikeTheBaselineStage) {
    auto sealed = ic_config();
    sealed.equity_evaluation_start = "2023-01-03";
    sealed.equity_evaluation_end = "2023-12-30";
    const auto rejected = impl::run_equity_ic(sealed);
    ASSERT_FALSE(rejected);
    EXPECT_EQ(rejected.error().code(), ErrorCode::InvalidArgument);
    // The same boundary the baseline stage rejects (stage_equity_baseline.cpp:99-100).
    impl::RunConfig baseline;
    baseline.subcommand = "equity-baseline";
    baseline.panel = "context.bin";
    baseline.out = "baseline-out";
    baseline.equity_evaluation_start = "2023-01-03";
    baseline.equity_evaluation_end = "2023-12-30";
    EXPECT_FALSE(impl::run_equity_baseline(baseline));
    auto early = ic_config();
    early.equity_evaluation_start = "2013-03-01";
    EXPECT_FALSE(impl::run_equity_ic(early));
}

TEST(ConfigEquityIc, ExistingGuardedStagesStillRejectTrialLedger) {
    impl::RunConfig baseline;
    baseline.subcommand = "equity-baseline";
    baseline.panel = "context.bin";
    baseline.out = "baseline-out";
    baseline.equity_evaluation_start = "2013-04-04";
    baseline.equity_evaluation_end = "2014-01-01";
    baseline.set_flags = {"trial-ledger"};
    const auto rejected_baseline = impl::run_equity_baseline(baseline);
    ASSERT_FALSE(rejected_baseline);
    EXPECT_NE(rejected_baseline.error().message().find("unsupported flag --trial-ledger"),
              std::string::npos);

    impl::RunConfig book;
    book.subcommand = "equity-book";
    book.panel = "context.bin";
    book.equity_baseline_dir = "baseline";
    book.out = "book-out";
    book.set_flags = {"trial-ledger"};
    const auto rejected_book = impl::run_equity_book(book);
    ASSERT_FALSE(rejected_book);
    EXPECT_NE(rejected_book.error().message().find("unsupported flag --trial-ledger"),
              std::string::npos);
}

TEST(ConfigEquityIc, TheOtherTenSubcommandsParseUnchangedAndGainNoNewField) {
    for (const std::string sub : {"load", "panel", "discover", "combine", "optimize", "report",
             "run", "regime", "sweep", "metabook"}) {
        const auto parsed = parse({"atx-impl", sub, "--panel", "p.bin", "--out", "o"});
        ASSERT_TRUE(parsed) << sub << ": " << parsed.error().message();
        EXPECT_EQ(parsed->subcommand, sub);
        EXPECT_EQ(parsed->panel, "p.bin");
        EXPECT_EQ(parsed->out, "o");
        // The one new RunConfig field stays unset for every pre-existing caller.
        EXPECT_TRUE(parsed->equity_trial_ledger.empty());
        EXPECT_FALSE(parsed->set_flags.contains("trial-ledger"));
    }
    // equity-baseline / equity-book parse exactly as config_baseline_test.cpp and
    // config_equity_book_test.cpp already pin them; only the new field is checked here.
    const auto baseline = parse({"atx-impl", "equity-baseline", "--panel", "c.bin", "--out", "b",
        "--evaluation-start", "2013-04-04", "--evaluation-end", "2014-01-01"});
    ASSERT_TRUE(baseline) << baseline.error().message();
    EXPECT_TRUE(baseline->equity_trial_ledger.empty());
    const auto book = parse({"atx-impl", "equity-book", "--panel", "c.bin", "--baseline-dir", "b",
        "--out", "k"});
    ASSERT_TRUE(book) << book.error().message();
    EXPECT_TRUE(book->equity_trial_ledger.empty());
}
