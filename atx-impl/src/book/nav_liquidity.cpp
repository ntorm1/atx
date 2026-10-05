// NAV replay: execution liquidity (windows, cache) and events.
// P9 C3 (task 1, move-only): moved unchanged from strategy_nav_replay.cpp.

#include "book/nav_replay_internal.hpp"

namespace atx::impl::strategy {
using namespace nav_book;
namespace nav_book {
// Same interval guard as the target replay / strategy runner.
bool guarded_move(f64 close_a, f64 close_b, f64 raw_a, f64 raw_b) {
  const f64 adjusted = std::log(close_b) - std::log(close_a);
  const f64 raw = std::log(raw_b) - std::log(raw_a);
  return !std::isfinite(adjusted) || std::abs(adjusted) > 1.5 ||
         std::abs(adjusted) > std::abs(raw) + .10;
}
// Execution-time liquidity of name i for session t from [t-w, t) only: raw-dollar
// ADV = sum of present raw_close*volume / w (absent or pre-history rows count as
// zero; never rescaled) and the sample SD of adjusted simple returns over adjacent
// present, unguarded pairs (k-1, k), k in [t-w, t), as strategy_runner.cpp does.
// Fewer than min_vol_pairs pairs -> the scenario's declared fallback sigma.
WindowLiquidity window_liquidity(const TargetReplayInput& x, std::span<const f64> volume,
                                 const NavReplayConfig& cfg, usize t, usize i) {
  const usize n = x.instruments, w = cfg.liquidity_window;
  const usize first = t > w ? t - w : 0;
  f64 dollars = 0, mean = 0, m2 = 0; usize pairs = 0;
  for (usize k = first; k < t; ++k) {
    const usize b = k * n + i;
    if (!x.present[b]) continue;
    dollars += x.raw_close[b] * volume[b];
    if (k == 0 || !x.present[b - n] ||
        guarded_move(x.close[b - n], x.close[b], x.raw_close[b - n], x.raw_close[b]))
      continue;
    const f64 r = x.close[b] / x.close[b - n] - 1;
    ++pairs; const f64 delta = r - mean;
    mean += delta / static_cast<f64>(pairs); m2 += delta * (r - mean);
  }
  // Not a fallback: pairs >= min_vol_pairs >= 2 and m2 finite >= 0, so sigma is a
  // finite number >= 0; NaN marks the fallback exactly.
  const bool fallback = pairs < cfg.min_vol_pairs || !std::isfinite(m2) || m2 < 0;
  return {dollars / static_cast<f64>(w),
          fallback ? nan : std::sqrt(m2 / static_cast<f64>(pairs - 1))};
}
WindowLiquidity window_liquidity(const Ctx& c, usize t, usize i) {
  return window_liquidity(c.x, c.volume, c.cfg, t, i);
}
// The scenario's row over a window: its half spread, and its declared fallback sigma
// when the window has too few pairs. The same values as computing it in one piece.
Liquidity liquidity_row(const Ctx& c, const WindowLiquidity& window) {
  Liquidity out;
  out.row.adv_dollars = window.adv;
  out.row.half_spread_bps = c.cfg.scenario.half_spread_bps;
  out.fallback = std::isnan(window.sigma);
  out.row.daily_vol = out.fallback ? c.cfg.scenario.fallback_daily_vol : window.sigma;
  return out;
}
Liquidity liquidity_row(const Ctx& c, usize t, usize i) {
  return liquidity_row(c, window_liquidity(c, t, i));
}
// Session t's cache: every name present at t that is a member of the decision at t
// (when `decision`: per-name rates only), or holds a working order in some book at an
// execution session t. It runs before any book's MARK at t, and MARK only ever cancels
// orders (a write-off), so this covers every order execute_orders prices at t, and
// every decision member (members are present). Other names are NaN.
void fill_liquidity(const Ctx& c, const std::vector<std::unique_ptr<Book>>& books, usize t,
                    bool execution, bool decision, LiquidityCache& cache) {
  const auto& x = c.x; const usize n = x.instruments;
  for (usize i = 0; i < n; ++i) {
    const usize k = t * n + i;
    bool need = x.present[k] && decision && x.member[k];
    if (x.present[k] && execution && !need)
      need = std::any_of(books.begin(), books.end(),
                         [i](const std::unique_ptr<Book>& b) { return b->names.active[i] != 0; });
    if (!need) { cache.adv[i] = nan; cache.sigma[i] = nan; continue; }
    const auto window = window_liquidity(c, t, i);
    cache.adv[i] = window.adv; cache.sigma[i] = window.sigma;
  }
}
void add(NavBucket& bucket, f64 exposure, f64 pnl) {
  ++bucket.count; bucket.gross_exposure += std::abs(exposure); bucket.pnl += pnl;
}
co::Status push_event(const Ctx& c, Book& b, NavEventKind kind, usize t, usize i,
                      const EventValues& v) {
  if (b.result.events.size() >= c.cfg.max_events)
    return co::Err(co::ErrorCode::OutOfRange,
                   "nav replay: event cap exceeded (" + std::to_string(c.cfg.max_events) + ")");
  NavEvent e;
  e.kind = kind; e.short_side = v.exposure < 0; e.run_length = v.run;
  e.session = c.x.session_keys[t]; e.instrument_id = c.x.instrument_ids[i];
  e.exposure = v.exposure; e.r_adj = v.r_adj; e.r_raw = v.r_raw; e.haircut = v.haircut;
  e.pnl = v.pnl;
  b.result.events.push_back(e);
  return co::Ok();
}
} // namespace nav_book
} // namespace atx::impl::strategy
