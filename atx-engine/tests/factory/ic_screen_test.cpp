#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "atx/core/types.hpp"
#include "atx/engine/alpha/panel.hpp"
#include "atx/engine/factory/ic_screen.hpp"

namespace {
using atx::f64;
using atx::usize;
using atx::engine::alpha::Panel;
using namespace atx::engine::factory;
constexpr f64 nan = std::numeric_limits<f64>::quiet_NaN();

std::vector<f64> prices(usize dates, usize names, bool ties = false) {
  std::vector<f64> out(dates * names);
  for (usize d = 0; d < dates; ++d) {
    for (usize i = 0; i < names; ++i) {
      const f64 level = static_cast<f64>(ties ? i / 2U : i) - static_cast<f64>(names) * 0.5;
      out[d * names + i] = std::exp(0.0001 * static_cast<f64>(d) * level);
    }
  }
  return out;
}

atx::core::Result<Panel> panel(usize dates, usize names, std::vector<f64> close,
                              std::vector<atx::u8> mask = {}) {
  std::vector<std::vector<f64>> columns;
  columns.push_back(std::move(close));
  return Panel::create(dates, names, {"close"}, std::move(columns), std::move(mask));
}

// Independent O(N^2) rank oracle: count smaller/equal values, no sorting.
std::vector<f64> reference_ranks(std::span<const f64> x) {
  std::vector<f64> ranks(x.size());
  for (usize i = 0; i < x.size(); ++i) {
    usize less = 0, same = 0;
    for (const f64 v : x) { less += v < x[i] ? 1U : 0U; same += v == x[i] ? 1U : 0U; }
    ranks[i] = static_cast<f64>(less) + 0.5 * static_cast<f64>(same - 1U);
  }
  return ranks;
}

// Independent all-pairs covariance identity, no SIMD or mean subtraction.
f64 reference_corr(std::span<const f64> x, std::span<const f64> y) {
  f64 sx = 0.0, sy = 0.0;
  for (usize i = 0; i < x.size(); ++i) { sx = std::max(sx, std::abs(x[i])); sy = std::max(sy, std::abs(y[i])); }
  f64 xx = 0.0, yy = 0.0, xy = 0.0;
  for (usize i = 0; i < x.size(); ++i) for (usize j = i + 1U; j < x.size(); ++j) {
    const f64 dx = x[i] / sx - x[j] / sx, dy = y[i] / sy - y[j] / sy;
    xx += dx * dx; yy += dy * dy; xy += dx * dy;
  }
  return (xy / std::sqrt(xx)) / std::sqrt(yy);
}

IcScreenConfig small_config() {
  IcScreenConfig cfg;
  cfg.rule = IcScreenRule::ConservativeV2;
  cfg.horizons = {1, 2, 3, 4}; cfg.min_names = 3; cfg.min_dates = 8;
  return cfg;
}

TEST(IcScreen, DisabledHasNoDenseCacheAndNeverScreensSignal) {
  auto p = panel(3, 5, prices(3, 5)); ASSERT_TRUE(p);
  auto cache = prepare_ic_screen(*p, {}); ASSERT_TRUE(cache);
  EXPECT_EQ(cache->bytes(), 0U); EXPECT_TRUE(cache->returns(0).empty());
  auto scratch = prepare_ic_screen_scratch(*cache); ASSERT_TRUE(scratch);
  const auto result = screen_ic({}, *cache, *scratch); ASSERT_TRUE(result);
  EXPECT_FALSE(result->reject); EXPECT_EQ(result->reason, IcScreenReason::Disabled);
}

TEST(IcScreen, SimdMatchesIndependentPairwiseOracleWithTiesMissingAndTails) {
  constexpr usize dates = 24, names = 35;
  auto raw = prices(dates, names, true);
  raw[9U * names + 2U] = nan;
  auto p = panel(dates, names, std::move(raw)); ASSERT_TRUE(p);
  const auto cfg = small_config();
  auto cache = prepare_ic_screen(*p, cfg); ASSERT_TRUE(cache);
  auto scratch = prepare_ic_screen_scratch(*cache); ASSERT_TRUE(scratch);
  std::vector<f64> signal(dates * names);
  for (usize d = 0; d < dates; ++d) for (usize i = 0; i < names; ++i)
    signal[d * names + i] = static_cast<f64>((i + 2U * d) % 7U) - 3.0;
  signal[3U * names + 1U] = nan;
  signal[3U * names + 7U] = std::numeric_limits<f64>::infinity();
  // Huge finite values exercise scaled reductions rather than overflowing x*x.
  for (usize i = 0; i < names; ++i) signal[5U * names + i] *= 1e300;
  const auto result = screen_ic(signal, *cache, *scratch); ASSERT_TRUE(result);
  EXPECT_GE(ic_screen_simd_width(), 2U);
  for (usize h = 0; h < 4U; ++h) {
    const auto labels = cache->returns(h);
    const auto actual = scratch->pearson_series(h), ranked = scratch->rank_series(h);
    ASSERT_EQ(labels.size(), actual.size() * names);
    for (usize d = 0; d < actual.size(); ++d) {
      std::vector<f64> x, y;
      for (usize i = 0; i < names; ++i) if (std::isfinite(signal[d * names + i]) && std::isfinite(labels[d * names + i])) {
        x.push_back(signal[d * names + i]); y.push_back(labels[d * names + i]);
      }
      ASSERT_GE(x.size(), 3U);
      EXPECT_NEAR(actual[d], reference_corr(x, y), 2e-13) << "h=" << h << " d=" << d;
      const auto xr = reference_ranks(x), yr = reference_ranks(y);
      EXPECT_NEAR(ranked[d], reference_corr(xr, yr), 2e-13) << "h=" << h << " d=" << d;
    }
  }
}

TEST(IcScreen, DelayedMatureLabelsUseDecisionMembershipAndReturnGuardOnly) {
  constexpr usize dates = 20, names = 9;
  auto raw = prices(dates, names);
  std::vector<atx::u8> mask(dates * names, 1), member(dates * names, 1);
  mask[3U * names] = 0; // future membership cannot remove the d=2 label
  member[2U * names + 1U] = 0;
  std::vector<atx::u32> bad(dates * names, 0);
  for (usize d = 4; d < dates; ++d) bad[d * names + 4U] = 1;
  auto cfg = small_config(); cfg.window_begin = 2; cfg.window_end = 14; cfg.maturity_end = 10;
  auto p = panel(dates, names, raw, mask); ASSERT_TRUE(p);
  auto a = prepare_ic_screen(*p, cfg, member, bad); ASSERT_TRUE(a);
  ASSERT_EQ(a->returns(0).size(), 6U * names); // d=2..7, endpoint d+2 < 10
  EXPECT_DOUBLE_EQ(a->returns(0)[0], raw[4U * names] / raw[3U * names] - 1.0);
  EXPECT_TRUE(std::isnan(a->returns(0)[1])); EXPECT_TRUE(std::isnan(a->returns(0)[4]));
  for (usize d = 10; d < dates; ++d) for (usize i = 0; i < names; ++i) {
    raw[d * names + i] = nan; member[d * names + i] = 0; bad[d * names + i] = 0;
  }
  auto changed = panel(dates, names, raw, mask); ASSERT_TRUE(changed);
  auto b = prepare_ic_screen(*changed, cfg, member, bad); ASSERT_TRUE(b);
  for (usize h = 0; h < 4U; ++h) {
    ASSERT_EQ(a->returns(h).size(), b->returns(h).size());
    EXPECT_EQ(std::memcmp(a->returns(h).data(), b->returns(h).data(), a->returns(h).size_bytes()), 0);
    EXPECT_EQ(std::memcmp(a->return_ranks(h).data(), b->return_ranks(h).data(), a->return_ranks(h).size_bytes()), 0);
  }
}

TEST(IcScreen, InvalidShapesModesBudgetsAndScratchBindingReturnErrors) {
  auto p = panel(20, 9, prices(20, 9)); ASSERT_TRUE(p);
  auto cfg = small_config();
  EXPECT_FALSE(prepare_ic_screen(*p, cfg, std::vector<atx::u8>(1, 1)));
  EXPECT_FALSE(prepare_ic_screen(*p, cfg, {}, std::vector<atx::u32>(1, 0)));
  auto invalid = cfg; invalid.execution_delay = 0; EXPECT_FALSE(prepare_ic_screen(*p, invalid));
  invalid = cfg; invalid.horizons[3] = std::numeric_limits<usize>::max(); EXPECT_FALSE(prepare_ic_screen(*p, invalid));
  invalid = cfg; invalid.max_cache_bytes = 1; EXPECT_FALSE(prepare_ic_screen(*p, invalid));
  invalid = cfg; invalid.window_end = 21; EXPECT_FALSE(prepare_ic_screen(*p, invalid));
  auto a = prepare_ic_screen(*p, cfg), b = prepare_ic_screen(*p, cfg); ASSERT_TRUE(a); ASSERT_TRUE(b);
  auto scratch = prepare_ic_screen_scratch(*a); ASSERT_TRUE(scratch);
  EXPECT_FALSE(screen_ic({}, *a, *scratch));
  EXPECT_FALSE(screen_ic(std::vector<f64>(180, 1.0), *b, *scratch));
}

TEST(IcScreen, PreparedCacheRequiresMatchingResolvedGeometryAndRecipe) {
  auto p = panel(32, 9, prices(32, 9)); ASSERT_TRUE(p);
  const auto cfg = small_config();
  auto cache = prepare_ic_screen(*p, cfg); ASSERT_TRUE(cache);
  EXPECT_TRUE(ic_screen_cache_matches(*cache, *p, cfg));
  auto resolved = cfg; resolved.window_end = 32; resolved.maturity_end = 32;
  EXPECT_TRUE(ic_screen_cache_matches(*cache, *p, resolved));
  std::array<IcScreenConfig, 11> different;
  different.fill(cfg);
  different[0].rule = IcScreenRule::DisabledV1;
  different[1].horizons[3] = 5;
  different[2].execution_delay = 2;
  different[3].window_begin = 1;
  different[4].window_end = 16;
  different[5].maturity_end = 16;
  different[6].min_names = 4;
  different[7].min_dates = 16;
  different[8].practical_abs_ic = 0.01;
  different[9].confidence_multiplier = 4.0;
  different[10].max_cache_bytes /= 2U;
  for (const auto& changed : different) EXPECT_FALSE(ic_screen_cache_matches(*cache, *p, changed));
  auto other = panel(32, 10, prices(32, 10)); ASSERT_TRUE(other);
  EXPECT_FALSE(ic_screen_cache_matches(*cache, *other, cfg));
  EXPECT_FALSE(ic_screen_cache_matches(IcScreenCache{}, *p, cfg));
}

TEST(IcScreen, ShortSparseAndConstantCandidatesPassThrough) {
  constexpr usize dates = 150, names = 32;
  auto p = panel(dates, names, prices(dates, names)); ASSERT_TRUE(p);
  IcScreenConfig cfg; cfg.rule = IcScreenRule::ConservativeV2;
  auto cache = prepare_ic_screen(*p, cfg); ASSERT_TRUE(cache);
  auto scratch = prepare_ic_screen_scratch(*cache); ASSERT_TRUE(scratch);
  std::vector<f64> signal(dates * names, 1.0);
  auto constant = screen_ic(signal, *cache, *scratch); ASSERT_TRUE(constant);
  EXPECT_FALSE(constant->reject); EXPECT_FALSE(constant->enough_evidence);
  for (usize d = 0; d < dates; ++d) for (usize i = 0; i < names; ++i)
    signal[d * names + i] = i < 15U ? nan : static_cast<f64>(i);
  const auto sparse = screen_ic(signal, *cache, *scratch); ASSERT_TRUE(sparse);
  EXPECT_FALSE(sparse->reject); EXPECT_EQ(sparse->reason, IcScreenReason::InsufficientEvidence);
}

TEST(IcScreen, PairCoverageUsesOriginalDecisionEligibleNames) {
  constexpr usize dates = 40, names = 100;
  auto raw = prices(dates, names);
  std::vector<f64> signal(dates * names, nan);
  for (usize d = 0; d < dates; ++d) {
    // All 100 names are decision-eligible in the declared membership. Only 80
    // have finite labels and only 64 have finite signals: 80% twice is 64%.
    for (usize i = 80; i < names; ++i) raw[d * names + i] = nan;
    for (usize i = 0; i < 64; ++i)
      signal[d * names + i] = static_cast<f64>((i + 14U) % 64U);
  }
  auto p = panel(dates, names, std::move(raw)); ASSERT_TRUE(p);
  const auto cfg = small_config();
  auto cache = prepare_ic_screen(*p, cfg); ASSERT_TRUE(cache);
  auto scratch = prepare_ic_screen_scratch(*cache); ASSERT_TRUE(scratch);
  const auto result = screen_ic(signal, *cache, *scratch); ASSERT_TRUE(result);
  EXPECT_FALSE(result->reject); EXPECT_EQ(result->reason, IcScreenReason::InsufficientEvidence);
  for (const auto& h : result->horizons) {
    EXPECT_EQ(h.pearson.valid_dates, 0U); EXPECT_EQ(h.rank.valid_dates, 0U);
  }
}

TEST(IcScreen, AlternatingNullIsRejectedWithoutAFullBacktest) {
  constexpr usize dates = 384, names = 32;
  auto p = panel(dates, names, prices(dates, names)); ASSERT_TRUE(p);
  IcScreenConfig cfg; cfg.rule = IcScreenRule::ConservativeV2;
  auto cache = prepare_ic_screen(*p, cfg); ASSERT_TRUE(cache);
  auto scratch = prepare_ic_screen_scratch(*cache); ASSERT_TRUE(scratch);
  std::vector<f64> signal(dates * names);
  for (usize d = 0; d < dates; ++d) for (usize i = 0; i < names; ++i) {
    const f64 shifted = static_cast<f64>((i + 7U) % names) - 15.5;
    signal[d * names + i] = d % 2U == 0U ? shifted : -shifted;
  }
  const auto result = screen_ic(signal, *cache, *scratch); ASSERT_TRUE(result);
  EXPECT_TRUE(result->enough_evidence); EXPECT_TRUE(result->reject);
  EXPECT_EQ(result->reason, IcScreenReason::PracticalNull);
  for (const auto& h : result->horizons) {
    EXPECT_LT(h.pearson.upper_abs_ic, cfg.practical_abs_ic);
    EXPECT_LT(h.rank.upper_abs_ic, cfg.practical_abs_ic);
    EXPECT_GE(h.pearson.hac_lag, 2U * h.horizon);
  }
  // Signal negation cannot change the screen's two-sided decision.
  for (auto& v : signal) v = -v;
  const auto inverse = screen_ic(signal, *cache, *scratch); ASSERT_TRUE(inverse);
  EXPECT_EQ(inverse->reject, result->reject);
}

// Construct a precise weak IC at a chosen horizon, with alternating daily
// perturbations. Orthogonalizing synthetic noise is only fixture construction;
// it never appears in the production screen or reads market data.
std::vector<f64> weak_signal(const IcScreenCache& cache, usize horizon_index,
                             f64 effect, bool regime = false) {
  const usize n = cache.instruments(), dates = cache.dates();
  std::vector<f64> signal(dates * n), noise(n), target(n);
  for (usize d = 0; d < dates; ++d) for (usize i = 0; i < n; ++i)
    signal[d * n + i] = std::sin(static_cast<f64>((d + 3U) * (i + 1U)) * 0.713);
  const auto labels = cache.returns(horizon_index);
  const usize active = labels.size() / n;
  for (usize d = 0; d < active; ++d) {
    f64 ym = 0.0, nm = 0.0;
    for (usize i = 0; i < n; ++i) { target[i] = labels[d * n + i]; ym += target[i]; noise[i] = signal[d * n + i]; nm += noise[i]; }
    ym /= static_cast<f64>(n); nm /= static_cast<f64>(n);
    f64 yy = 0.0, ny = 0.0;
    for (usize i = 0; i < n; ++i) { target[i] -= ym; noise[i] -= nm; yy += target[i] * target[i]; ny += noise[i] * target[i]; }
    f64 nn = 0.0;
    for (usize i = 0; i < n; ++i) { noise[i] -= ny / yy * target[i]; nn += noise[i] * noise[i]; }
    const f64 rho = regime ? (d < active / 4U ? 0.08 : -0.08 / 3.0) : effect + (d % 2U == 0U ? 0.006 : -0.006);
    for (usize i = 0; i < n; ++i)
      signal[d * n + i] = rho * target[i] / std::sqrt(yy) + std::sqrt(1.0 - rho * rho) * noise[i] / std::sqrt(nn);
  }
  return signal;
}

TEST(IcScreen, RetainsWeakInverseLongHorizonAndRegimeCohorts) {
  constexpr usize dates = 512, names = 64;
  auto p = panel(dates, names, prices(dates, names)); ASSERT_TRUE(p);
  IcScreenConfig cfg; cfg.rule = IcScreenRule::ConservativeV2;
  auto cache = prepare_ic_screen(*p, cfg); ASSERT_TRUE(cache);
  auto scratch = prepare_ic_screen_scratch(*cache); ASSERT_TRUE(scratch);
  for (const f64 effect : std::array<f64, 8>{0.002, 0.005, 0.01, 0.02, -0.002, -0.005, -0.01, -0.02}) {
    const auto signal = weak_signal(*cache, 3, effect);
    const auto result = screen_ic(signal, *cache, *scratch); ASSERT_TRUE(result);
    EXPECT_FALSE(result->reject) << effect;
    EXPECT_TRUE(result->horizons[3].pearson.defined);
    EXPECT_TRUE(result->horizons[3].pearson.suggestive_direction) << effect;
    EXPECT_NEAR(result->horizons[3].pearson.mean, effect, 0.00002);
  }
  for (usize h = 0; h < 4U; ++h) {
    const auto signal = weak_signal(*cache, h, 0.005);
    const auto result = screen_ic(signal, *cache, *scratch); ASSERT_TRUE(result);
    EXPECT_FALSE(result->reject) << h; EXPECT_TRUE(result->horizons[h].pearson.suggestive_direction);
  }
  const auto regime = screen_ic(weak_signal(*cache, 3, 0.0, true), *cache, *scratch); ASSERT_TRUE(regime);
  EXPECT_FALSE(regime->reject); EXPECT_GT(regime->horizons[3].pearson.max_segment_abs_ic, 0.07);
}

TEST(IcScreen, StrictMonotoneTransformPreservesRankPathAndDecision) {
  constexpr usize dates = 384, names = 32;
  auto p = panel(dates, names, prices(dates, names)); ASSERT_TRUE(p);
  IcScreenConfig cfg; cfg.rule = IcScreenRule::ConservativeV2;
  auto cache = prepare_ic_screen(*p, cfg); ASSERT_TRUE(cache);
  auto scratch = prepare_ic_screen_scratch(*cache); ASSERT_TRUE(scratch);
  std::vector<f64> signal(dates * names);
  for (usize d = 0; d < dates; ++d) for (usize i = 0; i < names; ++i) signal[d * names + i] = static_cast<f64>(i);
  const auto first = screen_ic(signal, *cache, *scratch); ASSERT_TRUE(first);
  std::array<std::vector<f64>, 4> rank;
  for (usize h = 0; h < 4U; ++h) rank[h].assign(scratch->rank_series(h).begin(), scratch->rank_series(h).end());
  for (auto& v : signal) v = std::exp(v * 0.25);
  const auto transformed = screen_ic(signal, *cache, *scratch); ASSERT_TRUE(transformed);
  EXPECT_FALSE(first->reject); EXPECT_FALSE(transformed->reject);
  for (usize h = 0; h < 4U; ++h) {
    ASSERT_EQ(rank[h].size(), scratch->rank_series(h).size());
    EXPECT_EQ(std::memcmp(rank[h].data(), scratch->rank_series(h).data(), rank[h].size() * sizeof(f64)), 0);
  }
}
} // namespace
