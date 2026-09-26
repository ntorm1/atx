#pragma once

#include <array>
#include <memory>
#include <span>
#include <string_view>

#include "atx/core/error.hpp"
#include "atx/core/types.hpp"

namespace atx::engine::alpha { class Panel; }

namespace atx::engine::factory {

enum class IcScreenRule : atx::u8 { DisabledV1 = 1, ConservativeV2 = 2 };

struct IcScreenConfig {
  IcScreenRule rule{IcScreenRule::DisabledV1};
  std::array<atx::usize, 4> horizons{{5, 21, 63, 126}};
  atx::usize execution_delay{1};
  atx::usize window_begin{};
  atx::usize window_end{};   // exclusive; zero resolves to panel.dates()
  atx::usize maturity_end{}; // exclusive; zero resolves to window_end
  atx::usize min_names{20};
  atx::usize min_dates{128};
  atx::f64 practical_abs_ic{0.02}; // screening tolerance, NOT an alpha admission gate
  atx::f64 confidence_multiplier{3.5};
  atx::u64 max_cache_bytes{atx::u64{512} * 1024U * 1024U};
};

enum class IcScreenReason : atx::u8 {
  Disabled, InsufficientEvidence, PossibleAlpha, PracticalNull
};

struct IcScreenEstimate {
  atx::usize valid_dates{};
  atx::usize calendar_dates{};
  atx::usize hac_lag{};
  atx::f64 mean{};
  atx::f64 standard_error{};
  atx::f64 upper_abs_ic{1.0};
  atx::f64 max_segment_abs_ic{};
  bool defined{};
  bool suggestive_direction{};
};

struct IcScreenHorizon {
  atx::usize horizon{};
  IcScreenEstimate pearson;
  IcScreenEstimate rank;
  bool enough_evidence{};
};

struct IcScreenResult {
  bool reject{};
  bool enough_evidence{};
  IcScreenReason reason{IcScreenReason::Disabled};
  std::array<IcScreenHorizon, 4> horizons;
};

namespace ic_screen_detail { struct Cache; struct Scratch; }
class IcScreenScratch;

// One immutable owned cache per training run. Copies share read-only storage;
// neither the source Panel nor the optional masks need outlive preparation.
class IcScreenCache {
public:
  IcScreenCache() = default;
  [[nodiscard]] atx::usize dates() const noexcept;
  [[nodiscard]] atx::usize instruments() const noexcept;
  [[nodiscard]] atx::usize first_date() const noexcept;
  [[nodiscard]] atx::usize cached_dates() const noexcept;
  [[nodiscard]] atx::u64 bytes() const noexcept;
  // Horizon-major, then date-major; first row is first_date(). Missing = NaN.
  [[nodiscard]] std::span<const atx::f64> returns(atx::usize horizon_index) const noexcept;
  [[nodiscard]] std::span<const atx::f64> return_ranks(atx::usize horizon_index) const noexcept;
private:
  std::shared_ptr<const ic_screen_detail::Cache> data_;
  friend atx::core::Result<IcScreenCache> prepare_ic_screen(
      const alpha::Panel&, const IcScreenConfig&, std::span<const atx::u8>,
      std::span<const atx::u32>, std::string_view);
  friend atx::core::Result<IcScreenScratch> prepare_ic_screen_scratch(const IcScreenCache&);
  friend atx::core::Result<IcScreenResult> screen_ic(
      std::span<const atx::f64>, const IcScreenCache&, IcScreenScratch&);
};

// One preallocated scratch per worker; never share between concurrent calls.
// No allocation on a successful screen_ic call. This keeps heavy SIMD/ranking
// implementation and its dependencies out of SearchDriver's public header.
class IcScreenScratch {
public:
  IcScreenScratch();
  ~IcScreenScratch();
  IcScreenScratch(IcScreenScratch&&) noexcept;
  IcScreenScratch& operator=(IcScreenScratch&&) noexcept;
  IcScreenScratch(const IcScreenScratch&) = delete;
  IcScreenScratch& operator=(const IcScreenScratch&) = delete;
  [[nodiscard]] std::span<const atx::f64> pearson_series(atx::usize horizon_index) const noexcept;
  [[nodiscard]] std::span<const atx::f64> rank_series(atx::usize horizon_index) const noexcept;
private:
  std::unique_ptr<ic_screen_detail::Scratch> data_;
  friend atx::core::Result<IcScreenScratch> prepare_ic_screen_scratch(const IcScreenCache&);
  friend atx::core::Result<IcScreenResult> screen_ic(
      std::span<const atx::f64>, const IcScreenCache&, IcScreenScratch&);
};

// Signal dates are in [window_begin, window_end). A label uses close[d+delay+h]
// /close[d+delay]-1 only when its endpoint < min(window_end,maturity_end).
// Membership is checked at d, NEVER at the future entry/exit. The optional
// binary membership further intersects Panel eligibility. bad_return_prefix
// is date-major cumulative excluded one-day returns; an interval is excluded
// when the endpoint/entry counts differ. Both optional spans are full Panel size.
// Price must be a declared total-return-compatible close supplied by the caller.
// Shapes/budgets are checked before allocation; exceptional allocation may throw.
[[nodiscard]] atx::core::Result<IcScreenCache> prepare_ic_screen(
    const alpha::Panel& panel, const IcScreenConfig& config,
    std::span<const atx::u8> decision_membership = {},
    std::span<const atx::u32> bad_return_prefix = {},
    std::string_view price_field = "close");
[[nodiscard]] atx::core::Result<IcScreenScratch>
prepare_ic_screen_scratch(const IcScreenCache& cache);

// Conservative equivalence screen, not significance/alpha admission. Reject only
// if EVERY horizon's Pearson AND tied-rank bounds exclude a practical effect in
// BOTH signs, with adequate coverage and no weak directional/regime evidence.
// Bartlett HAC uses calendar-preserving missing-data influence series and a lag
// >=2*h; inference is approximate. Undefined/short/sparse evidence passes.
[[nodiscard]] atx::core::Result<IcScreenResult> screen_ic(
    std::span<const atx::f64> signal, const IcScreenCache& cache, IcScreenScratch& scratch);
[[nodiscard]] std::string_view ic_screen_reason_name(IcScreenReason reason) noexcept;
[[nodiscard]] std::string_view ic_screen_rule_name(IcScreenRule rule) noexcept;
[[nodiscard]] atx::usize ic_screen_simd_width() noexcept;

} // namespace atx::engine::factory
