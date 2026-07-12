#include "atx/vol/american_batch.hpp"

#include "atx/vol/american.hpp"          // andersen_lake, american_greeks_fd/al, classify_regime
#include "atx/vol/counters.hpp"          // exact resolved-route diagnostics
#include "atx/vol/pricing_executor.hpp"  // PricingExecutor
#include "atx/vol/simd/american_boundary_batch.hpp"
#include "atx/vol/simd/cpu.hpp"

#include <array>
#include <cmath>
#include <cstdint>
#include <limits>

namespace atx::vol {

using atx::core::Err;
using atx::core::Ok;

namespace {

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

template <class Input, class Output>
[[nodiscard]] bool spans_overlap(std::span<Input> input,
                                 std::span<Output> output) noexcept {
  if (input.empty() || output.empty()) {
    return false;
  }
  // SAFETY: repository toolchains preserve object addresses in uintptr_t.
  // Integer interval comparison avoids undefined relational comparison between
  // pointers into unrelated arrays.
  const std::uintptr_t input_begin =
      reinterpret_cast<std::uintptr_t>(input.data());
  const std::uintptr_t output_begin =
      reinterpret_cast<std::uintptr_t>(output.data());
  if (input_begin <= output_begin) {
    return (output_begin - input_begin) < input.size_bytes();
  }
  return (input_begin - output_begin) < output.size_bytes();
}

// Mirror andersen_lake_core's degenerate + regime classification EXACTLY so the
// batch's "kernel vs scalar-patch" split is bit-consistent with a per-contract
// solve. Returns true iff this lane is a genuine single-boundary American solve
// (the only lanes the boundary kernel prices); everything else (invalid /
// degenerate / European / double-continuation) is patched to scalar andersen_lake.
[[nodiscard]] bool is_kernel_lane(double S, double K, double T, double sigma,
                                  double r, double q, Side side) noexcept {
  if (!(S > 0.0) || !(K > 0.0) || !(T >= 0.0) || !(sigma >= 0.0) ||
      !std::isfinite(r) || !std::isfinite(q)) {
    return false; // InvalidArgument corner -> scalar (NaN)
  }
  if (T <= 1.0e-12 || sigma <= 1.0e-8) {
    return false; // degenerate -> scalar (intrinsic)
  }
  const double rate = (side == Side::Put) ? r : q;
  const double yield = (side == Side::Put) ? q : r;
  return detail::classify_regime(rate, yield) ==
         detail::ExerciseRegime::American;
}

// Write the requested, non-null Greek columns of `out` at index i from `g`.
void write_masked(const simd::GreeksBatchSoA& out, std::size_t i,
                  const AmericanGreeks& g, GreekFieldMask fields) noexcept {
  if (out.delta && has_field(fields, GreekFieldMask::Delta)) out.delta[i] = g.delta;
  if (out.gamma && has_field(fields, GreekFieldMask::Gamma)) out.gamma[i] = g.gamma;
  if (out.vega && has_field(fields, GreekFieldMask::Vega)) out.vega[i] = g.vega;
  if (out.theta && has_field(fields, GreekFieldMask::Theta)) out.theta[i] = g.theta;
  if (out.rho && has_field(fields, GreekFieldMask::Rho)) out.rho[i] = g.rho;
  if (out.vanna && has_field(fields, GreekFieldMask::Vanna)) out.vanna[i] = g.vanna;
  if (out.volga && has_field(fields, GreekFieldMask::Volga)) out.volga[i] = g.volga;
  if (out.charm && has_field(fields, GreekFieldMask::Charm)) out.charm[i] = g.charm;
  if (out.price && has_field(fields, GreekFieldMask::Price)) out.price[i] = g.price;
}

} // namespace

Status american_price_batch(const AmericanBatchInput& in, PriceBatchOutput& out,
                            PricingKernel& kernel, PricingWorkspace& ws) {
  if (!in.consistent()) {
    return Err(ErrorCode::InvalidArgument,
               "american_price_batch: input span length mismatch");
  }
  const std::size_t n = in.size();
  out.resize(n);
  if (n == 0) {
    return Ok();
  }

  ws.reserve_lanes(n);

  // Pass 1: classify + compact. Genuine American lanes -> internal-put pack;
  // everything else patches to scalar andersen_lake in public order.
  std::size_t m = 0;
  for (std::size_t i = 0; i < n; ++i) {
    const Side side = in.side[i];
    const double S = in.S[i], K = in.K[i], T = in.T[i], sg = in.sigma[i],
                 r = in.r[i], q = in.q[i];
    if (is_kernel_lane(S, K, T, sg, r, q, side)) {
      // Internal-put coordinates. Put: as-is. Call: McDonald-Schroder swap
      // (S<->K, r<->q) — bit-identical to andersen_lake's own call path.
      if (side == Side::Put) {
        ws.ps_S[m] = S;
        ws.ps_K[m] = K;
        ws.ps_r[m] = r;
        ws.ps_q[m] = q;
      } else {
        ws.ps_S[m] = K;
        ws.ps_K[m] = S;
        ws.ps_r[m] = q;
        ws.ps_q[m] = r;
      }
      ws.ps_T[m] = T;
      ws.ps_sigma[m] = sg;
      ws.perm[m] = static_cast<std::uint32_t>(i);
      ++m;
    } else {
      const Result<double> px = andersen_lake(S, K, T, sg, r, q, side);
      const double v = px.has_value() ? *px : kNaN;
      out.price[i] = v;
      out.route[i] = simd::SimdRoute::Scalar;
      out.status[i] = std::isfinite(v) ? LaneStatus::Ok : LaneStatus::Unsupported;
    }
  }

  // Pass 2: dispatch the homogeneous internal-put pack through T13's REAL kernel.
  if (m > 0) {
    const simd::SimdRoute pack_route = simd::american_put_boundary_batch(
        ws.ps_S.data(), ws.ps_K.data(), ws.ps_T.data(), ws.ps_sigma.data(),
        ws.ps_r.data(), ws.ps_q.data(), ws.ps_price.data(), m, kernel.isa);
    for (std::size_t j = 0; j < m; ++j) {
      const std::uint32_t i = ws.perm[j];
      const double v = ws.ps_price[j];
      out.price[i] = v;
      out.route[i] = pack_route;
      out.status[i] =
          std::isfinite(v) ? LaneStatus::Ok : LaneStatus::Unsupported;
    }
  }

  return Ok();
}

Status american_price_batch_resolved(
    const ResolvedAmericanPriceBatchRequest& request) {
  if (!request.consistent()) {
    return Err(ErrorCode::InvalidArgument,
               "american_price_batch_resolved: input/output span length mismatch");
  }
  const auto overlaps_input = [&](const auto output) noexcept {
    return spans_overlap(request.K, output) ||
           spans_overlap(request.sigma, output) ||
           spans_overlap(request.side, output);
  };
  const bool outputs_overlap =
      spans_overlap(request.price, request.status) ||
      spans_overlap(request.price, request.pack_dispatch) ||
      spans_overlap(request.status, request.pack_dispatch);
  if (overlaps_input(request.price) || overlaps_input(request.status) ||
      overlaps_input(request.pack_dispatch) || outputs_overlap) {
    return Err(ErrorCode::InvalidArgument,
               "american_price_batch_resolved: input/output spans overlap");
  }

  const std::size_t n = request.size();
  ATX_VOL_COUNT(ResolvedPriceWrapperCalls);
  ATX_VOL_COUNT_N(ResolvedPriceWrapperLanes, n);

  const auto scalar_lane = [&](std::size_t i) {
    ATX_VOL_COUNT(AmericanWrapperKnownScalarLanes);
    const Result<double> result =
        american_price(request.S, request.K[i], request.T, request.sigma[i], request.r, request.q,
                       request.side[i], request.method, request.al_opts);
    if (result.has_value()) {
      request.price[i] = *result;
      request.status[i] = Ok();
    } else {
      request.price[i] = kNaN;
      request.status[i] = Err(result.error());
    }
    if (!request.pack_dispatch.empty()) {
      request.pack_dispatch[i] = simd::SimdRoute::Scalar;
    }
  };

  // Exactness gate: the existing SIMD boundary kernel is specifically the
  // null-options Andersen-Lake scheme. Resolved surfaces carry an engaged preset,
  // and BAW is a different model, so both stay on their exact scalar references.
  if (request.method != AmericanMethod::AndersenLake || request.al_opts.has_value() ||
      request.isa == simd::SimdIsa::ForceScalar) {
    for (std::size_t i = 0; i < n; ++i) {
      scalar_lane(i);
    }
    return Ok();
  }

  // The null-options AL route can use the existing exact-compatible kernel.
  // Compact only genuine single-boundary lanes into complete AVX-width packs;
  // irregular lanes and the tail retain their scalar Error and bit identity.
  std::array<double, 4> pack_S{}, pack_K{}, pack_T{}, pack_sigma{}, pack_r{}, pack_q{}, pack_price{};
  std::array<std::size_t, 4> pack_index{};
  std::size_t packed = 0;
  for (std::size_t i = 0; i < n; ++i) {
    const Side side = request.side[i];
    if (!is_kernel_lane(request.S, request.K[i], request.T, request.sigma[i], request.r, request.q,
                        side)) {
      scalar_lane(i);
      continue;
    }
    if (side == Side::Put) {
      pack_S[packed] = request.S;
      pack_K[packed] = request.K[i];
      pack_r[packed] = request.r;
      pack_q[packed] = request.q;
    } else {
      pack_S[packed] = request.K[i];
      pack_K[packed] = request.S;
      pack_r[packed] = request.q;
      pack_q[packed] = request.r;
    }
    pack_T[packed] = request.T;
    pack_sigma[packed] = request.sigma[i];
    pack_index[packed] = i;
    ++packed;
    if (packed != pack_S.size()) {
      continue;
    }

    const simd::SimdRoute pack_route = simd::american_put_boundary_batch(
        pack_S.data(), pack_K.data(), pack_T.data(), pack_sigma.data(), pack_r.data(),
        pack_q.data(), pack_price.data(), packed, request.isa);
    if (pack_route == simd::SimdRoute::Avx2) {
      ATX_VOL_COUNT(AmericanAvxPackDispatches);
    }
    for (std::size_t j = 0; j < packed; ++j) {
      const std::size_t original = pack_index[j];
      if (!std::isfinite(pack_price[j])) {
        scalar_lane(original);
        continue;
      }
      if (pack_route == simd::SimdRoute::Scalar) {
        ATX_VOL_COUNT(AmericanWrapperKnownScalarLanes);
      }
      request.price[original] = pack_price[j];
      request.status[original] = Ok();
      if (!request.pack_dispatch.empty()) {
        request.pack_dispatch[original] = pack_route;
      }
    }
    packed = 0;
  }
  for (std::size_t j = 0; j < packed; ++j) {
    scalar_lane(pack_index[j]);
  }
  return Ok();
}

Status american_greeks_batch(const AmericanBatchInput& in, GreekFieldMask fields,
                             simd::GreeksBatchSoA& greeks, PricingKernel& kernel,
                             PricingWorkspace& ws) {
  if (!in.consistent()) {
    return Err(ErrorCode::InvalidArgument,
               "american_greeks_batch: input span length mismatch");
  }
  const std::size_t n = in.size();
  ws.reserve_lanes(n);
  if (n == 0) {
    return Ok();
  }

  // Each lane's Greeks come from the EXISTING scalar T9 route (no vectorized Greek
  // stencil — see the header's honest-scope note). Grouping by (side, scheme) is
  // implicit: the FD/AL routes internally reuse the spot-independent base boundary
  // across their bumped solves. Lanes are independent, so an optional executor
  // fans them across the pool with disjoint per-lane writes (bit-identical).
  const bool analytic = kernel.analytic_greeks;
  const auto price_lane = [&](std::size_t i) noexcept {
    const Side side = in.side[i];
    const Result<AmericanGreeks> g =
        analytic ? american_greeks_al(in.S[i], in.K[i], in.T[i], in.sigma[i],
                                      in.r[i], in.q[i], side)
                 : american_greeks_fd(in.S[i], in.K[i], in.T[i], in.sigma[i],
                                      in.r[i], in.q[i], side);
    if (g.has_value()) {
      write_masked(greeks, i, *g, fields);
      ws.lane_status[i] = LaneStatus::Ok;
    } else {
      AmericanGreeks nan_g;
      nan_g.delta = nan_g.gamma = nan_g.vega = nan_g.theta = nan_g.rho =
          nan_g.vanna = nan_g.volga = nan_g.charm = nan_g.price = kNaN;
      write_masked(greeks, i, nan_g, fields);
      ws.lane_status[i] = LaneStatus::Unsupported;
    }
    ws.lane_route[i] = simd::SimdRoute::Scalar; // scalar Greek stencil (honest)
  };

  if (kernel.executor != nullptr) {
    kernel.executor->run_blocks(n, /*n_threads=*/0,
                                [&](std::size_t i) { price_lane(i); });
  } else {
    for (std::size_t i = 0; i < n; ++i) {
      price_lane(i);
    }
  }

  return Ok();
}

} // namespace atx::vol
