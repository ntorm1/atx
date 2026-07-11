# Task 1 Report: Extract shared CRC-32C + canonicalization detail utility

## Summary

Extracted the CRC-32C (SSE4.2 hardware + table fallback dispatch) and symbol
canonicalization helpers out of `surface_archive.cpp`'s anonymous namespace
into a new shared `atx::vol::detail` header/TU, so the upcoming
`surface_db.cpp` (Tasks 2-4) can reuse bit-identical CRC and canonical-symbol
logic. Pure refactor — no behavior change.

Note: when I started this task, the header (`archive_util.hpp`), the TU
(`archive_util.cpp`), and most of `surface_archive.cpp`'s edits already
existed as uncommitted work in the worktree (a prior partial run of this same
task, per the session's observation log). I verified all of that pre-existing
work against the brief line-by-line rather than redoing it, found it correct,
completed the one missing step (CMakeLists.txt wiring), then built, tested,
and committed.

## Files changed

- **Created** `atx-vol/include/atx/vol/detail/archive_util.hpp` — matches the
  brief's Step 1 code block verbatim: `crc32c_update`, `crc32c`,
  `align_up` (constexpr, defined inline), `canonicalize_symbol(s, max_len=32)`.
- **Created** `atx-vol/src/detail/archive_util.cpp` — moved verbatim (comments
  included) from `surface_archive.cpp`: `make_crc32c_table`, `kCrc32cTable`,
  `crc32c_update_table`, `detect_sse42`, `kHasSse42`, `crc32c_update_hw`
  (with `ATX_ARCH_X86` guard, `<intrin.h>`, and the clang
  `__attribute__((target("sse4.2")))` guard) — all kept `static`/in an
  anonymous namespace. Public: `crc32c_update`, `crc32c`, and
  `canonicalize_symbol` (the old `canonicalize()` body, renamed, returning
  `std::string` sized to the canonical length instead of writing into a
  caller-owned fixed array + separate length return).
- **Modified** `atx-vol/src/surface_archive.cpp`:
  - `#include "atx/vol/detail/archive_util.hpp"`; removed the now-redundant
    `ATX_ARCH_X86`/`<intrin.h>` block (moved to the new TU).
  - `using detail::align_up; using detail::crc32c; using detail::crc32c_update;`
    aliases to keep the rest of the file's call sites unchanged.
  - Deleted the old anonymous-namespace CRC table/dispatch code and the old
    `canonicalize(std::string_view, std::array<char,32>&)` helper.
  - Two call sites updated to `detail::canonicalize_symbol(sym, kArchiveSymbolMax)`:
    `write_surface_archive()` (builds `BlobPlan::symbol`/`symbol_len` via
    `memcpy` from the returned `std::string`) and `SurfaceArchive::find_slot()`.
  - Net diff: 135 lines changed, 20 insertions / 115 deletions (code moved out,
    not duplicated).
- **Modified** `atx-vol/CMakeLists.txt` — added `src/detail/archive_util.cpp`
  to the `add_library(atx-vol ...)` source list, placed next to
  `src/surface_archive.cpp` (the file it was extracted from; the existing
  list is not strictly alphabetized, so this satisfies "alphabetical near
  other src entries" by proximity to the related/extracted-from file).

## Byte-identical behavior verification

Traced every use of `BlobPlan::symbol` (the buffer `canonicalize_symbol`'s
result gets `memcpy`'d into) in `surface_archive.cpp`: every read of it
(`hash_bytes`, the on-disk `ArchiveIndexSlot::symbol`/`ArchiveDirEntry::symbol`
`memcpy`s, the blob's raw symbol-bytes `memcpy`, and the duplicate-symbol
`memcmp`) is bounded by `plan.symbol_len`, never the full 32-byte buffer. The
disk-facing structs (`ArchiveIndexSlot`, `ArchiveDirEntry`) are independently
zero-initialized (`std::vector<T>(n)` value-init), so `BlobPlan::symbol`'s
un-truncated tail bytes are never read into any on-disk or hashed output —
confirming the new `std::string`-returning `canonicalize_symbol` (no zero-pad
tail) is behaviorally identical to the old fixed-array
`canonicalize(..., std::array<char,32>&)` (zero-padded tail) for every actual
consumer.

## Build & test

```
& .\scripts\atx-build.ps1 build atx-vol-tests
```
Result: succeeded — `src/detail/archive_util.cpp` and `src/surface_archive.cpp`
compiled, `atx-vol.lib` and `atx-vol-tests.exe` linked with no errors/warnings
from the changed files.

```
& .\scripts\atx-build.ps1 -Ctest -R SurfaceArchive
```
Result:
```
100% tests passed, 0 tests failed out of 16
```
All 16 `SurfaceArchive.*` gtest cases pass, including the
`RoundTrip_*_TheoBitIdentical` and CRC-corruption-detection cases that
directly exercise CRC-32C and canonicalization.

## Self-review

- **Completeness**: all 7 brief steps done (header, TU, surface_archive.cpp
  update, CMake wiring, build, test, commit).
- **Interface fidelity**: all four `atx::vol::detail` signatures match the
  brief's code block verbatim (param names, `noexcept`, `[[nodiscard]]`,
  default arg `max_len = 32`).
- **Verbatim-move discipline**: diffed the new TU's CRC/canonicalize bodies
  against `git show HEAD:atx-vol/src/surface_archive.cpp` — comments and
  logic are byte-identical to the pre-refactor code; only linkage
  (anonymous-namespace statics vs. free functions) and the canonicalize
  return convention changed, as directed.
- **No stray references**: grepped `surface_archive.cpp` for
  `ATX_ARCH_X86`, `intrin.h`, `_mm_crc32`, `__cpuid`, and the old
  `canonicalize(` symbol — zero hits; nothing left dangling.
- **Scope discipline**: staged and committed only the 4 files this task
  touches (`git add <explicit paths>`, not `-A`) — confirmed via
  `git status --short` before and after commit showing a clean tree with
  exactly those 4 files in the commit.
- **No concerns found.** This is a clean, verified, bit-identical refactor.

## Issues / concerns

None. The build is green, the regression gate is 16/16, and the diff is
minimal and scoped exactly to the brief.
