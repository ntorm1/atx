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
