#include "atx/vol/parity.hpp"

#include <cmath>
#include <cstddef>
#include <span>
#include <vector>

#include "atx/core/error.hpp"
#include "atx/vol/american.hpp"      // american_price, american_price_cached
#include "atx/vol/black76.hpp"       // black76_value_and_vega
#include "atx/vol/correction.hpp"    // CorrectionCache (hot-path re-pricing)
#include "atx/vol/fit_metrics.hpp"   // vol_error_bar, reduced_chi_square, minimum_edge

// PORT NOTES
// ----------
// * Error-bar vega. The per-quote vol error bar converts a price half-spread
//   into a vol uncertainty through vega (σ_err ≈ ½·spread / vega). Because both
//   `model_iv` and `market_iv` live in EUROPEAN (Black-76) vol space, the
//   natural vega is the Black-76 vega evaluated at the de-Americanized MARKET
//   vol — the vol consistent with the observed mid. We use
//   `black76_value_and_vega(F, K, T, market_iv, df, side).vega` rather than the
//   American `american_greeks(..., nullptr)` vega. Two reasons: (1) with a null
//   correction cache the American Greeks path DEGRADES to exactly this Black-76
//   vega, so the numbers coincide, and (2) this module is deliberately decoupled
//   from any surface / correction-cache type, so the pure Black-76 kernel keeps
//   it dependency-free. The error bar only needs the vega MAGNITUDE to scale a
//   price spread into vol points, and the European-equivalent vega is the
//   consistent choice for the European-space residual it weights.
//
// * De-Americanized market vol. `market_iv` is expected to be the
//   European-equivalent (de-Americanized) market vol, NOT the raw American IV.
//   The caller performs the de-Americanization upstream; here we only
//   re-Americanize the MODEL vol (american_price(model_iv, …)) to obtain a fair
//   value comparable to the American quote.
//
// * dof source. The reduced chi-square divides by (N − dof); `dof` is taken from
//   `ParityInputs::n_curve_params`, the number of fitted surface/curve
//   parameters. It flows straight into `reduced_chi_square`, which enforces
//   N > dof.

namespace atx::vol {

using atx::core::Err;
using atx::core::ErrorCode;
using atx::core::Ok;

namespace {

// A quote is scorable when its bid-ask is a well-formed, positive, uncrossed
// price band, its strike is positive, and both vols are finite. Screened-out
// quotes are dropped from the scored population entirely (reflected in `n`).
[[nodiscard]] bool quote_scorable(double strike, double bid, double ask,
                                  double mid, double model_iv,
                                  double market_iv) noexcept {
  if (!std::isfinite(strike) || !(strike > 0.0)) return false;
  if (!std::isfinite(bid) || !std::isfinite(ask) || !std::isfinite(mid)) return false;
  if (!(bid > 0.0) || !(ask > 0.0)) return false;   // non-positive quote
  if (ask < bid) return false;                       // crossed
  if (!std::isfinite(model_iv) || !std::isfinite(market_iv)) return false;
  return true;
}

}  // namespace

Result<ParityReport> chain_parity(std::span<const double> strike,
                                  std::span<const double> bid,
                                  std::span<const double> ask,
                                  std::span<const double> mid,
                                  std::span<const Side> side,
                                  std::span<const double> model_iv,
                                  std::span<const double> market_iv,
                                  const ParityInputs& in) noexcept {
  const std::size_t n = strike.size();
  if (bid.size() != n || ask.size() != n || mid.size() != n ||
      side.size() != n || model_iv.size() != n || market_iv.size() != n) {
    return Err(ErrorCode::InvalidArgument, "chain_parity: input length mismatch");
  }
  if (n == 0) {
    return Err(ErrorCode::InvalidArgument, "chain_parity: empty chain");
  }

  const double fwd = in.S * std::exp((in.r - in.q_eff) * in.T);
  const double df = std::exp(-in.r * in.T);

  // Scored residuals/error bars, forwarded to reduced_chi_square. Reserve the
  // full length up front; the scored count never exceeds `n`.
  // SAFETY: `chain_parity` is `noexcept` per its contract, yet these vectors
  // allocate. The only throw they can raise is std::bad_alloc, which this layer
  // treats as a fatal, fail-safe condition (terminate) — never a recoverable
  // parity outcome — for a chain bounded by `n`.
  std::vector<double> resid_vol;
  std::vector<double> err_bar;
  resid_vol.reserve(n);
  err_bar.reserve(n);

  double sum_sq_price = 0.0;  // Σ (fair_value − mid)²
  double sum_sq_vol = 0.0;    // Σ (model_iv − market_iv)²
  double sum_edge = 0.0;      // Σ signed edge (model − market)
  std::size_t n_scored = 0;
  std::size_t n_within = 0;      // fair value inside [bid, ask]
  std::size_t n_within_edge = 0; // no statistical edge (|edge| < band_k·σ_err)

  for (std::size_t i = 0; i < n; ++i) {
    if (!quote_scorable(strike[i], bid[i], ask[i], mid[i], model_iv[i],
                        market_iv[i])) {
      continue;
    }

    // Re-Americanize the MODEL vol into an American fair value. When this side's
    // correction cache is populated, price through the cached hot path (the same
    // pricer the de-Americanization used — self-consistent round-trip);
    // otherwise the cold Andersen-Lake path.
    const CorrectionCache* const cc = in.caches.for_side(side[i]);
    double fair_value = 0.0;
    if (cc != nullptr && cc->populated() && cc->side() == side[i]) {
      fair_value = american_price_cached(in.S, strike[i], in.T, model_iv[i],
                                         in.r, in.q_eff, side[i], cc);
      if (!std::isfinite(fair_value)) {
        continue;  // cached pricer non-finite — excluded from the population
      }
    } else {
      const Result<double> fv = american_price(in.S, strike[i], in.T,
                                               model_iv[i], in.r, in.q_eff,
                                               side[i], in.method, in.al_opts);
      if (!fv.has_value() || !std::isfinite(*fv)) {
        continue;  // unpriceable quote — excluded from the scored population
      }
      fair_value = *fv;
    }

    // Bid-ask acceptance (inclusive band).
    const bool within = (fair_value >= bid[i]) && (fair_value <= ask[i]);
    if (within) ++n_within;

    // Error bar from the price half-spread and the European-equivalent (market)
    // Black-76 vega. See PORT NOTES.
    const double vega =
        black76_value_and_vega(fwd, strike[i], in.T, market_iv[i], df, side[i])
            .vega;
    const double err = vol_error_bar(bid[i], ask[i], vega);

    const double resid_p = fair_value - mid[i];
    const double resid_v = model_iv[i] - market_iv[i];
    sum_sq_price += resid_p * resid_p;
    sum_sq_vol += resid_v * resid_v;

    const EdgeResult edge = minimum_edge(model_iv[i], market_iv[i], err, in.band_k);
    if (edge.within_band) ++n_within_edge;
    sum_edge += edge.edge_vol;

    resid_vol.push_back(resid_v);
    err_bar.push_back(err);
    ++n_scored;
  }

  if (n_scored == 0) {
    return Err(ErrorCode::InvalidArgument, "chain_parity: no scorable quotes");
  }

  // Reduced chi-square (vol space, error-bar weighted). Propagates its own
  // guard when the scored count does not exceed the curve-parameter dof.
  ATX_TRY(const auto chi,
          reduced_chi_square(resid_vol, err_bar, in.n_curve_params));

  const double dn = static_cast<double>(n_scored);
  ParityReport out{};
  out.frac_fv_within_bidask = static_cast<double>(n_within) / dn;
  out.rmse_mid_price = std::sqrt(sum_sq_price / dn);
  out.rmse_mid_vol = std::sqrt(sum_sq_vol / dn);
  out.chi2_reduced = chi.chi2_reduced;
  out.frac_within_edge_band = static_cast<double>(n_within_edge) / dn;
  out.mean_edge_vol = sum_edge / dn;
  out.n = n_scored;
  out.n_within = n_within;
  return Ok(out);
}

}  // namespace atx::vol
