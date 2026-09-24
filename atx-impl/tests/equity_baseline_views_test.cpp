#include "equity_baseline_views.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "atx/engine/alpha/streams.hpp"
#include "atx/engine/book/replay.hpp"
#include "atx/engine/loop/weight_policy.hpp"
#include "research_sim.hpp"

namespace {
namespace impl = atx::impl;
namespace alpha = atx::engine::alpha;
namespace book = atx::engine::book;
constexpr atx::i64 kDay = 86'400'000'000'000LL;

atx::f64 price(atx::usize date, atx::usize instrument) {
    const auto t = static_cast<atx::f64>(date);
    return instrument == 1 ? 500.0 - 0.25 * t
                           : 100.0 + static_cast<atx::f64>(instrument + 1) * t;
}

std::vector<atx::f64> closes(atx::usize dates, atx::usize instruments) {
    std::vector<atx::f64> result(dates * instruments);
    for (atx::usize d = 0; d < dates; ++d) {
        for (atx::usize i = 0; i < instruments; ++i) {
            result[d * instruments + i] = price(d, i);
        }
    }
    return result;
}

impl::PanelArtifact context(atx::usize dates = 264, atx::usize instruments = 3,
                            std::vector<atx::f64> close = {},
                            std::vector<atx::f64> observed = {}) {
    if (close.empty()) close = closes(dates, instruments);
    std::vector<atx::f64> raw = close;
    for (auto &value : raw) value *= 2.0;
    std::vector<atx::f64> earn_flag(dates * instruments, 0.0);
    std::vector<atx::f64> iv_21(dates * instruments);
    std::vector<atx::f64> iv_126(dates * instruments);
    std::vector<atx::f64> sector(dates * instruments);
    for (atx::usize d = 0; d < dates; ++d) {
        for (atx::usize i = 0; i < instruments; ++i) {
            const auto cell = d * instruments + i;
            earn_flag[cell] = d % 63 == 10 ? 1.0 : 0.0;
            iv_21[cell] = 0.30 + 0.01 * static_cast<atx::f64>(i);
            iv_126[cell] = 0.28 + 0.01 * static_cast<atx::f64>(i);
            sector[cell] = static_cast<atx::f64>(i % 2 + 10);
        }
    }
    // Checkpoint 21 OHLC-range families read adjusted high/low/open;
    // checkpoint 22 short-interest families read si_shares and market_cap
    // (any positive finite).
    std::vector<atx::f64> si_shares = close;
    for (auto &value : si_shares) value *= 1e6;
    std::vector<atx::f64> market_cap = raw;
    for (auto &value : market_cap) value *= 1e8;
    std::vector<atx::f64> high = close;
    std::vector<atx::f64> low = close;
    std::vector<atx::f64> open = close;
    for (atx::usize cell = 0; cell < close.size(); ++cell) {
        high[cell] = close[cell] * 1.01;
        low[cell] = close[cell] * 0.98;
        open[cell] = close[cell] * 0.995;
    }
    std::vector<std::string> fields{"close",       "raw_close",    "volume", "earnFlag",
                                    "atmCenI_21d", "atmCenI_126d", "sector", "high",
                                    "low",         "open",         "si_shares",    "market_cap"};
    std::vector<std::vector<atx::f64>> columns{
        std::move(close), std::move(raw), std::vector<atx::f64>(dates * instruments, 1e6),
        std::move(earn_flag), std::move(iv_21), std::move(iv_126), std::move(sector),
        std::move(high), std::move(low), std::move(open), std::move(si_shares),
        std::move(market_cap)};
    if (!observed.empty()) {
        fields.push_back("observed");
        columns.push_back(std::move(observed));
    }
    std::vector<atx::u8> eligible(dates * instruments, 0);
    for (atx::usize d = 256; d < dates; ++d) {
        for (atx::usize i = 0; i < instruments; ++i) {
            eligible[d * instruments + i] = (i < 2 || d >= 260) ? 1U : 0U;
        }
    }
    auto panel = alpha::Panel::create(dates, instruments, std::move(fields), std::move(columns),
                                      std::move(eligible)).value();
    impl::PanelIdentity identity;
    identity.instrument_namespace = "synthetic.securityID";
    identity.recipe = "fixture: pointwise adjusted close; raw reported volume; unverified economics";
    identity.parents = {{"fixture", std::string(64, 'a')}};
    for (atx::usize d = 0; d < dates; ++d) {
        identity.session_keys.push_back(static_cast<atx::i64>(d) * kDay);
    }
    for (atx::usize i = 0; i < instruments; ++i) {
        identity.instrument_ids.push_back(std::to_string(1000 + i));
        identity.original_instrument_indices.push_back(10 + i);
    }
    return {std::move(panel), std::move(identity), std::string(64, 'b'), std::string(64, 'c')};
}

impl::EquityBaselineConfig config(atx::usize first = 256, atx::usize end = 264) {
    impl::EquityBaselineConfig result;
    result.evaluation = {static_cast<atx::i64>(first) * kDay,
                         static_cast<atx::i64>(end) * kDay};
    result.observation_basis =
        impl::EquityBaselineObservationBasis::ArchiveRawVolumeAndPointwiseAdjustedCloseV1;
    return result;
}

atx::f64 expected_signal(atx::usize date, atx::usize instrument, atx::usize lag) {
    atx::f64 sum = 0.0;
    for (atx::usize t = date - 4; t <= date; ++t) {
        sum += price(t - 21, instrument) / price(t - lag, instrument) - 1.0;
    }
    return sum / 5.0;
}

} // namespace

TEST(EquityBaselineViews, LateEntrantUsesObservedHistoryAndCommonReadiness) {
    const auto input = context();
    const auto result = impl::evaluate_equity_baseline(input, config());
    ASSERT_TRUE(result.has_value()) << result.error().message();
    EXPECT_EQ(result->signals.dates, 8U);
    ASSERT_EQ(result->signals.alphas.size(), 2U);
    EXPECT_EQ(result->signals.alphas[0].name, "momentum_252");
    EXPECT_EQ(result->signals.alphas[1].name, "momentum_126");
    EXPECT_EQ(result->observed_feature_cells, 264U * 3U);
    EXPECT_EQ(result->ready_evaluation_cells, 8U * 3U);
    EXPECT_EQ(result->admitted_evaluation_cells, 4U * 2U + 4U * 3U);
    for (atx::usize a = 0; a < 2; ++a) {
        const auto lag = a == 0 ? 252U : 126U;
        EXPECT_NEAR(result->signals.alpha_cross_section(a, 0)[0],
                    expected_signal(256, 0, lag), 1e-12);
        EXPECT_TRUE(std::isnan(result->signals.alpha_cross_section(a, 0)[2]));
        EXPECT_NEAR(result->signals.alpha_cross_section(a, 4)[2],
                    expected_signal(260, 2, lag), 1e-12);
    }
    EXPECT_FALSE(result->panel.in_universe(0, 2));
    EXPECT_TRUE(result->panel.in_universe(4, 2));
    EXPECT_EQ(result->ready_by_observation, std::vector<atx::usize>(8, 3));
    EXPECT_EQ(result->admitted_by_observation,
              (std::vector<atx::usize>{2, 2, 2, 2, 3, 3, 3, 3}));
}

TEST(EquityBaselineViews, FutureStorageMembersCannotEnterEarlierStaticUniverseWeights) {
    const auto input = context(264, 3);
    auto changed_values = closes(264, 4);
    for (atx::usize d = 260; d < 264; ++d) {
        for (atx::usize i = 0; i < 4; ++i) changed_values[d * 4 + i] *= 7.0;
    }
    const auto changed = context(264, 4, std::move(changed_values));
    const auto base = impl::evaluate_equity_baseline(input, config());
    const auto future = impl::evaluate_equity_baseline(changed, config());
    ASSERT_TRUE(base.has_value()) << base.error().message();
    ASSERT_TRUE(future.has_value()) << future.error().message();
    const atx::engine::WeightPolicy policy;
    const auto sim = impl::frictionless_sim();
    const auto base_streams = alpha::extract_streams(base->signals, policy, base->panel, sim);
    const auto future_streams = alpha::extract_streams(future->signals, policy, future->panel, sim);
    ASSERT_TRUE(base_streams.has_value());
    ASSERT_TRUE(future_streams.has_value());
    for (atx::usize a = 0; a < 2; ++a) {
        for (atx::usize d = 0; d < 4; ++d) {
            for (atx::usize i = 0; i < 2; ++i) {
                EXPECT_DOUBLE_EQ(base->signals.alpha_cross_section(a, d)[i],
                                 future->signals.alpha_cross_section(a, d)[i]);
                EXPECT_DOUBLE_EQ(base_streams->positions(a, d)[i],
                                 future_streams->positions(a, d)[i]);
            }
            EXPECT_DOUBLE_EQ(future_streams->positions(a, d)[2], 0.0);
            EXPECT_DOUBLE_EQ(future_streams->positions(a, d)[3], 0.0);
            EXPECT_TRUE(std::isnan(future->signals.alpha_cross_section(a, d)[3]));
        }
    }
    // The static Universe in extract_streams ignores Panel eligibility. This
    // counterfactual proves the explicit signal NaNs above are load-bearing.
    auto ungated = future->signals;
    ungated.alphas[0].values[3] = 1000.0;
    const auto unsafe = alpha::extract_streams(ungated, policy, future->panel, sim);
    ASSERT_TRUE(unsafe.has_value());
    EXPECT_NE(unsafe->positions(0, 0)[3], 0.0);
}

TEST(EquityBaselineViews, MissingLongHistoryBlocksBothSignalsUntilWindowRecovers) {
    auto values = closes(264, 3);
    values[3 * 3] = std::numeric_limits<atx::f64>::quiet_NaN();
    const auto input = context(264, 3, std::move(values));
    const auto result = impl::evaluate_equity_baseline(input, config());
    ASSERT_TRUE(result.has_value()) << result.error().message();
    for (atx::usize d = 0; d < 4; ++d) {
        EXPECT_FALSE(result->panel.in_universe(d, 0));
        EXPECT_TRUE(std::isnan(result->signals.alpha_cross_section(0, d)[0]));
        // The shorter signal has valid individual history but cannot trade alone.
        EXPECT_TRUE(std::isnan(result->signals.alpha_cross_section(1, d)[0]));
    }
    EXPECT_TRUE(result->panel.in_universe(4, 0));
    EXPECT_NEAR(result->signals.alpha_cross_section(0, 4)[0], expected_signal(260, 0, 252), 1e-12);
    EXPECT_NEAR(result->signals.alpha_cross_section(1, 4)[0], expected_signal(260, 0, 126), 1e-12);
    EXPECT_EQ(result->ready_evaluation_cells, 20U);
}

TEST(EquityBaselineViews, EvaluationSliceBorrowsExactPricesAndRebasesOrdersAndScoring) {
    const auto input = context(270, 3);
    const auto result = impl::evaluate_equity_baseline(input, config(260, 264));
    ASSERT_TRUE(result.has_value()) << result.error().message();
    EXPECT_EQ(result->plan.feature_begin, 4U);
    EXPECT_EQ(result->plan.first_evaluation_session_key, 260 * kDay);
    EXPECT_EQ(result->plan.last_evaluation_session_key, 263 * kDay);
    EXPECT_EQ(result->panel.dates(), 4U);
    EXPECT_EQ(result->panel.num_fields(), 12U);
    EXPECT_EQ(result->signals.dates, 4U);
    EXPECT_EQ(result->context_rows, (std::vector<atx::usize>{260, 261, 262, 263}));
    EXPECT_EQ(result->session_keys, (std::vector<atx::i64>{260 * kDay, 261 * kDay,
                                                         262 * kDay, 263 * kDay}));
    for (const auto field : {"close", "raw_close", "volume"}) {
        const auto source_id = input.panel.field_id(field).value();
        const auto slice_id = result->panel.field_id(field).value();
        EXPECT_EQ(result->panel.field_all(slice_id).data(),
                  input.panel.field_all(source_id).data() + 260 * 3);
    }
    const auto streams = alpha::extract_streams(result->signals, atx::engine::WeightPolicy{},
                                                result->panel, impl::frictionless_sim());
    ASSERT_TRUE(streams.has_value());
    EXPECT_EQ(streams->n_periods(), 4U);
    EXPECT_DOUBLE_EQ(streams->pnl(0)[0], 0.0);
    const auto first_target = streams->positions(0, 0);
    const std::vector<atx::usize> decisions{0};
    book::ReplayConfig replay_config;
    replay_config.initial_nav = 1000.0;
    const auto replay = book::replay_scheduled_targets(result->panel, result->session_keys,
                                                       decisions, first_target, replay_config);
    ASSERT_TRUE(replay.has_value()) << replay.error().message();
    ASSERT_EQ(replay->intervals.size(), 3U);
    EXPECT_DOUBLE_EQ(replay->intervals[0].pretrade_nav, 1000.0);
    EXPECT_FALSE(replay->intervals[0].decision_period.has_value());
    EXPECT_EQ(replay->intervals[1].decision_period, 0U);
    ASSERT_FALSE(replay->trades.empty());
    for (const auto &trade : replay->trades) EXPECT_EQ(trade.period, 1U);
}

TEST(EquityBaselineViews, InvalidDomainsAndInsufficientBudgetFailBeforeEvaluation) {
    const auto input = context();
    const auto planned = impl::plan_equity_baseline(input, config());
    ASSERT_TRUE(planned.has_value()) << planned.error().message();
    EXPECT_GT(planned->vm_slots, 0U);
    EXPECT_EQ(planned->vm_slot_bytes, planned->vm_slots * planned->feature_cells * sizeof(atx::f64));
    auto bounded = config();
    bounded.max_additional_bytes = planned->additional_array_bytes - 1;
    EXPECT_FALSE(impl::evaluate_equity_baseline(input, bounded).has_value());
    bounded.max_additional_bytes = planned->additional_array_bytes;
    EXPECT_TRUE(impl::plan_equity_baseline(input, bounded).has_value());
    EXPECT_FALSE(impl::plan_equity_baseline(input, config(255, 264)).has_value());
    EXPECT_FALSE(impl::plan_equity_baseline(input, config(264, 264)).has_value());
    auto missing_start = config();
    ++missing_start.evaluation.begin_session_key;
    EXPECT_FALSE(impl::plan_equity_baseline(input, missing_start).has_value());
    auto no_basis = config();
    no_basis.observation_basis = impl::EquityBaselineObservationBasis::Unspecified;
    EXPECT_FALSE(impl::plan_equity_baseline(input, no_basis).has_value());
    auto wrong_shape = input;
    wrong_shape.identity.session_keys.pop_back();
    EXPECT_FALSE(impl::plan_equity_baseline(wrong_shape, config()).has_value());
    auto duplicate = input;
    duplicate.identity.instrument_ids[1] = duplicate.identity.instrument_ids[0];
    EXPECT_FALSE(impl::plan_equity_baseline(duplicate, config()).has_value());
    auto wrong_field = input;
    wrong_field.panel = alpha::Panel::create(264, 3, {"close"}, {closes(264, 3)}, {}).value();
    EXPECT_FALSE(impl::plan_equity_baseline(wrong_field, config()).has_value());
}

TEST(EquityBaselineViews, ExplicitObservationMaskAndInvalidNumericClaimsAreChecked) {
    std::vector<atx::f64> observed(264 * 3, 1.0);
    observed[3 * 3] = 0.0;
    const auto hidden = context(264, 3, {}, observed);
    const auto result = impl::evaluate_equity_baseline(hidden, config());
    ASSERT_TRUE(result.has_value());
    EXPECT_FALSE(result->panel.in_universe(0, 0));
    EXPECT_TRUE(result->panel.in_universe(4, 0));
    observed[3 * 3] = std::numeric_limits<atx::f64>::quiet_NaN();
    const auto invalid_flag = context(264, 3, {}, observed);
    EXPECT_FALSE(impl::evaluate_equity_baseline(invalid_flag, config()).has_value());
    observed.assign(264 * 3, 1.0);
    auto values = closes(264, 3);
    values[3 * 3] = 0.0;
    const auto contradiction = context(264, 3, values, observed);
    EXPECT_FALSE(impl::evaluate_equity_baseline(contradiction, config()).has_value());
    values = closes(264, 3);
    values[256 * 3] = std::numeric_limits<atx::f64>::infinity();
    const auto eligible_gap = context(264, 3, std::move(values));
    EXPECT_FALSE(impl::evaluate_equity_baseline(eligible_gap, config()).has_value());
}

TEST(EquityBaselineViews, FamilyProgramCompilesDerivesWarmupAndGatesOnBaselineMask) {
    const auto input = context();
    const auto baseline = impl::evaluate_equity_baseline(input, config());
    ASSERT_TRUE(baseline.has_value()) << baseline.error().message();
    const auto families = impl::evaluate_equity_families(
        input, config(), *baseline, std::span<const std::string_view>(impl::kEquityFamilyDsl),
        std::span<const std::string_view>(impl::kEquityFamilySignalNames));
    ASSERT_TRUE(families.has_value()) << families.error().message();
    ASSERT_EQ(families->signals.alphas.size(), impl::kEquityFamilyDsl.size());
    EXPECT_EQ(families->signals.dates, 8U);
    // Longest rail is mom252_sector_neutral: ts_mean(.., 5) over delay(close, 252)
    // = 252 + 5 - 1 = 256, exactly the baseline warmup; derived, not hard-coded.
    EXPECT_EQ(families->warmup, 256U);
    for (atx::usize a = 0; a < impl::kEquityFamilyDsl.size(); ++a) {
        EXPECT_EQ(families->signals.alphas[a].name, impl::kEquityFamilySignalNames[a]);
        EXPECT_EQ(families->signals.alphas[a].values.size(), 8U * 3U);
    }
    const auto cont = families->signals.alpha_cross_section(7, 0);
    EXPECT_NEAR(cont[0], price(256, 0) / price(251, 0) - 1.0, 1e-12);
    EXPECT_TRUE(std::isnan(cont[2])); // Not admitted by the baseline mask yet.
    EXPECT_FALSE(std::isnan(families->signals.alpha_cross_section(7, 4)[2]));
    // Fixture volume (1e6) and raw_close (2 * close > 0) are finite and positive
    // everywhere, so amihud_21 and continuation_5 are finite wherever admitted.
    EXPECT_EQ(families->finite_admitted_cells[5], baseline->admitted_evaluation_cells);
    EXPECT_EQ(families->finite_admitted_cells[7], baseline->admitted_evaluation_cells);
    // The blend is rank(...) + rank(...): finite in [0, 2] where admitted, NaN elsewhere.
    const auto blend = families->signals.alpha_cross_section(14, 0);
    EXPECT_TRUE(std::isfinite(blend[0]));
    EXPECT_GE(blend[0], 0.0);
    EXPECT_LE(blend[0], 2.0);
    EXPECT_TRUE(std::isnan(blend[2]));
}

TEST(EquityBaselineViews, LiquidityFloorRejectsBelowAdvAndKeepsCountsHonest) {
    const auto input = context();
    const auto unfloored = impl::evaluate_equity_baseline(input, config());
    ASSERT_TRUE(unfloored.has_value()) << unfloored.error().message();
    // Fixture: raw_close = 2 * close, volume = 1e6, so 21-day mean dollar ADV is
    // 2e6 * mean(close). Over evaluation rows 256..263 the window mean of t is
    // 246..253: instrument 0 (100 + t) ~ 6.92e8..7.06e8, instrument 1
    // (500 - 0.25 t) ~ 8.73e8..8.77e8, instrument 2 (100 + 3 t) ~ 1.68e9..1.72e9.
    auto all_rejected = config();
    all_rejected.min_dollar_adv = 1e12;
    const auto none = impl::evaluate_equity_baseline(input, all_rejected);
    ASSERT_TRUE(none.has_value()) << none.error().message();
    EXPECT_EQ(none->admitted_evaluation_cells, 0U);
    EXPECT_EQ(none->liquidity_floor_rejected_cells, unfloored->admitted_evaluation_cells);

    atx::usize instrument0_admitted = 0;
    for (atx::usize row = 0; row < 8; ++row) {
        if (unfloored->panel.in_universe(row, 0)) ++instrument0_admitted;
    }
    auto floor_between = config();
    floor_between.min_dollar_adv = 7.5e8;
    const auto partial = impl::evaluate_equity_baseline(input, floor_between);
    ASSERT_TRUE(partial.has_value()) << partial.error().message();
    EXPECT_EQ(partial->admitted_evaluation_cells,
              unfloored->admitted_evaluation_cells - instrument0_admitted);
    EXPECT_EQ(partial->liquidity_floor_rejected_cells, instrument0_admitted);
    EXPECT_FALSE(partial->panel.in_universe(0, 0));
    EXPECT_EQ(partial->panel.in_universe(0, 2), unfloored->panel.in_universe(0, 2));

    auto oversized_window = config();
    oversized_window.min_dollar_adv = 1.0;
    oversized_window.dollar_adv_window = 1000;
    EXPECT_FALSE(impl::evaluate_equity_baseline(input, oversized_window).has_value());

    auto no_floor = config();
    no_floor.min_dollar_adv = 0.0;
    const auto unchanged = impl::evaluate_equity_baseline(input, no_floor);
    ASSERT_TRUE(unchanged.has_value()) << unchanged.error().message();
    EXPECT_EQ(unchanged->admitted_evaluation_cells, 4U * 2U + 4U * 3U);
    EXPECT_EQ(unchanged->liquidity_floor_rejected_cells, 0U);
}

TEST(EquityBaselineViews, DollarAdvAncillaryEvaluatesOnBaselineMask) {
    const auto input = context();
    const auto baseline = impl::evaluate_equity_baseline(input, config());
    ASSERT_TRUE(baseline.has_value()) << baseline.error().message();
    const std::array<std::string_view, 1> dsl{impl::kEquityDollarAdvDsl};
    const std::array<std::string_view, 1> names{impl::kEquityDollarAdvName};
    const auto adv = impl::evaluate_equity_families(
        input, config(), *baseline, std::span<const std::string_view>(dsl),
        std::span<const std::string_view>(names));
    ASSERT_TRUE(adv.has_value()) << adv.error().message();
    ASSERT_EQ(adv->signals.alphas.size(), 1U);
    EXPECT_EQ(adv->signals.alphas[0].name, impl::kEquityDollarAdvName);
    EXPECT_EQ(adv->warmup, 20U);
    // Fixture: raw_close = 2 * close, volume = 1e6 everywhere.
    atx::f64 expected = 0.0;
    for (atx::usize r = 236; r <= 256; ++r) expected += 2.0 * price(r, 0) * 1e6;
    expected /= 21.0;
    const auto first = adv->signals.alpha_cross_section(0, 0);
    EXPECT_NEAR(first[0], expected, 1e-3);
    EXPECT_TRUE(std::isnan(first[2])); // Not admitted by the baseline mask yet.
    EXPECT_FALSE(std::isnan(adv->signals.alpha_cross_section(0, 4)[2]));
    EXPECT_EQ(adv->finite_admitted_cells[0], baseline->admitted_evaluation_cells);
}
