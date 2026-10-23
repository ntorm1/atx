#pragma once

// Internal seam (lane C4, r6 2026-10-23): the exact in-box projection rung
// (QuoteSliceRepairConfig::inbox_projection, opt-in). Called from quote_slice.cpp at one point
// only - box_certify_slice, after certify_american_box refused the fitted prices. Definitions
// live in inbox_projection.cpp. Not public API.

#include <cstddef>
#include <span>
#include <string_view>

#include "atx/core/error.hpp"
#include "atx/vol/api/fitting/american_price_slice.hpp"
#include "atx/vol/api/fitting/quote_slice.hpp"

namespace atx::vol::detail {

// Newton-step budget of one projection, phase I and phase II together.
inline constexpr std::size_t kInboxMaxNewtonSteps = 600;

// Attribution of certify_american_box's refusal from its message (diagnostic only).
[[nodiscard]] QuoteSliceBoxRefusal classify_box_refusal(std::string_view message) noexcept;

// Exact in-box projection of one expiry's fitted node prices (one box per node, as
// certify_american_box takes them: a quote's [bid - h, ask + h], a side without a quote held
// at its fitted price with lo == hi).
//
// Feasible set, every inequality exact (no allowance; a box that misses the certificate's own
// per-node bound by at most the certificate allowance is pinned to its nearest quote edge):
//   each node side inside its box intersected with the certificate's node bounds (intrinsic,
//   upper) and, in the non-dividend regime (bounds.prepaid_min == prepaid_max == spot and
//   discount_max == 1), the discounted-intrinsic call floor spot - K * discount_min;
//   each side convex in strike; call slopes in [-discount_max, 0] (non-dividend:
//   [-discount_min, 0]); put slopes in [0, discount_max]; at every node the affine bound
//   prepaid_min - K discount_max <= call - put <= prepaid_max - K discount_min; non-dividend
//   only, check_nondividend_american's Legendre segment condition. Convexity of consecutive
//   triples plus the two end slopes imply every vertical-spread bound of
//   AmericanPriceSlice::create, so a curve strictly inside this set passes create() exactly.
// Objective: least squares to the fitted prices in units of each side's box half-width,
//   sum ((P - fitted) / h)^2 over the sides that may move (lo < hi).
// Method: exact infeasibility tests first (EmptyBox; NoConvexCurve against the greatest
//   convex curve with the side's slopes under the upper edges; AffineBox), then a two-phase
//   log-barrier interior-point method on the banded Newton system (interleaved call / put
//   variables, half-bandwidth 4; damped Newton steps, so every iterate stays strictly
//   inside every constraint). Phase I minimises the worst violation s: s < 0 proves an
//   interior point, a certified lower bound s - m / t > 0 proves the set empty (Coupled).
//   Phase II projects from that interior point; its result is strictly inside every box.
//
// Returns the projected curve certified by the UNCHANGED AmericanPriceSlice::create (moved
// flags against `fitted`). InfeasibleData: Unavailable, naming the conflict; NonConverged
// (budget spent): OutOfRange; NotCertified: create()'s own error; InvalidInput:
// InvalidArgument. `record` is reset and then describes the run (outcome, conflict, steps,
// distances, the rung's wall); `box_refusal` is certify_american_box's message, read only
// for record.box_refusal. Deterministic (the clock feeds only record.elapsed_us); allocates
// O(nodes); thread-compatible.
[[nodiscard]] Result<AmericanBoxCertificate>
project_american_in_box(std::span<const AmericanPriceNode> fitted,
                        std::span<const AmericanPriceBox> boxes,
                        const AmericanPriceBounds &bounds, std::string_view box_refusal,
                        QuoteSliceInboxRecord &record,
                        std::size_t max_newton_steps = kInboxMaxNewtonSteps);

} // namespace atx::vol::detail
