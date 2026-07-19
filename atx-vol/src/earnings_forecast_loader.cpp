#include "atx/vol/earnings_forecast_loader.hpp"

// ---------------------------------------------------------------------------
// Real-file schema (confirmed on a live 2026-02-10 SpiderRock export,
// `tblstockearnforecasthist_v2.00_2026-02-10`: TAB-delimited, CRLF, 59
// columns, header row first, 6507 data rows):
//
//   ticker_at  ticker_ts  ticker_tk  tradingDate
//   nearEarnDate  nearEarnDate_us  nearEarnType  nearEarnTime
//   nextEarnDate1Adj  nextEarnDate1Adj_us  nextEarnDate2Adj  nextEarnDate2Adj_us
//   { nextEarnDateN  nextEarnDateN_us  nextEarnTypeN  nextEarnTimeN } for N=1..8
//   timestamp  timestamp_us
//   { ..._cst mirrors of the UTC datetime columns above }
//   securityID
//
// We read by HEADER NAME (robust to column reordering/insertion), needing
// only `ticker_tk` and `nextEarnDate1..8`. The main datetime columns are UTC,
// format "YYYY-MM-DD HH:MM:SS.ffffff" (fractional seconds observed as always
// 6 digits/microseconds in the real export; parsed generically as 1-9 digits
// per the task contract).
//
// CONFIRMED SENTINEL (real export, NOT a spec guess): a ticker with fewer
// than 8 forward dates does NOT leave the unused `nextEarnDateN` cells blank
// -- SpiderRock fills them with the literal placeholder
// "1970-01-01 00:00:01.000000" (nextEarnTypeN/nextEarnTimeN read "None"/
// "None" in lockstep). This loader drops any cell whose parsed YEAR is
// <= 1970 rather than matching that exact string: that one rule uniformly
// absorbs the real sentinel, a genuinely blank cell, and an all-zeros/epoch
// variant -- none of which is a real scheduled earnings date (SpiderRock's
// forecast history starts in 2012; nothing legitimate parses to 1970 or
// earlier). Per the task brief: never emit a dropped slot as epoch 0/1.
// ---------------------------------------------------------------------------

#include <algorithm>
#include <array>
#include <charconv>
#include <cstdint>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

#include "atx/core/datetime.hpp" // is_valid_date, timestamp_from_utc (public Hinnant civil-date math)

namespace atx::vol {

using atx::core::Err;
using atx::core::Ok;

namespace {

constexpr std::size_t kForwardDateCount = 8;

// Header-resolved column indices this loader needs.
struct ColumnIndex {
  std::size_t ticker_tk{};
  std::array<std::size_t, kForwardDateCount> next_earn_date{};
};

// Split `line` on '\t' into views over `line` (no allocation of the pieces).
// Bounded by line.size(): the loop runs exactly line.size()+1 iterations.
[[nodiscard]] std::vector<std::string_view> split_tab(std::string_view line) {
  std::vector<std::string_view> out;
  std::size_t start = 0;
  for (std::size_t i = 0; i <= line.size(); ++i) {
    if (i == line.size() || line[i] == '\t') {
      out.push_back(line.substr(start, i - start));
      start = i + 1;
    }
  }
  return out;
}

// Drops a trailing '\r' (CRLF line endings) if present.
[[nodiscard]] std::string_view rstrip_cr(std::string_view v) noexcept {
  if (!v.empty() && v.back() == '\r') {
    v.remove_suffix(1);
  }
  return v;
}

// Resolves `ticker_tk` and `nextEarnDate1..8` to column indices from the
// header line. Reading by NAME (not fixed position) is the whole point --
// robust to the real export's column layout drifting under us.
[[nodiscard]] Result<ColumnIndex> resolve_columns(std::string_view header_line) {
  const std::vector<std::string_view> cols = split_tab(header_line);
  std::unordered_map<std::string_view, std::size_t> by_name;
  by_name.reserve(cols.size());
  for (std::size_t i = 0; i < cols.size(); ++i) {
    by_name.emplace(cols[i], i);
  }

  static constexpr std::array<std::string_view, kForwardDateCount> kDateColumnNames{
      "nextEarnDate1", "nextEarnDate2", "nextEarnDate3", "nextEarnDate4",
      "nextEarnDate5", "nextEarnDate6", "nextEarnDate7", "nextEarnDate8"};

  ColumnIndex idx{};
  const auto ticker_it = by_name.find("ticker_tk");
  if (ticker_it == by_name.end()) {
    return Err(ErrorCode::InvalidArgument,
               "load_earnings_events: header is missing column 'ticker_tk'");
  }
  idx.ticker_tk = ticker_it->second;

  for (std::size_t i = 0; i < kDateColumnNames.size(); ++i) {
    const auto it = by_name.find(kDateColumnNames[i]);
    if (it == by_name.end()) {
      return Err(ErrorCode::InvalidArgument,
                 "load_earnings_events: header is missing column '" +
                     std::string{kDateColumnNames[i]} + "'");
    }
    idx.next_earn_date[i] = it->second;
  }
  return Ok(std::move(idx));
}

// Parses one "YYYY-MM-DD HH:MM:SS[.ffffff]" UTC datetime cell to epoch-ns.
//   Ok(nullopt)  -- an empty cell, or the SpiderRock 1970 sentinel/placeholder
//                   (see the module comment) -- i.e. "no forward date here".
//   Ok(ns)       -- a real, parsed UTC instant.
//   Err(InvalidArgument) -- non-empty, non-sentinel, and not a well-formed
//                   UTC civil datetime.
[[nodiscard]] Result<std::optional<std::int64_t>> parse_earn_date_cell(std::string_view cell) {
  if (cell.empty()) {
    return Ok(std::optional<std::int64_t>{});
  }

  // Strict "YYYY-MM-DD HH:MM:SS" (19 bytes) + optional ".fraction" (1-9 digits).
  if (cell.size() < 19 || cell[4] != '-' || cell[7] != '-' || cell[10] != ' ' ||
      cell[13] != ':' || cell[16] != ':') {
    return Err(ErrorCode::InvalidArgument,
               "load_earnings_events: unparseable UTC datetime cell '" + std::string{cell} + "'");
  }

  const auto parse_field = [&](std::size_t off, std::size_t len, std::uint32_t &out) noexcept {
    std::uint32_t v = 0;
    const auto r = std::from_chars(cell.data() + off, cell.data() + off + len, v);
    if (r.ec != std::errc{} || r.ptr != cell.data() + off + len) {
      return false;
    }
    out = v;
    return true;
  };

  std::uint32_t year_u = 0, month = 0, day = 0, hour = 0, minute = 0, second = 0;
  const bool numeric_ok = parse_field(0, 4, year_u) && parse_field(5, 2, month) &&
                          parse_field(8, 2, day) && parse_field(11, 2, hour) &&
                          parse_field(14, 2, minute) && parse_field(17, 2, second);
  if (!numeric_ok || hour > 23 || minute > 59 || second > 59) {
    return Err(ErrorCode::InvalidArgument,
               "load_earnings_events: unparseable UTC datetime cell '" + std::string{cell} + "'");
  }

  std::uint32_t nano = 0;
  if (cell.size() > 19) {
    if (cell[19] != '.') {
      return Err(ErrorCode::InvalidArgument,
                 "load_earnings_events: unparseable UTC datetime cell '" + std::string{cell} + "'");
    }
    const std::string_view frac = cell.substr(20);
    if (frac.empty() || frac.size() > 9) {
      return Err(ErrorCode::InvalidArgument,
                 "load_earnings_events: unparseable UTC datetime cell '" + std::string{cell} + "'");
    }
    std::uint32_t frac_val = 0;
    const auto r = std::from_chars(frac.data(), frac.data() + frac.size(), frac_val);
    if (r.ec != std::errc{} || r.ptr != frac.data() + frac.size()) {
      return Err(ErrorCode::InvalidArgument,
                 "load_earnings_events: unparseable UTC datetime cell '" + std::string{cell} + "'");
    }
    // Right-pad the parsed fractional digits out to nanosecond precision
    // (e.g. 6 digits of microseconds "123456" -> *1000 = nanoseconds).
    std::uint32_t scale = 1;
    for (std::size_t i = frac.size(); i < 9; ++i) {
      scale *= 10;
    }
    nano = frac_val * scale;
  }

  const auto year = static_cast<std::int32_t>(year_u);
  if (!atx::core::time::is_valid_date(year, month, day)) {
    return Err(ErrorCode::InvalidArgument,
               "load_earnings_events: unparseable UTC datetime cell '" + std::string{cell} + "'");
  }

  // SpiderRock's "no forward date scheduled" placeholder -- see module comment.
  if (year <= 1970) {
    return Ok(std::optional<std::int64_t>{});
  }

  const std::int64_t ns =
      atx::core::time::timestamp_from_utc(year, month, day, hour, minute, second, nano)
          .unix_nanos();
  return Ok(std::optional<std::int64_t>{ns});
}

// Parses the 8 `nextEarnDateN` cells of the row that matched `ticker`, drops
// empty/sentinel slots, and sorts the survivors ascending.
[[nodiscard]] Result<std::vector<std::int64_t>>
parse_matched_row(const std::vector<std::string_view> &fields,
                  const std::array<std::size_t, kForwardDateCount> &date_idx) {
  const std::size_t max_idx = *std::max_element(date_idx.begin(), date_idx.end());
  if (fields.size() <= max_idx) {
    return Err(ErrorCode::InvalidArgument,
               "load_earnings_events: matched ticker's row is short (ragged TSV row)");
  }

  std::vector<std::int64_t> events;
  events.reserve(kForwardDateCount);
  for (const std::size_t col : date_idx) {
    ATX_TRY(const auto maybe_ns, parse_earn_date_cell(fields[col]));
    if (maybe_ns.has_value()) {
      events.push_back(*maybe_ns);
    }
  }
  std::sort(events.begin(), events.end());
  return Ok(std::move(events));
}

} // namespace

Result<std::vector<std::int64_t>> load_earnings_events(std::string_view forecast_tsv_path,
                                                        std::string_view ticker) {
  if (ticker.empty()) {
    return Err(ErrorCode::InvalidArgument, "load_earnings_events: ticker must not be empty");
  }

  std::ifstream in{std::string{forecast_tsv_path}, std::ios::binary};
  if (!in) {
    return Err(ErrorCode::IoError,
               "load_earnings_events: cannot open '" + std::string{forecast_tsv_path} + "'");
  }
  std::stringstream ss;
  ss << in.rdbuf();
  const std::string content = ss.str();
  const std::string_view whole{content};

  const std::size_t header_end = whole.find('\n');
  const std::string_view header_line =
      rstrip_cr(whole.substr(0, header_end == std::string_view::npos ? whole.size() : header_end));
  ATX_TRY(const ColumnIndex idx, resolve_columns(header_line));

  std::size_t pos = (header_end == std::string_view::npos) ? whole.size() + 1 : header_end + 1;

  // Bounded by whole.size(): each iteration advances `pos` past at least the
  // newline it just found (or the loop terminates), so this runs at most
  // whole.size()+1 times.
  while (pos <= whole.size()) {
    const std::size_t nl = whole.find('\n', pos);
    const std::string_view raw = rstrip_cr(
        whole.substr(pos, nl == std::string_view::npos ? std::string_view::npos : nl - pos));
    pos = (nl == std::string_view::npos) ? whole.size() + 1 : nl + 1;
    if (raw.empty()) {
      if (nl == std::string_view::npos) {
        break;
      }
      continue;
    }

    const std::vector<std::string_view> fields = split_tab(raw);
    if (fields.size() <= idx.ticker_tk || fields[idx.ticker_tk] != ticker) {
      continue; // unrelated (or too-ragged-to-tell) row -- skip
    }

    // Matched by ticker: from here a malformed row IS this call's problem.
    return parse_matched_row(fields, idx.next_earn_date);
  }

  return Err(ErrorCode::NotFound, "load_earnings_events: ticker not found: " + std::string{ticker});
}

} // namespace atx::vol
