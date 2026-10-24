#include "thomas.hpp"

#include <algorithm>
#include <immintrin.h>

#if !defined(__AVX2__) || !defined(__FMA__)
#error "Research Thomas pack requires AVX2 and FMA"
#endif

namespace atx::vol::ddiv_ref::research {

Statuses solve_avx2(ThomasPack &p, std::uint8_t lanes) noexcept {
  auto status = admit(p, lanes);
  if (std::none_of(status.begin(), status.end(), [](LaneStatus s) {
        return s == LaneStatus::Success;
      })) return status;

  const auto enabled = [&](std::size_t lane) {
    return status[lane] == LaneStatus::Success ? -1LL : 0LL;
  };
  const __m256i enabled_mask = _mm256_set_epi64x(enabled(3), enabled(2), enabled(1), enabled(0));
  const __m256d zero = _mm256_setzero_pd();
  const __m256d one = _mm256_set1_pd(1.);
  Quad lower{}, diagonal{1., 1., 1., 1.}, upper{}, bottom{}, top{};
  for (std::size_t lane = 0; lane < 4; ++lane) {
    if (status[lane] != LaneStatus::Success) continue;
    lower[lane] = p.rows[lane].lo;
    diagonal[lane] = p.rows[lane].di;
    upper[lane] = p.rows[lane].up;
    bottom[lane] = p.rows[lane].bottom;
    top[lane] = p.rows[lane].top;
  }
  const __m256d lo = _mm256_loadu_pd(lower.data());
  const __m256d di = _mm256_loadu_pd(diagonal.data());
  const __m256d up = _mm256_loadu_pd(upper.data());
  const __m256d hi = _mm256_loadu_pd(top.data());
  __m256d previous_cp = zero;
  __m256d previous_dp = _mm256_loadu_pd(bottom.data());
  _mm256_maskstore_pd(p.cp[0].data(), enabled_mask, previous_cp);
  _mm256_maskstore_pd(p.dp[0].data(), enabled_mask, previous_dp);
  const std::size_t n = p.rhs.size() - 1u;
  for (std::size_t i = 1; i <= n; ++i) {
    const auto identity = [&](std::size_t lane) {
      return i == n || p.active[i][lane] != 0 ? -1LL : 0LL;
    };
    const __m256d id = _mm256_castsi256_pd(
        _mm256_set_epi64x(identity(3), identity(2), identity(1), identity(0)));
    const __m256d l = _mm256_blendv_pd(lo, zero, id);
    const __m256d d = _mm256_blendv_pd(di, one, id);
    const __m256d u = _mm256_blendv_pd(up, zero, id);
    const __m256d rhs = _mm256_maskload_pd(p.rhs[i].data(), enabled_mask);
    const __m256d obstacle = _mm256_maskload_pd(p.obstacle[i].data(), enabled_mask);
    const __m256d rv = i == n ? hi : _mm256_blendv_pd(rhs, obstacle, id);
    // Same three fused subtracts and two divisions as the frozen scalar object.
    const __m256d denominator = _mm256_fnmadd_pd(l, previous_cp, d);
    previous_cp = _mm256_div_pd(u, denominator);
    previous_dp = _mm256_div_pd(_mm256_fnmadd_pd(l, previous_dp, rv), denominator);
    _mm256_maskstore_pd(p.cp[i].data(), enabled_mask, previous_cp);
    _mm256_maskstore_pd(p.dp[i].data(), enabled_mask, previous_dp);
  }
  __m256d next = previous_dp;
  _mm256_maskstore_pd(p.work[n].data(), enabled_mask, next);
  for (std::size_t i = n; i-- > 0;) {
    const __m256d cp = _mm256_maskload_pd(p.cp[i].data(), enabled_mask);
    const __m256d dp = _mm256_maskload_pd(p.dp[i].data(), enabled_mask);
    next = _mm256_fnmadd_pd(cp, next, dp);
    _mm256_maskstore_pd(p.work[i].data(), enabled_mask, next);
  }
  check_results(p, status);
  return status;
}

} // namespace atx::vol::ddiv_ref::research
