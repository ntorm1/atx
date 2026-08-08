#pragma once

// TrackStore -- Parquet-backed persistence for backtest tracks (Task D2,
// backtest-production-lakehouse sprint). Consumes `TrackKey` (D1, track_key.hpp)
// and `BacktestResult` (backtest.hpp) and lays them out as a hive-partitioned
// Parquet lakehouse under one root directory:
//
//   <lake_root>/staging/<track_key-hex>.feather
//       One Arrow IPC ("Feather V2") file per fresh track, written by
//       `TrackStore::write_staging`. Named by the track's own content-addressed
//       identity, so re-staging the SAME track overwrites its own file rather
//       than accumulating duplicates.
//
//   <lake_root>/tracks/underlier=<U>/family=<F>/batch-NNNNNN.parquet
//       zstd-compressed Parquet batch files, one row group per file, produced by
//       `compact(lake_root)` folding every staged track into its hive partition.
//       `NNNNNN` continues from the highest existing batch index already in that
//       partition directory, so repeated `compact()` calls are additive, never
//       overwriting a prior batch.
//
// Both write paths follow the house atomic-publish discipline verbatim
// (detail/archive_util.hpp): reserve a unique same-directory temp file, write +
// flush it, then atomically rename it onto the destination. `compact()` deletes
// a batch's staged inputs ONLY after that batch file's rename has landed, so a
// crash mid-compaction leaves the not-yet-batched staging files exactly where a
// retry will find them again -- no partial batch is ever visible, and no track
// is ever lost.
//
// Arrow-free header: every Arrow/Parquet type lives inside track_store.cpp,
// behind this purely-Result<T>-returning surface -- mirrors
// atx/core/io/parquet.hpp's firewall. This means the header itself compiles
// with ATX_VOL_LAKEHOUSE off; only track_store.cpp (and the CLI, track_compact)
// are excluded from the OFF build (see atx-vol/CMakeLists.txt) -- calling into
// TrackStore/compact from an ATX_VOL_LAKEHOUSE=OFF build is a LINK error, not a
// compile error, exactly as for any other conditionally-compiled translation
// unit in this tree.
//
// ## Schema v1 -- 33 columns, in this exact order
//
// (The reviewer should audit this list directly against `BacktestResult`,
// backtest.hpp, and the frozen `kBacktestSeriesColumns` table,
// detail/backtest_series_columns.hpp.)
//
//   track_key        utf8, non-null
//       `TrackKey::hex()` -- the lowercase 64-character hex rendering of the
//       full 32-byte SHA-256 digest. The brief's own wording ("binary16->hex")
//       is ambiguous about whether the STORED representation is a 16-byte
//       binary digest or hex text of the full 32-byte hash; per the task's
//       explicit resolution, this stores the hex string of the full 32-byte
//       sha (64 hex chars), not a truncated 16-byte binary. A COLUMN, not a
//       hive partition: many different tracks share one compacted batch file.
//   date             date32, non-null -- days since 1970-01-01, parsed from
//                     `BacktestResult::date` ("YYYY-MM-DD").
//   ts_ns             int64, non-null -- `BacktestResult::ts_ns`, zero-copy.
//
//   The 25 frozen series columns, in `kBacktestSeriesColumns` order
//   (detail/backtest_series_columns.hpp), all float64 non-null, zero-copy from
//   `BacktestResult`'s SoA vectors (they are already contiguous `double`
//   buffers -- no per-element transform):
//     pnl_total, pnl_delta, pnl_gamma, pnl_vega, pnl_vanna, pnl_volga,
//     pnl_theta, pnl_rho, pnl_charm, pnl_unexplained, pnl_settlement,
//     pnl_shares, financing, cost, nav, cash, gross_delta, gross_gamma,
//     gross_vega, gross_theta, turnover_notional, turnover_vega, n_open_lots,
//     n_unpriced_lots, n_unpriced_greeks
//
//   The swap lane -- 5 columns, float64 NULLABLE -- the frozen TSV/RunArchive
//   wire set could never carry (this lakehouse schema is where it finally
//   widens):
//     swap_pv, swap_pnl, gross_vega_abs, nav_liquidation
//         Each is EMPTY-OR-ROW-PARALLEL on `BacktestResult` (see backtest.hpp's
//         own doc comments on these members). When the source vector is
//         non-empty it must be exactly `result.size()` long and is stored
//         verbatim, one value per row. When the source vector is EMPTY (not
//         computed for this run), every row is stored NULL -- never a
//         fabricated 0.0, which would be indistinguishable from a real zero.
//     step_pnl_total
//         Full-resolution per-step series; by contract (see backtest.hpp and
//         backtest_db.cpp's `validate_series_data`, "stored history must
//         include one inception row") its length is either 0 or exactly
//         `result.size() - 1` -- ONE FEWER than every other column, because
//         the inception row (row 0) precedes the first step. Stored NULL on
//         row 0 and, when non-empty, `step_pnl_total[i - 1]` on row `i >= 1`.
//         When the source vector is empty, every row (including row 0) is
//         NULL.
//
// Hive partitioning is on `underlier`/`family` ONLY, taken from the
// caller-supplied `TrackMeta` -- neither `TrackKey` nor `BacktestResult`
// carries a queryable underlier or strategy-family label, so this is
// irreducibly caller-supplied placement metadata, not something TrackStore
// could derive.

#include <cstdint>
#include <string>
#include <string_view>

#include "atx/vol/backtest.hpp"           // BacktestResult
#include "atx/vol/research/track_key.hpp" // TrackKey
#include "atx/vol/types.hpp"              // Result, Status, Error, ErrorCode

namespace atx::vol {

// Caller-supplied hive-placement metadata for one track. `underlier` and
// `family` become the literal `underlier=<U>`/`family=<F>` path segments under
// `<lake_root>/tracks/`, so both must be non-empty and free of path separators
// and other filesystem/hive-unsafe characters (`/ \ : * ? " < > | =` and NUL);
// `write_staging` validates this and refuses with `ErrorCode::InvalidArgument`
// otherwise.
struct TrackMeta {
  std::string underlier;
  std::string family;
};

// Outcome of one `compact()` call. Counts reflect exactly what THIS call did --
// tracks already compacted by a prior run (and therefore no longer present
// under staging/) are not recounted.
struct CompactStats {
  std::uint64_t tracks_compacted{0};
  std::uint64_t batch_files_written{0};
  std::uint64_t staged_files_deleted{0};
};

// One Parquet track lakehouse rooted at `lake_root`. Stateless beyond the root
// path: every call re-derives `staging/`/`tracks/` beneath it, so multiple
// `TrackStore` instances over the same `lake_root` (even across processes) are
// safe to use concurrently -- the atomic-publish discipline is what makes that
// true, not any in-memory coordination this class performs.
class TrackStore {
public:
  explicit TrackStore(std::string lake_root) : lake_root_{std::move(lake_root)} {}

  [[nodiscard]] std::string_view lake_root() const noexcept { return lake_root_; }

  // Writes one fresh track's `BacktestResult` (already produced under
  // `TrackMeta`'s economics/placement) to `<lake_root>/staging/<key.hex()>.feather`,
  // atomically published. `Err(InvalidArgument)` on a malformed `meta`, an
  // empty or shape-inconsistent `result` (row-count mismatches across the
  // frozen columns, an out-of-contract swap-lane length, a malformed date, or
  // rows not strictly ordered by (date, ts_ns) -- see the schema comment
  // above for the exact per-column contracts). No partial file is ever visible
  // to a concurrent reader: the temp file is invisible under its `.tmp.` name
  // until the final atomic rename.
  [[nodiscard]] Status write_staging(const TrackKey &key, const BacktestResult &result,
                                     const TrackMeta &meta);

private:
  std::string lake_root_;
};

// Folds every staged track under `<lake_root>/staging/` into hive-partitioned
// Parquet batch files under `<lake_root>/tracks/underlier=<U>/family=<F>/`
// (see the schema comment above for the exact column layout). Tracks are
// grouped by (underlier, family) and, within a group, ordered by `track_key`
// hex before being packed into batches targeting 256-512 MB each; each batch
// file gets exactly ONE row group, zstd-compressed, with its rows sorted by
// (track_key, date) -- track_key primary (each source track's rows are
// already contiguous, since track_key is constant within one staged file) and
// date secondary (each staged track's own rows are already date-ordered, a
// precondition `write_staging` enforces).
//
// A staged input file is deleted ONLY after the batch file that folded it in
// has been durably, atomically published -- so a crash between two batches
// within one `compact()` call leaves every not-yet-batched staging file
// exactly where a later `compact()` call will find and fold it in; nothing is
// double-counted or lost. An empty or absent `staging/` directory is not an
// error: `compact()` returns a zeroed `CompactStats`.
[[nodiscard]] Result<CompactStats> compact(std::string_view lake_root);

} // namespace atx::vol
