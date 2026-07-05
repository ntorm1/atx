#include <gtest/gtest.h>
#include "config.hpp"

using atx::impl::parse_args;
using atx::impl::RunConfig;

namespace {
atx::core::Result<RunConfig> parse(std::vector<std::string> args) {
    std::vector<char*> argv;
    argv.reserve(args.size());
    for (auto& a : args) argv.push_back(a.data());
    return parse_args(static_cast<int>(argv.size()), argv.data());
}
} // namespace

TEST(ConfigParse, DeadAlphaLibDir_RoundTrip) {
    auto r = parse({"atx-impl", "optimize", "--dead-alpha-lib-dir", "/tmp/mylib",
                    "--dead-alpha-factors"});
    ASSERT_TRUE(r.has_value()) << r.error().message();
    EXPECT_EQ(r->dead_alpha_lib_dir, "/tmp/mylib");
    EXPECT_TRUE(r->dead_alpha_factors);
    EXPECT_TRUE(r->set_flags.count("dead-alpha-lib-dir"));
}

TEST(ConfigParse, DeadAlphaLibDir_OmittedIsInert) {
    auto r = parse({"atx-impl", "optimize"});
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->dead_alpha_lib_dir, "");
    EXPECT_FALSE(r->set_flags.count("dead-alpha-lib-dir"));
}
