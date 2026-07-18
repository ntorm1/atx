// backtest_throughput_bench.cpp — Google Benchmark throughput for the atx-vol
// backtest engine's `run_backtest` over a non-trivial multi-underlier American book.
//
// Relocated from tests/backtest_bench_test.cpp (which was a throughput smoke gated
// by ATX_VOL_SKIP_UNLESS_BENCH, asserting only a generous wall-clock ceiling — not a
// correctness gate). Here the same measurement becomes a proper benchmark: the
// engine's steps/s (backtest steps priced per wall-second) is what items/s reports.
//
// SYNTHETIC surfaces only (the backtest_test / tearsheet_test make_surface pattern —
// analytic eSSVI, no fitting): a real corpus fit is slow and would swamp the timing.
// We write one synthetic archive per date (U surfaces, distinct uids / symbols) over
// D dates, then run a multi-leg straddle strategy (one straddle clip per underlier,
// EveryStep / HoldToExpiry) so the book grows to D*U*2 overlapping lots — a genuinely
// large multi-underlier American book. The corpus/archives + Clock are built ONCE
// (static fixture, the bench-suite convention) OUTSIDE the timed loop; only
// `run_backtest` is timed. `run_backtest` fans out over all cores by default, so the
// case runs `->UseRealTime()` — the reported time (and hence items/s = steps/s) is
// wall-clock, matching the test's original steady_clock measurement.

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <benchmark/benchmark.h>

#include "atx/vol/american.hpp"         // al_fast_opts, AmericanMethod
#include "atx/vol/backtest.hpp"         // Clock, run_backtest, BacktestResult
#include "atx/vol/corpus.hpp"           // CorpusManifest, CorpusEntry, CorpusFitStatus
#include "atx/vol/priced_surface.hpp"   // PricedSurface, PricingContext
#include "atx/vol/strategy.hpp"         // DeclarativeStrategy, StrategySpec
#include "atx/vol/surface_archive.hpp"  // write_surface_archive_file, SurfaceArchiveItem
#include "atx/vol/surface_parity.hpp"   // SliceContext
#include "atx/vol/types.hpp"            // Side, Status
#include "atx/vol/vol_curve.hpp"        // CurveSurface, EssviCurve
#include "atx/vol/vol_surface.hpp"      // EssviParams

#include "bench_util.hpp"

namespace atx::vol::bench {
namespace {

namespace fs = std::filesystem;

constexpr double kR = 0.043;
constexpr std::int64_t kBaseNow = 1700000000000000000LL;
constexpr std::int64_t kDayNs = 86400LL * 1000000000LL;

// Bench dimensions. D dates x U underliers. A straddle clip (2 legs per underlier)
// is opened EVERY step and held to expiry, so the book grows to D*U*2 lots — a
// genuinely non-trivial multi-underlier American book. Sized so the whole run
// stays a few seconds (American greeks over a growing book are ~ms/lot). The
// straddle tenor stays inside the synthetic grid's [0.05, 1.0] T span across the
// whole run (front cohort ages to ~0.10, never below 0.05).
constexpr int kD = 20;                  // dates (=> D-1 priced steps)
constexpr int kU = 4;                   // underliers per date
constexpr std::uint32_t kUidBase = 100;
constexpr double kTargetT = 0.15;       // straddle tenor (in-grid all run)

// ── Universe-shaped case (M2) dimensions ─────────────────────────────────────
// N names × daily entry × held-to-expiry × daily delta-hedge — the WS-D dispersion
// strategy shape (40Δ strangles, DeltaToZero daily). Sized so the growing book
// (~kUnivN×(kUnivD-1)×2 option lots ≈ 180) lands near the scoreboard's 160-lot
// reference so the reshaped steps/s is directly comparable to the ~64 steps/s
// baseline, while the SHAPE (per-entry 40Δ delta-solves + a daily full-book hedge)
// is the real hot loop the ≥5–10× gate targets. The 3M tenor stays inside the
// synthetic grid's [0.05, 1.0] span across the whole (~9 trading-day) run.
constexpr int kUnivD = 10;              // universe dates (=> 9 priced steps)
constexpr int kUnivN = 10;              // universe names
constexpr std::uint32_t kUnivUidBase = 200;
constexpr double kUnivTargetT = 0.25;   // 3M strangle tenor (in-grid all run)

// Setup failures happen once, outside the timed region (static fixture init), so an
// abort is the honest response — a benchmark of an error return measures nothing.
[[noreturn]] void bench_fatal(const std::string& msg) {
  std::fprintf(stderr, "FATAL(backtest-throughput-bench): %s\n", msg.c_str());
  std::abort();
}

// A synthetic eSSVI PricedSurface (flat forward, genuine American premium via
// q_eff=0.02), slices T in [0.05, 1.0]. Mirrors backtest_test's make_surface.
[[nodiscard]] PricedSurface make_surface(std::uint32_t uid, double S, double fwd,
                                         std::int64_t now_ts, double vol_bump = 0.0) {
  CurveSurface cs;
  std::vector<SliceContext> ctx;
  const double Ts[] = {0.05, 0.10, 0.20, 0.35, 0.50, 0.75, 1.00};
  int i = 0;
  for (const double T : Ts) {
    EssviParams e{};
    e.theta = 0.04 + 0.005 * static_cast<double>(i) + vol_bump;
    e.phi = 1.5 - 0.05 * static_cast<double>(i);
    e.rho = -0.4 + 0.02 * static_cast<double>(i);
    e.psi = 0.5;
    e.p = 0.5;
    e.lambda = 0.5;
    e.T = T;
    e.F = fwd;
    e.expiry_id = static_cast<std::uint16_t>(i);
    cs.push(std::make_unique<EssviCurve>(e, std::exp(-kR * T)));
    ctx.push_back(SliceContext{T, fwd, 0.0, 0.02, 250, 7});
    ++i;
  }
  PricingContext pc;
  pc.S = S;
  pc.r = kR;
  pc.now_ts_ns = now_ts;
  pc.method = AmericanMethod::AndersenLake;
  pc.al_opts = al_fast_opts();
  pc.uid = uid;
  auto ps = PricedSurface::create(std::move(cs), std::move(ctx), pc);
  if (!ps.has_value()) {
    bench_fatal(ps.error().to_string());
  }
  return std::move(*ps);
}

// Write `items` (symbol -> surface) as one date's archive; return its path.
[[nodiscard]] std::string write_archive(
    const fs::path& dir, const std::string& date,
    const std::vector<std::pair<std::string, const PricedSurface*>>& items) {
  std::error_code ec;
  fs::create_directories(dir, ec);
  const std::string path = (dir / (date + ".atxvsa")).string();
  std::vector<SurfaceArchiveItem> its;
  its.reserve(items.size());
  for (const auto& [sym, ps] : items) {
    its.push_back(SurfaceArchiveItem{sym, ps});
  }
  const Status st = write_surface_archive_file(path, its);
  if (!st.has_value()) {
    bench_fatal(st.error().to_string());
  }
  return path;
}

// Hand-build an Ok-only manifest over (date, archive_path) rows.
[[nodiscard]] CorpusManifest make_manifest(
    const std::vector<std::pair<std::string, std::string>>& date_paths) {
  CorpusManifest m;
  for (const auto& [date, path] : date_paths) {
    m.dates.push_back(date);
    CorpusEntry e;
    e.date = date;
    e.symbol = "U0";  // first-Ok archive per date is all the clock needs
    e.status = CorpusFitStatus::Ok;
    e.archive_path = path;
    m.entries.push_back(std::move(e));
  }
  return m;
}

// Build `n_dates` per-date archives (each holding `n_u` distinct-uid synthetic
// surfaces) and return the Clock over them. Run ONCE (static fixture) — the archives
// are written to a process-lifetime temp dir and read from disk by each
// `run_backtest`. Shared by the straddle and universe cases (distinct uid_base +
// dir so their fixtures never collide).
[[nodiscard]] Clock build_corpus_impl(int n_dates, int n_u, std::uint32_t uid_base,
                                      const char* dir_name) {
  const fs::path dir = fs::temp_directory_path() / dir_name;
  std::error_code ec;
  fs::remove_all(dir, ec);

  std::vector<std::pair<std::string, std::string>> dp;
  dp.reserve(static_cast<std::size_t>(n_dates));
  for (int d = 0; d < n_dates; ++d) {
    const std::int64_t now = kBaseNow + static_cast<std::int64_t>(d) * kDayNs;
    // Surfaces are kept alive in `owned` for the duration of the archive write.
    std::vector<PricedSurface> owned;
    owned.reserve(static_cast<std::size_t>(n_u));
    std::vector<std::pair<std::string, const PricedSurface*>> items;
    items.reserve(static_cast<std::size_t>(n_u));
    for (int u = 0; u < n_u; ++u) {
      const double S = (100.0 + 10.0 * static_cast<double>(u)) *
                       (1.0 + 0.003 * static_cast<double>(d));
      const double vb = 0.001 * static_cast<double>(d) + 0.002 * static_cast<double>(u);
      owned.push_back(make_surface(uid_base + static_cast<std::uint32_t>(u), S, S, now, vb));
    }
    std::vector<std::string> syms;
    syms.reserve(static_cast<std::size_t>(n_u));
    for (int u = 0; u < n_u; ++u) {
      syms.push_back("U" + std::to_string(u));
      items.emplace_back(syms.back(), &owned[static_cast<std::size_t>(u)]);
    }
    char buf[16];
    std::snprintf(buf, sizeof buf, "2027-%02d-%02d", 1 + d / 28, 1 + d % 28);
    dp.emplace_back(buf, write_archive(dir, buf, items));
  }

  auto clock = Clock::from_manifest(make_manifest(dp));
  if (!clock.has_value()) {
    bench_fatal(clock.error().to_string());
  }
  if (clock->size() != static_cast<std::size_t>(n_dates)) {
    bench_fatal("clock size mismatch");
  }
  return std::move(*clock);
}

// Process-lifetime Clock fixtures (built once on first use).
[[nodiscard]] const Clock& corpus_clock() {
  static const Clock clock =
      build_corpus_impl(kD, kU, kUidBase, "atx-backtest-throughput-bench");
  return clock;
}

[[nodiscard]] const Clock& universe_clock() {
  static const Clock clock =
      build_corpus_impl(kUnivD, kUnivN, kUnivUidBase, "atx-backtest-universe-bench");
  return clock;
}

// One ATM straddle clip per underlier, a fresh cohort every step, held to expiry.
[[nodiscard]] StrategySpec make_spec() {
  StrategySpec spec;
  spec.name = "bench-multi-underlier-straddle";
  for (int u = 0; u < kU; ++u) {
    LegSpec leg;
    leg.uid = kUidBase + static_cast<std::uint32_t>(u);
    leg.tenor.target_T = kTargetT;
    leg.structure.kind = StructureSpec::Kind::Straddle;  // ATM call + put
    leg.size = SizeSpec{SizeSpec::Kind::FixedContracts, 1.0, +1.0};
    spec.legs.push_back(leg);
  }
  spec.lifecycle.entry = LifecycleSpec::Entry::EveryStep;
  spec.lifecycle.holding = LifecycleSpec::Holding::HoldToExpiry;
  return spec;
}

// The WS-D universe strategy shape: for each of kUnivN names, a 40Δ Strangle (OTM
// call + OTM put, strikes resolved by delta off the synthetic surface), a fresh
// cohort EVERY step, HELD TO EXPIRY (overlapping cohorts), with a DeltaToZero hedge
// rebalanced DAILY. FixedContracts sizing keeps the book deterministic (net-vega
// cross-leg sizing is a WS-D/D4 concern, not this throughput baseline). The engine
// executes the hedge overlay (validate_hedge_spec accepts DeltaToZero+Daily; the
// step re-prices the full book once to supply both entry Greeks and the per-uid
// hedge delta), so this steps/s number is the real deserialize+price+delta-hedge
// hot loop the ≥5–10× gate targets.
[[nodiscard]] StrategySpec make_universe_spec() {
  StrategySpec spec;
  spec.name = "universe-40d-strangle-htx-dhedge";
  for (int u = 0; u < kUnivN; ++u) {
    LegSpec leg;
    leg.uid = kUnivUidBase + static_cast<std::uint32_t>(u);
    leg.tenor.target_T = kUnivTargetT;
    leg.tenor.snap_to_listed = false;
    leg.structure.kind = StructureSpec::Kind::Strangle;  // OTM call + OTM put
    leg.structure.call_leg = StrikeSelector{StrikeSelector::Kind::Delta, 0.40};
    leg.structure.put_leg = StrikeSelector{StrikeSelector::Kind::Delta, 0.40};
    leg.size = SizeSpec{SizeSpec::Kind::FixedContracts, 1.0, +1.0};
    spec.legs.push_back(std::move(leg));
  }
  spec.lifecycle.entry = LifecycleSpec::Entry::EveryStep;
  spec.lifecycle.holding = LifecycleSpec::Holding::HoldToExpiry;
  spec.hedge = HedgeSpec{HedgeSpec::Kind::DeltaToZero, HedgeSpec::Cadence::Daily, 0.0};
  return spec;
}

// ── Throughput: D dates x U underliers, straddle clips held to expiry ──────────
void BM_MultiUnderlierStraddle(benchmark::State& state) {
  const Clock& clock = corpus_clock();
  const StrategySpec spec = make_spec();

  double final_open_lots = 0.0;
  for (auto _ : state) {
    // Fresh strategy each iteration (cheap spec copy) so every timed run opens the
    // same deterministic cohort sequence; the ~µs construction is negligible against
    // the multi-second backtest and keeps runs independent.
    DeclarativeStrategy strat{spec};
    auto res = run_backtest(clock, strat);
    if (!res.has_value()) {
      state.SkipWithError(res.error().to_string().c_str());
      break;
    }
    if (res->size() != static_cast<std::size_t>(kD)) {
      state.SkipWithError("run_backtest produced unexpected row count");
      break;
    }
    if (!res->n_open_lots.empty()) {
      final_open_lots = res->n_open_lots.back();
    }
    benchmark::DoNotOptimize(res->nav.data());
    benchmark::ClobberMemory();
  }

  // Headline unit: priced steps (D-1). items/s == steps/s (the metric the test
  // measured). leg_steps = straddle (2 legs) x U underliers x priced_steps.
  const int priced_steps = kD - 1;
  const long long leg_steps = static_cast<long long>(priced_steps) * kU * 2;
  const double iters = static_cast<double>(state.iterations());
  state.SetItemsProcessed(state.iterations() * static_cast<std::int64_t>(priced_steps));
  state.counters["leg_steps_per_s"] =
      benchmark::Counter(iters * static_cast<double>(leg_steps), benchmark::Counter::kIsRate);
  state.counters["final_open_lots"] = final_open_lots;
}

// ── Throughput (M2): universe of 40Δ strangles, held to expiry, daily delta-hedge ──
// The reshaped real-universe case. kUnivN names each enter a 40Δ strangle EVERY step
// (strikes resolved by delta off the synthetic surface — the per-entry solve WS-P P4
// batches), held to expiry (overlapping cohorts), with a DAILY DeltaToZero hedge. The
// book grows to ~kUnivN×(kUnivD-1)×2 option lots; each step re-prices the whole open
// book once for marks + Greeks + the per-uid hedge delta. Headline: steps/s (== the
// scoreboard's backtest ★ratify metric); final_open_lots reports the book size.
void BM_UniverseStrangleHedged(benchmark::State& state) {
  const Clock& clock = universe_clock();
  const StrategySpec spec = make_universe_spec();

  double final_open_lots = 0.0;
  for (auto _ : state) {
    DeclarativeStrategy strat{spec};
    auto res = run_backtest(clock, strat);
    if (!res.has_value()) {
      state.SkipWithError(res.error().to_string().c_str());
      break;
    }
    if (res->size() != static_cast<std::size_t>(kUnivD)) {
      state.SkipWithError("run_backtest produced unexpected row count");
      break;
    }
    if (!res->n_open_lots.empty()) {
      final_open_lots = res->n_open_lots.back();
    }
    benchmark::DoNotOptimize(res->nav.data());
    benchmark::ClobberMemory();
  }

  const int priced_steps = kUnivD - 1;
  // leg_steps = strangle (2 legs) × N names × priced_steps (entry legs opened; the
  // held book that each step reprices is far larger — final_open_lots captures it).
  const long long leg_steps = static_cast<long long>(priced_steps) * kUnivN * 2;
  const double iters = static_cast<double>(state.iterations());
  state.SetItemsProcessed(state.iterations() * static_cast<std::int64_t>(priced_steps));
  state.counters["leg_steps_per_s"] =
      benchmark::Counter(iters * static_cast<double>(leg_steps), benchmark::Counter::kIsRate);
  state.counters["names"] = static_cast<double>(kUnivN);
  state.counters["final_open_lots"] = final_open_lots;
}

const int kRegistered = [] {
  apply_common(benchmark::RegisterBenchmark("backtest/multiunderlier_straddle/steps",
                                            BM_MultiUnderlierStraddle))
      ->Unit(benchmark::kMillisecond)
      ->UseRealTime();
  apply_common(benchmark::RegisterBenchmark("backtest/universe_strangle_hedged/steps",
                                            BM_UniverseStrangleHedged))
      ->Unit(benchmark::kMillisecond)
      ->UseRealTime();
  return 0;
}();

}  // namespace
}  // namespace atx::vol::bench
