#include "atx/vol/api/marketdata/opra_batch.hpp"

#include <algorithm>
#include <bit>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <utility>
#include <vector>

#include "atx/core/error.hpp"       // Ok, Err, ErrorCode
#include "atx/core/hash.hpp"        // hash_bytes
#include "atx/vol/api/marketdata/data.hpp"         // iso_to_ns
#include "core/parallel_for.hpp" // parallel_for_dynamic (W4.3 per-file fan-out)
#include "atx/vol/api/pricing/rates_curve.hpp"  // YieldCurve
#include "marketdata/opra_batch_detail.hpp"    // Civil kernel, memo_iso_to_ns, resolve_market_inputs

namespace atx::vol {

using atx::core::Ok;
using atx::core::Err;
using atx::core::ErrorCode;

namespace {

namespace fs = std::filesystem;
namespace obd = atx::vol::opra_detail;

// The civil-date kernel (Civil, days_from_civil / civil_from_days, parse_civil,
// format_civil), the snapshot-stamp memoization, and the per-cell market-input
// resolution now live in opra_batch_detail.hpp so load_opra_hive reuses them
// VERBATIM instead of copy-pasting — the two loaders must resolve dates, stamps,
// and market inputs identically. Referenced below via the `obd::` alias.

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
  obd::Civil cell;
  obd::Civil as_of;
  if (!obd::parse_civil(cell_date, cell) ||
      !obd::parse_civil(std::string_view(tag.as_of).substr(0u, 10u), as_of)) {
    return false;
  }
  return obd::days_from_civil(as_of.y, as_of.m, as_of.d) <=
         obd::days_from_civil(cell.y, cell.m, cell.d);
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
  obd::Civil date;
  if (!obd::parse_civil(cell.date, date) || cell.symbol.empty() ||
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

namespace {

// Split on `sep`, KEEPING empty fields: a blank column is a malformed row, not
// an absent one, and the caller must be able to say so.
[[nodiscard]] std::vector<std::string_view> split_keep_empty(std::string_view text, char sep) {
  std::vector<std::string_view> out;
  std::size_t begin = 0;
  while (true) {
    const std::size_t at = text.find(sep, begin);
    if (at == std::string_view::npos) {
      out.push_back(text.substr(begin));
      return out;
    }
    out.push_back(text.substr(begin, at - begin));
    begin = at + 1u;
  }
}

// Strip a trailing CR so a CRLF-written artifact parses identically to an LF one.
[[nodiscard]] std::string_view strip_cr(std::string_view line) noexcept {
  if (!line.empty() && line.back() == '\r') {
    line.remove_suffix(1u);
  }
  return line;
}

// Whole-field finite-double parse. `from_chars`'s floating-point overload is not
// available in every standard library this repo builds against; `strtod` over a
// NUL-terminated copy is, and this runs once per artifact row, never per quote.
[[nodiscard]] bool parse_finite_double(std::string_view text, double &out) {
  const std::string owned(text);
  char *end = nullptr;
  const double parsed = std::strtod(owned.c_str(), &end);
  if (owned.empty() || end != owned.c_str() + owned.size() || !std::isfinite(parsed)) {
    return false;
  }
  out = parsed;
  return true;
}

} // namespace

Result<CorpusMarketInputTable> read_corpus_spot_inputs(const std::string &path,
                                                       CorpusMarketInputTable base) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    return Err(ErrorCode::InvalidArgument, "cannot open spot inputs '" + path + "'");
  }
  const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  const std::vector<std::string_view> lines = split_keep_empty(text, '\n');
  if (lines.size() < 2u) {
    return Err(ErrorCode::ParseError, "spot inputs are missing magic/header");
  }
  if (strip_cr(lines[0]) != "ATX_CORPUS_SPOTS\t1" ||
      strip_cr(lines[1]) != "date\tsymbol\tspot\tsource\tas_of") {
    return Err(ErrorCode::ParseError, "spot inputs magic/header mismatch");
  }

  // Start from `base`'s cells so the overlay is a field write on an existing cell
  // rather than a second table every consumer would have to learn to consult.
  std::vector<CorpusMarketInputCell> cells(base.cells().begin(), base.cells().end());
  const auto find_cell = [&cells](std::string_view date,
                                  std::string_view symbol) -> CorpusMarketInputCell * {
    for (CorpusMarketInputCell &cell : cells) {
      if (cell.date == date && cell.symbol == symbol) {
        return &cell;
      }
    }
    return nullptr;
  };

  for (std::size_t i = 2u; i < lines.size(); ++i) {
    const std::string_view line = strip_cr(lines[i]);
    if (line.empty()) {
      continue;
    }
    const std::vector<std::string_view> fields = split_keep_empty(line, '\t');
    double spot = 0.0;
    if (fields.size() != 5u || fields[0].empty() || fields[1].empty() ||
        !parse_finite_double(fields[2], spot) || !(spot > 0.0) || fields[3].empty() ||
        fields[4].empty()) {
      return Err(ErrorCode::ParseError, "malformed spot input row: '" + std::string(line) + "'");
    }
    const std::string date(fields[0]);
    const std::string symbol = canonical_market_symbol(fields[1]);
    CorpusMarketInputCell *cell = find_cell(date, symbol);
    if (cell == nullptr) {
      CorpusMarketInputCell fresh;
      fresh.date = date;
      fresh.symbol = symbol;
      // The three tags this artifact does not speak for. `create` requires all
      // four present and non-look-ahead; these mirror the dividend artifact's
      // fillers so the two produce comparable provenance.
      const std::string as_of_date = date + "T00:00:00Z";
      fresh.provenance.rates = {"run_spec.flat_rate", as_of_date};
      fresh.provenance.dividends = {"run_spec.no_dividends", as_of_date};
      fresh.provenance.fit_context = {"run_spec.default", as_of_date};
      cells.push_back(std::move(fresh));
      cell = &cells.back();
    } else if (cell->spot_override.has_value()) {
      return Err(ErrorCode::AlreadyExists, "spot input for " + date + " " + symbol +
                                               " collides with a spot already supplied by the "
                                               "base market-input table");
    }
    cell->spot_override = spot;
    cell->provenance.spot = {std::string(fields[3]), std::string(fields[4])};
  }
  return CorpusMarketInputTable::create(std::move(cells));
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
  obd::Civil lo;
  obd::Civil hi;
  if (!obd::parse_civil(spec.date_lo, lo)) {
    return Err(ErrorCode::InvalidArgument, "unparseable date_lo '" + spec.date_lo + "'");
  }
  if (!obd::parse_civil(spec.date_hi, hi)) {
    return Err(ErrorCode::InvalidArgument, "unparseable date_hi '" + spec.date_hi + "'");
  }
  const std::int64_t serial_lo = obd::days_from_civil(lo.y, lo.m, lo.d);
  const std::int64_t serial_hi = obd::days_from_civil(hi.y, hi.m, hi.d);
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
    const std::string date = obd::format_civil(obd::civil_from_days(serial));
    const std::string snapshot_iso = date + spec.snapshot_suffix;
    const std::int64_t snap_ts = obd::memo_iso_to_ns(snap_ts_cache, snapshot_iso);

    for (const std::string& symbol : spec.symbols) {
      OpraBatchEntry& entry = result.entries[slot];
      entry.symbol = symbol;
      entry.date = date;
      entry.snapshot_ts_ns = snap_ts;
      fs::path path = fs::path(spec.root_dir) / apply_template(spec.path_template, symbol, date);
      path.make_preferred();
      entry.path = path.string();

      OpraLoadSpec load;
      load.path = entry.path;
      load.underlying = symbol;
      load.snapshot_iso = snapshot_iso;
      load.r = spec.r;
      load.provenance_mode = spec.provenance_mode;

      const obd::MarketResolve mr =
          obd::resolve_market_inputs(spec.market_inputs, spec.missing_market_inputs, date,
                                        symbol, spec.yc_pillar_t, spec.yc_pillar_r, entry, load);
      if (mr.kind == obd::MarketResolveKind::Fatal) {
        return Err(ErrorCode::Unavailable, mr.message);
      }
      if (mr.kind == obd::MarketResolveKind::Quarantine) {
        ++slot;
        continue;  // finalized in place; no parquet read queued
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
      // Sub-counts of a LOADED cell, not a fourth bucket: an uncoverable expiry
      // costs that expiry, never the symbol (opra_panel.hpp). Counted here so a
      // silently-shortened long end is visible in the batch summary.
      if (entry.panel->n_dropped_uncovered_expiry > 0) {
        result.n_uncovered_expiry_rows += entry.panel->n_dropped_uncovered_expiry;
        ++result.n_cells_with_uncovered_expiries;
      }
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

Status load_opra_date_windows(const OpraBatchSpec &spec, std::size_t date_window_size,
                              const OpraWindowConsumer &consume,
                              OpraWindowLoadStats *stats) {
  if (date_window_size == 0u) {
    return Err(ErrorCode::InvalidArgument, "OPRA date window size must be positive");
  }
  if (!consume) {
    return Err(ErrorCode::InvalidArgument, "OPRA date window consumer is empty");
  }
  obd::Civil lo;
  obd::Civil hi;
  if (!obd::parse_civil(spec.date_lo, lo)) {
    return Err(ErrorCode::InvalidArgument, "unparseable date_lo '" + spec.date_lo + "'");
  }
  if (!obd::parse_civil(spec.date_hi, hi)) {
    return Err(ErrorCode::InvalidArgument, "unparseable date_hi '" + spec.date_hi + "'");
  }
  const std::int64_t serial_lo = obd::days_from_civil(lo.y, lo.m, lo.d);
  const std::int64_t serial_hi = obd::days_from_civil(hi.y, hi.m, hi.d);
  if (serial_hi < serial_lo) {
    return Err(ErrorCode::InvalidArgument,
               "date_hi '" + spec.date_hi + "' precedes date_lo '" + spec.date_lo + "'");
  }

  OpraWindowLoadStats measured;
  std::int64_t window_lo = serial_lo;
  while (window_lo <= serial_hi) {
    const std::uint64_t dates_remaining =
        static_cast<std::uint64_t>(serial_hi - window_lo) + 1u;
    const std::uint64_t window_width =
        std::min(dates_remaining, static_cast<std::uint64_t>(date_window_size));
    const std::int64_t window_hi =
        window_lo + static_cast<std::int64_t>(window_width) - 1;
    OpraBatchSpec window = spec;
    window.date_lo = obd::format_civil(obd::civil_from_days(window_lo));
    window.date_hi = obd::format_civil(obd::civil_from_days(window_hi));

    const std::clock_t cpu_begin = std::clock();
    const auto wall_begin = std::chrono::steady_clock::now();
    ATX_TRY(OpraBatchResult batch, load_opra_daterange(window));
    measured.load_wall_s +=
        std::chrono::duration<double>(std::chrono::steady_clock::now() - wall_begin).count();
    const std::clock_t cpu_end = std::clock();
    if (cpu_begin != static_cast<std::clock_t>(-1) &&
        cpu_end != static_cast<std::clock_t>(-1) && cpu_end >= cpu_begin) {
      measured.load_process_cpu_s +=
          static_cast<double>(cpu_end - cpu_begin) / static_cast<double>(CLOCKS_PER_SEC);
    }
    ++measured.n_windows;
    measured.peak_entries = std::max(measured.peak_entries, batch.entries.size());
    ATX_TRY_VOID(consume(std::move(batch)));
    window_lo = window_hi + 1;
  }
  if (stats != nullptr) {
    *stats = measured;
  }
  return Ok();
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
