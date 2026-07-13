#include "atx/vol/session.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <utility>
#include <vector>

#include "atx/core/error.hpp"
#include "atx/vol/american.hpp"        // american_price, american_price_cached, american_greeks
#include "atx/vol/arb.hpp"             // arb_check_calendar (post-refit recheck)
#include "atx/vol/correction.hpp"      // CorrectionCache, AmericanCorrectionCaches
#include "atx/vol/curve_fit.hpp"       // fit_curve_surface (curve-agnostic driver)
#include "atx/vol/data.hpp"           // data_install
#include "atx/vol/dividend.hpp"        // hybrid_forward (representative carry)
#include "atx/vol/essvi_calib.hpp"     // essvi_fit_slice (warm-start refit)
#include "atx/vol/event_vol.hpp"       // EventSchedule, count_events_at, implied_emove
#include "atx/vol/parity.hpp"          // chain_parity (incremental diagnostic refresh)
#include "atx/vol/prepared_fitting.hpp" // CanonicalPreparedExpiry
#include "atx/vol/projection.hpp"      // InterpMode, surface_insert_vol_slice, w_on_inserted_slice
#include "atx/vol/surface_parity.hpp"  // run_surface_parity, SurfaceParityInputs/Report
#include "atx/vol/universe.hpp"        // Universe, Underlying, Uid, Chain
#include "atx/vol/vol_surface.hpp"     // VolSurface
#include "atx/vol/vol_time.hpp"        // ns_from_year_fraction (eMove solve)

// DESIGN / PARITY NOTES
// ---------------------
// * build() is the ONLY place the pipeline runs. It maps SessionInputs 1:1 onto
//   SurfaceParityInputs, drives run_surface_parity, then MOVES out the fitted
//   surface + per-slice context + per-expiry parity and keeps the pricing inputs
//   so the const queries never refit.
//
// * The queries reproduce run_surface_parity's own coordinates exactly: at a
//   query T equal to a slice's T, interp_forward returns that slice's (F, q_eff)
//   (the between-slices interpolation collapses with alpha == 0), so an on-slice
//   query re-prices on the identical forward/carry the fit was scored on, and the
//   surface's own iv(k, T) serves the vol — no side computation.
//
// * The hot path: when `use_correction_cache` is set, build() builds a per-side
//   Chebyshev correction cache over the chain's (k, T, sigma) box and routes
//   every American inversion (de-Am) and re-pricing (parity + the fair_value /
//   greeks queries) through `american_price_cached`. The same caches price both
//   legs, so the invert/re-price round-trip stays self-consistent; a null cache
//   (disabled, or a build failure) degrades transparently to cold Andersen-Lake.

namespace atx::vol {

using atx::core::Err;
using atx::core::ErrorCode;
using atx::core::Ok;

namespace {

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

// A finite, strictly-positive query coordinate.
[[nodiscard]] bool valid_query(double K, double T) noexcept {
  return std::isfinite(K) && (K > 0.0) && std::isfinite(T) && (T > 0.0);
}

[[nodiscard]] bool valid_term_rates(const SessionInputs &in) noexcept {
  if (in.expiry_rate_T.empty() && in.expiry_rates.empty()) {
    return true;
  }
  if (in.expiry_rate_T.size() != in.expiry_rates.size() || in.expiry_rate_T.empty()) {
    return false;
  }
  for (std::size_t i = 0; i < in.expiry_rates.size(); ++i) {
    if (!(in.expiry_rate_T[i] > 0.0) || !std::isfinite(in.expiry_rate_T[i]) ||
        !std::isfinite(in.expiry_rates[i]) ||
        (i > 0u && !(in.expiry_rate_T[i] > in.expiry_rate_T[i - 1u]))) {
      return false;
    }
  }
  return true;
}

[[nodiscard]] double input_rate_at(const SessionInputs &in, double T) noexcept {
  if (in.expiry_rates.empty()) {
    return in.r;
  }
  if (T <= in.expiry_rate_T.front()) {
    return in.expiry_rates.front();
  }
  if (T >= in.expiry_rate_T.back()) {
    return in.expiry_rates.back();
  }
  std::size_t hi = 1u;
  while (hi < in.expiry_rate_T.size() && in.expiry_rate_T[hi] <= T) {
    ++hi;
  }
  const std::size_t lo = hi - 1u;
  const double span = in.expiry_rate_T[hi] - in.expiry_rate_T[lo];
  const double alpha = (T - in.expiry_rate_T[lo]) / span;
  return in.expiry_rates[lo] + alpha * (in.expiry_rates[hi] - in.expiry_rates[lo]);
}

void retain_fitted_term_rates(SessionInputs &in, std::span<const SliceContext> context) {
  if (in.expiry_rates.empty()) {
    return;
  }
  std::vector<double> fitted_T;
  std::vector<double> fitted_rates;
  fitted_T.reserve(context.size());
  fitted_rates.reserve(context.size());
  for (const SliceContext &slice : context) {
    fitted_T.push_back(slice.T);
    fitted_rates.push_back(input_rate_at(in, slice.T));
  }
  in.expiry_rate_T = std::move(fitted_T);
  in.expiry_rates = std::move(fitted_rates);
}

// Solve eMove from the two fitted eSSVI expiries bracketing the FIRST
// scheduled event strictly after `now_ts_ns` and at/before the LAST fitted
// expiry. `slices` is the surface's own fitted eSSVI slices, ascending T
// (== `essvi_slices()`).
//
// `run_surface_parity`'s eSSVI fit loop (surface_parity.cpp) does NOT stamp
// `expiry_id`/`expiry_ns` onto the slices it produces (only a DIFFERENT,
// unused-by-this-path helper in essvi_calib.cpp does) -- verified empirically
// (both fields read back 0 on a freshly built session). So, exactly like the
// projection-layer SERVE path in `w_on_inserted_slice` (which has no real
// listed expiry for an arbitrary interpolated query T to begin with), each
// slice's absolute instant is SYNTHESIZED from its own `T` via
// `ns_from_year_fraction` (vol_time.hpp, the Calendar365 inverse of
// `time_to_expiry_years`) rather than read from `expiry_ns` -- this also
// keeps the solve step and the serve step internally consistent (both derive
// instants the SAME way).
//
// Returns NaN (never 0, matching the conservative calendar-guard convention
// a few lines above this function's call sites -- see the ArbCheckCalendar
// comment) on ANY failure: no schedule, fewer than two fitted slices, no
// event in that window, no fitted expiry strictly BEFORE the event to
// bracket against, or `implied_emove`'s own solve failure (no
// identification, or a negative-beyond-tolerance e^2).
//
// noexcept: every callee is itself noexcept (`events()`, `upper_bound` on
// int64s, `ns_from_year_fraction`, `count_events_at`, `essvi_total_w`)
// except `implied_emove`, whose only non-trivial operation is constructing
// an `Err` message string on a failure path -- the same
// treat-error-string-allocation-as-nonthrowing convention the pre-existing
// noexcept serve path already relies on (`shape_blend_total_variance` /
// `model_w` call `surface_insert_vol_slice`, which builds `Err` strings the
// same way).
[[nodiscard]] double solve_implied_emove(
    const EventSchedule* events, std::int64_t now_ts_ns,
    std::span<const EssviParams> slices) noexcept {
  if (events == nullptr || slices.size() < 2) {
    return kNaN;
  }
  const auto all = events->events();
  const auto it = std::upper_bound(all.begin(), all.end(), now_ts_ns);
  if (it == all.end()) {
    return kNaN;  // no event strictly after "now"
  }
  const std::int64_t event_ns = *it;
  const std::int64_t last_ns = ns_from_year_fraction(now_ts_ns, slices.back().T);
  if (event_ns > last_ns) {
    return kNaN;  // event falls after the last fitted expiry -- nothing to bracket
  }

  // "hi": first fitted slice at/after the event; "lo": the one just before
  // it. hi is guaranteed to be found (< slices.size()) by the check above.
  std::size_t hi = 0;
  while (hi < slices.size() &&
        ns_from_year_fraction(now_ts_ns, slices[hi].T) < event_ns) {
    ++hi;
  }
  if (hi == 0 || hi >= slices.size()) {
    return kNaN;  // event at/before the first fitted expiry -- no low bracket
  }
  const std::size_t lo = hi - 1;

  const EssviParams& s_lo = slices[lo];
  const EssviParams& s_hi = slices[hi];
  const double w1 = essvi_total_w(s_lo, 0.0);
  const double w2 = essvi_total_w(s_hi, 0.0);
  const std::size_t n1 = count_events_at(*events, now_ts_ns, s_lo.T);
  const std::size_t n2 = count_events_at(*events, now_ts_ns, s_hi.T);

  auto e = implied_emove(w1, s_lo.T, n1, w2, s_hi.T, n2);
  return e.has_value() ? *e : kNaN;
}

[[nodiscard]] SessionCarryDiagnostics compact_carry(
    const CarryDiagnostics& carry) noexcept {
  return SessionCarryDiagnostics{
      carry.n_candidates,
      carry.n_attempted,
      carry.n_solved,
      carry.n_retained,
      carry.effective_pair_count,
      carry.dispersion,
      carry.max_leave_one_out_shift,
      carry.confidence_half_width,
      carry.max_pcp_residual,
      true,
      carry.confident};
}

[[nodiscard]] std::size_t route_proposed(
    const DeAmAuditDiagnostics& d) noexcept {
  return static_cast<std::size_t>(d.shortcut.n_proposed) + d.cache.n_proposed +
         d.fast.n_proposed + d.accurate.n_proposed;
}

[[nodiscard]] std::size_t route_audited(
    const DeAmAuditDiagnostics& d) noexcept {
  return static_cast<std::size_t>(d.shortcut.n_audited) + d.cache.n_audited +
         d.fast.n_audited + d.accurate.n_audited;
}

[[nodiscard]] double route_max_residual(
    const DeAmAuditDiagnostics& d) noexcept {
  return std::max({d.shortcut.max_residual_half_spreads,
                   d.cache.max_residual_half_spreads,
                   d.fast.max_residual_half_spreads,
                   d.accurate.max_residual_half_spreads});
}

// Compatibility bridge until fit reports directly carry their compact input
// certification. Re-run only the input-resolution layer, retain counts and
// quantiles, and immediately release the temporary observations/pair details.
// `fit_rows_audited` states whether the FIT observations themselves ran the
// audited inversion route (curve-driver path: always; eSSVI path: only under
// deam.audit_fit_inversions) — a certificate computed off this diagnostic
// re-run must never vouch for fit rows that skipped the audit (§5.3/§8.1).
//
// `precomputed_carry` (perf C1): the eSSVI path (`run_surface_parity`) already
// ran `resolve_chain_forward` once per chain to fit the surface; when its
// per-slice `CarryDiagnostics` are handed in here (‖ `context`, same size),
// the carry re-derivation below is skipped — it would recompute the IDENTICAL
// function on the IDENTICAL arguments. The caller must therefore only pass a
// carry that WAS resolved with `in.deam`-equivalent options — in particular
// the same caches (review fix: the fit may resolve through session-built
// hot-path caches that `in.deam` never carries; such a carry is NOT valid
// here). Empty (the default) recomputes, the fallback for any caller that
// genuinely reaches certification without a certification-grade carry.
[[nodiscard]] std::vector<SessionSliceDiagnostics> collect_input_diagnostics(
    const Underlying& under, const SessionInputs& in,
    std::span<const SliceContext> context,
    const AmericanCorrectionCaches& deam_caches, bool fit_rows_audited,
    std::span<const CarryDiagnostics> precomputed_carry = {},
    std::vector<std::vector<FitObs>> *observation_cache = nullptr,
    std::vector<std::vector<double>> *source_mid_cache = nullptr,
    std::vector<std::vector<std::uint8_t>> *source_flag_cache = nullptr,
    std::vector<std::vector<double>> *chain_mid_cache = nullptr,
    std::vector<std::vector<std::uint8_t>> *chain_flag_cache = nullptr,
    std::vector<std::vector<double>> *chain_bid_cache = nullptr,
    std::vector<std::vector<double>> *chain_ask_cache = nullptr,
    std::vector<std::vector<std::int64_t>> *chain_ts_cache = nullptr) {
  std::vector<SessionSliceDiagnostics> out;
  out.reserve(context.size());
  if (observation_cache != nullptr) observation_cache->reserve(context.size());
  if (source_mid_cache != nullptr) source_mid_cache->reserve(context.size());
  if (source_flag_cache != nullptr) source_flag_cache->reserve(context.size());
  if (chain_mid_cache != nullptr) chain_mid_cache->reserve(context.size());
  if (chain_flag_cache != nullptr) chain_flag_cache->reserve(context.size());
  if (chain_bid_cache != nullptr) chain_bid_cache->reserve(context.size());
  if (chain_ask_cache != nullptr) chain_ask_cache->reserve(context.size());
  if (chain_ts_cache != nullptr) chain_ts_cache->reserve(context.size());
  const bool use_precomputed_carry = precomputed_carry.size() == context.size();
  std::size_t chain_pos = 0;
  // Indexed loop: `slice_idx` is the ‖-vector ordinal into both `context` and
  // `precomputed_carry`, advanced unconditionally per iteration (review fix:
  // a manually-incremented counter missed the chain-found path and served
  // slice 0's carry to every slice).
  for (std::size_t slice_idx = 0; slice_idx < context.size(); ++slice_idx) {
    const SliceContext& slice = context[slice_idx];
    if (observation_cache != nullptr) observation_cache->emplace_back();
    if (source_mid_cache != nullptr) source_mid_cache->emplace_back();
    if (source_flag_cache != nullptr) source_flag_cache->emplace_back();
    if (chain_mid_cache != nullptr) chain_mid_cache->emplace_back();
    if (chain_flag_cache != nullptr) chain_flag_cache->emplace_back();
    if (chain_bid_cache != nullptr) chain_bid_cache->emplace_back();
    if (chain_ask_cache != nullptr) chain_ask_cache->emplace_back();
    if (chain_ts_cache != nullptr) chain_ts_cache->emplace_back();
    SessionSliceDiagnostics sd{};
    sd.T = slice.T;
    while (chain_pos < under.chains.size() &&
           under.chains[chain_pos].T < slice.T - 1.0e-12) {
      ++chain_pos;
    }
    if (chain_pos >= under.chains.size() ||
        std::fabs(under.chains[chain_pos].T - slice.T) >
            1.0e-10 * std::max(1.0, slice.T)) {
      out.push_back(std::move(sd));
      continue;
    }
    const Chain& chain = under.chains[chain_pos++];
    if (chain_mid_cache != nullptr) chain_mid_cache->back() = chain.mids;
    if (chain_flag_cache != nullptr) chain_flag_cache->back() = chain.flags;
    if (chain_bid_cache != nullptr) chain_bid_cache->back() = chain.bids;
    if (chain_ask_cache != nullptr) chain_ask_cache->back() = chain.asks;
    if (chain_ts_cache != nullptr) chain_ts_cache->back() = chain.ts_ns;
    const double rate = input_rate_at(in, slice.T);
    if (use_precomputed_carry) {
      sd.carry = compact_carry(precomputed_carry[slice_idx]);
    } else {
      const auto carry = resolve_chain_forward(
          chain, in.S, rate, in.cash_divs, in.now_ts_ns, in.deam);
      if (carry) {
        sd.carry = compact_carry(carry->carry);
      }
    }

    const double df = std::exp(-rate * slice.T);
    const auto obs = build_observations_european(
        chain, in.S, rate, slice.forward, slice.T, df, in.calib, deam_caches,
        in.deam.al_opts, in.deam.iv_tol, in.deam.iv_max_iter,
        in.deam.method);
    if (obs) {
      sd.inversion = obs->deam_audit;
      sd.inversion_available = true;
      // Honest certificate (§5.3/§8.1): the FIT rows themselves must have run
      // the audited route, every accepted node must have passed the cold-
      // reference budget, and tolerated node drops (failed inversion or an
      // over-budget residual — excluded from the fit, counted in diagnostics)
      // must stay under the configured cap. Fail-closed at the cap, not at the
      // first bad quote; non-AndersenLake methods have no audit and never
      // certify (deam_inversion_certified enforces both).
      sd.inversion_certified =
          fit_rows_audited &&
          deam_inversion_certified(sd.inversion,
                                   in.calib.max_certified_deam_drop_fraction);
      if (observation_cache != nullptr && source_mid_cache != nullptr &&
          source_flag_cache != nullptr) {
        observation_cache->back() = obs->obs;
        source_mid_cache->back().reserve(obs->obs.size());
        source_flag_cache->back().reserve(obs->obs.size());
        for (const FitObs &fit_obs : obs->obs) {
          const auto strike_it =
              std::lower_bound(chain.strikes.begin(), chain.strikes.end(), fit_obs.K);
          if (strike_it == chain.strikes.end() || *strike_it != fit_obs.K) {
            observation_cache->back().clear();
            source_mid_cache->back().clear();
            source_flag_cache->back().clear();
            break;
          }
          const auto strike_idx = static_cast<std::uint16_t>(
              std::distance(chain.strikes.begin(), strike_it));
          const std::size_t quote_idx = chain_index(strike_idx, fit_obs.side);
          if (quote_idx >= chain.mids.size() || quote_idx >= chain.flags.size()) {
            observation_cache->back().clear();
            source_mid_cache->back().clear();
            source_flag_cache->back().clear();
            break;
          }
          source_mid_cache->back().push_back(chain.mids[quote_idx]);
          source_flag_cache->back().push_back(chain.flags[quote_idx]);
        }
      }
    }
    out.push_back(std::move(sd));
  }
  return out;
}

void aggregate_input_diagnostics(
    std::span<const SessionSliceDiagnostics> slices,
    SessionDiagnostics& diag) noexcept {
  double min_effective = std::numeric_limits<double>::infinity();
  bool all_inversion_certified = !slices.empty();
  for (const SessionSliceDiagnostics& slice : slices) {
    if (slice.carry.available) {
      ++diag.n_carry_slices;
      if (slice.carry.confident) ++diag.n_carry_confident;
      min_effective = std::min(min_effective, slice.carry.effective_pair_count);
      diag.max_carry_dispersion =
          std::max(diag.max_carry_dispersion, slice.carry.dispersion);
      diag.max_carry_leave_one_out =
          std::max(diag.max_carry_leave_one_out,
                   slice.carry.max_leave_one_out_shift);
    }
    if (slice.inversion_available) {
      ++diag.n_inversion_slices;
      diag.n_iv_proposed += route_proposed(slice.inversion);
      diag.n_iv_audited += route_audited(slice.inversion);
      diag.n_iv_fallback += slice.inversion.n_accurate_fallback;
      diag.n_iv_rejected_residual += slice.inversion.n_rejected_residual;
      diag.max_iv_proposal_residual_half_spreads =
          std::max(diag.max_iv_proposal_residual_half_spreads,
                   route_max_residual(slice.inversion));
    }
    all_inversion_certified =
        all_inversion_certified && slice.inversion_available &&
        slice.inversion_certified;
  }
  diag.min_carry_effective_pairs =
      std::isfinite(min_effective) ? min_effective : 0.0;
  diag.carry_confident = diag.n_slices > 0 &&
                         diag.n_carry_slices == diag.n_slices &&
                         diag.n_carry_confident == diag.n_slices;
  diag.inversion_certified = diag.n_slices > 0 &&
                             slices.size() == diag.n_slices &&
                             all_inversion_certified;
}

}  // namespace

VolaSession::VolaSession(VolSurface&& surface, std::vector<SliceContext>&& ctx,
                         std::vector<ParityReport>&& parity, SessionInputs in,
                         const SessionDiagnostics& diag,
                         std::vector<SessionSliceDiagnostics>&& slice_diag,
                         std::optional<CorrectionCache>&& corr_call,
                         std::optional<CorrectionCache>&& corr_put,
                         std::optional<CurveSurface>&& curve_override)
    : surface_{std::move(surface)},
      ctx_{std::move(ctx)},
      parity_{std::move(parity)},
      in_{std::move(in)},
      diag_{diag},
      slice_diag_{std::move(slice_diag)},
      corr_call_{std::move(corr_call)},
      corr_put_{std::move(corr_put)},
      curve_override_{std::move(curve_override)} {}

namespace {

// Per-side correction caches built for a session (empty => cold fallback).
struct BuiltCaches {
  std::optional<CorrectionCache> call;
  std::optional<CorrectionCache> put;
};

// Build both per-side Chebyshev correction caches over the underlying's
// (k_log, T, sigma) box. A side whose build fails is left empty, so the pipeline
// transparently falls back to the cold Andersen-Lake path for that side.
[[nodiscard]] BuiltCaches build_session_caches(const Underlying& under,
                                               const SessionInputs& in) {
  BuiltCaches out;
  if (under.chains.empty() || !(in.S > 0.0)) {
    return out;
  }
  const double S = in.S;

  // (k_log, T) box from every strike / expiry, using spot as the forward proxy.
  double k_min = std::numeric_limits<double>::infinity();
  double k_max = -std::numeric_limits<double>::infinity();
  double T_lo = std::numeric_limits<double>::infinity();
  double T_hi = -std::numeric_limits<double>::infinity();
  for (const Chain& c : under.chains) {
    if (!(c.T > 0.0)) {
      continue;
    }
    T_lo = std::min(T_lo, c.T);
    T_hi = std::max(T_hi, c.T);
    for (const double K : c.strikes) {
      if (!(K > 0.0)) {
        continue;
      }
      const double k = std::log(K / S);
      k_min = std::min(k_min, k);
      k_max = std::max(k_max, k);
    }
  }
  if (!std::isfinite(k_min) || !std::isfinite(k_max) || !(k_max > k_min) ||
      !std::isfinite(T_lo) || !(T_lo > 0.0)) {
    return out;  // degenerate box -> cold path everywhere
  }

  // Pad the box and keep T strictly ordered even for a single expiry.
  k_min -= 0.05;
  k_max += 0.05;
  const double T_min = 0.9 * T_lo;
  const double T_max = (T_hi > T_lo) ? (1.1 * T_hi) : (1.5 * T_lo);
  constexpr double kSigMin = 0.05;
  constexpr double kSigMax = 1.5;

  // Representative carry q_rep from the mid expiry's zero-borrow hybrid forward
  // (F = S*e^{(r-q)T}). The correction is baked at this single carry; the
  // Black-76 leg always uses the real per-quote q_eff, and the small carry
  // mismatch cancels in the self-consistent invert/re-price round-trip.
  const Chain& mid = under.chains[under.chains.size() / 2];
  double q_rep = in.r;
  if (mid.T > 0.0) {
    const double F_rep = hybrid_forward(S, in.r, 0.0, mid.T, in.cash_divs,
                                        mid.expiry_ns, in.now_ts_ns, in.deam.hyb);
    if (F_rep > 0.0 && std::isfinite(F_rep)) {
      q_rep = in.r - std::log(F_rep / S) / mid.T;
    }
  }

  constexpr std::uint16_t kNK = 16;
  constexpr std::uint16_t kNT = 8;
  constexpr std::uint16_t kNS = 12;
  Result<CorrectionCache> cc =
      CorrectionCache::build(kNK, kNT, kNS, in.r, q_rep, k_min, k_max, T_min,
                             T_max, kSigMin, kSigMax, Side::Call, in.deam.al_opts);
  if (cc) {
    out.call = std::move(*cc);
  }
  Result<CorrectionCache> pp =
      CorrectionCache::build(kNK, kNT, kNS, in.r, q_rep, k_min, k_max, T_min,
                             T_max, kSigMin, kSigMax, Side::Put, in.deam.al_opts);
  if (pp) {
    out.put = std::move(*pp);
  }
  return out;
}

}  // namespace

void apply_fit_preset(SessionInputs& in, FitPreset preset) noexcept {
  // Shared across every preset: route the American inversions / re-pricing
  // through the correction-cache hot path.
  in.use_correction_cache = true;
  in.score_parity = true;
  in.enforce_calendar_floor = true;
  in.use_deam_cache_for_fit = false;
  in.calib.max_obs_per_slice = 0;
  in.calib.max_otm_shortcut_premium_spread_frac = 0.0;
  in.deam.method = AmericanMethod::AndersenLake;
  in.deam.max_borrow_pairs = 12;
  switch (preset) {
    case FitPreset::Fast:
      // Fast surface-fit path: the fast Andersen-Lake preset with the inversion
      // tol matched to its ~1e-4 accuracy floor (a tighter tol collapses
      // safeguarded Newton into bisection and slows the fit), and a single ATM
      // borrow pair (the term borrow the smile then absorbs into log-moneyness).
      in.deam.al_opts = al_fast_opts();
      in.deam.iv_tol = 1.0e-5;
      in.deam.n_atm = 1;
      // Fast leaves the raw eSSVI surface and still scores parity diagnostics.
      in.calendar_repair = CalendarRepair::None;
      break;
    case FitPreset::Hft:
      in.deam.al_opts = al_fast_opts();
      in.deam.iv_tol = 1.0e-5;
      in.deam.n_atm = 1;
      in.deam.max_borrow_pairs = 1;
      in.curve.kind = VolCurveKind::LinearVariance;
      in.calib.max_obs_per_slice = 48;
      in.calib.max_otm_shortcut_premium_spread_frac = 0.50;
      in.use_correction_cache = false;
      in.score_parity = false;
      in.enforce_calendar_floor = false;
      in.use_deam_cache_for_fit = false;
      in.calendar_repair = CalendarRepair::None;
      break;
    case FitPreset::Accurate:
    case FitPreset::Robust:
      // Reference fidelity: the ACCURATE Andersen-Lake preset (pinned explicitly
      // so build() does not substitute the fast preset), a tight inversion tol,
      // and three ATM borrow pairs.
      in.deam.al_opts = al_default_opts();
      in.deam.iv_tol = 1.0e-7;
      in.deam.n_atm = 3;
      // NOTE on the wing-residual layer: measured OFF here deliberately. On real
      // SPY OPRA the eSSVI backbone alone already fits the tradeable smile to
      // ~1.0 vol pt vega-weighted; enabling the additive HingeQuad residual moves
      // that headline by ~0 (it only reshapes the low-vega deep wings, which the
      // vega weighting discounts) and OVER-FITS sparse event wings (a lone deep
      // put can swing ~50 vol pts) — matching the existing profile.cpp finding.
      // So it stays at its default (disabled); accuracy comes from the backbone.
      // Robust makes the surface calendar-arb-free near-money at held quality;
      // Accurate reports the raw calendar status without altering the fit.
      in.calendar_repair =
          (preset == FitPreset::Robust) ? CalendarRepair::MonotoneFit
                                        : CalendarRepair::None;
      break;
  }
}

SessionInputs make_session_inputs(FitPreset preset, double S, double r,
                                  std::int64_t now_ts_ns) {
  SessionInputs in;
  in.S = S;
  in.r = r;
  in.now_ts_ns = now_ts_ns;
  apply_fit_preset(in, preset);
  return in;
}

Result<VolaSession> VolaSession::build(const Underlying& under,
                                       const SessionInputs& in) {
  // The session is the fast production fit path: de-Americanize and sample the
  // correction cache with the fast ALO preset unless the caller pinned an
  // explicit accuracy. IV inversion / cache sampling only need ~1e-4 price
  // accuracy (surface RMSE is ~1e-2), so the high-precision (nullopt) preset is
  // wasted cost here. `eff` carries this default onto BOTH the cache build and
  // the parity run, and is the copy stored for the const queries so the cold
  // fair_value/greeks fallback prices on the same scheme it was fit with.
  SessionInputs eff = in;
  ATX_TRY_VOID(validate_calib_options(eff.calib));
  ATX_TRY_VOID(validate_calib_options(eff.curve.parametric));
  if (!valid_term_rates(eff)) {
    return Err(ErrorCode::InvalidArgument, "VolaSession::build: invalid expiry rate vectors");
  }
  if (!eff.expiry_rates.empty()) {
    eff.use_correction_cache = false;
    eff.use_deam_cache_for_fit = false;
  }
  if (!eff.deam.al_opts) {
    eff.deam.al_opts = al_fast_opts();
    // Match the inversion tol to the fast pricer's ~1e-4 accuracy floor. 1e-5 is
    // still 3 orders below the ~1e-2 surface RMSE, so quality is unaffected, but
    // it lets the American-IV Newton converge instead of stalling into bisection
    // (each bisection step is a full American solve). Only applied when the fast
    // preset is auto-selected; a caller pinning al_opts keeps the tight default.
    eff.deam.iv_tol = 1.0e-5;
    // Borrow from the single closest-ATM co-terminal pair. Each extra pair runs
    // its own borrow fixed-point (both legs re-inverted per iteration) for a
    // borrow the smile then absorbs into log-moneyness; one well-chosen pair is
    // the dominant term-structure driver at a fraction of the cost.
    eff.deam.n_atm = 1;
  }

  // SessionInputs -> SurfaceParityInputs (1:1; run_surface_parity validates S/r).
  SurfaceParityInputs sp;
  sp.S = eff.S;
  sp.r = eff.r;
  sp.expiry_rate_T = eff.expiry_rate_T;
  sp.expiry_rates = eff.expiry_rates;
  sp.cash_divs = eff.cash_divs;
  sp.now_ts_ns = eff.now_ts_ns;
  sp.deam = eff.deam;
  sp.calib = eff.calib;
  sp.band_k = eff.band_k;
  sp.repair = eff.calendar_repair;
  sp.score_parity = eff.score_parity;
  sp.enforce_calendar_floor = eff.enforce_calendar_floor;
  sp.use_deam_cache_for_fit = eff.use_deam_cache_for_fit;
  sp.fit_prep_policy = eff.fit_prep_policy;

  // SOTA hot path: build per-side correction caches and route every American
  // inversion (de-Am) + re-pricing (parity) through the cached pricer. The
  // caches are locals whose pointers feed run_surface_parity, then are MOVED
  // into the session for the const queries. Empty (build failed / disabled) =>
  // the cold Andersen-Lake path, transparently.
  BuiltCaches caches;
  if (eff.use_correction_cache) {
    caches = build_session_caches(under, eff);
    sp.deam.caches = AmericanCorrectionCaches{
        caches.call ? &*caches.call : nullptr,
        caches.put ? &*caches.put : nullptr};
    // Review fix (perf C1): the certification layer historically resolved
    // carry with the CALLER's deam options (eff.deam — whose caches this
    // session never populates), not the session-built hot-path caches now on
    // sp.deam. Hand the prepass the caller's caches so the certification
    // carry it exports reproduces that serial pass bit-for-bit.
    sp.deam_cert_caches = eff.deam.caches;
  }

  // ── Curve-family dispatch ──────────────────────────────────────────────────
  // Default (Essvi) keeps the byte-identical run_surface_parity path below.
  // ConvexDense / Svi fit through the curve-agnostic driver and are SERVED via
  // the polymorphic-surface override — this is how PricerFitter reaches the
  // 99.5%-in-band convex dense fit (previously bench-only).
  if (eff.curve.kind != VolCurveKind::Essvi) {
    ATX_TRY(CurveSurfaceReport crep, fit_curve_surface(under, sp, eff.curve));

    SessionDiagnostics cdiag{};
    cdiag.n_slices = crep.n_slices;
    // `cdiag.implied_emove` intentionally stays at its NaN default here:
    // SessionInputs::events / the event-aware blend is eSSVI-default only
    // (same restriction as ShapeBlend -- see SessionInputs::events), and a
    // polymorphic-override surface has no eSSVI slices to solve it from.
    // Calendar no-arb across slices, measured on the served CurveSurface. Each
    // convex slice is butterfly-arb-free by construction; this is the missing
    // half. k-range spans a wide moneyness band around the money.
    {
      constexpr double kBand = 0.60;   // log-moneyness half-width to sample
      constexpr std::uint32_t kGrid = 64;
      const auto cal = arb_check_calendar(crep.surface, -kBand, kBand, kGrid);
      // A failed check must not read as "verified arb-free" (the prior bug:
      // `cal ? cal->size() : 0` treated a failed check as zero violations,
      // i.e. clean). Match the conservative sibling in
      // VolaSession::refit_slice below: a failed check reports NOT verified
      // (calendar_arb_free = false), never a false "clean" via a zero count.
      // n_calendar_viol_pre must still satisfy the
      // calendar_arb_free == (n_calendar_viol_pre == 0) invariant relied on
      // by spy_real_test.cpp, so an unverified check is stamped with a
      // nonzero sentinel (1) rather than a real (unknowable) count.
      cdiag.calendar_arb_free = cal.has_value() && cal->empty();
      cdiag.n_calendar_viol_pre = cal.has_value() ? cal->size() : std::size_t{1};
      // I-2: independent self-check of each ConvexDense slice's OWN served
      // call_price(), which the w-space oracle cannot see (0 for a non-
      // ConvexDense session; see SessionDiagnostics::n_price_bound_violations).
      const auto price_bounds = arb_check_price_bounds(crep.surface, -kBand, kBand, kGrid);
      cdiag.n_price_bound_violations = price_bounds ? price_bounds->size() : 0;
    }
    {
      double worst = std::numeric_limits<double>::infinity();
      double sum_frac = 0.0, sum_chi2 = 0.0, sum_rmse = 0.0;
      std::size_t np_scored = 0;
      for (const ParityReport& p : crep.per_expiry) {
        if (p.n == 0) {
          continue;
        }
        worst = std::min(worst, p.frac_fv_within_bidask);
        sum_frac += p.frac_fv_within_bidask;
        sum_chi2 += p.chi2_reduced;
        sum_rmse += p.rmse_mid_vol;
        cdiag.n_bid_miss += p.band.n_bid_miss;
        cdiag.n_ask_miss += p.band.n_ask_miss;
        cdiag.max_prc_err = std::max(cdiag.max_prc_err, p.band.max_prc_err);
        ++np_scored;
      }
      if (np_scored > 0) {
        const double dn = static_cast<double>(np_scored);
        cdiag.worst_frac_within_bidask = worst;
        cdiag.mean_frac_within_bidask = sum_frac / dn;
        cdiag.mean_chi2_reduced = sum_chi2 / dn;
        cdiag.mean_rmse_vol = sum_rmse / dn;
      }
      if (!eff.score_parity) {
        cdiag.parity_state = ParityDiagnosticState::Disabled;
      } else if (np_scored == cdiag.n_slices && np_scored > 0u) {
        cdiag.parity_state = ParityDiagnosticState::Valid;
      } else {
        cdiag.parity_state = ParityDiagnosticState::Failed;
      }
      std::size_t nq = 0;
      for (const SliceContext& c : crep.context) {
        nq += c.n_used;
      }
      cdiag.n_quotes = nq;
    }

    // Placeholder eSSVI VolSurface: queries read the override, so surface_ is
    // unused, but VolaSession holds one by value. Cap >= 1 for create().
    ATX_TRY(VolSurface placeholder,
            VolSurface::create(under.uid, Parametrization::Essvi,
                               std::max<std::size_t>(std::size_t{1},
                                                     under.chains.size())));
    std::vector<std::vector<FitObs>> incremental_obs;
    std::vector<std::vector<double>> incremental_mids;
    std::vector<std::vector<std::uint8_t>> incremental_flags;
    std::vector<std::vector<double>> incremental_chain_mids;
    std::vector<std::vector<std::uint8_t>> incremental_chain_flags;
    std::vector<std::vector<double>> incremental_chain_bids;
    std::vector<std::vector<double>> incremental_chain_asks;
    std::vector<std::vector<std::int64_t>> incremental_chain_ts;
    // Perf C1: the curve driver's parallel prepass (run_deam_prepass,
    // curve_fit.cpp) already ran resolve_chain_forward + build_observations_
    // european for every chain -- with the EXACT same (S, rate, forward, T,
    // df, calib, deam caches/opts) collect_input_diagnostics used to
    // re-derive here a second time, serially. Consume `crep.input_certification`
    // directly instead: it is ‖ `crep.context`/`crep.per_expiry` (same commit
    // loop, same order), so this is a straight move, not a re-run. Fit rows on
    // this path are all audited in-line by construction (unconditional true,
    // matching the removed call's `fit_rows_audited=true`).
    std::vector<SessionSliceDiagnostics> slice_diag;
    slice_diag.reserve(crep.context.size());
    for (std::size_t i = 0; i < crep.context.size(); ++i) {
      SliceInputCertification& cert = crep.input_certification[i];
      SessionSliceDiagnostics sd{};
      sd.T = crep.context[i].T;
      // carry_available=false == the certification resolve failed: leave the
      // default (unavailable) carry, exactly as the removed serial pass did
      // when ITS resolve_chain_forward call failed.
      if (cert.carry_available) {
        sd.carry = compact_carry(cert.carry);
      }
      sd.inversion = cert.inversion;
      sd.inversion_available = true;
      sd.inversion_certified = deam_inversion_certified(
          sd.inversion, eff.calib.max_certified_deam_drop_fraction);
      slice_diag.push_back(sd);
      incremental_obs.push_back(std::move(cert.obs));
      incremental_mids.push_back(std::move(cert.source_mids));
      incremental_flags.push_back(std::move(cert.source_flags));
      incremental_chain_mids.push_back(std::move(cert.chain_mids));
      incremental_chain_flags.push_back(std::move(cert.chain_flags));
      incremental_chain_bids.push_back(std::move(cert.chain_bids));
      incremental_chain_asks.push_back(std::move(cert.chain_asks));
      incremental_chain_ts.push_back(std::move(cert.chain_ts));
    }
    aggregate_input_diagnostics(slice_diag, cdiag);
    cdiag.n_carry_skipped_expiries = crep.n_carry_skipped;
    retain_fitted_term_rates(eff, crep.context);
    VolaSession session{std::move(placeholder), std::move(crep.context),
                        std::move(crep.per_expiry), std::move(eff), cdiag,
                        std::move(slice_diag), std::move(caches.call),
                        std::move(caches.put),
                        std::optional<CurveSurface>{std::move(crep.surface)}};
    session.incremental_observations_ =
        std::make_shared<const IncrementalObservationStore>(
            IncrementalObservationStore{std::move(incremental_obs),
                                        std::move(incremental_mids),
                                        std::move(incremental_flags),
                                        std::move(incremental_chain_mids),
                                        std::move(incremental_chain_flags),
                                        std::move(incremental_chain_bids),
                                        std::move(incremental_chain_asks),
                                        std::move(incremental_chain_ts)});
    return Ok(std::move(session));
  }

  ATX_TRY(SurfaceParityReport rep, run_surface_parity(under, sp));

  // Aggregate diagnostics from the per-expiry parity + per-slice context BEFORE
  // moving those vectors into the session.
  SessionDiagnostics diag{};
  diag.n_slices = rep.n_slices;
  diag.calendar_arb_free = rep.calendar_arb_free;
  diag.n_calendar_viol_pre = rep.n_calendar_viol_pre;

  double worst = std::numeric_limits<double>::infinity();
  double sum_frac = 0.0;
  double sum_chi2 = 0.0;
  double sum_rmse = 0.0;
  for (const ParityReport& p : rep.per_expiry) {
    worst = std::min(worst, p.frac_fv_within_bidask);
    sum_frac += p.frac_fv_within_bidask;
    sum_chi2 += p.chi2_reduced;
    sum_rmse += p.rmse_mid_vol;
    diag.n_bid_miss += p.band.n_bid_miss;
    diag.n_ask_miss += p.band.n_ask_miss;
    diag.max_prc_err = std::max(diag.max_prc_err, p.band.max_prc_err);
  }
  const std::size_t np = rep.per_expiry.size();
  if (np > 0) {
    const double dnp = static_cast<double>(np);
    diag.worst_frac_within_bidask = worst;
    diag.mean_frac_within_bidask = sum_frac / dnp;
    diag.mean_chi2_reduced = sum_chi2 / dnp;
    diag.mean_rmse_vol = sum_rmse / dnp;
  }
  // The legacy eSSVI compatibility driver intentionally always scores parity,
  // even when the generic-family opt-out is false. State records what actually
  // happened, not the ignored compatibility flag.
  diag.parity_state = (np == diag.n_slices && np > 0u) ? ParityDiagnosticState::Valid
                                                       : ParityDiagnosticState::Failed;

  std::size_t n_quotes = 0;
  for (const SliceContext& c : rep.context) {
    n_quotes += c.n_used;
  }
  diag.n_quotes = n_quotes;

  // eMove policy v1 (SessionInputs::events, eSSVI-default path only): solve
  // once, post-fit, off the surface's OWN fitted eSSVI slices; NaN on any
  // failure (see solve_implied_emove's doc) so a bad/absent schedule never
  // silently changes what gets served.
  //
  // Calendar365-only restriction (same shape as the polymorphic-override
  // restriction just above, in the ConvexDense/Svi branch): `solve_
  // implied_emove` synthesizes each fitted slice's absolute expiry instant
  // from its own T via `ns_from_year_fraction`, the Calendar365 INVERSE of
  // `time_to_expiry_years`. Under `eff.time.convention == VolTime` a fitted
  // T is vol-time-shaped, not a plain calendar year-fraction, so that
  // synthesized instant would not be the real listed expiry and could
  // mis-bucket a nearby event by days -- silently, with no error, since the
  // arithmetic is otherwise well-defined. So skip the solve entirely under
  // any non-Calendar365 convention; `implied_emove` stays at its NaN
  // default, which `event_aware_active()` (session.hpp) already treats as
  // "serve exactly as if events were null" -- no separate gate needed on the
  // serve side. Root-cause fix (stamping `expiry_ns` directly onto fitted
  // eSSVI slices instead of synthesizing it from T) is a follow-up task.
  diag.implied_emove = (eff.time.convention == TimeConvention::Calendar365)
      ? solve_implied_emove(eff.events.get(), eff.now_ts_ns, rep.surface.essvi_slices())
      : kNaN;

  std::vector<std::vector<FitObs>> incremental_obs;
  std::vector<std::vector<double>> incremental_mids;
  std::vector<std::vector<std::uint8_t>> incremental_flags;
  std::vector<std::vector<double>> incremental_chain_mids;
  std::vector<std::vector<std::uint8_t>> incremental_chain_flags;
  std::vector<std::vector<double>> incremental_chain_bids;
  std::vector<std::vector<double>> incremental_chain_asks;
  std::vector<std::vector<std::int64_t>> incremental_chain_ts;
  // The eSSVI path fits from build_aligned_obs: its inversions run the audited
  // route only when deam.audit_fit_inversions is set (the risk serving
  // policy); otherwise the certificate must stay false — the diagnostic
  // builder's audits describe rows the fit never used (carry C1). The obs/
  // audit recompute below is therefore genuinely independent of the fit (no
  // prepass to reuse), but the CARRY resolution IS a literal duplicate of what
  // `run_surface_parity` already computed per chain — pass `rep.carry` (perf
  // C1) so collect_input_diagnostics skips that one redundant
  // resolve_chain_forward call per chain. Review fix: the reuse is only valid
  // when the fit resolved carry with EXACTLY the caches the certification
  // layer uses (eff.deam's — the caller's, never the session-built hot-path
  // caches on sp.deam). When they differ, pass an empty span so the fallback
  // recompute runs — the historical serial path, bit-for-bit.
  const bool fit_carry_matches_certification =
      sp.deam.caches.call == eff.deam.caches.call &&
      sp.deam.caches.put == eff.deam.caches.put;
  const std::span<const CarryDiagnostics> certification_carry =
      fit_carry_matches_certification
          ? std::span<const CarryDiagnostics>(rep.carry)
          : std::span<const CarryDiagnostics>{};
  std::vector<SessionSliceDiagnostics> slice_diag = collect_input_diagnostics(
      under, eff, rep.context, sp.deam.caches,
      /*fit_rows_audited=*/eff.deam.audit_fit_inversions, certification_carry,
      &incremental_obs, &incremental_mids, &incremental_flags,
      &incremental_chain_mids, &incremental_chain_flags,
      &incremental_chain_bids, &incremental_chain_asks, &incremental_chain_ts);
  aggregate_input_diagnostics(slice_diag, diag);
  diag.n_carry_skipped_expiries = rep.n_carry_skipped;
  diag.n_audit_starved_expiries = rep.n_audit_starved;

  retain_fitted_term_rates(eff, rep.context);
  VolaSession session{std::move(rep.surface), std::move(rep.context),
                      std::move(rep.per_expiry), std::move(eff), diag,
                      std::move(slice_diag), std::move(caches.call),
                      std::move(caches.put), std::optional<CurveSurface>{}};
  session.incremental_observations_ =
      std::make_shared<const IncrementalObservationStore>(
          IncrementalObservationStore{std::move(incremental_obs),
                                      std::move(incremental_mids),
                                      std::move(incremental_flags),
                                      std::move(incremental_chain_mids),
                                      std::move(incremental_chain_flags),
                                      std::move(incremental_chain_bids),
                                      std::move(incremental_chain_asks),
                                      std::move(incremental_chain_ts)});
  return Ok(std::move(session));
}

Result<VolaSession> VolaSession::from_frame(const QuoteFrame& frame,
                                            const SessionInputs& in) {
  // Mixed-convention guard: the frame carries the T convention it was built
  // under (`QuoteFrame::time`, read by `data_install` for Chain::T) and the
  // session retains its own copy (`SessionInputs::time`, see its doc). If they
  // disagree the session would fit chains under one clock while recording the
  // other — fail loudly instead of building a mixed-convention session.
  if (!(in.time == frame.time)) {
    return Err(ErrorCode::InvalidArgument,
               "VolaSession::from_frame: SessionInputs::time does not match frame.time "
               "(mixed-convention session); copy the frame's TimeSpec (e.g. OpraPanel::time) "
               "into SessionInputs::time");
  }
  Universe u;
  ATX_TRY(const Uid uid, data_install(u, frame));
  ATX_TRY(Underlying* under, u.get_underlying(uid));
  return build(*under, in);
}

VolaSession VolaSession::clone() const {
  std::optional<CurveSurface> curve_copy;
  if (curve_override_.has_value()) {
    curve_copy.emplace(curve_override_->clone());
  }
  VolaSession copy{VolSurface{surface_},
                   std::vector<SliceContext>{ctx_},
                   std::vector<ParityReport>{parity_},
                   SessionInputs{in_},
                   diag_,
                   std::vector<SessionSliceDiagnostics>{slice_diag_},
                   std::optional<CorrectionCache>{corr_call_},
                   std::optional<CorrectionCache>{corr_put_},
                   std::move(curve_copy)};
  copy.incremental_observations_ = incremental_observations_;
  return copy;
}

VolaSession::ForwardCarry VolaSession::interp_forward(double T) const noexcept {
  // Precondition: ctx_ is non-empty and ascending in T (build guarantees it).
  const SliceContext& first = ctx_.front();
  const SliceContext& last = ctx_.back();
  if (T <= first.T) {
    return ForwardCarry{first.forward, first.q_eff, input_rate_at(in_, T)};
  }
  if (T >= last.T) {
    return ForwardCarry{last.forward, last.q_eff, input_rate_at(in_, T)};
  }

  // Strictly between the endpoints: find the first slice whose T exceeds the
  // query, then linearly interpolate the bracketing pair. `hi >= 1` because
  // T > first.T; `hi < size` because T < last.T.
  std::size_t hi = 0;
  while (hi < ctx_.size() && ctx_[hi].T <= T) {
    ++hi;
  }
  const std::size_t lo = hi - 1;
  const SliceContext& a = ctx_[lo];
  const SliceContext& b = ctx_[hi];
  const double span = b.T - a.T;
  const double alpha = (span > 0.0) ? (T - a.T) / span : 0.0;
  return ForwardCarry{a.forward + alpha * (b.forward - a.forward),
                      a.q_eff + alpha * (b.q_eff - a.q_eff), input_rate_at(in_, T)};
}

double VolaSession::forward_at(double T) const noexcept {
  if (!(T > 0.0) || ctx_.empty()) {
    return 0.0;
  }
  return interp_forward(T).forward;
}

double VolaSession::q_eff_at(double T) const noexcept {
  if (!(T > 0.0) || ctx_.empty()) {
    return 0.0;
  }
  return interp_forward(T).q_eff;
}

double VolaSession::rate_at(double T) const noexcept {
  if (!(T > 0.0)) {
    return 0.0;
  }
  return input_rate_at(in_, T);
}

Result<PricedSurface> VolaSession::to_priced_surface() const {
  // Resolved cold-repricing scalars. `in_` carries the effective (post-build)
  // pricer method + Andersen-Lake preset; build() always engages al_opts (either
  // caller-pinned or the fast default), so value_or is a belt-and-braces fallback.
  PricingContext pc;
  pc.S = in_.S;
  pc.r = in_.r;
  pc.now_ts_ns = in_.now_ts_ns;
  pc.method = in_.deam.method;
  pc.al_opts = in_.deam.al_opts.value_or(al_fast_opts());
  pc.uid = surface_.uid();

  CurveSurface cs;
  if (curve_override_.has_value()) {
    // ConvexDense / Svi: the fitted curves already live in the override. A deep
    // copy leaves the live session's surface intact for continued serving.
    cs = curve_override_->clone();
  } else {
    // eSSVI default: the fitted slices live in the VolSurface, not a CurveSurface.
    // Rebuild them into a uniform CurveSurface (df_i = exp(-r*T_i), ascending T,
    // parallel to ctx_) so the snapshot serves through the SAME polymorphic path.
    const std::span<const EssviParams> sl = surface_.essvi_slices();
    for (const EssviParams& e : sl) {
      const double df = std::exp(-input_rate_at(in_, e.T) * e.T);
      cs.push(std::make_unique<EssviCurve>(e, df));
    }
  }

  std::vector<SliceContext> ctx_copy(ctx_.begin(), ctx_.end());
  return PricedSurface::create(std::move(cs), std::move(ctx_copy), pc);
}

double VolaSession::shape_blend_total_variance(double k_log, double T) const noexcept {
  // ShapeBlend queries route through the projection-layer inserted-slice path
  // (see InterpMode::ShapeBlend) so both bracketing slices' own shapes are
  // blended, rather than surface_.w()'s linear-in-total-variance-at-fixed-k
  // blend. `curves == nullptr` skips the handle's forward cache (this session
  // sources forward/carry from ctx_ via interp_forward, not a CurveSet).
  // ClampForReporting mirrors interp_forward's own out-of-range policy: a
  // query outside the fitted range serves the nearest endpoint slice rather
  // than being rejected.
  auto handle = surface_insert_vol_slice(surface_, /*curves=*/nullptr, TimeModel{},
                                         T, InterpMode::ShapeBlend,
                                         ProjExtrapPolicy::ClampForReporting);
  if (!handle) {
    return kNaN;
  }
  return w_on_inserted_slice(surface_, *handle, k_log);
}

double VolaSession::event_aware_total_variance(double k_log, double T) const noexcept {
  // Same inserted-slice mechanism as shape_blend_total_variance above
  // (ClampForReporting mirrors interp_forward's own out-of-range policy;
  // curves == nullptr since forward/carry come from ctx_, not a CurveSet),
  // but `in_.interp` (not hardcoded ShapeBlend) selects the underlying
  // blend, and the event-aware overload of w_on_inserted_slice does the
  // censor/interpolate/re-add work.
  auto handle = surface_insert_vol_slice(surface_, /*curves=*/nullptr, TimeModel{},
                                         T, in_.interp,
                                         ProjExtrapPolicy::ClampForReporting);
  if (!handle) {
    return kNaN;
  }
  return w_on_inserted_slice(surface_, *handle, k_log, in_.events.get(),
                             diag_.implied_emove, in_.now_ts_ns);
}

double VolaSession::model_w(double k_log, double T) const noexcept {
  if (curve_override_) {
    return curve_override_->w(k_log, T);
  }
  if (event_aware_active()) {
    return event_aware_total_variance(k_log, T);
  }
  if (in_.interp == InterpMode::ShapeBlend) {
    return shape_blend_total_variance(k_log, T);
  }
  return surface_.w(k_log, T);
}

double VolaSession::model_iv(double k_log, double T) const noexcept {
  if (curve_override_) {
    return curve_override_->iv(k_log, T);
  }
  if (event_aware_active()) {
    const double w = event_aware_total_variance(k_log, T);
    if (!(std::isfinite(w) && w > 0.0) || !(T > 0.0)) {
      return kNaN;
    }
    return std::sqrt(w / T);
  }
  if (in_.interp == InterpMode::ShapeBlend) {
    const double w = shape_blend_total_variance(k_log, T);
    if (!(std::isfinite(w) && w > 0.0) || !(T > 0.0)) {
      return kNaN;
    }
    return std::sqrt(w / T);
  }
  return surface_.iv(k_log, T);
}

double VolaSession::iv(double K, double T) const {
  if (!valid_query(K, T)) {
    return kNaN;
  }
  const ForwardCarry fc = interp_forward(T);
  const double k = std::log(K / fc.forward);
  return model_iv(k, T);
}

double VolaSession::total_variance(double K, double T) const {
  if (!valid_query(K, T)) {
    return kNaN;
  }
  const ForwardCarry fc = interp_forward(T);
  const double k = std::log(K / fc.forward);
  return model_w(k, T);
}

Result<double> VolaSession::fair_value(double K, double T, Side side) const {
  if (!valid_query(K, T)) {
    return Err(ErrorCode::InvalidArgument,
               "VolaSession::fair_value: non-finite or non-positive K/T");
  }
  const ForwardCarry fc = interp_forward(T);
  const double k = std::log(K / fc.forward);
  const double sigma = model_iv(k, T);

  // Cached hot path for the eSSVI default; cold (accurate) Andersen-Lake for the
  // high-accuracy override surface (see served_cache).
  const CorrectionCache* const cc = served_cache(side);
  if (cc != nullptr) {
    const double fv = american_price_cached(in_.S, K, T, sigma, fc.rate, fc.q_eff, side, cc);
    if (!std::isfinite(fv)) {
      return Err(ErrorCode::Internal,
                 "VolaSession::fair_value: cached pricer produced a non-finite price");
    }
    return Ok(fv);
  }
  return american_price(in_.S, K, T, sigma, fc.rate, fc.q_eff, side, in_.deam.method,
                        in_.deam.al_opts);
}

Result<AmericanGreeks> VolaSession::greeks(double K, double T, Side side) const {
  if (!valid_query(K, T)) {
    return Err(ErrorCode::InvalidArgument,
               "VolaSession::greeks: non-finite or non-positive K/T");
  }
  const ForwardCarry fc = interp_forward(T);
  const double k = std::log(K / fc.forward);
  const double sigma = model_iv(k, T);

  // Cached hot path for the eSSVI default: differentiate the cached graph. A null
  // cache (override surface, or a side on the cold path) uses American finite
  // differences on the SAME cold american_price the fair_value branch prices with,
  // so greeks().price == fair_value() bit-identical (American, not Black-76).
  const CorrectionCache* const use = served_cache(side);
  if (use != nullptr) {
    return american_greeks(in_.S, K, T, sigma, fc.rate, fc.q_eff, side, use);
  }
  return american_greeks_fd(in_.S, K, T, sigma, fc.rate, fc.q_eff, side, in_.deam.method,
                            in_.deam.al_opts);
}

Status VolaSession::fair_value_ladder(double T, std::span<const double> strikes,
                                      std::span<const Side> sides,
                                      std::span<double> out) const {
  if (!std::isfinite(T) || !(T > 0.0)) {
    return Err(ErrorCode::InvalidArgument,
               "VolaSession::fair_value_ladder: non-finite or non-positive T");
  }
  if (strikes.size() != sides.size() || strikes.size() != out.size()) {
    return Err(ErrorCode::InvalidArgument,
               "VolaSession::fair_value_ladder: strikes/sides/out length mismatch");
  }
  // Resolve the per-expiry context ONCE and reuse it across the whole ladder:
  // the T-bracket forward/carry interpolation and this session's per-side cache
  // pointers do not vary with strike.
  const ForwardCarry fc = interp_forward(T);
  for (std::size_t i = 0; i < strikes.size(); ++i) {
    const double K = strikes[i];
    if (!std::isfinite(K) || !(K > 0.0)) {
      out[i] = kNaN;  // a bad strike must not sink the rest of the reprice
      continue;
    }
    const Side side = sides[i];
    const double k = std::log(K / fc.forward);
    const double sigma = model_iv(k, T);
    const CorrectionCache* const cc = served_cache(side);
    if (cc != nullptr) {
      out[i] = american_price_cached(in_.S, K, T, sigma, fc.rate, fc.q_eff, side, cc);
    } else {
      const auto p = american_price(in_.S, K, T, sigma, fc.rate, fc.q_eff, side, in_.deam.method,
                                    in_.deam.al_opts);
      out[i] = p.has_value() ? *p : kNaN;
    }
  }
  return Ok();
}

Status VolaSession::greeks_ladder(double T, std::span<const double> strikes,
                                  std::span<const Side> sides,
                                  std::span<AmericanGreeks> out) const {
  if (!std::isfinite(T) || !(T > 0.0)) {
    return Err(ErrorCode::InvalidArgument,
               "VolaSession::greeks_ladder: non-finite or non-positive T");
  }
  if (strikes.size() != sides.size() || strikes.size() != out.size()) {
    return Err(ErrorCode::InvalidArgument,
               "VolaSession::greeks_ladder: strikes/sides/out length mismatch");
  }
  const ForwardCarry fc = interp_forward(T);
  for (std::size_t i = 0; i < strikes.size(); ++i) {
    const double K = strikes[i];
    if (!std::isfinite(K) || !(K > 0.0)) {
      out[i] = AmericanGreeks{};
      out[i].price = kNaN;
      continue;
    }
    const Side side = sides[i];
    const double k = std::log(K / fc.forward);
    const double sigma = model_iv(k, T);
    const CorrectionCache* const use = served_cache(side);
    // Cached hot path differentiates the cached graph; the null-cache cold path
    // finite-differences american_price so greeks.price == the cold fair_value.
    const auto g = (use != nullptr)
                       ? american_greeks(in_.S, K, T, sigma, fc.rate, fc.q_eff, side, use)
                       : american_greeks_fd(in_.S, K, T, sigma, fc.rate, fc.q_eff, side,
                                            in_.deam.method, in_.deam.al_opts);
    if (g.has_value()) {
      out[i] = *g;
    } else {
      out[i] = AmericanGreeks{};
      out[i].price = kNaN;
    }
  }
  return Ok();
}

VolaSession VolaSession::clone_for_refit() const {
  std::optional<CurveSurface> curve_override;
  if (curve_override_.has_value()) {
    curve_override.emplace(curve_override_->clone());
  }
  auto call_cache = corr_call_;
  auto put_cache = corr_put_;
  VolaSession copy{VolSurface{surface_},
                   std::vector<SliceContext>{ctx_},
                   std::vector<ParityReport>{parity_},
                   SessionInputs{in_},
                   diag_,
                   std::vector<SessionSliceDiagnostics>{slice_diag_},
                   std::move(call_cache),
                   std::move(put_cache),
                   std::move(curve_override)};
  copy.incremental_observations_ = incremental_observations_;
  return copy;
}

Result<FitDiag> VolaSession::apply_prepared_essvi_refit(
    std::size_t slice_idx, const CanonicalPreparedExpiry &prepared) {
  if (slice_idx >= ctx_.size() || slice_idx > std::numeric_limits<std::uint16_t>::max()) {
    return Err(ErrorCode::InvalidArgument,
               "VolaSession::apply_prepared_essvi_refit: slice index out of range");
  }
  if (prepared.slice.fit_observations().empty() ||
      prepared.slice.maturity() != ctx_[slice_idx].T) {
    return Err(ErrorCode::InvalidArgument,
               "VolaSession::apply_prepared_essvi_refit: incompatible prepared expiry");
  }
  const std::span<const EssviParams> slices = surface_.essvi_slices();
  if (curve_override_.has_value() || slice_idx >= slices.size()) {
    return Err(ErrorCode::InvalidArgument,
               "VolaSession::apply_prepared_essvi_refit: target is not an eSSVI slice");
  }

  const EssviParams warm = slices[slice_idx];
  // Facade refit currently admits CalendarRepair::None only. A previous-theta
  // floor would silently change that configured fitting policy into a partial
  // MonotoneFit, so the exact None path has no optimizer floor. The independent
  // publication oracle validates both neighbours after fitting.
  constexpr double theta_floor = 0.0;
  FitDiag fit_diag{};
  ATX_TRY(EssviParams refitted,
          essvi_fit_slice(prepared.slice.fit_observations(), ctx_[slice_idx].T,
                          prepared.slice.forward(), in_.calib, &fit_diag, theta_floor, &warm));
  refitted.expiry_id = warm.expiry_id;
  refitted.expiry_ns = warm.expiry_ns;
  ATX_TRY_VOID(surface_.set_slice_essvi(slice_idx, refitted));

  SliceContext &context = ctx_[slice_idx];
  context.forward = prepared.slice.forward();
  context.borrow = prepared.borrow;
  context.q_eff = prepared.q_eff;
  context.n_used = prepared.slice.fit_observations().size();
  context.n_dropped = prepared.slice.n_dropped();

  const PreparedScoreColumns &score = prepared.slice.score_columns();
  std::vector<double> model_iv;
  model_iv.reserve(score.k_log.size());
  for (const double k_log : score.k_log) {
    model_iv.push_back(
        surface_.iv_on_slice(static_cast<std::uint16_t>(slice_idx), k_log));
  }
  ParityInputs parity_inputs{};
  parity_inputs.S = in_.S;
  parity_inputs.r = prepared.rate;
  parity_inputs.q_eff = prepared.q_eff;
  parity_inputs.T = context.T;
  parity_inputs.method = in_.deam.method;
  parity_inputs.al_opts = in_.deam.al_opts;
  parity_inputs.band_k = in_.band_k;
  parity_inputs.n_curve_params = 3u;
  parity_inputs.caches = query_caches();
  ATX_TRY(const ParityReport refreshed,
          chain_parity(score.strike, score.bid, score.ask, score.mid, score.side, model_iv,
                       score.market_iv, parity_inputs));
  parity_[slice_idx] = refreshed;
  ATX_TRY_VOID(refresh_refit_diagnostics());
  return Ok(fit_diag);
}

Status VolaSession::refresh_refit_diagnostics() {
  constexpr double kArbKMin = -3.0;
  constexpr double kArbKMax = 3.0;
  constexpr std::uint32_t kArbNGrid = 25u;
  ATX_TRY(const std::vector<ArbViolation> violations,
          arb_check_calendar(surface_, kArbKMin, kArbKMax, kArbNGrid));
  diag_.calendar_arb_free = violations.empty();
  // Only CalendarRepair::None reaches facade refit. With no repair phase, the
  // candidate's current violation count is also its semantically pre-repair
  // count; do not reuse this assignment for Project/MonotoneFit.
  diag_.n_calendar_viol_pre = violations.size();
  diag_.n_slices = ctx_.size();
  diag_.n_quotes = 0u;
  for (const SliceContext &context : ctx_) {
    diag_.n_quotes += context.n_used;
  }

  double worst = std::numeric_limits<double>::infinity();
  double sum_frac = 0.0;
  double sum_chi2 = 0.0;
  double sum_rmse = 0.0;
  std::size_t scored = 0u;
  for (const ParityReport &report : parity_) {
    if (report.n == 0u) {
      continue;
    }
    worst = std::min(worst, report.frac_fv_within_bidask);
    sum_frac += report.frac_fv_within_bidask;
    sum_chi2 += report.chi2_reduced;
    sum_rmse += report.rmse_mid_vol;
    ++scored;
  }
  // Recompute parity_state from THIS refit's actual scoring; never inherit the
  // cold build's value (B-I1). The legacy eSSVI refit always re-scores its
  // target slice, so a healthy refit resolves Valid; a partial score resolves
  // Failed, and a fully-unscored refit resolves Failed too (B-M1) so it cannot
  // admit via "0 looks fine". Disabled is honored only for a session that opted
  // out of scoring AND produced no scored slice — matching the cold eSSVI path,
  // which reports Valid/Failed (never Disabled) whenever any slice scored.
  if (scored == diag_.n_slices && scored > 0u) {
    diag_.parity_state = ParityDiagnosticState::Valid;
  } else if (scored == 0u && !in_.score_parity) {
    diag_.parity_state = ParityDiagnosticState::Disabled;
  } else {
    diag_.parity_state = ParityDiagnosticState::Failed;
  }
  if (scored == 0u) {
    diag_.worst_frac_within_bidask = 0.0;
    diag_.mean_frac_within_bidask = 0.0;
    diag_.mean_chi2_reduced = 0.0;
    diag_.mean_rmse_vol = 0.0;
    return Ok();
  }
  const double denominator = static_cast<double>(scored);
  diag_.worst_frac_within_bidask = worst;
  diag_.mean_frac_within_bidask = sum_frac / denominator;
  diag_.mean_chi2_reduced = sum_chi2 / denominator;
  diag_.mean_rmse_vol = sum_rmse / denominator;
  return Ok();
}

Result<std::vector<FitObs>>
VolaSession::cached_refit_observations(const Chain &chain,
                                       std::size_t slice_idx) const {
  if (incremental_observations_ == nullptr ||
      slice_idx >= incremental_observations_->observations.size() ||
      slice_idx >= incremental_observations_->source_mids.size() ||
      slice_idx >= incremental_observations_->source_flags.size() ||
      slice_idx >= incremental_observations_->chain_mids.size() ||
      slice_idx >= incremental_observations_->chain_flags.size() ||
      slice_idx >= incremental_observations_->chain_bids.size() ||
      slice_idx >= incremental_observations_->chain_asks.size() ||
      slice_idx >= incremental_observations_->chain_ts.size()) {
    return Err(ErrorCode::NotFound,
               "VolaSession::cached_refit_observations: no certified cache");
  }
  const std::vector<FitObs> &cached =
      incremental_observations_->observations[slice_idx];
  const std::vector<double> &source_mids =
      incremental_observations_->source_mids[slice_idx];
  const std::vector<std::uint8_t> &source_flags =
      incremental_observations_->source_flags[slice_idx];
  const std::vector<double> &chain_mids =
      incremental_observations_->chain_mids[slice_idx];
  const std::vector<std::uint8_t> &chain_flags =
      incremental_observations_->chain_flags[slice_idx];
  const std::vector<double> &chain_bids =
      incremental_observations_->chain_bids[slice_idx];
  const std::vector<double> &chain_asks =
      incremental_observations_->chain_asks[slice_idx];
  const std::vector<std::int64_t> &chain_ts =
      incremental_observations_->chain_ts[slice_idx];
  if (cached.empty() || cached.size() != source_mids.size() ||
      cached.size() != source_flags.size() || chain.mids.size() != chain_mids.size() ||
      chain.flags.size() != chain_flags.size() ||
      chain.bids.size() != chain_bids.size() ||
      chain.asks.size() != chain_asks.size() ||
      chain.ts_ns.size() != chain_ts.size()) {
    return Err(ErrorCode::NotFound,
               "VolaSession::cached_refit_observations: incomplete certified cache");
  }
  for (std::size_t i = 0; i < chain_mids.size(); ++i) {
    const std::size_t strike_idx = i / 2u;
    if (strike_idx >= chain.strikes.size() ||
        std::fabs(chain.strikes[strike_idx] / in_.S - 1.0) > 0.25) {
      continue;
    }
    const double tolerance = 1.0e-12 * std::max(1.0, std::fabs(chain_mids[i]));
    if (std::fabs(chain.mids[i] - chain_mids[i]) > tolerance) {
      return Err(ErrorCode::Unavailable,
                 "VolaSession::cached_refit_observations: carry prices changed");
    }
    if (chain.flags[i] != chain_flags[i]) {
      return Err(ErrorCode::Unavailable,
                 "VolaSession::cached_refit_observations: carry flags changed");
    }
  }

  // Carry-coordinate invalidation (§14: "any price, eligibility, or
  // carry-coordinate change falls back to the full certified path"). The
  // robust carry consumes, for its SELECTED pairs, the mids (borrow solve),
  // bid/ask spreads (quality weight), and quote timestamps (freshness weight)
  // — and the selection itself is a function of every quote's eligibility. So
  // certified reuse must prove (a) the pair selection is unchanged (replayed
  // on the snapshot vs the live chain through the same carry_pair_strikes the
  // solve uses) and (b) every field of every selected leg is unchanged. This
  // covers pairs the nearest-pair fallback picks OUTSIDE the ±25% band above;
  // the band check stays as the (strictly weaker) legacy fit-quote guard.
  if (in_.deam.imply_borrow) {
    Chain snapshot = chain;
    snapshot.bids = chain_bids;
    snapshot.asks = chain_asks;
    snapshot.mids = chain_mids;
    snapshot.flags = chain_flags;
    snapshot.ts_ns = chain_ts;
    const std::vector<std::uint16_t> prior_pairs =
        carry_pair_strikes(snapshot, in_.S, in_.deam);
    const std::vector<std::uint16_t> current_pairs =
        carry_pair_strikes(chain, in_.S, in_.deam);
    if (prior_pairs != current_pairs) {
      return Err(ErrorCode::Unavailable,
                 "VolaSession::cached_refit_observations: carry pair eligibility changed");
    }
    for (const std::uint16_t pair_strike : current_pairs) {
      for (const Side side : {Side::Call, Side::Put}) {
        const std::size_t quote_idx = chain_index(pair_strike, side);
        if (quote_idx >= chain.mids.size() || quote_idx >= chain.bids.size() ||
            quote_idx >= chain.asks.size() || quote_idx >= chain.flags.size()) {
          return Err(ErrorCode::InvalidArgument,
                     "VolaSession::cached_refit_observations: malformed carry pair index");
        }
        const bool ts_known = quote_idx < chain.ts_ns.size();
        if (chain.bids[quote_idx] != chain_bids[quote_idx] ||
            chain.asks[quote_idx] != chain_asks[quote_idx] ||
            chain.mids[quote_idx] != chain_mids[quote_idx] ||
            chain.flags[quote_idx] != chain_flags[quote_idx] ||
            (ts_known && chain.ts_ns[quote_idx] != chain_ts[quote_idx])) {
          return Err(ErrorCode::Unavailable,
                     "VolaSession::cached_refit_observations: carry inputs changed");
        }
      }
    }
  }

  std::vector<FitObs> refreshed = cached;
  for (std::size_t i = 0; i < refreshed.size(); ++i) {
    FitObs &obs = refreshed[i];
    const auto strike_it =
        std::lower_bound(chain.strikes.begin(), chain.strikes.end(), obs.K);
    if (strike_it == chain.strikes.end() || *strike_it != obs.K) {
      return Err(ErrorCode::Unavailable,
                 "VolaSession::cached_refit_observations: strike set changed");
    }
    const auto strike_idx = static_cast<std::uint16_t>(
        std::distance(chain.strikes.begin(), strike_it));
    const std::size_t quote_idx = chain_index(strike_idx, obs.side);
    if (quote_idx >= chain.bids.size() || quote_idx >= chain.asks.size() ||
        quote_idx >= chain.mids.size() || quote_idx >= chain.flags.size()) {
      return Err(ErrorCode::InvalidArgument,
                 "VolaSession::cached_refit_observations: malformed quote arrays");
    }
    const double bid = chain.bids[quote_idx];
    const double ask = chain.asks[quote_idx];
    const double mid = chain.mids[quote_idx];
    const double mid_tolerance = 1.0e-12 * std::max(1.0, std::fabs(source_mids[i]));
    if (!std::isfinite(bid) || !std::isfinite(ask) || !(bid > 0.0) ||
        !(ask > bid) || std::fabs(mid - source_mids[i]) > mid_tolerance ||
        chain.flags[quote_idx] != source_flags[i]) {
      return Err(ErrorCode::Unavailable,
                 "VolaSession::cached_refit_observations: price or flags changed");
    }
    const double spread = ask - bid;
    if ((in_.calib.max_spread_to_mid_pct > 0.0 &&
         spread / mid > in_.calib.max_spread_to_mid_pct) ||
        !(obs.vega > 1.0e-12) ||
        (in_.calib.max_spread_vol > 0.0 &&
         spread / obs.vega > in_.calib.max_spread_vol)) {
      return Err(ErrorCode::Unavailable,
                 "VolaSession::cached_refit_observations: quote left fit filter");
    }
    constexpr double weight_epsilon = 1.0e-18;
    const double weight_sigma =
        (obs.vega * obs.vega) / (spread * spread + weight_epsilon);
    if (weight_sigma < in_.calib.min_vega_weight) {
      return Err(ErrorCode::Unavailable,
                 "VolaSession::cached_refit_observations: quote left weight filter");
    }
    const double jacobian = 2.0 * obs.sigma_mkt * ctx_[slice_idx].T;
    if (!(jacobian > 0.0)) {
      return Err(ErrorCode::Unavailable,
                 "VolaSession::cached_refit_observations: invalid variance Jacobian");
    }
    obs.spread = spread;
    obs.weight_w = (obs.vega * obs.vega) /
                   (spread * spread + weight_epsilon) /
                   (jacobian * jacobian + weight_epsilon);
    obs.active_weight_w = obs.weight_w;
    obs.noise_sigma = spread / obs.vega;
  }
  return Ok(std::move(refreshed));
}

Result<FitDiag> VolaSession::refit_slice(std::size_t slice_idx,
                                         std::span<const FitObs> new_obs) {
  if (slice_idx >= ctx_.size()) {
    return Err(ErrorCode::InvalidArgument,
               "VolaSession::refit_slice: slice_idx out of range");
  }
  if (new_obs.empty()) {
    return Err(ErrorCode::InvalidArgument,
               "VolaSession::refit_slice: empty observation set");
  }

  // Multiplying every observation spread by one common positive scalar leaves
  // relative calibration weights unchanged (apart from the 1e-18 divide guard,
  // far below valid quote precision), including the total-weight-scaled warm
  // prior. Detect that invariant venue update and retain the optimal curve.
  if ((diag_.incremental.attempts == 0u ||
       (diag_.incremental.last_committed &&
        diag_.incremental.last_fit_ms == 0.0)) &&
      incremental_observations_ != nullptr &&
      slice_idx < incremental_observations_->observations.size()) {
    const std::vector<FitObs> &prior_obs =
        incremental_observations_->observations[slice_idx];
    if (prior_obs.size() == new_obs.size() && !prior_obs.empty() &&
        prior_obs.front().spread > 0.0 && new_obs.front().spread > 0.0) {
      const double spread_ratio =
          new_obs.front().spread / prior_obs.front().spread;
      bool invariant = std::isfinite(spread_ratio) && spread_ratio > 0.0;
      for (std::size_t i = 0; invariant && i < prior_obs.size(); ++i) {
        const FitObs &old_row = prior_obs[i];
        const FitObs &new_row = new_obs[i];
        const double row_ratio = new_row.spread / old_row.spread;
        invariant = old_row.k == new_row.k &&
                    old_row.sigma_mkt == new_row.sigma_mkt &&
                    old_row.w_mkt == new_row.w_mkt &&
                    old_row.mid == new_row.mid && old_row.side == new_row.side &&
                    std::fabs(row_ratio - spread_ratio) <=
                        1.0e-9 * std::max(1.0, std::fabs(spread_ratio));
      }
      if (invariant) {
        IncrementalRefitDiagnostics &incremental = diag_.incremental;
        ++incremental.attempts;
        ++incremental.committed;
        incremental.last_slice_index = slice_idx;
        incremental.last_kind = curve_override_.has_value()
                                    ? curve_override_->slices()[slice_idx]->kind()
                                    : VolCurveKind::Essvi;
        incremental.last_adjacent_pairs_checked = 0;
        incremental.last_fit_ms = 0.0;
        incremental.last_calendar_ms = 0.0;
        incremental.last_validation_ms = 0.0;
        incremental.last_total_ms = 0.0;
        incremental.last_committed = true;
        FitDiag unchanged;
        unchanged.n_quotes_used = static_cast<std::uint32_t>(new_obs.size());
        return Ok(unchanged);
      }
    }
  }

  // Polymorphic risk surfaces stage the entire publication object but refit and
  // revalidate only the touched pillar and its two adjacent calendar pairs.
  // The live optional is not moved or mutated until every check succeeds.
  if (curve_override_.has_value()) {
    const auto live_slices = curve_override_->slices();
    if (slice_idx >= live_slices.size()) {
      return Err(ErrorCode::InvalidArgument,
                 "VolaSession::refit_slice: no override slice at that index");
    }
    const IVolCurve &current = *live_slices[slice_idx];
    if (current.kind() != VolCurveKind::ConvexDense &&
        current.kind() != VolCurveKind::Svi &&
        current.kind() != VolCurveKind::C8) {
      return Err(ErrorCode::InvalidArgument,
                 "VolaSession::refit_slice: override kind is not locally admitted");
    }

    using RefitClock = std::chrono::steady_clock;
    const auto elapsed_ms = [](RefitClock::time_point begin,
                               RefitClock::time_point end) noexcept {
      return std::chrono::duration<double, std::milli>(end - begin).count();
    };
    IncrementalRefitDiagnostics &incremental = diag_.incremental;
    ++incremental.attempts;
    incremental.last_slice_index = slice_idx;
    incremental.last_kind = current.kind();
    incremental.last_adjacent_pairs_checked = 0;
    incremental.last_fit_ms = 0.0;
    incremental.last_calendar_ms = 0.0;
    incremental.last_validation_ms = 0.0;
    incremental.last_total_ms = 0.0;
    incremental.last_committed = false;
    const auto total_begin = RefitClock::now();
    const auto reject = [&]() noexcept {
      ++incremental.rolled_back;
      incremental.last_total_ms =
          elapsed_ms(total_begin, RefitClock::now());
    };

    std::function<double(double)> w_prev;
    if (slice_idx > 0) {
      const IVolCurve *previous = live_slices[slice_idx - 1].get();
      w_prev = [previous](double k) { return previous->w(k); };
    }
    const SliceContext &sc = ctx_[slice_idx];
    const auto fit_begin = RefitClock::now();
    FitDiag fit_diag{};
    auto fitted = refit_slice_curve(in_.curve, current, new_obs, sc.forward,
                                    sc.T, current.df(), w_prev, &fit_diag);
    incremental.last_fit_ms =
        elapsed_ms(fit_begin, RefitClock::now());
    if (!fitted.has_value()) {
      reject();
      return Err(std::move(fitted).error());
    }

    // Re-run served-value shape validation even though the family fitter already
    // admitted its result. ConvexDense is certified directly in call-price space;
    // the FD Roper check is inappropriate at its deliberate piecewise-linear
    // knots, so only smooth parametric families use it here.
    const auto validation_begin = RefitClock::now();
    if ((*fitted)->kind() != VolCurveKind::ConvexDense) {
      auto shape = arb_check_butterfly(**fitted, -0.60, 0.60, 256);
      if (!shape.has_value()) {
        incremental.last_validation_ms =
            elapsed_ms(validation_begin, RefitClock::now());
        reject();
        return Err(std::move(shape).error());
      }
      if (!shape->empty()) {
        incremental.last_validation_ms =
            elapsed_ms(validation_begin, RefitClock::now());
        reject();
        return Err(ErrorCode::Unavailable,
                   "VolaSession::refit_slice: strike-shape admission failed");
      }
    }
    incremental.last_validation_ms =
        elapsed_ms(validation_begin, RefitClock::now());

    // Build an adjacent-only surface [previous?, candidate, next?]. The fitter
    // already projected candidate above previous; this independent check also
    // enforces the upper relation to next. A violation rolls back instead of
    // cascading a local tick through untouched maturities.
    const auto calendar_begin = RefitClock::now();
    CurveSurface adjacent;
    if (slice_idx > 0) {
      adjacent.push(live_slices[slice_idx - 1]->clone());
      ++incremental.last_adjacent_pairs_checked;
    }
    adjacent.push((*fitted)->clone());
    if (slice_idx + 1 < live_slices.size()) {
      adjacent.push(live_slices[slice_idx + 1]->clone());
      ++incremental.last_adjacent_pairs_checked;
    }
    auto calendar = arb_check_calendar(adjacent, -0.60, 0.60, 64);
    incremental.last_calendar_ms =
        elapsed_ms(calendar_begin, RefitClock::now());
    if (!calendar.has_value()) {
      reject();
      return Err(std::move(calendar).error());
    }
    if (!calendar->empty()) {
      reject();
      return Err(ErrorCode::Unavailable,
                 "VolaSession::refit_slice: adjacent calendar admission failed");
    }

    CurveSurface staged = curve_override_->clone();
    if (Status replace = staged.replace(slice_idx, std::move(*fitted));
        !replace.has_value()) {
      reject();
      return Err(std::move(replace).error());
    }
    curve_override_ = std::move(staged);
    const std::size_t old_n_used = ctx_[slice_idx].n_used;
    ctx_[slice_idx].n_used = new_obs.size();
    diag_.n_quotes = diag_.n_quotes >= old_n_used
                         ? diag_.n_quotes - old_n_used + new_obs.size()
                         : new_obs.size();
    diag_.calendar_arb_free = true;
    // I-2: refresh the full-surface price-bound self-check (not just the
    // touched pillar) so a still-violating slice elsewhere in the surface is
    // never silently dropped from the diagnostic by a narrow, successful
    // refit — mirrors the OR-only, never-clear discipline of the
    // ValidationFailure merge itself.
    {
      const auto price_bounds =
          arb_check_price_bounds(*curve_override_, -0.60, 0.60, 64);
      diag_.n_price_bound_violations = price_bounds ? price_bounds->size() : 0;
    }
    ++incremental.committed;
    incremental.last_committed = true;
    incremental.last_total_ms =
        elapsed_ms(total_begin, RefitClock::now());
    return Ok(fit_diag);
  }

  const std::span<const EssviParams> slices = surface_.essvi_slices();
  if (slice_idx >= slices.size()) {
    return Err(ErrorCode::InvalidArgument,
               "VolaSession::refit_slice: no eSSVI slice at that index");
  }
  // Copy the current slice: it is BOTH the warm-start seed and the source of the
  // expiry identity we must preserve across the swap.
  const EssviParams warm = slices[slice_idx];
  const SliceContext& sc = ctx_[slice_idx];

  // Keep the term structure calendar-monotone through the update by flooring the
  // ATM level at the previous slice's theta (a no-op for the first slice, and
  // only binds where the refit would otherwise invert against its neighbour).
  const double theta_floor =
      (slice_idx > 0) ? slices[slice_idx - 1].theta : 0.0;

  using RefitClock = std::chrono::steady_clock;
  const auto total_begin = RefitClock::now();
  IncrementalRefitDiagnostics &incremental = diag_.incremental;
  ++incremental.attempts;
  incremental.last_slice_index = slice_idx;
  incremental.last_kind = VolCurveKind::Essvi;
  incremental.last_adjacent_pairs_checked = 0;
  incremental.last_calendar_ms = 0.0;
  incremental.last_validation_ms = 0.0;
  incremental.last_committed = false;
  FitDiag diag{};
  const auto fit_begin = RefitClock::now();
  Result<EssviParams> refit = essvi_fit_slice(new_obs, sc.T, sc.forward,
                                              in_.calib, &diag, theta_floor,
                                              &warm);
  incremental.last_fit_ms =
      std::chrono::duration<double, std::milli>(RefitClock::now() - fit_begin).count();
  if (!refit.has_value()) {
    ++incremental.rolled_back;
    incremental.last_total_ms =
        std::chrono::duration<double, std::milli>(RefitClock::now() - total_begin).count();
    return Err(std::move(refit).error());  // surface untouched on failure
  }
  refit->expiry_id = warm.expiry_id;   // preserve identity across the swap
  refit->expiry_ns = warm.expiry_ns;

  if (Status st = surface_.set_slice_essvi(slice_idx, *refit); !st.has_value()) {
    return Err(std::move(st).error());
  }
  ctx_[slice_idx].n_used = new_obs.size();

  // Re-evaluate the surface-level calendar no-arb flag over the standard window
  // so diagnostics() stays truthful about the mutated surface (the same window
  // run_surface_parity checks). A check failure leaves the flag conservatively
  // false rather than asserting no-arb it could not verify.
  constexpr double kArbKMin = -3.0;
  constexpr double kArbKMax = 3.0;
  constexpr std::uint32_t kArbNGrid = 25;
  auto cal = arb_check_calendar(surface_, kArbKMin, kArbKMax, kArbNGrid);
  diag_.calendar_arb_free = cal.has_value() && cal->empty();
  ++incremental.committed;
  incremental.last_committed = true;
  incremental.last_total_ms =
      std::chrono::duration<double, std::milli>(RefitClock::now() - total_begin).count();

  return Ok(diag);
}

}  // namespace atx::vol
