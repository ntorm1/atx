#pragma once

// Surface projection spine — the explicit, curve-aware evaluation layer above
// the hot-path `VolSurface::w` / `VolSurface::iv` evaluators.
//
// Ported from the C `ats-vol` library (ats_vol_projection.h / .c, Sprint 20
// Stage I). Callers evaluate a fitted surface at NON-listed maturities,
// strikes, standard-moneyness anchors, and quote-delta anchors with full
// provenance. Every entry takes a request struct and returns a result struct;
// nothing is implicit about coordinate kind, pricing route, time convention,
// or extrapolation policy.
//
// Canonical storage coordinate is (k_log = log(K/F(T_clock)), tau_vol) with
// w(k, tau_vol) = sigma^2 * tau_vol. Delta is solved, not stored: a 25-delta
// put is a request that resolves to (k_log, K, IV, price) on the surface.
// Inserted constant-maturity slices are derived views; they never mutate the
// market-fit `VolSurface`.
//
// ── Port scope / adaptations ─────────────────────────────────────────────
//
//   - v1 ships CLOCK time only (tau_vol == T_clock), PIECEWISE_TOTAL_VARIANCE
//     interpolation, and FORWARD delta convention — exactly as the C. Reserved
//     enum values are rejected with NotImplemented.
//   - The C's negative-integer `AtsVolStatus` channel becomes `Result<T>`:
//     ERR_INVALID -> InvalidArgument, ERR_NO_DATA -> NotFound, ERR_DOMAIN ->
//     OutOfRange, ERR_NO_CONVERGE -> Unavailable, ERR_UNSUPPORTED ->
//     NotImplemented. On an error return the provenance-flags struct is NOT
//     surfaced (Rust-style Result carries the value XOR the error); callers
//     that need the flag on a domain failure read the error code instead.
//   - PORT NOTE: `eval_ex` drops the C `AtsVolProfile` argument (atx has no
//     profile registry); the American route folds the correction cache
//     directly (american_price_cached).
//   - PORT NOTE: the optional dense no-arb butterfly/calendar sweep inside
//     `surface_insert_vol_slice` is deferred; `with_no_arb_check` is accepted
//     for API parity but always leaves `no_arb_status == 0`.
//   - The AVX2 batch4 inserted-slice IV kernel is deferred (scalar only).
//
// Thread-safety: every entry is a pure function of its surface / curve inputs
// (those carry the "many readers OR one writer" contract); no shared mutable
// state. Concurrent const calls against one surface/curve are safe.

#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>

#include "atx/vol/correction.hpp"  // CorrectionCache
#include "atx/vol/curve.hpp"       // CurveSet (also brings kQuietNaN)
#include "atx/vol/types.hpp"       // Side, Result/Status
#include "atx/vol/universe.hpp"    // Uid, kInvalidUid
#include "atx/vol/vol_surface.hpp" // VolSurface

namespace atx::vol {

// ── Provenance / resolver flag bits (mirror the C ATS_VOL_FLAG_* space) ──
//
// Bits 0..15 are owned by the Stage I evaluators; bits 16..20 are the Stage II
// resolver flags, kept in one numeric space so a leg priced through the
// portfolio-risk engine and a scalar eval agree on one provenance vocabulary.
inline constexpr std::uint32_t kFlagInterpolatedT = 1u << 0;   // T between two fitted slices
inline constexpr std::uint32_t kFlagInterpolatedK = 1u << 1;   // k bracketed within fitted region
inline constexpr std::uint32_t kFlagExtrapolatedT = 1u << 2;   // T outside fitted slice range
inline constexpr std::uint32_t kFlagExtrapolatedK = 1u << 3;   // k outside fitted region
inline constexpr std::uint32_t kFlagDeltaNotBracketed = 1u << 4;
inline constexpr std::uint32_t kFlagForwardInterp = 1u << 5;   // forward from non-pillar interp
inline constexpr std::uint32_t kFlagOutsideCore = 1u << 6;
inline constexpr std::uint32_t kFlagNoArbWarning = 1u << 7;
inline constexpr std::uint32_t kFlagPriorDominated = 1u << 8;
inline constexpr std::uint32_t kFlagVolTimeConverted = 1u << 9;
inline constexpr std::uint32_t kFlagInsertedSlice = 1u << 10;  // went through an inserted slice
inline constexpr std::uint32_t kFlagRouteAmerican = 1u << 11;
inline constexpr std::uint32_t kFlagRouteB76Only = 1u << 12;
inline constexpr std::uint32_t kFlagInvalid = 1u << 15;         // result not usable

inline constexpr std::uint32_t kResolverInsertedSliceReused = 1u << 16;
inline constexpr std::uint32_t kResolverNativeFastPath = 1u << 17;
inline constexpr std::uint32_t kResolverRouteFallbackB76 = 1u << 18;
inline constexpr std::uint32_t kResolverAmericanDeferred = 1u << 19;
inline constexpr std::uint32_t kResolverGroupNoArbWarning = 1u << 20;

// ── Coordinate / policy enums (numeric values match the C) ───────────────

// Which coordinate the caller supplies in a request `x`.
enum class CoordKind : std::uint8_t {
  Strike = 0,             // x = K, raw strike
  LogMoneyness = 1,       // x = log(K / F(T_clock))
  StandardMoneyness = 2,  // x = K / spot
  Delta = 3,              // x = signed quote-delta target
};

// Quote-delta convention. v1 ships FORWARD (d N(d1)) only.
enum class DeltaConvention : std::uint8_t {
  Forward = 0,
};

// Maturity interpolation mode. v1 ships piecewise total variance only.
enum class InterpMode : std::uint8_t {
  PiecewiseTotalVariance = 0,
};

// Out-of-bracket policy for the projection layer. Named `Proj*` to avoid a
// name clash with `ExtrapPolicy` (correction.hpp), a distinct concept.
enum class ProjExtrapPolicy : std::uint8_t {
  Forbid = 0,
  ClampForReporting = 1,
};

// Volatility-time clock. v1 ships clock time only (tau_vol == T_clock).
enum class TimeMode : std::uint8_t {
  Clock = 0,
};

// Requested pricing route (before the resolver decides what is actually
// possible given correction-cache availability). Also carried on the result as
// the actually-used route (B76Only / B76AlCache).
enum class RoutePolicy : std::uint8_t {
  B76Only = 0,
  B76AlCache = 1,
  AlCorrection = 2,  // reserved — interpolated correction across T
};

// Forward basis for surface-to-surface comparison.
enum class ForwardBasis : std::uint8_t {
  Self = 0,      // each side uses its own forward
  Source = 1,    // both sides use the source forward
  Target = 2,    // both sides use the target forward
  External = 3,  // caller-supplied forward
};

// ── Time model ───────────────────────────────────────────────────────────

// v1 leaves the reserved variance-integration knobs zero; carried now so the
// API is stable when calibrated trading time lands.
struct TimeModel {
  TimeMode mode{TimeMode::Clock};
  double overnight_weight{0.0};
  double weekend_weight{0.0};
  double event_variance_add{0.0};
};

// {mode = Clock, all reserved = 0}.
[[nodiscard]] TimeModel time_model_clock() noexcept;

// ── Volatility-time conversion ───────────────────────────────────────────

struct TauVol {
  double tau_vol{kQuietNaN};
  std::uint32_t flags{0u};
};

// Map clock time to variance-clock time. v1 supports Clock only (tau == T).
// @return InvalidArgument for non-finite / non-positive T_clock;
//         NotImplemented for a reserved time mode.
[[nodiscard]] Result<TauVol> tau_vol_from_clock(const TimeModel& tm,
                                                double T_clock);

// ── Non-pillar forward lookup ────────────────────────────────────────────

// Output of `curve_forward_T`: a forward value at a `T_clock` that need not be
// a listed expiry, with provenance. Linear-in-log(F) between the two
// bracketing forward-curve pillars.
struct ForwardLookup {
  double T{kQuietNaN};
  double F{kQuietNaN};      // interpolated / clamped forward
  double df{kQuietNaN};     // discount factor exp(-rT)
  double r{kQuietNaN};      // zero rate at T
  double q_eff{kQuietNaN};  // r - log(F/spot)/T
  std::uint16_t lo_idx{0xFFFFu};
  std::uint16_t hi_idx{0xFFFFu};
  std::uint32_t flags{0u};
};

// Forward at a (possibly non-listed) `T` with provenance. Outside the pillar
// range, Forbid returns OutOfRange; ClampForReporting returns the nearest
// pillar forward (EXTRAPOLATED_T flag set).
//
// @return InvalidArgument on non-finite/non-positive T; NotFound on an empty
//         forward curve; OutOfRange when Forbid + T outside the pillar range.
[[nodiscard]] Result<ForwardLookup> curve_forward_T(const CurveSet& curves,
                                                    double T,
                                                    ProjExtrapPolicy extrap);

// ── Inserted constant-maturity slice ─────────────────────────────────────

// A derived view over a `VolSurface` that exposes it as if a fitted slice
// existed at `T_clock`. Never mutates the underlying surface. For
// PIECEWISE_TOTAL_VARIANCE: w(k) = w_lo(k) + alpha_T * (w_hi(k) - w_lo(k)).
struct InsertedSliceHandle {
  Uid uid{kInvalidUid};
  double T_clock{kQuietNaN};
  double tau_vol{kQuietNaN};
  std::uint32_t parent_lo_idx{0u};
  std::uint32_t parent_hi_idx{0u};
  double alpha_T{kQuietNaN};
  InterpMode interp_mode{InterpMode::PiecewiseTotalVariance};
  std::int32_t exact_slice_idx{-1};  // >= 0 if T hits a fitted pillar
  double F{kQuietNaN};
  double df{kQuietNaN};
  double r{kQuietNaN};
  double q_eff{kQuietNaN};
  double logF{kQuietNaN};
  double sqrtT{kQuietNaN};
  std::uint32_t no_arb_status{0u};
  std::uint32_t flags{0u};
};

// Build a derived inserted-slice handle. `curves == nullptr` skips the
// forward/discount cache (handle still usable for IV-only evaluation).
// `with_no_arb_check` is accepted for API parity but deferred (see PORT NOTE).
//
// @return InvalidArgument on null-usable surface; NotFound on a zero-slice
//         surface; OutOfRange when Forbid + T outside the slice range;
//         NotImplemented for a reserved interp mode.
[[nodiscard]] Result<InsertedSliceHandle> surface_insert_vol_slice(
    const VolSurface& surface, const CurveSet* curves, const TimeModel& tm,
    double T_clock, InterpMode interp, ProjExtrapPolicy extrap,
    bool with_no_arb_check = false);

// Total variance w(k) against an inserted slice. NaN on a degenerate handle.
[[nodiscard]] double w_on_inserted_slice(const VolSurface& surface,
                                         const InsertedSliceHandle& handle,
                                         double k_log) noexcept;

// Implied vol sqrt(w / tau_vol) against an inserted slice. NaN if w or tau_vol
// is non-positive.
[[nodiscard]] double iv_on_inserted_slice(const VolSurface& surface,
                                          const InsertedSliceHandle& handle,
                                          double k_log) noexcept;

// Batched IV against an inserted slice. `out_iv.size()` must equal
// `k_log.size()`.
// @return InvalidArgument on a size mismatch or degenerate tau_vol.
[[nodiscard]] Status iv_on_inserted_slice_batch(
    const VolSurface& surface, const InsertedSliceHandle& handle,
    std::span<const double> k_log, std::span<double> out_iv);

// ── Coordinate conversion ────────────────────────────────────────────────

struct CoordConvertRequest {
  double T_clock{kQuietNaN};
  double x{kQuietNaN};
  CoordKind from_kind{CoordKind::LogMoneyness};
  CoordKind to_kind{CoordKind::LogMoneyness};
  Side side{Side::Call};
  DeltaConvention delta_convention{DeltaConvention::Forward};
  TimeMode time_mode{TimeMode::Clock};
  ProjExtrapPolicy extrap_policy{ProjExtrapPolicy::Forbid};
};

struct CoordConvertResult {
  double T_clock{kQuietNaN};
  double tau_vol{kQuietNaN};
  double K{kQuietNaN};
  double k_log{kQuietNaN};
  double standard_moneyness{kQuietNaN};
  double quote_delta{kQuietNaN};  // NaN unless the request involved Delta
  double F{kQuietNaN};
  double df{kQuietNaN};
  double r{kQuietNaN};
  double q_eff{kQuietNaN};
  std::uint32_t flags{0u};
};

// Zeroed request + the most common settings (from_kind == to_kind ==
// LogMoneyness, Call, Forward delta, Clock time, Forbid extrap).
[[nodiscard]] CoordConvertRequest coord_convert_request_default() noexcept;

// Resolve the requested coordinate to canonical (K, k_log) plus diagnostics.
// Quote-delta is computed only when the from-side or to-side involves Delta.
[[nodiscard]] Result<CoordConvertResult> convert_coord(
    const VolSurface& surface, const CurveSet& curves, const TimeModel& tm,
    const CoordConvertRequest& request);

// ── Extended scalar evaluation ───────────────────────────────────────────

struct EvalRequest {
  double T_clock{kQuietNaN};
  double x{kQuietNaN};
  CoordKind coord_kind{CoordKind::LogMoneyness};
  Side side{Side::Call};
  InterpMode interp_mode{InterpMode::PiecewiseTotalVariance};
  ProjExtrapPolicy extrap_policy{ProjExtrapPolicy::Forbid};
  DeltaConvention delta_convention{DeltaConvention::Forward};
  TimeMode time_mode{TimeMode::Clock};
  RoutePolicy pricing_route_policy{RoutePolicy::B76Only};
};

struct EvalResult {
  double T_clock{kQuietNaN};
  double tau_vol{kQuietNaN};
  double K{kQuietNaN};
  double k_log{kQuietNaN};
  double quote_delta{kQuietNaN};
  double iv{kQuietNaN};
  double total_variance{kQuietNaN};
  double price{kQuietNaN};  // NaN if no pricing route succeeded
  double F{kQuietNaN};
  double df{kQuietNaN};
  double r{kQuietNaN};
  double q_eff{kQuietNaN};
  RoutePolicy pricing_route{RoutePolicy::B76Only};
  std::uint32_t flags{0u};
};

// Zeroed request + the most common settings (LogMoneyness, Call, Forbid,
// Forward, Clock, B76Only).
[[nodiscard]] EvalRequest eval_request_default() noexcept;

// Extended scalar evaluation: coordinates, IV, total variance, routed price,
// forward data, route, and provenance in one struct. `correction` may be null
// when the request is B76Only.
[[nodiscard]] Result<EvalResult> surface_eval_ex(
    const VolSurface& surface, const CurveSet& curves,
    const CorrectionCache* correction, const TimeModel& tm,
    const EvalRequest& request);

// Caller-buffered batched evaluation. `out_results.size()` must equal
// `requests.size()`. No allocation on the direct-coordinate paths.
// @return InvalidArgument on a size mismatch; otherwise the last per-row error
//         (or Ok if every row succeeded).
[[nodiscard]] Status surface_eval_grid(
    const VolSurface& surface, const CurveSet& curves,
    const CorrectionCache* correction, const TimeModel& tm,
    std::span<const EvalRequest> requests, std::span<EvalResult> out_results);

// ── Delta inversion ──────────────────────────────────────────────────────

// Solve k_log such that forward_delta(F, F*exp(k_log), tau, iv, side,
// convention) == target_delta. Bracketed bisection over [-2, +2].
// `target_delta` is signed: calls positive, puts negative.
//
// @return InvalidArgument on null-usable inputs; NotImplemented for a reserved
//         convention; NotFound when the target is not bracketed; Unavailable
//         when the surface IV blows up mid-solve.
[[nodiscard]] Result<CoordConvertResult> surface_solve_k_for_delta(
    const VolSurface& surface, const CurveSet& curves, const TimeModel& tm,
    double T_clock, double target_delta, Side side, DeltaConvention convention,
    ProjExtrapPolicy extrap);

// ── Surface-to-surface comparison ────────────────────────────────────────

struct ProjectCompareInputs {
  const VolSurface* source_surface{nullptr};
  const CurveSet* source_curves{nullptr};
  const CorrectionCache* source_correction{nullptr};  // nullable

  const VolSurface* target_surface{nullptr};
  const CurveSet* target_curves{nullptr};
  const CorrectionCache* target_correction{nullptr};  // nullable

  ForwardBasis basis{ForwardBasis::Self};
  double external_F{kQuietNaN};   // used iff basis == External
  double external_df{kQuietNaN};  // used iff basis == External

  InterpMode interp_mode{InterpMode::PiecewiseTotalVariance};
  ProjExtrapPolicy extrap_policy{ProjExtrapPolicy::Forbid};
  DeltaConvention delta_convention{DeltaConvention::Forward};
  TimeMode time_mode{TimeMode::Clock};
  RoutePolicy route_policy{RoutePolicy::B76Only};
};

// One row of a comparison grid. Caller fills (T_clock, x, coord_kind, side);
// the helper populates from_source / from_target and the diffs.
struct ProjectGridRow {
  double T_clock{kQuietNaN};
  double x{kQuietNaN};
  CoordKind coord_kind{CoordKind::LogMoneyness};
  Side side{Side::Call};
  EvalResult from_source{};
  EvalResult from_target{};
  double price_diff{kQuietNaN};
  double iv_diff{kQuietNaN};
  std::uint32_t union_flags{0u};
};

// Compare two surfaces on a shared canonical grid. Fills each row's eval
// results, price/iv diffs, and the union of provenance flags.
// @return InvalidArgument on null surfaces/curves; otherwise the last per-row
//         error (or Ok if every row succeeded).
[[nodiscard]] Status surface_project_compare(const ProjectCompareInputs& in,
                                             std::span<ProjectGridRow> rows);

}  // namespace atx::vol
