#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace atx::vol::detail {
enum class JointConstraintMode { Compiled, Full, WorkingSet };
enum class JointWorkingReason {
  None, Capacity, OptionalAllocation, ReducedNonConverged, NonfiniteScan,
  NoProgress, RoundLimit, CertificateRefused
};
enum class JointWorkingSolveOutcome {
  Unattempted, Feasible, NonConverged, CertificateRefused, AllocationRefused, Aborted
};
struct JointWorkingSolveWork {
  bool full_fallback = false;
  std::size_t selected_rows = 0, iterations = 0, factorizations = 0, directions = 0;
  std::size_t matrix_nonzeros = 0, factor_nonzeros_peak = 0;
  std::size_t iterate_scan_rows = 0, publication_scan_rows = 0;
  std::size_t violated_rows = 0, newly_added_rows = 0;
  double min_iterate_slack = 0., min_published_slack = 0.;
  JointWorkingSolveOutcome outcome = JointWorkingSolveOutcome::Unattempted;
};
// Exactly one projection invocation. A separate fit counter aggregator retains
// the first invocation's slots and labels any subsequent slot omission while
// accumulating every invocation's actual totals.
struct JointWorkingSetWork {
  bool enabled = false;
  bool synthetic_control = false;
  std::size_t injected_refusals = 0;
  std::size_t full_variables = 0, full_rows = 0, bound_rows = 0;
  std::size_t initial_scan_rows = 0, initial_violated_rows = 0, selection_scan_rows = 0;
  std::size_t attempts = 0, reduced_solves = 0, full_solves = 0;
  std::size_t iterations = 0, factorizations = 0, directions = 0;
  std::size_t full_scan_rows = 0, newly_added_rows = 0;
  std::size_t workspace_bytes = 0, allocation_refusals = 0, capacity_refusals = 0;
  JointWorkingReason fallback_reason = JointWorkingReason::None;
  std::array<JointWorkingSolveWork, 4> solves{};
};
// Private test-only conservative controls. Normal library/CLI callers always
// pass null. Limits may only reduce the research bounds; no control supplies a
// price, changes a row, or admits an otherwise refused result. The synchronous
// callback is borrowed, may throw bad_alloc, and is used to arm real allocation
// injection immediately before optional container setup.
enum class JointWorkingFault { None, ReducedRefusal, ScanRefusal, NoProgress, CertificateRefusal };
struct JointWorkingSetControl {
  std::size_t variables_limit = 14000u, rows_limit = 60000u, reduced_rounds_limit = 3u;
  JointWorkingFault fault = JointWorkingFault::None;
  void (*before_optional_allocation)(void *) = nullptr;
  void *context = nullptr;
};
[[nodiscard]] bool joint_working_set_compiled() noexcept;

// Fixed storage, no clocks. Every enabled invocation contributes to totals.
// Detailed slots describe ONLY the first invocation; later omissions are explicit.
struct JointWorkingSetAggregate {
  std::size_t invocations = 0, slot_detail_invocations_omitted = 0;
  std::size_t synthetic_invocations = 0, injected_refusals = 0;
  std::size_t full_variables = 0, full_rows = 0, bound_rows = 0;
  std::size_t initial_scan_rows = 0, initial_violated_rows = 0, selection_scan_rows = 0;
  std::size_t attempts = 0, reduced_solves = 0, full_solves = 0;
  std::size_t iterations = 0, factorizations = 0, directions = 0;
  std::size_t full_scan_rows = 0, newly_added_rows = 0;
  std::size_t workspace_bytes_requested = 0, allocation_refusals = 0, capacity_refusals = 0;
  JointWorkingSetWork first{};
  void add(const JointWorkingSetWork &work) noexcept {
    if (!work.enabled) return;
    if (invocations++ == 0u) first = work;
    else ++slot_detail_invocations_omitted;
    synthetic_invocations += work.synthetic_control ? 1u : 0u;
    injected_refusals += work.injected_refusals;
    full_variables += work.full_variables; full_rows += work.full_rows;
    bound_rows += work.bound_rows; initial_scan_rows += work.initial_scan_rows;
    initial_violated_rows += work.initial_violated_rows;
    selection_scan_rows += work.selection_scan_rows;
    attempts += work.attempts; reduced_solves += work.reduced_solves;
    full_solves += work.full_solves; iterations += work.iterations;
    factorizations += work.factorizations; directions += work.directions;
    full_scan_rows += work.full_scan_rows; newly_added_rows += work.newly_added_rows;
    workspace_bytes_requested += work.workspace_bytes;
    allocation_refusals += work.allocation_refusals; capacity_refusals += work.capacity_refusals;
  }
};
} // namespace atx::vol::detail
