# Backtest Framework — Wave A: RunArchive Result Store Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace the ~20 loose result files of the dispersion backtest pipeline with a single custom binary `RunArchive` container (`run.atxrun`) — mmap columnar, CRC-layered, schema-hashed, atomically written — and cut every result consumer (C++ writers, `verify`, Python `parity.py`/`io.py`) over to it.

**Architecture:** Port the proven ATXVSA2 skeleton (`include/atx/vol/surface_archive.hpp:450-579`, `src/surface_archive.cpp`) to a results container: contiguous region, byte-offset section directory = manifest, columnar SoA sections, layered CRC-32C, a `sizeof`+column-table `schema_hash`, atomic `.tmp`+rename write, `ArchiveContentIdentity`. A single `constexpr` column registry drives the C++ writer, the `schema_hash`, and a generated Python descriptor, killing the current 4-place schema duplication. Hard cutover — no legacy-TSV dual-write; a `runarchive dump` command is the only escape hatch.

**Tech Stack:** C++20 (MSVC, Release preset, AVX2), `atx::vol` library; reuses `detail::crc32c`/`crc32c_update`/`align_up` (`detail/archive_util.hpp`) and the `tsdb::Mapping` mmap seam via `open_borrowed`; Python 3 + numpy (binding-free reader); CMake; pytest for the Python reader; the atx-vol C++ test target `atx-vol-tests`.

## Global Constraints

- Work directly on local `main`, in place. Explicit-path `git add` only — **never** `git add -A/-u/.` (the tree carries unrelated uncommitted work).
- ONE build at a time. Release preset only. Shared deps at `C:\atx-cache\deps`. `parquet.dll` needs `C:\atx\build-rel\bin` on PATH to run examples/tests.
- Do NOT modify golden fixtures. Do NOT touch `C:\atx-data` run dirs (controller owns them; use the 3-session fixture copy recipe, never `scratchpad\paired`).
- On-disk structs are an ABI: every struct is `static_assert`'d on `sizeof` AND `offsetof`; layout drift is a compile error and changes `schema_hash`. Host is little-endian LP64 only.
- All binary output little-endian; all text output `\n` line endings, `%.17g` for round-trip doubles.
- Windows/PowerShell: `$ErrorActionPreference='Continue'` (native stderr wraps in ErrorRecords); use `git commit -F <file>` or bash heredoc for multi-line messages (here-strings mangle).
- Commit trailer on every commit: `Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>`.

---

## File Structure

**New library headers/sources** (add each `.cpp` to `atx-vol/CMakeLists.txt` source list):
- `atx-vol/include/atx/vol/run_archive_schema.hpp` — `constexpr` column registry + `schema_hash` fold. Header-only. One responsibility: the single source of truth for every section's columns.
- `atx-vol/include/atx/vol/run_archive.hpp` + `src/run_archive.cpp` — on-disk structs, `write_run_archive*`, `RunArchive` reader, section encoders/decoders, `RunDir` handle.
- `atx-vol/include/atx/vol/run_diagnostics.hpp` + `src/run_diagnostics.cpp` — `PhaseTimer` + diagnostics section (lifted from the example).

**New tests:**
- `atx-vol/tests/run_archive_test.cpp` — registered in `atx-vol/tests/CMakeLists.txt` `add_executable(atx-vol-tests ...)`.
- `atx-vol/python/tests/test_runarchive.py` — pytest.

**New Python:**
- `atx-vol/python/src/atxvol/report/runarchive.py` — binding-free mmap reader + generated schema descriptor `_schema.py`.

**Modified:**
- `atx-vol/examples/spy_dispersion_backtest.cpp` — result writes → RunArchive section writes; add `runarchive dump` subcommand; delete the lifted `PhaseTimer`/`write_diagnostics`.
- `atx-vol/python/src/atxvol/report/io.py`, `parity.py` — read from RunArchive.
- `atx-vol/CMakeLists.txt`, `atx-vol/tests/CMakeLists.txt` — source/test registration + Python-descriptor codegen step.

**Deliberately NOT in the archive (stay text):** `run_spec.tsv`, `universe_schedule.tsv` (authored inputs; embed only their hash in `meta`), `definitions.tsv` (696 MB input), `occ_ess/*` + `occ_ess_inventory.tsv` + `methodology_map.tsv` (compliance/authored). `surface_manifest.tsv` stays the corpus manifest input.

---

## Task 1: Column registry + schema_hash (`run_archive_schema.hpp`)

**Files:**
- Create: `atx-vol/include/atx/vol/run_archive_schema.hpp`
- Test: add cases to `atx-vol/tests/run_archive_test.cpp` (created here)
- Modify: `atx-vol/tests/CMakeLists.txt` (register `run_archive_test.cpp`)

**Interfaces:**
- Produces:
  - `enum class RaDType : std::uint8_t { F64=0, I64=1, U32=2, U8Enum=3, DictStr=4 };`
  - `enum class RaSectionKind : std::uint8_t { ScalarKV=0, TimeSeries=1, SubTable=2 };`
  - `struct RaColumn { std::string_view name; RaDType dtype; std::string_view unit; };`
  - `struct RaSection { std::string_view name; RaSectionKind kind; std::span<const RaColumn> columns; };`
  - `std::span<const RaSection> ra_sections();` — the full registry.
  - `constexpr std::uint64_t ra_schema_hash();` — fold of every section name+kind and every column {name,dtype,unit}, salted with `kRaSchemaSalt`.
  - `inline constexpr std::uint16_t kRaMajor = 1, kRaMinor = 0;`
  - `inline constexpr char kRaMagic[8] = {'A','T','X','R','U','N','0','1'};`

**The `backtest` column set is fixed and load-bearing** (matches `tearsheet.cpp:190-216` and `BacktestResult`), in EXACTLY this order — `date`(DictStr), `ts_ns`(I64), then 25 F64: `pnl_total, pnl_delta, pnl_gamma, pnl_vega, pnl_vanna, pnl_volga, pnl_theta, pnl_rho, pnl_charm, pnl_unexplained, pnl_settlement, pnl_shares, financing, cost, nav, cash, gross_delta, gross_gamma, gross_vega, gross_theta, turnover_notional, turnover_vega, n_open_lots, n_unpriced_lots, n_unpriced_greeks`. Per-signal columns are appended dynamically at write time (not in the static registry). `projected_cold` and `projected_nodiv` reuse the `backtest` column set.

**Other sections' columns are defined by their existing writers** — enumerate each into the registry by reading the writer, do not invent columns:
- `reconciliation` ← `write_listed_reconciliation_file` (`src/listed_dispersion_reconciliation.cpp`).
- `trade_schedule` / `projected_schedule` ← `write_listed_dispersion_schedule_file` (`src/listed_dispersion_schedule.cpp`).
- `contract_marks` ← `write_listed_contract_marks_file` (`src/listed_dispersion_reconciliation.cpp`).
- `mark_divergence` ← the example's `write_mark_divergence_replay` header.
- `diagnostics` ← `subcommand, phase, wall_ms, count`.
- `meta` ← ScalarKV (resolved spec echo, window, roll-level scalars, input hashes, counts).

- [ ] **Step 1: Write the failing test** — schema hash is deterministic + backtest schema is exactly 27 columns with `nav` at column index 16.

```cpp
// in atx-vol/tests/run_archive_test.cpp
#include "atx/vol/run_archive_schema.hpp"
#include <cassert>
using namespace atx::vol;

static void test_schema_registry() {
  // backtest section present, 27 cols (date + ts_ns + 25 doubles), nav at index 16.
  const RaSection* bt = nullptr;
  for (const RaSection& s : ra_sections())
    if (s.name == "backtest") bt = &s;
  assert(bt && bt->kind == RaSectionKind::TimeSeries);
  assert(bt->columns.size() == 27);
  assert(bt->columns[0].name == "date"  && bt->columns[0].dtype == RaDType::DictStr);
  assert(bt->columns[1].name == "ts_ns" && bt->columns[1].dtype == RaDType::I64);
  assert(bt->columns[16].name == "nav"  && bt->columns[16].dtype == RaDType::F64);
  assert(bt->columns[26].name == "n_unpriced_greeks");
  // schema hash is stable + nonzero.
  static_assert(ra_schema_hash() != 0);
  assert(ra_schema_hash() == ra_schema_hash());
}
```

- [ ] **Step 2: Run test to verify it fails** — Run (from a configured Release build dir): `cmake --build C:\atx\build-rel --target atx-vol-tests` → expected FAIL: `run_archive_schema.hpp` not found.

- [ ] **Step 3: Implement `run_archive_schema.hpp`** — the enums, `RaColumn`/`RaSection`, `static constexpr RaColumn kBacktestCols[] = {...}` in the pinned order, the other section arrays enumerated from their writers, `ra_sections()` returning a `constexpr` array, and `ra_schema_hash()` as a `constexpr` FNV-1a-style fold over `(name,kind)` per section and `(name,dtype,unit)` per column plus `kRaSchemaSalt`. No I/O, no allocation — pure `constexpr`.

- [ ] **Step 4: Run test to verify it passes** — `cmake --build C:\atx\build-rel --target atx-vol-tests && C:\atx\build-rel\bin\atx-vol-tests.exe` → PASS.

- [ ] **Step 5: Commit**

```bash
git add atx-vol/include/atx/vol/run_archive_schema.hpp atx-vol/tests/run_archive_test.cpp atx-vol/tests/CMakeLists.txt
git commit -F <(printf '%s\n' "feat(vol): RunArchive column registry + schema_hash single-source" "" "Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>")
```

---

## Task 2: On-disk structs (ABI, pinned by static_assert)

**Files:**
- Modify: `atx-vol/include/atx/vol/run_archive.hpp` (create)
- Test: `atx-vol/tests/run_archive_test.cpp`

**Interfaces:**
- Produces `RunArchiveHeader` (256 B), `RaSectionDescriptor`, `RaSectionHeader`, `RaColumnDescriptor`. All `trivially_copyable` + `standard_layout`, `sizeof`+`offsetof` pinned. Mirror `ArchiveV2Header` (`surface_archive.hpp:450-475`) field discipline (descending alignment, naturally aligned).

Header fields (256 B): `char magic[8]` (`ATXRUN01`), `u64 file_size, created_ts_ns, schema_hash, writer_version_hash, run_identity_hash, section_dir_offset, data_offset`, `u32 section_count, header_crc32c` (own field zeroed), `u32 metadata_crc32c` (over section directory), `u32 flags`, `u16 major, minor, header_size, endian, pointer_bits, reserved_u16`, `u8 reserved[...]` pad to 256.

- [ ] **Step 1: Write the failing test**

```cpp
static void test_struct_abi() {
  static_assert(sizeof(RunArchiveHeader) == 256);
  static_assert(std::is_trivially_copyable_v<RunArchiveHeader>);
  static_assert(std::is_standard_layout_v<RunArchiveHeader>);
  static_assert(offsetof(RunArchiveHeader, file_size) == 8);
  static_assert(offsetof(RunArchiveHeader, schema_hash) == 24);
  static_assert(std::is_trivially_copyable_v<RaSectionDescriptor>);
  static_assert(std::is_trivially_copyable_v<RaSectionHeader>);
  static_assert(std::is_trivially_copyable_v<RaColumnDescriptor>);
}
```

- [ ] **Step 2: Run to verify it fails** — build target; expected FAIL: `run_archive.hpp` missing / static_assert.
- [ ] **Step 3: Implement the structs** in `run_archive.hpp` with exact fixed-width fields, `static_assert` on `sizeof` and each load-bearing `offsetof`, mirroring the ATXVSA2 field ordering. `RaSectionDescriptor` carries `{kind, char name[32], u64 section_offset, section_size, n_rows; u32 n_cols, col_desc_offset, payload_crc32c}` (payload_crc32c is a COPY of the section's own CRC so `metadata_crc32c` covers it — the F6 trick at `surface_archive.hpp:519-527`).
- [ ] **Step 4: Run to verify it passes** — build; PASS.
- [ ] **Step 5: Commit** (`git add atx-vol/include/atx/vol/run_archive.hpp atx-vol/tests/run_archive_test.cpp`).

---

## Task 3: Writer (`write_run_archive` + atomic file write)

**Files:** Modify `atx-vol/include/atx/vol/run_archive.hpp`, `src/run_archive.cpp` (create); `atx-vol/CMakeLists.txt` (register `src/run_archive.cpp`); Test `run_archive_test.cpp`.

**Interfaces:**
- Consumes Task 1 registry, Task 2 structs, `detail::crc32c`/`align_up`.
- Produces:
  - `struct RaColumnData { RaDType dtype; /* one of: */ std::span<const double>; std::span<const std::int64_t>; std::span<const std::uint32_t>; std::span<const std::uint8_t>; /* dict-str: */ std::span<const std::uint32_t> codes; std::span<const std::string> dict; /* enum labels */ std::span<const std::string> labels; };` (use a small tagged variant, mirror the encoder needs).
  - `struct RaSectionData { std::string name; RaSectionKind kind; std::uint64_t n_rows; std::vector<std::pair<std::string, RaColumnData>> columns; };`
  - `Result<std::vector<std::byte>> write_run_archive(std::span<const RaSectionData> sections, std::int64_t created_ts_ns, std::uint64_t run_identity_hash);`
  - `Status write_run_archive_file(std::string_view path, std::span<const RaSectionData> sections, std::int64_t created_ts_ns, std::uint64_t run_identity_hash);` — build buffer, write `path + ".tmp"`, `fsync`-equivalent flush, atomic rename (mirror `listed_dispersion_reconciliation.cpp` pending→rename).

Layout: header → section directory (sorted by name) → 64-B-aligned sections; each section = `RaSectionHeader` + `RaColumnDescriptor[]` + 8-B-aligned typed column arrays; dict-str → u32 code column + string table at aux offset; u8-enum → u8 code column + label table. `header_crc32c` over header (own field zeroed); `metadata_crc32c` over the section directory; per-section `payload_crc32c` over the section with its own field zeroed, copied into the descriptor. Stamp `schema_hash = ra_schema_hash()`.

- [ ] **Step 1: Write the failing test** — write a 2-row `backtest` section + a `meta` scalar section, assert bytes nonempty, magic present, `header_crc32c` verifies.

```cpp
static void test_writer_roundtrip_bytes() {
  std::vector<double> nav = {100.0, 101.5};
  std::vector<std::int64_t> ts = {1, 2};
  std::vector<std::uint32_t> date_codes = {0, 1};
  std::vector<std::string> date_dict = {"2026-07-11", "2026-07-12"};
  RaSectionData bt = make_backtest_section_from_columns(/* per Task 5 */);
  auto bytes = write_run_archive(std::span(&bt, 1), /*created*/123, /*rid*/0xABC);
  assert(bytes && bytes->size() > sizeof(RunArchiveHeader));
  const auto* h = reinterpret_cast<const RunArchiveHeader*>(bytes->data());
  assert(std::memcmp(h->magic, "ATXRUN01", 8) == 0);
  assert(h->schema_hash == ra_schema_hash());
}
```

- [ ] **Step 2: Run to verify it fails** — build; FAIL: `write_run_archive` undefined.
- [ ] **Step 3: Implement** the two-pass writer (size then fill) in `src/run_archive.cpp`, mirroring `write_surface_archive_v2` in `src/surface_archive.cpp`. Register `src/run_archive.cpp` in `atx-vol/CMakeLists.txt`.
- [ ] **Step 4: Run to verify it passes** — build + run; PASS.
- [ ] **Step 5: Commit.**

---

## Task 4: Reader (`RunArchive` class: open / section / validate) + mmap

**Files:** Modify `run_archive.hpp`, `src/run_archive.cpp`; Test `run_archive_test.cpp`.

**Interfaces:**
- Produces class `RunArchive` with: `static Result<RunArchive> open(std::vector<std::byte>)`, `open_file(path)`, `open_mapped(path)` (via `tsdb::Mapping` + `open_borrowed`); `count()`, `header()`, `directory()`, `identity()` (`ArchiveContentIdentity`); `Result<RaSectionView> section(std::string_view name)` returning zero-copy typed column views over the mapped bytes; `Status validate_section(name)` (lazy per-section CRC), `validate_all()`.
- `RaSectionView` exposes `f64_col(name) -> std::span<const double>`, `i64_col`, `u32_col`, `u8enum_col` + its label table, `dict_col` + its string table, `n_rows()`, `n_cols()`.

`open` validates: magic, major/minor, endian==1, pointer_bits==64, `schema_hash == ra_schema_hash()`, header CRC, metadata CRC, directory bounds. Per-section CRC is NOT checked on open.

- [ ] **Step 1: Write the failing tests** — round-trip equality, schema-hash drift rejection, CRC tamper rejection.

```cpp
static void test_reader_roundtrip() {
  auto bytes = /* Task 3 writer output for a known backtest section */;
  auto ar = RunArchive::open(std::move(*bytes));
  assert(ar);
  auto sec = ar->section("backtest");
  assert(sec);
  auto nav = sec->f64_col("nav");
  assert(nav.size() == 2 && nav[0] == 100.0 && nav[1] == 101.5);
  auto dates = sec->dict_col("date");
  assert(dates.at(0) == "2026-07-11");
}
static void test_reader_rejects_schema_drift() {
  auto bytes = /* writer output */;
  auto* h = reinterpret_cast<RunArchiveHeader*>(bytes->data());
  h->schema_hash ^= 1;                 // simulate a column rename
  // header_crc recompute would be needed for a "valid-but-drifted" file; here
  // assert open() rejects on schema_hash mismatch OR header CRC mismatch.
  assert(!RunArchive::open(std::move(*bytes)));
}
static void test_reader_rejects_crc_tamper() {
  auto bytes = /* writer output */;
  (*bytes)[bytes->size() - 8] = std::byte{0xFF};   // flip a payload byte
  auto ar = RunArchive::open(std::move(*bytes));    // open still ok (lazy)
  assert(ar);
  assert(!ar->validate_section("backtest"));        // lazy CRC catches it
}
```

- [ ] **Step 2: Run to verify they fail** — build; FAIL.
- [ ] **Step 3: Implement** `RunArchive::open_impl` (mirror `SurfaceArchiveV2::open_impl`), `section`, the typed views, and `open_mapped` through the existing mmap seam.
- [ ] **Step 4: Run to verify they pass** — build + run; PASS.
- [ ] **Step 5: Commit.**

---

## Task 5: Section encoders (BacktestResult / reconciliation / schedule / contract_marks / meta → RaSectionData)

**Files:** Modify `run_archive.hpp`, `src/run_archive.cpp`; Test `run_archive_test.cpp`.

**Interfaces (Produces):**
- `RaSectionData encode_backtest_section(std::string name, const BacktestResult& r);` — emits the 27 registry columns + one F64 column per `r.signals` entry; `date` as dict-str; enforces value-equality with `append_backtest_series_tsv` (`tearsheet.cpp:189`).
- `RaSectionData encode_reconciliation_section(const ListedDispersionReconciliation&);`
- `RaSectionData encode_schedule_section(std::string name, const ListedDispersionSchedule&);` — rolls×legs SubTable; enum/side as u8+labels; `source_fingerprint`/`surface_fingerprint` as dict-str.
- `RaSectionData encode_contract_marks_section(const ListedDispersionReconciliation&);`
- `RaSectionData encode_meta_section(const RunSpec&, /* roll scalars, input hashes, counts */);`

- [ ] **Step 1: Write the failing test** — encode a small `BacktestResult`, write→open, assert every one of the 25 double columns round-trips bit-exactly and matches the source vectors; a signal column appears.

```cpp
static void test_encode_backtest_valueexact() {
  BacktestResult r; r.resize(2);
  r.date = {"2026-07-11","2026-07-12"}; r.ts_ns = {10,20};
  r.nav = {100.0, 101.5}; r.pnl_vega = {1.25, -0.5};
  r.signals.push_back({"atm_iv", {0.20, 0.21}});
  auto sec = encode_backtest_section("backtest", r);
  auto bytes = write_run_archive(std::span(&sec,1), 0, 0);
  auto ar = RunArchive::open(std::move(*bytes));
  auto v = ar->section("backtest");
  assert(v->f64_col("nav")[1] == 101.5);
  assert(v->f64_col("pnl_vega")[0] == 1.25);
  assert(v->f64_col("atm_iv")[1] == 0.21);   // dynamic signal column
}
```

- [ ] **Step 2: Run to verify it fails** — build; FAIL.
- [ ] **Step 3: Implement** the encoders. Read each source writer to pin the exact columns/types before encoding (registry Task 1 is authoritative).
- [ ] **Step 4: Run to verify it passes** — build + run; PASS.
- [ ] **Step 5: Commit.**

---

## Task 6: `run_diagnostics` module (lift PhaseTimer + diagnostics section)

**Files:** Create `atx-vol/include/atx/vol/run_diagnostics.hpp`, `src/run_diagnostics.cpp`; register in `atx-vol/CMakeLists.txt`; Test `run_archive_test.cpp` (or a small `run_diagnostics_test.cpp`).

**Interfaces (Produces):** `class PhaseTimer` (verbatim from `spy_dispersion_backtest.cpp:105-146` — steady_clock, named phases, `now()`, `add(phase, start, count)`), and `RaSectionData encode_diagnostics_section(const PhaseTimer&, std::string_view subcommand, std::uint64_t total_count);`.

- [ ] **Step 1: Write the failing test** — a `PhaseTimer` with two phases produces a `diagnostics` section whose `phase` dict column contains both names and `total` row.
- [ ] **Step 2: Run to verify it fails.**
- [ ] **Step 3: Implement** — lift `PhaseTimer` unchanged; add the section encoder.
- [ ] **Step 4: Run to verify it passes.**
- [ ] **Step 5: Commit.**

---

## Task 7: `RunDir` handle (typed run-dir API + verify)

**Files:** Modify `run_archive.hpp`, `src/run_archive.cpp`; Test `run_archive_test.cpp`.

**Interfaces (Produces):** `class RunDir` owning a run-directory path; `Result<RunSpec> spec()`, `Result<Clock> clock()`, `Result<ListedDispersionSchedule> schedule()` — reads the inputs that stay text + the archive sections that don't; `Status write_run_archive(std::span<const RaSectionData>)` (writes `<dir>/run.atxrun` atomically, computing `run_identity_hash` from run_spec bytes + input fingerprints); `Result<RunArchive> archive()` (open_mapped); `Status verify(const /*methodology*/ ...)` — envelope/existence/count/core-mode gates (ex `spy_dispersion_backtest.cpp:552-587`, now over archive sections + the retained text inputs).

- [ ] **Step 1: Write the failing test** — build a temp run dir with a written `run.atxrun`, `RunDir::verify()` passes; corrupt a section CRC → `verify()` fails.
- [ ] **Step 2: Run to verify it fails.**
- [ ] **Step 3: Implement** `RunDir`.
- [ ] **Step 4: Run to verify it passes.**
- [ ] **Step 5: Commit.**

---

## Task 8: Python reader `runarchive.py` + generated schema descriptor

**Files:** Create `atx-vol/python/src/atxvol/report/runarchive.py`, `.../report/_schema.py` (generated); Test `atx-vol/python/tests/test_runarchive.py`; Modify `atx-vol/CMakeLists.txt` (codegen step) — or a standalone `tools/gen_runarchive_schema.py` run at build.

**Interfaces (Produces):**
- `class RunArchive` (pure-Python): `open(path)` mmaps, `struct.unpack`s the 256-B header, asserts magic/endian/pointer_bits, **recomputes `schema_hash` from `_schema.py` and compares — raises `ValueError` at open on mismatch**; `.section(name)` returns a `Section` with `.f64(col)`/`.i64(col)`/`.dict(col)`/`.u8enum(col)` as zero-copy `numpy.frombuffer` views (no per-cell `float()`).
- `read_backtest_section(archive, name="backtest") -> tuple[BacktestResult-like, dict, dict]` — the drop-in shim replacing `io.read_backtest_tsv`, returning `(result, meta, extra)` with the same shape `parity.py` consumes today.
- `_schema.py` is generated from the C++ `ra_sections()` (single source) so the two `schema_hash`es agree by construction.

- [ ] **Step 1: Write the failing test** (pytest) — a C++-written `run.atxrun` fixture opens, `section("backtest").f64("nav")` equals known values, and a byte-patched `schema_hash` raises at `open`.

```python
def test_open_and_read(tmp_path):
    p = _write_fixture_archive(tmp_path)      # produced by the C++ writer test helper / committed fixture
    ar = RunArchive.open(str(p))
    nav = ar.section("backtest").f64("nav")
    assert list(nav[:2]) == [100.0, 101.5]

def test_schema_drift_raises(tmp_path):
    p = _write_fixture_archive(tmp_path)
    data = bytearray(p.read_bytes()); data[24] ^= 1   # flip schema_hash byte
    q = tmp_path / "drift.atxrun"; q.write_bytes(data)
    with pytest.raises(ValueError):
        RunArchive.open(str(q))
```

- [ ] **Step 2: Run to verify it fails** — `pytest atx-vol/python/tests/test_runarchive.py -v` → FAIL (module missing).
- [ ] **Step 3: Implement** `runarchive.py`, the codegen for `_schema.py`, and the header `struct` format string matching Task 2's layout exactly.
- [ ] **Step 4: Run to verify it passes** — pytest → PASS.
- [ ] **Step 5: Commit.**

---

## Task 9: Cut the dispersion CLI over to RunArchive (hard cutover) + `dump`

**Files:** Modify `atx-vol/examples/spy_dispersion_backtest.cpp`; Modify `atx-vol/python/src/atxvol/report/io.py`, `parity.py`; Modify `atx-vol/tests/CMakeLists.txt` if a helper fixture-writer is added.

**Interfaces (Consumes):** all of Tasks 1–8.

Replace, per subcommand, the loose-TSV result writes with `RunDir::write_run_archive({...sections})`:
- `run-backtest`: `backtest`, `reconciliation`, `contract_marks`, `diagnostics` sections.
- `project-schedule`: `projected_schedule` section (+ diagnostics).
- `run-projected-backtest`: `projected_cold`, `projected_nodiv`, `mark_divergence`, `diagnostics`.
- `build-schedule`: `trade_schedule` section.
Delete the lifted `PhaseTimer`/`write_diagnostics` from the example (now in `run_diagnostics`). Add a `runarchive dump <run_dir> <section> [--tsv]` subcommand that reads a section and prints the legacy TSV shape (the escape hatch). Port `io.py`/`parity.py` to `runarchive.read_backtest_section` + `archive.section(...)`; **delete the phantom `step_pnl_total`** from `io._SERIES`.

- [ ] **Step 1: Write the failing test** — on the 3-session fixture copy (recipe from the prior sprint's `task-9-report.md`; NEVER modify `scratchpad\paired`), `run-backtest` then `parity.py` render reads from `run.atxrun` and reproduces the known final NAV / daily-pnl correlation. (Author as a pytest end-to-end guarded to the fixture.)
- [ ] **Step 2: Run to verify it fails** — build example + run subcommand → FAIL (still writing/reading TSV).
- [ ] **Step 3: Implement** the cutover.
- [ ] **Step 4: Run to verify it passes** — rebuild example (`scratchpad\build_example.bat`), run the fixture pipeline, run pytest → PASS; economics unchanged (final NAV, corr, zero mark divergence).
- [ ] **Step 5: Commit.**

---

## Task 10: Wave-A integration gate + docs

**Files:** Modify `docs/superpowers/plans/2026-07-21-...-wave-a-runarchive.md` (check boxes); update the sprint progress ledger.

- [ ] **Step 1:** Full Release build of `atx-vol` targets + `atx-vol-tests`; run `atx-vol-tests.exe` (all green) and the Python suite (`pytest atx-vol/python`, all green incl. new tests).
- [ ] **Step 2:** Controller runs the parity-full (135-session) pipeline end-to-end on the idle box; confirm economics unchanged reading from RunArchive (final NAV listed 125,026.06 vs projected-cold 123,243.12; daily-pnl corr 0.9972; zero mark divergences), and that `run.atxrun` opens + `validate_all()` passes + Python reader renders the report.
- [ ] **Step 3:** Confirm the loose result TSVs are gone from the run dir (only inputs + `run.atxrun` + compliance text remain); `runarchive dump` regenerates a section on demand.
- [ ] **Step 4: Commit** the ledger + checked plan.

---

## Self-Review

**Spec coverage (Wave A slice of the design §4.1/4.2/5, §9 testing):**
- run_diagnostics module → Task 6. ✓
- run_archive module + RunArchive format (§5) → Tasks 1–7. ✓
- Schema single-source killing 4-place duplication (M10/L13/L14) → Task 1 + Task 8 (`_schema.py` generated; phantom column deleted in Task 9). ✓
- Atomic write fixing `backtest.tsv` non-atomic writer (M12) → Task 3 (`write_run_archive_file` tmp+rename). ✓
- No manifest/checksum (M11) → Tasks 2–4 (directory=manifest, layered CRC). ✓
- Python binding-free reader + schema-hash-at-open (§5 read story) → Task 8. ✓
- Hard cutover of all consumers (§2) → Task 9. ✓
- Enum-label/dict-str travels-with-data (L14) → Tasks 3/5 encoders. ✓
- Integration on parity-full without economics change (§9) → Task 10. ✓
- **Deferred to later waves (correctly out of Wave A):** M1 clock-coupling (Wave B, needs the pipeline seam), StepObserver (Wave D), backtest_driver spine (Wave C), perf P1–P7 (Wave E), de-SPY (Wave D). Noted, not gaps.

**Placeholder scan:** section column sets for non-backtest sections are defined by-reference to named existing writers (precise, not TODO); the backtest 27-col set is given in full. No "TBD/handle errors/similar to". ✓

**Type consistency:** `RaSectionData`/`RaColumnData`/`RaSectionView`/`RunArchive`/`RunDir`/`ra_sections()`/`ra_schema_hash()`/`encode_*_section` used consistently across Tasks 1→9. `schema_hash` stamped in Task 3, checked in Tasks 4 & 8. ✓
