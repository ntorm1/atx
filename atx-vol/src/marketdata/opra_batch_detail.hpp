#pragma once

// Shared internals of the OPRA batch/hive loaders — the small kernel both
// `load_opra_daterange` (src/opra_batch.cpp) and `load_opra_hive`
// (src/opra_hive.cpp) build on. Lifted out of opra_batch.cpp so the second
// loader REUSES this logic verbatim instead of copy-pasting it (the two loaders
// must resolve dates, snapshot stamps, and per-cell market inputs identically —
// a divergence would make the v1 tree and v2 hive disagree cell-for-cell).
//
// This is a PRIVATE src-level header (not shipped in include/): it pulls in the
// full opra_batch.hpp / opra_panel.hpp aggregates because `resolve_market_inputs`
// operates on `OpraBatchEntry` / `OpraLoadSpec` / `CorpusMarketInputTable`.
//
// Contents:
//   * Civil-date kernel (Howard-Hinnant days-from-civil) — parse "YYYY-MM-DD",
//     walk an inclusive range as a contiguous integer interval, format back.
//   * `memo_iso_to_ns` — the date+suffix -> epoch-ns snapshot-stamp memoization
//     (the M distinct stamps of a range are each parsed once via `iso_to_ns`).
//   * `resolve_market_inputs` — the per-(date, symbol) point-in-time market-input
//     resolution + fallback/quarantine/error policy, filling the shared entry
//     provenance fields and the market-derived fields of an `OpraLoadSpec`.
//
// Thread-safety: every function here is pure over its arguments (no shared
// mutable state); `memo_iso_to_ns` mutates only the caller-owned cache it is
// handed. Intended for use from the SERIAL pre-pass of each loader, before any
// parallel fan-out.

#include <charconv>
#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "atx/core/error.hpp"     // Err, ErrorCode
#include "atx/vol/api/marketdata/data.hpp"       // iso_to_ns
#include "atx/vol/api/marketdata/opra_batch.hpp" // OpraBatchEntry, CorpusMarketInputTable, MissingMarketInputPolicy
#include "atx/vol/api/marketdata/opra_panel.hpp" // OpraLoadSpec

namespace atx::vol::opra_detail {

// ── Civil-date kernel (Howard-Hinnant days-from-civil) ──────────────────────
//
// A serial day count keyed at 1970-01-01 = 0, so an inclusive date range becomes
// a contiguous integer interval walked one day at a time. Self-contained on
// purpose: no external date library (chrono's year_month_day round-trip would do,
// but the raw algorithm is a dozen lines and keeps the dependency surface flat).

struct Civil {
  int y = 0;
  unsigned m = 0;
  unsigned d = 0;
};

// Serial day number for a civil date (Howard Hinnant, "chrono-Compatible
// Low-Level Date Algorithms"). Valid for the Gregorian calendar; m in [1,12],
// d in [1,31].
[[nodiscard]] inline std::int64_t days_from_civil(int y, unsigned m, unsigned d) noexcept {
  y -= (m <= 2);
  const std::int64_t era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = static_cast<unsigned>(y - era * 400);                  // [0, 399]
  const unsigned doy = (153u * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1;      // [0, 365]
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;                 // [0, 146096]
  return era * 146097 + static_cast<std::int64_t>(doe) - 719468;
}

// Inverse of days_from_civil.
[[nodiscard]] inline Civil civil_from_days(std::int64_t z) noexcept {
  z += 719468;
  const std::int64_t era = (z >= 0 ? z : z - 146096) / 146097;
  const unsigned doe = static_cast<unsigned>(z - era * 146097);               // [0, 146096]
  const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365; // [0, 399]
  const int y = static_cast<int>(yoe) + static_cast<int>(era) * 400;
  const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);               // [0, 365]
  const unsigned mp = (5 * doy + 2) / 153;                                    // [0, 11]
  const unsigned d = doy - (153 * mp + 2) / 5 + 1;                            // [1, 31]
  const unsigned m = mp < 10 ? mp + 3 : mp - 9;                               // [1, 12]
  return Civil{y + static_cast<int>(m <= 2), m, d};
}

// Parse a full-width unsigned decimal field, requiring the whole span be digits.
//
// REV-R3 (R1-c residual): the digit requirement is ENFORCED here, not merely
// documented. `std::from_chars` parses into a SIGNED `int` and therefore accepts
// a leading '-', which let `"-123-01-01"` through `parse_civil` end to end: it is
// 10 bytes, `s[4]` and `s[7]` are dashes, the year field parses to `-123`, the
// month/day are in range, and — decisively — a negative year ROUND-TRIPS through
// `days_from_civil`/`civil_from_days` (the Hinnant algorithm is defined for
// them), so R1-c's round-trip check cannot see it either. A civil-date field is
// unsigned by construction, so reject any non-digit byte before `from_chars`
// gets a say. This also hardens the month/day fields, where a '-' was caught only
// incidentally by the `m < 1 || d < 1` range test one frame up.
[[nodiscard]] inline bool parse_uint(std::string_view s, int& out) noexcept {
  for (const char c : s) {
    if (c < '0' || c > '9') {
      return false;
    }
  }
  const char* first = s.data();
  const char* last = s.data() + s.size();
  const std::from_chars_result res = std::from_chars(first, last, out);
  return res.ec == std::errc{} && res.ptr == last;
}

// Parse exactly "YYYY-MM-DD". Rejects wrong length, missing dashes, non-numeric
// fields, an out-of-range month/day, and — R1-c (review C-07) — a day that does
// not EXIST in that month of that year.
//
// The range check alone accepted `2026-02-31`. `days_from_civil` then silently
// NORMALIZED it (its `doy` arithmetic simply runs off the end of February into
// March), so a range walk enumerated `2026-03-03` onwards and the build proceeded
// against a DIFFERENT date than the operator typed — with nothing anywhere saying
// so. An impossible date is an operator error and must be refused at the gate,
// where both loaders already return `Err(InvalidArgument, "unparseable date_lo
// ...")`.
//
// The check is a ROUND TRIP rather than a leap-aware days-in-month table: it has
// fewer moving parts, both halves are right here in this header, and — decisively
// — it validates against the EXACT function whose normalisation is the defect,
// so it cannot drift from it the way a hand-written February rule could. A date
// survives iff `days_from_civil` maps it to a serial day that maps back to the
// same (y, m, d); a normalised date lands on some other civil date by
// construction and fails.
//
// THE RANGE CHECK STAYS, and is not merely an early-out: `days_from_civil` is
// documented valid only for m in [1,12] and d in [1,31], so it must not be handed
// `m = 99` at all. Ordering here is load-bearing.
//
// NEGATIVE YEARS are refused by `parse_uint`, not here — see its comment. The
// round trip provably cannot catch them (a negative year is a perfectly valid
// Gregorian date to the Hinnant algorithm and round-trips exactly), so the
// rejection has to happen at the field parse.
[[nodiscard]] inline bool parse_civil(std::string_view s, Civil& out) noexcept {
  if (s.size() != 10 || s[4] != '-' || s[7] != '-') {
    return false;
  }
  int y = 0;
  int m = 0;
  int d = 0;
  if (!parse_uint(s.substr(0, 4), y) || !parse_uint(s.substr(5, 2), m) ||
      !parse_uint(s.substr(8, 2), d)) {
    return false;
  }
  if (m < 1 || m > 12 || d < 1 || d > 31) {
    return false;
  }
  const Civil c{y, static_cast<unsigned>(m), static_cast<unsigned>(d)};
  const Civil round_trip = civil_from_days(days_from_civil(c.y, c.m, c.d));
  if (round_trip.y != c.y || round_trip.m != c.m || round_trip.d != c.d) {
    return false;
  }
  out = c;
  return true;
}

// Format a civil date as "YYYY-MM-DD".
[[nodiscard]] inline std::string format_civil(const Civil& c) {
  char buf[11];
  std::snprintf(buf, sizeof(buf), "%04d-%02u-%02u", c.y, c.m, c.d);
  return std::string(buf);
}

// ── Snapshot-stamp memoization ──────────────────────────────────────────────
//
// The symbols of one date share a single snapshot stamp `date + snapshot_suffix`,
// so the M distinct stamps of a range are parsed once (iso_to_ns) rather than
// N*M times. The cache is owned by the caller (one per loader invocation).
[[nodiscard]] inline std::int64_t
memo_iso_to_ns(std::unordered_map<std::string, std::int64_t>& cache, const std::string& iso) {
  if (const auto it = cache.find(iso); it != cache.end()) {
    return it->second;
  }
  const std::int64_t ns = iso_to_ns(iso);
  cache.emplace(iso, ns);
  return ns;
}

// ── Per-(date, symbol) market-input resolution ──────────────────────────────

enum class MarketResolveKind : std::uint8_t {
  Resolved,   // load spec fully built; queue the read
  Quarantine, // entry finalized in place (Err), no read
  Fatal,      // missing input under Error policy -> caller returns top-level Err
};

struct MarketResolve {
  MarketResolveKind kind{MarketResolveKind::Resolved};
  std::string message{}; // populated for Quarantine / Fatal
};

// Resolve the point-in-time market inputs for one (date, symbol) cell against
// `market_inputs` under `policy`, filling the shared provenance fields on `entry`
// and — on Resolved — the market-derived fields of `load` (spot_override, yc
// pillars with the spec fallback, cash_divs, fit_context, market provenance).
//
// The caller must have already set the non-market fields it owns:
//   entry.symbol/.date/.path/.snapshot_ts_ns
//   load.path/.underlying/.snapshot_iso/.r/.provenance_mode
//
// Returns:
//   Resolved   — `load` is complete; queue it for the parquet read.
//   Quarantine — no cell + Quarantine policy: `entry.panel` is set to
//                Err(Unavailable) here; the caller finalizes the entry and
//                queues NO read.
//   Fatal      — no cell + Error policy: the caller must return
//                Err(ErrorCode::Unavailable, message) as the batch's ONLY
//                top-level failure.
//
// Behaviourally identical to the inline resolution `load_opra_daterange` used
// before this was lifted.
[[nodiscard]] inline MarketResolve
resolve_market_inputs(const CorpusMarketInputTable& market_inputs, MissingMarketInputPolicy policy,
                      const std::string& date, const std::string& symbol,
                      const std::vector<double>& fallback_pillar_t,
                      const std::vector<double>& fallback_pillar_r, OpraBatchEntry& entry,
                      OpraLoadSpec& load) {
  const CorpusMarketInputCell* market = market_inputs.find(date, symbol);
  if (market == nullptr) {
    entry.used_market_input_fallback = true;
    if (policy == MissingMarketInputPolicy::Error) {
      return MarketResolve{MarketResolveKind::Fatal,
                           "missing market inputs for " + date + " " + symbol};
    }
    if (policy == MissingMarketInputPolicy::Quarantine) {
      entry.panel = atx::core::Err(atx::core::ErrorCode::Unavailable,
                                   "missing market inputs for " + date + " " + symbol);
      return MarketResolve{MarketResolveKind::Quarantine, {}};
    }
    // UseFallback: the spec's fallback term pillars stand in for the cell.
    load.yc_pillar_t = fallback_pillar_t;
    load.yc_pillar_r = fallback_pillar_r;
    return MarketResolve{MarketResolveKind::Resolved, {}};
  }

  entry.market_input_fingerprint = market->provenance.fingerprint;
  load.spot_override = market->spot_override.value_or(0.0);
  load.yc_pillar_t = market->yc_pillar_t.empty() ? fallback_pillar_t : market->yc_pillar_t;
  load.yc_pillar_r = market->yc_pillar_r.empty() ? fallback_pillar_r : market->yc_pillar_r;
  load.cash_divs = market->cash_divs;
  load.fit_context = market->fit_context;
  load.market_input_provenance = market->provenance;
  return MarketResolve{MarketResolveKind::Resolved, {}};
}

} // namespace atx::vol::opra_detail
