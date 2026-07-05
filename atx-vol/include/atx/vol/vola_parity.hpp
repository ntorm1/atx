#pragma once

// CAPSTONE — the end-to-end Vola Dynamics American-equity parity pipeline.
//
// This is the module that ties the prior waves together into one orchestrated
// run and proves the parity acceptance criteria. Given ONE expiry's American
// option `Chain`, it executes the full Vola workflow (Klassen 2017) and returns
// both the re-Americanized parity report and the fitted surface's fit quality:
//
//   1. De-Americanize      American market mids -> European-equivalent IVs, plus
//      (deamer.hpp)         an implied per-term borrow and the term forward F.
//   2. Fit an arb-free      the chosen 3-parameter curve (eSSVI backbone == the
//      curve                S3/SSVI shape, or the S3 baseline directly) to the
//                           European-equivalent IV strip.
//   3. Re-Americanize +     price the fitted model vols back through the American
//      score (parity.hpp)   pricer and measure fraction-inside-bid-ask, price /
//                           vol RMSE, and the error-bar reduced chi-square.
//   4. Fit quality          RMSE and reduced chi-square of the model vs the
//      (fit_metrics.hpp)     de-Americanized market IVs, error bars from the
//                           bid/ask spread and the Black-76 vega.
//
// The q_eff bridge (deamer.hpp / panel.hpp) is used throughout: with the term
// forward F fixed by the implied borrow, a single effective carry
// q_eff = r - ln(F / S) / T reproduces the discrete-dividend forward exactly, so
// the same scalar drives both the de-Americanization inversion and the
// re-Americanization scoring.
//
// ## Curve choice and the nested-curve story
//
// Both offered curves are 3-parameter SSVI-family shapes: atx-vol's eSSVI
// backbone with the default (symmetric-rho) calibration IS the S3/SSVI curve.
// They therefore fit a plain skew near-exactly but cannot represent an event
// W-shape with negative ATM curvature — that requires the nested C8/CStar
// families (cstar_calib.hpp), which is exactly Vola's S5 -> C8 -> C12
// chi-square-improvement story. `n_curve_params` is 3 for both curves.
//
// Stateless and pure — a single call owns only its local scratch. Safe to call
// concurrently on distinct chains from any number of threads (the cost is
// dominated by the cold American pricer, so this runs at surface-fit cadence,
// not per tick).

#include <cstddef>
#include <cstdint>
#include <vector>

#include "atx/vol/calib.hpp"     // CalibOpts
#include "atx/vol/curve.hpp"     // DividendEvent
#include "atx/vol/deamer.hpp"    // DeAmOptions
#include "atx/vol/parity.hpp"    // ParityReport
#include "atx/vol/types.hpp"     // Result, ErrorCode
#include "atx/vol/universe.hpp"  // Chain

namespace atx::vol {

// Which 3-parameter curve to fit to the de-Americanized IV strip. Both are
// SSVI-family (n_curve_params == 3); see the header note on the nested-curve
// story for why neither can capture an event W-shape.
enum class ParityCurve : std::uint8_t {
  Essvi = 0,  // eSSVI backbone (symmetric SSVI) via essvi_fit_slice
  S3 = 1,     // S3/SSVI baseline via s3_seed_from_ivs
};

// Inputs for one expiry's parity run. `cash_divs` is held by value so the call
// owns a span-friendly copy of the dividend schedule.
struct ExpiryParityInputs {
  double S{0.0};                          // spot (> 0)
  double r{0.0};                          // continuously-compounded rate (finite)
  std::vector<DividendEvent> cash_divs;   // discrete cash-dividend schedule
  std::int64_t now_ts_ns{0};              // valuation timestamp (epoch ns)
  DeAmOptions deam{};                     // borrow-implication / pricer policy
  CalibOpts calib{};                      // curve-fit policy
  ParityCurve curve{ParityCurve::Essvi};  // curve to fit
  double band_k{1.0};                     // minimum-edge band multiplier (parity)
};

// The capstone bundle: the re-Americanized parity report plus the fit's own
// diagnostics on the de-Americanized IV strip.
struct ExpiryParityReport {
  ParityReport parity{};          // from chain_parity (re-Americanized scoring)
  double implied_borrow{0.0};     // implied (or fixed) per-term borrow
  double forward{0.0};            // term forward F used for the fit / scoring
  double fit_rmse_vol{0.0};       // RMSE(model IV - de-Am market IV), vol pts
  double fit_chi2_reduced{0.0};   // reduced chi-square of the fit in vol space
  std::size_t n_used{0};          // strikes that survived to the fit
  std::size_t n_dropped{0};       // strikes skipped (bad quote / failed invert)
};

// Run the full parity pipeline for one expiry `chain`.
//
// De-Americanizes the chain (implied borrow + term forward), rebuilds the
// aligned per-strike observation set self-contained (so each surviving strike's
// index, quote, log-moneyness, and European-equivalent IV stay in lock-step),
// fits the chosen 3-parameter curve, re-Americanizes the model vols, and scores
// parity. `n_curve_params` is 3 for both curves.
//
// @param chain  one (uid, expiry) bucket — uses its T, expiry_ns, strikes, and
//               per-side bids/asks/mids.
// @param in     market/pricing context, borrow-implication + curve-fit policy.
// @return       the parity + fit bundle, or an Error:
//                 InvalidArgument — S <= 0, chain.T <= 0, non-finite r, or an
//                                   empty chain (propagated from de-Am);
//                 Unavailable     — implied borrow requested but no near-ATM
//                                   co-terminal pair yielded one (from de-Am);
//                 NotFound        — fewer than the minimum usable strikes
//                                   survived to fit;
//                 Internal        — a non-finite / non-positive term forward.
//               Any curve-fitter, chi-square, or pricer error is propagated.
[[nodiscard]] atx::core::Result<ExpiryParityReport> run_expiry_parity(
    const Chain& chain, const ExpiryParityInputs& in);

}  // namespace atx::vol
