#pragma once

// SnapshotPool — the process-wide, read-only, SEALED market-snapshot pool (C2).
//
// ## Why this exists
//
// `SnapshotCache` (atx/vol/backtest.hpp) is a PER-RUN object. A parameter sweep
// that replays the same corpus under 200 variants therefore deserializes the same
// 250 archives 200 times, and — because the cache starts every load with
// `std::async(std::launch::async, ...)` — spawns one OS thread per load while
// doing it. Both costs scale with the variant count and neither buys anything: a
// backtest corpus is HISTORICAL, so every variant reads byte-identical bytes.
//
// This pool is the shared, immutable substrate underneath that fan-out. One
// process-wide instance, handed to every variant through
// `RunConfig::snapshot_pool`, so N variants over the same clock open each archive
// ONCE between them.
//
// ## What it is, and what it deliberately is not
//
//   * SEALED TIERS ONLY. Every load declares `ArchiveBacking::Sealed`, i.e. the
//     snapshot keeps the archive mapped and borrows its records rather than
//     copying them. That is only legal for a corpus the caller has declared
//     read-only for the pool's lifetime, which is exactly the replay contract
//     (see `ArchiveBacking` in backtest.hpp). A pool over a corpus a SurfaceDb
//     store may still rewrite is a caller error; the pool cannot detect it and
//     does not try.
//
//   * WHOLE BOARD, never a uid subset. `SnapshotCache`'s subset-deserialize
//     (B1/WS-F F5) keys a snapshot to ONE book's referenced uids; a pool shared
//     across books with different referenced sets would serve a snapshot missing
//     another book's names. So the pool always loads the whole board — the same
//     choice `run_backtest` already makes for a caller-supplied shared cache.
//
//   * NO LRU, NO BACKGROUND EVICTION, NO BACKGROUND THREADS. The only way an
//     entry leaves is `trim(dates_before)`, called explicitly by the sweep driver
//     from its own watermark. A pool that evicted on its own would make "did this
//     date cost an archive open?" depend on wall-clock timing, which is exactly
//     the property the telemetry below is supposed to be able to prove.
//
//   * NOT A `SnapshotCache` REPLACEMENT. `RunConfig::snapshot_pool == nullptr`
//     (the default) keeps every existing caller on the private per-run cache,
//     bit-for-bit. `snapshot_cache` and `snapshot_pool` are mutually exclusive on
//     one `RunConfig` — supplying both is refused rather than silently ranked.
//
// ## Determinism (invariants I1–I8)
//
// The pool decides WHERE a run's bytes come from. It never decides WHAT they are:
//
//   * a pool hit and a cold load produce the same `MarketSnapshot` for the same
//     (archive, tier) — the archive is immutable under the Sealed declaration;
//   * `acquire` hands out `shared_ptr<const MarketSnapshot>`, so nothing a run
//     holds can be mutated or invalidated by another run, or by `trim`;
//   * no iteration over the pool's internal maps ever reaches a caller — the only
//     map traversal is inside `trim`/`clear`, whose effect is a SET of removals,
//     not an order;
//   * `warm()` reports the FIRST error in REF-INDEX order, not in completion
//     order, so a batch whose second and fifth refs are both broken names the
//     second one on every run and on every host.
//
// Consequently a run's `BacktestResult` is bit-identical with a pool, without a
// pool, warm or cold, and at any concurrency
// (`BacktestExec.SnapshotPoolIsBitIdenticalToPrivateCache` and
// `BacktestExec.SnapshotPoolConcurrentRunsMatchSerial` pin both directions).
//
// ## Locking invariants (this is the whole concurrency contract)
//
// State is split into `shard_count()` independent SHARDS, chosen by hashing the
// DATE string. A shard owns a `std::shared_mutex` and a `date -> Entry` map;
// entries are held by `shared_ptr`, so removing one from a map never invalidates
// a reader that already has it.
//
//   L1. WARM READ PATH IS SHARED-LOCK ONLY, and after the entry is in hand it is
//       LOCK-FREE. `acquire` takes `shared_lock(shard.mutex)` just long enough to
//       copy the `shared_ptr<Entry>` out of the map, then reads
//       `entry->ready` (an acquire load). A ready entry's snapshot/error fields
//       are written ONCE, before the release store that sets `ready`, and are
//       never written again — so a reader that observes `ready == true` may read
//       them with no lock at all.
//
//   L2. THE WRITE PATH IS THE FIRST LOAD OF A KEY, AND ONLY THAT. It takes
//       `unique_lock(shard.mutex)`, re-checks the map (double-checked insert),
//       and either finds a racer's entry — in which case it becomes a WAITER —
//       or inserts its own and becomes THE LOADER. Exactly one thread is ever the
//       loader for a given (key, generation), so concurrent first-acquires of one
//       date perform exactly ONE archive open (`stats().archive_opens` is the
//       proof, asserted by the 8-thread gate).
//
//   L3. NO LOCK IS HELD ACROSS I/O. The loader releases the shard lock BEFORE
//       probing the header or mapping the archive, so a slow load never blocks
//       another date, another shard, or a hit on the same shard.
//
//   L4. WAITERS BLOCK ONLY ON A RUNNING LOADER. An entry's condition variable is
//       signalled by the loader under the entry's own small mutex (never the
//       shard mutex). Because the loader is by construction a RUNNING thread that
//       itself never blocks on the pool, waiting cannot cycle — there is no
//       lock-order or wait-for cycle to construct. This is what makes it safe for
//       `warm()` to run acquisitions on `PricingExecutor` workers.
//
//   L5. LOCK ORDER, where two are held at all, is always shard THEN entry, and
//       `trim`/`clear` take one shard at a time. No code path holds two shard
//       mutexes.
//
//   L6. `stats()` reads atomics only and is therefore a consistent-per-counter,
//       not a consistent-across-counters, snapshot. Assert on it from a quiesced
//       point (all runs joined), as the gates do.
//
//   L7. A LOADER ALWAYS PUBLISHES, INCLUDING ON AN EXCEPTION. The entry is in the
//       map, not ready, for the whole duration of the load, and a waiter parked at
//       L4 has no exit but the publication. So the loader's publication runs from
//       a scope guard: if `MarketSnapshot::load`, an allocation, or a lock throws,
//       the entry is still marked ready — carrying a pre-seeded "loader did not
//       complete" error — and still evicted, so waiters get an error and the next
//       acquire retries. Without this a single `bad_alloc` on a single date would
//       wedge that key for the life of the PROCESS: waiters hang, later acquirers
//       hang, and a `warm()` whose workers are parked never reaches its join
//       barrier. It would present as a CI timeout, not a failing assertion.
//
// ## Loads route through `PricingExecutor`, never `std::async`
//
// `warm()` dispatches its missing loads with `pricing_executor().run_dynamic`, so
// a batch of dates is loaded by the ONE persistent process pool — no thread is
// created per load, and a nested call (a run that is itself executing inside
// executor work) degrades to inline rather than self-oversubscribing. `acquire`
// of a single date loads on the CALLING thread: a one-element dispatch resolves
// to exactly that anyway, and spelling it out keeps the single-date path free of
// any dispatch at all.
//
// SIZE MATTERS HERE. `run_dynamic` inlines below its own threshold of 4, so a
// look-ahead window of 1-3 refs never reaches a worker at all —
// `RunConfig::prefetch_depth`'s default of 2 is in that band. The fan-out is real
// only from depth 4 up; `SnapshotPoolStats::executor_dispatched_loads` reports
// which of the two happened.

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string_view>

#include "atx/vol/backtest.hpp"        // MarketSnapshot, SnapshotRef, ArchiveBacking
#include "atx/vol/query_pricing.hpp"   // QueryPricingTier
#include "atx/vol/surface_archive.hpp" // ArchiveContentIdentity
#include "atx/vol/types.hpp"           // Result, Status

namespace atx::vol {

// Pool telemetry. Every counter is monotone for the pool's life except
// `resident_entries`, which is the current map population.
struct SnapshotPoolStats {
  // Archives this pool actually opened and deserialized. THE counter the "second
  // run opens nothing" and "8 concurrent runs open each date once" gates assert
  // on. A failed load increments it too — the open was attempted and paid for.
  std::uint64_t archive_opens{0};
  // Acquisitions served by an entry that was already present (ready or in
  // flight), i.e. that did NOT become a loader.
  std::uint64_t hits{0};
  // Acquisitions that found an entry still loading and blocked on it. A subset of
  // `hits`; the difference between the two is how much single-flight coalescing
  // actually happened versus plain warm reuse.
  std::uint64_t coalesced_waits{0};
  // Archive content-identity probes: exactly ONE per (date, generation), read
  // before the load and cross-checked against the loaded snapshot's own identity.
  // A trimmed-then-reacquired date is a new generation and probes again.
  std::uint64_t identity_probes{0};
  // Entries currently resident across all shards.
  std::uint64_t resident_entries{0};
  // Entries removed by `trim` / `clear` over the pool's life.
  std::uint64_t trimmed_entries{0};
  // Loads that did not produce a snapshot — a reported error, OR a loader that
  // unwound (invariant L7). Either way the entry is removed rather than cached,
  // so a retry re-attempts instead of replaying a stale failure.
  std::uint64_t failed_loads{0};
  // Acquisitions issued by `warm()` that ran on a thread OTHER than the one that
  // called `warm` — i.e. real fan-out onto persistent `PricingExecutor` workers.
  //
  // LEGITIMATELY ZERO in the common case, which is exactly why it is worth
  // having. `run_dynamic` inlines below its own threshold (4, pricing_executor.cpp),
  // so a look-ahead window of 1-3 refs — what `RunConfig::prefetch_depth`'s
  // default of 2 produces — is executed entirely by the caller and never wakes a
  // worker. It is also 0 on a pool with no workers, and 0 for every acquisition
  // made outside `warm`. Non-zero is the observable that the executor route in
  // invariant L4 actually RAN, rather than being asserted only in a comment.
  std::uint64_t executor_dispatched_loads{0};
};

class SnapshotPool {
public:
  // Default shard count. Sixteen independent locks is enough that a variant
  // sweep's concurrent first-loads of DIFFERENT dates essentially never contend,
  // while keeping `trim` a 16-map walk rather than a large one.
  static constexpr std::size_t kDefaultShards = 16;

  // `n_shards` is clamped to at least 1. It is fixed for the pool's life, so the
  // shard a date lands in never moves.
  explicit SnapshotPool(std::size_t n_shards = kDefaultShards);
  ~SnapshotPool();
  SnapshotPool(const SnapshotPool &) = delete;
  SnapshotPool &operator=(const SnapshotPool &) = delete;
  SnapshotPool(SnapshotPool &&) = delete;
  SnapshotPool &operator=(SnapshotPool &&) = delete;

  // The one read entry point. Returns the pool's shared, immutable snapshot for
  // this (date, archive_path, tier), loading it Sealed + whole-board on first
  // request. Safe to call from any thread, including a `PricingExecutor` worker.
  //
  // `date` is the pool's identity for the entry and what `trim` compares against;
  // `archive_path` and `query_pricing_tier` are part of the key too, so one date
  // served at two tiers (or from two roots) never aliases.
  //
  // A load failure is reported to every waiter and the entry is REMOVED, so the
  // pool never caches a failure: a later acquire re-attempts. `failed_loads`
  // records that this happened.
  [[nodiscard]] Result<std::shared_ptr<const MarketSnapshot>>
  acquire(std::string_view date, std::string_view archive_path,
          QueryPricingTier query_pricing_tier);

  // Load every ref that is not already resident, dispatching the missing loads
  // over the persistent `PricingExecutor` (see the header note). Blocks until all
  // of them are resident. Returns the first error in REF-INDEX order — never in
  // completion order — so the reported failure is deterministic.
  //
  // Refs already resident cost a shared-lock lookup each. An empty span is Ok().
  [[nodiscard]] Status warm(std::span<const SnapshotRef> refs,
                            QueryPricingTier query_pricing_tier);

  // THE ONLY EVICTION. Removes every entry whose date sorts strictly before
  // `dates_before` (plain string ordering, which is chronological for the ISO
  // dates a corpus clock uses), and returns how many were removed.
  //
  // Callers already holding a snapshot keep it — handles are `shared_ptr` and the
  // mapped archive stays alive as long as any of them does. Trimming a date whose
  // load is still in flight is legal but wasteful: the in-flight loader still
  // publishes to its (now detached) entry and its own waiters still get the
  // answer, but a later acquire of that date starts a new generation and opens
  // the archive again. Drive this from a watermark BEHIND the sweep cursor.
  std::size_t trim(std::string_view dates_before);

  // Remove every entry (counted into `trimmed_entries`). Same live-reader safety
  // as `trim`.
  void clear();

  [[nodiscard]] SnapshotPoolStats stats() const noexcept;
  [[nodiscard]] std::size_t shard_count() const noexcept;

  // The content identity recorded for a resident date's archive, or a default
  // (all-zero) identity when the date is not resident. Diagnostic only.
  [[nodiscard]] ArchiveContentIdentity identity_of(std::string_view date,
                                                   std::string_view archive_path,
                                                   QueryPricingTier query_pricing_tier) const;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace atx::vol
