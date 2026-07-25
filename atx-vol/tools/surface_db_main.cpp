// surface_db_main — the production surface-database MANAGEMENT CLI: inspect,
// verify and fence a database that `atx-vol-surface-db-build` produced, with no
// Python in the loop. Eight subcommands (`info`, `partitions`, `symbols`,
// `config`, `query`, `verify`, `enable`, `disable`), each a parse -> one library
// call -> print shell over atx/vol/surface_db_admin.hpp. All logic lives in that
// library; nothing here decides anything about the database.
//
// SIX OF THE EIGHT ARE READ-ONLY. `enable` / `disable` write the manifest, and
// they are the only writes this tool has. See the WRITE PATH note below the
// subcommand list.
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
//          [--probe-tenor T] [--max-failures N] [--min-cells N] [--max-absent N]
//                              walk every (partition, symbol) cell: map it, check
//                              its stored payload CRC, and evaluate one ATM
//                              point; print the counters, each failing cell, each
//                              ABSENT cell, and a `verdict` line. A walk that
//                              covered ZERO cells over a database that HAS
//                              partitions is itself a FAILED verdict.
//                              `--min-cells` additionally fails a database
//                              smaller than the operator expected, and
//                              `--max-absent` fails one MISSING more cells than
//                              expected — the two things the library cannot know.
//   enable  --symbol <SYM>     start fitting SYM again on every date.
//   disable --symbol <SYM> --yes
//                              STOP fitting SYM on every date. Its already-stored
//                              surfaces are kept, keep loading, and survive later
//                              rewrites verbatim — `enabled = false` means stop
//                              fitting, never delete.
//
// ── WRITE PATH ───────────────────────────────────────────────────────────────
//
// `enable` / `disable` are this tool's only writes and they change exactly one
// field of one symbol's stored config (`SymbolFitConfig::enabled`) through
// `set_symbol_enabled`, which hands it to the same `SurfaceDb::upsert_symbol` the
// build path uses. There is no `upsert`/`config --set` verb: a stored config is
// never clobbered here, matching `atx-vol-surface-db-build`, which deliberately
// withholds the library's `overwrite_existing` for the same reason.
//
// `--yes` is REQUIRED by `disable` and by nothing else. The asymmetry is the
// blast radius, not squeamishness about writing: `enabled` is a per-SYMBOL
// switch, so disabling a name to silence a handful of bad cells stops fitting it
// on EVERY date and every future date. `enable` is the recovering direction and
// needs no confirmation (the build CLI's `--retry-disabled` already re-enables
// without one).
//
// SINGLE WRITER. Do not run `enable`/`disable` while a build is running against
// the same root. A build holds one in-memory manifest snapshot for its whole run
// and every partition write persists that snapshot's symbol table, so a mutation
// landing mid-build is silently overwritten. Nothing detects this; it is the
// scheduling rule `surface_db.hpp` has always stated for this database.
// The reverse direction is worse and is not hypothetical: both writers rewrite
// the WHOLE manifest — symbols and the partition table — from their own snapshot
// with no compare-and-swap on `generation`, so an interleaved mutation can drop a
// partition record the build already committed and regress the generation, after
// which `refresh()` will not pick the newer manifest up. See
// `set_symbol_enabled`'s CONCURRENCY note in surface_db_admin.hpp.
//
// Output is line-oriented and stable for scripting: scalars print as `key value`
// (mirroring atx-vol-surface-db-build), and repeated records print as
// `<record> <id> field=value ...`. See atx-vol/docs/surface-db-build.md.
//
// Exit codes (same convention as atx-vol-surface-db-build):
//   0  ok — and, for `verify`, the walk covered cells and every one the database
//      HOLDS passed. Cells it does not hold are counted, named, and warned about
//      on stderr without moving this (see ABSENCE below). For `enable`/`disable`,
//      the symbol is now in the requested state, whether or not this run is what
//      put it there (`changed 0` is a success: the verbs are idempotent so a
//      converging script can assert the state unconditionally).
//   1  runtime failure (message on stderr), OR `verify` returned a FAILED verdict
//      (on stdout — a runtime failure prints no verdict line). An `enable` /
//      `disable` naming a symbol the manifest does not configure is a runtime
//      failure: it is a fact about the database, not about the command line.
//   2  usage error: unknown subcommand, unknown flag, a required flag missing (of
//      which `disable`'s `--yes` is one), a flag left WITHOUT a value, or a
//      malformed numeric value. Every one of these is decided before the database
//      is opened, so no usage error can ever have written anything.
//   4  `verify` only, and only when the operator asked for it: more cells are
//      ABSENT than the `--max-absent N` ceiling allows. Nothing is corrupt — this
//      is a COVERAGE answer, kept off code 1 so a script can tell "the database
//      is missing more than I said to expect" from "the database is damaged".
//      3 is skipped: it is atx-vol-surface-db-build's total-failure code and the
//      two tools share one exit vocabulary.
//
// ── ABSENCE, and why it is not a failure (FIX-H) ─────────────────────────────
//
// `verify` used to report a cell the database never stored and a cell it stored
// and can no longer read as the same thing (`unmappable`). The finished
// production database has 9 permanently-unfittable cells out of 867, so it
// printed `verdict FAILED` and exited 1 on every run while being completely
// healthy — and a permanently-red signal is not a signal. The partition's own
// archive directory tells the two apart, so absence now has its own counter, its
// own capped list, and no vote in the verdict.
//
// It is NOT quiet. Absence prints `cells_absent`, one `absent <KEY> <SYM>` line
// per cell (bounded by --max-failures, with a never-silent `absent_elided`), and
// a stderr block that states the two readings and says what to watch. It is not
// quiet in a SCRIPT either: `--max-absent N` turns "more than N cells missing"
// into exit 4. That flag is the instrument for the one thing this tool genuinely
// cannot see — a stored surface DESTROYED by a whole-partition rewrite is
// byte-for-byte a cell that was never fitted, so the count is the only handle,
// and only the operator knows what the expected count is. It is the same
// division of labour as `--min-cells`.
//
// STATE THE COST PLAINLY, because it is paid by whoever does not pass the flag:
// WITHOUT `--max-absent`, a run that DESTROYED stored surfaces exits 0. Before
// FIX-H it exited 1 — but so did every healthy run of the same database, which is
// why nothing could branch on it. The ceiling is what buys back a DISCRIMINATING
// non-zero, so it belongs in the first script that runs this. A database expected
// to have no holes at all pins `--max-absent 0`.
//
// One shape gets its own stderr warning on top: the walk read cells and the
// database held NONE of them (`DbVerifyReport::stored_no_selected_cell`). That
// was a FAILED verdict before FIX-H and is now `ok`, so the tool says it out
// loud. It stays exit 0 because a correct, deliberate narrowing on a HEALTHY
// database produces it — see the block at the call site.

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

// `verify` found more ABSENT cells than `--max-absent` allows. Its own code, and
// deliberately not 1: absence is a coverage answer over an otherwise intact
// database, and a script that treats "the database is damaged" and "the database
// is missing two more cells than last month" identically will act wrongly on one
// of them. Never reached without the flag — a converged production database is
// permanently non-zero on `cells_absent`, so an unconditional non-zero here would
// rebuild the permanently-red verdict this whole change removes (the same
// argument `is_carry_masked_fit_failure` records for keeping the build CLI at
// exit 0 — surface_db_build.hpp). 3 belongs to atx-vol-surface-db-build's
// total-failure code; the two tools share one exit vocabulary, so it is skipped.
constexpr int kExitAbsentOverLimit = 4;

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
      "         [--probe-tenor T] [--max-failures N] [--min-cells N] [--max-absent N]\n"
      "                                map + ATM-evaluate every cell; nonzero exit on failure\n"
      "  enable  --symbol SYM          resume fitting SYM on every date\n"
      "  disable --symbol SYM --yes    STOP fitting SYM on EVERY date (stored surfaces\n"
      "                                are kept and keep serving; --yes is required)\n"
      "exit: 0 ok / 1 runtime failure or verify found failing cells / 2 usage /\n"
      "      4 verify found more absent cells than --max-absent allows\n");
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

// The operator's expected ABSENT ceiling, or "no ceiling asked for". Absence is
// permanent and non-zero on a healthy converged database, so this is opt-in by
// construction: a default ceiling of 0 would exit non-zero on `prod-2026-07`
// forever and rebuild the trained-away signal FIX-H removed.
struct AbsentCeiling {
  bool set{false};
  std::size_t max{0};
};

// `min_cells` is a presentation-layer floor on `cells_checked`, not a library
// concept: it is the one number only the OPERATOR knows (how big this database is
// supposed to be), so a script asserts it here. It is no longer the only thing
// standing between a broken database and `verdict ok`, though — the library's
// `selected_no_cells()` already fails a walk that covered nothing over a populated
// db, so forgetting `--min-cells` no longer turns "checked nothing" into green.
// The floor still catches what the library cannot know: a database that IS
// smaller than intended (wrong window, lost partitions, never built).
//
// `absent` is the SAME division of labour on the other axis — not "is this
// database big enough?" but "is it missing the cells I already know it is missing,
// and no others?". The library cannot answer that either, and for a harder reason:
// a stored surface destroyed by a whole-partition rewrite leaves no trace, so the
// count of absences is the only handle there is, and the expected count is a fact
// about the universe rather than about the database.
int run_verify(const SurfaceDb &db, const DbVerifySpec &spec, std::size_t min_cells,
               AbsentCeiling absent) {
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
  // Between `cells_ok` and the three fault counters on purpose: the five terms
  // print in the order of the invariant they satisfy (they sum to cells_checked),
  // and absence sits on the healthy side of the fault boundary, not inside it.
  std::printf("cells_absent %zu\n", rep->cells_absent);
  std::printf("cells_unmappable %zu\n", rep->cells_unmappable);
  std::printf("cells_non_finite %zu\n", rep->cells_non_finite);
  std::printf("cells_checksum %zu\n", rep->cells_checksum);
  std::printf("symbols_disabled %zu\n", rep->disabled_symbols.size());
  std::printf("failures_reported %zu\n", rep->failures.size());
  std::printf("failures_elided %zu\n", rep->n_failures_elided);
  std::printf("absent_reported %zu\n", rep->absent_cells.size());
  std::printf("absent_elided %zu\n", rep->n_absent_elided);
  for (const DbCellFault &f : rep->failures) {
    std::printf("fail %s %s kind=%s detail=%s\n", f.key.c_str(), f.symbol.c_str(),
                failure_name(f.kind), f.detail.c_str());
  }
  // Its own record type, never a `fail` line, because a script that greps
  // `^fail ` is asking about damage and these cells are not damaged. No
  // `detail=`: there is no error to quote, and inventing one ("symbol not
  // present") is exactly the sentence that made this look like a fault for the
  // whole life of the tool.
  for (const DbAbsentCell &a : rep->absent_cells) {
    std::printf("absent %s %s\n", a.key.c_str(), a.symbol.c_str());
  }
  // The columns this walk never looked at. `verdict ok` over a database that is
  // permanently missing a requested name is otherwise indistinguishable from
  // `verdict ok` over a complete one — the symbol is simply not a column.
  for (const std::string &s : rep->disabled_symbols) {
    std::printf("disabled_symbol %s\n", s.c_str());
  }
  if (!rep->disabled_symbols.empty()) {
    // FIX-E corrected this message. It used to assert, as a parenthetical
    // statement of fact, that disabled symbols "are absent from every partition"
    // -- which was true only for a symbol disabled BEFORE it ever fitted, and was
    // being MADE true for the others by the very data-loss bug FIX-E repaired. A
    // symbol disabled after it fitted keeps its stored surfaces, and they stay
    // servable (nothing on the read path gates on `enabled`), so the default walk
    // skipping them now leaves REAL cells unchecked rather than merely
    // non-existent ones.
    std::fprintf(stderr,
                 "atx-vol-surface-db: %zu manifest symbol(s) are DISABLED and were not checked. "
                 "A symbol disabled BEFORE it ever fitted is absent from every partition, but one "
                 "disabled AFTER it fitted KEEPS its stored surfaces (and they still load), so "
                 "this walk may be leaving real cells unverified. The verdict below describes only "
                 "the symbols that were walked. Re-run with --include-disabled to check them, or "
                 "rebuild with --retry-disabled to re-attempt them.\n",
                 rep->disabled_symbols.size());
  }
  // The absence block. It prints whenever anything is absent, which on a
  // converged production database is EVERY run — and saying so is the point. The
  // discriminator between the two readings is not the line's existence but the
  // SET, so the cells are named above and the operator is told, here, that the
  // line recurring is expected and the set moving is not. The build CLI's
  // carry-masked warning says the same thing about the same population; the two
  // are one statement in two registers and should change together.
  if (rep->cells_absent > 0) {
    std::fprintf(stderr,
                 "atx-vol-surface-db: NOTE: %zu of %zu checked cell(s) are ABSENT — their "
                 "partition's archive directory does not list the symbol, so no surface was "
                 "ever stored for them. This does NOT move the verdict.\n"
                 "  %zu named as `absent` lines on stdout, %zu elided by --max-failures.\n",
                 rep->cells_absent, rep->cells_checked, rep->absent_cells.size(),
                 rep->n_absent_elided);
    std::fprintf(
        stderr,
        "  Two very different histories produce an absent cell and NOTHING on disk tells "
        "them apart — the format keeps no tombstone, so a destroyed cell is byte-for-byte "
        "a cell that was never fitted:\n"
        "    (a) the fit for that (date, symbol) permanently FAILS, so nothing was ever "
        "written. Expected, permanent, not a defect — this is the converged steady state, "
        "and it is why the verdict ignores this count.\n"
        "    (b) a surface WAS stored there and is GONE. A present, enabled cell whose "
        "re-fit fails loses its stored surface, because a partition rewrite is whole-file. "
        "That is measured, current behaviour, not a hypothetical.\n"
        "  Compare the `absent` list with the previous run's: the SAME cells is (a); cells "
        "that used to verify and now do not is (b), and the count is the only handle you "
        "have on it. Wire the expected count into the script with `--max-absent N` so a "
        "growth exits %d instead of needing a human to notice — WITHOUT it, a run that "
        "destroyed stored surfaces exits 0.\n",
        kExitAbsentOverLimit);
  }
  // EVERY cell the walk read is a hole. `ok()` is true (nothing failed a gate),
  // `--min-cells` is satisfied (it counts the grid, and a grid of pure holes is
  // full-sized), and `selected_no_cells` cannot see it (the walk was not empty).
  // Before FIX-H this shape arrived as `unmappable` and a FAILED verdict, so on a
  // database that expects no holes it was an alarm that has just gone quiet.
  //
  // It stays a WARNING and not an exit code because it is reachable by a correct
  // invocation on a healthy database — `--symbols MCD --from 2026-07-01 --to
  // 2026-07-01` against `prod-2026-07` is exactly this shape, and it is the right
  // answer. The full argument is at `stored_no_selected_cell`'s declaration. Like
  // the build CLI's carry-masked warning, this names the two readings instead of
  // choosing between them.
  if (rep->stored_no_selected_cell()) {
    std::fprintf(
        stderr,
        "atx-vol-surface-db: WARNING (exit 0 unless a ceiling says otherwise): the walk read "
        "%zu cell(s) and this database holds NONE of them — every one is absent. Nothing "
        "failed a gate, so the verdict below is about a database that stored nothing where "
        "you looked.\n"
        "  Two readings, and the counters cannot tell them apart:\n"
        "    (a) you NARROWED the walk onto cells this database legitimately does not hold "
        "— a --symbols name the manifest never configured, or a --from/--to window whose "
        "every cell permanently fails. The answer is correct and nothing is wrong.\n"
        "    (b) the database holds nothing WHERE YOU LOOKED — it was never built over that "
        "window, it was built over a different one, or every surface there has been "
        "destroyed.\n"
        "  The walk selected %zu symbol(s) over %zu of the %zu partition(s) in the db; if "
        "you did not narrow it deliberately, read (b). If this database is supposed to have "
        "NO absent cells, run it with `--max-absent 0` so this exits %d instead of 0.\n",
        rep->cells_checked, rep->n_symbols, rep->n_partitions, rep->n_partitions_in_db,
        kExitAbsentOverLimit);
  }
  std::printf("min_cells %zu\n", min_cells);
  if (absent.set) {
    std::printf("max_absent %zu\n", absent.max);
  } else {
    std::printf("max_absent unset\n");
  }
  const bool enough = rep->cells_checked >= min_cells;
  if (!enough) {
    std::fprintf(stderr,
                 "atx-vol-surface-db: verify checked %zu cells, below the required minimum %zu\n",
                 rep->cells_checked, min_cells);
  }
  const bool within_absent_ceiling = !absent.set || rep->cells_absent <= absent.max;
  if (!within_absent_ceiling) {
    std::fprintf(stderr,
                 "atx-vol-surface-db: verify found %zu absent cell(s), above the declared "
                 "maximum %zu. Nothing is corrupt — %zu cell(s) failed a gate — but this "
                 "database is missing cells you did not expect it to be missing. Diff the "
                 "`absent` lines above against the set you sized this ceiling on.\n",
                 rep->cells_absent, absent.max,
                 rep->cells_unmappable + rep->cells_non_finite + rep->cells_checksum);
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
  //
  // Three values now, and FAILED wins: a database that is both damaged and
  // missing more than expected has one answer worth acting on first, and a
  // caller that saw only `ABSENT` would go looking for missing coverage while
  // the bytes were rotting. `ABSENT` therefore means EXACTLY "nothing failed a
  // gate, the walk was big enough, and the only thing wrong is that more cells
  // are missing than you declared" — it is unreachable without --max-absent.
  const bool failed = !rep->ok() || !enough;
  if (failed) {
    std::printf("verdict FAILED\n");
    return 1;
  }
  if (!within_absent_ceiling) {
    std::printf("verdict ABSENT\n");
    return kExitAbsentOverLimit;
  }
  std::printf("verdict ok\n");
  return 0;
}

// `enable` and `disable` are the same call with a different bool. The only
// asymmetry is which advisory note follows, and each note exists because the verb
// it follows is routinely reached for the wrong reason:
//
//   disable — an operator arrives here from the build CLI's carry-masked warning,
//             which names FAILING CELLS. `enabled` is per SYMBOL. Say the cost out
//             loud at the moment it is paid, and say what was NOT lost (the
//             stored surfaces), because that is the invariant the operator has to
//             trust to use this at all.
//   enable  — this flips the stored config's bit and runs no selection. If the
//             disable came from a failed config SELECTION, the stored config is
//             the generic fallback, and re-selecting needs `--retry-disabled` on a
//             symbol that is STILL disabled (`surface_db_build.cpp`: an enabled
//             existing config is `n_skipped_existing` regardless of the flag). So
//             the note must NOT say "just rebuild with --retry-disabled" — by the
//             time it prints, this command has already disarmed that flag, and the
//             rebuild would silently fit under the fallback instead.
//
// Both notes go to stderr so stdout stays the parseable `key value` record.
int run_set_enabled(SurfaceDb &db, const std::string &symbol, bool enabled) {
  const Result<SymbolEnableChange> ch = set_symbol_enabled(db, symbol, enabled);
  if (!ch) {
    std::fprintf(stderr, "atx-vol-surface-db: set_symbol_enabled(%s, %s): %s\n", symbol.c_str(),
                 enabled ? "enabled" : "disabled", ch.error().to_string().c_str());
    return 1;
  }
  std::printf("symbol %s\n", ch->symbol.c_str());
  std::printf("enabled_before %d\n", ch->was_enabled ? 1 : 0);
  std::printf("enabled %d\n", ch->now_enabled ? 1 : 0);
  std::printf("changed %d\n", ch->changed ? 1 : 0);
  std::printf("generation %llu\n", static_cast<unsigned long long>(ch->generation));

  if (!ch->changed) {
    std::fprintf(stderr, "atx-vol-surface-db: %s was already %s; nothing was written.\n",
                 ch->symbol.c_str(), enabled ? "enabled" : "disabled");
    return 0;
  }
  if (enabled) {
    std::fprintf(stderr,
                 "atx-vol-surface-db: %s is ENABLED again and will be fitted on every date the "
                 "hive carries for it.\n"
                 "  This restored the STORED config as-is; no selection was re-run. If the disable "
                 "came from a failed config selection (config.failed_symbols on a build report, "
                 "n_disabled_failed), that stored config is the generic preset fallback and the "
                 "symbol will now be fitted under it.\n"
                 "  To re-SELECT it instead, run 'disable %s' and then rebuild with "
                 "--retry-disabled. Do NOT just rebuild with --retry-disabled now: that flag only "
                 "re-selects symbols whose stored config is STILL DISABLED, so on a symbol this "
                 "command has just enabled it is a silent no-op — the build skips it as already "
                 "configured, fits it under the fallback, and reports nothing.\n",
                 ch->symbol.c_str(),
                 ch->symbol.c_str());
    return 0;
  }
  std::fprintf(stderr,
               "atx-vol-surface-db: %s is DISABLED. It will not be fitted on ANY date, past or "
               "future, until it is enabled again.\n"
               "  Nothing was deleted: every surface %s already fitted stays in its partition, "
               "still loads, still serves, and is re-emitted verbatim through any later rewrite of "
               "those dates (coverage.cells_carried_disabled on the build report).\n"
               "  If you came here to silence a few failing CELLS on an otherwise healthy name, "
               "this is the wrong instrument — it costs that name on every date it fits today. "
               "See 'Disabling a name' in atx-vol/docs/surface-db-build.md.\n",
               ch->symbol.c_str(), ch->symbol.c_str());
  return 0;
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
  AbsentCeiling max_absent;
  bool confirmed = false; // --yes; required by `disable` and by nothing else

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
    } else if (a == "--yes") {
      confirmed = true;
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
    } else if (a == "--max-absent") {
      // Same strict rule as --min-cells and --max-failures, and for the sharper
      // version of the same reason: this flag's entire job is to fail a database
      // that is missing MORE than expected, so coercing a typo to 0 would not
      // merely fail open — it would flip the flag's meaning to "no cell may ever
      // be absent" and fire on every healthy run instead.
      const std::string_view text = nv();
      if (!missing_value && !parse_count(text, max_absent.max)) {
        std::fprintf(stderr,
                     "atx-vol-surface-db: --max-absent expects a non-negative integer, got "
                     "'%.*s'\n",
                     static_cast<int>(text.size()), text.data());
        print_usage(stderr);
        return 2;
      }
      max_absent.set = !missing_value;
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
                                subcommand == "query" || subcommand == "verify" ||
                                subcommand == "enable" || subcommand == "disable";
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
  if ((subcommand == "enable" || subcommand == "disable") && symbol.empty()) {
    std::fprintf(stderr, "atx-vol-surface-db: %s requires --symbol <SYM>\n", subcommand.c_str());
    return 2;
  }
  // The confirmation. Checked HERE, with the other usage rules, so a `disable`
  // without `--yes` cannot have opened the database, let alone written to it —
  // the tool's standing rule that a usage error never depends on the db being
  // readable buys, for free, that it also never depends on the db being intact.
  // The message is the warning, not a scolding: it states the blast radius the
  // operator is confirming and the one thing that is NOT at risk.
  if (subcommand == "disable" && !confirmed) {
    std::fprintf(stderr,
                 "atx-vol-surface-db: disable requires --yes.\n"
                 "  `enabled` is a per-SYMBOL switch: disabling %s stops it being fitted on EVERY "
                 "date it fits today and on every future date. It does NOT delete anything — the "
                 "surfaces it already produced stay stored and keep serving.\n"
                 "  If you are here to silence a few failing cells on an otherwise healthy name, "
                 "that trade is almost never worth it; see 'Disabling a name' in "
                 "atx-vol/docs/surface-db-build.md.\n",
                 symbol.c_str());
    return 2;
  }

  // Non-const because `enable`/`disable` mutate. Note what did NOT change: there
  // is one `SurfaceDb::open`, it takes no lock and has no read-only mode (it
  // reads the manifest bytes and parses them), so the read-only-ness of this tool
  // was only ever this local's `const`. The six query subcommands still bind it
  // to a `const SurfaceDb &` and are unaffected.
  Result<SurfaceDb> db = SurfaceDb::open(db_root);
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
  if (subcommand == "enable") {
    return run_set_enabled(*db, symbol, true);
  }
  if (subcommand == "disable") {
    return run_set_enabled(*db, symbol, false);
  }
  // `verify` is the last of the eight `known_subcommand` names, so this is the
  // end of the dispatch — an unknown name already exited 2 above, before the open.
  return run_verify(*db, verify_spec, min_cells, max_absent);
}
