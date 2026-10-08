#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace atx::server {

/// Trusted frozen recipe and evidence boundary are resolved by the owning service.
/// This pure computation API performs no approval, persistence or broker operation.
struct PaperReplayInput {
  std::string approved_portfolio_request_json;
  std::string approved_evidence_end_date;
  std::string paper_start_date;
  std::string paper_end_date;
  std::uint32_t through_session_count{};
  /// Empty only for the first, cash-only observation. Later prefixes require exact IDs/order.
  std::vector<std::uint64_t> expected_cohort_security_ids;
};

struct PaperPosition {
  std::uint64_t security_id{};
  double tri_units{}, marked_dollars{};
  bool operator==(const PaperPosition&) const = default;
};

struct PaperAllocation {
  std::uint32_t decision_session_index{}, execution_session_index{};
  std::vector<double> target_weights, pretrade_held_weights, achieved_weights;
  double pretrade_cash{}, pretrade_nav{}, posttrade_cash{}, posttrade_nav{};
  double traded_dollars{}, trade_cost{};
  double target_net{}, target_gross{}, target_max_name{}, target_turnover{};
  double solver_primal_residual{}, solver_dual_residual{}, solver_rho_final{};
  bool solver_polished{};
  bool operator==(const PaperAllocation&) const = default;
};

/// A modeled TRI fill, not an exchange fill or a count of tradeable shares.
struct PaperFill {
  std::uint32_t session_index{}, decision_session_index{};
  std::uint64_t security_id{};
  double old_tri_units{}, new_tri_units{}, tri_mark{}, signed_dollar_delta{}, modeled_fee{};
  bool operator==(const PaperFill&) const = default;
};

struct PaperUnfilledTarget {
  std::uint32_t session_index{}, decision_session_index{};
  std::uint64_t security_id{};
  double requested_weight{};
  bool operator==(const PaperUnfilledTarget&) const = default;
};

struct PaperFinancing {
  std::uint32_t period_start_session_index{}, period_end_session_index{};
  double borrow_charge{}, calendar_days{};
  std::uint16_t day_basis{365};
  bool operator==(const PaperFinancing&) const = default;
};

/// Mandatory liquidation is separate from trading and carries no execution fee.
struct PaperLiquidation {
  std::uint32_t session_index{};
  std::uint64_t security_id{};
  double removed_tri_units{}, last_marked_dollars{}, applied_return{}, signed_proceeds{};
  std::string return_source;
  bool assumed{}, flagged{};
  bool operator==(const PaperLiquidation&) const = default;
};

/// Completed pre-execution valuation at this session; the interval starting here is pending.
struct PaperValuation {
  std::uint32_t session_index{};
  double cash{}, assets{}, gross{}, nav{}, gross_pnl{}, trade_fees{}, borrow_charge{}, net_return{};
  std::vector<PaperPosition> positions;
  bool operator==(const PaperValuation&) const = default;
};

/// Owns the full bounded ledger prefix; no sampled series or borrowed engine memory.
struct PaperReplayResult {
  std::vector<std::string> window_calendar;
  std::vector<std::uint64_t> cohort_security_ids;
  std::uint32_t through_session_count{};
  std::vector<PaperAllocation> allocations;
  std::vector<PaperFill> fills;
  std::vector<PaperUnfilledTarget> unfilled_targets;
  std::vector<PaperFinancing> financing;
  std::vector<PaperLiquidation> liquidations;
  std::vector<PaperValuation> valuations;
  std::vector<PaperPosition> final_positions;
  std::string receipt_json;
};

} // namespace atx::server
