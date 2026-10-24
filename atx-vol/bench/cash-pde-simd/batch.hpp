#pragma once

// Private, default-off full PDE experiment. The production entry point never
// dispatches here. Both the callback and any diagnostics are borrowed for this
// synchronous call only; all model/workspace storage is owned inside the call.
#include "thomas.hpp"

#include <cstdint>
#include <span>
#include <vector>

#include "pricing/american_discrete_div_reference.hpp"

namespace atx::vol::ddiv_ref::research {

using Sweep = Statuses (*)(ThomasPack &, std::uint8_t) noexcept;

struct BatchDiagnostics {
  std::vector<PdeDiagnostics> lanes; // original request order, including refusals
  std::uint64_t bundles = 0; // up to four lanes; a calendar can need many bundles
  std::uint64_t vector_sweeps = 0;
  std::uint64_t active_lane_sweeps = 0;
  std::uint64_t tail_slots = 0;
  std::uint64_t retired_slots = 0;
  std::uint64_t mask_disagreement_rows = 0;
};

// Groups only equal canonical event calendars, exercise styles and grid sizes;
// coefficients/model values may differ. Every input receives one result in its
// original position. A refused lane cannot prevent other lanes from completing.
// The callback must preserve masked lanes and implement the ThomasPack contract.
// No scalar retry hides a batch refusal. Null callback is InvalidArgument.
[[nodiscard]] std::vector<Result<PdeResult>> pde_price_batch(
    std::span<const Problem> problems, const PdeGrid &grid, Sweep sweep,
    BatchDiagnostics *diagnostics = nullptr);

} // namespace atx::vol::ddiv_ref::research
