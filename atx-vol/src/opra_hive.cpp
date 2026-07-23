#include "atx/vol/opra_hive.hpp"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <utility>
#include <vector>

#include "atx/core/error.hpp"        // Ok, Err, ErrorCode
#include "atx/core/io/parquet.hpp"   // read_parquet, ParquetTable
#include "atx/vol/opra_panel.hpp"    // OpraLoadSpec, load_opra_cbbo_from_table
#include "atx/vol/parallel_for.hpp"  // parallel_for_dynamic (W4.3 per-date fan-out)
#include "opra_batch_detail.hpp"     // Civil kernel, memo_iso_to_ns, resolve_market_inputs

namespace atx::vol {

using atx::core::Err;
using atx::core::ErrorCode;
using atx::core::Ok;

namespace {

namespace fs = std::filesystem;
namespace io = atx::core::io;
namespace obd = atx::vol::opra_detail;

// The frozen v2 date-file name (write_hive_parquet hard-codes it).
constexpr std::string_view kDateFileName = "data.parquet";

// `<root>/date=<YYYY-MM-DD>/data.parquet`, in the host's preferred separator.
[[nodiscard]] std::string hive_date_path(const std::string& root_dir, const std::string& date) {
  fs::path path = fs::path(root_dir) / ("date=" + date) / kDateFileName;
  path.make_preferred();
  return path.string();
}

// The sorted distinct `underlying` set of an already-read date table (discovery
// mode). Copies the borrowed string-views into owned strings so the caller may
// then move the table on.
[[nodiscard]] std::vector<std::string> distinct_underlyings(const io::ParquetTable& table) {
  std::vector<std::string> out;
  const auto und = table.strings("underlying");
  if (!und.has_value()) {
    return out; // caller checks the column separately; empty => nothing to load
  }
  out.reserve(und->size());
  for (const std::string_view sv : *und) {
    out.emplace_back(sv);
  }
  std::sort(out.begin(), out.end());
  out.erase(std::unique(out.begin(), out.end()), out.end());
  return out;
}

// One present date's read + per-symbol split, deferred to the parallel pass.
// `table` is engaged only in discovery mode (the cached pre-pass read, reused
// here); in explicit mode it is nullopt and the worker performs the single read.
struct SymbolLoad {
  std::size_t slot;  // index into result.entries this load finalizes (disjoint)
  OpraLoadSpec load; // fully resolved read spec (built serially, read in parallel)
};

struct DateReadTask {
  std::string path;
  std::optional<io::ParquetTable> table; // discovery: cached read; explicit: nullopt
  std::vector<SymbolLoad> loads;
};

} // namespace

Result<OpraBatchResult> load_opra_hive(const OpraHiveSpec& spec, const OpraBatchProgress& progress) {
  // ── Malformed-spec gate (the ONLY top-level errors) ──────────────────────
  if (spec.root_dir.empty()) {
    return Err(ErrorCode::InvalidArgument, "OpraHiveSpec.root_dir is empty");
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
    return Err(ErrorCode::InvalidArgument, "OpraHiveSpec.yc_pillar_t/_r length mismatch");
  }

  const bool discover = spec.symbols.empty();

  OpraBatchResult result;
  std::unordered_map<std::string, std::int64_t> snap_ts_cache;
  std::vector<DateReadTask> tasks;

  // ── Serial pre-pass: enumerate dates, size entries, resolve market inputs ──
  // Missing/quarantined cells are finalized in place here; present dates queue a
  // DateReadTask. In discovery mode this pass also performs the ONE materialized
  // read per present date (to discover its underlyings) and caches it for reuse
  // by the panel pass. Entry order is fixed here (date-major, then symbol-major),
  // BEFORE any parallel work, so it is independent of worker count.
  for (std::int64_t serial = serial_lo; serial <= serial_hi; ++serial) {
    const std::string date = obd::format_civil(obd::civil_from_days(serial));
    const std::string snapshot_iso = date + spec.snapshot_suffix;
    const std::int64_t snap_ts = obd::memo_iso_to_ns(snap_ts_cache, snapshot_iso);
    const std::string path = hive_date_path(spec.root_dir, date);

    std::error_code ec;
    const bool present = fs::exists(fs::path(path), ec) && !ec;

    // Finalize one absent/anonymous entry in place (no read queued).
    const auto emit_finalized = [&](std::string symbol, tl::unexpected<atx::core::Error> err) {
      OpraBatchEntry entry;
      entry.symbol = std::move(symbol);
      entry.date = date;
      entry.path = path;
      entry.snapshot_ts_ns = snap_ts;
      entry.panel = std::move(err);
      result.entries.push_back(std::move(entry));
    };

    if (!present) {
      // Absent file: NotFound. Explicit symbols -> one NotFound per requested
      // symbol; discovery -> a single anonymous NotFound entry.
      if (discover) {
        emit_finalized("", Err(ErrorCode::NotFound, "no parquet at '" + path + "'"));
      } else {
        for (const std::string& symbol : spec.symbols) {
          emit_finalized(symbol, Err(ErrorCode::NotFound, "no parquet at '" + path + "'"));
        }
      }
      continue;
    }

    // Present: determine this date's symbol list (and, in discovery mode, cache
    // the single read).
    std::vector<std::string> date_symbols;
    std::optional<io::ParquetTable> cached;
    if (discover) {
      Result<io::ParquetTable> tbl = io::read_parquet(path);
      if (!tbl.has_value()) {
        // Present but unreadable: cannot discover -> a single anonymous error
        // entry (NOT NotFound; the file exists). Counts toward n_error.
        emit_finalized("", Err(tbl.error()));
        continue;
      }
      if (!tbl->schema().find("underlying")) {
        emit_finalized("", Err(ErrorCode::InvalidArgument,
                               "hive date file '" + path + "' has no 'underlying' column"));
        continue;
      }
      date_symbols = distinct_underlyings(*tbl);
      cached = std::move(*tbl);
    } else {
      date_symbols = spec.symbols; // explicit, given order
    }

    // Build entries + queue the reads for this present date.
    DateReadTask task;
    task.path = path;
    if (cached.has_value()) {
      task.table = std::move(cached);
    }
    for (const std::string& symbol : date_symbols) {
      const std::size_t slot = result.entries.size();
      OpraBatchEntry entry;
      entry.symbol = symbol;
      entry.date = date;
      entry.path = path;
      entry.snapshot_ts_ns = snap_ts;

      OpraLoadSpec load;
      load.path = path;
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
        result.entries.push_back(std::move(entry)); // entry.panel already set
        continue;                                   // no read queued
      }
      task.loads.push_back(SymbolLoad{slot, std::move(load)});
      result.entries.push_back(std::move(entry));
    }
    if (!task.loads.empty()) {
      tasks.push_back(std::move(task));
    }
  }
  result.n_total = result.entries.size();

  // ── Parallel per-DATE panel pass (W4.3) ───────────────────────────────────
  // Each task owns a DISJOINT set of pre-sized entry slots and writes only its
  // own. The date file is read exactly once per task — the reused discovery read
  // (discovery mode) or a fresh read here (explicit mode) — then split per
  // underlying via the pure `load_opra_cbbo_from_table` seam. read_parquet /
  // LazyParquet hold no shared mutable state (distinct files, a fresh per-call
  // reader), so the outcome is independent of worker count / claim order.
  parallel_for_dynamic(tasks.size(), spec.n_threads, [&](std::size_t k) {
    const DateReadTask& task = tasks[k];
    if (task.table.has_value()) {
      const io::ParquetTable& tbl = *task.table;
      for (const SymbolLoad& sl : task.loads) {
        result.entries[sl.slot].panel = load_opra_cbbo_from_table(tbl, sl.load);
      }
      return;
    }
    Result<io::ParquetTable> fresh = io::read_parquet(task.path);
    if (!fresh.has_value()) {
      // Present-but-unreadable date: every requested symbol for it errors (not
      // NotFound — the file exists), so the date lands in n_error, not n_missing.
      for (const SymbolLoad& sl : task.loads) {
        result.entries[sl.slot].panel = Err(fresh.error());
      }
      return;
    }
    for (const SymbolLoad& sl : task.loads) {
      result.entries[sl.slot].panel = load_opra_cbbo_from_table(*fresh, sl.load);
    }
  });

  // ── Serial post-join: deterministic counters + in-order progress ──────────
  // Counted FROM the completed slots (never mutated in the parallel loop), so the
  // counters are independent of worker count / completion order and partition the
  // entries: n_loaded + n_missing + n_error == n_total. A NotFound panel is a
  // missing file; any other Err (quarantine / read / load failure) is n_error.
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

} // namespace atx::vol
