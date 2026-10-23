#pragma once

// ── K2 (r5, 2026-10-22): varying-parameter batch American IV inversion on a (σ, q) family ──
//
// invert_many inverts N prices of ONE (expiry, side) family at once. Every lane has its
// own strike K[i], target price, contract yield q[i] and its own σ iterate; the family
// map is K1's fused price + analytic vega (american_sigma_q_family.hpp). Inputs and
// outputs are structure-of-arrays spans of equal length.
//
// THE ROOT. Per lane a bounded, bracketed Newton on the family map P(σ) at the lane's
// own (K, q): the bracket [lo, hi] starts at the family's σ box (or the caller's retained
// bracket, or an anchor lane's evaluated points) and shrinks with every evaluation; a
// Newton step is taken only inside the bracket, else the lane bisects (or, toward a box
// edge never evaluated, evaluates that edge to settle a box miss). A lane converges when
// the Newton distance |P(x) − target| / P'(x) at an evaluated x is <= tol with P'(x) above
// the vega floor (the root is x minus that step), or when an evaluated bracket is no wider
// than 2 tol (the root is its midpoint). Nothing restarts: a failed seed narrows the
// bracket, and every exit returns the best evaluated bracket with its end prices, the
// last residual and vega, so the next route starts from what this one learned.
//
// TYPED OUTCOMES (InverseStatus), one per lane, never a shared flag: converged (with a
// usable slope at the root), converged without a usable slope (a 2 tol bracket whose last
// point's vega is NaN or <= the floor), low vega, exercise-flat (vega <= floor on the
// exercise plateau, price at intrinsic), σ below / above the box, q outside the box (also
// a q tile that cannot be cut), nonconverged at the evaluation cap, invalid input,
// unsupported ISA. A failed or out-of-bracket seed is NOT a terminal outcome — the lane
// starts from the bracket and records lane_flag::kSeedFailed / kSeedClamped (counted).
// No path calls the Search route or any cold solve: the caller decides with the bracket.
//
// BID -> MID -> ASK. anchor[i] = j makes lane i (a bid or an ask) start from lane j's
// outcome (the quote's mid, same K / q / side): P is increasing in σ, so every point j
// evaluated brackets i on one side, and j's last point and vega give i a Newton seed. An
// anchor lane must itself be anchor-free; dependents run right after their anchor in
// the same slot (one q contraction serves the chain).
//
// ISA. A table source (PremiumTable, K1's precontracted snapshot) runs the 4-lane AVX2
// kernel (simd/american_iv_batch_avx2.hpp) when InverseIsa and the host allow, else the
// scalar reference; a family source (AlSigmaQFamily) and a table of an AlProfile::Fast
// family (PremiumTable::fast: the kernel implements the full-precision map only) always
// run the scalar reference — InverseIsa::Avx2 on them is UnsupportedIsa. The scalar path
// IS K1's map bit for bit (price_vega_on /
// price_and_vega). The AVX2 kernel evaluates the same map with the vector Cody Φ / φ /
// exp / log (simd/vector_math.hpp): a ROUNDING-CLASS difference (documented in
// docs/american-iv-batch.md), so lanes may take different iterate paths and agree to the
// root tolerance; each lane's result is a pure function of its own inputs on either path
// (independent of batch order and of the other lanes in its vector).
//
// ALLOCATION. invert_many allocates nothing once `scratch` has seen a batch of this
// size (InverseScratch::reserve); a larger batch grows the scratch first (the only
// allocation, outside the per-lane loop). Thread-safe on distinct scratches; the family /
// table is read-only.
//
// NOT a public API. Not installed. No stability guarantees.

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <span>

#include "atx/vol/api/pricing/american.hpp"
#include "pricing/american_iv_batch_status.hpp"
#include "pricing/american_sigma_q_family.hpp"

namespace atx::vol::iv_batch {

inline constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
// Family evaluations (price + vega) one lane may spend. A bracketed Newton from a usable
// seed converges in 2-5; the cap bounds the bisection-heavy flat-map lanes.
inline constexpr std::uint16_t kInverseMaxEvaluations = 16;
// The vega floor relative to the strike, the fit's rising-slope floor (american_iv.cpp
// kCertSlopeFloorRel): a Newton step needs P'(σ) > kInverseVegaFloorRel · K.
inline constexpr double kInverseVegaFloorRel = 1.0e-8;
// The AVX2 kernel's lane width.
inline constexpr std::size_t kInverseLaneWidth = 4;

// InverseStatus / kInverseStatusCount / inverse_status_name: american_iv_batch_status.hpp.

// Per-lane diagnostics (bit set in InverseBatchOut::flags).
namespace lane_flag {
inline constexpr std::uint8_t kSeedFailed = 1u;       // seed absent / not finite: bracket midpoint
inline constexpr std::uint8_t kSeedClamped = 2u;      // seed outside the bracket: moved inside
inline constexpr std::uint8_t kAnchored = 4u;         // started from an anchor lane's outcome
inline constexpr std::uint8_t kRetainedBracket = 8u;  // started from the caller's bracket
inline constexpr std::uint8_t kBracketConverged = 16u; // converged on the bracket width
inline constexpr std::uint8_t kBoxEdgeEvaluated = 32u; // evaluated a σ box edge
inline constexpr std::uint8_t kDegenerateVega = 64u;   // a NearNodeDegenerate point (vega NaN)
inline constexpr std::uint8_t kBisected = 128u;        // took at least one bisection step
} // namespace lane_flag

enum class InverseIsa : std::uint8_t {
  Auto = 0,   // AVX2 when the source is a table and simd::use_avx2() holds, else scalar
  Scalar = 1, // the scalar reference (K1's map bit for bit)
  Avx2 = 2,   // the AVX2 kernel or UnsupportedIsa on every active lane
};

// The pure ISA decision (testable for a host without AVX2 on this host). avx2_source: the
// source is a table on the full-precision arithmetic (not a family, not PremiumTable::fast).
[[nodiscard]] constexpr bool resolve_avx2(InverseIsa isa, bool avx2_source, bool host_avx2,
                                          bool auto_allows) noexcept {
  switch (isa) {
  case InverseIsa::Scalar:
    return false;
  case InverseIsa::Avx2:
    return avx2_source && host_avx2;
  case InverseIsa::Auto:
    break;
  }
  return avx2_source && host_avx2 && auto_allows;
}

// A lane's acceptance record — the SLOT for K3's RootAcceptance (lane K3,
// american_family_error.hpp, not yet on this branch). Until K3 lands it carries only what
// the batch measured on the FAMILY map: how the root was accepted, its Newton distance in
// vol and the evaluated bracket that encloses it. It is not a bound against the accurate
// map and never by itself skips a confirmation.
enum class AcceptanceKind : std::uint8_t {
  None = 0,           // not converged
  NewtonDistance = 1, // |P(x) − target| / P'(x) <= tol at an evaluated x (P' > floor)
  FamilyBracket = 2,  // an evaluated family-map bracket no wider than 2 tol (status
                      // Converged, or ConvergedNoSlope without a usable slope there)
};

struct LaneAcceptance {
  AcceptanceKind kind = AcceptanceKind::None;
  double newton_distance = kNaN; // vol units (absolute σ)
  double interval_lo = kNaN;     // the evaluated bracket at exit (family map; NaN: an
  double interval_hi = kNaN;     // unevaluated end)
};

struct InverseBatchIn {
  // The family (group invariant): exactly one of the two. The table is a snapshot (it
  // never reads a family); the family must outlive the call.
  const sigma_q_family_detail::PremiumTable *table = nullptr;
  const AlSigmaQFamily *family = nullptr;
  double S = kNaN;  // spot (> 0)
  double tol = 1.0e-7; // absolute σ (the fit's iv_tolerance)
  std::uint16_t max_evaluations = kInverseMaxEvaluations;
  double vega_floor_rel = kInverseVegaFloorRel;
  InverseIsa isa = InverseIsa::Auto;
  // Per lane (all the same length n; the optional ones may be empty = absent everywhere).
  std::span<const double> K;
  std::span<const double> target; // the price to invert (USD, the family's side)
  std::span<const double> q;      // the contract's effective yield (inside the q box)
  std::span<const std::uint8_t> is_call; // must match the family's side
  std::span<const std::uint8_t> active;  // optional; 0 = masked off
  std::span<const double> seed_sigma;    // optional; NaN = none
  // optional retained bracket from an earlier route (NaN = that end unknown); price_lo /
  // price_hi are the FAMILY prices at those ends when evaluated (NaN = unevaluated).
  std::span<const double> bracket_lo;
  std::span<const double> bracket_hi;
  std::span<const double> price_lo;
  std::span<const double> price_hi;
  std::span<const std::int32_t> anchor; // optional; -1 = none (see BID -> MID -> ASK)
};

struct InverseBatchOut {
  std::span<double> sigma;      // the root (Converged / ConvergedNoSlope), else the best
                                // evaluated iterate
  std::span<double> bracket_lo; // the retained bracket (family map)
  std::span<double> bracket_hi;
  std::span<double> price_lo; // family prices at the bracket ends (NaN: unevaluated)
  std::span<double> price_hi;
  std::span<double> residual; // P(x) − target at the last evaluated x (USD; NaN: none)
  std::span<double> vega;     // P'(x) there (NaN: none / degenerate)
  std::span<double> last_sigma; // that x (NaN: none)
  std::span<std::uint16_t> evaluations;
  std::span<InverseStatus> status;
  std::span<std::uint8_t> flags;
  std::span<LaneAcceptance> acceptance;
};

// What a call spent (added to, never reset by invert_many).
struct InverseBatchWork {
  std::uint64_t lanes = 0;            // lanes seen (active or not)
  std::uint64_t active_lanes = 0;
  std::uint64_t evaluations = 0;      // lane evaluations (price + vega), either path
  std::uint64_t vector_steps = 0;     // AVX2 kernel invocations
  std::uint64_t vector_lane_slots = 0; // active lanes summed over those invocations
  std::uint64_t scalar_evaluations = 0;
  std::uint64_t q_contractions = 0;   // tiles cut (table source)
  std::uint64_t tile_reuses = 0;      // a chain lane on its anchor's tile
  std::uint64_t seed_failed = 0;
  std::uint64_t seed_clamped = 0;
  std::uint64_t anchored = 0;
  // AVX2 lane evaluations re-run on the scalar map: inside the exercise rule's recheck band
  // (simd::iv_batch_avx2::kBoundaryRecheckRel), so the classification is the scalar map's.
  std::uint64_t boundary_rechecks = 0;
  std::array<std::uint64_t, kInverseStatusCount> by_status{};
  bool avx2 = false; // the last call ran the AVX2 kernel
  void add(const InverseBatchWork &o) noexcept;
};

// Batch-level refusal (nothing written).
enum class InverseBatchError : std::uint8_t {
  None = 0,
  SpanMismatch = 1,   // a span length differs from n (required spans) or is not 0 / n
  NoFamily = 2,       // neither or both sources, or a source that is not prepared / ok
  ScratchFailed = 3,  // growing the scratch failed (std::bad_alloc)
};

// Preallocated work memory of invert_many (opaque; one per thread).
class InverseScratch {
public:
  InverseScratch();
  ~InverseScratch();
  InverseScratch(InverseScratch &&) noexcept;
  InverseScratch &operator=(InverseScratch &&) noexcept;
  InverseScratch(const InverseScratch &) = delete;
  InverseScratch &operator=(const InverseScratch &) = delete;
  // Size for batches of up to n lanes (allocates; invert_many then allocates nothing).
  void reserve(std::size_t n);
  [[nodiscard]] std::size_t capacity() const noexcept;

  struct Impl;
  [[nodiscard]] Impl &impl() noexcept { return *impl_; }

private:
  std::unique_ptr<Impl> impl_;
};

// Invert every active lane (contract at the top). Returns None and writes every output
// span of every active lane; an Inactive lane's outputs are left as they were except
// status = Inactive. Never throws.
[[nodiscard]] InverseBatchError invert_many(const InverseBatchIn &in, const InverseBatchOut &out,
                                            InverseScratch &scratch,
                                            InverseBatchWork *work = nullptr) noexcept;

} // namespace atx::vol::iv_batch
