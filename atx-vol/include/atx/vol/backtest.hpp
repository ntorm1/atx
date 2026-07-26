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
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "atx/vol/corpus.hpp"           // CorpusManifest
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
  // the whole-board `reconstruct_all_with_provenance`); if none match, the whole
  // board is loaded. Empty (the default) keeps the whole-board load. `uid_of` still
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
  // Read-only view of the OWNED surfaces (archive order). EMPTY on a borrowed
  // (zero-copy) load — use `n_surfaces()` / `surface_at()` for backing-agnostic
  // access. Safe across a move (vector move preserves element addresses).
  [[nodiscard]] std::span<const PricedSurface> surfaces() const noexcept { return surfaces_; }
  // Read-only view of the BORROWED surfaces (archive order). Empty on an owned load.
  [[nodiscard]] std::span<const PricedSurfaceView> views() const noexcept { return views_; }
  // True when this snapshot borrows its archive's mapped records rather than owning
  // reconstructed surfaces (WS-ZC1).
  [[nodiscard]] bool borrows_views() const noexcept { return !views_.empty(); }

  // Backing-agnostic surface access (archive order). Always non-empty after a
  // successful load, whichever backing was chosen.
  [[nodiscard]] std::size_t n_surfaces() const noexcept {
    return views_.empty() ? surfaces_.size() : views_.size();
  }
  [[nodiscard]] SurfaceRef surface_at(std::size_t i) const noexcept {
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
// deterministic least-recently-used mode for one-pass pipelines. Automatic
// eviction never removes an in-flight future; bounded mode can therefore exceed
// its target temporarily while every candidate is loading, then trims on the
// next cache operation. Copies share both entries and the configured mode.
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
};

// The open book across all cohorts. Plain for B0 (no cash/shares ledger yet).
class PortfolioState {
public:
  std::vector<Lot> lots;
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
  double half_spread_bps{0.0};    // PriceBps: half-spread = mark * bps/1e4 (per share)
  double vol_tick{0.0};           // VolTicks: half-spread = vega * vol_tick (per share)
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
  std::uint32_t uid{0};      // underlier whose shares pay/receive
  std::int64_t ex_ts_ns{0};  // ex-date instant (epoch ns)
  double amount{0.0};        // cash per share, >= 0
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

// What to do when a HELD (non-expiring) lot cannot be valued for step P&L or book
// Greeks. This policy does not apply to a strategy-driven roll close: omitting a
// close mark would destroy cash/economic value, so the executor always fails closed.
// The policy also governs HEDGE SHARES held across a step whose base or shifted
// surface is absent (WS-F F1(b), BT-P1-3): those shares used to be skipped in
// silence, so their move vanished from NAV with no count and no flag.
enum class UnpricedLotPolicy : std::uint8_t {
  // Preserve the historical held-valuation arithmetic (skip the unavailable P&L or
  // Greek lane) and report the exclusion in `BacktestResult::n_unpriced_lots`.
  ExcludeAndReport = 0,
  // Abort the run: any step with an unpriced held lot returns Err(NotFound). The mode
  // a production QIS run uses so a missing board can never silently truncate PnL.
  Error = 1,
};

// Admission policy for archived surfaces consumed by a backtest. Compatibility
// preserves the historical behavior. RequireAdmittedRisk fails closed unless
// every loaded surface is an admitted, currently serviceable risk surface.
enum class SurfaceProvenancePolicy : std::uint8_t {
  Compatibility = 0,
  RequireAdmittedRisk = 1,
};

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
  FrictionModel frictions{};          // execution frictions (B2; default: frictionless)
  FinancingConfig financing{};        // cash/borrow ledger (B2; default: off => B1-identity)
  unsigned record_every_n{1}; // positive; persist every Nth step (1 = every step)
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
  // A null cache creates a private per-run cache. Supplying one permits archive
  // reuse across backtest and reconciliation passes. Private one-pass caches retain
  // at most the current/base/look-ahead working set. Look-ahead overlaps the next
  // archive open/map with pricing and strategy work on the current step.
  std::shared_ptr<SnapshotCache> snapshot_cache{};
  bool prefetch_snapshots{true};
  // Appended for positional aggregate source compatibility. ReuseOnly consumes
  // a prepared fast snapshot but loads a separately-keyed cold snapshot on a
  // fast miss. Eager preserves the historical requested-tier behavior.
  QueryCacheBuildPolicy query_cache_build_policy{QueryCacheBuildPolicy::Eager};
  // Appended for positional aggregate source compatibility. Strict production
  // backtests opt in; the default deliberately preserves legacy archives.
  SurfaceProvenancePolicy surface_provenance_policy{SurfaceProvenancePolicy::Compatibility};
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
};

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
  std::vector<double> n_open_lots;
  // Positions whose surface was absent this step; their PnL and greeks are EXCLUDED
  // from this row's totals. 0.0 at inception. Under RunConfig::unpriced == Error a
  // step with a non-zero count aborts the run instead of recording a row.
  // WS-F F1(b): a HEDGE-SHARE ledger entry whose base or shifted surface is absent
  // over the step is counted here too (its share MTM and financing are excluded
  // from this row exactly as an unpriced lot's PnL is), so the column stays the
  // single honest "positions this row could not value" count.
  std::vector<double> n_unpriced_lots;
  // Positions whose surface was absent on THIS row's date; their greeks are EXCLUDED
  // from this row's `gross_*`. Distinct from `n_unpriced_lots`, which measures the
  // step's PnL completeness (base AND shifted): `book_greeks` prices a single-date
  // snapshot against this row's date alone, so the two counts can diverge (a held
  // lot absent from the step's base but present again on this row's date is counted
  // in `n_unpriced_lots` but NOT here). Filled on EVERY row including row 0 —
  // inception computes book greeks, so its count is a real measurement, not 0.0 by
  // convention. Under RunConfig::unpriced == Error a row with a non-zero count aborts.
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
};

// B0 driver: MTM a FIXED hand-built book forward across the clock. Canonical
// loop: base = load(refs[0]); for i in 1..N-1 { shifted = load(refs[i]);
// pnl_explain(base -> shifted); settle expiries observed exactly at shifted.ts;
// record @ granularity; base = std::move(shifted); }. NotFound is returned when
// the clock crosses a held lot's expiry without an exact timestamp observation.
[[nodiscard]] Result<BacktestResult> run_backtest(const Clock &clock, PortfolioState initial,
                                                  const RunConfig &cfg = {});

// B1 driver: the strategy-aware overload. `strat.on_step` runs at inception
// (step 0) and after each move-swap on the new base — opening entries / rolling
// cohorts / closing lots — then the same resolve-today -> pnl_explain-forward ->
// move-swap loop MTMs the evolving book. Book greeks and `signals(base)` are
// recorded AFTER each step's entries. Settlement of expiring lots is engine-owned
// (at intrinsic), identical to the fixed-book overload and subject to the same
// exact-expiry-observation contract.
[[nodiscard]] Result<BacktestResult> run_backtest(const Clock &clock, IStrategy &strat,
                                                  const RunConfig &cfg = {});

} // namespace atx::vol
