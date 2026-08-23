#pragma once

// Yield / forward / dividend / borrow curve set — the RATES side of "curve".
//
// Named `rates_curve.hpp` (it was `curve.hpp` through S4-T21 / plan 4.4) so the
// two unrelated `curve` vocabularies in this library stop colliding on include
// lines: this header is the discount / forward / dividend term structure, while
// `vol_curve.hpp` (`IVolCurve`, `CurveSurface`, `CurveConfig`) and
// `spline_curve.hpp` are the volatility-smile curve family.
//
// Ported from the C `ats-vol` library (ats_curve.h / ats_curve.c). A vol
// surface is meaningless without a clean forward curve; this module owns:
//
//   1. Yield curve (OIS/SOFR bootstrap)  : (T -> r(T)), queried as a
//      discount factor or a continuously-compounded zero rate.
//   2. Discrete cash-dividend schedule   : (ex-date, cash amount), plus the
//      closed-form "cash-divs-as-debt" forward correction (Battig & Jarrow
//      2017): F_T = (S - sum_{t_i<=T} D_i * DF(t_i)) * e^{rT}.
//   3. Per-expiry implied forward curve  : storage + O(1) lookup by expiry
//      index, plus the Sprint-08 persistence-gated hard-to-borrow (HTB)
//      detector that sweeps q_eff across expiries.
//
// ## Scope of this port
//
// The C library also ships `ats_vol_curve_refit_forward_ex`, which refits
// F(T) from *live chain quotes* via robust ATM put-call parity (band
// expansion + Tukey-biweight location) against an `AtsVolUnderlying` /
// `AtsVolChain` from the universe layer. That refit engine, and the
// universe/chain types it depends on, are explicitly out of scope for this
// port — it needs an arena-backed order-book snapshot this module has no
// business depending on (see the port task's scope note: no SpiderRock /
// universe / arena dependencies). What *is* ported is everything downstream
// of a forward curve already having been fit: storage, lookup, the HTB
// sweep, and the pure dividend-correction formula the refit engine calls
// into.
//
// ## Yield-curve interpolation
//
// Sprint 08 Fritsch-Carlson monotone-preserving cubic Hermite interpolation
// on log(discount factor), exactly as the C computes it: per-pillar
// tangents from centred secants, clamped into the de Boor/Swartz
// monotonicity disc (alpha^2 + beta^2 <= 9), with flat extrapolation
// outside the pillar range. Reference: Fritsch & Carlson, *Monotone
// Piecewise Cubic Interpolation*, SIAM J. Numer. Anal. 17(2), 1980,
// pp. 238-246.
//
// ## Ownership / thread-safety
//
// `YieldCurve`, `ForwardCurve`, `DividendSchedule`, and `CurveSet` each own
// their pillar/point arrays via `std::vector` (Rule of Zero; no arena, no
// raw owning pointers). Every type here is "many readers OR one writer",
// matching the C contract: concurrent const queries from any number of
// threads are safe, but a mutating call (`create`, `set`, `detect_htb`)
// must not race a reader — the caller holds an exclusive fence across
// refit-cadence boundaries.

#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <vector>

#include "atx/vol/api/core/types.hpp"

namespace atx::vol {

// Quiet NaN shorthand, used throughout this header/impl for "no value yet"
// sentinels (matches the C convention of NaN-as-unset for curve fields).
inline constexpr double kQuietNaN = std::numeric_limits<double>::quiet_NaN();

// ── Yield curve ───────────────────────────────────────────────────────────

// Fritsch-Carlson monotone cubic-Hermite yield curve over (T, zero-rate)
// pillars, evaluated as log-discount-factor. Flat extrapolation outside the
// pillar range.
//
// Thread-safety: many-readers-or-one-writer. `disc`/`zero` are const and
// safe to call concurrently from any number of threads once construction
// has completed; `create` must not race a reader of the object it builds
// into (it returns a fresh value, so this is naturally satisfied).
class YieldCurve {
public:
  YieldCurve() noexcept = default;

  // Build from parallel pillar arrays. `t_years` must be non-empty and
  // strictly ascending; `zero_rates` (continuously-compounded zero rate per
  // pillar, e.g. 0.045 for 4.5%/yr) must have the same length.
  //
  // @return InvalidArgument if the pillars are empty, mismatched in
  //         length, or not strictly ascending in T. (The C leaves these as
  //         undocumented caller preconditions; this port validates them
  //         explicitly per the Result-based construction convention.)
  [[nodiscard]] static Result<YieldCurve> create(std::span<const double> t_years,
                                                  std::span<const double> zero_rates);

  // Discount factor exp(-r(T)*T) at year-fraction T. A default-constructed
  // (empty) curve always returns 1.0, matching the C no-curve contract.
  // Flat extrapolation beyond the pillar range.
  [[nodiscard]] double disc(double T) const noexcept;

  // Continuously-compounded zero rate at year-fraction T. Returns 0.0 for
  // T <= 0 (matches the C contract — there is no rate "at" the value
  // date).
  [[nodiscard]] double zero(double T) const noexcept;

  [[nodiscard]] std::size_t size() const noexcept { return t_years_.size(); }

private:
  std::vector<double> t_years_; // [n] strictly ascending
  std::vector<double> log_df_;  // [n] log(discount factor) at each pillar
  std::vector<double> m_;       // [n] Fritsch-Carlson Hermite tangents
};

// ── Discrete dividend schedule ───────────────────────────────────────────

// A single cash-dividend event: `amount` (in the underlying's cash
// currency) paid at `ex_date_ns` (epoch nanoseconds).
struct DividendEvent {
  std::int64_t ex_date_ns = 0;
  double amount = 0.0;
};

// Owning container of dividend events for one underlier (AtsVolDividendSchedule
// in the C). `forward_div_corrected` below scans linearly and does not
// require any particular order.
//
// Thread-safety: many-readers-or-one-writer; `events()` is safe to call
// concurrently once `set()` has completed and no writer is racing it.
class DividendSchedule {
public:
  DividendSchedule() noexcept = default;

  // Replace the schedule wholesale (mirrors
  // `ats_vol_curve_set_set_dividends`). An empty span clears it.
  void set(std::span<const DividendEvent> evs);

  // BORROW of the event vector this schedule owns; `set` COPIES the caller's span
  // in, so the caller's storage is never retained. INVALIDATION: `set` replaces
  // the vector wholesale and therefore invalidates every outstanding span, as do
  // destroying the schedule and assigning over it (this type is copyable as well
  // as movable — a span from one schedule never names a copy's storage).
  // Concurrent readers are safe under the many-readers-or-one-writer contract
  // above; `set` is the writer. Copy out to survive a `set` or outlive the owner.
  [[nodiscard]] std::span<const DividendEvent> events() const noexcept { return events_; }
  [[nodiscard]] std::size_t size() const noexcept { return events_.size(); }

private:
  std::vector<DividendEvent> events_;
};

// Discrete-dividend forward correction (Battig & Jarrow 2017, "cash divs as
// debt"):
//
//     F_T = (S - sum_{now<=ex_i<=expiry} D_i * e^{-r*t_i}) * e^{r*T}
//
// where t_i = (ex_date_ns - now_ts_ns) converted to years (365.25-day
// year). Events already paid (ex_date_ns < now_ts_ns) or after expiry
// (ex_date_ns > expiry_ns) are ignored.
//
// THE SUM'S WINDOW IS DECIDED ON INSTANTS ONLY, never against `T`. It used to
// carry an extra `t_i <= T` condition, which was dead under the default
// Calendar365 clock (it only restated `ex_i <= expiry`) and WRONG under a
// vol-time `T`, where a calendar `t_i` and a weekend-compressed `T` are not
// comparable. `t_i` remains on the calendar clock because it discounts CASH.
// See the rationale in rates_curve.cpp; gated by DividendForward.VolTimeShortT*.
//
// @param S          spot (> 0)
// @param r          continuously-compounded rate for [now, T]
// @param T          year-fraction to expiry (> 0)
// @param events     dividend events to sum; an empty span collapses to the
//                   pure-carry forward S*e^{rT}
// @param expiry_ns  option expiry, epoch nanoseconds
// @param now_ts_ns  valuation timestamp, epoch nanoseconds
// @return           the corrected forward, or NaN if S <= 0, T <= 0, or r
//                   is non-finite
[[nodiscard]] double forward_div_corrected(double S, double r, double T,
                                           std::span<const DividendEvent> events,
                                           std::int64_t expiry_ns,
                                           std::int64_t now_ts_ns) noexcept;

// Disagreement threshold beyond which a PCP-implied forward and a
// dividend-corrected forward are considered inconsistent
// (ForwardFlag::DivInconsistent). Ported for parity with the C constant;
// the consistency check itself lives in the (out-of-scope) live refit
// engine — callers of this module compute the two forwards and compare
// directly.
inline constexpr double kFwdMaxDivDisagreementDefault = 0.05;

// ── Forward curve ────────────────────────────────────────────────────────

// Per-expiry forward-curve quality bits (ATS_VOL_FWDFLAG_* in the C).
enum class ForwardFlag : std::uint32_t {
  None = 0,
  // T < kFwdLowTDefaultYears: 1/T amplification makes q_eff a diagnostic
  // only, not a tradeable borrow rate. (Set by the live refit engine, out
  // of scope here; the enumerator is retained so a point fitted upstream
  // can carry it through detect_htb / downstream consumers.)
  LowT = 0x01u,
  // q_eff crossed the HTB detector's threshold (set by
  // ForwardCurve::detect_htb below).
  Htb = 0x02u,
  // PCP-implied F and dividend-corrected F disagree by more than
  // kFwdMaxDivDisagreementDefault. (Set by the live refit engine, out of
  // scope here.)
  DivInconsistent = 0x04u,
};

[[nodiscard]] constexpr ForwardFlag operator|(ForwardFlag a, ForwardFlag b) noexcept {
  return static_cast<ForwardFlag>(static_cast<std::uint32_t>(a) |
                                  static_cast<std::uint32_t>(b));
}
[[nodiscard]] constexpr ForwardFlag operator&(ForwardFlag a, ForwardFlag b) noexcept {
  return static_cast<ForwardFlag>(static_cast<std::uint32_t>(a) &
                                  static_cast<std::uint32_t>(b));
}
constexpr ForwardFlag &operator|=(ForwardFlag &a, ForwardFlag b) noexcept {
  a = a | b;
  return a;
}
constexpr ForwardFlag &operator&=(ForwardFlag &a, ForwardFlag b) noexcept {
  a = a & b;
  return a;
}
[[nodiscard]] constexpr bool has_flag(ForwardFlag value, ForwardFlag flag) noexcept {
  return (value & flag) != ForwardFlag::None;
}

// Low-T threshold default (ATS_VOL_FWD_LOW_T_DEFAULT_YEARS in the C): below
// this year-fraction, q_eff is diagnostic-only.
inline constexpr double kFwdLowTDefaultYears = 0.05;

// One expiry's fitted forward point (AtsVolForwardPoint in the C). Fields
// this port's construction path (ForwardCurve::set) does not compute
// (F_smoothed, last_smoothed_ts_ns, borrow, rmse_putcall) are retained so a
// caller who fits F upstream (e.g. via the live refit engine elsewhere) has
// somewhere to put them for `detect_htb` and downstream Greeks to read.
struct ForwardPoint {
  std::int64_t expiry_ns = 0;
  double T = 0.0;                        // year-fraction at fit time
  double F = kQuietNaN;                  // implied forward
  double q_eff = kQuietNaN;              // r - log(F/S)/T
  double borrow = kQuietNaN;             // q_eff - dividend_yield_disc
  double rmse_putcall = kQuietNaN;       // PCP fit residual
  double F_smoothed = kQuietNaN;         // EWMA of F across snapshots
  std::int64_t last_smoothed_ts_ns = 0;
  ForwardFlag flags = ForwardFlag::None;
};

// Persistence-gated hard-to-borrow (HTB) detector (Sprint 08). Deliberately
// requires q_eff to cross `htb_threshold` on `min_expiries` or more
// expiries before flagging the underlier HTB — single-expiry q anomalies
// (M&A targets, dividend-timing misses) are common and should not trip it
// alone.
struct HtbDetector {
  double htb_threshold = -0.10;              // q_eff below this flags
  double min_T_years = kFwdLowTDefaultYears; // expiries below this are ignored
  std::uint16_t min_expiries = 3;            // required offending count

  [[nodiscard]] static constexpr HtbDetector default_detector() noexcept {
    return HtbDetector{};
  }
};

// Result of one ForwardCurve::detect_htb() sweep.
struct HtbResult {
  bool is_htb = false;
  std::uint16_t n_offending = 0;
};

// Owning, index-addressed container of one underlier's per-expiry forward
// points (AtsVolForwardCurve in the C). `forward_at(expiry_id)` is the
// O(1) hot-path lookup the surface evaluators use.
//
// Thread-safety: many-readers-or-one-writer; `forward_at`/`points`/`size`
// are safe to call concurrently once `set()` (or a `detect_htb()` sweep)
// has completed and no writer is racing it.
class ForwardCurve {
public:
  ForwardCurve() noexcept = default;

  // Replace the point set wholesale (direct-set / test-mock path — mirrors
  // `ats_vol_curve_set_forward`; production code fits these upstream). An
  // empty span clears the curve.
  void set(std::span<const ForwardPoint> pts);

  // F at `expiry_id` (0-based index into the last `set()` call). NaN if
  // `expiry_id` is out of range — surface evaluators only ever look up
  // listed expiries. Mirrors `ats_vol_curve_forward`.
  [[nodiscard]] double forward_at(std::size_t expiry_id) const noexcept;

  // Both overloads BORROW the point vector this curve owns — the non-const one is
  // an IN-PLACE WRITE HANDLE, not a copy: edits through it are immediately visible
  // to `forward_at`/`detect_htb`, and it is the writer in the many-readers-or-one-
  // writer contract above. INVALIDATION: `set` replaces the vector wholesale and
  // invalidates every outstanding span (`detect_htb` does not — it only rewrites
  // element flags in place), as do destroying the curve and assigning over it
  // (copyable as well as movable; a span from one curve never names a copy's
  // storage). Copy out to survive a `set` or outlive the curve.
  [[nodiscard]] std::size_t size() const noexcept { return pts_.size(); }
  [[nodiscard]] std::span<const ForwardPoint> points() const noexcept { return pts_; }
  [[nodiscard]] std::span<ForwardPoint> points() noexcept { return pts_; }

  // Sweep q_eff across all points, set ForwardFlag::Htb on every expiry
  // that crosses `det.htb_threshold` (ignoring T < det.min_T_years), and
  // report whether the offending count reached `det.min_expiries`. Mirrors
  // `ats_vol_htb_detect` (the C's negative-return "invalid input" sentinel
  // does not apply here — `det` is a const-ref, never null).
  [[nodiscard]] HtbResult
  detect_htb(const HtbDetector &det = HtbDetector::default_detector()) noexcept;

private:
  std::vector<ForwardPoint> pts_;
};

// ── Curve set bundle ─────────────────────────────────────────────────────

// All curves needed to price options on one underlier (AtsVolCurveSet in
// the C, minus the `uid` / arena fields — those are universe-layer
// concerns out of scope for this port). Owns its yield/forward/dividend
// data via RAII; Rule of Zero.
//
// Thread-safety: many-readers-or-one-writer, same contract as the member
// curves — the portfolio engine's parallel pricing pass may query
// concurrently while the caller holds an exclusive fence across refit
// boundaries.
class CurveSet {
public:
  CurveSet() noexcept = default;

  double spot = 0.0; // reference spot used to derive borrow
  YieldCurve yield;
  ForwardCurve forward;
  DividendSchedule dividends;

  // Convenience wrapper for YieldCurve::create() that assigns into `yield`
  // in place (mirrors `ats_vol_curve_set_set_yield`).
  [[nodiscard]] Status set_yield(std::span<const double> t_years,
                                 std::span<const double> zero_rates);
};

} // namespace atx::vol
