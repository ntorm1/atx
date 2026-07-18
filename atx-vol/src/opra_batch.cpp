#include "atx/vol/opra_batch.hpp"

#include <algorithm>
#include <bit>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <filesystem>
#include <limits>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <utility>
#include <vector>

#include "atx/core/error.hpp"       // Ok, Err, ErrorCode
#include "atx/core/hash.hpp"        // hash_bytes
#include "atx/vol/curve.hpp"        // YieldCurve
#include "atx/vol/data.hpp"         // iso_to_ns
#include "atx/vol/parallel_for.hpp" // parallel_for_dynamic (W4.3 per-file fan-out)

namespace atx::vol {

using atx::core::Ok;
using atx::core::Err;
using atx::core::ErrorCode;

namespace {

namespace fs = std::filesystem;

// ── Civil-date kernel (Howard-Hinnant days-from-civil) ──────────────────────
//
// A serial day count keyed at 1970-01-01 = 0, so an inclusive date range becomes
// a contiguous integer interval we walk one day at a time. Self-contained on
// purpose: no external date library (chrono's year_month_day round-trip would do,
// but the raw algorithm is a dozen lines and keeps the dependency surface flat).

struct Civil {
  int y = 0;
  unsigned m = 0;
  unsigned d = 0;
};

// Serial day number for a civil date (Howard Hinnant, "chrono-Compatible Low-Level
// Date Algorithms"). Valid for the Gregorian calendar; m in [1,12], d in [1,31].
[[nodiscard]] std::int64_t days_from_civil(int y, unsigned m, unsigned d) noexcept {
  y -= (m <= 2);
  const std::int64_t era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = static_cast<unsigned>(y - era * 400);                  // [0, 399]
  const unsigned doy = (153u * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1;      // [0, 365]
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;                 // [0, 146096]
  return era * 146097 + static_cast<std::int64_t>(doe) - 719468;
}

// Inverse of days_from_civil.
[[nodiscard]] Civil civil_from_days(std::int64_t z) noexcept {
  z += 719468;
  const std::int64_t era = (z >= 0 ? z : z - 146096) / 146097;
  const unsigned doe = static_cast<unsigned>(z - era * 146097);               // [0, 146096]
  const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365; // [0, 399]
  const int y = static_cast<int>(yoe) + static_cast<int>(era) * 400;
  const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);               // [0, 365]
  const unsigned mp = (5 * doy + 2) / 153;                                    // [0, 11]
  const unsigned d = doy - (153 * mp + 2) / 5 + 1;                            // [1, 31]
  const unsigned m = mp < 10 ? mp + 3 : mp - 9;                               // [1, 12]
  return Civil{y + static_cast<int>(m <= 2), m, d};
}

// Parse a full-width unsigned decimal field, requiring the whole span be digits.
[[nodiscard]] bool parse_uint(std::string_view s, int& out) noexcept {
  const char* first = s.data();
  const char* last = s.data() + s.size();
  const std::from_chars_result res = std::from_chars(first, last, out);
  return res.ec == std::errc{} && res.ptr == last;
}

// Parse exactly "YYYY-MM-DD". Rejects wrong length, missing dashes, non-numeric
// fields, or an out-of-range month/day.
[[nodiscard]] bool parse_civil(std::string_view s, Civil& out) noexcept {
  if (s.size() != 10 || s[4] != '-' || s[7] != '-') {
    return false;
  }
  int y = 0;
  int m = 0;
  int d = 0;
  if (!parse_uint(s.substr(0, 4), y) || !parse_uint(s.substr(5, 2), m) ||
      !parse_uint(s.substr(8, 2), d)) {
    return false;
  }
  if (m < 1 || m > 12 || d < 1 || d > 31) {
    return false;
  }
  out = Civil{y, static_cast<unsigned>(m), static_cast<unsigned>(d)};
  return true;
}

// Format a civil date as "YYYY-MM-DD".
[[nodiscard]] std::string format_civil(const Civil& c) {
  char buf[11];
  std::snprintf(buf, sizeof(buf), "%04d-%02u-%02u", c.y, c.m, c.d);
  return std::string(buf);
}

// Substitute "{symbol}" and "{date}" tokens in a path template. Unknown text is
// copied verbatim; a lone '{' that starts neither token is copied as-is.
[[nodiscard]] std::string apply_template(std::string_view tmpl, std::string_view symbol,
                                         std::string_view date) {
  std::string out;
  out.reserve(tmpl.size() + symbol.size() + date.size());
  std::size_t i = 0;
  while (i < tmpl.size()) {
    if (tmpl.compare(i, 8, "{symbol}") == 0) {
      out.append(symbol);
      i += 8;
    } else if (tmpl.compare(i, 6, "{date}") == 0) {
      out.append(date);
      i += 6;
    } else {
      out.push_back(tmpl[i]);
      ++i;
    }
  }
  return out;
}

[[nodiscard]] std::string canonical_market_symbol(std::string_view symbol) {
  while (!symbol.empty() && symbol.front() == ' ') {
    symbol.remove_prefix(1u);
  }
  while (!symbol.empty() && symbol.back() == ' ') {
    symbol.remove_suffix(1u);
  }
  std::string out;
  out.reserve(symbol.size());
  for (const char ch : symbol) {
    out.push_back(ch >= 'a' && ch <= 'z' ? static_cast<char>(ch - ('a' - 'A')) : ch);
  }
  return out;
}

[[nodiscard]] bool valid_as_of(const ExternalInputTag &tag, std::string_view cell_date) noexcept {
  if (tag.source.empty() || tag.as_of.size() < 10u) {
    return false;
  }
  Civil cell;
  Civil as_of;
  if (!parse_civil(cell_date, cell) ||
      !parse_civil(std::string_view(tag.as_of).substr(0u, 10u), as_of)) {
    return false;
  }
  return days_from_civil(as_of.y, as_of.m, as_of.d) <= days_from_civil(cell.y, cell.m, cell.d);
}

void append_token(std::string &out, std::string_view value) {
  char size[32];
  const auto [ptr, ec] = std::to_chars(size, size + sizeof size, value.size());
  (void)ec;
  out.append(size, static_cast<std::size_t>(ptr - size));
  out.push_back(':');
  out.append(value);
  out.push_back('|');
}

void append_u64(std::string &out, std::uint64_t value) {
  char text[32];
  const auto [ptr, ec] = std::to_chars(text, text + sizeof text, value);
  (void)ec;
  out.append(text, static_cast<std::size_t>(ptr - text));
  out.push_back('|');
}

void append_optional_double(std::string &out, const std::optional<double> &value) {
  append_u64(out, value.has_value() ? 1u : 0u);
  if (value.has_value()) {
    append_u64(out, std::bit_cast<std::uint64_t>(*value));
  }
}

void append_tag(std::string &out, const ExternalInputTag &tag) {
  append_token(out, tag.source);
  append_token(out, tag.as_of);
}

void append_market_cell(std::string &out, const CorpusMarketInputCell &cell) {
  append_token(out, cell.date);
  append_token(out, cell.symbol);
  append_optional_double(out, cell.spot_override);
  append_u64(out, cell.yc_pillar_t.size());
  for (std::size_t i = 0; i < cell.yc_pillar_t.size(); ++i) {
    append_u64(out, std::bit_cast<std::uint64_t>(cell.yc_pillar_t[i]));
    append_u64(out, std::bit_cast<std::uint64_t>(cell.yc_pillar_r[i]));
  }
  append_u64(out, cell.cash_divs.size());
  for (const DividendEvent &dividend : cell.cash_divs) {
    append_u64(out, static_cast<std::uint64_t>(dividend.ex_date_ns));
    append_u64(out, std::bit_cast<std::uint64_t>(dividend.amount));
  }
  append_u64(out, cell.fit_context.profile_override.has_value() ? 1u : 0u);
  if (cell.fit_context.profile_override.has_value()) {
    append_u64(out, static_cast<std::uint64_t>(*cell.fit_context.profile_override));
  }
  append_u64(out, static_cast<std::uint64_t>(cell.fit_context.session_phase));
  append_u64(out, static_cast<std::uint64_t>(cell.fit_context.event_phase));
  append_u64(out, cell.fit_context.event_distance_days.has_value() ? 1u : 0u);
  if (cell.fit_context.event_distance_days.has_value()) {
    append_u64(out, *cell.fit_context.event_distance_days);
  }
  append_optional_double(out, cell.fit_context.forward_dispersion_bp);
  append_optional_double(out, cell.fit_context.median_q_eff);
  append_u64(out, cell.fit_context.htb.has_value() ? 1u : 0u);
  if (cell.fit_context.htb.has_value()) {
    append_u64(out, *cell.fit_context.htb ? 1u : 0u);
  }
  append_u64(out, cell.fit_context.vol_product ? 1u : 0u);
  append_tag(out, cell.provenance.spot);
  append_tag(out, cell.provenance.rates);
  append_tag(out, cell.provenance.dividends);
  append_tag(out, cell.provenance.fit_context);
  append_u64(out, static_cast<std::uint64_t>(cell.provenance.dividend_treatment));
}

[[nodiscard]] std::uint64_t stable_hash(std::string_view bytes) noexcept {
  const std::uint64_t hash = atx::core::hash_bytes(bytes.data(), bytes.size());
  return hash == 0u ? 1u : hash;
}

[[nodiscard]] bool valid_market_cell(const CorpusMarketInputCell &cell) {
  Civil date;
  if (!parse_civil(cell.date, date) || cell.symbol.empty() ||
      (cell.spot_override.has_value() &&
       (!std::isfinite(*cell.spot_override) || *cell.spot_override <= 0.0)) ||
      cell.yc_pillar_t.size() != cell.yc_pillar_r.size() ||
      !valid_as_of(cell.provenance.spot, cell.date) ||
      !valid_as_of(cell.provenance.rates, cell.date) ||
      !valid_as_of(cell.provenance.dividends, cell.date) ||
      !valid_as_of(cell.provenance.fit_context, cell.date)) {
    return false;
  }
  double previous_t = 0.0;
  for (std::size_t i = 0; i < cell.yc_pillar_t.size(); ++i) {
    if (!std::isfinite(cell.yc_pillar_t[i]) || !std::isfinite(cell.yc_pillar_r[i]) ||
        cell.yc_pillar_t[i] <= previous_t) {
      return false;
    }
    previous_t = cell.yc_pillar_t[i];
  }
  for (const DividendEvent &dividend : cell.cash_divs) {
    if (dividend.ex_date_ns <= 0 || !std::isfinite(dividend.amount) || dividend.amount < 0.0) {
      return false;
    }
  }
  return !cell.fit_context.forward_dispersion_bp.has_value() ||
         std::isfinite(*cell.fit_context.forward_dispersion_bp);
}

} // namespace

Result<CorpusMarketInputTable>
CorpusMarketInputTable::create(std::vector<CorpusMarketInputCell> cells) {
  for (CorpusMarketInputCell &cell : cells) {
    cell.symbol = canonical_market_symbol(cell.symbol);
    std::sort(cell.cash_divs.begin(), cell.cash_divs.end(),
              [](const DividendEvent &lhs, const DividendEvent &rhs) {
                if (lhs.ex_date_ns != rhs.ex_date_ns) {
                  return lhs.ex_date_ns < rhs.ex_date_ns;
                }
                return lhs.amount < rhs.amount;
              });
    if (!valid_market_cell(cell) || (cell.fit_context.median_q_eff.has_value() &&
                                     !std::isfinite(*cell.fit_context.median_q_eff))) {
      return Err(ErrorCode::InvalidArgument, "invalid or future-dated corpus market input cell");
    }
  }
  std::sort(cells.begin(), cells.end(),
            [](const CorpusMarketInputCell &lhs, const CorpusMarketInputCell &rhs) {
              return lhs.date != rhs.date ? lhs.date < rhs.date : lhs.symbol < rhs.symbol;
            });
  for (std::size_t i = 1u; i < cells.size(); ++i) {
    if (cells[i - 1u].date == cells[i].date && cells[i - 1u].symbol == cells[i].symbol) {
      return Err(ErrorCode::InvalidArgument, "duplicate corpus market input cell");
    }
  }

  std::string table_bytes;
  for (CorpusMarketInputCell &cell : cells) {
    std::string cell_bytes;
    append_market_cell(cell_bytes, cell);
    cell.provenance.fingerprint = stable_hash(cell_bytes);
    table_bytes.append(cell_bytes);
  }
  CorpusMarketInputTable table;
  table.cells_ = std::move(cells);
  table.fingerprint_ = stable_hash(table_bytes);
  return Ok(std::move(table));
}

const CorpusMarketInputCell *CorpusMarketInputTable::find(std::string_view date,
                                                          std::string_view symbol) const {
  const std::string canonical = canonical_market_symbol(symbol);
  const auto it = std::lower_bound(
      cells_.begin(), cells_.end(), std::pair{date, std::string_view(canonical)},
      [](const CorpusMarketInputCell &cell, const auto &key) {
        return cell.date != key.first ? cell.date < key.first : cell.symbol < key.second;
      });
  return it != cells_.end() && it->date == date && it->symbol == canonical ? &*it : nullptr;
}

Result<OpraBatchResult> load_opra_daterange(const OpraBatchSpec& spec,
                                            const OpraBatchProgress& progress) {
  // ── Malformed-spec gate (the ONLY top-level errors) ──────────────────────
  if (spec.symbols.empty()) {
    return Err(ErrorCode::InvalidArgument, "OpraBatchSpec.symbols is empty");
  }
  Civil lo;
  Civil hi;
  if (!parse_civil(spec.date_lo, lo)) {
    return Err(ErrorCode::InvalidArgument, "unparseable date_lo '" + spec.date_lo + "'");
  }
  if (!parse_civil(spec.date_hi, hi)) {
    return Err(ErrorCode::InvalidArgument, "unparseable date_hi '" + spec.date_hi + "'");
  }
  const std::int64_t serial_lo = days_from_civil(lo.y, lo.m, lo.d);
  const std::int64_t serial_hi = days_from_civil(hi.y, hi.m, hi.d);
  if (serial_hi < serial_lo) {
    return Err(ErrorCode::InvalidArgument,
               "date_hi '" + spec.date_hi + "' precedes date_lo '" + spec.date_lo + "'");
  }
  if (spec.yc_pillar_t.size() != spec.yc_pillar_r.size()) {
    return Err(ErrorCode::InvalidArgument, "OpraBatchSpec.yc_pillar_t/_r length mismatch");
  }

  OpraBatchResult result;
  const std::size_t n_symbols = spec.symbols.size();
  const std::size_t n_dates = static_cast<std::size_t>(serial_hi - serial_lo + 1);
  // Checked size arithmetic: n_symbols * n_dates must not wrap size_t (a
  // pathological date range × symbol list would otherwise under-allocate the
  // pre-sized entries vector and corrupt the disjoint-slot invariant below).
  if (n_dates != 0 &&
      n_symbols > std::numeric_limits<std::size_t>::max() / n_dates) {
    return Err(ErrorCode::InvalidArgument,
               "OpraBatchSpec: symbols × dates overflows size_t");
  }
  result.n_total = n_symbols * n_dates;
  // Pre-size so the parallel per-file reads below write DISJOINT slots by index
  // (no push_back, no ordering dependency). Each slot is finalized either in the
  // serial pre-pass (quarantine) or by exactly one worker (the file read).
  result.entries.resize(result.n_total);

  // ── Serial pre-pass: resolve every cell, queue the files that need a read ──
  // Memoize date+suffix -> ts_ns: the symbols of one date share a single snapshot
  // stamp, so the M distinct stamps are parsed once (iso_to_ns) instead of N*M
  // times. Per-contract OSI parsing remains inside load_opra_cbbo_parquet. The
  // Error policy is a top-level Err detected HERE, before any parallel work; a
  // Quarantine cell is finalized in place (no read queued).
  std::unordered_map<std::string, std::int64_t> snap_ts_cache;

  struct LoadTask {
    std::size_t idx;    // slot in result.entries this task finalizes
    OpraLoadSpec load;  // fully resolved read spec (built serially, read parallel)
  };
  std::vector<LoadTask> load_plan;
  load_plan.reserve(result.n_total);

  std::size_t slot = 0;
  for (std::int64_t serial = serial_lo; serial <= serial_hi; ++serial) {
    const std::string date = format_civil(civil_from_days(serial));
    const std::string snapshot_iso = date + spec.snapshot_suffix;

    std::int64_t snap_ts = 0;
    if (const auto it = snap_ts_cache.find(snapshot_iso); it != snap_ts_cache.end()) {
      snap_ts = it->second;
    } else {
      snap_ts = iso_to_ns(snapshot_iso);
      snap_ts_cache.emplace(snapshot_iso, snap_ts);
    }

    for (const std::string& symbol : spec.symbols) {
      OpraBatchEntry& entry = result.entries[slot];
      entry.symbol = symbol;
      entry.date = date;
      entry.snapshot_ts_ns = snap_ts;
      fs::path path = fs::path(spec.root_dir) / apply_template(spec.path_template, symbol, date);
      path.make_preferred();
      entry.path = path.string();

      const CorpusMarketInputCell *market = spec.market_inputs.find(date, symbol);
      if (market == nullptr) {
        entry.used_market_input_fallback = true;
        if (spec.missing_market_inputs == MissingMarketInputPolicy::Error) {
          return Err(ErrorCode::Unavailable, "missing market inputs for " + date + " " + symbol);
        }
        if (spec.missing_market_inputs == MissingMarketInputPolicy::Quarantine) {
          entry.panel =
              Err(ErrorCode::Unavailable, "missing market inputs for " + date + " " + symbol);
          ++slot;
          continue;  // finalized in place; no parquet read queued
        }
      } else {
        entry.market_input_fingerprint = market->provenance.fingerprint;
      }

      OpraLoadSpec load;
      load.path = entry.path;
      load.underlying = symbol;
      load.snapshot_iso = snapshot_iso;
      load.r = spec.r;
      load.provenance_mode = spec.provenance_mode;
      if (market != nullptr) {
        load.spot_override = market->spot_override.value_or(0.0);
        load.yc_pillar_t = market->yc_pillar_t.empty() ? spec.yc_pillar_t : market->yc_pillar_t;
        load.yc_pillar_r = market->yc_pillar_r.empty() ? spec.yc_pillar_r : market->yc_pillar_r;
        load.cash_divs = market->cash_divs;
        load.fit_context = market->fit_context;
        load.market_input_provenance = market->provenance;
      } else {
        load.yc_pillar_t = spec.yc_pillar_t;
        load.yc_pillar_r = spec.yc_pillar_r;
      }
      load_plan.push_back(LoadTask{slot, std::move(load)});
      ++slot;
    }
  }

  // ── Parallel per-file read (W4.3) ─────────────────────────────────────────
  // Each task finalizes exactly one pre-sized entry slot (disjoint) after pure
  // reads of its own resolved OpraLoadSpec. load_opra_cbbo_parquet reads a
  // DISTINCT file per task and holds no shared mutable state (a fresh Arrow
  // reader per call; the date/OSI parse helpers are pure), so the outcome is
  // independent of worker count / claim order. n_threads: 0 = auto, 1 = serial.
  parallel_for_dynamic(load_plan.size(), spec.n_threads, [&](std::size_t k) {
    const LoadTask& task = load_plan[k];
    OpraBatchEntry& entry = result.entries[task.idx];
    std::error_code ec;
    const bool present = fs::exists(fs::path(task.load.path), ec) && !ec;
    if (!present) {
      entry.panel = Err(ErrorCode::NotFound, "no parquet at '" + entry.path + "'");
    } else {
      entry.panel = load_opra_cbbo_parquet(task.load);
    }
  });

  // ── Serial post-join: deterministic counters + in-order progress ──────────
  // Counters are counted FROM the completed slots (never mutated in the parallel
  // loop), so they are independent of worker count / completion order and
  // partition the entries: n_loaded + n_missing + n_error == n_total. A NotFound
  // panel is a missing file; any other Err (quarantine / load failure) is
  // n_error. Progress fires once per cell in monotonic index order, preserving
  // the callback's `done` contract.
  std::size_t done = 0;
  for (const OpraBatchEntry& entry : result.entries) {
    if (entry.panel.has_value()) {
      ++result.n_loaded;
    } else if (entry.panel.error().code() == ErrorCode::NotFound) {
      ++result.n_missing;
    } else {
      ++result.n_error;
    }
    ++done;
    if (progress) {
      progress(done, result.n_total, entry);
    }
  }

  return Ok(std::move(result));
}

MarketEnv market_env_from_frame(const QuoteFrame& frame) {
  // Flat rate the no/one-pillar path uses (the sole pillar's rate, else 0).
  const double flat_r = frame.yc_pillar_r.empty() ? 0.0 : frame.yc_pillar_r.front();

  // >= 2 equal-length pillars => a real term curve; each date's chain then sees
  // its own rate_at(T). A single pillar interpolates as a CONSTANT discount
  // factor (not a flat rate), so it is treated as flat — matching the loader.
  if (frame.yc_pillar_t.size() >= 2 &&
      frame.yc_pillar_t.size() == frame.yc_pillar_r.size()) {
    Result<YieldCurve> yc = YieldCurve::create(frame.yc_pillar_t, frame.yc_pillar_r);
    if (yc.has_value()) {
      MarketEnv env =
          MarketEnv::flat(frame.spot, flat_r, frame.snapshot_ts_ns, frame.divs);
      env.yield = std::move(yc.value());  // rate_at(T>0) now interpolates
      return env;
    }
    // create() failed (e.g. non-ascending pillars): fall back to flat.
  }
  return MarketEnv::flat(frame.spot, flat_r, frame.snapshot_ts_ns, frame.divs);
}

CorpusBoard corpus_board_from_opra(std::string date, std::string symbol, OpraPanel panel) {
  CorpusBoard board;
  board.date = std::move(date);
  board.symbol = std::move(symbol);
  board.frame = std::move(panel.frame);
  board.env = market_env_from_frame(board.frame);
  board.fit_context = panel.fit_context;
  board.source_provenance_complete = panel.provenance_complete;
  board.source_schema_version = panel.source_schema_version;
  board.source_fingerprint = panel.source_fingerprint;
  board.market_input_fingerprint = panel.market_input_provenance.fingerprint;
  return board;
}

} // namespace atx::vol
