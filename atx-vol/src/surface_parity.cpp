#include "atx/vol/surface_parity.hpp"

#include <algorithm>
#include <chrono>    // ATX_VOL_PROFILE phase timing (temporary)
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>    // ATX_VOL_PROFILE stderr report (temporary)
#include <cstdlib>   // getenv (ATX_VOL_PROFILE)
#include <limits>
#include <utility>
#include <vector>

#include "atx/core/error.hpp"
#include "atx/vol/arb.hpp"          // arb_check_calendar, ArbViolation
#include "atx/vol/black76.hpp"      // black76_value_and_vega
#include "atx/vol/calib.hpp"        // FitObs, FitDiag, CalibOpts
#include "atx/vol/deamer.hpp"       // de_americanize_chain, european_equiv_iv, otm_side
#include "atx/vol/essvi_calib.hpp"  // essvi_fit_slice
#include "atx/vol/parity.hpp"       // chain_parity, ParityInputs, ParityReport
#include "atx/vol/types.hpp"
#include "atx/vol/universe.hpp"     // Underlying, Chain, chain_index
#include "atx/vol/vol_surface.hpp"  // VolSurface, EssviParams, Parametrization

// PORT / PARITY NOTES
// -------------------
// * Per-expiry pattern reuse. The de-Americanize -> aligned-obs -> eSSVI-fit
//   path is a faithful copy of `vola_parity.cpp` (leg_quote_valid /
//   build_aligned_obs / the q_eff bridge). vola_parity returns only metrics, so
//   we re-derive the fitted `EssviParams` slice here to WRITE it into the
//   surface. Because both harnesses invert with the same `european_equiv_iv` at
//   the same q_eff, the recovered market IVs are identical to the de-Am strip.
//
// * Model-IV read-back. Per-slice re-Am parity reads the model IV from the
//   ASSEMBLED surface via `VolSurface::iv_on_slice(idx, k)` (= sqrt(w_slice/
//   T_slice)), proving the number scored is the one the surface actually
//   serves, not a side computation.
//
// * Calendar no-arb checker (arb.hpp signature used):
//     Result<std::vector<ArbViolation>>
//     arb_check_calendar(const VolSurface& s, double k_min, double k_max,
//                        std::uint32_t n_grid);
//   An EMPTY violation vector means "no calendar arbitrage" (arb.hpp's C
//   `out_n_violations == 0` convention). We sample k in [-3, 3] over 25 grid
//   points (the spec grid) and set calendar_arb_free = violations.empty().

namespace atx::vol {

using atx::core::Err;
using atx::core::ErrorCode;
using atx::core::Ok;

namespace {

// Minimum strikes that must survive to attempt a fit (mirrors vola_parity /
// the C build_observations "< 5 rows" floor; keeps the 3-parameter SSVI
// backbone over-determined).
constexpr std::size_t kMinUsableObs = 5;

// Floor on the bid/ask spread in the w-space weight so a locked (bid == ask)
// but otherwise valid quote cannot divide by zero (matches deamer's floor).
constexpr double kMinSpread = 1.0e-8;

// Calendar no-arb sampling grid (spec: +/-3 over ~25 steps).
constexpr double kArbKMin = -3.0;
constexpr double kArbKMax = 3.0;
constexpr std::uint32_t kArbNGrid = 25;

// True iff the chosen leg's quote is invertible: strictly positive, non-crossed
// bid/ask and a finite positive mid. `idx` is chain_index(strike_idx, side).
// Identical predicate to de_americanize_chain's / vola_parity's leg_quote_valid.
[[nodiscard]] bool leg_quote_valid(const Chain& chain, std::size_t idx) noexcept {
  const double bid = chain.bids[idx];
  const double ask = chain.asks[idx];
  const double mid = chain.mids[idx];
  return (bid > 0.0) && (ask > 0.0) && (ask >= bid) && std::isfinite(mid) &&
         (mid > 0.0);
}

// The aligned, self-contained observation set rebuilt from the chain on the
// de-Am forward. Every vector is the same length (obs.size()); `obs` feeds the
// curve fitter, the rest feed chain_parity.
struct AlignedObs {
  std::vector<FitObs> obs;
  std::vector<double> strike;
  std::vector<double> bid;
  std::vector<double> ask;
  std::vector<double> mid;
  std::vector<Side> side;
  std::vector<double> k_log;
  std::vector<double> market_iv;
  std::size_t n_dropped{0};
};

// Rebuild the aligned observation set on forward `F` / carry `q_eff`. Any strike
// whose OTM leg is unquotable or fails to invert is counted in `n_dropped`.
// Mirrors vola_parity.cpp::build_aligned_obs.
[[nodiscard]] AlignedObs build_aligned_obs(const Chain& chain, double S, double r,
                                           double F, double q_eff,
                                           const DeAmOptions& deam) {
  const double T = chain.T;
  const double df = std::exp(-r * T);
  const std::size_t n = chain.n_strikes();

  AlignedObs a;
  a.obs.reserve(n);
  a.strike.reserve(n);
  a.bid.reserve(n);
  a.ask.reserve(n);
  a.mid.reserve(n);
  a.side.reserve(n);
  a.k_log.reserve(n);
  a.market_iv.reserve(n);

  for (std::size_t i = 0; i < n; ++i) {
    const double K = chain.strikes[i];
    if (!(K > 0.0)) {
      ++a.n_dropped;
      continue;
    }
    const double k = std::log(K / F);
    const Side side = otm_side(k);
    const std::size_t idx = chain_index(static_cast<std::uint16_t>(i), side);
    if (!leg_quote_valid(chain, idx)) {
      ++a.n_dropped;
      continue;
    }

    const Result<double> iv_res =
        european_equiv_iv(chain.mids[idx], S, K, T, r, q_eff, side, deam.method,
                          deam.al_opts, deam.caches.for_side(side), deam.iv_tol,
                          deam.iv_max_iter);
    if (!iv_res) {
      ++a.n_dropped;
      continue;
    }
    const double iv = *iv_res;

    const double bid = chain.bids[idx];
    const double ask = chain.asks[idx];
    const double mid = chain.mids[idx];
    const double spread = ask - bid;
    const double vega = black76_value_and_vega(F, K, T, iv, df, side).vega;

    // w-space weight = vega^2 / spread^2 / (2*sigma*T)^2 (calib.hpp FitObs
    // convention). Guarded against a vanishing vega / spread; a degenerate
    // result falls back to unit weight so the row still constrains the fit.
    const double sp = std::fmax(spread, kMinSpread);
    const double two_sig_t = 2.0 * iv * T;
    double weight_w = 0.0;
    if (vega > 0.0 && std::isfinite(vega) && two_sig_t > 0.0) {
      weight_w = (vega * vega) / (sp * sp * two_sig_t * two_sig_t);
    }
    if (!std::isfinite(weight_w) || !(weight_w > 0.0)) {
      weight_w = 1.0;
    }

    FitObs fo{};
    fo.k = k;
    fo.sigma_mkt = iv;
    fo.w_mkt = iv * iv * T;
    fo.weight_w = weight_w;
    fo.active_weight_w = weight_w;
    fo.K = K;
    fo.F = F;
    fo.df = df;
    fo.mid = mid;
    fo.spread = spread;
    fo.vega = vega;
    fo.noise_sigma = (vega > 0.0) ? (spread / vega) : 0.0;
    fo.side = side;
    a.obs.push_back(fo);

    a.strike.push_back(K);
    a.bid.push_back(bid);
    a.ask.push_back(ask);
    a.mid.push_back(mid);
    a.side.push_back(side);
    a.k_log.push_back(k);
    a.market_iv.push_back(iv);
  }
  return a;
}

// ── Calendar-floor-constrained slice fit (active-set) ────────────────────
//
// Fit this slice subject to a calendar floor w(k) >= w_prev(k) over the slice's
// own data k-range, so it does not calendar-cross the previous (shorter-T)
// slice where THIS expiry is quoted. Approach: fit normally, then iterate an
// ACTIVE SET of one-sided floor pseudo-observations — at each currently-violating
// grid point add a heavily-weighted obs targeting w = w_prev(k), and refit. The
// least-squares LM lifts only the violating region toward the floor, giving up
// the minimal fit error needed to stay monotone (vs. a global theta bump that
// lifts the whole slice). Converges when no grid point violates, or after a
// bounded number of passes. `prev == nullptr` (first slice) => a plain fit.
//
// The floor is compared on TOTAL variance (what the surface serves / arb checks)
// but the pseudo-obs is a backbone target with the residual layer DISABLED for
// the floored refit: a heavy pseudo-obs must not be absorbed by (or distort) the
// small additive wing residual. The returned slice keeps its residual from the
// initial fit only if it never needed flooring.
[[nodiscard]] Result<EssviParams> fit_slice_calendar_floored(
    const AlignedObs& a, double T, double F, const CalibOpts& opts,
    FitDiag* diag, const EssviParams* prev, double df) {
  const double theta_floor = (prev != nullptr) ? prev->theta : 0.0;
  Result<EssviParams> res = essvi_fit_slice(a.obs, T, F, opts, diag, theta_floor);
  if (!res || prev == nullptr || a.k_log.empty()) {
    return res;
  }

  // Enforce the floor only over this slice's own quoted k-range (+ a small
  // margin) — the region where the calendar cross is economically real. Outside
  // it, the wing extrapolation is left free.
  double k_lo = a.k_log.front();
  double k_hi = a.k_log.front();
  for (const double k : a.k_log) {
    k_lo = std::min(k_lo, k);
    k_hi = std::max(k_hi, k);
  }
  constexpr double kMargin = 0.10;
  // Enforce over the slice's own quoted range PLUS a near-money band, so a
  // crossing that sits just outside a narrow (short-expiry) slice's strikes is
  // still caught. The near-money band is where calendar crossings are
  // economically real; deep wings are left free (documented extrapolation).
  constexpr double kNearMoneyK = 0.7;
  k_lo = std::min(k_lo - kMargin, -kNearMoneyK);
  k_hi = std::max(k_hi + kMargin, kNearMoneyK);

  double w_base = 0.0;  // heaviest base weight → penalty scale
  for (const FitObs& o : a.obs) {
    w_base = std::max(w_base, o.weight_w);
  }
  const double penalty = (w_base > 0.0 ? w_base : 1.0) * 300.0;

  constexpr int kNGrid = 80;
  constexpr int kMaxPass = 8;
  const double dk = (k_hi - k_lo) / static_cast<double>(kNGrid);

  CalibOpts floored_opts = opts;
  floored_opts.residual_disable = true;  // keep pseudo-obs out of the residual

  std::vector<FitObs> aug;
  aug.reserve(a.obs.size() + static_cast<std::size_t>(kNGrid) + 1);
  for (int pass = 0; pass < kMaxPass; ++pass) {
    aug.assign(a.obs.begin(), a.obs.end());
    bool violated = false;
    for (int gi = 0; gi <= kNGrid; ++gi) {
      const double k = k_lo + static_cast<double>(gi) * dk;
      const double wp = essvi_total_w(*prev, k);
      const double wc = essvi_total_w(*res, k);
      if (wp > wc + 1.0e-12) {
        violated = true;
        FitObs o{};
        o.k = k;
        o.w_mkt = wp;
        o.sigma_mkt = std::sqrt(std::fmax(wp, 1.0e-12) / T);
        o.weight_w = penalty;
        o.active_weight_w = penalty;
        o.F = F;
        o.K = F * std::exp(k);
        o.df = df;
        o.side = otm_side(k);
        aug.push_back(o);
      }
    }
    if (!violated) {
      break;  // floor satisfied over the whole grid
    }
    Result<EssviParams> r2 =
        essvi_fit_slice(aug, T, F, floored_opts, diag, theta_floor);
    if (!r2) {
      break;  // keep the last good fit rather than fail the whole surface
    }
    res = std::move(r2);
  }
  return res;
}

}  // namespace

Result<SurfaceParityReport> run_surface_parity(const Underlying& under,
                                               const SurfaceParityInputs& in) {
  if (!(in.S > 0.0) || !std::isfinite(in.r)) {
    return Err(ErrorCode::InvalidArgument,
               "run_surface_parity: non-positive S or non-finite r");
  }
  const std::size_t n_chains = under.chains.size();
  if (n_chains == 0) {
    return Err(ErrorCode::NotFound,
               "run_surface_parity: underlying carries no chains");
  }
  if (!((in.expiry_rates.empty() && in.expiry_rate_T.empty()) ||
        (in.expiry_rates.size() == n_chains && in.expiry_rate_T.size() == n_chains))) {
    return Err(ErrorCode::InvalidArgument, "run_surface_parity: invalid expiry rate vectors");
  }
  for (std::size_t i = 0u; i < in.expiry_rates.size(); ++i) {
    if (!std::isfinite(in.expiry_rates[i]) || !std::isfinite(in.expiry_rate_T[i]) ||
        !(in.expiry_rate_T[i] > 0.0) || in.expiry_rate_T[i] != under.chains[i].T) {
      return Err(ErrorCode::InvalidArgument, "run_surface_parity: invalid expiry rate value");
    }
  }

  ATX_TRY(VolSurface surface,
          VolSurface::create(under.uid, Parametrization::Essvi, n_chains));

  std::vector<double> expiry_T;
  std::vector<ParityReport> per_expiry;
  std::vector<SliceContext> context;
  expiry_T.reserve(n_chains);
  per_expiry.reserve(n_chains);
  context.reserve(n_chains);

  // Everything a slice needs to be SCORED after the surface is fully assembled
  // (and possibly calendar-repaired). We defer scoring out of the fit loop so
  // the model IV read back is the one the FINAL surface serves — if a repair
  // pass moves a slice, the parity number reflects the moved slice, not a stale
  // pre-repair read.
  struct PendingSlice {
    AlignedObs a;               // aligned obs (strike/bid/ask/mid/side/k/mkt-iv)
    double T{0.0};              // slice maturity
    double rate{0.0};           // expiry-specific continuously-compounded rate
    double q_eff{0.0};          // effective carry for the re-Am scoring
    std::uint16_t slice_idx{0}; // surface write index for iv_on_slice read-back
  };
  std::vector<PendingSlice> pending;
  pending.reserve(n_chains);

  double worst = std::numeric_limits<double>::infinity();
  std::size_t idx = 0;  // ascending write index / fitted-slice count

  // Calendar-monotone fit (CalendarRepair::MonotoneFit): carry the previous
  // fitted slice forward so the next slice can be fit with a calendar floor
  // w(k) >= w_prev(k) over its data range (theta floor + active-set w-floor).
  EssviParams prev_slice{};
  bool has_prev = false;

  // ── Optional phase profile (ATX_VOL_PROFILE=1) ─────────────────────────
  // Temporary build-cost breakdown for the perf-tuning pass; zero cost when the
  // env var is unset. Times the three per-chain phases + the calendar check.
  std::size_t env_sz = 0;
  char env_buf[8] = {};
  const bool profile =
      getenv_s(&env_sz, env_buf, sizeof(env_buf), "ATX_VOL_PROFILE") == 0 &&
      env_sz > 0;
  const auto now_ns = []() noexcept {
    return std::chrono::duration<double, std::milli>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
  };
  double ms_deam = 0.0, ms_align = 0.0, ms_fit = 0.0;

  // Chains are stored ascending in T; walk them in that order so slices land
  // in the surface ascending as set_slice_essvi requires.
  for (std::size_t chain_index = 0u; chain_index < under.chains.size(); ++chain_index) {
    const Chain &chain = under.chains[chain_index];
    const double T = chain.T;
    const double rate = in.expiry_rates.empty() ? in.r : in.expiry_rates[chain_index];
    if (!(T > 0.0)) {
      continue;  // degenerate maturity: skip (not fatal)
    }

    // 1. Resolve the term (forward, borrow) ONLY — the per-strike inversion
    //    below (build_aligned_obs) rebuilds the fit observations itself, so
    //    calling the full de_americanize_chain here would invert every strike a
    //    second, wasted time. resolve_chain_forward is its borrow-only front half.
    const double t_deam = profile ? now_ns() : 0.0;
    const auto d_res =
        resolve_chain_forward(chain, in.S, rate, in.cash_divs, in.now_ts_ns, in.deam);
    if (profile) ms_deam += now_ns() - t_deam;
    if (!d_res) {
      continue;  // an expiry we cannot de-Americanize contributes no slice
    }
    const double F = d_res->forward;
    if (!(F > 0.0) || !std::isfinite(F)) {
      continue;
    }
    // q_eff bridge: S*e^{(r-q_eff)T} == F exactly.
    const double q_eff = rate - std::log(F / in.S) / T;

    // 2. Aligned, self-contained observation rebuild on (F, q_eff).
    const double t_align = profile ? now_ns() : 0.0;
    AlignedObs a = build_aligned_obs(chain, in.S, rate, F, q_eff, in.deam);
    if (profile) ms_align += now_ns() - t_align;
    if (a.obs.size() < kMinUsableObs) {
      continue;  // fewer than the minimum usable strikes: skip this slice
    }

    // 3. Fit the eSSVI slice (natural form, T/F stamped in). MonotoneFit adds a
    //    calendar floor vs. the previous fitted slice (theta floor + active-set
    //    w-floor over the data range); every other mode is the plain fit.
    const double t_fit = profile ? now_ns() : 0.0;
    FitDiag diag{};
    Result<EssviParams> slice_res =
        (in.repair == CalendarRepair::MonotoneFit)
            ? fit_slice_calendar_floored(a, T, F, in.calib, &diag, has_prev ? &prev_slice : nullptr,
                                         std::exp(-rate * T))
            : essvi_fit_slice(a.obs, T, F, in.calib, &diag);
    if (profile) ms_fit += now_ns() - t_fit;
    if (!slice_res) {
      continue;  // a slice that fails to fit contributes no slice
    }
    prev_slice = *slice_res;  // carry forward for the next slice's calendar floor
    has_prev = true;

    // 4. Write the slice into the surface at the next ascending index.
    ATX_TRY_VOID(surface.set_slice_essvi(idx, *slice_res));
    expiry_T.push_back(T);

    // 5. Retain the per-slice re-pricing context for the composable facade, and
    //    stash the aligned obs so this slice can be SCORED after the surface is
    //    fully assembled and (optionally) calendar-repaired.
    context.push_back(SliceContext{T, F, d_res->borrow, q_eff, a.obs.size(),
                                   a.n_dropped});
    pending.push_back(PendingSlice{std::move(a), T, rate, q_eff, static_cast<std::uint16_t>(idx)});

    ++idx;
  }

  if (idx == 0) {
    return Err(ErrorCode::NotFound,
               "run_surface_parity: no expiry produced a usable eSSVI slice");
  }

  // 6. Calendar no-arbitrage on the assembled surface. Count the raw crossings
  //    BEFORE any repair (independent per-slice fits + wing extrapolation can
  //    cross in total variance), then optionally repair to arb-free.
  const double t_cal = profile ? now_ns() : 0.0;
  ATX_TRY(const std::vector<ArbViolation> pre_viols,
          arb_check_calendar(surface, kArbKMin, kArbKMax, kArbNGrid));
  const std::size_t n_calendar_viol_pre = pre_viols.size();
  double ms_repair = 0.0;

  if (in.repair == CalendarRepair::Project && n_calendar_viol_pre > 0) {
    // Backbone theta-bump restores calendar monotonicity of the eSSVI backbone;
    // the residual damper is a no-op for a backbone-only slice but keeps the
    // pass correct if a residual basis is ever fit here. Both are no-ops on a
    // non-eSSVI surface. Repair over the SAME grid the check samples so the
    // post-repair check is guaranteed clean. (MonotoneFit needs no post-hoc
    // pass — its theta floor already enforced ATM monotonicity during the fit.)
    const double t_rep = profile ? now_ns() : 0.0;
    ATX_TRY_VOID(arb_project_calendar_essvi(surface, kArbKMin, kArbKMax, kArbNGrid));
    ATX_TRY_VOID(arb_repair_calendar_residual(surface, kArbKMin, kArbKMax, kArbNGrid));
    if (profile) ms_repair = now_ns() - t_rep;
  }

  // 7. Score per-expiry re-Am parity off the FINAL (possibly repaired) surface:
  //    the model IV is read back via iv_on_slice, so the number scored is the
  //    one the surface actually serves.
  const double t_parity = profile ? now_ns() : 0.0;
  for (const PendingSlice& ps : pending) {
    std::vector<double> model_iv;
    model_iv.reserve(ps.a.k_log.size());
    for (const double k : ps.a.k_log) {
      model_iv.push_back(surface.iv_on_slice(ps.slice_idx, k));
    }

    ParityInputs pin{};
    pin.S = in.S;
    pin.r = ps.rate;
    pin.q_eff = ps.q_eff;
    pin.T = ps.T;
    pin.method = in.deam.method;
    pin.al_opts = in.deam.al_opts;
    pin.band_k = in.band_k;
    pin.n_curve_params = 3;
    pin.caches = in.deam.caches;  // re-Am through the same hot-path caches
    ATX_TRY(const ParityReport parity,
            chain_parity(ps.a.strike, ps.a.bid, ps.a.ask, ps.a.mid, ps.a.side,
                         model_iv, ps.a.market_iv, pin));
    worst = std::min(worst, parity.frac_fv_within_bidask);
    per_expiry.push_back(parity);
  }
  const double ms_parity = profile ? (now_ns() - t_parity) : 0.0;

  // 8. Final calendar check on the surface the caller receives.
  ATX_TRY(const std::vector<ArbViolation> cal_viols,
          arb_check_calendar(surface, kArbKMin, kArbKMax, kArbNGrid));
  const bool calendar_arb_free = cal_viols.empty();

  if (profile) {
    const double ms_cal = now_ns() - t_cal;
    std::fprintf(stderr,
                 "[ATX_VOL_PROFILE] slices=%zu deam=%.1f align=%.1f fit=%.1f "
                 "repair=%.1f parity=%.1f calendar=%.1f ms viol_pre=%zu "
                 "(deam=borrow+per-strike invert; align=OTM-leg invert; "
                 "parity=re-Am score)\n",
                 idx, ms_deam, ms_align, ms_fit, ms_repair, ms_parity, ms_cal,
                 n_calendar_viol_pre);
  }

  SurfaceParityReport out{
      std::move(surface),   std::move(expiry_T), std::move(per_expiry),
      std::move(context),   worst,               calendar_arb_free,
      idx,                  n_calendar_viol_pre,
  };
  return Ok(std::move(out));
}

}  // namespace atx::vol
