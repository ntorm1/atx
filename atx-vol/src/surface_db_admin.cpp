#include "atx/vol/surface_db_admin.hpp"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <string>
#include <system_error>
#include <utility>

#include "atx/vol/priced_surface_view.hpp" // PricedSurfaceView (LoadedSurface::view)

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
[[nodiscard]] Result<std::vector<std::string>> verify_symbol_set(const SurfaceDb &db,
                                                                 const DbVerifySpec &spec) {
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
    }
  }
  return Ok(std::move(out));
}

} // namespace

Result<DbVerifyReport> verify_db(const SurfaceDb &db, const DbVerifySpec &spec) {
  if (!(std::isfinite(spec.probe_T) && spec.probe_T > 0.0)) {
    return Err(ErrorCode::InvalidArgument, "verify_db: probe_T must be finite and > 0");
  }

  Result<std::vector<std::string>> symbols = verify_symbol_set(db, spec);
  if (!symbols) {
    return Err(symbols.error());
  }

  // Partition rows, restricted to the canonical key range. `partitions()` is
  // sorted by canonical key, and ISO dates order correctly under that compare.
  const std::string lo = canonical_ascii(spec.key_lo);
  const std::string hi = canonical_ascii(spec.key_hi);
  const std::vector<DbPartitionInfo> all_partitions = db.partitions();
  std::vector<std::string> keys;
  for (const DbPartitionInfo &p : all_partitions) {
    if ((lo.empty() || p.key >= lo) && (hi.empty() || p.key <= hi)) {
      keys.push_back(p.key);
    }
  }

  DbVerifyReport out;
  out.n_partitions = keys.size();
  // Range-INDEPENDENT, and that is the point: it is what lets `selected_no_cells`
  // tell "this database is fresh" from "this run's window/symbol set matched
  // nothing in a database that is full".
  out.n_partitions_in_db = all_partitions.size();
  out.n_symbols = symbols->size();

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

  // Partition-major so each partition file is opened once and every symbol in it
  // is probed against the same cached mapping (the LRU view cache holds 16).
  for (const std::string &key : keys) {
    for (const std::string &symbol : *symbols) {
      ++out.cells_checked;
      const Result<LoadedSurface> mapped = db.map_surface(key, symbol);
      if (!mapped) {
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

} // namespace atx::vol
