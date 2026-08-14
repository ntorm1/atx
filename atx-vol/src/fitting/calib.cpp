#include "atx/vol/api/fitting/calib.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "fitting/calib_detail.hpp"    // SharedLaneBracket, shared_lane_residual_within_budget
#include "pricing/al_probe.hpp"        // env-gated shared-boundary engagement events (Perf 2b step 1)
#include "atx/core/error.hpp"
#include "atx/vol/api/pricing/american_iv.hpp" // american_implied_vol (de-Americanization)
#include "atx/vol/api/fitting/arb.hpp"         // QuoteFlag, has_flag (kill-mask filter step)
#include "atx/vol/api/pricing/black76.hpp"     // black76_value_and_vega, black76_price
#include "atx/vol/api/fitting/deamer.hpp"      // cold-reference IV proposal audit
#include "atx/vol/api/pricing/implied_vol.hpp" // implied_vol (IV inversion)
#include "pricing/boundary_interp.hpp"     // retained sigma-boundary de-Am path

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

// T6. Bounds COMPLETE a slice that a real market has almost pinned; they never
// BUILD one. A slice must therefore already be within one row of the
// identifiability floor on genuinely two-sided evidence before a bid-less row
// may top it up — this is `kMinObs - 1`, not a tuned constant: it is the floor
// itself, minus the single row that bounds are allowed to supply.
//
// The reason is that a set of upper bounds determines only an upper ENVELOPE of
// the price curve. Fitting band midpoints as if they were marks invents a level
// no market anchors, and a level invented independently per expiry is free to
// cross its neighbour in total variance. Measured on ROKU, which admits bounds
// on slices carrying as few as two marked rows: two extra expiries came back,
// the board-level family selection flipped from convex-dense to SVI, the risk
// surface picked up 40 calendar violations (w = 0.0398 against its
// predecessor's 0.0455 at k = -0.5) and the whole board was refused — a board
// that served three slices before. At `kMinObs - 1` ROKU serves its three
// slices again, unchanged. Same principle as the carry solver's
// `min_confident_borrow_pairs`: evidence that nothing disputes is not evidence.
inline constexpr std::size_t kMinBoundAnchorRows = kMinObs - 1u;

// ── Starvation diagnostics (W2-B) ───────────────────────────────────────────
// `ObsSet::provenance` records WHY each preferred leg was dropped, but the
// builder discards the whole set when too few rows survive — losing the
// evidence exactly when a caller needs it, which is how a filter-starved board
// reached the operator as an anonymous "no expiry produced a usable slice".
// These two helpers fold the provenance into the error string. Failure path
// only: no cost on any accepted slice.

[[nodiscard]] const char *obs_rejection_reason_name(ObsRejectionReason reason) noexcept {
  switch (reason) {
  case ObsRejectionReason::None:
    return "None";
  case ObsRejectionReason::InvalidStrike:
    return "InvalidStrike";
  case ObsRejectionReason::QuoteFlag:
    return "QuoteFlag";
  case ObsRejectionReason::InvalidBidAsk:
    return "InvalidBidAsk";
  case ObsRejectionReason::InvalidMid:
    return "InvalidMid";
  case ObsRejectionReason::SpreadToMid:
    return "SpreadToMid";
  case ObsRejectionReason::RawIvFailure:
    return "RawIvFailure";
  case ObsRejectionReason::RawIvOutOfBand:
    return "RawIvOutOfBand";
  case ObsRejectionReason::SpreadVol:
    return "SpreadVol";
  case ObsRejectionReason::LowVegaWeight:
    return "LowVegaWeight";
  case ObsRejectionReason::ObservationCap:
    return "ObservationCap";
  case ObsRejectionReason::Deamericanization:
    return "Deamericanization";
  case ObsRejectionReason::EuropeanPrice:
    return "EuropeanPrice";
  case ObsRejectionReason::IntrinsicIllPosed:
    return "IntrinsicIllPosed";
  }
  return "Unknown";
}

// "(kept=1 of 23; rejected SpreadToMid x14, Deamericanization x8)" — reasons
// ordered by descending count so the dominant cause reads first. Ties keep
// enumerator order, so the string is deterministic for a given provenance.
[[nodiscard]] std::string describe_rejections(std::size_t n_kept,
                                              std::span<const ObsProvenance> provenance) {
  constexpr std::size_t kNReasons =
      static_cast<std::size_t>(ObsRejectionReason::EuropeanPrice) + 1u;
  std::array<std::size_t, kNReasons> tally{};
  for (const ObsProvenance &row : provenance) {
    const auto slot = static_cast<std::size_t>(row.rejection);
    if (row.rejection != ObsRejectionReason::None && slot < kNReasons) {
      ++tally[slot];
    }
  }
  std::array<std::size_t, kNReasons> order{};
  for (std::size_t i = 0; i < kNReasons; ++i) {
    order[i] = i;
  }
  std::stable_sort(order.begin(), order.end(),
                   [&tally](std::size_t a, std::size_t b) { return tally[a] > tally[b]; });

  std::string out =
      "(kept=" + std::to_string(n_kept) + " of " + std::to_string(provenance.size()) + "; rejected";
  bool any = false;
  for (const std::size_t slot : order) {
    if (tally[slot] == 0u) {
      break; // descending order: nothing after the first zero can be nonzero
    }
    out += any ? ", " : " ";
    out += obs_rejection_reason_name(static_cast<ObsRejectionReason>(slot));
    out += " x" + std::to_string(tally[slot]);
    any = true;
  }
  if (!any) {
    out += " none";
  }
  out += ")";
  return out;
}

// Sentinel written into a fit row's independent score column when anchor-
// independent scoring is requested but its raw-mid inversion failed or landed
// out of band. It is deliberately non-finite so every score consumer (all guard
// on `std::isfinite`) skips the row, and — critically — an unscorable score
// never removes the row from the FIT population. Fit survival is decided solely
// by the fit inversion (or the OTM shortcut).
inline constexpr double kUnscoredIv = std::numeric_limits<double>::quiet_NaN();

// w-space observation weight: vega² / (spread² + eps) / ((2σT)² + eps). Shared
// by the American builder (`build_observations`) and the de-Americanized
// builder (`build_observations_european`) so the two can never drift. The
// operand order is exactly what both sites evaluated inline, so the returned
// weight is bit-identical to the historical per-site arithmetic.
[[nodiscard]] double obs_weight_w(double vega, double spread, double sigma, double T) noexcept {
  const double denom_w = 2.0 * sigma * T;
  return (vega * vega) / (spread * spread + kWeightEps) / (denom_w * denom_w + kWeightEps);
}

// Per-row filter outcome. `Skipped` is the non-preferred leg that passed the
// flag + bid/ask gates but lost the prefer-call heuristic — the C `continue`
// that is NOT counted as a drop (unlike a genuine `Rejected`).
enum class RowOutcome : std::uint8_t { Accepted, Rejected, Skipped };

struct RowResult {
  RowOutcome outcome{RowOutcome::Rejected};
  ObsRejectionReason rejection{ObsRejectionReason::InvalidStrike};
  // T6: the leg carried no two-sided market — its bid was absent and had to be
  // projected onto the lower arbitrage bound. Set at step 2 and INDEPENDENT of
  // the outcome, because "this strike has no OTM market" is what arms the
  // ITM-leg fallback and that fact must survive a later quality rejection.
  bool one_sided{false};
  // T6: the row exists only because such a leg was admitted as a bound.
  // Meaningful only when `outcome == Accepted`; implies `one_sided`.
  bool bound{false};
  FitObs obs{};
};

// Round `x` UP onto a multiple of `tick`. A non-positive/non-finite tick
// disables the rounding rather than fabricating a grid. A value already within
// a part-per-billion of a tick multiple counts as ON it, so binary
// representation error cannot silently push a floor a whole tick higher.
[[nodiscard]] double ceil_to_tick(double x, double tick) noexcept {
  if (!std::isfinite(x) || !(x > 0.0)) {
    return 0.0;
  }
  if (!std::isfinite(tick) || !(tick > 0.0)) {
    return x;
  }
  const double ticks = x / tick;
  const double nearest = std::round(ticks);
  const double n = (std::fabs(ticks - nearest) <= 1.0e-9 * std::max(1.0, std::fabs(nearest)))
                       ? nearest
                       : std::ceil(ticks);
  const double up = n * tick;
  return (up >= x) ? up : (n + 1.0) * tick;
}

// T6: the price a bid-less leg's missing bid is projected onto — the Black-76
// lower arbitrage bound, rounded up to a quotable tick.
//
// The EUROPEAN bound is used even when the chain is American, where immediate
// exercise makes `max(0, S−K)` the true (higher) floor. That is deliberate and
// conservative in the only direction that matters: this builder inverts the row
// under Black-76 (`implied_vol(mid, F, K, T, df, side)`), so the bound it must
// respect is Black-76's own. Claiming the American floor here would assert more
// than the pricing model in use can support, and an American premium that is
// still below its intrinsic after projection is caught downstream by the de-Am
// inversion rather than laundered into a fit row.
[[nodiscard]] double projected_bid_floor(double K, double F, double df, Side side,
                                         double tick) noexcept {
  const double intrinsic = (side == Side::Call) ? (F - K) : (K - F);
  return ceil_to_tick(df * std::max(0.0, intrinsic), tick);
}

// Evaluate one (strike, side) tuple against the quote-filter cascade. Mirrors
// exactly one iteration of the C `ats_vol_svi_build_observations` inner loop
// (tenor buckets omitted → scalar max_spread_vol / min_vega_weight). The caller
// has already validated F/T/df and the chain SoA sizing.
//
// W1-B: `ignore_preference` disarms ONLY the prefer-OTM gate; every other
// filter is untouched. It is set exclusively by the ITM-leg fallback pass,
// which has already established that this strike's OTM leg carries no
// two-sided quote — so the gate can no longer be choosing between two usable
// legs, only discarding the one that is left.
//
// T6: `admit_bounds` arms the one-sided projection at step 2. The caller runs
// the whole cascade once with it OFF and re-runs with it ON only when the
// two-sided population left the slice below the usable-row floor, which is what
// confines bound admission to the starvation it exists to fix.
[[nodiscard]] RowResult evaluate_row(const Chain &chain, std::uint16_t strike_idx, Side side,
                                     double F, double T, double df, const CalibOpts &opts,
                                     bool ignore_preference = false, bool admit_bounds = false) {
  RowResult r{};
  const double K = chain.strikes[strike_idx];
  if (!(K > 0.0)) {
    return r; // Rejected (defensive; the strike loop already guards K > 0)
  }
  const std::size_t idx = chain_index(strike_idx, side);

  // 1. Kill-mask flags (Locked/Crossed/Stale/Halted/WideSpread/Penny/LowVega).
  constexpr QuoteFlag kill_mask = QuoteFlag::Locked | QuoteFlag::Crossed | QuoteFlag::Stale |
                                  QuoteFlag::Halted | QuoteFlag::WideSpread | QuoteFlag::Penny |
                                  QuoteFlag::LowVega;
  const auto flag = static_cast<QuoteFlag>(chain.flags[idx]);
  if (has_flag(flag, kill_mask)) {
    r.rejection = ObsRejectionReason::QuoteFlag;
    return r; // Rejected
  }

  // 2. Two-sided, positive, non-crossed quote — or, under `one_sided_bounds`, a
  //    bid-less one admitted as a BOUND with its bid projected onto the lower
  //    arbitrage bound. The rescue is deliberately narrow: it fires ONLY when
  //    the bid is absent (`bid <= 0`) and the ask is a real upper bound strictly
  //    above the arbitrage floor. `bid = ask = 0` (an absent side) bounds
  //    nothing; a locked or crossed market (`bid > 0`, `ask <= bid`) is two
  //    sides contradicting each other, not a bracket, and is already killed by
  //    the flag mask above on any loader-built chain.
  const double raw_bid = chain.bids[idx];
  const double ask = chain.asks[idx];
  double bid = raw_bid;
  bool bound = false;
  if (!(raw_bid > 0.0 && ask > raw_bid)) {
    const bool bounds_armed = opts.one_sided_bounds && admit_bounds;
    const double floor_bid =
        bounds_armed ? projected_bid_floor(K, F, df, side, opts.price_tick) : 0.0;
    r.one_sided = !(raw_bid > 0.0) && std::isfinite(ask) && ask > 0.0;
    if (!bounds_armed || raw_bid > 0.0 || !std::isfinite(ask) || !(ask > floor_bid)) {
      r.rejection = ObsRejectionReason::InvalidBidAsk;
      return r; // Rejected
    }
    bid = floor_bid;
    bound = true;
  }

  // Prefer-call heuristic: keep the call leg for K ≥ F, the put leg otherwise.
  // The non-preferred leg is silently skipped — NOT counted as a drop (this
  // gate deliberately sits AFTER the flag + bid/ask checks, matching the C, so
  // a flagged non-preferred leg still counts as a drop above).
  const bool prefer_call = (K >= F);
  if (!ignore_preference && prefer_call != (side == Side::Call)) {
    r.outcome = RowOutcome::Skipped;
    r.rejection = ObsRejectionReason::None;
    return r;
  }

  // 3. Positive mid. A bound row references the midpoint of its OWN admitted
  //    band [floor, ask], which coincides with the loader's `0.5*(bid + ask)`
  //    exactly when the arbitrage floor is zero (the OTM case) and lifts above
  //    it otherwise. A non-bound row keeps the stored mid bit-identically.
  const double mid = bound ? (0.5 * (bid + ask)) : chain.mids[idx];
  if (!(mid > 0.0)) {
    r.rejection = ObsRejectionReason::InvalidMid;
    return r; // Rejected
  }

  // 4. Wide-spread-to-mid filter (Sprint 24 Phase D); disabled when the cap ≤ 0.
  //    Skipped for a bound row: `(ask − floor)/mid` is a pure function of
  //    one-sidedness (identically 2.0 whenever the floor is 0), so it cannot
  //    discriminate a usable bid-less quote from an unusable one. The absolute
  //    vol-space gate at step 6 does that job, on the FULL admitted width.
  if (!bound && opts.max_spread_to_mid_pct > 0.0) {
    const double spread_pct = (ask - bid) / mid;
    if (spread_pct > opts.max_spread_to_mid_pct) {
      r.rejection = ObsRejectionReason::SpreadToMid;
      return r; // Rejected
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
    r.rejection = ObsRejectionReason::RawIvFailure;
    return r; // Rejected
  }
  const double iv = *iv_res;
  if (!(iv > kObsIvMin && iv < kObsIvMax)) {
    r.rejection = ObsRejectionReason::RawIvOutOfBand;
    return r; // Rejected
  }

  // Vega = F·df·φ(d1)·√T (identical to the C's hand-rolled pdf(d1) form).
  const double vega = black76_value_and_vega(F, K, T, iv, df, side).vega;
  const double spread = ask - bid;

  // 6. Wide spread in vol units.
  if (vega > kVegaFloor) {
    const double spread_vol = spread / vega;
    if (spread_vol > opts.max_spread_vol) {
      r.rejection = ObsRejectionReason::SpreadVol;
      return r; // Rejected
    }
  }

  // 7. w-space weight: weight_sigma = vega²/spread² is the filter quantity;
  //    weight_w = weight_sigma / (2σT)² is the value stored on the obs (built by
  //    the shared obs_weight_w helper — identical arithmetic to the de-Am path).
  const double weight_sigma = (vega * vega) / (spread * spread + kWeightEps);
  const double weight_w = std::min(obs_weight_w(vega, spread, iv, T), opts.max_weight);
  if (weight_sigma < opts.min_vega_weight) {
    r.rejection = ObsRejectionReason::LowVegaWeight;
    return r; // Rejected
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
  o.source_strike_index = strike_idx;
  o.score_sigma_mkt = iv;
  r.outcome = RowOutcome::Accepted;
  r.rejection = ObsRejectionReason::None;
  r.bound = bound;
  return r;
}

// True if `chain` carries the full 2·n_strikes SoA columns the cascade reads.
[[nodiscard]] bool chain_soa_well_formed(const Chain &chain) noexcept {
  const std::size_t need = 2u * chain.n_strikes();
  return chain.bids.size() >= need && chain.asks.size() >= need && chain.mids.size() >= need &&
         chain.flags.size() >= need;
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
  std::sort(sorted.begin(), sorted.end(), [](const FitObs &a, const FitObs &b) {
    return std::tie(a.k, a.source_strike_index) < std::tie(b.k, b.source_strike_index);
  });

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
      // Degenerate duplicate-strike segments have zero k-span everywhere.
      // Fill the requested minimum deterministically by the sorted
      // (k, source-index) order instead of returning only the two endpoints.
      for (std::size_t i = 0; i < m; ++i) {
        if (selected[i] == 0) {
          best = i;
          break;
        }
      }
      if (best == m) {
        break;
      }
    }
    selected[best] = 1;
    ++n_selected;
  }

  std::vector<FitObs> kept;
  kept.reserve(n_selected);
  for (std::size_t i = 0; i < m; ++i) {
    if (selected[i] != 0) {
      kept.push_back(sorted[i]);
    } else {
      const std::size_t source_index = sorted[i].source_strike_index;
      if (source_index < set.provenance.size()) {
        set.provenance[source_index].rejection = ObsRejectionReason::ObservationCap;
      }
    }
  }
  set.n_dropped += static_cast<std::uint32_t>(m - kept.size());
  set.obs = std::move(kept);
}

[[nodiscard]] bool use_otm_shortcut_deam(const FitObs &o, double S, double T, double r,
                                         double q_eff, const CalibOpts &opts, AmericanMethod method,
                                         DeAmAuditDiagnostics *diag) noexcept {
  if (!(opts.max_otm_shortcut_premium_spread_frac > 0.0) ||
      opts.anchor_kind != CalibAnchorKind::Mid || method != AmericanMethod::AndersenLake ||
      !(o.spread > 0.0) || !(o.sigma_mkt > kObsIvMin && o.sigma_mkt < kObsIvMax)) {
    return false;
  }
  if (T < opts.min_otm_shortcut_T) {
    if (diag != nullptr)
      ++diag->n_forced_short_tenor;
    return false;
  }
  if (o.vega < opts.min_otm_shortcut_vega) {
    if (diag != nullptr)
      ++diag->n_forced_low_vega;
    return false;
  }
  if (std::fabs(o.k) > opts.max_otm_shortcut_abs_k) {
    if (diag != nullptr)
      ++diag->n_forced_far_wing;
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

[[nodiscard]] InversionRouteDiagnostics &route_diag(DeAmAuditDiagnostics &diag,
                                                    IvRoute route) noexcept {
  switch (route) {
  case IvRoute::Shortcut:
    return diag.shortcut;
  case IvRoute::Cache:
    return diag.cache;
  case IvRoute::Fast:
    return diag.fast;
  case IvRoute::Accurate:
    return diag.accurate;
  }
  return diag.accurate;
}

void finalize_route_diag(InversionRouteDiagnostics &diag, std::vector<double> residuals) {
  if (residuals.empty())
    return;
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

// European contracts need no de-Americanization, but risk admission still
// requires evidence that every IV carried into the fit reprices its source mid
// inside the configured economic budget. Record the exact Black-76 inversion
// and its explicit forward reprice on the accurate route. This deliberately
// uses the existing audit ledger so downstream certification has one contract
// for both direct-European and American-to-European observation paths.
void audit_direct_european_inversions(const Chain &chain, double F, double T, double df,
                                      const CalibOpts &opts, ObsSet &set) {
  DeAmAuditDiagnostics audit;
  InversionRouteDiagnostics &route = audit.accurate;
  std::vector<double> residuals;
  residuals.reserve(set.obs.size());
  std::vector<FitObs> accepted;
  accepted.reserve(set.obs.size());

  for (const FitObs &row : set.obs) {
    ++audit.n_deam_rows;
    ++route.n_proposed;
    ++route.n_audited;
    ++route.n_reference_reprices;

    const std::size_t quote_index =
        chain_index(static_cast<std::uint16_t>(row.source_strike_index), row.side);
    const double source_mid = chain.mids[quote_index];
    const double spread = chain.asks[quote_index] - chain.bids[quote_index];
    const double repriced = black76_price(F, row.K, T, row.sigma_mkt, df, row.side);
    const double normalized = std::fabs(repriced - source_mid) / (0.5 * spread);
    if (std::isfinite(normalized)) {
      residuals.push_back(normalized);
    }
    if (std::isfinite(normalized) && normalized <= opts.max_inversion_residual_half_spreads) {
      ++route.n_accepted;
      ++audit.n_deam_accepted;
      accepted.push_back(row);
      continue;
    }

    ++audit.n_rejected_residual;
    ++set.n_dropped;
    set.provenance[row.source_strike_index].rejection = ObsRejectionReason::RawIvFailure;
  }

  finalize_route_diag(route, std::move(residuals));
  set.obs = std::move(accepted);
  set.deam_audit = std::move(audit);
}

inline constexpr std::uint16_t kSharedSigmaNodes = 9u;
inline constexpr std::size_t kSharedMinSideRows = 16u;
inline constexpr std::size_t kSharedMinAcceptedRows = 12u;
inline constexpr std::size_t kSharedLaneCapacity = 128u;
inline constexpr double kSharedMinT = 3.0 / 365.0;
inline constexpr double kSharedMinSigma = 0.01;

struct SharedIvLane {
  FitObs *observation{nullptr};
  detail::SharedLaneBracket bracket{};
  bool active{false};
};

// R-31. The McDonald-Schroder internal-put mapping these two used to re-derive
// inline now lives once, in detail::internal_put_coords (boundary_interp.hpp),
// behind the side-aware price_side/price_side_embedded entry points that
// slice_sigma_impl also calls. Same arithmetic, one definition.
[[nodiscard]] double shared_boundary_price(detail::SigmaBoundaryInterp &interp, const FitObs &o,
                                           double S, double sigma) noexcept {
  return interp.price_side(o.side, S, o.K, sigma);
}

[[nodiscard]] double shared_boundary_embedded_price(detail::SigmaBoundaryInterp &interp,
                                                    const FitObs &o, double S,
                                                    double sigma) noexcept {
  return interp.price_side_embedded(o.side, S, o.K, sigma);
}

[[nodiscard]] double shared_economic_price_budget(const FitObs &o, double T, double sigma,
                                                  const CalibOpts &opts) noexcept {
  const double deam_vega = black76_value_and_vega(o.F, o.K, T, sigma, o.df, o.side).vega;
  const double conservative_vega = std::min(o.vega, deam_vega);
  const double half_spread_budget =
      0.5 * o.spread * std::min(0.99, opts.max_inversion_residual_half_spreads);
  return std::min({0.005, 0.1 * conservative_vega * 1.0e-4, half_spread_budget});
}

[[nodiscard]] bool initialize_shared_lane(SharedIvLane &lane, detail::SigmaBoundaryInterp &interp,
                                          FitObs &o, double S) noexcept {
  lane.observation = &o;
  lane.bracket = detail::SharedLaneBracket{};
  lane.bracket.lo = interp.sigma_lo();
  lane.bracket.hi = o.sigma_mkt;
  if (!(lane.bracket.hi > lane.bracket.lo)) {
    return false;
  }
  const double price_lo = shared_boundary_price(interp, o, S, lane.bracket.lo);
  const double price_hi = shared_boundary_price(interp, o, S, lane.bracket.hi);
  lane.bracket.f_lo = price_lo - o.mid;
  lane.bracket.f_hi = price_hi - o.mid;
  lane.active = std::isfinite(lane.bracket.f_lo) && std::isfinite(lane.bracket.f_hi) &&
                lane.bracket.f_lo < 0.0 && lane.bracket.f_hi >= 0.0 && price_hi >= price_lo;
  return lane.active;
}

void iterate_shared_lanes(std::span<SharedIvLane> lanes, detail::SigmaBoundaryInterp &interp,
                          double S, double solve_tol, std::uint16_t max_iter) noexcept {
  for (std::uint16_t iteration = 0u; iteration < max_iter; ++iteration) {
    bool any_active = false;
    for (SharedIvLane &lane : lanes) {
      if (!lane.active) {
        continue;
      }
      if (lane.bracket.hi - lane.bracket.lo <= solve_tol) {
        lane.active = false;
        continue;
      }
      any_active = true;
      const double sigma = lane.bracket.next_sigma();
      const double residual =
          shared_boundary_price(interp, *lane.observation, S, sigma) - lane.observation->mid;
      if (!std::isfinite(residual)) {
        lane.active = false;
        lane.observation = nullptr;
      } else {
        lane.bracket.update(sigma, residual);
      }
    }
    if (!any_active) {
      break;
    }
  }
}

[[nodiscard]] bool finalize_shared_lane(SharedIvLane &lane, detail::SigmaBoundaryInterp &interp,
                                        double S, double T, double solve_tol,
                                        const CalibOpts &opts) noexcept {
  if (lane.observation == nullptr || lane.bracket.hi < lane.bracket.lo ||
      lane.bracket.hi - lane.bracket.lo > solve_tol) {
    return false;
  }
  const double sigma = 0.5 * (lane.bracket.lo + lane.bracket.hi);
  const double price = shared_boundary_price(interp, *lane.observation, S, sigma);
  const double embedded = shared_boundary_embedded_price(interp, *lane.observation, S, sigma);
  const double budget = shared_economic_price_budget(*lane.observation, T, sigma, opts);
  if (!(sigma > kObsIvMin && sigma < kObsIvMax) ||
      !detail::shared_lane_residual_within_budget(price, lane.observation->mid, embedded, budget)) {
    return false;
  }
  lane.observation->score_sigma_mkt = sigma;
  return true;
}

// `shortcut_mask` is parallel to `observations`: a non-zero entry marks a row the
// OTM shortcut has already claimed. Such a row is never opened as a shared lane —
// the main loop's per-row priority is `shortcut -> shared_proposal -> scalar`, so a
// lane for it would be solved and then discarded, and its accepted-rank position
// would displace a real lane from the bounded sentinel certification.
[[nodiscard]] std::size_t solve_shared_side(std::vector<FitObs> &observations,
                                            std::span<const std::uint8_t> shortcut_mask, Side side,
                                            double S, double T,
                                            detail::SigmaBoundaryInterp &interp, double iv_tol,
                                            std::uint16_t max_iter,
                                            const CalibOpts &opts) noexcept {
  const double solve_tol = std::min(std::max(iv_tol, 1.0e-9), 2.5e-5);
  std::size_t accepted = 0u;
  std::size_t scan = 0u;
  while (scan < observations.size()) {
    std::array<SharedIvLane, kSharedLaneCapacity> storage{};
    std::size_t count = 0u;
    while (scan < observations.size() && count < storage.size()) {
      const std::size_t index = scan++;
      FitObs &observation = observations[index];
      if (observation.side == side && shortcut_mask[index] == 0u) {
        if (!initialize_shared_lane(storage[count], interp, observation, S)) {
          storage[count].observation = nullptr;
        }
        ++count;
      }
    }
    std::span<SharedIvLane> lanes{storage.data(), count};
    iterate_shared_lanes(lanes, interp, S, solve_tol, max_iter);
    for (SharedIvLane &lane : lanes) {
      if (finalize_shared_lane(lane, interp, S, T, solve_tol, opts)) {
        ++accepted;
      }
    }
  }
  return accepted;
}

[[nodiscard]] FitObs *shared_accepted_at(std::vector<FitObs> &observations, Side side,
                                         std::size_t rank) noexcept {
  for (FitObs &observation : observations) {
    if (observation.side == side && std::isfinite(observation.score_sigma_mkt)) {
      if (rank == 0u) {
        return &observation;
      }
      --rank;
    }
  }
  return nullptr;
}

[[nodiscard]] bool certify_shared_side(std::vector<FitObs> &observations, Side side, double S,
                                       double T, double r, double q_eff, std::size_t accepted,
                                       double iv_tol, std::uint16_t iv_max_iter,
                                       const CalibOpts &opts, DeAmAuditDiagnostics &diag) noexcept {
  if (accepted == 0u) {
    return false;
  }
  const std::array<std::size_t, 3> ranks{0u, accepted / 2u, accepted - 1u};
  const std::size_t sentinel_count = std::min<std::size_t>(accepted, ranks.size());
  for (std::size_t sentinel = 0u; sentinel < sentinel_count; ++sentinel) {
    const std::size_t rank = sentinel_count == 1u
                                 ? ranks[0]
                                 : (sentinel_count == 2u ? ranks[2u * sentinel] : ranks[sentinel]);
    FitObs *const observation = shared_accepted_at(observations, side, rank);
    if (observation == nullptr) {
      return false;
    }
    ++diag.n_shared_sentinel_reprices;
    const Result<double> reference = american_implied_vol(
        observation->mid, S, observation->K, T, r, q_eff, side, AmericanMethod::AndersenLake,
        std::min(iv_tol, 1.0e-7), std::max(iv_max_iter, std::uint16_t{64}), std::nullopt, nullptr);
    if (!reference || std::fabs(*reference - observation->score_sigma_mkt) > 1.0e-4) {
      return false;
    }
    const Result<double> reference_price =
        american_price(S, observation->K, T, observation->score_sigma_mkt, r, q_eff, side,
                       AmericanMethod::AndersenLake, std::nullopt);
    const double budget =
        shared_economic_price_budget(*observation, T, observation->score_sigma_mkt, opts);
    if (!reference_price || !(budget > 0.0) ||
        std::fabs(*reference_price - observation->mid) > budget) {
      return false;
    }
  }
  return true;
}

void invalidate_shared_side(std::vector<FitObs> &observations, Side side) noexcept {
  for (FitObs &observation : observations) {
    if (observation.side == side) {
      observation.score_sigma_mkt = kUnscoredIv;
    }
  }
}

void prepare_shared_boundary_side(std::vector<FitObs> &observations,
                                  std::span<const std::uint8_t> shortcut_mask, Side side, double S,
                                  double T, double r, double q_eff, const CalibOpts &opts,
                                  const std::optional<AlOpts> &al_opts, double iv_tol,
                                  std::uint16_t iv_max_iter, DeAmAuditDiagnostics &diag) noexcept {
  // Rows the OTM shortcut has claimed are excluded from the population this
  // interpolant serves. Two numeric consequences, both intentional:
  //   * `side_rows` (vs kSharedMinSideRows) now counts only rows that can
  //     actually become lanes, so the "is this side wide enough to amortise nine
  //     boundary builds" test is answered about the real beneficiaries rather
  //     than being inflated by rows that will never touch the interpolant.
  //   * The [sigma_lo, sigma_hi] bracket is spanned by the non-shortcut seeds
  //     only. Every lane sets hi = its own sigma_mkt and lo = interp.sigma_lo(),
  //     so each solve stays inside the built domain; dropping shortcut seeds can
  //     only tighten that domain, never widen it, which holds or improves the
  //     nine-node density and hence accuracy. The domain is still certified by
  //     the unchanged sentinel block below.
  alprobe::bump(alprobe::Event::SharedSideConsidered);
  std::size_t side_rows = 0u;
  double min_seed = std::numeric_limits<double>::infinity();
  double max_seed = 0.0;
  for (std::size_t index = 0; index < observations.size(); ++index) {
    const FitObs &observation = observations[index];
    if (observation.side == side && shortcut_mask[index] == 0u) {
      ++side_rows;
      min_seed = std::min(min_seed, observation.sigma_mkt);
      max_seed = std::max(max_seed, observation.sigma_mkt);
    }
  }
  // (rate, yield) half of the McDonald-Schroder duality, single-sourced via
  // detail::internal_put_rates (see boundary_interp.hpp) alongside the
  // (Sp, Kp) half that price_side/price_side_embedded already use.
  const detail::InternalPutRates rates = detail::internal_put_rates(side, r, q_eff);
  const double internal_rate = rates.rp;
  if (side_rows < kSharedMinSideRows || !(T >= kSharedMinT) || !(internal_rate > 0.0)) {
    alprobe::bump(alprobe::Event::SharedSideGuardSkip);
    alprobe::bump(alprobe::Event::SharedRowsFallback, side_rows);
    return;
  }
  const double sigma_lo = std::max(kSharedMinSigma, 0.35 * min_seed);
  const double sigma_hi = std::min(kObsIvMax, max_seed * (1.0 + 1.0e-12));
  if (!(sigma_hi > sigma_lo) || sigma_hi / sigma_lo > 20.0) {
    alprobe::bump(alprobe::Event::SharedSideGuardSkip);
    alprobe::bump(alprobe::Event::SharedRowsFallback, side_rows);
    return;
  }
  const double internal_yield = rates.qp;
  detail::SigmaBoundaryInterp interp;
  const amer::AlScheme scheme = amer::scheme_from_opts(al_opts);
  if (!interp.build(S, T, internal_rate, internal_yield, sigma_lo, sigma_hi, kSharedSigmaNodes,
                    scheme)) {
    diag.n_shared_scalar_fallback_lanes += static_cast<std::uint32_t>(side_rows);
    alprobe::bump(alprobe::Event::SharedSideBuildFail);
    alprobe::bump(alprobe::Event::SharedRowsFallback, side_rows);
    return;
  }
  diag.n_shared_boundary_solves += kSharedSigmaNodes;
  const std::size_t accepted =
      solve_shared_side(observations, shortcut_mask, side, S, T, interp, iv_tol, iv_max_iter, opts);
  const std::uint32_t sentinels_before = diag.n_shared_sentinel_reprices;
  const bool certified = accepted >= kSharedMinAcceptedRows &&
                         certify_shared_side(observations, side, S, T, r, q_eff, accepted, iv_tol,
                                             iv_max_iter, opts, diag);
  const std::uint32_t sentinel_count = diag.n_shared_sentinel_reprices - sentinels_before;
  const IvRoute shared_route = al_opts.has_value() ? IvRoute::Fast : IvRoute::Accurate;
  route_diag(diag, shared_route).n_reference_reprices += sentinel_count;
  if (!certified) {
    invalidate_shared_side(observations, side);
    diag.n_shared_scalar_fallback_lanes += static_cast<std::uint32_t>(side_rows);
    alprobe::bump(alprobe::Event::SharedSideRejected);
    alprobe::bump(alprobe::Event::SharedRowsFallback, side_rows);
    return;
  }
  alprobe::bump(alprobe::Event::SharedSideCertified);
  alprobe::bump(alprobe::Event::SharedRowsLaned, accepted);
  alprobe::bump(alprobe::Event::SharedRowsFallback, side_rows - accepted);
  diag.n_shared_boundary_lanes += static_cast<std::uint32_t>(accepted);
  diag.n_shared_scalar_fallback_lanes += static_cast<std::uint32_t>(side_rows - accepted);
  if (side == Side::Call) {
    diag.n_shared_call_lanes += static_cast<std::uint32_t>(accepted);
  } else {
    diag.n_shared_put_lanes += static_cast<std::uint32_t>(accepted);
  }
}

void prepare_shared_boundary_proposals(std::vector<FitObs> &observations,
                                       std::span<const std::uint8_t> shortcut_mask, double S,
                                       double T, double r, double q_eff, const CalibOpts &opts,
                                       const AmericanCorrectionCaches &caches,
                                       const std::optional<AlOpts> &al_opts, double iv_tol,
                                       std::uint16_t iv_max_iter, AmericanMethod method,
                                       DeAmAuditDiagnostics &diag) noexcept {
  // This is the private, pre-restatement ObsSet. Its score column is recomputed
  // below for every emitted row, so it safely carries the transient proposal
  // and avoids allocating a parallel n-strike vector in the hot path. The
  // caller does allocate one such vector — `shortcut_mask` at :897 — but that
  // one is required by the prepare/main-loop single-sourcing contract (both
  // passes must agree on which rows the shortcut claims) and costs only 1
  // byte/row; the score column here stays in-band for the same reason as
  // before, since it needs no vector of its own at all.
  for (FitObs &observation : observations) {
    observation.score_sigma_mkt = kUnscoredIv;
  }
  // W3.1 wiring. Two former bails are deliberately NOT tested here:
  //
  //   * `max_otm_shortcut_premium_spread_frac > 0.0` — the OTM shortcut and the
  //     shared boundary are per-ROW alternatives, not board-level exclusives.
  //     Disabling the whole board because SOME rows shortcut left every live
  //     Hft/Configured board (SPY -> IndexEtfUltraLiquid -> Hft sets 0.50) on the
  //     scalar inverter. `shortcut_mask` now carves the shortcut's rows out of
  //     the shared population, so the two routes coexist and the per-row priority
  //     `shortcut -> shared_proposal -> scalar` is unchanged.
  //
  //   * `q_eff < 0.0` — this bailed the whole board, but only the CALL side's
  //     internal rate IS q_eff. The PUT side's internal regime is (rate = r,
  //     yield = q_eff), so r > 0 with slightly negative q_eff (a hard-to-borrow
  //     single name) is a regular single-boundary American-put regime that the
  //     interpolant models exactly. The per-side `internal_rate > 0.0` test still
  //     excludes the call side there, and build()'s al_xmax_put(...) > 0.0 test
  //     excludes any genuinely non-American corner, so the guard was strictly
  //     redundant for puts and wrong to apply board-wide.
  //
  // `r < 0.0` is retained: it flips the PUT side's internal rate negative, where
  // al_xmax_put(K, r<0, q>=0) == 0 leaves no asymptotic boundary to interpolate.
  // That covers only the q_eff >= 0 sub-case of r < 0; al_xmax_put also
  // supports r < 0 with q_eff < r < 0 (american.cpp:560-562: `if (r < 0.0 &&
  // q < r) return K;`), a real single-boundary regime with a nonzero
  // asymptote. This board-wide bail excludes that regime too — a deliberate,
  // conservative choice, out of scope for R-09, not a gap in this guard's
  // reasoning.
  if (!opts.use_shared_boundary_deam || opts.audit_accurate_inversions ||
      opts.anchor_kind != CalibAnchorKind::Mid || method != AmericanMethod::AndersenLake ||
      r < 0.0 || !(iv_tol > 0.0) || iv_max_iter == 0u) {
    return;
  }
  if (shortcut_mask.size() != observations.size()) {
    return; // fail closed: without the mask a shortcut row could become a lane
  }
  for (const Side side : {Side::Put, Side::Call}) {
    const CorrectionCache *const cache = caches.for_side(side);
    if (cache != nullptr && cache->populated() && cache->side() == side) {
      continue;
    }
    prepare_shared_boundary_side(observations, shortcut_mask, side, S, T, r, q_eff, opts, al_opts,
                                 iv_tol, iv_max_iter, diag);
  }
}

} // namespace

namespace detail {

double SharedLaneBracket::next_sigma() const noexcept {
  const double middle = 0.5 * (lo + hi);
  // Bisection backstop. This REPLACES the former "reject any secant landing in
  // the outer 25% of the bracket" trust region -- see update() for why that guard
  // had to go, and kMaxSecantSteps for the bound this carries in its place.
  if (steps >= kMaxSecantSteps) {
    return middle;
  }
  const double residual_span = f_hi - f_lo;
  if (!(residual_span > 0.0) || !std::isfinite(residual_span)) {
    return middle;
  }
  const double secant = lo - f_lo * (hi - lo) / residual_span;
  // Any probe strictly inside the bracket is admissible. A step landing ON an
  // endpoint (or off it, via round-off) would make no progress, so it bisects.
  if (!(secant > lo && secant < hi)) {
    return middle;
  }
  return secant;
}

void SharedLaneBracket::update(double sigma, double residual) noexcept {
  // PRECONDITION: `residual` must be finite. `residual < 0.0` is false for NaN,
  // so a non-finite residual falls into the `else` (hi) branch below and writes
  // NaN over `f_hi`, breaking the `f_lo < 0 <= f_hi` invariant this type does not
  // itself re-check. The sole production caller (iterate_shared_lanes, above)
  // already gates on std::isfinite(residual) before calling update; any other
  // caller of this exposed detail:: type must do the same.
  //
  // R-11a. Numerically: the retained endpoint's stored residual is now HALVED
  // whenever that same endpoint survives two updates in a row (the Illinois
  // modification of false position); previously both stored residuals were always
  // the exact residuals at the endpoints.
  //
  // Why it is correct. `f_lo`/`f_hi` are consumed by exactly one expression --
  // the secant in `next_sigma()` -- and only through their SIGNS (for the
  // invariant) and their RATIO (for the crossing point). Halving preserves the
  // sign, so the bracket invariant `f_lo < 0 <= f_hi` is untouched and the root
  // stays enclosed; `finalize_shared_lane` re-prices from scratch at the accepted
  // sigma and never reads either field, so a deflated residual cannot leak into
  // an accepted price. What halving changes is only WHICH point inside the
  // bracket is probed next.
  //
  // Why it is needed. A lane's root hugs `hi` (hi = sigma_mkt is the Black-76 iv
  // of an American mid, which overstates the American iv being solved for), and
  // the map is convex in sigma there, so the plain secant always undershoots and
  // `hi` is retained forever: the bracket creeps up from `lo` and its WIDTH --
  // the quantity `hi - lo <= solve_tol` terminates on -- contracts only
  // geometrically. Deflating the stagnant endpoint's residual pulls the next
  // secant toward it, which is what makes the width collapse superlinearly
  // (order ~1.44) instead.
  //
  // Bound it holds: the accepted sigma is unchanged in kind -- termination is
  // still `hi - lo <= solve_tol` on a bracket that still encloses the root, so
  // the converged sigma stays within solve_tol/2 of the true root exactly as
  // before.
  ++steps;
  if (residual < 0.0) {
    lo = sigma;
    f_lo = residual;
    if (retained > 0) {
      f_hi *= 0.5; // `hi` retained twice running -> deflate it
    }
    retained = 1;
  } else {
    hi = sigma;
    f_hi = residual;
    if (retained < 0) {
      f_lo *= 0.5; // `lo` retained twice running -> deflate it
    }
    retained = -1;
  }
  // R-11a, second numeric change: the former safeguard rejected any secant
  // landing in the outer quarter of the bracket and bisected instead, which made
  // every iteration contract the width by >= 25% and bounded the iteration count
  // constructively. That guard CANNOT coexist with Illinois, and removing it is
  // what actually buys the speedup. Deflating the stagnant endpoint works
  // precisely BY aiming the next secant at that endpoint -- i.e. into the outer
  // quarter -- so the guard rejected exactly the steps Illinois exists to take.
  // Measured on the steep-smile lane fixture (mean evals/lane, worst):
  //     falsi + 25% guard (the pre-change path)  22.50 / 24   <- baseline
  //     Illinois + 25% guard                     22.66 / 25   <- a REGRESSION
  //     Illinois, guard replaced per below        5.19 / 11
  // The middle row is the point: keeping the guard as Illinois' "fallback", as
  // one might expect to be the conservative choice, silently discards every
  // deflated step and costs more than it saves.
  //
  // The bound the guard carried is re-established by kMaxSecantSteps' bisection
  // backstop (see calib.hpp) rather than by a per-step trust region. Compared
  // under MATCHED premises (same w0, same solve_tol -- the old guard's own
  // shrink-per-step is w-independent, so its count is exact, not a bound), the
  // new backstop is tighter on every premise, not merely "unchanged":
  //     worst case  (w0 = kObsIvMax - kSharedMinSigma = 4.99, tol = 1e-9):
  //         new 24 + ceil(log2(4.99/1e-9))   = 24 + 33 = 57
  //         old ceil(log(1e-9/4.99)/log(0.75))    = 78   <- EXCEEDS max_iter = 64
  //     default config (tol = 1e-7, same w0 = 4.99):
  //         new 24 + ceil(log2(4.99/1e-7))   = 24 + 26 = 50
  //         old ceil(log(1e-7/4.99)/log(0.75))    = 62
  //     typical bracket (w0 = 0.75, tol = 1e-7):
  //         new 24 + ceil(log2(0.75/1e-7))   = 24 + 23 = 47
  //         old ceil(log(1e-7/0.75)/log(0.75))    = 56
  // So the old guard did not actually deliver a bound inside max_iter = 64 at the
  // true worst case; the new backstop does, with margin, and is cheaper at every
  // matched premise besides.
  //
  // Nothing about acceptance moves: the bracket still encloses the root, the loop
  // still terminates on `hi - lo <= solve_tol`, and a lane that somehow fails to
  // reach solve_tol within max_iter is still rejected by finalize_shared_lane and
  // served by the exact scalar inverter. Only the probe SEQUENCE changes.
}

bool shared_lane_residual_within_budget(double price, double mid, double embedded,
                                        double budget) noexcept {
  if (!std::isfinite(price) || !std::isfinite(mid) || !std::isfinite(embedded) ||
      !(budget > 0.0)) {
    return false;
  }
  // R-07. Numerically: the two residuals were previously compared against
  // `budget` one at a time; they are now compared as a SUM, so this gate is
  // strictly stronger and can only reject lanes the old one accepted (it never
  // admits a new one). Correct because the sum, not either term, is what bounds
  // the true price error: |price_true - mid| <= |price_true - price| +
  // |price - mid|, and the 9-vs-5 gap |price - embedded| is this route's
  // estimate of the first term. Bound now proven per accepted lane:
  //     |price_true(sigma_hat) - mid| <~ budget
  //                                    = min(0.005, 0.1 x vega x 1e-4,
  //                                          0.5 x spread x half_spread_frac),
  // where the old form proved only <~ 2 x budget. Measured on the smile-stress
  // and flat fixtures the two terms run ~1e-6 against budgets ~1e-4..5e-3, so no
  // live lane changes route; this closes the proof, it does not move prices.
  return std::fabs(price - mid) + std::fabs(price - embedded) <= budget;
}

} // namespace detail

CalibOpts calib_default_opts() noexcept { return CalibOpts{}; }

Status validate_calib_options(const CalibOpts &opts) noexcept {
  if (!std::isfinite(opts.max_weight) || !(opts.max_weight > 0.0)) {
    return Err(ErrorCode::InvalidArgument,
               "validate_calib_options: max_weight must be finite and positive");
  }

  switch (opts.loss_kind) {
  case CalibLossKind::Mid:
    break;
  case CalibLossKind::Interval:
    return Err(ErrorCode::NotImplemented,
               "validate_calib_options: parametric interval loss is not implemented");
  default:
    return Err(ErrorCode::InvalidArgument, "validate_calib_options: invalid calibration loss kind");
  }
  switch (opts.essvi_rho_mode) {
  case EssviRhoMode::PerSlice:
    break;
  case EssviRhoMode::Shared:
  case EssviRhoMode::TermStructure:
    return Err(ErrorCode::NotImplemented,
               "validate_calib_options: requested eSSVI rho mode is not implemented");
  default:
    return Err(ErrorCode::InvalidArgument, "validate_calib_options: invalid eSSVI rho mode");
  }
  if (opts.essvi_asymmetric_rho) {
    return Err(ErrorCode::NotImplemented,
               "validate_calib_options: asymmetric eSSVI rho is not implemented");
  }

  if (opts.essvi_fallback_rmse_threshold != kDefaultEssviFallbackRmse) {
    return Err(ErrorCode::NotImplemented,
               "validate_calib_options: configurable eSSVI fallback threshold is not implemented");
  }
  if (opts.n_butterfly_grid != kDefaultButterflyGrid) {
    return Err(ErrorCode::NotImplemented,
               "validate_calib_options: configurable butterfly grid is not implemented");
  }

  switch (opts.residual_basis_kind) {
  case ResidualBasisKind::None:
    // A persisted config must carry no no-ops: an explicitly ENABLED residual
    // layer with a None basis fits nothing. The default config leaves the layer
    // disabled (residual_disable == true), so this rejects only the contradiction.
    if (!opts.residual_disable) {
      return Err(ErrorCode::InvalidArgument,
                 "validate_calib_options: residual layer enabled with a None basis is a no-op");
    }
    if (opts.residual_n_basis_terms != 0u) {
      return Err(ErrorCode::InvalidArgument,
                 "validate_calib_options: None residual requires zero basis terms");
    }
    break;
  case ResidualBasisKind::HingeQuad:
    if (opts.residual_n_basis_terms != 0u && opts.residual_n_basis_terms != 5u) {
      return Err(ErrorCode::InvalidArgument,
                 "validate_calib_options: HingeQuad residual requires 0 or 5 basis terms");
    }
    break;
  case ResidualBasisKind::C2Bspline:
    if (opts.residual_n_basis_terms != 0u &&
        (opts.residual_n_basis_terms < 5u || opts.residual_n_basis_terms > 16u)) {
      return Err(ErrorCode::InvalidArgument,
                 "validate_calib_options: C2 residual requires 0 or 5..16 basis terms");
    }
    break;
  case ResidualBasisKind::Chebyshev:
  case ResidualBasisKind::WingBspline:
  case ResidualBasisKind::Fengler:
    return Err(ErrorCode::NotImplemented,
               "validate_calib_options: requested residual basis is not implemented");
  default:
    return Err(ErrorCode::InvalidArgument, "validate_calib_options: invalid residual basis kind");
  }
  return Ok();
}

Result<ObsSet> build_observations(const Chain &chain, double F, double T, double df,
                                  const CalibOpts &opts) {
  if (!(F > 0.0) || !(T > 0.0)) {
    return Err(ErrorCode::InvalidArgument, "build_observations: F and T must be positive");
  }
  if (!chain_soa_well_formed(chain)) {
    return Err(ErrorCode::InvalidArgument,
               "build_observations: chain SoA arrays shorter than 2*n_strikes");
  }
  if (!std::isfinite(opts.max_weight) || !(opts.max_weight > 0.0)) {
    return Err(ErrorCode::InvalidArgument,
               "build_observations: max_weight must be finite and positive");
  }
  if (chain.exercise_style == ExerciseStyle::European &&
      (!std::isfinite(opts.max_inversion_residual_half_spreads) ||
       opts.max_inversion_residual_half_spreads < 0.0)) {
    return Err(ErrorCode::InvalidArgument,
               "build_observations: European inversion residual budget must be finite and "
               "non-negative");
  }

  const std::size_t n = chain.n_strikes();
  constexpr std::size_t kMaxIndexedStrikes =
      static_cast<std::size_t>(std::numeric_limits<std::uint16_t>::max()) + 1u;
  if (n > kMaxIndexedStrikes) {
    return Err(ErrorCode::InvalidArgument,
               "build_observations: more strikes than the source key can represent");
  }
  ObsSet out;
  out.obs.reserve(n); // at most one accepted (preferred) leg per strike
  out.provenance.reserve(n);
  std::uint32_t n_drop = 0;

  // T6: bound admission is a LAST RESORT, on the same design as the ITM-leg
  // fallback above. `collect` runs the whole cascade once with bound rows
  // refused; only if that leaves the slice below the usable-row floor does it
  // run again with them admitted.
  //
  // Measured, not assumed. Admitting bounds unconditionally on lqbench
  // 2026-08-03 moved boards that were ALREADY serving every expiry they could:
  // +425 quotes but IWM's rmse_vol x1.44, MSFT x1.19, VZ x1.21, and one board
  // (ROKU) lost outright. That is the wrong trade in both directions — a board
  // with 1,500 marked quotes gains nothing from a guess, and a bound row is
  // strictly weaker evidence than a two-sided quote at the same width because
  // its reference price is the midpoint of a band whose floor is pure
  // arbitrage. Gating on the floor makes the change provably inert wherever the
  // two-sided population already prepares a slice, so the recovery is confined
  // to expiries that were being LOST.
  const auto collect = [&](bool admit_bounds) {
  for (std::size_t s = 0; s < n; ++s) {
    const double K = chain.strikes[s];
    if (!(K > 0.0)) {
      ++n_drop; // matches the C strike-level drop (counted once per strike)
      out.provenance.push_back(ObsProvenance{static_cast<std::uint32_t>(s), Side::Put,
                                             ObsRejectionReason::InvalidStrike});
      continue;
    }
    const auto sidx = static_cast<std::uint16_t>(s);
    const Side preferred = (K >= F) ? Side::Call : Side::Put;
    const Side other = (preferred == Side::Call) ? Side::Put : Side::Call;
    // At most ONE leg per strike can be Accepted: the preference gate turns the
    // non-preferred leg into `Skipped` before it can reach acceptance, so the
    // chosen row can be settled after both legs have been judged rather than
    // pushed inside the loop. That deferral is what lets the ITM-leg fallback
    // below OUTRANK a bound row the preferred leg already produced.
    RowResult preferred_row{};
    RowOutcome other_outcome = RowOutcome::Rejected;
    for (int side_i = 0; side_i < 2; ++side_i) {
      const Side side = static_cast<Side>(static_cast<std::uint8_t>(side_i));
      const RowResult rr = evaluate_row(chain, sidx, side, F, T, df, opts, false, admit_bounds);
      if (rr.outcome == RowOutcome::Rejected) {
        ++n_drop;
      }
      if (side == preferred) {
        preferred_row = rr;
      } else {
        other_outcome = rr.outcome;
      }
      // Skipped: the non-preferred leg — not counted (C bare `continue`).
    }

    // W1-B (F21): the OTM leg carries no two-sided quote, but the ITM leg does
    // (it reached the preference gate, i.e. `Skipped`). Re-run that leg through
    // the SAME cascade with only the preference gate disarmed, and take it in
    // place of the strike's missing OTM evidence. Nothing else about the row is
    // special-cased: an ITM leg that cannot be inverted, is too wide in vol
    // units, or carries too little vega is rejected exactly as an OTM leg would
    // be, and the strike stays dead.
    //
    // T6 keeps the trigger meaning exactly what it always meant — "this strike
    // has no OTM market" — by reading `one_sided`, which step 2 records BEFORE
    // the quality cascade. Reading the rejection reason instead would silently
    // disarm the rescue the moment bound admission renamed the rejection.
    //
    // What T6 adds is a ranking, because the fallback can now be offered two
    // usable rows instead of one. Both legs of a strike share a vega, so the
    // narrower band is the sharper observation in vol units: a two-sided ITM
    // market always displaces a full-ask-wide OTM bound, while an ITM leg that
    // is ITSELF bid-less only steps in when the OTM leg produced nothing at all.
    const bool preferred_lacks_market =
        preferred_row.one_sided || (preferred_row.outcome == RowOutcome::Rejected &&
                                    preferred_row.rejection == ObsRejectionReason::InvalidBidAsk);
    RowResult chosen = preferred_row;
    Side used_side = preferred;
    ObsRejectionReason used_rejection = preferred_row.rejection;
    if (opts.itm_leg_fallback && preferred_lacks_market &&
        other_outcome == RowOutcome::Skipped) {
      const RowResult fb = evaluate_row(chain, sidx, other, F, T, df, opts,
                                        /*ignore_preference=*/true, admit_bounds);
      const bool preferred_accepted = preferred_row.outcome == RowOutcome::Accepted;
      if (fb.outcome == RowOutcome::Accepted && (!fb.bound || !preferred_accepted)) {
        chosen = fb;
        used_side = other;
        used_rejection = fb.rejection;
        ++out.n_itm_fallback;
      } else if (!preferred_accepted) {
        // The strike is dead for a NEW reason — the ITM leg's own — which the
        // first pass never evaluated. Count it, so `n_dropped` still equals the
        // number of legs this builder judged and threw away.
        used_side = other;
        used_rejection = fb.rejection;
        ++n_drop;
      }
      // Otherwise the OTM bound stands: the ITM leg offered no sharper market.
    }
    if (chosen.outcome == RowOutcome::Accepted) {
      out.obs.push_back(chosen.obs);
      if (chosen.bound) {
        ++out.n_bound_admitted;
      }
    }
    out.provenance.push_back(
        ObsProvenance{static_cast<std::uint32_t>(s), used_side, used_rejection});
  }
  };

  collect(/*admit_bounds=*/false);
  const std::size_t n_two_sided = out.obs.size();
  if (opts.one_sided_bounds && n_two_sided >= kMinBoundAnchorRows && n_two_sided < kMinObs) {
    out.obs.clear();
    out.provenance.clear();
    out.n_itm_fallback = 0u;
    out.n_bound_admitted = 0u;
    n_drop = 0u;
    collect(/*admit_bounds=*/true);
  }

  out.n_dropped = n_drop;
  if (out.provenance.size() != n) {
    return Err(ErrorCode::Internal, "build_observations: preferred-row provenance is incomplete");
  }
  if (chain.exercise_style == ExerciseStyle::European) {
    audit_direct_european_inversions(chain, F, T, df, opts, out);
  }
  if (out.obs.size() < kMinObs) {
    return Err(ErrorCode::NotFound, "build_observations: fewer than 5 observations survived " +
                                        describe_rejections(out.obs.size(), out.provenance));
  }
  return Ok(std::move(out));
}

Result<ObsSet> build_observations_european(const Chain &chain, double S, double r, double F,
                                           double T, double df, const CalibOpts &opts,
                                           const AmericanCorrectionCaches &caches,
                                           const std::optional<AlOpts> &al_opts, double iv_tol,
                                           std::uint16_t iv_max_iter, AmericanMethod method,
                                           bool prepare_scoring) {
  if (!(S > 0.0) || !(F > 0.0) || !(T > 0.0) || !(df > 0.0) || !std::isfinite(r)) {
    return Err(ErrorCode::InvalidArgument,
               "build_observations_european: S, F, T, df must be positive");
  }
  // Run the shared American filter cascade, then strip each surviving leg.
  auto am = build_observations(chain, F, T, df, opts);
  if (!am.has_value()) {
    return am; // propagate NotFound / InvalidArgument unchanged
  }
  cap_observations_for_deam(*am, opts.max_obs_per_slice);
  // q_eff bridge: S·e^{(r−q_eff)T} == F exactly (matches the fit/de-Am carry).
  const double q_eff = r - std::log(F / S) / T;

  ObsSet out;
  out.obs.reserve(am->obs.size());
  out.provenance = std::move(am->provenance);
  out.n_dropped = am->n_dropped;
  // Admission tallies belong to the shared American cascade that produced this
  // population, so they carry across the de-Americanization unchanged. They
  // count legs the FILTER admitted; the de-Am audit below may still drop some,
  // exactly as it may drop an ordinary two-sided row.
  out.n_itm_fallback = am->n_itm_fallback;
  out.n_bound_admitted = am->n_bound_admitted;
  // Chain strikes are ascending. Independent side seeds therefore follow a
  // nearby point on the smile without ever crossing call/put solver state. A
  // tolerance-terminated seed may move the last bits; the cold-reference gate
  // requires |dIV| <= 1e-4 and |dPrice| <= min(half tick, 0.1*vega*1e-4), while
  // remaining strictly inside the source quote's half-spread.
  const bool warm_start_deam = opts.warm_start_deam_adjacent_strikes;
  double warm_call = 0.0;
  double warm_put = 0.0;
  // Single-sourced OTM-shortcut mask. The predicate depends only on pre-de-Am row
  // fields, so hoisting it out of the main loop is decision-preserving, and it
  // must be hoisted: the prepare pass below has to know which rows the shortcut
  // claims in order to exclude them from the shared population, and the main loop
  // has to make the identical call when it picks the row's route. Evaluating it
  // twice would risk divergence (a shortcut row silently promoted to a shared
  // lane) and would double-count the n_forced_* proposal-guard diagnostics.
  // Cost is neutral: the loop below used to run exactly this predicate once per
  // row, and it still runs exactly once per row.
  std::vector<std::uint8_t> shortcut_mask(am->obs.size(), 0u);
  for (std::size_t index = 0; index < am->obs.size(); ++index) {
    shortcut_mask[index] =
        use_otm_shortcut_deam(am->obs[index], S, T, r, q_eff, opts, method, &out.deam_audit) ? 1u
                                                                                            : 0u;
  }
  prepare_shared_boundary_proposals(am->obs, shortcut_mask, S, T, r, q_eff, opts, caches, al_opts,
                                    iv_tol, iv_max_iter, method, out.deam_audit);
  std::array<std::vector<double>, 4> audit_residuals;

  // P3 (perf F3): the non-trusted rows of one (expiry, side) share (S, T, r,
  // q_eff) and differ only in (K, σ), so their reference reprices batch through
  // the σ-boundary interpolant slice route (audit_european_equiv_iv_batch) —
  // O(n_σ)=8 AL boundary solves per side instead of one ACCURATE cold solve per
  // audited row. The loop is split into a COLLECT pass (route + invert every row,
  // warm-chain the inversions, gather the audited rows per side), one batched
  // reprice per side, and a FINALIZE pass (apply verdicts + accurate fallback and
  // emit obs in obs_index order — unchanged from the original body).
  //
  // POLICY (PM-decided, perf-review F3): admissible. The audit certifies
  // IV-INVERSION consistency (does the recovered σ reprice the American mid
  // inside the half-spread budget?), NOT boundary-PATH independence. The
  // σ-interpolant's qualified max price gap vs a cold per-row solve is
  // 3.8e-5/share (Task 11 §P2.5 ship gate), orders below the half-spread budget
  // the audit scores against — no verdict the budget can resolve is affected. A
  // row that passes keeps the SAME recovered σ, European premium, vega and
  // weights; only the diagnostic residual_half_spreads shifts by ≤ that gap.
  enum class ObsDisposition : std::uint8_t { RejectedInCollect, Trusted, Audited };
  struct PreparedObs {
    std::size_t obs_index;
    std::size_t source_index;
    IvRoute route;
    double sig;             // recovered IV (primary; overwritten on accurate fallback)
    double score_sigma;
    bool independent_score;
    ObsDisposition disp;
    std::size_t audit_slot; // index into this side's audit span (Audited rows only)
  };
  const auto reject_row = [&](std::size_t si, ObsRejectionReason reason) {
    ++out.n_dropped;
    out.provenance[si].rejection = reason;
  };

  std::vector<PreparedObs> prepared;
  prepared.reserve(am->obs.size());
  std::vector<double> call_K, call_sig, call_mid, call_spread;
  std::vector<double> put_K, put_sig, put_mid, put_spread;

  // ── Collect pass: route + invert every row, warm-chaining the inversions ──
  for (std::size_t obs_index = 0; obs_index < am->obs.size(); ++obs_index) {
    const FitObs &o = am->obs[obs_index];
    const std::size_t source_index = o.source_strike_index;
    if (source_index >= chain.n_strikes() || source_index >= out.provenance.size()) {
      return Err(ErrorCode::Internal,
                 "build_observations_european: source strike key out of range");
    }
    ++out.deam_audit.n_deam_rows;
    // Same mask the prepare pass consumed — not a re-evaluation. Priority order
    // is unchanged: shortcut -> shared_proposal -> scalar.
    const bool shortcut = shortcut_mask[obs_index] != 0u;
    const bool shared_proposal = !shortcut && std::isfinite(o.score_sigma_mkt) &&
                                 o.score_sigma_mkt > kObsIvMin && o.score_sigma_mkt < kObsIvMax;
    // A shortcut-claimed row must never also be a shared lane. `solve_shared_side`
    // skips masked rows, so a masked row carrying a shared proposal means the two
    // consumers disagreed about the mask — the exact divergence single-sourcing
    // exists to prevent. Fail closed rather than silently preferring a route.
    if (shortcut && std::isfinite(o.score_sigma_mkt)) {
      return Err(ErrorCode::Internal,
                 "build_observations_european: shortcut row carries a shared-boundary proposal");
    }
    // A Mid fit inversion and its score solve the same equation. Warm-starting
    // can move a tolerance-terminated result by a few ULPs, but that is not an
    // independent economic observation; reuse it. A shortcut has no fit
    // inversion, while Bid/Ask anchors target a different premium, so both still
    // require the cold raw-mid scoring solve.
    const bool independent_score =
        prepare_scoring && (shortcut || opts.anchor_kind != CalibAnchorKind::Mid);
    // Anchor-independent score. Inverted COLD off the raw symmetric mid so the
    // scored IV is anchor-agnostic (the fit inversion below may target a bid/ask
    // anchor). A failed or out-of-band scoring inversion leaves the row UNSCORED
    // (`kUnscoredIv`) — it must NOT drop the row: the row's presence in the fit
    // population is decided only by the fit inversion or the OTM shortcut.
    double score_sigma = kUnscoredIv;
    if (independent_score) {
      ++out.n_score_inversions;
      const std::size_t quote_index = chain_index(static_cast<std::uint16_t>(source_index), o.side);
      const Result<double> score =
          american_implied_vol(chain.mids[quote_index], S, o.K, T, r, q_eff, o.side, method, iv_tol,
                               iv_max_iter, al_opts, caches.for_side(o.side));
      if (score.has_value() && *score > kObsIvMin && *score < kObsIvMax) {
        score_sigma = *score;
      }
    }
    // `o.mid` is the anchor premium (the raw American mid under the default Mid
    // anchor). Recover the European-equivalent lognormal vol, then restate the
    // observation entirely in European terms.
    const CorrectionCache *correction = caches.for_side(o.side);
    const bool cache_proposal =
        correction != nullptr && correction->populated() && correction->side() == o.side;
    const IvRoute route =
        shortcut ? IvRoute::Shortcut
                 : (cache_proposal ? IvRoute::Cache
                                   : (method == AmericanMethod::AndersenLake && al_opts.has_value()
                                          ? IvRoute::Fast
                                          : IvRoute::Accurate));
    InversionRouteDiagnostics &proposal_diag = route_diag(out.deam_audit, route);
    ++proposal_diag.n_proposed;
    const double warm = warm_start_deam ? ((o.side == Side::Call) ? warm_call : warm_put) : 0.0;
    const Result<double> sig_res =
        shortcut ? Ok(o.sigma_mkt)
                 : (shared_proposal
                        ? Ok(o.score_sigma_mkt)
                        : american_implied_vol(o.mid, S, o.K, T, r, q_eff, o.side, method, iv_tol,
                                               iv_max_iter, al_opts, correction, warm));
    if (!sig_res.has_value() || !(*sig_res > kObsIvMin && *sig_res < kObsIvMax)) {
      reject_row(source_index, ObsRejectionReason::Deamericanization);
      prepared.push_back(PreparedObs{obs_index, source_index, route, 0.0, score_sigma,
                                     independent_score, ObsDisposition::RejectedInCollect, 0});
      continue;
    }
    const double sig = *sig_res;
    // Warm chain: the original body advances warm_{call,put} from each accepted
    // row's FINAL σ at the tail. Here it advances with the PRIMARY σ right after a
    // successful inversion — identical for every row accepted without an accurate
    // fallback (the common case), so the fitted surface is bit-identical there. A
    // row that later falls back or is audit-rejected would have contributed its
    // accurate σ (or nothing) to the chain; that is a warm-SEED difference only
    // (safeguarded Newton's converged root is seed-independent to tolerance),
    // consistent with this task's economic-parity contract.
    if (warm_start_deam && !shortcut) {
      (o.side == Side::Call ? warm_call : warm_put) = sig;
    }

    if (method != AmericanMethod::AndersenLake) {
      ++proposal_diag.n_accepted;
      prepared.push_back(PreparedObs{obs_index, source_index, route, sig, score_sigma,
                                     independent_score, ObsDisposition::Trusted, 0});
      continue;
    }
    // A successful direct accurate inversion has already been cold-polished
    // against the exact map used by audit_european_equiv_iv. Repeating that map
    // cannot add independent evidence, so it is a logical audit with no reprice.
    // Approximate proposals still require an independent reference reprice and
    // an audited accurate fallback when they miss the budget.
    const bool trusted_accurate_controls = iv_tol <= 1.0e-7 && iv_max_iter >= 64;
    if (shared_proposal) {
      // W3.1 accuracy-trading route: every lane cleared the embedded 9-vs-5
      // price estimator and its side cleared three higher-accuracy cold IV
      // sentinels. Repricing every accepted lane would restore O(strikes)
      // boundary work and erase the structural gain; the bounded sentinels
      // are the independent audit for this side.
      ++proposal_diag.n_audited;
      ++proposal_diag.n_accepted;
      prepared.push_back(PreparedObs{obs_index, source_index, route, sig, score_sigma,
                                     independent_score, ObsDisposition::Trusted, 0});
    } else if (route == IvRoute::Accurate && !opts.audit_accurate_inversions &&
               trusted_accurate_controls) {
      ++proposal_diag.n_audited;
      ++proposal_diag.n_accepted;
      prepared.push_back(PreparedObs{obs_index, source_index, route, sig, score_sigma,
                                     independent_score, ObsDisposition::Trusted, 0});
    } else {
      // Non-trusted: needs the independent reference reprice. Collect it into its
      // side's batch; the verdict + accurate fallback happen in the finalize pass.
      ++proposal_diag.n_audited;
      ++proposal_diag.n_reference_reprices;
      std::vector<double> &bk = (o.side == Side::Call) ? call_K : put_K;
      std::vector<double> &bs = (o.side == Side::Call) ? call_sig : put_sig;
      std::vector<double> &bm = (o.side == Side::Call) ? call_mid : put_mid;
      std::vector<double> &bsp = (o.side == Side::Call) ? call_spread : put_spread;
      const std::size_t slot = bk.size();
      bk.push_back(o.K);
      bs.push_back(sig);
      bm.push_back(o.mid);
      bsp.push_back(o.spread);
      prepared.push_back(PreparedObs{obs_index, source_index, route, sig, score_sigma,
                                     independent_score, ObsDisposition::Audited, slot});
    }
  }

  // ── Batched primary reprice per side (O(n_σ) boundary solves each) ─────────
  std::vector<Result<IvRepricingAudit>> call_audit(call_K.size());
  std::vector<Result<IvRepricingAudit>> put_audit(put_K.size());
  if (!call_K.empty()) {
    ATX_TRY_VOID(audit_european_equiv_iv_batch(S, T, r, q_eff, Side::Call, call_K, call_sig,
                                               call_mid, call_spread,
                                               opts.max_inversion_residual_half_spreads,
                                               call_audit));
  }
  if (!put_K.empty()) {
    ATX_TRY_VOID(audit_european_equiv_iv_batch(S, T, r, q_eff, Side::Put, put_K, put_sig, put_mid,
                                               put_spread,
                                               opts.max_inversion_residual_half_spreads, put_audit));
  }

  // ── Finalize pass: apply verdicts + accurate fallback, emit in obs order ───
  for (const PreparedObs &prep : prepared) {
    if (prep.disp == ObsDisposition::RejectedInCollect) {
      continue; // already counted + provenance-stamped in the collect pass
    }
    FitObs o = am->obs[prep.obs_index];
    const std::size_t source_index = prep.source_index;
    double sig = prep.sig;
    double score_sigma = prep.score_sigma;

    if (prep.disp == ObsDisposition::Audited) {
      const Result<IvRepricingAudit> &audit =
          (o.side == Side::Call) ? call_audit[prep.audit_slot] : put_audit[prep.audit_slot];
      InversionRouteDiagnostics &proposal_diag = route_diag(out.deam_audit, prep.route);
      if (audit.has_value()) {
        audit_residuals[static_cast<std::size_t>(prep.route)].push_back(
            audit->residual_half_spreads);
      }
      if (!audit.has_value() || !audit->passed) {
        if (prep.route == IvRoute::Accurate) {
          ++out.deam_audit.n_rejected_residual;
          reject_row(source_index, ObsRejectionReason::Deamericanization);
          continue;
        }
        ++proposal_diag.n_fallback;
        ++out.deam_audit.n_accurate_fallback;
        InversionRouteDiagnostics &accurate_diag = out.deam_audit.accurate;
        ++accurate_diag.n_proposed;
        const Result<double> refit =
            american_implied_vol(o.mid, S, o.K, T, r, q_eff, o.side, AmericanMethod::AndersenLake,
                                 1.0e-7, 64, std::nullopt, nullptr, sig);
        if (!refit || !(*refit > kObsIvMin && *refit < kObsIvMax)) {
          reject_row(source_index, ObsRejectionReason::Deamericanization);
          continue;
        }
        sig = *refit;
        ++accurate_diag.n_audited;
        ++accurate_diag.n_reference_reprices;
        const Result<IvRepricingAudit> reaudit =
            audit_european_equiv_iv(o.mid, o.spread, sig, S, o.K, T, r, q_eff, o.side,
                                    opts.max_inversion_residual_half_spreads);
        if (reaudit) {
          audit_residuals[static_cast<std::size_t>(IvRoute::Accurate)].push_back(
              reaudit->residual_half_spreads);
        }
        if (!reaudit || !reaudit->passed) {
          ++out.deam_audit.n_rejected_residual;
          reject_row(source_index, ObsRejectionReason::Deamericanization);
          continue;
        }
        ++accurate_diag.n_accepted;
      } else {
        ++proposal_diag.n_accepted;
      }
    }

    const double sigma_eu = sig;
    if (!prep.independent_score) {
      score_sigma = sigma_eu;
    }
    // (warm_{call,put} were advanced with the primary σ in the collect pass; no
    // inversion follows in this pass, so there is nothing left to chain here.)
    const double eu_px = black76_price(F, o.K, T, sigma_eu, df, o.side);
    if (!(eu_px > 0.0) || !std::isfinite(eu_px)) {
      reject_row(source_index, ObsRejectionReason::EuropeanPrice);
      continue;
    }
    const double vega = black76_value_and_vega(F, o.K, T, sigma_eu, df, o.side).vega;
    o.sigma_mkt = sigma_eu;
    o.w_mkt = sigma_eu * sigma_eu * T;
    o.mid = eu_px; // European-equivalent premium (what the convex fold expects)
    o.vega = vega;
    // Full vega weighting is preserved on the de-Am path. `opts.max_weight` is an
    // upper clip meaningful for the raw builder's stored American-mid weights; the
    // de-Am builder RE-DERIVES weights from European vega, whose natural w-space
    // scale (vega²/(2σT)²) is many orders above the default clip, so applying it
    // here would collapse every observation to the ceiling and erase the vega
    // weighting the surface fit and its cold/cached parity are calibrated against.
    //
    // T6 makes that omission load-bearing rather than merely historical. A row
    // admitted as a BOUND carries `o.spread` equal to its FULL band width — the
    // whole ask, not a half-spread — and its entire justification is that it
    // enters the objective at the small weight that width earns it. Clipping
    // here would map that row and a penny-wide two-sided row onto the same
    // ceiling, i.e. let a bid-less guess outvote a marked strike. The clip that
    // DOES run, in the American builder, is `min(w, max_weight)`: monotone, so
    // it can compress a bound row onto the ceiling but never lift it past a row
    // that earned more.
    o.weight_w = obs_weight_w(vega, o.spread, sigma_eu, T);
    o.active_weight_w = o.weight_w;
    o.noise_sigma = (vega > kVegaFloor) ? (o.spread / vega) : 1.0;
    o.score_sigma_mkt = score_sigma;
    ++out.deam_audit.n_deam_accepted;
    out.obs.push_back(o);
  }
  finalize_route_diag(out.deam_audit.shortcut, std::move(audit_residuals[0]));
  finalize_route_diag(out.deam_audit.cache, std::move(audit_residuals[1]));
  finalize_route_diag(out.deam_audit.fast, std::move(audit_residuals[2]));
  finalize_route_diag(out.deam_audit.accurate, std::move(audit_residuals[3]));
  if (out.obs.size() < kMinObs) {
    return Err(ErrorCode::NotFound,
               "build_observations_european: fewer than 5 European obs survived " +
                   describe_rejections(out.obs.size(), out.provenance));
  }
  return Ok(std::move(out));
}

std::size_t shared_boundary_deam_batch(std::span<FitObs> rows, double S, double r, double F,
                                       double T, double df, const CalibOpts &opts,
                                       const AmericanCorrectionCaches &caches,
                                       const std::optional<AlOpts> &al_opts, double iv_tol,
                                       std::uint16_t iv_max_iter, AmericanMethod method,
                                       DeAmAuditDiagnostics *audit) {
  // Fail-closed: leave every row unscored so a malformed call degrades to the
  // caller's scalar oracle, never to a silently wrong shared proposal.
  for (FitObs &row : rows) {
    row.score_sigma_mkt = kUnscoredIv;
  }
  if (!(S > 0.0) || !(F > 0.0) || !(T > 0.0) || !(df > 0.0) || !std::isfinite(r) || rows.empty()) {
    return 0u;
  }
  // q_eff bridge: S·e^{(r−q_eff)T} == F exactly — the SAME carry
  // build_observations_european forms, so both paths de-Americanize on one forward.
  const double q_eff = r - std::log(F / S) / T;

  // Build the shared-lane population. Only a row whose OTM leg inverts to an
  // in-band European (Black-76) seed can open a lane: the seed is the lane bracket
  // hi and (with the side's min/max seeds) spans the interpolant's sigma domain, so
  // a failed-seed row must be EXCLUDED rather than admitted with a 0 seed that would
  // collapse that domain. Excluded rows stay unscored and fall to the scalar oracle.
  std::vector<FitObs> population;
  population.reserve(rows.size());
  std::vector<std::size_t> population_to_row;
  population_to_row.reserve(rows.size());
  for (std::size_t index = 0; index < rows.size(); ++index) {
    FitObs seed = rows[index];
    seed.F = F;
    seed.df = df;
    const Result<double> european_seed = implied_vol(seed.mid, F, seed.K, T, df, seed.side);
    if (!european_seed.has_value() ||
        !(*european_seed > kObsIvMin && *european_seed < kObsIvMax)) {
      continue; // no European seed — the caller's scalar fallback handles this row
    }
    seed.sigma_mkt = *european_seed;
    seed.vega = black76_value_and_vega(F, seed.K, T, *european_seed, df, seed.side).vega;
    seed.score_sigma_mkt = kUnscoredIv;
    population.push_back(seed);
    population_to_row.push_back(index);
  }
  if (population.empty()) {
    return 0u;
  }
  // The Legacy driver runs no OTM-shortcut route, so every eligible row is a
  // shared-lane candidate (all-zero mask); any row the batch does not certify falls
  // to the scalar oracle. `prepare_shared_boundary_proposals` itself enforces the
  // engagement guards (use_shared_boundary_deam, Mid anchor, Andersen-Lake, r ≥ 0,
  // a wide-enough positive-rate side) and writes score_sigma_mkt only for lanes it
  // solves AND certifies against the bounded accurate sentinels.
  DeAmAuditDiagnostics throwaway{};
  DeAmAuditDiagnostics &diag = (audit != nullptr) ? *audit : throwaway;
  const std::vector<std::uint8_t> shortcut_mask(population.size(), 0u);
  prepare_shared_boundary_proposals(population, shortcut_mask, S, T, r, q_eff, opts, caches, al_opts,
                                    iv_tol, iv_max_iter, method, diag);
  std::size_t certified = 0u;
  for (std::size_t index = 0; index < population.size(); ++index) {
    const double sigma = population[index].score_sigma_mkt;
    if (std::isfinite(sigma) && sigma > kObsIvMin && sigma < kObsIvMax) {
      rows[population_to_row[index]].score_sigma_mkt = sigma;
      ++certified;
    }
  }
  return certified;
}

bool deam_inversion_certified(const DeAmAuditDiagnostics &audit,
                              double max_drop_fraction) noexcept {
  // 1. Every ACCEPTED proposal must have been audited. A route that accepts
  //    more than it audits carries un-audited nodes into the fit set — the
  //    shape of a method (e.g. Baw) with no cold-reference audit at all.
  const auto route_audited = [](const InversionRouteDiagnostics &route) noexcept {
    return route.n_accepted <= route.n_audited;
  };
  if (!route_audited(audit.shortcut) || !route_audited(audit.cache) || !route_audited(audit.fast) ||
      !route_audited(audit.accurate)) {
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
  const double drop_fraction =
      static_cast<double>(dropped) / static_cast<double>(audit.n_deam_rows);
  return drop_fraction <= max_drop_fraction;
}

Result<double> obs_accepted(const Chain &chain, std::uint16_t strike_idx, Side side, double F,
                            double T, double df, const CalibOpts &opts) {
  if (strike_idx >= chain.n_strikes()) {
    return Err(ErrorCode::InvalidArgument, "obs_accepted: strike_idx out of range");
  }
  if (!(F > 0.0) || !(T > 0.0) || !(df > 0.0)) {
    return Err(ErrorCode::InvalidArgument, "obs_accepted: F, T and df must be positive");
  }
  if (!chain_soa_well_formed(chain)) {
    return Err(ErrorCode::InvalidArgument,
               "obs_accepted: chain SoA arrays shorter than 2*n_strikes");
  }

  const RowResult rr = evaluate_row(chain, strike_idx, side, F, T, df, opts);
  if (rr.outcome == RowOutcome::Accepted) {
    return Ok(rr.obs.sigma_mkt);
  }
  return Err(ErrorCode::NotFound, "obs_accepted: quote rejected by the filter cascade");
}

} // namespace atx::vol
