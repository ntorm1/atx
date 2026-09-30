// atx::engine::book — desired-target shaping kernels (platform v8). Contracts live in
// book/target_shaping.hpp.

#include "atx/engine/book/target_shaping.hpp"

#include <cmath>   // std::abs, std::isfinite
#include <limits>  // std::numeric_limits
#include <span>    // std::span
#include <string>  // std::string
#include <utility> // std::move (ATX_TRY_VOID)

#include "atx/core/error.hpp"
#include "atx/core/types.hpp"

namespace atx::engine::book {

using atx::f64;
using atx::u8;
using atx::usize;

namespace {

[[nodiscard]] atx::core::Status invalid(const char *what) {
  return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                        std::string("apply_hold_band: ") + what);
}

// The checks of apply_hold_band, before anything is modified.
[[nodiscard]] atx::core::Status check_hold_band(std::span<const f64> rank_now,
                                                std::span<const f64> desired,
                                                std::span<const u8> eligible, f64 band,
                                                const HoldBandState &state) {
  const usize n = rank_now.size();
  if (desired.size() != n || eligible.size() != n) {
    return invalid("rank_now, desired and eligible differ in width");
  }
  const bool sized = !state.rank_set.empty() || !state.desired_prev.empty();
  if (sized && (state.rank_set.size() != n || state.desired_prev.size() != n)) {
    return invalid("state width differs from the row width");
  }
  if (!std::isfinite(band) || band < 0.0) {
    return invalid("band must be finite and >= 0");
  }
  for (usize i = 0U; i < n; ++i) {
    if (eligible[i] != 0U && (!std::isfinite(rank_now[i]) || !std::isfinite(desired[i]))) {
      return invalid("an eligible name has a non-finite rank or desired value");
    }
    if (sized && std::isfinite(state.rank_set[i]) && !std::isfinite(state.desired_prev[i])) {
      return invalid("a set rank has a non-finite desired_prev");
    }
  }
  return atx::core::Ok();
}

} // namespace

atx::core::Result<HoldBandCounts> apply_hold_band(std::span<const f64> rank_now,
                                                  std::span<f64> desired,
                                                  std::span<const u8> eligible, f64 band,
                                                  HoldBandState &state) {
  ATX_TRY_VOID(check_hold_band(rank_now, desired, eligible, band, state));
  const usize n = rank_now.size();
  if (state.rank_set.empty()) {
    state.rank_set.assign(n, std::numeric_limits<f64>::quiet_NaN());
    state.desired_prev.assign(n, std::numeric_limits<f64>::quiet_NaN());
  }
  HoldBandCounts counts;
  for (usize i = 0U; i < n; ++i) {
    if (eligible[i] == 0U) {
      continue;
    }
    const f64 now = rank_now[i];
    const f64 fresh = desired[i];
    const f64 set = state.rank_set[i];
    const bool unset = !std::isfinite(set);
    if (unset || std::abs(now - set) > band) {
      state.rank_set[i] = now;
      state.desired_prev[i] = fresh;
      ++counts.moved;
      counts.first_set += unset ? 1U : 0U;
    } else {
      desired[i] = state.desired_prev[i];
      ++counts.kept;
    }
  }
  return atx::core::Ok(counts);
}

} // namespace atx::engine::book
