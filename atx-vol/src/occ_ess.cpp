#include "atx/vol/occ_ess.hpp"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

#include "atx/core/error.hpp"
#include "atx/core/hash.hpp"

namespace atx::vol {
namespace {

using atx::core::Err;
using atx::core::ErrorCode;
using atx::core::Ok;

[[nodiscard]] bool all_digits(std::string_view value) noexcept {
  return !value.empty() &&
         std::all_of(value.begin(), value.end(),
                     [](unsigned char ch) { return std::isdigit(ch) != 0; });
}

[[nodiscard]] std::vector<std::string_view> fields(std::string_view line) {
  std::vector<std::string_view> out;
  std::size_t cursor = 0u;
  while (cursor < line.size()) {
    while (cursor < line.size() && std::isspace(static_cast<unsigned char>(line[cursor])) != 0) {
      ++cursor;
    }
    if (cursor == line.size()) {
      break;
    }
    const std::size_t start = cursor;
    while (cursor < line.size() &&
           std::isspace(static_cast<unsigned char>(line[cursor])) == 0) {
      ++cursor;
    }
    out.push_back(line.substr(start, cursor - start));
  }
  return out;
}

[[nodiscard]] Result<std::string> activity_date(std::string_view text) {
  constexpr std::string_view marker = "ACTIVITY DATE ";
  const std::size_t marker_at = text.find(marker);
  if (marker_at == std::string_view::npos || marker_at + marker.size() + 8u > text.size()) {
    return Err(ErrorCode::ParseError, "OCC ESS: activity date missing");
  }
  const std::string_view date = text.substr(marker_at + marker.size(), 8u);
  if (date[2] != '/' || date[5] != '/' || !all_digits(date.substr(0, 2)) ||
      !all_digits(date.substr(3, 2)) || !all_digits(date.substr(6, 2))) {
    return Err(ErrorCode::ParseError, "OCC ESS: malformed activity date");
  }
  const unsigned month = static_cast<unsigned>((date[0] - '0') * 10 + date[1] - '0');
  const unsigned day = static_cast<unsigned>((date[3] - '0') * 10 + date[4] - '0');
  if (month == 0u || month > 12u || day == 0u || day > 31u) {
    return Err(ErrorCode::ParseError, "OCC ESS: activity date out of range");
  }
  std::string iso = "20";
  iso.append(date.substr(6, 2));
  iso.push_back('-');
  iso.append(date.substr(0, 2));
  iso.push_back('-');
  iso.append(date.substr(3, 2));
  return Ok(std::move(iso));
}

} // namespace

bool OccEssReport::is_special(std::string_view option_product_symbol) const noexcept {
  return std::binary_search(special_symbols_.begin(), special_symbols_.end(),
                            option_product_symbol);
}

Result<OccEssReport> parse_occ_ess_report(std::string_view text) {
  if (text.find("NON-STANDARD SETTLEMENTS") == std::string_view::npos) {
    return Err(ErrorCode::ParseError, "OCC ESS: report title missing");
  }
  ATX_TRY(std::string report_date, activity_date(text));
  std::string process_date;
  process_date.reserve(8u);
  process_date.append(report_date, 0u, 4u);
  process_date.append(report_date, 5u, 2u);
  process_date.append(report_date, 8u, 2u);

  std::vector<std::string> symbols;
  std::size_t cursor = 0u;
  std::size_t record_count = 0u;
  while (cursor <= text.size()) {
    const std::size_t end = text.find('\n', cursor);
    std::string_view line = text.substr(
        cursor, end == std::string_view::npos ? text.size() - cursor : end - cursor);
    if (!line.empty() && line.back() == '\r') {
      line.remove_suffix(1u);
    }
    if (line.starts_with("0706")) {
      const std::vector<std::string_view> row = fields(line);
      if (row.size() != 14u || row[0] != "0706" || row[2] != "OSTK" ||
          row[12] != process_date || row[1].empty()) {
        return Err(ErrorCode::ParseError, "OCC ESS: malformed settlement row");
      }
      symbols.emplace_back(row[1]);
      ++record_count;
    }
    if (end == std::string_view::npos) {
      break;
    }
    cursor = end + 1u;
  }
  if (record_count == 0u) {
    return Err(ErrorCode::ParseError, "OCC ESS: report contains no settlement rows");
  }
  std::sort(symbols.begin(), symbols.end());
  symbols.erase(std::unique(symbols.begin(), symbols.end()), symbols.end());

  OccEssReport report;
  report.trade_date_ = std::move(report_date);
  report.special_symbols_ = std::move(symbols);
  report.source_fingerprint_ = atx::core::hash_bytes(text.data(), text.size());
  if (report.source_fingerprint_ == 0u) {
    report.source_fingerprint_ = 1u;
  }
  return Ok(std::move(report));
}

Result<OccEssReport> read_occ_ess_report_file(std::string_view path) {
  if (path.empty()) {
    return Err(ErrorCode::InvalidArgument, "OCC ESS: empty input path");
  }
  std::ifstream input(std::filesystem::path(path), std::ios::binary);
  if (!input) {
    return Err(ErrorCode::NotFound, "OCC ESS: report file not found");
  }
  const std::string contents((std::istreambuf_iterator<char>(input)),
                             std::istreambuf_iterator<char>());
  if (!input.eof() && input.fail()) {
    return Err(ErrorCode::IoError, "OCC ESS: report read failed");
  }
  return parse_occ_ess_report(contents);
}

} // namespace atx::vol
