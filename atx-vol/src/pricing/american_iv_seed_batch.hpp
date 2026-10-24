#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>

#include "atx/vol/api/pricing/american_iv.hpp"
#include "atx/vol/api/pricing/american_region.hpp"

namespace atx::vol {

// Private experiment only: vector arithmetic has an empirical allowance, not
// a certified enclosure of the scalar Reference map. No public/default change.
enum class IvPriceSeedKernel { ScalarReference, VectorReferenceEmpirical };
enum class IvPriceSeedOrigin { None, ScalarReference, VectorReferenceEmpirical };
enum class IvPriceSeedRetry {
  None, ScalarMode, UnsupportedScope, NonmonotoneBand, Tail, KernelRefusal,
  ExerciseEdge, InsufficientMargin, FloorRefusal
};
inline constexpr std::size_t kIvPriceSeedRetryCount =
    static_cast<std::size_t>(IvPriceSeedRetry::FloorRefusal) + 1;
struct IvPriceSeedRequest {
  double price = 0.0, S = 0.0, K = 0.0, T = 0.0, r = 0.0, q = 0.0;
  double sigma = 0.0, budget = 0.0;
  Side side = Side::Put;
};
struct IvPriceSeedResult {
  std::optional<double> sigma;
  IvPriceSeedOrigin origin = IvPriceSeedOrigin::None;
  IvPriceSeedRetry retry = IvPriceSeedRetry::None;
  // Vector observation only, including declined candidates. Scalar fallback
  // does not populate these with a second diagnostic pricing call.
  double target_price = std::numeric_limits<double>::quiet_NaN();
  double boundary = std::numeric_limits<double>::quiet_NaN(); // internal put units
  double boundary_distance = std::numeric_limits<double>::quiet_NaN();
  double empirical_allowance = 0.0;
  AlExerciseClass exercise = AlExerciseClass::UnsupportedRegime;
  bool allocation_refused = false; // escaped cold-floor allocation, not optional-cache refusal
};
struct IvPriceSeedBatchStats {
  std::uint64_t requests = 0, vector_packs = 0, vector_lanes = 0, vector_admitted = 0;
  std::uint64_t scalar_fallbacks = 0, scalar_admitted = 0;
  std::uint64_t floor_preparations = 0, floor_hits = 0;
  std::uint64_t allocation_refusals = 0;
  std::uint64_t vector_ns = 0, scalar_ns = 0, floor_ns = 0, inclusive_ns = 0;
};

namespace research {
// Test-only synchronous borrowed observer. Called only after the optional floor
// cache refused and immediately before the actual cold floor evaluation. It may
// inject bad_alloc to exercise that otherwise allocation-free success path.
// Ordinary callers omit it; it cannot supply or replace a numerical value.
struct IvSeedColdFloorTestHook {
  void (*before_price)(std::size_t request_index, void *context) = nullptr;
  void *context = nullptr;
};
} // namespace research

// Every numerically declined experiment lane runs the unchanged scalar helper.
// Only full packs are vectorized; tails retain scalar arithmetic. Cold scalar
// floor checks and nonmonotone chord semantics are mandatory. Resets results and
// stats; false denotes invalid shape/mode, never a per-lane numerical refusal.
// Input and output spans must be disjoint and live through the call.
// No retained state. Allocation is limited to the existing boundary/floor owners.
// A cold-floor bad_alloc refuses only that proof and is separately counted;
// optional-cache allocation refusal may still complete its usual cold fallback.
// The final private argument is solely for synthetic exception-path testing.
[[nodiscard]] bool american_implied_vol_price_seed_batch(
    std::span<const IvPriceSeedRequest> requests, const IvOpts &opts,
    IvPriceSeedKernel kernel, std::span<IvPriceSeedResult> out,
    IvPriceSeedBatchStats &stats, bool timed = false,
    const research::IvSeedColdFloorTestHook *test_hook = nullptr);

namespace simd::detail {
struct IvSeedVectorPrice {
  bool solved = false;
  double price = std::numeric_limits<double>::quiet_NaN();
  double boundary = std::numeric_limits<double>::quiet_NaN();
  AlExerciseClass exercise = AlExerciseClass::UnsupportedRegime;
};
// Exactly four already screened requests; caller checks runtime AVX2 dispatch.
// No scalar patches here: the caller retains their explicit origin and reason.
void iv_seed_reference_pack_avx2(std::span<const IvPriceSeedRequest, 4> requests,
    const std::optional<AlOpts> &opts, std::span<IvSeedVectorPrice, 4> out) noexcept;
} // namespace simd::detail
} // namespace atx::vol
