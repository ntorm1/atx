#include "atx/vol/listed_opra.hpp"

#include <algorithm>
#include <bit>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#include "atx/core/datetime.hpp"
#include "atx/core/error.hpp"
#include "atx/core/hash.hpp"
#include "atx/vol/data.hpp"

namespace atx::vol {
namespace {

using atx::core::Err;
using atx::core::ErrorCode;
using atx::core::Ok;

constexpr std::string_view kMagic = "ATX_LISTED_DEFINITIONS\t1";
constexpr std::string_view kHeader =
    "trade_date\tinstrument_id\traw_symbol\tdefinition_ts_ns\texpiry_ts_ns\t"
    "multiplier\tstandard_monthly\tstandard_deliverable\tsource_fingerprint";

[[nodiscard]] auto definition_key(const ListedContractDefinition &definition) {
  return std::tie(definition.trade_date, definition.instrument_id, definition.raw_symbol);
}

void append_u64(std::string &out, std::uint64_t value) {
  char buffer[32];
  const auto [end, error] = std::to_chars(buffer, buffer + sizeof buffer, value);
  (void)error;
  out.append(buffer, end);
}

void append_i64(std::string &out, std::int64_t value) {
  char buffer[32];
  const auto [end, error] = std::to_chars(buffer, buffer + sizeof buffer, value);
  (void)error;
  out.append(buffer, end);
}

void append_double(std::string &out, double value) {
  char buffer[64];
  const auto [end, error] =
      std::to_chars(buffer, buffer + sizeof buffer, value, std::chars_format::general,
                    std::numeric_limits<double>::max_digits10);
  (void)error;
  out.append(buffer, end);
}

[[nodiscard]] std::uint64_t fingerprint_text(std::string_view text) {
  const std::uint64_t hash = atx::core::hash_bytes(text.data(), text.size());
  return hash == 0u ? 1u : hash;
}

// Epoch-ns of the LAST nanosecond of `trade_date`, formatted into a stack
// buffer. Replaces a per-row `trade_date + "T23:59:59.999999999Z"` heap
// concatenation: the stamp is 30 characters, past MSVC's 15-char small-string
// capacity, so every row used to allocate and free once.
//
// Exactly equivalent to the concatenation it replaces, INCLUDING for a
// trade_date too long to fit. `iso_to_ns` rejects any stamp whose 11th
// character is not 'T' or ' ' (data.cpp `parse_iso_ns`), so an over-long date
// yielded 0 before and yields 0 here; both land on the same `trade_end <= 0`
// rejection. The buffer holds a 12-character date, two more than a valid one.
[[nodiscard]] std::int64_t end_of_day_ns(std::string_view trade_date) noexcept {
  constexpr std::string_view kEndOfDay = "T23:59:59.999999999Z";
  char buffer[32];
  const std::size_t total = trade_date.size() + kEndOfDay.size();
  if (trade_date.empty() || total > sizeof buffer) {
    return 0;
  }
  std::memcpy(buffer, trade_date.data(), trade_date.size());
  std::memcpy(buffer + trade_date.size(), kEndOfDay.data(), kEndOfDay.size());
  return iso_to_ns(std::string_view(buffer, total));
}

// Locate the nine tab-separated fields of `line` into `out`, in a single forward
// pass with no allocation. Replaces a per-row `std::vector<std::string_view>`
// built by the old `split(line, '\t')` — one heap allocation per row, on a file
// of ~8.7M rows.
//
// Returns false unless `line` carries EXACTLY eight separators. `split(...)
// .size() != 9` gave both directions for free; a scan gives neither, and both
// are load-bearing:
//
//   - FEWER than eight tabs: the scan stops early and leaves `out` PARTIALLY
//     written, so the caller must treat a false return as "read nothing". `out`
//     is therefore declared per row by the caller rather than hoisted out of the
//     row loop: a hoisted buffer would let a short row silently inherit its
//     predecessor's trailing fields. (Observed: with a hoisted buffer an
//     eight-field row parses using the previous row's source_fingerprint.)
//   - A NINTH tab: a scan that stops once it has eight separators leaves the
//     tenth field unread past the end of `out[8]` and accepts a 10-field row as
//     if it had nine. The absence of any further tab before end-of-line is
//     therefore asserted explicitly rather than implied.
//
// LF-only, exactly as before: '\r' is not a separator and is not stripped, so a
// CRLF file still fails the header equality gate.
[[nodiscard]] bool find_row_fields(std::string_view line, std::string_view (&out)[9]) noexcept {
  if (line.empty()) {
    return false;
  }
  const char *cursor = line.data();
  const char *const end = cursor + line.size();
  for (std::size_t f = 0; f < 8; ++f) {
    const char *const tab = static_cast<const char *>(
        std::memchr(cursor, '\t', static_cast<std::size_t>(end - cursor)));
    if (tab == nullptr) {
      return false; // fewer than eight separators
    }
    out[f] = std::string_view(cursor, static_cast<std::size_t>(tab - cursor));
    cursor = tab + 1;
  }
  if (std::memchr(cursor, '\t', static_cast<std::size_t>(end - cursor)) != nullptr) {
    return false; // a ninth separator: ten or more fields
  }
  out[8] = std::string_view(cursor, static_cast<std::size_t>(end - cursor));
  return true;
}

// Number of '\n' bytes in `text`. `split(text, '\n').size()` is exactly this
// plus one; recovering the count with one sequential memchr pass is what lets
// `definitions` still be reserved exactly once without materialising the line
// index (see `parse_listed_definitions`).
[[nodiscard]] std::size_t count_newlines(std::string_view text) noexcept {
  std::size_t count = 0;
  const char *cursor = text.data();
  const char *const end = cursor + text.size();
  while (cursor != end) {
    const char *const found = static_cast<const char *>(
        std::memchr(cursor, '\n', static_cast<std::size_t>(end - cursor)));
    if (found == nullptr) {
      break;
    }
    ++count;
    cursor = found + 1;
  }
  return count;
}

template <typename T> [[nodiscard]] bool parse_integer(std::string_view text, T &value) {
  const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
  return error == std::errc{} && end == text.data() + text.size();
}

[[nodiscard]] bool parse_double(std::string_view text, double &value) {
  const auto [end, error] =
      std::from_chars(text.data(), text.data() + text.size(), value, std::chars_format::general);
  return error == std::errc{} && end == text.data() + text.size() && std::isfinite(value);
}

[[nodiscard]] std::uint64_t joined_source_fingerprint(std::uint64_t quote,
                                                      std::uint64_t definition) {
  std::string material;
  material.reserve(48);
  append_u64(material, quote);
  material.push_back('|');
  append_u64(material, definition);
  return fingerprint_text(material);
}

[[nodiscard]] Result<void> write_atomic(std::string_view path, std::string_view contents) {
  const std::filesystem::path target{path};
  if (target.empty()) {
    return Err(ErrorCode::InvalidArgument, "listed definitions: empty output path");
  }
  std::error_code error;
  if (!target.parent_path().empty()) {
    std::filesystem::create_directories(target.parent_path(), error);
    if (error) {
      return Err(ErrorCode::IoError, "listed definitions: cannot create output directory");
    }
  }
  std::filesystem::path pending = target;
  pending += ".pending";
  {
    std::ofstream stream(pending, std::ios::binary | std::ios::trunc);
    if (!stream || !stream.write(contents.data(), static_cast<std::streamsize>(contents.size()))) {
      return Err(ErrorCode::IoError, "listed definitions: cannot write pending file");
    }
  }
  std::filesystem::remove(target, error);
  error.clear();
  std::filesystem::rename(pending, target, error);
  if (error) {
    return Err(ErrorCode::IoError, "listed definitions: cannot publish file");
  }
  return Ok();
}

} // namespace

Result<ListedDefinitionTable>
ListedDefinitionTable::create(std::vector<ListedContractDefinition> definitions) {
  std::sort(definitions.begin(), definitions.end(),
            [](const auto &a, const auto &b) { return definition_key(a) < definition_key(b); });

  // Single-slot memo for the per-row end-of-day bound.
  //
  // CORRECTNESS DOES NOT DEPEND ON THE INPUT ORDER. This is a compare-then-
  // refresh memo, not a compute-once memo: it recomputes whenever the row's date
  // differs from the memoized one, so after the `if` below the invariant
  // `memo_trade_end == end_of_day_ns(definition.trade_date)` holds for EVERY row
  // under ANY permutation of the input. `end_of_day_ns` is a pure function of its
  // argument bytes, so a reordering can only change how often it is called.
  //
  // What the sort above buys is HIT RATE, not correctness. `definition_key`'s
  // FIRST field is `trade_date`, so the sort leaves `trade_date` non-decreasing
  // across this loop and the single slot refreshes once per distinct date (~60)
  // instead of up to once per row (~8.7M). If the sort ever stopped preceding the
  // loop the memo would still be exact — just slower — which is why a map keyed
  // by date is unnecessary here rather than unsafe.
  //
  // The memoed view aliases a row of `definitions`, which is only read until it is
  // moved out below.
  std::string_view memo_date;
  std::int64_t memo_trade_end = 0;

  for (std::size_t i = 0; i < definitions.size(); ++i) {
    const ListedContractDefinition &definition = definitions[i];
    if (definition.trade_date.empty() || definition.instrument_id == 0 ||
        definition.raw_symbol.empty() || definition.definition_ts_ns <= 0 ||
        definition.expiry_ts_ns <= definition.definition_ts_ns ||
        !std::isfinite(definition.multiplier) || !(definition.multiplier > 0.0) ||
        definition.source_fingerprint == 0u) {
      return Err(ErrorCode::InvalidArgument, "listed definitions: malformed definition");
    }
    // The gate above already rejected an empty `trade_date`, so the empty memo
    // sentinel can never collide with a real date.
    if (definition.trade_date != memo_date) {
      memo_date = definition.trade_date;
      memo_trade_end = end_of_day_ns(memo_date);
    }
    const std::int64_t trade_end = memo_trade_end;
    if (trade_end <= 0 || definition.definition_ts_ns > trade_end) {
      return Err(ErrorCode::InvalidArgument,
                 "listed definitions: future or invalid date-scoped definition");
    }
    if (i > 0 && definition_key(definitions[i - 1]) == definition_key(definition)) {
      return Err(ErrorCode::AlreadyExists, "listed definitions: duplicate definition key");
    }
  }

  ListedDefinitionTable table;
  table.definitions_ = std::move(definitions);
  return Ok(std::move(table));
}

// Lazy, memoized. See the header for why this is a plain `mutable` memo and not
// a `std::once_flag`, and why it is not `noexcept`. `serialize_listed_definitions`
// itself is unchanged — only WHEN it runs moved.
std::uint64_t ListedDefinitionTable::fingerprint() const {
  if (fingerprint_ == 0u) {
    fingerprint_ = fingerprint_text(serialize_listed_definitions(*this));
  }
  return fingerprint_;
}

const ListedContractDefinition *
ListedDefinitionTable::find(std::string_view trade_date, std::uint32_t instrument_id,
                            std::string_view raw_symbol) const noexcept {
  const auto key = std::tuple{trade_date, instrument_id, raw_symbol};
  const auto found = std::lower_bound(
      definitions_.begin(), definitions_.end(), key,
      [](const auto &definition, const auto &rhs) { return definition_key(definition) < rhs; });
  return found != definitions_.end() && definition_key(*found) == key ? &*found : nullptr;
}

std::string serialize_listed_definitions(const ListedDefinitionTable &table) {
  std::string out;
  out.append(kMagic).push_back('\n');
  out.append(kHeader).push_back('\n');
  for (const ListedContractDefinition &definition : table.definitions()) {
    out.append(definition.trade_date).push_back('\t');
    append_u64(out, definition.instrument_id);
    out.push_back('\t');
    out.append(definition.raw_symbol).push_back('\t');
    append_i64(out, definition.definition_ts_ns);
    out.push_back('\t');
    append_i64(out, definition.expiry_ts_ns);
    out.push_back('\t');
    append_double(out, definition.multiplier);
    out.push_back('\t');
    out.push_back(definition.standard_monthly ? '1' : '0');
    out.push_back('\t');
    out.push_back(definition.standard_deliverable ? '1' : '0');
    out.push_back('\t');
    append_u64(out, definition.source_fingerprint);
    out.push_back('\n');
  }
  return out;
}

Result<ListedDefinitionTable> parse_listed_definitions(std::string_view tsv) {
  // Forward line walk, no line index. `split(tsv, '\n')` used to materialise one
  // `string_view` per line: on the ~8.7M-row production file that is ~140 MB of
  // index, and its geometric growth peaks at ~2.5x that in transient
  // reallocation, all on top of the ~700 MB buffer the views point into. Nothing
  // here needs random access — the rows are consumed strictly in order.
  //
  // `cursor > tsv.size()` is the exhausted state, so a final line with no '\n'
  // advances the cursor to `tsv.size() + 1`. That reproduces `split`'s element
  // sequence exactly, INCLUDING the single empty element `split("")` yields and
  // the trailing empty element after a terminating '\n' (which the empty-line
  // skip below absorbs, unchanged).
  std::size_t cursor = 0;
  const auto exhausted = [&] { return cursor > tsv.size(); };
  const auto next_line = [&] {
    const std::size_t newline = tsv.find('\n', cursor);
    const std::size_t end = newline == std::string_view::npos ? tsv.size() : newline;
    const std::string_view line = tsv.substr(cursor, end - cursor);
    cursor = end + 1;
    return line;
  };

  const std::string_view magic = next_line(); // cursor starts at 0, so always present
  if (exhausted()) {                          // fewer than two lines
    return Err(ErrorCode::ParseError, "listed definitions: bad header");
  }
  const std::string_view header = next_line();
  if (magic != kMagic || header != kHeader) {
    return Err(ErrorCode::ParseError, "listed definitions: bad header");
  }

  std::vector<ListedContractDefinition> definitions;
  // `split(tsv, '\n').size()` was `count_newlines(tsv) + 1`, so the old
  // `lines.size() - 2` is `count_newlines(tsv) - 1`. The header gate above has
  // already established two lines, hence at least one '\n'. Reserving exactly
  // once still matters: on the production file this vector reaches ~1 GB, and
  // geometric growth would peak at ~1.5x that while moving 8.7M rows.
  definitions.reserve(count_newlines(tsv) - 1u);
  while (!exhausted()) {
    const std::string_view line = next_line();
    if (line.empty()) {
      continue; // absorbs the trailing element after a final '\n'
    }
    // Declared INSIDE the loop on purpose — see `find_row_fields`: a partial
    // write from a short row must never be visible to the next row.
    std::string_view fields[9];
    ListedContractDefinition definition;
    std::uint64_t instrument_id = 0;
    unsigned monthly = 0;
    unsigned deliverable = 0;
    if (!find_row_fields(line, fields) || !parse_integer(fields[1], instrument_id) ||
        instrument_id > std::numeric_limits<std::uint32_t>::max() ||
        !parse_integer(fields[3], definition.definition_ts_ns) ||
        !parse_integer(fields[4], definition.expiry_ts_ns) ||
        !parse_double(fields[5], definition.multiplier) || !parse_integer(fields[6], monthly) ||
        monthly > 1 || !parse_integer(fields[7], deliverable) || deliverable > 1 ||
        !parse_integer(fields[8], definition.source_fingerprint)) {
      return Err(ErrorCode::ParseError, "listed definitions: malformed row");
    }
    definition.trade_date = fields[0];
    definition.instrument_id = static_cast<std::uint32_t>(instrument_id);
    definition.raw_symbol = fields[2];
    definition.standard_monthly = monthly != 0;
    definition.standard_deliverable = deliverable != 0;
    definitions.push_back(std::move(definition));
  }
  auto table = ListedDefinitionTable::create(std::move(definitions));
  if (!table) {
    return Err(ErrorCode::ParseError, table.error().to_string());
  }
  return table;
}

Status write_listed_definitions_file(std::string_view path, const ListedDefinitionTable &table) {
  return write_atomic(path, serialize_listed_definitions(table));
}

Result<ListedDefinitionTable> read_listed_definitions_file(std::string_view path) {
  std::ifstream stream(std::filesystem::path{path}, std::ios::binary);
  if (!stream) {
    return Err(ErrorCode::NotFound, "listed definitions: file not found");
  }
  std::string contents((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
  if (!stream.good() && !stream.eof()) {
    return Err(ErrorCode::IoError, "listed definitions: read failed");
  }
  return parse_listed_definitions(contents);
}

namespace {

// UTC day serial (days since 1970-01-01) of an expiry instant. Canonicalizes
// away the time-of-day so a midnight-UTC and a 16:00-ET (20:00Z) stamp of the
// same expiry map to one date.
[[nodiscard]] std::int64_t expiry_day_serial(std::int64_t expiry_ts_ns) noexcept {
  namespace time = atx::core::time;
  const time::CivilTime civil = time::to_civil_utc(time::Timestamp::from_unix_nanos(expiry_ts_ns));
  return time::days_from_civil(civil.date.year, civil.date.month, civil.date.day);
}

} // namespace

std::vector<std::int64_t> standard_monthly_sessions(std::span<const std::int64_t> expiry_ts_ns) {
  namespace time = atx::core::time;
  // Distinct expiry dates (day serials), ascending. Sorting groups by month and
  // lets the third-Friday / Thursday-before lookups use binary search.
  std::vector<std::int64_t> days;
  days.reserve(expiry_ts_ns.size());
  for (const std::int64_t ns : expiry_ts_ns) {
    if (ns > 0) {
      days.push_back(expiry_day_serial(ns));
    }
  }
  std::sort(days.begin(), days.end());
  days.erase(std::unique(days.begin(), days.end()), days.end());
  const auto observed = [&](std::int64_t serial) {
    return std::binary_search(days.begin(), days.end(), serial);
  };

  std::vector<std::int64_t> sessions;
  std::int64_t last_year_month = -1;
  for (const std::int64_t serial : days) {
    const time::Date date = time::civil_from_days(serial);
    const std::int64_t year_month = static_cast<std::int64_t>(date.year) * 12 + date.month;
    if (year_month == last_year_month) {
      continue; // one lookup per calendar month
    }
    last_year_month = year_month;
    const std::int64_t third_friday =
        time::nth_weekday_of_month(date.year, date.month, time::Weekday::Friday, 3).to_days();
    if (observed(third_friday)) {
      sessions.push_back(third_friday);
    } else if (observed(third_friday - 1)) { // Thursday immediately before (holiday shift)
      sessions.push_back(third_friday - 1);
    }
    // else: no standard-monthly session observed for this month in this universe.
  }
  // `days` is ascending so months (and thus sessions) are emitted in order, but
  // keep the sort explicit — is_standard_monthly_expiry relies on it.
  std::sort(sessions.begin(), sessions.end());
  return sessions;
}

bool is_standard_monthly_expiry(std::span<const std::int64_t> sessions,
                                std::int64_t expiry_ts_ns) {
  if (expiry_ts_ns <= 0) {
    return false;
  }
  return std::binary_search(sessions.begin(), sessions.end(), expiry_day_serial(expiry_ts_ns));
}

Result<std::vector<ListedOptionQuote>>
listed_quotes_from_opra(std::string_view trade_date, std::int64_t valuation_ts_ns,
                        const OpraPanel &panel, const ListedDefinitionTable &definitions,
                        MissingDefinitionPolicy policy) {
  if (trade_date.empty() || valuation_ts_ns <= 0 || panel.frame.uid.empty() ||
      panel.frame.snapshot_ts_ns != valuation_ts_ns || panel.source_schema_version < 2 ||
      !panel.provenance_complete || panel.source_fingerprint == 0u ||
      panel.source_instrument_ids.size() != panel.frame.rows.size()) {
    return Err(ErrorCode::InvalidArgument,
               "listed OPRA join: panel lacks strict aligned source provenance");
  }
  if (ns_to_iso_date(valuation_ts_ns) != trade_date) {
    return Err(ErrorCode::InvalidArgument, "listed OPRA join: valuation date mismatch");
  }

  std::vector<ListedOptionQuote> quotes;
  quotes.reserve(panel.frame.rows.size());
  for (std::size_t i = 0; i < panel.frame.rows.size(); ++i) {
    const QuoteRow &row = panel.frame.rows[i];
    const std::uint32_t instrument_id = panel.source_instrument_ids[i];
    const auto identity = std::lower_bound(
        panel.source_identities.begin(), panel.source_identities.end(), instrument_id,
        [](const OpraInstrumentIdentity &candidate, std::uint32_t id) {
          return candidate.instrument_id < id;
        });
    if (instrument_id == 0 || identity == panel.source_identities.end() ||
        identity->instrument_id != instrument_id) {
      return Err(ErrorCode::NotFound, "listed OPRA join: aligned source identity missing");
    }
    const ListedContractDefinition *definition =
        definitions.find(trade_date, instrument_id, identity->raw_symbol);
    if (definition == nullptr) {
      // OCC numeric root suffixes identify adjusted/non-standard contracts.
      // The strict exporter intentionally omits them because OPRA does not
      // populate deliverable fields; they are ineligible for this workflow.
      ATX_TRY(const OsiSymbol missing_osi, parse_osi_symbol(identity->raw_symbol));
      if (std::any_of(missing_osi.root.begin(), missing_osi.root.end(),
                      [](unsigned char ch) { return std::isdigit(ch) != 0; })) {
        continue;
      }
      // Same-session (0DTE) contracts — OSI expiry date equal to the trade date
      // — are structurally outside this consumer's universe and are skipped, not
      // treated as a missing authority. The OPRA panel keeps them because their
      // PM-settled (16:00 ET) expiry instant is after the intraday snapshot, but
      // the point-in-time definition authority predates same-day rows and stamps
      // expiration at midnight-UTC of the expiry date, so a 0DTE contract can
      // never satisfy this join even when a definition row exists (it would trip
      // the look-ahead/expiry guard below). The listed-dispersion workflow only
      // ever selects 21-60 DTE, so these are pure noise here. Invariant: on this
      // path every kept quote has expiry strictly after the trade date.
      if (missing_osi.expiry_iso == trade_date) {
        continue;
      }
      // No point-in-time definition for a standard-root, non-0DTE contract. By
      // default this is a fatal missing authority; a caller may opt into
      // SkipUnlisted to treat it as panel noise (an intraday-listed contract the
      // authority does not yet know) outside its universe. This is the ONLY
      // behavior the policy alters — the look-ahead/expiry and economics-agreement
      // checks below stay fatal for any contract that DOES have a definition.
      if (policy == MissingDefinitionPolicy::SkipUnlisted) {
        continue;
      }
      return Err(ErrorCode::NotFound, "listed OPRA join: contract definition missing");
    }
    if (definition->definition_ts_ns > valuation_ts_ns ||
        definition->expiry_ts_ns <= valuation_ts_ns) {
      return Err(ErrorCode::InvalidArgument, "listed OPRA join: definition look-ahead or expiry");
    }
    ATX_TRY(const OsiSymbol osi, parse_osi_symbol(identity->raw_symbol));
    const std::string symbol = row.uid.empty() ? panel.frame.uid : row.uid;
    if (osi.root != symbol || osi.expiry_iso != row.expiry_iso || osi.side != row.side ||
        osi.strike != row.strike || ns_to_iso_date(definition->expiry_ts_ns) != osi.expiry_iso) {
      return Err(ErrorCode::InvalidArgument,
                 "listed OPRA join: quote, OSI, and definition economics disagree");
    }

    ListedOptionQuote quote;
    quote.trade_date = trade_date;
    quote.symbol = symbol;
    quote.instrument_id = instrument_id;
    quote.raw_symbol = identity->raw_symbol;
    quote.expiry_ts_ns = definition->expiry_ts_ns;
    quote.strike = row.strike;
    quote.side = row.side;
    quote.bid = row.bid;
    quote.ask = row.ask;
    quote.quote_ts_ns = row.ts_ns == 0 ? valuation_ts_ns : row.ts_ns;
    quote.multiplier = definition->multiplier;
    quote.standard_monthly = definition->standard_monthly;
    quote.standard_deliverable = definition->standard_deliverable;
    quote.source_fingerprint =
        joined_source_fingerprint(panel.source_fingerprint, definition->source_fingerprint);
    if (quote.quote_ts_ns > valuation_ts_ns) {
      return Err(ErrorCode::InvalidArgument, "listed OPRA join: future quote");
    }
    quotes.push_back(std::move(quote));
  }
  return Ok(std::move(quotes));
}

} // namespace atx::vol
