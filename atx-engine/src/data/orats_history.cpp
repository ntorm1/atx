#include "atx/engine/data/orats_history.hpp"

#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <limits>
#include <mutex>
#include <string>
#include <thread>
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
  // kOratsFields are SEGMENT field names; map each back to its TSV header column.
  // Most match by identical name EXCEPT:
  //   * gics -> "GICS"
  //   * cumReturnFactor (15-char segment name) -> "cumulReturnFactor" (real TSV
  //     header; the segment name was shortened to fit the atx-tsdb 15-char limit).
  for (atx::usize f = 0; ok && f < kOratsFields.size(); ++f) {
    std::string_view src = kOratsFields[f];
    if (src == "gics") src = "GICS";
    else if (src == kOratsFields[10]) src = "cumulReturnFactor";
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

// Coarse wall-clock split of the serial pipeline, emitted to stderr only when
// ATX_ORATS_PROFILE is set (any non-empty value). Off => zero overhead beyond a
// few steady_clock reads, which are negligible vs. the work they bracket.
struct LoadProfile {
  using clock = std::chrono::steady_clock;
  clock::duration inflate{}, parse{}, build_write{};
  bool enabled{false};
  LoadProfile() {
#if defined(_MSC_VER) || defined(_WIN32)
    char *e = nullptr;
    std::size_t len = 0;
    _dupenv_s(&e, &len, "ATX_ORATS_PROFILE");
    enabled = (e != nullptr && e[0] != '\0');
    free(e);
#else
    const char *e = std::getenv("ATX_ORATS_PROFILE");
    enabled = (e != nullptr && e[0] != '\0');
#endif
  }
  static double ms(clock::duration d) {
    return std::chrono::duration<double, std::milli>(d).count();
  }
  void report(const OratsLoadStats &s) const {
    if (!enabled) return;
    std::fprintf(stderr,
                 "[orats-profile] inflate=%.1fms parse=%.1fms build_write=%.1fms "
                 "rows_kept=%lld dates=%lld\n",
                 ms(inflate), ms(parse), ms(build_write),
                 static_cast<long long>(s.rows_kept),
                 static_cast<long long>(s.dates_written));
  }
};

inline atx::f64 parse_f64(std::string_view s) {
  if (s.empty()) return std::numeric_limits<atx::f64>::quiet_NaN();
  atx::f64 v{};
  const auto r = std::from_chars(s.data(), s.data() + s.size(), v);
  // Reject partial parses ("1.2x") as well as hard failures — NaN, never a
  // truncated value.
  if (r.ec != std::errc{} || r.ptr != s.data() + s.size())
    return std::numeric_limits<atx::f64>::quiet_NaN();
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
    values.assign(kOratsFields.size(), {});  // rebuild 16 empty columns (move-safe)
    constexpr atx::usize kRowsPerDateHint = 8192;
    symbols.reserve(kRowsPerDateHint);
    for (auto &v : values) v.reserve(kRowsPerDateHint);
  }
  bool empty() const { return symbols.empty(); }
};

atx::core::Status flush_date(DateAccumulator &acc, const std::string &out_dir,
                              atx::i64 created_at_nanos) {
  atx::tsdb::LongColumns cols;
  cols.field_names.assign(kOratsFields.begin(), kOratsFields.end());
  const atx::usize rows = acc.symbols.size();
  cols.times.assign(rows, acc.date_nanos); // all rows share this date's midnight nanos
  cols.symbols = std::move(acc.symbols);
  cols.values = std::move(acc.values);
  const std::string path = (fs::path(out_dir) / (acc.date_str + ".seg")).string();
  return atx::tsdb::build_from_long(cols, path, created_at_nanos);
}

// Mutable per-load state threaded through process_line. Holds everything the row
// projector mutates so the SAME code path serves the main streaming loop and the
// final carry flush (no duplication).
struct LoadState {
  const OratsLoadConfig &cfg;
  const detail::ColumnIndex &idx;
  OratsLoadStats &stats;
  DateAccumulator &acc;
  atx::i64 &current_date_nanos;
  std::unordered_map<atx::i64, std::pair<std::string, std::string>> &symbology;
  std::vector<std::string_view> &fields; // reusable scratch
  LoadProfile::clock::duration *build_write{}; // non-owning; nullptr => no timing
};

// Process ONE data line (header already consumed). Increments rows_read, then
// classifies the row into exactly one of {malformed, filtered, kept} so the
// invariant rows_read == rows_filtered + rows_malformed + rows_kept holds on
// every path. A date regression (raw input, BEFORE the floor) fails closed.
atx::core::Status process_line(std::string_view line, LoadState &st) {
  ++st.stats.rows_read;

  split_tabs(line, st.fields);
  const int max_col = static_cast<int>(st.fields.size()) - 1;
  auto field_at = [&](int col) -> std::string_view {
    return (col >= 0 && col <= max_col) ? st.fields[static_cast<atx::usize>(col)]
                                        : std::string_view{};
  };

  // 1) Parse trading date.
  const std::string_view date_sv = field_at(st.idx.tradingDate);
  const auto date_opt = detail::date_to_nanos(date_sv);
  if (!date_opt.has_value()) {
    ++st.stats.rows_malformed;
    return atx::core::Ok();
  }
  const atx::i64 date_nanos = *date_opt;

  // 2) Monotonic date-major guard — on ALL rows, BEFORE the floor filter. The
  //    contract is "input MUST be date-major"; a regression among sub-floor rows
  //    is still a malformed input, not a silent drop.
  if (date_nanos < st.current_date_nanos) {
    return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                          "orats load: non-monotonic tradingDate");
  }

  // 3) Date floor filter.
  if (date_nanos < st.cfg.min_date_nanos) {
    ++st.stats.rows_filtered;
    return atx::core::Ok();
  }

  // 4) securityID required.
  const std::string_view secid_sv = field_at(st.idx.securityID);
  if (secid_sv.empty()) {
    ++st.stats.rows_malformed;
    return atx::core::Ok();
  }

  // 5) Date boundary: flush the previous date's segment, then start a new one.
  if (date_nanos != st.current_date_nanos) {
    if (!st.acc.empty()) {
      const auto tbw = LoadProfile::clock::now();
      ATX_TRY_VOID(flush_date(st.acc, st.cfg.out_dir, st.cfg.created_at_nanos));
      if (st.build_write) *st.build_write += LoadProfile::clock::now() - tbw;
      ++st.stats.dates_written;
    }
    st.acc.clear(date_nanos, std::string(date_sv));
    st.current_date_nanos = date_nanos;
  }

  // 6) Accumulate the projected fields (empty/unparseable -> NaN).
  st.acc.symbols.emplace_back(secid_sv);
  for (atx::usize f = 0; f < kOratsFields.size(); ++f) {
    st.acc.values[f].push_back(parse_f64(field_at(st.idx.field[f])));
  }

  // 7) Symbology side-car: first-seen ticker info, keyed by securityID as i64.
  atx::i64 secid_i64 = 0;
  {
    const auto r =
        std::from_chars(secid_sv.data(), secid_sv.data() + secid_sv.size(), secid_i64);
    if (r.ec != std::errc{}) secid_i64 = 0;
  }
  st.symbology.try_emplace(secid_i64,
                           std::string(field_at(st.idx.ticker_tk)),
                           std::string(field_at(st.idx.todayTicker)));

  ++st.stats.rows_kept;
  return atx::core::Ok();
}

// One trading date's projected columns, sealed and ready to pivot+write.
struct DateJob {
  std::string out_path;                      // <out_dir>/YYYY-MM-DD.seg
  atx::i64 date_nanos{};
  atx::i64 created_at_nanos{};
  std::vector<std::string> symbols;          // securityID per row
  std::vector<std::vector<atx::f64>> values; // [16][rows]
};

// Bounded, blocking MPMC handoff. Coarse granularity (one job == one date,
// thousands of rows), so a mutex/condvar queue is the right tool — the
// lock-free ring buffers in atx-core/concurrent target per-event hot paths.
template <class T>
class BoundedQueue {
public:
  explicit BoundedQueue(std::size_t cap) : cap_{cap} {}

  // Returns false if the queue was closed before this push could be accepted.
  bool push(T v) {
    std::unique_lock lk(m_);
    not_full_.wait(lk, [&] { return q_.size() < cap_ || closed_; });
    if (closed_) return false;
    q_.push_back(std::move(v));
    lk.unlock();
    not_empty_.notify_one();
    return true;
  }

  // Returns false once the queue is closed AND drained.
  bool pop(T &out) {
    std::unique_lock lk(m_);
    not_empty_.wait(lk, [&] { return !q_.empty() || closed_; });
    if (q_.empty()) return false;
    out = std::move(q_.front());
    q_.pop_front();
    lk.unlock();
    not_full_.notify_one();
    return true;
  }

  void close() {
    {
      std::lock_guard lk(m_);
      closed_ = true;
    }
    not_empty_.notify_all();
    not_full_.notify_all();
  }

private:
  std::size_t cap_;
  std::deque<T> q_;
  std::mutex m_;
  std::condition_variable not_full_, not_empty_;
  bool closed_{false};
};

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

  std::vector<std::string_view> fields;
  fields.reserve(128);

  LoadState st{cfg, idx, stats, acc, current_date_nanos, symbology, fields, nullptr};

  constexpr atx::usize kChunk = 1u << 22; // 4 MiB inflate reads
  std::vector<char> buf(kChunk);
  atx::usize fill = 0;        // valid bytes currently in buf[0, fill)
  bool eof = false;

  LoadProfile prof;
  st.build_write = &prof.build_write;

  while (!eof) {
    // Ensure room to read: if buf is full of an unconsumed partial line, grow it
    // (a single line longer than the buffer is pathological but handled).
    if (fill == buf.size()) buf.resize(buf.size() * 2);

    const auto t0 = LoadProfile::clock::now();
    auto read_res = reader.read(std::span<char>(buf.data() + fill, buf.size() - fill));
    prof.inflate += LoadProfile::clock::now() - t0;
    if (!read_res.has_value()) return tl::unexpected(read_res.error());
    const atx::usize n = *read_res;
    fill += n;
    if (n == 0) eof = true;

    std::string_view chunk(buf.data(), fill);
    atx::usize pos = 0;
    for (;;) {
      const atx::usize nl = chunk.find('\n', pos);
      if (nl == std::string_view::npos) break; // no more complete lines this fill
      atx::usize end = nl;
      if (end > pos && chunk[end - 1] == '\r') --end;
      const std::string_view line = chunk.substr(pos, end - pos);
      pos = nl + 1;

      if (!header_parsed) {
        ATX_TRY(auto resolved, detail::resolve_header(line));
        idx = resolved;
        header_parsed = true;
        continue;
      }
      if (line.empty()) continue;
      const auto tp = LoadProfile::clock::now();
      ATX_TRY_VOID(process_line(line, st));
      prof.parse += LoadProfile::clock::now() - tp;
    }

    // Compact: keep the unconsumed tail [pos, fill) at the front of buf.
    if (pos > 0) {
      const atx::usize tail = fill - pos;
      std::memmove(buf.data(), buf.data() + pos, tail);
      fill = tail;
    }
  }

  // Final line with no trailing newline lives in buf[0, fill).
  if (fill > 0 && header_parsed) {
    std::string_view line(buf.data(), fill);
    if (!line.empty() && line.back() == '\r') line = line.substr(0, line.size() - 1);
    if (!line.empty()) {
      const auto tp = LoadProfile::clock::now();
      ATX_TRY_VOID(process_line(line, st));
      prof.parse += LoadProfile::clock::now() - tp;
    }
  }

  // Flush the final accumulated date.
  if (!acc.empty()) {
    const auto tbw = LoadProfile::clock::now();
    ATX_TRY_VOID(flush_date(acc, cfg.out_dir, cfg.created_at_nanos));
    prof.build_write += LoadProfile::clock::now() - tbw;
    ++stats.dates_written;
  }

  stats.distinct_securities = static_cast<atx::i64>(symbology.size());

  // Write _symbology.parquet (write_parquet returns a Status — propagate it).
  // Sort by securityID ascending for byte-reproducible output across builds.
  {
    std::vector<atx::i64> sorted_keys;
    sorted_keys.reserve(symbology.size());
    for (const auto &kv : symbology) {
      sorted_keys.push_back(kv.first);
    }
    std::sort(sorted_keys.begin(), sorted_keys.end());

    std::vector<atx::i64> sid;
    std::vector<std::string> tk, today;
    sid.reserve(symbology.size());
    tk.reserve(symbology.size());
    today.reserve(symbology.size());
    for (const atx::i64 key : sorted_keys) {
      const auto &pair = symbology.at(key);
      sid.push_back(key);
      tk.push_back(pair.first);
      today.push_back(pair.second);
    }
    const std::vector<atx::core::io::WriteColumn> man = {
        {"securityID", std::span<const atx::i64>(sid)},
        {"ticker_tk", std::span<const std::string>(tk)},
        {"todayTicker", std::span<const std::string>(today)}};
    ATX_TRY_VOID(
        atx::core::io::write_parquet(man, (fs::path(cfg.out_dir) / "_symbology.parquet").string()));
  }

  // Write _manifest.json — check the stream open AND the final state so write
  // errors (full disk, bad path) surface as Err(IoError), not silent data loss.
  {
    const fs::path manifest_path = fs::path(cfg.out_dir) / "_manifest.json";
    std::ofstream mf(manifest_path);
    if (!mf)
      return atx::core::Err(atx::core::ErrorCode::IoError,
                            "orats load: cannot open manifest: " + manifest_path.string());
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
    mf.flush();
    if (!mf)
      return atx::core::Err(atx::core::ErrorCode::IoError,
                            "orats load: failed writing manifest: " + manifest_path.string());
  }

  prof.report(stats);
  return atx::core::Ok(stats);
}

} // namespace atx::engine::data
