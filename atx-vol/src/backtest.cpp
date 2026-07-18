// atx-vol backtest engine (Phase B0) — see backtest.hpp for the model.

#include "atx/vol/backtest.hpp"

#include <algorithm>
#include <atomic>
#include <bit>
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
#include <utility>
#include <vector>

#include "atx/core/error.hpp"
#include "atx/vol/phase_profile.hpp"
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

// A forward-only run owns base, shifted, and at most one prefetched future.
// Retaining three cache entries covers that working set without accumulating one
// mapped archive per date. Caller-supplied caches remain reusable and unbounded.
constexpr std::size_t kPrivateSnapshotCacheCapacity = 3u;

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
      pricer_.emplace(std::move(portfolio));
      // PortfolioWorkspace is grow-only and validates its retained substrate by
      // logical book identity. Preserve its buffers across book changes instead
      // of destroying and reallocating the high-water mark.
      workspace_.reserve(pricer_->portfolio().n_contracts(), pricer_->portfolio().n_positions());
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
  // Steady-state allocation-free: the per-uid delta aggregate and the dedup set are
  // held as DENSE, generation-STAMPED vectors (not node-based unordered_map/set that
  // reallocate a node per key on every clear()+reinsert). A per-pass `generation_`
  // bump invalidates last pass's stamps in O(1) with no clears, so after warm-up
  // (every uid resident in `scratch_index_`) the pass performs no heap allocation.
  template <typename SpotOf>
  void hedge_daily(const std::vector<Lot> &lots, const PriceFrame &frame, double band,
                   double hedge_slippage_bps, SpotOf &&spot_of, double &cash, double &cost) {
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
        const double spot = spot_of(uid);
        cost += std::fabs(shares_to_trade) * spot * (hedge_slippage_bps / 1.0e4);
        cash -= shares_to_trade * spot;
        add(uid, shares_to_trade);
      }
    }
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

  std::vector<std::pair<std::uint32_t, double>> shares_;   // insertion order (traded uids)
  std::unordered_map<std::uint32_t, std::size_t> index_;   // uid -> slot in shares_

  // Per-step hedge scratch — dense, generation-stamped, allocation-free in steady state.
  std::unordered_map<std::uint32_t, std::size_t> scratch_index_; // uid -> dense scratch slot
  std::vector<double> agg_val_;         // per-uid summed option delta (valid iff agg_gen_==gen)
  std::vector<std::uint64_t> agg_gen_;  // stamp: agg_val_ valid this pass
  std::vector<std::uint64_t> seen_gen_; // stamp: uid already emitted into order_ this pass
  std::vector<std::uint32_t> order_;    // uid iteration order (dedup)
  std::uint64_t generation_{0};
};

[[nodiscard]] Status validate_strategy_transition(std::span<const Lot> before,
                                                  std::span<const Lot> after,
                                                  std::uint64_t next_id_before,
                                                  std::uint64_t next_id_after, std::int64_t base_ts,
                                                  ReusableLotIdIndex &before_index,
                                                  ReusableLotIdIndex &after_index) {
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
  return Ok();
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
};

// Price the current lots against `snap` at its residual T and report how many
// could not be valued. An empty book yields zero totals and n_unpriced 0 (an
// empty portfolio prices to an empty frame).
[[nodiscard]] Result<BookGreeks> book_greeks(const MarketSnapshot &snap,
                                             const std::vector<Lot> &lots, const PriceOptions &opts,
                                             RetainedBookPricer &retained) {
  ATX_VOL_PROFILE_SCOPE(BookGreeks);
  ATX_TRY(PortfolioPricer * pricer, retained.prepare(lots, snap.ts_ns()));
  PortfolioWorkspace &workspace = retained.workspace();
  auto totals = pricer->price_totals(snap.set(), PriceFieldMask::FullGreeks, workspace, opts);
  if (!totals) {
    return Err(totals.error());
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
};

[[nodiscard]] Result<StepPnl> compute_step(const MarketSnapshot &base,
                                           const MarketSnapshot &shifted,
                                           const std::vector<Lot> &lots, const PriceOptions &opts,
                                           RetainedBookPricer &retained,
                                           ReusableTargetMarkFrame *target_marks = nullptr) {
  ATX_VOL_PROFILE_SCOPE(StepPnl);
  std::vector<Lot> &alive = retained.reset_alive_scratch(lots.size());
  double settlement = 0.0;
  for (const Lot &lot : lots) {
    if (lot.expiry_ts_ns <= shifted.ts_ns()) {
      if (lot.expiry_ts_ns != shifted.ts_ns()) {
        return Err(
            ErrorCode::NotFound,
            "run_backtest: no exact expiry observation for lot id=" + std::to_string(lot.id) +
                " (expiry_ts_ns=" + std::to_string(lot.expiry_ts_ns) +
                ", next_snapshot_ts_ns=" + std::to_string(shifted.ts_ns()) + ")");
      }
      const double T_base = residual_T(lot.expiry_ts_ns, base.ts_ns());
      const PricedSurface *bs = base.find(lot.contract.uid);
      const PricedSurface *ss = shifted.find(lot.contract.uid);
      if (bs == nullptr || ss == nullptr) {
        return Err(ErrorCode::NotFound, "run_backtest: no surface for settling lot");
      }
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
  if (target_marks != nullptr) {
    ATX_TRY_VOID(target_marks->seal());
  }
  return Ok(StepPnl{t, settlement, n_unpriced, first_unpriced_uid});
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

// Validate after every cache load, not only while constructing a snapshot. That
// placement means a preloaded or previously cached compatibility snapshot cannot
// bypass a stricter policy selected by a later backtest run.
[[nodiscard]] Status validate_snapshot_provenance(const MarketSnapshot &snapshot,
                                                  SurfaceProvenancePolicy policy) {
  if (policy == SurfaceProvenancePolicy::Compatibility) {
    return Ok();
  }

  const std::span<const PricedSurface> surfaces = snapshot.surfaces();
  const std::span<const SurfaceProvenance> provenances = snapshot.provenances();
  if (provenances.size() != surfaces.size()) {
    return Err(ErrorCode::InvalidArgument,
               "run_backtest: snapshot provenance is not aligned with surfaces");
  }
  for (std::size_t index = 0; index < surfaces.size(); ++index) {
    const PricedSurface &surface = surfaces[index];
    const SurfaceProvenance &provenance = provenances[index];
    const bool admitted =
        !provenance.legacy_format && provenance.purpose == SurfacePurpose::Risk &&
        provenance.served_generation != 0u &&
        (provenance.state == SurfaceState::Healthy || provenance.state == SurfaceState::Degraded);
    if (!admitted) {
      const std::string uid = std::to_string(surface.uid());
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

// ── MarketSnapshot ──────────────────────────────────────────────────────────

MarketSnapshot::MarketSnapshot(std::vector<PricedSurface> &&surfaces,
                               std::vector<SurfaceProvenance> &&provenance, SurfaceSet &&set,
                               std::int64_t ts,
                               std::vector<std::pair<std::string, std::uint32_t>> &&syms) noexcept
    : surfaces_{std::move(surfaces)}, provenance_{std::move(provenance)}, set_{std::move(set)},
      ts_ns_{ts}, syms_{std::move(syms)} {}

std::uint64_t MarketSnapshot::open_count() noexcept { return g_open_count.load(); }
void MarketSnapshot::reset_open_count() noexcept { g_open_count.store(0); }

Result<MarketSnapshot> MarketSnapshot::load(std::string_view archive_path,
                                            QueryPricingTier query_pricing_tier) {
  ATX_VOL_PROFILE_SCOPE(SnapshotLoad);
  // S4 clean-break cutover: the whole board is deserialized from the v2 zero-copy
  // format. `reconstruct_all_with_provenance` rebuilds OWNED PricedSurfaces (kept
  // whole-board here on purpose) — the subset-map/PricedSurfaceView zero-copy win
  // reaching this loop is B1 (seam §6), not this format swap. The reconstructed
  // surfaces are bit-identical to what the old v1 reader produced.
  auto arch = [&]() {
    ATX_VOL_PROFILE_SCOPE(ArchiveOpen);
    return SurfaceArchiveV2::open_file(archive_path);
  }();
  if (!arch) {
    return Err(arch.error());
  }
  // One archive-open event (the load-once gate asserts N loads => N opens).
  g_open_count.fetch_add(1, std::memory_order_relaxed);

  auto mapped = [&]() {
    ATX_VOL_PROFILE_SCOPE(ArchiveMap);
    return arch->reconstruct_all_with_provenance();
  }();
  if (!mapped) {
    return Err(mapped.error());
  }
  std::vector<PricedSurface> surfaces;
  std::vector<SurfaceProvenance> provenance;
  surfaces.reserve(mapped->size());
  provenance.reserve(mapped->size());
  for (ArchivedSurface &record : *mapped) {
    surfaces.push_back(std::move(record.surface));
    provenance.push_back(std::move(record.provenance));
  }
  if (surfaces.empty()) {
    return Err(ErrorCode::InvalidArgument, "MarketSnapshot::load: archive holds no surfaces");
  }

  // Valuation timestamp: the surfaces of one date agree on now_ts_ns.
  const std::int64_t ts = surfaces.front().pricing().now_ts_ns;
  for (const PricedSurface &s : surfaces) {
    if (s.pricing().now_ts_ns != ts) {
      return Err(ErrorCode::InvalidArgument,
                 "MarketSnapshot::load: surfaces disagree on now_ts_ns within a date");
    }
  }

  // Query caches are runtime accelerators, not archive state. Prepare every
  // mapped surface under the caller's tier before building the pointer set, so
  // no partially-prepared snapshot can become observable.
  for (PricedSurface &surface : surfaces) {
    auto prepared = std::move(surface).with_query_pricing(query_pricing_tier);
    if (!prepared) {
      return Err(prepared.error());
    }
    surface = std::move(*prepared);
  }

  // Non-owning resolver over the owned surfaces' stable addresses.
  std::vector<const PricedSurface *> ptrs;
  ptrs.reserve(surfaces.size());
  for (const PricedSurface &s : surfaces) {
    ptrs.push_back(&s);
  }
  auto set = SurfaceSet::create(ptrs);
  if (!set) {
    return Err(set.error());
  }

  // symbol -> uid from the archive directory (canonical symbol bytes).
  std::vector<std::pair<std::string, std::uint32_t>> syms;
  const std::span<const ArchiveV2DirEntry> dir = arch->directory();
  syms.reserve(dir.size());
  for (const ArchiveV2DirEntry &e : dir) {
    syms.emplace_back(std::string(e.symbol, e.symbol_len), e.uid);
  }

  return MarketSnapshot{std::move(surfaces), std::move(provenance), std::move(*set), ts,
                        std::move(syms)};
}

const SurfaceProvenance *MarketSnapshot::provenance(std::uint32_t uid) const noexcept {
  if (provenance_.size() != surfaces_.size()) {
    return nullptr;
  }
  for (std::size_t i = 0; i < surfaces_.size(); ++i) {
    if (surfaces_[i].uid() == uid) {
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
  for (const auto &[sym, uid] : syms_) {
    if (sym == query) {
      return uid;
    }
  }
  return std::nullopt;
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

Result<BacktestResult> run_backtest(const Clock &clock, PortfolioState initial,
                                    const RunConfig &cfg) {
  ATX_VOL_PROFILE_SCOPE(BacktestTotal);
  ATX_TRY_VOID(validate_run_config(cfg));
  ATX_TRY_VOID(validate_run_query_route(cfg));
  ATX_TRY_VOID(validate_lot_economics(initial.lots, "initial fixed book"));
  const std::span<const SnapshotRef> refs = clock.refs();
  if (refs.empty()) {
    return Err(ErrorCode::InvalidArgument, "run_backtest: empty clock");
  }
  const std::size_t stride = cfg.record_every_n;

  BacktestResult out;
  PortfolioState book = std::move(initial);
  RetainedBookPricer retained_pricer;
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
    out.gross_vega.push_back(g.vega);
    out.gross_theta.push_back(g.theta);
    out.turnover_notional.push_back(0.0);
    out.turnover_vega.push_back(0.0);
    out.n_open_lots.push_back(static_cast<double>(n_lots));
    out.n_unpriced_lots.push_back(n_unpriced);
    out.n_unpriced_greeks.push_back(n_unpriced_greeks);
  };

  // base = load(refs[0]) — the inception snapshot.
  const std::shared_ptr<SnapshotCache> snapshot_cache =
      cfg.snapshot_cache ? cfg.snapshot_cache
                         : std::make_shared<SnapshotCache>(kPrivateSnapshotCacheCapacity);
  auto base_res = snapshot_cache->load(refs[0].archive_path, cfg.query_pricing_tier,
                                       cfg.query_cache_build_policy);
  if (!base_res) {
    return Err(base_res.error());
  }
  std::shared_ptr<const MarketSnapshot> base = std::move(*base_res);
  ATX_TRY_VOID(validate_snapshot_provenance(*base, cfg.surface_provenance_policy));
  ATX_TRY_VOID(validate_lot_economics(book.lots, "initial fixed book", base->ts_ns()));
  if (cfg.prefetch_snapshots && refs.size() > 1) {
    const Status prefetch_status = snapshot_cache->prefetch(
        refs[1].archive_path, cfg.query_pricing_tier, cfg.query_cache_build_policy);
    if (!prefetch_status) {
      return Err(prefetch_status.error());
    }
  }

  double nav = 0.0;

  // Row 0: inception (zero PnL, nav 0, book greeks on the first date). Even though
  // no step has run, book_greeks is a real measurement here — an inception book with
  // an unpriced held lot aborts under the Error policy (an empty book prices to 0).
  {
    auto g = book_greeks(*base, book.lots, cfg.price, retained_pricer);
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

  for (std::size_t i = 1; i < refs.size(); ++i) {
    auto shifted_res = snapshot_cache->load(refs[i].archive_path, cfg.query_pricing_tier,
                                            cfg.query_cache_build_policy);
    if (!shifted_res) {
      return Err(shifted_res.error());
    }
    std::shared_ptr<const MarketSnapshot> shifted = std::move(*shifted_res);
    ATX_TRY_VOID(validate_snapshot_provenance(*shifted, cfg.surface_provenance_policy));
    if (cfg.prefetch_snapshots && i + 1 < refs.size()) {
      const Status prefetch_status = snapshot_cache->prefetch(
          refs[i + 1].archive_path, cfg.query_pricing_tier, cfg.query_cache_build_policy);
      if (!prefetch_status) {
        return Err(prefetch_status.error());
      }
    }

    // Partition + Taylor PnL-explain: byte-identical arithmetic to the strategy
    // overload's step (shared `compute_step`), which now also reports the count of
    // held lots the pricer could not value this step.
    auto step = compute_step(*base, *shifted, book.lots, cfg.price, retained_pricer);
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
    nav += step_total; // cumulative every step, regardless of recording

    // Adopt the shifted snapshot as the next base (no reload) and drop expiries.
    base = std::move(shifted);
    std::erase_if(book.lots, [&base](const Lot &l) { return l.expiry_ts_ns <= base->ts_ns(); });

    const bool is_last = (i + 1 == refs.size());
    const bool record = ((i % stride) == 0) || is_last;
    if (record) {
      auto g = book_greeks(*base, book.lots, cfg.price, retained_pricer);
      if (!g) {
        return Err(g.error());
      }
      if (cfg.unpriced == UnpricedLotPolicy::Error && g->n_unpriced > 0) {
        return Err(ErrorCode::NotFound, unpriced_greeks_error_message(
                                            g->n_unpriced, g->first_unpriced_uid, refs[i].date));
      }
      push_row(refs[i].date, base->ts_ns(), step_total, t.pnl_delta, t.pnl_gamma, t.pnl_vega,
               t.pnl_vanna, t.pnl_volga, t.pnl_theta, t.pnl_rho, t.pnl_charm, t.pnl_unexplained,
               settlement, nav, g->total, book.lots.size(), static_cast<double>(step->n_unpriced),
               static_cast<double>(g->n_unpriced));
    }
  }

  return Ok(std::move(out));
}

// One step's realized trade accounting (frictions + turnover) computed by the
// engine executor over the book diff and the hedge overlay.
struct ExecResult {
  double cost{0.0};
  double turnover_notional{0.0};
  double turnover_vega{0.0};
  std::optional<BookGreeks> book_greeks{};
};

Result<BacktestResult> run_backtest(const Clock &clock, IStrategy &strat, const RunConfig &cfg) {
  ATX_VOL_PROFILE_SCOPE(BacktestTotal);
  ATX_TRY_VOID(validate_run_config(cfg));
  ATX_TRY_VOID(validate_run_query_route(cfg));
  const std::span<const SnapshotRef> refs = clock.refs();
  if (refs.empty()) {
    return Err(ErrorCode::InvalidArgument, "run_backtest: empty clock");
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

  BacktestResult out;
  PortfolioState book{};
  std::uint64_t next_id = 1; // monotonic lot ids the strategy consumes
  ReusablePriceFrame risk_frame;
  ReusableTargetMarkFrame target_marks;
  RetainedBookPricer retained_pricer;
  ReusableLotIdIndex before_lot_index;
  ReusableLotIdIndex after_lot_index;
  std::vector<Lot> before_lots;

  // ── Engine-internal cash + per-uid share ledger (B2/B3) ────────────────────
  double cash = cfg.financing.initial_cash;
  // B3: O(1) get/add/sum + allocation-free daily hedge pass (was a linear-scan
  // `shares` vector + a per-uid whole-frame delta rescan). Bit-identical output.
  HedgeLedger hedge_ledger;

  // Per-share half-spread under the friction model (0 when SpreadKind::None).
  const auto half_spread = [&cfg](double mark, double vega) -> double {
    switch (cfg.frictions.spread_kind) {
    case FrictionModel::SpreadKind::None:
      return 0.0;
    case FrictionModel::SpreadKind::PriceBps:
      return mark * (cfg.frictions.half_spread_bps / 1.0e4);
    case FrictionModel::SpreadKind::VolTicks:
      return vega * cfg.frictions.vol_tick;
    }
    return 0.0;
  };

  const auto push_row = [&out](const std::string &date, std::int64_t ts, double p_total,
                               double p_delta, double p_gamma, double p_vega, double p_vanna,
                               double p_volga, double p_theta, double p_rho, double p_charm,
                               double p_unexpl, double p_settle, double p_shares, double p_fin,
                               double p_cost, double nav_v, double cash_v, double g_delta,
                               const PriceTotals &g, double turn_notl, double turn_vega,
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
    out.pnl_shares.push_back(p_shares);
    out.financing.push_back(p_fin);
    out.cost.push_back(p_cost);
    out.nav.push_back(nav_v);
    out.cash.push_back(cash_v);
    out.gross_delta.push_back(g_delta); // NET book delta = option delta + hedge shares
    out.gross_gamma.push_back(g.gamma);
    out.gross_vega.push_back(g.vega);
    out.gross_theta.push_back(g.theta);
    out.turnover_notional.push_back(turn_notl);
    out.turnover_vega.push_back(turn_vega);
    out.n_open_lots.push_back(static_cast<double>(n_lots));
    out.n_unpriced_lots.push_back(n_unpriced);
    out.n_unpriced_greeks.push_back(n_unpriced_greeks);
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
      const double mark = lot.entry_price; // entry_mark (fill at mid)
      const double hs = half_spread(mark, vega);
      const double leg_cost = std::fabs(lot.qty) * lot.multiplier * hs +
                              cfg.frictions.per_contract_cost * std::fabs(lot.qty);
      ex.cost += leg_cost;
      cash -= lot.qty * lot.multiplier * mark; // premium paid (long) / received (short)
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
      const PricedSurface *s = base_snap.find(lot.contract.uid);
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
      hedge_ledger.hedge_daily(
          book.lots, *current_risk, hedge_spec.band, cfg.frictions.hedge_slippage_bps,
          [&base_snap](std::uint32_t uid) -> double {
            const PricedSurface *surface = base_snap.find(uid);
            return surface != nullptr ? surface->pricing().S : 0.0;
          },
          cash, ex.cost);
    }

    cash -= ex.cost; // realized frictions hit cash at fill
    return Ok(ex);
  };

  const std::shared_ptr<SnapshotCache> snapshot_cache =
      cfg.snapshot_cache ? cfg.snapshot_cache
                         : std::make_shared<SnapshotCache>(kPrivateSnapshotCacheCapacity);
  auto base_res = snapshot_cache->load(refs[0].archive_path, cfg.query_pricing_tier,
                                       cfg.query_cache_build_policy);
  if (!base_res) {
    return Err(base_res.error());
  }
  std::shared_ptr<const MarketSnapshot> base = std::move(*base_res);
  ATX_TRY_VOID(validate_snapshot_provenance(*base, cfg.surface_provenance_policy));
  if (cfg.prefetch_snapshots && refs.size() > 1) {
    const Status prefetch_status = snapshot_cache->prefetch(
        refs[1].archive_path, cfg.query_pricing_tier, cfg.query_cache_build_policy);
    if (!prefetch_status) {
      return Err(prefetch_status.error());
    }
  }

  double nav = 0.0;

  // Inception (row 0): open positions AS OF refs[0], book entry frictions + premium
  // + the opening hedge into cash; PnL columns are zero; record post-trade cash.
  {
    const std::uint64_t next_id_before = next_id;
    Status st = [&]() {
      ATX_VOL_PROFILE_SCOPE(StrategyStep);
      return strat.on_step(*base, 0, book, next_id, cfg.price);
    }();
    if (!st) {
      return Err(st.error());
    }
    before_lots.clear(); // empty => every opened lot is a fresh entry
    ATX_TRY_VOID(validate_strategy_transition(before_lots, book.lots, next_id_before, next_id,
                                              base->ts_ns(), before_lot_index, after_lot_index));
    const HedgeSpec hedge_spec = strat.hedge_spec();
    ATX_TRY_VOID(validate_hedge_spec(hedge_spec));
    auto ex = execute(*base, before_lots, hedge_spec, nullptr);
    if (!ex) {
      return Err(ex.error());
    }
    Result<BookGreeks> g = ex->book_greeks.has_value()
                               ? Ok(*ex->book_greeks)
                               : book_greeks(*base, book.lots, cfg.price, retained_pricer);
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
    push_row(refs[0].date, base->ts_ns(), nav, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0,
             0.0, 0.0, ex->cost, nav, cash, g_delta, g->total, ex->turnover_notional,
             ex->turnover_vega, book.lots.size(), 0.0, static_cast<double>(g->n_unpriced));
    record_signals(*base);
  }

  for (std::size_t i = 1; i < refs.size(); ++i) {
    auto shifted_res = snapshot_cache->load(refs[i].archive_path, cfg.query_pricing_tier,
                                            cfg.query_cache_build_policy);
    if (!shifted_res) {
      return Err(shifted_res.error());
    }
    std::shared_ptr<const MarketSnapshot> shifted = std::move(*shifted_res);
    ATX_TRY_VOID(validate_snapshot_provenance(*shifted, cfg.surface_provenance_policy));
    if (cfg.prefetch_snapshots && i + 1 < refs.size()) {
      const Status prefetch_status = snapshot_cache->prefetch(
          refs[i + 1].archive_path, cfg.query_pricing_tier, cfg.query_cache_build_policy);
      if (!prefetch_status) {
        return Err(prefetch_status.error());
      }
    }

    // 1. PnL of the current book (resolved on base) forward to shifted (unchanged B1).
    auto step = compute_step(*base, *shifted, book.lots, cfg.price, retained_pricer, &target_marks);
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
    if (cfg.financing.finance_premium) {
      const double r = base->surfaces().front().pricing().r; // base-date rate
      const double growth = std::exp(r * dt);
      financing += cash * (growth - 1.0); // cash carry on the pre-step balance
      cash *= growth;                     // apply to the ledger
    }
    for (const auto &[uid, n] : hedge_ledger.entries()) {
      const PricedSurface *bs = base->find(uid);
      const PricedSurface *ss = shifted->find(uid);
      if (bs == nullptr || ss == nullptr) {
        continue;
      }
      const double Sb = bs->pricing().S;
      shares_pnl += n * (ss->pricing().S - Sb);                      // shares held over the step
      const double short_amt = std::max(0.0, -n);                    // |min(shares,0)|
      financing += -cfg.financing.borrow_rate * short_amt * Sb * dt; // borrow (0 when rate 0)
      if (cfg.financing.shares_carry) {
        // Buying shares has already reduced the financed cash balance, so cash
        // carry owns the funding cost when enabled. Charging r here too would
        // count it twice. Without cash financing, retain the standalone (q-r)
        // total-carry shortcut.
        const double funding_rate = cfg.financing.finance_premium ? 0.0 : bs->pricing().r;
        financing += n * (bs->q_eff_at(0.25) - funding_rate) * Sb * dt;
      }
    }

    // 3. Adopt shifted as the next base; settle expiries into cash; drop them.
    base = std::move(shifted);
    for (const Lot &lot : book.lots) {
      if (lot.expiry_ts_ns > base->ts_ns()) {
        continue;
      }
      const PricedSurface *bs = base->find(lot.contract.uid);
      if (bs == nullptr) {
        continue;
      }
      const double S = bs->pricing().S;
      const double K = lot.contract.K;
      const double intrinsic =
          (lot.contract.side == Side::Call) ? std::max(0.0, S - K) : std::max(0.0, K - S);
      cash += lot.qty * lot.multiplier * intrinsic; // intrinsic settle proceeds
    }
    std::erase_if(book.lots, [&base](const Lot &l) { return l.expiry_ts_ns <= base->ts_ns(); });

    // 4-5. Strategy entries/rolls + hedge overlay on the new base.
    before_lots.assign(book.lots.begin(), book.lots.end()); // survivors before on_step
    const std::uint64_t next_id_before = next_id;
    Status st = [&]() {
      ATX_VOL_PROFILE_SCOPE(StrategyStep);
      return strat.on_step(*base, i, book, next_id, cfg.price);
    }();
    if (!st) {
      return Err(st.error());
    }
    ATX_TRY_VOID(validate_strategy_transition(before_lots, book.lots, next_id_before, next_id,
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
    step_total += financing;
    step_total -= ex->cost;
    nav += step_total;

    // 7. Record @ granularity: book greeks (net delta incl. shares) + B2 columns.
    const bool is_last = (i + 1 == refs.size());
    const bool record = ((i % stride) == 0) || is_last;
    if (record) {
      Result<BookGreeks> g = ex->book_greeks.has_value()
                                 ? Ok(*ex->book_greeks)
                                 : book_greeks(*base, book.lots, cfg.price, retained_pricer);
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
      push_row(refs[i].date, base->ts_ns(), step_total, t.pnl_delta, t.pnl_gamma, t.pnl_vega,
               t.pnl_vanna, t.pnl_volga, t.pnl_theta, t.pnl_rho, t.pnl_charm, t.pnl_unexplained,
               settlement, shares_pnl, financing, ex->cost, nav, cash, g_delta, g->total,
               ex->turnover_notional, ex->turnover_vega, book.lots.size(),
               static_cast<double>(step->n_unpriced), static_cast<double>(g->n_unpriced));
      record_signals(*base);
    }
  }

  return Ok(std::move(out));
}

} // namespace atx::vol
