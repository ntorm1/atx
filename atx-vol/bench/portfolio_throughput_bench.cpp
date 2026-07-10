// portfolio_throughput_bench.cpp — Google Benchmark throughput for the
// PricedSurface queries and the PricedSurface-native PortfolioPricer.
//
// Measures, all on the data-free synthetic 64-underlying market + book fixture
// (bench/support/synth_book.hpp — see its header for why it is bench-local):
//
//   1. PricedSurface query throughput: iv / fair_value / greeks / greeks_analytic
//      / delta over a realistic strike ladder.
//   2. PortfolioPricer::price over the matrix { n_unique } x { dedup ratio } x
//      { n_threads } x { prices_only } x { analytic_greeks }.
//   3. PortfolioPricer::pnl_explain (base vs shifted surfaces).
//   4. A position-scatter-only benchmark: the uniques are priced ONCE outside the
//      timed region, and only the per-position scale+store is timed — isolating
//      the store-bandwidth path from the pricing kernel.
//   5. An explicit KERNEL FLOOR: the exact per-unique fused op price() runs
//      (PricedSurface::greeks — which already yields the price), NOT the example's
//      fair_value + greeks double-solve.
//
// Every price()/pnl row emits unique_contracts/s, positions/s AND bytes/s
// together (a positions/s figure is meaningless without its dedup ratio, so the
// ratio + n_unique are always present as counters too). Custom stats (p95, CV) and
// the >=0.5 s warm-up + 5 repetitions come from bench_util.hpp.
//
// KERNEL-FLOOR / correction-cache note: PortfolioPricer prices each unique through
// PricedSurface, which holds NO CorrectionCache — its Greeks are cold Andersen-Lake
// by construction (priced_surface.hpp). So the correct production floor for THIS
// API is the cold PricedSurface::greeks loop below; there is no populated-cache
// path to match here (that would be the eSSVI served-cache session path, a
// different API). This is exactly the distinction the old example floor blurred.

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <benchmark/benchmark.h>

#include "atx/vol/american.hpp"
#include "atx/vol/portfolio_pricer.hpp"
#include "atx/vol/priced_surface.hpp"
#include "atx/vol/types.hpp"

#include "bench_util.hpp"
#include "support/synth_book.hpp"

namespace {

using atx::vol::AmericanGreeks;
using atx::vol::OptionContract;
using atx::vol::Portfolio;
using atx::vol::PortfolioBuildOptions;
using atx::vol::PortfolioPricer;
using atx::vol::Position;
using atx::vol::PriceOptions;
using atx::vol::PricedSurface;
using atx::vol::Side;
using atx::vol::SurfaceSet;
using atx::vol::bench::apply_common;
using atx::vol::bench::dump_counters;
using atx::vol::bench::SynthMarket;

constexpr int kUnderlyings = 64;
constexpr int kSlices = 6;       // 64 uids x 6 slices x 7 strikes = 2688 uniques
constexpr int kConvexNodes = 40;

// Per-position frame widths (bytes/row) from portfolio_pricer.hpp.
constexpr double kPriceRowBytes = 101.0;  // 14 columns
constexpr double kPnlRowBytes = 141.0;    // 19 columns

// ── Shared fixtures (built once) ─────────────────────────────────────────
[[nodiscard]] const SynthMarket& market() {
  static const SynthMarket m =
      atx::vol::bench::build_market(kUnderlyings, kSlices, kConvexNodes);
  return m;
}

// PortfolioPricer per (n_unique, dedup ratio), built once and reused across the
// thread/mode variants so the 2.688M-position book is not rebuilt per case.
[[nodiscard]] const PortfolioPricer& pricer_for(std::size_t n_unique, std::size_t ratio) {
  struct Entry {
    std::size_t nu;
    std::size_t ratio;
    std::unique_ptr<PortfolioPricer> pr;
  };
  static std::vector<Entry> cache;
  for (const Entry& e : cache) {
    if (e.nu == n_unique && e.ratio == ratio) {
      return *e.pr;
    }
  }
  std::vector<Position> book =
      atx::vol::bench::make_book(kUnderlyings, kSlices, n_unique, ratio);
  PortfolioBuildOptions opts;
  opts.expected_unique_contracts = n_unique;
  auto pf = Portfolio::create(book, opts).value();
  cache.push_back(Entry{n_unique, ratio, std::make_unique<PortfolioPricer>(std::move(pf))});
  return *cache.back().pr;
}

// Emit the always-together dedup counters plus positions/s, unique_contracts/s and
// bytes/s. `row_bytes` is the per-position frame width the API materializes.
void emit_book_counters(benchmark::State& state, const PortfolioPricer& pr,
                        double row_bytes) {
  const double iters = static_cast<double>(state.iterations());
  const double n_pos = static_cast<double>(pr.portfolio().n_positions());
  const double n_uni = static_cast<double>(pr.portfolio().n_contracts());
  state.counters["unique_contracts_per_s"] =
      benchmark::Counter(n_uni * iters, benchmark::Counter::kIsRate);
  state.counters["positions_per_s"] =
      benchmark::Counter(n_pos * iters, benchmark::Counter::kIsRate);
  state.counters["bytes_per_s"] =
      benchmark::Counter(n_pos * row_bytes * iters, benchmark::Counter::kIsRate);
  // Never report a positions/s without its dedup context.
  state.counters["n_unique"] = n_uni;
  state.counters["n_positions"] = n_pos;
  state.counters["dedup_ratio"] = (n_uni > 0.0) ? n_pos / n_uni : 0.0;
}

// ── 1. PricedSurface query throughput ────────────────────────────────────
enum class Query { Iv, FairValue, Greeks, GreeksAnalytic, Delta };

void run_query(benchmark::State& state, Query which) {
  const PricedSurface& surf = market().base.front();  // uid 1 (convex/index)
  const double T = atx::vol::bench::slice_T(2);
  // A realistic 40-strike ladder around spot.
  std::vector<double> ladder;
  ladder.reserve(40);
  for (int i = 0; i < 40; ++i) {
    ladder.push_back(70.0 + 60.0 * (static_cast<double>(i) + 0.5) / 40.0);
  }
  for (auto _ : state) {
    for (const double K : ladder) {
      const Side side = (K <= atx::vol::bench::kSpot) ? Side::Put : Side::Call;
      switch (which) {
        case Query::Iv: {
          double v = surf.iv(K, T);
          benchmark::DoNotOptimize(v);
          break;
        }
        case Query::FairValue: {
          auto v = surf.fair_value(K, T, side);
          benchmark::DoNotOptimize(v);
          break;
        }
        case Query::Greeks: {
          auto v = surf.greeks(K, T, side);
          benchmark::DoNotOptimize(v);
          break;
        }
        case Query::GreeksAnalytic: {
          auto v = surf.greeks_analytic(K, T, side);
          benchmark::DoNotOptimize(v);
          break;
        }
        case Query::Delta: {
          auto v = surf.delta(K, T, side);
          benchmark::DoNotOptimize(v);
          break;
        }
      }
    }
  }
  const double iters = static_cast<double>(state.iterations());
  const double n = static_cast<double>(ladder.size());
  state.counters["queries_per_s"] = benchmark::Counter(iters * n, benchmark::Counter::kIsRate);
  state.counters["ns_per_query"] = benchmark::Counter(
      iters * n * 1e-9, benchmark::Counter::kIsRate | benchmark::Counter::kInvert);
}

// ── 2/3. PortfolioPricer::price / pnl_explain ────────────────────────────
void run_price(benchmark::State& state, std::size_t n_unique, std::size_t ratio,
               unsigned n_threads, bool prices_only, bool analytic) {
  const PortfolioPricer& pr = pricer_for(n_unique, ratio);
  const SurfaceSet& surfaces = market().base_set();
  PriceOptions opts;
  opts.n_threads = n_threads;
  opts.prices_only = prices_only;
  opts.analytic_greeks = analytic;
  for (auto _ : state) {
    auto fr = pr.price(surfaces, opts);
    benchmark::DoNotOptimize(fr->total.pv);
    benchmark::ClobberMemory();
  }
  emit_book_counters(state, pr, kPriceRowBytes);
  state.counters["threads"] = static_cast<double>(n_threads);
  dump_counters(state, [&] {
    auto fr = pr.price(surfaces, opts);
    benchmark::DoNotOptimize(fr->total.pv);
  });
}

void run_pnl(benchmark::State& state, std::size_t n_unique, std::size_t ratio,
             unsigned n_threads) {
  const PortfolioPricer& pr = pricer_for(n_unique, ratio);
  const SurfaceSet& base = market().base_set();
  const SurfaceSet& shifted = market().shifted_set();
  PriceOptions opts;
  opts.n_threads = n_threads;
  for (auto _ : state) {
    auto er = pr.pnl_explain(base, shifted, opts);
    benchmark::DoNotOptimize(er->total.pnl_total);
    benchmark::ClobberMemory();
  }
  emit_book_counters(state, pr, kPnlRowBytes);
  state.counters["threads"] = static_cast<double>(n_threads);
  dump_counters(state, [&] {
    auto er = pr.pnl_explain(base, shifted, opts);
    benchmark::DoNotOptimize(er->total.pnl_total);
  });
}

// ── 4. Position-scatter-only ──────────────────────────────────────────────
// Uniques priced ONCE outside the timed region; the loop only scales each unique
// result by qty*multiplier and stores it into the (pre-allocated) output columns.
// Isolates store bandwidth from the Andersen-Lake kernel.
void run_scatter(benchmark::State& state, std::size_t n_unique, std::size_t ratio) {
  const PortfolioPricer& pr = pricer_for(n_unique, ratio);
  const Portfolio& pf = pr.portfolio();
  const SurfaceSet& surfaces = market().base_set();
  const auto contracts = pf.contracts();
  const auto positions = pf.positions();
  const std::size_t np = pf.n_positions();

  // Pre-price the uniques once (the kernel cost we are EXCLUDING).
  std::vector<AmericanGreeks> uni(contracts.size());
  for (std::size_t c = 0; c < contracts.size(); ++c) {
    const OptionContract& oc = contracts[c];
    const PricedSurface* s = surfaces.find(oc.uid);
    if (s != nullptr) {
      auto g = s->greeks(oc.K, oc.T, oc.side);
      if (g) {
        uni[c] = *g;
      }
    }
  }

  // Output columns (allocated once; the timed region is pure scatter/store).
  std::vector<double> pv(np), delta(np), gamma(np), vega(np), theta(np), rho(np),
      price(np), iv(np);
  // 8 double columns written per position.
  constexpr double kScatterBytes = 8.0 * 8.0;

  for (auto _ : state) {
    for (std::size_t i = 0; i < np; ++i) {
      const std::uint32_t ix = pf.contract_ix(i);
      const Position& p = positions[i];
      const double w = p.qty * p.multiplier;
      const AmericanGreeks& g = uni[ix];
      pv[i] = w * g.price;
      delta[i] = w * g.delta;
      gamma[i] = w * g.gamma;
      vega[i] = w * g.vega;
      theta[i] = w * g.theta;
      rho[i] = w * g.rho;
      price[i] = g.price;
      iv[i] = g.vega;  // stand-in per-share column
    }
    benchmark::DoNotOptimize(pv.data());
    benchmark::ClobberMemory();
  }
  const double iters = static_cast<double>(state.iterations());
  const double n_pos = static_cast<double>(np);
  const double n_uni = static_cast<double>(contracts.size());
  state.counters["positions_per_s"] = benchmark::Counter(n_pos * iters, benchmark::Counter::kIsRate);
  state.counters["unique_contracts_per_s"] =
      benchmark::Counter(n_uni * iters, benchmark::Counter::kIsRate);
  state.counters["bytes_per_s"] =
      benchmark::Counter(n_pos * kScatterBytes * iters, benchmark::Counter::kIsRate);
  state.counters["n_unique"] = n_uni;
  state.counters["n_positions"] = n_pos;
  state.counters["dedup_ratio"] = (n_uni > 0.0) ? n_pos / n_uni : 0.0;
}

// ── 5. Kernel floor ───────────────────────────────────────────────────────
// The exact per-unique fused op price() runs, single-threaded over the uniques.
enum class Floor { Greeks, FairValue, GreeksAnalytic };

void run_floor(benchmark::State& state, std::size_t n_unique, Floor kind) {
  const PortfolioPricer& pr = pricer_for(n_unique, /*ratio=*/1);
  const SurfaceSet& surfaces = market().base_set();
  const auto contracts = pr.portfolio().contracts();
  for (auto _ : state) {
    for (const OptionContract& oc : contracts) {
      const PricedSurface* s = surfaces.find(oc.uid);
      if (s == nullptr) {
        continue;
      }
      switch (kind) {
        case Floor::Greeks: {
          auto g = s->greeks(oc.K, oc.T, oc.side);
          benchmark::DoNotOptimize(g);
          break;
        }
        case Floor::FairValue: {
          auto v = s->fair_value(oc.K, oc.T, oc.side);
          benchmark::DoNotOptimize(v);
          break;
        }
        case Floor::GreeksAnalytic: {
          auto g = s->greeks_analytic(oc.K, oc.T, oc.side);
          benchmark::DoNotOptimize(g);
          break;
        }
      }
    }
  }
  const double iters = static_cast<double>(state.iterations());
  const double n_uni = static_cast<double>(contracts.size());
  state.counters["unique_contracts_per_s"] =
      benchmark::Counter(n_uni * iters, benchmark::Counter::kIsRate);
  state.counters["ns_per_unique"] = benchmark::Counter(
      n_uni * iters * 1e-9, benchmark::Counter::kIsRate | benchmark::Counter::kInvert);
}

// ── Registration (data-driven) ────────────────────────────────────────────
void register_all() {
  // 1. Query throughput.
  struct QReg { const char* name; Query q; };
  for (const QReg& qr : {QReg{"iv", Query::Iv}, QReg{"fair_value", Query::FairValue},
                         QReg{"greeks", Query::Greeks},
                         QReg{"greeks_analytic", Query::GreeksAnalytic},
                         QReg{"delta", Query::Delta}}) {
    apply_common(benchmark::RegisterBenchmark(
                     std::string("surf/query/") + qr.name,
                     [qr](benchmark::State& st) { run_query(st, qr.q); }))
        ->Unit(benchmark::kNanosecond);
  }

  const std::size_t uniques[] = {64, 2688};
  const std::size_t ratios[] = {1, 10, 100, 1000};
  const unsigned threads[] = {1, 2, 4, 8};

  // 2a. price() core scaling: full { n_unique x ratio x threads } (default greeks).
  for (const std::size_t nu : uniques) {
    for (const std::size_t ratio : ratios) {
      for (const unsigned nt : threads) {
        char buf[128];
        std::snprintf(buf, sizeof buf, "port/price/greeks/u%zu/r%zu/t%u", nu, ratio, nt);
        apply_common(benchmark::RegisterBenchmark(
                         buf, [nu, ratio, nt](benchmark::State& st) {
                           run_price(st, nu, ratio, nt, /*prices_only=*/false,
                                     /*analytic=*/false);
                         }))
            ->Unit(benchmark::kMicrosecond)
            ->UseRealTime();
      }
    }
  }

  // 2b. prices_only quote-refresh (subset).
  for (const std::size_t nu : uniques) {
    for (const std::size_t ratio : {std::size_t{1}, std::size_t{1000}}) {
      for (const unsigned nt : {1u, 8u}) {
        char buf[128];
        std::snprintf(buf, sizeof buf, "port/price/prices_only/u%zu/r%zu/t%u", nu, ratio, nt);
        apply_common(benchmark::RegisterBenchmark(
                         buf, [nu, ratio, nt](benchmark::State& st) {
                           run_price(st, nu, ratio, nt, /*prices_only=*/true,
                                     /*analytic=*/false);
                         }))
            ->Unit(benchmark::kMicrosecond)
            ->UseRealTime();
      }
    }
  }

  // 2c. analytic_greeks (subset).
  for (const std::size_t nu : uniques) {
    for (const std::size_t ratio : {std::size_t{1}, std::size_t{100}}) {
      for (const unsigned nt : {1u, 8u}) {
        char buf[128];
        std::snprintf(buf, sizeof buf, "port/price/analytic/u%zu/r%zu/t%u", nu, ratio, nt);
        apply_common(benchmark::RegisterBenchmark(
                         buf, [nu, ratio, nt](benchmark::State& st) {
                           run_price(st, nu, ratio, nt, /*prices_only=*/false,
                                     /*analytic=*/true);
                         }))
            ->Unit(benchmark::kMicrosecond)
            ->UseRealTime();
      }
    }
  }

  // 3. pnl_explain (subset).
  for (const std::size_t ratio : {std::size_t{1}, std::size_t{100}}) {
    for (const unsigned nt : {1u, 8u}) {
      char buf[128];
      std::snprintf(buf, sizeof buf, "port/pnl_explain/u2688/r%zu/t%u", ratio, nt);
      apply_common(benchmark::RegisterBenchmark(
                       buf, [ratio, nt](benchmark::State& st) {
                         run_pnl(st, 2688, ratio, nt);
                       }))
          ->Unit(benchmark::kMicrosecond)
          ->UseRealTime();
    }
  }

  // 4. Position-scatter-only.
  for (const std::size_t ratio : {std::size_t{100}, std::size_t{1000}}) {
    char buf[128];
    std::snprintf(buf, sizeof buf, "port/scatter_only/u2688/r%zu", ratio);
    apply_common(benchmark::RegisterBenchmark(
                     buf, [ratio](benchmark::State& st) { run_scatter(st, 2688, ratio); }))
        ->Unit(benchmark::kMicrosecond);
  }

  // 5. Kernel floor (single-thread per-unique fused op).
  struct FReg { const char* name; Floor f; };
  for (const std::size_t nu : uniques) {
    for (const FReg& fr : {FReg{"greeks", Floor::Greeks}, FReg{"fair_value", Floor::FairValue},
                           FReg{"greeks_analytic", Floor::GreeksAnalytic}}) {
      char buf[128];
      std::snprintf(buf, sizeof buf, "port/floor/%s/u%zu", fr.name, nu);
      apply_common(benchmark::RegisterBenchmark(
                       buf, [nu, fr](benchmark::State& st) { run_floor(st, nu, fr.f); }))
          ->Unit(benchmark::kMicrosecond);
    }
  }
}

const bool kRegistered = [] {
  register_all();
  return true;
}();

}  // namespace
