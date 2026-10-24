// risk_kernel_cold — the oracle kernel: per contract, resolve against the state and
// price the resolved (S, K, T, sigma, r, q_eff) through american_greeks_al /
// american_greeks_fd. Deliberately simple: it is what the family kernel is pinned to.
//
// NearExerciseEdge on this kernel means the EXERCISE REGIME only (the served price on
// a positive intrinsic, see cold_row in risk_kernel.cpp): the cold bundles expose no
// edge distance, so continuation-side lanes inside the family's 0.25 sigma sqrt(T)
// band are not flagged here. That is narrower than design F5. On the family kernel
// the full band meaning holds only for lanes the family actually judged; lanes it
// sends cold before a family answer (tau floor, failed prepare / carry set, off
// geometry, below the floor, outside the box) carry this same narrow bit.

#include <cstddef>
#include <cstdint>
#include <span>

#include "atx/vol/api/pricing/pricing_executor.hpp"
#include "risk/risk_kernel_detail.hpp"

namespace atx::vol::risk_detail {

void evaluate_cold(const PreparedRiskBook &book, const MarketState &state, RiskNeeds needs,
                   const ColdKernelPolicy &policy, RiskFrame &out) {
  const std::span<const std::uint32_t> uid = book.uid();
  const std::span<const double> strike = book.strike();
  const std::span<const std::int64_t> expiry = book.expiry_ns();
  const std::span<const Side> side = book.side();
  // One row per index, each written only by its own worker: the frame is a pure
  // function of (book, state, needs, policy) for any thread count.
  pricing_executor().run_blocks(book.n_unique(), policy.n_threads, [&](std::size_t r) {
    const ResolvedContract rc = state.resolve(uid[r], strike[r], expiry[r], side[r]);
    write_resolved(out, r, rc);
    if (rc.status != RiskLaneStatus::Ok) {
      return;
    }
    const AlOpts opts = resolve_al_opts(policy.al_opts, state, uid[r]);
    cold_row(out, r, rc.spot, strike[r], side[r], opts, policy.analytic, needs);
  });
}

} // namespace atx::vol::risk_detail
