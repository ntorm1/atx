#pragma once

// Date-major adaptive listed-options backtest coordinator.
//
// The coordinator joins the point-in-time research panel, cross-sectional
// WeightPolicy, option-aware whole-contract targets, and the persistent
// execution session. Reconciliation inspects every signed nonterminal order
// leaf. Unsafe retargets are cancel-first: replacements are deferred until a
// later explicit decision frontier observes the cancel outcome.

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>

#include "atx/core/error.hpp"
#include "atx/engine/loop/weight_policy.hpp"
#include "atx/options/option_execution_replay.hpp"
#include "atx/options/option_research_panel.hpp"

namespace atx::options::adaptive {

inline constexpr std::uint64_t kOptionAdaptiveCoordinatorModelVersion =
    0x4154584F41430001ULL; // "ATXOAC", revision 1
inline constexpr std::uint64_t kOptionAdaptiveCoordinatorOrderingVersion = 1U;

enum class OptionAdaptiveCoordinatorState : std::uint8_t {
  Empty = 0,
  Running = 1,
  Finished = 2,
  Failed = 3,
};

enum class OptionReconciliationScope : std::uint8_t {
  // Any unsafe or pending-cancel leaf cancels every non-pending active leaf and
  // blocks new orders across the decision basket. This reduces asynchronous
  // leg drift but does not provide atomic multi-leg execution.
  WholeBasketCancelBarrier = 0,
  // Independent strategies may continue safe contracts while another contract
  // waits at a cancellation barrier.
  IndependentContract = 1,
};

enum class OptionMissingSignalPolicy : std::uint8_t {
  // Safe default: preserve the filled position, retain only working leaves that
  // reduce its absolute size, and cancel exposure-increasing/mixed leaves.
  // Whole-basket scope may conservatively cancel those retained leaves too.
  HoldPositionAndReduceRisk = 0,
  // Explicitly convert missing/non-tradable cells into a flat target.
  LiquidateToZero = 1,
  // Fail the decision before emitting commands.
  RejectDecision = 2,
};

struct OptionAdaptiveCoordinatorLimits {
  execution::OptionExecutionSessionLimits execution{};
  std::size_t max_decisions{100'000};
  std::size_t max_commands_per_decision{100'000};
  // Exact coordinator-owned vector payload bound. The nested execution session
  // has its own independently enforced workspace bound.
  std::size_t max_workspace_bytes{1'073'741'824}; // 1 GiB
};

struct OptionAdaptiveCoordinatorConfig {
  atx::engine::WeightPolicy weight_policy{};
  research::OptionTargetSpec target{};
  execution::OptionOrderBatchSpec orders{};
  std::uint64_t first_cancel_id{1};
  std::int64_t cancel_latency_ns{1};
  OptionReconciliationScope reconciliation_scope{
      OptionReconciliationScope::WholeBasketCancelBarrier};
  OptionMissingSignalPolicy missing_signal_policy{
      OptionMissingSignalPolicy::HoldPositionAndReduceRisk};
};

// Bounded decision-summary record. Hashes are deterministic, non-cryptographic
// regression fingerprints; this is not a per-contract audit ledger or an
// artifact/content attestation.
struct OptionAdaptiveDecisionAudit {
  std::size_t date_index{0};
  std::int64_t decision_ts_ns{0};
  std::uint64_t signal_hash{0};
  std::uint64_t input_state_hash{0};
  std::uint64_t target_hash{0};
  std::uint64_t command_hash{0};
  std::uint64_t session_trace_hash_before{0};
  // Raw cross-sectional target-book summaries before missing-signal hold and
  // live-leaf reconciliation. They are not account-level projected risk.
  double requested_gross_exposure{0.0};
  double realized_gross_exposure{0.0};
  double initial_margin{0.0};
  double maintenance_margin{0.0};
  std::size_t active_leaf_count{0};
  std::size_t retained_leaf_count{0};
  std::size_t deferred_contract_count{0};
  std::size_t submitted_order_count{0};
  std::size_t cancellation_count{0};
  std::size_t missing_signal_count{0};
  bool margin_clamped{false};
  bool cancel_barrier{false};

  [[nodiscard]] bool operator==(const OptionAdaptiveDecisionAudit &) const noexcept = default;
};

struct OptionAdaptiveRunView {
  execution::OptionExecutionSessionResult execution{};
  std::span<const OptionAdaptiveDecisionAudit> decisions;
  std::uint64_t model_version{kOptionAdaptiveCoordinatorModelVersion};
  std::uint64_t ordering_version{kOptionAdaptiveCoordinatorOrderingVersion};
  std::uint64_t run_definition_hash{0};
  std::uint64_t decision_trace_hash{0};
};

// Exact reserved payload bytes required by the coordinator itself. Container
// objects, allocator metadata, and the nested execution-session workspace are
// excluded.
[[nodiscard]] atx::core::Result<std::size_t>
option_adaptive_coordinator_required_workspace_bytes(const OptionAdaptiveCoordinatorLimits &limits);

// Reusable bounded coordinator. create() performs every capacity allocation.
// A successful run performs no allocation. Returned spans borrow from this
// object and are invalidated by the next run, move, or destruction.
//
// Decisions are explicit panel dates. Each date is advanced, observed, sized,
// reconciled, and acknowledged exactly once. No fill creates a hidden strategy
// decision. A pending cancel remains economically exposed and cannot be treated
// as terminal. This revision models synthetic cancel/new only; venue-native
// replace, acknowledgements/rejections, atomic complex execution, assignment,
// settlement, and broker/OCC portfolio margin remain outside the fidelity claim.
class OptionAdaptiveCoordinator {
public:
  [[nodiscard]] static atx::core::Result<OptionAdaptiveCoordinator>
  create(OptionAdaptiveCoordinatorLimits limits = {});

  ~OptionAdaptiveCoordinator();
  OptionAdaptiveCoordinator(OptionAdaptiveCoordinator &&) noexcept;
  OptionAdaptiveCoordinator &operator=(OptionAdaptiveCoordinator &&) noexcept;
  OptionAdaptiveCoordinator(const OptionAdaptiveCoordinator &) = delete;
  OptionAdaptiveCoordinator &operator=(const OptionAdaptiveCoordinator &) = delete;

  [[nodiscard]] atx::core::Result<OptionAdaptiveRunView>
  run(const research::OptionResearchPanel &panel, const execution::OptionReplayInputs &inputs,
      const execution::OptionReplayConfig &replay_config,
      const OptionAdaptiveCoordinatorConfig &config);

  [[nodiscard]] OptionAdaptiveCoordinatorState state() const noexcept;

private:
  struct Impl;
  explicit OptionAdaptiveCoordinator(std::unique_ptr<Impl> impl) noexcept;
  std::unique_ptr<Impl> impl_;
};

} // namespace atx::options::adaptive
