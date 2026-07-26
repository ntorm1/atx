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
#include <string_view>

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

} // namespace atx::vol
