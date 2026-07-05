#pragma once

// CStar (C16M) calibration — a port of the C17 `ats-vol` CStar calibrator
// (ats_calibrate_cstar.c, plus the vol-domain block LM in ats_vol_cstar.c)
// into idiomatic C++20 (agent profile .agents/cpp/agent.md).
//
// Three entry points, in increasing scope:
//
//   1. cstar_lm_inner_block_w   — the vol-domain (total-variance target)
//      Marquardt-damped block-coordinate LM step. Used to unit-test the
//      block-LM machinery against synthetic w-targets from a known-truth
//      slice, exactly like the C `ats_vol_cstar_lm_inner_block_w`. The modal
//      fit is linear-in-coefficients; every damped normal-equations solve goes
//      through atx-core's `solve_spd` (no hand-rolled Cholesky).
//
//   2. cstar_seed_from_essvi    — closed-form base match (ATM level / skew /
//      curvature / wing slopes) + ridge-LSQ modal fit at 41 z-knots against an
//      eSSVI slice, then a no-arb projection. The ridge solve is `solve_spd`.
//
//   3. cstar_calibrate_slice    — the price-domain per-slice pipeline: seed
//      from eSSVI, then an IRLS-Huber (k = 1.345) block-coordinate LM
//      (BASE → MODAL → FULL) against the chain's filtered observation set
//      (built with `build_observations`), a final no-arb projection, and a
//      seed-revert quality gate.
//
// PORT NOTE — the surface-level orchestration `ats_vol_cstar_calib_surface`
// (which first fits a full eSSVI *seed surface* via `ats_vol_essvi_calib_surface`
// and then loops the per-slice pipeline over an arena + matched chains) is NOT
// reproduced here: the eSSVI *surface* calibrator has not yet been ported into
// atx-vol, so there is no in-tree source of the eSSVI seed surface. The
// caller-facing seam that survives is `cstar_calibrate_slice`, which takes an
// already-fitted eSSVI seed slice; a surface driver can loop it once that
// dependency lands. The Andersen-Lake American-exercise correction the C added
// to every predicted price (`P_pred = P_eu + F·C`) is likewise deferred: this
// port prices European (Black-76) only. Both deferrals are marked with
// `// PORT NOTE:` at their sites in cstar_calib.cpp.
//
// Thread-safety: every function is a pure transform of its inputs (the `slice`
// out-parameter of the LM is the only mutated state, owned by the caller). No
// globals, no shared state — safe to call concurrently on distinct slices.

#include <cstdint>
#include <span>

#include "atx/vol/calib.hpp"        // CalibOpts, FitObs, build_observations, Chain
#include "atx/vol/cstar.hpp"        // CStarParams, CStarBlock
#include "atx/vol/types.hpp"        // Result, Status
#include "atx/vol/vol_surface.hpp"  // EssviParams, essvi_total_w

namespace atx::vol {

// Outcome of a single block-coordinate LM run (maps the C's 0 / 1 return):
// Accepted ⇔ at least one damped step reduced the SSE; Exhausted ⇔ the inner
// iteration budget ran out (or the block was under-determined) with no step
// accepted. A zero-dimension block (e.g. MODAL on a C5 slice) is Accepted with
// the slice untouched.
enum class CStarLmStatus : std::uint8_t {
  Accepted = 0,
  Exhausted = 1,
};

// Marquardt-damped LM step on a named block, fitting `w_target[i]` at
// `k_log[i]` in total-variance space. `spread_w[i]` is the per-row uncertainty
// in w-units (empty ⇒ 1.0); `w_obs[i]` is a per-row IRLS weight (empty ⇒ 1.0).
// `lambda_inout` is the Marquardt damping factor — read as the starting value,
// written back so callers can chain. Inactive modes are skipped via
// `slice.active_modes`.
//
// @return InvalidArgument for empty/size-mismatched target spans or
//         max_inner_iters ≤ 0; otherwise Ok(Accepted | Exhausted).
[[nodiscard]] Result<CStarLmStatus> cstar_lm_inner_block_w(
    CStarParams& slice, CStarBlock block, std::span<const double> k_log,
    std::span<const double> w_target, std::span<const double> spread_w,
    std::span<const double> w_obs, int max_inner_iters, double& lambda_inout);

// Seed a CStar slice from an eSSVI slice: match θ = w_essvi(0), the ATM
// derivatives (→ s2, c2) and asymptotic wing slopes (→ C_left, C_right) in
// closed form, ridge-LSQ the 11 modal coefficients against (w_essvi − w_base)
// on 41 z-knots, then project to no-arb. Tags the slice C16 (all modes active).
//
// @return InvalidArgument if the eSSVI ATM variance w_essvi(0) is not > 0.
[[nodiscard]] Result<CStarParams> cstar_seed_from_essvi(const EssviParams& src);

// Price-domain per-slice calibration (Black-76 European; see the AL PORT NOTE).
// Seeds from `essvi_seed`, builds the filtered observation set for `chain`
// (using F/T from the eSSVI slice and the caller-supplied discount factor
// `df`), runs the IRLS-Huber block LM, projects to no-arb, and quality-gates
// against the seed (reverting when the fit is worse than its own seed by > 5%).
//
// @return whatever `build_observations` returns on failure (NotFound when
//         fewer than 5 quotes survive, InvalidArgument on a malformed chain),
//         or InvalidArgument if the eSSVI seed is degenerate; else the fitted
//         (or seed-reverted) slice.
[[nodiscard]] Result<CStarParams> cstar_calibrate_slice(
    const EssviParams& essvi_seed, const Chain& chain, double df,
    const CalibOpts& opts);

}  // namespace atx::vol
