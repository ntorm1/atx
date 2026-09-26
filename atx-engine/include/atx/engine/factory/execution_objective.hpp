#pragma once

#include <memory>
#include <span>
#include <string>
#include <string_view>
#include "atx/core/error.hpp"
#include "atx/core/types.hpp"

namespace atx::engine { struct WeightPolicy; }
namespace atx::engine::alpha { class Panel; struct SignalSet; struct AlphaStreams; }
namespace atx::engine::cost { class CostSurface; }

namespace atx::engine::factory {
enum class ExecutionObjectiveRule : atx::u8 { LegacyStreamsV1=1, DelayedSurfaceV2=2 };
enum class ExecutionBorrowRule : atx::u8 { DisabledExplicitV1=1, RequireModeledV2=2 };

struct ExecutionObjectiveConfig {
  ExecutionObjectiveRule rule{ExecutionObjectiveRule::LegacyStreamsV1};
  atx::usize delay{1}; // V2 requires >=1; entry=d+delay, realized endpoint=entry+1
  atx::usize window_begin{};
  atx::usize window_end{}; // exclusive signal-date bound; 0 resolves to Panel dates
  atx::usize maturity_end{}; // exclusive realized endpoint bound; 0 resolves to window_end
  atx::usize min_names{2};
  atx::f64 initial_nav{1'000'000'000.0};
  ExecutionBorrowRule borrow{ExecutionBorrowRule::RequireModeledV2};
  atx::f64 borrow_days_per_year{365.0};
  bool guard_returns{true}; // true requires the supplied cumulative ReturnGuard
  std::string price_field{"close"};
  atx::u64 max_working_bytes{atx::u64{512}*1024U*1024U};
  // Explicit scheduled policy. Phase is relative to window_begin; off-cycle
  // positions remain marked and charged borrow. Partial targets interpolate
  // decision-known held dollars toward desired dollars before queuing; actual
  // fill deltas are netted against marked holdings at entry. Defaults preserve
  // the original V2 arithmetic/identity. Nondefaults extend the hashed recipe.
  atx::usize rebalance_sessions{1};
  atx::f64 trade_fraction{1.0};
};
struct ExecutionObjectiveIdentity {
  std::string source_sha256; // bound panel/role provenance, 64 lowercase hex
  std::string role; // train / validation / holdout / explicitly named synthetic role
  std::string price_recipe; // declared total-return-compatible close/mark convention
};
struct ExecutionCashClaimEvent;
struct ExecutionCashClaimStreams;
namespace execution_objective_detail { struct Context; }

// Immutable owned execution inputs and WeightPolicy; copies share read-only storage.
// The original Panel's storage address is a process-local matching token, never
// serialized identity. Replacing its storage requires preparing a new context.
class ExecutionObjectiveContext {
public:
  ExecutionObjectiveContext()=default;
  [[nodiscard]] atx::usize dates() const noexcept;
  [[nodiscard]] atx::usize instruments() const noexcept;
  [[nodiscard]] atx::usize first_decision() const noexcept;
  [[nodiscard]] atx::usize decision_end() const noexcept;
  [[nodiscard]] atx::usize first_realization() const noexcept;
  [[nodiscard]] atx::usize realization_end() const noexcept;
  [[nodiscard]] const ExecutionObjectiveConfig& config() const noexcept;
  [[nodiscard]] std::string_view identity_sha256() const noexcept;
  // Admission charges retained payload plus explicit allocator/control-block
  // slack, not measured RSS. Per-signal includes output, pending targets and
  // book/WeightPolicy scratch; context + concurrent calls must fit the config
  // maximum. Existing VM/search caches are separate from this execution budget.
  [[nodiscard]] atx::u64 bytes() const noexcept;
  [[nodiscard]] atx::u64 per_signal_working_bytes() const noexcept;
private:
  std::shared_ptr<const execution_objective_detail::Context> data_;
  friend atx::core::Result<ExecutionObjectiveContext> prepare_execution_objective(
      const alpha::Panel&,const WeightPolicy&,const ExecutionObjectiveConfig&,
      std::span<const cost::CostSurface>,std::span<const atx::i64>,
      std::span<const atx::i64>,std::span<const atx::u64>,
      const ExecutionObjectiveIdentity&,std::span<const atx::u8>,
      std::span<const atx::u32>,std::span<const atx::u32>);
  friend atx::core::Result<ExecutionObjectiveContext> prepare_execution_objective_claims(
      const alpha::Panel&,const WeightPolicy&,const ExecutionObjectiveConfig&,
      std::span<const cost::CostSurface>,std::span<const atx::i64>,
      std::span<const atx::i64>,std::span<const atx::u64>,
      const ExecutionObjectiveIdentity&,std::span<const ExecutionCashClaimEvent>,
      std::span<const atx::u8>,std::span<const atx::u32>,std::span<const atx::u32>);
  friend atx::core::Result<ExecutionCashClaimStreams> extract_execution_signal_claims(
      std::span<const atx::f64>,const ExecutionObjectiveContext&,atx::f64);
  friend bool execution_objective_matches(const ExecutionObjectiveContext&,
      const alpha::Panel&,const WeightPolicy&,const ExecutionObjectiveConfig&) noexcept;
  friend bool execution_support_matches(const ExecutionObjectiveContext&,
      std::span<const atx::u8>,std::span<const atx::u32>) noexcept;
  friend atx::core::Result<alpha::AlphaStreams> extract_execution_streams(
      const alpha::SignalSet&,const ExecutionObjectiveContext&,atx::f64);
  friend atx::core::Result<alpha::AlphaStreams> extract_execution_signal(
      std::span<const atx::f64>,const ExecutionObjectiveContext&,atx::f64);
};

// Snapshots are in decision order [first_decision, decision_end); their exact
// ID axis and decision clock must match. mark_times/decision_times each have D
// rows and must establish mark[d] < decision[d] < mark[d+1] on used decisions.
// Membership and guard are D*N; group_map is empty, N or D*N. Eligibility reads
// only decision d. A missing/guarded realized return on a held asset is an Err.
// Snapshots contain strictly prior liquidity; future entry prices do not form
// weights. At d store fixed target dollars using NAV known at d. At entry rebalance
// marked holdings toward those stored dollars; apply snapshot-d participation
// caps to actual fills and debit cash costs. Short proceeds remain in cash.
// This is a total-return marked-dollar book: fractional fills are permitted;
// corporate-action claims require the explicit execution_cash_claim.hpp route. Negative cash beyond
// 32*epsilon*current-positive-NAV refuses after completed fills/borrow because
// this recipe has no cash-funding input; transient intra-batch cash is permitted.
// There is no automatic terminal liquidation: ending NAV includes the
// final marked holdings, and terminal liquidation costs are not fabricated.
[[nodiscard]] atx::core::Result<ExecutionObjectiveContext> prepare_execution_objective(
    const alpha::Panel& panel,const WeightPolicy& policy,const ExecutionObjectiveConfig& config,
    std::span<const cost::CostSurface> decision_snapshots,
    std::span<const atx::i64> mark_times_ns,std::span<const atx::i64> decision_times_ns,
    std::span<const atx::u64> instrument_ids,const ExecutionObjectiveIdentity& identity,
    std::span<const atx::u8> decision_membership={},
    std::span<const atx::u32> bad_return_prefix={},std::span<const atx::u32> group_map={});
[[nodiscard]] bool execution_objective_matches(const ExecutionObjectiveContext& context,
    const alpha::Panel& panel,const WeightPolicy& policy,const ExecutionObjectiveConfig& config) noexcept;

// Compare the caller's separately supplied support once per role. Empty member
// means all ones before Panel membership; guard is ignored only when disabled
// explicitly in the context recipe. No shape-only support substitution.
[[nodiscard]] bool execution_support_matches(const ExecutionObjectiveContext& context,
    std::span<const atx::u8> decision_membership,
    std::span<const atx::u32> bad_return_prefix) noexcept;

// Outputs retain the full Panel calendar axis. PnL is indexed by realized
// endpoint e+1, and includes entry-e costs plus the e -> e+1 holding return/borrow.
// Valid rows are the contiguous [first_realization, realization_end); outside
// rows are NaN/invalid, never structural zero observations. The corresponding
// decision index is realized_index-delay-1. Sign is independently rescored, not
// A nonempty cash-claim context requires extract_execution_signal_claims.
// Negating PnL cannot synthesize the negative sign (borrow/caps/NAV make that invalid).
[[nodiscard]] atx::core::Result<alpha::AlphaStreams> extract_execution_streams(
    const alpha::SignalSet& signals,const ExecutionObjectiveContext& context,atx::f64 sign=1.0);
[[nodiscard]] atx::core::Result<alpha::AlphaStreams> extract_execution_signal(
    std::span<const atx::f64> signal,const ExecutionObjectiveContext& context,atx::f64 sign=1.0);
} // namespace atx::engine::factory
