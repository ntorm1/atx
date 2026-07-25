// surface_db_main — the production surface-database MANAGEMENT CLI: inspect and
// verify a database that `atx-vol-surface-db-build` produced, with no Python in
// the loop. Six subcommands (`info`, `partitions`, `symbols`, `config`, `query`,
// `verify`), each a parse -> one library call -> print shell over
// atx/vol/surface_db_admin.hpp. All logic lives in that library; nothing here
// decides anything about the database.
//
// Usage:
//   atx-vol-surface-db <subcommand> --db <root> [flags]
//
//   info                       whole-db shape: generation, counts, bytes, and
//                              one `partition` line per partition.
//   partitions                 one `partition` line per partition.
//   partitions --key <KEY>     what that partition actually holds: a `partition`
//                              line then one `surface` line per symbol.
//   symbols                    one `symbol` line per configured symbol.
//   config --symbol <SYM>      one symbol's full stored fit config + provenance.
//   query --key <KEY> --symbol <SYM> --strike <K> --tenor <T>
//                              evaluate one cell through the zero-copy
//                              map_surface path (iv, total variance, forward).
//   verify [--from KEY] [--to KEY] [--symbols A,B,C] [--include-disabled]
//          [--probe-tenor T] [--max-failures N] [--min-cells N]
//                              walk every (partition, symbol) cell: map it, check
//                              its stored payload CRC, and evaluate one ATM
//                              point; print the counters, each failing cell, and
//                              a `verdict` line. A walk that covered ZERO cells
//                              over a database that HAS partitions is itself a
//                              FAILED verdict. `--min-cells` additionally fails a
//                              database smaller than the operator expected — the
//                              one thing the library cannot know.
//
// Output is line-oriented and stable for scripting: scalars print as `key value`
// (mirroring atx-vol-surface-db-build), and repeated records print as
// `<record> <id> field=value ...`. See atx-vol/docs/surface-db-build.md.
//
// Exit codes (same convention as atx-vol-surface-db-build):
//   0  ok — and, for `verify`, the walk covered cells and every one passed.
//   1  runtime failure (message on stderr), OR `verify` returned a FAILED verdict
//      (on stdout — a runtime failure prints no verdict line).
//   2  usage error: unknown subcommand, unknown flag, a required flag missing, a
//      flag left WITHOUT a value, or a malformed numeric value. Every one of
//      these is decided before the database is opened.

#include <cerrno>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>

#include "atx/vol/session.hpp"          // FitPreset
#include "atx/vol/surface_db.hpp"       // SurfaceDb
#include "atx/vol/surface_db_admin.hpp" // describe_*, query_surface, verify_db
#include "atx/vol/surface_policy.hpp"   // to_string(SurfacePurpose/FitQualityMode/SurfaceState)
#include "atx/vol/types.hpp"            // Result, Status
#include "atx/vol/vol_curve.hpp"        // to_string(VolCurveKind)

using namespace atx::vol;

namespace {

void print_usage(std::FILE *out) {
  std::fprintf(
      out,
      "usage: atx-vol-surface-db <subcommand> --db <root> [flags]\n"
      "  info                          whole-db shape (counts, bytes, partitions)\n"
      "  partitions [--key KEY]        list partitions, or one partition's symbols\n"
      "  symbols                       list configured symbols\n"
      "  config --symbol SYM           one symbol's stored fit config + provenance\n"
      "  query --key KEY --symbol SYM --strike K --tenor T\n"
      "                                evaluate one cell (zero-copy map_surface)\n"
      "  verify [--from KEY] [--to KEY] [--symbols A,B,C] [--include-disabled]\n"
      "         [--probe-tenor T] [--max-failures N] [--min-cells N]\n"
      "                                map + ATM-evaluate every cell; nonzero exit on failure\n"
      "exit: 0 ok / 1 runtime failure or verify found failing cells / 2 usage\n");
}

// Parse a non-negative count from a flag value, consuming the WHOLE token.
//
// Carries the same discipline as atx-vol-surface-db-build's `parse_finite_double`
// (--r), and for the same reason. The bare `strtoull` this replaced silently
// coerced anything unparseable to 0 — and for `--min-cells`, 0 is "no floor at
// all", so the one flag whose entire job is to fail a too-small database FAILED
// OPEN. `verify --db /db --min-cells $EXPECTED` with EXPECTED unset drops the
// word entirely, the shell hands us `--min-cells` as the last argv, the value
// reads as ""; an empty or all-disabled database then printed `verdict ok` and
// exited 0 with no diagnostic. `--min-cells abc` did the same. Both are usage
// errors now.
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

// Split a comma-separated list, trimming whitespace and dropping empty fields —
// same rule as atx-vol-surface-db-build's --symbols, so the two tools accept
// identical strings.
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

// Stable spellings for the enums the CLI prints. FitPreset has no library
// to_string (the other three do), and these names must match
// atx-vol-surface-db-build's --preset vocabulary so a config listing can be fed
// straight back to a rebuild.
[[nodiscard]] const char *preset_name(FitPreset preset) noexcept {
  switch (preset) {
  case FitPreset::Fast:
    return "fast";
  case FitPreset::Accurate:
    return "accurate";
  case FitPreset::Robust:
    return "robust";
  case FitPreset::Hft:
    return "hft";
  case FitPreset::Populate:
    return "populate";
  }
  return "unknown"; // unreachable for valid enumerators
}

[[nodiscard]] const char *failure_name(DbCellFailure kind) noexcept {
  switch (kind) {
  case DbCellFailure::Unmappable:
    return "unmappable";
  case DbCellFailure::NonFinite:
    return "non_finite";
  case DbCellFailure::ChecksumMismatch:
    return "checksum";
  }
  return "unknown"; // unreachable for valid enumerators
}

// One partition record line — the shared shape of `info` and `partitions`.
void print_partition_line(const DbPartitionSummary &p) {
  std::printf("partition %s surfaces=%u manifest_bytes=%llu bytes_on_disk=%llu present=%d "
              "created_ts_ns=%lld\n",
              p.key.c_str(), p.surface_count,
              static_cast<unsigned long long>(p.manifest_bytes),
              static_cast<unsigned long long>(p.bytes_on_disk), p.present ? 1 : 0,
              static_cast<long long>(p.created_ts_ns));
}

// ── Subcommands (parse -> one library call -> print) ────────────────────────

int run_info(const SurfaceDb &db) {
  const Result<DbDescription> desc = describe_db(db);
  if (!desc) {
    std::fprintf(stderr, "atx-vol-surface-db: describe_db: %s\n", desc.error().to_string().c_str());
    return 1;
  }
  std::printf("root %s\n", desc->root.c_str());
  std::printf("generation %llu\n", static_cast<unsigned long long>(desc->generation));
  std::printf("symbols %zu\n", desc->n_symbols);
  std::printf("symbols_enabled %zu\n", desc->n_symbols_enabled);
  std::printf("partitions %zu\n", desc->n_partitions);
  std::printf("partitions_missing %zu\n", desc->n_partitions_missing);
  std::printf("surfaces %llu\n", static_cast<unsigned long long>(desc->total_surface_count));
  std::printf("manifest_bytes %llu\n", static_cast<unsigned long long>(desc->total_manifest_bytes));
  std::printf("bytes_on_disk %llu\n", static_cast<unsigned long long>(desc->total_bytes_on_disk));
  for (const DbPartitionSummary &p : desc->partitions) {
    print_partition_line(p);
  }
  return 0;
}

int run_partitions(const SurfaceDb &db, const std::string &key) {
  if (key.empty()) {
    const Result<DbDescription> desc = describe_db(db);
    if (!desc) {
      std::fprintf(stderr, "atx-vol-surface-db: describe_db: %s\n",
                   desc.error().to_string().c_str());
      return 1;
    }
    for (const DbPartitionSummary &p : desc->partitions) {
      print_partition_line(p);
    }
    return 0;
  }

  const Result<PartitionDescription> part = describe_partition(db, key);
  if (!part) {
    std::fprintf(stderr, "atx-vol-surface-db: describe_partition(%s): %s\n", key.c_str(),
                 part.error().to_string().c_str());
    return 1;
  }
  std::printf("partition %s manifest_surfaces=%u archive_surfaces=%u manifest_bytes=%llu "
              "bytes_on_disk=%llu\n",
              part->key.c_str(), part->manifest_surface_count, part->archive_surface_count,
              static_cast<unsigned long long>(part->manifest_bytes),
              static_cast<unsigned long long>(part->bytes_on_disk));
  for (const PartitionSymbolInfo &s : part->symbols) {
    std::printf("surface %s uid=%u slices=%u bytes=%llu\n", s.symbol.c_str(), s.uid, s.n_slices,
                static_cast<unsigned long long>(s.surface_bytes));
  }
  return 0;
}

int run_symbols(const SurfaceDb &db) {
  for (const std::string &name : db.symbols()) {
    const Result<SymbolDescription> sym = describe_symbol(db, name);
    if (!sym) {
      std::fprintf(stderr, "atx-vol-surface-db: describe_symbol(%s): %s\n", name.c_str(),
                   sym.error().to_string().c_str());
      return 1;
    }
    std::printf("symbol %s enabled=%d preset=%s pin_curve=%d curve=%s provenance=%d\n",
                sym->symbol.c_str(), sym->enabled ? 1 : 0, preset_name(sym->preset),
                sym->pin_curve ? 1 : 0, to_string(sym->curve_kind), sym->has_provenance ? 1 : 0);
  }
  return 0;
}

int run_config(const SurfaceDb &db, const std::string &symbol) {
  const Result<SymbolDescription> sym = describe_symbol(db, symbol);
  if (!sym) {
    std::fprintf(stderr, "atx-vol-surface-db: describe_symbol(%s): %s\n", symbol.c_str(),
                 sym.error().to_string().c_str());
    return 1;
  }
  std::printf("symbol %s\n", sym->symbol.c_str());
  std::printf("enabled %d\n", sym->enabled ? 1 : 0);
  std::printf("preset %s\n", preset_name(sym->preset));
  std::printf("pin_curve %d\n", sym->pin_curve ? 1 : 0);
  std::printf("curve %s\n", to_string(sym->curve_kind));
  std::printf("band_k %.17g\n", sym->band_k);
  std::printf("policy.quality_mode %s\n",
              std::string(to_string(sym->surface_policy.quality_mode)).c_str());
  std::printf("policy.risk_admission %d\n",
              static_cast<int>(sym->surface_policy.risk_admission));
  std::printf("policy.fallback %d\n", static_cast<int>(sym->surface_policy.fallback));
  std::printf("provenance %d\n", sym->has_provenance ? 1 : 0);
  if (sym->has_provenance) {
    const SurfaceProvenance &p = sym->provenance;
    std::printf("provenance.purpose %s\n", std::string(to_string(p.purpose)).c_str());
    std::printf("provenance.quality_mode %s\n", std::string(to_string(p.quality_mode)).c_str());
    std::printf("provenance.state %s\n", std::string(to_string(p.state)).c_str());
    std::printf("provenance.admitted %d\n", p.validation.admitted() ? 1 : 0);
    std::printf("provenance.validation_failures %u\n",
                static_cast<unsigned>(p.validation.failures));
    std::printf("provenance.source_generation %llu\n",
                static_cast<unsigned long long>(p.source_generation));
    std::printf("provenance.served_generation %llu\n",
                static_cast<unsigned long long>(p.served_generation));
    std::printf("provenance.legacy_format %d\n", p.legacy_format ? 1 : 0);
  }
  return 0;
}

int run_query(const SurfaceDb &db, const std::string &key, const std::string &symbol, double K,
              double T) {
  const Result<SurfacePointQuote> q = query_surface(db, key, symbol, K, T);
  if (!q) {
    std::fprintf(stderr, "atx-vol-surface-db: query_surface(%s/%s): %s\n", key.c_str(),
                 symbol.c_str(), q.error().to_string().c_str());
    return 1;
  }
  std::printf("key %s\n", q->key.c_str());
  std::printf("symbol %s\n", q->symbol.c_str());
  std::printf("strike %.17g\n", q->K);
  std::printf("tenor %.17g\n", q->T);
  std::printf("iv %.17g\n", q->iv);
  std::printf("total_variance %.17g\n", q->total_variance);
  std::printf("forward %.17g\n", q->forward);
  std::printf("uid %u\n", q->uid);
  std::printf("n_slices %zu\n", q->n_slices);
  return 0;
}

// `min_cells` is a presentation-layer floor on `cells_checked`, not a library
// concept: it is the one number only the OPERATOR knows (how big this database is
// supposed to be), so a script asserts it here. It is no longer the only thing
// standing between a broken database and `verdict ok`, though — the library's
// `selected_no_cells()` already fails a walk that covered nothing over a populated
// db, so forgetting `--min-cells` no longer turns "checked nothing" into green.
// The floor still catches what the library cannot know: a database that IS
// smaller than intended (wrong window, lost partitions, never built).
int run_verify(const SurfaceDb &db, const DbVerifySpec &spec, std::size_t min_cells) {
  const Result<DbVerifyReport> rep = verify_db(db, spec);
  if (!rep) {
    std::fprintf(stderr, "atx-vol-surface-db: verify_db: %s\n", rep.error().to_string().c_str());
    return 1;
  }
  std::printf("partitions %zu\n", rep->n_partitions);
  std::printf("partitions_in_db %zu\n", rep->n_partitions_in_db);
  std::printf("symbols %zu\n", rep->n_symbols);
  std::printf("cells_checked %zu\n", rep->cells_checked);
  std::printf("cells_ok %zu\n", rep->cells_ok);
  std::printf("cells_unmappable %zu\n", rep->cells_unmappable);
  std::printf("cells_non_finite %zu\n", rep->cells_non_finite);
  std::printf("cells_checksum %zu\n", rep->cells_checksum);
  std::printf("failures_reported %zu\n", rep->failures.size());
  std::printf("failures_elided %zu\n", rep->n_failures_elided);
  for (const DbCellFault &f : rep->failures) {
    std::printf("fail %s %s kind=%s detail=%s\n", f.key.c_str(), f.symbol.c_str(),
                failure_name(f.kind), f.detail.c_str());
  }
  std::printf("min_cells %zu\n", min_cells);
  const bool enough = rep->cells_checked >= min_cells;
  if (!enough) {
    std::fprintf(stderr,
                 "atx-vol-surface-db: verify checked %zu cells, below the required minimum %zu\n",
                 rep->cells_checked, min_cells);
  }
  // A walk that covered nothing over a database that HAS partitions prints every
  // counter as zero and no fail line — the exact shape of a perfect result. Say
  // out loud why it is not one, and how each of the three doors is reached.
  if (rep->selected_no_cells()) {
    std::fprintf(stderr,
                 "atx-vol-surface-db: verify checked 0 cells while the database holds %zu "
                 "partitions — nothing was read, so nothing could fail.\n"
                 "  %zu partition(s) matched --from/--to, and %zu symbol(s) were selected. A "
                 "zero symbol count means every symbol is fail-closed DISABLED (use "
                 "--include-disabled to prove they are absent) or --symbols named none that the "
                 "manifest configures; a zero partition count means --from/--to matched no "
                 "partition.\n",
                 rep->n_partitions_in_db, rep->n_partitions, rep->n_symbols);
  }
  // The verdict is the scriptable answer; the exit code mirrors it so a shell
  // can branch on either. A runtime failure above returns 1 WITHOUT a verdict.
  const bool passed = rep->ok() && enough;
  std::printf("verdict %s\n", passed ? "ok" : "FAILED");
  return passed ? 0 : 1;
}

} // namespace

int main(int argc, char **argv) {
  if (argc < 2) {
    print_usage(stderr);
    return 2;
  }
  const std::string subcommand = argv[1];
  if (subcommand == "--help" || subcommand == "-h" || subcommand == "help") {
    print_usage(stdout);
    return 0;
  }

  std::string db_root;
  std::string key;
  std::string symbol;
  std::string strike_arg;
  std::string tenor_arg;
  DbVerifySpec verify_spec;
  std::size_t min_cells = 0;

  for (int i = 2; i < argc; ++i) {
    const std::string_view a = argv[i];
    // Value for a flag that takes one. A flag that ENDED the argv used to yield
    // "", and every consumer then read that as a deliberate choice: `--key` meant
    // "list every partition" (the operator asked about one and silently got all
    // of them, exit 0), `--min-cells` meant "no floor". A dropped shell variable
    // is never a choice — record it and make it a usage error below.
    bool missing_value = false;
    const auto nv = [&]() -> const char * {
      if (i + 1 < argc) {
        return argv[++i];
      }
      missing_value = true;
      return "";
    };
    if (a == "--db") {
      db_root = nv();
    } else if (a == "--key") {
      key = nv();
    } else if (a == "--symbol") {
      symbol = nv();
    } else if (a == "--strike") {
      strike_arg = nv();
    } else if (a == "--tenor") {
      tenor_arg = nv();
    } else if (a == "--from") {
      verify_spec.key_lo = nv();
    } else if (a == "--to") {
      verify_spec.key_hi = nv();
    } else if (a == "--symbols") {
      verify_spec.symbols = split_csv(nv());
    } else if (a == "--include-disabled") {
      verify_spec.include_disabled = true;
    } else if (a == "--probe-tenor") {
      verify_spec.probe_T = std::strtod(nv(), nullptr);
    } else if (a == "--max-failures") {
      const std::string_view text = nv();
      if (!missing_value && !parse_count(text, verify_spec.max_reported_failures)) {
        std::fprintf(stderr,
                     "atx-vol-surface-db: --max-failures expects a non-negative integer, got "
                     "'%.*s'\n",
                     static_cast<int>(text.size()), text.data());
        print_usage(stderr);
        return 2;
      }
    } else if (a == "--min-cells") {
      const std::string_view text = nv();
      if (!missing_value && !parse_count(text, min_cells)) {
        std::fprintf(stderr,
                     "atx-vol-surface-db: --min-cells expects a non-negative integer, got '%.*s'\n",
                     static_cast<int>(text.size()), text.data());
        print_usage(stderr);
        return 2;
      }
    } else if (a == "--help" || a == "-h") {
      print_usage(stdout);
      return 0;
    } else {
      std::fprintf(stderr, "atx-vol-surface-db: unknown flag: %s\n", argv[i]);
      print_usage(stderr);
      return 2;
    }
    // One check for EVERY value-taking flag: a flag that ended the argv never
    // reaches its consumer as "".
    if (missing_value) {
      std::fprintf(stderr, "atx-vol-surface-db: %.*s requires a value\n",
                   static_cast<int>(a.size()), a.data());
      print_usage(stderr);
      return 2;
    }
  }

  // Is this even a subcommand? Checked FIRST, and before the open: a typo'd
  // subcommand alongside an unreadable --db used to report the open failure and
  // exit 1, telling the operator to go look at the database when the command
  // itself was wrong. A usage error must never depend on the db being readable.
  const bool known_subcommand = subcommand == "info" || subcommand == "partitions" ||
                                subcommand == "symbols" || subcommand == "config" ||
                                subcommand == "query" || subcommand == "verify";
  if (!known_subcommand) {
    std::fprintf(stderr, "atx-vol-surface-db: unknown subcommand: %s\n", subcommand.c_str());
    print_usage(stderr);
    return 2;
  }

  if (db_root.empty()) {
    std::fprintf(stderr, "atx-vol-surface-db: --db <root> is required\n");
    print_usage(stderr);
    return 2;
  }

  // Per-subcommand required flags, checked BEFORE opening the db so a usage
  // error never depends on the database being readable.
  if (subcommand == "config" && symbol.empty()) {
    std::fprintf(stderr, "atx-vol-surface-db: config requires --symbol <SYM>\n");
    return 2;
  }
  if (subcommand == "query" &&
      (key.empty() || symbol.empty() || strike_arg.empty() || tenor_arg.empty())) {
    std::fprintf(stderr,
                 "atx-vol-surface-db: query requires --key, --symbol, --strike and --tenor\n");
    return 2;
  }

  const Result<SurfaceDb> db = SurfaceDb::open(db_root);
  if (!db) {
    std::fprintf(stderr, "atx-vol-surface-db: open(%s): %s\n", db_root.c_str(),
                 db.error().to_string().c_str());
    return 1;
  }

  if (subcommand == "info") {
    return run_info(*db);
  }
  if (subcommand == "partitions") {
    return run_partitions(*db, key);
  }
  if (subcommand == "symbols") {
    return run_symbols(*db);
  }
  if (subcommand == "config") {
    return run_config(*db, symbol);
  }
  if (subcommand == "query") {
    return run_query(*db, key, symbol, std::strtod(strike_arg.c_str(), nullptr),
                     std::strtod(tenor_arg.c_str(), nullptr));
  }
  // `verify` is the last of the six `known_subcommand` names, so this is the end
  // of the dispatch — an unknown name already exited 2 above, before the open.
  return run_verify(*db, verify_spec, min_cells);
}
