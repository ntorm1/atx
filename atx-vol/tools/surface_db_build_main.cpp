// surface_db_build_main — the production build CLI: point it at an OPRA hive v2
// tree and a SurfaceDb root and it create-or-opens the db, loads the date window,
// auto-generates the per-symbol fit configs, and cell-aware-streaming-populates
// every (symbol, date) surface — the one-call `build_surface_db` driver
// (atx/vol/surface_db_build.hpp) wrapped in a hand-rolled arg loop.
//
// Fully resumable: re-running over an unchanged hive fits ZERO once every cell has
// either fitted successfully or been config-disabled (configs skip-existing, the
// cell-aware populate writes no date; a cell that FAILS to fit is retried, so it
// keeps re-fitting its date). A grown hive fits only the new dates, and an
// un-pulled (empty) window is a graceful all-zero success.
//
// Usage:
//   atx-vol-surface-db-build --db <root> --hive <root>
//       --from YYYY-MM-DD --to YYYY-MM-DD
//       [--symbols A,B,C] [--index SPY] [--preset populate] [--r 0.045]
//       [--deep-selection] [--retry-disabled] [--pin-curve-family true|false]
//       [--fit-workers N] [--report out.csv] [--max-failures N]
//
//   --db            SurfaceDb root (created if absent, else opened/resumed).
//   --hive          OPRA hive v2 root holding date=<YYYY-MM-DD>/data.parquet.
//   --from / --to   inclusive date window.
//   --symbols       CSV universe; OMIT (or empty) to discover every underlying
//                   in the window (rectangular date x union grid, visible holes).
//   --index         designated index leg, pinned to the dense index recipe.
//   --preset        fast | accurate | robust | hft | populate (default populate).
//   --r             flat continuously-compounded carry rate (default 0.0). MUST
//                   match the rate the hive's quotes were priced under, or every
//                   put-call-parity forward is wrong and every fit fails.
//   --deep-selection  run the full held-out select_curve OOS search per symbol.
//   --retry-disabled  re-attempt the symbols whose STORED config is disabled,
//                   instead of skipping them. Without it, a fail-closed disable is
//                   permanent for the life of the database: the symbol is skipped
//                   as already-configured on every later run, so no fix to the
//                   loader, the hive or the selector can ever reach it. Enabled
//                   configs are still left untouched (unlike a full overwrite), so
//                   this cannot clobber an operator's tuned config — but it DOES
//                   re-enable a symbol an operator disabled by hand, which is why
//                   it is opt-in. The standing disabled names are on the
//                   config.failed_symbols line of every run's report.
//   --pin-curve-family true|false  store the auto-selected curve family as a HARD
//                   PIN (default false). Pinned, each cell gets exactly ONE
//                   family attempt: PricerFitter's construction-failure and
//                   admission-rejection fallback ladders are both disabled for a
//                   pinned symbol. Unpinned (default) the family is recorded as
//                   the preferred route and the fit auto-routes with both ladders
//                   live — a production build lost 10 of 45 cells to marginal
//                   single-attempt rejections the ladders exist to recover. Pass
//                   `true` to restore the old behaviour. Requires a value; a
//                   missing or unrecognised one is a usage error (exit 2).
//   --fit-workers   outer fit fan-out; 0 = auto (honors ATX_VOL_FIT_WORKERS).
//   --report        also write the three-section CSV report to this path.
//   --max-failures  cap on the printed `failed_cell` lines (default 32). Overflow
//                   is counted in coverage.failed_cells_elided, never dropped
//                   silently; the --report CSV always carries the FULL list.
//                   Same flag name/semantics as atx-vol-surface-db verify.
//
// Prints one line per report field to stdout; exits 0 on Ok, 1 on Err (message
// on stderr), 2 on a usage error (unknown/missing/malformed flag), 3 when the
// build ran but produced NOTHING — either every symbol failed CONFIG SELECTION
// (is_total_config_failure) or work was scheduled and no cell fitted
// (is_total_fit_failure). See atx-vol/docs/surface-db-build.md.

#include <cerrno>
#include <cmath>
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

// The build ran to completion and produced NOTHING. Distinct from 1 (the tool or
// the db broke, no report) and from 2 (the operator's command line was wrong):
// the inputs parsed and every symbol died — either at config selection or at the
// fit. A script can therefore tell "atx is broken" from "your data/rate is wrong".
// ONE code for both stages: the question a script asks is "did this run produce
// anything?", and the stderr diagnostic names which stage swallowed it.
constexpr int kExitTotalFitFailure = 3;

void print_usage(std::FILE *out) {
  std::fprintf(out,
               "usage: atx-vol-surface-db-build --db <root> --hive <root> "
               "--from YYYY-MM-DD --to YYYY-MM-DD\n"
               "         [--symbols A,B,C] [--index SPY] [--preset populate] [--r 0.045] "
               "[--deep-selection] [--retry-disabled] [--pin-curve-family true|false] "
               "[--fit-workers N] [--report out.csv] "
               "[--max-failures N]\n");
}

// Parse a non-negative count, consuming the whole token. Byte-for-byte the rule
// atx-vol-surface-db's --max-failures uses (surface_db_main.cpp), so the same
// string is accepted or rejected identically by both tools.
[[nodiscard]] bool parse_count(std::string_view text, std::size_t &out) {
  if (text.empty()) {
    return false;
  }
  const std::string s(text);
  const char *first = s.c_str();
  char *end = nullptr;
  errno = 0;
  const unsigned long long v = std::strtoull(first, &end, 10);
  if (end != first + s.size() || errno == ERANGE) {
    return false; // trailing junk or out of range
  }
  // strtoull accepts a leading '-' and wraps it; a negative count is nonsense.
  if (s.find('-') != std::string::npos) {
    return false;
  }
  out = static_cast<std::size_t>(v);
  return true;
}

// Parse a FINITE double from a flag value, consuming the whole token.
//
// Deliberately stricter than the `--fit-workers` strtoul path next door, which
// silently coerces a typo to 0: a silently-zeroed carry rate is precisely the
// trap `--r` exists to close, so `--r abc`, `--r 0.03x`, `--r nan`, `--r inf`
// and an absent value are all hard usage errors instead.
[[nodiscard]] bool parse_finite_double(std::string_view text, double &out) {
  if (text.empty()) {
    return false;
  }
  const std::string s(text);
  const char *first = s.c_str();
  char *end = nullptr;
  errno = 0;
  const double v = std::strtod(first, &end);
  if (end != first + s.size() || errno == ERANGE || !std::isfinite(v)) {
    return false; // trailing junk, out of range, or nan/inf
  }
  out = v;
  return true;
}

// Parse an explicit boolean flag value. Same strict-parsing discipline as
// `parse_finite_double` above and for the same reason: the value decides whether
// a production build gets one curve attempt per cell or the full fallback ladder,
// so a typo must be a loud usage error, never a silent default. Only these six
// spellings are accepted; anything else — including an ABSENT value, which the
// caller passes in as "" — is rejected.
[[nodiscard]] bool parse_bool(std::string_view text, bool &out) {
  if (text == "true" || text == "1" || text == "on") {
    out = true;
    return true;
  }
  if (text == "false" || text == "0" || text == "off") {
    out = false;
    return true;
  }
  return false;
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
// section-1 key set), then the failed-symbol list, the per-symbol coverage rows,
// and the per-cell fit-failure reasons (capped at `max_failed_cells`).
// Deterministic and self-describing.
void print_report(const SurfaceDbBuildReport &r, std::size_t max_failed_cells) {
  std::printf("config.n_symbols %u\n", r.config.n_symbols);
  std::printf("config.n_configured %u\n", r.config.n_configured);
  std::printf("config.n_skipped_existing %u\n", r.config.n_skipped_existing);
  std::printf("config.n_disabled_failed %u\n", r.config.n_disabled_failed);
  std::printf("config.n_disabled_existing %u\n", r.config.n_disabled_existing);
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
  std::printf("n_coverage_holes %zu\n", r.n_coverage_holes);

  // Every symbol the database is currently NOT serving (fail-closed; never
  // silently served) — the ones this run disabled AND the ones it found already
  // stored disabled. Printed on every run, not just the one that first stored the
  // disable, because a standing failure that only the first run mentions is a
  // silent failure (FIX-C-2).
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

  // WHY each cell in coverage.cells_failed failed — the fit stage's counterpart to
  // config.failed_symbols above, in the populate's deterministic (date, symbol)
  // order. Capped so a wholesale failure prints a sample rather than a wall of
  // hundreds of lines, and the elided count keeps the truncation loud (the
  // failures_reported / failures_elided pair `atx-vol-surface-db verify` uses).
  // The --report CSV carries every entry.
  const ReportedFailedCells failed = reported_failed_cells(r, max_failed_cells);
  std::printf("coverage.failed_cells_reported %zu\n", failed.reported.size());
  std::printf("coverage.failed_cells_elided %zu\n", failed.n_elided);
  for (const FailedCell &f : failed.reported) {
    const std::string_view code_name = atx::core::to_string(f.code);
    std::printf("failed_cell %s %s code=%.*s detail=%s\n", f.date.c_str(), f.symbol.c_str(),
                static_cast<int>(code_name.size()), code_name.data(), f.detail.c_str());
  }
}

} // namespace

int main(int argc, char **argv) {
  SurfaceDbBuildSpec spec;
  std::string preset_name = "populate"; // matches the SurfaceDbBuildSpec default (Populate)
  std::string report_path;
  bool fit_workers_set = false;
  std::size_t max_failed_cells = kSurfaceDbBuildMaxReportedFailedCells;

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
    } else if (a == "--r") {
      const std::string_view text = nv();
      if (!parse_finite_double(text, spec.hive.r)) {
        std::fprintf(stderr,
                     "atx-vol-surface-db-build: --r expects a finite number, got '%.*s'\n",
                     static_cast<int>(text.size()), text.data());
        print_usage(stderr);
        return 2;
      }
    } else if (a == "--deep-selection") {
      spec.auto_config.deep_selection = true;
    } else if (a == "--retry-disabled") {
      spec.auto_config.retry_disabled = true;
    } else if (a == "--pin-curve-family") {
      const std::string_view text = nv();
      if (!parse_bool(text, spec.auto_config.pin_curve_family)) {
        std::fprintf(stderr,
                     "atx-vol-surface-db-build: --pin-curve-family expects "
                     "true|false (or 1|0, on|off), got '%.*s'\n",
                     static_cast<int>(text.size()), text.data());
        print_usage(stderr);
        return 2;
      }
    } else if (a == "--fit-workers") {
      spec.fit_workers = static_cast<unsigned>(std::strtoul(nv(), nullptr, 10));
      fit_workers_set = true;
    } else if (a == "--report") {
      report_path = nv();
    } else if (a == "--max-failures") {
      const std::string_view text = nv();
      if (!parse_count(text, max_failed_cells)) {
        std::fprintf(stderr,
                     "atx-vol-surface-db-build: --max-failures expects a non-negative integer, "
                     "got '%.*s'\n",
                     static_cast<int>(text.size()), text.data());
        print_usage(stderr);
        return 2;
      }
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

  print_report(*report, max_failed_cells);

  if (!report_path.empty()) {
    const Status w = write_build_report_csv(*report, report_path);
    if (!w) {
      std::fprintf(stderr, "atx-vol-surface-db-build: write_build_report_csv(%s): %s\n",
                   report_path.c_str(), w.error().to_string().c_str());
      return 1;
    }
    std::printf("report %s\n", report_path.c_str());
  }

  // A database that is permanently not serving a requested name must not read as
  // a clean run. The counters and the config.failed_symbols line above already
  // carry it, but a resumed build is otherwise ALL GREEN — every date
  // `dates_skipped_complete`, zero cells to fit — so the one line that matters
  // gets a stderr callout naming the symbols and the flag that retries them.
  // Not an error: a partially-disabled universe is a legitimate production state.
  if (report->config.n_disabled_existing > 0) {
    std::fprintf(stderr,
                 "atx-vol-surface-db-build: %u symbol(s) are STORED DISABLED and were skipped:",
                 report->config.n_disabled_existing);
    for (const std::string &s : report->config.failed_symbols) {
      std::fprintf(stderr, " %s", s.c_str());
    }
    std::fprintf(stderr,
                 "\n  These names are absent from every partition and will stay absent on every "
                 "rerun: a stored config (even a disabled one) is skip-existing, so the build "
                 "never re-attempts them. Re-run with --retry-disabled to re-select them once the "
                 "cause is fixed, or upsert a config by hand.\n");
  }

  // The silent-failure trap, one stage EARLIER than the fit: if config selection
  // failed for every symbol, every config is stored disabled, nothing is ever
  // scheduled, and `is_total_fit_failure` below cannot see it — `cells_to_fit`
  // is 0, which is also what a healthy nothing-to-do resume looks like. Checked
  // first because it is the upstream cause: when both fire, the config stage is
  // what the operator has to fix.
  if (is_total_config_failure(*report)) {
    std::fprintf(stderr,
                 "atx-vol-surface-db-build: TOTAL CONFIG FAILURE: %u symbols seen, 0 enabled "
                 "(%u disabled by a selection failure on this run, %u already stored "
                 "disabled).\n"
                 "  NOT ONE symbol in this database has an enabled config, so no cell was ever "
                 "scheduled to fit; the database will stay empty. Most likely causes: the hive "
                 "window holds no usable board for these names (check n_dates_loaded / "
                 "n_load_errors above), or the universe is wrong. The names are on the "
                 "config.failed_symbols line. If the cause is already fixed, the stored "
                 "disables still need --retry-disabled to be re-attempted.\n",
                 report->config.n_symbols, report->config.n_disabled_failed,
                 report->config.n_disabled_existing);
    return kExitTotalFitFailure;
  }

  // A build that scheduled work and fitted NOTHING used to exit 0, so an operator
  // saw green over an empty database. It is a failure, and the diagnostic names
  // the top suspect (the report above is still printed and the --report CSV still
  // written — this only changes the exit code).
  if (is_total_fit_failure(*report)) {
    std::fprintf(stderr,
                 "atx-vol-surface-db-build: TOTAL FIT FAILURE: %u cells scheduled, 0 fitted "
                 "(%u failed).\n"
                 "  Most likely cause: the carry rate does not match the hive. This build used "
                 "--r %.17g. If the hive's quotes embed a non-zero funding/borrow rate, every "
                 "put-call-parity forward is wrong and every fit fails identically. Re-run with "
                 "the matching --r <rate>.\n"
                 "  Do not guess: the failed_cell lines above carry each cell's own reason "
                 "straight from the fitter, and --report writes all of them.\n",
                 report->coverage.cells_to_fit, report->coverage.cells_failed, spec.hive.r);
    return kExitTotalFitFailure;
  }

  return 0;
}
