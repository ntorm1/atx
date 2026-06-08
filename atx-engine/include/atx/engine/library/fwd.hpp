#pragma once

// atx::engine::library — disk-backed append-only alpha library forward declarations (Sprint S4).
//
// A lightweight header other engine headers include to NAME the library-layer
// types without pulling in the on-disk record schema, segmented store, dedup
// index, correlation-neighbor index, lifecycle journal, manifest, or the
// Library facade behind them.
//
// WHAT THIS LAYER IS: the Phase-4 combine::AlphaStore is an in-process,
// non-serialized registry that loses all provenance between runs — it carries
// no expression source, no lineage, and no cross-run stable identity. S4 is
// the disk-backed superset: an APPEND-ONLY, IMMUTABLE-SEGMENT library that
// persists every admitted alpha's on-disk record (expression, lineage,
// canonical hash, lifecycle state) so the full pool survives process restart,
// supports cross-run dedup, and can be loaded selectively into combine::AlphaStore
// for a new run. The mutable metadata (dedup index, manifest) live in a per-
// thread SQLite connection (SQLITE_THREADSAFE=2, one-Database-per-thread rule);
// the bulk signal data lives in immutable mmap-able segments.
//
// STORAGE MODEL — LSM-inspired memtable → immutable sealed segment:
//   1. New alphas are appended to an in-memory write buffer (the "memtable").
//   2. On flush (capacity threshold / explicit seal), the buffer is written to
//      a new immutable segment file with a tag8 magic header, format_version,
//      per-section byte-offsets, a trailing seal marker, integrity CRC, and
//      a validate-before-expose check — mirroring the atx::tsdb segment framing.
//   3. Sealed segments are NEVER modified; they are read via read-only mmap
//      (reusing atx::tsdb::Mapping for the RAII lifetime). Segments accumulate
//      until a compaction pass merges and GC-deads.
//   4. The mutable metadata (manifest + dedup index + lifecycle events) are
//      stored in a SQLite database — one connection owned per thread, never
//      shared across threads.
//
// IMMUTABLE-BULK-IN-MMAP-SEGMENTS / MUTABLE-METADATA-IN-SQLITE SPLIT:
//   * Segment files hold the bulk append-only data (AlphaDirEntry + Provenance
//     + raw bytes) — accessed read-only via mmap after sealing.
//   * SQLite holds the mutable cross-segment index (dedup keys, lifecycle
//      states, manifest entries) — small, transactional, easily backup-able.
//
// Per-unit header map (added per unit):
//   library/record.hpp       — SegmentHeader, AlphaDirEntry, Provenance       (S4-1)
//   library/store.hpp        — LibraryStore (append-only segmented store)      (S4-1)
//   library/dedup_index.hpp  — DedupIndex (canonical-hash dedup over SQLite)   (S4-2)
//   library/corr_index.hpp   — CorrNeighborIndex (SimHash LSH accelerator)     (S4-3)
//   library/lifecycle.hpp    — LifecycleState, LifecycleJournal (PIT journal)  (S4-4)
//   library/manifest.hpp     — LibraryManifest (versioned segment manifest)    (S4-5)
//   library/library.hpp      — Library facade (unified public API)             (S4-5)

#include "atx/core/types.hpp" // atx::u8 (needed for LifecycleState underlying type)

namespace atx::engine::library {

// ===========================================================================
//  On-disk segment framing types (S4-1)
// ===========================================================================

// Fixed-size header at byte-offset 0 of every library segment file. Carries
// the tag8 magic cookie, format_version, per-section byte-offsets, and the
// integrity CRC — validated by LibraryStore before the segment is mmap-exposed.
// Full definition in library/record.hpp (S4-1).
struct SegmentHeader;

// One alpha's directory entry within a sealed segment: stable AlphaId handle,
// byte-offset into the segment's data section, and the on-disk record length.
// Enables O(1) random access into an mmap-ed segment.
// Full definition in library/record.hpp (S4-1).
struct AlphaDirEntry;

// Expression source + lineage + canonical-hash provenance record persisted for
// each admitted alpha. Superset of combine::AlphaRecord: adds the S3 DSL
// expression text, the factory-run lineage (parent hashes, generation index),
// and the S3 canonical_hash used as the cross-run dedup key.
// Full definition in library/record.hpp (S4-1).
struct Provenance;

// ===========================================================================
//  Store — append-only segmented store (S4-1)
// ===========================================================================

// Manages the on-disk sequence of immutable library segments (write buffer →
// sealed mmap-segment lifecycle). Provides append(), flush/seal, and read-back
// by AlphaId. Thread-compatible: the owning thread drives mutations; readers on
// other threads access only sealed segments (immutable after seal).
// Full definition in library/store.hpp (S4-1).
class LibraryStore;

// ===========================================================================
//  Dedup index — canonical-hash dedup over SQLite (S4-2)
// ===========================================================================

// Cross-run, cross-segment dedup index keyed by the S3 factory::canonical_hash.
// Backed by a SQLite table on the owning thread's Database connection (one-
// Database-per-thread; SQLITE_THREADSAFE=2 serial-owner rule). A lookup returns
// the existing AlphaId if the canonical hash is already present, or inserts a
// new row. Prevents structurally-equivalent expressions from being re-evaluated
// or re-admitted across runs.
// Full definition in library/dedup_index.hpp (S4-2).
class DedupIndex;

// ===========================================================================
//  Correlation-neighbor index — SimHash LSH accelerator (S4-3)
// ===========================================================================

// Approximate nearest-neighbor index over admitted alpha PnL vectors using
// SimHash (signed random projection on simd::dot + Xoshiro256pp::normal()
// for projection generation). Accelerates the marginal-corr screen by finding
// the k-nearest neighbors in O(1) SimHash-bucket lookup rather than scanning
// the entire pool via combine::pairwise_complete_corr. The exact pairwise_
// complete_corr call remains the differential reference for correctness checks.
// Full definition in library/corr_index.hpp (S4-3).
class CorrNeighborIndex;

// ===========================================================================
//  Lifecycle state machine (S4-4)
// ===========================================================================

// Point-in-time lifecycle state for an alpha in the library. The PIT lifecycle
// is keyed to an as-of PERIOD INDEX (not a wall-clock date) so the journal is
// deterministic under replay and independent of system time. Transitions are
// append-only: a new journal entry is written for each state change; the current
// state is the latest entry for a given AlphaId.
// Full definition in library/lifecycle.hpp (S4-4).
enum class LifecycleState : atx::u8;

// Append-only journal of LifecycleState transitions per alpha. Backed by a
// SQLite table on the owning thread's Database connection. Provides the current
// state lookup and the full transition history for audit / replay.
// Full definition in library/lifecycle.hpp (S4-4).
class LifecycleJournal;

// ===========================================================================
//  Versioned manifest (S4-5)
// ===========================================================================

// Versioned catalog of sealed segments + their byte-ranges and integrity hashes.
// Written to SQLite on each seal; loaded at library open to reconstruct the full
// mmap-segment set. Guarantees that a snapshot of the manifest + its segments
// is byte-identical to a prior snapshot (the round-trip identity invariant).
// Full definition in library/manifest.hpp (S4-5).
struct LibraryManifest;

// ===========================================================================
//  Library facade (S4-5)
// ===========================================================================

// Unified public API over the full S4 library stack: LibraryStore + DedupIndex
// + CorrNeighborIndex + LifecycleJournal + LibraryManifest. Entry point for
// the factory and backtest loop to admit, query, and lifecycle-manage alphas
// across runs.
// Full definition in library/library.hpp (S4-5).
class Library;

} // namespace atx::engine::library
