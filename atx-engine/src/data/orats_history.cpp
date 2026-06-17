#include "atx/engine/data/orats_history.hpp"

#include <charconv>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string>
#include <unordered_map>
#include <vector>

#include "atx/core/io/parquet_writer.hpp"
#include "atx/core/io/zip_reader.hpp"
#include "atx/tsdb/load_parquet.hpp"

namespace atx::engine::data {
namespace detail {
namespace {
// Days from 1970-01-01 to civil (y,m,d), proleptic Gregorian (Howard Hinnant).
constexpr atx::i64 days_from_civil(int y, unsigned m, unsigned d) noexcept {
  y -= static_cast<int>(m <= 2);
  const int era = (y >= 0 ? y : y - 399) / 400;
  const auto yoe = static_cast<unsigned>(y - era * 400);
  const unsigned doy = (153u * (m + (m > 2 ? -3u : 9u)) + 2u) / 5u + d - 1u;
  const unsigned doe = yoe * 365u + yoe / 4u - yoe / 100u + doy;
  return static_cast<atx::i64>(era) * 146097LL + static_cast<atx::i64>(doe) - 719468LL;
}
} // namespace

std::optional<atx::i64> date_to_nanos(std::string_view ymd) {
  // strict "YYYY-MM-DD"
  if (ymd.size() != 10 || ymd[4] != '-' || ymd[7] != '-') return std::nullopt;
  int y = 0, mo = 0, d = 0;
  auto num = [](std::string_view s, int &out) {
    const auto r = std::from_chars(s.data(), s.data() + s.size(), out);
    return r.ec == std::errc{} && r.ptr == s.data() + s.size();
  };
  if (!num(ymd.substr(0, 4), y) || !num(ymd.substr(5, 2), mo) || !num(ymd.substr(8, 2), d))
    return std::nullopt;
  if (mo < 1 || mo > 12 || d < 1 || d > 31) return std::nullopt;
  const atx::i64 days = days_from_civil(y, static_cast<unsigned>(mo), static_cast<unsigned>(d));
  return days * 86400LL * 1000000000LL;
}

atx::core::Result<ColumnIndex> resolve_header(std::string_view header_line) {
  std::unordered_map<std::string_view, int> pos;
  int col = 0;
  atx::usize start = 0;
  while (start <= header_line.size()) {
    const atx::usize tab = header_line.find('\t', start);
    const atx::usize end = (tab == std::string_view::npos) ? header_line.size() : tab;
    pos.emplace(header_line.substr(start, end - start), col++);
    if (tab == std::string_view::npos) break;
    start = end + 1;
  }
  ColumnIndex idx;
  auto need = [&](std::string_view name, int &dst) -> bool {
    const auto it = pos.find(name);
    if (it == pos.end()) return false;
    dst = it->second;
    return true;
  };
  bool ok = need("tradingDate", idx.tradingDate) && need("securityID", idx.securityID) &&
            need("ticker_tk", idx.ticker_tk) && need("todayTicker", idx.todayTicker);
  // kOratsFields map by identical name EXCEPT gics->"GICS", earnFlag->"earnFlag".
  for (atx::usize f = 0; ok && f < kOratsFields.size(); ++f) {
    std::string_view src = kOratsFields[f];
    if (src == "gics") src = "GICS";
    ok = need(src, idx.field[f]);
  }
  if (!ok)
    return atx::core::Err(atx::core::ErrorCode::ParseError,
                          "orats resolve_header: a required column is missing");
  return atx::core::Ok(idx);
}
} // namespace detail

namespace {
namespace fs = std::filesystem;

inline atx::f64 parse_f64(std::string_view s) {
  if (s.empty()) return std::numeric_limits<atx::f64>::quiet_NaN();
  atx::f64 v{};
  const auto r = std::from_chars(s.data(), s.data() + s.size(), v);
  if (r.ec != std::errc{}) return std::numeric_limits<atx::f64>::quiet_NaN();
  return v;
}

// Split a line into fields by tab WITHOUT allocating (returns views into `line`).
void split_tabs(std::string_view line, std::vector<std::string_view> &out) {
  out.clear();
  atx::usize start = 0;
  for (;;) {
    const atx::usize tab = line.find('\t', start);
    if (tab == std::string_view::npos) {
      out.push_back(line.substr(start));
      break;
    }
    out.push_back(line.substr(start, tab - start));
    start = tab + 1;
  }
}

// Accumulates one date's rows, then writes a sealed .seg via build_from_long.
struct DateAccumulator {
  atx::i64 date_nanos{};
  std::string date_str;                        // YYYY-MM-DD for the filename
  std::vector<std::string> symbols;            // securityID per row
  std::vector<std::vector<atx::f64>> values;   // [16][rows]
  DateAccumulator() : values(kOratsFields.size()) {}
  void clear(atx::i64 dn, std::string ds) {
    date_nanos = dn;
    date_str = std::move(ds);
    symbols.clear();
    for (auto &v : values) v.clear();
  }
  bool empty() const { return symbols.empty(); }
};

atx::core::Status flush_date(DateAccumulator &acc, const std::string &out_dir,
                              atx::i64 created_at_nanos) {
  atx::tsdb::LongColumns cols;
  cols.field_names.assign(kOratsFields.begin(), kOratsFields.end());
  const atx::usize rows = acc.symbols.size();
  cols.times.assign(rows, acc.date_nanos); // all rows share this date's midnight nanos
  cols.symbols = acc.symbols;
  cols.values = acc.values;
  const std::string path = (fs::path(out_dir) / (acc.date_str + ".seg")).string();
  return atx::tsdb::build_from_long(cols, path, created_at_nanos);
}

} // namespace

atx::core::Result<OratsLoadStats> load_orats_history(const OratsLoadConfig &cfg) {
  namespace fs = std::filesystem;

  fs::create_directories(cfg.out_dir);

  ATX_TRY(auto reader, atx::core::io::ZipEntryReader::open(cfg.zip_path, "tbltickerhistory"));

  OratsLoadStats stats{};
  DateAccumulator acc;
  atx::i64 current_date_nanos = std::numeric_limits<atx::i64>::min();

  // symbology: securityID (as i64) -> (ticker_tk, todayTicker), first-seen
  std::unordered_map<atx::i64, std::pair<std::string, std::string>> symbology;

  detail::ColumnIndex idx;
  bool header_parsed = false;

  constexpr atx::usize kChunk = 1u << 20; // 1 MiB
  std::vector<char> buf(kChunk);
  std::string carry;
  carry.reserve(4096);

  std::vector<std::string_view> fields;
  fields.reserve(128);

  for (;;) {
    auto read_res = reader.read(std::span<char>(buf.data(), buf.size()));
    if (!read_res.has_value()) return tl::unexpected(read_res.error());
    const atx::usize n = *read_res;

    std::string_view chunk;
    std::string combined;
    if (!carry.empty()) {
      combined = std::move(carry);
      carry.clear();
      combined.append(buf.data(), n);
      chunk = std::string_view(combined);
    } else {
      chunk = std::string_view(buf.data(), n);
    }

    atx::usize pos = 0;
    while (pos < chunk.size()) {
      const atx::usize nl = chunk.find('\n', pos);
      if (nl == std::string_view::npos) {
        // Partial trailing line — save for next chunk
        carry.assign(chunk.data() + pos, chunk.size() - pos);
        break;
      }
      // Strip trailing \r if present
      atx::usize end = nl;
      if (end > pos && chunk[end - 1] == '\r') --end;
      const std::string_view line = chunk.substr(pos, end - pos);
      pos = nl + 1;

      if (!header_parsed) {
        auto res = detail::resolve_header(line);
        if (!res.has_value()) return tl::unexpected(res.error());
        idx = *res;
        header_parsed = true;
        continue;
      }

      if (line.empty()) continue;
      ++stats.rows_read;

      split_tabs(line, fields);
      const int max_col = static_cast<int>(fields.size()) - 1;

      // Parse trading date
      const std::string_view date_sv =
          (idx.tradingDate <= max_col) ? fields[idx.tradingDate] : std::string_view{};
      auto date_opt = detail::date_to_nanos(date_sv);
      if (!date_opt.has_value()) {
        ++stats.rows_malformed;
        continue;
      }
      const atx::i64 date_nanos = *date_opt;
      const std::string_view date_str_sv = date_sv; // YYYY-MM-DD

      // Date floor filter
      if (date_nanos < cfg.min_date_nanos) {
        ++stats.rows_filtered;
        continue;
      }

      // Parse securityID
      const std::string_view secid_sv =
          (idx.securityID <= max_col) ? fields[idx.securityID] : std::string_view{};
      if (secid_sv.empty()) {
        ++stats.rows_malformed;
        continue;
      }

      // Date-major guard
      if (date_nanos < current_date_nanos) {
        return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                              "orats load: non-monotonic tradingDate");
      }

      // Date boundary: flush previous date
      if (date_nanos != current_date_nanos && current_date_nanos != std::numeric_limits<atx::i64>::min()) {
        if (!acc.empty()) {
          ATX_TRY_VOID(flush_date(acc, cfg.out_dir, cfg.created_at_nanos));
          ++stats.dates_written;
        }
        acc.clear(date_nanos, std::string(date_str_sv));
        current_date_nanos = date_nanos;
      } else if (current_date_nanos == std::numeric_limits<atx::i64>::min()) {
        current_date_nanos = date_nanos;
        acc.clear(date_nanos, std::string(date_str_sv));
      }

      // Accumulate row
      acc.symbols.emplace_back(secid_sv);
      for (atx::usize f = 0; f < kOratsFields.size(); ++f) {
        const int col = idx.field[f];
        const std::string_view val_sv = (col <= max_col) ? fields[col] : std::string_view{};
        acc.values[f].push_back(parse_f64(val_sv));
      }

      // Symbology: record first-seen ticker info; key by securityID as i64
      atx::i64 secid_i64 = 0;
      {
        const auto r = std::from_chars(secid_sv.data(), secid_sv.data() + secid_sv.size(), secid_i64);
        if (r.ec != std::errc{}) secid_i64 = 0;
      }
      if (symbology.find(secid_i64) == symbology.end()) {
        const std::string_view tk_sv =
            (idx.ticker_tk <= max_col) ? fields[idx.ticker_tk] : std::string_view{};
        const std::string_view today_sv =
            (idx.todayTicker <= max_col) ? fields[idx.todayTicker] : std::string_view{};
        symbology.emplace(secid_i64, std::make_pair(std::string(tk_sv), std::string(today_sv)));
      }

      ++stats.rows_kept;
    }

    if (n == 0) break; // EOF
  }

  // Flush any remaining carry (no trailing newline case)
  if (!carry.empty() && header_parsed) {
    std::string_view line = carry;
    if (!line.empty() && line.back() == '\r')
      line = line.substr(0, line.size() - 1);
    if (!line.empty()) {
      ++stats.rows_read;
      split_tabs(line, fields);
      const int max_col = static_cast<int>(fields.size()) - 1;
      const std::string_view date_sv =
          (idx.tradingDate <= max_col) ? fields[idx.tradingDate] : std::string_view{};
      auto date_opt = detail::date_to_nanos(date_sv);
      if (date_opt.has_value()) {
        const atx::i64 date_nanos = *date_opt;
        const std::string_view secid_sv =
            (idx.securityID <= max_col) ? fields[idx.securityID] : std::string_view{};
        if (!secid_sv.empty() && date_nanos >= cfg.min_date_nanos) {
          if (date_nanos < current_date_nanos) {
            return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                                  "orats load: non-monotonic tradingDate");
          }
          if (date_nanos != current_date_nanos &&
              current_date_nanos != std::numeric_limits<atx::i64>::min()) {
            if (!acc.empty()) {
              ATX_TRY_VOID(flush_date(acc, cfg.out_dir, cfg.created_at_nanos));
              ++stats.dates_written;
            }
            acc.clear(date_nanos, std::string(date_sv));
            current_date_nanos = date_nanos;
          }
          acc.symbols.emplace_back(secid_sv);
          for (atx::usize f = 0; f < kOratsFields.size(); ++f) {
            const int col = idx.field[f];
            const std::string_view val_sv = (col <= max_col) ? fields[col] : std::string_view{};
            acc.values[f].push_back(parse_f64(val_sv));
          }
          atx::i64 secid_i64 = 0;
          {
            const auto r =
                std::from_chars(secid_sv.data(), secid_sv.data() + secid_sv.size(), secid_i64);
            if (r.ec != std::errc{}) secid_i64 = 0;
          }
          if (symbology.find(secid_i64) == symbology.end()) {
            const std::string_view tk_sv =
                (idx.ticker_tk <= max_col) ? fields[idx.ticker_tk] : std::string_view{};
            const std::string_view today_sv =
                (idx.todayTicker <= max_col) ? fields[idx.todayTicker] : std::string_view{};
            symbology.emplace(secid_i64,
                              std::make_pair(std::string(tk_sv), std::string(today_sv)));
          }
          ++stats.rows_kept;
        } else if (date_nanos < cfg.min_date_nanos) {
          ++stats.rows_filtered;
        } else {
          ++stats.rows_malformed;
        }
      } else {
        ++stats.rows_malformed;
      }
    }
  }

  // Flush final date
  if (!acc.empty()) {
    ATX_TRY_VOID(flush_date(acc, cfg.out_dir, cfg.created_at_nanos));
    ++stats.dates_written;
  }

  // Distinct securities
  stats.distinct_securities = static_cast<atx::i64>(symbology.size());

  // Write _symbology.parquet
  {
    std::vector<atx::i64> sid;
    std::vector<std::string> tk, today;
    sid.reserve(symbology.size());
    tk.reserve(symbology.size());
    today.reserve(symbology.size());
    for (const auto &kv : symbology) {
      sid.push_back(kv.first);
      tk.push_back(kv.second.first);
      today.push_back(kv.second.second);
    }
    const std::vector<atx::core::io::WriteColumn> man = {
        {"securityID", std::span<const atx::i64>(sid)},
        {"ticker_tk", std::span<const std::string>(tk)},
        {"todayTicker", std::span<const std::string>(today)}};
    ATX_TRY_VOID(
        atx::core::io::write_parquet(man, (fs::path(cfg.out_dir) / "_symbology.parquet").string()));
  }

  // Write _manifest.json
  {
    const fs::path manifest_path = fs::path(cfg.out_dir) / "_manifest.json";
    std::ofstream mf(manifest_path);
    mf << "{\n";
    mf << "  \"rows_read\": " << stats.rows_read << ",\n";
    mf << "  \"rows_kept\": " << stats.rows_kept << ",\n";
    mf << "  \"rows_filtered\": " << stats.rows_filtered << ",\n";
    mf << "  \"rows_malformed\": " << stats.rows_malformed << ",\n";
    mf << "  \"dates_written\": " << stats.dates_written << ",\n";
    mf << "  \"distinct_securities\": " << stats.distinct_securities << ",\n";
    mf << "  \"min_date_nanos\": " << cfg.min_date_nanos << ",\n";
    mf << "  \"fields\": [";
    for (atx::usize i = 0; i < kOratsFields.size(); ++i) {
      if (i > 0) mf << ", ";
      mf << "\"" << kOratsFields[i] << "\"";
    }
    mf << "]\n";
    mf << "}\n";
  }

  return atx::core::Ok(stats);
}

} // namespace atx::engine::data
