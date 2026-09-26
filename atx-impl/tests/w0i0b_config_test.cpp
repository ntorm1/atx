// W0-I0b config hygiene: boolean values are parsed (I-10), --config works in every
// stage or is rejected (I-10), every double flag must be finite (I-12), the
// short-interest lag default follows D0 (D-02), and the execution-delay guard
// (B-02 impl) holds at the config boundary.
#include <array>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

#include "config.hpp"
#include "w0i0b_support.hpp"

namespace atx_test_w0_i0b_config {
namespace fs = std::filesystem;
namespace impl = atx::impl;
using atx_test_w0_i0b_support::dispatch;
using atx_test_w0_i0b_support::parse;

fs::path scratch(const std::string &leaf) {
    const auto dir = fs::temp_directory_path() / "atx_w0i0b_config";
    fs::create_directories(dir);
    return dir / leaf;
}

fs::path write_config(const std::string &leaf, const std::string &text) {
    const auto path = scratch(leaf);
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << text;
    return path;
}

// --- I-10: boolean values are parsed -------------------------------------------

TEST(ImplConfigBool_Cli, ValuelessMeansTrueAndAFollowingLiteralIsTheValue) {
    const auto off = parse({"atx-impl", "optimize", "--metabook", "false", "--gated", "true",
                            "--compact-universe", "true", "--quiet"});
    ASSERT_TRUE(off.has_value()) << off.error().message();
    EXPECT_FALSE(off->metabook);
    EXPECT_TRUE(off->gated);
    EXPECT_TRUE(off->compact_universe);
    EXPECT_TRUE(off->quiet);
    EXPECT_TRUE(off->set_flags.contains("metabook")); // explicitly supplied, value false

    const auto on = parse({"atx-impl", "optimize", "--metabook", "--gross", "1.5"});
    ASSERT_TRUE(on.has_value()) << on.error().message();
    EXPECT_TRUE(on->metabook);
    EXPECT_DOUBLE_EQ(on->gross, 1.5);
}

TEST(ImplConfigBool_Cli, ANonLiteralTokenAfterABooleanIsNotSwallowed) {
    // Pre-W0 `--require-sector x` consumed "x" and set the flag; now a boolean only
    // consumes the literal true/false, so a stray token is an error, not a value.
    const auto stray = parse({"atx-impl", "panel", "--require-sector", "yes"});
    ASSERT_FALSE(stray.has_value());
    EXPECT_NE(stray.error().message().find("unexpected argument"), std::string::npos);
}

TEST(ImplConfigBool_Cli, CommittedRunbooksNumericBooleanLiteralsStillParse) {
    // Fix pass 1: scripts/canonical-acceptance-run.ps1:88 and
    // scripts/build-tradeable-alphas.ps1:51 pass `--require-sector 1 --compact-universe 1`.
    // `1` / `0` are boolean literals, so both runbooks keep parsing (and mean true).
    const auto canonical = parse({"atx-impl", "panel", "--segs", "segs", "--panel-out", "p.bin",
                                  "--min-price", "1.0", "--min-adv-usd", "25000000",
                                  "--adv-window", "20", "--top-n-by-adv", "0",
                                  "--require-sector", "1", "--compact-universe", "1"});
    ASSERT_TRUE(canonical.has_value()) << canonical.error().message();
    EXPECT_TRUE(canonical->require_sector);
    EXPECT_TRUE(canonical->compact_universe);
    EXPECT_EQ(canonical->top_n_by_adv, 0L);
    const auto dev = parse({"atx-impl", "panel", "--segs", "segs", "--panel-out", "dev.bin",
                            "--start", "2012-01-01", "--end", "2013-12-31", "--min-price", "1.0",
                            "--min-adv-usd", "25000000", "--adv-window", "20", "--top-n-by-adv",
                            "300", "--augment-panel", "--adv-windows", "5,10,20,60",
                            "--require-sector", "1", "--compact-universe", "1"});
    ASSERT_TRUE(dev.has_value()) << dev.error().message();
    EXPECT_TRUE(dev->augment_panel);
    EXPECT_TRUE(dev->require_sector);
    EXPECT_TRUE(dev->compact_universe);
    // `0` is false, and a numeric literal followed by another flag leaves it alone.
    const auto zero = parse({"atx-impl", "optimize", "--metabook", "0", "--exclude-no-sector",
                             "0", "--gated", "1", "--gross", "1.5"});
    ASSERT_TRUE(zero.has_value()) << zero.error().message();
    EXPECT_FALSE(zero->metabook);
    EXPECT_FALSE(zero->exclude_no_sector);
    EXPECT_TRUE(zero->gated);
    EXPECT_DOUBLE_EQ(zero->gross, 1.5);
    // Other numbers are not boolean literals: `2` is a stray token, not a value.
    const auto two = parse({"atx-impl", "panel", "--require-sector", "2"});
    ASSERT_FALSE(two.has_value());
    EXPECT_NE(two.error().message().find("unexpected argument"), std::string::npos);
    // The same literals are accepted from a config file.
    const auto path = write_config("numeric_bool.cfg", "require-sector=1\ncompact-universe=0\n");
    const auto file = impl::parse_config_file(path.string(), "panel");
    ASSERT_TRUE(file.has_value()) << file.error().message();
    EXPECT_TRUE(file->require_sector);
    EXPECT_FALSE(file->compact_universe);
    EXPECT_FALSE(impl::parse_bool_flag_value("metabook", "2").has_value());
    EXPECT_FALSE(impl::parse_bool_flag_value("metabook", "yes").has_value());
}

TEST(ImplConfigBool_File, FalseTurnsAFlagOffAndGarbageIsRejected) {
    const auto path = write_config("bool.cfg", "metabook=false\ngated=true\nconviction=\n"
                                               "group-neutralize=false\n");
    const auto parsed = impl::parse_config_file(path.string(), "optimize");
    ASSERT_TRUE(parsed.has_value()) << parsed.error().message();
    EXPECT_FALSE(parsed->metabook) << "I-10: metabook=false used to turn the stage ON";
    EXPECT_TRUE(parsed->gated);
    EXPECT_TRUE(parsed->conviction) << "a bare key keeps its historical meaning (true)";
    EXPECT_FALSE(parsed->group_neutralize);

    const auto bad = write_config("bool_bad.cfg", "metabook=yes\n");
    const auto rejected = impl::parse_config_file(bad.string(), "optimize");
    ASSERT_FALSE(rejected.has_value());
    EXPECT_NE(rejected.error().message().find("takes true or false"), std::string::npos);
}

TEST(ImplConfigBool_File, ACliFalseStillWinsTheMerge) {
    const auto path = write_config("merge.cfg", "metabook=true\n");
    auto cli = parse({"atx-impl", "optimize", "--metabook", "false"});
    ASSERT_TRUE(cli.has_value());
    ASSERT_TRUE(impl::merge_config_file(*cli, path.string()));
    EXPECT_FALSE(cli->metabook);
}

TEST(ImplConfigBool_Dispatch, ConfigIsReadByEveryMergingStage) {
    // An unknown key proves the file was READ: before W0 ten of these stages ignored
    // --config entirely, so the same invocation fell through to the stage.
    const auto path = write_config("unknown.cfg", "no-such-flag-w0i0b=1\n");
    for (const std::string sub : {"load", "panel", "discover", "combine", "optimize", "report",
                                  "run", "regime", "sweep", "metabook", "equity-baseline",
                                  "equity-book"}) {
        const auto outcome = dispatch({"atx-impl", sub, "--config", path.string()});
        EXPECT_EQ(outcome.code, 2) << sub << " did not read its --config file";
        EXPECT_NE(outcome.err.find("no-such-flag-w0i0b"), std::string::npos) << sub;
    }
}

TEST(ImplConfigBool_Dispatch, FrozenRecipeStagesRejectConfigOutright) {
    const auto path = write_config("frozen.cfg", "quiet=true\n");
    for (const std::string sub : {"equity-ic", "equity-universe"}) {
        const auto outcome = dispatch({"atx-impl", sub, "--config", path.string()});
        EXPECT_EQ(outcome.code, 2) << sub;
        EXPECT_NE(outcome.err.find("--config is not accepted"), std::string::npos) << sub;
    }
    EXPECT_TRUE(impl::subcommand_rejects_config("equity-ic"));
    EXPECT_FALSE(impl::subcommand_rejects_config("discover"));
}

TEST(ImplConfigBool_Dispatch, AMergedFileMeetsTheSameCrossFlagRules) {
    const auto delay0 = write_config("delay0.cfg", "replay-execution-delay=0\n");
    const auto refused = dispatch({"atx-impl", "report", "--config", delay0.string()});
    EXPECT_EQ(refused.code, 2);
    EXPECT_NE(refused.err.find("--allow-same-close"), std::string::npos) << refused.err;

    const auto nested = write_config("nested.cfg", "config=other.cfg\n");
    const auto nested_out = dispatch({"atx-impl", "report", "--config", nested.string()});
    EXPECT_EQ(nested_out.code, 2);
    EXPECT_NE(nested_out.err.find("cannot name another config file"), std::string::npos);
}

// --- I-12: every double flag is finite --------------------------------------------

constexpr std::array<std::string_view, 44> kDoubleFlags{
    "min-adv-usd", "min-adv", "min-price", "min-dsr", "min-split-sharpe", "max-pbo",
    "robust-holdout-frac", "reject-price-scale", "winsorize-limit", "gross-leverage",
    "min-sharpe", "min-fitness", "max-turnover", "max-pool-corr", "cost-bps-admit",
    "min-holding-days", "cost-max-turnover", "turnover-penalty-slope", "max-turnover-target",
    "min-viable-raw", "target-aum", "oos-fraction", "oos-embargo", "holdout-frac",
    "corr-penalty", "capacity-floor", "risk-aversion", "turnover-penalty", "gross", "name-cap",
    "trade-rate", "cost-bps", "gp-risk-aversion", "gp-trade-cost-scale", "selection-aum",
    "kelly-fraction", "kelly-max-gross", "report-aum", "min-dollar-adv", "replay-trade-bps",
    "replay-annual-borrow-bps", "book-turnover-gate", "participation-cap", "borrow-bps"};

TEST(ImplConfigFinite_Doubles, EveryDoubleFlagRejectsNanAndInfinityOnCliAndInFiles) {
    atx::usize rejected = 0;
    for (const auto flag : kDoubleFlags) {
        for (const std::string value : {"nan", "inf", "-inf", "NaN", "infinity"}) {
            const auto cli = parse({"atx-impl", "discover", "--" + std::string(flag), value});
            EXPECT_FALSE(cli.has_value()) << "--" << flag << " accepted " << value;
            const auto file = write_config("finite.cfg", std::string(flag) + "=" + value + "\n");
            EXPECT_FALSE(impl::parse_config_file(file.string(), "discover").has_value())
                << flag << "=" << value << " accepted from a file";
            rejected += cli.has_value() ? 0U : 1U;
        }
        // A finite value in range still parses (the guard is not a blanket refusal).
        const auto ok = parse({"atx-impl", "discover", "--" + std::string(flag), "0.5"});
        EXPECT_TRUE(ok.has_value()) << "--" << flag << " 0.5: "
                                    << (ok ? "" : ok.error().message());
    }
    EXPECT_EQ(rejected, kDoubleFlags.size() * 5U);
    std::printf("[ImplConfigFinite] %zu double flags x 5 non-finite spellings rejected: %zu\n",
                kDoubleFlags.size(), static_cast<std::size_t>(rejected));
}

TEST(ImplConfigFinite_Doubles, HoldoutFracNanNoLongerDisablesTheHoldout) {
    const auto nan = parse({"atx-impl", "combine", "--holdout-frac", "nan"});
    ASSERT_FALSE(nan.has_value());
    EXPECT_NE(nan.error().message().find("finite"), std::string::npos);
    const auto quarter = parse({"atx-impl", "combine", "--holdout-frac", "0.25"});
    ASSERT_TRUE(quarter.has_value());
    EXPECT_DOUBLE_EQ(quarter->combine_holdout_frac, 0.25);
}

// --- D-02: the si-publication-lag default follows D0 ------------------------------

TEST(ImplConfigFinite_SiLag, DefaultIsSevenNyseSessionsAndCalendarDaysStayReachable) {
    const auto defaults = parse({"atx-impl", "discover"});
    ASSERT_TRUE(defaults.has_value());
    EXPECT_EQ(defaults->si_publication_lag, 7);
    EXPECT_EQ(defaults->si_publication_lag_rule, "nyse-sessions-v2");
    const auto legacy = parse({"atx-impl", "discover", "--si-publication-lag", "2",
                               "--si-publication-lag-rule", "calendar-days-v1"});
    ASSERT_TRUE(legacy.has_value()) << legacy.error().message();
    EXPECT_EQ(legacy->si_publication_lag, 2);
    EXPECT_EQ(legacy->si_publication_lag_rule, "calendar-days-v1");
    EXPECT_FALSE(parse({"atx-impl", "discover", "--si-publication-lag", "-1"}).has_value());
    EXPECT_FALSE(
        parse({"atx-impl", "discover", "--si-publication-lag-rule", "weeks"}).has_value());
}

// --- B-02 (impl): the delay guard at the config boundary -------------------------

TEST(ImplDelayGuard_Config, ZeroDelayNeedsAllowSameCloseInAnyOrder) {
    const auto refused = parse({"atx-impl", "report", "--replay-execution-delay", "0"});
    ASSERT_FALSE(refused.has_value());
    EXPECT_NE(refused.error().message().find("--allow-same-close"), std::string::npos);
    const auto after = parse({"atx-impl", "report", "--replay-execution-delay", "0",
                              "--allow-same-close"});
    ASSERT_TRUE(after.has_value()) << after.error().message();
    EXPECT_EQ(after->replay_execution_delay, 0U);
    EXPECT_TRUE(after->allow_same_close);
    const auto before = parse({"atx-impl", "report", "--allow-same-close", "true",
                               "--replay-execution-delay", "0"});
    ASSERT_TRUE(before.has_value()) << before.error().message();
    const auto ic = parse({"atx-impl", "equity-ic", "--ic-execution-delay", "0"});
    ASSERT_FALSE(ic.has_value());
    EXPECT_NE(ic.error().message().find("--allow-same-close"), std::string::npos);
    const auto one = parse({"atx-impl", "report", "--replay-execution-delay", "1"});
    ASSERT_TRUE(one.has_value());
    EXPECT_FALSE(one->allow_same_close);
}

TEST(ImplDelayGuard_Config, AConfigFileOptInCountsForACliZeroDelay) {
    // Fix pass 1: with --config the cross-flag pass runs on the MERGED result, so the
    // opt-in may come from the file; without any opt-in the merge is still refused.
    const auto opt_in = write_config("same_close_opt_in.cfg", "allow-same-close=true\n");
    const auto parsed = parse({"atx-impl", "report", "--replay-execution-delay", "0",
                               "--config", opt_in.string()});
    ASSERT_TRUE(parsed.has_value()) << parsed.error().message();
    auto merged = *parsed;
    ASSERT_TRUE(impl::merge_config_file(merged, opt_in.string()));
    EXPECT_TRUE(merged.allow_same_close);
    EXPECT_TRUE(impl::validate_cross_flags(merged));
    const auto accepted = dispatch({"atx-impl", "report", "--replay-execution-delay", "0",
                                    "--config", opt_in.string()});
    EXPECT_EQ(accepted.err.find("--allow-same-close"), std::string::npos) << accepted.err;
    EXPECT_EQ(accepted.code, 1) << "parsed and merged; the report then fails on its inputs";

    const auto no_opt_in = write_config("same_close_none.cfg", "quiet=true\n");
    const auto refused = dispatch({"atx-impl", "report", "--replay-execution-delay", "0",
                                   "--config", no_opt_in.string()});
    EXPECT_EQ(refused.code, 2);
    EXPECT_NE(refused.err.find("--allow-same-close"), std::string::npos) << refused.err;
    // An explicit CLI false still beats a file's true (CLI wins the merge).
    const auto cli_false = dispatch({"atx-impl", "report", "--replay-execution-delay", "0",
                                     "--allow-same-close", "false", "--config", opt_in.string()});
    EXPECT_EQ(cli_false.code, 2);
    EXPECT_NE(cli_false.err.find("--allow-same-close"), std::string::npos) << cli_false.err;
}

TEST(ImplDelayGuard_Config, ProgrammaticValidationMatchesTheCli) {
    impl::RunConfig cfg;
    EXPECT_TRUE(impl::validate_execution_delay(cfg));
    cfg.replay_execution_delay = 0;
    EXPECT_FALSE(impl::validate_execution_delay(cfg));
    cfg.allow_same_close = true;
    EXPECT_TRUE(impl::validate_execution_delay(cfg));
    cfg.allow_same_close = false;
    cfg.replay_execution_delay = 1;
    cfg.equity_ic_execution_delay = 0;
    EXPECT_FALSE(impl::validate_cross_flags(cfg));
}

TEST(ImplConfigVwap, RawDefaultExplicitLegacyAndClosedRuleNames) {
    using atx::engine::alpha::VwapRule;
    const auto def = parse({"atx-impl", "panel"});
    ASSERT_TRUE(def);
    EXPECT_EQ(def->vwap_rule, VwapRule::RawDailyCloseV2);
    const auto old = parse({"atx-impl", "panel", "--vwap-rule", "adjusted-typical-v1"});
    ASSERT_TRUE(old);
    EXPECT_EQ(old->vwap_rule, VwapRule::AdjustedTypicalV1);
    EXPECT_FALSE(parse({"atx-impl", "panel", "--vwap-rule", "true-vwap"}));
}

} // namespace atx_test_w0_i0b_config
