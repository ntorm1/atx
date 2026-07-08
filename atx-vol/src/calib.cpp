#include "atx/vol/calib.hpp"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <utility>

#include "atx/core/error.hpp"
#include "atx/vol/american_iv.hpp"  // american_implied_vol (de-Americanization)
#include "atx/vol/arb.hpp"          // QuoteFlag, has_flag (kill-mask filter step)
#include "atx/vol/black76.hpp"      // black76_value_and_vega, black76_price
#include "atx/vol/implied_vol.hpp"  // implied_vol (IV inversion)

// Shared calibration infrastructure — implementation.
//
// The observation builder and the accept predicate share one row-level filter
// cascade (`evaluate_row`) so they can never drift; the C kept two hand-copied
// copies (`ats_vol_svi_build_observations` and `ats_vol_calib_obs_accepted`)
// and a comment demanding they stay in lock-step. The cascade, the weight
// formula, and the IV band are ported bit-for-bit from ats_calibrate_svi.c.

namespace atx::vol {

using atx::core::Err;
using atx::core::ErrorCode;
using atx::core::Ok;

namespace {

// IV acceptance band — the C hardcodes (0.005, 5.0) in the obs builder. Note
// the upper bound is 5.0, NOT the pricing kernel's kIvMax (10.0); keep a local
// constant so the cascade stays a faithful port.
inline constexpr double kObsIvMin = 0.005;
inline constexpr double kObsIvMax = 5.0;

// Vega floor below which the spread/vega ratios are ill-conditioned (the C's
// `1e-12` guard), and the additive epsilons in the weight formula (the C's
// `1e-18`). Reproduced verbatim so the weights match to the ULP.
inline constexpr double kVegaFloor = 1.0e-12;
inline constexpr double kWeightEps = 1.0e-18;

// The C returns ERR_NO_DATA (→ NotFound) unless at least this many rows survive.
inline constexpr std::size_t kMinObs = 5;

// w-space observation weight: vega² / (spread² + eps) / ((2σT)² + eps). Shared
// by the American builder (`build_observations`) and the de-Americanized
// builder (`build_observations_european`) so the two can never drift. The
// operand order is exactly what both sites evaluated inline, so the returned
// weight is bit-identical to the historical per-site arithmetic.
[[nodiscard]] double obs_weight_w(double vega, double spread, double sigma,
                                  double T) noexcept {
  const double denom_w = 2.0 * sigma * T;
  return (vega * vega) / (spread * spread + kWeightEps) /
         (denom_w * denom_w + kWeightEps);
}

// Per-row filter outcome. `Skipped` is the non-preferred leg that passed the
// flag + bid/ask gates but lost the prefer-call heuristic — the C `continue`
// that is NOT counted as a drop (unlike a genuine `Rejected`).
enum class RowOutcome : std::uint8_t { Accepted, Rejected, Skipped };

struct RowResult {
  RowOutcome outcome{RowOutcome::Rejected};
  FitObs obs{};
};

// Evaluate one (strike, side) tuple against the quote-filter cascade. Mirrors
// exactly one iteration of the C `ats_vol_svi_build_observations` inner loop
// (tenor buckets omitted → scalar max_spread_vol / min_vega_weight). The caller
// has already validated F/T/df and the chain SoA sizing.
[[nodiscard]] RowResult evaluate_row(const Chain &chain, std::uint16_t strike_idx,
                                     Side side, double F, double T, double df,
                                     const CalibOpts &opts) {
  RowResult r{};
  const double K = chain.strikes[strike_idx];
  if (!(K > 0.0)) {
    return r;  // Rejected (defensive; the strike loop already guards K > 0)
  }
  const std::size_t idx = chain_index(strike_idx, side);

  // 1. Kill-mask flags (Locked/Crossed/Stale/Halted/WideSpread/Penny/LowVega).
  constexpr QuoteFlag kill_mask = QuoteFlag::Locked | QuoteFlag::Crossed |
                                  QuoteFlag::Stale | QuoteFlag::Halted |
                                  QuoteFlag::WideSpread | QuoteFlag::Penny |
                                  QuoteFlag::LowVega;
  const auto flag = static_cast<QuoteFlag>(chain.flags[idx]);
  if (has_flag(flag, kill_mask)) {
    return r;  // Rejected
  }

  // 2. Two-sided, positive, non-crossed quote.
  const double bid = chain.bids[idx];
  const double ask = chain.asks[idx];
  if (!(bid > 0.0 && ask > bid)) {
    return r;  // Rejected
  }

  // Prefer-call heuristic: keep the call leg for K ≥ F, the put leg otherwise.
  // The non-preferred leg is silently skipped — NOT counted as a drop (this
  // gate deliberately sits AFTER the flag + bid/ask checks, matching the C, so
  // a flagged non-preferred leg still counts as a drop above).
  const bool prefer_call = (K >= F);
  if (prefer_call != (side == Side::Call)) {
    r.outcome = RowOutcome::Skipped;
    return r;
  }

  // 3. Positive mid.
  const double mid = chain.mids[idx];
  if (!(mid > 0.0)) {
    return r;  // Rejected
  }

  // 4. Wide-spread-to-mid filter (Sprint 24 Phase D); disabled when the cap ≤ 0.
  if (opts.max_spread_to_mid_pct > 0.0) {
    const double spread_pct = (ask - bid) / mid;
    if (spread_pct > opts.max_spread_to_mid_pct) {
      return r;  // Rejected
    }
  }

  // Anchor-aware price target. Only the stored value changes; the IV inversion
  // below always uses the raw symmetric mid.
  double target = mid;
  if (opts.anchor_kind == CalibAnchorKind::Bid) {
    target = bid;
  } else if (opts.anchor_kind == CalibAnchorKind::Ask) {
    target = ask;
  }

  // 5. Invert IV from the raw mid; reject on failure or out-of-band.
  const Result<double> iv_res = implied_vol(mid, F, K, T, df, side);
  if (!iv_res.has_value()) {
    return r;  // Rejected
  }
  const double iv = *iv_res;
  if (!(iv > kObsIvMin && iv < kObsIvMax)) {
    return r;  // Rejected
  }

  // Vega = F·df·φ(d1)·√T (identical to the C's hand-rolled pdf(d1) form).
  const double vega = black76_value_and_vega(F, K, T, iv, df, side).vega;
  const double spread = ask - bid;

  // 6. Wide spread in vol units.
  if (vega > kVegaFloor) {
    const double spread_vol = spread / vega;
    if (spread_vol > opts.max_spread_vol) {
      return r;  // Rejected
    }
  }

  // 7. w-space weight: weight_sigma = vega²/spread² is the filter quantity;
  //    weight_w = weight_sigma / (2σT)² is the value stored on the obs (built by
  //    the shared obs_weight_w helper — identical arithmetic to the de-Am path).
  const double weight_sigma = (vega * vega) / (spread * spread + kWeightEps);
  const double weight_w = obs_weight_w(vega, spread, iv, T);
  if (weight_sigma < opts.min_vega_weight) {
    return r;  // Rejected
  }

  // Accepted — fill the observation.
  FitObs &o = r.obs;
  o.k = std::log(K / F);
  o.sigma_mkt = iv;
  o.w_mkt = iv * iv * T;
  o.weight_w = weight_w;
  o.active_weight_w = weight_w;
  o.K = K;
  o.F = F;
  o.df = df;
  o.mid = target;
  o.spread = spread;
  o.vega = vega;
  o.noise_sigma = (vega > kVegaFloor) ? (spread / vega) : 1.0;
  o.side = side;
  r.outcome = RowOutcome::Accepted;
  return r;
}

// True if `chain` carries the full 2·n_strikes SoA columns the cascade reads.
[[nodiscard]] bool chain_soa_well_formed(const Chain &chain) noexcept {
  const std::size_t need = 2u * chain.n_strikes();
  return chain.bids.size() >= need && chain.asks.size() >= need &&
         chain.mids.size() >= need && chain.flags.size() >= need;
}

}  // namespace

CalibOpts calib_default_opts() noexcept { return CalibOpts{}; }

Result<ObsSet> build_observations(const Chain &chain, double F, double T,
                                  double df, const CalibOpts &opts) {
  if (!(F > 0.0) || !(T > 0.0)) {
    return Err(ErrorCode::InvalidArgument,
               "build_observations: F and T must be positive");
  }
  if (!chain_soa_well_formed(chain)) {
    return Err(ErrorCode::InvalidArgument,
               "build_observations: chain SoA arrays shorter than 2*n_strikes");
  }

  const std::size_t n = chain.n_strikes();
  ObsSet out;
  out.obs.reserve(n);  // at most one accepted (preferred) leg per strike
  std::uint32_t n_drop = 0;

  for (std::size_t s = 0; s < n; ++s) {
    const double K = chain.strikes[s];
    if (!(K > 0.0)) {
      ++n_drop;  // matches the C strike-level drop (counted once per strike)
      continue;
    }
    const auto sidx = static_cast<std::uint16_t>(s);
    for (int side_i = 0; side_i < 2; ++side_i) {
      const Side side = static_cast<Side>(static_cast<std::uint8_t>(side_i));
      const RowResult rr = evaluate_row(chain, sidx, side, F, T, df, opts);
      if (rr.outcome == RowOutcome::Accepted) {
        out.obs.push_back(rr.obs);
      } else if (rr.outcome == RowOutcome::Rejected) {
        ++n_drop;
      }
      // Skipped: the non-preferred leg — not counted (C bare `continue`).
    }
  }

  out.n_dropped = n_drop;
  if (out.obs.size() < kMinObs) {
    return Err(ErrorCode::NotFound,
               "build_observations: fewer than 5 observations survived");
  }
  return Ok(std::move(out));
}

Result<ObsSet> build_observations_european(const Chain &chain, double S, double r,
                                           double F, double T, double df,
                                           const CalibOpts &opts,
                                           const AmericanCorrectionCaches &caches,
                                           const std::optional<AlOpts> &al_opts,
                                           double iv_tol,
                                           std::uint16_t iv_max_iter) {
  if (!(S > 0.0) || !(F > 0.0) || !(T > 0.0) || !(df > 0.0) || !std::isfinite(r)) {
    return Err(ErrorCode::InvalidArgument,
               "build_observations_european: S, F, T, df must be positive");
  }
  // Run the shared American filter cascade, then strip each surviving leg.
  auto am = build_observations(chain, F, T, df, opts);
  if (!am.has_value()) {
    return am;  // propagate NotFound / InvalidArgument unchanged
  }
  // q_eff bridge: S·e^{(r−q_eff)T} == F exactly (matches the fit/de-Am carry).
  const double q_eff = r - std::log(F / S) / T;

  ObsSet out;
  out.obs.reserve(am->obs.size());
  out.n_dropped = am->n_dropped;
  for (FitObs o : am->obs) {
    // `o.mid` is the anchor premium (the raw American mid under the default Mid
    // anchor). Recover the European-equivalent lognormal vol, then restate the
    // observation entirely in European terms.
    const Result<double> sig = american_implied_vol(
        o.mid, S, o.K, T, r, q_eff, o.side, AmericanMethod::AndersenLake, iv_tol,
        iv_max_iter, al_opts, caches.for_side(o.side));
    if (!sig.has_value() || !(*sig > kObsIvMin && *sig < kObsIvMax)) {
      ++out.n_dropped;
      continue;
    }
    const double sigma_eu = *sig;
    const double eu_px = black76_price(F, o.K, T, sigma_eu, df, o.side);
    if (!(eu_px > 0.0) || !std::isfinite(eu_px)) {
      ++out.n_dropped;
      continue;
    }
    const double vega =
        black76_value_and_vega(F, o.K, T, sigma_eu, df, o.side).vega;
    o.sigma_mkt = sigma_eu;
    o.w_mkt = sigma_eu * sigma_eu * T;
    o.mid = eu_px;  // European-equivalent premium (what the convex fold expects)
    o.vega = vega;
    o.weight_w = obs_weight_w(vega, o.spread, sigma_eu, T);
    o.active_weight_w = o.weight_w;
    o.noise_sigma = (vega > kVegaFloor) ? (o.spread / vega) : 1.0;
    out.obs.push_back(o);
  }
  if (out.obs.size() < kMinObs) {
    return Err(ErrorCode::NotFound,
               "build_observations_european: fewer than 5 European obs survived");
  }
  return Ok(std::move(out));
}

Result<double> obs_accepted(const Chain &chain, std::uint16_t strike_idx,
                            Side side, double F, double T, double df,
                            const CalibOpts &opts) {
  if (strike_idx >= chain.n_strikes()) {
    return Err(ErrorCode::InvalidArgument, "obs_accepted: strike_idx out of range");
  }
  if (!(F > 0.0) || !(T > 0.0) || !(df > 0.0)) {
    return Err(ErrorCode::InvalidArgument,
               "obs_accepted: F, T and df must be positive");
  }
  if (!chain_soa_well_formed(chain)) {
    return Err(ErrorCode::InvalidArgument,
               "obs_accepted: chain SoA arrays shorter than 2*n_strikes");
  }

  const RowResult rr = evaluate_row(chain, strike_idx, side, F, T, df, opts);
  if (rr.outcome == RowOutcome::Accepted) {
    return Ok(rr.obs.sigma_mkt);
  }
  return Err(ErrorCode::NotFound,
             "obs_accepted: quote rejected by the filter cascade");
}

}  // namespace atx::vol
