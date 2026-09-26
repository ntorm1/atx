#include <array>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "config.hpp"
#include "store_progress_sink.hpp"

namespace atx_test_ic_screen_config {
using atx::engine::factory::IcScreenRule;
namespace impl = atx::impl;

atx::core::Result<impl::RunConfig> parse(std::initializer_list<std::string> input) {
    std::vector<std::string> storage(input);
    std::vector<char*> argv;
    for (auto& token : storage) argv.push_back(token.data());
    return impl::parse_args(static_cast<int>(argv.size()), argv.data());
}

TEST(ImplIcScreenConfig, DefaultEnabledAndExplicitLegacyAvailable) {
    auto current = parse({"atx-impl", "discover"});
    ASSERT_TRUE(current);
    EXPECT_EQ(current->ic_screen.rule, IcScreenRule::EquivalenceV3);
    EXPECT_DOUBLE_EQ(current->ic_screen.practical_abs_ic, 0.002);
    EXPECT_DOUBLE_EQ(current->ic_screen.confidence_multiplier, 3.5);
    auto v2 = parse({"atx-impl", "discover", "--ic-screen-rule", "conservative-v2",
                     "--ic-screen-min-abs-ic", "0.02"});
    ASSERT_TRUE(v2);
    EXPECT_EQ(v2->ic_screen.rule, IcScreenRule::ConservativeV2);
    EXPECT_DOUBLE_EQ(v2->ic_screen.practical_abs_ic, 0.02);
    auto legacy = parse({"atx-impl", "discover", "--ic-screen-rule", "disabled-v1"});
    ASSERT_TRUE(legacy);
    EXPECT_EQ(legacy->ic_screen.rule, IcScreenRule::DisabledV1);
    EXPECT_TRUE(legacy->set_flags.contains("ic-screen-rule"));
}

TEST(ImplIcScreenConfig, RejectsInvalidAndAmbiguousScreenRecipes) {
    for (const auto& horizons : {"", "5,21,63", "5,21,63,126,252", "5,5,63,126",
                                "0,21,63,126", "21,5,63,126", "5,21,63,126,", "5,21,x,126"}) {
        EXPECT_FALSE(parse({"atx-impl", "discover", "--ic-screen-horizons", horizons}));
    }
    for (const auto& value : {"nan", "inf", "-1", "0", "1", "1.1"}) {
        EXPECT_FALSE(parse({"atx-impl", "discover", "--ic-screen-min-abs-ic", value}));
    }
    EXPECT_FALSE(parse({"atx-impl", "discover", "--ic-screen-confidence", "1.0"}));
    EXPECT_FALSE(parse({"atx-impl", "discover", "--ic-screen-confidence", "2.5"}));
    EXPECT_FALSE(parse({"atx-impl", "discover", "--ic-screen-min-dates", "-1"}));
    EXPECT_FALSE(parse({"atx-impl", "discover", "--ic-screen-min-dates", "7"}));
    EXPECT_FALSE(parse({"atx-impl", "discover", "--ic-screen-max-cache-mib", "0"}));
    EXPECT_FALSE(parse({"atx-impl", "discover", "--ic-screen-rule", "fast"}));
}

TEST(ImplIcScreenConfig, FileRecipeAndExplicitCliOverrideAgree) {
    const auto path = std::filesystem::temp_directory_path() / "atx_ic_screen_config.cfg";
    {
        std::ofstream file(path);
        file << "ic-screen-rule=disabled-v1\nic-screen-horizons=1,5,21,63\n"
                "ic-screen-min-abs-ic=0.005\nic-screen-max-cache-mib=64\n";
    }
    auto cfg = parse({"atx-impl", "discover", "--ic-screen-rule", "conservative-v2"});
    ASSERT_TRUE(cfg);
    ASSERT_TRUE(impl::merge_config_file(*cfg, path.string()));
    EXPECT_EQ(cfg->ic_screen.rule, IcScreenRule::ConservativeV2);
    EXPECT_EQ(cfg->ic_screen.horizons, (std::array<atx::usize, 4>{1, 5, 21, 63}));
    EXPECT_DOUBLE_EQ(cfg->ic_screen.practical_abs_ic, 0.005);
    EXPECT_EQ(cfg->ic_screen.max_cache_bytes, atx::u64{64} * 1024U * 1024U);
    std::filesystem::remove(path);
}

TEST(ImplIcScreenConfig, EveryEnabledDecisionInputInvalidatesResume) {
    impl::RunConfig base;
    base.panel = "fixed-panel.bin";
    const auto fingerprint = impl::compute_discover_fingerprint(base);
    std::vector<impl::RunConfig> changed(11, base);
    changed[0].ic_screen.rule = IcScreenRule::DisabledV1;
    changed[1].ic_screen.horizons[0] = 2;
    changed[2].ic_screen.execution_delay = 2;
    changed[3].ic_screen.window_begin = 1;
    changed[4].ic_screen.window_end = 1000;
    changed[5].ic_screen.maturity_end = 900;
    changed[6].ic_screen.min_names += 1;
    changed[7].ic_screen.min_dates += 1;
    changed[8].ic_screen.practical_abs_ic *= 0.5;
    changed[9].ic_screen.confidence_multiplier += 0.5;
    changed[10].ic_screen.max_cache_bytes /= 2;
    for (const auto& config : changed) {
        EXPECT_NE(impl::compute_discover_fingerprint(config), fingerprint);
    }
    base.ic_screen.rule = IcScreenRule::DisabledV1;
    auto inactive = base;
    inactive.ic_screen.horizons[0] = 2;
    inactive.ic_screen.practical_abs_ic = 0.5;
    EXPECT_EQ(impl::compute_discover_fingerprint(base),
              impl::compute_discover_fingerprint(inactive));
}
} // namespace atx_test_ic_screen_config
