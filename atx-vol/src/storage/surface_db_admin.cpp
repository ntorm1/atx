#include "atx/vol/tools/surface_db_admin.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>   // std::snprintf (band_audit's ns -> "Thh:mm:ssZ" suffix formatting)
#include <filesystem>
#include <iterator> // make_move_iterator (tenor_audit's per-symbol group append)
#include <limits>   // quiet_NaN (tenor_audit's no-neighbors sentinel)
#include <optional>
#include <string>
#include <system_error>
#include <utility>

#include "atx/vol/api/core/chain.hpp"               // OptionChain::from_frame (band_audit)
#include "atx/vol/api/marketdata/corpus.hpp"              // CorpusBoard (band_audit)
#include "atx/vol/api/fitting/fit_metrics.hpp"         // band_violation_stats (band_audit)
#include "atx/vol/api/marketdata/opra_batch.hpp"          // corpus_board_from_opra, OpraBatchEntry/Result
#include "atx/vol/api/marketdata/opra_hive.hpp"           // OpraHiveSpec, load_opra_hive (band_audit)
#include "atx/vol/api/backtest/priced_surface.hpp"      // PricedSurface::tenor_domain (tenor_audit, Task 1/3)
#include "atx/vol/api/backtest/priced_surface_view.hpp" // PricedSurfaceView (LoadedSurface::view)
#include "atx/vol/api/marketdata/universe.hpp"            // Chain, chain_index (band_audit)
#include "atx/vol/api/core/vol_time.hpp"            // settlement_instant_ns (band_audit's DST-aware fallback)

#include "marketdata/opra_batch_detail.hpp" // Civil, parse_civil, days_from_civil (band_audit snapshot math)

namespace atx::vol {

using atx::core::Err;
using atx::core::ErrorCode;
using atx::core::Ok;

namespace {

// Canonical byte rule shared by SurfaceDb's partition keys and symbols: ASCII
// upper-case, truncated to kSurfaceDbKeyMax. Deliberately does NOT validate —
// charset/length VALIDATION belongs to SurfaceDb (canonicalize_key /
// canonicalize_symbol run inside the call being made), so this is only used to
// (a) echo the canonical spelling of an argument a SurfaceDb call already
// accepted, and (b) compare against the already-canonical keys `partitions()`
// returns. Duplicating the validation here would be a second source of truth.
[[nodiscard]] std::string canonical_ascii(std::string_view s) {
  const std::size_t n = std::min(s.size(), kSurfaceDbKeyMax);
  std::string out(n, '\0');
  for (std::size_t i = 0; i < n; ++i) {
    const char c = s[i];
    out[i] = (c >= 'a' && c <= 'z') ? static_cast<char>(c - 'a' + 'A') : c;
  }
  return out;
}

// The documented on-disk location of one partition: <root>/partitions/<KEY>.atxvsa
// (kSurfaceDbPartitionDir / kSurfaceDbPartitionExt are public format constants).
// `canonical_key` must already be canonical — it comes from `partitions()`.
[[nodiscard]] std::filesystem::path partition_file_path(const std::string &root,
                                                        const std::string &canonical_key) {
  return std::filesystem::path(root) / std::string(kSurfaceDbPartitionDir) /
         (canonical_key + std::string(kSurfaceDbPartitionExt));
}

// Live size of a partition file. A missing/unreadable file yields {0, false}
// rather than an error: reporting "the manifest indexes it but it is not there"
// IS the answer an inspector exists to give.
struct OnDiskSize {
  std::uint64_t bytes{0};
  bool present{false};
};
[[nodiscard]] OnDiskSize stat_partition(const std::string &root, const std::string &canonical_key) {
  std::error_code ec;
  const std::uintmax_t size = std::filesystem::file_size(partition_file_path(root, canonical_key), ec);
  if (ec) {
    return OnDiskSize{};
  }
  return OnDiskSize{static_cast<std::uint64_t>(size), true};
}

// Short diagnostic for a fault detail / error message. `std::to_string`'s fixed
// 6-decimal form is plenty for "this number was not usable".
[[nodiscard]] std::string describe_point(double forward, double iv) {
  return "forward=" + std::to_string(forward) + " iv=" + std::to_string(iv);
}

// The ONE rule for "this evaluated to a usable implied vol", shared by
// `query_surface`'s spot check and `verify_db`'s per-cell probe. They used to
// disagree — query rejected only non-finite while verify also rejected `iv <= 0`,
// so `query` printed `iv 0` and exited 0 on a cell `verify` called broken. Both
// now call this, so they cannot drift apart again.
[[nodiscard]] bool usable_iv(double iv) noexcept { return std::isfinite(iv) && iv > 0.0; }

} // namespace

// ── describe_db ─────────────────────────────────────────────────────────────

Result<DbDescription> describe_db(const SurfaceDb &db) {
  DbDescription out;
  out.root = db.root();
  out.generation = db.generation();

  const std::vector<std::string> symbols = db.symbols();
  out.n_symbols = symbols.size();
  for (const std::string &s : symbols) {
    const Result<SymbolFitConfig> cfg = db.symbol_config(s);
    if (!cfg) {
      return Err(cfg.error());
    }
    if (cfg->enabled) {
      ++out.n_symbols_enabled;
    }
  }

  const std::vector<DbPartitionInfo> parts = db.partitions();
  out.n_partitions = parts.size();
  out.partitions.reserve(parts.size());
  for (const DbPartitionInfo &p : parts) {
    const OnDiskSize live = stat_partition(out.root, p.key);
    DbPartitionSummary s;
    s.key = p.key;
    s.surface_count = p.surface_count;
    s.manifest_bytes = p.file_size;
    s.bytes_on_disk = live.bytes;
    s.present = live.present;
    s.created_ts_ns = p.created_ts_ns;
    if (!live.present) {
      ++out.n_partitions_missing;
    }
    out.total_surface_count += p.surface_count;
    out.total_manifest_bytes += p.file_size;
    out.total_bytes_on_disk += live.bytes;
    out.partitions.push_back(std::move(s));
  }
  return Ok(std::move(out));
}

// ── describe_partition ──────────────────────────────────────────────────────

Result<PartitionDescription> describe_partition(const SurfaceDb &db, std::string_view key) {
  // open_partition owns the argument validation AND the manifest membership
  // check, so a bad key surfaces InvalidArgument, an unindexed one NotFound, and
  // a manifest entry whose file vanished an IoError — all without this layer
  // re-implementing any of it.
  const Result<SurfaceArchiveV2> archive = db.open_partition(key);
  if (!archive) {
    return Err(archive.error());
  }

  PartitionDescription out;
  // The manifest side of the same partition. open_partition already proved the
  // key is indexed, so this scan always finds it; the canonical spelling comes
  // from the manifest record, never from the caller's argument.
  const std::string wanted = canonical_ascii(key);
  for (const DbPartitionInfo &p : db.partitions()) {
    if (p.key == wanted) {
      out.key = p.key;
      out.manifest_surface_count = p.surface_count;
      out.manifest_bytes = p.file_size;
      break;
    }
  }
  out.bytes_on_disk = stat_partition(db.root(), out.key).bytes;
  out.archive_surface_count = archive->count();

  const std::span<const ArchiveV2DirEntry> dir = archive->directory();
  out.symbols.reserve(dir.size());
  for (const ArchiveV2DirEntry &e : dir) {
    PartitionSymbolInfo info;
    info.symbol.assign(e.symbol, std::min<std::size_t>(e.symbol_len, sizeof(e.symbol)));
    info.uid = e.uid;
    info.n_slices = e.n_slices;
    info.surface_bytes = e.surface_size;
    out.symbols.push_back(std::move(info));
  }
  return Ok(std::move(out));
}

// ── describe_symbol ─────────────────────────────────────────────────────────

Result<SymbolDescription> describe_symbol(const SurfaceDb &db, std::string_view symbol) {
  const Result<SymbolFitConfig> cfg = db.symbol_config(symbol);
  if (!cfg) {
    return Err(cfg.error());
  }
  const Result<std::optional<SurfaceProvenance>> prov = db.surface_provenance(symbol);
  if (!prov) {
    return Err(prov.error());
  }

  SymbolDescription out;
  out.symbol = canonical_ascii(symbol); // symbol_config succeeded => this IS the stored key
  out.enabled = cfg->enabled;
  out.preset = cfg->preset;
  out.pin_curve = cfg->pin_curve;
  out.curve_kind = cfg->curve.kind;
  out.band_k = cfg->band_k;
  out.surface_policy = cfg->surface_policy;
  out.has_provenance = prov->has_value();
  if (out.has_provenance) {
    out.provenance = **prov;
  }
  return Ok(std::move(out));
}

// ── query_surface ───────────────────────────────────────────────────────────

Result<SurfacePointQuote> query_surface(const SurfaceDb &db, std::string_view key,
                                        std::string_view symbol, double K, double T) {
  // Boundary validation, once (agent profile §4). These are exactly the bounds
  // PricedSurfaceView::resolve treats as a valid query, so a caller gets a named
  // argument error instead of a NaN they have to interpret.
  if (!(std::isfinite(K) && K > 0.0)) {
    return Err(ErrorCode::InvalidArgument, "query_surface: K must be finite and > 0");
  }
  if (!(std::isfinite(T) && T > 0.0)) {
    return Err(ErrorCode::InvalidArgument, "query_surface: T must be finite and > 0");
  }

  // The ZERO-COPY path — the same call production readers make.
  const Result<LoadedSurface> mapped = db.map_surface(key, symbol);
  if (!mapped) {
    return Err(mapped.error());
  }
  const PricedSurfaceView &view = mapped->view;

  SurfacePointQuote out;
  out.key = canonical_ascii(key);
  out.symbol = canonical_ascii(symbol);
  out.K = K;
  out.T = T;
  out.iv = view.iv(K, T);
  out.total_variance = view.total_variance(K, T);
  out.forward = view.forward_at(T);
  out.uid = view.uid();
  out.n_slices = view.n_slices();

  if (!usable_iv(out.iv) || !std::isfinite(out.total_variance)) {
    return Err(ErrorCode::Internal, "query_surface: " + out.key + "/" + out.symbol +
                                        " evaluated unusable (" +
                                        describe_point(out.forward, out.iv) + ")");
  }
  return Ok(std::move(out));
}

// ── verify_db ───────────────────────────────────────────────────────────────

namespace {

// Resolve the symbol columns of the walk. An explicit subset is taken as given
// (a name the manifest never configured is a legitimate thing to assert about);
// otherwise the manifest's symbol table supplies them, minus the fail-closed
// disabled ones unless the caller insists.
//
// `dropped` collects exactly the names that filter removed, so the walk can NAME
// the columns it narrowed itself down to rather than silently reporting a clean
// result over a database that is permanently missing a symbol.
[[nodiscard]] Result<std::vector<std::string>> verify_symbol_set(const SurfaceDb &db,
                                                                 const DbVerifySpec &spec,
                                                                 std::vector<std::string> &dropped) {
  std::vector<std::string> out;
  if (!spec.symbols.empty()) {
    out.reserve(spec.symbols.size());
    for (const std::string &s : spec.symbols) {
      if (s.empty()) {
        return Err(ErrorCode::InvalidArgument, "verify_db: empty symbol in spec.symbols");
      }
      if (s.size() > kSurfaceDbKeyMax) {
        return Err(ErrorCode::InvalidArgument,
                   "verify_db: symbol '" + s + "' exceeds " + std::to_string(kSurfaceDbKeyMax) +
                       " chars");
      }
      out.push_back(canonical_ascii(s));
    }
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return Ok(std::move(out));
  }

  for (const std::string &s : db.symbols()) { // already canonical + sorted
    const Result<SymbolFitConfig> cfg = db.symbol_config(s);
    if (!cfg) {
      return Err(cfg.error());
    }
    if (spec.include_disabled || cfg->enabled) {
      out.push_back(s);
    } else {
      dropped.push_back(s); // canonical + sorted, inherited from db.symbols()
    }
  }
  return Ok(std::move(out));
}

} // namespace

Result<DbVerifyReport> verify_db(const SurfaceDb &db, const DbVerifySpec &spec) {
  if (!(std::isfinite(spec.probe_T) && spec.probe_T > 0.0)) {
    return Err(ErrorCode::InvalidArgument, "verify_db: probe_T must be finite and > 0");
  }

  std::vector<std::string> dropped_disabled;
  Result<std::vector<std::string>> symbols = verify_symbol_set(db, spec, dropped_disabled);
  if (!symbols) {
    return Err(symbols.error());
  }

  // Partition rows, restricted to the canonical key range. `partitions()` is
  // sorted by canonical key, and ISO dates order correctly under that compare.
  const std::string lo = canonical_ascii(spec.key_lo);
  const std::string hi = canonical_ascii(spec.key_hi);
  const std::vector<DbPartitionInfo> all_partitions = db.partitions();
  // The rows, carrying their MANIFEST RECORD with them: gate -1 compares that
  // record against the file, so the walk needs the whole entry, not just the key.
  std::vector<DbPartitionInfo> rows;
  for (const DbPartitionInfo &p : all_partitions) {
    if ((lo.empty() || p.key >= lo) && (hi.empty() || p.key <= hi)) {
      rows.push_back(p);
    }
  }

  DbVerifyReport out;
  out.n_partitions = rows.size();
  // Range-INDEPENDENT, and that is the point: it is what lets `selected_no_cells`
  // tell "this database is fresh" from "this run's window/symbol set matched
  // nothing in a database that is full".
  out.n_partitions_in_db = all_partitions.size();
  out.n_symbols = symbols->size();
  // The columns this walk narrowed itself out of. Reported, never acted on: the
  // verdict stays a statement about the bytes that ARE stored (see the field's
  // doc), but an operator reading a clean report must be able to see that N
  // requested names were not looked at at all.
  out.disabled_symbols = std::move(dropped_disabled);

  // Record a fault, honoring the cap. Truncation is COUNTED, never silent: a
  // capped list with a zero elision count would read as "that was all of them".
  const auto record = [&](std::string key, std::string symbol, DbCellFailure kind,
                          std::string detail) {
    if (out.failures.size() < spec.max_reported_failures) {
      out.failures.push_back(
          DbCellFault{std::move(key), std::move(symbol), kind, std::move(detail)});
    } else {
      ++out.n_failures_elided;
    }
  };

  // The same cap discipline for absences, on its OWN budget — see
  // `DbVerifySpec::max_reported_failures` for why the two must not share a pool.
  const auto record_absent = [&](std::string key, std::string symbol) {
    if (out.absent_cells.size() < spec.max_reported_failures) {
      out.absent_cells.push_back(DbAbsentCell{std::move(key), std::move(symbol)});
    } else {
      ++out.n_absent_elided;
    }
  };

  // And again for the partition-index faults, on a THIRD independent budget. Same
  // argument as the absence list: one number spent per list, never one pool shared,
  // so a database with many absences can never elide the index fault beside them.
  const auto record_index_fault = [&](DbPartitionIndexFault fault) {
    if (out.index_faults.size() < spec.max_reported_failures) {
      out.index_faults.push_back(std::move(fault));
    } else {
      ++out.n_index_faults_elided;
    }
  };

  // Partition-major so each partition file is opened once and every symbol in it
  // is probed against the same cached mapping (the LRU view cache holds 16).
  for (const DbPartitionInfo &row : rows) {
    const std::string &key = row.key;
    // ONE open per row, serving gate -1 and gate 0 both. `open_partition` re-reads
    // the file rather than borrowing the db's cached mapping (there is no accessor
    // for that mapping without a successful map, which is precisely what gate 0
    // does not have); a concurrent writer can therefore make the two disagree,
    // which is the torn view this header already documents as the
    // concurrent-writer contract.
    const Result<SurfaceArchiveV2> row_archive = db.open_partition(key);

    // ── Gate -1: the manifest's RECORD against the FILE it indexes ────────────
    // The one inconsistency no per-cell gate can see: after a torn write (archive
    // rewritten, manifest not yet persisted) every cell still maps, checksums and
    // probes — the file is a perfectly good file — while the index describes the
    // PREVIOUS write. See `DbPartitionIndexFault` for the window and for why the
    // write order that opens it is nevertheless the right one.
    //
    // A partition that will not open is deliberately NOT counted here: gate 0/1
    // is about to report every cell on this row `unmappable`, which is louder and
    // more precise, and a second fault over the same bytes would double-report it.
    if (row_archive) {
      const auto archive_count = static_cast<std::uint32_t>(row_archive->count());
      const std::uint64_t live_bytes = stat_partition(db.root(), key).bytes;
      if (archive_count != row.surface_count || live_bytes != row.file_size) {
        ++out.partitions_index_mismatch;
        record_index_fault(DbPartitionIndexFault{key, row.surface_count, archive_count,
                                                 row.file_size, live_bytes});
      }
    }

    // "The partition opened, and it does NOT list this symbol" — the one fact
    // that separates a cell that was never stored from one that is unreadable.
    // A partition that will not open answers `false`: the directory that would
    // decide is itself the thing that is missing, so the cell keeps the
    // corruption reading rather than being quietly excused as absent.
    const auto never_stored = [&](const std::string &sym) -> bool {
      if (!row_archive) {
        return false;
      }
      const Result<ArchiveV2DirEntry> entry = row_archive->find(sym);
      return !entry && entry.error().code() == ErrorCode::NotFound;
    };

    for (const std::string &symbol : *symbols) {
      ++out.cells_checked;
      const Result<LoadedSurface> mapped = db.map_surface(key, symbol);
      if (!mapped) {
        // Gate 0. Nothing was ever written here, so there are no bytes to have
        // gone bad and no fault to record — only a cell the database does not
        // hold. Counted and named, never folded into the verdict: see the
        // ABSENCE IS NOT A FAULT block in surface_db_admin.hpp.
        //
        // TWO conditions, and the NotFound one is not redundant. The directory
        // probe answers "is this symbol in the file?", so on its own it would
        // reclassify EVERY mapping failure whose symbol happens not to be in the
        // file — including an InvalidArgument from a malformed `spec.symbols`
        // entry, or a ParseError/IoError from a partition that opened for the
        // directory read and broke on the record. Absence must mean "the map said
        // NOT FOUND and the directory agrees", so any other error stays a fault.
        if (mapped.error().code() == ErrorCode::NotFound && never_stored(symbol)) {
          ++out.cells_absent;
          record_absent(key, symbol);
          continue;
        }
        ++out.cells_unmappable;
        record(key, symbol, DbCellFailure::Unmappable, mapped.error().to_string());
        continue;
      }
      // Mapping validated magic, framing and bounds — NOT one payload byte. The
      // record carries its own CRC-32C; check it against the archive the map is
      // already holding open (`LoadedSurface::partition`), so this costs no extra
      // open and no extra mapping — only the read of the payload it checksums.
      // Without this, intra-record damage maps cleanly, and a probe that happens
      // to miss the damaged slices reports the cell healthy.
      const Status crc = mapped->partition->validate_symbol(symbol);
      if (!crc) {
        ++out.cells_checksum;
        record(key, symbol, DbCellFailure::ChecksumMismatch, crc.error().to_string());
        continue;
      }
      // Bytes intact. Evaluate one ATM-ish point — K at the surface's own forward
      // — to prove valid bytes actually produce a usable number.
      const PricedSurfaceView &view = mapped->view;
      const double forward = view.forward_at(spec.probe_T);
      const double iv = view.iv(forward, spec.probe_T);
      if (!std::isfinite(forward) || forward <= 0.0 || !usable_iv(iv)) {
        ++out.cells_non_finite;
        record(key, symbol, DbCellFailure::NonFinite, describe_point(forward, iv));
        continue;
      }
      ++out.cells_ok;
    }
  }
  return Ok(std::move(out));
}

// ── set_symbol_enabled ──────────────────────────────────────────────────────

Result<SymbolEnableChange> set_symbol_enabled(SurfaceDb &db, std::string_view symbol,
                                              bool enabled) {
  // Read the STORED config first. This is also the existence check: a symbol the
  // manifest does not configure is NotFound and nothing is written, rather than
  // `upsert_symbol` quietly creating a default-constructed config for a name the
  // operator most likely misspelled.
  Result<SymbolFitConfig> cfg = db.symbol_config(symbol);
  if (!cfg) {
    return Err(cfg.error());
  }

  SymbolEnableChange out;
  out.symbol = canonical_ascii(symbol);
  out.was_enabled = cfg->enabled;
  out.now_enabled = enabled;
  out.changed = cfg->enabled != enabled;

  // Already in the requested state: do NOT write. A no-op rewrite would bump the
  // generation and move nothing, and an operator script that re-asserts the
  // desired state on every run would churn the manifest for no reason.
  if (!out.changed) {
    out.generation = db.generation();
    return Ok(std::move(out));
  }

  // One field. Everything else in `cfg` is the config as stored — an operator's
  // tuned knobs, or the selector's chosen curve family — and it goes back
  // unchanged. Provenance is preserved by `upsert_symbol` itself: passing
  // `nullopt` for an EXISTING symbol keeps the record's stored provenance rather
  // than clearing it (surface_db.cpp, the `replaced` branch), so `config
  // --symbol` reads the same provenance after this call as before.
  cfg->enabled = enabled;
  if (const Status up = db.upsert_symbol(symbol, *cfg); !up) {
    return Err(up.error());
  }
  out.generation = db.generation();
  return Ok(std::move(out));
}

// ── tenor_audit ──────────────────────────────────────────────────────────────

namespace {

// The rolling-median comparison value for row `i` of a symbol's date-ordered
// `max_Ts` series: median of up to 5 OTHER rows nearest `i` by index
// distance, ties broken toward the earlier (smaller-index) row — see
// `tenor_audit`'s doc comment in the header for why this is the "2 before + 2
// after + 1 more from whichever side has slack" rule stated as one sort.
// NaN when `max_Ts.size() <= 1` (no OTHER row exists to compare against).
[[nodiscard]] double rolling_median_max_T(const std::vector<double> &max_Ts, std::size_t i) {
  std::vector<std::pair<std::size_t, std::size_t>> by_distance; // (distance, index)
  by_distance.reserve(max_Ts.size() > 0 ? max_Ts.size() - 1 : 0);
  for (std::size_t j = 0; j < max_Ts.size(); ++j) {
    if (j == i) {
      continue;
    }
    const std::size_t dist = j > i ? j - i : i - j;
    by_distance.emplace_back(dist, j);
  }
  std::sort(by_distance.begin(), by_distance.end());
  const std::size_t take = std::min<std::size_t>(5, by_distance.size());
  if (take == 0) {
    return std::numeric_limits<double>::quiet_NaN();
  }
  std::vector<double> neighbors;
  neighbors.reserve(take);
  for (std::size_t k = 0; k < take; ++k) {
    neighbors.push_back(max_Ts[by_distance[k].second]);
  }
  std::sort(neighbors.begin(), neighbors.end());
  const std::size_t m = neighbors.size();
  return (m % 2 == 1) ? neighbors[m / 2] : 0.5 * (neighbors[m / 2 - 1] + neighbors[m / 2]);
}

// How far below the rolling median a row's max_T must fall to be flagged.
// Not exposed as a spec knob (the ambiguity resolution this task was scoped
// under pins the literal value); kept as a named constant rather than a bare
// literal at the compare site.
inline constexpr double kTenorAuditTruncationSlackYears = 0.25;

} // namespace

Result<TenorAuditReport> tenor_audit(const SurfaceDb &db, const TenorAuditSpec &spec) {
  std::vector<std::string> symbols;
  if (!spec.symbols.empty()) {
    // Fix Round 1 (Important 2, review of df09222). An unconfigured symbol
    // name used to reach the walk unchanged and simply audit ZERO rows -- and
    // an empty walk has `n_truncated == 0`, which the CLI's
    // `--fail-on-truncated` (surface_db_main.cpp) cannot tell apart from a
    // healthy database with nothing truncated: `tenor-audit --symbol SPYY
    // --fail-on-truncated` (a typo for SPY) exited 0 while auditing nothing.
    // Reject an unconfigured name up front instead -- the same NotFound
    // `set_symbol_enabled` already gives an unconfigured `--symbol` on the
    // `enable`/`disable` subcommands -- so the failure is loud at the source
    // rather than indistinguishable from success three calls downstream.
    const std::vector<std::string> configured = db.symbols(); // canonical, sorted
    symbols.reserve(spec.symbols.size());
    for (const std::string &s : spec.symbols) {
      const std::string canon = canonical_ascii(s);
      if (std::find(configured.begin(), configured.end(), canon) == configured.end()) {
        return Err(ErrorCode::NotFound,
                   "tenor_audit: symbol '" + canon + "' is not configured in this database");
      }
      symbols.push_back(canon);
    }
  } else {
    symbols = db.symbols(); // canonical, sorted; every manifest symbol, disabled included
  }

  std::vector<std::string> dates;
  {
    const std::vector<DbPartitionInfo> parts = db.partitions(); // sorted by canonical key
    dates.reserve(parts.size());
    for (const DbPartitionInfo &p : parts) {
      dates.push_back(p.key);
    }
  }

  TenorAuditReport out;
  for (const std::string &symbol : symbols) {
    std::vector<TenorAuditRow> group;
    group.reserve(dates.size());
    for (const std::string &date : dates) {
      const Result<PricedSurface> surf = db.load_surface(date, symbol);
      if (!surf) {
        // Fix Round 1 (Important 3): capped, same discipline as `verify_db`'s
        // `record`/`record_absent`/`record_index_fault` above — the skip
        // itself (excluding this cell from `group`/the median comparison)
        // happens unconditionally via `continue`; only the note TEXT is
        // bounded.
        if (out.skip_notes.size() < spec.max_skip_notes) {
          out.skip_notes.push_back(date + " " + symbol + ": " + surf.error().to_string());
        } else {
          ++out.n_skip_notes_elided;
        }
        continue;
      }
      const PricedSurface::TenorDomain dom = surf->tenor_domain();
      TenorAuditRow row;
      row.date = date;
      row.symbol = symbol;
      row.n_slices = surf->n_slices();
      row.min_T = dom.min_T;
      row.max_T = dom.max_T;
      group.push_back(std::move(row));
    }

    std::vector<double> max_Ts;
    max_Ts.reserve(group.size());
    for (const TenorAuditRow &row : group) {
      max_Ts.push_back(row.max_T);
    }
    for (std::size_t i = 0; i < group.size(); ++i) {
      const double median = rolling_median_max_T(max_Ts, i);
      if (std::isfinite(median) && group[i].max_T < median - kTenorAuditTruncationSlackYears) {
        group[i].truncated = true;
        ++out.n_truncated;
      }
    }
    out.rows.insert(out.rows.end(), std::make_move_iterator(group.begin()),
                    std::make_move_iterator(group.end()));
  }
  return Ok(std::move(out));
}

// ── band_audit ───────────────────────────────────────────────────────────────

BandAuditRow score_expiry_band(std::span<const double> model_price,
                               std::span<const double> bid,
                               std::span<const double> ask,
                               double min_frac_in_band) {
  BandAuditRow row;
  const std::size_t n_in =
      std::min({model_price.size(), bid.size(), ask.size()});
  std::vector<double> m, b, a;
  m.reserve(n_in);
  b.reserve(n_in);
  a.reserve(n_in);
  double sum_signed = 0.0;
  double max_abs = 0.0;
  for (std::size_t i = 0; i < n_in; ++i) {
    const double p = model_price[i];
    if (!(bid[i] > 0.0) || !(ask[i] > bid[i]) || !std::isfinite(p)) {
      continue; // one-sided / crossed / failed model — unscorable
    }
    const double mid = 0.5 * (bid[i] + ask[i]);
    const double half = 0.5 * (ask[i] - bid[i]); // > 0 by the gate above
    const double err_hs = (p - mid) / half;
    sum_signed += err_hs;
    max_abs = std::max(max_abs, std::fabs(err_hs));
    m.push_back(p);
    b.push_back(bid[i]);
    a.push_back(ask[i]);
  }
  row.n = m.size();
  if (row.n == 0) {
    return row; // never flagged: nothing was measurable
  }
  // Counts via the shared scorer (its skip set is empty after the filter
  // above, so `stats.n == row.n` by construction).
  const auto stats = band_violation_stats(m, b, a);
  if (stats.has_value() && stats->n > 0) {
    const double n_d = static_cast<double>(stats->n);
    row.frac_in_band =
        static_cast<double>(stats->n - stats->n_bid_miss - stats->n_ask_miss) / n_d;
    row.frac_above_ask = static_cast<double>(stats->n_ask_miss) / n_d;
  }
  row.avg_signed_err_half_spreads = sum_signed / static_cast<double>(row.n);
  row.max_abs_err_half_spreads = max_abs;
  row.flagged = row.frac_in_band < min_frac_in_band;
  return row;
}

// ── Fix round 1 (review Important 1): DST-correct hive snapshot resolution ──
//
// A hardcoded EDT suffix ("T19:55:00Z") silently produced ZERO scored rows
// for an EST-season date rather than a wrong number: `opra_panel.cpp`'s
// FIX-C-1 guard already refuses a hive load whose requested snapshot stamp
// disagrees with the file's own `ts` column, so the wrong suffix surfaced as
// a skip note, not a bad price. Either way the audit covered nothing for that
// date and a `--fail-on-flagged` gate over it could never fire. The fix:
// derive the hive snapshot suffix from the ARCHIVED SURFACE's OWN stored
// valuation instant (`PricedSurface::pricing().now_ts_ns`) instead of
// assuming a fixed UTC hour — DST-proof by construction, because the
// surface's own timestamp already encodes whatever offset it was actually
// fit under.

inline constexpr std::int64_t kBandAuditNsPerSecond = 1'000'000'000LL;
inline constexpr std::int64_t kBandAuditNsPerDay = 86'400LL * kBandAuditNsPerSecond;

// The "Thh:mm:ssZ" hive snapshot suffix that reconstructs `surf_now_ts_ns`
// exactly when combined with `date` (i.e. `iso_to_ns(date + out) ==
// surf_now_ts_ns`), so a hive load using it reprices against the EXACT
// instant the surface was fit to — independent of any EDT/EST assumption.
//
// Returns false (leaving `out` untouched) when `surf_now_ts_ns` is
// non-positive (no stored valuation timestamp — an old/degenerate archive:
// `PricingContext::now_ts_ns` default-constructs to 0 and no real fit ever
// produces a non-positive epoch stamp) or does not fall within `date`'s own
// UTC calendar day (a surface whose stamp disagrees with its own partition
// key, closer to corruption than a DST question). Either case is the
// caller's cue to fall back to `dst_aware_fallback_suffix` below.
[[nodiscard]] bool snapshot_suffix_from_surface_ts(const std::string &date,
                                                    std::int64_t surf_now_ts_ns,
                                                    std::string &out) {
  if (surf_now_ts_ns <= 0) {
    return false;
  }
  opra_detail::Civil c;
  if (!opra_detail::parse_civil(date, c)) {
    return false;
  }
  const std::int64_t day_start_ns = opra_detail::days_from_civil(c.y, c.m, c.d) * kBandAuditNsPerDay;
  const std::int64_t tod_ns = surf_now_ts_ns - day_start_ns;
  if (tod_ns < 0 || tod_ns >= kBandAuditNsPerDay) {
    return false; // the surface's own valuation instant falls outside `date`'s UTC day
  }
  const std::int64_t tod_s = tod_ns / kBandAuditNsPerSecond;
  const int hh = static_cast<int>(tod_s / 3600);
  const int mm = static_cast<int>((tod_s % 3600) / 60);
  const int ss = static_cast<int>(tod_s % 60);
  char buf[16];
  std::snprintf(buf, sizeof(buf), "T%02d:%02d:%02dZ", hh, mm, ss);
  out = buf;
  return true;
}

// Is US-Eastern civil date `c` in daylight saving time (EDT)? Reuses the
// already-public, already-DST-correct `settlement_instant_ns` (16:00 ET's UTC
// instant is 20:00Z under EDT, 21:00Z under EST) rather than re-deriving the
// "2nd Sunday of March .. 1st Sunday of November" rule a third time —
// `opra_panel.cpp` and `vol_time.cpp` each already carry a private copy of it
// for their own translation unit.
[[nodiscard]] bool date_is_us_eastern_dst(const opra_detail::Civil &c) {
  const std::int64_t day = opra_detail::days_from_civil(c.y, c.m, c.d);
  const std::int64_t pm_close_utc_ns =
      settlement_instant_ns(static_cast<std::int32_t>(day), SettlementSession::Pm);
  const std::int64_t ns_of_day = pm_close_utc_ns - day * kBandAuditNsPerDay;
  return ns_of_day / (3600LL * kBandAuditNsPerSecond) == 20; // 20:00Z=16:00 EDT; 21:00Z=16:00 EST
}

// DST-aware fallback for a cell whose surface carries no usable stored
// valuation timestamp (`snapshot_suffix_from_surface_ts` above returned
// false). `baseline`'s clock is EDT-anchored by construction
// (`BandAuditSpec::snapshot_suffix`'s default "T19:55:00Z" == 15:55 ET on an
// EDT date, the production orchestrator's `--snap-et 15:55` pre-close pull
// minute — run_surface_db_backfill.py's DST addendum). On an EST-season
// `date` the SAME 15:55 ET instant is one UTC hour later, so this shifts the
// baseline's wall-clock hour by +1. Only recognizes the exact "Thh:mm:ssZ"
// shape (10 bytes, fixed separators/terminator); anything else — a
// caller-supplied offset stamp, fractional seconds, a bare date — is returned
// unchanged rather than mis-parsed (the pre-fix passthrough behavior).
[[nodiscard]] std::string dst_aware_fallback_suffix(const std::string &date,
                                                    const std::string &baseline) {
  opra_detail::Civil c;
  if (!opra_detail::parse_civil(date, c) || baseline.size() != 10 || baseline.front() != 'T' ||
      baseline[3] != ':' || baseline[6] != ':' || baseline.back() != 'Z') {
    return baseline;
  }
  if (date_is_us_eastern_dst(c)) {
    return baseline;
  }
  const int hh = (baseline[1] - '0') * 10 + (baseline[2] - '0');
  const int shifted = (hh + 1) % 24;
  std::string out = baseline;
  out[1] = static_cast<char>('0' + shifted / 10);
  out[2] = static_cast<char>('0' + shifted % 10);
  return out;
}

Result<BandAuditReport> band_audit(const SurfaceDb &db, const BandAuditSpec &spec) {
  if (spec.hive_root.empty()) {
    return Err(ErrorCode::InvalidArgument, "band_audit: hive_root is required");
  }
  // Symbol resolution — the tenor_audit stance, verbatim (loud NotFound on an
  // unconfigured explicit name, BEFORE any hive IO).
  std::vector<std::string> symbols;
  if (!spec.symbols.empty()) {
    const std::vector<std::string> configured = db.symbols();
    symbols.reserve(spec.symbols.size());
    for (const std::string &s : spec.symbols) {
      const std::string canon = canonical_ascii(s);
      if (std::find(configured.begin(), configured.end(), canon) == configured.end()) {
        return Err(ErrorCode::NotFound,
                   "band_audit: symbol '" + canon + "' is not configured in this database");
      }
      symbols.push_back(canon);
    }
  } else {
    symbols = db.symbols();
  }

  std::vector<std::string> dates;
  for (const DbPartitionInfo &p : db.partitions()) { // sorted ascending
    if (!spec.date_lo.empty() && p.key < spec.date_lo) continue;
    if (!spec.date_hi.empty() && p.key > spec.date_hi) continue;
    dates.push_back(p.key);
  }

  BandAuditReport out;
  // Final-review I1. Recorded BEFORE the walk so the "audited nothing" message
  // can name what was asked for even when the window matched no partition at
  // all — "0 scored expiries across 0 requested dates" is a different operator
  // problem from "0 scored expiries across 63 requested dates".
  out.n_dates_requested = dates.size();
  const auto note = [&](std::string text) {
    if (out.skip_notes.size() < spec.max_skip_notes) {
      out.skip_notes.push_back(std::move(text));
    } else {
      ++out.n_skip_notes_elided;
    }
  };
  const auto note_fallback = [&](std::string text) {
    if (out.snapshot_fallback_notes.size() < spec.max_skip_notes) {
      out.snapshot_fallback_notes.push_back(std::move(text));
    } else {
      ++out.n_snapshot_fallback_notes_elided;
    }
  };

  for (const std::string &date : dates) {
    for (const std::string &symbol : symbols) {
      // The surface is loaded FIRST (a local partition read, no hive IO) so
      // its own stored valuation instant can pick the hive snapshot — see the
      // fix-round-1 block above.
      const Result<PricedSurface> surf = db.load_surface(date, symbol);
      if (!surf) {
        note(date + " " + symbol + ": surface: " + surf.error().to_string());
        continue;
      }
      std::string snapshot_suffix;
      if (!snapshot_suffix_from_surface_ts(date, surf->pricing().now_ts_ns, snapshot_suffix)) {
        snapshot_suffix = dst_aware_fallback_suffix(date, spec.snapshot_suffix);
        note_fallback(date + " " + symbol +
                      ": surface has no usable stored valuation timestamp "
                      "(pricing().now_ts_ns=" +
                      std::to_string(surf->pricing().now_ts_ns) +
                      "); using DST-aware fallback snapshot suffix " + snapshot_suffix);
      }

      OpraHiveSpec hs;
      hs.root_dir = spec.hive_root;
      hs.date_lo = date;
      hs.date_hi = date;
      hs.symbols = {symbol};
      hs.snapshot_suffix = snapshot_suffix;
      hs.r = spec.r;
      Result<OpraBatchResult> loaded = load_opra_hive(hs);
      if (!loaded) {
        note(date + " " + symbol + ": hive load failed: " + loaded.error().to_string());
        continue;
      }
      for (OpraBatchEntry &entry : loaded->entries) {
        if (!entry.panel.has_value()) {
          note(date + " " + entry.symbol + ": " + entry.panel.error().to_string());
          continue;
        }
        CorpusBoard board =
            corpus_board_from_opra(date, entry.symbol, std::move(*entry.panel));
        auto chain = OptionChain::from_frame(board.frame, board.env);
        if (!chain) {
          note(date + " " + entry.symbol + ": chain: " + chain.error().to_string());
          continue;
        }
        for (const Chain &c : chain->underlying().chains) {
          std::vector<double> model, cb, ca;
          const std::size_t n_str = c.strikes.size();
          model.reserve(2 * n_str);
          cb.reserve(2 * n_str);
          ca.reserve(2 * n_str);
          for (std::size_t i = 0; i < n_str; ++i) {
            for (const Side side : {Side::Call, Side::Put}) {
              const std::size_t idx = chain_index(static_cast<std::uint16_t>(i), side);
              const double qb = c.bids[idx];
              const double qa = c.asks[idx];
              if (!(qb > 0.0) || !(qa > qb)) {
                continue; // unscorable quote — keep spans aligned by skipping here
              }
              const Result<double> fv = surf->fair_value(c.strikes[i], c.T, side);
              model.push_back(fv.has_value() ? *fv
                                             : std::numeric_limits<double>::quiet_NaN());
              cb.push_back(qb);
              ca.push_back(qa);
            }
          }
          BandAuditRow row = score_expiry_band(model, cb, ca, spec.min_frac_in_band);
          row.date = date;
          row.symbol = entry.symbol;
          row.T = c.T;
          if (row.flagged) {
            ++out.n_flagged;
          }
          // Final-review I1: an expiry that measured NOTHING (`row.n == 0` —
          // every listed quote one-sided/crossed, or every model price
          // non-finite) still emits a row and can never be flagged, so
          // `rows.size()` is not a measure of coverage. This is.
          if (row.n > 0) {
            ++out.n_scored_expiries;
          }
          out.rows.push_back(std::move(row));
        } // for (Chain c : chain->underlying().chains)
      }   // for (OpraBatchEntry &entry : loaded->entries)
    }     // for (symbol : symbols)
  }       // for (date : dates)
  return Ok(std::move(out));
}

} // namespace atx::vol
