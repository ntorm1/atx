#include "atx/vol/api/pricing/derivatives.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cassert>
#include <cmath>
#include <concepts>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <numbers>
#include <optional>
#include <span>
#include <utility>

#include <vector>

#include "atx/core/error.hpp"
#include "atx/vol/api/core/types.hpp" // Task F-4: kCalendarTotalVarianceTol (calendar floor)
#include "atx/vol/api/pricing/black76.hpp"
#include "atx/vol/api/backtest/portfolio_pricer.hpp" // Task 9: SurfaceRef (borrowed handle)
#include "atx/vol/api/backtest/priced_surface.hpp" // E6: PricedSurface-native entry points
#include "atx/vol/api/fitting/surface_overlay.hpp" // Task F-8: SurfaceOverlay/StickyMode
#include "atx/vol/api/fitting/surface_parity.hpp" // SliceContext (E6 carry extraction)
#include "atx/vol/api/fitting/surface_policy.hpp" // certified_wing_half_band (FIT-C7 / C-6)
#include "atx/vol/api/fitting/vol_surface.hpp" // Tier-A calibration-grade surface container

#include "analytics/rv_lognormal.hpp" // lognormal_call, truncated_expect, norm_cdf (Tasks 4-5)
#include "fitting/counters.hpp" // Task P-6: ledger::Solve::VarSwapStripEvals
#include "fitting/legacy_surface.hpp" // Essvi/SviSurface (demoted, S4-T21)
#include "fitting/risk_surface_validation.hpp" // RiskSurfaceValidationConfig (wing-clamp band)
#include "pricing/deriv_analytic_greeks.hpp" // Task P-4 / GK-P: DerivGreekMethod::AnalyticStrip
#include "pricing/deriv_ref_bridge.hpp" // Task 9: SurfaceRef-native entry points
#include "pricing/strip_grid.hpp"

namespace atx::vol {

using atx::core::Err;
using atx::core::ErrorCode;
using atx::core::Ok;

namespace detail {

// See the declaration (derivatives.hpp, "Carr-Lee convexity refinement") for
// the from-paper derivation -- INCLUDING review fix I-1's note that
// `k_vol_naive` is a straddle PROXY for the paper's IV0 (ATM implied vol),
// not IV0 itself, with its own ~sigma^3*T/24 un-corrected residual
// (~1.667 vol bp on this task's fixture, larger than the +0.906 vol bp this
// refinement adds). Pure arithmetic -- no branches, no early exits, so every
// input (including a degenerate k_vol_naive == 0) produces a well-defined
// finite result: the denominator is >= 8 for any T > 0, so there is no
// division-by-zero to guard.
double refine_carr_lee_k_vol(double k_vol_naive, double k_var, double T) noexcept {
  const double naive_sq = k_vol_naive * k_vol_naive;
  const double numerator = T * (k_var - naive_sq);
  const double denominator = 8.0 + 2.0 * T * naive_sq;
  return k_vol_naive * (1.0 + numerator / denominator);
}

// Task P-3 test/bench seams. Declared here (external linkage, no header
// touched -- mirrors this exact file's own `risk_validation_config` forward-
// declaration precedent in deriv_greeks_test.cpp) so a test TU can force each
// optimization off and prove the ON path is bit-identical to the OFF one on
// the same inputs; defined below, past the anonymous-namespace state each one
// toggles. Production call sites never invoke either.
void set_strip_batch_disabled_for_test(bool disabled) noexcept;
void set_bump_read_cache_disabled_for_test(bool disabled) noexcept;

// Task F-7 test seam. Counts the PRICING EVALUATIONS the templated
// `deriv_greeks` itself issues: its one center `deriv_price`, every
// `bumped_pv` its bump table runs, and the standalone `var_swap_fair_strike`
// that resolves the carry-theta fixing rate. Exists because there is no other
// way to assert a repricing COUNT from a test -- `deriv_greeks`'s body is
// explicitly instantiated for a fixed surface set, so a test cannot substitute
// a counting `SurfaceT` of its own (see the instantiation list at the bottom
// of this file). `SmileGreeks.OffByDefaultCostsNothing` uses it to pin that
// `smile_greeks = false` costs exactly nothing.
//
// Deliberately NOT instrumented: the P-6 book memo path
// (`deriv_greeks_var_swap_shared`), whose whole purpose is to issue a
// different, shared number of evaluations. Counting both through one counter
// would make the number mean neither thing.
void reset_deriv_greeks_reprice_count_for_test() noexcept;
[[nodiscard]] std::uint64_t deriv_greeks_reprice_count_for_test() noexcept;

// Task F-7 oracle seams. `deriv_greeks`' own smile stencil cannot be checked
// against a reference built from `deriv_greeks` -- that is the circular oracle
// this sprint keeps rediscovering -- and a test cannot build its own view,
// because `deriv_price`'s body is explicitly instantiated for a fixed surface
// set (bottom of this file). These expose the two ingredients SEPARATELY so a
// test can re-derive the answer along a different route:
//
//   *_shifted_iv_for_test  -- what the view does to ONE surface read. The test
//     compares it against `surface.iv(k,T)` plus its own hand-written `s*k`,
//     which is genuinely independent: the surface's `iv` is public API the
//     test already calls directly, and nothing about the comparison passes
//     through the greeks path.
//   deriv_pv_skew_shifted_for_test -- one full repricing under the smile-
//     shifted view and the SAME pinned centre scheme, but with NO bump table,
//     NO stencil and NO divisor. The test builds its own two-sided difference
//     at its own bump size, so a wrong sign, a wrong divisor, a swapped
//     up/down slot or a mis-wired bump would all show up as disagreement.
//
// Monomorphic in `EssviSurface` deliberately: it is the skew-carrying fixture
// type the greeks tests already use, and keeping the seam concrete avoids
// exporting yet another template from this TU.
[[nodiscard]] double skew_shifted_iv_for_test(const EssviSurface& surface, double k_log, double T,
                                              double slope) noexcept;
[[nodiscard]] double convex_shifted_iv_for_test(const EssviSurface& surface, double k_log, double T,
                                                double curvature) noexcept;
[[nodiscard]] Result<double> deriv_pv_skew_shifted_for_test(const EssviSurface& surface,
                                                            const CurveSet& curves,
                                                            const DerivContract& contract,
                                                            const DerivConfig& cfg, double slope);

}  // namespace detail

namespace {

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

// Composite-Simpson weight. E2: delegated to the shared strip/grid convention
// (`strip_grid.hpp`) so this TU and analytics_density.cpp quadrature identically.
[[nodiscard]] double simpson_w(std::size_t i, std::size_t n) noexcept {
  return strip::simpson_weight(i, n);
}

// Quality-driven default log-strike grid (strip_quality_defaults). Node counts
// are odd so composite Simpson applies.
struct StripGrid {
  double k_min_log;
  double k_max_log;
  std::size_t n_nodes;
};

[[nodiscard]] StripGrid strip_quality_defaults(DerivQuality q) noexcept {
  switch (q) {
  case DerivQuality::Fast:
    return StripGrid{-1.0, 1.0, 97};
  case DerivQuality::High:
    return StripGrid{-2.0, 2.0, 769};
  case DerivQuality::Audit:
    return StripGrid{-3.0, 3.0, 2049};
  case DerivQuality::Standard:
    return StripGrid{-1.5, 1.5, 257};
  }
  return StripGrid{-1.5, 1.5, 257};  // unreachable; STANDARD fallback
}

// Forward at an arbitrary maturity T from the curve set, with flat (clamped)
// extrapolation outside the pillar range — the atx-curve analogue of the C's
// ats_vol_curve_forward_T under ATS_VOL_EXTRAP_CLAMP_FOR_REPORTING. Falls back
// to the reference spot when no forward curve has been set (F == S).
//
// E2 / AN-P1-2: the INTERIOR blend is now LINEAR IN log(F)
// (`strip::forward_log_blend`), matching `projection.cpp`'s `curve_forward_T`.
// It used to be linear in F, so the same forward curve read at the same T gave
// two different answers depending on which module asked. Clamped extrapolation
// and the pillar values themselves are unchanged, so this moves nothing on a
// flat or single-pillar forward curve.
//
// Precondition (matches the C, documented not checked): forward points are in
// ascending T order.
[[nodiscard]] double resolve_forward(const CurveSet& curves, double T) noexcept {
  const std::span<const ForwardPoint> pts = curves.forward.points();
  if (pts.empty()) {
    return curves.spot;
  }
  if (T <= pts.front().T) {
    return pts.front().F;
  }
  if (T >= pts.back().T) {
    return pts.back().F;
  }
  for (std::size_t i = 1; i < pts.size(); ++i) {
    if (T <= pts[i].T) {
      return strip::forward_log_blend(pts[i - 1].T, pts[i - 1].F, pts[i].T, pts[i].F, T);
    }
  }
  return pts.back().F;
}

// Discount factor at T. T <= 0 is the at-expiry shortcut: df = 1.0 by
// definition, no flag. For T > 0 a non-positive yield lookup substitutes
// df = 1.0 and stamps DfFallback so a mismarked PV is detectable (mirrors the
// C's deriv_df_at_T; here the discount comes from curves.yield).
[[nodiscard]] double deriv_df_at_T(const CurveSet& curves, double T,
                                   DerivFlags& out_flags) noexcept {
  if (T <= 0.0) {
    return 1.0;
  }
  const double df = curves.yield.disc(T);
  if (!(df > 0.0)) {
    out_flags |= DerivFlags::DfFallback;
    return 1.0;
  }
  return df;
}

// Reject any non-zero reserved config field (Sprint 28 enforcement). Returns
// true if the config is clean.
[[nodiscard]] bool reserved_fields_clean(const DerivConfig& cfg) noexcept {
  return cfg.abs_price_tol == 0.0 && cfg.rel_price_tol == 0.0 &&
         cfg.flags_request == 0u;
}

// Reject a negative vol_of_vol (Sprint 29 / Task 3 enforcement): 0 selects
// auto-calibration, > 0 is used as-is, and a negative vol-of-vol has no
// meaning. `!(x >= 0.0)` also catches NaN (comparisons with NaN are false),
// matching this file's existing NaN-safe validation idiom (see the `T > 0.0`
// guards above).
[[nodiscard]] bool vol_of_vol_valid(const DerivConfig& cfg) noexcept {
  return cfg.vol_of_vol >= 0.0;
}

// Reject a NaN wing-clamp band. 0 selects the certified default and any
// negative value disables the clamp, so NaN is the only unrepresentable input.
[[nodiscard]] bool wing_clamp_valid(const DerivConfig& cfg) noexcept {
  return !std::isnan(cfg.wing_clamp_k);
}

// Review fix I-4 (Task C-6, round 2): reject an unusable surface-carried band
// instead of silently widening trust. `resolve_wing_clamp` only CONSULTS
// `surface_band` when it is `> 0.0` (see below), so a caller-supplied NaN or
// non-positive value used to fall through UNCHECKED to the wider mode-blind
// default -- the unsafe direction: a caller whose own
// `certified_wing_half_band` arithmetic produced garbage got MORE uncertified
// trust, not less, with no diagnostic. `std::nullopt` ("no band supplied") is
// always valid; a supplied value must be finite and positive, mirroring
// `certified_wing_half_band`'s own contract (it never returns <= 0).
[[nodiscard]] bool surface_certified_wing_band_valid(std::optional<double> band) noexcept {
  return !band.has_value() || (std::isfinite(*band) && *band > 0.0);
}

// FIT-C7 / Task C-6: structural (not inheritance-based) detection of a
// surface adapter that carries its own certified wing band -- `PricedSurface`-
// native and `SurfaceRef`-native callers thread one in via
// `PricedSurfaceStripView`/`SurfaceRefStripView` (see the wrapper functions
// below); the templated legacy containers (VolSurface/EssviSurface/
// SviSurface) have no such member and fall through to `std::nullopt`, i.e.
// "no provenance", with no per-type special-casing needed. A bumped-greek
// adapter (`CachedBumpView`) also has no such member -- `deriv_greeks`
// pins the CENTER's resolved band into `cfg.wing_clamp_k` for every bump
// instead (`pin_center_scheme`), so those never need to consult this.
template <class SurfaceT>
[[nodiscard]] std::optional<double> surface_certified_wing_band(const SurfaceT& surface) noexcept {
  if constexpr (requires {
                  { surface.certified_wing_band } -> std::convertible_to<std::optional<double>>;
                }) {
    return surface.certified_wing_band;
  } else {
    return std::nullopt;
  }
}

// Task P-2 / GK-P: structural (not config-based) detection that `SurfaceT` is
// a bumped-greek adapter -- `CachedBumpView` below, the outermost wrapper
// `bumped_pv` always constructs for every evaluation `eval_bump_table` issues
// (including its own zero-shift "center" reprice and the two carry-theta
// rolled reprices). `bumped_pv` is the ONLY call site that ever builds a
// `CachedBumpView` and hands it to `deriv_price`, so this tag exactly
// identifies "this is a bump", with no reliance on a new `DerivConfig` field:
// the unwrapped surface `deriv_greeks` uses for its own CENTER quote (and
// every ordinary `deriv_price`/`price_vol_swap` caller outside
// `deriv_greeks`) never carries it. `price_vol_swap` reads this to skip its
// best-effort convexity diagnostic strip on bumped evaluations -- see that
// function's own comment. Deferred (dependent) name lookup means
// `CachedBumpView` need not be declared yet at this point in the file (same
// instantiation-time-only requirement `surface_certified_wing_band` above
// already relies on for `PricedSurfaceStripView`, declared far below).
template <class SurfaceT>
[[nodiscard]] constexpr bool is_bumped_greek_view() noexcept {
  return requires { SurfaceT::is_bumped_greek_view; };
}

// Task P-3 / PV-P4: structural detection that `SurfaceT` exposes a batched
// surface read alongside its scalar `iv(k_log, T)` -- today only
// `PricedSurfaceStripView` (wired to `PricedSurface::iv_batch`). Every other
// SurfaceT (VolSurface/EssviSurface/SviSurface, SurfaceRefStripView) has no
// such member and the variance strip's node loop falls through to its
// original, untouched per-node scalar path -- see var_swap_fair_strike below.
template <class SurfaceT>
[[nodiscard]] constexpr bool has_strip_iv_batch() noexcept {
  return requires(const SurfaceT& s, std::span<const double> x, double t, std::span<double> out) {
    s.iv_batch(x, t, out);
  };
}

// Read a boolean override from an environment variable ONCE at process load,
// mirroring `simd::cpu.cpp`'s `ATX_SIMD_ISA` seeding exactly (same rationale:
// a CI/bench leg toggles a whole-process default without touching any call
// site). "1" enables the override; anything else (including unset) leaves it
// off.
[[nodiscard]] bool env_flag_enabled(const char* name) noexcept {
#if defined(_WIN32)
  std::size_t sz = 0;
  char buf[8] = {};
  if (getenv_s(&sz, buf, sizeof(buf), name) != 0 || sz == 0) {
    return false;
  }
  return std::strcmp(buf, "1") == 0;
#else
  const char* v = std::getenv(name);
  return v != nullptr && std::strcmp(v, "1") == 0;
#endif
}

// Task P-3 test/bench seam (mirrors `simd::set_simd_isa_override`): forces
// `var_swap_fair_strike` to run its per-node scalar surface-read loop even
// when `SurfaceT` exposes a batched `iv_batch`. Production call sites never
// set this; `Strip.BatchedMatchesScalar*` (derivatives_test.cpp) uses it to
// prove the batched gather path is bit-identical to the untouched per-node
// loop on the exact same surface and resolved grid, and the paired A/B bench
// uses the `ATX_VOL_DISABLE_STRIP_BATCH` env seed to measure the SAME binary
// with and without the optimization.
std::atomic<bool> g_strip_batch_disabled{env_flag_enabled("ATX_VOL_DISABLE_STRIP_BATCH")};

[[nodiscard]] bool strip_batch_disabled_for_test() noexcept {
  return g_strip_batch_disabled.load(std::memory_order_relaxed);
}

// Task P-3 test/bench seam for the greek bump table's read-vector cache
// (`BumpReadCache`/`CachedBumpView` below): same rationale and pattern as
// `g_strip_batch_disabled` above, independent knob (the two optimizations
// are orthogonal -- see each type's own header comment).
std::atomic<bool> g_bump_read_cache_disabled{env_flag_enabled("ATX_VOL_DISABLE_BUMP_CACHE")};

[[nodiscard]] bool bump_read_cache_disabled_for_test() noexcept {
  return g_bump_read_cache_disabled.load(std::memory_order_relaxed);
}

// Task F-7 repricing counter -- see the seam's own doc in `detail` above for
// what it counts and what it deliberately does not. Always live rather than
// compiled out under a test macro: this file has no test-only build, and one
// relaxed increment per full contract repricing (a strip integration at
// minimum) is not measurable against what it counts.
std::atomic<std::uint64_t> g_deriv_greeks_reprices{0};

void count_deriv_greeks_reprice() noexcept {
  g_deriv_greeks_reprices.fetch_add(1u, std::memory_order_relaxed);
}

// Resolve the wing trust half-band: 0 -> the surface's own certified band
// when it carries one, else the mode-blind certified validation band; > 0 ->
// the caller's own band; < 0 -> 0.0 (clamp off). The <= 0 encoding of "off"
// lets every consumer test one condition (`band > 0.0`).
//
// Task F-1: `StripWingMode::Raw` forces the same 0.0 ("off") answer
// UNCONDITIONALLY, regardless of `wing_clamp_k`'s own value -- it is an
// explicit alternative spelling of `wing_clamp_k < 0`, not a second knob that
// composes with it, so a caller does not have to fight the two fields against
// each other to get the raw-everywhere reads `StripWingMode::Raw` promises.
[[nodiscard]] double resolve_wing_clamp(const DerivConfig& cfg,
                                        std::optional<double> surface_band) noexcept {
  static_assert(strip::kCertifiedWingHalfBand == RiskSurfaceValidationConfig{}.k_max,
                "the strip's default wing trust band must equal the band the fit "
                "pipeline actually validates (risk_surface_validation.hpp)");
  static_assert(certified_wing_half_band(FitQualityMode::Balanced) == strip::kCertifiedWingHalfBand,
                "surface_policy's mode-keyed certified band must agree with the strip's own "
                "mode-blind default at Balanced quality");
  if (cfg.wing_mode == StripWingMode::Raw) {
    return 0.0;
  }
  if (cfg.wing_clamp_k == 0.0) {
    // FIT-C7: a Latency/Accuracy-mode surface certifies a NARROWER/WIDER band
    // than this mode-blind default -- trusting 0.5 for a surface only
    // certified to 0.35 is exactly the defect this branch exists to close.
    if (surface_band.has_value() && *surface_band > 0.0) {
      return *surface_band;
    }
    return strip::kCertifiedWingHalfBand;
  }
  return cfg.wing_clamp_k > 0.0 ? cfg.wing_clamp_k : 0.0;
}

// Aged variance blend (decimal units). Marked total variance over the original
// contract horizon; caller guarantees n_done <= n_total (aged_total_variance_dec).
//   n_total == 0          -> fully unaged (return K_var_future).
//   n_done >= n_total > 0 -> fully aged (return rv_done).
//   n_done == 0           -> fully unaged (return K_var_future).
[[nodiscard]] double aged_total_variance_dec(double rv_done_dec,
                                             double k_var_future_dec,
                                             std::uint32_t n_done,
                                             std::uint32_t n_total) noexcept {
  if (n_total == 0u) {
    return k_var_future_dec;
  }
  if (n_done >= n_total) {
    return rv_done_dec;
  }
  if (n_done == 0u) {
    return k_var_future_dec;
  }
  const double w_done = static_cast<double>(n_done) / static_cast<double>(n_total);
  const double w_future =
      static_cast<double>(n_total - n_done) / static_cast<double>(n_total);
  return w_done * rv_done_dec + w_future * k_var_future_dec;
}

// ── Task F-3 (PV-F3 / LIT-7): THE corridor invariant, stated once ──────────
//
// A corridor variance swap differs from a plain one in exactly one way: a
// contribution counts only when the underlying's reference level is INSIDE
// [corridor_lo, corridor_hi]. That single rule has to hold in two very
// different-looking places -- the replicating strip (where the "level" is an
// option STRIKE and the rule becomes an integration domain) and the realized
// leg (where the "level" is a fixing's PREVIOUS CLOSE and the rule becomes an
// indicator on the accrual). F-2's C-1..C-4 are this sprint's standing lesson
// on what happens when such a rule is re-expressed per site: a correction
// applied where a PREDICATE says it is needed, rather than where the INVARIANT
// holds, relocates the bug instead of closing it, twice landing at a LARGER
// error than the original. So the rule is written here, once, and every site
// -- `strip_fair_value_core`'s window, `RealizedTracker::observe`,
// `inject_carry_fixing` -- routes through these three functions. Grep
// `corridor_contains` / `corridor_valid` / `corridor_log_window` before adding
// a fourth.
struct StrikeCorridor {
  double lo = 0.0;  // absolute strike; 0 == unbounded below
  double hi = 0.0;  // absolute strike; 0 == unbounded above
};

// The PREDICATE half. `0` is OVERLOADED as "unbounded on this side", so 0
// cannot also mean "invalid" -- which forces every OTHER unusable value to be
// named explicitly rather than caught by a `> 0.0` test. In particular
// `isfinite` is required, not implied: `+Inf` passes `x > 0.0` (F-2 shipped
// exactly that mistake in `gamma_anchor_valid`'s predecessor and had to fix it
// in a cleanup round) and would otherwise read as a bound at infinity, which
// is "unbounded" spelled a second, undocumented way.
[[nodiscard]] bool corridor_bound_valid(double bound) noexcept {
  return std::isfinite(bound) && bound >= 0.0;
}

// A corridor is usable iff both bounds are, and -- when BOTH are bounded --
// they are strictly ordered. `lo == hi` is rejected rather than treated as a
// degenerate zero-width corridor that accrues and replicates identically
// nothing: a contract that can only ever be worth zero is a caller error, and
// returning 0.0 for it silently is the failure mode this sprint keeps finding.
[[nodiscard]] bool corridor_valid(const StrikeCorridor& c) noexcept {
  return corridor_bound_valid(c.lo) && corridor_bound_valid(c.hi) &&
         (!(c.lo > 0.0) || !(c.hi > 0.0) || c.lo < c.hi);
}

// The MEMBERSHIP half: is `level` inside the corridor? CLOSED on both ends
// (the boundary is a measure-zero event for a continuous spot, and a closed
// test makes the unbounded encoding a strict special case of the bounded one
// rather than a separate branch). Precondition: `corridor_valid(c)` -- callers
// validate at their own boundary, which is why this stays a pure predicate.
[[nodiscard]] bool corridor_contains(double level, const StrikeCorridor& c) noexcept {
  return (!(c.lo > 0.0) || level >= c.lo) && (!(c.hi > 0.0) || level <= c.hi);
}

// The STRIP half: the same membership rule expressed in the log-forward-
// moneyness coordinate the strip integrates in, k = ln(K/F). An unbounded side
// maps to an infinite endpoint DELIBERATELY, so that intersecting it with the
// resolved span (`fmax`/`fmin`) is exactly the identity on that side -- which
// is what makes a fully-unbounded corridor bit-for-bit identical to no
// corridor at all, on the same nodes with the same weights, with no
// "is there a corridor?" branch anywhere downstream.
struct CorridorLogWindow {
  double k_lo = -std::numeric_limits<double>::infinity();
  double k_hi = std::numeric_limits<double>::infinity();
};

[[nodiscard]] CorridorLogWindow corridor_log_window(const StrikeCorridor& c,
                                                     double forward) noexcept {
  CorridorLogWindow w{};
  if (c.lo > 0.0) {
    w.k_lo = std::log(c.lo / forward);
  }
  if (c.hi > 0.0) {
    w.k_hi = std::log(c.hi / forward);
  }
  return w;
}

// Carry the strip's resolved grid onto a product quote. Every dispatch path
// that runs a strip owes its caller this, because reproducing a quote's exact
// quadrature (deriv_greeks' bump pinning) is only possible if the grid travels
// with the quote. A default-constructed `strip` carries NaN/0, which is exactly
// the "no strip ran" encoding, so this is safe to call unconditionally.
void carry_strip_grid(DerivQuote& out, const DerivQuote& strip) noexcept {
  out.strip_k_lo_used = strip.strip_k_lo_used;
  out.strip_k_hi_used = strip.strip_k_hi_used;
  out.strip_nodes_used = strip.strip_nodes_used;
  out.resolved_wing_clamp = strip.resolved_wing_clamp;
}

// Broadie-Jain (2008) / Buhler discrete-monitoring diffusion-drift addend for
// the future implied-variance leg. Per-fixing E[r_i^2] = kvar_fut*dt +
// mu^2*dt^2 (mu = r_bar - q_bar - kvar_fut/2, dt = T_resid/n_remaining);
// summing n_remaining fixings and annualizing leaves kvar_fut's own term
// exactly recovered plus this ADDITIVE leading-order piece -- NOT the
// multiplicative (1 + 1/n) the code applied before this task (~100x too
// large at index vols, and keyed off the contract's n_obs_total instead of
// the future leg's own n_remaining; see CHANGELOG.md's PV-1/PV-8 entry for
// the full magnitude comparison). The residual
// O(1/n) JUMP term (Broadie-Jain sec 4) is NOT covered here -- LIT-3: jumps
// need the FullMc engine (reserved). Magnitude: a fraction of a variance
// point for a daily-monitored (n ~ 252) contract at typical rate/carry
// differentials.
[[nodiscard]] double discrete_monitoring_addend(double kvar_fut, double T_resid,
                                                std::uint32_t n_remaining,
                                                double r_minus_q) noexcept {
  assert(n_remaining >= 1u && "discrete_monitoring_addend: n_remaining must be >= 1");
  const double mu = r_minus_q - 0.5 * kvar_fut;
  return (T_resid / static_cast<double>(n_remaining)) * mu * mu;
}

// Continuously-compounded carry differential r_bar - q_bar at T: ln(F/S)/T,
// read from the same CurveSet the strip already resolves F from. F itself is
// guaranteed > 0 here -- every call site only reaches this after its own
// var_swap_fair_strike call at the SAME T already validated
// resolve_forward(curves, T) > 0 -- so only curves.spot needs a fresh check.
//
// @return InvalidArgument if curves.spot <= 0.
[[nodiscard]] Result<double> resolve_carry_diff(const CurveSet& curves, double T) {
  if (!(curves.spot > 0.0)) {
    return Err(ErrorCode::InvalidArgument, "discrete correction needs curves.spot > 0");
  }
  const double f = resolve_forward(curves, T);
  return Ok(std::log(f / curves.spot) / T);
}

// ── Dispatch helpers (templated on the surface parametrization) ────────────

template <class SurfaceT>
[[nodiscard]] Result<DerivQuote> price_var_swap(const SurfaceT& surface,
                                                const CurveSet& curves,
                                                const DerivContract& contract,
                                                const DerivConfig& cfg) {
  const RealizedVarianceSpec& rv = contract.rv_spec;
  const double T = contract.maturity_t;

  // Discrete-monitoring FULL_MC is rejected up-front (reserved engine).
  if (cfg.discrete_correction_mode == DerivDiscreteCorrection::FullMc) {
    return Err(ErrorCode::NotImplemented, "FULL_MC discrete correction reserved");
  }

  // Future leg: only price the strip when there is residual time.
  double k_var_future_dec = 0.0;
  DerivQuote strip_quote{};
  bool strip_ran = false;
  DerivFlags flags = DerivFlags::None;

  if (rv.n_obs_total == 0u || rv.n_obs_done < rv.n_obs_total) {
    if (!(T > 0.0)) {
      return Err(ErrorCode::InvalidArgument,
                 "var swap needs T > 0 to price the future leg");
    }
    ATX_TRY(auto sq, var_swap_fair_strike(surface, curves, T, cfg));
    strip_quote = sq;
    k_var_future_dec = strip_quote.fair_strike_dec;
    strip_ran = true;

    // Discrete-monitoring correction (Broadie-Jain 2008 diffusion term,
    // leading order in 1/n_remaining): applies to the future implied-variance
    // leg only, keyed off the FUTURE leg's own remaining fixing count.
    if (cfg.discrete_correction_mode == DerivDiscreteCorrection::Diffusion1OverN &&
        rv.n_obs_total >= 1u) {
      ATX_TRY(const double r_minus_q, resolve_carry_diff(curves, T));
      const std::uint32_t n_remaining = rv.n_obs_total - rv.n_obs_done;
      k_var_future_dec += discrete_monitoring_addend(k_var_future_dec, T, n_remaining, r_minus_q);
      flags |= DerivFlags::DiscreteCorrApplied;
    }
  }

  const double total = aged_total_variance_dec(rv.rv_done_dec, k_var_future_dec,
                                               rv.n_obs_done, rv.n_obs_total);

  const double df = deriv_df_at_T(curves, T, flags);
  const double pv = df * contract.notional * (total - contract.strike_dec);

  // Provenance.
  flags |= strip_quote.flags;
  if (rv.n_obs_done > 0u) {
    flags |= DerivFlags::Aged;
  }
  if (rv.n_obs_total > 0u && rv.n_obs_done >= rv.n_obs_total) {
    flags |= DerivFlags::FullyAged;
  }

  double w_done = 0.0;
  double w_future = 1.0;
  if (rv.n_obs_total > 0u) {
    w_done = static_cast<double>(rv.n_obs_done) / static_cast<double>(rv.n_obs_total);
    w_future = 1.0 - w_done;
  }

  DerivQuote out{};
  out.fair_strike_dec = total;  // fair strike that prices the contract to PV = 0
  out.fair_strike_points = 1.0e4 * total;
  out.pv = pv;
  out.undiscounted_expectation_dec = total;
  out.uncapped_var_dec = strip_ran ? strip_quote.uncapped_var_dec : 0.0;
  out.accrued_component_dec = w_done * rv.rv_done_dec;
  out.future_component_dec = w_future * k_var_future_dec;
  out.convexity_adjustment_dec = 0.0;
  out.integration_error_est = strip_quote.integration_error_est;
  carry_strip_grid(out, strip_quote);
  out.flags = flags;
  return Ok(out);
}

// ── Task P-6 (GK-P book memo): price_var_swap split into strip/assemble ────
//
// L VarSwap rows sharing (uid, T) each pay their own full `price_var_swap`
// above, including its OWN `var_swap_fair_strike` call, even though that
// call reads nothing but (surface, curves, T, cfg) -- none of it depends on
// `contract.strike_dec` / `notional` / `rv_spec`. These two functions split
// that call out so `deriv_book.cpp`'s book-level memo can resolve it ONCE per
// (uid, T) and reuse it for every row, while the CONTRACT-SPECIFIC tail (aged
// blend, discount, strike offset) still runs once per row, unchanged.
//
// SCOPE: `cfg.discrete_correction_mode == None` only. `Diffusion1OverN` folds
// a QUADRATIC-in-K_var addend into `k_var_future_dec` keyed off the ROW's own
// `n_remaining` (`price_var_swap`'s own branch, above) -- reproducing that
// here would mean caching `resolve_carry_diff`'s result too and re-deriving
// the addend per row, which this task's scope does not cover (mirrors Task
// P-4's own `AnalyticStrip` scope exclusion for the identical reason -- see
// `DerivGreekMethod`'s doc, derivatives.hpp). A caller (`deriv_book.cpp`)
// checks `cfg.discrete_correction_mode` itself before using these; they do
// not re-check it, since `cfg` is one book-wide value for the whole memo.
//
// These intentionally DUPLICATE `price_var_swap`'s tail rather than having it
// call through them, to keep that heavily-tested function completely
// unchanged. `VarSwapMemo.*` (deriv_book_test.cpp) pins the two paths
// bit-identical against each other; keep them in sync.

// The STRIP-ONLY half: resolves K_var(T), nothing contract-specific. Safe to
// cache and reuse across every row sharing (surface, T, cfg).
template <class SurfaceT>
[[nodiscard]] Result<DerivQuote> resolve_var_swap_strip_raw(const SurfaceT& surface,
                                                             const CurveSet& curves, double T,
                                                             const DerivConfig& cfg) {
  if (!(T > 0.0)) {
    return Err(ErrorCode::InvalidArgument, "var swap needs T > 0 to price the future leg");
  }
  return var_swap_fair_strike(surface, curves, T, cfg);
}

// The CONTRACT-SPECIFIC half: `price_var_swap`'s tail, fed a precomputed (or
// absent, for a fully-aged row that never needed one) strip result instead of
// resolving it fresh. `df`/`df_fallback` are `deriv_df_at_T(curves, T, ...)`'s
// own outputs -- also uid/T-only, so the caller resolves them once per group
// too. `raw` is `std::nullopt` exactly when
// `!(rv.n_obs_total == 0u || rv.n_obs_done < rv.n_obs_total)` (fully aged) --
// the caller evaluates that same gate before deciding whether to look up a
// strip block at all.
[[nodiscard]] DerivQuote assemble_var_swap_quote(const DerivContract& contract, double df,
                                                  bool df_fallback,
                                                  const std::optional<DerivQuote>& raw) {
  const RealizedVarianceSpec& rv = contract.rv_spec;
  const bool strip_ran = raw.has_value();
  const double k_var_future_dec = strip_ran ? raw->fair_strike_dec : 0.0;

  const double total = aged_total_variance_dec(rv.rv_done_dec, k_var_future_dec, rv.n_obs_done,
                                               rv.n_obs_total);
  const double pv = df * contract.notional * (total - contract.strike_dec);

  DerivFlags flags = df_fallback ? DerivFlags::DfFallback : DerivFlags::None;
  if (strip_ran) {
    flags |= raw->flags;
  }
  if (rv.n_obs_done > 0u) {
    flags |= DerivFlags::Aged;
  }
  if (rv.n_obs_total > 0u && rv.n_obs_done >= rv.n_obs_total) {
    flags |= DerivFlags::FullyAged;
  }

  double w_done = 0.0;
  double w_future = 1.0;
  if (rv.n_obs_total > 0u) {
    w_done = static_cast<double>(rv.n_obs_done) / static_cast<double>(rv.n_obs_total);
    w_future = 1.0 - w_done;
  }

  DerivQuote out{};
  out.fair_strike_dec = total;  // fair strike that prices the contract to PV = 0
  out.fair_strike_points = 1.0e4 * total;
  out.pv = pv;
  out.undiscounted_expectation_dec = total;
  out.uncapped_var_dec = strip_ran ? raw->uncapped_var_dec : 0.0;
  out.accrued_component_dec = w_done * rv.rv_done_dec;
  out.future_component_dec = w_future * k_var_future_dec;
  out.convexity_adjustment_dec = 0.0;
  out.integration_error_est = strip_ran ? raw->integration_error_est : 0.0;
  if (strip_ran) {
    carry_strip_grid(out, *raw);
  }
  out.flags = flags;
  return out;
}

// Carr-Lee ATMF-straddle vol-strike, K_vol ~= sqrt(2 pi / T) * C_ATMF(T) /
// (F * df). Factored out of vol_swap_fair_strike (below) so it and
// resolve_vol_of_vol's auto-calibration path (also below) share ONE
// implementation — the brief that introduced vol-of-vol auto-calibration
// requires the calibrated lognormal to reproduce this number exactly, which
// is only guaranteed if both callers compute it the same way.
//
// @return InvalidArgument for T <= 0; OutOfRange if the forward/discount
//         cannot be resolved or the ATMF implied vol is non-finite/<= 0.
template <class SurfaceT>
[[nodiscard]] Result<double> carr_lee_k_vol(const SurfaceT& surface,
                                            const CurveSet& curves, double T) {
  if (!(T > 0.0)) {
    return Err(ErrorCode::InvalidArgument, "carr-lee K_vol needs T > 0");
  }

  const double F = resolve_forward(curves, T);
  const double df = curves.yield.disc(T);
  if (!(F > 0.0) || !(df > 0.0)) {
    return Err(ErrorCode::OutOfRange, "forward/discount unavailable at T");
  }

  // ATMF implied vol — log-moneyness 0.
  const double sigma_atmf = surface.iv(0.0, T);
  if (!std::isfinite(sigma_atmf) || sigma_atmf <= 0.0) {
    return Err(ErrorCode::OutOfRange, "ATMF implied vol non-finite or <= 0");
  }

  const double c_atmf = black76_price(F, F, T, sigma_atmf, df, Side::Call);
  return Ok(std::sqrt(2.0 * std::numbers::pi / T) * c_atmf / (F * df));
}

// Vol-of-vol resolution result. Internal only (Tasks 4-6's shared helper,
// same TU) — no header declaration; consumers in later tasks live in this
// same anonymous namespace.
struct VolOfVol {
  double xi;
  bool calibrated;
};

// Resolve the vol-of-vol for a contract: explicit cfg wins; otherwise calibrate
// s.t. the lognormal E[sqrt(W)] reproduces the Carr-Lee K_vol on this surface
// at this tenor: s^2 = -8 ln(k_vol_cl / sqrt(k_var)), xi = s / sqrt(T).
// Returns xi and whether it was calibrated (for the flag). k_vol_cl >= sqrt(k_var)
// (no convexity, or degenerate inputs) yields xi = 0.
//
// `calibrated` means "the auto path ran" — set true whenever cfg.vol_of_vol
// selected auto-calibration, INCLUDING the degenerate xi = 0 outcome (no
// convexity, or a k_var_future too small/non-finite to calibrate against).
// The alternative reading — calibrated only when xi > 0 — would make the flag
// answer "did we compute a value" AND "is that value non-trivial" at once,
// forcing a caller to inspect xi just to know which config path ran. Only an
// EXPLICIT cfg.vol_of_vol (the caller's own number, not ours) gets `false`.
template <class SurfaceT>
[[nodiscard]] Result<VolOfVol> resolve_vol_of_vol(const SurfaceT& surface,
                                                  const CurveSet& curves, double T,
                                                  double k_var_future,
                                                  const DerivConfig& cfg) {
  if (cfg.vol_of_vol > 0.0) {
    return Ok(VolOfVol{cfg.vol_of_vol, false});
  }

  // Auto-calibrate against THIS surface's own Carr-Lee convexity — the same
  // helper vol_swap_fair_strike uses, so a caller pricing off this xi never
  // disagrees with the plain Carr-Lee vol-swap quote by construction.
  ATX_TRY(const double k_vol_cl, carr_lee_k_vol(surface, curves, T));

  if (!std::isfinite(k_vol_cl) || !std::isfinite(k_var_future) ||
      !(k_var_future > 0.0)) {
    return Ok(VolOfVol{0.0, true});  // degenerate input; auto path still "ran"
  }

  // Task C-5: refined form calibrates against the Remark 6.4/6.5 convexity
  // refinement instead of the naive ATMF-straddle number -- k_var_future is
  // ALREADY the strip's own K_var (every caller of resolve_vol_of_vol runs
  // the strip first), so this costs nothing extra here, unlike the
  // standalone vol_swap_fair_strike entry.
  //
  // DIRECTION (verified against this function's own closed form below, NOT
  // the task brief's paraphrase -- see CHANGELOG.md's C-5 entry for the full
  // ruling): xi solves
  // s^2 = -8*ln(ratio), ratio = k_vol/sqrt(k_var_future), which is STRICTLY
  // DECREASING in k_vol for fixed k_var_future. Refined k_vol >= naive k_vol
  // under positive convexity (K_var > k_vol_naive^2), so ratio GROWS and xi
  // SHRINKS under Refined -- less inferred dispersion is needed to explain a
  // SMALLER Jensen gap once the K_vol input is less biased. Cap options are
  // vega-positive, so cap_option_value_dec shrinks too. The brief's "richer
  // caps" framing does not hold against this formula; a caller relying on
  // Refined to CHEAPEN naive's caps (not enrich them) has the right mental
  // model.
  const double k_vol =
      cfg.carr_lee_form == CarrLeeForm::Refined
          ? detail::refine_carr_lee_k_vol(k_vol_cl, k_var_future, T)
          : k_vol_cl;

  const double sqrt_k_var = std::sqrt(k_var_future);
  const double ratio = k_vol / sqrt_k_var;
  // ratio >= 1 (written as !(ratio < 1.0) so a NaN ratio also lands here, not
  // in the log() below): no convexity, or an inverted/degenerate input. s^2
  // would be <= 0, not a valid lognormal log-stdev, so xi = 0 (RV collapses
  // to its own mean).
  if (!(ratio < 1.0) || !(ratio > 0.0)) {
    return Ok(VolOfVol{0.0, true});
  }

  const double s2 = -8.0 * std::log(ratio);
  const double xi = std::sqrt(s2) / std::sqrt(T);
  return Ok(VolOfVol{xi, true});
}

// Mid-life vol-swap distribution model (Task 6): E[sqrt(a + b*W)] for the
// blended variance V = a + b*W (see file header model), W lognormal at the
// strip's own mean m (residual maturity_t, Diffusion1OverN-corrected when
// configured) and log-stdev xi*sqrt(T). sqrt(a+b*w) is SMOOTH in w (a, b >=
// 0), unlike the capped pricers' kinked payoffs above, so plain
// Gauss-Hermite (detail::lognormal_expect) is the right tool -- no
// split-domain quadrature needed.
//
// Two callers share this: the true mid-life blend (a = w_done*rv_done_dec,
// b = w_future) and, when the caller explicitly asks for RvDistributionProxy
// on an unaged contract, the degenerate a = 0 / b = 1 case -- "the
// distribution engine end to end" per the brief is this same formula with no
// accrued leg, not a separate code path.
template <class SurfaceT>
[[nodiscard]] Result<DerivQuote> price_vol_swap_distribution(
    const SurfaceT& surface, const CurveSet& curves, const DerivContract& contract,
    const DerivConfig& cfg, double a, double b) {
  const double T = contract.maturity_t;
  if (!(T > 0.0)) {
    return Err(ErrorCode::InvalidArgument, "vol swap distribution model needs T > 0");
  }
  ATX_TRY(auto sq, var_swap_fair_strike(surface, curves, T, cfg));
  const RealizedVarianceSpec& rv = contract.rv_spec;

  // xi auto-calibration resolves against the UNCORRECTED strip mean (PV-8):
  // resolve_vol_of_vol's "reproduces Carr-Lee exactly" contract must survive
  // the discrete-monitoring correction mode, so xi is resolved BEFORE the
  // correction below ever touches the mean.
  const double m_uncorrected = sq.fair_strike_dec;
  ATX_TRY(const VolOfVol vv, resolve_vol_of_vol(surface, curves, T, m_uncorrected, cfg));

  // Same Diffusion1OverN correction as price_var_swap / the capped pricers,
  // applied to the mean actually fed to the distribution model below (never
  // to xi's calibration input above) -- see price_capped_var_swap's comment
  // for why this has to match exactly.
  double m = m_uncorrected;
  DerivFlags flags = DerivFlags::None;
  if (cfg.discrete_correction_mode == DerivDiscreteCorrection::Diffusion1OverN &&
      rv.n_obs_total >= 1u) {
    ATX_TRY(const double r_minus_q, resolve_carry_diff(curves, T));
    const std::uint32_t n_remaining = rv.n_obs_total - rv.n_obs_done;
    m += discrete_monitoring_addend(m, T, n_remaining, r_minus_q);
    flags |= DerivFlags::DiscreteCorrApplied;
  }

  const double s = vv.xi * std::sqrt(T);
  const double e_sqrt_v =
      detail::lognormal_expect(m, s, [a, b](double w) { return std::sqrt(a + b * w); });

  flags |= DerivFlags::ModelProxy | sq.flags;
  if (rv.n_obs_done > 0u) {
    flags |= DerivFlags::Aged;
  }
  if (vv.calibrated) {
    flags |= DerivFlags::VolOfVolCalibrated;
  }
  const double df = deriv_df_at_T(curves, T, flags);

  DerivQuote out{};
  out.fair_strike_dec = e_sqrt_v;
  out.fair_strike_points = 1.0e2 * e_sqrt_v;
  out.pv = df * contract.notional * (e_sqrt_v - contract.strike_dec);
  out.undiscounted_expectation_dec = e_sqrt_v;
  out.uncapped_var_dec = a + b * m;
  out.accrued_component_dec = a;
  out.future_component_dec = b * m;
  out.convexity_adjustment_dec = std::sqrt(a + b * m) - e_sqrt_v;
  out.integration_error_est = sq.integration_error_est;
  carry_strip_grid(out, sq);
  out.vol_of_vol_used = vv.xi;
  out.flags = flags;
  return Ok(out);
}

// Vol-swap dispatch across the three age regimes.
//   FULLY AGED (n_done >= n_total > 0): exact, sqrt(rv_done_dec), no model --
//     unaffected by cfg.engine (an explicit RvDistributionProxy here keeps
//     this same branch; the model has nothing left to add).
//   UNAGED (n_done == 0): Carr-Lee (engine Auto or explicit VolCarrLee;
//     Marquee pins this at inception) UNLESS the caller explicitly asks for
//     RvDistributionProxy, which runs the distribution model end to end
//     (a = 0, b = 1 -- see price_vol_swap_distribution above).
//   MID-LIFE (0 < n_done < n_total): always the distribution model (Auto or
//     RvDistributionProxy). Carr-Lee has no way to blend an already-accrued
//     leg, so an explicit VolCarrLee here is InvalidArgument rather than
//     silently pricing the wrong thing.
template <class SurfaceT>
[[nodiscard]] Result<DerivQuote> price_vol_swap(const SurfaceT& surface,
                                                const CurveSet& curves,
                                                const DerivContract& contract,
                                                const DerivConfig& cfg) {
  const RealizedVarianceSpec& rv = contract.rv_spec;
  const double T = contract.maturity_t;
  const bool fully_aged =
      (rv.n_obs_total > 0u && rv.n_obs_done >= rv.n_obs_total);
  const bool unaged = (rv.n_obs_done == 0u);

  // Discrete-monitoring FULL_MC is rejected up-front (reserved engine),
  // mirroring price_var_swap and the capped pricers -- even though the
  // fully-aged and Carr-Lee-unaged paths below never apply the correction, a
  // reserved-engine request fails the same way regardless of aging state.
  if (cfg.discrete_correction_mode == DerivDiscreteCorrection::FullMc) {
    return Err(ErrorCode::NotImplemented, "FULL_MC discrete correction reserved");
  }

  if (!fully_aged && !unaged && cfg.engine == DerivEngine::VolCarrLee) {
    return Err(ErrorCode::InvalidArgument,
               "Carr-Lee cannot blend accrued realized variance");
  }

  if (fully_aged) {
    const double r1 = std::sqrt(std::fmax(rv.rv_done_dec, 0.0));
    DerivFlags flags = DerivFlags::Aged | DerivFlags::FullyAged;
    const double df = deriv_df_at_T(curves, T, flags);
    const double pv = df * contract.notional * (r1 - contract.strike_dec);

    DerivQuote out{};
    out.fair_strike_dec = r1;
    out.fair_strike_points = 1.0e2 * r1;
    out.pv = pv;
    out.undiscounted_expectation_dec = r1;
    out.accrued_component_dec = r1;
    out.future_component_dec = 0.0;
    out.convexity_adjustment_dec = 0.0;
    out.integration_error_est = kNaN;  // NaN = not estimated
    out.flags = flags;
    return Ok(out);
  }

  if (unaged && cfg.engine != DerivEngine::RvDistributionProxy) {
    // Unaged vol-swap pricing (Carr-Lee).
    if (!(T > 0.0)) {
      return Err(ErrorCode::InvalidArgument, "unaged vol swap needs T > 0");
    }

    ATX_TRY(auto vol_q, vol_swap_fair_strike(surface, curves, T, cfg));

    const double k_vol = vol_q.fair_strike_dec;
    DerivFlags flags = DerivFlags::VolCarrLee;
    // Review fix I-2 (C-5): propagate the strip's own provenance flags
    // (StripTruncatedLeft/Right, WingClamped, LowT, InteriorBadNodes) onto
    // this public dispatch quote. Under Naive this is a no-op --
    // vol_q.flags == VolCarrLee exactly, so the OR changes nothing -- but
    // under Refined, vol_swap_fair_strike now runs a strip that FEEDS the
    // price, and its provenance previously vanished at this boundary: a
    // caller gating on StripTruncated*/WingClamped/LowT (the pattern this
    // file establishes everywhere else a strip runs) would pass a quote
    // built on a truncated or wing-clamped strip with no trace of it.
    flags |= vol_q.flags;
    const double df = deriv_df_at_T(curves, T, flags);
    const double pv = df * contract.notional * (k_vol - contract.strike_dec);

    DerivQuote out{};
    out.fair_strike_dec = k_vol;
    out.fair_strike_points = 1.0e2 * k_vol;
    out.pv = pv;
    out.undiscounted_expectation_dec = k_vol;
    out.integration_error_est = kNaN;  // carry-forward fix: NaN, not 0.0, unless the strip below runs

    // Best-effort variance strip to populate the convexity diagnostic; do not
    // fail the price call if the strip is unavailable.
    //
    // Task P-2 / GK-P: SKIPPED on every bumped/rolled evaluation `deriv_greeks`
    // issues through `bumped_pv` (`is_bumped_greek_view` detects this
    // structurally -- see its own comment). Those stencils read only PV --
    // `bumped_pv` returns `q.pv` and nothing else -- so this diagnostic's own
    // fields (`uncapped_var_dec`, `convexity_adjustment_dec`,
    // `integration_error_est`, the carried grid, the OR'd-in flags) are never
    // read off a bumped quote; paying a second full strip integration per
    // bump was pure waste, up to 14 of them on a Standard-tier unaged VolSwap
    // `deriv_greeks` call. The CENTER quote is unaffected: `deriv_greeks`
    // computes it via the unwrapped surface (never a `CachedBumpView`), and so
    // does every ordinary `price_vol_swap`/`deriv_price` caller outside
    // `deriv_greeks`.
    if constexpr (!is_bumped_greek_view<SurfaceT>()) {
      if (const Result<DerivQuote> strip = var_swap_fair_strike(surface, curves, T, cfg);
          strip.has_value()) {
        out.uncapped_var_dec = strip->uncapped_var_dec;
        out.convexity_adjustment_dec =
            std::sqrt(std::fmax(strip->uncapped_var_dec, 0.0)) - k_vol;
        out.integration_error_est = strip->integration_error_est;
        carry_strip_grid(out, *strip);
        // Aggregate review fix (I-1): this is the v1.1-default (Naive) path's
        // ONLY strip -- C-5's I-2 fix ORs the Refined strip's flags in at :624
        // above, but under Naive that strip never runs, so this one's
        // provenance (StripTruncatedLeft/Right, WingClamped, LowT,
        // InteriorBadNodes) was silently dropped even though its NUMBERS
        // (uncapped_var_dec, integration_error_est, the grid fields just
        // carried above, including resolved_wing_clamp) were served -- a quote
        // that could contradict itself (e.g. a nonzero resolved_wing_clamp with
        // WingClamped absent from flags). `out.flags = flags` below still wins,
        // so OR it into `flags` here rather than `out.flags` directly.
        flags |= strip->flags;
      }
    }
    out.accrued_component_dec = 0.0;
    out.future_component_dec = k_vol;
    out.flags = flags;
    return Ok(out);
  }

  // Distribution model (Task 6): the true mid-life blend, or an explicit
  // RvDistributionProxy pricing an unaged contract end to end (a = 0, b = 1).
  double w_done = 0.0;
  double w_future = 1.0;
  if (rv.n_obs_total > 0u) {
    w_done = static_cast<double>(rv.n_obs_done) / static_cast<double>(rv.n_obs_total);
    w_future = 1.0 - w_done;
  }
  return price_vol_swap_distribution(surface, curves, contract, cfg,
                                     w_done * rv.rv_done_dec, w_future);
}

// Assembles a DerivQuote for price_capped_var_swap's three exit paths
// (pinned, fully-aged deterministic, model-based blend) so the shared
// bookkeeping -- PV, points conversion, component breakdown -- lives in one
// place instead of being repeated at each exit.
[[nodiscard]] DerivQuote capped_var_swap_quote(double expectation_dec, double accrued_dec,
                                               double future_dec, double cap_option_dec,
                                               double df, const DerivContract& contract,
                                               DerivFlags flags) noexcept {
  DerivQuote out{};
  out.fair_strike_dec = expectation_dec;  // E[min(V,C)]: the strike pricing to PV = 0
  out.fair_strike_points = 1.0e4 * expectation_dec;
  out.pv = df * contract.notional * (expectation_dec - contract.strike_dec);
  out.undiscounted_expectation_dec = expectation_dec;
  out.accrued_component_dec = accrued_dec;
  out.future_component_dec = future_dec;
  out.cap_option_value_dec = cap_option_dec;
  out.flags = flags;
  return out;
}

// Capped variance swap: E[min(V,C)] for the blended variance V = a + b*W (see
// file header for the model). Mirrors price_var_swap's structure -- blend
// weights, strip for the future leg, aged-provenance flags -- but the PIN
// check has to run BEFORE the strip and BEFORE any T > 0 requirement: a
// contract whose accrued leg alone already reached the cap is a valid quote
// request at expiry (T == 0), and must not pay for (or fail on) a strip it
// does not need.
//
// Exit paths, in order:
//   1. PIN: a = w_done*rv_done_dec >= cap_dec -> deterministic, no strip.
//   2. FULLY AGED (not pinned): min(V,C) = rv_done_dec exactly (w_future ==
//      0, no future leg to model).
//   3. Otherwise: strip for K_var_future, resolve vol-of-vol, and the
//      displaced-lognormal closed form via detail::lognormal_call.
//
// Precondition (enforced by deriv_price before this is ever called):
// contract.cap_dec > 0.
template <class SurfaceT>
[[nodiscard]] Result<DerivQuote> price_capped_var_swap(const SurfaceT& surface,
                                                        const CurveSet& curves,
                                                        const DerivContract& contract,
                                                        const DerivConfig& cfg) {
  assert(contract.cap_dec > 0.0 && "capped var swap: cap_dec validated by the caller");
  const RealizedVarianceSpec& rv = contract.rv_spec;
  const double T = contract.maturity_t;
  const double cap = contract.cap_dec;

  // Discrete-monitoring FULL_MC is rejected up-front (reserved engine),
  // mirroring price_var_swap's guard.
  if (cfg.discrete_correction_mode == DerivDiscreteCorrection::FullMc) {
    return Err(ErrorCode::NotImplemented, "FULL_MC discrete correction reserved");
  }

  double w_done = 0.0;
  double w_future = 1.0;
  if (rv.n_obs_total > 0u) {
    w_done = static_cast<double>(rv.n_obs_done) / static_cast<double>(rv.n_obs_total);
    w_future = 1.0 - w_done;
  }
  const double accrued = w_done * rv.rv_done_dec;

  DerivFlags flags = DerivFlags::None;
  if (rv.n_obs_done > 0u) {
    flags |= DerivFlags::Aged;
  }
  if (rv.n_obs_total > 0u && rv.n_obs_done >= rv.n_obs_total) {
    flags |= DerivFlags::FullyAged;
  }

  if (accrued >= cap) {
    flags |= DerivFlags::CapPinned | DerivFlags::CapApplied;
    const double df = deriv_df_at_T(curves, T, flags);
    return Ok(capped_var_swap_quote(cap, accrued, 0.0, 0.0, df, contract, flags));
  }

  if (has_flag(flags, DerivFlags::FullyAged)) {
    // rv_done_dec < cap here (the pin check above already handled >= cap):
    // min(V,C) collapses to the realized leg, no model needed.
    const double df = deriv_df_at_T(curves, T, flags);
    return Ok(capped_var_swap_quote(accrued, accrued, 0.0, 0.0, df, contract, flags));
  }

  if (!(T > 0.0)) {
    return Err(ErrorCode::InvalidArgument,
               "capped var swap needs T > 0 to price the future leg");
  }
  ATX_TRY(auto sq, var_swap_fair_strike(surface, curves, T, cfg));

  // xi auto-calibration resolves against the UNCORRECTED strip mean (PV-8):
  // resolve_vol_of_vol's "reproduces Carr-Lee exactly" contract must survive
  // the discrete-monitoring correction mode. The corrected mean below is
  // still both the blend's future leg AND what the lognormal model actually
  // integrates, so a plain VarSwap and a CappedVarSwap on the same underlying
  // still see the same future variance leg under this config (CapParityHolds)
  // -- it is only xi's calibration input that must stay uncorrected.
  const double m_uncorrected = sq.fair_strike_dec;
  ATX_TRY(const VolOfVol vv, resolve_vol_of_vol(surface, curves, T, m_uncorrected, cfg));

  // Discrete-monitoring correction (Broadie-Jain 2008, leading order in
  // 1/n_remaining) -- same formula and flag as price_var_swap's, applied to
  // the mean fed to the blend and the lognormal model below.
  double m = m_uncorrected;
  if (cfg.discrete_correction_mode == DerivDiscreteCorrection::Diffusion1OverN &&
      rv.n_obs_total >= 1u) {
    ATX_TRY(const double r_minus_q, resolve_carry_diff(curves, T));
    const std::uint32_t n_remaining = rv.n_obs_total - rv.n_obs_done;
    m += discrete_monitoring_addend(m, T, n_remaining, r_minus_q);
    flags |= DerivFlags::DiscreteCorrApplied;
  }

  const double s = vv.xi * std::sqrt(T);
  const double k_c = (cap - accrued) / w_future;  // w_future > 0: not fully aged (checked above)
  const double cap_option = w_future * detail::lognormal_call(m, s, k_c);
  const double expectation = (accrued + w_future * m) - cap_option;

  flags |= DerivFlags::ModelProxy | DerivFlags::CapApplied | sq.flags;
  if (vv.calibrated) {
    flags |= DerivFlags::VolOfVolCalibrated;
  }
  const double df = deriv_df_at_T(curves, T, flags);

  DerivQuote out =
      capped_var_swap_quote(expectation, accrued, w_future * m, cap_option, df, contract, flags);
  out.uncapped_var_dec = sq.uncapped_var_dec;
  out.integration_error_est = sq.integration_error_est;
  carry_strip_grid(out, sq);
  out.vol_of_vol_used = vv.xi;
  return Ok(out);
}

// Assembles a DerivQuote for price_variance_option's three exit paths, so the
// premium -> PV -> component bookkeeping lives in one place. Same role as
// capped_var_swap_quote above, with the ONE structural difference these kinds
// have: `pv` does NOT subtract `contract.strike_dec`, because K is already
// inside the payoff whose expectation `premium_dec` is. Getting that wrong
// would subtract the strike twice, and would do it QUIETLY -- the sign and
// magnitude stay plausible. See `DerivKind::VarianceCall`'s header doc for the
// full field contract.
[[nodiscard]] DerivQuote variance_option_quote(double premium_dec, double accrued_dec,
                                               double future_dec, double df,
                                               const DerivContract& contract,
                                               DerivFlags flags) noexcept {
  DerivQuote out{};
  out.fair_strike_dec = premium_dec;  // the PREMIUM: no strike prices an option to PV = 0
  out.fair_strike_points = 1.0e4 * premium_dec;  // variance points, the VarSwap scale
  out.pv = df * contract.notional * premium_dec;
  out.undiscounted_expectation_dec = premium_dec;
  out.accrued_component_dec = accrued_dec;
  out.future_component_dec = future_dec;
  out.flags = flags;
  return out;
}

// European option on realized variance (Task F-5, PV-F5 / LIT-5): E[(V-K)+]
// for a call and E[(K-V)+] for a put, over the SAME blended variance
// V = a + b*W that `price_capped_var_swap` above prices -- a = w_done*
// rv_done_dec, b = w_future, W lognormal with mean K_var_future and log-stdev
// xi*sqrt(T). One function serves both kinds: they differ only in which closed
// form the shared (a, b, m, s, K) resolution is handed to.
//
// Exit paths, in order:
//   1. FULLY AGED (b == 0): V == a exactly, so the payoff is its own intrinsic
//      value. Deterministic, no strip, valid at T == 0.
//   2. PUT PIN (a >= K, not fully aged): V >= a >= K makes (K-V)+ identically
//      0. Like the capped kinds' cap pin, this must run BEFORE the strip and
//      before any T > 0 requirement -- a put already accrued past its strike is
//      a legitimate quote request at expiry and must not fail on, or pay for, a
//      strip it does not need.
//   3. Otherwise: strip for K_var_future, resolve vol-of-vol, closed form.
//
// A CALL HAS NO MIRROR OF EXIT 2, and the asymmetry is the easiest thing here
// to get backwards. a >= K makes exercise CERTAIN, but certain exercise is not
// a deterministic value: the payoff is still a + b*W - K, whose expectation
// needs the strip's m. `lognormal_call` already returns m - k for k <= 0, so
// that regime is the ordinary model path with a negative effective strike and
// needs no branch at all.
//
// The discrete-monitoring correction, the xi-calibrates-against-the-
// UNCORRECTED-mean rule (PV-8), and the strip/flag bookkeeping are all
// `price_capped_var_swap`'s, deliberately identical -- a variance call struck
// at C and a capped var swap capped at C are the same expectation
// (`VarOption.CappedSwapIdentity`), which can only hold if both pricers resolve
// the future leg the same way.
template <class SurfaceT>
[[nodiscard]] Result<DerivQuote> price_variance_option(const SurfaceT& surface,
                                                        const CurveSet& curves,
                                                        const DerivContract& contract,
                                                        const DerivConfig& cfg) {
  assert((contract.kind == DerivKind::VarianceCall ||
          contract.kind == DerivKind::VariancePut) &&
         "variance option: kind routed by deriv_price");
  const bool is_call = contract.kind == DerivKind::VarianceCall;
  const RealizedVarianceSpec& rv = contract.rv_spec;
  const double T = contract.maturity_t;
  const double k_opt = contract.strike_dec;  // the OPTION strike, in decimal variance

  if (cfg.discrete_correction_mode == DerivDiscreteCorrection::FullMc) {
    return Err(ErrorCode::NotImplemented, "FULL_MC discrete correction reserved");
  }

  double w_done = 0.0;
  double w_future = 1.0;
  if (rv.n_obs_total > 0u) {
    w_done = static_cast<double>(rv.n_obs_done) / static_cast<double>(rv.n_obs_total);
    w_future = 1.0 - w_done;
  }
  const double a = w_done * rv.rv_done_dec;

  DerivFlags flags = DerivFlags::None;
  if (rv.n_obs_done > 0u) {
    flags |= DerivFlags::Aged;
  }
  if (rv.n_obs_total > 0u && rv.n_obs_done >= rv.n_obs_total) {
    flags |= DerivFlags::FullyAged;
  }

  if (has_flag(flags, DerivFlags::FullyAged)) {
    const double payoff = is_call ? std::fmax(a - k_opt, 0.0) : std::fmax(k_opt - a, 0.0);
    const double df = deriv_df_at_T(curves, T, flags);
    return Ok(variance_option_quote(payoff, a, 0.0, df, contract, flags));
  }

  if (!is_call && a >= k_opt) {
    // Fix round 1: stamp the pin. Without a flag, "dead by accrual" and "cheap
    // by model" both quote ~0 and are separable only by inference across three
    // other fields. The flag also marks the ONE path where this file's usual
    // E[V] == accrued_component_dec + future_component_dec reading fails: no
    // strip ran, so the future leg is 0 in the NOT-COMPUTED sense while the
    // true E[V] is a + b*m with b > 0.
    flags |= DerivFlags::OptionPinned;
    const double df = deriv_df_at_T(curves, T, flags);
    return Ok(variance_option_quote(0.0, a, 0.0, df, contract, flags));
  }

  if (!(T > 0.0)) {
    return Err(ErrorCode::InvalidArgument,
               "variance option needs T > 0 to price the future leg");
  }
  ATX_TRY(auto sq, var_swap_fair_strike(surface, curves, T, cfg));

  // xi auto-calibration resolves against the UNCORRECTED strip mean (PV-8),
  // for the reason price_capped_var_swap's own copy of this comment gives.
  const double m_uncorrected = sq.fair_strike_dec;
  ATX_TRY(const VolOfVol vv, resolve_vol_of_vol(surface, curves, T, m_uncorrected, cfg));

  double m = m_uncorrected;
  if (cfg.discrete_correction_mode == DerivDiscreteCorrection::Diffusion1OverN &&
      rv.n_obs_total >= 1u) {
    ATX_TRY(const double r_minus_q, resolve_carry_diff(curves, T));
    const std::uint32_t n_remaining = rv.n_obs_total - rv.n_obs_done;
    m += discrete_monitoring_addend(m, T, n_remaining, r_minus_q);
    flags |= DerivFlags::DiscreteCorrApplied;
  }

  const double s = vv.xi * std::sqrt(T);
  // The option's strike carried into W-space: (V-K)+ == b*(W - (K-a)/b)+.
  // w_future > 0 here -- the fully-aged exit above already returned.
  const double k_w = (k_opt - a) / w_future;
  const double premium = w_future * (is_call ? detail::lognormal_call(m, s, k_w)
                                             : detail::lognormal_put(m, s, k_w));

  flags |= DerivFlags::ModelProxy | sq.flags;
  if (vv.calibrated) {
    flags |= DerivFlags::VolOfVolCalibrated;
  }
  const double df = deriv_df_at_T(curves, T, flags);

  DerivQuote out = variance_option_quote(premium, a, w_future * m, df, contract, flags);
  out.uncapped_var_dec = sq.uncapped_var_dec;
  out.integration_error_est = sq.integration_error_est;
  carry_strip_grid(out, sq);
  out.vol_of_vol_used = vv.xi;
  return Ok(out);
}

// Assembles a DerivQuote for price_capped_vol_swap's three exit paths
// (pinned, fully-aged deterministic, model-based split-domain quadrature).
// Mirrors capped_var_swap_quote's bookkeeping but in VOL units:
// fair_strike_points uses the 1e2 vol-points scale (not 1e4 var-points), and
// accrued_component_dec / future_component_dec deliberately stay
// VARIANCE-space diagnostics (a and b*m, not their square roots) -- the brief
// specifies these as the blended-variance decomposition even though the
// strike itself is vol-space.
[[nodiscard]] DerivQuote capped_vol_swap_quote(double expectation_dec, double accrued_dec,
                                               double future_dec, double cap_option_dec,
                                               double df, const DerivContract& contract,
                                               DerivFlags flags) noexcept {
  DerivQuote out{};
  out.fair_strike_dec = expectation_dec;  // E[min(sqrt V,c)]: strike pricing to PV = 0
  out.fair_strike_points = 1.0e2 * expectation_dec;  // vol points, NOT var points
  out.pv = df * contract.notional * (expectation_dec - contract.strike_dec);
  out.undiscounted_expectation_dec = expectation_dec;
  out.accrued_component_dec = accrued_dec;
  out.future_component_dec = future_dec;
  out.cap_option_value_dec = cap_option_dec;
  out.flags = flags;
  return out;
}

// Capped volatility swap: E[min(sqrt(V), c)] for the blended variance
// V = a + b*W (see file header / Task 4 for the a/b/W model), c =
// contract.cap_dec a decimal VOL cap, C = c^2 its variance-units image.
//
// Raw Gauss-Hermite on min(sqrt V, c) is NOT used: the payoff is kinked in W
// (established by Task 2's rv_lognormal.hpp header note -- GH loses spectral
// accuracy past a kink), so the domain is split instead at the kink's
// standard-normal coordinate z* solving a + b*w* = C:
//   z* = (ln(w*/m) + s^2/2) / s
// Below the kink sqrt(a+b*W) is smooth and integrated by
// lognormal_truncated_expect (GL-64); above it the payoff is the constant c
// and the tail probability closes analytically via 1 - Phi(z*). The
// degenerate s == 0 case (W collapses to a point mass at m) is handled before
// the quadrature call -- lognormal_truncated_expect asserts s > 0.
//
// Exit paths, in order (mirrors price_capped_var_swap):
//   1. PIN: a = w_done*rv_done_dec >= C -> deterministic df*N*(c-K).
//   2. FULLY AGED (not pinned): min(sqrt(V),C) collapses to sqrt(rv_done_dec)
//      exactly (w_future == 0, no future leg to model).
//   3. Otherwise: strip for K_var_future, resolve vol-of-vol, split-domain
//      quadrature.
//
// Precondition (enforced by deriv_price before this is ever called):
// contract.cap_dec > 0.
template <class SurfaceT>
[[nodiscard]] Result<DerivQuote> price_capped_vol_swap(const SurfaceT& surface,
                                                        const CurveSet& curves,
                                                        const DerivContract& contract,
                                                        const DerivConfig& cfg) {
  assert(contract.cap_dec > 0.0 && "capped vol swap: cap_dec validated by the caller");
  const RealizedVarianceSpec& rv = contract.rv_spec;
  const double T = contract.maturity_t;
  const double c = contract.cap_dec;
  const double cap_var = c * c;  // C: the vol cap's variance-units image

  if (cfg.discrete_correction_mode == DerivDiscreteCorrection::FullMc) {
    return Err(ErrorCode::NotImplemented, "FULL_MC discrete correction reserved");
  }

  double w_done = 0.0;
  double w_future = 1.0;
  if (rv.n_obs_total > 0u) {
    w_done = static_cast<double>(rv.n_obs_done) / static_cast<double>(rv.n_obs_total);
    w_future = 1.0 - w_done;
  }
  const double a = w_done * rv.rv_done_dec;

  DerivFlags flags = DerivFlags::None;
  if (rv.n_obs_done > 0u) {
    flags |= DerivFlags::Aged;
  }
  if (rv.n_obs_total > 0u && rv.n_obs_done >= rv.n_obs_total) {
    flags |= DerivFlags::FullyAged;
  }

  if (a >= cap_var) {
    flags |= DerivFlags::CapPinned | DerivFlags::CapApplied;
    const double df = deriv_df_at_T(curves, T, flags);
    return Ok(capped_vol_swap_quote(c, a, 0.0, 0.0, df, contract, flags));
  }

  if (has_flag(flags, DerivFlags::FullyAged)) {
    // a < C here (the pin check above already handled a >= C): min(V,C)
    // collapses to the realized leg, no model needed.
    const double r1 = std::sqrt(std::fmax(a, 0.0));
    const double df = deriv_df_at_T(curves, T, flags);
    return Ok(capped_vol_swap_quote(r1, a, 0.0, 0.0, df, contract, flags));
  }

  if (!(T > 0.0)) {
    return Err(ErrorCode::InvalidArgument,
               "capped vol swap needs T > 0 to price the future leg");
  }
  ATX_TRY(auto sq, var_swap_fair_strike(surface, curves, T, cfg));

  // Same xi-before-correction ordering (PV-8) and the same Diffusion1OverN
  // correction as price_capped_var_swap / price_var_swap -- see
  // price_capped_var_swap's comment for why both have to match exactly.
  const double m_uncorrected = sq.fair_strike_dec;
  ATX_TRY(const VolOfVol vv, resolve_vol_of_vol(surface, curves, T, m_uncorrected, cfg));

  double m = m_uncorrected;
  if (cfg.discrete_correction_mode == DerivDiscreteCorrection::Diffusion1OverN &&
      rv.n_obs_total >= 1u) {
    ATX_TRY(const double r_minus_q, resolve_carry_diff(curves, T));
    const std::uint32_t n_remaining = rv.n_obs_total - rv.n_obs_done;
    m += discrete_monitoring_addend(m, T, n_remaining, r_minus_q);
    flags |= DerivFlags::DiscreteCorrApplied;
  }

  const double s = vv.xi * std::sqrt(T);
  const double b = w_future;  // > 0: not fully aged (checked above)
  const auto sqrt_v = [a, b](double w) noexcept { return std::sqrt(a + b * w); };

  // {E[min(sqrt V,c)], E[sqrt V] - E[min(sqrt V,c)]}. s <= 0: W collapses to
  // a point mass at m, no quadrature, no kink to split. s > 0: split-domain
  // quadrature at the kink z* solving a + b*w* = C.
  const auto [expectation, cap_option] = [&]() -> std::pair<double, double> {
    if (s <= 0.0) {
      const double sqrt_v_mean = sqrt_v(m);
      const double capped = std::fmin(sqrt_v_mean, c);
      return {capped, sqrt_v_mean - capped};
    }
    const double w_star = (cap_var - a) / b;  // > 0: a < C (checked above)
    const double z_star = (std::log(w_star / m) + 0.5 * s * s) / s;
    const double lower = detail::lognormal_truncated_expect(m, s, -8.0, z_star, sqrt_v);
    const double tail_prob = 1.0 - detail::norm_cdf(z_star);
    const double capped = lower + c * tail_prob;
    const double uncapped_sqrt = detail::lognormal_truncated_expect(m, s, -8.0, 8.0, sqrt_v);
    return {capped, uncapped_sqrt - capped};
  }();

  flags |= DerivFlags::ModelProxy | DerivFlags::CapApplied | sq.flags;
  if (vv.calibrated) {
    flags |= DerivFlags::VolOfVolCalibrated;
  }
  const double df = deriv_df_at_T(curves, T, flags);

  DerivQuote out =
      capped_vol_swap_quote(expectation, a, b * m, cap_option, df, contract, flags);
  out.uncapped_var_dec = sq.uncapped_var_dec;
  out.integration_error_est = sq.integration_error_est;
  carry_strip_grid(out, sq);
  out.vol_of_vol_used = vv.xi;
  out.convexity_adjustment_dec = sqrt_v(m) - expectation;
  return Ok(out);
}

}  // namespace

// ── Variance strip ─────────────────────────────────────────────────────────

namespace {

// Task F-3: the per-DerivKind constants of `strip_fair_value_core`'s shared
// body, in ONE exhaustive switch instead of scattered `kind ==` tests. Kinds
// with no OTM-strip form of their own return `has_strip_form == false` rather
// than silently inheriting VarSwap's -- no call site can reach that today
// (each of the three strip dispatchers names its own kind), which is exactly
// why it must fail loud if one ever does.
struct StripKindTraits {
  // VarSwap's model-free 1/K^2 weight, one K absorbed by the log-strike
  // Jacobian dK = K dx; false only for GammaSwap, whose Lee weight
  // lambda_yy = 2/(Y0*K) cancels the Jacobian entirely.
  bool weight_by_strike = true;
  bool scale_by_spot = false;  // GammaSwap's 1/S0 (Y0) in the outer scale
  bool has_strip_form = true;
  const char* t_error = "var strip needs T > 0";
  counters::ledger::Solve counter = counters::ledger::Solve::VarSwapStripEvals;
};

[[nodiscard]] StripKindTraits strip_kind_traits(DerivKind kind) noexcept {
  StripKindTraits t{};
  switch (kind) {
  case DerivKind::VarSwap:
    return t;
  case DerivKind::CorridorVarSwap:
    // Same integrand and same outer scale as VarSwap -- a corridor swap
    // differs ONLY in the integration window (see `corridor_log_window`).
    t.t_error = "corridor strip needs T > 0";
    t.counter = counters::ledger::Solve::CorridorVarSwapStripEvals;
    return t;
  case DerivKind::GammaSwap:
    t.weight_by_strike = false;
    t.scale_by_spot = true;
    t.t_error = "gamma strip needs T > 0";
    t.counter = counters::ledger::Solve::GammaSwapStripEvals;
    return t;
  case DerivKind::VolSwap:
  case DerivKind::CappedVarSwap:
  case DerivKind::CappedVolSwap:
  // Task F-5: an option on variance has no OTM-strip form of ITS OWN. It runs
  // the VarSwap strip -- through `var_swap_fair_strike`, exactly as the capped
  // kinds do -- to resolve the future leg's mean, and then applies a nonlinear
  // payoff to it. Landing here (rather than on the VarSwap arm) is what keeps
  // `strip_fair_value_core` from being enterable with an option kind, and is
  // also why F-5 adds no new solve-ledger counter: the strip work it does IS a
  // var-swap strip and is already counted as one.
  case DerivKind::VarianceCall:
  case DerivKind::VariancePut:
    break;  // priced through a nonlinear model layer, never through this body
  }
  t.has_strip_form = false;
  return t;
}

// Task F-2 (PV-F1 / LIT-7): shared strip core for the two model-free OTM-
// strip products this file ships -- VarSwap's K_var(T) and GammaSwap's
// K_gamma(T) (Lee's weighted-variance strip, w(y) = y/Y0). Every
// grid/span/wing-clamp/Lee-slope/C-3-split resolution step below is
// IDENTICAL between the two kinds (none of it reads anything but the
// surface, curves, T, and cfg -- `kind` is not consulted until the
// integrand and the outer scale, both marked `kind ==` at their one site
// each); "share the resolved-grid path" per the task brief. The two kinds
// differ in EXACTLY two places:
//   - the per-node integrand (`price_node` below): VarSwap divides by K (the
//     model-free variance strip's own 1/K^2 weight, one K absorbed by the
//     log-strike Jacobian dK = K dx); GammaSwap does not (Lee's weight gives
//     lambda_yy = 2/(Y0*K), and THAT 1/K cancels the Jacobian entirely,
//     leaving the raw undiscounted OTM price -- see `DerivKind::GammaSwap`'s
//     own header doc for the from-paper derivation).
//   - the outer scale (just above `out.fair_strike_dec` below): 2/T for
//     VarSwap; 2/(T*S0) for GammaSwap (Y0 = S0 = `curves.spot`), which is
//     why GammaSwap alone needs `curves.spot > 0`.
// `var_swap_fair_strike` (the public template just below this one) is a
// one-line forward with `kind = DerivKind::VarSwap` -- every VarSwap-path
// line in this function is textually UNCHANGED from the pre-F-2 body that
// used to live directly in `var_swap_fair_strike` itself, so the VarSwap
// path is bit-for-bit identical to before this task. `price_gamma_swap`
// (this file's GammaSwap dispatcher, mirroring `price_var_swap`) calls this
// with `kind = DerivKind::GammaSwap`.
//
// Task F-3 adds a THIRD kind, `CorridorVarSwap`, which differs from VarSwap in
// neither of the two places above (same 1/K integrand, same 2/T outer scale)
// but in a third: the INTEGRATION WINDOW, restricted to the corridor. See the
// `corridor` parameter and the window block below.
template <class SurfaceT>
[[nodiscard]] Result<DerivQuote> strip_fair_value_core(const SurfaceT& surface,
                                                        const CurveSet& curves, double T,
                                                        const DerivConfig& cfg, DerivKind kind,
                                                        const StrikeCorridor& corridor) {
  // Task F-3: every kind-dependent quantity in this body, resolved ONCE
  // through an EXHAUSTIVE switch. F-2 left them as two inline `kind ==
  // GammaSwap` tests, each of which routes a NEW enumerator onto the VarSwap
  // branch SILENTLY -- which happens to be right for CorridorVarSwap and would
  // have been wrong for the next kind, with nothing to say so. `-Wswitch -WX`
  // makes this site refuse to compile instead (empirically confirmed on this
  // tree at F-2: `backtest.cpp`'s own DerivKind switch did exactly that).
  const StripKindTraits traits = strip_kind_traits(kind);
  if (!traits.has_strip_form) {
    return Err(ErrorCode::InvalidArgument,
               "strip core: this DerivKind has no OTM-strip form of its own");
  }
  if (!(T > 0.0)) {
    // m-1 (Task F-2 fix round 1): restores VarSwap's original, kind-specific
    // message -- collapsed to the generic "strip needs T > 0" by the
    // refactor that factored this function out of var_swap_fair_strike's old
    // body, the one line of that refactor that was NOT bit-for-bit despite
    // the commit claiming it was. GammaSwap gets its own equally specific
    // message rather than inheriting VarSwap's exact wording.
    return Err(ErrorCode::InvalidArgument, traits.t_error);
  }
  if (!reserved_fields_clean(cfg)) {
    return Err(ErrorCode::NotImplemented, "reserved config field is non-zero");
  }
  if (!vol_of_vol_valid(cfg)) {
    return Err(ErrorCode::InvalidArgument, "vol_of_vol must be >= 0");
  }
  if (!wing_clamp_valid(cfg)) {
    return Err(ErrorCode::InvalidArgument, "wing_clamp_k must not be NaN");
  }
  // Task P-6 (VarSwap) / Task F-2 (GammaSwap): one bump per actual strip
  // quadrature attempt, counted past the cheap up-front validation (a caller
  // error above never touches the grid, so it is not "an evaluation") -- see
  // Solve::VarSwapStripEvals/GammaSwapStripEvals's own doc. TWO SEPARATE
  // counters, not one shared one: P-6's book-memo O(K)-not-O(L) gate reads
  // VarSwapStripEvals specifically, and folding GammaSwap evals into it would
  // silently corrupt what that gate measures. Task F-3 adds a third counter on
  // the same reasoning, resolved in `strip_kind_traits` with the rest.
  counters::ledger::bump(traits.counter);
  // Review fix I-4: resolved ONCE here (reused at the resolve_wing_clamp call
  // below) and validated eagerly -- a non-finite/non-positive supplied band
  // fails the call instead of silently widening trust to the mode-blind
  // default (see surface_certified_wing_band_valid).
  const std::optional<double> cert_wing_band = surface_certified_wing_band(surface);
  if (!surface_certified_wing_band_valid(cert_wing_band)) {
    return Err(ErrorCode::InvalidArgument,
               "surface_certified_wing_band must be unset or a finite, positive half-band");
  }

  // Grid bounds and node count: quality default, overridden by the config.
  // An explicit [k_min_log, k_max_log] PINS the span — the caller asked for
  // exactly that strip and gets exactly it (with a truncation flag if it does
  // not cover the wings). Otherwise the tier span is a FLOOR that E2 widens to
  // the tenor's own vol scale below, once the ATM vol is known.
  StripGrid grid = strip_quality_defaults(cfg.quality);
  bool span_pinned = false;
  if (cfg.k_min_log != 0.0 || cfg.k_max_log != 0.0) {
    if (!(cfg.k_min_log < cfg.k_max_log)) {
      return Err(ErrorCode::InvalidArgument, "k_min_log must be < k_max_log");
    }
    grid.k_min_log = cfg.k_min_log;
    grid.k_max_log = cfg.k_max_log;
    span_pinned = true;
  }
  if (cfg.strip_nodes != 0u) {
    std::uint32_t n = cfg.strip_nodes;
    if ((n & 1u) == 0u) {
      n += 1u;  // force odd for composite Simpson
    }
    if (n < 5u) {
      n = 5u;
    }
    grid.n_nodes = n;
  }

  // Forward + discount at T.
  const double F = resolve_forward(curves, T);
  const double df = curves.yield.disc(T);
  if (!(F > 0.0) || !(df > 0.0)) {
    return Err(ErrorCode::OutOfRange, "forward/discount unavailable at T");
  }
  // Task F-2: GammaSwap's fair-strike formula divides by S0 (Y0 in Lee's
  // notation, curves.spot here). VarSwap's own formula never reads
  // curves.spot at all, so this check is scoped to the one kind that needs
  // it -- checked here, right beside F/df, so a bad spot fails before any
  // grid work rather than after a full quadrature.
  if (traits.scale_by_spot && !(curves.spot > 0.0)) {
    return Err(ErrorCode::OutOfRange, "gamma strip needs curves.spot > 0");
  }
  // Task F-3: the corridor's k-image at THIS call's forward. Resolved here,
  // beside F, because it is a pure function of (corridor, F) and every use
  // below wants the same value. Validation is the CALLER's (each dispatcher
  // rejects a malformed corridor before it can reach a quadrature); this
  // asserts the contract rather than re-deriving it, because a bad bound
  // reaching here would silently produce a NaN window that `fmax`/`fmin`
  // propagate into the span.
  assert(corridor_valid(corridor) && "strip core: caller must validate the corridor");
  const CorridorLogWindow corridor_k = corridor_log_window(corridor, F);

  // ── E2 / AN-P1-2: adaptive wings ────────────────────────────────────────
  //
  // The tier span is fixed in k and knows nothing about σ√T, so a high-vol or
  // long-dated tenor integrated only the middle of its own distribution and
  // reported K_var biased LOW. Widen the (symmetric) span to the shared
  // convention `max(tier_span, 6·σ_atm·√T)` — the same policy
  // `analytics_density.cpp` already used via `RndConfig::width_sigmas`.
  //
  // `required` is also what decides truncation below. A tenor whose ATM vol is
  // unusable yields required == 0, i.e. "coverage not judgeable", and the span
  // stays at the tier default.
  //
  // FIX-E M-6: the width is a CONFIG knob (`DerivConfig::width_sigmas`), as it
  // already was on the density route. 0 keeps the shared 6σ default, so every
  // existing caller is unchanged; a negative value turns vol scaling off, which
  // is the escape hatch for a caller who wants an exactly-specified strip and
  // does not want it flagged short.
  const double width_sigmas =
      cfg.width_sigmas == 0.0 ? strip::kDefaultWidthSigmas : cfg.width_sigmas;
  const double sigma_atm = surface.iv(0.0, T);
  const double required = strip::required_half_width(sigma_atm, T, width_sigmas);
  // Set when the adaptive rescale below hit `strip::kMaxRescaleNodes`, i.e. the
  // grid resolves COARSER in dk than this tier promises for its own vol scale.
  // That is exactly what LowT reports, and nothing downstream can re-derive it.
  bool span_rescale_capped = false;
  if (!span_pinned) {
    const double floor_half = std::fmax(-grid.k_min_log, grid.k_max_log);
    const double kh = strip::adaptive_half_width(floor_half, sigma_atm, T, width_sigmas);
    if (kh > 0.0) {
      grid.k_min_log = -kh;
      grid.k_max_log = kh;
    }
    // FIX-E M-7: SCALE THE NODE COUNT WITH THE SPAN. A tier promises a
    // resolution (Δk), not just a node count: Standard is 257 nodes over ±1.5,
    // i.e. Δk ≈ 0.0117. Widening to ±3.6 at 257 nodes silently made Δk 2.4x
    // COARSER (3.6x on Fast) on exactly the tenors E2 widens — trading a
    // truncation bias for a quadrature one. Hold Δk at the tier's own value
    // instead. Only when the caller has not pinned `strip_nodes`: an explicit
    // node count is a request, same as an explicit span.
    //
    // The rescale itself (the 4m+1 Richardson rounding, and the
    // `kMaxRescaleNodes` bound that keeps the unbounded `kh / floor_half` ratio
    // away from a `ceil()`->`size_t` cast that would be UB) lives in
    // `strip::span_rescaled_nodes`.
    //
    // `capped` is carried out to LowT below rather than left to the spacing
    // check: that check CANNOT see a capped rescale, because both sides of it
    // scale linearly in sigma_atm*sqrt(T) and the ratio is a constant well
    // under the ceiling for every input (see span_rescaled_nodes' own note).
    if (cfg.strip_nodes == 0u) {
      const strip::SpanRescale rescaled =
          strip::span_rescaled_nodes(grid.n_nodes, kh, floor_half);
      grid.n_nodes = rescaled.n_nodes;
      span_rescale_capped = rescaled.capped;
    }
  }

  // ── Task F-3: the corridor restricts the INTEGRATION WINDOW ───────────────
  //
  // Everything above resolved the SPAN [grid.k_min_log, grid.k_max_log] and
  // the node budget `n`. The corridor now cuts the interval that budget is
  // actually spent on, and from here down `integ_k_lo`/`integ_k_hi` -- not the
  // span -- are what the split, the resolution floor, the wing-clamp verdict
  // and the quadrature see.
  //
  // RESTRICT, do not indicate. The alternative (keep the full span, multiply
  // the integrand by 1{K in C}) would put the corridor edges mid-panel, which
  // is a JUMP discontinuity -- worse than the C1 kinks C-3 exists to isolate --
  // and would apportion the node budget in proportion to LENGTH, so a corridor
  // spanning 1/60th of the span would be integrated on ~4 nodes. Restricting
  // spends the whole budget inside the corridor and makes both edges panel
  // boundaries by construction, which is what the brief's "corridor edges
  // become composite-Simpson split points" buys.
  //
  // BIT-IDENTITY FOR EVERY OTHER KIND, BY CONSTRUCTION, NOT BY TESTING. An
  // unbounded side of the corridor is -/+ infinity (`corridor_log_window`), and
  // fmax(x, -inf) == x / fmin(x, +inf) == x exactly for every finite x, so a
  // VarSwap or GammaSwap call -- which always passes `StrikeCorridor{}` --
  // computes the same two doubles the span already held. Every substitution
  // below is therefore a no-op on those paths at the bit level.
  const double integ_k_lo = std::fmax(grid.k_min_log, corridor_k.k_lo);
  const double integ_k_hi = std::fmin(grid.k_max_log, corridor_k.k_hi);
  if (!(integ_k_hi > integ_k_lo)) {
    // The corridor and the resolved span do not overlap (or touch at a single
    // point). There is no strip to integrate, and 0.0 would be a plausible-
    // looking wrong number: the true corridor variance over a region the grid
    // never reaches is small but not zero, and the caller would have no signal.
    return Err(ErrorCode::OutOfRange,
               "corridor does not intersect the resolved strip span");
  }

  // Wing trust band for the surface READS (see DerivConfig::wing_clamp_k): a
  // node beyond the band prices at its true strike under the BAND-EDGE vol —
  // flat-vol tails over the uncertified extrapolation region, never a
  // truncated span. band <= 0 means the clamp is off. Resolved BEFORE the
  // resolution floor below, which has to know how many panels the C-3 split
  // will cut — and that depends on where this band falls inside the span.
  //
  // Task F-3: judged on the INTEGRATION WINDOW, not the span. The flag's
  // documented meaning is "flat-vol tails were in effect"; a corridor that
  // keeps every node inside the band means they were not, and saying otherwise
  // would be a false provenance claim.
  const double wing_band = resolve_wing_clamp(cfg, cert_wing_band);
  const bool wing_clamped =
      wing_band > 0.0 && (integ_k_lo < -wing_band || integ_k_hi > wing_band);

  // Task F-1: exhaustive over `StripWingMode` (no `default:`, so a future
  // enumerator turns this into a compiler error under /W4 /WX rather than
  // silently falling through) -- resolved ONCE here and reused by every
  // downstream consumer (the C-3 split below, the node-read loops, the
  // provenance flag) instead of re-testing `cfg.wing_mode` at each site.
  bool use_lee_slope = false;
  switch (cfg.wing_mode) {
  case StripWingMode::FlatClamp:
  case StripWingMode::Raw:
    use_lee_slope = false;
    break;
  case StripWingMode::LeeSlopeExtrapolation:
    use_lee_slope = true;
    break;
  }
  // Review fix m-3: the single shared "is Lee extrapolation actually doing
  // anything on this call" predicate. `use_lee_slope && wing_band > 0.0` was
  // previously written out independently at three sites (the slope-resolution
  // guard just below, the scalar node-loop dispatch, and the batched-path
  // disable), held in sync by nothing but proximity -- the same defect class
  // as P-4's I-2 and P-5's I-1, just in code instead of a named local. The
  // concrete failure this closes: narrowing the batched-path disable alone
  // (e.g. to `wing_clamped`, round-0 ledger Minor M-3's proposal) without
  // narrowing the other two identically would let a PricedSurface-backed
  // LeeSlope strip take the batched gather, which has no representation for
  // the extrapolation formula and would silently serve a clamped-position
  // read instead -- FlatClamp values under a LeeSlope config. One bool, used
  // at all three sites, makes that impossible by construction.
  const bool lee_engaged = use_lee_slope && wing_band > 0.0;

  // Task F-1 (review fix I-1): the fitted slice's OWN total-variance slope at
  // each band edge, resolved ONCE per call (not per node) from a central
  // difference of the RAW surface straddling +-wing_band -- the surface is
  // continuous there (the band is a validation-sampling artifact, not a kink
  // in the fit itself), so this is genuinely the slope the certified
  // region's own curve is heading at, not an artifact of the clamp. `beta`
  // is d(total variance)/d|k|, i.e. already sign-corrected for the LEFT edge
  // (where d w/dk < 0 under the usual negative-skew convention -- Lee's
  // bound is stated on the RATE OF GROWTH as |k| increases, a quantity that
  // must be >= 0 on both sides, not on the signed derivative), and clamped
  // to [0, 2-eps] per Lee's moment bound BEFORE it is used for anything.
  //
  // MUST be resolved before the C-3 split decision below, not after: the
  // split needs to know whether the clamp actually CHANGED the slope, since
  // that is exactly when the served function stops being C1 at the edge (see
  // that comment for the full argument). Review fix I-1 found this ordered
  // the other way round -- the split was decided unconditionally before the
  // slope (and so the binding state) was even known -- measured 96.9x-
  // 17,000x worse quadrature and a 2.2x-3.2x understated
  // `integration_error_est` on a fixture whose right wing's slope goes
  // negative below the smile minimum (ordinary for rho < 0, not an exotic
  // corner) and folds to the beta = 0 clamp floor.
  struct LeeWingSlope {
    double w_edge_left = 0.0;
    double w_edge_right = 0.0;
    double beta_left = 0.0;
    double beta_right = 0.0;
  };
  // Step for the central difference. Small enough that the fitted slice's own
  // curvature does not contaminate a FIRST-derivative read (O(h^2) truncation
  // error at h = 1e-4 is ~1e-8 relative on a curve whose second derivative is
  // O(1) in k -- far below anything this task's tolerances can see), large
  // enough to stay well clear of the ULP-scale cancellation a much smaller h
  // would risk in `(w(edge+h) - w(edge-h))`.
  constexpr double kLeeSlopeH = 1.0e-4;
  constexpr double kLeeMomentEps = 1.0e-3;  // Lee 2004 moment bound: beta in [0, 2-eps]
  const auto total_w_at = [&](double k) noexcept {
    const double s = surface.iv(k, T);
    return s * s * T;
  };
  // Clamped slope plus whether the clamp actually CHANGED the value. This is
  // NOT the same test as "the result sits at 0 or 2-eps": a raw slope that
  // already IS exactly 0 (a genuinely flat wing, e.g. WingMode.
  // FlatSurfaceInvariant) needs no correction and leaves no kink behind --
  // `bound` must stay false there, or LeeSlope would pay an unnecessary
  // split on a fixture that has nothing to split. `beta_raw == beta` is
  // false whenever beta_raw is NaN (NaN-safe: a bad edge read is treated as
  // "bound", the safer direction -- keep the split rather than risk missing
  // a real discontinuity).
  struct ClampedSlope {
    double beta = 0.0;
    bool bound = false;
  };
  const auto clamp_lee_slope = [](double beta_raw) noexcept {
    double beta = beta_raw;
    if (!(beta > 0.0)) {
      beta = 0.0;
    } else if (beta > (2.0 - kLeeMomentEps)) {
      beta = 2.0 - kLeeMomentEps;
    }
    return ClampedSlope{beta, !(beta_raw == beta)};
  };
  LeeWingSlope lee_slope{};
  bool lee_clamp_bound = false;
  if (lee_engaged) {
    lee_slope.w_edge_right = total_w_at(wing_band);
    const double slope_r =
        (total_w_at(wing_band + kLeeSlopeH) - total_w_at(wing_band - kLeeSlopeH)) /
        (2.0 * kLeeSlopeH);
    const ClampedSlope cr = clamp_lee_slope(slope_r);
    lee_slope.beta_right = cr.beta;

    lee_slope.w_edge_left = total_w_at(-wing_band);
    // Sign flip: Lee's beta is d w / d|k|, and |k| = -k on the left side, so
    // d w/d|k| = -(d w/dk) there.
    const double slope_l =
        -(total_w_at(-wing_band + kLeeSlopeH) - total_w_at(-wing_band - kLeeSlopeH)) /
        (2.0 * kLeeSlopeH);
    const ClampedSlope cl = clamp_lee_slope(slope_l);
    lee_slope.beta_left = cl.beta;

    lee_clamp_bound = cr.bound || cl.bound;
  }

  // C-3 / LIT-10: the band edge is a genuine C1 KINK under FlatClamp always
  // (d(iv)/dk drops to zero across it), and under LeeSlopeExtrapolation
  // exactly when the clamp above actually bound on either side (review fix
  // I-1) -- when it did not bind, the served function's slope AT the edge
  // equals the raw surface's own slope there by construction (continuous AND
  // slope-matched, C1), so there is no discontinuity for the split to
  // isolate; when it DID bind, the served slope (0 or 2-eps) differs from
  // the raw surface's actual slope just inside the band, which is exactly
  // the same C0/C1 break FlatClamp's own split exists to isolate, and the
  // fix is to isolate it the SAME way: put the edge back on a panel
  // boundary. `Raw`'s `wing_band` is already 0.0 above, so this is a no-op
  // for that mode.
  const double split_wing_band = (use_lee_slope && !lee_clamp_bound) ? 0.0 : wing_band;

  // C-2 / PV-2: the MIRROR rule. The rescale above only widens the span for a
  // high-vol/long-dated tenor; a short-tenor/low-vol quote can still resolve
  // too coarsely even at (or below) the tier's own floor span, because the
  // tier grids are sized for a roughly-1Y reference vol scale, not a 1-day
  // one. Enforce dk <= sigma_atm*sqrt(T)/4 by raising the node count, same
  // 4m+1 rounding as above (`strip::dk_floor_nodes`). `sigma_atm` is the same
  // ATM vol read the span rescale above already resolved -- no second read.
  // `cfg.strip_nodes` pinned is never overridden here, same as the span
  // rescale: a pinned node count is a caller request and gets flagged
  // (LowT) instead of silently changed.
  //
  // The ceiling binds the spacing the strip ACTUALLY integrates on, which
  // after C-3 is per-panel, not one uniform dk — hence the panel count.
  const double dk_max = strip::dk_ceiling(sigma_atm, T);
  const double resolved_span = integ_k_hi - integ_k_lo;
  const std::size_t n_panels =
      strip::strip_panel_count(integ_k_lo, integ_k_hi, split_wing_band);
  // Seeded from the span rescale: a capped rescale is under-resolved for its own
  // vol scale by construction, and the `max_panel_spacing > dk_max` check below
  // provably cannot detect it (sigma_atm*sqrt(T) cancels out of that ratio).
  bool low_t = span_rescale_capped;
  if (cfg.strip_nodes == 0u) {
    const std::size_t raised =
        strip::dk_floor_nodes(resolved_span, grid.n_nodes, dk_max, n_panels);
    if (raised != grid.n_nodes) {
      grid.n_nodes = raised;
      low_t = true;
    }
  }

  const std::size_t n = grid.n_nodes;

  // C-3 / LIT-10: the integrand is piecewise smooth, not smooth — it kinks at
  // k = 0 (put-call parity) and at ±wing_band whenever `split_wing_band`
  // above is nonzero: always under `StripWingMode::FlatClamp` when the clamp
  // binds, and under `LeeSlopeExtrapolation` too whenever ITS clamp actually
  // bound on either edge (review fix I-1 — see the `split_wing_band` comment
  // above for why a non-binding LeeSlope edge needs no cut, and a binding one
  // needs exactly the same cut FlatClamp always takes). Split the composite
  // Simpson at every interior kink so each one is a PANEL BOUNDARY for any
  // span, symmetric or not, and the O(h⁴) law (and with it the Richardson
  // estimate below) holds on every panel. See `plan_strip_split` for the
  // budget apportionment and its degradation ladder; the total node count and
  // the reported span are unchanged by the split.
  assert(n >= 3u && "composite Simpson needs at least one panel of 3 nodes");
  // Task F-3: planned on the corridor-restricted window, which is what makes
  // both corridor edges panel boundaries -- they are this interval's own
  // endpoints. `strip_panel_bounds`'s strict comparisons already dedup a k = 0
  // or +-band kink that coincides with, or falls outside, an edge, so a
  // corridor that swallows or abuts either interior kink degrades to fewer
  // panels rather than producing a degenerate one.
  const strip::StripSplit split =
      strip::plan_strip_split(integ_k_lo, integ_k_hi, n, split_wing_band);

  // LowT, decided on the grid actually integrated. For the unpinned path the
  // floor above has already provisioned every panel under the ceiling, so this
  // only ever confirms it; for a caller-pinned node count — never overridden —
  // it is the whole job of the flag, and checking the widest PANEL rather than
  // the nominal span/(n-1) is what makes the verdict honest after C-3.
  if (dk_max > 0.0 && strip::max_panel_spacing(split) > dk_max) {
    low_t = true;
  }

  // Richardson half-grid quadrature error estimate. Valid only when EVERY
  // panel is 4m+1, so each panel's half grid ((n+1)/2 nodes, every other node)
  // is itself an odd count, a valid composite-Simpson grid, and — decisively —
  // still has the kinks on its own boundaries. The FIX-E M-7 rounding, the C-2
  // resolution floor and the tier defaults all keep n on the 4m+1 lattice, and
  // the split keeps every panel there; a caller-pinned `strip_nodes` is a
  // request and is not rounded, so it may leave `halvable` false.
  const bool halvable = split.richardson_ok;

  // Composite Simpson on   integral OTM(K) / (df * F * e^x) dx
  //                      == integral OTM(K) / (df * K) dx   since K = F * e^x.
  // Nodes with a non-finite / non-positive surface IV contribute zero; a bad
  // node touching either integration boundary flips the truncation flag.
  //
  // Task P-3 / PV-P4: classification + Black-76 pricing given an ALREADY-
  // RESOLVED sigma, shared verbatim by every walk of the strip's node grid
  // below -- only WHERE sigma comes from ever differs between them.
  const auto price_node = [&](double x, double sigma) noexcept {
    const double K = F * std::exp(x);
    const Side side = (x < 0.0) ? Side::Put : Side::Call;
    const bool bad = !std::isfinite(sigma) || sigma <= 0.0;
    const double price = bad ? 0.0 : black76_price(F, K, T, sigma, df, side);
    // Task F-2: the 1/K weight is VarSwap-only -- see this function's own
    // header comment for the Jacobian-cancellation argument that drops it
    // for GammaSwap (whose per-node value is just the undiscounted price).
    const double weighted = traits.weight_by_strike ? (price / (df * K)) : (price / df);
    return std::pair<double, bool>{weighted, bad};
  };

  double integral = 0.0;
  double integral_half = 0.0;
  bool bad_first = false;
  bool bad_last = false;
  // PV-4: nodes strictly inside the grid whose surface read was non-finite/
  // non-positive. bad_first/bad_last (the two grid ENDPOINTS) are tracked
  // separately, unchanged, below -- they drive StripTruncatedLeft/Right, a
  // COVERAGE signal. An interior bad node is a different failure (a hole in
  // the middle of an otherwise-usable surface), counted here and consumed
  // after the loop.
  std::size_t interior_bad_count = 0;

  // Review fix round 1, I-6: node-position formula shared by every walk
  // below (the gather pass and the accumulate loop) -- previously inlined
  // three separate times (scalar loop, batched consume loop, batched gather
  // loop), which is exactly the kind of duplication that lets one copy drift
  // from the other two. Panel ends are the kink abscissae verbatim rather
  // than k_lo + i*dx, so no rounding step can drift a kink off the node it
  // must sit on.
  const auto node_x = [](const strip::StripPanel& panel, std::size_t np, double dx,
                         std::size_t i) noexcept {
    return (i == 0)        ? panel.k_lo
          : (i + 1 == np) ? panel.k_hi
                          : panel.k_lo + dx * static_cast<double>(i);
  };

  // Review fix round 1, I-6: the node-walk/Simpson-accumulate/endpoint-
  // classify block, unified into ONE traversal parameterized by how sigma is
  // obtained for a freshly-visited node -- this used to be two full
  // near-identical copies (the scalar loop and the batched "consume" loop).
  // Everything except the sigma source is identical between callers: K/side/
  // price via `price_node`, endpoint-vs-interior classification, Simpson
  // accumulation, and the shared-boundary-value reuse that keeps this at
  // exactly one sigma fetch per DISTINCT node -- so it can only ever be
  // written once now, and the two callers below cannot compute a price or a
  // badness verdict differently by construction, not merely by inspection.
  // `sigma_at(x)` is called exactly when a fresh node is visited (`p == 0 ||
  // i != 0`), with the node's UNCLAMPED x (price_node's own K/side read is
  // always unclamped; a `sigma_at` that reads the surface clamps internally,
  // matching what `iv_batch`'s gather pass writes into its buffer).
  const auto accumulate_strip = [&](auto&& sigma_at) noexcept {
    double shared = 0.0;  // integrand at the node the previous panel ended on
    for (std::size_t p = 0; p < split.count; ++p) {
      const strip::StripPanel& panel = split.panels[p];
      const std::size_t np = panel.n_nodes;
      const std::size_t np_half = (np + 1) / 2;
      const double dx = (panel.k_hi - panel.k_lo) / static_cast<double>(np - 1);
      double sum = 0.0;
      double sum_half = 0.0;
      for (std::size_t i = 0; i < np; ++i) {
        // A panel's first node IS the previous panel's last node, and carries
        // the same value: reusing it is what keeps the split at exactly one
        // sigma fetch per DISTINCT node, as the un-split single pass was.
        double integrand = shared;
        if (p == 0 || i != 0) {
          const double x = node_x(panel, np, dx, i);
          const auto [value, bad] = price_node(x, sigma_at(x));
          integrand = value;
          // A panel-boundary kink (e.g. k = 0) is a node strictly inside the
          // WHOLE grid even though it sits at the edge of ITS panel -- only
          // the true ends of the entire split (p == 0's first node, the last
          // panel's last node) are the grid's own boundary.
          const bool is_grid_first = (p == 0 && i == 0);
          const bool is_grid_last = (p + 1 == split.count && i + 1 == np);
          if (is_grid_first) {
            bad_first = bad;
          } else if (is_grid_last) {
            bad_last = bad;
          } else if (bad) {
            ++interior_bad_count;
          }
        }
        sum += simpson_w(i, np) * integrand;
        if (halvable && (i % 2u) == 0u) {
          // Every other node of this panel, quadratured on its own
          // half-density grid (spacing 2*dx) with that grid's own Simpson
          // weights. The panel boundaries -- and so the kinks -- are
          // boundaries of THAT grid too, which is what makes the /15
          // difference an error estimate.
          sum_half += simpson_w(i / 2u, np_half) * integrand;
        }
        if (i + 1 == np) {
          shared = integrand;
        }
      }
      integral += sum * (dx / 3.0);
      integral_half += sum_half * (2.0 * dx / 3.0);
    }
  };

  // Serves total variance w(k_band) + beta*(|k| - k_band) beyond the band,
  // converted back to vol at T -- continuous by construction (the (|k| -
  // k_band) term is exactly 0 at the edge) and additionally slope-matched
  // (C1) whenever `clamp_lee_slope` did not need to bind.
  const auto lee_slope_sigma = [&](double x) noexcept {
    if (std::fabs(x) <= wing_band) {
      return surface.iv(x, T);
    }
    const bool right = x > 0.0;
    const double beta = right ? lee_slope.beta_right : lee_slope.beta_left;
    if (beta == 0.0) {
      // No slope to extrapolate: the served vol is exactly the band-edge
      // vol, mathematically identical to what FlatClamp's clamped read
      // computes at this node -- skip the iv -> w -> iv round trip (whose
      // sqrt/square pair is not guaranteed to be an exact inverse) so a
      // surface with a genuinely flat wing agrees with FlatClamp to the bit,
      // not merely to a quadrature-noise tolerance.
      return surface.iv(right ? wing_band : -wing_band, T);
    }
    const double w_edge = right ? lee_slope.w_edge_right : lee_slope.w_edge_left;
    const double w = w_edge + beta * (std::fabs(x) - wing_band);
    return std::sqrt(w / T);
  };

  // Untouched pre-P-3 single-pass semantics: one surface read per distinct
  // node, interleaved with the Simpson accumulation via `accumulate_strip`.
  // This is the ONLY path for any SurfaceT without a batched read (VolSurface
  // /EssviSurface/SviSurface, SurfaceRefStripView) -- see `has_strip_iv_batch`
  // below -- and it is also what `Strip.BatchedMatchesScalar*` compares the
  // batched path against via `detail::set_strip_batch_disabled_for_test`.
  // Task F-1: under LeeSlopeExtrapolation with an engaged band, every node
  // read routes through `lee_slope_sigma` instead -- the FlatClamp/Raw branch
  // below is otherwise byte-for-byte the pre-F-1 expression, so that mode's
  // reads are unchanged in every particular, not merely in aggregate result.
  const auto run_scalar_node_loop = [&]() noexcept {
    if (lee_engaged) {
      accumulate_strip([&](double x) noexcept { return lee_slope_sigma(x); });
      return;
    }
    accumulate_strip([&](double x) noexcept {
      const double x_read = wing_band > 0.0 ? std::clamp(x, -wing_band, wing_band) : x;
      return surface.iv(x_read, T);
    });
  };

  // Task P-3 / GK-P2 / PV-P4: batched surface read. Runs ONLY when SurfaceT
  // structurally exposes a batched `iv_batch(x, T, out)` (today:
  // `PricedSurfaceStripView` and, since Review fix round 1 / I-7,
  // `CachedBumpView<PricedSurfaceStripView>` -- see that struct's own
  // `iv_batch`) -- every other SurfaceT falls straight through to the
  // untouched scalar loop above, zero behaviour change.
  // `g_strip_batch_disabled_for_test` is a test-only escape hatch (mirrors
  // `simd::set_simd_isa_override`'s established pattern; also readable from
  // `ATX_VOL_DISABLE_STRIP_BATCH` for A/B benchmarking the SAME binary) that
  // forces the scalar loop even when the batched path is available, so a
  // test can prove the two are bit-identical on the exact same surface/grid.
  //
  // Review fix round 1, CRITICAL-1: `n` is NOT bounded by `kMaxStripNodes` on
  // every path that can reach here, and that is deliberate on BOTH of them. A
  // caller-pinned `cfg.strip_nodes` is never clamped (see that block's own
  // comment), and the adaptive span rescale above is bounded by
  // `strip::kMaxRescaleNodes` (32769), not by this buffer length -- bounding it
  // here instead would trade a widened span's truncation error back for
  // quadrature error, which is the exact swap FIX-E M-7 exists to prevent, and
  // at the Audit tier it disabled M-7 outright. So an Audit quote with
  // sigma_atm*sqrt(T) >= ~0.5, or a pinned strip_nodes past the cap, still
  // resolves a node count larger than the fixed-size gather buffers below.
  // The guard is structural, not an assert-only check: `x_read_buf`/
  // `sigma_buf` are never touched at all unless `n <= kMaxStripNodes`
  // (equivalently `gather_n <= kMaxStripNodes`, since `plan_strip_split`
  // preserves the total distinct node count exactly) -- an oversized strip
  // simply falls through to the scalar loop above, which has no fixed-size
  // buffer and is correct, if slower, for any n. See
  // `Strip.BatchedPathFallsBackAboveMaxStripNodes` (derivatives_test.cpp).
  //
  // Gather pass: walk the SAME distinct-node sequence `accumulate_strip`
  // will (`p == 0 || i != 0`, the same `node_x` formula) into a flat buffer,
  // so the ONE `iv_batch` call below resolves the whole strip's carry/
  // bracket once instead of once per node (P-1 already hoisted that across
  // CALLS at the SAME T via `PricedSurfaceStripView`'s own carry cache; this
  // hoists it across NODES within one call too, and collapses N per-node
  // function calls -- through PricedSurfaceStripView::iv -> iv_at ->
  // PricedSurface::iv_with_carry -> resolve_with_carry_and_bracket -- into
  // one tight loop). The consume pass then re-walks the identical sequence a
  // second time via `accumulate_strip`, sourcing sigma from the gathered
  // buffer instead of a live read -- reusing `price_node`/the classification
  // logic verbatim, since it is the SAME lambda instance now, not a second
  // copy of it. `PricedSurface::iv_batch` is PROVEN bit-identical to per-call
  // `iv(K,T)` (PricedSurface.IvBatchMatchesPerCallIv*, priced_surface_test.
  // cpp) and `PricedSurfaceStripView::iv_batch` performs the identical
  // `K = F*exp(x_read)` round-trip the scalar `sigma_at` above already does
  // per node, so this path's sigma values are bit-identical to the scalar
  // loop's by construction, not merely by observation.
  bool used_batched_path = false;
  if constexpr (has_strip_iv_batch<SurfaceT>()) {
    // Task F-1: the gather pass below writes one clamped x per node and
    // resolves sigma from a single raw `iv_batch` read -- it has no way to
    // represent the Lee-slope extrapolation formula (which needs the
    // band-edge slope, not just a clamped position), so that mode falls back
    // to the scalar loop above instead of gathering wrong values. Correctness
    // over performance for a brand-new opt-in mode; FlatClamp/Raw are
    // unaffected (`use_lee_slope` is false for both, so this condition is
    // identical to the pre-F-1 one for them).
    if (!strip_batch_disabled_for_test() && n <= strip::kMaxStripNodes && !lee_engaged) {
      used_batched_path = true;

      // Review fix round 1, I-2: no `{}` value-init on either buffer -- every
      // element up to `gather_n` is written by the loops below before
      // `iv_batch`/the consume pass reads any of it (both spans are
      // explicitly sized to `gather_n`, never to the full array), so
      // zero-filling first was 2 * 16 KB of dead work on every batched call.
      std::array<double, strip::kMaxStripNodes> x_read_buf;
      std::size_t gather_n = 0;
      for (std::size_t p = 0; p < split.count; ++p) {
        const strip::StripPanel& panel = split.panels[p];
        const std::size_t np = panel.n_nodes;
        const double dx = (panel.k_hi - panel.k_lo) / static_cast<double>(np - 1);
        for (std::size_t i = 0; i < np; ++i) {
          if (p != 0 && i == 0) {
            continue;  // shared boundary value, reused below -- no fresh read
          }
          const double x = node_x(panel, np, dx, i);
          // Defense in depth: `n <= kMaxStripNodes` above already makes this
          // unreachable (gather_n tracks n exactly), but a bound this cheap
          // costs nothing and turns "silently wrong" into "loud" if that
          // invariant is ever violated by a future change to the split.
          assert(gather_n < strip::kMaxStripNodes && "gather_n must track n");
          x_read_buf[gather_n++] = wing_band > 0.0 ? std::clamp(x, -wing_band, wing_band) : x;
        }
      }
      std::array<double, strip::kMaxStripNodes> sigma_buf;
      surface.iv_batch(std::span<const double>(x_read_buf.data(), gather_n), T,
                       std::span<double>(sigma_buf.data(), gather_n));

      std::size_t read_idx = 0;
      accumulate_strip([&](double /*x*/) noexcept { return sigma_buf[read_idx++]; });
    }
  }
  if (!used_batched_path) {
    run_scalar_node_loop();
  }

  // Review fix round 1 (Critical): interior-bad-node accounting is gated on
  // the strip's own ENDPOINTS, not on a fresh ATM read. `sigma_atm` above
  // reads the SAME (k_log=0.0, T) point the k = 0 panel-boundary kink node
  // reads inside the loop just run -- k = 0 is a forced, distinct grid node
  // whenever k_lo < 0 < k_hi (`strip_panel_bounds`), true of virtually
  // every real call. Gating on sigma_atm's finiteness could therefore never
  // tell "the surface is unusable everywhere" (T under the legacy_surface
  // short-T extrapolation guard, `T < 0.5*T0`, where EVERY node including
  // both true endpoints reads non-finite) apart from "the one bad interior
  // node happens to sit at ATM" -- exactly the case PV-4's finding names
  // explicitly ("including the k = 0 put-call-parity kink"), silently
  // reproducing the pre-fix bug for it. `bad_first`/`bad_last` (the two
  // TRUE grid endpoints, already computed by the loop above) carry the
  // right signal instead: both true only in the wholesale-unusable case --
  // an interior node's badness, wherever it sits, cannot set either.
  const bool strip_wholly_unusable = bad_first && bad_last;

  // PV-4: a strip whose middle is mostly holes is broken, not merely sparse
  // -- refuse before spending any more work computing a number that would be
  // built mostly from the bad-node zero substitution. `max(2, n/100)` gives
  // small grids a fixed floor (two isolated gaps stays a quote) and scales
  // with node count on large ones, matching the brief's own budget. Skipped
  // entirely when the strip is wholly unusable (see above) -- that is the
  // pre-existing, deliberately-tolerated degenerate corner, not this task's
  // target.
  if (!strip_wholly_unusable && interior_bad_count > std::max<std::size_t>(2, n / 100u)) {
    return Err(ErrorCode::Internal, "variance strip has too many interior bad nodes");
  }

  // Task F-2: the outer scale is the SECOND (and last) kind-dependent
  // quantity -- see this function's own header comment. Resolved once and
  // reused for both the fair strike and the Richardson error estimate below,
  // so the two can never disagree about which scale was actually integrated.
  const double outer_scale = traits.scale_by_spot ? (2.0 / (T * curves.spot)) : (2.0 / T);
  const double k_out = outer_scale * integral;

  // Composite-Simpson error is O(h^4): halving h (doubling the node density)
  // shrinks it ~16x, so the difference between the two estimates is ~15/16 of
  // the coarse grid's own error — a self-contained error bound with no
  // external reference. Summing the SIGNED per-panel differences (rather than
  // their magnitudes) keeps this exact: in the h⁴ limit the sum equals the
  // total error, cancellation between panels included. Stays NaN (not 0) when
  // a panel is not 4m+1: that is a caller-pinned exact node count, and NaN
  // says "not estimated" rather than claiming a zero error nothing checked.
  double err_est = kNaN;
  if (halvable) {
    err_est = std::fabs(outer_scale * (integral - integral_half)) / 15.0;
  }

  // E2 / AN-P1-2: truncation is a COVERAGE property, not a NaN property. The
  // old code raised these flags only when the surface returned a non-finite IV
  // at an integration boundary — which a parametric eSSVI/SVI surface never
  // does, so a truncated parametric strip claimed full coverage. Report a wing
  // as truncated when the span does not reach 6·σ_atm·√T on that side, OR when
  // the boundary node was unusable (the original condition, still meaningful
  // for surfaces with genuine NaN wings).
  const strip::WingCoverage cover =
      strip::wing_coverage(grid.k_min_log, grid.k_max_log, required);

  // Task F-3: on a BOUNDED corridor side the coverage requirement is the
  // CORRIDOR EDGE, not 6*sigma*sqrt(T). Two things would otherwise go wrong at
  // once, in opposite directions. (a) A narrow corridor well inside the span
  // would report BOTH wings truncated forever -- the strip covers everything
  // the product is defined over, and the vol-scaled requirement is simply not
  // the right question for it. (b) A corridor reaching PAST the span really is
  // truncated (in-corridor variance the grid never integrated), and the
  // vol-scaled test can easily say the span was fine. So: bounded side ->
  // "did the span reach the edge?"; unbounded side -> the existing test,
  // unchanged. `corridor_k` is -/+ infinity on an unbounded side, so a
  // non-corridor call takes the `cover` branch on both sides and the flags are
  // bit-identical to before this task.
  const bool left_short = std::isfinite(corridor_k.k_lo)
                              ? (grid.k_min_log > corridor_k.k_lo)
                              : cover.left_short;
  const bool right_short = std::isfinite(corridor_k.k_hi)
                               ? (grid.k_max_log < corridor_k.k_hi)
                               : cover.right_short;

  DerivFlags flags = DerivFlags::None;
  if (bad_first || left_short) {
    flags |= DerivFlags::StripTruncatedLeft;
  }
  if (bad_last || right_short) {
    flags |= DerivFlags::StripTruncatedRight;
  }
  if (wing_clamped) {
    // Task F-1: same STRUCTURAL condition, mode-routed -- `use_lee_slope` is
    // false for `Raw` too, but `wing_clamped` is always false there (its
    // `wing_band` resolves to 0.0), so this expression is unreachable for
    // that mode and the choice of flag on this line never matters for it.
    flags |= use_lee_slope ? DerivFlags::WingExtrapolated : DerivFlags::WingClamped;
  }
  if (low_t) {
    flags |= DerivFlags::LowT;
  }
  if (!strip_wholly_unusable && interior_bad_count > 0u) {
    flags |= DerivFlags::InteriorBadNodes;
  }

  DerivQuote out{};
  out.fair_strike_dec = k_out;
  out.fair_strike_points = 1.0e4 * k_out;
  out.pv = 0.0;
  out.undiscounted_expectation_dec = k_out;
  out.uncapped_var_dec = k_out;
  out.accrued_component_dec = 0.0;
  out.future_component_dec = k_out;
  out.convexity_adjustment_dec = 0.0;
  out.integration_error_est = err_est;
  // The grid this quote was actually integrated on, so a caller (deriv_greeks)
  // can pin it back and reproduce this exact quadrature.
  //
  // Task F-3, DELIBERATE AND LOAD-BEARING: for a corridor strip these report
  // the span BEFORE the corridor cut it, not the window that was integrated.
  // The field's contract is REPRODUCTION -- "feed these back as k_min_log /
  // k_max_log / strip_nodes and get this quote again" -- and reproduction
  // needs the pre-corridor span, because the corridor is re-derived from the
  // CONTRACT (which the replaying caller also supplies) against that
  // evaluation's OWN forward. Reporting the window instead would silently
  // break `deriv_greeks`' spot bumps: the pinned span would clip the corridor
  // on ONE side only (fmax(pinned_lo, ln(lo/F')) moves when F falls and does
  // not when F rises), so the central difference would collect roughly half of
  // the corridor edge's contribution to delta. The integrated window is
  // [max(strip_k_lo_used, ln(corridor_lo/F)), min(strip_k_hi_used,
  // ln(corridor_hi/F))] -- derivable by any caller that has the contract.
  out.strip_k_lo_used = grid.k_min_log;
  out.strip_k_hi_used = grid.k_max_log;
  out.strip_nodes_used = static_cast<std::uint32_t>(n);
  // The band actually resolved above (FIT-C7 / Task C-6) -- carried the same
  // way as the grid fields it sits beside, so a caller can inspect exactly
  // which trust band this quote's reads were clamped to.
  out.resolved_wing_clamp = wing_band;
  out.flags = flags;
  return Ok(out);
}

}  // namespace

template <class SurfaceT>
Result<DerivQuote> var_swap_fair_strike(const SurfaceT& surface,
                                        const CurveSet& curves, double T,
                                        const DerivConfig& cfg) {
  // Task F-3: the corridor is passed EXPLICITLY (rather than defaulted) at
  // every one of the three call sites, so that adding a strip kind cannot
  // acquire "no corridor" by accident -- the author has to write it down.
  // `StrikeCorridor{}` is unbounded on both sides and provably a no-op: see
  // the corridor-window block in `strip_fair_value_core`.
  return strip_fair_value_core(surface, curves, T, cfg, DerivKind::VarSwap, StrikeCorridor{});
}

namespace {

// ── Task F-4 (PV-F4 / FIT-F2 / LIT-7): forward-start variance ────────────
//
// THE rule, in ONE function that both public entries route through. The two
// entries differ only in how they obtain a CurveSet and a surface adapter;
// neither of them owns a line of the forward-variance arithmetic, the
// tolerance policy, or the calendar verdict, so there is no second copy that
// could drift from this one.

// The numerator's noise floor, in TOTAL-VARIANCE units -- the amount by which
// w2 may fall short of w1 while still being indistinguishable from a flat term
// structure. Stated ONCE and consumed by BOTH consumers below (the
// resolvability gate and the calendar verdict), so those two can never
// disagree about what "within accuracy" means.
//
// Two contributions, both named rather than lumped into one magic number:
//   * 2x the library's own calendar accuracy floor (`kCalendarTotalVarianceTol`,
//     types.hpp -- the SAME constant the fit-side no-arb checks measure against,
//     not a second literal). Two legs, either of which may sit AT that floor in
//     the adverse direction, so a surface the fit side calls clean cannot trip
//     the detector here. A tighter bar would fire on good surfaces (FIT-F3).
//   * each leg's OWN reported Richardson quadrature estimate, converted to
//     total-variance units (err*T). MEASURED per call, so a coarse tier widens
//     the band and a fine tier narrows it -- Task F-1's "anchor the gate to
//     integration_error_est, not to a magic constant" rule.
//
// A leg that reported NO estimate (NaN -- a caller-pinned node count off the
// 4m+1 lattice) contributes 0 rather than NaN: the fit term still stands, and
// a NaN floor would poison both consumers into unconditional refusal.
// The forward-start tenor contract, stated ONCE. Called by `forward_var_core`
// below and, before it, by the PricedSurface entry -- which has to reject a
// bad tenor pair BEFORE `carry_from` gets to answer the same question with its
// own (different, and for +Inf wrong-coded) message. A pure function of two
// doubles, so calling it twice cannot produce two answers.
//
// Finiteness is tested FIRST so the `> 0.0` tests below it cannot admit +Inf,
// the exact mistake this sprint shipped once already. 0 carries exactly one
// meaning on these arguments -- an invalid tenor -- so unlike the corridor
// bounds beside them, rejecting it is unambiguous.
[[nodiscard]] Status validate_forward_var_tenors(double T1, double T2) noexcept {
  if (!std::isfinite(T1) || !std::isfinite(T2)) {
    return Err(ErrorCode::InvalidArgument, "forward var: T1 and T2 must be finite");
  }
  if (!(T1 > 0.0)) {
    return Err(ErrorCode::InvalidArgument, "forward var: T1 must be > 0");
  }
  if (!(T2 > T1)) {
    return Err(ErrorCode::InvalidArgument, "forward var: T2 must be > T1");
  }
  return Ok();
}

[[nodiscard]] double forward_var_noise_floor_w(const DerivQuote& leg1, double T1,
                                               const DerivQuote& leg2, double T2) noexcept {
  const auto quad_w = [](const DerivQuote& q, double T) noexcept {
    const double err = q.integration_error_est;
    return std::isfinite(err) ? std::fabs(err) * T : 0.0;
  };
  return 2.0 * kCalendarTotalVarianceTol + quad_w(leg1, T1) + quad_w(leg2, T2);
}

// `Leg1`/`Leg2` are the two surface adapters. They are DIFFERENT types on no
// path today but are kept separate on purpose: the PricedSurface entry builds
// one strip view PER TENOR (each caches its own forward and carry at its own
// T), and collapsing them to one parameter would invite a future caller to
// hand the same T-cached view to both legs -- which reads correctly and prices
// the far leg through the view's slow fallback.
//
// `shared_cfg` is the SINGLE policy resolution the brief demands: one object,
// named twice, reaching two strips. There is no per-leg config to construct,
// so there is nothing for a second resolution to diverge from. What differs
// per tenor is only what `strip_fair_value_core` derives from T itself -- the
// adaptive span and the node budget -- which is exactly right.
template <class Leg1, class Leg2>
[[nodiscard]] Result<DerivQuote>
forward_var_core(const Leg1& leg1_surface, const Leg2& leg2_surface, const CurveSet& curves,
                 double T1, double T2, const DerivConfig& shared_cfg,
                 DerivQuote* diagnostic_out) {
  // The out-parameter contract (see the header): assigned on EVERY return
  // path. Default-constructing it here is what makes that true for the
  // argument-validation returns below without repeating the write at each one.
  if (diagnostic_out != nullptr) {
    *diagnostic_out = DerivQuote{};
  }
  ATX_TRY_VOID(validate_forward_var_tenors(T1, T2));

  // ONE config object, both legs. Not two configs asserted equal.
  ATX_TRY(const DerivQuote leg1, var_swap_fair_strike(leg1_surface, curves, T1, shared_cfg));
  ATX_TRY(const DerivQuote leg2, var_swap_fair_strike(leg2_surface, curves, T2, shared_cfg));

  // Total variance is additive in time (LIT-7): w(T) = K_var(T)*T.
  const double w1 = leg1.fair_strike_dec * T1;
  const double w2 = leg2.fair_strike_dec * T2;
  const double dT = T2 - T1;
  const double dw = w2 - w1;
  const double noise_w = forward_var_noise_floor_w(leg1, T1, leg2, T2);
  // The floor propagated into K_fwd's own units -- what the caller reads as
  // this quote's `integration_error_est`, and the yardstick both the gate
  // immediately below and the calendar verdict after it are stated against.
  const double noise_var = noise_w / dT;

  DerivQuote out{};
  out.leg_T1_var_dec = leg1.fair_strike_dec;
  out.leg_T2_var_dec = leg2.fair_strike_dec;
  out.flags = leg1.flags | leg2.flags;
  out.integration_error_est = noise_var;
  // Grid provenance is the T2 LEG's: the two legs genuinely resolve different
  // grids (that is the point of per-tenor node budgets), so there is no single
  // grid to report. `leg_T2_var_dec` plus a direct `var_swap_fair_strike` at
  // T2 with the same config reproduces this leg exactly; the T1 leg is
  // recovered the same way.
  out.strip_k_lo_used = leg2.strip_k_lo_used;
  out.strip_k_hi_used = leg2.strip_k_hi_used;
  out.strip_nodes_used = leg2.strip_nodes_used;
  out.resolved_wing_clamp = leg2.resolved_wing_clamp;

  const auto publish = [&](const DerivQuote& q) noexcept {
    if (diagnostic_out != nullptr) {
      *diagnostic_out = q;
    }
  };

  // A leg can return Ok and still carry a non-finite strike only through a
  // surface pathology the strip's own guards did not name; if that ever
  // happens, `dw` is NaN and BOTH tests below are false, so the clamp branch
  // would serve a plausible-looking 0.0. Refuse instead -- the same reasoning
  // as the corridor's "0.0 would be a plausible wrong number" refusal.
  if (!std::isfinite(w1) || !std::isfinite(w2)) {
    publish(out);
    return Err(ErrorCode::Internal,
               "forward var: a leg strip produced a non-finite total variance");
  }

  // Catastrophic cancellation, guarded where it bites: `dw` subtracts two
  // nearly-equal totals and `dT` divides by a small number, so the floor above
  // is amplified by exactly 1/dT. Refuse when the amplified floor passes what
  // this entry is willing to call a variance. `!(x <= y)` rather than `x > y`
  // so a NaN noise floor refuses instead of sailing through.
  if (!(noise_var <= kFwdVarNoiseCeilingVar)) {
    publish(out);
    return Err(ErrorCode::OutOfRange,
               "forward var: tenor separation too small to resolve K_fwd above the "
               "legs' own accuracy");
  }

  if (dw < -noise_w) {
    // Calendar-arbitrageable at STRIP level: the T2 smile prices less total
    // variance than the T1 smile by more than either leg's accuracy explains.
    // Fail loud, and publish the raw (negative) quotient rather than a clamped
    // one -- on this path the number IS the evidence.
    out.flags |= DerivFlags::CalendarInconsistent;
    out.fair_strike_dec = dw / dT;
    out.fair_strike_points = 1.0e4 * out.fair_strike_dec;
    out.undiscounted_expectation_dec = out.fair_strike_dec;
    out.uncapped_var_dec = dw;
    publish(out);
    return Err(ErrorCode::Internal,
               "forward var: negative forward variance (calendar-inconsistent surface)");
  }

  // Within the floor a negative numerator is indistinguishable from a flat
  // term structure, and a flat long end genuinely yields ZERO forward variance
  // (FIT-C8) -- so serve exactly 0.0 rather than a small negative variance no
  // caller can take a square root of. Continuous at dw == 0, and the two
  // published legs let a caller recompute the raw quotient if it wants it.
  const double k_fwd = dw > 0.0 ? dw / dT : 0.0;
  out.fair_strike_dec = k_fwd;
  out.fair_strike_points = 1.0e4 * k_fwd;
  out.undiscounted_expectation_dec = k_fwd;
  out.uncapped_var_dec = k_fwd * dT;  // the forward TOTAL variance over [T1, T2]
  publish(out);
  return Ok(out);
}

}  // namespace

template <class SurfaceT>
Result<DerivQuote> forward_var_fair_strike(const SurfaceT& surface, const CurveSet& curves,
                                           double T1, double T2, const DerivConfig& cfg,
                                           DerivQuote* diagnostic_out) {
  return forward_var_core(surface, surface, curves, T1, T2, cfg, diagnostic_out);
}

namespace {

// Task F-2 fix round 2 (C-3/C-4 Critical): THE anchor invariant, stated once.
// `RealizedVarianceSpec::rv_gamma_done_dec` is denominated in SEED-ANCHORED
// units -- Lee's weight w(y) = S_i/S_seed, S_seed = `gamma_seed_spot`. A
// quantity `k_other_anchored` computed or realized at some OTHER spot (the
// future-leg strip, evaluated at `curves.spot`; an injected synthetic
// fixing, evaluated at whatever spot resolved it) is expressed in THAT
// spot's units and is not commensurable with the gamma leg until rescaled by
// `other_spot / seed_spot`.
//
// This is PREDICATE-FREE: the conversion holds regardless of n_obs_done or
// n_obs_total. Fix round 1 closed C-1/C-2 by gating the rescale on
// `genuinely_mixing` (0 < n_obs_done < n_obs_total), a REGIME predicate --
// which excluded n_obs_done == 0 with a live anchor (C-3, the state a
// tracker-driven contract occupies between its seed observe() and its first
// return fixing) and the injected carry-theta fixing itself (C-4). Every
// site in this file that combines an other-anchored quantity with the
// seed-anchored gamma leg must route through this function -- grep this
// name, not `genuinely_mixing`, before adding a new one.
//
// Pure conversion, no validation: callers own their own error handling,
// since one (`price_gamma_swap`) can fail loud (`Result<DerivQuote>`) and
// the other (`inject_carry_fixing`) cannot (`noexcept`, returns by value --
// its malformed-anchor case is left for the downstream reprice to reject).
[[nodiscard]] double gamma_anchor_rescale(double k_other_anchored, double other_spot,
                                          double seed_spot) noexcept {
  return (other_spot / seed_spot) * k_other_anchored;
}

// Task F-2 cleanup round (m-9): the PREDICATE half of the same invariant --
// is `spot` usable as a gamma-leg anchor at all (a divisor `gamma_anchor_
// rescale` can safely take, or a value `RealizedVarianceSpec::gamma_seed_
// spot` can safely hold)? Named once, alongside `gamma_anchor_rescale`, for
// the identical reason that function was: three call sites had each grown
// their OWN copy of "is this a valid anchor" (`price_gamma_swap`'s guard,
// its rescale condition, and `inject_carry_fixing`'s injection), and one of
// the three copies -- the injection -- disagreed, checking `> 0.0` alone
// and admitting `+Inf` (whose rescale factor divides to 0.0, silently
// zeroing the injected fixing instead of being caught here, at the one
// place that should decide). A round whose whole thesis is "state the
// invariant once, route every site through it" cannot leave its own
// predicate stated three different ways.
[[nodiscard]] bool gamma_anchor_valid(double spot) noexcept {
  return std::isfinite(spot) && spot > 0.0;
}

// Task F-2 (PV-F1 / LIT-7): gamma-swap aged-blend dispatch. Mirrors
// `price_var_swap` above field for field -- the brief's "identical dispatch
// structure to VarSwap" -- with exactly three differences:
//   - the strip call goes straight to `strip_fair_value_core(..., DerivKind::
//     GammaSwap)` (no public `gamma_swap_fair_strike` entry point exists;
//     see the header note on `DerivKind::GammaSwap`), rather than through a
//     public wrapper.
//   - the accrued leg reads `rv.rv_gamma_done_dec` (the S_i/S0-weighted
//     accumulator, Task F-2's `RealizedVarianceSpec` append), not
//     `rv.rv_done_dec` -- `aged_total_variance_dec` itself is unchanged and
//     unaware which "variance" it is blending, exactly as the brief states
//     ("the linear-in-variance blend applies to the gamma-weighted variance
//     identically").
//   - `DerivDiscreteCorrection::Diffusion1OverN` is REJECTED (NotImplemented)
//     rather than silently applied or silently ignored: Broadie-Jain's
//     addend is derived for the PLAIN realized-variance estimator VarSwap's
//     future leg is, and there is no re-derivation of the analogous
//     discrete-monitoring correction for the S_i/S0-weighted gamma estimator
//     in this task's scope. Silently applying VarSwap's addend to K_gamma's
//     future leg would be a wrong number; silently ignoring the caller's
//     request would be the P-4-class defect this sprint keeps finding. A
//     loud, explicit NotImplemented is the same remedy this sprint already
//     chose for an analogous scope gap (P-4's AnalyticStrip exclusion).
template <class SurfaceT>
[[nodiscard]] Result<DerivQuote> price_gamma_swap(const SurfaceT& surface,
                                                   const CurveSet& curves,
                                                   const DerivContract& contract,
                                                   const DerivConfig& cfg) {
  const RealizedVarianceSpec& rv = contract.rv_spec;
  const double T = contract.maturity_t;

  if (cfg.discrete_correction_mode == DerivDiscreteCorrection::FullMc) {
    return Err(ErrorCode::NotImplemented, "FULL_MC discrete correction reserved");
  }
  if (cfg.discrete_correction_mode == DerivDiscreteCorrection::Diffusion1OverN) {
    return Err(ErrorCode::NotImplemented,
               "Diffusion1OverN discrete correction is not derived for gamma swaps");
  }

  double k_gamma_future_dec = 0.0;
  DerivQuote strip_quote{};
  bool strip_ran = false;
  DerivFlags flags = DerivFlags::None;

  if (rv.n_obs_total == 0u || rv.n_obs_done < rv.n_obs_total) {
    if (!(T > 0.0)) {
      return Err(ErrorCode::InvalidArgument,
                 "gamma swap needs T > 0 to price the future leg");
    }
    ATX_TRY(auto sq, strip_fair_value_core(surface, curves, T, cfg, DerivKind::GammaSwap,
                                           StrikeCorridor{}));
    strip_quote = sq;
    k_gamma_future_dec = strip_quote.fair_strike_dec;
    strip_ran = true;
  }

  // C-1/C-3 Critical (Task F-2 fix rounds 1-2): FAIL LOUD, unchanged in
  // spirit from round 1 -- when the accrued leg has genuine nonzero weight
  // (0 < n_obs_done < n_obs_total, so `aged_total_variance_dec` will actually
  // read `rv.rv_gamma_done_dec`) there MUST be an anchor to interpret it, or
  // blending is a guess. This predicate is legitimately regime-gated: only
  // real accrual creates the NEED for an anchor. It does NOT gate the
  // rescale below -- round 1's bug was using this same predicate for that
  // too (C-3, closed in 24d0342): a tracker-seeded contract
  // with n_obs_done == 0 has a LIVE anchor (RealizedTracker::observe's seed
  // call writes it before any return is realized) that this guard, being
  // false there, never even inspects.
  const bool needs_anchor_to_blend =
      rv.n_obs_done > 0u && rv.n_obs_total > 0u && rv.n_obs_done < rv.n_obs_total;
  if (needs_anchor_to_blend && !gamma_anchor_valid(rv.gamma_seed_spot)) {
    return Err(ErrorCode::InvalidArgument,
               "aged gamma swap needs a finite rv_spec.gamma_seed_spot > 0 to "
               "blend the accrued leg (anchored at the seed spot) with the "
               "future leg (anchored at curves.spot) on a common anchor");
  }

  // C-1/C-3: the invariant (`gamma_anchor_rescale`'s own comment), applied
  // WITHOUT a regime gate -- whenever the future leg ran (`strip_ran`) and an
  // anchor exists (`gamma_anchor_valid`, m-9 cleanup round -- isfinite, not
  // just `> 0.0`), rescale it onto that anchor, regardless of n_obs_done.
  // This is what covers n_obs_done == 0 with a live
  // anchor (C-3): reviewer's fixture (seed 100, `RealizedTracker::observe`
  // called once, zero returns realized yet, priced at spot 120, sigma=20%,
  // zero carry, T=0.5, N=1e6) -- unrescaled fair_strike_dec =
  // 0.040000000391266097, correct = 0.048000000469519320, a SILENT 16.67% /
  // $8,000 error at spot 120 (33.3% / $20,000 at spot 150), larger than the
  // original C-1 because w_future is 1.0 here, not 0.6. When no anchor
  // exists (a truly virgin contract, gamma_seed_spot == 0.0), there is
  // nothing to rescale onto and none is needed: the future leg IS the whole
  // answer in that state (`aged_total_variance_dec`'s own n_done == 0 /
  // n_total == 0 branches already return it verbatim).
  //
  // `curves.spot > 0` is guaranteed here already (m-5, round-1 review):
  // `strip_ran` implies `strip_fair_value_core` already rejected
  // `!(curves.spot > 0.0)` before returning, so re-checking it here was dead
  // code -- removed rather than kept as a defensive no-op.
  double k_gamma_for_blend = k_gamma_future_dec;
  if (strip_ran && gamma_anchor_valid(rv.gamma_seed_spot)) {
    k_gamma_for_blend = gamma_anchor_rescale(k_gamma_future_dec, curves.spot, rv.gamma_seed_spot);
  }

  const double total = aged_total_variance_dec(rv.rv_gamma_done_dec, k_gamma_for_blend,
                                               rv.n_obs_done, rv.n_obs_total);

  const double df = deriv_df_at_T(curves, T, flags);
  const double pv = df * contract.notional * (total - contract.strike_dec);

  flags |= strip_quote.flags;
  if (rv.n_obs_done > 0u) {
    flags |= DerivFlags::Aged;
  }
  if (rv.n_obs_total > 0u && rv.n_obs_done >= rv.n_obs_total) {
    flags |= DerivFlags::FullyAged;
  }

  double w_done = 0.0;
  double w_future = 1.0;
  if (rv.n_obs_total > 0u) {
    w_done = static_cast<double>(rv.n_obs_done) / static_cast<double>(rv.n_obs_total);
    w_future = 1.0 - w_done;
  }

  DerivQuote out{};
  out.fair_strike_dec = total;  // fair strike that prices the contract to PV = 0
  out.fair_strike_points = 1.0e4 * total;
  out.pv = pv;
  out.undiscounted_expectation_dec = total;
  out.uncapped_var_dec = strip_ran ? strip_quote.uncapped_var_dec : 0.0;
  out.accrued_component_dec = w_done * rv.rv_gamma_done_dec;
  // C-1: use the SAME rescaled value the blend above actually used, so this
  // field is consistent with `out.fair_strike_dec` (accrued_component_dec +
  // future_component_dec == total, as documented) -- `uncapped_var_dec`
  // below stays RAW/unrescaled, matching its own documented "no accrued leg,
  // just the strip's own quote" semantics (it is not part of this blend).
  out.future_component_dec = w_future * k_gamma_for_blend;
  out.convexity_adjustment_dec = 0.0;
  out.integration_error_est = strip_quote.integration_error_est;
  carry_strip_grid(out, strip_quote);
  out.flags = flags;
  return Ok(out);
}

// Task F-3: the CONDITIONAL corridor variance realized to date -- the same
// accrual `rv_corridor_done_dec` holds, re-normalized by the number of fixings
// that were actually inside the corridor instead of by the number observed.
// See `DerivQuote::conditional_corridor_var_dec` for the full contract,
// including what this deliberately is NOT (a forward-looking conditional
// strike). NaN when nothing has been inside the corridor: there is no
// conditional average of an empty set, and 0.0 would read as "flat in there".
[[nodiscard]] double conditional_corridor_accrued_dec(const RealizedVarianceSpec& rv) noexcept {
  if (rv.n_obs_in_corridor == 0u) {
    return kNaN;
  }
  return rv.rv_corridor_done_dec * static_cast<double>(rv.n_obs_done) /
         static_cast<double>(rv.n_obs_in_corridor);
}

// Cross-field consistency of the corridor accrual. Unlike the gamma leg -- for
// which F-2 could demand a witness field (`gamma_seed_spot`) whose absence
// proves the spec was never populated -- a corridor spec has NO value that
// distinguishes "not populated" from the perfectly legitimate "spot never
// entered the corridor": both are all-zeros. What CAN be checked is the
// arithmetic relationship the tracker maintains, and these three are exactly
// the ones a hand-built or half-migrated spec breaks:
//   * n_obs_in_corridor <= n_obs_done -- a subset cannot outnumber the whole;
//   * rv_corridor_done_dec finite and >= 0 -- it is annualization * a sum of
//     squares / a positive count;
//   * n_obs_in_corridor == 0 => rv_corridor_done_dec == 0 EXACTLY -- the sum
//     is over an empty set, so nothing else is representable. This is the one
//     that catches "the caller populated rv_done_dec and forgot the corridor
//     fields, then set the corridor leg by hand".
[[nodiscard]] Status validate_corridor_accrual(const RealizedVarianceSpec& rv) noexcept {
  if (rv.n_obs_in_corridor > rv.n_obs_done) {
    return Err(ErrorCode::InvalidArgument,
               "corridor swap: rv_spec.n_obs_in_corridor exceeds n_obs_done");
  }
  if (!std::isfinite(rv.rv_corridor_done_dec) || rv.rv_corridor_done_dec < 0.0) {
    return Err(ErrorCode::InvalidArgument,
               "corridor swap: rv_spec.rv_corridor_done_dec must be finite and >= 0");
  }
  if (rv.n_obs_in_corridor == 0u && rv.rv_corridor_done_dec != 0.0) {
    return Err(ErrorCode::InvalidArgument,
               "corridor swap: rv_spec.rv_corridor_done_dec is non-zero with no "
               "in-corridor observations");
  }
  return Ok();
}

// Task F-3 (PV-F3 / LIT-7): corridor-variance-swap dispatch. Mirrors
// `price_var_swap` field for field, with exactly three differences:
//   - the strip call carries the contract's corridor into
//     `strip_fair_value_core`, which restricts the integration window to it
//     (no public `corridor_var_swap_fair_strike` entry point exists, mirroring
//     GammaSwap's own arrangement).
//   - the accrued leg reads `rv.rv_corridor_done_dec`, the in-corridor
//     accumulator, not `rv.rv_done_dec`. `aged_total_variance_dec` is unchanged
//     and unaware which "variance" it blends -- the n_done/n_total weighting is
//     correct for the corridor leg precisely BECAUSE the accumulator is
//     normalized by n_done rather than by the in-corridor count (see that
//     field's own doc).
//   - `DerivDiscreteCorrection::Diffusion1OverN` is REJECTED, for the same
//     reason GammaSwap rejects it: Broadie-Jain's addend is derived for the
//     plain, always-counting realized-variance estimator, and no re-derivation
//     for an indicator-gated estimator exists in this task's scope. Applying
//     VarSwap's addend anyway would be a wrong number; ignoring the caller's
//     request would be the silent-scope defect P-4 shipped once already.
template <class SurfaceT>
[[nodiscard]] Result<DerivQuote> price_corridor_var_swap(const SurfaceT& surface,
                                                          const CurveSet& curves,
                                                          const DerivContract& contract,
                                                          const DerivConfig& cfg) {
  const RealizedVarianceSpec& rv = contract.rv_spec;
  const double T = contract.maturity_t;
  const StrikeCorridor corridor{contract.corridor_lo, contract.corridor_hi};

  if (cfg.discrete_correction_mode == DerivDiscreteCorrection::FullMc) {
    return Err(ErrorCode::NotImplemented, "FULL_MC discrete correction reserved");
  }
  if (cfg.discrete_correction_mode == DerivDiscreteCorrection::Diffusion1OverN) {
    return Err(ErrorCode::NotImplemented,
               "Diffusion1OverN discrete correction is not derived for corridor swaps");
  }
  if (!corridor_valid(corridor)) {
    return Err(ErrorCode::InvalidArgument,
               "corridor swap needs finite corridor_lo/corridor_hi >= 0 (0 == "
               "unbounded on that side) with corridor_lo < corridor_hi when both "
               "are bounded");
  }
  ATX_TRY_VOID(validate_corridor_accrual(rv));

  double k_corridor_future_dec = 0.0;
  DerivQuote strip_quote{};
  bool strip_ran = false;
  DerivFlags flags = DerivFlags::None;

  if (rv.n_obs_total == 0u || rv.n_obs_done < rv.n_obs_total) {
    if (!(T > 0.0)) {
      return Err(ErrorCode::InvalidArgument,
                 "corridor swap needs T > 0 to price the future leg");
    }
    ATX_TRY(auto sq, strip_fair_value_core(surface, curves, T, cfg,
                                           DerivKind::CorridorVarSwap, corridor));
    strip_quote = sq;
    k_corridor_future_dec = strip_quote.fair_strike_dec;
    strip_ran = true;
  }

  const double total = aged_total_variance_dec(rv.rv_corridor_done_dec, k_corridor_future_dec,
                                               rv.n_obs_done, rv.n_obs_total);

  const double df = deriv_df_at_T(curves, T, flags);
  const double pv = df * contract.notional * (total - contract.strike_dec);

  flags |= strip_quote.flags;
  if (rv.n_obs_done > 0u) {
    flags |= DerivFlags::Aged;
  }
  if (rv.n_obs_total > 0u && rv.n_obs_done >= rv.n_obs_total) {
    flags |= DerivFlags::FullyAged;
  }

  double w_done = 0.0;
  double w_future = 1.0;
  if (rv.n_obs_total > 0u) {
    w_done = static_cast<double>(rv.n_obs_done) / static_cast<double>(rv.n_obs_total);
    w_future = 1.0 - w_done;
  }

  DerivQuote out{};
  out.fair_strike_dec = total;  // fair strike that prices the contract to PV = 0
  out.fair_strike_points = 1.0e4 * total;
  out.pv = pv;
  out.undiscounted_expectation_dec = total;
  out.uncapped_var_dec = strip_ran ? strip_quote.uncapped_var_dec : 0.0;
  out.accrued_component_dec = w_done * rv.rv_corridor_done_dec;
  out.future_component_dec = w_future * k_corridor_future_dec;
  out.convexity_adjustment_dec = 0.0;
  out.integration_error_est = strip_quote.integration_error_est;
  out.conditional_corridor_var_dec = conditional_corridor_accrued_dec(rv);
  carry_strip_grid(out, strip_quote);
  out.flags = flags;
  return Ok(out);
}

}  // namespace

// ── Carr-Lee volatility strike ─────────────────────────────────────────────

template <class SurfaceT>
Result<DerivQuote> vol_swap_fair_strike(const SurfaceT& surface,
                                        const CurveSet& curves, double T,
                                        const DerivConfig& cfg) {
  if (!(T > 0.0)) {
    return Err(ErrorCode::InvalidArgument, "vol strip needs T > 0");
  }
  if (!reserved_fields_clean(cfg)) {
    return Err(ErrorCode::NotImplemented, "reserved config field is non-zero");
  }
  if (!vol_of_vol_valid(cfg)) {
    return Err(ErrorCode::InvalidArgument, "vol_of_vol must be >= 0");
  }
  if (!wing_clamp_valid(cfg)) {
    return Err(ErrorCode::InvalidArgument, "wing_clamp_k must not be NaN");
  }
  // Review fix I-4: validated defensively here too, mirroring wing_clamp_valid
  // beside it -- the Naive Carr-Lee path below never runs a strip and so never
  // consults this, but the Refined path delegates to var_swap_fair_strike,
  // which does, and a bad band should fail loud at THIS entry rather than only
  // the one branch that happens to use it.
  if (!surface_certified_wing_band_valid(surface_certified_wing_band(surface))) {
    return Err(ErrorCode::InvalidArgument,
               "surface_certified_wing_band must be unset or a finite, positive half-band");
  }

  // K_vol ~= sqrt(2 pi / T) * C_ATMF / (F * df) — shared with
  // resolve_vol_of_vol's auto-calibration path so the two never drift.
  ATX_TRY(const double k_vol_naive, carr_lee_k_vol(surface, curves, T));

  DerivQuote out{};
  out.pv = 0.0;
  out.flags = DerivFlags::VolCarrLee;

  // Task C-5: Refined form needs the strip's own K_var (Remark 6.4/6.5), so
  // unlike Naive this branch pays for one var_swap_fair_strike evaluation --
  // an opt-in cost, never paid by a Naive (default) caller. A strip failure
  // here propagates (ATX_TRY): the caller explicitly asked for the
  // strip-dependent form, so a surface the strip cannot integrate is this
  // call's failure too, not a silent fall-back to the naive number.
  if (cfg.carr_lee_form == CarrLeeForm::Refined) {
    ATX_TRY(auto strip, var_swap_fair_strike(surface, curves, T, cfg));
    const double k_vol =
        detail::refine_carr_lee_k_vol(k_vol_naive, strip.fair_strike_dec, T);
    out.fair_strike_dec = k_vol;
    out.fair_strike_points = 1.0e2 * k_vol;
    out.undiscounted_expectation_dec = k_vol;
    out.uncapped_var_dec = strip.uncapped_var_dec;
    out.convexity_adjustment_dec =
        std::sqrt(std::fmax(strip.uncapped_var_dec, 0.0)) - k_vol;
    out.integration_error_est = strip.integration_error_est;
    carry_strip_grid(out, strip);
    out.flags |= strip.flags;
    return Ok(out);
  }

  out.fair_strike_dec = k_vol_naive;
  out.fair_strike_points = 1.0e2 * k_vol_naive;
  out.undiscounted_expectation_dec = k_vol_naive;
  out.uncapped_var_dec = 0.0;  // not computed in this entry
  out.convexity_adjustment_dec = 0.0;
  out.integration_error_est = kNaN;  // no strip runs here; NaN = not estimated
  return Ok(out);
}

// ── Unified dispatch ───────────────────────────────────────────────────────

namespace {

// Task F-3 fix round 1 (C-1 Critical, "instance nine"): THE dispatch-level
// validation, stated ONCE.
//
// WHY THIS FUNCTION EXISTS AT ALL. It used to be two hand-synchronised copies:
// this logic inline in `deriv_price`, and `validate_var_swap_dispatch` (below)
// re-deriving it for the P-6 book-memo lane, held together by a comment
// declaring that it "mirrors" the original. F-3 added a new dispatch-level
// rule -- corridor bounds are illegal on a non-corridor kind -- to the first
// copy only, and the review measured the consequence: the SAME VarSwap
// contract carrying `corridor_lo`/`corridor_hi` is InvalidArgument through
// `deriv_price_on_ref` and a silently-uncorridored 0.093403309340219273
// through `price_deriv_book`'s memo lane, against 0.036525125893640986 for
// the bounds it names -- 2.56x, with WHICH behaviour you get decided by
// `cfg.discrete_correction_mode`, a book-wide knob unrelated to corridors.
//
// PASTING THE MISSING RULE INTO THE SECOND COPY WOULD HAVE BEEN THE WRONG FIX.
// That is the predicate-not-invariant move F-2 made twice, and both times it
// relocated the defect into the regime the predicate excluded rather than
// closing it. Two copies is what produced this bug; three lines of pasted
// agreement leaves the same trap armed for F-4..F-9. So the two lanes now
// CALL THE SAME FUNCTION, which makes divergence impossible by construction
// rather than merely detectable -- strictly stronger than the compile-time
// tripwire the m-6 arity pin gave us, because there is no second expression
// left that could drift.
//
// SCOPE, precisely. Everything here is a pure function of `(contract.kind,
// contract's scope-gated fields, cfg)` -- no surface, no curves, no
// quadrature. That is exactly the set both lanes owe, and it is why the leaf
// entries (`strip_fair_value_core`, `vol_swap_fair_strike`) are NOT part of
// this mirror: they take no `DerivContract` at all, so the contract-field
// rules are inapplicable to them by signature, not by inspection. They keep
// their own `reserved_fields_clean`/`vol_of_vol_valid` checks because they are
// separately reachable as public entry points.
//
// ORDERING NOTE (a deliberate, tiny behaviour change): the corridor rule now
// runs BEFORE the per-kind engine rejection on the memo lane, matching
// `deriv_price`'s own precedence. A VarSwap contract that is malformed in BOTH
// ways at once -- a corridor AND `VolCarrLee` -- now reports the corridor
// error on both lanes instead of two different ones. Making the two lanes
// agree is the entire point.
// The KIND axis of `DerivEngine::RvDistributionProxy`'s admission, stated once
// and exhaustively. Task F-5 turned this from a hand-written three-term
// `kind !=` chain into a switch for the reason the census gave it top billing:
// the chain routed every future enumerator to "reserved pricing engine", which
// is a safe default for a kind that has some OTHER engine and a silent dead end
// for a kind whose ONLY engine is this one -- which is precisely what F-5's own
// two kinds are. `-Wswitch -WX` now forces the choice instead of making it.
[[nodiscard]] constexpr bool rv_distribution_prices_kind(DerivKind kind) noexcept {
  switch (kind) {
  case DerivKind::VolSwap:
  case DerivKind::CappedVarSwap:
  case DerivKind::CappedVolSwap:
  case DerivKind::VarianceCall:
  case DerivKind::VariancePut:
    return true;
  case DerivKind::VarSwap:
  case DerivKind::GammaSwap:
  case DerivKind::CorridorVarSwap:
    return false;
  }
  return false;  // out-of-enum value: reserved, matching the `!=` chain replaced
}

[[nodiscard]] Status validate_deriv_dispatch(const DerivContract& contract,
                                              const DerivConfig& cfg) {
  // Reserved engines fail clean before any work. RvDistributionProxy is the
  // one exception: Task 4 wires it up (alongside Auto) as the distribution
  // model's entry point for CappedVarSwap, Task 5 adds CappedVolSwap, Task 6
  // adds plain VolSwap (mid-life, and an unaged contract priced end to end
  // through the model instead of Carr-Lee), and Task F-5 adds the two variance
  // OPTION kinds -- every other kind still sees it as reserved.
  //
  // The kind axis here is an if-chain, not a switch, so -Wswitch cannot police
  // it: a new enumerator falls to `NotImplemented`, which is the safe default
  // but is also silently WRONG for any kind whose only engine is this one.
  // F-5's kinds are exactly that case, which is why the census flagged this
  // line as the one that would have made the feature's own engine unreachable.
  switch (cfg.engine) {
  case DerivEngine::RvDistributionProxy:
    if (!rv_distribution_prices_kind(contract.kind)) {
      return Err(ErrorCode::NotImplemented, "reserved pricing engine");
    }
    break;
  case DerivEngine::RvDistributionAffine:
  case DerivEngine::McQe:
    return Err(ErrorCode::NotImplemented, "reserved pricing engine");
  case DerivEngine::Auto:
  case DerivEngine::StripLogContract:
  case DerivEngine::VolCarrLee:
    break;
  }

  // Reject any non-zero reserved field before dispatch.
  if (!reserved_fields_clean(cfg)) {
    return Err(ErrorCode::NotImplemented, "reserved config field is non-zero");
  }
  if (!vol_of_vol_valid(cfg)) {
    return Err(ErrorCode::InvalidArgument, "vol_of_vol must be >= 0");
  }

  // cap_dec validation applies uniformly to both capped kinds: a malformed
  // contract should fail the same way regardless of which capped product it
  // names. Uncapped kinds (VarSwap/VolSwap/GammaSwap/CorridorVarSwap) must
  // leave cap_dec at 0.
  if (deriv_kind_is_capped(contract.kind)) {
    if (!(contract.cap_dec > 0.0)) {
      return Err(ErrorCode::InvalidArgument, "capped kind needs cap_dec > 0");
    }
  } else if (contract.cap_dec != 0.0) {
    return Err(ErrorCode::InvalidArgument, "cap_dec is only valid on capped kinds");
  }

  // Task F-3: the corridor bounds follow `cap_dec`'s rule exactly -- a knob
  // that names nothing on this kind is a caller error, not a silent no-op.
  // Without this, a corridor set on a VarSwap would price as a plain var swap
  // and the caller would never learn the corridor was dropped, which is the
  // same silent-scope class P-4's C-1 and F-2's C-2 both were. Behaviour-
  // compatible by construction: both fields default to 0.0, so no contract
  // written before they existed can trip it.
  if (contract.kind != DerivKind::CorridorVarSwap &&
      (contract.corridor_lo != 0.0 || contract.corridor_hi != 0.0)) {
    return Err(ErrorCode::InvalidArgument,
               "corridor_lo/corridor_hi are only valid on DerivKind::CorridorVarSwap");
  }

  // Task F-5 (folded): `marking` is the FOURTH instance of the silent-scope
  // class the rule directly above names, and the worst of them, because it was
  // not merely dropped -- it was answered. `DerivMarkingConvention::Cboe
  // VarianceFuture` shipped as a field no executable code anywhere read, so a
  // contract naming CBOE variance-future conventions priced through the
  // parametric OTC strip and returned a confident OTC number. A caller hedging
  // LISTED variance got the wrong marks with no error, no flag, and a header
  // that documented the gap only in prose ("DECLARED, UNENFORCED").
  //
  // NOT kind-gated, unlike `cap_dec` and the corridor bounds above: no kind
  // reads `marking`, so the reserved value is refused on all of them.
  // `NotImplemented`, not `InvalidArgument`, because the contract is well
  // formed and the enumerator is a legal value the library RESERVES -- the same
  // code, message shape, and reasoning as the reserved pricing engines at the
  // top of this function. Behaviour-compatible by construction: the field
  // defaults to `Otc` and no contract that ever priced correctly set anything
  // else.
  if (contract.marking != DerivMarkingConvention::Otc) {
    return Err(ErrorCode::NotImplemented, "reserved marking convention");
  }

  // Kind x engine dispatch matrix (PV-5), second stage. The reserved-engine
  // switch above narrowed `cfg.engine` to what each kind's own arm below can
  // still misuse; each arm rejects the one engine value that survives
  // narrowing but still names no pricing formula for that kind. Exhaustive, no
  // `default:` -- a future DerivKind turns this into a compile error under
  // /W4 /WX rather than silently inheriting someone else's engine matrix.
  // Full matrix: VarSwap -> {Auto, StripLogContract}; VolSwap -> {Auto,
  // VolCarrLee (unaged only -- price_vol_swap itself checks that), Rv
  // DistributionProxy}; CappedVarSwap/CappedVolSwap -> {Auto,
  // RvDistributionProxy}; GammaSwap/CorridorVarSwap -> {Auto,
  // StripLogContract}; VarianceCall/VariancePut (Task F-5) -> {Auto,
  // RvDistributionProxy}, the same pair as the capped kinds and for the same
  // reason -- they price through the same lognormal RV distribution.
  switch (contract.kind) {
  case DerivKind::VarSwap:
    // VarSwap only ever runs the strip -- price_var_swap never reads
    // cfg.engine at all, so an explicit VolCarrLee here used to silently price
    // the strip anyway (VolCarrLee has no variance-swap formula of its own to
    // run instead).
    if (cfg.engine == DerivEngine::VolCarrLee) {
      return Err(ErrorCode::InvalidArgument, "engine cannot price a var swap");
    }
    break;
  case DerivKind::VolSwap:
    // An explicit StripLogContract here used to silently fall through to
    // price_vol_swap's unaged Carr-Lee branch -- the same branch Auto/
    // VolCarrLee take -- because that branch only tests `cfg.engine !=
    // RvDistributionProxy`, not which engine it actually is.
    if (cfg.engine == DerivEngine::StripLogContract) {
      return Err(ErrorCode::InvalidArgument, "engine cannot price a vol swap");
    }
    break;
  case DerivKind::CappedVarSwap:
  case DerivKind::CappedVolSwap:
    if (cfg.engine == DerivEngine::StripLogContract || cfg.engine == DerivEngine::VolCarrLee) {
      return Err(ErrorCode::InvalidArgument, "engine cannot price capped kinds");
    }
    break;
  case DerivKind::GammaSwap:
    if (cfg.engine == DerivEngine::VolCarrLee) {
      return Err(ErrorCode::InvalidArgument, "engine cannot price a gamma swap");
    }
    break;
  case DerivKind::CorridorVarSwap:
    if (cfg.engine == DerivEngine::VolCarrLee) {
      return Err(ErrorCode::InvalidArgument, "engine cannot price a corridor var swap");
    }
    break;
  case DerivKind::VarianceCall:
  case DerivKind::VariancePut:
    if (cfg.engine == DerivEngine::StripLogContract || cfg.engine == DerivEngine::VolCarrLee) {
      return Err(ErrorCode::InvalidArgument, "engine cannot price a variance option");
    }
    break;
  }
  return Ok();
}

}  // namespace

template <class SurfaceT>
Result<DerivQuote> deriv_price(const SurfaceT& surface, const CurveSet& curves,
                               const DerivContract& contract,
                               const DerivConfig& cfg) {
  // Task F-3 fix round 1 (C-1): every dispatch-level rule -- reserved engines,
  // reserved cfg fields, vol_of_vol, the cap_dec and corridor scope rules, and
  // the per-kind engine matrix -- now lives in ONE place that the book-memo
  // lane calls too. See `validate_deriv_dispatch` for why that mattered.
  ATX_TRY_VOID(validate_deriv_dispatch(contract, cfg));

  // Routing only. `validate_deriv_dispatch` above has already rejected every
  // (kind, engine, scope-gated field) combination that names no formula, so
  // each arm here is a pure hand-off to its pricer.
  switch (contract.kind) {
  case DerivKind::VarSwap:
    return price_var_swap(surface, curves, contract, cfg);
  case DerivKind::VolSwap:
    return price_vol_swap(surface, curves, contract, cfg);
  case DerivKind::CappedVarSwap:
    return price_capped_var_swap(surface, curves, contract, cfg);
  case DerivKind::CappedVolSwap:
    return price_capped_vol_swap(surface, curves, contract, cfg);
  case DerivKind::GammaSwap:
    return price_gamma_swap(surface, curves, contract, cfg);
  case DerivKind::CorridorVarSwap:
    return price_corridor_var_swap(surface, curves, contract, cfg);
  case DerivKind::VarianceCall:
  case DerivKind::VariancePut:
    // ONE pricer for both: the two payoffs differ only in which closed form
    // the same (a, b, m, s, K) resolution is fed to. Two arms here and a
    // `kind ==` inside would be a fifth place this file decides what a
    // variance option is.
    return price_variance_option(surface, curves, contract, cfg);
  }
  // Defends against an out-of-enum kind (matches the C default's ERR_INVALID).
  return Err(ErrorCode::InvalidArgument, "unknown derivative kind");
}

// ── Finite-difference greeks ───────────────────────────────────────────────

namespace {

// Task P-3 / GK-P2: per-(shift, T) memoized surface read for the greek bump
// table. `eval_bump_table` pins the strip's own grid identically across every
// bumped evaluation (`pin_center_scheme`, called by its caller `deriv_greeks`
// before the table runs) -- and, as established in `resolve_wing_clamp`'s own
// comments, the pinned wing band and n_nodes/span do not depend on the
// bumped surface's runtime vol either -- so the SAME sequence of (shifted)
// log-moneyness nodes is queried by every evaluation sharing a (k_shift, T)
// pair: {x} (center: pv.c, pv.v_up, pv.v_dn), {x+ks+}/{x+ks-} (spot bumps and
// their vol cross-terms), each again at the rolled T-dt (theta and its
// carry-fixing variants; second-order's t_s_up/t_s_dn). Six distinct
// buckets total, matching GK-P2's "6 distinct read vectors" finding.
//
// Keyed on the EXACT bit pattern of the shifted query AND of T (Review fix
// round 1, I-4 -- see below), not a positional replay, so a slot is correct
// regardless of how many reads a given DerivKind/aging dispatch performs
// inside one `bumped_pv` call, or in what order -- a genuine memoization
// cache, not an order-coupled recording that would silently misindex if two
// dispatch paths read a different number of points. A miss just computes and
// caches; nothing here can return a value for the wrong input.
//
// PV-P4 perf note: the first cut of this cache used `std::unordered_map` and
// MEASURED SLOWER than no caching at all on this Debug/SSE2 preset (paired
// A/B, `deriv/greeks/standard_priced_surface`: ~1.5ms uncached vs ~2.1ms
// cached, a ~35% REGRESSION) -- MSVC's debug-iterator-checked STL makes
// hash-table find/emplace pay far more than the vtable-dispatch-plus-sqrt eSSVI
// read it was meant to avoid. A flat, sorted `vector<Entry>` avoids hashing
// and node-based bucket traversal entirely: recording APPENDS (O(1), no
// lookup -- a within-evaluation duplicate, e.g. the ATM sigma_atm read vs the
// node loop's own k=0 node, just appends twice, which is harmless and far
// cheaper than an O(n) duplicate check on every append); `eval_bump_table`
// calls `finish_recording()` once, exactly when it knows the slot's one
// recording evaluation has completed, sorting by key ONCE (O(n log n));
// every subsequent read for that slot is then an O(log n) binary search over
// contiguous memory. What that bought is a RECOVERY, not a win: re-measured
// paired with the cache toggled as the only variable (6 pairs, same preset),
// cache_off and cache_on both median 4167us -- parity, inside the noise band.
// So this cache earns its place by removing the regression above, and the
// honest claim for it is "no longer slower", NOT "faster".
struct BumpReadCache {
  struct Entry {
    std::uint64_t x_bits;  // bit_cast of the shifted log-moneyness query
    std::uint64_t t_bits;  // Review fix round 1, I-4: bit_cast of T, folded
                           // into the key rather than assumed constant
    double sigma;
  };
  std::vector<Entry> entries;
  bool sorted = false;

  // Review fix round 1, I-3 / I-5: true only for a slot `eval_bump_table`
  // knows has a later replayer (set via `begin_recording` below, right
  // before that slot's one recording evaluation). A slot nothing ever
  // replays against (the two second-order T-dt slots, each fed by exactly
  // one bump) is left at its default `false`: `CachedBumpView::iv`/
  // `iv_batch` then just read the base surface live and never touch
  // `entries` at all, so the two single-use slots pay none of the append
  // cost a previous version of this comment claimed (wrongly) was already
  // free.
  bool recording = false;

  // Called by `eval_bump_table` right before a slot's ONE recording
  // evaluation, for slots with a known later replayer only. `expected_nodes`
  // is the one allocation this cache ever performs: reserving it here, in
  // `eval_bump_table` (not `noexcept`), rather than inside the noexcept
  // `CachedBumpView` methods below, is what makes I-5's `noexcept` honest --
  // every `entries.push_back` during recording then lands inside
  // already-reserved capacity (the pinned grid means every evaluation
  // sharing a slot queries the exact same distinct-node count, so the
  // reservation is never undersized), which cannot reallocate and, for this
  // trivially-constructible `Entry`, cannot throw.
  void begin_recording(std::size_t expected_nodes) {
    recording = true;
    entries.reserve(expected_nodes);
  }

  // Called by `eval_bump_table` immediately after this slot's ONE recording
  // evaluation returns successfully -- every bump that reuses this slot runs
  // strictly after, so no reader ever observes a partially-sorted vector.
  void finish_recording() noexcept {
    // Raw-pointer sort, not `entries.begin()/.end()`: this build's Debug CRT
    // (`/MDd`, see the cpp persona doc) defaults `_ITERATOR_DEBUG_LEVEL=2`,
    // which wraps `vector<T>::iterator` in a checked type carrying a
    // container pointer and generation counter that every increment/
    // dereference/comparison re-validates -- measured (paired A/B, see this
    // struct's own header comment) as SLOWER under this preset than the
    // plain, uninstrumented function-call chain the cache exists to
    // short-circuit. `Entry*` is a raw pointer, not a wrapped container
    // iterator, so sorting/searching through it directly keeps the win real.
    Entry* data = entries.data();
    std::sort(data, data + entries.size(), [](const Entry& a, const Entry& b) noexcept {
      return a.x_bits < b.x_bits || (a.x_bits == b.x_bits && a.t_bits < b.t_bits);
    });
    sorted = true;
  }

  // Hand-rolled binary search over the raw-pointer sorted array (see
  // `finish_recording`'s comment for why not `std::lower_bound` over
  // `entries.begin()/.end()`). Returns {found, sigma}; sigma is the RAW
  // (pre-vol-shift) cached read.
  [[nodiscard]] std::pair<bool, double> find(std::uint64_t x_bits,
                                             std::uint64_t t_bits) const noexcept {
    const Entry* data = entries.data();
    std::size_t lo = 0;
    std::size_t hi = entries.size();
    while (lo < hi) {
      const std::size_t mid = lo + (hi - lo) / 2;
      const bool less =
          data[mid].x_bits < x_bits || (data[mid].x_bits == x_bits && data[mid].t_bits < t_bits);
      if (less) {
        lo = mid + 1;
      } else {
        hi = mid;
      }
    }
    if (lo < entries.size() && data[lo].x_bits == x_bits && data[lo].t_bits == t_bits) {
      return {true, data[lo].sigma};
    }
    // Not found among the recorded reads. Never reached in practice (the
    // pinned grid guarantees every replaying evaluation queries a subset of
    // exactly what the recording evaluation queried, at the same T -- see
    // this struct's own header comment), but callers fall back to a live
    // read rather than mutate an already-sorted vector with an unsorted
    // append, which would break every LATER binary search against it.
    return {false, 0.0};
  }
};

// Task F-8: the ALGEBRA of every bump this file takes -- sticky-strike respot,
// parallel vol shift, and the two smile-shape shifts -- now lives in the public
// `SurfaceOverlay` (surface_overlay.hpp). What stays here is the part that is
// not algebra at all: the shared `BumpReadCache` slot that turns the shifted
// read into a memoized one, and the `is_bumped_greek_view` tag that marks an
// evaluation as a bump. Keeping the cache private is the point of the split --
// it is a three-state recording/replay machine with a `reserve` contract whose
// `noexcept` honesty depends on `eval_bump_table` calling `begin_recording`
// first, and exporting that onto a public view would make a caller able to
// break it.
//
// The cache sits BETWEEN the overlay's two halves: `read_k`/`read_t` produce
// the base query (and so the cache key), and `shift_iv` applies the vol offsets
// AFTER the lookup/store. That ordering is what lets sigma+- share one cached
// read with the centre and the spot bumps for their common (k_shift, T): a miss
// caches the RAW pre-shift read, never a shifted one a differently-shifted bump
// could not reuse. NaN propagates through the shift unchanged, so an unusable
// read needs no special case.
//
// `is_bumped_greek_view` is read at exactly one place (`price_vol_swap`'s
// best-effort convexity diagnostic, which a bumped evaluation skips). Losing it
// would not move a single greek -- it would silently add up to 14 extra strip
// integrations per `deriv_greeks` call on a VolSwap, and no test pins that
// count. That is why it is declared on the type that reaches `deriv_price`
// rather than inferred from the overlay.
template <class SurfaceT>
struct CachedBumpView {
  SurfaceOverlay<SurfaceT> shift;
  BumpReadCache* cache;  // non-owning, non-null; shared across every bump
                         // evaluation querying this (k_shift, T) pair

  // Task P-2 / GK-P: structural marker `is_bumped_greek_view` (above) detects
  // via `requires`. Every construction site is a bump-table or shared-block
  // evaluation (`bump_view`/`skew_bump_view`/`convex_bump_view` below are the
  // only makers, and nothing outside this file can name the type), so the tag
  // exactly identifies a bumped/rolled greek evaluation -- see
  // `is_bumped_greek_view`'s own comment for why this beats a DerivConfig field.
  static constexpr bool is_bumped_greek_view = true;

  [[nodiscard]] double iv(double k_log, double T) const noexcept {
    const double x = shift.read_k(k_log);
    const double t = shift.read_t(T);
    // Task P-3 test/bench seam: forcing this off reproduces the pre-P-3
    // RespotView+VolShiftView composition exactly (always a live read), so
    // `DerivGreeks.ReadCacheMatchesUncached` can prove the cache changes
    // nothing but how many times the surface is actually read.
    if (bump_read_cache_disabled_for_test()) {
      return shift.shift_iv(shift.base->iv(x, t), k_log);
    }
    const std::uint64_t x_bits = std::bit_cast<std::uint64_t>(x);
    const std::uint64_t t_bits = std::bit_cast<std::uint64_t>(t);
    if (cache->sorted) {
      const auto [found, sigma] = cache->find(x_bits, t_bits);
      return shift.shift_iv(found ? sigma : shift.base->iv(x, t), k_log);
    }
    if (!cache->recording) {
      // Review fix round 1, I-3: a slot nothing ever replays against -- read
      // live, do not bother appending to a vector no one will search.
      return shift.shift_iv(shift.base->iv(x, t), k_log);
    }
    // Recording phase: append-only, no lookup -- see `BumpReadCache`'s own
    // comment for why. `push_back` lands in the capacity `begin_recording`
    // already reserved (Review fix round 1, I-5), so this cannot reallocate.
    const double sigma = shift.base->iv(x, t);
    cache->entries.push_back(BumpReadCache::Entry{x_bits, t_bits, sigma});
    return shift.shift_iv(sigma, k_log);
  }

  // Review fix round 1, I-7: forwards the strip's batched read through the
  // cache. Only participates in overload resolution (and so in
  // `has_strip_iv_batch<CachedBumpView<SurfaceT>>()`'s structural detection)
  // when the WRAPPED `SurfaceT` itself has a batched `iv_batch` -- true for
  // `PricedSurfaceStripView`, false for the legacy VolSurface/EssviSurface/
  // SviSurface/SurfaceRefStripView adapters, which never had this member and
  // must keep taking `var_swap_fair_strike`'s scalar path unchanged. Before
  // this, EVERY bumped evaluation exposed no `iv_batch` regardless of what it
  // wrapped, so only the center quote (unwrapped `PricedSurfaceStripView`)
  // ever took the batched node loop -- the 13 bumped strips a full second-
  // order greek block prices took the scalar loop every time, which is why
  // the first paired A/B over Task P-3 (commit 0a63305) measured no
  // significant speedup: the strip batching this task exists to deliver was
  // wired to the ONE evaluation out of 14 that costs least to batch.
  //
  // Uses `out` as scratch for the shifted-x values passed to `base->
  // iv_batch` (mirroring `PricedSurfaceStripView::iv_batch`'s own in-place
  // trick) -- no second fixed-size buffer, so this carries none of C-1's
  // risk regardless of how large a span a future caller hands it; `n` here
  // is bounded by whatever `var_swap_fair_strike`'s own C-1 guard already
  // bounds its gather span to.
  void iv_batch(std::span<const double> x, double T, std::span<double> out) const noexcept
      requires requires(const SurfaceT& s, std::span<const double> xs, double t,
                        std::span<double> o) { s.iv_batch(xs, t, o); }
  {
    const std::size_t n = std::min(x.size(), out.size());
    if (bump_read_cache_disabled_for_test()) {
      // The uncached path IS the overlay, unmediated -- which is what makes
      // `ReadCacheMatchesUncached` a statement about the cache alone.
      shift.iv_batch(x.first(n), T, out.first(n));
      return;
    }
    const std::uint64_t t_bits = std::bit_cast<std::uint64_t>(shift.read_t(T));
    if (cache->sorted) {
      // Replay: the pinned grid means every element is expected to hit, but
      // each is independently searched/verified rather than assumed --
      // never wrong even if that invariant were ever violated, just not
      // batched on a miss (see `BumpReadCache::find`'s own comment).
      const double t = shift.read_t(T);
      for (std::size_t i = 0; i < n; ++i) {
        const double shifted = shift.read_k(x[i]);
        const auto [found, sigma] =
            cache->find(std::bit_cast<std::uint64_t>(shifted), t_bits);
        out[i] = shift.shift_iv(found ? sigma : shift.base->iv(shifted, t), x[i]);
      }
      return;
    }
    if (!cache->recording) {
      // I-3: single-use slot -- one batched live read, nothing cached.
      shift.iv_batch(x.first(n), T, out.first(n));
      return;
    }
    // Recording: ONE batched raw read via `base->iv_batch`, then cache each
    // (key recomputed from the ORIGINAL `x`/`k_shift`, bit-identical to what
    // was written into `out` before the call below) and apply the shift.
    for (std::size_t i = 0; i < n; ++i) {
      out[i] = shift.read_k(x[i]);
    }
    shift.base->iv_batch(out.first(n), shift.read_t(T), out.first(n));  // out[i] is raw sigma
    for (std::size_t i = 0; i < n; ++i) {
      const double shifted = shift.read_k(x[i]);
      cache->entries.push_back(
          BumpReadCache::Entry{std::bit_cast<std::uint64_t>(shifted), t_bits, out[i]});
      out[i] = shift.shift_iv(out[i], x[i]);
    }
  }
};

// The only makers of a `CachedBumpView`. `SurfaceOverlay`'s field ORDER is
// {vol, skew, convexity, k, term}, which is not the order any of the bump call
// sites think in, so they go through designated initializers here rather than
// each spelling out a five-double brace-init that a future field insert would
// silently re-associate.
template <class SurfaceT>
[[nodiscard]] CachedBumpView<SurfaceT> bump_view(const SurfaceT& surface, double k_shift,
                                                 double vol_shift,
                                                 BumpReadCache& cache) noexcept {
  return CachedBumpView<SurfaceT>{
      SurfaceOverlay<SurfaceT>{.base = &surface, .vol_shift = vol_shift, .k_shift = k_shift},
      &cache};
}

// Task F-7 smile bumps. Both run at ZERO spot and vol shift, so the cache slot
// they take is the identity-shift one the centre already recorded.
//
// THE k THESE RECEIVE IS ALREADY WING-CLAMPED. `var_swap_fair_strike` clamps
// each node into the resolved trust band BEFORE calling `iv` (both the scalar
// loop and the gather buffer the batched path fills), so `s*k` saturates at
// `s*wing_band` rather than growing across the wings. That is a real modelling
// decision, documented for callers on `DerivGreeks::skew_vega`; it is also the
// self-consistent one, since the clamped surface is precisely what produced the
// PV being differentiated.
template <class SurfaceT>
[[nodiscard]] CachedBumpView<SurfaceT> skew_bump_view(const SurfaceT& surface, double slope,
                                                      BumpReadCache& cache) noexcept {
  return CachedBumpView<SurfaceT>{SurfaceOverlay<SurfaceT>{.base = &surface, .skew_shift = slope},
                                  &cache};
}

template <class SurfaceT>
[[nodiscard]] CachedBumpView<SurfaceT> convex_bump_view(const SurfaceT& surface, double curvature,
                                                        BumpReadCache& cache) noexcept {
  return CachedBumpView<SurfaceT>{
      SurfaceOverlay<SurfaceT>{.base = &surface, .convexity_shift = curvature}, &cache};
}

// Spot bump: the reference spot and every fitted forward scale together, so the
// bumped world is the same carry seen from a different spot. Yield and
// dividends are untouched.
[[nodiscard]] CurveSet respot_curves(const CurveSet& base, double scale) {
  CurveSet out = base;
  out.spot = base.spot * scale;
  // `out` is a fresh copy from the line above (ForwardCurve has no shared
  // storage -- Rule of Zero over its own std::vector), so this exclusively-
  // owned instance is the only reference in play: the non-const `points()`
  // in-place write handle (see its doc in rates_curve.hpp) cannot alias
  // `base` or any concurrent reader, satisfying the many-readers-or-one-
  // writer contract trivially.
  for (ForwardPoint& p : out.forward.points()) {
    p.F *= scale;
  }
  return out;
}

// One bumped repricing, through the SAME deriv_price every mark goes through.
// `cache` is the (k_shift, T)-keyed `BumpReadCache` slot this evaluation
// shares with every other bump in the table that reads the same shifted
// nodes at the same T -- see `CachedBumpView` and `eval_bump_table`'s own
// six-slot layout below.
// The one place a bump-table evaluation actually reprices, whatever view it
// prices under -- so the Task F-7 repricing counter has exactly one site to
// increment and cannot drift out of step with the table below.
template <class ViewT>
[[nodiscard]] Result<double> pv_under_view(const ViewT& view, const CurveSet& curves,
                                           const DerivContract& contract, const DerivConfig& cfg) {
  count_deriv_greeks_reprice();
  ATX_TRY(const DerivQuote q, deriv_price(view, curves, contract, cfg));
  return Ok(q.pv);
}

template <class SurfaceT>
[[nodiscard]] Result<double> bumped_pv(const SurfaceT& surface, const CurveSet& curves,
                                       const DerivContract& contract, const DerivConfig& cfg,
                                       double k_shift, double vol_shift, BumpReadCache& cache) {
  return pv_under_view(bump_view(surface, k_shift, vol_shift, cache), curves, contract, cfg);
}

// Task F-7: one smile-shape repricing. Both smile bumps run at ZERO spot and
// vol shift, so they take the identity-shift cache slot whose reads
// `cache_c_t0` already recorded for the center -- the four extra evaluations
// therefore cost four strip INTEGRATIONS but no additional surface reads.
template <class SurfaceT>
[[nodiscard]] Result<double> skew_bumped_pv(const SurfaceT& surface, const CurveSet& curves,
                                            const DerivContract& contract, const DerivConfig& cfg,
                                            double slope, BumpReadCache& cache) {
  return pv_under_view(skew_bump_view(surface, slope, cache), curves, contract, cfg);
}

template <class SurfaceT>
[[nodiscard]] Result<double> convex_bumped_pv(const SurfaceT& surface, const CurveSet& curves,
                                              const DerivContract& contract, const DerivConfig& cfg,
                                              double curvature, BumpReadCache& cache) {
  return pv_under_view(convex_bump_view(surface, curvature, cache), curves, contract, cfg);
}

// Injects one additional fixing (n_done -> n_done+1) into a COPY of `rv`,
// realized at annualized decimal rate `fixing_dec` (Task C-10 / GK-C2).
// `fixing_dec` is K_var_future for theta_carry (the fixing lands exactly at
// today's model-free implied variance rate) or 0.0 for theta_zero_fixing (a
// literal zero return).
//
// PRICING ONLY CONSUMES `rv_done_dec` -- `aged_total_variance_dec` and every
// dispatch path read that field directly, never `sum_sq_log_returns_done`
// (confirmed: the raw sum is written by RealizedTracker::observe /
// backtest.cpp / swap_leg.cpp and read back only by those same writers, never
// by any pricer in this file). So the new fixing is folded in as a running-
// mean update directly on rv_done_dec:
//   rv_done_dec' = (n_done*rv_done_dec + fixing_dec) / (n_done + 1)
// which is the EXACT algebraic identity a raw-sum injection of
// `fixing_dec / annualization` (the tracker's own per-fixing conversion --
// rv_done_dec = annualization*sum/n_done, inverted for one fixing) produces
// when `rv.sum_sq_log_returns_done` is itself consistent with `rv.rv_done_dec`
// (the derivation is exactly the algebra above) -- and, unlike accumulating
// forward from the raw sum, does not silently discard a caller-set
// rv_done_dec when sum_sq_log_returns_done was never populated to match it
// (every deriv_greeks test fixture in this file does exactly that: rv_done_dec
// set directly, sum_sq_log_returns_done left at its 0.0 default).
// `sum_sq_log_returns_done` is still updated, back-derived from the NEW
// rv_done_dec so the returned spec stays internally consistent with the
// tracker's own identity, rather than accumulated forward from a raw sum that
// may already have disagreed with rv_done_dec on entry.
// `anchor_spot` (Task F-2 fix round 1 / C-2, additive param): the caller's
// curves.spot, needed only to seed `gamma_seed_spot` when this injection is
// itself the contract's first-ever fixing -- see the comment on that field
// below.
// `corridor` (Task F-3, additive param): the contract's own corridor, needed
// because the injected fixing has to be tested against it exactly as a real
// one would be -- see the corridor block at the end of this function.
[[nodiscard]] RealizedVarianceSpec inject_carry_fixing(const RealizedVarianceSpec& rv,
                                                        double fixing_dec, double anchor_spot,
                                                        const StrikeCorridor& corridor) noexcept {
  RealizedVarianceSpec out = rv;
  const double n0 = static_cast<double>(rv.n_obs_done);
  out.n_obs_done = rv.n_obs_done + 1u;
  out.rv_done_dec = (n0 * rv.rv_done_dec + fixing_dec) / (n0 + 1.0);
  // MUST-FIX 4 (aggregate review): a hand-built DerivContract can set
  // annualization <= 0 without going through RealizedTracker::create's own
  // validation -- this is the first division on this path with none of its
  // own. No pricer in this file reads sum_sq_log_returns_done back (see the
  // comment above), so a bad annualization is otherwise harmless, but
  // writing +-inf/NaN into a caller-visible spec is worse than leaving the
  // field at its ordinary "not populated" 0.0 default, which every fixture
  // in this file already treats as normal.
  out.sum_sq_log_returns_done =
      (rv.annualization > 0.0)
          ? out.rv_done_dec * static_cast<double>(out.n_obs_done) / rv.annualization
          : 0.0;

  // C-2/C-4 Critical (Task F-2 fix rounds 1-2): this function's header
  // comment above claimed pricing consumes only rv_done_dec /
  // sum_sq_log_returns_done -- true until F-2 gave RealizedVarianceSpec a
  // SECOND, per-kind accrued leg (rv_gamma_done_dec /
  // sum_weighted_sq_log_returns_done) that price_gamma_swap reads instead.
  // Leaving those two fields untouched here made theta_carry and
  // theta_zero_fixing bitwise IDENTICAL on every scheduled gamma swap (C-2).
  // Write BOTH legs unconditionally rather than branch on kind -- restoring
  // this function's ORIGINAL "regardless of DerivKind" contract -- since a
  // VarSwap contract never reads rv_gamma_done_dec back (a harmless no-op).
  //
  // C-4 (round 2): `fixing_dec` is resolved at TODAY's spot (`anchor_spot` --
  // it is `k_var_future`, the strip's own future-leg value, or 0.0), but
  // `rv.rv_gamma_done_dec` is denominated at `rv.gamma_seed_spot`. Adding
  // `fixing_dec` in RAW made round 1's own justification here -- "S_new/
  // S_seed treated as 1, a one-step diagnostic" -- wrong: it named the right
  // symbol (S_new/S_seed) and reasoned about a DIFFERENT one (S_new/S_prev,
  // which genuinely is ~1 over one day). S_new/S_seed is the inception-to-
  // today ratio C-1 exists to correct, and is NOT close to 1 whenever spot
  // has moved since inception (0.80-1.25 in the review's own fixture).
  // Route through `gamma_anchor_rescale` (this file's stated invariant)
  // whenever an anchor already exists, regardless of n_obs_done -- covering
  // BOTH the ordinary mid-life case AND a tracker-seeded n_obs_done == 0
  // contract (RealizedTracker::observe's seed call writes gamma_seed_spot
  // before any return is realized, so this injection can land on a LIVE
  // anchor even at n_obs_done == 0). "Exists" is `gamma_anchor_valid` (m-9
  // cleanup round), not a bare `> 0.0` -- this site used to check `> 0.0`
  // alone, admitting `+Inf` (whose rescale factor divides to 0.0, silently
  // zeroing the injected fixing instead of falling back to the unrescaled
  // path below, which is what a genuinely-absent anchor gets). The other
  // two sites this invariant touches (`price_gamma_swap`'s guard and its own
  // rescale condition) already required finiteness; this one now agrees.
  const double fixing_for_gamma = gamma_anchor_valid(rv.gamma_seed_spot)
                                       ? gamma_anchor_rescale(fixing_dec, anchor_spot, rv.gamma_seed_spot)
                                       : fixing_dec;
  out.rv_gamma_done_dec = (n0 * rv.rv_gamma_done_dec + fixing_for_gamma) / (n0 + 1.0);
  out.sum_weighted_sq_log_returns_done =
      (rv.annualization > 0.0)
          ? out.rv_gamma_done_dec * static_cast<double>(out.n_obs_done) / rv.annualization
          : 0.0;
  // The anchor itself: a real mid-life OR tracker-seeded-but-unfixed contract
  // already has one (copied through by `out = rv` above, untouched -- and
  // just used, rescaled, immediately above). The one case that does not is a
  // hand-built spec that was never seeded through a tracker at all (the
  // shape `solve_cycle_swap` produces: n_obs_done == 0 AND gamma_seed_spot ==
  // 0.0) -- where THIS injection effectively IS the contract's first-ever
  // fixing, so it establishes the anchor at `anchor_spot` (the caller's
  // curves.spot; a theta/carry roll never bumps spot, so it is identical to
  // what the center evaluation itself sees -- exactly what a real seed
  // observe() at this same moment would have recorded). Note the rescale
  // above is then a no-op by construction here (`fixing_dec` was resolved AT
  // `anchor_spot`, and this branch only fires when `gamma_seed_spot` was NOT
  // already positive, so the ternary above took the unrescaled path).
  // Deliberately NOT applied when rv.n_obs_done > 0 with no anchor: a
  // mid-life spec with real accrual and no anchor is a genuine caller error,
  // left that way so price_gamma_swap's own guard fails loud on it, rather
  // than silently papering over it here.
  if (rv.n_obs_done == 0u && !(out.gamma_seed_spot > 0.0) && anchor_spot > 0.0) {
    out.gamma_seed_spot = anchor_spot;
  }

  // Task F-3: the THIRD accrued leg, written unconditionally for the same
  // reason the gamma pair above is -- restoring this function's original
  // "regardless of DerivKind" contract, since a VarSwap/GammaSwap contract
  // never reads the corridor leg back (a harmless no-op) while a corridor
  // contract that found these fields untouched would make theta_carry and
  // theta_zero_fixing bitwise IDENTICAL, which is exactly the defect F-2's C-2
  // was.
  //
  // The membership test is `corridor_contains` -- THE rule (see its own
  // comment), not a second expression of it -- applied to the injected
  // fixing's own PREVIOUS CLOSE. That close is `anchor_spot`: this injection
  // models "one more session passes", i.e. tomorrow's return measured FROM
  // today's spot, so today's spot is precisely the level the corridor
  // indicator is predictable with respect to. No regime gate: an unbounded
  // corridor (every non-corridor contract) makes `corridor_contains` true and
  // the corridor leg simply tracks the plain one, which is what an unbounded
  // corridor means.
  //
  // CONSEQUENCE WORTH STATING, because it looks like a bug and is not: when
  // today's spot is OUTSIDE the corridor, both variants inject 0.0 into the
  // corridor leg and theta_carry == theta_zero_fixing BITWISE on a corridor
  // contract. That is the truth of the product -- a fixing outside the
  // corridor contributes nothing no matter what the market does that day -- and
  // not the C-2 symptom it superficially resembles. `Corridor.CarryTheta*`
  // (derivatives_test.cpp) pins BOTH sides of it.
  const bool fixing_in_corridor = corridor_contains(anchor_spot, corridor);
  out.n_obs_in_corridor = rv.n_obs_in_corridor + (fixing_in_corridor ? 1u : 0u);
  out.rv_corridor_done_dec =
      (n0 * rv.rv_corridor_done_dec + (fixing_in_corridor ? fixing_dec : 0.0)) / (n0 + 1.0);
  out.sum_sq_log_returns_in_corridor =
      (rv.annualization > 0.0)
          ? out.rv_corridor_done_dec * static_cast<double>(out.n_obs_done) / rv.annualization
          : 0.0;
  return out;
}

// Fix round 1, CRITICAL-1: does injecting one fixing move `contract` across a
// DISPATCH-ENGINE boundary the CENTER was never in? Every bump above this one
// holds `rv_spec` fixed, so it can only ever land on the same dispatch branch
// the center already priced through; the carry-theta injection is the one
// evaluation in this whole table that changes `n_obs_done`, so it is the one
// that can cross such a boundary.
//
// Exactly one boundary exists in this file today: `price_vol_swap` accepts an
// EXPLICIT `DerivEngine::VolCarrLee` both unaged (n_done == 0) and fully aged
// (n_done >= n_total) -- both exact/closed-form -- but REJECTS it mid-life
// (0 < n_done < n_total), because Carr-Lee has no way to blend an already-
// accrued leg (see that function's own guard). An unaged VolSwap priced under
// an explicit VolCarrLee engine is a documented, previously-working
// configuration (this file's own vol-swap dispatch doc: "UNAGED (n_done ==
// 0): Carr-Lee by default ... or explicit VolCarrLee -- Marquee pins this at
// inception"); injecting one fixing moves it to n_done == 1, which is
// mid-life whenever n_total > 1, and would otherwise fail the WHOLE greek
// block (delta through charm, not just the two carry fields) over an
// opt-in-by-default diagnostic every other bump in the table never triggers.
//
// Detected structurally, not by calling deriv_price and inspecting the error:
// a genuine numeric failure on this same cell (e.g. the fresh
// var_swap_fair_strike call below hitting an unpriceable surface) still
// propagates via the ordinary ATX_TRY calls, exactly like every other
// stencil evaluation -- this predicate only ever turns a would-be
// CONFIGURATION failure into "not computed", never a numeric one.
[[nodiscard]] bool carry_fixing_crosses_engine_boundary(const DerivContract& contract,
                                                         const DerivConfig& cfg) noexcept {
  const RealizedVarianceSpec& rv = contract.rv_spec;
  return contract.kind == DerivKind::VolSwap && cfg.engine == DerivEngine::VolCarrLee &&
         rv.n_obs_done == 0u && rv.n_obs_total > 1u;
}

// The repricings the stencils below difference. Members left at NaN are ones
// this bump set did not evaluate, and NaN then propagates into exactly the
// greeks that depend on them -- which is the "NaN = not computed" contract.
struct BumpPvs {
  double c = kNaN;                      // center
  double s_up = kNaN, s_dn = kNaN;      // S(1 +/- h)
  double v_up = kNaN, v_dn = kNaN;      // sigma +/- dv
  double t_dn = kNaN;                   // T - dt
  double t_dn_carry = kNaN;             // T - dt, +1 fixing at K_var_future
  double t_dn_zero_fixing = kNaN;       // T - dt, +1 zero-return fixing
  double sv_pp = kNaN, sv_pm = kNaN;    // (S+, sigma+), (S+, sigma-)
  double sv_mp = kNaN, sv_mm = kNaN;    // (S-, sigma+), (S-, sigma-)
  double t_s_up = kNaN, t_s_dn = kNaN;  // S(1 +/- h) at T - dt
  double sk_up = kNaN, sk_dn = kNaN;    // iv + (+/-)s*k        (Task F-7)
  double cx_up = kNaN, cx_dn = kNaN;    // iv + (+/-)c*k^2      (Task F-7)
};

// Up to 6 evaluations here, 12 with second_order (one fewer / three fewer when
// the contract cannot roll), plus 3 more when `bumps.carry_theta` is on (one
// var_swap_fair_strike call to resolve K_var_future, then the two carry-theta
// reprices above) -- skipped entirely (no extra evaluation) when
// `contract.rv_spec.n_obs_total == 0`, where there is no fixing schedule to
// inject into -- plus 4 more when `bumps.smile_greeks` is on. Every failure
// propagates: a bumped contract that will not price is a real failure, not a
// missing greek.
//
// Task F-7 recount: this said "7 / 13" and had done since Task P-2 deleted the
// FD rate bump without re-counting. The maximum is 14 `bumped_pv` calls (18
// with smile_greeks), and `deriv_greeks` adds its own center `deriv_price`
// and, on the carry-theta path, one `var_swap_fair_strike` -- 16 pricing
// evaluations for a maximal default call, 20 with smile_greeks. Those totals
// are MEASURED, not counted by eye: `SmileGreeks.OffByDefaultCostsNothing`
// (deriv_greeks_test.cpp) pins them through
// `deriv_greeks_reprice_count_for_test`.
//
// The center is repriced HERE, under the same pinned config as the bumps,
// rather than reusing the caller's center quote: a stencil must difference
// values from one consistent configuration, and the caller's center was priced
// before the grid and xi were pinned.
//
// `skip_market_bumps` (Task P-4 / GK-P): true when `deriv_greeks` will source
// delta/gamma/vega/vanna/volga from the closed form instead
// (`DerivGreekMethod::AnalyticStrip` on an in-scope `DerivKind::VarSwap`) --
// skips pv.s_up/s_dn/v_up/v_dn and, under `second_order`, the four sv_*
// cross terms (up to 8 of this table's evaluations), the ONLY consumers of
// which are those five greeks. `pv.c` (theta's own reference point) and
// theta/theta_carry/theta_zero_fixing/charm's own evaluations (pv.t_dn and
// friends, pv.t_s_up/pv.t_s_dn) are UNCHANGED either way -- those roll
// `maturity_t` and so price "genuinely new information" no closed form here
// shortcuts (see `DerivGreekMethod`'s own doc, derivatives.hpp).
template <class SurfaceT>
[[nodiscard]] Result<BumpPvs> eval_bump_table(const SurfaceT& surface, const CurveSet& curves,
                                              const DerivContract& contract,
                                              const DerivConfig& cfg,
                                              const DerivGreekBumps& bumps,
                                              bool skip_market_bumps) {
  const double h = bumps.spot_rel;
  const double dv = bumps.vol_abs;
  const double ks_up = sticky_k_shift(StickyMode::StickyStrike, h);
  const double ks_dn = sticky_k_shift(StickyMode::StickyStrike, -h);
  const CurveSet cs_up = respot_curves(curves, 1.0 + h);
  const CurveSet cs_dn = respot_curves(curves, 1.0 - h);

  // A roll landing at or past expiry has no future leg to price; theta/charm
  // stay NaN rather than failing the whole block (see the header).
  const bool can_roll = contract.maturity_t > bumps.time_years;
  DerivContract rolled = contract;
  rolled.maturity_t = contract.maturity_t - bumps.time_years;

  // Task P-3 / GK-P2: the six (k_shift, T) read-vector caches -- see
  // `BumpReadCache`/`CachedBumpView`'s own comments for the full mapping.
  // {center, spot-up, spot-down} x {this contract's T, T - bumps.time_years}.
  BumpReadCache cache_c_t0;   // {0}          @ T       -- pv.c, pv.v_up, pv.v_dn
  BumpReadCache cache_up_t0;  // {+ks_up}     @ T       -- pv.s_up, pv.sv_pp, pv.sv_pm
  BumpReadCache cache_dn_t0;  // {+ks_dn}     @ T       -- pv.s_dn, pv.sv_mp, pv.sv_mm
  BumpReadCache cache_c_tr;   // {0}          @ T - dt  -- pv.t_dn, carry_pv, zero_pv
  BumpReadCache cache_up_tr;  // {+ks_up}     @ T - dt  -- pv.t_s_up
  BumpReadCache cache_dn_tr;  // {+ks_dn}     @ T - dt  -- pv.t_s_dn

  // Each cache_*_t0/cache_*_tr slot's RECORDING evaluation is the first
  // `bumped_pv` call issued against it below, preceded by `begin_recording`
  // (Review fix round 1, I-3/I-5): only the four slots below EVER call it,
  // since only they have a later replayer -- `cache_up_tr`/`cache_dn_tr`
  // (each fed by exactly one bump: pv.t_s_up/pv.t_s_dn) never do, so they
  // stay in `recording == false` mode and `CachedBumpView` reads them live
  // without touching `entries` at all. `finish_recording()` right after each
  // recording call succeeds sorts the slot ONCE so every later evaluation
  // sharing it (still to come, strictly after -- see each call's own comment
  // for exactly which) gets the O(log n) binary-search path instead of the
  // O(n) append/scan-never one.
  //
  // `reserve_hint` sizes every `begin_recording` reservation to the pinned
  // strip's own node count (`pin_center_scheme`, called by `deriv_greeks`
  // before this table runs, pins `cfg.strip_nodes` to the center's resolved
  // grid whenever the center resolved one -- true of virtually every real
  // call) plus a small margin for the handful of non-strip reads a dispatch
  // may also fold into the same (k_shift, T) bucket (e.g. the ATM sigma_atm
  // read). Falls back to the hard cap `kMaxStripNodes` on the rare
  // caller-degenerate center that left `strip_nodes` unpinned -- always a
  // safe upper bound, just a more generous one than usually needed.
  const std::size_t reserve_hint =
      cfg.strip_nodes != 0u ? static_cast<std::size_t>(cfg.strip_nodes) + 4u
                            : strip::kMaxStripNodes;

  BumpPvs pv{};
  // I-3's "single-use slot" rule, restated now that TWO groups can replay this
  // slot: record `cache_c_t0` iff something later actually reads it back --
  // the vol bumps (skipped under `skip_market_bumps`) or Task F-7's smile
  // bumps, which price at zero spot AND zero vol shift and so query exactly
  // the nodes the center just recorded. With neither, `CachedBumpView` reads
  // live and the reserve()+sort() a recording slot pays for is not spent on
  // zero readers.
  const bool c_t0_has_replayer = !skip_market_bumps || bumps.smile_greeks;
  if (c_t0_has_replayer) {
    cache_c_t0.begin_recording(reserve_hint);
  }
  ATX_TRY(pv.c, bumped_pv(surface, curves, contract, cfg, 0.0, 0.0, cache_c_t0));
  if (c_t0_has_replayer) {
    cache_c_t0.finish_recording();  // replayed by pv.v_up/v_dn and/or the smile bumps
  }
  if (!skip_market_bumps) {
    cache_up_t0.begin_recording(reserve_hint);
    ATX_TRY(pv.s_up, bumped_pv(surface, cs_up, contract, cfg, ks_up, 0.0, cache_up_t0));
    cache_up_t0.finish_recording();  // replayed by pv.sv_pp, pv.sv_pm below
    cache_dn_t0.begin_recording(reserve_hint);
    ATX_TRY(pv.s_dn, bumped_pv(surface, cs_dn, contract, cfg, ks_dn, 0.0, cache_dn_t0));
    cache_dn_t0.finish_recording();  // replayed by pv.sv_mp, pv.sv_mm below
    ATX_TRY(pv.v_up, bumped_pv(surface, curves, contract, cfg, 0.0, dv, cache_c_t0));
    ATX_TRY(pv.v_dn, bumped_pv(surface, curves, contract, cfg, 0.0, -dv, cache_c_t0));
  }

  // Task F-7 smile bumps. Run for EVERY method, including AnalyticStrip
  // (`skip_market_bumps`): the closed form has no skew/convexity term, so
  // finite difference is the only construction available, and silently
  // returning NaN for a greek the caller explicitly asked for would be the
  // worse outcome. Both are pure central differences with no dependence on
  // `pv.c`, so they are correct whether or not the market bumps ran.
  if (bumps.smile_greeks) {
    const double sk = bumps.skew_abs;
    const double cx = bumps.convexity_abs;
    ATX_TRY(pv.sk_up, skew_bumped_pv(surface, curves, contract, cfg, sk, cache_c_t0));
    ATX_TRY(pv.sk_dn, skew_bumped_pv(surface, curves, contract, cfg, -sk, cache_c_t0));
    ATX_TRY(pv.cx_up, convex_bumped_pv(surface, curves, contract, cfg, cx, cache_c_t0));
    ATX_TRY(pv.cx_dn, convex_bumped_pv(surface, curves, contract, cfg, -cx, cache_c_t0));
  }

  if (can_roll) {
    cache_c_tr.begin_recording(reserve_hint);
    ATX_TRY(pv.t_dn, bumped_pv(surface, curves, rolled, cfg, 0.0, 0.0, cache_c_tr));
    cache_c_tr.finish_recording();  // replayed by carry_pv/zero_pv below

    if (bumps.carry_theta) {
      const RealizedVarianceSpec& rv = contract.rv_spec;
      if (rv.n_obs_total == 0u) {
        // No fixing schedule exists (n_total == 0 -> "fully unaged, no
        // accrual concept" per aged_total_variance_dec): the fixing roll and
        // the calendar roll coincide, so both variants collapse to the
        // plain theta reprice already computed above -- no extra cost.
        pv.t_dn_carry = pv.t_dn;
        pv.t_dn_zero_fixing = pv.t_dn;
      } else if (carry_fixing_crosses_engine_boundary(contract, cfg)) {
        // CRITICAL-1 (fix round 1): the injected fixing would move this
        // unaged, explicit-VolCarrLee VolSwap into `price_vol_swap`'s
        // mid-life branch, which rejects that engine outright -- see
        // `carry_fixing_crosses_engine_boundary`. Left NaN: "not computed" is
        // the honest reading for a diagnostic that cannot be priced under the
        // caller's own explicit engine choice, not a value borrowed from a
        // pricer the center never used.
        pv.t_dn_carry = kNaN;
        pv.t_dn_zero_fixing = kNaN;
      } else {
        // K_var_future resolved fresh, at the CENTER's own T and under the
        // SAME pinned grid (`cfg` here is already `cfg_pinned` -- see
        // deriv_greeks below) every other bump reads. Independent of
        // DerivKind by construction: every product this file prices strikes
        // its future leg against this same model-free variance process (see
        // the file header), so this does not need, and deliberately does not
        // read, any DerivKind-specific quote field.
        //
        // Task F-3 tested that claim against the corridor kind and it
        // SURVIVES, for a reason worth writing down because the obvious
        // alternative is wrong. The PLAIN (all-strike) K_var is the right
        // injection rate for a corridor contract too: this stencil injects ONE
        // fixing whose previous close is today's KNOWN spot, so conditional on
        // that fixing counting at all, its expected r^2 is the one-period
        // forward variance -- not K_corridor, which is a TIME AVERAGE over
        // sessions the spot may spend OUTSIDE the corridor and would
        // understate a fixing already known to be inside it. WHETHER the
        // fixing counts is `inject_carry_fixing`'s corridor indicator, not
        // this rate's business.
        //
        // Aggregate review fix (C-R Critical #1): this strip is the SAME
        // `var_swap_fair_strike` call that `price_vol_swap`'s unaged branch
        // (above, :635-637) already treats as best-effort -- and C-4 gave it
        // a brand-new hard `Internal` failure past its interior-bad-node
        // threshold. Left as a plain `ATX_TRY`, a holey-but-otherwise-
        // servable surface would lose the WHOLE `Result<DerivGreeks>` over a
        // diagnostic pair, under the DEFAULT config (`carry_theta` defaults
        // true, engine defaults `Auto`), where pre-C-10 the block priced
        // clean. Degrade both carry fields to NaN on any failure here --
        // strip resolution or either reprice below, since a reprice of the
        // fixing-injected contract can hit the same strip through the
        // mid-life distribution branch -- mirroring the engine-boundary
        // degrade immediately above. "Not computed" is the honest reading;
        // the CENTER quote's fail-loud contract (C-4) is untouched, since
        // this stencil pair never feeds it.
        count_deriv_greeks_reprice();  // Task F-7 seam: a strip is an evaluation
        if (const Result<DerivQuote> strip_q =
                var_swap_fair_strike(surface, curves, contract.maturity_t, cfg);
            strip_q.has_value()) {
          const double k_var_future = strip_q->fair_strike_dec;

          // Task F-3: the contract's own corridor -- unbounded (a no-op) for
          // every kind but CorridorVarSwap, and for that kind the thing that
          // decides whether the injected fixing counts at all.
          const StrikeCorridor corridor{contract.corridor_lo, contract.corridor_hi};
          DerivContract rolled_carry = rolled;
          rolled_carry.rv_spec = inject_carry_fixing(rv, k_var_future, curves.spot, corridor);
          const Result<double> carry_pv =
              bumped_pv(surface, curves, rolled_carry, cfg, 0.0, 0.0, cache_c_tr);

          DerivContract rolled_zero = rolled;
          rolled_zero.rv_spec = inject_carry_fixing(rv, 0.0, curves.spot, corridor);
          const Result<double> zero_pv =
              bumped_pv(surface, curves, rolled_zero, cfg, 0.0, 0.0, cache_c_tr);

          pv.t_dn_carry = carry_pv.has_value() ? *carry_pv : kNaN;
          pv.t_dn_zero_fixing = zero_pv.has_value() ? *zero_pv : kNaN;
        } else {
          pv.t_dn_carry = kNaN;
          pv.t_dn_zero_fixing = kNaN;
        }
      }
    }
  }

  if (bumps.second_order) {
    if (!skip_market_bumps) {
      ATX_TRY(pv.sv_pp, bumped_pv(surface, cs_up, contract, cfg, ks_up, dv, cache_up_t0));
      ATX_TRY(pv.sv_pm, bumped_pv(surface, cs_up, contract, cfg, ks_up, -dv, cache_up_t0));
      ATX_TRY(pv.sv_mp, bumped_pv(surface, cs_dn, contract, cfg, ks_dn, dv, cache_dn_t0));
      ATX_TRY(pv.sv_mm, bumped_pv(surface, cs_dn, contract, cfg, ks_dn, -dv, cache_dn_t0));
    }
    if (can_roll) {
      ATX_TRY(pv.t_s_up, bumped_pv(surface, cs_up, rolled, cfg, ks_up, 0.0, cache_up_tr));
      ATX_TRY(pv.t_s_dn, bumped_pv(surface, cs_dn, rolled, cfg, ks_dn, 0.0, cache_dn_tr));
    }
  }
  return Ok(pv);
}

// Bump sizes are a caller input: validate once, at the boundary. vol_abs also
// gets a surface-aware check: a down-bump >= the surface's own ATM vol pushes
// v_dn's vol-shifted nodes (CachedBumpView::iv, above) to <= 0, and the strip
// funnel resolves that non-positive iv silently rather than erroring --
// hollowing out vega/volga/vanna with no visible signal. Reading sigma_atm
// once at k=0, this contract's own T, is a cheap sufficient guard for the
// common case (it does not certify every node across the smile survives the
// shift, only the ATM one). A non-finite read (surface has no opinion at this
// T) is not itself a bump-size failure, so it does not reject here.
template <class SurfaceT>
[[nodiscard]] bool bumps_valid(const DerivGreekBumps& b, const SurfaceT& surface,
                                double T) noexcept {
  // Task F-7: the two smile bumps are validated UNCONDITIONALLY, not only when
  // `smile_greeks` is set -- the same convention `rate_abs` already follows
  // now that nothing reads it. Every bump size is a caller input validated
  // once at the boundary, so which knob happens to consume it is not this
  // predicate's business, and a caller cannot leave a zero divisor parked in
  // the struct waiting for the day someone flips the flag on.
  if (!(b.spot_rel > 0.0 && b.spot_rel < 1.0 && b.vol_abs > 0.0 && b.rate_abs > 0.0 &&
        b.time_years > 0.0 && b.skew_abs > 0.0 && b.convexity_abs > 0.0)) {
    return false;
  }
  const double sigma_atm = surface.iv(0.0, T);
  return !(std::isfinite(sigma_atm) && b.vol_abs >= sigma_atm);
}

// Pin everything about the center quote that a bumped evaluation would
// otherwise re-derive for itself: the strip's grid, the resolved wing clamp,
// and the vol-of-vol. All three are resolved from the SURFACE, so all three
// drift when the surface is bumped, and would then contaminate the
// differences with a change in the numerical scheme rather than a change in
// the price.
[[nodiscard]] DerivConfig pin_center_scheme(const DerivConfig& cfg, const DerivQuote& center) noexcept {
  DerivConfig out = cfg;

  // Grid: a bumped surface can cross the adaptive rescale's ceil() boundary and
  // integrate on a different node count than the center, making the stencils
  // straddle a step discontinuity.
  if (center.strip_nodes_used > 0u && std::isfinite(center.strip_k_lo_used) &&
      std::isfinite(center.strip_k_hi_used) &&
      center.strip_k_lo_used < center.strip_k_hi_used) {
    out.k_min_log = center.strip_k_lo_used;
    out.k_max_log = center.strip_k_hi_used;
    out.strip_nodes = center.strip_nodes_used;
  }

  // Wing clamp (FIT-C7 / Task C-6): a bumped evaluation prices through
  // `CachedBumpView`, an adapter that carries no surface provenance of
  // its own -- left alone, `resolve_wing_clamp` would silently fall back to
  // the mode-blind default for every bump while the center resolved a
  // surface-carried band, straddling a DIFFERENT clamp than the value it is
  // differenced against. Only pin when the center itself consulted surface
  // provenance (`cfg.wing_clamp_k == 0.0`, the default-resolution branch) and
  // actually resolved a positive band (a strip ran); an explicit >0/<0
  // override on `cfg` already resolves identically for every bump with no
  // surface read at all, and pinning 0.0 here would wrongly turn "clamp
  // resolved off" into "explicit request for the certified band".
  if (cfg.wing_clamp_k == 0.0 && center.resolved_wing_clamp > 0.0) {
    out.wing_clamp_k = center.resolved_wing_clamp;
  }

  // Vol-of-vol: pin the calibrated xi so vega measures the model's response to
  // the vol shift, not the calibration re-fitting itself. A calibrated xi of
  // exactly 0 cannot be written back as 0 (that is the config's
  // "auto-calibrate" selector), so it is pinned as the smallest positive double
  // instead. Every consumer of xi reaches the same limit at a denormal as at
  // zero: `lognormal_expect`'s nodes collapse onto the mean, `lognormal_call`
  // resolves to max(m-k,0) through Phi(+-inf), and the capped-vol-swap's kink
  // coordinate z* overflows to +-inf, which `lognormal_truncated_expect` clamps
  // into its own [-8,8] domain. So this pins the VALUE without selecting the
  // auto path, which is exactly what is needed.
  if (std::isfinite(center.vol_of_vol_used)) {
    out.vol_of_vol = center.vol_of_vol_used > 0.0
                         ? center.vol_of_vol_used
                         : std::numeric_limits<double>::denorm_min();
  }
  return out;
}

// Task P-4 / GK-P, Task F-1 (review fix I-2): the `DerivConfig`-projection
// half of `AnalyticStrip`'s scope predicate -- the two fields that decide
// whether the closed form (`deriv_analytic_greeks.hpp`) reproduces what
// `price_var_swap` actually priced, independent of which caller is asking.
// Single definition shared by BOTH call sites (`deriv_greeks` below and
// P-6's `ensure_var_swap_greeks_block`) instead of two independently-
// maintained copies held in sync only by a comment: the P-5 review's
// Important I-1 found the identical shape (a projection duplicated across
// TUs) and the controller closed it by extraction, and this is the same
// remedy for the same reason -- a future scope-relevant `DerivConfig` field
// added to only one copy is exactly how P-4's own C-1 finding happened
// (CHANGELOG.md). The `kind`/`bumps.method` checks each call site also needs
// stay at the call site: `ensure_var_swap_greeks_block` already has `kind ==
// VarSwap` guaranteed by its caller's memo-eligibility gate
// (`var_swap_memo_eligible`, deriv_book.cpp) and would wastefully re-check
// it here, so only the part that is NOT already guaranteed anywhere belongs
// in the shared definition.
[[nodiscard]] bool analytic_scope_from_cfg(const DerivConfig& cfg) noexcept {
  return cfg.discrete_correction_mode == DerivDiscreteCorrection::None &&
         cfg.wing_mode != StripWingMode::LeeSlopeExtrapolation;
}

}  // namespace

template <class SurfaceT>
Result<DerivGreeks> deriv_greeks(const SurfaceT& surface, const CurveSet& curves,
                                 const DerivContract& contract, const DerivConfig& cfg,
                                 const DerivGreekBumps& bumps) {
  if (!bumps_valid(bumps, surface, contract.maturity_t)) {
    return Err(ErrorCode::InvalidArgument,
               "greek bump sizes must be > 0 (spot_rel < 1, vol_abs < ATM vol)");
  }
  if (!(curves.spot > 0.0)) {
    return Err(ErrorCode::InvalidArgument, "greeks need curves.spot > 0 (delta's divisor)");
  }

  count_deriv_greeks_reprice();  // Task F-7 seam: the center is a repricing too
  ATX_TRY(const DerivQuote center, deriv_price(surface, curves, contract, cfg));
  DerivGreeks g{};
  g.pv = center.pv;
  g.quote = center;

  // Fully aged: nothing is left to realize, so PV is a fixed settlement amount
  // under a pure discount, PV(t) = e^{-r(T-t)}*X. Both time greeks are then
  // analytic and must agree with each other -- dPV/dr = -(T-t)*PV and
  // dPV/dt = +r*PV are the same statement differentiated two ways. No bumping.
  // At T == 0 the discount is gone and both collapse to 0 (YieldCurve::zero
  // returns 0 for T <= 0, so theta lands there without a special case).
  if (has_flag(center.flags, DerivFlags::FullyAged)) {
    // Clamp T >= 0 before the analytic rho: a lot can be FullyAged (obs-count
    // driven) while its own maturity_t has already gone negative (expired,
    // not yet rolled off the book). Un-clamped, -T*PV sign-flips into a
    // fabricated positive rho instead of the "nothing left to discount" 0.
    const double t_nonneg = std::fmax(contract.maturity_t, 0.0);
    g.rho = -t_nonneg * center.pv;
    g.theta = curves.yield.zero(contract.maturity_t) * center.pv;
    // Nothing left to realize -> no fixing left to roll either (Task C-10):
    // both carry variants equal theta exactly, the same identity rho/theta
    // already share on this branch.
    g.theta_carry = g.theta;
    g.theta_zero_fixing = g.theta;
    return Ok(g);
  }

  // Pin the center's numerical scheme (strip grid + calibrated xi) into every
  // bumped evaluation; see pin_center_scheme and the header.
  const DerivConfig cfg_pinned = pin_center_scheme(cfg, center);

  // Task P-4 / GK-P: AnalyticStrip is IN SCOPE only for an uncapped VarSwap
  // (see `DerivGreekMethod`'s own doc, derivatives.hpp) -- every other kind
  // falls back to FiniteDifference SILENTLY, right here, by simply never
  // setting this true; the kind x engine dispatch matrix already ran inside
  // `deriv_price` above (the `ATX_TRY(center, ...)` at the top of this
  // function), so a genuinely invalid (kind, engine) pairing has ALREADY
  // failed loud before this line is ever reached -- this fallback only
  // selects which numerical method computes a greek, never which errors
  // `deriv_price` raises.
  //
  // Review fix round 1, C-1: `kind == VarSwap` alone is NOT the whole scope
  // -- `analytic_strip_greeks` differentiates the RAW strip K_var only, and
  // `price_var_swap` does not always price the raw strip. Every `DerivConfig`
  // field was re-audited for whether it changes what the closed form must
  // reproduce:
  //   - `discrete_correction_mode == Diffusion1OverN` adds
  //     `(T/n_rem)*((r-q) - K_var/2)^2` to `k_var_future_dec` BEFORE it
  //     becomes the linear-in-K_var quantity PV is linear in (`price_var_
  //     swap`, the `Diffusion1OverN` branch below `var_swap_fair_strike`) --
  //     that addend is QUADRATIC in K_var, so every first-order sensitivity
  //     picks up a `(1 - (T/n_rem)*mu)` factor this closed form does not
  //     know about, and volga/vanna pick up an extra cross term too. Gated
  //     below -- this was the missed dimension (C-1).
  //   - `engine`: VarSwap reaches `price_var_swap` regardless of engine
  //     (`deriv_price`'s dispatch matrix); no engine choice changes the
  //     strip formula for this kind. Not scope-relevant.
  //   - `quality`/`k_min_log`/`k_max_log`/`strip_nodes`/`width_sigmas`: all
  //     four only shape WHICH grid gets resolved; `analytic_strip_greeks` is
  //     handed the CENTER's own already-resolved `strip_k_lo_used`/
  //     `strip_k_hi_used`/`strip_nodes_used`, so whatever grid these
  //     produced is reproduced exactly regardless of how it got there. Not
  //     separately scope-relevant.
  //   - `wing_clamp_k`: same reasoning -- the resolved band travels as
  //     `center.resolved_wing_clamp`, already threaded into the analytic
  //     call. Not separately scope-relevant.
  //   - `wing_mode` (Task F-1): IS separately scope-relevant, unlike
  //     `wing_clamp_k` above. `analytic_strip_greeks` (deriv_analytic_
  //     greeks.hpp) hard-codes the FlatClamp/Raw "clamp the grid position
  //     once, then shift" wing identity -- every clamped node's sigma'/
  //     sigma'' equals the band-edge node's, by construction -- which is
  //     simply the WRONG differentiation target under
  //     `StripWingMode::LeeSlopeExtrapolation` (a node past the band there
  //     tracks `w_edge + beta*(|k|-k_band)`, whose own sensitivity to a spot/
  //     vol bump the closed form has no term for: beta is itself a finite
  //     difference of the surface, not a value the surface hands back
  //     directly). Gated below, alongside `discrete_correction_mode`.
  //   - `vol_of_vol`/`carr_lee_form`: both drive only the RV distribution
  //     model (mid-life VolSwap, both capped kinds) and the standalone
  //     Carr-Lee K_vol entry -- `price_var_swap` never reads either field.
  //     Not scope-relevant for VarSwap.
  //   - `abs_price_tol`/`rel_price_tol`/`flags_request`: reserved, must be
  //     zero (`reserved_fields_clean`, checked inside `var_swap_fair_strike`
  //     before any of this runs). Not scope-relevant.
  //
  // The `discrete_correction_mode`/`wing_mode` half of this predicate is
  // shared with P-6's `ensure_var_swap_greeks_block` via
  // `analytic_scope_from_cfg` (review fix I-2) -- see that function's own
  // doc for why the `kind`/`method` checks stay here instead of joining it.
  const bool analytic_in_scope = bumps.method == DerivGreekMethod::AnalyticStrip &&
                                 contract.kind == DerivKind::VarSwap &&
                                 analytic_scope_from_cfg(cfg);

  ATX_TRY(const BumpPvs p,
          eval_bump_table(surface, curves, contract, cfg_pinned, bumps, analytic_in_scope));

  const double ds = bumps.spot_rel * curves.spot;  // absolute spot bump
  const double dv = bumps.vol_abs;
  if (analytic_in_scope) {
    // Closed form: differentiates the model-free strip's own linear
    // functional of Black-76 prices instead of repricing it under a bump --
    // see deriv_analytic_greeks.hpp for the full derivation. `F`/`df` are
    // resolved fresh at the SAME (curves, T) the center's own strip used
    // (pure functions of those two inputs, so they agree with the center's
    // internal resolution by construction, not by re-derivation); `w_future`
    // mirrors `price_var_swap`'s own blend weight exactly -- see
    // `AnalyticStripGreeks`'s own doc for why that is the whole scale factor
    // needed (PV is LINEAR in k_var_future_dec with that slope).
    DerivFlags df_flags_unused = DerivFlags::None;
    const double f_at_t = resolve_forward(curves, contract.maturity_t);
    const double df_at_t = deriv_df_at_T(curves, contract.maturity_t, df_flags_unused);
    double w_future = 1.0;
    if (contract.rv_spec.n_obs_total > 0u) {
      const double w_done = static_cast<double>(contract.rv_spec.n_obs_done) /
                            static_cast<double>(contract.rv_spec.n_obs_total);
      w_future = 1.0 - w_done;
    }
    const detail::AnalyticStripGreeks ag = detail::analytic_strip_greeks(
        surface, f_at_t, curves.spot, contract.maturity_t, df_at_t, center.strip_k_lo_used,
        center.strip_k_hi_used, static_cast<std::size_t>(center.strip_nodes_used),
        center.resolved_wing_clamp);
    const double scale = df_at_t * contract.notional * w_future;
    g.delta = scale * ag.delta;
    g.gamma = scale * ag.gamma;
    g.vega = scale * ag.vega;
    // `second_order` still gates vanna under EITHER method -- its documented
    // contract ("NaN when off", DerivGreekBumps::second_order) predates
    // AnalyticStrip and is preserved rather than silently narrowed just
    // because the closed form makes vanna no more expensive than the other
    // four.
    g.vanna = bumps.second_order ? scale * ag.vanna : kNaN;
    g.volga = scale * ag.volga;
  } else {
    g.delta = (p.s_up - p.s_dn) / (2.0 * ds);
    g.gamma = (p.s_up - 2.0 * p.c + p.s_dn) / (ds * ds);
    g.vega = (p.v_up - p.v_dn) / (2.0 * dv);
    g.volga = (p.v_up - 2.0 * p.c + p.v_dn) / (dv * dv);
    g.vanna = (p.sv_pp - p.sv_pm - p.sv_mp + p.sv_mm) / (4.0 * ds * dv);
  }
  // Task F-7 smile greeks. UNCONDITIONAL arithmetic, exactly like vanna's
  // above: `sk_*`/`cx_*` stay at `BumpPvs`' own kNaN default when
  // `smile_greeks` is off, so both fields become NaN by PROPAGATION rather
  // than by an explicit assignment -- which is what keeps the NaN payload
  // bit-identical to the P-6 memoized path (see `deriv_greeks_var_swap_shared`
  // for the same reasoning applied to theta/charm). Central differences, so
  // each is dPV per 1.00 of the coefficient; see `DerivGreeks::skew_vega` for
  // the k convention and the expected signs.
  g.skew_vega = (p.sk_up - p.sk_dn) / (2.0 * bumps.skew_abs);
  g.convexity_vega = (p.cx_up - p.cx_dn) / (2.0 * bumps.convexity_abs);
  g.theta = (p.t_dn - p.c) / bumps.time_years;
  // Task C-10 / GK-C2: same one-sided roll and divisor as theta above, just
  // against a T - dt reprice that also carries one injected fixing (see
  // eval_bump_table / inject_carry_fixing). NaN propagates from p.t_dn_carry
  // / p.t_dn_zero_fixing exactly when theta itself is NaN (cannot roll) or
  // when DerivGreekBumps::carry_theta is false.
  g.theta_carry = (p.t_dn_carry - p.c) / bumps.time_years;
  g.theta_zero_fixing = (p.t_dn_zero_fixing - p.c) / bumps.time_years;
  // Task P-2 / GK-C3: rho is EXACTLY -T*PV here too, not just on the
  // fully-aged branch above -- every non-fully-aged quote this file builds is
  // `pv = df(r) * X` with X (the fair-strike/expectation/cap-option blend)
  // provably independent of the rate curve (the strip integrand's own df
  // cancels, Carr-Lee's ATMF formula never reads curves.yield, and the
  // Diffusion1OverN carry differential comes off the forward/spot, not the
  // yield curve -- see Rho.AnalyticMatchesFD, deriv_greeks_test.cpp, which
  // pins this against the FD bump this replaced across every DerivKind and
  // aging state).
  //
  // CLAMP T >= 0 (fix round 1, C-1), same reason PV-9 clamps the fully-aged
  // branch above: `deriv_price` performs NO `maturity_t` validation of its
  // own, and NOT EVERY non-fully-aged success path requires T > 0 --
  // `price_capped_var_swap`/`price_capped_vol_swap`'s CAP-PIN exit
  // (`accrued >= cap` / `a >= cap_var`) returns a deterministic quote for ANY
  // T, including T <= 0, and does so WITHOUT setting `DerivFlags::FullyAged`
  // whenever the contract is only partially aged (n_done < n_total) -- so a
  // cap-pinned, partially-aged, EXPIRED-but-not-rolled-off contract reaches
  // this line, not the clamped fully-aged branch above. At T <= 0,
  // `deriv_df_at_T` short-circuits `df = 1.0` unconditionally (no
  // `curves.yield` read at all), so the true `dPV/dr` there is exactly 0, not
  // `-T*PV` -- an un-clamped negative T would sign-flip that into a
  // fabricated POSITIVE rho instead. `Rho.AnalyticMatchesFD` pins this exact
  // scenario (a cap-pinned, partially-aged, negative-T CappedVarSwap/
  // CappedVolSwap) against an independent oracle.
  //
  // Replaces a one-sided FD r+ bump (a full extra repricing -- a second strip
  // integration, or for VolSwap a second Carr-Lee straddle plus its own
  // diagnostic strip) that only ever recovered this same identity, to FD
  // precision, never anything else: forward-channel rate risk is
  // deliberately NOT in rho (the bump mechanics doc above `deriv_greeks`
  // holds the forward curve fixed under a rate shift), so there is no
  // additional rate sensitivity this closed form could be missing.
  g.rho = -std::fmax(contract.maturity_t, 0.0) * center.pv;
  // charm = d(delta)/dt on the SAME calendar-time convention as theta above:
  // one day of calendar time is one day of maturity gone. `delta_rolled` is
  // ALWAYS the FD spot-bump delta at T - dt (Task P-4 does not touch this --
  // charm needs a genuinely NEW, rolled repricing, same "not a closed form"
  // reasoning as theta itself; see DerivGreekMethod's own doc). Under
  // AnalyticStrip this differences an FD delta (at T - dt) against the
  // ANALYTIC delta (`g.delta`, at T) -- a documented hybrid, not an
  // inconsistency: the two constructions are proven to agree within the
  // parity suite's own gate (`AnalyticGreeks.*`, deriv_greeks_test.cpp), so
  // substituting one for the other here moves charm by at most that same
  // tiny, already-bounded amount divided by `bumps.time_years`.
  const double delta_rolled = (p.t_s_up - p.t_s_dn) / (2.0 * ds);
  g.charm = (delta_rolled - g.delta) / bumps.time_years;
  return Ok(g);
}

// ── Task P-6 (GK-P book memo): VarSwap shared per-(uid,T) block ────────────
//
// `deriv_book.cpp` prices L VarSwap rows that share (uid, T, book-wide cfg).
// PV and every strip-affine greek read nothing surface/tenor-dependent that
// is not IDENTICAL across those L rows -- `resolve_var_swap_strip_raw` /
// `assemble_var_swap_quote` above already isolate that for PV; the functions
// below extend the same split through `deriv_greeks`'s own bump table, so a
// book-level caller can resolve the expensive part ONCE per (uid, T) and
// combine it with each row's own strike/notional/rv_spec via EXACTLY the
// formulas `price_var_swap` / `deriv_greeks` already use.
//
// `VarSwapSharedBlock` itself (the memo state) is declared in
// `deriv_ref_bridge.hpp`, not here -- see that header for the field-by-field
// doc. It holds no SurfaceT template parameter (every field is a plain
// value/`Result<DerivQuote>`), so `deriv_book.cpp` can hold it directly in
// its memo map; only the BUILDERS below are templated, exactly like
// `SurfaceRefStripView` itself.
//
// SCOPE mirrors `resolve_var_swap_strip_raw`: `contract.kind == VarSwap` and
// `cfg.discrete_correction_mode == None`, enforced by the caller
// (deriv_book.cpp), not re-checked here.
//
// EVERY raw strip evaluation is shared, including theta / theta_carry /
// theta_zero_fixing / charm's own T-dt roll: `ensure_var_swap_greeks_block`
// resolves the T-dt strip (`c_tdt_raw`) and, under `second_order`, its two
// spot-bumped companions (`s_up_tdt_raw` / `s_dn_tdt_raw`) ONCE per (uid, T)
// group too, alongside the T-side market-bump grid -- T - bumps.time_years is
// a book-wide constant offset from T, so every row in the group rolls to the
// SAME T-dt. What genuinely stays PER ROW is only the CHEAP combine step
// (`assemble_var_swap_quote`'s aged blend + discount + strike offset, and
// carry-theta's `inject_carry_fixing`) -- no strip integration, just a few
// flops -- since that is exactly where each row's own strike/notional/rv_spec
// enters.

using detail::VarSwapSharedBlock;

// The book-memo lane's dispatch validation. This exists at all because the
// shared-block path calls `var_swap_fair_strike` directly for a NOT-fully-aged
// row (bypassing `deriv_price`'s dispatch) and never reaches it AT ALL for a
// fully-aged one, so a fully-aged row with a malformed `cfg` or contract must
// still fail exactly as `deriv_price` would have.
//
// Task F-3 fix round 1 (C-1 Critical): it used to RE-DERIVE that validation --
// a hand-synchronised second copy whose own comment claimed it "mirrors" the
// original. It does not re-derive anything now; it calls the same
// `validate_deriv_dispatch` `deriv_price` does, so the mirror is STRUCTURAL
// and the two lanes cannot disagree about any dispatch-level rule, present or
// future. See that function's comment for the measured failure this closes.
//
// `wing_clamp_valid` / `surface_certified_wing_band_valid` are still not part
// of it -- `deriv_price`'s dispatch never checked them either; only
// `var_swap_fair_strike` itself does, which is why a fully-aged row is
// unaffected by a bad wing-clamp band under the UNMEMOIZED path too. That is a
// property of the shared function now, so it too cannot drift between lanes.
[[nodiscard]] Status validate_var_swap_dispatch(const DerivContract& contract,
                                                 const DerivConfig& cfg) {
  return validate_deriv_dispatch(contract, cfg);
}

// Resolve `df_at_T` -- cheap (no quadrature), and every row wants it
// (including a fully-aged one, whose PV is still `df * notional *
// (rv_done_dec - strike)`). Idempotent, unconditional.
void ensure_var_swap_df(VarSwapSharedBlock& block, const CurveSet& curves, double T) noexcept {
  if (block.df_resolved) {
    return;
  }
  block.df_resolved = true;
  DerivFlags df_flags = DerivFlags::None;
  block.df_at_T = deriv_df_at_T(curves, T, df_flags);
  block.df_fallback_at_T = has_flag(df_flags, DerivFlags::DfFallback);
}

// Resolve `center_raw` -- the strip itself. Fix round 1, I-2: the CALLER
// must gate this on `need_strip` (this file's own
// `n_obs_total == 0u || n_obs_done < n_obs_total` test, recomputed at each
// call site) -- an all-fully-aged (uid,T) group must never call this at all,
// matching `price_var_swap`'s own "fully aged never runs the strip" gate.
// Idempotent for the groups that DO need it (first not-fully-aged row
// resolves it once; every sibling, fully aged or not, reuses the result).
template <class SurfaceT>
void ensure_var_swap_center_strip(VarSwapSharedBlock& block, const SurfaceT& surface,
                                   const CurveSet& curves, double T, const DerivConfig& cfg) {
  if (block.strip_resolved) {
    return;
  }
  block.strip_resolved = true;
  block.center_raw = resolve_var_swap_strip_raw(surface, curves, T, cfg);
}

// Resolve the market-greek sub-block (pinned-grid center + FD spot/vol bump
// grid, or the raw P-4 analytic block) AND the T-dt roll sub-block (theta /
// carry-theta / charm's shared input) -- called once a NOT-fully-aged row
// asks for greeks. `cfg` is the group's own (unpinned) config; `center` is
// the already-assembled center quote (used only to derive `cfg_pinned` and,
// under AnalyticStrip, its own grid/wing-clamp fields -- exactly
// `deriv_greeks`'s own `pin_center_scheme(cfg, center)` / `analytic_strip_
// greeks(..., center.strip_k_lo_used, ...)` call sites). Idempotent.
template <class SurfaceT>
void ensure_var_swap_greeks_block(VarSwapSharedBlock& block, const SurfaceT& surface,
                                   const CurveSet& curves, double T, const DerivConfig& cfg,
                                   const DerivGreekBumps& bumps, const DerivQuote& center) {
  if (block.greeks_resolved) {
    return;
  }
  block.greeks_resolved = true;
  block.cfg_pinned = pin_center_scheme(cfg, center);
  block.pinned_center_raw = resolve_var_swap_strip_raw(surface, curves, T, block.cfg_pinned);

  if (block.pinned_center_raw.has_value()) {
    // `kind == VarSwap` is already guaranteed by the caller's memo-
    // eligibility gate (`var_swap_memo_eligible`, deriv_book.cpp) before a
    // row ever reaches this shared-block builder -- unlike `deriv_greeks`'s
    // own `analytic_in_scope`, this one does not need to re-check it.
    // `discrete_correction_mode`/`wing_mode` are re-checked via the SAME
    // shared `analytic_scope_from_cfg` (review fix I-2) the other site uses
    // -- `discrete_correction_mode == None` happens to already be guaranteed
    // upstream too (the same gate), but calling the one shared definition
    // here costs nothing and removes the risk of the two sites drifting
    // apart on a future scope-relevant field.
    const bool analytic_in_scope =
        bumps.method == DerivGreekMethod::AnalyticStrip && analytic_scope_from_cfg(cfg);
    if (analytic_in_scope) {
      block.have_analytic = true;
      const double f_at_t = resolve_forward(curves, T);
      const detail::AnalyticStripGreeks ag = detail::analytic_strip_greeks(
          surface, f_at_t, curves.spot, T, block.df_at_T, center.strip_k_lo_used,
          center.strip_k_hi_used, static_cast<std::size_t>(center.strip_nodes_used),
          center.resolved_wing_clamp);
      block.a_delta = ag.delta;
      block.a_gamma = ag.gamma;
      block.a_vega = ag.vega;
      block.a_vanna = ag.vanna;
      block.a_volga = ag.volga;
    } else {
      block.have_second_order = bumps.second_order;
      const double h = bumps.spot_rel;
      const double dv = bumps.vol_abs;
      const double ks_up = sticky_k_shift(StickyMode::StickyStrike, h);
      const double ks_dn = sticky_k_shift(StickyMode::StickyStrike, -h);
      const CurveSet cs_up = respot_curves(curves, 1.0 + h);
      const CurveSet cs_dn = respot_curves(curves, 1.0 - h);

      // Single-use slots (no `begin_recording`): each grid point below is
      // read exactly once per GROUP (vs. once per ROW on the unmemoized
      // path), so the within-row read-cache replay `eval_bump_table` relies
      // on has nothing left to buy here -- see `CachedBumpView`'s own I-3
      // "single-use slot" comment for why a live read is correct in that mode.
      BumpReadCache no_cache_up;
      BumpReadCache no_cache_dn;
      BumpReadCache no_cache_c;
      {
        const CachedBumpView<SurfaceT> view = bump_view(surface, ks_up, 0.0, no_cache_up);
        block.s_up_raw = resolve_var_swap_strip_raw(view, cs_up, T, block.cfg_pinned);
      }
      {
        const CachedBumpView<SurfaceT> view = bump_view(surface, ks_dn, 0.0, no_cache_dn);
        block.s_dn_raw = resolve_var_swap_strip_raw(view, cs_dn, T, block.cfg_pinned);
      }
      {
        const CachedBumpView<SurfaceT> view = bump_view(surface, 0.0, dv, no_cache_c);
        block.v_up_raw = resolve_var_swap_strip_raw(view, curves, T, block.cfg_pinned);
      }
      {
        const CachedBumpView<SurfaceT> view = bump_view(surface, 0.0, -dv, no_cache_c);
        block.v_dn_raw = resolve_var_swap_strip_raw(view, curves, T, block.cfg_pinned);
      }
      if (block.have_second_order) {
        BumpReadCache no_cache_pp;
        BumpReadCache no_cache_pm;
        BumpReadCache no_cache_mp;
        BumpReadCache no_cache_mm;
        {
          const CachedBumpView<SurfaceT> view = bump_view(surface, ks_up, dv, no_cache_pp);
          block.sv_pp_raw = resolve_var_swap_strip_raw(view, cs_up, T, block.cfg_pinned);
        }
        {
          const CachedBumpView<SurfaceT> view = bump_view(surface, ks_up, -dv, no_cache_pm);
          block.sv_pm_raw = resolve_var_swap_strip_raw(view, cs_up, T, block.cfg_pinned);
        }
        {
          const CachedBumpView<SurfaceT> view = bump_view(surface, ks_dn, dv, no_cache_mp);
          block.sv_mp_raw = resolve_var_swap_strip_raw(view, cs_dn, T, block.cfg_pinned);
        }
        {
          const CachedBumpView<SurfaceT> view = bump_view(surface, ks_dn, -dv, no_cache_mm);
          block.sv_mm_raw = resolve_var_swap_strip_raw(view, cs_dn, T, block.cfg_pinned);
        }
      }
    }
  }

  block.can_roll = T > bumps.time_years;
  if (block.can_roll) {
    block.t_minus_dt = T - bumps.time_years;
    DerivFlags df_flags = DerivFlags::None;
    block.df_at_Tdt = deriv_df_at_T(curves, block.t_minus_dt, df_flags);
    block.df_fallback_at_Tdt = has_flag(df_flags, DerivFlags::DfFallback);
    block.c_tdt_raw = resolve_var_swap_strip_raw(surface, curves, block.t_minus_dt, block.cfg_pinned);
    if (bumps.second_order) {
      block.have_tdt_spot_bumps = true;
      const double h = bumps.spot_rel;
      const double ks_up = sticky_k_shift(StickyMode::StickyStrike, h);
      const double ks_dn = sticky_k_shift(StickyMode::StickyStrike, -h);
      const CurveSet cs_up = respot_curves(curves, 1.0 + h);
      const CurveSet cs_dn = respot_curves(curves, 1.0 - h);
      BumpReadCache no_cache_up;
      BumpReadCache no_cache_dn;
      {
        const CachedBumpView<SurfaceT> view = bump_view(surface, ks_up, 0.0, no_cache_up);
        block.s_up_tdt_raw = resolve_var_swap_strip_raw(view, cs_up, block.t_minus_dt, block.cfg_pinned);
      }
      {
        const CachedBumpView<SurfaceT> view = bump_view(surface, ks_dn, 0.0, no_cache_dn);
        block.s_dn_tdt_raw = resolve_var_swap_strip_raw(view, cs_dn, block.t_minus_dt, block.cfg_pinned);
      }
    }
  }
}

// Marks-only entry point. Bit-identical to `deriv_price(surface, curves,
// contract, cfg)` on an in-scope VarSwap contract, reusing `block`'s
// per-(uid,T) strip instead of resolving it fresh.
template <class SurfaceT>
[[nodiscard]] Result<DerivQuote> deriv_price_var_swap_shared(const SurfaceT& surface,
                                                              const CurveSet& curves,
                                                              const DerivContract& contract,
                                                              const DerivConfig& cfg,
                                                              VarSwapSharedBlock& block) {
  ATX_TRY_VOID(validate_var_swap_dispatch(contract, cfg));
  const double T = contract.maturity_t;
  ensure_var_swap_df(block, curves, T);
  const RealizedVarianceSpec& rv = contract.rv_spec;
  const bool need_strip = rv.n_obs_total == 0u || rv.n_obs_done < rv.n_obs_total;
  if (need_strip) {
    ensure_var_swap_center_strip(block, surface, curves, T, cfg);
    if (!block.center_raw.has_value()) {
      return Err(block.center_raw.error());
    }
  }
  const std::optional<DerivQuote> center_strip =
      need_strip ? std::optional<DerivQuote>(*block.center_raw) : std::nullopt;
  return Ok(assemble_var_swap_quote(contract, block.df_at_T, block.df_fallback_at_T, center_strip));
}

// Full-greeks entry point. Bit-identical to `deriv_greeks(surface, curves,
// contract, cfg, bumps)` on an in-scope VarSwap contract: every raw strip
// value `block` supplies is exactly what a fresh per-row call would have
// resolved for the SAME (surface, T-or-T-dt, cfg) (a pure, deterministic
// function -- reusing it changes nothing about the bits), and every formula
// below is the SAME expression `deriv_greeks<SurfaceT>` uses, just fed a
// cached raw input instead of a freshly-resolved one.
template <class SurfaceT>
[[nodiscard]] Result<DerivGreeks> deriv_greeks_var_swap_shared(
    const SurfaceT& surface, const CurveSet& curves, const DerivContract& contract,
    const DerivConfig& cfg, const DerivGreekBumps& bumps, VarSwapSharedBlock& block) {
  if (!bumps_valid(bumps, surface, contract.maturity_t)) {
    return Err(ErrorCode::InvalidArgument,
               "greek bump sizes must be > 0 (spot_rel < 1, vol_abs < ATM vol)");
  }
  if (!(curves.spot > 0.0)) {
    return Err(ErrorCode::InvalidArgument, "greeks need curves.spot > 0 (delta's divisor)");
  }
  ATX_TRY_VOID(validate_var_swap_dispatch(contract, cfg));

  const double T = contract.maturity_t;
  ensure_var_swap_df(block, curves, T);

  const RealizedVarianceSpec& rv = contract.rv_spec;
  const bool need_strip = rv.n_obs_total == 0u || rv.n_obs_done < rv.n_obs_total;
  if (need_strip) {
    ensure_var_swap_center_strip(block, surface, curves, T, cfg);
    if (!block.center_raw.has_value()) {
      return Err(block.center_raw.error());
    }
  }
  const std::optional<DerivQuote> center_strip =
      need_strip ? std::optional<DerivQuote>(*block.center_raw) : std::nullopt;
  const DerivQuote center =
      assemble_var_swap_quote(contract, block.df_at_T, block.df_fallback_at_T, center_strip);

  DerivGreeks g{};
  g.pv = center.pv;
  g.quote = center;

  if (has_flag(center.flags, DerivFlags::FullyAged)) {
    const double t_nonneg = std::fmax(T, 0.0);
    g.rho = -t_nonneg * center.pv;
    g.theta = curves.yield.zero(T) * center.pv;
    g.theta_carry = g.theta;
    g.theta_zero_fixing = g.theta;
    return Ok(g);
  }

  ensure_var_swap_greeks_block(block, surface, curves, T, cfg, bumps, center);

  if (!block.pinned_center_raw.has_value()) {
    return Err(block.pinned_center_raw.error());
  }
  const auto assemble_at = [&](const Result<DerivQuote> &raw) noexcept -> double {
    const std::optional<DerivQuote> s = need_strip ? std::optional<DerivQuote>(*raw) : std::nullopt;
    return assemble_var_swap_quote(contract, block.df_at_T, block.df_fallback_at_T, s).pv;
  };
  const double pv_c = assemble_at(block.pinned_center_raw);

  const double ds = bumps.spot_rel * curves.spot;
  const double dv = bumps.vol_abs;
  if (block.have_analytic) {
    double w_future = 1.0;
    if (rv.n_obs_total > 0u) {
      const double w_done =
          static_cast<double>(rv.n_obs_done) / static_cast<double>(rv.n_obs_total);
      w_future = 1.0 - w_done;
    }
    const double scale = block.df_at_T * contract.notional * w_future;
    g.delta = scale * block.a_delta;
    g.gamma = scale * block.a_gamma;
    g.vega = scale * block.a_vega;
    g.vanna = bumps.second_order ? scale * block.a_vanna : kNaN;
    g.volga = scale * block.a_volga;
  } else {
    if (!block.s_up_raw.has_value()) return Err(block.s_up_raw.error());
    if (!block.s_dn_raw.has_value()) return Err(block.s_dn_raw.error());
    if (!block.v_up_raw.has_value()) return Err(block.v_up_raw.error());
    if (!block.v_dn_raw.has_value()) return Err(block.v_dn_raw.error());
    const double s_up = assemble_at(block.s_up_raw);
    const double s_dn = assemble_at(block.s_dn_raw);
    const double v_up = assemble_at(block.v_up_raw);
    const double v_dn = assemble_at(block.v_dn_raw);
    g.delta = (s_up - s_dn) / (2.0 * ds);
    g.gamma = (s_up - 2.0 * pv_c + s_dn) / (ds * ds);
    g.vega = (v_up - v_dn) / (2.0 * dv);
    g.volga = (v_up - 2.0 * pv_c + v_dn) / (dv * dv);
    // `sv_*` stay `kNaN` (this file's own sentinel) when `!second_order`, and
    // `g.vanna`'s UNCONDITIONAL arithmetic below then produces NaN by
    // PROPAGATION -- the same mechanism `eval_bump_table`'s own BumpPvs
    // defaults rely on -- rather than an explicit `kNaN` assignment, so the
    // NaN's bit pattern matches the unmemoized path exactly (see this
    // function's own theta/charm section for the identical reasoning).
    double sv_pp = kNaN;
    double sv_pm = kNaN;
    double sv_mp = kNaN;
    double sv_mm = kNaN;
    if (block.have_second_order) {
      if (!block.sv_pp_raw.has_value()) return Err(block.sv_pp_raw.error());
      if (!block.sv_pm_raw.has_value()) return Err(block.sv_pm_raw.error());
      if (!block.sv_mp_raw.has_value()) return Err(block.sv_mp_raw.error());
      if (!block.sv_mm_raw.has_value()) return Err(block.sv_mm_raw.error());
      sv_pp = assemble_at(block.sv_pp_raw);
      sv_pm = assemble_at(block.sv_pm_raw);
      sv_mp = assemble_at(block.sv_mp_raw);
      sv_mm = assemble_at(block.sv_mm_raw);
    }
    g.vanna = (sv_pp - sv_pm - sv_mp + sv_mm) / (4.0 * ds * dv);
  }

  // Theta / theta_carry / theta_zero_fixing / charm are computed via the SAME
  // UNCONDITIONAL formulas `deriv_greeks<SurfaceT>` always runs, fed a local
  // that stays `kNaN` (this file's own sentinel, `BumpPvs`'s default) exactly
  // when the unmemoized `eval_bump_table` would never have attempted that
  // cell -- e.g. `!can_roll` or `!second_order`. This is deliberate: it lets
  // NaN arithmetic (not an explicit branch) produce the "not computed"
  // result, which is what makes the NaN PAYLOAD bit-identical to the
  // unmemoized path (an explicit `g.theta = kNaN_literal` would not be
  // guaranteed to carry the same bit pattern the arithmetic propagation does).
  double pv_t_dn = kNaN;
  double pv_t_dn_carry = kNaN;
  double pv_t_dn_zero_fixing = kNaN;
  double pv_t_s_up = kNaN;
  double pv_t_s_dn = kNaN;

  const auto assemble_at_tdt = [&](const DerivContract &c,
                                    const Result<DerivQuote> &raw) noexcept -> double {
    const bool need = c.rv_spec.n_obs_total == 0u || c.rv_spec.n_obs_done < c.rv_spec.n_obs_total;
    const std::optional<DerivQuote> s = need ? std::optional<DerivQuote>(*raw) : std::nullopt;
    return assemble_var_swap_quote(c, block.df_at_Tdt, block.df_fallback_at_Tdt, s).pv;
  };

  DerivContract rolled = contract;
  if (block.can_roll) {
    rolled.maturity_t = block.t_minus_dt;
    if (!block.c_tdt_raw.has_value()) {
      return Err(block.c_tdt_raw.error());
    }
    pv_t_dn = assemble_at_tdt(rolled, block.c_tdt_raw);

    // Carry-theta (Task C-10). VarSwap never crosses
    // `carry_fixing_crosses_engine_boundary`'s VolSwap-only dispatch
    // boundary (unconditionally false for `kind == VarSwap`), so it is not
    // re-checked here. The injection value is `eval_bump_table`'s own
    // "K_var_future resolved fresh, at the CENTER's own T, under the SAME
    // pinned grid" -- by construction the SAME (surface, T, cfg_pinned) call
    // as `pinned_center_raw`'s (a pure, deterministic function of those three
    // inputs), so reusing it here cannot diverge from a fresh call and never
    // independently fails once `pinned_center_raw` (checked above) has.
    if (bumps.carry_theta) {
      if (rv.n_obs_total == 0u) {
        pv_t_dn_carry = pv_t_dn;
        pv_t_dn_zero_fixing = pv_t_dn;
      } else {
        const double k_var_future_at_T = block.pinned_center_raw->fair_strike_dec;
        // Task F-3: the contract's own corridor, not a hard-coded unbounded
        // one. This entry point is VarSwap-only (`validate_var_swap_shared_
        // scope`) so the pair is 0.0/0.0 here today, but reading it from the
        // contract keeps this site correct by construction rather than by a
        // whitelist in a different function -- the exact coupling F-2's
        // Criticals were made of.
        const StrikeCorridor corridor{contract.corridor_lo, contract.corridor_hi};
        DerivContract rolled_carry = rolled;
        rolled_carry.rv_spec = inject_carry_fixing(rv, k_var_future_at_T, curves.spot, corridor);
        DerivContract rolled_zero = rolled;
        rolled_zero.rv_spec = inject_carry_fixing(rv, 0.0, curves.spot, corridor);
        pv_t_dn_carry = assemble_at_tdt(rolled_carry, block.c_tdt_raw);
        pv_t_dn_zero_fixing = assemble_at_tdt(rolled_zero, block.c_tdt_raw);
      }
    }

    if (block.have_tdt_spot_bumps) {
      if (!block.s_up_tdt_raw.has_value()) return Err(block.s_up_tdt_raw.error());
      if (!block.s_dn_tdt_raw.has_value()) return Err(block.s_dn_tdt_raw.error());
      pv_t_s_up = assemble_at_tdt(rolled, block.s_up_tdt_raw);
      pv_t_s_dn = assemble_at_tdt(rolled, block.s_dn_tdt_raw);
    }
  }

  g.theta = (pv_t_dn - pv_c) / bumps.time_years;
  g.theta_carry = (pv_t_dn_carry - pv_c) / bumps.time_years;
  g.theta_zero_fixing = (pv_t_dn_zero_fixing - pv_c) / bumps.time_years;
  const double delta_rolled = (pv_t_s_up - pv_t_s_dn) / (2.0 * ds);
  g.charm = (delta_rolled - g.delta) / bumps.time_years;

  // Task F-7 fix round 1, C-1. These two were previously not assigned at all
  // here, so they kept `DerivGreeks`' struct default of 0.0 while the
  // unmemoized `deriv_greeks` produced NaN for the SAME contract under the
  // DEFAULT bumps -- a silent divergence between two paths this library
  // promises are bit-identical, and on the worse side of it: NaN reads as "not
  // computed", 0.0 reads as "measured, and this book has no skew exposure".
  //
  // Fixed the way every other conditional greek in this function is: locals
  // that stay at `kNaN` feed the SAME unconditional stencil `deriv_greeks`
  // runs, so the NaN arrives by arithmetic PROPAGATION and its payload matches
  // bit for bit (an explicit `= kNaN` literal would not guarantee that -- see
  // this function's own theta/charm comment above).
  //
  // Both locals are unconditionally NaN today because `smile_greeks` rows never
  // reach this entry point at all: `validate_var_swap_shared_scope` now rejects
  // them outright, and the book layer routes them to the unmemoized path before
  // that. Written as the full stencil rather than collapsed to a constant so
  // that wiring real smile slots into `VarSwapSharedBlock` later is a change of
  // the four inputs and nothing else.
  double pv_sk_up = kNaN;
  double pv_sk_dn = kNaN;
  double pv_cx_up = kNaN;
  double pv_cx_dn = kNaN;
  g.skew_vega = (pv_sk_up - pv_sk_dn) / (2.0 * bumps.skew_abs);
  g.convexity_vega = (pv_cx_up - pv_cx_dn) / (2.0 * bumps.convexity_abs);

  g.rho = -std::fmax(T, 0.0) * center.pv;
  return Ok(g);
}

// ── RealizedTracker ────────────────────────────────────────────────────────

Result<RealizedTracker> RealizedTracker::create(double annualization,
                                                std::uint32_t n_obs_total) {
  if (!(annualization > 0.0)) {
    return Err(ErrorCode::InvalidArgument, "annualization must be > 0");
  }
  if (n_obs_total == 0u) {
    return Err(ErrorCode::InvalidArgument, "n_obs_total must be > 0");
  }
  RealizedTracker t;
  t.rv_.annualization = annualization;
  t.rv_.n_obs_total = n_obs_total;
  return Ok(std::move(t));
}

Result<RealizedTracker> RealizedTracker::create_corridor(double annualization,
                                                          std::uint32_t n_obs_total,
                                                          double corridor_lo,
                                                          double corridor_hi) {
  // Validated through the SAME predicate the pricer uses (`corridor_valid`),
  // so a corridor a tracker will accumulate against is exactly a corridor a
  // contract can be priced against -- there is no configuration that accrues
  // here and fails there, or vice versa.
  if (!corridor_valid(StrikeCorridor{corridor_lo, corridor_hi})) {
    return Err(ErrorCode::InvalidArgument,
               "corridor bounds must be finite and >= 0 (0 == unbounded on that "
               "side), with corridor_lo < corridor_hi when both are bounded");
  }
  ATX_TRY(RealizedTracker t, create(annualization, n_obs_total));
  t.corridor_lo_ = corridor_lo;
  t.corridor_hi_ = corridor_hi;
  return Ok(std::move(t));
}

Status RealizedTracker::set_dividend_adjustment(bool on) {
  // Refused once ANY observation has landed -- including the seeding one, which
  // forms no return but does fix the prior close the next return is measured
  // from. Flipping mid-stream would leave the accumulators a mix of two
  // conventions that no consumer of the snapshot could decompose, since the
  // snapshot carries ONE flag for the whole of Sigma r^2.
  if (have_prev_) {
    return Err(ErrorCode::InvalidArgument,
               "dividend adjustment must be selected before the first observation");
  }
  rv_.include_dividend_adjustment = on;
  return Ok();
}

Status RealizedTracker::observe(double spot) {
  // This entry cannot carry a dividend, so on a single-name tracker it would
  // accrue INDEX-convention returns under a snapshot advertising the
  // single-name one. Refuse rather than accrue a lie; observe_batch inherits
  // this through its own loop below.
  if (rv_.include_dividend_adjustment) {
    return Err(ErrorCode::InvalidArgument,
               "dividend-adjusted tracker requires observe_dated(ts_ns, spot, ex_div_cash)");
  }
  return observe_impl(spot, 0.0);
}

Status RealizedTracker::observe_impl(double spot, double ex_div_cash) {
  if (!(spot > 0.0)) {
    return Err(ErrorCode::InvalidArgument, "spot must be > 0");
  }
  if (!(rv_.annualization > 0.0)) {
    return Err(ErrorCode::InvalidArgument, "tracker not initialized");
  }
  // `!(x >= 0.0)` rather than `x < 0.0` so a NaN dividend is rejected here
  // instead of silently poisoning every accumulator downstream.
  if (!(ex_div_cash >= 0.0)) {
    return Err(ErrorCode::InvalidArgument, "ex_div_cash must be finite and >= 0");
  }
  if (ex_div_cash > 0.0 && !rv_.include_dividend_adjustment) {
    return Err(ErrorCode::InvalidArgument,
               "ex_div_cash > 0 needs set_dividend_adjustment(true) (index legs are unadjusted)");
  }
  // Guard against the transposed call `observe_dated(ts, div, spot)`: both
  // parameters are `double`, so the compiler cannot catch the swap, and a
  // transposition otherwise books a wildly wrong return AND poisons `prev_spot_`
  // for every later fixing. A cash dividend larger than the price it is paid out
  // of is economically absurd, whereas the legitimate case (D = 5 against a 94
  // close) clears it easily.
  //
  // This CLOSES the transposition for every fixing this API accepts, and the
  // argument is forced rather than measured: acceptance requires D <= S, so both
  // orderings of a pair being accepted needs D <= S and S <= D, i.e. S == D,
  // where the two orderings are the same call. Swept rather than sampled by
  // `RealizedTracker.TransposedFixingIsRefusedWheneverTheOriginalIsAccepted`.
  // What remains -- a fixing this API ALREADY REJECTS transposing into an
  // accepted one -- is pinned by
  // `RealizedTracker.RejectedFixingCanTransposeIntoAnAcceptedOne`.
  //
  // 25320ee shipped this guard calling itself partial ("a small dividend against
  // a small price still passes"); 0abd59c retracted that and put the full
  // contract on `observe_dated` in derivatives.hpp. The accepted cost is a
  // liquidating distribution exceeding the residual price, refused here -- a
  // case in which "the return" has no agreed meaning anyway.
  if (ex_div_cash > spot) {
    return Err(ErrorCode::InvalidArgument,
               "ex_div_cash exceeds the observed close (arguments transposed?)");
  }

  if (!have_prev_) {
    // No prior close exists yet, so there is nothing for a dividend to adjust.
    // Rejected rather than dropped, for the same reason a dividend on an
    // unadjusted tracker is: a silently ignored corporate action is exactly the
    // failure this task exists to remove.
    if (ex_div_cash > 0.0) {
      return Err(ErrorCode::InvalidArgument,
                 "ex_div_cash > 0 on the seeding observation, which forms no return");
    }
    // First observation seeds the previous spot AND the gamma-weight anchor
    // S0 (Task F-2), written to the snapshot as `rv_.gamma_seed_spot` (Task
    // F-2 fix round 1 / C-1, so a pricer can read it back) -- both at the
    // same seed spot, and neither touched again. No return yet.
    prev_spot_ = spot;
    rv_.gamma_seed_spot = spot;
    have_prev_ = true;
    return Ok();
  }

  // Refuse once all n_obs_total returns have been observed.
  if (rv_.n_obs_done >= rv_.n_obs_total) {
    return Err(ErrorCode::InvalidArgument, "all observations already recorded");
  }

  // Task F-6 (LIT-9, ISDA/MCA): the ONLY thing a dividend moves is this
  // denominator. `ex_div_cash` is 0 on every index-convention path, so that
  // case is bit-for-bit the `prev_spot_` this line always used.
  const double prev_adjusted = prev_spot_ - ex_div_cash;
  if (!(prev_adjusted > 0.0)) {
    return Err(ErrorCode::InvalidArgument, "ex_div_cash must be < the previous close");
  }
  const double r = std::log(spot / prev_adjusted);
  rv_.sum_sq_log_returns_done += r * r;
  // Task F-2 (PV-F1 / LIT-7): Lee's w(y) = y/Y0 weight, S_i the spot AT this
  // return (the just-observed `spot`, matching the discrete estimator's own
  // convention -- the weight applies to the return just realized, not the
  // spot it started from). Task F-6 ruling: the dividend does NOT enter this
  // weight. Lee's y is the price LEVEL where the variance is earned, and on an
  // ex-div day that level is the post-drop close -- which is `spot`, already.
  rv_.sum_weighted_sq_log_returns_done += (spot / rv_.gamma_seed_spot) * r * r;
  // Task F-3 (PV-F3): THE corridor rule (`corridor_contains`, this file's one
  // statement of it), applied to `prev_spot_` -- the PREVIOUS CLOSE, the spot
  // this return was measured FROM. Note the deliberate contrast with the gamma
  // weight one line above, which reads the just-observed `spot`: a WEIGHT is
  // evaluated where the variance is earned, whereas a corridor INDICATOR must
  // be predictable with respect to the return it gates (it is the barrier test
  // a desk can actually run before the session, and it keeps the accrual a
  // martingale increment). Getting these two the same way round would be a
  // silent look-ahead in one of them.
  //
  // Task F-6 ruling: `prev_spot_`, NOT `prev_adjusted`. A corridor is a barrier
  // in TRADED PRICE space -- the desk's pre-session test is "where did the stock
  // actually close", and a dividend does not move that. The adjustment above is
  // a return-construction device for stripping a mechanical drop out of measured
  // variance, and it has no business restating where the underlying was.
  if (corridor_contains(prev_spot_, StrikeCorridor{corridor_lo_, corridor_hi_})) {
    rv_.sum_sq_log_returns_in_corridor += r * r;
    rv_.n_obs_in_corridor += 1u;
  }
  rv_.n_obs_done += 1u;
  // Task F-6 ruling: the RAW close, not `prev_adjusted`. The adjustment is
  // per-return and is consumed by the return just formed; folding it into the
  // stored mark would subtract the same dividend a second time from TOMORROW's
  // return. This also keeps `prev_spot()` a truthful report of the last close.
  prev_spot_ = spot;

  const double n = static_cast<double>(rv_.n_obs_done);
  rv_.rv_done_dec = rv_.annualization * rv_.sum_sq_log_returns_done / n;
  rv_.rv_gamma_done_dec = rv_.annualization * rv_.sum_weighted_sq_log_returns_done / n;
  // Normalized by n_obs_done, NOT by n_obs_in_corridor -- see the field's own
  // doc: this is the leg the n_done/n_total aged blend consumes, and the
  // conditional (in-corridor-normalized) reading is recovered from it plus
  // `n_obs_in_corridor` by `conditional_corridor_accrued_dec`.
  rv_.rv_corridor_done_dec = rv_.annualization * rv_.sum_sq_log_returns_in_corridor / n;
  return Ok();
}

Status RealizedTracker::observe_batch(std::span<const double> spots) {
  for (const double spot : spots) {
    ATX_TRY_VOID(observe(spot));
  }
  return Ok();
}

Status RealizedTracker::observe_dated(std::int64_t ts_ns, double spot) {
  // Task F-6: forwards with D = 0 rather than duplicating the guard, so the two
  // entries cannot drift on ordering semantics. "No dividend today" is the
  // ordinary day on a single-name tracker too, so this entry stays usable there
  // -- unlike the UNDATED `observe`, which is refused outright because it also
  // gives up the replay protection a corporate-actions-driven leg needs.
  return observe_dated(ts_ns, spot, 0.0);
}

Status RealizedTracker::observe_dated(std::int64_t ts_ns, double spot, double ex_div_cash) {
  // Ordering validated FIRST and unconditionally: a stale/replayed ts_ns
  // mutates nothing, even when observe_impl would itself have rejected the spot
  // (e.g. non-positive) or the dividend -- the caller learns "not ascending",
  // not a value-validation error that implies the timestamp was otherwise fine.
  if (ts_ns <= last_fixing_ts_ns_) {
    return Err(ErrorCode::AlreadyExists, "fixing timestamp not ascending");
  }
  ATX_TRY_VOID(observe_impl(spot, ex_div_cash));
  last_fixing_ts_ns_ = ts_ns;
  return Ok();
}

// ── Explicit instantiations (mirrors surface.cpp) ──────────────────────────
//
// SUPPORTED SET (v1 ruling, closeout item 1.2). `SurfaceT`'s whole requirement
// is `iv(k_log, T)`, so the set is a linkage decision, not a modelling one.
// Three entries, in the order a caller should reach for them:
//
//   1. `VolSurface` — the TIER-A calibration-grade surface container, and the
//      only entry a Tier-A caller can name without reaching into `detail/`.
//      This is what makes the templated overloads usable from the frozen API
//      at all: before it, every instantiation was on a demoted type, so the
//      declarations in `derivatives.hpp` were reachable only by including a
//      `detail/` header — a Tier-A signature you could not link against.
//   2. The two per-family containers demoted to `detail/legacy_surface.hpp` by
//      S4-T21. Kept ONLY for source compatibility with callers that predate
//      the demotion (in-tree: the deriv unit tests and their shared fixture).
//      Not un-demoted by appearing here — a `detail/` type reached through a
//      Tier-A function template is still a `detail/` type, and the tier
//      manifest is what says so.
//   3. Neither of the above, for the modern fitted pipeline: it produces a
//      `PricedSurface` / `SurfaceRef`, which do NOT go through this list.
//      Their entry points are the non-templated `PricedSurface`-native
//      overloads below and `detail::deriv_price_on_ref`; both instantiate
//      their own file-local log-moneyness adapters inside THIS translation
//      unit, so they need nothing here.
//
// A caller with some other `SurfaceT` adds an instantiation beside these. New
// code should not need to: it should hold a `PricedSurface` or a `SurfaceRef`.

template Result<DerivQuote> var_swap_fair_strike<VolSurface>(
    const VolSurface&, const CurveSet&, double, const DerivConfig&);
template Result<DerivQuote> vol_swap_fair_strike<VolSurface>(
    const VolSurface&, const CurveSet&, double, const DerivConfig&);
template Result<DerivQuote> deriv_price<VolSurface>(
    const VolSurface&, const CurveSet&, const DerivContract&, const DerivConfig&);
template Result<DerivGreeks> deriv_greeks<VolSurface>(
    const VolSurface&, const CurveSet&, const DerivContract&, const DerivConfig&,
    const DerivGreekBumps&);
template Result<DerivQuote> forward_var_fair_strike<VolSurface>(
    const VolSurface&, const CurveSet&, double, double, const DerivConfig&, DerivQuote*);

template Result<DerivQuote> var_swap_fair_strike<EssviSurface>(
    const EssviSurface&, const CurveSet&, double, const DerivConfig&);
template Result<DerivQuote> var_swap_fair_strike<SviSurface>(
    const SviSurface&, const CurveSet&, double, const DerivConfig&);
template Result<DerivQuote> vol_swap_fair_strike<EssviSurface>(
    const EssviSurface&, const CurveSet&, double, const DerivConfig&);
template Result<DerivQuote> vol_swap_fair_strike<SviSurface>(
    const SviSurface&, const CurveSet&, double, const DerivConfig&);
template Result<DerivQuote> deriv_price<EssviSurface>(
    const EssviSurface&, const CurveSet&, const DerivContract&, const DerivConfig&);
template Result<DerivQuote> deriv_price<SviSurface>(
    const SviSurface&, const CurveSet&, const DerivContract&, const DerivConfig&);
template Result<DerivGreeks> deriv_greeks<EssviSurface>(
    const EssviSurface&, const CurveSet&, const DerivContract&, const DerivConfig&,
    const DerivGreekBumps&);
template Result<DerivGreeks> deriv_greeks<SviSurface>(
    const SviSurface&, const CurveSet&, const DerivContract&, const DerivConfig&,
    const DerivGreekBumps&);
template Result<DerivQuote> forward_var_fair_strike<EssviSurface>(
    const EssviSurface&, const CurveSet&, double, double, const DerivConfig&, DerivQuote*);
template Result<DerivQuote> forward_var_fair_strike<SviSurface>(
    const SviSurface&, const CurveSet&, double, double, const DerivConfig&, DerivQuote*);

// ── E6 / AN-W: PricedSurface-native entry points ───────────────────────────

namespace {

// The fitted surface's OWN carry, expressed as a CurveSet so the strip resolves
// forward and discount exactly as it does on the templated path. Pillars come
// straight from the surface's fitted `context()`; between them `resolve_forward`
// applies the shared log-F convention (strip_grid.hpp, E2) and `YieldCurve`
// interpolates the per-expiry rates `rate_at` decodes from each slice's own
// discount factor.
//
// `roll_dt` (GK-C8, 0.0 default for every caller but the greeks bridge below)
// is the theta roll about to be taken against this SAME CurveSet, if any. When
// the rolled tenor T - roll_dt lands below the front fitted pillar, one extra
// forward + rate pillar is appended there (mirrors `carry_from_ref`'s rolled-
// forward fix) so the roll interpolates on the surface's own economically-
// extrapolated carry instead of clamping flat at T's -- see the call site in
// the `deriv_greeks(PricedSurface, ...)` overload.
// `also_admit_t` (Task F-4, NaN default = "no second tenor") is a SECOND
// maturity this same CurveSet must serve. `forward_var_fair_strike` prices two
// strips off ONE carry, and both of their tenors have to clear the
// fitted-range gate below -- naming the second one here keeps that a single
// gate expression against a single pillar set, instead of two carry
// resolutions whose agreement would have to be argued.
[[nodiscard]] Result<CurveSet> carry_from(const PricedSurface& ps, double T,
                                          double roll_dt = 0.0,
                                          double also_admit_t = kNaN) {
  const std::span<const SliceContext> pillars = ps.context();
  if (pillars.empty()) {
    return Err(ErrorCode::InvalidArgument, "deriv: surface carries no fitted pillar");
  }
  // FITTED-RANGE GATE (applied below, once the usable pillar set is known).
  // Between pillars this CurveSet reproduces the surface's own carry; OUTSIDE
  // them it does not, and the disagreement is not benign. `resolve_forward`
  // clamps flat past the end pillars, whereas `PricedSurface::forward_at` keeps
  // extrapolating economically (S·exp((r−q_eff)·T)). The strip prices every node
  // at F·e^x and reads its vol from `ps.iv(F·e^x, T)`, so a forward that is not
  // the surface's own would put k = 0 somewhere other than the surface's ATM and
  // bias K_var — silently. Refuse instead. A caller who genuinely wants an
  // extrapolated tenor supplies its own `CurveSet` through the templated
  // overload and owns that choice.
  if (!(T > 0.0)) {
    return Err(ErrorCode::InvalidArgument, "deriv: T must be > 0");
  }
  CurveSet cs;
  cs.spot = ps.pricing().S;

  std::vector<double> ts;
  std::vector<double> rates;
  std::vector<ForwardPoint> fwd;
  ts.reserve(pillars.size());
  rates.reserve(pillars.size());
  fwd.reserve(pillars.size());
  for (const SliceContext& p : pillars) {
    if (!(p.T > 0.0) || !(p.forward > 0.0)) {
      continue; // a degenerate pillar contributes no carry
    }
    ts.push_back(p.T);
    rates.push_back(ps.rate_at(p.T));
    ForwardPoint fp;
    fp.T = p.T;
    fp.F = p.forward;
    fp.q_eff = p.q_eff;
    fwd.push_back(fp);
  }
  if (ts.empty()) {
    return Err(ErrorCode::InvalidArgument, "deriv: surface carries no usable fitted pillar");
  }
  // FIX-E I-5. The gate runs AFTER the filter and on the SURVIVING pillars.
  // Gating on `pillars.front()/back()` while building the CurveSet from the
  // filtered list is two different pillar sets in one function: if the first or
  // last pillar is degenerate (T <= 0 or forward <= 0) it is dropped from the
  // curve but still widens the admitted range, so an admitted T could land
  // outside the surviving forward curve — exactly the flat-clamp-vs-extrapolate
  // disagreement the gate exists to prevent, reopened on the degenerate-pillar
  // path. `ts` is the correct set: it is the one the curve is built from, so
  // "admitted" and "interpolated rather than clamped" become the same
  // condition by construction. (`ps.context()` is ascending in T, and the
  // filter preserves order, so front/back of `ts` are its min/max.)
  const bool second_out_of_range =
      std::isfinite(also_admit_t) && (also_admit_t < ts.front() || also_admit_t > ts.back());
  if (T < ts.front() || T > ts.back() || second_out_of_range) {
    return Err(ErrorCode::OutOfRange,
               "deriv: T is outside the surface's usable fitted pillar range; the "
               "PricedSurface overloads do not extrapolate carry");
  }

  // FRONT-PILLAR ROLLED-FORWARD FIX (GK-C8). Both curves are extended: below
  // the front pillar `resolve_forward` clamps the forward flat (mis-centering
  // the strip's k=0, see the file comment above) and `YieldCurve` clamps
  // log(df) flat -- a flat DISCOUNT, not a flat rate, dropping theta's own
  // discount-roll term (same failure mode `carry_from_ref`'s kFlatYieldFloorFrac
  // pillar exists to fix). `ps.rate_at(t_rolled)` and `ps.forward_at(t_rolled)`
  // read the surface's OWN economic extrapolation below its front pillar
  // (interp_forward: rate held flat at the front slice's own rate, forward
  // grown off it) -- exactly what carry_from_ref fakes with two pillars
  // carrying one rate, available here for real. Inserted at the front keeps
  // both `ts`/`rates` and `fwd` ascending, which `resolve_forward` and
  // `YieldCurve` require.
  const double t_rolled = T - roll_dt;
  if (roll_dt > 0.0 && t_rolled > 0.0 && t_rolled < ts.front()) {
    ForwardPoint fp;
    fp.T = t_rolled;
    fp.F = ps.forward_at(t_rolled);
    fp.q_eff = ps.q_eff_at(t_rolled);
    fwd.insert(fwd.begin(), fp);
    ts.insert(ts.begin(), t_rolled);
    rates.insert(rates.begin(), ps.rate_at(t_rolled));
  }

  ATX_TRY_VOID(cs.set_yield(ts, rates));
  cs.forward.set(fwd);
  return Ok(std::move(cs));
}

// Presents a PricedSurface through the LOG-MONEYNESS `iv(k_log, T)` contract the
// strip templates require. `PricedSurface::iv` is STRIKE-based, so the
// conversion has to happen somewhere, and it MUST use the same forward the strip
// itself uses — otherwise the vol would be read at one strike while the price is
// computed at another. Hence `resolve_forward(*curves, T)` here rather than
// `ps->forward_at(T)`: inside the fitted pillar range (the only range
// `carry_from` admits) the two agree, and using the strip's own forward is what
// keeps the two reads on the same strike by construction.
// FIX-E M-8: the forward is CONSTANT across a strip but `resolve_forward` is a
// linear scan over the pillars, and the strip calls `iv` once per node (97-2049
// times). Resolve it once for the strip's own tenor at construction and reuse it
// whenever the query T matches; a query at any other T (nothing does today, but
// the templates are free to) falls back to the full resolve, so the cache is an
// optimisation and never a behaviour change.
//
// Task P-1 (PV-P1/FIT-P2/GK-P): `ps->iv(K, T)` ALSO redoes its own
// `interp_forward(T)` + `CurveSurface::bracket(T)` on every single call --
// work that is just as T-invariant as `F_cached` above, and just as wasted
// across the same 97-2049 per-node reads (plus, per GK-P, across the up-to-17
// bumped/rolled repricings a greek stencil runs at the SAME T; see
// `eval_bump_table`). `carry_cached` resolves that pair ONCE here, exactly
// the way `PricedSurface::evaluate_batch`'s own bit-identical-T ladder reuse
// already does (see its "T-bracket and carry are resolved ONCE and reused"
// comment), and `iv_with_carry` consumes it in place of the redundant
// `ps->iv`. `T_other_cached`/`F_other_cached`/`carry_other_cached` are a
// second, LAZY slot for the one other T the theta/charm stencil ever queries
// this same view at -- the roll to `T - dt` (deriv_greeks' eval_bump_table,
// and C-10's two carry_theta reprices, all reuse that identical rolled T; see
// the header note above `DerivGreeks::theta_carry`). Resolved on first miss,
// reused for every later node/repricing at that T; a THIRD distinct T
// (nothing does today) simply evicts and re-resolves -- always correct, only
// loses the cache hit.
struct PricedSurfaceStripView {
  const PricedSurface* ps;
  const CurveSet* curves;
  double T_cached;
  double F_cached;
  SurfaceStripCarry carry_cached;
  // FIT-C7 / Task C-6: the caller-supplied certified band, if any --
  // `resolve_wing_clamp` reads this via `surface_certified_wing_band`'s
  // structural detection. `std::nullopt` (no band supplied) is
  // indistinguishable from "this adapter carries no provenance", which is
  // exactly the mode-blind-default behaviour a caller who does not know (or
  // does not care about) the surface's build quality mode should keep.
  std::optional<double> certified_wing_band;

  // `mutable`: a read-through cache over pure functions of T (interp_forward /
  // bracket / resolve_forward never depend on anything but T and this
  // surface's/curve-set's own immutable state), not a change in what `iv`
  // reports for a given (k_log, T) -- see the bit-identity note above.
  mutable double T_other_cached = kNaN;
  mutable double F_other_cached = kNaN;
  mutable SurfaceStripCarry carry_other_cached{};

  PricedSurfaceStripView(const PricedSurface* surface, const CurveSet* cs, double T,
                         std::optional<double> band = std::nullopt) noexcept
      : ps{surface}, curves{cs}, T_cached{T}, F_cached{resolve_forward(*cs, T)},
        carry_cached{surface->strip_carry_at(T)}, certified_wing_band{band} {}

  [[nodiscard]] double iv(double k_log, double T) const noexcept {
    if (T == T_cached) {
      return iv_at(k_log, F_cached, carry_cached);
    }
    // Raw double compare, no tolerance -- the same "bit-identical T" ladder
    // convention `evaluate_batch` uses: a stencil always rolls to the exact
    // same `contract.maturity_t - bumps.time_years` value, never a
    // recomputed-and-therefore-possibly-different one.
    if (T != T_other_cached) {
      T_other_cached = T;
      F_other_cached = resolve_forward(*curves, T);
      carry_other_cached = ps->strip_carry_at(T);
    }
    return iv_at(k_log, F_other_cached, carry_other_cached);
  }

  // Task P-3 / PV-P4: batched sibling of `iv(k_log, T)`, consumed by
  // var_swap_fair_strike's node loop (`has_strip_iv_batch` /
  // `strip_batch_disabled_for_test` decide whether it is reached). `x` holds
  // ALREADY-CLAMPED log-moneyness values -- the caller applies the wing clamp
  // itself, exactly as every `iv(x_read, T)` call already does. Fast path
  // handles the strip's own tenor (T == T_cached); var_swap_fair_strike's
  // node loop never queries any other T in one call, so the `iv()` fallback
  // below (correct, just unbatched) is never actually reached in production.
  //
  // `out[i] = F_cached * std::exp(x[i])` reproduces `iv_at`'s own
  // `F * std::exp(k_log)` round-trip verbatim, so the strike this hands to
  // `PricedSurface::iv_batch` is bit-identical to what `iv_at` would compute
  // per node; `iv_batch`'s own K-validity check (`isfinite(K) && K > 0.0`)
  // then reproduces `iv_at`'s `!(F > 0.0) || !isfinite(k_log)` guard exactly
  // -- every combination that trips one trips the other (F <= 0 or a
  // non-finite x makes K non-finite or non-positive), so no separate guard is
  // needed here. `out` doubles as K's scratch buffer: each element is READ
  // into the resolution call before that SAME index is overwritten with
  // sigma, so the aliasing is safe in-place transform, not a hazard.
  void iv_batch(std::span<const double> x, double T, std::span<double> out) const noexcept {
    const std::size_t n = std::min(x.size(), out.size());
    if (T != T_cached) {
      for (std::size_t i = 0; i < n; ++i) {
        out[i] = iv(x[i], T);
      }
      return;
    }
    for (std::size_t i = 0; i < n; ++i) {
      out[i] = F_cached * std::exp(x[i]);
    }
    ps->iv_batch(out.first(n), T_cached, out.first(n));
  }

private:
  [[nodiscard]] double iv_at(double k_log, double F,
                             const SurfaceStripCarry& carry) const noexcept {
    if (!(F > 0.0) || !std::isfinite(k_log)) {
      return kNaN;
    }
    return ps->iv_with_carry(F * std::exp(k_log), carry);
  }
};

} // namespace

namespace detail {
void set_strip_batch_disabled_for_test(bool disabled) noexcept {
  g_strip_batch_disabled.store(disabled, std::memory_order_relaxed);
}
void set_bump_read_cache_disabled_for_test(bool disabled) noexcept {
  g_bump_read_cache_disabled.store(disabled, std::memory_order_relaxed);
}
void reset_deriv_greeks_reprice_count_for_test() noexcept {
  g_deriv_greeks_reprices.store(0u, std::memory_order_relaxed);
}
std::uint64_t deriv_greeks_reprice_count_for_test() noexcept {
  return g_deriv_greeks_reprices.load(std::memory_order_relaxed);
}

// A default-constructed `BumpReadCache` is in I-3's "single-use slot" mode
// (`recording == false`, never sorted), so every read goes straight to the
// surface -- which is what these probes want: the view's arithmetic, with no
// cache state to reason about.
double skew_shifted_iv_for_test(const EssviSurface& surface, double k_log, double T,
                                double slope) noexcept {
  BumpReadCache cache;
  return skew_bump_view(surface, slope, cache).iv(k_log, T);
}

double convex_shifted_iv_for_test(const EssviSurface& surface, double k_log, double T,
                                  double curvature) noexcept {
  BumpReadCache cache;
  return convex_bump_view(surface, curvature, cache).iv(k_log, T);
}

// Mirrors `deriv_greeks`' own centre-then-pin sequence exactly, because a
// difference taken on an UNPINNED grid would differ from the production
// stencil by a change of quadrature rather than a change of price -- the very
// contamination `pin_center_scheme` exists to prevent. Everything past the pin
// is deliberately NOT shared with the bump table.
Result<double> deriv_pv_skew_shifted_for_test(const EssviSurface& surface, const CurveSet& curves,
                                              const DerivContract& contract,
                                              const DerivConfig& cfg, double slope) {
  ATX_TRY(const DerivQuote center, deriv_price(surface, curves, contract, cfg));
  const DerivConfig cfg_pinned = pin_center_scheme(cfg, center);
  BumpReadCache cache;
  return skew_bumped_pv(surface, curves, contract, cfg_pinned, slope, cache);
}
} // namespace detail

Result<DerivQuote> var_swap_fair_strike(const PricedSurface& surface, double T,
                                        const DerivConfig& cfg,
                                        std::optional<double> surface_certified_wing_band) {
  ATX_TRY(const CurveSet curves, carry_from(surface, T));
  const PricedSurfaceStripView view{&surface, &curves, T, surface_certified_wing_band};
  return var_swap_fair_strike(view, curves, T, cfg);
}

Result<DerivQuote> vol_swap_fair_strike(const PricedSurface& surface, double T,
                                        const DerivConfig& cfg,
                                        std::optional<double> surface_certified_wing_band) {
  ATX_TRY(const CurveSet curves, carry_from(surface, T));
  const PricedSurfaceStripView view{&surface, &curves, T, surface_certified_wing_band};
  return vol_swap_fair_strike(view, curves, T, cfg);
}

Result<DerivQuote> forward_var_fair_strike(const PricedSurface& surface, double T1, double T2,
                                          const DerivConfig& cfg,
                                          std::optional<double> surface_certified_wing_band,
                                          DerivQuote* diagnostic_out) {
  // The tenor contract is checked HERE, not left to `carry_from`, because
  // `carry_from` answers a different question and would answer this one with
  // the wrong code (a +Inf T1 clears its `> 0.0` guard and then trips its
  // fitted-range gate as OutOfRange). Same helper `forward_var_core` uses --
  // it is a pure function of the two doubles, so the second call inside the
  // core cannot disagree with this one; and the out-parameter reset is
  // likewise idempotent.
  if (diagnostic_out != nullptr) {
    *diagnostic_out = DerivQuote{};
  }
  ATX_TRY_VOID(validate_forward_var_tenors(T1, T2));
  // `carry_from` owns the fitted-range gate; naming T2 as the second admitted
  // tenor makes ONE carry resolution serve both legs and gate both of them.
  ATX_TRY(const CurveSet curves, carry_from(surface, T1, /*roll_dt=*/0.0, /*also_admit_t=*/T2));
  // ONE band value reaching both views, exactly as ONE cfg reaches both legs.
  const PricedSurfaceStripView leg1{&surface, &curves, T1, surface_certified_wing_band};
  const PricedSurfaceStripView leg2{&surface, &curves, T2, surface_certified_wing_band};
  return forward_var_core(leg1, leg2, curves, T1, T2, cfg, diagnostic_out);
}

Result<DerivQuote> deriv_price(const PricedSurface& surface, const DerivContract& contract,
                               const DerivConfig& cfg,
                               std::optional<double> surface_certified_wing_band) {
  ATX_TRY(const CurveSet curves, carry_from(surface, contract.maturity_t));
  const PricedSurfaceStripView view{&surface, &curves, contract.maturity_t,
                                    surface_certified_wing_band};
  return deriv_price(view, curves, contract, cfg);
}

Result<DerivGreeks> deriv_greeks(const PricedSurface& surface, const DerivContract& contract,
                                 const DerivConfig& cfg, const DerivGreekBumps& bumps,
                                 std::optional<double> surface_certified_wing_band) {
  // The fitted-range gate is paid ONCE here, on the contract's own maturity;
  // the theta roll below reuses this same carry with a shorter contract T (see
  // the header) rather than re-deriving it at T - dt. Passing bumps.time_years
  // as the roll lets carry_from append its front-pillar rolled carry pillar
  // (GK-C8) when this contract's T sits at or near the surface's own front
  // pillar -- the one case the shared carry would otherwise clamp.
  ATX_TRY(const CurveSet curves,
          carry_from(surface, contract.maturity_t, bumps.time_years));
  const PricedSurfaceStripView view{&surface, &curves, contract.maturity_t,
                                    surface_certified_wing_band};
  return deriv_greeks(view, curves, contract, cfg, bumps);
}

// ── Task 9 / DerivBook: SurfaceRef-native bridge ───────────────────────────
//
// See include/atx/vol/detail/deriv_ref_bridge.hpp for why these two functions
// live in THIS translation unit: they instantiate the strip templates over a
// third surface adapter, and the template bodies are here.

namespace detail {
namespace {

// Lower yield pillar as a fraction of the contract tenor. TWO pillars carrying
// the SAME zero rate make the curve genuinely flat in RATE: with exactly two
// pillars the Fritsch-Carlson tangents both equal the secant, so the Hermite
// interpolant of log(df) is exactly the straight line -r*t between them and
// `disc(t) == e^{-r*t}` for every t in [frac*T, T].
//
// A SINGLE pillar would not do this. `YieldCurve` extrapolates log(df) FLAT
// outside its pillar range, so a one-pillar curve returns the SAME discount
// factor e^{-r*T} at every tenor -- a flat DISCOUNT, not a flat yield. The mark
// at T is identical either way (T is a pillar in both), but `deriv_greeks`'
// theta stencil reprices at T - dt, and under a frozen discount that repricing
// silently drops theta's r*PV discount-roll term. The second pillar costs one
// vector element and makes the roll exact.
//
// 1e-3 puts the floor pillar far enough below any roll the greek stencil takes
// that `disc` interpolates rather than clamps. The one exception is a contract
// with T in (dt, ~1.001*dt) -- i.e. within a tenth of a percent of the roll size
// itself -- where T - dt falls under the floor and df clamps at e^{-r*1e-3*T}.
// On a contract expiring in about a day that differs from the exact
// e^{-r*(T-dt)} by well under 1e-6: theta is then microscopically off, never
// dropped, and the term it exists to capture is itself ~0 there.
constexpr double kFlatYieldFloorFrac = 1.0e-3;

// The borrowed surface's OWN carry, expressed as a CurveSet so the strip
// resolves forward and discount exactly as it does on every other path.
//
// `roll_dt` is the theta roll the caller is about to take (0 when it takes
// none). It exists because the curve must carry a forward pillar at the ROLLED
// tenor as well. `resolve_forward` clamps outside the pillar range, so with a
// lone pillar at T a repricing at T - dt would read F(T) while the surface's
// smile stays anchored at its own F(T - dt): the strip's k = 0 would then land
// at k = ln(F(T)/F(T-dt)) = (r - q)*dt ON THE SMILE instead of at its ATM point.
// On a skewed name that MIS-CENTERING biases K_var by about
// 2*sigma*(dsigma/dk)*(r-q)*dt, which theta promptly divides by dt -- a
// first-order error in theta, comparable to and opposing the discount-roll term.
// Two pillars make the rolled repricing read the surface's own forward at its
// own residual tenor, which is exactly what the multi-pillar E6 `carry_from`
// gives the PricedSurface path.
//
// (An earlier revision justified the lone pillar by claiming it was needed to
// keep the adapter's vol read and the strip's strike on the same K. That was
// WRONG: both resolve F through this same CurveSet, so they agree at ANY pillar
// count -- which is precisely why the E6 path has always been free to carry
// every fitted pillar.)
//
// UNLIKE the E6 `carry_from`, no fitted-range gate is applied. Not because it is
// impossible -- an owned handle could reach `owned()->context()` -- but because a
// view-backed `PricedSurfaceView` exposes no pillar list, so gating would make
// the two `SurfaceRef` forms behave DIFFERENTLY on the same surface. Uniform
// behaviour is chosen instead, and the resulting tenor-hygiene obligation is
// documented on the caller-facing API (deriv_book.hpp).
[[nodiscard]] Result<CurveSet> carry_from_ref(const SurfaceRef& ref, double T, double roll_dt) {
  CurveSet cs;
  cs.spot = ref.pricing().S;

  // Ascending in T -- `resolve_forward`'s documented precondition. A roll that
  // would land at or before the valuation date contributes no pillar; the
  // greek stencil skips theta/charm on exactly that condition (`can_roll`), and
  // the at-expiry path never reads a forward at all.
  ForwardPoint pts[2];
  std::size_t n_pts = 0;
  const double t_rolled = T - roll_dt;
  if (roll_dt > 0.0 && t_rolled > 0.0) {
    pts[n_pts].T = t_rolled;
    pts[n_pts].F = ref.forward_at(t_rolled);
    pts[n_pts].q_eff = ref.q_eff_at(t_rolled);
    ++n_pts;
  }
  pts[n_pts].T = T;
  pts[n_pts].F = ref.forward_at(T);
  pts[n_pts].q_eff = ref.q_eff_at(T);
  ++n_pts;
  cs.forward.set(std::span<const ForwardPoint>{pts, n_pts});

  // T <= 0 is the at-expiry case: `deriv_df_at_T` short-circuits df = 1 there
  // and never consults the curve, so a default (empty) YieldCurve -- which
  // itself returns 1.0 -- is the correct and only representable answer. Feeding
  // a non-positive pillar to `set_yield` would be a fabricated rate.
  if (T > 0.0) {
    const double r = ref.rate_at(T);
    const double ts[] = {T * kFlatYieldFloorFrac, T};
    const double rates[] = {r, r};
    ATX_TRY_VOID(cs.set_yield(ts, rates));
  }
  return Ok(std::move(cs));
}

// Presents a SurfaceRef through the LOG-MONEYNESS `iv(k_log, T)` contract the
// strip templates require. Mirrors `PricedSurfaceStripView` above -- including
// the reason it resolves F from the CurveSet rather than from the surface: the
// vol must be read at the strike the strip is pricing, and the strip's own
// forward is what makes that true by construction. Same base+rolled carry
// hoist too (Task P-1), through `SurfaceRef::strip_carry_at` /
// `iv_with_carry`, which forward to whichever of PricedSurface /
// PricedSurfaceView this handle borrows (see SurfaceStripCarry,
// priced_surface.hpp) -- so this view's own code needs no is_view() branch of
// its own, exactly like every other SurfaceRef accessor.
struct SurfaceRefStripView {
  const SurfaceRef* ref; // non-owning, non-null, valid()
  const CurveSet* curves;
  double T_cached;
  double F_cached;
  SurfaceStripCarry carry_cached;
  // FIT-C7 / Task C-6: mirrors `PricedSurfaceStripView::certified_wing_band`
  // above -- see that member's comment.
  std::optional<double> certified_wing_band;

  // Lazy second slot for the rolled T -dt the greek stencil reprices at --
  // see `PricedSurfaceStripView`'s header comment for the full rationale.
  mutable double T_other_cached = kNaN;
  mutable double F_other_cached = kNaN;
  mutable SurfaceStripCarry carry_other_cached{};

  SurfaceRefStripView(const SurfaceRef* handle, const CurveSet* cs, double T,
                      std::optional<double> band = std::nullopt) noexcept
      : ref{handle}, curves{cs}, T_cached{T}, F_cached{resolve_forward(*cs, T)},
        carry_cached{handle->strip_carry_at(T)}, certified_wing_band{band} {}

  [[nodiscard]] double iv(double k_log, double T) const noexcept {
    if (T == T_cached) {
      return iv_at(k_log, F_cached, carry_cached);
    }
    if (T != T_other_cached) {
      T_other_cached = T;
      F_other_cached = resolve_forward(*curves, T);
      carry_other_cached = ref->strip_carry_at(T);
    }
    return iv_at(k_log, F_other_cached, carry_other_cached);
  }

private:
  [[nodiscard]] double iv_at(double k_log, double F,
                             const SurfaceStripCarry& carry) const noexcept {
    if (!(F > 0.0) || !std::isfinite(k_log)) {
      return kNaN;
    }
    return ref->iv_with_carry(F * std::exp(k_log), carry);
  }
};

}  // namespace

Result<DerivQuote> deriv_price_on_ref(const SurfaceRef& ref, const DerivContract& contract,
                                      const DerivConfig& cfg,
                                      std::optional<double> surface_certified_wing_band) {
  if (!ref.valid()) {
    return Err(ErrorCode::InvalidArgument, "deriv: null surface handle");
  }
  // No roll happens in pricing, so the carry needs no rolled forward pillar.
  ATX_TRY(const CurveSet curves, carry_from_ref(ref, contract.maturity_t, 0.0));
  const SurfaceRefStripView view{&ref, &curves, contract.maturity_t, surface_certified_wing_band};
  return deriv_price(view, curves, contract, cfg);
}

// Task F-8: ONE shocked repricing, for the scenario grid's Exact deriv cell.
//
// This is the deriv counterpart of the option grid's "sticky-strike, NO smile
// roll" Exact cell (scenario_grid.hpp): the surface is not re-fit, the smile is
// not rolled, and the shocked read is the SAME `SurfaceOverlay` composition
// every greek bump already prices under -- which is what makes a Taylor cell
// and an Exact cell comparable rather than two different models.
//
// The rate shock is applied as an exact discount rescale rather than a curve
// bump. Every product here is a discounted expectation whose undiscounted leg
// does not see the discount curve at all (`rho = -T*pv`, unconditionally, on
// all four assembly paths), so `pv * exp(-dr*T')` IS the repriced value under a
// parallel shift, not an approximation of it.
//
// ── CENTRE-THEN-PIN, FOR THE SAME REASON `deriv_greeks` DOES IT ────────────
//
// The shocked reprice runs under `pin_center_scheme(cfg, centre)`, never under
// the raw `cfg`. Without it, a shocked evaluation re-resolves its own strip
// grid, re-reads its own wing band, and -- the one that actually bites --
// RE-CALIBRATES the vol-of-vol at the shocked point, so the difference against
// the base carries a change of MODEL rather than a change of price.
//
// MEASURED on the scenario leg before this pin was added: the Taylor-vs-Exact
// gap converged at O(h) rather than O(h^3) for every kind whose payoff runs
// through the distribution model -- VolSwap 89.03 -> 44.34 -> 22.13 as the shock
// halved twice (ratio 2, i.e. a first-order disagreement worth ~1% of the cell),
// and the same for both capped kinds and VariancePut. VarSwap, CorridorVarSwap
// and VarianceCall converged at ratio ~8 throughout and hid it completely.
//
// This is the sequence `deriv_greeks` and `deriv_pv_skew_shifted_for_test`
// already run. It belongs here rather than at the call site so the rule has one
// home instead of three.
//
// `centre` is the caller's already-priced unbumped quote when it has one --
// `DerivPriceRow::greeks.quote` is exactly that, so a book-driven caller pays
// nothing extra. Pass nullptr and this prices the centre itself: correct
// standalone, at the cost of one extra strip.
Result<DerivQuote> deriv_price_shocked_on_ref(const SurfaceRef& ref, const DerivContract& contract,
                                              const DerivConfig& cfg,
                                              std::optional<double> surface_certified_wing_band,
                                              const DerivShock& shock, const DerivQuote* centre) {
  if (!ref.valid()) {
    return Err(ErrorCode::InvalidArgument, "deriv: null surface handle");
  }
  if (!std::isfinite(shock.spot_rel) || shock.spot_rel <= -1.0) {
    return Err(ErrorCode::InvalidArgument,
               "deriv_price_shocked_on_ref: spot_rel must be finite and > -1");
  }
  if (!std::isfinite(shock.vol_shift) || !std::isfinite(shock.skew_shift) ||
      !std::isfinite(shock.convexity_shift) || !std::isfinite(shock.rate_shift) ||
      !std::isfinite(shock.time_roll)) {
    return Err(ErrorCode::InvalidArgument, "deriv_price_shocked_on_ref: non-finite shock");
  }

  DerivContract rolled = contract;
  rolled.maturity_t = contract.maturity_t - shock.time_roll;
  if (!(rolled.maturity_t > 0.0)) {
    return Err(ErrorCode::InvalidArgument,
               "deriv_price_shocked_on_ref: time_roll consumes the whole tenor");
  }

  // The centre is resolved at the UNROLLED, UNSHOCKED contract -- that is the
  // evaluation the shocked one is differenced against, and pinning off anything
  // else would reintroduce the drift this pin exists to remove.
  DerivQuote owned_centre{};
  if (centre == nullptr) {
    ATX_TRY(owned_centre,
            deriv_price_on_ref(ref, contract, cfg, surface_certified_wing_band));
    centre = &owned_centre;
  }
  const DerivConfig cfg_pinned = pin_center_scheme(cfg, *centre);

  // The carry snapshot is taken at the ROLLED tenor, because that is the tenor
  // being priced; asking for a roll on top would carry a pillar nothing reads.
  ATX_TRY(const CurveSet base_curves, carry_from_ref(ref, rolled.maturity_t, 0.0));
  const CurveSet curves = respot_curves(base_curves, 1.0 + shock.spot_rel);
  const SurfaceRefStripView base_view{&ref, &base_curves, rolled.maturity_t,
                                      surface_certified_wing_band};
  const SurfaceOverlay<SurfaceRefStripView> view{
      .base = &base_view,
      .vol_shift = shock.vol_shift,
      .skew_shift = shock.skew_shift,
      .convexity_shift = shock.convexity_shift,
      .k_shift = sticky_k_shift(StickyMode::StickyStrike, shock.spot_rel)};
  ATX_TRY(DerivQuote q, deriv_price(view, curves, rolled, cfg_pinned));
  const double df_shift = std::exp(-shock.rate_shift * rolled.maturity_t);
  q.pv *= df_shift;
  return Ok(q);
}

// Task F-8 S4. Three surface reads and one curve read, in the k = ln(K/F)
// convention every smile sensitivity in this file already differentiates -- see
// the declaration for why that matching matters more than the sampling scheme.
Result<SurfaceSmileSample> sample_smile_on_ref(const SurfaceRef& ref, double T, double h) {
  if (!ref.valid()) {
    return Err(ErrorCode::InvalidArgument, "sample_smile_on_ref: null surface handle");
  }
  if (!(T > 0.0) || !std::isfinite(T) || !(h > 0.0) || !std::isfinite(h)) {
    return Err(ErrorCode::InvalidArgument,
               "sample_smile_on_ref: T and h must be finite and positive");
  }
  ATX_TRY(const CurveSet curves, carry_from_ref(ref, T, 0.0));
  const double F = resolve_forward(curves, T);
  if (!(F > 0.0)) {
    return Err(ErrorCode::InvalidArgument, "sample_smile_on_ref: non-positive forward");
  }

  const SurfaceStripCarry carry = ref->strip_carry_at(T);
  const double s_dn = ref->iv_with_carry(F * std::exp(-h), carry);
  const double s_0 = ref->iv_with_carry(F, carry);
  const double s_up = ref->iv_with_carry(F * std::exp(h), carry);

  SurfaceSmileSample out{};
  out.sigma_atm = s_0;
  out.skew_slope = (s_up - s_dn) / (2.0 * h);
  // HALF the second derivative: `convexity_shift` adds `c*k^2`, so the
  // observable that pairs with it is c, and the central second difference is
  // 2c. A factor of two here would silently halve or double every convexity
  // attribution while leaving the identity intact.
  out.smile_curvature = (s_up - 2.0 * s_0 + s_dn) / (2.0 * h * h);
  // The same accessor `deriv_greeks`' own theta leg reads (`g.theta =
  // curves.yield.zero(T) * center.pv`), so the rate this differences is the
  // rate `rho` was derived against.
  out.zero_rate = curves.yield.zero(T);
  return Ok(out);
}

Result<DerivGreeks> deriv_greeks_on_ref(const SurfaceRef& ref, const DerivContract& contract,
                                        const DerivConfig& cfg, const DerivGreekBumps& bumps,
                                        std::optional<double> surface_certified_wing_band) {
  if (!ref.valid()) {
    return Err(ErrorCode::InvalidArgument, "deriv: null surface handle");
  }
  // The carry snapshot is taken ONCE, at the contract's own maturity, and every
  // bumped evaluation reuses it, so a stencil never differences two
  // differently-derived carries. It is told the roll up front so it can carry a
  // forward pillar at the rolled tenor too -- see `carry_from_ref`.
  ATX_TRY(const CurveSet curves, carry_from_ref(ref, contract.maturity_t, bumps.time_years));
  const SurfaceRefStripView view{&ref, &curves, contract.maturity_t, surface_certified_wing_band};
  return deriv_greeks(view, curves, contract, cfg, bumps);
}

// Fix round 1, I-3. `VarSwapSharedBlock` is only a valid cache for a VarSwap
// contract priced with no discrete-monitoring correction (see
// `resolve_var_swap_strip_raw`'s own doc for why `Diffusion1OverN` cannot
// safely share the raw strip) -- both `*_on_ref_shared` entry points below
// document this as a precondition, and BOTH used to leave it unenforced:
// violating it silently returns a WRONG number (a VolSwap priced through the
// variance-swap formula; a Diffusion1OverN correction silently dropped) with
// no `Err`, the same failure shape as P-4's shipped C-1. Enforced here,
// centrally, as an `Err(InvalidArgument, ...)` rather than an `assert`:
// these are already `Result`-returning entry points, so a Result-typed
// rejection costs nothing new at any call site and — unlike `assert` — fails
// the same way in every build configuration, not just when assertions are
// compiled in. `price_deriv_book` (this scope's only caller today) already
// gates on both conditions before ever reaching here (`var_swap_memo_
// eligible`, deriv_book.cpp); this closes the door for the next one.
[[nodiscard]] Status validate_var_swap_shared_scope(const DerivContract& contract,
                                                     const DerivConfig& cfg) {
  if (contract.kind != DerivKind::VarSwap) {
    return Err(ErrorCode::InvalidArgument,
               "deriv: shared-block entry point requires DerivKind::VarSwap");
  }
  if (cfg.discrete_correction_mode != DerivDiscreteCorrection::None) {
    return Err(ErrorCode::InvalidArgument,
               "deriv: shared-block entry point requires discrete_correction_mode == None");
  }
  return Ok();
}

// Task P-6 (GK-P book memo). Mirrors `deriv_price_on_ref` exactly, just
// sourcing/extending the strip through `block` (a caller-owned, per-(uid,T)
// memo entry) instead of resolving it fresh -- see
// `deriv_price_var_swap_shared`'s own doc for the bit-identity argument.
Result<DerivQuote> deriv_price_var_swap_on_ref_shared(const SurfaceRef& ref,
                                                       const DerivContract& contract,
                                                       const DerivConfig& cfg,
                                                       VarSwapSharedBlock& block,
                                                       std::optional<double> surface_certified_wing_band) {
  if (!ref.valid()) {
    return Err(ErrorCode::InvalidArgument, "deriv: null surface handle");
  }
  ATX_TRY_VOID(validate_var_swap_shared_scope(contract, cfg));
  ATX_TRY(const CurveSet curves, carry_from_ref(ref, contract.maturity_t, 0.0));
  const SurfaceRefStripView view{&ref, &curves, contract.maturity_t, surface_certified_wing_band};
  return deriv_price_var_swap_shared(view, curves, contract, cfg, block);
}

// Task P-6. Mirrors `deriv_greeks_on_ref` exactly, sourcing/extending the
// strip and market-bump grid through `block` -- see
// `deriv_greeks_var_swap_shared`'s own doc.
Result<DerivGreeks> deriv_greeks_var_swap_on_ref_shared(const SurfaceRef& ref,
                                                         const DerivContract& contract,
                                                         const DerivConfig& cfg,
                                                         const DerivGreekBumps& bumps,
                                                         VarSwapSharedBlock& block,
                                                         std::optional<double> surface_certified_wing_band) {
  if (!ref.valid()) {
    return Err(ErrorCode::InvalidArgument, "deriv: null surface handle");
  }
  ATX_TRY_VOID(validate_var_swap_shared_scope(contract, cfg));
  // Task F-7 fix round 1, C-1. `VarSwapSharedBlock` carries no smile-bump
  // slots, so this entry point cannot serve `skew_vega`/`convexity_vega` and
  // must not pretend to: silently returning NaN for a greek the caller
  // explicitly asked for is the failure this whole review round is about.
  // Enforced HERE, not in `validate_var_swap_shared_scope`, because that
  // predicate is shared with the marks-only entry point, which legitimately
  // ignores `bumps` entirely.
  //
  // `price_deriv_book` already routes such rows to the unmemoized path before
  // reaching this line (deriv_book.cpp); this makes the bit-identity contract
  // above true by CONSTRUCTION rather than by that routing staying correct --
  // the same "close the door for the next caller" reasoning
  // `validate_var_swap_shared_scope` itself was added for.
  if (bumps.smile_greeks) {
    return Err(ErrorCode::InvalidArgument,
               "deriv: shared-block greeks cannot serve smile_greeks (no smile slots in the "
               "shared block) -- use deriv_greeks_on_ref");
  }
  ATX_TRY(const CurveSet curves, carry_from_ref(ref, contract.maturity_t, bumps.time_years));
  const SurfaceRefStripView view{&ref, &curves, contract.maturity_t, surface_certified_wing_band};
  return deriv_greeks_var_swap_shared(view, curves, contract, cfg, bumps, block);
}

}  // namespace detail

}  // namespace atx::vol
