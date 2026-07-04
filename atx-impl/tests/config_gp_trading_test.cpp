// config_gp_trading_test.cpp — p9 S3-0: CLI/config-file surface for GP-trading.
//
// Every field defaults to its INERT value, so an optimize invocation with NONE
// of these flags asserted is byte-identical to pre-S3 (proven separately by the
// off-path byte-identity test in stage_optimize_gp_trading_test.cpp). This file
// proves only the PARSE layer: each flag reaches its field, negatives are
// rejected, and a config-file value round-trips.
#include <fstream>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "config.hpp"

using atx::impl::parse_args;
using atx::impl::parse_config_file;
using atx::impl::merge_config_file;
using atx::impl::RunConfig;

namespace {
atx::core::Result<RunConfig> parse(std::vector<std::string> args) {
    std::vector<char*> argv;
    argv.reserve(args.size());
    for (auto& a : args) argv.push_back(a.data());
    return parse_args(static_cast<int>(argv.size()), argv.data());
}
} // namespace

TEST(ConfigParse, GpTradingFlags_RoundTrip) {
    auto r = parse({"atx-impl", "optimize",
                    "--gp-trading",
                    "--gp-risk-aversion", "2.5",
                    "--gp-trade-cost-scale", "0.1"});
    ASSERT_TRUE(r.has_value()) << (r.has_value() ? "" : r.error().message());
    const RunConfig& cfg = *r;
    EXPECT_TRUE(cfg.gp_trading);
    EXPECT_DOUBLE_EQ(cfg.gp_risk_aversion, 2.5);
    EXPECT_DOUBLE_EQ(cfg.gp_trade_cost_scale, 0.1);
}

TEST(ConfigParse, GpTradingFlags_OmittedAreInert) {
    auto r = parse({"atx-impl", "optimize"});
    ASSERT_TRUE(r.has_value());
    const RunConfig& cfg = *r;
    EXPECT_FALSE(cfg.gp_trading);
    EXPECT_DOUBLE_EQ(cfg.gp_risk_aversion, 0.0);
    EXPECT_DOUBLE_EQ(cfg.gp_trade_cost_scale, 0.0);
}

TEST(ConfigParse, GpRiskAversionRejectsNegative) {
    auto r = parse({"atx-impl", "optimize", "--gp-risk-aversion", "-1.0"});
    EXPECT_FALSE(r.has_value());
}

TEST(ConfigParse, GpTradeCostScaleRejectsNegative) {
    auto r = parse({"atx-impl", "optimize", "--gp-trade-cost-scale", "-0.5"});
    EXPECT_FALSE(r.has_value());
}

TEST(ConfigFile, GpTradingFlags_RoundTrip) {
    const std::string path = "atx_s3_0_gp_trading_flags_test.cfg";
    {
        std::ofstream f(path);
        f << "gp-trading=\n"
             "gp-risk-aversion=1.5\n"
             "gp-trade-cost-scale=0.2\n";
    }
    auto file_r = parse_config_file(path, "optimize");
    ASSERT_TRUE(file_r.has_value()) << file_r.error().message();
    EXPECT_TRUE(file_r->gp_trading);
    EXPECT_DOUBLE_EQ(file_r->gp_risk_aversion, 1.5);
    EXPECT_DOUBLE_EQ(file_r->gp_trade_cost_scale, 0.2);
    std::remove(path.c_str());
}
