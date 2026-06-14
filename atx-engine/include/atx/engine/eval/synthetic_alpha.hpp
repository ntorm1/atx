#pragma once

// atx::engine::eval — synthetic-alpha recovery: a planted-signal panel + the
// recovery-correlation statistic (S4.4a). The FIRST half of the robustness
// measurement layer (the second being regime_slice.hpp's regime/walk-forward
// survival).
//
// ===========================================================================
//  What this header is — the recovery experiment
// ===========================================================================
//  A factory that admits a real edge but stays silent on pure noise (the F4
//  anti-snooping proof) tells us the gate is non-vacuous. The RECOVERY experiment
//  asks the dual question: when an edge IS present, does the search find THE RIGHT
//  ONE? We PLANT a known signal into a panel's forward returns —
//
//      forward_return(t, i) = beta · zscore_t( planted_signal(t-1, i) )
//                                   + noise_sigma · eps(t, i)
//
//  — where planted_signal is either a known DSL expression's output (the
//  SignalExpr fork) or a hidden latent factor (the LatentFactor fork), and the
//  noise eps is deterministic seeded draws. The return at date t depends only on
//  the signal at t-1 (CAUSAL: no look-ahead — the planted edge is predictive, not
//  contemporaneous). A small seeded GA then mines the panel; recovery_correlation
//  measures how well the admitted survivors' realized OOS PnL tracks the planted
//  signal's OWN realized OOS PnL. beta>0 => survivors recover the edge (corr high);
//  the matched beta=0 panel is pure noise and admits ~nothing under the same gate.
//
// ===========================================================================
//  Causality / firewall (load-bearing)
// ===========================================================================
//  The generated panel is causal by construction: forward_return(t) uses
//  planted_signal(t-1), built from prices strictly BEFORE t, so the planted edge
//  is a genuine one-step-ahead predictor (a refit-free firewall — the panel itself
//  never leaks future information into a past return). Downstream, the GA's fitness
//  eval is OOS+deflated (CPCV TEST folds, deflated Sharpe) exactly as in
//  production; this header adds no new fit path. recovery_correlation is a pure
//  read of already-realized OOS PnL streams.
//
// ===========================================================================
//  Determinism (load-bearing)
// ===========================================================================
//  generate_synthetic_panel is a PURE function of its seed: the base price walk,
//  the latent factor, and the noise are all drawn from ONE seeded SplitMix/LCG
//  stream in a fixed (date-major, instrument-minor) order. No clock, no global
//  RNG. Same (seed, spec, dims) => byte-identical panel. The DSL expr is evaluated
//  through the deterministic VM (Engine::evaluate). recovery_correlation reuses
//  combine::pairwise_complete_corr (the shared §3.3 NaN policy) with ascending-
//  index reductions.

#include <cmath>   // std::isnan, std::sqrt
#include <cstdint> // std::uint64_t (the seeded LCG state)
#include <span>    // std::span
#include <string>  // std::string (the DSL expr fork)
#include <utility> // std::move
#include <vector>  // std::vector

#include "atx/core/error.hpp" // Result, Ok, Err, ErrorCode
#include "atx/core/types.hpp" // atx::f64, atx::u8, atx::u64, atx::usize

#include "atx/engine/alpha/bytecode.hpp"      // alpha::compile
#include "atx/engine/alpha/panel.hpp"         // alpha::Panel, SignalSet
#include "atx/engine/alpha/parser.hpp"        // alpha::parse_expr
#include "atx/engine/alpha/registry.hpp"      // alpha::Library
#include "atx/engine/alpha/streams.hpp"       // alpha::extract_streams, AlphaStreams
#include "atx/engine/alpha/typecheck.hpp"     // alpha::analyze
#include "atx/engine/alpha/vm.hpp"            // alpha::Engine
#include "atx/engine/combine/correlation.hpp" // combine::pairwise_complete_corr
#include "atx/engine/exec/execution_sim.hpp"  // exec::ExecutionSimulator
#include "atx/engine/loop/weight_policy.hpp"  // engine::WeightPolicy

namespace atx::engine::eval {

// ===========================================================================
//  PlantedKind — the "signal_expr | latent factor" fork (exhaustive enum, no
//  default switch anywhere that consumes it). SignalExpr plants a known DSL
//  expression's output; LatentFactor plants a hidden seeded factor unobservable
//  to the GA (the GA never sees the factor, only its imprint on prices).
// ===========================================================================
enum class PlantedKind : atx::u8 { SignalExpr, LatentFactor };

// ===========================================================================
//  Dims — the synthetic panel shape.
// ===========================================================================
struct Dims {
  atx::usize dates;
  atx::usize instruments;
};

// ===========================================================================
//  PlantedSpec — what to plant and how strongly.
//
//  kind        : SignalExpr (use signal_expr) or LatentFactor (hidden factor).
//  signal_expr : the DSL expression evaluated over the base price panel to obtain
//                the planted signal (used IFF kind == SignalExpr; ignored for
//                LatentFactor). Must reference only the "close" field.
//  beta        : the planted edge strength — the coefficient on the z-scored
//                signal in the forward return. 0 => a pure-noise panel (the F4
//                matched control); >0 => a recoverable edge.
//  noise_sigma : the std of the per-cell i.i.d. forward-return noise.
// ===========================================================================
struct PlantedSpec {
  PlantedKind kind;
  std::string signal_expr;
  atx::f64 beta;
  atx::f64 noise_sigma;
};

namespace detail {

// ---------------------------------------------------------------------------
//  SynthRng — the ONE seeded LCG driving the whole synthetic panel (base price
//  walk + latent factor + noise). uniform(-1, 1). The S3/S4 fixture idiom; pure,
//  portable, no global RNG / clock. Drawn in a fixed order so the panel is a pure
//  function of the seed.
// ---------------------------------------------------------------------------
struct SynthRng {
  std::uint64_t s;
  [[nodiscard]] atx::f64 next() noexcept {
    s = s * 6364136223846793005ULL + 1442695040888963407ULL;
    const std::uint64_t hi = s >> 11U;
    const atx::f64 u = static_cast<atx::f64>(hi) / static_cast<atx::f64>(1ULL << 53U);
    return 2.0 * u - 1.0;
  }
};

// ---------------------------------------------------------------------------
//  zscore_row — cross-sectional z-score of one date's signal row (subtract the
//  cross-sectional mean, divide by the population std). NaN cells are skipped in
//  the moments and map to 0 in the output (out-of-universe contributes nothing).
//  A degenerate (zero-variance / all-NaN) row maps to all-0 (no tilt that date).
// ---------------------------------------------------------------------------
inline void zscore_row(std::span<const atx::f64> in, std::span<atx::f64> out) noexcept {
  const atx::usize n = in.size();
  atx::f64 sum = 0.0;
  atx::usize cnt = 0U;
  for (atx::usize j = 0; j < n; ++j) {
    if (!std::isnan(in[j])) {
      sum += in[j];
      ++cnt;
    }
  }
  if (cnt == 0U) {
    for (atx::usize j = 0; j < n; ++j) {
      out[j] = 0.0;
    }
    return;
  }
  const atx::f64 mean = sum / static_cast<atx::f64>(cnt);
  atx::f64 sq = 0.0;
  for (atx::usize j = 0; j < n; ++j) {
    if (!std::isnan(in[j])) {
      const atx::f64 d = in[j] - mean;
      sq += d * d;
    }
  }
  const atx::f64 sd = std::sqrt(sq / static_cast<atx::f64>(cnt));
  for (atx::usize j = 0; j < n; ++j) {
    out[j] = (std::isnan(in[j]) || sd == 0.0) ? 0.0 : (in[j] - mean) / sd;
  }
}

// ---------------------------------------------------------------------------
//  base_close — a seeded per-instrument multiplicative random walk (the price
//  series the planted DSL expr is evaluated over). Date-major, instrument-minor;
//  pure in the seed. A small drift spread across instruments gives the price
//  templates (rank/ts_mean/delta) something non-degenerate to latch onto.
// ---------------------------------------------------------------------------
[[nodiscard]] inline std::vector<atx::f64> base_close(atx::u64 seed, Dims dims) {
  std::vector<atx::f64> close(dims.dates * dims.instruments);
  std::vector<atx::f64> px(dims.instruments, 100.0);
  SynthRng rng{seed};
  for (atx::usize t = 0; t < dims.dates; ++t) {
    for (atx::usize j = 0; j < dims.instruments; ++j) {
      px[j] *= (1.0 + 0.008 * rng.next());
      close[t * dims.instruments + j] = px[j];
    }
  }
  return close;
}

// ---------------------------------------------------------------------------
//  eval_expr_signal — evaluate a single-output DSL expr over a {"close"} panel of
//  `base` prices, returning the date-major signal matrix (NaN where the VM masks).
//  Err propagates a parse / analyze / compile / eval failure.
// ---------------------------------------------------------------------------
[[nodiscard]] inline atx::core::Result<std::vector<atx::f64>>
eval_expr_signal(const std::string &expr, const std::vector<atx::f64> &base, Dims dims) {
  std::vector<std::vector<atx::f64>> cols{base};
  ATX_TRY(alpha::Panel panel,
          alpha::Panel::create(dims.dates, dims.instruments, {"close"}, std::move(cols), {}));
  alpha::Library dsl;
  ATX_TRY(alpha::Ast ast, alpha::parse_expr(expr, dsl));
  ATX_TRY(alpha::Analysis info, alpha::analyze(ast));
  ATX_TRY(alpha::Program prog, alpha::compile(ast, info));
  alpha::Engine engine{panel};
  ATX_TRY(alpha::SignalSet sig, engine.evaluate(prog));
  if (sig.alphas.empty()) {
    return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                          "eval_expr_signal: expr produced no alpha root");
  }
  return atx::core::Ok(std::move(sig.alphas.front().values));
}

// ---------------------------------------------------------------------------
//  latent_factor — a hidden per-(date, instrument) AR(1) factor drawn from the
//  seed (continued from the base-price stream by re-seeding with seed ^ const so
//  the factor stream is independent of, yet reproducible with, the price walk).
//  This is the LatentFactor planted signal — the GA never observes it directly.
// ---------------------------------------------------------------------------
[[nodiscard]] inline std::vector<atx::f64> latent_factor(atx::u64 seed, Dims dims) {
  std::vector<atx::f64> f(dims.dates * dims.instruments, 0.0);
  std::vector<atx::f64> prev(dims.instruments, 0.0);
  SynthRng rng{seed ^ 0x9E3779B97F4A7C15ULL};
  for (atx::usize t = 0; t < dims.dates; ++t) {
    for (atx::usize j = 0; j < dims.instruments; ++j) {
      // AR(1) with a persistent component so the factor has exploitable structure.
      const atx::f64 v = 0.6 * prev[j] + 0.4 * rng.next();
      f[t * dims.instruments + j] = v;
      prev[j] = v;
    }
  }
  return f;
}

} // namespace detail

// ===========================================================================
//  generate_synthetic_panel — build a panel whose forward returns carry the
//  planted signal. Returns a {"close", "signal"} panel:
//    * "close"  — the integrated price series whose period-over-period return at
//      date t is  beta · zscore_t(planted_signal(t-1)) + noise_sigma·eps(t).
//    * "signal" — the planted signal matrix itself (the recovery target;
//      planted_signal_pnl trades rank(signal) over THIS panel to get the planted
//      edge's own OOS PnL). The GA is given only "close" (panel_fields), so it
//      never peeks at "signal".
//  PURE in `seed`. Err propagates a DSL eval failure (SignalExpr fork) or an
//  invalid dims/panel build.
// ===========================================================================
[[nodiscard]] inline atx::core::Result<alpha::Panel>
generate_synthetic_panel(atx::u64 seed, const PlantedSpec &spec, Dims dims) {
  const atx::usize cells = dims.dates * dims.instruments;
  const std::vector<atx::f64> base = detail::base_close(seed, dims);

  // 1. The planted signal matrix S(t, i) — the DSL-expr output OR the latent factor.
  std::vector<atx::f64> signal;
  switch (spec.kind) {
  case PlantedKind::SignalExpr: {
    ATX_TRY(signal, detail::eval_expr_signal(spec.signal_expr, base, dims));
    break;
  }
  case PlantedKind::LatentFactor: {
    signal = detail::latent_factor(seed, dims);
    break;
  }
  }

  // 2. Per-date cross-sectional z-score of the signal (the return tilt).
  std::vector<atx::f64> zsig(cells, 0.0);
  for (atx::usize t = 0; t < dims.dates; ++t) {
    const atx::usize off = t * dims.instruments;
    detail::zscore_row(std::span<const atx::f64>{signal.data() + off, dims.instruments},
                       std::span<atx::f64>{zsig.data() + off, dims.instruments});
  }

  // 3. Integrate the close: ret(t,i) = beta·zsig(t-1,i) + noise·eps(t,i) (CAUSAL —
  // the return at t is driven by the signal at t-1, a one-step-ahead predictor).
  std::vector<atx::f64> close(cells);
  std::vector<atx::f64> px(dims.instruments, 100.0);
  // A fresh noise stream, re-seeded so it does not alias the price/factor draws.
  detail::SynthRng noise{seed ^ 0xD1B54A32D192ED03ULL};
  for (atx::usize t = 0; t < dims.dates; ++t) {
    for (atx::usize j = 0; j < dims.instruments; ++j) {
      const atx::f64 e = noise.next();
      atx::f64 ret = spec.noise_sigma * e;
      if (t >= 1U) {
        ret += spec.beta * zsig[(t - 1U) * dims.instruments + j];
      }
      px[j] *= (1.0 + ret);
      close[t * dims.instruments + j] = px[j];
    }
  }

  // 4. Assemble the {"close", "signal"} panel. The "signal" field carries the
  // Z-SCORED planted signal zsig — the EXACT per-date return tilt that drives the
  // forward returns — so planted_signal_pnl(panel) trading "signal" directly is
  // the maximal-edge recovery TARGET (no rank() re-projection, no base/final
  // divergence). The GA is handed only "close" (panel_fields), so it never peeks
  // at "signal".
  std::vector<std::vector<atx::f64>> cols;
  cols.reserve(2);
  cols.push_back(std::move(close));
  cols.push_back(std::move(zsig));
  return alpha::Panel::create(dims.dates, dims.instruments, {"close", "signal"}, std::move(cols),
                              {});
}

// ===========================================================================
//  planted_signal_pnl — the planted signal's OWN realized OOS PnL stream: trade
//  the "signal" field (the z-scored planted tilt) DIRECTLY over the synthetic
//  panel through the SAME extract_streams path the factory fitness uses (no look-
//  ahead — w[t-1] earns ret[t]). Trading the raw planted tilt is the maximal-edge
//  alpha, so this PnL is the strongest recovery TARGET recovery_correlation
//  compares survivors against. Err propagates a DSL eval / stream-extract failure
//  (e.g. a panel without a "signal" field).
// ===========================================================================
[[nodiscard]] inline atx::core::Result<std::vector<atx::f64>>
planted_signal_pnl(const alpha::Panel &panel, const WeightPolicy &policy,
                   const exec::ExecutionSimulator &sim) {
  alpha::Library dsl;
  // "signal" alone is the raw z-scored planted tilt — the exact return-driving
  // alpha; no rank() re-projection (which would discard its magnitude).
  ATX_TRY(alpha::Ast ast, alpha::parse_expr("signal", dsl));
  ATX_TRY(alpha::Analysis info, alpha::analyze(ast));
  ATX_TRY(alpha::Program prog, alpha::compile(ast, info));
  alpha::Engine engine{panel};
  ATX_TRY(alpha::SignalSet sig, engine.evaluate(prog));
  ATX_TRY(alpha::AlphaStreams strm, alpha::extract_streams(sig, policy, panel, sim));
  const std::span<const atx::f64> pnl = strm.pnl(0);
  return atx::core::Ok(std::vector<atx::f64>{pnl.begin(), pnl.end()});
}

// ===========================================================================
//  recovery_correlation — the recovery statistic: the MEAN over admitted
//  survivors of |pairwise_complete_corr(survivor_oos_pnl, planted_pnl)|.
//
//  Each survivor's realized OOS PnL is correlated against the planted signal's
//  own OOS PnL via the shared pairwise-complete Pearson (combine §3.3); the
//  absolute value treats a perfect copy and a perfect anti-correlation as equally
//  "recovered" (a reversed-sign rediscovery is still the same structure). An
//  EMPTY survivor set => 0 (nothing recovered). PURE; no RNG. Reuses the ONE corr
//  machinery — no new correlation convention.
// ===========================================================================
[[nodiscard]] inline atx::f64
recovery_correlation(std::span<const std::span<const atx::f64>> survivors,
                     std::span<const atx::f64> planted) noexcept {
  if (survivors.empty()) {
    return 0.0;
  }
  atx::f64 acc = 0.0;
  for (const std::span<const atx::f64> s : survivors) {
    const atx::f64 c = combine::pairwise_complete_corr(s, planted);
    acc += (c < 0.0) ? -c : c; // |corr| — a sign-flipped rediscovery still counts
  }
  return acc / static_cast<atx::f64>(survivors.size());
}

} // namespace atx::engine::eval
