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
//   fast: theta_f = 1 - 2^(-C / two_speed_fast_half_life) per rebalance at cadence C sessions
//         (Ruling PM8-16 #4): the fast sleeve's aim gap closes at the rate its alpha decays
//         (half-life matched, 5 sessions at any cadence), so it is held while it still forecasts.
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

// theta_f = 1 - 2^(-C / h_f), C the rebalance cadence in sessions (one step per rebalance): after
// h_f sessions half of an aim gap is left, as half of the alpha is, at any cadence (C = 1: .1294;
// C = 5: .5). C >= 1 (the replay's cadence is 1..4096).
[[nodiscard]] inline atx::f64 two_speed_fast_theta(atx::f64 cadence) noexcept {
  return 1.0 - std::exp2(-cadence / two_speed_fast_half_life);
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

// The netted aim of one rebalance decision (two-speed-v1's NAV construction; the replay holds one
// book per scenario and plans it with aim-partial-v5, current + theta_slow (L desired - current)).
// On entry `desired` holds the slow sleeve's desired target d_s and `fast` the virtual fast sleeve F
// (book-independent: no drift, no fills); per name with member[i]:
//   F_next = F + theta_fast (L m_f d_f - F)
//   desired = (1 - m_f) d_s + (F + (F_next - F) / theta_slow) / L,
// so that aim-partial-v5's step from any current weight c is F_next - F plus the remainder's
// (c - F) step toward L (1 - m_f) d_s: the fast and slow sleeves' moves, netted before trading. A
// nonmember's F and desired become 0. m_f = fast_share in [0, 1], L > 0, thetas in (0, 1], member
// flags 0/1, every span one entry per name and the members' entries finite; Err(InvalidArgument)
// before any write otherwise.
[[nodiscard]] inline atx::core::Status two_speed_aim(std::span<const atx::u8> member,
                                                    std::span<const atx::f64> fast_desired,
                                                    atx::f64 fast_share, atx::f64 leverage,
                                                    atx::f64 theta_fast, atx::f64 theta_slow,
                                                    std::span<atx::f64> fast, std::span<atx::f64> desired) {
  const auto n = member.size();
  if (fast_desired.size() != n || fast.size() != n || desired.size() != n)
    return atx::core::Err(atx::core::ErrorCode::InvalidArgument, "two-speed: one entry per name in every span");
  if (!(fast_share >= 0.0 && fast_share <= 1.0) || !std::isfinite(leverage) || !(leverage > 0.0))
    return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                          "two-speed: the fast share must lie in [0, 1] and the leverage be finite and > 0");
  for (const atx::f64 theta : {theta_fast, theta_slow})
    if (!std::isfinite(theta) || !(theta > 0.0) || theta > 1.0)
      return atx::core::Err(atx::core::ErrorCode::InvalidArgument, "two-speed: theta must lie in (0, 1]");
  for (atx::usize i = 0; i < n; ++i) {
    if (member[i] > atx::u8{1})
      return atx::core::Err(atx::core::ErrorCode::InvalidArgument, "two-speed: member flags must be 0 or 1");
    if (member[i] != atx::u8{0} &&
        (!std::isfinite(fast_desired[i]) || !std::isfinite(desired[i]) || !std::isfinite(fast[i])))
      return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                            "two-speed: a member's desired targets and fast sleeve must be finite");
  }
  const atx::f64 slow_share = 1.0 - fast_share;
  for (atx::usize i = 0; i < n; ++i) {
    if (member[i] == atx::u8{0}) {
      fast[i] = 0.0;
      desired[i] = 0.0;
      continue;
    }
    const atx::f64 before = fast[i];
    const atx::f64 after = before + theta_fast * (leverage * fast_share * fast_desired[i] - before);
    fast[i] = after;
    desired[i] = slow_share * desired[i] + (before + (after - before) / theta_slow) / leverage;
  }
  return atx::core::Ok();
}

} // namespace atx::engine::book
