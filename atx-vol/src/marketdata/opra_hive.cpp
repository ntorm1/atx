#include "atx/vol/api/marketdata/opra_hive.hpp"

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

#include "atx/core/error.hpp"        // Ok, Err, ErrorCode, Error
#include "atx/core/io/parquet.hpp"   // read_parquet, ParquetTable
#include "atx/vol/api/marketdata/opra_panel.hpp"    // OpraLoadSpec, scan_opra_cbbo_table, load_opra_cbbo_from_scan
#include "core/parallel_for.hpp"  // parallel_for_dynamic (W4.3 per-date fan-out)
#include "marketdata/opra_batch_detail.hpp"     // Civil kernel, memo_iso_to_ns, resolve_market_inputs

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
// then move the table on. Err when the column is absent OR not string-typed
// (`strings()` fails) — the caller surfaces that as a date-level read failure
// rather than silently dropping the date.
[[nodiscard]] Result<std::vector<std::string>> distinct_underlyings(const io::ParquetTable& table) {
  const auto und = table.strings("underlying");
  if (!und.has_value()) {
    return Err(und.error());
  }
  std::vector<std::string> out;
  out.reserve(und->size());
  for (const std::string_view sv : *und) {
    out.emplace_back(sv);
  }
  std::sort(out.begin(), out.end());
  out.erase(std::unique(out.begin(), out.end()), out.end());
  return Ok(std::move(out));
}

// One calendar date's pre-pass state. In discovery mode a present, readable file
// is read ONCE here (to contribute to the union) and its table is cached in
// `table` for reuse by the panel pass; an unreadable present file records the
// failure in `read_error` so every one of its cells surfaces it.
struct DateInfo {
  std::string date;
  std::string snapshot_iso;
  std::int64_t snapshot_ts_ns{0};
  std::string path;
  bool present{false};
  std::optional<io::ParquetTable> table{};    // discovery cached read (present & OK)
  std::optional<atx::core::Error> read_error{}; // discovery: present but unreadable
  // Discovery mode only: this date file's own distinct `underlying` set, sorted
  // and unique (the very set its contribution to the union came from). Engaged
  // exactly when `table` is — a symbol absent from it is a COVERAGE HOLE, not a
  // load failure.
  std::vector<std::string> symbols_present{};
};

// The Err a cell gets when its symbol is simply not in a present, readable date
// file. Byte-identical to what the table seam (`panel_from_table`'s zero-match
// branch) returns, so classifying the hole structurally instead of by calling the
// seam changes nothing an `.error()` reader can observe.
[[nodiscard]] atx::core::Error coverage_hole_error(const std::string& symbol) {
  return atx::core::Error{ErrorCode::InvalidArgument,
                          "underlying '" + symbol + "' not found in parquet"};
}

// One present date's per-symbol split, deferred to the parallel pass. `table` is
// engaged only in discovery mode (the cached pre-pass read, reused here); in
// explicit mode it is nullopt and the worker performs the single read.
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

  // ── Phase A: enumerate the calendar range; discover the symbol UNION ───────
  // Every date is stat'd. In discovery mode each PRESENT file is read ONCE here
  // (to contribute its underlyings to the union U) and the table is cached for
  // reuse by the panel pass — one materialized read per date. The union is
  // resolved BEFORE any entries are built so the entry grid is rectangular and
  // globally deterministic (date-major × the sorted union), independent of which
  // date carries which symbol.
  std::unordered_map<std::string, std::int64_t> snap_ts_cache;
  std::vector<DateInfo> days;
  std::vector<std::string> discovered; // accumulates U (unsorted, with dups)
  for (std::int64_t serial = serial_lo; serial <= serial_hi; ++serial) {
    DateInfo di;
    di.date = obd::format_civil(obd::civil_from_days(serial));
    di.snapshot_iso = di.date + spec.snapshot_suffix;
    di.snapshot_ts_ns = obd::memo_iso_to_ns(snap_ts_cache, di.snapshot_iso);
    di.path = hive_date_path(spec.root_dir, di.date);

    std::error_code ec;
    di.present = fs::exists(fs::path(di.path), ec) && !ec;

    if (discover && di.present) {
      Result<io::ParquetTable> tbl = io::read_parquet(di.path);
      if (!tbl.has_value()) {
        di.read_error = tbl.error(); // present but unreadable (corrupt/truncated)
      } else if (!tbl->schema().find("underlying")) {
        di.read_error = atx::core::Error{
            ErrorCode::InvalidArgument, "hive date file '" + di.path + "' has no 'underlying' column"};
      } else {
        Result<std::vector<std::string>> syms = distinct_underlyings(*tbl);
        if (!syms.has_value()) {
          di.read_error = syms.error(); // e.g. `underlying` present but not string-typed
        } else {
          for (const std::string& s : *syms) {
            discovered.push_back(s);
          }
          // Retained (sorted+unique, as distinct_underlyings returns it) so phase
          // B can tell a COVERAGE HOLE — a union symbol this date simply does not
          // carry — from a genuine load failure, without inspecting error codes.
          di.symbols_present = std::move(*syms);
          di.table = std::move(*tbl);
        }
      }
    }
    days.push_back(std::move(di));
  }

  // Effective symbol list: the requested symbols (explicit, given order) or the
  // sorted distinct union across all present, readable dates (discovery).
  std::vector<std::string> effective;
  if (discover) {
    std::sort(discovered.begin(), discovered.end());
    discovered.erase(std::unique(discovered.begin(), discovered.end()), discovered.end());
    effective = std::move(discovered);
  } else {
    effective = spec.symbols;
  }
  // Degenerate discovery: no symbols were discoverable from ANY date (the range
  // holds no readable file). There is no union to make a grid over, so each date
  // contributes a single anonymous entry rather than being silently dropped.
  const bool degenerate = discover && effective.empty();

  // ── Phase B: build entries date-major × `effective` (rectangular) ─────────
  // Missing/quarantined/coverage-hole cells are finalized in place; present,
  // readable cells queue a load. A symbol absent from a present date's file is a
  // VISIBLE coverage hole (counted in n_error, and in n_coverage_holes): in
  // DISCOVERY mode the pre-pass already knows this date's symbol set, so the hole
  // is finalized right here with the table seam's exact zero-match Err and NO read
  // is queued; in explicit mode the set is only known in the panel pass, which
  // classifies it there. Either way the cell carries the same Err an explicit
  // request for that symbol always did. Entry order is fixed here, before any
  // parallel work, so it is independent of worker count.
  OpraBatchResult result;
  std::vector<DateReadTask> tasks;
  for (DateInfo& di : days) {
    if (degenerate) {
      OpraBatchEntry entry;
      entry.symbol = "";
      entry.date = di.date;
      entry.path = di.path;
      entry.snapshot_ts_ns = di.snapshot_ts_ns;
      if (!di.present) {
        entry.panel = Err(ErrorCode::NotFound, "no parquet at '" + di.path + "'");
      } else if (di.read_error.has_value()) {
        entry.panel = Err(*di.read_error);
      } else {
        entry.panel = Err(ErrorCode::Unavailable,
                          "hive date file '" + di.path + "' has no underlyings to discover");
      }
      result.entries.push_back(std::move(entry));
      continue;
    }

    DateReadTask task;
    task.path = di.path;
    if (di.table.has_value()) {
      task.table = std::move(di.table); // discovery cached read, reused below
    }
    for (const std::string& symbol : effective) {
      const std::size_t slot = result.entries.size();
      OpraBatchEntry entry;
      entry.symbol = symbol;
      entry.date = di.date;
      entry.path = di.path;
      entry.snapshot_ts_ns = di.snapshot_ts_ns;

      if (!di.present) {
        // Absent file: NotFound for every symbol of the (rectangular) grid.
        entry.panel = Err(ErrorCode::NotFound, "no parquet at '" + di.path + "'");
        result.entries.push_back(std::move(entry));
        continue;
      }
      if (di.read_error.has_value()) {
        // Discovery: the whole date was unreadable -> every cell surfaces it.
        entry.panel = Err(*di.read_error);
        result.entries.push_back(std::move(entry));
        continue;
      }
      // Discovery: the file is present AND readable and we already hold its own
      // distinct-underlying set, so a symbol absent from it is a COVERAGE HOLE by
      // construction — finalize it here with the seam's exact zero-match Err and
      // queue no read (calling the seam merely to be handed that error back is
      // wasted work). Classified structurally, so the counter never has to infer
      // "sparse universe" vs "broken file" from an error code.
      if (di.table.has_value() &&
          !std::binary_search(di.symbols_present.begin(), di.symbols_present.end(), symbol)) {
        entry.coverage_hole = true;
        entry.panel = Err(coverage_hole_error(symbol));
        result.entries.push_back(std::move(entry));
        continue;
      }

      OpraLoadSpec load;
      load.path = di.path;
      load.underlying = symbol;
      load.snapshot_iso = di.snapshot_iso;
      load.r = spec.r;
      load.provenance_mode = spec.provenance_mode;

      const obd::MarketResolve mr =
          obd::resolve_market_inputs(spec.market_inputs, spec.missing_market_inputs, di.date,
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
  // underlying via the pure `load_opra_cbbo_from_table` seam (a symbol absent
  // from the table yields a zero-match Err for that slot). read_parquet /
  // LazyParquet hold no shared mutable state (distinct files, a fresh per-call
  // reader), so the outcome is independent of worker count / claim order.
  // Split one date's table across its queued symbols, classifying a symbol the
  // file does not carry as a COVERAGE HOLE instead of calling the seam for the
  // error. `present` is the file's own distinct-underlying set (sorted+unique) or
  // EMPTY-AND-UNKNOWN when it could not be computed (no/!string `underlying`
  // column) — in which case nothing is a hole and every cell goes to the seam, so
  // a schema-broken file still surfaces its real error on every cell.
  //
  // ── P-01: ONE column scan + row index per DATE, not per symbol ─────────────
  // `scan_opra_cbbo_table` (opra_panel.hpp) materializes each column once and
  // indexes the rows by `underlying`, so each symbol below costs O(ITS rows)
  // instead of O(the table's) -- the per-symbol seam used to re-materialize
  // every column and re-walk every other underlying's rows, 102 times on a
  // 102-name date, and (worse) size every retained buffer for the whole table.
  // Scanning once also makes the loads independent of each other, which is what
  // lets `n_threads` stay a pure perf knob.
  //
  // The 8-column check the per-symbol seam did lives in the scan, so a
  // schema-broken date reports the SAME error on every cell it would have before
  // -- `spec.path` is the date file, identical for every symbol of the date.
  // ORDER IS LOAD-BEARING: the hole check stays AHEAD of the scan's error, so a
  // file that is both schema-broken and missing a requested symbol still reads
  // that symbol as a hole and the rest as defects (the header's contract; gated
  // by OpraHive.SchemaBrokenFileStillReportsHoleAndDefectSeparately).
  const auto split_table = [&](const io::ParquetTable& tbl, const DateReadTask& task,
                               const std::vector<std::string>* present) {
    const Result<OpraTableScan> scan = scan_opra_cbbo_table(tbl, task.path);
    for (const SymbolLoad& sl : task.loads) {
      OpraBatchEntry& entry = result.entries[sl.slot];
      if (present != nullptr &&
          !std::binary_search(present->begin(), present->end(), entry.symbol)) {
        entry.coverage_hole = true;
        entry.panel = Err(coverage_hole_error(entry.symbol));
        continue;
      }
      if (!scan.has_value()) {
        entry.panel = Err(scan.error());
        continue;
      }
      entry.panel = load_opra_cbbo_from_scan(*scan, sl.load);
    }
  };

  parallel_for_dynamic(tasks.size(), spec.n_threads, [&](std::size_t k) {
    DateReadTask& task = tasks[k];
    if (task.table.has_value()) {
      // Discovery mode: holes were already finalized serially in phase B from the
      // pre-pass symbol set, so every queued load here is a symbol the file has.
      split_table(*task.table, task, nullptr);
      // P-01: release this date's decoded table the moment its panels exist,
      // rather than holding every date's table until the whole window is done.
      // Each task is claimed by exactly one worker and nothing reads `task`
      // afterwards, so this is a private write. (`split_table` has returned, so
      // the reference it took is dead.)
      task.table.reset();
      return;
    }
    Result<io::ParquetTable> fresh = io::read_parquet(task.path);
    if (!fresh.has_value()) {
      // Present-but-unreadable date (explicit mode): every requested symbol for it
      // errors (not NotFound — the file exists), so it lands in n_error.
      for (const SymbolLoad& sl : task.loads) {
        result.entries[sl.slot].panel = Err(fresh.error());
      }
      return;
    }
    // Explicit mode: phase A never read this file, so compute its distinct
    // underlyings ONCE here (one extra column scan per date, cheap next to panel
    // construction) and classify holes from it. Purely per-date and derived from
    // the task's own table, so it stays worker-count independent.
    const Result<std::vector<std::string>> present = distinct_underlyings(*fresh);
    split_table(*fresh, task, present.has_value() ? &*present : nullptr);
  });

  // ── Serial post-join: deterministic counters + in-order progress ──────────
  // Counted FROM the completed slots (never mutated in the parallel loop), so the
  // counters are independent of worker count / completion order and partition the
  // entries: n_loaded + n_missing + n_error == n_total. A NotFound panel is a
  // missing file; any other Err (quarantine / read / load / coverage-hole
  // failure) is n_error. `n_coverage_holes` is counted INSIDE that last branch,
  // so it is a sub-count of n_error by construction and the partition is
  // untouched.
  std::size_t done = 0;
  for (const OpraBatchEntry& entry : result.entries) {
    if (entry.panel.has_value()) {
      ++result.n_loaded;
    } else if (entry.panel.error().code() == ErrorCode::NotFound) {
      ++result.n_missing;
    } else {
      ++result.n_error;
      if (entry.coverage_hole) {
        ++result.n_coverage_holes;
      }
    }
    ++done;
    if (progress) {
      progress(done, result.n_total, entry);
    }
  }

  return Ok(std::move(result));
}

} // namespace atx::vol
