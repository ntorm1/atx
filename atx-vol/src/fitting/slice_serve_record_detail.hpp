#pragma once

// Internal seams of the serving record (lane H0-A, r6 2026-10-23). Not public API.
//
// 1. Compile-time seams. Lane A0's (sigma, q) family Greek bundle (american.hpp
//    family_greeks on AlSigmaQFamily) and lane K3's family error envelope
//    (pricing/american_family_error.hpp) are not in this lane's base (the W2c-f root);
//    the record compiles against them only when the integration that lands them sets the
//    seam to 1. With a seam at 0 the record serves without that lane: the family tier's
//    Greeks other than vega are BundleUnavailable, and every family root is Uncovered.
//    Verified with each lane overlaid on this branch (lane report, Evidence). Both lanes
//    are in the root since W2c-h, so integration W2c-i sets both seams to 1.
// 2. The phase-B hook: a record built around the chain cache's families (the de-Am IV
//    cache's DeAmSigmaFamilies, shared, not copied), so tier 1 costs no kernel work.

#include <memory>

#include "atx/vol/api/core/types.hpp"
#include "atx/vol/api/fitting/quote_slice.hpp"
#include "atx/vol/api/fitting/slice_serve_record.hpp"
#include "fitting/deam_iv_cache.hpp" // DeAmSigmaFamilies

#ifndef ATX_VOL_SERVE_RECORD_A0
#define ATX_VOL_SERVE_RECORD_A0 1
#endif
#ifndef ATX_VOL_SERVE_RECORD_K3
#define ATX_VOL_SERVE_RECORD_K3 1
#endif

namespace atx::vol::detail {

// The side families a serving record builds for `fit` under `policy` (the fitter's
// (sigma, q) recipe: kDeAmQFamilySigmaNodes x kDeAmQFamilyQNodes / NarrowQNodes nodes on
// the LogSigma axis, the measured bound of deam_family_probe_worst x 10). nullptr when no
// side can be built (DatedCash, T below kAlSigmaFamilyMinTau, every side refused).
[[nodiscard]] std::shared_ptr<const DeAmSigmaFamilies>
build_serve_families(const QuoteSliceFit &fit, const ServeRecordPolicy &policy);

// build_serve_record with `families` held instead of built (nullptr: built as by
// build_serve_record). The families must have been built for this fit's expiry (T, r and
// the AlOpts of the fit); a side whose box does not cover a contract refuses that
// contract on the family tier, as for built families.
[[nodiscard]] Result<std::shared_ptr<const SliceServeRecord>>
build_serve_record_with(const QuoteSliceFit &fit, const ServeRecordPolicy &policy,
                        std::shared_ptr<const DeAmSigmaFamilies> families);

} // namespace atx::vol::detail
