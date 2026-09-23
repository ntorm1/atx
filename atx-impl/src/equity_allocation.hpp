#pragma once

#include <span>
#include <vector>

#include "atx/core/error.hpp"
#include "atx/core/types.hpp"
#include "atx/engine/book/replay.hpp"
#include "atx/engine/risk/qp_solver.hpp"

namespace atx::impl {

inline constexpr atx::usize kEquityAllocationReturnCount = 63;

enum class EquityAllocationRepresentation : atx::u8 {
    ExactWeights,
    MachinePrecisionIntentsV1
};

enum class EquityExecutionAvailability : atx::u8 {
    RequireRequestedMark,
    ObservedCloseEntryConstraintV1
};

// Bitmask codebook for required_zero_reasons. Reasons can overlap outside the
// solve union; no held missing mark is ever converted to an execution exclusion.
enum class EquityAllocationZeroReason : atx::u8 {
    None = 0,
    DecisionIneligible = 1,
    RiskUnready = 2,
    ExecutionCloseUnavailable = 4
};

struct EquityAllocationConfig {
    atx::f64 risk_penalty{1.0}; // Preference tracking, not expected-return calibration.
    atx::f64 variance_floor{1e-6}; // Population variance of adjacent TRI returns.
    atx::f64 gross_limit{1.0}; // Immediate post-fee risky weights; upper bound only.
    atx::f64 name_limit{0.01};
    atx::f64 turnover_limit{0.2}; // Full L1 against execution-time marked holdings.
    atx::f64 trade_bps{5.0};
    atx::f64 feasibility_tolerance{1e-8}; // Original normalized economic units.
    atx::f64 residual_tolerance{1e-6};
    atx::engine::risk::QpConfig solver{1200, 1.0, 1e-6, 1e-8, 10, true, 3, 268'435'456ULL};
    // Additional live array/workspace admission budget. Caller budgets resident
    // context/replay/report storage and runtime/allocator overhead separately.
    atx::u64 max_additional_bytes{3'000'000'000ULL};
    EquityAllocationRepresentation representation{EquityAllocationRepresentation::ExactWeights};
    EquityExecutionAvailability execution_availability{EquityExecutionAvailability::RequireRequestedMark};
    // Opt-in factor constraints (negative == off, the historical default). Both
    // are post-fee economic bounds; the QP enforces them on pre-fee weights
    // scaled by the same fee reserve as gross/name, and certify() re-checks the
    // represented book. They need the matching decision exposures (below).
    atx::f64 beta_tolerance{-1.0};  // |sum_i beta_i w_i| <= this.
    atx::f64 sector_net_cap{-1.0};  // |sum_{i in g} w_i| <= this for every sector g.
};

// Exactly 64 canonical, consecutive source observations ending at the decision.
// No future rows or future eligibility are accepted. Session labels alone do not
// establish data publication times or prove that the source has every session.
struct EquityAllocationRiskWindow {
    atx::usize decision_period{}; // Evaluation/replay row.
    atx::usize first_context_row{};
    atx::usize decision_context_row{};
    atx::i64 decision_session_key{};
    std::span<const atx::i64> session_keys;
    std::span<const atx::f64> closes; // Row-major 64 * canonical instrument count.
    std::span<const atx::u8> observed; // Optional explicit 0/1 observation flags.
};

// Owned frozen input. No borrowed close history survives freezing. The config is
// captured here and reused by allocation, preventing a second inconsistent config.
// Keep the snapshot immutable after creation; callers bind source/axis identities.
struct EquityAllocationDecision {
    EquityAllocationConfig config;
    atx::usize decision_period{};
    atx::usize first_context_row{};
    atx::usize decision_context_row{};
    atx::i64 first_risk_session_key{};
    atx::i64 decision_session_key{};
    std::vector<atx::f64> preference;
    std::vector<atx::u8> eligibility;
    std::vector<atx::f64> variance; // Floor placeholder if count != 63; never fitted across gaps.
    std::vector<atx::usize> valid_return_count;
    // Point-in-time exposures as of the decision, attached by
    // attach_equity_exposures(). Empty == none. Required iff the matching
    // config bound is enabled.
    std::vector<atx::f64> beta;
    std::vector<atx::usize> sector;
};

struct EquityAllocationExecution {
    atx::usize decision_period{};
    atx::usize execution_period{};
    atx::i64 decision_session_key{};
    atx::i64 execution_session_key{};
    atx::f64 cash{};
    atx::f64 pretrade_nav{};
    std::span<const atx::f64> tri_units;
    std::span<const atx::f64> current_marks;
    std::span<const atx::f64> marked_dollars;
};

struct EquityAllocationPlan {
    atx::usize canonical_instruments{};
    atx::usize union_instruments{};
    atx::u64 primal_dimension{};
    atx::u64 augmented_rows{};
    atx::u64 kkt_dimension{};
    atx::u64 additional_bytes_bound{};
};

struct EquityAllocationCertificate {
    // This raw QP diagnostic applies only to continuous_weights, never to the
    // subsequently lifted/represented execution candidate.
    atx::engine::risk::QpCertificate solver;
    bool solver_used{}; // Empty union is a checked all-cash allocation.
    // Original augmented-row tolerance after L1/mandatory-zero/fee propagation;
    // zero when no solver is used. Economic acceptance tolerance is unchanged.
    atx::f64 effective_solver_feasibility_tolerance{};
    atx::f64 fee_reserve{};
    atx::f64 requested_prefee_net{};
    atx::f64 requested_prefee_gross{};
    atx::f64 requested_turnover{};
    atx::f64 actual_turnover{}; // Actual representable TRI dollar trades / pretrade NAV.
    atx::f64 postfee_net{};
    atx::f64 postfee_gross{};
    atx::f64 postfee_max_name{};
    atx::f64 traded_dollars{};
    atx::f64 trade_cost{};
    atx::f64 posttrade_cash{};
    atx::f64 posttrade_nav{};
    atx::f64 objective{};
    atx::usize fixed_zero_count{}; // Decision-ineligible coordinates in the solve union.
    atx::f64 continuous_objective{};
    atx::f64 represented_objective{}; // Actual marked-dollar weights after unit sizing.
    atx::f64 representation_objective_gap{}; // Stable actual-minus-continuous objective delta.
    atx::f64 fixed_zero_l1_change{}; // Mandatory lifting; existing solver-row budget.
    atx::f64 representation_l1_change{}; // Ordinary intent changes, excluding mandatory lifting.
    atx::f64 representation_l1_budget{};
    atx::f64 representation_max_eta{};
    atx::f64 actual_representation_l1_change{}; // Raw -> actual marked dollars / pretrade NAV.
    atx::usize close_count{};
    atx::usize hold_count{};
    // Current invalid marks for exact-unheld canonical names, even in strict mode.
    atx::usize execution_unavailable_count{};
    atx::usize execution_fixed_zero_count{}; // Additional execution equalities in the union.
    atx::usize required_zero_count{}; // Deduplicated required-zero coordinates in the union.
    // Components of total fixed_zero_l1_change; decision/risk reasons take
    // precedence over execution reasons if a coordinate carries both.
    atx::f64 decision_fixed_zero_l1_change{};
    atx::f64 execution_fixed_zero_l1_change{};
    // Represented post-fee factor exposures; 0 when the bound is off.
    atx::f64 postfee_beta_exposure{};
    atx::f64 postfee_max_sector_net{};
};

struct EquityAllocationResult {
    std::vector<atx::f64> weights; // Full canonical order, pretrade-NAV units.
    std::vector<atx::f64> continuous_weights; // Unchanged raw QP solution; zero outside union.
    std::vector<atx::engine::book::ReplayTargetIntent> intents;
    std::vector<atx::usize> union_indices; // Eligible/risk-ready OR nonzero held; never truncated.
    std::vector<atx::u8> required_zero_reasons; // Full canonical bitmask; see codebook above.
    EquityAllocationPlan plan;
    EquityAllocationCertificate certificate;
};

[[nodiscard]] atx::core::Result<EquityAllocationDecision>
freeze_equity_allocation_decision(const EquityAllocationRiskWindow &window,
                                  std::span<const atx::f64> preference,
                                  std::span<const atx::u8> eligibility,
                                  const EquityAllocationConfig &config = {});

// Attaches decision-time beta (finite, one per canonical instrument) and sector
// labels (one per instrument, any nonnegative id) to a frozen decision. Either
// span may be empty to leave that exposure off. The caller owns the point-in-
// time guarantee: exposures must be estimable at the decision session.
[[nodiscard]] atx::core::Status
attach_equity_exposures(EquityAllocationDecision &decision, std::span<const atx::f64> beta,
                        std::span<const atx::usize> sector);

// Conservative worst-case bound for the specific diagonal/net/box/gross/turnover
// path, including dense ConstraintSet A, sparse assembly and both guarded factor
// budgets. This is admission control, not an OS RSS ceiling. Oversized unions
// fail before solver/union allocations; no holdings are dropped to fit a budget.
[[nodiscard]] atx::core::Result<EquityAllocationPlan>
plan_equity_allocation(atx::usize canonical_instruments, atx::usize union_instruments,
                       const EquityAllocationConfig &config);

// Minimize 0.5||w-preference||^2 + lambda * sum(variance_i*w_i^2).
// Net is zero; gross/name/turnover are hard upper bounds within declared numeric
// tolerance. Decision-ineligible names are fixed exactly zero. Risk-unready new
// names cannot enter; risk-unready held names fail unless mandatory fixed exits.
// Optional observed-close availability adds an exact-unheld missing-current-mark
// equality before solving, without changing frozen eligibility/risk or the union.
// Independent certification reproduces replay's representable TRI sizing and fee
// arithmetic before returning any candidate. No I/O, fallback or state mutation.
[[nodiscard]] atx::core::Result<EquityAllocationResult>
allocate_equity_preference(const EquityAllocationDecision &decision,
                           const EquityAllocationExecution &execution);

// Pure continuous-to-execution boundary. No solver is run: solver_used stays
// false, and success certifies the represented economics, not QP optimality.
// Allocation calls the same representation/certification path after its QP.
[[nodiscard]] atx::core::Result<EquityAllocationResult>
represent_equity_allocation(const EquityAllocationDecision &decision,
                           const EquityAllocationExecution &execution,
                           std::span<const atx::f64> continuous_weights);

} // namespace atx::impl
