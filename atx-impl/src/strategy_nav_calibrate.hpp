#pragma once

// nav --calibrate-gross G_P [--calibrate-tolerance T] (P9 C2): Ruling PM6-6's gross matching in
// one process, instead of the two NAV runs of the wave match stage.
//
// G(L), the all-rows mean gross leverage of the primary S2 book at aim leverage L, is not linear
// in L (the dust band, the exits and the ADV cap bend it; nav review section 5), so PM6-6 makes
// ONE correction and then checks, never a solve:
//   pass 1: the S2 book alone at L (no other or capacity book); G = numpy_mean of its daily
//           gross_leverage over every row (nav_summ mean_gross_leverage_all_rows, bit for bit);
//   |G - G_P| <= T: L stays; else L' = L x G_P / G written with 4 decimals (Python's
//           f"{x:.4f}", as the -gm spec carries it) and parsed back; L' outside [1, 2] (the
//           aim-partial-v5 leverage range) is refused before anything is published;
//   pass 2: the full run at the final L, published as the plain run at that L publishes it, plus
//           summary.json calibration {...} (matched = |G' - G_P| <= T for pass 2's S2 book; a
//           pass outside the tolerance still publishes, with matched false: the match stage stops).
// One load serves both passes (fields, liquidity, combined signal). Where the shared
// construction reads nothing that differs between the passes, pass 2 replays pass 1's recorded
// construction instead of forming it again (strategy_nav_replay.cpp ConstructionTape).

#include <cmath>
#include <limits>
#include <span>
#include <string>

#include "atx/core/error.hpp"
#include "atx/core/types.hpp"
#include "strategy_target_replay.hpp" // aim_leverage_min / aim_leverage_max

namespace atx::impl::strategy {

inline constexpr atx::f64 nav_calibrate_default_tolerance = 0.005; // Ruling PM6-6
// The aim-partial-v5 leverage range (validate_target_config's, one constant): a correction outside
// is refused.
inline constexpr atx::f64 nav_calibrate_leverage_min = aim_leverage_min;
inline constexpr atx::f64 nav_calibrate_leverage_max = aim_leverage_max;

// CLI --calibrate-gross G_P (finite > 0) --calibrate-tolerance T (finite > 0, default .005).
// target_gross NaN: off (the default; the run is the plain run, every byte unchanged).
struct NavCalibrateOptions {
  atx::f64 target_gross{std::numeric_limits<atx::f64>::quiet_NaN()};
  atx::f64 tolerance{nav_calibrate_default_tolerance};
  [[nodiscard]] bool on() const noexcept { return !std::isnan(target_gross); }
};
// Off, or G_P and T finite and > 0; else InvalidArgument.
[[nodiscard]] atx::core::Status validate_calibrate(const NavCalibrateOptions& options);

// numpy 1.26's contiguous float64 ndarray.mean(): 0.0, then += the pairwise sum (8 accumulators
// up to 128 values, split at n/2 rounded down to a multiple of 8 above) of each 8192-value block
// (the ufunc buffer) in order, divided by n. NaN when empty. Bit for bit what
// nav_summ.construction_stats computes from a daily CSV (17 significant digits, so every value
// round-trips), at every n (Ruling C2-R3; numpy-generated fixture in the gtests).
[[nodiscard]] atx::f64 numpy_mean(std::span<const atx::f64> values) noexcept;

// x written as Python f"{x:.4f}" (correctly rounded, ties to even) and parsed back; OutOfRange
// when x is not finite or does not fit the buffer.
[[nodiscard]] atx::core::Result<atx::f64> leverage_4_decimals(atx::f64 x);

// PM6-6's decision from pass 1: corrected = |gross - target| > tolerance; leverage = the final L.
struct NavCalibrateStep {
  bool corrected{};
  atx::f64 leverage{};
};
// InvalidArgument: gross not finite and > 0, or the corrected L' outside [1, 2].
[[nodiscard]] atx::core::Result<NavCalibrateStep> calibrate_step(atx::f64 leverage,
                                                                 atx::f64 target,
                                                                 atx::f64 gross,
                                                                 atx::f64 tolerance);

// What one calibrated run did: its options, pass 1's L and G, the step, pass 2's G and whether
// pass 2 replayed pass 1's construction (provenance only: the published bytes are the same).
struct NavCalibration {
  NavCalibrateOptions options{};
  atx::f64 leverage_requested{}, gross_requested{};
  NavCalibrateStep step{};
  atx::f64 gross{};
  bool construction_reused{};
};
[[nodiscard]] bool calibration_matched(const NavCalibration& c) noexcept;
// summary.json's calibration record (a JSON object text): target, tolerance,
// leverage_requested, gross_requested, corrected, leverage, gross, matched,
// construction_reused, rule.
[[nodiscard]] std::string calibration_summary_json(const NavCalibration& c);

} // namespace atx::impl::strategy
