// Whole-universe cycle-time harness (Sub-Sprint S / S6) — HARNESS ONLY.
//
// ── Metric ─────────────────────────────────────────────────────────────────
// One full universe cycle wall = ingest -> fit -> archive-write, over a named
// universe, with a per-stage breakdown:
//   * ingest_ms   — load_opra_daterange (the S4 parallel + projected OPRA path)
//                   over the universe root: parquet decode + panel construction.
//   * fit_ms      — for every loaded board: OptionChain::from_frame + the blessed
//                   PricerFitter (the production surface fit), run as a BLACK BOX
//                   (no edits to any Sprint-R TU).
//   * archive_ms  — serialize every fitted surface via the blessed
//                   write_surface_archive_v2_file writer (ATXVSA2 / v2).
//   * total_ms    — ingest + fit + archive.
// Reported as Google Benchmark counters (JSON via --benchmark_format=json), one
// Iterations(1) corpus-style row (the W0.1 pattern) so the cycle runs exactly
// once per repetition and the counters are the per-op stage walls.
//
// ── JSON schema (per benchmark row) ────────────────────────────────────────
//   name:            "universe/cycle/smoke3"
//   iterations:      1
//   real_time:       total cycle wall (ns) — Google Benchmark's own timing
//   counters:
//     ingest_ms, fit_ms, archive_ms, total_ms  (wall per stage, ms)
//     n_names            requested universe size
//     n_boards_loaded    boards ingested Ok
//     n_boards_fitted    boards the blessed fit admitted
//     n_boards_archived  surfaces written to the archive
//
// ── NOT recorded this sprint ───────────────────────────────────────────────
// NO universe baseline / no gate. The absolute number is meaningless until
// Sprint R lands the pipeline connection and Sprint I wires the seams (per the
// plan). This harness only proves the metric is well-defined, the schema is
// stable, and the black-box cycle runs — here on a self-contained 3-name smoke
// universe (synthetic parquet written to a temp dir, so there is no external
// fixture dependency). The SpiderRock comparison (~45 s full-universe envelope)
// is a Sprint-G row, not this one.

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <benchmark/benchmark.h>

#include "atx/core/io/parquet_writer.hpp"  // write_parquet, WriteColumn
#include "atx/vol/black76.hpp"             // black76_price
#include "atx/vol/chain.hpp"              // OptionChain
#include "atx/vol/corpus.hpp"            // CorpusBoard
#include "atx/vol/opra_batch.hpp"        // load_opra_daterange, corpus_board_from_opra
#include "atx/vol/pricer_fitter.hpp"     // PricerFitter, PricerConfig
#include "atx/vol/session.hpp"           // FitPreset
#include "atx/vol/surface_archive.hpp"   // write_surface_archive_v2_file
#include "atx/vol/surface_db.hpp"        // SurfaceDb, symbol_config_from_preset
#include "atx/vol/tools/surface_db_populate.hpp"  // populate_surface_db (U1-U4 + E2)
#include "atx/vol/vol_curve.hpp"         // CurveConfig

namespace {

namespace fs = std::filesystem;
namespace io = atx::core::io;
using atx::i64;
using namespace atx::vol;

// OSI/OCC 21-char symbol: 6-char space-padded root + YYMMDD + C/P + 8-digit
// strike (price × 1000).
[[nodiscard]] std::string osi_sym(std::string root, const std::string& yymmdd, char cp,
                                  double strike) {
  root.resize(6, ' ');
  char buf[9];
  std::snprintf(buf, sizeof(buf), "%08lld",
                static_cast<long long>(std::llround(strike * 1000.0)));
  return root + yymmdd + std::string(1, cp) + std::string(buf);
}

// Write a multi-expiry flat-vol board for one name to a parquet file (the 8 OPRA
// columns the projected loader consumes). Two expiries × ~19 strikes give the
// blessed fitter a term structure + coverage to admit. Prices in nano-dollars.
void write_board(const fs::path& path, const std::string& symbol, double F, double vol) {
  // (yymmdd, year-fraction to snapshot 2026-06-05) — a near and a far expiry.
  const std::array<std::pair<const char*, double>, 2> expiries = {
      {{"260717", 0.115}, {"260918", 0.290}}};
  const double df = 1.0;
  constexpr int kStrikes = 19;
  const auto to_px = [](double d) { return static_cast<i64>(std::llround(d * 1e9)); };

  std::vector<i64> ts_col;
  std::vector<std::string> und_col;
  std::vector<std::string> sym_col;
  std::vector<i64> bidpx;
  std::vector<i64> askpx;
  std::vector<i64> bidsz;
  std::vector<i64> asksz;
  // Distinct strikes on a fixed "nice" step centred at ATM (round() over a
  // fractional grid can collide, which data_install rejects as a duplicate row).
  const double step = std::max(1.0, std::round(F * 0.03));
  const double atm = std::round(F / step) * step;
  for (const auto& [yymmdd, T] : expiries) {
    for (int i = 0; i < kStrikes; ++i) {
      const double K = atm + static_cast<double>(i - (kStrikes - 1) / 2) * step;
      if (K <= 0.0) {
        continue;
      }
      for (char cp : {'C', 'P'}) {
        const auto side = (cp == 'C') ? Side::Call : Side::Put;
        const double mid = black76_price(F, K, T, vol, df, side);
        if (mid < 0.10) {
          continue;  // skip near-zero deep-OTM legs (negative bids => bad rows)
        }
        ts_col.push_back(1780000000000000000LL);
        und_col.push_back(symbol);
        sym_col.push_back(osi_sym(symbol, yymmdd, cp, K));
        bidpx.push_back(to_px(std::max(mid - 0.05, 0.01)));
        askpx.push_back(to_px(mid + 0.05));
        bidsz.push_back(10);
        asksz.push_back(10);
      }
    }
  }
  const std::vector<io::WriteColumn> cols = {
      {"ts", std::span<const i64>(ts_col)},
      {"underlying", std::span<const std::string>(und_col)},
      {"symbol", std::span<const std::string>(sym_col)},
      {"bid_px", std::span<const i64>(bidpx)},
      {"ask_px", std::span<const i64>(askpx)},
      {"bid_sz", std::span<const i64>(bidsz)},
      {"ask_sz", std::span<const i64>(asksz)},
  };
  fs::create_directories(path.parent_path());
  fs::remove(path);
  (void)io::write_parquet(cols, path.string());
}

// The 3-name smoke universe: written once, reused across repetitions.
[[nodiscard]] const std::string& smoke_root() {
  static const std::string root = [] {
    const fs::path r = fs::temp_directory_path() / "atx_universe_cycle_smoke";
    fs::remove_all(r);
    write_board(r / "AAA" / "2026-06-05.parquet", "AAA", 100.0, 0.22);
    write_board(r / "BBB" / "2026-06-05.parquet", "BBB", 55.0, 0.35);
    write_board(r / "CCC" / "2026-06-05.parquet", "CCC", 250.0, 0.18);
    return r.string();
  }();
  return root;
}

[[nodiscard]] double ms_since(std::chrono::steady_clock::time_point t0) {
  const auto t1 = std::chrono::steady_clock::now();
  return std::chrono::duration<double, std::milli>(t1 - t0).count();
}

void BM_UniverseCycle_Smoke3(benchmark::State& state) {
  const std::string root = smoke_root();

  for (auto _ : state) {
    double ingest_ms = 0.0;
    double fit_ms = 0.0;
    double archive_ms = 0.0;
    std::size_t n_loaded = 0;
    std::size_t n_fitted = 0;
    std::size_t n_archived = 0;

    // ── Stage 1: ingest (S4 parallel + projected loader) ──────────────────
    OpraBatchSpec spec;
    spec.symbols = {"AAA", "BBB", "CCC"};
    spec.date_lo = "2026-06-05";
    spec.date_hi = "2026-06-05";
    spec.root_dir = root;
    spec.r = 0.02;
    const auto t_ing = std::chrono::steady_clock::now();
    auto batch = load_opra_daterange(spec);
    ingest_ms = ms_since(t_ing);

    // ── Stage 2: fit (blessed PricerFitter, black box) ────────────────────
    std::vector<PricedSurface> priced;   // owns the surfaces for the archive
    std::vector<std::string> symbols;
    if (batch.has_value()) {
      for (const OpraBatchEntry& entry : batch->entries) {
        if (!entry.panel.has_value()) {
          continue;
        }
        ++n_loaded;
        const QuoteFrame& frame = entry.panel->frame;
        const MarketEnv env = market_env_from_frame(frame);
        const auto t_fit = std::chrono::steady_clock::now();
        auto chain = OptionChain::from_frame(frame, env);
        if (!chain.has_value()) {
          fit_ms += ms_since(t_fit);
          continue;
        }
        PricerConfig config;
        config.quality_mode = FitQualityMode::Balanced;
        config.outputs = SurfaceOutputs::MarketMarkAndRisk;
        PricerFitter fitter{config};
        const Status fitted = fitter.fit(*chain);
        fit_ms += ms_since(t_fit);
        if (!fitted.has_value() || fitter.surface() == nullptr) {
          continue;
        }
        auto snapshot = fitter.surface()->session().to_priced_surface();
        if (!snapshot.has_value()) {
          continue;
        }
        ++n_fitted;
        priced.push_back(std::move(snapshot.value()));
        symbols.push_back(entry.symbol);
      }
    }

    // ── Stage 3: archive write (blessed writer) ───────────────────────────
    const auto t_arch = std::chrono::steady_clock::now();
    if (!priced.empty()) {
      std::vector<SurfaceArchiveItem> items;
      items.reserve(priced.size());
      for (std::size_t i = 0; i < priced.size(); ++i) {
        items.push_back(SurfaceArchiveItem{symbols[i], &priced[i], std::nullopt});
      }
      const fs::path apath = fs::temp_directory_path() / "atx_universe_cycle_smoke.atxvsa";
      // ATXVSA2 (v2) writer — the blessed archive writer after the WS-S S4
      // clean-break cutover; same SurfaceArchiveItem inputs as the v1 writer.
      const Status w = write_surface_archive_v2_file(
          apath.string(), std::span<const SurfaceArchiveItem>{items});
      if (w.has_value()) {
        n_archived = priced.size();
      }
    }
    archive_ms = ms_since(t_arch);

    state.counters["ingest_ms"] = ingest_ms;
    state.counters["fit_ms"] = fit_ms;
    state.counters["archive_ms"] = archive_ms;
    state.counters["total_ms"] = ingest_ms + fit_ms + archive_ms;
    state.counters["n_names"] = 3.0;
    state.counters["n_boards_loaded"] = static_cast<double>(n_loaded);
    state.counters["n_boards_fitted"] = static_cast<double>(n_fitted);
    state.counters["n_boards_archived"] = static_cast<double>(n_archived);
  }
}

// ── U6: real multi-name universe-cycle mechanism proof ──────────────────────
// The smoke row above is a SELF-CONTAINED harness (synthetic 3-name universe, its
// own manual fit loop) that predates the streaming SurfaceDb pipeline. This row
// instead drives the LANDED production universe machinery end-to-end on REAL OPRA
// boards on disk:
//   ingest  = load_opra_daterange (the parallel + projected OPRA loader; W4.3)
//   fit     = populate_surface_db — U1 streaming per-date partition release,
//             U2 Longest-Processing-Time claim order, U3 date-granular durability,
//             U4 shared small-book worker budget, over the E2 help-first
//             work-stealing pool. The per-date archive write happens INSIDE the
//             fit drain (streaming), so archive folds into fit and
//             ingest_ms + fit_ms == the full universe cycle.
// Env-configured so it stays OFF unless a cohort is supplied — the smoke row and
// CI baseline are untouched (this row is registered only when the root env is
// set):
//   ATX_UNIVERSE_OPRA_ROOT  parquet hive root; files at <root>/{sym}/{date}.parquet
//   ATX_UNIVERSE_SYMBOLS    CSV cohort, e.g. "AAPL,AMZN,SPY,XOM"
//   ATX_UNIVERSE_DATE_LO    "YYYY-MM-DD" inclusive lower (default = _DATE_HI)
//   ATX_UNIVERSE_DATE_HI    "YYYY-MM-DD" inclusive upper (default = _DATE_LO)
//   ATX_UNIVERSE_WORKERS    ingest+fit workers; 0 = auto (hw concurrency), 1 = serial
//   ATX_UNIVERSE_R          flat fallback rate (default 0.043)
// Counters mirror the smoke schema (ingest_ms/fit_ms/archive_ms/total_ms + board
// counts) plus `workers`, so a worker sweep (one process per count) reads a stable
// schema. Best-of-3 via Repetitions; a fresh temp SurfaceDb per repetition so
// skip_existing never short-circuits the fit.

[[nodiscard]] std::string env_str(const char* name) {
#if defined(_MSC_VER)
  char* raw = nullptr;
  std::size_t n = 0;
  if (::_dupenv_s(&raw, &n, name) != 0 || raw == nullptr) {
    return {};
  }
  std::string v(raw);
  std::free(raw);
  return v;
#else
  const char* raw = std::getenv(name);
  return raw != nullptr ? std::string(raw) : std::string{};
#endif
}

[[nodiscard]] std::vector<std::string> split_csv(const std::string& s) {
  std::vector<std::string> out;
  std::size_t start = 0;
  while (start <= s.size()) {
    const std::size_t end = s.find(',', start);
    const std::string field =
        s.substr(start, end == std::string::npos ? s.size() - start : end - start);
    if (!field.empty()) {
      out.push_back(field);
    }
    if (end == std::string::npos) {
      break;
    }
    start = end + 1;
  }
  return out;
}

void BM_UniverseCycle_Real(benchmark::State& state) {
  const std::string opra_root = env_str("ATX_UNIVERSE_OPRA_ROOT");
  const std::vector<std::string> symbols = split_csv(env_str("ATX_UNIVERSE_SYMBOLS"));
  std::string date_hi = env_str("ATX_UNIVERSE_DATE_HI");
  std::string date_lo = env_str("ATX_UNIVERSE_DATE_LO");
  if (date_hi.empty()) {
    date_hi = date_lo;
  }
  if (date_lo.empty()) {
    date_lo = date_hi;
  }
  if (opra_root.empty() || symbols.empty() || date_lo.empty()) {
    state.SkipWithError(
        "set ATX_UNIVERSE_OPRA_ROOT / _SYMBOLS / _DATE_LO[/_DATE_HI] to run the real cohort");
    return;
  }
  const std::string workers_s = env_str("ATX_UNIVERSE_WORKERS");
  const unsigned workers =
      workers_s.empty() ? 0u : static_cast<unsigned>(std::strtoul(workers_s.c_str(), nullptr, 10));
  const std::string r_s = env_str("ATX_UNIVERSE_R");
  const double r = r_s.empty() ? 0.043 : std::strtod(r_s.c_str(), nullptr);

  for (auto _ : state) {
    // Fresh db per repetition so skip_existing never short-circuits the fit.
    const fs::path db_root = fs::temp_directory_path() / "atx_universe_cycle_real_db";
    fs::remove_all(db_root);
    Result<SurfaceDb> db = SurfaceDb::create(db_root.string());
    if (!db.has_value()) {
      state.SkipWithError("SurfaceDb::create failed");
      return;
    }
    for (const std::string& sym : symbols) {
      SymbolFitConfig c = symbol_config_from_preset(FitPreset::Fast);
      c.pin_curve = true;
      c.curve = CurveConfig{};  // default pinned ConvexDense (mirrors mag7_surfdb_populate)
      (void)db->upsert_symbol(sym, c);
    }

    // ── Stage 1: ingest (parallel projected OPRA loader) ──────────────────
    OpraBatchSpec spec;
    spec.symbols = symbols;
    spec.date_lo = date_lo;
    spec.date_hi = date_hi;
    spec.root_dir = opra_root;
    spec.r = r;
    spec.n_threads = workers;
    const auto t_ing = std::chrono::steady_clock::now();
    auto batch = load_opra_daterange(spec);
    const double ingest_ms = ms_since(t_ing);

    std::vector<CorpusBoard> boards;
    std::size_t n_loaded = 0;
    if (batch.has_value()) {
      boards.reserve(batch->n_loaded);
      for (OpraBatchEntry& e : batch->entries) {
        if (!e.panel.has_value()) {
          continue;  // NotFound (weekend/holiday) or a load failure — non-fatal
        }
        ++n_loaded;
        boards.push_back(corpus_board_from_opra(e.date, e.symbol, std::move(*e.panel)));
      }
    }

    // ── Stage 2+3: fit + streaming per-date archive write (populate) ──────
    double fit_ms = 0.0;
    std::size_t n_ok = 0;
    std::size_t n_dates_written = 0;
    if (!boards.empty()) {
      SurfaceDbPopulateConfig cfg;
      cfg.n_threads = workers;
      cfg.skip_existing = false;
      const auto t_fit = std::chrono::steady_clock::now();
      const Result<SurfaceDbPopulateStats> st = populate_surface_db(*db, boards, cfg);
      fit_ms = ms_since(t_fit);
      if (st.has_value()) {
        n_ok = st->n_ok;
        n_dates_written = st->n_dates_written;
      }
    }

    state.counters["ingest_ms"] = ingest_ms;
    state.counters["fit_ms"] = fit_ms;
    state.counters["archive_ms"] = 0.0;  // folded into fit (streaming writer)
    state.counters["total_ms"] = ingest_ms + fit_ms;
    state.counters["workers"] = static_cast<double>(workers);
    state.counters["n_names"] = static_cast<double>(symbols.size());
    state.counters["n_boards_loaded"] = static_cast<double>(n_loaded);
    state.counters["n_boards_fitted"] = static_cast<double>(n_ok);
    state.counters["n_dates_written"] = static_cast<double>(n_dates_written);
  }
}

// Register the real-cohort row ONLY when a cohort root is supplied, so the
// default (smoke) run and its CI baseline see exactly the smoke3 row and nothing
// changes. Env is read at static-init (set before process launch), so this is a
// clean compile-in-always / register-on-demand gate.
[[maybe_unused]] const int kRegisterUniverseReal = [] {
  if (!env_str("ATX_UNIVERSE_OPRA_ROOT").empty()) {
    benchmark::RegisterBenchmark("universe/cycle/real", BM_UniverseCycle_Real)
        ->Iterations(1)
        ->Repetitions(3)
        ->UseRealTime();
  }
  return 0;
}();

}  // namespace

// Iterations(1) corpus-style (W0.1): one full cycle per repetition; the counters
// carry the per-stage walls. No baseline / no gate this sprint.
BENCHMARK(BM_UniverseCycle_Smoke3)
    ->Name("universe/cycle/smoke3")
    ->Iterations(1)
    ->Repetitions(3)
    ->UseRealTime();
