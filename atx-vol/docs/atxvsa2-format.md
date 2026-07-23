# ATXVSA2 — zero-copy mmap columnar surface-archive format (v2)

Status: **wave-1 design + implementation** (WS-S / S1–S3 of the
`2026-07-18-atx-vol-backtest-hotpath-throughput-sprint`). This document is the
authoritative layout spec; the on-disk POD structs live in
`atx-vol/include/atx/vol/surface_archive.hpp` (the `ArchiveV2*` family) and the
reader/writer in `atx-vol/src/surface_archive.cpp`. The zero-copy read view is
`atx-vol/include/atx/vol/priced_surface_view.hpp`.

Cite this file (and the primary sources in §7) in-code at the point of use, per
the sprint §3 research-first mandate.

---

## 0. Why a new format (the three bottlenecks it kills)

The v1 archive (ATXVSA v3, `major==3`, magic `ATXVSA03`) deserializes by
`reconstruct`: it CRC-32Cs the **whole blob** on every open, `make_unique`s a
polymorphic `IVolCurve` per slice, and copies each node array into fresh
`std::vector`s — *for every surface, even when the caller wants a subset*. Each
blob is padded to **4096 bytes** (`kArchiveBlobAlign`), a ~4 KB/surface floor and
read-amplification tax.

v2 removes all three:

| v1 cost | v2 fix |
|---|---|
| whole-blob CRC-32C on every `reconstruct` (bottleneck #5) | CRC is **lazy** — `open` verifies only header + directory framing (O(directory), not O(data)); per-surface payload CRC is a `validate()`-on-demand API, never run to price |
| `make_unique`/vector-copy per slice (bottleneck #5) | `PricedSurfaceView` reads slice scalars + parametric params **in place** from the mapping; **zero per-surface allocation** for parametric surfaces (see §4) |
| whole-board touch for a subset (bottleneck #1) | `map_symbol(sym)` hash-probes the lookup and constructs a view over **only that surface record's byte extent** — no other surface's bytes are read |
| 4096-B blob pad (bottleneck #6) | surfaces packed on **64-byte** boundaries (cache line / SIMD headroom); only the *file* tail is page-aligned |

Clean break (§0 of the sprint): v2 is not backward compatible with v1 and there is
**no dual-read** in product code. v1 read/write is retained this wave *only* for
not-yet-cutover readers and the throwaway migrator; the reader cutover + v1
deletion is WS-S S4 (wave 2).

---

## 1. Design lineage (researched primary sources → decisions)

The layout is grounded in a survey of the state-of-the-art zero-copy /
memory-mapped serialization formats (§7 sources), taking one idea from each:

- **Apache Arrow columnar / IPC** — buffers are contiguous typed arrays written
  end-to-end, min **8-byte** aligned (64-byte preferred for AVX-512), and the IPC
  file format is "designed to support memory-mapped I/O with aligned buffers …
  arrays point directly to the mapped memory without any copying or
  deserialization." → v2 stores the per-slice re-pricing scalars as **contiguous
  typed columns** (SoA): a `T[]` f64 array the bracket binary-search walks, plus
  `forward[]`, `q_eff[]`, `df[]`, `kind[]`. The view's `resolve`/`interp_forward`
  read these columns in place. Columns are 8-byte aligned; surfaces 64-byte.
- **FlatBuffers** — "each scalar is aligned to its own size … little-endian …
  the format is defined in terms of offsets and adjacency only," enabling
  zero-copy access with no parse step. → every v2 field is **naturally aligned**
  (f64/u64 on 8, u32 on 4, u16 on 2); records are laid out by offset+adjacency so
  a mapped region is queryable with no fix-up.
- **Cap'n Proto** — "the entire message is allocated in a few contiguous
  segments … relative pointers within the segment"; a struct is a *data section*
  followed by a *pointer (offset) section*. → v2 is a **single contiguous region**
  (one mmap); every internal reference is a **byte offset** (directory→surface is
  file-relative; surface-header→columns/payloads is record-relative), so **mapping
  needs no relocation / pointer fix-up**. The `ArchiveV2SurfaceHeader`'s block of
  `col_*_off` / `payload` offsets is exactly the "pointer section."

Deliberately **not** adopted: FlatBuffers vtables / Cap'n Proto far-pointers /
Arrow Flatbuffer metadata framing. Our schema is fixed and versioned by a
`schema_hash` (sizeof-fold + salt, §5), so we pay none of the schema-evolution
indirection — a reader built against a different struct shape recomputes a
different hash and refuses the file, exactly as v1 does.

---

## 2. File shape (single contiguous mmap region)

```
offset 0   ┌────────────────────────────┐
           │ ArchiveV2Header (256 B)     │  magic, version, schema_hash, counts,
           │                             │  section byte-offsets, header/meta CRC
lookup_off ├────────────────────────────┤
           │ ArchiveV2LookupSlot[slots]  │  open-addressed hash: symbol → surface
           │  (64 B each, pow2 count)    │  offset/size. O(1) map_symbol / find.
dir_off    ├────────────────────────────┤
           │ ArchiveV2DirEntry[count]    │  one per surface, sorted by canonical
           │  (80 B each)                │  symbol. Ordered map_all + jump table.
data_off   ├────────────────────────────┤  (64-B aligned)
           │ SurfaceRecord 0             │  packed, 64-B aligned, self-contained
           │ SurfaceRecord 1             │
           │ …                           │
           └────────────────────────────┘  file tail padded to 4096 (clean mmap)
```

`lookup ‖ directory` is the **metadata span** covered by `metadata_crc32c`. Both
the lookup slot and the directory entry carry `(surface_offset, surface_size)`,
and **the directory entry additionally carries a copy of each record's
`payload_crc32c`** (wave-2 S4/S5) — so *any* surface-payload rewrite, **including
one that preserves the record's byte length and offset**, changes
`metadata_crc32c` and hence the archive's content identity (F6
`ArchiveContentIdentity`, reused from v1). Without the directory CRC copy a
same-length in-place rewrite would be invisible to the identity: v2's per-record
CRC otherwise lives only in the record header, which `metadata_crc32c` does not
cover (the price of the lazy-CRC deserialize win). This is the faithful v2 port of
v1's per-blob `surface_crc32c` in the lookup slot.

### 2.1 SurfaceRecord — columnar, self-contained

All offsets below are **relative to the record start** (`surface_offset`). The
record is self-contained: `[surface_offset, surface_offset+surface_size)` holds
everything needed to price the surface and touches no other record.

```
rec off 0  ┌─────────────────────────────────┐
           │ ArchiveV2SurfaceHeader          │  pricing scalars (S,r,now,method,
           │                                 │  AlOpts), provenance record, n_slices,
           │                                 │  kind_bits, the col_*_off pointer block,
           │                                 │  payload_crc32c
           ├─────────────────────────────────┤  ── columnar SoA scalars (8-B aligned) ──
col_kind   │ u8   kind[n_slices]             │  VolCurveKind per slice (dispatch)
col_T      │ f64  T[n_slices]                │  slice maturity — bracket binary search
col_fwd    │ f64  forward[n_slices]          │  term forward F
col_qeff   │ f64  q_eff[n_slices]            │  effective carry
col_df      │ f64  df[n_slices]              │  discount factor (slice-rate decode)
col_borrow │ f64  borrow[n_slices]           │  SliceContext.borrow (fidelity)
col_nused  │ u64  n_used[n_slices]           │  SliceContext.n_used
col_ndrop  │ u64  n_dropped[n_slices]        │  SliceContext.n_dropped
col_nodes  │ u32  node_count[n_slices]       │  array-curve node counts (0 = parametric)
col_poff   │ u64  payload_off[n_slices]      │  record-relative offset of each payload
           ├─────────────────────────────────┤  ── variable payloads (packed, 8-B) ──
           │ slice 0 payload                 │  parametric POD OR node arrays (§3)
           │ slice 1 payload                 │
           │ …                               │
           └─────────────────────────────────┘  padded to 64 B (next record)
```

The hot query path (`bracket` + `interp_forward` + surface-`w` interp) reads only
the `kind`,`T`,`forward`,`q_eff`,`df` columns + at most two slices' payloads —
all contiguous, all in place.

---

## 3. Per-slice payload encoding (one kind byte drives it)

Mirrors v1's per-kind payloads byte-for-byte so the migrator is a pure
re-pack (no numeric change). `n = node_count`.

| kind | `node_count` | payload bytes (all f64/u32, 8-B aligned start) |
|---|---|---|
| `Essvi` (1) | 0 | `EssviParams` verbatim |
| `Svi` (2) | 0 | `SviParams` verbatim |
| `C8` (4) | 0 | `C8Params` verbatim |
| `ConvexDense` (0) | #nodes | `rmse_price` f64 · `n_obs` u64 · `n_active` u64 · `u[n]` f64 · `C[n]` f64 |
| `LinearVariance` (3) | #nodes | `k[n]` f64 · `w[n]` f64 |
| `SplineVol` (5) | #knots | `atm_vol,z_lo,z_hi` f64×3 · `n` u32 · pad u32 · `z[n]` f64 · `mult[n]` f64 · **`mult_cap` f64 · `w_offset` f64** · `n_butterfly_viol` u32 |

**`mult_cap` and `w_offset` are load-bearing** — both are live eval-time terms of
`SplineVolCurve::w()` (`mult_cap` clamps the served multiple; `w_offset` is the
calendar-cone additive total-variance lift). Dropping them (the pre-review v2
layout, and v1's ATXVSA v3, both did) silently misprices any SplineVol slice with
a clamping multiple or a projected offset — the view rebuilds them as their 0.0
struct defaults. They are serialized here so the view is bit-exact; the change
bumped `schema_hash_v2`'s salt (minor→1) so any older v2 file is rejected.

The ConvexDense fit diagnostics (`rmse_price/n_obs/n_active`) live **inline** in
its payload (only that kind needs them) rather than as a mostly-zero column.

---

## 4. Zero-copy read view — allocation profile & bit-exactness

`PricedSurfaceView` (S2) is a non-owning handle over a mapped SurfaceRecord span.
It reproduces `PricedSurface`'s **cold** query path (`QueryPricingTier::
LegacyCompatible` / `ColdReference` — no `QueryAccelerator`, which is the default
and only bit-reproducible backtest route). The surface-level math
(`interp_forward`, `bracket`, surface `w`/`iv` interpolation, `resolve`) is
replicated **inline and bit-for-bit** from `priced_surface.cpp` /
`vol_curve.cpp` over the columns; price/greeks call the identical free functions
(`american_price`, `american_greeks_fd/_al`, `american_delta`, `american_vega_al`).

Per-slice `w(k_log)` dispatch, by kind:

- **Essvi / Svi / C8** — `memcpy` the POD params from the mapping to a stack
  struct and call the exact free evaluator (`essvi_total_w` / `svi_total_w` /
  `c8_slice_w`). **Zero heap.** Bit-exact (same function).
- **LinearVariance** — replicate the ~12-line linear-in-w interpolation over the
  mapped `k[]`/`w[]` spans (`LinearVarianceCurve::w`). **Zero heap.** Bit-exact.
- **ConvexDense / SplineVol** — these carry non-trivial derived state
  (`ConvexDenseCurve`'s wing-anchor extrapolation table; `SplineVolCurve`'s
  natural-spline second-derivatives) and an iterative evaluator (Black-76
  inversion / spline solve). The view **eagerly materializes the exact concrete
  `IVolCurve`** for these two kinds at construction (copying the node arrays out
  of the mapping — one small alloc per such slice), then evaluates through the
  exact virtual `w`/`iv`. Bit-exact by construction (reuses the concrete curve);
  **not** zero-alloc for these kinds.

**Net allocation profile** (measured intent; the measure agent ratifies):

- Parametric surfaces (single-name Essvi/Svi/C8 — the dispersion-basket bulk):
  **zero per-surface allocation**. A 50-surface parametric partition maps with no
  heap traffic; `map_symbol` on one is an O(1) hash probe + a stack view.
- ConvexDense/SplineVol surfaces (SPY / index — a handful): a one-time,
  subset-scoped, CRC-free materialization of the array curves. Far cheaper than
  v1 (no whole-blob CRC, only referenced uids, done once and cached by S5), but
  **not** zero-alloc.

**Wave-2 opportunity (documented, not done):** push ConvexDense/SplineVol fully
zero-copy by (a) storing their derived state in the record (spline `m2nd[]`;
optional ConvexDense wing anchors) and (b) extracting span-based evaluator cores
that the concrete curves also call (guaranteeing bit-exactness). Both touch
`dense_slice.cpp`/`spline_curve.cpp`/`vol_curve.cpp` (not WS-S-owned), so they
are coordinated follow-ups, not wave-1 work.

The economic-correctness gate is **bit-equality** vs the v1 `reconstruct` path on
the same source surface (same doubles, same free functions). The parity test
asserts `view.resolve/fair_value/greeks/evaluate_batch` are `std::bit_cast`-equal
to a `PricedSurface` reconstructed from the same bytes.

---

## 5. Integrity, schema hash, endianness

- `schema_hash` folds `sizeof` of every v2 on-disk struct + the serialized POD
  slice structs (`EssviParams`, `SviParams`) + a **v2 salt** distinct from v1's,
  so a v1 file, a v2 file with drifted structs, and a v2 file from a different
  build are all rejected with `ParseError`. (C8/SplineVol/LinearVariance kinds
  are frozen by `static_assert` on their sizes, as in v1, so a new kind is a new
  *kind byte*, not a layout change to the fold.)
- **Salt history & the n_quad_price accept-list (C2 / SE-P1-2).** Salt low bits:
  `0100` initial · `0101` SplineVol payload gained `mult_cap`+`w_offset` · `0102`
  `AlOpts::n_quad_price` (the decoupled premium Gauss-Legendre order) now persists
  in `ArchiveV2SurfaceHeader::al_n_quad_price` / `DbSymbolRecord::al_n_quad_price`,
  each a formerly-zero **reserved u16**. Because that reuse is **layout-invariant**
  (`sizeof` unchanged, so the fold is unchanged), a `0101` record is byte-identical
  to a `0102` record whenever `n_quad_price == 0` (the tied default). The reader
  therefore **accepts both `0102` and the immediately-prior `0101` salt** — every
  existing archive still opens and reads `n_quad_price` back as `0` (tied). The
  salt is bumped anyway so a **pre-C2 reader rejects** a NEW archive that sets a
  genuinely decoupled premium order (it would otherwise silently reprice it with
  the tied order — the SE-P1-2 round-trip-fidelity bug). Salts `<= 0100` stay
  rejected (their payloads really are incompatible). The **DB manifest** side does
  NOT bump its schema hash — a fit-config field cannot misprice already-stored
  surfaces, and the reused reserved slot keeps every existing manifest openable.
- Records are **host byte order**; the header stamps `endian=1` (little) /
  `pointer_bits=64` and the reader rejects any mismatch. Little-endian LP64 only
  (matches the rest of atx-vol).
- CRC-32C (hardware SSE4.2 with table fallback, shared `detail::crc32c`):
  `header_crc32c` (header, own field zeroed), `metadata_crc32c` (lookup ‖
  directory), and a per-record `payload_crc32c` (record bytes, own field zeroed).
  **`open` verifies header + metadata + framing bounds only.** Per-record CRC is
  checked **only** by the explicit `validate_symbol` / `validate_all` API — never
  on the price path. This is the lazy-CRC win (#5).

### 5.1 Migration (v2 `0101` → `0102`, n_quad_price)

No migration action is required. Every `.atxvsa2` written before this change
opens unchanged and prices identically (its surfaces were fit/priced with the
premium order tied to `n_quadrature`, and `al_n_quad_price` reads back as `0`,
which resolves to exactly that tied scheme). A rewrite through the current writer
re-stamps the header with salt `0102`; the surface record bytes are unchanged for
tied surfaces. Only a surface actually fit under a decoupled-premium rung (e.g.
the `ql_fast` `n_quadrature=8, n_quad_price=32`) records a non-zero
`al_n_quad_price` and now round-trips to identical theo — previously it silently
reverted to the tied order on reload.

---

## 6. Reader/writer API surface (see surface_archive.hpp)

Writer: `write_surface_archive_v2(items, opts) -> Result<vector<byte>>` and
`write_surface_archive_v2_file(path, items, opts)`. Same `SurfaceArchiveItem`
inputs as v1 (symbol, `const PricedSurface*`, optional `SurfaceProvenance`),
so a caller re-targets by swapping the function name (clean break, no overload).

Reader: `class SurfaceArchiveV2` — backing-agnostic (owns a
`std::span<const std::byte>` + a type-erased owner `shared_ptr<void>`, so the
bytes can be an owned buffer today or a real mmap in wave-2 with no view change):
- `open(vector<byte>)` / `open_file(path)` — owned-buffer entry.
- `open_borrowed(span, shared_ptr<void> owner)` — the **mmap seam**: wave-2's
  `SnapshotCache`/`SurfaceDb` supply an `atx::tsdb::Mapping`-owning `shared_ptr`.
- `find(symbol) -> Result<ArchiveV2DirEntry>` — O(1) hash probe.
- `map_symbol(symbol) -> Result<PricedSurfaceView>` — **subset map**; touches
  only that record's extent. The returned view borrows the archive's bytes and
  must not outlive the archive.
- `map_all() -> Result<vector<PricedSurfaceView>>` — ordered, whole-board.
- `provenance(symbol)` / `validate_symbol(symbol)` / `validate_all()` — lazy CRC.

Lifetime: a `PricedSurfaceView` is a **borrow** of the archive's mapped bytes; it
is valid only while the owning `SurfaceArchiveV2` (and its backing) is alive. The
view is immutable after construction and concurrent-const-safe (like
`PricedSurface`). It exposes a never-reused `instance_id()` with `PricedSurface`'s
move semantics so the `PortfolioPricer` retained-cache ABA guard keeps working.

---

## 7. Primary sources (cite at point of use)

- FlatBuffers Internals — memory layout, natural alignment, little-endian,
  offsets+adjacency, zero-copy: https://flatbuffers.dev/internals/
- Cap'n Proto Encoding Spec — contiguous segments, relative pointers, struct =
  data section + pointer section: https://capnproto.org/encoding.html
- Apache Arrow Columnar Format — contiguous typed buffers, 8/64-byte alignment,
  mmap-first direct access:
  https://arrow.apache.org/docs/format/Columnar.html
  and the IPC/Feather mmap design: https://github.com/apache/arrow (docs/source/format)
