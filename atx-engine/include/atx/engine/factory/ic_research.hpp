#pragma once
#include "atx/engine/factory/ic_screen.hpp"
namespace atx::engine::parallel { class DetPool; }
namespace atx::engine::factory {
// Explicit research IC worker bound (platform v8 B-2: 4 -> 16). Rows are per-date
// independent, so every count in 1..max gives the same bits.
inline constexpr atx::usize max_research_ic_workers = 16;
// Separate research boundary: old IcScreenConfig/API/layout remain unchanged.
struct ResearchIcOptions {
  atx::usize active_horizons{3}; // active ordered prefix of config.horizons, 1..4
  bool require_endpoint_presence{true}; // source observation, NOT future membership
  atx::usize workers{1}; // 1..max_research_ic_workers; row scratch only, no extra dense caches
};
struct ResearchIcCoverage {
  atx::usize mature_dates{}, structural_tail_dates{};
  atx::u64 decision_eligible_pairs{}, finite_label_pairs{}, paired_signal_pairs{};
  // Entry/exit counts overlap when both endpoints are absent. Other exclusions
  // are counted after presence admission; never sum these as a partition.
  atx::u64 missing_entry_pairs{}, missing_exit_pairs{}, guard_excluded_pairs{};
  atx::u64 invalid_price_pairs{}, nonfinite_return_pairs{};
};
struct ResearchIcResult {
  IcScreenResult screen;
  std::array<ResearchIcCoverage,4> coverage{};
  atx::usize active_horizons{};
};
class ResearchIcScratch;
class ResearchIcCache {
public:
  ResearchIcCache()=default;
  [[nodiscard]] atx::usize dates() const noexcept;
  [[nodiscard]] atx::usize instruments() const noexcept;
  [[nodiscard]] atx::usize first_date() const noexcept;
  [[nodiscard]] atx::u64 bytes() const noexcept;
private:
  std::shared_ptr<const ic_screen_detail::Cache> data_;
  friend atx::core::Result<ResearchIcCache> prepare_research_ic(const alpha::Panel&,
      const IcScreenConfig&,const ResearchIcOptions&,std::span<const atx::u8>,
      std::span<const atx::u32>,std::string_view);
  friend atx::core::Result<ResearchIcScratch> prepare_research_ic_scratch(const ResearchIcCache&);
  friend atx::core::Result<ResearchIcResult> evaluate_research_ic(
      std::span<const atx::f64>,const ResearchIcCache&,ResearchIcScratch&,parallel::DetPool*);
};
class ResearchIcScratch {
public:
  ResearchIcScratch(); ~ResearchIcScratch();
  ResearchIcScratch(ResearchIcScratch&&) noexcept;
  ResearchIcScratch& operator=(ResearchIcScratch&&) noexcept;
  ResearchIcScratch(const ResearchIcScratch&)=delete;
  ResearchIcScratch& operator=(const ResearchIcScratch&)=delete;
  [[nodiscard]] atx::u64 bytes() const noexcept;
  [[nodiscard]] std::span<const atx::f64> pearson_series(atx::usize horizon_index) const noexcept;
  [[nodiscard]] std::span<const atx::f64> rank_series(atx::usize horizon_index) const noexcept;
private:
  std::unique_ptr<ic_screen_detail::Scratch> data_;
  friend atx::core::Result<ResearchIcScratch> prepare_research_ic_scratch(const ResearchIcCache&);
  friend atx::core::Result<ResearchIcResult> evaluate_research_ic(
      std::span<const atx::f64>,const ResearchIcCache&,ResearchIcScratch&,parallel::DetPool*);
};
// Same vectorized correlations, exact pairwise tied ranks, calendar-preserving
// overlap-aware HAC and conservative equivalence rule as the legacy kernel.
// Strict endpoints are physically present positive finite prices; only decision
// d membership affects formation. Unmatured labels are absent, never zero.
// Inactive slots have zero horizon and undefined estimates, never evidence.
// Cache owns inputs; scratch is pointer-bound to exactly that cache/options.
// Caller must bind external source/mask/guard/options identity into its recipe.
// workers>1 requires a borrowed pool with exactly that count. The caller must
// invoke evaluation outside that pool's jobs (no nested dispatch), keep the pool
// alive until return, and exclusively own scratch for the call. It may reuse a
// VM pool sequentially. workers==1 uses nullptr and preserves the serial path.
// Each worker owns O(instruments) row buffers. Output calendar series and the
// immutable dense labels remain single copies; ordered HAC runs after joining.
[[nodiscard]] atx::core::Result<ResearchIcCache> prepare_research_ic(
    const alpha::Panel&,const IcScreenConfig&,const ResearchIcOptions&,
    std::span<const atx::u8> decision_membership={},
    std::span<const atx::u32> bad_return_prefix={},std::string_view price_field="close");
[[nodiscard]] atx::core::Result<ResearchIcScratch> prepare_research_ic_scratch(const ResearchIcCache&);
[[nodiscard]] atx::core::Result<ResearchIcResult> evaluate_research_ic(
    std::span<const atx::f64>,const ResearchIcCache&,ResearchIcScratch&,
    parallel::DetPool* pool=nullptr);
} // namespace atx::engine::factory
