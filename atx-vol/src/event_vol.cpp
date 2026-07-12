#include "atx/vol/event_vol.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <utility>
#include <vector>

#include "atx/core/error.hpp"

namespace atx::vol {

using atx::core::Err;
using atx::core::ErrorCode;
using atx::core::Ok;

// ── EventSchedule ────────────────────────────────────────────────────────

EventSchedule::EventSchedule(std::vector<std::int64_t> event_ts_ns)
    : events_{std::move(event_ts_ns)} {
  std::sort(events_.begin(), events_.end());
}

std::size_t EventSchedule::count_between(std::int64_t now_ns,
                                         std::int64_t expiry_ns) const noexcept {
  if (expiry_ns < now_ns) return 0;
  // First element strictly greater than now_ns (excludes an event AT now_ns)
  // through the first element strictly greater than expiry_ns (includes an
  // event AT expiry_ns). upper_bound is monotone non-decreasing in its
  // threshold, so `hi` is guaranteed >= `lo` here (expiry_ns >= now_ns).
  const auto lo = std::upper_bound(events_.begin(), events_.end(), now_ns);
  const auto hi = std::upper_bound(events_.begin(), events_.end(), expiry_ns);
  return static_cast<std::size_t>(std::distance(lo, hi));
}

std::span<const std::int64_t> EventSchedule::events() const noexcept {
  return events_;
}

// ── censored_total_variance ──────────────────────────────────────────────

double censored_total_variance(double w_total, std::size_t n_events, double emove) noexcept {
  const double w_cen = w_total - static_cast<double>(n_events) * emove * emove;
  // NaN in (w_total or emove) makes w_cen NaN; the comparison below is then
  // false (IEEE-754: any comparison with NaN is false), so we fall through
  // to `return w_cen`, i.e. NaN out. No separate isnan check needed.
  return (w_cen < kWCenFloor) ? kWCenFloor : w_cen;
}

// ── event_recombined_vol ─────────────────────────────────────────────────

double event_recombined_vol(double atm_cen, double T, std::size_t n_events,
                            double emove) noexcept {
  if (!(T > 0.0)) {
    // Catches T <= 0 and NaN T (NaN > 0.0 is false, so !(false) is true).
    return std::numeric_limits<double>::quiet_NaN();
  }
  const double val = atm_cen * atm_cen + static_cast<double>(n_events) * emove * emove / T;
  return std::sqrt(val);
}

// ── implied_emove ────────────────────────────────────────────────────────

Result<double> implied_emove(double w1, double T1, std::size_t n1, double w2, double T2,
                             std::size_t n2) {
  if (!std::isfinite(w1) || !std::isfinite(T1) || !std::isfinite(w2) || !std::isfinite(T2)) {
    return Err(ErrorCode::InvalidArgument, "implied_emove: non-finite input");
  }
  if (!(T1 > 0.0) || !(T2 > 0.0)) {
    return Err(ErrorCode::InvalidArgument, "implied_emove: T1 and T2 must be > 0");
  }
  if (T1 == T2) {
    return Err(ErrorCode::InvalidArgument, "implied_emove: T1 == T2 (no identification)");
  }

  const double n1d = static_cast<double>(n1);
  const double n2d = static_cast<double>(n2);
  const double denom = n1d * T2 - n2d * T1;
  if (denom == 0.0) {
    return Err(ErrorCode::InvalidArgument,
               "implied_emove: n1*T2 == n2*T1 (no identification)");
  }

  const double numer = w1 * T2 - w2 * T1;
  const double e_sq = numer / denom;

  if (e_sq < -kEmoveSqClampEps) {
    return Err(ErrorCode::OutOfRange, "implied_emove: solved e^2 < 0");
  }
  if (e_sq < 0.0) {
    // Inside the [-kEmoveSqClampEps, 0) noise window -> exact 0.
    return Ok(0.0);
  }
  return Ok(std::sqrt(e_sq));
}

// ── event_aware_w ────────────────────────────────────────────────────────

double event_aware_w(double w_lo, double T_lo, std::size_t n_lo, double w_hi, double T_hi,
                     std::size_t n_hi, double T_query, std::size_t n_query,
                     double emove) noexcept {
  const double weight_hi = (T_query - T_lo) / (T_hi - T_lo);

  // Plain linear-in-w fallback: no event structure to censor/re-add. Using
  // `emove <= 0.0` (not its logical negation of `> 0.0`) so a NaN emove does
  // NOT trigger this branch when real events are present — it instead flows
  // through the censored path below and propagates to NaN, per this
  // module's NaN-in/NaN-out convention.
  if ((emove <= 0.0) || (n_lo == 0 && n_hi == 0 && n_query == 0)) {
    return w_lo + weight_hi * (w_hi - w_lo);
  }

  const double w_cen_lo = censored_total_variance(w_lo, n_lo, emove);
  const double w_cen_hi = censored_total_variance(w_hi, n_hi, emove);
  const double w_cen_query = w_cen_lo + weight_hi * (w_cen_hi - w_cen_lo);
  return w_cen_query + static_cast<double>(n_query) * emove * emove;
}

}  // namespace atx::vol
