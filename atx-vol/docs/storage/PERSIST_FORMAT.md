# ATXVPF: the persist container, its directory manifest, and the MarketState kind

Status: format major 1 (lane P1, wave-3 persistence, 2026-10-24). This file is the on-disk
specification. The structs that implement it are public and pinned by `static_assert`:

| What | Header |
| --- | --- |
| Header / section entry / footer, enums, statuses, fingerprint | `include/atx/vol/api/persist/persist_format.hpp` |
| Builder, reader (`PersistFile`), manifest entry, directory | `include/atx/vol/api/persist/persist_file.hpp` |
| MarketState kind API (section ids, archive, state store) | `include/atx/vol/api/persist/state_archive.hpp` |
| MarketState private records + schema table | `src/persist/state_archive_format.hpp` |

The other two kinds (SurfaceTimeline, RiskRun) live in `PERSIST_HISTORY_FORMAT.md` (lane P2);
they reuse this container unchanged. Tool usage, store layout and measured numbers: `../persist.md`.

There is no backward compatibility below major 1 and none is promised across schema changes: a
reader refuses a file it cannot read exactly (`SchemaMismatch`, `UnsupportedMajor`) and the
state store rebuilds it from the source. All integers are little-endian; a big-endian or
non-LP64 host does not compile (`static_assert` in `persist_file.cpp`).

## 1. Container layout

```
offset 0                 PersistHeader            256 B
offset 256               PersistSectionEntry[n]   64 B each (n <= 256)
offset 256 + 64 n        payloads, each starting on a 64-byte boundary, in table order,
                         zero bytes between them and after the last one
offset file_size - 64    PersistFooter            64 B
file_size % 4096 == 0
```

The payload area begins at `align_up(256 + 64 n, 64)` (= `256 + 64 n`). Sections never overlap
and appear in table order (`offset` non-decreasing). Every byte that is not header, table, a
section payload or the footer is zero. The 4096-byte file size lets a reader issue one
`FILE_FLAG_NO_BUFFERING` read of the whole file.

### 1.1 Header (256 B)

| Off | Size | Field | Rule |
| --- | --- | --- | --- |
| 0 | 8 | `magic` | `89 56 50 46 0D 0A 1A 0A` (`\x89VPF\r\n\x1A\n`) |
| 8 | 2 | `format_major` | 1 |
| 10 | 2 | `format_minor` | 0 |
| 12 | 2 | `header_size` | 256 |
| 14 | 2 | `section_entry_size` | 64 |
| 16 | 4 | `endian_tag` | `0x01020304` read as a native u32 |
| 20 | 4 | `footer_size` | 64 |
| 24 | 8 | `file_size` | multiple of 4096, nonzero, equals the byte count on disk |
| 32 | 8 | `section_table_offset` | 256 |
| 40 | 4 | `section_count` | <= 256 |
| 44 | 4 | `kind` | `PersistKind`: 1 Manifest, 2 MarketState, 3 SurfaceTimeline, 4 RiskRun |
| 48 | 8 | `schema_fingerprint` | section 3 |
| 56 | 8 | `source_fingerprint` | writer-defined (state store: section 6.2) |
| 64 | 8 | `created_unix_ns` | writer input (pinned in tests and licensed builds for byte identity) |
| 72 | 8 | `ts_min_ns` | <= `ts_max_ns` |
| 80 | 8 | `ts_max_ns` | |
| 88 | 8 | `writer_version` | `ATX_VOL_VERSION` of the writer |
| 96 | 32 | `writer_name` | NUL-terminated, NUL-padded, <= 31 bytes |
| 128 | 4 | `flags` | 0 |
| 132 | 4 | `section_table_crc32c` | CRC32C of bytes [256, 256 + 64 n) |
| 136 | 4 | `header_crc32c` | CRC32C of the 256 header bytes with this field zeroed |
| 140 | 4 | `reserved0` | 0 |
| 144 | 112 | `reserved` | all 0 |

### 1.2 Section entry (64 B)

| Off | Size | Field | Rule |
| --- | --- | --- | --- |
| 0 | 4 | `type` | kind-defined id, unique within the file |
| 4 | 2 | `type_major` | kind-defined (all 1 today) |
| 6 | 2 | `type_minor` | informational |
| 8 | 8 | `offset` | multiple of 64, inside the payload area |
| 16 | 8 | `length` | stored bytes; `offset + length <= file_size - 64` |
| 24 | 8 | `element_count` | `raw_length / element_stride` |
| 32 | 8 | `raw_length` | decoded bytes (= `length` when `codec` = None) |
| 40 | 4 | `element_stride` | nonzero; equals the dtype size for scalar dtypes |
| 44 | 4 | `flags` | bit 0 `Required`; other bits 0 |
| 48 | 4 | `crc32c` | CRC32C of the stored bytes |
| 52 | 4 | `raw_crc32c` | CRC32C of the decoded bytes (= `crc32c` when `codec` = None) |
| 56 | 1 | `dtype` | 0 Bytes, 1 U8, 2 U16, 3 U32, 4 U64, 5 I32, 6 I64, 7 F32, 8 F64, 9 Pod |
| 57 | 1 | `codec` | 0 None, 1 Zstd |
| 58 | 1 | `filter` | 0 None, 1 ByteShuffle (only with a codec) |
| 59 | 1 | `reserved0` | 0 |
| 60 | 4 | `reserved1` | 0 |

Bytes and Pod sizes are given by `element_stride`; the scalar dtypes have the obvious sizes.

### 1.3 Footer (64 B)

| Off | Size | Field | Rule |
| --- | --- | --- | --- |
| 0 | 8 | `magic` | `VPF-END\n` |
| 8 | 8 | `file_size` | equals the header's |
| 16 | 8 | `section_table_offset` | equals the header's |
| 24 | 4 | `section_count` | equals the header's |
| 28 | 4 | `kind` | equals the header's |
| 32 | 8 | `schema_fingerprint` | equals the header's |
| 40 | 4 | `header_crc32c` | equals the header's |
| 44 | 4 | `file_crc32c` | CRC32C of bytes [0, file_size - 64) |
| 48 | 4 | `footer_crc32c` | CRC32C of the 64 footer bytes with this field zeroed |
| 52 | 4 | `reserved0` | 0 |
| 56 | 8 | `reserved1` | 0 |

### 1.4 Checksums and codecs

- CRC32C is Castagnoli (reflected polynomial `0x82F63B78`, init and xor-out `0xFFFFFFFF`) via
  `detail::crc32c`. Known answers pinned by `PersistFile.Crc32cKnownAnswer`: `"123456789"` ->
  `0xE3069283`, 32 zero bytes -> `0x8A9136AA`.
- `Zstd`: one zstd frame per section with the content size recorded. A reader refuses
  (`CodecError`) a frame whose compressed size is not exactly `length` or whose content size is not
  exactly `raw_length`. `ByteShuffle` (Blosc-style) transposes the bytes of `element_stride`-wide
  elements before compression and back after it. Encoding fans sections out over the pricing
  executor; the bytes are identical for any thread count.
- The MarketState writer stores every section uncompressed (zero-copy mapping is the point). The
  codec path is exercised by `PersistFile.ZstdShuffleRoundTripsBitExact` and measured as an
  informational cold-tier option by `atx-vol-state-store bench`.

## 2. Verify levels and the reader contract

`PersistFile::open(path, kind, fingerprint, PersistOpenOptions{mapped, verify, prefetch,
expected_size})` maps the file read-only (or, with `mapped = false`, reads it with one sequential
read into an owned 64-byte aligned buffer). Section views borrow the mapping; the `PersistFile`
(and every archive built over it) owns the mapping through a `shared_ptr`, so a view cannot outlive
its bytes. A mapped file is never rewritten in place (section 5).

| Level | At open | On first `section(type)` |
| --- | --- | --- |
| `Framing` | header, footer, table, entry rules; codec sections decoded and both CRCs checked | nothing more |
| `Lazy` | as Framing | the section's stored CRC, once; the verdict (atomic, seq_cst) is sticky |
| `Full` | as Framing + every section CRC + every padding byte | nothing more |
| `Paranoid` | as Full + `file_crc32c` | nothing more |

Codec sections are always decoded and verified at open, so nothing allocates after open
(`PersistFile.NoAllocationAfterOpen`). `MarketStateArchive::open` defaults to `Full`
(`kMarketStateOpenDefaults`).

### 2.1 Validation order (the first refusal wins)

1. Base address not 8-byte aligned -> `Misaligned`.
2. Fewer than 320 bytes -> `BadMagic` if at least 8 bytes and the magic differs, else `Truncated`.
3. Header: magic -> `BadMagic`; endian tag -> `UnsupportedEndian`; major -> `UnsupportedMajor`;
   header/entry/footer sizes -> `HeaderInvalid`; `header_crc32c` -> `HeaderChecksum`; flags,
   reserved, table offset, count > 256, size not a nonzero multiple of 4096, `ts_min > ts_max` ->
   `HeaderInvalid`; bytes on disk < `file_size` -> `Truncated`, > `file_size` -> `SizeMismatch`;
   table + footer do not fit -> `HeaderInvalid`.
4. `kind` differs from the caller's -> `WrongKind`; fingerprint differs -> `SchemaMismatch`.
5. Footer: magic -> `FooterInvalid`; `footer_crc32c` -> `FooterChecksum`; reserved ->
   `FooterInvalid`; any mirrored field differs -> `FooterMismatch`.
6. `section_table_crc32c` -> `SectionTableChecksum`.
7. Each entry in table order: reserved -> `HeaderInvalid`; offset % 64 -> `SectionAlignment`;
   outside the payload area -> `SectionBounds`; before the previous section's end ->
   `SectionOverlap`; unknown dtype / codec / filter / flag bits -> `UnsupportedSection`; filter
   without codec -> `HeaderInvalid`; stride / count / raw length inconsistent (or `length !=
   raw_length` uncompressed) -> `SectionStride`; uncompressed `crc32c != raw_crc32c` ->
   `HeaderInvalid`.
8. Two entries with one `type` -> `DuplicateSection`.
9. Codec sections: stored CRC -> `SectionChecksum`; decode -> `CodecError`; raw CRC ->
   `RawChecksum`.
10. `Full` / `Paranoid`: each section CRC in file order -> `SectionChecksum`; a nonzero gap byte ->
    `PaddingNotZero`.
11. `Paranoid`: `file_crc32c` -> `FileChecksum`.

Then the kind's reader runs `check_schema`: a known section with the wrong dtype / stride /
major -> `SectionStride` / `UnsupportedSection`; a missing required section ->
`MissingSection`; an unknown section flagged `Required` -> `UnsupportedSection`; an unknown
optional section is skipped (`PersistFile.SkipsUnknownOptionalRefusesUnknownRequired`).
`typed<T>(type)` refuses a stride other than `sizeof(T)` (`SectionStride`) and a payload not
aligned for `T` (`Misaligned`).

`PersistFile.RefusesEachCraftedCorruption` pins 40 crafted files against these statuses.

## 3. Schema fingerprint

Each kind declares a constexpr table of `PersistSectionSpec {type, type_major, dtype, stride,
name, fields[], required}`; Pod sections list their fields as `{name, offset, size}` and
`persist_fields_tile` proves the list covers `sizeof` with no gap or overlap. The fingerprint is

```
h = FNV-1a/64 offset basis 0xcbf29ce484222325, prime 0x100000001b3, folding little-endian bytes:
h = fold(salt, 8)                      salt = the kind's 8-char tag read as a LE u64
h = fold(section_count, 4)
for each section:  fold(type,4) fold(type_major,2) fold(dtype,1) fold(stride,4)
                   fold(required ? 1 : 0, 1) fold_str(name)
                   fold(field_count,4)  for each field: fold_str(name) fold(offset,4) fold(size,4)
fingerprint = fmix64(h)                (MurmurHash3 finalizer)
fold_str(s) = fold(len(s), 4) then each byte of s
```

Salts: Manifest `ATXVMF01`, MarketState `ATXVMS01` (SurfaceTimeline `ATXVST01`, RiskRun
`ATXVRR01`, lane P2). `PersistFile.FingerprintKnownAnswer` pins one table's value
(`0xafc0b50aa5cf0f7a`, salt `0x3130414b56585441`) against an independent Python implementation.
Moving, resizing or renaming a field, or changing a section's dtype / stride / requiredness,
changes the fingerprint, so an old file is refused, never misread.

## 4. Directory manifest (kind Manifest)

A persist directory holds published files plus `persist-manifest.atxvpm`, an ATXVPF file of kind
Manifest with one required Pod section (type 1, stride 128) of `PersistManifestEntry`, sorted by
`key`, at most one entry per key:

| Off | Size | Field |
| --- | --- | --- |
| 0 | 8 | `seq` (publish generation, unique in the directory) |
| 8 | 48 | `file_name` (NUL-padded, canonical, section 4.1) |
| 56 | 16 | `key` (NUL-padded, `[A-Za-z0-9_.-]{1,15}`, not `.` or `..`) |
| 72 | 8 | `file_size` |
| 80 | 4 | `file_crc32c` (the file's footer value) |
| 84 | 4 | `header_crc32c` |
| 88 | 4 | `kind` |
| 92 | 4 | `records` (kind-defined; MarketState: mark count, saturating) |
| 96 | 8 | `schema_fingerprint` |
| 104 | 8 | `source_fingerprint` |
| 112 | 8 | `ts_min_ns` |
| 120 | 8 | `ts_max_ns` |

The manifest is read whole at `Paranoid`. A non-canonical name, a bad key, an unknown kind ->
`RecordInvalid`; keys out of order -> `IndexUnsorted`; a repeated key or seq -> `DuplicateKey`.

### 4.1 Canonical file names

| Kind | Name |
| --- | --- |
| MarketState | `bucket_et=<key>-g<seq, 12 digits>.atxms` |
| SurfaceTimeline | `<key>-g<seq12>.atxstl` |
| RiskRun | `seg-<seq12>.atxrr` |

A reader resolves a key only through the manifest; a file the manifest does not list is ignored
(`PersistDirectory.ReaderIgnoresUnlistedFile`), and a manifest name that is not canonical (path
separators, `..`) is refused (`RefusesTraversalFileName`).

### 4.2 Publish protocol

`PersistDirectory::publish(dir, key, image, records)`:

1. Open the image at `Framing` (its own kind and fingerprint) -> refuse a malformed image.
2. Create `dir`; take `dir/persist.lock` (exclusive; serializes publishers across processes,
   `LockSerializesConcurrentPublishers`).
3. Read the current manifest; `seq = max(seq) + 1`, skipping any seq whose canonical file already
   exists (an orphan of an interrupted publish).
4. Write the file to a temp name, flush, rename to its canonical name; refuse (`AlreadyExists`)
   if that name exists.
5. Replace or insert the key's entry, write the new manifest to a temp name, flush, rename over the
   old one.

A crash before step 5 leaves an unlisted file (ignored, and its seq is skipped next time); a
crash after it leaves the previous file unlisted. Published files are never rewritten or removed
by the writer; readers open them with `FILE_SHARE_DELETE` so a republish never blocks.

## 5. MarketState kind (`.atxms`)

One file is one `MarketStateStorage` (one SpiderRock bucket), optionally with the per-mark
selection extras and embedded fitted surfaces. Salt `ATXVMS01`. `MarketStateArchive::open` builds
a `MarketState` whose mark spans, term-carry rows and dividend rows point into the mapping
(zero-copy); yield curves are rebuilt from their pillars at open; embedded surfaces are served
from the embedded ATXVSA2 archive (borrowed bytes).

### 5.1 Sections (file order)

| Type | Name | dtype | Stride | Required | Rows |
| --- | --- | --- | --- | --- | --- |
| 1 | `meta` | Pod | 256 | yes | 1 |
| 2 | `underliers` | Pod | 96 | yes | one per underlier, strictly ascending uid |
| 3 | `strings` | Bytes | 1 | yes | tickers and surface symbols |
| 4 | `term-carry` | Pod | 32 (`ExpiryCarry`) | yes | per underlier, contiguous |
| 5 | `cash-divs` | Pod | 16 (`DividendEvent`) | yes | per underlier, contiguous |
| 6 | `yield-pillars` | Pod | 16 | yes | per distinct curve |
| 7 | `uid-table` | Pod | 8 | no | the writer's uid table (row i names uid i + 1) |
| 16 | `strike` | F64 | 8 | yes | one per mark |
| 17 | `expiry-ns` | I64 | 8 | yes | one per mark |
| 18 | `side` | U8 | 1 | yes | 0 call, 1 put |
| 19 | `sigma` | F64 | 8 | yes | one per mark |
| 20 | `vendor-price` | F64 | 8 | no | present iff `meta.has_vendor_price` |
| 21 | `vendor-delta` | F64 | 8 | no | present iff `meta.has_vendor_delta` |
| 32..38 | `bid` `ask` `vendor-gamma` `vendor-vega` `vendor-theta` `vendor-t` `row-uprc` | F64 | 8 | no | all seven present iff `meta.has_extras` |
| 48 | `surface-index` | Pod | 16 | no | one per embedded surface |
| 49 | `surface-archive` | Bytes | 1 | no | one ATXVSA2 image |

Per mark: core columns 41 B (strike, expiry, side, sigma, vendor price, vendor delta), extras
56 B. Mark rows are grouped by underlier in uid order; within one underlier they are
non-decreasing in (expiry, side, strike). Equal keys are allowed (the source may carry duplicate
marks). A vendor column missing for one underlier but present for
another is stored NaN-filled for the missing rows; the row's `kRowVendorPrice` / `kRowVendorDelta`
flag says whether the span is populated.

### 5.2 Records

`Meta` (256 B): `ts_ns` i64 @0; `time_convention` u8 @8 (0 Calendar365, 1 VolTime);
`has_vendor_price` @9, `has_vendor_delta` @10, `has_extras` @11 (0/1); `conventions_version` u32
@12; the five `VolTimeParams` doubles @16..55 (alpha, trading hours/yr, non-trading hours/yr,
session open hour ET, session span hours); counts u64 @56..103 (`n_underliers`, `n_marks`,
`n_term_carry`, `n_cash_divs`, `n_yield_pillars`, `n_surfaces`); the `MarketStateBuildReport`
@104..167 (`n_uids`, `n_uids_dropped`, `n_marks`, `n_uids_with_divs`, `n_expiry_rows`,
`max_ddiv_spread` f64, `n_duplicate_marks`, `n_rows_bad_sigma`); `has_report` u8 @168;
`reserved0[7]` @169; `date[16]` @176 (NUL-padded `YYYY-MM-DD`); `bucket_et[8]` @192 (NUL-padded
`HHMM`); `reserved1[56]` @200.

`UnderlierRow` (96 B): `uid` u32 @0; `carry` u8 @4 (`CarryFrom`, 0/1); `mark_flags` u8 @5
(1 has marks, 2 vendor price, 4 vendor delta); `reserved0` u16 @6; `spot`, `flat_rate`, `flat_q`
f64 @8/16/24; `marks_begin`, `marks_count` u64 @32/40; `carry_begin/count`,
`divs_begin/count`, `curve_begin/count` u32 @48..71 (curve count 0 = no yield curve);
`ticker_offset/length` u32 @72/76; `surface_slot` u32 @80 (`0xFFFFFFFF` = none); `reserved1`
u32 @84; `reserved2` u64 @88.

`YieldPillar` (16 B): `t_years`, `zero_rate` f64. `UidTableRow` (8 B): `offset`, `length` u32
into `strings`. `SurfaceIndexRow` (16 B): `uid`, `reserved`, `symbol_offset`, `symbol_length` u32.
`ExpiryCarry` (32 B: `expiry_ns`, `pv_divs`, `rate`, `q`) and `DividendEvent` (16 B: `ex_date_ns`,
`amount`) are the library's own PODs, stored as-is; their field lists are part of the fingerprint.

The writer refuses a yield curve whose rebuilt discount factors are not bit-equal to the source
curve at every pillar, midpoint and both extremes (`YieldCurvePillars` input; the public
`YieldCurve` has no pillar accessor).

### 5.3 Reader validation (after section 2.1)

`check_schema`; exactly one `meta` row with flags in range, zero reserved bytes and terminated
date / bucket strings -> else `RecordInvalid`; every column's row count equals its meta count ->
else `SectionStride`; `side` bytes in {0, 1} -> else `RecordInvalid`; an optional column present
without its meta flag (or absent with it) -> `RecordInvalid`; surface sections without
`n_surfaces` -> `RecordInvalid`. Then rows: uid not strictly ascending -> `IndexUnsorted`; bad
carry / flags / reserved, or vendor flag without the meta column -> `RecordInvalid`; mark ranges
not contiguous from 0, any carry / dividend / pillar / ticker / uid-table / surface reference out
of range -> `IndexBounds`. Then mark order within each underlier -> `IndexUnsorted` (detail =
row). Then `MarketState::create` and the embedded surfaces (`SurfaceArchiveV2::open_borrowed`,
uid / symbol cross-check) -> `RecordInvalid` on refusal.

## 6. The SpiderRock-derived state store

### 6.1 Layout

```
<out_root>/date=YYYY-MM-DD/persist-manifest.atxvpm
<out_root>/date=YYYY-MM-DD/persist.lock
<out_root>/date=YYYY-MM-DD/bucket_et=HHMM-g<seq12>.atxms     one per bucket (manifest key = HHMM)
```

`out_root` must not be inside the SpiderRock store root (`InvalidArgument`; the comparison is on
weakly-canonical, case-folded paths on Windows).

### 6.2 Source fingerprint (`spiderrock_state_source_fingerprint`)

FNV-1a/64 + fmix64 (as section 3) over: salt `ATXVSS01`; `conventions_version` (u32, required
nonzero); `ATX_VOL_VERSION`; the store root (lexically normal, generic separators); date; the
underlier filter list; `max_underliers`; time convention and the five `VolTimeParams` doubles
(bit patterns); `american_only`; `standard_roots_only`; the bucket; then, over the bucket's
`*.parquet` files sorted by name: file name, size, last-write time. A bucket is rebuilt when its
fingerprint or extras choice differs from the archived one (or `force`), or when its archived
`uid-table` is not a prefix-consistent replay of the build's uid assignment so far; otherwise it is
skipped and its uid assignments are replayed from the archived `uid-table`, so later buckets of the
same call get the same uids as a from-scratch build.

### 6.3 Deterministic uids

Buckets load in parallel into private underlier tables, then remap serially, in bucket order,
into the day's table: the result equals one serial load (tickers interned in first-seen order).
Rebuilding a day with `--parallel 1` and `--parallel 2` gives byte-identical files (see
`../persist.md`). The archives of ONE build call share one uid space (what a `RiskStream` replay
needs); a call over a sub-range of buckets (`--from` / `--to`) starts its uid table fresh, so build
a day in one call when its archives will be replayed together.
