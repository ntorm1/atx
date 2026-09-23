// replay_cost_bench.cpp -- scheduled-replay throughput with per-name cost models.
// MEASURED-ONLY: no correctness assertions. Globbed into atx-engine-bench.
//
//   BM_ReplaySqrtImpact/<names>/<days>  weekly rebalance of a dollar-neutral book
//     through SqrtImpactCost with a 10% ADV participation cap (working orders on).
//   BM_ReplayFlatBps/<names>/<days>     the same schedule on the aggregate
//     FlatBpsCost path (the historical trade_bps arithmetic), as the baseline.
//
// The panel, liquidity grid and targets are built once per case outside the
// timed loop; only replay_scheduled_targets is timed. Counters report
// name-days per second (names * days / wall time).

#include <cmath>
#include <limits>
#include <vector>

#include <benchmark/benchmark.h>

#include "atx/core/types.hpp"
#include "atx/engine/alpha/panel.hpp"
#include "atx/engine/book/replay.hpp"
#include "atx/engine/book/replay_cost.hpp"

namespace atx_bench_l8_replay_cost {

namespace book = atx::engine::book;
using atx::f64;
using atx::usize;
constexpr atx::i64 kDay = 86'400'000'000'000LL;
constexpr usize kRebalanceEvery = 5;

struct Fixture {
  atx::engine::alpha::Panel panel;
  std::vector<atx::i64> times;
  std::vector<usize> decisions;
  std::vector<f64> targets;
  std::vector<book::LiquidityRow> liquidity;
};

// Deterministic LCG in [0, 1); avoids <random> distribution variance across STLs.
struct Stream {
  atx::u64 state;
  f64 next() noexcept {
    state = state * 6364136223846793005ULL + 1442695040888963407ULL;
    return static_cast<f64>(state >> 11) * (1.0 / 9007199254740992.0);
  }
};

inline Fixture build(usize names, usize days) {
  Stream rng{0x5eedULL + names * 31U + days};
  std::vector<f64> close(names * days);
  std::vector<book::LiquidityRow> liquidity(names * days);
  for (usize i = 0; i < names; ++i) {
    f64 price = 20.0 + 80.0 * rng.next();
    const f64 adv = 2.0e5 * std::exp(6.0 * rng.next()); // 0.2m .. 80m ADV.
    for (usize d = 0; d < days; ++d) {
      price *= 1.0 + 0.02 * (rng.next() - 0.5);
      close[d * names + i] = price;
      liquidity[d * names + i] = book::LiquidityRow{adv, 0.02, 2.0 + 8.0 * rng.next()};
    }
  }
  Fixture fx{atx::engine::alpha::Panel::create(days, names, {"close"}, {std::move(close)}, {})
                 .value(),
             {}, {}, {}, std::move(liquidity)};
  for (usize d = 0; d < days; ++d) fx.times.push_back(static_cast<atx::i64>(d) * kDay);
  for (usize d = 0; d + 1 < days; d += kRebalanceEvery) fx.decisions.push_back(d);
  fx.targets.resize(fx.decisions.size() * names);
  const f64 gross = 2.0 / static_cast<f64>(names); // 100% long, 100% short.
  for (usize k = 0; k < fx.targets.size(); ++k) {
    fx.targets[k] = (rng.next() < 0.5 ? -1.0 : 1.0) * gross * (0.5 + rng.next());
  }
  return fx;
}

void run(benchmark::State &state, const book::ReplayCostModel &model, bool liquidity) {
  const auto names = static_cast<usize>(state.range(0));
  const auto days = static_cast<usize>(state.range(1));
  const auto fx = build(names, days);
  book::ReplayConfig cfg;
  cfg.initial_nav = 1.0e8;
  cfg.execution_delay_periods = 1;
  cfg.cost_model = &model;
  if (liquidity) cfg.liquidity = fx.liquidity;
  usize trades = 0;
  for (auto _ : state) {
    auto result = book::replay_scheduled_targets(fx.panel, fx.times, fx.decisions, fx.targets,
                                                 cfg);
    if (!result) state.SkipWithError(result.error().message().c_str());
    trades = result ? result->trades.size() : 0;
    benchmark::DoNotOptimize(result);
  }
  state.counters["name_days_per_s"] = benchmark::Counter(
      static_cast<f64>(names * days), benchmark::Counter::kIsIterationInvariantRate);
  state.counters["trades"] = static_cast<f64>(trades);
}

void BM_ReplaySqrtImpact(benchmark::State &state) {
  const auto model = book::SqrtImpactCost::create({1.0, 0.5}, 0.10).value();
  run(state, model, true);
}

void BM_ReplayFlatBps(benchmark::State &state) {
  const auto model = book::FlatBpsCost::create(5.0).value();
  run(state, model, false);
}

BENCHMARK(BM_ReplaySqrtImpact)
    ->Args({500, 2520})
    ->Args({3000, 2520})
    ->Unit(benchmark::kMillisecond)
    ->Iterations(3);
BENCHMARK(BM_ReplayFlatBps)
    ->Args({500, 2520})
    ->Args({3000, 2520})
    ->Unit(benchmark::kMillisecond)
    ->Iterations(3);

} // namespace atx_bench_l8_replay_cost
