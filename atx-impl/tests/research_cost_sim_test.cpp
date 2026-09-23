#include "research_cost_sim.hpp"

#include <cmath>
#include <limits>
#include <vector>

#include <gtest/gtest.h>

namespace atx_test_l8_e2e_research_cost {
namespace impl = atx::impl;
namespace book = atx::engine::book;
constexpr atx::usize kNames = 20;
constexpr atx::usize kPeriods = 500;

struct Stream {
    atx::u64 state;
    atx::f64 next() noexcept {
        state = state * 6364136223846793005ULL + 1442695040888963407ULL;
        return static_cast<atx::f64>(state >> 11) * (1.0 / 9007199254740992.0);
    }
};

// Planted +/-10 bps/day edge on a sign pattern; `flip` alternates the sign every
// period (a fast alpha), otherwise the pattern is fixed (a slow alpha). Returns
// the weights (gross 1, equal magnitude) and matching realized returns.
struct Path {
    std::vector<atx::f64> weights;
    std::vector<atx::f64> returns;
};

inline Path planted(bool flip, atx::u64 seed) {
    Stream rng{seed};
    Path p;
    p.weights.resize(kPeriods * kNames);
    p.returns.resize(kPeriods * kNames);
    for (atx::usize t = 0; t < kPeriods; ++t) {
        for (atx::usize i = 0; i < kNames; ++i) {
            atx::f64 sign = (i % 2 == 0) ? 1.0 : -1.0;
            if (flip && t % 2 == 1) sign = -sign;
            p.weights[t * kNames + i] = sign / static_cast<atx::f64>(kNames);
            p.returns[t * kNames + i] = 0.001 * sign + 0.02 * (rng.next() - 0.5);
        }
    }
    return p;
}

TEST(ResearchCostSim, HighTurnoverAlphaLosesAdmission) {
    const auto flat = book::FlatBpsCost::create(8.0).value();
    const auto slow = planted(false, 7);
    const auto fast = planted(true, 7);
    const auto slow_result =
        impl::research_cost_sim(slow.weights, slow.returns, kPeriods, kNames, {}, flat);
    const auto fast_result =
        impl::research_cost_sim(fast.weights, fast.returns, kPeriods, kNames, {}, flat);
    ASSERT_TRUE(slow_result) << slow_result.error().message();
    ASSERT_TRUE(fast_result) << fast_result.error().message();
    // Both carry the same planted gross edge ...
    EXPECT_GT(slow_result->gross_sharpe, 3.0);
    EXPECT_GT(fast_result->gross_sharpe, 3.0);
    // ... but only the slow alpha survives its costs.
    EXPECT_TRUE(slow_result->admitted);
    EXPECT_FALSE(fast_result->admitted);
    EXPECT_GT(fast_result->mean_turnover, 1.9);
    EXPECT_LT(slow_result->mean_turnover, 0.1);
    EXPECT_LT(fast_result->net_sharpe, slow_result->net_sharpe);
}

TEST(ResearchCostSim, FrictionlessModelLeavesNetEqualToGross) {
    const auto frictionless = book::FlatBpsCost::create(0.0).value();
    const auto path = planted(true, 3);
    const auto result =
        impl::research_cost_sim(path.weights, path.returns, kPeriods, kNames, {}, frictionless);
    ASSERT_TRUE(result);
    for (atx::usize t = 0; t < kPeriods; ++t) {
        EXPECT_EQ(result->net_returns[t], result->gross_returns[t]);
    }
    EXPECT_EQ(result->cost_drag_bps, 0.0);
}

TEST(ResearchCostSim, ImpactCostGrowsWithAum) {
    const auto impact = book::SqrtImpactCost::create(
        {1.0, 0.5}, std::numeric_limits<atx::f64>::infinity()).value();
    const auto path = planted(false, 11);
    const std::vector<book::LiquidityRow> liquidity(kPeriods * kNames,
                                                    book::LiquidityRow{5.0e6, 0.02, 2.0});
    atx::f64 previous_drag = -1.0;
    for (const auto aum : {1.0e7, 1.0e8, 1.0e9}) {
        impl::ResearchCostSimConfig cfg;
        cfg.aum = aum;
        const auto result = impl::research_cost_sim(path.weights, path.returns, kPeriods, kNames,
                                                    liquidity, impact, cfg);
        ASSERT_TRUE(result) << result.error().message();
        EXPECT_GT(result->cost_drag_bps, previous_drag);
        previous_drag = result->cost_drag_bps;
    }
}

TEST(ResearchCostSim, RejectsBadShapesAndHeldNaNReturns) {
    const auto flat = book::FlatBpsCost::create(5.0).value();
    std::vector<atx::f64> w{0.5, -0.5};
    std::vector<atx::f64> r{0.01, std::numeric_limits<atx::f64>::quiet_NaN()};
    EXPECT_FALSE(impl::research_cost_sim(w, r, 1, 2, {}, flat));
    EXPECT_FALSE(impl::research_cost_sim(w, r, 2, 2, {}, flat));
    const auto impact = book::SqrtImpactCost::create({1.0, 0.5}, 0.1).value();
    r[1] = 0.0;
    EXPECT_FALSE(impl::research_cost_sim(w, r, 1, 2, {}, impact)); // Needs liquidity.
}

TEST(ResearchCostSim, UnusableLiquidityNameIsNeverCheaperThanALiquidOne) {
    const auto impact = book::SqrtImpactCost::create({1.0, 0.5}, 0.1).value();
    const std::vector<atx::f64> w{0.5, -0.5};
    const std::vector<atx::f64> r{0.0, 0.0};
    const book::LiquidityRow liquid{5.0e6, 0.02, 2.0};
    const atx::f64 nan = std::numeric_limits<atx::f64>::quiet_NaN();
    for (const auto bad : {book::LiquidityRow{nan, 0.02, 2.0}, book::LiquidityRow{0.0, 0.02, 2.0},
                           book::LiquidityRow{-1.0, 0.02, 2.0}}) {
        impl::ResearchCostSimConfig cfg;
        cfg.aum = 1.0e6;
        const std::vector<book::LiquidityRow> both_liquid{liquid, liquid};
        const std::vector<book::LiquidityRow> one_bad{liquid, bad};
        const auto base = impl::research_cost_sim(w, r, 1, 2, both_liquid, impact, cfg);
        const auto thin = impl::research_cost_sim(w, r, 1, 2, one_bad, impact, cfg);
        ASSERT_TRUE(base) << base.error().message();
        ASSERT_TRUE(thin) << thin.error().message();
        EXPECT_GT(thin->cost_drag_bps, 0.0);
        EXPECT_GT(thin->cost_drag_bps, base->cost_drag_bps);
        cfg.unusable_liquidity_penalty_bps = nan; // Reject instead of penalize.
        EXPECT_FALSE(impl::research_cost_sim(w, r, 1, 2, one_bad, impact, cfg));
        cfg.unusable_liquidity_penalty_bps = -1.0;
        EXPECT_FALSE(impl::research_cost_sim(w, r, 1, 2, both_liquid, impact, cfg));
    }
}

TEST(ResearchCostSim, CappedTradeIsChargedAtTheUncappedFullSizeRate) {
    // 50% of ADV with a 10% cap: the whole request is charged at the 50% rate,
    // (0.5/0.1)^0.5 ~= 2.24x the capped fill's average impact rate.
    const auto capped = book::SqrtImpactCost::create({1.0, 0.5}, 0.1).value();
    const auto uncapped = book::SqrtImpactCost::create(
        {1.0, 0.5}, std::numeric_limits<atx::f64>::infinity()).value();
    const book::LiquidityRow row{2.0e6, 0.02, 0.0};
    const std::vector<atx::f64> w{1.0};
    const std::vector<atx::f64> r{0.0};
    const std::vector<book::LiquidityRow> liquidity{row};
    impl::ResearchCostSimConfig cfg;
    cfg.aum = 1.0e6; // 1e6 trade on 2e6 ADV == 50% participation.
    const auto a = impl::research_cost_sim(w, r, 1, 1, liquidity, capped, cfg);
    const auto b = impl::research_cost_sim(w, r, 1, 1, liquidity, uncapped, cfg);
    ASSERT_TRUE(a && b);
    EXPECT_DOUBLE_EQ(a->cost_drag_bps, b->cost_drag_bps);
    const auto full_rate = 0.02 * std::sqrt(0.5);
    EXPECT_NEAR(a->cost_drag_bps, full_rate * 1.0e4, 1.0e-9);
    const auto capped_avg_rate = 0.02 * std::sqrt(0.1);
    EXPECT_NEAR(a->cost_drag_bps / (capped_avg_rate * 1.0e4), std::sqrt(5.0), 1.0e-9);
}

} // namespace atx_test_l8_e2e_research_cost
