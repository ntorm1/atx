// fit_serialize_bench.cpp — WS-F F-b: the FUSED streaming fit->serialize
// throughput bench (class: perf). Measures surfaces/sec through the production
// `populate_surface_db` pipeline: fit each board on the E1/E2 pool, serialize its
// date's partition to ATXVSA2 (write_surface_archive_v2_file), and RELEASE it in
// ascending date order — U1 streaming, so peak RSS is O(dates in flight), not
// O(all dates). No intermediate whole-board materialization beyond the per-date
// group the writer needs. This is deliberately NOT corpus_build_bench.cpp
// (measure-owned), which times the whole-corpus build_corpus over a fixed 20
// boards and reports boards/s; this TU times the SurfaceDb streaming populate the
// universe-date target rides on and reports surfaces/s fit+serialized.
//
// ── Metric & honesty (sprint §3) ─────────────────────────────────────────────
// Headline: `surfaces_per_s` (surfaces fit AND serialized per second), derived
// from the MEDIAN of an internal best-of-kReps loop. Every emitted row carries:
//   cv_pct       = 100 * stddev/mean of the per-rep populate wall (CV%).
//   provisional  = 1.0 ALWAYS — this host is shared with sibling agents; ALL
//                  numbers here are provisional until the PM re-captures under the
//                  quiet-window protocol (P-core pin, best-of-N, CV<=5%).
// Correctness is validated on Debug (the n_ok == n_planned gate below aborts a row
// whose fit dropped a board); perf numbers are read on rel-avx2. Determinism holds
// regardless of worker count (populate results are byte-identical across n_threads,
// gated by surface_db_populate_test) — the thread rows measure throughput, not a
// different result.
//
// ── Schema (per benchmark row, JSON counters) ────────────────────────────────
//   name:  "fit_serialize/populate/threads:<n>"
//   counters:
//     surfaces_per_s      headline throughput (fit+serialized surfaces / s), median
//     ms_median           median wall of one full universe populate (ms)
//     cv_pct              CV% of the per-rep wall (measurement-honesty stamp)
//     provisional         1.0 (host busy — see above)
//     threads             populate worker count for this row
//     surfaces / n_planned  surfaces produced / boards planned (n_ok gate)
//     dates / symbols_per_date  universe shape
//     in_flight_surfaces  per-date group size = the streaming RSS bound (U1)
//     archive_bytes / bytes_per_surface  serialized partition footprint

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <optional>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

#include <benchmark/benchmark.h>

#include "atx/vol/api/marketdata/corpus.hpp"              // CorpusBoard
#include "atx/vol/api/marketdata/data.hpp"               // iso_to_ns, year_fraction
#include "atx/vol/api/core/market_env.hpp"         // MarketEnv
#include "atx/vol/api/backtest/panel.hpp"             // make_synthetic_american_panel, SynthPanelSpec
#include "atx/vol/api/fitting/s3.hpp"                // S3Params
#include "atx/vol/api/storage/surface_db.hpp"        // SurfaceDb
#include "atx/vol/tools/surface_db_populate.hpp" // populate_surface_db (U1 streaming)

#include "bench_util.hpp" // stat_cv (shared CV helper)

namespace atx::vol::bench {
namespace {

namespace fs = std::filesystem;

// A genuinely fittable single-name board (the make_board_spec pattern from
// tests/surface_db_populate_test.cpp, gtest assertions dropped): four expiries
// with a mild declining term structure and a 13-strike ladder — auto-selects the
// eSSVI backbone (the dispersion single-name bulk, the sprint's target shape).
[[nodiscard]] SynthPanelSpec make_board_spec(const std::string &symbol, const std::string &date,
                                             double spot, double sigma0) {
  SynthPanelSpec s;
  s.uid = symbol;
  s.snapshot_iso = date;
  s.spot = spot;
  s.r = 0.03;
  s.borrow = 0.0;
  struct Row {
    const char *iso;
    double sig;
    double skew_k;
    double c2;
  };
  const Row rows[] = {
      {"2026-10-16", sigma0, -0.55, 0.6},
      {"2026-11-20", sigma0 - 0.02, -0.52, 0.7},
      {"2026-12-18", sigma0 - 0.04, -0.50, 0.8},
      {"2027-03-19", sigma0 - 0.06, -0.46, 0.9},
  };
  for (const Row &r : rows) {
    SynthExpiry e;
    e.expiry_iso = r.iso;
    e.T = year_fraction(date, r.iso);
    const double s2 = 2.0 * std::sqrt(e.T) * r.skew_k;
    e.truth = S3Params{r.sig, s2, r.c2};
    s.expiries.push_back(e);
  }
  for (const double m :
       {0.80, 0.83, 0.87, 0.91, 0.95, 0.98, 1.0, 1.02, 1.05, 1.09, 1.13, 1.17, 1.20}) {
    s.strikes.push_back(spot * m);
  }
  s.half_spread_frac = 0.05;
  s.min_half_spread = 0.05;
  return s;
}

[[nodiscard]] std::optional<CorpusBoard> make_board(const std::string &date,
                                                    const std::string &symbol, double spot,
                                                    double sigma0) {
  const SynthPanelSpec spec = make_board_spec(symbol, date, spot, sigma0);
  auto panel = make_synthetic_american_panel(spec);
  if (!panel.has_value()) {
    return std::nullopt;
  }
  CorpusBoard b;
  b.date = date;
  b.symbol = symbol;
  b.frame = panel->frame;
  b.env = MarketEnv::flat(spec.spot, spec.r, iso_to_ns(date), spec.cash_divs);
  return b;
}

// The universe: kDates dates x kSymbols single-names — a daily universe-date grid.
// Built ONCE (process-lifetime), reused across every thread row and rep.
constexpr int kDates = 5;
constexpr int kSymbols = 8;

struct Universe {
  std::vector<CorpusBoard> boards;
  std::size_t n_planned = 0;
  std::size_t dates = 0;
  std::size_t symbols = 0;
  bool ok = false;
};

[[nodiscard]] const Universe &universe() {
  static const Universe u = [] {
    Universe out;
    for (int d = 0; d < kDates; ++d) {
      char date[16];
      std::snprintf(date, sizeof date, "2026-08-%02d", 3 + d); // 5 weekday-ish dates
      for (int s = 0; s < kSymbols; ++s) {
        char sym[8];
        std::snprintf(sym, sizeof sym, "NM%02d", s);
        const double spot = 40.0 + 12.0 * static_cast<double>(s);
        const double sigma0 = 0.24 + 0.01 * static_cast<double>(s);
        auto b = make_board(date, sym, spot, sigma0);
        if (!b.has_value()) {
          return out; // ok stays false -> rows SkipWithError
        }
        out.boards.push_back(std::move(*b));
      }
    }
    out.n_planned = out.boards.size();
    out.dates = static_cast<std::size_t>(kDates);
    out.symbols = static_cast<std::size_t>(kSymbols);
    out.ok = out.n_planned == static_cast<std::size_t>(kDates) * kSymbols;
    return out;
  }();
  return u;
}

[[nodiscard]] double median(std::vector<double> v) {
  if (v.empty()) {
    return 0.0;
  }
  std::sort(v.begin(), v.end());
  const std::size_t n = v.size();
  return (n % 2 == 1) ? v[n / 2] : 0.5 * (v[n / 2 - 1] + v[n / 2]);
}

[[nodiscard]] std::uint64_t sum_partition_bytes(const SurfaceDb &db) {
  std::uint64_t total = 0;
  for (const DbPartitionInfo &p : db.partitions()) {
    total += p.file_size;
  }
  return total;
}

// One full universe populate under `n_threads` workers, timed best-of-kReps so the
// row can carry an honest median + CV under a loaded host. A FRESH SurfaceDb is
// created (untimed) per rep so skip_existing never short-circuits the fit.
void run_fit_serialize(benchmark::State &state, unsigned n_threads) {
  const Universe &u = universe();
  if (!u.ok) {
    state.SkipWithError("synthetic universe generation failed");
    return;
  }

  constexpr int kReps = 7;
  double ms_median = 0.0;
  double cv_pct = 0.0;
  std::size_t n_ok = 0;
  std::uint64_t archive_bytes = 0;

  for (auto _ : state) {
    std::vector<double> samples;
    samples.reserve(kReps);
    for (int rep = 0; rep < kReps; ++rep) {
      const fs::path db_root =
          fs::temp_directory_path() / ("atx-fit-serialize-bench-t" + std::to_string(n_threads));
      std::error_code ec;
      fs::remove_all(db_root, ec);
      auto db = SurfaceDb::create(db_root.string());
      if (!db.has_value()) {
        state.SkipWithError("SurfaceDb::create failed");
        return;
      }
      SurfaceDbPopulateConfig cfg;
      cfg.n_threads = n_threads;
      cfg.skip_existing = false; // re-fit+re-serialize every date each rep

      const auto t0 = std::chrono::steady_clock::now();
      auto st = populate_surface_db(*db, u.boards, cfg);
      const auto t1 = std::chrono::steady_clock::now();
      if (!st.has_value()) {
        state.SkipWithError("populate_surface_db failed");
        return;
      }
      samples.push_back(std::chrono::duration<double, std::milli>(t1 - t0).count());
      n_ok = st->n_ok;
      if (rep == 0) {
        archive_bytes = sum_partition_bytes(*db);
      }
      benchmark::DoNotOptimize(st->n_ok);
      fs::remove_all(db_root, ec);
    }
    ms_median = median(samples);
    cv_pct = 100.0 * stat_cv(samples); // stat_cv = stddev/mean (bench_util.hpp)
    benchmark::ClobberMemory();
  }

  // Correctness gate (Debug): every planned board must have fit+serialized. A
  // dropped board would silently inflate surfaces/s, so abort the row instead.
  if (n_ok != u.n_planned) {
    state.SkipWithError("populate dropped a board (n_ok != n_planned)");
    return;
  }

  const double surfaces = static_cast<double>(n_ok);
  const double surfaces_per_s = ms_median > 0.0 ? surfaces / (ms_median / 1000.0) : 0.0;
  state.counters["surfaces_per_s"] = surfaces_per_s;
  state.counters["ms_median"] = ms_median;
  state.counters["cv_pct"] = cv_pct;
  state.counters["provisional"] = 1.0; // host BUSY w/ sibling agents (sprint §3)
  state.counters["threads"] = static_cast<double>(n_threads);
  state.counters["surfaces"] = surfaces;
  state.counters["n_planned"] = static_cast<double>(u.n_planned);
  state.counters["dates"] = static_cast<double>(u.dates);
  state.counters["symbols_per_date"] = static_cast<double>(u.symbols);
  state.counters["in_flight_surfaces"] = static_cast<double>(u.symbols); // per-date group (U1 RSS bound)
  state.counters["archive_bytes"] = static_cast<double>(archive_bytes);
  state.counters["bytes_per_surface"] =
      surfaces > 0.0 ? static_cast<double>(archive_bytes) / surfaces : 0.0;
}

// Iterations(1) / Repetitions(1): the internal kReps loop owns the statistics, so
// Google Benchmark drives the body exactly once and the counters carry the median +
// CV. (apply_common's aggregate machinery is intentionally not used — this row
// computes its own median/CV so cv_pct is a first-class stamped counter.)
void register_row(unsigned n_threads) {
  const std::string name = "fit_serialize/populate/threads:" + std::to_string(n_threads);
  benchmark::RegisterBenchmark(
      name, [n_threads](benchmark::State &st) { run_fit_serialize(st, n_threads); })
      ->Iterations(1)
      ->Repetitions(1)
      ->Unit(benchmark::kMillisecond)
      ->UseRealTime();
}

const int kRegistered = [] {
  // Serial baseline + a multi-worker row (>=6 eff cores is the universe-date
  // target envelope). hardware_concurrency clamped to a sane bound so a huge box
  // does not register an unbounded thread count.
  const unsigned hw = std::thread::hardware_concurrency();
  const unsigned many = hw == 0 ? 6u : (hw < 2u ? 2u : (hw > 16u ? 16u : hw));
  register_row(1u);
  if (many > 1u) {
    register_row(many);
  }
  return 0;
}();

} // namespace
} // namespace atx::vol::bench
