#include <algorithm>
#include <bit>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "atx/core/error.hpp"
#include "atx/core/sha256.hpp"
#include "atx/engine/alpha/bytecode.hpp"
#include "atx/engine/alpha/panel.hpp"
#include "atx/engine/alpha/parser.hpp"
#include "atx/engine/alpha/registry.hpp"
#include "atx/engine/alpha/typecheck.hpp"
#include "atx/engine/alpha/vm.hpp"

namespace {
namespace alpha = atx::engine::alpha;
using atx::f64;
using atx::usize;
constexpr usize kDates = 420, kNames = 64, kFuture = 360;

atx::core::Result<alpha::Panel> strategy_panel(bool changed) {
    std::vector<std::vector<f64>> fields(3, std::vector<f64>(kDates * kNames));
    std::vector<atx::u8> present(kDates * kNames, 1);
    for (usize d = 0; d < kDates; ++d) for (usize i = 0; i < kNames; ++i) {
        const auto t = static_cast<f64>(d), n = static_cast<f64>(i);
        const auto cell = d * kNames + i;
        const f64 raw = 40 + .25 * n + .02 * t + 3 * std::sin(.03 * t + .17 * n) +
            .7 * std::cos(.09 * t + .11 * n);
        fields[0][cell] = raw * (1 + .0001 * t); // synthetic adjusted return proxy
        fields[1][cell] = raw;
        fields[2][cell] = 500000 + 2500 * n + 100000 * std::sin(.07 * t + .21 * n) +
            50000 * std::cos(.11 * t + .15 * n);
        if (changed && (d >= kFuture || i >= 60)) {
            // Future payload may differ arbitrarily. Forever-ineligible names
            // may also differ in the past without entering cross-sectional ops.
            fields[0][cell] *= 3 + .01 * n;
            fields[1][cell] *= 1e8;
            fields[2][cell] *= 1e9;
        }
    }
    present[25 * kNames + 2] = 0; // independent source absence with finite backing
    return alpha::Panel::create(kDates, kNames, {"close", "raw_close", "volume"},
        std::move(fields), std::move(present));
}
std::vector<atx::u8> strategy_membership() {
    std::vector<atx::u8> member(kDates * kNames, 1);
    for (usize d = 0; d < kDates; ++d) for (usize i = 0; i < kNames; ++i)
        if (i >= 60 || (i < 8 && d < 80)) member[d * kNames + i] = 0;
    return member;
}
} // namespace

TEST(StrategyDsl, FrozenLibraryCompilesRunsAndPreservesCausalMaskedPrefix) {
    const auto directory = std::filesystem::path{__FILE__}.parent_path().parent_path() / "strategies";
    std::ifstream input(directory / "slow_price_volume_24_v1.json", std::ios::binary);
    ASSERT_TRUE(input);
    const std::string bytes{std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
    const auto library = nlohmann::json::parse(bytes);
    ASSERT_EQ(library.at("schema"), "atx.dsl-strategy-library/v1");
    ASSERT_EQ(library.at("candidates").size(), 24U);
    ASSERT_EQ(library.at("families").size(), 6U);
    ASSERT_EQ(library.at("primary_variant"), "weekly_partial25");
    std::ifstream recipe_input(directory / "slow_price_volume_24_v1.recipe.json", std::ios::binary);
    ASSERT_TRUE(recipe_input);
    const auto recipe = nlohmann::json::parse(recipe_input);
    auto digest = atx::core::sha256_hex(bytes); ASSERT_TRUE(digest);
    EXPECT_EQ(*digest, recipe.at("library").at("sha256").get<std::string>());
    EXPECT_EQ(recipe.at("trial_accounting").at("declared_train_evaluations"), 50);

    auto original = strategy_panel(false), mutated = strategy_panel(true);
    ASSERT_TRUE(original) << original.error().message();
    ASSERT_TRUE(mutated) << mutated.error().message();
    const auto member = strategy_membership();
    alpha::Engine engine{*original}, changed_engine{*mutated};
    engine.set_eval_mode(alpha::EvalMode::ResearchFast);
    changed_engine.set_eval_mode(alpha::EvalMode::ResearchFast);
    ASSERT_TRUE(engine.set_cross_section_mask(member));
    ASSERT_TRUE(changed_engine.set_cross_section_mask(member));
    alpha::Library ops;
    std::set<std::string> ids;
    usize largest_lookback = 0;
    for (const auto &candidate : library.at("candidates")) {
        const auto id = candidate.at("id").get<std::string>();
        SCOPED_TRACE(id);
        ASSERT_TRUE(ids.insert(id).second);
        const auto dsl = candidate.at("dsl").get<std::string>();
        auto ast = alpha::parse_expr(dsl, ops); ASSERT_TRUE(ast) << ast.error().message();
        auto analysis = alpha::analyze(*ast); ASSERT_TRUE(analysis) << analysis.error().message();
        largest_lookback = std::max(largest_lookback, static_cast<usize>(analysis->required_lookback()));
        EXPECT_LE(analysis->required_lookback(), 314U);
        auto program = alpha::compile(*ast, *analysis); ASSERT_TRUE(program) << program.error().message();
        auto actual = engine.evaluate(*program), changed = changed_engine.evaluate(*program);
        ASSERT_TRUE(actual) << actual.error().message();
        ASSERT_TRUE(changed) << changed.error().message();
        ASSERT_EQ(actual->dates, kDates); ASSERT_EQ(actual->instruments, kNames);
        ASSERT_EQ(actual->alphas.size(), 1U); ASSERT_EQ(changed->alphas.size(), 1U);
        const auto &values = actual->alphas.front().values;
        const auto &other = changed->alphas.front().values;
        ASSERT_EQ(values.size(), kDates * kNames); ASSERT_EQ(other.size(), values.size());
        usize ready = 0;
        for (usize i = 0; i < 60; ++i) ready += std::isfinite(values[345 * kNames + i]);
        EXPECT_GE(ready, 48U); // real rolling windows have completed, no padded readiness
        for (usize d = 0; d < kFuture; ++d) for (usize i = 0; i < 60; ++i)
            EXPECT_EQ(std::bit_cast<atx::u64>(values[d * kNames + i]),
                      std::bit_cast<atx::u64>(other[d * kNames + i]));
        for (usize i = 60; i < kNames; ++i) EXPECT_FALSE(std::isfinite(values[345 * kNames + i]));
    }
    EXPECT_EQ(ids.size(), 24U);
    EXPECT_EQ(largest_lookback, 314U);
    // This fixture establishes syntax, geometry, readiness and causal masking;
    // it makes no assertion about returns, costs, capacity or profitability.
}
