#pragma once

// strike_coord — strike-coordinate conversions on a fitted surface:
// K <-> log-moneyness k = ln(K / F(T)) <-> standard moneyness K / S <-> delta under
// a NAMED convention, plus the policy every projection answer carries (maturity
// interpolation mode, extrapolation policy, delta convention, clock).
//
// FROZEN API CONTRACT (sprint 2026-10-24; see risk_types.hpp banner).
//
// ## Delta conventions (types.hpp DeltaConvention, extended in place)
//
//   Forward   European Black-76 forward delta: N(d1) call, N(d1) - 1 put, no
//             discounting. Closed-form inversion. The vendor quote convention.
//   Spot      Black spot delta e^{-qT} N(d1) (put: e^{-qT}(N(d1) - 1)) — what a
//             delta-one hedge in shares needs under the state's carry. Closed form.
//   American  dP/dS on the American mark: the cold-confirmed secant on
//             PricedSurface::delta (the convention strategy.hpp and
//             contract_projection.hpp have always used). Materially different from
//             Forward on high-carry / deep-ITM names.
//
// Every solver here accepts all three; a convention a solver cannot honour returns
// NotImplemented — never a different convention's answer under the same name.
//
// ## Provenance
//
// Every result carries RiskFlags: TenorExtrapolated when the surface serves T
// outside its fitted pillars (CurveSurface::w extrapolates SILENTLY: flat vol short
// of the front pillar, flat total variance past the last), MoneynessExtrapolated
// when the surface reports `moneyness_extrapolated(K, T)` (lane B adds that additive
// query to PricedSurface / PricedSurfaceView: the bracketing slices' IVolCurve::
// is_extrapolated(k), which only data-driven curve kinds (spline / convex dense) can
// assert; a parametric slice never flags, a mapped view answers from what its record
// stores, else false. BEST EFFORT, documented at the query). TenorExtrapPolicy::Forbid
// turns a tenor extrapolation into an OutOfRange error; moneyness is flag-only.

#include <cstddef>
#include <cstdint>
#include <span>

#include "atx/vol/api/backtest/portfolio_pricer.hpp" // SurfaceRef
#include "atx/vol/api/core/types.hpp"                // DeltaConvention, Result, Side
#include "atx/vol/api/core/vol_time.hpp"             // TimeSpec
#include "atx/vol/api/fitting/vol_curve.hpp"         // TenorInterp
#include "atx/vol/api/risk/risk_types.hpp"

namespace atx::vol {

enum class StrikeCoordKind : std::uint8_t {
  Strike = 0,                // value = K
  LogMoneyness = 1,          // value = ln(K / F(T))
  StandardMoneyness = 2,     // value = K / S  (projection.hpp CoordKind's meaning, kept)
  Delta = 3,                 // value = SIGNED delta (call > 0, put < 0) under `delta_conv`
  StandardizedMoneyness = 4, // value = z = ln(K / F(T)) / (sigma_atm(T) sqrt T): the
                             // vega-split pivot / ShapeBlend coordinate; sigma_atm is the
                             // surface's vol at k = 0 for that T
};

struct StrikeCoord {
  StrikeCoordKind kind{StrikeCoordKind::LogMoneyness};
  double value{0.0};
  DeltaConvention delta_conv{DeltaConvention::Forward}; // read iff kind == Delta

  [[nodiscard]] static StrikeCoord strike(double K) noexcept;
  [[nodiscard]] static StrikeCoord log_moneyness(double k) noexcept;
  [[nodiscard]] static StrikeCoord standard_moneyness(double m) noexcept;
  [[nodiscard]] static StrikeCoord standardized_moneyness(double z) noexcept;
  // `signed_delta`: calls in (0, 1), puts in (-1, 0).
  [[nodiscard]] static StrikeCoord delta(double signed_delta,
                                         DeltaConvention conv = DeltaConvention::Forward) noexcept;
  [[nodiscard]] bool operator==(const StrikeCoord &) const = default;
};

// Maturity interpolation for a query T between two fitted pillars: `TenorInterp`
// (TotalVariance / ShapeBlend) is declared in api/fitting/vol_curve.hpp, beside the
// CurveSurface overloads that take it, and re-exported by the include above.

// What to do when T (or k) leaves the fitted domain.
enum class TenorExtrapPolicy : std::uint8_t {
  Flag = 0,   // serve the surface's own extrapolation, set the flag (the default)
  Forbid = 1, // OutOfRange error
};

struct ProjectionPolicy {
  TenorInterp interp{TenorInterp::TotalVariance};
  TenorExtrapPolicy extrap{TenorExtrapPolicy::Flag};
  DeltaConvention delta_conv{DeltaConvention::Forward}; // used when a result reports delta
  double delta_tolerance{1.0e-7};                       // |achieved - target| for American
  TimeSpec time{};
  [[nodiscard]] bool operator==(const ProjectionPolicy &) const = default;
};

struct CoordResult {
  double T{0.0};
  double forward{0.0};
  double spot{0.0};
  double K{0.0};
  double k_log{0.0};
  double standard_moneyness{0.0};
  double standardized_moneyness{0.0}; // z
  double sigma_atm{0.0};              // the surface's vol at k = 0, T
  double delta{0.0}; // signed, under `delta_conv`
  double sigma{0.0}; // surface vol at (K, T) under the policy's interp
  DeltaConvention delta_conv{DeltaConvention::Forward};
  RiskFlags flags{0};
  std::uint16_t delta_evaluations{0}; // American solves spent (0 for closed forms)
};

// ── Closed-form helpers (pure functions; no surface) ────────────────────────────

// Black-76 forward delta / Black spot delta of (F, K, T, sigma, side); q is the
// continuous carry (spot delta only). NaN on degenerate inputs.
[[nodiscard]] double forward_delta(double F, double K, double T, double sigma,
                                   Side side) noexcept;
[[nodiscard]] double spot_delta(double F, double K, double T, double sigma, double q,
                                Side side) noexcept;
// Inverse: the strike whose Forward (resp. Spot) delta equals `signed_delta` at a
// FIXED sigma. NaN when the target is outside the side's range.
[[nodiscard]] double strike_from_forward_delta(double F, double T, double sigma,
                                               double signed_delta, Side side) noexcept;
[[nodiscard]] double strike_from_spot_delta(double F, double T, double sigma, double q,
                                            double signed_delta, Side side) noexcept;

// ── Conversions on a surface ────────────────────────────────────────────────────

// Resolve `from` at (T, side) on `surface` to every coordinate (K, k_log, standard
// moneyness, sigma) and report the delta under `policy.delta_conv`. For a Delta
// input the smile is honoured: Forward / Spot solve K with the surface's own
// sigma(K) by a bounded fixed-point / bisection (the closed form is the seed);
// American runs the cold-confirmed solve on PricedSurface::delta.
//
// @return InvalidArgument on a null surface, T <= 0, non-finite value, a delta
//         outside (0,1) / (-1,0) or with the wrong sign for the side; OutOfRange
//         under TenorExtrapPolicy::Forbid; NotFound when a delta target is unreachable
//         within the surface's domain; NotImplemented for a convention a route
//         cannot honour.
[[nodiscard]] Result<CoordResult> convert_strike_coord(const SurfaceRef &surface, double T,
                                                       Side side, const StrikeCoord &from,
                                                       const ProjectionPolicy &policy = {});

// Batched form. All spans length n; `out` is written for every row (a failed row
// carries the error in `row_status`, NaN fields and flags). Structural errors
// (length mismatch, null surface) fail the call before any row is processed.
[[nodiscard]] Status convert_strike_coord_batch(const SurfaceRef &surface,
                                                std::span<const double> T,
                                                std::span<const Side> side,
                                                std::span<const StrikeCoord> from,
                                                const ProjectionPolicy &policy,
                                                std::span<CoordResult> out,
                                                std::span<Status> row_status);

[[nodiscard]] const char *to_string(StrikeCoordKind value) noexcept;
[[nodiscard]] const char *to_string(TenorInterp value) noexcept;
[[nodiscard]] const char *to_string(TenorExtrapPolicy value) noexcept;

} // namespace atx::vol
