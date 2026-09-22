#pragma once

#include <array>
#include <functional>
#include <optional>
#include <span>
#include <vector>

#include "atx/core/error.hpp"
#include "atx/core/types.hpp"
#include "atx/engine/book/claims_state.hpp"
#include "atx/engine/data/security_transition.hpp"

namespace atx::engine::alpha { class Panel; }

namespace atx::engine::book {

enum class ReplayDayBasis : atx::u16 { D360 = 360, D365 = 365 };

struct ReplayConfig {
  atx::f64 initial_nav{1.0};
  atx::usize execution_delay_periods{1};
  atx::f64 trade_bps{0.0};             // Per absolute dollar traded, each direction.
  atx::f64 annual_borrow_bps{0.0};     // Simple annual fee on post-trade short dollars.
  ReplayDayBasis borrow_day_basis{ReplayDayBasis::D365};
};

// One observed interval. cash/assets/nav are END valuations after financing;
// assets is signed, gross values are absolute dollar exposure. pretrade_nav is
// the START valuation before this interval's trade cost. decision_period is the
// most recently effective target's original decision index, including flat targets.
//
// SETTLED-CASH SEMANTICS (iteration-13 design §6 and §11 ruling 4). `cash` keeps
// its name and is SETTLED cash: a recognized-but-unpaid transition cash claim
// never lives in it, and is never netted against it. `settled_cash` repeats the
// same number under the claims ledger's own name so `ledger.csv` can append the
// columns below without renaming an existing one. The three non-claims entry
// points leave every added field at exactly 0.0, so their rows still satisfy
// `nav == cash + assets`; with claims outstanding the identity is
// `nav == cash + assets + signed_pending_claims`, and
//   nav(d+1) - nav(d) == gross_pnl + mandatory_value_bridge - trade_cost - borrow_cost,
// where `pretrade_nav` already carries this observation's mandatory_value_bridge
// (the bridge is applied before the trade, so nav(d) is the pre-event NAV).
// `mandatory_settled_cash` / `claim_recognized` / `claim_settled` are the
// observation's mandatory movements; none of them touches traded_dollars or
// trade_cost, which stay exactly 0.0 for every mandatory movement.
struct ReplayInterval {
  atx::usize start_period{};
  atx::usize end_period{};
  std::optional<atx::usize> decision_period;
  atx::f64 pretrade_nav{};
  atx::f64 cash{};
  atx::f64 assets{};
  atx::f64 nav{};
  atx::f64 gross_pnl{};
  atx::f64 trade_cost{};
  atx::f64 borrow_cost{};
  atx::f64 net_return{};
  atx::f64 traded_dollars{};
  atx::f64 start_gross{};
  atx::f64 end_gross{};
  atx::f64 settled_cash{};
  atx::f64 signed_pending_claims{};
  atx::f64 gross_receivable{};
  atx::f64 gross_payable{};
  atx::f64 mandatory_value_bridge{};
  atx::f64 mandatory_settled_cash{};
  atx::f64 claim_recognized{};
  atx::f64 claim_settled{};
};

// Actual nonzero external trade, in canonical period/instrument order. Positive
// dollar_delta buys, negative sells. TRI units are research holdings, not shares.
struct ReplayTrade {
  atx::usize period{};
  atx::usize decision_period{};
  atx::usize instrument{};
  atx::f64 dollar_delta{};
};

struct ReplayResult {
  std::vector<ReplayInterval> intervals;
  std::vector<ReplayTrade> trades;
  std::vector<atx::f64> final_tri_units;
  atx::f64 initial_nav{};
  atx::f64 final_cash{};
  atx::f64 final_assets{};
  atx::f64 final_nav{};
  atx::usize effective_rebalances{};  // Includes applied targets that trade zero dollars.
  atx::usize unexecuted_decisions{}; // Effective at/after the final valuation.
};

// Borrowed only for the duration of the policy call. Instrument spans have the
// panel's canonical order and full instrument count; the policy receives no panel
// or future rows. Every held position has a valid current mark before this view
// is constructed. Unused current marks may be missing/nonpositive/nonfinite.
struct ReplayAllocationState {
  atx::usize schedule_index{};
  atx::usize decision_period{};
  atx::usize execution_period{};
  atx::i64 decision_session_key{};
  atx::i64 execution_session_key{};
  atx::f64 cash{};
  atx::f64 pretrade_nav{};
  std::span<const atx::f64> tri_units;
  std::span<const atx::f64> current_marks;
  std::span<const atx::f64> marked_dollars;
  std::span<const atx::u8> decision_eligibility;
  std::span<const atx::f64> preference_weights;
  // Claims-aware terms; exactly 0.0 / empty for the three non-claims entry
  // points. `cash` above is SETTLED cash, so a pending claim is never spendable
  // from it, and pretrade_nav == cash + sum(marked_dollars) + signed_pending_claims.
  // `retired_representation` is one byte per canonical instrument, 1 == the
  // representation was retired by a committed transition: a policy may not
  // return a nonzero TargetWeight or a HoldCurrent on such a name, whatever its
  // original decision eligibility.
  atx::f64 signed_pending_claims{};
  atx::f64 gross_receivable{};
  atx::f64 gross_payable{};
  std::span<const PendingTransitionCashClaim> pending_claims;
  std::span<const atx::u8> retired_representation;
};

// Return owned weights of PRE-TRADE NAV, one per canonical instrument. The
// policy owns any risk/preference snapshot beyond the supplied frozen row and
// must enforce its own information cutoff. Engine inputs must remain immutable
// during replay, including through externally captured references.
using ReplayAllocationPolicy = std::function<atx::core::Result<std::vector<atx::f64>>(
    const ReplayAllocationState &)>;

// Explicit instructions avoid reconstructing a held unit count from a weight.
// HoldCurrent and Close require a finite zero weight payload. An empty vector is
// invalid: every canonical instrument must have an explicit instruction.
enum class ReplayTargetAction : atx::u8 { TargetWeight = 0, HoldCurrent = 1, Close = 2 };

struct ReplayTargetIntent {
  ReplayTargetAction action{ReplayTargetAction::TargetWeight};
  atx::f64 weight{};
};

struct ReplaySizedTarget {
  atx::f64 tri_units{};
  atx::f64 marked_dollars{};
  atx::f64 dollar_delta{};
  // Requested weight for TargetWeight, current marked dollars / NAV for Hold,
  // and zero for Close. This diagnostic value never resizes a held position.
  atx::f64 resolved_weight{};
};

// Pure scalar sizing shared with policy certification; no allocation on success.
// Validates positive NAV and consistent finite starting units/marked dollars.
// Every held position requires a finite positive price even when closing. An
// unheld zero target/Hold/Close may have an invalid unused mark. Nonzero resulting
// positions, including Hold, require original decision eligibility. Hold copies
// units/value exactly and returns +0 delta; Close returns +0 units/value. Weight
// uses (weight * pretrade_nav) / current_mark, then units * current_mark, retaining
// the fixed replay's operation order and rejection of overflow/underflow.
[[nodiscard]] atx::core::Result<ReplaySizedTarget>
resolve_replay_target(const ReplayTargetIntent &intent, atx::f64 current_tri_units,
                      atx::f64 current_marked_dollars, atx::f64 current_mark,
                      atx::f64 pretrade_nav, bool decision_eligible);

using ReplayIntentPolicy = std::function<atx::core::Result<std::vector<ReplayTargetIntent>>(
    const ReplayAllocationState &)>;

// Accepted allocations are separate from the frozen input preferences. Dollars
// reflect actual representable TRI units; posttrade NAV includes the trade fee,
// before the following interval's price move and borrow charge. No row is exposed
// on a failed replay, including failure after earlier successful allocations.
struct ReplayAllocation {
  atx::usize schedule_index{};
  atx::usize decision_period{};
  atx::usize execution_period{};
  atx::i64 decision_session_key{};
  atx::i64 execution_session_key{};
  atx::f64 pretrade_cash{};
  atx::f64 pretrade_nav{};
  atx::f64 posttrade_cash{};
  atx::f64 posttrade_nav{};
  atx::f64 traded_dollars{};
  atx::f64 trade_cost{};
  std::vector<atx::f64> target_weights;
  std::vector<atx::f64> pretrade_marked_dollars;
  std::vector<atx::f64> posttrade_marked_dollars;
  // Empty for both older APIs. Intent replay retains original instructions;
  // target_weights then contains their resolved_weight diagnostics defined above.
  std::vector<ReplayTargetIntent> target_intents;
};

struct ReplayPolicyResult {
  ReplayResult replay;
  // Additional payload for E effective decisions and N instruments: three N-
  // double arrays per allocation, plus allocation objects reserved for the full
  // input schedule. Replay also keeps N eligibility bytes while running. Callback
  // workspace, returned-vector spare capacity and allocator overhead are extra;
  // callers imposing a process budget must account for these owned snapshots.
  // Intent replay additionally owns E*N*sizeof(ReplayTargetIntent) payload bytes;
  // the extra vector object exists for every reserved allocation in all policy
  // paths. The resolver uses scalar storage; no additional price arrays are kept.
  std::vector<ReplayAllocation> allocations;
};

// Deterministic cold-path cash/holdings replay, without file I/O or capacity models.
// Uses the unmasked close field as total-return prices: dividends are implicitly
// reinvested, no cash interest or separate dividend flow. Starts entirely in cash.
// Targets are schedule-major weights of PRE-TRADE NAV and execute at decision +
// delay. Every nonzero target must be eligible at its decision date. Eligibility
// changes never suppress existing holdings' subsequent marks or P&L.
//
// Validates positive shape/NAV, strictly ordered schedule/times, finite targets,
// nonnegative finite cost rates, D360/D365 and checked index/duration arithmetic.
// At least one observation is allowed; final observation is valuation-only, with
// no trade/fee/liquidation. All D-1 intervals are recorded, including initial cash.
// Missing/nonpositive/nonfinite required marks and invalid resulting NAV fail with
// an error; missing unused marks are ignored. Inputs are never mutated and failures
// return no partial result. Allocation failure may throw. Durations must fit i64 ns.
// Instrument-specific errors end with " at period=<index> instrument=<index>";
// callers can map those zero-based indices to their externally verified axes.
[[nodiscard]] atx::core::Result<ReplayResult>
replay_scheduled_targets(const alpha::Panel &research,
                         std::span<const atx::i64> session_keys,
                         std::span<const atx::usize> decision_periods,
                         std::span<const atx::f64> target_weights,
                         const ReplayConfig &config = {});

// Same accounting and input validation as fixed-target replay. At each delayed,
// nonterminal execution, first mark all held units and validate NAV, then invoke
// the nonempty policy exactly once. Preferences/eligibility come from the original
// decision row. Validate its owned result for exact shape, finiteness, original
// decision eligibility, required marks and cash conservation before accepting it.
// No liquidity, hard exposure, risk or borrowing-availability constraints are
// implied by this seam; those belong to the policy. Callback errors propagate;
// allocation/callback exceptions may escape. Either failure returns no result;
// external side effects performed by a callback cannot be rolled back.
// Config is explicit to preserve existing five-argument calls with `{}`.
[[nodiscard]] atx::core::Result<ReplayPolicyResult>
replay_scheduled_targets(const alpha::Panel &research,
                         std::span<const atx::i64> session_keys,
                         std::span<const atx::usize> decision_periods,
                         std::span<const atx::f64> preference_weights,
                         const ReplayAllocationPolicy &policy,
                         const ReplayConfig &config);

// Opt-in explicit instructions at the same delayed, nonterminal policy seam.
// Requires a nonempty callback and exactly N instructions from each call. Uses
// resolve_replay_target and the shared replay accounting; timing, frozen input
// validation, eligibility, required marks and all-or-error publication contracts
// above remain in force. Hold is not permission to retain an ineligible target.
[[nodiscard]] atx::core::Result<ReplayPolicyResult>
replay_scheduled_intents(const alpha::Panel &research,
                         std::span<const atx::i64> session_keys,
                         std::span<const atx::usize> decision_periods,
                         std::span<const atx::f64> preference_weights,
                         const ReplayIntentPolicy &policy,
                         const ReplayConfig &config);

// ---------------------------------------------------------------------------
// Claims-aware replay (iteration-13 design §3-§6). Opt-in: the three entry
// points above are untouched and keep their exact accounting.
// ---------------------------------------------------------------------------

// A terminal predecessor stops printing exactly at the event it dies in, so the
// ordinary "every held name needs a close at d" rule cannot admit one. The
// exemption is an explicit, rejectable admission value and never a default:
// Unknown rejects as soon as a batch is non-empty, RequireMarkAtEvent keeps the
// ordinary rule, and CarryLastAccountedValueV1 values the predecessor of an
// admitted event in THIS observation's batch at its completed d-1 mark. The
// (d-1, d] price move of that name is then not observed market P&L; it lands in
// mandatory_value_bridge and in the movement ledger's carried_mark column. No
// forward fill, no synthesized quote, and no exemption for any other name.
enum class TransitionPredecessorMark : atx::u8 {
  Unknown, RequireMarkAtEvent, CarryLastAccountedValueV1
};

// Accounting-only: the short obligation continues in the successor
// representation. It asserts NO loan transfer, recall, termination, re-rate,
// locate or delivery, and a ratio that shrinks the short shrinks the borrow
// base as a representation effect, not as evidence of a cheaper loan.
enum class TransitionLoanTreatment : atx::u8 { Unknown, NoDischargeSuccessorContinuesV1 };

// Charges nothing on a pending payable and DECLARES that this understates
// financing cost rather than asserting the cost is zero.
enum class ClaimFinancing : atx::u8 { Unknown, NoneDisclosedV1 };

// Borrow accrues on post-trade, post-settlement marked short EQUITY dollars.
// Pending claims of either sign contribute nothing to the base.
enum class BorrowBase : atx::u8 { Unknown, MarkedShortEquityDollarsV1 };

// All four conventions must be explicitly admitted before any non-empty batch
// is accepted; `admission` must be the synthetic-fixture admission the
// checkpoint-12 planners require. No real-data mode is admitted.
struct ReplayClaimsConfig {
  TransitionAdmission admission{};
  TransitionPredecessorMark predecessor_mark{TransitionPredecessorMark::Unknown};
  TransitionLoanTreatment loan{TransitionLoanTreatment::Unknown};
  ClaimFinancing claim_financing{ClaimFinancing::Unknown};
  BorrowBase borrow_base{BorrowBase::Unknown};
};

struct ReplayTransitionRequest {
  data::SecurityTransition event;
  data::TransitionBasis basis;
};

// Fixed size, owns no borrowed storage: the policy fills the prefix it needs.
// A count beyond its bound rejects rather than truncating.
struct ReplayEventBatch {
  atx::usize transition_count{};
  atx::usize payment_count{};
  std::array<ReplayTransitionRequest, kMaxEventsPerObservation> transitions{};
  std::array<data::CashClaimPayment, kMaxPaymentsPerObservation> payments{};
};

// Borrowed for the duration of the policy call only. `settled_cash`, the claim
// view and the retired mask are the book as of `period` before any event of
// this observation is applied; `previous_marked_dollars` is the completed
// marked-dollar row of `period - 1`, which is exactly the accounted value a
// CarryLastAccountedValueV1 predecessor keeps. `current_marks` is the panel's
// close row at `period`; a name with no position may carry an invalid mark.
struct ReplayEventContext {
  atx::usize period{};
  atx::i64 session_key{};
  atx::i64 previous_session_key{};
  atx::u64 state_version{};
  atx::u64 last_applied_sequence{};
  atx::f64 settled_cash{};
  std::span<const atx::f64> tri_units;
  std::span<const atx::f64> previous_marked_dollars;
  std::span<const atx::f64> current_marks;
  std::span<const PendingTransitionCashClaim> pending_claims;
  std::span<const atx::u8> retired_representation;
};

// Queried once per observation with 1 <= period < dates - 1. That bound is
// STRUCTURAL, not a rejection: the replay simply never asks the policy outside
// it, so design §11 ruling 5 — no transition or payment at the final
// valuation-only observation, and none at period 0, which has no completed
// previous valuation — holds because no such batch can be offered, not because
// one would be refused. A period-0 or terminal batch is unrepresentable rather
// than rejectable and has no error path; a fixture that returns one from those
// periods is never called and therefore asserts nothing. The policy owns its
// own information cutoff exactly as the intent policy does.
using ReplayMandatoryEventPolicy =
    std::function<atx::core::Result<ReplayEventBatch>(const ReplayEventContext &)>;

// `movements` is reserved once to kMaxMandatoryMovements and holds the ledger
// rows in commit order; `final_state` is the whole retained claims ledger.
struct ReplayClaimsResult {
  ReplayPolicyResult policy;
  std::vector<MandatoryMovement> movements;
  ClaimsBookState final_state{};
};

// Same validation, accounting and all-or-error publication as
// replay_scheduled_intents, plus the design §4 per-observation ordering:
//   0. request this observation's event batch (query only; no commit),
//   1. value the pre-event book at d, exempting ONLY admitted predecessors,
//   2. plan + atomically commit each transition, re-marking the two touched
//      coordinates from the panel close,
//   3-4. claims-aware pretrade NAV = settled cash + marked equities + signed claims,
//   5. allocation, which may not target or hold a retired representation,
//   6. trades, whose post-trade gate is the three-term identity,
//   7. payment settlement AFTER the trade (ruling 1: cash recognized and paid
//      at d is first spendable at d+1) and NAV-neutral by assertion,
//   8. borrow on post-trade, post-settlement marked short equity dollars.
// With an empty batch at every observation the result is bit-identical to
// replay_scheduled_intents on the same inputs; that equivalence is a required
// regression test. Both callbacks must be nonempty. The claims ledger is owned
// by this call: it is sized in kilobytes, so the result is moved, not copied.
[[nodiscard]] atx::core::Result<ReplayClaimsResult> replay_scheduled_intents_with_events(
    const alpha::Panel &research, std::span<const atx::i64> session_keys,
    std::span<const atx::usize> decision_periods,
    std::span<const atx::f64> preference_weights, const ReplayIntentPolicy &policy,
    const ReplayMandatoryEventPolicy &events, const ReplayConfig &config,
    const ReplayClaimsConfig &claims);

} // namespace atx::engine::book
