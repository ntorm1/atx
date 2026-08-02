// surface_db_dispersion_backtest.cpp — the surface-db dispersion route's
// example driver: a SurfaceDb + a date window in, the three pinned run_report
// CSVs out. OFF by default (ATX_BUILD_EXAMPLES).
//
//   surface_db_dispersion_backtest --db DIR --from YYYY-MM-DD --to YYYY-MM-DD
//       [--config FILE] [--out DIR] [--index SPY] [--universe FILE]
//
// This shell is DELIBERATELY THIN. The whole run — open db, build the clock,
// window it, resolve the basket, time the engine — is one library call,
// `run_surface_db_dispersion_backtest` (atx/vol/dispersion_surface_db.hpp), which
// the SurfaceDbDispersionBacktest suite gates branch by branch. What is left here
// is argv, artifact paths, a console headline and exit codes: the three things a
// library must not decide.
//
// Exit codes: 0 success, 1 runtime error, 2 usage error.

#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "atx/vol/backtest.hpp"              // BacktestResult
#include "atx/vol/research/backtest_driver.hpp"       // RunOutcome, EngineRunStats
#include "atx/vol/research/dispersion_backtest.hpp"   // DispersionBacktestConfig
#include "atx/vol/dispersion_surface_db.hpp" // SurfaceDbDispersionSpec, the one-call entry
#include "atx/vol/research/dispersion_workflow.hpp"   // read_universe, all_symbols
#include "atx/vol/tools/run_report.hpp"            // MetaKv, write_* emitters
#include "atx/vol/surface_db.hpp"            // SurfaceDb (meta: root + generation)
#include "atx/vol/tools/tearsheet.hpp"             // TearSheet
#include "atx/vol/types.hpp"                 // Result, Status

using namespace atx::vol;
namespace fs = std::filesystem;

namespace {

struct Args {
  std::string db;
  std::string from;
  std::string to;
  std::string config;   // optional: read_dispersion_backtest_config
  std::string out;      // defaults to %TEMP%/atx-surface-db-dispersion
  std::string index_symbol{"SPY"};
  std::string universe; // optional: UniverseRow TSV (point-in-time schedule)
};

void print_usage() {
  std::fprintf(stderr, "usage: surface_db_dispersion_backtest --db DIR --from YYYY-MM-DD "
                       "--to YYYY-MM-DD [--config FILE] [--out DIR] [--index SPY] "
                       "[--universe FILE]\n");
}

// Parse argv into `a`. False (unknown flag / missing required value) -> caller
// prints usage and exits 2.
[[nodiscard]] bool parse_args(int argc, char **argv, Args &a) {
  // EVERY flag here takes a value, and a flag whose value is missing or empty is a
  // USAGE ERROR, not "leave the default". Absorbing it would make a trailing
  // `--config` (or `--universe`) silently run a DIFFERENT backtest than the one
  // the command line describes, and report success.
  bool bad_value = false;
  for (int i = 1; i < argc; ++i) {
    const std::string_view arg = argv[i];
    const auto nv = [&]() -> const char * {
      if (i + 1 >= argc || argv[i + 1][0] == '\0') {
        std::fprintf(stderr, "%s requires a non-empty value\n", argv[i]);
        bad_value = true;
        return "";
      }
      return argv[++i];
    };
    if (arg == "--db") {
      a.db = nv();
    } else if (arg == "--from") {
      a.from = nv();
    } else if (arg == "--to") {
      a.to = nv();
    } else if (arg == "--config") {
      a.config = nv();
    } else if (arg == "--out") {
      a.out = nv();
    } else if (arg == "--index") {
      a.index_symbol = nv();
    } else if (arg == "--universe") {
      a.universe = nv();
    } else {
      std::fprintf(stderr, "unknown arg: %s\n", argv[i]);
      return false;
    }
  }
  if (bad_value) {
    return false;
  }
  if (a.db.empty()) {
    std::fprintf(stderr, "--db is required\n");
    return false;
  }
  if (a.from.empty() || a.to.empty()) {
    std::fprintf(stderr, "--from and --to are required\n");
    return false;
  }
  // Window ORDER and window EMPTINESS are deliberately not checked here:
  // `Clock::between` rejects both and its message names the db's available range,
  // which is strictly more useful than anything this shell could say.
  if (a.out.empty()) {
    a.out = (fs::temp_directory_path() / "atx-surface-db-dispersion").string();
  }
  return true;
}

// Metric-value formatting discipline (%.10g), matching run_report.cpp.
[[nodiscard]] std::string fmt_num(double v) {
  char buf[64];
  std::snprintf(buf, sizeof buf, "%.10g", v);
  return buf;
}

} // namespace

int main(int argc, char **argv) {
  Args args;
  if (!parse_args(argc, argv, args)) {
    print_usage();
    return 2;
  }

  SurfaceDbDispersionSpec spec;
  spec.db_root = args.db;
  spec.date_lo = args.from;
  spec.date_hi = args.to;
  spec.index_symbol = args.index_symbol;
  if (!args.universe.empty()) {
    spec.universe_path = fs::path(args.universe);
  }
  if (!args.config.empty()) {
    auto cfg = read_dispersion_backtest_config(args.config);
    if (!cfg) {
      std::fprintf(stderr, "read_dispersion_backtest_config: %s\n", cfg.error().to_string().c_str());
      return 1;
    }
    spec.config = std::move(*cfg);
  }
  // NB: `spec.config.run.snapshot_cache` is left NULL on purpose — see the entry
  // point's header. A shared cache here would cost a whole-archive copy per date
  // and forfeit the engine's private Sealed mmap on a single-pass replay.

  const auto outcome = run_surface_db_dispersion_backtest(spec);
  if (!outcome) {
    // No stage label of our own: the entry point already names itself AND the
    // stage that failed, so prefixing here would print the function name twice.
    std::fprintf(stderr, "error: %s\n", outcome.error().to_string().c_str());
    return 1;
  }
  const BacktestResult &r = outcome->result;
  const TearSheet &ts = outcome->sheet;
  const EngineRunStats &stats = outcome->stats;

  // Provenance for the meta block. The db is reopened rather than plumbed out of
  // the entry point: `generation` and `root` are manifest metadata that belong to
  // the ARTIFACT, not to the run, and a second manifest parse is one small file
  // read against a run that just priced the whole window.
  auto db = SurfaceDb::open(spec.db_root);
  if (!db) {
    std::fprintf(stderr, "SurfaceDb::open(%s): %s\n", spec.db_root.c_str(),
                 db.error().to_string().c_str());
    return 1;
  }
  // `n_names` is the basket size for the route that actually ran. On the
  // point-in-time route membership varies by step, so the only honest scalar is
  // the count of DISTINCT constituents the schedule ever names (`all_symbols`
  // returns the index plus those, deduped).
  std::size_t n_names = 0;
  if (spec.universe_path) {
    auto rows = read_universe(*spec.universe_path);
    if (!rows) {
      std::fprintf(stderr, "read_universe: %s\n", rows.error().to_string().c_str());
      return 1;
    }
    const std::vector<std::string> symbols = all_symbols(*rows, spec.index_symbol);
    n_names = symbols.empty() ? 0 : symbols.size() - 1; // minus the index leg
  } else {
    auto universe = universe_from_surface_db(*db, spec.index_symbol);
    if (!universe) {
      std::fprintf(stderr, "universe_from_surface_db: %s\n", universe.error().to_string().c_str());
      return 1;
    }
    n_names = universe->names.size();
  }

  std::error_code ec;
  fs::create_directories(args.out, ec);

  // The REQUESTED window clamps to the dates the db actually holds (documented,
  // encouraged behavior — see the operator guide's §8), so `--from/--to` and the
  // range the run really covered can differ. Derived once here and used by BOTH
  // the meta block's `window_resolved` and the headline below, so the two can
  // never disagree. The empty guard is defensive: an empty window is rejected
  // upstream by `Clock::between`, so a zero-step run cannot reach this point.
  const std::string date_lo = r.date.empty() ? std::string("-") : r.date.front();
  const std::string date_hi = r.date.empty() ? std::string("-") : r.date.back();

  // Shared meta block, written verbatim into every emitted file, in this order.
  // `universe` rides along because the two routes build DIFFERENT books and a
  // series.csv read six months from now must say which one produced it.
  const MetaKv meta = {
      {"data_source", "surface_db"},
      {"db_root", db->root()},
      {"db_generation", std::to_string(db->generation())},
      {"window", args.from + ".." + args.to},
      {"window_resolved", date_lo + ".." + date_hi},
      {"index", spec.index_symbol},
      {"n_names", std::to_string(n_names)},
      {"universe", args.universe.empty() ? std::string("surface_db_manifest") : args.universe},
  };

  const std::string series_path = (fs::path(args.out) / "series.csv").string();
  Status st = write_backtest_series_csv(r, meta, series_path);
  if (!st) {
    std::fprintf(stderr, "write_backtest_series_csv: %s\n", st.error().to_string().c_str());
    return 1;
  }

  MetaKv strat_rows = strategy_metrics(ts);
  const MetaKv summary_rows = result_summary_metrics(r);
  strat_rows.insert(strat_rows.end(), summary_rows.begin(), summary_rows.end());
  const std::string strat_path = (fs::path(args.out) / "strategy_metrics.csv").string();
  st = write_metrics_csv(meta, strat_rows, strat_path);
  if (!st) {
    std::fprintf(stderr, "write_metrics_csv(strategy_metrics): %s\n",
                 st.error().to_string().c_str());
    return 1;
  }

  const std::string engine_path = (fs::path(args.out) / "engine_metrics.csv").string();
  st = write_metrics_csv(meta, engine_metrics(stats), engine_path);
  if (!st) {
    std::fprintf(stderr, "write_metrics_csv(engine_metrics): %s\n", st.error().to_string().c_str());
    return 1;
  }

  const double wall_ms = stats.wall_clock_ms;
  const double steps_per_s =
      (wall_ms > 0.0) ? 1000.0 * static_cast<double>(r.size()) / wall_ms : 0.0;
  const double total_pnl = r.nav.empty() ? 0.0 : r.nav.back();
  std::printf("=== surface-db dispersion backtest ===\n"
              "db: %s (generation %llu) | window: %s .. %s (%zu steps)\n"
              "index: %s | basket: %zu names | universe: %s | config: %s\n"
              "[pnl] total=%s sharpe=%s max_drawdown=%s total_return=%s\n"
              "[timing] engine: %.1f ms over %llu steps (%.1f steps/s)\n"
              "[snapshot-cache] loads=%llu hits=%llu prefetches=%llu "
              "(all-zero == the engine's PRIVATE Sealed cache, by design)\n"
              "[wrote] %s\n[wrote] %s\n[wrote] %s\n",
              db->root().c_str(), static_cast<unsigned long long>(db->generation()),
              date_lo.c_str(), date_hi.c_str(), r.size(), spec.index_symbol.c_str(),
              n_names, args.universe.empty() ? "surface_db_manifest" : args.universe.c_str(),
              args.config.empty() ? "(defaults)" : args.config.c_str(), fmt_num(total_pnl).c_str(),
              fmt_num(ts.sharpe).c_str(), fmt_num(ts.max_drawdown).c_str(),
              fmt_num(ts.total_return).c_str(), wall_ms,
              static_cast<unsigned long long>(stats.n_steps), steps_per_s,
              static_cast<unsigned long long>(stats.cache.loads),
              static_cast<unsigned long long>(stats.cache.hits),
              static_cast<unsigned long long>(stats.cache.prefetches), series_path.c_str(),
              strat_path.c_str(), engine_path.c_str());
  return 0;
}
