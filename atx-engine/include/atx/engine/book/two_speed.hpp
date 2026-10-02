#pragma once

// atx::engine::book -- two-speed netted sleeves (platform v8 Y, lane YCOMB, rule Y-5, Ruling PM8-5).
//
// PM8-5 asked for a multi-horizon combination with a multi-period optimiser (Garleanu and Pedersen
// 2013; Grinold 2010; Qian, Sorensen and Hua 2007; Boyd et al. 2017). The registered R-3 already was
// Garleanu-Pedersen's per-signal term structure in the aim: its gain g_k = theta * sum_j (1 - theta)^j
// rho_k(j) is, for an AR(1) signal with per-session decay phi_k, theta / (theta + phi_k - theta phi_k)
// ~ 1 / (1 + phi_k / theta), GP's aim weight B_k / (1 + phi_k a / gamma) with a / gamma = 1 / theta.
// R-3 was not accepted (N 43, dSR -.0118). This kernel is the nearest form that is not R-3: the
// signal-specific decay sets how fast each horizon bucket is TRADED, not how much of it is aimed at.
// The blend splits into a fast sleeve (themes whose registered alpha half-life is at most
// two_speed_fast_bound sessions) and a slow sleeve (the rest), each with the parent's per-date
// standardisation and theme masses; each sleeve keeps its own holdings and moves toward its own
// aim (L x the sleeve's desired target) by partial adjustment at its own rate; the book holds the
// sum and trades only the net change, so opposite sleeve trades cancel before they cost anything.
//   slow: theta_s = two_speed_slow_theta (the parent's aim-partial-v5 theta; the slow sleeve trades
//         as the whole book does today);
//   fast: theta_f = 1 - 2^(-1 / two_speed_fast_half_life): the fast sleeve's aim gap closes at the
//         rate its alpha decays (half-life matched), so it is held while it still forecasts.
// Not R-4 (a band on the whole book's rank), not R-9 (one book-level theta), not R-3 (one aim, one
// theta, per-signal weights): two aims, two rates, one netted trade.
//
// Every constant is registered blind (task-YCOMB-report.md, per-theme half-life table). The kernel
// is pure; it reads no return.

#include <cmath>
#include <initializer_list>
#include <span>

#include "atx/core/error.hpp"
#include "atx/core/types.hpp"

namespace atx::engine::book {

inline constexpr atx::f64 two_speed_slow_theta = 0.05;      // aim-partial-v5's registered theta
inline constexpr atx::f64 two_speed_fast_half_life = 5.0;   // sessions: reversal_seasonality, price_volume
inline constexpr atx::f64 two_speed_fast_bound = 10.0;      // a theme with half-life <= 10 sessions is fast

// theta_f = 1 - 2^(-1 / h_f): after h_f sessions half of an aim gap is left, as half of the alpha is.
[[nodiscard]] inline atx::f64 two_speed_fast_theta() noexcept {
  return 1.0 - std::exp2(-1.0 / two_speed_fast_half_life);
}

struct TwoSpeedTrade {
  atx::f64 sleeve_trade{}; // sum_i |d fast_i| + |d slow_i|: what two separate books would trade
  atx::f64 net_trade{};    // sum_i |d (fast_i + slow_i)|: what the netted book trades
};

// One rebalance. Per name i: fast_i += theta_fast (aim_fast_i - fast_i), slow_i += theta_slow
// (aim_slow_i - slow_i), book_i = fast_i + slow_i; net_trade is measured against the carried book_i
// (the previous step's fast_i + slow_i; 0 before deployment). Every span has one entry per name; aims
// and the carried sleeves finite; 0 < theta <= 1. Err(InvalidArgument) before any write otherwise.
[[nodiscard]] inline atx::core::Result<TwoSpeedTrade> two_speed_step(std::span<const atx::f64> aim_fast,
                                                                     std::span<const atx::f64> aim_slow,
                                                                     atx::f64 theta_fast, atx::f64 theta_slow,
                                                                     std::span<atx::f64> fast,
                                                                     std::span<atx::f64> slow,
                                                                     std::span<atx::f64> book) {
  const auto n = aim_fast.size();
  if (aim_slow.size() != n || fast.size() != n || slow.size() != n || book.size() != n)
    return atx::core::Err(atx::core::ErrorCode::InvalidArgument, "two-speed: one entry per name in every span");
  for (const atx::f64 theta : {theta_fast, theta_slow})
    if (!std::isfinite(theta) || !(theta > 0.0) || theta > 1.0)
      return atx::core::Err(atx::core::ErrorCode::InvalidArgument, "two-speed: theta must lie in (0, 1]");
  for (atx::usize i = 0; i < n; ++i)
    if (!std::isfinite(aim_fast[i]) || !std::isfinite(aim_slow[i]) || !std::isfinite(fast[i]) ||
        !std::isfinite(slow[i]))
      return atx::core::Err(atx::core::ErrorCode::InvalidArgument, "two-speed: aims and sleeves must be finite");
  TwoSpeedTrade out;
  for (atx::usize i = 0; i < n; ++i) {
    const atx::f64 d_fast = theta_fast * (aim_fast[i] - fast[i]);
    const atx::f64 d_slow = theta_slow * (aim_slow[i] - slow[i]);
    fast[i] += d_fast;
    slow[i] += d_slow;
    const atx::f64 next = fast[i] + slow[i];
    out.sleeve_trade += std::abs(d_fast) + std::abs(d_slow);
    out.net_trade += std::abs(next - book[i]);
    book[i] = next;
  }
  return atx::core::Ok(out);
}

} // namespace atx::engine::book
