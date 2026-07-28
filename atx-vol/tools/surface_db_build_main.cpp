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
//       [--snapshot-suffix T19:55:00Z]
//       [--deep-selection] [--retry-disabled] [--pin-curve-family true|false]
//       [--fit-workers N] [--report out.csv] [--max-failures N]
//       [--allow-coverage-regression] [--strict]
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
//   --snapshot-suffix  per-date snapshot stamp suffix, `T HH:MM:SSZ` (default
//                   "T19:55:00Z", byte-identical to the prior unconditional
//                   default when omitted). Threaded straight into
//                   OpraHiveSpec::snapshot_suffix (atx/vol/opra_hive.hpp), which
//                   the loader concatenates onto each date to form the load
//                   spec's snapshot_iso (opra_hive.cpp) -- the instant the
//                   T-to-expiry math treats every quote as observed at
//                   (opra_panel.cpp). Task 4 addendum §B: an ET-anchored
//                   multi-year backfill (pull_opra_hive.py --snap-et) lands at
//                   19:55Z on EDT dates and 20:55Z on EST dates, so a build
//                   whose window sits entirely on the EST side of a DST
//                   transition must pass --snapshot-suffix T20:55:00Z or every
//                   cell in it is silently stamped an hour off. Same strict-
//                   parsing discipline as --r: must match `^T\d{2}:\d{2}:\d{2}Z$`
//                   (is_valid_snapshot_suffix, surface_db_build_cli.hpp) or it
//                   is a hard usage error (exit 2) -- a silently-accepted
//                   malformed stamp would be exactly the trap --r's own strict
//                   parser exists to close, one field over.
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
//                   Deliberately NOT strictly parsed: an unparseable value
//                   coerces to 0, and 0 is a legitimate, safe choice. Contrast
//                   --r, where every value is a claim about the market and a
//                   coerced 0.0 is a WRONG claim. A missing value is still a
//                   usage error, like every other value-taking flag here.
//   --report        also write the five-section CSV report to this path. A write
//                   failure is named on stderr and exits 1 -- but it never
//                   preempts exit 3 or 5; see `build_exit_code` in
//                   atx/vol/surface_db_build.hpp for the whole precedence.
//   --max-failures  cap on the printed `failed_cell` AND `coverage_regression_cell`
//                   lines (default 32). Overflow is counted in the matching
//                   `_elided` scalar, never dropped silently; the --report CSV
//                   always carries the FULL list of both.
//                   ONE EXEMPTION (REV-R3 fix-1): on the DESTRUCTIVE
//                   `--allow-coverage-regression` branch the cap does NOT apply to
//                   the regression cells -- every destroyed cell is printed. That
//                   branch's own banner says the printed list and the CSV are the
//                   only record those surfaces ever existed, and a cap (0 in
//                   particular) would empty the list under that sentence.
//                   Same flag name/semantics as atx-vol-surface-db verify.
//   --allow-coverage-regression  permit a date's rewrite to DESTROY a stored
//                   surface. Off by default: a partition write is whole-file, so
//                   a present, enabled cell whose re-fit FAILS is simply not in
//                   the new file, and one production-shaped run at the wrong --r
//                   removed 95 stored surfaces this way while reporting success.
//                   By default such a date is REFUSED — the existing partition is
//                   left untouched, the run continues with the other dates, and
//                   the exit code is 5. Pass this flag only for a run that INTENDS
//                   retirement; the destroyed cells are still counted and named,
//                   on stderr and in the --report CSV's section 5.
//   --strict        make "scheduled work, fitted nothing" a NON-ZERO exit (3)
//                   even when the run CARRIED stored surfaces. Off by default,
//                   and deliberately so: the flagship database holds cells that
//                   fail permanently, so this fires on every run of a perfectly
//                   healthy database and a strict DEFAULT would be exactly the
//                   permanently-red signal the carry exemption was added to
//                   remove. For UNATTENDED SCHEDULERS over a database whose
//                   failing-cell set is expected to be empty; an interactive
//                   operator on a database with standing failures should leave it
//                   off and read the is_carry_masked_fit_failure warning instead.
//                   The strict diagnostic deliberately does NOT repeat the --r
//                   advice -- see the block that prints it.
//
// Prints one line per report field to stdout; exits 0 on Ok, 1 on Err (message
// on stderr), 2 on a usage error (unknown/missing/malformed flag), 3 when the
// build ran but produced NOTHING — every present input file was unreadable
// (is_total_load_failure), or every symbol failed CONFIG SELECTION
// (is_total_config_failure), or work was scheduled and no cell fitted
// (is_total_fit_failure, or is_strict_total_fit_failure under --strict), and 5
// when at least one date was REFUSED because its rewrite would have destroyed a
// stored surface. See atx-vol/docs/surface-db-build.md.
//
// One shape exits 0 with a stderr WARNING instead: nothing fitted, something
// failed, and something was CARRIED (is_carry_masked_fit_failure). It is exempt
// from exit 3 because the converged steady state has that exact shape on a
// perfectly healthy database — but so does a run whose every scheduled cell died,
// so the tool names the ambiguity rather than judging it. REV-R4: `--strict`
// resolves that ambiguity as a FAILURE (still exit 3) for callers who can afford
// to — see the flag's doc above and is_strict_total_fit_failure's declaration.

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>

#include "atx/vol/counters.hpp"            // counters::ledger (ATX_VOL_SOLVE_LEDGER dump)
#include "atx/vol/opra_hive.hpp"           // OpraHiveSpec
#include "atx/vol/session.hpp"             // FitPreset
#include "atx/vol/surface_db_build.hpp"    // SurfaceDbBuildSpec, build_surface_db, write_build_report_csv
#include "atx/vol/surface_db_populate.hpp" // PopulateSymbolStats
#include "atx/vol/types.hpp"               // Result, Status

#include "surface_db_build_cli.hpp" // is_valid_snapshot_suffix (Task 4 addendum §B)

using namespace atx::vol;

namespace {

// REV-R3 fix-1 (review I-2). The exit CODES and the decision that picks between
// them now live in `atx/vol/surface_db_build.hpp` — `kSurfaceDbBuildExit*` and
// `build_exit_code` — so the tool's most operator-visible contract is reachable
// from a test, like the predicates it reads already were. They used to be a
// file-local `constexpr` pair plus a lambda inside `main()`, and exit 5 and its
// preemption of 3 and 1 rested on one manual CLI run. This file keeps what only
// it can own: WHICH diagnostic prints, in what order, with what advice.
//
// The bare `return 2`s in the arg loop below stay bare: they fire before the db
// is opened, so there is no report for `build_exit_code` to read
// (`kSurfaceDbBuildExitUsage` documents the number on the header's side).

void print_usage(std::FILE *out) {
  std::fprintf(out,
               "usage: atx-vol-surface-db-build --db <root> --hive <root> "
               "--from YYYY-MM-DD --to YYYY-MM-DD\n"
               "         [--symbols A,B,C] [--index SPY] [--preset populate] [--r 0.045] "
               "[--snapshot-suffix T19:55:00Z] "
               "[--deep-selection] [--retry-disabled] [--pin-curve-family true|false] "
               "[--fit-workers N] [--report out.csv] "
               "[--max-failures N] [--allow-coverage-regression] [--strict]\n");
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
// the per-cell fit-failure reasons, and the coverage-regression cells. In that
// order, matching the blocks below.
//
// The two cell lists are capped by DIFFERENT numbers (REV-R6; this summary named
// one cap and omitted the regression block entirely, which REV-R3 added):
// `failed_cell` is capped at `max_failed_cells`, `coverage_regression_cell` at
// `coverage_regression_display_cap(r, max_failed_cells)` — which waives the cap
// on the destructive `--allow-coverage-regression` branch; see that helper.
// Each list prints its own `_reported` / `_elided` pair, so neither truncation is
// silent. Deterministic and self-describing.
void print_report(const SurfaceDbBuildReport &r, std::size_t max_failed_cells) {
  std::printf("config.n_symbols %u\n", r.config.n_symbols);
  std::printf("config.n_configured %u\n", r.config.n_configured);
  std::printf("config.n_skipped_existing %u\n", r.config.n_skipped_existing);
  std::printf("config.n_disabled_failed %u\n", r.config.n_disabled_failed);
  std::printf("config.n_disabled_existing %u\n", r.config.n_disabled_existing);
  std::printf("coverage.cells_loaded %u\n", r.coverage.cells_loaded);
  std::printf("coverage.cells_to_fit %u\n", r.coverage.cells_to_fit);
  std::printf("coverage.cells_refit %u\n", r.coverage.cells_refit);
  // FIX-D fix-1 (I2). The converged carry steady state prints ok=0 and refit=0;
  // without this line the terminal cannot tell it apart from a run that did
  // nothing at all — and that verdict is now precisely what `is_total_fit_failure`
  // has been widened to stop treating as a failure, so the counter has to be
  // visible where the operator reads the exit.
  std::printf("coverage.cells_carried %u\n", r.coverage.cells_carried);
  // FIX-E. Stored cells belonging to a DISABLED symbol, preserved through a
  // rewrite instead of deleted. Separate from cells_carried: those are healthy
  // surfaces reused as this run's output, these are surfaces of a name the
  // operator switched off, kept because `enabled = false` means stop fitting, not
  // delete.
  std::printf("coverage.cells_carried_disabled %u\n", r.coverage.cells_carried_disabled);
  std::printf("coverage.cells_already_present %u\n", r.coverage.cells_already_present);
  std::printf("coverage.cells_ok %u\n", r.coverage.cells_ok);
  std::printf("coverage.cells_failed %u\n", r.coverage.cells_failed);
  std::printf("coverage.dates_total %u\n", r.coverage.dates_total);
  std::printf("coverage.dates_written %u\n", r.coverage.dates_written);
  std::printf("coverage.dates_skipped_complete %u\n", r.coverage.dates_skipped_complete);
  std::printf("coverage.dates_skipped_would_drop %u\n", r.coverage.dates_skipped_would_drop);
  // REV-R3. Distinct from the line above it, which is the PRE-fit filter guard.
  // These two are the WRITE path's: a date whose candidate partition did not
  // contain everything the stored one did. `refused` means the guard held and the
  // stored surfaces are intact; `dropped` means --allow-coverage-regression was
  // given and they are gone.
  std::printf("coverage.dates_refused_coverage_regression %u\n",
              r.coverage.dates_refused_coverage_regression);
  // REV-R3 fix-2 (review N-3). A SUBSET of the line above, not a third outcome:
  // how many of those refusals were on a partition file the manifest does not
  // list. Always printed, so the state is greppable in a scheduler's log and not
  // only readable in the stderr banner.
  std::printf("coverage.dates_refused_partition_unlisted %u\n",
              r.coverage.dates_refused_partition_unlisted);
  std::printf("coverage.dates_dropped_coverage_regression %u\n",
              r.coverage.dates_dropped_coverage_regression);
  std::printf("n_dates_loaded %zu\n", r.n_dates_loaded);
  std::printf("n_dates_missing %zu\n", r.n_dates_missing);
  std::printf("n_load_errors %zu\n", r.n_load_errors);
  std::printf("n_coverage_holes %zu\n", r.n_coverage_holes);

  // Coarse phase split, so a slow build says WHICH phase is slow without
  // attaching a profiler. The three scale on different axes: `load` per date
  // file, `config` per NEW symbol (serial), `populate` per cell (fanned out over
  // --fit-workers). A resumed run reports a near-zero config phase because every
  // symbol is already stored, which is itself the useful signal.
  std::printf("timing.load_s %.3f\n", r.t_load_s);
  std::printf("timing.config_s %.3f\n", r.t_config_s);
  std::printf("timing.populate_s %.3f\n", r.t_populate_s);

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

  // Per-symbol populate coverage over the dates this run PROCESSED, sorted by
  // symbol. NOT "written dates only" — a date the coverage guard refused ran its
  // fits and withheld only the commit, so its cells are in these rows while
  // `coverage.dates_written` never counted the date (REV-R5, review M-2; the
  // counter's own contract is in surface_db_populate.hpp).
  for (const PopulateSymbolStats &s : r.coverage.per_symbol) {
    std::printf("symbol.%s attempted=%u ok=%u failed=%u disabled=%u carried=%u\n", s.symbol.c_str(),
                s.n_attempted, s.n_ok, s.n_failed, s.n_disabled, s.n_carried);
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

  // REV-R3: WHICH stored surfaces a refused write would have destroyed (or, under
  // --allow-coverage-regression, did). Same never-silent elision count and same
  // deterministic (date, symbol) order as the block above, and the --report CSV
  // likewise carries every entry. Two counters are always printed even when the
  // list is empty, so a scripted diff of two runs sees a regression appear. The
  // cap is `regression_cell_cap`'s, not `max_failed_cells` directly -- see there
  // for why the destructive branch is exempt.
  const ReportedCoverageRegressionCells regressed =
      reported_coverage_regression_cells(r, coverage_regression_display_cap(r, max_failed_cells));
  std::printf("coverage.coverage_regression_cells_reported %zu\n", regressed.reported.size());
  std::printf("coverage.coverage_regression_cells_elided %zu\n", regressed.n_elided);
  for (const CoverageRegressionCell &c : regressed.reported) {
    std::printf("coverage_regression_cell %s %s\n", c.date.c_str(), c.symbol.c_str());
  }
}

} // namespace

int main(int argc, char **argv) {
  SurfaceDbBuildSpec spec;
  std::string preset_name = "populate"; // matches the SurfaceDbBuildSpec default (Populate)
  std::string report_path;
  bool fit_workers_set = false;
  std::size_t max_failed_cells = kSurfaceDbBuildMaxReportedFailedCells;
  // REV-R4 (review C-05). NOT a SurfaceDbBuildSpec field: it changes nothing
  // about what the build does, only how this process reports the verdict. The
  // build must be byte-identical with and without it.
  bool strict = false;

  for (int i = 1; i < argc; ++i) {
    const std::string_view a = argv[i];
    // Value for a flag that takes one. A flag that ENDED the argv used to yield ""
    // and every consumer read that as a deliberate choice: `--report` wrote no CSV
    // AND EXITED 0 (the run looked clean and the file the operator asked for was
    // simply not there), `--symbols` fell back to discover-all so the universe
    // silently WIDENED, `--index` dropped the index leg, `--fit-workers` meant
    // auto. `--db`/`--hive`/`--from`/`--to` were saved by the required-flag check
    // below and `--r`/`--preset`/`--pin-curve-family`/`--max-failures` by strict
    // parsing; the other four were not. A dropped shell variable is never a
    // choice — record it and make it a usage error, the same rule and the same
    // words as the sibling admin CLI (tools/surface_db_main.cpp).
    bool missing_value = false;
    const auto nv = [&]() -> const char * {
      if (i + 1 < argc) {
        return argv[++i];
      }
      missing_value = true;
      return "";
    };
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
      if (!missing_value && !parse_finite_double(text, spec.hive.r)) {
        std::fprintf(stderr,
                     "atx-vol-surface-db-build: --r expects a finite number, got '%.*s'\n",
                     static_cast<int>(text.size()), text.data());
        print_usage(stderr);
        return 2;
      }
    } else if (a == "--snapshot-suffix") {
      const std::string_view text = nv();
      if (!missing_value && !is_valid_snapshot_suffix(text)) {
        std::fprintf(stderr,
                     "atx-vol-surface-db-build: --snapshot-suffix expects 'THH:MM:SSZ', got '%.*s'\n",
                     static_cast<int>(text.size()), text.data());
        print_usage(stderr);
        return 2;
      }
      if (!missing_value) {
        spec.hive.snapshot_suffix = std::string(text);
      }
    } else if (a == "--deep-selection") {
      spec.auto_config.deep_selection = true;
    } else if (a == "--retry-disabled") {
      spec.auto_config.retry_disabled = true;
    } else if (a == "--allow-coverage-regression") {
      spec.allow_coverage_regression = true;
    } else if (a == "--strict") {
      strict = true;
    } else if (a == "--pin-curve-family") {
      const std::string_view text = nv();
      if (!missing_value && !parse_bool(text, spec.auto_config.pin_curve_family)) {
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
      if (!missing_value && !parse_count(text, max_failed_cells)) {
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
    // One check for EVERY value-taking flag: a flag that ended the argv never
    // reaches its consumer as "".
    if (missing_value) {
      std::fprintf(stderr, "atx-vol-surface-db-build: %.*s requires a value\n",
                   static_cast<int>(a.size()), a.data());
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

  // Perf attribution seam (same env-gated shape as ATX_VOL_PROFILE): dump the
  // always-on solve ledger so an operator can see where a build's CPU went
  // (AL boundary solves / premium evals / IV Newton iterations) without a
  // profiler. stderr, one `ledger.<name> <count>` line per counter — the stdout
  // report shape is pinned by tests and stays untouched.
  {
    char env_buf[8];
    std::size_t env_sz = 0;
    if (getenv_s(&env_sz, env_buf, sizeof(env_buf), "ATX_VOL_SOLVE_LEDGER") == 0 && env_sz > 0) {
      const counters::ledger::Counts c = counters::ledger::snapshot();
      for (unsigned i = 0; i < counters::ledger::kCount; ++i) {
        std::fprintf(stderr, "ledger.%s %llu\n", counters::ledger::kNames[i],
                     static_cast<unsigned long long>(c.v[i]));
      }
    }
  }

  print_report(*report, max_failed_cells);

  // A --report write failure must NOT preempt the verdict. Returning 1 here (what
  // this used to do) jumped over the disabled-symbol callout and BOTH TOTAL
  // FAILURE blocks below, so a totally dead build whose report path happened to be
  // unwritable exited 1 with no `TOTAL FIT FAILURE` banner and no `--r` advice —
  // losing exactly the diagnostic that shape exists to deliver, on exactly the run
  // that needs it. It also falsified the documented contract that 1 means "no
  // report to read": `print_report` has already run, so the full report IS on
  // stdout.
  //
  // So: say it on stderr, fall through, and let the predicates pick the code. A
  // build that produced nothing exits 3 even if the CSV failed — the operator's
  // question is "did this run produce anything?", not "did the CSV land?", and the
  // stderr line above is not lost either way. 1 is returned only when no predicate
  // fires, i.e. the run was otherwise fine and the ONE thing that went wrong is
  // the file the operator asked for.
  bool report_write_failed = false;
  if (!report_path.empty()) {
    const Status w = write_build_report_csv(*report, report_path);
    if (!w) {
      std::fprintf(stderr, "atx-vol-surface-db-build: write_build_report_csv(%s): %s\n",
                   report_path.c_str(), w.error().to_string().c_str());
      report_write_failed = true;
    } else {
      std::printf("report %s\n", report_path.c_str());
    }
  }

  // ── REV-R3 (review C-02/F-02): the coverage-regression banner ───────────────
  //
  // Printed BEFORE the failure predicates, and unconditionally, for one reason:
  // it must never be swallowed. The predicate blocks below `return`, and a refusal
  // can legitimately coexist with a total fit failure (a date holding a
  // preserved-disabled cell can produce a non-empty candidate with zero fitted
  // cells), so both diagnostics have to print even though only one exit code can
  // come back.
  //
  // The exit code itself is `build_exit_code`'s (surface_db_build.hpp), where a
  // refusal PREEMPTS 3 and 1. Which should win is a real question and that is the
  // answer: 3 says "your inputs produced nothing, fix them and re-run", which
  // invites exactly the re-run that would destroy the data if the operator
  // reached for --allow-coverage-regression to make the tool stop complaining. 5
  // says "a rewrite would have deleted stored surfaces and I stopped", which is
  // both more specific and more urgent. The exit-3 diagnostics (including the --r
  // advice, the top suspect for BOTH shapes) still print in full; only the number
  // changes.
  const bool coverage_refused = report->coverage.dates_refused_coverage_regression > 0;
  if (coverage_refused || report->coverage.dates_dropped_coverage_regression > 0) {
    // The SAME cap `print_report` used (REV-R3 fix-1, review M-5: exempted on the
    // destructive branch), taken from the one helper so the stdout scalars and
    // this banner can never report different "shown / elided" numbers for the
    // same run.
    const ReportedCoverageRegressionCells regressed =
        reported_coverage_regression_cells(*report,
                                           coverage_regression_display_cap(*report, max_failed_cells));
    if (coverage_refused) {
      std::fprintf(stderr,
                   "atx-vol-surface-db-build: COVERAGE REGRESSION REFUSED: %u date(s) were NOT "
                   "written because the rewrite would have DESTROYED %zu stored surface(s).\n"
                   "  A partition write is whole-file, so a cell that is already stored and does "
                   "NOT fit this run is simply absent from the new file and is deleted by the "
                   "commit. Those dates were left exactly as they were; every other date in this "
                   "run was built normally. NOTHING WAS LOST.\n",
                   report->coverage.dates_refused_coverage_regression,
                   report->coverage.coverage_regression_cells.size());
    } else {
      std::fprintf(stderr,
                   "atx-vol-surface-db-build: COVERAGE REGRESSION ALLOWED (exit 0 unless another "
                   "verdict fires): %u date(s) were rewritten WITHOUT %zu previously stored "
                   "surface(s), which are now GONE.\n"
                   "  You passed --allow-coverage-regression, so the guard did not stop this. "
                   "The archive format keeps no tombstone -- a destroyed cell is byte-for-byte a "
                   "cell that was never fitted -- so the list below and the --report CSV are the "
                   "ONLY record that these surfaces existed. Keep them.\n",
                   report->coverage.dates_dropped_coverage_regression,
                   report->coverage.coverage_regression_cells.size());
    }
    std::fprintf(stderr, "  Cells (%zu shown, %zu elided):", regressed.reported.size(),
                 regressed.n_elided);
    for (const CoverageRegressionCell &c : regressed.reported) {
      std::fprintf(stderr, " %s/%s", c.date.c_str(), c.symbol.c_str());
    }
    std::fprintf(stderr, "\n");
    if (coverage_refused) {
      // ── REV-R3 fix-2 (review N-3): WHICH cause, and only that cause's advice ──
      //
      // A refusal has two quite different causes and they want opposite actions.
      // The --r advice below is right for the incident this banner was written
      // for -- a wrong carry rate fails every re-fit at once -- and it is WRONG
      // for a partition the manifest does not list, where nothing failed to fit
      // and the run was merely narrower than a file the index has lost track of.
      // Worse, the escape it offers (--allow-coverage-regression) is exactly what
      // deletes the surfaces the operator still has.
      //
      // So this follows the rule the strict diagnostic already established: on a
      // state where a piece of advice is not the right advice, do not print it.
      // The two are printed independently rather than as an if/else because one
      // run can hold both shapes -- `unlisted` is a documented SUBSET of
      // `dates_refused_coverage_regression`, so `listed` is the rest, and a mixed
      // run gets both paragraphs, each scoped to its own date count. The
      // subtraction is saturating: the subset relation is guaranteed by the
      // populate that fills these two, and this is a diagnostic, not the place to
      // find out that a hand-built report disagrees.
      const std::uint32_t n_unlisted =
          std::min(report->coverage.dates_refused_partition_unlisted,
                   report->coverage.dates_refused_coverage_regression);
      const std::uint32_t n_listed =
          report->coverage.dates_refused_coverage_regression - n_unlisted;
      if (n_unlisted > 0) {
        std::fprintf(
            stderr,
            "  %u of those date(s) have a partition FILE on disk that this database's MANIFEST "
            "does NOT list. That is not a fit problem and --r is not the suspect: the cells above "
            "did not have to fail for this to happen. The file holds surfaces, the index has lost "
            "track of it, and this run's board set is narrower than what the file holds -- so the "
            "rewrite would have deleted the difference. Causes: a crash between a previous run's "
            "archive write and its manifest commit, a manifest restored from an older copy, a "
            "hand-assembled or partially-copied database root, or an interrupted/failed "
            "drop_partition.\n"
            "  Remedy, and NOT --allow-coverage-regression (it would delete exactly the surfaces "
            "that survived): re-run those dates over the FULL board set, which rewrites the file "
            "with everything it already holds and re-lists it in the manifest. If you have "
            "confirmed the file is genuinely stale, delete <db>/partitions/<DATE>.atxvsa by hand "
            "and re-run -- that turns the date into a first write.\n",
            n_unlisted);
      }
      // ── REV-R5 (review I-3): the SECOND state on which --r is the wrong advice ──
      //
      // The rule above was applied once, to the unlisted-partition cause. It has a
      // second instance the block never saw: a run that CARRIED stored surfaces.
      // Under --strict such a run also prints the strict banner below, whose text
      // is "Do NOT reach for --r ... re-running at a 'corrected' --r would ...
      // DELETE those surfaces" -- so one stderr stream told the operator to
      // suspect --r and re-run with --allow-coverage-regression, and then that
      // doing so destroys data. Following the first paragraph is the destructive
      // action; the two blocks had each argued their own advice was safe and
      // neither had anticipated printing beside the other.
      //
      // The gate is `refusal_advice_names_the_carry_rate` (surface_db_build.hpp),
      // a pure predicate rather than an `if` here, so the mutual exclusion with
      // the strict banner is PINNED by a test instead of by these two comments
      // agreeing. It folds in the `n_listed > 0` condition this block already had.
      if (refusal_advice_names_the_carry_rate(*report)) {
        std::fprintf(stderr,
                     "  Most likely cause for the remaining %u date(s): --r does not match the "
                     "hive. This build used --r %.17g. "
                     "The carry rate is an ordinary build input that is neither stored in nor "
                     "checked against the database, and a wrong one fails every re-fit at once -- "
                     "one production-shaped run at the wrong --r destroyed 95 stored surfaces "
                     "before this guard existed. The failed_cell lines on stdout carry each cell's "
                     "own reason.\n"
                     "  If the fit failures are genuine and you INTEND to retire these cells, "
                     "re-run with --allow-coverage-regression; the run will then delete them and "
                     "list exactly what it deleted. Do not pass it to silence this line.\n",
                     n_listed, spec.hive.r);
      } else if (n_listed > 0) {
        // Same refusals, and they still need an explanation -- the count must not
        // vanish just because the --r paragraph is suppressed. What is withheld is
        // the SUSPECT and the escape hatch, not the fact.
        std::fprintf(stderr,
                     "  The remaining %u date(s) were refused because stored cells did not re-fit "
                     "on this run. --r IS NOT THE SUSPECT HERE and is deliberately not named: this "
                     "run CARRIED %u stored surface(s), which means those records validated for "
                     "reuse, so the rate this build used is not the thing that is wrong. Re-running "
                     "at a 'corrected' --r would fail every re-fit and be refused date by date -- "
                     "or, with --allow-coverage-regression, DELETE the carried surfaces.\n"
                     "  What to compare instead: this run's failed-cell list against the previous "
                     "run's. The same cells failing the same way is the converged steady state; a "
                     "cell that is NEW, or an old cell with a NEW reason, is a real regression. The "
                     "failed_cell lines on stdout carry each cell's own reason and --report writes "
                     "every one of them.\n",
                     n_listed, report->coverage.cells_carried);
      }
    }
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
                 "\n  No NEW cell will be written for these names on any rerun: a stored config "
                 "(even a disabled one) is skip-existing, so the build never re-attempts them. "
                 "One disabled before it ever fitted is absent from every partition; one disabled "
                 "afterwards KEEPS the surfaces it already produced (FIX-E preserves them through "
                 "a rewrite). Re-run with --retry-disabled to re-select them once the cause is "
                 "fixed, or upsert a config by hand.\n");
  }

  // R1-b (review C-04). The silent-failure trap at INGEST — earlier than either
  // predicate below, and invisible to both. When NOT ONE requested date could be
  // read, the fit stage is handed an empty board span, so `cells_to_fit` and every
  // config counter are 0 — which is byte-for-byte what a healthy no-op window
  // looks like. A wholly corrupt input window therefore used to print an empty
  // stderr and exit 0, and no scheduler could tell it from an intentional
  // nothing-to-do run. Tested FIRST because it is the most upstream cause: a
  // config stage that disabled everything did so because it was handed nothing,
  // and the config diagnostic would send the operator to the wrong place.
  if (is_total_load_failure(*report)) {
    std::fprintf(stderr,
                 "atx-vol-surface-db-build: TOTAL LOAD FAILURE: 0 of the requested dates were "
                 "readable (%zu date(s) produced no board, %zu cell(s) failed to load).\n"
                 "  NOT ONE board reached the fitter, so nothing was configured, nothing was "
                 "scheduled, and the database is unchanged. This is NOT an empty window: files "
                 "were present and every one of them was a real defect. Most likely causes: the "
                 "hive holds truncated or non-Parquet files (an interrupted pull), the wrong "
                 "--hive root, or a schema the loader does not accept. Check the date=<d>/"
                 "data.parquet files in the window with `parquet-tools`/`pyarrow`, or re-pull "
                 "the window, then re-run. A window with NO files present is a different, quiet "
                 "shape and still exits 0.\n",
                 report->n_dates_missing, report->n_load_errors);
    return build_exit_code(*report, report_write_failed, strict);
  }

  // Some dates loaded and some did not. NOT a failure — the readable dates were
  // built and the database really did gain surfaces — but it must not be silent
  // either: an unattended run that quietly ingests 12 of 17 dates produces a
  // database with holes nobody asked for. This condition contributes nothing to
  // the exit code; the operator watches the count. Unreachable when the block
  // above fired (that one requires `n_dates_loaded == 0` and returns).
  //
  // NO EXIT CODE IN THE BANNER (REV-R5, review I-2). This line used to read
  // `WARNING (exit 0)`, which was a hardcoded literal on a path that does not
  // decide the exit: partial load corruption composes with a total CONFIG failure
  // (3), a total FIT failure (3), a --strict verdict (3) and a coverage-regression
  // refusal (5), all of which are still ahead of or below this line. The tool
  // printed `exit 0` and returned 5. The fix is structural rather than a corrected
  // literal: a banner that names no code cannot contradict one, and this banner
  // never owned the code to begin with.
  if (report->n_load_errors > 0) {
    std::fprintf(stderr,
                 "atx-vol-surface-db-build: WARNING: %zu cell(s) failed to LOAD "
                 "(%zu date(s) loaded, %zu produced no board).\n"
                 "  These cells never reached the fitter, so they are in neither cells_ok nor "
                 "cells_failed and no failed_cell line names them. A present file that is "
                 "unreadable, truncated, wrong-schema, or missing its market inputs lands here; "
                 "a date that simply does not carry a symbol does NOT (that is "
                 "n_coverage_holes %zu, which every sparse universe produces). The dates that "
                 "did load were built normally, and a re-run after fixing or re-pulling the "
                 "bad files fills the gaps.\n"
                 "  This WARNING is not a verdict and does not set the exit code: partial load "
                 "corruption is not a failure on its own. Read the process's exit status, and any "
                 "banner above or below this one, for what this run is being judged as.\n",
                 report->n_load_errors, report->n_dates_loaded, report->n_dates_missing,
                 report->n_coverage_holes);
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
    return build_exit_code(*report, report_write_failed, strict);
  }

  // A build that scheduled work and fitted NOTHING used to exit 0, so an operator
  // saw green over an empty database. It is a failure, and the diagnostic names
  // the top suspect (the report above is still printed and the --report CSV still
  // written — this only changes the exit code).
  //
  // The predicate no longer fires when this run CARRIED stored surfaces (FIX-D
  // fix-1): that shape is the healthy converged resume, not a dead build, and the
  // `--r` guidance below would have been actively destructive on it. The full cost
  // of that widening — ANY run that carried anything is exempt, including one
  // whose every scheduled cell failed systematically — is written down at the
  // predicate's declaration in surface_db_build.hpp. `coverage.cells_carried`
  // above is what tells the operator a quiet run was a carry, and the
  // `is_carry_masked_fit_failure` warning below is what tells them the exempt
  // shape is ambiguous.
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
    return build_exit_code(*report, report_write_failed, strict);
  }

  // ── REV-R4 (review C-05): --strict ──────────────────────────────────────────
  //
  // The same "scheduled work, fitted nothing" question with the carry exemption
  // removed. Reached only when the block above did NOT fire, so by construction
  // this run CARRIED something -- `is_strict_total_fit_failure` is a superset of
  // `is_total_fit_failure` and differs from it exactly on `cells_carried > 0`.
  // That is why the two blocks cannot double-print and why this one is written
  // for the carry-masked shape specifically.
  //
  // Placed BEFORE the warning below rather than after it because the two are
  // alternatives, not layers: the warning's whole text is "the tool is not
  // judging this, here is why it can't". Under --strict the operator has already
  // told the tool to judge it, so printing the hedge as well would contradict the
  // exit code this block returns. The failed-cell list the warning exists to show
  // is reproduced here, from the same helper under the same --max-failures cap.
  //
  // THIS ORDERING IS THE ONLY THING SEPARATING THE TWO (REV-R5, review I-1).
  // `is_strict_total_fit_failure` is true on exactly the region
  // `is_carry_masked_fit_failure` occupies — the hedge's comment below claimed for
  // two releases that no verdict could accompany it "independently of the order
  // they are tested in here", which was never true of this block. Reorder these
  // two and the CLI prints a verdict and a hedge that says it is not judging, in
  // one stream. `SurfaceDbCarryMaskedFitFailure.OverlapsTheStrictVerdictByDesign`
  // asserts the overlap so it cannot be forgotten again.
  //
  // The hedge below used to be headed "WARNING (exit 0)", which is why this
  // ordering mattered doubly. That literal is gone (review I-2) — it was wrong on
  // exit 5 as well — but the ordering requirement is unchanged and independent of
  // it: the two TEXTS contradict each other, not just the number.
  //
  // SAME EXIT CODE, 3. No new number. A script's question is unchanged ("did this
  // run produce anything?"), the answer under --strict is just stricter about
  // what counts as producing something; inventing a code would force every
  // existing consumer to learn it to keep the behaviour it already has.
  //
  // The CODE comes from `build_exit_code`, which evaluates the SAME `strict &&
  // is_strict_total_fit_failure` conjunct — so a refused coverage regression
  // still preempts it with 5, and the strict decision is pinned by
  // `SurfaceDbBuildExitCode.StrictNeverFailsARunThatScheduledNothing` /
  // `.WithoutARefusalItIsTheExistingBehaviour` rather than resting on this line.
  // Before REV-R3 fix-1 (review I-2) this conjunct was the ONLY place the flag
  // was consulted, in `main()`, unreachable from any test: dropping the `strict
  // &&` token made strict the DEFAULT — permanently red on every run of the
  // flagship database — with nothing to catch it. The condition below now gates
  // only WHICH DIAGNOSTIC prints.
  //
  // NO --r ADVICE HERE, DELIBERATELY. The exit-3 block above names a carry-rate
  // mismatch as the top suspect; on this shape that advice is the dangerous one.
  // A run that carried surfaces is a run whose stored records validated for
  // reuse, so the rate this build used is not what is wrong, and "re-run with a
  // corrected --r" would fail every re-fit -- refusing every date under the
  // default guard, or destroying those surfaces with --allow-coverage-regression.
  // What separates the converged steady state from a real regression is WHICH
  // cells failed, so that is what this says to compare.
  if (strict && is_strict_total_fit_failure(*report)) {
    std::fprintf(stderr,
                 "atx-vol-surface-db-build: TOTAL FIT FAILURE (--strict): %u cells scheduled, "
                 "0 fitted (%u failed), %u carried.\n"
                 "  Without --strict this run exits 0: carrying stored surfaces exempts it, "
                 "because the converged steady state -- permanently-failing cells retried "
                 "forever beside their healthy carried siblings -- has this exact shape on a "
                 "database that is entirely healthy. You asked for the strict reading, so it "
                 "is a failure here: this run SCHEDULED work and produced no new surface.\n",
                 report->coverage.cells_to_fit, report->coverage.cells_failed,
                 report->coverage.cells_carried);

    const ReportedFailedCells strict_failed = reported_failed_cells(*report, max_failed_cells);
    std::fprintf(stderr, "  Cells that failed (%zu shown, %zu elided):",
                 strict_failed.reported.size(), strict_failed.n_elided);
    for (const FailedCell &f : strict_failed.reported) {
      std::fprintf(stderr, " %s/%s", f.date.c_str(), f.symbol.c_str());
    }
    std::fprintf(stderr, "\n");

    std::fprintf(stderr,
                 "  WHAT TO COMPARE: this run's failed-cell list against the PREVIOUS run's. "
                 "The same cells failing the same way is the converged steady state and this "
                 "exit is expected on every run -- if that is your database, do not pass "
                 "--strict. A cell that is NEW, or an old cell with a NEW reason, is a real "
                 "regression: a fitter or loader change, or a bad config for a newly-added "
                 "name. The failed_cell lines on stdout carry each cell's own reason and "
                 "--report writes every one of them, so diff the CSVs.\n"
                 "  Do NOT reach for --r. This run CARRIED %u stored surface(s), which means "
                 "they validated for reuse -- the rate this build used is not the thing that "
                 "is wrong. Re-running at a 'corrected' --r would fail every re-fit and be "
                 "refused date by date (exit 5), or, with --allow-coverage-regression, would "
                 "DELETE those %u surfaces.\n",
                 report->coverage.cells_carried, report->coverage.cells_carried);
    return build_exit_code(*report, report_write_failed, strict);
  }

  // FIX-D fix-2 (I2). The predicate above is exempt whenever ANYTHING was carried,
  // which is wider than the converged-database case it was widened for: a run
  // whose every scheduled cell died systematically, beside a healthy carried
  // population, is exempt too. The exit code must NOT come back for that shape —
  // it is indistinguishable from the converged steady state, which is a healthy
  // production database, and failing it is precisely the defect the carry clause
  // fixed. So the tool says the two are different instead of judging between them.
  //
  // WHAT THIS BLOCK IS AND IS NOT DISJOINT FROM (REV-R5, review I-1). It used to
  // say "disjoint from BOTH exit-3 blocks above by construction ... independently
  // of the order they are tested in here", and named two blocks when four now sit
  // above it. Three separate facts, each pinned by a test named at the predicate's
  // declaration in surface_db_build.hpp:
  //
  //   - The unconditional TOTAL FIT FAILURE and TOTAL CONFIG FAILURE blocks really
  //     are disjoint from this one, algebraically: both need `cells_carried == 0`
  //     and this needs `> 0`. Order does not matter between those three.
  //   - The TOTAL LOAD FAILURE block is disjoint only on reports the build
  //     actually produces (an empty board span zeroes every counter this reads) —
  //     a reachability fact, not an algebraic one.
  //   - THE --strict BLOCK IS NOT DISJOINT FROM THIS ONE AT ALL. It fires on
  //     exactly this region, and the ONLY thing that stops the CLI printing a
  //     verdict and this hedge together is that it `return`s above. ORDER IS
  //     LOAD-BEARING; the comment on that block already said so, directly above a
  //     comment here that said the opposite for two releases.
  //
  // And the exit code is independent of all of it: a refusal (5) is orthogonal to
  // every predicate here and co-occurs with this warning by design, which is why
  // the banner below no longer names a number.
  if (is_carry_masked_fit_failure(*report)) {
    // NO EXIT CODE IN THE BANNER (REV-R5, review I-2). This read
    // `WARNING (exit 0)`. Reaching this line says the three unconditional exit-3
    // predicates are false and (under --strict) the strict one too — it says
    // NOTHING about `dates_refused_coverage_regression`, so a run that carries
    // surfaces, fits nothing, and has one date refused printed `exit 0` here and
    // then returned 5. Two dates are enough to build it and the shape is ordinary.
    // The parenthetical is removed rather than corrected: this block does not
    // decide the exit code (`build_exit_code` never reads this predicate), so it
    // must not name one — a banner that cannot state a code cannot state a wrong
    // one, which is a stronger guarantee than two sites edited to agree today.
    std::fprintf(stderr,
                 "atx-vol-surface-db-build: WARNING: 0 cells fitted, %u failed, "
                 "%u carried.\n",
                 report->coverage.cells_failed, report->coverage.cells_carried);

    // FIX-G. The warning used to state counters and send the operator off to find
    // the `failed_cell` lines themselves. On the flagship database this fires on
    // EVERY run — the residual failures are a permanent per-run condition — so
    // "go read the report" is a cost paid every time, and a line that costs
    // something every time and says nothing new is the line that gets trained
    // away. The whole discriminator between (a) and (b) below is WHICH cells
    // failed, so put them on the warning itself.
    //
    // Bounded by the same `--max-failures` cap as the `failed_cell` block, using
    // the same library helper and the same never-silent elision count, so a
    // wholesale failure cannot flood stderr and a truncated list can never read
    // as the whole set.
    const ReportedFailedCells warned = reported_failed_cells(*report, max_failed_cells);
    std::fprintf(stderr, "  Cells that failed (%zu shown, %zu elided):", warned.reported.size(),
                 warned.n_elided);
    for (const FailedCell &f : warned.reported) {
      std::fprintf(stderr, " %s/%s", f.date.c_str(), f.symbol.c_str());
    }
    std::fprintf(stderr, "\n");

    std::fprintf(stderr,
                 "  This run produced no NEW surface. Two very different runs look like "
                 "this and the counters cannot tell them apart:\n"
                 "    (a) the converged steady state — a permanently-failing cell is retried "
                 "on every run (by design; nothing is persisted as known-failed) while its "
                 "healthy siblings are carried. Nothing is wrong.\n"
                 "    (b) every cell this run scheduled died for a SYSTEMATIC reason — a "
                 "fitter or loader regression, or a bad config for a newly-added name — "
                 "beside %u carried cells that were never re-fitted and so could not "
                 "re-fail. Before carry-over this run would have exited 3.\n"
                 "  Compare the list above with the previous run's: the SAME cells failing "
                 "the same way is (a); a fresh name, or a new reason, is (b). The "
                 "failed_cell lines on stdout carry each cell's own reason, and --report "
                 "writes every one of them.\n",
                 report->coverage.cells_carried);

    // The remedy, stated honestly. `atx-vol-surface-db disable` exists now (it did
    // not when this warning was written, which is why the manual named a C++ API
    // call), but it is a per-SYMBOL switch aimed at a per-CELL problem, and for
    // the population that actually produces this warning — a name that fits on 16
    // of its 17 dates — it is the wrong trade. Saying so here is the point: an
    // operator who cannot clear a line, and is not told that not clearing it is
    // correct, stops reading it.
    std::fprintf(
        stderr, "  Clearing it: fix the failing cell, or stop fitting the name entirely with "
                "`atx-vol-surface-db disable --db <root> --symbol <SYM> --yes` (its stored "
                "surfaces are kept). Disabling costs that symbol on EVERY date, so on a name "
                "that is healthy everywhere else it trades many good surfaces for one silenced "
                "line — usually a bad deal. If neither applies, this line is EXPECTED on every "
                "run and is not a defect on its own; what you watch is the list above CHANGING.\n");

    // What replaced the `(exit 0)` the header line used to carry: the SET of codes
    // this run can still return, rather than a single number this block does not
    // own. Every verdict block above returned instead of falling through to here,
    // so the three "produced nothing" predicates and the --strict one are all
    // false — which leaves exactly `build_exit_code`'s remaining branches, each
    // pinned by a SurfaceDbBuildExitCode test on a carry-masked report.
    std::fprintf(stderr,
                 "  This WARNING does not set the exit code: build_exit_code never reads this "
                 "predicate. Reaching it means no verdict block fired, so this run exits 0, or 5 "
                 "if a coverage regression was REFUSED (its banner is above), or 1 if --report "
                 "could not be written (its message is above).\n");
  }

  // No predicate fired, so the only thing that can still be wrong is the CSV the
  // operator asked for and did not get. A script that reads the file must not see
  // a green exit for a build whose report never landed.
  return build_exit_code(*report, report_write_failed, strict);
}
