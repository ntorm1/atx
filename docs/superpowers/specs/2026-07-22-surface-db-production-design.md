# Surface Database Production Feature — Design

Date: 2026-07-22
Status: Approved for implementation (user pre-authorized full pipeline + $100 data spend)
Scope: atx-vol

## 1. Goal

Turn `surface_db.hpp` / `surface_db_populate.hpp` / `surface_archive.hpp` into a
complete production feature:

1. A **surface database system** that manages millions of vol surfaces,
   **auto-generates and stores per-symbol fit configurations**, and
   **auto-builds** when pointed at a parquet directory of OPRA slices.
2. A **redesigned OPRA on-disk layout**: hive-partitioned parquet where **each
   date is one file** (all symbols in it), replacing the per-symbol partition
   tree `{symbol}/{date}.parquet`.
3. A **first production dataset**: OPRA slice partitions + a built surface
   database covering **2026-07**, using up to **$100** of real Databento spend,
   built incrementally (smoke first, never re-pulling data already on disk).

## 2. Current state (what exists, what is missing)

Existing and production-grade:

- `SurfaceArchive` v3 / `SurfaceArchiveV2` (ATXVSA2): CRC-layered, mmap,
  zero-copy per-symbol views. Partition file format is done.
- `SurfaceDb`: manifest (`manifest.atxdb`) with fixed-width `SymbolFitConfig`
  records + partition index, atomic rewrite, generation counter, LRU partition
  view cache, `map_surface`/`load_surface`. Done.
- `populate_surface_db` + `populate_universe_streaming`: streaming fit→write,
  cell-aware idempotent resume, would-drop safety guard. Done.
- Loaders: `load_opra_cbbo_parquet` (one symbol/file), `load_opra_daterange`
  (`OpraBatchSpec`, path template `{symbol}/{date}.parquet`),
  `corpus_board_from_opra`. All assume **one underlier per parquet file**.
- Auto-config machinery: `select_curve` (`curve_selector.hpp`),
  `select_fit_policy` / `FitDecision` (`fit_policy.hpp`),
  `symbol_config_from_preset`, `SymbolFitConfig` storage in the manifest.
- Pull tooling: `tools/pull_opra_universe_batch.py` (Databento OPRA.PILLAR
  cbbo-1m, 19:55Z snapshot minute, free `get_cost` preflight, hard cap,
  resume-by-file-existence, per-symbol hive output).
- On disk today: `C:/atx-data/spy-dispersion/opra/{symbol}/{date}.parquet`,
  ~50 symbols x 135 sessions (2026-01-02 .. 2026-07-17), already paid for.

Missing (this design):

- Date-partitioned hive format + a C++ loader for it.
- Auto symbol-config generation wired into the db (generate → store → reuse).
- A one-call **build driver** (dir of slices in → populated SurfaceDb out),
  as a C++ API, a CLI tool, and a Python binding.
- Migration of the existing symbol-partitioned hive (zero re-spend).
- New-layout pull tool + the 2026-07 production dataset.

## 3. OPRA hive v2 — date-partitioned parquet

### Layout

```
<root>/date=YYYY-MM-DD/data.parquet     # ALL symbols for that session
```

- True hive partitioning (`date=` key) so DuckDB / pyarrow.dataset read the
  tree natively with partition-column inference.
- Exactly one parquet file per session. A date's file is written atomically
  (tmp + rename) and is the unit of resume.
- Schema: the existing 8 columns, unchanged
  (`ts, underlying, symbol, instrument_id, bid_px, ask_px, bid_sz, ask_sz`;
  px int64 1e-9 fixed-point, unset side = INT64_MIN).
- Rows sorted by `underlying`, then `symbol`, so `underlying` column
  statistics support predicate pushdown for future selective readers. The
  v1 C++ loader does **one materialized read per date file** and splits by
  `underlying` in memory (one IO pass per date regardless of universe size);
  per-symbol row-group pruning is a documented future optimization, not
  built now.
- Snapshot minute stays the fixed **19:55:00Z** hive convention (uniformity
  with the existing corpus; DST rationale documented in the v1 pull tool).

### Resume / merge semantics (avoid repeat pulls)

The pull unit is (date, symbol-set). On resume:

1. If `date=<d>/data.parquet` absent → pull all requested symbols for d.
2. If present → read its parquet footer statistics (distinct `underlying`
   row-group stats — no data scan) to get the on-disk symbol set; pull only
   the missing symbols; rewrite the date file as the **union** (atomic).
3. Raw per-day DBN cached under `<root>/_dbn/` exactly as v1, so a crash
   between download and split never re-bills.

### C++ loader

New header `atx/vol/opra_hive.hpp` (+ `src/opra_hive.cpp`):

```
struct OpraHiveSpec {
  std::string root_dir;                // hive root holding date=*/ dirs
  std::string date_lo, date_hi;        // inclusive "YYYY-MM-DD"
  std::vector<std::string> symbols;    // empty = every underlying present
  std::string snapshot_suffix = "T19:55:00Z";
  double r = 0.0;
  std::vector<double> yc_pillar_t, yc_pillar_r;
  CorpusMarketInputTable market_inputs{};
  MissingMarketInputPolicy missing_market_inputs{...UseFallback};
  unsigned n_threads{0};               // per-date read fan-out
};
Result<OpraBatchResult> load_opra_hive(const OpraHiveSpec &spec,
                                       const OpraBatchProgress &progress = {});
```

- Enumerates `date=*` dirs in range; missing dates are non-fatal
  (`n_missing`), matching `load_opra_daterange` semantics.
- Reads one date file, splits rows by `underlying`, and feeds each group
  through the **same validation path** as `load_opra_cbbo_parquet` (one
  underlier per panel, same error taxonomy), producing the same
  `OpraBatchResult` shape — so `corpus_board_from_opra` and everything
  downstream (populate, backtests) work unchanged.
- Per-symbol subset loads use row-group pruning on `underlying` stats.
- Deterministic entry order: date-major then symbol-major, byte-identical
  result for any `n_threads` (same contract as `load_opra_daterange`).

## 4. Auto symbol-config generation

New header `atx/vol/surface_db_build.hpp` (+ src), part 1:

```
struct AutoConfigSpec {
  std::string config_date;             // board date used for selection; "" = first available per symbol
  FitPreset preset{FitPreset::Populate};
  std::string index_symbol{};          // pinned to dense index recipe (as UniversePopulateSpec)
  bool overwrite_existing{false};      // false = idempotent skip if symbol already configured
};
Result<AutoConfigReport> generate_symbol_configs(SurfaceDb &db,
                                                 std::span<const CorpusBoard> boards,
                                                 const AutoConfigSpec &spec);
```

- For each symbol: take its `config_date` board, run `select_curve` +
  `select_fit_policy` to produce a `SymbolFitConfig` (curve kind/knobs pinned
  when the selector is confident, preset auto-selection otherwise; admission
  posture from the policy decision), then `db.upsert_symbol` with provenance.
- Idempotent by default: a symbol already in the manifest is left untouched
  (`n_skipped_existing`), so re-running a build never clobbers operator
  overrides. `overwrite_existing` is the explicit escape hatch.
- A symbol whose selection fails is stored **disabled** with the fallback
  preset config (`enabled=false`, reason in the report) — fail closed, never
  silently served.

## 5. Auto-build driver

`surface_db_build.hpp`, part 2 — the one-call production entry point:

```
struct SurfaceDbBuildSpec {
  std::string db_root;                 // created if absent
  OpraHiveSpec hive;                   // where the slices live + range + universe
  AutoConfigSpec auto_config{};
  FitPreset preset{FitPreset::Populate};
  unsigned fit_workers{0};
};
struct SurfaceDbBuildReport {
  AutoConfigReport config;
  UniversePopulateCoverage coverage;   // from populate_universe_streaming
  std::size_t n_dates_loaded{}, n_dates_missing{}, n_load_errors{};
};
Result<SurfaceDbBuildReport> build_surface_db(const SurfaceDbBuildSpec &spec);
```

Orchestration: create-or-open db → `load_opra_hive` → boards via
`corpus_board_from_opra` → `generate_symbol_configs` → 
`populate_universe_streaming`. Fully resumable at every stage (hive resume,
config idempotence, cell-aware populate resume): **re-running an unchanged
build RE-FITS zero and spends $0**. The gate is `coverage.cells_refit == 0`,
**not** `cells_to_fit == 0`: a permanently-failing cell is absent from its
partition, so it is rescheduled and re-attempted forever and its date is
rewritten on every run while its healthy siblings are carried. Measured on
`prod-2026-07`, pass 2: `cells_refit 0`, `cells_carried 150`, `cells_to_fit 3`,
`cells_ok 0`, `cells_failed 3`, exit 0. Only a database with no permanently-
failing cell reaches `cells_to_fit == 0`.

Consumers:

- CLI: `tools/surface_db_build.cpp` (arg parsing → spec → report to stdout +
  a stats CSV via `write_populate_stats_csv`).
- Python: `atxvol` bindings — `SurfaceDb` open/query (exists in
  `bindings/surface_db.cpp`) extended with `build_surface_db`,
  `populate` coverage/report structs, and hive load introspection, so a
  notebook can build and query the same db the C++ tools produce.
  **Shipped with a known limitation:** `import atxvol` and `import pyarrow` in
  the same interpreter collide over Arrow DLLs, so the notebook route is not
  usable wherever `pyarrow` is present (loud `ImportError`, never wrong
  numbers). Building *and verifying* a database needs no Python at all — that is
  what `atx-vol-surface-db-build` and `atx-vol-surface-db` are for; see
  `atx-vol/python/README.md` for the exact error text in both directions.

## 6. Scale posture (millions of surfaces)

Target shape: thousands of symbols x ~250 sessions/yr → 1M surfaces ≈ 4k
symbols x 250 dates. Per-date partition file: 4k surfaces x ~2–6 KB ≈ 10–25 MB
— comfortably inside `SurfaceArchiveV2` mmap + the LRU view cache (16 resident
partitions default; O(1) `map_surface` probe per query). Manifest: 4k x 256 B
symbol records + 250 x 128 B partition records/yr ≈ ~1 MB, rewritten atomically
— fine at this scale. Explicit limits documented and asserted: partition key ≤
32 chars, symbol ≤ 32 chars. Multi-year growth (manifest > ~100 MB or
partitions > ~100k) is out of scope; the seam is one-db-per-year roots, noted
in docs — no sharding built now (YAGNI).

## 7. Migration + pull tooling (Python)

- `tools/migrate_opra_hive.py`: old `{symbol}/{date}.parquet` tree → new
  `date=*/data.parquet` hive. Pure local IO, $0, atomic per date,
  idempotent (existing complete date files skipped), verifies row counts and
  schema equality per date, writes a migration manifest CSV.
- `tools/pull_opra_hive.py`: v2 pull targeting the new layout. Reuses the v1
  discipline verbatim: free `get_cost` preflight for missing cells only,
  hard `--cap` with degrade-to-top-N-by-weight, `--dry-run`, DBN cache,
  atomic writes, spend log. Differences: one `get_range` per date over the
  union of missing parents (fewer API round-trips), output = merged date file
  (per §3 resume/merge), manifest rows per (date, symbol).

## 8. Production run — 2026-07 (≤ $100)

Phased, cheapest-first; cumulative spend logged at every step; hard stop at cap.

1. **Migrate** (free): existing ~50-symbol Jan–Jul-17 hive → new layout at
   `C:/atx-data/opra-hive/`. 2026-07-01..17 sessions become production
   partitions with zero spend.
2. **Smoke** (≈ pennies): `--dry-run` preflight, then pull 3 symbols x 2
   missing sessions (Jul 20–21) into the new hive; run `build_surface_db`
   over Jul 1–22, 3 symbols; verify end-to-end (configs stored, partitions
   written, `map_surface` round-trip, resume run fits zero).
3. **Top-up** (small): pull the full existing universe (~50 names) for the
   missing 2026-07 sessions (Jul 20, 21; then daily as the month advances).
4. **Scale within budget** (optional, only if preflight says it fits): extend
   the universe (e.g. toward top-100 by weight) for the whole 2026-07 month —
   free preflight decides; degrade logic keeps SPY + top-N under the
   remaining cap.
5. **Full build + verification report**: `build_surface_db` over 2026-07,
   whole universe; report coverage, per-symbol success rates, spend total,
   db stats (partitions, surfaces, bytes).

## 9. Testing

TDD per component (superpowers:test-driven-development):

- `opra_hive`: synthetic multi-symbol date parquet fixtures (written by a
  test helper via the existing parquet writer path); loader parity test —
  same rows through old per-symbol layout and new hive layout produce
  byte-identical `OpraPanel`s; row-group pruning subset test; missing-date /
  malformed-file taxonomy tests; determinism across `n_threads`.
- `generate_symbol_configs`: idempotence, overwrite flag, disabled-on-failure,
  provenance round-trip through the manifest.
- `build_surface_db`: end-to-end on fixtures; re-run fits zero (cell-aware
  resume); report field correctness.
- Python: migration tool round-trip on a temp tree (old→new→loader-parity via
  bindings); pull tool logic tests with a faked Databento client (no network,
  no spend); pytest for new bindings.
- Real-data gates (run phase, not CI): smoke build on 3 real names before any
  scale spend.

## 10. Error handling

- Fail-closed everywhere the money or the data could be wrong: pull tool
  blocks over-cap, loader rejects schema drift (ParseError), configs store
  disabled-on-selection-failure, populate keeps the would-drop partition
  guard, db manifest CRC/schema-hash validation unchanged.
- Missing data is graceful: absent dates non-fatal (`n_missing`), un-pulled
  window → zero-coverage no-op build.
- Every phase emits a machine-readable report (CSV/stdout) so a resumed run
  can be audited against the previous one.
