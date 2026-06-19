#include "atx/engine/regime/source_csv.hpp"

#include <algorithm>
#include <charconv>
#include <fstream>
#include <limits>
#include <sstream>

namespace atx::engine::regime {

namespace {

constexpr atx::f64 kNaN = std::numeric_limits<atx::f64>::quiet_NaN();

// Howard Hinnant days_from_civil (proleptic Gregorian), matching the engine's
// date math. y/m/d are 1-based month/day.
[[nodiscard]] atx::i64 days_from_civil(atx::i64 y, unsigned m, unsigned d) noexcept {
  y -= (m <= 2);
  const atx::i64 era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = static_cast<unsigned>(y - era * 400);
  const unsigned doy = (153u * (m + (m > 2 ? -3 : 9)) + 2u) / 5u + (d - 1u);
  const unsigned doe = yoe * 365u + yoe / 4u - yoe / 100u + doy;
  return era * 146097LL + static_cast<atx::i64>(doe) - 719468LL;
}

[[nodiscard]] std::vector<std::string_view> split(std::string_view line, char sep) {
  std::vector<std::string_view> out;
  std::size_t start = 0;
  for (std::size_t i = 0; i <= line.size(); ++i) {
    if (i == line.size() || line[i] == sep) {
      out.push_back(line.substr(start, i - start));
      start = i + 1;
    }
  }
  return out;
}

[[nodiscard]] std::string_view rstrip_cr(std::string_view v) {
  if (!v.empty() && v.back() == '\r') v.remove_suffix(1);
  return v;
}

}  // namespace

atx::core::Result<atx::i64> date_to_nanos(std::string_view ymd) {
  // Expect exactly "YYYY-MM-DD".
  if (ymd.size() != 10 || ymd[4] != '-' || ymd[7] != '-') {
    return atx::core::Err(atx::core::ErrorCode::ParseError,
                          std::string{"date_to_nanos: bad date '"} + std::string{ymd} + "'");
  }
  int y = 0, m = 0, d = 0;
  auto ok = [](std::from_chars_result r, const char *end) {
    return r.ec == std::errc{} && r.ptr == end;
  };
  const char *p = ymd.data();
  if (!ok(std::from_chars(p, p + 4, y), p + 4) ||
      !ok(std::from_chars(p + 5, p + 7, m), p + 7) ||
      !ok(std::from_chars(p + 8, p + 10, d), p + 10) || m < 1 || m > 12 || d < 1 || d > 31) {
    return atx::core::Err(atx::core::ErrorCode::ParseError,
                          std::string{"date_to_nanos: non-numeric '"} + std::string{ymd} + "'");
  }
  const atx::i64 days = days_from_civil(y, static_cast<unsigned>(m), static_cast<unsigned>(d));
  return atx::core::Ok(days * 86400LL * 1000000000LL);
}

atx::core::Result<std::vector<std::pair<atx::i64, atx::f64>>>
parse_series_content(std::string_view content, CsvFormat /*fmt*/, std::string_view value_column) {
  std::vector<std::pair<atx::i64, atx::f64>> rows;
  std::size_t pos = 0;
  bool header_seen = false;
  std::size_t date_col = 0;
  std::size_t val_col = std::string_view::npos;

  while (pos <= content.size()) {
    const std::size_t nl = content.find('\n', pos);
    const std::string_view raw =
        rstrip_cr(content.substr(pos, nl == std::string_view::npos ? std::string_view::npos
                                                                   : nl - pos));
    pos = (nl == std::string_view::npos) ? content.size() + 1 : nl + 1;
    if (raw.empty()) {
      if (nl == std::string_view::npos) break;
      continue;
    }
    const std::vector<std::string_view> cols = split(raw, ',');
    if (!header_seen) {
      header_seen = true;
      for (std::size_t i = 0; i < cols.size(); ++i) {
        if (cols[i] == value_column) val_col = i;
      }
      if (val_col == std::string_view::npos) {
        return atx::core::Err(atx::core::ErrorCode::ParseError,
                              std::string{"parse_series_content: header lacks column '"} +
                                  std::string{value_column} + "'");
      }
      continue;
    }
    if (date_col >= cols.size() || val_col >= cols.size()) continue;  // ragged row -> skip
    auto dn = date_to_nanos(cols[date_col]);
    if (!dn.has_value()) continue;  // unparseable date -> skip the row
    atx::f64 value = kNaN;
    const std::string_view cell = cols[val_col];
    if (!cell.empty() && cell != ".") {
      double parsed = 0.0;
      const std::from_chars_result r =
          std::from_chars(cell.data(), cell.data() + cell.size(), parsed);
      value = (r.ec == std::errc{} && r.ptr == cell.data() + cell.size()) ? parsed : kNaN;
    }
    rows.emplace_back(dn.value(), value);
  }

  std::stable_sort(rows.begin(), rows.end(),
                   [](const auto &a, const auto &b) { return a.first < b.first; });
  // Dedup keeping the LAST occurrence of each date (stable_sort preserved order).
  std::vector<std::pair<atx::i64, atx::f64>> deduped;
  for (const auto &row : rows) {
    if (!deduped.empty() && deduped.back().first == row.first) {
      deduped.back().second = row.second;  // later wins
    } else {
      deduped.push_back(row);
    }
  }
  return atx::core::Ok(std::move(deduped));
}

atx::core::Result<std::vector<std::pair<atx::i64, atx::f64>>>
parse_series_csv(const std::string &path, CsvFormat fmt, std::string_view value_column) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    return atx::core::Err(atx::core::ErrorCode::IoError,
                          std::string{"parse_series_csv: cannot open '"} + path + "'");
  }
  std::stringstream ss;
  ss << in.rdbuf();
  const std::string content = ss.str();
  return parse_series_content(content, fmt, value_column);
}

}  // namespace atx::engine::regime
