#pragma once

// atx::engine — Portfolio: average-cost position keeping + exact-money P&L (P2-5).
//
// ===========================================================================
//  The accounting model — average cost basis
// ===========================================================================
//  Each Holding tracks a signed share quantity, a running average entry price,
//  and cumulative realized P&L and fees. apply_fill drives a four-case state
//  machine off a single signed-qty FillPayload (+buy / −sell, the canonical
//  direction from exec/payloads.hpp):
//
//    open / increase  (flat, or fill same sign as position):
//        new_avg = (|old|·avg + |fill|·price) / (|old| + |fill|)   [weighted avg]
//        qty    += fill                                            (avg unchanged
//        for old==0 this reduces to avg = price.)                   per share added
//
//    reduce / close   (fill opposes position, |fill| ≤ |position|):
//        realized += closed · (price − avg) · sign(position)
//        qty      += fill          (avg UNCHANGED; the remaining shares keep their
//                                   original cost basis)
//        if qty == 0 → avg = 0     (a flat position has no basis)
//
//    flip             (fill opposes position, |fill| > |position|):
//        realized += |position| · (price − avg) · sign(position)   (close old leg)
//        qty       = position + fill                               (the remainder,
//        avg       = price                                          same sign as
//                                                                    fill, opened
//                                                                    at the fill)
//
//  Cash, every frontier: cash -= fill·price + fee. With fill signed, a buy
//  (fill>0) DEBITS cash and a sell (fill<0) CREDITS it — the sign falls out of
//  the signed multiply. Fees always DEBIT (charged on both buys and sells).
//
// ===========================================================================
//  Realized-P&L sign convention (stated explicitly — exact-money critical)
// ===========================================================================
//  realized for a closed lot = closed · (price − avg) · sign(position):
//    * closing a LONG  (sign +1): profit when price > avg (sold above cost). ✓
//    * closing a SHORT (sign −1): (price − avg) is the BUY-vs-entry move, and
//      multiplying by −1 flips it so covering BELOW the short entry is a GAIN.
//  The whole term is exact Decimal: (price − avg) is a Decimal subtraction; the
//  integer `closed` enters via Decimal::from_int; the sign is a Decimal negation.
//  No floating point touches realized.
//
// ===========================================================================
//  The Decimal ↔ f64 money boundary
// ===========================================================================
//  EXACT Decimal: cash, realized, fees, avg_price — the ledger. Every cash and
//  realized update is an exact Decimal op (no FP drift across a long backtest).
//  f64: mark, market_value, unrealized, equity, gross, net, leverage — these are
//  marked-to-MARKET quantities derived from f64 market data (Market::mark), so
//  an exact type would be false precision. There are exactly TWO Decimal→f64
//  crossings, both read-only (the ledger is never mutated through them):
//  cash().to_double() in equity(), and avg_price.to_double() in
//  Holding::unrealized(); each is documented at its call site. Portfolio-level
//  f64 sums (equity/gross/net) iterate holdings in the FIXED universe index order
//  so the floating-point reduction is deterministic across runs.
//
// ===========================================================================
//  Universe & storage
// ===========================================================================
//  DENSE storage: a std::vector<Holding> sized ONCE at construction to the
//  universe size and NEVER reallocated thereafter (no element is ever inserted or
//  erased — apply_fill mutates in place), indexed by the same sorted (id → index)
//  idiom RollingPanel / Market use (Symbol has no std::hash and hash iteration
//  order is non-deterministic — a sorted array is the deterministic house
//  pattern). std::vector rather than core::container::fixed_vector because the
//  latter's capacity is a compile-time template parameter, while the universe
//  size is a runtime constructor argument. A fill for an instrument NOT in the
//  universe is a programmer error (ATX_ASSERT): the loop only fills universe names.
//
// ===========================================================================
//  Ownership / threading
// ===========================================================================
//  The universe span is non-owning — the caller's InstrumentId storage must
//  outlive the Portfolio. Holdings live in an owned std::vector sized once at
//  construction and never reallocated. Single-threaded backtest use; no
//  synchronisation.

#include <algorithm> // std::sort, std::lower_bound
#include <cmath>     // std::isnan (unpriced-mark sentinel handling)
#include <limits>    // std::numeric_limits (quiet NaN sentinel for an unpriced mark)
#include <span>      // std::span (universe)
#include <vector>    // std::vector (dense holdings sized once + sorted index)

#include "atx/core/decimal.hpp" // atx::core::Decimal
#include "atx/core/macro.hpp"   // ATX_ASSERT
#include "atx/core/types.hpp"   // usize, i64, f64

#include "atx/engine/exec/payloads.hpp" // FillPayload
#include "atx/engine/loop/market.hpp"   // Market (mark_to_market source)
#include "atx/engine/loop/types.hpp"    // InstrumentId

namespace atx::engine {

// ===========================================================================
//  Holding — one instrument's average-cost position + cumulative P&L.
//
//  qty is signed (+long / −short). avg_price/realized/fees are exact Decimal
//  (the ledger). mark is the latest f64 reference price written by
//  mark_to_market; market_value/unrealized are f64 because they value the open
//  position against f64 market data.
//
//  UNPRICED SENTINEL: mark initialises to a quiet NaN — "not yet priced" —
//  matching Market::mark()'s sentinel. A position opened before the first
//  mark_to_market therefore has a NaN mark; market_value()/unrealized() treat a
//  NaN mark as a ZERO contribution (an unpriced instrument adds nothing to
//  equity) so the portfolio's f64 aggregates stay well-defined and never
//  propagate a phantom value off a default-zero price.
// ===========================================================================
struct Holding {
  InstrumentId id{};
  atx::i64 qty = 0;
  atx::core::Decimal avg_price{};
  atx::core::Decimal realized{};
  atx::core::Decimal fees{};
  atx::f64 mark = std::numeric_limits<atx::f64>::quiet_NaN(); // NaN == not yet priced

  /// Signed dollar exposure: qty · mark (long > 0, short < 0). An unpriced (NaN
  /// mark) instrument contributes 0 — never a phantom value. f64 (market data).
  [[nodiscard]] atx::f64 market_value() const noexcept {
    if (std::isnan(mark)) {
      return 0.0;
    }
    return static_cast<atx::f64>(qty) * mark;
  }

  /// Open-position P&L: qty · (mark − avg_price). Zero when flat (qty == 0) so a
  /// closed position never reports phantom unrealized off a stale avg, and zero
  /// when unpriced (NaN mark) so a freshly-opened position pre-mark adds nothing.
  /// f64. SAFETY: avg_price.to_double() is a deliberate Decimal→f64 crossing of
  /// the money boundary (open-position value is an f64 estimate; the realized
  /// ledger stays exact). The other crossing is cash().to_double() in equity().
  [[nodiscard]] atx::f64 unrealized() const noexcept {
    if (qty == 0 || std::isnan(mark)) {
      return 0.0;
    }
    return static_cast<atx::f64>(qty) * (mark - avg_price.to_double());
  }
};

// ===========================================================================
//  Portfolio — cash ledger + average-cost holdings over a fixed universe.
// ===========================================================================
class Portfolio {
public:
  /// Build a portfolio with `starting_cash` (exact) over `universe` (fixed
  /// instrument set). NON-OWNING universe: the caller's InstrumentId storage must
  /// outlive the Portfolio. Every universe member starts flat (qty 0, avg 0).
  Portfolio(atx::core::Decimal starting_cash, std::span<const InstrumentId> universe) noexcept
      : universe_{universe}, cash_{starting_cash} {
    build_index();
    holdings_.resize(universe.size());
    for (atx::usize i = 0; i < universe.size(); ++i) {
      holdings_[i].id = universe[i]; // flat Holding with its id stamped
    }
  }

  /// Apply one confirmed fill, advancing the open/increase/reduce/close/flip
  /// state machine and the cash ledger. fill.qty is signed (+buy / −sell).
  ///
  /// PRECONDITIONS (programmer errors — ABORT in debug):
  ///   * fill.price > 0  — the exec sim never emits a zero/negative price;
  ///   * fill.id is in the universe — the loop only fills universe instruments;
  ///   * fill.qty != 0   — a zero-qty fill is not a transaction;
  ///   * fill.fee >= 0   — Phase-2 commissions are always a cost (no rebates), so
  ///     the accumulated `fees` stays a positive magnitude (the cash debit uses
  ///     the same value). A signed rebate is out of scope; relax this assert and
  ///     the fee-sign comment together if it is ever introduced.
  void apply_fill(const exec::FillPayload &f) noexcept {
    ATX_ASSERT(f.price > atx::core::Decimal{}); // zero-price guard (see header)
    ATX_ASSERT(f.qty != 0);
    ATX_ASSERT(f.fee >= atx::core::Decimal{}); // commissions are a cost, never a rebate
    Holding &h = holdings_[require_index(f.id)];

    apply_cash(f);
    apply_position(h, f);
  }

  /// Refresh every holding's mark from the book (f64). Marks an absent / unset
  /// instrument with whatever the book holds (NaN if never priced); callers that
  /// read equity must ensure all held instruments have been priced.
  void mark_to_market(const Market &mkt) noexcept {
    for (Holding &h : holdings_) {
      h.mark = mkt.mark(h.id);
    }
  }

  /// Exact cash balance.
  [[nodiscard]] atx::core::Decimal cash() const noexcept { return cash_; }

  /// Total account value: cash + Σ market_value, summed in fixed index order.
  /// SAFETY: cash().to_double() here is ONE of the two Decimal→f64 crossings of
  /// the money boundary (the other is avg_price.to_double() inside
  /// Holding::unrealized()). The exact ledger (cash/realized/fees/avg_price) is
  /// never mutated through these reads; they only value the book in f64 for the
  /// equity/exposure aggregates, which are inherently market-data (f64) figures.
  [[nodiscard]] atx::f64 equity() const noexcept {
    atx::f64 sum = cash_.to_double();
    for (const Holding &h : holdings_) {
      sum += h.market_value();
    }
    return sum;
  }

  /// Gross exposure: Σ |market_value| (fixed index order).
  [[nodiscard]] atx::f64 gross() const noexcept {
    atx::f64 sum = 0.0;
    for (const Holding &h : holdings_) {
      const atx::f64 mv = h.market_value();
      sum += (mv < 0.0) ? -mv : mv;
    }
    return sum;
  }

  /// Net exposure: Σ market_value (fixed index order).
  [[nodiscard]] atx::f64 net() const noexcept {
    atx::f64 sum = 0.0;
    for (const Holding &h : holdings_) {
      sum += h.market_value();
    }
    return sum;
  }

  /// Leverage: gross / equity. Returns 0 when equity is non-positive (a wiped or
  /// empty book has no meaningful leverage — guarded to avoid div-by-zero / inf).
  [[nodiscard]] atx::f64 leverage() const noexcept {
    const atx::f64 eq = equity();
    if (eq <= 0.0) {
      return 0.0;
    }
    return gross() / eq;
  }

  /// Debit a financing / borrow charge directly from cash, with NO position change.
  /// Short-borrow accrual (S6) cannot ride apply_fill (which requires qty != 0), so
  /// this is the minimal additive cash-only mutator. `charge` >= 0 (a cost, never a
  /// rebate) — same fee-sign discipline as apply_fill.
  void accrue_financing(atx::core::Decimal charge) noexcept {
    ATX_CHECK(charge >= atx::core::Decimal{}); // financing is a cost, never a credit
    cash_ = cash_ - charge;
  }

  /// Read-only access to an instrument's holding (for inspection / tests).
  /// PRECONDITION: id is in the universe (ABORTS in debug).
  [[nodiscard]] const Holding &holding(InstrumentId id) const noexcept {
    return holdings_[require_index(id)];
  }

private:
  /// Cash ledger update: cash -= fill·price + fee, and accumulate the fee as a
  /// positive magnitude. Exact Decimal throughout. fill.qty signed: a buy debits,
  /// a sell credits; the fee always debits.
  void apply_cash(const exec::FillPayload &f) noexcept {
    const atx::core::Decimal notional = atx::core::Decimal::from_int(f.qty) * f.price;
    cash_ = cash_ - notional - f.fee;
  }

  /// Position state machine (avg-cost). Routes the fill to the increase, reduce/
  /// close, or flip branch and accumulates the fee on the touched holding.
  void apply_position(Holding &h, const exec::FillPayload &f) noexcept {
    h.fees = h.fees + f.fee; // positive magnitude (fees are always charged)

    const bool same_side_or_flat = (h.qty == 0) || same_sign(h.qty, f.qty);
    if (same_side_or_flat) {
      apply_increase(h, f);
      return;
    }
    const atx::i64 closing = abs_i64(f.qty);
    const atx::i64 open_mag = abs_i64(h.qty);
    if (closing <= open_mag) {
      apply_reduce(h, f);
    } else {
      apply_flip(h, f);
    }
  }

  /// Open from flat or increase on the same side → weighted-average entry price.
  /// new_avg = (|old|·avg + |fill|·price) / (|old| + |fill|). For old == 0 the
  /// first term vanishes and avg collapses to the fill price.
  void apply_increase(Holding &h, const exec::FillPayload &f) noexcept {
    const atx::i64 old_mag = abs_i64(h.qty);
    const atx::i64 add_mag = abs_i64(f.qty);
    const atx::core::Decimal old_q = atx::core::Decimal::from_int(old_mag);
    const atx::core::Decimal add_q = atx::core::Decimal::from_int(add_mag);
    const atx::core::Decimal total_q = atx::core::Decimal::from_int(old_mag + add_mag);

    const atx::core::Decimal weighted = (old_q * h.avg_price) + (add_q * f.price);
    h.avg_price = weighted / total_q;
    h.qty += f.qty;
  }

  /// Reduce or fully close (|fill| ≤ |position|). Books realized on the closed
  /// shares; the remaining shares keep their original avg. A full close (qty→0)
  /// zeroes the avg so a flat position carries no stale basis.
  void apply_reduce(Holding &h, const exec::FillPayload &f) noexcept {
    const atx::i64 closed = abs_i64(f.qty);
    h.realized = h.realized + realized_for(h, f.price, closed);
    h.qty += f.qty;
    if (h.qty == 0) {
      h.avg_price = atx::core::Decimal{};
    }
  }

  /// Flip (|fill| > |position|): close the entire old leg (booking its realized),
  /// then open the remainder on the OPPOSITE side at the fill price.
  void apply_flip(Holding &h, const exec::FillPayload &f) noexcept {
    const atx::i64 closed = abs_i64(h.qty); // whole old leg
    h.realized = h.realized + realized_for(h, f.price, closed);
    h.qty += f.qty;        // remainder, same sign as the fill
    h.avg_price = f.price; // remainder basis is the fill price
  }

  /// realized for closing `closed` (positive magnitude) shares of `h` at `price`:
  /// closed · (price − avg) · sign(position). Exact Decimal; sign(position) is a
  /// Decimal negation when the position is short. See header sign convention.
  [[nodiscard]] static atx::core::Decimal realized_for(const Holding &h, atx::core::Decimal price,
                                                       atx::i64 closed) noexcept {
    const atx::core::Decimal per_share = price - h.avg_price;
    const atx::core::Decimal pnl = atx::core::Decimal::from_int(closed) * per_share;
    return (h.qty < 0) ? -pnl : pnl;
  }

  // ---- small integer helpers (avoid std::abs UB at INT64_MIN; qty is bounded) -

  /// |x| for an i64 share count. Share counts are bounded well below INT64_MAX in
  /// any realistic universe, so the negate is safe; documented for the reader.
  [[nodiscard]] static atx::i64 abs_i64(atx::i64 x) noexcept { return (x < 0) ? -x : x; }

  /// True iff a and b have the same (nonzero) sign.
  [[nodiscard]] static bool same_sign(atx::i64 a, atx::i64 b) noexcept {
    return (a > 0 && b > 0) || (a < 0 && b < 0);
  }

  // ---- sorted id → dense index (the deterministic house pattern) -------------

  static constexpr atx::usize kNoIndex = static_cast<atx::usize>(-1);

  struct IdIndex {
    InstrumentId id{};
    atx::usize idx{};
  };

  void build_index() {
    index_.reserve(universe_.size());
    for (atx::usize i = 0; i < universe_.size(); ++i) {
      index_.push_back(IdIndex{universe_[i], i});
    }
    std::sort(index_.begin(), index_.end(),
              [](const IdIndex &a, const IdIndex &b) noexcept { return a.id < b.id; });
  }

  [[nodiscard]] atx::usize index_of(InstrumentId id) const noexcept {
    const auto it =
        std::lower_bound(index_.begin(), index_.end(), id,
                         [](const IdIndex &e, InstrumentId key) noexcept { return e.id < key; });
    if (it != index_.end() && it->id == id) {
      return it->idx;
    }
    return kNoIndex;
  }

  [[nodiscard]] atx::usize require_index(InstrumentId id) const noexcept {
    const atx::usize idx = index_of(id);
    ATX_ASSERT(idx != kNoIndex); // out-of-universe fill/query is a programmer error
    return idx;
  }

  std::span<const InstrumentId> universe_; // non-owning, fixed order
  std::vector<IdIndex> index_{};           // sorted id → dense index (built once)
  std::vector<Holding> holdings_{};        // dense, parallel to the universe order
  atx::core::Decimal cash_{};              // exact cash ledger
};

} // namespace atx::engine
