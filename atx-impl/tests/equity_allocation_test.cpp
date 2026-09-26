#include "equity_allocation.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <limits>
#include <span>
#include <string>
#include <vector>

#include <gtest/gtest.h>

namespace {
namespace impl = atx::impl;
constexpr atx::i64 kDay = 86'400'000'000'000LL;

struct Fixture {
    atx::usize n{2};
    std::vector<atx::i64> keys;
    std::vector<atx::f64> closes;
    std::vector<atx::f64> preference{0.5, -0.5};
    std::vector<atx::u8> eligibility{1, 1};
    std::vector<atx::f64> units{0.0, 0.0};
    std::vector<atx::f64> marks{100.0, 100.0};
    std::vector<atx::f64> marked{0.0, 0.0};
    atx::f64 cash{1000.0};
    atx::f64 nav{1000.0};
    impl::EquityAllocationConfig config;

    Fixture() {
        config.name_limit = 0.5;
        config.risk_penalty = 0.0;
        config.trade_bps = 0.0;
        for (atx::usize t = 0; t < 68; ++t) {
            keys.push_back(static_cast<atx::i64>(t) * kDay);
            closes.push_back(100.0 + static_cast<atx::f64>(t));
            closes.push_back(200.0 - static_cast<atx::f64>(t));
        }
    }

    impl::EquityAllocationRiskWindow window() const {
        return {0, 0, 63, keys[63], std::span<const atx::i64>(keys).first(64),
                std::span<const atx::f64>(closes).first(64 * n), {}};
    }

    auto freeze() const {
        return impl::freeze_equity_allocation_decision(window(), preference, eligibility, config);
    }

    impl::EquityAllocationExecution execution() const {
        return {0, 1, keys[63], keys[64], cash, nav, units, marks, marked};
    }
};
} // namespace

TEST(EquityAllocation, UnchangedPreferenceTradesAgainstDriftedMarkedHoldings) {
    Fixture f;
    f.preference = {0.1, -0.1};
    f.units = {1.0, -1.0};
    f.marks = {110.0, 90.0};
    f.marked = {110.0, -90.0};
    f.cash = 980.0;
    f.config.turnover_limit = 0.02;
    const auto decision = f.freeze();
    ASSERT_TRUE(decision) << decision.error().message();
    const auto allocation = impl::allocate_equity_preference(*decision, f.execution());
    ASSERT_TRUE(allocation) << allocation.error().message();
    EXPECT_NEAR(allocation->weights[0], 0.1, 1e-8);
    EXPECT_NEAR(allocation->weights[1], -0.1, 1e-8);
    EXPECT_NEAR(allocation->certificate.actual_turnover, 0.02, 1e-8);
    EXPECT_NEAR(allocation->certificate.traded_dollars, 20.0, 1e-5);
    EXPECT_NEAR(allocation->certificate.postfee_net, 0.0, 1e-8);
    EXPECT_NEAR(allocation->certificate.posttrade_nav, 1000.0, 1e-10);
    EXPECT_EQ(allocation->union_indices, (std::vector<atx::usize>{0, 1}));
    // Target-vs-target turnover would have been exactly zero for this preference.
    EXPECT_GT(allocation->certificate.actual_turnover, 0.019);
}

TEST(EquityAllocationR1, SparsePlanningAdmitsFiveThousandWithoutSolvingOrAllocatingDenseMatrices) {
    impl::EquityAllocationConfig cfg;
    EXPECT_EQ(cfg.rule, impl::EquityAllocationRule::LegacyDenseAbsoluteV1);
    EXPECT_FALSE(impl::plan_equity_allocation(5000, 5000, cfg));
    cfg.rule = impl::EquityAllocationRule::SparseRelativeV2;
    const auto plan = impl::plan_equity_allocation(5000, 5000, cfg);
    ASSERT_TRUE(plan) << plan.error().message();
    EXPECT_EQ(plan->primal_dimension, 15'001U);
    EXPECT_EQ(plan->augmented_rows, 35'004U);
    EXPECT_EQ(plan->kkt_dimension, 50'005U);
    EXPECT_LT(plan->additional_bytes_bound, cfg.max_additional_bytes);
    cfg.max_additional_bytes = plan->additional_bytes_bound - 1;
    EXPECT_FALSE(impl::plan_equity_allocation(5000, 5000, cfg));
    cfg.max_additional_bytes = 3'000'000'000ULL;
    cfg.sparse_storage.max_nnz = 9999;
    EXPECT_FALSE(impl::plan_equity_allocation(5000, 5000, cfg));
}

TEST(EquityAllocationR1, ExplicitLegacyRepeatsBitsAndSparseRelativeRecertifiesSameSmallBook) {
    Fixture f;
    f.config.trade_bps = 5.0;
    const auto original = f.freeze();
    ASSERT_TRUE(original);
    const auto before = impl::allocate_equity_preference(*original, f.execution());
    ASSERT_TRUE(before) << before.error().message();
    f.config.rule = impl::EquityAllocationRule::LegacyDenseAbsoluteV1;
    const auto legacy = f.freeze();
    ASSERT_TRUE(legacy);
    const auto repeated = impl::allocate_equity_preference(*legacy, f.execution());
    ASSERT_TRUE(repeated) << repeated.error().message();
    for (atx::usize i = 0; i < f.n; ++i)
        EXPECT_EQ(std::bit_cast<atx::u64>(before->weights[i]), std::bit_cast<atx::u64>(repeated->weights[i]));
    EXPECT_DOUBLE_EQ(repeated->certificate.effective_solver_relative_tolerance, 0.0);
    f.config.rule = impl::EquityAllocationRule::SparseRelativeV2;
    const auto sparse = f.freeze();
    ASSERT_TRUE(sparse);
    const auto now = impl::allocate_equity_preference(*sparse, f.execution());
    ASSERT_TRUE(now) << now.error().message();
    EXPECT_EQ(now->certificate.rule, impl::EquityAllocationRule::SparseRelativeV2);
    EXPECT_GT(now->certificate.effective_solver_relative_tolerance, 0.0);
    EXPECT_LE(now->certificate.actual_turnover, f.config.turnover_limit + f.config.feasibility_tolerance);
    EXPECT_LE(std::abs(now->certificate.postfee_net), f.config.feasibility_tolerance);
    for (atx::usize i = 0; i < f.n; ++i)
        EXPECT_NEAR(now->weights[i], before->weights[i], f.config.feasibility_tolerance);
    f.config.sparse_storage.max_solver_bytes = 4;
    const auto limited = f.freeze();
    ASSERT_TRUE(limited);
    EXPECT_FALSE(impl::allocate_equity_preference(*limited, f.execution()));
}

TEST(EquityAllocation, AllCashStartupAndNextAllocationRespectFullL1WithoutGrossRenormalization) {
    Fixture f;
    f.config.trade_bps = 5.0;
    const auto decision = f.freeze();
    ASSERT_TRUE(decision) << decision.error().message();
    const auto first = impl::allocate_equity_preference(*decision, f.execution());
    ASSERT_TRUE(first) << first.error().message();
    EXPECT_GT(first->certificate.effective_solver_feasibility_tolerance, 0.0);
    EXPECT_LT(first->certificate.effective_solver_feasibility_tolerance, f.config.solver.feas_tol);
    EXPECT_NEAR(first->weights[0], 0.1, 1e-8);
    EXPECT_NEAR(first->weights[1], -0.1, 1e-8);
    EXPECT_NEAR(first->certificate.actual_turnover, 0.2, 1e-8);
    EXPECT_NEAR(first->certificate.trade_cost, 0.1, 1e-8);
    EXPECT_LT(first->certificate.postfee_gross, 0.201);
    EXPECT_NEAR(first->certificate.fee_reserve, 0.9999, 1e-15);

    for (atx::usize i = 0; i < f.n; ++i) {
        f.units[i] = (first->weights[i] * f.nav) / f.marks[i];
        f.marked[i] = f.units[i] * f.marks[i];
    }
    f.cash = first->certificate.posttrade_cash;
    f.nav = first->certificate.posttrade_nav;
    const auto second = impl::allocate_equity_preference(*decision, f.execution());
    ASSERT_TRUE(second) << second.error().message();
    EXPECT_NEAR(second->certificate.actual_turnover, 0.2, 1e-8);
    EXPECT_GT(second->certificate.postfee_gross, 0.4);
    EXPECT_LT(second->certificate.postfee_gross, 0.401);
    EXPECT_NEAR(second->certificate.traded_dollars, f.nav * 0.2, 1e-5);
}

TEST(EquityAllocation, RiskSnapshotExcludesFutureRowsAndNeverBridgesMissingObservations) {
    Fixture f;
    f.config.risk_penalty = 100.0;
    const auto original = f.freeze();
    ASSERT_TRUE(original) << original.error().message();
    EXPECT_EQ(original->valid_return_count, (std::vector<atx::usize>{63, 63}));
    // Independent population variance of all adjacent pairs through the decision.
    std::array<atx::f64, 63> returns{};
    atx::f64 mean = 0.0;
    for (atx::usize t = 1; t < 64; ++t) {
        returns[t - 1] = f.closes[t * 2] / f.closes[(t - 1) * 2] - 1.0;
        mean += returns[t - 1];
    }
    mean /= 63.0;
    atx::f64 variance = 0.0;
    for (const auto r : returns) variance += (r - mean) * (r - mean);
    EXPECT_NEAR(original->variance[0], std::max(f.config.variance_floor, variance / 63.0), 1e-18);
    for (atx::usize t = 64; t < 68; ++t) {
        f.closes[t * 2] = 1e100;
        f.closes[t * 2 + 1] = std::numeric_limits<atx::f64>::quiet_NaN();
    }
    const auto perturbed = f.freeze();
    ASSERT_TRUE(perturbed) << perturbed.error().message();
    EXPECT_EQ(perturbed->variance, original->variance);
    EXPECT_EQ(perturbed->valid_return_count, original->valid_return_count);
    const auto a = impl::allocate_equity_preference(*original, f.execution());
    const auto b = impl::allocate_equity_preference(*perturbed, f.execution());
    ASSERT_TRUE(a) << a.error().message();
    ASSERT_TRUE(b) << b.error().message();
    EXPECT_EQ(a->weights, b->weights);
    f.preference[0] = -99.0;
    f.eligibility[0] = 0;
    EXPECT_EQ(original->preference[0], 0.5);
    EXPECT_EQ(original->eligibility[0], 1);

    f.preference = {0.5, -0.5};
    f.eligibility = {1, 1};
    f.closes[30 * 2] = std::numeric_limits<atx::f64>::quiet_NaN();
    const auto gap = f.freeze();
    ASSERT_TRUE(gap) << gap.error().message();
    EXPECT_EQ(gap->valid_return_count[0], 61U); // Two adjacent returns, no bridge.
    f.units = {1.0, -1.0};
    f.marked = {100.0, -100.0};
    const auto held_gap = impl::allocate_equity_preference(*gap, f.execution());
    ASSERT_FALSE(held_gap);
    EXPECT_NE(held_gap.error().message().find("held eligible name lacks"), std::string::npos);
    auto future_window = f.window();
    future_window.decision_session_key -= kDay;
    EXPECT_FALSE(impl::freeze_equity_allocation_decision(future_window, f.preference, f.eligibility, f.config));
}

TEST(EquityAllocation, MandatoryExitRetainsHeldNameAndNeutralizingTradeCanMakeTurnoverInfeasible) {
    Fixture f;
    f.preference = {0.0, -0.1};
    f.eligibility = {0, 1};
    f.units = {0.5, -0.5};
    f.marked = {50.0, -50.0};
    f.config.turnover_limit = 0.08; // Exit itself costs .05, but neutrality needs .10.
    f.closes[30 * 2] = std::numeric_limits<atx::f64>::quiet_NaN();
    const auto decision = f.freeze();
    ASSERT_TRUE(decision) << decision.error().message();
    const auto denied = impl::allocate_equity_preference(*decision, f.execution());
    ASSERT_FALSE(denied);
    EXPECT_EQ(f.units, (std::vector<atx::f64>{0.5, -0.5}));
    f.config.turnover_limit = 0.11;
    const auto feasible = f.freeze();
    ASSERT_TRUE(feasible) << feasible.error().message();
    const auto exit = impl::allocate_equity_preference(*feasible, f.execution());
    ASSERT_TRUE(exit) << exit.error().message();
    EXPECT_EQ(exit->union_indices, (std::vector<atx::usize>{0, 1}));
    EXPECT_DOUBLE_EQ(exit->weights[0], 0.0); // Mandatory boxes lifted to exact zero.
    EXPECT_NEAR(exit->weights[1], 0.0, 1e-8);
    EXPECT_NEAR(exit->certificate.actual_turnover, 0.1, 1e-8);
    EXPECT_EQ(exit->certificate.fixed_zero_count, 1U);
    f.marks[0] = std::numeric_limits<atx::f64>::quiet_NaN();
    EXPECT_FALSE(impl::allocate_equity_preference(*feasible, f.execution()));
}

TEST(EquityAllocation, FeeReserveCertifiesActualPostFeeCapsAndInvalidBudgetsFailClosed) {
    Fixture f;
    f.config.gross_limit = 0.2;
    f.config.name_limit = 0.1;
    f.config.turnover_limit = 1.0;
    f.config.trade_bps = 100.0;
    const auto decision = f.freeze();
    ASSERT_TRUE(decision) << decision.error().message();
    const auto result = impl::allocate_equity_preference(*decision, f.execution());
    ASSERT_TRUE(result) << result.error().message();
    EXPECT_NEAR(result->weights[0], 0.099, 1e-8);
    EXPECT_NEAR(result->weights[1], -0.099, 1e-8);
    EXPECT_LE(result->certificate.postfee_gross, 0.2 + 1e-8);
    EXPECT_LE(result->certificate.postfee_max_name, 0.1 + 1e-8);
    EXPECT_NEAR(result->certificate.posttrade_nav, 1000.0 - result->certificate.trade_cost, 1e-10);
    EXPECT_EQ(result->certificate.solver.primal_infeasible, false);
    EXPECT_LE(result->certificate.solver.dual_res, f.config.residual_tolerance);
    EXPECT_TRUE(impl::plan_equity_allocation(1661, 1000, f.config));

    f.config.max_additional_bytes = result->plan.additional_bytes_bound - 1;
    const auto too_small = f.freeze();
    ASSERT_TRUE(too_small) << too_small.error().message();
    const auto no_budget = impl::allocate_equity_preference(*too_small, f.execution());
    ASSERT_FALSE(no_budget);
    EXPECT_NE(no_budget.error().message().find("byte budget"), std::string::npos);
    f.config.max_additional_bytes = 3'000'000'000ULL;
    f.config.solver.max_factor_bytes = 1;
    const auto factor_limited = f.freeze();
    ASSERT_TRUE(factor_limited) << factor_limited.error().message();
    EXPECT_FALSE(impl::allocate_equity_preference(*factor_limited, f.execution()));
    f.config.solver.max_factor_bytes = 268'435'456ULL;
    f.config.trade_bps = 10000.0;
    EXPECT_FALSE(f.freeze()); // Worst-case fees exhaust NAV.
    f.config.trade_bps = 0.0;
    f.config.risk_penalty = std::numeric_limits<atx::f64>::quiet_NaN();
    EXPECT_FALSE(f.freeze());
}

TEST(EquityAllocation, MachineRepresentationClosesNumericalSupportBeforeRequiringUnusedMarks) {
    namespace book = atx::engine::book;
    Fixture f;
    f.preference = {0.0, 0.0};
    f.config.representation = impl::EquityAllocationRepresentation::MachinePrecisionIntentsV1;
    f.marks.assign(2, std::numeric_limits<atx::f64>::quiet_NaN());
    const auto decision = f.freeze();
    ASSERT_TRUE(decision) << decision.error().message();
    const std::array<atx::f64, 2> numerical_zero{1e-27, -1e-27};
    const auto represented = impl::represent_equity_allocation(*decision, f.execution(), numerical_zero);
    ASSERT_TRUE(represented) << represented.error().message();
    EXPECT_EQ(represented->continuous_weights, (std::vector<atx::f64>{1e-27, -1e-27}));
    EXPECT_EQ(represented->weights, (std::vector<atx::f64>{0.0, 0.0}));
    EXPECT_EQ(represented->intents[0].action, book::ReplayTargetAction::Close);
    EXPECT_EQ(represented->intents[1].action, book::ReplayTargetAction::Close);
    EXPECT_EQ(represented->certificate.close_count, 2U);
    EXPECT_EQ(represented->certificate.hold_count, 0U);
    EXPECT_FALSE(represented->certificate.solver_used); // Representation alone is no QP certificate.
    EXPECT_DOUBLE_EQ(represented->certificate.traded_dollars, 0.0);
    EXPECT_DOUBLE_EQ(represented->certificate.representation_l1_change, 2e-27);
    EXPECT_LE(represented->certificate.representation_l1_change,
              represented->certificate.representation_l1_budget);
    EXPECT_DOUBLE_EQ(represented->certificate.actual_representation_l1_change, 2e-27);
    EXPECT_DOUBLE_EQ(represented->certificate.represented_objective, 0.0);
    EXPECT_LT(represented->certificate.representation_objective_gap, 0.0);
    const auto solved = impl::allocate_equity_preference(*decision, f.execution());
    ASSERT_TRUE(solved) << solved.error().message();
    EXPECT_TRUE(solved->certificate.solver_used);
    EXPECT_EQ(solved->weights, (std::vector<atx::f64>{0.0, 0.0}));
    EXPECT_EQ(solved->certificate.close_count, 2U);

    const std::array<atx::f64, 2> ordinary_entry{0.05, -0.05};
    EXPECT_FALSE(impl::represent_equity_allocation(*decision, f.execution(), ordinary_entry));
    f.config.representation = impl::EquityAllocationRepresentation::ExactWeights;
    const auto exact = f.freeze();
    ASSERT_TRUE(exact) << exact.error().message();
    EXPECT_FALSE(impl::represent_equity_allocation(*exact, f.execution(), numerical_zero));
}

TEST(EquityAllocation, MachineRepresentationPreservesExactHeldUnitsAndMandatoryClosePriority) {
    namespace book = atx::engine::book;
    Fixture f;
    f.config.representation = impl::EquityAllocationRepresentation::MachinePrecisionIntentsV1;
    f.config.trade_bps = 5.0;
    f.units = {0.7661368727868479, -0.7661368727868479};
    f.marks = {51.75873612214492, 51.75873612214492};
    f.marked = {f.units[0] * f.marks[0], f.units[1] * f.marks[1]};
    const auto prior_weight = f.marked[0] / f.nav;
    // This binary64 fixture loses a unit bit through an ordinary weight roundtrip.
    EXPECT_NE(std::bit_cast<atx::u64>((prior_weight * f.nav) / f.marks[0]),
              std::bit_cast<atx::u64>(f.units[0]));
    const auto d = 4.0 * std::numeric_limits<atx::f64>::epsilon();
    const std::array<atx::f64, 2> raw{prior_weight + d, -prior_weight - d};
    const auto decision = f.freeze();
    ASSERT_TRUE(decision) << decision.error().message();
    const auto held = impl::represent_equity_allocation(*decision, f.execution(), raw);
    ASSERT_TRUE(held) << held.error().message();
    EXPECT_EQ(held->certificate.hold_count, 2U);
    EXPECT_EQ(held->certificate.close_count, 0U);
    EXPECT_DOUBLE_EQ(held->certificate.traded_dollars, 0.0);
    EXPECT_DOUBLE_EQ(held->certificate.trade_cost, 0.0);
    EXPECT_DOUBLE_EQ(held->certificate.posttrade_cash, f.cash);
    for (atx::usize i = 0; i < f.n; ++i) {
        const auto sized = book::resolve_replay_target(held->intents[i], f.units[i], f.marked[i],
                                                       f.marks[i], f.nav, true);
        ASSERT_TRUE(sized) << sized.error().message();
        EXPECT_EQ(std::bit_cast<atx::u64>(sized->tri_units), std::bit_cast<atx::u64>(f.units[i]));
        EXPECT_EQ(std::bit_cast<atx::u64>(sized->marked_dollars), std::bit_cast<atx::u64>(f.marked[i]));
        EXPECT_DOUBLE_EQ(sized->dollar_delta, 0.0);
    }
    f.eligibility = {0, 0};
    const auto mandatory = f.freeze();
    ASSERT_TRUE(mandatory) << mandatory.error().message();
    const auto closed = impl::represent_equity_allocation(*mandatory, f.execution(), raw);
    ASSERT_TRUE(closed) << closed.error().message();
    EXPECT_EQ(closed->certificate.close_count, 2U);
    EXPECT_EQ(closed->certificate.hold_count, 0U);
    EXPECT_EQ(closed->certificate.fixed_zero_count, 2U);
    EXPECT_DOUBLE_EQ(closed->certificate.representation_l1_change, 0.0);
    EXPECT_GT(closed->certificate.fixed_zero_l1_change, closed->certificate.representation_l1_budget);
    EXPECT_DOUBLE_EQ(closed->certificate.traded_dollars, 2.0 * f.marked[0]);
    f.marks[0] = std::numeric_limits<atx::f64>::quiet_NaN();
    EXPECT_FALSE(impl::represent_equity_allocation(*decision, f.execution(), raw));
    EXPECT_FALSE(impl::represent_equity_allocation(*mandatory, f.execution(), raw));
}

TEST(EquityAllocation, MachineRepresentationRecertifiesEconomicLimitsAndRejectsUnrepresentableBudget) {
    Fixture f;
    f.config.representation = impl::EquityAllocationRepresentation::MachinePrecisionIntentsV1;
    const auto decision = f.freeze();
    ASSERT_TRUE(decision) << decision.error().message();
    const std::array<atx::f64, 2> bad_book{0.1 + 2e-8, -0.1};
    const auto infeasible = impl::represent_equity_allocation(*decision, f.execution(), bad_book);
    ASSERT_FALSE(infeasible);
    EXPECT_NE(infeasible.error().message().find("original-unit/post-fee"), std::string::npos);
    EXPECT_EQ(f.units, (std::vector<atx::f64>{0.0, 0.0}));
    EXPECT_DOUBLE_EQ(f.cash, 1000.0);
    const std::array<atx::f64, 2> feasible_book{0.1, -0.1};
    const auto feasible = impl::represent_equity_allocation(*decision, f.execution(), feasible_book);
    ASSERT_TRUE(feasible) << feasible.error().message();
    EXPECT_DOUBLE_EQ(feasible->certificate.representation_l1_change, 0.0);
    EXPECT_LE(feasible->certificate.actual_turnover, f.config.turnover_limit + f.config.feasibility_tolerance);
    // Same fixed policy and same hard economic tolerance; no fallback to a
    // larger epsilon when binary64 cannot represent its derived change budget.
    f.config.feasibility_tolerance = std::numeric_limits<atx::f64>::denorm_min();
    f.config.solver.feas_tol = f.config.feasibility_tolerance;
    const auto microscopic = f.freeze();
    ASSERT_TRUE(microscopic) << microscopic.error().message();
    const auto no_budget = impl::represent_equity_allocation(*microscopic, f.execution(), feasible_book);
    ASSERT_FALSE(no_budget);
    EXPECT_NE(no_budget.error().message().find("representation change budget"), std::string::npos);
}

TEST(EquityAllocation, ObservedCloseConstraintReoptimizesBalancedBookAndValidCurrentMarkRestoresEntry) {
    Fixture f;
    f.n = 3;
    f.preference = {0.4, -0.3, -0.1};
    f.eligibility = {1, 1, 1};
    f.units.assign(3, 0.0);
    f.marked.assign(3, 0.0);
    f.marks = {std::numeric_limits<atx::f64>::quiet_NaN(), 100.0, 100.0};
    f.closes.clear();
    for (atx::usize t = 0; t < f.keys.size(); ++t) {
        f.closes.push_back(100.0 + static_cast<atx::f64>(t));
        f.closes.push_back(200.0 - static_cast<atx::f64>(t));
        f.closes.push_back(150.0);
    }
    f.config.turnover_limit = 1.0;
    f.config.trade_bps = 5.0;
    f.config.representation = impl::EquityAllocationRepresentation::MachinePrecisionIntentsV1;
    EXPECT_EQ(f.config.execution_availability, impl::EquityExecutionAvailability::RequireRequestedMark);
    const auto strict = f.freeze();
    ASSERT_TRUE(strict) << strict.error().message();
    const auto denied = impl::allocate_equity_preference(*strict, f.execution());
    ASSERT_FALSE(denied);
    EXPECT_NE(denied.error().message().find("missing/nonpositive required close"), std::string::npos);

    f.config.execution_availability = impl::EquityExecutionAvailability::ObservedCloseEntryConstraintV1;
    const auto conditional = f.freeze();
    ASSERT_TRUE(conditional) << conditional.error().message();
    EXPECT_EQ(conditional->preference, strict->preference);
    EXPECT_EQ(conditional->eligibility, strict->eligibility);
    EXPECT_EQ(conditional->variance, strict->variance);
    const auto selected = impl::allocate_equity_preference(*conditional, f.execution());
    ASSERT_TRUE(selected) << selected.error().message();
    // With w0=0 and w1+w2=0, the identity-Hessian optimum is (0,-.1,+.1).
    // Dropping the missing entry only AFTER an unrestricted solve gives net -.4.
    EXPECT_EQ(selected->union_indices, (std::vector<atx::usize>{0, 1, 2}));
    EXPECT_DOUBLE_EQ(selected->weights[0], 0.0);
    EXPECT_NEAR(selected->weights[1], -0.1, f.config.feasibility_tolerance);
    EXPECT_NEAR(selected->weights[2], 0.1, f.config.feasibility_tolerance);
    EXPECT_EQ(selected->intents[0].action, atx::engine::book::ReplayTargetAction::Close);
    EXPECT_EQ(selected->required_zero_reasons, (std::vector<atx::u8>{4, 0, 0}));
    EXPECT_TRUE(selected->certificate.solver_used);
    EXPECT_EQ(selected->certificate.execution_unavailable_count, 1U);
    EXPECT_EQ(selected->certificate.execution_fixed_zero_count, 1U);
    EXPECT_EQ(selected->certificate.required_zero_count, 1U);
    EXPECT_EQ(selected->certificate.fixed_zero_count, 0U);
    EXPECT_DOUBLE_EQ(selected->certificate.decision_fixed_zero_l1_change, 0.0);
    EXPECT_DOUBLE_EQ(selected->certificate.execution_fixed_zero_l1_change,
                     std::abs(selected->continuous_weights[0]));
    EXPECT_DOUBLE_EQ(selected->certificate.fixed_zero_l1_change,
                     selected->certificate.execution_fixed_zero_l1_change);
    EXPECT_NEAR(selected->certificate.traded_dollars, 200.0, 2e-5);
    EXPECT_NEAR(selected->certificate.trade_cost, 0.1, 1e-8);
    EXPECT_NEAR(selected->certificate.posttrade_nav, 999.9, 1e-8);
    EXPECT_NEAR(selected->certificate.postfee_net, 0.0, f.config.feasibility_tolerance);

    // The pure boundary uses exactly the same rule, while retaining its explicit
    // lack of a QP certificate. All invalid current-price classes behave alike.
    const std::array<atx::f64, 3> supplied{0.25, -0.1, 0.1};
    for (const auto invalid : {std::numeric_limits<atx::f64>::quiet_NaN(), 0.0, -1.0,
                               std::numeric_limits<atx::f64>::infinity()}) {
        f.marks[0] = invalid;
        const auto represented = impl::represent_equity_allocation(*conditional, f.execution(), supplied);
        ASSERT_TRUE(represented) << represented.error().message();
        EXPECT_FALSE(represented->certificate.solver_used);
        EXPECT_EQ(represented->required_zero_reasons, selected->required_zero_reasons);
        EXPECT_DOUBLE_EQ(represented->certificate.execution_fixed_zero_l1_change, 0.25);
        EXPECT_DOUBLE_EQ(represented->certificate.representation_l1_change, 0.0);
        EXPECT_FALSE(impl::represent_equity_allocation(*strict, f.execution(), supplied));
    }
    // A later current observation can admit entry. The frozen decision and the
    // earlier result are unchanged; no forward coverage/retirement rule exists.
    f.marks[0] = 100.0;
    auto later = f.execution();
    later.execution_period = 2;
    later.execution_session_key = f.keys[65];
    const auto available = impl::allocate_equity_preference(*conditional, later);
    ASSERT_TRUE(available) << available.error().message();
    EXPECT_NEAR(available->weights[0], 0.4, f.config.feasibility_tolerance);
    EXPECT_NEAR(available->weights[1], -0.3, f.config.feasibility_tolerance);
    EXPECT_NEAR(available->weights[2], -0.1, f.config.feasibility_tolerance);
    EXPECT_EQ(available->required_zero_reasons, (std::vector<atx::u8>{0, 0, 0}));
    EXPECT_EQ(available->certificate.execution_unavailable_count, 0U);
    EXPECT_EQ(available->certificate.execution_fixed_zero_count, 0U);
    EXPECT_EQ(available->certificate.required_zero_count, 0U);
    EXPECT_DOUBLE_EQ(selected->weights[0], 0.0);
    EXPECT_EQ(conditional->preference, f.preference);
    EXPECT_EQ(conditional->eligibility, f.eligibility);
}

TEST(EquityAllocation, ExecutionAvailabilityNeverSuppressesMissingHeldMarkOrMandatoryExitValuation) {
    Fixture f;
    f.units = {0.5, -0.5};
    f.marked = {50.0, -50.0};
    f.marks[0] = std::numeric_limits<atx::f64>::quiet_NaN();
    f.config.representation = impl::EquityAllocationRepresentation::MachinePrecisionIntentsV1;
    // A factor allocation would fail too, but the required held-mark error must
    // occur first. Neither a closing preference nor an exclusion can bypass it.
    f.config.solver.max_factor_bytes = 1;
    const std::array<atx::f64, 2> raw_close{0.0, 0.0};
    for (const auto mode : {impl::EquityExecutionAvailability::RequireRequestedMark,
                           impl::EquityExecutionAvailability::ObservedCloseEntryConstraintV1}) {
        f.config.execution_availability = mode;
        for (const atx::u8 first_eligible : {atx::u8{1}, atx::u8{0}}) {
            f.eligibility[0] = first_eligible;
            const auto decision = f.freeze();
            ASSERT_TRUE(decision) << decision.error().message();
            const auto allocated = impl::allocate_equity_preference(*decision, f.execution());
            const auto represented = impl::represent_equity_allocation(*decision, f.execution(), raw_close);
            ASSERT_FALSE(allocated);
            ASSERT_FALSE(represented);
            EXPECT_NE(allocated.error().message().find("inconsistent/missing held execution mark"), std::string::npos);
            EXPECT_NE(represented.error().message().find("inconsistent/missing held execution mark"), std::string::npos);
            EXPECT_EQ(f.units, (std::vector<atx::f64>{0.5, -0.5}));
            EXPECT_EQ(f.marked, (std::vector<atx::f64>{50.0, -50.0}));
            EXPECT_DOUBLE_EQ(f.cash, 1000.0);
        }
    }
}
