# atx-tsdb v2 — Zero-Copy Batch Panel for the Alpha VM — Design

> **Status:** Approved (brainstorming). Next: implementation plan via `superpowers:writing-plans`.
> **Predecessor:** [2026-06-04-atx-tsdb-shm-engine-design.md](2026-06-04-atx-tsdb-shm-engine-design.md) (v1: format, mapping, loader, reader, `ShmBarFeed`). v1 §10 deferred "Approach B" (zero-copy panel) to a v2 — this is that v2.

---

## 1. Goal

Let the batch alpha VM (`alpha::Engine`) evaluate a compiled `alpha::Program` directly over a sealed atx-tsdb segment **with zero copy and zero conversion** — the segment's field blocks are mapped once (OS page cache) and the VM's `alpha::Panel` reads them in place. This is the high-throughput read path for the atx-engine backtesting pipeline: a large historical dataset is mapped once and shared read-only across every backtest reader process.

**Non-goal restatement (from v1, still holds):** no writes-after-seal, no live ingest, no compression, no network/RPC, no query language.

---

## 2. Why this is the win

The sealed segment stores a dense grid `[field][time][instrument]` of f64 (v1 §4). The batch VM's input type, `alpha::Panel` (`atx-engine/include/atx/engine/alpha/panel.hpp`), stores **date-major field columns**: the value of `(field, date, inst)` lives at flat index `date * instruments + inst` within that field's column. **The two layouts are byte-identical** (field-blocked, time/date-major, instrument-contiguous). No transpose, no repack.

`alpha::Engine` (`alpha/vm.hpp`) borrows the Panel by `const Panel&` for its whole lifetime and touches it through exactly four members: `field_all(FieldId)`, `field_cross_section(FieldId, DateIdx)`, `in_universe(DateIdx, inst)`, `field_id(name)`. The **only** thing standing between the VM and the mmap is that `Panel` currently *owns* its `std::vector` columns.

Today's batch backtest data path is wasteful:

```
shm f64 → ShmBarFeed (f64→Decimal Bar) → EventBus → loop drain → SliceRow → RollingPanel f64
```

Two conversions (f64→Decimal→f64), per-bar event overhead, and a full copy of the window — to reconstruct a panel the file already contains. v2 deletes that path for batch evaluation.

---

## 3. Architecture

```
SegmentReader::attach(path)                       // v1: validate magic/version/SEALED/crc
   → attach_segment_panel(reader, [t0,t1), fields, universe_policy)   // v2 bridge, zero-copy
       → MappedPanel{ Mapping (moved in), alpha::Panel (columns alias the mmap) }
   → alpha::Engine(panel).evaluate(program) → SignalSet               // VM runs over page cache
```

The `alpha::Panel` returned by the bridge holds `std::span<const f64>` column views that point **into the mapped bytes**; reads are pure span indexing — the same machine code as the owned path, no branch, no virtual dispatch. The `MappedPanel` owner bundles the `Mapping` with the `Panel` so the mapping cannot be outlived (see §7).

### Decisions locked in brainstorming

| Decision | Choice |
|---|---|
| Primary read consumer | **Batch alpha VM** (whole-window Panel). Streaming `ShmPanelSource` deferred. |
| Zero-copy mechanism | **Borrowing `Panel` backend** — `columns_` becomes `vector<span<const f64>>`; owned vs mapped behind the same read surface. Engine/oracle unchanged. |
| Universe mask source | **Present-bitmap by default, explicit universe field as override.** |
| SIMD alignment | **Bump segment to format v2**: 64-byte-align each field block. |

---

## 4. Components (each a TDD unit)

### U1 — Segment format v2 (64-byte aligned field blocks)

- Builder pads each field block start to a 64-byte (cache-line / AVX-512) boundary before writing; `SegmentHeader` offsets already record the padded positions, so no addressing math changes.
- `kFormatVersion` bumped to `2`.
- Reader accepts **both** `1` and `2` (rejects anything else). All section access is via header byte-offsets, so a v1 (8-byte-aligned) segment still loads correctly — alignment only gates the VM's SIMD fast path, not correctness. Existing v1 segments need no reload.
- The mmap base is page-aligned (4 KiB), so a block offset that is a multiple of 64 is 64-byte-aligned in virtual memory.
- **Scope note:** only field *block starts* are aligned (the SIMD-hot sections). Per-date rows are `instruments` f64 and are *not* individually padded — padding every row would break the dense `field_all` contiguity the Panel requires. Aligned block start (date 0) is the main vectorization win; kernels peel / use unaligned loads for the row remainder.

### U2 — `alpha::Panel` borrowing backend

- Refactor storage from `std::vector<std::vector<f64>> field_data_` to `std::vector<std::span<const f64>> columns_` (the access plane) **plus** an optional owned backing store (`std::vector<std::vector<f64>>`) used only when the Panel owns its data.
- Keep `create(...)` (owned) working byte-for-byte; add `create_borrowed(dates, instruments, field_names, column_spans, universe)` where `column_spans[i]` is a non-owning view of length `dates*instruments`.
- The read surface (`field_all`, `field_cross_section`, `in_universe`, `field_id`, `dates`, `instruments`, `num_fields`) is **unchanged** → `alpha::Engine` and `alpha::Oracle` need zero edits.
- Gate: an equality test proving identical `SignalSet` from an owned Panel and a borrowed Panel over the same bytes.

### U3 — Universe bridge (present-bitmap → mask, + override)

- **Default:** materialize the date×inst `u8` universe from the segment present-bitmap once at attach (cold path). A single `u8` mask read on the hot path is the fastest per-cell membership test (vs a runtime bit-shift on the u64 bitmap); the mask is ~`1/(8·num_fields)` the size of the data, so the one allocation is negligible against the zero-copy data win.
- **Override:** if the segment carries an explicit universe field (e.g. a `"universe"` field written by the loader, or a dedicated universe bitmap section), use it instead of cell-presence — so membership can differ from data-presence (a listed-but-untraded day stays in-universe).
- Survivorship-correct either way: a delisted instrument's cells go absent (NaN, out-of-universe) after its final row, exactly as the engine does today.

### U4 — `attach_segment_panel` bridge

- Lives in atx-engine (already links `atx::tsdb` since tsdb-8). Header e.g. `atx-engine/include/atx/engine/alpha/segment_panel.hpp`.
- Signature (shape):
  `Result<MappedPanel> attach_segment_panel(std::string path, TimeWindow window, std::span<const std::string> fields = {}, UniversePolicy policy = FromPresentBitmap)`.
- Responsibilities:
  - Attach + validate the segment (delegates to `SegmentReader`).
  - Resolve the requested field names to segment field indices (empty `fields` ⇒ all fields).
  - Slice a `[t0, t1)` date window over the segment time axis (binary-search the bounds; `t1` is the no-look-ahead cutoff). The window selects a contiguous date sub-range; column spans are offset into each field block by the window start.
  - Build a **borrowed** `alpha::Panel` whose `column_spans` alias the mapped field blocks and whose universe comes from U3.
  - Return `MappedPanel` (owns the `Mapping`, exposes `const Panel&`).
- **Window addressing:** because field blocks are date-major (`date*inst + inst`), a `[d0, d1)` window is the contiguous sub-span `[d0*inst, d1*inst)` of each field column — a pure offset, still zero-copy.

### U5 — Throughput machinery + benchmarks

- **Prefetch:** per-field-block `prefetch()` (madvise `WILLNEED` / `PrefetchVirtualMemory`) for *only* the projected fields and selected window, not the whole file. Optional `MAP_POPULATE` (POSIX) to fault the window in eagerly.
- **Bench (`bench/panel_read_bench.cpp`):** GB/s and ns/cell for (a) zero-copy attach + `evaluate`, (b) memcpy-load-into-owned + `evaluate`, (c) the v1 `ShmBarFeed` event path — quantifying the win.
- **Golden equality test:** `VM(shm-borrowed) == VM(owned) == Oracle` on a shared fixture (determinism + correctness proof for the whole bridge).

---

## 5. Data contract / lifetime

- `MappedPanel` is **move-only** and owns the `Mapping`. The `Panel` it exposes borrows the mapping; bundling them makes "Panel outlives its mapping" structurally impossible.
- The borrowed `Panel`'s column spans are valid for the `MappedPanel`'s lifetime. The VM (`Engine`) borrows the `Panel` by const-ref for the duration of `evaluate` — strictly within the `MappedPanel`'s lifetime. No span escapes.

---

## 6. Invariants (preserved from v1)

1. **Determinism** — same segment + program → identical `SignalSet`; proven by the U5 equality test (borrowed == owned == oracle).
2. **No-look-ahead** — structural: ts-operators read backward-only (VM's responsibility), and the `[t0, t1)` window is the future cutoff. No per-date gating in the panel.
3. **No-survivorship-bias** — universe from present-bitmap (or explicit override); delisted instruments are absent after their final row.
4. **Read safety** — `SegmentReader` validates magic / version / SEALED / crc before exposing a byte; read-only mapping cannot corrupt the dataset for sibling processes. v2 readers also reject `format_version > 2`.

---

## 7. Risks & mitigations

| Risk | Mitigation |
|---|---|
| Borrowed Panel outliving its mapping → UB | `MappedPanel` owner-bundle (move-only) owns the `Mapping`; the `Panel` is only reachable through it. |
| `Panel` refactor blast radius (core type) | Keep the exact read surface; owned `create(...)` stays; the owned-vs-borrowed equality test gates the change before any VM code trusts it. |
| Cross-section SIMD alignment | Align block *starts* only (date 0); kernels peel / unaligned the per-row remainder. Don't pad rows (would break dense `field_all`). |
| Format bump breaks existing segments | Reader accepts v1 **and** v2 (offset-based addressing); no reload required. |
| Window slice off-by-one / look-ahead | `[t0, t1)` half-open with binary-searched bounds; reuse v1 `cutoff_index` boundary tests (before-first, after-last, exact-hit). |

---

## 8. Testing (TDD, GoogleTest; ASan/UBSan)

- **U1 format v2** — block-start 64-byte alignment via `static_assert`/offset checks; v1 segment still attaches; round-trip build→read; `format_version` accept/reject (1 ok, 2 ok, 3 rejected).
- **U2 borrowing Panel** — owned vs borrowed produce identical `field_all`/`field_cross_section`/`in_universe`; identical `SignalSet` from `Engine` over each.
- **U3 universe** — present-bitmap → mask correctness (absent cell ⇒ out-of-universe); explicit-override path; delisted-after-final-row survivorship.
- **U4 bridge** — full-window and sub-window panels; field projection (subset + all); window boundary cases (mirror `cutoff_index`); lifetime (MappedPanel move leaves source safe).
- **U5** — `VM(shm) == VM(owned) == Oracle` golden equality; bench runs and reports (not a pass/fail gate, but must not regress correctness).

---

## 9. Build integration

- Format/builder/reader changes land in `atx-tsdb/` (v1's home). The bridge (`attach_segment_panel`, `MappedPanel`) lands in `atx-engine/` (it depends on both `atx::tsdb` and `alpha::Panel`; the link already exists from tsdb-8).
- Auto-globbed `tests/*_test.cpp` and `bench/*_bench.cpp` per the existing convention; `atx_warnings` (`/W4 /permissive- /WX`) linked.

---

## 10. Phasing

| Phase | Deliverable | Exit criterion |
|---|---|---|
| **P0** | U1 segment format v2 (64B-aligned blocks; reader accepts v1+v2) | round-trip + v1 back-compat + version accept/reject green |
| **P1** | U2 borrowing `Panel` backend | owned == borrowed `SignalSet`; Engine/oracle unchanged |
| **P2** | U3 universe bridge (present-bitmap default + explicit override) | universe correctness + survivorship tests green |
| **P3** | U4 `attach_segment_panel` + `MappedPanel` | window/projection/lifetime tests green; VM runs over shm |
| **P4** | U5 prefetch + bench + golden equality | `VM(shm)==VM(owned)==Oracle`; bench reports GB/s + ns/cell |

---

## 11. Deferred (not in v2)

- Streaming `PanelView` ring→linear generalization + `ShmPanelSource` for the live event loop (the original v1 §6.3 / §10-P3 item; the loop already works via `ShmBarFeed`).
- Multi-segment virtual concatenation (a `SegmentSet` spanning files / one backtest across many segments).
- Compression / encoding (raw f64 only — mmap wants raw bytes).
- Bitemporal restatement overlay (`knowledge_ts > event_ts` corrections).
- Large / huge pages (privilege hassle on Windows; `WILLNEED` + optional `MAP_POPULATE` cover the win for now).

---

## 12. Open items for the implementation plan

- Exact `MappedPanel` API: does it expose `Panel&` (mutable, for `Engine` ctor which takes `const Panel&`) or only `const Panel&`? (Lean: `const Panel&`.)
- Explicit-universe representation in the segment: a named `"universe"` f64 field (0/1) vs a dedicated universe bitmap section + format flag. (Lean: named field first — no second format change.)
- Whether `attach_segment_panel` takes a `SegmentReader` (caller owns) or a path (bridge owns the reader inside `MappedPanel`). (Lean: path in, reader owned by `MappedPanel` — simplest lifetime.)
- `TimeWindow` type: unix-nanos `[t0,t1)` vs date-index `[d0,d1)`. (Lean: nanos in, resolved to indices internally — matches `cutoff_index`.)
