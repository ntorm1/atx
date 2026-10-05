// NAV replay: EXECUTE (working orders against liquidity).
// P9 C3 (task 1, move-only): moved unchanged from strategy_nav_replay.cpp.

#include "book/nav_replay_internal.hpp"

namespace atx::impl::strategy {
using namespace nav_book;
namespace nav_book {
// EXECUTE working orders at close t. Absent names are blocked (order persists); an
// unusable ADV fills nothing; a capped fill leaves the residual working. A target
// order requests order - held (the drift since its decision is traded back); a delta
// order requests order - anchor, its remaining decision-dollar delta (the drift rides).
co::Status execute_orders(const Ctx& c, Book& b, usize t, const LiquidityCache& cache,
                          NavReplayDay& day) {
  const auto& x = c.x; auto& s = b.names; const usize n = x.instruments;
  Trace* const trace = b.trace.get(); // --emit-holdings observer; never read back
  f64 cost = 0, traded = 0;
  for (usize i = 0; i < n; ++i) {
    if (!s.active[i]) continue;
    if (!x.present[t * n + i]) {
      ++day.blocked_absent;
      if (trace) trace->record(i, NavFillStatus::BlockedAbsent, 0, 0, 0);
      continue;
    }
    const bool delta = delta_order(s, i);
    const f64 requested = s.order[i] - (delta ? s.anchor[i] : s.held[i]);
    if (requested == 0) { s.active[i] = 0; continue; }
    // fill_liquidity formed it (MARK only cancels orders). Fail loud in debug; in
    // release an unformed (NaN) entry recomputes the window -- the same arithmetic, so
    // the same bytes -- instead of reading as an unusable ADV that silently fills nothing.
    assert(!cache.on() || !std::isnan(cache.adv[i]));
    const auto liquidity = cache.on() && !std::isnan(cache.adv[i])
        ? liquidity_row(c, WindowLiquidity{cache.adv[i], cache.sigma[i]})
        : liquidity_row(c, t, i);
    const auto priced = c.model.cost(i, t, requested, liquidity.row);
    const f64 full = c.model.unrationed_cost(i, t, requested, liquidity.row);
    if (std::isfinite(full)) day.unrationed_cost_dollars += full; else ++day.unrationed_unpriced;
    const f64 filled = priced.filled_dollars, size = std::abs(filled);
    if (filled == 0) {
      ++day.blocked_liquidity; day.unfilled_dollars += std::abs(requested);
      if (trace) trace->record(i, NavFillStatus::BlockedLiquidity, 0, 0, std::abs(requested));
      continue;
    }
    const bool complete = filled == requested;
    if (trace)
      trace->record(i, complete ? NavFillStatus::Complete : NavFillStatus::Capped, filled,
                    priced.cost_dollars, complete ? 0.0 : std::abs(requested) - size);
    // A complete target fill lands exactly on the decision-dollar target; a complete
    // delta fill on that target plus the drift since the decision (held - anchor, an
    // exact 0 without drift, so it is then the target fill bit for bit).
    if (delta) {
      s.held[i] = complete ? s.order[i] + (s.held[i] - s.anchor[i]) : s.held[i] + filled;
      if (!complete) s.anchor[i] += filled;
    } else {
      s.held[i] = complete ? s.order[i] : s.held[i] + filled;
    }
    s.active[i] = complete ? u8{0} : u8{1};
    if (!complete) { ++day.capped_fills; day.unfilled_dollars += std::abs(requested) - size; }
    b.cash -= filled; cost += priced.cost_dollars; traded += size; ++day.fills;
    const f64 linear = c.sqrt_model ? size * c.linear_rate : priced.cost_dollars;
    day.linear_cost_dollars += linear; day.impact_cost_dollars += priced.cost_dollars - linear;
    if (c.sqrt_model && liquidity.fallback) ++day.fallback_vol_fills;
    if (liquidity.row.adv_dollars > 0) b.participation.add(size / liquidity.row.adv_dollars);
  }
  b.cash -= cost;
  day.executed = true; day.traded_dollars = traded; day.trade_cost_dollars = cost;
  day.one_way_turnover = traded / b.nav_pre;
  b.nav_post = b.nav_pre - cost; b.pending_cost = cost;
  if (!std::isfinite(b.nav_post) || b.nav_post <= 0 || !std::isfinite(b.cash))
    return co::Err(co::ErrorCode::OutOfRange, "nav replay: nonpositive NAV after costs");
  return co::Ok();
}
} // namespace nav_book
} // namespace atx::impl::strategy
