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
  [[nodiscard]] static Result<MarketSnapshot>
  load(std::string_view archive_path,
       QueryPricingTier query_pricing_tier = QueryPricingTier::LegacyCompatible,
       std::span<const std::uint32_t> referenced_uids = {});

  MarketSnapshot(MarketSnapshot &&) noexcept = default;
  MarketSnapshot &operator=(MarketSnapshot &&) noexcept = default;
  MarketSnapshot(const MarketSnapshot &) = delete;
  MarketSnapshot &operator=(const MarketSnapshot &) = delete;

  [[nodiscard]] const SurfaceSet &set() const noexcept { return set_; }
  [[nodiscard]] const PricedSurface *find(std::uint32_t uid) const noexcept {
    return set_.find(uid);
  }
  // Same-archive provenance for uid. Pointer lifetime matches this snapshot;
  // lookup performs no allocation and returns nullptr for an unknown uid.
  [[nodiscard]] const SurfaceProvenance *provenance(std::uint32_t uid) const noexcept;
  // Provenance aligned one-for-one with surfaces() in archive order. The view is
  // invalidated only when this snapshot is destroyed or moved from.
  [[nodiscard]] std::span<const SurfaceProvenance> provenances() const noexcept {
    return provenance_;
  }
  // Read-only view of the owned surfaces (archive order; always non-empty after a
  // successful load). Used by the financing ledger to read a representative
  // base-date rate. Safe across a move (vector move preserves element addresses).
  [[nodiscard]] std::span<const PricedSurface> surfaces() const noexcept { return surfaces_; }
  [[nodiscard]] std::int64_t ts_ns() const noexcept { return ts_ns_; }
  [[nodiscard]] std::optional<std::uint32_t> uid_of(std::string_view symbol) const;

  // Test seam: how many archives have been opened process-wide (the load-once
  // gate resets this, runs, and asserts it equals the ref count).
  [[nodiscard]] static std::uint64_t open_count() noexcept;
  static void reset_open_count() noexcept;

private:
  MarketSnapshot(std::vector<PricedSurface> &&surfaces, std::vector<SurfaceProvenance> &&provenance,
                 SurfaceSet &&set, std::int64_t ts,
                 std::vector<std::pair<std::string, std::uint32_t>> &&syms) noexcept;

  std::vector<PricedSurface> surfaces_;       // owned (archive directory order)
  std::vector<SurfaceProvenance> provenance_; // same-blob, parallel to surfaces_
  SurfaceSet set_;                            // non-owning over surfaces_
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
  // Bounded LRU mode. A zero capacity is defensively normalized to one entry.
  explicit SnapshotCache(std::size_t max_retained_entries);
  // B1: bounded LRU + subset-deserialize. Every load/prefetch through this cache
  // deserializes ONLY the archive surfaces whose uid appears in `referenced_uids`
  // (empty => whole board). Intended for a PRIVATE per-run cache whose single book's
  // referenced uids are known up front (the fixed-book run_backtest overload); do NOT
  // share such a cache across books with different referenced sets (a subset snapshot
  // cached under one book would be missing another's uids).
  SnapshotCache(std::size_t max_retained_entries, std::vector<std::uint32_t> referenced_uids);
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
  double per_contract_cost{0.0};  // $ per option contract traded
  double hedge_slippage_bps{0.0}; // shares fill at S * (1 +/- bps/1e4)
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
};

// ── Run config + result ─────────────────────────────────────────────────────

// What to do when a HELD (non-expiring) lot cannot be valued for step P&L or book
// Greeks. This policy does not apply to a strategy-driven roll close: omitting a
// close mark would destroy cash/economic value, so the executor always fails closed.
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
  unsigned record_every_n{1};         // positive; persist every Nth step (1 = every step)
  bool retain_position_frames{false}; // reserved for B1 (per-position frames)
  // Policy for held P&L/Greek valuation only. Strategy close execution always
  // requires an economically valid mark regardless of this setting.
  UnpricedLotPolicy unpriced{UnpricedLotPolicy::ExcludeAndReport};
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
// zero PnL, book greeks on the first date); each later recorded row is one priced step, downsampled by
// `record_every_n` (the final step is always recorded). `pnl_*` are per-step;
// `nav` is the cumulative Σ pnl_total (incl. settlement) from inception = 0.
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
  std::vector<double> gross_delta, gross_gamma, gross_vega, gross_theta; // book greeks on the base
  std::vector<double> turnover_notional, turnover_vega; // traded |notional| / |vega| this step
  std::vector<double> n_open_lots;
  // Positions whose surface was absent this step; their PnL and greeks are EXCLUDED
  // from this row's totals. 0.0 at inception. Under RunConfig::unpriced == Error a
  // step with a non-zero count aborts the run instead of recording a row.
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
