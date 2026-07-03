// config_megabook_flags_test.cpp — p8 Sprint 5 (S5-0): thread the full S1-S4 +
// p7-carry-forward CLI flag surface through the shared hub (config.hpp/config.cpp).
//
// Every new RunConfig field defaults to its INERT value, so a run/discover/combine
// invocation with NONE of these flags asserted is byte-identical to pre-S5 (proven
// separately by the pinned AtxImplDiscover / FactoryOos / NsgaSearch goldens — this
// file only proves the PARSE layer: each flag reaches its field, and a config-file
// value is overridden by an explicitly-supplied CLI flag (the set_flags merge).

#include <fstream>
#include <string>

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

// ---------------------------------------------------------------------------
// ConfigParse.MegaBookFlags_RoundTrip — each new flag parses to its field.
// ---------------------------------------------------------------------------
TEST(ConfigParse, MegaBookFlags_RoundTrip) {
    auto r = parse({"atx-impl", "discover",
                    "--risk-model", "factor",
                    "--dead-alpha-factors",
                    "--group-neutralize",
                    "--metabook",
                    "--sleeve-method", "hrp",
                    "--impact-in-selection",
                    "--selection-aum", "5e7",
                    "--capacity-curve",
                    "--require-split-stable",
                    "--blocking-pbo",
                    "--short-interest", "si.csv",
                    "--augment-out", "aug.bin",
                    "--si-publication-lag", "3",
                    "--kelly-fraction", "0.5",
                    "--kelly-max-gross", "2.0",
                    "--incremental-panel"});
    ASSERT_TRUE(r.has_value()) << (r.has_value() ? "" : r.error().message());
    const RunConfig& cfg = *r;
    EXPECT_EQ(cfg.risk_model, "factor");
    EXPECT_TRUE(cfg.dead_alpha_factors);
    EXPECT_TRUE(cfg.group_neutralize);
    EXPECT_TRUE(cfg.metabook);
    EXPECT_EQ(cfg.sleeve_method, "hrp");
    EXPECT_TRUE(cfg.impact_in_selection);
    EXPECT_DOUBLE_EQ(cfg.selection_aum, 5e7);
    EXPECT_TRUE(cfg.capacity_curve);
    EXPECT_TRUE(cfg.require_split_stable);
    EXPECT_TRUE(cfg.blocking_pbo);
    EXPECT_EQ(cfg.short_interest, "si.csv");
    EXPECT_EQ(cfg.augment_out, "aug.bin");
    EXPECT_EQ(cfg.si_publication_lag, 3);
    EXPECT_DOUBLE_EQ(cfg.kelly_fraction, 0.5);
    EXPECT_DOUBLE_EQ(cfg.kelly_max_gross, 2.0);
    EXPECT_TRUE(cfg.incremental_panel);
}

// omitted -> the inert default (byte-identical no-flag path).
TEST(ConfigParse, MegaBookFlags_OmittedAreInert) {
    auto r = parse({"atx-impl", "discover"});
    ASSERT_TRUE(r.has_value());
    const RunConfig& cfg = *r;
    EXPECT_EQ(cfg.risk_model, "diagonal");
    EXPECT_FALSE(cfg.dead_alpha_factors);
    EXPECT_FALSE(cfg.group_neutralize);
    EXPECT_FALSE(cfg.metabook);
    EXPECT_EQ(cfg.sleeve_method, "invvol");
    EXPECT_FALSE(cfg.impact_in_selection);
    EXPECT_DOUBLE_EQ(cfg.selection_aum, 0.0);
    EXPECT_FALSE(cfg.capacity_curve);
    EXPECT_FALSE(cfg.require_split_stable);
    EXPECT_FALSE(cfg.blocking_pbo);
    EXPECT_TRUE(cfg.short_interest.empty());
    EXPECT_TRUE(cfg.augment_out.empty());
    EXPECT_EQ(cfg.si_publication_lag, 2);
    EXPECT_DOUBLE_EQ(cfg.kelly_fraction, 0.0);
    EXPECT_DOUBLE_EQ(cfg.kelly_max_gross, 1.0);
    EXPECT_FALSE(cfg.incremental_panel);
}

// --risk-model / --sleeve-method reject an unknown value (closed taxonomy, like
// --weight-transform / --executor).
TEST(ConfigParse, RiskModelRejectsUnknownValue) {
    auto r = parse({"atx-impl", "discover", "--risk-model", "bogus"});
    EXPECT_FALSE(r.has_value());
}

TEST(ConfigParse, SleeveMethodRejectsUnknownValue) {
    auto r = parse({"atx-impl", "discover", "--sleeve-method", "bogus"});
    EXPECT_FALSE(r.has_value());
}

// ---------------------------------------------------------------------------
// ConfigFile.MegaBookFlags_RoundTrip — the same flags round-trip via --config,
// and a CLI-present flag overrides a file value (the set_flags merge).
// ---------------------------------------------------------------------------
TEST(ConfigFile, MegaBookFlags_RoundTrip) {
    const std::string path = "atx_s5_0_megabook_flags_test.cfg";
    {
        std::ofstream f(path);
        f << "risk-model=factor\n"
             "dead-alpha-factors=\n"
             "metabook=\n"
             "sleeve-method=erc\n"
             "require-split-stable=\n"
             "blocking-pbo=\n"
             "kelly-fraction=0.25\n"
             "min-dsr=0.5\n";
    }
    auto file_r = parse_config_file(path, "discover");
    ASSERT_TRUE(file_r.has_value()) << file_r.error().message();
    EXPECT_EQ(file_r->risk_model, "factor");
    EXPECT_TRUE(file_r->dead_alpha_factors);
    EXPECT_TRUE(file_r->metabook);
    EXPECT_EQ(file_r->sleeve_method, "erc");
    EXPECT_TRUE(file_r->require_split_stable);
    EXPECT_TRUE(file_r->blocking_pbo);
    EXPECT_DOUBLE_EQ(file_r->kelly_fraction, 0.25);

    // A CLI-present flag wins the merge, regardless of the file's value.
    auto cli_r = parse({"atx-impl", "discover", "--min-dsr", "0.9", "--config", path});
    ASSERT_TRUE(cli_r.has_value());
    RunConfig cfg = *cli_r;
    ASSERT_TRUE(merge_config_file(cfg, path));
    EXPECT_DOUBLE_EQ(cfg.min_dsr, 0.9);      // CLI value wins (file also had none for min-dsr... file DOES set nothing for min-dsr so it stays 0.9)
    EXPECT_EQ(cfg.risk_model, "factor");     // filled from the file (CLI left it unset)
    EXPECT_TRUE(cfg.metabook);               // filled from the file

    std::remove(path.c_str());
}
