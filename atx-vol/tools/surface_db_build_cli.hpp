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
#include <string>
#include <string_view>

#include "atx/vol/opra_hive.hpp" // OpraHiveSpec (apply_snapshot_suffix_flag)

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

} // namespace atx::vol
