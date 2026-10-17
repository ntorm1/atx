# Surface-archive lane, resume 5 (2026-10-17)

- Branch: `feat/vol-surface-archive-20261017`, new from base `24e01644`.
- Code tip: `e35ef4c4`. This note is committed on top of it.
- Worktree: pool-34 (RunId `vol-20261017-surface-archive`).
- Spec: `docs/storage/ARCHIVE_FORMAT.md` (written first, committed alone at
  `6f9e7a91`, aligned with the implementation at `e35ef4c4`).

Every timing below comes from `vol-rel` on a shared, loaded host (16 logical
processors, `Win32_Processor.LoadPercentage` 76% before and 66% after the
measured run; 94-99% during the aborted runs). They are UNQUALIFIED: nothing was
pinned and no quiet baseline exists. `ATX_VOL_FIT_WORKERS=3`, `--workers 3`.

## What it is

ATXVFA is a new versioned, checksummed, memory-mappable archive for fitted vol
surfaces and their serving records. It stores:

- quote-slice fits with grade, status, repair diagnostics and the policy digest;
- cross-expiry quote surfaces and their config;
- `ExpiryRecord`s (stored as the object representation and served zero-copy)
  and the spot-ladder sources that rebuild `ChainRecords`;
- common Markov surface specs and an optional fit summary.

Lookup is by (underlier, snapshot, expiry, sub) through a sorted 24-byte index
per record type. Corrupt, truncated or foreign files are refused with a typed
`ArchiveStatus`; the read path throws nothing. After `open`, no query allocates.
Series directories add immutable segments without rewriting history, and
compaction merges segments bit-exactly.

It is opt-in. No existing fitting, pricing, tick-path, serving or benchmark path
reads or writes it.

## Commits

| SHA | What |
| --- | --- |
| `6f9e7a91` | `ARCHIVE_FORMAT.md`, the on-disk spec (work item 1) |
| `ad06d6ec` | Core framing, record types, series, and `atx-vol-fit-archive-tests` (items 2-3) |
| `8b50fc9d` | `atx-vol-surface-archive` CLI (inspect, dump, verify) and its tests (item 4) |
| `73461064` | Fix: archive `moved_nodes` of a slice stripped after a repair (found by the bench) |
| `36c6ffd2` | `atx-vol-fit-archive-bench` (item 5) |
| `e35ef4c4` | Spec aligned with the implementation; new section 11 on the tools |

## Files

All new except the two CMake files, which only gain lines.

- `include/atx/vol/api/storage/fit_archive_format.hpp`: POD layout, constants
  and `ArchiveStatus`, with every size and offset pinned by `static_assert`.
- `include/atx/vol/api/storage/fit_archive.hpp` and `src/storage/fit_archive.cpp`:
  builder, publish, `FitArchive` open and validate, and the views.
- `include/atx/vol/api/storage/fit_archive_records.hpp` and
  `src/storage/fit_archive_records.cpp`: the typed writer, decoders, and record
  validation.
- `include/atx/vol/api/storage/fit_archive_series.hpp` and
  `src/storage/fit_archive_series.cpp`: append, compact, and the series reader.
- `tools/surface_archive_main.cpp` (the CLI) and
  `tools/surface_archive_bench_main.cpp` (the bench).
- Tests: `tests/fit_archive_test.cpp`, `tests/fit_archive_records_test.cpp`,
  `tests/fit_archive_series_test.cpp`, `tests/fit_archive_cli_test.cpp` and
  `tests/support/fit_archive_fixture.hpp`.
- `CMakeLists.txt` gains the three storage sources and the two tools.
- `tests/CMakeLists.txt` gains the focused target and the ctest
  `atx-vol-fit-archive` (label `atx_vol_fast`).

## Bench: dev200 graded-v1 payload

Command (exit 0, wall 103.6 s):

```
atx-vol-fit-archive-bench --input C:\atx-cache\quote-benchmark\20261014-dev200\quotes.csv
  --policy C:\atx-cache\quote-benchmark\slice-coverage-20261016-policies\graded-v1.json
  --reference C:\atx-cache\quote-benchmark\20261014-dev200\20261016-slice-graded-v1-mergeb9374ae7.csv
  --out-dir C:\atx-cache\quote-benchmark\surface-archive-20261017-bench
  --dividends funding-pv --workers 3 --reps 9 --lookups 262144 --ladders on
```

Outputs are in `C:\atx-cache\quote-benchmark\surface-archive-20261017-bench\`.
`SHA256SUMS` in that folder lists the archive (`c7b78164...`) and
`report.json` (`70d9bd05...`). The bench binary's sha256 is `169bd153...`,
built at `36c6ffd2`.

**Payload.** The archive holds:

- 191 snapshots, one per underlier, as 191 surfaces;
- 1,138 expiry fits: grade A 196, B 678, C 89, refused 175;
- 963 served expiries, each with `ChainRecords` built (0 build failures);
- 1,926 `ExpiryRecord`s and 50,348 ladder contracts;
- 63,226 quote rows and 25,456 certified price nodes.

No snapshot took the static fallback.

**Parity with graded-v1 (`1348f63d`).** The reproduced payload matches the
reference predictions exactly:

- 50,348 of 50,348 served rows match bit for bit in price, vol and grade;
- the 12,878 unserved rows are also unserved in the reference.

| Measure | Result |
| --- | --- |
| Archive size | 12,140,032 B; 10,668 B per expiry fit |
| Largest sections | expiry-data 3.51 MB, quote-rows 3.03 MB, ladder-contracts 2.42 MB, fit-records 1.02 MB |
| Encode (all `add_*` calls) | 13.5 ms |
| `finish` (layout + every CRC), n=9 | median 7.51 ms, max 8.75 ms |
| Publish (temp + one fsync + rename), n=9 | median 25.6 ms, max 27.8 ms |
| Open, mapped, `Framing` (warm cache), n=9 | median 5.02 ms, max 6.13 ms |
| Open, mapped, `Full` (default), n=9 | median 5.42 ms, max 6.21 ms |
| Open, mapped, `Paranoid`, n=9 | median 6.97 ms, max 10.28 ms |
| Open, `Read` mode, `Full`, n=9 | median 16.8 ms, max 18.9 ms |
| Cold proxy: unbuffered read (`FILE_FLAG_NO_BUFFERING`), n=9 | median 6.40 ms, max 10.2 ms |
| Cold proxy: then `Full` validate in memory, n=9 | median 2.15 ms, max 3.88 ms |
| `ExpiryRecord` random read: lookup plus one `price()`, single op, n=262,144 | median 500 ns, p99 1,300 ns |
| The same, mean per block of 256 lookups, 1,024 blocks | median 361 ns, p99 1,234 ns |
| Fit random lookup, single op, n=262,144 | median 200 ns, p99 400 ns |
| Fit lookup touching every row and node, mean per block of 256 | median 236 ns, p99 862 ns |
| Owned decode of one fit to `QuoteSurfaceSlice`, n=4,096 | median 1.9 us, p99 9.1 us |

Notes on the table:

- With n=9, "max" is the p99.
- Single-op times are quantised to the 100 ns `steady_clock` tick on Windows.
- An ExpiryRecord lookup is symbol, then id, then binary search, then the record.
- A true cold re-open after dropping the page cache was not feasible, because
  emptying the standby list needs administrator rights. The unbuffered read
  is the proxy.

The fit (101.4 s wall at 3 workers) and the `ChainRecords` build (17.5 s CPU
summed) are the payload's cost, not the archive's.

The CLI ran on the bench archive:

- `verify` reports `OK (paranoid: ...)`, exit 0.
- `inspect` lists 18 sections and grades `A=196 B=678 C=89 refused=175`.
- `dump --underlier META` prints 69 lines.

## Defect found by the bench (fixed in `73461064`)

The first bench run refused 15 of 191 surfaces with `invalid-argument
section=36` (MovedNodes). The writer had assumed that `QuoteSliceFit::moved_nodes`
is either empty or has one entry per price node. That is not always true:
`strip()` in `quote_surface.cpp` resets `fit.prices` of a slice refused after a
repair but keeps `moved_nodes`, and `quote_slice.cpp` bound-checks the index
before it reads the array.

The archive now stores `moved_nodes` exactly as the fit holds it; each value
must still be 0 to 3. A new test round-trips a stripped repaired slice; with the
old check restored, it fails at `add_surface`.

## Gates

All gates ran on `vol-rel` with `-Jobs 2` unless noted.

- `atx-vol-fit-archive-tests`: 43 of 43 tests in 7 suites. These cover:
  - framing refusals on crafted corrupt, truncated and foreign files (bad
    magic, endian, major, header, footer, section table, bounds, alignment,
    stride, duplicates, unknown types, checksums, padding, index order);
  - record-pass refusals;
  - bit-exact round trips of every record type and every curve kind;
  - `copy_from` byte identity;
  - zero allocation after open, checked with a global allocation counter;
  - series append, duplicates, `SegmentMismatch`, compaction and `Busy`;
  - the CLI on good, corrupt, truncated and missing files and on a series
    directory.
- ctest `^atx-vol-fit-archive$`: 1 of 1 passed.
- PCH-off (`vol-hygiene`, `ATX_USE_PCH=OFF`): every new TU passed with exit 0
  and 0 diagnostics. That is the 3 src files, the 4 test files and the 2 tools,
  plus a re-check of the 3 TUs changed after the first pass. Each new header is
  the first include of its own src TU, and `fit_archive_format.hpp` brings in
  its own includes.
- Storage slice (`atx-vol-storage-slice`, ctest `^storage/`): 527 of 530 passed;
  3 failed and are not explained by this lane.
  - `SurfaceDbPopulate.PropagatesStoredSurfacePolicyAndPersistsServedProvenance`
    is a ledger-recorded pre-existing failure.
  - `SurfaceDbPopulate.SymbolConfigOverlayReachesFit` and
    `ChainExport.PinnedSplineVolIsTheFamilyActuallyServed` also failed.
  - Why the lane is not the cause: those tests and the code they reach are
    unchanged, `atx-vol` is a static library, and nothing in the slice
    references an ATXVFA symbol.
  - A probe on the frozen base could not run, because the permission system
    denied checking out the base in the lane tree. The integrator should
    confirm these two failures on `24e01644`.
- Parity: no default path changed. Outside the new files, the diff from
  `24e01644` is 14 added lines in `atx-vol/CMakeLists.txt` and 24 added lines in
  `atx-vol/tests/CMakeLists.txt`. No fitting, pricing or tick-path source or
  header changed, and `atx-vol-quote-bench` and its sources are untouched. So
  dev200 default `c89d0739` (and graded-v1 `1348f63d`, R104 `6b129f64`, dev200b
  `4b273b08`) cannot move: the module is on no existing path.

## Deviations

- Bench target name: `atx-vol-fit-archive-bench`. The name
  `atx-vol-surface-archive-bench` already belongs to the ATXVSA2 bench in
  `bench/CMakeLists.txt:408`. The clash shows only in configures with
  `ATX_BUILD_BENCH`, such as `vol-hygiene`.
- Commit trailers say Claude Opus 5.5, not the brief's Fable 5.1. The harness
  attribution names the model that did the work; the doc commit `6f9e7a91` has
  the brief's trailer.
- The `ExpiryRecord` adapter stores the object representation of a non-POD
  library type. It is guarded by pinned offsets, a one-time default-object
  self-check (`LayoutMismatch`) and a stride check. It is the only
  layout-coupled record (spec section 7).

## Open

- Integrator: confirm the two storage-slice failures above on the frozen base.
- Not in v1: compression, network transport, `gc` of segments that compaction
  left unreferenced, and any default serve path that reads the archive.
