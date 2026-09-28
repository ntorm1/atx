#pragma once

#include <memory>
#include <span>
#include <string>
#include <vector>
#include "atx/core/error.hpp"
#include "atx/core/types.hpp"

namespace atx::engine::parallel { class DetPool; }
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
  std::vector<atx::f64> signal; // date-major; nonmembers NaN, missing contributions zero (pinned themes: see create)
  std::vector<atx::f64> planned_turnover, contribution_fraction;
  std::vector<atx::f64> planned_gross, planned_net;
  std::vector<atx::usize> eligible_names;
  atx::f64 total_planned_turnover{}, deployment_turnover{};
  atx::usize deployment_date{}; // dates if there was no nonzero deployment
};
// Conservative owned allocation envelope, including result + scratch and bounded
// candidate strings. Excludes caller Panel, VM, labels and incoming signal.
// `themes` > 0 (pinned within-theme redistribution, at most 32) adds two f64
// planes per theme; 0 is the unchanged envelope.
[[nodiscard]] atx::core::Result<atx::u64> ic_composition_working_bytes(
    atx::usize dates, atx::usize instruments, atx::usize candidates, atx::usize themes = 0);

// Streaming equal-family/equal-within-family centered tied-rank composition.
// create copies membership and candidate metadata. add borrows one signal only
// for that call and requires library order exactly once, including sign==0.
// Per-add sign permits TRAIN evaluate -> orient -> add -> discard in one VM
// pass. Validation supplies those same frozen signs; this helper never fits.
// No return labels or validation evidence enter this helper.
// Optional pinned_weights (library order, finite, >= 0) replace the equal
// family/within-family weights value-for-value: no normalization and the same
// accumulation expression, so pinning the default values reproduces its bits.
// A zero weight contributes nothing; without pinned_themes nothing is ever
// redistributed.
// Optional pinned_themes (library order, one theme index per candidate; needs
// pinned_weights; indices of zero-weight candidates are ignored; each < 32) switch
// on within-theme coverage redistribution (fitter composition ew-theme-v6, rule
// within-theme-v1): per name and date a theme adds
//   W_theme * sum_{k present} w_k s_k r_k / sum_{k present} w_k,
// W_theme = the sum of its pinned weights, so a member missing for that name keeps
// its mass inside its theme and a theme with no present member adds nothing.
// Present = a member name with a finite signal on a date where the candidate ranks
// >= 2 names, added with a nonzero sign. Empty (default): the path above, bit for bit.
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
      std::span<const atx::f64> pinned_weights = {}, // empty: equal family/within
      std::span<const atx::usize> pinned_themes = {}); // empty: no redistribution
  // Optional `pool` (borrowed; alive for the call, never invoked from inside one
  // of its jobs): dates split into contiguous bands, each ranked in a per-worker
  // row. Every blend cell and per-date coverage sum is written by exactly one
  // band with the serial expression, and candidates still arrive in library
  // order, so the result bits equal the serial path by construction. Worker rows
  // (16 B/name each) are allocated on the first pooled add.
  [[nodiscard]] atx::core::Status add(atx::usize candidate_index,
                                    std::span<const atx::f64> signal,
                                    int frozen_sign, // -1/+1; 0 neutral (pinned themes: mass stays in theme)
                                    atx::engine::parallel::DetPool* pool = nullptr);
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
