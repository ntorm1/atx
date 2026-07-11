// american_pricing_bench.cpp — Google Benchmark matrix for the American pricer.
//
// The matrix is route x side x API over a moneyness/maturity/vol grid, generated
// data-driven (nested loops over grid + scenario tables register one case each —
// no copy-pasted BENCHMARK blocks). Every case reports contracts/s (rate),
// ns/option, and fallback_rate via benchmark::Counter, carries the mandated
// >=0.5 s warm-up + 5 repetitions + p95/CV custom statistics (bench_util.hpp),
// and DoNotOptimize's its result.
//
// ── Regime safety (Task 1 interface) ─────────────────────────────────────
// Every registered (r, q, side) is asserted to be American or European at
// REGISTRATION time via detail::classify_regime (a call maps to an internal put
// with rate=q, yield=r). An Unsupported (negative-rate double-continuation)
// point aborts the process — it is never silently skipped, because a benchmark of
// an error return measures nothing. r<0 is therefore never registered; the
// European short-circuit (a no-dividend American call, which equals its European
// value) IS registered, since it is a genuine fast path.
//
// ── Grid trim (documented; see report) ───────────────────────────────────
// The spec's full 5-moneyness x 4-maturity x 5-vol product (100 pts) for the two
// cheapest cases (fast-cold price, cached price) is trimmed to a 3 x 4 x 3 = 36-pt
// product: moneyness {0.80, 1.00, 1.20} (the 0.95/1.05 near-wings dropped), vol
// {0.05, 0.30, 1.50} (the 0.15/0.75 mid-points dropped), maturities kept in full.
// The expensive Greek/analytic/delta routes and the accurate-cold price use an
// 8-pt subset (ATM + one wing) x 2 maturities x 2 vols, per the spec. The trim
// exists because the mandated 0.5 s warm-up x 5 repetitions costs ~2.7 s/point, so
// the untrimmed matrix would exceed the ~10 min/target wall-time budget. The
// report row points (ATM=1.00, 1/12, 30%) are retained in BOTH grids.

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <benchmark/benchmark.h>

#include "atx/vol/american.hpp"
#include "atx/vol/correction.hpp"
#include "atx/vol/types.hpp"

#include "bench_util.hpp"

namespace {

using atx::vol::AlOpts;
using atx::vol::al_fast_opts;
using atx::vol::AmericanMethod;
using atx::vol::andersen_lake;
using atx::vol::andersen_lake_call_slice;
using atx::vol::andersen_lake_put_slice;
using atx::vol::american_delta;
using atx::vol::american_greeks;
using atx::vol::american_greeks_al;
using atx::vol::american_greeks_fd;
using atx::vol::american_price_cached;
using atx::vol::CorrectionCache;
using atx::vol::Side;
using atx::vol::bench::apply_common;
using atx::vol::bench::dump_counters;

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
constexpr double kStrike = 100.0;  // K fixed; S = moneyness * K

// ── Grid ───────────────────────────────────────────────────────────────
struct GridPoint {
  double moneyness;  // S/K
  double T;
  double sigma;
};

constexpr double kT_1d = 1.0 / 252.0;
constexpr double kT_1m = 1.0 / 12.0;

// Reduced full product (36) for the two cheapest cases.
[[nodiscard]] std::vector<GridPoint> grid_full() {
  std::vector<GridPoint> g;
  for (const double m : {0.80, 1.00, 1.20}) {
    for (const double T : {kT_1d, kT_1m, 0.5, 2.0}) {
      for (const double s : {0.05, 0.30, 1.50}) {
        g.push_back(GridPoint{m, T, s});
      }
    }
  }
  return g;
}

// Greek/analytic/delta subset (8): ATM + one wing, 2 maturities, 2 vols.
[[nodiscard]] std::vector<GridPoint> grid_subset() {
  std::vector<GridPoint> g;
  for (const double m : {1.00, 1.20}) {
    for (const double T : {kT_1m, 0.5}) {
      for (const double s : {0.30, 0.75}) {
        g.push_back(GridPoint{m, T, s});
      }
    }
  }
  return g;
}

// ── Scenario ─────────────────────────────────────────────────────────────
enum class Api {
  AndersenLakePrice,
  PriceCached,
  GreeksCached,  // american_greeks — the cached bundle nothing measured before
  GreeksFd,
  GreeksAl,
  Delta,
};

struct Scenario {
  const char* api_name;
  const char* route_name;
  const char* side_name;
  Api api;
  Side side;
  double r;
  double q;
  std::optional<AlOpts> opts;       // cold-route accuracy preset (nullopt=accurate)
  const CorrectionCache* cache;     // populated for cached routes, else nullptr
};

// Built-once correction caches for the two American sides we cache. Production
// dims 16 x 8 x 12; box covers the whole grid (k_log ~ [-0.3,0.3], T in
// [1/252,2], sigma in [0.05,1.5]). Sampled with al_fast_opts (session cadence).
[[nodiscard]] const CorrectionCache* cache_for(Side side, double r, double q) {
  auto build = [&](Side s, double rr, double qq) -> CorrectionCache {
    auto res = CorrectionCache::build(/*n_k=*/16, /*n_T=*/8, /*n_s=*/12, rr, qq,
                                      /*k_log_min=*/-0.5, /*k_log_max=*/0.5,
                                      /*T_min=*/kT_1d * 0.5, /*T_max=*/2.5,
                                      /*sigma_min=*/0.03, /*sigma_max=*/1.6, s,
                                      al_fast_opts());
    if (!res) {
      std::fprintf(stderr, "FATAL: correction-cache build failed for the bench\n");
      std::abort();
    }
    return std::move(*res);
  };
  if (side == Side::Put) {
    static const CorrectionCache put_cache = build(Side::Put, r, q);
    return &put_cache;
  }
  static const CorrectionCache call_cache = build(Side::Call, r, q);
  return &call_cache;
}

// One American valuation at the grid point; NaN signals failure/fallback so the
// timed loop and the counter probe share one code path.
[[nodiscard]] double do_one(const Scenario& sc, const GridPoint& g) {
  const double S = g.moneyness * kStrike;
  const double T = g.T;
  const double sigma = g.sigma;
  switch (sc.api) {
    case Api::AndersenLakePrice: {
      const auto r = andersen_lake(S, kStrike, T, sigma, sc.r, sc.q, sc.side, sc.opts);
      return r ? *r : kNaN;
    }
    case Api::PriceCached:
      return american_price_cached(S, kStrike, T, sigma, sc.r, sc.q, sc.side, sc.cache);
    case Api::GreeksCached: {
      const auto r = american_greeks(S, kStrike, T, sigma, sc.r, sc.q, sc.side, sc.cache);
      return r ? r->price : kNaN;
    }
    case Api::GreeksFd: {
      const auto r = american_greeks_fd(S, kStrike, T, sigma, sc.r, sc.q, sc.side,
                                        AmericanMethod::AndersenLake, sc.opts, false);
      return r ? r->price : kNaN;
    }
    case Api::GreeksAl: {
      const auto r = american_greeks_al(S, kStrike, T, sigma, sc.r, sc.q, sc.side, sc.opts);
      return r ? r->price : kNaN;
    }
    case Api::Delta: {
      const auto r = american_delta(S, kStrike, T, sigma, sc.r, sc.q, sc.side,
                                    AmericanMethod::AndersenLake, sc.opts);
      return r ? *r : kNaN;
    }
  }
  return kNaN;
}

void run_point(benchmark::State& state, Scenario sc, GridPoint g) {
  std::uint64_t fallbacks = 0;
  for (auto _ : state) {
    double v = do_one(sc, g);
    benchmark::DoNotOptimize(v);
    if (!std::isfinite(v)) {
      ++fallbacks;
    }
  }
  const double iters = static_cast<double>(state.iterations());
  state.SetItemsProcessed(state.iterations());
  state.counters["contracts_per_s"] = benchmark::Counter(iters, benchmark::Counter::kIsRate);
  state.counters["ns_per_option"] =
      benchmark::Counter(iters * 1e-9, benchmark::Counter::kIsRate | benchmark::Counter::kInvert);
  state.counters["fallback_rate"] =
      benchmark::Counter(static_cast<double>(fallbacks), benchmark::Counter::kAvgIterations);
  dump_counters(state, [&] { benchmark::DoNotOptimize(do_one(sc, g)); });
}

// Registration-time regime guard — fires in Release too (assert() would be
// compiled out under NDEBUG). A call maps to an internal put (rate=q, yield=r).
void require_supported(const Scenario& sc) {
  const double rate = (sc.side == Side::Put) ? sc.r : sc.q;
  const double yield = (sc.side == Side::Put) ? sc.q : sc.r;
  if (atx::vol::detail::classify_regime(rate, yield) ==
      atx::vol::detail::ExerciseRegime::Unsupported) {
    std::fprintf(stderr,
                 "FATAL: bench '%s/%s/%s' is an Unsupported (r<0 double-continuation) "
                 "regime — refusing to register (it would measure an error return).\n",
                 sc.api_name, sc.route_name, sc.side_name);
    std::abort();
  }
}

[[nodiscard]] std::string point_name(const Scenario& sc, const GridPoint& g) {
  char buf[192];
  std::snprintf(buf, sizeof buf, "amer/%s/%s/%s/m%.2f/T%.4f/s%.2f", sc.api_name,
                sc.route_name, sc.side_name, g.moneyness, g.T, g.sigma);
  return std::string(buf);
}

void register_scenario(const Scenario& sc, const std::vector<GridPoint>& grid) {
  require_supported(sc);
  for (const GridPoint& g : grid) {
    apply_common(benchmark::RegisterBenchmark(
        point_name(sc, g),
        [sc, g](benchmark::State& state) { run_point(state, sc, g); }))
        ->Unit(benchmark::kNanosecond);
  }
}

// ── Call-slice batch throughput (one boundary solve, many strikes) ────────
// Prices `batch` American call strikes at one sigma via andersen_lake_call_slice.
// Div-paying call (q>0) so the American early-exercise path runs. contracts/s
// counts every strike priced.
void run_slice(benchmark::State& state) {
  const auto batch = static_cast<std::size_t>(state.range(0));
  const double S = 100.0;
  const double T = 0.5;
  const double sigma = 0.30;
  const double r = 0.043;
  const double q = 0.06;  // American call regime
  std::vector<double> strikes(batch);
  std::vector<double> out(batch);
  for (std::size_t i = 0; i < batch; ++i) {
    // Strikes spread around the forward.
    strikes[i] = 70.0 + 60.0 * (static_cast<double>(i) + 0.5) / static_cast<double>(batch);
  }
  std::uint64_t fallbacks = 0;
  for (auto _ : state) {
    const auto st = andersen_lake_call_slice(S, strikes, T, sigma, r, q, out, std::nullopt);
    benchmark::DoNotOptimize(out.data());
    if (!st) {
      ++fallbacks;
    }
  }
  const double n = static_cast<double>(batch);
  state.SetItemsProcessed(state.iterations() * static_cast<std::int64_t>(batch));
  state.counters["contracts_per_s"] =
      benchmark::Counter(static_cast<double>(state.iterations()) * n, benchmark::Counter::kIsRate);
  state.counters["ns_per_option"] = benchmark::Counter(
      static_cast<double>(state.iterations()) * n * 1e-9,
      benchmark::Counter::kIsRate | benchmark::Counter::kInvert);
  state.counters["fallback_rate"] =
      benchmark::Counter(static_cast<double>(fallbacks), benchmark::Counter::kAvgIterations);
  state.counters["batch"] = n;
  dump_counters(state, [&] {
    benchmark::DoNotOptimize(
        andersen_lake_call_slice(S, strikes, T, sigma, r, q, out, std::nullopt));
  });
}

// ── Put-slice batch throughput (one boundary solve reused by homogeneity) ──
// Prices `batch` American put strikes at one sigma via andersen_lake_put_slice:
// ONE boundary solve at the reference strike, reused across the ladder by strike
// homogeneity (rescale K + xmax, same y[]). American put regime (r>0, q=0).
void run_put_slice(benchmark::State& state) {
  const auto batch = static_cast<std::size_t>(state.range(0));
  const double S = 100.0;
  const double T = 0.5;
  const double sigma = 0.30;
  const double r = 0.043;
  const double q = 0.0;  // American put regime
  std::vector<double> strikes(batch);
  std::vector<double> out(batch);
  for (std::size_t i = 0; i < batch; ++i) {
    strikes[i] = 70.0 + 60.0 * (static_cast<double>(i) + 0.5) / static_cast<double>(batch);
  }
  std::uint64_t fallbacks = 0;
  for (auto _ : state) {
    const auto st = andersen_lake_put_slice(S, strikes, T, sigma, r, q, out, std::nullopt);
    benchmark::DoNotOptimize(out.data());
    if (!st) {
      ++fallbacks;
    }
  }
  const double n = static_cast<double>(batch);
  state.SetItemsProcessed(state.iterations() * static_cast<std::int64_t>(batch));
  state.counters["contracts_per_s"] =
      benchmark::Counter(static_cast<double>(state.iterations()) * n, benchmark::Counter::kIsRate);
  state.counters["ns_per_option"] = benchmark::Counter(
      static_cast<double>(state.iterations()) * n * 1e-9,
      benchmark::Counter::kIsRate | benchmark::Counter::kInvert);
  state.counters["fallback_rate"] =
      benchmark::Counter(static_cast<double>(fallbacks), benchmark::Counter::kAvgIterations);
  state.counters["batch"] = n;
  dump_counters(state, [&] {
    benchmark::DoNotOptimize(
        andersen_lake_put_slice(S, strikes, T, sigma, r, q, out, std::nullopt));
  });
}

// Baseline: the SAME put ladder priced per strike with a fresh cold boundary solve
// each (N solves), to quantify the put-slice's N->1 boundary-solve speedup.
void run_put_per_strike(benchmark::State& state) {
  const auto batch = static_cast<std::size_t>(state.range(0));
  const double S = 100.0;
  const double T = 0.5;
  const double sigma = 0.30;
  const double r = 0.043;
  const double q = 0.0;  // American put regime
  std::vector<double> strikes(batch);
  std::vector<double> out(batch);
  for (std::size_t i = 0; i < batch; ++i) {
    strikes[i] = 70.0 + 60.0 * (static_cast<double>(i) + 0.5) / static_cast<double>(batch);
  }
  std::uint64_t fallbacks = 0;
  for (auto _ : state) {
    for (std::size_t i = 0; i < batch; ++i) {
      const auto p = andersen_lake(S, strikes[i], T, sigma, r, q, Side::Put, std::nullopt);
      out[i] = p ? *p : (++fallbacks, 0.0);
    }
    benchmark::DoNotOptimize(out.data());
  }
  const double n = static_cast<double>(batch);
  state.SetItemsProcessed(state.iterations() * static_cast<std::int64_t>(batch));
  state.counters["contracts_per_s"] =
      benchmark::Counter(static_cast<double>(state.iterations()) * n, benchmark::Counter::kIsRate);
  state.counters["ns_per_option"] = benchmark::Counter(
      static_cast<double>(state.iterations()) * n * 1e-9,
      benchmark::Counter::kIsRate | benchmark::Counter::kInvert);
  state.counters["fallback_rate"] =
      benchmark::Counter(static_cast<double>(fallbacks), benchmark::Counter::kAvgIterations);
  state.counters["batch"] = n;
}

// ── Register the whole matrix (data-driven, at static-init) ──────────────
void register_all() {
  const std::vector<GridPoint> full = grid_full();
  const std::vector<GridPoint> sub = grid_subset();

  // Side (r,q) presets. Put & div-call are American; no-div-call is European.
  constexpr double kR = 0.043;
  const CorrectionCache* put_cache = cache_for(Side::Put, kR, 0.0);
  const CorrectionCache* call_cache = cache_for(Side::Call, kR, 0.06);

  const AlOpts fast = al_fast_opts();

  // ── andersen_lake raw price ─────────────────────────────────────────
  register_scenario({"andersen_lake", "fast_cold", "put", Api::AndersenLakePrice,
                     Side::Put, kR, 0.0, fast, nullptr},
                    full);                                   // cheapest #1: full grid
  register_scenario({"andersen_lake", "accurate_cold", "put", Api::AndersenLakePrice,
                     Side::Put, kR, 0.0, std::nullopt, nullptr},
                    sub);
  register_scenario({"andersen_lake", "fast_cold", "divcall", Api::AndersenLakePrice,
                     Side::Call, kR, 0.06, fast, nullptr},
                    sub);
  // European short-circuit: no-dividend American call == European (Black-76 fast path).
  register_scenario({"andersen_lake", "european_shortcircuit", "nodivcall",
                     Api::AndersenLakePrice, Side::Call, kR, 0.0, std::nullopt, nullptr},
                    sub);

  // ── american_price_cached (hot path) ────────────────────────────────
  register_scenario({"american_price_cached", "cached", "put", Api::PriceCached,
                     Side::Put, kR, 0.0, std::nullopt, put_cache},
                    full);                                   // cheapest #2: full grid
  register_scenario({"american_price_cached", "cached", "divcall", Api::PriceCached,
                     Side::Call, kR, 0.06, std::nullopt, call_cache},
                    sub);

  // ── american_greeks — the CACHED BUNDLE nothing measured before ─────
  register_scenario({"american_greeks", "cached", "put", Api::GreeksCached,
                     Side::Put, kR, 0.0, std::nullopt, put_cache},
                    sub);
  register_scenario({"american_greeks", "cached", "divcall", Api::GreeksCached,
                     Side::Call, kR, 0.06, std::nullopt, call_cache},
                    sub);

  // ── cold Greek routes (expensive; subset grid) ──────────────────────
  register_scenario({"american_greeks_fd", "fast_cold", "put", Api::GreeksFd,
                     Side::Put, kR, 0.0, fast, nullptr},
                    sub);
  register_scenario({"american_greeks_al", "fast_cold", "put", Api::GreeksAl,
                     Side::Put, kR, 0.0, fast, nullptr},
                    sub);
  register_scenario({"american_delta", "fast_cold", "put", Api::Delta,
                     Side::Put, kR, 0.0, fast, nullptr},
                    sub);

  // ── call-slice batch throughput ─────────────────────────────────────
  apply_common(benchmark::RegisterBenchmark("amer/andersen_lake_call_slice/divcall", run_slice))
      ->Arg(1)
      ->Arg(4)
      ->Arg(16)
      ->Arg(256)
      ->Arg(4096)
      ->Unit(benchmark::kNanosecond);

  // ── put-slice batch throughput + per-strike baseline (N->1 boundary solves) ─
  apply_common(benchmark::RegisterBenchmark("amer/andersen_lake_put_slice/put", run_put_slice))
      ->Arg(1)
      ->Arg(4)
      ->Arg(16)
      ->Arg(40)
      ->Arg(256)
      ->Arg(4096)
      ->Unit(benchmark::kNanosecond);
  apply_common(benchmark::RegisterBenchmark("amer/put_per_strike_baseline/put", run_put_per_strike))
      ->Arg(1)
      ->Arg(4)
      ->Arg(16)
      ->Arg(40)
      ->Arg(256)
      ->Arg(4096)
      ->Unit(benchmark::kNanosecond);
}

const bool kRegistered = [] {
  register_all();
  return true;
}();

}  // namespace
