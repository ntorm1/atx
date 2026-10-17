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
  (Done by the review: all three fail identically at `24e01644`, so they predate
  the lane.)
- Not in v1: compression, network transport, `gc` of segments that compaction
  left unreferenced, and any default serve path that reads the archive.

## Fix round 1 (resume 6)

The exact-SHA review of `431b724f` returned BLOCK: 1 major, 3 minor, 4 nit
(`C:/atx-cache/quote-benchmark/surface-archive-431b724f-review-v1/`). Resume 5
checkpointed a partial fix as `c0545fe9`; resume 6 finished it in `3a1f663b`
(code, tests, format doc) and `45955ea2` (the CLI test keeps one crafted series
per name on disk). The archive stays opt-in: outside the archive files the diff
from `24e01644` is still 14 + 24 added CMake lines, and dev200 default
`c89d0739` reproduces (below).

| Finding | Fix | Covering tests |
| --- | --- | --- |
| MAJOR-1: u32 wrap in `expected_curve_words` | Word count in u64 (`c0545fe9`); `decode_curve` now runs open's whole `curve_ok` shape check, so an edited record copy is refused before any payload read; manifest record counts are ORed, not summed (a forged u64 sum could wrap to 0 and hide a segment) | `FitArchiveRecordRefusal.CurveNodeCountThatWrapsU32IsRefusedForEveryCurveKind` (6 kinds x node_count {n + 2^31, 2^31, 2^32 - 1} x Framing / Full / Paranoid, all `RecordInvalid` at `FitRecords`); `...DecodersRefuseARecordCopyWhoseNodeCountWraps`; `FitArchiveSeries.RecordCountsWhoseSumWrapsStillRouteLookups`; `FitArchiveCli.VerifyRefusesACurveNodeCountThatWrapsU32` |
| MINOR-1: segment names escape the series folder | Name must equal the writer's `seg-<sequence>.atxvfa` (`fit_archive_segment_name_ok`), checked at (a) the manifest record pass, (b) `open_segment`, (c) before compaction's delete (`c0545fe9`) | `FitArchiveSeries.SegmentNameRuleAcceptsOnlyTheWritersOwnName`, `...ForeignSegmentNameIsRefusedWhenTheManifestOpens` (a), `...SeriesOpenNeverFollowsAForeignSegmentName` (b), `...CompactionNeverOpensOrDeletesAFileOutsideTheSeries` (sentinel; c); `FitArchiveCli.VerifyRefusesASeriesWhoseManifestNamesAForeignFile`. Names: `..\`, `../`, `C:\` absolute, `C:` drive-relative, two embedded-NUL forms, `sub\`, another sequence, unpadded |
| MINOR-2: bench parity was payload parity | The bench reopens the published file (Paranoid), decodes every surface and repeats the reference row check on decoded values; it also compares every archived `ExpiryRecord` (1820 value bytes) with the built one; any difference exits 1 | bench runs below |
| MINOR-3: raw handle and buffer in the bench | `FileHandle` / `PageBuffer` RAII owners, every Win32 return checked, short read refused | bench runs below; PCH-off check |
| NIT-1: archive bytes depend on wall time | Done at `c0545fe9` (`reserved_timing` slot, optional `FitTimings` section 40 via `record_timings(true)`); now documented (ARCHIVE_FORMAT.md sections 4, 5.1, 9, 11) and the bench takes `--created-unix-ns` and `--timings on/off` | `FitArchiveRecords.ArchivesOfTheSameInputsAreByteIdenticalWhateverTheTimings`; two-run sha256 below |
| NIT-4: pool-struct `sizeof` pins | Already present (`fit_archive_records.hpp:47-52`) | build |

Not done (nits, deferred): NIT-2 `intern_reason` keeps every distinct foreign
reason string; NIT-3 the temp file is left behind when the final rename of
`publish_fit_archive` fails.

Size-arithmetic audit (every size or offset product and sum over file data in
`fit_archive_*`): `fit_archive.cpp` builds the table end as
`256 + u64(section_count) * 64` after `section_count <= 256`; checks section
bounds in subtraction form (`length <= footer_offset - offset`) and the count
as `count == length / stride`, so `offset + length` cannot wrap; checks the
stride-upgrade size as `count <= SIZE_MAX / stride` before `count * stride`
(allocation) and copies element `k` at `k * stride < length`;
`fit_archive_records.cpp` computes curve word counts in u64
(`fixed + 2 * u64(n)`, then `13 + n` / `13 + 2n` / `6 + n` in `size_t` only
after the payload length equals that count), checks every pool reference as
`first <= pool && count <= pool - first`, sums surface and ladder indices in
`size_t` after that check, addresses the ExpiryRecord bytes at
`i * 1824 < section length`, and keeps writer pool totals below 2^32 before
narrowing; `fit_archive_series.cpp` ORs the manifest record counts and bounds
segment names below 40 bytes before indexing. The bench rounds a file size up to
a page only after `size <= SIZE_MAX - 4096`.

Revert evidence (each restored by the exact inverse Edit; `git hash-object`
equals `HEAD:` for both sources, `git status --porcelain` empty):
- Wall time back in the record, check (a) removed, counts summed: exactly
  `ArchivesOfTheSame...Timings`, `ForeignSegmentNameIsRefusedWhenTheManifestOpens`
  and `RecordCountsWhoseSumWrapsStillRouteLookups` fail (50 of 53 pass). The (b)
  and (c) tests still pass, so (b) catches what (a) misses.
- (a) and (b) removed: the series-open, compaction and CLI tests fail too; CLI
  `verify` prints `OK` for the `..\` series. The sentinel assertions still
  pass: (c) alone kept the outside file.
- (a), (b) and (c) removed: the compaction test reports `sentinel deleted` for
  both `..\` and `../`.
- 32-bit word count: the in-place revert was denied by the permission system,
  so the evidence is the reviewed binary. `atx-vol-surface-archive` built at
  `431b724f` verifies the three node-kind crafted files below as `OK`; the fixed
  build refuses all six.

Bench (`atx-vol-fit-archive-bench` at `45955ea2`, sha256 `e41d3123...`,
`ATX_VOL_FIT_WORKERS=3`, `--workers 3 --reps 9 --lookups 262144 --ladders on
--created-unix-ns 1792000000000000000 --timings off`). Run 1 went to
`surface-archive-fix1-20261017-bench-run1` (exit 0, 84 s, load 39% before and
9% after); run 2 went to `...-bench-run2` (exit 0, 110 s, load 87% before and
80% after):
- Round trip, decoded from the published file in both runs: fits 1,138/1,138;
  served rows 50,348, price exact 50,348, vol exact 50,348, grade 50,348;
  unserved in both 12,878; ExpiryRecords bit-identical 1,926/1,926. This equals
  the payload parity.
- Archive: 12,140,032 B, sha256
  `50cac1218af04e73ae1471c80dc103e59841f3bf2da02112ed72d078dac434c1` in both
  runs (and in a scratch smoke run of the `3a1f663b` binary). With the time
  pinned and timings off, the bytes are reproducible.
- Timings, run 1, unqualified (loaded host, other lanes building): finish 12.0
  ms median; publish with fsync 33.6 ms; mapped open Framing 8.7, Full 14.9,
  Paranoid 12.0 ms; Read+Full 22.7 ms; unbuffered read 10.0 ms then Full
  validate 2.7 ms; ExpiryRecord lookup + price single op median 500 ns, p99 1.3
  us; fit lookup single op median 200 ns, p99 500 ns; owned fit decode median
  3.8 us. Slower than the lane's first run on the open path (5.4 ms Full),
  consistent with the load; nothing here is a qualified number.

Crafted files (`C:/atx-cache/quote-benchmark/surface-archive-fix1-20261017-crafted/`,
`sha256.txt` 86 files): the review's 22 inputs copied unchanged, the 6
node-count files written by the overflow test and the 9 foreign-name series
written by the CLI test. `atx-vol-surface-archive verify` at `45955ea2`
(`cli-verify-45955ea2.txt`): every malformed input is refused with a typed
status (the review's 19 plus all 15 new ones, including review cases 16, 17 and
18 that `431b724f` accepted). The three valid controls (`00-valid`,
`16c-control`, `18-outside-segment`) open `OK`. `cli-verify-431b724f.txt`
holds the reviewed binary's results on the 15 new inputs.

Parity: `atx-vol-quote-bench --model quote-slice --policy auto --dividends
funding-pv` on dev200, built at `45955ea2`, exit 0, csv sha256
`c89d07393c90ad86860a6bcacd3d1a84ede85f4647d9cce136567e34763d5b7c` (MATCH);
`surface-archive-fix1-20261017-parity/`.

Gates at `45955ea2`: `atx-vol-fit-archive-tests` 53/53 (44 before the round);
ctest `^atx-vol-fit-archive$` 1/1; PCH-off (`vol-hygiene`) on the 3 sources, 4
test TUs and 2 tools, each exit 0 with 0 diagnostics; build of the 4 targets
with 0 warnings.
