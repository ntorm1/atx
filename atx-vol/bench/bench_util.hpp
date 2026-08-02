#pragma once

// Shared helpers for the atx-vol Google Benchmark suites.
//
//  * apply_common(b)   — the mandatory per-registration knobs every atx-vol
//                        benchmark must carry (P0.1): >=0.5 s warm-up, 5
//                        repetitions, per-repetition rows kept (ReportAggregatesOnly
//                        false), and two custom statistics on top of Google
//                        Benchmark's median/mean/stddev: p95 and CV
//                        (coefficient of variation = stddev/mean).
//  * counters_(...)    — dump the opt-in ATX_VOL_COUNTERS algorithm counters into
//                        the JSON as cnt_* columns. Compiles to nothing when the
//                        counters are OFF (the default) via `if constexpr`.
//
// The p95 / CV statistics receive the per-repetition timing values (in the
// benchmark's time unit) that ReportAggregatesOnly(false) + Repetitions(5)
// produce, so the JSON carries a `_p95` and `_cv` aggregate row per case.

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <vector>

#include <benchmark/benchmark.h>

#include "atx/vol/detail/counters.hpp"

namespace atx::vol::bench {

// 95th percentile (linear interpolation between order statistics). Google
// Benchmark hands us the per-repetition values; an empty vector reads 0.
[[nodiscard]] inline double stat_p95(const std::vector<double>& v) {
  if (v.empty()) {
    return 0.0;
  }
  std::vector<double> s(v);
  std::sort(s.begin(), s.end());
  const double rank = 0.95 * static_cast<double>(s.size() - 1);
  const auto lo = static_cast<std::size_t>(std::floor(rank));
  const auto hi = static_cast<std::size_t>(std::ceil(rank));
  const double frac = rank - static_cast<double>(lo);
  return s[lo] + (s[hi] - s[lo]) * frac;
}

// Coefficient of variation = sample stddev / mean (dimensionless). The gate in
// compare_baseline.py reads this to decide NOISY (CV > 5%) vs comparable.
[[nodiscard]] inline double stat_cv(const std::vector<double>& v) {
  if (v.size() < 2) {
    return 0.0;
  }
  double mean = 0.0;
  for (const double x : v) {
    mean += x;
  }
  mean /= static_cast<double>(v.size());
  if (mean == 0.0) {
    return 0.0;
  }
  double acc = 0.0;
  for (const double x : v) {
    const double d = x - mean;
    acc += d * d;
  }
  const double var = acc / static_cast<double>(v.size() - 1);
  return std::sqrt(var) / std::abs(mean);
}

// The knobs the brief mandates on EVERY registration. Callers may chain further
// (Args/ArgsProduct/Unit/UseRealTime) on the returned pointer.
inline benchmark::internal::Benchmark* apply_common(benchmark::internal::Benchmark* b) {
  return b->MinWarmUpTime(0.5)
      ->Repetitions(5)
      ->ReportAggregatesOnly(false)
      ->ComputeStatistics("p95", stat_p95)
      // NOTE: registering a StatisticUnit::kPercentage statistic makes Google
      // Benchmark emit TWO identical "cv" aggregate rows per case in the JSON
      // (a quirk of that unit; "p95" above emits only one). Harmless — both
      // rows carry the same value, so compare_baseline.py's dict-by-name
      // parse just overwrites one with the other — but don't be surprised by
      // the duplicate line if you're eyeballing raw JSON output.
      ->ComputeStatistics("cv", stat_cv, benchmark::StatisticUnit::kPercentage);
}

// Dump the algorithm counters as cnt_* columns, measured EXACTLY over a single
// representative operation run OUTSIDE the timed loop (so warm-up iterations do
// not inflate the per-op count). No-op — and no columns — when the counters are
// compiled out, so the OFF build's JSON is unchanged.
template <class Fn>
inline void dump_counters(benchmark::State& state, Fn&& one_op) {
  if constexpr (atx::vol::counters::counters_enabled()) {
    atx::vol::counters::reset();
    one_op();
    const auto snap = atx::vol::counters::snapshot();
    for (unsigned i = 0; i < atx::vol::counters::kCount; ++i) {
      state.counters[atx::vol::counters::kNames[i]] =
          static_cast<double>(snap.values[i]);
    }
  } else {
    (void)state;
    (void)one_op;
  }
}

}  // namespace atx::vol::bench
