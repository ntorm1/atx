#include "atx/vol/calib.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <tuple>
#include <utility>
#include <vector>

#include "atx/core/error.hpp"
#include "atx/vol/american_iv.hpp" // american_implied_vol (de-Americanization)
#include "atx/vol/arb.hpp"         // QuoteFlag, has_flag (kill-mask filter step)
#include "atx/vol/black76.hpp"     // black76_value_and_vega, black76_price
#include "atx/vol/deamer.hpp"      // cold-reference IV proposal audit
#include "atx/vol/implied_vol.hpp" // implied_vol (IV inversion)
#include "boundary_interp.hpp"     // retained sigma-boundary de-Am path

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
  FitObs obs{};
};

// Evaluate one (strike, side) tuple against the quote-filter cascade. Mirrors
// exactly one iteration of the C `ats_vol_svi_build_observations` inner loop
// (tenor buckets omitted → scalar max_spread_vol / min_vega_weight). The caller
// has already validated F/T/df and the chain SoA sizing.
[[nodiscard]] RowResult evaluate_row(const Chain &chain, std::uint16_t strike_idx, Side side,
                                     double F, double T, double df, const CalibOpts &opts) {
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

  // 2. Two-sided, positive, non-crossed quote.
  const double bid = chain.bids[idx];
  const double ask = chain.asks[idx];
  if (!(bid > 0.0 && ask > bid)) {
    r.rejection = ObsRejectionReason::InvalidBidAsk;
    return r; // Rejected
  }

  // Prefer-call heuristic: keep the call leg for K ≥ F, the put leg otherwise.
  // The non-preferred leg is silently skipped — NOT counted as a drop (this
  // gate deliberately sits AFTER the flag + bid/ask checks, matching the C, so
  // a flagged non-preferred leg still counts as a drop above).
  const bool prefer_call = (K >= F);
  if (prefer_call != (side == Side::Call)) {
    r.outcome = RowOutcome::Skipped;
    r.rejection = ObsRejectionReason::None;
    return r;
  }

  // 3. Positive mid.
  const double mid = chain.mids[idx];
  if (!(mid > 0.0)) {
    r.rejection = ObsRejectionReason::InvalidMid;
    return r; // Rejected
  }

  // 4. Wide-spread-to-mid filter (Sprint 24 Phase D); disabled when the cap ≤ 0.
  if (opts.max_spread_to_mid_pct > 0.0) {
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

inline constexpr std::uint16_t kSharedSigmaNodes = 9u;
inline constexpr std::size_t kSharedMinSideRows = 16u;
inline constexpr std::size_t kSharedMinAcceptedRows = 12u;
inline constexpr std::size_t kSharedLaneCapacity = 128u;
inline constexpr double kSharedMinT = 3.0 / 365.0;
inline constexpr double kSharedMinSigma = 0.01;

struct SharedIvLane {
  FitObs *observation{nullptr};
  double lo{0.0};
  double hi{0.0};
  double f_lo{0.0};
  double f_hi{0.0};
  bool active{false};
};

[[nodiscard]] double shared_boundary_price(detail::SigmaBoundaryInterp &interp, const FitObs &o,
                                           double S, double sigma) noexcept {
  const double internal_spot = o.side == Side::Call ? o.K : S;
  const double internal_strike = o.side == Side::Call ? S : o.K;
  return interp.price_internal_put(internal_spot, internal_strike, sigma);
}

[[nodiscard]] double shared_boundary_embedded_price(detail::SigmaBoundaryInterp &interp,
                                                    const FitObs &o, double S,
                                                    double sigma) noexcept {
  const double internal_spot = o.side == Side::Call ? o.K : S;
  const double internal_strike = o.side == Side::Call ? S : o.K;
  return interp.price_internal_put_embedded(internal_spot, internal_strike, sigma);
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
  lane.lo = interp.sigma_lo();
  lane.hi = o.sigma_mkt;
  if (!(lane.hi > lane.lo)) {
    return false;
  }
  const double price_lo = shared_boundary_price(interp, o, S, lane.lo);
  const double price_hi = shared_boundary_price(interp, o, S, lane.hi);
  lane.f_lo = price_lo - o.mid;
  lane.f_hi = price_hi - o.mid;
  lane.active = std::isfinite(lane.f_lo) && std::isfinite(lane.f_hi) && lane.f_lo < 0.0 &&
                lane.f_hi >= 0.0 && price_hi >= price_lo;
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
      const double width = lane.hi - lane.lo;
      if (width <= solve_tol) {
        lane.active = false;
        continue;
      }
      any_active = true;
      double sigma = 0.5 * (lane.lo + lane.hi);
      const double residual_span = lane.f_hi - lane.f_lo;
      if (residual_span > 0.0 && std::isfinite(residual_span)) {
        const double secant = lane.lo - lane.f_lo * width / residual_span;
        // A central secant is accepted; a tail-hugging regula-falsi step is
        // replaced by bisection so each iteration shrinks the bracket by at
        // least 25% and the bounded max_iter contract is constructive.
        const double guard = 0.25 * width;
        if (secant > lane.lo + guard && secant < lane.hi - guard) {
          sigma = secant;
        }
      }
      const double residual =
          shared_boundary_price(interp, *lane.observation, S, sigma) - lane.observation->mid;
      if (!std::isfinite(residual)) {
        lane.active = false;
        lane.observation = nullptr;
      } else if (residual < 0.0) {
        lane.lo = sigma;
        lane.f_lo = residual;
      } else {
        lane.hi = sigma;
        lane.f_hi = residual;
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
  if (lane.observation == nullptr || lane.hi < lane.lo || lane.hi - lane.lo > solve_tol) {
    return false;
  }
  const double sigma = 0.5 * (lane.lo + lane.hi);
  const double price = shared_boundary_price(interp, *lane.observation, S, sigma);
  const double embedded = shared_boundary_embedded_price(interp, *lane.observation, S, sigma);
  const double budget = shared_economic_price_budget(*lane.observation, T, sigma, opts);
  if (!(sigma > kObsIvMin && sigma < kObsIvMax) || !(budget > 0.0) || !std::isfinite(price) ||
      !std::isfinite(embedded) || std::fabs(price - lane.observation->mid) > budget ||
      std::fabs(price - embedded) > budget) {
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
  const double internal_rate = side == Side::Call ? q_eff : r;
  if (side_rows < kSharedMinSideRows || !(T >= kSharedMinT) || !(internal_rate > 0.0)) {
    return;
  }
  const double sigma_lo = std::max(kSharedMinSigma, 0.35 * min_seed);
  const double sigma_hi = std::min(kObsIvMax, max_seed * (1.0 + 1.0e-12));
  if (!(sigma_hi > sigma_lo) || sigma_hi / sigma_lo > 20.0) {
    return;
  }
  const double internal_yield = side == Side::Call ? r : q_eff;
  detail::SigmaBoundaryInterp interp;
  const amer::AlScheme scheme = amer::scheme_from_opts(al_opts);
  if (!interp.build(S, T, internal_rate, internal_yield, sigma_lo, sigma_hi, kSharedSigmaNodes,
                    scheme)) {
    diag.n_shared_scalar_fallback_lanes += static_cast<std::uint32_t>(side_rows);
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
    return;
  }
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
    for (int side_i = 0; side_i < 2; ++side_i) {
      const Side side = static_cast<Side>(static_cast<std::uint8_t>(side_i));
      const RowResult rr = evaluate_row(chain, sidx, side, F, T, df, opts);
      if (rr.outcome == RowOutcome::Accepted) {
        out.obs.push_back(rr.obs);
      } else if (rr.outcome == RowOutcome::Rejected) {
        ++n_drop;
      }
      if (side == preferred) {
        out.provenance.push_back(ObsProvenance{static_cast<std::uint32_t>(s), side, rr.rejection});
      }
      // Skipped: the non-preferred leg — not counted (C bare `continue`).
    }
  }

  out.n_dropped = n_drop;
  if (out.provenance.size() != n) {
    return Err(ErrorCode::Internal, "build_observations: preferred-row provenance is incomplete");
  }
  if (out.obs.size() < kMinObs) {
    return Err(ErrorCode::NotFound, "build_observations: fewer than 5 observations survived");
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
  for (std::size_t obs_index = 0; obs_index < am->obs.size(); ++obs_index) {
    FitObs o = am->obs[obs_index];
    const std::size_t source_index = o.source_strike_index;
    if (source_index >= chain.n_strikes() || source_index >= out.provenance.size()) {
      return Err(ErrorCode::Internal,
                 "build_observations_european: source strike key out of range");
    }
    const auto reject = [&](ObsRejectionReason reason) {
      ++out.n_dropped;
      out.provenance[source_index].rejection = reason;
    };
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
    Result<double> sig =
        shortcut ? Ok(o.sigma_mkt)
                 : (shared_proposal
                        ? Ok(o.score_sigma_mkt)
                        : american_implied_vol(o.mid, S, o.K, T, r, q_eff, o.side, method, iv_tol,
                                               iv_max_iter, al_opts, correction, warm));
    if (!sig.has_value() || !(*sig > kObsIvMin && *sig < kObsIvMax)) {
      reject(ObsRejectionReason::Deamericanization);
      continue;
    }
    bool fit_sigma_converged = !shortcut;

    // A successful direct accurate inversion has already been cold-polished
    // against the exact map used by audit_european_equiv_iv. Repeating that map
    // cannot add independent evidence, so it is a logical audit with no reprice.
    // Approximate proposals still require an independent reference reprice and
    // an audited accurate fallback when they miss the budget.
    if (method == AmericanMethod::AndersenLake) {
      const bool trusted_accurate_controls = iv_tol <= 1.0e-7 && iv_max_iter >= 64;
      if (shared_proposal) {
        // W3.1 accuracy-trading route: every lane cleared the embedded 9-vs-5
        // price estimator and its side cleared three higher-accuracy cold IV
        // sentinels. Repricing every accepted lane would restore O(strikes)
        // boundary work and erase the structural gain; the bounded sentinels
        // are the independent audit for this side.
        ++proposal_diag.n_audited;
        ++proposal_diag.n_accepted;
      } else if (route == IvRoute::Accurate && !opts.audit_accurate_inversions &&
                 trusted_accurate_controls) {
        ++proposal_diag.n_audited;
        ++proposal_diag.n_accepted;
      } else {
        ++proposal_diag.n_audited;
        ++proposal_diag.n_reference_reprices;
        Result<IvRepricingAudit> audit =
            audit_european_equiv_iv(o.mid, o.spread, *sig, S, o.K, T, r, q_eff, o.side,
                                    opts.max_inversion_residual_half_spreads);
        if (audit) {
          audit_residuals[static_cast<std::size_t>(route)].push_back(audit->residual_half_spreads);
        }
        if (!audit || !audit->passed) {
          if (route == IvRoute::Accurate) {
            ++out.deam_audit.n_rejected_residual;
            reject(ObsRejectionReason::Deamericanization);
            continue;
          }
          ++proposal_diag.n_fallback;
          ++out.deam_audit.n_accurate_fallback;
          InversionRouteDiagnostics &accurate_diag = out.deam_audit.accurate;
          ++accurate_diag.n_proposed;
          sig =
              american_implied_vol(o.mid, S, o.K, T, r, q_eff, o.side, AmericanMethod::AndersenLake,
                                   1.0e-7, 64, std::nullopt, nullptr, *sig);
          if (!sig || !(*sig > kObsIvMin && *sig < kObsIvMax)) {
            reject(ObsRejectionReason::Deamericanization);
            continue;
          }
          ++accurate_diag.n_audited;
          ++accurate_diag.n_reference_reprices;
          audit = audit_european_equiv_iv(o.mid, o.spread, *sig, S, o.K, T, r, q_eff, o.side,
                                          opts.max_inversion_residual_half_spreads);
          if (audit) {
            audit_residuals[static_cast<std::size_t>(IvRoute::Accurate)].push_back(
                audit->residual_half_spreads);
          }
          if (!audit || !audit->passed) {
            ++out.deam_audit.n_rejected_residual;
            reject(ObsRejectionReason::Deamericanization);
            continue;
          }
          ++accurate_diag.n_accepted;
          fit_sigma_converged = true;
        } else {
          ++proposal_diag.n_accepted;
        }
      }
    } else {
      ++proposal_diag.n_accepted;
    }

    const double sigma_eu = *sig;
    if (!independent_score) {
      score_sigma = sigma_eu;
    }
    if (warm_start_deam && fit_sigma_converged) {
      if (o.side == Side::Call) {
        warm_call = sigma_eu;
      } else {
        warm_put = sigma_eu;
      }
    }
    const double eu_px = black76_price(F, o.K, T, sigma_eu, df, o.side);
    if (!(eu_px > 0.0) || !std::isfinite(eu_px)) {
      reject(ObsRejectionReason::EuropeanPrice);
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
