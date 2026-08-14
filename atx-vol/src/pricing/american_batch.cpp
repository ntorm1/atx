#include "atx/vol/api/pricing/american_batch.hpp"

#include "backtest/laned_greek_run.hpp"           // FIX-5/I3: detail::{normalize_unrequested_greeks,
                                         // requested_greeks_finite} — the SINGLE Ok-stamp
                                         // definition FIX-1/2/3 established
#include "atx/vol/api/pricing/american.hpp"          // andersen_lake, american_greeks_fd/al, classify_regime
#include "fitting/counters.hpp"          // exact resolved-route diagnostics
#include "pricing/pricing_executor.hpp"  // PricingExecutor
#include "atx/vol/api/simd/american_boundary_batch.hpp"
#include "atx/vol/api/simd/cpu.hpp"

#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <new> // std::bad_alloc (per-lane noexcept containment)

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

Result<std::size_t> american_price_batch(const AmericanBatchInput& in, PriceBatchOutput& out,
                                         PricingKernel& kernel, PricingWorkspace& ws) {
  if (!in.consistent()) {
    return Err(ErrorCode::InvalidArgument,
               "american_price_batch: input span length mismatch");
  }
  const std::size_t n = in.size();
  out.resize(n);
  if (n == 0) {
    return Ok(n);
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
    // This is the SECOND AVX2 dispatch site in this TU, and until REVWSA finding 6
    // it was the uncounted one: `AmericanAvxPackDispatches` was bumped only inside
    // american_price_batch_resolved, so complete packs dispatched from HERE were
    // invisible and a reading of 0 meant "not observed on the entries that count",
    // never "the pack path is dead". The AVX2 driver runs floor(m/4) complete 4-lane
    // packs and prices the m % 4 tail scalar (american_boundary_avx2.cpp:53,:104), so
    // `m / 4` is exactly the quantity the resolved entry accumulates one pack at a
    // time. See the counter's definition in counters.hpp for what it still excludes.
    if (pack_route == simd::SimdRoute::Avx2) {
      ATX_VOL_COUNT_N(AmericanAvxPackDispatches, m / 4);
    }
    for (std::size_t j = 0; j < m; ++j) {
      const std::uint32_t i = ws.perm[j];
      const double v = ws.ps_price[j];
      out.price[i] = v;
      out.route[i] = pack_route;
      out.status[i] =
          std::isfinite(v) ? LaneStatus::Ok : LaneStatus::Unsupported;
    }
  }

  return Ok(n);
}

Result<std::size_t> american_price_batch_resolved(
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

  // BAW is a different model and stays on its exact scalar reference. The AL
  // kernel consumes the same optional unchanged, so null and engaged schemes
  // share this route without changing their option semantics.
  if (request.method != AmericanMethod::AndersenLake ||
      request.isa == simd::SimdIsa::ForceScalar) {
    for (std::size_t i = 0; i < n; ++i) {
      scalar_lane(i);
    }
    return Ok(n);
  }

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
        pack_q.data(), pack_price.data(), packed, request.al_opts, request.isa);
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
  return Ok(n);
}

Result<std::size_t> american_greeks_batch(const AmericanBatchInput& in, GreekFieldMask fields,
                                          simd::GreeksBatchSoA& greeks, PricingKernel& kernel,
                                          PricingWorkspace& ws) {
  if (!in.consistent()) {
    return Err(ErrorCode::InvalidArgument,
               "american_greeks_batch: input span length mismatch");
  }
  const std::size_t n = in.size();
  ws.reserve_lanes(n);
  if (n == 0) {
    return Ok(n);
  }

  // Each lane's Greeks come from the EXISTING scalar T9 route (no vectorized Greek
  // stencil — see the header's honest-scope note). Grouping by (side, scheme) is
  // implicit: the FD/AL routes internally reuse the spot-independent base boundary
  // across their bumped solves. Lanes are independent, so an optional executor
  // fans them across the pool with disjoint per-lane writes (bit-identical).
  const bool analytic = kernel.analytic_greeks;
  // K4 first-order tier: the column mask -> the analytic path's solve-skip selectors, so
  // a hedge ({delta}) bundle does 1 boundary solve instead of 5 ON THE SCALAR PRODUCTION
  // ROUTE (american_greeks_al), independent of the dark AVX2 gate. FD route is unaffected
  // (american_greeks_fd is the full reference; the analytic route is what production
  // uses under analytic_greeks=true).
  const bool need_vega = has_field(fields, GreekFieldMask::Vega) ||
                         has_field(fields, GreekFieldMask::Volga) ||
                         has_field(fields, GreekFieldMask::Vanna);
  const bool need_rho = has_field(fields, GreekFieldMask::Rho);
  const bool need_charm = has_field(fields, GreekFieldMask::Charm);
  // FIX-5/I3: the REQUESTED Greek set, in the vocabulary the shared Ok-stamp speaks.
  // Same derivation the K4 solve-skip selectors above use, so the columns that were
  // asked for are exactly the columns that are swept.
  const GreekNeeds needs{need_vega, need_rho, need_charm};
  // SAFETY: the lane keeps its `noexcept` — it runs on PricingExecutor worker
  // threads below, where an escaping exception is unrecoverable no matter what
  // this lambda claims — and the claim is made TRUE by containing the throw. The
  // Greek routes are NOT noexcept: american_greeks_al/fd construct the ~46 KB
  // Andersen-Lake pricer state through make_unique and build Error message
  // strings above any SSO buffer, so memory pressure raises std::bad_alloc. A
  // lane that cannot be solved is exactly what the batch's per-lane failure
  // vocabulary already expresses, so route it into the existing not-ok branch
  // (NaN Greeks + LaneStatus::Unsupported) rather than killing the process. Only
  // bad_alloc is caught: any other exception means a contract violation deeper
  // in the call tree and should stay loud.
  const auto price_lane = [&](std::size_t i) noexcept {
    const Side side = in.side[i];
    const Result<AmericanGreeks> g = [&]() -> Result<AmericanGreeks> {
      try {
        return analytic ? american_greeks_al(in.S[i], in.K[i], in.T[i], in.sigma[i],
                                             in.r[i], in.q[i], side, std::nullopt, need_vega,
                                             need_rho, need_charm)
                        : american_greeks_fd(in.S[i], in.K[i], in.T[i], in.sigma[i],
                                             in.r[i], in.q[i], side);
      } catch (const std::bad_alloc &) {
        return Err(ErrorCode::Internal);
      }
    }();
    // FIX-5/I3: `has_value()` alone certified a bundle with a finite mark and a NaN
    // in a REQUESTED column as Ok — the pre-FIX-1 predicate, on the driver WS-Y made
    // public Python API (bindings/pricing.cpp, GreekFieldMask::All). Route both this
    // stamp and the laned one below through the SINGLE shared pair so they cannot
    // disagree by side: normalize the unrequested slots (FIX-1/F3's load-bearing
    // half), then sweep the requested set finite.
    if (g.has_value()) {
      AmericanGreeks gg = *g;
      detail::normalize_unrequested_greeks(gg, needs);
      if (detail::requested_greeks_finite(gg, needs)) {
        write_masked(greeks, i, gg, fields);
        ws.lane_status[i] = LaneStatus::Ok;
      } else {
        // Computed columns are left in place for diagnosis (exactly as
        // laned_greek_run's scatter does); consumers gate on status.
        write_masked(greeks, i, gg, fields);
        ws.lane_status[i] = LaneStatus::Unsupported;
      }
    } else {
      AmericanGreeks nan_g;
      nan_g.delta = nan_g.gamma = nan_g.vega = nan_g.theta = nan_g.rho =
          nan_g.vanna = nan_g.volga = nan_g.charm = nan_g.price = kNaN;
      write_masked(greeks, i, nan_g, fields);
      ws.lane_status[i] = LaneStatus::Unsupported;
    }
    ws.lane_route[i] = simd::SimdRoute::Scalar; // scalar Greek stencil (honest)
  };

  // K3 laned fast path: taken when the AVX2 Greeks route is selected for this isa
  // (avx2_greeks_selected: Auto => AVX2 on an AVX2 host, since kShipAvx2Greeks is now
  // true; ForceScalar => never). It is NOT dark at the flag level any more, AND the
  // surface caller is no longer holding it back either: priced_surface.cpp:1111-1134
  // (and priced_surface_view.cpp:968-975) now gate on
  // detail::laned_greek_route_selected -> simd::avx2_greeks_selected, NOT on the old
  // resolved_price_isa == ForceAvx2. So under production Auto on an AVX2 host this
  // path is LIVE end to end. (This comment used to say the surface caller "still
  // gates its greeks route on ForceAvx2 for seed-bit-identity (WS-G G4)"; that clause
  // was stale from the P1a gate flip and was corrected by the BENCH G4 pass.)
  // Both sides go through their homogeneous laned bundle. Each solves the needed
  // boundaries 4-wide per pack and patches any non-early-exercise / non-finite
  // lane through the exact same-side scalar american_greeks_al oracle. The two
  // passes retain only one chunk of scratch at a time and scatter by original
  // index, preserving public input order for mixed books.
  if (analytic && simd::avx2_greeks_selected(kernel.isa)) {
    // Chunked gather of one side -> laned dispatch -> scatter (allocation-free).
    constexpr std::size_t kC = 256;
    const auto run_side = [&](Side batch_side) noexcept {
      double cs[kC], ck[kC], ct[kC], cv[kC], cr[kC], cq[kC];
      std::size_t oidx[kC];
      AmericanGreeks gbuf[kC];
      std::size_t cnt = 0;
      // need_vega/need_rho/need_charm are the same K4 mask selectors used by
      // the scalar analytic route.
      const auto flush = [&]() noexcept {
        if (cnt == 0) {
          return;
        }
        const simd::SimdRoute route =
            (batch_side == Side::Put)
                ? simd::american_put_greeks_batch(
                      cs, ck, ct, cv, cr, cq, cnt, std::nullopt, gbuf,
                      kernel.isa, need_vega, need_rho, need_charm)
                : simd::american_call_greeks_batch(
                      cs, ck, ct, cv, cr, cq, cnt, std::nullopt, gbuf,
                      kernel.isa, need_vega, need_rho, need_charm);
        for (std::size_t j = 0; j < cnt; ++j) {
          const std::size_t oi = oidx[j];
          AmericanGreeks gg = gbuf[j];
          detail::normalize_unrequested_greeks(gg, needs);
          const bool ok = detail::requested_greeks_finite(gg, needs);
          write_masked(greeks, oi, gg, fields);
          ws.lane_status[oi] =
              ok ? LaneStatus::Ok : LaneStatus::Unsupported;
          ws.lane_route[oi] = route;
        }
        cnt = 0;
      };
      for (std::size_t i = 0; i < n; ++i) {
        if (in.side[i] != batch_side) {
          continue;
        }
        cs[cnt] = in.S[i];
        ck[cnt] = in.K[i];
        ct[cnt] = in.T[i];
        cv[cnt] = in.sigma[i];
        cr[cnt] = in.r[i];
        cq[cnt] = in.q[i];
        oidx[cnt] = i;
        if (++cnt == kC) {
          flush();
        }
      }
      flush();
    };
    run_side(Side::Put);
    run_side(Side::Call);
    return Ok(n);
  }

  if (kernel.executor != nullptr) {
    kernel.executor->run_blocks(n, /*n_threads=*/0,
                                [&](std::size_t i) { price_lane(i); });
  } else {
    for (std::size_t i = 0; i < n; ++i) {
      price_lane(i);
    }
  }

  return Ok(n);
}

} // namespace atx::vol
