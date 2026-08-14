// Surface projection spine — implementation.
//
// Ports ats_vol_projection.c (Sprint 20 Stage I). See projection.hpp for the
// public contract and the port-scope adaptations. Where a primitive serves
// both the scalar Stage I paths and the Stage II portfolio-risk hot path (the
// inserted-slice IV blend), it is written here once.

#include "atx/vol/projection.hpp"

#include <algorithm>
#include <cmath>
#include <vector>

#include "atx/core/math.hpp"    // norm_cdf
#include "atx/vol/american.hpp" // american_price_cached
#include "atx/vol/arb.hpp"      // arb_check_butterfly_slice (opt-in no-arb sweep)
#include "atx/vol/black76.hpp"  // black76_price
#include "atx/vol/event_vol.hpp"  // EventSchedule, count_events_at, event_aware_w
#include "atx/vol/detail/strip_grid.hpp" // strip::forward_log_blend (E2 shared convention)

namespace atx::vol {

using atx::core::Err;
using atx::core::Ok;

namespace {

// Exact-pillar tolerance on the forward-curve T axis (~1 trading minute);
// matches the C ATS_VOL_T_EXACT_TOL and VolSurface::find_exact_T.
inline constexpr double kProjExactTTol = 1.0 / (252.0 * 6.5 * 60.0);

// Forward-delta, B76 convention: calls positive, puts negative.
[[nodiscard]] double forward_delta(double F, double K, double tau, double sigma,
                                   Side side) noexcept {
  if (!(tau > 0.0) || !(sigma > 0.0) || !(F > 0.0) || !(K > 0.0)) {
    return kQuietNaN;
  }
  const double v = sigma * std::sqrt(tau);
  const double d1 = (std::log(F / K) + 0.5 * v * v) / v;
  const double n_d1 = atx::core::norm_cdf(d1);
  return (side == Side::Call) ? n_d1 : (n_d1 - 1.0);
}

// Per-slice total variance w(k), matching VolSurface::eval_slice_w exactly so
// the inserted-slice blend reproduces VolSurface::w to machine precision.
[[nodiscard]] double slice_w(const VolSurface& s, std::uint16_t idx,
                             double k_log) noexcept {
  const std::size_t i = static_cast<std::size_t>(idx);
  switch (s.param()) {
    case Parametrization::Essvi: {
      const auto sl = s.essvi_slices();
      return (i < sl.size()) ? essvi_total_w(sl[i], k_log) : kQuietNaN;
    }
    case Parametrization::Svi:
    case Parametrization::SviMm: {
      const auto sl = s.svi_slices();
      return (i < sl.size()) ? svi_total_w(sl[i], k_log) : kQuietNaN;
    }
    case Parametrization::Wing:
    case Parametrization::C8:
    case Parametrization::CStar16M:
      return kQuietNaN;
  }
  return kQuietNaN;
}

[[nodiscard]] double slice_T(const VolSurface& s, std::uint16_t idx) noexcept {
  const std::size_t i = static_cast<std::size_t>(idx);
  switch (s.param()) {
    case Parametrization::Essvi: {
      const auto sl = s.essvi_slices();
      return (i < sl.size()) ? sl[i].T : kQuietNaN;
    }
    case Parametrization::Svi:
    case Parametrization::SviMm: {
      const auto sl = s.svi_slices();
      return (i < sl.size()) ? sl[i].T : kQuietNaN;
    }
    case Parametrization::Wing:
    case Parametrization::C8:
    case Parametrization::CStar16M:
      return kQuietNaN;
  }
  return kQuietNaN;
}

// SpiderRock FLEXVolInterpolation-style "vol-multiple" blend (see
// InterpMode::ShapeBlend for the full contract). `handle.alpha_T` doubles as
// wwHi; wwLo = 1 - wwHi. Falls back to the PiecewiseTotalVariance formula
// (linear-in-w at fixed k) when either parent's ATM total variance is
// non-finite / non-positive, or the blended ATM total variance / vol comes
// out non-finite / non-positive — this is a documented degeneracy fallback,
// not an error.
[[nodiscard]] double shape_blend_w(const VolSurface& s,
                                   const InsertedSliceHandle& h,
                                   double k_log, const EventSchedule* events,
                                   double emove, std::int64_t now_ns) noexcept {
  const auto lo = static_cast<std::uint16_t>(h.parent_lo_idx);
  const auto hi = static_cast<std::uint16_t>(h.parent_hi_idx);
  const double ww_hi = h.alpha_T;
  const double ww_lo = 1.0 - ww_hi;

  auto linear_w_fallback = [&]() noexcept {
    const double w_lo = slice_w(s, lo, k_log);
    const double w_hi = slice_w(s, hi, k_log);
    return w_lo + ww_hi * (w_hi - w_lo);
  };

  const double T_lo = slice_T(s, lo);
  const double T_hi = slice_T(s, hi);
  const double T_q = h.T_clock;
  if (!(T_lo > 0.0) || !(T_hi > 0.0) || !(T_q > 0.0)) {
    return linear_w_fallback();
  }

  // ATM total variance of each parent (k = 0), and the SpiderRock ATM blend
  // atm(T_q)^2 * T_q = wwLo*T_lo*atm_lo^2 + wwHi*T_hi*atm_hi^2
  //                  = wwLo*w_lo(0) + wwHi*w_hi(0)
  // — identical to the PiecewiseTotalVariance blend evaluated at k = 0, so
  // ATM matches it exactly by construction.
  const double w_lo0 = slice_w(s, lo, 0.0);
  const double w_hi0 = slice_w(s, hi, 0.0);
  if (!(std::isfinite(w_lo0) && w_lo0 > 0.0) ||
      !(std::isfinite(w_hi0) && w_hi0 > 0.0)) {
    return linear_w_fallback();
  }
  const double atm_lo = std::sqrt(w_lo0 / T_lo);
  const double atm_hi = std::sqrt(w_hi0 / T_hi);

  // ATM anchor: the ONE genuine two-point raw-total-variance blend step in
  // this mode (see the module doc on InterpMode::ShapeBlend -- it is
  // literally the same formula PiecewiseTotalVariance uses at k = 0). This
  // is the step `events` wraps with SpiderRock's earnings event-variance
  // model when active; the vol-multiple shape blending below it (m_lo/m_hi)
  // is unchanged either way -- the event contribution shifts the ATM level
  // the smile shape then reconstructs around, it does not reshape the smile.
  double w_atm_q;
  if (events != nullptr) {
    const std::size_t n_lo = count_events_at(*events, now_ns, T_lo);
    const std::size_t n_hi = count_events_at(*events, now_ns, T_hi);
    const std::size_t n_q = count_events_at(*events, now_ns, T_q);
    w_atm_q = event_aware_w(w_lo0, T_lo, n_lo, w_hi0, T_hi, n_hi, T_q, n_q, emove);
  } else {
    w_atm_q = ww_lo * w_lo0 + ww_hi * w_hi0;
  }
  if (!(std::isfinite(w_atm_q) && w_atm_q > 0.0)) {
    return linear_w_fallback();
  }
  const double atm_q = std::sqrt(w_atm_q / T_q);
  if (!(std::isfinite(atm_q) && atm_q > 0.0)) {
    return linear_w_fallback();
  }

  // Common standardized moneyness z, mapped into each parent's own
  // coordinates: k_x = z * atm_x * sqrt(T_x).
  const double z = k_log / (atm_q * std::sqrt(T_q));
  const double k_lo = z * atm_lo * std::sqrt(T_lo);
  const double k_hi = z * atm_hi * std::sqrt(T_hi);

  const double w_lo_k = slice_w(s, lo, k_lo);
  const double w_hi_k = slice_w(s, hi, k_hi);
  if (!(std::isfinite(w_lo_k) && w_lo_k >= 0.0) ||
      !(std::isfinite(w_hi_k) && w_hi_k >= 0.0)) {
    return linear_w_fallback();
  }

  // Vol multiples m_x(z) = sigma_x(k_x) / atm_x, blended and re-scaled by
  // atm(T_q) into the query's own vol, then back to total variance so the
  // caller-facing contract (w_on_inserted_slice returns total variance)
  // stays uniform across both interp modes.
  const double m_lo = std::sqrt(w_lo_k / T_lo) / atm_lo;
  const double m_hi = std::sqrt(w_hi_k / T_hi) / atm_hi;
  const double sigma_q = atm_q * (ww_lo * m_lo + ww_hi * m_hi);
  return sigma_q * sigma_q * T_q;
}

}  // namespace

// ── Defaults ─────────────────────────────────────────────────────────────

TimeModel time_model_clock() noexcept { return TimeModel{}; }

CoordConvertRequest coord_convert_request_default() noexcept {
  return CoordConvertRequest{};
}

EvalRequest eval_request_default() noexcept { return EvalRequest{}; }

// ── Volatility-time conversion ───────────────────────────────────────────

Result<TauVol> tau_vol_from_clock(const TimeModel& tm, double T_clock) {
  if (!(std::isfinite(T_clock) && T_clock > 0.0)) {
    return Err(ErrorCode::InvalidArgument, "non-positive T_clock");
  }
  if (tm.mode == TimeMode::Clock) {
    return TauVol{T_clock, 0u};
  }
  return Err(ErrorCode::NotImplemented, "reserved time mode");
}

// ── Non-pillar forward lookup ────────────────────────────────────────────

Result<ForwardLookup> curve_forward_T(const CurveSet& curves, double T,
                                      ProjExtrapPolicy extrap) {
  if (!(std::isfinite(T) && T > 0.0)) {
    return Err(ErrorCode::InvalidArgument, "non-positive T");
  }
  const std::span<const ForwardPoint> pts = curves.forward.points();
  if (pts.empty()) {
    return Err(ErrorCode::NotFound, "empty forward curve");
  }

  // Gather the valid (finite, positive) rows and sort ascending by T.
  struct Row {
    double T;
    double F;
    double q_eff;
    std::uint16_t idx;
  };
  std::vector<Row> rows;
  rows.reserve(pts.size());
  for (std::size_t i = 0; i < pts.size(); ++i) {
    const ForwardPoint& p = pts[i];
    if (!(std::isfinite(p.T) && p.T > 0.0)) continue;
    if (!(std::isfinite(p.F) && p.F > 0.0)) continue;
    rows.push_back(Row{p.T, p.F, p.q_eff, static_cast<std::uint16_t>(i)});
  }
  if (rows.empty()) {
    return Err(ErrorCode::NotFound, "no valid forward pillars");
  }
  std::sort(rows.begin(), rows.end(),
            [](const Row& a, const Row& b) noexcept { return a.T < b.T; });

  ForwardLookup out;
  out.T = T;
  out.df = curves.yield.disc(T);
  out.r = curves.yield.zero(T);
  const double spot = curves.spot;

  const double T_first = rows.front().T;
  const double T_last = rows.back().T;

  // Exact pillar — favoured fast path (no interpolation flag).
  for (const Row& row : rows) {
    if (std::fabs(row.T - T) <= kProjExactTTol) {
      out.F = row.F;
      out.lo_idx = row.idx;
      out.hi_idx = row.idx;
      out.q_eff = row.q_eff;
      return out;
    }
  }

  // Out of bracket.
  if (T < T_first || T > T_last) {
    out.flags |= kFlagExtrapolatedT;
    if (extrap == ProjExtrapPolicy::ClampForReporting) {
      const Row& pick = (T < T_first) ? rows.front() : rows.back();
      out.F = pick.F;
      out.lo_idx = pick.idx;
      out.hi_idx = pick.idx;
      if (spot > 0.0 && T > 0.0) {
        out.q_eff = out.r - std::log(out.F / spot) / T;
      }
      return out;
    }
    out.flags |= kFlagInvalid;
    return Err(ErrorCode::OutOfRange, "forward extrapolation forbidden");
  }

  // Bracket-search over the sorted valid rows.
  std::size_t lo_pos = 0;
  std::size_t hi_pos = rows.size() - 1;
  while (hi_pos > lo_pos + 1) {
    const std::size_t mid = (lo_pos + hi_pos) / 2;
    if (rows[mid].T <= T) {
      lo_pos = mid;
    } else {
      hi_pos = mid;
    }
  }
  const Row& plo = rows[lo_pos];
  const Row& phi = rows[hi_pos];

  // Linear in log(F): keeps the result positive and monotone-friendly. E2
  // single-sourced this into `strip::forward_log_blend` so the derivatives var
  // strip reads the same forward at the same T (it used to interpolate linearly
  // in F). Bit-identical to the expression it replaces on a validated bracket.
  out.F = strip::forward_log_blend(plo.T, plo.F, phi.T, phi.F, T);
  out.lo_idx = plo.idx;
  out.hi_idx = phi.idx;
  out.flags |= kFlagForwardInterp;
  if (spot > 0.0 && T > 0.0 && out.F > 0.0) {
    out.q_eff = out.r - std::log(out.F / spot) / T;
  }
  return out;
}

// ── Inserted-slice helpers ───────────────────────────────────────────────

namespace {

// Builds the inserted-slice handle. `pre`, when non-null, is an ALREADY-
// RESOLVED forward lookup for this exact (curves, T_clock, extrap) — used by
// `surface_eval_ex`'s ShapeBlend branch, which has already run
// `curve_forward_T` once inside `convert_coord` for the very same query and
// would otherwise redo it here. `surface_insert_vol_slice` (the public entry)
// always passes `pre = nullptr` and resolves the forward itself, unchanged.
[[nodiscard]] Result<InsertedSliceHandle> build_inserted_slice(
    const VolSurface& surface, const CurveSet* curves, const TimeModel& tm,
    double T_clock, InterpMode interp, ProjExtrapPolicy extrap,
    const ForwardLookup* pre) {
  InsertedSliceHandle out;
  out.uid = surface.uid();
  out.T_clock = T_clock;
  out.interp_mode = interp;

  const std::size_t n = surface.n_slices();
  if (n == 0) {
    out.flags |= kFlagInvalid;
    return Err(ErrorCode::NotFound, "surface has no slices");
  }
  if (interp != InterpMode::PiecewiseTotalVariance &&
      interp != InterpMode::ShapeBlend) {
    out.flags |= kFlagInvalid;
    return Err(ErrorCode::NotImplemented, "reserved interp mode");
  }

  auto tau = tau_vol_from_clock(tm, T_clock);
  if (!tau) {
    out.flags |= kFlagInvalid;
    return Err(std::move(tau).error());
  }
  out.tau_vol = tau->tau_vol;
  out.flags |= tau->flags;
  if (tm.mode != TimeMode::Clock) {
    out.flags |= kFlagVolTimeConverted;
  }

  // Bracket on the surface's slice-T axis (slices ascending by T).
  const std::uint16_t exact = surface.find_exact_T(T_clock);
  if (exact != 0xFFFFu) {
    out.parent_lo_idx = exact;
    out.parent_hi_idx = exact;
    out.alpha_T = 0.0;
    out.exact_slice_idx = static_cast<std::int32_t>(exact);
    out.flags |= kResolverNativeFastPath;
  } else {
    const std::size_t last = n - 1;
    const double T_first = slice_T(surface, 0);
    const double T_last = slice_T(surface, static_cast<std::uint16_t>(last));
    if (T_clock < T_first || T_clock > T_last) {
      out.flags |= kFlagExtrapolatedT;
      if (extrap == ProjExtrapPolicy::ClampForReporting) {
        const std::size_t pick = (T_clock < T_first) ? std::size_t{0} : last;
        out.parent_lo_idx = static_cast<std::uint32_t>(pick);
        out.parent_hi_idx = static_cast<std::uint32_t>(pick);
        out.alpha_T = 0.0;
        out.exact_slice_idx = static_cast<std::int32_t>(pick);
      } else {
        out.flags |= kFlagInvalid;
        return Err(ErrorCode::OutOfRange, "slice-T extrapolation forbidden");
      }
    } else {
      std::size_t lo = 0;
      std::size_t hi = last;
      while (hi > lo + 1) {
        const std::size_t mid = (lo + hi) / 2;
        if (slice_T(surface, static_cast<std::uint16_t>(mid)) <= T_clock) {
          lo = mid;
        } else {
          hi = mid;
        }
      }
      out.parent_lo_idx = static_cast<std::uint32_t>(lo);
      out.parent_hi_idx = static_cast<std::uint32_t>(hi);
      const double T_lo = slice_T(surface, static_cast<std::uint16_t>(lo));
      const double T_hi = slice_T(surface, static_cast<std::uint16_t>(hi));
      out.alpha_T = (T_clock - T_lo) / (T_hi - T_lo);
      out.flags |= kFlagInterpolatedT;
      out.flags |= kFlagInsertedSlice;
      if (interp == InterpMode::ShapeBlend) {
        // Genuine two-slice blend: ATM still matches PiecewiseTotalVariance
        // by construction, but non-ATM calendar-arbitrage safety is not
        // guaranteed (InterpMode::ShapeBlend doc).
        out.flags |= kFlagShapeBlendCalendarUnsafe;
      }
    }
  }

  // Forward / discount cache (optional). A pre-resolved lookup (`pre`) is
  // used as-is -- no second `curve_forward_T` call; otherwise resolve it here
  // exactly as before.
  if (pre != nullptr) {
    out.F = pre->F;
    out.df = pre->df;
    out.r = pre->r;
    out.q_eff = pre->q_eff;
    out.logF = (pre->F > 0.0) ? std::log(pre->F) : kQuietNaN;
    out.sqrtT = (T_clock > 0.0) ? std::sqrt(T_clock) : kQuietNaN;
    out.flags |= pre->flags;
  } else if (curves != nullptr) {
    auto fl = curve_forward_T(*curves, T_clock, extrap);
    if (fl) {
      out.F = fl->F;
      out.df = fl->df;
      out.r = fl->r;
      out.q_eff = fl->q_eff;
      out.logF = (fl->F > 0.0) ? std::log(fl->F) : kQuietNaN;
      out.sqrtT = (T_clock > 0.0) ? std::sqrt(T_clock) : kQuietNaN;
      out.flags |= fl->flags;
    }
    // If the forward lookup failed under Forbid, leave the cache NaN so
    // IV-only callers still get a usable handle.
  }
  return out;
}

// Opt-in dense no-arb sweep over an already-resolved handle. Fixed convention
// (documented on `surface_insert_vol_slice`): 128 intervals over
// k in [-h, +h], h = kNoArbSigmaSpan * sqrt(w_atm) clamped to
// [kNoArbSpanMin, kNoArbSpanMax]. Butterfly reuses `arb_check_butterfly_slice`
// so an inserted slice is judged by the same finite-difference density scheme
// as a fitted one. Calendar asks the narrower question this handle can
// actually answer: does the DERIVED slice's total variance stay inside the
// band its two parents span? PiecewiseTotalVariance satisfies that by
// construction (it is a convex combination), so the bit fires there only when
// the PARENTS themselves cross — which is precisely the calendar-arb region
// the caller wants flagged — while ShapeBlend can genuinely leave the band
// (see InterpMode::ShapeBlend's documented non-ATM calendar caveat).
inline constexpr double kNoArbSigmaSpan = 4.0;
inline constexpr double kNoArbSpanMin = 0.1;
inline constexpr double kNoArbSpanMax = 1.5;
inline constexpr std::uint32_t kNoArbGrid = 128u;
// Task F-4: was a local `1.0e-7` whose comment asked the reader to keep it
// matching arb.cpp; it now IS arb.cpp's constant (types.hpp).
inline constexpr double kNoArbCalendarTol = kCalendarTotalVarianceTol;

[[nodiscard]] std::uint32_t inserted_slice_no_arb_status(
    const VolSurface& surface, const InsertedSliceHandle& h) {
  const double w_atm = w_on_inserted_slice(surface, h, 0.0);
  if (!(std::isfinite(w_atm) && w_atm > 0.0)) {
    return kNoArbStatusNotEvaluated;
  }
  double span = kNoArbSigmaSpan * std::sqrt(w_atm);
  span = std::clamp(span, kNoArbSpanMin, kNoArbSpanMax);

  std::uint32_t status = 0u;

  const auto bf = arb_check_butterfly_slice(
      [&surface, &h](double k) { return w_on_inserted_slice(surface, h, k); },
      h.T_clock, -span, span, kNoArbGrid);
  if (!bf) {
    // The only failure mode is k_max <= k_min, which the clamp above rules
    // out; treat any surprise as "not evaluated" rather than "clean".
    return status | kNoArbStatusNotEvaluated;
  }
  if (!bf->empty()) {
    status |= kNoArbStatusButterfly;
  }

  if (h.exact_slice_idx < 0) {
    const auto lo = static_cast<std::uint16_t>(h.parent_lo_idx);
    const auto hi = static_cast<std::uint16_t>(h.parent_hi_idx);
    const double dk = (2.0 * span) / static_cast<double>(kNoArbGrid);
    for (std::uint32_t g = 0; g <= kNoArbGrid; ++g) {
      const double k = -span + static_cast<double>(g) * dk;
      const double w_lo = slice_w(surface, lo, k);
      const double w_hi = slice_w(surface, hi, k);
      const double w_q = w_on_inserted_slice(surface, h, k);
      if (!std::isfinite(w_lo) || !std::isfinite(w_hi) ||
          !std::isfinite(w_q)) {
        continue;  // same skip rule the surface-level calendar check uses
      }
      const double band_lo = std::min(w_lo, w_hi);
      const double band_hi = std::max(w_lo, w_hi);
      if (w_q - band_hi > kNoArbCalendarTol ||
          band_lo - w_q > kNoArbCalendarTol ||
          w_lo - w_hi > kNoArbCalendarTol) {
        status |= kNoArbStatusCalendar;
        break;
      }
    }
  }
  return status;
}

}  // namespace

Result<InsertedSliceHandle> surface_insert_vol_slice(
    const VolSurface& surface, const CurveSet* curves, const TimeModel& tm,
    double T_clock, InterpMode interp, ProjExtrapPolicy extrap,
    bool with_no_arb_check) {
  auto handle = build_inserted_slice(surface, curves, tm, T_clock, interp,
                                     extrap, /*pre=*/nullptr);
  if (!handle || !with_no_arb_check) {
    return handle;
  }
  const std::uint32_t status = inserted_slice_no_arb_status(surface, *handle);
  handle->no_arb_status = status;
  // `status != 0u` also covers kNoArbStatusNotEvaluated (sweep couldn't run,
  // e.g. non-finite ATM w), so kFlagNoArbWarning does NOT by itself imply a
  // confirmed density/calendar violation -- callers must inspect
  // `no_arb_status` to tell "violated" from "not evaluated".
  if (status != 0u) {
    handle->flags |= kFlagNoArbWarning;
  }
  return handle;
}

double w_on_inserted_slice(const VolSurface& surface,
                           const InsertedSliceHandle& handle,
                           double k_log, const EventSchedule* events,
                           double emove, std::int64_t now_ns) noexcept {
  if (handle.exact_slice_idx >= 0) {
    return slice_w(surface, static_cast<std::uint16_t>(handle.exact_slice_idx),
                   k_log);
  }
  if (handle.interp_mode == InterpMode::ShapeBlend) {
    return shape_blend_w(surface, handle, k_log, events, emove, now_ns);
  }
  const auto lo = static_cast<std::uint16_t>(handle.parent_lo_idx);
  const auto hi = static_cast<std::uint16_t>(handle.parent_hi_idx);
  const double w_lo = slice_w(surface, lo, k_log);
  const double w_hi = slice_w(surface, hi, k_log);
  if (events == nullptr) {
    return w_lo + handle.alpha_T * (w_hi - w_lo);
  }
  // PiecewiseTotalVariance has no separate ATM/shape split -- the SAME raw-w
  // blend formula runs at every k_log, so the event-aware wrap runs at the
  // query's own k_log directly (contrast ShapeBlend's ATM-anchor-only wrap
  // above).
  const double T_lo = slice_T(surface, lo);
  const double T_hi = slice_T(surface, hi);
  const double T_q = handle.T_clock;
  const std::size_t n_lo = count_events_at(*events, now_ns, T_lo);
  const std::size_t n_hi = count_events_at(*events, now_ns, T_hi);
  const std::size_t n_q = count_events_at(*events, now_ns, T_q);
  return event_aware_w(w_lo, T_lo, n_lo, w_hi, T_hi, n_hi, T_q, n_q, emove);
}

double iv_on_inserted_slice(const VolSurface& surface,
                            const InsertedSliceHandle& handle,
                            double k_log) noexcept {
  const double w = w_on_inserted_slice(surface, handle, k_log);
  if (!(std::isfinite(w) && w > 0.0)) return kQuietNaN;
  if (!(handle.tau_vol > 0.0)) return kQuietNaN;
  return std::sqrt(w / handle.tau_vol);
}

Status iv_on_inserted_slice_batch(const VolSurface& surface,
                                  const InsertedSliceHandle& handle,
                                  std::span<const double> k_log,
                                  std::span<double> out_iv) {
  if (out_iv.size() != k_log.size()) {
    return Err(ErrorCode::InvalidArgument, "k_log / out_iv size mismatch");
  }
  if (handle.exact_slice_idx < 0 && !(handle.tau_vol > 0.0)) {
    for (double& v : out_iv) v = kQuietNaN;
    return Err(ErrorCode::InvalidArgument, "degenerate tau_vol");
  }
  for (std::size_t i = 0; i < k_log.size(); ++i) {
    out_iv[i] = iv_on_inserted_slice(surface, handle, k_log[i]);
  }
  return Ok();
}

// ── Coordinate conversion ────────────────────────────────────────────────

Result<CoordConvertResult> convert_coord(const VolSurface& surface,
                                         const CurveSet& curves,
                                         const TimeModel& tm,
                                         const CoordConvertRequest& request) {
  // Reserved delta conventions are rejected rather than guessed.
  if (request.from_kind == CoordKind::Delta ||
      request.to_kind == CoordKind::Delta) {
    if (request.delta_convention != DeltaConvention::Forward) {
      return Err(ErrorCode::NotImplemented, "reserved delta convention");
    }
  }

  CoordConvertResult out;
  out.T_clock = request.T_clock;

  auto tau = tau_vol_from_clock(tm, request.T_clock);
  if (!tau) {
    return Err(std::move(tau).error());
  }
  out.tau_vol = tau->tau_vol;
  out.flags |= tau->flags;
  if (tm.mode != TimeMode::Clock) {
    out.flags |= kFlagVolTimeConverted;
  }

  auto fwd = curve_forward_T(curves, request.T_clock, request.extrap_policy);
  if (!fwd) {
    return Err(std::move(fwd).error());
  }
  out.F = fwd->F;
  out.df = fwd->df;
  out.r = fwd->r;
  out.q_eff = fwd->q_eff;
  out.flags |= fwd->flags;

  double K = kQuietNaN;
  double k_log = kQuietNaN;
  switch (request.from_kind) {
    case CoordKind::Strike:
      K = request.x;
      if (!(K > 0.0) || !(out.F > 0.0)) {
        return Err(ErrorCode::InvalidArgument, "bad strike / forward");
      }
      k_log = std::log(K) - std::log(out.F);
      break;
    case CoordKind::LogMoneyness:
      k_log = request.x;
      if (!std::isfinite(k_log) || !(out.F > 0.0)) {
        return Err(ErrorCode::InvalidArgument, "bad log-moneyness / forward");
      }
      K = out.F * std::exp(k_log);
      break;
    case CoordKind::StandardMoneyness:
      if (!(curves.spot > 0.0) || !(out.F > 0.0)) {
        return Err(ErrorCode::InvalidArgument, "bad spot / forward");
      }
      K = curves.spot * request.x;
      if (!(K > 0.0)) {
        return Err(ErrorCode::InvalidArgument, "non-positive strike");
      }
      k_log = std::log(K) - std::log(out.F);
      break;
    case CoordKind::Delta: {
      auto inner = surface_solve_k_for_delta(
          surface, curves, tm, request.T_clock, request.x, request.side,
          request.delta_convention, request.extrap_policy);
      if (!inner) {
        return Err(std::move(inner).error());
      }
      K = inner->K;
      k_log = inner->k_log;
      out.flags |= inner->flags;
      out.quote_delta = inner->quote_delta;
      break;
    }
  }

  out.K = K;
  out.k_log = k_log;
  if (curves.spot > 0.0) {
    out.standard_moneyness = K / curves.spot;
  }

  // Output delta requested (and input wasn't delta): surface-implied delta.
  if (request.to_kind == CoordKind::Delta &&
      request.from_kind != CoordKind::Delta) {
    const double iv = surface.iv(k_log, tau->tau_vol);
    if (std::isfinite(iv) && iv > 0.0) {
      out.quote_delta = forward_delta(out.F, K, tau->tau_vol, iv, request.side);
    }
  }
  return out;
}

// ── Extended evaluation ──────────────────────────────────────────────────

Result<EvalResult> surface_eval_ex(const VolSurface& surface,
                                   const CurveSet& curves,
                                   const CorrectionCache* correction,
                                   const TimeModel& tm,
                                   const EvalRequest& request) {
  // AL_CORRECTION (interpolated correction across T) is reserved.
  if (request.pricing_route_policy == RoutePolicy::AlCorrection) {
    return Err(ErrorCode::NotImplemented, "AL correction route deferred");
  }
  if (request.interp_mode != InterpMode::PiecewiseTotalVariance &&
      request.interp_mode != InterpMode::ShapeBlend) {
    return Err(ErrorCode::NotImplemented, "reserved interp mode");
  }

  CoordConvertRequest creq;
  creq.T_clock = request.T_clock;
  creq.x = request.x;
  creq.from_kind = request.coord_kind;
  creq.to_kind = CoordKind::LogMoneyness;
  creq.side = request.side;
  creq.delta_convention = request.delta_convention;
  creq.time_mode = request.time_mode;
  creq.extrap_policy = request.extrap_policy;

  auto cr = convert_coord(surface, curves, tm, creq);
  if (!cr) {
    return Err(std::move(cr).error());
  }

  EvalResult out;
  out.T_clock = cr->T_clock;
  out.tau_vol = cr->tau_vol;
  out.K = cr->K;
  out.k_log = cr->k_log;
  out.F = cr->F;
  out.df = cr->df;
  out.r = cr->r;
  out.q_eff = cr->q_eff;
  out.flags |= cr->flags;
  if (request.coord_kind == CoordKind::Delta) {
    out.quote_delta = cr->quote_delta;
  }

  double w = kQuietNaN;
  if (request.interp_mode == InterpMode::ShapeBlend || request.events != nullptr) {
    // Route through the inserted-slice path so the blend can see both
    // bracketing slices individually: ShapeBlend always needs this (to
    // reconstruct the vol-multiple shape); the event-aware wrap needs it
    // too, for EITHER interp mode, since `w_on_inserted_slice`'s event-aware
    // overload is what actually calls `event_aware_w` (see its doc) --
    // `VolSurface::w()` (the plain-PiecewiseTotalVariance fast path below)
    // has no way to see the two parent slices separately. A null `events`
    // with PiecewiseTotalVariance never reaches this branch, so that path
    // (the default) stays on the untouched `surface.w()` call below --
    // bit-identical to the pre-event-vol behavior.
    //
    // `convert_coord` above already ran `curve_forward_T` for this exact
    // (curves, T_clock, extrap_policy) to produce `cr`; reuse it (`cr->F` /
    // `df` / `r` / `q_eff` are copied straight from that lookup) instead of
    // resolving the forward a second time inside the inserted-slice builder.
    // Masked to the bits `curve_forward_T` itself can set on success (not
    // `cr->flags` wholesale) so a flag some OTHER branch of `convert_coord`
    // adds for an unrelated reason (e.g. the Delta coordinate-solve path)
    // can never leak into the handle's provenance.
    ForwardLookup pre_fwd;
    pre_fwd.T = request.T_clock;
    pre_fwd.F = cr->F;
    pre_fwd.df = cr->df;
    pre_fwd.r = cr->r;
    pre_fwd.q_eff = cr->q_eff;
    pre_fwd.flags = cr->flags & (kFlagExtrapolatedT | kFlagForwardInterp);

    auto handle = build_inserted_slice(surface, &curves, tm, request.T_clock,
                                       request.interp_mode,
                                       request.extrap_policy, &pre_fwd);
    if (!handle) {
      out.flags |= kFlagInvalid;
      return Err(std::move(handle).error());
    }
    out.flags |= handle->flags;
    w = w_on_inserted_slice(surface, *handle, cr->k_log, request.events,
                            request.emove, request.now_ns);
  } else {
    w = surface.w(cr->k_log, cr->tau_vol);
  }
  if (!(std::isfinite(w) && w > 0.0)) {
    out.flags |= kFlagInvalid;
    return Err(ErrorCode::OutOfRange, "non-positive total variance");
  }
  out.total_variance = w;
  const double iv = std::sqrt(w / cr->tau_vol);
  out.iv = iv;

  if (request.coord_kind != CoordKind::Delta) {
    out.quote_delta = forward_delta(cr->F, cr->K, cr->tau_vol, iv, request.side);
  }

  switch (request.pricing_route_policy) {
    case RoutePolicy::B76Only:
      out.price = black76_price(cr->F, cr->K, cr->T_clock, iv, cr->df, request.side);
      out.pricing_route = RoutePolicy::B76Only;
      out.flags |= kFlagRouteB76Only;
      break;
    case RoutePolicy::B76AlCache:
      if (correction != nullptr && correction->populated()) {
        const double S = (curves.spot > 0.0) ? curves.spot : cr->F;
        const double q = std::isfinite(cr->q_eff) ? cr->q_eff : 0.0;
        out.price = american_price_cached(S, cr->K, cr->T_clock, iv, cr->r, q,
                                          request.side, correction);
        if (!std::isfinite(out.price)) {
          // The cached pricer returns NaN in the double-continuation regime
          // (put q < r <= 0 / call r < q <= 0); do not launder it into an Ok
          // price. In practice unreachable — a populated cache implies a
          // priceable (r, q) — but guard rather than emit a silent NaN.
          out.flags |= kFlagInvalid;
          return Err(ErrorCode::NotImplemented,
                     "american_price_cached: double-continuation regime");
        }
        out.pricing_route = RoutePolicy::B76AlCache;
        out.flags |= kFlagRouteAmerican;
      } else {
        out.price = black76_price(cr->F, cr->K, cr->T_clock, iv, cr->df,
                                  request.side);
        out.pricing_route = RoutePolicy::B76Only;
        out.flags |= kFlagRouteB76Only;
        out.flags |= kResolverRouteFallbackB76;
      }
      break;
    case RoutePolicy::AlCorrection:
      out.flags |= kFlagInvalid;
      return Err(ErrorCode::NotImplemented, "AL correction route deferred");
  }
  return out;
}

Status surface_eval_grid(const VolSurface& surface, const CurveSet& curves,
                         const CorrectionCache* correction, const TimeModel& tm,
                         std::span<const EvalRequest> requests,
                         std::span<EvalResult> out_results) {
  if (out_results.size() != requests.size()) {
    return Err(ErrorCode::InvalidArgument, "requests / results size mismatch");
  }
  Status last = Ok();
  for (std::size_t i = 0; i < requests.size(); ++i) {
    auto r = surface_eval_ex(surface, curves, correction, tm, requests[i]);
    if (r) {
      out_results[i] = *r;
    } else {
      out_results[i] = EvalResult{};
      last = Err(r.error());
    }
  }
  return last;
}

// ── Delta inversion ──────────────────────────────────────────────────────

namespace {

inline constexpr double kDeltaBracketLo = -2.0;
inline constexpr double kDeltaBracketHi = 2.0;
inline constexpr double kDeltaTol = 1.0e-9;
inline constexpr int kDeltaMaxIter = 64;

[[nodiscard]] double delta_at_k(const VolSurface& surface, double F, double tau,
                                double k_log, Side side) noexcept {
  const double K = F * std::exp(k_log);
  const double iv = surface.iv(k_log, tau);
  if (!(std::isfinite(iv) && iv > 0.0)) return kQuietNaN;
  return forward_delta(F, K, tau, iv, side);
}

}  // namespace

Result<CoordConvertResult> surface_solve_k_for_delta(
    const VolSurface& surface, const CurveSet& curves, const TimeModel& tm,
    double T_clock, double target_delta, Side side, DeltaConvention convention,
    ProjExtrapPolicy extrap) {
  if (convention != DeltaConvention::Forward) {
    return Err(ErrorCode::NotImplemented, "reserved delta convention");
  }
  // Sign sanity: calls in (0, 1), puts in (-1, 0).
  if (side == Side::Call) {
    if (!(target_delta > 0.0 && target_delta < 1.0)) {
      return Err(ErrorCode::NotFound, "call delta out of (0,1)");
    }
  } else {
    if (!(target_delta < 0.0 && target_delta > -1.0)) {
      return Err(ErrorCode::NotFound, "put delta out of (-1,0)");
    }
  }

  CoordConvertResult out;
  out.T_clock = T_clock;

  auto tau = tau_vol_from_clock(tm, T_clock);
  if (!tau) {
    return Err(std::move(tau).error());
  }
  out.tau_vol = tau->tau_vol;
  out.flags |= tau->flags;

  auto fwd = curve_forward_T(curves, T_clock, extrap);
  if (!fwd) {
    return Err(std::move(fwd).error());
  }
  out.F = fwd->F;
  out.df = fwd->df;
  out.r = fwd->r;
  out.q_eff = fwd->q_eff;
  out.flags |= fwd->flags;

  double k_lo = kDeltaBracketLo;
  double k_hi = kDeltaBracketHi;
  double d_lo = delta_at_k(surface, fwd->F, tau->tau_vol, k_lo, side);
  double d_hi = delta_at_k(surface, fwd->F, tau->tau_vol, k_hi, side);

  if (!std::isfinite(d_lo) || !std::isfinite(d_hi)) {
    for (int shrink = 0; shrink < 8; ++shrink) {
      if (!std::isfinite(d_lo)) {
        k_lo += 0.25;
        d_lo = delta_at_k(surface, fwd->F, tau->tau_vol, k_lo, side);
      }
      if (!std::isfinite(d_hi)) {
        k_hi -= 0.25;
        d_hi = delta_at_k(surface, fwd->F, tau->tau_vol, k_hi, side);
      }
      if (std::isfinite(d_lo) && std::isfinite(d_hi)) break;
    }
    if (!std::isfinite(d_lo) || !std::isfinite(d_hi) || k_hi <= k_lo) {
      out.flags |= kFlagDeltaNotBracketed;
      return Err(ErrorCode::Unavailable, "delta bracket collapsed");
    }
  }

  const double diff_lo = d_lo - target_delta;
  const double diff_hi = d_hi - target_delta;
  if (diff_lo * diff_hi > 0.0) {
    out.flags |= kFlagDeltaNotBracketed;
    return Err(ErrorCode::NotFound, "delta target not bracketed");
  }

  double k_mid = 0.5 * (k_lo + k_hi);
  double d_mid = kQuietNaN;
  for (int it = 0; it < kDeltaMaxIter; ++it) {
    k_mid = 0.5 * (k_lo + k_hi);
    d_mid = delta_at_k(surface, fwd->F, tau->tau_vol, k_mid, side);
    if (!std::isfinite(d_mid)) {
      out.flags |= kFlagDeltaNotBracketed;
      return Err(ErrorCode::Unavailable, "delta blew up mid-solve");
    }
    if (std::fabs(d_mid - target_delta) < kDeltaTol ||
        (k_hi - k_lo) < 1.0e-12) {
      break;
    }
    const double dm = d_mid - target_delta;
    if (dm * (d_lo - target_delta) < 0.0) {
      k_hi = k_mid;
      d_hi = d_mid;
    } else {
      k_lo = k_mid;
      d_lo = d_mid;
    }
  }

  out.k_log = k_mid;
  out.K = fwd->F * std::exp(k_mid);
  out.quote_delta = d_mid;
  if (curves.spot > 0.0) {
    out.standard_moneyness = out.K / curves.spot;
  }
  return out;
}

// ── Surface-to-surface project_compare ───────────────────────────────────

Status surface_project_compare(const ProjectCompareInputs& in,
                               std::span<ProjectGridRow> rows) {
  if (in.source_surface == nullptr || in.target_surface == nullptr) {
    return Err(ErrorCode::InvalidArgument, "null surface");
  }
  if (in.source_curves == nullptr || in.target_curves == nullptr) {
    return Err(ErrorCode::InvalidArgument, "null curves");
  }

  TimeModel tm;
  tm.mode = in.time_mode;

  Status last = Ok();
  for (ProjectGridRow& row : rows) {
    EvalRequest req = eval_request_default();
    req.T_clock = row.T_clock;
    req.x = row.x;
    req.coord_kind = row.coord_kind;
    req.side = row.side;
    req.interp_mode = in.interp_mode;
    req.extrap_policy = in.extrap_policy;
    req.delta_convention = in.delta_convention;
    req.time_mode = in.time_mode;
    req.pricing_route_policy = in.route_policy;

    auto rs = surface_eval_ex(*in.source_surface, *in.source_curves,
                              in.source_correction, tm, req);
    if (rs) {
      row.from_source = *rs;
    } else {
      row.from_source = EvalResult{};
      last = Err(rs.error());
    }

    // Target side. Forward basis matters only for the LogMoneyness path: a
    // shared K is forwarded to both sides under the chosen basis.
    EvalRequest req_t = req;
    if (req.coord_kind == CoordKind::LogMoneyness &&
        in.basis != ForwardBasis::Self) {
      double F_basis = kQuietNaN;
      if (in.basis == ForwardBasis::Source) {
        F_basis = row.from_source.F;
      } else if (in.basis == ForwardBasis::Target) {
        auto fl = curve_forward_T(*in.target_curves, row.T_clock, in.extrap_policy);
        if (fl) F_basis = fl->F;
      } else if (in.basis == ForwardBasis::External) {
        F_basis = in.external_F;
      }
      if (std::isfinite(F_basis) && F_basis > 0.0) {
        req_t.x = F_basis * std::exp(row.x);
        req_t.coord_kind = CoordKind::Strike;
      }
    }

    auto rt = surface_eval_ex(*in.target_surface, *in.target_curves,
                              in.target_correction, tm, req_t);
    if (rt) {
      row.from_target = *rt;
    } else {
      row.from_target = EvalResult{};
      last = Err(rt.error());
    }

    row.price_diff = row.from_target.price - row.from_source.price;
    row.iv_diff = row.from_target.iv - row.from_source.iv;
    row.union_flags = row.from_source.flags | row.from_target.flags;
  }
  return last;
}

}  // namespace atx::vol
