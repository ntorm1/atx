#pragma once

// Public SoA batch APIs for American option pricing + Greeks (P3.4).
//
// These are the grouped, structure-of-arrays entry points that sit ON TOP of the
// T13 AVX2 American-put boundary kernel (simd/american_boundary_batch.hpp) and the
// T9 scalar Greek routes (american.hpp). They take a whole book of independent
// American options as SoA columns, GROUP the genuine early-exercise lanes into a
// homogeneous internal-put stream (a Call maps to a put via McDonald-Schroder
// C(S,K,r,q)=P(K,S,q,r)), and dispatch that stream through the REAL batched
// boundary kernel; degenerate / European / double-continuation / non-AVX2 lanes
// patch through the exact scalar andersen_lake so the batch is bit-identical to a
// per-contract scalar loop. Public output order is always preserved.
//
// ## Honest scope (READ THIS)
//   * american_price_batch dispatches T13's REAL vectorized boundary kernel. That
//     kernel's AVX2 path is GATED OFF by default (kShipAvx2Boundary=false: measured
//     ~1.7× < the 2.0× ship gate, seed-bound), so under PricingKernel{isa=Auto}
//     every lane runs the scalar boundary solve — the batch INHERITS T13's scalar
//     default. Under isa=ForceAvx2 on an AVX2 host the genuine-American lanes run
//     the vector kernel (provably exercised, validated to T13's ~6.4e-7 gate).
//   * american_greeks_batch is a SoA surface + grouping + boundary-reuse over the
//     EXISTING scalar T9 Greek routes (american_greeks_fd / american_greeks_al).
//     There is NO vectorized American Greek STENCIL — greek-stencil vectorization
//     beyond the price boundary is future work. Every Greek lane's route is Scalar.
//
// Not thread-safe against a concurrent change of the process-global SIMD ISA
// override: PricingKernel::isa is applied via that override for the duration of a
// call (the coarse T13 seam), then restored.

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "atx/vol/simd/american_boundary_batch.hpp" // SimdRoute
#include "atx/vol/simd/cpu.hpp"                      // SimdIsa
#include "atx/vol/simd/greeks_batch.hpp"             // GreeksBatchSoA
#include "atx/vol/types.hpp"                         // Status, Side

namespace atx::vol {

class PricingExecutor; // pricing_executor.hpp (optional cross-lane parallelism)

// ── Per-lane outcome ──────────────────────────────────────────────────────
//
// Ok      — a finite price / Greek bundle was produced for this lane.
// Unsupported — the scalar route returned non-finite (the double-continuation
//               negative-carry corner andersen_lake reports as NotImplemented, or
//               an invalid-argument lane); the numeric output is NaN.
enum class LaneStatus : std::uint8_t {
  Ok = 0,
  Unsupported = 1,
};

// ── SoA input over a book of independent American options ─────────────────
//
// Every span has the same length n; side[i] pairs with (S,K,T,sigma,r,q)[i]. The
// spans are caller-owned and only read.
struct AmericanBatchInput {
  std::span<const double> S;
  std::span<const double> K;
  std::span<const double> T;
  std::span<const double> sigma;
  std::span<const double> r;
  std::span<const double> q;
  std::span<const Side> side;

  [[nodiscard]] std::size_t size() const noexcept { return S.size(); }

  // All seven columns share one length.
  [[nodiscard]] bool consistent() const noexcept {
    const std::size_t n = S.size();
    return K.size() == n && T.size() == n && sigma.size() == n &&
           r.size() == n && q.size() == n && side.size() == n;
  }
};

// ── Price batch output (owning; sized once, reused across snapshots) ───────
struct PriceBatchOutput {
  std::vector<double> price;             // per-lane American mark (NaN if Unsupported)
  std::vector<LaneStatus> status;        // per-lane outcome
  std::vector<simd::SimdRoute> route;    // per-lane numerical route (Scalar/Avx2)

  void resize(std::size_t n) {
    price.assign(n, 0.0);
    status.assign(n, LaneStatus::Ok);
    route.assign(n, simd::SimdRoute::Scalar);
  }
  [[nodiscard]] std::size_t size() const noexcept { return price.size(); }

  // Fraction of lanes that did NOT execute the vector boundary kernel (route ==
  // Scalar): the scalar-fallback rate. 100% under the T13 scalar default; the
  // genuine-degenerate/European fraction under ForceAvx2 on an AVX2 host.
  [[nodiscard]] double scalar_fallback_rate() const noexcept {
    if (route.empty()) {
      return 0.0;
    }
    std::size_t nscalar = 0;
    for (const simd::SimdRoute rt : route) {
      if (rt == simd::SimdRoute::Scalar) {
        ++nscalar;
      }
    }
    return static_cast<double>(nscalar) / static_cast<double>(route.size());
  }
};

// ── Pricing kernel handle (wraps the T13 ISA seam) ────────────────────────
//
// Minimal by design: the ISA/route selection plus an optional executor for
// cross-lane parallelism of the scalar Greek routes.
struct PricingKernel {
  // Applied via the process-global SIMD ISA override for the call's duration.
  // Auto inherits the T13 ship gate (scalar today); ForceAvx2/ForceScalar force.
  simd::SimdIsa isa{simd::SimdIsa::Auto};

  // american_greeks_batch route: false -> american_greeks_fd (FD reference),
  // true -> american_greeks_al (analytic; defers to FD off its supported regime).
  bool analytic_greeks{false};

  // Optional persistent pool (pricing_executor.hpp). When set, american_greeks_batch
  // fans its independent per-lane scalar Greek solves across the pool (disjoint
  // per-lane writes -> bit-identical for any worker count). Null -> serial.
  PricingExecutor* executor{nullptr};
};

// ── Pack scratch (owning; grows monotonically, no per-call heap churn) ─────
//
// Holds the compacted homogeneous internal-put SoA stream fed to the boundary
// kernel, the permutation back to public order, and the per-lane Greek-path
// diagnostics (american_greeks_batch has no separate status/route out-param, so
// they live here and are read back via lane_status()/lane_route()).
class PricingWorkspace {
 public:
  // Size the scratch to hold at least n lanes (idempotent; never shrinks).
  void reserve_lanes(std::size_t n) {
    if (ps_S.size() < n) {
      ps_S.resize(n);
      ps_K.resize(n);
      ps_T.resize(n);
      ps_sigma.resize(n);
      ps_r.resize(n);
      ps_q.resize(n);
      ps_price.resize(n);
      perm.resize(n);
    }
    lane_status.assign(n, LaneStatus::Ok);
    lane_route.assign(n, simd::SimdRoute::Scalar);
  }

  // Per-lane Greek-path diagnostics after american_greeks_batch (length n).
  [[nodiscard]] std::span<const LaneStatus> lane_status_view() const noexcept {
    return lane_status;
  }
  [[nodiscard]] std::span<const simd::SimdRoute> lane_route_view() const noexcept {
    return lane_route;
  }

  // Internal columns (public so the batch implementation writes them directly).
  std::vector<double> ps_S, ps_K, ps_T, ps_sigma, ps_r, ps_q, ps_price;
  std::vector<std::uint32_t> perm; // pack slot -> original public lane index
  std::vector<LaneStatus> lane_status;
  std::vector<simd::SimdRoute> lane_route;
};

// ── Greek field selector (models PriceFieldMask; per-greek granularity) ────
enum class GreekFieldMask : std::uint32_t {
  None = 0,
  Delta = 1u << 0,
  Gamma = 1u << 1,
  Vega = 1u << 2,
  Theta = 1u << 3,
  Rho = 1u << 4,
  Vanna = 1u << 5,
  Volga = 1u << 6,
  Charm = 1u << 7,
  Price = 1u << 8,
  AllGreeks = Delta | Gamma | Vega | Theta | Rho | Vanna | Volga | Charm,
  All = AllGreeks | Price,
};

[[nodiscard]] constexpr GreekFieldMask operator|(GreekFieldMask a,
                                                 GreekFieldMask b) noexcept {
  return static_cast<GreekFieldMask>(static_cast<std::uint32_t>(a) |
                                     static_cast<std::uint32_t>(b));
}
[[nodiscard]] constexpr GreekFieldMask operator&(GreekFieldMask a,
                                                 GreekFieldMask b) noexcept {
  return static_cast<GreekFieldMask>(static_cast<std::uint32_t>(a) &
                                     static_cast<std::uint32_t>(b));
}
[[nodiscard]] constexpr bool has_field(GreekFieldMask set,
                                       GreekFieldMask bit) noexcept {
  return (static_cast<std::uint32_t>(set) & static_cast<std::uint32_t>(bit)) !=
         0u;
}

// ── The batch APIs ────────────────────────────────────────────────────────

// Price a book of American options into `out` (SoA). Groups the genuine
// early-exercise lanes into a homogeneous internal-put pack and dispatches it
// through simd::american_put_boundary_batch (T13); every other lane patches to the
// exact scalar andersen_lake. Public output order is preserved (out[i] pairs with
// input lane i). Returns InvalidArgument on a span-length mismatch.
[[nodiscard]] Status american_price_batch(const AmericanBatchInput& in,
                                           PriceBatchOutput& out,
                                           PricingKernel& kernel,
                                           PricingWorkspace& ws);

// American Greeks for a book into the SoA columns of `greeks` (only the fields in
// `fields` — and only non-null columns — are written). Each lane's Greeks come
// from the EXISTING scalar T9 route (american_greeks_fd or, if kernel.analytic_greeks,
// american_greeks_al); this is grouping + SoA + boundary-reuse, NOT a vectorized
// Greek stencil (every lane's route is Scalar). Per-lane status/route land in the
// workspace (ws.lane_status_view()/lane_route_view()). Returns InvalidArgument on
// a span-length mismatch.
[[nodiscard]] Status american_greeks_batch(const AmericanBatchInput& in,
                                           GreekFieldMask fields,
                                           simd::GreeksBatchSoA& greeks,
                                           PricingKernel& kernel,
                                           PricingWorkspace& ws);

} // namespace atx::vol
