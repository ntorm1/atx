#include "strategy_spo.hpp"

#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iomanip>
#include <limits>
#include <locale>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <nlohmann/json.hpp>
#include "atx/core/sha256.hpp"
#include "atx/engine/book/target_tracking.hpp"
#include "atx/engine/cost/borrow_tiers.hpp"
#include "strategy_spo_v3.hpp"
#include "strategy_target_replay_detail.hpp"

namespace atx::impl::strategy::spo {
namespace {
using namespace atx;
namespace co = atx::core;
namespace ce = atx::engine::cost;
namespace tt = atx::engine::book;
using Json = nlohmann::json;
constexpr f64 nan = std::numeric_limits<f64>::quiet_NaN();
constexpr f64 inf = std::numeric_limits<f64>::infinity();
constexpr f64 eps = std::numeric_limits<f64>::epsilon();
constexpr f64 sessions_per_year = 252.0;
constexpr usize max_root_steps = 200;   // one monotone 1-D root (value + slope passes)
constexpr usize max_beta_steps = 60;    // outer beta root (each step: the inner roots)
constexpr usize max_backtracks = 60;    // metric scale increases per solve
constexpr usize power_steps = 40;       // metric scale estimate
constexpr f64 root_tolerance = 1e-13;   // side sums (net, gross) at the prox, x max(1, |target|)
constexpr f64 beta_tolerance = 1e-12;   // beta'w at the prox
constexpr f64 coupling_tolerance = 1e-10;
constexpr u32 not_optimized = std::numeric_limits<u32>::max();
constexpr usize no_date = std::numeric_limits<usize>::max();
static_assert(std::endian::native == std::endian::little, "risk payloads are little-endian");

bool finite_positive(f64 x) { return std::isfinite(x) && x > 0; }
f64 clamp_to(f64 x, f64 lo, f64 hi) { return x < lo ? lo : (x > hi ? hi : x); }

// ---- factor structure: y = X'w, out = X u, out = F y (never N x N) -----------------------
void exposures(const Problem& p, const f64* w, f64* y) {
  const usize k = p.layout.factors(), styles = p.layout.styles, first = 1 + p.layout.industries;
  std::fill(y, y + k, 0.0);
  const u32* industry = p.industry.data();
  const f64* x = p.styles.data();
  f64 market = 0;
  for (usize i = 0; i < p.n; ++i) {
    const f64 wi = w[i];
    market += wi;
    if (industry[i] != 0) y[industry[i]] += wi;
    const f64* row = x + i * styles;
    for (usize c = 0; c < styles; ++c) y[first + c] += row[c] * wi;
  }
  y[0] += market;
}
void loads(const Problem& p, const f64* u, f64* out) {
  const usize styles = p.layout.styles, first = 1 + p.layout.industries;
  const u32* industry = p.industry.data();
  const f64* x = p.styles.data();
  const f64* us = u + first;
  for (usize i = 0; i < p.n; ++i) {
    f64 v = u[0];
    if (industry[i] != 0) v += u[industry[i]];
    const f64* row = x + i * styles;
    for (usize c = 0; c < styles; ++c) v += row[c] * us[c];
    out[i] = v;
  }
}
void cov_apply(const Problem& p, const f64* y, f64* out) {
  const usize k = p.layout.factors();
  const f64* f = p.covariance.data();
  for (usize r = 0; r < k; ++r) {
    const f64* row = f + r * k;
    f64 sum = 0;
    for (usize c = 0; c < k; ++c) sum += row[c] * y[c];
    out[r] = sum;
  }
}
f64 quadratic(const Problem& p, const f64* y, f64* scratch) {
  cov_apply(p, y, scratch);
  f64 sum = 0;
  for (usize c = 0; c < p.layout.factors(); ++c) sum += y[c] * scratch[c];
  return sum;
}

// ---- the separable prox --------------------------------------------------------------------
// Unconstrained minimizer of (1/(2 tau))(w - v)^2 + c w + s|w - w0| + eta |w - w0|^{3/2} and
// dw/dc. Right of w0: t = sqrt(w - w0) solves t^2 + 1.5 eta tau t - q = 0 with q = -tau x the
// right derivative at w0 (> 0), in the cancellation-free form t = 2q / (b + sqrt(b^2 + 4q));
// left of w0 symmetric; otherwise w0 is the minimizer (the linear cost's dead zone).
struct Min1d {
  f64 w, slope;
};
Min1d minimize_1d(f64 v, f64 tau, f64 c, f64 s, f64 eta, f64 w0) {
  const f64 b = 1.5 * eta * tau;
  const f64 right = w0 - v + tau * (c + s);
  if (right < 0) {
    const f64 q = -right;
    const f64 t = 2.0 * q / (b + std::sqrt(b * b + 4.0 * q));
    const f64 den = t + 0.75 * eta * tau;
    return {w0 + t * t, den > 0 ? -tau * t / den : -tau};
  }
  const f64 left = w0 - v + tau * (c - s);
  if (left > 0) {
    const f64 t = 2.0 * left / (b + std::sqrt(b * b + 4.0 * left));
    const f64 den = t + 0.75 * eta * tau;
    return {w0 - t * t, den > 0 ? -tau * t / den : -tau};
  }
  return {w0, 0.0};
}

struct Scratch {
  explicit Scratch(const Problem& p)
      : v(p.n), tau(p.n), g(p.n), w(p.n), pos(p.n), neg(p.n), load(p.n),
        y(p.layout.factors()), fy(p.layout.factors()) {}
  std::vector<f64> v, tau, g, w, pos, neg, load, y, fy;
  f64 p_min{}, p_max{}, m_min{}, m_max{}; // side sums at +-inf multipliers (bounds only)
  f64 step{1.0};      // initial expansion step of the side roots
  f64 beta_curv{1.0}; // sum tau beta^2 >= |d beta'w / d rho|: the beta root's first guess
  usize passes{};
};

// Positive part of every name at alpha+ = a (and beta multiplier rho): the minimizer over
// [max(lower, 0), upper] of the name's function with linear coefficient a + rho beta + l.
// Returns (sum, d sum / d a).
std::pair<f64, f64> positive_side(const Problem& p, Scratch& s, f64 a, f64 rho) {
  const f64* v = s.v.data(); const f64* tau = s.tau.data();
  const f64* lo = p.lower.data(); const f64* hi = p.upper.data();
  const f64* lin = p.linear_cost.data(); const f64* eta = p.impact_cost.data();
  const f64* rate = p.long_rate.data(); const f64* beta = p.beta.data();
  const f64* w0 = p.w0.data();
  f64* out = s.pos.data();
  f64 sum = 0, slope = 0;
  for (usize i = 0; i < p.n; ++i) {
    const f64 floor = lo[i] > 0 ? lo[i] : 0.0;
    if (hi[i] < floor) { out[i] = 0.0; continue; } // always negative
    const Min1d m = minimize_1d(v[i], tau[i], a + rho * beta[i] + rate[i], lin[i], eta[i], w0[i]);
    f64 x = m.w, dx = m.slope;
    if (!(x > floor)) { x = floor; dx = 0; } else if (x >= hi[i]) { x = hi[i]; dx = 0; }
    out[i] = x; sum += x; slope += dx;
  }
  ++s.passes;
  return {sum, slope};
}
// Negative part at alpha- = a: the minimizer over [lower, min(upper, 0)] with linear
// coefficient rho beta - a - b. Returns (sum of -w, d / d a), both >= 0 resp. <= 0.
std::pair<f64, f64> negative_side(const Problem& p, Scratch& s, f64 a, f64 rho) {
  const f64* v = s.v.data(); const f64* tau = s.tau.data();
  const f64* lo = p.lower.data(); const f64* hi = p.upper.data();
  const f64* lin = p.linear_cost.data(); const f64* eta = p.impact_cost.data();
  const f64* rate = p.short_rate.data(); const f64* beta = p.beta.data();
  const f64* w0 = p.w0.data();
  f64* out = s.neg.data();
  f64 sum = 0, slope = 0;
  for (usize i = 0; i < p.n; ++i) {
    const f64 ceiling = hi[i] < 0 ? hi[i] : 0.0;
    if (ceiling < lo[i]) { out[i] = 0.0; continue; } // always positive
    const Min1d m = minimize_1d(v[i], tau[i], rho * beta[i] - a - rate[i], lin[i], eta[i], w0[i]);
    f64 x = m.w, dx = m.slope;
    if (!(x < ceiling)) { x = ceiling; dx = 0; } else if (x <= lo[i]) { x = lo[i]; dx = 0; }
    out[i] = x; sum -= x; slope += dx;
  }
  ++s.passes;
  return {sum, slope};
}
void side_limits(const Problem& p, Scratch& s) {
  s.p_min = s.p_max = s.m_min = s.m_max = 0;
  for (usize i = 0; i < p.n; ++i) {
    const f64 lo = p.lower[i], hi = p.upper[i];
    const f64 floor = lo > 0 ? lo : 0.0, ceiling = hi < 0 ? hi : 0.0;
    if (hi >= floor) { s.p_min += floor; s.p_max += hi; }
    if (ceiling >= lo) { s.m_min -= ceiling; s.m_max -= lo; }
  }
}
void assemble(const Problem& p, Scratch& s) {
  for (usize i = 0; i < p.n; ++i) s.w[i] = s.pos[i] > 0 ? s.pos[i] : s.neg[i];
}

// Root of a continuous nonincreasing f: eval(x) -> (f(x), slope or NaN). Newton (or the
// secant of the last two evaluations) inside the bracket, bisection when it leaves the
// bracket or the bracket stalls, doubling expansion until bracketed. [lo, hi] is an optional
// known bracket (f(lo) >= target >= f(hi)). The caller's scratch holds eval(returned x).
struct Root {
  f64 x;
  bool met;
};
Root monotone_root(const std::function<std::pair<f64, f64>(f64)>& eval, f64 target, f64 x0,
                   f64 step, f64 tol, usize limit, f64 lo = -inf, f64 hi = inf) {
  if (!(step > 0) || !std::isfinite(step)) step = 1.0;
  f64 x = x0, last = x0, prev_x = nan, prev_f = nan, width_ref = inf;
  usize stall = 0;
  for (usize k = 0; k < limit; ++k) {
    const auto [f, slope_in] = eval(x);
    last = x;
    const f64 r = f - target;
    if (std::abs(r) <= tol) return {x, true};
    if (r > 0 || std::isnan(r)) lo = x; else hi = x;
    f64 slope = slope_in;
    if (!(slope < 0) && std::isfinite(prev_x) && prev_x != x) {
      const f64 secant = (f - prev_f) / (x - prev_x);
      if (secant < 0 && std::isfinite(secant)) slope = secant;
    }
    prev_x = x; prev_f = f;
    f64 next = slope < 0 && std::isfinite(slope) && std::isfinite(r) ? x - r / slope : nan;
    if (std::isfinite(lo) && std::isfinite(hi)) {
      const f64 width = hi - lo;
      if (!(width > 4.0 * eps * std::max({std::abs(lo), std::abs(hi), 1e-300}))) break;
      if (width <= 0.5 * width_ref) { width_ref = width; stall = 0; } else { ++stall; }
      if (!(next > lo && next < hi) || stall >= 4) { next = lo + 0.5 * width; stall = 0; }
    } else if (r > 0) { // need a larger x, no upper end yet
      if (!(next > x) || !std::isfinite(next)) next = x + step;
      next = std::min(next, x + 16.0 * step);
      step *= 2.0;
    } else {
      if (!(next < x) || !std::isfinite(next)) next = x - step;
      next = std::max(next, x - 16.0 * step);
      step *= 2.0;
    }
    x = next;
  }
  if (x != last) eval(last); // the scratch must hold the returned point
  return {last, false};
}

// The coupled prox at fixed beta multiplier rho (net and gross by the two side roots).
// Returns whether the net/gross targets were attainable and met.
bool prox_inner(const Problem& p, Scratch& s, Multipliers& m, f64 rho) {
  const f64 net = p.net, gross = p.gross;
  const auto run_a = [&]() { // gross binds: P = (G + net) / 2, M = (G - net) / 2
    const f64 pt = 0.5 * (gross + net), mt = 0.5 * (gross - net);
    const f64 ptc = clamp_to(pt, s.p_min, s.p_max), mtc = clamp_to(mt, s.m_min, s.m_max);
    const auto rp = monotone_root([&](f64 a) { return positive_side(p, s, a, rho); }, ptc, m.pos,
                                  s.step, root_tolerance * std::max(1.0, std::abs(ptc)),
                                  max_root_steps);
    const auto rm = monotone_root([&](f64 a) { return negative_side(p, s, a, rho); }, mtc, m.neg,
                                  s.step, root_tolerance * std::max(1.0, std::abs(mtc)),
                                  max_root_steps);
    m.pos = rp.x; m.neg = rm.x;
    return rp.met && rm.met && ptc == pt && mtc == mt;
  };
  f64 gross_now = 0;
  const auto run_b = [&]() { // gross slack: mu = 0, alpha- = -alpha+, P(a) - M(-a) = net
    const f64 lo = s.p_min - s.m_max, hi = s.p_max - s.m_min;
    const f64 tc = clamp_to(net, lo, hi);
    const auto r = monotone_root(
        [&](f64 a) {
          const auto [pv, ps] = positive_side(p, s, a, rho);
          const auto [mv, ms] = negative_side(p, s, -a, rho);
          gross_now = pv + mv;
          return std::pair<f64, f64>{pv - mv, ps + ms};
        },
        tc, m.pos, s.step, root_tolerance * std::max(1.0, std::abs(tc)), max_root_steps);
    m.pos = r.x; m.neg = -r.x;
    return r.met && tc == net;
  };
  if (!(gross < inf)) { m.gross_binding = false; return run_b(); }
  if (m.gross_binding) {
    const bool met = run_a();
    if (m.pos + m.neg >= 0) return met; // mu >= 0: the gross budget binds
  }
  const bool met = run_b();
  if (gross_now <= gross * (1.0 + 1e-12) + root_tolerance) { m.gross_binding = false; return met; }
  m.gross_binding = true;
  return run_a();
}
f64 beta_exposure(const Problem& p, const Scratch& s) {
  f64 sum = 0;
  for (usize i = 0; i < p.n; ++i) sum += p.beta[i] * s.w[i];
  return sum;
}
// The full prox: s.w = argmin sum_i psi_i(w_i) + (1/(2 tau_i))(w_i - v_i)^2 subject to the
// net, gross and beta constraints. The beta multiplier rho is the outer monotone root of
// Phi(rho) = beta'w(rho) - hi (rho > 0) / - lo (rho < 0); rho = 0 when beta'w(0) is inside.
bool coupled_prox(const Problem& p, Scratch& s, Multipliers& m) {
  const f64 lo = p.beta_lo, hi = p.beta_hi;
  if (!(lo > -inf) && !(hi < inf)) {
    m.rho = 0;
    const bool met = prox_inner(p, s, m, 0.0);
    assemble(p, s);
    return met;
  }
  bool inner_met = true;
  const auto g_at = [&](f64 rho) {
    inner_met = prox_inner(p, s, m, rho);
    assemble(p, s);
    return beta_exposure(p, s);
  };
  // Phi(rho) = beta'w(rho) - bound on one side of 0 (g nonincreasing in rho); first guesses
  // from the curvature bound |dg/drho| <= sum tau beta^2.
  const f64 curv = std::max(s.beta_curv, 1e-300);
  const f64 rho0 = m.rho;
  const f64 g0 = g_at(rho0);
  f64 bound = 0, blo = -inf, bhi = inf, start = 0;
  if (rho0 == 0) {
    if (g0 >= lo && g0 <= hi) return inner_met;
    bound = g0 > hi ? hi : lo;
    (g0 > hi ? blo : bhi) = 0.0;
    start = (g0 - bound) / curv;
  } else {
    bound = rho0 > 0 ? hi : lo;
    const f64 r0 = g0 - bound;
    if (std::abs(r0) <= beta_tolerance) return inner_met;
    const bool toward_zero = rho0 > 0 ? r0 < 0 : r0 > 0;
    if (!toward_zero) {
      (rho0 > 0 ? blo : bhi) = rho0;
      start = rho0 + r0 / curv;
    } else {
      const f64 gz = g_at(0.0);
      if (gz >= lo && gz <= hi) { m.rho = 0; return inner_met; }
      if ((rho0 > 0) == (gz > hi)) { // the root lies between 0 and rho0
        if (rho0 > 0) { blo = 0.0; bhi = rho0; } else { blo = rho0; bhi = 0.0; }
        start = 0.5 * rho0;
      } else { // the root lies on the other side of 0
        bound = gz > hi ? hi : lo;
        (gz > hi ? blo : bhi) = 0.0;
        start = (gz - bound) / curv;
      }
    }
  }
  const f64 anchor = std::isfinite(blo) ? blo : bhi;
  const auto r = monotone_root([&](f64 rho) { return std::pair<f64, f64>{g_at(rho), nan}; }, bound,
                               start, std::max(std::abs(start - anchor), 1e-300), beta_tolerance,
                               max_beta_steps, blo, bhi);
  m.rho = r.x;
  return inner_met && r.met;
}

void set_steps(const Problem& p, Scratch& s, f64 scale) {
  s.beta_curv = 0;
  for (usize i = 0; i < p.n; ++i) {
    s.tau[i] = 1.0 / (scale * p.gamma * p.specific[i]);
    s.beta_curv += s.tau[i] * p.beta[i] * p.beta[i];
  }
}
void set_root_steps(const Problem& p, Scratch& s, std::span<const f64> start) {
  f64 weight = 1e-4, metric = 0;
  for (usize i = 0; i < p.n; ++i) {
    weight = std::max({weight, std::abs(start[i]), std::abs(p.w0[i])});
    metric = std::max(metric, 1.0 / s.tau[i]);
  }
  s.step = std::max(weight * metric, 1e-300);
}
// g = grad f(w) = -a + gamma (X F (X'w + fixed) + D w), with z = X'w given.
void gradient(const Problem& p, Scratch& s, const f64* w, const f64* z) {
  const usize k = p.layout.factors();
  for (usize c = 0; c < k; ++c) s.y[c] = z[c] + p.fixed_exposure[c];
  cov_apply(p, s.y.data(), s.fy.data());
  loads(p, s.fy.data(), s.load.data());
  const f64* a = p.alpha.data(); const f64* d = p.specific.data(); const f64* l = s.load.data();
  f64* g = s.g.data();
  for (usize i = 0; i < p.n; ++i) g[i] = -a[i] + p.gamma * (l[i] + d[i] * w[i]);
}
f64 prox_residual(const Problem& p, Scratch& s, Multipliers& m, const std::vector<f64>& w,
                  const std::vector<f64>& z) {
  gradient(p, s, w.data(), z.data());
  for (usize i = 0; i < p.n; ++i) s.v[i] = w[i] - s.tau[i] * s.g[i];
  coupled_prox(p, s, m);
  f64 r = 0;
  for (usize i = 0; i < p.n; ++i) r = std::max(r, std::abs(w[i] - s.w[i]));
  return r;
}
bool coupling_met(const Problem& p, std::span<const f64> w) {
  f64 net = 0, gross = 0, beta = 0;
  for (usize i = 0; i < p.n; ++i) {
    net += w[i]; gross += std::abs(w[i]); beta += p.beta[i] * w[i];
  }
  return std::abs(net - p.net) <= coupling_tolerance && gross <= p.gross + coupling_tolerance &&
         beta >= p.beta_lo - coupling_tolerance && beta <= p.beta_hi + coupling_tolerance;
}
} // namespace

// ---- parameters and problem validation ---------------------------------------------------
SpoParams v2_params() {
  SpoParams p;
  p.alpha_horizon = 21.0;   // the IC's measurement horizon, independent of H
  p.gross_budget = 1.0;     // a hard cap on planned gross (R6': mean gross in [.90, 1.05])
  p.specific_ceiling = 1.0; // daily specific vol 100%, a tripwire:
  p.void_on_capped = true;  // one clamped entry voids the run
  p.gamma_rule = GammaRule::Vol;
  p.version = 2;
  return p;
}
const char* rule_name(const SpoParams& p) {
  return p.version == 3 ? "spo-v3" : p.version == 2 ? "spo-v2" : "spo-v1";
}
const char* json_key(const SpoParams& p) {
  return p.version == 3 ? "spo_v3" : p.version == 2 ? "spo_v2" : "spo_v1";
}

co::Status validate_params(const SpoParams& p) {
  const bool gamma_ok = std::isnan(p.gamma) || (finite_positive(p.gamma) && p.gamma <= 1e12);
  const bool horizon_ok = std::isnan(p.horizon) || (p.horizon >= 1 && p.horizon <= 10000);
  const bool alpha_ok = p.alpha_horizon >= 1 && p.alpha_horizon <= 10000; // NaN refused
  const bool budget_ok = std::isnan(p.gross_budget) || finite_positive(p.gross_budget);
  if (!gamma_ok || !horizon_ok || !alpha_ok || !budget_ok || !(p.ic_book > 0 && p.ic_book <= 1) ||
      !(p.w_max > 0 && p.w_max <= 1) || !(p.adv_cap_q > 0 && p.adv_cap_q <= 1) ||
      !(p.adv_trade_p > 0 && p.adv_trade_p <= 1) || p.max_iterations == 0 ||
      p.max_iterations > 100000 || !(p.tolerance > 0 && p.tolerance <= 1e-3) ||
      !(p.target_vol > 0 && p.target_vol <= 1) || !(p.beta_max >= 0 && p.beta_max <= 1) ||
      !(p.specific_ceiling > 0) || p.version < 1 || p.version > 3)
    return co::Err(co::ErrorCode::InvalidArgument,
                   "spo: gamma > 0, IC in (0, 1], w_max/q/p in (0, 1], 1..100000 iterations, "
                   "tolerance in (0, 1e-3], target vol in (0, 1], horizons in [1, 10000] "
                   "(--alpha-horizon a number), specific ceiling > 0, --spo-gross > 0");
  if (p.version == 3 && !(finite_positive(p.sharpe_prior) && p.sharpe_prior <= 1e3))
    return co::Err(co::ErrorCode::InvalidArgument, "spo-v3: S_prior finite in (0, 1e3]");
  return co::Ok();
}
f64 gross_budget_of(const SpoParams& p, f64 aim_leverage) noexcept {
  return std::isnan(p.gross_budget) ? aim_leverage : p.gross_budget;
}
co::Status validate_gross_budget(f64 budget, f64 aim_leverage) {
  if (!(finite_positive(budget) && budget <= gross_budget_sanity * aim_leverage))
    return co::Err(co::ErrorCode::InvalidArgument,
                   "spo: --spo-gross " + std::to_string(budget) +
                       " outside (0, 1.5 x --aim-leverage " + std::to_string(aim_leverage) + "]");
  return co::Ok();
}

f64 member_sd(std::span<const f64> desired, std::span<const u8> member) {
  const usize n = std::min(desired.size(), member.size());
  f64 sum = 0, sq = 0;
  usize members = 0;
  for (usize i = 0; i < n; ++i) {
    if (!member[i]) continue;
    ++members;
    sum += desired[i]; sq += desired[i] * desired[i];
  }
  const f64 count = static_cast<f64>(members);
  const f64 var = members > 1 ? (sq - sum * sum / count) / (count - 1.0) : 0.0;
  return var > 0 ? std::sqrt(var) : 0.0;
}

co::Status validate_problem(const Problem& p) {
  const usize n = p.n, k = p.layout.factors();
  const auto sized = [n](const std::vector<f64>& v) { return v.size() == n; };
  if (p.industry.size() != n || p.styles.size() != n * p.layout.styles ||
      p.covariance.size() != k * k || p.fixed_exposure.size() != k || !sized(p.specific) ||
      !sized(p.alpha) || !sized(p.beta) || !sized(p.w0) || !sized(p.lower) || !sized(p.upper) ||
      !sized(p.linear_cost) || !sized(p.impact_cost) || !sized(p.long_rate) || !sized(p.short_rate))
    return co::Err(co::ErrorCode::InvalidArgument, "spo: problem geometry");
  if (!finite_positive(p.gamma) || !std::isfinite(p.net) || !(p.gross >= 0) ||
      !(p.beta_lo <= p.beta_hi) || std::isnan(p.beta_lo) || std::isnan(p.beta_hi))
    return co::Err(co::ErrorCode::InvalidArgument, "spo: gamma, net, gross or beta bounds");
  for (const f64 f : p.covariance)
    if (!std::isfinite(f)) return co::Err(co::ErrorCode::InvalidArgument, "spo: covariance");
  for (const f64 f : p.fixed_exposure)
    if (!std::isfinite(f)) return co::Err(co::ErrorCode::InvalidArgument, "spo: fixed exposure");
  for (const f64 f : p.styles)
    if (!std::isfinite(f)) return co::Err(co::ErrorCode::InvalidArgument, "spo: styles");
  for (usize i = 0; i < n; ++i) {
    const bool ok = p.industry[i] <= p.layout.industries && finite_positive(p.specific[i]) &&
        std::isfinite(p.alpha[i]) && std::isfinite(p.beta[i]) && std::isfinite(p.w0[i]) &&
        p.lower[i] <= p.upper[i] && p.lower[i] < inf && p.upper[i] > -inf &&
        std::isfinite(p.linear_cost[i]) && p.linear_cost[i] >= 0 &&
        std::isfinite(p.impact_cost[i]) && p.impact_cost[i] >= 0 &&
        std::isfinite(p.long_rate[i]) && p.long_rate[i] >= 0 &&
        std::isfinite(p.short_rate[i]) && p.short_rate[i] >= 0;
    if (!ok) return co::Err(co::ErrorCode::InvalidArgument, "spo: name " + std::to_string(i));
  }
  return co::Ok();
}

// ---- metric scale ------------------------------------------------------------------------
f64 estimate_metric_scale(const Problem& p) {
  const usize n = p.n, k = p.layout.factors();
  if (n < 2) return 1.0;
  f64 inverse = 0;
  for (usize i = 0; i < n; ++i) inverse += 1.0 / p.specific[i];
  // D-orthogonal projection onto 1'd = 0: d - D^{-1} 1 (1'd) / (1'D^{-1} 1).
  const auto project = [&](std::vector<f64>& d) {
    f64 sum = 0;
    for (const f64 v : d) sum += v;
    const f64 c = sum / inverse;
    for (usize i = 0; i < n; ++i) d[i] -= c / p.specific[i];
  };
  std::vector<f64> d(n), y(k), u(k), e(n);
  u64 state = 0x9E3779B97F4A7C15ULL;
  for (usize i = 0; i < n; ++i) { // deterministic start in [-1, 1)
    state = state * 6364136223846793005ULL + 1442695040888963407ULL;
    d[i] = 2.0 * static_cast<f64>(state >> 11U) * 0x1.0p-53 - 1.0;
  }
  project(d);
  f64 lambda = 0;
  for (usize step = 0; step < power_steps; ++step) {
    f64 norm = 0;
    for (usize i = 0; i < n; ++i) norm += p.specific[i] * d[i] * d[i];
    if (!(norm > 0) || !std::isfinite(norm)) break;
    norm = std::sqrt(norm);
    for (f64& v : d) v /= norm;
    exposures(p, d.data(), y.data());
    const f64 rq = quadratic(p, y.data(), u.data()); // d'XFX'd / d'Dd with d'Dd = 1
    if (std::isfinite(rq)) lambda = std::max(lambda, rq);
    loads(p, u.data(), e.data());
    for (usize i = 0; i < n; ++i) d[i] = e[i] / p.specific[i];
    project(d);
  }
  return 1.0 + 1.1 * lambda;
}

// ---- FISTA with adaptive restart and backtracking ------------------------------------------
co::Result<Solution> solve(const Problem& p, std::span<const f64> start, const SolverOptions& o,
                           const Multipliers& warm) {
  ATX_TRY_VOID(validate_problem(p));
  if (start.size() != p.n || o.max_iterations == 0 || !(o.tolerance > 0))
    return co::Err(co::ErrorCode::InvalidArgument, "spo: start size, iterations or tolerance");
  for (const f64 v : start)
    if (!std::isfinite(v)) return co::Err(co::ErrorCode::InvalidArgument, "spo: start");
  Solution sol;
  sol.multipliers = warm;
  for (f64* m : {&sol.multipliers.pos, &sol.multipliers.neg, &sol.multipliers.rho})
    if (!std::isfinite(*m)) *m = 0.0;
  const usize n = p.n, k = p.layout.factors();
  f64 scale = std::isfinite(o.metric_scale) && o.metric_scale >= 1 ? o.metric_scale
                                                                  : estimate_metric_scale(p);
  if (n == 0) {
    sol.converged = true; sol.metric_scale = scale;
    sol.coupling_met = coupling_met(p, sol.w);
    return co::Ok(std::move(sol));
  }
  Scratch s(p);
  set_steps(p, s, scale);
  side_limits(p, s);
  set_root_steps(p, s, start);
  // Feasible start: the prox of the warm start.
  std::copy(start.begin(), start.end(), s.v.begin());
  coupled_prox(p, s, sol.multipliers);
  std::vector<f64> x(s.w), x_prev(x), y(x), z(k), z_prev(k), zy(k), z_new(k), dz(k), fdz(k);
  exposures(p, x.data(), z.data());
  z_prev = z; zy = z;
  f64 t = 1.0;
  bool converged = false;
  usize iterations = 0;
  while (iterations < o.max_iterations) {
    gradient(p, s, y.data(), zy.data());
    for (;;) { // backtracking on the exact quadratic model (f is quadratic)
      for (usize i = 0; i < n; ++i) s.v[i] = y[i] - s.tau[i] * s.g[i];
      coupled_prox(p, s, sol.multipliers);
      exposures(p, s.w.data(), z_new.data());
      for (usize c = 0; c < k; ++c) dz[c] = z_new[c] - zy[c];
      const f64 factor = quadratic(p, dz.data(), fdz.data());
      f64 specific = 0;
      for (usize i = 0; i < n; ++i) {
        const f64 d = s.w[i] - y[i];
        specific += p.specific[i] * d * d;
      }
      if (sol.backtracks >= max_backtracks || !(specific > 0) ||
          factor <= (scale - 1.0) * specific * (1.0 + 1e-9))
        break;
      scale = 1.0 + 1.25 * factor / specific;
      set_steps(p, s, scale);
      ++sol.backtracks;
    }
    ++iterations;
    f64 restart = 0, step = 0;
    for (usize i = 0; i < n; ++i) {
      restart += (y[i] - s.w[i]) * (s.w[i] - x[i]) / s.tau[i];
      step = std::max(step, std::abs(s.w[i] - y[i]));
    }
    x_prev.swap(x); x = s.w;
    z_prev.swap(z); z = z_new;
    if (step <= o.tolerance) {
      const f64 r = prox_residual(p, s, sol.multipliers, x, z);
      if (r <= o.tolerance) { converged = true; sol.residual = r; break; }
    }
    if (restart > 0) { t = 1.0; ++sol.restarts; }
    const f64 t_next = 0.5 * (1.0 + std::sqrt(1.0 + 4.0 * t * t));
    const f64 coef = (t - 1.0) / t_next;
    t = t_next;
    for (usize i = 0; i < n; ++i) y[i] = x[i] + coef * (x[i] - x_prev[i]);
    for (usize c = 0; c < k; ++c) zy[c] = z[c] + coef * (z[c] - z_prev[c]);
  }
  if (!converged) sol.residual = prox_residual(p, s, sol.multipliers, x, z);
  sol.iterations = iterations;
  sol.converged = converged;
  sol.metric_scale = scale;
  sol.prox_passes = s.passes;
  sol.coupling_met = coupling_met(p, x);
  sol.w = std::move(x);
  return co::Ok(std::move(sol));
}

co::Result<f64> kkt_residual(const Problem& p, std::span<const f64> w, f64 metric_scale) {
  ATX_TRY_VOID(validate_problem(p));
  if (w.size() != p.n || !(metric_scale >= 1))
    return co::Err(co::ErrorCode::InvalidArgument, "spo: residual point or scale");
  if (p.n == 0) return co::Ok(0.0);
  Scratch s(p);
  set_steps(p, s, metric_scale);
  side_limits(p, s);
  set_root_steps(p, s, w);
  const std::vector<f64> at(w.begin(), w.end());
  std::vector<f64> z(p.layout.factors());
  exposures(p, at.data(), z.data());
  Multipliers m;
  return co::Ok(prox_residual(p, s, m, at, z));
}

Terms objective_terms(const Problem& p, std::span<const f64> w) {
  Terms t;
  if (w.size() != p.n) return t;
  const usize k = p.layout.factors();
  std::vector<f64> y(k), u(k);
  exposures(p, w.data(), y.data());
  for (usize c = 0; c < k; ++c) y[c] += p.fixed_exposure[c];
  t.variance = quadratic(p, y.data(), u.data());
  for (usize i = 0; i < p.n; ++i) {
    const f64 d = std::abs(w[i] - p.w0[i]);
    t.alpha += p.alpha[i] * w[i];
    t.variance += p.specific[i] * w[i] * w[i];
    t.cost += p.linear_cost[i] * d + p.impact_cost[i] * d * std::sqrt(d);
    t.financing += w[i] > 0 ? p.long_rate[i] * w[i] : -p.short_rate[i] * w[i];
  }
  t.risk = 0.5 * p.gamma * t.variance;
  t.objective = t.alpha - t.risk - t.cost - t.financing;
  return t;
}

// ---- atx-risk-v1 store -------------------------------------------------------------------
namespace {
constexpr const char* covariance_file = "factor_covariance.f64";
constexpr const char* specific_file = "specific_variance.f64";
constexpr const char* styles_file = "style_exposures.f32";
constexpr const char* slot_file = "industry_slot.u8";
constexpr const char* diagnostics_file = "diagnostics.csv";

co::Status read_bytes(const std::filesystem::path& path, u64 offset, char* out, u64 bytes) {
  std::ifstream file(path, std::ios::binary);
  if (!file) return co::Err(co::ErrorCode::IoError, "spo-v1: cannot open " + path.string());
  file.seekg(static_cast<std::streamoff>(offset));
  file.read(out, static_cast<std::streamsize>(bytes));
  if (!file || static_cast<u64>(file.gcount()) != bytes)
    return co::Err(co::ErrorCode::IoError, "spo-v1: short read " + path.string());
  return co::Ok();
}
} // namespace

co::Result<RiskStore> RiskStore::open(const std::string& directory,
                                      const std::string& manifest_sha256,
                                      const std::string& role_sha256) {
  namespace fs = std::filesystem;
  const fs::path base(directory);
  const auto manifest_path = base / "manifest.json";
  if (manifest_sha256.empty() || role_sha256.empty())
    return co::Err(co::ErrorCode::InvalidArgument, "spo-v1: --risk-model-sha256 and the role pin");
  ATX_TRY(const auto sha, co::sha256_file(manifest_path.string()));
  if (sha != manifest_sha256)
    return co::Err(co::ErrorCode::InvalidArgument,
                   "spo-v1: risk model manifest SHA-256 " + sha + " != pinned " + manifest_sha256);
  RiskStore store;
  store.directory_ = directory;
  store.manifest_sha256_ = manifest_sha256;
  try {
    std::ifstream in(manifest_path, std::ios::binary);
    const Json m = Json::parse(in);
    if (m.at("schema").get<std::string>() != "atx.risk-model/v1" ||
        m.at("status").get<std::string>() != "complete")
      return co::Err(co::ErrorCode::InvalidArgument, "spo-v1: not a complete atx.risk-model/v1");
    if (m.at("role").at("manifest_sha256").get<std::string>() != role_sha256)
      return co::Err(co::ErrorCode::InvalidArgument,
                     "spo-v1: the risk model was fitted on another role (manifest_sha256)");
    const auto& geometry = m.at("geometry");
    store.dates_ = geometry.at("dates").get<usize>();
    store.instruments_ = geometry.at("instruments").get<usize>();
    if (geometry.at("factors").get<usize>() != risk_factors ||
        geometry.at("styles").get<usize>() != risk_styles || store.dates_ == 0 ||
        store.instruments_ == 0)
      return co::Err(co::ErrorCode::InvalidArgument, "spo-v1: risk model geometry");
    const u64 dates = store.dates_, names = store.instruments_;
    const std::pair<const char*, u64> files[] = {
        {covariance_file, dates * risk_factors * risk_factors * sizeof(f64)},
        {specific_file, dates * names * sizeof(f64)},
        {styles_file, dates * names * risk_styles * sizeof(f32)},
        {slot_file, dates * names},
        {diagnostics_file, 0}};
    const auto& listed = m.at("files");
    for (const auto& [name, bytes] : files) {
      if (!listed.contains(name))
        return co::Err(co::ErrorCode::InvalidArgument,
                       std::string("spo-v1: the risk model lacks ") + name +
                           " (run the risk verb with --emit-exposures all)");
      const auto path = base / name;
      const u64 size = fs::file_size(path);
      if (size != listed.at(name).at("bytes").get<u64>() || (bytes != 0 && size != bytes))
        return co::Err(co::ErrorCode::InvalidArgument, std::string("spo-v1: size of ") + name);
      ATX_TRY(const auto file_sha, co::sha256_file(path.string()));
      if (file_sha != listed.at(name).at("sha256").get<std::string>())
        return co::Err(co::ErrorCode::InvalidArgument, std::string("spo-v1: SHA-256 of ") + name);
    }
    std::ifstream csv(base / diagnostics_file, std::ios::binary);
    std::string line;
    if (!std::getline(csv, line) || line.rfind("session,forecast,", 0) != 0)
      return co::Err(co::ErrorCode::ParseError, "spo-v1: diagnostics.csv header");
    while (std::getline(csv, line)) {
      if (line.empty()) continue;
      const auto first = line.find(','), second = line.find(',', first + 1);
      if (first == std::string::npos || second == std::string::npos)
        return co::Err(co::ErrorCode::ParseError, "spo-v1: diagnostics.csv row");
      usize used = 0;
      const std::string session = line.substr(0, first);
      const i64 key = std::stoll(session, &used);
      const std::string flag = line.substr(first + 1, second - first - 1);
      if (used != session.size() || (flag != "0" && flag != "1"))
        return co::Err(co::ErrorCode::ParseError, "spo-v1: diagnostics.csv row");
      store.sessions_.push_back(key);
      store.forecast_.push_back(flag == "1" ? u8{1} : u8{0});
    }
    if (store.sessions_.size() != store.dates_)
      return co::Err(co::ErrorCode::InvalidArgument, "spo-v1: diagnostics.csv rows != dates");
  } catch (const std::exception& e) {
    return co::Err(co::ErrorCode::ParseError, std::string("spo-v1: risk model: ") + e.what());
  }
  return co::Ok(std::move(store));
}

co::Status RiskStore::check_axes(std::span<const i64> sessions, usize instruments) const {
  if (instruments != instruments_ || sessions.size() != dates_ ||
      !std::equal(sessions.begin(), sessions.end(), sessions_.begin()))
    return co::Err(co::ErrorCode::InvalidArgument,
                   "spo-v1: the replay's sessions/instruments are not the risk model's");
  return co::Ok();
}

co::Status RiskStore::read(usize d, RiskSlice& out) const {
  if (d >= dates_)
    return co::Err(co::ErrorCode::Unavailable, "spo-v1: the risk model has no date " +
                                                   std::to_string(d));
  if (!forecast_[d])
    return co::Err(co::ErrorCode::Unavailable,
                   "spo-v1: the risk model has no forecast at session " +
                       std::to_string(sessions_[d]));
  namespace fs = std::filesystem;
  const fs::path base(directory_);
  const u64 n = instruments_, date = d;
  constexpr u64 cells = risk_factors * risk_factors;
  out.date = d; out.session = sessions_[d];
  out.covariance.resize(cells);
  out.specific.resize(n);
  out.slot.resize(n);
  out.styles.resize(n * risk_styles);
  std::vector<f32> narrow(n * risk_styles);
  // SAFETY: char views of trivially copyable f64/f32/u8 arrays sized exactly `bytes`; the
  // payloads are little-endian (static_assert above) and validated in size at open().
  ATX_TRY_VOID(read_bytes(base / covariance_file, date * cells * sizeof(f64),
                          reinterpret_cast<char*>(out.covariance.data()), cells * sizeof(f64)));
  ATX_TRY_VOID(read_bytes(base / specific_file, date * n * sizeof(f64),
                          reinterpret_cast<char*>(out.specific.data()), n * sizeof(f64)));
  ATX_TRY_VOID(read_bytes(base / styles_file, date * n * risk_styles * sizeof(f32),
                          reinterpret_cast<char*>(narrow.data()), n * risk_styles * sizeof(f32)));
  ATX_TRY_VOID(read_bytes(base / slot_file, date * n, reinterpret_cast<char*>(out.slot.data()), n));
  out.nan_covariance_entries = 0;
  for (f64& c : out.covariance)
    if (!std::isfinite(c)) { c = 0.0; ++out.nan_covariance_entries; }
  for (usize k = 0; k < narrow.size(); ++k) {
    const f64 v = static_cast<f64>(narrow[k]);
    out.styles[k] = std::isfinite(v) ? v : 0.0;
  }
  return co::Ok();
}

usize cap_specific(RiskSlice& r, f64 ceiling) {
  usize capped = 0;
  for (f64& v : r.specific)
    if (std::isfinite(v) && v > ceiling) { v = ceiling; ++capped; }
  r.capped_specific = capped;
  return capped;
}

// ---- gamma calibration -----------------------------------------------------------------------
co::Status calibrate_gamma(const Problem& base, const SpoParams& params, f64 budget,
                           f64 metric_scale, Calibration& calibration) {
  Problem c = base;
  const usize n = c.n;
  f64 a2d = 0, ad = 0;
  for (usize j = 0; j < n; ++j) {
    a2d += c.alpha[j] * c.alpha[j] / c.specific[j];
    ad += std::abs(c.alpha[j]) / c.specific[j];
  }
  if (n < 2 || !finite_positive(a2d) || !finite_positive(budget))
    return co::Err(co::ErrorCode::Unavailable,
                   "spo: the first rebalance decision has no alpha to calibrate gamma "
                   "(pass --gamma)");
  calibration.rule = params.gamma_rule;
  c.w0.assign(n, 0.0); c.lower.assign(n, -params.w_max); c.upper.assign(n, params.w_max);
  c.linear_cost.assign(n, 0.0); c.impact_cost.assign(n, 0.0);
  c.long_rate.assign(n, 0.0); c.short_rate.assign(n, 0.0);
  c.net = 0; c.gross = inf; c.beta_lo = -params.beta_max; c.beta_hi = params.beta_max;
  c.fixed_exposure.assign(c.layout.factors(), 0.0);
  const SolverOptions options{params.max_iterations * 4, params.tolerance, metric_scale};
  std::vector<f64> start(n, 0.0);
  Multipliers warm;
  f64 last = nan, vol = nan, gross = nan;
  co::Status failure = co::Ok();
  const auto evaluate = [&](f64 g) {
    c.gamma = g;
    if (std::isfinite(last)) for (f64& v : start) v *= last / g;
    auto sol = solve(c, start, options, warm);
    if (!sol) { failure = co::Err(sol.error()); return false; }
    ++calibration.evaluations;
    start = sol->w; warm = sol->multipliers; last = g;
    vol = std::sqrt(sessions_per_year * objective_terms(c, sol->w).variance);
    gross = 0;
    for (const f64 w : sol->w) gross += std::abs(w);
    return true;
  };
  // ln q(gamma) - ln target is nonincreasing in u = ln gamma for q = the aim's vol (Markowitz
  // variance is nonincreasing in risk aversion) and its gross; bracketed at guess x 1e-4..1e4.
  const auto root = [&](bool vol_target, f64 target, f64 guess, bool& reached) {
    const auto f = [&](f64 u) {
      if (!evaluate(std::exp(u))) return std::pair<f64, f64>{nan, nan};
      return std::pair<f64, f64>{std::log(vol_target ? vol : gross) - std::log(target), nan};
    };
    const f64 lo = std::log(guess) - std::log(1e4), hi = std::log(guess) + std::log(1e4);
    reached = false;
    const f64 flo = f(lo).first;
    if (!(flo >= 0)) return lo; // unreachable even at the least risk aversion
    const f64 fhi = f(hi).first;
    if (!(fhi <= 0)) return hi;
    const auto r = monotone_root(f, 0.0, std::log(guess), 1.0, 1e-7, 60, lo, hi);
    reached = r.met;
    return r.x;
  };
  const f64 vol_guess = std::sqrt(sessions_per_year * a2d) / params.target_vol;
  const f64 u_vol = root(true, params.target_vol, vol_guess, calibration.vol_reached);
  ATX_TRY_VOID(failure);
  calibration.gamma_vol = std::exp(u_vol);
  if (params.gamma_rule == GammaRule::VolAndBind) {
    // spo-v1 as pre-registered: the same evaluation sequence (vol root, bind root, aim).
    const f64 u_bind = root(false, budget, ad / budget, calibration.bind_reached);
    ATX_TRY_VOID(failure);
    calibration.gamma_bind = std::exp(u_bind);
    const f64 gamma = std::max(calibration.gamma_vol, calibration.gamma_bind);
    if (!evaluate(gamma)) return failure;
    calibration.aim_vol = vol; calibration.aim_gross = gross;
    calibration.gamma = gamma; calibration.names = n;
    calibration.done = true;
    return co::Ok();
  }
  // spo-v2: the aim at gamma_vol first, so the report-only bind root cannot move it.
  const f64 gamma = calibration.gamma_vol;
  if (!evaluate(gamma)) return failure;
  calibration.aim_vol = vol; calibration.aim_gross = gross;
  calibration.gamma = gamma; calibration.names = n;
  // spo-v2 declares the vol target: a bracket end (the target out of reach of the aim at
  // every gamma in guess x 1e-4..1e4) is refused, never used.
  if (!(std::abs(vol / params.target_vol - 1.0) <= 1e-3))
    return co::Err(co::ErrorCode::Unavailable,
                   "spo-v2: the cost-free aim cannot reach --target-vol (ex-ante vol " +
                       std::to_string(vol) + "); pass --gamma or a reachable target");
  // gamma_bind, report only (R2 m-3): a solver failure or a bracket end is recorded as NaN
  // with the reason; the run goes on with gamma_vol.
  const f64 u_bind = root(false, budget, ad / budget, calibration.bind_reached);
  if (!failure) {
    calibration.gamma_bind = nan;
    calibration.bind_note = "gamma_bind root failed: " + failure.error().to_string();
    calibration.bind_reached = false;
  } else if (!calibration.bind_reached) {
    calibration.gamma_bind = nan;
    calibration.bind_note = "gamma_bind root not reached: the cost-free aim's gross stays on "
                            "one side of G over gamma guess x 1e-4..1e4";
  } else {
    calibration.gamma_bind = std::exp(u_bind);
  }
  calibration.done = true;
  return co::Ok();
}

// ---- the replay rule -----------------------------------------------------------------------
namespace {
constexpr usize style_column = 1 + risk_industry_slots;
bool has_row(const RiskSlice& r, usize i) {
  return r.slot[i] < risk_industry_slots && finite_positive(r.specific[i]);
}
// y += w x_i (atx-risk-v1 row of instrument i).
void add_exposure(const RiskSlice& r, usize i, f64 w, std::vector<f64>& y) {
  y[0] += w;
  y[1 + r.slot[i]] += w;
  const f64* row = r.styles.data() + i * risk_styles;
  for (usize c = 0; c < risk_styles; ++c) y[style_column + c] += row[c] * w;
}
f64 load_of(const RiskSlice& r, usize i, const std::vector<f64>& u) {
  f64 v = u[0] + u[1 + r.slot[i]];
  const f64* row = r.styles.data() + i * risk_styles;
  for (usize c = 0; c < risk_styles; ++c) v += row[c] * u[style_column + c];
  return v;
}
// Variance of a whole book over the priced instruments: y'Fy + sum D w^2.
f64 book_variance(const RiskSlice& r, std::span<const f64> w) {
  std::vector<f64> y(risk_factors, 0.0);
  f64 specific = 0;
  for (usize i = 0; i < w.size(); ++i) {
    if (w[i] == 0 || !has_row(r, i)) continue;
    add_exposure(r, i, w[i], y);
    specific += r.specific[i] * w[i] * w[i];
  }
  f64 factor = 0;
  for (usize a = 0; a < risk_factors; ++a) {
    f64 row = 0;
    for (usize b = 0; b < risk_factors; ++b) row += r.covariance[a * risk_factors + b] * y[b];
    factor += y[a] * row;
  }
  return factor + specific;
}
f64 correlation(std::span<const f64> a, std::span<const f64> b) {
  const usize n = std::min(a.size(), b.size());
  if (n < 3) return nan;
  f64 ma = 0, mb = 0;
  for (usize i = 0; i < n; ++i) { ma += a[i]; mb += b[i]; }
  ma /= static_cast<f64>(n); mb /= static_cast<f64>(n);
  f64 ab = 0, aa = 0, bb = 0;
  for (usize i = 0; i < n; ++i) {
    const f64 x = a[i] - ma, y = b[i] - mb;
    ab += x * y; aa += x * x; bb += y * y;
  }
  return aa > 0 && bb > 0 ? ab / std::sqrt(aa * bb) : nan;
}
// Per-session financing rate of the book's scenario (annual bps x (365/252) / day count, the
// replay's calendar-day accrual averaged per session).
f64 per_session(f64 bps, u32 day_count) {
  return bps * 1e-4 * (365.0 / sessions_per_year) / static_cast<f64>(day_count);
}
// The plan fields of a book's move current -> next, exactly as aim-partial-v5 accumulates
// them (every rule).
void accumulate_plan(std::span<const u8> member, std::span<const f64> next,
                     std::span<const f64> current, TargetReplayDay& out) {
  out.applied_fraction = 1.0;
  f64 squared = 0;
  for (usize i = 0; i < next.size(); ++i) {
    const bool live = member[i] != 0;
    const f64 w = next[i], move = std::abs(w - current[i]);
    out.turnover += move;
    if (!live) out.forced_turnover += move; else out.discretionary_turnover += move;
    out.gross += std::abs(w); out.net += w;
    out.long_weight += std::max(0.0, w); out.short_weight += std::max(0.0, -w);
    out.max_abs_weight = std::max(out.max_abs_weight, std::abs(w));
    out.held_names += w != 0 ? 1U : 0U; squared += w * w;
  }
  out.effective_names = squared > 0 ? out.gross * out.gross / squared : 0;
}
// spo-v3: annualised ex-ante tracking error of the whole book w to the aim (priced names);
// gap is scratch.
f64 tracking_error(const RiskSlice& r, std::span<const f64> w, std::span<const f64> aim,
                   std::vector<f64>& gap) {
  gap.resize(aim.size());
  for (usize i = 0; i < aim.size(); ++i) gap[i] = w[i] - aim[i];
  return std::sqrt(sessions_per_year * book_variance(r, gap));
}
} // namespace

struct Engine::Impl {
  // The shared data of one decision (every book of a lockstep replay reads it).
  struct DateData {
    const f64* key{};
    usize d{no_date};
    RiskSlice slice;
    std::vector<usize> names;  // optimized instruments: members with a risk row, ascending
    std::vector<u32> position; // instrument -> index in names, not_optimized otherwise
    std::vector<f64> beta;     // ex-ante beta per instrument (0 without a risk row)
    Problem base;              // the shared parts over `names`
    f64 scale{1.0};
    usize members{}, unpriced_members{};
  };
  struct BookState {
    Multipliers warm;
    std::vector<f64> shadow; // the plan-level aim-partial-v5 shadow book (weights)
    std::vector<f64> dual;   // spo-v3: the last solve's dual per instrument (a warm start)
  };
  // Positions outside the problem at d (every rule): nonmembers follow aim-partial-v5's exit
  // rule, members without a risk row keep their weight.
  struct FixedPositions {
    std::vector<f64> next;     // per instrument: the fixed weights, 0 on the optimized names
    std::vector<f64> exposure; // risk_factors: X'w of the fixed positions with a risk row
    f64 net{}, gross{}, beta{};
    usize nonmembers{};        // nonmembers held before or after d
  };
  // One book's per-name market terms over the optimized names (every rule): the primary S2
  // law's linear cost per unit (unamortized) and impact coefficient, spo-v1/v2's holding cap,
  // the trade limit p ADV / NAV (0 without ADV), the locate floor min(w0, 0) where the book's
  // locate rule guards the name (-inf elsewhere) and the book's financing per session.
  struct MarketTerms {
    f64 linear_raw{};
    std::vector<f64> cap, trade, floor, impact_raw, long_rate, short_rate;
    std::vector<u8> guarded;
  };
  Impl(const SpoParams& p, std::shared_ptr<const RiskStore> r) : params(p), risk(std::move(r)) {}
  SpoParams params;
  std::shared_ptr<const RiskStore> risk;
  bool axes_checked{};
  f64 gamma{nan}, horizon{nan}, alpha_h{nan};
  // G in effect (--spo-gross, else the book's --aim-leverage); spo-v3: the gross sanity bound
  f64 budget{nan};
  Calibration calibration;
  DateData date;
  std::map<std::string, BookState, std::less<>> books;
  std::vector<DiagnosticRow> rows;
  std::vector<TrackingRow> tracking_rows; // spo-v3
  std::vector<f64> last_aim;              // spo-v3: the latest decision's aim (observation)
  Timing timing;
  std::vector<f64> desired_copy, shadow_before; // scratch

  co::Status prepare(const BookDecision& in);
  co::Status calibrate(const BookDecision& in);
  co::Status plan(const BookDecision& in, std::vector<f64>& planned, TargetReplayDay& out);
  co::Status warm_up_step(const BookDecision& in, std::vector<f64>& planned,
                          TargetReplayDay& out);
  co::Status shadow_step(const TargetReplayInput& x, const NavReplayConfig& cfg, usize d,
                         bool rebalance, std::span<const f64> desired, BookState& book,
                         TargetReplayDay& day);
  co::Status fixed_positions(const BookDecision& in, std::span<const f64> current,
                             FixedPositions& out) const;
  void market_terms(const BookDecision& in, std::span<const f64> current,
                    MarketTerms& out) const;
  // spo-v3 (strategy_spo_v3.hpp).
  co::Status calibrate_tracking(const BookDecision& in, std::span<const f64> aim);
  co::Status plan_tracking(const BookDecision& in, std::vector<f64>& planned,
                           TargetReplayDay& out);
  tt::TrackingProblem tracking_problem(std::span<const f64> aim, std::span<const f64> current,
                                       const FixedPositions& fixed,
                                       const MarketTerms& market) const;
  TrackingRow tracking_row(const BookDecision& in, const tt::TrackingProblem& p,
                           const tt::TrackingSolution& sol, std::span<const f64> aim,
                           std::span<const f64> current, std::span<const f64> next,
                           usize fixed_nonmembers, const TargetReplayDay& out) const;
};

// The shadow book's aim-partial-v5 move at d: detail::update_weights on its own plan-level
// weights (the replay's book plans from its drifted, filled holdings instead).
co::Status Engine::Impl::shadow_step(const TargetReplayInput& x, const NavReplayConfig& cfg,
                                     usize d, bool rebalance, std::span<const f64> desired,
                                     BookState& book, TargetReplayDay& day) {
  if (book.shadow.size() != x.instruments) book.shadow.assign(x.instruments, 0.0);
  desired_copy.assign(desired.begin(), desired.end());
  return detail::update_weights(x, cfg.target, d, rebalance, 0.0, desired_copy, book.shadow,
                                day);
}

// A warm-up rebalance decision (v8 D-0: d before the role's decision_begin; review A-2): the
// book makes aim-partial-v5's move toward the shared desired target (the rule the spo rules
// are rewritten to on the command line) and the shadow book the same move. No risk row is
// read and nothing is solved, calibrated or recorded, so gamma is calibrated on the first
// scored decision and a risk store without rows before decision_begin serves a warm start.
co::Status Engine::Impl::warm_up_step(const BookDecision& in, std::vector<f64>& planned,
                                      TargetReplayDay& out) {
  if (in.desired.size() != in.x.instruments)
    return co::Err(co::ErrorCode::InvalidArgument, "spo-v1: decision geometry");
  auto& book = books[std::string(in.book)];
  TargetReplayDay shadow_day;
  ATX_TRY_VOID(shadow_step(in.x, in.cfg, in.d, true, in.desired, book, shadow_day));
  calibration.warm_up = true;
  // shadow_step left the desired target in desired_copy (update_weights reads it only).
  return detail::update_weights(in.x, in.cfg.target, in.d, true, 0.0, desired_copy, planned,
                                out);
}

// The shared data of decision d (once for every book): the risk slice, the optimized names,
// ex-ante betas to the equal-weight market of those names, alpha and the metric scale.
co::Status Engine::Impl::prepare(const BookDecision& in) {
  const auto& x = in.x;
  if (!risk) return co::Err(co::ErrorCode::InvalidArgument, "spo-v1: no risk model (--risk-model)");
  if (!axes_checked) {
    ATX_TRY_VOID(risk->check_axes(x.session_keys, x.instruments));
    axes_checked = true;
  }
  if (date.key == x.close.data() && date.d == in.d) return co::Ok();
  const usize n_all = x.instruments, d = in.d;
  if (in.desired.size() != n_all || in.liquidity.adv.size() != n_all ||
      in.liquidity.sigma.size() != n_all)
    return co::Err(co::ErrorCode::InvalidArgument, "spo-v1: decision geometry");
  date.key = nullptr; date.d = no_date;
  ATX_TRY_VOID(risk->read(d, date.slice));
  cap_specific(date.slice, params.specific_ceiling); // inf (spo-v1): no change
  const auto& r = date.slice;
  const auto member = x.member.subspan(d * n_all, n_all);
  date.names.clear();
  date.position.assign(n_all, not_optimized);
  date.members = 0; date.unpriced_members = 0;
  for (usize i = 0; i < n_all; ++i) {
    if (!member[i]) continue;
    ++date.members;
    if (!has_row(r, i)) { ++date.unpriced_members; continue; }
    date.position[i] = static_cast<u32>(date.names.size());
    date.names.push_back(i);
  }
  const f64 sd = member_sd(in.desired, member);
  const usize n = date.names.size();
  Problem& b = date.base;
  b = Problem{};
  b.layout = FactorLayout{risk_industry_slots, risk_styles};
  b.n = n;
  b.industry.resize(n); b.styles.resize(n * risk_styles); b.specific.resize(n);
  b.alpha.resize(n); b.beta.resize(n);
  b.covariance = r.covariance;
  b.fixed_exposure.assign(risk_factors, 0.0);
  // spo-v3 tracks the aim: no alpha vector (b.alpha stays 0) and no FISTA metric.
  const bool alpha_rule = params.version != 3;
  for (usize j = 0; j < n; ++j) {
    const usize i = date.names[j];
    b.industry[j] = static_cast<u32>(1 + r.slot[i]);
    std::copy_n(r.styles.begin() + static_cast<std::ptrdiff_t>(i * risk_styles), risk_styles,
                b.styles.begin() + static_cast<std::ptrdiff_t>(j * risk_styles));
    b.specific[j] = r.specific[i];
    if (!alpha_rule) continue;
    const f64 z = sd > 0 ? in.desired[i] / sd : 0.0;
    b.alpha[j] = gk_alpha(params.ic_book, r.specific[i], z, alpha_h); // per session over h
  }
  // Ex-ante beta to the equal-weight market m of the optimized names: Sigma m / m'Sigma m.
  date.beta.assign(n_all, 0.0);
  if (n > 0) {
    std::vector<f64> y(risk_factors, 0.0), u(risk_factors, 0.0);
    const f64 weight = 1.0 / static_cast<f64>(n);
    f64 specific = 0;
    for (const usize i : date.names) {
      add_exposure(r, i, weight, y);
      specific += r.specific[i] * weight * weight;
    }
    f64 factor = 0;
    for (usize a = 0; a < risk_factors; ++a) {
      f64 row = 0;
      for (usize c = 0; c < risk_factors; ++c) row += r.covariance[a * risk_factors + c] * y[c];
      u[a] = row; factor += y[a] * row;
    }
    const f64 market = factor + specific;
    if (finite_positive(market))
      for (usize i = 0; i < n_all; ++i) {
        if (!has_row(r, i)) continue;
        const f64 own = date.position[i] != not_optimized ? r.specific[i] * weight : 0.0;
        date.beta[i] = (load_of(r, i, u) + own) / market;
      }
  }
  for (usize j = 0; j < n; ++j) b.beta[j] = date.beta[date.names[j]];
  date.scale = alpha_rule ? estimate_metric_scale(b) : 1.0;
  date.key = x.close.data(); date.d = d;
  return co::Ok();
}

// gamma on the cost-free aim of the first rebalance decision (calibrate_gamma) or --gamma.
co::Status Engine::Impl::calibrate(const BookDecision& in) {
  calibration.session = in.x.session_keys[in.d];
  calibration.rule = params.gamma_rule;
  if (std::isfinite(params.gamma)) {
    gamma = params.gamma;
    calibration.done = true; calibration.from_flag = true; calibration.gamma = gamma;
    return co::Ok();
  }
  ATX_TRY_VOID(calibrate_gamma(date.base, params, budget, date.scale, calibration));
  gamma = calibration.gamma;
  return co::Ok();
}

// Positions outside the problem: nonmembers follow aim-partial-v5's exit rule, members
// without a risk row keep their weight.
co::Status Engine::Impl::fixed_positions(const BookDecision& in, std::span<const f64> current,
                                         FixedPositions& out) const {
  const auto& x = in.x; const auto& cfg = in.cfg;
  const usize n_all = x.instruments, d = in.d;
  const auto& r = date.slice;
  const auto member = x.member.subspan(d * n_all, n_all);
  const bool decaying = cfg.target.exit_rate != 1.0;
  if (decaying && x.present.size() != x.dates * n_all)
    return co::Err(co::ErrorCode::InvalidArgument, "spo-v1: exit_rate below 1 needs prices");
  const f64 exit_band = date.members ? cfg.target.dust_multiple / static_cast<f64>(date.members)
                                     : inf;
  const f64 keep = 1.0 - cfg.target.exit_rate;
  out.next.assign(n_all, 0.0);
  out.exposure.assign(risk_factors, 0.0);
  out.net = 0; out.gross = 0; out.beta = 0;
  out.nonmembers = 0;
  for (usize i = 0; i < n_all; ++i) {
    if (date.position[i] != not_optimized) continue;
    f64 w = 0;
    if (member[i]) {
      w = current[i];
    } else if (decaying && x.present[d * n_all + i]) {
      w = current[i] * keep;
      if (std::abs(w) <= exit_band) w = 0;
    }
    if (!member[i] && (current[i] != 0 || w != 0)) ++out.nonmembers;
    out.next[i] = w;
    if (w == 0) continue;
    out.net += w; out.gross += std::abs(w);
    if (has_row(r, i)) { add_exposure(r, i, w, out.exposure); out.beta += date.beta[i] * w; }
  }
  return co::Ok();
}

// The book's per-name market terms over the optimized names (primary S2 law, the book's
// financing and locate rule).
void Engine::Impl::market_terms(const BookDecision& in, std::span<const f64> current,
                                MarketTerms& out) const {
  const usize n = date.names.size();
  const f64 nav = in.nav_post;
  const auto& fin = in.cfg.scenario.financing;
  const bool tiered = fin.rule == NavFinancingRule::TieredSwapV1;
  out.linear_raw = (in.s2.half_spread_bps + in.s2.commission_bps) * 1e-4;
  out.cap.assign(n, 0.0); out.trade.assign(n, 0.0); out.floor.assign(n, -inf);
  out.impact_raw.assign(n, 0.0); out.long_rate.assign(n, 0.0); out.short_rate.assign(n, 0.0);
  out.guarded.assign(n, u8{0});
  for (usize j = 0; j < n; ++j) {
    const usize i = date.names[j];
    const f64 w0 = current[i], adv = in.liquidity.adv[i];
    const bool liquid = finite_positive(adv);
    out.cap[j] = liquid ? std::min(params.w_max, params.adv_cap_q * adv / nav) : 0.0;
    out.trade[j] = liquid ? params.adv_trade_p * adv / nav : 0.0;
    const bool special = !in.tier.empty() &&
                         in.tier[i] == static_cast<u8>(ce::BorrowTier::Special);
    const bool guarded = fin.block_special_shorts &&
                         (special || (!in.no_locate.empty() && in.no_locate[i] != 0));
    if (guarded) { out.guarded[j] = u8{1}; out.floor[j] = std::min(w0, 0.0); }
    const f64 sigma = std::isnan(in.liquidity.sigma[i]) ? in.s2.fallback_daily_vol
                                                        : in.liquidity.sigma[i];
    out.impact_raw[j] = liquid ? in.s2.impact_y * sigma * std::sqrt(nav / adv) : 0.0;
    if (tiered) {
      const u8 tier = in.tier.empty() ? static_cast<u8>(ce::BorrowTier::Warm) : in.tier[i];
      const f64 fee = tier == static_cast<u8>(ce::BorrowTier::GeneralCollateral) ? fin.gc_bps
                      : tier == static_cast<u8>(ce::BorrowTier::Special)         ? fin.special_bps
                                                                                  : fin.warm_bps;
      out.long_rate[j] = per_session(fin.long_spread_bps, fin.day_count);
      out.short_rate[j] = per_session(fin.short_spread_bps + fee, fin.day_count);
    } else {
      out.long_rate[j] = 0.0;
      out.short_rate[j] = per_session(fin.flat_short_bps, fin.day_count);
    }
  }
}

co::Status Engine::Impl::plan(const BookDecision& in, std::vector<f64>& planned,
                              TargetReplayDay& out) {
  const auto& x = in.x; const auto& cfg = in.cfg;
  const usize n_all = x.instruments, d = in.d;
  if (d >= x.dates || planned.size() != n_all || !finite_positive(in.nav_post) ||
      (!in.tier.empty() && in.tier.size() != n_all) ||
      (!in.no_locate.empty() && in.no_locate.size() != n_all))
    return co::Err(co::ErrorCode::InvalidArgument, "spo-v1: decision geometry or NAV");
  if (in.s2.cost != NavCostRule::SqrtImpactV1 || in.s2.impact_delta != 0.5)
    return co::Err(co::ErrorCode::InvalidArgument, "spo-v1: the cost law must be S2's sqrt law");
  if (!std::isfinite(horizon)) {
    horizon = std::isfinite(params.horizon) ? params.horizon : 1.0 / cfg.target.trade_fraction;
    if (!(horizon >= 1 && horizon <= 10000))
      return co::Err(co::ErrorCode::InvalidArgument, "spo: horizon 1 / theta out of [1, 1e4]");
    alpha_h = params.alpha_horizon; // independent of H (R2 M-2)
  }
  // v8 D-0 warm-up (review A-2): aim-partial-v5's move, no risk row, gamma not yet taken.
  // Without a warm start every decision has d >= decision_begin: nothing changes.
  if (d < x.decision_begin) return warm_up_step(in, planned, out);
  if (params.version == 3) return plan_tracking(in, planned, out); // target tracking
  // G: the hard cap on the book's planned gross. Without --spo-gross it is the book's own
  // --aim-leverage, spo-v1's budget bit for bit (and not re-validated: no new refusal).
  budget = gross_budget_of(params, cfg.target.aim_leverage);
  if (!std::isnan(params.gross_budget))
    ATX_TRY_VOID(validate_gross_budget(budget, cfg.target.aim_leverage));
  ATX_TRY_VOID(prepare(in));
  if (!calibration.done) ATX_TRY_VOID(calibrate(in));
  const auto& r = date.slice;
  const std::vector<f64> current(planned);
  const auto member = x.member.subspan(d * n_all, n_all);
  FixedPositions fixed;
  ATX_TRY_VOID(fixed_positions(in, current, fixed));
  std::vector<f64>& next = fixed.next;
  Problem p = date.base;
  p.gamma = gamma;
  p.fixed_exposure = std::move(fixed.exposure);
  // The book's names: bounds, costs (primary S2 law, amortized over H) and financing.
  const usize n = p.n;
  p.w0.resize(n); p.lower.resize(n); p.upper.resize(n);
  p.linear_cost.resize(n); p.impact_cost.resize(n); p.long_rate.resize(n); p.short_rate.resize(n);
  MarketTerms market;
  market_terms(in, current, market);
  const f64 linear = market.linear_raw / horizon;
  for (usize j = 0; j < n; ++j) {
    const usize i = date.names[j];
    const f64 w0 = current[i], trade = market.trade[j];
    f64 hold_lo = -market.cap[j];
    const f64 hold_hi = market.cap[j];
    if (market.guarded[j] != 0) hold_lo = std::max(hold_lo, market.floor[j]);
    f64 lo = std::max(hold_lo, w0 - trade), hi = std::min(hold_hi, w0 + trade);
    if (lo > hi) lo = hi = w0 - trade > hold_hi ? w0 - trade : w0 + trade;
    p.w0[j] = w0; p.lower[j] = lo; p.upper[j] = hi;
    p.linear_cost[j] = linear;
    p.impact_cost[j] = market.impact_raw[j] / horizon;
    p.long_rate[j] = market.long_rate[j]; p.short_rate[j] = market.short_rate[j];
  }
  p.net = -fixed.net;
  p.gross = std::max(0.0, budget - fixed.gross);
  p.beta_lo = -params.beta_max - fixed.beta;
  p.beta_hi = params.beta_max - fixed.beta;
  auto& book = books[std::string(in.book)];
  const auto started = std::chrono::steady_clock::now();
  ATX_TRY(const auto sol, solve(p, p.w0, SolverOptions{params.max_iterations, params.tolerance,
                                                       date.scale},
                                book.warm));
  const f64 seconds =
      std::chrono::duration<f64>(std::chrono::steady_clock::now() - started).count();
  ++timing.solves; timing.seconds += seconds;
  timing.max_seconds = std::max(timing.max_seconds, seconds);
  book.warm = sol.multipliers;
  for (usize j = 0; j < n; ++j) next[date.names[j]] = sol.w[j];
  // The plan fields, exactly as aim-partial-v5 accumulates them.
  accumulate_plan(member, next, current, out);
  // Diagnostics of this decision and book.
  DiagnosticRow row;
  row.session = x.session_keys[d]; row.book = std::string(in.book);
  row.members = date.members; row.optimized = n; row.unpriced_members = date.unpriced_members;
  row.fixed_nonmembers = fixed.nonmembers; row.gamma = gamma;
  row.iterations = sol.iterations; row.restarts = sol.restarts; row.backtracks = sol.backtracks;
  row.prox_passes = sol.prox_passes; row.converged = sol.converged;
  row.coupling_met = sol.coupling_met; row.residual = sol.residual;
  const Terms terms = objective_terms(p, sol.w);
  const f64 variance = book_variance(r, next);
  row.alpha = terms.alpha; row.risk = 0.5 * gamma * variance;
  row.amortized_cost = terms.cost; row.trade_cost = terms.cost * horizon;
  row.financing = terms.financing;
  row.objective = terms.alpha - row.risk - terms.cost - terms.financing;
  row.exante_vol = std::sqrt(sessions_per_year * variance);
  row.exante_vol_current = std::sqrt(sessions_per_year * book_variance(r, current));
  std::vector<f64> score(n);
  for (usize j = 0; j < n; ++j) score[j] = p.alpha[j] / p.specific[j]; // a_i / sigma_i^2
  row.transfer_coefficient = correlation(score, sol.w);
  f64 beta = 0;
  for (usize i = 0; i < n_all; ++i) beta += date.beta[i] * next[i];
  row.gross = out.gross; row.net = out.net; row.long_weight = out.long_weight;
  row.short_weight = out.short_weight; row.abs_beta = std::abs(beta); row.turnover = out.turnover;
  for (usize j = 0; j < n; ++j) {
    const f64 w = sol.w[j], move = std::abs(w - p.w0[j]);
    if (w == p.w0[j]) ++row.no_trade;
    if (market.cap[j] > 0 && std::abs(w) >= market.cap[j] * (1.0 - 1e-9)) ++row.at_cap;
    if (market.trade[j] > 0 && move >= market.trade[j] * (1.0 - 1e-9)) ++row.at_trade_limit;
    if (std::isfinite(market.floor[j]) && w <= market.floor[j]) ++row.at_locate_floor;
  }
  out.construction.banded_names = row.no_trade;
  const auto& m = sol.multipliers;
  row.gross_binding = m.gross_binding; row.beta_binding = m.rho != 0;
  row.mu = m.gross_binding ? 0.5 * (m.pos + m.neg) : 0.0;
  row.nu = m.gross_binding ? 0.5 * (m.pos - m.neg) : m.pos;
  row.rho = m.rho;
  row.capped_specific = r.capped_specific;
  // The shadow aim-partial-v5 book at d, scored with the same alpha and S2 law.
  if (book.shadow.size() != n_all) book.shadow.assign(n_all, 0.0);
  shadow_before = book.shadow;
  TargetReplayDay shadow_day;
  ATX_TRY_VOID(shadow_step(x, cfg, d, true, in.desired, book, shadow_day));
  f64 alpha_shadow = 0, cost_shadow = 0;
  for (usize j = 0; j < n; ++j) {
    const usize i = date.names[j];
    const f64 move = std::abs(book.shadow[i] - shadow_before[i]);
    alpha_shadow += p.alpha[j] * book.shadow[i];
    cost_shadow += market.linear_raw * move + market.impact_raw[j] * move * std::sqrt(move);
  }
  row.alpha_shadow = alpha_shadow; row.gross_shadow = shadow_day.gross;
  row.turnover_shadow = shadow_day.turnover; row.trade_cost_shadow = cost_shadow;
  row.exante_vol_shadow = std::sqrt(sessions_per_year * book_variance(r, book.shadow));
  // Scored decisions only: a warm-up decision (v8 D-0) never reaches here (warm_up_step),
  // so spo_diagnostics.csv, the summary and the tripwire cover scored decisions only.
  if (d >= x.decision_begin) rows.push_back(std::move(row));
  planned = std::move(next);
  return co::Ok();
}

// ---- spo-v3: target tracking (strategy_spo_v3.hpp) --------------------------------------------
// gamma on the first rebalance decision: S_prior / sigma_aim, sigma_aim the annualised ex-ante
// vol of the whole aim (its names with a risk row), so the aim's implied annual Sharpe is
// S_prior.
co::Status Engine::Impl::calibrate_tracking(const BookDecision& in, std::span<const f64> aim) {
  const f64 sigma_aim = std::sqrt(sessions_per_year * book_variance(date.slice, aim));
  if (!finite_positive(sigma_aim))
    return co::Err(co::ErrorCode::Unavailable,
                   "spo-v3: the aim of the first rebalance decision has no ex-ante vol "
                   "(sigma_aim " + std::to_string(sigma_aim) + ")");
  f64 aim_gross = 0;
  for (const f64 w : aim) aim_gross += std::abs(w);
  gamma = params.sharpe_prior / sigma_aim;
  calibration.session = in.x.session_keys[in.d];
  calibration.gamma = gamma;
  calibration.aim_vol = sigma_aim;
  calibration.aim_gross = aim_gross;
  calibration.names = date.names.size();
  calibration.done = true;
  return co::Ok();
}

// One book's problem over the optimized names: the decision's factor structure, the aim, the
// S2 costs amortized over H, the short financing, the trade limits and locate floors, and the
// limits net of the fixed positions, whose factor exposure is the external gap (their aim is
// 0: nonmembers; unpriced members have no risk row).
tt::TrackingProblem Engine::Impl::tracking_problem(std::span<const f64> aim,
                                                   std::span<const f64> current,
                                                   const FixedPositions& fixed,
                                                   const MarketTerms& market) const {
  const Problem& b = date.base;
  const usize n = b.n;
  tt::TrackingProblem p;
  p.factors = tt::TrackingFactors{b.layout.industries, b.layout.styles};
  p.n = n;
  p.group = b.industry; p.styles = b.styles; p.covariance = b.covariance;
  p.specific = b.specific; p.beta = b.beta;
  p.external_gap = fixed.exposure;
  p.target.resize(n); p.current.resize(n); p.impact_cost.resize(n); p.borrow_cost.resize(n);
  p.trade_limit.resize(n); p.lower.resize(n);
  p.linear_cost.assign(n, market.linear_raw / horizon);
  p.upper.assign(n, inf); // no holding cap (not in the registration)
  for (usize j = 0; j < n; ++j) {
    const usize i = date.names[j];
    p.target[j] = aim[i];
    p.current[j] = current[i];
    p.impact_cost[j] = market.impact_raw[j] / horizon;
    p.borrow_cost[j] = market.short_rate[j];
    p.trade_limit[j] = market.trade[j];
    p.lower[j] = market.guarded[j] != 0 ? market.floor[j] : -inf;
  }
  p.gamma = gamma;
  p.net = tt::TrackingLimit{-fixed.net, -fixed.net};
  p.beta_limit = tt::TrackingLimit{-params.beta_max - fixed.beta, params.beta_max - fixed.beta};
  return p;
}

// The diagnostics of one book's decision (plan_tracking adds the shadow's columns).
TrackingRow Engine::Impl::tracking_row(const BookDecision& in, const tt::TrackingProblem& p,
                                       const tt::TrackingSolution& sol,
                                       std::span<const f64> aim, std::span<const f64> current,
                                       std::span<const f64> next, usize fixed_nonmembers,
                                       const TargetReplayDay& out) const {
  const auto& r = date.slice;
  TrackingRow row;
  row.session = in.x.session_keys[in.d]; row.book = std::string(in.book);
  row.members = date.members; row.optimized = p.n; row.unpriced_members = date.unpriced_members;
  row.fixed_nonmembers = fixed_nonmembers; row.gamma = gamma;
  row.iterations = sol.iterations; row.converged = sol.converged;
  row.limits_met = sol.limits_met;
  row.primal_residual = sol.primal_residual; row.dual_residual = sol.dual_residual;
  row.limit_violation = sol.limit_violation; row.clipped_eigenvalues = sol.clipped_eigenvalues;
  std::vector<f64> gap;
  row.tracking_error = tracking_error(r, next, aim, gap);
  row.tracking_error_current = tracking_error(r, current, aim, gap);
  row.aim_correlation = correlation(sol.w, p.target);
  row.objective = sol.terms.objective; row.amortized_cost = sol.terms.trade_cost;
  row.trade_cost = sol.terms.trade_cost * horizon; row.borrow = sol.terms.borrow;
  f64 aim_gross = 0, beta = 0;
  for (usize i = 0; i < aim.size(); ++i) {
    aim_gross += std::abs(aim[i]);
    beta += date.beta[i] * next[i];
  }
  row.gross = out.gross; row.aim_gross = aim_gross; row.net = out.net;
  row.long_weight = out.long_weight; row.short_weight = out.short_weight;
  row.abs_beta = std::abs(beta); row.turnover = out.turnover;
  row.no_trade = sol.no_trade; row.at_trade_limit = sol.at_trade_limit;
  row.trade_limit_share = sol.trade_limit_share;
  for (usize j = 0; j < p.n; ++j)
    if (std::isfinite(p.lower[j]) && sol.w[j] <= p.lower[j]) ++row.at_locate_floor;
  row.gross_bound_breached = !(out.gross <= budget);
  row.nu = sol.net_multiplier; row.rho = sol.beta_multiplier;
  row.capped_specific = r.capped_specific;
  return row;
}

// One book's rebalance decision: track the aim L x desired. plan() validated the decision
// and set H; the plan fields, the fixed positions, the market terms and the shadow book are
// spo-v1/v2's.
co::Status Engine::Impl::plan_tracking(const BookDecision& in, std::vector<f64>& planned,
                                       TargetReplayDay& out) {
  const auto& x = in.x;
  const usize n_all = x.instruments, d = in.d;
  budget = v3_gross_bound_multiple * in.cfg.target.aim_leverage; // checked, never imposed
  ATX_TRY_VOID(prepare(in));
  const auto member = x.member.subspan(d * n_all, n_all);
  // The aim L x desired, with desired the NAV replay's shared desired target as aim-partial-v5
  // forms it (v8 E-26: --hold-band / --adv-hold-q shape it there, detail::form_desired; this
  // rule adds nothing). Kept as last_aim: an observation, never published.
  auto& aim = last_aim;
  aim.assign(n_all, 0.0);
  for (usize i = 0; i < n_all; ++i)
    if (member[i]) aim[i] = in.cfg.target.aim_leverage * in.desired[i];
  if (!calibration.done) ATX_TRY_VOID(calibrate_tracking(in, aim));
  const std::vector<f64> current(planned);
  FixedPositions fixed;
  ATX_TRY_VOID(fixed_positions(in, current, fixed));
  MarketTerms market;
  market_terms(in, current, market);
  const tt::TrackingProblem p = tracking_problem(aim, current, fixed, market);
  const auto& names = date.names;
  auto& book = books[std::string(in.book)];
  std::vector<f64> warm; // the book's last dual on today's names (cold: empty)
  if (book.dual.size() == n_all) {
    warm.resize(names.size());
    for (usize j = 0; j < names.size(); ++j) warm[j] = book.dual[names[j]];
  }
  const tt::TrackingOptions options{params.max_iterations, params.tolerance};
  const auto started = std::chrono::steady_clock::now();
  ATX_TRY(const auto sol, tt::solve_tracking(p, options, warm));
  const f64 seconds =
      std::chrono::duration<f64>(std::chrono::steady_clock::now() - started).count();
  ++timing.solves; timing.seconds += seconds;
  timing.max_seconds = std::max(timing.max_seconds, seconds);
  if (!sol.converged) { ++timing.unconverged; timing.unconverged_seconds += seconds; }
  std::vector<f64> next = std::move(fixed.next);
  book.dual.assign(n_all, 0.0);
  for (usize j = 0; j < names.size(); ++j) {
    next[names[j]] = sol.w[j];
    book.dual[names[j]] = sol.dual[j];
  }
  accumulate_plan(member, next, current, out);
  out.construction.banded_names = sol.no_trade;
  TrackingRow row = tracking_row(in, p, sol, aim, current, next, fixed.nonmembers, out);
  // The shadow aim-partial-v5 book at d, scored against the same aim, risk model and S2 law.
  if (book.shadow.size() != n_all) book.shadow.assign(n_all, 0.0);
  shadow_before = book.shadow;
  TargetReplayDay shadow_day;
  ATX_TRY_VOID(shadow_step(x, in.cfg, d, true, in.desired, book, shadow_day));
  f64 cost_shadow = 0;
  std::vector<f64> held(names.size()), gap;
  for (usize j = 0; j < names.size(); ++j) {
    const usize i = names[j];
    const f64 move = std::abs(book.shadow[i] - shadow_before[i]);
    cost_shadow += market.linear_raw * move + market.impact_raw[j] * move * std::sqrt(move);
    held[j] = book.shadow[i];
  }
  row.gross_shadow = shadow_day.gross; row.turnover_shadow = shadow_day.turnover;
  row.trade_cost_shadow = cost_shadow;
  row.tracking_error_shadow = tracking_error(date.slice, book.shadow, aim, gap);
  row.aim_correlation_shadow = correlation(held, p.target);
  // Scored decisions only (v8 D-0), as spo-v1/v2.
  if (d >= x.decision_begin) tracking_rows.push_back(std::move(row));
  planned = std::move(next);
  return co::Ok();
}

Engine::Engine(const SpoParams& params, std::shared_ptr<const RiskStore> risk)
    : impl_(std::make_unique<Impl>(params, std::move(risk))) {}
Engine::~Engine() = default;
Engine::Engine(Engine&&) noexcept = default;
Engine& Engine::operator=(Engine&&) noexcept = default;
co::Status Engine::plan(const BookDecision& in, std::vector<f64>& planned, TargetReplayDay& out) {
  return impl_->plan(in, planned, out);
}
co::Status Engine::hold(const TargetReplayInput& x, const NavReplayConfig& cfg, usize d,
                        std::span<const f64> desired, std::string_view book) {
  TargetReplayDay day;
  return impl_->shadow_step(x, cfg, d, false, desired, impl_->books[std::string(book)], day);
}
void Engine::begin_run() { impl_->books.clear(); impl_->date = Impl::DateData{}; }
std::span<const DiagnosticRow> Engine::rows() const noexcept { return impl_->rows; }
std::span<const TrackingRow> Engine::tracking_rows() const noexcept {
  return impl_->tracking_rows;
}
std::span<const f64> Engine::last_aim() const noexcept { return impl_->last_aim; }
const Calibration& Engine::calibration() const noexcept { return impl_->calibration; }
const SpoParams& Engine::params() const noexcept { return impl_->params; }
Timing Engine::timing() const noexcept { return impl_->timing; }
f64 Engine::horizon() const noexcept { return impl_->horizon; }
f64 Engine::gross_budget() const noexcept { return impl_->budget; }

// ---- declarations and outputs ------------------------------------------------------------
std::string declaration(const SpoParams& params) {
  const bool v2 = params.version == 2;
  const auto pick = [v2](const char* two, const char* one) {
    return std::string(v2 ? two : one);
  };
  return std::string(rule_name(params)) +
         " (literature-v7 R2.1 + R3.4; platform v7 W1" + pick(", fix-up 2, W1b", "") +
         "): on every rebalance decision d "
         "each book solves max_w a'w - (gamma/2) w'(X F X' + D) w - (1/H) sum_i [s_i |dw_i| + "
         "eta_i |dw_i|^1.5] - sum_i [b_i max(-w_i, 0) + l_i max(w_i, 0)] s.t. book net 0, "
         "|book beta| <= beta_max, book gross <= G (the gross budget, a hard cap on planned "
         "gross: --spo-gross, default " + pick("1.0", "--aim-leverage") +
         "; refused outside (0, 1.5 x --aim-leverage]), |w_i| <= min(w_max, q ADV_i / "
         "NAV), |dw_i| <= p ADV_i / NAV, w_i >= min(w0_i, 0) where the book's locate rule "
         "guards the name; a_i = IC_book sqrt(D_i) z_i / sqrt(h) (Grinold-Kahn per session "
         "of an h-session forecast, z = desired / its members' SD; h = --alpha-horizon, " +
         pick("default 21 (the IC's measurement horizon), independent of H", "default 1") +
         "; one uniform 1/sqrt(h) for every sleeve: the per-sleeve decay of R2.1 is not "
         "modeled), X/F/D = atx-risk-v1 at the close of d (NaN factor entries 0; specific "
         "variance above --specific-ceiling clamped, " + pick("default 1", "default off") +
         "; --specific-ceiling-void, default " + pick("on", "off") +
         ": a clamped entry at any decision voids the run, which exits non-zero after writing "
         "spo_diagnostics.csv and v7_extras.json and before any NAV or return file), beta = "
         "Sigma m / m'Sigma m with m the equal-weight portfolio of the optimized names, s_i = "
         "(half spread + commission) and eta_i = impact_y sigma_i sqrt(NAV / ADV_i) of the "
         "primary S2 law on the decision liquidity window (sigma fallback .05), b_i / l_i = "
         "the book's financing per session (annual x (365/252) / day count; tier fee at d), H "
         "= --spo-horizon (default 1 / theta); optimized names = members with a risk row; "
         "nonmembers follow aim-partial-v5's exit rule and unpriced members keep their weight "
         "(fixed positions in the book constraints and the risk); a position outside its box "
         "by more than one session's trade limit moves by the limit toward it; gamma = --gamma "
         "or " + pick("gamma_vol (refused when out of reach; gamma_bind reported only, NaN "
                      "with a note when its root fails or leaves the bracket)",
                      "max(gamma_vol, gamma_bind)") +
         " on the cost-free aim of the first rebalance decision (|w_i| <= w_max, net 0, |beta| "
         "<= beta_max; gamma_vol: annualised ex-ante vol --target-vol, gamma_bind: gross G); "
         "solver FISTA with adaptive restart in the metric sigma gamma D, exact coupled prox, "
         "stop at prox-gradient residual <= --spo-tol or --spo-iters; non-rebalance decisions "
         "are aim-partial-v5's (exits only); diagnostics beside each book: a plan-level "
         "aim-partial-v5 shadow book (same desired target, --aim-leverage, full fills, no "
         "drift) scored with the same alpha and S2 law; exante_vol columns are annualised "
         "(sqrt(252 x daily variance)), the other money columns per session";
}

namespace {
std::string number(f64 x) {
  std::ostringstream out;
  out.imbue(std::locale::classic());
  out << std::setprecision(17) << x;
  return out.str();
}
Json finite_or_null(f64 x) { return std::isfinite(x) ? Json(x) : Json(nullptr); }
const char* gamma_rule_text(GammaRule rule) {
  return rule == GammaRule::Vol ? "gamma_vol on the first rebalance decision"
                                : "max(gamma_vol, gamma_bind) on the first rebalance decision";
}
} // namespace

std::string diagnostics_csv(std::span<const DiagnosticRow> rows) {
  std::string text =
      "session,book,members,optimized,unpriced_members,fixed_nonmembers,gamma,iterations,"
      "restarts,backtracks,prox_passes,converged,coupling_met,kkt_residual,alpha,risk,"
      "trade_cost,amortized_cost,financing,objective,exante_vol,exante_vol_current,"
      "transfer_coefficient,gross,net,long,short,abs_beta,turnover,no_trade,at_cap,"
      "at_trade_limit,at_locate_floor,gross_binding,beta_binding,mu,nu,rho,capped_specific,"
      "alpha_shadow,gross_shadow,turnover_shadow,trade_cost_shadow,exante_vol_shadow\n";
  for (const auto& r : rows) {
    const auto u = [](usize v) { return std::to_string(v); };
    const auto b = [](bool v) { return std::string(v ? "1" : "0"); };
    text += std::to_string(r.session) + ',' + r.book + ',' + u(r.members) + ',' + u(r.optimized) +
            ',' + u(r.unpriced_members) + ',' + u(r.fixed_nonmembers) + ',' + number(r.gamma) +
            ',' + u(r.iterations) + ',' + u(r.restarts) + ',' + u(r.backtracks) + ',' +
            u(r.prox_passes) + ',' + b(r.converged) + ',' + b(r.coupling_met) + ',' +
            number(r.residual) + ',' + number(r.alpha) + ',' + number(r.risk) + ',' +
            number(r.trade_cost) + ',' + number(r.amortized_cost) + ',' + number(r.financing) +
            ',' + number(r.objective) + ',' + number(r.exante_vol) + ',' +
            number(r.exante_vol_current) + ',' + number(r.transfer_coefficient) + ',' +
            number(r.gross) + ',' + number(r.net) + ',' + number(r.long_weight) + ',' +
            number(r.short_weight) + ',' + number(r.abs_beta) + ',' + number(r.turnover) + ',' +
            u(r.no_trade) + ',' + u(r.at_cap) + ',' + u(r.at_trade_limit) + ',' +
            u(r.at_locate_floor) + ',' + b(r.gross_binding) + ',' + b(r.beta_binding) + ',' +
            number(r.mu) + ',' + number(r.nu) + ',' + number(r.rho) + ',' +
            u(r.capped_specific) + ',' + number(r.alpha_shadow) + ',' +
            number(r.gross_shadow) + ',' + number(r.turnover_shadow) + ',' +
            number(r.trade_cost_shadow) + ',' + number(r.exante_vol_shadow) + '\n';
  }
  return text;
}

Json diagnostics_units_json() {
  constexpr const char* annualised = "annualised: sqrt(252 x daily ex-ante variance)";
  constexpr const char* session = "per session, NAV fraction";
  return Json{{"exante_vol", annualised}, {"exante_vol_current", annualised},
              {"exante_vol_shadow", annualised}, {"alpha", session}, {"risk", session},
              {"trade_cost", "per decision, NAV fraction (unamortized)"},
              {"amortized_cost", "trade_cost / H"}, {"financing", session},
              {"objective", session}, {"alpha_shadow", session},
              {"trade_cost_shadow", "per decision, NAV fraction (unamortized)"}};
}

Json parameters_json(const SpoParams& p, f64 horizon, f64 gross_budget) {
  return Json{{"rule", rule_name(p)}, {"version", p.version},
              {"gamma", finite_or_null(p.gamma)},
              {"gamma_rule", std::isfinite(p.gamma) ? "--gamma" : gamma_rule_text(p.gamma_rule)},
              {"gross_budget", finite_or_null(gross_budget)},
              {"gross_budget_rule",
               std::isnan(p.gross_budget) ? "--aim-leverage" : "--spo-gross (hard cap on "
                                                               "planned gross; the shadow keeps "
                                                               "--aim-leverage)"},
              {"alpha_horizon", finite_or_null(p.alpha_horizon)},
              {"alpha_scaling", "uniform 1/sqrt(h) for every sleeve, independent of H"},
              {"specific_ceiling", finite_or_null(p.specific_ceiling)},
              {"specific_ceiling_void", p.void_on_capped},
              {"ic_book", p.ic_book}, {"w_max", p.w_max}, {"adv_cap_q", p.adv_cap_q},
              {"adv_trade_p", p.adv_trade_p}, {"spo_iters", p.max_iterations},
              {"spo_tol", p.tolerance}, {"target_vol", p.target_vol},
              {"horizon", finite_or_null(horizon)},
              {"horizon_rule", std::isfinite(p.horizon) ? "--spo-horizon" : "1 / theta"},
              {"beta_max", p.beta_max}, {"books", p.all_books ? "all" : "primary (S1, S2)"}};
}

Json calibration_json(const Calibration& c) {
  Json j{{"done", c.done}, {"from_flag", c.from_flag}, {"session", c.session},
         {"rule", gamma_rule_text(c.rule)},
         {"gamma", finite_or_null(c.gamma)}, {"gamma_vol", finite_or_null(c.gamma_vol)},
         {"gamma_bind", finite_or_null(c.gamma_bind)}, {"vol_reached", c.vol_reached},
         {"bind_reached", c.bind_reached},
         {"gamma_bind_note", c.bind_note.empty() ? Json(nullptr) : Json(c.bind_note)},
         {"aim_vol", finite_or_null(c.aim_vol)},
         {"aim_gross", finite_or_null(c.aim_gross)}, {"evaluations", c.evaluations},
         {"names", c.names}};
  if (c.warm_up) j["warm_up"] = warm_up_calibration_text; // review A-2; absent without one
  return j;
}

namespace {
struct CeilingCount {
  usize decisions{}, names_max{};
};
// Distinct decisions (sessions) whose risk slice had a clamped entry, and the most entries
// clamped at one decision (every book of a decision reads the same slice).
CeilingCount count_capped(std::span<const DiagnosticRow> rows) {
  std::set<i64> sessions;
  CeilingCount c;
  for (const auto& r : rows) {
    if (r.capped_specific == 0) continue;
    sessions.insert(r.session);
    c.names_max = std::max(c.names_max, r.capped_specific);
  }
  c.decisions = sessions.size();
  return c;
}
} // namespace

co::Status ceiling_tripwire(const SpoParams& p, std::span<const DiagnosticRow> rows) {
  if (!p.void_on_capped) return co::Ok();
  const auto c = count_capped(rows);
  if (c.decisions == 0) return co::Ok();
  return co::Err(co::ErrorCode::Unavailable,
                 std::string(rule_name(p)) + ": specific-ceiling tripwire: " +
                     std::to_string(c.decisions) +
                     " decisions had a daily specific variance above " +
                     number(p.specific_ceiling) + " (at most " + std::to_string(c.names_max) +
                     " entries at one decision); the run is VOID (--specific-ceiling-void on): "
                     "no NAV or return file is written");
}

Json tripwire_json(const SpoParams& p, std::span<const DiagnosticRow> rows) {
  const auto c = count_capped(rows);
  const char* status = c.decisions == 0 ? "clear"
                       : p.void_on_capped ? "void"
                                          : "tripped (not voiding: --specific-ceiling-void off)";
  return Json{{"specific_ceiling", finite_or_null(p.specific_ceiling)},
              {"specific_ceiling_void", p.void_on_capped},
              {"capped_specific_decisions", c.decisions},
              {"capped_specific_names_max", c.names_max}, {"status", status}};
}

Json summary_json(std::span<const DiagnosticRow> rows) {
  std::map<std::string, std::vector<const DiagnosticRow*>> by_book;
  for (const auto& r : rows) by_book[r.book].push_back(&r);
  Json books = Json::object();
  for (const auto& [book, list] : by_book) {
    usize unconverged = 0, unmet = 0, gross_binding = 0, beta_binding = 0;
    usize capped_decisions = 0, capped_max = 0;
    f64 iterations = 0, residual = 0, vol = 0, tc = 0, alpha = 0, cost = 0, gross = 0;
    f64 turnover = 0, alpha_s = 0, cost_s = 0, gross_s = 0, turnover_s = 0, vol_s = 0;
    usize tc_n = 0;
    for (const auto* r : list) {
      unconverged += r->converged ? 0U : 1U; unmet += r->coupling_met ? 0U : 1U;
      gross_binding += r->gross_binding ? 1U : 0U; beta_binding += r->beta_binding ? 1U : 0U;
      iterations += static_cast<f64>(r->iterations); residual = std::max(residual, r->residual);
      vol += r->exante_vol; alpha += r->alpha; cost += r->trade_cost; gross += r->gross;
      turnover += r->turnover;
      alpha_s += r->alpha_shadow; cost_s += r->trade_cost_shadow; gross_s += r->gross_shadow;
      turnover_s += r->turnover_shadow; vol_s += r->exante_vol_shadow;
      capped_decisions += r->capped_specific ? 1U : 0U;
      capped_max = std::max(capped_max, r->capped_specific);
      if (std::isfinite(r->transfer_coefficient)) { tc += r->transfer_coefficient; ++tc_n; }
    }
    const f64 count = static_cast<f64>(list.size());
    // Holding period of the plan (gross / one-way turnover, sessions): the amortization H
    // presumes the book holds a traded dollar about H sessions.
    const auto ratio = [](f64 a, f64 b) { return finite_or_null(b > 0 ? a / b : nan); };
    books[book] = Json{{"decisions", list.size()}, {"unconverged", unconverged},
                       {"coupling_unmet", unmet}, {"gross_binding", gross_binding},
                       {"beta_binding", beta_binding}, {"mean_iterations", iterations / count},
                       {"max_kkt_residual", residual}, {"mean_exante_vol", vol / count},
                       {"mean_transfer_coefficient",
                        finite_or_null(tc_n ? tc / static_cast<f64>(tc_n) : nan)},
                       {"mean_alpha", alpha / count}, {"mean_trade_cost", cost / count},
                       {"mean_gross", gross / count}, {"mean_turnover", turnover / count},
                       {"holding_sessions", ratio(gross, turnover)},
                       {"capped_specific_decisions", capped_decisions},
                       {"capped_specific_names_max", capped_max},
                       {"shadow", Json{{"rule", "plan-level aim-partial-v5 (full fills, no drift)"},
                                       {"mean_alpha", finite_or_null(alpha_s / count)},
                                       {"mean_trade_cost", finite_or_null(cost_s / count)},
                                       {"mean_gross", finite_or_null(gross_s / count)},
                                       {"mean_turnover", finite_or_null(turnover_s / count)},
                                       {"mean_exante_vol", finite_or_null(vol_s / count)},
                                       {"holding_sessions", ratio(gross_s, turnover_s)}}},
                       {"alpha_capture", ratio(alpha, alpha_s)},
                       {"trade_cost_ratio", ratio(cost, cost_s)}};
  }
  return books;
}
} // namespace atx::impl::strategy::spo
