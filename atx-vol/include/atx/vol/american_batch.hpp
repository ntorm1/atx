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
// ## Scope (READ THIS) — updated 2026-07-19 (WS-K AVX2 default-ON flip)
//   * american_price_batch dispatches the REAL vectorized boundary kernel. The AVX2
//     path is now DEFAULT ON (kShipAvx2Boundary=true): under PricingKernel{isa=Auto}
//     the genuine early-exercise American lanes run the 4-wide vector boundary pack on
//     an AVX2-capable host (validated within the economic gate; ForceScalar and non-AVX2
//     hosts keep the exact scalar solve). isa=ForceScalar/ForceAvx2 override per call.
//   * american_greeks_batch: the FD route (analytic_greeks=false) is the scalar
//     american_greeks_fd fan. The ANALYTIC route (analytic_greeks=true) dispatches PUT
//     and CALL lanes through their K3 LANED AVX2 Greeks bundles (5 boundaries
//     4-wide/pack) when AVX2 is selected (kShipAvx2Greeks=true, so Auto uses it on
//     capable hosts; ForceAvx2 opts in explicitly), matching scalar american_greeks_al
//     within the documented economic gate. The FD route and every ineligible /
//     non-finite lane stay on the scalar oracle. Per-lane route is reported via
//     ws.lane_route_view().
//
// PricingKernel::isa is a call-local dispatch choice. Concurrent batch calls may
// select different ISAs without reading or mutating the legacy process-global
// override used by the coarse SIMD boundary API.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include "atx/vol/american.hpp"                    // AlOpts, AmericanMethod
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

// Non-owning price-only request for an already-resolved equal-expiry surface
// run. S/T/r/q are broadcast scalars because PricedSurface resolves one carry
// for each raw-bit-identical T run; K/sigma/side remain lane columns. The exact
// method and option engagement are part of the request: an engaged AlOpts must
// never be replaced by the low-level kernel's null-options scheme.
//
// `price` and `status` must contain one entry per lane. `pack_dispatch` is
// optional; when supplied it must have the same length. An Avx2 value means the
// lane belonged to a pack sent to the AVX2 kernel; it does NOT claim that the
// kernel avoided its internal scalar patch for that lane. Status retains the
// scalar pricer's full ErrorCode/message per lane. All spans are borrowed for the
// duration of the call and may not alias input storage. The valid path allocates
// nothing.
struct ResolvedAmericanPriceBatchRequest {
  double S{0.0};
  double T{0.0};
  double r{0.0};
  double q{0.0};
  std::span<const double> K;
  std::span<const double> sigma;
  std::span<const Side> side;
  AmericanMethod method{AmericanMethod::AndersenLake};
  std::optional<AlOpts> al_opts{std::nullopt};
  simd::SimdIsa isa{simd::SimdIsa::Auto};
  std::span<double> price;
  std::span<Status> status;
  std::span<simd::SimdRoute> pack_dispatch;

  [[nodiscard]] std::size_t size() const noexcept { return K.size(); }
  [[nodiscard]] bool consistent() const noexcept {
    const std::size_t n = size();
    return sigma.size() == n && side.size() == n && price.size() == n &&
           status.size() == n &&
           (pack_dispatch.empty() || pack_dispatch.size() == n);
  }
};

// ── Price batch output (owning; sized once, reused across snapshots) ───────
struct PriceBatchOutput {
  std::vector<double> price;             // per-lane American mark (NaN if Unsupported)
  std::vector<LaneStatus> status;        // per-lane outcome
  // Dispatch of the containing pack. Avx2 does not prove that the opaque AVX2
  // kernel avoided its internal scalar patch for this specific lane.
  std::vector<simd::SimdRoute> route;

  void resize(std::size_t n) {
    price.assign(n, 0.0);
    status.assign(n, LaneStatus::Ok);
    route.assign(n, simd::SimdRoute::Scalar);
  }
  [[nodiscard]] std::size_t size() const noexcept { return price.size(); }

  // Fraction of lanes that were not submitted in an AVX2-dispatched pack. This
  // is a dispatch statistic, not a count of low-level per-lane scalar patches.
  [[nodiscard]] double scalar_dispatch_rate() const noexcept {
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

  // Compatibility spelling retained for callers of the original API. The
  // semantics are `scalar_dispatch_rate()`, not exact scalar execution share.
  [[nodiscard]] double scalar_fallback_rate() const noexcept {
    return scalar_dispatch_rate();
  }
};

// ── Pricing kernel handle (wraps the T13 ISA seam) ────────────────────────
//
// Minimal by design: the call-local ISA selection plus an optional executor for
// cross-lane parallelism of the scalar Greek routes.
struct PricingKernel {
  // Passed directly to the boundary kernel for this call. Auto inherits the T13
  // ship gate (scalar today); ForceScalar forces scalar, while ForceAvx2 uses AVX2
  // when supported and safely falls back to scalar on other hosts.
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

// Price an exact resolved equal-T run into caller-owned output spans. BAW remains
// scalar; Andersen-Lake passes the request's exact option engagement through the
// configured boundary kernel and every scalar patch. `isa` remains a call-local
// request; `pack_dispatch` reports only whether a containing pack was sent to
// AVX2, not the low-level kernel's opaque per-lane patch decision.
// Returns InvalidArgument for request span-shape errors or any overlap between
// input/output spans or between nonempty output spans. Validation completes
// before counters or writes. Model failures are retained independently in
// `status[i]` with price[i] = NaN.
[[nodiscard]] Status american_price_batch_resolved(
    const ResolvedAmericanPriceBatchRequest& request);

// American Greeks for a book into the SoA columns of `greeks` (only the fields in
// `fields` — and only non-null columns — are written). FD route (analytic_greeks=false)
// fans scalar american_greeks_fd per lane. Analytic route (analytic_greeks=true)
// dispatches PUT and CALL lanes through their K3 laned AVX2 bundles when AVX2 is
// selected (kShipAvx2Greeks — Auto on capable hosts, or ForceAvx2), matching scalar
// american_greeks_al within the documented economic gate, and patches ineligible /
// non-finite lanes to the same-side scalar oracle; ForceScalar reproduces the
// bit-identical scalar bundle. `fields` also drives the K4 first-order solve-skip
// (need_vega = Vega|Volga|Vanna, need_rho = Rho, need_charm = Charm). Per-lane
// status/route land in the workspace (ws.lane_status_view()/lane_route_view()). Returns
// InvalidArgument on a span-length mismatch.
[[nodiscard]] Status american_greeks_batch(const AmericanBatchInput& in,
                                           GreekFieldMask fields,
                                           simd::GreeksBatchSoA& greeks,
                                           PricingKernel& kernel,
                                           PricingWorkspace& ws);

} // namespace atx::vol
