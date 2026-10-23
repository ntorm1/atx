#pragma once

// Internal seams of the serving record (lanes H0-A and H1, r6 2026-10-23). Not public API.
//
// 1. Compile-time seams. Lane A0's (sigma, q) family Greek bundle (american.hpp
//    family_greeks on AlSigmaQFamily) and lane K3's family error envelope
//    (pricing/american_family_error.hpp). With a seam at 0 the record serves without that
//    lane: the family tier's Greeks other than vega are BundleUnavailable, and every
//    family is Uncovered (no validated statement; ladders can only be Sampled). Both lanes
//    are in the root since W2c-h, so integration W2c-i set both seams to 1.
// 2. The phase-B hand-off (lane H1): what certify_and_serve leaves in the fit for the
//    record builder (QuoteSliceFit::serve_handoff), and the builder over a supplied holder.
// 3. The evaluate_quote_slice fast path (lane H1): a between-node or moved-node volatility
//    from the record's family where K3's enclosure accepts it.

#include <array>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>

#include "atx/vol/api/core/types.hpp"
#include "atx/vol/api/fitting/quote_slice.hpp"
#include "atx/vol/api/fitting/slice_serve_record.hpp"
#include "fitting/deam_iv_cache.hpp"          // DeAmSigmaFamilies
#include "pricing/american_family_error.hpp" // ValidatedErrorEnvelope

#ifndef ATX_VOL_SERVE_RECORD_A0
#define ATX_VOL_SERVE_RECORD_A0 1
#endif
#ifndef ATX_VOL_SERVE_RECORD_K3
#define ATX_VOL_SERVE_RECORD_K3 1
#endif

namespace atx::vol {

// The fitter's (sigma, q) families as certify_and_serve saw them (lane H1, phase B). The
// fit's chain cache owns the families and its holder is not reachable with shared
// ownership from certify_and_serve (VolSpaceCache::families() returns a raw pointer, and
// the cache dies with the fit's call frame or its memo), so the hand-off carries what
// rebuilds them EXACTLY: each side's geometry inputs and family box (the fitter's
// build_sigma_q recipe), the fitter's audited K3 envelope and grid bound, and two of the
// fitter family's own prices at fixed points. The serve_build stage re-prepares the
// families from it (off the fit clock) and keeps a side only when the rebuilt family
// reproduces both prices and the envelope bit for bit. Immutable once stamped.
struct QuoteSliceServeHandoff {
  struct Probe {
    double K = 0.0;
    double sigma = 0.0;
    double q = 0.0;
    double price = std::numeric_limits<double>::quiet_NaN();
  };
  struct Family {
    bool present = false;
    double geometry_T = 0.0; // AlExpiryGeometry::T() / r() / q() of the side's geometry
    double geometry_r = 0.0;
    double geometry_q = 0.0;
    double anchor = 0.0;
    double sigma_lo = 0.0;
    double sigma_hi = 0.0;
    std::uint16_t n_sigma = 0;
    double q_lo = 0.0;
    double q_hi = 0.0;
    std::uint16_t n_q = 0;
    ValidatedErrorEnvelope envelope{}; // `family` cleared: identity is checked by value
    double q_bound = std::numeric_limits<double>::infinity();
    std::array<Probe, 2> probes{};
  };
  std::array<Family, 2> sides{}; // [0] put, [1] call
};

namespace detail {

// The hook at the end of certify_and_serve (lane H1): stamps out.serve_handoff from the
// families the certification read (nullptr: none - the default path - or a fit that did
// not certify, which clears any earlier hand-off). Two family prices per present side; no
// solve, no change to any served value.
void stamp_serve_handoff(QuoteSliceFit &out, const DeAmSigmaFamilies *families);

// The side families a serving record builds for `fit` under `policy` with the record's
// own recipe (kDeAmQFamilySigmaNodes x kDeAmQFamilyQNodes / NarrowQNodes nodes on the
// LogSigma axis over the box of the side's served sigmas), each certified through lane
// K3's deam_family_certify exactly as the fitter's. nullptr when no side can be built
// (DatedCash, T below kAlSigmaFamilyMinTau, every side refused).
[[nodiscard]] std::shared_ptr<const DeAmSigmaFamilies>
build_serve_families(const QuoteSliceFit &fit, const ServeRecordPolicy &policy);

// build_serve_record with `families` held instead of obtained (nullptr: obtained as by
// build_serve_record). InvalidArgument when a present side's family was not built for
// this fit's expiry (its geometry's T and r, or its anchor, differ from the fit's bit for
// bit: review-H0A m7). The holder's K3 envelope is read as it is (a holder without one is
// Uncovered); a side whose box does not cover a contract refuses that contract on the
// family tier.
[[nodiscard]] Result<std::shared_ptr<const SliceServeRecord>>
build_serve_record_with(const QuoteSliceFit &fit, const ServeRecordPolicy &policy,
                        std::shared_ptr<const DeAmSigmaFamilies> families);

// The evaluate_quote_slice fast path (lane H1): when fit.serve_record was built from this
// fit (spot, clock, rate and the bracketing node prices bit-equal) and (strike, side) is
// served between nodes or at a moved node, the certified slice price with the volatility
// of the record's family inversion of THAT price - only where K3's enclosure accepts the
// root (validated, within iv_tolerance) clear of the floor and outside the L1c band.
// nullopt everywhere else: the caller then takes query_quote_slice's path unchanged.
// Allocation-free.
[[nodiscard]] std::optional<QuoteSliceValue> serve_record_value(const QuoteSliceFit &fit,
                                                                double strike, Side side);

} // namespace detail
} // namespace atx::vol
