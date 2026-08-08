#pragma once

// atx-vol backtest engine (Phase B0) — the canonical forward-pass driver that
// marks a FIXED hand-built option book across a corpus of fitted-surface
// snapshots and decomposes each step's PnL into the PortfolioPricer Taylor axes.
//
// B0 is the skeleton: snapshot loader (`MarketSnapshot`), a `Clock` over a
// corpus manifest, an absolute-expiry-aged `Lot`/`PortfolioState`, and the
// resolve-today -> pnl_explain-forward -> move-swap loop that produces a SoA
// `BacktestResult` time series (PnL + attribution + book greeks). There is NO
// strategy, NO frictions, and NO cash ledger here — those arrive in B1/B2. The
// book is handed in whole and held to expiry; expiring lots settle at intrinsic
// only when the clock contains an observation at the exact expiry timestamp.
// Crossing expiry without that observation fails closed: a later spot is not a
// valid settlement price.
//
// ## Load-once invariant
//
// The engine holds exactly two live snapshots (`base` and `shifted`); after a
// step it does `base = std::move(shifted)` so the just-loaded date BECOMES the
// next base with no re-open. Over an N-ref run each archive is opened exactly
// once (`MarketSnapshot::open_count()` increments N times). The move-swap is
// pointer-safe: `std::vector` move keeps element addresses, so the non-owning
// `SurfaceSet` pointers into the owned surfaces stay valid.
//
// ## Aging
//
// Aging is delegated to `PortfolioPricer::pnl_explain`, which rolls each
// contract's T by the two surfaces' `now_ts_ns` gap. The engine therefore builds
// each step's `Portfolio` at the BASE-date residual T = (expiry - base.ts)/year;
// because the lot's `expiry_ts_ns` is fixed, `T_base - dt == T_shifted` exactly.

#include <cmath> // std::isfinite — should_exercise_early
#include <cstddef>
#include <cstdint>
#include <functional> // std::function — RunConfig::step_observer
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "atx/vol/corpus.hpp"           // CorpusManifest
#include "atx/vol/derivatives.hpp"      // DerivKind, RealizedVarianceSpec (the swap lane)
#include "atx/vol/detail/aggregate_arity.hpp" // RunConfig field-count drift pin
#include "atx/vol/portfolio_pricer.hpp" // OptionContract, SurfaceSet, PriceOptions, PriceTotals
#include "atx/vol/priced_surface.hpp"   // PricedSurface
#include "atx/vol/query_pricing.hpp"    // QueryPricingTier
#include "atx/vol/surface_archive.hpp"  // SurfaceProvenance
#include "atx/vol/types.hpp"            // Result, Side

namespace atx::vol {

class IStrategy; // strategy.hpp — drives the strategy-aware run_backtest overload
class SurfaceDb; // surface_db.hpp — Clock::from_surface_db source
// research/snapshot_pool.hpp — the process-wide sealed snapshot pool (C2).
// FORWARD-DECLARED, never included: this header is Tier-A and closed under
// inclusion (vol_umbrella_test.cpp), and the pool is a research-tier type.
// `RunConfig::snapshot_pool` is a non-owning pointer, so the declaration is all
// a caller who does not use the pool ever needs.
class SnapshotPool;

// ── Timeline ────────────────────────────────────────────────────────────────

// One dated market snapshot in the backtest timeline: the corpus date string and
// the on-disk archive that holds that date's fitted surface(s).
struct SnapshotRef {
  std::string date;
  std::string archive_path;
};

// The backtest timeline enumerated from a corpus manifest: one `SnapshotRef` per
// unique date (ascending) pointing at that date's first Ok archive. A
// SurfaceDb-backed route is also available (`from_surface_db` below), one
// `SnapshotRef` per partition instead of per manifest entry.
class Clock {
public:
  [[nodiscard]] static Result<Clock> from_manifest(const CorpusManifest &manifest);

  // Build a clock over a SurfaceDb: one SnapshotRef per partition, ordered by
  // ascending partition key (keys are canonical uppercase; ISO dates like
  // "2026-07-11" sort chronologically). ref.date = partition key,
  // ref.archive_path = "<root>/partitions/<KEY>.atxvsa". InvalidArgument if
  // the db has no partitions. The snapshot files are ordinary ATXVSA
  // archives, so MarketSnapshot::load / SnapshotCache consume the refs
  // unchanged.
  [[nodiscard]] static Result<Clock> from_surface_db(const SurfaceDb &db);

  // Subset this clock to the refs whose date lies in [date_lo, date_hi] —
  // INCLUSIVE on both ends. Dates are compared as plain strings, which is
  // chronological for the canonical ISO "YYYY-MM-DD" partition keys
  // `from_surface_db` produces (and for the corpus dates `from_manifest` does).
  // Refs are copied whole, so the subset keeps each ref's archive_path and its
  // ascending order; `*this` is unchanged.
  //
  // Out-of-range bounds CLAMP: a window wider than the corpus is the whole
  // corpus, not an error — an operator asking for "2020..2030" wants everything
  // there is. InvalidArgument when the window selects NO ref (date_lo > date_hi,
  // or a real gap between two dates the corpus does not cover); the message
  // names the available range so the caller can self-serve the correction.
  [[nodiscard]] Result<Clock> between(std::string_view date_lo, std::string_view date_hi) const;

  // BORROW of the timeline vector this `Clock` owns — including the `date` and
  // `archive_path` strings inside each `SnapshotRef`. A clock is immutable once a
  // `from_*` factory has returned it (nothing rewrites `refs_`), so the span is
  // valid for the clock's lifetime and concurrent const readers are safe.
  // INVALIDATION: destroying the clock, or assigning over it — the type is
  // COPYABLE as well as movable, and a span taken from one clock never names the
  // copy's storage. A step loop that outlives the clock (or that stores refs past
  // a rebuild) must copy them out, not keep this span.
  [[nodiscard]] std::span<const SnapshotRef> refs() const noexcept { return refs_; }
  [[nodiscard]] std::size_t size() const noexcept { return refs_.size(); }

  // Task A6 (backtest-lakehouse sprint): dates from the manifest's `dates` list
  // that `from_manifest` could NOT place a ref for -- no entry on that date had
  // `status == CorpusFitStatus::Ok` with a non-empty `archive_path`. Ascending,
  // matching `dates`' own order. Before this task these dates were dropped with
  // no record at all, so the run silently spanned the gap as one ordinary step
  // (fit-survivorship); they are always recorded now, regardless of
  // `RunConfig::clock_gaps`, which only governs whether a non-empty list here
  // is tolerated or refused. Always EMPTY for a clock built by
  // `from_surface_db` (a SurfaceDb partition list has no separate
  // "planned but not admitted" notion to compare against). `between()` carries
  // forward only the dropped dates that fall inside the requested window, so a
  // sliced clock's own gap accounting describes exactly what IT spans.
  [[nodiscard]] std::span<const std::string> dropped_dates() const noexcept {
    return dropped_dates_;
  }

private:
  std::vector<SnapshotRef> refs_;
  std::vector<std::string> dropped_dates_;
};

// ── Archive backing (WS-ZC1) ────────────────────────────────────────────────
//
// How a snapshot that BORROWS its surfaces obtains the archive bytes those views
// read. It is a statement about the FILE'S LIFECYCLE, made by the caller, because
// only the caller knows it — not something the loader can infer.
//
//   Mutable (default, safe): map, copy once into an owned buffer, DROP the mapping.
//     For any archive something may still rewrite, evict, or delete — above all the
//     `SurfaceDb` store path (write -> reopen -> rewrite/evict/delete). A resident
//     `atx::tsdb::Mapping` keeps the file open, and on Windows those operations then
//     fail with a sharing violation; commit 8627ccb reverted exactly that regression
//     (~22 SurfaceDb/SurfaceDbPopulate tests) and `SnapshotCacheEvictsStaleEntry-
//     WhenArchiveRewrittenSameLength` pins it. This backing costs a whole-archive
//     copy, which scales with archive BYTES while the reconstruction it replaces
//     scales with SURFACES — so it is a net loss on short runs.
//
//   Sealed: keep the mapping for the snapshot's lifetime — no copy at all. ONLY for
//     an immutable, read-only corpus: a historical replay, where nothing rewrites or
//     deletes a partition mid-run. This is where the WS-ZC1 win actually lives
//     (~9x on snapshot_load); it is opt-in precisely because it pins the file.
//     Loaded through `SnapshotCache`, the same declaration is trusted further: the
//     cache memoizes each archive's content-identity check for the CACHE's whole
//     lifetime, not just one snapshot's (see `SnapshotCache::archive_backing`).
//
// Choosing Sealed for a partition the store may mutate does not corrupt data — the
// mapped bytes stay coherent — but it WILL make the store's rewrite/delete fail. Ask
// for Sealed only when the corpus is genuinely read-only for the snapshot's lifetime.
enum class ArchiveBacking : std::uint8_t {
  Mutable = 0,
  Sealed = 1,
};

// ── Snapshot loader ─────────────────────────────────────────────────────────

// A single loaded market date: owns the archive's `PricedSurface`s and their
// same-blob provenance, and holds a non-owning `SurfaceSet` over the surfaces'
// stable addresses. Move-only; the move leaves the `SurfaceSet` pointers valid
// (vector move preserves element addresses and `surfaces_` is never mutated
// after `set_` is built).
class MarketSnapshot {
public:
  // Open `archive_path`, deserialize its surfaces, prepare their runtime-only query
  // tier, build the `SurfaceSet`, and take the valuation timestamp from the surfaces'
  // `now_ts_ns` (validating they agree). The archive wire format is unchanged.
  // Errors propagate from open/map/query preparation or `SurfaceSet::create`;
  // InvalidArgument if the archive is empty or its surfaces disagree on the ts.
  //
  // B1 subset-deserialize: `referenced_uids`, when non-empty, restricts the
  // deserialize to the archive directory entries whose uid is referenced (dropping
  // the whole-board `reconstruct_all_with_provenance`); if none match, the snapshot
  // contains an empty SurfaceSet and reads only timestamp metadata from one mapped
  // record. Empty (the default) keeps the whole-board load. `uid_of` still
  // resolves every archived name regardless. NB: while the pricer's SurfaceSet takes
  // `const PricedSurface*` (seam §6), the subset is reconstructed owned, not served
  // zero-copy — the win is the reduced surface count, not zero-allocation.
  // `backing` states this archive's lifecycle (see ArchiveBacking). It affects only
  // HOW the borrowed bytes are obtained — the resulting snapshot is byte-identical
  // either way. Defaults to Mutable: safe for every caller, including the SurfaceDb
  // store path; a read-only replay corpus opts into Sealed.
  [[nodiscard]] static Result<MarketSnapshot>
  load(std::string_view archive_path,
       QueryPricingTier query_pricing_tier = QueryPricingTier::LegacyCompatible,
       std::span<const std::uint32_t> referenced_uids = {},
       ArchiveBacking backing = ArchiveBacking::Mutable);

  MarketSnapshot(MarketSnapshot &&) noexcept = default;
  MarketSnapshot &operator=(MarketSnapshot &&) noexcept = default;
  MarketSnapshot(const MarketSnapshot &) = delete;
  MarketSnapshot &operator=(const MarketSnapshot &) = delete;

  [[nodiscard]] const SurfaceSet &set() const noexcept { return set_; }
  // WS-ZC1: a `SurfaceRef` handle — the resolved surface may be OWNED (a freshly
  // fit board) or a zero-copy BORROW of this snapshot's mapped archive record.
  // Pointer-style use (`s->pricing()`, `s != nullptr`) is unchanged.
  [[nodiscard]] SurfaceRef find(std::uint32_t uid) const noexcept { return set_.find(uid); }
  // Same-archive provenance for uid. Pointer lifetime matches this snapshot;
  // lookup performs no allocation and returns nullptr for an unknown uid.
  [[nodiscard]] const SurfaceProvenance *provenance(std::uint32_t uid) const noexcept;
  // Provenance aligned one-for-one with surfaces() in archive order. The view is
  // invalidated only when this snapshot is destroyed or moved from.
  [[nodiscard]] std::span<const SurfaceProvenance> provenances() const noexcept {
    return provenance_;
  }
  // Content-derived identity of the archive opened by load(). It is available
  // for both owned and borrowed surface backings and remains stable across moves.
  [[nodiscard]] ArchiveContentIdentity source_identity() const noexcept { return source_identity_; }
  // Read-only view of the OWNED surfaces (archive order). EMPTY on a borrowed
  // (zero-copy) load — use `n_surfaces()` / `surface_at()` for backing-agnostic
  // access. Safe across a move (vector move preserves element addresses).
  [[nodiscard]] std::span<const PricedSurface> surfaces() const noexcept { return surfaces_; }
  // Read-only view of the BORROWED surfaces (archive order). Empty on an owned load.
  [[nodiscard]] std::span<const PricedSurfaceView> views() const noexcept { return views_; }
  // True when this snapshot borrows its archive's mapped records rather than owning
  // reconstructed surfaces (WS-ZC1).
  [[nodiscard]] bool borrows_views() const noexcept { return !views_.empty(); }

  // Backing-agnostic surface access (archive order), whichever backing was chosen.
  //
  // NOT always non-empty (this used to claim it was): `load`'s subset-miss path —
  // a requested uid subset that matched no directory entry — deliberately keeps an
  // EMPTY SurfaceSet rather than falling back to a whole-board read, so a
  // successfully loaded snapshot can legally own zero surfaces. Callers that want
  // "the date's first surface" must check `n_surfaces()` (or the null handle below)
  // before using it.
  [[nodiscard]] std::size_t n_surfaces() const noexcept {
    return views_.empty() ? surfaces_.size() : views_.size();
  }
  // Out of range (which includes EVERY index of a zero-surface snapshot) yields a
  // NULL handle — `ref == nullptr`, checkable exactly like `find()`'s result —
  // instead of indexing an empty backing out of bounds.
  [[nodiscard]] SurfaceRef surface_at(std::size_t i) const noexcept {
    if (i >= n_surfaces()) {
      return SurfaceRef{};
    }
    return views_.empty() ? SurfaceRef{&surfaces_[i]} : SurfaceRef{&views_[i]};
  }

  [[nodiscard]] std::int64_t ts_ns() const noexcept { return ts_ns_; }
  [[nodiscard]] std::optional<std::uint32_t> uid_of(std::string_view symbol) const;

  // Test seam: how many archives have been opened process-wide (the load-once
  // gate resets this, runs, and asserts it equals the ref count).
  [[nodiscard]] static std::uint64_t open_count() noexcept;
  static void reset_open_count() noexcept;

  // WS-F F5 (BT-T2): process-wide SURFACE-RECORD BYTES materialized by loads —
  // the sum of the archive directory entries' `surface_size` over every record a
  // load turned into a surface or a view. Deterministic by construction (the
  // same number on a busy host as on a quiet one), so a subset-vs-whole-board
  // claim is a fact and not a timing measurement. Counts record bytes, not
  // resident pages: on the mapped tiers the OS faults in only what is read, so
  // this is the upper bound the subset shrinks.
  [[nodiscard]] static std::uint64_t deserialized_bytes() noexcept;
  static void reset_deserialized_bytes() noexcept;

private:
  MarketSnapshot(std::shared_ptr<const SurfaceArchiveV2> archive,
                 std::vector<PricedSurface> &&surfaces, std::vector<PricedSurfaceView> &&views,
                 std::vector<SurfaceProvenance> &&provenance, SurfaceSet &&set, std::int64_t ts,
                 ArchiveContentIdentity source_identity,
                 std::vector<std::pair<std::string, std::uint32_t>> &&syms) noexcept;

  // ── LIFETIME (WS-ZC1) ──────────────────────────────────────────────────────
  // On the borrowed path each `PricedSurfaceView` in `views_` points into the
  // memory mapping that `archive_` co-owns, and `set_` holds `SurfaceRef`s to the
  // `views_` elements. That makes the ownership a strict chain:
  //
  //     archive_ (owns the Mapping)  <--  views_ (borrow its bytes)  <--  set_
  //
  // DECLARATION ORDER BELOW IS THE ENFORCEMENT. Members are destroyed in reverse
  // declaration order, so `set_` dies first, then `views_`, and `archive_` — the
  // mapping — dies LAST. A view therefore cannot outlive the bytes it reads, and
  // the snapshot cannot hand out a `SurfaceRef` into a released mapping. Do not
  // reorder these four members.
  //
  // The archive is held by `shared_ptr<const ...>` so a snapshot is movable while
  // keeping the mapping pinned for every copy of the handle. It is null on the
  // owned path, where `surfaces_` is self-contained.
  //
  // Move safety: moving a vector transfers its heap buffer, so element ADDRESSES
  // are preserved and `set_`'s refs stay valid across a `MarketSnapshot` move.
  // Neither `surfaces_` nor `views_` is ever mutated after `set_` is built.
  std::shared_ptr<const SurfaceArchiveV2> archive_; // mapping owner (borrowed path only)
  std::vector<PricedSurface> surfaces_;             // owned path (archive directory order)
  std::vector<PricedSurfaceView> views_;            // borrowed path (archive directory order)
  std::vector<SurfaceProvenance> provenance_;       // same-blob, parallel to whichever is populated
  SurfaceSet set_;                                  // non-owning over surfaces_ OR views_
  ArchiveContentIdentity source_identity_{};
  std::int64_t ts_ns_{0};
  std::vector<std::pair<std::string, std::uint32_t>> syms_; // symbol -> uid
};

// ABI note: this pre-1.0 hot-path revision changes the IStrategy vtable and the
// public ResolvedLeg, MarketSnapshot, SnapshotCacheStats, and RunConfig layouts.
// Rebuild the atx-vol DLL and every consumer together; binary compatibility with
// earlier headers is not promised.
struct SnapshotCacheStats {
  std::uint64_t loads{0};
  std::uint64_t hits{0};
  std::uint64_t prefetches{0};
  std::uint64_t retained_entries{0};
  std::uint64_t evictions{0};
  // Appended route telemetry. fast_build_loads counts fast snapshot loads
  // actually started, not the number of per-surface accelerator pairs.
  // reuse_only_fast_hits counts requested fast entries found by load OR
  // prefetch, including in-flight/failed candidates; a failed candidate that
  // ultimately serves cold increments reuse_only_cold_resolutions as well.
  std::uint64_t fast_build_loads{0};
  std::uint64_t reuse_only_fast_hits{0};
  std::uint64_t reuse_only_cold_resolutions{0};
};

// Thread-safe archive cache. Loads are coalesced by normalized path AND query
// tier, so a synchronous caller and an asynchronous prefetch with the same
// accuracy contract share one archive open/map. Distinct tiers never alias.
// The default constructor is unbounded for deliberate reuse across repeated
// backtest/reconciliation sweeps. Passing a positive capacity selects a
// deterministic INSERTION-ORDER (FIFO) eviction mode for one-pass pipelines —
// deliberately not least-recently-used: a forward-only sweep never revisits a
// date, so recency ranks the dates it has already passed above the read-ahead
// entries it is about to need (sequential flooding), and the cache then evicts
// completed prefetches and reloads them. Automatic eviction never removes an
// in-flight future; bounded mode can therefore exceed its target temporarily
// while every candidate is loading, then trims on the next cache operation.
// Copies share both entries and the configured mode.
class SnapshotCache {
public:
  // Reusable/unbounded mode.
  SnapshotCache();
  // Reusable/unbounded mode over an explicitly-declared archive lifecycle.
  explicit SnapshotCache(ArchiveBacking backing);
  // Bounded LRU mode. A zero capacity is defensively normalized to one entry.
  explicit SnapshotCache(std::size_t max_retained_entries,
                         ArchiveBacking backing = ArchiveBacking::Mutable);
  // B1: bounded LRU + subset-deserialize. Every load/prefetch through this cache
  // deserializes ONLY the archive surfaces whose uid appears in `referenced_uids`
  // (empty => whole board). Intended for a PRIVATE per-run cache whose single book's
  // referenced uids are known up front (the fixed-book run_backtest overload); do NOT
  // share such a cache across books with different referenced sets (a subset snapshot
  // cached under one book would be missing another's uids).
  SnapshotCache(std::size_t max_retained_entries, std::vector<std::uint32_t> referenced_uids,
                ArchiveBacking backing = ArchiveBacking::Mutable);
  ~SnapshotCache();
  SnapshotCache(const SnapshotCache &) noexcept = default;
  SnapshotCache &operator=(const SnapshotCache &) noexcept = default;
  SnapshotCache(SnapshotCache &&) noexcept = default;
  SnapshotCache &operator=(SnapshotCache &&) noexcept = default;

  void prefetch(std::string archive_path,
                QueryPricingTier query_pricing_tier = QueryPricingTier::LegacyCompatible);
  // ReuseOnly checks the requested fast-tier entry atomically and otherwise
  // prefetches ColdReference under its own key. This function never publishes a
  // cold snapshot under a fast identity.
  [[nodiscard]] Status prefetch(std::string archive_path, QueryPricingTier query_pricing_tier,
                                QueryCacheBuildPolicy build_policy);
  [[nodiscard]] Result<std::shared_ptr<const MarketSnapshot>>
  load(std::string_view archive_path,
       QueryPricingTier query_pricing_tier = QueryPricingTier::LegacyCompatible);
  // ReuseOnly returns an existing/in-flight fast snapshot when available and a
  // separately-keyed ColdReference snapshot on a fast miss. Concurrent callers
  // requesting the same effective key coalesce on one shared load.
  [[nodiscard]] Result<std::shared_ptr<const MarketSnapshot>>
  load(std::string_view archive_path, QueryPricingTier query_pricing_tier,
       QueryCacheBuildPolicy build_policy);
  // The lifecycle this cache's loads declare (see ArchiveBacking). FIXED AT
  // CONSTRUCTION and immutable thereafter — there is deliberately no setter.
  //
  // WHY NO SETTER (WS-ZC regression). The backing is part of the cache key, so an
  // entry loaded under one backing is never served under the other. A mid-flight
  // `set_archive_backing` therefore does not "retune" a cache — it ORPHANS every
  // entry already in it. `run_backtest` used to call exactly that, unconditionally,
  // on a cache the CALLER owned and had preloaded: the run silently re-loaded every
  // preloaded snapshot, and a ReuseOnly fast request then missed and silently
  // resolved down to ColdReference. Because copies of a SnapshotCache share one
  // Impl, a setter is reachable from any handle and can retarget a cache the caller
  // is still using. Binding the backing to construction makes that class of bug
  // unrepresentable, and makes the field a plain immutable read for `cache_key`
  // (which reads it off the mutex).
  //
  // Opt into Sealed by CONSTRUCTING a cache with it, and only for a corpus that is
  // read-only for the cache's whole lifetime.
  [[nodiscard]] ArchiveBacking archive_backing() const noexcept;

  [[nodiscard]] SnapshotCacheStats stats() const noexcept;
  void clear();

private:
  struct Impl;
  std::shared_ptr<Impl> impl_;
};

// ── Book state (absolute-expiry aging) ──────────────────────────────────────

// One open lot. At the engine boundary K must be finite and positive, T and qty
// finite, multiplier finite and positive, entry_price finite and nonnegative,
// side valid, and expiry_ts_ns strictly after the current base timestamp.
// `contract.T` is re-derived each step as (expiry_ts_ns - base.ts)/year;
// `expiry_ts_ns` is the fixed anchor that drives both aging and settlement.
struct Lot {
  std::uint64_t id{0};
  OptionContract contract{};
  double qty{0.0};
  double multiplier{100.0};
  std::int64_t expiry_ts_ns{0};
  std::uint32_t cohort{0};
  double entry_price{0.0};

  [[nodiscard]] bool operator==(const Lot &) const = default;
};

// ── Vol-derivative (swap) lane ──────────────────────────────────────────────
//
// One OTC variance/volatility-swap lot. ADDITIVE to the option lane: a book with
// no swap lots prices, accrues and settles exactly as it did before this lane
// existed, and both swap result columns are exactly 0.0 (the engine early-outs
// on an empty `PortfolioState::swap_lots` — no pricing, no allocation).
//
// IMMUTABLE once emitted by a strategy: the same transition check that pins
// option lots bit-compares every field of a surviving swap lot, and a new lot's
// `id` must come from the engine's monotonic `next_lot_id` watermark (shared
// with option lots, so the two id spaces never collide). A strategy opens one by
// appending to `book.swap_lots` in `on_step`.
//
// NO EARLY CLOSE IN v1 — swaps are HELD TO EXPIRY. A strategy that ERASES a swap
// lot from `book.swap_lots` gets InvalidArgument, not a close: there is no unwind
// price for an OTC swap here and nowhere to book one, so an erased lot would
// leave its last mark in NAV with nothing to offset it, orphan its accrual (which
// makes the checkpoint unresumable), and drop `swap_pv` by a mark that never
// settled. The engine removes a lot only by SETTLING it, which happens in the
// swap pass before `on_step` ever sees the book — so every lot present when the
// strategy is called must still be there when it returns.
//
// Running fixing/accrual state lives in the ENGINE (`SwapAccrual`), NEVER on the
// lot — that is what makes the lot bit-comparable across a step.
//
// ENTRY ECONOMICS v1: a swap opens at ZERO COST. There are no frictions on this
// lane (no spread, no per-contract cost, no impact) and no premium moves at
// entry — whether the strike is fair is the strategy's problem. The accrual's
// `prev_pv` therefore starts at 0.0, so the WHOLE first mark (the entry PV
// difference against the chosen strike) lands in that step's `swap_pnl`.
//
// FIXING SEED: the swap pass runs only inside the step loop, so the FIRST STEP
// that sees a lot seeds its fixing series from that step's snapshot spot and
// accrues no return. A lot opened at inception therefore seeds on refs[1] and
// needs N+1 steps to accrue N returns.
//
// EVERY STEP A LOT IS ALIVE TAKES A FIXING, INCLUDING ITS EXPIRY DAY — the
// expiry close IS that contract's final fixing. So an absent surface fails
// closed on BOTH sides of expiry: NotFound for a held lot (it cannot be marked)
// and NotFound for a settling one (it cannot take its terminal fixing, and
// settling anyway would divide a short Σr² by a denominator one observation
// light). This mirrors the option lane, which likewise refuses to settle a lot
// whose surface is missing.
//
// SETTLEMENT needs no PRICING — the terminal rate is read off the accrual, not
// solved: `rv_done_dec` (the Σr²/n_done estimator), square-rooted for the vol
// kinds and capped for the capped ones, less `strike_dec`, times `qty *
// notional`. A clock coarser than the fixing schedule therefore settles on the
// returns it actually observed, which is a real estimate; a series with NO
// observed return is not, and that degenerate case is NotFound rather than a
// strike-wide payout.
struct SwapLot {
  std::uint64_t id{0};
  std::uint32_t uid{0};
  DerivKind kind{DerivKind::VarSwap};
  double strike_dec{0.0};
  double cap_dec{0.0}; // > 0 required on a capped kind; must be 0 otherwise
  double notional{0.0};
  double qty{1.0};
  // INFORMATIONAL ONLY — the engine seeds on FIRST SIGHT, never off this stamp.
  // It is validated (must not be after `expiry_ts_ns`) and carried for
  // provenance/reporting; changing it changes no engine arithmetic.
  std::int64_t start_ts_ns{0};
  std::int64_t expiry_ts_ns{0}; // exact-match settlement, option convention
  std::uint32_t n_obs_total{0};
  double annualization{252.0};

  [[nodiscard]] bool operator==(const SwapLot &) const = default;
};

// The open book across all cohorts. Plain for B0 (no cash/shares ledger yet);
// `swap_lots` is the additive vol-derivative lane and defaults to empty.
class PortfolioState {
public:
  std::vector<Lot> lots;
  std::vector<SwapLot> swap_lots;

  [[nodiscard]] bool operator==(const PortfolioState &) const = default;
};

// Engine-owned per-swap-lot running state: the fixing series and yesterday's
// mark. A checkpointable POD, keyed to its lot by `lot_id`.
//
// The accrual arithmetic is `RealizedTracker`'s, transcribed rather than
// delegated so the state stays a trivially serializable struct:
//
//   r = ln(S/prev_spot); sum_sq += r*r; ++n_done;
//   rv_done_dec = annualization * sum_sq / n_done
//
// TWO DELIBERATE DEVIATIONS from `RealizedTracker::observe_dated`:
//   * a fixing past `n_obs_total` is a NO-OP (the contract is fully observed and
//     simply stops accruing — `prev_spot`/`prev_ts_ns` freeze too), where the
//     tracker returns InvalidArgument. A backtest clock keeps delivering dates
//     after the last fixing; that is not a caller error.
//   * ordering is still validated FIRST and unconditionally: `ts_ns <=
//     prev_ts_ns` on a seeded accrual returns AlreadyExists and mutates nothing,
//     so a replayed snapshot date can never double-count a fixing.
struct SwapAccrual {
  std::uint64_t lot_id{0};
  RealizedVarianceSpec rv{};
  double prev_spot{0.0};
  std::int64_t prev_ts_ns{0};
  bool have_prev{false};
  // Yesterday's QTY-SCALED mark (qty * DerivQuote::pv), 0.0 before the first
  // mark. Qty-scaled so `swap_pnl` and `swap_pv` are both position dollars.
  double prev_pv{0.0};

  // RealizedVarianceSpec is a C-ABI mirror with no comparison operator, so this
  // is spelled out rather than defaulted.
  [[nodiscard]] bool operator==(const SwapAccrual &other) const noexcept {
    return lot_id == other.lot_id && rv.annualization == other.rv.annualization &&
           rv.n_obs_total == other.rv.n_obs_total && rv.n_obs_done == other.rv.n_obs_done &&
           rv.sum_sq_log_returns_done == other.rv.sum_sq_log_returns_done &&
           rv.rv_done_dec == other.rv.rv_done_dec &&
           rv.include_dividend_adjustment == other.rv.include_dividend_adjustment &&
           prev_spot == other.prev_spot && prev_ts_ns == other.prev_ts_ns &&
           have_prev == other.have_prev && prev_pv == other.prev_pv;
  }
};

// ── EXERCISE MODEL: what this engine does and does NOT simulate (WS-F F3) ────
//
// EXPIRY SETTLEMENT is the ONLY UNCONDITIONAL exercise event. Every American
// lot is carried to its `expiry_ts_ns` and cash-settled at intrinsic against
// the spot observed at the exactly-matching snapshot. Before Task B3
// (backtest-lakehouse sprint) there was deliberately no assignment or
// early-exercise simulation at all:
//
//   * a short ITM call over an ex-date was never assigned away;
//   * a long deep-ITM put's optimal early exercise was never taken. Its
//     forgone value IS carried in the mark (the pricer is American), but it
//     was never REALIZED, so a hold-to-expiry book gave the early-exercise
//     premium back at expiry instead of capturing it.
//
// Task B3 closes part of that gap with `ExercisePolicy` (below), split into an
// always-safe diagnostic and an opt-in simulation:
//
//   `Advisory` (DEFAULT, behaviour-preserving): every step counts, but never
//   converts, the lots meeting the early-exercise/assignment boundary --
//   `BacktestResult::n_short_calls_assignable` /
//   `n_puts_exercisable`. A default-constructed `RunConfig` is therefore
//   BIT-IDENTICAL to the pre-B3 engine on every existing column; the two new
//   counters are pure observation.
//
//   `Simulate` (opt-in): the boundary lot is converted to the hedge SHARE
//   ledger at INTRINSIC on the ex-dividend-preceding close (calls) or the
//   step it first meets the carry boundary (puts) -- see
//   `should_exercise_early` and `backtest.cpp`'s `apply_early_exercise`. The
//   share-side economics reuse the SAME `HedgeLedger` / `FinancingConfig`
//   machinery a strategy's own delta hedge does (Task A5): the resulting
//   shares carry financing, discrete dividends and MTM exactly like any other
//   hedge-ledger position from the conversion step onward. ONLY the
//   strategy-aware overload can simulate -- it alone owns a cash/share
//   ledger; the fixed-book (B0) overload fails closed
//   (`ErrorCode::InvalidArgument`) if `Simulate` is requested, the same
//   "no honest place to book it" refusal `PortfolioState::swap_lots` and
//   `RunConfig::step_observer` already use there.
//
// Roll-at-N-DTE strategies (the dispersion route) mostly dodge the underlying
// problem because they close well before expiry. `HoldToExpiry` /
// `CloseAtHorizon` DSL books, and any `Advisory`-policy run, do NOT: read their
// settlement PnL as a lower bound on an optimally-exercised book.
//
// STILL OUT OF SCOPE (Task B3 is deliberately narrow -- YAGNI): no
// corporate-action handling (`Lot` is immutable by design, so a split
// in-window would change K and multiplier with no adjustment); no
// pin/counterparty-level assignment allocation (a short position is either
// fully converted or not at all, never partially); no continuous-dividend
// early-exercise boundary (`Simulate`'s call lane keys off the DISCRETE
// `FinancingConfig::share_dividends` schedule only, matching A5/F3(b)'s own
// discrete-dividend design).
//
// What was already modelled, as of WS-F, independent of B3: discrete cash
// dividends on the HEDGE SHARE ledger (`FinancingConfig::share_dividends`) —
// the leg of P1-4 that needed no exercise model, and the schedule B3's call
// lane reuses verbatim.

// Task B3 (backtest-lakehouse sprint, target 1.1.0): governs
// `RunConfig::exercise_policy`. See the EXERCISE MODEL block above for the
// full behavioural contract; `Advisory` is the DEFAULT and is
// behaviour-preserving (diagnostic counters only, never mutates the book or
// any ledger).
enum class ExercisePolicy : std::uint8_t {
  Advisory = 0,
  Simulate = 1,
};

// Task B3: the exercise/assignment decision rule, factored out as a PURE
// function so it is unit-testable independent of the engine's per-step
// machinery (same rationale as `MarginBreachPolicy`'s placement note below —
// this lives in backtest.hpp rather than a dedicated header because it has no
// Tier-B dependency to promote: every parameter is a `double`).
//
// `intrinsic` is the lot's current intrinsic value; a non-ITM lot
// (`intrinsic <= 0.0`) is never exercise-optimal and this always returns
// false for one. `extension_value` is the lot's remaining time value (mark -
// intrinsic; must be finite and >= 0 for a sane American mark, else this
// returns false rather than let a solver artifact drive a decision).
// `threshold` is what the extension is compared against:
//
//   * a short call's assignment risk over an ex-date: the discrete forward
//     dividend a long counterparty would capture by exercising before the
//     ex-date (`FinancingConfig::share_dividends`, WS-F F3(b)/A5) -- the
//     caller passes 0 (never firing) when no ex-date falls in this step's
//     window;
//   * a deep-ITM put's exercise opportunity, either side of the book: the
//     interest-carry benefit of collecting the strike now instead of at
//     expiry, `K * (1 - exp(-r*T))` -- the caller computes this from the
//     board's own `r` and the lot's residual `T`, both already read for other
//     per-step arithmetic (A5's financing-rate lookup, `compute_step`'s own
//     `residual_T`).
//
// Exercise/assignment is optimal exactly when the remaining time value is
// LESS than what early action would capture; a non-finite or non-positive
// threshold never fires (there is nothing to capture).
[[nodiscard]] inline bool should_exercise_early(double intrinsic, double extension_value,
                                                double threshold) noexcept {
  return intrinsic > 0.0 && std::isfinite(extension_value) && extension_value >= 0.0 &&
         std::isfinite(threshold) && threshold > 0.0 && extension_value < threshold;
}

// ── Execution frictions + financing (Phase B2) ───────────────────────────────

// Modeled bid/ask + costs applied ONLY to traded quantity (entries, roll-closes,
// hedge shares); holding accrues financing only. PricedSurface is a fitted MID
// surface (no stored bid/ask), so the spread is a documented model. The default
// (`SpreadKind::None`, all costs 0) reproduces the frictionless run bit-for-bit.
struct FrictionModel {
  // B1 (backtest-lakehouse sprint, target 1.1.0): `QuoteSide` APPENDED at the
  // end (same additive-enum treatment as every `RunConfig` knob this sprint
  // adds — see the `aggregate_arity_is_v<RunConfig, 19>` note below). It fills
  // at `mid ± f(leg_count)·half_spread` instead of charging a synthetic spread
  // ON TOP of the model mid: `quote_lookup` supplies the recorded NBBO when the
  // caller has one, `half_spread_bps` (the existing PriceBps knob) supplies the
  // fallback half-spread when it does not. See `quote_lookup`,
  // `crossing_fraction_single/_complex` below and `execute()` (backtest.cpp).
  enum class SpreadKind : std::uint8_t { None = 0, PriceBps = 1, VolTicks = 2, QuoteSide = 3 };
  SpreadKind spread_kind{SpreadKind::None};
  double half_spread_bps{0.0}; // PriceBps: half-spread = mark * bps/1e4 (per share)
  double vol_tick{0.0};        // VolTicks: half-spread = vega * vol_tick (per share)
  // REVIEW C-4. A size/participation-driven impact term, as a FRACTION of the
  // mark, charged PER SHARE and IN ADDITION to whichever `spread_kind` lane is
  // selected — including `None`, which is how a pure-impact run is expressed.
  //
  // It is its OWN lane rather than a `half_spread_bps` addend because the spread
  // and the impact scale on different quantities (vega vs price), so folding one
  // into the other is exact only when both happen to be price-proportional. The
  // dispersion route used to fold unconditionally and thereby DELETED a
  // configured `vol_tick` whenever impact was active; see
  // `dispersion_effective_frictions`. Additivity is the semantics
  // dispersion_backtest.hpp's `fill_price` already documents:
  // `mid + direction * (half_spread + impact)`.
  //
  // 0.0 (the default) is bit-identical to the pre-C-4 engine: the added term is
  // exactly `mark * 0.0`.
  double impact_fraction{0.0};
  double per_contract_cost{0.0};  // $ per option contract traded
  double hedge_slippage_bps{0.0}; // shares fill at S * (1 +/- bps/1e4)

  // B1: one recorded two-sided quote for an option leg, returned by
  // `quote_lookup` below. Usable only when both sides are finite, `bid > 0.0`
  // and `ask >= bid`; anything else (including an unset `quote_lookup`) is
  // treated exactly like an absent quote by `execute()` — it falls back to the
  // modeled PriceBps half-spread around that fill's own model mark, so a
  // `QuoteSide` run with no (or a broken) quote source degrades to precisely
  // the PriceBps-modeled economics rather than a silently wrong price.
  struct RawQuote {
    double bid{0.0};
    double ask{0.0};
  };
  // B1: optional per-fill recorded-quote source for `SpreadKind::QuoteSide`.
  // `execute()` (backtest.cpp) calls this once per entry, roll-close, and close
  // fill on an option leg — never for hedge shares, which keep their own
  // `hedge_slippage_bps` — with that leg's exact contract. Unset (the default,
  // a falsy `std::function`) means every `QuoteSide` fill takes the modeled
  // fallback; inert under every OTHER `spread_kind`. The listed-quote route
  // (`ListedScheduleLeg::raw_bid/raw_ask`, `listed_dispersion_schedule.hpp`) is
  // the intended production source a caller wires in here — this field is the
  // seam, not that wiring, which is caller-side.
  using QuoteLookup = std::function<std::optional<RawQuote>(const OptionContract &)>;
  QuoteLookup quote_lookup{};
  // B1: ORATS-calibrated crossing fractions for `SpreadKind::QuoteSide`
  // (`mid + f·half_spread`) — the sprint's feature-gap literature review reads
  // "mid + f·half-spread, f≈0.75 single-leg / ≈0.53 four-leg". `_single` prices
  // a fill whose COHORT (`Lot::cohort`) contributes exactly one leg to this
  // entry/close pass; `_complex` prices every wider cohort (a straddle, a
  // dispersion basket) — multi-leg fills cross LESS of the spread on average.
  // Both are inert (never read) under every OTHER `spread_kind`, so leaving
  // them at their ORATS defaults is safe even on a run that never turns
  // `QuoteSide` on. Overridable.
  double crossing_fraction_single{0.75};
  double crossing_fraction_complex{0.53};
};

// B1 (backtest-lakehouse sprint, target 1.1.0): which execution-friction
// assumption produced a `BacktestResult` — see `BacktestResult::friction_regime`
// below, the result-scalar stamp every run carries so no artifact leaves the
// engine without its own pricing assumption attached (a TSV/tearsheet reader
// should never have to cross-reference the `RunConfig` that produced it to know
// whether the NAV assumes free liquidity). Classified from `FrictionModel`
// alone (`friction_regime_for`, backtest.cpp), NOT from realized cost, so a
// `QuoteSide` run whose quotes all happened to fall back still reports
// `QuoteSide` — it names the ASSUMPTION, not the outcome:
//   Frictionless: `spread_kind == None` AND every other cost knob
//     (`impact_fraction`, `per_contract_cost`, `hedge_slippage_bps`) is 0 — the
//     `SpreadKind::None`, "all costs 0" combination `FrictionModel` itself
//     documents as bit-identical to a frictionless replay (invariant I3).
//   Modeled: any other NON-`QuoteSide` combination (`PriceBps`, `VolTicks`, a
//     `None` run with `impact_fraction`/`per_contract_cost`/
//     `hedge_slippage_bps` nonzero) — an assumed/synthetic spread, not an
//     observed one.
//   QuoteSide: `spread_kind == QuoteSide` — fills cross a recorded (or,
//     absent one, a modeled-PriceBps-fallback) NBBO.
enum class FrictionRegime : std::uint8_t { Frictionless = 0, Modeled = 1, QuoteSide = 2 };

// WS-F F3(b) (BT-P1-4). One discrete cash dividend on one underlier's SHARES.
// The hedge-share ledger used to accrue dividends only as a continuous
// `q_eff_at(0.25)` proxy — the surface's effective carry at a FIXED 3-month
// tenor, regardless of the step length and regardless of where the ex-dates
// actually fall — while the OPTION surfaces price the real discrete schedule.
// A delta-hedged book across ex-dates on a high-yield name therefore carried a
// systematic hedge-carry bias.
//
// The archive does NOT persist the corpus dividend schedule (`PricingContext`
// carries S / r / now_ts only), so replay cannot rediscover it: the schedule is
// a caller input, and it must be THE SAME schedule that priced the surfaces or
// the ledger and the marks disagree.
struct ShareDividend {
  std::uint32_t uid{0};     // underlier whose shares pay/receive
  std::int64_t ex_ts_ns{0}; // ex-date instant (epoch ns)
  double amount{0.0};       // cash per share, >= 0
};

// Engine-internal cash / borrow ledger config. The B2 DEFAULT keeps `finance_premium`
// and `shares_carry` OFF (a documented deviation from the design's true-defaults) so
// that a default `RunConfig{}` reproduces B1 bit-for-bit; operators opt in explicitly.
struct FinancingConfig {
  double borrow_rate{0.0};     // continuous, on |short shares| * S (hard-borrow proxy)
  bool finance_premium{false}; // cash balance accrues at r (DEFAULT OFF => B2 == B1)
  // Long shares earn q_eff*S*dt. With premium financing off, also charge
  // r*S*dt here; when it is on, the cash ledger already carries that funding.
  bool shares_carry{false};
  double initial_cash{0.0}; // opening cash balance
  // F3(b): discrete cash dividends on hedge shares. EMPTY BY DEFAULT, and an
  // empty schedule leaves every run bit-identical. A step whose (base, shifted]
  // window contains an ex-date books `shares * amount` into BOTH cash and the
  // financing column — a real cash event, so it is booked whether or not
  // `shares_carry` is on. For a uid that HAS a schedule the continuous
  // `q_eff_at(0.25)` dividend proxy is suppressed (its yield term drops to 0,
  // the funding term is unaffected): the discrete cash IS the dividend, and
  // charging both would double-count. uids with no schedule keep the proxy
  // exactly as before.
  std::vector<ShareDividend> share_dividends{};

  // A5 (backtest-production-lakehouse sprint, target 1.1.0): which uid's `r`
  // sources the cash-carry rate `finance_premium` accrues at. Before this field
  // existed the rate always came from `MarketSnapshot::surface_at(0)` -- the
  // first surface in ARCHIVE order, an arbitrary constituent on any multi-name
  // corpus. 0 (the default) means "require a single-name corpus": on a base
  // snapshot with at most one surface this reproduces the old `surface_at(0)`
  // behaviour bit-for-bit (there is only one surface to pick), and on a
  // multi-name corpus with no `flat_r` override it now FAILS CLOSED with an
  // explicit config error instead of silently keying off archive order. Set an
  // explicit uid to pin the rate source on a multi-name corpus. Ignored
  // whenever `flat_r` is set. APPENDED (additive, Tier-A freeze amended for
  // this sprint) -- existing fields keep their order.
  std::uint32_t reference_uid{0};
  // Explicit flat continuous rate for `finance_premium`, overriding any
  // per-uid surface lookup (and therefore any board/corpus dependency)
  // entirely. unset (the default) leaves `reference_uid` in force. APPENDED,
  // same additive treatment as `reference_uid` above.
  std::optional<double> flat_r{};
};

// ── Run config + result ─────────────────────────────────────────────────────

// What to do when a step cannot value a position because its surface is absent.
// This policy does not apply to a strategy-driven roll close: omitting a close mark
// would destroy cash/economic value, so the executor always fails closed.
//
// Four distinct lanes are governed, all reported in the same
// `BacktestResult::n_unpriced_lots` column (one count per excluded lane per step):
//
//   1. HELD (non-expiring) lots, for step P&L and book Greeks.
//   2. HEDGE SHARES held across a step whose base or shifted surface is absent
//      (WS-F F1(b), BT-P1-3): those shares used to be skipped in silence, so their
//      move vanished from NAV with no count and no flag.
//   3. The daily delta hedge's SHARE FILL on a uid with no surface on this base
//      (WS-F F1(a), BT-P1-3). Before F1 the fill priced at spot 0.0, which flattened
//      a residual position for free; F1 made it a hard error under both policies.
//   4. EXPIRY SETTLEMENT at a lot's exact expiry observation whose surface is absent.
//
// What this policy does NOT relax, under either setting: a lot whose expiry passed
// BETWEEN sessions without ever hitting an exact step still errors ("no exact expiry
// observation"). That is a calendar bug rather than missing market data — see
// `TenorSpec::snap_to_sessions` — and it stays fail-closed.
enum class UnpricedLotPolicy : std::uint8_t {
  // Preserve the historical held-valuation arithmetic (skip the unavailable P&L or
  // Greek lane) and report the exclusion in `BacktestResult::n_unpriced_lots`.
  //
  // Lane 3: the uid's hedge fill is SKIPPED for that step. Its share position is
  // left exactly as it was — deliberately NOT flattened, which is the pre-F1 bug —
  // so it rides the gap unhedged and shows up in lane 2 until the board returns.
  //
  // Lane 4: the lot is DEFERRED. It keeps its economic exposure and stays counted in
  // `n_open_lots`, and on the first later step whose surface is back it settles at
  // intrinsic against THAT step's spot (counted again on that step). The P&L explain
  // uses the base mark frozen at the deferral step; when even that board was absent
  // the settlement contributes cash only, because the intervening move is the
  // excluded, never-recovered P&L this policy already documents. A board that never
  // returns leaves the lot open to the end of the run, counted every step.
  // Deferred settlements cannot cross a `run_backtest_incremental` checkpoint (there
  // is no slot for them in `BacktestCheckpoint`); capturing one there is an error.
  // While a lot is deferred it is in neither the cash ledger nor the repriced book,
  // so `reconcile_nav` (opt-in) sees it as drift — the same ExcludeAndReport drift
  // `BacktestResult::nav_liquidation` is there to expose, not a new class of leak.
  ExcludeAndReport = 0,
  // Abort the run: any step with an unpriced held lot returns Err(NotFound). The mode
  // a production QIS run uses so a missing board can never silently truncate PnL.
  // FULLY fail-closed — every one of the four lanes above aborts, exactly as it did
  // before ExcludeAndReport learned to tolerate lanes 3 and 4.
  Error = 1,
};

// Admission policy for archived surfaces consumed by a backtest. Compatibility
// preserves the historical behavior. RequireAdmittedRisk fails closed unless
// every loaded surface is an admitted, currently serviceable risk surface.
enum class SurfaceProvenancePolicy : std::uint8_t {
  Compatibility = 0,
  RequireAdmittedRisk = 1,
};

// How the swap realized-variance lane reconciles a live lot's fixing schedule
// (implicitly daily — `SwapLot::n_obs_total` is sized assuming one fixing per
// NYSE session) against the clock's actual step cadence.
//
// A step's ACCRUAL WINDOW is `(base.ts_ns, shifted.ts_ns]`; the number of NYSE
// trading sessions inside it ("elapsed sessions") is 1 for an ordinary daily
// clock and >1 whenever the clock skips sessions the fixing schedule assumed
// it would visit (a weekly clock, a corpus with gaps). The pre-fix engine
// always booked `n_obs_done += 1` per step regardless, so a coarser-than-daily
// clock silently overstated `annualization * Sigma r^2 / n_done` by roughly the
// gap factor (a weekly clock ~5x).
enum class SwapFixingCadence : std::uint8_t {
  // FAIL CLOSED (default): every step of a live swap lot's fixing pass must
  // observe EXACTLY 1 elapsed session. A step spanning 0 or >1 sessions is a
  // schedule violation (`ErrorCode::SwapFixingScheduleViolation`) rather than
  // a silently mispriced accrual.
  RequireEverySession = 0,
  // Opt-in: accept the clock's own step cadence AS the fixing schedule. One
  // squared return is still booked per step (the gap's realized move is one
  // observation, not `elapsed_sessions` of them), but `n_obs_done` advances by
  // `elapsed_sessions` rather than by 1, so the daily-strike convention
  // (`annualization * Sigma r^2 / n_done`) is not overstated by the gap. This
  // is a conservative reading of a multi-session gap (one wide return spread
  // over N accrual slots), not a reconstruction of the unobserved daily path.
  AcceptClockAsSchedule = 1,
};

// Task A6 (backtest-lakehouse sprint): how `run_backtest` / `run_backtest_incremental`
// react to a `Clock` whose source manifest had dates with no admitted (Ok) fit
// -- see `Clock::dropped_dates()`. Those dates are always COUNTED now; this
// governs only whether a non-empty `clock.dropped_dates()` is tolerated (the
// run spans the gap as one step, same as every engine version before this
// task) or refused outright before a single step runs.
enum class ClockGapPolicy : std::uint8_t {
  // Preserve the historical step sequence: the run proceeds exactly as it
  // always has, spanning every gap as one ordinary step between the
  // surrounding Ok dates. `BacktestResult::n_clock_dates_dropped` and
  // `clock_dates_dropped` report the exclusion instead of leaving it
  // invisible, but no economics number moves -- BEHAVIOUR-PRESERVING DEFAULT.
  Accept = 0,
  // Fail closed: refuse (`ErrorCode::InvalidArgument`) before processing a
  // single step whenever `clock.dropped_dates()` is non-empty, naming every
  // dropped date in the error message. The production QIS mode for a corpus
  // that must have no undocumented gap.
  Error = 1,
};

// Task B2 (backtest-lakehouse sprint, target 1.1.0): what a recorded row whose
// Reg-T margin requirement (`BacktestResult::margin_required`, computed every
// row from `atx/vol/margin.hpp`'s `regt_short_option_margin` summed over the
// book's short option lots -- see backtest.cpp's `book_margin_required`)
// exceeds AVAILABLE CAPITAL should do. "Available capital" is that row's cash
// balance: `BacktestResult::cash` for the strategy overload, always 0.0 for
// the fixed-book (B0) overload, which carries no ledger at all -- so a short
// book under `Halt` on B0 refuses unconditionally. That is an intentional
// fail-closed reading, not an oversight: B0 represents no funded account, and
// `Halt` says "represent only a book capital can actually cover".
//
// This enum lives in backtest.hpp (Tier-A) rather than margin.hpp (Tier-B)
// deliberately: `RunConfig` is Tier-A and Tier-A is closed under inclusion
// (`VolUmbrella.TierAIsClosedUnderInclusion`), so a Tier-A struct may not
// reach into a Tier-B header for one of its field's types without promoting
// that header too -- and margin.hpp is new surface this sprint deliberately
// keeps OUT of the frozen umbrella. See margin.hpp's own file header for the
// one-way "engine calls into margin, never the reverse" rule this mirrors at
// the type level.
enum class MarginBreachPolicy : std::uint8_t {
  // BEHAVIOUR-PRESERVING DEFAULT: `margin_required` is still computed and
  // recorded every row (see `BacktestResult::margin_required`), but a
  // shortfall never halts the run -- bit-identical to the pre-B2 engine, with
  // the new column carried as a diagnostic only.
  Ignore = 0,
  // Fail closed: the FIRST recorded row whose margin requirement exceeds
  // available capital aborts the run (`ErrorCode::InvalidArgument`, naming
  // the date and the shortfall) instead of being recorded.
  Halt = 1,
};

// One observed engine step, handed to RunConfig::step_observer immediately after
// IStrategy::on_step returns Ok and BEFORE the transition validation / hedge /
// execute stage. Fires once per clock step INCLUDING inception (step_index 0), in
// step order, on EVERY step regardless of RunConfig::record_every_n (the recorded
// BacktestResult rows are downsampled; these events are not). Every reference is
// borrowed and valid only for the duration of the call.
struct StepEvent {
  std::size_t step_index;         // index into Clock::refs(); 0 == inception
  const SnapshotRef &ref;         // this step's clock entry (date + archive_path)
  const MarketSnapshot &snapshot; // the base the strategy just stepped on
  const IStrategy &strategy;      // post-on_step strategy state
};

// Optional per-step observation hook. Returning Err aborts the run with that error
// (the engine propagates it verbatim), so an observer may enforce its own
// invariants fail-closed. C++-only: deliberately NOT exposed through the pybind11
// RunConfig binding (the hand-kept def_readwrite list in
// python/src/bindings/backtest.cpp) — a std::function is not sensibly bindable and
// the Python surface stays unchanged. The omission is a decision, not drift.
using StepObserver = std::function<Status(const StepEvent &)>;

// CONSTRUCTION CONTRACT (v1, plan item 4.2) — DESIGNATED INITIALIZERS ONLY.
// Set fields by name (`RunConfig cfg{}; cfg.unpriced = ...;` or a designated
// initializer); never `RunConfig{a, b, c, ...}`. Three fields here used to carry
// an "appended for positional aggregate source compatibility" note, meaning they
// were parked at the end of the struct — away from the knob each belongs with —
// solely so positional initializers kept compiling. They now live in their
// natural groups, and the field-count `static_assert` below makes a silent
// re-append a compile error.
//
// backtest.hpp is Tier-A (frozen v1 surface): this reorder is the LAST layout
// change allowed here. After v1 the order is fixed and new knobs append at the
// end WITHOUT any positional-compatibility promise.
struct RunConfig {
  // Pricer thread fan-out. Default 0 => use all hardware cores (clamped to the
  // book's unique-contract count). Output is bit-identical to any thread count
  // (PortfolioPricer's serial-scatter reduction), so parallel-by-default is a free
  // throughput win — a backtest step reprices the whole book, the dominant cost.
  // analytic_greeks on by default: the backtest reprices the whole book every step,
  // so the analytic Andersen-Lake greeks (5 solves/contract vs the FD path's 7, with
  // exact continuation-PDE theta/charm) are a direct per-step speedup; the mark and
  // delta/gamma/vega/rho/vanna/volga are bit-identical to the FD path.
  PriceOptions price{/*n_threads=*/0, /*analytic_greeks=*/true};
  // Archived surfaces historically served the cold reference pricer. Preserve
  // that contract by default; faster cached query tiers are an explicit
  // backtest-level accuracy/latency choice and never alter the archive bytes.
  QueryPricingTier query_pricing_tier{QueryPricingTier::LegacyCompatible};
  // How the tier above is materialized. ReuseOnly consumes a prepared fast
  // snapshot but loads a separately-keyed cold snapshot on a fast miss. Eager
  // preserves the historical requested-tier behavior.
  QueryCacheBuildPolicy query_cache_build_policy{QueryCacheBuildPolicy::Eager};
  FrictionModel frictions{};          // execution frictions (B2; default: frictionless)
  FinancingConfig financing{};        // cash/borrow ledger (B2; default: off => B1-identity)
  unsigned record_every_n{1}; // positive; persist every Nth step (1 = every step)
  // Per-step observation hook. Empty by default => zero cost beyond one
  // predictable branch per step and byte-identical output. Fires on EVERY step
  // regardless of `record_every_n` above (recorded rows are downsampled; these
  // events are not). Ignored by no overload: the fixed-book (B0) overload has no
  // strategy, so setting this there is a fail-closed InvalidArgument, never a
  // silent drop.
  StepObserver step_observer{};
  // Cooperative cancellation, polled at the TOP of each step before any of that
  // step's work (plan item 5.5). Default-constructed => never cancels, one
  // predictable branch per step, byte-identical output.
  //
  // On a requested stop `run_backtest` returns `ErrorCode::Cancelled` and NO
  // BacktestResult: the run is abandoned before the result is handed back, so a
  // partially-filled result can never reach a caller, a writer, or validate().
  // Nothing on disk is touched either way — `run_backtest` performs no file I/O.
  // Sits beside `step_observer` because both are per-step run control the caller
  // owns; the referenced flag must outlive the call.
  CancelToken cancel{};
  // Policy for held P&L/Greek valuation only. Strategy close execution always
  // requires an economically valid mark regardless of this setting.
  //
  // WS-F F1(c) (BT-P1-2) BREAKING DEFAULT CHANGE: this used to default to
  // `ExcludeAndReport`, so a default-constructed RunConfig silently truncated NAV
  // across a missing board (the excluded step's PnL is never recovered when the
  // surface reappears — NAV permanently diverges from liquidation value, reported
  // only as a count in `n_unpriced_lots`). A production QIS default must fail
  // closed; callers that genuinely want the lenient arithmetic now opt in.
  UnpricedLotPolicy unpriced{UnpricedLotPolicy::Error};
  // The admission half of the same fail-closed story. Strict production backtests
  // opt in; the default deliberately preserves legacy archives.
  SurfaceProvenancePolicy surface_provenance_policy{SurfaceProvenancePolicy::Compatibility};
  // A null cache creates a private per-run cache. Supplying one permits archive
  // reuse across backtest and reconciliation passes. Private one-pass caches retain
  // at most the current/base/look-ahead working set. Look-ahead overlaps the next
  // archive open/map with pricing and strategy work on the current step.
  std::shared_ptr<SnapshotCache> snapshot_cache{};
  bool prefetch_snapshots{true};
  // L2 (AL-solve-wall sprint, fewer-solves): serve an expiring lot's base
  // settlement mark from the per-step mark memo (populated by the prior step's
  // book-greeks pass at the SAME base date) instead of re-solving it. The memo'd
  // mark is bit-identical to the settlement solve (FullGreeks mark == Marks mark,
  // pinned by the L2 crux gate), so ON vs OFF is bit-for-bit identical output; OFF
  // reproduces the pre-L2 solve-every-settlement behavior (and makes the
  // DuplicateMarkSolves ledger counter observe the duplication it removes).
  bool settlement_mark_memo{true};
  // WS-F F1(d): per-recorded-row NAV-vs-liquidation reconciliation (IStrategy
  // overload only — the fixed-book overload has no cash/share ledger to
  // reconcile against). NAV is a cumulative flow sum; nothing ever checked it
  // against an INDEPENDENTLY recomputed liquidation value, so the P1-2/P1-3 leak
  // family drifted it silently. When on, every recorded row recomputes
  //
  //   liquidation = (cash - initial_cash) + book MTM (PriceTotals::pv on this
  //                 row's base) + hedge-share MTM + cumulative NON-CASH financing
  //
  // publishes it in `BacktestResult::nav_liquidation`, and aborts the run when it
  // deviates from `nav` by more than `reconcile_nav_tol`.
  //
  // ON by default (Task A3, BT-P1-2): NAV is production accounting, not a
  // debug artifact, so the gate that proves it closes ships on too. A run that
  // wants the pre-A3 bit-exact replay behavior opts out explicitly; it costs
  // nothing when off either way (one bool test per row).
  bool reconcile_nav{true};
  // WS-F F2 (BT-P1-1): book the ENTRY FILL SLIPPAGE — qty*multiplier*(fill -
  // model mark) — as a realized execution cost.
  //
  // The engine carries the book at its model mark but pays `Lot::entry_price`.
  // When a strategy fills AWAY from the mark (a quote-side fill policy, an
  // ask-crossing entry) the difference never reaches NAV: NAV is a sum of
  // mark-to-mark moves, and the very first move is measured from the entry
  // date's mark, not from what was paid. So an entry crossing the spread used to
  // look free. With this on, the gap is charged into `cost` (hence into NAV and,
  // exactly once, into cash) and `reconcile_nav` closes.
  //
  // ON by default (Task A3, BT-P1-2): a production QIS default must show a
  // fill-vs-mark gap, not delete it. BIT-IDENTICAL to the pre-A3 default when
  // turned OFF (the opt-out for bit-exact replay suites): the mark used is
  // `entry_price` itself, so the booked slippage is exactly 0.0 and the cash
  // expression is unchanged. On, an entry whose model mark cannot be solved is
  // a hard error rather than a silent zero. Note the modeled `FrictionModel`
  // half-spread is ADDITIVE to this; a run using real quote-side fills
  // normally sets `half_spread_bps`/`vol_tick` to 0 so the spread is not paid
  // twice.
  bool book_entry_fill_slippage{true};
  // Absolute drift tolerance for `reconcile_nav`. The two quantities are the same
  // flows summed in different orders, so the honest floor is rounding
  // (~|cash|*eps per row), not zero. Must be finite and positive.
  double reconcile_nav_tol{1.0e-6};
  // Snapshot look-ahead DEPTH: how many future steps' snapshots may be in flight
  // at once. 1 is the historical single-step look-ahead — at step i exactly one
  // load (i+1) overlaps step i's economics, so if a load costs more than a step's
  // economics the run is load-bound and the loads are effectively serialized.
  // A depth of D keeps D loads in flight, so an independent-and-parallel load
  // stage pipelines against the strictly sequential economics; the run then costs
  // about max(economics, total_load / D) instead of economics + total_load.
  //
  // OUTPUT IS UNAFFECTED AT ANY DEPTH. Depth changes only WHEN a snapshot is
  // deserialized, never which bytes it deserializes from nor the order the
  // economics consume them: the loop still loads refs[i] at step i, and every
  // load is a pure function of one archive's bytes.
  // `Backtest.PrefetchDepthIsBitIdenticalToSingleStepLookAhead` pins that
  // bit-identity.
  //
  // A depth of D needs a snapshot cache retaining at least D+2 entries (base +
  // shifted + D in flight) or the LRU drops a completed prefetch before its step
  // reaches it, converting look-ahead into wasted work. `run_backtest`'s private
  // cache is sized from this field; a CALLER-SUPPLIED cache is the caller's to
  // size, and an undersized one costs throughput but never correctness.
  // 0 is normalized to 1 (no look-ahead is expressed by prefetch_snapshots=false).
  //
  // WHY THE DEFAULT IS 2 AND NOT 1 (v1 closeout sprint Task 4.8, plan item 6.7).
  // Measured on the 135-session projected replay, one binary with the depth
  // alternated inside a single session, 12 interleaved rounds over depths
  // {1,2,4,8}, medians and win-counts only (this
  // host has no frequency pinning and per-pair spreads reach ±40 %):
  //
  //     1 -> 2 : +15.2 % median, 11/12 rounds won
  //     1 -> 4 : +19.8 % median, 10/12
  //     1 -> 8 : +19.6 % median, 10/12
  //     2 -> 4 :  +1.9 % median,  7/12   (a wash)
  //     4 -> 8 :  +1.6 % median,  7/12   (a wash)
  //
  // The rungs FROM depth 1 and the rungs BETWEEN the deeper depths have to be read
  // together, which is why all five are here: every transition off 1 pays about the
  // same 15-20 %, and every transition among 2, 4 and 8 pays nothing, at win-counts
  // no better than chance. A table showing only 1 -> 2 could not distinguish that
  // from a ramp that simply had not been sampled far enough.
  //
  // The curve is a step, not a ramp: overlapping the FIRST load is where all of the
  // win is, and nothing past depth 2 is distinguishable from noise. Depth 2 is
  // therefore the cheapest default that takes it — one extra in-flight snapshot and
  // one extra cache slot (`private_snapshot_cache_capacity` = depth + 2, so 4
  // instead of 3), against a fifth of the replay's wall clock.
  //
  // A caller who cannot afford the extra resident snapshot sets 1 explicitly and
  // gets exactly the old behavior; the OUTPUT does not move either way, which is
  // what makes this a default worth changing inside a release.
  std::size_t prefetch_depth{2};
  // Task A1 (2026-08 backtest-lakehouse sprint): whether a live swap lot's
  // fixing pass requires the clock to visit exactly one NYSE session per step
  // (`RequireEverySession`, fail closed) or accepts the clock's own cadence as
  // the schedule, scaling `n_obs_done` by the elapsed-session count instead of
  // always advancing it by one (`AcceptClockAsSchedule`). See
  // `SwapFixingCadence` above. Sprint-owner-approved additive Tier-A exception
  // (backtest.hpp's own "new knobs append at the end" convention); this is
  // field 18, appended last per that convention.
  SwapFixingCadence swap_fixing_cadence{SwapFixingCadence::RequireEverySession};
  // Task A6 (2026-08 backtest-lakehouse sprint): whether a run tolerates a
  // `Clock` carrying a non-empty `dropped_dates()` (`Accept`, spanning the gap
  // as one step, same as always) or refuses to run at all (`Error`). See
  // `ClockGapPolicy` above. Same sprint-owner-approved additive Tier-A
  // exception as `swap_fixing_cadence`; this is field 19, appended last per
  // that convention.
  ClockGapPolicy clock_gaps{ClockGapPolicy::Accept};
  // Task B2 (backtest-lakehouse sprint, target 1.1.0): see `MarginBreachPolicy`
  // above. Same sprint-owner-approved additive Tier-A exception as
  // `swap_fixing_cadence` / `clock_gaps` before it; this is field 20, appended
  // last per that convention. Output is bit-identical for every caller that
  // does not set it -- `Ignore` (the default) computes and records
  // `BacktestResult::margin_required` on every row exactly as `Halt` would,
  // it just never turns a shortfall into an aborted run, so NAV/cash/every
  // other column a caller already depended on is untouched.
  MarginBreachPolicy margin_breach{MarginBreachPolicy::Ignore};
  // Task B3 (backtest-lakehouse sprint, target 1.1.0): see `ExercisePolicy`
  // above. Same sprint-owner-approved additive Tier-A exception as
  // `swap_fixing_cadence` / `clock_gaps` / `margin_breach` before it; this is
  // field 21, appended last per that convention. Output is bit-identical for
  // every caller that does not set it -- `Advisory` (the default) only adds
  // the two new `BacktestResult` counters (`n_short_calls_assignable`,
  // `n_puts_exercisable`); it never mutates a lot, the cash ledger or the
  // hedge-share ledger, so every existing column is untouched.
  ExercisePolicy exercise_policy{ExercisePolicy::Advisory};
  // Task C2 (backtest-lakehouse sprint, target 1.1.0): the process-wide SEALED
  // snapshot pool this run reads its dated archives from
  // (`atx/vol/research/snapshot_pool.hpp`). NON-OWNING — the pointee must
  // outlive the run, exactly like `CancelToken`'s flag and `step_observer`'s
  // callable, and for the same reason: a `shared_ptr` here would silently keep
  // a process-lifetime cache alive past the sweep that owns it.
  //
  // NULL (the default) is TODAY'S BEHAVIOUR, bit-for-bit: the engine builds its
  // private per-run `SnapshotCache` exactly as before, with the same subset,
  // capacity, Sealed backing and look-ahead prefetch. Non-null routes every
  // snapshot acquisition through the shared pool instead, so N variants over one
  // corpus open each archive ONCE between them; the bytes are identical either
  // way, so every column is (`BacktestExec.SnapshotPoolIsBitIdenticalToPrivateCache`).
  //
  // MUTUALLY EXCLUSIVE with `snapshot_cache` above: setting both is refused with
  // InvalidArgument rather than silently ranked. And a pool run refuses
  // `QueryCacheBuildPolicy::ReuseOnly`, which resolves each date to a fast or a
  // cold tier depending on what some OTHER run already built — history-dependent
  // pricing is precisely what a process-wide pool must not introduce.
  SnapshotPool *snapshot_pool{nullptr};
};

// Drift pin (plan item 4.2). RunConfig has exactly TWENTY-TWO fields. Adding,
// removing or splitting one breaks this line, which is the point: it forces
// whoever changes the struct to read the construction contract above instead of
// appending a knob "for compatibility" with positional initializers that are no
// longer part of the API.
//
// 15 -> 16 (plan item 5.5): `cancel` was INSERTED beside `step_observer`, its
// semantic group, not appended at the end. That is exactly the move the old
// convention forbade and this one requires — and it is safe only because there
// are no positional initializers left in-tree to rebind.
//
// 16 -> 17 (main merge): `prefetch_depth` APPENDED at the end — the pipelined
// snapshot look-ahead depth main's backtest-replay work introduced. Appending is
// the form this convention prescribes for a new knob, and it is what supersedes
// the branch's earlier `kPrefetchLookahead` file-scope constant in backtest.cpp.
// Output is bit-identical at any depth
// (`Backtest.PrefetchDepthIsBitIdenticalToSingleStepLookAhead` pins it), so the
// addition moves no number a caller already depends on.
//
// 17 -> 18 (Task A1, backtest-lakehouse sprint): `swap_fixing_cadence` APPENDED
// at the end. Sprint-owner-approved exception to the 1.x Tier-A freeze
// (target: 1.1.0) for this ONE additive field; see the sprint's
// global-constraints.md amendment. Output is bit-identical for every caller
// that does not set it (`RequireEverySession` — the default — reduces to the
// old book-a-fixing-per-step arithmetic on any clock that genuinely visits one
// NYSE session per step; it changes behavior only when the clock and the
// fixing schedule actually disagree, which used to compute a silently wrong
// answer instead of an error).
//
// 18 -> 19 (Task A6, backtest-lakehouse sprint): `clock_gaps` APPENDED at the
// end. Sprint-owner-approved exception to the 1.x Tier-A freeze for this ONE
// additive field, same as `swap_fixing_cadence` before it. Output is
// bit-identical for every caller that does not set it (`Accept` -- the
// default -- reduces to the pre-A6 step sequence exactly; it changes
// behavior only when the clock actually carries a dropped date AND the
// caller opts into `Error`, which used to be unrepresentable).
//
// 19 -> 20 (Task B2, backtest-lakehouse sprint): `margin_breach` APPENDED at
// the end. Same sprint-owner-approved exception to the 1.x Tier-A freeze as
// `swap_fixing_cadence` / `clock_gaps` before it. Output is bit-identical for
// every caller that does not set it (`Ignore` -- the default -- still
// populates `BacktestResult::margin_required` every row, it just never turns
// a shortfall into an aborted run, so it changes behavior only when a book's
// margin requirement actually exceeds available capital AND the caller opts
// into `Halt`, which used to be unrepresentable).
//
// 20 -> 21 (Task B3, backtest-lakehouse sprint): `exercise_policy` APPENDED at
// the end. Same sprint-owner-approved exception to the 1.x Tier-A freeze as
// `swap_fixing_cadence` / `clock_gaps` / `margin_breach` before it. Output is
// bit-identical for every caller that does not set it (`Advisory` -- the
// default -- only populates the two new NON-WIRE result scalars; it changes
// behavior only when a lot actually crosses the early-exercise boundary AND
// the caller opts into `Simulate`, which used to be unrepresentable).
//
// 21 -> 22 (Task C2, backtest-lakehouse sprint): `snapshot_pool` APPENDED at
// the end. Same sprint-owner-approved exception to the 1.x Tier-A freeze as
// `swap_fixing_cadence` / `clock_gaps` / `margin_breach` / `exercise_policy`
// before it. Output is bit-identical for every caller that does not set it
// (`nullptr` -- the default -- builds the same private per-run SnapshotCache
// with the same subset, capacity, Sealed backing and look-ahead the engine has
// always built); it changes anything at all only when a caller hands the run a
// process-wide pool, which used to be unrepresentable. The field is a
// FORWARD-DECLARED pointer, so no Tier-A header gained a non-Tier-A include.
//
// BLIND SPOT: this probe pins the field COUNT, nothing else. It cannot see a
// REORDER that leaves the count at 22 -- `aggregate_arity_is_v` (see
// detail/aggregate_arity.hpp) only checks how many brace-initializer slots
// `RunConfig{...}` accepts, with no notion of field NAMES or TYPES, so swapping
// two existing fields (e.g. two `bool`s, or two `std::size_t`s) still compiles
// green here. What actually protects against that is the "designated
// initializers only" contract above (a named initializer binds by field, so a
// reorder cannot mis-target it) -- this assert only proves the contract is not
// being silently defeated by an APPEND, and a reorder still needs the contract
// upheld EVERYWHERE to be safe. UPDATE DISCIPLINE for a reorder: before
// landing one, confirm every construction site still names its fields --
// `git grep -n "RunConfig{"` across src/, tests/, bench/ and the python
// bindings should turn up only empty `RunConfig{}` (or none) with no
// multi-argument positional brace list; `git grep -n "RunConfig cfg"` sites
// build via `RunConfig cfg;` plus `cfg.field = ...` assignment, which is
// order-independent by construction.
static_assert(detail::aggregate_arity_is_v<RunConfig, 22>,
              "RunConfig field count changed: update this pin, and confirm every "
              "construction site still initializes by field name.");

// Reusable caller-owned handoff from a step's P&L target solve to the strategy
// execution ledger. Storage grows to the largest observed book and never
// shrinks; `prepare(n)` changes only the active prefix when capacity is already
// warm. `seal()` builds a sorted ID index over the active prefix, rejects
// duplicate lot IDs, and must succeed before lookup. A sealed lookup is O(log N),
// allocation-free, and accepts only a matching Ok row with a finite nonnegative
// raw per-contract mark and finite prior-date vega.
class ReusableTargetMarkFrame {
public:
  struct Match {
    double raw_mark{0.0};
    // One-step-lag per-share vega from the P&L base solve. This is unbounded
    // approximate turnover telemetry only, never a friction or cash input.
    double base_vega_proxy{0.0};
  };

  void prepare(std::size_t n);
  [[nodiscard]] TargetMarkView write_view() noexcept;
  // Duplicate IDs return InvalidArgument and leave lookup disabled. Successful
  // seals allocate only when the largest observed frame grows.
  [[nodiscard]] Status seal();
  [[nodiscard]] std::optional<Match> find_ok(std::uint64_t id) const noexcept;
  // Rows preserve the input portfolio order. This diagnostic scans only the
  // active prefix, so warmed storage from a larger prior book is ignored.
  [[nodiscard]] std::optional<std::size_t> first_non_ok_index() const noexcept;

  [[nodiscard]] std::size_t size() const noexcept { return active_size_; }
  [[nodiscard]] std::size_t storage_size() const noexcept { return id_.size(); }
  [[nodiscard]] std::size_t index_storage_size() const noexcept { return order_.size(); }

private:
  std::vector<std::uint64_t> id_;
  std::vector<double> raw_mark_;
  std::vector<double> base_vega_proxy_;
  std::vector<PriceStatus> status_;
  std::vector<std::size_t> order_;
  std::size_t active_size_{0};
  bool sealed_{false};
};

// SoA time series. Row 0 is inception (opening friction in PnL/NAV, otherwise
// zero PnL, book greeks on the first date); each later recorded row is one priced
// step, downsampled by `record_every_n` (the final step is always recorded). When
// `record_every_n>1` the per-step FLOW columns (`pnl_*`, `pnl_settlement`,
// `pnl_shares`, `financing`, `cost`, `turnover_*`, `n_unpriced_lots`) hold the
// BLOCK SUM over every step since the previous recorded row — not the last step
// alone — so `Σ column == nav.back()` and the attribution totals are exact at any
// stride. STATE columns (`nav`, `cash`, `gross_*`, `n_open_lots`,
// `n_unpriced_greeks`) are the value AT the recorded row. `nav` is the cumulative
// Σ step_total (incl. settlement) from inception = 0.
// C1 (backtest-lakehouse sprint, target 1.1.0): a RESULT SCALAR PAIR, not a
// row-parallel series — run-totals of the step-mark memo's settlement
// admission decisions (see `detail::StepMarkMemo`, `src/step_mark_memo.hpp`,
// and `compute_step`'s L2 settlement branch in backtest.cpp). An expiring
// lot's base mark either SERVES from the memo (`settlement_memo_hits`,
// populated by the immediately preceding step's book-greeks/execute()
// FullGreeks pass) or falls back to a fresh Andersen-Lake solve
// (`settlement_full_solves` — a memo miss, a stale surface-instance guard, or
// `RunConfig::settlement_mark_memo == false`). Per-lot-per-settlement-event
// counts (a step settling two lots adds 2, split between the two counters as
// each lot resolves). Populated identically on BOTH `run_backtest` overloads
// (`compute_step` is shared). Same append-only, non-wire treatment as
// `n_steps_entry_skipped` and its siblings below: absent from
// `kBacktestSeriesColumns` and RunArchive serialization, so `ra_schema_hash()`
// and every TSV/CSV header and golden are untouched. Both members are always
// 0 on a run with no expiring lots, and `settlement_memo_hits` is always 0
// under `RunConfig::settlement_mark_memo == false`, which never consults the
// memo.
struct SolveLedgerSummary {
  std::uint64_t settlement_memo_hits{0};
  std::uint64_t settlement_full_solves{0};

  [[nodiscard]] bool operator==(const SolveLedgerSummary &) const noexcept = default;
};

struct BacktestResult {
  std::vector<std::string> date;
  std::vector<std::int64_t> ts_ns;
  std::vector<double> pnl_total, pnl_delta, pnl_gamma, pnl_vega, pnl_vanna, pnl_volga, pnl_theta,
      pnl_rho, pnl_charm, pnl_unexplained;
  std::vector<double> pnl_settlement; // intrinsic settlement PnL this step
  // B2 execution/ledger columns (per-step; 0.0 in the fixed-book overload and when
  // the corresponding feature is off). `pnl_shares` = hedge-share MTM, `financing` =
  // cash carry + short borrow + shares carry, `cost` = realized frictions this step.
  std::vector<double> pnl_shares, financing, cost;
  std::vector<double> nav;  // cumulative from inception = 0 (running Sum step_total)
  std::vector<double> cash; // engine cash ledger balance (B2; 0.0 fixed-book)
  // Book greeks on the base. NAME WARNING (C-3, pipeline-m production review):
  // every one of these is the SIGNED aggregate `PriceTotals::<greek>`, i.e. the
  // NET book exposure — "gross" here has always meant "whole book" rather than
  // "sum of absolute leg exposures". The names are frozen: they are the emitted
  // TSV header and the RunArchive column registry `kBacktestCols` whose fold is
  // `ra_schema_hash()`, so renaming one would move a schema hash and every
  // golden. `gross_vega_abs` below carries the genuinely gross vega instead.
  std::vector<double> gross_delta, gross_gamma, gross_vega, gross_theta;
  // TRUE GROSS vega: Σ|position-scaled leg vega| over the Ok lanes of the same
  // book pricing that produced `gross_vega`, in the same per-UNIT-vol,
  // position-scaled dollars (multiply by `kVegaPerVolPoint` for dollars per vol
  // point). Non-negative by construction.
  //
  // C-3. `gross_vega` is a near-cancelling residual for any vega-neutral book —
  // a dispersion book drives it to ~0 BY DESIGN — so the risk-normalized
  // tearsheet statistics that call themselves gross (`avg_gross_vega`,
  // `return_on_gross_vega`, `vega_adj_sharpe`) were dividing the run's return by
  // that residual. They consume THIS series now.
  //
  // DELIBERATELY NOT SERIALIZED. It is absent from `kBacktestSeriesColumns`
  // (backtest_series_columns.hpp) and from the frozen RunArchive registry, so
  // `ra_schema_hash()`, every TSV header and every golden are untouched. It is
  // therefore EMPTY — not zero-filled — on any result that did not come from
  // `run_backtest`: a hand-built one, a TSV read, or an archive decode. The
  // tearsheet fold falls back to |`gross_vega`| in exactly that case, which is
  // bit-for-bit what it always did. Empty-or-row-parallel, like
  // `nav_liquidation`.
  std::vector<double> gross_vega_abs;
  std::vector<double> turnover_notional, turnover_vega; // traded |notional| / |vega| this step
  // Vol-derivative (swap) lane. `swap_pv` is a STATE column: the sum of the LIVE
  // swap lots' qty-scaled marks after this row's swap pass (a settled lot has
  // left the book, so it contributes 0). `swap_pnl` is a FLOW column,
  // block-summed like every other flow at `record_every_n > 1`: each live lot's
  // (mark - prev mark) plus each settling lot's (payoff - prev mark).
  //
  // BOTH ARE EXACTLY 0.0 ON EVERY ROW of a book with no swap lots, and both are
  // 0.0 on the inception row and throughout the fixed-book overload (which
  // refuses swap lots outright). Row-parallel to `date` on any engine-produced
  // result; EMPTY on a hand-built one, a TSV read or an archive decode — this
  // pair is deliberately NOT part of the frozen `kBacktestSeriesColumns` /
  // RunArchive registry, so `ra_schema_hash()` and every golden are untouched
  // (and `backtest_db` refuses to persist a run that carries swap state at all
  // rather than dropping it silently).
  std::vector<double> swap_pv, swap_pnl;
  // Open lots at this row. USUALLY the size of the engine's book, but not by
  // definition: under `UnpricedLotPolicy::ExcludeAndReport` a lot whose expiry step
  // had no board is DEFERRED, and a deferred lot has left the engine's `lots`
  // vector (it is past its expiry, which every book-facing invariant forbids) while
  // still holding real exposure the run has not settled. Those lots are counted
  // here — this column is "positions still open", not "book vector size" — and are
  // excluded from `gross_*`. Always exactly the book size under `Error`, which
  // never defers.
  std::vector<double> n_open_lots;
  // Positions this row could not value; their PnL and greeks are EXCLUDED from this
  // row's totals. Under RunConfig::unpriced == Error a step with a non-zero count
  // aborts the run instead of recording a row, so a non-zero value here only ever
  // appears under ExcludeAndReport. One count per excluded lane per step, summed
  // over the four lanes `UnpricedLotPolicy` governs:
  //   1. a HELD lot whose base or shifted surface is absent over the step;
  //   2. (WS-F F1(b)) a HEDGE-SHARE ledger entry whose base or shifted surface is
  //      absent over the step — its share MTM and financing are excluded from this
  //      row exactly as an unpriced lot's PnL is;
  //   3. (SP100) a delta-hedge SHARE FILL the overlay skipped because the uid has no
  //      board on this row's base — the position is left in place, unhedged;
  //   4. (SP100) a SETTLEMENT event at a lot's exact expiry with no board: the step
  //      that defers the lot, every later step it spends waiting, and the step it
  //      finally settles on are each counted once.
  // So the column stays the single honest "things this row could not value" count.
  // Row 0 (inception) is 0.0 for lanes 1/2/4, which cannot occur there; lane 3 is
  // recorded on it like any other row (also 0.0 for a normally-opened book, since
  // the strategy never opens a lot in an absent name and the share ledger is empty).
  std::vector<double> n_unpriced_lots;
  // Positions whose surface was absent on THIS row's date; their greeks are EXCLUDED
  // from this row's `gross_*`. Distinct from `n_unpriced_lots`, which measures the
  // step's PnL completeness (base AND shifted): `book_greeks` prices a single-date
  // snapshot against this row's date alone, so the two counts can diverge (a held
  // lot absent from the step's base but present again on this row's date is counted
  // in `n_unpriced_lots` but NOT here). Filled on EVERY row including row 0 —
  // inception computes book greeks, so its count is a real measurement, not 0.0 by
  // convention. Under RunConfig::unpriced == Error a row with a non-zero count aborts.
  // A DEFERRED lot (see `n_open_lots`) is NOT counted here: it is absent from the
  // priced book entirely rather than present-and-unpriceable, so a row can show
  // `n_open_lots` above the greek-covered lot count with this column at 0. The
  // deferral is reported on the step side, in `n_unpriced_lots`.
  std::vector<double> n_unpriced_greeks;
  // FULL-RESOLUTION per-step total PnL: one entry per priced step (steps 1..N-1),
  // retained regardless of `record_every_n`. This is the TRUE per-step return
  // series the risk/return statistics (Sharpe, ann_return, ann_vol, hit_rate,
  // avg_daily_pnl) are computed from, so those stats are invariant to the record
  // stride — block-summed recorded rows would destroy the per-step variance and
  // win/loss distribution. Length == refs-1 at any stride (NOT parallel to `date`,
  // which is downsampled). Empty for hand-built results, in which case the
  // consumers fall back to the recorded `pnl_total` rows (i.e. stride-1 behavior).
  std::vector<double> step_pnl_total;
  // WS-F F1(d): the INDEPENDENTLY recomputed liquidation value at each recorded
  // row, anchored so it is directly comparable to `nav`:
  //   (cash - initial_cash) + book MTM + hedge-share MTM + cumulative non-cash
  //   financing accrual.
  // EMPTY unless `RunConfig::reconcile_nav` is set (and always empty for the
  // fixed-book overload, which has no cash/share ledger). When populated it is
  // parallel to `date`, and the run has already asserted
  // |nav_liquidation[i] - nav[i]| <= RunConfig::reconcile_nav_tol on every row.
  std::vector<double> nav_liquidation;
  // Task B2 (backtest-lakehouse sprint, target 1.1.0): Reg-T margin required
  // for this row's book -- `atx/vol/margin.hpp`'s `regt_short_option_margin`,
  // summed over the SHORT option lots (long/flat lots need no maintenance
  // collateral; see backtest.cpp's `book_margin_required`). Same non-wire,
  // EMPTY-OR-ROW-PARALLEL convention as `gross_vega_abs` / `nav_liquidation`
  // above: absent from `kBacktestSeriesColumns` and the frozen RunArchive
  // registry, so `ra_schema_hash()`, every TSV/CSV header and every existing
  // golden are untouched. ALWAYS COMPUTED -- unlike `nav_liquidation`, this
  // column is not gated by a RunConfig on/off switch; `RunConfig::
  // margin_breach` governs only whether a shortfall HALTS the run (see
  // `MarginBreachPolicy`), never whether this column is populated, so
  // `Ignore` (the default) still records it every row. Empty on a hand-built
  // result, a TSV read or an archive decode, exactly like its two non-wire
  // siblings.
  std::vector<double> margin_required;
  // Strategy diagnostics: name -> per-recorded-row series (parallel to `date`).
  // Empty for the fixed-book overload; populated by the IStrategy overload.
  std::vector<std::pair<std::string, std::vector<double>>> signals;

  // A2 (backtest-production-lakehouse sprint, target 1.1.0): a RESULT SCALAR,
  // not a row-parallel series — one count for the whole run, copied verbatim
  // from `IStrategy::n_steps_entry_skipped()` after the strategy-aware
  // `run_backtest` completes (see backtest.cpp). It is NOT part of the frozen
  // 25-column wire series set: absent from `kBacktestSeriesColumns`
  // (backtest_series_columns.hpp) and from BacktestDb/RunArchive serialization,
  // so `ra_schema_hash()`, every TSV/CSV header and every golden are untouched.
  // Sprint-owner-approved additive field on this Tier-A struct (append-only;
  // existing fields keep their order). Always 0 for the fixed-book (B0)
  // overload, which has no strategy to ask.
  std::uint64_t n_steps_entry_skipped{0};

  // A4 (backtest-production-lakehouse sprint, target 1.1.0): a RESULT SCALAR,
  // not a row-parallel series — a run-total count of deferred-lot settlements
  // that could not use the expiry-date spot recorded at deferral time (see
  // `DeferredSettlementBook` in backtest.cpp) and substituted a later,
  // post-expiry spot instead. The substitution still books cash/explain at
  // that stale spot — this counter only NAMES it rather than letting it drift
  // silently. Same append-only, non-wire treatment as `n_steps_entry_skipped`
  // above: absent from `kBacktestSeriesColumns` and RunArchive serialization,
  // so `ra_schema_hash()` and every TSV/CSV header and golden are untouched.
  // Always 0 under `UnpricedLotPolicy::Error`, which never defers.
  std::uint64_t n_settlements_at_stale_spot{0};

  // A5 (backtest-production-lakehouse sprint, target 1.1.0): a RESULT SCALAR,
  // not a row-parallel series — a run-total count of STEPS (inception included)
  // on which the daily delta-hedge overlay wanted to fill at least one uid's
  // shares and could not, because that uid's base surface was absent
  // (`ExcludeAndReport` only; see `ExecResult::n_unpriced_hedges` and
  // `HedgeLedger::hedge_daily` in backtest.cpp). The skipped fill leaves that
  // uid's hedge-share position exactly as it was — the whole point, since the
  // pre-F1 bug flattened it "for free" at spot 0.0 instead — so this counter
  // only NAMES the exclusion rather than letting it pass unremarked. Counts
  // STEPS, not individual skipped fills: a step where two uids both skip still
  // adds 1. Same append-only, non-wire treatment as `n_steps_entry_skipped` and
  // `n_settlements_at_stale_spot` above: absent from `kBacktestSeriesColumns`
  // and RunArchive serialization, so `ra_schema_hash()` and every TSV/CSV
  // header and golden are untouched. Always 0 under `UnpricedLotPolicy::Error`,
  // which aborts the run instead of skipping, and always 0 for the fixed-book
  // (B0) overload, which has no strategy and therefore never hedges.
  std::uint64_t n_hedge_steps_skipped{0};

  // A6 (backtest-lakehouse sprint, target 1.1.0): a RESULT SCALAR, not a
  // row-parallel series — copied verbatim from `clock.dropped_dates().size()`
  // (see `Clock::dropped_dates`) once, before the run processes its first
  // step. Counts dates the source manifest listed with no admitted (Ok) fit,
  // which `Clock::from_manifest` used to drop with no record at all — the run
  // spanned the resulting gap as one ordinary step (fit-survivorship). Same
  // append-only, non-wire treatment as `n_steps_entry_skipped` and its
  // siblings above: absent from `kBacktestSeriesColumns` and RunArchive
  // serialization, so `ra_schema_hash()` and every TSV/CSV header and golden
  // are untouched. Always 0 for a clock built by `from_surface_db`, and
  // always 0 under `RunConfig::clock_gaps == ClockGapPolicy::Error`, which
  // refuses the run outright rather than let a nonzero count through.
  std::uint64_t n_clock_dates_dropped{0};
  // A6: the actual dropped dates behind `n_clock_dates_dropped` above, in the
  // same ascending order as `Clock::dropped_dates()` — the diagnostic detail
  // a bare count cannot carry ("which dates"). Same non-wire, append-only
  // treatment; empty exactly when `n_clock_dates_dropped == 0`.
  std::vector<std::string> clock_dates_dropped{};

  // B1 (backtest-lakehouse sprint, target 1.1.0): a RESULT SCALAR, not a
  // row-parallel series — which execution-friction assumption produced this
  // run, classified once from `RunConfig::frictions` by `friction_regime_for`
  // (backtest.cpp) and copied onto `out` before it is returned. See
  // `FrictionRegime` above for the three-way classification. Same
  // append-only, non-wire treatment as `n_steps_entry_skipped` and its
  // siblings: absent from `kBacktestSeriesColumns` and RunArchive
  // serialization, so `ra_schema_hash()` and every TSV/CSV header and golden
  // are untouched. Always `Frictionless` for the fixed-book (B0) overload,
  // which never calls `execute()` and therefore never charges anything a
  // configured `frictions` might otherwise imply.
  FrictionRegime friction_regime{FrictionRegime::Frictionless};

  // Task B3 (backtest-lakehouse sprint, target 1.1.0): RESULT SCALARS, not
  // row-parallel series — run-totals of the early-exercise/assignment
  // boundary checks `RunConfig::exercise_policy` governs (see
  // `ExercisePolicy` / `should_exercise_early` in this header and
  // `apply_early_exercise` in backtest.cpp). Each counts LOT-STEP
  // occurrences (a lot flagged on three consecutive steps adds 3; two
  // distinct lots flagged the same step add 2), computed identically under
  // BOTH policies -- `Advisory` only counts, `Simulate` counts AND converts,
  // so the two counters mean the same thing regardless of which policy
  // produced them. Same append-only, non-wire treatment as
  // `n_steps_entry_skipped` and its siblings above: absent from
  // `kBacktestSeriesColumns` and RunArchive serialization, so
  // `ra_schema_hash()` and every TSV/CSV header and golden are untouched.
  // Populated on BOTH `run_backtest` overloads (the check itself needs no
  // cash/share ledger, only the base/shifted boards and the lot); ALWAYS 0
  // for a book with no ITM short calls / ITM puts, and always 0 for a run
  // whose `FinancingConfig::share_dividends` never places an ex-date inside
  // a held short call's step window (the calls counter has no schedule-free
  // trigger — see `should_exercise_early`'s doc comment).
  //
  // `n_short_calls_assignable`: a SHORT call (`Lot::qty < 0`) going ex-
  // dividend next session whose extension value (mark - intrinsic) is LESS
  // than the forward dividend it would cost the long counterparty to wait —
  // i.e. assignment risk on OUR book.
  std::uint64_t n_short_calls_assignable{0};
  // `n_puts_exercisable`: a deep-ITM put, EITHER side of our book, whose
  // extension value is LESS than the interest-carry benefit of collecting
  // the strike now (`K * (1 - exp(-r*T))`) — an early-exercise opportunity
  // on a long put we hold, or an assignment risk on a short put we wrote;
  // the boundary condition is the same option economics either way (see
  // `should_exercise_early`'s doc comment), so both sides share one counter.
  std::uint64_t n_puts_exercisable{0};

  // Task C1 (backtest-lakehouse sprint, target 1.1.0): the settlement side of
  // the L2 step-mark memo's solve ledger. See `SolveLedgerSummary` above.
  SolveLedgerSummary solve_ledger{};

  [[nodiscard]] std::size_t size() const noexcept { return date.size(); }

  // ── Column-shape invariant (plan item 4.6) ────────────────────────────────
  //
  // Everything above is a parallel column set over `size()` rows, and nothing
  // used to enforce that. A result whose `nav` was one row shorter than `date`
  // indexed OUT OF RANGE inside the tearsheet fold and both serializers rather
  // than reporting a shape error — benchmark_stats_test.cpp documented the
  // hazard in a comment and worked around it by filling every column by hand.
  // `validate` is the enforcement point.
  //
  // The invariant is EMPTY-OR-ROW-PARALLEL, per column:
  //
  //   * a row-parallel column is either EMPTY (that column was not produced) or
  //     EXACTLY `size()` long. A different NON-ZERO length is the silent
  //     misalignment this rejects; it is never a legal partial result.
  //   * this is already the documented contract of `gross_vega_abs` and
  //     `nav_liquidation`. 4.6 makes it universal and checked, which also keeps
  //     legitimately sparse results legal — a fixture that fills the columns a
  //     fold reads and leaves the rest empty stays valid.
  //   * every `signals` series is empty-or-row-parallel too, and signal names
  //     must be non-empty and unique: both serializers append one dynamic
  //     column per signal, so a duplicate name emits an ambiguous header.
  //   * `step_pnl_total` is EXEMPT. It is the full-resolution per-step series
  //     (length == refs-1) and is deliberately NOT parallel to the downsampled
  //     `date`, exactly as its own comment states.
  //
  // Enforcement, not just availability: both `run_backtest` overloads validate
  // before returning Ok, so no engine-produced result can be skewed; the TSV and
  // CSV writers validate on entry, so no hand-built or decoded one reaches those
  // wires. `encode_backtest_section` (the RunArchive encoder) has no error channel
  // and is NOT checked; its production callers pass a `run_backtest` result that
  // already was. The check is a pure read — it never touches a value.
  //
  // @return InvalidArgument naming the first offending column and both lengths.
  [[nodiscard]] Status validate() const;
};

// One entry in the engine's insertion-ordered delta-hedge share ledger. Order is
// economically significant: share P&L, financing, and subsequent hedge trades
// accumulate in this order, so a continuation must preserve it exactly.
struct HedgeSharePosition {
  std::uint32_t uid{0};
  double shares{0.0};

  [[nodiscard]] bool operator==(const HedgeSharePosition &) const = default;
};

// Complete strategy-aware engine state at one processed clock reference. This is
// a persistence seam, not a serialized format: database code owns versioning and
// encoding. A valid checkpoint contains only live lots, monotonically-issued lot
// identity state, the ordered hedge ledger, and every cumulative accounting term
// needed to make a resumed step identical to the corresponding one-shot step.
struct BacktestCheckpoint {
  std::int64_t base_ts_ns{0};
  std::size_t completed_step_index{0};
  std::uint64_t next_lot_id{1};
  PortfolioState portfolio{};
  std::vector<HedgeSharePosition> hedge_shares{};
  // One entry per swap lot the run has already seeded, in first-sight order. A
  // resumed step reproduces its marks exactly because the fixing series, the
  // running variance and yesterday's mark all travel here. Every `lot_id` must
  // name a lot in `portfolio.swap_lots`; a lot opened but not yet seeded (a
  // single-ref run) legitimately has no entry.
  std::vector<SwapAccrual> swap_accruals{};
  double cash{0.0};
  double nav{0.0};
  double cumulative_noncash_financing{0.0};

  [[nodiscard]] bool operator==(const BacktestCheckpoint &) const = default;
};

// Rows produced by this invocation plus the state immediately after its final
// processed reference. Fresh runs contain the ordinary inception row and all
// subsequent rows. Resumed runs omit the anchor and contain only refs[1..].
struct BacktestContinuation {
  BacktestResult rows{};
  BacktestCheckpoint checkpoint{};
};

// B0 driver: MTM a FIXED hand-built book forward across the clock. Canonical
// loop: base = load(refs[0]); for i in 1..N-1 { shifted = load(refs[i]);
// pnl_explain(base -> shifted); settle expiries observed exactly at shifted.ts;
// record @ granularity; base = std::move(shifted); }. NotFound is returned when
// the clock crosses a held lot's expiry without an exact timestamp observation.
//
// THE SWAP LANE IS NOT AVAILABLE HERE: a non-empty `initial.swap_lots` is
// InvalidArgument, never a silent drop. A swap settles into a cash ledger this
// overload does not have (its `cash` column is 0.0 by construction), so there is
// no honest place for the payoff to land. Use the strategy overload.
[[nodiscard]] Result<BacktestResult> run_backtest(const Clock &clock, PortfolioState initial,
                                                  const RunConfig &cfg = {});

// B1 driver: the strategy-aware overload. `strat.on_step` runs at inception
// (step 0) and after each move-swap on the new base — opening entries / rolling
// cohorts / closing lots — then the same resolve-today -> pnl_explain-forward ->
// move-swap loop MTMs the evolving book. Book greeks and `signals(base)` are
// recorded AFTER each step's entries. Settlement of expiring lots is engine-owned
// (at intrinsic), identical to the fixed-book overload and subject to the same
// exact-expiry-observation contract.
//
// This is the ONLY overload carrying the vol-derivative lane: a strategy may
// append `SwapLot`s to `book.swap_lots`, and each step then (a) takes that
// date's spot as a fixing, (b) marks the lot through `deriv_price` against the
// shifted surface, or (c) cash-settles it from the accrual when its expiry is
// observed EXACTLY, on the same convention options settle on. An absent surface
// for ANY live swap lot — held OR settling — is NotFound: the lane fails closed
// exactly as the hedge-share ledger and the option settlement path do.
[[nodiscard]] Result<BacktestResult> run_backtest(const Clock &clock, IStrategy &strat,
                                                  const RunConfig &cfg = {});

// Incremental strategy-aware driver. With `resume == nullptr`, behavior and rows
// match the strategy-aware run_backtest overload and a final checkpoint is
// returned. With a checkpoint, refs[0] must be its exact base timestamp: that
// anchor is loaded for forward P&L but inception/on_step is deliberately skipped.
// Only refs[1..] are processed and returned, while strategy and observer calls
// receive checkpoint.completed_step_index + their local clock index.
//
// Checkpoints and configuration are validated before state is consumed. The API
// currently accepts only record_every_n == 1 because pending stride-block state
// is intentionally not part of BacktestCheckpoint.
[[nodiscard]] Result<BacktestContinuation>
run_backtest_incremental(const Clock &clock, IStrategy &strat, const RunConfig &cfg = {},
                         const BacktestCheckpoint *resume = nullptr);

} // namespace atx::vol
