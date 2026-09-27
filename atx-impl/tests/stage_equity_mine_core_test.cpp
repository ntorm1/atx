// Lane 9 equity-mine: span stitching, as-of membership, and the honest
// delay-1 rank long/short scorer, pinned against hand-computed values.

#include <array>
#include <cmath>
#include <span>
#include <utility>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "atx/core/error.hpp"
#include "atx/engine/alpha/panel.hpp"
#include "atx/engine/alpha/registry.hpp"
#include "atx/engine/alpha/streams.hpp"
#include "atx/engine/cost/cost_surface.hpp"
#include "atx/engine/eval/trial_registry.hpp"
#include "atx/engine/factory/execution_objective.hpp"
#include "atx/engine/loop/weight_policy.hpp"
#include "atx/engine/data/point_in_time_universe.hpp"

#include "stage_equity_mine.hpp"

namespace atx_test_l9_realmine_core {

using atx::engine::alpha::Panel;
namespace mine = atx::impl::mine;

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

// dates x instruments panel with a single "close" field; universe all-valid.
Panel close_panel(std::size_t dates, std::size_t inst, const std::vector<double> &close) {
    std::vector<std::vector<double>> cols{close};
    auto p = Panel::create(dates, inst, {"close"}, std::move(cols),
                           std::vector<std::uint8_t>(dates * inst, 1));
    EXPECT_TRUE(p.has_value());
    return std::move(p).value();
}

// close_i(t) grows by 1%*(i+1) per date, so r_i(t) = 0.01*(i+1) for t >= 1.
std::vector<double> growth_close(std::size_t dates, std::size_t inst) {
    std::vector<double> c(dates * inst);
    for (std::size_t i = 0; i < inst; ++i) {
        double px = 100.0;
        for (std::size_t d = 0; d < dates; ++d) {
            if (d > 0) px *= 1.0 + 0.01 * static_cast<double>(i + 1);
            c[d * inst + i] = px;
        }
    }
    return c;
}

std::vector<double> index_signal(std::size_t dates, std::size_t inst) {
    std::vector<double> s(dates * inst);
    for (std::size_t d = 0; d < dates; ++d)
        for (std::size_t i = 0; i < inst; ++i) s[d * inst + i] = static_cast<double>(i);
    return s;
}

mine::ScoreCfg small_cfg() {
    mine::ScoreCfg cfg;
    cfg.cost_bps = 10.0;
    cfg.delay = 1;
    cfg.min_names = 2;
    cfg.ic_horizons = {1, 2, 3};
    cfg.nw_lags = 1;
    return cfg;
}

TEST(EquityMineScore, RankLongShortDelayOnePnlAndCost) {
    constexpr std::size_t D = 6, I = 4;
    const Panel p = close_panel(D, I, growth_close(D, I));
    const std::vector<std::uint8_t> member(D * I, 1);
    auto s = mine::score_signal(index_signal(D, I), 1.0, p, 0, member, {0, D}, small_cfg());
    ASSERT_TRUE(s.has_value()) << s.error().message();
    ASSERT_EQ(s->gross.size(), D);
    // w = (-0.375, -0.125, 0.125, 0.375); r = 0.01*(1..4) -> 0.0125 per traded date.
    for (std::size_t d = 0; d < 4; ++d) EXPECT_NEAR(s->gross[d], 0.0125, 1e-12) << d;
    // d = 4, 5: the realized return (d + 2) is beyond the panel -> unscorable, flat.
    EXPECT_EQ(s->gross[4], 0.0);
    EXPECT_EQ(s->gross[5], 0.0);
    EXPECT_NEAR(s->turnover[0], 1.0, 1e-12); // entry from cash
    EXPECT_NEAR(s->turnover[1], 0.0, 1e-12);
    EXPECT_NEAR(s->net[0], 0.0125 - 1e-3, 1e-12);
    EXPECT_NEAR(s->net[1], 0.0125, 1e-12);
    EXPECT_NEAR(s->coverage, 4.0 / 6.0, 1e-12);
    EXPECT_NEAR(s->ic_mean[0], 1.0, 1e-12);
    EXPECT_NEAR(s->mean_names, 4.0, 1e-12);
}

TEST(EquityMineScore, MembershipExcludesNamesFromTheBook) {
    constexpr std::size_t D = 6, I = 4;
    const Panel p = close_panel(D, I, growth_close(D, I));
    std::vector<std::uint8_t> member(D * I, 1);
    for (std::size_t d = 0; d < D; ++d) member[d * I + 3] = 0;
    auto s = mine::score_signal(index_signal(D, I), 1.0, p, 0, member, {0, D}, small_cfg());
    ASSERT_TRUE(s.has_value());
    // three names: w = (-0.5, 0, 0.5) -> 0.01*(3 - 1)*0.5 = 0.01.
    EXPECT_NEAR(s->gross[0], 0.01, 1e-12);
    EXPECT_NEAR(s->mean_names, 3.0, 1e-12);
}

TEST(EquityMineScore, MissingRealizedReturnContributesZeroNotExclusion) {
    constexpr std::size_t D = 6, I = 4;
    auto close = growth_close(D, I);
    close[2 * I + 3] = kNaN; // r_3 undefined at t = 2 and t = 3
    const Panel p = close_panel(D, I, close);
    const std::vector<std::uint8_t> member(D * I, 1);
    auto s = mine::score_signal(index_signal(D, I), 1.0, p, 0, member, {0, D}, small_cfg());
    ASSERT_TRUE(s.has_value());
    // Signal date 0 realizes r(2): name 3 has NaN return -> contributes 0.
    EXPECT_NEAR(s->gross[0], 0.01 * (-0.375 * 1 - 0.125 * 2 + 0.125 * 3), 1e-12);
    // Signal date 2: close NaN at d -> name 3 ineligible; 3-name book.
    EXPECT_NEAR(s->gross[2], 0.01, 1e-12);
}

TEST(EquityMineScore, TooFewNamesIsFlat) {
    constexpr std::size_t D = 6, I = 4;
    const Panel p = close_panel(D, I, growth_close(D, I));
    const std::vector<std::uint8_t> member(D * I, 1);
    auto cfg = small_cfg();
    cfg.min_names = 5;
    auto s = mine::score_signal(index_signal(D, I), 1.0, p, 0, member, {0, D}, cfg);
    ASSERT_TRUE(s.has_value());
    for (double g : s->gross) EXPECT_EQ(g, 0.0);
    EXPECT_EQ(s->coverage, 0.0);
    EXPECT_EQ(s->sharpe_net, 0.0);
}

TEST(EquityMineScore, FlipNegatesGrossKeepsCost) {
    constexpr std::size_t D = 6, I = 4;
    const Panel p = close_panel(D, I, growth_close(D, I));
    const std::vector<std::uint8_t> member(D * I, 1);
    const auto cfg = small_cfg();
    auto pos = mine::score_signal(index_signal(D, I), 1.0, p, 0, member, {0, D}, cfg);
    auto neg = mine::score_signal(index_signal(D, I), -1.0, p, 0, member, {0, D}, cfg);
    ASSERT_TRUE(pos.has_value() && neg.has_value());
    const auto flipped = mine::flip_score(*pos, cfg);
    for (std::size_t d = 0; d < D; ++d) {
        EXPECT_NEAR(flipped.gross[d], neg->gross[d], 1e-15);
        EXPECT_NEAR(flipped.net[d], neg->net[d], 1e-15);
    }
    EXPECT_NEAR(flipped.ic_mean[0], -1.0, 1e-12);
    EXPECT_NEAR(flipped.sharpe_net, neg->sharpe_net, 1e-12);
}

TEST(EquityMineScore, RejectsShapeMismatch) {
    const Panel p = close_panel(3, 2, std::vector<double>(6, 1.0));
    const std::vector<std::uint8_t> member(6, 1);
    auto s = mine::score_signal(std::vector<double>(5, 0.0), 1.0, p, 0, member, {0, 3},
                                small_cfg());
    EXPECT_FALSE(s.has_value());
    auto w = mine::score_signal(std::vector<double>(6, 0.0), 1.0, p, 0, member, {0, 4},
                                small_cfg());
    EXPECT_FALSE(w.has_value());
}

// --- realized-return guard ----------------------------------------------------

// close (adjusted) + raw_close panel; universe all-valid.
Panel adj_raw_panel(std::size_t dates, std::size_t inst, const std::vector<double> &adj,
                    const std::vector<double> &raw) {
    std::vector<std::vector<double>> cols{adj, raw};
    auto p = Panel::create(dates, inst, {"close", "raw_close"}, std::move(cols),
                           std::vector<std::uint8_t>(dates * inst, 1));
    EXPECT_TRUE(p.has_value());
    return std::move(p).value();
}

TEST(EquityMineGuard, PlantedAdjustmentBreakIsExcludedFromPnlAndIc) {
    constexpr std::size_t D = 6, I = 4;
    const auto raw = growth_close(D, I);
    auto adj = raw;
    // Adjustment break: name 3's adjusted close jumps x3 on session 2 while its
    // raw close moves its ordinary +4% (the real 69872 2017-03-15 pattern).
    for (std::size_t d = 2; d < D; ++d) adj[d * I + 3] *= 3.0;
    const Panel p = adj_raw_panel(D, I, adj, raw);
    const std::vector<std::uint8_t> member(D * I, 1);
    auto cfg = small_cfg();

    auto g = mine::build_return_guard(p, 0, cfg);
    ASSERT_TRUE(g.has_value()) << g.error().message();
    EXPECT_TRUE(g->has_raw);
    ASSERT_EQ(g->excluded.size(), 1u);
    EXPECT_EQ(g->excluded[0].date, 2u);
    EXPECT_EQ(g->excluded[0].inst, 3u);
    EXPECT_FALSE(g->excluded[0].cap); // below the cap: caught by the raw comparison
    EXPECT_NEAR(g->excluded[0].adj_return, 1.04 * 3.0 - 1.0, 1e-12);
    EXPECT_NEAR(g->excluded[0].raw_return, 0.04, 1e-12);
    EXPECT_EQ(g->count_in(0, D), 1u);
    EXPECT_EQ(g->count_in(3, D), 0u);

    auto guarded = mine::score_signal(index_signal(D, I), 1.0, p, 0, member, {0, D}, cfg);
    ASSERT_TRUE(guarded.has_value()) << guarded.error().message();
    // Signal date 0 realizes session 1 -> 2: name 3 (w = +0.375) is dropped.
    EXPECT_NEAR(guarded->gross[0], 0.01 * (-0.375 * 1 - 0.125 * 2 + 0.125 * 3), 1e-12);
    EXPECT_NEAR(guarded->gross[1], 0.0125, 1e-12); // later dates are ordinary
    EXPECT_EQ(guarded->excluded_returns, 1u);

    cfg.guard_returns = false;
    auto naive = mine::score_signal(index_signal(D, I), 1.0, p, 0, member, {0, D}, cfg);
    ASSERT_TRUE(naive.has_value());
    EXPECT_NEAR(naive->gross[0],
                0.01 * (-0.375 * 1 - 0.125 * 2 + 0.125 * 3) + 0.375 * (1.04 * 3.0 - 1.0), 1e-12);
    EXPECT_EQ(naive->excluded_returns, 0u);
    EXPECT_GT(naive->gross[0], guarded->gross[0] + 0.5);
}

TEST(EquityMineGuard, RawSplitIsKeptAndCapExcludesAgreeingJumps) {
    constexpr std::size_t D = 6, I = 4;
    auto adj = growth_close(D, I);
    auto raw = adj;
    // A real 2:1 split of name 2 on session 3: raw halves, adjusted is smooth.
    for (std::size_t d = 3; d < D; ++d) raw[d * I + 2] *= 0.5;
    // Name 1 jumps x10 on session 4 in BOTH series: consistent, but beyond the
    // declared |log| cap (ln 10 > 1.5).
    for (std::size_t d = 4; d < D; ++d) {
        adj[d * I + 1] *= 10.0;
        raw[d * I + 1] *= 10.0;
    }
    const Panel p = adj_raw_panel(D, I, adj, raw);
    auto g = mine::build_return_guard(p, 0, small_cfg());
    ASSERT_TRUE(g.has_value());
    ASSERT_EQ(g->excluded.size(), 1u);
    EXPECT_EQ(g->excluded[0].date, 4u);
    EXPECT_EQ(g->excluded[0].inst, 1u);
    EXPECT_TRUE(g->excluded[0].cap);
    // A multi-day return spanning session 4 is excluded as well (IC horizons).
    EXPECT_TRUE(g->bad(2, 5, 1));
    EXPECT_FALSE(g->bad(2, 3, 1));
    EXPECT_FALSE(g->bad(2, 5, 2)); // the split day is not a break

    // Without a raw column only the cap applies.
    const Panel only_adj = close_panel(D, I, adj);
    auto g2 = mine::build_return_guard(only_adj, 0, small_cfg());
    ASSERT_TRUE(g2.has_value());
    EXPECT_FALSE(g2->has_raw);
    EXPECT_EQ(g2->excluded.size(), 1u);
}

// --- stitching ---------------------------------------------------------------

mine::SpanSource source(std::vector<std::int64_t> keys, std::vector<std::int64_t> ids,
                        double base) {
    const std::size_t D = keys.size(), I = ids.size();
    std::vector<double> c(D * I);
    for (std::size_t d = 0; d < D; ++d)
        for (std::size_t i = 0; i < I; ++i)
            c[d * I + i] = base + static_cast<double>(keys[d]) * 10.0 + static_cast<double>(ids[i]);
    mine::SpanSource s{close_panel(D, I, c), std::move(keys), std::move(ids)};
    return s;
}

TEST(EquityMineStitch, PrefersEarlierSourceAndFillsWarmupFromLater) {
    std::vector<mine::SpanSource> src;
    src.push_back(source({1, 2, 3}, {10, 20}, 0.0));
    src.push_back(source({2, 3, 4, 5}, {20, 30}, 1000.0)); // disagrees on overlap
    auto span = mine::stitch_span(src);
    ASSERT_TRUE(span.has_value()) << span.error().message();
    EXPECT_EQ(span->session_keys, (std::vector<std::int64_t>{1, 2, 3, 4, 5}));
    EXPECT_EQ(span->instrument_ids, (std::vector<std::int64_t>{10, 20, 30}));
    const auto close = span->panel.field_all(0);
    auto at = [&](std::size_t d, std::size_t i) { return close[d * 3 + i]; };
    EXPECT_EQ(at(0, 0), 20.0);         // key 1, id 10 from A
    EXPECT_TRUE(std::isnan(at(0, 2))); // key 1, id 30: nobody has it
    EXPECT_FALSE(span->panel.in_universe(0, 2));
    EXPECT_EQ(at(1, 1), 40.0);         // key 2, id 20: A preferred over B
    EXPECT_EQ(at(1, 2), 1050.0);       // key 2, id 30: warmup from B
    EXPECT_EQ(at(3, 1), 1060.0);       // key 4, id 20: only B covers key 4
    EXPECT_TRUE(std::isnan(at(3, 0))); // id 10 absent after A ends
    EXPECT_EQ(span->owner_source, (std::vector<std::uint32_t>{0, 0, 0, 1, 1}));
    EXPECT_EQ(span->overlap_cells, 2u);          // (2,20), (3,20)
    EXPECT_EQ(span->overlap_mismatch_cells, 2u); // base differs
    const auto mask = mine::owner_union_mask(*span, src);
    EXPECT_EQ(mask[0 * 3 + 0], 1);
    EXPECT_EQ(mask[0 * 3 + 2], 0); // id 30 not in A's axes
    EXPECT_EQ(mask[3 * 3 + 0], 0); // id 10 not in B's axes
    EXPECT_EQ(mask[3 * 3 + 2], 1);
}

TEST(EquityMineStitch, RejectsFieldMismatchAndEmpty) {
    std::vector<mine::SpanSource> none;
    EXPECT_FALSE(mine::stitch_span(none).has_value());
    std::vector<mine::SpanSource> src;
    src.push_back(source({1, 2}, {10}, 0.0));
    auto p = Panel::create(2, 1, {"open"}, {{1.0, 2.0}}, std::vector<std::uint8_t>(2, 1));
    ASSERT_TRUE(p.has_value());
    src.push_back(mine::SpanSource{std::move(p).value(), {3, 4}, {10}});
    EXPECT_FALSE(mine::stitch_span(src).has_value());
}

// --- as-of membership --------------------------------------------------------

TEST(EquityMineMembership, AsOfUsesLastEffectiveRebalance) {
    atx::engine::data::PitMembershipImage img{};
    img.top_n = {1000};
    img.band_bp = {0};
    atx::engine::data::PitMembershipRebalance r1{};
    r1.rank_session_key = 1;
    r1.effective_session_key = 2;
    r1.cuts = {atx::engine::data::PitMembershipCut{{10}, {1}}};
    atx::engine::data::PitMembershipRebalance r2{};
    r2.rank_session_key = 3;
    r2.effective_session_key = 4;
    r2.cuts = {atx::engine::data::PitMembershipCut{{20, 30}, {1, 2}}};
    img.rebalances = {r1, r2};
    const std::vector<std::int64_t> keys{1, 2, 3, 4, 5};
    const std::vector<std::int64_t> ids{10, 20, 30};
    auto m = mine::asof_membership_mask(img, 0, keys, ids);
    ASSERT_TRUE(m.has_value()) << m.error().message();
    const std::vector<std::uint8_t> want{0, 0, 0, 1, 0, 0, 1, 0, 0, 0, 1, 1, 0, 1, 1};
    EXPECT_EQ(*m, want);
    EXPECT_FALSE(mine::asof_membership_mask(img, 1, keys, ids).has_value());
}

// --- parsing -----------------------------------------------------------------

TEST(EquityMineParse, FixtureLinesAndDates) {
    const auto seeds = mine::parse_fixture_seeds("# c\n\n3: rank(close)\r\n12: -1 * volume\n");
    ASSERT_EQ(seeds.size(), 2u);
    EXPECT_EQ(seeds[0].dsl, "rank(close)");
    EXPECT_EQ(seeds[0].origin, "wq101:3");
    EXPECT_EQ(seeds[1].origin, "wq101:12");
    auto ns = mine::parse_iso_date_ns("2020-01-01");
    ASSERT_TRUE(ns.has_value());
    EXPECT_EQ(*ns, 1577836800LL * 1000000000LL);
    EXPECT_FALSE(mine::parse_iso_date_ns("2020-13-01").has_value());
    EXPECT_FALSE(mine::parse_iso_date_ns("20200101").has_value());
    EXPECT_FALSE(mine::literature_seeds().empty());
}

namespace factory = atx::engine::factory;
namespace cost = atx::engine::cost;
namespace eval = atx::engine::eval;

// Synthetic, declared input recipe only: no measured liquidity or locate claim.
atx::core::Result<factory::ExecutionObjectiveContext> execution_context(
    const Panel &panel, std::span<const atx::u8> member, mine::EvalWindow window,
    const mine::ScoreCfg &score, char source = 'a', bool unpriced_first = false) {
    factory::ExecutionObjectiveConfig cfg;
    cfg.rule = factory::ExecutionObjectiveRule::DelayedSurfaceV2;
    cfg.delay = score.delay;
    cfg.min_names = score.min_names;
    cfg.guard_returns = score.guard_returns;
    cfg.window_begin = window.begin;
    cfg.window_end = cfg.maturity_end = window.end;
    cfg.initial_nav = 1000.0;
    cfg.max_working_bytes = 4U * 1024U * 1024U;
    std::vector<atx::i64> marks(panel.dates()), decisions(panel.dates());
    std::vector<atx::u64> ids(panel.instruments());
    for (atx::usize d = 0; d < panel.dates(); ++d) {
        marks[d] = 1'000'000'000LL + static_cast<atx::i64>(d) * 86'400'000'000'000LL;
        decisions[d] = marks[d] + 1;
    }
    for (atx::usize i = 0; i < ids.size(); ++i) ids[i] = 100U + i;
    cost::CostSurfaceRecipe recipe;
    recipe.rule = cost::CostSurfaceRule::ModeledInputsV2;
    recipe.impact_y = 0.0;
    recipe.commission_bps = 0.1;
    recipe.max_participation = 1.0;
    std::vector<cost::CostSurface> snapshots;
    for (atx::usize d = window.begin; d < window.end - score.delay - 1U; ++d) {
        std::vector<cost::CostSurfaceRow> rows(ids.size());
        for (atx::usize i = 0; i < rows.size(); ++i) {
            auto &r = rows[i];
            r.instrument_id = ids[i];
            r.state = unpriced_first && d == window.begin && i == 0
                ? cost::CostInputState::Unavailable : cost::CostInputState::Available;
            r.available_at_ns = marks[d] - 1;
            r.adv_dollars = 1e8;
            r.daily_vol = 0.02;
            r.full_spread = 0.0;
            r.borrow_state = cost::CostInputState::Available;
            r.borrow_available_at_ns = marks[d] - 1;
            r.borrow_annual_fraction = 0.02 + 0.03 * static_cast<double>(i);
        }
        cost::CostSurfaceIdentity id;
        id.decision_time_ns = decisions[d];
        id.source_sha256 = std::string(64, source);
        id.liquidity_recipe = "synthetic-constant-adv-vol-spread-v1";
        id.calibration_identity = "synthetic-uncalibrated-v1";
        ATX_TRY(auto snapshot, cost::CostSurface::create(recipe, id, rows));
        snapshots.push_back(std::move(snapshot));
    }
    atx::engine::WeightPolicy policy;
    policy.winsorize_limit = 0.0;
    const factory::ExecutionObjectiveIdentity identity{
        std::string(64, source), "synthetic-mine", "synthetic-total-return-close-v1"};
    return factory::prepare_execution_objective(panel, policy, cfg, snapshots, marks,
                                                decisions, ids, identity, member);
}

TEST(EquityMineExecution, V2UsesMatureKernelValuesAndRejectsIncompatibleSupport) {
    constexpr atx::usize D = 14, I = 4;
    const auto px = growth_close(D, I);
    const Panel panel = close_panel(D, I, px);
    const std::vector<atx::u8> member(panel.cells(), 1);
    const auto signal = index_signal(D, I);
    auto cfg = small_cfg();
    cfg.guard_returns = false;
    cfg.execution_rule = factory::ExecutionObjectiveRule::DelayedSurfaceV2;
    const mine::EvalWindow window{1, 11};
    auto context = execution_context(panel, member, window, cfg);
    ASSERT_TRUE(context.has_value()) << context.error().message();
    auto core = factory::extract_execution_signal(signal, *context);
    ASSERT_TRUE(core.has_value()) << core.error().message();
    auto score = mine::score_signal(signal, 1.0, panel, 0, member, window, cfg, nullptr, &*context);
    ASSERT_TRUE(score.has_value()) << score.error().message();
    ASSERT_EQ(score->net.size(), 8U);
    EXPECT_EQ(score->realization_begin, 3U);
    EXPECT_EQ(score->realization_end, 11U);
    EXPECT_EQ(score->execution_context_sha256, context->identity_sha256());
    for (atx::usize t = 0; t < score->net.size(); ++t) {
        EXPECT_DOUBLE_EQ(score->net[t], core->pnl_flat[t + 3]);
        EXPECT_DOUBLE_EQ(score->gross[t], core->gross_flat[t + 3]);
        EXPECT_DOUBLE_EQ(score->turnover[t], core->turnover_flat[t + 3]);
    }
    auto negative = mine::score_signal(signal, -1.0, panel, 0, member, window, cfg, nullptr, &*context);
    auto negative_core = factory::extract_execution_signal(signal, *context, -1.0);
    ASSERT_TRUE(negative.has_value()); ASSERT_TRUE(negative_core.has_value());
    EXPECT_DOUBLE_EQ(negative->net[0], negative_core->pnl_flat[3]);
    EXPECT_NE(negative->total_borrow_return, score->total_borrow_return);
    EXPECT_FALSE(mine::score_signal(signal, 1.0, panel, 0, member, window, cfg).has_value());
    auto different_member = member;
    different_member[window.begin * I] = 0;
    EXPECT_FALSE(mine::score_signal(signal, 1.0, panel, 0, different_member, window,
                                   cfg, nullptr, &*context).has_value());
    EXPECT_FALSE(mine::score_signal(signal, 1.0, panel, 0, member, {1, 10},
                                   cfg, nullptr, &*context).has_value());
    auto bad_context = execution_context(panel, member, window, cfg, 'a', true);
    ASSERT_TRUE(bad_context.has_value()) << bad_context.error().message();
    EXPECT_FALSE(mine::score_signal(signal, 1.0, panel, 0, member, window,
                                   cfg, nullptr, &*bad_context).has_value());
    auto future = px;
    for (atx::usize d = window.end; d < D; ++d)
        for (atx::usize i = 0; i < I; ++i) future[d * I + i] *= 5.0 + static_cast<double>(i);
    const Panel mutated = close_panel(D, I, future);
    auto future_context = execution_context(mutated, member, window, cfg);
    ASSERT_TRUE(future_context.has_value());
    auto future_score = mine::score_signal(signal, 1.0, mutated, 0, member, window,
                                           cfg, nullptr, &*future_context);
    ASSERT_TRUE(future_score.has_value());
    EXPECT_EQ(future_score->net, score->net);
    EXPECT_EQ(future_score->ic_mean, score->ic_mean);
}

TEST(EquityMineExecution, ActualMineRolesRescoreSignAndBindMatureTrialIdentity) {
    constexpr atx::usize D = 36, I = 4;
    const Panel panel = close_panel(D, I, growth_close(D, I));
    const std::vector<atx::u8> member(panel.cells(), 1);
    mine::MineConfig cfg;
    cfg.run_search = false;
    cfg.search.ic_screen.rule = factory::IcScreenRule::DisabledV1;
    cfg.score = small_cfg();
    cfg.score.guard_returns = false;
    cfg.score.execution_rule = factory::ExecutionObjectiveRule::DelayedSurfaceV2;
    cfg.min_train_sharpe = -1e6;
    cfg.min_coverage = 0.0;
    cfg.boot.n_boot = 16;
    cfg.boot.mean_block = 2.0;
    const mine::EvalWindow train_window{1, 19}, validation_window{20, 35};
    auto train_context = execution_context(panel, member, train_window, cfg.score);
    auto validation_context = execution_context(panel, member, validation_window, cfg.score);
    ASSERT_TRUE(train_context.has_value()); ASSERT_TRUE(validation_context.has_value());
    mine::MineData train{&panel, member, train_window}; train.execution = &*train_context;
    mine::MineData validation{&panel, member, validation_window};
    validation.execution = &*validation_context;
    eval::TrialRegistryConfig registry_cfg;
    registry_cfg.pnl_len = train_window.size();
    registry_cfg.sketch_dim = 32;
    auto registry = eval::TrialRegistry::in_memory(registry_cfg);
    ASSERT_TRUE(registry.has_value());
    const atx::engine::alpha::Library library;
    const std::array<mine::SeedExpr, 1> seeds{{{"-1 * rank(close)", "extra"}}};
    auto outcome = mine::mine_train(library, train, seeds, cfg, *registry);
    ASSERT_TRUE(outcome.has_value()) << outcome.error().message();
    ASSERT_EQ(outcome->candidates.size(), 1U);
    const auto &row = outcome->candidates[0];
    ASSERT_TRUE(row.scored) << row.error;
    EXPECT_EQ(row.sign, -1.0);
    auto positive = mine::score_signal(index_signal(D, I), 1.0, panel, 0, member,
                                       train_window, cfg.score, nullptr, &*train_context);
    ASSERT_TRUE(positive.has_value());
    EXPECT_EQ(row.train.net, positive->net); // rerun selected orientation, including borrow/NAV
    ASSERT_EQ(registry->trials().size(), 1U);
    EXPECT_EQ(registry->trials()[0].meta.window_start, 2U);
    EXPECT_EQ(registry->trials()[0].meta.window_end, train_window.size() - 1U);
    ASSERT_EQ(outcome->family.size(), 1U);
    auto validated = mine::mine_validate(library, validation, cfg, *outcome);
    ASSERT_TRUE(validated.has_value()) << validated.error().message();
    EXPECT_EQ(outcome->validation_execution_context_sha256, validation_context->identity_sha256());
    EXPECT_EQ(outcome->candidates[0].validation.net.size(), validation_window.size() - 2U);
    const std::span<const mine::CandidateRow> one{outcome->candidates.data(), 1};
    auto holdout = mine::evaluate_holdout(library, train, one, cfg.score);
    ASSERT_TRUE(holdout.has_value()) << holdout.error().message();
    ASSERT_EQ(holdout->size(), 2U);
    EXPECT_EQ((*holdout)[0].score.net, row.train.net);
    EXPECT_EQ((*holdout)[1].score.net, row.train.net);
    auto blend = mine::evaluate_blend(library, train, one, cfg.score);
    ASSERT_TRUE(blend.has_value()); EXPECT_EQ(blend->net, row.train.net);
    auto changed_context = execution_context(panel, member, train_window, cfg.score, 'b');
    ASSERT_TRUE(changed_context.has_value());
    train.execution = &*changed_context;
    auto changed_registry = eval::TrialRegistry::in_memory(registry_cfg);
    ASSERT_TRUE(changed_registry.has_value());
    auto changed = mine::mine_train(library, train, seeds, cfg, *changed_registry);
    ASSERT_TRUE(changed.has_value());
    EXPECT_NE(changed->candidates[0].config_hash, row.config_hash);
    train.execution = nullptr;
    EXPECT_FALSE(mine::mine_train(library, train, seeds, cfg, *registry).has_value());
    EXPECT_FALSE(mine::evaluate_holdout(library, train, {}, cfg.score).has_value());
}

} // namespace atx_test_l9_realmine_core
