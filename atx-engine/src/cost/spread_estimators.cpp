#include "atx/engine/cost/spread_estimators.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

// Adapted from Emanuele Guidotti's bidask (MIT, copyright 2024), pinned at
// 1caba55d63ebab6c855536be51c43bdfc48d2dec: r/R/{cs,ar,edge}.R.
// Full notice: bidask-LICENSE.txt beside this source. No market data are bundled.
namespace atx::engine::cost {
namespace {
using atx::f64;
using atx::core::Err;
using atx::core::ErrorCode;
using atx::core::Ok;

bool positive(f64 x) noexcept { return std::isfinite(x) && x > 0.0; }
struct Pair {
  f64 tau{}, po{}, pc{}, r1{}, r2{}, r3{}, r4{}, r5{}, cs{}, ar2{};
};
Pair pair(const SpreadOhlc& prev, const SpreadOhlc& next) noexcept {
  const auto h0 = std::log(prev.high), l0 = std::log(prev.low);
  const auto c0 = std::log(prev.close), o = std::log(next.open);
  const auto h = std::log(next.high), l = std::log(next.low);
  const auto m0 = (h0 + l0) / 2.0, m = (h + l) / 2.0;
  Pair p;
  p.tau = (h != l || l != c0) ? 1.0 : 0.0;
  p.po = ((o != h ? 1.0 : 0.0) + (o != l ? 1.0 : 0.0)) * p.tau;
  p.pc = ((c0 != h0 ? 1.0 : 0.0) + (c0 != l0 ? 1.0 : 0.0)) * p.tau;
  p.r1 = m - o; p.r2 = o - m0; p.r3 = m - c0;
  p.r4 = c0 - m0; p.r5 = o - c0;
  const auto gap = std::max(0.0, c0 - h) + std::min(0.0, c0 - l);
  const auto b = (h - l) * (h - l) + (h0 - l0) * (h0 - l0);
  const auto width = std::max(h + gap, h0) - std::min(l + gap, l0);
  const auto k = 3.0 - 2.0 * std::sqrt(2.0);
  const auto a = (std::sqrt(2.0 * b) - std::sqrt(b)) / k -
                 std::sqrt(width * width / k);
  p.cs = 2.0 * std::tanh(a / 2.0); // algebraic CS form without exp overflow
  p.ar2 = 4.0 * (c0 - m0) * (c0 - m);
  return p;
}
} // namespace

atx::core::Result<SpreadEstimate> estimate_spread(std::span<const SpreadOhlc> history,
    atx::i64 decision_time_ns, const SpreadRecipe& recipe) {
  if (recipe.rule != SpreadRule::BidaskUnsignedCompleteV1 || decision_time_ns <= 0 ||
      !std::isfinite(recipe.scale) || recipe.scale < 0.0 || recipe.min_observations < 3U ||
      recipe.max_observations < recipe.min_observations ||
      history.size() > recipe.max_observations)
    return Err(ErrorCode::InvalidArgument, "spread: invalid recipe, decision or window bound");
  SpreadEstimate out;
  bool missing = history.size() < recipe.min_observations;
  for (atx::usize i = 0U; i < history.size(); ++i) {
    const auto& row = history[i];
    if (i > 0U) {
      const auto prev = history[i - 1U].session;
      if (row.session <= prev)
        return Err(ErrorCode::InvalidArgument, "spread: sessions must increase");
      if (prev == std::numeric_limits<atx::i64>::max() || row.session != prev + 1)
        missing = true;
    }
    if (row.state == SpreadState::Unavailable) { missing = true; continue; }
    if (row.state != SpreadState::Available || !positive(row.open) || !positive(row.high) ||
        !positive(row.low) || !positive(row.close) || row.high < row.low ||
        row.open < row.low || row.open > row.high || row.close < row.low || row.close > row.high)
      return Err(ErrorCode::InvalidArgument, "spread: invalid available OHLC row");
    if (row.available_at_ns <= 0 || row.available_at_ns >= decision_time_ns) missing = true;
    out.available_at_ns = std::max(out.available_at_ns, row.available_at_ns);
  }
  if (missing) return Ok(out);
  const auto count = static_cast<f64>(history.size() - 1U);
  f64 tau = 0.0, po = 0.0, pc = 0.0, r1 = 0.0, r3 = 0.0, r5 = 0.0;
  f64 cs = 0.0, ar2 = 0.0;
  for (atx::usize i = 1U; i < history.size(); ++i) {
    const auto p = pair(history[i - 1U], history[i]);
    tau += p.tau; po += p.po; pc += p.pc;
    r1 += p.r1; r3 += p.r3; r5 += p.r5; cs += p.cs; ar2 += p.ar2;
  }
  if (tau < 2.0 || po == 0.0 || pc == 0.0) return Ok(out);
  // r*/tau == mean(r*)/mean(tau), retaining all calendar pairs.
  r1 /= tau; r3 /= tau; r5 /= tau; po /= count; pc /= count;
  f64 e1 = 0.0, e2 = 0.0, q1 = 0.0, q2 = 0.0;
  for (atx::usize i = 1U; i < history.size(); ++i) {
    const auto p = pair(history[i - 1U], history[i]);
    const auto d1 = p.r1 - p.tau * r1;
    const auto d3 = p.r3 - p.tau * r3;
    const auto d5 = p.r5 - p.tau * r5;
    const auto x1 = (-4.0 / po) * d1 * p.r2 + (-4.0 / pc) * d3 * p.r4;
    const auto x2 = (-4.0 / po) * d1 * p.r5 + (-4.0 / pc) * d5 * p.r4;
    e1 += x1; e2 += x2; q1 += x1 * x1; q2 += x2 * x2;
  }
  e1 /= count; e2 /= count;
  const auto v1 = q1 / count - e1 * e1, v2 = q2 / count - e2 * e2;
  const auto vt = v1 + v2;
  const auto s2 = vt > 0.0 ? (v2 * e1 + v1 * e2) / vt : (e1 + e2) / 2.0;
  out.corwin_schultz = std::abs(cs / count);
  out.abdi_ranaldo = std::sqrt(std::abs(ar2 / count));
  out.edge = std::sqrt(std::abs(s2));
  // Scaling only the final ensemble keeps raw component diagnostics reference-comparable.
  out.full_spread = recipe.scale *
      (out.corwin_schultz / 3.0 + out.abdi_ranaldo / 3.0 + out.edge / 3.0);
  if (!std::isfinite(out.corwin_schultz) || !std::isfinite(out.abdi_ranaldo) ||
      !std::isfinite(out.edge) || !std::isfinite(out.full_spread))
    return Err(ErrorCode::InvalidArgument, "spread: nonfinite estimator or scaled composite");
  out.state = SpreadState::Available;
  return Ok(out);
}
} // namespace atx::engine::cost
