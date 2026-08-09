#include "atx/vol/derivatives.hpp"

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
#include "atx/vol/black76.hpp"
#include "atx/vol/detail/counters.hpp" // Task P-6: ledger::Solve::VarSwapStripEvals
#include "atx/vol/detail/deriv_ref_bridge.hpp" // Task 9: SurfaceRef-native entry points
#include "atx/vol/detail/legacy_surface.hpp" // Essvi/SviSurface (demoted, S4-T21)
#include "atx/vol/detail/risk_surface_validation.hpp" // RiskSurfaceValidationConfig (wing-clamp band assert)
#include "atx/vol/detail/rv_lognormal.hpp" // lognormal_call, truncated_expect, norm_cdf (Tasks 4-5)
#include "atx/vol/portfolio_pricer.hpp" // Task 9: SurfaceRef (the borrowed-surface handle)
#include "atx/vol/priced_surface.hpp" // E6: PricedSurface-native entry points
#include "atx/vol/detail/strip_grid.hpp"
#include "atx/vol/surface_parity.hpp" // SliceContext (E6 carry extraction)
#include "atx/vol/surface_policy.hpp" // certified_wing_half_band (FIT-C7 / Task C-6)
#include "atx/vol/vol_surface.hpp" // Tier-A calibration-grade surface container
#include "deriv_analytic_greeks.hpp" // Task P-4 / GK-P: DerivGreekMethod::AnalyticStrip

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

// Resolve the wing trust half-band: 0 -> the surface's own certified band
// when it carries one, else the mode-blind certified validation band; > 0 ->
// the caller's own band; < 0 -> 0.0 (clamp off). The <= 0 encoding of "off"
// lets every consumer test one condition (`band > 0.0`).
[[nodiscard]] double resolve_wing_clamp(const DerivConfig& cfg,
                                        std::optional<double> surface_band) noexcept {
  static_assert(strip::kCertifiedWingHalfBand == RiskSurfaceValidationConfig{}.k_max,
                "the strip's default wing trust band must equal the band the fit "
                "pipeline actually validates (risk_surface_validation.hpp)");
  static_assert(certified_wing_half_band(FitQualityMode::Balanced) == strip::kCertifiedWingHalfBand,
                "surface_policy's mode-keyed certified band must agree with the strip's own "
                "mode-blind default at Balanced quality");
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

template <class SurfaceT>
Result<DerivQuote> var_swap_fair_strike(const SurfaceT& surface,
                                        const CurveSet& curves, double T,
                                        const DerivConfig& cfg) {
  if (!(T > 0.0)) {
    return Err(ErrorCode::InvalidArgument, "var strip needs T > 0");
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
  // Task P-6: one bump per actual strip quadrature attempt, counted past the
  // cheap up-front validation (a caller error above never touches the grid,
  // so it is not "an evaluation") -- see Solve::VarSwapStripEvals's own doc.
  counters::ledger::bump(counters::ledger::Solve::VarSwapStripEvals);
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
    if (cfg.strip_nodes == 0u && kh > floor_half && floor_half > 0.0) {
      const double intervals = static_cast<double>(grid.n_nodes - 1) * (kh / floor_half);
      grid.n_nodes = strip::odd_nodes(
          static_cast<std::size_t>(std::ceil(intervals)) + 1u, grid.n_nodes);
      // Round up to 4m+1: the Richardson half-grid error estimate below needs
      // the half grid ((n+1)/2 nodes) to be odd again, which plain odd-forcing
      // does not guarantee (e.g. n=99 halves to 50, even). The tier defaults
      // are already 4m+1 (97/257/769/2049); only this adaptive rescale can
      // land off that lattice, so only it needs the correction.
      if ((grid.n_nodes % 4u) != 1u) {
        grid.n_nodes += 2u;
      }
    }
  }

  // Wing trust band for the surface READS (see DerivConfig::wing_clamp_k): a
  // node beyond the band prices at its true strike under the BAND-EDGE vol —
  // flat-vol tails over the uncertified extrapolation region, never a
  // truncated span. band <= 0 means the clamp is off. Resolved BEFORE the
  // resolution floor below, which has to know how many panels the C-3 split
  // will cut — and that depends on where this band falls inside the span.
  const double wing_band = resolve_wing_clamp(cfg, cert_wing_band);
  const bool wing_clamped =
      wing_band > 0.0 && (grid.k_min_log < -wing_band || grid.k_max_log > wing_band);

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
  const double resolved_span = grid.k_max_log - grid.k_min_log;
  const std::size_t n_panels =
      strip::strip_panel_count(grid.k_min_log, grid.k_max_log, wing_band);
  bool low_t = false;
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
  // k = 0 (put-call parity) and at ±wing_band when the clamp binds. Split the
  // composite Simpson at every interior kink so each one is a PANEL BOUNDARY
  // for any span, symmetric or not, and the O(h⁴) law (and with it the
  // Richardson estimate below) holds on every panel. See `plan_strip_split`
  // for the budget apportionment and its degradation ladder; the total node
  // count and the reported span are unchanged by the split.
  assert(n >= 3u && "composite Simpson needs at least one panel of 3 nodes");
  const strip::StripSplit split =
      strip::plan_strip_split(grid.k_min_log, grid.k_max_log, n, wing_band);

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
    return std::pair<double, bool>{price / (df * K), bad};
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

  // Untouched pre-P-3 single-pass semantics: one surface read per distinct
  // node, interleaved with the Simpson accumulation via `accumulate_strip`.
  // This is the ONLY path for any SurfaceT without a batched read (VolSurface
  // /EssviSurface/SviSurface, SurfaceRefStripView) -- see `has_strip_iv_batch`
  // below -- and it is also what `Strip.BatchedMatchesScalar*` compares the
  // batched path against via `detail::set_strip_batch_disabled_for_test`.
  const auto run_scalar_node_loop = [&]() noexcept {
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
  // every path that can reach here. The adaptive span rescale above (unlike
  // `strip::dk_floor_nodes`) never caps its own raise, and a caller-pinned
  // `cfg.strip_nodes` is deliberately never clamped either (see that block's
  // own comment) -- so an Audit-tier quote with sigma_atm*sqrt(T) high enough
  // (e.g. ~0.55 at 1Y), or a directly pinned strip_nodes past the cap, can
  // resolve a node count larger than the fixed-size gather buffers below.
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
    if (!strip_batch_disabled_for_test() && n <= strip::kMaxStripNodes) {
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

  const double k_var = (2.0 / T) * integral;

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
    err_est = std::fabs((2.0 / T) * (integral - integral_half)) / 15.0;
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

  DerivFlags flags = DerivFlags::None;
  if (bad_first || cover.left_short) {
    flags |= DerivFlags::StripTruncatedLeft;
  }
  if (bad_last || cover.right_short) {
    flags |= DerivFlags::StripTruncatedRight;
  }
  if (wing_clamped) {
    flags |= DerivFlags::WingClamped;
  }
  if (low_t) {
    flags |= DerivFlags::LowT;
  }
  if (!strip_wholly_unusable && interior_bad_count > 0u) {
    flags |= DerivFlags::InteriorBadNodes;
  }

  DerivQuote out{};
  out.fair_strike_dec = k_var;
  out.fair_strike_points = 1.0e4 * k_var;
  out.pv = 0.0;
  out.undiscounted_expectation_dec = k_var;
  out.uncapped_var_dec = k_var;
  out.accrued_component_dec = 0.0;
  out.future_component_dec = k_var;
  out.convexity_adjustment_dec = 0.0;
  out.integration_error_est = err_est;
  // The grid this quote was actually integrated on, so a caller (deriv_greeks)
  // can pin it back and reproduce this exact quadrature.
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

template <class SurfaceT>
Result<DerivQuote> deriv_price(const SurfaceT& surface, const CurveSet& curves,
                               const DerivContract& contract,
                               const DerivConfig& cfg) {
  // Reserved engines fail clean before any work. RvDistributionProxy is the
  // one exception: Task 4 wires it up (alongside Auto) as the distribution
  // model's entry point for CappedVarSwap, Task 5 adds CappedVolSwap, and
  // Task 6 adds plain VolSwap (mid-life, and an unaged contract priced end to
  // end through the model instead of Carr-Lee) -- every other kind still sees
  // it as reserved.
  switch (cfg.engine) {
  case DerivEngine::RvDistributionProxy:
    if (contract.kind != DerivKind::CappedVarSwap && contract.kind != DerivKind::CappedVolSwap &&
        contract.kind != DerivKind::VolSwap) {
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
  // names. Uncapped kinds (VarSwap/VolSwap) must leave cap_dec at 0.
  const bool is_capped_kind = contract.kind == DerivKind::CappedVarSwap ||
                              contract.kind == DerivKind::CappedVolSwap;
  if (is_capped_kind) {
    if (!(contract.cap_dec > 0.0)) {
      return Err(ErrorCode::InvalidArgument, "capped kind needs cap_dec > 0");
    }
  } else if (contract.cap_dec != 0.0) {
    return Err(ErrorCode::InvalidArgument, "cap_dec is only valid on capped kinds");
  }

  // Kind x engine dispatch matrix (PV-5), enforced in two stages: the
  // reserved-engine switch above (RvDistributionAffine/McQe always
  // NotImplemented; RvDistributionProxy NotImplemented except on the kinds
  // it is wired up for) narrows cfg.engine to what each kind's own case below
  // can still misuse, and each case rejects the one engine value that
  // survives narrowing but still names no pricing formula for that kind.
  // Full matrix: VarSwap -> {Auto, StripLogContract}; VolSwap -> {Auto,
  // VolCarrLee (unaged only -- price_vol_swap itself checks that), Rv
  // DistributionProxy}; CappedVarSwap/CappedVolSwap -> {Auto,
  // RvDistributionProxy}. Everything else is InvalidArgument.
  switch (contract.kind) {
  case DerivKind::VarSwap:
    // Kind x engine matrix (PV-5): VarSwap only ever runs the strip --
    // price_var_swap never reads cfg.engine at all, so an explicit
    // VolCarrLee here used to silently price the strip anyway (VolCarrLee
    // has no variance-swap formula of its own to run instead). RvDistribution
    // Proxy/RvDistributionAffine/McQe on VarSwap are already NotImplemented
    // from the reserved-engine switch above; VolCarrLee is the one gap.
    if (cfg.engine == DerivEngine::VolCarrLee) {
      return Err(ErrorCode::InvalidArgument, "engine cannot price a var swap");
    }
    return price_var_swap(surface, curves, contract, cfg);
  case DerivKind::VolSwap:
    // Kind x engine matrix (PV-5): an explicit StripLogContract here used to
    // silently fall through to price_vol_swap's unaged Carr-Lee branch --
    // the same branch Auto/VolCarrLee take -- because that branch only tests
    // `cfg.engine != RvDistributionProxy`, not which engine it actually is.
    // StripLogContract has no vol-swap formula of its own to run instead.
    if (cfg.engine == DerivEngine::StripLogContract) {
      return Err(ErrorCode::InvalidArgument, "engine cannot price a vol swap");
    }
    return price_vol_swap(surface, curves, contract, cfg);
  case DerivKind::CappedVarSwap:
    if (cfg.engine == DerivEngine::StripLogContract || cfg.engine == DerivEngine::VolCarrLee) {
      return Err(ErrorCode::InvalidArgument, "engine cannot price capped kinds");
    }
    return price_capped_var_swap(surface, curves, contract, cfg);
  case DerivKind::CappedVolSwap:
    if (cfg.engine == DerivEngine::StripLogContract || cfg.engine == DerivEngine::VolCarrLee) {
      return Err(ErrorCode::InvalidArgument, "engine cannot price capped kinds");
    }
    return price_capped_vol_swap(surface, curves, contract, cfg);
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
// contiguous memory -- see the paired A/B in task-P-3-report.md for the
// measured recovery.
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

// Combines the sticky-strike respot (bumped curves move the forward by
// e^{k_shift}, so reading the base surface at k + k_shift keeps the vol tied
// to the SAME absolute strike the bumped strip prices at) and the parallel
// additive vol shift into ONE view, with the shifted read routed through a
// shared `BumpReadCache` slot instead of a live surface call on every hit.
// `vol_shift` is applied AFTER the cache lookup/store -- "the constant vol
// offset applied at use site" (task brief) -- so sigma+-'s bumps reuse the
// exact same cached read the center/spot-bump evaluations already populated
// for their shared (k_shift, T), and a miss still computes and caches the
// RAW (pre-vol-shift) read exactly once, never a vol-shifted one that a
// later, differently-shifted bump could not reuse. NaN propagates through
// `sigma + vol_shift` unchanged (NaN + x is always NaN), so no special case
// is needed for an unusable read.
template <class SurfaceT>
struct CachedBumpView {
  const SurfaceT* base;  // non-owning, non-null
  double k_shift;
  double vol_shift;
  BumpReadCache* cache;  // non-owning, non-null; shared across every bump
                         // evaluation querying this (k_shift, T) pair

  // Task P-2 / GK-P: structural marker `is_bumped_greek_view` (above) detects
  // via `requires`. `bumped_pv` is the only call site that ever constructs a
  // `CachedBumpView`, so this tag exactly identifies a bumped/rolled greek
  // evaluation -- see `is_bumped_greek_view`'s own comment for why this beats
  // a new `DerivConfig` field.
  static constexpr bool is_bumped_greek_view = true;

  [[nodiscard]] double iv(double k_log, double T) const noexcept {
    const double x = k_log + k_shift;
    // Task P-3 test/bench seam: forcing this off reproduces the pre-P-3
    // RespotView+VolShiftView composition exactly (always a live read), so
    // `DerivGreeks.ReadCacheMatchesUncached` can prove the cache changes
    // nothing but how many times the surface is actually read.
    if (bump_read_cache_disabled_for_test()) {
      return base->iv(x, T) + vol_shift;
    }
    const std::uint64_t x_bits = std::bit_cast<std::uint64_t>(x);
    const std::uint64_t t_bits = std::bit_cast<std::uint64_t>(T);
    if (cache->sorted) {
      const auto [found, sigma] = cache->find(x_bits, t_bits);
      return (found ? sigma : base->iv(x, T)) + vol_shift;
    }
    if (!cache->recording) {
      // Review fix round 1, I-3: a slot nothing ever replays against -- read
      // live, do not bother appending to a vector no one will search.
      return base->iv(x, T) + vol_shift;
    }
    // Recording phase: append-only, no lookup -- see `BumpReadCache`'s own
    // comment for why. `push_back` lands in the capacity `begin_recording`
    // already reserved (Review fix round 1, I-5), so this cannot reallocate.
    const double sigma = base->iv(x, T);
    cache->entries.push_back(BumpReadCache::Entry{x_bits, t_bits, sigma});
    return sigma + vol_shift;
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
  // the first paired A/B (task-P-3-report.md, Fix round 0) measured no
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
      for (std::size_t i = 0; i < n; ++i) {
        out[i] = x[i] + k_shift;
      }
      base->iv_batch(out.first(n), T, out.first(n));
      for (std::size_t i = 0; i < n; ++i) {
        out[i] += vol_shift;
      }
      return;
    }
    const std::uint64_t t_bits = std::bit_cast<std::uint64_t>(T);
    if (cache->sorted) {
      // Replay: the pinned grid means every element is expected to hit, but
      // each is independently searched/verified rather than assumed --
      // never wrong even if that invariant were ever violated, just not
      // batched on a miss (see `BumpReadCache::find`'s own comment).
      for (std::size_t i = 0; i < n; ++i) {
        const double shifted = x[i] + k_shift;
        const auto [found, sigma] =
            cache->find(std::bit_cast<std::uint64_t>(shifted), t_bits);
        out[i] = (found ? sigma : base->iv(shifted, T)) + vol_shift;
      }
      return;
    }
    if (!cache->recording) {
      // I-3: single-use slot -- one batched live read, nothing cached.
      for (std::size_t i = 0; i < n; ++i) {
        out[i] = x[i] + k_shift;
      }
      base->iv_batch(out.first(n), T, out.first(n));
      for (std::size_t i = 0; i < n; ++i) {
        out[i] += vol_shift;
      }
      return;
    }
    // Recording: ONE batched raw read via `base->iv_batch`, then cache each
    // (key recomputed from the ORIGINAL `x`/`k_shift`, bit-identical to what
    // was written into `out` before the call below) and apply vol_shift.
    for (std::size_t i = 0; i < n; ++i) {
      out[i] = x[i] + k_shift;
    }
    base->iv_batch(out.first(n), T, out.first(n));  // out[i] is now raw sigma
    for (std::size_t i = 0; i < n; ++i) {
      const double shifted = x[i] + k_shift;
      cache->entries.push_back(
          BumpReadCache::Entry{std::bit_cast<std::uint64_t>(shifted), t_bits, out[i]});
      out[i] += vol_shift;
    }
  }
};

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
template <class SurfaceT>
[[nodiscard]] Result<double> bumped_pv(const SurfaceT& surface, const CurveSet& curves,
                                       const DerivContract& contract, const DerivConfig& cfg,
                                       double k_shift, double vol_shift, BumpReadCache& cache) {
  const CachedBumpView<SurfaceT> view{&surface, k_shift, vol_shift, &cache};
  ATX_TRY(const DerivQuote q, deriv_price(view, curves, contract, cfg));
  return Ok(q.pv);
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
[[nodiscard]] RealizedVarianceSpec inject_carry_fixing(const RealizedVarianceSpec& rv,
                                                        double fixing_dec) noexcept {
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
};

// Up to 7 evaluations, 13 with second_order (one fewer / three fewer when the
// contract cannot roll), plus 3 more when `bumps.carry_theta` is on (one
// var_swap_fair_strike call to resolve K_var_future, then the two carry-theta
// reprices above) -- skipped entirely (no extra evaluation) when
// `contract.rv_spec.n_obs_total == 0`, where there is no fixing schedule to
// inject into. Every failure propagates: a bumped contract that will not
// price is a real failure, not a missing greek.
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
  const double ks_up = std::log1p(h);
  const double ks_dn = std::log1p(-h);
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
  if (skip_market_bumps) {
    // I-3's existing "single-use slot" mode (no `begin_recording`): nothing
    // will ever replay `cache_c_t0` once v_up/v_dn are skipped too, so
    // `CachedBumpView` just reads live and the reserve()+sort() a recording
    // slot pays for is not spent on zero readers.
    ATX_TRY(pv.c, bumped_pv(surface, curves, contract, cfg, 0.0, 0.0, cache_c_t0));
  } else {
    cache_c_t0.begin_recording(reserve_hint);
    ATX_TRY(pv.c, bumped_pv(surface, curves, contract, cfg, 0.0, 0.0, cache_c_t0));
    cache_c_t0.finish_recording();  // replayed by pv.v_up, pv.v_dn below
    cache_up_t0.begin_recording(reserve_hint);
    ATX_TRY(pv.s_up, bumped_pv(surface, cs_up, contract, cfg, ks_up, 0.0, cache_up_t0));
    cache_up_t0.finish_recording();  // replayed by pv.sv_pp, pv.sv_pm below
    cache_dn_t0.begin_recording(reserve_hint);
    ATX_TRY(pv.s_dn, bumped_pv(surface, cs_dn, contract, cfg, ks_dn, 0.0, cache_dn_t0));
    cache_dn_t0.finish_recording();  // replayed by pv.sv_mp, pv.sv_mm below
    ATX_TRY(pv.v_up, bumped_pv(surface, curves, contract, cfg, 0.0, dv, cache_c_t0));
    ATX_TRY(pv.v_dn, bumped_pv(surface, curves, contract, cfg, 0.0, -dv, cache_c_t0));
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
        if (const Result<DerivQuote> strip_q =
                var_swap_fair_strike(surface, curves, contract.maturity_t, cfg);
            strip_q.has_value()) {
          const double k_var_future = strip_q->fair_strike_dec;

          DerivContract rolled_carry = rolled;
          rolled_carry.rv_spec = inject_carry_fixing(rv, k_var_future);
          const Result<double> carry_pv =
              bumped_pv(surface, curves, rolled_carry, cfg, 0.0, 0.0, cache_c_tr);

          DerivContract rolled_zero = rolled;
          rolled_zero.rv_spec = inject_carry_fixing(rv, 0.0);
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
  if (!(b.spot_rel > 0.0 && b.spot_rel < 1.0 && b.vol_abs > 0.0 && b.rate_abs > 0.0 &&
        b.time_years > 0.0)) {
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
  //   - `vol_of_vol`/`carr_lee_form`: both drive only the RV distribution
  //     model (mid-life VolSwap, both capped kinds) and the standalone
  //     Carr-Lee K_vol entry -- `price_var_swap` never reads either field.
  //     Not scope-relevant for VarSwap.
  //   - `abs_price_tol`/`rel_price_tol`/`flags_request`: reserved, must be
  //     zero (`reserved_fields_clean`, checked inside `var_swap_fair_strike`
  //     before any of this runs). Not scope-relevant.
  const bool analytic_in_scope = bumps.method == DerivGreekMethod::AnalyticStrip &&
                                 contract.kind == DerivKind::VarSwap &&
                                 cfg.discrete_correction_mode == DerivDiscreteCorrection::None;

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

// Mirrors `deriv_price<SurfaceT>`'s OWN dispatch-level validation for the
// VarSwap case (the reserved-engine switch, `reserved_fields_clean`,
// `vol_of_vol_valid`, the uncapped-kind `cap_dec == 0` rule, and VarSwap's own
// "VolCarrLee cannot price a var swap" rejection) -- reproduced here because
// the shared-block path calls `var_swap_fair_strike` directly for a
// NOT-fully-aged row (bypassing `deriv_price`'s dispatch) and never reaches
// it AT ALL for a fully-aged one, so a fully-aged row with a malformed `cfg`
// must still fail exactly as `deriv_price` would have. `wing_clamp_valid` /
// `surface_certified_wing_band_valid` are deliberately NOT duplicated here --
// `deriv_price`'s dispatch never checks them either; only
// `var_swap_fair_strike` itself does, which is why a fully-aged row is
// unaffected by a bad wing-clamp band under the UNMEMOIZED path too.
[[nodiscard]] Status validate_var_swap_dispatch(const DerivContract& contract,
                                                 const DerivConfig& cfg) {
  switch (cfg.engine) {
  case DerivEngine::RvDistributionProxy:
  case DerivEngine::RvDistributionAffine:
  case DerivEngine::McQe:
    return Err(ErrorCode::NotImplemented, "reserved pricing engine");
  case DerivEngine::Auto:
  case DerivEngine::StripLogContract:
  case DerivEngine::VolCarrLee:
    break;
  }
  if (!reserved_fields_clean(cfg)) {
    return Err(ErrorCode::NotImplemented, "reserved config field is non-zero");
  }
  if (!vol_of_vol_valid(cfg)) {
    return Err(ErrorCode::InvalidArgument, "vol_of_vol must be >= 0");
  }
  if (contract.cap_dec != 0.0) {
    return Err(ErrorCode::InvalidArgument, "cap_dec is only valid on capped kinds");
  }
  if (cfg.engine == DerivEngine::VolCarrLee) {
    return Err(ErrorCode::InvalidArgument, "engine cannot price a var swap");
  }
  return Ok();
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
    const bool analytic_in_scope = bumps.method == DerivGreekMethod::AnalyticStrip;
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
      const double ks_up = std::log1p(h);
      const double ks_dn = std::log1p(-h);
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
        const CachedBumpView<SurfaceT> view{&surface, ks_up, 0.0, &no_cache_up};
        block.s_up_raw = resolve_var_swap_strip_raw(view, cs_up, T, block.cfg_pinned);
      }
      {
        const CachedBumpView<SurfaceT> view{&surface, ks_dn, 0.0, &no_cache_dn};
        block.s_dn_raw = resolve_var_swap_strip_raw(view, cs_dn, T, block.cfg_pinned);
      }
      {
        const CachedBumpView<SurfaceT> view{&surface, 0.0, dv, &no_cache_c};
        block.v_up_raw = resolve_var_swap_strip_raw(view, curves, T, block.cfg_pinned);
      }
      {
        const CachedBumpView<SurfaceT> view{&surface, 0.0, -dv, &no_cache_c};
        block.v_dn_raw = resolve_var_swap_strip_raw(view, curves, T, block.cfg_pinned);
      }
      if (block.have_second_order) {
        BumpReadCache no_cache_pp;
        BumpReadCache no_cache_pm;
        BumpReadCache no_cache_mp;
        BumpReadCache no_cache_mm;
        {
          const CachedBumpView<SurfaceT> view{&surface, ks_up, dv, &no_cache_pp};
          block.sv_pp_raw = resolve_var_swap_strip_raw(view, cs_up, T, block.cfg_pinned);
        }
        {
          const CachedBumpView<SurfaceT> view{&surface, ks_up, -dv, &no_cache_pm};
          block.sv_pm_raw = resolve_var_swap_strip_raw(view, cs_up, T, block.cfg_pinned);
        }
        {
          const CachedBumpView<SurfaceT> view{&surface, ks_dn, dv, &no_cache_mp};
          block.sv_mp_raw = resolve_var_swap_strip_raw(view, cs_dn, T, block.cfg_pinned);
        }
        {
          const CachedBumpView<SurfaceT> view{&surface, ks_dn, -dv, &no_cache_mm};
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
      const double ks_up = std::log1p(h);
      const double ks_dn = std::log1p(-h);
      const CurveSet cs_up = respot_curves(curves, 1.0 + h);
      const CurveSet cs_dn = respot_curves(curves, 1.0 - h);
      BumpReadCache no_cache_up;
      BumpReadCache no_cache_dn;
      {
        const CachedBumpView<SurfaceT> view{&surface, ks_up, 0.0, &no_cache_up};
        block.s_up_tdt_raw = resolve_var_swap_strip_raw(view, cs_up, block.t_minus_dt, block.cfg_pinned);
      }
      {
        const CachedBumpView<SurfaceT> view{&surface, ks_dn, 0.0, &no_cache_dn};
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
        DerivContract rolled_carry = rolled;
        rolled_carry.rv_spec = inject_carry_fixing(rv, k_var_future_at_T);
        DerivContract rolled_zero = rolled;
        rolled_zero.rv_spec = inject_carry_fixing(rv, 0.0);
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

Status RealizedTracker::observe(double spot) {
  if (!(spot > 0.0)) {
    return Err(ErrorCode::InvalidArgument, "spot must be > 0");
  }
  if (!(rv_.annualization > 0.0)) {
    return Err(ErrorCode::InvalidArgument, "tracker not initialized");
  }

  if (!have_prev_) {
    // First observation seeds the previous spot. No return yet.
    prev_spot_ = spot;
    have_prev_ = true;
    return Ok();
  }

  // Refuse once all n_obs_total returns have been observed.
  if (rv_.n_obs_done >= rv_.n_obs_total) {
    return Err(ErrorCode::InvalidArgument, "all observations already recorded");
  }

  const double r = std::log(spot / prev_spot_);
  rv_.sum_sq_log_returns_done += r * r;
  rv_.n_obs_done += 1u;
  prev_spot_ = spot;

  const double n = static_cast<double>(rv_.n_obs_done);
  rv_.rv_done_dec = rv_.annualization * rv_.sum_sq_log_returns_done / n;
  return Ok();
}

Status RealizedTracker::observe_batch(std::span<const double> spots) {
  for (const double spot : spots) {
    ATX_TRY_VOID(observe(spot));
  }
  return Ok();
}

Status RealizedTracker::observe_dated(std::int64_t ts_ns, double spot) {
  // Ordering validated FIRST and unconditionally: a stale/replayed ts_ns
  // mutates nothing, even when observe(spot) would itself have rejected the
  // spot (e.g. non-positive) -- the caller learns "not ascending", not a
  // spot-validation error that implies the timestamp was otherwise fine.
  if (ts_ns <= last_fixing_ts_ns_) {
    return Err(ErrorCode::AlreadyExists, "fixing timestamp not ascending");
  }
  ATX_TRY_VOID(observe(spot));
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
[[nodiscard]] Result<CurveSet> carry_from(const PricedSurface& ps, double T,
                                          double roll_dt = 0.0) {
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
  if (T < ts.front() || T > ts.back()) {
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
                                                     const DerivConfig& cfg) noexcept {
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
  ATX_TRY(const CurveSet curves, carry_from_ref(ref, contract.maturity_t, bumps.time_years));
  const SurfaceRefStripView view{&ref, &curves, contract.maturity_t, surface_certified_wing_band};
  return deriv_greeks_var_swap_shared(view, curves, contract, cfg, bumps, block);
}

}  // namespace detail

}  // namespace atx::vol
