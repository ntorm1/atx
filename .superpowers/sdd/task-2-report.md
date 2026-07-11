# Task 2 Report — surface_db.hpp on-disk records, SymbolFitConfig, manifest write/parse

## Summary

Implemented the ATXVDB v1 manifest binary format: fixed-layout on-disk records
(`DbManifestHeader`, `DbSymbolRecord`, `DbPartitionRecord`), the public
`SymbolFitConfig` (full `CurveConfig` + `AlOpts` override + session policy
scalars), an in-memory `write_db_manifest` writer, and a validated `DbManifest`
reader with O(log n) canonical symbol/partition lookup. Pure in-memory per the
brief — file IO and the `SurfaceDb` class are Task 3.

## Files changed

- Created `atx-vol/include/atx/vol/surface_db.hpp` — header matches the
  brief's code block verbatim (names/types/constants/static_asserts), with a
  house-style top-of-file doc comment (on-disk shape / integrity / schema-hash
  / thread-safety sections, mirroring `surface_archive.hpp`).
- Created `atx-vol/src/surface_db.cpp` — writer, reader, `encode_symbol_record`
  (internal), `decode_symbol_record` (public), `canonicalize_key`, `cmp_key`,
  `db_schema_hash`, `header_crc`.
- Created `atx-vol/tests/surface_db_test.cpp` — the brief's 3 tests
  (`RoundTrip_FullConfig_EveryFieldPreserved`, `Write_RejectsDuplicateAndInvalid`,
  `Open_RejectsCorruption`) plus the requested `RoundTrip_Empty` test (0
  symbols / 0 partitions is a valid manifest).
- Modified `atx-vol/CMakeLists.txt` — added `src/surface_db.cpp` to the
  `atx-vol` library sources (next to `src/surface_archive.cpp`).
- Modified `atx-vol/tests/CMakeLists.txt` — added `surface_db_test.cpp` to
  `atx-vol-tests` (next to `surface_archive_test.cpp`).

## Design notes / how the brief's requirements were satisfied

- **Record sizes**: hand-traced byte offsets for all three records before
  writing any code (7-bit-alignment bookkeeping for the 16/32/64-bit knob
  runs in `DbSymbolRecord`). All three landed exactly on 192/256/128 bytes
  with the brief's field order and reserved-tail sizes unchanged — no
  reordering was needed to satisfy the static_asserts, and the RED build
  confirmed this (header compiled clean on the first attempt with no
  static_assert failures).
- **`SymbolFitConfig` <-> `DbSymbolRecord` mapping**: verified every field
  against the real headers before coding (`CalibOpts` @ calib.hpp:133,
  `ConvexFitOpts` @ dense_slice.hpp:70, `CurveConfig` @ vol_curve.hpp:287,
  `AlOpts` @ american.hpp:46, `FitPreset` @ session.hpp:120, `CalendarRepair`
  @ surface_parity.hpp:87). The 13 `kDbSym*` flag bits exactly cover the 13
  boolean fields across `SymbolFitConfig`/`ConvexFitOpts`/`CalibOpts` — no
  bit left unassigned, no boolean field left unmapped.
- **Enum wire-range validation** (`symbol_record_enums_valid`, called once per
  record inside `DbManifest::open`): `preset<=3` (FitPreset: Fast/Accurate/
  Robust/Hft), `curve_kind<=4` (VolCurveKind through C8), `calendar_repair<=2`,
  `convex_loss`/`loss_kind<=1` (CalibLossKind: Mid/Interval),
  `essvi_rho_mode<=2`, `optimization_level<=4`, `residual_basis_kind<=5`,
  `anchor_kind<=2` — matches the brief's Step 4 bounds exactly, cross-checked
  against each enum's live definition.
- **Partition key rule**: `canonicalize_key` rejects length 0 or >32,
  non-`[A-Za-z0-9._-]` characters, and any `".."` substring (checked after
  uppercasing, so casing doesn't evade the check); valid keys are
  upper-cased. `find_partition` uses the shared `detail::canonicalize_symbol`
  (uppercase only, no charset check) since a non-matching lookup key simply
  fails the binary search — no separate validation needed on the read path.
- **CRC discipline**: `payload_crc32c` is computed over the contiguous
  `[symbols_offset, partitions_offset + partitions_bytes)` span (inter-section
  alignment padding is zero-initialized by `std::vector`'s value-init and
  never overwritten, so it's deterministic and CRC'd); `header_crc32c` is
  computed last, over the header bytes with only that field zeroed —
  `payload_crc32c` is already filled in by the time `header_crc` runs, so it
  is itself covered by the header CRC (this is what makes the `bad[100]`
  corruption test — which flips a byte inside the `payload_crc32c` field —
  trip the *header* CRC check, not the payload check; the test's inline
  comment says "header reserved" but the actual mechanism is "header CRC
  covers payload_crc32c value," and either way the observable contract —
  `ErrorCode::ParseError` — holds).
- **Empty manifest**: `write_db_manifest({}, {})` is valid (`symbol_count=0`,
  `partition_count=0`, `file_size=192`); `crc32c(ptr, 0)` degenerates cleanly
  to `0` (the update loop just doesn't execute). Covered by the added
  `RoundTrip_Empty` test.
- **`decode_symbol_record`**: the header declares it as a plain
  `SymbolFitConfig` return (no `Result`), so it performs no validation itself
  — it's the *exact inverse* of `encode_symbol_record`, trusted to be called
  only on records `DbManifest::open` already validated. The enum-range checks
  live in a separate, non-public `symbol_record_enums_valid` helper invoked
  once per record at `open` time, so `find_symbol` stays a cheap decode with
  no re-validation per the brief's "validates eagerly... so find_symbol
  stays cheap" requirement.

## TDD evidence

### RED

Command:
```
& .\scripts\atx-build.ps1 build atx-vol-tests
```
With the header + tests + a stub `surface_db.cpp` (includes only) wired into
both CMakeLists, the build compiled every translation unit successfully
(confirming the header's static_asserts on record sizes were already correct)
and failed at **link** time — the expected RED state for a compiled language:

```
lld-link: error: undefined symbol: ... atx::vol::write_db_manifest(...)
lld-link: error: undefined symbol: ... atx::vol::DbManifest::open(...)
lld-link: error: undefined symbol: ... atx::vol::DbManifest::find_symbol(...) const
lld-link: error: undefined symbol: ... atx::vol::DbManifest::find_partition(...) const
```
All four symbols the tests call were undefined, as expected before
implementation existed.

### GREEN

Command:
```
& .\scripts\atx-build.ps1 build atx-vol-tests
& .\scripts\atx-build.ps1 -Ctest -R SurfaceDbManifest
```
Output:
```
[1/4] Building CXX object atx-vol\CMakeFiles\atx-vol.dir\src\surface_db.cpp.obj
[2/4] Linking CXX static library lib\atx-vol.lib
[3/4] Linking CXX executable bin\atx-vol-tests.exe

    Start 447: SurfaceDbManifest.RoundTrip_FullConfig_EveryFieldPreserved
1/4 Test #447: SurfaceDbManifest.RoundTrip_FullConfig_EveryFieldPreserved ...   Passed    0.16 sec
    Start 448: SurfaceDbManifest.RoundTrip_Empty
2/4 Test #448: SurfaceDbManifest.RoundTrip_Empty ............................   Passed    0.20 sec
    Start 449: SurfaceDbManifest.Write_RejectsDuplicateAndInvalid
3/4 Test #449: SurfaceDbManifest.Write_RejectsDuplicateAndInvalid ...........   Passed    0.07 sec
    Start 450: SurfaceDbManifest.Open_RejectsCorruption
4/4 Test #450: SurfaceDbManifest.Open_RejectsCorruption .....................   Passed    0.06 sec

100% tests passed, 0 tests failed out of 4
```

(One intermediate build failure between RED and GREEN: `-Werror,-Wunused-function`
on an unused `const` overload of the local `buf_at` helper — removed, since
`DbManifest::open` takes its byte buffer by value/non-const and never needed
the const overload. Re-verified GREEN after the fix.)

## Test results

Both required ctest filters, final run:

```
& .\scripts\atx-build.ps1 -Ctest -R "SurfaceDbManifest|SurfaceArchive"
...
17/20 Test #447: SurfaceDbManifest.RoundTrip_FullConfig_EveryFieldPreserved ................   Passed    0.08 sec
18/20 Test #448: SurfaceDbManifest.RoundTrip_Empty .........................................   Passed    0.08 sec
19/20 Test #449: SurfaceDbManifest.Write_RejectsDuplicateAndInvalid ........................   Passed    0.08 sec
20/20 Test #450: SurfaceDbManifest.Open_RejectsCorruption ..................................   Passed    0.07 sec

100% tests passed, 0 tests failed out of 20
```

- `SurfaceDbManifest`: 4/4 passed (new).
- `SurfaceArchive`: 16/16 passed (regression, unchanged — confirms Task 1's
  shared `detail::` helpers were reused, not duplicated, and no cross-damage).

## Self-review findings

- Fixed one build-time issue myself before reporting: an unused `const`
  overload of `buf_at` tripped `-Wunused-function` under `/W4 /WX`; removed
  it (only the non-const overload is ever called, since `DbManifest::open`
  takes bytes by value).
- Removed an unused `<type_traits>` include from `surface_db.cpp` (the
  header's static_asserts already cover trivial-copyability/standard-layout;
  the `.cpp` itself doesn't add its own).
- Verified by hand-tracing byte offsets that no field reordering was needed
  to hit the pinned 192/256/128 sizes — the brief's declared field order
  already lands exactly on target for the natural (no `#pragma pack`)
  compiler layout used here.
- Verified the 13 `kDbSym*` bits map 1:1 onto the 13 boolean fields spanning
  `SymbolFitConfig`, `ConvexFitOpts`, and `CalibOpts` — no bit unused, no bool
  unmapped.
- Confirmed the `bad[100]` corruption-test byte lands inside the
  `payload_crc32c` field of the header (not literally "header reserved" as
  the brief's inline test comment says), but the resulting failure mode is
  still the header CRC check (since `payload_crc32c`'s value is itself
  covered by `header_crc32c`), so the test's asserted `ErrorCode::ParseError`
  holds regardless — no code change needed, just noting the discrepancy
  between the comment and the actual field for anyone debugging this later.
- No `TODO`/stub paths remain; `write_db_manifest`, `DbManifest::open`,
  `find_symbol`, `find_partition`, and `decode_symbol_record` are all fully
  implemented per the brief's Step 4 notes.

## Concerns

None blocking. Two documentation-level notes for Task 3+ implementers:

1. The public `write_db_manifest` doc comment (verbatim from the brief) says
   "InvalidArgument (empty/**oversized** symbol...)" but the implementation —
   matching the "symbols canonicalized via detail::canonicalize_symbol
   (identical to archive keys)" requirement — *truncates* an oversized symbol
   to 32 chars rather than rejecting it (identical to
   `write_surface_archive`'s behavior). Only an empty *canonical* symbol is
   rejected. This mirrors the archive exactly and no test in the brief
   exercises an oversized-but-truncatable symbol, so this was a deliberate
   choice to match "identical to archive keys" over the doc comment's literal
   wording.
2. `DbPartitionRecord::flags` exists on the wire (per the brief's exact
   layout) but no `kDbPartition*` bit constants are defined yet and the field
   is always written as 0 / never validated on read — consistent with the
   brief (only `kDbSym*` bits are specified for Task 2); a future task should
   define partition flag bits if/when needed.

## Review-fix addendum (post-review, same day)

Review verdict: Approved except one Important finding (no executable coverage
of the enum wire-range rejection path) + one Minor (doc-comment wording).
Both fixed:

1. **Important — enum-rejection coverage.** Added
   `SurfaceDbManifest.Open_RejectsOutOfRangeEnum` to
   `atx-vol/tests/surface_db_test.cpp`:
   - Writes a valid single-symbol manifest via `write_db_manifest`.
   - A `restamp_crcs` helper recomputes `payload_crc32c` over
     `[symbols_offset, end)` with `atx::vol::detail::crc32c` and then
     `header_crc32c` (field zeroed first, computed last), writing both via
     `offsetof(DbManifestHeader, ...)` — no magic offsets into the header.
   - Sanity leg: restamp with NO mutation still opens (proves the helper
     reproduces the writer's CRCs, so the rejections are the enum check, not
     a broken restamp).
   - Loops two enum bytes — `preset` (symbols_offset+36: after symbol[32] +
     symbol_len u16 + flags u16) and `curve_kind` (+37) — sets each to 0xFF,
     restamps, and asserts `DbManifest::open` returns
     `ErrorCode::ParseError`. This executes `symbol_record_enums_valid` and
     its ParseError branch (the error message naming the symbol).

2. **Minor — doc comment.** `write_db_manifest` comment in
   `atx-vol/include/atx/vol/surface_db.hpp` reworded: oversized symbols are
   truncated to `kSurfaceDbKeyMax` (matching the archive's canonical keys),
   not rejected; InvalidArgument is "(empty symbol, bad partition key)".

Verification:
```
& .\scripts\atx-build.ps1 build atx-vol-tests    # clean build
& .\scripts\atx-build.ps1 -Ctest -R SurfaceDbManifest
1/5 ... RoundTrip_FullConfig_EveryFieldPreserved   Passed
2/5 ... RoundTrip_Empty                            Passed
3/5 ... Write_RejectsDuplicateAndInvalid           Passed
4/5 ... Open_RejectsCorruption                     Passed
5/5 ... Open_RejectsOutOfRangeEnum                 Passed
100% tests passed, 0 tests failed out of 5

& .\scripts\atx-build.ps1 -Ctest -R SurfaceArchive
100% tests passed, 0 tests failed out of 16
```
