// The FINRA inputs of si_shares / si_dtc: the strict as-of CSV contract and the dissemination
// schedule (prepare_research_fields.py parse_asof_csv and read_schedule). Declared in
// finra_asof_field.hpp.
#include <algorithm>
#include <charconv>
#include <cmath>
#include <limits>
#include <optional>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "atx/core/sha256.hpp"
#include "atx/engine/research/fields/clock.hpp"
#include "atx/engine/research/fields/file_io.hpp"
#include "atx/engine/research/fields/finra_asof_field.hpp"

namespace atx::engine::research::fields {
namespace {

constexpr std::string_view kAsofHeader = "security_id,available_at,value";
constexpr u64 kScheduleLimit = 64ULL << 20;

[[nodiscard]] core::Error row_error(usize line_no, std::string_view why) {
  return core::Error(core::ErrorCode::ParseError,
                     "asof csv: line " + std::to_string(line_no) + ": " + std::string(why));
}

[[nodiscard]] bool all_digits(std::string_view text) noexcept {
  return !text.empty() &&
         std::all_of(text.begin(), text.end(), [](char c) { return c >= '0' && c <= '9'; });
}

// -?([0-9]+\.?[0-9]*|\.[0-9]+)([eE][-+]?[0-9]+)?  (the Python's decimal pattern, matched in full)
[[nodiscard]] bool is_decimal(std::string_view s) noexcept {
  usize i = 0;
  const auto digit_run = [&s, &i]() {
    const usize start = i;
    while (i < s.size() && s[i] >= '0' && s[i] <= '9') {
      ++i;
    }
    return i - start;
  };
  if (i < s.size() && s[i] == '-') {
    ++i;
  }
  if (digit_run() > 0) {
    if (i < s.size() && s[i] == '.') {
      ++i;
      static_cast<void>(digit_run());
    }
  } else {
    if (i >= s.size() || s[i] != '.') {
      return false;
    }
    ++i;
    if (digit_run() == 0) {
      return false;
    }
  }
  if (i < s.size() && (s[i] == 'e' || s[i] == 'E')) {
    ++i;
    if (i < s.size() && (s[i] == '-' || s[i] == '+')) {
      ++i;
    }
    if (digit_run() == 0) {
      return false;
    }
  }
  return i == s.size();
}

[[nodiscard]] bool is_nan_literal(std::string_view s) noexcept {
  const auto lower = [](char c) {
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
  };
  return s.size() == 3 && lower(s[0]) == 'n' && lower(s[1]) == 'a' && lower(s[2]) == 'n';
}

[[nodiscard]] core::Result<AsofCsvRow> parse_row(std::string_view line, usize line_no) {
  const auto c1 = line.find(',');
  const auto c2 = c1 == std::string_view::npos ? c1 : line.find(',', c1 + 1);
  if (c1 == std::string_view::npos || c2 == std::string_view::npos ||
      line.find(',', c2 + 1) != std::string_view::npos) {
    return core::Err(row_error(line_no, "expected exactly 3 fields"));
  }
  const std::string_view id_text = line.substr(0, c1);
  const std::string_view date_text = line.substr(c1 + 1, c2 - c1 - 1);
  const std::string_view value_text = line.substr(c2 + 1);
  AsofCsvRow row;
  const char *id_end = id_text.data() + id_text.size();
  const auto id = std::from_chars(id_text.data(), id_end, row.security_id);
  if (!all_digits(id_text) || id.ec != std::errc{} || id.ptr != id_end || row.security_id <= 0) {
    return core::Err(row_error(line_no, "security_id is not a positive i64"));
  }
  const auto day = parse_iso_day(date_text);
  if (!day) {
    return core::Err(row_error(line_no, "available_at is not YYYY-MM-DD"));
  }
  row.available_day = *day;
  if (value_text.empty() || is_nan_literal(value_text)) {
    row.value = std::numeric_limits<f64>::quiet_NaN();
    return core::Ok(row);
  }
  const char *value_end = value_text.data() + value_text.size();
  const auto value =
      std::from_chars(value_text.data(), value_end, row.value, std::chars_format::general);
  if (!is_decimal(value_text) || value.ec != std::errc{} || value.ptr != value_end ||
      !std::isfinite(row.value)) {
    return core::Err(row_error(line_no, "value is not a finite decimal"));
  }
  return core::Ok(row);
}

// One schedule line split by RFC 4180 rules (comma separators, double-quoted fields with ""
// escapes); nullopt on an unterminated quote.
[[nodiscard]] std::optional<std::vector<std::string>> split_csv_line(std::string_view line) {
  std::vector<std::string> fields;
  std::string current;
  bool quoted = false;
  bool field_start = true;
  usize i = 0;
  while (i < line.size()) {
    const char c = line[i];
    ++i;
    if (quoted) {
      if (c != '"') {
        current.push_back(c);
      } else if (i < line.size() && line[i] == '"') {
        current.push_back('"');
        ++i;
      } else {
        quoted = false;
      }
      continue;
    }
    if (c == '"' && field_start) {
      quoted = true;
      field_start = false;
    } else if (c == ',') {
      fields.push_back(std::move(current));
      current.clear();
      field_start = true;
    } else {
      current.push_back(c);
      field_start = false;
    }
  }
  if (quoted) {
    return std::nullopt;
  }
  fields.push_back(std::move(current));
  return fields;
}

// The lines of a text file without their LF / CRLF terminators (a final empty segment is not a
// line).
[[nodiscard]] std::vector<std::string_view> lines_of(std::string_view text) {
  std::vector<std::string_view> out;
  while (!text.empty()) {
    const auto nl = text.find('\n');
    std::string_view line = text.substr(0, nl);
    text = nl == std::string_view::npos ? std::string_view{} : text.substr(nl + 1);
    if (!line.empty() && line.back() == '\r') {
      line.remove_suffix(1);
    }
    out.push_back(line);
  }
  return out;
}

} // namespace

core::Result<std::vector<AsofCsvRow>> parse_asof_csv(std::string_view bytes) {
  const auto first_newline = bytes.find('\n');
  std::string_view header = bytes.substr(0, first_newline);
  if (!header.empty() && header.back() == '\r') {
    header.remove_suffix(1);
  }
  if (header != kAsofHeader) {
    return core::Err(core::ErrorCode::InvalidArgument,
                     "asof csv: header must be exactly '" + std::string(kAsofHeader) + "'");
  }
  // One optional trailing newline; any empty line (or a bare CR line) is a parse error, as the C++
  // panel loader.
  const auto has = [bytes](std::string_view what) {
    return bytes.find(what) != std::string_view::npos;
  };
  if (has("\n\n") || has("\n\r\n") || has("\r\r") ||
      (bytes.size() >= 2 && bytes.substr(bytes.size() - 2) == "\n\r")) {
    return core::Err(core::ErrorCode::ParseError, "asof csv: empty line");
  }
  std::vector<AsofCsvRow> rows;
  if (first_newline == std::string_view::npos) {
    return core::Ok(std::move(rows));
  }
  const auto lines = lines_of(bytes.substr(first_newline + 1));
  rows.reserve(lines.size());
  for (usize k = 0; k < lines.size(); ++k) {
    ATX_TRY(const auto row, parse_row(lines[k], k + 2));
    rows.push_back(row);
  }
  std::sort(rows.begin(), rows.end(), [](const AsofCsvRow &a, const AsofCsvRow &b) {
    return a.security_id != b.security_id ? a.security_id < b.security_id
                                          : a.available_day < b.available_day;
  });
  for (usize i = 1; i < rows.size(); ++i) {
    if (rows[i].security_id == rows[i - 1].security_id &&
        rows[i].available_day == rows[i - 1].available_day) {
      return core::Err(core::ErrorCode::InvalidArgument,
                       "asof csv: duplicate (security_id, available_at)");
    }
  }
  return core::Ok(std::move(rows));
}

core::Result<DisseminationSchedule>
read_dissemination_schedule(const std::filesystem::path &finra_root) {
  const auto path = finra_root / "dissemination_schedule.csv";
  ATX_TRY(const auto bytes, read_bounded(path, kScheduleLimit));
  ATX_TRY(const auto digest, core::sha256_hex(std::string_view(bytes)));
  const auto lines = lines_of(bytes);
  if (lines.empty()) {
    return core::Err(core::ErrorCode::InvalidArgument, "dissemination schedule: empty file");
  }
  const auto header = split_csv_line(lines.front());
  if (!header) {
    return core::Err(core::ErrorCode::ParseError, "dissemination schedule: bad header");
  }
  const auto column = [&header](std::string_view name) -> std::optional<usize> {
    const auto it = std::find(header->begin(), header->end(), name);
    if (it == header->end()) {
      return std::nullopt;
    }
    return static_cast<usize>(it - header->begin());
  };
  const auto settlement = column("settlement_date");
  const auto dissemination = column("dissemination_date");
  if (!settlement || !dissemination) {
    return core::Err(core::ErrorCode::InvalidArgument,
                     "dissemination schedule: header lacks settlement_date or dissemination_date");
  }
  const auto republication_end = parse_iso_day(kFinraRepublicationSettlementBefore);
  if (!republication_end) {
    return core::Err(core::ErrorCode::Internal,
                     "dissemination schedule: bad republication constant");
  }
  DisseminationSchedule out;
  for (usize k = 1; k < lines.size(); ++k) {
    if (lines[k].empty()) {
      continue; // csv.DictReader skips blank rows
    }
    const auto fields = split_csv_line(lines[k]);
    if (!fields || fields->size() <= std::max(*settlement, *dissemination)) {
      return core::Err(core::ErrorCode::ParseError,
                       "dissemination schedule: short row " + std::to_string(k + 1));
    }
    const auto settled = parse_iso_day((*fields)[*settlement]);
    const auto published = parse_iso_day((*fields)[*dissemination]);
    if (!settled || !published) {
      return core::Err(core::ErrorCode::ParseError,
                       "dissemination schedule: bad date in row " + std::to_string(k + 1));
    }
    out.dissemination_days.push_back(*published);
    if (*settled < *republication_end &&
        (!out.vintage_cutoff_day || *published > *out.vintage_cutoff_day)) {
      out.vintage_cutoff_day = *published;
    }
  }
  std::sort(out.dissemination_days.begin(), out.dissemination_days.end());
  out.dissemination_days.erase(
      std::unique(out.dissemination_days.begin(), out.dissemination_days.end()),
      out.dissemination_days.end());
  out.source = SourceRecord{record_path(path), static_cast<u64>(bytes.size()), digest};
  return core::Ok(std::move(out));
}

} // namespace atx::engine::research::fields
