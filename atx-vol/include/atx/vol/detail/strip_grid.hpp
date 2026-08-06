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
// (`DerivConfig::wing_clamp_k == 0`), in absolute log-forward-moneyness. MUST
// stay equal to the risk-validation band `RiskSurfaceValidationConfig{}.k_max`
// (detail/risk_surface_validation.hpp) — the clamp's whole claim is "the strip
// trusts the surface exactly where the pipeline certified it", and the claim
// dissolves if the two constants drift apart. static_asserted against the
// validation config at the use site in derivatives.cpp.
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

// Minimum node count that keeps `span`'s own grid spacing at or under
// `dk_max` (from `dk_ceiling`). Preserves the 4m+1 Richardson invariant the
// same way the span-driven rescale (FIX-E M-7, derivatives.cpp) does: force
// odd, then nudge +2 if that lands off 4m+1 -- odd counts alternate 1 mod 4 /
// 3 mod 4 as they step by two, so a single +2 always suffices. Returns
// `current_n` unchanged when `span`/`dk_max` is non-positive (no floor is
// expressible) or the current spacing already satisfies it -- the caller
// compares the result against `current_n` to learn whether the floor
// actually engaged.
[[nodiscard]] inline std::size_t dk_floor_nodes(double span, std::size_t current_n,
                                                double dk_max) noexcept {
  if (!(span > 0.0) || !(dk_max > 0.0) || current_n < 2u) {
    return current_n;
  }
  const double dk = span / static_cast<double>(current_n - 1u);
  if (dk <= dk_max) {
    return current_n;
  }
  const double intervals = span / dk_max;
  std::size_t n = odd_nodes(static_cast<std::size_t>(std::ceil(intervals)) + 1u, current_n);
  if ((n % 4u) != 1u) {
    n += 2u;
  }
  return n;
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
// evenly. The bound is dk_i <= dk * intervals/(intervals - unit*count)
// (share_i >= spare*len_i/span), i.e. under 7% at Standard's 256 intervals and
// 1.6% in fact; C-2's dk <= sigma_atm*sqrt(T)/4 resolution floor keeps holding
// with that much slack.
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
