#pragma once

// atx::engine::exec — ExecutionSimulator: the cost-honesty core (P2-6).
//
// ===========================================================================
//  Why this type exists — the cost model is what makes a backtest honest
// ===========================================================================
//  A strategy that transacts at the untouched mid price is a fiction: real
//  fills pay the spread, move the book, and incur commission. The
//  ExecutionSimulator turns intents (OrderPayload, signed-qty +buy/-sell) into
//  confirmed executions (FillPayload) under a deterministic, fully-configurable
//  cost model. The modelled cost — not the gross alpha — is what separates a
//  trustworthy backtest from an over-fit one.
//
// ===========================================================================
//  The look-ahead firewall (execution side)
// ===========================================================================
//  An order queued on the bar at time t can NEVER fill on that same bar: the
//  decision used bar-t information, so executing against bar-t's own close would
//  be trading on knowledge the strategy did not have when it decided. queue()
//  records each order's queued_at; settle_pending(now, ...) fills an order only
//  when `now > queued_at` (strictly later slice) AND `now >= queued_at +
//  latency`. The same-bar relaxation (`>=` instead of `>`) is gated behind
//  FillCfg.allow_same_bar_fill, which DEFAULTS OFF — a reviewer must never find
//  the cheat enabled by default. This is the execution-side counterpart to the
//  RollingPanel's data-side point-in-time firewall (P2-2).
//
// ===========================================================================
//  Application order per settle (frozen, see Appendix A)
// ===========================================================================
//    eligibility firewall  -> volume cap (+ partial spillover)
//      -> slippage          (fill price moves adverse to the trade direction)
//      -> temporary impact  (added to the FILL PRICE; does NOT persist)
//      -> permanent impact  (shifts the instrument MARK; persists)
//      -> commission        (exact Decimal fee)
//      -> emit Fill
//  Open orders are processed in INSERTION (FIFO) order so the scarce per-bar
//  liquidity (the volume cap) is allocated reproducibly across orders.
//
// ===========================================================================
//  Temporary vs permanent impact — the distinction that matters
// ===========================================================================
//  TEMPORARY impact is the transient cost of demanding immediacy; it is paid on
//  THIS fill (folded into the fill PRICE) and vanishes — it does NOT move the
//  reference mark, so it never leaks into a later bar's mark-to-market P&L.
//  PERMANENT impact is the lasting information the trade reveals to the market;
//  it SHIFTS THE MARK via Market::shift_mark, so every subsequent valuation (and
//  every later fill against that instrument) reflects the moved price. Modelling
//  these separately is the whole point: conflating them either double-counts the
//  transient cost into future P&L or lets a large trade move the market for free.
//
// ===========================================================================
//  Units convention (the one genuine design choice — documented crisply)
// ===========================================================================
//  Slippage and BOTH impact terms are FRACTIONS of the reference mark:
//    slippage : fill = ref * (1 + sign * slip_frac)
//    temporary: fill *= (1 + sign * temp)        temp = Y * sigma * part^delta
//    permanent: shift_mark(ref * perm * sign)    perm = 0.5 * gamma * sigma * part
//  where part = fillable / ADV, share = fillable / bar_vol, sign = +1 buy / -1
//  sell. Working in fractions of ref keeps every coefficient scale-free (a $10
//  and a $1000 stock incur the same FRACTIONAL cost for the same participation),
//  matches how sigma (a return volatility) is naturally expressed, and is why the
//  permanent shift is `ref * perm` (converting the fractional move back to price
//  units for shift_mark, which takes an absolute price delta).
//
// ===========================================================================
//  Configurability ethos — every coefficient is a named, defaulted knob
// ===========================================================================
//  There are NO hardcoded magic numbers in the algorithm. Every coefficient is a
//  field on one of the cfg structs below, each carrying its Appendix-A default.
//  Models are config-driven (a mode enum + coefficients held BY VALUE), NOT a
//  virtual model hierarchy: Phase-2 ships exactly one slippage and one commission
//  formula, selected by an exhaustive switch (no `default`, so a new mode surfaces
//  as a /W4 warning at the switch rather than a silent fall-through).
//
// ===========================================================================
//  The f64 <-> Decimal money boundary
// ===========================================================================
//  All cost MATH is f64 (mark, sigma, ADV, spread and the slippage/impact terms
//  are inherently approximate market statistics — an exact type would be false
//  precision, matching Market's f64 book and Portfolio's f64 mark-to-market). The
//  RESULT crosses into exact Decimal at exactly two points: the fill PRICE and the
//  commission FEE, both via Decimal::from_double(...).value_or(...). A finite
//  positive price/fee always converts; the value_or fallback is a defensive floor
//  that is unreachable for in-range inputs (documented at each call site).
//
// ===========================================================================
//  Zero steady-state allocation (per-slice hot path)
// ===========================================================================
//  settle_pending returns a std::span<const FillPayload> into a sim-owned,
//  reserved-once `fills_` scratch buffer (cleared, not reallocated, each call) —
//  NOT a fresh container per call. open_ and the volume accumulator are likewise
//  reserved at construction. BORROW LIFETIME: the returned span is valid only
//  until the next queue()/settle_pending() call mutates the scratch buffer; the
//  caller must consume it before the next sim call.
//
// ===========================================================================
//  Determinism
// ===========================================================================
//  No RNG: the sim is deterministic by construction (a probabilistic-fill model
//  is a deferred residual, see phase-2 ledger). open_ is processed in FIFO order;
//  the per-instrument volume accumulator uses a deterministic linear scan over a
//  reset-per-bar small vector (no hash container in the hot path). Two identical
//  sims fed identical orders + market produce bit-for-bit identical fills.

#include <cmath>  // std::pow (√-impact term), std::isnan (unpriced-mark guard)
#include <span>   // std::span (order input + fill output view)
#include <vector> // std::vector (reserved-once open set + scratch buffers)

#include "atx/core/datetime.hpp" // atx::core::time::Timestamp
#include "atx/core/decimal.hpp"  // atx::core::Decimal (fill price + fee, exact)
#include "atx/core/math.hpp"     // atx::core::clamp (slippage-fraction caps)
#include "atx/core/types.hpp"    // i64, f64, usize

#include "atx/engine/exec/payloads.hpp" // OrderPayload, FillPayload, OrderType
#include "atx/engine/loop/market.hpp"   // Market, InstrumentStats
#include "atx/engine/loop/types.hpp"    // InstrumentId

namespace atx::engine::exec {

// ===========================================================================
//  Configuration structs — every coefficient a named, Appendix-A-defaulted knob.
// ===========================================================================

/// Eligibility / firewall config.
struct FillCfg {
  /// The same-bar "cheat": when true, an order may fill on the SAME slice it was
  /// queued (relaxes the firewall from `now > queued_at` to `now >= queued_at`).
  /// DEFAULTS OFF — same-bar execution is look-ahead and must be opt-in only.
  bool allow_same_bar_fill = false;
};

/// Slippage formula selector (exhaustive — no `default` in the switch).
enum class SlippageMode { VolumeShare, FixedBps };

/// Slippage config. VolumeShare: fill = ref*(1 ± k*share²), share=fillable/bar_vol,
/// capped at cap_volshare. FixedBps: fill = ref*(1 ± bps/1e4), capped at cap_bps.
/// Both modes additionally floor the move at half the spread (always cross the
/// spread). All defaults from Appendix A.
struct SlippageCfg {
  SlippageMode mode = SlippageMode::VolumeShare;
  atx::f64 k = 0.1;              // VolumeShare quadratic coefficient
  atx::f64 bps = 5.0;            // FixedBps basis points
  atx::f64 cap_volshare = 0.025; // VolumeShare slippage-fraction cap (2.5%)
  atx::f64 cap_bps = 0.10;       // FixedBps slippage-fraction cap (10%)
};

/// √-impact config. Temporary (folded into the fill price): temp = Y*sigma*part^delta.
/// Permanent (shifts the mark): perm = 0.5*gamma*sigma*part. part = fillable/ADV.
struct ImpactCfg {
  atx::f64 Y = 1.0;       // temporary-impact scale
  atx::f64 delta = 0.5;   // temporary-impact exponent (√ by default; cfg 0.45–0.65)
  atx::f64 gamma = 0.314; // permanent-impact coefficient
};

/// Commission formula selector (exhaustive — no `default` in the switch).
enum class CommissionMode { PerShare, PerDollar };

/// Commission config. PerShare: max(|qty|*per_share, min_fee) capped at
/// max_pct*notional. PerDollar: |notional|*per_dollar_bps/1e4. All Appendix-A.
struct CommissionCfg {
  CommissionMode mode = CommissionMode::PerShare;
  atx::f64 per_share = 0.005;     // $/share
  atx::f64 min_fee = 1.0;         // $ per-trade floor
  atx::f64 max_pct = 0.005;       // cap as a fraction of notional (0.5%)
  atx::f64 per_dollar_bps = 15.0; // PerDollar basis points on notional
};

/// Latency config: an order is eligible only at queued_at + latency_nanos.
struct LatencyCfg {
  atx::i64 latency_nanos = 0;
};

/// Participation cap: per (instrument, bar) fillable shares are capped at
/// volume_limit * bar_volume (minus what already filled this bar).
struct VolumeCapCfg {
  atx::f64 volume_limit = 0.025; // 2.5% of bar volume per bar (Appendix A vol-share)
};

// ===========================================================================
//  ExecutionSimulator
// ===========================================================================
class ExecutionSimulator {
public:
  /// Construct with one config per cost stage (each held BY VALUE). Reserves the
  /// open set + scratch buffers up front so the settle path never allocates.
  ExecutionSimulator(FillCfg fill, SlippageCfg slippage, ImpactCfg impact, CommissionCfg commission,
                     LatencyCfg latency, VolumeCapCfg volume_cap) noexcept
      : fill_cfg_{fill}, slip_cfg_{slippage}, impact_cfg_{impact}, comm_cfg_{commission},
        latency_cfg_{latency}, cap_cfg_{volume_cap} {
    open_.reserve(kReserve);
    fills_.reserve(kReserve);
    vol_for_bar_.reserve(kReserve);
  }

  /// Default-configured simulator (every coefficient at its Appendix-A default).
  ExecutionSimulator() noexcept
      : ExecutionSimulator{FillCfg{},       SlippageCfg{}, ImpactCfg{},
                           CommissionCfg{}, LatencyCfg{},  VolumeCapCfg{}} {}

  /// Read-only access to the configured commission model. Lets a research-cadence
  /// consumer (e.g. alpha::extract_streams, P3c-2) extract the per-notional cost
  /// COEFFICIENT and apply a lightweight turnover charge WITHOUT replaying the full
  /// bar-by-bar queue/settle loop — reusing this sim's own coefficient rather than
  /// inventing a second cost number. Pure observability; mutates nothing.
  [[nodiscard]] const CommissionCfg &commission_cfg() const noexcept { return comm_cfg_; }

  /// Read-only access to the configured √-impact model. Lets a research-cadence
  /// consumer (e.g. risk::capacity_curve, P4-10) reuse this sim's own impact
  /// COEFFICIENTS to size market-impact erosion WITHOUT replaying the full
  /// bar-by-bar queue/settle loop — one cost surface, not a second cost number.
  /// Pure observability; mutates nothing.
  [[nodiscard]] const ImpactCfg &impact_cfg() const noexcept { return impact_cfg_; }

  /// Toggle the same-bar fill relaxation (the delay-0 knob; P3c-3). When false
  /// (the DEFAULT, set at construction) an order fills only on a STRICTLY-LATER
  /// slice — the no-look-ahead firewall. When true, an order may fill on the same
  /// slice it was queued (delay-0). This is the single mutable seam the BacktestLoop
  /// flips when wired with loop::Delay::Same; it never weakens the default (Next)
  /// posture, which leaves the flag off. The relaxation is itself opt-in (FillCfg
  /// defaults it OFF), so flipping it is always a deliberate, loud decision.
  void set_allow_same_bar_fill(bool allow) noexcept { fill_cfg_.allow_same_bar_fill = allow; }

  /// Add new orders to the open set. They settle on a STRICTLY LATER slice (the
  /// firewall). A zero-qty order is not a transaction and is dropped here (mirrors
  /// the make_order_event qty!=0 guard) — it never reaches the fill path. `now`
  /// is accepted for symmetry with settle_pending but order eligibility is keyed
  /// off each order's own queued_at, not this argument.
  void queue(std::span<const OrderPayload> orders, atx::core::time::Timestamp now) noexcept {
    (void)now; // eligibility uses per-order queued_at; `now` documents call cadence
    for (const OrderPayload &o : orders) {
      if (o.qty == 0) {
        continue; // zero-qty is not a transaction (see header / make_order_event)
      }
      open_.push_back(o);
    }
  }

  /// Settle every eligible open order against the book at `now`, emitting fills.
  ///
  /// Returns a span into a sim-owned scratch buffer — valid ONLY until the next
  /// queue()/settle_pending() call (see header borrow-lifetime note). Ineligible
  /// or unfillable (capped-out, limit-not-penetrated, zero-volume) orders stay in
  /// the open set; partially-filled orders have their remainder retained.
  ///
  /// `market` is taken by NON-const reference (deviating from the plan's literal
  /// `const Market&`): permanent impact MUST mutate the book via shift_mark, which
  /// is non-const. A const Market cannot express permanent impact — the contract
  /// requires the write.
  [[nodiscard]] std::span<const FillPayload> settle_pending(atx::core::time::Timestamp now,
                                                            Market &market) noexcept {
    fills_.clear();
    reset_vol_accumulator_if_new_bar(now);

    // Process the open set in FIFO order; survivors compact to the front so the
    // open set stays insertion-ordered for the next slice (determinism).
    atx::usize write = 0;
    for (atx::usize read = 0; read < open_.size(); ++read) {
      OrderPayload order = open_[read];
      const bool fully_consumed = settle_one(order, now, market);
      if (!fully_consumed) {
        open_[write++] = order; // retain (ineligible, capped remainder, no-fill)
      }
    }
    open_.resize(write);
    return std::span<const FillPayload>{fills_.data(), fills_.size()};
  }

private:
  /// Initial reservation for the open set + scratch buffers. A backtest's per-bar
  /// open set is small; this is sized once and grows only if a pathological run
  /// exceeds it (still amortised, never on the steady-state path).
  static constexpr atx::usize kReserve = 256;

  /// One in-flight per-(instrument, current bar) filled-volume tally. Reset when
  /// the bar advances; a deterministic linear scan (no hash) keys it by id.
  struct VolAccum {
    InstrumentId id{};
    atx::f64 filled = 0.0;
  };

  /// Settle a single order as far as the cost model allows on this slice. Mutates
  /// `order.qty` down by the filled amount. Returns true iff the order is fully
  /// consumed (nothing left to retain); false if a remainder (or the whole order)
  /// must stay open.
  [[nodiscard]] bool settle_one(OrderPayload &order, atx::core::time::Timestamp now,
                                Market &market) noexcept {
    if (!eligible(order, now)) {
      return false; // firewall / latency not satisfied — stays open
    }
    const atx::f64 ref = market.mark(order.id);
    if (std::isnan(ref) || !limit_marketable(order, ref)) {
      return false; // unpriced or limit not penetrated — no fill, stays open
    }

    const atx::i64 fillable = volume_capped_qty(order, market);
    if (fillable == 0) {
      return false; // cap exhausted / zero bar volume — stays open for next slice
    }

    emit_fill(order, fillable, ref, now, market);

    // Reduce the SIGNED remainder's MAGNITUDE toward zero. `fillable` is a positive
    // magnitude (<= |order.qty|), so a buy (qty > 0) subtracts it and a sell
    // (qty < 0) adds it — both move toward zero without flipping sign. A plain
    // `qty -= fillable` would shrink a buy but GROW a short (doubling it every
    // slice), so the side must drive the update.
    order.qty -= is_buy(order.qty) ? fillable : -fillable;
    return order.qty == 0; // fully consumed iff no remainder
  }

  /// Firewall + latency: eligible iff `now` is strictly after queued_at (or `>=`
  /// when the same-bar cheat is enabled) AND at least latency_nanos have elapsed.
  [[nodiscard]] bool eligible(const OrderPayload &order,
                              atx::core::time::Timestamp now) const noexcept {
    const atx::i64 now_ns = now.unix_nanos();
    const atx::i64 queued_ns = order.queued_at.unix_nanos();
    const bool past_firewall =
        fill_cfg_.allow_same_bar_fill ? (now_ns >= queued_ns) : (now_ns > queued_ns);
    const bool past_latency = now_ns >= queued_ns + latency_cfg_.latency_nanos;
    return past_firewall && past_latency;
  }

  /// Limit-order penetration gate (Market orders always pass). Buy fills only when
  /// ref <= limit; sell only when ref >= limit. Full limit-order-book realism is a
  /// deferred residual (see phase-2 ledger); this is the minimal marketable check.
  [[nodiscard]] static bool limit_marketable(const OrderPayload &order, atx::f64 ref) noexcept {
    switch (order.type) {
    case OrderType::Market:
      return true;
    case OrderType::Limit: {
      const atx::f64 limit = order.limit.to_double();
      return is_buy(order.qty) ? (ref <= limit) : (ref >= limit);
    }
    }
    return false; // unreachable for a valid OrderType (bit-corrupted cast sentinel)
  }

  /// Shares fillable on this slice: min(|open_qty|, max(0, vlim*bar_vol - already
  /// filled this bar)), as an integer (truncated toward zero — never over-fill the
  /// cap). Accumulates the granted amount into the per-bar tally.
  [[nodiscard]] atx::i64 volume_capped_qty(const OrderPayload &order,
                                           const Market &market) noexcept {
    const atx::f64 bar_vol = market.bar_volume(order.id);
    const atx::f64 already = vol_filled_for(order.id);
    const atx::f64 budget = cap_cfg_.volume_limit * bar_vol - already;
    if (budget <= 0.0) {
      return 0; // zero bar volume, or this instrument's per-bar cap is exhausted
    }
    const atx::i64 open_mag = abs_i64(order.qty);
    // Truncate the f64 budget to whole shares; never round up past the cap.
    const atx::i64 budget_shares = static_cast<atx::i64>(budget);
    const atx::i64 fillable = (open_mag < budget_shares) ? open_mag : budget_shares;
    if (fillable > 0) {
      add_vol_filled(order.id, static_cast<atx::f64>(fillable));
    }
    return fillable;
  }

  /// Price the fill (slippage + temporary impact), apply permanent impact to the
  /// mark, compute the commission, and append the FillPayload to the scratch buffer.
  void emit_fill(const OrderPayload &order, atx::i64 fillable, atx::f64 ref,
                 atx::core::time::Timestamp now, Market &market) noexcept {
    const int dir = is_buy(order.qty) ? 1 : -1;
    const InstrumentStats &st = market.stats(order.id);
    const atx::f64 bar_vol = market.bar_volume(order.id);
    const atx::f64 part = (st.adv > 0.0) ? static_cast<atx::f64>(fillable) / st.adv : 0.0;

    const atx::f64 slip = slippage_fraction(fillable, ref, bar_vol, st);
    const atx::f64 temp = temporary_impact(part, st);

    // Both are fractional adverse moves on ref: a buy (dir +1) pays more, a sell
    // (dir -1) receives less. Compose multiplicatively in fraction-of-ref units.
    const atx::f64 fill_px =
        ref * (1.0 + static_cast<atx::f64>(dir) * slip) * (1.0 + static_cast<atx::f64>(dir) * temp);

    apply_permanent_impact(order.id, part, ref, dir, st, market);

    const atx::f64 fee = commission(fillable, fill_px);

    FillPayload f{};
    f.id = order.id;
    f.qty = static_cast<atx::i64>(dir) * fillable; // restore signed direction
    f.price = to_decimal_price(fill_px);
    f.fee = to_decimal_fee(fee);
    f.impact = temp; // temporary-impact fraction recorded for cost attribution
    f.t = now;
    fills_.push_back(f);
  }

  /// Slippage as a fraction of ref, floored at half the spread (always cross the
  /// spread). VolumeShare: k*share², share=fillable/bar_vol, capped at cap_volshare.
  /// FixedBps: bps/1e4, capped at cap_bps. Exhaustive switch (no `default`).
  /// `bar_vol` is the current bar's volume (the VolumeShare denominator).
  [[nodiscard]] atx::f64 slippage_fraction(atx::i64 fillable, atx::f64 ref, atx::f64 bar_vol,
                                           const InstrumentStats &st) const noexcept {
    atx::f64 frac = 0.0;
    switch (slip_cfg_.mode) {
    case SlippageMode::VolumeShare: {
      const atx::f64 share = (bar_vol > 0.0) ? static_cast<atx::f64>(fillable) / bar_vol : 0.0;
      frac = atx::core::clamp(slip_cfg_.k * share * share, 0.0, slip_cfg_.cap_volshare);
      break;
    }
    case SlippageMode::FixedBps:
      frac = atx::core::clamp(slip_cfg_.bps / 1e4, 0.0, slip_cfg_.cap_bps);
      break;
    }
    // Spread floor: always cross at least half the spread (as a fraction of ref).
    const atx::f64 half_spread_frac = (ref > 0.0) ? (st.spread * 0.5) / ref : 0.0;
    return (frac > half_spread_frac) ? frac : half_spread_frac;
  }

  /// Temporary impact fraction: Y * sigma * part^delta. Zero when part is zero
  /// (no ADV configured, or zero fill) so an unconfigured book charges no impact.
  [[nodiscard]] atx::f64 temporary_impact(atx::f64 part, const InstrumentStats &st) const noexcept {
    if (part <= 0.0) {
      return 0.0;
    }
    return impact_cfg_.Y * st.sigma * std::pow(part, impact_cfg_.delta);
  }

  /// Permanent impact: shift the mark by ref * (0.5*gamma*sigma*part) * sign. Only
  /// applied when the move is non-zero and the mark is priced (shift_mark asserts
  /// a priced mark; we already gated on a non-NaN ref in settle_one).
  void apply_permanent_impact(InstrumentId id, atx::f64 part, atx::f64 ref, int dir,
                              const InstrumentStats &st, Market &market) const noexcept {
    if (part <= 0.0) {
      return; // no participation -> no permanent footprint
    }
    const atx::f64 perm = 0.5 * impact_cfg_.gamma * st.sigma * part;
    const atx::f64 delta = ref * perm * static_cast<atx::f64>(dir);
    if (delta != 0.0) {
      market.shift_mark(id, delta);
    }
  }

  /// Commission (>= 0), exact at the Decimal boundary by the caller. PerShare:
  /// clamp(max(|qty|*per_share, min_fee), 0, max_pct*notional). PerDollar:
  /// |notional|*per_dollar_bps/1e4. notional = |fillable| * fill_px. Exhaustive
  /// switch (no `default`).
  [[nodiscard]] atx::f64 commission(atx::i64 fillable, atx::f64 fill_px) const noexcept {
    const atx::f64 shares = static_cast<atx::f64>(fillable);
    const atx::f64 notional = shares * fill_px; // fillable >= 0, fill_px > 0 => >= 0
    switch (comm_cfg_.mode) {
    case CommissionMode::PerShare: {
      const atx::f64 raw = shares * comm_cfg_.per_share;
      const atx::f64 floored = (raw > comm_cfg_.min_fee) ? raw : comm_cfg_.min_fee;
      const atx::f64 cap = comm_cfg_.max_pct * notional;
      const atx::f64 fee = (floored < cap) ? floored : cap; // cap dominates floor
      return (fee > 0.0) ? fee : 0.0;
    }
    case CommissionMode::PerDollar: {
      const atx::f64 fee = notional * comm_cfg_.per_dollar_bps / 1e4;
      return (fee > 0.0) ? fee : 0.0;
    }
    }
    return 0.0; // unreachable for a valid CommissionMode (bit-corrupted sentinel)
  }

  // ---- Decimal money boundary -----------------------------------------------

  /// Convert an f64 fill price to exact Decimal. A finite positive price always
  /// converts; value_or supplies a defensive floor for the (unreachable) failure.
  [[nodiscard]] static atx::core::Decimal to_decimal_price(atx::f64 px) noexcept {
    // SAFETY: px derives from a finite mark * finite fractional factors, so it is
    // finite and positive in range; from_double can only fail on NaN/inf/range,
    // none of which arise here. The value_or(0) is a defensive sentinel.
    return atx::core::Decimal::from_double(px).value_or(atx::core::Decimal{});
  }

  /// Convert an f64 fee (>= 0) to exact Decimal. Same boundary discipline.
  [[nodiscard]] static atx::core::Decimal to_decimal_fee(atx::f64 fee) noexcept {
    return atx::core::Decimal::from_double(fee).value_or(atx::core::Decimal{});
  }

  // ---- per-bar volume accumulator (deterministic, reset-per-bar) -------------

  /// Reset the per-instrument filled-volume tally when the bar advances. The
  /// volume cap is a PER-BAR budget; a new slice (new `now`) starts each
  /// instrument's budget fresh.
  void reset_vol_accumulator_if_new_bar(atx::core::time::Timestamp now) noexcept {
    if (!bar_seen_ || now.unix_nanos() != current_bar_ns_) {
      vol_for_bar_.clear();
      current_bar_ns_ = now.unix_nanos();
      bar_seen_ = true;
    }
  }

  /// Volume already filled for `id` on the current bar (0 if untouched). Linear
  /// scan over the small reset-per-bar tally — deterministic, no hash container.
  [[nodiscard]] atx::f64 vol_filled_for(InstrumentId id) const noexcept {
    for (const VolAccum &a : vol_for_bar_) {
      if (a.id == id) {
        return a.filled;
      }
    }
    return 0.0;
  }

  /// Accumulate `shares` into `id`'s current-bar filled-volume tally.
  void add_vol_filled(InstrumentId id, atx::f64 shares) noexcept {
    for (VolAccum &a : vol_for_bar_) {
      if (a.id == id) {
        a.filled += shares;
        return;
      }
    }
    vol_for_bar_.push_back(VolAccum{id, shares});
  }

  // ---- small helpers --------------------------------------------------------

  /// True for a buy (qty > 0); a sell has qty < 0 (zero-qty never reaches here).
  [[nodiscard]] static bool is_buy(atx::i64 qty) noexcept { return qty > 0; }

  /// |x| for an i64 share count. Counts are bounded well below INT64_MAX in any
  /// realistic universe, so the negate is safe (matches Portfolio::abs_i64).
  [[nodiscard]] static atx::i64 abs_i64(atx::i64 x) noexcept { return (x < 0) ? -x : x; }

  // ---- config (held by value) -----------------------------------------------
  FillCfg fill_cfg_{};
  SlippageCfg slip_cfg_{};
  ImpactCfg impact_cfg_{};
  CommissionCfg comm_cfg_{};
  LatencyCfg latency_cfg_{};
  VolumeCapCfg cap_cfg_{};

  // ---- state ----------------------------------------------------------------
  std::vector<OrderPayload> open_{};    // open set, FIFO order (reserved once)
  std::vector<FillPayload> fills_{};    // per-call scratch (cleared, not freed)
  std::vector<VolAccum> vol_for_bar_{}; // per-(instrument, current bar) tally
  atx::i64 current_bar_ns_{0};          // the bar the tally currently covers
  bool bar_seen_{false};                // false until the first settle_pending
};

} // namespace atx::engine::exec
