#include <filesystem>
#include <fstream>
#include <string>

#include <gtest/gtest.h>

#include "config.hpp"
#include "w0i0b_support.hpp"

namespace atx_test_w0_replay_config {
namespace impl = atx::impl;
using atx_test_w0_i0b_support::parse;

TEST(ImplReplayPolicy, CorrectedDefaultAndExplicitLegacyCli) {
    const auto cfg = parse({"atx-impl", "report"});
    ASSERT_TRUE(cfg);
    EXPECT_EQ(cfg->replay_delisting_policy, impl::ReplayDelistingPolicy::TerminalReturnV2);
    const auto legacy = parse({"atx-impl", "report", "--replay-delisting-policy", "abort"});
    ASSERT_TRUE(legacy);
    EXPECT_EQ(legacy->replay_delisting_policy, impl::ReplayDelistingPolicy::AbortV1);
    EXPECT_TRUE(legacy->set_flags.contains("replay-delisting-policy"));
    EXPECT_FALSE(parse({"atx-impl", "report", "--replay-delisting-policy", "ignore"}));
    EXPECT_FALSE(parse({"atx-impl", "report", "--replay-delisting-policy"}));
}

TEST(ImplReplayPolicy, ConfigFileWorksAndExplicitCliDefaultWins) {
    const auto path = std::filesystem::temp_directory_path() / "atx_w0replay_config.cfg";
    {
        std::ofstream out(path, std::ios::binary);
        out << "replay-delisting-policy=abort\n";
    }
    const auto file = impl::parse_config_file(path.string(), "report");
    ASSERT_TRUE(file) << file.error().message();
    EXPECT_EQ(file->replay_delisting_policy, impl::ReplayDelistingPolicy::AbortV1);
    auto cli = parse({"atx-impl", "report", "--replay-delisting-policy", "terminal-return"});
    ASSERT_TRUE(cli);
    ASSERT_TRUE(impl::merge_config_file(*cli, path.string()));
    EXPECT_EQ(cli->replay_delisting_policy, impl::ReplayDelistingPolicy::TerminalReturnV2);
    std::filesystem::remove(path);
}
} // namespace atx_test_w0_replay_config
