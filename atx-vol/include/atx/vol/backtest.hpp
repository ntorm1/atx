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
// book is handed in whole and held to expiry; expiring lots settle at intrinsic.
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
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "atx/vol/corpus.hpp"           // CorpusManifest
#include "atx/vol/portfolio_pricer.hpp" // OptionContract, SurfaceSet, PriceOptions, PriceTotals
#include "atx/vol/priced_surface.hpp"   // PricedSurface
#include "atx/vol/types.hpp"            // Result, Side

namespace atx::vol {

class IStrategy;  // strategy.hpp — drives the strategy-aware run_backtest overload

// ── Timeline ────────────────────────────────────────────────────────────────

// One dated market snapshot in the backtest timeline: the corpus date string and
// the on-disk archive that holds that date's fitted surface(s).
struct SnapshotRef {
  std::string date;
  std::string archive_path;
};

// The backtest timeline enumerated from a corpus manifest: one `SnapshotRef` per
// unique date (ascending) pointing at that date's first Ok archive.
class Clock {
 public:
  [[nodiscard]] static Result<Clock> from_manifest(const CorpusManifest& manifest);

  [[nodiscard]] std::span<const SnapshotRef> refs() const noexcept { return refs_; }
  [[nodiscard]] std::size_t size() const noexcept { return refs_.size(); }

 private:
  std::vector<SnapshotRef> refs_;
};

// ── Snapshot loader ─────────────────────────────────────────────────────────

// A single loaded market date: owns the archive's `PricedSurface`s and holds a
// non-owning `SurfaceSet` over their (stable) addresses. Move-only; the move
// leaves the `SurfaceSet` pointers valid (vector move preserves element
// addresses and `surfaces_` is never mutated after `set_` is built).
class MarketSnapshot {
 public:
  // Open `archive_path`, map every surface, build the `SurfaceSet`, and take the
  // valuation timestamp from the surfaces' `now_ts_ns` (validating they agree).
  // Errors propagate from the archive open/map or `SurfaceSet::create`;
  // InvalidArgument if the archive is empty or its surfaces disagree on the ts.
  [[nodiscard]] static Result<MarketSnapshot> load(std::string_view archive_path);

  MarketSnapshot(MarketSnapshot&&) noexcept = default;
  MarketSnapshot& operator=(MarketSnapshot&&) noexcept = default;
  MarketSnapshot(const MarketSnapshot&) = delete;
  MarketSnapshot& operator=(const MarketSnapshot&) = delete;

  [[nodiscard]] const SurfaceSet& set() const noexcept { return set_; }
  [[nodiscard]] const PricedSurface* find(std::uint32_t uid) const noexcept {
    return set_.find(uid);
  }
  [[nodiscard]] std::int64_t ts_ns() const noexcept { return ts_ns_; }
  [[nodiscard]] std::optional<std::uint32_t> uid_of(std::string_view symbol) const;

  // Test seam: how many archives have been opened process-wide (the load-once
  // gate resets this, runs, and asserts it equals the ref count).
  [[nodiscard]] static std::uint64_t open_count() noexcept;
  static void reset_open_count() noexcept;

 private:
  MarketSnapshot(std::vector<PricedSurface>&& surfaces, SurfaceSet&& set, std::int64_t ts,
                 std::vector<std::pair<std::string, std::uint32_t>>&& syms) noexcept;

  std::vector<PricedSurface> surfaces_;                        // owned (map_all)
  SurfaceSet set_;                                             // non-owning over surfaces_
  std::int64_t ts_ns_{0};
  std::vector<std::pair<std::string, std::uint32_t>> syms_;    // symbol -> uid
};

// ── Book state (absolute-expiry aging) ──────────────────────────────────────

// One open lot. `contract.T` is re-derived each step as (expiry_ts_ns -
// base.ts)/year; `expiry_ts_ns` is the fixed anchor that drives both the aging
// residual and the settlement trigger.
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

// ── Run config + result ─────────────────────────────────────────────────────

struct RunConfig {
  PriceOptions price{};              // pricer thread fan-out (bit-deterministic)
  unsigned record_every_n{1};        // persist every Nth step (1 = every step)
  bool retain_position_frames{false};  // reserved for B1 (per-position frames)
};

// SoA time series. Row 0 is inception (all-zero PnL, nav 0, book greeks on the
// first date); each later recorded row is one priced step, downsampled by
// `record_every_n` (the final step is always recorded). `pnl_*` are per-step;
// `nav` is the cumulative Σ pnl_total (incl. settlement) from inception = 0.
struct BacktestResult {
  std::vector<std::string> date;
  std::vector<std::int64_t> ts_ns;
  std::vector<double> pnl_total, pnl_delta, pnl_gamma, pnl_vega, pnl_vanna, pnl_volga, pnl_theta,
      pnl_rho, pnl_charm, pnl_unexplained;
  std::vector<double> pnl_settlement;  // intrinsic settlement PnL this step
  std::vector<double> nav;             // cumulative from inception = 0
  std::vector<double> gross_delta, gross_gamma, gross_vega, gross_theta;  // book greeks on the base
  std::vector<double> n_open_lots;
  // Strategy diagnostics: name -> per-recorded-row series (parallel to `date`).
  // Empty for the fixed-book overload; populated by the IStrategy overload.
  std::vector<std::pair<std::string, std::vector<double>>> signals;

  [[nodiscard]] std::size_t size() const noexcept { return date.size(); }
};

// B0 driver: MTM a FIXED hand-built book forward across the clock. Canonical
// loop: base = load(refs[0]); for i in 1..N-1 { shifted = load(refs[i]);
// pnl_explain(base -> shifted); settle expiries; record @ granularity;
// base = std::move(shifted); }.
[[nodiscard]] Result<BacktestResult> run_backtest(const Clock& clock, PortfolioState initial,
                                                  const RunConfig& cfg = {});

// B1 driver: the strategy-aware overload. `strat.on_step` runs at inception
// (step 0) and after each move-swap on the new base — opening entries / rolling
// cohorts / closing lots — then the same resolve-today -> pnl_explain-forward ->
// move-swap loop MTMs the evolving book. Book greeks and `signals(base)` are
// recorded AFTER each step's entries. Settlement of expiring lots is engine-owned
// (at intrinsic), identical to the fixed-book overload.
[[nodiscard]] Result<BacktestResult> run_backtest(const Clock& clock, IStrategy& strat,
                                                  const RunConfig& cfg = {});

}  // namespace atx::vol
