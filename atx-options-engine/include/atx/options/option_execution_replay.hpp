#pragma once

// Deterministic, evidence-graded execution replay for US listed options.
//
// This API implements a conservative Consolidated-L1 model. It consumes one
// selected participant's displayed top-of-book size, never invents depth or
// passive queue priority, and never represents an independent-leg result as an
// atomic complex fill. Venue-book and native-complex replay require different
// evidence and are intentionally outside this contract.

#include <compare>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

#include "atx/core/decimal.hpp"
#include "atx/core/error.hpp"
#include "atx/engine/loop/types.hpp"
#include "atx/options/option_research_panel.hpp"
#include "atx/vol/surface_archive.hpp"

namespace atx::options::execution {

inline constexpr std::uint64_t kOptionExecutionReplayModelVersion =
    0x4154584F45520001ULL; // "ATXOER", revision 1
inline constexpr std::uint64_t kOptionExecutionReplayOrderingVersion = 1U;
inline constexpr std::uint64_t kOptionExecutionSessionModelVersion =
    0x4154584F45530001ULL; // "ATXOES", revision 1
inline constexpr std::uint64_t kOptionExecutionSessionOrderingVersion = 1U;

struct OptionOrderId {
  std::uint64_t value{0};
  [[nodiscard]] bool operator==(const OptionOrderId &) const noexcept = default;
  [[nodiscard]] auto operator<=>(const OptionOrderId &) const noexcept = default;
};

struct OptionCancelId {
  std::uint64_t value{0};
  [[nodiscard]] bool operator==(const OptionCancelId &) const noexcept = default;
  [[nodiscard]] auto operator<=>(const OptionCancelId &) const noexcept = default;
};

enum class OptionReplayScenario : std::uint8_t {
  Strict = 0,
  Calibrated = 1,
  Stress = 2,
};

enum class OptionReplayEvidenceGrade : std::uint8_t {
  ConsolidatedL1 = 0,
};

enum class OptionQuoteStatus : std::uint8_t {
  Firm = 0,
  Locked = 1,
  MissingQuote = 2,
  MissingSize = 3,
  Crossed = 4,
  NonFirm = 5,
  Halted = 6,
};

enum class OptionTimeInForce : std::uint8_t {
  // Attempts the first strictly-future quote event for the contract, then
  // cancels leaves even when that event is stale, non-firm, one-sided, or
  // non-marketable. This intentionally is not exchange IOC-at-activation.
  FirstFutureQuoteOrCancel = 0,
  Day = 1,
  GoodTillCanceled = 2,
};

enum class OptionOrderDisposition : std::uint8_t {
  Filled = 0,
  Canceled = 1,
  Expired = 2,
  OpenAtEnd = 3,
};

enum class OptionCancelDisposition : std::uint8_t {
  Applied = 0,
  AlreadyTerminal = 1,
  UnknownOrder = 2,
};

// Stable merge key for market records whose native streams do not define one
// global order. Native sequence remains authoritative within source/channel.
struct OptionMarketOrderKey {
  std::uint32_t source_rank{0};
  std::uint32_t channel_id{0};
  // Monotone trading-session or feed-reset epoch within (source, channel).
  // Native sequence numbers may restart only when this value increases; an
  // old epoch cannot re-enter after a newer one becomes available.
  std::uint64_t stream_epoch{0};
  std::uint64_t native_sequence{0};
  std::uint32_t packet_index{0};
  std::uint64_t stable_ingest_ordinal{0};

  [[nodiscard]] bool operator==(const OptionMarketOrderKey &) const noexcept = default;
};

// Absolute selected-participant top-of-book state at one availability time.
// Sizes are contracts. OPRA BBO size is not aggregate national depth.
struct OptionTopOfBookEvent {
  std::uint64_t contract_id{0};
  atx::engine::InstrumentId engine_id{};
  std::int64_t quote_event_ts_ns{0};
  std::int64_t available_ts_ns{0};
  OptionMarketOrderKey order_key{};
  atx::core::Decimal bid{};
  atx::core::Decimal ask{};
  std::int64_t bid_size_contracts{0};
  std::int64_t ask_size_contracts{0};
  std::uint16_t bid_participant_id{0};
  std::uint16_t ask_participant_id{0};
  // An absolute row may be emitted because only one side changed. A false flag
  // means that side is context only and cannot replenish its modeled pool.
  bool bid_updated{true};
  bool ask_updated{true};
  OptionQuoteStatus status{OptionQuoteStatus::Firm};
  bool cross_stream_ordering_ambiguous{false};
  atx::vol::ArchiveContentIdentity source_identity{};

  [[nodiscard]] bool operator==(const OptionTopOfBookEvent &) const noexcept = default;
};

// A marketable-limit intent. Direction is the sign of quantity_contracts.
// decision_ts_ns < arrival_ts_ns is mandatory. Fills require a strictly later
// quote availability time; the book observed before or at arrival is context,
// not reusable execution evidence. Passive L1 fills are disabled: a
// non-marketable order remains working until a future firm quote crosses it.
struct OptionOrderRequest {
  OptionOrderId order_id{};
  std::uint64_t strategy_id{0};
  std::uint64_t basket_id{0};
  std::uint64_t contract_id{0};
  atx::engine::InstrumentId engine_id{};
  std::int64_t quantity_contracts{0};
  atx::core::Decimal limit_price{};
  std::int64_t decision_ts_ns{0};
  std::int64_t arrival_ts_ns{0};
  // Required and strictly after arrival for Day; zero otherwise.
  std::int64_t expire_ts_ns{0};
  std::uint64_t priority_sequence{0};
  std::uint32_t fee_schedule_key{0};
  OptionTimeInForce time_in_force{OptionTimeInForce::FirstFutureQuoteOrCancel};

  [[nodiscard]] bool operator==(const OptionOrderRequest &) const noexcept = default;
};

// A replacement is represented by a cancel of the old order plus a new submit.
// At an identical availability timestamp, cancellation is processed before
// quote updates and new submissions; the replacement therefore loses priority.
struct OptionCancelRequest {
  OptionCancelId cancel_id{};
  OptionOrderId order_id{};
  std::int64_t event_ts_ns{0};
  std::int64_t available_ts_ns{0};
  std::uint64_t priority_sequence{0};

  [[nodiscard]] bool operator==(const OptionCancelRequest &) const noexcept = default;
};

// Effective interval is [effective_from_ts_ns, effective_until_ts_ns). Each
// component is exact dollars. sell_premium_rate is an exact unitless fraction
// applied only to sell premium, supporting sales-value charges without a
// floating-money seam. Negative exchange_per_contract permits rebates, though
// the Consolidated-L1 engine only models configured taker execution.
struct OptionFeeSchedule {
  std::uint32_t key{0};
  // The schedule revision must be known no later than its effective start.
  std::int64_t available_ts_ns{0};
  std::int64_t effective_from_ts_ns{0};
  std::int64_t effective_until_ts_ns{0};
  atx::core::Decimal exchange_per_contract{};
  atx::core::Decimal clearing_per_contract{};
  atx::core::Decimal regulatory_per_contract{};
  atx::core::Decimal commission_per_contract{};
  atx::core::Decimal commission_per_order{};
  atx::core::Decimal sell_premium_rate{};
  atx::vol::ArchiveContentIdentity source_identity{};

  [[nodiscard]] bool operator==(const OptionFeeSchedule &) const noexcept = default;
};

// Point-in-time simple-order minimum-price-variation rule. A zero threshold
// means tick_at_or_above_threshold applies at every positive price. Otherwise
// tick_below_threshold applies below the threshold and the second tick applies
// at or above it. The effective interval is [from, until).
struct OptionTickSchedule {
  std::uint32_t key{0};
  std::int64_t available_ts_ns{0};
  std::int64_t effective_from_ts_ns{0};
  std::int64_t effective_until_ts_ns{0};
  atx::core::Decimal price_threshold{};
  atx::core::Decimal tick_below_threshold{};
  atx::core::Decimal tick_at_or_above_threshold{};
  atx::vol::ArchiveContentIdentity source_identity{};

  [[nodiscard]] bool operator==(const OptionTickSchedule &) const noexcept = default;
};

// Canonical execution catalog input. Standard deliverables require a positive
// whole-number multiplier. Results are sorted by contract_id, independent of
// caller ordering.
struct OptionReplayContract {
  std::uint64_t contract_id{0};
  atx::engine::InstrumentId engine_id{};
  std::int64_t multiplier{100};
  std::int64_t initial_contracts{0};
  std::uint32_t tick_schedule_key{0};
  std::int64_t definition_effective_ts_ns{0};
  std::int64_t definition_available_ts_ns{0};
  std::int64_t expiry_ts_ns{0};
  atx::vol::ArchiveContentIdentity definition_source_identity{};

  [[nodiscard]] bool operator==(const OptionReplayContract &) const noexcept = default;
};

struct OptionFeeBreakdown {
  atx::core::Decimal exchange{};
  atx::core::Decimal clearing{};
  atx::core::Decimal regulatory{};
  atx::core::Decimal commission{};
  atx::core::Decimal sales_value{};
  atx::core::Decimal total{};

  [[nodiscard]] bool operator==(const OptionFeeBreakdown &) const noexcept = default;
};

struct OptionFill {
  OptionOrderId order_id{};
  std::uint64_t strategy_id{0};
  std::uint64_t basket_id{0};
  std::uint64_t contract_id{0};
  atx::engine::InstrumentId engine_id{};
  std::int64_t quantity_contracts{0};
  std::int64_t multiplier{0};
  atx::core::Decimal touch_price{};
  atx::core::Decimal fill_price{};
  atx::core::Decimal modeled_slippage_per_option{};
  atx::core::Decimal premium_notional{};
  OptionFeeBreakdown fees{};
  // Buy: negative premium and fees. Sell: positive premium less fees.
  atx::core::Decimal cash_delta{};
  std::int64_t decision_ts_ns{0};
  std::int64_t arrival_ts_ns{0};
  std::int64_t fill_ts_ns{0};
  std::int64_t quote_event_ts_ns{0};
  std::int64_t quote_available_ts_ns{0};
  OptionMarketOrderKey quote_order_key{};
  std::int64_t displayed_size_before_contracts{0};
  std::int64_t displayed_size_after_contracts{0};
  std::uint16_t selected_participant_id{0};
  bool cross_stream_ordering_ambiguous{false};
  atx::vol::ArchiveContentIdentity quote_source_identity{};
  atx::vol::ArchiveContentIdentity fee_source_identity{};
  atx::vol::ArchiveContentIdentity tick_source_identity{};

  [[nodiscard]] bool operator==(const OptionFill &) const noexcept = default;
};

struct OptionOrderAudit {
  OptionOrderRequest request{};
  // Signed like request.quantity_contracts. The two always sum to the request.
  std::int64_t filled_contracts{0};
  std::int64_t remaining_contracts{0};
  std::size_t fill_count{0};
  atx::core::Decimal total_fees{};
  std::int64_t first_fill_ts_ns{0};
  std::int64_t last_fill_ts_ns{0};
  OptionOrderDisposition disposition{OptionOrderDisposition::OpenAtEnd};

  [[nodiscard]] bool operator==(const OptionOrderAudit &) const noexcept = default;
};

struct OptionCancelAudit {
  OptionCancelRequest request{};
  OptionCancelDisposition disposition{OptionCancelDisposition::AlreadyTerminal};

  [[nodiscard]] bool operator==(const OptionCancelAudit &) const noexcept = default;
};

struct OptionPositionSnapshot {
  std::uint64_t contract_id{0};
  atx::engine::InstrumentId engine_id{};
  std::int64_t multiplier{0};
  std::int64_t contracts{0};

  [[nodiscard]] bool operator==(const OptionPositionSnapshot &) const noexcept = default;
};

struct OptionReplaySummary {
  OptionReplayEvidenceGrade evidence_grade{OptionReplayEvidenceGrade::ConsolidatedL1};
  OptionReplayScenario scenario{OptionReplayScenario::Strict};
  std::uint64_t model_version{kOptionExecutionReplayModelVersion};
  std::uint64_t ordering_version{kOptionExecutionReplayOrderingVersion};
  atx::vol::ArchiveContentIdentity market_data_identity{};
  atx::vol::ArchiveContentIdentity sequence_validation_identity{};
  atx::vol::ArchiveContentIdentity calibration_identity{};
  bool sequence_continuity_verified{true};
  bool allow_locked_market{false};
  atx::core::Decimal displayed_size_fraction{};
  atx::core::Decimal adverse_price_bps{};
  std::int64_t max_quote_age_ns{0};
  std::int64_t replay_end_ts_ns{0};
  bool cross_stream_ordering_ambiguous{false};
  std::size_t quote_events{0};
  std::size_t firm_quote_events{0};
  std::size_t non_executable_quote_events{0};
  std::size_t stale_match_attempts{0};
  std::size_t requested_contracts{0};
  std::size_t filled_contracts{0};
  std::size_t canceled_orders{0};
  std::size_t expired_orders{0};
  std::size_t open_orders{0};
  std::size_t peak_working_orders{0};
  // Consolidated-L1 fills are configured taker/removing-liquidity scenarios;
  // actual venue and exchange allocation are not inferred from OPRA.
  bool configured_taker_execution{true};
  atx::core::Decimal initial_cash{};
  atx::core::Decimal final_cash{};
  atx::core::Decimal gross_premium_turnover{};
  atx::core::Decimal total_fees{};

  [[nodiscard]] bool operator==(const OptionReplaySummary &) const noexcept = default;
};

struct OptionReplayView {
  std::span<const OptionFill> fills;
  std::span<const OptionOrderAudit> orders;
  std::span<const OptionCancelAudit> cancellations;
  std::span<const OptionPositionSnapshot> positions;
  std::span<const OptionFeeSchedule> fee_schedules;
  std::span<const OptionTickSchedule> tick_schedules;
  OptionReplaySummary summary{};
};

enum class OptionExecutionSessionState : std::uint8_t {
  Empty = 0,
  ReadyToAdvance = 1,
  AtFrontier = 2,
  Finished = 3,
  Failed = 4,
};

enum class OptionOrderLifecycleState : std::uint8_t {
  Scheduled = 0,
  Working = 1,
  PartiallyFilled = 2,
  PendingCancel = 3,
  Filled = 4,
  Canceled = 5,
  Expired = 6,
  // Used only by cancel-ledger records that never resolved to an order.
  NotApplicable = 7,
};

enum class OptionOrderTransitionKind : std::uint8_t {
  Scheduled = 0,
  Submitted = 1,
  PartiallyFilled = 2,
  Filled = 3,
  CancelRequested = 4,
  Canceled = 5,
  Expired = 6,
  CancelAlreadyTerminal = 7,
  CancelUnknownOrder = 8,
};

// Current execution state aligned one-to-one with the cumulative order audit.
// A pending cancel remains economically exposed until its availability event.
struct OptionOrderStateSnapshot {
  OptionOrderId order_id{};
  std::uint64_t contract_id{0};
  atx::engine::InstrumentId engine_id{};
  std::int64_t remaining_contracts{0};
  OptionOrderLifecycleState state{OptionOrderLifecycleState::Scheduled};
  bool cancel_pending{false};

  [[nodiscard]] bool operator==(const OptionOrderStateSnapshot &) const noexcept = default;
};

// Signed net account projection aligned one-to-one with the canonical contract
// catalog. pending_cancel_contracts is a subset of scheduled + working
// contracts; projected_contracts therefore counts it only once. Inspect the
// aligned orders/order_states to measure gross or opposing working leaves.
struct OptionContractExposureSnapshot {
  std::uint64_t contract_id{0};
  atx::engine::InstrumentId engine_id{};
  std::int64_t position_contracts{0};
  std::int64_t scheduled_contracts{0};
  std::int64_t working_contracts{0};
  std::int64_t pending_cancel_contracts{0};
  std::int64_t projected_contracts{0};

  [[nodiscard]] bool operator==(const OptionContractExposureSnapshot &) const noexcept = default;
};

// Immutable append-only lifecycle record. sequence is session-local and starts
// at one. fill_index is one-based for fill transitions and zero otherwise.
struct OptionOrderTransition {
  std::uint64_t sequence{0};
  std::int64_t event_ts_ns{0};
  std::int64_t available_ts_ns{0};
  OptionOrderId order_id{};
  OptionCancelId cancel_id{};
  OptionOrderTransitionKind kind{OptionOrderTransitionKind::Scheduled};
  OptionOrderLifecycleState state_before{OptionOrderLifecycleState::Scheduled};
  OptionOrderLifecycleState state_after{OptionOrderLifecycleState::Scheduled};
  std::int64_t last_fill_contracts{0};
  std::int64_t cumulative_fill_contracts{0};
  std::int64_t remaining_contracts{0};
  std::size_t fill_index{0};

  [[nodiscard]] bool operator==(const OptionOrderTransition &) const noexcept = default;
};

struct OptionExecutionSessionSummary {
  std::uint64_t model_version{kOptionExecutionSessionModelVersion};
  std::uint64_t ordering_version{kOptionExecutionSessionOrderingVersion};
  std::uint64_t command_trace_hash{0};
  std::size_t frontier_count{0};
  std::size_t command_batch_count{0};
  std::size_t transition_count{0};
  std::int64_t frontier_ts_ns{-1};

  [[nodiscard]] bool operator==(const OptionExecutionSessionSummary &) const noexcept = default;
};

struct OptionExecutionFrontierView {
  std::int64_t frontier_ts_ns{0};
  // Deltas since the previously returned frontier. new_transitions includes
  // command-acceptance transitions appended after that prior observation.
  std::span<const OptionFill> new_fills;
  std::span<const OptionOrderTransition> new_transitions;
  std::span<const OptionFill> fills;
  std::span<const OptionOrderAudit> orders;
  std::span<const OptionCancelAudit> cancellations;
  std::span<const OptionPositionSnapshot> positions;
  std::span<const OptionOrderStateSnapshot> order_states;
  std::span<const OptionContractExposureSnapshot> exposures;
  std::span<const OptionOrderTransition> transitions;
  OptionReplaySummary replay_summary{};
  OptionExecutionSessionSummary session_summary{};
};

struct OptionCommandBatch {
  std::span<const OptionOrderRequest> orders;
  std::span<const OptionCancelRequest> cancellations;
};

struct OptionReplayLimits {
  std::size_t max_contracts{100'000};
  std::size_t max_quote_events{1'000'000};
  std::size_t max_orders{100'000};
  std::size_t max_cancellations{100'000};
  std::size_t max_fee_rows{10'000};
  std::size_t max_tick_rows{10'000};
  std::size_t max_fills{1'000'000};
  std::size_t max_workspace_bytes{1'073'741'824}; // 1 GiB
};

struct OptionExecutionSessionLimits {
  OptionReplayLimits replay{};
  std::size_t max_frontiers{100'000};
  std::size_t max_transitions{2'000'000};
  std::size_t max_workspace_bytes{1'073'741'824}; // 1 GiB
};

// Exact reserved payload bytes required by OptionExecutionSession::create().
// Container objects and allocator metadata are excluded.
[[nodiscard]] atx::core::Result<std::size_t>
option_execution_session_required_workspace_bytes(const OptionExecutionSessionLimits &limits);

struct OptionReplayConfig {
  OptionReplayScenario scenario{OptionReplayScenario::Strict};
  std::uint64_t model_version{kOptionExecutionReplayModelVersion};
  atx::vol::ArchiveContentIdentity market_data_identity{};
  // Frozen validation report for the lossless source capture. Replay rows may
  // be a BBO-changing subset, so raw-sequence gaps are attested externally.
  atx::vol::ArchiveContentIdentity sequence_validation_identity{};
  atx::vol::ArchiveContentIdentity calibration_identity{};
  bool sequence_continuity_verified{true};
  bool allow_locked_market{false};
  // Fraction of selected-participant displayed size made available to the
  // counterfactual strategy. Strict/Stress are capped at 25%; Calibrated may
  // use up to 100% but requires calibration_identity.
  atx::core::Decimal displayed_size_fraction{
      atx::core::Decimal::from_raw(atx::core::Decimal::kScale / 4)};
  // Deterministic adverse move from the observed touch, in basis points.
  atx::core::Decimal adverse_price_bps{};
  // Zero disables the age gate. Otherwise fill_time - quote_event_time must not
  // exceed this bound.
  std::int64_t max_quote_age_ns{1'000'000'000};
  std::int64_t replay_end_ts_ns{0};
};

struct OptionReplayInputs {
  std::span<const OptionReplayContract> contracts;
  std::span<const OptionTopOfBookEvent> quotes;
  std::span<const OptionOrderRequest> orders;
  std::span<const OptionCancelRequest> cancellations;
  std::span<const OptionFeeSchedule> fee_schedules;
  std::span<const OptionTickSchedule> tick_schedules;
  atx::core::Decimal initial_cash{};
};

// Exact reserved payload bytes required by create() for these limits. Container
// objects and allocator metadata are not included. This lets callers enforce a
// deterministic memory budget before constructing the workspace.
[[nodiscard]] atx::core::Result<std::size_t>
option_replay_required_workspace_bytes(const OptionReplayLimits &limits);

// Reusable, bounded workspace. create() performs all capacity allocations.
// A successful run performs no dynamic allocation. Returned spans borrow from
// the workspace and remain valid only until the next run or destruction.
//
// run() validates and canonicalizes all caller inputs before replay. Inputs and
// caller-owned account state are never mutated. On failure the workspace stays
// reusable, but any prior borrowed view is invalidated at run entry.
// One workspace instance is not thread-safe; independent instances may run in
// parallel. Expected validation, capacity, and arithmetic failures use Result.
class OptionExecutionReplay {
public:
  [[nodiscard]] static atx::core::Result<OptionExecutionReplay>
  create(OptionReplayLimits limits = {});

  ~OptionExecutionReplay();
  OptionExecutionReplay(OptionExecutionReplay &&) noexcept;
  OptionExecutionReplay &operator=(OptionExecutionReplay &&) noexcept;
  OptionExecutionReplay(const OptionExecutionReplay &) = delete;
  OptionExecutionReplay &operator=(const OptionExecutionReplay &) = delete;

  [[nodiscard]] atx::core::Result<OptionReplayView> run(const OptionReplayInputs &inputs,
                                                        const OptionReplayConfig &config);

private:
  struct Impl;
  explicit OptionExecutionReplay(std::unique_ptr<Impl> impl) noexcept;
  std::unique_ptr<Impl> impl_;
};

struct OptionExecutionSessionResult {
  OptionReplayView replay{};
  std::span<const OptionOrderStateSnapshot> order_states;
  std::span<const OptionContractExposureSnapshot> exposures;
  std::span<const OptionOrderTransition> transitions;
  OptionExecutionSessionSummary session_summary{};
};

// Persistent deterministic replay session for adaptive strategies.
//
// start() accepts immutable market/catalog inputs and requires empty initial
// order/cancel spans. advance_to() settles all evidence available at or before
// one strictly increasing frontier. The caller must acknowledge that frontier
// exactly once with apply_commands(), including an empty batch, before the next
// advance. Every submitted command must be decided at the current frontier and
// become effective strictly later. Nonzero order and cancellation identifiers
// must each increase globally across accepted command batches. Within a batch,
// identifiers must also increase in canonical availability/priority order.
//
// apply_commands() validates and canonicalizes the complete basket before
// mutating session state. A validation failure is retryable. A failure while
// advancing poisons the session because prior borrowed views are invalidated at
// mutation entry and a partial event cycle is not exposed as a sealed frontier.
// Unknown cancellation targets are pinned when the batch is accepted and
// cannot attach to an order created later with the same identifier.
// Admission reserves transition capacity for immediate acceptance plus the
// mandatory future submit/cancel outcome. Data-dependent fill/expiry
// transitions can still exhaust the configured limit and poison the session.
//
// All returned spans borrow from the session and are invalidated by the next
// start, advance_to, apply_commands, finish, move, or destruction. create()
// performs every capacity allocation; successful lifecycle calls do not
// allocate. At a frontier, replay_summary, order_states, and exposures reflect
// only events processed through that frontier. One instance is not thread-safe.
// Final order and cancellation audit spans retain canonical command-acceptance
// order across frontiers; they are not globally resorted by later availability.
class OptionExecutionSession {
public:
  [[nodiscard]] static atx::core::Result<OptionExecutionSession>
  create(OptionExecutionSessionLimits limits = {});

  ~OptionExecutionSession();
  OptionExecutionSession(OptionExecutionSession &&) noexcept;
  OptionExecutionSession &operator=(OptionExecutionSession &&) noexcept;
  OptionExecutionSession(const OptionExecutionSession &) = delete;
  OptionExecutionSession &operator=(const OptionExecutionSession &) = delete;

  [[nodiscard]] atx::core::Result<void> start(const OptionReplayInputs &inputs,
                                              const OptionReplayConfig &config);
  [[nodiscard]] atx::core::Result<OptionExecutionFrontierView>
  advance_to(std::int64_t frontier_ts_ns);
  [[nodiscard]] atx::core::Result<void> apply_commands(const OptionCommandBatch &commands);
  [[nodiscard]] atx::core::Result<OptionExecutionSessionResult> finish();
  [[nodiscard]] OptionExecutionSessionState state() const noexcept;

private:
  struct Impl;
  explicit OptionExecutionSession(std::unique_ptr<Impl> impl) noexcept;
  std::unique_ptr<Impl> impl_;
};

struct OptionOrderBatchSpec {
  std::uint64_t first_order_id{1};
  std::uint64_t strategy_id{1};
  std::uint64_t basket_id{1};
  std::uint64_t first_priority_sequence{1};
  std::uint32_t fee_schedule_key{1};
  std::int64_t arrival_latency_ns{1};
  OptionTimeInForce time_in_force{OptionTimeInForce::FirstFutureQuoteOrCancel};
  std::int64_t expire_ts_ns{0};
  // Widen the decision-time touch into a marketable limit using an exact basis
  // point value. A nonzero offset also requires a positive increment and rounds
  // buys up/sells down. For heterogeneous tick classes, build separate batches.
  atx::core::Decimal limit_offset_bps{};
  atx::core::Decimal limit_price_increment{};
};

// Convert a target book into canonical marketable-limit requests. Orders use
// decision-time bid/ask only to set their limits and cannot arrive until a
// strictly later timestamp. Zero order quantities are omitted.
[[nodiscard]] atx::core::Result<std::vector<OptionOrderRequest>> make_option_order_batch(
    const atx::options::research::OptionResearchPanel &panel, std::size_t date_index,
    const atx::options::research::OptionTargetBook &targets, const OptionOrderBatchSpec &spec);

} // namespace atx::options::execution
