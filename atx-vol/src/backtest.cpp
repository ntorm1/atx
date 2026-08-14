// atx-vol backtest engine (Phase B0) — see backtest.hpp for the model.

#include "atx/vol/backtest.hpp"

#include <algorithm>
#include <atomic>
#include <bit>
#include <cassert>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <numeric>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "step_mark_memo.hpp" // detail::StepMarkMemo — the L2 settlement mark memo

#include "atx/core/error.hpp"
#include "atx/vol/detail/backtest_series_columns.hpp" // the 25 {name, member} series columns
#include "atx/vol/detail/counters.hpp"      // counters::ledger — V1 always-on solve ledger (per-step scrape)
#include "atx/vol/deriv_pnl.hpp"               // Task F-8: the swap lane's P&L explain
#include "atx/vol/detail/deriv_ref_bridge.hpp" // detail::deriv_price_on_ref — swap-lot marks
#include "atx/vol/detail/phase_profile.hpp"
#include "atx/vol/strategy.hpp"        // IStrategy
#include "atx/vol/surface_archive.hpp" // SurfaceArchive
#include "atx/vol/surface_db.hpp"      // SurfaceDb, DbPartitionInfo, kSurfaceDbPartitionDir/Ext
#include "atx/vol/universe.hpp"        // canonical_symbol

namespace atx::vol {

using atx::core::Err;
using atx::core::ErrorCode;
using atx::core::Ok;

namespace {

// Process-wide archive-open counter (test seam). Loads increment it exactly once.
std::atomic<std::uint64_t> g_open_count{0};

// WS-F F5 (BT-T2). Process-wide count of SURFACE-RECORD BYTES actually
// materialized by snapshot loads — the sum of `ArchiveV2DirEntry::surface_size`
// over every directory entry a load turns into a surface or a view.
//
// This is deliberately a DETERMINISTIC counter, not a timing number: it is the
// same value on a busy host as on a quiet one, so a subset-vs-whole-board claim
// is a fact rather than a measurement. It counts record bytes, not resident
// pages: on the borrowed (mapped) tiers the OS faults in only what is read, so
// this is the upper bound the subset actually shrinks.
std::atomic<std::uint64_t> g_deserialized_bytes{0};

// Bounded-cache capacity for a look-ahead of `depth`: exactly the live working set
// of a forward-only run — `base` + `shifted` + `depth` snapshots in flight.
//
// This is derivable rather than tuned only because SnapshotCache's bounded mode
// evicts in INSERTION order, so the eviction victim is always the lowest-indexed
// entry, which a forward-only walk has already passed. Under the recency order it
// used to keep, the victim was instead an unconsumed prefetch (sequential
// flooding), and no capacity of the form depth + k was reliably safe: depth + 3
// happened to work at depth 4 and still reloaded at depth 8. See the eviction-order
// comment in snapshot_cache.cpp.
//
// `Backtest.PrefetchDepthIsBitIdenticalToSingleStepLookAhead` pins this by asserting
// `MarketSnapshot::open_count() == refs.size()` at several depths — the assertion that
// fails if either the order or this capacity regresses, since a reload does not change
// the ECONOMICS.
[[nodiscard]] constexpr std::size_t private_snapshot_cache_capacity(std::size_t depth) noexcept {
  return depth + 2u;
}

// Look-ahead depth actually used: 0 is normalized to 1, because "no look-ahead"
// is expressed by prefetch_snapshots == false, not by a zero depth.
[[nodiscard]] constexpr std::size_t effective_prefetch_depth(const RunConfig &cfg) noexcept {
  return cfg.prefetch_depth == 0u ? 1u : cfg.prefetch_depth;
}

// Issue the look-ahead prefetches for `refs[first .. first+count)`, skipping past
// the end of the clock. Callers issue this TWICE per run and once per step: an
// initial fill of the whole window before the loop, then exactly the ONE ref that
// newly enters the window at each step. Deliberately NOT a per-step rescan of the
// whole window — `SnapshotCache::prefetch` reads the candidate archive's identity
// header on every call (a small file open + read) to fail closed on an archive
// rewritten in place, so rescanning would multiply that probe by the depth for no
// added look-ahead. That per-call probe is now memoized under `Sealed` backing
// (`SnapshotCache::identity_for`, snapshot_cache.cpp), which is what `run_backtest`'s
// private cache uses, so on the default Sealed replay path this cost argument no
// longer applies — it holds only for a caller-supplied `Mutable` cache, and on
// Sealed the reason to keep one call per ref per run is instead probe-count parity
// with the single-step look-ahead already paid.
//
// A prefetch error is propagated, not swallowed: it means the cache could not be
// asked (an invalid build policy), which is a programming error, and the load that
// would follow is going to fail anyway.
[[nodiscard]] Status prefetch_window(SnapshotCache &cache, std::span<const SnapshotRef> refs,
                                     std::size_t first, std::size_t count, const RunConfig &cfg) {
  const std::size_t last = first + count < refs.size() ? first + count : refs.size();
  for (std::size_t k = first; k < last; ++k) {
    ATX_TRY_VOID(
        cache.prefetch(refs[k].archive_path, cfg.query_pricing_tier, cfg.query_cache_build_policy));
  }
  return Ok();
}

// Contract residual T on the snapshot dated `base_ts`: (expiry - base.ts)/year.
[[nodiscard]] double residual_T(std::int64_t expiry_ts_ns, std::int64_t base_ts_ns) noexcept {
  if (expiry_ts_ns >= base_ts_ns) {
    const std::uint64_t delta =
        static_cast<std::uint64_t>(expiry_ts_ns) - static_cast<std::uint64_t>(base_ts_ns);
    return static_cast<double>(delta) / kNsPerYear;
  }
  const std::uint64_t delta =
      static_cast<std::uint64_t>(base_ts_ns) - static_cast<std::uint64_t>(expiry_ts_ns);
  return -static_cast<double>(delta) / kNsPerYear;
}

// Materialize the current book as `Position`s priced at `base_ts`'s residual T.
[[nodiscard]] std::vector<Position> positions_at(const std::vector<Lot> &lots,
                                                 std::int64_t base_ts_ns) {
  std::vector<Position> out;
  out.reserve(lots.size());
  for (const Lot &lot : lots) {
    const double T = residual_T(lot.expiry_ts_ns, base_ts_ns);
    out.push_back(Position{lot.id,
                           OptionContract{lot.contract.uid, lot.contract.K, T, lot.contract.side},
                           lot.qty, lot.multiplier});
  }
  return out;
}

class RetainedBookPricer {
public:
  [[nodiscard]] Result<PortfolioPricer *> prepare(const std::vector<Lot> &lots,
                                                  std::int64_t valuation_ts) {
    if (!same_book(lots)) {
      ATX_TRY(Portfolio portfolio, Portfolio::create(positions_at(lots, valuation_ts)));
      PortfolioPricer next(std::move(portfolio));
      // PortfolioWorkspace is grow-only and validates its retained substrate by
      // logical book identity. Preserve its buffers across book changes instead
      // of destroying and reallocating the high-water mark.
      workspace_.reserve(next.portfolio().n_contracts(), next.portfolio().n_positions());
      // L1 (AL-solve-wall sprint, fewer-solves): when the new book is a SUBSET of the
      // outgoing one — an expiry/roll settlement shrinks the alive set — re-home the
      // surviving uniques' retained base-risk bundle onto `next` so the following
      // pnl solve REUSES it instead of re-solving a full-Greek bundle per survivor
      // across the membership change (the expiry-day 11 -> 6 solve-equivs/unit win).
      // Bit-identical by construction (the retained row is the same per-lane solve a
      // fresh path would produce); fails closed to the ordinary full solve on an
      // add / field change / first book, and the pnl reuse guard independently
      // re-validates the base surface, so a stale carry can only fall back, never
      // serve wrong risk.
      if (pricer_.has_value()) {
        (void)next.carry_base_risk_subset(*pricer_, workspace_);
      }
      pricer_ = std::move(next);
      key_ = lots;
    } else {
      tenors_.resize(lots.size());
      for (std::size_t i = 0; i < lots.size(); ++i) {
        tenors_[i] = residual_T(lots[i].expiry_ts_ns, valuation_ts);
      }
      ATX_TRY_VOID(pricer_->retime(tenors_));
    }
    return &*pricer_;
  }

  [[nodiscard]] PortfolioWorkspace &workspace() noexcept { return workspace_; }

  [[nodiscard]] std::vector<Lot> &reset_alive_scratch(std::size_t capacity) {
    alive_.clear();
    if (alive_.capacity() < capacity) {
      // R-35: grow geometrically, not to the EXACT requested capacity. A book
      // that grows a lot at a time across steps would otherwise trigger an
      // exact reserve every step (O(n^2) reallocations); doubling keeps the
      // amortized growth this reused scratch buffer is meant to provide.
      alive_.reserve(std::max(capacity, alive_.capacity() * 2u));
    }
    return alive_;
  }

private:
  [[nodiscard]] bool same_book(const std::vector<Lot> &lots) const noexcept {
    if (!pricer_.has_value() || key_.size() != lots.size()) {
      return false;
    }
    for (std::size_t i = 0; i < lots.size(); ++i) {
      const Lot &a = key_[i];
      const Lot &b = lots[i];
      if (a.id != b.id || a.contract.uid != b.contract.uid || a.contract.K != b.contract.K ||
          a.contract.side != b.contract.side || a.qty != b.qty || a.multiplier != b.multiplier ||
          a.expiry_ts_ns != b.expiry_ts_ns || a.cohort != b.cohort) {
        return false;
      }
    }
    return true;
  }

  std::vector<Lot> key_;
  std::vector<double> tenors_;
  std::vector<Lot> alive_;
  std::optional<PortfolioPricer> pricer_;
  PortfolioWorkspace workspace_;
};

// L2 (AL-solve-wall sprint): per-step per-(unique-contract, base-surface) mark memo.
// Defined in `src/step_mark_memo.hpp` so its admission contract has a direct unit
// test; used unqualified here exactly as when it lived in this file.
using detail::StepMarkMemo;

[[nodiscard]] bool same_double_bits(double lhs, double rhs) noexcept {
  return std::bit_cast<std::uint64_t>(lhs) == std::bit_cast<std::uint64_t>(rhs);
}

[[nodiscard]] bool valid_side(Side side) noexcept {
  switch (side) {
  case Side::Call:
  case Side::Put:
    return true;
  default:
    return false;
  }
}

[[nodiscard]] bool valid_query_pricing_tier(QueryPricingTier tier) noexcept {
  switch (tier) {
  case QueryPricingTier::LegacyCompatible:
  case QueryPricingTier::ColdReference:
  case QueryPricingTier::RepresentativeFast:
  case QueryPricingTier::CarryBank:
    return true;
  default:
    return false;
  }
}

[[nodiscard]] bool valid_query_cache_build_policy(QueryCacheBuildPolicy policy) noexcept {
  switch (policy) {
  case QueryCacheBuildPolicy::Eager:
  case QueryCacheBuildPolicy::ReuseOnly:
    return true;
  default:
    return false;
  }
}

[[nodiscard]] bool valid_query_execution(QueryExecution execution) noexcept {
  switch (execution) {
  case QueryExecution::Configured:
  case QueryExecution::ColdReference:
    return true;
  default:
    return false;
  }
}

[[nodiscard]] bool valid_resolved_price_isa(simd::SimdIsa isa) noexcept {
  switch (isa) {
  case simd::SimdIsa::Auto:
  case simd::SimdIsa::ForceScalar:
  case simd::SimdIsa::ForceAvx2:
    return true;
  default:
    return false;
  }
}

[[nodiscard]] bool valid_spread_kind(FrictionModel::SpreadKind kind) noexcept {
  switch (kind) {
  case FrictionModel::SpreadKind::None:
  case FrictionModel::SpreadKind::PriceBps:
  case FrictionModel::SpreadKind::VolTicks:
    return true;
  default:
    return false;
  }
}

[[nodiscard]] bool valid_unpriced_policy(UnpricedLotPolicy policy) noexcept {
  switch (policy) {
  case UnpricedLotPolicy::ExcludeAndReport:
  case UnpricedLotPolicy::Error:
    return true;
  default:
    return false;
  }
}

[[nodiscard]] bool valid_provenance_policy(SurfaceProvenancePolicy policy) noexcept {
  switch (policy) {
  case SurfaceProvenancePolicy::Compatibility:
  case SurfaceProvenancePolicy::RequireAdmittedRisk:
    return true;
  default:
    return false;
  }
}

[[nodiscard]] Status validate_run_config(const RunConfig &cfg) {
  if (cfg.record_every_n == 0u) {
    return Err(ErrorCode::InvalidArgument, "run_backtest: record_every_n must be positive");
  }
  if (!valid_query_pricing_tier(cfg.query_pricing_tier) ||
      !valid_query_cache_build_policy(cfg.query_cache_build_policy) ||
      !valid_query_execution(cfg.price.query_execution) ||
      !valid_resolved_price_isa(cfg.price.resolved_price_isa) ||
      !valid_spread_kind(cfg.frictions.spread_kind) || !valid_unpriced_policy(cfg.unpriced) ||
      !valid_provenance_policy(cfg.surface_provenance_policy)) {
    return Err(ErrorCode::InvalidArgument, "run_backtest: invalid RunConfig enum value");
  }
  if (cfg.price.prices_only) {
    return Err(ErrorCode::InvalidArgument,
               "run_backtest: prices_only cannot supply required backtest Greeks");
  }
  if (!std::isfinite(cfg.price.sticky.ref_uprc_weight)) {
    return Err(ErrorCode::InvalidArgument, "run_backtest: sticky ref_uprc_weight must be finite");
  }
  const auto finite_nonnegative = [](double value) noexcept {
    return std::isfinite(value) && value >= 0.0;
  };
  if (!finite_nonnegative(cfg.frictions.half_spread_bps) ||
      !finite_nonnegative(cfg.frictions.vol_tick) ||
      !finite_nonnegative(cfg.frictions.impact_fraction) ||
      !finite_nonnegative(cfg.frictions.per_contract_cost) ||
      !finite_nonnegative(cfg.frictions.hedge_slippage_bps)) {
    return Err(ErrorCode::InvalidArgument,
               "run_backtest: friction inputs must be finite and nonnegative");
  }
  if (!std::isfinite(cfg.financing.initial_cash)) {
    return Err(ErrorCode::InvalidArgument, "run_backtest: initial_cash must be finite");
  }
  if (!finite_nonnegative(cfg.financing.borrow_rate)) {
    return Err(ErrorCode::InvalidArgument,
               "run_backtest: borrow_rate must be finite and nonnegative");
  }
  for (const ShareDividend &div : cfg.financing.share_dividends) {
    if (div.uid == 0u || div.ex_ts_ns <= 0 || !finite_nonnegative(div.amount)) {
      return Err(ErrorCode::InvalidArgument,
                 "run_backtest: share dividend needs a real uid, a positive ex-date and a "
                 "finite nonnegative amount");
    }
  }
  if (cfg.reconcile_nav && !(std::isfinite(cfg.reconcile_nav_tol) && cfg.reconcile_nav_tol > 0.0)) {
    return Err(ErrorCode::InvalidArgument,
               "run_backtest: reconcile_nav_tol must be finite and positive");
  }
  return Ok();
}

[[nodiscard]] Status validate_lot_economics(const Lot &lot, std::string_view owner,
                                            std::optional<std::int64_t> base_ts) {
  const auto invalid = [&lot, owner](std::string_view field) {
    return Err(ErrorCode::InvalidArgument, "run_backtest: lot id=" + std::to_string(lot.id) +
                                               " in " + std::string{owner} + " has invalid " +
                                               std::string{field});
  };
  if (!std::isfinite(lot.contract.K) || lot.contract.K <= 0.0) {
    return invalid("strike K");
  }
  if (!std::isfinite(lot.contract.T)) {
    return invalid("contract T");
  }
  if (!valid_side(lot.contract.side)) {
    return invalid("option side");
  }
  if (!std::isfinite(lot.qty)) {
    return invalid("qty");
  }
  if (!std::isfinite(lot.multiplier) || lot.multiplier <= 0.0) {
    return invalid("multiplier");
  }
  if (!std::isfinite(lot.entry_price) || lot.entry_price < 0.0) {
    return invalid("entry_price");
  }
  if (base_ts.has_value() && lot.expiry_ts_ns <= *base_ts) {
    return invalid("expiry_ts_ns (must be after base)");
  }
  return Ok();
}

[[nodiscard]] Status validate_lot_economics(std::span<const Lot> lots, std::string_view owner,
                                            std::optional<std::int64_t> base_ts = std::nullopt) {
  for (const Lot &lot : lots) {
    ATX_TRY_VOID(validate_lot_economics(lot, owner, base_ts));
  }
  return Ok();
}

[[nodiscard]] Status validate_hedge_spec(const HedgeSpec &spec) {
  switch (spec.kind) {
  case HedgeSpec::Kind::None:
  case HedgeSpec::Kind::DeltaToZero:
    break;
  default:
    return Err(ErrorCode::InvalidArgument, "run_backtest: invalid hedge kind");
  }
  switch (spec.cadence) {
  case HedgeSpec::Cadence::AtEntry:
  case HedgeSpec::Cadence::Daily:
    break;
  default:
    return Err(ErrorCode::InvalidArgument, "run_backtest: invalid hedge cadence");
  }
  if (!std::isfinite(spec.band) || spec.band < 0.0) {
    return Err(ErrorCode::InvalidArgument,
               "run_backtest: hedge band must be finite and nonnegative");
  }
  return Ok();
}

// A lot ID is the executor's identity key. Once emitted, every economic field is
// immutable; a strategy expresses a resize/restrike as close-old plus open-new.
[[nodiscard]] bool same_lot_identity(const Lot &lhs, const Lot &rhs) noexcept {
  return lhs.id == rhs.id && lhs.contract.uid == rhs.contract.uid &&
         same_double_bits(lhs.contract.K, rhs.contract.K) &&
         same_double_bits(lhs.contract.T, rhs.contract.T) &&
         lhs.contract.side == rhs.contract.side && same_double_bits(lhs.qty, rhs.qty) &&
         same_double_bits(lhs.multiplier, rhs.multiplier) && lhs.expiry_ts_ns == rhs.expiry_ts_ns &&
         lhs.cohort == rhs.cohort && same_double_bits(lhs.entry_price, rhs.entry_price);
}

// Task F-3 fix round 1 (I-3): the admission LIST now lives in `backtest.hpp`
// as `engine_supports_swap_kind`, so the strategy layer's swap-leg spec
// validation reaches the identical verdict instead of carrying a second
// hand-written copy of it -- the same two-copies hazard that produced F-3's
// own C-1. This wrapper is kept because it names the engine-boundary question
// at the two call sites below.
//
// Why the two refused kinds are refused (unchanged from F-2 / F-3): GammaSwap
// needs the S_i/S0-weighted accumulator wired through the live daily-fixing
// loop, and CorridorVarSwap needs BOTH a corridor-aware accrual and corridor
// bounds on `SwapLot`, which that struct does not carry -- so a corridor lot
// would blend a corridor FUTURE leg with a non-corridor REALIZED one. Both
// fail the WHOLE run loud at `validate_swap_lot_economics` below rather than
// silently mis-accruing a live position.
[[nodiscard]] bool valid_deriv_kind(DerivKind kind) noexcept {
  return engine_supports_swap_kind(kind);
}

// Task F-5 (pre-feature refactor): the two payoff-shape predicates that used
// to live here as `kind ==` chains -- `is_vol_kind` and `is_capped_kind` --
// now live beside the enum itself as `deriv_kind_settles_in_vol` /
// `deriv_kind_is_capped` (derivatives.hpp). The capped one had THREE
// hand-written copies across three translation units; see that header for the
// full rationale and for why both are now exhaustive switches.

// Boundary validation for one swap lot. `base_ts` is present whenever the lot
// must still be live (a strategy's post-step book, a checkpoint's), absent for a
// pure structural check. Mirrors validate_lot_economics for the option lane; the
// cap rule is `deriv_price`'s own contract, checked HERE so a malformed lot is
// rejected at the engine boundary instead of failing on its first mark.
[[nodiscard]] Status validate_swap_lot_economics(const SwapLot &lot, std::string_view owner,
                                                 std::optional<std::int64_t> base_ts) {
  const auto invalid = [&lot, owner](std::string_view field) {
    return Err(ErrorCode::InvalidArgument, "run_backtest: swap lot id=" + std::to_string(lot.id) +
                                               " in " + std::string{owner} + " has invalid " +
                                               std::string{field});
  };
  if (lot.uid == 0u) {
    return invalid("uid");
  }
  if (!valid_deriv_kind(lot.kind)) {
    return invalid("kind");
  }
  if (!std::isfinite(lot.strike_dec) || lot.strike_dec < 0.0) {
    return invalid("strike_dec");
  }
  if (!std::isfinite(lot.cap_dec) || lot.cap_dec < 0.0) {
    return invalid("cap_dec");
  }
  if (deriv_kind_is_capped(lot.kind) != (lot.cap_dec > 0.0)) {
    return invalid("cap_dec (required by a capped kind, forbidden otherwise)");
  }
  if (!std::isfinite(lot.notional) || lot.notional <= 0.0) {
    return invalid("notional");
  }
  if (!std::isfinite(lot.qty)) {
    return invalid("qty");
  }
  if (lot.n_obs_total == 0u) {
    return invalid("n_obs_total");
  }
  if (!std::isfinite(lot.annualization) || lot.annualization <= 0.0) {
    return invalid("annualization");
  }
  if (lot.start_ts_ns > lot.expiry_ts_ns) {
    return invalid("start_ts_ns (must not be after expiry)");
  }
  if (base_ts.has_value() && lot.expiry_ts_ns <= *base_ts) {
    return invalid("expiry_ts_ns (must be after base)");
  }
  return Ok();
}

[[nodiscard]] Status validate_swap_lot_economics(std::span<const SwapLot> lots,
                                                 std::string_view owner,
                                                 std::optional<std::int64_t> base_ts) {
  for (const SwapLot &lot : lots) {
    ATX_TRY_VOID(validate_swap_lot_economics(lot, owner, base_ts));
  }
  return Ok();
}

// A swap lot's ID is its identity key, exactly as an option lot's is: once
// emitted every economic field is immutable, and a resize/restrike is expressed
// as close-old plus open-new.
[[nodiscard]] bool same_swap_lot_identity(const SwapLot &lhs, const SwapLot &rhs) noexcept {
  return lhs.id == rhs.id && lhs.uid == rhs.uid && lhs.kind == rhs.kind &&
         same_double_bits(lhs.strike_dec, rhs.strike_dec) &&
         same_double_bits(lhs.cap_dec, rhs.cap_dec) &&
         same_double_bits(lhs.notional, rhs.notional) && same_double_bits(lhs.qty, rhs.qty) &&
         lhs.start_ts_ns == rhs.start_ts_ns && lhs.expiry_ts_ns == rhs.expiry_ts_ns &&
         lhs.n_obs_total == rhs.n_obs_total &&
         same_double_bits(lhs.annualization, rhs.annualization);
}

// Grow-only sorted ID index. Rebuild changes only the active prefix once storage
// is warm and never reorders the authoritative lot vector, preserving reduction
// and execution arithmetic order.
class ReusableLotIdIndex {
public:
  [[nodiscard]] Status rebuild(std::span<const Lot> lots, std::string_view owner) {
    if (order_.size() < lots.size()) {
      order_.resize(lots.size());
    }
    active_size_ = lots.size();
    if (active_size_ == 0u) {
      return Ok();
    }
    std::iota(order_.begin(), order_.begin() + static_cast<std::ptrdiff_t>(active_size_),
              std::size_t{0});
    std::sort(order_.begin(), order_.begin() + static_cast<std::ptrdiff_t>(active_size_),
              [&lots](std::size_t lhs, std::size_t rhs) { return lots[lhs].id < lots[rhs].id; });
    for (std::size_t i = 1; i < active_size_; ++i) {
      const std::uint64_t id = lots[order_[i]].id;
      if (lots[order_[i - 1u]].id == id) {
        return Err(ErrorCode::InvalidArgument,
                   "run_backtest: duplicate lot id=" + std::to_string(id) + " in " +
                       std::string{owner});
      }
    }
    return Ok();
  }

  [[nodiscard]] std::optional<std::size_t> find(std::span<const Lot> lots,
                                                std::uint64_t id) const noexcept {
    if (active_size_ == 0u) {
      return std::nullopt;
    }
    const auto first = order_.begin();
    const auto last = first + static_cast<std::ptrdiff_t>(active_size_);
    const auto it =
        std::lower_bound(first, last, id, [&lots](std::size_t index, std::uint64_t key) {
          return lots[index].id < key;
        });
    if (it == last || lots[*it].id != id) {
      return std::nullopt;
    }
    return *it;
  }

private:
  std::vector<std::size_t> order_;
  std::size_t active_size_{0};
};

// B3: O(1) per-uid hedge-share ledger + allocation-free daily delta-hedge pass.
//
// Replaces the pre-B3 hot loop's three linear scans — the `shares` vector's
// O(book) get/add/sum, and the per-uid whole-frame delta rescan (O(book) per uid
// => O(book^2) per hedge step) plus the fresh `uids` vector heap-allocated on
// every hedge step (bottleneck #7). Share counts live in an insertion-ordered
// vector (deterministic iteration for sum() / financing / the hedge trade order)
// with a parallel hash index for O(1) get/add; the per-step scratch (per-uid
// option-delta aggregate, uid iteration order, dedup set) is retained and only
// clear()ed, so after warm-up (every hedged uid already resident) the pass makes
// no heap allocation.
//
// Class: pure-refactor. Determinism/parity: sum(), entries() and the hedge trade
// loop iterate in the SAME order the pre-B3 code did (book.lots dedup order, then
// shares insertion order), and per-uid option delta is summed in frame order
// (i ascending) exactly as the pre-B3 inner loop did, so the emitted hedge trades,
// cash, and net delta are BIT-IDENTICAL to the pre-B3 path (B5 parity + n_threads
// determinism gates verify this).
class HedgeLedger {
public:
  [[nodiscard]] double get(std::uint32_t uid) const noexcept {
    const auto it = index_.find(uid);
    return it == index_.end() ? 0.0 : shares_[it->second].second;
  }

  void add(std::uint32_t uid, double dn) {
    const auto it = index_.find(uid);
    if (it != index_.end()) {
      shares_[it->second].second += dn;
      return;
    }
    index_.emplace(uid, shares_.size());
    shares_.emplace_back(uid, dn);
  }

  // Sum of held shares across every uid, in insertion order (matches the pre-B3
  // shares_sum linear scan bit-for-bit).
  [[nodiscard]] double sum() const noexcept {
    double s = 0.0;
    for (const auto &kv : shares_) {
      s += kv.second;
    }
    return s;
  }

  // The share ledger in insertion order (the financing loop reads it; iteration
  // order is the pre-B3 `shares` order, so shares P&L / financing are unchanged).
  [[nodiscard]] std::span<const std::pair<std::uint32_t, double>> entries() const noexcept {
    return {shares_.data(), shares_.size()};
  }

  // One allocation-free daily delta-hedge pass. `frame` is the full-book risk frame
  // aligned to `lots`. For each uid — book.lots dedup order, then any remaining
  // ledger uids — net = summed-Ok-option-delta + held shares; when |net| > band,
  // trade -net shares at that uid's base spot (`spot_of(uid)`), charge slippage into
  // `cost`, settle the notional into `cash`, and record the shares. Bit-identical to
  // the pre-B3 overlay (same uid order, same per-uid frame-order delta sum, same
  // cash accumulation order).
  //
  // WS-F F1(a) (BT-P1-3): `spot_of` returns Result<double> and this pass returns
  // Status, so a uid with no base surface FAILS CLOSED instead of trading at spot
  // 0.0. The old signature returned a bare double whose missing-surface value was
  // 0.0, which made `cash -= shares_to_trade * 0.0` flatten a residual share
  // position for FREE and left the ledger at zero with no error and no flag.
  //
  // SP100 task 2: `spot_of` now answers Result<optional<double>>. `nullopt` means
  // "this uid has no board on this base and the caller's policy tolerates it": the
  // uid's fill is SKIPPED — its ledger position is left exactly as it was, which is
  // the whole point, since zeroing it at spot 0.0 is the original bug — and
  // `n_skipped` is bumped so the row can report the exclusion. Err still aborts, so
  // `UnpricedLotPolicy::Error` keeps the pre-change control flow verbatim.
  //
  // Steady-state allocation-free: the per-uid delta aggregate and the dedup set are
  // held as DENSE, generation-STAMPED vectors (not node-based unordered_map/set that
  // reallocate a node per key on every clear()+reinsert). A per-pass `generation_`
  // bump invalidates last pass's stamps in O(1) with no clears, so after warm-up
  // (every uid resident in `scratch_index_`) the pass performs no heap allocation.
  template <typename SpotOf>
  [[nodiscard]] Status hedge_daily(const std::vector<Lot> &lots, const PriceFrame &frame,
                                   double band, double hedge_slippage_bps, SpotOf &&spot_of,
                                   double &cash, double &cost, std::uint32_t &n_skipped) {
    ++generation_;
    order_.clear(); // vector clear keeps capacity (no per-element deallocation)
    // 1. Aggregate Ok option delta per uid in ONE frame pass (frame order per uid).
    for (std::size_t i = 0; i < frame.size(); ++i) {
      if (frame.status[i] != PriceStatus::Ok) {
        continue;
      }
      const std::size_t s = scratch_slot(frame.uid[i]);
      if (agg_gen_[s] != generation_) {
        agg_gen_[s] = generation_;
        agg_val_[s] = 0.0;
      }
      agg_val_[s] += frame.delta[i];
    }
    // 2. uid iteration order: book.lots dedup, then ledger uids not yet seen.
    for (const Lot &lot : lots) {
      const std::size_t s = scratch_slot(lot.contract.uid);
      if (seen_gen_[s] != generation_) {
        seen_gen_[s] = generation_;
        order_.push_back(lot.contract.uid);
      }
    }
    for (const auto &kv : shares_) {
      const std::size_t s = scratch_slot(kv.first);
      if (seen_gen_[s] != generation_) {
        seen_gen_[s] = generation_;
        order_.push_back(kv.first);
      }
    }
    // 3. Trade each uid whose net delta breaches the band.
    for (const std::uint32_t uid : order_) {
      const std::size_t s = scratch_slot(uid);
      const double option_delta = (agg_gen_[s] == generation_) ? agg_val_[s] : 0.0;
      const double net = option_delta + get(uid);
      if (std::fabs(net) > band) {
        const double shares_to_trade = -net;
        const Result<std::optional<double>> spot_res = spot_of(uid);
        if (!spot_res) {
          return Err(spot_res.error());
        }
        if (!spot_res->has_value()) {
          ++n_skipped; // no board, tolerated: leave the ledger and cash untouched
          continue;
        }
        const double spot = **spot_res;
        cost += std::fabs(shares_to_trade) * spot * (hedge_slippage_bps / 1.0e4);
        cash -= shares_to_trade * spot;
        add(uid, shares_to_trade);
      }
    }
    return Ok();
  }

private:
  // Dense scratch slot for `uid`; grows once per newly-seen uid and is never cleared,
  // so steady state allocates nothing. A freshly-grown slot's stamps are 0 (< any
  // live generation_, which starts at 1 after the first ++), i.e. "not this pass".
  [[nodiscard]] std::size_t scratch_slot(std::uint32_t uid) {
    const auto it = scratch_index_.find(uid);
    if (it != scratch_index_.end()) {
      return it->second;
    }
    const std::size_t s = agg_val_.size();
    scratch_index_.emplace(uid, s);
    agg_val_.push_back(0.0);
    agg_gen_.push_back(0);
    seen_gen_.push_back(0);
    return s;
  }

  std::vector<std::pair<std::uint32_t, double>> shares_; // insertion order (traded uids)
  std::unordered_map<std::uint32_t, std::size_t> index_; // uid -> slot in shares_

  // Per-step hedge scratch — dense, generation-stamped, allocation-free in steady state.
  std::unordered_map<std::uint32_t, std::size_t> scratch_index_; // uid -> dense scratch slot
  std::vector<double> agg_val_;         // per-uid summed option delta (valid iff agg_gen_==gen)
  std::vector<std::uint64_t> agg_gen_;  // stamp: agg_val_ valid this pass
  std::vector<std::uint64_t> seen_gen_; // stamp: uid already emitted into order_ this pass
  std::vector<std::uint32_t> order_;    // uid iteration order (dedup)
  std::uint64_t generation_{0};
};

[[nodiscard]] Status validate_checkpoint(const BacktestCheckpoint &checkpoint,
                                         std::size_t continuation_ref_count) {
  if (checkpoint.base_ts_ns <= 0) {
    return Err(ErrorCode::InvalidArgument,
               "run_backtest_incremental: checkpoint base_ts_ns must be positive");
  }
  if (checkpoint.next_lot_id == 0u) {
    return Err(ErrorCode::InvalidArgument,
               "run_backtest_incremental: checkpoint next_lot_id must be positive");
  }
  if (!std::isfinite(checkpoint.cash) || !std::isfinite(checkpoint.nav) ||
      !std::isfinite(checkpoint.cumulative_noncash_financing)) {
    return Err(ErrorCode::InvalidArgument,
               "run_backtest_incremental: checkpoint accounting state must be finite");
  }
  if (continuation_ref_count > 0u &&
      checkpoint.completed_step_index >
          std::numeric_limits<std::size_t>::max() - (continuation_ref_count - 1u)) {
    return Err(ErrorCode::InvalidArgument,
               "run_backtest_incremental: checkpoint global step index would overflow");
  }

  ATX_TRY_VOID(validate_lot_economics(checkpoint.portfolio.lots, "checkpoint portfolio",
                                      checkpoint.base_ts_ns));
  std::unordered_set<std::uint64_t> lot_ids;
  lot_ids.reserve(checkpoint.portfolio.lots.size());
  for (const Lot &lot : checkpoint.portfolio.lots) {
    if (lot.id == 0u || lot.id >= checkpoint.next_lot_id || lot.contract.uid == 0u) {
      return Err(ErrorCode::InvalidArgument,
                 "run_backtest_incremental: checkpoint lot id=" + std::to_string(lot.id) +
                     " has an invalid identity");
    }
    if (!lot_ids.insert(lot.id).second) {
      return Err(ErrorCode::InvalidArgument,
                 "run_backtest_incremental: duplicate checkpoint lot id=" + std::to_string(lot.id));
    }
  }

  std::unordered_set<std::uint32_t> share_uids;
  share_uids.reserve(checkpoint.hedge_shares.size());
  for (const HedgeSharePosition &position : checkpoint.hedge_shares) {
    if (position.uid == 0u || !std::isfinite(position.shares)) {
      return Err(ErrorCode::InvalidArgument,
                 "run_backtest_incremental: checkpoint hedge shares require a real uid and "
                 "finite quantity");
    }
    if (!share_uids.insert(position.uid).second) {
      return Err(ErrorCode::InvalidArgument,
                 "run_backtest_incremental: duplicate checkpoint hedge uid=" +
                     std::to_string(position.uid));
    }
  }

  ATX_TRY_VOID(validate_swap_lot_economics(checkpoint.portfolio.swap_lots, "checkpoint swap book",
                                           checkpoint.base_ts_ns));
  std::unordered_set<std::uint64_t> swap_ids;
  swap_ids.reserve(checkpoint.portfolio.swap_lots.size());
  for (const SwapLot &lot : checkpoint.portfolio.swap_lots) {
    if (lot.id == 0u || lot.id >= checkpoint.next_lot_id || lot_ids.count(lot.id) != 0u) {
      return Err(ErrorCode::InvalidArgument,
                 "run_backtest_incremental: checkpoint swap lot id=" + std::to_string(lot.id) +
                     " has an invalid identity");
    }
    if (!swap_ids.insert(lot.id).second) {
      return Err(ErrorCode::InvalidArgument,
                 "run_backtest_incremental: duplicate checkpoint swap lot id=" +
                     std::to_string(lot.id));
    }
  }
  // Accrual state is only meaningful against a live lot: an orphaned entry would
  // silently resurrect a settled contract's fixing series onto a reused id.
  std::unordered_set<std::uint64_t> accrual_ids;
  accrual_ids.reserve(checkpoint.swap_accruals.size());
  for (const SwapAccrual &accrual : checkpoint.swap_accruals) {
    if (swap_ids.count(accrual.lot_id) == 0u || !accrual_ids.insert(accrual.lot_id).second) {
      return Err(ErrorCode::InvalidArgument,
                 "run_backtest_incremental: checkpoint swap accrual lot_id=" +
                     std::to_string(accrual.lot_id) + " is orphaned or duplicated");
    }
    if (accrual.rv.n_obs_done > accrual.rv.n_obs_total || !(accrual.rv.annualization > 0.0) ||
        !std::isfinite(accrual.rv.sum_sq_log_returns_done) ||
        !std::isfinite(accrual.rv.rv_done_dec) || !std::isfinite(accrual.prev_spot) ||
        !std::isfinite(accrual.prev_pv) || (accrual.have_prev && !(accrual.prev_spot > 0.0))) {
      return Err(ErrorCode::InvalidArgument,
                 "run_backtest_incremental: checkpoint swap accrual lot_id=" +
                     std::to_string(accrual.lot_id) + " has invalid state");
    }
  }
  return Ok();
}

// The swap lane's half of the transition check. Same three rules as the option
// lane — surviving lots bit-identical, new ids inside the step's watermark
// window, no duplicate ids — plus a cross-lane id check against the (already
// rebuilt) option index, because both lanes draw from ONE `next_lot_id`.
//
// Linear scans rather than a sorted index: a swap book is tens of lots next to
// the option book's thousands, and every loop here is bounded by `after.size()`.
[[nodiscard]] Status validate_swap_transition(std::span<const SwapLot> before,
                                              std::span<const SwapLot> after,
                                              std::uint64_t next_id_before,
                                              std::uint64_t next_id_after, std::int64_t base_ts,
                                              std::span<const Lot> after_option_lots,
                                              const ReusableLotIdIndex &after_option_index) {
  ATX_TRY_VOID(validate_swap_lot_economics(after, "post-strategy swap book", base_ts));
  // NO EARLY CLOSE IN v1. Settled lots left the book in the swap pass, BEFORE
  // `before_swaps` was captured, so any lot missing from `after` was erased by
  // the strategy. Letting that through fabricates NAV: the lot's accumulated
  // `prev_pv` stays in the running total with nothing to offset it, its accrual
  // is orphaned (which then makes the checkpoint unresumable), and `swap_pv`
  // drops by a mark that never settled — silent drift the reconciliation would
  // only surface as an unexplained level shift.
  for (const SwapLot &prior : before) {
    bool survives = false;
    for (const SwapLot &lot : after) {
      if (lot.id == prior.id) {
        survives = true;
        break;
      }
    }
    if (!survives) {
      return Err(ErrorCode::InvalidArgument,
                 "run_backtest: swap lot id=" + std::to_string(prior.id) +
                     " erased by strategy; early close unsupported in v1 (swaps are held to "
                     "expiry)");
    }
  }
  for (std::size_t i = 0; i < after.size(); ++i) {
    const SwapLot &lot = after[i];
    for (std::size_t j = 0; j < i; ++j) {
      if (after[j].id == lot.id) {
        return Err(ErrorCode::InvalidArgument,
                   "run_backtest: duplicate swap lot id=" + std::to_string(lot.id));
      }
    }
    if (after_option_index.find(after_option_lots, lot.id).has_value()) {
      return Err(ErrorCode::InvalidArgument, "run_backtest: swap lot id=" + std::to_string(lot.id) +
                                                 " collides with an option lot");
    }
    const SwapLot *prior = nullptr;
    for (const SwapLot &candidate : before) {
      if (candidate.id == lot.id) {
        prior = &candidate;
        break;
      }
    }
    if (prior != nullptr) {
      if (!same_swap_lot_identity(*prior, lot)) {
        return Err(ErrorCode::InvalidArgument,
                   "run_backtest: mutated surviving swap lot id=" + std::to_string(lot.id));
      }
      continue;
    }
    if (lot.id < next_id_before || lot.id >= next_id_after) {
      return Err(ErrorCode::InvalidArgument,
                 "run_backtest: reused or non-monotonic swap lot id=" + std::to_string(lot.id));
    }
  }
  return Ok();
}

[[nodiscard]] Status validate_strategy_transition(
    std::span<const Lot> before, std::span<const Lot> after, std::span<const SwapLot> before_swaps,
    std::span<const SwapLot> after_swaps, std::uint64_t next_id_before, std::uint64_t next_id_after,
    std::int64_t base_ts, ReusableLotIdIndex &before_index, ReusableLotIdIndex &after_index) {
  ATX_TRY_VOID(validate_lot_economics(after, "post-strategy book", base_ts));
  ATX_TRY_VOID(before_index.rebuild(before, "pre-strategy book"));
  ATX_TRY_VOID(after_index.rebuild(after, "post-strategy book"));
  if (next_id_after < next_id_before) {
    return Err(ErrorCode::InvalidArgument, "run_backtest: strategy made next_lot_id non-monotonic");
  }
  for (const Lot &lot : after) {
    const std::optional<std::size_t> prior = before_index.find(before, lot.id);
    if (prior.has_value()) {
      if (!same_lot_identity(before[*prior], lot)) {
        return Err(ErrorCode::InvalidArgument,
                   "run_backtest: mutated surviving lot id=" + std::to_string(lot.id));
      }
      continue;
    }
    if (lot.id < next_id_before || lot.id >= next_id_after) {
      return Err(ErrorCode::InvalidArgument,
                 "run_backtest: reused or non-monotonic lot id=" + std::to_string(lot.id));
    }
  }
  return validate_swap_transition(before_swaps, after_swaps, next_id_before, next_id_after, base_ts,
                                  after, after_index);
}

// ── Swap lane: accrual + mark + settlement ─────────────────────────────────

// Task F-8 fix round 2: the ONE roster. `swap_explain_columns()`'s order is the
// order of these indices and of `BacktestResult`'s declarations, and the
// `static_assert` below is what keeps the three from drifting apart in COUNT.
// Order itself is carried by the table literal sitting directly beside the enum:
// a reorder compiles, so the mitigation is adjacency, not the assert.
enum SwapExplainIx : std::size_t {
  kExCarry = 0,
  kExRealized,
  kExVolLevel,
  kExSkew,
  kExConvexity,
  kExDiscount,
  kExResidual,
  kExUnattributed,
  kSwapExplainCount,
};

constexpr BacktestExplainColumn kSwapExplainColumns[] = {
    {"swap_explain_carry", &BacktestResult::swap_explain_carry},
    {"swap_explain_realized", &BacktestResult::swap_explain_realized},
    {"swap_explain_vol_level", &BacktestResult::swap_explain_vol_level},
    {"swap_explain_skew", &BacktestResult::swap_explain_skew},
    {"swap_explain_convexity", &BacktestResult::swap_explain_convexity},
    {"swap_explain_discount", &BacktestResult::swap_explain_discount},
    {"swap_explain_residual", &BacktestResult::swap_explain_residual},
    {"swap_explain_unattributed", &BacktestResult::swap_explain_unattributed},
};
static_assert(std::size(kSwapExplainColumns) == kSwapExplainCount,
              "the explain roster and its index enum must name the same columns");

// One step's swap-lane economics, in QTY-SCALED position dollars.
struct SwapStepResult {
  double swap_pnl{0.0};        // Σ (mark - prev mark) over live lots + settlements
  double swap_pv{0.0};         // Σ live marks AFTER the pass (a state quantity)
  double settlement_cash{0.0}; // Σ settled payoffs, to be added to the ledger
  // Task F-8: the explain, summed over the lane in fixed lot order. All FLOWS,
  // like `swap_pnl` and unlike `swap_pv` -- see BacktestResult::swap_explain_*
  // for why treating any one of them as state breaks the identity under
  // `record_every_n > 1`. All exactly 0.0 when `RunConfig::swap_pnl_explain` is
  // off, at which point nothing here is ever written.
  //
  // An ARRAY indexed by `SwapExplainIx`, not eight named doubles, so `push_row`
  // can walk it beside `swap_explain_columns()` without either a ninth
  // hand-written list or pointer arithmetic across distinct members (which the
  // named-field version would have needed, and which is not something this
  // codebase gets to do).
  std::array<double, kSwapExplainCount> ex{};
};

// Task F-8 fix round 2: the explain carries NO per-lot state between steps.
//
// Round 1 held one (`SwapExplainPrior`), which is what put `theta_zero_fixing`
// and the `dt` multiplying it one step apart (C-1) and what left a per-lot entry
// behind for every lot that left the book by a path other than settlement.
// Everything the explain needs about the start of a step now comes from `base`
// — the snapshot the step is measured from, which the engine already holds —
// plus the accrual state captured before this step's fixing lands. See
// `accumulate_swap_explain`.

// The accrual for `lot`, seeded from the lot's contract terms on first sight.
// First-sight order is `swap_lots` order, which the engine never reorders.
[[nodiscard]] SwapAccrual &accrual_for(std::vector<SwapAccrual> &accruals, const SwapLot &lot) {
  for (SwapAccrual &acc : accruals) {
    if (acc.lot_id == lot.id) {
      return acc;
    }
  }
  SwapAccrual fresh;
  fresh.lot_id = lot.id;
  fresh.rv.annualization = lot.annualization;
  fresh.rv.n_obs_total = lot.n_obs_total;
  accruals.push_back(fresh);
  return accruals.back();
}

// Take one dated fixing into `acc`. Ordering is validated FIRST and
// unconditionally (a replayed or backdated snapshot mutates nothing); a fully
// observed contract stops accruing AND stops advancing `prev_*`. See SwapAccrual
// in backtest.hpp for why this transcribes RealizedTracker rather than using it.
[[nodiscard]] Status observe_swap_fixing(SwapAccrual &acc, std::int64_t ts_ns, double spot) {
  if (acc.have_prev && ts_ns <= acc.prev_ts_ns) {
    return Err(ErrorCode::AlreadyExists,
               "run_backtest: duplicate/backdated swap fixing for lot id=" +
                   std::to_string(acc.lot_id) + " at ts=" + std::to_string(ts_ns));
  }
  if (!(spot > 0.0)) {
    return Err(ErrorCode::InvalidArgument,
               "run_backtest: swap fixing spot must be > 0 for lot id=" +
                   std::to_string(acc.lot_id));
  }
  if (!acc.have_prev) {
    acc.prev_spot = spot;
    acc.prev_ts_ns = ts_ns;
    acc.have_prev = true;
    return Ok();
  }
  if (acc.rv.n_obs_done >= acc.rv.n_obs_total) {
    return Ok(); // fully observed: the contract's fixing series is closed
  }
  const double r = std::log(spot / acc.prev_spot);
  acc.rv.sum_sq_log_returns_done += r * r;
  acc.rv.n_obs_done += 1u;
  acc.prev_spot = spot;
  acc.prev_ts_ns = ts_ns;
  acc.rv.rv_done_dec = acc.rv.annualization * acc.rv.sum_sq_log_returns_done /
                       static_cast<double>(acc.rv.n_obs_done);
  return Ok();
}

// The settled terminal rate: annualized realized VARIANCE for the var kinds, its
// square root for the vol kinds, each capped where the product caps. Read purely
// off the accrual — settlement needs no surface.
//
// PRECONDITION, and the reason it is asserted rather than left to a comment
// (Task F-5 fix round 1). The caller multiplies this by `qty * notional` and
// subtracts `strike_dec` — a payoff LINEAR in the returned rate. Every kind
// `engine_supports_swap_kind` admits has that shape; F-5's option kinds do not
// (their payoff is kinked), which is why that list refuses them. The assert
// re-states the boundary contract at the point that DEPENDS on it, so a path
// that reaches settlement without passing `validate_swap_lot_economics` fails
// loudly in debug instead of paying a swap's linear P&L on an option.
//
// It deliberately CALLS the admission list rather than re-listing the linear
// kinds: a second copy of that membership is the exact hazard Task F-5's own
// unification commit existed to remove. What it therefore cannot catch is a
// future author WIDENING `engine_supports_swap_kind` without teaching this
// function the new payoff shape — that coupling is not expressible as a check,
// and is carried by the cross-reference on that function's own arms.
[[nodiscard]] double swap_terminal_value(const SwapLot &lot, const SwapAccrual &acc) noexcept {
  assert(engine_supports_swap_kind(lot.kind) &&
         "swap_terminal_value settles a payoff LINEAR in the terminal rate; see "
         "engine_supports_swap_kind (backtest.hpp) for which kinds have that shape");
  const double terminal =
      deriv_kind_settles_in_vol(lot.kind) ? std::sqrt(acc.rv.rv_done_dec) : acc.rv.rv_done_dec;
  return deriv_kind_is_capped(lot.kind) ? std::min(terminal, lot.cap_dec) : terminal;
}

// Fix, mark and settle every swap lot against the newly loaded `shifted`
// snapshot, in book order. Settled lots (and their accruals) are erased.
//
// Per lot, in order: resolve the surface (absent + UNEXPIRED => fail closed,
// same policy as an unpriced hedge share); take today's fixing when a surface
// is there; then EITHER settle (expiry observed exactly — an expiring lot
// settles, it does not mark) OR mark through `deriv_price` at the residual
// tenor. Crossing an expiry without an exact observation is NotFound, exactly as
// it is for an option lot.
// Task F-8: one live lot's contribution to the explain. Returns false when the
// lot cannot be attributed at all, in which case the caller books the whole move
// to the residual and counts it.
//
// ── THE SENSITIVITIES ARE RESOLVED AGAINST THE STEP'S OWN BASE DATE ─────────
//
// Fix round 2 / C-1. This used to carry the previous step's greeks forward in an
// engine-local `SwapExplainPrior`, which put `theta_zero_fixing` and the `dt` it
// is multiplied by ONE STEP APART. That greek is a rate obtained by rolling
// `bumps.time_years` while injecting exactly ONE fixing
// (`derivatives.cpp`'s `inject_carry_fixing`: `n_obs_done + 1u`, independent of
// the roll length), so `theta_zero_fixing * h` is the deterministic move over
// `h` INCLUDING that one fixing. Multiplying a greek bumped over `h_prev` by
// `dt_this` scales the fixing leg by `dt_this / h_prev` — on a real calendar a
// Friday-to-Monday step follows a one-day step and over-states carry 3x, and the
// next step under-states it 3x. The residual absorbed all of it, so the identity
// closed and NAV never moved: it read exactly like a thing that passed.
//
// Resolving the start-of-step state against `base` — the snapshot the step is
// measured FROM, which the engine already holds — removes the coupling instead
// of correcting for it. `bumps.time_years` is now this step's own length AND the
// interval the result is multiplied by, so both legs of carry (the one fixing
// and the calendar roll) are priced over exactly the interval that elapsed.
//
// It also deletes the carried state entirely: no prior to keep in step, nothing
// to erase when a lot leaves the book by an unexpected path, and a resumed run
// can attribute its FIRST step, which the carried-prior version could not.
[[nodiscard]] bool accumulate_swap_explain(SwapStepResult &out, const SurfaceRef &base_surface,
                                           const SurfaceRef &surface, const SwapLot &lot,
                                           const DerivContract &base_contract,
                                           const DerivContract &contract,
                                           const DerivConfig &deriv_cfg,
                                           std::optional<double> base_wing_band, double pv_scaled,
                                           double prev_pv, double dt_years, double realized_var) {
  if (base_surface == nullptr || !(dt_years > 0.0) || !std::isfinite(dt_years)) {
    return false;
  }
  DerivGreekBumps bumps{};
  bumps.smile_greeks = true;    // the two smile components are the point of this
  bumps.time_years = dt_years;  // see the C-1 note above: this is load-bearing

  const auto g = detail::deriv_greeks_on_ref(base_surface, base_contract, deriv_cfg, bumps,
                                             base_wing_band);
  const auto base_smile = detail::sample_smile_on_ref(base_surface, base_contract.maturity_t);
  const auto smile = detail::sample_smile_on_ref(surface, contract.maturity_t);
  if (!g.has_value() || !base_smile.has_value() || !smile.has_value()) {
    return false;
  }

  DerivPnlInputs in{};
  in.greeks = *g;
  in.dt_years = dt_years;
  in.realized_var_dec = realized_var;
  // I-1. The weight must land in the same PV units as the two marks, which are
  // DISCOUNTED (`deriv_price` returns `df * notional * (...)`). Passing 1.0 here
  // over-stated `realized` by `1/df ~ 1 + rT` and dumped the difference in the
  // residual -- invisible, because the closed-form oracle in deriv_pnl_test.cpp
  // is a zero-rate world where df is exactly 1. The fixing lands on the TO date,
  // so it is discounted at that date's own zero to that date's own tenor, which
  // is precisely what `sample_smile_on_ref` already returned.
  //
  // qty-scaled for the same reason `pv_scaled` and `prev_pv` are.
  const double df_to = std::exp(-smile->zero_rate * contract.maturity_t);
  in.fixing_weight = lot.qty * var_swap_fixing_weight(contract, df_to);
  in.from.pv = prev_pv;
  in.from.sigma_atm = base_smile->sigma_atm;
  in.from.skew_slope = base_smile->skew_slope;
  in.from.smile_curvature = base_smile->smile_curvature;
  in.from.zero_rate = base_smile->zero_rate;
  in.to.pv = pv_scaled;
  in.to.sigma_atm = smile->sigma_atm;
  in.to.skew_slope = smile->skew_slope;
  in.to.smile_curvature = smile->smile_curvature;
  in.to.zero_rate = smile->zero_rate;

  const auto e = deriv_pnl_explain(in);
  if (!e.has_value()) {
    return false;
  }
  // A component the pricer could not compute makes the whole lot unattributed
  // rather than partially attributed: booking five of six terms and calling the
  // sixth a residual would report a clean-looking explain for a day nobody
  // measured.
  if (e->flags != DerivPnlFlags::None || !std::isfinite(e->residual)) {
    return false;
  }
  out.ex[kExCarry] += e->carry;
  out.ex[kExRealized] += e->realized;
  out.ex[kExVolLevel] += e->vol_level;
  out.ex[kExSkew] += e->skew;
  out.ex[kExConvexity] += e->convexity;
  out.ex[kExDiscount] += e->discount;
  out.ex[kExResidual] += e->residual;
  return true;
}

[[nodiscard]] Result<SwapStepResult> step_swap_lots(const MarketSnapshot &base,
                                                    const MarketSnapshot &shifted,
                                                    PortfolioState &book,
                                                    std::vector<SwapAccrual> &accruals,
                                                    const DerivConfig &deriv_cfg,
                                                    bool want_explain) {
  SwapStepResult out;
  if (book.swap_lots.empty()) {
    return Ok(out); // zero-swap books never price, allocate, or touch NAV
  }
  const std::int64_t ts = shifted.ts_ns();
  for (std::size_t i = 0; i < book.swap_lots.size();) {
    const SwapLot &lot = book.swap_lots[i];
    const bool expiring = lot.expiry_ts_ns <= ts;
    if (expiring && lot.expiry_ts_ns != ts) {
      return Err(ErrorCode::NotFound, "run_backtest: swap lot id=" + std::to_string(lot.id) +
                                          " expired without an exact observation (expiry=" +
                                          std::to_string(lot.expiry_ts_ns) +
                                          ", snapshot=" + std::to_string(ts) + ")");
    }
    // FAIL CLOSED ON BOTH SIDES OF EXPIRY. A held lot needs the surface to mark;
    // a SETTLING lot needs it just as much, because the expiry close IS that
    // contract's final fixing — settling without it would divide a short Sigma r^2
    // by a denominator one observation light and call the answer terminal. The
    // option lane refuses a settling lot with no surface for the same reason
    // ("no surface for settling lot", compute_step); this mirrors it.
    const SurfaceRef surface = shifted.find(lot.uid);
    if (surface == nullptr) {
      return Err(ErrorCode::NotFound,
                 std::string("run_backtest: no surface for ") + (expiring ? "settling" : "held") +
                     " swap lot id=" + std::to_string(lot.id) + " uid=" + std::to_string(lot.uid));
    }
    SwapAccrual &acc = accrual_for(accruals, lot);
    // Task F-8: the explain differences the interval the mark move came from, so
    // it needs the accrual's state BEFORE this step's fixing lands.
    // `observe_swap_fixing` advances `prev_ts_ns`/`prev_spot`/`n_obs_done` in
    // place, so reading them afterwards would report a zero-length step and a
    // fixing that had already been folded in.
    const bool had_prev_fixing = acc.have_prev;
    const std::int64_t prev_ts = acc.prev_ts_ns;
    const std::uint32_t prev_n_done = acc.rv.n_obs_done;
    const double prev_spot = acc.prev_spot;
    const RealizedVarianceSpec prev_rv = acc.rv;
    ATX_TRY_VOID(observe_swap_fixing(acc, ts, surface->pricing().S));
    if (expiring) {
      // `rv_done_dec` is the Sigma r^2 / n_done estimator, so a SHORT fixing
      // series still settles on the returns actually observed — but a series
      // with NO returns at all is not an estimate, it is a fabricated 0.0 that
      // would pay away the whole strike. A lot needs N+1 steps to accrue N
      // returns (the first step it is seen seeds the series); refuse the
      // degenerate case loudly instead of settling it.
      if (acc.rv.n_obs_done == 0u) {
        return Err(ErrorCode::NotFound,
                   "run_backtest: swap lot id=" + std::to_string(lot.id) +
                       " reached expiry with no observed return (a lot needs one step to seed "
                       "its fixing series and one more per accrued return)");
      }
      const double payoff =
          lot.qty * lot.notional * (swap_terminal_value(lot, acc) - lot.strike_dec);
      out.settlement_cash += payoff;
      out.swap_pnl += payoff - acc.prev_pv;
      if (want_explain) {
        // A SETTLEMENT IS NOT A MARKET MOVE. The lot stops existing and pays its
        // terminal value; there is no surface state to difference and no
        // sensitivity that describes it. It goes to the residual whole -- which
        // keeps the identity exact -- and is counted, so a big residual on an
        // expiry date reads as "a lot settled" rather than as a modelling gap.
        out.ex[kExResidual] += payoff - acc.prev_pv;
        out.ex[kExUnattributed] += 1.0;
      }
      std::erase_if(accruals, [id = lot.id](const SwapAccrual &a) { return a.lot_id == id; });
      book.swap_lots.erase(book.swap_lots.begin() + static_cast<std::ptrdiff_t>(i));
      continue; // `i` now indexes the next lot
    }
    DerivContract contract;
    contract.kind = lot.kind;
    contract.maturity_t = residual_T(lot.expiry_ts_ns, ts);
    contract.strike_dec = lot.strike_dec;
    contract.cap_dec = lot.cap_dec;
    contract.notional = lot.notional;
    contract.rv_spec = acc.rv;
    // FIT-C7 / Task C-6: trust this lot's surface's OWN certified wing band
    // (from the SAME snapshot's same-blob provenance), not the mode-blind
    // default -- a Latency-fit surface certifies only +-0.35, and marking it
    // through the wider +-0.5 default read uncertified extrapolation into
    // swap_pnl.
    const std::optional<double> wing_band = certified_wing_band_for(shifted, lot.uid);
    ATX_TRY(const DerivQuote quote,
            detail::deriv_price_on_ref(surface, contract, deriv_cfg, wing_band));
    const double pv_scaled = lot.qty * quote.pv;
    out.swap_pnl += pv_scaled - acc.prev_pv;
    out.swap_pv += pv_scaled;
    if (want_explain) {
      // Attribute only a step in which a fixing actually LANDED. `carry` is
      // `theta_zero_fixing * dt`, which prices one more fixing arriving at a
      // zero return -- so on a step where the series was already closed, or the
      // lot was only being seeded, that term describes an event that did not
      // happen. Those steps are counted as unattributed rather than explained
      // with a term that does not apply.
      const bool fixing_landed = had_prev_fixing && acc.rv.n_obs_done > prev_n_done;
      const double dt_years =
          fixing_landed ? static_cast<double>(ts - prev_ts) / kNsPerYear : 0.0;
      // The annualized rate of THIS step's fixing, in `inject_carry_fixing`'s
      // own units (`rv_done_dec = annualization * sum r^2 / n_done`, so one
      // fixing's rate is `annualization * r^2`).
      double realized = kQuietNaN;
      if (fixing_landed && prev_spot > 0.0) {
        const double r = std::log(surface->pricing().S / prev_spot);
        realized = acc.rv.annualization * r * r;
      }
      // The contract as it stood at the START of the step: a longer tenor and
      // the accrual BEFORE this step's fixing was folded in. Reconstructed from
      // the pre-step state captured above rather than carried, so it cannot
      // drift out of step with the greeks resolved against it.
      DerivContract base_contract = contract;
      base_contract.maturity_t = residual_T(lot.expiry_ts_ns, prev_ts);
      base_contract.rv_spec = prev_rv;
      const SurfaceRef base_surface = fixing_landed ? base.find(lot.uid) : SurfaceRef{};
      const std::optional<double> base_wing_band =
          (base_surface != nullptr) ? certified_wing_band_for(base, lot.uid) : std::nullopt;
      const bool attributed =
          fixing_landed && accumulate_swap_explain(out, base_surface, surface, lot, base_contract,
                                                   contract, deriv_cfg, base_wing_band, pv_scaled,
                                                   acc.prev_pv, dt_years, realized);
      if (!attributed) {
        // Nothing was measured, so the whole move IS the residual and the
        // identity still closes exactly. Counted so a reader can tell this from
        // a day that genuinely resisted explanation.
        out.ex[kExResidual] += pv_scaled - acc.prev_pv;
        out.ex[kExUnattributed] += 1.0;
      }
    }
    acc.prev_pv = pv_scaled;
    ++i;
  }
  return Ok(out);
}

// Book greeks + the count of positions the pricer could not value on THIS
// snapshot's date. `total`'s `gross_*` sum only the Ok lanes; `n_unpriced`
// = n_pos - PriceTotals::n_ok is the number EXCLUDED from that sum (surface
// absent, degenerate contract, or numeric failure). `first_unpriced_uid` names
// the first such position (input order) for the Error-policy diagnostic; 0 when
// none. This is a single-date snapshot count — distinct from a STEP's
// completeness (which needs base AND shifted); see BacktestResult.
struct BookGreeks {
  PriceTotals total{};
  std::uint32_t n_unpriced{0};
  std::uint32_t first_unpriced_uid{0};
};

[[nodiscard]] BookGreeks summarize_price_frame(const PriceFrame &frame) {
  BookGreeks result;
  result.total = frame.total;
  const std::size_t n_pos = frame.size();
  const std::size_t n_ok = frame.total.n_ok;
  result.n_unpriced = static_cast<std::uint32_t>((n_pos >= n_ok) ? (n_pos - n_ok) : std::size_t{0});
  if (result.n_unpriced > 0) {
    for (std::size_t i = 0; i < frame.size(); ++i) {
      if (frame.status[i] != PriceStatus::Ok) {
        result.first_unpriced_uid = frame.uid[i];
        break;
      }
    }
  }
  return result;
}

struct ReusablePriceFrame {
  PriceFrame frame;

  void resize(std::size_t n) {
    frame.id.resize(n);
    frame.uid.resize(n);
    frame.pv.resize(n);
    frame.price.resize(n);
    frame.iv.resize(n);
    frame.delta.resize(n);
    frame.gamma.resize(n);
    frame.vega.resize(n);
    frame.theta.resize(n);
    frame.rho.resize(n);
    frame.vanna.resize(n);
    frame.volga.resize(n);
    frame.charm.resize(n);
    frame.status.resize(n);
  }

  [[nodiscard]] PriceFrameView view() noexcept {
    return PriceFrameView{frame.id,    frame.uid,   frame.pv,    frame.price,  frame.iv,
                          frame.delta, frame.gamma, frame.vega,  frame.theta,  frame.rho,
                          frame.vanna, frame.volga, frame.charm, frame.status, &frame.total};
  }

  // Marks-only sizing (B2 settlement): marks columns to n, greek columns emptied so
  // a PriceFieldMask::Marks price_into stays off the greek path (the seam leaves the
  // eight greek spans EMPTY under the Marks mask). clear() keeps capacity, so this is
  // allocation-free once warm.
  void resize_marks(std::size_t n) {
    frame.id.resize(n);
    frame.uid.resize(n);
    frame.pv.resize(n);
    frame.price.resize(n);
    frame.iv.resize(n);
    frame.status.resize(n);
    frame.delta.clear();
    frame.gamma.clear();
    frame.vega.clear();
    frame.theta.clear();
    frame.rho.clear();
    frame.vanna.clear();
    frame.volga.clear();
    frame.charm.clear();
  }

  [[nodiscard]] PriceFrameView marks_view() noexcept {
    return PriceFrameView{frame.id, frame.uid, frame.pv, frame.price,  frame.iv,
                          {},       {},        {},       {},           {},
                          {},       {},        {},       frame.status, &frame.total};
  }
};

// Price the current lots against `snap` at its residual T and report how many
// could not be valued. An empty book yields zero totals and n_unpriced 0 (an
// empty portfolio prices to an empty frame).
[[nodiscard]] Result<BookGreeks> book_greeks(const MarketSnapshot &snap,
                                             const std::vector<Lot> &lots, const PriceOptions &opts,
                                             RetainedBookPricer &retained,
                                             StepMarkMemo *mark_memo = nullptr) {
  ATX_VOL_PROFILE_SCOPE(BookGreeks);
  ATX_TRY(PortfolioPricer * pricer, retained.prepare(lots, snap.ts_ns()));
  PortfolioWorkspace &workspace = retained.workspace();
  auto totals = pricer->price_totals(snap.set(), PriceFieldMask::FullGreeks, workspace, opts);
  if (!totals) {
    return Err(totals.error());
  }
  // L2: publish this pass's per-unique base marks into the mark memo so the NEXT
  // step's settlement (same base date) reads an expiring lot's base mark instead of
  // re-solving it. Reads the retained bundle (no extra solve). Bit-identical: the
  // marks are the same andersen_lake values a Marks-mask solve would produce.
  if (mark_memo != nullptr) {
    mark_memo->populate_from(*pricer, workspace, snap);
  }
  BookGreeks result;
  result.total = *totals;
  const std::size_t n_positions = pricer->portfolio().n_positions();
  result.n_unpriced = static_cast<std::uint32_t>(
      n_positions >= totals->n_ok ? n_positions - totals->n_ok : std::size_t{0});
  if (result.n_unpriced > 0) {
    // The common all-Ok path stays totals-only. Materialize a diagnostic frame
    // only when the caller needs the first failing uid for an error message.
    auto frame = pricer->price(snap.set(), opts);
    if (!frame) {
      return Err(frame.error());
    }
    for (std::size_t i = 0; i < frame->size(); ++i) {
      if (frame->status[i] != PriceStatus::Ok) {
        result.first_unpriced_uid = frame->uid[i];
        break;
      }
    }
  }
  return Ok(result);
}

// One priced step base -> shifted over `lots`: partition lots with an exact
// expiry-time observation (settled at intrinsic: qty*mult*(intrinsic(S_expiry) -
// base_mark)) from survivors, then Taylor PnL-explain the survivors. A step that
// skips across expiry fails closed because the shifted spot is not an expiry
// settlement observation. Shared by both backtest overloads.
struct StepPnl {
  PnlTotals totals{};
  double settlement{0.0};
  // Alive (non-expiring) positions the pricer could not value this step (surface
  // absent, rolled past expiry, or numeric failure) — i.e. alive.size() - n_ok.
  // Their PnL is excluded from `totals`. `first_unpriced_uid` names the first such
  // position (input order) for the Error-policy diagnostic; 0 when none.
  std::uint32_t n_unpriced{0};
  std::uint32_t first_unpriced_uid{0};
  // ExcludeAndReport only: settlement events this step could not price at their
  // exact expiry observation — a lot newly deferred, a deferred lot still waiting,
  // or a deferred lot finally settling. Reported alongside `n_unpriced` in the
  // row's exclusion tally (the F1(b) hedge-share precedent); never non-zero under
  // `Error`, which still fails closed at the first absent settlement board.
  std::uint32_t n_deferred{0};
  // Intrinsic proceeds of the deferred lots that settled THIS step, for the cash
  // ledger. The fixed-book overload has no cash ledger and ignores it.
  double deferred_settle_cash{0.0};
};

// WS-F F1 tolerance extension (SP100 task 2). One lot whose EXACT expiry step had
// no shifted-side board under `UnpricedLotPolicy::ExcludeAndReport`: there is no
// observed spot to settle it against, and dropping it would delete a real
// position, so the lot leaves `book.lots` and waits here for the first later step
// whose board is back.
//
// `base_mark` freezes the deferral step's base mark — the exact mark the
// non-deferred settlement would have explained against. When the deferral step's
// BASE board was absent too there is no such mark (`base_mark_known == false`) and
// the eventual settlement contributes NOTHING to the P&L explain: that step's move
// is the excluded, never-recovered P&L this policy already documents. The cash
// ledger still books the intrinsic either way.
struct DeferredSettlement {
  Lot lot{};
  double base_mark{0.0};
  bool base_mark_known{false};
};

// The deferral book, held ACROSS steps by each run loop. Entries are kept sorted
// by lot id, so every traversal — and therefore the settlement accumulation order
// and the reported counts — depends only on lot identity, never on insertion
// order or on any unordered-container layout. Empty and never consulted under
// `UnpricedLotPolicy::Error` (the run loops pass a null pointer there, which keeps
// the strict path's control flow bit-identical to the pre-change engine).
class DeferredSettlementBook {
public:
  [[nodiscard]] bool empty() const noexcept { return entries_.empty(); }
  [[nodiscard]] std::size_t size() const noexcept { return entries_.size(); }
  [[nodiscard]] std::span<const DeferredSettlement> entries() const noexcept {
    return {entries_.data(), entries_.size()};
  }

  void insert(const Lot &lot, double base_mark, bool base_mark_known) {
    const auto at = std::lower_bound(entries_.begin(), entries_.end(), lot.id,
                                     [](const DeferredSettlement &d, std::uint64_t id) {
                                       return d.lot.id < id;
                                     });
    entries_.insert(at, DeferredSettlement{lot, base_mark, base_mark_known});
  }

  // Scratch for the survivors of one resolution pass; `commit` swaps it in, which
  // preserves the id ordering because the pass walks `entries_` in that order.
  [[nodiscard]] std::vector<DeferredSettlement> &retain_scratch() noexcept { return scratch_; }
  void commit() noexcept { entries_.swap(scratch_); }

private:
  std::vector<DeferredSettlement> entries_;
  std::vector<DeferredSettlement> scratch_;
};

[[nodiscard]] Result<StepPnl>
compute_step(const MarketSnapshot &base, const MarketSnapshot &shifted,
             const std::vector<Lot> &lots, const PriceOptions &opts, RetainedBookPricer &retained,
             ReusableTargetMarkFrame *target_marks = nullptr, RetainedBookPricer *settle = nullptr,
             ReusablePriceFrame *settle_frame = nullptr, StepMarkMemo *mark_memo = nullptr,
             bool memo_enabled = false, DeferredSettlementBook *deferrals = nullptr) {
  ATX_VOL_PROFILE_SCOPE(StepPnl);
  std::vector<Lot> &alive = retained.reset_alive_scratch(lots.size());
  // B2: batch the expiry-settlement base marks (bottleneck #3). When a settlement
  // pricer is supplied, expiring lots are collected and their base marks come from
  // ONE PortfolioPricer Marks pass below, replacing the per-lot scalar fair_value.
  // The batched mark equals the scalar fair_value to the andersen_lake tolerance
  // (~1e-9 — economic parity per §3, checked by the ExpirySettlement gate); the
  // settlement is accumulated in the SAME lot order and the pricer reduction is
  // thread-invariant, so the result is deterministic and thread-count-invariant.
  // With no settle pricer (settle==nullptr) the exact per-lot scalar path is kept.
  const bool batch_settle = settle != nullptr && settle_frame != nullptr;
  std::vector<Lot> *expiring = batch_settle ? &settle->reset_alive_scratch(lots.size()) : nullptr;
  double settlement = 0.0;
  std::uint32_t n_deferred = 0;
  double deferred_settle_cash = 0.0;

  // Deferred settlements carried in from EARLIER steps, walked in LOT-ID order: a
  // lot settles at intrinsic against THIS step's shifted spot the first time its
  // board is back, and is otherwise counted and carried forward untouched. Runs
  // BEFORE the partition loop so a lot deferred on this very step is counted once,
  // here on the step it settles or waits — never twice on the step it deferred.
  // On a clean step the book is empty and nothing below changes, bit-for-bit.
  if (deferrals != nullptr && !deferrals->empty()) {
    std::vector<DeferredSettlement> &kept = deferrals->retain_scratch();
    kept.clear();
    for (const DeferredSettlement &pending : deferrals->entries()) {
      ++n_deferred;
      const SurfaceRef ss = shifted.find(pending.lot.contract.uid);
      if (ss == nullptr) {
        kept.push_back(pending); // still no board: stays open, counted, unchanged
        continue;
      }
      const double S = ss->pricing().S;
      const double K = pending.lot.contract.K;
      const double intrinsic =
          (pending.lot.contract.side == Side::Call) ? std::max(0.0, S - K) : std::max(0.0, K - S);
      const double weight = pending.lot.qty * pending.lot.multiplier;
      if (pending.base_mark_known) {
        settlement += weight * (intrinsic - pending.base_mark);
      }
      deferred_settle_cash += weight * intrinsic;
    }
    deferrals->commit();
  }

  for (const Lot &lot : lots) {
    if (lot.expiry_ts_ns <= shifted.ts_ns()) {
      if (lot.expiry_ts_ns != shifted.ts_ns()) {
        // UNCHANGED under BOTH policies: an expiry that passed between sessions was
        // never observed at all, so there is no settlement instant to defer TO. That
        // is a calendar bug (see `TenorSpec::snap_to_sessions`), not missing market
        // data, and it keeps failing closed.
        return Err(
            ErrorCode::NotFound,
            "run_backtest: no exact expiry observation for lot id=" + std::to_string(lot.id) +
                " (expiry_ts_ns=" + std::to_string(lot.expiry_ts_ns) +
                ", next_snapshot_ts_ns=" + std::to_string(shifted.ts_ns()) + ")");
      }
      const SurfaceRef bs = base.find(lot.contract.uid);
      const SurfaceRef ss = shifted.find(lot.contract.uid);
      if (bs == nullptr || ss == nullptr) {
        if (deferrals == nullptr) {
          return Err(ErrorCode::NotFound, "run_backtest: no surface for settling lot");
        }
        ++n_deferred;
        if (ss != nullptr) {
          // The settlement SPOT is observed; only the base mark this step would have
          // explained against is missing (the lot was already unpriced on the prior
          // step). Settle here with a ZERO explain contribution — that unrecovered
          // move is exactly what ExcludeAndReport documents — and fall out of the
          // partition, so the caller erases the lot as a normal expiry and its cash
          // ledger books the intrinsic off this same (present) board.
          continue;
        }
        // No settlement spot at all. Freeze the base mark when the base board is
        // there and carry the lot until some later step's board returns.
        double frozen_mark = 0.0;
        bool frozen_known = false;
        if (bs != nullptr) {
          const double T_base = residual_T(lot.expiry_ts_ns, base.ts_ns());
          const Result<double> mark =
              bs->fair_value(lot.contract.K, T_base, lot.contract.side, opts.query_execution);
          if (mark) {
            frozen_mark = *mark;
            frozen_known = true;
          }
          // A present board that fails to SOLVE is a numeric failure, not missing
          // market data, and the non-deferred settlement path returns that Err under
          // both policies. Here it is folded into "no mark" instead: a run that has
          // already opted into the lenient policy for this lot should not be killed
          // by a solve on a lot it can no longer value anyway. The two cases are
          // indistinguishable downstream — the count fires either way.
        }
        deferrals->insert(lot, frozen_mark, frozen_known);
        continue;
      }
      if (expiring != nullptr) {
        expiring->push_back(lot); // base mark supplied by the batched pass below
        continue;
      }
      const double T_base = residual_T(lot.expiry_ts_ns, base.ts_ns());
      auto mark = bs->fair_value(lot.contract.K, T_base, lot.contract.side, opts.query_execution);
      if (!mark) {
        return Err(mark.error());
      }
      const double S = ss->pricing().S;
      const double K = lot.contract.K;
      const double intrinsic =
          (lot.contract.side == Side::Call) ? std::max(0.0, S - K) : std::max(0.0, K - S);
      settlement += lot.qty * lot.multiplier * (intrinsic - *mark);
    } else {
      alive.push_back(lot);
    }
  }
  // B2 batched settlement: one Marks pass over all expiring lots at the base-date
  // residual T, then accumulate settlement in expiring (input) order. `bs`/`ss`
  // were validated non-null in the partition loop above.
  //
  // L2 (AL-solve-wall sprint): when a mark memo is supplied and enabled, an expiring
  // lot's base mark that the PRIOR step's book-greeks pass already computed (same
  // base date, matching uid surface instance) is SERVED from the memo instead of
  // re-solved — only the memo MISSES go into the Marks batch. The served mark is
  // bit-identical to the solve (FullGreeks mark == Marks mark, L2 crux; per-lane
  // solve independence makes the misses' marks batch-composition-invariant), and the
  // settlement is summed in the SAME expiring order, so memo ON == memo OFF ==
  // legacy, bit-for-bit. When the memo is present but DISABLED, every expiring lot
  // still solves (legacy behavior), and each solve of a memo-available mark bumps
  // DuplicateMarkSolves — the counter L2 drives to 0.
  if (expiring != nullptr && !expiring->empty()) {
    const std::size_t n_exp = expiring->size();
    if (mark_memo == nullptr) {
      // Legacy path (no memo supplied): one Marks pass over ALL expiring lots.
      ATX_TRY(PortfolioPricer * sp, settle->prepare(*expiring, base.ts_ns()));
      settle_frame->resize_marks(sp->portfolio().n_positions());
      ATX_TRY_VOID(sp->price_into(base.set(), PriceFieldMask::Marks, settle_frame->marks_view(),
                                  settle->workspace(), opts));
      const PriceFrame &sf = settle_frame->frame;
      for (std::size_t i = 0; i < n_exp; ++i) {
        const Lot &lot = (*expiring)[i];
        if (i >= sf.size() || sf.id[i] != lot.id) {
          return Err(ErrorCode::Internal,
                     "run_backtest: settlement frame misaligned with expiring lot id=" +
                         std::to_string(lot.id));
        }
        if (sf.status[i] != PriceStatus::Ok) {
          return Err(ErrorCode::NotFound, "run_backtest: no valid base mark for settling lot id=" +
                                              std::to_string(lot.id));
        }
        const SurfaceRef ss = shifted.find(lot.contract.uid);
        const double S = ss->pricing().S;
        const double K = lot.contract.K;
        const double intrinsic =
            (lot.contract.side == Side::Call) ? std::max(0.0, S - K) : std::max(0.0, K - S);
        settlement += lot.qty * lot.multiplier * (intrinsic - sf.price[i]);
      }
    } else {
      // L2 path: served[i] holds a lot's memo mark, or NaN when it must be solved.
      std::vector<double> &served = mark_memo->served_scratch(n_exp);
      std::vector<Lot> &to_solve = mark_memo->solve_scratch();
      for (std::size_t i = 0; i < n_exp; ++i) {
        const Lot &lot = (*expiring)[i];
        const double T_base = residual_T(lot.expiry_ts_ns, base.ts_ns());
        const SurfaceRef bs = base.find(lot.contract.uid); // non-null (partition loop)
        const std::uint64_t inst = bs->instance_id();
        const std::optional<double> mm =
            mark_memo->find(lot.contract.uid, lot.contract.K, T_base, lot.contract.side, inst);
        if (mm.has_value() && memo_enabled) {
          served[i] = *mm; // duplicate settlement solve avoided
        } else {
          if (mm.has_value()) {
            // Memo has it, but consumption is disabled -> the solve below duplicates it.
            counters::ledger::bump(counters::ledger::Solve::DuplicateMarkSolves);
          }
          to_solve.push_back(lot); // served[i] stays NaN -> solved below
        }
      }
      const PriceFrame *sf_ptr = nullptr;
      if (!to_solve.empty()) {
        ATX_TRY(PortfolioPricer * sp, settle->prepare(to_solve, base.ts_ns()));
        settle_frame->resize_marks(sp->portfolio().n_positions());
        ATX_TRY_VOID(sp->price_into(base.set(), PriceFieldMask::Marks, settle_frame->marks_view(),
                                    settle->workspace(), opts));
        sf_ptr = &settle_frame->frame;
      }
      std::size_t solve_ix = 0;
      for (std::size_t i = 0; i < n_exp; ++i) {
        const Lot &lot = (*expiring)[i];
        double mark = 0.0;
        if (!std::isnan(served[i])) {
          mark = served[i];
        } else {
          const PriceFrame &sf = *sf_ptr; // non-null: this lot is a memo miss to solve
          if (solve_ix >= sf.size() || sf.id[solve_ix] != lot.id) {
            return Err(ErrorCode::Internal,
                       "run_backtest: settlement frame misaligned with expiring lot id=" +
                           std::to_string(lot.id));
          }
          if (sf.status[solve_ix] != PriceStatus::Ok) {
            return Err(ErrorCode::NotFound,
                       "run_backtest: no valid base mark for settling lot id=" +
                           std::to_string(lot.id));
          }
          mark = sf.price[solve_ix];
          ++solve_ix;
        }
        const SurfaceRef ss = shifted.find(lot.contract.uid);
        const double S = ss->pricing().S;
        const double K = lot.contract.K;
        const double intrinsic =
            (lot.contract.side == Side::Call) ? std::max(0.0, S - K) : std::max(0.0, K - S);
        settlement += lot.qty * lot.multiplier * (intrinsic - mark);
      }
    }
  }

  PnlTotals t{};
  std::uint32_t n_unpriced = 0;
  std::uint32_t first_unpriced_uid = 0;
  if (target_marks != nullptr) {
    target_marks->prepare(alive.size());
  }
  if (!alive.empty()) {
    // R-35: `alive` IS `retained`'s own alive_ scratch (from reset_alive_scratch
    // above). Passing it back into retained.prepare() is a self-alias, and it is
    // SAFE only because prepare() strictly READS `lots` (into positions_at /
    // tenors_ / key_ copies) and never mutates alive_ (in particular never calls
    // reset_alive_scratch). Keep it that way: prepare() must not write through the
    // scratch buffer while it holds this reference.
    ATX_TRY(PortfolioPricer * pricer, retained.prepare(alive, base.ts_ns()));
    PortfolioWorkspace &workspace = retained.workspace();
    Result<PnlTotals> totals =
        target_marks != nullptr
            ? pricer->pnl_totals_with_target_marks_into(base.set(), shifted.set(),
                                                        target_marks->write_view(), workspace, opts)
            : pricer->pnl_totals(base.set(), shifted.set(), workspace, opts);
    if (!totals) {
      return Err(totals.error());
    }
    t = *totals;
    // Count the alive positions the reduction skipped. `n_ok` is produced by the
    // same serial-scatter reduction (bit-identical across thread counts) and can
    // never exceed the position count; guard the subtraction rather than underflow.
    const std::size_t n_pos = alive.size();
    const std::size_t n_ok = t.n_ok;
    n_unpriced = static_cast<std::uint32_t>((n_pos >= n_ok) ? (n_pos - n_ok) : std::size_t{0});
    if (n_unpriced > 0) {
      if (target_marks != nullptr) {
        // The target-mark scatter already carries the same per-position status
        // used by the totals reduction. Preserve input order and reuse it for
        // diagnostics instead of repricing the entire live strategy book.
        const std::optional<std::size_t> first_bad = target_marks->first_non_ok_index();
        if (!first_bad.has_value() || *first_bad >= alive.size()) {
          return Err(ErrorCode::Internal,
                     "run_backtest: target-mark status disagrees with unpriced count");
        }
        first_unpriced_uid = alive[*first_bad].contract.uid;
      } else {
        auto fr = pricer->pnl_explain(base.set(), shifted.set(), opts);
        if (!fr) {
          return Err(fr.error());
        }
        for (std::size_t i = 0; i < fr->size(); ++i) {
          if (fr->status[i] != PriceStatus::Ok) {
            first_unpriced_uid = fr->uid[i];
            break;
          }
        }
      }
    }
  }
  if (target_marks != nullptr) {
    ATX_TRY_VOID(target_marks->seal());
  }
  return Ok(StepPnl{t, settlement, n_unpriced, first_unpriced_uid, n_deferred, deferred_settle_cash});
}

// The Error-policy message for a step that has `n_unpriced` held lots with no
// surface. Kept next to `compute_step` so both run_backtest overloads word it the
// same. Non-empty precondition: callers only build this when n_unpriced > 0.
[[nodiscard]] std::string unpriced_error_message(std::uint32_t n_unpriced,
                                                 std::uint32_t first_uid) {
  return "run_backtest: " + std::to_string(n_unpriced) +
         " held lot(s) have no surface this step (first uid=" + std::to_string(first_uid) + ")";
}

// The Error-policy message for a recorded row whose book greeks could not value
// `n_unpriced` held lots on `date`. Distinct wording from the step message: this
// is a single-date snapshot (book_greeks), so it names the date rather than "this
// step". Non-empty precondition: callers only build this when n_unpriced > 0.
[[nodiscard]] std::string unpriced_greeks_error_message(std::uint32_t n_unpriced,
                                                        std::uint32_t first_uid,
                                                        const std::string &date) {
  return "run_backtest: " + std::to_string(n_unpriced) + " held lot(s) have no surface on " + date +
         " (first uid=" + std::to_string(first_uid) + ")";
}

[[nodiscard]] constexpr std::string_view purpose_name(SurfacePurpose purpose) noexcept {
  switch (purpose) {
  case SurfacePurpose::MarketMark:
    return "MarketMark";
  case SurfacePurpose::Risk:
    return "Risk";
  }
  return "Unknown";
}

[[nodiscard]] constexpr std::string_view state_name(SurfaceState state) noexcept {
  switch (state) {
  case SurfaceState::Healthy:
    return "Healthy";
  case SurfaceState::Degraded:
    return "Degraded";
  case SurfaceState::Stale:
    return "Stale";
  case SurfaceState::Rejected:
    return "Rejected";
  }
  return "Unknown";
}

// Every economic quantity a step produces is scaled by
// `dt = (shifted.ts_ns - base.ts_ns) / kNsPerYear`. A `Clock` is ordered by DATE
// STRING, but that timestamp comes from the ARCHIVE's `now_ts_ns` — a different
// thing entirely — so a mislabelled or misordered partition hands the loop a
// NEGATIVE dt. Nothing downstream can tell that apart from a real step:
// `exp(r*dt)` shrinks the cash balance instead of growing it, the borrow bleed
// becomes a rebate, the shares-carry term flips sign, and the run still reports
// a full set of plausible rows and no error. Fail closed on the step that would
// compute it, naming both dates.
//
// EQUAL timestamps are accepted deliberately: dt == 0 makes every dt-scaled term
// an exact no-op (`cash * (exp(0) - 1)` is 0.0 for every finite r), so there is
// no sign to flip and nothing to protect the caller from. Refusing a
// degenerate-but-harmless corpus would be a stricter contract than the defect
// warrants. Kept next to `validate_snapshot_provenance` so both run_backtest
// overloads word it the same.
[[nodiscard]] Status validate_step_ordering(const MarketSnapshot &base,
                                            const MarketSnapshot &shifted,
                                            const std::string &base_date,
                                            const std::string &shifted_date) {
  if (shifted.ts_ns() < base.ts_ns()) {
    return Err(ErrorCode::InvalidArgument,
               "run_backtest: snapshot timestamps run backwards over the step " + base_date +
                   " -> " + shifted_date + " (ts_ns " + std::to_string(base.ts_ns()) + " -> " +
                   std::to_string(shifted.ts_ns()) + ")");
  }
  return Ok();
}

// Validate after every cache load, not only while constructing a snapshot. That
// placement means a preloaded or previously cached compatibility snapshot cannot
// bypass a stricter policy selected by a later backtest run.
[[nodiscard]] Status validate_snapshot_provenance(const MarketSnapshot &snapshot,
                                                  SurfaceProvenancePolicy policy) {
  if (policy == SurfaceProvenancePolicy::Compatibility) {
    return Ok();
  }

  // Backing-agnostic (WS-ZC1): a borrowed snapshot exposes the same uid per index.
  const std::size_t n_surfaces = snapshot.n_surfaces();
  const std::span<const SurfaceProvenance> provenances = snapshot.provenances();
  if (provenances.size() != n_surfaces) {
    return Err(ErrorCode::InvalidArgument,
               "run_backtest: snapshot provenance is not aligned with surfaces");
  }
  for (std::size_t index = 0; index < n_surfaces; ++index) {
    const SurfaceRef surface = snapshot.surface_at(index);
    const SurfaceProvenance &provenance = provenances[index];
    const bool admitted =
        !provenance.legacy_format && provenance.purpose == SurfacePurpose::Risk &&
        provenance.served_generation != 0u &&
        (provenance.state == SurfaceState::Healthy || provenance.state == SurfaceState::Degraded);
    if (!admitted) {
      const std::string uid = std::to_string(surface->uid());
      const std::string purpose{purpose_name(provenance.purpose)};
      const std::string state{state_name(provenance.state)};
      const std::string legacy = provenance.legacy_format ? "true" : "false";
      const std::string served_generation = std::to_string(provenance.served_generation);
      return Err(ErrorCode::InvalidArgument,
                 "run_backtest: surface provenance rejected uid=" + uid + " purpose=" + purpose +
                     " state=" + state + " legacy=" + legacy +
                     " served_generation=" + served_generation);
    }
  }
  return Ok();
}

// ReuseOnly may resolve each requested fast snapshot independently to either an
// already-resident fast entry or a cold miss. Configured execution would then
// make the economic P&L route depend on cache history, potentially mixing bases
// within one run. A forced-cold execution is invariant to that residency choice.
[[nodiscard]] Status validate_run_query_route(const RunConfig &cfg) {
  const bool requested_fast = cfg.query_pricing_tier == QueryPricingTier::RepresentativeFast ||
                              cfg.query_pricing_tier == QueryPricingTier::CarryBank;
  if (cfg.query_cache_build_policy == QueryCacheBuildPolicy::ReuseOnly && requested_fast &&
      cfg.price.query_execution != QueryExecution::ColdReference) {
    return Err(ErrorCode::InvalidArgument,
               "run_backtest: ReuseOnly with a fast requested tier requires ColdReference "
               "economic query execution");
  }
  return Ok();
}

} // namespace

// The accessor `backtest.hpp` declares. Out of line so the roster has exactly
// one definition and every consumer -- `validate()`, `push_row`, and both
// `backtest_db` rosters -- reaches the same object rather than its own copy.
std::span<const BacktestExplainColumn> swap_explain_columns() noexcept {
  return std::span<const BacktestExplainColumn>{kSwapExplainColumns};
}

// ── Clock ─────────────────────────────────────────────────────────────────

Result<Clock> Clock::from_manifest(const CorpusManifest &manifest) {
  Clock clock;
  // `manifest.dates` are unique + ascending; `entries` are sorted (date asc,
  // symbol asc) so the first Ok entry per date is deterministic.
  for (const std::string &d : manifest.dates) {
    for (const CorpusEntry &e : manifest.entries) {
      if (e.date == d && e.status == CorpusFitStatus::Ok && !e.archive_path.empty()) {
        clock.refs_.push_back(SnapshotRef{d, e.archive_path});
        break;
      }
    }
  }
  if (clock.refs_.empty()) {
    return Err(ErrorCode::InvalidArgument, "Clock::from_manifest: no Ok snapshots in manifest");
  }
  return Ok(std::move(clock));
}

Result<Clock> Clock::from_surface_db(const SurfaceDb &db) {
  auto parts = db.partitions();
  if (parts.empty()) {
    return Err(ErrorCode::InvalidArgument, "Clock::from_surface_db: surface db has no partitions");
  }
  std::sort(parts.begin(), parts.end(),
            [](const DbPartitionInfo &a, const DbPartitionInfo &b) { return a.key < b.key; });
  Clock clock;
  clock.refs_.reserve(parts.size());
  const std::filesystem::path dir = std::filesystem::path(db.root()) / kSurfaceDbPartitionDir;
  for (const auto &p : parts) {
    clock.refs_.push_back(
        SnapshotRef{p.key, (dir / (p.key + std::string(kSurfaceDbPartitionExt))).string()});
  }
  return Ok(std::move(clock));
}

Result<Clock> Clock::between(std::string_view date_lo, std::string_view date_hi) const {
  // Rendered once and shared by both failure messages: the window a caller CAN
  // ask for. An empty clock is unreachable through either factory (both reject
  // an empty source), but `Clock` is default-constructible, so this stays total
  // rather than indexing front()/back() on an empty vector.
  const std::string available =
      refs_.empty() ? std::string{"<none>"} : (refs_.front().date + ".." + refs_.back().date);
  if (date_lo > date_hi) {
    return Err(ErrorCode::InvalidArgument, "Clock::between: date_lo '" + std::string{date_lo} +
                                               "' > date_hi '" + std::string{date_hi} +
                                               "' (available " + available + ")");
  }
  Clock out;
  // Lexicographic == chronological for the canonical ISO date keys, and refs_ is
  // already ascending, so a linear filter preserves the ordering contract.
  for (const SnapshotRef &r : refs_) {
    const std::string_view d{r.date};
    if (d >= date_lo && d <= date_hi) {
      out.refs_.push_back(r);
    }
  }
  if (out.refs_.empty()) {
    return Err(ErrorCode::InvalidArgument, "Clock::between: no snapshots in [" +
                                               std::string{date_lo} + ", " + std::string{date_hi} +
                                               "] (available " + available + ")");
  }
  return Ok(std::move(out));
}

// ── MarketSnapshot ──────────────────────────────────────────────────────────

MarketSnapshot::MarketSnapshot(std::shared_ptr<const SurfaceArchiveV2> archive,
                               std::vector<PricedSurface> &&surfaces,
                               std::vector<PricedSurfaceView> &&views,
                               std::vector<SurfaceProvenance> &&provenance, SurfaceSet &&set,
                               std::int64_t ts, ArchiveContentIdentity source_identity,
                               std::vector<std::pair<std::string, std::uint32_t>> &&syms) noexcept
    : archive_{std::move(archive)}, surfaces_{std::move(surfaces)}, views_{std::move(views)},
      provenance_{std::move(provenance)}, set_{std::move(set)}, source_identity_{source_identity},
      ts_ns_{ts}, syms_{std::move(syms)} {}

std::uint64_t MarketSnapshot::open_count() noexcept { return g_open_count.load(); }
void MarketSnapshot::reset_open_count() noexcept { g_open_count.store(0); }

std::uint64_t MarketSnapshot::deserialized_bytes() noexcept { return g_deserialized_bytes.load(); }
void MarketSnapshot::reset_deserialized_bytes() noexcept { g_deserialized_bytes.store(0); }

Result<MarketSnapshot> MarketSnapshot::load(std::string_view archive_path,
                                            QueryPricingTier query_pricing_tier,
                                            std::span<const std::uint32_t> referenced_uids,
                                            ArchiveBacking backing) {
  ATX_VOL_PROFILE_SCOPE(SnapshotLoad);
  // WS-ZC1: which BACKING the borrowed views read from. `open_mapped` is demand-paged
  // and copy-free, but on Windows a file with an active mapped section CANNOT be
  // deleted or replaced (ERROR_USER_MAPPED_FILE) regardless of share mode — and a
  // borrowed snapshot holds its archive for its whole lifetime. That would break
  // atomic republish of a partition (write .tmp + rename) while any reader is live,
  // which `Backtest.SnapshotCacheEvictsStaleEntryWhenArchiveRewrittenSameLength`
  // pins as a supported workflow.
  //
  // So a BORROWING load uses `open_copied`: map, ONE memcpy into an owned buffer, drop
  // the mapping. Views then borrow that buffer (kept alive by the same archive
  // shared_ptr), no section stays open against the file, and republish keeps working.
  // The copy runs at memory bandwidth out of pages the OS cache already holds, so it
  // keeps nearly all of the mapped open's speed — `open_file`'s stream read, the
  // obvious alternative, measured ~210 ms over this replay versus ~8 ms to map. A
  // whole-board borrow touches every record anyway, so the demand-paging that motivated
  // `open_mapped` (WS-S S2) has nothing left to skip here.
  //
  // The owned (non-borrowing) path keeps `open_mapped`: it reconstructs and then drops
  // the archive inside this function, so it never holds a mapping past the load and
  // still benefits from faulting in only the subset it reconstructs.
  // ESCAPE HATCH / MEASUREMENT LEVER: `ATX_VOL_ZC_BORROW=0` forces the owned
  // reconstruct path. It exists so the two backings can be A/B'd inside ONE binary on
  // ONE host — which is how the WS-ZC1 numbers were taken, and how they should be
  // re-taken — and so a borrow-specific regression can be bisected without a rebuild.
  // Read once; it cannot change mid-run, so it cannot make a run non-deterministic.
  static const bool borrow_allowed = []() {
#if defined(_WIN32)
    std::size_t sz = 0;
    char buf[8] = {};
    if (getenv_s(&sz, buf, sizeof(buf), "ATX_VOL_ZC_BORROW") != 0 || sz == 0) {
      return true;
    }
    return std::string_view{buf} != "0";
#else
    const char *e = std::getenv("ATX_VOL_ZC_BORROW");
    return e == nullptr || std::string_view{e} != "0";
#endif
  }();
  // BORROW INSTEAD OF RECONSTRUCT. `PricedSurfaceView` carries no query accelerator: it
  // IS the cold reference route. So it can serve the two cache-free tiers
  // (LegacyCompatible, ColdReference) bit-for-bit, and only those — a fast tier must
  // still reconstruct owned surfaces because `with_query_pricing` has to build a real
  // accelerator. Since WS-P1v, both forms drive the same laned analytic-Greek kernels,
  // so borrowing costs no pricing performance on the tiers it serves.
  const bool borrow = borrow_allowed && (query_pricing_tier == QueryPricingTier::LegacyCompatible ||
                                         query_pricing_tier == QueryPricingTier::ColdReference);

  auto arch = [&]() {
    ATX_VOL_PROFILE_SCOPE(ArchiveOpen);
    if (borrow) {
      // The CALLER declares the archive's lifecycle; the loader never guesses.
      // `ATX_VOL_ZC_BACKING=map|copy` is an override/escape hatch for measurement and
      // for forcing either backing without a rebuild. Read once, so it cannot make a
      // run non-deterministic.
      static const std::optional<ArchiveBacking> backing_override = []() {
        auto parse = [](std::string_view v) -> std::optional<ArchiveBacking> {
          if (v == "map") {
            return ArchiveBacking::Sealed;
          }
          if (v == "copy") {
            return ArchiveBacking::Mutable;
          }
          return std::nullopt;
        };
#if defined(_WIN32)
        std::size_t sz = 0;
        char buf[8] = {};
        if (getenv_s(&sz, buf, sizeof(buf), "ATX_VOL_ZC_BACKING") != 0 || sz == 0) {
          return std::optional<ArchiveBacking>{};
        }
        return parse(std::string_view{buf});
#else
        const char *e = std::getenv("ATX_VOL_ZC_BACKING");
        return e != nullptr ? parse(std::string_view{e}) : std::optional<ArchiveBacking>{};
#endif
      }();
      const ArchiveBacking effective = backing_override.value_or(backing);
      if (effective == ArchiveBacking::Sealed) {
        // Read-only corpus: keep the mapping, copy nothing. No section is ever closed,
        // so this must not be used for a partition the store may rewrite or delete.
        return SurfaceArchiveV2::open_mapped(archive_path);
      }
      // Map + one memcpy into an owned buffer, mapping dropped (see the note above).
      return SurfaceArchiveV2::open_copied(archive_path);
    }
    // S2 (WS-S): mmap the partition instead of reading the whole file into an owned
    // heap buffer. open_impl CRC-validates only the header/lookup/directory (small),
    // so this open faults in only metadata pages; a subset load (below) then faults
    // in only the referenced records' pages via the OS page cache — the whole-file
    // read amplification (67% of backtest wall in the profile) is eliminated. The
    // owned path reconstructs and drops this archive before returning, so it never
    // holds a mapping past the load.
    //
    // MEASURED AND KEPT for the WHOLE-BOARD path too, against the intuition that a
    // reader touching every record should prefer one buffered read. It should not,
    // here: the reconstruct reads the mapped bytes DIRECTLY, so a read's kernel copy
    // into a 1.8 MB owned buffer is work with no consumer, while the mapping's
    // first-touch faults are served from the page cache. Interleaved on the
    // 135-session projected replay (median wall, 5 reps): mmap 295 ms vs buffered
    // read 315 ms at look-ahead depth 8, and 368 ms vs 503 ms at depth 1 — mmap
    // ahead at every depth, and further ahead the less the loads overlap. Cold
    // (page cache evicted between reps) it also held: 431-464 ms vs 464-501 ms.
    // An explicit PrefetchVirtualMemory populate of the whole mapping was measured
    // too and is NOT used: 427-444 ms cold and 298-308 ms warm, inside the noise of
    // plain mmap, so it buys nothing for the extra call.
    return SurfaceArchiveV2::open_mapped(archive_path);
  }();
  if (!arch) {
    return Err(arch.error());
  }
  // One archive-open event (the load-once gate asserts N loads => N opens).
  g_open_count.fetch_add(1, std::memory_order_relaxed);

  // WS-ZC1: the snapshot co-owns the archive — and therefore the buffer every borrowed
  // view reads — for its whole lifetime. See the lifetime note on MarketSnapshot's
  // members.
  auto archive = std::make_shared<const SurfaceArchiveV2>(std::move(*arch));
  const ArchiveContentIdentity source_identity = archive->identity();

  const std::span<const ArchiveV2DirEntry> dir = archive->directory();
  std::vector<PricedSurface> surfaces;
  std::vector<PricedSurfaceView> views;
  std::vector<SurfaceProvenance> provenance;

  // B1 subset-deserialize (bottleneck #1 at the reader): when the caller names the
  // uids its book references, reconstruct ONLY those directory entries
  // (`reconstruct_symbol` per referenced uid) and DROP the whole-board
  // `reconstruct_all_with_provenance`. An empty referenced set (the strategy
  // overload — its touched names are not known before on_step — and any shared-cache
  // caller) keeps the whole-board load. If the subset matches no directory entry
  // (e.g. a book naming only names absent from this partition), keep an empty
  // SurfaceSet and read only one mapped record for the valuation timestamp. Loading
  // the full board on a miss turns the cheapest missing-name case into worst-case I/O.
  //
  // WS-ZC1 lands the seam §6 note that used to sit here: `SurfaceSet` now holds
  // `SurfaceRef`, so on the two cache-free tiers the subset AND the whole board are
  // served as zero-copy `PricedSurfaceView`s over the mapped records. The archive
  // reconstruction that dominated replay (`archive_map`, ~49% of wall) disappears on
  // those tiers; a fast tier still reconstructs owned surfaces below.
  const auto n_surfaces = [&]() { return borrow ? views.size() : surfaces.size(); };
  const bool subset_requested = !referenced_uids.empty();
  bool loaded_subset = false;
  if (subset_requested) {
    ATX_VOL_PROFILE_SCOPE(ArchiveMap);
    // O(dir) match via a hash set of the referenced uids (built once) instead of an
    // O(dir x subset) nested scan.
    const std::unordered_set<std::uint32_t> wanted_uids(referenced_uids.begin(),
                                                        referenced_uids.end());
    surfaces.reserve(borrow ? 0u : referenced_uids.size());
    views.reserve(borrow ? referenced_uids.size() : 0u);
    provenance.reserve(referenced_uids.size());
    for (const ArchiveV2DirEntry &e : dir) {
      if (wanted_uids.find(e.uid) == wanted_uids.end()) {
        continue;
      }
      g_deserialized_bytes.fetch_add(e.surface_size, std::memory_order_relaxed); // F5
      // S3 (WS-S): build the surface AND its provenance from the directory entry `e`
      // already in hand — ONE pass over the record extent, no hash re-probe.
      if (borrow) {
        auto rec = archive->map_entry(e);
        if (!rec) {
          return Err(rec.error());
        }
        views.push_back(std::move(rec->view));
        provenance.push_back(std::move(rec->provenance));
      } else {
        auto rec = archive->reconstruct_entry(e);
        if (!rec) {
          return Err(rec.error());
        }
        surfaces.push_back(std::move(rec->surface));
        provenance.push_back(std::move(rec->provenance));
      }
    }
    loaded_subset = n_surfaces() != 0u;
  }
  const bool subset_missed = subset_requested && !loaded_subset;
  if (!subset_requested) {
    ATX_VOL_PROFILE_SCOPE(ArchiveMap);
    surfaces.clear();
    views.clear();
    provenance.clear();
    // F5: the whole-board load materializes every directory entry. Counted here
    // (rather than inside the archive) so the subset and whole-board paths are
    // measured by the same rule and are directly comparable.
    for (const ArchiveV2DirEntry &e : dir) {
      g_deserialized_bytes.fetch_add(e.surface_size, std::memory_order_relaxed);
    }
    if (borrow) {
      auto mapped = archive->map_all_with_provenance();
      if (!mapped) {
        return Err(mapped.error());
      }
      views.reserve(mapped->size());
      provenance.reserve(mapped->size());
      for (ArchivedSurfaceView &record : *mapped) {
        views.push_back(std::move(record.view));
        provenance.push_back(std::move(record.provenance));
      }
    } else {
      auto mapped = archive->reconstruct_all_with_provenance();
      if (!mapped) {
        return Err(mapped.error());
      }
      surfaces.reserve(mapped->size());
      provenance.reserve(mapped->size());
      for (ArchivedSurface &record : *mapped) {
        surfaces.push_back(std::move(record.surface));
        provenance.push_back(std::move(record.provenance));
      }
    }
  }
  if (n_surfaces() == 0u && !subset_missed) {
    return Err(ErrorCode::InvalidArgument, "MarketSnapshot::load: archive holds no surfaces");
  }

  // Valuation timestamp: the surfaces of one date agree on now_ts_ns. Read through
  // whichever backing was populated. A requested subset that matched no uid
  // intentionally owns no surface; map only the first record to recover its timestamp
  // without reconstructing/materializing the full board.
  const auto pricing_at = [&](std::size_t i) -> const PricingContext & {
    return borrow ? views[i].pricing() : surfaces[i].pricing();
  };
  std::int64_t ts = 0;
  if (subset_missed) {
    if (dir.empty()) {
      return Err(ErrorCode::InvalidArgument, "MarketSnapshot::load: archive holds no surfaces");
    }
    auto metadata_record = archive->map_entry(dir.front());
    if (!metadata_record) {
      return Err(metadata_record.error());
    }
    ts = metadata_record->view.pricing().now_ts_ns;
  } else {
    ts = pricing_at(0).now_ts_ns;
    for (std::size_t i = 0; i < n_surfaces(); ++i) {
      if (pricing_at(i).now_ts_ns != ts) {
        return Err(ErrorCode::InvalidArgument,
                   "MarketSnapshot::load: surfaces disagree on now_ts_ns within a date");
      }
    }
  }

  // Query caches are runtime accelerators, not archive state. Prepare every
  // mapped surface under the caller's tier before building the pointer set, so
  // no partially-prepared snapshot can become observable. A borrowed view carries no
  // accelerator by construction (it IS the cold route), so this is the owned path
  // only — and `borrow` is false for exactly the tiers that need one.
  for (PricedSurface &surface : surfaces) {
    auto prepared = std::move(surface).with_query_pricing(query_pricing_tier);
    if (!prepared) {
      return Err(prepared.error());
    }
    surface = std::move(*prepared);
  }
  // A borrowed view has nothing to prepare (it IS the cold route), but it still
  // records WHICH cold tier it is serving so a borrowed snapshot reports the same
  // tier an owned one prepared at would.
  for (PricedSurfaceView &v : views) {
    if (Status s = v.set_cold_query_pricing_tier(query_pricing_tier); !s.has_value()) {
      return Err(s.error());
    }
  }

  // Non-owning resolver over the surfaces' stable addresses. Neither vector is
  // mutated after this point, so the refs stay valid for the snapshot's lifetime
  // (and across a move, which preserves element addresses).
  auto set = [&]() {
    if (borrow) {
      std::vector<const PricedSurfaceView *> ptrs;
      ptrs.reserve(views.size());
      for (const PricedSurfaceView &v : views) {
        ptrs.push_back(&v);
      }
      return SurfaceSet::create_from_views(ptrs);
    }
    std::vector<const PricedSurface *> ptrs;
    ptrs.reserve(surfaces.size());
    for (const PricedSurface &s : surfaces) {
      ptrs.push_back(&s);
    }
    return SurfaceSet::create(ptrs);
  }();
  if (!set) {
    return Err(set.error());
  }

  // symbol -> uid from the WHOLE archive directory (canonical symbol bytes) even
  // under a subset load: uid_of must still resolve any archived name (a strategy may
  // query a name its current book does not hold); this is cheap (strings, no surfaces).
  std::vector<std::pair<std::string, std::uint32_t>> syms;
  syms.reserve(dir.size());
  for (const ArchiveV2DirEntry &e : dir) {
    syms.emplace_back(std::string(e.symbol, e.symbol_len), e.uid);
  }
  // 6.6: SORTED BY SYMBOL so `uid_of` is a binary search instead of a scan of the
  // whole directory. The order is not observable anywhere else — `syms_` has exactly
  // one reader (`uid_of`) — and sorting is paid once per archive load against a
  // lookup that a strategy runs per leg per step.
  //
  // STABLE, and that is load-bearing rather than a default-grade choice.
  // `canonical_symbol` TRUNCATES (and upper-cases), so two distinct archived names can
  // collapse to the same canonical bytes while carrying different uids. `uid_of`'s
  // `lower_bound` then resolves to whichever of them sorted first, and only a stable
  // sort makes that the one that appeared first in the DIRECTORY — which is exactly
  // what the linear scan this replaces returned. Under `std::sort` the winner among
  // equal keys is unspecified, i.e. this optimization would silently gain the ability
  // to move a resolved uid on a malformed archive.
  std::stable_sort(syms.begin(), syms.end(),
                   [](const auto &a, const auto &b) { return a.first < b.first; });

  // The snapshot co-owns `archive` — the Mapping every borrowed view reads — so the
  // mapping outlives the views, which outlive nothing else. See MarketSnapshot's
  // member lifetime note.
  return MarketSnapshot{std::move(archive),    std::move(surfaces), std::move(views),
                        std::move(provenance), std::move(*set),     ts,
                        source_identity,       std::move(syms)};
}

const SurfaceProvenance *MarketSnapshot::provenance(std::uint32_t uid) const noexcept {
  if (provenance_.size() != n_surfaces()) {
    return nullptr;
  }
  for (std::size_t i = 0; i < n_surfaces(); ++i) {
    if (surface_at(i).uid() == uid) {
      return &provenance_[i];
    }
  }
  return nullptr;
}

std::optional<std::uint32_t> MarketSnapshot::uid_of(std::string_view symbol) const {
  // The directory stores CANONICAL symbol bytes (ASCII-upper, truncated), so
  // canonicalize the query the same way before comparing. Without this a
  // lower-case or over-long query would miss a symbol that is present — and a
  // universe authored in lower case would fail to resolve. `canonical_symbol` is
  // the single source of truth shared with `uid_for_symbol` (the write side).
  const std::string query = canonical_symbol(symbol);
  // `syms_` is STABLE-sorted by symbol at load (see MarketSnapshot::load), so this is
  // a binary search. When two directory entries share canonical bytes, `lower_bound`
  // picks the first of the equal run, and stability makes that the first such entry in
  // the directory — which is the one the linear scan this replaces returned. So a
  // (malformed) archive with a repeated symbol still resolves to the same uid it did
  // before; see the stability note at the sort itself for why that is not free.
  const auto it = std::lower_bound(
      syms_.begin(), syms_.end(), query,
      [](const std::pair<std::string, std::uint32_t> &a, const std::string &q) {
        return a.first < q;
      });
  if (it == syms_.end() || it->first != query) {
    return std::nullopt;
  }
  return it->second;
}

// ── Driver ──────────────────────────────────────────────────────────────────

void ReusableTargetMarkFrame::prepare(std::size_t n) {
  if (id_.size() < n) {
    id_.resize(n);
    raw_mark_.resize(n);
    base_vega_proxy_.resize(n);
    status_.resize(n);
    order_.resize(n);
  }
  active_size_ = n;
  sealed_ = false;
}

TargetMarkView ReusableTargetMarkFrame::write_view() noexcept {
  sealed_ = false;
  return TargetMarkView{std::span<std::uint64_t>{id_.data(), active_size_},
                        std::span<double>{raw_mark_.data(), active_size_},
                        std::span<PriceStatus>{status_.data(), active_size_},
                        std::span<double>{base_vega_proxy_.data(), active_size_}};
}

std::optional<std::size_t> ReusableTargetMarkFrame::first_non_ok_index() const noexcept {
  for (std::size_t i = 0; i < active_size_; ++i) {
    if (status_[i] != PriceStatus::Ok) {
      return i;
    }
  }
  return std::nullopt;
}

Status ReusableTargetMarkFrame::seal() {
  sealed_ = false;
  if (active_size_ == 0u) {
    sealed_ = true;
    return Ok();
  }
  std::iota(order_.begin(), order_.begin() + static_cast<std::ptrdiff_t>(active_size_),
            std::size_t{0});
  std::sort(order_.begin(), order_.begin() + static_cast<std::ptrdiff_t>(active_size_),
            [this](std::size_t lhs, std::size_t rhs) { return id_[lhs] < id_[rhs]; });
  for (std::size_t i = 1; i < active_size_; ++i) {
    const std::uint64_t id = id_[order_[i]];
    if (id_[order_[i - 1u]] == id) {
      return Err(ErrorCode::InvalidArgument,
                 "run_backtest: duplicate target-mark lot id=" + std::to_string(id));
    }
  }
  sealed_ = true;
  return Ok();
}

std::optional<ReusableTargetMarkFrame::Match>
ReusableTargetMarkFrame::find_ok(std::uint64_t id) const noexcept {
  if (!sealed_ || active_size_ == 0u) {
    return std::nullopt;
  }
  const auto first = order_.begin();
  const auto last = first + static_cast<std::ptrdiff_t>(active_size_);
  const auto it = std::lower_bound(
      first, last, id, [this](std::size_t index, std::uint64_t key) { return id_[index] < key; });
  if (it == last || id_[*it] != id) {
    return std::nullopt;
  }
  const std::size_t index = *it;
  const double raw_mark = raw_mark_[index];
  const double base_vega_proxy = base_vega_proxy_[index];
  if (status_[index] != PriceStatus::Ok || !std::isfinite(raw_mark) || raw_mark < 0.0 ||
      !std::isfinite(base_vega_proxy)) {
    return std::nullopt;
  }
  return Match{raw_mark, base_vega_proxy};
}

// ── BacktestResult column-shape invariant (plan item 4.6) ───────────────────
//
// See the contract on `BacktestResult::validate`. Pure read: the check never
// touches a value, so a validated result is bit-identical to an unvalidated one.
namespace {

[[nodiscard]] Status column_shape_error(std::string_view column, std::size_t got,
                                        std::size_t rows) {
  std::string msg{"BacktestResult column '"};
  msg += column;
  msg += "' has ";
  msg += std::to_string(got);
  msg += " rows but 'date' has ";
  msg += std::to_string(rows);
  msg += "; every column is empty or exactly row-parallel";
  return Err(ErrorCode::InvalidArgument, std::move(msg));
}

// Empty-or-row-parallel, the invariant every column but `step_pnl_total` obeys.
template <class T>
[[nodiscard]] Status check_column(std::string_view name, const std::vector<T> &col,
                                  std::size_t rows) {
  if (!col.empty() && col.size() != rows) {
    return column_shape_error(name, col.size(), rows);
  }
  return Ok();
}

} // namespace

Status BacktestResult::validate() const {
  const std::size_t rows = date.size();

  ATX_TRY_VOID(check_column("ts_ns", ts_ns, rows));

  // The 25 canonical F64 series columns, driven off the SAME {name, member}
  // table both serializers iterate. One list: a column cannot reach the wire
  // without also entering this check.
  for (const BacktestSeriesColumn &column : backtest_series_columns()) {
    ATX_TRY_VOID(check_column(column.name, this->*column.member, rows));
  }

  // The two documented empty-or-row-parallel extras, deliberately absent from
  // the frozen wire registry (see their member comments).
  ATX_TRY_VOID(check_column("gross_vega_abs", gross_vega_abs, rows));
  ATX_TRY_VOID(check_column("nav_liquidation", nav_liquidation, rows));
  // Task F-8: the explain columns, driven off the ONE roster rather than
  // re-listed. They join `gross_vega_abs`/`nav_liquidation` rather than the
  // frozen registry above -- and note `swap_pv`/`swap_pnl` are STILL absent from
  // both, a pre-existing gap this task did not widen but did not close.
  for (const BacktestExplainColumn &column : swap_explain_columns()) {
    ATX_TRY_VOID(check_column(std::string(column.name), this->*column.member, rows));
  }

  // `step_pnl_total` is EXEMPT: length == refs-1 by contract, not parallel to
  // the downsampled `date`.

  for (std::size_t i = 0; i < signals.size(); ++i) {
    const std::string &name = signals[i].first;
    if (name.empty()) {
      return Err(ErrorCode::InvalidArgument,
                 "BacktestResult signal " + std::to_string(i) +
                     " has an empty name; both serializers emit one column per signal");
    }
    for (std::size_t j = 0; j < i; ++j) {
      if (signals[j].first == name) {
        return Err(ErrorCode::InvalidArgument,
                   "BacktestResult has duplicate signal name '" + name +
                       "'; the emitted series header would be ambiguous");
      }
    }
    ATX_TRY_VOID(check_column("signal '" + name + "'", signals[i].second, rows));
  }

  return Ok();
}

Result<BacktestResult> run_backtest(const Clock &clock, PortfolioState initial,
                                    const RunConfig &cfg) {
  ATX_VOL_PROFILE_SCOPE(BacktestTotal);
  ATX_TRY_VOID(validate_run_config(cfg));
  ATX_TRY_VOID(validate_run_query_route(cfg));
  // Fail closed rather than silently ignore: this overload never calls on_step, so
  // there is no post-on_step strategy state for a StepEvent to carry. A dropped
  // observer would be a dropped observation the caller believes it made.
  if (cfg.step_observer) {
    return Err(ErrorCode::InvalidArgument,
               "run_backtest: RunConfig::step_observer requires the strategy overload (the "
               "fixed-book run has no on_step)");
  }
  // Fail closed rather than silently ignore: the swap lane settles into a cash
  // ledger this overload does not have (its `cash` column is 0.0 by
  // construction), so a swap lot here has nowhere honest to pay out.
  if (!initial.swap_lots.empty()) {
    return Err(ErrorCode::InvalidArgument,
               "run_backtest: PortfolioState::swap_lots requires the strategy overload (the "
               "fixed-book run has no cash ledger to settle a swap into)");
  }
  ATX_TRY_VOID(validate_lot_economics(initial.lots, "initial fixed book"));
  const std::span<const SnapshotRef> refs = clock.refs();
  if (refs.empty()) {
    return Err(ErrorCode::InvalidArgument, "run_backtest: empty clock");
  }
  const std::size_t stride = cfg.record_every_n;

  BacktestResult out;
  PortfolioState book = std::move(initial);
  RetainedBookPricer retained_pricer;
  RetainedBookPricer settle_pricer; // B2: retained batched-settlement pricer
  ReusablePriceFrame settle_frame;  // B2: retained Marks frame for settlement
  StepMarkMemo mark_memo;           // L2: per-step settlement-mark memo
  ReusableLotIdIndex initial_lot_index;
  ATX_TRY_VOID(initial_lot_index.rebuild(book.lots, "initial fixed book"));

  // Append one row from fully-computed step totals.
  const auto push_row = [&out](const std::string &date, std::int64_t ts, double p_total,
                               double p_delta, double p_gamma, double p_vega, double p_vanna,
                               double p_volga, double p_theta, double p_rho, double p_charm,
                               double p_unexpl, double p_settle, double nav_v, const PriceTotals &g,
                               std::size_t n_lots, double n_unpriced, double n_unpriced_greeks) {
    out.date.push_back(date);
    out.ts_ns.push_back(ts);
    out.pnl_total.push_back(p_total);
    out.pnl_delta.push_back(p_delta);
    out.pnl_gamma.push_back(p_gamma);
    out.pnl_vega.push_back(p_vega);
    out.pnl_vanna.push_back(p_vanna);
    out.pnl_volga.push_back(p_volga);
    out.pnl_theta.push_back(p_theta);
    out.pnl_rho.push_back(p_rho);
    out.pnl_charm.push_back(p_charm);
    out.pnl_unexplained.push_back(p_unexpl);
    out.pnl_settlement.push_back(p_settle);
    // Fixed-book overload: no strategy trades, no ledger — zero-fill the B2 columns.
    out.pnl_shares.push_back(0.0);
    out.financing.push_back(0.0);
    out.cost.push_back(0.0);
    out.nav.push_back(nav_v);
    out.cash.push_back(0.0);
    out.gross_delta.push_back(g.delta);
    out.gross_gamma.push_back(g.gamma);
    out.gross_vega.push_back(g.vega);         // NET (signed); see backtest.hpp
    out.gross_vega_abs.push_back(g.abs_vega); // C-3: the genuinely GROSS series
    out.gross_theta.push_back(g.theta);
    out.turnover_notional.push_back(0.0);
    out.turnover_vega.push_back(0.0);
    // The swap lane is strategy-only (a non-empty swap_lots is rejected above),
    // so these stay row-parallel and exactly zero.
    out.swap_pv.push_back(0.0);
    out.swap_pnl.push_back(0.0);
    // Task F-8's explain columns are deliberately NOT pushed here: this is the
    // fixed-book overload, which refuses swap lots outright, so they stay EMPTY
    // rather than zero-filled -- the same distinction `nav_liquidation` makes on
    // this same overload.
    out.n_open_lots.push_back(static_cast<double>(n_lots));
    out.n_unpriced_lots.push_back(n_unpriced);
    out.n_unpriced_greeks.push_back(n_unpriced_greeks);
  };

  // base = load(refs[0]) — the inception snapshot.
  // B1 subset-deserialize: a PRIVATE per-run cache deserializes only the fixed book's
  // referenced uids (the book is known up front and never grows in this overload), so
  // an archive/partition holding more names than the book references drops the
  // whole-board reconstruct (bottleneck #1). A caller-supplied (shared) cache stays
  // whole-board — it may be reused across books with different referenced sets.
  std::vector<std::uint32_t> book_uids;
  book_uids.reserve(book.lots.size());
  for (const Lot &lot : book.lots) {
    if (std::find(book_uids.begin(), book_uids.end(), lot.contract.uid) == book_uids.end()) {
      book_uids.push_back(lot.contract.uid);
    }
  }
  // WS-ZC1: a backtest replays a SEALED corpus — the clock's partitions are historical
  // and nothing in a run rewrites, evicts, or deletes them — so its snapshots may keep
  // the archive mapped and skip the whole-archive copy entirely. This is the explicit
  // caller declaration ArchiveBacking documents; the store path never makes it, so the
  // SurfaceDb write -> reopen -> rewrite/delete cycle keeps its buffered backing.
  //
  // ONLY THE PRIVATE CACHE MAY BE SEALED (WS-ZC regression fix). The backing is part
  // of the snapshot-cache key, so declaring Sealed on a cache we did not create does
  // not merely re-tune it — it orphans every entry the CALLER preloaded under the
  // default Mutable backing, silently re-loading them and silently downgrading a
  // ReuseOnly fast request to ColdReference. A caller-supplied cache is therefore
  // used exactly as the caller configured it; the Sealed win is taken on the cache
  // this function constructs and exclusively owns, which is the replay path the
  // optimization was built for and the one no caller can observe.
  const std::size_t prefetch_depth = effective_prefetch_depth(cfg);
  const std::shared_ptr<SnapshotCache> snapshot_cache =
      cfg.snapshot_cache
          ? cfg.snapshot_cache
          : std::make_shared<SnapshotCache>(private_snapshot_cache_capacity(prefetch_depth),
                                            std::move(book_uids), ArchiveBacking::Sealed);
  auto base_res = snapshot_cache->load(refs[0].archive_path, cfg.query_pricing_tier,
                                       cfg.query_cache_build_policy);
  if (!base_res) {
    return Err(base_res.error());
  }
  std::shared_ptr<const MarketSnapshot> base = std::move(*base_res);
  ATX_TRY_VOID(validate_snapshot_provenance(*base, cfg.surface_provenance_policy));
  ATX_TRY_VOID(validate_lot_economics(book.lots, "initial fixed book", base->ts_ns()));
  if (cfg.prefetch_snapshots) {
    // Initial fill of the whole look-ahead window; each later step adds only the
    // one ref that newly enters it.
    ATX_TRY_VOID(prefetch_window(*snapshot_cache, refs, 1u, prefetch_depth, cfg));
  }

  double nav = 0.0;

  // Row 0: inception (zero PnL, nav 0, book greeks on the first date). Even though
  // no step has run, book_greeks is a real measurement here — an inception book with
  // an unpriced held lot aborts under the Error policy (an empty book prices to 0).
  {
    // L2: inception book-greeks seeds the mark memo for date 0, so a lot expiring on
    // step 1 settles from the memo instead of re-solving.
    auto g = book_greeks(*base, book.lots, cfg.price, retained_pricer, &mark_memo);
    if (!g) {
      return Err(g.error());
    }
    if (cfg.unpriced == UnpricedLotPolicy::Error && g->n_unpriced > 0) {
      return Err(ErrorCode::NotFound,
                 unpriced_greeks_error_message(g->n_unpriced, g->first_unpriced_uid, refs[0].date));
    }
    push_row(refs[0].date, base->ts_ns(), 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0,
             0.0, g->total, book.lots.size(), 0.0, static_cast<double>(g->n_unpriced));
  }

  // Block accumulators for record_every_n>1: each non-recorded step's per-axis PnL
  // is summed here and flushed into the next recorded row, so a recorded row carries
  // the WHOLE block's flow (not just its last step). At stride 1 each block is one
  // step, reproducing the per-step columns bit-for-bit. `nav` is unaffected — it
  // already accumulates every step below.
  double b_total = 0.0, b_delta = 0.0, b_gamma = 0.0, b_vega = 0.0, b_vanna = 0.0, b_volga = 0.0;
  double b_theta = 0.0, b_rho = 0.0, b_charm = 0.0, b_unexpl = 0.0, b_settle = 0.0,
         b_nunpriced = 0.0;

  // Deferred expiry settlements (ExcludeAndReport only). A null pointer under
  // Error keeps `compute_step` on its pre-change fail-closed path verbatim.
  DeferredSettlementBook deferrals;
  DeferredSettlementBook *const deferrals_ptr =
      cfg.unpriced == UnpricedLotPolicy::ExcludeAndReport ? &deferrals : nullptr;

  for (std::size_t i = 1; i < refs.size(); ++i) {
    // Plan 5.5 safe point: the TOP of the step, before this step's snapshot load
    // and before any row is appended. `out` is abandoned along with the error, so
    // no partially-filled result can reach a caller, a writer, or validate() —
    // the run returns no BacktestResult at all. Neither run_backtest overload
    // performs any file I/O, so nothing on disk is touched either way.
    if (cfg.cancel.stop_requested()) {
      return Err(ErrorCode::Cancelled, "run_backtest: cancelled before step " +
                                           std::to_string(i) + " (" + refs[i].date + ")");
    }
    auto shifted_res = snapshot_cache->load(refs[i].archive_path, cfg.query_pricing_tier,
                                            cfg.query_cache_build_policy);
    if (!shifted_res) {
      return Err(shifted_res.error());
    }
    std::shared_ptr<const MarketSnapshot> shifted = std::move(*shifted_res);
    ATX_TRY_VOID(validate_snapshot_provenance(*shifted, cfg.surface_provenance_policy));
    ATX_TRY_VOID(validate_step_ordering(*base, *shifted, refs[i - 1].date, refs[i].date));
    if (cfg.prefetch_snapshots) {
      // The window is [i+1, i+depth]; step i-1 already covered through i+depth-1,
      // so exactly one ref newly enters it here.
      ATX_TRY_VOID(prefetch_window(*snapshot_cache, refs, i + prefetch_depth, 1u, cfg));
    }

    // V1 solve ledger: per-step solve deltas when a StepTrace is armed (fixed-book
    // overload has no execute/entry work — just compute_step). Zero cost otherwise.
    counters::ledger::StepScope step_ledger_scope;

    // Partition + Taylor PnL-explain: byte-identical arithmetic to the strategy
    // overload's step (shared `compute_step`), which now also reports the count of
    // held lots the pricer could not value this step.
    auto step = compute_step(*base, *shifted, book.lots, cfg.price, retained_pricer,
                             /*target_marks=*/nullptr, &settle_pricer, &settle_frame, &mark_memo,
                             cfg.settlement_mark_memo, deferrals_ptr);
    if (!step) {
      return Err(step.error());
    }
    if (cfg.unpriced == UnpricedLotPolicy::Error && step->n_unpriced > 0) {
      return Err(ErrorCode::NotFound,
                 unpriced_error_message(step->n_unpriced, step->first_unpriced_uid));
    }
    const PnlTotals &t = step->totals;
    const double settlement = step->settlement;

    const double step_total = t.pnl_total + settlement;
    nav += step_total;                        // cumulative every step, regardless of recording
    out.step_pnl_total.push_back(step_total); // full-res per-step series (metrics)

    // Accrue this step's flow into the pending block (flushed on the next record).
    b_total += step_total;
    b_delta += t.pnl_delta;
    b_gamma += t.pnl_gamma;
    b_vega += t.pnl_vega;
    b_vanna += t.pnl_vanna;
    b_volga += t.pnl_volga;
    b_theta += t.pnl_theta;
    b_rho += t.pnl_rho;
    b_charm += t.pnl_charm;
    b_unexpl += t.pnl_unexplained;
    b_settle += settlement;
    // Deferred settlement events join the row's exclusion count (F1(b) precedent).
    b_nunpriced += static_cast<double>(step->n_unpriced) + static_cast<double>(step->n_deferred);

    // Adopt the shifted snapshot as the next base (no reload) and drop expiries.
    base = std::move(shifted);
    std::erase_if(book.lots, [&base](const Lot &l) { return l.expiry_ts_ns <= base->ts_ns(); });

    const bool is_last = (i + 1 == refs.size());
    const bool record = ((i % stride) == 0) || is_last;
    if (record) {
      // L2: repopulate the memo for the new base date so the NEXT step's settlement
      // reads from it. (On a non-recorded step the memo is left stale; the surface-
      // instance guard then forces settlement to solve — fail-closed.)
      auto g = book_greeks(*base, book.lots, cfg.price, retained_pricer, &mark_memo);
      if (!g) {
        return Err(g.error());
      }
      if (cfg.unpriced == UnpricedLotPolicy::Error && g->n_unpriced > 0) {
        return Err(ErrorCode::NotFound, unpriced_greeks_error_message(
                                            g->n_unpriced, g->first_unpriced_uid, refs[i].date));
      }
      // A deferred lot is OPEN — it holds real exposure the run never settled — so
      // it counts toward n_open_lots even though it has left `book.lots` (it is
      // past its expiry, which every book-facing invariant and pricer forbids).
      push_row(refs[i].date, base->ts_ns(), b_total, b_delta, b_gamma, b_vega, b_vanna, b_volga,
               b_theta, b_rho, b_charm, b_unexpl, b_settle, nav, g->total,
               book.lots.size() + deferrals.size(), b_nunpriced,
               static_cast<double>(g->n_unpriced));
      b_total = b_delta = b_gamma = b_vega = b_vanna = b_volga = 0.0;
      b_theta = b_rho = b_charm = b_unexpl = b_settle = b_nunpriced = 0.0;
    }
  }

  // Plan 4.6: the engine may not emit a skewed column set. Pure read, so this
  // cannot change a value the run produced.
  ATX_TRY_VOID(out.validate());
  return Ok(std::move(out));
}

// One step's realized trade accounting (frictions + turnover) computed by the
// engine executor over the book diff and the hedge overlay.
struct ExecResult {
  double cost{0.0};
  double turnover_notional{0.0};
  double turnover_vega{0.0};
  std::optional<BookGreeks> book_greeks{};
  // ExcludeAndReport only: hedge fills the overlay skipped because the uid has no
  // board on this base. Joins the row's exclusion tally (F1(b) precedent).
  std::uint32_t n_unpriced_hedges{0};
};

[[nodiscard]] static Result<BacktestContinuation>
run_backtest_strategy_impl(const Clock &clock, IStrategy &strat, const RunConfig &cfg,
                           const BacktestCheckpoint *resume, bool capture_checkpoint) {
  ATX_VOL_PROFILE_SCOPE(BacktestTotal);
  ATX_TRY_VOID(validate_run_config(cfg));
  ATX_TRY_VOID(validate_run_query_route(cfg));
  const std::span<const SnapshotRef> refs = clock.refs();
  if (refs.empty()) {
    return Err(ErrorCode::InvalidArgument, "run_backtest: empty clock");
  }
  if (resume != nullptr) {
    ATX_TRY_VOID(validate_checkpoint(*resume, refs.size()));
  }
  const QueryExecution required_execution = strat.required_economic_execution();
  if (!valid_query_execution(required_execution)) {
    return Err(ErrorCode::InvalidArgument,
               "run_backtest: strategy returned invalid required economic execution");
  }
  const bool prepared_fast = cfg.query_pricing_tier == QueryPricingTier::RepresentativeFast ||
                             cfg.query_pricing_tier == QueryPricingTier::CarryBank;
  if (required_execution == QueryExecution::ColdReference && prepared_fast &&
      cfg.price.query_execution != QueryExecution::ColdReference) {
    return Err(ErrorCode::InvalidArgument,
               "run_backtest: strategy requires ColdReference economics for the configured "
               "fast query tier");
  }
  const std::size_t stride = cfg.record_every_n;
  const std::size_t global_step_base =
      resume != nullptr ? resume->completed_step_index : std::size_t{0};

  BacktestResult out;
  PortfolioState book = resume != nullptr ? resume->portfolio : PortfolioState{};
  std::uint64_t next_id =
      resume != nullptr ? resume->next_lot_id : std::uint64_t{1}; // monotonic strategy lot ids
  ReusablePriceFrame risk_frame;
  ReusableTargetMarkFrame target_marks;
  RetainedBookPricer retained_pricer;
  RetainedBookPricer settle_pricer; // B2: retained batched-settlement pricer
  ReusablePriceFrame settle_frame;  // B2: retained Marks frame for settlement
  StepMarkMemo mark_memo;           // L2: per-step settlement-mark memo
  ReusableLotIdIndex before_lot_index;
  ReusableLotIdIndex after_lot_index;
  std::vector<Lot> before_lots;
  std::vector<SwapLot> before_swap_lots;

  // ── Engine-internal cash + per-uid share ledger (B2/B3) ────────────────────
  double cash = resume != nullptr ? resume->cash : cfg.financing.initial_cash;
  // B3: O(1) get/add/sum + allocation-free daily hedge pass (was a linear-scan
  // `shares` vector + a per-uid whole-frame delta rescan). Bit-identical output.
  HedgeLedger hedge_ledger;
  if (resume != nullptr) {
    for (const HedgeSharePosition &position : resume->hedge_shares) {
      hedge_ledger.add(position.uid, position.shares);
    }
  }
  // Swap-lane fixing/accrual state, one entry per seeded swap lot. Restored
  // verbatim on a resume so the first resumed mark reads the same running
  // variance and the same prior mark the uninterrupted run would have.
  std::vector<SwapAccrual> swap_accruals =
      resume != nullptr ? resume->swap_accruals : std::vector<SwapAccrual>{};
  const DerivConfig swap_price_cfg{}; // v1: the default deriv pricing config

  // Per-share execution half-spread under the friction model: the selected
  // `spread_kind` lane (0 when None) PLUS the C-4 impact lane. The two are
  // separate components because they scale on different quantities — the
  // vol-tick lane on vega, the impact on the mark — so neither is expressible
  // inside the other. `impact_fraction == 0.0` (the default) adds exactly 0.0
  // and is therefore bit-identical to the pre-C-4 engine.
  const auto half_spread = [&cfg](double mark, double vega) -> double {
    const auto spread = [&]() -> double {
      switch (cfg.frictions.spread_kind) {
      case FrictionModel::SpreadKind::None:
        return 0.0;
      case FrictionModel::SpreadKind::PriceBps:
        return mark * (cfg.frictions.half_spread_bps / 1.0e4);
      case FrictionModel::SpreadKind::VolTicks:
        return vega * cfg.frictions.vol_tick;
      }
      return 0.0;
    }();
    return spread + mark * cfg.frictions.impact_fraction;
  };

  const auto push_row =
      [&out, &cfg](const std::string &date, std::int64_t ts, double p_total, double p_delta,
             double p_gamma, double p_vega, double p_vanna, double p_volga, double p_theta,
             double p_rho, double p_charm, double p_unexpl, double p_settle, double p_shares,
             double p_fin, double p_cost, double nav_v, double cash_v, double g_delta,
             const PriceTotals &g, double turn_notl, double turn_vega, double swap_pv_v,
             double swap_pnl_v, const SwapStepResult &ex, std::size_t n_lots,
             double n_unpriced, double n_unpriced_greeks) {
        out.date.push_back(date);
        out.ts_ns.push_back(ts);
        out.pnl_total.push_back(p_total);
        out.pnl_delta.push_back(p_delta);
        out.pnl_gamma.push_back(p_gamma);
        out.pnl_vega.push_back(p_vega);
        out.pnl_vanna.push_back(p_vanna);
        out.pnl_volga.push_back(p_volga);
        out.pnl_theta.push_back(p_theta);
        out.pnl_rho.push_back(p_rho);
        out.pnl_charm.push_back(p_charm);
        out.pnl_unexplained.push_back(p_unexpl);
        out.pnl_settlement.push_back(p_settle);
        out.pnl_shares.push_back(p_shares);
        out.financing.push_back(p_fin);
        out.cost.push_back(p_cost);
        out.nav.push_back(nav_v);
        out.cash.push_back(cash_v);
        out.gross_delta.push_back(g_delta); // NET book delta = option delta + hedge shares
        out.gross_gamma.push_back(g.gamma);
        out.gross_vega.push_back(g.vega);         // NET (signed); see backtest.hpp
        out.gross_vega_abs.push_back(g.abs_vega); // C-3: the genuinely GROSS series
        out.gross_theta.push_back(g.theta);
        out.turnover_notional.push_back(turn_notl);
        out.turnover_vega.push_back(turn_vega);
        out.swap_pv.push_back(swap_pv_v);   // STATE: live swap marks at this row
        out.swap_pnl.push_back(swap_pnl_v); // FLOW: block-summed like every other
        if (cfg.swap_pnl_explain) {
          // Every one a FLOW, pushed from the same block accumulator `swap_pnl_v`
          // came from, so the identity survives `record_every_n > 1`. Driven off
          // the roster and `SwapStepResult`'s parallel accessor so a ninth column
          // cannot reach `validate()` and miss this loop.
          const std::span<const BacktestExplainColumn> roster = swap_explain_columns();
          for (std::size_t c = 0; c < roster.size(); ++c) {
            (out.*(roster[c].member)).push_back(ex.ex[c]);
          }
        }
        out.n_open_lots.push_back(static_cast<double>(n_lots));
        out.n_unpriced_lots.push_back(n_unpriced);
        out.n_unpriced_greeks.push_back(n_unpriced_greeks);
      };

  // ── F1(d) NAV-vs-liquidation reconciliation (RunConfig::reconcile_nav) ──────
  //
  // NAV is a cumulative flow sum. This recomputes the book's liquidation value
  // from an INDEPENDENT set of inputs — the cash ledger, the row's repriced book
  // PV, the share ledger marked at this row's spots — and anchors it so it is
  // directly comparable to NAV (which starts at 0 while liquidation starts at
  // `initial_cash`). The only term that is not directly observable in cash or a
  // mark is financing that accrued to NAV without moving cash (borrow + shares
  // carry), so it is carried as a running total.
  //
  // Every trade the engine books is liquidation-NEUTRAL by construction (a hedge
  // moves cash and share MTM by the same notional; a roll-close moves cash and
  // book PV by the same mark; an entry at its own model mark likewise), so any
  // deviation is a genuine accounting leak: excluded held-lot PnL, a share
  // position that went unmarked, a settlement whose surface vanished, or a fill
  // priced away from the mark the book is carried at.
  double financing_noncash_total = resume != nullptr ? resume->cumulative_noncash_financing : 0.0;
  // Σ of the LIVE swap lots' qty-scaled marks after the most recent swap pass —
  // this row's `swap_pv` column, and the swap lane's liquidation term. Exactly
  // 0.0 (and therefore liquidation-neutral) for a book with no swap lots.
  double swap_pv_state = 0.0;
  const auto liquidation_value = [&](const MarketSnapshot &snap,
                                     const PriceTotals &book_totals) -> double {
    double shares_mtm = 0.0;
    for (const auto &[uid, n] : hedge_ledger.entries()) {
      const SurfaceRef s = snap.find(uid);
      if (s == nullptr) {
        continue; // unvaluable; shows up as drift, which is the point
      }
      shares_mtm += n * s->pricing().S;
    }
    // A swap mark enters NAV as (mark - prev mark) and its settlement as
    // (payoff - prev mark) with `payoff` also paid into cash, so carrying the
    // live marks here closes the identity on both events.
    return (cash - cfg.financing.initial_cash) + book_totals.pv + shares_mtm + swap_pv_state +
           financing_noncash_total;
  };
  const auto reconcile_row = [&](const std::string &date, double nav_v,
                                 const PriceTotals &book_totals,
                                 const MarketSnapshot &snap) -> Status {
    if (!cfg.reconcile_nav) {
      return Ok();
    }
    const double liq = liquidation_value(snap, book_totals);
    out.nav_liquidation.push_back(liq);
    const double drift = liq - nav_v;
    if (!(std::fabs(drift) <= cfg.reconcile_nav_tol)) {
      return Err(ErrorCode::Internal, "run_backtest: NAV reconciliation failed on " + date +
                                          " (nav=" + std::to_string(nav_v) + ", liquidation=" +
                                          std::to_string(liq) + ", drift=" + std::to_string(drift) +
                                          ", tol=" + std::to_string(cfg.reconcile_nav_tol) + ")");
    }
    return Ok();
  };

  // Signal series: names captured on the first recorded row, then one value per
  // recorded row per series (NaN when a name is absent that row).
  bool sig_init = false;
  const auto record_signals = [&out, &strat, &sig_init](const MarketSnapshot &snap) {
    ATX_VOL_PROFILE_SCOPE(Signals);
    const std::vector<std::pair<std::string, double>> s = strat.signals(snap);
    if (!sig_init) {
      for (const auto &kv : s) {
        out.signals.emplace_back(kv.first, std::vector<double>{});
      }
      sig_init = true;
    }
    for (auto &series : out.signals) {
      double v = std::numeric_limits<double>::quiet_NaN();
      for (const auto &kv : s) {
        if (kv.first == series.first) {
          v = kv.second;
          break;
        }
      }
      series.second.push_back(v);
    }
  };

  // Book this step's trades (entries + roll-closes diffed against `before_lots`,
  // then the DeltaToZero hedge overlay), updating `cash`/`shares` and returning the
  // step's realized friction `cost` + option turnover. Frictionless + hedge-None
  // ⇒ cost/turnover 0 and cash/shares untouched.
  const auto execute = [&](const MarketSnapshot &base_snap, const std::vector<Lot> &before_lots,
                           const HedgeSpec &hedge_spec,
                           const ReusableTargetMarkFrame *close_marks) -> Result<ExecResult> {
    ATX_VOL_PROFILE_SCOPE(Execution);
    ExecResult ex;
    bool entry_happened = false;

    for (const Lot &lot : book.lots) {
      if (!before_lot_index.find(before_lots, lot.id).has_value()) {
        entry_happened = true;
        break;
      }
    }
    const bool hedge_fires =
        hedge_spec.kind == HedgeSpec::Kind::DeltaToZero &&
        ((hedge_spec.cadence == HedgeSpec::Cadence::Daily) ||
         (hedge_spec.cadence == HedgeSpec::Cadence::AtEntry && entry_happened));

    // One full-book price supplies entry vega, per-uid hedge delta, and the row
    // Greek totals. This replaces N per-uid portfolios plus the later row pass.
    const PriceFrame *current_risk = nullptr;
    if (entry_happened || hedge_fires) {
      ATX_TRY(PortfolioPricer * pricer, retained_pricer.prepare(book.lots, base_snap.ts_ns()));
      risk_frame.resize(pricer->portfolio().n_positions());
      const std::span<const FullGreekSeed> entry_seeds =
          entry_happened ? strat.entry_risk_seeds() : std::span<const FullGreekSeed>{};
      ATX_TRY_VOID(pricer->price_into(base_snap.set(), PriceFieldMask::FullGreeks,
                                      risk_frame.view(), retained_pricer.workspace(), cfg.price,
                                      entry_seeds));
      ex.book_greeks = summarize_price_frame(risk_frame.frame);
      current_risk = &risk_frame.frame;
    }

    // Entry trades: lots present now but absent from `before_lots`.
    for (std::size_t lot_index = 0; lot_index < book.lots.size(); ++lot_index) {
      const Lot &lot = book.lots[lot_index];
      if (before_lot_index.find(before_lots, lot.id).has_value()) {
        continue;
      }
      ATX_VOL_PROFILE_SCOPE(EntryRisk);
      double vega = 0.0;
      if (current_risk != nullptr &&
          (lot_index >= current_risk->size() || current_risk->id[lot_index] != lot.id)) {
        return Err(ErrorCode::Internal,
                   "run_backtest: entry-risk frame is not aligned with lot id=" +
                       std::to_string(lot.id));
      }
      if (current_risk != nullptr && current_risk->status[lot_index] == PriceStatus::Ok) {
        const double weight = lot.qty * lot.multiplier;
        if (weight != 0.0) {
          vega = current_risk->vega[lot_index] / weight;
        }
      }
      const double mark = lot.entry_price; // the FILL price the strategy chose
      // F2: the price the BOOK is carried at this row. Identical to the fill
      // unless the caller opted into fill-slippage accounting, which makes the
      // (fill - mark) gap a realized cost instead of an invisible one.
      double model_mark = mark;
      if (cfg.book_entry_fill_slippage) {
        if (current_risk == nullptr || current_risk->status[lot_index] != PriceStatus::Ok) {
          return Err(
              ErrorCode::NotFound,
              "run_backtest: no model mark to price entry fill slippage against for lot id=" +
                  std::to_string(lot.id) + " uid=" + std::to_string(lot.contract.uid));
        }
        model_mark = current_risk->price[lot_index];
      }
      const double hs = half_spread(mark, vega);
      // Signed: positive whenever the fill is worse than the mark (buying above
      // it, selling below it), negative on genuine price improvement.
      const double fill_slippage = lot.qty * lot.multiplier * (mark - model_mark);
      const double leg_cost = std::fabs(lot.qty) * lot.multiplier * hs +
                              cfg.frictions.per_contract_cost * std::fabs(lot.qty) + fill_slippage;
      ex.cost += leg_cost;
      // Premium at the CARRY mark; the fill/mark gap rides in `leg_cost`, and
      // `cash -= ex.cost` below completes it — so cash still moves by exactly
      // qty*multiplier*fill, and NAV now sees the gap too.
      cash -= lot.qty * lot.multiplier * model_mark;
      ex.turnover_notional += std::fabs(lot.qty * lot.multiplier * mark);
      ex.turnover_vega += std::fabs(lot.qty * lot.multiplier * vega);
    }

    // Roll-close trades: lots in `before_lots` gone now (expiries were already
    // settled + erased before on_step, so these are strategy-driven closes).
    for (const Lot &lot : before_lots) {
      if (after_lot_index.find(book.lots, lot.id).has_value()) {
        continue;
      }
      const double T_res = residual_T(lot.expiry_ts_ns, base_snap.ts_ns());
      const SurfaceRef s = base_snap.find(lot.contract.uid);
      const std::optional<ReusableTargetMarkFrame::Match> exact_mark =
          close_marks != nullptr ? close_marks->find_ok(lot.id) : std::nullopt;
      double mark = exact_mark.has_value() ? exact_mark->raw_mark : 0.0;
      // None/PriceBps do not need target-date vega for friction. Report the P&L
      // base bundle's unbounded one-step-lag approximation in turnover telemetry
      // only; it never enters friction, cash, or NAV. VolTicks replaces it below
      // with exact configured target-date vega.
      double vega = exact_mark.has_value() ? exact_mark->base_vega_proxy : 0.0;
      if (cfg.frictions.spread_kind == FrictionModel::SpreadKind::VolTicks) {
        if (s == nullptr) {
          return Err(ErrorCode::NotFound,
                     "run_backtest: no surface for roll-close lot id=" + std::to_string(lot.id) +
                         " uid=" + std::to_string(lot.contract.uid));
        }
        // L4 note: this per-lot roll-close friction bundle consumes ONLY `price` and
        // `vega` (below) and is NOT reused by any later P&L, so it is a genuine
        // first-order-tier site — `greeks_analytic(..., GreekNeeds{vega=true,rho=false,
        // charm=false})` would drop it 5 -> 3 boundary solves, price/vega BIT-IDENTICAL.
        // Left at the full bundle here because the VolTicks roll-close solve-count pin
        // lives in backtest_test.cpp (Backtest.VolTicksRollCloseUsesConfiguredFdOr-
        // AnalyticGreekRoute, BoundarySolves==32) — a non-loop-owned test whose pin a
        // narrowing would move (32 -> 28); flipping it needs a PM license (loop-stage3).
        const Result<AmericanGreeks> risk =
            cfg.price.analytic_greeks
                ? s->greeks_analytic(lot.contract.K, T_res, lot.contract.side,
                                     cfg.price.query_execution)
                : s->greeks(lot.contract.K, T_res, lot.contract.side, cfg.price.query_execution);
        if (!risk) {
          return Err(risk.error());
        }
        if (!exact_mark.has_value()) {
          mark = risk->price;
        }
        vega = risk->vega;
      } else if (!exact_mark.has_value()) {
        if (s == nullptr) {
          return Err(ErrorCode::NotFound,
                     "run_backtest: no surface for roll-close lot id=" + std::to_string(lot.id) +
                         " uid=" + std::to_string(lot.contract.uid));
        }
        const Result<double> fallback =
            s->fair_value(lot.contract.K, T_res, lot.contract.side, cfg.price.query_execution);
        if (!fallback) {
          return Err(fallback.error());
        }
        mark = *fallback;
      }
      const double hs = half_spread(mark, vega);
      const double leg_cost = std::fabs(lot.qty) * lot.multiplier * hs +
                              cfg.frictions.per_contract_cost * std::fabs(lot.qty);
      ex.cost += leg_cost;
      cash += lot.qty * lot.multiplier * mark; // proceeds from closing
      ex.turnover_notional += std::fabs(lot.qty * lot.multiplier * mark);
      ex.turnover_vega += std::fabs(lot.qty * lot.multiplier * vega);
    }

    // Hedge overlay (B3): O(book) single-pass per-uid delta aggregation + O(1)
    // ledger, allocation-free after warm-up. `current_risk` is the full-book frame
    // priced above (non-null whenever hedge_fires). Bit-identical to the pre-B3
    // per-uid whole-frame rescan (see HedgeLedger::hedge_daily).
    if (hedge_fires && current_risk != nullptr) {
      ATX_VOL_PROFILE_SCOPE(HedgeRisk);
      // F1(a): no surface for a uid the hedge wants to trade => hard error. The
      // pre-F1 lambda returned 0.0 here and the pass filled at spot 0.0.
      //
      // SP100 task 2: under ExcludeAndReport that uid's fill is skipped instead —
      // its share position is LEFT IN PLACE (unhedged and still marked-to-nothing,
      // which the row reports) rather than flattened for free. Error is untouched.
      ATX_TRY_VOID(hedge_ledger.hedge_daily(
          book.lots, *current_risk, hedge_spec.band, cfg.frictions.hedge_slippage_bps,
          [&base_snap, &cfg](std::uint32_t uid) -> Result<std::optional<double>> {
            const SurfaceRef surface = base_snap.find(uid);
            if (surface == nullptr) {
              if (cfg.unpriced == UnpricedLotPolicy::Error) {
                return Err(ErrorCode::NotFound,
                           "run_backtest: no surface for delta hedge on uid=" +
                               std::to_string(uid) + " (share fill would price at spot 0.0)");
              }
              return Ok(std::optional<double>{});
            }
            return Ok(std::optional<double>{surface->pricing().S});
          },
          cash, ex.cost, ex.n_unpriced_hedges));
    }

    cash -= ex.cost; // realized frictions hit cash at fill
    return Ok(ex);
  };

  // WS-ZC1: a backtest replays a SEALED corpus, and only the PRIVATE cache may be
  // sealed — see the note on the fixed-book overload above.
  //
  // WS-F F5 (BT-T2): the strategy overload used to build its private cache with
  // NO referenced set, on the reasoning that "on_step names are not known up
  // front". That is false for a schedule-driven strategy — the schedule
  // enumerates every uid it will ever touch — so a replay against a wide archive
  // reconstructed the whole board on every date to price ~11 names.
  // `IStrategy::referenced_uids()` lets a strategy that CAN answer say so; the
  // empty default keeps the whole-board load for every strategy that cannot.
  // Only the PRIVATE cache may be subsetted: a caller-supplied cache can be
  // reused across books whose referenced sets differ, and a subset snapshot
  // cached under one book would be missing another's uids.
  const std::span<const std::uint32_t> strategy_uids = strat.referenced_uids();
  const std::size_t prefetch_depth = effective_prefetch_depth(cfg);
  const std::size_t private_capacity = private_snapshot_cache_capacity(prefetch_depth);
  const std::shared_ptr<SnapshotCache> snapshot_cache =
      cfg.snapshot_cache ? cfg.snapshot_cache
      : strategy_uids.empty()
          ? std::make_shared<SnapshotCache>(private_capacity, ArchiveBacking::Sealed)
          : std::make_shared<SnapshotCache>(
                private_capacity,
                std::vector<std::uint32_t>(strategy_uids.begin(), strategy_uids.end()),
                ArchiveBacking::Sealed);
  auto base_res = snapshot_cache->load(refs[0].archive_path, cfg.query_pricing_tier,
                                       cfg.query_cache_build_policy);
  if (!base_res) {
    return Err(base_res.error());
  }
  std::shared_ptr<const MarketSnapshot> base = std::move(*base_res);
  ATX_TRY_VOID(validate_snapshot_provenance(*base, cfg.surface_provenance_policy));
  if (resume != nullptr && base->ts_ns() != resume->base_ts_ns) {
    return Err(ErrorCode::InvalidArgument,
               "run_backtest_incremental: clock anchor timestamp does not match checkpoint "
               "(clock=" +
                   std::to_string(base->ts_ns()) +
                   ", checkpoint=" + std::to_string(resume->base_ts_ns) + ")");
  }
  if (cfg.prefetch_snapshots) {
    // Initial fill of the whole look-ahead window; each later step adds only the
    // one ref that newly enters it.
    ATX_TRY_VOID(prefetch_window(*snapshot_cache, refs, 1u, prefetch_depth, cfg));
  }

  double nav = resume != nullptr ? resume->nav : 0.0;

  // Inception (row 0): open positions AS OF refs[0], book entry frictions + premium
  // + the opening hedge into cash; PnL columns are zero; record post-trade cash.
  if (resume == nullptr) {
    const std::uint64_t next_id_before = next_id;
    Status st = [&]() {
      ATX_VOL_PROFILE_SCOPE(StrategyStep);
      return strat.on_step(*base, 0, book, next_id, cfg.price);
    }();
    if (!st) {
      return Err(st.error());
    }
    // L10 observation point: NOTHING may sit between on_step and this call. The
    // shadow replay loop this hook replaces read strategy state with nothing in
    // between, so only this position is definitionally equivalent to it.
    if (cfg.step_observer) {
      ATX_TRY_VOID(cfg.step_observer(StepEvent{0, refs[0], *base, strat}));
    }
    before_lots.clear(); // empty => every opened lot is a fresh entry
    ATX_TRY_VOID(validate_strategy_transition(before_lots, book.lots, {}, book.swap_lots,
                                              next_id_before, next_id, base->ts_ns(),
                                              before_lot_index, after_lot_index));
    const HedgeSpec hedge_spec = strat.hedge_spec();
    ATX_TRY_VOID(validate_hedge_spec(hedge_spec));
    auto ex = execute(*base, before_lots, hedge_spec, nullptr);
    if (!ex) {
      return Err(ex.error());
    }
    Result<BookGreeks> g = ex->book_greeks.has_value() ? Ok(*ex->book_greeks)
                                                       : book_greeks(*base, book.lots, cfg.price,
                                                                     retained_pricer, &mark_memo);
    if (!g) {
      return Err(g.error());
    }
    // Inception book greeks are a real measurement (the strategy has already opened
    // its entries): under the Error policy an unpriced held lot here aborts, exactly
    // as a later row would. The strategy never opens a lot in an absent name, so this
    // is 0 for a normally-opened basket and never fires on an empty book.
    if (cfg.unpriced == UnpricedLotPolicy::Error && g->n_unpriced > 0) {
      return Err(ErrorCode::NotFound,
                 unpriced_greeks_error_message(g->n_unpriced, g->first_unpriced_uid, refs[0].date));
    }
    const double g_delta = g->total.delta + hedge_ledger.sum();
    // Opening fills are the first economic event of the run. execute() already
    // deducted their friction from cash; stamp the same loss into row-0 PnL/NAV
    // so total return and attribution include every paid dollar.
    nav = -ex->cost;
    // Swap columns are 0.0 at inception: the swap pass lives in the step loop,
    // so a lot opened here has neither seeded a fixing nor taken a mark yet
    // (entry economics v1 — a swap opens at zero cost).
    push_row(refs[0].date, base->ts_ns(), nav, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0,
             0.0, 0.0, ex->cost, nav, cash, g_delta, g->total, ex->turnover_notional,
             ex->turnover_vega, 0.0, 0.0, SwapStepResult{}, book.lots.size(),
             static_cast<double>(ex->n_unpriced_hedges), static_cast<double>(g->n_unpriced));
    ATX_TRY_VOID(reconcile_row(refs[0].date, nav, g->total, *base));
    record_signals(*base);
  }

  // Block accumulators for record_every_n>1: sum each non-recorded step's flow and
  // flush into the next recorded row (see fixed-book overload). `nav`/`cash` and the
  // book greeks stay AT the recorded row; every additive flow column is block-summed.
  // At stride 1 each block is one step, reproducing the per-step columns bit-for-bit.
  double b_total = 0.0, b_delta = 0.0, b_gamma = 0.0, b_vega = 0.0, b_vanna = 0.0, b_volga = 0.0;
  double b_theta = 0.0, b_rho = 0.0, b_charm = 0.0, b_unexpl = 0.0, b_settle = 0.0;
  double b_shares = 0.0, b_fin = 0.0, b_cost = 0.0, b_turn_notl = 0.0, b_turn_vega = 0.0;
  double b_nunpriced = 0.0, b_swap_pnl = 0.0;
  SwapStepResult b_explain{}; // Task F-8: the explain's own block accumulator

  // Deferred expiry settlements (ExcludeAndReport only); see the fixed-book loop.
  DeferredSettlementBook deferrals;
  DeferredSettlementBook *const deferrals_ptr =
      cfg.unpriced == UnpricedLotPolicy::ExcludeAndReport ? &deferrals : nullptr;

  for (std::size_t i = 1; i < refs.size(); ++i) {
    // Plan 5.5 safe point: the TOP of the step, before this step's snapshot load
    // and before any row is appended. `out` is abandoned along with the error, so
    // no partially-filled result can reach a caller, a writer, or validate() —
    // the run returns no BacktestResult at all. Neither run_backtest overload
    // performs any file I/O, so nothing on disk is touched either way.
    if (cfg.cancel.stop_requested()) {
      return Err(ErrorCode::Cancelled, "run_backtest: cancelled before step " +
                                           std::to_string(i) + " (" + refs[i].date + ")");
    }
    const std::size_t global_step_index = global_step_base + i;
    auto shifted_res = snapshot_cache->load(refs[i].archive_path, cfg.query_pricing_tier,
                                            cfg.query_cache_build_policy);
    if (!shifted_res) {
      return Err(shifted_res.error());
    }
    std::shared_ptr<const MarketSnapshot> shifted = std::move(*shifted_res);
    ATX_TRY_VOID(validate_snapshot_provenance(*shifted, cfg.surface_provenance_policy));
    ATX_TRY_VOID(validate_step_ordering(*base, *shifted, refs[i - 1].date, refs[i].date));
    if (cfg.prefetch_snapshots) {
      // The window is [i+1, i+depth]; step i-1 already covered through i+depth-1,
      // so exactly one ref newly enters it here.
      ATX_TRY_VOID(prefetch_window(*snapshot_cache, refs, i + prefetch_depth, 1u, cfg));
    }

    // V1 solve ledger: record this step's per-unique solve deltas (pnl-base + target +
    // execute) when a StepTrace is armed. Spans to the end of the loop body, so it
    // captures execute()'s risk-frame solves too. Zero cost (two TLS-null checks) when
    // no trace is armed — never on the shipping hot path.
    counters::ledger::StepScope step_ledger_scope;

    // 1. PnL of the current book (resolved on base) forward to shifted (unchanged B1).
    auto step = compute_step(*base, *shifted, book.lots, cfg.price, retained_pricer, &target_marks,
                             &settle_pricer, &settle_frame, &mark_memo, cfg.settlement_mark_memo,
                             deferrals_ptr);
    if (!step) {
      return Err(step.error());
    }
    if (cfg.unpriced == UnpricedLotPolicy::Error && step->n_unpriced > 0) {
      return Err(ErrorCode::NotFound,
                 unpriced_error_message(step->n_unpriced, step->first_unpriced_uid));
    }
    const PnlTotals &t = step->totals;
    const double settlement = step->settlement;

    // 2. Shares PnL + financing over the step, from the ledger held over the step.
    const double dt =
        (static_cast<double>(shifted->ts_ns()) - static_cast<double>(base->ts_ns())) / kNsPerYear;
    double shares_pnl = 0.0;
    double financing = 0.0;
    // F1(d): financing credited to NAV but NOT to the cash ledger (borrow + shares
    // carry). The cash-carry leg below grows `cash` itself, so it must NOT be
    // accumulated here or the reconciliation would double-count it.
    double financing_noncash_step = 0.0;
    // F1(b): hedge-share ledger entries this step could not value (base or shifted
    // surface absent with a non-zero position).
    std::uint32_t n_unpriced_shares = 0;
    if (cfg.financing.finance_premium) {
      // Backing-agnostic (WS-ZC1): index 0 is the first archive-order surface whether
      // this snapshot owns its surfaces or borrows mapped views.
      //
      // A subset-miss load legally yields a snapshot owning ZERO surfaces, so there
      // may be no base-date rate to accrue at. Disposition follows the hedge-share
      // ledger's rule immediately below: a FLAT slot carries no economics — with
      // cash == 0.0 both `cash * (growth - 1.0)` and `cash *= growth` are exact
      // no-ops for every finite r, so skipping is bit-identical — while a LIVE
      // balance is a valuation failure. Silently skipping THAT would delete a step
      // of financing from NAV permanently (the flow is never recovered when the
      // board returns) and report it nowhere, so it fails closed.
      const SurfaceRef base_rate_surface = base->surface_at(0);
      if (base_rate_surface == nullptr) {
        if (cash != 0.0) {
          return Err(ErrorCode::NotFound,
                     "run_backtest: no base surface on " + refs[i - 1].date +
                         " to source the premium-financing rate (cash=" +
                         std::to_string(cash) + ")");
        }
      } else {
        const double r = base_rate_surface->pricing().r; // base-date rate
        const double growth = std::exp(r * dt);
        financing += cash * (growth - 1.0); // cash carry on the pre-step balance
        cash *= growth;                     // apply to the ledger
      }
    }
    for (const auto &[uid, n] : hedge_ledger.entries()) {
      const SurfaceRef bs = base->find(uid);
      const SurfaceRef ss = shifted->find(uid);
      if (bs == nullptr || ss == nullptr) {
        // F1(b) (BT-P1-3): shares held across a surface gap used to be skipped in
        // SILENCE — the position's move over the step vanished from NAV with no
        // count, no flag and no error, and reappeared as an unexplained level
        // shift when the surface came back. A flat (n == 0) ledger slot carries no
        // economics, so only a live position is a valuation failure.
        if (n != 0.0) {
          if (cfg.unpriced == UnpricedLotPolicy::Error) {
            return Err(ErrorCode::NotFound,
                       "run_backtest: hedge share position on uid=" + std::to_string(uid) +
                           " has no surface this step (shares=" + std::to_string(n) + ")");
          }
          ++n_unpriced_shares;
        }
        continue;
      }
      const double Sb = bs->pricing().S;
      shares_pnl += n * (ss->pricing().S - Sb);   // shares held over the step
      const double short_amt = std::max(0.0, -n); // |min(shares,0)|
      const double borrow = -cfg.financing.borrow_rate * short_amt * Sb * dt; // 0 when rate 0
      financing += borrow;
      financing_noncash_step += borrow;
      // F3(b): discrete dividend cash on every ex-date inside this step's
      // (base, shifted] window. Long shares receive it, short shares pay it.
      // This is a CASH event, not a modelled accrual, so it moves the ledger and
      // is deliberately NOT part of `financing_noncash_step`.
      bool uid_has_dividend_schedule = false;
      double dividend_cash = 0.0;
      for (const ShareDividend &div : cfg.financing.share_dividends) {
        if (div.uid != uid) {
          continue;
        }
        uid_has_dividend_schedule = true;
        if (div.ex_ts_ns > base->ts_ns() && div.ex_ts_ns <= shifted->ts_ns()) {
          dividend_cash += n * div.amount;
        }
      }
      if (dividend_cash != 0.0) {
        financing += dividend_cash;
        cash += dividend_cash;
      }
      if (cfg.financing.shares_carry) {
        // Buying shares has already reduced the financed cash balance, so cash
        // carry owns the funding cost when enabled. Charging r here too would
        // count it twice. Without cash financing, retain the standalone (q-r)
        // total-carry shortcut.
        const double funding_rate = cfg.financing.finance_premium ? 0.0 : bs->pricing().r;
        // F3(b): a uid with a real dividend schedule books the CASH above, so its
        // continuous yield proxy drops out (charging both double-counts). The
        // funding leg is unaffected. uids with no schedule keep the pre-F3
        // expression bit-for-bit.
        const double q_proxy = uid_has_dividend_schedule ? 0.0 : bs->q_eff_at(0.25);
        const double carry = n * (q_proxy - funding_rate) * Sb * dt;
        financing += carry;
        financing_noncash_step += carry;
      }
    }

    // 2b. Swap lane: fixings, marks and settlements against the shifted date.
    //     Runs BEFORE the move-swap while `shifted` is still named, and is a
    //     no-op — no pricing, no allocation, exactly 0.0 into every column — on
    //     a book with no swap lots.
    ATX_TRY(const SwapStepResult swap_step,
            step_swap_lots(*base, *shifted, book, swap_accruals, swap_price_cfg,
                           cfg.swap_pnl_explain));
    cash += swap_step.settlement_cash; // settled payoffs hit the ledger
    swap_pv_state = swap_step.swap_pv; // live marks after this pass

    // 3. Adopt shifted as the next base; settle expiries into cash; drop them.
    base = std::move(shifted);
    for (const Lot &lot : book.lots) {
      if (lot.expiry_ts_ns > base->ts_ns()) {
        continue;
      }
      const SurfaceRef bs = base->find(lot.contract.uid);
      if (bs == nullptr) {
        continue;
      }
      const double S = bs->pricing().S;
      const double K = lot.contract.K;
      const double intrinsic =
          (lot.contract.side == Side::Call) ? std::max(0.0, S - K) : std::max(0.0, K - S);
      cash += lot.qty * lot.multiplier * intrinsic; // intrinsic settle proceeds
    }
    // Deferred lots whose board came back this step settle at THIS step's spot;
    // `compute_step` summed their intrinsic in lot-id order. Their board was absent
    // when they expired, so the loop above can never have booked them twice: it
    // skips exactly the uids the deferral was triggered by. Guarded rather than
    // unconditional so the strict path does not even execute a `+= 0.0`, which
    // would map a -0.0 balance to +0.0: Error stays bit-identical.
    if (deferrals_ptr != nullptr) {
      cash += step->deferred_settle_cash;
    }
    std::erase_if(book.lots, [&base](const Lot &l) { return l.expiry_ts_ns <= base->ts_ns(); });

    // 4-5. Strategy entries/rolls + hedge overlay on the new base.
    before_lots.assign(book.lots.begin(), book.lots.end()); // survivors before on_step
    // Swap survivors likewise: settled lots left the book in the swap pass, so
    // anything still here must come back from on_step bit-identical.
    before_swap_lots.assign(book.swap_lots.begin(), book.swap_lots.end());
    const std::uint64_t next_id_before = next_id;
    Status st = [&]() {
      ATX_VOL_PROFILE_SCOPE(StrategyStep);
      return strat.on_step(*base, global_step_index, book, next_id, cfg.price);
    }();
    if (!st) {
      return Err(st.error());
    }
    // L10 observation point — see the inception call above. Fires on EVERY step,
    // independent of record_every_n; an Err aborts here, mid-step, so no partial
    // row is recorded.
    if (cfg.step_observer) {
      ATX_TRY_VOID(cfg.step_observer(StepEvent{global_step_index, refs[i], *base, strat}));
    }
    ATX_TRY_VOID(validate_strategy_transition(before_lots, book.lots, before_swap_lots,
                                              book.swap_lots, next_id_before, next_id,
                                              base->ts_ns(), before_lot_index, after_lot_index));
    const HedgeSpec hedge_spec = strat.hedge_spec();
    ATX_TRY_VOID(validate_hedge_spec(hedge_spec));
    auto ex = execute(*base, before_lots, hedge_spec, &target_marks);
    if (!ex) {
      return Err(ex.error());
    }

    // 6. Running NAV increment — EXACT add order; collapses to B1's
    //    (pnl_total + settlement) bit-for-bit when features are off.
    double step_total = t.pnl_total;
    step_total += settlement;
    step_total += shares_pnl;
    step_total += swap_step.swap_pnl; // exactly 0.0 without a swap lane
    step_total += financing;
    step_total -= ex->cost;
    nav += step_total;
    out.step_pnl_total.push_back(step_total); // full-res per-step series (metrics)

    // Accrue this step's flow into the pending block (flushed on the next record).
    b_total += step_total;
    b_delta += t.pnl_delta;
    b_gamma += t.pnl_gamma;
    b_vega += t.pnl_vega;
    b_vanna += t.pnl_vanna;
    b_volga += t.pnl_volga;
    b_theta += t.pnl_theta;
    b_rho += t.pnl_rho;
    b_charm += t.pnl_charm;
    b_unexpl += t.pnl_unexplained;
    b_settle += settlement;
    b_shares += shares_pnl;
    b_swap_pnl += swap_step.swap_pnl;
    for (std::size_t c = 0; c < kSwapExplainCount; ++c) {
      b_explain.ex[c] += swap_step.ex[c];
    }
    b_fin += financing;
    b_cost += ex->cost;
    b_turn_notl += ex->turnover_notional;
    b_turn_vega += ex->turnover_vega;
    // F1(b): unpriced hedge-share positions join the row's exclusion count, and
    // (SP100 task 2) so do skipped hedge fills and deferred settlement events.
    b_nunpriced += static_cast<double>(step->n_unpriced) + static_cast<double>(n_unpriced_shares) +
                   static_cast<double>(step->n_deferred) +
                   static_cast<double>(ex->n_unpriced_hedges);
    financing_noncash_total += financing_noncash_step;

    // 7. Record @ granularity: book greeks (net delta incl. shares) + B2 columns.
    const bool is_last = (i + 1 == refs.size());
    const bool record = ((i % stride) == 0) || is_last;
    if (record) {
      Result<BookGreeks> g = ex->book_greeks.has_value() ? Ok(*ex->book_greeks)
                                                         : book_greeks(*base, book.lots, cfg.price,
                                                                       retained_pricer, &mark_memo);
      if (!g) {
        return Err(g.error());
      }
      // The step-level Error guard above already fired for i>=1 whenever a held lot
      // is unpriced across this step (book_greeks under-counts only when this row's
      // surface is absent, which also breaks the step's pnl_explain), so this check
      // is a consistent belt-and-braces here; it is the sole guard only at inception.
      if (cfg.unpriced == UnpricedLotPolicy::Error && g->n_unpriced > 0) {
        return Err(ErrorCode::NotFound, unpriced_greeks_error_message(
                                            g->n_unpriced, g->first_unpriced_uid, refs[i].date));
      }
      const double g_delta = g->total.delta + hedge_ledger.sum();
      // Deferred lots are open exposure the run never settled; see the fixed-book
      // loop's note on why they cannot live in `book.lots`.
      push_row(refs[i].date, base->ts_ns(), b_total, b_delta, b_gamma, b_vega, b_vanna, b_volga,
               b_theta, b_rho, b_charm, b_unexpl, b_settle, b_shares, b_fin, b_cost, nav, cash,
               g_delta, g->total, b_turn_notl, b_turn_vega, swap_pv_state, b_swap_pnl, b_explain,
               book.lots.size() + deferrals.size(), b_nunpriced,
               static_cast<double>(g->n_unpriced));
      ATX_TRY_VOID(reconcile_row(refs[i].date, nav, g->total, *base));
      record_signals(*base);
      b_total = b_delta = b_gamma = b_vega = b_vanna = b_volga = 0.0;
      b_theta = b_rho = b_charm = b_unexpl = b_settle = 0.0;
      b_shares = b_fin = b_cost = b_turn_notl = b_turn_vega = b_nunpriced = 0.0;
      b_swap_pnl = 0.0;
      b_explain = SwapStepResult{};
    }
  }

  // Plan 4.6: the engine may not emit a skewed column set. Pure read, so this
  // cannot change a value the run produced.
  ATX_TRY_VOID(out.validate());
  BacktestCheckpoint checkpoint;
  if (capture_checkpoint) {
    // `BacktestCheckpoint` carries the book, the share ledger and the cash/NAV
    // scalars — it has no slot for a deferred settlement, and silently dropping one
    // would delete a real position's proceeds across the resume boundary. Fail
    // closed rather than lose it; a caller that hits this can finish the segment on
    // a clock whose last session has the missing board back.
    if (!deferrals.empty()) {
      return Err(ErrorCode::NotImplemented,
                 "run_backtest_incremental: " + std::to_string(deferrals.size()) +
                     " settlement(s) deferred by UnpricedLotPolicy::ExcludeAndReport cannot be "
                     "carried in a checkpoint");
    }
    checkpoint.base_ts_ns = base->ts_ns();
    checkpoint.completed_step_index = global_step_base + (refs.size() - 1u);
    checkpoint.next_lot_id = next_id;
    checkpoint.portfolio = std::move(book);
    checkpoint.hedge_shares.reserve(hedge_ledger.entries().size());
    for (const auto &[uid, shares] : hedge_ledger.entries()) {
      checkpoint.hedge_shares.push_back(HedgeSharePosition{uid, shares});
    }
    checkpoint.swap_accruals = std::move(swap_accruals);
    checkpoint.cash = cash;
    checkpoint.nav = nav;
    checkpoint.cumulative_noncash_financing = financing_noncash_total;
  }
  return Ok(BacktestContinuation{std::move(out), std::move(checkpoint)});
}

Result<BacktestResult> run_backtest(const Clock &clock, IStrategy &strat, const RunConfig &cfg) {
  auto continuation = run_backtest_strategy_impl(clock, strat, cfg, nullptr, false);
  if (!continuation) {
    return Err(continuation.error());
  }
  return Ok(std::move(continuation->rows));
}

Result<BacktestContinuation> run_backtest_incremental(const Clock &clock, IStrategy &strat,
                                                      const RunConfig &cfg,
                                                      const BacktestCheckpoint *resume) {
  if (cfg.record_every_n != 1u) {
    return Err(ErrorCode::InvalidArgument,
               "run_backtest_incremental: record_every_n must be 1 because checkpoint state "
               "does not persist pending stride blocks");
  }
  return run_backtest_strategy_impl(clock, strat, cfg, resume, true);
}

} // namespace atx::vol
