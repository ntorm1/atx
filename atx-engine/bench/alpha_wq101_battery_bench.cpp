// alpha_wq101_battery_bench.cpp — Lane 2 throughput gate: the WQ101 battery
// (alpha/wq101_battery.hpp, 70 alphas) under every evaluation strategy.
//
//   Wq101_StrategyA/<threads>        parallel_evaluate: one Program per alpha, one
//                                    alpha per worker (today's path; CSE lost).
//   Wq101_StrategyB/<threads>        global_dag_evaluate over the union Program
//                                    (cross-alpha CSE + level-scheduled chunks).
//   Wq101_StrategyBFused/<threads>   the same over the fused union Program.
//   Wq101_WarmCache/<threads>        strategy B with a warm SubtreeCache (the GP
//                                    re-evaluation case; ~all nodes served).
//   Wq101_OpFamily/<family>          ns/cell of one representative program per
//                                    opcode family (0 element-wise, 1 fused
//                                    element-wise, 2 cross-section, 3 time-series).
//
// Counters: alpha_days_per_s and cells_per_s (rates; us/alpha-day = 1e6 / the
// former, ns/cell = 1e9 / the latter), cse_pct (Program::cache_hit_pct of the union),
// cache_hit_pct (warm pass). Panel: 2520 dates x ATX_WQ101_INSTRUMENTS (default 500;
// the plan's 3000 needs ~6x the memory and does not fit next to 7 other lanes on a
// 16 GB host — set the env var to reproduce it on a dedicated box). Threads sweep
// {1,2,4,8,16}. Release only (equity-rel + -Bench); Debug numbers are meaningless.

#include <cstdlib>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <benchmark/benchmark.h>

#include "atx/core/types.hpp"

#include "atx/engine/alpha/bytecode.hpp"
#include "atx/engine/alpha/fusion.hpp"
#include "atx/engine/alpha/panel.hpp"
#include "atx/engine/alpha/registry.hpp"
#include "atx/engine/alpha/subtree_cache.hpp"
#include "atx/engine/alpha/vm.hpp"
#include "atx/engine/alpha/wq101_battery.hpp"
#include "atx/engine/parallel/batch_eval.hpp"
#include "atx/engine/parallel/det_pool.hpp"
#include "atx/engine/parallel/global_dag_eval.hpp"

namespace atx_bench_l2_wq101 {

namespace alpha = atx::engine::alpha;
namespace par = atx::engine::parallel;

constexpr atx::usize kDates = 2520;

// ATX_WQ101_INSTRUMENTS, read once before any benchmark runs (single-threaded).
[[nodiscard]] atx::usize instruments() {
  std::string val;
#ifdef _WIN32
  char *buf = nullptr;
  std::size_t len = 0;
  if (_dupenv_s(&buf, &len, "ATX_WQ101_INSTRUMENTS") == 0 && buf != nullptr) {
    val = buf;
  }
  std::free(buf); // _dupenv_s allocates with malloc; free(nullptr) is a no-op
#else
  if (const char *env = std::getenv("ATX_WQ101_INSTRUMENTS"); env != nullptr) {
    val = env;
  }
#endif
  const long v = val.empty() ? 0L : std::strtol(val.c_str(), nullptr, 10);
  return v > 0 ? static_cast<atx::usize>(v) : atx::usize{500};
}

struct Fixture {
  alpha::Panel panel;
  alpha::Program union_prog;
  alpha::FusedProgram fused;
  std::vector<alpha::Program> per_alpha;
};

[[nodiscard]] const Fixture &fixture() {
  static const Fixture f = [] {
    static const alpha::Library lib;
    const auto all = alpha::wq101_alphas();
    const std::vector<std::string_view> srcs(all.begin(), all.end());
    Fixture x{alpha::make_wq101_panel(kDates, instruments()).value(),
              alpha::compile_batch(srcs, lib).value(), {}, {}};
    x.fused = alpha::fuse(x.union_prog).value();
    for (const std::string_view s : srcs) {
      x.per_alpha.push_back(alpha::compile_batch(std::vector<std::string_view>{s}, lib).value());
    }
    return x;
  }();
  return f;
}

void set_counters(benchmark::State &state, double extra_hit_pct = -1.0) {
  const Fixture &f = fixture();
  const double alphas = static_cast<double>(f.union_prog.roots.size());
  const double iters = static_cast<double>(state.iterations());
  // Rates (higher is better). us/alpha-day = 1e6 / alpha_days_per_s; ns/cell =
  // 1e9 / cells_per_s. The bench gate reads real_time.
  state.counters["alpha_days_per_s"] =
      benchmark::Counter(alphas * static_cast<double>(kDates) * iters, benchmark::Counter::kIsRate);
  state.counters["cells_per_s"] = benchmark::Counter(
      alphas * static_cast<double>(f.panel.cells()) * iters, benchmark::Counter::kIsRate);
  state.counters["cse_pct"] = f.union_prog.cache_hit_pct();
  if (extra_hit_pct >= 0.0) {
    state.counters["cache_hit_pct"] = extra_hit_pct;
  }
}

void Wq101_StrategyA(benchmark::State &state) {
  const Fixture &f = fixture();
  par::DetPool pool{static_cast<atx::usize>(state.range(0))};
  for (auto _ : state) {
    auto r = par::parallel_evaluate(f.per_alpha, f.panel, pool);
    benchmark::DoNotOptimize(r);
  }
  set_counters(state);
}

void Wq101_StrategyB(benchmark::State &state) {
  const Fixture &f = fixture();
  par::DetPool pool{static_cast<atx::usize>(state.range(0))};
  for (auto _ : state) {
    auto r = par::global_dag_evaluate(f.union_prog, f.panel, pool);
    benchmark::DoNotOptimize(r);
  }
  set_counters(state);
}

void Wq101_StrategyBFused(benchmark::State &state) {
  const Fixture &f = fixture();
  par::DetPool pool{static_cast<atx::usize>(state.range(0))};
  for (auto _ : state) {
    auto r = par::global_dag_evaluate(f.fused, f.panel, pool);
    benchmark::DoNotOptimize(r);
  }
  set_counters(state);
}

void Wq101_WarmCache(benchmark::State &state) {
  const Fixture &f = fixture();
  par::DetPool pool{static_cast<atx::usize>(state.range(0))};
  alpha::SubtreeCache cache{std::size_t{2} << 30};
  // A GP loop hashes its panel once per generation, not once per call.
  par::GlobalDagOptions opt;
  opt.panel_digest = alpha::panel_content_digest(f.panel);
  (void)par::global_dag_evaluate(f.union_prog, f.panel, pool, &cache, nullptr, opt); // warm
  const alpha::CacheStats before = cache.stats();
  for (auto _ : state) {
    auto r = par::global_dag_evaluate(f.union_prog, f.panel, pool, &cache, nullptr, opt);
    benchmark::DoNotOptimize(r);
  }
  const alpha::CacheStats after = cache.stats();
  const double h = static_cast<double>(after.hits - before.hits);
  const double m = static_cast<double>(after.misses - before.misses);
  set_counters(state, h + m > 0.0 ? 100.0 * h / (h + m) : 0.0);
}

// One representative program per opcode family, serial Engine, ns per output cell.
void Wq101_OpFamily(benchmark::State &state) {
  static const alpha::Library lib;
  const Fixture &f = fixture();
  static const std::vector<std::string_view> fams[] = {
      {"((close - open) / ((high - low) + 0.001)) * volume - vwap"},
      {"((close - open) / ((high - low) + 0.001)) * volume - vwap"},
      {"rank(close) + zscore(volume) + group_rank(returns, IndClass.sector)"},
      {"correlation(close, volume, 10) + ts_rank(close, 10) + ts_std(returns, 20)"}};
  const auto fam = static_cast<atx::usize>(state.range(0));
  const alpha::Program prog = alpha::compile_batch(fams[fam], lib).value();
  const alpha::FusedProgram fp = alpha::fuse(prog).value();
  alpha::Engine eng{f.panel};
  for (auto _ : state) {
    auto r = fam == 1 ? eng.evaluate(fp) : eng.evaluate(prog);
    benchmark::DoNotOptimize(r);
  }
  state.counters["cells_per_s"] = benchmark::Counter(
      static_cast<double>(f.panel.cells()) * static_cast<double>(state.iterations()),
      benchmark::Counter::kIsRate);
}

void thread_sweep(benchmark::internal::Benchmark *b) {
  for (const int t : {1, 2, 4, 8, 16}) {
    b->Arg(t);
  }
  b->UseRealTime()->Unit(benchmark::kMillisecond)->MinTime(1.0);
}

BENCHMARK(Wq101_StrategyA)->Apply(thread_sweep);
BENCHMARK(Wq101_StrategyB)->Apply(thread_sweep);
BENCHMARK(Wq101_StrategyBFused)->Apply(thread_sweep);
BENCHMARK(Wq101_WarmCache)->Arg(8)->UseRealTime()->Unit(benchmark::kMillisecond);
BENCHMARK(Wq101_OpFamily)->DenseRange(0, 3)->Unit(benchmark::kMillisecond);

} // namespace atx_bench_l2_wq101
