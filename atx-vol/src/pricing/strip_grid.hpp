#pragma once

// ── The ONE log-forward-moneyness strip/grid convention (E2 / AN-P1-2) ──────
//
// atx-vol integrates model-free quantities — the variance strip / MFIV, the
// Breeden–Litzenberger density, the BKM moments — on a grid that is UNIFORM IN
// LOG-FORWARD-MONEYNESS k = ln(K/F), quadratured with composite Simpson.
//
// Before E2 there were TWO independent implementations of that grid:
// `derivatives.cpp` (fixed ±1.5 span by quality tier, linear-in-F forward
// interpolation) and `analytics_density.cpp` (adaptive width_sigmas·σ_atm·√T
// span). They disagreed on span policy AND on forward interpolation, so the
// same tenor on the same surface could produce two different K_var. This header
// is the single source of both conventions; both TUs call it, and it is also the
// seam E6 re-types `derivatives` onto.
//
// SPAN POLICY. Half-width in k is
//
//     kh = max(floor_half_width, width_sigmas · σ_atm · √T)
//
// The vol-scaled term is what keeps the wings on a high-vol or long-dated
// tenor: a σ = 60%, T = 1y name needs ±3.6 to reach 6σ√T, and integrating it on
// a fixed ±1.5 truncates the strip and biases K_var LOW. `floor_half_width` is a
// floor, never a cap. `width_sigmas = 0` pins the fixed span (the escape hatch
// a caller uses when it wants an exactly-specified strip).
//
// TRUNCATION IS A COVERAGE PROPERTY, NOT A NaN PROPERTY. The pre-E2 code raised
// `StripTruncated*` only when the surface returned a non-finite IV at an
// integration boundary. A parametric eSSVI/SVI surface returns a finite IV at
// EVERY k, so a truncated parametric strip reported full coverage — silently
// wrong, which is the AN-P1-2 defect. `wing_coverage` below decides truncation
// by comparing the actual span against the vol-scaled requirement.
//
// FORWARD INTERPOLATION IS LOG-LINEAR IN F. `forward_log_blend` matches
// `projection.cpp`'s `curve_forward_T` exactly (linear in log F, clamped
// outside the pillar range), so a forward read for a var strip and a forward
// read for a projection agree by construction.

#include <algorithm> // std::max (adaptive_half_width) — was only transitive
#include <array>     // std::array (plan_strip_split's fixed panel storage)
#include <cmath>
#include <cstddef>

namespace atx::vol::strip {

// Default adaptive width in σ√T units. Matches `RndConfig::width_sigmas`, which
// is where this policy was already correct — E2 propagates it, it does not
// invent it. 6σ covers ~1 - 2e-9 of a lognormal's mass per side.
inline constexpr double kDefaultWidthSigmas = 6.0;

// Default wing trust half-band for the variance strip's surface READS
// (`DerivConfig::wing_clamp_k == 0`), in absolute log-forward-moneyness. This
// is the MODE-BLIND default — it equals the BALANCED-quality certified band
// (`RiskSurfaceValidationConfig{}.k_max`, the default risk-validation config)
// and is what every path without surface provenance (the templated legacy
// VolSurface/eSSVI/SVI containers, or a PricedSurface/SurfaceRef a caller
// prices without stating its build quality mode) resolves to. MUST stay equal
// to that default risk-validation band — the clamp's whole claim is "the
// strip trusts the surface exactly where the pipeline certified it", and the
// claim dissolves if the two constants drift apart. static_asserted against
// the validation config at the use site in derivatives.cpp.
//
// FIT-C7: a surface fit at a NON-Balanced quality mode certifies a DIFFERENT
// band (Latency ±0.35, Accuracy ±0.60 — `risk_validation_config`,
// pricer_fitter.cpp) — reading this mode-blind constant for such a surface
// trusts a band nobody certified. A caller who knows the surface's own build
// quality mode should resolve `atx::vol::certified_wing_half_band(mode)`
// (surface_policy.hpp) instead and pass it as `var_swap_fair_strike`'s (etc.)
// `surface_certified_wing_band` argument; `resolve_wing_clamp` (derivatives.cpp)
// only falls back to this constant when the caller supplies no such band.
inline constexpr double kCertifiedWingHalfBand = 0.5;

// Half-width in log-forward-moneyness: the tier/config floor, widened to the
// tenor's own vol scale. Returns `floor_half_width` unchanged when σ_atm is
// unusable (non-finite / non-positive), when T is unusable, or when
// `width_sigmas <= 0` (span pinned by the caller).
[[nodiscard]] inline double adaptive_half_width(double floor_half_width, double sigma_atm, double T,
                                                double width_sigmas) noexcept {
  double kh = floor_half_width;
  if (std::isfinite(sigma_atm) && sigma_atm > 0.0 && std::isfinite(T) && T > 0.0 &&
      width_sigmas > 0.0) {
    kh = std::max(kh, width_sigmas * sigma_atm * std::sqrt(T));
  }
  return kh;
}

// How far out in k the wings must reach for the strip to be considered complete
// at this tenor. Zero (== "no requirement expressible") when σ_atm or T is
// unusable, which callers treat as "cannot judge coverage".
[[nodiscard]] inline double required_half_width(double sigma_atm, double T,
                                                double width_sigmas) noexcept {
  if (!std::isfinite(sigma_atm) || sigma_atm <= 0.0 || !std::isfinite(T) || T <= 0.0 ||
      !(width_sigmas > 0.0)) {
    return 0.0;
  }
  return width_sigmas * sigma_atm * std::sqrt(T);
}

// Per-side truncation verdict for an actual integration span [k_lo, k_hi].
struct WingCoverage {
  bool left_short{false};  // k_lo > -required  => left wing cut
  bool right_short{false}; // k_hi <  required  => right wing cut
};

// Decide truncation from SPAN COVERAGE. `required` comes from
// `required_half_width`; a non-positive `required` means coverage cannot be
// judged and neither side is reported short (the caller's own NaN-boundary
// check still applies on top).
[[nodiscard]] inline WingCoverage wing_coverage(double k_lo, double k_hi,
                                                double required) noexcept {
  WingCoverage out;
  if (!(required > 0.0)) {
    return out;
  }
  out.left_short = k_lo > -required;
  out.right_short = k_hi < required;
  return out;
}

// Force an odd node count (composite Simpson needs an even interval count),
// bumping a too-small request up to `minimum` rather than erroring.
[[nodiscard]] inline std::size_t odd_nodes(std::size_t requested, std::size_t minimum) noexcept {
  std::size_t n = requested < minimum ? minimum : requested;
  if ((n % 2u) == 0u) {
    ++n;
  }
  return n;
}

// ── C-2 / PV-2: resolution floor (the SPAN policy's mirror) ────────────────
//
// `adaptive_half_width` only WIDENS the span for a high-vol/long-dated tenor;
// nothing rescales the node count for the OPPOSITE regime, a short-tenor/
// low-vol quote that sits comfortably inside the tier's own span floor. The
// tier grids are sized for a roughly-1Y reference vol scale, so a T = 1
// trading day quote can resolve far coarser than its own sigma_atm*sqrt(T)
// calls for -- e.g. Fast (97 nodes over +-1.0) resolves dk ~= 0.0208 at
// T = 1/252, sigma = 20%, ~6.6x coarser than the dk_ceiling below, and the
// quadrature error that starves is dominated by the near-ATM curvature the
// strip integrates through (the price/(df*K) integrand's kink at k = 0), not
// by truncated wings -- verified +6.06% on K_var at the Fast tier (PV-2).

// Resolution ceiling in log-forward-moneyness the strip's own node spacing
// must not exceed. Returns 0.0 ("no requirement expressible", the same
// convention `required_half_width` uses) when sigma_atm or T is unusable.
[[nodiscard]] inline double dk_ceiling(double sigma_atm, double T) noexcept {
  if (!std::isfinite(sigma_atm) || sigma_atm <= 0.0 || !std::isfinite(T) || T <= 0.0) {
    return 0.0;
  }
  return sigma_atm * std::sqrt(T) / 4.0;
}

// Minimum node count that keeps EVERY PANEL of the kink-aligned split (C-3,
// `plan_strip_split`) at or under `dk_max` (from `dk_ceiling`). `n_panels` is
// that split's panel count (`strip_panel_count`); pass 1 for a genuinely
// uniform grid.
//
// WHY THE PANEL COUNT ENTERS. The ceiling constrains the spacing the strip
// actually integrates on, and after C-3 that is no longer one uniform lattice:
// integer apportionment cannot divide a span evenly, so a panel's own spacing
// runs ABOVE the nominal span/(n-1). Sizing against the nominal value would
// make this floor approximate -- a tenor whose nominal dk sits just under
// dk_max would clear the check while a panel breached the ceiling.
//
// The excess is bounded exactly. Apportionment hands out units of `unit`
// intervals and gives panel i a share strictly greater than
// spare*len_i/span, with spare = intervals/unit - n_panels, so
//
//     dk_i = len_i / (unit * share_i) < span / (intervals - unit * n_panels)
//
// Provisioning `intervals >= span/dk_max + unit*n_panels` therefore makes
// dk_i < dk_max hold for EVERY panel. `unit` is 4 here: the 4m+1 rounding
// below puts the budget on the lattice where `plan_strip_split` picks 4, and
// every path it can instead take (unit 2, a degraded panel count, no split at
// all) only shrinks `unit * n_panels`, so 4*n_panels is an upper bound on all
// of them. The term costs at most 16 intervals (kMaxStripPanels == 4).
//
// Preserves the 4m+1 Richardson invariant the same way the span-driven rescale
// (FIX-E M-7, derivatives.cpp) does: force odd, then nudge +2 if that lands
// off 4m+1 -- odd counts alternate 1 mod 4 / 3 mod 4 as they step by two, so a
// single +2 always suffices. Returns `current_n` unchanged when `span`/
// `dk_max` is non-positive (no floor is expressible) or the current budget
// already satisfies the requirement -- the caller compares the result against
// `current_n` to learn whether the floor actually engaged.
//
// Aggregate review fix (C-R Important I-2 / MUST-FIX 3): the raw demand
// `span/dk_max + 4*n_panels` grows without bound as sigma_atm*sqrt(T) -> 0
// (a short-tenor or near-zero-vol quote) -- the tier SPAN floor does not
// shrink with T, so `dk_max` alone drives the demand toward infinity with no
// error, no flag, just an unbounded slowdown (a `deriv_greeks` call pays up
// to 17 of these strips after C-10). It also let `std::ceil(intervals)` feed
// a `std::size_t` cast that is UB once `intervals` exceeds `size_t`'s range.
// `kMaxStripNodes` -- the Audit tier's own node count, the richest tier this
// file ships -- caps both, checked BEFORE the cast so the overflow-prone
// value never reaches it. A caller past the cap pays no more than Audit's
// own cost; the existing `raised != current_n` check at the call site still
// reports LowT ("under-resolved for its own vol scale"), which already
// covers a capped grid honestly -- no new flag needed.
inline constexpr std::size_t kMaxStripNodes = 2049;

[[nodiscard]] inline std::size_t dk_floor_nodes(double span, std::size_t current_n,
                                                double dk_max,
                                                std::size_t n_panels) noexcept {
  if (!(span > 0.0) || !(dk_max > 0.0) || current_n < 2u) {
    return current_n;
  }
  const double intervals = span / dk_max + 4.0 * static_cast<double>(n_panels);
  if (static_cast<double>(current_n - 1u) >= intervals) {
    return current_n;
  }
  // Capped here, before the ceil()->size_t cast below: `intervals` can be
  // astronomically large, and casting an out-of-range double to `size_t` is
  // undefined behaviour, not a saturating truncation.
  if (!(intervals < static_cast<double>(kMaxStripNodes))) {
    return current_n > kMaxStripNodes ? current_n : kMaxStripNodes;
  }
  std::size_t n = odd_nodes(static_cast<std::size_t>(std::ceil(intervals)) + 1u, current_n);
  if ((n % 4u) != 1u) {
    n += 2u;
  }
  return n > kMaxStripNodes ? kMaxStripNodes : n;
}

// ── FIX-E M-7: the SPAN-driven node rescale (the mirror of dk_floor_nodes) ──
//
// Widening the span at a fixed node count coarsens Δk, which is the resolution
// a tier actually promises. Holding it means scaling the INTERVAL count by the
// same factor the half-width grew by, `kh / floor_half`.
//
// This body used to live inline in `strip_fair_value_core` (derivatives.cpp),
// and it was the one copy of this shape that never got a pre-cast bound. `kh`
// carries `width_sigmas * sigma_atm * sqrt(T)` and nothing upstream bounds
// `sigma_atm`, so the ratio is unbounded above and `intervals` runs away
// exactly as `dk_floor_nodes`'s own demand does: at σ√T ≈ 100 a Standard-tier
// quote asked for ~102 401 nodes, and past `size_t`'s range the
// `ceil()`→`size_t` cast is UNDEFINED BEHAVIOUR, not a saturating truncation.
// Extracted here so the bound is written once and so the out-of-range end is
// testable without paying for a quadrature.
//
// THE BOUND IS `kMaxRescaleNodes`, NOT `kMaxStripNodes` — review fix, and the
// distinction is load-bearing. `kMaxStripNodes` is the batched gather's fixed
// STACK BUFFER length, and the only correct consequence of exceeding it is the
// fallback to the scalar loop that gather already performs structurally
// (derivatives.cpp, "Review fix round 1, CRITICAL-1"). Bounding the RESCALE
// there instead made this function INCAPABLE of raising at the Audit tier at
// all — 2048 intervals over ±3.0 need `intervals < 2049` to escape the cap
// while any widening at all forces `intervals > 2048` — and clipped High from
// σ√T ≥ 1 and Standard from σ√T > 2, i.e. it re-introduced the widened-span/
// coarsened-Δk trade M-7 exists to prevent, in the regime M-7 was written for.
// `kMaxRescaleNodes` is a COST bound instead, set where no real quote reaches
// it: the richest tier only hits it at σ√T = 8, an 800-vol year.
//
// A capped rescale is NOT self-reporting. The call site's
// `max_panel_spacing(split) > dk_max` check cannot detect it — after the bound
// the spacing is `2*kh/(kMaxRescaleNodes-1)` ∝ σ√T and `dk_ceiling` is
// `σ√T/4`, so σ√T cancels and the ratio is a constant ~1.5e-3, under the
// ceiling for EVERY input. `capped` is therefore returned explicitly and the
// caller raises LowT from it; do not delete that plumbing on the assumption
// the spacing check covers it.
//
// Returns `current_n` unchanged when the span did not widen, or when either
// half-width is unusable.
inline constexpr std::size_t kMaxRescaleNodes = 32769;  // 4*8192+1, on the 4m+1 lattice

struct SpanRescale {
  std::size_t n_nodes;  // resolved node count; never below `current_n`
  bool capped;          // the cost bound engaged: Δk is coarser than the tier promises
};

[[nodiscard]] inline SpanRescale span_rescaled_nodes(std::size_t current_n, double kh,
                                                     double floor_half) noexcept {
  if (current_n < 2u || !(floor_half > 0.0) || !(kh > floor_half)) {
    return SpanRescale{current_n, false};
  }
  const double intervals = static_cast<double>(current_n - 1u) * (kh / floor_half);
  // Bounded HERE, before the ceil()->size_t cast below, for the same reason
  // `dk_floor_nodes` bounds its own: an out-of-range double->size_t cast is UB.
  // `!(intervals < ceiling)` catches +inf and NaN on the same test, so no
  // out-of-range value can reach the cast on any input.
  const auto ceiling = static_cast<double>(kMaxRescaleNodes - 1u);
  const bool capped = !(intervals < ceiling);
  std::size_t n =
      odd_nodes(static_cast<std::size_t>(std::ceil(capped ? ceiling : intervals)) + 1u, current_n);
  // Round up to 4m+1: the Richardson half-grid error estimate at the call site
  // needs the half grid ((n+1)/2 nodes) to be odd again, which plain
  // odd-forcing does not guarantee (e.g. n=99 halves to 50, even). The tier
  // defaults are already 4m+1 (97/257/769/2049); only this adaptive rescale
  // can land off that lattice, so only it needs the correction.
  if ((n % 4u) != 1u) {
    n += 2u;
  }
  return SpanRescale{n > kMaxRescaleNodes ? kMaxRescaleNodes : n, capped};
}

// Composite-Simpson weight for node i of n (n odd): end nodes 1, interior
// alternating 4 / 2. The caller supplies the trailing Δk/3.
[[nodiscard]] inline double simpson_weight(std::size_t i, std::size_t n) noexcept {
  if (i == 0 || i + 1 == n) {
    return 1.0;
  }
  return (i % 2u != 0u) ? 4.0 : 2.0;
}

// ── C-3 / LIT-10: kink-aligned Simpson panels ──────────────────────────────
//
// The OTM integrand the variance strip quadratures is only PIECEWISE smooth.
// It carries up to three interior C1 kinks:
//
//   k = 0           put-call parity. OTM switches from the put branch to the
//                   call branch; the two agree in VALUE at K = F but their
//                   K-derivatives differ by exactly the discount factor, so
//                   the normalized integrand OTM/(df*K) has a slope jump of
//                   exactly 1 there -- the largest kink in the strip, ~25x the
//                   integrand's own ATM value at a 3M 20-vol.
//   k = +-wing_band the wing clamp freezes the surface read at the band edge,
//                   so d(iv)/dk drops to zero across it. Present only when the
//                   clamp actually binds, i.e. the span reaches past the band.
//
// Composite Simpson is O(h^4) on a smooth panel but only O(h^2) on a panel that
// STRADDLES such a kink -- the straddle term is J*h^2/6 at worst (J the slope
// jump, worst case being a kink at the panel MIDPOINT), and it does not shrink
// with the panel count the way the h^4 term does. The Richardson
// |I_h - I_2h|/15 estimate, which assumes the h^4 law, then reports a number
// unrelated to the true error in EITHER direction.
//
// Measured pre-C-3 on the 3M skew fixture (derivatives_test.cpp, StripQuadrature):
//   - asymmetric 101-node pin: K_var off by 2.61e-4 on a 0.04 truth (6.5e-3
//     rel), matching J*h^2/6*(2/T) with J = 1, h = 0.014 to three digits; its
//     error estimate understated by 7.5x.
//   - symmetric 101-node pin: k = 0 IS a full-grid boundary, but lands on an
//     ODD index of the HALF grid, so the /15 difference measures the half
//     grid's own straddle instead of the error -- 574x too LARGE on the skew
//     fixture, 4.1e4x on the flat one.
// Before C-3 the k = 0 kink landed on a panel boundary only because every
// DEFAULT grid happens to be symmetric with 4m+1 nodes -- an accident of the
// defaults, which any caller-pinned asymmetric span silently broke.
//
// `plan_strip_split` retires the accident: it cuts [k_lo, k_hi] at every
// interior kink and apportions the resolved node budget across the resulting
// sub-intervals in proportion to length, so the kinks are panel boundaries BY
// CONSTRUCTION on any grid, symmetric or not. Total node count is preserved
// exactly (the boundary node two panels share is counted once), so
// `strip_nodes_used` and the span it reports keep their meaning.

// [k_lo, -band], [-band, 0], [0, band], [band, k_hi] -- the widest split.
inline constexpr std::size_t kMaxStripPanels = 4;

// One sub-interval of the split. `n_nodes` is odd and counts BOTH ends, so the
// node the next panel starts from is this panel's last node.
struct StripPanel {
  double k_lo{0.0};
  double k_hi{0.0};
  std::size_t n_nodes{0};
};

struct StripSplit {
  std::array<StripPanel, kMaxStripPanels> panels{};
  std::size_t count{0};
  // Every panel is 4m+1, so every panel's half grid ((n+1)/2 nodes, spacing
  // 2*dk) is itself a valid composite-Simpson grid WITH THE SAME BOUNDARIES --
  // which is what makes the per-panel /15 estimate meaningful.
  bool richardson_ok{false};
};

namespace detail {

// Ascending panel boundaries for [k_lo, k_hi]: the two endpoints plus the
// interior kinks. `with_clamp` selects whether the +-wing_band pair joins them.
// Returns the boundary count, i.e. panel count + 1. Strict comparisons against
// the previous boundary and the right endpoint keep the list ascending, keep
// every panel non-degenerate, and dedup a band edge that coincides with an
// endpoint or with k = 0. A non-finite `wing_band` fails both comparisons and
// so degrades to the k = 0 split, which is the safe direction.
[[nodiscard]] inline std::size_t strip_panel_bounds(
    double k_lo, double k_hi, double wing_band, bool with_clamp,
    std::array<double, kMaxStripPanels + 1>& bound) noexcept {
  const std::array<double, 3> kink = {-wing_band, 0.0, wing_band};
  std::size_t nb = 0;
  bound[nb++] = k_lo;
  for (std::size_t i = 0; i < kink.size(); ++i) {
    const bool clamp_edge = (i != 1u);
    if (clamp_edge && (!with_clamp || !(wing_band > 0.0))) {
      continue;
    }
    if (kink[i] > bound[nb - 1] && kink[i] < k_hi) {
      bound[nb++] = kink[i];
    }
  }
  bound[nb++] = k_hi;
  return nb;
}

// Hand out `spare` surplus units across `count` panels in proportion to
// `len`, on top of the one unit every panel is owed. Largest-remainder
// apportionment: floor each exact share, then give the leftovers to the
// largest dropped fractions. Writes `count` entries of `share`; returns
// nothing because the total is exactly `count + spare` by construction (the
// leftovers are handed out, never dropped), which is what makes the split
// node-count-preserving.
//
// Ties go to the longer panel, then to the lower index, so the plan is a
// deterministic function of its inputs -- a pinned grid (deriv_greeks' bump
// stencils, an archived quote) must replay it bit-identically.
inline void apportion_units(const std::array<double, kMaxStripPanels>& len,
                            std::size_t count, std::size_t spare,
                            std::array<std::size_t, kMaxStripPanels>& share) noexcept {
  double span = 0.0;
  for (std::size_t i = 0; i < count; ++i) {
    span += len[i];
  }
  std::array<double, kMaxStripPanels> frac{};
  std::size_t handed = 0;
  for (std::size_t i = 0; i < count; ++i) {
    const double exact = static_cast<double>(spare) * len[i] / span;
    const double whole = std::floor(exact);
    share[i] = 1u + static_cast<std::size_t>(whole);
    frac[i] = exact - whole;
    handed += static_cast<std::size_t>(whole);
  }
  // Each dropped fraction is < 1 and there are `count` of them, so `spare -
  // handed` is at most count - 1: the loop is bounded by the panel count and
  // no panel can be picked twice (a winner's remainder is retired to -1).
  for (std::size_t left = spare - handed; left > 0u; --left) {
    std::size_t best = 0;
    for (std::size_t i = 1; i < count; ++i) {
      if (frac[i] > frac[best] || (frac[i] == frac[best] && len[i] > len[best])) {
        best = i;
      }
    }
    share[best] += 1u;
    frac[best] = -1.0;
  }
}

} // namespace detail

// Plan the kink-aligned split of [k_lo, k_hi] over a budget of `n_nodes`
// DISTINCT nodes (odd), with the strip's wing trust half-band `wing_band`
// (<= 0 when the clamp is off).
//
// BUDGET APPORTIONMENT. The interval count n_nodes - 1 is handed out in UNITS
// of 4 intervals whenever it divides by 4, which is what puts every panel on
// the 4m+1 Richardson lattice; every default grid takes that path (the tier
// defaults are 97/257/769/2049, and both the adaptive span rescale and the C-2
// resolution floor round back onto 4m+1). Each panel gets one mandatory unit
// and the spare units go out proportionally to length, so total intervals stay
// exactly n_nodes - 1 and total distinct nodes exactly n_nodes.
//
// The panels' spacings therefore differ slightly from the un-split uniform
// dk = span/(n_nodes-1) -- integer apportionment cannot divide a length
// evenly. The excess is bounded exactly by
//
//     dk_i < span / (intervals - unit*count)
//
// (from share_i > spare*len_i/span), i.e. under 7% at Standard's 256 intervals
// and 1.6% in fact. C-2's dk <= sigma_atm*sqrt(T)/4 resolution floor is sized
// against THAT bound rather than against the nominal dk (`dk_floor_nodes`
// takes the panel count for exactly this reason), so its guarantee stays
// exact per-panel and not merely approximate.
//
// DEGRADATION LADDER, in priority order -- a starved budget gives up the
// smallest kink first, the error estimate next, and the split itself last:
//   1. all kinks, units of 4        (every default grid; estimate populated)
//   2. k = 0 only, units of 4       (the clamp edges' slope jump is smaller
//                                    than k = 0's by orders of magnitude)
//   3. as above but units of 2      (estimate goes NaN, kink stays aligned)
//   4. no split                     (fewer than 2 intervals per panel)
//
// Pure function of its four arguments: same grid in, same panels out.
[[nodiscard]] inline StripSplit plan_strip_split(double k_lo, double k_hi,
                                                 std::size_t n_nodes,
                                                 double wing_band) noexcept {
  StripSplit out;
  out.panels[0] = StripPanel{k_lo, k_hi, n_nodes};
  out.count = 1;
  out.richardson_ok = (n_nodes % 4u) == 1u && n_nodes >= 5u;
  if (!std::isfinite(k_lo) || !std::isfinite(k_hi) || !(k_hi > k_lo) || n_nodes < 3u ||
      (n_nodes % 2u) == 0u) {
    return out; // not a composite-Simpson grid: quadrature it as one panel
  }

  const std::size_t intervals = n_nodes - 1u;
  std::size_t unit = ((intervals % 4u) == 0u) ? 4u : 2u;

  std::array<double, kMaxStripPanels + 1> bound{};
  std::size_t nb = detail::strip_panel_bounds(k_lo, k_hi, wing_band, true, bound);
  if (intervals / unit < nb - 1u) {
    nb = detail::strip_panel_bounds(k_lo, k_hi, wing_band, false, bound);
  }
  if (unit == 4u && intervals / unit < nb - 1u) {
    unit = 2u;
  }
  if (intervals / unit < nb - 1u) {
    return out; // budget cannot give every panel even one Simpson pair
  }

  const std::size_t count = nb - 1u;
  std::array<double, kMaxStripPanels> len{};
  for (std::size_t i = 0; i < count; ++i) {
    len[i] = bound[i + 1] - bound[i];
  }
  std::array<std::size_t, kMaxStripPanels> share{};
  detail::apportion_units(len, count, intervals / unit - count, share);

  out.count = count;
  bool rich = true;
  for (std::size_t i = 0; i < count; ++i) {
    const std::size_t panel_intervals = share[i] * unit;
    out.panels[i] = StripPanel{bound[i], bound[i + 1], panel_intervals + 1u};
    rich = rich && ((panel_intervals % 4u) == 0u);
  }
  out.richardson_ok = rich;
  return out;
}

// Panel count of the FULL kink-aligned split of [k_lo, k_hi] -- every interior
// kink cut -- independent of any node budget. `plan_strip_split` can end up
// with FEWER panels when a starved budget walks the degradation ladder, never
// with more, so this is the count the C-2 resolution floor provisions against.
[[nodiscard]] inline std::size_t strip_panel_count(double k_lo, double k_hi,
                                                   double wing_band) noexcept {
  std::array<double, kMaxStripPanels + 1> bound{};
  return detail::strip_panel_bounds(k_lo, k_hi, wing_band, true, bound) - 1u;
}

// Widest node spacing over the plan's panels. Once the grid is no longer one
// uniform lattice this -- not span/(n-1) -- is the quantity C-2's resolution
// ceiling constrains, so it is what the LowT flag must be decided on.
[[nodiscard]] inline double max_panel_spacing(const StripSplit& split) noexcept {
  double dk = 0.0;
  for (std::size_t i = 0; i < split.count; ++i) {
    const StripPanel& panel = split.panels[i];
    if (panel.n_nodes > 1u) {
      dk = std::max(dk, (panel.k_hi - panel.k_lo) /
                            static_cast<double>(panel.n_nodes - 1u));
    }
  }
  return dk;
}

// The ONE bracketing-pillar forward blend: LINEAR IN log(F).
//
// `projection.cpp`'s `curve_forward_T` already used this convention; the
// derivatives var strip used linear-in-F. Same forward curve, two answers at any
// T strictly between two pillars with F0 != F1. Both now call this.
//
// Linear-in-log-F keeps F strictly positive and is the convention that composes
// with the log-forward-moneyness grid the strip integrates on. Degenerate
// inputs — a non-increasing bracket, or a non-positive pillar forward where the
// log is undefined — fall back to the linear reading rather than returning NaN.
[[nodiscard]] inline double forward_log_blend(double t0, double f0, double t1, double f1,
                                              double T) noexcept {
  if (!(t1 > t0)) {
    return f0;
  }
  const double alpha = (T - t0) / (t1 - t0);
  if (!(f0 > 0.0) || !(f1 > 0.0)) {
    return f0 + alpha * (f1 - f0);
  }
  const double log_f = std::log(f0) + alpha * (std::log(f1) - std::log(f0));
  return std::exp(log_f);
}

} // namespace atx::vol::strip
