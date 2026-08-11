#pragma once

// American-equity PARITY metrics — the Vola Dynamics acceptance layer.
//
// Given a fitted volatility surface (evaluated by the caller into a span of
// model vols) and a chain of market quotes, this module prices the fitted vols
// with the chain's exercise convention and measures how well they reproduce the
// market:
//
//   - the fraction of model fair values landing inside the bid-ask spread,
//   - RMSE of the fair value vs the mid (price space),
//   - RMSE of the model vol vs the market-implied vol (vol space),
//   - the reduced chi-square in vol space (error-bar weighted), and
//   - the "minimum edge" band statistics (how many quotes carry no
//     statistical edge, and the mean signed vol edge).
//
// These are the numbers the Vola Dynamics parity gate reads: "match rmse and
// fair value % within bid-ask for American equity options".
//
// ## Decoupling
//
// The core `chain_parity` is deliberately independent of any concrete surface
// type: the caller evaluates whatever surface it fit and hands the resulting
// model vols in as a span. A convenience overload additionally accepts a
// callable `double(double k_log, double T)` and evaluates the surface on the
// fly at each quote's log-moneyness `k_log = ln(K / F)`, `F = S·e^{(r−q)T}`.
//
// ## Vol conventions
//
// `model_iv` and `market_iv` are paired element-wise with the strike/bid/ask/
// mid/side arrays (the SAME (strike, side) selection). Both live in EUROPEAN
// (Black-76) vol space: for American chains `market_iv` is the de-Americanized,
// European-equivalent market vol; for European chains it is the raw implied
// vol. Fair values use Black-76 for European chains and re-Americanization for
// American chains. See the PORT NOTES in parity.cpp.
//
// ## Thread-safety
//
// Every entry is a pure function of its arguments (read-only spans / a
// stateless surface callable, no shared state) — safe to call concurrently
// from any number of threads.

#include <cmath>
#include <concepts>
#include <cstddef>
#include <optional>
#include <span>
#include <vector>

#include "atx/vol/american.hpp"    // AmericanMethod, AlOpts
#include "atx/vol/correction.hpp"  // AmericanCorrectionCaches (hot-path re-pricing)
#include "atx/vol/fit_metrics.hpp" // BandViolationStats
#include "atx/vol/types.hpp"       // Result, Side

namespace atx::vol {

// Pricing/market context shared by every quote in the chain.
struct ParityInputs {
  double S;     // spot
  double r;     // rate
  double q_eff; // effective carry so forward = S*exp((r-q_eff)*T)
  double T;
  ExerciseStyle exercise_style = ExerciseStyle::American;
  AmericanMethod method = AmericanMethod::AndersenLake;
  std::optional<AlOpts> al_opts = std::nullopt;
  double band_k = 1.0;            // error-bar band multiplier for minimum-edge
  std::size_t n_curve_params = 0; // dof for reduced chi-square
  // Optional per-side hot-path caches for the re-Americanization pricing.
  // Default-empty => cold Andersen-Lake (pre-cache behavior). Using the SAME
  // caches the de-Americanization used keeps the round-trip self-consistent.
  AmericanCorrectionCaches caches{};
};

// The parity acceptance bundle.
struct ParityReport {
  double frac_fv_within_bidask; // fraction of quotes whose model American fair value in [bid,ask]
  double rmse_mid_price;        // RMSE(model fair value - mid) in price
  double rmse_mid_vol;          // RMSE(model vol - market-implied vol) in vol pts
  double chi2_reduced; // from fit_metrics::reduced_chi_square (vol space, error-bar weighted)
  double
      frac_within_edge_band; // fraction with |model_vol - mkt_vol| < band_k*err_bar (no statistical edge)
  double mean_edge_vol; // mean signed edge in vol pts
  std::size_t n;        // quotes scored
  std::size_t n_within; // count within bid-ask
  // SpiderRock-style surface/quote band-violation stats for this expiry
  // (model price vs bid/ask): miss counts, worst premium violation, signed bias.
  BandViolationStats band{};

  // ── The ABSOLUTE round trip, in vol points (T5 item 3) ─────────────────
  //
  // de-Americanize -> fit -> re-Americanize -> compare to the ORIGINAL American
  // mid, expressed as the vol displacement that price gap represents:
  //
  //     round_trip_vol_i = |fair_value_i - mid_i| / vega_i
  //
  // with `vega_i` the Black-76 vega already used for this quote's error bar.
  // `rmse_round_trip_vol` is the RMS over the scored population and
  // `max_round_trip_vol` its worst quote.
  //
  // WHY IT IS NEEDED (de-Am review D6). Every other acceptance number on this
  // path is SPREAD-NORMALISED — `frac_fv_within_bidask` here, and the de-Am
  // audit's `residual_half_spreads` upstream (budget 0.25, i.e. spread/8). A
  // spread-normalised gate cannot produce a large number on a wide board BY
  // CONSTRUCTION, which is how "zero de-Am rejections on thin boards" and a
  // large silent error coexist. Thin, wide-spread boards are exactly the
  // population breadth work is trying to serve, so the metric that decides
  // whether serving them is honest must not scale with their spread.
  //
  // Distinct from `rmse_mid_vol`, which compares the model vol to the recovered
  // MARKET vol and therefore cannot see any error the de-Am inversion itself
  // introduced: it is measured against the inversion's own output. This is
  // measured against the raw quote, so it carries the whole chain — de-Am
  // inversion error, fit error, and re-Americanization error together.
  //
  // A quote whose vega is below one tick per vol point (a dead deep wing) has no
  // vol interpretation and contributes nothing: it is excluded from these two
  // statistics only, leaving the rest of the report's population untouched.
  // `n_round_trip` is how many contributed, so `n_round_trip < n` is the visible
  // signal that part of the slice carries no vol-space verdict, and
  // `n_round_trip == 0` means NOT MEASURED — never "measured as zero".
  double rmse_round_trip_vol{0.0};
  double max_round_trip_vol{0.0};
  std::size_t n_round_trip{0};
};

// Score a chain of quotes against a fitted surface.
//
// model_iv[i] and market_iv[i] correspond to the SAME (strike,side) selection as
// the strike/bid/ask/mid arrays; caller supplies parallel spans. market_iv is the
// European-equivalent market vol; model_iv is the fitted surface vol at that k.
// Fair value uses the exercise style declared in ParityInputs.
//
// Contract:
//   - All eight spans must share the same length; a mismatch -> InvalidArgument.
//   - Empty input, or no quote surviving the per-quote screen, -> InvalidArgument.
//   - Quotes with a crossed / non-positive bid-ask, a non-positive strike, or a
//     non-finite iv are skipped and excluded from `n`.
//   - chi2_reduced is delegated to reduced_chi_square with dof = n_curve_params;
//     if the surviving count does not exceed the dof, that call's error is
//     propagated (a reduced chi-square is undefined without a positive
//     denominator).
[[nodiscard]] atx::core::Result<ParityReport>
chain_parity(std::span<const double> strike, std::span<const double> bid,
             std::span<const double> ask, std::span<const double> mid, std::span<const Side> side,
             std::span<const double> model_iv, std::span<const double> market_iv,
             const ParityInputs &in) noexcept;

// Convenience overload: evaluate the fitted surface on the fly.
//
// `model_vol_at(k_log, T)` returns the fitted model vol at log-moneyness
// `k_log = ln(K / F)` (K over the forward) and year-fraction `T`; it is invoked
// once per strike to build the model-vol span, after which the span-based
// `chain_parity` runs unchanged. The callable must be a pure function of its
// arguments (it is the caller's surface evaluator).
//
// Not `noexcept`: it allocates the model-vol buffer and may invoke a
// caller-provided callable that can throw.
template <class SurfaceFn>
  requires std::invocable<SurfaceFn &, double, double>
[[nodiscard]] atx::core::Result<ParityReport>
chain_parity(std::span<const double> strike, std::span<const double> bid,
             std::span<const double> ask, std::span<const double> mid, std::span<const Side> side,
             SurfaceFn &&model_vol_at, std::span<const double> market_iv, const ParityInputs &in) {
  // Evaluate the surface at each strike's log-moneyness. A non-positive strike
  // or forward yields a non-finite k_log; the resulting non-finite model vol is
  // screened out downstream, so no guard is needed here.
  const double fwd = in.S * std::exp((in.r - in.q_eff) * in.T);
  std::vector<double> model_iv;
  model_iv.reserve(strike.size());
  for (const double k : strike) {
    const double k_log = std::log(k / fwd);
    model_iv.push_back(static_cast<double>(model_vol_at(k_log, in.T)));
  }
  return chain_parity(strike, bid, ask, mid, side, std::span<const double>{model_iv}, market_iv,
                      in);
}

} // namespace atx::vol
