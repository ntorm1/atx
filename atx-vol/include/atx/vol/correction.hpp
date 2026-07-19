#pragma once

// American-minus-European correction cache.
//
// Ported from the C `ats-vol` library (ats_pricer_correction.c and
// ats_pricer_correction_cheb.c). The hot-path American pricer is European
// Black-76 plus a trivariate-tensor Chebyshev interpolation of the correction
//
//     C(k_log, T, sigma) = (P_american - P_european) / F
//
// scaled by the forward F so the cache is dimensionless. The table stores the
// n_k × n_T × n_s Chebyshev coefficient tensor in DCT-II form; evaluation is
// three nested Clenshaw recursions.
//
// The table is populated once per underlier at surface-fit cadence using the
// Andersen-Lake cold pricer (american.hpp). Populate normalizes to F = 1 by
// setting S = e^{-(r-q)T}, K = e^{k_log}; the correction then depends only on
// (k_log, T, sigma) given (r, q, side). Hot-path callers supply their own F.
//
// Ownership: the coefficient tensor lives in a `std::vector<double>` owned by
// the cache (Rule of Zero) — no arena, no raw new/delete. The cache is a plain
// value type; `build()` returns one by value. Once built it is read-only: any
// number of threads may query concurrently; `build`/`set_extrap_policy` mutate.
//
// The C library shipped AVX2 batch4 query variants and an on-disk archive
// format; both are deferred in this port.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

#include "atx/vol/american.hpp" // AlOpts
#include "atx/vol/types.hpp"

namespace atx::vol {

// Out-of-box query policy, stamped on the cache. Mirrors the C
// `AtsVolCorrExtrapPolicy`. CLAMP is the default and clamps queries to the box
// edge; the other two surface the boundary explicitly for strategy callers.
enum class ExtrapPolicy : std::uint8_t {
  Clamp = 0,        // clamp value to box edge, zero out-of-axis partials
  NanOutside = 1,   // value/partials NaN beyond the box, status Ok
  ErrorOutside = 2, // OutOfRange error beyond the box
};

// Which correction partials a query should populate. Bitmask; combine with `|`.
enum class CorrPartials : std::uint32_t {
  None = 0,
  Value = 1u << 0,  // always implied; included for clarity
  Dk = 1u << 1,     // d/dk_log
  Dt = 1u << 2,     // d/dT
  Dsigma = 1u << 3, // d/dsigma
};

[[nodiscard]] constexpr CorrPartials operator|(CorrPartials a, CorrPartials b) noexcept {
  return static_cast<CorrPartials>(static_cast<std::uint32_t>(a) | static_cast<std::uint32_t>(b));
}
[[nodiscard]] constexpr CorrPartials operator&(CorrPartials a, CorrPartials b) noexcept {
  return static_cast<CorrPartials>(static_cast<std::uint32_t>(a) & static_cast<std::uint32_t>(b));
}
[[nodiscard]] constexpr bool any(CorrPartials a) noexcept {
  return static_cast<std::uint32_t>(a) != 0u;
}
[[nodiscard]] constexpr bool has(CorrPartials set, CorrPartials flag) noexcept {
  return any(set & flag);
}

// Result of a mask-driven query. Partials not requested are left at 0.0.
struct CorrResult {
  double value = 0.0;
  double dk_log = 0.0;
  double dT = 0.0;
  double dsigma = 0.0;
  CorrPartials mask_filled = CorrPartials::None; // echoes Value | requested partials
};

// Value, gradient, and the four Hessian entries consumed by cached American
// Greeks. All derivatives are in physical (k_log, T, sigma) coordinates.
struct CorrSecondOrder {
  double value = 0.0;
  double dk_log = 0.0;
  double dT = 0.0;
  double dsigma = 0.0;
  double dkk = 0.0;
  double dk_dT = 0.0;
  double dk_dsigma = 0.0;
  double dsigma2 = 0.0;
};

// Cached (P_amer - P_euro)/F correction surface over a (k_log, T, sigma) box.
class CorrectionCache {
public:
  // A default-constructed cache is empty/unpopulated; every eval reads as 0.
  CorrectionCache() = default;

  // Build and populate the cache in one shot. Chebyshev node counts are bounded
  // by kChebMaxNodes; the box must be strictly ordered with T_min, sigma_min > 0.
  // `opts` selects the Andersen-Lake accuracy preset used to sample the grid.
  //
  // @return OutOfRange      — a node count exceeds kChebMaxNodes
  //         InvalidArgument — a zero node count, or an inverted / non-positive box
  [[nodiscard]] static Result<CorrectionCache>
  build(std::uint16_t n_log_moneyness, std::uint16_t n_T_nodes, std::uint16_t n_sigma_nodes,
        double r, double q, double k_log_min, double k_log_max, double T_min, double T_max,
        double sigma_min, double sigma_max, Side side,
        const std::optional<AlOpts> &opts = std::nullopt);

  [[nodiscard]] bool populated() const noexcept { return populated_; }
  [[nodiscard]] Side side() const noexcept { return side_; }
  [[nodiscard]] std::uint16_t n_k() const noexcept { return n_k_; }
  [[nodiscard]] std::uint16_t n_T() const noexcept { return n_T_; }
  [[nodiscard]] std::uint16_t n_s() const noexcept { return n_s_; }

  // Interpolation-box bounds and baked carry (C2 cross-date cache-reuse stale-gate:
  // a reused cache is valid for a new board only when the new board's queried
  // (k_log, T) box lies INSIDE this box — no extrapolation — and the new board's
  // representative carry is within tolerance of this baked (r, q), the two
  // conditions the cache's own self-consistency already requires).
  [[nodiscard]] double k_log_min() const noexcept { return k_log_min_; }
  [[nodiscard]] double k_log_max() const noexcept { return k_log_max_; }
  [[nodiscard]] double T_min() const noexcept { return T_min_; }
  [[nodiscard]] double T_max() const noexcept { return T_max_; }
  [[nodiscard]] double sigma_min() const noexcept { return sigma_min_; }
  [[nodiscard]] double sigma_max() const noexcept { return sigma_max_; }
  [[nodiscard]] double baked_r() const noexcept { return r_; }
  [[nodiscard]] double baked_q() const noexcept { return q_; }

  // True only for a finite point inside the closed interpolation box of a
  // populated cache. Fast serving code uses this before the raw evaluators,
  // whose intentional low-level contract is to clamp outside the box.
  [[nodiscard]] bool contains(double k_log, double T, double sigma) const noexcept;

  // Value-only evaluation. Out-of-box queries always clamp to the box edge (the
  // extrap policy governs `query`, not this raw evaluator). Returns 0 when
  // unpopulated. The correction is non-negative by construction; tiny negatives
  // from interpolation noise are clamped to 0.
  [[nodiscard]] double eval(double k_log, double T, double sigma) const noexcept;

  // Value + selected first-order partials. Partials whose axis is out of the box
  // are zeroed (the value still clamps). Pass nullptr to skip a partial. The
  // return (the value) may be discarded when only the partials are wanted; when
  // it is, call eval_partials instead to skip the (discarded) value sweep.
  //
  // Bit-identical to eval() for the value and to eval_partials() for the partials
  // (it is literally their composition), so callers can mix the three freely.
  double eval_grad(double k_log, double T, double sigma, double *out_dk_log, double *out_dT,
                   double *out_dsigma) const noexcept;

  // Value plus k_log partial for the delta-only route. This shares coordinate
  // setup and scratch across the two tensor sweeps and applies the same
  // max(0, polynomial) derivative contract as eval_second_order().
  double eval_value_dk(double k_log, double T, double sigma, double *out_dk_log) const noexcept;

  // Selected first-order partials ONLY — never runs the 3D Chebyshev value sweep.
  // Identical partials, box handling, and nullptr-skip semantics to eval_grad; use
  // this on the hot path when the value is not wanted (e.g. the finite-difference
  // stencils in american_greeks that consume only a partial). noexcept and
  // allocation-free, like eval / eval_grad.
  void eval_partials(double k_log, double T, double sigma, double *out_dk_log, double *out_dT,
                     double *out_dsigma) const noexcept;

  // Fused value/gradient/Hessian evaluation for the cached-American Greek
  // bundle. A single traversal of the coefficient tensor differentiates the
  // nested Clenshaw recurrences directly; no finite-difference stencil and no
  // dynamic allocation are used. Out-of-box axes clamp and zero every
  // derivative involving that axis, matching eval_partials().
  [[nodiscard]] CorrSecondOrder eval_second_order(double k_log, double T,
                                                  double sigma) const noexcept;

  // Mask-driven query honouring the stamped extrap policy.
  //
  // @return OutOfRange under ErrorOutside when the point is beyond the box.
  [[nodiscard]] Result<CorrResult> query(double k_log, double T, double sigma,
                                         CorrPartials want) const;

  // Set / read the out-of-box extrap policy. Rejects an unrecognized value.
  [[nodiscard]] Status set_extrap_policy(ExtrapPolicy policy) noexcept;
  [[nodiscard]] ExtrapPolicy extrap_policy() const noexcept { return extrap_policy_; }

private:
  double k_log_min_ = 0.0;
  double k_log_max_ = 0.0;
  double T_min_ = 0.0;
  double T_max_ = 0.0;
  double sigma_min_ = 0.0;
  double sigma_max_ = 0.0;
  double scale_k_ = 0.0;
  double scale_T_ = 0.0;
  double scale_s_ = 0.0;
  double r_ = 0.0;
  double q_ = 0.0;
  std::uint16_t n_k_ = 0;
  std::uint16_t n_T_ = 0;
  std::uint16_t n_s_ = 0;
  Side side_ = Side::Put;
  bool populated_ = false;
  ExtrapPolicy extrap_policy_ = ExtrapPolicy::Clamp;
  std::vector<double> coefs_; // n_k * n_T * n_s Chebyshev coefficients
  // T16b: k_log-axis derivative-coefficient tensor (C_k), precomputed once in
  // build() by running cheb_diff_coefs over each contiguous i-row of coefs_ (box
  // axis scale folded in). Same n_k * n_T * n_s shape / cheb_idx layout as coefs_.
  // eval_partials reads dC/dk_log as a plain value Clenshaw over this tensor,
  // hoisting the k_log partial's per-query row differentiation to build time.
  // Bit-identical to the in-pass diff because the k_log diff is the innermost op
  // (no prior collapse to reorder against). Empty on a default-constructed cache.
  std::vector<double> dk_coefs_;
};

// ── Optional per-side hot-path caches (de-Americanization / parity) ──────
//
// The correction cache is built per (side): its baked (r, q, side) fix the
// early-exercise premium surface. A de-Americanization run inverts the OTM leg
// of every strike — CALLS above the forward, PUTS below — so it needs BOTH a
// call and a put cache to accelerate the whole chain. This lightweight bundle
// carries the two non-owning pointers; a null member (or a default-constructed
// bundle) selects the COLD Andersen-Lake path for that side, so passing an empty
// bundle is exactly the pre-cache behavior.
//
// Self-consistency: when the SAME cache accelerates both the inversion (market
// premium -> European-equivalent vol) and the re-Americanization (model vol ->
// fair value), the round-trip is a self-consistent map through
// `american_price_cached`, so the fair-value-within-bid-ask parity holds even
// though the cache is an interpolation of the cold pricer.
// Non-owning, call-constant linear blend of two fixed-carry correction caches.
// `upper_weight` is deliberately NOT a function of T inside evaluation: callers
// resolve it once from the carry curve, then value and every derivative blend as
// (1-w)*lower + w*upper. In particular d/dT holds w fixed, preserving the
// fixed-(r,q) theta/charm semantics of each underlying cache.
struct CorrectionBlend {
  const CorrectionCache *lower = nullptr;
  const CorrectionCache *upper = nullptr;
  double upper_weight = 0.0;

  [[nodiscard]] static constexpr CorrectionBlend single(const CorrectionCache *cache) noexcept {
    return CorrectionBlend{cache, nullptr, 0.0};
  }

  // True when the active endpoint(s) are populated, the weight is finite and in
  // [0,1], and an interior blend joins caches for the same option side.
  [[nodiscard]] bool valid() const noexcept;
  [[nodiscard]] bool usable(Side side) const noexcept;

  // Invalid blends return NaN (for every field in the second-order bundle).
  // Exact endpoints and identical pointers evaluate only one cache.
  [[nodiscard]] double eval(double k_log, double T, double sigma) const noexcept;
  // Sigma partial only, for IV-inversion Newton steps. Uses each endpoint's
  // partial-only kernel and never constructs the full second-order jet.
  [[nodiscard]] double eval_dsigma(double k_log, double T, double sigma) const noexcept;
  double eval_value_dk(double k_log, double T, double sigma, double *out_dk_log) const noexcept;
  [[nodiscard]] CorrSecondOrder eval_second_order(double k_log, double T,
                                                  double sigma) const noexcept;
};

struct AmericanCorrectionCaches {
  const CorrectionCache *call = nullptr;
  const CorrectionCache *put = nullptr;

  // The cache to use for `s`, or nullptr when that side is not cached.
  [[nodiscard]] const CorrectionCache *for_side(Side s) const noexcept {
    return (s == Side::Call) ? call : put;
  }
  // True iff at least one side is cached.
  [[nodiscard]] bool any() const noexcept { return call != nullptr || put != nullptr; }
};

// ── Chebyshev primitives (first-kind / roots grid) ──────────────────────
//
// Internal to the correction cache but exposed so the DCT-II round-trip,
// Clenshaw, and derivative recurrences can be locked directly in tests. Raw
// pointers are non-owning observers with the documented length contracts.
namespace detail {

// Max nodes per axis (matches C ATS_CHEB_MAX_NODES).
inline constexpr std::uint16_t kChebMaxNodes = 64;

// j-th first-kind Chebyshev node, x = cos(pi (2j+1) / (2n)) in [-1, 1].
[[nodiscard]] double cheb_node(std::uint16_t j, std::uint16_t n) noexcept;

// Affine maps between physical box [a, b] and the unit interval [-1, 1].
[[nodiscard]] constexpr double cheb_to_unit(double x, double a, double b) noexcept {
  return (2.0 * x - (a + b)) / (b - a);
}
[[nodiscard]] constexpr double cheb_from_unit(double xi, double a, double b) noexcept {
  return 0.5 * (a + b) + 0.5 * (b - a) * xi;
}

// Tensor layout: coef[i, j, k] lives at j*(n_s*n_k) + k*n_k + i (i = k_log axis,
// innermost/contiguous; j = T axis; k = sigma axis).
[[nodiscard]] constexpr std::size_t cheb_idx(std::uint16_t i, std::uint16_t j, std::uint16_t k,
                                             std::uint16_t n_k, std::uint16_t n_s) noexcept {
  return static_cast<std::size_t>(j) * static_cast<std::size_t>(n_s) *
             static_cast<std::size_t>(n_k) +
         static_cast<std::size_t>(k) * static_cast<std::size_t>(n_k) + static_cast<std::size_t>(i);
}

// Forward DCT-II: n function values at Chebyshev nodes -> n coefficients.
// `vals` and `coefs` must not alias; both have length n.
void cheb_dct2(const double *vals, double *coefs, std::uint16_t n) noexcept;

// 1D Clenshaw evaluation p(x) = a_0 + sum_{k>=1} a_k T_k(x). `coefs` length n.
[[nodiscard]] double cheb_clenshaw1d(const double *coefs, std::uint16_t n, double x) noexcept;

// Derivative-coefficient transform (Numerical Recipes 5.9). `c`/`d` length n,
// must not alias; `scale` = 2/(b-a) maps back to physical units on box [a, b].
void cheb_diff_coefs(const double *c, double *d, std::uint16_t n, double scale) noexcept;

// 3D Clenshaw evaluation. `coefs` laid out per cheb_idx(); `tmp_jk` scratch of
// at least n_T*n_s doubles.
[[nodiscard]] double cheb_clenshaw3d(const double *coefs, std::uint16_t n_k, std::uint16_t n_T,
                                     std::uint16_t n_s, double xi, double xj, double xk,
                                     double *tmp_jk) noexcept;

// 3D Clenshaw of a partial derivative along `diff_axis` (0 = k_log, 1 = T,
// 2 = sigma). `axis_scale` = 2/(box_max - box_min) for that axis. `tmp_jk`
// scratch of at least n_T*n_s doubles.
[[nodiscard]] double cheb_clenshaw3d_partial(const double *coefs, std::uint16_t n_k,
                                             std::uint16_t n_T, std::uint16_t n_s, double xi,
                                             double xj, double xk, int diff_axis, double axis_scale,
                                             double *tmp_jk) noexcept;

} // namespace detail

} // namespace atx::vol
