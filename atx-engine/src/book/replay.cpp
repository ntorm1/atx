#include "atx/engine/book/replay.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <limits>
#include <memory>
#include <string>
#include <utility>

#include "atx/engine/alpha/panel.hpp"
#include "atx/engine/book/borrow_schedule.hpp"

namespace atx::engine::book {
namespace {

using atx::core::Err;
using atx::core::ErrorCode;
using atx::core::Ok;
using atx::core::Result;
using atx::core::Status;

constexpr atx::f64 kNanosPerDay = 86'400'000'000'000.0;
constexpr atx::f64 kBpsToFraction = 1.0e-4;

std::string location(atx::usize period, atx::usize instrument) {
  return " at period=" + std::to_string(period) + " instrument=" +
         std::to_string(instrument);
}

std::string at_period(atx::usize period) { return " at period=" + std::to_string(period); }

// Bitwise f64 identity: an admitted basis price must be the SAME close the
// valuation used, not a value that merely compares equal after rounding.
bool same_bits(atx::f64 left, atx::f64 right) noexcept {
  return std::bit_cast<atx::u64>(left) == std::bit_cast<atx::u64>(right);
}

Status require_finite(atx::f64 value, const std::string &description) {
  if (!std::isfinite(value)) {
    return Err(ErrorCode::OutOfRange, "replay: nonfinite " + description);
  }
  return Ok();
}

Status require_nav(atx::f64 nav, atx::usize period) {
  if (!std::isfinite(nav) || nav <= 0.0) {
    return Err(ErrorCode::OutOfRange,
               "replay: nonfinite/nonpositive NAV at period=" + std::to_string(period));
  }
  return Ok();
}

Status require_reconciled(atx::f64 actual, atx::f64 expected, atx::usize period,
                          atx::usize instruments) {
  ATX_TRY_VOID(require_finite(expected, "expected accounting NAV"));
  // Allow rounding in ordered reductions, relative to NAV rather than gross
  // holdings: enormous offsetting positions must not hide a material cash loss.
  // This tolerance only rejects a result; it never clips positions or changes NAV.
  const auto relative = 64.0 * std::numeric_limits<atx::f64>::epsilon() *
                        (static_cast<atx::f64>(instruments) + 1.0);
  const auto tolerance = relative * std::max(std::abs(actual), std::abs(expected));
  if (std::abs(actual - expected) > tolerance) {
    return Err(ErrorCode::OutOfRange,
               "replay: cash/assets/P&L do not reconcile at period=" + std::to_string(period));
  }
  return Ok();
}

Result<atx::f64> elapsed_days(atx::i64 start, atx::i64 end) {
  if (end <= start) {
    return Err(ErrorCode::InvalidArgument, "replay: session keys must strictly increase");
  }
  // Ordered signed endpoints can straddle zero. Unsigned subtraction is defined
  // modulo 2^64 and equals their mathematical positive difference; check before
  // accepting a duration that the rest of the engine can represent as i64 ns.
  const auto nanos = static_cast<atx::u64>(end) - static_cast<atx::u64>(start);
  if (nanos > static_cast<atx::u64>(std::numeric_limits<atx::i64>::max())) {
    return Err(ErrorCode::OutOfRange, "replay: elapsed duration exceeds i64 nanoseconds");
  }
  return Ok(static_cast<atx::f64>(nanos) / kNanosPerDay);
}

Status validate_target(const alpha::Panel &panel, atx::usize decision,
                        std::span<const atx::f64> weights) {
  if (weights.size() != panel.instruments()) {
    return Err(ErrorCode::InvalidArgument, "replay: invalid allocated target shape");
  }
  for (atx::usize i = 0; i < weights.size(); ++i) {
    if (!std::isfinite(weights[i]) || (weights[i] != 0.0 && !panel.in_universe(decision, i))) {
      return Err(ErrorCode::InvalidArgument,
                 "replay: nonfinite or ineligible target" + location(decision, i));
    }
  }
  return Ok();
}

// Opt-in ReplayConfig fields (cost model, liquidity, borrow schedule, delisting).
// Every default leaves the historical path untouched.
Status validate_cost_model(const ReplayConfig &cfg, atx::usize cells) {
  if (cfg.cost_model == nullptr) return Ok();
  if (cfg.trade_bps != 0.0) {
    return Err(ErrorCode::InvalidArgument, "replay: cost_model and trade_bps are exclusive");
  }
  const auto rate = cfg.cost_model->proportional_rate();
  if (rate.has_value() && (!std::isfinite(*rate) || *rate < 0.0)) {
    return Err(ErrorCode::InvalidArgument, "replay: invalid proportional cost rate");
  }
  if (cfg.cost_model->needs_liquidity() && cfg.liquidity.size() != cells) {
    return Err(ErrorCode::InvalidArgument, "replay: liquidity shape mismatch");
  }
  return Ok();
}

Status validate_listing_exchange(const ReplayConfig &cfg, atx::usize instruments) {
  if (cfg.listing_exchange.empty()) return Ok();
  if (cfg.listing_exchange.size() != instruments) {
    return Err(ErrorCode::InvalidArgument, "replay: listing_exchange shape mismatch");
  }
  for (const auto exchange : cfg.listing_exchange) {
    if (exchange != ListingExchange::Unknown && exchange != ListingExchange::NyseAmex &&
        exchange != ListingExchange::Nasdaq) {
      return Err(ErrorCode::InvalidArgument, "replay: unrecognized listing exchange");
    }
  }
  return Ok();
}

Status validate_delistings(const ReplayConfig &cfg, atx::usize dates, atx::usize instruments) {
  if (cfg.delisting_policy != DelistingPolicy::Abort &&
      cfg.delisting_policy != DelistingPolicy::CrspDelistReturn &&
      cfg.delisting_policy != DelistingPolicy::LastMarkZeroReturn &&
      cfg.delisting_policy != DelistingPolicy::TerminalReturn &&
      cfg.delisting_policy != DelistingPolicy::TerminalReturnExPostV1) {
    return Err(ErrorCode::InvalidArgument, "replay: unrecognized delisting policy");
  }
  ATX_TRY_VOID(validate_listing_exchange(cfg, instruments));
  std::vector<atx::u8> seen(cfg.delistings.empty() ? 0 : instruments, atx::u8{0});
  for (const auto &event : cfg.delistings) {
    if (event.instrument >= instruments || event.last_valid_period >= dates ||
        seen[event.instrument] != 0) {
      return Err(ErrorCode::InvalidArgument, "replay: invalid or duplicate delisting event");
    }
    seen[event.instrument] = 1;
    // TerminalReturn reads NaN as "unknown" (the flagged Shumway fallback);
    // every policy rejects an infinite return or one below -100 %.
    const bool terminal = cfg.delisting_policy == DelistingPolicy::TerminalReturn ||
                          cfg.delisting_policy == DelistingPolicy::TerminalReturnExPostV1;
    const bool unknown_ok = terminal &&
                            std::isnan(event.delist_return);
    const bool invalid = !unknown_ok && (!std::isfinite(event.delist_return) ||
                                         event.delist_return < -1.0);
    const bool checked = cfg.delisting_policy == DelistingPolicy::CrspDelistReturn || terminal;
    if (checked && invalid) {
      return Err(ErrorCode::InvalidArgument,
                 "replay: missing or invalid delisting return for instrument=" +
                     std::to_string(event.instrument));
    }
  }
  return Ok();
}

Status validate_extensions(const ReplayConfig &cfg, atx::usize dates, atx::usize instruments) {
  ATX_TRY_VOID(validate_cost_model(cfg, dates * instruments));
  if (cfg.borrow_schedule != nullptr) {
    if (cfg.annual_borrow_bps != 0.0) {
      return Err(ErrorCode::InvalidArgument,
                 "replay: borrow_schedule and annual_borrow_bps are exclusive");
    }
    ATX_TRY_VOID(cfg.borrow_schedule->validate(dates, instruments));
  }
  return validate_delistings(cfg, dates, instruments);
}

// The claims-aware entry point owns terminal events. The default
// TerminalReturn policy is admitted with an empty table and exchange list and
// is run with Abort semantics there (see replay_scheduled_intents_with_events).
bool uses_conflicting_delisting_policy(const ReplayConfig &cfg) noexcept {
  const bool policy_ok = cfg.delisting_policy == DelistingPolicy::Abort ||
                         cfg.delisting_policy == DelistingPolicy::TerminalReturn;
  return !policy_ok || !cfg.delistings.empty() || !cfg.listing_exchange.empty();
}

Status validate_inputs(const alpha::Panel &panel, std::span<const atx::i64> times,
                       std::span<const atx::usize> decisions,
                       std::span<const atx::f64> weights, const ReplayConfig &cfg) {
  const auto dates = panel.dates();
  const auto instruments = panel.instruments();
  const auto maximum = std::numeric_limits<atx::usize>::max();
  if (dates == 0 || instruments == 0 || dates > maximum / instruments ||
      times.size() != dates || decisions.size() > maximum / instruments ||
      weights.size() != decisions.size() * instruments) {
    return Err(ErrorCode::InvalidArgument, "replay: invalid panel, time, or target shape");
  }
  ATX_TRY_VOID(require_nav(cfg.initial_nav, 0));
  if (!std::isfinite(cfg.trade_bps) || cfg.trade_bps < 0.0 ||
      !std::isfinite(cfg.annual_borrow_bps) || cfg.annual_borrow_bps < 0.0 ||
      (cfg.borrow_day_basis != ReplayDayBasis::D360 &&
       cfg.borrow_day_basis != ReplayDayBasis::D365)) {
    return Err(ErrorCode::InvalidArgument, "replay: invalid cost rate or day basis");
  }
  // B-02: a zero delay fills at the decision close, the same close any signal
  // computed at that decision has already seen. Only an explicit opt-in admits it.
  if (cfg.execution_delay_periods == 0 && !cfg.allow_same_close) {
    return Err(ErrorCode::InvalidArgument,
               "replay: execution_delay_periods=0 fills at the decision close; set "
               "allow_same_close to opt in");
  }
  if (cfg.locate_breach != LocateBreach::AbortV1 && cfg.locate_breach != LocateBreach::ClipV2) {
    return Err(ErrorCode::InvalidArgument, "replay: unrecognized locate breach rule");
  }
  ATX_TRY_VOID(validate_extensions(cfg, dates, instruments));
  for (atx::usize d = 1; d < dates; ++d) {
    ATX_TRY(auto days, elapsed_days(times[d - 1], times[d]));
    (void)days;
  }
  for (atx::usize s = 0; s < decisions.size(); ++s) {
    const auto d = decisions[s];
    if (d >= dates || (s > 0 && d <= decisions[s - 1])) {
      return Err(ErrorCode::InvalidArgument, "replay: invalid or unordered decision schedule");
    }
    if (d > maximum - cfg.execution_delay_periods) {
      return Err(ErrorCode::OutOfRange, "replay: delayed decision index overflows");
    }
    ATX_TRY_VOID(validate_target(panel, d, weights.subspan(s * instruments, instruments)));
  }
  return Ok();
}

struct MarkedPortfolio {
  atx::f64 assets{};
  atx::f64 gross{};
  atx::f64 shorts{};
};

Status add_mark(MarkedPortfolio &marked, atx::f64 dollars) {
  marked.assets += dollars;
  marked.gross += std::abs(dollars);
  if (dollars < 0.0) marked.shorts -= dollars;
  ATX_TRY_VOID(require_finite(marked.assets, "signed asset sum"));
  ATX_TRY_VOID(require_finite(marked.gross, "gross asset sum"));
  return require_finite(marked.shorts, "short asset sum");
}

Result<atx::f64> required_price(std::span<const atx::f64> close, atx::usize instruments,
                               atx::usize period, atx::usize instrument) {
  const auto price = close[period * instruments + instrument];
  if (!std::isfinite(price) || price <= 0.0) {
    return Err(ErrorCode::InvalidArgument,
               "replay: missing/nonpositive required close" + location(period, instrument));
  }
  return Ok(price);
}

// `carried` is empty for every non-claims path. A nonzero byte marks an
// instrument the caller admitted under CarryLastAccountedValueV1 — the
// predecessor of an event in THIS observation's batch — which is valued at
// `carry_period` (always `period - 1`) instead of at `period`. Every other held
// instrument still requires a valid close at `period`, so an event never
// rescues a missing mark for a name it does not retire.
// `carry_from` is the delisting seam's per-name equivalent (empty unless a
// delisting policy is active; never used together with `carried`): an entry
// other than kNoCarry values that held name at the given earlier period, its
// last valid close — the pending liquidation of a delisted name, or a name
// carried over an interior gap under DelistingPolicy::TerminalReturn.
constexpr atx::usize kNoCarry = std::numeric_limits<atx::usize>::max();

Result<MarkedPortfolio> mark_holdings(std::span<const atx::f64> units,
                                      std::span<const atx::f64> close, atx::usize period,
                                      std::span<atx::f64> values,
                                      std::span<const atx::u8> carried = {},
                                      atx::usize carry_period = 0,
                                      std::span<const atx::usize> carry_from = {}) {
  MarkedPortfolio marked;
  for (atx::usize i = 0; i < units.size(); ++i) {
    values[i] = 0.0;
    if (units[i] == 0.0) continue; // Unused missing prices have no economic effect.
    auto valued_at = (!carried.empty() && carried[i] != 0) ? carry_period : period;
    if (!carry_from.empty() && carry_from[i] != kNoCarry) valued_at = carry_from[i];
    ATX_TRY(auto price, required_price(close, units.size(), valued_at, i));
    values[i] = units[i] * price;
    if (!std::isfinite(values[i]) || values[i] == 0.0) {
      return Err(ErrorCode::OutOfRange,
                 "replay: held value overflow/underflow" + location(valued_at, i));
    }
    ATX_TRY_VOID(add_mark(marked, values[i]));
  }
  return Ok(marked);
}

// Re-totals an already-valued book. Only the claims path needs it: an atomic
// transition commit rewrites two coordinates of `values` after mark_holdings
// has run, and the interval's exposure must reflect the post-event book.
Result<MarkedPortfolio> total_marked(std::span<const atx::f64> values) {
  MarkedPortfolio marked;
  for (const auto value : values) {
    if (value == 0.0) continue;
    ATX_TRY_VOID(add_mark(marked, value));
  }
  return Ok(marked);
}

struct AppliedTarget {
  MarkedPortfolio marked;
  atx::f64 traded_dollars{};
  atx::f64 trade_cost{};
};

// Opt-in per-name execution terms. Default-constructed == the historical path:
// no per-name model (the aggregate trade_rate applies), no working orders, no
// locate check and no unfillable-target skip.
struct TradeContext {
  const ReplayCostModel *model{};          // Per-name model; null == aggregate rate.
  std::span<const LiquidityRow> liquidity; // This period's N rows, or empty.
  std::span<atx::f64> working;             // N goal TRI units; NaN == no working order.
  const BorrowSchedule *borrow{};
  LocateBreach locate_rule{LocateBreach::AbortV1};
  std::vector<ReplayLocateClip> *clips{};  // Required when locate_rule == ClipV2.
  // TerminalReturn: a nonzero target on an unheld name with no valid close is
  // recorded here and left in cash instead of failing the replay.
  std::vector<ReplayUnfilledTarget> *unfilled{};
  // TerminalReturn: N indices into *gaps (kNoCarry == not carried at this
  // period). A carried held name is not traded; its row records the block.
  std::span<const atx::usize> gap_record;
  std::vector<ReplayGapCarry> *gaps{};
};

constexpr atx::f64 kNoWorkingOrder = std::numeric_limits<atx::f64>::quiet_NaN();

struct PricedFill {
  atx::f64 tri_units{};
  atx::f64 marked_dollars{};
  atx::f64 dollar_delta{};
  atx::f64 cost{};
  bool residual{};
};

// Prices one requested move toward `goal_units` through the per-name model. A
// complete fill lands exactly on the goal (the same representable units the
// unconstrained replay would hold); a partial fill adds filled/price units.
Result<PricedFill> priced_fill(const TradeContext &ctx, atx::usize period, atx::usize i,
                               atx::f64 requested, atx::f64 price, atx::f64 units,
                               atx::f64 value, atx::f64 goal_units, atx::f64 goal_value) {
  const auto row = ctx.liquidity.empty() ? LiquidityRow{} : ctx.liquidity[i];
  const auto priced = ctx.model->cost(i, period, requested, row);
  const bool sign_ok = priced.filled_dollars == 0.0 ||
                       std::signbit(priced.filled_dollars) == std::signbit(requested);
  if (!std::isfinite(priced.filled_dollars) || !std::isfinite(priced.cost_dollars) ||
      priced.cost_dollars < 0.0 || !sign_ok ||
      std::abs(priced.filled_dollars) > std::abs(requested)) {
    return Err(ErrorCode::OutOfRange, "replay: invalid cost model fill" + location(period, i));
  }
  PricedFill fill{units, value, 0.0, priced.cost_dollars, true};
  if (priced.filled_dollars == requested) {
    fill = PricedFill{goal_units, goal_value, goal_value - value, priced.cost_dollars, false};
  } else if (priced.filled_dollars != 0.0) {
    fill.tri_units = units + priced.filled_dollars / price;
    fill.marked_dollars = fill.tri_units * price;
    fill.dollar_delta = fill.marked_dollars - value;
    if (!std::isfinite(fill.marked_dollars) || !std::isfinite(fill.dollar_delta)) {
      return Err(ErrorCode::OutOfRange, "replay: partial fill overflow" + location(period, i));
    }
  }
  return Ok(fill);
}

// LocateBreach::AbortV1: a trade may not grow a short beyond its locate;
// carrying or covering is free. Checked on the executed fill (pre-W0 order).
Status check_locate(const TradeContext &ctx, atx::usize period, atx::usize i,
                    atx::f64 value_before, atx::f64 value_after) {
  if (ctx.borrow == nullptr || ctx.locate_rule != LocateBreach::AbortV1 ||
      value_after >= 0.0 || value_after >= value_before) {
    return Ok();
  }
  if (-value_after > ctx.borrow->locate(i, period)) {
    return Err(ErrorCode::InvalidArgument, "replay: short exceeds locate" + location(period, i));
  }
  return Ok();
}

struct LocateGoal {
  atx::f64 units{};
  atx::f64 value{};
  bool clipped{};
};

// LocateBreach::ClipV2 (B-04): the post-trade goal is clipped BEFORE pricing, so
// a cost model prices only the locatable trade. A short may grow to the locate;
// a carried short that already exceeds a shrunken locate is held, never grown
// and never force-covered. Each clip is recorded. `price` is the valid
// execution close whenever the goal is nonzero.
Result<LocateGoal> locate_goal(const TradeContext &ctx, atx::usize period, atx::usize i,
                               atx::f64 units_before, atx::f64 value_before,
                               atx::f64 goal_units, atx::f64 goal_value, atx::f64 price) {
  LocateGoal goal{goal_units, goal_value, false};
  if (ctx.borrow == nullptr || ctx.locate_rule != LocateBreach::ClipV2 || goal_value >= 0.0 ||
      goal_value >= value_before) {
    return Ok(goal);
  }
  const auto locate = ctx.borrow->locate(i, period);
  if (-goal_value <= locate) return Ok(goal);
  const auto allowed = std::min(value_before, -locate);
  if (allowed == value_before) {
    goal = LocateGoal{units_before, value_before, true};
  } else if (allowed == 0.0) {
    goal = LocateGoal{0.0, 0.0, true}; // Flat, as +0 (no signed-zero holding).
  } else {
    const auto units = allowed / price;
    const auto value = units * price;
    if (!std::isfinite(units) || !std::isfinite(value) || units == 0.0 || value == 0.0) {
      return Err(ErrorCode::OutOfRange, "replay: locate clip overflow/underflow" +
                                            location(period, i));
    }
    goal = LocateGoal{units, value, true};
  }
  ctx.clips->push_back(ReplayLocateClip{period, i, goal_value, goal.value, locate});
  return Ok(goal);
}

// Books one executed move: cash, traded dollars and the trade record.
Status book_move(atx::f64 delta, atx::usize period, atx::usize decision, atx::usize i,
                 AppliedTarget &applied, atx::f64 &cash, std::vector<ReplayTrade> &trades) {
  ATX_TRY_VOID(require_finite(delta, "trade delta" + location(period, i)));
  if (delta == 0.0) return Ok();
  cash -= delta;
  applied.traded_dollars += std::abs(delta);
  ATX_TRY_VOID(require_finite(cash, "cash after trade" + location(period, i)));
  ATX_TRY_VOID(require_finite(applied.traded_dollars, "traded dollar sum"));
  trades.push_back(ReplayTrade{period, decision, i, delta});
  return Ok();
}

// Re-attempts every open working order at a period with no new decision. A name
// without a valid mark keeps its order open; nothing is traded blind.
Result<AppliedTarget> work_residuals(const TradeContext &ctx, std::span<const atx::f64> close,
                                     atx::usize period, atx::usize decision, atx::f64 nav,
                                     std::span<atx::f64> units, std::span<atx::f64> values,
                                     atx::f64 &cash, std::vector<ReplayTrade> &trades,
                                     atx::f64 signed_pending_claims) {
  AppliedTarget applied;
  atx::f64 model_cost = 0.0;
  for (atx::usize i = 0; i < units.size(); ++i) {
    const auto goal = ctx.working[i];
    if (std::isnan(goal)) continue;
    const auto price = close[period * units.size() + i];
    if (!std::isfinite(price) || price <= 0.0) continue;
    // ClipV2 amends the order to what is locatable now (no-op otherwise).
    ATX_TRY(const auto target, locate_goal(ctx, period, i, units[i], values[i], goal,
                                           goal * price, price));
    if (target.clipped) ctx.working[i] = target.units;
    const auto goal_value = target.value;
    const auto requested = goal_value - values[i];
    ATX_TRY_VOID(require_finite(requested, "working order" + location(period, i)));
    if (requested == 0.0) {
      ctx.working[i] = kNoWorkingOrder;
      continue;
    }
    ATX_TRY(const auto fill, priced_fill(ctx, period, i, requested, price, units[i], values[i],
                                         target.units, goal_value));
    ATX_TRY_VOID(check_locate(ctx, period, i, values[i], fill.marked_dollars));
    ATX_TRY_VOID(book_move(fill.dollar_delta, period, decision, i, applied, cash, trades));
    model_cost += fill.cost;
    units[i] = fill.tri_units;
    values[i] = fill.marked_dollars;
    if (!fill.residual) ctx.working[i] = kNoWorkingOrder;
  }
  ATX_TRY(applied.marked, total_marked(values));
  applied.trade_cost = model_cost;
  ATX_TRY_VOID(require_finite(applied.trade_cost, "trade cost"));
  cash -= applied.trade_cost;
  ATX_TRY_VOID(require_finite(cash, "cash after trade cost"));
  const auto accounted = cash + applied.marked.assets + signed_pending_claims;
  ATX_TRY_VOID(require_nav(accounted, period));
  ATX_TRY_VOID(require_reconciled(accounted, nav - applied.trade_cost, period, units.size()));
  return Ok(applied);
}

Result<AppliedTarget> apply_target(std::span<const atx::f64> weights,
                                   std::span<const ReplayTargetIntent> intents,
                                   std::span<const atx::u8> eligibility,
                                   std::span<atx::f64> resolved_weights,
                                   std::span<const atx::f64> close, atx::usize period,
                                   atx::usize decision, atx::f64 nav, atx::f64 trade_rate,
                                   std::span<atx::f64> units, std::span<atx::f64> values,
                                   atx::f64 &cash, std::vector<ReplayTrade> &trades,
                                   atx::f64 signed_pending_claims = 0.0,
                                   const TradeContext &ctx = {}) {
  AppliedTarget applied;
  atx::f64 model_cost = 0.0;
  for (atx::usize i = 0; i < weights.size(); ++i) {
    const auto intent = intents.empty()
        ? ReplayTargetIntent{ReplayTargetAction::TargetWeight, weights[i]} : intents[i];
    const auto price = close[period * units.size() + i];
    const bool eligible = eligibility.empty() || eligibility[i] != 0;
    // TerminalReturn: an eligible nonzero target on an unheld name that has no
    // valid close at execution (it left the panel after the decision) cannot
    // trade. It stays in cash and is reported; nothing is traded blind.
    if (ctx.unfilled != nullptr && units[i] == 0.0 && eligible &&
        intent.action == ReplayTargetAction::TargetWeight && std::isfinite(intent.weight) &&
        intent.weight != 0.0 && (!std::isfinite(price) || price <= 0.0)) {
      ctx.unfilled->push_back(ReplayUnfilledTarget{period, decision, i, intent.weight});
      if (!resolved_weights.empty()) resolved_weights[i] = 0.0;
      if (ctx.model != nullptr) ctx.working[i] = kNoWorkingOrder;
      continue;
    }
    // TerminalReturn: a held name carried over an interior gap has no close to
    // trade at. It keeps its units at the carried value (a Hold), the new
    // decision still replaces its open order, and a non-Hold instruction is
    // recorded as blocked on the carry row. The payload is still validated;
    // eligibility is not, because nothing executes (a gap day commonly drops the
    // name from the decision universe, and a Hold there must not abort).
    if (!ctx.gap_record.empty() && ctx.gap_record[i] != kNoCarry) {
      const bool zero_payload = intent.action == ReplayTargetAction::TargetWeight ||
                                intent.weight == 0.0;
      const bool known = intent.action == ReplayTargetAction::TargetWeight ||
                         intent.action == ReplayTargetAction::HoldCurrent ||
                         intent.action == ReplayTargetAction::Close;
      if (!std::isfinite(intent.weight) || !zero_payload || !known) {
        return Err(ErrorCode::InvalidArgument,
                   "replay: invalid instruction on a gap-carried name" + location(period, i));
      }
      if (!resolved_weights.empty()) resolved_weights[i] = values[i] / nav;
      if (ctx.model != nullptr) ctx.working[i] = kNoWorkingOrder;
      if (intent.action != ReplayTargetAction::HoldCurrent) {
        (*ctx.gaps)[ctx.gap_record[i]].trade_blocked = true;
      }
      ATX_TRY_VOID(add_mark(applied.marked, values[i]));
      continue;
    }
    // Older weight paths validated every target's original eligibility already.
    const auto sized = resolve_replay_target(intent, units[i], values[i], price, nav, eligible);
    if (!sized) {
      return Err(sized.error().code(), sized.error().message() + location(period, i));
    }
    if (!resolved_weights.empty()) resolved_weights[i] = sized->resolved_weight;
    // ClipV2 caps a short-growing goal at the locate before it is priced.
    ATX_TRY(const auto goal, locate_goal(ctx, period, i, units[i], values[i], sized->tri_units,
                                         sized->marked_dollars, price));
    // Debit the actually representable new TRI holding, preserving self-financing
    // even when dividing target dollars by price rounds the requested unit count.
    auto fill = PricedFill{goal.units, goal.value,
                           goal.clipped ? goal.value - values[i] : sized->dollar_delta, 0.0,
                           false};
    if (ctx.model != nullptr) {
      ctx.working[i] = kNoWorkingOrder; // A new decision replaces every open order.
      if (fill.dollar_delta != 0.0) {
        ATX_TRY(fill, priced_fill(ctx, period, i, fill.dollar_delta, price, units[i], values[i],
                                  goal.units, goal.value));
        if (fill.residual) ctx.working[i] = goal.units;
      }
    }
    ATX_TRY_VOID(check_locate(ctx, period, i, values[i], fill.marked_dollars));
    ATX_TRY_VOID(book_move(fill.dollar_delta, period, decision, i, applied, cash, trades));
    model_cost += fill.cost;
    units[i] = fill.tri_units;
    values[i] = fill.marked_dollars;
    ATX_TRY_VOID(add_mark(applied.marked, fill.marked_dollars));
  }
  applied.trade_cost = applied.traded_dollars * trade_rate;
  if (ctx.model != nullptr) applied.trade_cost = model_cost;
  ATX_TRY_VOID(require_finite(applied.trade_cost, "trade cost"));
  cash -= applied.trade_cost;
  ATX_TRY_VOID(require_finite(cash, "cash after trade cost"));
  // Claims are constant across the trade by construction, so the post-trade
  // gate is the same identity the pretrade NAV was built from. The term is
  // exactly 0.0 on every non-claims path, which leaves the sum unchanged.
  const auto accounted = cash + applied.marked.assets + signed_pending_claims;
  ATX_TRY_VOID(require_nav(accounted, period));
  ATX_TRY_VOID(require_reconciled(accounted, nav - applied.trade_cost, period, units.size()));
  return Ok(applied);
}

// Net financing under a BorrowSchedule (see borrow_schedule.hpp, B-05).
// FeeAndRebateV1, and a rebate-quoted FeeOnceV2 schedule (whose fees are all
// zero by validation): per-name fees on post-trade marked short dollars, less
// the short rebate, less interest on FREE cash (cash - shorts); the proceeds
// earn only the rebate. A fee-quoted FeeOnceV2 schedule: the fee is the whole
// borrow cost and the proceeds are ordinary settled cash earning the cash rate.
Result<atx::f64> scheduled_financing(const BorrowSchedule &schedule,
                                     std::span<const atx::f64> values, atx::f64 shorts,
                                     atx::f64 cash, atx::usize period, atx::f64 days,
                                     const ReplayConfig &cfg) {
  atx::f64 fee_dollars = 0.0;
  for (atx::usize i = 0; i < values.size(); ++i) {
    if (values[i] < 0.0) fee_dollars += -values[i] * schedule.fee(i, period);
  }
  const bool fee_quoted_once =
      schedule.financing == ShortFinancing::FeeOnceV2 && schedule.is_fee_quote();
  const auto free_cash = cash - shorts;
  const auto bps_dollars = fee_quoted_once
      ? fee_dollars - cash * schedule.cash_bps
      : fee_dollars - shorts * schedule.rebate_bps - free_cash * schedule.cash_bps;
  const auto basis = static_cast<atx::f64>(cfg.borrow_day_basis);
  const auto charge = bps_dollars * ((kBpsToFraction / basis) * days);
  ATX_TRY_VOID(require_finite(charge, "scheduled financing charge"));
  return Ok(charge);
}

Result<atx::f64> borrow_charge(atx::f64 short_dollars, atx::f64 days,
                              const ReplayConfig &cfg) {
  if (short_dollars == 0.0 || cfg.annual_borrow_bps == 0.0) return Ok(0.0);
  const auto basis = static_cast<atx::f64>(cfg.borrow_day_basis);
  const auto rate = cfg.annual_borrow_bps * kBpsToFraction;
  const auto charge = short_dollars * ((rate / basis) * days);
  ATX_TRY_VOID(require_finite(charge, "elapsed borrow charge"));
  return Ok(charge);
}

// ---------------------------------------------------------------------------
// Claims-aware seam (design §4). Everything below runs only when the claims
// entry point supplies a capture; the no-event path never enters it.
// ---------------------------------------------------------------------------

// Owned per replay, never per observation: the ledger alone is tens of
// kilobytes, so the entry point holds it behind one heap allocation made before
// the loop starts. Nothing here allocates again on the per-observation path
// for its ledgers, whose vectors are reserved once to the fixture bounds.
struct ClaimsCapture {
  const ReplayMandatoryEventPolicy *events{};
  ReplayClaimsConfig config{};
  ClaimsBookState state{};
  std::vector<atx::u8> retired;  // N bytes, owned by commit_security_transition.
  std::vector<atx::u8> carried;  // N bytes, the admitted exemption for `batch_period`.
  ReplayEventBatch batch{};
  atx::usize batch_period{kNoInstrument}; // kNoInstrument == no cached batch.
  std::array<PendingTransitionCashClaim, kMaxPendingClaims> claim_scratch{};
  std::vector<MandatoryMovement> movements;
  std::vector<ReplayTransitionOrderCancellation> order_cancellations;
  atx::f64 value_bridge{};       // Per-observation accumulators, reset at each `d`.
  atx::f64 settled_cash_delta{};
  atx::f64 recognized{};
  atx::f64 settled{};
};

void begin_observation(ClaimsCapture &claims, atx::f64 settled_cash) noexcept {
  claims.state.settled_cash = settled_cash;
  claims.value_bridge = 0.0;
  claims.settled_cash_delta = 0.0;
  claims.recognized = 0.0;
  claims.settled = 0.0;
}

// All four conventions are rejectable admissions, never defaults: an Unknown in
// any of them refuses a non-empty batch outright (design §5).
Status require_admitted_conventions(const ReplayClaimsConfig &config) {
  if ((config.predecessor_mark != TransitionPredecessorMark::RequireMarkAtEvent &&
       config.predecessor_mark != TransitionPredecessorMark::CarryLastAccountedValueV1) ||
      config.loan != TransitionLoanTreatment::NoDischargeSuccessorContinuesV1 ||
      config.claim_financing != ClaimFinancing::NoneDisclosedV1 ||
      config.borrow_base != BorrowBase::MarkedShortEquityDollarsV1) {
    return Err(ErrorCode::InvalidArgument,
               "replay: mandatory event conventions must all be explicitly admitted");
  }
  if (config.admission.mode != TransitionMode::SyntheticFixture) {
    return Err(ErrorCode::InvalidArgument,
               "replay: only synthetic fixture transition admission is supported");
  }
  return Ok();
}

Status validate_batch_ids(const ReplayEventBatch &batch, atx::usize period,
                          atx::usize instruments) {
  for (atx::usize i = 0; i < batch.transition_count; ++i) {
    const auto &event = batch.transitions[i].event;
    if (event.predecessor.instrument >= instruments ||
        event.successor.instrument >= instruments) {
      return Err(ErrorCode::InvalidArgument,
                 "replay: transition instrument outside the canonical axis" + at_period(period));
    }
    for (atx::usize j = 0; j < i; ++j) {
      const auto &other = batch.transitions[j].event;
      if (event.event_id == other.event_id || event.cash_claim_id == other.cash_claim_id) {
        return Err(ErrorCode::AlreadyExists,
                   "replay: duplicate event or claim ID inside one batch" + at_period(period));
      }
    }
  }
  for (atx::usize i = 0; i < batch.payment_count; ++i) {
    for (atx::usize j = 0; j < i; ++j) {
      if (batch.payments[i].payment_id == batch.payments[j].payment_id) {
        return Err(ErrorCode::AlreadyExists,
                   "replay: duplicate payment ID inside one batch" + at_period(period));
      }
    }
  }
  return Ok();
}

// Step 0. The batch is a query result, not yet a commit: everything structural
// is refused here so that no partially admitted batch ever reaches valuation.
// There is deliberately NO period-0 / terminal rejection: `request_event_batch`
// never asks the policy outside 1 <= period < dates - 1, so a batch at those
// observations cannot exist to be refused. Design §11 ruling 5 is enforced
// structurally by the caller's guard, not by an unreachable branch here.
Status validate_batch(const ReplayEventBatch &batch, const ReplayClaimsConfig &config,
                      atx::usize period, atx::usize instruments) {
  if (batch.transition_count > kMaxEventsPerObservation ||
      batch.payment_count > kMaxPaymentsPerObservation) {
    return Err(ErrorCode::OutOfRange,
               "replay: mandatory event batch exceeds its per-observation bound" +
                   at_period(period));
  }
  if (batch.transition_count == 0 && batch.payment_count == 0) return Ok();
  ATX_TRY_VOID(require_admitted_conventions(config));
  return validate_batch_ids(batch, period, instruments);
}

// Fetches the batch that observation `period` will apply, one observation early
// so that the valuation of `period` — which happens both as the END mark of the
// previous interval and as the START mark of this one — already knows which
// predecessors are exempt. Ruling 5 keeps the request inside 1 <= period < dates-1.
Status request_event_batch(ClaimsCapture &claims, std::span<const atx::f64> close,
                           std::span<const atx::i64> session_keys, atx::usize period,
                           atx::usize dates, atx::usize instruments,
                           atx::f64 settled_cash, std::span<const atx::f64> units,
                           std::span<const atx::f64> previous_marked) {
  claims.batch = ReplayEventBatch{};
  claims.batch_period = kNoInstrument;
  std::fill(claims.carried.begin(), claims.carried.end(), atx::u8{0});
  if (period == 0 || period + 1 >= dates) return Ok();
  ReplayEventContext context;
  context.period = period;
  context.session_key = session_keys[period];
  context.previous_session_key = session_keys[period - 1];
  context.state_version = claims.state.state_version;
  context.last_applied_sequence = claims.state.last_applied_sequence;
  context.settled_cash = settled_cash;
  context.tri_units = units;
  context.previous_marked_dollars = previous_marked;
  context.current_marks = close.subspan(period * instruments, instruments);
  context.pending_claims = pending_claims_view(claims.state, claims.claim_scratch);
  context.retired_representation = claims.retired;
  ATX_TRY(claims.batch, (*claims.events)(context));
  ATX_TRY_VOID(validate_batch(claims.batch, claims.config, period, instruments));
  claims.batch_period = period;
  if (claims.config.predecessor_mark != TransitionPredecessorMark::CarryLastAccountedValueV1) {
    return Ok();
  }
  for (atx::usize i = 0; i < claims.batch.transition_count; ++i) {
    claims.carried[claims.batch.transitions[i].event.predecessor.instrument] = atx::u8{1};
  }
  return Ok();
}

TransitionHoldingView holding_view(const ClaimsCapture &claims,
                                   const ReplayTransitionRequest &request,
                                   std::span<const atx::i64> session_keys, atx::usize period,
                                   std::span<const atx::f64> units,
                                   std::span<const atx::f64> values) {
  const auto predecessor = request.event.predecessor.instrument;
  const auto successor = request.event.successor.instrument;
  TransitionHoldingView view;
  view.state_version = claims.state.state_version;
  view.last_applied_sequence = claims.state.last_applied_sequence;
  view.predecessor = request.event.predecessor;
  view.successor = request.event.successor;
  view.source_artifact_sha256 = request.basis.source_artifact_sha256;
  view.axes_sha256 = request.basis.axes_sha256;
  view.adjustment_recipe_sha256 = request.basis.adjustment_recipe_sha256;
  view.predecessor_tri_units = units[predecessor];
  view.predecessor_accounted_value = values[predecessor];
  view.successor_tri_units = units[successor];
  view.successor_marked_value = values[successor];
  view.settled_cash = claims.state.settled_cash;
  view.predecessor_retired = claims.retired[predecessor] != 0;
  view.last_valuation_at_ns = session_keys[period - 1];
  view.next_valuation_at_ns = session_keys[period];
  view.applied_events = applied_events(claims.state);
  view.known_cash_claim_ids = known_claim_ids(claims.state);
  return view;
}

// Step 2, one event. The commit is atomic on its own; a rejection after it
// still publishes nothing, because the whole replay is all-or-error.
Status commit_transition(ClaimsCapture &claims, const ReplayTransitionRequest &request,
                         std::span<const atx::f64> close,
                         std::span<const atx::i64> session_keys, atx::usize period,
                         std::span<atx::f64> units, std::span<atx::f64> values) {
  const auto instruments = units.size();
  const auto predecessor = request.event.predecessor.instrument;
  const auto successor = request.event.successor.instrument;
  const bool carried = claims.carried[predecessor] != 0;
  ATX_TRY(const auto successor_mark, required_price(close, instruments, period, successor));
  ATX_TRY(const auto predecessor_mark,
          required_price(close, instruments, carried ? period - 1 : period, predecessor));
  if (!same_bits(request.basis.successor.tri_price, successor_mark) ||
      !same_bits(request.basis.predecessor.tri_price, predecessor_mark)) {
    return Err(ErrorCode::InvalidArgument,
               "replay: transition basis price is not the admitted panel close" +
                   location(period, successor));
  }
  const auto view = holding_view(claims, request, session_keys, period, units, values);
  ATX_TRY(const auto plan, plan_security_transition(request.event, request.basis, view,
                                                    claims.config.admission));
  ATX_TRY(const auto receipt, commit_security_transition(plan, period, units, values,
                                                         claims.retired, claims.state));
  // Re-mark the two touched coordinates from the panel close at `period` and
  // require the committed book to be exactly what the panel says it is worth.
  if (!same_bits(units[successor] * successor_mark, values[successor]) ||
      units[predecessor] != 0.0 || values[predecessor] != 0.0) {
    return Err(ErrorCode::InvalidArgument,
               "replay: committed transition does not re-mark to the plan" +
                   location(period, successor));
  }
  claims.value_bridge += plan.valuation_bridge;
  claims.recognized += plan.signed_cash_claim;
  ATX_TRY_VOID(require_finite(claims.value_bridge, "mandatory valuation bridge"));
  for (atx::usize i = 0; i < receipt.movement_count; ++i) {
    auto &row = claims.state.movements[receipt.first_movement + i];
    // The carried mark is replay's own disclosure, not the commit's: only this
    // layer knows which close the exempt predecessor was actually valued at.
    if (carried && row.kind == MandatoryMovementKind::PredecessorRetirement) {
      row.carried_mark = predecessor_mark;
      row.carried_mark_period = period - 1;
    }
    claims.movements.push_back(row);
  }
  return Ok();
}

Status apply_transitions(ClaimsCapture &claims, std::span<const atx::f64> close,
                         std::span<const atx::i64> session_keys, atx::usize period,
                         std::span<atx::f64> units, std::span<atx::f64> values) {
  for (atx::usize i = 0; i < claims.batch.transition_count; ++i) {
    ATX_TRY_VOID(commit_transition(claims, claims.batch.transitions[i], close, session_keys,
                                   period, units, values));
  }
  return Ok();
}

// Step 7, after the trade: cash recognized and paid at `period` is first
// spendable at `period + 1` (ruling 1). NAV is unchanged across settlement.
Status settle_payments(ClaimsCapture &claims, atx::usize period, atx::f64 marked_equities,
                       atx::usize instruments) {
  for (atx::usize i = 0; i < claims.batch.payment_count; ++i) {
    const auto &payment = claims.batch.payments[i];
    ATX_TRY(const auto before, compute_claims_nav(claims.state, marked_equities));
    const auto pending = pending_claims_view(claims.state, claims.claim_scratch);
    CashClaimPaymentView view;
    bool found = false;
    for (const auto &claim : pending) {
      if (payment.cash_claim_id == 0 || claim.cash_claim_id != payment.cash_claim_id) continue;
      view.claim = claim;
      found = true;
    }
    if (!found) {
      return Err(ErrorCode::NotFound,
                 "replay: payment names no pending cash claim" + at_period(period));
    }
    view.state_version = claims.state.state_version;
    view.last_applied_sequence = claims.state.last_applied_sequence;
    view.settled_cash = claims.state.settled_cash;
    view.applied_payment_ids = applied_payments(claims.state);
    ATX_TRY(const auto plan, plan_cash_claim_payment(payment, view, claims.config.admission));
    ATX_TRY(const auto receipt, commit_cash_claim_payment(plan, period, claims.state));
    ATX_TRY(const auto after, compute_claims_nav(claims.state, marked_equities));
    ATX_TRY_VOID(require_reconciled(after.nav, before.nav, period, instruments));
    claims.settled_cash_delta += plan.actual_cash_delta;
    claims.settled += plan.payment.amount;
    ATX_TRY_VOID(require_finite(claims.settled_cash_delta, "mandatory settled cash"));
    claims.movements.push_back(claims.state.movements[receipt.first_movement]);
  }
  return Ok();
}

// Step 5's added rejection: a delayed predecessor target may not reopen a
// retired representation, whatever its original decision eligibility said.
Status reject_retired_targets(std::span<const ReplayTargetIntent> intents,
                              std::span<const atx::u8> retired, atx::usize period) {
  for (atx::usize i = 0; i < intents.size() && i < retired.size(); ++i) {
    if (retired[i] == 0) continue;
    const bool reopens = intents[i].action == ReplayTargetAction::HoldCurrent ||
                         (intents[i].action == ReplayTargetAction::TargetWeight &&
                          intents[i].weight != 0.0);
    if (reopens) {
      return Err(ErrorCode::InvalidArgument,
                 "replay: target or hold on a retired representation" + location(period, i));
    }
  }
  return Ok();
}

struct PolicyCapture {
  const ReplayAllocationPolicy *policy{};
  const ReplayIntentPolicy *intent_policy{};
  std::vector<ReplayAllocation> allocations;
  std::vector<atx::u8> eligibility;
};

Result<ReplayAllocation> allocate_target(PolicyCapture &capture,
    const alpha::Panel &research, const ReplayAllocationState &state) {
  for (atx::usize i = 0; i < capture.eligibility.size(); ++i) {
    capture.eligibility[i] = research.in_universe(state.decision_period, i) ? 1 : 0;
  }
  ReplayAllocation allocation;
  if (capture.intent_policy != nullptr) {
    ATX_TRY(auto intents, (*capture.intent_policy)(state));
    if (intents.size() != research.instruments()) {
      return Err(ErrorCode::InvalidArgument, "replay: invalid allocated intent shape");
    }
    allocation.target_intents = std::move(intents);
    allocation.target_weights.resize(research.instruments());
  } else {
    ATX_TRY(auto weights, (*capture.policy)(state));
    ATX_TRY_VOID(validate_target(research, state.decision_period, weights));
    allocation.target_weights = std::move(weights);
  }
  allocation.schedule_index = state.schedule_index;
  allocation.decision_period = state.decision_period;
  allocation.execution_period = state.execution_period;
  allocation.decision_session_key = state.decision_session_key;
  allocation.execution_session_key = state.execution_session_key;
  allocation.pretrade_cash = state.cash;
  allocation.pretrade_nav = state.pretrade_nav;
  allocation.pretrade_marked_dollars.assign(state.marked_dollars.begin(),
                                            state.marked_dollars.end());
  return Ok(std::move(allocation));
}

// Replay-owned state for the opt-in ReplayConfig fields. With every field at its
// default, init() allocates nothing and every query below is a no-op, so the
// historical path executes exactly the same arithmetic.
struct ReplayExtensions {
  static constexpr atx::usize kNoEvent = std::numeric_limits<atx::usize>::max();
  const ReplayConfig *config{};
  atx::usize instruments{};
  const ReplayCostModel *per_name_model{}; // Non-proportional cost model.
  std::optional<atx::f64> proportional_rate;
  std::vector<atx::f64> working;         // N goal units; NaN == none.
  std::vector<atx::usize> delist_event;  // N indices into config->delistings.
  std::vector<atx::u8> delisting;        // N mask for the pending valuation.
  std::vector<TerminalReturn> terminal;  // N returns resolved for the pending valuation.
  // N periods of the last valid close a held name is valued at instead of the
  // current one (kNoCarry == its own close): a pending liquidation or, under
  // TerminalReturn, an interior-gap carry. Persists across a multi-session gap.
  std::vector<atx::usize> carry_from;
  // TerminalReturn only: N final valid-close periods (kNoCarry == never prints)
  // and N indices of this valuation's gap_carries row (kNoCarry == none).
  std::vector<atx::usize> last_print;
  std::vector<atx::usize> gap_record;
  bool delisting_pending{};
  bool terminal_policy{};                // DelistingPolicy::TerminalReturn.
  std::vector<ReplayLocateClip> *clips{};
  std::vector<ReplayUnfilledTarget> *unfilled{};
  std::vector<ReplayGapCarry> *gaps{};

  // The "last bar" evidence: the final period with a valid close, per name.
  void scan_last_print(std::span<const atx::f64> close, atx::usize dates) {
    last_print.assign(instruments, kNoCarry);
    for (atx::usize t = 0; t < dates; ++t) {
      for (atx::usize i = 0; i < instruments; ++i) {
        const auto price = close[t * instruments + i];
        if (std::isfinite(price) && price > 0.0) last_print[i] = t;
      }
    }
  }

  void init(const ReplayConfig &cfg, atx::usize n, ReplayResult &result,
            std::span<const atx::f64> close, atx::usize dates) {
    config = &cfg;
    instruments = n;
    if (cfg.cost_model != nullptr) {
      proportional_rate = cfg.cost_model->proportional_rate();
      if (!proportional_rate.has_value()) {
        per_name_model = cfg.cost_model;
        working.assign(n, kNoWorkingOrder);
      }
    }
    terminal_policy = cfg.delisting_policy == DelistingPolicy::TerminalReturn ||
                      cfg.delisting_policy == DelistingPolicy::TerminalReturnExPostV1;
    if (terminal_policy ||
        (cfg.delisting_policy != DelistingPolicy::Abort && !cfg.delistings.empty())) {
      delist_event.assign(n, kNoEvent);
      delisting.assign(n, atx::u8{0});
      terminal.assign(n, TerminalReturn{});
      carry_from.assign(n, kNoCarry);
      for (atx::usize k = 0; k < cfg.delistings.size(); ++k) {
        delist_event[cfg.delistings[k].instrument] = k;
      }
    }
    if (cfg.borrow_schedule != nullptr && cfg.locate_breach == LocateBreach::ClipV2) {
      clips = &result.locate_clips;
    }
    if (terminal_policy) {
      unfilled = &result.unfilled_targets;
      gaps = &result.gap_carries;
      gap_record.assign(n, kNoCarry);
      if (cfg.delisting_policy == DelistingPolicy::TerminalReturnExPostV1) {
        scan_last_print(close, dates);
      }
    }
  }

  [[nodiscard]] atx::f64 trade_rate(const ReplayConfig &cfg) const noexcept {
    return proportional_rate.has_value() ? *proportional_rate : cfg.trade_bps * kBpsToFraction;
  }

  [[nodiscard]] TradeContext context(atx::usize period) {
    TradeContext ctx;
    ctx.borrow = config->borrow_schedule;
    ctx.locate_rule = config->locate_breach;
    ctx.clips = clips;
    ctx.unfilled = unfilled;
    ctx.gap_record = gap_record;
    ctx.gaps = gaps;
    if (per_name_model != nullptr) {
      ctx.model = per_name_model;
      ctx.working = working;
      if (per_name_model->needs_liquidity()) {
        ctx.liquidity = config->liquidity.subspan(period * instruments, instruments);
      }
    }
    return ctx;
  }

  // Open-order count, refreshed after every trading step (recount) and every
  // cancellation, so the per-period has_working_orders() query is O(1). The
  // recount itself only runs on periods that already did O(N) trade work.
  atx::usize open_orders{};
  void recount_working_orders() noexcept {
    open_orders = 0;
    for (const auto goal : working) open_orders += std::isnan(goal) ? 0U : 1U;
  }
  [[nodiscard]] atx::usize open_working_orders() const noexcept { return open_orders; }
  [[nodiscard]] bool has_working_orders() const noexcept { return open_orders != 0; }

  // A transition invalidates goals sized in the old representation, including
  // an existing successor goal whose inventory has just changed. Cancel both
  // coordinates before any residual execution; a fresh decision may size again.
  // Unrelated orders retain their original goals and execution provenance.
  void cancel_transition_orders(const ReplayEventBatch &batch, atx::usize period,
                                std::vector<ReplayTransitionOrderCancellation> &out) {
    if (!has_working_orders()) return;
    for (atx::usize k = 0; k < batch.transition_count; ++k) {
      const auto &event = batch.transitions[k].event;
      for (const auto i : {event.predecessor.instrument, event.successor.instrument}) {
        if (std::isnan(working[i])) continue;
        out.push_back({period, i, event.event_id, working[i]});
        working[i] = kNoWorkingOrder;
        --open_orders;
      }
    }
  }

  // The terminal return of flagged name i whose event (if any) is due or not.
  [[nodiscard]] TerminalReturn resolve_terminal(atx::usize i, bool event_due,
                                                bool is_short) const noexcept {
    switch (config->delisting_policy) {
    case DelistingPolicy::CrspDelistReturn:
      return {config->delistings[delist_event[i]].delist_return, TerminalReturnSource::Table};
    case DelistingPolicy::LastMarkZeroReturn:
      return {0.0, TerminalReturnSource::LastMarkZero};
    default:
      break;
    }
    if (event_due) {
      const auto supplied = config->delistings[delist_event[i]].delist_return;
      if (std::isfinite(supplied)) return {supplied, TerminalReturnSource::Table};
    }
    const auto exchange = config->listing_exchange.empty() ? ListingExchange::Unknown
                                                           : config->listing_exchange[i];
    if (!event_due && config->delisting_policy == DelistingPolicy::TerminalReturn) {
      return assumed_missing_price_return(exchange, is_short);
    }
    return shumway_terminal_return(exchange, is_short);
  }

  // Per-name last-close periods for mark_holdings; empty when no policy is active.
  [[nodiscard]] std::span<const atx::usize> carry_view() const noexcept { return carry_from; }

  // Only the explicit ex-post diagnostic may use future print/event evidence.
  // The default liquidates conservatively at the first missing held valuation.
  [[nodiscard]] bool gap_carried(atx::usize i, atx::usize period, bool event_due) const noexcept {
    if (config->delisting_policy != DelistingPolicy::TerminalReturnExPostV1 || event_due) {
      return false;
    }
    const bool listed_by_table = delist_event[i] != kNoEvent; // Not due == still listed.
    const bool prints_again = last_print[i] != kNoCarry && last_print[i] > period;
    return listed_by_table || prints_again;
  }

  // Classifies every held name with no valid mark at `period`: carried over an
  // interior gap (TerminalReturn, recorded) or flagged for liquidation — under
  // TerminalReturn every other such name, otherwise only names whose event says
  // the final close preceded `period`. Each such name is valued at its last
  // valid close (carry_from). Returns whether any name is flagged for liquidation.
  bool flag_delistings(std::span<const atx::f64> close, atx::usize period,
                       std::span<const atx::f64> units) {
    delisting_pending = false;
    if (delist_event.empty()) return false;
    for (atx::usize i = 0; i < instruments; ++i) {
      delisting[i] = 0;
      if (!gap_record.empty()) gap_record[i] = kNoCarry;
      const auto event = delist_event[i];
      const bool event_due =
          event != kNoEvent && config->delistings[event].last_valid_period < period &&
          config->delistings[event].available_period <= period;
      if (!event_due && !terminal_policy) continue;
      // A delisted name can never fill again: cancel its working order even when
      // nothing is held yet, so the order cannot stay open silently for good.
      if (event_due && !working.empty() && !std::isnan(working[i])) {
        working[i] = kNoWorkingOrder;
        --open_orders;
      }
      const auto price = close[period * instruments + i];
      if (units[i] == 0.0 || (std::isfinite(price) && price > 0.0)) {
        carry_from[i] = kNoCarry; // Valued at its own close (or not held).
        continue;
      }
      // Its last valid close: kept through a multi-session gap, else period - 1
      // (the close it was valued at, or bought at, in the previous observation).
      if (carry_from[i] == kNoCarry) carry_from[i] = period - 1;
      if (gap_carried(i, period, event_due)) {
        const auto mark = close[carry_from[i] * instruments + i];
        gap_record[i] = gaps->size();
        gaps->push_back(ReplayGapCarry{period, i, carry_from[i], units[i], units[i] * mark,
                                       false});
        continue;
      }
      delisting[i] = 1;
      terminal[i] = resolve_terminal(i, event_due, units[i] < 0.0);
      delisting_pending = true;
    }
    return delisting_pending;
  }

  // The flagged names were valued at their last mark; apply the delisting return.
  Result<MarkedPortfolio> apply_delist_returns(atx::usize period,
                                               std::span<atx::f64> end_values) const {
    for (atx::usize i = 0; i < instruments; ++i) {
      if (delisting[i] == 0) continue;
      end_values[i] *= 1.0 + terminal[i].value;
      ATX_TRY_VOID(require_finite(end_values[i], "delisting value" + location(period, i)));
    }
    return total_marked(end_values);
  }

  // Converts every flagged name into signed cash at its delisting value. This is
  // a mandatory movement, not a trade: no cost, no ReplayTrade, NAV unchanged.
  Result<MarkedPortfolio> liquidate(atx::usize period, std::span<atx::f64> units,
                                    std::span<const atx::f64> last_values,
                                    std::span<atx::f64> end_values, atx::f64 &cash,
                                    ReplayResult &result) {
    for (atx::usize i = 0; i < instruments; ++i) {
      if (delisting[i] == 0) continue;
      const auto proceeds = end_values[i];
      cash += proceeds;
      ATX_TRY_VOID(require_finite(cash, "cash after delisting" + location(period, i)));
      const auto source = terminal[i].source;
      const bool flagged = source == TerminalReturnSource::ShumwayNyseAmex ||
                           source == TerminalReturnSource::ShumwayNasdaq ||
                           source == TerminalReturnSource::ShumwayUnknownAdverse ||
                           source == TerminalReturnSource::AssumedMissingPriceAdverse;
      if (source == TerminalReturnSource::AssumedMissingPriceAdverse) {
        ++result.assumed_liquidations;
        result.assumed_liquidation_pnl += proceeds - last_values[i];
        ATX_TRY_VOID(require_finite(result.assumed_liquidation_pnl, "assumed liquidation P&L"));
      }
      result.flagged_delistings += flagged ? 1U : 0U;
      if (flagged && units[i] < 0.0) {
        ++result.flagged_short_delistings;
        result.flagged_short_pnl += proceeds - last_values[i];
        ATX_TRY_VOID(require_finite(result.flagged_short_pnl, "flagged short P&L"));
      }
      result.delistings.push_back(ReplayDelisting{period, i, units[i], last_values[i],
                                                  terminal[i].value, proceeds, source, flagged});
      units[i] = 0.0;
      end_values[i] = 0.0;
      carry_from[i] = kNoCarry;
      if (!working.empty() && !std::isnan(working[i])) {
        working[i] = kNoWorkingOrder;
        --open_orders;
      }
      delisting[i] = 0;
    }
    delisting_pending = false;
    return total_marked(end_values);
  }
};

// All entry points share this exact accounting path. The optional capture only
// replaces the target at an existing execution seam; fixed replay allocates no
// eligibility or allocation-history arrays. The optional claims capture adds the
// design §4 ordering around the same accounting; with no capture, or with an
// empty batch at every observation, every branch it owns is skipped and the
// numbers are bit-identical to the three original entry points.
Result<ReplayResult> replay_targets(
    const alpha::Panel &research, std::span<const atx::i64> session_keys,
    std::span<const atx::usize> decision_periods, std::span<const atx::f64> target_weights,
    const ReplayConfig &config, PolicyCapture *capture, ClaimsCapture *claims = nullptr) {
  ATX_TRY_VOID(validate_inputs(research, session_keys, decision_periods, target_weights, config));
  ATX_TRY(auto close_field, research.field_id("close"));
  const auto close = research.field_all(close_field);
  const auto dates = research.dates();
  const auto instruments = research.instruments();
  if (close.size() != dates * instruments) {
    return Err(ErrorCode::InvalidArgument, "replay: close field shape mismatch");
  }
  ReplayResult result;
  result.initial_nav = config.initial_nav;
  result.final_cash = config.initial_nav;
  result.final_nav = config.initial_nav;
  result.final_tri_units.resize(instruments, 0.0);
  result.intervals.reserve(dates - 1);
  std::vector<atx::f64> start_values(instruments, 0.0);
  std::vector<atx::f64> end_values(instruments, 0.0);
  if (capture != nullptr) {
    capture->eligibility.resize(instruments);
    capture->allocations.reserve(decision_periods.size());
  }
  ReplayExtensions ext;
  ext.init(config, instruments, result, close, dates);
  if (claims != nullptr) {
    claims->retired.assign(instruments, atx::u8{0});
    claims->carried.assign(instruments, atx::u8{0});
    claims->movements.reserve(kMaxMandatoryMovements);
    if (ext.per_name_model != nullptr) {
      claims->order_cancellations.reserve(2 * kMaxTransitionEvents);
    }
    claims->state.settled_cash = config.initial_nav;
  }
  atx::usize next_decision = 0;
  std::optional<atx::usize> origin;
  for (atx::usize d = 0; d + 1 < dates; ++d) {
    // Step 1: valuation at d, exempting only the admitted predecessors of the
    // batch this observation will apply.
    const auto carried = (claims != nullptr && claims->batch_period == d)
        ? std::span<const atx::u8>(claims->carried) : std::span<const atx::u8>{};
    // A gap-carried name (TerminalReturn) keeps the last-close valuation its
    // end mark at d already used.
    ATX_TRY(auto start, mark_holdings(result.final_tri_units, close, d, start_values, carried,
                                      d == 0 ? 0 : d - 1, ext.carry_view()));
    auto pretrade_nav = result.final_cash + start.assets;
    ClaimsNav claims_nav{};
    if (claims != nullptr) {
      begin_observation(*claims, result.final_cash);
      ATX_TRY(const auto pre_event, compute_claims_nav(claims->state, start.assets));
      if (claims->batch_period == d && claims->batch.transition_count != 0) { // Step 2.
        ATX_TRY_VOID(apply_transitions(*claims, close, session_keys, d, result.final_tri_units,
                                       start_values));
        ext.cancel_transition_orders(claims->batch, d, claims->order_cancellations);
        ATX_TRY(start, total_marked(start_values)); // The commit rewrote two coordinates.
      }
      // Steps 3-4: the recognized claims are already in the ledger.
      ATX_TRY(claims_nav, compute_claims_nav(claims->state, start.assets));
      ATX_TRY_VOID(require_reconciled(claims_nav.nav, pre_event.nav + claims->value_bridge, d,
                                      instruments));
      pretrade_nav = claims_nav.nav;
    }
    ATX_TRY_VOID(require_nav(pretrade_nav, d));
    atx::f64 trade_cost = 0.0;
    atx::f64 traded = 0.0;
    if (next_decision < decision_periods.size() &&
        decision_periods[next_decision] + config.execution_delay_periods == d) {
      origin = decision_periods[next_decision];
      auto weights = target_weights.subspan(next_decision * instruments, instruments);
      std::optional<ReplayAllocation> allocation;
      if (capture != nullptr) {
        // Every field is initialised explicitly: the two claims spans carry no
        // default member initialiser, so an omitted tail is a -Werror diagnostic
        // rather than a silent empty view. The claims terms are replaced below
        // when a claims capture is running.
        ReplayAllocationState state{next_decision, *origin, d,
            session_keys[*origin], session_keys[d], result.final_cash, pretrade_nav,
            result.final_tri_units, close.subspan(d * instruments, instruments),
            start_values, capture->eligibility, weights, 0.0, 0.0, 0.0,
            std::span<const PendingTransitionCashClaim>{}, std::span<const atx::u8>{}};
        if (claims != nullptr) { // Step 5: claims-aware allocation inputs.
          state.signed_pending_claims = claims_nav.signed_pending_claims;
          state.gross_receivable = claims_nav.gross_receivable;
          state.gross_payable = claims_nav.gross_payable;
          state.pending_claims = pending_claims_view(claims->state, claims->claim_scratch);
          state.retired_representation = claims->retired;
        }
        ATX_TRY(auto candidate, allocate_target(*capture, research, state));
        allocation = std::move(candidate);
        weights = allocation->target_weights;
        if (claims != nullptr) {
          ATX_TRY_VOID(reject_retired_targets(allocation->target_intents, claims->retired, d));
        }
      }
      const bool uses_intents = capture != nullptr && capture->intent_policy != nullptr;
      const auto intents = uses_intents
          ? std::span<const ReplayTargetIntent>(allocation->target_intents)
          : std::span<const ReplayTargetIntent>{};
      const auto eligibility = uses_intents ? std::span<const atx::u8>(capture->eligibility)
                                            : std::span<const atx::u8>{};
      const auto resolved = uses_intents ? std::span<atx::f64>(allocation->target_weights)
                                         : std::span<atx::f64>{};
      // Step 6: trades. Mandatory movements are not trades and contribute
      // exactly 0.0 to traded_dollars and trade_cost.
      ATX_TRY(auto applied, apply_target(weights, intents, eligibility, resolved,
          close, d, *origin, pretrade_nav, ext.trade_rate(config), result.final_tri_units,
          start_values, result.final_cash, result.trades, claims_nav.signed_pending_claims,
          ext.context(d)));
      ext.recount_working_orders();
      start = applied.marked;
      trade_cost = applied.trade_cost;
      traded = applied.traded_dollars;
      if (capture != nullptr) {
        allocation->posttrade_cash = result.final_cash;
        // Claims are constant across the trade, so the certificate's posttrade
        // NAV stays the same three-term identity the pretrade NAV used.
        allocation->posttrade_nav =
            result.final_cash + start.assets + claims_nav.signed_pending_claims;
        allocation->traded_dollars = traded;
        allocation->trade_cost = trade_cost;
        allocation->posttrade_marked_dollars = start_values;
        capture->allocations.push_back(std::move(*allocation));
      }
      ++next_decision;
      ++result.effective_rebalances;
    } else if (ext.has_working_orders()) {
      ATX_TRY(auto applied, work_residuals(ext.context(d), close, d, *origin, pretrade_nav,
          result.final_tri_units, start_values, result.final_cash, result.trades,
          claims_nav.signed_pending_claims));
      ext.recount_working_orders();
      start = applied.marked;
      trade_cost = applied.trade_cost;
      traded = applied.traded_dollars;
    }
    if (claims != nullptr) { // Step 7: settlement follows the trade.
      claims->state.settled_cash = result.final_cash;
      if (claims->batch_period == d && claims->batch.payment_count != 0) {
        ATX_TRY_VOID(settle_payments(*claims, d, start.assets, instruments));
        result.final_cash = claims->state.settled_cash;
      }
    }
    // Step 8: borrow on post-trade, post-settlement marked short equity
    // dollars; pending claims of either sign contribute nothing to the base.
    ATX_TRY(auto days, elapsed_days(session_keys[d], session_keys[d + 1]));
    atx::f64 borrow = 0.0;
    if (config.borrow_schedule != nullptr) {
      ATX_TRY(borrow, scheduled_financing(*config.borrow_schedule, start_values, start.shorts,
                                          result.final_cash, d, days, config));
    } else {
      ATX_TRY(borrow, borrow_charge(start.shorts, days, config));
    }
    // Names flagged for liquidation and gap-carried names are valued at their
    // last valid close through ext.carry_view(); claims predecessors at d.
    std::span<const atx::u8> next_carried;
    (void)ext.flag_delistings(close, d + 1, result.final_tri_units);
    if (claims != nullptr) {
      // Step 0 for the NEXT observation, requested here because the valuation
      // of d + 1 happens first as this interval's end mark.
      ATX_TRY_VOID(request_event_batch(*claims, close, session_keys, d + 1, dates, instruments,
                                       result.final_cash - borrow, result.final_tri_units,
                                       start_values));
      if (claims->batch_period == d + 1) next_carried = claims->carried;
    }
    ATX_TRY(auto end, mark_holdings(result.final_tri_units, close, d + 1, end_values,
                                    next_carried, d, ext.carry_view()));
    if (ext.delisting_pending) {
      ATX_TRY(end, ext.apply_delist_returns(d + 1, end_values));
    }
    atx::f64 gross_pnl = 0.0;
    for (atx::usize i = 0; i < instruments; ++i) {
      gross_pnl += end_values[i] - start_values[i];
      ATX_TRY_VOID(require_finite(gross_pnl, "gross P&L"));
    }
    result.final_cash -= borrow;
    if (ext.delisting_pending) {
      ATX_TRY(end, ext.liquidate(d + 1, result.final_tri_units, start_values, end_values,
                                 result.final_cash, result));
    }
    result.final_assets = end.assets;
    result.final_nav = result.final_cash + end.assets;
    ClaimsNav end_nav{};
    if (claims != nullptr) {
      claims->state.settled_cash = result.final_cash;
      ATX_TRY(end_nav, compute_claims_nav(claims->state, end.assets));
      result.final_nav = end_nav.nav;
    }
    ATX_TRY_VOID(require_finite(result.final_cash, "cash after financing"));
    ATX_TRY_VOID(require_nav(result.final_nav, d + 1));
    ATX_TRY_VOID(require_reconciled(result.final_nav,
        pretrade_nav + gross_pnl - trade_cost - borrow, d + 1, instruments));
    const auto net_return = (result.final_nav - pretrade_nav) / pretrade_nav;
    ATX_TRY_VOID(require_finite(net_return, "net return"));
    ReplayInterval interval{d, d + 1, origin, pretrade_nav,
        result.final_cash, end.assets, result.final_nav, gross_pnl, trade_cost, borrow,
        net_return, traded, start.gross, end.gross};
    if (claims != nullptr) {
      interval.settled_cash = result.final_cash;
      interval.signed_pending_claims = end_nav.signed_pending_claims;
      interval.gross_receivable = end_nav.gross_receivable;
      interval.gross_payable = end_nav.gross_payable;
      interval.mandatory_value_bridge = claims->value_bridge;
      interval.mandatory_settled_cash = claims->settled_cash_delta;
      interval.claim_recognized = claims->recognized;
      interval.claim_settled = claims->settled;
    }
    result.intervals.push_back(interval);
  }
  result.unexecuted_decisions = decision_periods.size() - result.effective_rebalances;
  result.open_working_orders = ext.open_working_orders();
  return Ok(std::move(result));
}

} // namespace

Result<ReplaySizedTarget> resolve_replay_target(const ReplayTargetIntent &intent,
    atx::f64 current_tri_units, atx::f64 current_marked_dollars, atx::f64 current_mark,
    atx::f64 pretrade_nav, bool decision_eligible) {
  if (!std::isfinite(pretrade_nav) || pretrade_nav <= 0.0 ||
      !std::isfinite(current_tri_units) || !std::isfinite(current_marked_dollars)) {
    return Err(ErrorCode::InvalidArgument, "replay: invalid sizing NAV or held state");
  }
  const bool held = current_tri_units != 0.0;
  if (held && (!std::isfinite(current_mark) || current_mark <= 0.0)) {
    return Err(ErrorCode::InvalidArgument, "replay: missing/nonpositive required close");
  }
  if ((held && (current_marked_dollars == 0.0 ||
                 current_tri_units * current_mark != current_marked_dollars)) ||
      (!held && current_marked_dollars != 0.0)) {
    return Err(ErrorCode::InvalidArgument, "replay: inconsistent held units/marked dollars");
  }
  if (!std::isfinite(intent.weight)) {
    return Err(ErrorCode::InvalidArgument, "replay: nonfinite intent weight");
  }
  ReplaySizedTarget sized;
  switch (intent.action) {
  case ReplayTargetAction::TargetWeight:
    sized.resolved_weight = intent.weight;
    if (intent.weight != 0.0) {
      if (!decision_eligible) {
        return Err(ErrorCode::InvalidArgument, "replay: ineligible target intent");
      }
      if (!std::isfinite(current_mark) || current_mark <= 0.0) {
        return Err(ErrorCode::InvalidArgument, "replay: missing/nonpositive required close");
      }
      const auto desired = intent.weight * pretrade_nav;
      sized.tri_units = desired / current_mark;
      sized.marked_dollars = sized.tri_units * current_mark;
      if (!std::isfinite(desired) || !std::isfinite(sized.tri_units) ||
          !std::isfinite(sized.marked_dollars) || desired == 0.0 ||
          sized.tri_units == 0.0 || sized.marked_dollars == 0.0) {
        return Err(ErrorCode::OutOfRange, "replay: target value/units overflow/underflow");
      }
    }
    break;
  case ReplayTargetAction::HoldCurrent:
    if (intent.weight != 0.0 || (held && !decision_eligible)) {
      return Err(ErrorCode::InvalidArgument, "replay: nonzero Hold payload or ineligible Hold");
    }
    sized.tri_units = current_tri_units;
    sized.marked_dollars = current_marked_dollars;
    sized.resolved_weight = current_marked_dollars / pretrade_nav;
    if (!std::isfinite(sized.resolved_weight)) {
      return Err(ErrorCode::OutOfRange, "replay: nonfinite resolved Hold weight");
    }
    return Ok(sized); // Preserve units/value exactly, including a +0 trade delta.
  case ReplayTargetAction::Close:
    if (intent.weight != 0.0) {
      return Err(ErrorCode::InvalidArgument, "replay: nonzero Close payload");
    }
    break;
  default:
    return Err(ErrorCode::InvalidArgument, "replay: unrecognized target action");
  }
  sized.dollar_delta = sized.marked_dollars - current_marked_dollars;
  if (!std::isfinite(sized.dollar_delta)) {
    return Err(ErrorCode::OutOfRange, "replay: nonfinite trade delta");
  }
  return Ok(sized);
}

// Accounting follows the cash/holdings dynamics in Boyd et al., sections 2.1-2.6:
// https://web.stanford.edu/~boyd/papers/pdf/cvx_portfolio.pdf
// The elapsed simple borrow convention is explicit here; compare actual-period
// simulation and post-trade short fees at https://www.cvxportfolio.com/en/stable/costs.html.
Result<ReplayResult> replay_scheduled_targets(
    const alpha::Panel &research, std::span<const atx::i64> session_keys,
    std::span<const atx::usize> decision_periods, std::span<const atx::f64> target_weights,
    const ReplayConfig &config) {
  return replay_targets(research, session_keys, decision_periods, target_weights, config, nullptr);
}

Result<ReplayPolicyResult> replay_scheduled_targets(
    const alpha::Panel &research, std::span<const atx::i64> session_keys,
    std::span<const atx::usize> decision_periods, std::span<const atx::f64> preference_weights,
    const ReplayAllocationPolicy &policy, const ReplayConfig &config) {
  if (!policy) {
    return Err(ErrorCode::InvalidArgument, "replay: allocation policy must not be empty");
  }
  PolicyCapture capture{&policy, nullptr, {}, {}};
  ATX_TRY(auto replay, replay_targets(research, session_keys, decision_periods,
      preference_weights, config, &capture));
  return Ok(ReplayPolicyResult{std::move(replay), std::move(capture.allocations)});
}

Result<ReplayPolicyResult> replay_scheduled_intents(
    const alpha::Panel &research, std::span<const atx::i64> session_keys,
    std::span<const atx::usize> decision_periods, std::span<const atx::f64> preference_weights,
    const ReplayIntentPolicy &policy, const ReplayConfig &config) {
  if (!policy) {
    return Err(ErrorCode::InvalidArgument, "replay: intent policy must not be empty");
  }
  PolicyCapture capture{nullptr, &policy, {}, {}};
  ATX_TRY(auto replay, replay_targets(research, session_keys, decision_periods,
      preference_weights, config, &capture));
  return Ok(ReplayPolicyResult{std::move(replay), std::move(capture.allocations)});
}

// The claims ledger, the batch and the pending-claim scratch together run to
// tens of kilobytes, so the capture is heap-owned once before the loop rather
// than placed on this frame; nothing inside the per-observation path allocates.
Result<ReplayClaimsResult> replay_scheduled_intents_with_events(
    const alpha::Panel &research, std::span<const atx::i64> session_keys,
    std::span<const atx::usize> decision_periods,
    std::span<const atx::f64> preference_weights, const ReplayIntentPolicy &policy,
    const ReplayMandatoryEventPolicy &events, const ReplayConfig &config,
    const ReplayClaimsConfig &claims) {
  if (!policy || !events) {
    return Err(ErrorCode::InvalidArgument,
               "replay: intent and mandatory event policies must not be empty");
  }
  if (uses_conflicting_delisting_policy(config)) {
    return Err(ErrorCode::InvalidArgument,
               "replay: delisting table/exchanges and non-default delisting policies "
               "are not supported with mandatory events");
  }
  // Mandatory events are this path's terminal mechanism (a retiring
  // predecessor is carried by its admitted transition), and the path is
  // synthetic-fixture only: the default TerminalReturn policy runs with Abort
  // semantics here, so a held name missing a close outside an event rejects.
  ReplayConfig effective = config;
  effective.delisting_policy = DelistingPolicy::Abort;
  PolicyCapture capture{nullptr, &policy, {}, {}};
  auto owned = std::make_unique<ClaimsCapture>();
  owned->events = &events;
  owned->config = claims;
  ATX_TRY(auto replay, replay_targets(research, session_keys, decision_periods,
      preference_weights, effective, &capture, owned.get()));
  ReplayClaimsResult result;
  result.policy = ReplayPolicyResult{std::move(replay), std::move(capture.allocations)};
  result.movements = std::move(owned->movements);
  result.order_cancellations = std::move(owned->order_cancellations);
  result.final_state = owned->state;
  return Ok(std::move(result));
}

} // namespace atx::engine::book
