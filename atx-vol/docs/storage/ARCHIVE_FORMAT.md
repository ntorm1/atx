# ATXVFA: archive format for fitted vol surfaces and their serving records

Status: format major 1, minor 0 (lane `surface-archive`, 2026-10-17). This file
is the on-disk specification. The POD structs that implement it live in
`include/atx/vol/api/storage/fit_archive_format.hpp`; every size and offset
below is pinned there by `static_assert`, so the two cannot drift silently.

Name: the format is **ATXVFA** ("atx vol fit archive"), after its file
extension `.atxvfa`. It is unrelated to, and does not replace, the two existing
containers:

| Container | Holds | Key |
| --- | --- | --- |
| ATXVSA2 (`surface_archive.hpp`, `atxvsa2-format.md`) | backtest `PricedSurface` curve surfaces | symbol |
| ATXVDB (`surface_db.hpp`) | manifest of ATXVSA2 partitions | (symbol, date) |
| **ATXVFA** (this file) | per-expiry quote-slice fits with grade / status / repair diagnostics, cross-expiry quote surfaces, `ExpiryRecord`s and their chain (spot-ladder) sources, common Markov model specs | (underlier, snapshot, expiry) |

Nothing reads or writes ATXVFA by default. It is a new module; no existing
fitting, pricing, serving or benchmark path calls it.

## 1. Design goals and the choices that meet them

| Goal | Choice |
| --- | --- |
| zero-copy reads of large arrays | fixed little-endian layout, every section 64-byte aligned, every element naturally aligned; arrays are read as `std::span<const T>` straight off the mapping |
| O(1) random access to (underlier, snapshot, expiry) | per record type a sorted fixed-stride index (24 B per key), binary search, then O(1) record address `offset + i * stride` |
| strict validation before any view | `open` checks framing, every checksum, every bound, every index order and every record reference before it returns |
| typed refusal of corrupt / truncated / foreign files | `ArchiveStatus` enum (no strings, no exceptions on the read path) |
| atomic, single-fsync writes | build in memory, write a unique same-directory temp, `FlushFileBuffers`/`fsync` it once, rename onto the destination |
| append without rewriting | a series is a directory of immutable segment files plus a manifest that is itself an ATXVFA file (section 9) |
| schema evolution | file major/minor plus a major/minor/stride per section (section 8) |

Checksum: CRC-32C (Castagnoli). atx-core has none; atx-vol already ships a tested
hardware-dispatched implementation (`src/storage/archive_util.hpp`,
`detail::crc32c`, SSE4.2 `_mm_crc32` with a bit-identical table fallback) that
ATXVSA2 and ATXVDB use. ATXVFA reuses it; the archive tests add the standard
known-answer vector (`"123456789"` -> `0xE3069283`).

Memory mapping: `atx::tsdb::Mapping` (atx-tsdb, `CreateFileMappingW` /
`MapViewOfFile` behind RAII; POSIX `mmap`). Atomic publish:
`detail::reserve_unique_publish_temp_file` + `detail::flush_and_publish_file`
(the ATXVSA2/ATXVDB primitive). Cross-process series append lock:
`detail::WriterLock`. No new IO primitive is introduced.

## 2. File layout

```
offset 0                 FileHeader            256 B
offset 256               SectionTable          section_count x 64 B
next multiple of 64      section 0 payload     (zero padding up to the next 64)
...                      section n-1 payload
file_size - 64           Footer                64 B
```

* All integers little-endian, all floating point IEEE-754 binary64. The writer
  and reader refuse to build on a big-endian host (`static_assert`); a file whose
  `endian_tag` does not read back as `0x01020304` is `UnsupportedEndian`.
* Every section starts on a 64-byte boundary. `file_size` is a multiple of 64.
  Every byte that is not header, table, section payload or footer is zero
  padding, and `open` verifies it is zero.
* Sections appear in the file in table order, strictly increasing offsets, no
  overlap.

### 2.1 FileHeader (256 B, offset 0)

| Off | Size | Field | Value / meaning |
| --- | --- | --- | --- |
| 0 | 8 | `magic` | `89 56 46 41 0D 0A 1A 0A` (`\x89VFA\r\n\x1A\n`: high bit, CRLF and ^Z detect 7-bit or text-mode transfer damage, PNG-style) |
| 8 | 2 | `format_major` | 1 |
| 10 | 2 | `format_minor` | 0 |
| 12 | 2 | `header_size` | 256 |
| 14 | 2 | `section_entry_size` | 64 |
| 16 | 4 | `endian_tag` | `0x01020304` |
| 20 | 4 | `footer_size` | 64 |
| 24 | 8 | `file_size` | total bytes |
| 32 | 8 | `section_table_offset` | 256 |
| 40 | 4 | `section_count` | <= 256 |
| 44 | 4 | `flags` | 0 (reserved) |
| 48 | 8 | `created_unix_ns` | writer-supplied (tests pin it for determinism) |
| 56 | 8 | `writer_version` | `ATX_VOL_VERSION_NUM(major, minor, patch)` of the writing library |
| 64 | 8 | `archive_id` | caller id (0 = none); a series stamps its id on every segment |
| 72 | 8 | `segment_sequence` | 0 standalone; series segment number (1, 2, ...) |
| 80 | 32 | `writer_name` | ASCII, NUL padded (e.g. `atx-vol 0.9.0`) |
| 112 | 4 | `section_table_crc32c` | CRC-32C of the section table bytes |
| 116 | 4 | `header_crc32c` | CRC-32C of these 256 bytes with this field zero |
| 120 | 136 | `reserved` | zero |

### 2.2 SectionEntry (64 B, one per section)

| Off | Size | Field | Meaning |
| --- | --- | --- | --- |
| 0 | 4 | `type` | `FitArchiveSectionType` (section 4) |
| 4 | 2 | `type_major` | record-type schema major |
| 6 | 2 | `type_minor` | record-type schema minor |
| 8 | 8 | `offset` | absolute, multiple of 64 |
| 16 | 8 | `length` | payload bytes, excluding padding |
| 24 | 8 | `element_count` | number of elements |
| 32 | 4 | `element_stride` | bytes per element; `length == element_count * element_stride` (blobs: stride 1) |
| 36 | 4 | `element_align` | required element alignment (1 or 8); `offset` and stride are multiples |
| 40 | 4 | `flags` | bit 0 `Required`: a reader that does not know `type` must refuse the file |
| 44 | 4 | `crc32c` | CRC-32C of `[offset, offset + length)` |
| 48 | 16 | `reserved` | zero |

A type appears at most once per file (`DuplicateSection`).

### 2.3 Footer (64 B, at `file_size - 64`)

| Off | Size | Field | Meaning |
| --- | --- | --- | --- |
| 0 | 8 | `magic` | `56 46 41 2D 45 4E 44 0A` (`VFA-END\n`) |
| 8 | 8 | `header_offset` | 0: where the header is, so the file is checkable from its tail |
| 16 | 8 | `file_size` | must equal the header's and the real size |
| 24 | 8 | `section_table_offset` | copy of the header's |
| 32 | 4 | `section_count` | copy of the header's |
| 36 | 4 | `header_crc32c` | copy of the header's |
| 40 | 4 | `file_crc32c` | CRC-32C of `[0, file_size - 64)`: the whole-file checksum |
| 44 | 4 | `footer_crc32c` | CRC-32C of these 64 bytes with this field zero |
| 48 | 8 | `segment_sequence` | copy of the header's |
| 56 | 8 | `reserved` | zero |

Truncation is detected from either end: a short file fails the header's
`file_size`; a file cut and re-padded fails the footer magic/CRC; a file with
appended bytes fails `file_size` equality (`SizeMismatch`). The footer copies let
a tail reader (for example a series manifest check) identify the file and its
whole-file checksum without parsing the header.

## 3. Shared value types

| Type | Size | Layout |
| --- | --- | --- |
| `FitArchiveKey` | 24 | `u32 underlier` @0 (symbol id), `i32 expiry_day` @4 (days since 1970-01-01; 0 for snapshot-level records), `i64 snapshot_ns` @8 (Unix ns of the valuation snapshot), `u32 sub` @16 (disambiguator, see each record), `u32 reserved` @20 |
| `FitArchiveIndexEntry` | 24 | same first 20 bytes as the key, then `u32 record` @20 (row in the record section) |
| `FitArchiveStringRef` | 8 | `u32 offset`, `u32 length` into the `Strings` blob |
| `FitArchiveArrayRef` | 8 | `u32 first`, `u32 count`: element indices into the named pool section |
| `FitArchiveSymbol` | 48 | `u32 length` @0, `u32 reserved` @4, `char name[40]` @8 (bytes past `length` zero) |

Key order is (underlier, snapshot_ns, expiry_day, sub), each compared as a signed
or unsigned integer of its own type. Symbol ids are positions in the `Symbols`
section, which is sorted strictly by (bytes, length) and unique; symbol bytes are
stored exactly as supplied (no case folding). An underlier is resolved once by a
binary search over `Symbols`; after that every lookup is integer-keyed.

## 4. Sections

| Type | Name | Stride | Align | Major.minor | Required | Content |
| --- | --- | --- | --- | --- | --- | --- |
| 1 | `Symbols` | 48 | 8 | 1.0 | yes | `FitArchiveSymbol[]`, sorted |
| 2 | `Strings` | 1 | 1 | 1.0 | yes | UTF-8 bytes referenced by `FitArchiveStringRef` |
| 3 | `Provenance` | 1 | 1 | 1.0 | no | caller bytes (e.g. the policy JSON and run receipt); never interpreted |
| 16 | `SurfaceRecords` | 192 | 8 | 1.0 | yes | `FitArchiveSurfaceRecord[]` (section 5.2) |
| 17 | `SurfaceIndex` | 24 | 8 | 1.0 | yes | index over 16 |
| 32 | `FitRecords` | 896 | 8 | 1.0 | yes | `FitArchiveFitRecord[]` (section 5.1) |
| 33 | `FitIndex` | 24 | 8 | 1.0 | yes | index over 32 |
| 34 | `PriceNodes` | 24 | 8 | 1.0 | yes | `{f64 strike, call, put}` = `AmericanPriceNode` |
| 35 | `QuoteRows` | 48 | 8 | 1.0 | yes | `FitArchiveQuoteRow[]` |
| 36 | `MovedNodes` | 1 | 1 | 1.0 | yes | `u8[]` (`QuoteSliceFit::moved_nodes`) |
| 37 | `Dividends` | 16 | 8 | 1.0 | yes | `{f64 tau, amount}` = `CashDividend` |
| 38 | `CarryPoints` | 16 | 8 | 1.0 | yes | `{f64 strike, continuous_yield}` = `QuoteSliceCarryPoint` |
| 39 | `CurveWords` | 8 | 8 | 1.0 | yes | 8-byte words of curve payloads (section 6) |
| 48 | `ExpiryMeta` | 128 | 8 | 1.0 | yes | `FitArchiveExpiryMeta[]` (section 5.3) |
| 49 | `ExpiryData` | 1824 | 8 | 1.0 | yes | `ExpiryRecord` object representations (section 7) |
| 50 | `ExpiryIndex` | 24 | 8 | 1.0 | yes | index over 48 |
| 51 | `Ladders` | 192 | 8 | 1.0 | yes | `FitArchiveLadderRecord[]` (section 5.4) |
| 52 | `LadderIndex` | 24 | 8 | 1.0 | yes | index over 51 |
| 53 | `LadderContracts` | 48 | 8 | 1.0 | yes | `FitArchiveLadderContract[]` |
| 64 | `MarkovRecords` | 192 | 8 | 1.0 | yes | `FitArchiveMarkovRecord[]` (section 5.5) |
| 65 | `MarkovIndex` | 24 | 8 | 1.0 | yes | index over 64 |
| 66 | `MarkovIntervals` | 56 | 8 | 1.0 | yes | `FitArchiveMarkovInterval[]` |
| 67 | `MarkovLocalVol` | 16 | 8 | 1.0 | yes | `{f64 log_state_over_spot, volatility_multiplier}` = `AmericanMarkovLocalVolNode` |
| 68 | `MarkovWords` | 8 | 8 | 1.0 | yes | `f64[]` (state grids, fit sigmas / knots / prices) |
| 80 | `SeriesSegments` | 128 | 8 | 1.0 | yes | series manifest entries (section 9) |

Absent sections mean "no records of that kind". The writer emits only non-empty
sections (a file with no records still carries `Symbols` and `Strings`, both
possibly empty). Every keyed section `R` has its index `R + 1`; both are present
or both absent, with equal element counts.

Pool sections (34-39, 53, 66-68) are shared by all records of a file; a record
names its slice of a pool with a `FitArchiveArrayRef`. Records never point at
each other with byte offsets, only with element indices, so a section can be
copied or relocated without patching.

## 5. Records

All records start with a `FitArchiveKey`, contain only fixed-width fields in
naturally aligned positions, and carry explicit zero `reserved` bytes; no record
has implicit padding. Enumerations are stored as their `std::uint8_t` underlying
value and range-checked on open. Booleans are bytes restricted to 0/1, or bits
of a named `flags` byte.

### 5.1 `FitArchiveFitRecord` (896 B): one expiry fit

One `QuoteSurfaceSlice` (graded admission) or one standalone `QuoteSliceFit`
(static path). Key: (underlier, snapshot, expiry_day, `sub` = caller ordinal,
0 normally).

| Off | Size | Group | Source fields |
| --- | --- | --- | --- |
| 0 | 24 | `key` | |
| 24 | 48 | `market` | `QuoteSliceMarket` (6 x f64) |
| 72 | 280 | `diagnostics` | `QuoteSliceDiagnostics`: 6 counts (u64), forward / discount / effective rate / effective yield, `curve_fit` (`FitDiag`, 48 B: 3 f64, 3 u32, 2 u16, termination, projection, flags bit0 reverted_to_seed bit1 warm_started bit2 has final_grad_norm), `channel` (96 B: 6 u64, 5 f64, flags bit0 used bit1 term_prior bit2 converged), `repair` (`QuoteSliceRepairRecord`, 56 B) |
| 352 | 144 | `carry` | `QuoteSliceCarryResolution`: 11 f64, 5 u64, `reason` string, mode / applied / fallback / status |
| 496 | 48 | `curve` | `present`, `kind` (`VolCurveKind`), base `T`, `F`, `df`, `node_count`, `payload` ref into `CurveWords` |
| 544 | 56 | `prices` | `present`, `AmericanPriceBounds` (5 f64), `nodes` ref into `PriceNodes` |
| 600 | 16 | `al_opts` | `AlOpts` (tol, 4 x u16); meaningful when `has_al_opts` |
| 616 | 184 | `slice` | `QuoteSurfaceSlice` extras: calendar gap / gap strike / repair required / point outside fraction, point outside quotes, `rung_repair`, `calendar_channel_repair`, refusal / error / point_error strings, grade, point_status, flags (bit0 channel_refit, bit1 calendar_retry, bit2 calendar_repaired, bit3 repair_rung, bit4 calendar_channel_repaired, bit5 point_gated, bit6 point_fallback, bit7 calendar_readmitted), `present` (0 = standalone fit) |
| 800 | 8 | `rows` | ref into `QuoteRows`, one per input quote (`QuoteSliceFit::rows`) |
| 808 | 8 | `moved_nodes` | ref into `MovedNodes`, stored as the fit holds it (each value 0..3); usually count 0 or `prices.nodes.count`, but a slice stripped after a repair keeps its flags with `prices` absent, so the count is not tied to `prices` |
| 816 | 8 | `dividends` | ref into `Dividends` (`variance_clock_dividends`) |
| 824 | 8 | `carry_curve` | ref into `CarryPoints` |
| 832 | 8 | `fit_error` | string |
| 840 | 4 | `discrete_div_steps` | i32 |
| 844 | 4 | `surface` | owning `SurfaceRecords` row, `0xFFFFFFFF` for a standalone fit |
| 848 | 4 | | `fit_status`, `dividend_model`, `discrete_div_scheme`, `has_al_opts` |
| 852 | 44 | `reserved` | zero |

`FitArchiveQuoteRow` (48 B): `market_iv`, `fitted_iv`, `fitted_price`,
`spread_vol`, `weight_w` (f64), then `quote_status`, `evaluation_status`,
`admitted`, `weight_capped` (u8) and 4 reserved bytes.

### 5.2 `FitArchiveSurfaceRecord` (192 B): one cross-expiry quote surface

One `fit_quote_surface` result for ONE underlier at ONE valuation instant. Key:
(underlier, snapshot, expiry_day 0, sub 0).

| Off | Size | Field |
| --- | --- | --- |
| 0 | 24 | `key` |
| 24 | 48 | `QuoteSurfaceConfig` doubles (term_prior_weight, prior_decay_log_time, prior_quote_halflife, channel_node_weight, calendar_retry_factor, calendar_repair_half_spreads) |
| 72 | 8 | `grade_b_min_mid_strikes` (u64) |
| 80 | 32 | `policy_digest`: caller digest of the policy that produced the surface (the bench stores the SHA-256 of the policy document) |
| 112 | 8 | `profile` string (resolved policy profile name) |
| 120 | 8 | `policy_source` string |
| 128 | 8 | `first_fit`, `fit_count`: the surface's expiries, contiguous and in key (expiry) order in `FitRecords` |
| 136 | 16 | `grade_counts[4]`: A, B, C, Refused |
| 152 | 8 | `flags`, reserved |
| 160 | 32 | reserved |

### 5.3 `FitArchiveExpiryMeta` (128 B) + `ExpiryData`: per-expiry calibration records

Key: (underlier, snapshot, expiry_day, `sub` = side << 16 | ordinal), where the
ordinal separates records of one (expiry, side) built at different carries.
Fields: the record's public accessor values (`expiry`, `rate`, `carry`,
`k_min`, `k_max`, `sigma_min`, `sigma_max`, `forward_factor`,
`certified_max_abs_error_fwd`, `certified_points`, `uncertified_rows`, `side`,
`populated`, `american`, `has_exercise_edge`), the owning `ladder` row
(`0xFFFFFFFF` for a standalone record) and `data_index`, the row in
`ExpiryData` that holds the record itself (section 7).

### 5.4 `FitArchiveLadderRecord` (192 B) + `LadderContracts`: chain record sources

A `ChainSpotLadder` is a PIMPL with no readable state, so the archive stores
what it is built from: the chain (`ChainContractsView` columns, one
`FitArchiveLadderContract` = strike, expiry, sigma, rate, carry, record row,
side per contract), the build spot, `ChainRecordPolicy`, `SpotLadderPolicy`, the
range of `ExpiryMeta` rows that are the chain's `ChainRecords::bank()` (in bank
order), `record_of_contract` (each contract's `record`) and
`worst_certified_error_fwd`. Key: (underlier, snapshot, expiry_day, sub
ordinal). The stored `ExpiryRecord`s serve every contract directly
(`ExpiryRecord::greeks`, zero copy); a ladder itself is rebuilt with
`ChainRecords::build` + `ChainSpotLadder::build`, which are deterministic for a
given chain and policy, and the archive test checks the rebuilt bank is
bit-identical to the stored records.

### 5.5 `FitArchiveMarkovRecord` (192 B) + intervals: common Markov model specs

The compiled `AmericanMarkovModel` is derived state (up to 8 M transition rows)
that only `compile_american_markov` can create, so the archive stores its input,
the `AmericanMarkovSpec`: spot, `AmericanMarkovConfig` (all eleven fields), the
state grid (`MarkovWords`), the intervals (`FitArchiveMarkovInterval`, 56 B:
calendar_end, variance_end, rate, continuous_yield, volatility,
cash_dividend_at_end, local-vol ref into `MarkovLocalVol`), plus an optional fit
summary (`AmericanMarkovFit` sigmas, log-state knots, time shape, variance time
knots, quote prices) and a label string. Compilation is deterministic, so the
archived spec reproduces the model bit for bit (tested). Key: (underlier,
snapshot, expiry_day 0, sub ordinal).

## 6. Curve payloads (`CurveWords`)

8-byte words; integer fields are stored as their u64 bit pattern. `node_count`
is the per-kind array length `n`.

| Kind | Words | Layout |
| --- | --- | --- |
| 0 ConvexDense | 13 + 2n | T, F, df, rmse_price, n_obs, n_active, qp_iterations, qp_stationarity, qp_primal_violation, qp_complementarity, qp_dual_violation, effective_lambda, noise_scale, `u[n]`, `C[n]` |
| 1 Essvi | 31 | the `EssviParams` native layout (248 B), written field by field over zeroed bytes by the shared ATXVSA2 helper (`surface_archive_payload.hpp`, offsets pinned there) |
| 2 Svi | 9 | `SviParams` native layout (72 B), same helper |
| 3 LinearVariance | 2n | `k[n]`, `w[n]` |
| 4 C8 | 22 | `C8Params` native layout (176 B), same helper |
| 5 SplineVol | 6 + 2n | atm_vol, z_lo_valid, z_hi_valid, mult_cap, w_offset, n_butterfly_viol, `z[n]`, `mult[n]` |

Decoding constructs the concrete curve through its public constructor and
refuses the record (`RecordInvalid`) unless the rebuilt curve's `T()`, `F()` and
`df()` equal the stored ones bit for bit.

## 7. The one layout-coupled adapter: `ExpiryRecord`

`ExpiryRecord` is a class with private state and no public constructor from its
coefficients, and its internals are out of scope for this lane. It is trivially
copyable and standard layout, so the archive stores its object representation:
`sizeof(ExpiryRecord) == 1824` bytes, of which bytes `[0, 1820)` are the
declared members in declaration order (192 + 12 + 12 coefficient doubles, ten
scalar doubles, two u32, `Side` and three `bool` bytes) and `[1820, 1824)` are
tail padding, which the writer zeroes so files are deterministic. This is
pinned three ways:

1. `static_assert` on `sizeof`, `alignof`, trivially-copyable and
   standard-layout (a member added, removed or resized breaks the build of the
   storage module, not a file);
2. a one-time self-check of a default-constructed record's object
   representation against the expected byte pattern (`growth_ = df_ = 1.0`,
   `side_ = Put`, booleans false); on mismatch `ExpiryData` is refused with
   `LayoutMismatch` instead of being reinterpreted;
3. on open, every stored record's flag bytes must be 0/1, its padding zero, and
   its public accessors must equal the `ExpiryMeta` copy bit for bit
   (`LayoutMismatch` otherwise).

Readers get `std::span<const ExpiryRecord>` over the mapping and can price
(`price`, `greeks`, `implied_vol`) with no copy and no allocation.

## 8. Validation, errors, schema evolution

`ArchiveStatus` (u16) is the only error currency of the read path, carried in
`ArchiveError {status, section, detail}` (no strings, so a refusal never
allocates; `to_string(ArchiveStatus)` gives a static name). `open` performs, in
order: size floor (`Truncated`), header magic (`BadMagic`), endian tag, format
major (`UnsupportedMajor`), header sizes (`HeaderInvalid`), header CRC
(`HeaderChecksum`), real size vs `file_size` (`Truncated` / `SizeMismatch`),
footer magic / CRC / copies (`FooterInvalid` / `FooterChecksum` /
`FooterMismatch`), section table bounds and CRC (`SectionTableChecksum`),
per-section bounds / alignment / ordering / stride / duplicates
(`SectionBounds`, `SectionOverlap`, `SectionAlignment`, `SectionStride`,
`DuplicateSection`), unknown required types (`UnsupportedSection`), record-type
majors (`UnsupportedRecordVersion`), then (verify level `Full`, the default)
every section CRC (`SectionChecksum`) and every padding byte (`PaddingNotZero`),
then the record pass: symbols sorted and unique (`IndexUnsorted`), each index
strictly sorted, in range and equal to its record's key (`IndexUnsorted`,
`IndexBounds`), every reference inside its pool (`RecordBounds`), every enum,
flag and reserved byte valid (`RecordInvalid`), and the `ExpiryRecord` checks
(`LayoutMismatch`). Verify level `Paranoid` adds the whole-file CRC
(`FileChecksum`); level `Framing` skips the section CRCs and padding scan (for a
trusted cache) but never the structural pass. Each refusal is exercised by a
crafted file in `tests/fit_archive_test.cpp`.

Evolution rules:

* File `format_major` must equal the reader's; a newer `format_minor` is
  readable (it can only add optional sections or new section types).
* A section with an unknown `type` is skipped unless its `Required` flag is set
  (`UnsupportedSection`).
* A known section whose `type_major` differs from the reader's is refused
  (`UnsupportedRecordVersion`).
* Minor evolution of a fixed-stride record type appends fields at the end and
  grows `element_stride`; the zero value of every appended field means
  "absent". A reader handed a different stride than its own `sizeof(T)` copies
  each element's first `min(stride, sizeof(T))` bytes into a zeroed `T` once, at
  open (older minor: missing tail reads as zero; newer minor: unknown tail
  ignored), and serves views from that copy. A stride below a type's minor-0
  stride is `SectionStride`. `ExpiryData` is exempt: its stride must equal
  `sizeof(ExpiryRecord)` exactly (`LayoutMismatch`).

## 9. Writing, appending, compaction

Writer: `FitArchiveWriter` copies every input into staging vectors (inputs are
not retained; each `add_*` is all-or-nothing, so a refused add leaves the writer
unchanged), then at `finish` builds the sorted symbol table and rewrites the
records' symbol ids to it, keeps every record type in insertion order, builds
each type's index sorted by key, refuses duplicate keys (`DuplicateKey`), lays
the sections out and computes all checksums. Only `Symbols` and `Strings` are
always emitted; any other section is written only when it has elements (a
missing index of a present record type is `MissingSection`). `finish` is
deterministic: the same adds and `FitArchiveFileInfo` give the same bytes.
`publish_fit_archive` / `write_file` publish with one fsync of the temp file and
an atomic rename (POSIX additionally fsyncs the directory); a reader never sees
a partial archive, and on a failed rename the temp is preserved and the previous
file is intact.

Series (append): a directory holding immutable segments
`seg-<sequence, 12 digits zero-padded>.atxvfa` and the manifest `series.atxvfa`,
an ATXVFA file whose only section is `SeriesSegments` (128 B per segment:
sequence, file name, file size, whole-file CRC, header CRC, min / max snapshot,
record counts). Sequences are nonzero and unique within a manifest (not
necessarily increasing in list order, because compaction puts the merged segment
where the run was); a new segment takes the largest listed sequence plus one.
Append (`append_fit_archive_segment`) = take the `series.lock` WriterLock
(`Busy` after `lock_timeout`), read the manifest, refuse keys already present in
a segment whose snapshot range overlaps (`DuplicateKey`), publish the new segment
(temp + fsync + rename), then publish the new manifest the same way. A crash
between the two leaves an unreferenced segment that the next append of the same
sequence replaces; readers only ever see segments the manifest lists. Readers
(`FitArchiveSeries::open`) read the manifest into memory with a Paranoid check
(never map it, so it can be replaced while they run), map the listed segments,
and refuse a segment whose size, whole-file CRC (footer copy) or sequence
differs from its manifest entry (`SegmentMismatch`).

Why segments rather than an append log: (1) on Windows a mapped file cannot be
extended or replaced while any reader maps it (`atx::tsdb::Mapping` opens with
`FILE_SHARE_READ`), so an in-place append log would either block on readers or
need remapping; immutable segments are never written after publish, so readers'
mappings never change underneath them; (2) a torn append can only damage the
new segment, which is self-validating, never the published history;
(3) visibility is one atomic manifest rename; (4) compaction is a plain merge.

Compaction: `compact_fit_archive_series(dir, first, count)` copies the records
of a run of manifest entries into one new segment through
`FitArchiveWriter::copy_from` (records are copied as stored, with only their
symbol ids and pool references rebased, so compaction is bit-exact and never
re-encodes a fit), publishes a manifest in which that segment, under a new
sequence, takes the run's position, then deletes the old segment files
best-effort (`FitArchiveCompaction::removed` / `kept`: a file still mapped by a
reader on Windows is kept; `gc` of unreferenced segments is a later run).

Concurrency: an open `FitArchive` is immutable; every query is `const`,
allocation-free and safe from any number of threads. Writers are
single-threaded objects. Series appends are serialised across processes by the
lock.

## 10. Allocation budget

`open` allocates the reader's section table copy, the file buffer in `Read` mode
(`open_copy` likewise), and, only for a section whose stride differs from the
reader's struct, an upgraded copy. After `open`, every
view (`fits()`, `find_fit`, `price_nodes`, `quote_rows`, `expiry_records`,
`find_expiry`, symbol lookup, string views, `ExpiryRecord` pricing) is
allocation-free; `tests/fit_archive_records_test.cpp` checks it with a global
allocation counter. `decode_*` functions materialise owned library objects
(`QuoteSurfaceSlice`, `AmericanPriceSlice`, curves, Markov specs) and allocate.

## 11. Tools

`atx-vol-surface-archive` (`tools/surface_archive_main.cpp`, read-only):
`inspect <file|series-dir>` prints the header, the section table and record
counts (fit grades A / B / C / refused / standalone); `dump <file>
[--underlier S]` prints one tab-separated line per record; `verify
<file|series-dir>` runs a `Paranoid` open and, on refusal, reopens at `Framing`
to name each damaged section. Exit codes: 0 ok, 1 usage, 2 refused (the
`ArchiveStatus` name is printed).

`atx-vol-fit-archive-bench` (`tools/surface_archive_bench_main.cpp`; the name
`atx-vol-surface-archive-bench` belongs to the ATXVSA2 bench in `bench/`)
reproduces one cohort's graded quote-slice run, checks every served row against
that run's predictions CSV bit for bit, archives the payload with each served
expiry's `ChainRecords`, and reports write, open and random-access timings.
Results: `sprints/2026-10-17-surface-archive-sprint.md`.

## 12. Not in this version

Compression (a per-section codec field can be added as a new section flag in a
minor), network transport, and any default serve path that reads the archive.
