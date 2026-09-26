#include <atomic>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "atx/core/sha256.hpp"
#include "config.hpp"
#include "prereg.hpp"
#include "stage_equity_ic.hpp"

namespace {
using Json = nlohmann::json;
namespace impl = atx::impl;

Json declaration() {
    return Json{{"schema", "atx-equity-ic-prereg-v1"}, {"epoch", "synthetic-v1"},
        {"checkpoint", 1001}, {"declared_n", 8},
        {"forward_variants", {"DropMissingForward", "IncludeAuditedTerminalV1"}},
        {"restrictions", {"full", "_ex34"}},
        {"families", Json::array({Json{{"id", "new_family"}, {"name", "new_signal"},
            {"dsl", "close / delay(close, 5) - 1"}, {"sign", -1}, {"theme", "reversal"},
            {"horizons", {5, 21}}, {"lineage", {{"kind", "new"}}}}})}};
}
impl::RunConfig cli_config(const std::vector<std::string> &extra, bool &ok) {
    std::vector<std::string> arguments{"atx-impl", "equity-ic"};
    arguments.insert(arguments.end(), extra.begin(), extra.end());
    std::vector<char *> argv;
    for (auto &arg : arguments) argv.push_back(arg.data());
    auto parsed = impl::parse_args(static_cast<int>(argv.size()), argv.data());
    ok = parsed.has_value();
    return parsed ? std::move(*parsed) : impl::RunConfig{};
}
} // namespace

TEST(EquityIcPrereg, CanonicalDslIgnoresLayoutButExactFileDigestDoesNot) {
    const auto input = declaration();
    auto first = impl::parse_equity_ic_prereg(input.dump());
    ASSERT_TRUE(first) << first.error().message();
    auto spaced = input;
    spaced["families"][0]["dsl"] = "close/delay(close,5)-1";
    auto second = impl::parse_equity_ic_prereg(spaced.dump(2));
    ASSERT_TRUE(second) << second.error().message();
    EXPECT_EQ(first->canonical_sha256, second->canonical_sha256);
    EXPECT_NE(first->file_sha256, second->file_sha256);
    EXPECT_EQ(first->measured_n, 8);
    EXPECT_EQ(first->horizons, (std::vector<atx::usize>{5, 21}));
    for (const std::string key : {"sign", "theme", "horizons"}) {
        auto changed = input;
        if (key == "sign") changed["families"][0][key] = 1;
        else if (key == "theme") changed["families"][0][key] = "other";
        else changed["families"][0][key] = Json::array({5, 63});
        auto parsed = impl::parse_equity_ic_prereg(changed.dump());
        ASSERT_TRUE(parsed) << parsed.error().message();
        EXPECT_NE(parsed->canonical_sha256, first->canonical_sha256);
        EXPECT_NE(parsed->families[0].configuration_sha256,
                  first->families[0].configuration_sha256);
    }
}

TEST(EquityIcPrereg, RejectsAmbiguousKeysCountsUnsupportedVariantsAndUnsafeIdentifiers) {
    const auto valid = declaration();
    auto duplicate_key = valid.dump();
    duplicate_key.insert(1, "\"epoch\":\"duplicate\",");
    EXPECT_FALSE(impl::parse_equity_ic_prereg(duplicate_key));
    for (int mutation = 0; mutation < 10; ++mutation) {
        auto bad = valid;
        switch (mutation) {
        case 0: bad["unknown"] = 1; break;
        case 1: bad["declared_n"] = 7; break;
        case 2: bad["declared_n"] = 18446744073709551615ULL; break;
        case 3: bad["families"][0]["sign"] = 0; break;
        case 4: bad["families"][0]["name"] = "bad,name"; break;
        case 5: bad["families"][0]["horizons"] = Json::array({21, 5}); break;
        case 6: bad["forward_variants"] = Json::array({"DropMissingForward"}); break;
        case 7: bad["families"][0]["lineage"]["unknown"] = 1; break;
        case 8: bad["families"][0]["dsl"] = "1 / 0"; break;
        default: bad["checkpoint"] = 22; break;
        }
        EXPECT_FALSE(impl::parse_equity_ic_prereg(bad.dump())) << mutation;
    }
    EXPECT_FALSE(impl::parse_equity_ic_prereg(std::string(1024U * 1024U + 1U, ' ')));
}

TEST(EquityIcPrereg, RejectsCanonicalConfigurationOverlapUnderDifferentFamilyNames) {
    auto input = declaration();
    auto other = input["families"][0];
    other["id"] = "other";
    other["name"] = "other";
    other["dsl"] = "close/delay(close,5)-1";
    other["horizons"] = Json::array({21});
    input["families"].push_back(other);
    input["declared_n"] = 12;
    EXPECT_FALSE(impl::parse_equity_ic_prereg(input.dump()));
    input["families"][1]["horizons"] = Json::array({63});
    EXPECT_TRUE(impl::parse_equity_ic_prereg(input.dump()));
}

TEST(EquityIcPrereg, RetainedLineageRequiresExactConfigurationAndNeverReducesMeasuredCount) {
    auto input = declaration();
    const auto original = impl::parse_equity_ic_prereg(input.dump());
    ASSERT_TRUE(original) << original.error().message();
    input["families"][0]["lineage"] = Json{{"kind", "retained"},
        {"prereg_sha256", original->canonical_sha256},
        {"configuration_sha256", original->families[0].configuration_sha256},
        {"trial_id", "prior-attempt-1"}};
    input["declared_n"] = 0;
    auto retained = impl::parse_equity_ic_prereg(input.dump());
    ASSERT_TRUE(retained) << retained.error().message();
    EXPECT_EQ(retained->declared_n, 0);
    EXPECT_EQ(retained->measured_n, 8);
    EXPECT_EQ(retained->retained_count, 1U);
    EXPECT_NE(retained->canonical_sha256, original->canonical_sha256);
    input["families"][0]["sign"] = 1;
    EXPECT_FALSE(impl::parse_equity_ic_prereg(input.dump()));
}

TEST(EquityIcPrereg, E2KeepsPriorFamilyProofAcrossDisplayAliasChangeWithoutDiscountingMeasuredN) {
    auto input = declaration();
    const auto prior = impl::parse_equity_ic_prereg(input.dump());
    ASSERT_TRUE(prior);
    input["families"][0]["id"] = "renamed_id";
    input["families"][0]["name"] = "renamed_display";
    input["families"][0]["theme"] = "renamed_theme";
    input["families"][0]["lineage"] = Json{{"kind", "retained"},
        {"prereg_sha256", prior->file_sha256},
        {"configuration_sha256", prior->families[0].configuration_sha256},
        {"trial_id", "prior-epoch-attempt"}};
    input["declared_n"] = 0;
    EXPECT_FALSE(impl::parse_equity_ic_prereg(input.dump())); // explicit legacy unchanged
    const auto parsed = impl::parse_equity_ic_prereg(input.dump(),
        impl::PreregLineageRule::CatalogVerifiedCellsE2);
    ASSERT_TRUE(parsed) << parsed.error().message();
    ASSERT_TRUE(parsed->families[0].lineage);
    EXPECT_EQ(parsed->families[0].lineage->configuration_sha256,
              prior->families[0].configuration_sha256);
    EXPECT_NE(parsed->families[0].configuration_sha256, prior->families[0].configuration_sha256);
    EXPECT_EQ(parsed->measured_n, 8);
    EXPECT_EQ(parsed->declared_n, 0);
    // This parse does not verify historical lineage; the mandatory catalog call does.
}

TEST(EquityIcPrereg, EpochSelectorRequiresRuntimeFileExplicitAnchorAndSeparateLegacyMode) {
    bool parsed = false;
    auto cfg = cli_config({"--ic-prereg-file", "fixture.json", "--ic-prereg-sha256",
        std::string(64, 'a'), "--ic-trial-accounting-rule", "epoch-e2-v1",
        "--ic-epoch-catalog", "epoch.bin", "--ic-epoch-anchor", std::string(64, '0')}, parsed);
    ASSERT_TRUE(parsed);
    EXPECT_TRUE(impl::validate_ic_epoch_flags(cfg));
    cfg.equity_ic_epoch_anchor.clear();
    EXPECT_FALSE(impl::validate_ic_epoch_flags(cfg));
    EXPECT_FALSE(impl::run_equity_ic(cfg));
    cfg.equity_ic_epoch_anchor.assign(64, '0');
    cfg.equity_ic_trial_accounting_rule = "legacy-ledger-v1";
    EXPECT_FALSE(impl::validate_ic_epoch_flags(cfg));
    cfg.equity_ic_trial_accounting_rule = "epoch-e2-v1";
    cfg.equity_ic_prereg_file.clear();
    cfg.equity_ic_prereg_sha256.clear();
    EXPECT_FALSE(impl::validate_ic_epoch_flags(cfg));
}

TEST(EquityIcPrereg, ExactFileHashRefusesChangedBytes) {
    namespace fs = std::filesystem;
    static std::atomic<unsigned> sequence{};
    fs::path directory;
    for (unsigned i = 0; i < 1000; ++i) {
        const auto candidate = fs::temp_directory_path() /
            ("atx_prereg_" + std::to_string(sequence.fetch_add(1)));
        if (fs::create_directory(candidate)) { directory = candidate; break; }
    }
    ASSERT_FALSE(directory.empty());
    struct Cleanup {
        fs::path path;
        ~Cleanup() { std::error_code ec; fs::remove_all(path, ec); }
    } cleanup{directory};
    const auto file = directory / "prereg.json";
    const auto text = declaration().dump();
    { std::ofstream out(file, std::ios::binary); out << text; }
    const auto sha = atx::core::sha256_hex(text);
    ASSERT_TRUE(sha);
    EXPECT_TRUE(impl::load_equity_ic_prereg(file.string(), *sha));
    { std::ofstream out(file, std::ios::binary | std::ios::app); out << '\n'; }
    EXPECT_FALSE(impl::load_equity_ic_prereg(file.string(), *sha));
}

TEST(EquityIcPrereg, ParserAndDirectStageSharePairedHashAndCommandGuard) {
    bool parsed = false;
    (void)cli_config({"--ic-prereg-file", "fixture.json"}, parsed);
    EXPECT_FALSE(parsed);
    (void)cli_config({"--ic-prereg-sha256", std::string(64, 'a')}, parsed);
    EXPECT_FALSE(parsed);
    auto cfg = cli_config({"--ic-prereg-file", "fixture.json", "--ic-prereg-sha256",
                           std::string(64, 'a')}, parsed);
    ASSERT_TRUE(parsed);
    EXPECT_TRUE(impl::validate_ic_prereg_flags(cfg));
    cfg.subcommand = "discover";
    EXPECT_FALSE(impl::validate_ic_prereg_flags(cfg));
    EXPECT_FALSE(impl::run_equity_ic(cfg));
    cfg.subcommand = "equity-ic";
    cfg.equity_ic_prereg_sha256[0] = 'A';
    EXPECT_FALSE(impl::validate_ic_prereg_flags(cfg));
    EXPECT_FALSE(impl::run_equity_ic(cfg));
    cfg.equity_ic_prereg_sha256.clear();
    EXPECT_FALSE(impl::run_equity_ic(cfg));
}
