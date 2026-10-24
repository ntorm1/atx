#pragma once

#include <cstdint>
#include <optional>
#include <span>

#include "pricing/american_iv_search.hpp"
#include "pricing/american_iv_seed_batch.hpp"

namespace atx::vol {

struct IvPriceCandidateRequest {
  double price = 0., S = 0., K = 0., T = 0., r = 0., q = 0., budget = 0.;
  Side side = Side::Put;
};
struct IvPriceCandidateResult {
  std::optional<double> sigma;
  IvPriceCandidateStats stats;
  IvPriceSeedResult screen_proof;
  bool allocation_refused = false;
};
struct IvPriceCandidateBatchStats {
  std::uint64_t requests = 0, screen_proposals = 0, screen_seeds = 0;
  std::uint64_t fast_continuations = 0, allocation_refusals = 0;
  std::uint64_t setup_allocation_fallbacks = 0, complete_scalar_fallbacks = 0;
  std::uint64_t continuation_floor_preparations = 0, continuation_floor_hits = 0;
  std::uint64_t setup_ns = 0, proposal_ns = 0, continuation_ns = 0, inclusive_ns = 0;
  IvPriceSeedBatchStats proof;
};

// Private synchronous experiment, no ordinary/default dispatch. All requests
// share exact opts; request/output spans must be disjoint and remain live.
// Screen proposals retain their original scalar inverse. Only their proof is
// batched; failures resume the shared original Fast chain without rerunning
// Screen. Fresh floor owners and scratch destruction are charged inclusively.
// Output sigma is supplied only after a complete proof; screen_proof retains
// explicit empirical/scalar origin and never supplies a generic error bound.
// Scratch allocation refusal falls back to the complete scalar path for every
// request before any proposal is run. Escaping Screen/Fast bad_alloc follows
// per-call refusal. A contained proof allocation failure resumes the original
// Fast chain, as the scalar seed helper does; screen_proof retains that refusal.
[[nodiscard]] bool american_implied_vol_price_candidate_batch(
    std::span<const IvPriceCandidateRequest> requests, const IvOpts &opts,
    IvPriceSeedKernel proof_kernel, std::span<IvPriceCandidateResult> out,
    IvPriceCandidateBatchStats &stats, bool timed = false,
    IvPriceProposalKernel proposal_kernel = IvPriceProposalKernel::ScreenThenFast,
    const research::IvSeedColdFloorTestHook *test_hook = nullptr);

} // namespace atx::vol
