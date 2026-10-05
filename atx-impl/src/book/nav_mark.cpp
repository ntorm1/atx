// NAV replay: MARK (marks, carries, write-offs, financing).
// P9 C3 (task 1, move-only): moved unchanged from strategy_nav_replay.cpp.

#include "book/nav_replay_internal.hpp"

namespace atx::impl::strategy {
using namespace nav_book;
namespace {
// A flat name only remembers its observable marks. A reprint after a write-off is
// reported with its forgone return versus the haircut and never touches NAV. MARK reads
// c.mark (v8 E-25: the label role's prices with --label-role, else x's own).
co::Status mark_flat(const Ctx& c, Book& b, usize t, usize i) {
  const auto& m = c.mark; auto& s = b.names; const usize k = t * c.x.instruments + i;
  if (!m.present[k]) {
    if (s.written_off[i]) ++s.absent[i];
    return co::Ok();
  }
  if (s.written_off[i]) {
    const f64 exposure = s.written_exposure[i], eta = s.written_haircut[i];
    const f64 r_adj = m.close[k] / s.mark[i] - 1, r_raw = m.raw_close[k] / s.raw_mark[i] - 1;
    const f64 forgone = exposure * (r_adj - eta);
    add(b.result.reappeared, exposure, forgone);
    ATX_TRY_VOID(push_event(c, b, NavEventKind::ReappearedAfterWriteOff, t, i,
                            {s.absent[i], exposure, r_adj, r_raw, eta, forgone}));
    s.written_off[i] = 0; s.absent[i] = 0;
  }
  s.mark[i] = m.close[k]; s.raw_mark[i] = m.raw_close[k];
  return co::Ok();
}
// Held and absent at mark t: carry at the stale mark (orders stay blocked) until
// K consecutive absences, then write off at last mark x (1 + eta), causally.
co::Status carry_absent(const Ctx& c, Book& b, usize t, usize i, f64& writeoff) {
  auto& s = b.names; const f64 h = s.held[i];
  ++s.absent[i];
  if (s.absent[i] < c.cfg.scenario.stale_exit_sessions) return co::Ok();
  const f64 eta = h > 0 ? c.eta_long : c.eta_short;
  const f64 loss = h * eta;
  writeoff += loss; b.cash += h * (1.0 + eta);
  add(b.result.written_off, h, loss);
  ATX_TRY_VOID(push_event(c, b, NavEventKind::WriteOff, t, i,
                          {s.absent[i], h, nan, nan, eta, loss}));
  s.held[i] = 0; s.order[i] = 0; s.active[i] = 0;
  s.written_off[i] = 1; s.written_exposure[i] = h; s.written_haircut[i] = eta;
  return co::Ok();
}
// Held and present: realize the true (cumulative, if a gap preceded) adjusted return, at
// c.mark's prices (v8 E-25: a label role's terminal return is realized here).
co::Status realize(const Ctx& c, Book& b, usize t, usize i, f64& pnl, NavReplayDay& day) {
  const auto& m = c.mark; auto& s = b.names; const usize k = t * c.x.instruments + i;
  const f64 h = s.held[i], close = m.close[k], raw = m.raw_close[k];
  const f64 next = h * (close / s.mark[i]);
  if (!std::isfinite(next))
    return co::Err(co::ErrorCode::OutOfRange, "nav replay: holding mark overflow");
  const f64 r_adj = close / s.mark[i] - 1, r_raw = raw / s.raw_mark[i] - 1;
  const usize run = s.absent[i];
  pnl += next - h;
  if (run) {
    add(run == 1 ? b.result.gap_run_1 : run < 5 ? b.result.gap_run_2_4 : b.result.gap_run_5_plus,
        h, next - h);
    ATX_TRY_VOID(push_event(c, b, NavEventKind::GapResolved, t, i,
                            {run, h, r_adj, r_raw, 0, next - h}));
    s.absent[i] = 0;
  }
  if (guarded_move(s.mark[i], close, s.raw_mark[i], raw)) {
    const f64 difference = h * (r_raw - r_adj);
    ++day.guarded_intervals; add(b.result.guarded, h, difference);
    b.result.guard_sensitivity += difference;
    ATX_TRY_VOID(push_event(c, b, NavEventKind::Guarded, t, i,
                            {run, h, r_adj, r_raw, 0, difference}));
  }
  s.held[i] = next; s.mark[i] = close; s.raw_mark[i] = raw;
  return co::Ok();
}
struct FinancingCharge {
  f64 short_leg{}, long_leg{};
};
// Financing over (t-1, t] on pre-mark dollars. FlatShortV0 keeps the legacy
// expression (rate x calendar days / 365 per short dollar, summed in name order)
// bit for bit; TieredSwapV1 charges the long spread on every long and (short spread
// + fee) on every short at the tier of the latest decision <= t-1. With tiers, the
// short leg is also split by tier, whatever the rule.
FinancingCharge accrue_financing(const Ctx& c, const NameState& s, const BorrowTiers& tiers,
                                 f64 calendar_days, NavReplayDay& day) {
  const auto& financing = c.cfg.scenario.financing;
  const bool flat = financing.rule == NavFinancingRule::FlatShortV0;
  std::array<f64, 4> short_factor{};
  f64 long_factor = 0;
  if (flat) {
    short_factor.fill(c.borrow_rate * calendar_days / 365.0);
  } else {
    const f64 basis = static_cast<f64>(financing.day_count);
    for (usize k = 0; k < short_factor.size(); ++k)
      short_factor[k] = c.short_rate[k] * calendar_days / basis;
    long_factor = c.long_rate * calendar_days / basis;
  }
  FinancingCharge out;
  for (usize i = 0; i < s.held.size(); ++i) {
    const f64 h = s.held[i];
    if (h > 0 && !flat) out.long_leg += h * long_factor;
    if (!(h < 0)) continue;
    const u8 tier = tiers.active ? tiers.tier[i] : tier_warm;
    const f64 charge = -h * short_factor[tier];
    out.short_leg += charge;
    if (!tiers.active) continue;
    const auto slot = static_cast<usize>(tier - tier_gc);
    day.short_dollars_by_tier[slot] += -h; day.short_financing_by_tier[slot] += charge;
    if (!tiers.missing[i]) continue;
    ++day.missing_predictor_shorts; day.missing_predictor_short_dollars += -h;
  }
  return out;
}
} // namespace
namespace nav_book {
// MARK at t > begin: financing on pre-mark dollars x calendar days / day count, then
// drift/stale/write-off. NAVpre_t = NAVpost_{t-1} + P + W - B - L (B: short leg, L:
// long leg), so the fill costs of t-1 land in r_t; cash + sum(held) must reconcile
// to it (checked in close_day).
co::Status mark_session(const Ctx& c, Book& b, const BorrowTiers& tiers, usize t,
                        NavReplayDay& day) {
  const auto& x = c.x; auto& s = b.names; const usize n = x.instruments;
  const f64 calendar_days = static_cast<f64>((x.session_keys[t] - x.session_keys[t - 1]) / day_ns);
  const auto financing = accrue_financing(c, s, tiers, calendar_days, day);
  const f64 borrow = financing.short_leg, long_financing = financing.long_leg;
  f64 pnl = 0, writeoff = 0;
  for (usize i = 0; i < n; ++i) {
    if (s.held[i] == 0) { ATX_TRY_VOID(mark_flat(c, b, t, i)); continue; }
    if (!c.mark.present[t * n + i]) {
      ATX_TRY_VOID(carry_absent(c, b, t, i, writeoff));
      continue;
    }
    ATX_TRY_VOID(realize(c, b, t, i, pnl, day));
  }
  // Subtracting a zero long leg is exact: flat books keep the legacy arithmetic.
  b.cash -= borrow; b.cash -= long_financing;
  const f64 base = b.nav_pre;
  b.nav_pre = b.nav_post + pnl + writeoff - borrow - long_financing;
  if (!std::isfinite(b.nav_pre) || b.nav_pre <= 0)
    return co::Err(co::ErrorCode::OutOfRange, "nav replay: nonpositive/nonfinite pre-trade NAV");
  day.mark_pnl_dollars = pnl; day.writeoff_dollars = writeoff; day.borrow_dollars = borrow;
  day.long_financing_dollars = long_financing;
  day.net_return = b.nav_pre / base - 1;
  day.gross_return = (pnl + writeoff) / base; day.writeoff_return = writeoff / base;
  day.trade_cost_return = b.pending_cost / base; day.borrow_return = borrow / base;
  day.long_financing_return = long_financing / base;
  const f64 identity = std::abs(day.net_return - (day.gross_return - day.trade_cost_return -
                                                  day.borrow_return - day.long_financing_return));
  b.result.max_return_identity_error = std::max(b.result.max_return_identity_error, identity);
  if (!(identity <= identity_tolerance))
    return co::Err(co::ErrorCode::Internal, "nav replay: r != gross - cost - financing");
  return co::Ok();
}
} // namespace nav_book
} // namespace atx::impl::strategy
