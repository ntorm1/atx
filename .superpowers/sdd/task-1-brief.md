### Task 1: Extract shared CRC-32C + canonicalization detail utility

The CRC-32C (HW SSE4.2 + table fallback) and symbol canonicalization currently live in an anonymous namespace inside `surface_archive.cpp` (lines ~64-205). `surface_db.cpp` needs bit-identical CRC and identical canonical symbols. Extract to a shared detail header/TU; behavior must be bit-identical (existing archive tests are the gate).

**Files:**
- Create: `atx-vol/include/atx/vol/detail/archive_util.hpp`
- Create: `atx-vol/src/detail/archive_util.cpp`
- Modify: `atx-vol/src/surface_archive.cpp` (delete moved code, call detail fns)
- Modify: `atx-vol/CMakeLists.txt` (add `src/detail/archive_util.cpp` to the `add_library(atx-vol ...)` source list, alphabetical near other src entries)

**Interfaces:**
- Consumes: nothing new.
- Produces (used by Tasks 2-4):
  - `namespace atx::vol::detail`
  - `std::uint32_t crc32c_update(std::uint32_t crc, const std::byte* p, std::size_t n) noexcept;` (running, un-finalized state)
  - `std::uint32_t crc32c(const std::byte* p, std::size_t n) noexcept;` (one-shot, init/final XOR applied)
  - `constexpr std::uint64_t align_up(std::uint64_t v, std::uint64_t a) noexcept;`
  - `std::string canonicalize_symbol(std::string_view s);` (ASCII upper-case, truncate to 32 — extract the archive's `canonicalize()` verbatim)

**Steps:**

- [ ] **Step 1: Create the detail header** `atx-vol/include/atx/vol/detail/archive_util.hpp`:

```cpp
#pragma once

// Shared low-level helpers for the ATX binary container formats
// (surface_archive.hpp ATXVSA, surface_db.hpp ATXVDB): hardware-accelerated
// CRC-32C, alignment, and canonical symbol normalization. Moved verbatim from
// surface_archive.cpp so both formats share ONE bit-identical implementation.
//
// Thread-safety: all functions are pure / touch no shared mutable state.

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace atx::vol::detail {

// Continue a CRC-32C (Castagnoli) over [p, p+n). `crc` is the running
// (un-finalized) state. Runtime-dispatched: SSE4.2 `_mm_crc32` when available,
// table-driven fallback otherwise — bit-identical outputs.
[[nodiscard]] std::uint32_t crc32c_update(std::uint32_t crc, const std::byte* p,
                                          std::size_t n) noexcept;

// One-shot CRC-32C with the standard init/final XOR applied.
[[nodiscard]] std::uint32_t crc32c(const std::byte* p, std::size_t n) noexcept;

[[nodiscard]] constexpr std::uint64_t align_up(std::uint64_t v, std::uint64_t a) noexcept {
  return (v + (a - 1u)) & ~(a - 1u);
}

// Canonical symbol: ASCII upper-cased, truncated to `max_len` bytes. Extracted
// verbatim from surface_archive.cpp — archive lookup keys and surface_db
// manifest keys MUST agree.
[[nodiscard]] std::string canonicalize_symbol(std::string_view s, std::size_t max_len = 32);

}  // namespace atx::vol::detail
```

- [ ] **Step 2: Move the implementations.** Create `atx-vol/src/detail/archive_util.cpp`, moving from `surface_archive.cpp` **verbatim** (keep comments): `make_crc32c_table`, `kCrc32cTable`, `crc32c_update_table`, `detect_sse42`, `kHasSse42`, `crc32c_update_hw` (with the `ATX_ARCH_X86` guards, `__attribute__((target("sse4.2")))` clang guard, and the `__cpuid`/`_mm_crc32_*` includes those functions need — check the top of surface_archive.cpp for which headers gate on `ATX_ARCH_X86`), plus public `crc32c_update`, `crc32c`, and the body of the archive's `canonicalize()` renamed `canonicalize_symbol(s, max_len)`. Keep the moved statics in an anonymous namespace inside the new TU; only the four interface functions are non-static.

- [ ] **Step 3: Update `surface_archive.cpp`** to `#include "atx/vol/detail/archive_util.hpp"`, delete the moved code, and forward: keep thin local aliases if that minimizes the diff (`using detail::crc32c; using detail::crc32c_update; using detail::align_up;`) and change `canonicalize(...)` call sites to `detail::canonicalize_symbol(sym, kArchiveSymbolMax)`. Do not change any hashing, layout, or CRC semantics.

- [ ] **Step 4: Wire the build.** In `atx-vol/CMakeLists.txt` add `src/detail/archive_util.cpp` to the `add_library(atx-vol ...)` list.

- [ ] **Step 5: Build.** Run: `& .\scripts\atx-build.ps1 build atx-vol-tests` — expect success.

- [ ] **Step 6: Run the archive regression gate.** Run: `& .\scripts\atx-build.ps1 -Ctest -R SurfaceArchive` — expect **all 16 SurfaceArchive tests PASS** (bit-identical CRC/canonicalization proof).

- [ ] **Step 7: Commit.**

```bash
git add -A
git commit -m "refactor(atx-vol): extract CRC-32C + symbol canonicalization into detail/archive_util"
```

---

