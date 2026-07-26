# atx-vol Surface Store / Archive / Snapshot / Populate — Review 03

Scope: the persistence + retrieval layer the backtester reads surfaces from.
Files: `surface_archive.{hpp,cpp}` (ATXVSA v1 major-3 + ATXVSA2 v2 major-4),
`detail/archive_util.{hpp,cpp}`, `surface_db.{hpp,cpp}`, `surface_db_populate.cpp`,
`snapshot_cache.cpp`, `priced_surface_view.{hpp,cpp}`, `dense_slice.cpp`,
`sr_tenor_grid.cpp`. Reviewed tree: `main` @ `99f332f`. READ-ONLY.

## Verdict up front

- **No CRITICAL data-corruption or determinism bug found.** The v2 (ATXVSA2)
  writer/reader round-trip is self-consistent; the F6 content-identity /
  SnapshotCache staleness design is sound (see "Determinism & integrity" below).
- **The "v1-isolation refactor (`archive_v1_support`)" named in the task is NOT
  in the reviewed tree.** It lives on an *unmerged* branch `feat/bt-v1iso`
  (commits `228edf5` link-isolate, `e720f71` define v1 support lib, `5510342`
  compile-fail probe). On `main`, v1 (ATXVSA03/major-3) and v2 (ATXVSA20/major-4)
  fully coexist in one 2323-line `surface_archive.cpp` with no isolation
  namespace/library. The v1 read path is therefore *unchanged* and — verified by
  full read — self-consistent and correct.

## Top 5

1. **[Medium] v1 "clean break" is claimed done but v1 code is fully live in the
   product TU** — `surface_archive.cpp:71-1221` (v1 writer+reader) is compiled
   into `atx::vol`, referenced only by the throwaway migrator
   (`tools/migrate_atxvsa_v1_to_v2.cpp`) and benches, yet `snapshot_cache.cpp:93`
   ("S4 clean break … v1 is gone") and `backtest.cpp` comments assert it is gone.
   The isolation/deletion is on unmerged `feat/bt-v1iso`. *Fix:* land the S4
   deletion (or the link-isolation branch) so product cannot reach v1; pin the
   migrator to a tagged commit.

2. **[Medium/perf] The zero-copy `PricedSurfaceView` win does not reach the
   production backtest hot path** — `MarketSnapshot::load` (`backtest.cpp:968-1041`)
   still owned-reconstructs (`reconstruct_symbol` / `reconstruct_all_with_provenance`,
   per-surface heap) rather than mapping views. `SurfaceDb::map_surface` /
   `load_surface` + the S5 `partition_cache_` LRU (`surface_db.cpp:1175-1285`) are
   exercised **only by tests**, never by product. *Fix (wave-2 B1):* re-point
   `SurfaceSet`/`PortfolioPricer` at `PricedSurfaceView` and route the hot loop
   through `map_symbol`.

3. **[Medium/perf] Whole-partition file read on every open (no mmap)** —
   `SurfaceArchiveV2::open_file` (`surface_archive.cpp:1865-1886`) reads the entire
   `.atxvsa` file into an owned buffer; `MarketSnapshot::load` does this even for a
   single-uid subset load. Across the ~135-partition backtest sweep this is pure
   read amplification. The `open_borrowed` mmap seam (`surface_archive.cpp:1860`,
   `shared_ptr<const void>` owner) exists but is unused. *Fix:* supply an
   `atx::tsdb::Mapping` owner to `open_borrowed`.

4. **[Low/perf] Subset-load re-probes the hash table twice per referenced surface**
   — `backtest.cpp:1014-1018` calls `arch->reconstruct_symbol(sym)` then
   `arch->provenance(sym)`, each re-running `find_slot` (hash probe +
   `canonicalize_symbol` string alloc) even though the directory entry `e` in hand
   already holds `surface_offset`/`surface_size`. *Fix:* reconstruct + read
   provenance directly from `e`'s record extent (one pass, no re-probe).

5. **[Low] v1 SplineVol serialization is lossy** — `slice_payload_size` v1
   SplineVol = `32 + 16n` (`surface_archive.cpp:127-140`) omits `mult_cap` +
   `w_offset`, which v2 fixed (`52 + 16n`). v1 `reconstruct` rebuilds both as `0.0`
   → mispriced SplineVol slice. Latent (v1 write is bench-only; the manifest
   rejects SplineVol at `surface_db.cpp:312`, `curve_kind <= 4`), but a live
   foot-gun while finding #1 stands. *Fix:* delete v1, or add the two f64s and bump
   `kV3Salt`.

---

## Correctness

### Determinism & integrity (verified sound — no bug)

- **Record bytes are byte-deterministic across runs.** Buffers are zero-initialized
  (`std::vector<std::byte> buffer(file_size)`), every field is written via `memcpy`,
  and all inter-column / inter-payload / trailing-pad gaps stay zero. A v2 surface
  *record* contains no wall-clock field (`now_ts_ns` is the deterministic pricing
  timestamp, not a write clock), so identical surfaces serialize to identical
  record bytes and identical `payload_crc32c`.
- **`record_crc_v2` (`surface_archive.cpp:1265-1273`) is correct**: piecewise CRC
  over `[0,168) + 4 zero bytes + [172,size)`, i.e. the whole record with the
  `payload_crc32c` field forced to 0 — matches `validate_record`
  (`:2287-2302`). Writer stores the same value and mirrors it into
  `directory[idx].payload_crc32c` *before* the `metadata_crc32c` is computed
  (`:1655-1677`), so a same-length in-place payload rewrite changes
  `metadata_crc32c` and hence F6 identity. The R-19/F6 staleness contract holds.
- **SnapshotCache** (`snapshot_cache.cpp`) keys on `(path,tier)` and
  `evict_if_stale` on the 256-byte-header F6 identity read *before* the lock; a
  default/unreadable identity is treated as "unknown, do not evict" (no thrash).
  `SurfaceDb`'s S5 cache mirrors this (`surface_db.cpp:1156-1230`) and additionally
  evicts unconditionally on `write_partition`/`drop_partition`. Correct.
- **CRC-32C** (`detail/archive_util.cpp`): SSE4.2 `_mm_crc32` path is CPUID-gated
  and produces bit-identical output to the table fallback (running-state semantics
  match, one-shot applies init/final XOR). Correct and shared by ATXVSA + ATXVDB so
  both agree bit-for-bit.
- **v2 reader robustness vs untrusted files** is solid: `create_over_record`
  (`priced_surface_view.cpp:165-347`) rejects a non-8B-aligned record base, checks
  every column's bounds + natural alignment, and validates each payload extent;
  `open_impl` (`surface_archive.cpp:1744-1852`) validates framing, both CRCs,
  section topology, and record offset alignment. `reconstruct_v2_record` reads
  every field via `memcpy` (alignment-safe). No OOB/unaligned-UB path found.
- **View bit-exactness**: `PricedSurfaceView`'s `kFlatDiscountRelativeTolerance =
  1e-12` and `term_rates`/`interp_forward`/`slice_rate` transcription match
  `priced_surface.cpp:37-48,408-523` (confirmed); gated by
  `corpus_test.cpp` view-vs-fresh-fit tests.

### Other correctness notes

- **[Low] v1 `provenance()` re-CRCs the whole blob per call**
  (`surface_archive.cpp:890-909`, `read_provenance` → `crc32c(base,size)`) — O(blob)
  per query. Off any product hot path (v1 legacy).
- **[Low] `SurfaceArchiveV2::find` returns a partial `ArchiveV2DirEntry`**
  (`surface_archive.cpp:1914-1927`): `n_slices`/`kind_bits`/`payload_crc32c` left 0
  (fabricated from the lookup slot). Currently only presence is read
  (`surface_db_populate.cpp:531`, `corpus.cpp:886`), so harmless today, but a trap
  for any future caller that reads those fields off a `find` result.
- **[Low/info] `ConvexSliceFit` QP diagnostics are not serialized** — only
  `rmse_price`/`n_obs`/`n_active` are stored (v1 and v2). `qp_stationarity`,
  `effective_lambda`, `noise_scale`, etc. rebuild as defaults. Pricing-safe
  (`ConvexDenseCurve::w/iv` read only `u,C,T,F,df`); diagnostics-only loss.
- **[Low] "v2" / "v3" naming collision** — `surface_archive.hpp:1-13` top comment
  calls the *current legacy* format "v3" (magic `ATXVSA03`, major 3) and an *older*
  deprecated format "v2", while `docs/atxvsa2-format.md` + in-code comments call
  major-3 "v1" and the new columnar major-4 format "v2" (`ATXVSA20`). The overload
  already misled the task framing. Also `surface_db.hpp:3` still documents partitions
  as "ATXVSA v3" although `write_partition` writes v2 (`surface_db.cpp:1093`).

## Performance

- **Read amplification**: see Top-5 #2/#3/#4. The current production read path
  (`MarketSnapshot::load` + `SnapshotCache`) does a full-file read + per-surface
  owned reconstruct. The columnar zero-copy store (v2) is built and test-covered
  but not yet on that path.
- **Two parallel caches exist**: `SnapshotCache` (used by the backtest) and
  `SurfaceDb::partition_cache_` (S5, test-only). Consolidation is a wave-2 item.
- **Populate throughput** (`surface_db_populate.cpp`) is well-engineered: LPT
  claim-ordering (`:217-236`), shared bounded fit queue with split inner budget
  (`:278-282`), P-core cap + pinning (`:252-257`), and O(dates-in-flight) RSS via
  the streaming drain (`:339-413`). `pricer_config_for_symbol` + the
  `apply_symbol_config` session-overlay (`:310-315`) *do* convey the FitPreset,
  al_override/al, band_k, calendar_repair and pinned-curve knobs — **not** ignored
  (contra the task hint). Determinism is gated
  (`SharedWorkerBudgetKeepsOutputByteIdentical`).
- **[Low] Whole-date partition rewrite on incremental add** —
  `populate_universe_streaming` (`:515-556`) refits every already-present cell when
  a date gains one symbol ("price of date-keyed partitions"), guarded to never drop
  an absent symbol. A true incremental append would avoid refitting present cells.

## Code not wired in

- **v1 writer + reader** (`surface_archive.cpp:315-1221`): no product caller. Only
  the throwaway migrator + `surface_archive_bench`. (Top-5 #1.)
- **`SurfaceDb::map_surface` / `load_surface` / `LoadedSurface` / `cached_partition`
  / S5 LRU**: product-unused; tests only. (Top-5 #2.)
- **`SurfaceArchiveV2::open_borrowed`** (mmap seam): unused. (Top-5 #3.)
- **`SurfaceArchiveV2::map_all` (zero-copy views)**: used only by `corpus.cpp:881`
  for a count/framing cross-check; the backtest uses `reconstruct_*`, not views.

## Feature gaps vs SOTA

- **mmap random access by (symbol,session)**: designed (`open_borrowed` +
  type-erased owner) but unimplemented — still full-file read. Highest-leverage gap.
- **Compression**: none (records are raw POD; parametric surfaces are tiny, dense/
  spline node arrays are not — a per-record codec is a future lever).
- **Incremental populate**: whole-date rewrite (above); no append.
- **Columnar / zero-copy store**: DONE (v2 SoA columns), but see wiring gap #2.

## v1-isolation status (deliverable answer)

**NOT wired on `main`.** The `archive_v1_support` extraction is on an unmerged
branch `feat/bt-v1iso`; the reviewed tree has v1 + v2 coexisting in one TU with no
isolation. The v1 *read* path is intact and correct as-is (nothing was extracted to
break it). The product-facing claim that "v1 is gone" (snapshot_cache/backtest
comments) is true only in the sense that *no product path calls v1* — the code is
still compiled and reachable via the migrator/benches.
