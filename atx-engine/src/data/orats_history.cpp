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
#include <functional>
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
    // Phase 3: the 16x from_chars value parse moved off the producer into the
    // workers. So `parse` here is the producer's per-row ROUTING cost (line
    // frame + 4 key-column extract + symbology), and the worker-summed metric now
    // covers parse+build+write. Both worker terms are OVERLAPPED behind the
    // producer wall (not added to it) -> labeled "(workers,summed)".
    std::fprintf(stderr,
                 "[orats-profile] inflate=%.1fms producer_route=%.1fms "
                 "worker_parse_build_write(workers,summed)=%.1fms rows_kept=%lld dates=%lld\n",
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

// The handful of columns the PRODUCER needs from each row: date (routing/guard/
// floor), securityID (required-check + symbology key), ticker_tk + todayTicker
// (symbology). Views into the source line — copy before the buffer is reused.
struct KeyFields {
  std::string_view date, secid, ticker, today;
  std::string_view gics; // captured only when the producer scans as far as the GICS column
};

// Single-pass partial split: walk tabs only as far as `max_needed` (the highest
// of the four key column indices), capturing just those four fields. The 16 value
// columns are deliberately NOT touched here — their raw bytes ride to a worker,
// where the expensive from_chars parse runs in parallel (Phase 3). A column index
// past the row's actual field count yields an empty view, matching the old
// field_at() out-of-range behavior (-> malformed date / missing securityID).
KeyFields extract_key_fields(std::string_view line, const detail::ColumnIndex &idx,
                             int max_needed) {
  KeyFields k;
  int col = 0;
  atx::usize start = 0;
  for (;;) {
    const atx::usize tab = line.find('\t', start);
    const atx::usize end = (tab == std::string_view::npos) ? line.size() : tab;
    const std::string_view f = line.substr(start, end - start);
    if (col == idx.tradingDate) k.date = f;
    if (col == idx.securityID) k.secid = f;
    if (col == idx.ticker_tk) k.ticker = f;
    if (col == idx.todayTicker) k.today = f;
    if (col == idx.field[11]) k.gics = f; // GICS (kOratsFields[11]); captured iff max_needed reaches it
    if (tab == std::string_view::npos || col >= max_needed) break;
    start = end + 1;
    ++col;
  }
  return k;
}

// Accumulates one date's rows, then writes a sealed .seg via build_from_long.
// Phase 3: the producer stores each kept row's RAW bytes (not parsed doubles) so
// the 16x from_chars parse can run later on a worker thread. `symbols` and `raw`
// stay in lockstep — one securityID and one '\n'-terminated line appended per row.
struct DateAccumulator {
  atx::i64 date_nanos{};
  std::string date_str;             // YYYY-MM-DD for the filename
  std::vector<std::string> symbols; // securityID per kept row
  std::string raw;                  // kept rows' raw bytes, '\n'-separated (worker parses)
  DateAccumulator() = default;
  void clear(atx::i64 dn, std::string ds) {
    date_nanos = dn;
    date_str = std::move(ds);
    symbols.clear();
    raw.clear(); // seal() moves raw out, so this re-reserves a fresh buffer below
    constexpr atx::usize kRowsPerDateHint = 8192;
    constexpr atx::usize kBytesPerRowHint = 200; // ~71 cols, mostly short numerics
    symbols.reserve(kRowsPerDateHint);
    raw.reserve(kRowsPerDateHint * kBytesPerRowHint);
  }
  bool empty() const { return symbols.empty(); }
};

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
  int max_needed_col; // highest key-column index the producer must scan to (set post-header)
  // Seals a completed date's accumulator into a writer-pool job (the loader
  // supplies this). Routing the date-boundary flush through a callback keeps
  // process_line oblivious to threading: it just hands off the previous date.
  std::function<atx::core::Status(DateAccumulator &)> flush;
};

// Process ONE data line (header already consumed). Increments rows_read, then
// classifies the row into exactly one of {malformed, filtered, kept} so the
// invariant rows_read == rows_filtered + rows_malformed + rows_kept holds on
// every path. A date regression (raw input, BEFORE the floor) fails closed.
atx::core::Status process_line(std::string_view line, LoadState &st) {
  ++st.stats.rows_read;

  // Cheap partial split: pull only the four key columns (all early in the real
  // header). The 16 value columns stay as raw bytes for a worker to parse.
  const KeyFields k = extract_key_fields(line, st.idx, st.max_needed_col);

  // 1) Parse trading date.
  const auto date_opt = detail::date_to_nanos(k.date);
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
  if (k.secid.empty()) {
    ++st.stats.rows_malformed;
    return atx::core::Ok();
  }

  // 4b) Single-stock prune (opt-in): a row with no parseable GICS sector is not a
  //     single stock (ETF/fund), so drop it before it reaches the partition. This
  //     keeps such symbols out of every .seg, tightening the panel. Counts as a
  //     filtered row so rows_read == filtered + malformed + kept still holds.
  //     `k.gics` is populated only when max_needed_col reaches the GICS column,
  //     which the loader extends iff exclude_no_sector is set (so the guard and the
  //     scan are enabled together). NaN-without-<cmath>: g != g.
  if (st.cfg.exclude_no_sector) {
    const atx::f64 g = parse_f64(k.gics);
    if (g != g) { // NaN -> empty or unparseable GICS -> not a single stock
      ++st.stats.rows_filtered;
      return atx::core::Ok();
    }
  }

  // 5) Date boundary: seal the previous date into a writer-pool job (the loader's
  //    callback counts it + enqueues it), then start a new accumulator. The seal
  //    is a near-instant enqueue; the parse+build+write happens on a worker thread.
  if (date_nanos != st.current_date_nanos) {
    ATX_TRY_VOID(st.flush(st.acc)); // no-op if empty; the callback owns the count
    st.acc.clear(date_nanos, std::string(k.date));
    st.current_date_nanos = date_nanos;
  }

  // 6) Keep the row: securityID (already extracted) + the raw line bytes. The 16
  //    value columns are parsed later on a worker thread (run_job), not here.
  //    symbols and raw stay in lockstep (one entry + one line per kept row).
  st.acc.symbols.emplace_back(k.secid);
  st.acc.raw.append(line.data(), line.size());
  st.acc.raw.push_back('\n');

  // 7) Symbology side-car: first-seen ticker info, keyed by securityID as i64.
  atx::i64 secid_i64 = 0;
  {
    const auto r =
        std::from_chars(k.secid.data(), k.secid.data() + k.secid.size(), secid_i64);
    if (r.ec != std::errc{}) secid_i64 = 0;
  }
  st.symbology.try_emplace(secid_i64, std::string(k.ticker), std::string(k.today));

  ++st.stats.rows_kept;
  return atx::core::Ok();
}

// One trading date's rows, sealed and ready for a worker to parse+pivot+write.
// Phase 3: carries the kept rows' RAW bytes (not pre-parsed doubles) plus the
// resolved column map; the worker runs the 16x from_chars in run_job.
struct DateJob {
  std::string out_path;             // <out_dir>/YYYY-MM-DD.seg
  atx::i64 date_nanos{};
  atx::i64 created_at_nanos{};
  std::vector<std::string> symbols; // securityID per kept row
  std::string raw;                  // kept rows' raw bytes, '\n'-separated
  detail::ColumnIndex idx;          // which TSV columns hold the 16 values (copied, ~80 B)
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

// Holds the first worker error (if any) under a mutex; workers race to set it.
// The atomic flag is the fast path (lock-free check from the producer's seal);
// the mutex only guards reading/writing the Error payload itself.
struct WorkerError {
  std::mutex m;
  std::atomic<bool> failed{false};
  atx::core::Error err;
  void set(atx::core::Error e) {
    if (failed.exchange(true)) return; // keep only the first
    std::lock_guard<std::mutex> lk(m);
    err = std::move(e);
  }
};

// Parse one job's raw rows, pivot into columns, and write its .seg. Safe to run
// on any thread: SegmentBuilder is build-once/instance-local and each job writes
// a distinct file (one per trading date), so workers never contend on shared
// output. Phase 3: the 16x from_chars value parse runs HERE (in parallel across
// workers) instead of on the producer. `scratch` is a per-worker reusable split
// buffer (one allocation amortized across all of a worker's jobs).
//
// Output is byte-identical to the serial parse: parse_f64 is a pure function of
// the row bytes, rows are parsed in producer (input) order, and symbols[i] still
// pairs with values[f][i] — so each .seg is unchanged regardless of W.
atx::core::Status run_job(DateJob &job, std::vector<std::string_view> &scratch) {
  const atx::usize rows = job.symbols.size();
  atx::tsdb::LongColumns cols;
  cols.field_names.assign(kOratsFields.begin(), kOratsFields.end());
  cols.times.assign(rows, job.date_nanos); // all rows share this date's midnight nanos
  cols.symbols = std::move(job.symbols);
  cols.values.assign(kOratsFields.size(), {});
  for (auto &v : cols.values) v.reserve(rows);

  // Re-split each kept row and run the 16 from_chars (empty/unparseable -> NaN).
  std::string_view raw(job.raw);
  atx::usize pos = 0;
  for (;;) {
    const atx::usize nl = raw.find('\n', pos);
    if (nl == std::string_view::npos) break; // raw is '\n'-terminated; last find -> npos
    const std::string_view line = raw.substr(pos, nl - pos);
    pos = nl + 1;
    split_tabs(line, scratch);
    const int max_col = static_cast<int>(scratch.size()) - 1;
    for (atx::usize f = 0; f < kOratsFields.size(); ++f) {
      const int c = job.idx.field[f];
      const std::string_view fv =
          (c >= 0 && c <= max_col) ? scratch[static_cast<atx::usize>(c)] : std::string_view{};
      cols.values[f].push_back(parse_f64(fv));
    }
  }

  return atx::tsdb::build_from_long(cols, job.out_path, job.created_at_nanos);
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

  // max_needed_col is set once the header resolves (the producer scans each row
  // only this far); 0 until then — no data row is processed before the header.
  LoadState st{cfg, idx, stats, acc, current_date_nanos, symbology, 0, {}};

  constexpr atx::usize kChunk = 1u << 22; // 4 MiB inflate reads
  std::vector<char> buf(kChunk);
  atx::usize fill = 0;        // valid bytes currently in buf[0, fill)
  bool eof = false;

  LoadProfile prof;

  // -------------------------------------------------------------------------
  //  Writer pool: producer (this thread) parses + routes; workers build+write.
  //
  //  The producer stays single-threaded and ordered (inflate -> frame ->
  //  process_line: date parse, monotonic guard, floor, securityID, accumulate,
  //  symbology). Only the per-date pivot+write (run_job) runs on workers. Each
  //  <date>.seg is fully determined by that date's rows, so output is
  //  byte-identical regardless of W or worker scheduling.
  // -------------------------------------------------------------------------
  const unsigned hw = std::thread::hardware_concurrency();
  const std::size_t W = std::clamp<std::size_t>(hw == 0 ? 1 : hw - 1, 1, 8);
  WorkerError werr;
  // Worker-summed build+write time (nanoseconds). One fetch_add per date (~hundreds
  // total) -> negligible contention. This is OVERLAPPED wall time hidden behind the
  // producer, not added to it; report() labels it as such.
  std::atomic<long long> build_write_ns{0};
  BoundedQueue<DateJob> jobs{2 * W}; // small backlog so the producer rarely stalls

  // seal: hand a completed date's accumulator to the pool. Empty -> no-op. If a
  // worker has already failed, surface that error promptly so the producer stops
  // pushing (and the read loop unwinds). The producer owns dates_written: it
  // knows exactly which dates were sealed, independent of worker timing.
  auto seal = [&](DateAccumulator &a) -> atx::core::Status {
    if (a.empty()) return atx::core::Ok();
    if (werr.failed.load(std::memory_order_acquire)) {
      std::lock_guard<std::mutex> lk(werr.m);
      return atx::core::Err(werr.err); // a worker already failed; abort the load
    }
    DateJob job;
    job.out_path = (fs::path(cfg.out_dir) / (a.date_str + ".seg")).string();
    job.date_nanos = a.date_nanos;
    job.created_at_nanos = cfg.created_at_nanos;
    job.symbols = std::move(a.symbols);
    job.raw = std::move(a.raw);
    job.idx = idx; // resolved before any data row is sealed; worker parses with it
    jobs.push(std::move(job));
    ++stats.dates_written;
    return atx::core::Ok();
  };
  st.flush = seal;

  std::vector<std::thread> pool;
  pool.reserve(W);
  for (std::size_t i = 0; i < W; ++i) {
    pool.emplace_back([&jobs, &werr, &build_write_ns] {
      std::vector<std::string_view> scratch; // per-worker split buffer (reused across jobs)
      scratch.reserve(128);
      DateJob job;
      while (jobs.pop(job)) {
        const auto tbw = LoadProfile::clock::now();
        const auto s = run_job(job, scratch);
        const auto elapsed = LoadProfile::clock::now() - tbw;
        build_write_ns.fetch_add(
            std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count(),
            std::memory_order_relaxed);
        if (!s.has_value()) {
          werr.set(s.error());
          // Keep draining: stopping here would let the queue fill and deadlock
          // the producer's push(). Once failed is set, remaining jobs are popped
          // and built anyway (cheap relative to a hang); the first error wins.
        }
      }
    });
  }

  // RAII safety net: on EVERY return path (inflate error, header-parse error,
  // non-monotonic-date error, or normal completion) this closes the queue and
  // joins every still-joinable worker. Without it an early return would leave
  // workers blocked in pop() and ~std::thread would std::terminate. The normal
  // path also closes+joins explicitly below (to check werr); joining an already
  // joined thread is skipped via joinable().
  struct PoolJoiner {
    BoundedQueue<DateJob> &q;
    std::vector<std::thread> &p;
    ~PoolJoiner() {
      q.close();
      for (auto &t : p)
        if (t.joinable()) t.join();
    }
  } joiner{jobs, pool};

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
        // Producer scans each row only to the highest key column (all early in
        // the real header), then hands the rest off as raw bytes.
        st.max_needed_col =
            std::max({idx.tradingDate, idx.securityID, idx.ticker_tk, idx.todayTicker});
        // The single-stock prune reads GICS in the producer; extend the scan to it.
        if (cfg.exclude_no_sector) {
          st.max_needed_col = std::max(st.max_needed_col, idx.field[11]);
        }
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

  // Seal the final accumulated date (no-op if empty).
  ATX_TRY_VOID(seal(acc));

  // Drain + join the pool, then surface the first worker error (if any). The
  // RAII PoolJoiner would also do this, but we join explicitly here so a worker
  // build/write failure is reported as the load result rather than swallowed.
  jobs.close();
  for (auto &t : pool)
    if (t.joinable()) t.join();
  if (werr.failed.load(std::memory_order_acquire)) {
    std::lock_guard<std::mutex> lk(werr.m);
    return atx::core::Err(werr.err);
  }
  // All workers joined -> build_write_ns is fully synchronized. Record the
  // worker-summed build+write time for the profile (overlapped wall time).
  prof.build_write = std::chrono::duration_cast<LoadProfile::clock::duration>(
      std::chrono::nanoseconds(build_write_ns.load(std::memory_order_relaxed)));

  // ---- Below here is producer-only post-processing: it MUST run after the join
  //      so symbology/manifest are built once, deterministically, on this
  //      thread. Workers never touch symbology. ----
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
