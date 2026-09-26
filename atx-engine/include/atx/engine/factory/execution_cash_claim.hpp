#pragma once
#include <span>
#include <string>
#include "atx/engine/factory/execution_objective.hpp"

namespace atx::engine::factory {
// Publication clocks reconstructed from pinned sources are research evidence,
// not proof of a historical feed delivery. Synthetic fixtures are identified.
enum class ExecutionCashClaimEvidence : atx::u8 {
  SyntheticFixtureV1=1, ReconstructedPublicationV1=2
};
// Explicit, narrow all-cash USD completion. Every digest is an externally
// supplied assertion, not authentication by this decoder. No stock conversion,
// appraisal, tax, split or separate dividend entitlement is inferred.
struct ExecutionCashClaimEvent {
  std::string event_id;
  atx::u64 revision{};
  atx::u64 instrument_id{};
  std::string security_id_namespace;
  std::string historical_identity;
  std::string panel_source_sha256; // same role/source pin as ExecutionObjectiveIdentity
  std::string identity_evidence_sha256;
  std::string completion_evidence_sha256;
  std::string basis_evidence_sha256;
  ExecutionCashClaimEvidence evidence{ExecutionCashClaimEvidence::ReconstructedPublicationV1};
  atx::i64 reference_mark_ns{};
  atx::f64 reference_raw_close{};
  atx::f64 reference_adjusted_close{};
  // Completion lies in (effective_after_ns, effective_by_ns]. The upper bound
  // and public availability must BOTH precede recognition_mark_ns strictly.
  // This represents a verified before-open boundary without inventing an exact
  // closing instant. reference_mark <= effective_after < effective_by.
  atx::i64 effective_after_ns{};
  atx::i64 effective_by_ns{};
  atx::i64 available_at_ns{};
  atx::i64 recognition_mark_ns{};
  atx::f64 cash_usd_per_raw_share{};
  // Required affirmative basis assertion: the last raw/adjusted pair denotes
  // the same predecessor share basis; cash consideration is NOT already in TRI.
  bool cash_excluded_from_adjusted_close{};
};
struct ExecutionCashClaimStreams;

// At most 1024 events, one per instrument and event_id. Events are owned/copied.
// In-role reference is the immediately previous observed mark, including raw_close;
// exact matching to Panel prices/ID/source pin is required. Completion becomes
// state only at recognition, never a future universe filter. Empty events keep
// original hash and arithmetic. Known pre-role lines retire without an opening
// claim; out-of-axis and future events are hash-bound but inactive and reported.
// In-role recognition must be the first mark strictly after public completion.
// Unknown payment remains a claim.
// Policy: fixed USD face; positive receivables excluded from target NAV;
// negative claims reserve settled cash and retain the last admitted modeled
// annual short rate until role end. This is a conservative modeling convention,
// not evidence of actual stock-loan termination or financing. No settlement API.
[[nodiscard]] atx::core::Result<ExecutionObjectiveContext> prepare_execution_objective_claims(
    const alpha::Panel& panel,const WeightPolicy& policy,const ExecutionObjectiveConfig& config,
    std::span<const cost::CostSurface> decision_snapshots,
    std::span<const atx::i64> mark_times_ns,std::span<const atx::i64> decision_times_ns,
    std::span<const atx::u64> instrument_ids,const ExecutionObjectiveIdentity& identity,
    std::span<const ExecutionCashClaimEvent> events,
    std::span<const atx::u8> decision_membership={},
    std::span<const atx::u32> bad_return_prefix={},std::span<const atx::u32> group_map={});
// Include execution_cash_claim_streams.hpp when consuming this owning result.
[[nodiscard]] atx::core::Result<ExecutionCashClaimStreams> extract_execution_signal_claims(
    std::span<const atx::f64> signal,const ExecutionObjectiveContext& context,atx::f64 sign=1.0);
} // namespace atx::engine::factory
