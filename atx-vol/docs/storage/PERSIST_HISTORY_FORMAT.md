# PersistFile history kinds — SurfaceTimeline (`.atxstl`) and RiskRun (`.atxrr`)

Status: v1, wave-3 lane P2 (2026-10-24). Design of record: `wave3-persist-design.md` D12 / D13
(sprint `.superpowers/sdd/2026-10-24-risk-engine/`). Code: `include/atx/vol/api/persist/
surface_timeline.hpp`, `risk_run_archive.hpp`, `src/persist/surface_timeline.cpp`,
`risk_run_archive.cpp`, `risk_run_format.hpp`. Tool: `atx-vol-history-store`
(`docs/persist-history.md`).

Both kinds are **PersistFile (ATXVPF) images**: the framing (256 B header, 64 B section entries,
64-byte aligned payloads, 64 B footer, file padded to 4096, CRC-32C everywhere, verify levels
Framing / Lazy / Full / Paranoid, the manifest pattern) is P1's and is specified in
`docs/storage/PERSIST_FORMAT.md`. This document specifies only the section CONTENT of the two
kinds. Little-endian only; every struct below is trivially copyable, standard layout, and proved
free of implicit padding by a field list that tiles its `sizeof` (`static_assert`); the same field
lists are folded into the kind's schema fingerprint, so a reader accepts exactly one layout and a
drift is `SchemaMismatch` (no backward compatibility, persist D5).

| Kind | `PersistKind` | Salt | Schema fingerprint (v1) |
|---|---|---|---|
| SurfaceTimeline | 3 | `ATXVST01` | `0x8e3c8cc7c33a7ea1` |
| RiskRun | 4 | `ATXVRR01` | `0xc881a1e8c5e87255` (pinned by `PersistRiskRun.RefusesLayoutDriftFingerprint`) |

## 1. SurfaceTimeline

One file holds every fitted surface of one PERIOD (a UTC calendar month), keyed by `(uid, ts)`.
Records are **verbatim ATXVSA2 surface records** (`docs/atxvsa2-format.md` §3): the timeline never
re-encodes a surface, so a view built here runs `PricedSurfaceView::create_over_record` over the
same bytes the partition's `SurfaceArchiveV2::map_symbol` maps — bit-exact by construction. The
record carries its own provenance (`prov_*` fields of `ArchiveV2SurfaceHeader`), so no per-entry
provenance section is needed (D12's contingency did not trigger).

### 1.1 Sections (all `type_major` 1, all required)

| Type | Name | dtype / stride | Rows |
|---|---|---|---|
| 1 | Meta | Pod / 64 | exactly 1 |
| 2 | UidDirectory | Pod / 24 | one per uid, strictly ascending uid |
| 3 | Strings | Bytes / 1 | tickers, concatenated, no terminators |
| 4 | Entries | Pod / 48 | sorted by (uid, ts_ns), ts strictly ascending within a uid |
| 5 | Records | Bytes / 1 | records on a 64-byte grid, zero padding between |

**Meta (64 B)**: `n_entries u64 @0`, `records_bytes u64 @8` (== Records length), `ts_min_ns i64
@16`, `ts_max_ns i64 @24` (== header ts range), `n_uids u32 @32`, `n_sources u32 @36`
(partitions folded in; informational), `flags u32 @40` (0), `reserved0 u32 @44` (0),
`source_fingerprint u64 @48` (== header `source_fingerprint`), `reserved1 u64 @56` (0).

**TimelineUid (24 B)**: `uid u32 @0`, `entry_count u32 @4` (>= 1), `entry_begin u64 @8` (rows tile
`[0, n_entries)` in uid order), `ticker_offset u32 @16`, `ticker_len u32 @20` (into Strings).

**TimelineEntry (48 B, public, `surface_timeline.hpp`)**: `uid u32 @0`, `n_slices u16 @4`, `flags
u16 @6` (the record's `kind_bits`: OR of `1 << VolCurveKind`), `ts_ns i64 @8`, `record_offset u64
@16` (into Records, multiple of 64), `record_size u32 @24`, `record_crc32c u32 @28` (the record's
ATXVSA2 payload CRC), `source_fingerprint u64 @32` (identity of the partition the record came
from: FNV-1a/64 + fmix64 over its `ArchiveContentIdentity`), `reserved u64 @40` (0).

### 1.2 Integrity model

The integrity unit is the RECORD. Open validates the framing (PersistFile), the schema, the Meta
consistency and the whole index in O(n_entries) without touching a record byte: uids strictly
ascending, entry ranges tiling the Entries section, each entry bound to its uid, ts strictly
ascending, records on the 64-byte grid, inside the section and in non-overlapping ascending order.
Under the default **Lazy** open the four index sections are CRC-verified on their first touch (at
open) and the Records section is never CRC'd as a whole; every `view(e)` verifies that record:
bounds and grid (`IndexBounds`), the ATXVSA2 payload CRC (CRC-32C over the record with its
`payload_crc32c` field read as zero) against `e.record_crc32c` (`SectionChecksum`, section 5,
detail = record offset), then magic / `record_size` / payload CRC copy / uid / n_slices binding
(`RecordInvalid`). `Full` / `Paranoid` verify the Records section at open instead. A view borrows
the timeline's backing (mapping or owned buffer) and dies with it.

### 1.3 Build

`encode_surface_timeline(sources, created_unix_ns)`: each source partition is read whole with one
positional read (`FILE_SHARE_DELETE`, never mapped, so a build never blocks a SurfaceDb republish
on Windows), opened with `SurfaceArchiveV2::open_borrowed` and fully verified (`validate_all`:
every record's CRC, framing and symbol binding). Every record becomes an entry; `ts_ns` is the
source's `ts_ns` when nonzero, else the record's own `PricingContext::now_ts_ns`. A repeated
`(uid, ts)` is `DuplicateKey` (detail = uid); a uid naming two tickers within the period is
`RecordInvalid` (section 3, detail = uid) — a uid-keyed history must never splice two
underliers. The output is deterministic (same sources -> same bytes; `created_unix_ns = 0` keeps
the header byte-identical).

### 1.4 Store (`SurfaceTimelineStore`)

`<root>/persist-manifest.atxvpm` + `<YYYY-MM>-g<seq12>.atxstl` (PersistDirectory, key = period).
`build_from_surface_db(db, keys, root, created)` resolves every key first (the key must be listed
by the db: `NotFound` otherwise; its instant is `SurfaceDb::session_ts`; its file is the db's
public layout `<db root>/partitions/<CANONICAL KEY>.atxvsa`), routes each partition to the UTC
calendar month of its instant, and republishes each touched period as a new generation that
MERGES the previous generation (fully verified) with the new partitions — a new partition at an
existing instant replaces that whole snapshot. Returns the entries published. One builder per
store root at a time (each period publish is atomic, the read-merge-publish of a period is not).
`open(root)` maps every listed period (Lazy, size bound to the manifest) and refuses periods
whose ts ranges interleave (`IndexUnsorted`). `collect(uid, from, to, views, refs, ts)` counts
first, reserves `views` once, re-points any existing ref that named an element of `views` if
the reserve moved the buffer, then appends in ascending ts across periods; on a failed view it
rolls all three vectors back (strong guarantee).

## 2. RiskRun

A run is a DIRECTORY: `persist-manifest.atxvpm` + `seg-<seq12>.atxrr` segments (key =
12-digit segment ordinal, so key order is segment order). Every segment is a self-contained
PersistFile: the book (identical bytes in every segment) plus a contiguous range of whole
steps and VaR results.

### 2.1 Steps

`RiskRunWriter::append_frame(f)` opens a step at `f.ts_ns` (frame columns stored unless
`RiskRunOptions::frames == false`). `append_explain(r)` attaches to the open step when that step
has no explain yet and `r.shifted_ts_ns` equals its instant (the `RiskStream` pattern: frame k,
then explain k-1 -> k); otherwise it opens an explain-only step. The rows must be the book's legs
in explain order (option legs in book order, then share legs; an option-only explain is allowed)
with matching ids / uids / kinds — anything else is `InvalidArgument`. `append_var(v)` records a
`HistoricalVarResult` against the last step (or none). A segment is sealed when a NEW step would
start while the staged bytes have reached `segment_bytes`, and on `finish()`; a writer destroyed
without `finish()` publishes nothing it had not sealed. `create` refuses a directory that already
holds a run (`AlreadyExists`), `segment_bytes == 0` and an unknown `SealDurability`.

### 2.1a Publication and durability

A seal publishes with PersistDirectory's protocol (`src/persist/risk_run_archive_publish.cpp`):
under `<dir>/persist.lock`, read the manifest, write the segment under a fresh canonical name
(`seg-<seq12>.atxrr`, seq = max + 1, skipping any crash orphan on disk), then replace the
manifest. Both files go through a unique temp beside the target and a rename, so a reader never
sees a partial file, and a crash between them leaves an unreferenced orphan, never a dangling
entry. The manifest image is byte-compatible with PersistDirectory's (manifest kind and
fingerprint, one Pod section of `PersistManifestEntry` rows sorted by key, header ts range = the
union of the rows'), built in this lane because `PersistDirectory::publish` always flushes and
can add or replace only one key.

| `SealDurability` | per file | survives |
|---|---|---|
| `Publish` (default for `segment_bytes` seals) | temp, write, rename | a process crash; a power loss can tear a file, then `open` refuses it by CRC |
| `Flush` (default for `finish()` and compaction) | temp, write, FlushFileBuffers, rename | a power loss, for that segment and the manifest |

A `Flush` seal does not reach back to earlier `Publish` segments: they are durable once the OS
has written them back. A run whose every segment must survive a power loss sets
`seal_durability = Flush`. The writer starts no thread.

### 2.2 Sections (all `type_major` 1, all required, in this file order)

| Type | Name | dtype / stride | Rows (per segment) |
|---|---|---|---|
| 1 | meta | Pod / 128 | 1 |
| 2 | book.legs | Pod / 48 | n_option_legs + n_share_legs (explain row order) |
| 3 | steps | Pod / 264 | n_steps |
| 4 | step.totals | Pod / 152 (`ExplainTotals`) | n_steps (zero row when no explain) |
| 5 | book.unique | Pod / 24 | n_unique (PREPARED order = frame rows) |
| 6 | book.uids | U32 | n_uids (`PreparedRiskBook::uids()` order) |
| 7 | book.leg_ids | U64 | n_option_legs + n_share_legs |
| 16..31 | frame.T .. frame.vendor_delta | F64 | n_frames x n_unique (RiskFrameColumn order) |
| 32 / 33 / 34 | frame.route / flags / status | U8 / U32 / U8 | n_frames x n_unique |
| 35 / 36 / 37 | frame.uid_spot / uid_flat_rate / uid_status | F64 / F64 / U8 | n_frames x n_uids |
| 38 / 39 | frame.share_mark / share_status | F64 / U8 | n_frames x n_share_legs |
| 48..67 | explain.pv_base .. explain.d_rate | F64 | n_explain_rows (RiskExplainColumn order, 20 columns) |
| 68 / 69 / 70 / 71 | explain.route_base / route_shifted / flags / status | U8 / U8 / U32 / U8 | n_explain_rows |
| 80 / 81 | buckets.by_uid / by_expiry | Pod / 176 | sum of the steps' bucket counts |
| 96 | var.index | Pod / 64 | n_var |
| 97 / 98 | var.frames / var.points | Pod / 184 / 184 | sums of the index counts |

Sections without content (frames off, buckets off, no VaR) are present and empty.

**meta (128 B)**: `book_fingerprint u64 @0` (FNV-1a/64 + fmix64 over the four book sections, each
length-prefixed), `first_step u64 @8`, `n_steps u64 @16`, `first_var u64 @24`, `n_var u64 @32`,
`n_option_legs u32 @40`, `n_share_legs u32 @44`, `n_unique u32 @48`, `n_uids u32 @52`, `flags u32
@56` (1 frames recorded, 2 buckets recorded), `segment_ordinal u32 @60`, `created_unix_ns i64 @64`,
`n_frames u64 @72`, `n_explain_rows u64 @80`, `reserved u64[5] @88` (0).

**book.legs (48 B)**: `uid u32 @0`, `kind u8 @4` (`ExplainLegKind`), `side u8 @5` (`Side`; 0 for a
share), `reserved0 u16 @6`, `slot u32 @8` (option: prepared row; share: uid slot), `reserved1 u32
@12`, `strike f64 @16`, `expiry_ns i64 @24`, `qty f64 @32`, `multiplier f64 @40` (as given; 1 for a
share). **book.unique (24 B)**: `uid u32 @0`, `side u8 @4`, `reserved0 u8 @5`, `reserved1 u16 @6`,
`strike f64 @8`, `expiry_ns i64 @16`.

**steps (264 B)**: `step u64 @0` (global), `ts_ns i64 @8`, `base_ts_ns i64 @16`, `dt_years f64 @24`,
`frame_index u64 @32` (ordinal of the step's frame in the segment, `~0` when none), `explain_row
u64 @40` (first explain row, `~0` when none), `bucket_uid_begin u64 @48`, `bucket_expiry_begin u64
@56`, `book_logical_id u64 @64` (the frame's process-local identity, round-tripped verbatim),
`explain_seconds f64 @72`, `flags u32 @80` (1 has frame, 2 has explain, 4 rate move without rho, 8
/ 16 / 32 the frame's RiskNeeds second_order / rho / charm), `explain_rows u32 @84`,
`n_pivot_groups u32 @88`, `n_pivot_edge_fallback u32 @92`, `n_bucket_uid u32 @96`,
`n_bucket_expiry u32 @100`, `excluded_count u32[6] @104`, `excluded_premium f64[6] @128`,
`counters FrameCounters @176` (88 B: six u64 counters, `status_count u32[6] @224`,
`evaluate_seconds f64 @248`, `family_build_seconds f64 @256`).

**ExplainTotals (152 B)**: 17 f64 `pv_base @0 .. shares_pnl @128` in struct order, `n_ok u32 @136`,
`n_excluded u32 @140`, `excluded_premium f64 @144`. **Buckets (176 B)**: `step u64 @0`, `key u64
@8`, `expiry_ns i64 @16`, `totals @24`. **var.index (64 B)**: `step u64 @0` (`~0` before any step),
`frame_begin @8`, `frame_count @16`, `point_begin @24`, `point_count @32`, `n_admitted @40` (u64),
`reference_pv f64 @48`, `reserved u64 @56`. **var.frames (184 B)**: `base_ts_ns @0`, `shifted_ts_ns
@8`, `totals @16`, `n_legs_resolved u32 @168`, `n_legs_unresolved u32 @172`, `admitted u8 @176`
(0 / 1), `reserved u8[7] @177`. **var.points (184 B)**: `confidence @0`, `value_at_risk @8`,
`expected_shortfall @16` (f64), `tail_scenarios u64 @24`, `tail_mean @32`.
(`HistoricalVarFrame` itself has implicit padding, so it is re-laid explicitly — persist D3.)

### 2.3 Reader validation

Per segment: the manifest entry must match the file (`expected_size`, header CRC, file CRC —
`SegmentMismatch`); schema; meta (known flags, zero reserved); book row counts, leg kinds (options
first), side / reserved bytes; column element counts against `n_frames` / `n_explain_rows`
(from the section table, no payload touched); steps: global numbering, known flags, frame ordinals
and explain rows contiguous, bucket ranges contiguous and owned by their step; VaR ranges
contiguous. Across segments: `segment_ordinal` = position, `first_step` / `first_var` continuous,
identical book bytes and counts; segment 0's book fingerprint recomputed. Column payloads stay lazy
(Lazy default): a column's CRC is verified on its first `frame()` / `explain()` touch, and a failing
column reads as empty. `risk_run::Reader` (private) adds owned `materialize_frame` /
`materialize_explain` / `var` that validate every enum byte (`RecordInvalid`).

### 2.4 Compaction and the cold tier

`compact_risk_run(dir, codec, level[, durability])` (the 3-argument form is `Flush`) reads the run
in Read mode at Full verification, concatenates
every column section's bytes in segment order (no value is re-encoded: bit-exact), re-bases the
step (`frame_index`, `explain_row`, bucket begins) and VaR (`frame_begin`, `point_begin`) indices,
writes ONE segment (ordinal 0) — with `PersistCodec::Zstd` every section is zstd-coded at `level`
in [1, 19] after a byte shuffle by element stride — and, under `<dir>/persist.lock`, refuses if the
manifest changed since it was read (`SegmentMismatch`: every entry's seq and file CRC are
compared), publishes the merged file under a fresh name and replaces the manifest with its
single entry (§2.1a; `PersistDirectory::replace_all` was deferred, so this is the compaction
contract). The superseded segment files stay on disk
(they may be mapped; offline gc removes them). Compressed sections are decoded and CRC-verified
at open by the container, so nothing allocates after open on either tier.

## 3. Lifetime and threading

Archives / timelines / stores are move-only PIMPL; spans and views borrow the backing and die
with the object (moves keep them valid). All readers are immutable after open and safe to query
from any number of threads (the lazy CRC verdicts are the container's seq_cst atomics). Writers
are single-threaded; one writer per run directory, one builder per timeline store.
