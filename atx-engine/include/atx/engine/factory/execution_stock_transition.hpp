#pragma once
#include "atx/engine/factory/execution_cash_claim.hpp"

namespace atx::engine::factory {
// Continuous research-share equivalents, NOT physical whole-share delivery.
// Actual fractional cash-in-lieu and account delivery/stock-loan operations are
// unresolved. No separate cash-in-lieu amount or loan discharge is invented.
enum class ExecutionStockFractionRule : atx::u8 { ContinuousResearchUnitsV1=1 };
enum class ExecutionStockBorrowRule : atx::u8 { ModeledSuccessorNoDischargeV1=1 };
struct ExecutionStockTransitionEvent {
  std::string event_id;
  atx::u64 revision{};
  atx::u64 predecessor_id{}, successor_id{};
  std::string security_id_namespace;
  std::string predecessor_identity, successor_identity;
  std::string panel_source_sha256;
  std::string predecessor_identity_evidence_sha256, successor_identity_evidence_sha256;
  std::string completion_evidence_sha256, basis_evidence_sha256;
  ExecutionCashClaimEvidence evidence{ExecutionCashClaimEvidence::ReconstructedPublicationV1};
  atx::i64 reference_mark_ns{};
  atx::f64 reference_raw_close{}, reference_adjusted_close{};
  atx::i64 effective_after_ns{}, effective_by_ns{}, available_at_ns{}, recognition_mark_ns{};
  atx::u64 stock_ratio_numerator{}, stock_ratio_denominator{};
  // Observed successor raw and adjusted mark at recognition, not a future
  // decision/sizing input. Both must match the present Panel cell exactly.
  atx::f64 successor_raw_close{}, successor_adjusted_close{};
  // Optional fixed USD consideration per predecessor raw-share equivalent.
  // This EXCLUDES unknown holder-level fractional cash. Zero means no fixed leg.
  atx::f64 fixed_cash_usd_per_raw_share{};
  bool stock_and_cash_excluded_from_adjusted_close{};
  ExecutionStockFractionRule fraction_rule{ExecutionStockFractionRule::ContinuousResearchUnitsV1};
  ExecutionStockBorrowRule borrow_rule{ExecutionStockBorrowRule::ModeledSuccessorNoDischargeV1};
};
struct ExecutionStockTransitionStreams;
// Exact prior cash/default route when stock_events is empty. Nonempty stock
// events require the explicit result below. Same strict public/effective clocks
// and previous observed predecessor raw/adjusted basis as cash claims.
// Existing successor holdings mark first, then signed delivery is additive;
// ordinary queued successor targets net against that new holding. The mandatory
// bridge has no modeled execution turnover/fee. Subsequent ordinary trades do.
// Known pre-role events retire only the predecessor, with no opening entitlement.
// Active in-role successor must exist on the role axis. Outside-axis, pre-role,
// and future events need no irrelevant successor mark. This recipe refuses event chains
// and a successor also terminal under another event. No delivery/locate guarantee.
[[nodiscard]] atx::core::Result<ExecutionObjectiveContext> prepare_execution_objective_transitions(
    const alpha::Panel& panel,const WeightPolicy& policy,const ExecutionObjectiveConfig& config,
    std::span<const cost::CostSurface> decision_snapshots,
    std::span<const atx::i64> mark_times_ns,std::span<const atx::i64> decision_times_ns,
    std::span<const atx::u64> instrument_ids,const ExecutionObjectiveIdentity& identity,
    std::span<const ExecutionCashClaimEvent> cash_events,
    std::span<const ExecutionStockTransitionEvent> stock_events,
    std::span<const atx::u8> decision_membership={},
    std::span<const atx::u32> bad_return_prefix={},std::span<const atx::u32> group_map={});
[[nodiscard]] atx::core::Result<ExecutionStockTransitionStreams> extract_execution_signal_transitions(
    std::span<const atx::f64> signal,const ExecutionObjectiveContext& context,atx::f64 sign=1.0);
} // namespace atx::engine::factory
