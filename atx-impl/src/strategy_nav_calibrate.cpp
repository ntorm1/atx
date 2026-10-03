#include "strategy_nav_calibrate.hpp"

#include <array>
#include <charconv>
#include <cmath>
#include <system_error>
#include <nlohmann/json.hpp>

namespace atx::impl::strategy {
namespace {
using namespace atx;
namespace co = atx::core;
using Json = nlohmann::json;

// numpy's PW_BLOCKSIZE and unroll factor (numpy/core/src/umath/loops_utils.h.src, 1.26).
constexpr usize pairwise_block = 128, pairwise_unroll = 8;

constexpr const char* calibration_rule =
    "P9 C2, Ruling PM6-6 in one process: pass 1 replays the primary S2 book alone at "
    "leverage_requested; gross_requested = the numpy mean of its daily gross_leverage over every "
    "row (nav_summ mean_gross_leverage_all_rows); |gross_requested - target| <= tolerance keeps "
    "the leverage, else ONE correction leverage = leverage_requested x target / gross_requested "
    "written with 4 decimals (refused outside [1, 2]); the published run is the plain run at "
    "leverage (recipe.json and every other file its bytes), gross its S2 book's all-rows mean "
    "and matched = |gross - target| <= tolerance (false: the match stage stops); "
    "construction_reused: pass 2 replayed pass 1's shared construction (provenance only)";

// numpy's pairwise summation of a[0, n) (loops_utils.h.src @TYPE@_pairwise_sum, unit stride).
// SAFETY (bounded recursion): each split halves n above pairwise_block, so the depth is at most
// log2(n / 128) + 1 (<= 64 for any usize n).
f64 pairwise_sum(const f64* a, usize n) noexcept {
  if (n < pairwise_unroll) {
    f64 res = 0.0;
    for (usize i = 0; i < n; ++i) res += a[i];
    return res;
  }
  if (n <= pairwise_block) {
    std::array<f64, pairwise_unroll> r{};
    for (usize j = 0; j < pairwise_unroll; ++j) r[j] = a[j];
    usize i = pairwise_unroll;
    for (; i < n - (n % pairwise_unroll); i += pairwise_unroll)
      for (usize j = 0; j < pairwise_unroll; ++j) r[j] += a[i + j];
    f64 res = ((r[0] + r[1]) + (r[2] + r[3])) + ((r[4] + r[5]) + (r[6] + r[7]));
    for (; i < n; ++i) res += a[i];
    return res;
  }
  usize n2 = n / 2;
  n2 -= n2 % pairwise_unroll;
  return pairwise_sum(a, n2) + pairwise_sum(a + n2, n - n2);
}
Json finite_or_null(f64 x) { return std::isfinite(x) ? Json(x) : Json(nullptr); }
} // namespace

co::Status validate_calibrate(const NavCalibrateOptions& options) {
  if (!options.on()) return co::Ok();
  if (!std::isfinite(options.target_gross) || !(options.target_gross > 0))
    return co::Err(co::ErrorCode::InvalidArgument,
                   "nav calibrate: --calibrate-gross must be finite and > 0");
  if (!std::isfinite(options.tolerance) || !(options.tolerance > 0))
    return co::Err(co::ErrorCode::InvalidArgument,
                   "nav calibrate: --calibrate-tolerance must be finite and > 0");
  return co::Ok();
}

f64 numpy_mean(std::span<const f64> values) noexcept {
  if (values.empty()) return std::numeric_limits<f64>::quiet_NaN();
  // np.add.reduce starts from the identity 0.0 and adds the pairwise sum of every value.
  const f64 sum = 0.0 + pairwise_sum(values.data(), values.size());
  return sum / static_cast<f64>(values.size());
}

co::Result<f64> leverage_4_decimals(f64 x) {
  if (!std::isfinite(x))
    return co::Err(co::ErrorCode::OutOfRange, "nav calibrate: the corrected leverage is not finite");
  std::array<char, 64> text{};
  const auto printed =
      std::to_chars(text.data(), text.data() + text.size(), x, std::chars_format::fixed, 4);
  if (printed.ec != std::errc{})
    return co::Err(co::ErrorCode::OutOfRange, "nav calibrate: the corrected leverage is too large");
  f64 parsed = 0.0;
  const auto read = std::from_chars(text.data(), printed.ptr, parsed);
  if (read.ec != std::errc{} || read.ptr != printed.ptr)
    return co::Err(co::ErrorCode::Internal, "nav calibrate: the 4-decimal leverage did not parse");
  return co::Ok(parsed);
}

co::Result<NavCalibrateStep> calibrate_step(f64 leverage, f64 target, f64 gross, f64 tolerance) {
  if (!std::isfinite(gross) || !(gross > 0))
    return co::Err(co::ErrorCode::InvalidArgument,
                   "nav calibrate: the calibration pass's all-rows gross must be finite and > 0 "
                   "(got " + std::to_string(gross) + ")");
  if (std::abs(gross - target) <= tolerance) return co::Ok(NavCalibrateStep{false, leverage});
  ATX_TRY(const f64 corrected, leverage_4_decimals(leverage * target / gross));
  if (!(corrected >= nav_calibrate_leverage_min && corrected <= nav_calibrate_leverage_max))
    return co::Err(co::ErrorCode::InvalidArgument,
                   "nav calibrate: the corrected leverage " + std::to_string(corrected) +
                       " (L x G_P / G) leaves the leverage range [1, 2]; nothing published");
  return co::Ok(NavCalibrateStep{true, corrected});
}

bool calibration_matched(const NavCalibration& c) noexcept {
  return std::abs(c.gross - c.options.target_gross) <= c.options.tolerance;
}

std::string calibration_summary_json(const NavCalibration& c) {
  const Json record{{"target", c.options.target_gross},
                    {"tolerance", c.options.tolerance},
                    {"leverage_requested", c.leverage_requested},
                    {"gross_requested", finite_or_null(c.gross_requested)},
                    {"corrected", c.step.corrected},
                    {"leverage", c.step.leverage},
                    {"gross", finite_or_null(c.gross)},
                    {"matched", calibration_matched(c)},
                    {"construction_reused", c.construction_reused},
                    {"rule", calibration_rule}};
  return record.dump();
}

} // namespace atx::impl::strategy
