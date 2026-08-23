#include "atx/vol/api/pricing/rates_curve.hpp"

#include <cmath>

#include "atx/core/error.hpp"
#include "atx/vol/api/core/vol_time.hpp" // kCalendarYearNs (THE calendar-365 year length)

namespace atx::vol {

using atx::core::Err;
using atx::core::ErrorCode;
using atx::core::Ok;

namespace {

// Fritsch-Carlson (1980) monotone-preserving tangents for a piecewise
// cubic-Hermite interpolant over (t, y=log_df). Mirrors ats_curve.c's
// `fritsch_carlson_compute_tangents` exactly. (The C splits its secant
// scratch `d` between a 64-pillar stack buffer and a malloc fallback for
// larger curves — a pure allocation-strategy detail; std::vector here
// always heap-allocates the scratch, which is numerically identical.)
void fritsch_carlson_compute_tangents(std::span<const double> t, std::span<const double> y,
                                      std::span<double> m_out) noexcept {
  const std::size_t n = t.size();
  if (n == 0) {
    return;
  }
  if (n == 1) {
    m_out[0] = 0.0;
    return;
  }

  std::vector<double> d(n - 1);

  // Step 1: secant slopes.
  for (std::size_t i = 0; i + 1 < n; ++i) {
    const double dt = t[i + 1] - t[i];
    d[i] = (dt > 0.0) ? (y[i + 1] - y[i]) / dt : 0.0;
  }

  // Step 2: initial tangents.
  m_out[0] = d[0];
  for (std::size_t i = 1; i + 1 < n; ++i) {
    m_out[i] = 0.5 * (d[i - 1] + d[i]);
  }
  m_out[n - 1] = d[n - 2];

  // Step 3 + 5: zero out at flat segments.
  for (std::size_t i = 0; i + 1 < n; ++i) {
    if (d[i] == 0.0) {
      m_out[i] = 0.0;
      m_out[i + 1] = 0.0;
    }
  }

  // Steps 4 + 5: enforce the de Boor/Swartz monotonicity disc.
  for (std::size_t i = 0; i + 1 < n; ++i) {
    if (d[i] == 0.0) {
      continue;
    }
    const double alpha = m_out[i] / d[i];
    const double beta = m_out[i + 1] / d[i];
    if (alpha < 0.0) {
      m_out[i] = 0.0;
    }
    if (beta < 0.0) {
      m_out[i + 1] = 0.0;
    }
    const double s = alpha * alpha + beta * beta;
    if (s > 9.0) {
      const double tau = 3.0 / std::sqrt(s);
      m_out[i] = tau * alpha * d[i];
      m_out[i + 1] = tau * beta * d[i];
    }
  }
}

} // namespace

// ── YieldCurve ────────────────────────────────────────────────────────────

Result<YieldCurve> YieldCurve::create(std::span<const double> t_years,
                                      std::span<const double> zero_rates) {
  if (t_years.empty() || zero_rates.empty()) {
    return Err(ErrorCode::InvalidArgument, "YieldCurve::create: empty pillars");
  }
  if (t_years.size() != zero_rates.size()) {
    return Err(ErrorCode::InvalidArgument,
               "YieldCurve::create: t_years/zero_rates size mismatch");
  }
  for (std::size_t i = 0; i + 1 < t_years.size(); ++i) {
    if (!(t_years[i] < t_years[i + 1])) {
      return Err(ErrorCode::InvalidArgument,
                 "YieldCurve::create: t_years must be strictly ascending");
    }
  }

  YieldCurve curve;
  const std::size_t n = t_years.size();
  curve.t_years_.resize(n);
  curve.log_df_.resize(n);
  curve.m_.resize(n);
  for (std::size_t i = 0; i < n; ++i) {
    curve.t_years_[i] = t_years[i];
    curve.log_df_[i] = -zero_rates[i] * t_years[i];
  }
  fritsch_carlson_compute_tangents(curve.t_years_, curve.log_df_, curve.m_);

  return Ok(std::move(curve));
}

double YieldCurve::disc(double T) const noexcept {
  if (t_years_.empty()) {
    return 1.0;
  }
  // A discount factor to the value date is 1 by definition, and zero() already
  // reports no rate for T <= 0; the two have to agree, so this is not the pillar
  // clamp's job.
  if (!(T > 0.0)) {
    return 1.0;
  }

  // Boundary extrapolation is flat in the ZERO RATE, not in the discount factor.
  // Clamping the DF flat made zero(T) = -log_df/T, which diverges like 1/T below
  // the first pillar — on the test suite's own OIS pillars (t0 = 1 day, r0 = 4.05%)
  // one hour read 97.2% and five minutes read 1108.8%, and every 0DTE/intraday path
  // through MarketEnv::rate_at inherits zero(). Past the last pillar the same clamp
  // is the symmetric error: a flat DF is a ZERO instantaneous forward rate, so
  // zero(T) decayed as rN*tN/T. Flat rate is what this class's contract already
  // reads as and what a market curve means by flat extrapolation.
  //
  // Written as log_df * (T / t_pillar) rather than exp(-r * T) so that T at the
  // pillar reproduces that pillar's discount factor BIT-exactly (the ratio is
  // exactly 1.0 there) — the knot-exactness the interior interpolant guarantees
  // must not be lost to a rate round-trip.
  const std::size_t last = t_years_.size() - 1;
  if (T <= t_years_.front()) {
    const double t0 = t_years_.front();
    // A degenerate t = 0 first pillar carries no rate to extrapolate (r0 would be
    // log_df/0); fall back to the flat-DF clamp rather than emit inf/NaN.
    if (!(t0 > 0.0)) {
      return std::exp(log_df_.front());
    }
    return std::exp(log_df_.front() * (T / t0));
  }
  if (T >= t_years_[last]) {
    const double tn = t_years_[last];
    if (!(tn > 0.0)) {
      return std::exp(log_df_[last]);
    }
    return std::exp(log_df_[last] * (T / tn));
  }

  // Binary search for the bracket.
  std::size_t lo = 0;
  std::size_t hi = last;
  while (hi - lo > 1) {
    const std::size_t mid = (lo + hi) >> 1;
    if (t_years_[mid] <= T) {
      lo = mid;
    } else {
      hi = mid;
    }
  }

  // Cubic Hermite eval. h = t[hi] - t[lo], tau = (T - t[lo]) / h.
  const double h = t_years_[hi] - t_years_[lo];
  const double tau = (T - t_years_[lo]) / h;
  const double tau2 = tau * tau;
  const double tau3 = tau2 * tau;

  // Standard Hermite basis.
  const double h00 = 2.0 * tau3 - 3.0 * tau2 + 1.0; // y_lo
  const double h10 = tau3 - 2.0 * tau2 + tau;        // m_lo
  const double h01 = -2.0 * tau3 + 3.0 * tau2;       // y_hi
  const double h11 = tau3 - tau2;                    // m_hi

  const double l = log_df_[lo] * h00 + h * m_[lo] * h10 + log_df_[hi] * h01 +
                   h * m_[hi] * h11;
  return std::exp(l);
}

double YieldCurve::zero(double T) const noexcept {
  if (T <= 0.0) {
    return 0.0;
  }
  // Outside the pillar range the rate is flat by construction (see disc), so read
  // it straight off the pillar rather than through -log(disc(T))/T: that roundtrip
  // is ill-conditioned as T -> 0 — measured 1.5e-12 of drift at five minutes — and
  // there is nothing to interpolate out there to justify paying for it.
  if (!t_years_.empty()) {
    const double t0 = t_years_.front();
    if (T <= t0 && t0 > 0.0) {
      return -log_df_.front() / t0;
    }
    const double tn = t_years_.back();
    if (T >= tn && tn > 0.0) {
      return -log_df_.back() / tn;
    }
  }
  return -std::log(disc(T)) / T;
}

// ── Dividend schedule / correction ───────────────────────────────────────

void DividendSchedule::set(std::span<const DividendEvent> evs) {
  events_.assign(evs.begin(), evs.end());
}

double forward_div_corrected(double S, double r, double T,
                             std::span<const DividendEvent> events,
                             std::int64_t expiry_ns, std::int64_t now_ts_ns) noexcept {
  if (!(S > 0.0) || !(T > 0.0) || !std::isfinite(r)) {
    return kQuietNaN;
  }
  if (events.empty()) {
    return S * std::exp(r * T);
  }

  // sum_{ex_date_ns <= expiry_ns} D_i * DF(t_i), t_i = (ex - now) / year.
  //
  // THE WINDOW IS DECIDED ON INSTANTS, NEVER ON `T`. A `t_i < 0 || t_i > T`
  // screen used to sit under the two guards below. Under Calendar365 it was
  // provably dead code: a consistent caller's `T` is
  // `(expiry_ns - now_ts_ns) / kCalendarYearNs`, and both int64->double
  // conversion and division by a positive constant are monotone, so
  // `now_ts_ns <= ex_date_ns <= expiry_ns` already implies `0 <= t_i <= T`.
  // Under a VOL-TIME `T` the two clocks disagree and that screen became a live,
  // WRONG filter: vol time compresses a weekend while the calendar `t_i` does
  // not, so a Monday ex-date off a Friday snapshot was silently dropped —
  // precisely the population a discrete schedule exists to price. It is deleted
  // rather than made conditional because the instant comparisons are
  // clock-independent and were always the real test; a conditional would leave
  // two answers to one question. `t_i` itself stays on the CALENDAR clock
  // deliberately: it discounts cash at a calendar rate, which no vol clock
  // changes. Gated by DividendForward.VolTimeShortT*.
  double pv_divs = 0.0;
  for (const DividendEvent &ev : events) {
    if (ev.ex_date_ns < now_ts_ns) {
      continue; // paid already
    }
    if (ev.ex_date_ns > expiry_ns) {
      continue; // after expiry
    }
    const double t_i =
        static_cast<double>(ev.ex_date_ns - now_ts_ns) / kCalendarYearNs;
    pv_divs += ev.amount * std::exp(-r * t_i);
  }
  return (S - pv_divs) * std::exp(r * T);
}

// ── Forward curve / HTB detector ─────────────────────────────────────────

void ForwardCurve::set(std::span<const ForwardPoint> pts) {
  pts_.assign(pts.begin(), pts.end());
}

double ForwardCurve::forward_at(std::size_t expiry_id) const noexcept {
  if (expiry_id >= pts_.size()) {
    return kQuietNaN;
  }
  return pts_[expiry_id].F;
}

HtbResult ForwardCurve::detect_htb(const HtbDetector &det) noexcept {
  std::uint16_t n_off = 0;
  for (ForwardPoint &fp : pts_) {
    if (fp.T < det.min_T_years) {
      continue;
    }
    if (!std::isfinite(fp.q_eff)) {
      continue;
    }
    if (fp.q_eff < det.htb_threshold) {
      fp.flags |= ForwardFlag::Htb;
      ++n_off;
    }
  }
  return HtbResult{n_off >= det.min_expiries, n_off};
}

// ── Curve set ─────────────────────────────────────────────────────────────

Status CurveSet::set_yield(std::span<const double> t_years,
                           std::span<const double> zero_rates) {
  ATX_TRY(yield, YieldCurve::create(t_years, zero_rates));
  return Ok();
}

} // namespace atx::vol
