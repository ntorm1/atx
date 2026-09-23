// Lane 9 equity-mine: span stitching, as-of membership, and the honest
// delay-1 rank long/short scorer, pinned against hand-computed values.

#include <cmath>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "atx/engine/alpha/panel.hpp"
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

} // namespace atx_test_l9_realmine_core
