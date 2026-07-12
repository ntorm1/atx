#include "atx/vol/calib.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

#include "atx/core/error.hpp"
#include "atx/vol/american_iv.hpp"  // american_implied_vol (de-Americanization)
#include "atx/vol/arb.hpp"          // QuoteFlag, has_flag (kill-mask filter step)
#include "atx/vol/black76.hpp"      // black76_value_and_vega, black76_price
#include "atx/vol/deamer.hpp"       // cold-reference IV proposal audit
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

// Cap a very dense per-slice population before the American de-Am inversion.
// Retain endpoints, then greedily split the segment with the largest normalized
// total-variance interpolation miss. This is a bounded Ramer-Douglas-Peucker
// simplifier adapted to option noise: knots are spent where a piecewise-linear
// market curve would otherwise leave the NBBO first, rather than uniformly on
// already-linear regions. The raw European-seed w is sufficient for selection;
// retained rows are still fully de-Americanized below.
void cap_observations_for_deam(ObsSet &set, std::uint32_t requested_cap) {
  if (requested_cap == 0 || set.obs.empty()) {
    return;
  }
  const std::size_t cap = std::max<std::size_t>(kMinObs, static_cast<std::size_t>(requested_cap));
  if (set.obs.size() <= cap) {
    return;
  }

  std::vector<FitObs> sorted = std::move(set.obs);
  std::sort(sorted.begin(), sorted.end(),
            [](const FitObs &a, const FitObs &b) { return a.k < b.k; });

  const std::size_t m = sorted.size();
  std::vector<char> selected(m, 0);
  selected.front() = 1;
  selected.back() = 1;
  std::size_t n_selected = 2;

  while (n_selected < cap) {
    std::size_t best = m;
    double best_score = -1.0;
    std::size_t lo = 0;
    while (lo + 1 < m) {
      std::size_t hi = lo + 1;
      while (hi < m && selected[hi] == 0) {
        ++hi;
      }
      if (hi >= m) {
        break;
      }
      const double k_span = sorted[hi].k - sorted[lo].k;
      if (k_span > 0.0) {
        for (std::size_t i = lo + 1; i < hi; ++i) {
          const double a = (sorted[i].k - sorted[lo].k) / k_span;
          const double w_linear = sorted[lo].w_mkt + a * (sorted[hi].w_mkt - sorted[lo].w_mkt);
          const double noise_w = std::max(1.0e-12, 2.0 * sorted[i].w_mkt / sorted[i].sigma_mkt *
                                                       sorted[i].noise_sigma);
          // A tiny coverage term recursively bisects a perfectly linear segment.
          const double coverage = std::min(a, 1.0 - a);
          const double score = std::fabs(sorted[i].w_mkt - w_linear) / noise_w + 1.0e-9 * coverage;
          if (score > best_score ||
              (score == best_score &&
               (best == m || sorted[i].active_weight_w > sorted[best].active_weight_w))) {
            best = i;
            best_score = score;
          }
        }
      }
      lo = hi;
    }
    if (best == m) {
      break;
    }
    selected[best] = 1;
    ++n_selected;
  }

  std::vector<FitObs> kept;
  kept.reserve(n_selected);
  for (std::size_t i = 0; i < m; ++i) {
    if (selected[i] != 0) {
      kept.push_back(sorted[i]);
    }
  }
  set.n_dropped += static_cast<std::uint32_t>(m - kept.size());
  set.obs = std::move(kept);
}

[[nodiscard]] bool use_otm_shortcut_deam(const FitObs &o, double S, double T, double r,
                                         double q_eff, const CalibOpts &opts,
                                         AmericanMethod method,
                                         DeAmAuditDiagnostics* diag) noexcept {
  if (!(opts.max_otm_shortcut_premium_spread_frac > 0.0) ||
      opts.anchor_kind != CalibAnchorKind::Mid || method != AmericanMethod::AndersenLake ||
      !(o.spread > 0.0) || !(o.sigma_mkt > kObsIvMin && o.sigma_mkt < kObsIvMax)) {
    return false;
  }
  if (T < opts.min_otm_shortcut_T) {
    if (diag != nullptr) ++diag->n_forced_short_tenor;
    return false;
  }
  if (o.vega < opts.min_otm_shortcut_vega) {
    if (diag != nullptr) ++diag->n_forced_low_vega;
    return false;
  }
  if (std::fabs(o.k) > opts.max_otm_shortcut_abs_k) {
    if (diag != nullptr) ++diag->n_forced_far_wing;
    return false;
  }

  const Result<double> baw = baw_american(S, o.K, T, o.sigma_mkt, r, q_eff, o.side);
  if (!baw.has_value() || !std::isfinite(*baw)) {
    return false;
  }
  const double eu = black76_price(o.F, o.K, T, o.sigma_mkt, o.df, o.side);
  if (!(eu > 0.0) || !std::isfinite(eu)) {
    return false;
  }
  const double premium = *baw - eu;
  const double tol = opts.max_otm_shortcut_premium_spread_frac * o.spread;
  return premium >= -0.05 * o.spread && premium <= tol;
}

enum class IvRoute : std::uint8_t { Shortcut = 0, Cache = 1, Fast = 2, Accurate = 3 };

[[nodiscard]] InversionRouteDiagnostics& route_diag(DeAmAuditDiagnostics& diag,
                                                    IvRoute route) noexcept {
  switch (route) {
    case IvRoute::Shortcut: return diag.shortcut;
    case IvRoute::Cache: return diag.cache;
    case IvRoute::Fast: return diag.fast;
    case IvRoute::Accurate: return diag.accurate;
  }
  return diag.accurate;
}

void finalize_route_diag(InversionRouteDiagnostics& diag,
                         std::vector<double> residuals) {
  if (residuals.empty()) return;
  std::sort(residuals.begin(), residuals.end());
  const auto percentile = [&](double p) {
    const double pos = p * static_cast<double>(residuals.size() - 1);
    const std::size_t lo = static_cast<std::size_t>(pos);
    const std::size_t hi = std::min(lo + 1, residuals.size() - 1);
    const double a = pos - static_cast<double>(lo);
    return residuals[lo] + a * (residuals[hi] - residuals[lo]);
  };
  diag.p50_residual_half_spreads = percentile(0.50);
  diag.p95_residual_half_spreads = percentile(0.95);
  diag.max_residual_half_spreads = residuals.back();
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

Result<ObsSet> build_observations_european(const Chain &chain, double S, double r, double F,
                                           double T, double df, const CalibOpts &opts,
                                           const AmericanCorrectionCaches &caches,
                                           const std::optional<AlOpts> &al_opts, double iv_tol,
                                           std::uint16_t iv_max_iter, AmericanMethod method) {
  if (!(S > 0.0) || !(F > 0.0) || !(T > 0.0) || !(df > 0.0) || !std::isfinite(r)) {
    return Err(ErrorCode::InvalidArgument,
               "build_observations_european: S, F, T, df must be positive");
  }
  // Run the shared American filter cascade, then strip each surviving leg.
  auto am = build_observations(chain, F, T, df, opts);
  if (!am.has_value()) {
    return am;  // propagate NotFound / InvalidArgument unchanged
  }
  cap_observations_for_deam(*am, opts.max_obs_per_slice);
  // q_eff bridge: S·e^{(r−q_eff)T} == F exactly (matches the fit/de-Am carry).
  const double q_eff = r - std::log(F / S) / T;

  ObsSet out;
  out.obs.reserve(am->obs.size());
  out.n_dropped = am->n_dropped;
  // Warm-starting changes the last few bits of a tolerance-terminated IV solve.
  // Keep the historical/default full-board path cold so its fitted surface stays
  // bit-identical; the accelerated path opts in through either of its explicit
  // observation shortcuts.
  const bool warm_start_deam = opts.max_obs_per_slice > 0 ||
                                opts.max_otm_shortcut_premium_spread_frac > 0.0;
  double warm_call = 0.0;
  double warm_put = 0.0;
  std::array<std::vector<double>, 4> audit_residuals;
  for (FitObs o : am->obs) {
    // `o.mid` is the anchor premium (the raw American mid under the default Mid
    // anchor). Recover the European-equivalent lognormal vol, then restate the
    // observation entirely in European terms.
    ++out.deam_audit.n_deam_rows;
    const bool shortcut =
        use_otm_shortcut_deam(o, S, T, r, q_eff, opts, method,
                              &out.deam_audit);
    const CorrectionCache* correction = caches.for_side(o.side);
    const bool cache_proposal = correction != nullptr && correction->populated() &&
                                correction->side() == o.side;
    const IvRoute route = shortcut
                              ? IvRoute::Shortcut
                              : (cache_proposal
                                     ? IvRoute::Cache
                                     : (method == AmericanMethod::AndersenLake &&
                                                al_opts.has_value()
                                            ? IvRoute::Fast
                                            : IvRoute::Accurate));
    InversionRouteDiagnostics& proposal_diag =
        route_diag(out.deam_audit, route);
    ++proposal_diag.n_proposed;
    const double warm = warm_start_deam ? ((o.side == Side::Call) ? warm_call : warm_put) : 0.0;
    Result<double> sig = shortcut
                             ? Ok(o.sigma_mkt)
                             : american_implied_vol(
                                   o.mid, S, o.K, T, r, q_eff, o.side, method,
                                   iv_tol, iv_max_iter, al_opts, correction, warm);
    if (!sig.has_value() || !(*sig > kObsIvMin && *sig < kObsIvMax)) {
      ++out.n_dropped;
      continue;
    }

    // All Andersen-Lake routes, including the nominally accurate one, are
    // independently repriced. Approximate proposals that miss the budget are
    // recomputed with the cold accurate solver; the fallback is audited again.
    if (method == AmericanMethod::AndersenLake) {
      ++proposal_diag.n_audited;
      Result<IvRepricingAudit> audit = audit_european_equiv_iv(
          o.mid, o.spread, *sig, S, o.K, T, r, q_eff, o.side,
          opts.max_inversion_residual_half_spreads);
      if (audit) {
        audit_residuals[static_cast<std::size_t>(route)].push_back(
            audit->residual_half_spreads);
      }
      if (!audit || !audit->passed) {
        if (route == IvRoute::Accurate) {
          ++out.deam_audit.n_rejected_residual;
          ++out.n_dropped;
          continue;
        }
        ++proposal_diag.n_fallback;
        ++out.deam_audit.n_accurate_fallback;
        InversionRouteDiagnostics& accurate_diag = out.deam_audit.accurate;
        ++accurate_diag.n_proposed;
        sig = american_implied_vol(o.mid, S, o.K, T, r, q_eff, o.side,
                                   AmericanMethod::AndersenLake, 1.0e-7, 64,
                                   std::nullopt, nullptr, *sig);
        if (!sig || !(*sig > kObsIvMin && *sig < kObsIvMax)) {
          ++out.n_dropped;
          continue;
        }
        ++accurate_diag.n_audited;
        audit = audit_european_equiv_iv(
            o.mid, o.spread, *sig, S, o.K, T, r, q_eff, o.side,
            opts.max_inversion_residual_half_spreads);
        if (audit) {
          audit_residuals[static_cast<std::size_t>(IvRoute::Accurate)].push_back(
              audit->residual_half_spreads);
        }
        if (!audit || !audit->passed) {
          ++out.deam_audit.n_rejected_residual;
          ++out.n_dropped;
          continue;
        }
        ++accurate_diag.n_accepted;
      } else {
        ++proposal_diag.n_accepted;
      }
    } else {
      ++proposal_diag.n_accepted;
    }

    const double sigma_eu = *sig;
    if (warm_start_deam) {
      if (o.side == Side::Call) {
        warm_call = sigma_eu;
      } else {
        warm_put = sigma_eu;
      }
    }
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
    ++out.deam_audit.n_deam_accepted;
    out.obs.push_back(o);
  }
  finalize_route_diag(out.deam_audit.shortcut,
                      std::move(audit_residuals[0]));
  finalize_route_diag(out.deam_audit.cache, std::move(audit_residuals[1]));
  finalize_route_diag(out.deam_audit.fast, std::move(audit_residuals[2]));
  finalize_route_diag(out.deam_audit.accurate,
                      std::move(audit_residuals[3]));
  if (out.obs.size() < kMinObs) {
    return Err(ErrorCode::NotFound,
               "build_observations_european: fewer than 5 European obs survived");
  }
  return Ok(std::move(out));
}

bool deam_inversion_certified(const DeAmAuditDiagnostics &audit,
                              double max_drop_fraction) noexcept {
  // 1. Every ACCEPTED proposal must have been audited. A route that accepts
  //    more than it audits carries un-audited nodes into the fit set — the
  //    shape of a method (e.g. Baw) with no cold-reference audit at all.
  const auto route_audited = [](const InversionRouteDiagnostics &route) noexcept {
    return route.n_accepted <= route.n_audited;
  };
  if (!route_audited(audit.shortcut) || !route_audited(audit.cache) ||
      !route_audited(audit.fast) || !route_audited(audit.accurate)) {
    return false;
  }
  // 2. The stage must have run and produced at least one accepted node.
  if (audit.n_deam_rows == 0u || audit.n_deam_accepted == 0u ||
      audit.n_deam_accepted > audit.n_deam_rows) {
    return false;
  }
  // 3. Tolerated node drops stay under the cap; fail-closed on a bad budget.
  if (!std::isfinite(max_drop_fraction) || max_drop_fraction < 0.0) {
    return false;
  }
  const std::uint32_t dropped = audit.n_deam_rows - audit.n_deam_accepted;
  const double drop_fraction = static_cast<double>(dropped) /
                               static_cast<double>(audit.n_deam_rows);
  return drop_fraction <= max_drop_fraction;
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
