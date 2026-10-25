#pragma once

#include <cstddef>
#include <utility>
#include <nlohmann/json.hpp>
#include "fitting/joint_working_set_work.hpp"

namespace atx::vol::quote_bench_joint_working_set {
using Json = nlohmann::json;

inline Json report(const detail::JointWorkingSetAggregate &a) {
  const auto &first = a.first;
  Json slots = Json::array();
  for (std::size_t i = 0; i < first.attempts; ++i) {
    const auto &s = first.solves[i];
    slots.push_back({{"full_fallback", s.full_fallback}, {"selected_rows", s.selected_rows},
        {"iterations", s.iterations}, {"factorizations", s.factorizations},
        {"directions", s.directions}, {"matrix_nonzeros", s.matrix_nonzeros},
        {"factor_nonzeros_peak", s.factor_nonzeros_peak},
        {"iterate_scan_rows", s.iterate_scan_rows},
        {"publication_scan_rows", s.publication_scan_rows},
        {"violated_published_rows", s.violated_rows}, {"newly_added_rows", s.newly_added_rows},
        {"min_iterate_slack", s.min_iterate_slack}, {"min_published_slack", s.min_published_slack},
        {"outcome", static_cast<unsigned>(s.outcome)}});
  }
  return {{"schema", "joint-working-set-work-v1"},
      {"scope", "Global projection charged once to first chain; sums across all invocations"},
      {"invocations", a.invocations},
      {"synthetic_invocations", a.synthetic_invocations},
      {"injected_refusals", a.injected_refusals},
      {"slot_detail_invocations_omitted", a.slot_detail_invocations_omitted},
      {"full_variables_sum", a.full_variables}, {"full_rows_sum", a.full_rows},
      {"bound_rows_sum", a.bound_rows}, {"initial_scan_rows", a.initial_scan_rows},
      {"initial_violated_rows", a.initial_violated_rows},
      {"selection_scan_rows", a.selection_scan_rows}, {"attempts", a.attempts},
      {"reduced_solves", a.reduced_solves}, {"full_solves", a.full_solves},
      {"iterations", a.iterations}, {"factorizations", a.factorizations},
      {"directions", a.directions}, {"full_scan_rows", a.full_scan_rows},
      {"newly_added_rows", a.newly_added_rows},
      {"workspace_bytes_requested_sum", a.workspace_bytes_requested},
      {"allocation_refusals", a.allocation_refusals}, {"capacity_refusals", a.capacity_refusals},
      {"first_invocation", {{"present", a.invocations != 0u},
          {"synthetic_control", first.synthetic_control},
          {"injected_refusals", first.injected_refusals},
          {"full_variables", first.full_variables}, {"full_rows", first.full_rows},
          {"bound_rows", first.bound_rows}, {"initial_scan_rows", first.initial_scan_rows},
          {"initial_violated_rows", first.initial_violated_rows},
          {"selection_scan_rows", first.selection_scan_rows},
          {"full_scan_rows", first.full_scan_rows}, {"workspace_bytes", first.workspace_bytes},
          {"fallback_reason", static_cast<unsigned>(first.fallback_reason)},
          {"solves", std::move(slots)}}}};
}
} // namespace atx::vol::quote_bench_joint_working_set
