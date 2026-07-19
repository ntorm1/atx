#pragma once

// Loader for SpiderRock's `tblstockearnforecasthist` TSV export -- a
// point-in-time snapshot of each ticker's next-8 forward earnings-date
// forecasts. See earnings_forecast_loader.cpp for the confirmed real-file
// schema (59 columns, TAB-delimited, CRLF, header row first, parsed by
// HEADER NAME not fixed column index) and the SpiderRock "no forward date"
// sentinel this loader drops rather than parses as a real event.
//
// This module has exactly one entry point: `load_earnings_events` extracts
// one ticker's `nextEarnDate1..8` UTC instants as sorted epoch-ns, ready to
// feed `EventSchedule` (event_vol.hpp) for earnings-event counting.
//
// Thread-safety: `load_earnings_events` is a pure function of its arguments
// (opens/reads the given file, touches no shared/global state) -- safe to
// call concurrently from multiple threads, including on the same path.

#include <cstdint>
#include <string_view>
#include <vector>

#include "atx/vol/types.hpp" // Result, ErrorCode

namespace atx::vol {

// Parses `forecast_tsv_path` (a tblstockearnforecasthist export) and returns
// `ticker`'s forward earnings instants -- its nextEarnDate1..8 UTC datetime
// columns -- converted to epoch-ns and sorted ascending. A ticker
// legitimately has fewer than 8 scheduled forward dates (SpiderRock fills
// unused slots with a "no forecast" sentinel, dropped here): Ok with a short
// (even empty) vector is a normal, successful result, not an error.
//
// @param forecast_tsv_path path to the TAB-delimited, CRLF, header-first
//                           tblstockearnforecasthist export
// @param ticker             ticker_tk to look up (case-sensitive exact
//                           match; the FIRST matching row wins if the file
//                           somehow carries duplicates -- SpiderRock's real
//                           export is one row per ticker for a single as-of
//                           date)
// @return  Ok(events): sorted ascending, epoch-ns, empty/sentinel slots
//          dropped (never included as a spurious epoch-0/epoch-1 event).
//          Err(NotFound) if `ticker` does not appear in the file (the file
//          itself opened and its header parsed fine).
//          Err(InvalidArgument) if `ticker` is empty, the header is missing
//          the `ticker_tk` column or any `nextEarnDate1..8` column, or the
//          MATCHED ticker's row is short (ragged TSV) or carries a
//          non-empty, non-sentinel, un-parseable date cell.
//          Err(IoError) if `forecast_tsv_path` can't be opened/read.
[[nodiscard]] Result<std::vector<std::int64_t>> load_earnings_events(
    std::string_view forecast_tsv_path, std::string_view ticker);

} // namespace atx::vol
