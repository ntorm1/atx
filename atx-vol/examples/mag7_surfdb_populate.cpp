// mag7_surfdb_populate.cpp — fit a real OPRA parquet hive (MAG7 + SPY, or any
// symbol set) into a SurfaceDb via populate_surface_db, honoring per-symbol
// manifest configs. Gate test: SurfaceDbPopulate
// (tests/surface_db_populate_test.cpp). OFF by default (ATX_BUILD_EXAMPLES).
//
//   mag7_surfdb_populate --opra-root DIR --db DIR --symbols A,B,... \
//       --start YYYY-MM-DD --end YYYY-MM-DD \
//       [--r 0.043] [--preset fast] [--fit-workers N] [--stats FILE]
//
// Flow: parse args -> SurfaceDb::open(db) else SurfaceDb::create(db) -> for
// each requested symbol absent from the manifest, upsert a config mirroring
// the SPY YTD corpus fit policy (spy_ytd_corpus.cpp:94-101: pinned
// ConvexDense, since a penny-dense board deterministically auto-selects it
// anyway) -> load_opra_daterange -> corpus_board_from_opra per loaded entry
// (skip !entry.panel, count missing) -> populate_surface_db ->
// write_populate_stats_csv -> print summary.

#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>

#include "atx/vol/corpus.hpp"              // CorpusBoard
#include "atx/vol/opra_batch.hpp"          // OpraBatchSpec, load_opra_daterange, corpus_board_from_opra
#include "atx/vol/detail/parallel_for.hpp"        // atx_auto_worker_count
#include "atx/vol/tools/run_report.hpp"          // MetaKv
#include "atx/vol/session.hpp"             // FitPreset
#include "atx/vol/surface_db.hpp"          // SurfaceDb, symbol_config_from_preset
#include "atx/vol/tools/surface_db_populate.hpp" // populate_surface_db, write_populate_stats_csv
#include "atx/vol/types.hpp"               // Result, Status
#include "atx/vol/vol_curve.hpp"           // CurveConfig

using namespace atx::vol;

namespace {

std::vector<std::string> split_symbols(std::string_view csv) {
  std::vector<std::string> out;
  std::size_t start = 0;
  while (start <= csv.size()) {
    const std::size_t end = csv.find(',', start);
    const std::string_view field =
        csv.substr(start, end == std::string_view::npos ? csv.size() - start : end - start);
    if (!field.empty()) {
      out.emplace_back(field);
    }
    if (end == std::string_view::npos) {
      break;
    }
    start = end + 1;
  }
  return out;
}

FitPreset parse_preset(std::string_view name) {
  if (name == "accurate") {
    return FitPreset::Accurate;
  }
  if (name == "robust") {
    return FitPreset::Robust;
  }
  if (name == "hft") {
    return FitPreset::Hft;
  }
  return FitPreset::Fast;
}

} // namespace

int main(int argc, char **argv) {
  std::string opra_root;
  std::string db_root;
  std::string symbols_csv;
  std::string start;
  std::string end;
  double r = 0.043;
  std::string preset_name = "fast";
  unsigned fit_workers = atx_auto_worker_count(); // honors ATX_VOL_FIT_WORKERS
  std::string stats_path;

  for (int i = 1; i < argc; ++i) {
    const std::string_view a = argv[i];
    const auto nv = [&]() -> const char * { return (i + 1 < argc) ? argv[++i] : ""; };
    if (a == "--opra-root") {
      opra_root = nv();
    } else if (a == "--db") {
      db_root = nv();
    } else if (a == "--symbols") {
      symbols_csv = nv();
    } else if (a == "--start") {
      start = nv();
    } else if (a == "--end") {
      end = nv();
    } else if (a == "--r") {
      r = std::strtod(nv(), nullptr);
    } else if (a == "--preset") {
      preset_name = nv();
    } else if (a == "--fit-workers") {
      fit_workers = static_cast<unsigned>(std::strtoul(nv(), nullptr, 10));
    } else if (a == "--stats") {
      stats_path = nv();
    } else {
      std::fprintf(stderr, "unknown arg: %s\n", argv[i]);
      return 2;
    }
  }

  if (opra_root.empty() || db_root.empty() || symbols_csv.empty() || start.empty() || end.empty()) {
    std::fprintf(stderr,
                 "usage: mag7_surfdb_populate --opra-root DIR --db DIR --symbols A,B,... "
                 "--start YYYY-MM-DD --end YYYY-MM-DD [--r 0.043] [--preset fast] "
                 "[--fit-workers N] [--stats FILE]\n");
    return 2;
  }

  const std::vector<std::string> symbols = split_symbols(symbols_csv);
  if (symbols.empty()) {
    std::fprintf(stderr, "no symbols given\n");
    return 2;
  }
  const FitPreset preset = parse_preset(preset_name);

  // ── Open or create the SurfaceDb ──────────────────────────────────────────
  Result<SurfaceDb> db = SurfaceDb::open(db_root);
  if (!db) {
    db = SurfaceDb::create(db_root);
  }
  if (!db) {
    std::fprintf(stderr, "SurfaceDb::open/create(%s): %s\n", db_root.c_str(),
                 db.error().to_string().c_str());
    return 1;
  }

  // ── Register any requested symbol not already in the manifest ────────────
  for (const std::string &sym : symbols) {
    if (db->symbol_config(sym).has_value()) {
      continue; // already configured (resumed run / operator override) -- leave it
    }
    auto c = symbol_config_from_preset(preset);
    c.pin_curve = true;
    c.curve = CurveConfig{}; // default pinned ConvexDense (node_cap 40)
    const Status up = db->upsert_symbol(sym, c);
    if (!up) {
      std::fprintf(stderr, "upsert_symbol(%s): %s\n", sym.c_str(), up.error().to_string().c_str());
      return 1;
    }
  }

  // ── Load the parquet hive into per-date QuoteFrames ───────────────────────
  OpraBatchSpec spec;
  spec.symbols = symbols;
  spec.date_lo = start;
  spec.date_hi = end;
  spec.root_dir = opra_root;
  spec.r = r; // snapshot_suffix + path_template keep their 19:55Z / {symbol}/{date} defaults

  const Result<OpraBatchResult> batch = load_opra_daterange(spec);
  if (!batch) {
    std::fprintf(stderr, "load_opra_daterange: %s\n", batch.error().to_string().c_str());
    return 1;
  }
  std::printf("[opra] loaded=%zu missing=%zu error=%zu of %zu (%s..%s)\n", batch->n_loaded,
              batch->n_missing, batch->n_error, batch->n_total, start.c_str(), end.c_str());

  // ── Frames -> CorpusBoards (skip missing/failed cells) ────────────────────
  std::vector<CorpusBoard> boards;
  boards.reserve(batch->n_loaded);
  std::size_t n_missing = 0;
  for (const OpraBatchEntry &e : batch->entries) {
    if (!e.panel) {
      ++n_missing; // NotFound (weekend/holiday) or a load failure -- non-fatal
      continue;
    }
    boards.push_back(corpus_board_from_opra(e.date, e.symbol, *e.panel));
  }
  std::printf("[populate] fitting %zu boards (%zu missing) -> %s\n", boards.size(), n_missing,
              db_root.c_str());
  if (boards.empty()) {
    std::fprintf(stderr, "no loadable boards under %s -- did the pull run?\n", opra_root.c_str());
    return 1;
  }

  // ── Fit -> SurfaceDb partitions, honoring each symbol's manifest config ───
  SurfaceDbPopulateConfig cfg;
  cfg.n_threads = fit_workers;
  const Result<SurfaceDbPopulateStats> stats = populate_surface_db(*db, boards, cfg);
  if (!stats) {
    std::fprintf(stderr, "populate_surface_db: %s\n", stats.error().to_string().c_str());
    return 1;
  }
  std::printf(
      "[populate] n_boards=%u n_ok=%u n_failed=%u n_dates_written=%u n_dates_skipped_existing=%u\n",
      stats->n_boards, stats->n_ok, stats->n_failed, stats->n_dates_written,
      stats->n_dates_skipped_existing);

  const std::string out_stats = stats_path.empty() ? (db_root + "/populate_stats.csv") : stats_path;
  const MetaKv meta{
      {"opra_root", opra_root},
      {"db_root", db_root},
      {"start", start},
      {"end", end},
      {"preset", preset_name},
  };
  const Status w = write_populate_stats_csv(*stats, meta, out_stats);
  if (!w) {
    std::fprintf(stderr, "write_populate_stats_csv: %s\n", w.error().to_string().c_str());
    return 1;
  }
  std::printf("[populate] stats: %s\n", out_stats.c_str());
  return 0;
}
