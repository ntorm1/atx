#pragma once

// atx::engine::risk — PortfolioOptimizer: turnover-penalized risk-aware optimizer
// (P4-9). Solves the long/short mean-variance book with a dollar-neutral, gross-
// leverage, per-name-cap constrained, turnover-penalized objective:
//
//     maximize_w   αᵀw  −  λ·wᵀVw  −  κ·‖w − w_prev‖₁
//     subject to   Σ w = 0          (dollar-neutral)
//                  Σ |w| ≤ L        (gross leverage)
//                  |w_i| ≤ cap      (per-name cap)
//
// where α is the combined mega-alpha (an expected-return proxy, length-M span) and V
// is the FACTORED risk::FactorModel (Barra V = X F Xᵀ + D, kept factored). The risk
// term is touched ONLY through the model's factored apply path (apply_inverse /
// risk) — V is NEVER materialized as a dense M×M matrix, so a solve is
// O(iters·(MK + M)), never O(M²).
//
// ===========================================================================
//  The two regimes (λ == 0 vs λ > 0)
// ===========================================================================
//  λ == 0 (no risk term) — handled BY CONSTRUCTION as the canonical WeightPolicy
//  long/short tail so the regression pin is EXACT (≤ 1e-9): the smooth solution is
//  the pure-alpha direction demean(α) (there is no V to invert — skip apply_inverse
//  entirely), then the SAME post-transform pipeline as WeightPolicy:
//      demean(α)  →  gross-normalize (Σ|w| → L)  →  prox-L1 toward w_prev (κ)
//                 →  per-name cap clip-renorm.
//  With κ == 0 the prox step is the identity and with the cap off the clip is the
//  identity, so solve(α, V, {}) reduces to gross_normalize(demean(α)) — bit-for-bit
//  the WeightPolicy dollar-neutral book. (// SAFETY: branching on risk_aversion ==
//  0.0 is the deliberate, documented resolution of the "w* ∝ V⁻¹α is NOT ∝ α"
//  problem — at λ=0 there simply is no risk term to invert, so the pure-alpha
//  direction is the correct smooth solution, and it is the EXACT WeightPolicy book.)
//
//  λ > 0 (risk-aware) — the equality-constrained mean-variance optimum for
//  max αᵀw − λ wᵀVw s.t. Σw=0 is the analytic w* = (1/2λ)·P V⁻¹ P α, where
//  P = I − (1/M)·11ᵀ is the dollar-neutral centering projection (Σ(Pα)=0, and
//  P V⁻¹ P keeps the book centered). This SMOOTH TARGET uses ONLY apply_inverse —
//  no forward Vw is needed (FactorModel exposes no Vw). The inequality terms (the
//  per-name cap) and the L1 turnover penalty are then imposed by a DETERMINISTIC
//  FIXED-ITERATION projected/proximal loop (no convergence-dependent early exit,
//  §3.2): each pass takes a gradient step of the smooth surrogate ½‖w − t‖² toward
//  the target t, a proximal soft-threshold toward w_prev for the κ turnover term,
//  then PROJECTS onto {Σw=0, Σ|w|=L, |w_i|≤cap}. The (1/2λ) scalar on t washes out
//  under the subsequent gross-normalize (Σ|w|→L), so the λ effect on the final book
//  is BINARY — off at λ=0 (pure-alpha direction), on at λ>0 — not λ-graduated: only
//  the λ-independent V⁻¹ DIRECTIONAL tilt away from high-variance names survives, so
//  any λ>0 shrinks high-variance gross relative to λ=0 (pin #3). A κ>0 prox shrinks
//  the move away from w_prev, cutting turnover (pin #4).
//
// ===========================================================================
//  Determinism (§3.2)
// ===========================================================================
//  NO RNG. The loop runs EXACTLY cfg.max_iters passes every time (no early exit),
//  all reductions are order-fixed (ascending index), the projection tie-breaking is
//  stable, and the smooth target / apply_inverse are deterministic given the model.
//  Same inputs ⇒ bitwise-identical weights.
//
// ===========================================================================
//  NaN / out-of-universe handling
// ===========================================================================
//  A NaN α cell ("no opinion") gets EXACTLY 0 weight and is EXCLUDED from the demean
//  and the Σ reductions (mirrors WeightPolicy's NaN→0 exclusion), so the λ=0
//  WeightPolicy recovery stays exact even when α has holes. The dead cells are held
//  at 0 through the whole loop.
//
// ===========================================================================
//  Allocation
// ===========================================================================
//  A solve ALLOCATES its scratch ONCE up front (the WeightPolicy allocate-once-per-
//  rebalance precedent) — never inside the iteration loop. Runs at rebalance
//  cadence, so a single per-call allocation is acceptable.

#include <algorithm> // std::clamp
#include <cmath>     // std::isnan, std::fabs
#include <span>      // std::span (alpha / w_prev / weight spans)
#include <vector>    // std::vector (per-rebalance scratch + result)

#include "atx/core/error.hpp" // Result, Ok, Err, ErrorCode
#include "atx/core/types.hpp" // f64, usize, u8

#include "atx/engine/risk/factor_model.hpp" // FactorModel (factored V apply path)
#include "atx/engine/risk/fwd.hpp"          // OptimizerConfig / PortfolioOptimizer fwd decls

namespace atx::engine::risk {

// ===========================================================================
//  OptimizerConfig — the optimizer knobs (pure configuration).
// ===========================================================================
struct OptimizerConfig {
  atx::f64 risk_aversion = 1.0;    // λ (penalty on wᵀVw); 0 ⇒ pure-alpha WeightPolicy book
  atx::f64 turnover_penalty = 0.0; // κ (penalty on ‖w − w_prev‖₁); 0 ⇒ ignore turnover
  atx::f64 gross_leverage = 1.0;   // L (target Σ|w|, Alpha101 `scale`)
  atx::f64 name_cap = 1.0;         // max |w_i| (≥ L ⇒ effectively uncapped)
  bool dollar_neutral = true;      // Σ w = 0 (subtract the cross-sectional mean)
  atx::usize max_iters = 64;       // FIXED iteration count (determinism §3.2)
};

// ===========================================================================
//  PortfolioOptimizer — risk-aware turnover-penalized solver.
// ===========================================================================
class PortfolioOptimizer {
public:
  OptimizerConfig cfg;

  // Solve the optimizer for the alpha signal `alpha` (length M, NaN == no opinion),
  // the factored risk model `V`, and the previous book `w_prev` (length M, or EMPTY
  // ⇒ a flat all-zero previous book — turnover measured from flat). Returns the
  // length-M target weights. Err on a dimension mismatch (alpha.size() !=
  // V.n_instruments(), or w_prev non-empty and != M). The math is documented in the
  // header block above. Allocates its scratch once; const (pure configuration).
  [[nodiscard]] atx::core::Result<std::vector<atx::f64>>
  solve(std::span<const atx::f64> alpha, const FactorModel &V,
        std::span<const atx::f64> w_prev) const {
    const atx::usize m = V.n_instruments();
    if (alpha.size() != m) {
      return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                            "PortfolioOptimizer::solve: alpha length must equal V.n_instruments()");
    }
    if (!w_prev.empty() && w_prev.size() != m) {
      return atx::core::Err(
          atx::core::ErrorCode::InvalidArgument,
          "PortfolioOptimizer::solve: w_prev must be empty or length V.n_instruments()");
    }

    // --- single up-front scratch allocation (never inside the loop) ----------
    Scratch s(m);
    // `live[i]` marks an α cell with an opinion (non-NaN); dead cells stay 0.
    for (atx::usize i = 0; i < m; ++i) {
      s.live[i] = !std::isnan(alpha[i]);
    }
    // Materialize the previous book ONCE: an EMPTY w_prev is treated as a flat
    // all-zero book (turnover measured from flat), and a dead (NaN-α) cell has no
    // name in the book so its previous weight is 0. Using this single concrete `prev`
    // in BOTH the seed and the prox makes "empty w_prev" behave BITWISE-identically
    // to an explicit all-zero w_prev (the empty-w_prev boundary).
    for (atx::usize i = 0; i < m; ++i) {
      s.prev[i] = (s.live[i] && i < w_prev.size()) ? w_prev[i] : 0.0;
    }

    // Build the FEASIBLE smooth optimum `t`: the raw smooth direction (demean(α) at
    // λ=0, P V⁻¹ P α scaled at λ>0), PROJECTED onto {Σw=0, Σ|w|=L, |w_i|≤cap}. Making
    // t the gross-normalized/capped book (not the raw direction) is what makes the
    // surrogate ½‖w − t‖² STATIONARY at the κ=0 optimum, so:
    //   * κ=0 ⇒ the loop holds w == t == the WeightPolicy / mean-variance book, and
    //   * w_prev == t ⇒ the deviation w − w_prev is exactly 0 at t, so the κ prox is
    //     the identity and t is a genuine no-trade fixed point (the boundary).
    smooth_target(alpha, V, s); // fills s.t with the raw smooth direction
    project(s.t, s);            // t ← the feasible smooth optimum (the κ=0 book)

    // Seed the iterate. With a turnover penalty we START from the previous book
    // (projected to feasibility) so that a w_prev already AT the optimum is a genuine
    // no-trade fixed point; otherwise we seed at t itself. Both seeds reach the same
    // κ=0 fixed point.
    if (cfg.turnover_penalty > 0.0) {
      s.w = s.prev;
    } else {
      s.w = s.t;
    }
    project(s.w, s); // make the seed feasible (centered, on-leverage, capped)

    // FIXED-ITERATION projected/proximal loop — NO convergence early-exit (§3.2).
    for (atx::usize it = 0; it < cfg.max_iters; ++it) {
      gradient_step(s);         // move w toward the smooth target t
      prox_turnover(s);         // soft-threshold toward s.prev (κ); identity if κ==0
      project(s.w, s);          // {Σw=0, Σ|w|=L, |w_i|≤cap}
    }
    return atx::core::Ok(std::move(s.w));
  }

private:
  // Smooth-surrogate gradient-step size. The loop minimizes ½‖w − t‖² toward the
  // target t; a step of kStep on (w − t) is w ← (1 − kStep)·w + kStep·t. kStep = 1
  // would snap straight to t (then project) — using a < 1 step lets the prox + cap
  // projections settle gradually across the fixed iters for a smoother fixed point.
  static constexpr atx::f64 kStep = 0.5;

  // Fixed clip-renorm passes inside project() to settle which names bind the cap
  // while holding Σ|w| == L for a feasible cap (mirrors WeightPolicy::kTruncateIters).
  static constexpr atx::usize kCapIters = 8;

  // Per-call scratch — allocated ONCE in solve(), reused across all iterations.
  struct Scratch {
    explicit Scratch(atx::usize m)
        : w(m, 0.0), t(m, 0.0), prev(m, 0.0), buf(m, 0.0), live(m, false), n(m) {}
    std::vector<atx::f64> w;    // the working iterate (the result on return)
    std::vector<atx::f64> t;    // the smooth target direction
    std::vector<atx::f64> prev; // the previous book (empty w_prev → all-zero flat)
    std::vector<atx::f64> buf;  // apply_inverse output / general M-scratch
    std::vector<bool> live;     // live[i] == α_i has an opinion (non-NaN)
    atx::usize n;               // M
  };

  // Subtract the mean of the LIVE cells from every live cell (dollar-neutral). Dead
  // (NaN-α) cells are excluded from the mean and held at 0. Order-fixed reduction.
  // When dollar_neutral is off this is a no-op (the book may carry net exposure).
  void demean_live(std::vector<atx::f64> &v, const Scratch &s) const noexcept {
    if (!cfg.dollar_neutral) {
      return;
    }
    atx::f64 sum = 0.0;
    atx::usize cnt = 0;
    for (atx::usize i = 0; i < s.n; ++i) {
      if (s.live[i]) {
        sum += v[i];
        ++cnt;
      }
    }
    if (cnt == 0) {
      return;
    }
    const atx::f64 mean = sum / static_cast<atx::f64>(cnt);
    for (atx::usize i = 0; i < s.n; ++i) {
      v[i] = s.live[i] ? (v[i] - mean) : 0.0;
    }
  }

  // Fill `s.t` with the RAW smooth target direction (solve() then projects it onto
  // the feasible set to get the gross-normalized κ=0 book):
  //   λ == 0: t = demean(α)              (pure-alpha; the WeightPolicy direction)
  //   λ  > 0: t = (1/2λ)·P V⁻¹ P α        (the dollar-neutral mean-variance optimum)
  // P is the centering projection (demean_live). For λ>0 the V⁻¹ apply uses ONLY the
  // factored model's apply_inverse — never a forward Vw or a dense V. Dead α cells
  // are 0 in P(α) and held at 0 through V⁻¹ (the model treats them as 0-weight names).
  void smooth_target(std::span<const atx::f64> alpha, const FactorModel &V, Scratch &s) const {
    // c = P·(α with NaN→0): centered, dead cells zeroed.
    for (atx::usize i = 0; i < s.n; ++i) {
      s.t[i] = s.live[i] ? alpha[i] : 0.0;
    }
    demean_live(s.t, s); // c = P α

    if (cfg.risk_aversion == 0.0) {
      // SAFETY: λ=0 ⇒ no risk term to invert; the pure-alpha direction demean(α) IS
      // the correct smooth solution and (after gross-normalize) reduces solve() to the
      // EXACT WeightPolicy dollar-neutral book (the non-negotiable regression pin #1).
      return; // s.t already holds demean(α)
    }

    // λ>0: u = V⁻¹·c via the factored Woodbury apply (O(MK+K³), no dense V).
    V.apply_inverse(std::span<const atx::f64>(s.t), std::span<atx::f64>(s.buf)); // buf = V⁻¹ c
    // t = (1/2λ)·P·u. Zero the dead cells first (a NaN α has no name in the book),
    // then re-center so Σt = 0, then scale by 1/(2λ). (The scale is immaterial after
    // the subsequent gross-normalize, but the V⁻¹ tilt away from high-variance names
    // — pin #3 — survives the normalization.)
    for (atx::usize i = 0; i < s.n; ++i) {
      s.t[i] = s.live[i] ? s.buf[i] : 0.0;
    }
    demean_live(s.t, s); // P u
    const atx::f64 inv = 1.0 / (2.0 * cfg.risk_aversion);
    for (atx::usize i = 0; i < s.n; ++i) {
      s.t[i] *= inv;
    }
  }

  // One gradient step of the smooth surrogate ½‖w − t‖²: w ← (1 − kStep)·w + kStep·t.
  // Pulls the iterate toward the smooth target; dead cells stay 0 (t and w are 0 there).
  void gradient_step(Scratch &s) const noexcept {
    for (atx::usize i = 0; i < s.n; ++i) {
      s.w[i] = (1.0 - kStep) * s.w[i] + kStep * s.t[i];
    }
  }

  // Proximal step for the κ·‖w − w_prev‖₁ turnover term: soft-threshold w TOWARD the
  // previous book s.prev by τ = κ·kStep (the prox of an L1 penalty is a soft-threshold
  // of the deviation w − w_prev). κ == 0 ⇒ τ == 0 ⇒ identity (pin #2). s.prev is the
  // pre-materialized previous book (empty w_prev → flat all-zero). Dead cells stay 0.
  void prox_turnover(Scratch &s) const noexcept {
    const atx::f64 tau = cfg.turnover_penalty * kStep;
    if (tau <= 0.0) {
      return; // κ == 0 ⇒ no turnover term (exactly identity)
    }
    for (atx::usize i = 0; i < s.n; ++i) {
      if (!s.live[i]) {
        s.w[i] = 0.0;
        continue;
      }
      const atx::f64 prev = s.prev[i];
      const atx::f64 dz = s.w[i] - prev;          // deviation from the previous book
      const atx::f64 mag = std::fabs(dz);
      const atx::f64 shr = (mag > tau) ? (mag - tau) : 0.0; // soft-threshold magnitude
      s.w[i] = prev + ((dz < 0.0) ? -shr : shr);  // pull back toward w_prev by τ
    }
  }

  // Project the iterate onto the feasible set {Σw=0 (if dollar_neutral), Σ|w|=L,
  // |w_i|≤cap}: dollar-neutral demean, then a FIXED clip-renorm to impose the cap
  // while driving Σ|w| to L (the deployed gross budget, like WeightPolicy::scale).
  // The cap clip-renorm is the WeightPolicy truncate construction: clip each |w_i| to
  // the cap, re-normalize the unpinned mass to fill the remaining gross budget, end
  // on a hard clip so |w_i| ≤ cap holds. An infeasible cap (cap·n_live < L) pins every
  // name at the cap and Σ|w| settles below L (the cap wins — documented degenerate).
  void project(std::vector<atx::f64> &v, const Scratch &s) const noexcept {
    demean_live(v, s);     // Σw = 0 (no-op if dollar_neutral off)
    gross_normalize(v, s); // Σ|w| → L
    const atx::f64 cap = cfg.name_cap;
    // Only clip if the cap can actually bind (cap < L; a cap ≥ L can never be hit by a
    // single name once Σ|w| = L with ≥ 2 live names).
    if (cap < cfg.gross_leverage) {
      cap_clip_renorm(v, s, cap);
    }
  }

  // Scale the live cells so Σ|w| == L (gross_leverage). All-zero (degenerate
  // demeaned-constant / single live name) ⇒ leave flat (no div-by-zero), matching
  // WeightPolicy::gross_normalize. Order-fixed L1 reduction.
  void gross_normalize(std::vector<atx::f64> &v, const Scratch &s) const noexcept {
    atx::f64 l1 = 0.0;
    for (atx::usize i = 0; i < s.n; ++i) {
      l1 += std::fabs(v[i]);
    }
    if (l1 == 0.0) {
      return;
    }
    const atx::f64 scale = cfg.gross_leverage / l1;
    for (atx::usize i = 0; i < s.n; ++i) {
      v[i] *= scale;
    }
  }

  // FIXED clip-renorm to impose |w_i| ≤ cap while holding Σ|w| ≈ L for a feasible cap
  // (no convergence early-exit, §3.2 — mirrors WeightPolicy::truncate_renorm +
  // finalize). After kCapIters clip-then-renorm passes settle the binding set, a final
  // hard clip + a deficit-renorm of ONLY the unpinned names makes |w| ≤ cap exact and
  // Σ|w| == L exact for a feasible cap; an infeasible cap leaves Σ|w| < L (cap wins).
  void cap_clip_renorm(std::vector<atx::f64> &v, const Scratch &s, atx::f64 cap) const noexcept {
    for (atx::usize iter = 0; iter < kCapIters; ++iter) {
      for (atx::usize i = 0; i < s.n; ++i) {
        v[i] = std::clamp(v[i], -cap, cap);
      }
      atx::f64 l1 = 0.0;
      for (atx::usize i = 0; i < s.n; ++i) {
        l1 += std::fabs(v[i]);
      }
      if (l1 == 0.0) {
        return;
      }
      const atx::f64 scale = cfg.gross_leverage / l1;
      for (atx::usize i = 0; i < s.n; ++i) {
        v[i] *= scale;
      }
    }
    // Final exact pass: hard-clip to the cap, then pour the remaining gross budget
    // onto the UNPINNED (sub-cap) names alone so Σ|w| == L without reopening the cap.
    atx::f64 pinned = 0.0;
    atx::f64 unbound = 0.0;
    for (atx::usize i = 0; i < s.n; ++i) {
      v[i] = std::clamp(v[i], -cap, cap);
      const atx::f64 a = std::fabs(v[i]);
      (a >= cap ? pinned : unbound) += a;
    }
    const atx::f64 target_unbound = cfg.gross_leverage - pinned;
    if (unbound <= 0.0 || target_unbound <= 0.0) {
      return; // infeasible cap: no unpinned mass to absorb the deficit (cap wins)
    }
    const atx::f64 scale = target_unbound / unbound;
    for (atx::usize i = 0; i < s.n; ++i) {
      if (std::fabs(v[i]) < cap) {
        v[i] *= scale; // only the sub-cap names absorb the deficit
      }
    }
  }
};

} // namespace atx::engine::risk
