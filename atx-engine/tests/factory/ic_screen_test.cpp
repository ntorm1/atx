#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <iostream>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "atx/core/types.hpp"
#include "atx/engine/alpha/panel.hpp"
#include "atx/engine/eval/hac.hpp"
#include "atx/engine/factory/ic_research.hpp"
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

// Analytic estimate-level cases describe mean/SE directly. They do not construct
// artificially quiet signal observations or claim a population recall rate.
std::array<IcScreenHorizon, 4> precise_estimates(const IcScreenConfig& cfg,
                                               f64 mean = 0.0, f64 se = 0.0001) {
  std::array<IcScreenHorizon, 4> out{};
  for (usize k = 0; k < out.size(); ++k) {
    out[k].horizon = cfg.horizons[k];
    for (auto* e : {&out[k].pearson, &out[k].rank}) {
      e->valid_dates = 600; e->calendar_dates = 600; e->hac_lag = cfg.horizons[k] * 2U;
      e->mean = mean; e->standard_error = se;
      e->upper_abs_ic = std::abs(mean) + cfg.confidence_multiplier * se;
      e->max_segment_abs_ic = std::abs(mean);
      e->defined = true; e->suggestive_direction = std::abs(mean) >= se;
    }
  }
  return out;
}

TEST(IcScreen, EquivalenceV3PoweredEstimatesProtectDeclaredEffectsAcrossEveryMetric) {
  const auto cfg = equivalence_ic_screen_config();
  EXPECT_EQ(cfg.rule, IcScreenRule::EquivalenceV3);
  EXPECT_DOUBLE_EQ(cfg.practical_abs_ic, 0.002);
  EXPECT_DOUBLE_EQ(cfg.confidence_multiplier, 3.5);
  EXPECT_EQ(ic_screen_rule_name(cfg.rule), "equivalence-v3");
  EXPECT_DOUBLE_EQ(IcScreenConfig{}.practical_abs_ic, 0.02); // unchanged legacy construction
  const auto powered_null = precise_estimates(cfg);
  EXPECT_TRUE(classify_ic_screen_estimates(powered_null, cfg).reject);
  for (usize k = 0; k < 4U; ++k) {
    for (const bool rank : {false, true}) {
      for (const f64 effect : {0.002, -0.002, 0.005, -0.005}) {
        auto estimates = powered_null;
        auto& e = rank ? estimates[k].rank : estimates[k].pearson;
        e.mean = effect;
        // Deliberately leave the old cached upper bound untouched: V3 must use
        // the configured mean/SE formula, including both signs and every metric.
        const auto result = classify_ic_screen_estimates(estimates, cfg);
        EXPECT_TRUE(result.enough_evidence);
        EXPECT_FALSE(result.reject) << k << ':' << rank << ':' << effect;
        EXPECT_EQ(result.reason, IcScreenReason::PossibleAlpha);
      }
      auto uncertain = powered_null;
      auto& e = rank ? uncertain[k].rank : uncertain[k].pearson;
      e.standard_error = 0.001;
      EXPECT_FALSE(classify_ic_screen_estimates(uncertain, cfg).reject);
      e.defined = false;
      EXPECT_EQ(classify_ic_screen_estimates(uncertain, cfg).reason, IcScreenReason::InsufficientEvidence);
    }
  }
}

TEST(IcScreen, EquivalenceV3RemovesOnlyV2DirectionalVetoAndKeepsQuarterHeuristic) {
  auto cfg = equivalence_ic_screen_config();
  auto legacy = cfg; legacy.rule = IcScreenRule::ConservativeV2;
  for (const f64 mean : {0.0004, -0.0004}) {
    const auto precise_submargin = precise_estimates(cfg, mean);
    EXPECT_TRUE(precise_submargin[0].pearson.suggestive_direction);
    EXPECT_FALSE(classify_ic_screen_estimates(precise_submargin, legacy).reject);
    EXPECT_TRUE(classify_ic_screen_estimates(precise_submargin, cfg).reject);
  }
  auto estimates = precise_estimates(cfg);
  estimates[2].rank.max_segment_abs_ic = 0.003;
  EXPECT_FALSE(classify_ic_screen_estimates(estimates, cfg).reject);
  // The quarter-mean check is an extra heuristic, not a calibrated claim that
  // all regimes or intermittent effects are retained.
  estimates = precise_estimates(cfg);
  estimates[0].pearson.standard_error = 0.0;
  EXPECT_FALSE(classify_ic_screen_estimates(estimates, cfg).reject);
  estimates[0].pearson.standard_error = nan;
  EXPECT_EQ(classify_ic_screen_estimates(estimates, cfg).reason, IcScreenReason::InsufficientEvidence);
  estimates = precise_estimates(cfg);
  estimates[3].rank.max_segment_abs_ic = nan;
  EXPECT_FALSE(classify_ic_screen_estimates(estimates, cfg).reject);
  estimates = precise_estimates(cfg);
  cfg.practical_abs_ic = estimates[0].pearson.upper_abs_ic;
  EXPECT_FALSE(classify_ic_screen_estimates(estimates, cfg).reject); // strict boundary
  estimates[0].pearson.upper_abs_ic = 0.5;
  EXPECT_FALSE(classify_ic_screen_estimates(estimates, legacy).reject); // V2 uses its stored bound
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

TEST(IcScreen, SharedDateRanksHandleEqualCountDifferentNamesAndScratchReuse) {
  constexpr usize dates = 24, names = 35;
  auto raw = prices(dates, names, true);
  // Missing decision/entry names are shared; each horizon has a distinct
  // missing exit name. All four paired subsets have the same size.
  for (usize d = 0; d < dates; ++d) raw[d * names + d % names] = nan;
  auto p = panel(dates, names, std::move(raw)); ASSERT_TRUE(p);
  auto cache = prepare_ic_screen(*p, small_config()); ASSERT_TRUE(cache);
  auto scratch = prepare_ic_screen_scratch(*cache); ASSERT_TRUE(scratch);
  std::vector<f64> signal(dates * names);
  for (usize pass = 0; pass < 2U; ++pass) {
    for (usize d = 0; d < dates; ++d) for (usize i = 0; i < names; ++i)
      signal[d * names + i] = static_cast<f64>((i * (pass + 1U) + 3U * d) % 9U);
    ASSERT_TRUE(screen_ic(signal, *cache, *scratch));
    for (usize h = 0; h < 4U; ++h) {
      const auto labels = cache->returns(h);
      const auto actual = scratch->rank_series(h);
      for (usize d = 0; d < actual.size(); ++d) {
        std::vector<f64> x, y;
        for (usize i = 0; i < names; ++i) if (std::isfinite(labels[d * names + i])) {
          x.push_back(signal[d * names + i]); y.push_back(labels[d * names + i]);
        }
        ASSERT_EQ(x.size(), names - 3U);
        EXPECT_NEAR(actual[d], reference_corr(reference_ranks(x), reference_ranks(y)), 2e-13)
            << "pass=" << pass << " h=" << h << " d=" << d;
      }
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

TEST(IcScreen, EquivalenceV3ActualScreenUsesSmallDeclaredMarginAndRetainsBoundarySignals) {
  constexpr usize dates = 384, names = 128;
  std::vector<f64> close(dates * names), signal(dates * names);
  for (usize d = 0; d < dates; ++d) for (usize i = 0; i < names; ++i) {
    close[d * names + i] = std::exp(1e-7 * static_cast<f64>(d) * (static_cast<f64>(i) - 63.5));
    const f64 shifted = static_cast<f64>((i + 27U) % names) - 63.5;
    signal[d * names + i] = d % 2U == 0U ? shifted : -shifted;
  }
  // A deliberately precise algebraic fixture exercises production wiring only;
  // it is not stochastic null-power or general weak-signal recall evidence.
  auto p = panel(dates, names, std::move(close)); ASSERT_TRUE(p);
  const auto cfg = equivalence_ic_screen_config();
  auto cache = prepare_ic_screen(*p, cfg); ASSERT_TRUE(cache);
  auto scratch = prepare_ic_screen_scratch(*cache); ASSERT_TRUE(scratch);
  const auto rejected = screen_ic(signal, *cache, *scratch); ASSERT_TRUE(rejected);
  EXPECT_TRUE(rejected->enough_evidence); EXPECT_TRUE(rejected->reject);
  auto legacy = cfg; legacy.rule = IcScreenRule::ConservativeV2;
  EXPECT_FALSE(ic_screen_cache_matches(*cache, *p, legacy)); // resume/cache identity is versioned
  for (const f64 effect : {0.002, -0.002, 0.005, -0.005}) {
    const auto result = screen_ic(weak_signal(*cache, 3, effect), *cache, *scratch);
    ASSERT_TRUE(result); EXPECT_FALSE(result->reject) << effect;
    EXPECT_GE(result->horizons[3].pearson.upper_abs_ic, cfg.practical_abs_ic);
  }
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

class CohortRng {
public:
  explicit CohortRng(atx::u64 seed) : state_{seed} {}
  f64 normal() {
    const f64 u = (static_cast<f64>(next() >> 11U) + 0.5) * 0x1.0p-53;
    const f64 v = (static_cast<f64>(next() >> 11U) + 0.5) * 0x1.0p-53;
    return std::sqrt(-2.0 * std::log(u)) * std::cos(6.283185307179586 * v);
  }
private:
  atx::u64 next() {
    atx::u64 x = (state_ += 0x9e3779b97f4a7c15ULL);
    x = (x ^ (x >> 30U)) * 0xbf58476d1ce4e5b9ULL;
    x = (x ^ (x >> 27U)) * 0x94d049bb133111ebULL;
    return x ^ (x >> 31U);
  }
  atx::u64 state_;
};

std::vector<f64> stochastic_prices(usize dates, usize names) {
  CohortRng rng{0x194902U};
  std::vector<f64> close(dates * names, 100.0);
  for (usize d = 1; d < dates; ++d) {
    const f64 market = 0.003 * rng.normal();
    for (usize i = 0; i < names; ++i)
      close[d * names + i] = close[(d - 1U) * names + i] * std::exp(market + 0.01 * rng.normal());
  }
  return close;
}

f64 scalar_dot(std::span<const f64> x, std::span<const f64> y) {
  f64 value = 0.0;
  for (usize i = 0; i < x.size(); ++i) value += x[i] * y[i];
  return value;
}

void center_unit(std::span<f64> x) {
  f64 mean = 0.0;
  for (const f64 v : x) mean += v;
  mean /= static_cast<f64>(x.size());
  for (auto& v : x) v -= mean;
  const f64 scale = std::sqrt(scalar_dot(x, x));
  for (auto& v : x) v /= scale;
}

struct NoisySignal {
  std::vector<f64> values;
  f64 max_other_effect_projection{};
};

// The injected component is orthogonal to every other AVAILABLE horizon's
// return vector. Independent Gaussian signal noise is NOT orthogonalized or
// rescaled to force a sample IC: .002/.005 cases have low expected t at this
// sample size, unlike the separate precise algebraic fixtures above.
NoisySignal noisy_signal(const IcScreenCache& cache, usize target_horizon,
                         f64 effect, atx::u64 seed, bool regime = false) {
  const usize n = cache.instruments(), dates = cache.dates();
  const auto labels = cache.returns(target_horizon);
  const usize active = labels.size() / n;
  CohortRng rng{seed};
  NoisySignal out;
  out.values.resize(dates * n);
  for (auto& v : out.values) v = rng.normal();
  if (effect == 0.0 && !regime) return out;
  std::array<std::vector<f64>, 3> basis;
  for (auto& b : basis) b.resize(n);
  std::vector<f64> target(n), residual(n), original_other(n);
  for (usize d = 0; d < active; ++d) {
    const auto target_row = labels.subspan(d * n, n);
    std::copy(target_row.begin(), target_row.end(), target.begin());
    center_unit(target);
    usize used = 0;
    for (usize h = 0; h < 4U; ++h) {
      const auto other = cache.returns(h);
      if (h == target_horizon || d >= other.size() / n) continue;
      auto& b = basis[used];
      std::copy_n(other.begin() + static_cast<std::ptrdiff_t>(d * n), n, b.begin());
      center_unit(b);
      for (usize j = 0; j < used; ++j) {
        const f64 projection = scalar_dot(b, basis[j]);
        for (usize i = 0; i < n; ++i) b[i] -= projection * basis[j][i];
      }
      center_unit(b); ++used;
    }
    residual = target;
    for (usize j = 0; j < used; ++j) {
      const f64 projection = scalar_dot(residual, basis[j]);
      for (usize i = 0; i < n; ++i) residual[i] -= projection * basis[j][i];
    }
    center_unit(residual);
    const f64 target_projection = scalar_dot(residual, target);
    for (usize h = 0; h < 4U; ++h) {
      const auto other = cache.returns(h);
      if (h == target_horizon || d >= other.size() / n) continue;
      std::copy_n(other.begin() + static_cast<std::ptrdiff_t>(d * n), n, original_other.begin());
      center_unit(original_other);
      out.max_other_effect_projection = std::max(out.max_other_effect_projection,
                                                  std::abs(scalar_dot(residual, original_other)));
    }
    const f64 rho = regime ? (d < active / 4U ? 0.06 : -0.02) : effect;
    const f64 coefficient = rho / target_projection;
    if (!std::isfinite(coefficient) || std::abs(coefficient) >= 0.9) return {};
    for (usize i = 0; i < n; ++i)
      out.values[d * n + i] = std::sqrt(1.0 - coefficient * coefficient) * out.values[d * n + i] +
                              coefficient * std::sqrt(static_cast<f64>(n)) * residual[i];
  }
  return out;
}

TEST(IcScreen, BoundedNoisyDistinctHorizonQualificationReportsActualCohorts) {
  constexpr usize dates = 512, names = 96;
  auto p = panel(dates, names, stochastic_prices(dates, names)); ASSERT_TRUE(p);
  IcScreenConfig cfg; cfg.rule = IcScreenRule::ConservativeV2;
  auto cache = prepare_ic_screen(*p, cfg); ASSERT_TRUE(cache);
  auto scratch = prepare_ic_screen_scratch(*cache); ASSERT_TRUE(scratch);
  // Stochastic increments remove the common ordering shared by the old smooth
  // exponential fixture. Injected effects also remove cross-horizon loadings.
  EXPECT_LT(std::abs(reference_corr(cache->returns(0).first(names), cache->returns(3).first(names))), 0.99);
  usize null_total = 0, null_rejected = 0, effect_total = 0, effect_retained = 0;
  std::cout << "ic-screen-cohort,synthetic-only,not-general-recall,dates=512,names=96,floor="
            << cfg.practical_abs_ic << ",confidence=" << cfg.confidence_multiplier << '\n';
  const auto qualify = [&](std::string_view kind, usize h, f64 effect, atx::u64 seed,
                           bool regime, bool inverse) {
    auto signal = noisy_signal(*cache, h, effect, seed, regime);
    EXPECT_EQ(signal.values.size(), dates * names);
    EXPECT_LT(signal.max_other_effect_projection, 1e-11);
    if (signal.values.size() != dates * names) return;
    if (inverse) for (auto& v : signal.values) v = -v;
    const auto result = screen_ic(signal.values, *cache, *scratch);
    EXPECT_TRUE(result.has_value()); if (!result) return;
    EXPECT_TRUE(result->enough_evidence); // a pass must not be vacuous/short
    if (kind == "null") { ++null_total; null_rejected += result->reject ? 1U : 0U; }
    else { ++effect_total; effect_retained += result->reject ? 0U : 1U; }
    const auto& measured = result->horizons[h].pearson;
    std::cout << "ic-screen-cohort," << kind << ",h=" << cfg.horizons[h]
              << ",effect=" << effect * (inverse ? -1.0 : 1.0) << ",seed=" << seed
              << ",reject=" << result->reject << ",mean=" << measured.mean
              << ",hac_se=" << measured.standard_error << ",bound=" << measured.upper_abs_ic << '\n';
  };
  for (atx::u64 seed = 1; seed <= 6; ++seed) qualify("null", 3, 0.0, 700U + seed, false, false);
  for (usize h = 0; h < 4U; ++h) {
    qualify("weak", h, 0.002, 810U + static_cast<atx::u64>(h), false, false);
    qualify("weak", h, 0.005, 820U + static_cast<atx::u64>(h), false, false);
    qualify("inverse", h, 0.005, 820U + static_cast<atx::u64>(h), false, true);
    qualify("regime", h, 0.0, 830U + static_cast<atx::u64>(h), true, false);
  }
  qualify("positive", 3, 0.01, 840U, false, false);
  qualify("positive", 3, 0.02, 841U, false, false);
  EXPECT_EQ(null_total, 6U); EXPECT_EQ(effect_total, 18U);
  std::cout << "ic-screen-cohort,totals,null_rejected=" << null_rejected << '/' << null_total
            << ",effect_retained=" << effect_retained << '/' << effect_total << '\n';
  // Counts above are measurements to review, not a tuned recall/rejection gate.
  // Paired inverse/regime cases share data; these are not independent estimates
  // of population recall. No throughput claim is inferred from this test.
  auto monotone = noisy_signal(*cache, 3, 0.005, 920U);
  ASSERT_EQ(monotone.values.size(), dates * names);
  ASSERT_TRUE(screen_ic(monotone.values, *cache, *scratch));
  std::vector<f64> ranks(scratch->rank_series(3).begin(), scratch->rank_series(3).end());
  for (auto& v : monotone.values) v = std::exp(0.4 * v);
  ASSERT_TRUE(screen_ic(monotone.values, *cache, *scratch));
  EXPECT_EQ(std::memcmp(ranks.data(), scratch->rank_series(3).data(), ranks.size() * sizeof(f64)), 0);
  auto short_cfg = cfg; short_cfg.maturity_end = 198;
  auto short_cache = prepare_ic_screen(*p, short_cfg); ASSERT_TRUE(short_cache);
  auto short_scratch = prepare_ic_screen_scratch(*short_cache); ASSERT_TRUE(short_scratch);
  const auto short_result = screen_ic(monotone.values, *short_cache, *short_scratch); ASSERT_TRUE(short_result);
  EXPECT_FALSE(short_result->reject); EXPECT_EQ(short_result->reason, IcScreenReason::InsufficientEvidence);
}

TEST(IcScreen, WiderNoisyCohortReportsRejectionAndWeakSignalRetention) {
  constexpr usize dates = 512, names = 512;
  auto p = panel(dates, names, stochastic_prices(dates, names)); ASSERT_TRUE(p);
  IcScreenConfig cfg; cfg.rule = IcScreenRule::ConservativeV2;
  auto cache = prepare_ic_screen(*p, cfg); ASSERT_TRUE(cache);
  auto scratch = prepare_ic_screen_scratch(*cache); ASSERT_TRUE(scratch);
  usize null_rejected = 0, weak_retained = 0;
  const auto measure = [&](usize h, f64 effect, atx::u64 seed) {
    auto signal = noisy_signal(*cache, h, effect, seed);
    EXPECT_EQ(signal.values.size(), dates * names);
    if (signal.values.size() != dates * names) return false;
    const auto result = screen_ic(signal.values, *cache, *scratch);
    EXPECT_TRUE(result.has_value()); if (!result) return false;
    EXPECT_TRUE(result->enough_evidence);
    std::cout << "ic-screen-wide,dates=512,names=512,h=" << cfg.horizons[h]
              << ",effect=" << effect << ",seed=" << seed << ",reject=" << result->reject
              << ",mean=" << result->horizons[h].pearson.mean
              << ",hac_se=" << result->horizons[h].pearson.standard_error << '\n';
    return result->reject;
  };
  for (atx::u64 seed = 701; seed <= 712; ++seed)
    null_rejected += measure(3, 0.0, seed) ? 1U : 0U;
  for (usize h = 0; h < 4U; ++h) {
    weak_retained += measure(h, 0.002, 810U + h) ? 0U : 1U;
    weak_retained += measure(h, 0.005, 820U + h) ? 0U : 1U;
    weak_retained += measure(h, -0.005, 850U + h) ? 0U : 1U;
  }
  std::cout << "ic-screen-wide,totals,null_rejected=" << null_rejected
            << "/12,weak_retained=" << weak_retained << "/12,synthetic-only\n";
  // Fixed diagnostic cohort: report the actual rates, never tune seeds or
  // assert a desired quota. Statistical uncertainty and economic value remain.
}
} // namespace

// ---- P9 lane S2: the IC method options (min_coverage, the HAC rule, the label terminal rule) ----
// A named namespace: this file joins unity batches beside other unnamed-namespace fixtures.
namespace atx_test_ic_screen_s2 {
using atx::f64;
using atx::u64;
using atx::u8;
using atx::usize;
namespace ex = atx::engine::factory;
namespace hac = atx::engine::eval::hac;
using atx::engine::alpha::Panel;
constexpr f64 kS2NaN = std::numeric_limits<f64>::quiet_NaN();

// Deterministic noise in [-1, 1) from a cell index (no RNG state, so each fixture is a pure
// function of its geometry).
f64 s2_noise(usize k) {
  u64 x = static_cast<u64>(k) * 0x9e3779b97f4a7c15ULL + 0x632be59bd9b4e019ULL;
  x = (x ^ (x >> 30U)) * 0xbf58476d1ce4e5b9ULL;
  x = (x ^ (x >> 27U)) * 0x94d049bb133111ebULL;
  x ^= x >> 31U;
  return static_cast<f64>(x >> 11U) * 0x1.0p-52 - 1.0;
}

// A positive random-walk close panel with every cell present (unless `mask` says otherwise).
std::vector<f64> s2_close(usize dates, usize names) {
  std::vector<f64> close(dates * names, 100.0);
  for (usize d = 1; d < dates; ++d)
    for (usize i = 0; i < names; ++i)
      close[d * names + i] = close[(d - 1U) * names + i] * std::exp(0.01 * s2_noise(d * names + i));
  return close;
}

atx::core::Result<Panel> s2_panel(usize dates, usize names, std::vector<f64> close,
                                  std::vector<u8> mask = {}, std::string_view field = "close") {
  std::vector<std::vector<f64>> columns;
  columns.push_back(std::move(close));
  return Panel::create(dates, names, {std::string(field)}, std::move(columns), std::move(mask));
}

ex::IcScreenConfig s2_config() {
  ex::IcScreenConfig cfg;
  cfg.rule = ex::IcScreenRule::ConservativeV2;
  cfg.horizons = {1, 2, 3, 4};
  cfg.min_names = 3;
  cfg.min_dates = 8;
  return cfg;
}

bool same_bits(f64 a, f64 b) { return std::bit_cast<u64>(a) == std::bit_cast<u64>(b); }

bool same_series(std::span<const f64> a, std::span<const f64> b) {
  return a.size() == b.size() &&
         (a.empty() || std::memcmp(a.data(), b.data(), a.size_bytes()) == 0);
}

void expect_same_estimate(const ex::IcScreenEstimate& a, const ex::IcScreenEstimate& b) {
  EXPECT_EQ(a.valid_dates, b.valid_dates);
  EXPECT_EQ(a.calendar_dates, b.calendar_dates);
  EXPECT_EQ(a.hac_lag, b.hac_lag);
  EXPECT_TRUE(same_bits(a.mean, b.mean));
  EXPECT_TRUE(same_bits(a.standard_error, b.standard_error));
  EXPECT_TRUE(same_bits(a.upper_abs_ic, b.upper_abs_ic));
  EXPECT_TRUE(same_bits(a.max_segment_abs_ic, b.max_segment_abs_ic));
  EXPECT_EQ(a.defined, b.defined);
  EXPECT_EQ(a.suggestive_direction, b.suggestive_direction);
}

struct S2Scored {
  ex::ResearchIcResult result;
  std::array<std::vector<f64>, 4> pearson, rank;
};

// One research IC pass of `signal` under `cfg` (all four horizons, endpoint presence required),
// with the return guard `bad` (empty: none).
atx::core::Result<S2Scored> s2_score(const Panel& p, const ex::IcScreenConfig& cfg,
                                     std::span<const f64> signal,
                                     const ex::ResearchIcTerminal& terminal = {},
                                     std::span<const atx::u32> bad = {}) {
  ATX_TRY(auto cache, ex::prepare_research_ic(p, cfg, {4, true, 1}, {}, bad, "close", terminal));
  ATX_TRY(auto scratch, ex::prepare_research_ic_scratch(cache));
  ATX_TRY(auto result, ex::evaluate_research_ic(signal, cache, scratch));
  S2Scored out{std::move(result), {}, {}};
  for (usize k = 0; k < 4U; ++k) {
    out.pearson[k].assign(scratch.pearson_series(k).begin(), scratch.pearson_series(k).end());
    out.rank[k].assign(scratch.rank_series(k).begin(), scratch.rank_series(k).end());
  }
  return atx::core::Ok(std::move(out));
}

// DS-4: the coverage fraction of every gate is IcScreenConfig::min_coverage, in (0, 1], default .8
// (the former constant). It keys the prepared cache and moves the gates, never the daily series.
TEST(IcScreen, MinCoverageConfig) {
  EXPECT_EQ(ex::kDefaultIcMinCoverage, 0.8);
  EXPECT_EQ(ex::IcScreenConfig{}.min_coverage, ex::kDefaultIcMinCoverage);
  constexpr usize dates = 160, names = 10;
  auto p = s2_panel(dates, names, s2_close(dates, names));
  ASSERT_TRUE(p);
  const auto cfg = s2_config();
  constexpr f64 kInf = std::numeric_limits<f64>::infinity();
  for (const f64 bad : {0.0, -0.25, 1.0 + 1e-12, 1.5, kS2NaN, kInf}) {
    auto refused = cfg;
    refused.min_coverage = bad;
    EXPECT_FALSE(ex::prepare_ic_screen(*p, refused)) << bad;
    EXPECT_FALSE(ex::prepare_research_ic(*p, refused, {4, true, 1})) << bad;
  }
  for (const f64 good : {1.0, 0.5, 1e-9}) {
    auto accepted = cfg;
    accepted.min_coverage = good;
    EXPECT_TRUE(ex::prepare_ic_screen(*p, accepted)) << good;
  }
  // The prepared cache binds the fraction (and the HAC rule).
  auto prepared = ex::prepare_ic_screen(*p, cfg);
  ASSERT_TRUE(prepared);
  EXPECT_TRUE(ex::ic_screen_cache_matches(*prepared, *p, cfg));
  auto other = cfg;
  other.min_coverage = 0.5;
  EXPECT_FALSE(ex::ic_screen_cache_matches(*prepared, *p, other));
  other = cfg;
  other.hac_rule = ex::IcHacRule::HorizonAwareV3;
  EXPECT_FALSE(ex::ic_screen_cache_matches(*prepared, *p, other));

  // Date-level gate: the signal is absent on 3 of every 10 decision dates, so each daily series
  // has ~70% valid dates (every quarter segment too): undefined at .8, defined at .5.
  std::vector<f64> signal(dates * names);
  for (usize d = 0; d < dates; ++d)
    for (usize i = 0; i < names; ++i)
      signal[d * names + i] = d % 10U < 3U ? kS2NaN : s2_noise(7919U + d * names + i);
  auto half = cfg;
  half.min_coverage = 0.5;
  const auto strict = s2_score(*p, cfg, signal);
  const auto loose = s2_score(*p, half, signal);
  ASSERT_TRUE(strict);
  ASSERT_TRUE(loose);
  for (usize k = 0; k < 4U; ++k) {
    SCOPED_TRACE(k);
    // Every paired row passes both row gates (all names paired): the series are the same bits.
    EXPECT_TRUE(same_series(strict->pearson[k], loose->pearson[k]));
    EXPECT_TRUE(same_series(strict->rank[k], loose->rank[k]));
    for (const auto* e : {&strict->result.screen.horizons[k].pearson,
                          &strict->result.screen.horizons[k].rank}) {
      const auto share = static_cast<f64>(e->valid_dates) / static_cast<f64>(e->calendar_dates);
      EXPECT_GT(share, 0.5);
      EXPECT_LT(share, 0.8);
      EXPECT_FALSE(e->defined);
    }
    EXPECT_TRUE(loose->result.screen.horizons[k].pearson.defined);
    EXPECT_TRUE(loose->result.screen.horizons[k].rank.defined);
  }
  EXPECT_EQ(strict->result.screen.reason, ex::IcScreenReason::InsufficientEvidence);

  // Row-level gate: on every date 3 of the 10 eligible names lack a signal (7 paired): every row
  // is refused at .8 (7 < 8) and scored at .5.
  for (usize d = 0; d < dates; ++d)
    for (usize i = 0; i < names; ++i)
      signal[d * names + i] = i < 3U ? kS2NaN : s2_noise(104729U + d * names + i);
  const auto rows_strict = s2_score(*p, cfg, signal);
  const auto rows_loose = s2_score(*p, half, signal);
  ASSERT_TRUE(rows_strict);
  ASSERT_TRUE(rows_loose);
  for (usize k = 0; k < 4U; ++k) {
    SCOPED_TRACE(k);
    ASSERT_FALSE(rows_strict->rank[k].empty());
    for (usize d = 0; d < rows_strict->rank[k].size(); ++d) {
      EXPECT_TRUE(std::isnan(rows_strict->rank[k][d])) << d;
      EXPECT_TRUE(std::isfinite(rows_loose->rank[k][d])) << d;
    }
    EXPECT_EQ(rows_strict->result.screen.horizons[k].rank.valid_dates, 0U);
    EXPECT_TRUE(rows_loose->result.screen.horizons[k].rank.defined);
  }
}

// DSL/IC review §3: the screen's HAC rule is named and selectable. Bartlett2hV1 (the default)
// keeps the pre-S2 arithmetic bit for bit; HorizonAwareV3 is eval/hac.hpp's mean_tstat on the
// same calendar-preserving influence series, scaled by T / n; estimate_daily_ic reproduces both.
TEST(IcScreen, HacRuleNamedSelectableAndReproduced) {
  EXPECT_EQ(ex::IcScreenConfig{}.hac_rule, ex::IcHacRule::Bartlett2hV1);
  EXPECT_EQ(ex::ic_hac_rule_name(ex::IcHacRule::Bartlett2hV1), "bartlett-2h-v1");
  EXPECT_EQ(ex::ic_hac_rule_name(ex::IcHacRule::HorizonAwareV3), "horizon-aware-v3");
  EXPECT_EQ(ex::ic_hac_rule_name(static_cast<ex::IcHacRule>(u8{9})), "unknown");
  constexpr usize dates = 300, names = 24;
  auto p = s2_panel(dates, names, s2_close(dates, names));
  ASSERT_TRUE(p);
  auto cfg = s2_config();
  cfg.horizons = {1, 3, 5, 10};
  auto unknown = cfg;
  unknown.hac_rule = static_cast<ex::IcHacRule>(u8{7});
  EXPECT_FALSE(ex::prepare_research_ic(*p, unknown, {4, true, 1}));
  auto aware = cfg;
  aware.hac_rule = ex::IcHacRule::HorizonAwareV3;
  // A signal with calendar gaps (every 11th decision date absent), so T != n.
  std::vector<f64> signal(dates * names);
  for (usize d = 0; d < dates; ++d)
    for (usize i = 0; i < names; ++i)
      signal[d * names + i] = d % 11U == 4U ? kS2NaN : s2_noise(31U + d * names + i);
  const auto bartlett = s2_score(*p, cfg, signal);
  const auto horizon = s2_score(*p, aware, signal);
  ASSERT_TRUE(bartlett);
  ASSERT_TRUE(horizon);
  usize defined = 0;
  for (usize k = 0; k < 4U; ++k) {
    const usize h = cfg.horizons[k];
    SCOPED_TRACE(h);
    EXPECT_TRUE(same_series(bartlett->pearson[k], horizon->pearson[k]));
    EXPECT_TRUE(same_series(bartlett->rank[k], horizon->rank[k]));
    const std::array<std::pair<const std::vector<f64>*, bool>, 2> metrics{
        {{&bartlett->pearson[k], false}, {&bartlett->rank[k], true}}};
    for (const auto& [series, is_rank] : metrics) {
      const std::span<const f64> daily(*series);
      const auto& b = is_rank ? bartlett->result.screen.horizons[k].rank
                              : bartlett->result.screen.horizons[k].pearson;
      const auto& a = is_rank ? horizon->result.screen.horizons[k].rank
                              : horizon->result.screen.horizons[k].pearson;
      expect_same_estimate(ex::estimate_daily_ic(daily, h, cfg), b);
      expect_same_estimate(ex::estimate_daily_ic(daily, h, aware), a);
      // The independent recomputation, in the screen's order of operations.
      f64 sum = 0.0;
      usize n = 0;
      for (const f64 v : daily)
        if (std::isfinite(v)) {
          sum += v;
          ++n;
        }
      ASSERT_GT(n, 0U);
      ASSERT_LT(n, daily.size());
      const f64 mean = sum / static_cast<f64>(n);
      std::vector<f64> work(daily.size());
      for (usize d = 0; d < daily.size(); ++d)
        work[d] = std::isfinite(daily[d]) ? daily[d] - mean : 0.0;
      const auto t_over_n = [&](f64 se) {
        return se * static_cast<f64>(daily.size()) / static_cast<f64>(n);
      };
      EXPECT_TRUE(same_bits(b.mean, mean));
      EXPECT_TRUE(same_bits(a.mean, mean));
      ASSERT_TRUE(b.defined);
      ASSERT_TRUE(a.defined);
      ++defined;
      const usize lag = std::max(h * 2U, hac::newey_west_rule_of_thumb_lag(daily.size()));
      const auto hac_se = hac::mean_inference(work, hac::Kernel::BartlettV1, lag, true);
      const auto iid_se = hac::mean_inference(work, hac::Kernel::BartlettV1, 0U, true);
      EXPECT_EQ(b.hac_lag, lag);
      EXPECT_TRUE(same_bits(b.standard_error, std::max(t_over_n(hac_se.se), t_over_n(iid_se.se))));
      const auto v3 = hac::mean_tstat(work, hac::TStatRule::HorizonAwareV3, h);
      ASSERT_TRUE(v3.defined);
      EXPECT_EQ(a.hac_lag, v3.lag);
      EXPECT_TRUE(same_bits(a.standard_error, t_over_n(v3.se)));
      EXPECT_TRUE(same_bits(a.upper_abs_ic,
                            std::abs(mean) + cfg.confidence_multiplier * t_over_n(v3.se)));
    }
  }
  EXPECT_EQ(defined, 8U);
}

// DSL/IC review §7 item 3 (LabelTerminalRule::ImputedV1). Name 0 leaves the scored panel after
// session L; the label-only panel prices it once more at L+1 (a role built with delisting returns:
// close[L] x (1 + r)). A decision whose holding [entry, entry + h] straddles L then has the label
// close_label[L+1] / close[entry] - 1 (exit at the imputed terminal close, cash after) instead of
// none; every other pair keeps its MissingExitV1 label bit for bit. Name 3's one-session hole has
// no label-panel price, so its missing exits stay missing. The terminal session may be the
// horizon's own end session (entry = L, h = 1): its label still reads the label panel's close.
TEST(IcLabels, TerminalImputedClosedForm) {
  constexpr usize dates = 14, names = 5, last = 7;
  constexpr f64 r = -0.4;
  std::vector<f64> close = s2_close(dates, names);
  std::vector<u8> scored_mask(dates * names, u8{1});
  for (usize d = last + 1U; d < dates; ++d) {
    scored_mask[d * names] = u8{0};
    close[d * names] = kS2NaN;
  }
  scored_mask[4U * names + 3U] = u8{0};
  close[4U * names + 3U] = kS2NaN;
  std::vector<f64> label_close = close;
  std::vector<u8> label_mask = scored_mask;
  const f64 terminal_close = close[last * names] * (1.0 + r);
  label_close[(last + 1U) * names] = terminal_close;
  label_mask[(last + 1U) * names] = u8{1};
  auto scored = s2_panel(dates, names, close, scored_mask);
  auto label = s2_panel(dates, names, label_close, label_mask);
  ASSERT_TRUE(scored);
  ASSERT_TRUE(label);
  const auto cfg = s2_config();
  std::vector<f64> signal(dates * names);
  for (usize k = 0; k < signal.size(); ++k) signal[k] = s2_noise(5U + k);

  const ex::ResearchIcTerminal imputed{ex::LabelTerminalRule::ImputedV1, &*label, "close"};
  auto legacy_cache = ex::prepare_research_ic(*scored, cfg, {4, true, 1});
  auto imputed_cache =
      ex::prepare_research_ic(*scored, cfg, {4, true, 1}, {}, {}, "close", imputed);
  // MissingExitV1 named explicitly, with a label panel beside it, ignores that panel.
  auto named_cache = ex::prepare_research_ic(
      *scored, cfg, {4, true, 1}, {}, {}, "close",
      ex::ResearchIcTerminal{ex::LabelTerminalRule::MissingExitV1, &*label, "close"});
  ASSERT_TRUE(legacy_cache);
  ASSERT_TRUE(imputed_cache);
  ASSERT_TRUE(named_cache);
  const auto legacy = s2_score(*scored, cfg, signal);
  const auto with_terminal = s2_score(*scored, cfg, signal, imputed);
  ASSERT_TRUE(legacy);
  ASSERT_TRUE(with_terminal);
  for (usize k = 0; k < 4U; ++k) {
    const usize h = cfg.horizons[k];
    SCOPED_TRACE(h);
    const auto old_labels = legacy_cache->labels(k);
    const auto new_labels = imputed_cache->labels(k);
    EXPECT_TRUE(same_series(old_labels, named_cache->labels(k)));
    ASSERT_EQ(old_labels.size(), new_labels.size());
    ASSERT_EQ(imputed_cache->first_date(), 0U);
    usize imputed_cells = 0;
    for (usize row = 0; row < imputed_cache->label_rows(k); ++row) {
      const usize entry = row + cfg.execution_delay;
      for (usize i = 0; i < names; ++i) {
        const usize cell = row * names + i;
        const bool straddles = i == 0U && entry <= last && last < entry + h;
        if (!straddles) {
          EXPECT_TRUE(same_bits(old_labels[cell], new_labels[cell])) << row << ',' << i;
          continue;
        }
        ++imputed_cells;
        EXPECT_TRUE(std::isnan(old_labels[cell])) << row;
        EXPECT_TRUE(same_bits(new_labels[cell], terminal_close / close[entry * names] - 1.0))
            << row;
      }
    }
    // Closed form: decisions d with entry = d + 1 in [L - h + 1, L], one per bar of the horizon.
    EXPECT_EQ(imputed_cells, h);
    const auto& before = legacy->result.coverage[k];
    const auto& after = with_terminal->result.coverage[k];
    EXPECT_EQ(before.terminal_imputed_pairs, 0U);
    EXPECT_EQ(after.terminal_imputed_pairs, static_cast<u64>(h));
    EXPECT_EQ(after.finite_label_pairs - before.finite_label_pairs, after.terminal_imputed_pairs);
    // A subset of the missing exits, which the rule does not recount; name 3's hole stays missing.
    EXPECT_EQ(after.missing_exit_pairs, before.missing_exit_pairs);
    EXPECT_EQ(after.missing_entry_pairs, before.missing_entry_pairs);
    EXPECT_EQ(after.decision_eligible_pairs, before.decision_eligible_pairs);
    EXPECT_GT(after.missing_exit_pairs, after.terminal_imputed_pairs);
    EXPECT_EQ(after.guard_excluded_pairs + after.invalid_price_pairs + after.nonfinite_return_pairs,
              0U);
  }
  EXPECT_EQ(ex::label_terminal_rule_name(ex::LabelTerminalRule::MissingExitV1), "missing-exit-v1");
  EXPECT_EQ(ex::label_terminal_rule_name(ex::LabelTerminalRule::ImputedV1), "imputed-v1");

  // Geometry and binding refusals: no label panel, other axes, no price field, unknown rule.
  EXPECT_FALSE(ex::prepare_research_ic(*scored, cfg, {4, true, 1}, {}, {}, "close",
                                       ex::ResearchIcTerminal{ex::LabelTerminalRule::ImputedV1,
                                                              nullptr, "close"}));
  auto longer = s2_panel(dates + 1U, names, s2_close(dates + 1U, names));
  auto wider = s2_panel(dates, names + 1U, s2_close(dates, names + 1U));
  auto renamed = s2_panel(dates, names, label_close, label_mask, "px");
  ASSERT_TRUE(longer);
  ASSERT_TRUE(wider);
  ASSERT_TRUE(renamed);
  for (const Panel* other : {&*longer, &*wider, &*renamed}) {
    EXPECT_FALSE(ex::prepare_research_ic(*scored, cfg, {4, true, 1}, {}, {}, "close",
                                         ex::ResearchIcTerminal{ex::LabelTerminalRule::ImputedV1,
                                                                other, "close"}));
  }
  EXPECT_FALSE(ex::prepare_research_ic(
      *scored, cfg, {4, true, 1}, {}, {}, "close",
      ex::ResearchIcTerminal{static_cast<ex::LabelTerminalRule>(u8{9}), &*label, "close"}));
}

// Fix round 1 (review S2 #1, #6): an imputed pair passes the return guard of BOTH roles over
// (entry, T]: the scored guard (its steps up to L) and the label role's (which alone sees the
// terminal step L -> T). The fixture of TerminalImputedClosedForm (L = 7, T = 8) with
//   (A) one bad scored return on name 0 at session 6 (in both guards, as the label role's guard
//       computes it on the same cells): a straddling pair imputes only when entry >= 6, else it is
//       guard-excluded, and every other label is the guarded MissingExitV1 label;
//   (B) a bad terminal step in the label guard alone: every straddling pair is guard-excluded and
//       nothing is imputed.
// A guard given without the label guard, or a decreasing label guard, refuses.
TEST(IcLabels, TerminalImputedPairsPassBothReturnGuards) {
  constexpr usize dates = 14, names = 5, last = 7, bad_session = 6;
  std::vector<f64> close = s2_close(dates, names);
  std::vector<u8> scored_mask(dates * names, u8{1});
  for (usize d = last + 1U; d < dates; ++d) {
    scored_mask[d * names] = u8{0};
    close[d * names] = kS2NaN;
  }
  std::vector<f64> label_close = close;
  std::vector<u8> label_mask = scored_mask;
  const f64 terminal_close = close[last * names] * 0.7;
  label_close[(last + 1U) * names] = terminal_close;
  label_mask[(last + 1U) * names] = u8{1};
  auto scored = s2_panel(dates, names, close, scored_mask);
  auto label = s2_panel(dates, names, label_close, label_mask);
  ASSERT_TRUE(scored);
  ASSERT_TRUE(label);
  const auto cfg = s2_config();
  std::vector<f64> signal(dates * names);
  for (usize k = 0; k < signal.size(); ++k) signal[k] = s2_noise(11U + k);
  // Prefix guards of name 0: one bad return counted from session `from` on.
  const auto guard_from = [&](usize from) {
    std::vector<atx::u32> bad(dates * names, 0U);
    for (usize d = from; d < dates; ++d) bad[d * names] = 1U;
    return bad;
  };
  const auto scored_bad = guard_from(bad_session);
  const std::vector<atx::u32> clean(dates * names, 0U);
  const auto terminal_bad = guard_from(last + 1U);
  const auto with = [&](std::span<const atx::u32> label_bad) {
    return ex::ResearchIcTerminal{ex::LabelTerminalRule::ImputedV1, &*label, "close", label_bad};
  };
  const auto legacy_a = s2_score(*scored, cfg, signal, {}, scored_bad);
  const auto imputed_a = s2_score(*scored, cfg, signal, with(scored_bad), scored_bad);
  const auto legacy_b = s2_score(*scored, cfg, signal, {}, clean);
  const auto imputed_b = s2_score(*scored, cfg, signal, with(terminal_bad), clean);
  ASSERT_TRUE(legacy_a);
  ASSERT_TRUE(imputed_a);
  ASSERT_TRUE(legacy_b);
  ASSERT_TRUE(imputed_b);
  auto cache_a = ex::prepare_research_ic(*scored, cfg, {4, true, 1}, {}, scored_bad, "close",
                                         with(scored_bad));
  auto legacy_cache_a = ex::prepare_research_ic(*scored, cfg, {4, true, 1}, {}, scored_bad);
  ASSERT_TRUE(cache_a);
  ASSERT_TRUE(legacy_cache_a);
  for (usize k = 0; k < 4U; ++k) {
    const usize h = cfg.horizons[k];
    SCOPED_TRACE(h);
    // The straddling entries [L - h + 1, L]: imputed from the bad session on, guarded before it.
    usize kept = 0, guarded = 0;
    for (usize entry = last + 1U - h; entry <= last; ++entry) {
      if (entry >= bad_session) ++kept;
      else ++guarded;
    }
    // (A)
    const auto& a0 = legacy_a->result.coverage[k];
    const auto& a1 = imputed_a->result.coverage[k];
    EXPECT_EQ(a1.terminal_imputed_pairs, static_cast<u64>(kept));
    EXPECT_EQ(a1.guard_excluded_pairs - a0.guard_excluded_pairs, static_cast<u64>(guarded));
    EXPECT_EQ(a1.finite_label_pairs - a0.finite_label_pairs, static_cast<u64>(kept));
    const auto old_labels = legacy_cache_a->labels(k);
    const auto new_labels = cache_a->labels(k);
    ASSERT_EQ(old_labels.size(), new_labels.size());
    for (usize row = 0; row < cache_a->label_rows(k); ++row) {
      const usize entry = row + cfg.execution_delay;
      for (usize i = 0; i < names; ++i) {
        const usize cell = row * names + i;
        if (i == 0U && entry <= last && last < entry + h && entry >= bad_session) {
          EXPECT_TRUE(same_bits(new_labels[cell], terminal_close / close[entry * names] - 1.0))
              << row;
        } else {
          EXPECT_TRUE(same_bits(old_labels[cell], new_labels[cell])) << row << ',' << i;
        }
      }
    }
    // (B)
    const auto& b0 = legacy_b->result.coverage[k];
    const auto& b1 = imputed_b->result.coverage[k];
    EXPECT_EQ(b1.terminal_imputed_pairs, 0U);
    EXPECT_EQ(b1.guard_excluded_pairs - b0.guard_excluded_pairs, static_cast<u64>(h));
    EXPECT_EQ(b1.finite_label_pairs, b0.finite_label_pairs);
    EXPECT_TRUE(same_series(legacy_b->rank[k], imputed_b->rank[k]));
  }
  // Refusals: a scored guard without the label guard; a decreasing label guard.
  EXPECT_FALSE(ex::prepare_research_ic(*scored, cfg, {4, true, 1}, {}, scored_bad, "close",
                                       with({})));
  auto decreasing = terminal_bad;
  decreasing[(dates - 1U) * names] = 0U;
  EXPECT_FALSE(ex::prepare_research_ic(*scored, cfg, {4, true, 1}, {}, clean, "close",
                                       with(decreasing)));
}
} // namespace atx_test_ic_screen_s2
