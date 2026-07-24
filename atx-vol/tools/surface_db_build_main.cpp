// surface_db_build_main — the production build CLI: point it at an OPRA hive v2
// tree and a SurfaceDb root and it create-or-opens the db, loads the date window,
// auto-generates the per-symbol fit configs, and cell-aware-streaming-populates
// every (symbol, date) surface — the one-call `build_surface_db` driver
// (atx/vol/surface_db_build.hpp) wrapped in a hand-rolled arg loop.
//
// Fully resumable: re-running over an unchanged hive fits ZERO (configs
// skip-existing, the cell-aware populate writes no date), a grown hive fits only
// the new dates, and an un-pulled (empty) window is a graceful all-zero success.
//
// Usage:
//   atx-vol-surface-db-build --db <root> --hive <root>
//       --from YYYY-MM-DD --to YYYY-MM-DD
//       [--symbols A,B,C] [--index SPY] [--preset populate]
//       [--deep-selection] [--fit-workers N] [--report out.csv]
//
//   --db            SurfaceDb root (created if absent, else opened/resumed).
//   --hive          OPRA hive v2 root holding date=<YYYY-MM-DD>/data.parquet.
//   --from / --to   inclusive date window.
//   --symbols       CSV universe; OMIT (or empty) to discover every underlying
//                   in the window (rectangular date x union grid, visible holes).
//   --index         designated index leg, pinned to the dense index recipe.
//   --preset        fast | accurate | robust | hft | populate (default populate).
//   --deep-selection  run the full held-out select_curve OOS search per symbol.
//   --fit-workers   outer fit fan-out; 0 = auto (honors ATX_VOL_FIT_WORKERS).
//   --report        also write the two-section CSV report to this path.
//
// Prints one line per report field to stdout; exits 0 on Ok, 1 on Err (message
// on stderr), 2 on a usage error (unknown/missing flag). See
// atx-vol/docs/surface-db-build.md.

#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>

#include "atx/vol/opra_hive.hpp"           // OpraHiveSpec
#include "atx/vol/session.hpp"             // FitPreset
#include "atx/vol/surface_db_build.hpp"    // SurfaceDbBuildSpec, build_surface_db, write_build_report_csv
#include "atx/vol/surface_db_populate.hpp" // PopulateSymbolStats
#include "atx/vol/types.hpp"               // Result, Status

using namespace atx::vol;

namespace {

void print_usage(std::FILE *out) {
  std::fprintf(out,
               "usage: atx-vol-surface-db-build --db <root> --hive <root> "
               "--from YYYY-MM-DD --to YYYY-MM-DD\n"
               "         [--symbols A,B,C] [--index SPY] [--preset populate] "
               "[--deep-selection] [--fit-workers N] [--report out.csv]\n");
}

// Split a comma-separated list, trimming surrounding whitespace and dropping
// empty fields — so `--symbols "AAA, BBB , CCC"`, a trailing/leading comma, or a
// doubled comma never yields a blank or space-padded symbol. An all-empty/blank
// string => {} = discover-all, exactly what an omitted --symbols means.
std::vector<std::string> split_csv(std::string_view csv) {
  std::vector<std::string> out;
  std::size_t start = 0;
  while (start <= csv.size()) {
    const std::size_t end = csv.find(',', start);
    std::string_view field =
        csv.substr(start, end == std::string_view::npos ? csv.size() - start : end - start);
    const auto ws = [](char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; };
    while (!field.empty() && ws(field.front())) {
      field.remove_prefix(1);
    }
    while (!field.empty() && ws(field.back())) {
      field.remove_suffix(1);
    }
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

// Parse a preset name; returns false for an unknown name (a hard usage error —
// this is a production tool, we don't silently coerce a typo to a default).
bool parse_preset(std::string_view name, FitPreset &out) {
  if (name == "fast") {
    out = FitPreset::Fast;
  } else if (name == "accurate") {
    out = FitPreset::Accurate;
  } else if (name == "robust") {
    out = FitPreset::Robust;
  } else if (name == "hft") {
    out = FitPreset::Hft;
  } else if (name == "populate") {
    out = FitPreset::Populate;
  } else {
    return false;
  }
  return true;
}

// Emit every scalar report field, one `key value` line each (mirrors the CSV
// section-1 key set), then the failed-symbol list and the per-symbol coverage
// rows. Deterministic and self-describing.
void print_report(const SurfaceDbBuildReport &r) {
  std::printf("config.n_symbols %u\n", r.config.n_symbols);
  std::printf("config.n_configured %u\n", r.config.n_configured);
  std::printf("config.n_skipped_existing %u\n", r.config.n_skipped_existing);
  std::printf("config.n_disabled_failed %u\n", r.config.n_disabled_failed);
  std::printf("coverage.cells_loaded %u\n", r.coverage.cells_loaded);
  std::printf("coverage.cells_to_fit %u\n", r.coverage.cells_to_fit);
  std::printf("coverage.cells_refit %u\n", r.coverage.cells_refit);
  std::printf("coverage.cells_already_present %u\n", r.coverage.cells_already_present);
  std::printf("coverage.cells_ok %u\n", r.coverage.cells_ok);
  std::printf("coverage.cells_failed %u\n", r.coverage.cells_failed);
  std::printf("coverage.dates_total %u\n", r.coverage.dates_total);
  std::printf("coverage.dates_written %u\n", r.coverage.dates_written);
  std::printf("coverage.dates_skipped_complete %u\n", r.coverage.dates_skipped_complete);
  std::printf("coverage.dates_skipped_would_drop %u\n", r.coverage.dates_skipped_would_drop);
  std::printf("n_dates_loaded %zu\n", r.n_dates_loaded);
  std::printf("n_dates_missing %zu\n", r.n_dates_missing);
  std::printf("n_load_errors %zu\n", r.n_load_errors);

  // The disabled-on-selection-failure names (fail-closed; never silently served).
  std::printf("config.failed_symbols");
  for (const std::string &s : r.config.failed_symbols) {
    std::printf(" %s", s.c_str());
  }
  std::printf("\n");

  // Per-symbol populate coverage (written dates only), sorted by symbol.
  for (const PopulateSymbolStats &s : r.coverage.per_symbol) {
    std::printf("symbol.%s attempted=%u ok=%u failed=%u disabled=%u\n", s.symbol.c_str(),
                s.n_attempted, s.n_ok, s.n_failed, s.n_disabled);
  }
}

} // namespace

int main(int argc, char **argv) {
  SurfaceDbBuildSpec spec;
  std::string preset_name = "populate"; // matches the SurfaceDbBuildSpec default (Populate)
  std::string report_path;
  bool fit_workers_set = false;

  for (int i = 1; i < argc; ++i) {
    const std::string_view a = argv[i];
    // Fetch the value for a flag that takes one; "" when the flag ended the argv.
    const auto nv = [&]() -> const char * { return (i + 1 < argc) ? argv[++i] : ""; };
    if (a == "--db") {
      spec.db_root = nv();
    } else if (a == "--hive") {
      spec.hive.root_dir = nv();
    } else if (a == "--from") {
      spec.hive.date_lo = nv();
    } else if (a == "--to") {
      spec.hive.date_hi = nv();
    } else if (a == "--symbols") {
      spec.hive.symbols = split_csv(nv());
    } else if (a == "--index") {
      spec.auto_config.index_symbol = nv();
    } else if (a == "--preset") {
      preset_name = nv();
    } else if (a == "--deep-selection") {
      spec.auto_config.deep_selection = true;
    } else if (a == "--fit-workers") {
      spec.fit_workers = static_cast<unsigned>(std::strtoul(nv(), nullptr, 10));
      fit_workers_set = true;
    } else if (a == "--report") {
      report_path = nv();
    } else if (a == "--help" || a == "-h") {
      print_usage(stdout);
      return 0;
    } else {
      std::fprintf(stderr, "atx-vol-surface-db-build: unknown flag: %s\n", argv[i]);
      print_usage(stderr);
      return 2;
    }
  }

  // Required flags.
  if (spec.db_root.empty() || spec.hive.root_dir.empty() || spec.hive.date_lo.empty() ||
      spec.hive.date_hi.empty()) {
    std::fprintf(stderr, "atx-vol-surface-db-build: --db, --hive, --from and --to are required\n");
    print_usage(stderr);
    return 2;
  }

  // Preset drives BOTH the manifest seeding (auto_config.preset) and the populate
  // fallback tier (spec.preset); keep them in lockstep from the one flag.
  FitPreset preset{};
  if (!parse_preset(preset_name, preset)) {
    std::fprintf(stderr,
                 "atx-vol-surface-db-build: unknown --preset '%s' "
                 "(fast|accurate|robust|hft|populate)\n",
                 preset_name.c_str());
    print_usage(stderr);
    return 2;
  }
  spec.preset = preset;
  spec.auto_config.preset = preset;
  (void)fit_workers_set; // spec.fit_workers already defaults to 0 = auto when unset.

  const Result<SurfaceDbBuildReport> report = build_surface_db(spec);
  if (!report) {
    std::fprintf(stderr, "atx-vol-surface-db-build: build_surface_db: %s\n",
                 report.error().to_string().c_str());
    return 1;
  }

  print_report(*report);

  if (!report_path.empty()) {
    const Status w = write_build_report_csv(*report, report_path);
    if (!w) {
      std::fprintf(stderr, "atx-vol-surface-db-build: write_build_report_csv(%s): %s\n",
                   report_path.c_str(), w.error().to_string().c_str());
      return 1;
    }
    std::printf("report %s\n", report_path.c_str());
  }

  return 0;
}
