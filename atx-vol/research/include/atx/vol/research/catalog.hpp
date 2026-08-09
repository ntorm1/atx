#pragma once

// Catalog -- SQLite catalog for the backtest lakehouse (Task D3,
// backtest-production-lakehouse sprint): tracks / trials / trial statistics
// over `<lake_root>/catalog.sqlite`, alongside D2's `staging/`/`tracks/`
// Parquet layout (track_store.hpp) under the SAME `lake_root`. `Catalog` is
// metadata about the lakehouse (what tracks exist, where, and what trials
// were attempted against them) -- the Parquet files under `tracks/` remain
// the actual economics data; nothing here duplicates or re-derives them.
//
// ## Dependency note (read before reaching for vcpkg)
//
// SQLite is NOT a new build dependency: atx-core already vendors the SQLite
// amalgamation (atx-core/third-party/sqlite, compiled as the `atx_sqlite3`
// static lib) behind the RAII/Result wrapper this header uses,
// `atx::core::db::Database`/`Statement` (atx/core/db/sqlite.hpp) --
// atx-core/CMakeLists.txt's own comment: "atx-core links it PRIVATE: the
// symbols propagate to the final link". atx-vol already links atx::core
// PUBLIC unconditionally, so no new find_package/vcpkg entry, and no new
// link edge, is needed for this header or catalog.cpp to use it. This ALSO
// means `<sqlite3.h>` never appears in this translation unit or this header
// -- `atx::core::db` already forward-declares the opaque `sqlite3`/
// `sqlite3_stmt` handles and confines the real C API (and its non-/W4-clean
// warnings) to atx-core/src/db/sqlite.cpp, which is exactly the
// "SYSTEM include or wrapper" firewall D2 built for Arrow, gotten here for
// free by depending on an existing wrapper instead of a raw C header.
//
// `catalog.cpp` (the only TU that actually opens a `Database`) still joins
// atx-vol's build ONLY under `ATX_VOL_LAKEHOUSE` (atx-vol/CMakeLists.txt),
// mirroring D2's `track_store.cpp` gate exactly, so calling into `Catalog`
// from an `ATX_VOL_LAKEHOUSE=OFF` build is a LINK error, not a compile
// error -- this header itself has no Arrow/SQLite-specific type in it (only
// the already-firewalled `atx::core::db::Database` by value) and compiles
// unconditionally, same as track_store.hpp.
//
// ## Schema v1
//
// ```sql
// CREATE TABLE tracks(
//   track_key TEXT PRIMARY KEY,          -- hex SHA-256 (TrackKey::hex())
//   underlier TEXT NOT NULL, family TEXT NOT NULL,
//   config_json TEXT NOT NULL,           -- canonical, human-queryable copy
//   engine_id TEXT NOT NULL, economics_rev INTEGER NOT NULL,
//   data_snapshot_id TEXT NOT NULL,
//   date_min TEXT NOT NULL, date_max TEXT NOT NULL,
//   status TEXT NOT NULL CHECK(status IN ('staging','compacted','retired')),
//   file TEXT, row_group INTEGER,        -- NULL while staging
//   created_ts INTEGER NOT NULL, last_access_ts INTEGER NOT NULL);
// CREATE TABLE trials(                   -- B4's N: EVERY variant ever attempted
//   trial_id INTEGER PRIMARY KEY,
//   track_key TEXT NOT NULL REFERENCES tracks(track_key),
//   sweep_id TEXT NOT NULL, sharpe REAL, created_ts INTEGER NOT NULL);
// CREATE INDEX idx_tracks_dims ON tracks(underlier, family, date_min, date_max);
// ```
//
// Landed verbatim from the brief -- see catalog.cpp's schema constant for the
// exact DDL text executed at `open()`.
//
// `PRAGMA journal_mode=WAL; busy_timeout=<open()'s parameter, default 5000>;
// synchronous=NORMAL;` are applied on every `open()`. WAL is the mechanism
// that makes the lakehouse's catalog writes single-writer/many-reader
// WITHOUT this task's own file-lock machinery (contrast `detail/
// writer_lock.hpp`, which exists because BacktestDb/SurfaceDb's manifest is
// a flat file with no built-in concurrency control at all): `catalog.sqlite`
// is the house atomic-publish discipline's one documented exception --
// `detail/archive_util.hpp`'s doc comment names it explicitly -- since a
// SQLite-managed database file has no "temp copy + atomic rename" equivalent
// while a connection has it open; durability is delegated entirely to
// SQLite's own WAL journal instead. `open()` reads `journal_mode` back after
// setting it (rather than trusting the PRAGMA's own Status, which reports
// success even when WAL silently falls back to a different mode on an
// unsupported filesystem) and fails closed if it did not actually land in
// WAL.
//
// A concurrent SECOND writer beyond `busy_timeout`'s budget fails fast with
// `Err(ErrorCode::Unavailable)` (SQLite's `SQLITE_BUSY`/`SQLITE_LOCKED`,
// mapped by `atx::core::db`) -- bounded by `busy_timeout`, never an
// indefinite block.
//
// ## Reused types
//
// `TrackKey` (D1, track_key.hpp) and `TrackMeta` (D2, track_store.hpp) are
// reused verbatim as the track identity/placement -- this is the SAME track
// TrackStore's Parquet files are keyed and hive-partitioned by, so there is
// exactly one definition of "which track" and "where it lives" across the
// lakehouse. `TrialStats` (B4, tools/tearsheet.hpp) is reused verbatim as
// `trial_stats()`'s return type -- B4's `dsr()` takes it directly, with no
// translation struct in between; tearsheet.hpp's own doc comment names this
// header as the source ("the driver queries D3's registry and passes these
// two numbers in").

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "atx/core/db/sqlite.hpp"           // atx::core::db::Database (opaque-handle RAII wrapper)
#include "atx/vol/research/track_key.hpp"   // TrackKey
#include "atx/vol/research/track_store.hpp" // TrackMeta
#include "atx/vol/tools/tearsheet.hpp"      // TrialStats (B4)
#include "atx/vol/types.hpp"                // Result, Status, Error, ErrorCode

namespace atx::vol {

// `catalog.sqlite`'s filename, a sibling of D2's `staging/`/`tracks/` under
// the same `lake_root`.
inline constexpr std::string_view kCatalogDbName = "catalog.sqlite";

// `tracks.status`'s three states, in the CHECK constraint's exact spelling.
enum class TrackStatus : std::uint8_t {
  Staging,   // "staging"   -- write_staging() landed it; file/row_group NULL
  Compacted, // "compacted" -- mark_compacted() folded it into a Parquet batch
  Retired,   // "retired"   -- Task D5: `register_staging` writes this on an
             //                OLDER-generation row it just superseded (see
             //                that method's doc comment). Retired rows are
             //                NEVER deleted or rewritten -- still `probe()`-
             //                able, still returned by an unfiltered
             //                `SELECT * FROM tracks` (atxpy.tracks.catalog())
             //                -- "kept queryable" per the brief. A future
             //                retention/eviction task (D6) may additionally
             //                write this status for a DIFFERENT reason
             //                (last_access_ts-driven GC) and may choose to
             //                actually delete/rewrite Parquet data for rows
             //                already in this state; that is out of D5's
             //                scope.
};

[[nodiscard]] std::string_view to_string(TrackStatus status) noexcept;
// Err(InvalidArgument) if `text` is not exactly one of the CHECK constraint's
// three spellings.
[[nodiscard]] atx::core::Result<TrackStatus> track_status_from_string(std::string_view text);

// One `tracks` row, as read back by `probe()`. `file`/`row_group` are
// populated (non-nullopt) iff `status == Compacted` -- exactly the schema's
// "NULL while staging" contract.
struct TrackRow {
  std::string track_key;
  std::string underlier;
  std::string family;
  std::string config_json;
  std::string engine_id;
  std::int64_t economics_rev{0};
  std::string data_snapshot_id;
  std::string date_min;
  std::string date_max;
  TrackStatus status{TrackStatus::Staging};
  std::optional<std::string> file;
  std::optional<std::int64_t> row_group;
  std::int64_t created_ts{0};
  std::int64_t last_access_ts{0};
};

// Everything `register_staging` needs about a fresh track beyond its
// identity (`TrackKey`) and hive placement (`TrackMeta`, reused from D2 --
// both are already-validated caller-supplied types the lakehouse agrees on).
struct TrackRegistration {
  std::string config_json;     // canonical, human-queryable copy; must be non-empty
  std::string engine_id;       // e.g. TrackKey's own make_engine_id()
  std::int64_t economics_rev{0};
  std::string data_snapshot_id;
  std::string date_min; // "YYYY-MM-DD", inclusive
  std::string date_max; // "YYYY-MM-DD", inclusive; must be >= date_min
};

// One SQLite connection over `<lake_root>/catalog.sqlite`. Move-only (owns
// the connection); safe to have multiple `Catalog` instances -- in this
// process or another -- open on the SAME `lake_root` concurrently: that is
// exactly the single-writer/many-reader contract WAL mode exists to make
// safe, and `open()` sets `busy_timeout` so a transient writer/writer
// collision waits (bounded) rather than corrupting anything.
class Catalog {
public:
  // Creates `lake_root` (and `catalog.sqlite` within it) if absent, applies
  // the WAL/busy_timeout/synchronous pragmas (see the file doc comment),
  // verifies WAL actually took effect, and creates the schema (idempotent --
  // `CREATE TABLE IF NOT EXISTS`/`CREATE INDEX IF NOT EXISTS`, so opening an
  // already-initialized catalog is a normal, repeatable operation, not a
  // one-shot "create" distinct from "open"). `busy_timeout` overrides the
  // brief's 5000ms default; production callers should not need to.
  [[nodiscard]] static atx::core::Result<Catalog>
  open(std::string lake_root, std::chrono::milliseconds busy_timeout = std::chrono::milliseconds{5000});

  Catalog(Catalog &&) noexcept = default;
  Catalog &operator=(Catalog &&) noexcept = default;
  Catalog(const Catalog &) = delete;
  Catalog &operator=(const Catalog &) = delete;

  [[nodiscard]] std::string_view lake_root() const noexcept { return lake_root_; }

  // `nullopt` on a miss (not an error). Does not mutate `last_access_ts` --
  // deliberately pure-read; a future retention/eviction task owns deciding
  // what "access" means for that column, out of this task's scope.
  [[nodiscard]] atx::core::Result<std::optional<TrackRow>> probe(const TrackKey &key);

  // Inserts a fresh `status='staging'` row (file/row_group NULL,
  // created_ts == last_access_ts == now). Err(AlreadyExists) if `key` is
  // already registered (staging, compacted, or retired) -- re-staging the
  // SAME track is TrackStore::write_staging's job (it overwrites its own
  // Parquet file idempotently); the catalog row for an already-known key is
  // not re-created or updated by this call. Err(InvalidArgument) on an empty
  // `meta.underlier`/`meta.family`/any `TrackRegistration` field, or
  // `date_max < date_min`.
  //
  // Task D5, economics-rev SUPERSESSION: immediately after the insert
  // succeeds, retires (status -> `Retired`, see that enumerator's doc
  // comment) every row sharing this call's `meta.underlier`/`meta.family`/
  // `registration.config_json`/`registration.data_snapshot_id` -- the parts
  // of a track's identity that stay byte-identical across a
  // `kBacktestEconomicsRev` bump, since none of them encodes `engine_id`
  // (track_key.hpp) -- whose `economics_rev` is STRICTLY LESS than this
  // call's `registration.economics_rev`. A plain integer comparison, never
  // `created_ts`/wall-clock, so which generation's row happens to land first
  // in real time cannot change the outcome (I1-I8). Retiring never touches
  // `file`/`row_group` or deletes anything -- an old row that was already
  // compacted keeps pointing at its real Parquet data; both generations stay
  // `probe()`-able and `catalog()`-queryable. A row can therefore be
  // registered as `Retired`'s target zero, one, or several times over its
  // life (once per LATER generation that ever gets registered against the
  // same variant) -- always idempotent, since re-setting an already-retired
  // row to `Retired` is a no-op.
  [[nodiscard]] atx::core::Status register_staging(const TrackKey &key, const TrackMeta &meta,
                                                    const TrackRegistration &registration);

  // Transitions `key`'s row from staging to compacted, recording which
  // Parquet batch file and row group it landed in (D2's `compact()`).
  // Err(NotFound) if `key` is not registered at all. Err(InvalidArgument) if
  // it is registered but not currently `staging` (already compacted or
  // retired -- a double-compact is a caller bug, not silently accepted) or
  // if `file` is empty.
  [[nodiscard]] atx::core::Status mark_compacted(const TrackKey &key, std::string_view file,
                                                  std::int64_t row_group);

  // Appends one row to `trials` -- EVERY variant ever attempted, per the
  // schema's own comment, not just the ones that panned out. `sharpe`
  // absent means the trial did not produce a usable Sharpe estimate (a
  // degenerate/empty return series, say) -- stored as SQL NULL, still
  // counted in `trial_stats().n_trials` (it was still an attempted, and
  // therefore multiple-testing-relevant, trial) but excluded from
  // `sr_variance`. Returns the assigned `trial_id`. Err if `track_key` does
  // not name a registered track -- enforced by the `trials.track_key`
  // foreign key, so the code is whatever atx::core::db maps SQLite's
  // SQLITE_CONSTRAINT to: `ErrorCode::AlreadyExists` (see sqlite.cpp's
  // map_code -- shared across every constraint violation, not specific to
  // foreign keys, and out of this task's scope to change). Counterintuitive
  // for a "the referenced row is missing" failure, but accurate to what a
  // caller actually observes -- pinned by catalog_test.cpp so a future
  // atx-core remap cannot silently drift this comment out of sync.
  [[nodiscard]] atx::core::Result<std::int64_t>
  record_trial(const TrackKey &track_key, std::string_view sweep_id, std::optional<double> sharpe);

  // `TrialStats` for every trial recorded under `sweep_id` (B4's `dsr()`
  // input). `n_trials` counts ALL rows for `sweep_id`, whether or not they
  // carried a usable `sharpe` -- Bailey-LdP's N is "how many independent
  // variants were tried", which does not shrink just because some failed to
  // produce a Sharpe. `sr_variance` is the SAMPLE variance (Bessel's
  // correction, N-1 denominator) of the non-NULL `sharpe` values only;
  // `{0, 0.0}` for an unknown/empty `sweep_id`, and `sr_variance == 0.0`
  // whenever fewer than 2 trials have a non-NULL `sharpe` (a variance needs
  // at least 2 samples -- reported as 0, not NaN, so a caller who forgets to
  // guard `n_trials <= 1` still gets a well-defined, non-poisoning number;
  // `dsr()` itself separately guards `n_trials <= 1` regardless of this
  // value, per tearsheet.hpp's own doc comment).
  [[nodiscard]] atx::core::Result<TrialStats> trial_stats(std::string_view sweep_id);

private:
  Catalog(atx::core::db::Database db, std::string lake_root)
      : db_{std::move(db)}, lake_root_{std::move(lake_root)} {}

  atx::core::db::Database db_;
  std::string lake_root_;
};

} // namespace atx::vol
