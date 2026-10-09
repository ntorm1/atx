#pragma once

// surface_db_build_cli — a tiny, header-only SEAM pulled out of
// surface_db_build_main.cpp's arg loop so the --snapshot-suffix validator is
// unit-testable without spawning the real CLI process (the arg loop itself
// stays in the .cpp: this header owns only the one pure parse/validate
// decision that needed a home reachable from a test binary).
//
// Task 4 addendum §B: the C++ hive loader applies ONE `snapshot_suffix`
// uniformly per load call (opra_hive.cpp:144: `di.snapshot_iso = di.date +
// spec.snapshot_suffix`), and that stamp feeds T-to-expiry math
// (opra_panel.cpp ~565-770). ET-anchored pulls (pull_opra_hive.py's
// --snap-et) land at 19:55Z on EDT dates and 20:55Z on EST dates, so a build
// spanning only one DST side of a backfill must stamp with the matching
// suffix -- the orchestrator resolves and passes a fresh
// `--snapshot-suffix T{HH}:{MM}:00Z` per chunk (see run_surface_db_backfill.py).

#include <cstddef>
#include <cstdint>
#include <fstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "atx/vol/api/marketdata/opra_batch.hpp" // CorpusMarketInputTable, read_corpus_spot_inputs
#include "atx/vol/api/marketdata/opra_hive.hpp" // OpraHiveSpec (apply_snapshot_suffix_flag)
#include "atx/vol/api/storage/surface_db.hpp"   // kSurfaceDbKeyMax (--symbols-file cap)

namespace atx::vol {

// Validate a `--snapshot-suffix` value against `^T\d{2}:\d{2}:\d{2}Z$`
// (hand-rolled, no <regex> dependency -- same file-avoids-regex convention
// `surface_db_build_main.cpp`'s existing `parse_count`/`parse_finite_double`/
// `parse_bool` already follow). Format ONLY: this does not range-check the
// hour/minute/second digits (e.g. "T99:99:99Z" passes) -- exactly what the
// addendum's regex describes, and matching the file's existing --r validator,
// which also only rejects UNPARSEABLE input, not out-of-range-but-well-formed
// input.
//
// Same strictness discipline as --r (parse_finite_double): every value here is
// a claim about which UTC instant a hive session was snapshotted at, and a
// wrong stamp silently mis-scores every T-to-expiry computation downstream
// rather than raising -- so a malformed value must be a loud usage error
// (exit 2), never a silently-accepted default.
[[nodiscard]] inline bool is_valid_snapshot_suffix(std::string_view text) noexcept {
  constexpr std::size_t kLen = 10; // T DD : DD : DD Z
  if (text.size() != kLen) {
    return false;
  }
  if (text[0] != 'T' || text[3] != ':' || text[6] != ':' || text[9] != 'Z') {
    return false;
  }
  const auto is_digit = [](char c) noexcept { return c >= '0' && c <= '9'; };
  for (const std::size_t i : {1u, 2u, 4u, 5u, 7u, 8u}) {
    if (!is_digit(text[i])) {
      return false;
    }
  }
  return true;
}

// Validate `--snapshot-suffix`'s value AND apply it to the spec it governs, in
// one testable call. Returns false (leaving `hive` untouched) exactly when
// `is_valid_snapshot_suffix` rejects the text, so the arg loop's behaviour is
// unchanged: print the usage error and exit 2.
//
// FIX-I-1. The assignment used to live inline in `run_build_cli`'s arg loop,
// where no test could reach it -- deleting `spec.hive.snapshot_suffix =
// std::string(text);` passed the entire C++ and Python suite while silently
// pinning every build to the 19:55Z default, which is wrong for all 83 EST
// sessions of the production hive. The validator was the only tested half of the
// branch. Moving the mutation into this header puts BOTH halves of the flag's
// decision behind one unit-testable seam (surface_db_build_test.cpp), which is
// the regression gate an out-of-repo log audit can never be.
[[nodiscard]] inline bool apply_snapshot_suffix_flag(std::string_view text,
                                                     OpraHiveSpec& hive) {
  if (!is_valid_snapshot_suffix(text)) {
    return false;
  }
  hive.snapshot_suffix = std::string(text);
  return true;
}

// ── --symbols-file: a universe too wide for one argv token ───────────────────
//
// `--symbols` is ONE comma-joined argv token. The 616-name cross-section joins
// to 2,656 chars; the full OPRA census (`data/universe/census_2026-08-21.csv`,
// 6,189 underliers, mean symbol length 3.65) joins to 28,777, and the other
// flags `run_surface_db_backfill.py`'s `build_build_command` emits and the exe
// path bring a representative full-universe invocation to a MEASURED 29,068 of
// the Windows `CreateProcess` 32,767-char command-line ceiling — 3,699 left.
// The same invocation with `--symbols-file` measures 409. So the joined form
// FITS, by less than one universe revision, and that margin (not an overflow)
// is the defect: at the ceiling the spawn fails inside the ORCHESTRATOR with an
// opaque OS error, never as a diagnostic from this tool. The file form's argv
// cost does not scale with the universe at all.
//
// Discovery mode (omit `--symbols`) is NOT the alternative: it reads each date's
// Parquet table in a serial pre-pass and RETAINS it for the panel pass
// (`src/marketdata/opra_hive.cpp:132-134` moves every `tbl` into `di.table`), so
// a wide window sits resident. That is why an empty symbols file must be a hard
// error rather than an empty list — `spec.symbols.empty()` IS the discovery
// switch (`opra_hive.cpp:129`), so a file that parsed to nothing would silently
// alias the one mode this flag exists to avoid, on the widest possible universe.

// Why one flag-level parse FAILED. Ordered so the arg loop can `switch` it and
// so `symbols_file_diagnostic` can say something specific about each.
enum class SymbolsFlagError : std::uint8_t {
  None = 0,      // parsed; the universe has been applied to the spec
  BothFlags,     // --symbols and --symbols-file are mutually exclusive
  Unreadable,    // the path could not be opened, or the read failed part-way
  Empty,         // no symbols survived blank/comment stripping
  SymbolTooLong, // a symbol exceeds the storage layer's kSurfaceDbKeyMax
  InteriorWhitespace, // a line holds more than one whitespace-separated field
};

// The outcome of one `--symbols-file` value. `line`/`offender` are populated
// only for `SymbolTooLong`, whose diagnostic has to name the offending entry to
// be actionable in a 6,000-line file.
struct SymbolsFileOutcome {
  SymbolsFlagError error{SymbolsFlagError::None};
  std::size_t line{0};   // 1-based PHYSICAL line (blanks and comments counted)
  std::string offender;  // the rejected symbol, verbatim after trimming

  [[nodiscard]] explicit operator bool() const noexcept {
    return error == SymbolsFlagError::None;
  }
};

// Validate `--symbols-file`'s value AND apply the universe it names to the spec
// it governs, in one testable call — the same both-halves-in-one-seam rule
// FIX-I-1 imposed on `apply_snapshot_suffix_flag` above, and for the same
// reason: a validator alone lets the ASSIGNMENT be deleted with the suite still
// green.
//
// Format: one symbol per line. A line whose first non-whitespace character is
// `#` is a comment; a blank (or all-whitespace) line is skipped; leading and
// trailing whitespace — `\r` included, so a CRLF file parses on either platform
// — is trimmed off each symbol. FILE ORDER IS PRESERVED: `load_opra_hive` loads
// an explicit list "in the given order" and lays the entry grid out date-major
// × that order (`effective = spec.symbols`, opra_hive.cpp:185), so sorting here
// would silently reorder every build's report. There are no inline comments: the
// whole trimmed line is the symbol, so `AAA # note` would be a (nonexistent)
// symbol rather than `AAA` — which is exactly why a line carrying INTERIOR
// whitespace is refused outright (see `InteriorWhitespace` below). Taking the
// whole line is only a safe rule when a multi-field line cannot be mistaken for
// a symbol; a columnar fixture must have its symbol column cut out first.
//
// `symbols_flag_seen` is the caller's record that `--symbols` also appeared.
// Taking both would make the universe depend on argv ORDER, so it is refused
// here rather than resolved — and refused BEFORE the open, so a conflicting run
// touches neither the disk nor `hive`.
//
// A symbol longer than `kSurfaceDbKeyMax` is REJECTED, not truncated. The
// storage layer truncates silently (`canonicalize_symbol`, ASCII upper-case +
// truncate to the cap — length-preserving, so this check on the raw token is
// exact), and a truncated symbol is a different symbol that matches nothing in
// the hive: every cell for it becomes a coverage hole under a name the operator
// never typed. `--symbols` keeps its existing truncating behaviour; this is new
// code declining to reproduce the trap, not a change to that flag.
//
// STRONG GUARANTEE: on any error `hive` is exactly as it was. Symbols are
// accumulated locally and moved in only once the whole file has parsed.
[[nodiscard]] inline SymbolsFileOutcome apply_symbols_file_flag(std::string_view path,
                                                                bool symbols_flag_seen,
                                                                OpraHiveSpec& hive) {
  SymbolsFileOutcome outcome;
  if (symbols_flag_seen) {
    outcome.error = SymbolsFlagError::BothFlags;
    return outcome;
  }

  std::ifstream in(std::string(path), std::ios::binary);
  if (!in) {
    outcome.error = SymbolsFlagError::Unreadable;
    return outcome;
  }

  const auto is_space = [](char c) noexcept {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\v' || c == '\f';
  };

  std::vector<std::string> parsed;
  std::string line;
  std::size_t lineno = 0;
  while (std::getline(in, line)) {
    ++lineno;
    std::string_view field{line};
    while (!field.empty() && is_space(field.front())) {
      field.remove_prefix(1);
    }
    while (!field.empty() && is_space(field.back())) {
      field.remove_suffix(1);
    }
    if (field.empty() || field.front() == '#') {
      continue;
    }
    // A line with interior whitespace is a COLUMNAR file fed in by mistake, not
    // a symbol. The universe fixtures this tool is pointed at are tab-separated
    // (`atx-vol/data/universe/xsec_2026-08.csv` is `SYM<TAB>weight`), so the
    // realistic operator error is `--symbols-file xsec_2026-08.csv`, which
    // yields symbols like "SPY\t4.1404". Those are short enough to clear the
    // length check and become a coverage hole for EVERY cell under a name the
    // operator never typed — the same silent-mismatch trap SymbolTooLong exists
    // to prevent, arriving by a different road. Refuse it here instead: this
    // reader takes the whole trimmed line as the symbol by design (there are no
    // inline comments), and that design is only safe if a multi-field line is
    // an error rather than a symbol containing a tab.
    if (field.find_first_of(" \t\v\f") != std::string_view::npos) {
      outcome.error = SymbolsFlagError::InteriorWhitespace;
      outcome.line = lineno;
      outcome.offender.assign(field);
      return outcome;
    }
    if (field.size() > kSurfaceDbKeyMax) {
      outcome.error = SymbolsFlagError::SymbolTooLong;
      outcome.line = lineno;
      outcome.offender.assign(field);
      return outcome;
    }
    parsed.emplace_back(field);
  }
  // `getline` sets failbit at a clean EOF, so only badbit distinguishes a real
  // read failure part-way through the file from the normal end of one.
  if (in.bad()) {
    outcome.error = SymbolsFlagError::Unreadable;
    return outcome;
  }
  if (parsed.empty()) {
    outcome.error = SymbolsFlagError::Empty;
    return outcome;
  }

  hive.symbols = std::move(parsed);
  return outcome;
}

// The one-line operator diagnostic for a failed `--symbols-file`, without the
// `atx-vol-surface-db-build: ` prefix the caller adds (matching how every other
// message in the arg loop is assembled). Returned as a string rather than
// printed so the WORDING is unit-testable — the path, the line number and the
// offending symbol are the only things that make a 6,000-line file debuggable,
// and a diagnostic that silently stopped naming them would otherwise pass.
[[nodiscard]] inline std::string symbols_file_diagnostic(std::string_view path,
                                                          const SymbolsFileOutcome& out) {
  const std::string quoted = "'" + std::string(path) + "'";
  switch (out.error) {
  case SymbolsFlagError::None:
    return {};
  case SymbolsFlagError::BothFlags:
    return "--symbols and --symbols-file are mutually exclusive; pass exactly one";
  case SymbolsFlagError::Unreadable:
    return "--symbols-file " + quoted + ": cannot open for reading";
  case SymbolsFlagError::Empty:
    return "--symbols-file " + quoted +
           ": no symbols (every line is blank or a '#' comment). An empty universe is NOT "
           "discovery mode -- omit --symbols-file entirely to discover every underlying";
  case SymbolsFlagError::SymbolTooLong:
    return "--symbols-file " + quoted + " line " + std::to_string(out.line) + ": symbol '" +
           out.offender + "' is " + std::to_string(out.offender.size()) + " chars, over the " +
           std::to_string(kSurfaceDbKeyMax) +
           "-char storage limit (DbSymbolRecord::symbol); it would be silently TRUNCATED into a "
           "different symbol";
  case SymbolsFlagError::InteriorWhitespace:
    return "--symbols-file " + quoted + " line " + std::to_string(out.line) + ": '" +
           out.offender +
           "' has interior whitespace, so it is a columnar row, not a symbol -- this file takes "
           "the WHOLE trimmed line as one symbol. A tab/comma universe fixture (e.g. "
           "data/universe/xsec_2026-08.csv, 'SYM<TAB>weight') must have its symbol column cut "
           "out first, or every cell becomes a coverage hole under a name you never typed";
  }
  return {}; // unreachable: every enumerator returns above
}

// ── --spots (ATX_CORPUS_SPOTS overlay) ──────────────────────────────────────
//
// Same seam discipline as --symbols-file above: VALIDATE AND APPLY in one
// function so the ASSIGNMENT onto the hive spec is unit-testable, not just the
// parse. The flag exists because `atx-vol-surface-db-build` otherwise implies
// every board's spot from put-call parity on that board's own quotes, and a
// board with no strike carrying a two-sided call AND a two-sided put on any
// expiry cannot imply one at all -- it refuses to LOAD, landing in neither
// cells_ok nor cells_failed, named by no failed_cell line, visible only as
// n_load_errors (943 of 6,189 underliers on the 2026-08-21 full-OPRA board).
//
// STRICTNESS, AND WHY IT IS NOT MERELY TASTE. Every failure below is a usage
// error rather than a warning-and-continue, because the fallback for a spot
// that did not arrive is not "no spot" -- it is the PCP-implied spot, which is
// a DIFFERENT and silently plausible number. A run that lost its overlay
// therefore looks completely clean while pricing an unknown fraction of the
// board off the wrong underlier. That is the failure mode this refuses to have.
// An overlay covering only PART of the board is fine and expected
// (MissingMarketInputPolicy::UseFallback leaves uncovered cells on PCP); what
// is rejected is an overlay that was ASKED FOR and did not take effect.
enum class SpotsFlagError : std::uint8_t {
  None = 0,
  EmptyPath,   // --spots with no value, or the empty string
  Unreadable,  // cannot open, or magic/header/rows malformed (reader's words)
  NoRows,      // parses, but overlays nothing -- an overlay that does nothing
};

struct SpotsFlagOutcome {
  SpotsFlagError error{SpotsFlagError::None};
  std::string detail{};      // the reader's own message, for Unreadable
  std::size_t n_cells{0};    // cells overlaid, on success
};

// Read `path` and, on success, MOVE the table onto `hive.market_inputs`.
// `hive` is left untouched on every failure, so a rejected flag cannot
// half-apply.
[[nodiscard]] inline SpotsFlagOutcome apply_spots_flag(std::string_view path, OpraHiveSpec &hive) {
  SpotsFlagOutcome outcome;
  if (path.empty()) {
    outcome.error = SpotsFlagError::EmptyPath;
    return outcome;
  }
  Result<CorpusMarketInputTable> table = read_corpus_spot_inputs(std::string(path));
  if (!table) {
    outcome.error = SpotsFlagError::Unreadable;
    outcome.detail = table.error().to_string();
    return outcome;
  }
  if (table->cells().empty()) {
    outcome.error = SpotsFlagError::NoRows;
    return outcome;
  }
  outcome.n_cells = table->cells().size();
  hive.market_inputs = std::move(*table);
  return outcome;
}

[[nodiscard]] inline std::string spots_flag_diagnostic(std::string_view path,
                                                       const SpotsFlagOutcome &out) {
  const std::string quoted = "'" + std::string(path) + "'";
  switch (out.error) {
  case SpotsFlagError::None:
    return {};
  case SpotsFlagError::EmptyPath:
    return "--spots expects a path. A dropped shell variable is not a choice: without the "
           "overlay every board falls back to a PCP-implied spot, which is a different number "
           "and not a missing one, so the run would look clean";
  case SpotsFlagError::Unreadable:
    return "--spots " + quoted + ": " + out.detail;
  case SpotsFlagError::NoRows:
    return "--spots " + quoted +
           ": parses but carries no spot rows, so the overlay would do nothing while the run "
           "reported success -- omit --spots entirely to imply every spot from put-call parity";
  }
  return {}; // unreachable: every enumerator returns above
}

} // namespace atx::vol
