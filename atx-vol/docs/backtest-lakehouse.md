# Backtest lakehouse

The lakehouse turns individual `BacktestResult`s into a queryable, deduplicated
history: a content-addressed identity (`TrackKey`), a hive-partitioned Parquet
store for the economics, and a SQLite catalog of what exists and what has been
tried against it. It sits beside, and is independent of, `BacktestDb`
([docs/backtest-db.md](backtest-db.md)) — different root, different storage
format, different consistency mechanism. The library API is `atx::vol::`
(Tier-B, `atx-vol/include/atx/vol/{track_key,track_store,catalog,sweep_driver,
snapshot_pool}.hpp`); the CLI is `track_compact`
(`atx-vol/tools/track_compact.cpp`); the read path from Python is
`atxpy.tracks` (`python/src/atxpy/tracks.py`).

Everything under this root is gated behind the CMake option
`ATX_VOL_LAKEHOUSE` (default ON for `dev`, OFF for the minimal install): the
five headers above compile unconditionally (they are documented
Arrow/SQLite-free at the header level — only the SQLite/Arrow API surface
itself is confined to their `.cpp`s), but `TrackStore::compact`, `Catalog`,
`run_sweep`, `gc`, and `reconcile_stuck_compactions` are link errors, not
compile errors, in an `ATX_VOL_LAKEHOUSE=OFF` build (`atx-vol/CMakeLists.txt`).

## Lake layout

One lake is one directory (`lake_root`), passed to every entry point below:

```text
<lake_root>/
  staging/
    <track_key-hex>.feather          # one Arrow IPC (Feather V2) file per
                                      # fresh track, TrackStore::write_staging
  tracks/
    underlier=<U>/
      family=<F>/
        batch-NNNNNN.parquet         # written by compact()
        batch-gc-NNNNNN.parquet      # written by gc() (see "GC" below) --
                                      # a deliberately distinct numbering
                                      # scheme, so a compact() and a gc() call
                                      # racing on the same partition can never
                                      # compute the same next index and
                                      # collide on one destination filename
  catalog.sqlite                     # + catalog.sqlite-wal / -shm (WAL mode)
```

`staging/` and `tracks/` are `TrackStore`'s (D2, `track_store.hpp`).
`catalog.sqlite` is `Catalog`'s (D3, `catalog.hpp`) — WAL journal mode,
`busy_timeout` on open, single-writer/many-reader by construction. Every
durable write in either half follows the house atomic-publish discipline
(`detail/archive_util.hpp`: unique same-directory temp file, flush, atomic
rename) with SQLite's own WAL journal as `catalog.sqlite`'s documented
exception to that pattern (a live SQLite connection has no "temp copy +
rename" equivalent — durability is delegated to WAL instead).

**Do not confuse this with `BacktestDb`'s own `<root>/readers/` directory.**
`BacktestDb` (a separate, older, unrelated system) marks live readers with
file-based `BacktestReaderMark`s (`<root>/readers/reader-<pid>-<nonce>.mark`)
to guard its own `vacuum_unindexed_partitions()`. The lakehouse's reader
protection is a SQLite `reader_marks` table inside `catalog.sqlite` (see
"GC and crash recovery" below) — the two mechanisms share one platform PID-
liveness primitive (`detail::process_alive`, `detail/writer_lock.hpp`) but are
otherwise independent; a mark registered against one has no effect on the
other.

## Schema v1 (33 columns)

One row is one `(track_key, date)` observation. Row order within a batch file
is `(track_key, date)` — track_key primary, date secondary — one row group per
batch file, zstd-compressed.

| Group | Columns |
|---|---|
| Identity (3, non-null) | `track_key` (lowercase 64-hex `TrackKey::hex()`), `date` (`date32`), `ts_ns` (`int64`) |
| Frozen series (25, `float64` non-null) | `pnl_total`, `pnl_delta`, `pnl_gamma`, `pnl_vega`, `pnl_vanna`, `pnl_volga`, `pnl_theta`, `pnl_rho`, `pnl_charm`, `pnl_unexplained`, `pnl_settlement`, `pnl_shares`, `financing`, `cost`, `nav`, `cash`, `gross_delta`, `gross_gamma`, `gross_vega`, `gross_theta`, `turnover_notional`, `turnover_vega`, `n_open_lots`, `n_unpriced_lots`, `n_unpriced_greeks` — `kBacktestSeriesColumns` order (`detail/backtest_series_columns.hpp`), zero-copy from `BacktestResult`'s SoA vectors |
| Swap lane (5, `float64` NULLABLE) | `swap_pv`, `swap_pnl`, `gross_vega_abs`, `nav_liquidation`, `step_pnl_total` — the frozen TSV/RunArchive wire set can never carry these; NULL on every row when the source `BacktestResult` vector is empty (never a fabricated `0.0`), and `step_pnl_total` is additionally always NULL on row 0 (it has one fewer element than every other column: no step precedes the inception row) |
| Virtual (2, DuckDB hive columns, not stored) | `underlier`, `family` — synthesized from the `underlier=<U>/family=<F>` path segments when reading with `hive_partitioning=1` |

`underlier`/`family` are caller-supplied placement metadata (`TrackMeta`) —
neither `TrackKey` nor `BacktestResult` carries a queryable label, so
`TrackStore` cannot derive them itself.

## `TrackKey` recipe

```
TrackKey = SHA-256( len(canonical_config) || canonical_config
                   || len(engine_id)       || engine_id
                   || data_snapshot_id )
```

(`track_key.hpp`, `make_track_key`). The two variable-length inputs are
8-byte-little-endian length-prefixed so no boundary shift between them can
collide two different `(config, engine_id)` pairs onto the same bytes;
`data_snapshot_id` is fixed at 32 bytes and needs no prefix.

- **`canonical_config`** (`canonical_config_bytes(strategy_template,
  run_config)`) — a deterministic byte encoding, not a `RunArchive` section:
  every field is written individually (no struct-memcpy, so no
  compiler-inserted padding byte ever reaches the hash), doubles go through
  `std::bit_cast<uint64_t>` with `-0.0` normalized to `+0.0`, enums through
  their declared underlying integer, little-endian throughout. It covers
  `BacktestStrategyTemplate`'s existing economic fingerprint
  (`fingerprint_backtest_template`, reused verbatim) plus a field-by-field
  subset of `RunConfig`.
- **`engine_id`** (`make_engine_id()`) — `ATX_VOL_VERSION_STRING` +
  `kBacktestEconomicsRev` + `ra_schema_hash()`, joined by `|`. "Which build of
  the engine, at which economics revision, against which on-disk schema" — a
  property of the binary that ran, not of one run's settings.
- **`data_snapshot_id`** — a caller-computed SHA-256 over the sorted per-date
  `SurfaceDb` partition content identities the run actually consumed. Computed
  by the caller, not by this header: `track_key.hpp` is Tier-B (promoted from
  `research/` by this task) and deliberately does not depend upward on
  `backtest_db.hpp` or `SurfaceDb`, to keep the layering one-directional.

### Economics vs. execution — the `RunConfig` split (13 / 9 of 22 fields)

`RunConfig` has exactly 22 fields (`aggregate_arity_is_v<RunConfig, 22>`,
`backtest.hpp`). Getting the split wrong toward omitting an economics field
silently serves a wrong cached result; getting it wrong toward including an
execution field only costs a cache hit — so the list below is deliberately
biased toward inclusion. Full field-by-field rationale, with citations to each
field's own doc comment, lives in `track_key.hpp`'s header comment; this is
the summary a reviewer needs without opening it.

**Included (economics — changes what a run computes, or whether it produces a
result at all):** `query_pricing_tier`, `query_cache_build_policy`,
`frictions` (`FrictionModel`, including `crossing_fraction_single/_complex`
and whether `quote_lookup` is set), `financing` (`FinancingConfig`, including
`reference_uid`/`flat_r` presence-and-value), `unpriced`,
`surface_provenance_policy`, `reconcile_nav` / `reconcile_nav_tol` (fail-closed
abort — see "what invalidates what" below), `book_entry_fill_slippage`,
`swap_fixing_cadence`, `clock_gaps`, `margin_breach`, `exercise_policy`.

**Excluded (execution — changes only how fast, or on what topology, the SAME
result is computed):** `price` (`PriceOptions`: `n_threads` is bit-identical
by design; `analytic_greeks` is bit-identical for price and every Greek
**except theta/charm** — see the callout below), `record_every_n`,
`step_observer`, `cancel`, `snapshot_cache`, `snapshot_pool`,
`prefetch_snapshots`, `prefetch_depth`, `settlement_mark_memo`.

**Known limitation, stated rather than silently absorbed:** a track's
`pnl_theta`/`pnl_charm` columns are compute-path-dependent
(`analytic_greeks` on vs. off — FD vs. exact PDE value) but `PriceOptions` is
entirely excluded from the key. `TrackKey` is scoped to economics (NAV / cash
/ P&L), not to every stored column; a consumer that needs theta/charm
reproducibility from the cache needs its own key component for
`PriceOptions`.

## Writing and compacting

1. **`TrackStore::write_staging(key, result, meta)`** — writes one fresh
   track to `<lake_root>/staging/<key.hex()>.feather`, atomically published.
   Rejects a malformed `meta`, a shape-inconsistent `result`, or rows not
   strictly ordered by `(date, ts_ns)`.
2. **`compact(lake_root) -> Result<CompactStats>`** — folds every staged file
   into hive-partitioned batches under `tracks/underlier=<U>/family=<F>/`,
   targeting 256–512 MB compressed per batch file. A staged input is deleted
   only after its batch file's rename has durably landed, so a crash mid-run
   leaves every not-yet-batched file exactly where the next `compact()` call
   will find it — nothing is double-counted or lost.
   `CompactStats::placements` names, per track, the `(file, row_group)` it
   landed in — the exact input `Catalog::mark_compacted` needs.

Neither `TrackStore` nor `compact()` touches the catalog — `track_store.hpp`
is deliberately Catalog-free (one-directional dependency graph). The
`track_compact` CLI (below) is where writing and cataloging meet.

## The catalog (`Catalog`, `catalog.hpp`)

`Catalog::open(lake_root, busy_timeout = 5s)` creates or opens
`catalog.sqlite`, applies WAL/`busy_timeout`/`synchronous` pragmas, verifies
WAL actually took, and creates the `tracks`/`trials`/`reader_marks` schema
(idempotent). Core API:

- `probe(key) -> Result<optional<TrackRow>>` — cache-first lookup, pure read.
- `register_staging(key, meta, registration)` — inserts a fresh `Staging`
  row; `Err(AlreadyExists)` if `key` is already known. Immediately after,
  **supersedes older-economics-generation rows** (D5): every row sharing
  `underlier`/`family`/`config_json`/`data_snapshot_id` with STRICTLY lower
  `economics_rev` transitions to `Retired` — a plain integer comparison, never
  wall-clock, so registration order across processes cannot change the
  outcome. This is what makes a `kBacktestEconomicsRev` bump self-cleaning:
  every rerun of an old variant retires its own stale generation automatically.
- `mark_compacted(key, file, row_group)` — `Staging` → `Compacted`.
- `record_trial(track_key, sweep_id, sharpe)` / `trial_stats(sweep_id) ->
  TrialStats{n_trials, sr_variance}` — every attempted variant, not just the
  ones that panned out (B4's Deflated Sharpe `N`); `trial_stats` is fed
  directly into `atx::vol::dsr()` (`tools/tearsheet.hpp`).
- `list_by_status(status)` — every row at one `TrackStatus`, for a bulk
  scan (crash-recovery, retention).

`TrackStatus` is `Staging` → `Compacted` → `Retired`. A row is **never
deleted** — `Retired` is reached two ways (D5 supersession above, or D6's
age-based `retire_stale` below) and stays `probe()`-able and
`catalog()`-queryable forever. `file`/`row_group` on a `Retired` row are
non-null (the Parquet data is still there) **unless** `gc()` has physically
reclaimed the bytes, at which point `apply_gc_rewrite` clears both to NULL —
"NULL means reclaimed, non-null means still resolvable" is the whole rule.

## The sweep driver (`run_sweep`, `sweep_driver.hpp`)

`run_sweep(SweepSpec, SweepConfig) -> Result<SweepResult>` turns a grid of
`BacktestStrategyTemplate` variants into tracks, cache-first:

1. **Enumerate + dedupe** — every variant canonicalizes to a `TrackKey` over
   `(variant, SweepSpec::base_config, SweepSpec::data_snapshot_id)`; only the
   first occurrence of a repeated key is scheduled.
2. **Cache-first** — each unique key is `Catalog::probe`d. A hit skips the
   run entirely (and, since `TrackKey` already folds `engine_id` — which
   folds `kBacktestEconomicsRev` — a hit can only ever be a track computed
   under the *current* economics revision: there is no separate freshness
   check to get wrong).
3. **Variant-parallel execution** — every miss runs `run_backtest` with
   `price.n_threads` forced to 1; the outer fan-out
   (`SweepConfig::n_threads`) is what runs variants concurrently. Every run
   shares `SweepConfig::snapshot_pool` (C2, `snapshot_pool.hpp`) so N
   variants over the same corpus open each archive once between them,
   bit-identically to a solo run (I1–I8).
4. **Publish** — back on the calling thread only (never inside the fan-out —
   `atx::core::db::Database` must not be shared across threads), each fresh
   miss is `write_staging`'d then `register_staging`'d.
5. **Trial registration** — `record_trial` is called once per *original*
   variant (including duplicates and cache hits) — the `trials` table counts
   attempts, not unique configs.

Fail-closed on anything structural (a variant that fails
`validate_backtest_template`, any catalog/store error) — aborts the whole
sweep, matching the rest of this sprint's posture.

## GC and crash recovery

**`reconcile_stuck_compactions(catalog, lake_root)`**
(`track_compact_reconcile.hpp`, D5 fix-round) repairs a specific crash window:
a process that dies after `compact()` deletes a track's staged input but
before that track's `mark_compacted` call lands leaves the row stuck at
`Staging` forever (nothing left in `staging/` for a plain re-run to find). For
every `Staging` row whose staging file is absent, it relocates the row
directly in the lake (scans the row's own hive partition's batch files for a
matching `track_key` column value) and marks it compacted. Idempotent, and
itself safe to interrupt at any point.

**`gc(lake_root, older_than_ts_ns) -> Result<GcStats>`** (`track_gc.hpp`, D6):

1. `Catalog::retire_stale(older_than_ts_ns)` — every `Compacted` row whose
   `last_access_ts` is older than the threshold → `Retired`.
2. For every batch file any `Retired` row still points at: **skip it
   entirely** if `Catalog::has_live_reader_mark(file)` reports a live
   advisory mark; otherwise delete the file outright if every track in it is
   retired, or rewrite it without the retired rows' data and repoint the
   survivors (`Catalog::apply_gc_rewrite`) if some rows survive.

Deletion ordering is fixed: the new/rewritten file is durably published
*before* the catalog is touched; `apply_gc_rewrite` commits one transaction
that repoints survivors and clears reclaimed rows to NULL; only then is the
old file removed, best-effort (`GcStats::old_files_not_removed` counts a
failure here — disk left unreclaimed, never a correctness hazard). A crash
between the catalog commit and the old-file removal leaves a harmless orphan.

**Reader marks.** `Catalog::mark_reader(file)` / `release_reader_mark(id)`
register/release a shared advisory mark tagged with the calling process's own
PID (never forgeable against another process); `has_live_reader_mark`
opportunistically deletes any mark whose PID is confirmed dead while scanning
(self-healing against a reader that crashed without releasing). Multiple
marks may coexist — this is many-reader registration, never mutual exclusion.

### `track_compact` CLI (`atx-vol/tools/track_compact.cpp`, `ATX_VOL_LAKEHOUSE` only)

```
track_compact <lake_root>
    compact()s every staged track, mark_compacted()s each placement in the
    catalog, then unconditionally runs reconcile_stuck_compactions() — every
    invocation, whether or not this call's compact() found anything new,
    since the crash it recovers from may have happened during any earlier run.

track_compact gc <lake_root> <older_than_ts_ns>
    Runs gc() with the given nanoseconds-since-epoch threshold. A separate,
    explicitly-invoked subcommand — never run automatically by the default
    invocation — because GC is destructive (retires and eventually reclaims
    tracks) and needs an operator-chosen age policy.
```

Both subcommands are fail-closed: any error aborts the run and reports a
non-zero exit code naming what failed.

## What invalidates what

| Change | Effect |
|---|---|
| Any of the 13 economics `RunConfig` fields, or the strategy template's economic fingerprint | Different `canonical_config` → different `TrackKey`. Old track stays queryable at its own key; a new run under the new config is cache-miss and gets its own key. |
| `data_snapshot_id` (different source `SurfaceDb` content) | Different `TrackKey`, same as above — the lake never conflates two runs over different market data. |
| A `reconcile_nav`/`clock_gaps`/`margin_breach` fail-closed field flipped to its strict setting | Different `TrackKey` (all three are economics-included specifically *because* they can make a run abort with no `BacktestResult` at all — a cache keyed as if the lenient branch ran would otherwise silently serve a result for a config that must fail closed). |
| Any of the 9 execution `RunConfig` fields (`price.n_threads`, `prefetch_depth`, `snapshot_pool`, `record_every_n`, …) | **No effect on `TrackKey`.** Two runs differing only here are deduplicated to the same track. |
| `price.analytic_greeks` specifically | No effect on `TrackKey`, but genuinely changes the stored `pnl_theta`/`pnl_charm` values (documented gap — not covered by this identity). |
| `kBacktestEconomicsRev` bumped | `engine_id` changes → every `TrackKey` in existence changes. Every track computed under the old revision becomes cache-unreachable (a probe under the new revision can never hit it); `register_staging`'s D5 supersession then retires the old-generation catalog row the next time that variant is rerun. This must happen together with re-pinning `golden_pin.hpp` (see below) or the build fails to compile. |
| `ATX_VOL_VERSION_STRING` or `ra_schema_hash()` changes | Same effect as an economics-rev bump (both fold into `engine_id`) even without a `kBacktestEconomicsRev` change — e.g. a release version bump alone invalidates the cache. |
| `last_access_ts` ages past a `gc()` threshold | `Compacted` → `Retired` (D6). The row stays queryable; only a *later* `gc()` call may physically reclaim its bytes, and only once no live reader mark protects its batch file. |
| A batch file's rows are all `Retired` and no live reader mark protects it | `gc()` deletes the file outright; the catalog's `file`/`row_group` for those rows go to NULL. |

## Economics tripwire: the golden 82-session pin

`golden_pin.hpp` (Tier — stays in `research/`; depends on the newly-promoted
`track_key.hpp`) pairs one literal against `kBacktestEconomicsRev`:

```cpp
inline constexpr double kGolden82SessionFinalNav = 247.4065016443293;
inline constexpr int kGolden82SessionEconomicsRev = 1;
static_assert(kGolden82SessionEconomicsRev == kBacktestEconomicsRev, /* ... */);
```

Two-layer enforcement:

1. **Compile time.** Bumping `kBacktestEconomicsRev` without updating
   `kGolden82SessionEconomicsRev` alongside it fails the build — the pairing
   cannot silently drift apart.
2. **Run time.** `TrackKeyGoldenReplay.Pinned82SessionNavUnlessEconomicsRevBumped`
   (`tests/track_key_test.cpp`) replays the pinned 82-session SPY corpus and
   compares the final NAV bit-for-bit against the literal above. It looks for
   the corpus at `$ATX_VOL_GOLDEN_82_SESSION_CORPUS`, then
   `data/golden/82-session-spy` relative to a few candidate working
   directories, and `GTEST_SKIP()`s cleanly (naming what it checked) when
   absent — which is every worktree today. **Wiring an actual corpus into
   CI, and completing the replay call against the real dispersion-backtest
   pipeline, is out of this task's scope** (this doc does not claim it is
   wired) — the sprint's CI gate for this tripwire lands in Task E3.

**Known open discrepancy, stated rather than hidden.** A prior task in this
sprint (A3) found that `247.4065016443293` predates an earlier (pre-sprint)
dispersion sizing-unit migration and, reproduced directly against the real
82-session corpus on that task's machine, measured the corpus's actual
current NAV as `24740.624124996561` — roughly 100× the literal above. That
finding was out of A3's scope to fix and is repeated here so a reader of this
doc, and whoever wires Task E3's CI gate, has it in front of them rather than
discovering it independently: **the literal `golden_pin.hpp` ships today may
need re-measurement and re-pinning before E3 can rely on it**, independent of
anything in this task.

## Python / DuckDB query cookbook (`atxpy.tracks`, `pip install atxpy[lakehouse]`)

`atxpy.tracks` is a **read-only** view — it never opens a Parquet file for
write and never takes a lock, so it can never block a concurrent C++ writer.

```python
from atxpy import tracks

# All tracks for one (underlier, family), sorted (track_key, date).
# underlier/family push down to hive-partition pruning; date_range/track_keys
# push down to Parquet row-group statistics.
tbl = tracks.load(
    "/path/to/lake_root",
    underlier="SPY",
    family="strangle-40d-3cm",
    date_range=("2026-01-02", "2026-06-30"),
    columns=["track_key", "date", "nav", "pnl_total"],  # default: every column
)  # -> pyarrow.Table

# The D3 catalog's `tracks` table, one row per registered track (any status).
cat = tracks.catalog("/path/to/lake_root")  # -> pandas.DataFrame

# T x N daily-return pivot for every track trialled under one sweep_id,
# feeding directly into atxpy.pbo.cscv_pbo() (B5's CSCV/PBO harness).
rm = tracks.returns_matrix("/path/to/lake_root", sweep_id="sweep-2026-08-05")
```

`tracks.ALL_COLUMNS` is the allow-list `columns=` is validated against before
the SQL is built (never string-interpolated — every filter value is a bound
DuckDB/SQLite parameter). `tracks.catalog()`/`tracks.returns_matrix()` connect
to `catalog.sqlite` with SQLite's own `mode=ro` URI flag — it refuses to
create the file and refuses any write at the driver level.

## Known operational gaps (stated honestly)

- **`atxpy.tracks` reads Parquet with no advisory mark.** `load()` (and
  `returns_matrix()`, which calls it) scans `tracks/**/*.parquet` directly
  through DuckDB with no registered reader mark — it is not, and cannot
  become without a Python-side `Catalog` binding, a *registered* reader in
  `has_live_reader_mark`'s sense. A `gc()` run racing an in-flight Python
  scan can delete or rewrite a file that scan is mid-read on. This is the
  advisory-mark mechanism's documented scope (it protects registered C++
  readers only), not a bug being silently carried.
- **Concurrent `gc()` runs are not mutually exclusive.** Unlike `BacktestDb`'s
  vacuum (which refuses outright while any `BacktestReaderMark` is live),
  nothing in `gc()` takes a process-level lock against a second, simultaneous
  `gc()` invocation over the same `lake_root`. Each individual `Catalog`
  mutation stays transactionally safe (`retire_stale`/`apply_gc_rewrite` are
  no-ops on a row already in its target state, so re-running the same work
  twice is wasted effort, not corruption), but nothing coordinates two `gc()`
  processes beyond that — running one `gc()`/`track_compact` actor at a time
  per lake is the operational assumption, not something enforced. This is a
  separate concern from a `compact()` and a `gc()` racing each other: THAT
  case is closed by construction, because `gc()`'s batch files use the
  `batch-gc-NNNNNN.parquet` naming scheme — deliberately distinct from
  `compact()`'s own `batch-NNNNNN.parquet` numbering — so the two writers'
  next-index numbering can never collide on one destination filename even
  when they do run at the same time.
- **The vacuum-guard mechanisms are two different systems that share only a
  liveness primitive.** See the "Do not confuse this with `BacktestDb`'s own
  `<root>/readers/` directory" note in "Lake layout" above.
