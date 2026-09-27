#pragma once

#include <memory>
#include <span>
#include <string>
#include <vector>
#include "atx/core/error.hpp"
#include "atx/core/types.hpp"

namespace atx::impl::strategy {
struct IcCompositionCandidate {
  std::string id, family;
};
struct IcCompositionConfig {
  atx::usize dates{}, instruments{}, decision_begin{}, decision_end{};
  atx::usize cadence{5};
  atx::f64 trade_fraction{0.25};
  atx::u64 max_working_bytes{256ULL << 20};
};
struct IcCompositionResult {
  std::vector<atx::f64> signal; // date-major; nonmembers NaN, missing contributions zero
  std::vector<atx::f64> planned_turnover, contribution_fraction;
  std::vector<atx::f64> planned_gross, planned_net;
  std::vector<atx::usize> eligible_names;
  atx::f64 total_planned_turnover{}, deployment_turnover{};
  atx::usize deployment_date{}; // dates if there was no nonzero deployment
};
// Conservative owned allocation envelope, including result + scratch and bounded
// candidate strings. Excludes caller Panel, VM, labels and incoming signal.
[[nodiscard]] atx::core::Result<atx::u64> ic_composition_working_bytes(
    atx::usize dates, atx::usize instruments, atx::usize candidates);

// Streaming equal-family/equal-within-family centered tied-rank composition.
// create copies membership and candidate metadata. add borrows one signal only
// for that call and requires library order exactly once, including sign==0.
// Per-add sign permits TRAIN evaluate -> orient -> add -> discard in one VM
// pass. Validation supplies those same frozen signs; this helper never fits.
// No return labels or validation evidence enter this helper.
// Optional pinned_weights (library order, finite, >= 0) replace the equal
// family/within-family weights value-for-value: no normalization and the same
// accumulation expression, so pinning the default values reproduces its bits.
// A zero weight contributes nothing; nothing is ever redistributed.
class IcComposition {
 public:
  ~IcComposition();
  IcComposition(IcComposition&&) noexcept;
  IcComposition& operator=(IcComposition&&) noexcept;
  IcComposition(const IcComposition&) = delete;
  IcComposition& operator=(const IcComposition&) = delete;
  [[nodiscard]] static atx::core::Result<IcComposition> create(
      const IcCompositionConfig&, std::span<const IcCompositionCandidate>,
      std::span<const atx::u8> decision_member,
      std::span<const atx::f64> pinned_weights = {}); // empty: equal family/within
  [[nodiscard]] atx::core::Status add(atx::usize candidate_index,
                                    std::span<const atx::f64> signal,
                                    int frozen_sign); // -1/+1; 0 neutral, never redistributed
  // Planned target proxy only: tied ranks, dollar-neutral desired gross1, no
  // winsorization. Cadence is anchored to decision_begin; interpolate .25 by
  // default from prior planned weights, with no price drift. Membership exits
  // force zero even off cadence; survivors are NOT renormalized. Consequently
  // planned gross/net are reported, not promised to stay1/0 between rebalances.
  // Initial deployment is included in both daily and total turnover.
  [[nodiscard]] atx::core::Result<IcCompositionResult> finish();
 private:
  struct Impl;
  explicit IcComposition(std::unique_ptr<Impl>);
  std::unique_ptr<Impl> impl_;
};
} // namespace atx::impl::strategy
