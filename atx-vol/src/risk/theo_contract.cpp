#include "atx/vol/api/risk/theo_contract.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <utility>

#include "atx/core/datetime.hpp" // civil-date arithmetic, US/Eastern offset
#include "atx/core/error.hpp"
#include "atx/vol/api/core/vol_time.hpp"
#include "atx/vol/api/pricing/pricing_executor.hpp"
#include "risk/cold_route.hpp"

namespace atx::vol {

using atx::core::Err;
using atx::core::ErrorCode;
using atx::core::Ok;

namespace {

namespace civil = atx::core::time;

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
constexpr std::int64_t kNsPerHour = 3'600'000'000'000;
constexpr std::int64_t kNsPerDay = 24 * kNsPerHour;
// Instants are accepted STRICTLY inside (-2^62, 2^62) ns (1824 .. 2116), an
// exclusive bound: any expiry - valuation difference is then below 2^63 in
// magnitude (time_to_expiry_years subtracts them in int64), and a derived expiry
// (valuation + at most ~100 years ~ 3.2e18 ns) stays below 2^63.
constexpr std::int64_t kInstantBoundNs = std::int64_t{1} << 62;
constexpr double kMaxYears = 100.0;
constexpr std::int32_t kMaxCalendarDays = 36'600;
constexpr std::int32_t kMaxCalendarMonths = 1'200;
constexpr double kMaxDeltaTolerance = 1.0e-3; // the American solve's own ceiling

[[nodiscard]] bool instant_in_range(std::int64_t ns) noexcept {
  return ns > -kInstantBoundNs && ns < kInstantBoundNs;
}

[[nodiscard]] std::int64_t floor_div(std::int64_t a, std::int64_t b) noexcept {
  const std::int64_t q = a / b;
  return (a % b != 0 && ((a < 0) != (b < 0))) ? q - 1 : q;
}

// The US/Eastern civil day of an instant: a first guess at EST, then the offset
// actually in force on that ET date (modern DST rule; transitions fall on Sunday
// early mornings, which no settlement instant touches).
[[nodiscard]] std::int32_t et_day_of(std::int64_t ns) noexcept {
  const std::int64_t est_day = floor_div(ns - 5 * kNsPerHour, kNsPerDay);
  const std::int64_t offset =
      civil::eastern_utc_offset(civil::Date::from_days(est_day)).count_ns();
  return static_cast<std::int32_t>(floor_div(ns + offset, kNsPerDay));
}

// The same calendar day `months` later, clamped to the target month's end.
[[nodiscard]] std::int32_t add_months(std::int32_t day, std::int32_t months) noexcept {
  const civil::Date d = civil::Date::from_days(day);
  const std::int64_t month_index = static_cast<std::int64_t>(d.month) - 1 + months;
  const auto year = static_cast<std::int32_t>(d.year + month_index / 12);
  const auto month = static_cast<std::uint32_t>(month_index % 12) + 1u;
  const std::uint32_t dom = std::min(d.day, civil::days_in_month(year, month));
  return static_cast<std::int32_t>(civil::days_from_civil(year, month, dom));
}

[[nodiscard]] bool valid_side(Side side) noexcept {
  return side == Side::Call || side == Side::Put;
}

// A malformed template is a structural error (the Result), never a row status.
[[nodiscard]] Status validate_spec(const TheoContract &spec) {
  if (!valid_side(spec.side)) {
    return Err(ErrorCode::InvalidArgument, "theo contract: unknown side");
  }
  if (!(std::isfinite(spec.multiplier) && spec.multiplier > 0.0)) {
    return Err(ErrorCode::InvalidArgument, "theo contract: multiplier must be finite and > 0");
  }
  const StrikeCoord &k = spec.strike;
  if (!std::isfinite(k.value)) {
    return Err(ErrorCode::InvalidArgument, "theo contract: non-finite strike coordinate");
  }
  switch (k.kind) {
  case StrikeCoordKind::Strike:
  case StrikeCoordKind::StandardMoneyness:
    if (!(k.value > 0.0)) {
      return Err(ErrorCode::InvalidArgument, "theo contract: strike / K-over-S must be > 0");
    }
    return Ok();
  case StrikeCoordKind::LogMoneyness:
  case StrikeCoordKind::StandardizedMoneyness:
    return Ok();
  case StrikeCoordKind::Delta: {
    const bool in_range = spec.side == Side::Call ? (k.value > 0.0 && k.value < 1.0)
                                                  : (k.value < 0.0 && k.value > -1.0);
    if (!in_range) {
      return Err(ErrorCode::InvalidArgument,
                 "theo contract: delta must lie in (0,1) for a call, (-1,0) for a put");
    }
    switch (k.delta_conv) {
    case DeltaConvention::Forward:
    case DeltaConvention::American:
    case DeltaConvention::Spot:
      return Ok();
    }
    return Err(ErrorCode::InvalidArgument, "theo contract: unknown delta convention");
  }
  }
  return Err(ErrorCode::InvalidArgument, "theo contract: unknown strike-coordinate kind");
}

[[nodiscard]] Status validate_policy(const ProjectionPolicy &policy) {
  const bool interp_ok =
      policy.interp == TenorInterp::TotalVariance || policy.interp == TenorInterp::ShapeBlend;
  const bool extrap_ok =
      policy.extrap == TenorExtrapPolicy::Flag || policy.extrap == TenorExtrapPolicy::Forbid;
  const bool conv_ok = policy.delta_conv == DeltaConvention::Forward ||
                       policy.delta_conv == DeltaConvention::American ||
                       policy.delta_conv == DeltaConvention::Spot;
  const bool tol_ok = std::isfinite(policy.delta_tolerance) && policy.delta_tolerance > 0.0 &&
                      policy.delta_tolerance <= kMaxDeltaTolerance;
  if (!interp_ok || !extrap_ok || !conv_ok || !tol_ok) {
    return Err(ErrorCode::InvalidArgument,
               "projection policy: unknown mode or delta tolerance outside (0, 1e-3]");
  }
  return Ok();
}

// FNV-1a over (uid, K bits, expiry_ns, side), never 0.
[[nodiscard]] std::uint64_t fingerprint_of(std::uint32_t uid, double K, std::int64_t expiry_ns,
                                           Side side) noexcept {
  constexpr std::uint64_t kOffset = 14695981039346656037ULL;
  constexpr std::uint64_t kPrime = 1099511628211ULL;
  std::uint64_t h = kOffset;
  const auto mix = [&h](std::uint64_t value, int n_bytes) noexcept {
    for (int b = 0; b < n_bytes; ++b) {
      h ^= (value >> (8 * b)) & 0xFFU;
      h *= kPrime;
    }
  };
  mix(uid, 4);
  mix(std::bit_cast<std::uint64_t>(K), 8);
  mix(static_cast<std::uint64_t>(expiry_ns), 8);
  mix(static_cast<std::uint64_t>(side), 1);
  return h != 0U ? h : kPrime;
}

[[nodiscard]] AmericanGreeks nan_greeks() noexcept {
  return AmericanGreeks{kNaN, kNaN, kNaN, kNaN, kNaN, kNaN, kNaN, kNaN, kNaN};
}

// A row with the identity fields set and every number NaN ("not computed").
//
// THE uniform rule for a row whose status is not Ok: fingerprint 0 and every
// STRIKE-level number (leg.K, k_log, standard_moneyness, delta, forward_delta, iv,
// mark, greeks) NaN, even where a strike was found before a later step failed.
// Only the identity (uid, side, multiplier, valuation, expiry when resolved), the
// maturity-level T / forward when resolved, the provenance flags and the delta
// evaluations spent survive. A consumer can therefore key on `status == Ok` or on
// `fingerprint != 0` interchangeably.
[[nodiscard]] ResolvedTheo unresolved_row(const TheoContract &spec, std::int64_t valuation_ns,
                                          std::int64_t expiry_ns, TheoStatus status) noexcept {
  ResolvedTheo r;
  r.leg.uid = spec.uid;
  r.leg.K = kNaN;
  r.leg.expiry_ns = expiry_ns;
  r.leg.side = spec.side;
  r.leg.qty = 0.0;
  r.leg.multiplier = spec.multiplier;
  r.valuation_ns = valuation_ns;
  r.T = kNaN;
  r.forward = kNaN;
  r.k_log = kNaN;
  r.standard_moneyness = kNaN;
  r.delta = kNaN;
  r.forward_delta = kNaN;
  r.iv = kNaN;
  r.mark = kNaN;
  r.greeks = nan_greeks();
  r.status = status;
  return r;
}

// A strike-resolution failure that is not structural. The mapping (forward / ATM
// vol availability is checked BEFORE the conversion, so it never lands here):
//   NotFound    Delta strike: DeltaUnreachable (target outside the surface's reach).
//               Any other kind: InvalidSpec — convert_strike_coord's only NotFound
//               for a moneyness / strike coordinate is a strike that is not finite
//               and positive (e.g. log-moneyness 800 overflows F e^k): the template
//               names no strike on this state.
//   OutOfRange  MaturityUnavailable (TenorExtrapPolicy::Forbid).
//   Unavailable Delta strike: DeltaUnreachable (the solve could not evaluate /
//               converge, or the solved strike has no vol); any other kind:
//               SurfaceUnavailable (no vol at that strike).
//   otherwise   PricingError.
// (An if-chain, not a switch: ErrorCode is atx-core's open vocabulary and every
// code not named here is, deliberately, a pricing error.)
[[nodiscard]] TheoStatus market_status(ErrorCode code, const StrikeCoord &strike) noexcept {
  const bool delta_strike = strike.kind == StrikeCoordKind::Delta;
  if (code == ErrorCode::NotFound) {
    return delta_strike ? TheoStatus::DeltaUnreachable : TheoStatus::InvalidSpec;
  }
  if (code == ErrorCode::OutOfRange) {
    return TheoStatus::MaturityUnavailable;
  }
  if (code == ErrorCode::Unavailable) {
    return delta_strike ? TheoStatus::DeltaUnreachable : TheoStatus::SurfaceUnavailable;
  }
  return TheoStatus::PricingError;
}

// Resolve a validated template at a known expiry instant. The Result is an error
// only for a structural strike problem surfaced by the conversion.
[[nodiscard]] Result<ResolvedTheo> resolve_at_expiry(const TheoContract &spec,
                                                     const SurfaceRef &surface,
                                                     std::int64_t valuation_ns,
                                                     std::int64_t expiry_ns,
                                                     const ProjectionPolicy &policy) {
  ResolvedTheo out = unresolved_row(spec, valuation_ns, expiry_ns, TheoStatus::Ok);
  const Result<double> T = time_to_expiry_years(valuation_ns, expiry_ns, policy.time);
  if (!T) {
    out.status = TheoStatus::MaturityUnavailable; // e.g. VolTime outside its calendar
    return Ok(out);
  }
  out.T = *T;
  if (!(out.T > 0.0) || !std::isfinite(out.T)) {
    out.status = TheoStatus::MaturityUnavailable;
    return Ok(out);
  }
  out.forward = surface.forward_at(out.T);
  if (surface.extrapolates_tenor(out.T)) {
    out.flags |= RiskFlag::TenorExtrapolated;
    if (policy.extrap == TenorExtrapPolicy::Forbid) {
      out.status = TheoStatus::MaturityUnavailable;
      return Ok(out);
    }
  }
  // A surface that serves no forward / ATM vol at T is unavailable for EVERY strike
  // kind (a Delta template must not read this as an unreachable delta).
  const bool forward_ok = std::isfinite(out.forward) && out.forward > 0.0;
  const double sigma_atm = forward_ok ? surface.iv(out.forward, out.T, policy.interp) : kNaN;
  if (!forward_ok || !(std::isfinite(sigma_atm) && sigma_atm > 0.0)) {
    out.status = TheoStatus::SurfaceUnavailable;
    return Ok(out);
  }

  const Result<CoordResult> coord =
      convert_strike_coord(surface, out.T, spec.side, spec.strike, policy);
  if (!coord) {
    const ErrorCode code = coord.error().code();
    if (code == ErrorCode::InvalidArgument || code == ErrorCode::NotImplemented) {
      return Err(coord.error());
    }
    out.status = market_status(code, spec.strike);
    return Ok(out);
  }
  const CoordResult &c = *coord;
  out.delta_evaluations = c.delta_evaluations;
  out.flags |= c.flags;

  const Result<double> mark =
      risk_detail::cold_price(surface, c.K, out.T, spec.side, policy.interp);
  const Result<AmericanGreeks> greeks =
      risk_detail::cold_greeks_analytic(surface, c.K, out.T, spec.side, policy.interp);
  if (!mark || !greeks || !std::isfinite(*mark)) {
    // The uniform non-Ok rule (unresolved_row): the strike found above is not
    // served, so K / iv / deltas stay NaN and the fingerprint stays 0.
    out.status = TheoStatus::PricingError;
    return Ok(out);
  }
  out.leg.K = c.K;
  out.forward = c.forward;
  out.k_log = c.k_log;
  out.standard_moneyness = c.standard_moneyness;
  out.delta = c.delta;
  out.forward_delta = forward_delta(c.forward, c.K, out.T, c.sigma, spec.side);
  out.iv = c.sigma;
  out.mark = *mark;
  out.greeks = *greeks;
  out.fingerprint = fingerprint_of(spec.uid, c.K, expiry_ns, spec.side);
  return Ok(out);
}

[[nodiscard]] Status validate_structure(const TheoContract &spec, const ProjectionPolicy &policy) {
  ATX_TRY_VOID(validate_spec(spec));
  ATX_TRY_VOID(validate_policy(policy));
  return Ok();
}

} // namespace

// ── MaturitySpec factories ───────────────────────────────────────────────────

MaturitySpec MaturitySpec::years_of(double value) noexcept {
  MaturitySpec m;
  m.kind = MaturityKind::Years;
  m.years = value;
  return m;
}
MaturitySpec MaturitySpec::days(std::int32_t value) noexcept {
  MaturitySpec m;
  m.kind = MaturityKind::CalendarDays;
  m.calendar_count = value;
  return m;
}
MaturitySpec MaturitySpec::months(std::int32_t value) noexcept {
  MaturitySpec m;
  m.kind = MaturityKind::CalendarMonths;
  m.calendar_count = value;
  return m;
}
MaturitySpec MaturitySpec::absolute(std::int64_t expiry_ns) noexcept {
  MaturitySpec m;
  m.kind = MaturityKind::Absolute;
  m.expiry_ns = expiry_ns;
  return m;
}

// ── Maturity ─────────────────────────────────────────────────────────────────

Result<std::int64_t> resolve_maturity(std::int64_t valuation_ns, const MaturitySpec &spec,
                                      const TimeSpec &time) {
  if (!instant_in_range(valuation_ns)) {
    return Err(ErrorCode::InvalidArgument, "resolve_maturity: valuation instant out of range");
  }
  switch (spec.kind) {
  case MaturityKind::Years:
    if (!(std::isfinite(spec.years) && spec.years > 0.0 && spec.years <= kMaxYears)) {
      return Err(ErrorCode::InvalidArgument, "resolve_maturity: years must lie in (0, 100]");
    }
    switch (time.convention) {
    case TimeConvention::Calendar365:
      return Ok(ns_from_year_fraction(valuation_ns, spec.years));
    case TimeConvention::VolTime:
      return Err(ErrorCode::NotImplemented,
                 "resolve_maturity: Years under VolTime has no closed inverse; use "
                 "CalendarDays or Absolute");
    }
    return Err(ErrorCode::InvalidArgument, "resolve_maturity: unknown time convention");
  case MaturityKind::CalendarDays:
    if (spec.calendar_count < 0 || spec.calendar_count > kMaxCalendarDays) {
      return Err(ErrorCode::InvalidArgument, "resolve_maturity: calendar days out of range");
    }
    return Ok(settlement_instant_ns(et_day_of(valuation_ns) + spec.calendar_count,
                                    SettlementSession::Pm));
  case MaturityKind::CalendarMonths:
    if (spec.calendar_count < 0 || spec.calendar_count > kMaxCalendarMonths) {
      return Err(ErrorCode::InvalidArgument, "resolve_maturity: calendar months out of range");
    }
    return Ok(settlement_instant_ns(add_months(et_day_of(valuation_ns), spec.calendar_count),
                                    SettlementSession::Pm));
  case MaturityKind::Absolute:
    if (!instant_in_range(spec.expiry_ns)) {
      return Err(ErrorCode::InvalidArgument, "resolve_maturity: expiry instant out of range");
    }
    return Ok(spec.expiry_ns);
  }
  return Err(ErrorCode::InvalidArgument, "resolve_maturity: unknown maturity kind");
}

// ── Resolution ───────────────────────────────────────────────────────────────

Result<ResolvedTheo> resolve_theo(const TheoContract &spec, const SurfaceRef &surface,
                                  std::int64_t valuation_ns, const ProjectionPolicy &policy) {
  if (!surface.valid()) {
    return Err(ErrorCode::InvalidArgument, "resolve_theo: null surface");
  }
  ATX_TRY_VOID(validate_structure(spec, policy));
  ATX_TRY(const std::int64_t expiry_ns, resolve_maturity(valuation_ns, spec.maturity, policy.time));
  return resolve_at_expiry(spec, surface, valuation_ns, expiry_ns, policy);
}

Status resolve_theo_many(std::span<const TheoContract> specs, const SurfaceRef &surface,
                         std::int64_t valuation_ns, const ProjectionPolicy &policy,
                         std::span<ResolvedTheo> out) {
  if (!surface.valid()) {
    return Err(ErrorCode::InvalidArgument, "resolve_theo_many: null surface");
  }
  if (out.size() != specs.size()) {
    return Err(ErrorCode::InvalidArgument, "resolve_theo_many: out length != specs length");
  }
  ATX_TRY_VOID(validate_policy(policy));
  // Every row is resolved independently (no shared seeds), so a row's answer never
  // depends on what else is in the batch; each writes only its own slot, so the
  // result is identical for any worker count.
  pricing_executor().run_blocks(specs.size(), 0u, [&](std::size_t i) {
    Result<ResolvedTheo> row = resolve_theo(specs[i], surface, valuation_ns, policy);
    out[i] = row ? std::move(*row)
                 : unresolved_row(specs[i], valuation_ns, 0, TheoStatus::InvalidSpec);
  });
  return Ok();
}

Status project_series(const TheoContract &spec, std::span<const SurfaceRef> surfaces,
                      std::span<const std::int64_t> valuation_ns, const ProjectionPolicy &policy,
                      std::span<ResolvedTheo> out) {
  if (surfaces.size() != valuation_ns.size() || out.size() != surfaces.size()) {
    return Err(ErrorCode::InvalidArgument, "project_series: span length mismatch");
  }
  ATX_TRY_VOID(validate_structure(spec, policy));
  if (spec.maturity.kind == MaturityKind::Years &&
      policy.time.convention == TimeConvention::VolTime) {
    return Err(ErrorCode::NotImplemented, "project_series: Years under VolTime has no inverse");
  }
  // Per element the SAME resolution as resolve_theo: a Delta strike is re-solved
  // on each surface, a moneyness strike is held, a relative maturity is re-anchored
  // at that element's valuation (so its TTE is held) and an absolute one ages.
  pricing_executor().run_blocks(surfaces.size(), 0u, [&](std::size_t i) {
    if (!surfaces[i].valid()) {
      out[i] = unresolved_row(spec, valuation_ns[i], 0, TheoStatus::SurfaceUnavailable);
      return;
    }
    Result<ResolvedTheo> row = resolve_theo(spec, surfaces[i], valuation_ns[i], policy);
    out[i] = row ? std::move(*row)
                 : unresolved_row(spec, valuation_ns[i], 0, TheoStatus::InvalidSpec);
  });
  return Ok();
}

// ── Names ────────────────────────────────────────────────────────────────────

const char *to_string(TheoStatus value) noexcept {
  switch (value) {
  case TheoStatus::Ok:
    return "ok";
  case TheoStatus::InvalidSpec:
    return "invalid-spec";
  case TheoStatus::SurfaceUnavailable:
    return "surface-unavailable";
  case TheoStatus::MaturityUnavailable:
    return "maturity-unavailable";
  case TheoStatus::DeltaUnreachable:
    return "delta-unreachable";
  case TheoStatus::PricingError:
    return "pricing-error";
  }
  return "unknown";
}

const char *to_string(MaturityKind value) noexcept {
  switch (value) {
  case MaturityKind::Years:
    return "years";
  case MaturityKind::CalendarDays:
    return "calendar-days";
  case MaturityKind::CalendarMonths:
    return "calendar-months";
  case MaturityKind::Absolute:
    return "absolute";
  }
  return "unknown";
}

} // namespace atx::vol
