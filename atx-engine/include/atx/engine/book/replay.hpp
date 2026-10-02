#pragma once

#include <array>
#include <functional>
#include <optional>
#include <span>
#include <vector>

#include "atx/core/error.hpp"
#include "atx/core/types.hpp"
#include "atx/engine/book/claims_state.hpp"
#include "atx/engine/book/replay_cost.hpp"
#include "atx/engine/data/security_transition.hpp"

namespace atx::engine::alpha { class Panel; }

namespace atx::engine::book {

enum class ReplayDayBasis : atx::u16 { D360 = 360, D365 = 365 };

struct BorrowSchedule;

// What the replay does when a HELD name has no valid close at a valuation.
//   Abort              the pre-W0 default: the replay fails.
//   CrspDelistReturn   the name is liquidated into cash at its last accounted
//                      value times (1 + delist_return) of its DelistingEvent;
//                      a NaN return rejects (fill it explicitly, e.g. with a
//                      Shumway-style replacement, in the event builder).
//   LastMarkZeroReturn liquidated at its last accounted value (return 0).
//   TerminalReturn     THE DEFAULT: liquidate at the first missing held close,
//                      using an available due table return, or Shumway if that
//                      evidenced event has an unknown return. Without a due event,
//                      an explicit adverse missing-price stress haircut is used:
//                      negative for longs, positive for shorts, always a loss.
//                      This is an assumption, never observed delisting evidence.
//                      Future prints and future events
//                      never determine today's holdings, cash, or NAV.
//   TerminalReturnExPostV1  Legacy W0 diagnostic, NOT a causal trading replay:
//                      preserves the hindsight gap classification below.
//                      A held name with no
//                      valid close at a valuation never aborts the replay. It is
//                      either CARRIED over an interior gap or LIQUIDATED:
//                        * carried (fix pass 1) when no DelistingEvent is due for
//                          it and either the panel prints a valid close for it at
//                          a LATER period (the "last bar" rule shared with the
//                          legacy report's holding_interval_returns) or its
//                          DelistingEvent says it is still listed
//                          (last_valid_period >= the valuation). The name keeps
//                          its units and is valued at its last valid close at
//                          both the start and end valuations (zero P&L over the
//                          gap; the whole move lands when it prints again). It
//                          cannot trade until it prints again: a decision that
//                          executes on it is not filled, its working order is
//                          cancelled, and each carried valuation is recorded in
//                          ReplayResult::gap_carries. Knowing that a name prints
//                          again is the same ex-post evidence a delisting table
//                          is; it never sizes a trade.
//                        * otherwise liquidated at its last accounted value
//                          times (1 + r), never assuming r = 0:
//                          - r = the supplied DelistingEvent's delist_return when
//                            the name has an event whose last_valid_period
//                            precedes the valuation and whose return is finite
//                            (source Table, not flagged);
//                          - otherwise the Shumway fallback, FLAGGED: -30 %
//                            (Shumway 1997, NYSE/AMEX) or -55 % (Shumway &
//                            Warther 1999, Nasdaq) by
//                            ReplayConfig::listing_exchange; an Unknown exchange
//                            takes the ADVERSE of the two for the position's side
//                            (a long gets -55 %, a short -30 %). A flagged SHORT
//                            liquidation books a gain; its count and dollars are
//                            reported separately (ReplayResult::
//                            flagged_short_delistings / flagged_short_pnl).
//                      A TargetWeight on an unheld name with no valid close at
//                      its execution is unfillable: it stays in cash and is
//                      reported in ReplayResult::unfilled_targets.
// Under CrspDelistReturn / LastMarkZeroReturn only names with a DelistingEvent
// whose last_valid_period precedes the missing valuation are liquidated; any
// other missing held close still fails (their historical contract).
enum class DelistingPolicy : atx::u8 {
  Abort = 0, CrspDelistReturn = 1, LastMarkZeroReturn = 2, TerminalReturn = 3,
  TerminalReturnExPostV1 = 4
};

// Primary listing venue, only for the Shumway fallback. Empty span == all Unknown.
enum class ListingExchange : atx::u8 { Unknown = 0, NyseAmex = 1, Nasdaq = 2 };

// Shumway (1997, JF 52(1)) performance-delisting replacement return for
// NYSE/AMEX; Shumway & Warther (1999, JF 54(6)) for Nasdaq.
inline constexpr atx::f64 kShumwayNyseAmexReturn = -0.30;
inline constexpr atx::f64 kShumwayNasdaqReturn = -0.55;

// Where a liquidation's return came from. Every Shumway* and AssumedMissingPrice*
// source is flagged; Table and LastMarkZero are not.
enum class TerminalReturnSource : atx::u8 {
  Table = 0, LastMarkZero = 1, ShumwayNyseAmex = 2, ShumwayNasdaq = 3,
  ShumwayUnknownAdverse = 4, AssumedMissingPriceAdverse = 5
};

struct TerminalReturn {
  atx::f64 value{};
  TerminalReturnSource source{TerminalReturnSource::Table};
};

// The Shumway fallback for a position of the given side. Pure; shared with the
// legacy weight report (book/report.hpp) so both paths apply the same numbers.
[[nodiscard]] constexpr TerminalReturn shumway_terminal_return(ListingExchange exchange,
                                                               bool is_short) noexcept {
  switch (exchange) {
  case ListingExchange::NyseAmex:
    return {kShumwayNyseAmexReturn, TerminalReturnSource::ShumwayNyseAmex};
  case ListingExchange::Nasdaq:
    return {kShumwayNasdaqReturn, TerminalReturnSource::ShumwayNasdaq};
  default:
    break;
  }
  // Adverse for the side: the more negative return hurts a long, the less
  // negative one hurts a short.
  return {is_short ? kShumwayNyseAmexReturn : kShumwayNasdaqReturn,
          TerminalReturnSource::ShumwayUnknownAdverse};
}

// No terminal evidence exists: a stress valuation adverse to the position,
// not a delisting-return estimate. The venue determines only haircut magnitude.
[[nodiscard]] constexpr TerminalReturn assumed_missing_price_return(ListingExchange exchange,
                                                                     bool is_short) noexcept {
  const auto reference = shumway_terminal_return(exchange, is_short);
  return {is_short ? -reference.value : reference.value,
          TerminalReturnSource::AssumedMissingPriceAdverse};
}

// What a trade that would grow a short beyond its locate does (B-04).
//   AbortV1  the pre-W0 behaviour: the replay fails.
//   ClipV2   THE DEFAULT: the post-trade short is clipped to the locate (or,
//            when the carried short already exceeds a shrunken locate, to the
//            carried position), the clip is reported in
//            ReplayResult::locate_clips, and the replay continues.
enum class LocateBreach : atx::u8 { AbortV1 = 1, ClipV2 = 2 };

// One terminal delisting. last_valid_period is the name's final valid close;
// the first held valuation after it with a missing close liquidates the name,
// even when that is earlier than the recorded delisting date (a missing final
// print before the formal delisting).
struct DelistingEvent {
  atx::usize instrument{};
  atx::usize last_valid_period{};
  atx::f64 delist_return{}; // Fraction; NaN == unknown.
  // First observation at which this table row is available to the replay.
  // Zero asserts it is supplied before the replay begins; later evidence must
  // name its actual availability. Future availability never changes prior NAV.
  atx::usize available_period{};
};

// A liquidation the replay performed. Not a trade: no trade cost, no fill.
struct ReplayDelisting {
  atx::usize period{};      // Valuation at which the name was liquidated.
  atx::usize instrument{};
  atx::f64 tri_units{};     // Units removed.
  atx::f64 last_value{};    // Last accounted marked dollars (at period - 1).
  atx::f64 delist_return{}; // Return applied.
  atx::f64 proceeds{};      // Signed cash credited (negative buys back a short).
  TerminalReturnSource source{TerminalReturnSource::Table};
  bool flagged{};           // True for every Shumway fallback.
};

// A trade clipped to its locate (LocateBreach::ClipV2). Dollars are marked at
// the execution close; requested_value is the post-trade value the target (or
// working order) asked for, allowed_value the post-trade value actually sought.
struct ReplayLocateClip {
  atx::usize period{};
  atx::usize instrument{};
  atx::f64 requested_value{};
  atx::f64 allowed_value{};
  atx::f64 locate{};
};

// A nonzero TargetWeight that could not trade because the name had no valid
// close at the execution period (the TerminalReturn policies). The
// intended dollars stay in cash.
struct ReplayUnfilledTarget {
  atx::usize period{};
  atx::usize decision_period{};
  atx::usize instrument{};
  atx::f64 weight{};
};

// A held name carried over an interior gap at one valuation
// (DelistingPolicy::TerminalReturnExPostV1 only): no valid close at `period` but
// prints again later, or its DelistingEvent says it is still listed. One row
// per carried valuation, in period/instrument order.
struct ReplayGapCarry {
  atx::usize period{};       // Valuation with no valid close.
  atx::usize instrument{};
  atx::usize mark_period{};  // Period of the last valid close it is valued at.
  atx::f64 tri_units{};      // Units carried (unchanged across the gap).
  atx::f64 carried_value{};  // tri_units * close[mark_period], signed.
  bool trade_blocked{};      // A decision executing at `period` could not trade it.
};

// Fields after borrow_day_basis are extensions. The cost model, liquidity,
// borrow schedule and delisting table are opt-in; with them unset and a
// nonzero delay the replay is bit-identical to the historical accounting on any
// panel whose held names all keep a valid close. The claims-aware entry point
// supports costs and borrow schedules, but rejects conflicting delisting
// evidence; default TerminalReturn runs with Abort semantics on that entry point
// (see replay_scheduled_intents_with_events).
struct ReplayConfig {
  atx::f64 initial_nav{1.0};
  // B-02: 0 fills at the decision close (look-ahead for any signal computed
  // from that close) and is rejected unless allow_same_close is set explicitly.
  atx::usize execution_delay_periods{1};
  bool allow_same_close{false};
  atx::f64 trade_bps{0.0};             // Per absolute dollar traded, each direction.
  atx::f64 annual_borrow_bps{0.0};     // Simple annual fee on post-trade short dollars.
  ReplayDayBasis borrow_day_basis{ReplayDayBasis::D365};
  // Per-name cost model (exclusive with a nonzero trade_bps). Borrowed; must
  // outlive the call. A capped model leaves a working order for the residual,
  // re-attempted until filled, replaced, or canceled by a mandatory transition.
  const ReplayCostModel *cost_model{nullptr};
  // dates * instruments, period-major; required iff cost_model->needs_liquidity().
  std::span<const LiquidityRow> liquidity{};
  // Per-name fee/locate/rebate/cash schedule (exclusive with a nonzero
  // annual_borrow_bps). Borrowed; must outlive the call.
  const BorrowSchedule *borrow_schedule{nullptr};
  DelistingPolicy delisting_policy{DelistingPolicy::TerminalReturn};
  std::span<const DelistingEvent> delistings{}; // At most one per instrument.
  // Empty, or one venue per canonical instrument (TerminalReturn fallback only).
  std::span<const ListingExchange> listing_exchange{};
  LocateBreach locate_breach{LocateBreach::ClipV2}; // Only with a borrow_schedule.
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
  std::vector<ReplayDelisting> delistings; // Empty unless a delisting policy fired.
  // Working orders still open after the final valuation (cost-model cap only).
  atx::usize open_working_orders{};
  std::vector<ReplayLocateClip> locate_clips;         // LocateBreach::ClipV2 only.
  std::vector<ReplayUnfilledTarget> unfilled_targets; // TerminalReturn policies.
  atx::usize flagged_delistings{}; // Count of `delistings` rows with flagged == true.
  // The flagged rows whose position was SHORT. P&L is signed sum(proceeds -
  // last_value): a due-event Shumway estimate can credit a gain; an unevidenced
  // missing-price stress always debits a loss. The source distinguishes them.
  atx::usize flagged_short_delistings{};
  atx::f64 flagged_short_pnl{};
  atx::usize assumed_liquidations{}; // No due/available evidence; adverse stress only.
  atx::f64 assumed_liquidation_pnl{}; // Signed sum(proceeds - last_value), never positive.
  std::vector<ReplayGapCarry> gap_carries; // TerminalReturnExPostV1 only.
};

// Borrowed only for the duration of the policy call. Instrument spans have the
// panel's canonical order and full instrument count; the policy receives no panel
// or future rows. Every held position has a valid current mark before this view
// is constructed, except a name carried over an interior gap
// (DelistingPolicy::TerminalReturnExPostV1): its current mark is missing, its
// marked_dollars is the carried value, and whatever the policy returns for it
// is not executed (see ReplayResult::gap_carries). Unused current marks may be
// missing/nonpositive/nonfinite.
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
// nonnegative finite cost rates, D360/D365, a nonzero execution delay unless
// allow_same_close is set (B-02), and checked index/duration arithmetic.
// At least one observation is allowed; final observation is valuation-only, with
// no trade/fee/liquidation. All D-1 intervals are recorded, including initial cash.
// Under the default DelistingPolicy::TerminalReturn a held name whose required
// mark is missing/nonpositive/nonfinite is carried over an interior gap or
// liquidated at a terminal return (never an abort); under Abort (and for names
// outside the CrspDelistReturn / LastMarkZeroReturn tables) it fails with an error. Invalid resulting NAV always
// fails; missing unused marks are ignored. Inputs are never mutated and failures
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

// A mandatory transition cancels unfilled goals on both changed coordinates.
// The retired predecessor must never reopen; the successor inventory change
// invalidates its old target. Fresh decisions after the event may size anew.
struct ReplayTransitionOrderCancellation {
  atx::usize period{};
  atx::usize instrument{};
  atx::u64 event_id{};
  atx::f64 goal_tri_units{};
};

// `movements` is reserved once to kMaxMandatoryMovements and holds the ledger
// rows in commit order; `final_state` is the whole retained claims ledger.
struct ReplayClaimsResult {
  ReplayPolicyResult policy;
  std::vector<MandatoryMovement> movements;
  ClaimsBookState final_state{};
  std::vector<ReplayTransitionOrderCancellation> order_cancellations;
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
// regression test. Both callbacks must be nonempty. Cost models, residual fills,
// and borrow schedules share the ordinary replay's validation and accounting.
// Transition coordinates cancel old working orders; unrelated orders continue
// with claims in their NAV identity. Mandatory deliveries incur no trading fee
// or discretionary locate check. Post-event net signed equity positions use
// the SUCCESSOR's canonical schedule row for interval financing and subsequent
// short-growth locate checks, without transferring the predecessor's locate.
// Pending claims earn/pay no financing under NoneDisclosedV1; settled payments
// affect this interval's cash interest only after allocation and execution.
// These are admitted accounting conventions, not evidence of broker loan
// continuity, recalls, or claim financing. Delisting tables/exchanges and other
// delisting policies remain unsupported; default TerminalReturn is admitted (with no
// table and no exchanges) and run with Abort semantics, because this path is
// synthetic-fixture only and its terminal mechanism is the admitted mandatory
// event, not a missing close. The claims ledger is owned
// by this call: it is sized in kilobytes, so the result is moved, not copied.
[[nodiscard]] atx::core::Result<ReplayClaimsResult> replay_scheduled_intents_with_events(
    const alpha::Panel &research, std::span<const atx::i64> session_keys,
    std::span<const atx::usize> decision_periods,
    std::span<const atx::f64> preference_weights, const ReplayIntentPolicy &policy,
    const ReplayMandatoryEventPolicy &events, const ReplayConfig &config,
    const ReplayClaimsConfig &claims);

} // namespace atx::engine::book
