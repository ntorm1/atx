# ATXVSA v3 — Priced curve-surface binary archive

**Date:** 2026-07-05
**Status:** Implemented, 591/591 tests green.
**Scope:** `atx-vol` — revamp `surface_archive.hpp/.cpp` for the configurable curve
family; add `priced_surface.hpp/.cpp` and `VolaSession::to_priced_surface`.

## 1. Goal

Revamp the binary archive so a fitted **virtual vol surface of any selected curve
type** can be `fit -> serialize -> deserialize -> slot into a pricer -> output the
SAME theo values`. The format is proprietary and high-performance: multi-surface,
binary headers with a jump table of byte offsets so a single surface loads without
reading the whole file, hot-path-optimized deserialization at SOTA speeds, and
end-to-end complete — it reproduces the real SPY OPRA board accuracy. No backward
compatibility with the v2 (VolSurface-only) format.

## 2. Problem with v2

The v2 archive serialized a `VolSurface` — eSSVI/SVI slices only, as a
**fixed-stride POD array** (`memcpy` of `EssviParams[]` / `SviParams[]`). The new
curve family (`vol_curve.hpp`) is polymorphic (`IVolCurve`: ConvexDense / Essvi /
Svi), and the accuracy-critical **ConvexDense** curve carries a **variable-length**
node array (`u[]`, `C[]`). A fixed-stride array cannot represent it. Worse, v2
serialized only the vol slices — not the per-slice re-pricing context or the
pricing scalars — so a deserialized surface could not reproduce a session's theo.

## 3. Design

### 3.1 `PricedSurface` — the serialization currency

A new value type (`priced_surface.hpp`) is the "virtual vol surface that slots into
a pricer": the minimal, cache-free, value-typed state that fully determines served
theo.

```
PricedSurface = CurveSurface           // polymorphic fitted curves (any kind)
              + vector<SliceContext>   // per-slice term carry (T, F, borrow, q_eff)
              + PricingContext         // S, r, now_ts, AmericanMethod, AlOpts, uid
```

Its queries reproduce `VolaSession`'s **cold served path** bit-for-bit:

```
fair_value(K,T,side) = american_price(S, K, T, surface.iv(k,T), r, q_eff(T),
                                       side, method, al_opts)
   where k = ln(K / F(T)),  (F, q_eff) = the session's clamp-outside /
   linear-between forward interpolation.
```

This is the exact path the ConvexDense (index / SPY) surface is served on:
`VolaSession::fair_value` takes the cold Andersen-Lake branch whenever a
polymorphic override is present (`served_cache` returns null). So a `PricedSurface`
snapshot of such a session prices **identically** to the live session — reproducing
its board accuracy.

`VolaSession::to_priced_surface()` produces one: it deep-copies the override
`CurveSurface` (ConvexDense/Svi), or for the eSSVI default path rebuilds the fitted
eSSVI slices into a uniform `CurveSurface`; it copies `ctx_` and the resolved
pricing scalars from `in_`. Deep copy is enabled by a new `IVolCurve::clone()` /
`CurveSurface::clone()` (the container is move-only).

### 3.2 On-disk format (ATXVSA v3)

```
header (464 B) -> lookup table -> directory (jump table) -> 4096-aligned blobs
```

- **Header** — magic `ATXVSA03`, versions, endian/pointer-bits guards, a
  sizeof-based **schema hash** (with a v3 salt), section offsets, header CRC-32C,
  metadata CRC-32C. A reader rejects any framing / size / schema / endian mismatch.
- **Lookup table** — open-addressed hash (`symbol_hash -> slot`), power-of-two, load
  factor configurable. Resolves one symbol to its blob offset/size + whole-blob CRC
  without scanning.
- **Directory (jump table)** — one 88-B entry per surface, sorted by canonical
  symbol: `(offset, size, uid, kind_bits, n_slices)`. The random-access index — seek
  to and reconstruct ONE surface, touching no other blob's bytes.
- **Blob** — self-describing, **variable-length**:
  ```
  SurfaceBlobHeader (128 B)   magic, n_slices, uid, section offsets, payload CRC
  symbol bytes                padded to 64
  ArchivePricingRecord (48 B) S, r, now_ts, method, AlOpts, uid
  slice records[]             sequential, each = ArchiveSliceHeader (96 B) + payload
     ConvexDense payload      u[node_count] ‖ C[node_count]  (variable)
     Essvi payload            EssviParams (fixed POD)
     Svi payload              SviParams   (fixed POD)
  ```
  Each `ArchiveSliceHeader` is kind-tagged and carries the slice's `SliceContext`
  carry, the curve scalars (T, F, df) needed to reconstruct the polymorphic curve,
  and — for ConvexDense — the fit diagnostics + `node_count`. `rec_size` lets the
  reader walk slices with a single running offset (a single surface is small; no
  intra-blob jump table needed). Blobs start 4096-aligned so a memory-mapped reader
  page-faults only the target surface.

### 3.3 Integrity + SOTA deserialization

- **Layered CRC-32C:** header CRC (own field zeroed), metadata CRC (lookup ‖
  directory), per-blob whole-blob CRC (in the owning lookup slot). A `payload_crc32c`
  is written into each blob header (self-describing / partial-verify field) but NOT
  re-checked on the hot path — the whole-blob CRC strictly subsumes it, so
  reconstruction is ONE CRC pass, not two.
- **Hardware CRC-32C:** an SSE4.2 `_mm_crc32_u64` path (8 bytes/step), runtime-
  dispatched by CPUID, with a table-driven fallback of bit-identical output. Emitted
  under `__attribute__((target("sse4.2")))`, only ever called behind the CPUID gate.
- **Single-pass parse, direct construction:** `map_symbol` = one hash probe + one
  blob parse (whole-blob CRC verify, then direct curve `make_unique`), independent of
  archive size. No intermediate validate/`create` churn.

### 3.4 Reader/writer API

```
Result<vector<byte>> write_surface_archive(span<SurfaceArchiveItem>, opts);   // {symbol, const PricedSurface*}
Status               write_surface_archive_file(path, items, opts);           // atomic tmp+rename
SurfaceArchive::open(bytes) / open_file(path)                                  // validate framing
  .find(symbol)        -> ArchiveDirEntry                                      // metadata only
  .map_symbol(symbol)  -> PricedSurface   (case-insensitive, hot path)
  .map_all() / map_all_into(span<optional<PricedSurface>>)
```

All reader queries are `const` and concurrent-safe; each `map_*` returns a fresh,
independently-owned `PricedSurface`.

## 4. Measured results

**Round-trip correctness (591/591 tests):**
- All three curve kinds (ConvexDense / eSSVI / SVI) serialize → deserialize with
  **bit-identical** iv, total-variance, and re-Americanized fair_value across a
  (K,T,side) grid; ConvexDense node arrays round-trip byte-for-byte.
- Multi-symbol mixed-kind archive, symbol lookup (500 symbols, case-insensitive),
  low-load-factor growth room, `map_all` + capacity guard, 4-thread concurrent reads.
- Corruption rejection: bad magic, header CRC, schema-hash mismatch, blob-payload
  bit-flip → `ParseError`.

**SPY real-OPRA end-to-end (`SpyArchiveRoundTrip`):**
- Fit SPY ConvexDense via `VolaSession` → `to_priced_surface` → archive →
  `open` → `map_symbol("SPY")`.
- 35 slices, **30.5 KB** archive. **5701** IVs and **5377** fair-values
  **bit-identical** between the live session and the reload-from-archive surface.
- Reconstructed **pxCLN = 99.49%** (4314/4336 clean quotes in NBBO band) — the SPY
  board accuracy reproduced exactly through serialize/deserialize.

**Deserialization throughput (`surface_archive_bench`, 512-surface mixed book):**
- `open()` (framing + header/metadata CRC): ~0.15 ms.
- `map_symbol` (hot single-surface random access): **~3.3–4.7 µs/surface**
  (~0.25 M surfaces/s/core), CRC-bound.
- `map_all` bulk warm-load: ~7–8 µs/surface, ~1 GB/s effective on CRC'd blob bytes
  (hardware CRC-32C).

## 5. Limitations / future levers

- **eSSVI snapshot serves cold.** The archive reproduces the session's COLD served
  path. For a ConvexDense session that IS the served path (bit-identical, the SPY
  headline). For an eSSVI-default session the live session serves via its *cached*
  correction pricer; the snapshot rebuilds the eSSVI curves and prices them cold
  (the accurate reference). The surface's model IV is preserved bit-identically; the
  penny-level cached-vs-cold difference is the correction cache's single-carry
  surrogate, not an archive loss. Serializing the correction cache was rejected
  (large, carry-baked, rebuildable — not worth the bytes).
- **CRC is the deserialize floor.** Single-stream hardware CRC-32C runs ~1 GB/s on
  random small blobs (memory-bound). A tri-stream interleaved CRC (3 accumulators +
  GF(2) combine) would ~3× large-buffer CRC throughput; deferred (marginal on small
  blobs, adds a polynomial-combine dependency).
- **Owning reconstruction.** Curves own their params/nodes, so reconstruction
  allocates. A zero-copy variant (fixed-POD curves viewing the mapped buffer) is
  possible but would ripple through the curve family; not pursued given the
  single-digit-µs single-surface cost.

## 6. Files

New: `include/atx/vol/priced_surface.hpp`, `src/priced_surface.cpp`,
`tests/spy_archive_roundtrip_test.cpp`, `examples/surface_archive_bench.cpp`.
Rewritten: `include/atx/vol/surface_archive.hpp`, `src/surface_archive.cpp`,
`tests/surface_archive_test.cpp`.
Modified: `vol_curve.hpp/.cpp` (clone), `session.hpp/.cpp` (to_priced_surface),
`vol.hpp` (umbrella), `CMakeLists.txt`, `tests/CMakeLists.txt`.
