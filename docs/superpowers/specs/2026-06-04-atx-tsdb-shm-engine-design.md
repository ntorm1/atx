# atx-tsdb — In-Memory Time-Series Engine with Shared-Memory Reads

**Status:** Design approved (brainstorming complete) — ready for implementation plan
**Date:** 2026-06-04
**Author:** Nathan Tormaschy (with Claude Opus 4.8)
**Scope:** A new binary project in the `atx` monorepo. A standalone in-memory time-series store whose immutable datasets are served to multiple backtest reader processes through shared memory (file-backed `mmap`) for zero-copy, zero-re-parse reads.

---

## 1. Purpose & context

`atx-engine` is a deterministic quant backtesting engine (header-only, consumes the `atx-core` C++20 stdlib). It currently feeds market data from `InMemoryBarFeed`, which holds bars in a per-process `std::vector`. Every backtest process therefore re-parses parquet and holds its own full copy of the dataset in RAM.

Parameter sweeps and walk-forward analysis launch **many** backtest processes over the **same** historical dataset. Re-parsing and duplicating a multi-gigabyte dataset per process is the dominant cost and RAM ceiling.

`atx-tsdb` removes that cost. A dataset is loaded **once** from parquet into a sealed, immutable on-disk segment. Every reader process `mmap`s that segment read-only; the OS page cache holds a **single** physical copy shared across all readers. Reads are pure pointer arithmetic into the mapped grid — no copy, no parse, no syscall round-trip on the hot path.

### Role (decided)

**Backtest accelerator.** Serve historical bars/ticks to backtest reader processes at zero-copy speed while preserving the engine's three load-bearing invariants: determinism, no-look-ahead, no-survivorship-bias. Not a live market-data plane, not a general networked TSDB.

### Dependency direction

```
atx-engine  →  atx-tsdb  →  atx-core
```

The position-independent **segment format + loader + reader** live in `atx-tsdb/` (engine-agnostic). The engine-coupled read adapters live in `atx-engine/` and consume the reader.

---

## 2. Key decisions (brainstorming outcomes)

| # | Decision | Choice | Consequence |
|---|----------|--------|-------------|
| 1 | Primary role | Backtest accelerator | Deterministic, no-look-ahead, historical only |
| 2 | Write lifecycle | Load-once → seal → **immutable** | No concurrent writer; **zero atomics on the read path** |
| 3 | Storage layout | **Dense panel grid** `[field][time][instrument]` f64 + present-bitmap | Mirrors `RollingPanel`; reads are pointer math, no transform |
| 4 | Field schema | Named f64 fields, OHLCV preset, field directory in header | New fields (fundamentals/returns/vwap) drop in with no format bump |
| 5 | Platform | Cross-platform Win32 + POSIX behind one `Mapping` seam | Sweeps run on a Linux cluster and on the Windows dev box |
| 6 | Reader integration | **Layered (C)**: `ShmBarFeed` now, zero-copy `PanelView` as a deferred seam | Safe full-loop win first; headline zero-copy speed later, no rework |
| 7 | Shm mechanism | **File-backed `mmap`** (page-cache-shared), tmpfs option for pure RAM | Persistent, no daemon, cross-platform; immutable data makes this strictly better than anonymous named shm |

---

## 3. Architecture

### 3.1 Shared memory = file-backed `mmap`

Because datasets are immutable after seal, file-backed `mmap` is the superior form of shared memory:

- The loader writes the sealed segment to a **file** once.
- Every reader `mmap`s it **read-only** (`MAP_SHARED` / `MapViewOfFile(FILE_MAP_READ)`).
- The OS page cache holds **one** physical copy; all readers share those pages. That is shared memory — cross-process, zero-copy, automatic.
- Free persistence and warm restart (no re-parse on reader launch), trivial lifetime (the file exists until deleted), and **no long-running server**.
- Pure-RAM option: place the file on **`/dev/shm` (tmpfs)** on Linux — file semantics, RAM speed. Optional `mlock`/`VirtualLock` + prefetch to pin pages resident.

Anonymous named shm (`shm_open` / `CreateFileMapping`) is the right tool for *volatile* live state, not for an immutable historical dataset. The `Mapping` seam isolates the choice, so switching later is local.

### 3.2 Process topology

```
            ┌─────────────────────────┐
parquet ──► │  atx-tsdb-load (batch)   │ ──► dataset.atxseg  (sealed file)
            └─────────────────────────┘
                                              │  mmap read-only (shared pages)
              ┌───────────────┬───────────────┼───────────────┐
              ▼               ▼               ▼               ▼
        backtest #1     backtest #2     backtest #3   …  backtest #N
        (ShmBarFeed)    (ShmBarFeed)    (ShmBarFeed)      (ShmBarFeed)
```

No daemon. The loader is a pure batch tool. Readers are ordinary backtest processes.

### 3.3 Position-independence rule (load-bearing)

Different processes map the file at different virtual base addresses. The segment therefore stores **only offsets-from-base, never raw pointers**. Every accessor computes `base + offset`. This is non-negotiable and is enforced by the format design and tested.

---

## 4. Segment format (`format/segment.hpp` — the contract)

One contiguous file. All internal references are offsets from the mapping base. Sections in order:

```
┌─ SegmentHeader ──────────────── (cache-aligned, fixed size)
│   magic            u64   "ATXSEG1"   reject foreign/garbage files
│   format_version   u32
│   flags            u32   bit0 = SEALED (reader refuses unsealed)
│   total_bytes      u64
│   field_count      u32  (F)
│   instrument_count u32  (N)
│   time_count       u64  (T)
│   created_at_nanos i64   loader passes wall clock (no in-engine clock dep)
│   content_hash     u64   xxhash of data sections: dataset identity + determinism proof
│   off_field_dir / off_symbol_table / off_string_blob /
│   off_time_axis  / off_field_blocks / off_present_bitmap / off_footer   (u64 each)
├─ FieldDirectory ─── F × { name[16] (e.g. "close"), field_index u32 }
├─ SymbolTable ────── N × { name_off u32, name_len u32 }   column order == universe order
├─ StringBlob ─────── packed symbol-name characters
├─ TimeAxis ───────── T × i64 unix_nanos, sorted ascending   ← the visibility timeline
├─ FieldBlocks ────── F blocks, each [T][N] f64 row-major (time-major, instruments contiguous)
├─ PresentBitmap ──── T × ceil(N/64) u64   bit set ⇒ cell present; clear ⇒ NaN/absent
└─ Footer ─────────── seal marker + crc32(header + data)   torn/partial-write detection
```

### 4.1 Addressing (pure constexpr arithmetic, position-independent)

```
cell(f,t,i) = base + off_field_blocks + f*(T*N*8) + (t*N + i)*8        // f64 element
present(t,i) = bit (t*ceil(N/64)*64 + i) of PresentBitmap
```

This is byte-identical in shape to `RollingPanel`'s `[field][row][inst]` block, so the reader hands the alpha/panel path data in its native layout with no transform.

### 4.2 No-look-ahead, enforced by construction

`TimeAxis` is the **visibility timeline**. A reader holding `SimClock now` computes:

```
cutoff = upper_bound(TimeAxis, now) - 1        // newest visible row index
window = rows [cutoff - lookback + 1 .. cutoff]
```

Indexing past `cutoff` is impossible through the API ⇒ look-ahead cannot occur.

**v1 assumption (otherwise a non-goal):** each cross-section's axis time *is* its `knowledge_ts` (the engine's release-at-close default, where `knowledge_ts == bar.ts`). True bitemporal restatements (`knowledge_ts > event_ts`) are deferred to a v2 correction-overlay — documented, not built.

### 4.3 Ragged histories

IPO / delisting / differing start dates are handled entirely by the present-bitmap: absent cells read NaN and report `present == false`, exactly as `RollingPanel` already does. No special-casing. Survivorship is preserved: a delisted symbol keeps its column with absent cells after its final bar.

### 4.4 f64 vs Decimal (resolved in plan)

The grid is **f64** because the alpha/panel path is f64. `ShmBarFeed` reconstructs `domain::Bar` (whose prices are exact `Decimal`) via a **deterministic fixed-scale f64→Decimal** conversion for the exec-sim path. Input-price precision is f64-bounded (backtests already accept f64 market data); `Decimal` exactness still governs position/cash *accumulation*. Optional v2: a parallel scaled-integer price block for bit-exact input prices. The exact conversion rule is an implementation detail finalized in the plan.

---

## 5. Components

### 5.1 `atx-tsdb/` (position-independent, engine-agnostic)

| Unit | Role |
|------|------|
| `format/segment.hpp` | Header struct, offset/addressing helpers, magic/version constants. Pure, shared by loader + reader. **The contract.** |
| `platform/mapping.hpp` | `Mapping` RAII seam: `map_file_ro(path) → {base, bytes}`, unmap on destruction. Optional `prefetch()` / `lock_resident()`. |
| `platform/mapping_win32.cpp` | `CreateFile` + `CreateFileMapping(PAGE_READONLY)` + `MapViewOfFile(FILE_MAP_READ)` + `PrefetchVirtualMemory` / `VirtualLock`. |
| `platform/mapping_posix.cpp` | `open` + `mmap(PROT_READ, MAP_SHARED)` + `madvise(WILLNEED)` / `mlock`. |
| `loader/builder.hpp` | In-RAM builder: accumulate fields/symbols/time rows → compute offsets → write sealed file + `content_hash` + crc. |
| `loader/load_parquet.hpp` | atx-core `io/parquet` + `series::Frame` → builder. Maps parquet columns to named f64 fields; builds time axis + present-bitmap. |
| `reader/segment_reader.hpp` | `attach(path) → Result<SegmentReader>`: map, validate (magic/version/SEALED/crc), expose typed accessors (field-block span, time-axis span, symbol table, `present(t,i)`, `cutoff_index(now)`). Read-only. |

### 5.2 `atx-engine/` (engine-coupled adapters, consume the reader)

| Unit | Role |
|------|------|
| `data/shm_bar_feed.hpp` | **Approach A (v1):** `IDataHandler` over `SegmentReader`. `step()` walks the time axis → builds a `MarketSlice` from present cells → publishes Market events. Drop-in for `InMemoryBarFeed`. |
| `loop/shm_panel_source.hpp` | **Approach B seam (deferred):** zero-copy `PanelView` over shm. Prerequisite noted in §6. |

### 5.3 Tools (binaries)

| Binary | Role |
|--------|------|
| `atx-tsdb-load` | CLI: `--in data.parquet --out dataset.atxseg --fields close,volume,…`. The batch loader. |
| `atx-tsdb-stat` | CLI: dump header, field directory, T/N/F, `content_hash`, crc-verify. Inspect/debug. |

---

## 6. Data flow

### 6.1 Loader path

```
parquet → load_parquet (Frame) → builder
        → assign field blocks, dedup+sort time axis, intern symbols → column order,
          set present bits where data exists, NaN-fill absent
        → compute content_hash + crc → write file → set SEALED flag
```

Deterministic: same parquet + same field list → byte-identical file. `content_hash` is the proof and is reused as a golden test.

### 6.2 Reader path (Approach A — the v1 spine)

```
SegmentReader::attach(path) → ShmBarFeed{reader} → existing Phase-1/2 backtest loop, UNCHANGED
```

Same events in the same order ⇒ the existing `replay_determinism_test` invariants hold. Cross-check: `ShmBarFeed` event-hash equals `InMemoryBarFeed` event-hash on the same data.

### 6.3 Approach B prerequisite (honest scope)

`ISignalSource::evaluate` takes the concrete `PanelView`, whose addressing is **ring-specific** (`(head + cap - row) & (cap - 1)`, power-of-two capacity). The shm grid is **linear**. Zero-copy-into-`PanelView` therefore requires a small `PanelView` generalization (pluggable linear vs ring addressing). That refactor is the first task of the B seam — **not** in v1. v1's `ShmBarFeed` already delivers the load/RAM win with zero engine change.

### 6.4 Lifecycle & discovery

A dataset is a file path — no registry, no daemon. Readers attach by path; deletion is `rm`. Stale-format protection via magic + version; partial-write protection via the SEALED flag + footer crc. A reader refuses an unsealed or corrupt file with `Result::Err`, never UB.

---

## 7. Invariants

1. **Determinism** — same parquet + field-list → byte-identical segment (`content_hash`); `ShmBarFeed` → byte-identical event stream vs `InMemoryBarFeed`.
2. **No-look-ahead** — the reader can only address `≤ cutoff_index(now)`; reading the future is structurally impossible.
3. **No-survivorship-bias** — delisted symbols keep their column with absent cells after their final bar (present-bitmap), as the engine already does.
4. **Read safety (shm-specific)** — the reader validates magic / version / SEALED / crc before trusting one byte; a bad file → `Result::Err`. Offsets-only (position-independent). The read-only mapping means a buggy reader **cannot** corrupt the dataset for sibling processes.

---

## 8. Testing (TDD, GoogleTest; ASan/UBSan)

TSan is not applicable — there is no concurrent writer on a sealed segment.

- **`segment` format** — addressing math via constexpr `static_assert`; round-trip build → read; boundaries `T/N/F = 0, 1, max`.
- **`mapping`** — map/unmap a temp file on both platforms; reject missing/short file; prefetch/lock without crash.
- **`loader`** — golden parquet → known `content_hash`; determinism (build twice → identical bytes); NaN-fill + present bits for ragged input.
- **`reader`** — reject bad magic / wrong version / unsealed / crc-mismatch (each returns `Err`); `cutoff_index` binary-search boundaries (before-first, after-last, exact-hit).
- **`shm_bar_feed`** — event-hash equals `InMemoryBarFeed` on a shared fixture (determinism cross-check); no event with `knowledge_ts > now` (no-look-ahead).
- **cross-process integration** — loader writes a file → a child reader process maps and verifies the hash (or two mappings in-process if process spawning is painful on Windows CI).

---

## 9. Build integration

New top-level `atx-tsdb/` directory with its own `CMakeLists.txt`, auto-globbed `tests/*_test.cpp` and `bench/*_bench.cpp` (matching the engine convention — do not hand-edit the globs). Wired into the root `CMakeLists.txt` and `CMakePresets.json`. The `atx_warnings` interface (`/W4 /permissive- /WX`) is linked; any warning fails the build. Reuses the vcpkg Arrow dependency already present for parquet. Two install targets: `atx-tsdb-load`, `atx-tsdb-stat`.

---

## 10. Phasing

| Phase | Deliverable | Exit criterion |
|-------|-------------|----------------|
| **P1** | format + mapping + loader + reader + `atx-tsdb-stat` (standalone, no engine dep) | load parquet → seal → re-attach → verify `content_hash`, on both platforms |
| **P2** | `ShmBarFeed` + engine cross-check (Approach A) | backtest via shm == backtest via in-memory, byte-identical; no-look-ahead test green |
| **P3** (deferred seam) | `PanelView` addressing generalization + `ShmPanelSource` (Approach B zero-copy) | contract recorded, compile-guarded like `VmSignalSource`; **not built in this spec** |

---

## 11. Non-goals (YAGNI, explicit)

- No writes-after-seal; no live ingest.
- No true bitemporal restatements (deferred to a v2 correction-overlay).
- No compression/encoding — raw f64 (mmap wants raw bytes).
- No network/RPC — shared memory only.
- No query language — typed accessors only.
- No multiple frequencies per segment — one time axis per file; multi-frequency = multiple files.
- No Approach-B build in v1 (seam contract only).

---

## 12. Open items to resolve in the implementation plan

- Exact deterministic f64→Decimal conversion rule for `ShmBarFeed` Bar reconstruction (§4.4).
- Precise `SegmentHeader` field padding/alignment and `format_version` bump policy.
- Whether the cross-process integration test spawns a child process or maps twice in-process on Windows CI.
- `xxhash` / `crc32` source (atx-core `hash.hpp` vs a vendored small implementation).
