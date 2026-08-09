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

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <random>
#include <string_view>
#include <vector>

#include <benchmark/benchmark.h>

#include "atx/vol/american.hpp" // al_fast_opts, AmericanMethod
#include "atx/vol/breakeven.hpp"
#include "atx/vol/priced_surface.hpp" // PricedSurface, PricingContext
#include "atx/vol/surface_parity.hpp" // SliceContext
#include "atx/vol/theo.hpp"           // TheoEngine, compute_theo_sheet (Task 10)
#include "atx/vol/vol_curve.hpp"      // CurveSurface, EssviCurve
#include "atx/vol/vol_surface.hpp"    // EssviParams

#include "bench_util.hpp"

namespace atx::vol::bench {
namespace {

using atx::core::Ok;

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

// ── theo/sheet_200q (Task 10: batch perf pass + theo sheet convenience API) ─
//
// compute_theo_sheet over 200 queries -- a screening-sheet-sized batch, well
// under one kTheoMaxBatch (256) chunk, so this isolates per-query resolve +
// overlay cost with no chunk-boundary effects. One overlay is engaged with a
// small nonzero dvol on EVERY query (mirrors a live RvBlend lean, not the
// identity path -- with zero overlays engaged, price_theo has nothing to do:
// M2's identity branch reuses market_price unconditionally and never reaches
// the American reprice either way, which would make a true/false comparison
// meaningless). `price_theo=true` therefore pays a fresh cold Andersen-Lake
// reprice at the shifted theo_vol on every row -- the M1 (fused evaluate) and
// M2 (skip-scratch-when-empty) perf changes land on the RESOLVE/overlay side
// of this cost, not on that reprice, which is the same American-pricer cost
// bench/ANCHORS.md's published anchors already bound (~45,000 prices/s @
// Ryzen 9 5900, 1 core; "close to 100,000/s/CPU" algorithmic ceiling, SSRN
// 2547027). `price_theo=false` skips the reprice entirely (the vol-space-only
// screening path the header banner documents), isolating the resolve+overlay
// cost alone -- the ratio between the two variants is the reprice's own
// share of the sheet's total cost.

// A small nonzero dvol on every query -- deliberately NOT `make_rv_blend_overlay`
// (which needs an `RvPanel` this bench has no reason to fabricate): the point is a
// stable, engine-agnostic nonzero net adjustment so `price_theo=true` always takes
// the reprice branch, not a specific overlay's own cost.
class ConstantDvolOverlay final : public ITheoOverlay {
public:
  explicit ConstantDvolOverlay(double dvol) : dvol_(dvol) {}

  [[nodiscard]] std::string_view name() const noexcept override { return "bench_constant_dvol"; }

  [[nodiscard]] Status adjust(const TheoContext & /*ctx*/, std::span<const TheoQuery> queries,
                              std::span<OverlayAdjust> out) const override {
    for (std::size_t i = 0; i < queries.size(); ++i) {
      out[i] = OverlayAdjust{.dvol = dvol_, .band = 0.0};
    }
    return Ok();
  }

private:
  double dvol_;
};

// Cheap synthetic eSSVI surface (mirrors theo_test.cpp's make_fast_tier_surface
// recipe) -- cold tier by construction (no with_query_pricing call), so this
// exercises the SAME cold Andersen-Lake reprice route fair_value() itself takes.
[[nodiscard]] Result<PricedSurface> make_theo_bench_surface() {
  constexpr double kSpot = 100.0;
  constexpr double kRate = 0.03;
  CurveSurface curves;
  std::vector<SliceContext> context;
  std::uint16_t expiry_id = 0;
  for (const double term : {0.10, 0.25, 0.50, 1.00}) {
    EssviParams parameters{};
    parameters.theta = 0.04 + 0.01 * term;
    parameters.phi = 1.3;
    parameters.rho = -0.3;
    parameters.psi = 0.5;
    parameters.p = 0.5;
    parameters.lambda = 0.5;
    parameters.T = term;
    parameters.F = kSpot;
    parameters.expiry_id = expiry_id++;
    curves.push(std::make_unique<EssviCurve>(parameters, std::exp(-kRate * term)));
    context.push_back(SliceContext{term, kSpot, 0.0, 0.0, 50, 0});
  }
  PricingContext pricing;
  pricing.S = kSpot;
  pricing.r = kRate;
  pricing.now_ts_ns = 1'700'000'000'000'000'000LL;
  pricing.method = AmericanMethod::AndersenLake;
  pricing.al_opts = al_fast_opts();
  pricing.uid = 7;
  return PricedSurface::create(std::move(curves), std::move(context), pricing);
}

// 200 queries spanning every strike x tenor x side combination the fixture
// surface fits, cycled to exactly 200 -- a screening-sheet-sized batch.
[[nodiscard]] std::vector<TheoQuery> make_theo_bench_queries() {
  constexpr std::size_t kN = 200;
  const std::array<double, 5> strikes{85.0, 92.5, 100.0, 107.5, 115.0};
  const std::array<double, 4> tenors{0.10, 0.25, 0.50, 1.00};
  std::vector<TheoQuery> qs;
  qs.reserve(kN);
  for (std::size_t i = 0; i < kN; ++i) {
    const Side side = (i % 2 == 0) ? Side::Call : Side::Put;
    qs.push_back(TheoQuery{.strike = strikes[i % strikes.size()],
                           .tenor_years = tenors[i % tenors.size()],
                           .side = side});
  }
  return qs;
}

void BM_TheoSheet_200q(benchmark::State &state, bool price_theo) {
  const Result<PricedSurface> surface = make_theo_bench_surface();
  if (!surface.has_value()) {
    state.SkipWithError(surface.error().to_string().c_str());
    return;
  }
  const std::vector<TheoQuery> queries = make_theo_bench_queries();
  std::vector<std::unique_ptr<ITheoOverlay>> overlays;
  overlays.push_back(std::make_unique<ConstantDvolOverlay>(0.02));
  const Result<TheoEngine> engine =
      TheoEngine::create(std::move(overlays), TheoConfig{.price_theo = price_theo});
  if (!engine.has_value()) {
    state.SkipWithError(engine.error().to_string().c_str());
    return;
  }
  const TheoContext ctx{.surface = &*surface};

  for (auto _ : state) {
    const Result<std::vector<TheoValue>> sheet = compute_theo_sheet(ctx, *engine, queries);
    if (!sheet.has_value()) {
      state.SkipWithError(sheet.error().to_string().c_str());
      return;
    }
    benchmark::DoNotOptimize(sheet->data());
  }
  state.SetItemsProcessed(state.iterations() * static_cast<std::int64_t>(queries.size()));
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
  apply_common(benchmark::RegisterBenchmark(
                   "theo/sheet_200q/price_theo_true",
                   [](benchmark::State &state) { BM_TheoSheet_200q(state, /*price_theo=*/true); }))
      ->Unit(benchmark::kMicrosecond);
  apply_common(benchmark::RegisterBenchmark(
                   "theo/sheet_200q/price_theo_false",
                   [](benchmark::State &state) { BM_TheoSheet_200q(state, /*price_theo=*/false); }))
      ->Unit(benchmark::kMicrosecond);
  return 0;
}();

} // namespace
} // namespace atx::vol::bench
