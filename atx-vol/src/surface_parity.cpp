#include "atx/vol/surface_parity.hpp"

#include <algorithm>
#include <chrono> // ATX_VOL_PROFILE phase timing (temporary)
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>  // ATX_VOL_PROFILE stderr report (temporary)
#include <cstdlib> // getenv (ATX_VOL_PROFILE)
#include <limits>
#include <optional> // std::nullopt (cold accurate re-inversion on audit failure)
#include <span>     // prepared.fit_observations() view (C1 input certification)
#include <utility>
#include <vector>

#include "atx/core/error.hpp"
#include "atx/vol/arb.hpp"              // arb_check_calendar, ArbViolation
#include "atx/vol/calib.hpp"            // FitObs, FitDiag, CalibOpts
#include "atx/vol/deamer.hpp"           // de_americanize_chain, european_equiv_iv, otm_side
#include "atx/vol/essvi_calib.hpp"      // essvi_fit_slice
#include "atx/vol/parity.hpp"           // chain_parity, ParityInputs, ParityReport
#include "atx/vol/prepared_fitting.hpp" // PreparedSlice legacy compatibility seam
#include "atx/vol/types.hpp"
#include "atx/vol/universe.hpp"    // Underlying, Chain, chain_index
#include "atx/vol/vol_surface.hpp" // VolSurface, EssviParams, Parametrization

// PORT / PARITY NOTES
// -------------------
// * Per-expiry pattern reuse. The de-Americanize -> prepared-slice -> eSSVI-fit
//   path uses PreparedObservationPolicy::LegacyEssviCompatibility to preserve
//   the historical `vola_parity.cpp` row population and arithmetic. The policy
//   is explicit and isolated; new family-neutral consumers use Configured.
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

// Calendar no-arb sampling grid (spec: +/-3 over ~25 steps).
constexpr double kArbKMin = -3.0;
constexpr double kArbKMax = 3.0;
constexpr std::uint32_t kArbNGrid = 25;
constexpr double kMonotoneKMin = -0.7;
constexpr double kMonotoneKMax = 0.7;

// W3.4 (F4): an EXPECTED slice failure is a genuinely thin / data-shaped outcome
// (`NotFound` = fewer than the usable-row floor; `Unavailable` = non-positive
// forward, audit-rejected rows). Every other code (`InvalidArgument`, `Internal`,
// `OutOfRange`, …) is a real defect that the completeness contract propagates.
[[nodiscard]] bool slice_error_is_expected(ErrorCode code) noexcept {
  return code == ErrorCode::NotFound || code == ErrorCode::Unavailable;
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
[[nodiscard]] Result<EssviParams> fit_slice_calendar_floored(const PreparedSlice &prepared,
                                                             double T, double F,
                                                             const CalibOpts &opts, FitDiag *diag,
                                                             const EssviParams *prev, double df) {
  const std::span<const FitObs> observations = prepared.fit_observations();
  const std::vector<double> &score_k = prepared.score_columns().k_log;
  const double theta_floor = (prev != nullptr) ? prev->theta : 0.0;
  Result<EssviParams> res = essvi_fit_slice(observations, T, F, opts, diag, theta_floor);
  if (!res || prev == nullptr || score_k.empty()) {
    return res;
  }

  // Enforce the floor only over this slice's own quoted k-range (+ a small
  // margin) — the region where the calendar cross is economically real. Outside
  // it, the wing extrapolation is left free.
  double k_lo = score_k.front();
  double k_hi = score_k.front();
  for (const double k_log : score_k) {
    k_lo = std::min(k_lo, k_log);
    k_hi = std::max(k_hi, k_log);
  }
  constexpr double kMargin = 0.10;
  // Enforce over the slice's own quoted range PLUS a near-money band, so a
  // crossing that sits just outside a narrow (short-expiry) slice's strikes is
  // still caught. The near-money band is where calendar crossings are
  // economically real; deep wings are left free (documented extrapolation).
  constexpr double kNearMoneyK = 0.7;
  k_lo = std::min(k_lo - kMargin, -kNearMoneyK);
  k_hi = std::max(k_hi + kMargin, kNearMoneyK);

  double w_base = 0.0; // heaviest base weight → penalty scale
  for (const FitObs &o : observations) {
    w_base = std::max(w_base, o.weight_w);
  }
  const double penalty = (w_base > 0.0 ? w_base : 1.0) * 300.0;

  constexpr int kNGrid = 80;
  constexpr int kMaxPass = 8;
  const double dk = (k_hi - k_lo) / static_cast<double>(kNGrid);

  CalibOpts floored_opts = opts;
  floored_opts.residual_disable = true; // keep pseudo-obs out of the residual

  std::vector<FitObs> aug;
  aug.reserve(observations.size() + static_cast<std::size_t>(kNGrid) + 1);
  for (int pass = 0; pass < kMaxPass; ++pass) {
    aug.assign(observations.begin(), observations.end());
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
      break; // floor satisfied over the whole grid
    }
    Result<EssviParams> r2 = essvi_fit_slice(aug, T, F, floored_opts, diag, theta_floor);
    if (!r2) {
      break; // keep the last good fit rather than fail the whole surface
    }
    res = std::move(r2);
  }
  return res;
}

// Perf C1: capture the de-Am input certification for one committed slice from the
// fit's own `PreparedSlice`, so `VolaSession::build` reuses it instead of running
// a SECOND, independent de-Am pass (finding 10). The obs are the exact rows the
// fit de-Americanized; source_mids/flags are the raw chain quotes at each row's
// (strike, side); the chain snapshot lets the incremental refit cache detect a
// carry-coordinate change. A malformed row index (should not happen for a fitted
// slice) leaves the obs cache EMPTY, so the consumer's size checks fall back to
// the full certified path rather than reading out of range.
[[nodiscard]] EssviInputCertification make_input_certification(const PreparedSlice &prepared,
                                                              const Chain &chain) {
  EssviInputCertification cert;
  cert.inversion = prepared.deam_audit();
  const std::span<const FitObs> rows = prepared.fit_observations();
  cert.obs.assign(rows.begin(), rows.end());
  cert.source_mids.reserve(rows.size());
  cert.source_flags.reserve(rows.size());
  for (const FitObs &row : rows) {
    const std::size_t quote_index =
        chain_index(static_cast<std::uint16_t>(row.source_strike_index), row.side);
    if (quote_index >= chain.mids.size() || quote_index >= chain.flags.size()) {
      cert.obs.clear();
      cert.source_mids.clear();
      cert.source_flags.clear();
      break;
    }
    cert.source_mids.push_back(chain.mids[quote_index]);
    cert.source_flags.push_back(chain.flags[quote_index]);
  }
  cert.chain_mids = chain.mids;
  cert.chain_flags = chain.flags;
  cert.chain_bids = chain.bids;
  cert.chain_asks = chain.asks;
  cert.chain_ts = chain.ts_ns;
  return cert;
}

} // namespace

Result<SurfaceParityReport> run_surface_parity(const Underlying &under,
                                               const SurfaceParityInputs &in) {
  if (!(in.S > 0.0) || !std::isfinite(in.r)) {
    return Err(ErrorCode::InvalidArgument, "run_surface_parity: non-positive S or non-finite r");
  }
  const std::size_t n_chains = under.chains.size();
  if (n_chains == 0) {
    return Err(ErrorCode::NotFound, "run_surface_parity: underlying carries no chains");
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

  ATX_TRY(VolSurface surface, VolSurface::create(under.uid, Parametrization::Essvi, n_chains));

  std::vector<double> expiry_T;
  std::vector<ParityReport> per_expiry;
  std::vector<SliceContext> context;
  std::vector<CarryDiagnostics> carry_diag;
  std::vector<EssviInputCertification> input_certs; // C1: ‖ context/carry_diag
  expiry_T.reserve(n_chains);
  per_expiry.reserve(n_chains);
  context.reserve(n_chains);
  carry_diag.reserve(n_chains);
  input_certs.reserve(n_chains);

  // Everything a slice needs to be SCORED after the surface is fully assembled
  // (and possibly calendar-repaired). We defer scoring out of the fit loop so
  // the model IV read back is the one the FINAL surface serves — if a repair
  // pass moves a slice, the parity number reflects the moved slice, not a stale
  // pre-repair read.
  struct PendingSlice {
    PreparedSlice prepared;     // keyed fit rows + raw scoring population
    double T{0.0};              // slice maturity
    double rate{0.0};           // expiry-specific continuously-compounded rate
    double q_eff{0.0};          // effective carry for the re-Am scoring
    std::uint16_t slice_idx{0}; // surface write index for iv_on_slice read-back
  };
  std::vector<PendingSlice> pending;
  pending.reserve(n_chains);

  // W3.4 (F4): per-expiry build outcome for EVERY chain walked (‖ under.chains,
  // in chain order). Lets admission distinguish a thin/absent expiry from a real
  // defect and stop treating a partial fit as a clean success.
  std::vector<ExpiryFitReport> expiry_reports;
  expiry_reports.reserve(n_chains);

  double worst = std::numeric_limits<double>::infinity();
  std::size_t idx = 0; // ascending write index / fitted-slice count
  // Expiries dropped because carry could not be resolved (confidence gate /
  // no quotable pair / degenerate forward). Surfaced through the report so a
  // risk surface missing an expiry never reports clean health with no trace
  // (§5.2: "uncertain carry is surfaced, not hidden").
  std::size_t n_carry_skipped = 0;
  // Expiries dropped because the fit-inversion AUDIT starved the slice below
  // the usable-observation floor: it would have fit but for audit drops. The
  // same silent-gap pattern as a carry skip, reached through §8.1's audit —
  // counted and surfaced identically. Genuinely sparse slices (too few valid
  // quotes regardless of the audit) keep the historical silent-skip shape.
  std::size_t n_audit_starved = 0;

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
      getenv_s(&env_sz, env_buf, sizeof(env_buf), "ATX_VOL_PROFILE") == 0 && env_sz > 0;
  const bool time_stages = profile || in.collect_stage_timings;
  const auto now_ns = []() noexcept {
    return std::chrono::duration<double, std::milli>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
  };
  const double fit_start = time_stages ? now_ns() : 0.0;
  double ms_carry = 0.0;
  double ms_deam = 0.0;
  double ms_fit = 0.0;

  // Chains are stored ascending in T; walk them in that order so slices land
  // in the surface ascending as set_slice_essvi requires.
  for (std::size_t chain_index = 0u; chain_index < under.chains.size(); ++chain_index) {
    const Chain &chain = under.chains[chain_index];
    const double T = chain.T;
    if (!(T > 0.0)) {
      expiry_reports.push_back(
          ExpiryFitReport{chain_index, T, ExpiryFitOutcome::Skipped, 0, ErrorCode::Unknown});
      continue; // degenerate maturity: skip (not fatal)
    }

    // 1-2. One shared carry + observation seam. Compatibility preparation
    // consumes in.deam.caches unconditionally, preserving this cold eSSVI
    // driver's historical cache behavior.
    // FT-C8: honor the configured `fit_prep_policy` (the full CalibOpts filter
    // cascade) when the flag-guarded rollout is enabled; default stays the
    // permissive LegacyEssviCompatibility predicate (byte-identical).
    const PreparedObservationPolicy prep_policy =
        in.essvi_serve_configured_prep ? in.fit_prep_policy
                                       : PreparedObservationPolicy::LegacyEssviCompatibility;
    PrepareExpiryDiagnostics prep_diag{};
    Result<CanonicalPreparedExpiry> prepared_result =
        prepare_expiry(chain, static_cast<std::uint32_t>(chain_index), in,
                       prep_policy, &prep_diag);
    if (time_stages) {
      ms_carry += prep_diag.carry_solve_ms;
      ms_deam += prep_diag.observation_deam_ms;
    }
    if (!prepared_result.has_value()) {
      const ErrorCode prep_code = prepared_result.error().code();
      if (prep_diag.carry_failed) {
        // No slice, but the skip is counted, not hidden (§5.2).
        ++n_carry_skipped;
        expiry_reports.push_back(ExpiryFitReport{chain_index, T, ExpiryFitOutcome::CarryFailed,
                                                   0, prep_code});
      } else if (!slice_error_is_expected(prep_code)) {
        // W3.4 (F4): a HARD preparation defect (not carry, not thin) — surface it
        // and, under the completeness contract, fail the board instead of
        // silently publishing the rest as a clean partial fit.
        expiry_reports.push_back(
            ExpiryFitReport{chain_index, T, ExpiryFitOutcome::PrepFailed, 0, prep_code});
        if (in.fail_board_on_hard_slice_error) {
          return Err(prep_code, "run_surface_parity: expiry preparation failed (hard): " +
                                    prepared_result.error().to_string());
        }
      } else {
        if (prep_diag.n_fit_rows + prep_diag.n_audit_dropped >= kMinPreparedFitRows) {
          // Fewer than the minimum usable strikes survived, but the slice would
          // have reached the floor but for rows the fit-inversion audit dropped
          // — the gap was CREATED by the audit; count it so admission can
          // surface it instead of serving a silently thinner surface. Genuinely
          // sparse slices keep the historical silent-skip shape.
          ++n_audit_starved;
        }
        expiry_reports.push_back(
            ExpiryFitReport{chain_index, T, ExpiryFitOutcome::PrepStarved, 0, prep_code});
      }
      continue;
    }
    const double rate = prepared_result->rate;
    const double F = prepared_result->slice.forward();
    const double q_eff = prepared_result->q_eff;
    const double borrow = prepared_result->borrow;
    const double df = prepared_result->df;
    PreparedSlice prepared = std::move(prepared_result->slice);

    // 3. Fit the eSSVI slice (natural form, T/F stamped in). MonotoneFit adds a
    //    calendar floor vs. the previous fitted slice (theta floor + active-set
    //    w-floor over the data range); every other mode is the plain fit.
    const double t_fit = time_stages ? now_ns() : 0.0;
    FitDiag diag{};
    Result<EssviParams> slice_res =
        (in.repair == CalendarRepair::MonotoneFit)
            ? fit_slice_calendar_floored(prepared, T, F, in.calib, &diag,
                                         has_prev ? &prev_slice : nullptr, df)
            : essvi_fit_slice(prepared.fit_observations(), T, F, in.calib, &diag);
    if (time_stages)
      ms_fit += now_ns() - t_fit;
    if (!slice_res) {
      // W3.4 (F4): record the fit failure and, under the completeness contract,
      // propagate a HARD fit error instead of silently dropping the slice.
      const ErrorCode fit_code = slice_res.error().code();
      expiry_reports.push_back(ExpiryFitReport{
          chain_index, T, ExpiryFitOutcome::FitFailed, prepared.fit_observations().size(),
          fit_code});
      if (in.fail_board_on_hard_slice_error && !slice_error_is_expected(fit_code)) {
        return Err(fit_code, "run_surface_parity: expiry slice fit failed (hard): " +
                                 slice_res.error().to_string());
      }
      continue; // a slice that fails to fit contributes no slice
    }
    expiry_reports.push_back(ExpiryFitReport{
        chain_index, T, ExpiryFitOutcome::Fitted, prepared.fit_observations().size(),
        ErrorCode::Unknown});

    // Stamp this slice's real listed-expiry instant (+ its dense surface
    // write index) so downstream event-bucketing (solve_implied_emove,
    // count_events_at in session.cpp) can bracket against the actual
    // expiry instead of a Calendar365-inverse synthesis from T.
    slice_res->expiry_ns = chain.expiry_ns;
    slice_res->expiry_id = static_cast<std::uint16_t>(idx);

    prev_slice = *slice_res; // carry forward for the next slice's calendar floor
    has_prev = true;

    // 4. Write the slice into the surface at the next ascending index.
    ATX_TRY_VOID(surface.set_slice_essvi(idx, *slice_res));
    expiry_T.push_back(T);

    // 5. Retain the per-slice re-pricing context for the composable facade, and
    //    stash the aligned obs so this slice can be SCORED after the surface is
    //    fully assembled and (optionally) calendar-repaired.
    context.push_back(SliceContext{T, F, borrow, q_eff, prepared.fit_observations().size(),
                                   prepared.n_dropped()});
    // Perf C1: retain the carry diagnostics the preparation's
    // `resolve_chain_forward` already produced for this chain, ‖ context, so
    // `VolaSession::build`'s certification layer can reuse it instead of a
    // second, identical `resolve_chain_forward` call.
    carry_diag.push_back(prep_diag.carry);
    // Perf C1: capture the fit's own de-Am certification for this slice BEFORE
    // `prepared` is moved into `pending`, so VolaSession::build reuses it.
    input_certs.push_back(make_input_certification(prepared, chain));
    pending.push_back(
        PendingSlice{std::move(prepared), T, rate, q_eff, static_cast<std::uint16_t>(idx)});

    ++idx;
  }

  if (idx == 0) {
    return Err(ErrorCode::NotFound, "run_surface_parity: no expiry produced a usable eSSVI slice");
  }

  // 6. Calendar no-arbitrage on the assembled surface. Count the raw crossings
  //    BEFORE any repair (independent per-slice fits + wing extrapolation can
  //    cross in total variance), then optionally repair to arb-free.
  const double t_cal = time_stages ? now_ns() : 0.0;
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
    const double t_rep = time_stages ? now_ns() : 0.0;
    ATX_TRY_VOID(arb_project_calendar_essvi(surface, kArbKMin, kArbKMax, kArbNGrid));
    ATX_TRY_VOID(arb_repair_calendar_residual(surface, kArbKMin, kArbKMax, kArbNGrid));
    if (time_stages)
      ms_repair = now_ns() - t_rep;
  } else if (in.repair == CalendarRepair::MonotoneFit) {
    // The active-set fit uses penalty observations and can leave small
    // between-node crossings. Close those residuals over MonotoneFit's
    // documented near-money guarantee without projecting the extrapolated
    // wings, where a global theta bump materially degrades held fit quality.
    const double t_rep = time_stages ? now_ns() : 0.0;
    ATX_TRY(const std::vector<ArbViolation> near_money_viols,
            arb_check_calendar(surface, kMonotoneKMin, kMonotoneKMax, kArbNGrid));
    if (!near_money_viols.empty()) {
      ATX_TRY_VOID(arb_project_calendar_essvi(surface, kMonotoneKMin, kMonotoneKMax, kArbNGrid));
      ATX_TRY_VOID(arb_repair_calendar_residual(surface, kMonotoneKMin, kMonotoneKMax, kArbNGrid));
    }
    if (time_stages)
      ms_repair = now_ns() - t_rep;
  }
  double ms_calendar = time_stages ? (now_ns() - t_cal) : 0.0;

  // 7. Score per-expiry re-Am parity off the FINAL (possibly repaired) surface:
  //    the model IV is read back via iv_on_slice, so the number scored is the
  //    one the surface actually serves.
  const double t_parity = time_stages ? now_ns() : 0.0;
  for (const PendingSlice &ps : pending) {
    const PreparedScoreColumns &score = ps.prepared.score_columns();
    std::vector<double> model_iv;
    model_iv.reserve(score.k_log.size());
    for (const double k_log : score.k_log) {
      model_iv.push_back(surface.iv_on_slice(ps.slice_idx, k_log));
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
    pin.caches = in.deam.caches; // re-Am through the same hot-path caches
    ATX_TRY(const ParityReport parity, chain_parity(score.strike, score.bid, score.ask, score.mid,
                                                    score.side, model_iv, score.market_iv, pin));
    worst = std::min(worst, parity.frac_fv_within_bidask);
    per_expiry.push_back(parity);
  }
  const double ms_parity = time_stages ? (now_ns() - t_parity) : 0.0;

  // 8. Final calendar check on the surface the caller receives.
  const double final_calendar_start = time_stages ? now_ns() : 0.0;
  ATX_TRY(const std::vector<ArbViolation> cal_viols,
          arb_check_calendar(surface, kArbKMin, kArbKMax, kArbNGrid));
  const bool calendar_arb_free = cal_viols.empty();
  if (time_stages) {
    ms_calendar += now_ns() - final_calendar_start;
  }

  if (profile) {
    std::fprintf(stderr,
                 "[ATX_VOL_PROFILE] slices=%zu carry=%.1f deam=%.1f fit=%.1f "
                 "repair=%.1f parity=%.1f calendar=%.1f ms viol_pre=%zu "
                 "(carry=forward/borrow solve; deam=per-strike invert; "
                 "parity=re-Am score)\n",
                 idx, ms_carry, ms_deam, ms_fit, ms_repair, ms_parity, ms_calendar,
                 n_calendar_viol_pre);
  }

  SurfaceParityReport out{
      std::move(surface),    std::move(expiry_T),
      std::move(per_expiry), std::move(context),
      std::move(carry_diag), std::move(input_certs),
      worst,                 calendar_arb_free,
      idx,                   n_calendar_viol_pre,
      n_carry_skipped,       n_audit_starved,
  };
  out.expiry_reports = std::move(expiry_reports); // W3.4 (F4): ‖ under.chains, chain order
  if (in.collect_stage_timings) {
    out.fit_timings.carry_solve_ms = ms_carry;
    out.fit_timings.observation_deam_ms = ms_deam;
    out.fit_timings.slice_fit_ms = ms_fit;
    out.fit_timings.audit_ms = ms_parity;
    out.fit_timings.calendar_validation_ms = ms_calendar;
    out.fit_timings.total_wall_ms = now_ns() - fit_start;
    out.fit_timings.collected = true;
  }
  return Ok(std::move(out));
}

} // namespace atx::vol
