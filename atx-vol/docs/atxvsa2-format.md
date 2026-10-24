# ATXVSA2 revision 1 — surface archive with an O(1) point read

Status: **authoritative layout spec** for the surface-archive format as of lane SER
(2026-10-23). The on-disk POD structs live in
`include/atx/vol/api/storage/surface_archive.hpp` (`ArchiveV2*`); the writer, the
whole-board reader and the point read in `src/storage/surface_archive.cpp`; the
positional-read seam in `src/storage/positional_file.{hpp,cpp}`; the zero-copy view in
`include/atx/vol/api/backtest/priced_surface_view.hpp`. Every size and offset below is
pinned by a `static_assert` in the header, so this table and the code cannot drift
silently.

## 0. Naming and identity

The format family is **ATXVSA2** (the `2` in `SurfaceArchiveV2` /
`write_surface_archive_v2`). This document specifies **revision 1**, identified on disk
by the file magic `ATXVSA21`, `major == kArchiveV2Major == 5`, `minor == 0`, and the
schema hash of §7. Two older containers exist and are refused with a `ParseError`
that names the magic found:

| Magic | Name | Status |
| --- | --- | --- |
| `ATXVSA03` | ATXVSA03 (major 3) | refused; its declarations and the `run_archive.cpp` recogniser are deleted |
| `ATXVSA20` | ATXVSA2 revision 0 (major 4) | refused; no point read, wyhash lookup key, zero-marker provenance |
| `ATXVSA21` | ATXVSA2 revision 1 (major 5) | **this spec** |

There is no dual read and no migrator: no `.atxvsa` file exists outside rebuildable
caches (production rebuilds from the OPRA hives), so the break is free.

A surface **record** keeps its own magic `ATXVSR20`. The record magic tags "this extent
is a surface record"; the record LAYOUT is versioned by the file (magic + major +
schema hash). A record is only reachable through a validated file header, and the
zero-copy view (`priced_surface_view.cpp`, outside the storage module) carries its own
copy of the record magic, so the record magic did not move with the layout.

## 1. Why revision 1

Revision 0 served a single underlying only after reading the WHOLE partition:
`SurfaceDb::map_surface` / `load_surface` streamed the file into a heap buffer
(`open_file`), copied the entire lookup table and directory, CRC'd the whole metadata
span and cross-checked every directory entry against the lookup — O(file) bytes and
O(#surfaces) work before one hash probe. Revision 1 adds a **point read** that touches
only the header, the probed lookup slots and the record's own extent, and makes every
byte it touches self-verifying (header CRC, per-slot CRC, record CRC + symbol binding).
Whole-board readers keep an eager, fully cross-checked open (§5).

## 2. File shape

```
offset 0             ArchiveV2Header            128 B
128                  ArchiveV2LookupSlot[S]      64 B each, S = lookup_slot_count (pow2)
128 + 64*S           ArchiveV2DirEntry[N]        72 B each, N = surface_count, sorted by symbol
data_offset          SurfaceRecord 0..N-1        each at a multiple of the surface alignment
EOF                  zero padding to 4096
```

Metadata placement is **canonical**: `lookup_offset == 128`, `directory_offset ==
lookup_offset + 64*S`, `data_offset >= directory_offset + 72*N` and a multiple of 8.
A reader refuses any other placement, so no byte between the header and the data
section escapes a checksum. All integers are little-endian, all floats IEEE-754
binary64, every field naturally aligned (FlatBuffers rule), every internal reference a
byte offset (Cap'n Proto rule: mapping needs no relocation).

### 2.1 `ArchiveV2Header` (128 B, offset 0)

| Off | Size | Field | Meaning |
| --- | --- | --- | --- |
| 0 | 8 | `magic` | `ATXVSA21` |
| 8 | 8 | `file_size` | total bytes; must equal the real size |
| 16 | 8 | `created_ts_ns` | content identity, NOT a time (§8) |
| 24 | 8 | `schema_hash` | §7 |
| 32 | 8 | `lookup_offset` | 128 |
| 40 | 8 | `directory_offset` | `128 + 64*S` |
| 48 | 8 | `data_offset` | first record; multiple of 8 |
| 56 | 4 | `surface_count` | N, `<= kArchiveV2MaxSurfaces` (2^24) |
| 60 | 4 | `lookup_slot_count` | S: power of two, `>= 8`, `> N` (at least one empty slot) |
| 64 | 4 | `lookup_slot_size` | 64 |
| 68 | 4 | `dir_entry_size` | 72 |
| 72 | 4 | `surface_header_size` | 224 |
| 76 | 4 | `header_crc32c` | CRC-32C of these 128 bytes with this field zero |
| 80 | 4 | `metadata_crc32c` | CRC-32C of lookup ‖ directory |
| 84 | 4 | `flags` | writer `ArchiveV2WriteOpts::flags`, opaque |
| 88 | 2 | `major` | 5 |
| 90 | 2 | `minor` | 0 |
| 92 | 2 | `header_size` | 128 |
| 94 | 2 | `endian` | 1 (little) |
| 96 | 2 | `pointer_bits` | 64 |
| 98 | 30 | `reserved` | zero; pads the header to the 64-B lookup alignment so no uncovered gap precedes the lookup |

### 2.2 `ArchiveV2LookupSlot` (64 B) — open-addressed hash table

Linear probing from `archive_v2_symbol_hash(symbol) & (S - 1)`. Every slot, empty or
occupied, carries its own CRC, so a point read verifies exactly the slots it reads.

| Off | Size | Field | Meaning |
| --- | --- | --- | --- |
| 0 | 8 | `symbol_hash` | §6 |
| 8 | 8 | `surface_offset` | absolute record offset |
| 16 | 4 | `surface_size` | record extent in bytes (records are `< 4 GiB`; the writer refuses larger) |
| 20 | 4 | `payload_crc32c` | copy of the record's `payload_crc32c` |
| 24 | 4 | `slot_crc32c` | CRC-32C of these 64 bytes with this field zero |
| 28 | 1 | `symbol_len` | 1..32 (occupied), 0 (empty) |
| 29 | 1 | `flags` | 0 empty, 1 occupied |
| 30 | 2 | `reserved` | zero |
| 32 | 32 | `symbol` | canonical bytes, zero padded |

An empty slot is all-zero except `slot_crc32c` (= the CRC of 64 zero bytes).

### 2.3 `ArchiveV2DirEntry` (72 B) — the ordered jump table

Sorted strictly ascending by canonical symbol (memcmp over the shared prefix, shorter
first). Whole-board readers (`map_all`, `entries()`) iterate it; nothing on the point
read touches it.

| Off | Size | Field |
| --- | --- | --- |
| 0 | 8 | `surface_offset` |
| 8 | 8 | `surface_size` |
| 16 | 8 | `symbol_hash` |
| 24 | 4 | `uid` |
| 28 | 4 | `payload_crc32c` (copy; makes `metadata_crc32c` sensitive to any payload rewrite) |
| 32 | 2 | `n_slices` |
| 34 | 2 | `kind_bits` (OR of `1 << VolCurveKind`) |
| 36 | 2 | `symbol_len` |
| 38 | 2 | `flags` (zero) |
| 40 | 32 | `symbol` |

## 3. Surface record

A record is self-contained and self-identifying: `[surface_offset, surface_offset +
surface_size)` holds everything needed to price the surface, its provenance, the
canonical symbol it belongs to and its own CRC. Offsets inside it are record-relative.

### 3.1 `ArchiveV2SurfaceHeader` (224 B)

| Off | Size | Field | Meaning |
| --- | --- | --- | --- |
| 0 | 8 | `magic` | `ATXVSR20` |
| 8 | 8 | `record_size` | equals the extent |
| 16 | 8 | `S` | spot |
| 24 | 8 | `r` | scalar rate |
| 32 | 8 | `now_ts_ns` | valuation instant |
| 40 | 8 | `al_tol` | `AlOpts::tol` |
| 48 | 8 | `prov_validation_id` | |
| 56 | 8 | `prov_source_generation` | |
| 64 | 8 | `prov_served_generation` | |
| 72 | 80 | `col_*_off` x10 | kind, T, forward, q_eff, df, borrow, n_used, n_dropped, node_count, payload_off |
| 152 | 4 | `uid` | |
| 156 | 4 | `n_slices` | 1..65535 |
| 160 | 4 | `prov_validation_failures` | `ValidationFailure` bits 0..12 |
| 164 | 4 | `payload_crc32c` | CRC-32C of the whole record with this field zero |
| 168 | 2 | `kind_bits` | must equal the OR over the kind column |
| 170 | 2 | `symbol_len` | 1..32 |
| 172 | 8 | `al_n_collocation`, `al_n_quadrature`, `al_max_newton_iter`, `al_n_quad_price` | u16 each; `n_quad_price == 0` = tied |
| 180 | 1 | `method` | `AmericanMethod` (0 AndersenLake, 1 Baw); anything else is refused |
| 181 | 1 | `prov_status` | 1 Attested, 2 Unattested; 0 or anything else is refused |
| 182 | 3 | `prov_purpose`, `prov_quality_mode`, `prov_state` | enum wire values |
| 185 | 7 | `reserved` | zero; aligns `symbol` to a 64-B boundary |
| 192 | 32 | `symbol` | canonical symbol, zero padded — the slot -> record binding |

`prov_status` replaces revision 0's zero-marker convention. `Attested` records carry a
validated provenance (purpose <= 1, quality <= 2, state <= 3, failure bits <= 12, and a
`Healthy` state carries no failure bit). `Unattested` records were written from a
`SurfaceArchiveItem` with `provenance == nullopt`; every provenance field is zero and
the record decodes to `legacy_surface_provenance()` (a degraded, never-admitted market
mark with `legacy_format = true`, the flag the backtest risk gate reads). An all-zero
provenance block is no longer a state; it is `ParseError`.

### 3.2 Columns and payloads — canonical geometry

The reader recomputes the writer's geometry from `n = n_slices` and requires equality,
which rules out every aliasing class (a column overlapping the header, two columns
overlapping, a payload overlapping a column) in O(n):

```
col_kind_off      = 224
col_T_off         = align8(224 + n)
col_forward_off   = col_T_off + 8n        col_qeff_off     = col_forward_off + 8n
col_df_off        = col_qeff_off + 8n     col_borrow_off   = col_df_off + 8n
col_nused_off     = col_borrow_off + 8n   col_ndropped_off = col_nused_off + 8n
col_nodecount_off = col_ndropped_off + 8n
col_payload_off_off = align8(col_nodecount_off + 4n)
payload_off[0]    = align8(col_payload_off_off + 8n)
payload_off[i]    = align8(payload_off[i-1] + payload_size(kind[i-1], node_count[i-1]))
record_size       = payload_off[n-1] + payload_size(kind[n-1], node_count[n-1])
```

Per-slice payload encoding (unchanged from revision 0, so record encoding is bit-exact
and the view/reconstruct parity tests hold verbatim):

| kind | `node_count` | payload bytes (8-B aligned start) |
| --- | --- | --- |
| `ConvexDense` (0) | >= 1 | `rmse_price` f64, `n_obs` u64, `n_active` u64, `u[n]` f64, `C[n]` f64 |
| `Essvi` (1) | 0 | `EssviParams` (248 B, padding canonical zero) |
| `Svi` (2) | 0 | `SviParams` (72 B, padding canonical zero) |
| `LinearVariance` (3) | >= 1 | `k[n]` f64, `w[n]` f64 |
| `C8` (4) | 0 | `C8Params` (176 B, padding canonical zero) |
| `SplineVol` (5) | >= 1 | `atm_vol, z_lo, z_hi` f64x3, `n` u32 (== node_count), pad u32 (0), `z[n]`, `mult[n]`, **`mult_cap` f64, `w_offset` f64**, `n_butterfly_viol` u32 |

`mult_cap` and `w_offset` are live terms of `SplineVolCurve::w()`; dropping them
misprices clamped / calendar-lifted slices. `df[i]` must be finite and `> 0`.

## 4. Point read (`point_read_archive_surface`)

Given a partition path and a symbol, serve that one surface:

1. Canonicalize the symbol (ASCII upper-case). Empty or longer than 32 bytes:
   `InvalidArgument` (the writer refuses such symbols, so no truncation alias exists).
2. Open the file for **positional reads** (`detail::PositionalFile`: Win32
   `ReadFile` with an `OVERLAPPED` offset, POSIX `pread`), shared
   `READ | DELETE` on Windows. Absent file: `NotFound`; any other open failure:
   `IoError`. No mapping is ever created.
3. Read 128 B at offset 0. Validate magic (an older magic is named in the error),
   major, endian, pointer width, struct sizes, schema hash, header CRC, `file_size ==`
   real size (truncation and appended bytes both refuse), canonical metadata
   placement, `S` pow2 `>= 8` and `> N`, `N <= 2^24`, `reserved == 0`.
4. `h = archive_v2_symbol_hash(canon)`, `i = h & (S - 1)`. Read slots in runs of up to
   8 (512 B, never wrapping inside one read). For each slot, verify `slot_crc32c`
   (`ParseError` on mismatch), then: empty -> `NotFound`; `flags > 1` or a non-zero
   empty slot -> `ParseError`; hash and symbol bytes equal -> hit. The probe is bounded
   by `S` steps; because `S > N` an absent symbol always meets an empty slot.
5. Check the slot extent: `surface_offset >= data_offset`, a multiple of 8,
   `surface_size >= 224`, `surface_size <= file_size - surface_offset` (subtraction
   form). Read exactly `[surface_offset, surface_offset + surface_size)` into an owned,
   16-B aligned buffer. Close the file.
6. Verify the record: magic, `record_size == surface_size`, symbol binding
   (`symbol_len`/`symbol` equal the canonical query), `payload_crc32c` recomputed over
   the record equals both the record's field and the slot's copy, then the canonical
   geometry and enum checks of §3. Any failure: `ParseError` — never a wrong surface.
7. Build the `PricedSurfaceView` over the owned buffer (zero copy from there) or
   reconstruct an owned `PricedSurface`, plus provenance.

I/O budget (the reader's own accounting, `ArchivePointReadStats`): 3 positional reads in
the common case — 128 B header, one slot run (<= 512 B), the record (~0.3–2 KB for a
parametric surface) — independent of the partition's size. The file handle lives only
for the call, so a point read never blocks the atomic republish of the partition.

## 5. Whole-board open (`SurfaceArchiveV2::open*`)

`open` / `open_copied` / `open_file` / `open_mapped` / `open_borrowed` keep the eager
metadata parse: header checks of step 3, the metadata CRC, every slot's CRC and
emptiness, every slot's and entry's `symbol_hash` recomputed from its symbol, strictly
ascending directory symbols, occupied-slot count `== N`, and the lookup <-> directory
bijection (offset, size, CRC copy and hash agree). `map_symbol`, `map_entry`, `map_all*`,
`reconstruct_*` and `provenance` then check, per record they touch, the record
framing, the symbol binding against the slot / entry, `n_slices` / `kind_bits` / `uid`
/ CRC-copy agreement with the entry, and the §3 geometry and enums. They do NOT
recompute the record CRC (lazy, `validate_symbol` / `validate_all`): whole-board loads
stay O(bytes touched), not O(bytes CRC'd).

`open_copied` (and `open_file`, kept as the same operation for test fixtures) reads the
whole file with ONE positional read into an owned buffer — no `ifstream`, no mapping —
so it never blocks a republish and keeps `NotFound` typed. `open_mapped` maps the file
and keeps the mapping for the archive's lifetime (on Windows that blocks a republish
of that file while the archive lives; use it only for read-only corpora).

## 6. Symbol hash

`archive_v2_symbol_hash(canonical bytes)` = FNV-1a/64 (offset basis
`0xcbf29ce484222325`, prime `0x100000001b3`) over the bytes, then the MurmurHash3
`fmix64` finalizer so the low bits the table masks are well mixed. It is defined by
this spec, fixed arithmetic, and pinned by a known-answer test. Revision 0 persisted
`atx::core::hash_bytes` (wyhash through ankerl's private `detail` namespace), whose own
contract says "NOT stable across process restarts or platforms": a dependency bump
would have turned every lookup into `NotFound` with no schema-hash change.

## 7. Integrity model

| Region | Covered by | Point read | Whole-board open | A bit flip there |
| --- | --- | --- | --- | --- |
| header | `header_crc32c` | verified | verified | `ParseError` |
| one lookup slot | its `slot_crc32c` and `metadata_crc32c` | verified (each slot read) | both verified | `ParseError` |
| directory | `metadata_crc32c` | not read | verified | `ParseError` at open |
| record | its `payload_crc32c` (+ copies in its slot and entry) | verified | lazy (`validate_*`) | point read: `ParseError`; whole-board: served unless validated (by design) |
| inter-record / tail padding | nothing | not read | not read | harmless, undetected (never read) |

`schema_hash` = `0x9e3779b97f4a7c15 ^ salt`, then `^= sizeof(T) * 0x100000001b3` for
`ArchiveV2Header`, `ArchiveV2LookupSlot`, `ArchiveV2DirEntry`, `ArchiveV2SurfaceHeader`,
`EssviParams`, `SviParams`, `C8Params`; salt `0xA7C3'5F04'2E1F'0201`. There is no
prior-salt accept-list: a reader accepts exactly one schema hash.

Records are host byte order; the header stamps `endian = 1`, `pointer_bits = 64` and
the reader refuses a mismatch (little-endian LP64 only).

## 8. Content identity (`created_ts_ns`)

`ArchiveContentIdentity = {file_size, created_ts_ns, header_crc32c, metadata_crc32c}`
keys the backtest `SnapshotCache` / `SnapshotPool` staleness checks. With the writer's
default `created_ts_ns = 0`, the field is filled from `(CRC32C(bytes[128, EOF)) << 32)
^ file_size` (never 0): two identical builds are byte-identical, two different builds
differ. It is a staleness signal only — CRC-32C is linear and forgeable, never tamper
evidence — and it may be negative as an `int64`; never order archives by it. When a
caller pins `created_ts_ns`, the identity of a same-size rewrite rests on
`metadata_crc32c` alone (32 bits, which the CRC copies in slots and entries make
sensitive to every record byte).

## 9. Durable publish

Unchanged: `write_surface_archive_v2_file` writes a unique same-directory temp, fsyncs
it (`FlushFileBuffers` / `fsync`), and renames it over the destination with bounded
retry (8 attempts, ~635 ms) while a reader holds the destination open without
`FILE_SHARE_DELETE`; on final failure the temp is preserved. The point read and the
whole-file read open with `FILE_SHARE_DELETE` and close before returning, so they never
cause that retry. A long-lived `open_mapped` archive still can (documented above).
External in-place truncation of a mapped file remains the standard mmap hazard
(`EXCEPTION_IN_PAGE_ERROR` / `SIGBUS`); positional reads turn it into a short read
(`IoError`).

## 10. SurfaceDb integration (ATXVDB02)

`SurfaceDb::map_surface` / `load_surface` serve every call through the point read; the
revision-0 LRU partition cache (16 whole-file buffers keyed by content identity) is
deleted. A cache hit used to pay a file open plus a 256-B identity read anyway, so a
point read costs the same order as the old hit while a miss drops from O(file) to
O(record). Each call reads one consistent file version through one handle, which
removes the cache's identity-outside-the-lock window and its "unreadable identity keeps
the stale entry" rule. `open_partition` / `open_partition_file` (whole-board callers)
use `open_copied`; `session_ts` uses positional reads of the header, the first
directory entry and that record's header.

The manifest is **ATXVDB02** (`major 2`): a 128-B header (24-B zero tail), 264-B
`DbSymbolRecord`s whose surface policy and provenance are named fields (no marker, no
all-zero legacy policy: `outputs` must be 1..3, and the manifest provenance now keeps
`source_generation`), and 64-B `DbPartitionRecord`s with a named `config_fingerprint`.
Placement is canonical (symbols at 128, partitions at the next 64-B boundary, `file_size
==` end of partitions), so every manifest byte is covered by a CRC. `ATXVDB01` is
refused by name.

## 11. What revision 1 removed, and why

| Removed | Why |
| --- | --- |
| ATXVSA03 record declarations (`ArchiveHeader`, `ArchiveIndexSlot`, `ArchiveDirEntry`, `SurfaceBlobHeader`, `ArchivePricingRecord`, `ArchiveSliceHeader`, `ArchiveSurfaceProvenanceRecord`) and their `run_archive.cpp` recogniser | kept only to fold an ATXVSA03 header into a run identity; nothing can read such a file |
| prior-salt accept-list (`schema_hash_v2_prev`, salt 0101) | backward compatibility waived; one reader accepts one schema |
| zero-marker provenance (`prov_marker == 0` -> legacy) | replaced by the explicit `prov_status` byte |
| `writer_version_hash` | always written 0, never read |
| SurfacePolicy / provenance / fingerprint in manifest `reserved` bytes | named fields |
| revision-0 reserved tails (header 148 B, record 66 B, entry 8 B, manifest 88 B, partition 64 B) | every remaining reserved byte is an alignment pad that must be zero |
| `SurfaceDb` partition cache, `partition_cache_stats`, `partition_cache_capacity` | replaced by the point read (§10) |

## 12. API

```cpp
// Writer (unchanged signatures).
Result<std::vector<std::byte>> write_surface_archive_v2(span<const SurfaceArchiveItem>, opts);
Status write_surface_archive_v2_file(string_view path, span<const SurfaceArchiveItem>, opts);

// Point read: one surface from a partition file, O(record) I/O, no mapping kept.
Result<PointReadSurface> point_read_archive_surface(string_view path, string_view symbol);
Result<PointReadOwnedSurface> point_read_archive_reconstruct(string_view path, string_view symbol);

// Whole-board.
SurfaceArchiveV2::open(vector<byte>) / open_copied(path) / open_file(path)
                / open_mapped(path) / open_borrowed(span, owner);
find, provenance, map_symbol, map_entry, map_all, map_all_with_provenance,
reconstruct_symbol, reconstruct_entry, reconstruct_all, reconstruct_all_with_provenance,
validate_symbol, validate_all, entries(), identity().
```

`PointReadSurface` owns the record bytes its view borrows (move-safe: the bytes live on
the heap); `PricedSurfaceView`s from a `SurfaceArchiveV2` borrow that archive's bytes and
must not outlive it.

## 13. Primary sources

- FlatBuffers internals — natural alignment, offsets + adjacency, zero-copy:
  https://flatbuffers.dev/internals/
- Cap'n Proto encoding — contiguous segments, relative offsets:
  https://capnproto.org/encoding.html
- Apache Arrow columnar format — contiguous typed buffers, 8/64-B alignment, mmap-first:
  https://arrow.apache.org/docs/format/Columnar.html
- FNV-1a: http://www.isthe.com/chongo/tech/comp/fnv/ ; MurmurHash3 `fmix64`:
  https://github.com/aappleby/smhasher/blob/master/src/MurmurHash3.cpp
- CRC-32C (Castagnoli), RFC 3720 appendix B.4 (known answer `"123456789"` -> `0xE3069283`).
- Win32 `ReadFile` with `OVERLAPPED` offsets on a synchronous handle, and
  `FILE_SHARE_DELETE` semantics: https://learn.microsoft.com/windows/win32/api/fileapi/nf-fileapi-readfile
