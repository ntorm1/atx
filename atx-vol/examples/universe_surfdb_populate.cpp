// universe_surfdb_populate.cpp — WS-F F-c: fit + serialize the SPY-dispersion
// universe (SPY index leg + top-N single names) across a YTD date range from the
// real OPRA parquet hive into a SurfaceDb, via the fused streaming
// populate_universe_streaming (surface_db_populate.hpp).
//
// Resumable / idempotent (cell-aware): re-running as the pull dribbles in new
// (symbol,date) cells fits ONLY the new work; a re-run over unchanged data fits
// ZERO. Graceful gaps: a missing/incomplete parquet cell is a logged skip, not an
// error (load_opra_daterange returns it as NotFound/load-error, non-fatal). The
// OPRA hive is READ-ONLY (a pull may be writing it) — this driver only reads it.
//
//   universe_surfdb_populate [--opra-root DIR] [--db DIR]
//       (--universe FILE.tsv | --symbols A,B,...) [--index-symbol SPY]
//       [--start YYYY-MM-DD] [--end YYYY-MM-DD] [--preset fast]
//       [--fit-workers N] [--r 0.043] [--stats FILE]
//
// Defaults target the WS-D hive + a sibling non-committed db root; see --help.
// Gate test: SurfaceDbPopulate.UniverseStreamingResumeOverRealHive
// (tests/surface_db_populate_test.cpp). OFF by default (ATX_BUILD_EXAMPLES).

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

#include "atx/vol/corpus.hpp"              // CorpusBoard
#include "atx/vol/opra_batch.hpp"          // OpraBatchSpec, load_opra_daterange, corpus_board_from_opra
#include "atx/vol/detail/parallel_for.hpp"        // atx_auto_worker_count
#include "atx/vol/run_report.hpp"          // MetaKv
#include "atx/vol/session.hpp"             // FitPreset
#include "atx/vol/surface_db.hpp"          // SurfaceDb
#include "atx/vol/surface_db_populate.hpp" // populate_universe_streaming, write_populate_stats_csv
#include "atx/vol/types.hpp"               // Result, Status

using namespace atx::vol;

namespace {

std::vector<std::string> split_csv(std::string_view csv) {
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

// Read the `symbol` column from the D1 universe fixture (tab- OR comma-separated,
// header row names the columns; the committed atx-vol universe schema is
// `effective_date, symbol, raw_weight, source, as_of`). Falls back to column 1
// when no header cell is named "symbol". Blank/`#`-comment lines are skipped.
std::vector<std::string> read_universe_file(const std::string &path) {
  std::vector<std::string> out;
  std::ifstream in(path);
  if (!in) {
    return out;
  }
  const auto split = [](const std::string &line) {
    std::vector<std::string> cols;
    std::string cur;
    for (const char ch : line) {
      if (ch == '\t' || ch == ',') {
        cols.push_back(cur);
        cur.clear();
      } else if (ch != '\r') {
        cur.push_back(ch);
      }
    }
    cols.push_back(cur);
    return cols;
  };
  std::string line;
  std::size_t sym_col = 1; // default: 2nd column (effective_date, symbol, ...)
  bool header_done = false;
  while (std::getline(in, line)) {
    if (line.empty() || line[0] == '#') {
      continue;
    }
    const std::vector<std::string> cols = split(line);
    if (!header_done) {
      header_done = true;
      for (std::size_t i = 0; i < cols.size(); ++i) {
        if (cols[i] == "symbol") {
          sym_col = i;
          break;
        }
      }
      continue; // skip the header row
    }
    if (sym_col < cols.size() && !cols[sym_col].empty()) {
      out.push_back(cols[sym_col]);
    }
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
  // Defaults: the WS-D hive + a sibling NON-committed db root (both on the data
  // drive, never in git — the disposable-worktree rule).
  std::string opra_root = "C:/atx-data/spy-dispersion/opra";
  std::string db_root = "C:/atx-data/spy-dispersion/surfdb-ytd";
  std::string universe_file;
  std::string symbols_csv;
  std::string index_symbol = "SPY";
  std::string start = "2026-01-02";
  std::string end = "2026-07-17";
  std::string preset_name = "fast";
  double r = 0.043;
  unsigned fit_workers = atx_auto_worker_count(); // honors ATX_VOL_FIT_WORKERS
  std::string stats_path;

  for (int i = 1; i < argc; ++i) {
    const std::string_view a = argv[i];
    const auto nv = [&]() -> const char * { return (i + 1 < argc) ? argv[++i] : ""; };
    if (a == "--opra-root") {
      opra_root = nv();
    } else if (a == "--db") {
      db_root = nv();
    } else if (a == "--universe") {
      universe_file = nv();
    } else if (a == "--symbols") {
      symbols_csv = nv();
    } else if (a == "--index-symbol") {
      index_symbol = nv();
    } else if (a == "--start") {
      start = nv();
    } else if (a == "--end") {
      end = nv();
    } else if (a == "--preset") {
      preset_name = nv();
    } else if (a == "--fit-workers") {
      fit_workers = static_cast<unsigned>(std::strtoul(nv(), nullptr, 10));
    } else if (a == "--r") {
      r = std::strtod(nv(), nullptr);
    } else if (a == "--stats") {
      stats_path = nv();
    } else if (a == "--help" || a == "-h") {
      std::printf("usage: universe_surfdb_populate [--opra-root DIR] [--db DIR] "
                  "(--universe FILE | --symbols A,B,...) [--index-symbol SPY] "
                  "[--start YYYY-MM-DD] [--end YYYY-MM-DD] [--preset fast] "
                  "[--fit-workers N] [--r 0.043] [--stats FILE]\n");
      return 0;
    } else {
      std::fprintf(stderr, "unknown arg: %s\n", argv[i]);
      return 2;
    }
  }

  std::vector<std::string> symbols =
      !symbols_csv.empty() ? split_csv(symbols_csv) : read_universe_file(universe_file);
  if (symbols.empty()) {
    std::fprintf(stderr,
                 "no symbols: pass --symbols A,B,... or --universe FILE (with a 'symbol' column)\n");
    return 2;
  }
  const FitPreset preset = parse_preset(preset_name);

  Result<SurfaceDb> db = SurfaceDb::open(db_root);
  if (!db) {
    db = SurfaceDb::create(db_root);
  }
  if (!db) {
    std::fprintf(stderr, "SurfaceDb::open/create(%s): %s\n", db_root.c_str(),
                 db.error().to_string().c_str());
    return 1;
  }

  // ── Load the READ-ONLY hive (missing/partial cells are non-fatal) ─────────
  OpraBatchSpec spec;
  spec.symbols = symbols;
  spec.date_lo = start;
  spec.date_hi = end;
  spec.root_dir = opra_root;
  spec.r = r; // snapshot_suffix/path_template keep 19:55Z / {symbol}/{date} defaults
  const Result<OpraBatchResult> batch = load_opra_daterange(spec);
  if (!batch) {
    std::fprintf(stderr, "load_opra_daterange: %s\n", batch.error().to_string().c_str());
    return 1;
  }
  std::printf("[opra] %s..%s  cells: loaded=%zu missing=%zu error=%zu of %zu\n", start.c_str(),
              end.c_str(), batch->n_loaded, batch->n_missing, batch->n_error, batch->n_total);

  std::vector<CorpusBoard> boards;
  boards.reserve(batch->n_loaded);
  for (const OpraBatchEntry &e : batch->entries) {
    if (!e.panel) {
      continue; // NotFound (weekend/holiday/un-pulled) or a load failure — logged skip
    }
    boards.push_back(corpus_board_from_opra(e.date, e.symbol, *e.panel));
  }

  // ── Fused streaming fit -> serialize v2, cell-aware idempotent resume ─────
  UniversePopulateSpec upspec;
  upspec.index_symbol = index_symbol;
  upspec.preset = preset;
  upspec.fit_workers = fit_workers;
  const Result<UniversePopulateCoverage> cov = populate_universe_streaming(*db, boards, upspec);
  if (!cov) {
    std::fprintf(stderr, "populate_universe_streaming: %s\n", cov.error().to_string().c_str());
    return 1;
  }

  // `carried` sits beside `refit` (FIX-D fix-1, I2): they are the two halves of
  // what a rewritten date did with its already-present cells, and a converged
  // carry resume prints refit=0 ok=0 — which without `carried` reads as a no-op.
  // `carried_disabled` (FIX-E) is the third: stored cells of a switched-off symbol
  // that a rewrite PRESERVED rather than deleted.
  std::printf("[populate] loaded=%u to_fit=%u refit=%u carried=%u carried_disabled=%u "
              "already_present=%u ok=%u failed=%u | dates: total=%u written=%u "
              "skipped_complete=%u would_drop=%u\n",
              cov->cells_loaded, cov->cells_to_fit, cov->cells_refit, cov->cells_carried,
              cov->cells_carried_disabled, cov->cells_already_present, cov->cells_ok,
              cov->cells_failed, cov->dates_total, cov->dates_written,
              cov->dates_skipped_complete, cov->dates_skipped_would_drop);
  std::printf("[populate] db=%s\n", db_root.c_str());

  // Reuse the populate stats CSV shape for the per-symbol report.
  SurfaceDbPopulateStats stats;
  stats.n_boards = cov->cells_loaded;
  stats.n_ok = cov->cells_ok;
  stats.n_failed = cov->cells_failed;
  stats.n_carried = cov->cells_carried; // FIX-D fix-1 (I2): reaches the CSV meta block
  stats.n_dates_written = cov->dates_written;
  stats.per_symbol = cov->per_symbol;
  const std::string out_stats = stats_path.empty() ? (db_root + "/populate_stats.csv") : stats_path;
  const MetaKv meta{{"opra_root", opra_root}, {"db_root", db_root}, {"start", start},
                    {"end", end},            {"preset", preset_name},
                    {"index_symbol", index_symbol}, {"n_symbols", std::to_string(symbols.size())},
                    {"cells_to_fit", std::to_string(cov->cells_to_fit)}};
  const Status w = write_populate_stats_csv(stats, meta, out_stats);
  if (!w) {
    std::fprintf(stderr, "write_populate_stats_csv: %s\n", w.error().to_string().c_str());
    return 1;
  }
  std::printf("[populate] stats: %s\n", out_stats.c_str());
  return 0;
}
