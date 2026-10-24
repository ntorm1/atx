#include "thomas.hpp"

#include <cmath>

namespace atx::vol::ddiv_ref::research {

Statuses admit(const ThomasPack &p, std::uint8_t lanes) noexcept {
  Statuses status{};
  const auto size = p.rhs.size();
  const bool shape = size >= 2 && size <= (1u << 22) + 1u &&
      p.obstacle.size() == size && p.cp.size() == size && p.dp.size() == size &&
      p.work.size() == size && p.active.size() == size && (lanes & 0xf0u) == 0;
  for (std::size_t lane = 0; lane < 4; ++lane) {
    if ((lanes & (1u << lane)) == 0) continue;
    status[lane] = LaneStatus::InvalidInput;
    if (!shape) continue;
    const Rows &r = p.rows[lane];
    if (!std::isfinite(r.lo) || !std::isfinite(r.di) || !std::isfinite(r.up) ||
        !std::isfinite(r.bottom) || !std::isfinite(r.top) || r.lo > 0. || r.up > 0. ||
        !(r.di > std::abs(r.lo) + std::abs(r.up))) continue;
    bool valid = true;
    for (std::size_t i = 1; i + 1 < size; ++i) {
      valid = valid && std::isfinite(p.active[i][lane] != 0
          ? p.obstacle[i][lane] : p.rhs[i][lane]);
    }
    if (valid) status[lane] = LaneStatus::Success;
  }
  return status;
}

void check_results(const ThomasPack &p, Statuses &status) noexcept {
  for (std::size_t lane = 0; lane < 4; ++lane) {
    if (status[lane] != LaneStatus::Success) continue;
    for (std::size_t i = 0; i < p.rhs.size(); ++i) {
      if (!std::isfinite(p.cp[i][lane]) || !std::isfinite(p.dp[i][lane]) ||
          !std::isfinite(p.work[i][lane])) {
        status[lane] = LaneStatus::NonfiniteResult;
        break;
      }
    }
  }
}

Statuses solve_scalar(ThomasPack &p, std::uint8_t lanes) noexcept {
  auto status = admit(p, lanes);
  for (std::size_t lane = 0; lane < 4; ++lane) {
    if (status[lane] != LaneStatus::Success) continue;
    const std::size_t n = p.rhs.size() - 1u;
    const Rows &r = p.rows[lane];
    p.cp[0][lane] = 0.;
    p.dp[0][lane] = r.bottom;
    for (std::size_t i = 1; i <= n; ++i) {
      const bool identity = i == n || p.active[i][lane] != 0;
      const double l = identity ? 0. : r.lo;
      const double d = identity ? 1. : r.di;
      const double u = identity ? 0. : r.up;
      const double rv = i == n ? r.top : identity ? p.obstacle[i][lane] : p.rhs[i][lane];
      const double m = std::fma(-l, p.cp[i - 1u][lane], d);
      p.cp[i][lane] = u / m;
      p.dp[i][lane] = std::fma(-l, p.dp[i - 1u][lane], rv) / m;
    }
    p.work[n][lane] = p.dp[n][lane];
    for (std::size_t i = n; i-- > 0;) {
      p.work[i][lane] = std::fma(-p.cp[i][lane], p.work[i + 1u][lane], p.dp[i][lane]);
    }
  }
  check_results(p, status);
  return status;
}

} // namespace atx::vol::ddiv_ref::research
