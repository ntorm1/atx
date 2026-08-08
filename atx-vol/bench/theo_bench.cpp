// THEO-4: batch label runner benchmarks.
//
//   bev/solve/126d_al_fast — a single solve_breakeven_vol call (bisection
//     root-find over bev_replay_pnl) on a synthetic 126-day GBM path, at the
//     BevSolveConfig default (al_fast_opts()). Anchors the per-label solve
//     cost the batch runner multiplies: each label re-prices/re-hedges an
//     American option at every one of the path's 126 closes, once per
//     bisection step (~15-20 bev_replay_pnl evaluations to the default
//     1e-4 sigma_tol), so one label is on the order of a few thousand
//     American solves. The perf-review plan puts this at ~0.2-0.3 s/label
//     at al_fast on a Release build; see this benchmark's own report entry
//     for the CPU + observed number (this suite normally runs Debug, which
//     is expected to differ materially -- see bench/README.md's Debug-vs-
//     Release caveat).
//   bev/batch/64jobs       — solve_breakeven_batch over 64 independent jobs
//     (n_threads=0, i.e. atx_auto_worker_count()) -- the deterministic
//     parallel fan-out this task adds over the single-label solve above.
//
// Both cases build their synthetic path(s) with the SAME ACT/365.25-
// calibrated GBM generator as tests/breakeven_test.cpp's synth_gbm_path
// (duplicated here rather than shared, since that helper lives in an
// anonymous namespace of a *_test.cpp, not a tests/support/ header this
// target could reach) -- see that file's comment for why the per-step
// variance is sigma^2/365.25, not the naive 1/252 trading-year convention:
// bev_replay_pnl prices/hedges on T = (expiry-ts)/365.25 days against a
// path timestamped one calendar day apart, and the 1/252 convention
// silently injects ~45% extra variance per unit of priced time against that
// grid.

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <random>
#include <vector>

#include <benchmark/benchmark.h>

#include "atx/vol/breakeven.hpp"

#include "bench_util.hpp"

namespace atx::vol::bench {
namespace {

constexpr std::int64_t kDayNs = 86'400'000'000'000LL;

// Mirrors tests/breakeven_test.cpp's synth_gbm_path exactly (see file banner
// above for the day-count rationale); duplicated rather than shared.
[[nodiscard]] std::vector<BevDayState> synth_gbm_path(double sigma, std::size_t n_days,
                                                      std::uint32_t seed, double s0 = 100.0,
                                                      double r = 0.0, double q = 0.0) {
  std::mt19937 rng(seed);
  std::normal_distribution<double> z(0.0, 1.0);
  const double dt = 1.0 / 365.25;
  const double sq = sigma * std::sqrt(dt);
  std::vector<BevDayState> p;
  p.reserve(n_days + 1);
  double s = s0;
  for (std::size_t i = 0; i <= n_days; ++i) {
    p.push_back(BevDayState{static_cast<std::int64_t>(i) * kDayNs, s, r, q});
    s *= std::exp((r - q - 0.5 * sigma * sigma) * dt + sq * z(rng));
  }
  return p;
}

[[nodiscard]] BevSpec atm_call_expiring_at(const std::vector<BevDayState> &p) {
  return BevSpec{p.front().s, p.back().ts_ns, Side::Call};
}

// ── bev/solve/126d_al_fast ─────────────────────────────────────────────────
void BM_BevSolve_126d_AlFast(benchmark::State &state) {
  const std::vector<BevDayState> path = synth_gbm_path(0.25, 126, 42u);
  const BevSpec spec = atm_call_expiring_at(path);
  for (auto _ : state) {
    const Result<BevLabel> lab = solve_breakeven_vol(path, spec, {}, {});
    if (!lab.has_value()) {
      state.SkipWithError(lab.error().to_string().c_str());
      return;
    }
    double sink = lab->sigma_be;
    benchmark::DoNotOptimize(sink);
  }
  state.SetItemsProcessed(state.iterations());
}

// ── bev/batch/64jobs ────────────────────────────────────────────────────────
void BM_BevBatch_64jobs(benchmark::State &state) {
  constexpr std::size_t kN = 64;
  std::vector<std::vector<BevDayState>> paths;
  std::vector<BevSpec> specs;
  paths.reserve(kN);
  specs.reserve(kN);
  for (std::size_t i = 0; i < kN; ++i) {
    paths.push_back(synth_gbm_path(0.25, 126, 1000u + static_cast<std::uint32_t>(i)));
    specs.push_back(atm_call_expiring_at(paths.back()));
  }
  std::vector<BevJob> jobs;
  jobs.reserve(kN);
  for (std::size_t i = 0; i < kN; ++i) {
    jobs.push_back(BevJob{.path = paths[i], .spec = specs[i], .dividends = {}});
  }

  for (auto _ : state) {
    const Result<BevLabelFrame> frame = solve_breakeven_batch(jobs, {}, /*n_threads=*/0);
    if (!frame.has_value()) {
      state.SkipWithError(frame.error().to_string().c_str());
      return;
    }
    const double *sink = frame->sigma_be.data();
    benchmark::DoNotOptimize(sink);
  }
  state.SetItemsProcessed(state.iterations() * static_cast<std::int64_t>(kN));
}

const int kRegistered = [] {
  apply_common(benchmark::RegisterBenchmark("bev/solve/126d_al_fast", BM_BevSolve_126d_AlFast))
      ->Unit(benchmark::kMillisecond);
  // UseRealTime(): solve_breakeven_batch fans out over parallel_for's own
  // jthread workers, so the calling thread's own CPU time (what Google
  // Benchmark measures by default) reads near-zero while it blocks in the
  // join barrier -- the same all-core case bench/README.md documents for
  // backtest/multiunderlier_straddle/steps. Real (wall) time is the only
  // metric that reflects the actual parallel cost here.
  apply_common(benchmark::RegisterBenchmark("bev/batch/64jobs", BM_BevBatch_64jobs))
      ->Unit(benchmark::kMillisecond)
      ->UseRealTime();
  return 0;
}();

} // namespace
} // namespace atx::vol::bench
