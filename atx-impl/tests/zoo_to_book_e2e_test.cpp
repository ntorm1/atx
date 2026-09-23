// Zoo-to-book end-to-end: a synthetic "discovered" alpha zoo is combined into a
// combo artifact, read back through PreferenceSource, allocated by the equity
// QP under beta and sector constraints, and replayed with square-root impact.
#include <algorithm>
#include <bit>
#include <cmath>
#include <span>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "atx/core/sha256.hpp"
#include "atx/engine/alpha/panel.hpp"
#include "atx/engine/book/replay.hpp"
#include "atx/engine/book/replay_cost.hpp"
#include "equity_allocation.hpp"
#include "preference_source.hpp"

namespace atx_test_l8_e2e_zoo {
namespace impl = atx::impl;
namespace book = atx::engine::book;
using atx::engine::alpha::Panel;
constexpr atx::i64 kDay = 86'400'000'000'000LL;
constexpr atx::usize kNames = 24;
constexpr atx::usize kDates = 150;
constexpr atx::usize kSignals = 3;
constexpr atx::usize kWindow = 64;
constexpr atx::usize kEvery = 5;
constexpr atx::f64 kBetaTol = 0.02;
constexpr atx::f64 kSectorCap = 0.03;
constexpr atx::f64 kNav = 1.0e7;

struct Stream {
    atx::u64 state;
    atx::f64 uniform() noexcept {
        state = state * 6364136223846793005ULL + 1442695040888963407ULL;
        return static_cast<atx::f64>(state >> 11) * (1.0 / 9007199254740992.0);
    }
    atx::f64 noise() noexcept { return uniform() - 0.5; }
};

struct World {
    std::vector<atx::f64> close;              // kDates x kNames
    std::vector<std::vector<atx::f64>> zoo;   // kSignals x (kDates x kNames)
    std::vector<atx::f64> beta;
    std::vector<atx::usize> sector;
    std::vector<atx::i64> keys;
};

// Market beta, three sectors, and a persistent planted signal z that predicts the
// next return; each zoo member observes z with its own noise.
inline World make_world() {
    Stream rng{42};
    World w;
    for (atx::usize i = 0; i < kNames; ++i) {
        w.beta.push_back(0.5 + static_cast<atx::f64>(i % 6) * 0.2);
        w.sector.push_back(i % 3);
    }
    std::vector<atx::f64> z(kNames, 0.0);
    std::vector<atx::f64> price(kNames, 50.0);
    w.zoo.assign(kSignals, std::vector<atx::f64>(kDates * kNames));
    for (atx::usize t = 0; t < kDates; ++t) {
        w.keys.push_back(static_cast<atx::i64>(t) * kDay);
        const auto market = 0.01 * rng.noise();
        for (atx::usize i = 0; i < kNames; ++i) {
            if (t > 0) {
                const auto r = w.beta[i] * market + 0.004 * z[i] + 0.02 * rng.noise();
                price[i] *= 1.0 + r;
            }
            w.close.push_back(price[i]);
            z[i] = 0.9 * z[i] + rng.noise();
            for (atx::usize k = 0; k < kSignals; ++k) {
                w.zoo[k][t * kNames + i] = z[i] + 0.5 * rng.noise();
            }
        }
    }
    return w;
}

inline impl::PanelIdentity identity(const World &w, atx::usize dates) {
    impl::PanelIdentity id;
    id.instrument_namespace = "test.l8.zoo";
    id.session_keys.assign(w.keys.begin(), w.keys.begin() + static_cast<std::ptrdiff_t>(dates));
    for (atx::usize i = 0; i < kNames; ++i) {
        id.instrument_ids.push_back(std::to_string(1 + i));
        id.original_instrument_indices.push_back(i);
    }
    id.recipe = "stage=zoo-e2e";
    id.parents.push_back({"research", atx::core::sha256_hex("zoo-e2e").value()});
    return id;
}

// Combine: equal-weight average of cross-sectionally z-scored zoo members.
inline std::vector<atx::f64> combine(const World &w, atx::usize dates) {
    std::vector<atx::f64> alpha(dates * kNames, 0.0);
    for (atx::usize t = 0; t < dates; ++t) {
        for (const auto &signal : w.zoo) {
            const auto row = std::span<const atx::f64>(signal).subspan(t * kNames, kNames);
            atx::f64 mean = 0.0;
            for (const auto x : row) mean += x;
            mean /= static_cast<atx::f64>(kNames);
            atx::f64 var = 0.0;
            for (const auto x : row) var += (x - mean) * (x - mean);
            const auto sd = std::sqrt(var / static_cast<atx::f64>(kNames));
            for (atx::usize i = 0; i < kNames; ++i) {
                alpha[t * kNames + i] += (row[i] - mean) / sd / static_cast<atx::f64>(kSignals);
            }
        }
    }
    return alpha;
}

struct Run {
    book::ReplayPolicyResult replay;
    std::vector<impl::EquityAllocationResult> allocations;
};

inline impl::EquityAllocationConfig allocation_config() {
    impl::EquityAllocationConfig cfg;
    cfg.gross_limit = 1.0;
    cfg.name_limit = 0.1;
    cfg.turnover_limit = 0.6;
    cfg.trade_bps = 10.0;
    cfg.risk_penalty = 1.0;
    cfg.beta_tolerance = kBetaTol;
    cfg.sector_net_cap = kSectorCap;
    return cfg;
}

// discover (zoo) -> combine -> PreferenceSource -> constrained QP -> impact replay,
// on the first `dates` observations only.
inline atx::core::Result<Run> run_book(const World &w, atx::usize dates,
                                       const book::ReplayCostModel &cost) {
    const auto close = std::vector<atx::f64>(w.close.begin(),
                                             w.close.begin() + static_cast<std::ptrdiff_t>(dates * kNames));
    auto panel = Panel::create(dates, kNames, {"close"}, {close}, {}).value();
    const impl::PanelArtifact combo{Panel::create(dates, kNames, {"alpha"}, {combine(w, dates)}, {}).value(),
                                    identity(w, dates), "", ""};
    ATX_TRY(auto source, impl::PreferenceSource::from_combo(combo, identity(w, dates),
                                                           w.keys[dates - 1], 1.0));
    std::vector<atx::usize> decisions;
    std::vector<atx::f64> preferences;
    for (atx::usize t = kWindow - 1; t + 1 < dates; t += kEvery) {
        decisions.push_back(t);
        ATX_TRY(auto row, source.preference(t));
        preferences.insert(preferences.end(), row.begin(), row.end());
    }
    Run run;
    const auto keys = std::span<const atx::i64>(w.keys).first(dates);
    const book::ReplayIntentPolicy policy = [&](const book::ReplayAllocationState &s)
        -> atx::core::Result<std::vector<book::ReplayTargetIntent>> {
        const auto first = s.decision_period + 1 - kWindow;
        const impl::EquityAllocationRiskWindow window{s.decision_period, first, s.decision_period,
            s.decision_session_key, keys.subspan(first, kWindow),
            std::span<const atx::f64>(close).subspan(first * kNames, kWindow * kNames), {}};
        ATX_TRY(auto decision, impl::freeze_equity_allocation_decision(
            window, s.preference_weights, s.decision_eligibility, allocation_config()));
        ATX_TRY_VOID(impl::attach_equity_exposures(decision, w.beta, w.sector));
        const impl::EquityAllocationExecution execution{s.decision_period, s.execution_period,
            s.decision_session_key, s.execution_session_key, s.cash, s.pretrade_nav, s.tri_units,
            s.current_marks, s.marked_dollars};
        ATX_TRY(auto allocated, impl::allocate_equity_preference(decision, execution));
        auto intents = allocated.intents;
        run.allocations.push_back(std::move(allocated));
        return atx::core::Ok(std::move(intents));
    };
    book::ReplayConfig cfg;
    cfg.initial_nav = kNav;
    cfg.execution_delay_periods = 1;
    cfg.cost_model = &cost;
    const std::vector<book::LiquidityRow> liquidity(dates * kNames,
                                                    book::LiquidityRow{5.0e7, 0.02, 3.0});
    cfg.liquidity = liquidity;
    ATX_TRY(auto replay, book::replay_scheduled_intents(panel, keys, decisions, preferences,
                                                        policy, cfg));
    run.replay = std::move(replay);
    return atx::core::Ok(std::move(run));
}

inline bool same_bits(atx::f64 a, atx::f64 b) {
    return std::bit_cast<atx::u64>(a) == std::bit_cast<atx::u64>(b);
}
} // namespace atx_test_l8_e2e_zoo

using namespace atx_test_l8_e2e_zoo;

TEST(ZooToBookE2E, ConstrainedCostAwareBookHoldsItsFactorBounds) {
    const auto world = make_world();
    const auto cost = book::SqrtImpactCost::create({0.8, 0.5}, 0.1).value();
    const auto run = run_book(world, kDates, cost);
    ASSERT_TRUE(run) << run.error().message();
    ASSERT_GT(run->replay.allocations.size(), 10U);
    ASSERT_EQ(run->replay.allocations.size(), run->allocations.size());
    EXPECT_EQ(run->replay.replay.open_working_orders, 0U);
    for (const auto &allocation : run->replay.allocations) {
        atx::f64 beta = 0.0;
        std::vector<atx::f64> sector(3, 0.0);
        atx::f64 gross = 0.0;
        for (atx::usize i = 0; i < kNames; ++i) {
            // Allocation weights are fractions of PRE-trade NAV; the replay's impact
            // cost (above the allocation's planning fee) only shrinks post-trade NAV.
            const auto w = allocation.posttrade_marked_dollars[i] / allocation.pretrade_nav;
            beta += world.beta[i] * w;
            sector[world.sector[i]] += w;
            gross += std::abs(w);
        }
        EXPECT_LE(std::abs(beta), kBetaTol + 1e-6) << allocation.decision_period;
        for (const auto net : sector) EXPECT_LE(std::abs(net), kSectorCap + 1e-6);
        EXPECT_LE(gross, 1.0 + 1e-6);
    }
    // The book actually took risk: some allocation is well away from flat.
    atx::f64 max_gross = 0.0;
    for (const auto &a : run->allocations) {
        max_gross = std::max(max_gross, a.certificate.postfee_gross);
        EXPECT_LE(std::abs(a.certificate.postfee_beta_exposure), kBetaTol + 1e-8);
        EXPECT_LE(a.certificate.postfee_max_sector_net, kSectorCap + 1e-8);
    }
    EXPECT_GT(max_gross, 0.5);
}

TEST(ZooToBookE2E, NetOfCostPnlIsBelowGross) {
    const auto world = make_world();
    const auto cost = book::SqrtImpactCost::create({0.8, 0.5}, 0.1).value();
    const auto run = run_book(world, kDates, cost);
    ASSERT_TRUE(run) << run.error().message();
    atx::f64 gross = 0.0;
    atx::f64 costs = 0.0;
    for (const auto &row : run->replay.replay.intervals) {
        gross += row.gross_pnl;
        costs += row.trade_cost;
    }
    const auto net = run->replay.replay.final_nav - kNav;
    EXPECT_GT(costs, 0.0);
    EXPECT_LT(net, gross);
    EXPECT_NEAR(net, gross - costs, 1e-6 * kNav);
}

TEST(ZooToBookE2E, TruncationInvariance) {
    const auto world = make_world();
    const auto cost = book::SqrtImpactCost::create({0.8, 0.5}, 0.1).value();
    const auto full = run_book(world, kDates, cost);
    const auto truncated = run_book(world, 120, cost);
    ASSERT_TRUE(full) << full.error().message();
    ASSERT_TRUE(truncated) << truncated.error().message();
    const auto &a = truncated->replay.replay.intervals;
    const auto &b = full->replay.replay.intervals;
    ASSERT_EQ(a.size(), 119U);
    for (atx::usize k = 0; k < a.size(); ++k) {
        EXPECT_TRUE(same_bits(a[k].nav, b[k].nav)) << k;
        EXPECT_TRUE(same_bits(a[k].trade_cost, b[k].trade_cost)) << k;
        EXPECT_TRUE(same_bits(a[k].gross_pnl, b[k].gross_pnl)) << k;
    }
}
