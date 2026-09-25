#pragma once

// atx::engine::data — FINRA consolidated short-interest loader (Track B1).
//
// The FIRST consumer of the FINRA short-interest parquet partition emitted by
// python/scripts/download_finra_short_interest.py. It reads the hive-partitioned
// parquet (date=YYYY-MM-DD/part-*.parquet) and projects three derived, CAUSALLY
// placed fields onto an externally supplied research-panel axis:
//
//   si_dtc   = days_to_cover_quantity
//   si_util  = current_short_position_quantity / shares_outstanding
//              (fraction of float short; shares come from the panel's own
//               shares/market_cap field, NOT from FINRA). If a per-(date,inst)
//               shares value is absent, falls back to
//               current_short_position_quantity / average_daily_volume_quantity
//               (a days-to-cover-like proxy) and records that it did so.
//   si_chg   = change_percent
//
// CAUSALITY (the central correctness requirement)
// -----------------------------------------------
// FINRA stamps a `settlement_date`, but the data is publicly disseminated only
// after the close on the 7th business day after it. A value is therefore knowable
// on a panel date only on or after its first USABLE day:
//
//     publish_day = finra_first_usable_day(settlement_day, lag)
//                 = the (7 + 1)-th NYSE session after settlement (default rule)
//
// (The legacy CalendarDaysV1 rule, settlement + 10 calendar days, is kept behind
// FinraLagRule for reproduction only; it leaks — finding D-02.)
//
// For each (instrument, observation) the loader places the value on every panel
// date >= publish_day and forward-fills it onto subsequent panel dates until the
// NEXT observation for that instrument becomes visible (its own publish_day). A
// value is NEVER placed on a panel date strictly before its publish_day — this is
// the no-look-ahead guarantee, unit-tested in finra_short_test.cpp.
//
// The function is AXIS-PARAMETRIC: it takes the panel's calendar-date axis and a
// symbol->column map as arguments rather than reconstructing them internally, so
// it is fully unit-testable on synthetic parquet fixtures with no real download
// (the augment stage reconstructs the real axis from the ORATS partition and the
// FINRA symbology and hands it in).
//
// PIMPL parquet only: this header pulls in no Arrow. The .cpp reads via
// atx::core::io::read_parquet, mirroring atx-tsdb/src/load_parquet.cpp.

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "atx/core/error.hpp"
#include "atx/core/types.hpp"

#include "atx/engine/data/dataset_schema.hpp" // DateKey, InstKey

namespace atx::engine::data {

// Canonical appended-field names (referenceable by name in a FeatureSpec / B6).
inline constexpr std::string_view kFinraFieldDtc  = "si_dtc";  // days_to_cover_quantity
inline constexpr std::string_view kFinraFieldUtil = "si_util"; // short / shares (or short / ADV fallback)
inline constexpr std::string_view kFinraFieldChg  = "si_chg";  // change_percent

// LEGACY publication lag in CALENDAR days (FinraLagRule::CalendarDaysV1). Kept so
// old panels reproduce. It is not conservative: settlement + 10 calendar days
// lands 1-2 sessions before the value is public for Thursday/Friday settlements
// (about 40% of observations, finding D-02).
inline constexpr int kFinraDefaultPublicationLagDays = 10;

// =========================================================================
//  Publication lag counted in NYSE sessions (W0-D0, D-02)
// =========================================================================
//
// FINRA disseminates short interest after the market close on the 7th business day
// after the settlement date. The value is therefore first usable on the NEXT
// session: 7 sessions + 1 for the after-close release = the 8th NYSE session after
// settlement. Sessions are NYSE trading days (weekends, NYSE holidays and the
// unscheduled closures listed in finra_short.cpp excluded).
enum class FinraLagRule : std::uint8_t {
  CalendarDaysV1 = 1, // publish_day = settlement + lag calendar days (legacy, leaks)
  NyseSessionsV2 = 2, // first usable day = settlement + lag NYSE sessions (+1 after close)
};

inline constexpr int kFinraPublicationSessions = 7; // FINRA release: 7th business day

struct FinraPublicationLag {
  FinraLagRule rule = FinraLagRule::NyseSessionsV2;
  // Sessions (NyseSessionsV2) or calendar days (CalendarDaysV1). Must be >= 0.
  int lag = kFinraPublicationSessions;
  // NyseSessionsV2 only: the release happens after the close, so the value is
  // usable one session later. Ignored by CalendarDaysV1.
  bool after_close = true;
};

// Supported NYSE calendar range (the holiday rules and closure list cover it).
inline constexpr atx::i64 kNyseCalendarFirstDay = 7305;  // 1990-01-01
inline constexpr atx::i64 kNyseCalendarLastDay = 25932;  // 2040-12-31

// True iff epoch-day `day` is an NYSE session. Err(OutOfRange) outside
// [kNyseCalendarFirstDay, kNyseCalendarLastDay].
[[nodiscard]] atx::core::Result<bool> nyse_is_session(atx::i64 day);

// The `n`-th NYSE session strictly after epoch-day `day` (n >= 1; n == 0 returns
// `day` itself). Err(InvalidArgument) for n < 0; Err(OutOfRange) when `day` or the
// result leaves the supported calendar range.
[[nodiscard]] atx::core::Result<atx::i64> nyse_add_sessions(atx::i64 day, int n);

// First epoch-day on which an observation settled on `settlement_day` may be used:
//   CalendarDaysV1: settlement_day + lag
//   NyseSessionsV2: nyse_add_sessions(settlement_day, lag + (after_close ? 1 : 0))
// Err(InvalidArgument) for a negative lag; Err(OutOfRange) outside the calendar.
[[nodiscard]] atx::core::Result<atx::i64>
finra_first_usable_day(atx::i64 settlement_day, const FinraPublicationLag &lag);

// The three derived feature columns, each date-major (length == panel_dates ×
// instruments) in the SAME axis order the caller supplied. NaN where no causal
// FINRA observation covers the (date, instrument) cell.
struct FinraFeatures {
  atx::usize dates{};        // == panel_dates.size()
  atx::usize instruments{};  // == the column count implied by sym_to_inst's range
  std::vector<atx::f64> si_dtc;
  std::vector<atx::f64> si_util;
  std::vector<atx::f64> si_chg;
  // Diagnostics (not part of the data plane):
  atx::usize rows_read{};         // FINRA rows scanned across all partitions
  atx::usize rows_placed{};       // rows that mapped onto an in-axis (symbol, publish<=date)
  atx::usize symbols_unmatched{}; // distinct FINRA symbols not in sym_to_inst (dropped)
  atx::usize util_from_shares{};  // si_util cells computed from the shares denominator
  atx::usize util_from_adv{};     // si_util cells computed from the ADV fallback
};

// Per-cell shares denominator for si_util, date-major over the SAME axis as the
// output (length == dates × instruments). A NaN / non-positive entry triggers the
// per-cell ADV fallback. Pass an EMPTY span to force the ADV fallback everywhere.
//
// load_finra_features:
//   short_interest_root  — directory holding date=YYYY-MM-DD/part-*.parquet (the
//                          downloader's hive root). Missing/empty -> Err(IoError).
//   panel_dates          — the panel's calendar date axis as EPOCH-DAYS (days
//                          since 1970-01-01), STRICTLY ASCENDING. Each entry is
//                          one panel date row, in panel row order.
//   sym_to_inst          — FINRA `symbol` (ticker) -> panel instrument-column
//                          index. The column count N is 1 + max value (or 0 if
//                          empty); callers pass the panel's instrument count via
//                          `instruments` to pin N exactly.
//   instruments          — the panel's instrument-column count N (pins the output
//                          width; every value in sym_to_inst must be < N).
//   shares               — per-(date,inst) shares-outstanding denominator (see
//                          above); empty -> ADV fallback everywhere.
//   publication_lag_days — causal dissemination lag (calendar days); see header.
//
//   lag                  — publication-lag rule (default: NYSE sessions, 7 + 1 after
//                          close; see FinraPublicationLag).
//
// Errors: Err(IoError) if the root is missing or a partition cannot be read;
// Err(InvalidArgument) if panel_dates is not strictly ascending, if a sym_to_inst
// value is out of range, if `shares` is non-empty but mis-sized, or if the lag is
// negative; Err(OutOfRange) if a matched settlement date lies outside the NYSE
// calendar range (NyseSessionsV2 only).
[[nodiscard]] atx::core::Result<FinraFeatures> load_finra_features(
    const std::string& short_interest_root, std::span<const DateKey> panel_dates,
    const std::unordered_map<std::string, InstKey>& sym_to_inst, atx::usize instruments,
    std::span<const atx::f64> shares, const FinraPublicationLag& lag = FinraPublicationLag{});

// LEGACY overload: `publication_lag_days` calendar days (FinraLagRule::CalendarDaysV1).
// Existing callers that pass an explicit int keep their old placement; new code uses
// the FinraPublicationLag overload above (W0-I0b moves the atx-impl default).
[[nodiscard]] atx::core::Result<FinraFeatures> load_finra_features(
    const std::string& short_interest_root, std::span<const DateKey> panel_dates,
    const std::unordered_map<std::string, InstKey>& sym_to_inst, atx::usize instruments,
    std::span<const atx::f64> shares, int publication_lag_days);

} // namespace atx::engine::data
