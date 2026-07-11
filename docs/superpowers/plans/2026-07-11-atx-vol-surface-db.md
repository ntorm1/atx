# atx-vol `surface_db` Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** An on-disk database (`SurfaceDb`) layered on the ATXVSA v3 surface archive: a root directory holding (a) many partitioned archive files, each packing a huge number of binary-serialized `PricedSurface`s, and (b) a top-level binary **manifest** that stores the per-underlying-symbol fitting configuration the fitting pipeline loads, and that can be adjusted at run time (atomic rewrite + generation counter + cheap `refresh()`).

**Architecture:** `surface_archive.hpp` stays the single-file container; `surface_db` adds the database layer above it. On-disk shape:

```
<root>/
  manifest.atxdb            binary manifest (magic "ATXVDB01"): header + symbol
                            config records + partition index records, CRC-32C
                            protected, schema-hashed, generation-stamped
  partitions/
    <KEY>.atxvsa            one ATXVSA v3 surface archive per partition key
                            (e.g. one per trading date / snapshot label)
```

All manifest mutation goes through an atomic tmp+rename rewrite that bumps a
`generation` counter; readers hold an immutable `shared_ptr<const DbManifest>`
snapshot and call `refresh()` to pick up external writer updates cheaply.

**Tech Stack:** C++20, gtest, CMake (ninja preset, clang-cl), existing atx-vol vocabulary (`Result`/`Status`, `PricedSurface`, `SurfaceArchive`, `CurveConfig`, `CalibOpts`, `ConvexFitOpts`, `AlOpts`, `FitPreset`, `CalendarRepair`, `SessionInputs`). **No new third-party dependencies. No JSON/TOML — binary fixed-layout records, house style.**

## Global Constraints

- Little-endian LP64 hosts only (matches surface_archive.hpp:47-48). Header stamps `endian = 1`, `pointer_bits = 64`; reader rejects mismatch.
- Every on-disk record: trivially copyable + standard layout + fixed-width fields, size pinned by `static_assert` (house style, surface_archive.hpp:89-93).
- Errors via `Result<T>` / `Status` with `atx::core::ErrorCode` vocabulary: `InvalidArgument`, `AlreadyExists`, `NotFound`, `ParseError`, `IoError`, `OutOfRange`. Constructors: `Err(ErrorCode::X, "msg")`, `Ok(value)`.
- Manifest magic is exactly `"ATXVDB01"` (8 bytes, no NUL). Partition-blob files keep the archive's own `"ATXVSA03"` format untouched.
- Manifest file name is exactly `manifest.atxdb`; partition files are exactly `partitions/<CANONICAL_KEY>.atxvsa` under the db root.
- Symbols are canonicalized identically to the archive (ASCII upper-case, truncated to `kArchiveSymbolMax = 32`) so a manifest symbol key equals the archive lookup key.
- Partition keys: 1..32 chars, charset `[A-Za-z0-9._-]`, no `".."` substring, canonicalized to ASCII upper-case. Anything else → `InvalidArgument`.
- Atomic persistence: write `<path>.tmp` then `std::filesystem::rename` (pattern: surface_archive.cpp `write_surface_archive_file`, line ~617).
- Thread-safety contract: `SurfaceDb` const queries are safe from any thread; mutating calls are serialized internally by a mutex; cross-process coordination is single-writer / many-reader (document in the header).
- All new tests are gtest cases named `SurfaceDb*` inside `atx-vol/tests/surface_db_test.cpp`, registered in `atx-vol/tests/CMakeLists.txt`.
- The plan's test snippets write `Result` idioms as `has_value()` / `error().code()`; before writing test code, check how `surface_archive_test.cpp` interrogates `Result`/`Status` (value access, error access, void-Status success checks) and use THOSE exact idioms — the archive test file is the binding reference for the atx-core Result API, not this plan's pseudocode.
- **Curve-kind coverage (explicit requirement):** ConvexDense and LinearVariance surfaces must be FULLY supported through the surface_db binary path, proven by tests: the Task 4 partition round-trip covers ConvexDense (node arrays byte-equal) and LinearVariance (nodes bit-identical) alongside Essvi, and the Task 5 end-to-end test stores/serves a ConvexDense surface. The archive layer already round-trips both kinds (surface_archive_test.cpp `RoundTrip_ConvexDense_TheoBitIdentical_AndNodesByteEqual`, `RoundTrip_LinearVariance_TheoAndNodesBitIdentical`) — surface_db must not narrow that support anywhere (no kind switches that omit LinearVariance/ConvexDense).
- Build/test commands (run from worktree root; they self-enter the VS dev shell). NOTE: `pwsh` (PowerShell 7) is NOT installed — invoke the script directly from Windows PowerShell 5.1 as `& .\scripts\atx-build.ps1 ...`:
  - configure (once, already done by the controller): `& .\scripts\atx-build.ps1 configure`
  - build: `& .\scripts\atx-build.ps1 build atx-vol-tests`
  - run new tests: `& .\scripts\atx-build.ps1 -Ctest -R SurfaceDb`
  - run archive regression: `& .\scripts\atx-build.ps1 -Ctest -R SurfaceArchive`
- Commit after every green task step block, message style `feat(atx-vol): ...` / `refactor(atx-vol): ...` / `test(atx-vol): ...`.

---

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

### Task 2: `surface_db.hpp` on-disk records, `SymbolFitConfig`, manifest write/parse (in-memory)

The manifest binary format. Fixed-layout POD records mirroring the full per-symbol fit configuration (`CurveConfig` = kind + `ConvexFitOpts` + full `CalibOpts`, plus `AlOpts` override, plus session policy scalars), a writer that serializes to bytes, and an immutable parsed `DbManifest` with O(log n) symbol/partition lookup. Pure in-memory in this task — file IO is Task 3.

**Files:**
- Create: `atx-vol/include/atx/vol/surface_db.hpp`
- Create: `atx-vol/src/surface_db.cpp`
- Create: `atx-vol/tests/surface_db_test.cpp`
- Modify: `atx-vol/CMakeLists.txt` (add `src/surface_db.cpp`)
- Modify: `atx-vol/tests/CMakeLists.txt` (add `surface_db_test.cpp` to the `add_executable(atx-vol-tests ...)` list)

**Interfaces:**
- Consumes: Task 1 `atx::vol::detail::{crc32c, align_up, canonicalize_symbol}`; existing types `CurveConfig` (vol_curve.hpp:287), `ConvexFitOpts` (dense_slice.hpp:70), `CalibOpts` (calib.hpp:133), `AlOpts` (american.hpp:46), `FitPreset` (session.hpp:120), `CalendarRepair` (surface_parity.hpp:87), enums `VolCurveKind`, `CalibLossKind`, `CalibAnchorKind`, `EssviRhoMode`, `OptimizationLevel`, `ResidualBasisKind`.
- Produces (used by Tasks 3-5): everything below, exactly as declared.

- [ ] **Step 1: Write the header.** `atx-vol/include/atx/vol/surface_db.hpp` — start with a house-style top-of-file doc comment (see surface_archive.hpp:1-56 for tone: on-disk shape, integrity, thread-safety sections), then:

```cpp
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

#include "atx/vol/american.hpp"        // AlOpts
#include "atx/vol/session.hpp"         // FitPreset, SessionInputs
#include "atx/vol/surface_archive.hpp" // SurfaceArchive, SurfaceArchiveItem
#include "atx/vol/surface_parity.hpp"  // CalendarRepair
#include "atx/vol/types.hpp"
#include "atx/vol/vol_curve.hpp"       // CurveConfig, VolCurveKind

namespace atx::vol {

// ── On-wire constants ─────────────────────────────────────────────────────
inline constexpr std::uint16_t kSurfaceDbMajor = 1;
inline constexpr std::uint16_t kSurfaceDbMinor = 0;
inline constexpr std::size_t kSurfaceDbKeyMax = 32;   // partition-key chars
inline constexpr std::string_view kSurfaceDbManifestName = "manifest.atxdb";
inline constexpr std::string_view kSurfaceDbPartitionDir = "partitions";
inline constexpr std::string_view kSurfaceDbPartitionExt = ".atxvsa";
inline constexpr std::uint32_t kSurfaceDbSectionAlign = 64;

// DbSymbolRecord::flags bits.
inline constexpr std::uint16_t kDbSymEnabled = 1u << 0;
inline constexpr std::uint16_t kDbSymPinCurve = 1u << 1;
inline constexpr std::uint16_t kDbSymAlOverride = 1u << 2;
inline constexpr std::uint16_t kDbSymUseCorrectionCache = 1u << 3;
inline constexpr std::uint16_t kDbSymScoreParity = 1u << 4;
inline constexpr std::uint16_t kDbSymEnforceCalendarFloor = 1u << 5;
inline constexpr std::uint16_t kDbSymUseDeamCacheForFit = 1u << 6;
inline constexpr std::uint16_t kDbSymConvexBoundSlopeBelow = 1u << 7;
inline constexpr std::uint16_t kDbSymLeeBoundProject = 1u << 8;
inline constexpr std::uint16_t kDbSymMorozovStop = 1u << 9;
inline constexpr std::uint16_t kDbSymValidateNoArb = 1u << 10;
inline constexpr std::uint16_t kDbSymResidualDisable = 1u << 11;
inline constexpr std::uint16_t kDbSymEssviAsymmetricRho = 1u << 12;

// ── Per-symbol fitting configuration (public, in-memory) ──────────────────
//
// The manifest's unit of configuration: how the fitting pipeline should fit
// THIS underlying. `apply_symbol_config` (below) maps it onto SessionInputs:
// `preset` is applied first (apply_fit_preset — DeAm/cache/iv-tol policy),
// then every explicit field here overwrites the preset's choice, so the
// stored values are always the final word on the fields this struct carries.
struct SymbolFitConfig {
  bool enabled{true};                 // pipeline may skip disabled symbols
  FitPreset preset{FitPreset::Robust};
  bool pin_curve{false};              // false => preset/selector decides family
  CurveConfig curve{};                // used when pin_curve; parametric knobs
                                      // also mirror into SessionInputs::calib
  bool al_override{false};            // true => deam.al_opts = al
  AlOpts al{};
  double band_k{1.0};
  CalendarRepair calendar_repair{CalendarRepair::None};
  bool use_correction_cache{true};
  bool score_parity{true};
  bool enforce_calendar_floor{true};
  bool use_deam_cache_for_fit{false};
};

// ── On-disk records (POD, little-endian, fixed layout) ────────────────────

// Manifest file header, at offset 0. `header_crc32c` covers the header with
// that field zeroed; `payload_crc32c` covers the symbols ‖ partitions span.
struct DbManifestHeader {
  char magic[8]{};                    // "ATXVDB01", no NUL
  std::uint16_t major{};              // kSurfaceDbMajor
  std::uint16_t minor{};
  std::uint16_t header_size{};        // sizeof(DbManifestHeader)
  std::uint16_t endian{};             // 1 = little
  std::uint16_t pointer_bits{};       // 64
  std::uint16_t reserved0{};
  std::uint32_t flags{};
  std::uint64_t file_size{};
  std::int64_t created_ts_ns{};
  std::int64_t updated_ts_ns{};
  std::uint64_t generation{};         // ++ on every manifest rewrite
  std::uint64_t schema_hash{};        // sizeof-based layout fingerprint
  std::uint32_t symbol_count{};
  std::uint32_t partition_count{};
  std::uint64_t symbols_offset{};
  std::uint64_t partitions_offset{};
  std::uint32_t symbol_record_size{};    // sizeof(DbSymbolRecord)
  std::uint32_t partition_record_size{}; // sizeof(DbPartitionRecord)
  std::uint32_t header_crc32c{};
  std::uint32_t payload_crc32c{};
  std::uint8_t reserved[88]{};
};
static_assert(sizeof(DbManifestHeader) == 192, "DbManifestHeader layout drift");
static_assert(std::is_trivially_copyable_v<DbManifestHeader>);
static_assert(std::is_standard_layout_v<DbManifestHeader>);

// One symbol's full fit configuration, fixed-width. Sorted by canonical
// symbol in the file. Field-for-field mirror of SymbolFitConfig (bools in
// `flags`, enums as uint8, CurveConfig/CalibOpts/ConvexFitOpts/AlOpts fields
// laid out flat).
struct DbSymbolRecord {
  char symbol[32]{};                  // canonical, not NUL-terminated
  std::uint16_t symbol_len{};
  std::uint16_t flags{};              // kDbSym* bits
  // enums (uint8 wire width)
  std::uint8_t preset{};              // FitPreset
  std::uint8_t curve_kind{};          // VolCurveKind (meaningful when PinCurve)
  std::uint8_t calendar_repair{};     // CalendarRepair
  std::uint8_t convex_loss{};         // CalibLossKind (curve.convex.loss)
  std::uint8_t essvi_rho_mode{};      // EssviRhoMode
  std::uint8_t optimization_level{};  // OptimizationLevel
  std::uint8_t residual_basis_kind{}; // ResidualBasisKind
  std::uint8_t residual_n_basis_terms{};
  std::uint8_t loss_kind{};           // CalibLossKind (calib loss)
  std::uint8_t anchor_kind{};         // CalibAnchorKind
  std::uint8_t reserved1[2]{};
  // 16-bit knobs
  std::uint16_t max_outer_iter{};
  std::uint16_t max_inner_iter{};
  std::uint16_t max_iter_quick_mark{};
  std::uint16_t max_iter_trading{};
  std::uint16_t max_iter_risk{};
  std::uint16_t max_iter_reference{};
  std::uint16_t max_iter_cold_fast{};
  std::uint16_t al_n_collocation{};
  std::uint16_t al_n_quadrature{};
  std::uint16_t al_max_newton_iter{};
  // 32-bit knobs
  std::int32_t convex_node_cap{};
  std::int32_t convex_max_iter{};
  std::uint32_t max_obs_per_slice{};
  std::uint32_t n_butterfly_grid{};
  std::uint32_t min_obs_per_slice{};
  // 64-bit knobs
  double convex_lambda{};
  double tol_param{};
  double tol_residual{};
  double huber_k{};
  double min_vega_weight{};
  double max_spread_vol{};
  double max_weight{};
  double max_otm_shortcut_premium_spread_frac{};
  double prior_strength{};
  double essvi_fallback_rmse_threshold{};
  double wing_floor_alpha{};
  double morozov_tau{};
  double residual_ridge_factor{};
  double max_post_fit_sigma{};
  double max_spread_to_mid_pct{};
  double al_tol{};
  double band_k{};
  std::uint8_t reserved[32]{};
};
static_assert(sizeof(DbSymbolRecord) == 256, "DbSymbolRecord layout drift");
static_assert(std::is_trivially_copyable_v<DbSymbolRecord>);
static_assert(std::is_standard_layout_v<DbSymbolRecord>);

// One partition's index entry, sorted by canonical key. Integrity of the
// partition file itself is the archive's job (layered CRCs inside .atxvsa);
// the manifest tracks identity + bookkeeping.
struct DbPartitionRecord {
  char key[32]{};                     // canonical, not NUL-terminated
  std::uint16_t key_len{};
  std::uint16_t flags{};
  std::uint32_t surface_count{};
  std::uint64_t file_size{};          // partition file bytes at write time
  std::int64_t created_ts_ns{};
  std::uint64_t reserved0{};
  std::uint8_t reserved[64]{};
};
static_assert(sizeof(DbPartitionRecord) == 128, "DbPartitionRecord layout drift");
static_assert(std::is_trivially_copyable_v<DbPartitionRecord>);
static_assert(std::is_standard_layout_v<DbPartitionRecord>);

// ── Manifest writer inputs ────────────────────────────────────────────────

struct DbSymbolEntry {
  std::string_view symbol{};          // canonicalized before storage
  SymbolFitConfig config{};
};

struct DbPartitionInfo {
  std::string key{};                  // canonical
  std::uint32_t surface_count{};
  std::uint64_t file_size{};
  std::int64_t created_ts_ns{};
};

struct SurfaceDbManifestWriteOpts {
  std::uint64_t generation{1};
  std::int64_t created_ts_ns{0};      // 0 => system clock
  std::int64_t updated_ts_ns{0};      // 0 => system clock
  std::uint32_t flags{0};
};

// Serialize a manifest to bytes. Symbols/partitions are canonicalized and
// sorted internally. Errors: InvalidArgument (empty/oversized symbol, bad
// partition key); AlreadyExists (duplicate canonical symbol or key).
[[nodiscard]] Result<std::vector<std::byte>>
write_db_manifest(std::span<const DbSymbolEntry> symbols,
                  std::span<const DbPartitionInfo> partitions,
                  const SurfaceDbManifestWriteOpts& opts = {});

// ── Parsed manifest (immutable) ───────────────────────────────────────────

// Owns its bytes; validated on open (magic, version, endian, sizes, schema
// hash, header CRC, payload CRC, bounds, sort order). All queries const +
// thread-safe.
class DbManifest {
 public:
  [[nodiscard]] static Result<DbManifest> open(std::vector<std::byte> bytes);

  [[nodiscard]] const DbManifestHeader& header() const noexcept { return header_; }
  [[nodiscard]] std::uint64_t generation() const noexcept { return header_.generation; }
  [[nodiscard]] std::span<const DbSymbolRecord> symbols() const noexcept { return symbols_; }
  [[nodiscard]] std::span<const DbPartitionRecord> partitions() const noexcept {
    return partitions_;
  }

  // Case-insensitive (canonicalized) binary search. NotFound if absent.
  [[nodiscard]] Result<SymbolFitConfig> find_symbol(std::string_view symbol) const;
  [[nodiscard]] const DbPartitionRecord* find_partition(std::string_view key) const noexcept;

 private:
  DbManifest() = default;
  DbManifestHeader header_{};
  std::vector<DbSymbolRecord> symbols_{};      // sorted by canonical symbol
  std::vector<DbPartitionRecord> partitions_{}; // sorted by canonical key
};

// Decode one symbol record into the public config (exact inverse of the
// writer's encoding; used by DbManifest::find_symbol and tests).
[[nodiscard]] SymbolFitConfig decode_symbol_record(const DbSymbolRecord& rec);

}  // namespace atx::vol
```

(The `SurfaceDb` class and `apply_symbol_config` are added to this same header in Tasks 3-5.)

- [ ] **Step 2: Write failing tests.** `atx-vol/tests/surface_db_test.cpp` — follow surface_archive_test.cpp's include/pch style. Tests to write now:

```cpp
#include <gtest/gtest.h>

#include <vector>

#include "atx/vol/calib.hpp"
#include "atx/vol/surface_db.hpp"

namespace atx::vol {
namespace {

// A config where EVERY field differs from its default, so a round-trip that
// drops or transposes any field fails the equality sweep below.
SymbolFitConfig make_full_config() {
  SymbolFitConfig c;
  c.enabled = false;
  c.preset = FitPreset::Hft;
  c.pin_curve = true;
  c.curve.kind = VolCurveKind::ConvexDense;
  c.curve.convex.lambda = 7.5e-4;
  c.curve.convex.bound_slope_below = true;
  c.curve.convex.node_cap = 56;
  c.curve.convex.max_iter = 123;
  c.curve.convex.loss = CalibLossKind::Interval;
  auto& p = c.curve.parametric;
  p.max_outer_iter = 5; p.max_inner_iter = 13;
  p.tol_param = 2e-9; p.tol_residual = 3e-10;
  p.huber_k = 1.75;
  p.min_vega_weight = 2e-6; p.max_spread_vol = 0.07; p.max_weight = 500.0;
  p.max_obs_per_slice = 96; p.max_otm_shortcut_premium_spread_frac = 0.25;
  p.prior_strength = 0.5;
  p.essvi_rho_mode = EssviRhoMode::Shared;
  p.optimization_level = OptimizationLevel::Risk;
  p.essvi_fallback_rmse_threshold = 0.02; p.n_butterfly_grid = 128;
  p.max_iter_quick_mark = 9; p.max_iter_trading = 36; p.max_iter_risk = 101;
  p.max_iter_reference = 251; p.max_iter_cold_fast = 11;
  p.wing_floor_alpha = 0.05;
  p.lee_bound_project = false;
  p.morozov_stop = true; p.morozov_tau = 1.3;
  p.validate_no_arb = false;
  p.residual_disable = false;
  p.residual_basis_kind = ResidualBasisKind::C2Bspline;
  p.residual_n_basis_terms = 8; p.residual_ridge_factor = 2e-3;
  p.loss_kind = CalibLossKind::Interval;
  p.anchor_kind = CalibAnchorKind::Ask;
  p.essvi_asymmetric_rho = true;
  p.min_obs_per_slice = 6; p.max_post_fit_sigma = 3.0;
  p.max_spread_to_mid_pct = 0.4;
  c.al_override = true;
  c.al = AlOpts{9, 20, 6, 1e-9};
  c.band_k = 1.25;
  c.calendar_repair = CalendarRepair::Project;
  c.use_correction_cache = false;
  c.score_parity = false;
  c.enforce_calendar_floor = false;
  c.use_deam_cache_for_fit = true;
  return c;
}

void expect_config_eq(const SymbolFitConfig& a, const SymbolFitConfig& b) {
  EXPECT_EQ(a.enabled, b.enabled);
  EXPECT_EQ(a.preset, b.preset);
  EXPECT_EQ(a.pin_curve, b.pin_curve);
  EXPECT_EQ(a.curve.kind, b.curve.kind);
  EXPECT_EQ(a.curve.convex.lambda, b.curve.convex.lambda);
  EXPECT_EQ(a.curve.convex.bound_slope_below, b.curve.convex.bound_slope_below);
  EXPECT_EQ(a.curve.convex.node_cap, b.curve.convex.node_cap);
  EXPECT_EQ(a.curve.convex.max_iter, b.curve.convex.max_iter);
  EXPECT_EQ(a.curve.convex.loss, b.curve.convex.loss);
  const auto& x = a.curve.parametric; const auto& y = b.curve.parametric;
  EXPECT_EQ(x.max_outer_iter, y.max_outer_iter);
  EXPECT_EQ(x.max_inner_iter, y.max_inner_iter);
  EXPECT_EQ(x.tol_param, y.tol_param);
  EXPECT_EQ(x.tol_residual, y.tol_residual);
  EXPECT_EQ(x.huber_k, y.huber_k);
  EXPECT_EQ(x.min_vega_weight, y.min_vega_weight);
  EXPECT_EQ(x.max_spread_vol, y.max_spread_vol);
  EXPECT_EQ(x.max_weight, y.max_weight);
  EXPECT_EQ(x.max_obs_per_slice, y.max_obs_per_slice);
  EXPECT_EQ(x.max_otm_shortcut_premium_spread_frac, y.max_otm_shortcut_premium_spread_frac);
  EXPECT_EQ(x.prior_strength, y.prior_strength);
  EXPECT_EQ(x.essvi_rho_mode, y.essvi_rho_mode);
  EXPECT_EQ(x.optimization_level, y.optimization_level);
  EXPECT_EQ(x.essvi_fallback_rmse_threshold, y.essvi_fallback_rmse_threshold);
  EXPECT_EQ(x.n_butterfly_grid, y.n_butterfly_grid);
  EXPECT_EQ(x.max_iter_quick_mark, y.max_iter_quick_mark);
  EXPECT_EQ(x.max_iter_trading, y.max_iter_trading);
  EXPECT_EQ(x.max_iter_risk, y.max_iter_risk);
  EXPECT_EQ(x.max_iter_reference, y.max_iter_reference);
  EXPECT_EQ(x.max_iter_cold_fast, y.max_iter_cold_fast);
  EXPECT_EQ(x.wing_floor_alpha, y.wing_floor_alpha);
  EXPECT_EQ(x.lee_bound_project, y.lee_bound_project);
  EXPECT_EQ(x.morozov_stop, y.morozov_stop);
  EXPECT_EQ(x.morozov_tau, y.morozov_tau);
  EXPECT_EQ(x.validate_no_arb, y.validate_no_arb);
  EXPECT_EQ(x.residual_disable, y.residual_disable);
  EXPECT_EQ(x.residual_basis_kind, y.residual_basis_kind);
  EXPECT_EQ(x.residual_n_basis_terms, y.residual_n_basis_terms);
  EXPECT_EQ(x.residual_ridge_factor, y.residual_ridge_factor);
  EXPECT_EQ(x.loss_kind, y.loss_kind);
  EXPECT_EQ(x.anchor_kind, y.anchor_kind);
  EXPECT_EQ(x.essvi_asymmetric_rho, y.essvi_asymmetric_rho);
  EXPECT_EQ(x.min_obs_per_slice, y.min_obs_per_slice);
  EXPECT_EQ(x.max_post_fit_sigma, y.max_post_fit_sigma);
  EXPECT_EQ(x.max_spread_to_mid_pct, y.max_spread_to_mid_pct);
  EXPECT_EQ(a.al_override, b.al_override);
  EXPECT_EQ(a.al.n_collocation, b.al.n_collocation);
  EXPECT_EQ(a.al.n_quadrature, b.al.n_quadrature);
  EXPECT_EQ(a.al.max_newton_iter, b.al.max_newton_iter);
  EXPECT_EQ(a.al.tol, b.al.tol);
  EXPECT_EQ(a.band_k, b.band_k);
  EXPECT_EQ(a.calendar_repair, b.calendar_repair);
  EXPECT_EQ(a.use_correction_cache, b.use_correction_cache);
  EXPECT_EQ(a.score_parity, b.score_parity);
  EXPECT_EQ(a.enforce_calendar_floor, b.enforce_calendar_floor);
  EXPECT_EQ(a.use_deam_cache_for_fit, b.use_deam_cache_for_fit);
}

TEST(SurfaceDbManifest, RoundTrip_FullConfig_EveryFieldPreserved) {
  const auto cfg = make_full_config();
  const std::vector<DbSymbolEntry> syms{{"aapl", cfg}, {"SPY", SymbolFitConfig{}}};
  const std::vector<DbPartitionInfo> parts{
      {"2026-07-10", 123, 456789, 1720569600000000000LL}};
  auto bytes = write_db_manifest(syms, parts, {.generation = 7});
  ASSERT_TRUE(bytes.has_value());
  auto m = DbManifest::open(std::move(*bytes));
  ASSERT_TRUE(m.has_value());
  EXPECT_EQ(m->generation(), 7u);
  ASSERT_EQ(m->symbols().size(), 2u);
  ASSERT_EQ(m->partitions().size(), 1u);
  // canonical sort: AAPL < SPY
  auto got = m->find_symbol("AaPl");   // case-insensitive
  ASSERT_TRUE(got.has_value());
  expect_config_eq(*got, cfg);
  auto dflt = m->find_symbol("spy");
  ASSERT_TRUE(dflt.has_value());
  expect_config_eq(*dflt, SymbolFitConfig{});
  const auto* p = m->find_partition("2026-07-10");
  ASSERT_NE(p, nullptr);
  EXPECT_EQ(p->surface_count, 123u);
  EXPECT_EQ(p->file_size, 456789u);
  EXPECT_EQ(m->find_partition("2026-07-11"), nullptr);
  EXPECT_EQ(m->find_symbol("MSFT").error().code(), ErrorCode::NotFound);
}

TEST(SurfaceDbManifest, Write_RejectsDuplicateAndInvalid) {
  const std::vector<DbSymbolEntry> dup{{"AAPL", {}}, {"aapl", {}}};
  EXPECT_EQ(write_db_manifest(dup, {}).error().code(), ErrorCode::AlreadyExists);
  const std::vector<DbSymbolEntry> empty_sym{{"", {}}};
  EXPECT_EQ(write_db_manifest(empty_sym, {}).error().code(), ErrorCode::InvalidArgument);
  const std::vector<DbPartitionInfo> bad_key{{"bad/key", 0, 0, 0}};
  EXPECT_EQ(write_db_manifest({}, bad_key).error().code(), ErrorCode::InvalidArgument);
  const std::vector<DbPartitionInfo> dotdot{{"..", 0, 0, 0}};
  EXPECT_EQ(write_db_manifest({}, dotdot).error().code(), ErrorCode::InvalidArgument);
}

TEST(SurfaceDbManifest, Open_RejectsCorruption) {
  auto bytes = write_db_manifest({{DbSymbolEntry{"AAPL", {}}}}, {});
  ASSERT_TRUE(bytes.has_value());
  {
    auto bad = *bytes; bad[0] ^= std::byte{0xFF};  // magic
    EXPECT_EQ(DbManifest::open(std::move(bad)).error().code(), ErrorCode::ParseError);
  }
  {
    auto bad = *bytes; bad[100] ^= std::byte{0x01};  // header reserved => header CRC
    EXPECT_EQ(DbManifest::open(std::move(bad)).error().code(), ErrorCode::ParseError);
  }
  {
    auto bad = *bytes; bad[200] ^= std::byte{0x01};  // symbol record => payload CRC
    EXPECT_EQ(DbManifest::open(std::move(bad)).error().code(), ErrorCode::ParseError);
  }
  {
    auto bad = *bytes; bad.resize(bad.size() - 1);   // truncation
    EXPECT_EQ(DbManifest::open(std::move(bad)).error().code(), ErrorCode::ParseError);
  }
}

}  // namespace
}  // namespace atx::vol
```

Note: if `write_db_manifest({}, ...)` with zero symbols should be legal (an empty db is created before any symbol is configured), it is — empty symbol AND partition lists are valid; only malformed entries reject. Add `TEST(SurfaceDbManifest, RoundTrip_Empty)` asserting an empty manifest round-trips with counts 0.

- [ ] **Step 3: Wire the build and run tests to verify they fail.** Add `src/surface_db.cpp` (can be a stub containing only includes at first compile) and the test file to the two CMakeLists. Run: `& .\scripts\atx-build.ps1 build atx-vol-tests` — expect link/compile failure on missing symbols (that is the "failing" state for a compiled language; do not fake-pass).

- [ ] **Step 4: Implement `src/surface_db.cpp`.** Implementation notes (follow surface_archive.cpp structure):
  - Anonymous namespace: `kDbMagic = {'A','T','X','V','D','B','0','1'}`; `db_schema_hash()` — FNV-1a fold of `sizeof(DbManifestHeader)`, `sizeof(DbSymbolRecord)`, `sizeof(DbPartitionRecord)` + a salt constant (copy the fold pattern from surface_archive.cpp `schema_hash()`, line ~167, with its own salt string, e.g. `"atx-vol-surface-db-v1"`).
  - `encode_symbol_record(canon, cfg) -> DbSymbolRecord` and public `decode_symbol_record` — field-for-field, bools to/from `flags` bits, enums via `static_cast`. Reject (InvalidArgument) enum wire values outside their defined range on decode (`preset > 3`, `curve_kind > 4`, `calendar_repair > 2`, `essvi_rho_mode > 2`, `optimization_level > 4`, `residual_basis_kind > 5`, `convex_loss/loss_kind > 1`, `anchor_kind > 2`) — a manifest written by a future writer must not alias into wrong behavior. (DbManifest::open validates every record eagerly so `find_symbol` stays cheap; open returns ParseError naming the symbol on the first bad record.)
  - Partition key validation: `canonicalize_key(k)`: length 1..32, each char in `[A-Za-z0-9._-]`, no `".."` substring, upper-cased; else InvalidArgument.
  - Writer: canonicalize + dedup-check symbols and keys (AlreadyExists), sort both (memcmp on canonical), layout: header @0, symbols @ `align_up(192, 64) = 192`, partitions @ `align_up(symbols_end, 64)`; fill counts/offsets/sizes; `payload_crc32c = crc32c(symbols‖partitions span)` over the contiguous [symbols_offset, end) span; then `header_crc32c` over header bytes with that field zeroed (order: fill everything else first — same discipline as archive header, surface_archive.cpp ~586-612).
  - `DbManifest::open`: bounds-check → magic/major/endian/pointer_bits/header_size/record sizes/schema hash → header CRC → offsets/counts sanity (within file, no overlap, sorted strictly ascending) → payload CRC → memcpy records out → validate every record decodes (enum ranges, `symbol_len <= 32`, `key_len <= 32`). Every failure: `Err(ErrorCode::ParseError, ...)` with a specific message.
  - `find_symbol`: canonicalize input, `std::lower_bound` over sorted records comparing (len, bytes); decode on hit.
  - Timestamps: `opts.created_ts_ns == 0` → `std::chrono::system_clock` now (copy archive's stamp pattern).

- [ ] **Step 5: Build + run the new tests.** Run: `& .\scripts\atx-build.ps1 build atx-vol-tests` then `& .\scripts\atx-build.ps1 -Ctest -R SurfaceDbManifest` — expect ALL PASS.

- [ ] **Step 6: Run the archive regression to prove no cross-damage.** `& .\scripts\atx-build.ps1 -Ctest -R SurfaceArchive` — expect PASS.

- [ ] **Step 7: Commit.**

```bash
git add -A
git commit -m "feat(atx-vol): surface_db manifest format - records, writer, validated parser"
```

---

### Task 3: `SurfaceDb` class — create/open, atomic manifest persistence, symbol CRUD, `refresh()`

The database object: a root directory, a manifest held as an immutable snapshot, serialized mutations with atomic rewrite + generation bump, and cheap re-sync for the real-time-adjustment story.

**Files:**
- Modify: `atx-vol/include/atx/vol/surface_db.hpp` (add `SurfaceDb`)
- Modify: `atx-vol/src/surface_db.cpp`
- Modify: `atx-vol/tests/surface_db_test.cpp` (add tests)

**Interfaces:**
- Consumes: Task 2 manifest writer/parser.
- Produces (used by Tasks 4-5):

```cpp
// Append inside namespace atx::vol in surface_db.hpp:

struct SurfaceDbCreateOpts {
  std::int64_t created_ts_ns{0};      // 0 => system clock
};

// An opened surface database. Const queries are thread-safe (they read an
// immutable manifest snapshot swapped under a mutex); mutating calls are
// serialized internally. Cross-process: single writer, many readers; every
// mutation is an atomic manifest rewrite (tmp+rename) with generation++ so a
// reader process picks it up via refresh().
class SurfaceDb {
 public:
  // Create <root>/ (and partitions/) and write an empty manifest
  // (generation 1). Errors: AlreadyExists if a manifest already exists at
  // root; IoError on filesystem failure.
  [[nodiscard]] static Result<SurfaceDb> create(std::string_view root,
                                                const SurfaceDbCreateOpts& opts = {});

  // Open an existing database. Errors: NotFound (no manifest), ParseError,
  // IoError.
  [[nodiscard]] static Result<SurfaceDb> open(std::string_view root);

  [[nodiscard]] const std::string& root() const noexcept { return root_; }

  // ── Manifest snapshot queries (thread-safe) ──
  [[nodiscard]] std::shared_ptr<const DbManifest> manifest() const;
  [[nodiscard]] std::uint64_t generation() const;
  [[nodiscard]] std::vector<std::string> symbols() const;   // canonical, sorted
  [[nodiscard]] Result<SymbolFitConfig> symbol_config(std::string_view symbol) const;
  [[nodiscard]] std::vector<DbPartitionInfo> partitions() const;

  // ── Manifest mutation (serialized; atomic rewrite; generation++) ──
  [[nodiscard]] Status upsert_symbol(std::string_view symbol, const SymbolFitConfig& cfg);
  [[nodiscard]] Status remove_symbol(std::string_view symbol);  // NotFound if absent

  // Re-read the manifest from disk iff its generation advanced past the
  // in-memory snapshot (external writer). Ok and no-op when current.
  [[nodiscard]] Status refresh();

  // ── Partition IO (Task 4) ──
  [[nodiscard]] Status write_partition(std::string_view key,
                                       std::span<const SurfaceArchiveItem> items,
                                       const SurfaceArchiveWriteOpts& opts = {});
  [[nodiscard]] Result<SurfaceArchive> open_partition(std::string_view key) const;
  [[nodiscard]] Result<PricedSurface> load_surface(std::string_view key,
                                                   std::string_view symbol) const;
  [[nodiscard]] Status drop_partition(std::string_view key);

 private:
  SurfaceDb() = default;
  [[nodiscard]] Status persist_locked(std::vector<DbSymbolEntry> symbols,
                                      std::vector<DbPartitionInfo> partitions);
  [[nodiscard]] std::string manifest_path() const;
  [[nodiscard]] std::string partition_path(std::string_view canonical_key) const;

  std::string root_{};
  mutable std::mutex mu_{};                       // guards snapshot_ swap + writes
  std::shared_ptr<const DbManifest> snapshot_{};
};
```

(`<mutex>` joins the header includes. `SurfaceDb` is movable, non-copyable — `std::mutex` member means: implement move ctor/assign manually by locking the source, or hold `mu_` in a `std::unique_ptr<std::mutex>`; the unique_ptr route is simpler and fine here.)

**Steps:**

- [ ] **Step 1: Write failing tests** (append to surface_db_test.cpp). Use a per-test temp dir: `std::filesystem::temp_directory_path() / "atx_surface_db_test" / <unique test-name suffix>`; `std::filesystem::remove_all` it at test start AND end (self-cleaning even after a prior crashed run).

```cpp
TEST(SurfaceDb, CreateOpenUpsertReopen_ConfigPersists) {
  const auto root = test_root("create_open");     // helper: fresh temp dir
  auto db = SurfaceDb::create(root.string());
  ASSERT_TRUE(db.has_value());
  EXPECT_EQ(db->generation(), 1u);
  EXPECT_TRUE(db->symbols().empty());

  const auto cfg = make_full_config();
  ASSERT_TRUE(db->upsert_symbol("aapl", cfg).has_value());
  EXPECT_EQ(db->generation(), 2u);
  ASSERT_TRUE(db->upsert_symbol("SPY", SymbolFitConfig{}).has_value());
  EXPECT_EQ(db->generation(), 3u);

  auto db2 = SurfaceDb::open(root.string());      // fresh process simulation
  ASSERT_TRUE(db2.has_value());
  EXPECT_EQ(db2->generation(), 3u);
  EXPECT_EQ(db2->symbols(), (std::vector<std::string>{"AAPL", "SPY"}));
  auto got = db2->symbol_config("AAPL");
  ASSERT_TRUE(got.has_value());
  expect_config_eq(*got, cfg);

  ASSERT_TRUE(db2->remove_symbol("aapl").has_value());
  EXPECT_EQ(db2->symbol_config("AAPL").error().code(), ErrorCode::NotFound);
  EXPECT_EQ(db2->remove_symbol("AAPL").error().code(), ErrorCode::NotFound);
  std::filesystem::remove_all(root);
}

TEST(SurfaceDb, Create_RejectsExisting_Open_RejectsMissing) {
  const auto root = test_root("create_guard");
  ASSERT_TRUE(SurfaceDb::create(root.string()).has_value());
  EXPECT_EQ(SurfaceDb::create(root.string()).error().code(), ErrorCode::AlreadyExists);
  const auto missing = test_root("no_such_db");
  EXPECT_EQ(SurfaceDb::open(missing.string()).error().code(), ErrorCode::NotFound);
  std::filesystem::remove_all(root);
}

TEST(SurfaceDb, Refresh_SeesExternalWriterUpdate) {
  const auto root = test_root("refresh");
  auto writer = SurfaceDb::create(root.string());
  ASSERT_TRUE(writer.has_value());
  auto reader = SurfaceDb::open(root.string());
  ASSERT_TRUE(reader.has_value());
  EXPECT_EQ(reader->generation(), 1u);

  ASSERT_TRUE(writer->upsert_symbol("QQQ", SymbolFitConfig{}).has_value());
  // Reader still on its old snapshot until refresh:
  EXPECT_EQ(reader->generation(), 1u);
  ASSERT_TRUE(reader->refresh().has_value());
  EXPECT_EQ(reader->generation(), 2u);
  EXPECT_TRUE(reader->symbol_config("QQQ").has_value());
  // Idempotent when current:
  ASSERT_TRUE(reader->refresh().has_value());
  EXPECT_EQ(reader->generation(), 2u);
  std::filesystem::remove_all(root);
}

TEST(SurfaceDb, ConcurrentReaders_DuringUpserts_AreSafe) {
  const auto root = test_root("concurrent");
  auto db = SurfaceDb::create(root.string());
  ASSERT_TRUE(db.has_value());
  ASSERT_TRUE(db->upsert_symbol("SPY", SymbolFitConfig{}).has_value());
  std::atomic<bool> stop{false};
  std::vector<std::thread> readers;
  for (int t = 0; t < 4; ++t) {
    readers.emplace_back([&] {
      while (!stop.load(std::memory_order_relaxed)) {
        auto snap = db->manifest();
        auto cfg = db->symbol_config("SPY");
        ASSERT_TRUE(cfg.has_value());
        (void)snap;
      }
    });
  }
  for (int i = 0; i < 50; ++i) {
    SymbolFitConfig c; c.band_k = 1.0 + 0.01 * i;
    ASSERT_TRUE(db->upsert_symbol("SPY", c).has_value());
  }
  stop.store(true);
  for (auto& th : readers) th.join();
  auto final_cfg = db->symbol_config("SPY");
  ASSERT_TRUE(final_cfg.has_value());
  EXPECT_DOUBLE_EQ(final_cfg->band_k, 1.0 + 0.01 * 49);
  std::filesystem::remove_all(root);
}
```

`test_root(name)` helper: `std::filesystem::temp_directory_path() / ("atx_surface_db_" + std::string(name))`, `remove_all` then return; add `<filesystem>`, `<thread>`, `<atomic>` includes.

- [ ] **Step 2: Build; verify the new tests fail** (missing SurfaceDb symbols): `& .\scripts\atx-build.ps1 build atx-vol-tests` — expect failure mentioning `SurfaceDb`.

- [ ] **Step 3: Implement.** Notes:
  - `create`: `std::filesystem::create_directories(root / "partitions")`; if `root/manifest.atxdb` exists → AlreadyExists; write empty manifest via `write_db_manifest({}, {}, {.generation = 1, .created_ts_ns = opts.created_ts_ns})` and the atomic file write helper below; then delegate to `open`.
  - Atomic write helper (private, reuse for every manifest persist): serialize → `manifest_path() + ".tmp"` → `std::ofstream` binary write → `std::filesystem::rename` (copy the archive's `write_surface_archive_file` error handling, incl. tmp cleanup on failure).
  - `open`: read file fully (NotFound if `!exists`, IoError otherwise on stream failure) → `DbManifest::open` → store `snapshot_ = make_shared<const DbManifest>(std::move(m))`.
  - Mutations (`upsert_symbol`, `remove_symbol`, and Task 4's partition bookkeeping): lock `mu_`; rebuild `std::vector<DbSymbolEntry>` + `std::vector<DbPartitionInfo>` from the current snapshot (decode each record); apply the change (upsert = replace by canonical match or append; remove = erase or NotFound); call `persist_locked` which writes with `generation = old + 1`, `created_ts_ns` preserved from header, `updated_ts_ns = 0 (now)`, re-opens the bytes via `DbManifest::open` (cheap; guarantees the in-memory snapshot is exactly what a fresh reader parses), swaps `snapshot_`.
  - `refresh()`: read the manifest file's first `sizeof(DbManifestHeader)` bytes; if `generation <= snapshot->generation()` → Ok no-op; else full re-read + parse + swap under lock. (Read the header via `std::ifstream` with `read()`; a short read → ParseError.)
  - `manifest()` / queries: lock, copy `shared_ptr`, unlock, then operate on the snapshot.

- [ ] **Step 4: Build + run.** `& .\scripts\atx-build.ps1 build atx-vol-tests && & .\scripts\atx-build.ps1 -Ctest -R SurfaceDb` — expect ALL SurfaceDb* PASS.

- [ ] **Step 5: Commit.**

```bash
git add -A
git commit -m "feat(atx-vol): SurfaceDb - atomic manifest persistence, symbol CRUD, generation refresh"
```

---

### Task 4: Partition store — `write_partition` / `open_partition` / `load_surface` / `drop_partition`

Partitioned surface storage: each partition key maps to one ATXVSA archive file under `partitions/`; the manifest's partition index is the database's "table of contents".

**Files:**
- Modify: `atx-vol/src/surface_db.cpp` (implement the four methods declared in Task 3)
- Modify: `atx-vol/tests/surface_db_test.cpp`

**Interfaces:**
- Consumes: `write_surface_archive_file` (surface_archive.hpp:275), `SurfaceArchive::open_file` (surface_archive.hpp:292), `SurfaceArchive::map_symbol`, Task 3 `persist_locked`.
- Produces: the four `SurfaceDb` partition methods exactly as declared in Task 3.

**Semantics (bind the implementation to these):**
- `write_partition(key, items, opts)`: validate/canonicalize key (InvalidArgument on bad key; empty `items` is InvalidArgument — delegated to the archive writer which already rejects it). Write the archive to `partitions/<KEY>.atxvsa` via `write_surface_archive_file` (which is itself atomic tmp+rename). On success stat the file size, then update the manifest partition index under the writer lock: upsert `DbPartitionInfo{key, count(items), file_size, now}` — **overwriting an existing key is allowed** (a partition rewrite replaces it; generation bumps). Symbols in `items` do NOT need to be in the manifest symbol table (partitions store surfaces; the symbol table stores fit config — orthogonal namespaces; document this in the header comment).
- `open_partition(key)`: canonicalize; look up the CURRENT snapshot; NotFound if the key is not in the index; `SurfaceArchive::open_file` the path (its ParseError/IoError propagate).
- `load_surface(key, symbol)`: `open_partition` then `map_symbol(symbol)` — one symbol, no full-archive scan (archive guarantees O(1) probe + single blob parse).
- `drop_partition(key)`: NotFound if absent from index; remove from the manifest FIRST (persist, generation++), then `std::filesystem::remove` the file (an orphaned file after a crash between the two steps is harmless garbage — never the reverse order which would leave a dangling index entry; put this reasoning in a comment).

**Steps:**

- [ ] **Step 1: Write failing tests.** Synthesize `PricedSurface`s exactly the way surface_archive_test.cpp does — copy its `make_essvi(uid, n_slices)`, `make_convex(uid, n_slices, n_nodes)`, and `make_linear(...)` helpers (top of that file) into surface_db_test.cpp's anonymous namespace (or a tiny shared local helper; keep it self-contained). **ConvexDense and LinearVariance are first-class citizens of this task's coverage — the mixed-kind test below is mandatory, not optional.**

```cpp
TEST(SurfaceDbPartition, WriteOpenLoad_TheoBitIdentical) {
  const auto root = test_root("part_roundtrip");
  auto db = SurfaceDb::create(root.string());
  ASSERT_TRUE(db.has_value());

  const auto s1 = make_essvi(/*uid=*/1, /*n_slices=*/3);
  const auto s2 = make_essvi(/*uid=*/2, /*n_slices=*/2);
  const std::vector<SurfaceArchiveItem> items{{"AAPL", &s1}, {"MSFT", &s2}};
  ASSERT_TRUE(db->write_partition("2026-07-10", items).has_value());
  EXPECT_EQ(db->partitions().size(), 1u);
  EXPECT_EQ(db->partitions()[0].key, "2026-07-10");
  EXPECT_EQ(db->partitions()[0].surface_count, 2u);

  auto loaded = db->load_surface("2026-07-10", "aapl");
  ASSERT_TRUE(loaded.has_value());
  // Bit-identical theo assertion: copy VERBATIM the probe + `bits_equal`
  // comparison block from surface_archive_test.cpp's
  // RoundTrip_Essvi_TheoBitIdentical (compare `loaded` against `s1` exactly
  // the way that test compares its mapped surface against its source —
  // same probe points, same bits_equal oracle, no tolerance comparisons).

  // reopen db cold and load through the fresh instance:
  auto db2 = SurfaceDb::open(root.string());
  ASSERT_TRUE(db2.has_value());
  auto arch = db2->open_partition("2026-07-10");
  ASSERT_TRUE(arch.has_value());
  EXPECT_EQ(arch->count(), 2u);
  ASSERT_TRUE(arch->map_symbol("MSFT").has_value());
  std::filesystem::remove_all(root);
}
```

(The implementer copies the exact theo-probe + `bits_equal` assertions from `RoundTrip_Essvi_TheoBitIdentical` — the plan intentionally defers to that file as the bit-identity oracle rather than restating it; it is the binding pattern.)

```cpp
TEST(SurfaceDbPartition, MixedKinds_ConvexDenseAndLinearVariance_RoundTripBitIdentical) {
  // Explicit requirement: ConvexDense + LinearVariance surfaces fully
  // supported through the db's binary path. One partition holding all three
  // kinds; each loads back with the SAME assertions the archive suite uses:
  //  - ConvexDense: theo bit-identical AND node arrays byte-equal (copy the
  //    assertion block from RoundTrip_ConvexDense_TheoBitIdentical_
  //    AndNodesByteEqual in surface_archive_test.cpp);
  //  - LinearVariance: theo + nodes bit-identical (copy from RoundTrip_
  //    LinearVariance_TheoAndNodesBitIdentical);
  //  - Essvi: theo bit-identical.
  const auto root = test_root("part_mixed_kinds");
  auto db = SurfaceDb::create(root.string());
  ASSERT_TRUE(db.has_value());
  const auto sc = make_convex(/*uid=*/11, /*n_slices=*/2, /*n_nodes=*/40);
  const auto sl = make_linear(/*uid=*/12, /*n_slices=*/2);
  const auto se = make_essvi(/*uid=*/13, /*n_slices=*/2);
  const std::vector<SurfaceArchiveItem> items{
      {"CVX", &sc}, {"LIN", &sl}, {"ESS", &se}};
  ASSERT_TRUE(db->write_partition("2026-07-10", items).has_value());
  auto db2 = SurfaceDb::open(root.string());
  ASSERT_TRUE(db2.has_value());
  auto c = db2->load_surface("2026-07-10", "CVX");
  ASSERT_TRUE(c.has_value());
  // <ConvexDense assertions here — theo bit-identity + byte-equal nodes>
  auto l = db2->load_surface("2026-07-10", "LIN");
  ASSERT_TRUE(l.has_value());
  // <LinearVariance assertions here — theo + nodes bit-identical>
  auto e = db2->load_surface("2026-07-10", "ESS");
  ASSERT_TRUE(e.has_value());
  // <Essvi theo bit-identity assertion here>
  std::filesystem::remove_all(root);
}

TEST(SurfaceDbPartition, RewriteReplaces_DropRemoves) {
  const auto root = test_root("part_lifecycle");
  auto db = SurfaceDb::create(root.string());
  ASSERT_TRUE(db.has_value());
  const auto s1 = make_essvi(1, 2);
  const auto s2 = make_essvi(2, 2);
  const std::vector<SurfaceArchiveItem> one{{"AAPL", &s1}};
  const std::vector<SurfaceArchiveItem> two{{"AAPL", &s1}, {"MSFT", &s2}};
  ASSERT_TRUE(db->write_partition("2026-07-10", one).has_value());
  ASSERT_TRUE(db->write_partition("2026-07-10", two).has_value());  // rewrite
  EXPECT_EQ(db->partitions().size(), 1u);
  EXPECT_EQ(db->partitions()[0].surface_count, 2u);

  ASSERT_TRUE(db->drop_partition("2026-07-10").has_value());
  EXPECT_TRUE(db->partitions().empty());
  EXPECT_EQ(db->open_partition("2026-07-10").error().code(), ErrorCode::NotFound);
  EXPECT_EQ(db->drop_partition("2026-07-10").error().code(), ErrorCode::NotFound);
  // file physically gone:
  EXPECT_FALSE(std::filesystem::exists(
      std::filesystem::path(root) / "partitions" / "2026-07-10.atxvsa"));
  std::filesystem::remove_all(root);
}

TEST(SurfaceDbPartition, ManySymbols_ManyPartitions_SingleSurfaceLookup) {
  const auto root = test_root("part_scale");
  auto db = SurfaceDb::create(root.string());
  ASSERT_TRUE(db.has_value());
  std::vector<PricedSurface> pool;
  pool.reserve(64);
  for (int i = 0; i < 64; ++i) pool.push_back(make_essvi(100 + i, 2));
  for (int p = 0; p < 4; ++p) {
    std::vector<SurfaceArchiveItem> items;
    for (int i = 0; i < 64; ++i) {
      items.push_back({std::string("SYM") + std::to_string(i), &pool[i]});
    }
    // NOTE: symbol strings must outlive the call — build a std::vector<std::string>
    // holder first, then string_views into it.
    ASSERT_TRUE(db->write_partition("2026-07-1" + std::to_string(p), items).has_value());
  }
  EXPECT_EQ(db->partitions().size(), 4u);
  auto s = db->load_surface("2026-07-12", "SYM42");
  ASSERT_TRUE(s.has_value());
  EXPECT_EQ(db->load_surface("2026-07-12", "NOPE").error().code(), ErrorCode::NotFound);
  EXPECT_EQ(db->load_surface("2026-99-99", "SYM1").error().code(), ErrorCode::NotFound);
  std::filesystem::remove_all(root);
}

TEST(SurfaceDbPartition, BadKey_Rejected) {
  const auto root = test_root("part_badkey");
  auto db = SurfaceDb::create(root.string());
  ASSERT_TRUE(db.has_value());
  const auto s1 = make_essvi(1, 2);
  const std::vector<SurfaceArchiveItem> items{{"AAPL", &s1}};
  for (const char* bad : {"", "a/b", "a\\b", "..", "x..y",
                          "0123456789012345678901234567890123"}) {
    EXPECT_EQ(db->write_partition(bad, items).error().code(),
              ErrorCode::InvalidArgument) << bad;
  }
  std::filesystem::remove_all(root);
}
```

- [ ] **Step 2: Build; verify new tests fail.** (Methods stubbed as `Err(ErrorCode::NotImplemented, ...)` from Task 3 or missing → compile/behavioral failure.)

- [ ] **Step 3: Implement** per the Semantics block above.

- [ ] **Step 4: Build + run all db tests + archive regression.** `& .\scripts\atx-build.ps1 build atx-vol-tests` then `-Ctest -R "SurfaceDb|SurfaceArchive"` — expect ALL PASS.

- [ ] **Step 5: Commit.**

```bash
git add -A
git commit -m "feat(atx-vol): SurfaceDb partition store over ATXVSA archives"
```

---

### Task 5: Fitting-pipeline binding — `apply_symbol_config` + end-to-end integration test

The manifest config must load into the fitting pipeline: one function maps `SymbolFitConfig` onto `SessionInputs` (the core builder input, session.hpp:69) with pinned semantics, plus a preset-capture helper, plus an end-to-end test through a real db on disk.

**Files:**
- Modify: `atx-vol/include/atx/vol/surface_db.hpp`
- Modify: `atx-vol/src/surface_db.cpp`
- Modify: `atx-vol/tests/surface_db_test.cpp`

**Interfaces:**
- Consumes: `apply_fit_preset(SessionInputs&, FitPreset)` (session.hpp:141), Task 3 `SurfaceDb`.
- Produces:

```cpp
// Map `cfg` onto the fit-policy fields of `in`, leaving the market snapshot
// (S, r, expiry rates, cash_divs, now_ts_ns) untouched. Order: apply_fit_preset
// (cfg.preset) first — it sets the DeAm/cache/inversion policy — then every
// explicit SymbolFitConfig field overwrites the preset's choice:
//   in.curve = cfg.curve (when pin_curve; otherwise the preset's curve stands),
//   in.calib = cfg.curve.parametric (when pin_curve),
//   in.deam.al_opts = cfg.al (when al_override),
//   in.band_k / in.calendar_repair / in.use_correction_cache / in.score_parity
//   / in.enforce_calendar_floor / in.use_deam_cache_for_fit = cfg.<same>.
void apply_symbol_config(const SymbolFitConfig& cfg, SessionInputs& in);

// Capture `preset`'s effective policy into a SymbolFitConfig whose explicit
// fields equal what apply_fit_preset(in, preset) would produce — the identity
// starting point for per-symbol tuning (adjust one knob, store, done).
[[nodiscard]] SymbolFitConfig symbol_config_from_preset(FitPreset preset);
```

**Steps:**

- [ ] **Step 1: Write failing tests.**

```cpp
TEST(SurfaceDbApply, PinnedConfig_OverridesPreset) {
  auto cfg = make_full_config();          // pin_curve=true, al_override=true, Hft
  SessionInputs in;
  in.S = 100.0; in.r = 0.04; in.now_ts_ns = 42;   // market snapshot
  apply_symbol_config(cfg, in);
  // market snapshot untouched:
  EXPECT_DOUBLE_EQ(in.S, 100.0);
  EXPECT_DOUBLE_EQ(in.r, 0.04);
  EXPECT_EQ(in.now_ts_ns, 42);
  // explicit fields won over the Hft preset:
  EXPECT_EQ(in.curve.kind, VolCurveKind::ConvexDense);
  EXPECT_EQ(in.curve.convex.node_cap, 56);
  EXPECT_EQ(in.calib.optimization_level, OptimizationLevel::Risk);
  EXPECT_DOUBLE_EQ(in.band_k, 1.25);
  EXPECT_EQ(in.calendar_repair, CalendarRepair::Project);
  EXPECT_FALSE(in.use_correction_cache);
  EXPECT_FALSE(in.score_parity);
  EXPECT_FALSE(in.enforce_calendar_floor);
  EXPECT_TRUE(in.use_deam_cache_for_fit);
  ASSERT_TRUE(in.deam.al_opts.has_value());
  EXPECT_EQ(in.deam.al_opts->n_collocation, 9);
  EXPECT_DOUBLE_EQ(in.deam.al_opts->tol, 1e-9);
}

TEST(SurfaceDbApply, UnpinnedConfig_PresetCurveStands) {
  SymbolFitConfig cfg = symbol_config_from_preset(FitPreset::Robust);
  cfg.pin_curve = false;
  SessionInputs via_apply;
  apply_symbol_config(cfg, via_apply);
  SessionInputs via_preset;
  apply_fit_preset(via_preset, FitPreset::Robust);
  // Identity: a config captured from a preset and applied unpinned reproduces
  // apply_fit_preset exactly on the fields SymbolFitConfig carries.
  EXPECT_EQ(via_apply.curve.kind, via_preset.curve.kind);
  EXPECT_DOUBLE_EQ(via_apply.band_k, via_preset.band_k);
  EXPECT_EQ(via_apply.calendar_repair, via_preset.calendar_repair);
  EXPECT_EQ(via_apply.use_correction_cache, via_preset.use_correction_cache);
  EXPECT_EQ(via_apply.score_parity, via_preset.score_parity);
  EXPECT_EQ(via_apply.enforce_calendar_floor, via_preset.enforce_calendar_floor);
  EXPECT_EQ(via_apply.use_deam_cache_for_fit, via_preset.use_deam_cache_for_fit);
  EXPECT_EQ(via_apply.deam.al_opts.has_value(), via_preset.deam.al_opts.has_value());
  if (via_preset.deam.al_opts.has_value()) {
    EXPECT_EQ(via_apply.deam.al_opts->n_collocation, via_preset.deam.al_opts->n_collocation);
    EXPECT_DOUBLE_EQ(via_apply.deam.al_opts->tol, via_preset.deam.al_opts->tol);
  }
}

TEST(SurfaceDbEndToEnd, ConfigureStoreReloadServe) {
  const auto root = test_root("e2e");
  // Session 1: operator configures the universe + pipeline stores fits.
  {
    auto db = SurfaceDb::create(root.string());
    ASSERT_TRUE(db.has_value());
    auto spy = symbol_config_from_preset(FitPreset::Robust);
    spy.pin_curve = true;
    spy.curve.kind = VolCurveKind::ConvexDense;
    spy.curve.convex.node_cap = 48;
    ASSERT_TRUE(db->upsert_symbol("SPY", spy).has_value());
    auto aapl = symbol_config_from_preset(FitPreset::Fast);
    aapl.enabled = false;
    ASSERT_TRUE(db->upsert_symbol("AAPL", aapl).has_value());

    // SPY stored as ConvexDense (matches its pinned config; exercises the
    // variable-length-node kind end-to-end), AAPL as Essvi.
    const auto s1 = make_convex(1, 3, 40);
    const auto s2 = make_essvi(2, 3);
    const std::vector<SurfaceArchiveItem> items{{"SPY", &s1}, {"AAPL", &s2}};
    ASSERT_TRUE(db->write_partition("2026-07-11", items).has_value());
  }
  // Session 2 (fresh open — the fitting pipeline at startup):
  auto db = SurfaceDb::open(root.string());
  ASSERT_TRUE(db.has_value());
  auto spy_cfg = db->symbol_config("SPY");
  ASSERT_TRUE(spy_cfg.has_value());
  EXPECT_TRUE(spy_cfg->enabled);
  SessionInputs in;
  in.S = 500.0; in.r = 0.05;
  apply_symbol_config(*spy_cfg, in);
  EXPECT_EQ(in.curve.kind, VolCurveKind::ConvexDense);
  EXPECT_EQ(in.curve.convex.node_cap, 48);
  auto aapl_cfg = db->symbol_config("AAPL");
  ASSERT_TRUE(aapl_cfg.has_value());
  EXPECT_FALSE(aapl_cfg->enabled);      // pipeline skips disabled names
  // Real-time adjustment: another handle flips node_cap; pipeline refreshes.
  {
    auto ops = SurfaceDb::open(root.string());
    ASSERT_TRUE(ops.has_value());
    auto c = *ops->symbol_config("SPY");
    c.curve.convex.node_cap = 64;
    ASSERT_TRUE(ops->upsert_symbol("SPY", c).has_value());
  }
  ASSERT_TRUE(db->refresh().has_value());
  EXPECT_EQ(db->symbol_config("SPY")->curve.convex.node_cap, 64);
  // Stored surfaces still serve:
  ASSERT_TRUE(db->load_surface("2026-07-11", "SPY").has_value());
  std::filesystem::remove_all(root);
}
```

- [ ] **Step 2: Build; verify failure** (missing `apply_symbol_config` / `symbol_config_from_preset`).

- [ ] **Step 3: Implement.**
  - `apply_symbol_config`: exactly the doc-comment order: `apply_fit_preset(in, cfg.preset);` then `if (cfg.pin_curve) { in.curve = cfg.curve; in.calib = cfg.curve.parametric; }` then `if (cfg.al_override) in.deam.al_opts = cfg.al;` then the six scalars/flags unconditionally.
  - `symbol_config_from_preset`: `SessionInputs tmp; apply_fit_preset(tmp, preset);` capture into a `SymbolFitConfig`: `preset` = preset, `pin_curve = false`, `curve = tmp.curve`, `al_override = tmp.deam.al_opts.has_value()`, `al = tmp.deam.al_opts.value_or(AlOpts{})`, `band_k = tmp.band_k`, `calendar_repair = tmp.calendar_repair`, four bool flags from `tmp`. (Read session.cpp's `apply_fit_preset` first to confirm which fields it touches; the capture must mirror it faithfully.)

- [ ] **Step 4: Build + full module gate.** `& .\scripts\atx-build.ps1 build atx-vol-tests` then `& .\scripts\atx-build.ps1 -Ctest -R "SurfaceDb|SurfaceArchive"` — ALL PASS. Then the whole-module sanity run: `& .\scripts\atx-build.ps1 -Ctest -L atx_vol` — expect no regressions (same pass count as the pre-task baseline).

- [ ] **Step 5: Commit.**

```bash
git add -A
git commit -m "feat(atx-vol): apply_symbol_config pipeline binding + surface_db end-to-end test"
```

---

## Final Verification (controller, after all tasks)

- [ ] Full atx-vol suite: `& .\scripts\atx-build.ps1 -Ctest -L atx_vol` — zero failures.
- [ ] Dispatch the final whole-branch code review (superpowers:requesting-code-review) with the merge-base diff package.
- [ ] superpowers:finishing-a-development-branch.
