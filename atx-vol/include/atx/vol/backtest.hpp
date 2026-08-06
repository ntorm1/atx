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

private:
  std::vector<SnapshotRef> refs_;
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
// EXPIRY SETTLEMENT is the ONLY exercise event. Every American lot is carried to
// its `expiry_ts_ns` and cash-settled at intrinsic against the spot observed at
// the exactly-matching snapshot. There is deliberately no assignment or
// early-exercise simulation:
//
//   * a short ITM call over an ex-date is never assigned away;
//   * a long deep-ITM put's optimal early exercise is never taken. Its forgone
//     value IS carried in the mark (the pricer is American), but it is never
//     REALIZED, so a hold-to-expiry book gives the early-exercise premium back
//     at expiry instead of capturing it.
//
// Roll-at-N-DTE strategies (the dispersion route) mostly dodge this because
// they close well before expiry. `HoldToExpiry` / `CloseAtHorizon` DSL books do
// NOT: read their settlement PnL as a lower bound on an optimally-exercised
// book.
//
// Nor is there any corporate-action handling: `Lot` is immutable by design, so a
// split in-window would change K and multiplier with no adjustment.
//
// This is a TRACKED DEFERRAL, not an oversight — see the sprint plan
// §9 ("Assignment/early-exercise simulation in the backtest"). Closing it needs
// an exercise-boundary decision rule per step, a settlement/assignment cash
// convention, and share-delivery into the hedge ledger; that is its own design,
// and half of it (a discrete-cash-dividend PDE American pricer) is the same
// §9 deferral the pricing lane carries.
//
// What IS modelled, as of WS-F: discrete cash dividends on the HEDGE SHARE
// ledger (`FinancingConfig::share_dividends`) — the leg of P1-4 that needed no
// exercise model.

// ── Execution frictions + financing (Phase B2) ───────────────────────────────

// Modeled bid/ask + costs applied ONLY to traded quantity (entries, roll-closes,
// hedge shares); holding accrues financing only. PricedSurface is a fitted MID
// surface (no stored bid/ask), so the spread is a documented model. The default
// (`SpreadKind::None`, all costs 0) reproduces the frictionless run bit-for-bit.
struct FrictionModel {
  enum class SpreadKind : std::uint8_t { None = 0, PriceBps = 1, VolTicks = 2 };
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
};

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
  // deviates from `nav` by more than `reconcile_nav_tol`. OFF by default: it is a
  // debug/audit gate, and it costs nothing when off (one bool test per row).
  bool reconcile_nav{false};
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
  // OFF by default and BIT-IDENTICAL when off: the mark used is `entry_price`
  // itself, so the booked slippage is exactly 0.0 and the cash expression is
  // unchanged. On, an entry whose model mark cannot be solved is a hard error
  // rather than a silent zero. Note the modeled `FrictionModel` half-spread is
  // ADDITIVE to this; a run using real quote-side fills normally sets
  // `half_spread_bps`/`vol_tick` to 0 so the spread is not paid twice.
  bool book_entry_fill_slippage{false};
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
};

// Drift pin (plan item 4.2). RunConfig has exactly EIGHTEEN fields. Adding,
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
// BLIND SPOT: this probe pins the field COUNT, nothing else. It cannot see a
// REORDER that leaves the count at 18 -- `aggregate_arity_is_v` (see
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
static_assert(detail::aggregate_arity_is_v<RunConfig, 18>,
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
  // Strategy diagnostics: name -> per-recorded-row series (parallel to `date`).
  // Empty for the fixed-book overload; populated by the IStrategy overload.
  std::vector<std::pair<std::string, std::vector<double>>> signals;

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
