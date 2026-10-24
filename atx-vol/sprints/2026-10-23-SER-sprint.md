# Lane SER (2026-10-23/24) — surface-archive serialization audit + O(1) point read

Branch `feat/vol-SER-surface-db-20261023` off `main @ a53c48c4`, pool-34. Brief:
`.superpowers/sdd/2026-10-23-vol-bindings-storage/lane-SER-surface-db-brief.md`; full
evidence in that folder's `lane-SER-surface-db-report.md`.

## What and why

`SurfaceDb::map_surface` / `load_surface` served ONE underlying only after reading the
WHOLE partition: `open_file` streamed the file into a heap buffer, then `open_impl`
copied the lookup and directory, CRC'd the metadata and cross-checked every entry. The
format had a hash lookup but no way to use it without the rest of the file, and the
record did not name its symbol, so a slot -> record binding could not be checked from
the record.

ATXVSA2 revision 1 (`ATXVSA21`, major 5; spec `docs/atxvsa2-format.md`) adds the point
read: header (128 B) -> one run of up to 8 lookup slots (each slot carries its own
CRC-32C) -> the record extent, whose header now carries the canonical symbol. The read
verifies the record CRC and the binding, closes its delete-sharing handle and returns
an owned record + zero-copy view. Whole-board readers keep the eager cross-checked open
but read the file with one positional read. The manifest became `ATXVDB02` (named
policy / provenance / fingerprint fields, canonical placement). Compat paths the user
waived are deleted (ATXVSA03 declarations + recogniser, prior-salt accept-list,
zero-marker provenance, the LRU partition cache).

## Numbers (one pass, unqualified, vol-rel, host CPU 18 % before the run)

`atx-vol-surface-archive-bench --benchmark_filter=pointread_study` (6,000 symbols:
Essvi/Svi bulk + 25 ConvexDense + 25 SplineVol; 200 seeded random symbols, a fresh
`SurfaceDb` per cold read; bytes / calls from `GetProcessIoCounters`). Before = the
a53c48c4 binary the lane built before touching the library, re-run back-to-back with
the after binary. Outputs: `C:\atx-cache\quote-benchmark\SER-20261023-pointread\`
(`sha256.txt`).

| metric | before (a53c48c4) | after (ATXVSA21) |
| --- | --- | --- |
| cold `map_surface` p50 / p99 | 11,806 / 14,602 µs | 24.2 / 41.2 µs |
| cold `SurfaceDb::open` + `map_surface` p50 / p99 | 11,946 / 14,756 µs | 46.1 / 67.7 µs |
| cold `load_surface` p50 / p99 | 11,640 / 15,664 µs | 24.5 / 59.4 µs |
| bytes read per cold map (mean / max) | 11,575,296 / 11,575,296 | 2,214 / 5,264 |
| read calls per cold map (mean) | 2,826 | 3 |
| warm map, one handle, p50 / p99 | 18.7 / 37.7 µs (cache hit) | 23.6 / 36.7 µs (point read) |
| whole board `open_partition` + `map_all` p50 | 12,341 µs | 7,557 µs |
| `write_partition` wall | 27,707 µs | 27,879 µs |
| partition file size | 11,571,200 B | 11,329,536 B |

The warm repeated map is ~5 µs slower at p50: the deleted cache served a hit from a
resident buffer after a file open + identity read; every call is now a three-read point
read. That is the price of holding nothing resident and of each call reading one
consistent file version.

## Findings fixed (with failing-then-passing tests)

- Windows republish under concurrent readers: `MoveFileEx`-semantics rename is refused
  while any reader holds the target, even with `FILE_SHARE_DELETE`; 4 / 40 publishes
  exhausted the retry budget. `flush_and_publish_file` now tries a POSIX-semantics
  replace (`FileRenameInfoEx`) first.
- `DbManifest::find_symbol` / `find_symbol_provenance` / `find_partition` truncated a
  > 32-char query onto a stored 32-char record (aliasing). Now refused.
- `SurfaceDbAdmin.VerifyDbDetectsInPlacePayloadCorruption` re-pinned: the point read
  now refuses a rotted record that the old cached map served.

## Follow-ups

- ATXVFA `Lazy` verify level (CRC a section on first touch) — design paragraph in
  `docs/storage/ARCHIVE_FORMAT.md` "Open cost model".
- `SnapshotCache` / `SnapshotPool` identity probes still read the 128-B header through
  `std::ifstream` (no `FILE_SHARE_DELETE`): a probe concurrent with a republish can push
  it onto the retry path. Move them to `detail::PositionalFile`.
- `src/backtest/priced_surface_view.cpp:285` comment still names the deleted
  `ArchivePricingRecord`.
