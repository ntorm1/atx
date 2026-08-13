#include "atx/vol/parity.hpp"

#include <cmath>
#include <cstddef>
#include <span>
#include <vector>

#include "atx/core/error.hpp"
#include "atx/vol/american.hpp"    // american_price, american_price_cached
#include "atx/vol/black76.hpp"     // black76_value_and_vega
#include "atx/vol/correction.hpp"  // CorrectionCache (hot-path re-pricing)
#include "atx/vol/fit_metrics.hpp" // vol_error_bar, reduced_chi_square, minimum_edge

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

// T5 item 3: the vega below which a price residual has NO vol interpretation.
//
// Black-76 vega is dPrice/dSigma in price per unit vol, so one vol point (0.01)
// moves a quote by `0.01 * vega`. At vega = 1.0 that is exactly one cent — one
// tick. Below it, a whole vol point does not move the quote by a displayable
// increment, so `|dPrice| / vega` is not a vol displacement, it is a division by
// something the market cannot resolve: on a real SPY board the 1-day deep wings
// produce ratios of 1e13 "vol points" that describe nothing.
//
// Excluded quotes are counted out of `ParityReport::n_round_trip` (not silently
// folded in at zero), so `n_round_trip < n` is the visible signal that part of
// the slice carries no vol-space verdict.
constexpr double kMinRoundTripVega = 1.0;

// A quote is scorable when its bid-ask is a well-formed, positive, uncrossed
// price band, its strike is positive, and both vols are finite. Screened-out
// quotes are dropped from the scored population entirely (reflected in `n`).
[[nodiscard]] bool quote_scorable(double strike, double bid, double ask, double mid,
                                  double model_iv, double market_iv) noexcept {
  if (!std::isfinite(strike) || !(strike > 0.0))
    return false;
  if (!std::isfinite(bid) || !std::isfinite(ask) || !std::isfinite(mid))
    return false;
  if (!(bid > 0.0) || !(ask > 0.0))
    return false; // non-positive quote
  if (ask < bid)
    return false; // crossed
  if (!std::isfinite(model_iv) || !std::isfinite(market_iv))
    return false;
  return true;
}

} // namespace

Result<ParityReport> chain_parity(std::span<const double> strike, std::span<const double> bid,
                                  std::span<const double> ask, std::span<const double> mid,
                                  std::span<const Side> side, std::span<const double> model_iv,
                                  std::span<const double> market_iv,
                                  const ParityInputs &in) noexcept {
  const std::size_t n = strike.size();
  if (bid.size() != n || ask.size() != n || mid.size() != n || side.size() != n ||
      model_iv.size() != n || market_iv.size() != n) {
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

  // SpiderRock-style band-violation scoring (model price vs bid/ask), scored
  // over the SAME population as the rest of this report. Parallel spans fed
  // to `band_violation_stats` below.
  std::vector<double> band_price;
  std::vector<double> band_bid;
  std::vector<double> band_ask;
  band_price.reserve(n);
  band_bid.reserve(n);
  band_ask.reserve(n);

  double sum_sq_price = 0.0; // Σ (fair_value − mid)²
  double sum_sq_vol = 0.0;   // Σ (model_iv − market_iv)²
  double sum_edge = 0.0;     // Σ signed edge (model − market)
  std::size_t n_scored = 0;
  std::size_t n_within = 0;      // fair value inside [bid, ask]
  std::size_t n_within_edge = 0; // no statistical edge (|edge| < band_k·σ_err)
  // T5 item 3: Σ (|fair_value − mid| / vega)² and its worst quote — the absolute
  // round trip in vol points (see ParityReport::rmse_round_trip_vol).
  double sum_sq_round_trip = 0.0;
  double max_round_trip = 0.0;
  std::size_t n_round_trip = 0;

  for (std::size_t i = 0; i < n; ++i) {
    if (!quote_scorable(strike[i], bid[i], ask[i], mid[i], model_iv[i], market_iv[i])) {
      continue;
    }

    // Price European contracts directly with Black-76. For American contracts,
    // re-Americanize the model vol; use the same cached correction path as
    // de-Americanization when available.
    double fair_value = 0.0;
    if (in.exercise_style == ExerciseStyle::European) {
      fair_value = black76_value_and_vega(fwd, strike[i], in.T, model_iv[i], df, side[i]).price;
      if (!std::isfinite(fair_value)) {
        continue;
      }
    } else {
      const CorrectionCache *const cc = in.caches.for_side(side[i]);
      if (cc != nullptr && cc->populated() && cc->side() == side[i]) {
        fair_value =
            american_price_cached(in.S, strike[i], in.T, model_iv[i], in.r, in.q_eff, side[i], cc);
        if (!std::isfinite(fair_value)) {
          continue; // cached pricer non-finite — excluded from the population
        }
      } else {
        const Result<double> fv = american_price(in.S, strike[i], in.T, model_iv[i], in.r, in.q_eff,
                                                 side[i], in.method, in.al_opts);
        if (!fv.has_value() || !std::isfinite(*fv)) {
          continue; // unpriceable quote — excluded from the scored population
        }
        fair_value = *fv;
      }
    }

    // Bid-ask acceptance (inclusive band).
    const bool within = (fair_value >= bid[i]) && (fair_value <= ask[i]);
    if (within)
      ++n_within;

    // Error bar from the price half-spread and the European-equivalent (market)
    // Black-76 vega. See PORT NOTES.
    const double vega =
        black76_value_and_vega(fwd, strike[i], in.T, market_iv[i], df, side[i]).vega;
    const double err = vol_error_bar(bid[i], ask[i], vega);

    const double resid_p = fair_value - mid[i];
    const double resid_v = model_iv[i] - market_iv[i];
    sum_sq_price += resid_p * resid_p;
    sum_sq_vol += resid_v * resid_v;

    // T5 item 3: the ABSOLUTE round trip in vol points. `resid_p` is already the
    // re-Americanized model fair value against the ORIGINAL American mid, so
    // dividing by this quote's vega restates the whole de-Am -> fit ->
    // re-Americanize chain as a vol displacement that does NOT scale with the
    // board's spread (see ParityReport). A dead-wing quote (vega <= 0 or
    // non-finite) has no vol interpretation and is excluded from THIS statistic
    // only — the rest of the report keeps its full population.
    if (std::isfinite(vega) && vega >= kMinRoundTripVega) {
      const double round_trip_vol = std::fabs(resid_p) / vega;
      if (std::isfinite(round_trip_vol)) {
        sum_sq_round_trip += round_trip_vol * round_trip_vol;
        max_round_trip = std::fmax(max_round_trip, round_trip_vol);
        ++n_round_trip;
      }
    }

    const EdgeResult edge = minimum_edge(model_iv[i], market_iv[i], err, in.band_k);
    if (edge.within_band)
      ++n_within_edge;
    sum_edge += edge.edge_vol;

    resid_vol.push_back(resid_v);
    err_bar.push_back(err);
    band_price.push_back(fair_value);
    band_bid.push_back(bid[i]);
    band_ask.push_back(ask[i]);
    ++n_scored;
  }

  if (n_scored == 0) {
    return Err(ErrorCode::InvalidArgument, "chain_parity: no scorable quotes");
  }

  // Reduced chi-square (vol space, error-bar weighted). Propagates its own
  // guard when the scored count does not exceed the curve-parameter dof.
  ATX_TRY(const auto chi, reduced_chi_square(resid_vol, err_bar, in.n_curve_params));

  // SpiderRock-style band-violation stats over the same scored population.
  // The three spans are built in lockstep above, so a length mismatch here
  // is impossible by construction; propagate the error anyway per convention.
  ATX_TRY(const auto band, band_violation_stats(band_price, band_bid, band_ask));

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
  out.band = band;
  // D4 (T10c): witness the dof this report's chi2 was scored against by reading
  // it off the SAME input `reduced_chi_square` consumed above — the published
  // number and its denominator can no longer drift apart silently.
  out.chi2_dof = in.n_curve_params;
  out.n_round_trip = n_round_trip;
  out.rmse_round_trip_vol =
      (n_round_trip > 0) ? std::sqrt(sum_sq_round_trip / static_cast<double>(n_round_trip)) : 0.0;
  out.max_round_trip_vol = max_round_trip;
  return Ok(out);
}

} // namespace atx::vol
