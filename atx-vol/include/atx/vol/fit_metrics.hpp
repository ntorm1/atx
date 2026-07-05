#pragma once

// Volatility fit-quality metrics — per-quote error bars from bid-ask spreads,
// reduced chi-square, "minimum edge" flags, and average fit error, following
// the Vola Dynamics calibration philosophy: fit implied vols WITH error bars,
// the primary goodness-of-fit number is the reduced chi-square, and a model vol
// that lands inside the bid-ask-derived error band carries no statistical edge.
//
// ## Relationship to FitDiag (calib.hpp)
//
// These metrics COMPLEMENT — they do not replace — the per-slice `FitDiag`
// (calib.hpp: rmse_vol_vega_weighted, max_residual_vol, outer_iters, ...) that
// the LM fitter already fills. The bundle here is a *separate, standalone*
// struct precisely so it can be threaded into the calibrators later without
// editing calib.hpp. It deliberately reuses calib's vocabulary: the error-bar
// weight 1/σ_err² is the same vega²/spread² weight family the LM already uses
// (σ_err ≈ ½·spread/vega, so 1/σ_err² ≈ vega²/(½·spread)²).
//
// ## Concepts (Klassen / Vola 2017; Wilmott 2020)
//
//   - Per-observation error bar: a price half-spread maps to a vol uncertainty
//     through vega,  σ_err ≈ ½·(ask − bid) / vega  — floored and capped so a
//     vanishing vega or a zero/crossed quote cannot produce a div-by-zero or an
//     absurdly tight bar.
//   - Reduced chi-square:  χ²_red = Σ_i ((σ_model_i − σ_mkt_i)/σ_err_i)² /
//     (N − dof), with dof = number of fitted curve parameters. Vola's primary
//     fit metric (their examples: SVI 6.458, C8 0.599, C12 0.021 on one snap).
//   - Minimum edge: compare |σ_model − σ_mkt| to k·σ_err. When the market (mid)
//     vol sits inside [σ_model − k·σ_err, σ_model + k·σ_err] the difference is
//     within one error bar and there is "no statistical edge".
//   - avE5: an average absolute fit error scaled by 1e5 (Vola's "avE5"/"e5Av").
//
// ## PORT NOTE — avE5 unit ambiguity
//
// The Vola slide labels the average-fit-error metric "avE5"/"e5Av" but does not
// state whether the residual it averages is in VOL points or PRICE units.
// `avg_abs_error_e5` is therefore intentionally residual-agnostic: pass it vol
// residuals for the vol-space avE5 (what `SliceFitMetrics::avE5_vol` reports),
// or pass it price residuals for the price-space variant. Both are supported
// through the one entry point; the caller chooses the residual basis.
//
// ## Thread-safety
//
// Every entry is a pure function of its arguments (spans are read-only, no
// shared state) — safe to call concurrently from any number of threads.

#include <cstddef>
#include <span>

#include "atx/vol/types.hpp"  // Result, ErrorCode

namespace atx::vol {

// Per-quote vol error bar from a price bid-ask spread and vega.
//
//   σ_err = clamp( ½·(ask − bid) / vega, min_bar, max_bar )
//
// A non-positive or non-finite vega (a price spread carrying no vol
// sensitivity) maps to the widest bar `max_bar`; a zero or crossed spread
// floors to `min_bar`.
//
// @param bid_price  quote bid (price units)
// @param ask_price  quote ask (price units); ask ≥ bid for a normal quote
// @param vega       Black-76 vega at the quote's IV (∂price/∂σ); > 0 expected
// @param min_bar    floor on the returned bar (default 1e-4 vol pts)
// @param max_bar    cap on the returned bar (default 5.0 vol pts)
// @return           vol-space 1σ error bar, always in [min_bar, max_bar]
[[nodiscard]] double vol_error_bar(double bid_price, double ask_price,
                                   double vega, double min_bar = 1e-4,
                                   double max_bar = 5.0) noexcept;

// Reduced chi-square inputs/outputs. All residuals and error bars are in VOL
// space; `dof` is the number of fitted curve parameters.
struct ChiSquareResult {
  double chi2;          // raw Σ ((resid_i / err_bar_i)²) standardized residuals
  double chi2_reduced;  // chi2 / (N − dof)
  std::size_t n;        // observations used
  std::size_t dof;      // fitted parameters
};

// Reduced chi-square of a residual vector standardized by its per-observation
// error bars. Residuals and error bars are paired element-wise and must be in
// the same (vol) space.
//
// @param resid_vol    per-obs residual σ_model − σ_mkt (vol pts)
// @param err_bar_vol  per-obs 1σ error bar (vol pts); each entry must be > 0
// @param dof          number of fitted curve parameters
// @return  InvalidArgument if the two spans differ in length, if `resid_vol` is
//          empty, if N ≤ dof (no positive denominator), or if any error bar is
//          ≤ 0 / non-finite; otherwise Ok with chi2 and the reduced value.
[[nodiscard]] atx::core::Result<ChiSquareResult>
reduced_chi_square(std::span<const double> resid_vol,
                   std::span<const double> err_bar_vol,
                   std::size_t dof) noexcept;

// "Minimum edge" verdict for one quote. `within_band` true ⇒ the model vol is
// inside the market's ±k·σ_err band, i.e. NO statistical edge.
struct EdgeResult {
  bool within_band;  // |σ_model − σ_mkt| < k·σ_err  (no statistical edge)
  double edge_vol;   // signed σ_model − σ_mkt (vol pts)
  double n_sigma;    // signed edge in error-bar units: edge_vol / σ_err
};

// Minimum-edge test for a single quote.
//
// @param iv_model     model implied vol
// @param iv_mkt       market (mid) implied vol
// @param err_bar_vol  1σ vol error bar for this quote
// @param k            band half-width in error-bar units (default 1.0 = 1σ)
// @return  within_band / signed edge (model − market) / signed n_sigma.
[[nodiscard]] EdgeResult minimum_edge(double iv_model, double iv_mkt,
                                      double err_bar_vol, double k = 1.0) noexcept;

// Average absolute fit error scaled by 1e5 (Vola "avE5"): mean(|resid|)·1e5.
// Residual-basis-agnostic — pass vol residuals for avE5_vol or price residuals
// for the price-space variant (see the avE5 PORT NOTE above). An empty span
// yields 0.
[[nodiscard]] double avg_abs_error_e5(std::span<const double> resid) noexcept;

// Full fit-quality bundle over one slice. Mirrors calib's vocabulary; the
// weighted RMSE and reduced chi-square share the error-bar weight w = 1/σ_err².
struct SliceFitMetrics {
  double rmse_vol;           // sqrt( Σ r² / N ), r = σ_model − σ_mkt (vol pts)
  double rmse_vol_weighted;  // sqrt( Σ w·r² / Σ w ), w = 1/σ_err²  (vol pts)
  double chi2_reduced;       // Σ (r/σ_err)² / (N − dof)
  double avE5_vol;           // mean(|r|)·1e5
  std::size_t n;             // observations used
  std::size_t n_within_band; // count with |r| < σ_err (inside the 1σ band)
};

// End-to-end slice metrics. Error bars are derived per quote from its bid/ask
// and vega via `vol_error_bar` (default floor/cap), so every divisor is
// strictly positive. All five input spans are paired element-wise.
//
// @param iv_model   per-obs model implied vol
// @param iv_mkt     per-obs market (mid) implied vol
// @param bid_price  per-obs quote bid (price units)
// @param ask_price  per-obs quote ask (price units)
// @param vega       per-obs Black-76 vega at the quote's IV
// @param dof        number of fitted curve parameters
// @return  InvalidArgument if the spans differ in length, are empty, or N ≤ dof;
//          otherwise Ok with the populated bundle.
[[nodiscard]] atx::core::Result<SliceFitMetrics>
slice_fit_metrics(std::span<const double> iv_model,
                  std::span<const double> iv_mkt,
                  std::span<const double> bid_price,
                  std::span<const double> ask_price,
                  std::span<const double> vega, std::size_t dof) noexcept;

}  // namespace atx::vol
