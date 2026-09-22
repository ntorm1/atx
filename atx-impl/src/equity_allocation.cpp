#include "equity_allocation.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <iomanip>
#include <limits>
#include <sstream>
#include <string>
#include <utility>

#include "atx/core/linalg/linalg.hpp"
#include "atx/engine/risk/constraints.hpp"
#include "atx/engine/risk/factor_model.hpp"

namespace atx::impl {
namespace {
using atx::core::Err;
using atx::core::ErrorCode;
using atx::core::Ok;
using atx::core::Result;
using atx::core::Status;
namespace risk = atx::engine::risk;

Result<atx::u64> add(atx::u64 a, atx::u64 b) {
    if (a > std::numeric_limits<atx::u64>::max() - b)
        return Err(ErrorCode::OutOfRange, "equity allocation: byte/index sum overflow");
    return Ok(a + b);
}

Result<atx::u64> mul(atx::u64 a, atx::u64 b) {
    if (b != 0 && a > std::numeric_limits<atx::u64>::max() / b)
        return Err(ErrorCode::OutOfRange, "equity allocation: byte/index product overflow");
    return Ok(a * b);
}

bool nonnegative(atx::f64 x) { return std::isfinite(x) && x >= 0.0; }
bool positive(atx::f64 x) { return std::isfinite(x) && x > 0.0; }

Status validate_config(const EquityAllocationConfig &c) {
    if (!nonnegative(c.risk_penalty) || !positive(c.variance_floor) ||
        !nonnegative(c.gross_limit) || !nonnegative(c.name_limit) ||
        !nonnegative(c.turnover_limit) || !nonnegative(c.trade_bps) ||
        !positive(c.feasibility_tolerance) || c.feasibility_tolerance > 1e-4 ||
        !positive(c.residual_tolerance) || c.residual_tolerance > 1e-4 ||
        !positive(c.solver.rho) || !positive(c.solver.sigma) ||
        !positive(c.solver.feas_tol) || c.solver.feas_tol > c.feasibility_tolerance ||
        c.solver.iters == 0 || c.solver.iters > 100'000 ||
        c.solver.ruiz_passes > 100 || c.solver.polish_refine > 100 || c.solver.max_factor_bytes == 0 ||
        c.max_additional_bytes == 0 ||
        (c.representation != EquityAllocationRepresentation::ExactWeights &&
         c.representation != EquityAllocationRepresentation::MachinePrecisionIntentsV1) ||
        (c.execution_availability != EquityExecutionAvailability::RequireRequestedMark &&
         c.execution_availability != EquityExecutionAvailability::ObservedCloseEntryConstraintV1)) {
        return Err(ErrorCode::InvalidArgument, "equity allocation: invalid numeric/configuration bounds");
    }
    const auto reserve = 1.0 - (c.trade_bps * 1e-4) * c.turnover_limit;
    if (!positive(reserve) || !nonnegative(c.gross_limit * reserve) ||
        !nonnegative(c.name_limit * reserve)) {
        return Err(ErrorCode::InvalidArgument, "equity allocation: nonpositive/nonfinite fee reserve");
    }
    return Ok();
}

Result<atx::u64> snapshot_bound(atx::usize n) {
    ATX_TRY(auto arrays, mul(static_cast<atx::u64>(n), 64));
    return add(arrays, 65'536);
}

std::string at_instrument(atx::usize i) { return " instrument=" + std::to_string(i); }

Status validate_decision(const EquityAllocationDecision &d) {
    ATX_TRY_VOID(validate_config(d.config));
    const auto n = d.preference.size();
    if (n == 0 || d.eligibility.size() != n || d.variance.size() != n ||
        d.valid_return_count.size() != n || d.decision_context_row < d.first_context_row ||
        d.decision_context_row - d.first_context_row != kEquityAllocationReturnCount ||
        d.first_risk_session_key >= d.decision_session_key ||
        d.decision_context_row == std::numeric_limits<atx::usize>::max()) {
        return Err(ErrorCode::InvalidArgument, "equity allocation: malformed frozen decision");
    }
    for (atx::usize i = 0; i < n; ++i) {
        if (!std::isfinite(d.preference[i]) || d.eligibility[i] > 1 ||
            !positive(d.variance[i]) || d.variance[i] < d.config.variance_floor ||
            d.valid_return_count[i] > kEquityAllocationReturnCount ||
            (d.valid_return_count[i] != kEquityAllocationReturnCount &&
             d.variance[i] != d.config.variance_floor)) {
            return Err(ErrorCode::InvalidArgument,
                       "equity allocation: invalid frozen risk/preference" + at_instrument(i));
        }
    }
    return Ok();
}

// Scale by accumulated absolute economic amounts. This only checks accounting
// roundoff; it is deliberately unrelated to configured optimization tolerance.
bool reconciled(atx::f64 actual, atx::f64 expected, atx::f64 scale, atx::usize n) {
    const auto tolerance = 64.0 * std::numeric_limits<atx::f64>::epsilon() *
                           static_cast<atx::f64>(std::max<atx::usize>(1, n)) *
                           std::max({1.0, std::abs(actual), std::abs(expected), scale});
    return std::isfinite(tolerance) && std::abs(actual - expected) <= tolerance;
}

Result<atx::usize> validate_execution_and_count(const EquityAllocationDecision &d,
                                               const EquityAllocationExecution &e) {
    const auto n = d.preference.size();
    if (e.tri_units.size() != n || e.current_marks.size() != n || e.marked_dollars.size() != n ||
        e.decision_period != d.decision_period || e.decision_session_key != d.decision_session_key ||
        e.execution_period < e.decision_period || e.execution_session_key < e.decision_session_key ||
        !std::isfinite(e.cash) || !positive(e.pretrade_nav)) {
        return Err(ErrorCode::InvalidArgument, "equity allocation: malformed execution state/cutoff");
    }
    atx::f64 assets = 0.0;
    atx::f64 gross = 0.0;
    atx::usize union_count = 0;
    for (atx::usize i = 0; i < n; ++i) {
        const bool held = e.tri_units[i] != 0.0;
        if (!std::isfinite(e.tri_units[i]) || !std::isfinite(e.marked_dollars[i]) ||
            (held && (!positive(e.current_marks[i]) || e.marked_dollars[i] == 0.0 ||
                      e.tri_units[i] * e.current_marks[i] != e.marked_dollars[i])) ||
            (!held && e.marked_dollars[i] != 0.0)) {
            return Err(ErrorCode::InvalidArgument,
                       "equity allocation: inconsistent/missing held execution mark" + at_instrument(i));
        }
        const bool ready = d.valid_return_count[i] == kEquityAllocationReturnCount;
        if (held && d.eligibility[i] != 0 && !ready) {
            return Err(ErrorCode::InvalidArgument,
                       "equity allocation: held eligible name lacks 63 adjacent risk returns" + at_instrument(i));
        }
        if (held || (d.eligibility[i] != 0 && ready)) ++union_count;
        assets += e.marked_dollars[i];
        gross += std::abs(e.marked_dollars[i]);
        if (!std::isfinite(assets) || !std::isfinite(gross) ||
            !std::isfinite(e.marked_dollars[i] / e.pretrade_nav)) {
            return Err(ErrorCode::OutOfRange, "equity allocation: marked holding sum/weight overflow");
        }
    }
    const auto computed_nav = e.cash + assets;
    if (!positive(computed_nav) || !reconciled(computed_nav, e.pretrade_nav, gross, n)) {
        return Err(ErrorCode::InvalidArgument, "equity allocation: pretrade cash/holdings/NAV do not reconcile");
    }
    return Ok(union_count);
}

void identify_required_zeros(const EquityAllocationDecision &d, const EquityAllocationExecution &e,
                            EquityAllocationResult &out) {
    // Caller has already validated every held mark and admitted canonical storage.
    // This is price availability within the declared hypothetical-close model,
    // not evidence of an exchange halt or publication-time availability.
    const bool conditional = d.config.execution_availability ==
                             EquityExecutionAvailability::ObservedCloseEntryConstraintV1;
    out.required_zero_reasons.resize(d.preference.size(), 0);
    auto &cert = out.certificate;
    for (atx::usize i = 0; i < d.preference.size(); ++i) {
        const bool held = e.tri_units[i] != 0.0;
        const bool eligible = d.eligibility[i] != 0;
        const bool ready = d.valid_return_count[i] == kEquityAllocationReturnCount;
        const bool unavailable = !held && !positive(e.current_marks[i]);
        atx::u8 reason = 0;
        if (!eligible) reason |= static_cast<atx::u8>(EquityAllocationZeroReason::DecisionIneligible);
        if (!ready) reason |= static_cast<atx::u8>(EquityAllocationZeroReason::RiskUnready);
        if (unavailable) {
            ++cert.execution_unavailable_count;
            if (conditional) reason |= static_cast<atx::u8>(EquityAllocationZeroReason::ExecutionCloseUnavailable);
        }
        out.required_zero_reasons[i] = reason;
        // Keep the original canonical union even when an entry is unpriced.
        if (held || (eligible && ready)) {
            if (!eligible) ++cert.fixed_zero_count;
            if (reason != 0) ++cert.required_zero_count;
            if (conditional && unavailable) ++cert.execution_fixed_zero_count;
        }
    }
}

Status prepare_representation(const EquityAllocationDecision &d, const EquityAllocationExecution &e,
                              EquityAllocationResult &out) {
    using atx::engine::book::ReplayTargetAction;
    const auto n = d.preference.size();
    auto &cert = out.certificate;
    const auto &c = d.config;
    const bool machine = c.representation == EquityAllocationRepresentation::MachinePrecisionIntentsV1;
    const auto fee_rate = c.trade_bps * 1e-4;
    const auto fee_amplification = 1.0 + fee_rate + fee_rate * c.gross_limit + fee_rate * c.name_limit;
    atx::f64 per_name_cap = 0.0;
    if (machine) {
        cert.representation_l1_budget =
            (c.feasibility_tolerance * cert.fee_reserve / 128.0) / fee_amplification;
        per_name_cap = cert.representation_l1_budget / static_cast<atx::f64>(n);
        if (!positive(fee_amplification) || !positive(cert.representation_l1_budget) ||
            !positive(per_name_cap)) {
            return Err(ErrorCode::OutOfRange, "equity allocation: invalid representation change budget");
        }
    }
    out.weights.assign(n, 0.0);
    out.intents.resize(n);
    for (atx::usize i = 0; i < n; ++i) {
        const auto raw = out.continuous_weights[i];
        if (!std::isfinite(raw))
            return Err(ErrorCode::InvalidArgument, "equity allocation: nonfinite continuous weight" + at_instrument(i));
        const auto previous = e.marked_dollars[i] / e.pretrade_nav;
        const auto reason = out.required_zero_reasons[i];
        const bool fixed = reason != 0;
        auto &intent = out.intents[i];
        auto &weight = out.weights[i];
        weight = fixed ? 0.0 : raw;
        intent = {ReplayTargetAction::TargetWeight, weight};
        if (fixed) {
            // Mandatory lifting is a separate pre-existing transform, covered
            // by the propagated QP row tolerance rather than ordinary eta.
            cert.fixed_zero_l1_change += std::abs(raw);
            const auto decision_reasons = static_cast<atx::u8>(EquityAllocationZeroReason::DecisionIneligible) |
                                          static_cast<atx::u8>(EquityAllocationZeroReason::RiskUnready);
            if ((reason & decision_reasons) != 0) cert.decision_fixed_zero_l1_change += std::abs(raw);
            else cert.execution_fixed_zero_l1_change += std::abs(raw);
            if (machine) intent = {ReplayTargetAction::Close, 0.0};
        } else if (machine) {
            const auto eta = std::min(64.0 * std::numeric_limits<atx::f64>::epsilon() *
                std::max({1.0, std::abs(raw), std::abs(previous)}), per_name_cap);
            if (!positive(eta))
                return Err(ErrorCode::OutOfRange, "equity allocation: invalid representation eta");
            cert.representation_max_eta = std::max(cert.representation_max_eta, eta);
            if (std::abs(raw) <= eta) {
                weight = 0.0;
                intent = {ReplayTargetAction::Close, 0.0};
            } else if (std::abs(raw - previous) <= eta) {
                weight = previous;
                intent = {ReplayTargetAction::HoldCurrent, 0.0};
            }
            cert.representation_l1_change += std::abs(weight - raw);
        }
        if (intent.action == ReplayTargetAction::Close) ++cert.close_count;
        if (intent.action == ReplayTargetAction::HoldCurrent) ++cert.hold_count;
        if (!std::isfinite(cert.fixed_zero_l1_change) || !std::isfinite(cert.representation_l1_change) ||
            !std::isfinite(cert.decision_fixed_zero_l1_change) ||
            !std::isfinite(cert.execution_fixed_zero_l1_change))
            return Err(ErrorCode::OutOfRange, "equity allocation: representation change overflow");
    }
    // No relaxation for accumulated roundoff: this policy admits only a full
    // vector within its declared aggregate bound. Economic checks follow.
    if (cert.representation_l1_change > cert.representation_l1_budget)
        return Err(ErrorCode::InvalidArgument, "equity allocation: aggregate representation change budget exceeded");
    return Ok();
}

Status certify(const EquityAllocationDecision &d, const EquityAllocationExecution &e,
               EquityAllocationResult &out) {
    auto &cert = out.certificate;
    const auto &c = d.config;
    const auto tol = c.feasibility_tolerance;
    const auto n = out.weights.size();
    const auto prefee_cap = c.name_limit * cert.fee_reserve;
    atx::f64 assets = 0.0;
    atx::f64 gross = 0.0;
    atx::f64 max_name = 0.0;
    atx::f64 cash = e.cash;
    atx::f64 gap_compensation = 0.0;
    for (atx::usize i = 0; i < n; ++i) {
        const auto w = out.weights[i];
        const auto old_weight = e.marked_dollars[i] / e.pretrade_nav;
        const bool fixed = out.required_zero_reasons[i] != 0;
        if (!std::isfinite(w) || (fixed && w != 0.0) || std::abs(w) > prefee_cap + tol) {
            return Err(ErrorCode::InvalidArgument,
                       "equity allocation: original-unit box/mandatory-zero certificate failed" + at_instrument(i));
        }
        cert.requested_prefee_net += w;
        cert.requested_prefee_gross += std::abs(w);
        cert.requested_turnover += std::abs(w - old_weight);
        const auto diff = w - d.preference[i];
        cert.objective += 0.5 * diff * diff + c.risk_penalty * (d.variance[i] * w * w);
        auto sized = atx::engine::book::resolve_replay_target(out.intents[i], e.tri_units[i],
            e.marked_dollars[i], e.current_marks[i], e.pretrade_nav, !fixed);
        if (!sized)
            return Err(sized.error().code(), "equity allocation: candidate intent failed" +
                       at_instrument(i) + ": " + std::string(sized.error().message()));
        const auto value = sized->marked_dollars;
        const auto delta = sized->dollar_delta;
        const auto actual_weight = value / e.pretrade_nav;
        const auto raw = out.continuous_weights[i];
        const auto raw_diff = raw - d.preference[i];
        const auto actual_diff = actual_weight - d.preference[i];
        const auto weight_delta = actual_weight - raw;
        cert.continuous_objective += 0.5 * raw_diff * raw_diff + c.risk_penalty * (d.variance[i] * raw * raw);
        cert.represented_objective += 0.5 * actual_diff * actual_diff +
                                      c.risk_penalty * (d.variance[i] * actual_weight * actual_weight);
        cert.actual_representation_l1_change += std::abs(weight_delta);
        // Compute f(raw + delta) - f(raw) directly rather than subtracting
        // nearly identical objective totals. Compensate the signed sum too.
        const auto gap_term = weight_delta *
            (raw_diff + 0.5 * weight_delta + c.risk_penalty * d.variance[i] * (actual_weight + raw));
        const auto corrected_gap = gap_term - gap_compensation;
        const auto next_gap = cert.representation_objective_gap + corrected_gap;
        gap_compensation = (next_gap - cert.representation_objective_gap) - corrected_gap;
        cert.representation_objective_gap = next_gap;
        cash -= delta;
        cert.traded_dollars += std::abs(delta);
        assets += value;
        gross += std::abs(value);
        max_name = std::max(max_name, std::abs(value));
        if (!std::isfinite(delta) || !std::isfinite(cash) || !std::isfinite(assets) ||
            !std::isfinite(gross) || !std::isfinite(cert.traded_dollars) ||
            !std::isfinite(cert.requested_prefee_net) || !std::isfinite(cert.requested_prefee_gross) ||
            !std::isfinite(cert.requested_turnover) || !std::isfinite(cert.objective) ||
            !std::isfinite(actual_weight) || !std::isfinite(cert.continuous_objective) ||
            !std::isfinite(cert.represented_objective) || !std::isfinite(cert.actual_representation_l1_change) ||
            !std::isfinite(gap_term) || !std::isfinite(gap_compensation) ||
            !std::isfinite(cert.representation_objective_gap)) {
            return Err(ErrorCode::OutOfRange, "equity allocation: candidate accounting/objective overflow");
        }
    }
    cert.trade_cost = cert.traded_dollars * (c.trade_bps * 1e-4);
    cert.posttrade_cash = cash - cert.trade_cost;
    cert.posttrade_nav = cert.posttrade_cash + assets;
    cert.actual_turnover = cert.traded_dollars / e.pretrade_nav;
    if (!std::isfinite(cert.trade_cost) || !std::isfinite(cert.posttrade_cash) ||
        !positive(cert.posttrade_nav) || !std::isfinite(cert.actual_turnover) ||
        !reconciled(cert.posttrade_nav, e.pretrade_nav - cert.trade_cost, gross, n)) {
        return Err(ErrorCode::InvalidArgument, "equity allocation: post-fee NAV/cash conservation certificate failed");
    }
    cert.postfee_net = assets / cert.posttrade_nav;
    cert.postfee_gross = gross / cert.posttrade_nav;
    cert.postfee_max_name = max_name / cert.posttrade_nav;
    if (std::abs(cert.requested_prefee_net) > tol ||
        cert.requested_prefee_gross > c.gross_limit * cert.fee_reserve + tol ||
        cert.requested_turnover > c.turnover_limit + tol ||
        cert.actual_turnover > c.turnover_limit + tol ||
        !std::isfinite(cert.postfee_net) || std::abs(cert.postfee_net) > tol ||
        !std::isfinite(cert.postfee_gross) || cert.postfee_gross > c.gross_limit + tol ||
        !std::isfinite(cert.postfee_max_name) || cert.postfee_max_name > c.name_limit + tol) {
        std::ostringstream detail;
        detail << std::setprecision(std::numeric_limits<atx::f64>::max_digits10)
               << "equity allocation: original-unit/post-fee net/gross/name/turnover certificate failed"
               << "; tolerance=" << tol
               << " prefee_net=" << cert.requested_prefee_net
               << " prefee_gross=" << cert.requested_prefee_gross
               << " prefee_gross_limit=" << c.gross_limit * cert.fee_reserve
               << " requested_turnover=" << cert.requested_turnover
               << " actual_turnover=" << cert.actual_turnover
               << " turnover_limit=" << c.turnover_limit
               << " postfee_net=" << cert.postfee_net
               << " postfee_gross=" << cert.postfee_gross
               << " gross_limit=" << c.gross_limit
               << " postfee_max_name=" << cert.postfee_max_name
               << " name_limit=" << c.name_limit
               << " effective_solver_feasibility_tolerance="
               << cert.effective_solver_feasibility_tolerance
               << " solver_prim_res=" << cert.solver.prim_res
               << " solver_dual_res=" << cert.solver.dual_res;
        return Err(ErrorCode::InvalidArgument, detail.str());
    }
    return Ok();
}
} // namespace

Result<EquityAllocationDecision> freeze_equity_allocation_decision(
    const EquityAllocationRiskWindow &window, std::span<const atx::f64> preference,
    std::span<const atx::u8> eligibility, const EquityAllocationConfig &config) {
    ATX_TRY_VOID(validate_config(config));
    const auto n = preference.size();
    ATX_TRY(auto cells, mul(static_cast<atx::u64>(n), kEquityAllocationReturnCount + 1));
    ATX_TRY(auto bytes, snapshot_bound(n));
    if (bytes > config.max_additional_bytes) {
        return Err(ErrorCode::OutOfRange, "equity allocation: frozen snapshot exceeds additional byte budget");
    }
    if (n == 0 || eligibility.size() != n || window.closes.size() != cells ||
        (!window.observed.empty() && window.observed.size() != cells) ||
        window.session_keys.size() != kEquityAllocationReturnCount + 1 ||
        window.decision_context_row < window.first_context_row ||
        window.decision_context_row - window.first_context_row != kEquityAllocationReturnCount ||
        window.decision_context_row == std::numeric_limits<atx::usize>::max() ||
        window.session_keys.back() != window.decision_session_key) {
        return Err(ErrorCode::InvalidArgument, "equity allocation: expected exactly 64 rows ending at decision");
    }
    for (atx::usize t = 1; t < window.session_keys.size(); ++t) {
        if (window.session_keys[t - 1] >= window.session_keys[t])
            return Err(ErrorCode::InvalidArgument, "equity allocation: non-increasing risk sessions");
    }
    for (atx::usize i = 0; i < n; ++i) {
        if (!std::isfinite(preference[i]) || eligibility[i] > 1)
            return Err(ErrorCode::InvalidArgument, "equity allocation: invalid preference/eligibility" + at_instrument(i));
    }
    if (!window.observed.empty()) {
        for (atx::usize cell = 0; cell < window.observed.size(); ++cell) {
            if (window.observed[cell] > 1 ||
                (window.observed[cell] == 1 && !positive(window.closes[cell]))) {
                return Err(ErrorCode::InvalidArgument, "equity allocation: contradictory observed-close flag");
            }
        }
    }
    EquityAllocationDecision d;
    d.config = config;
    d.decision_period = window.decision_period;
    d.first_context_row = window.first_context_row;
    d.decision_context_row = window.decision_context_row;
    d.first_risk_session_key = window.session_keys.front();
    d.decision_session_key = window.decision_session_key;
    d.preference.assign(preference.begin(), preference.end());
    d.eligibility.assign(eligibility.begin(), eligibility.end());
    d.variance.assign(n, config.variance_floor);
    d.valid_return_count.assign(n, 0);
    for (atx::usize i = 0; i < n; ++i) {
        std::array<atx::f64, kEquityAllocationReturnCount> returns{};
        atx::f64 mean = 0.0;
        for (atx::usize t = 1; t <= kEquityAllocationReturnCount; ++t) {
            const auto before = (t - 1) * n + i;
            const auto after = t * n + i;
            if ((!window.observed.empty() &&
                 (window.observed[before] == 0 || window.observed[after] == 0)) ||
                !positive(window.closes[before]) || !positive(window.closes[after])) continue;
            const auto r = window.closes[after] / window.closes[before] - 1.0;
            if (!std::isfinite(r)) continue;
            returns[t - 1] = r;
            ++d.valid_return_count[i];
            mean += r / static_cast<atx::f64>(kEquityAllocationReturnCount);
        }
        if (d.valid_return_count[i] != kEquityAllocationReturnCount) continue;
        atx::f64 variance = 0.0;
        for (const auto r : returns) {
            const auto delta = r - mean;
            variance += (delta * delta) / static_cast<atx::f64>(kEquityAllocationReturnCount);
        }
        if (!std::isfinite(mean) || !nonnegative(variance)) {
            return Err(ErrorCode::OutOfRange, "equity allocation: nonfinite risk moments" + at_instrument(i));
        }
        d.variance[i] = std::max(config.variance_floor, variance);
    }
    return Ok(std::move(d));
}

Result<EquityAllocationPlan> plan_equity_allocation(atx::usize canonical, atx::usize count,
                                                   const EquityAllocationConfig &config) {
    ATX_TRY_VOID(validate_config(config));
    if (canonical == 0 || count > canonical)
        return Err(ErrorCode::InvalidArgument, "equity allocation: invalid canonical/union dimensions");
    ATX_TRY(auto resident_arrays, mul(static_cast<atx::u64>(canonical), 256));
    // Includes canonical continuous/represented weights, intents (<=16 bytes),
    // union indices, one-byte zero reasons and input/decision snapshots; scalar
    // sizing adds no N-array.
    static_assert(sizeof(atx::engine::book::ReplayTargetIntent) <= 16);
    if (count == 0) {
        ATX_TRY(auto bytes, add(resident_arrays, 65'536));
        if (bytes > config.max_additional_bytes)
            return Err(ErrorCode::OutOfRange, "equity allocation: empty-union arrays exceed additional byte budget");
        return Ok(EquityAllocationPlan{canonical, 0, 0, 0, 0, bytes});
    }
    const auto m = static_cast<atx::u64>(count);
    ATX_TRY(auto three_m, mul(m, 3));
    ATX_TRY(auto seven_m, mul(m, 7));
    ATX_TRY(auto n, add(three_m, 1));
    ATX_TRY(auto rows, add(seven_m, 4));
    ATX_TRY(auto kkt, add(n, rows));
    ATX_TRY(auto forty_m, mul(m, 40));
    ATX_TRY(auto kkt_nnz, add(forty_m, 7));
    ATX_TRY(auto twice_kkt, mul(kkt, 2));
    ATX_TRY(auto amd_extra, add(kkt_nnz / 5, twice_kkt));
    ATX_TRY(auto amd_nnz, add(kkt_nnz, amd_extra));
    // This also bounds 8*(D+1) in Eigen's AMD scratch-index arithmetic.
    ATX_TRY(auto kkt_plus_one, add(kkt, 1));
    ATX_TRY(auto amd_work, mul(kkt_plus_one, 8));
    ATX_TRY(auto worst_count, mul(kkt_plus_one, kkt_plus_one));
    if (amd_nnz > static_cast<atx::u64>(std::numeric_limits<int>::max()) / 4 ||
        amd_work > static_cast<atx::u64>(std::numeric_limits<int>::max()) / 4 ||
        worst_count > static_cast<atx::u64>(std::numeric_limits<int>::max()) / 4)
        return Err(ErrorCode::OutOfRange, "equity allocation: sparse/AMD index bound exceeded");
    // Descriptor-specific bounds; do not reuse for arbitrary ConstraintSets.
    // Atilde has 15M+1 entries, INCLUDING M explicit zero factor exposures;
    // full symmetric KKT has 40M+7. Eigen AMD copies the selfadjoint pattern,
    // then reserves nnz+nnz/5+2D entries and 8(D+1) integer workspace.
    // 64(M+1)^2 covers 8M(M+1) dense A and 16(M^2+10M+1)
    // triplet reserve, including growth/copies. 8192D separately covers all
    // original/scaled/active/KKT CSC copies, conversions, AMD, permuted Kup,
    // linear solver vectors and factor-model/snapshot/result arrays. The small-M
    // triplet-reserve undercount is covered by this deliberately broad linear term.
    // Factor Li/Lx can be dense regardless of these sparse patterns: BOTH calls
    // must enforce max_factor_bytes before allocating, so reserve twice that cap.
    // The ADMM and polish factors do not coexist, but we do not rely on reuse.
    // 64MiB fixed allowance covers container/solver overhead; not OS RSS enforcement.
    static_assert(sizeof(atx::f64) == 8 && sizeof(atx::usize) <= 8 && sizeof(int) <= 4);
    static_assert(sizeof(Eigen::Triplet<atx::f64>) <= 16);
    ATX_TRY(auto m_plus_one, add(m, 1));
    ATX_TRY(auto dense_cells, mul(m_plus_one, m_plus_one));
    ATX_TRY(auto matrix_bytes, mul(dense_cells, 64));
    ATX_TRY(auto linear_bytes, mul(kkt, 8192));
    ATX_TRY(auto factor_bytes, mul(config.solver.max_factor_bytes, 2));
    ATX_TRY(auto bytes0, add(matrix_bytes, linear_bytes));
    ATX_TRY(auto bytes1, add(bytes0, resident_arrays));
    ATX_TRY(auto bytes2, add(bytes1, factor_bytes));
    ATX_TRY(auto bytes, add(bytes2, 67'108'864));
    if (bytes > config.max_additional_bytes) {
        return Err(ErrorCode::OutOfRange,
                   "equity allocation: conservative solver workspace exceeds additional byte budget; union=" +
                   std::to_string(count) + " required=" + std::to_string(bytes) +
                   " budget=" + std::to_string(config.max_additional_bytes));
    }
    return Ok(EquityAllocationPlan{canonical, count, n, rows, kkt, bytes});
}

Result<EquityAllocationResult> allocate_equity_preference(const EquityAllocationDecision &d,
                                                         const EquityAllocationExecution &e) {
    ATX_TRY_VOID(validate_decision(d));
    ATX_TRY(auto count, validate_execution_and_count(d, e));
    ATX_TRY(auto plan, plan_equity_allocation(d.preference.size(), count, d.config));
    EquityAllocationResult out;
    out.plan = plan;
    out.continuous_weights.assign(d.preference.size(), 0.0);
    out.union_indices.reserve(count);
    out.certificate.fee_reserve = 1.0 - (d.config.trade_bps * 1e-4) * d.config.turnover_limit;
    for (atx::usize i = 0; i < d.preference.size(); ++i) {
        if (e.tri_units[i] != 0.0 || (d.eligibility[i] != 0 &&
            d.valid_return_count[i] == kEquityAllocationReturnCount)) out.union_indices.push_back(i);
    }
    identify_required_zeros(d, e, out);
    if (count != 0) {
        std::vector<atx::f64> previous(count);
        std::vector<atx::f64> q(count);
        atx::core::linalg::VecX diagonal(static_cast<Eigen::Index>(count));
        atx::f64 forced_turnover = 0.0;
        for (atx::usize j = 0; j < count; ++j) {
            const auto i = out.union_indices[j];
            previous[j] = e.marked_dollars[i] / e.pretrade_nav;
            q[j] = -d.preference[i];
            // A fixed exit has zero risk contribution and needs no risk estimate.
            const auto hessian = d.eligibility[i] == 0 ? 1.0 :
                1.0 + 2.0 * d.config.risk_penalty * d.variance[i];
            if (!positive(hessian))
                return Err(ErrorCode::OutOfRange, "equity allocation: Hessian overflow" + at_instrument(i));
            diagonal[static_cast<Eigen::Index>(j)] = hessian;
            if (d.eligibility[i] == 0) {
                forced_turnover += std::abs(previous[j]);
            }
        }
        if (!std::isfinite(forced_turnover) ||
            forced_turnover > d.config.turnover_limit + d.config.feasibility_tolerance) {
            return Err(ErrorCode::InvalidArgument,
                       "equity allocation: mandatory exits alone exceed hard turnover budget");
        }
        auto exposures = atx::core::linalg::MatX::Zero(static_cast<Eigen::Index>(count), 1).eval();
        auto factor = atx::core::linalg::MatX::Identity(1, 1).eval();
        ATX_TRY(auto model, risk::FactorModel::create(std::move(exposures), std::move(factor),
            std::move(diagonal), d.first_context_row, d.decision_context_row + 1));
        risk::ConstraintSet constraints;
        constraints.gross = {d.config.gross_limit * out.certificate.fee_reserve, true};
        constraints.pos = risk::PositionCap{d.config.name_limit * out.certificate.fee_reserve};
        constraints.turn = risk::TurnoverBudget{d.config.turnover_limit};
        ATX_TRY(auto materialized, constraints.materialize(model.exposures(), previous, count));
        if (materialized.A.rows() != static_cast<Eigen::Index>(count + 1))
            return Err(ErrorCode::Internal, "equity allocation: unexpected constraint row layout");
        for (atx::usize j = 0; j < count; ++j) {
            if (out.required_zero_reasons[out.union_indices[j]] != 0) {
                // ConstraintSet documents net first, then canonical identity boxes.
                materialized.l[static_cast<Eigen::Index>(j + 1)] = 0.0;
                materialized.u[static_cast<Eigen::Index>(j + 1)] = 0.0;
            }
        }
        risk::ConstrainedQpSolver solver;
        solver.cfg = d.config.solver;
        // If every augmented row is feasible within eps, |w_i| <= s_i+eps
        // and sum(s_i) <= G+eps imply gross <= G+(M+1)*eps. The same
        // propagation applies to turnover. Lifting F mathematically fixed-zero
        // coordinates (decision exits or unpriced exact-unheld entries) can add
        // F*eps to turnover/net. F<=M still holds, so B=2M+1 bounds all
        // pre-fee economic errors. A row tolerance equal to the economic
        // tolerance is therefore insufficient, including for only two names.
        // With r=1-c*T and delta=B*eps, post-fee gross excess is bounded by
        // delta*(1+c*G)/(r-c*delta), and likewise for the name cap. Choosing
        // eps <= tol*r/[8*B*(1+c*(1+G+N))] leaves margin for these errors and
        // representable sizing. This is an algebraic propagation bound, not a
        // floating-point proof: the independent economic/accounting gate below
        // remains mandatory. Fixed iteration counts and actual limits stay put;
        // this also prevents accepting a regularized polish with accumulated
        // L1 violations. Optional execution representation has its own smaller
        // change budget and never changes the raw solver book/certificate.
        const auto fee_rate = d.config.trade_bps * 1e-4;
        const auto propagation = 2.0 * static_cast<atx::f64>(count) + 1.0;
        const auto fee_amplification = 1.0 + fee_rate +
            fee_rate * d.config.gross_limit + fee_rate * d.config.name_limit;
        // Sequential divisions avoid overflow in the positive denominator.
        const auto propagated_tolerance =
            ((d.config.feasibility_tolerance * out.certificate.fee_reserve / 8.0) /
             propagation) / fee_amplification;
        if (!positive(propagation) || !positive(fee_amplification) ||
            !positive(propagated_tolerance)) {
            return Err(ErrorCode::OutOfRange,
                       "equity allocation: nonfinite/underflowed propagated solver tolerance");
        }
        solver.cfg.feas_tol = std::min(solver.cfg.feas_tol, propagated_tolerance);
        out.certificate.effective_solver_feasibility_tolerance = solver.cfg.feas_tol;
        auto solved = solver.solve_with_cert(risk::QpProblem{model, 0.5, q, materialized});
        if (!solved) {
            return Err(solved.error().code(),
                       "equity allocation: solver did not certify feasibility within fixed budget; union=" +
                       std::to_string(count) + " decision_period=" + std::to_string(e.decision_period) +
                       " execution_period=" + std::to_string(e.execution_period) + ": " +
                       std::string(solved.error().message()));
        }
        const auto &cert = solved->cert;
        if (solved->book.size() != count || !nonnegative(cert.prim_res) || !nonnegative(cert.dual_res) ||
            cert.prim_res > d.config.residual_tolerance || cert.dual_res > d.config.residual_tolerance ||
            cert.primal_infeasible || cert.dual_infeasible) {
            return Err(ErrorCode::InvalidArgument,
                       "equity allocation: solver residual/infeasibility certificate rejected");
        }
        out.certificate.solver = cert;
        out.certificate.solver_used = true;
        for (atx::usize j = 0; j < count; ++j) {
            const auto i = out.union_indices[j];
            if (!std::isfinite(solved->book[j]))
                return Err(ErrorCode::InvalidArgument, "equity allocation: nonfinite solver weight");
            out.continuous_weights[i] = solved->book[j];
        }
    }
    ATX_TRY_VOID(prepare_representation(d, e, out));
    ATX_TRY_VOID(certify(d, e, out));
    return Ok(std::move(out));
}

Result<EquityAllocationResult> represent_equity_allocation(const EquityAllocationDecision &d,
                                                          const EquityAllocationExecution &e,
                                                          std::span<const atx::f64> continuous_weights) {
    ATX_TRY_VOID(validate_decision(d));
    if (continuous_weights.size() != d.preference.size())
        return Err(ErrorCode::InvalidArgument, "equity allocation: continuous weight shape mismatch");
    ATX_TRY(auto count, validate_execution_and_count(d, e));
    ATX_TRY(auto plan, plan_equity_allocation(d.preference.size(), count, d.config));
    EquityAllocationResult out;
    out.plan = plan;
    out.continuous_weights.assign(continuous_weights.begin(), continuous_weights.end());
    out.union_indices.reserve(count);
    out.certificate.fee_reserve = 1.0 - (d.config.trade_bps * 1e-4) * d.config.turnover_limit;
    for (atx::usize i = 0; i < d.preference.size(); ++i) {
        const bool in_union = e.tri_units[i] != 0.0 || (d.eligibility[i] != 0 &&
            d.valid_return_count[i] == kEquityAllocationReturnCount);
        if (in_union) {
            out.union_indices.push_back(i);
        } else if (continuous_weights[i] != 0.0) {
            return Err(ErrorCode::InvalidArgument,
                       "equity allocation: nonzero continuous weight outside canonical union" + at_instrument(i));
        }
    }
    identify_required_zeros(d, e, out);
    ATX_TRY_VOID(prepare_representation(d, e, out));
    ATX_TRY_VOID(certify(d, e, out));
    return Ok(std::move(out));
}

} // namespace atx::impl
