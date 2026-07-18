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
//   5. An explicit KERNEL FLOOR: the exact per-unique fused op price() runs —
//      PricedSurface::iv() THEN PricedSurface::greeks() (greeks() already yields
//      the price) — NOT the example's fair_value + greeks double-solve. price()
//      always resolves iv() before its fair_value/greeks/greeks_analytic branch
//      (portfolio_pricer.cpp), so every floor variant below times that same
//      iv()-then-op sequence, not the second call in isolation.
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

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <benchmark/benchmark.h>

#include "atx/vol/american.hpp"
#include "atx/vol/pnl_attribution.hpp"
#include "atx/vol/portfolio_pricer.hpp"
#include "atx/vol/priced_surface.hpp"
#include "atx/vol/scenario_grid.hpp"
#include "atx/vol/types.hpp"

#include "bench_util.hpp"
#include "support/synth_book.hpp"

namespace {

using atx::vol::AmericanGreeks;
using atx::vol::OptionContract;
using atx::vol::Portfolio;
using atx::vol::PortfolioBuildOptions;
using atx::vol::PortfolioPricer;
using atx::vol::PortfolioWorkspace;
using atx::vol::Position;
using atx::vol::PriceFieldMask;
using atx::vol::PriceFrameView;
using atx::vol::AttributionOptions;
using atx::vol::pnl_attribution;
using atx::vol::PriceOptions;
using atx::vol::PriceStatus;
using atx::vol::PriceTotals;
using atx::vol::PricedSurface;
using atx::vol::scenario_grid;
using atx::vol::ScenarioGridSpec;
using atx::vol::Side;
using atx::vol::SurfaceSet;
using atx::vol::bench::apply_common;
using atx::vol::bench::dump_counters;
using atx::vol::bench::SynthMarket;

constexpr int kUnderlyings = 64;
constexpr int kSlices = 6;       // 64 uids x 6 slices x 7 strikes = 2688 uniques
constexpr int kConvexNodes = 40;

// Per-position frame widths (bytes/row) from portfolio_pricer.hpp.
constexpr double kPriceRowBytes = 101.0;  // 14 columns (FullGreeks)
constexpr double kMarksRowBytes = 37.0;   // 6 columns  (Marks: id,uid,pv,price,iv,status)
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

// A SINGLE-UNDERLYING chain on market uid 1 (convex/index): one uid, both sides, a
// wide strike ladder at every slice — the shape of a real single-name book (a SPY
// strangle backtest). The grouped solve yields only 2 (uid,side) groups, so this is
// the row that guards price()'s per-unique fan-out from regressing back to per-group
// (which stranded all but 2 workers on this shape). ~500 unique contracts, priced
// once (ratio 1) so the timed cost is the solve fan-out, not the position scatter.
[[nodiscard]] const PortfolioPricer& single_name_pricer() {
  static const PortfolioPricer pr = [] {
    constexpr int kStrikesPerSlice = 42;  // 6 slices x 42 strikes x 2 sides = 504 uniques
    std::vector<Position> book;
    book.reserve(static_cast<std::size_t>(kSlices) * kStrikesPerSlice * 2);
    std::uint64_t id = 0;
    for (int i = 0; i < kSlices; ++i) {
      const double T = atx::vol::bench::slice_T(i);
      for (int s = 0; s < kStrikesPerSlice; ++s) {
        // [78, 122] — inside the convex surface's [70, 130] node domain.
        const double K = 78.0 + 44.0 * static_cast<double>(s) /
                                    static_cast<double>(kStrikesPerSlice - 1);
        for (const Side side : {Side::Put, Side::Call}) {
          book.push_back(Position{id++, OptionContract{1u, K, T, side}, 5.0, 100.0});
        }
      }
    }
    PortfolioBuildOptions opts;
    opts.expected_unique_contracts = book.size();
    return PortfolioPricer(Portfolio::create(book, opts).value());
  }();
  return pr;
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

// ── 1b. Ladder reuse: evaluate_batch vs per-entry evaluate ────────────────
// A single-expiry 40-strike ladder priced two ways at one T: `evaluate_batch`
// resolves the T-bracket + carry ONCE and reuses it across the ladder; the
// per-entry path calls `evaluate` (which re-resolves) per strike. Bit-identical
// output (proved in tests); this measures the reuse throughput win. Run at
// Iv-only (the T-resolution is the whole cost, so reuse shows) and at Iv|Price
// (the Andersen-Lake solve dominates, so reuse is in the noise) so the report can
// show WHERE the ladder pays.
enum class LadderMode { Batch, PerEntry };

void run_ladder(benchmark::State& state, LadderMode mode, PricedSurface::EvalField fields) {
  const PricedSurface& surf = market().base.front();  // uid 1 (convex/index)
  const double T = atx::vol::bench::slice_T(2);
  std::vector<double> Ks;
  std::vector<double> Ts;
  std::vector<Side> sides;
  Ks.reserve(40);
  Ts.reserve(40);
  sides.reserve(40);
  for (int i = 0; i < 40; ++i) {
    const double K = 70.0 + 60.0 * (static_cast<double>(i) + 0.5) / 40.0;
    Ks.push_back(K);
    Ts.push_back(T);
    sides.push_back((K <= atx::vol::bench::kSpot) ? Side::Put : Side::Call);
  }
  const std::size_t n = Ks.size();
  const bool want_greeks = has_field(fields, PricedSurface::EvalField::FirstOrder) ||
                           has_field(fields, PricedSurface::EvalField::SecondOrder);
  std::vector<double> out_iv(n), out_px(n);
  std::vector<AmericanGreeks> out_gk(n);
  std::vector<atx::vol::Status> out_st(n);
  const std::span<AmericanGreeks> gk_span =
      want_greeks ? std::span<AmericanGreeks>(out_gk) : std::span<AmericanGreeks>{};

  for (auto _ : state) {
    if (mode == LadderMode::Batch) {
      auto rc = surf.evaluate_batch(Ks, Ts, sides, fields, /*analytic=*/false,
                                    PricedSurface::EvaluationSoA{out_iv, out_px, gk_span, out_st,
                                                                {}, {}});
      benchmark::DoNotOptimize(rc);
    } else {
      for (std::size_t i = 0; i < n; ++i) {
        const PricedSurface::FusedResult fr =
            surf.evaluate(Ks[i], Ts[i], sides[i], fields, /*analytic=*/false);
        // Write the SAME output set evaluate_batch writes, so the batch/per_entry
        // delta isolates the T-carry reuse, not a difference in stores.
        out_iv[i] = fr.iv;
        out_px[i] = fr.price;
        if (want_greeks) {
          out_gk[i] = fr.greeks;
        }
        out_st[i] = fr.status;
      }
    }
    benchmark::DoNotOptimize(out_iv.data());
    benchmark::ClobberMemory();
  }
  const double iters = static_cast<double>(state.iterations());
  const double nq = static_cast<double>(n);
  state.counters["queries_per_s"] = benchmark::Counter(iters * nq, benchmark::Counter::kIsRate);
  state.counters["ns_per_query"] = benchmark::Counter(
      iters * nq * 1e-9, benchmark::Counter::kIsRate | benchmark::Counter::kInvert);
}

// ── 2/3. PortfolioPricer::price / pnl_explain ────────────────────────────
void run_price(benchmark::State& state, std::size_t n_unique, std::size_t ratio,
               unsigned n_threads, bool prices_only, bool analytic, bool adjoint = false) {
  const PortfolioPricer& pr = pricer_for(n_unique, ratio);
  const SurfaceSet& surfaces = market().base_set();
  PriceOptions opts;
  opts.n_threads = n_threads;
  opts.prices_only = prices_only;
  opts.analytic_greeks = analytic;
  // WS-P P5: adjoint FullGreeks A/B (evaluate_batch marks + american_greeks_adjoint
  // risk instead of the FD bundle). Compare this row to the matching
  // port/price/greeks (FD) and port/price/analytic (american_greeks_al) rows.
  opts.adjoint_greeks = adjoint;
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

// price_into() over a WARMED workspace + caller-owned output columns. The
// retained PreparedPortfolio and scratch are built once outside the timed region,
// so this row isolates the allocation-free, no-rebuild hot path — its delta from
// the matching port/price/greeks row is the eliminated per-call PreparedPortfolio
// build + frame allocation cost.
void run_price_into(benchmark::State& state, std::size_t n_unique, std::size_t ratio,
                    unsigned n_threads, PriceFieldMask fields) {
  const PortfolioPricer& pr = pricer_for(n_unique, ratio);
  const SurfaceSet& surfaces = market().base_set();
  const std::size_t np = pr.portfolio().n_positions();
  const bool want_greeks = has_field(fields, PriceFieldMask::Greeks);
  PriceOptions opts;
  opts.n_threads = n_threads;

  // Caller-owned output columns (allocated once, outside the timed region). Greek
  // columns exist only under a Greeks mask (empty spans under Marks).
  std::vector<std::uint64_t> id(np);
  std::vector<std::uint32_t> uid(np);
  std::vector<double> pv(np), price(np), iv(np);
  std::vector<double> delta, gamma, vega, theta, rho, vanna, volga, charm;
  if (want_greeks) {
    delta.resize(np);
    gamma.resize(np);
    vega.resize(np);
    theta.resize(np);
    rho.resize(np);
    vanna.resize(np);
    volga.resize(np);
    charm.resize(np);
  }
  std::vector<PriceStatus> status(np);
  PriceTotals total;
  PriceFrameView view{id,    uid,   pv,    price, iv,     delta,  gamma, vega,
                      theta, rho,   vanna, volga, charm,  status, &total};

  PortfolioWorkspace ws;
  ws.reserve(pr.portfolio().n_contracts(), np);
  (void)pr.price_into(surfaces, fields, view, ws, opts);  // warm substrate + scratch

  for (auto _ : state) {
    auto s = pr.price_into(surfaces, fields, view, ws, opts);
    benchmark::DoNotOptimize(s);
    benchmark::DoNotOptimize(total.pv);
    benchmark::ClobberMemory();
  }
  emit_book_counters(state, pr, want_greeks ? kPriceRowBytes : kMarksRowBytes);
  state.counters["threads"] = static_cast<double>(n_threads);
}

// price_totals() over a warmed workspace: solve + fixed-order reduction, NO
// scatter, NO per-row frame. The delta from run_price_into is the whole
// per-position scatter/store cost a totals-only caller avoids.
void run_price_totals(benchmark::State& state, std::size_t n_unique, std::size_t ratio,
                      unsigned n_threads, PriceFieldMask fields) {
  const PortfolioPricer& pr = pricer_for(n_unique, ratio);
  const SurfaceSet& surfaces = market().base_set();
  PriceOptions opts;
  opts.n_threads = n_threads;
  PortfolioWorkspace ws;
  ws.reserve(pr.portfolio().n_contracts(), pr.portfolio().n_positions());
  (void)pr.price_totals(surfaces, fields, ws, opts);  // warm substrate + scratch

  for (auto _ : state) {
    auto t = pr.price_totals(surfaces, fields, ws, opts);
    benchmark::DoNotOptimize(t->pv);
    benchmark::ClobberMemory();
  }
  emit_book_counters(state, pr, /*row_bytes=*/0.0);  // no per-row frame materialized
  state.counters["threads"] = static_cast<double>(n_threads);
}

// price() over the single-name chain at a given thread count. Isolates the solve
// fan-out on the 2-group shape so its 1->N-thread scaling is measurable.
void run_price_single_name(benchmark::State& state, unsigned n_threads) {
  const PortfolioPricer& pr = single_name_pricer();
  const SurfaceSet& surfaces = market().base_set();
  PriceOptions opts;
  opts.n_threads = n_threads;
  for (auto _ : state) {
    auto fr = pr.price(surfaces, opts);
    benchmark::DoNotOptimize(fr->total.pv);
    benchmark::ClobberMemory();
  }
  emit_book_counters(state, pr, kPriceRowBytes);
  state.counters["threads"] = static_cast<double>(n_threads);
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

// ── 3b. pnl_attribution: base->shifted P&L attribution (C3.3) ─────────────
// The attribution layer runs ONE pnl_explain solve (the dominant cost) plus a cheap
// pivot-sampling + vega-partition pass. This case reports its OVERHEAD vs plain
// pnl_explain: both the attribution and the reference rebuild the Portfolio from the
// same book and run the same solve, so `ratio_attr_over_pnl` isolates the pivot+split
// cost (expect ~1.0 — the Andersen-Lake solve dominates).
void run_attribution(benchmark::State& state, std::size_t n_unique, std::size_t ratio,
                     unsigned n_threads) {
  const SurfaceSet& base = market().base_set();
  const SurfaceSet& shifted = market().shifted_set();
  const std::vector<Position> book =
      atx::vol::bench::make_book(kUnderlyings, kSlices, n_unique, ratio);
  AttributionOptions opts;
  opts.n_threads = n_threads;

  for (auto _ : state) {
    auto ar = pnl_attribution(book, base, shifted, opts);
    benchmark::DoNotOptimize(ar->total.pnl_total);
    benchmark::ClobberMemory();
  }

  // Overhead reference: Portfolio::create + pnl_explain over the SAME book/threads
  // (the attribution's own solve path, minus the pivot+split). Timed manually so the
  // report can state attribution-time / pnl_explain-time directly.
  PriceOptions popts;
  popts.n_threads = n_threads;
  using clock = std::chrono::steady_clock;
  constexpr int kReps = 20;
  const auto t0 = clock::now();
  for (int rep = 0; rep < kReps; ++rep) {
    auto ar = pnl_attribution(book, base, shifted, opts);
    benchmark::DoNotOptimize(ar->total.pnl_total);
  }
  const auto t1 = clock::now();
  for (int rep = 0; rep < kReps; ++rep) {
    auto pf = Portfolio::create(book);
    const PortfolioPricer pr(std::move(pf.value()));
    auto er = pr.pnl_explain(base, shifted, popts);
    benchmark::DoNotOptimize(er->total.pnl_total);
  }
  const auto t2 = clock::now();
  const double attr_ns = std::chrono::duration<double, std::nano>(t1 - t0).count() / kReps;
  const double pnl_ns = std::chrono::duration<double, std::nano>(t2 - t1).count() / kReps;

  const double n_pos = static_cast<double>(book.size());
  const double iters = static_cast<double>(state.iterations());
  state.counters["positions_per_s"] =
      benchmark::Counter(n_pos * iters, benchmark::Counter::kIsRate);
  state.counters["attr_us"] = attr_ns / 1e3;
  state.counters["pnl_explain_us"] = pnl_ns / 1e3;
  state.counters["ratio_attr_over_pnl"] = (pnl_ns > 0.0) ? attr_ns / pnl_ns : 0.0;
  state.counters["n_unique"] = static_cast<double>(n_unique);
  state.counters["n_positions"] = n_pos;
  state.counters["threads"] = static_cast<double>(n_threads);
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
// price() (portfolio_pricer.cpp) always resolves `out.iv = surf->iv(c.K, c.T)`
// BEFORE its fair_value/greeks/greeks_analytic branch, so the floor includes
// that iv() call too — otherwise it times only the second half of the fused
// operation and understates price()'s true per-unique work.
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
      double iv = s->iv(oc.K, oc.T);  // price() always runs this first.
      benchmark::DoNotOptimize(iv);
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

// ── 6. scenario_grid: full-book 11×11 spot×vol scenario matrix (C3.1 + C3.2) ──
// The Taylor variant (radii=inf) reconstructs all 121 cells from ONE deduped Greek
// solve — grid-cost ≈ one full-Greeks solve. The Exact variant (radii=0) re-solves
// EVERY cell per unique — grid-cost ≈ 121 solve waves. The Mixed variant (default
// radii) routes inner cells Taylor / outer cells Exact. Each row reports cells/s and
// ratio_grid_over_solve = grid-build time / one price_totals(FullGreeks) call (the
// "one Greek solve" reference) on the SAME 2688-unique full board — honestly measured
// (~1.0 confirms Taylor amortizes into the solve; ~cell-count confirms all-exact).
void run_scenario_grid(benchmark::State& state, unsigned n_threads, double rad_spot,
                       double rad_vol) {
  const std::size_t n_unique = 2688;  // 64 uids × 6 slices × 7 strikes (full board)
  const SurfaceSet& surfaces = market().base_set();
  std::vector<Position> book =
      atx::vol::bench::make_book(kUnderlyings, kSlices, n_unique, /*positions_per_unique=*/1);

  ScenarioGridSpec spec;
  spec.n_threads = n_threads;
  for (int i = 0; i < 11; ++i) {
    // Spot: −10%..+10% in 2% steps; vol: −5..+5 vol pts in 1-pt steps.
    spec.spot_pct.push_back(-0.10 + 0.02 * static_cast<double>(i));
    spec.vol_bump.push_back(-0.05 + 0.01 * static_cast<double>(i));
  }
  spec.dr = 5e-4;
  spec.dt = 3.0 / 365.0;
  spec.taylor_radius_spot = rad_spot;
  spec.taylor_radius_vol = rad_vol;
  const double cells = static_cast<double>(spec.spot_pct.size() * spec.vol_bump.size());

  for (auto _ : state) {
    auto g = scenario_grid(book, surfaces, spec);
    benchmark::DoNotOptimize(g->pnl.data());
    benchmark::ClobberMemory();
  }

  // Ratio reference: one warm price_totals(FullGreeks) — the single Greek solve the
  // grid is built on. Timed manually (same n_threads) so the report can state
  // grid-time / one-Greek-solve-time directly.
  const PortfolioPricer& pr = pricer_for(n_unique, /*ratio=*/1);
  PriceOptions popts;
  popts.n_threads = n_threads;
  PortfolioWorkspace ws;
  ws.reserve(pr.portfolio().n_contracts(), pr.portfolio().n_positions());
  (void)pr.price_totals(surfaces, PriceFieldMask::FullGreeks, ws, popts);  // warm

  using clock = std::chrono::steady_clock;
  constexpr int kReps = 20;
  const auto t0 = clock::now();
  for (int rep = 0; rep < kReps; ++rep) {
    auto g = scenario_grid(book, surfaces, spec);
    benchmark::DoNotOptimize(g->pnl.data());
  }
  const auto t1 = clock::now();
  for (int rep = 0; rep < kReps; ++rep) {
    auto t = pr.price_totals(surfaces, PriceFieldMask::FullGreeks, ws, popts);
    benchmark::DoNotOptimize(t->pv);
  }
  const auto t2 = clock::now();
  const double grid_ns = std::chrono::duration<double, std::nano>(t1 - t0).count() / kReps;
  const double ref_ns = std::chrono::duration<double, std::nano>(t2 - t1).count() / kReps;

  const double iters = static_cast<double>(state.iterations());
  state.counters["cells_per_s"] = benchmark::Counter(iters * cells, benchmark::Counter::kIsRate);
  state.counters["cells"] = cells;
  state.counters["grid_us"] = grid_ns / 1e3;
  state.counters["price_totals_greeks_us"] = ref_ns / 1e3;
  state.counters["ratio_grid_over_solve"] = (ref_ns > 0.0) ? grid_ns / ref_ns : 0.0;
  state.counters["threads"] = static_cast<double>(n_threads);
  state.counters["n_unique"] = static_cast<double>(pr.portfolio().n_contracts());

  // Route mix + fallback count (one untimed build) so the report can read how many of
  // the 121 cells re-solved and whether any lane fell back.
  auto gg = scenario_grid(book, surfaces, spec);
  double n_exact = 0.0;
  if (gg.has_value()) {
    for (const std::uint8_t rv : gg->route) {
      n_exact += (rv == static_cast<std::uint8_t>(atx::vol::ScenarioRoute::Exact)) ? 1.0 : 0.0;
    }
    state.counters["exact_cells"] = n_exact;
    state.counters["fallback_lanes"] = static_cast<double>(gg->n_exact_fallback_lanes);
  }
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

  // 1b. Ladder reuse: evaluate_batch vs per-entry, Iv-only and Iv|Price.
  struct LReg {
    const char* name;
    LadderMode mode;
    PricedSurface::EvalField fields;
  };
  const PricedSurface::EvalField iv_only = PricedSurface::EvalField::Iv;
  const PricedSurface::EvalField iv_px =
      PricedSurface::EvalField::Iv | PricedSurface::EvalField::Price;
  for (const LReg& lr : {LReg{"batch/iv", LadderMode::Batch, iv_only},
                         LReg{"per_entry/iv", LadderMode::PerEntry, iv_only},
                         LReg{"batch/price", LadderMode::Batch, iv_px},
                         LReg{"per_entry/price", LadderMode::PerEntry, iv_px}}) {
    apply_common(benchmark::RegisterBenchmark(
                     std::string("surf/ladder/") + lr.name,
                     [lr](benchmark::State& st) { run_ladder(st, lr.mode, lr.fields); }))
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

  // 2b-into. In-place API: price_into (warmed workspace, allocation-free) and
  // price_totals (no scatter) at the report's 100:1 / 1000:1 dedup rows, both
  // masks. Compare against the matching port/price/greeks and
  // port/price/prices_only rows to read the eliminated build/alloc/scatter cost.
  for (const std::size_t ratio : {std::size_t{100}, std::size_t{1000}}) {
    for (const unsigned nt : {1u, 8u}) {
      char buf[128];
      std::snprintf(buf, sizeof buf, "port/price_into/greeks/u2688/r%zu/t%u", ratio, nt);
      apply_common(benchmark::RegisterBenchmark(buf, [ratio, nt](benchmark::State& st) {
                     run_price_into(st, 2688, ratio, nt, PriceFieldMask::FullGreeks);
                   }))
          ->Unit(benchmark::kMicrosecond)
          ->UseRealTime();
      std::snprintf(buf, sizeof buf, "port/price_into/marks/u2688/r%zu/t%u", ratio, nt);
      apply_common(benchmark::RegisterBenchmark(buf, [ratio, nt](benchmark::State& st) {
                     run_price_into(st, 2688, ratio, nt, PriceFieldMask::Marks);
                   }))
          ->Unit(benchmark::kMicrosecond)
          ->UseRealTime();
      std::snprintf(buf, sizeof buf, "port/price_totals/greeks/u2688/r%zu/t%u", ratio, nt);
      apply_common(benchmark::RegisterBenchmark(buf, [ratio, nt](benchmark::State& st) {
                     run_price_totals(st, 2688, ratio, nt, PriceFieldMask::FullGreeks);
                   }))
          ->Unit(benchmark::kMicrosecond)
          ->UseRealTime();
      std::snprintf(buf, sizeof buf, "port/price_totals/marks/u2688/r%zu/t%u", ratio, nt);
      apply_common(benchmark::RegisterBenchmark(buf, [ratio, nt](benchmark::State& st) {
                     run_price_totals(st, 2688, ratio, nt, PriceFieldMask::Marks);
                   }))
          ->Unit(benchmark::kMicrosecond)
          ->UseRealTime();
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

  // 2c-adjoint (WS-P P5). FullGreeks risk via the Christianson through-iterations
  // adjoint (PriceOptions::adjoint_greeks) — the A/B against the FD bundle
  // (port/price/greeks) and the AL analytic bundle (port/price/analytic) at the same
  // { n_unique x ratio x threads } cells. Honest measurement: apply_common stamps the
  // per-row _cv aggregate; treat any row with cv > 0.05 (5%) as provisional on a
  // shared/noisy host. Report BOTH scopes (full 8-greek bundle here; first-order
  // delta+vega is the micro-bench in adjoint_greeks_test DISABLED_PerfSanity). The
  // >=5x gate scope is PM-held — this row reports the number, it does not claim it.
  for (const std::size_t nu : uniques) {
    for (const std::size_t ratio : {std::size_t{1}, std::size_t{100}}) {
      for (const unsigned nt : {1u, 8u}) {
        char buf[128];
        std::snprintf(buf, sizeof buf, "port/price/adjoint/u%zu/r%zu/t%u", nu, ratio, nt);
        apply_common(benchmark::RegisterBenchmark(
                         buf, [nu, ratio, nt](benchmark::State& st) {
                           run_price(st, nu, ratio, nt, /*prices_only=*/false,
                                     /*analytic=*/false, /*adjoint=*/true);
                         }))
            ->Unit(benchmark::kMicrosecond)
            ->UseRealTime();
      }
    }
  }

  // 2d. Single-name chain: ONE uid, both sides (=> 2 groups), ~500 strikes. Guards
  // price()'s per-unique fan-out from regressing to per-group on the single-name
  // (SPY-strangle) shape; t in {1,4,8} exposes the 1->8 thread scaling that a
  // per-group fan-out strands (only 2 of 8 workers fed).
  for (const unsigned nt : {1u, 4u, 8u}) {
    char buf[128];
    std::snprintf(buf, sizeof buf, "port/price/greeks/single_name/u504/r1/t%u", nt);
    apply_common(benchmark::RegisterBenchmark(
                     buf, [nt](benchmark::State& st) { run_price_single_name(st, nt); }))
        ->Unit(benchmark::kMicrosecond)
        ->UseRealTime();
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

  // 3b. pnl_attribution (C3.3): one case on the synth book, hw threads. Reports the
  // pivot+split overhead vs plain pnl_explain (ratio_attr_over_pnl ~ 1.0).
  apply_common(benchmark::RegisterBenchmark(
                   "attr/book_attribution/synth_book",
                   [](benchmark::State& st) { run_attribution(st, 2688, /*ratio=*/1, /*t=*/0u); }))
      ->Unit(benchmark::kMicrosecond)
      ->UseRealTime();

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

  // 6. scenario_grid: full-book 11×11 spot×vol matrix at t1 (clean ratio) and hw
  // (throughput). Three routing variants: taylor (radii=inf, C3.1 reference), mixed
  // (default radii, C3.2 product), exact (radii=0, all-cell re-solve). Each emits
  // cells/s + the grid-time / one-Greek-solve ratio.
  struct GReg {
    const char* tag;
    double rs;
    double rv;
  };
  const double kInfR = std::numeric_limits<double>::infinity();
  for (const GReg& g :
       {GReg{"taylor", kInfR, kInfR},
        GReg{"mixed", atx::vol::kDefaultTaylorRadiusSpot, atx::vol::kDefaultTaylorRadiusVol},
        GReg{"exact", 0.0, 0.0}}) {
    for (const unsigned nt : {1u, 0u}) {
      char buf[128];
      std::snprintf(buf, sizeof buf, "scenario/grid_11x11_%s/synth_book/t%u", g.tag, nt);
      apply_common(benchmark::RegisterBenchmark(buf, [nt, g](benchmark::State& st) {
                     run_scenario_grid(st, nt, g.rs, g.rv);
                   }))
          ->Unit(benchmark::kMicrosecond)
          ->UseRealTime();
    }
  }
}

const bool kRegistered = [] {
  register_all();
  return true;
}();

}  // namespace
