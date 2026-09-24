#pragma once

// atx::engine::alpha — cross-program / cross-worker subtree result cache (Lane 2).
//
// WHY: a GP search evaluates thousands of genomes whose offspring share most of
// their subtrees with their parents, and a WQ101-style battery shares `rank(close)`,
// `delta(close,1)`, `returns`, … across alphas. Hash-consing (dag.hpp) removes
// duplicates WITHIN one Program; this cache removes them ACROSS Programs, calls and
// workers. The research note (§1 "Cache across GP generations") is explicit: the
// hit rate dominates throughput.
//
// KEY — what a cached panel buffer depends on:
//   * the STRUCTURE of the subtree (opcode, immediates, field NAMES, pin index and
//     children, recursively) — `subtree_hashes` folds this bottom-up over a Program
//     into a 128-bit fingerprint (two independently seeded 64-bit lanes), so slot
//     numbers, field-dictionary ids and instruction positions never leak into it;
//   * the PANEL it was evaluated on — `panel_digest` (dims, field names, raw field
//     bytes, universe mask);
//   * the EvalMode tier (AuditExact vs ResearchFast produce different bits for the
//     variance family).
//   Nothing else changes a value: the Ts column pool and the Cs date pool are
//   bit-identical to the serial kernels by contract (vm.hpp S3-3 / Lane 2).
//
// COLLISIONS: the map is keyed by the full SubtreeKey (128-bit structure hash +
// panel digest + mode). A false hit needs a simultaneous collision of two
// independently seeded 64-bit structural hashes — ~2^-128 per pair, far below the
// hardware soft-error floor. Documented, not verified per lookup, because a
// verifying key would have to carry the whole subtree serialization.
//
// CONCURRENCY: internally synchronized by ONE mutex (find() also mutates the LRU
// order and the stats). Values are immutable `shared_ptr<const PanelBuf>`: a reader
// that obtained one keeps it alive even if the entry is evicted afterwards, so no
// reader ever sees a buffer change under it. Publishing from several workers is
// safe; which worker publishes first only changes the stats, never a value (every
// publisher of a key computed the same bits). Lock hold time is O(1) map work plus
// the LRU splice — never a buffer copy (copies happen outside the lock).
//
// BUDGET: `byte_budget` bounds the sum of cached buffer bytes. publish() evicts
// least-recently-used entries until the new entry fits; an entry larger than the
// whole budget is dropped (counted as `rejected`).

#include <cstddef>
#include <list>
#include <memory>
#include <mutex>
#include <span>
#include <unordered_map>
#include <vector>

#include "atx/core/types.hpp"

#include "atx/engine/alpha/bytecode.hpp" // Program, Instr
#include "atx/engine/alpha/panel.hpp"    // Panel

namespace atx::engine::alpha {

// Defined in vm.hpp (opaque declaration with its fixed underlying type).
enum class EvalMode : atx::u8;

// One cached whole-panel value (date-major, dates*instruments).
using PanelBuf = std::vector<atx::f64>;

// 128-bit structural fingerprint of a subtree (two independently seeded lanes).
struct SubtreeHash {
  atx::u64 lo{};
  atx::u64 hi{};
  [[nodiscard]] bool operator==(const SubtreeHash &) const noexcept = default;
};

struct SubtreeKey {
  SubtreeHash node_hash{};
  atx::u64 panel_digest{};
  EvalMode mode{};
  [[nodiscard]] bool operator==(const SubtreeKey &) const noexcept = default;
};

struct CacheStats {
  atx::u64 hits{};
  atx::u64 misses{};
  atx::u64 publishes{}; // entries inserted
  atx::u64 evictions{};
  atx::u64 rejected{}; // publishes dropped (larger than the budget, or duplicate key)
  std::size_t bytes{};
  std::size_t entries{};

  [[nodiscard]] double hit_pct() const noexcept {
    const atx::u64 n = hits + misses;
    return n == 0 ? 0.0 : 100.0 * static_cast<double>(hits) / static_cast<double>(n);
  }
};

namespace detail {
struct SubtreeKeyHash {
  [[nodiscard]] std::size_t operator()(const SubtreeKey &k) const noexcept;
};
} // namespace detail

class SubtreeCache {
public:
  explicit SubtreeCache(std::size_t byte_budget) noexcept : budget_{byte_budget} {}

  SubtreeCache(const SubtreeCache &) = delete;
  SubtreeCache &operator=(const SubtreeCache &) = delete;
  SubtreeCache(SubtreeCache &&) = delete;
  SubtreeCache &operator=(SubtreeCache &&) = delete;
  ~SubtreeCache() = default;

  // Lookup. A hit marks the entry most-recently-used and returns a shared,
  // immutable buffer (nullptr on a miss). Counts one hit or one miss.
  [[nodiscard]] std::shared_ptr<const PanelBuf> find(const SubtreeKey &key) const;

  // Probe WITHOUT touching the LRU order or the stats (test / planner use).
  [[nodiscard]] bool contains(const SubtreeKey &key) const;

  // Insert `buf` under `key` (moved in). A key already present is left as is (its
  // value is bit-identical by construction) and counted `rejected`. Evicts LRU
  // entries until the new bytes fit; a buffer larger than the budget is dropped.
  void publish(const SubtreeKey &key, PanelBuf &&buf);

  [[nodiscard]] CacheStats stats() const;
  [[nodiscard]] std::size_t byte_budget() const noexcept { return budget_; }

  // Drop every entry and zero the counters.
  void clear();

private:
  struct Entry {
    std::shared_ptr<const PanelBuf> buf;
    std::list<SubtreeKey>::iterator lru_pos; // position in lru_ (front == MRU)
  };

  const std::size_t budget_;
  mutable std::mutex mtx_;
  mutable std::list<SubtreeKey> lru_; // front = most recently used
  std::unordered_map<SubtreeKey, Entry, detail::SubtreeKeyHash> map_;
  mutable CacheStats stats_{};
};

// Canonical digest of a Panel: shape, field names, every raw field byte and the
// universe mask. Equal panels -> equal digests; a single flipped bit anywhere ->
// a different digest (with wyhash's collision odds). Deterministic within a process
// (hash_bytes' contract), which is all an in-process cache needs. O(cells*fields).
[[nodiscard]] atx::u64 panel_content_digest(const Panel &panel);

// Per-instruction structural subtree fingerprints for `prog`, aligned with
// `prog.code` (Free / StoreAlpha entries are zero). A compute instruction's hash
// folds its opcode, n_out, param, immediates, the NAME of a LoadField's field, and
// its producers' hashes (a Pin folds its block producer + pin index). Two
// instructions in DIFFERENT programs computing the same subtree get equal hashes.
// Writes into `out` (resized to prog.code.size()); `slot_hash` is caller scratch.
void subtree_hashes(const Program &prog, std::vector<SubtreeHash> &out,
                    std::vector<SubtreeHash> &slot_hash);

// Convenience overload allocating its own scratch.
[[nodiscard]] std::vector<SubtreeHash> subtree_hashes(const Program &prog);

// True for instructions whose result is worth caching: every compute opcode with a
// single output except the leaves (LoadField / Const) and the Pin projection, which
// are cheaper to rebuild than to copy out of a cache entry.
[[nodiscard]] bool subtree_cacheable(const Instr &in) noexcept;

} // namespace atx::engine::alpha
