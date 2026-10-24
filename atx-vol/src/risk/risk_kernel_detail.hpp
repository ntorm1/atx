#pragma once

// risk_kernel_detail — the private seam between the RiskKernel strategy wrapper
// (risk_kernel.cpp) and its two kernels (risk_kernel_family.cpp, risk_kernel_cold.cpp):
// the workspace state, the shared row writers and the cold-route pricer.
//
// The wrapper validates the call, sizes / binds the frame and the workspace, fills
// the per-uid and per-share rows and tallies the counters; a kernel fills the
// per-contract rows only, each row from exactly one worker.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

#include "atx/vol/api/core/types.hpp"
#include "atx/vol/api/pricing/american.hpp"
#include "atx/vol/api/risk/market_state.hpp"
#include "atx/vol/api/risk/risk_book.hpp"
#include "atx/vol/api/risk/risk_frame.hpp"
#include "atx/vol/api/risk/risk_kernel.hpp"
#include "atx/vol/api/risk/risk_types.hpp"

namespace atx::vol::risk_detail {

// A run of adjacent prepared groups sharing (uid, expiry_ns): the call and the put
// group of one expiry share ONE Andersen-Lake geometry (the carry is K- and
// side-independent), so the family kernel fans out over blocks, not groups.
struct ExpiryBlock {
  std::uint32_t group_begin{0};
  std::uint32_t group_end{0};
};

// Per-block tallies written by the block's worker, reduced serially afterwards.
struct BlockCounters {
  std::uint64_t families_built{0};
  std::uint64_t carry_sets_built{0};
  double family_build_seconds{0.0};
};

// The retained family-kernel state of one book shape. Member order matters: the
// families observe the geometries, so `family` is declared after `geometry` and is
// therefore destroyed first.
struct WorkspaceState {
  std::uint64_t book_id{0};
  std::vector<ExpiryBlock> blocks;
  std::vector<AlExpiryGeometry> geometry;           // per block; never resized while bound
  std::vector<std::optional<AlSigmaFamily>> family; // per group
  std::vector<AlSigmaFamilyCarryBumps> carry;       // per group
  std::vector<std::uint8_t> cold;                   // per prepared row: handed to the cold pass
  std::vector<BlockCounters> block_counters;        // per block

  // (Re)bind to `book`: idempotent for the bound book, a full reset for another.
  void bind(const PreparedRiskBook &book);
};

// The Andersen-Lake preset a uid's lanes are priced with (risk_kernel.hpp "Pricer
// options precedence"): the policy's, else the uid's surface's, else al_fast_opts().
[[nodiscard]] AlOpts resolve_al_opts(const std::optional<AlOpts> &policy,
                                     const MarketState &state, std::uint32_t uid) noexcept;

// Copy a resolved contract into row `r` (T / sigma / carry / vendor / flags / status).
// A non-Ok resolution leaves the row Refused with NaN numbers.
void write_resolved(RiskFrame &f, std::size_t r, const ResolvedContract &rc) noexcept;

// Make row `r` a refusal: NaN in every numeric column, `status`, route Refused.
void write_refused(RiskFrame &f, std::size_t r, RiskLaneStatus status) noexcept;

// Price row `r` on the cold route from its resolved (T, sigma, rate, q_eff) at spot S:
// american_greeks_al (analytic) or american_greeks_fd. Unrequested axes are 0.0. A
// pricer error or a non-finite requested value refuses the row NumericError. A row
// whose served price sits on intrinsic (the exercise regime: the cold bundle's only
// edge signal) is flagged NearExerciseEdge. Not noexcept: the solvers may throw
// bad_alloc, which the executor carries back to the caller.
void cold_row(RiskFrame &f, std::size_t r, double S, double K, Side side, const AlOpts &opts,
              bool analytic, RiskNeeds needs);

// The kernels. Preconditions (checked by RiskKernel::evaluate): the frame is sized
// and cleared for `book`, its per-uid rows filled, `ws` is bound to `book`.
void evaluate_cold(const PreparedRiskBook &book, const MarketState &state, RiskNeeds needs,
                   const ColdKernelPolicy &policy, RiskFrame &out);
void evaluate_family(const PreparedRiskBook &book, const MarketState &state, RiskNeeds needs,
                     const FamilyKernelPolicy &policy, RiskFrame &out, WorkspaceState &ws);

} // namespace atx::vol::risk_detail
