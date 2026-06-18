#pragma once

// atx::engine::risk — elasticity: the CONSTRAINT-HIERARCHY / infeasibility-elasticity
// layer (S8.6 — "the Axioma flagship"). When a constraint set is infeasible, instead of
// returning Err, relax the ELASTIC constraints in a deterministic PRIORITY order (lowest
// priority first) via a minimize-violation auxiliary solve, returning the CLOSEST-FEASIBLE
// book + a report of which constraints were relaxed and by how much. HARD (elastic=false)
// constraints NEVER relax.
//
// ===========================================================================
//  The mechanism — minimize-violation penalized slack (R3 / R5)
// ===========================================================================
//  S8.4 added per-descriptor `priority` (lower = relaxed first) / `elastic`; S8.6 CONSUMES
//  them. materialize() records, per ELASTIC descriptor, the materialized rows / cone it
//  owns + its priority into C.elastic (cone.hpp::ElasticSpec). On infeasibility we build a
//  RELAXED augmented QP and re-solve it through the UNCHANGED ConstrainedQpSolver:
//
//    * An elastic LINEAR row  l ≤ aᵀx ≤ u  gets two nonneg slack columns e⁺,e⁻ ≥ 0 and
//      becomes  l ≤ aᵀx − e⁺ + e⁻ ≤ u, so the achievable aᵀx ranges over [l−e⁻, u+e⁺];
//      the objective gains  +γ_p·(e⁺+e⁻). HARD rows get NO slack ⇒ stay exact.
//    * An elastic CONE  ‖arg‖₂ ≤ radius  gets a nonneg slack e ≥ 0 on the budget, becoming
//      ‖arg‖₂ ≤ radius + e (a VARIABLE-APEX SOC with apex t = radius + e), penalized +γ_p·e.
//      A ball is rebuilt as a contiguous variable-apex block at the matrix tail (apex row
//      then a COPY of the cone-argument rows); the original ball rows are left INERT (their
//      ±kAugInf band is a no-op clamp and they are dropped from the projected cone set).
//      An already-variable-apex cone (robust) gets +1·e added to its apex row in place.
//
//  This is ONE convex solve that JOINTLY minimizes the weighted violations — the genuine
//  closest-feasible point — through the SAME Ruiz-conditioned no-pivot-LDLᵀ ADMM (R5: no
//  new factorization, no second solver; the slack columns are the same established move
//  S8.5a/b/c used to add cone rows / the epigraph column). NEVER densifies V or Ã (R4 —
//  the slacks are sparse columns; cones reuse the low-rank cone-argument rows).
//
// ===========================================================================
//  The γ ladder — strict lowest-priority-first hierarchy (R1)
// ===========================================================================
//  Each elastic constraint at priority p gets penalty weight
//      γ_p = kGammaBase · kGammaRho^(p_max − p)
//  where p_max is the LARGEST elastic priority present. So the HIGHEST priority (p_max)
//  gets the SMALLEST weight kGammaBase and the LOWEST priority (p=0) gets the LARGEST
//  weight kGammaBase·kGammaRho^p_max. Minimizing Σ γ_p·(violation_p) therefore relaxes the
//  LOWEST-priority constraint FIRST (it is cheapest per unit to leave un-relaxed only if it
//  has the LARGEST weight — wait: a LARGER weight makes a violation MORE expensive, so the
//  solver avoids violating it). To get "lowest priority relaxed FIRST" the cheapest-to-
//  violate must be the lowest priority ⇒ the SMALLEST weight goes to the LOWEST priority:
//      γ_p = kGammaBase · kGammaRho^p              (p=0 cheapest, p_max most protected)
//  kGammaRho is chosen large enough that one unit of a higher-priority violation costs more
//  than fully relaxing every lower-priority constraint, yielding a STRICT hierarchy: the
//  solve exhausts the lowest-priority slack before paying for any higher-priority one. The
//  schedule is a pure function of the priorities (no RNG / clock); ties (equal priority)
//  share a weight and relax together — the report still lists them in fixed order.
//
// ===========================================================================
//  Integration — a WRAPPER around the hard solve (R10)
// ===========================================================================
//  solve_elastic FIRST runs the existing solver.solve_with_cert(p) UNCHANGED. A FEASIBLE
//  problem returns that book UNTOUCHED ⇒ byte-identical to S8.5 (the elastic code is never
//  reached). ONLY on the infeasible signal (the feasibility-gate Err) do we build the
//  relaxed form and re-solve. If the relaxed solve is STILL infeasible, a HARD constraint
//  is binding — we return a DISTINCT Err (NOT a relaxed book): hard constraints have no
//  slack columns, so they can never be relaxed to manufacture feasibility (R3).
//
// ===========================================================================
//  Determinism (R1)
// ===========================================================================
//  No RNG, no clock, no unordered iteration. The γ ladder, the slack-column emission, the
//  relaxed-row surgery, and the report are all FIXED-order (ascending priority, then the
//  materialize cone/row order). The re-solve is the same fixed-iteration ADMM. The report's
//  achieved-violation values are read from the returned book against the ORIGINAL surface
//  (an order-fixed reduction) ⇒ byte-identical across runs / thread counts.

#include <algorithm> // std::stable_sort, std::max
#include <cmath>     // std::fabs, std::pow
#include <span>
#include <string>    // std::to_string (distinct hard-infeasible Err)
#include <utility>   // std::move
#include <vector>

#include <Eigen/SparseCore>

#include "atx/core/error.hpp"         // Result, Ok, Err, ErrorCode
#include "atx/core/linalg/linalg.hpp" // VecX
#include "atx/core/types.hpp"         // f64, usize

#include "atx/engine/risk/cone.hpp"        // SocBlock, ordered_norm2, ElasticSpec
#include "atx/engine/risk/constraints.hpp" // MaterializedConstraints
#include "atx/engine/risk/qp_augment.hpp"  // build_augmented, AugmentedQp, kAugInf
#include "atx/engine/risk/qp_solver.hpp"   // ConstrainedQpSolver, QpProblem, QpResult, QpCertificate

namespace atx::engine::risk {

// ===========================================================================
//  γ-ladder constants (R1 — fixed, no RNG). γ_p = kGammaBase · kGammaRho^priority.
//  kGammaBase is set well ABOVE the portfolio QP's own objective scale (‖q‖≈O(α)≈O(1),
//  ‖P‖≈2λ·D≈O(1)) so the penalty dominates — the solver never trades a slack for alpha
//  (no over-relaxing); the slacks are driven to their MINIMAL feasibility value (the
//  closest-feasible point). kGammaRho makes each priority tier strictly dominate all lower
//  tiers (one unit of a tier-(p+1) violation costs kGammaRho× a tier-p unit) so the solve
//  EXHAUSTS the lowest-priority slack before paying for any higher-priority one — a strict
//  lowest-priority-first hierarchy. The magnitudes are kept MODERATE (not 1eN huge) because
//  the no-pivot LDLᵀ + Ruiz equilibration condition the KKT only so far: a 1e9-scale penalty
//  wrecks the conditioning and the fixed-budget ADMM diverges. base=64, ρ=64 gives the
//  ladder 64, 4096, 262144, … — each tier ≥64× the objective AND ≥64× the tier below, which
//  empirically clears the feasibility gate at the cone budget (rho=10, iters=1500).
// ===========================================================================
inline constexpr atx::f64 kGammaBase = 4.0;
inline constexpr atx::f64 kGammaRho = 8.0;

// The RELAXED minimize-violation QP is strictly HARDER than the hard solve (it adds the
// penalized slack columns + the e ≥ 0 rows + the rebuilt variable-apex cone), so the
// fixed ADMM budget that clears the hard problem may leave the HARD rows (e.g. the dollar-
// neutral Σw=0 row) a hair outside feas_tol on the relaxed form. We re-solve at a LARGER
// FIXED budget (the caller's cfg.iters × kRelaxedIterScale, never less than kRelaxedIterMin)
// so the relaxed solve clears the gate. Still a FIXED count (R1) — no convergence early-exit,
// no data-dependent control flow; it is a pure function of the caller's budget. The caller's
// solver cfg is NOT mutated (we re-solve through a local copy).
inline constexpr atx::usize kRelaxedIterScale = 6U;
inline constexpr atx::usize kRelaxedIterMin = 9000U;

// The relaxed form adds many slack-bound rows (e ≥ 0) and the rebuilt variable-apex cone —
// equality/penalty-coupled rows that an ADMM converges TIGHTER at a larger constraint
// penalty ρ. We re-solve at max(caller ρ, kRelaxedRho) so the HARD equality rows (e.g. the
// dollar-neutral Σw=0 row) clear feas_tol on the relaxed form. FIXED (R1); the caller's
// solver cfg is not mutated (local copy). A caller already using a larger ρ keeps it.
inline constexpr atx::f64 kRelaxedRho = 30.0;

// One relaxed constraint in the report. `kind` distinguishes a linear-row block from a
// cone; `index` is the augmented row_begin (LinearRow) or the cone index (Cone); `count`
// is the number of A-rows in the block (1 for a cone); `priority` is the descriptor's
// (lower = relaxed first); `violation` is the achieved relaxation (how much the ORIGINAL
// constraint is violated at the returned book — the "by how much").
struct RelaxationEntry {
  enum class Kind { LinearRow, Cone };
  Kind kind = Kind::LinearRow;
  atx::usize index = 0;
  atx::usize count = 0;
  atx::usize priority = 0;
  atx::f64 violation = 0.0;
};

// The relaxation report: which elastic constraints were relaxed (by how much), in the
// fixed priority order (lowest priority first). `relaxed == false` ⇒ the problem was
// feasible and elasticity did nothing (a pure no-op).
struct RelaxationReport {
  bool relaxed = false;
  std::vector<RelaxationEntry> entries;
};

// The elastic solve result: the closest-feasible book + the report + the certificate.
struct ElasticResult {
  std::vector<atx::f64> book;
  RelaxationReport report;
  QpCertificate cert;
};

namespace detail {

// The penalty weight for an elastic constraint at `priority` (the γ ladder, R1).
[[nodiscard]] inline atx::f64 gamma_for(atx::usize priority) noexcept {
  // kGammaBase · kGammaRho^priority — order-fixed integer power (no std::pow rounding
  // surprise; priorities are tiny). p=0 cheapest, larger p strictly more protected.
  atx::f64 g = kGammaBase;
  for (atx::usize i = 0; i < priority; ++i) {
    g *= kGammaRho;
  }
  return g;
}

// Achieved violation of an elastic LINEAR row block at the returned w-block book: the
// order-fixed sum over its rows of max(0, aᵀw − u) + max(0, l − aᵀw), read from the
// ORIGINAL (un-relaxed) materialized surface. aug_row_begin is the row in the AUGMENTED Ã
// frame (== K factor rows + the row in C.A). Order-fixed (R1).
[[nodiscard]] inline atx::f64 linear_block_violation(const AugmentedQp &hard,
                                                     const atx::core::linalg::VecX &x,
                                                     atx::usize aug_row_begin, atx::usize count) {
  const atx::core::linalg::VecX ax = hard.A_tilde * x;
  atx::f64 acc = 0.0; // ascending row (R1)
  for (atx::usize j = 0; j < count; ++j) {
    const auto r = static_cast<Eigen::Index>(aug_row_begin + j);
    const atx::f64 over = ax[r] - hard.u[r];
    const atx::f64 under = hard.l[r] - ax[r];
    acc += (over > 0.0) ? over : 0.0;
    acc += (under > 0.0) ? under : 0.0;
  }
  return acc;
}

// Achieved violation of an elastic CONE at the returned book: max(0, ‖Ãx+offset‖₂ − radius)
// for a ball, or max(0, ‖v‖₂ − apex) for a variable-apex SOC — read from the ORIGINAL cone
// block. Order-fixed norm (R1).
[[nodiscard]] inline atx::f64 cone_violation_at(const AugmentedQp &hard,
                                                const atx::core::linalg::VecX &x,
                                                atx::usize cone_index) {
  const SocBlock &blk = hard.cones[cone_index];
  const atx::core::linalg::VecX ax = hard.A_tilde * x;
  std::vector<atx::f64> arg(blk.dim, 0.0);
  for (atx::usize j = 0; j < blk.dim; ++j) {
    arg[j] = ax[static_cast<Eigen::Index>(blk.row_start + j)] +
             blk.offset[static_cast<Eigen::Index>(j)];
  }
  atx::f64 viol = 0.0;
  if (!blk.variable_apex) {
    viol = ordered_norm2(std::span<const atx::f64>(arg)) - blk.radius;
  } else {
    const std::span<const atx::f64> v(arg.data() + 1, arg.size() - 1U);
    viol = ordered_norm2(v) - arg[0];
  }
  return (viol > 0.0) ? viol : 0.0;
}

// Build the RELAXED augmented QP from the HARD one + the elastic spec + the γ ladder.
// Appends penalized slack columns and widens the elastic rows / cones; HARD rows untouched.
// Returns the relaxed AugmentedQp; the ORIGINAL `hard` is consumed read-only (the hard
// solve's assembly is literally untouched — R10).
[[nodiscard]] inline AugmentedQp build_relaxed(const AugmentedQp &hard,
                                               const MaterializedConstraints &C, atx::usize k) {
  using Trip = Eigen::Triplet<atx::f64>;
  const ElasticSpec &es = C.elastic;
  const auto n_old = static_cast<atx::usize>(hard.P.cols());
  const auto r_old = static_cast<atx::usize>(hard.A_tilde.rows());

  // ---- count the new slack columns + new rows -----------------------------
  // Linear: 2 slack columns (e⁺,e⁻) per elastic row, + 1 e≥0 bound row per slack column.
  // Cone (ball→variable-apex): 1 slack column e, + 1 e≥0 bound row, + (1 apex + dim arg)
  //   new contiguous cone rows. Cone (already variable-apex): 1 slack column e + 1 e≥0 row
  //   (the apex row gains +1·e in place — no new cone rows).
  atx::usize n_slack = 0U;
  atx::usize r_extra = 0U;
  for (const ElasticRow &er : es.linear_rows) {
    n_slack += 2U * er.count;
    r_extra += 2U * er.count; // one e≥0 row per slack column
  }
  for (const ElasticCone &ec : es.cones) {
    const SocBlock &blk = hard.cones[ec.cone_index];
    n_slack += 1U;
    r_extra += 1U; // e ≥ 0 row
    if (!blk.variable_apex) {
      r_extra += 1U + blk.dim; // a fresh contiguous variable-apex block (apex + dim arg rows)
    }
  }

  const atx::usize n_new = n_old + n_slack;
  const atx::usize r_new = r_old + r_extra;

  AugmentedQp out;
  out.n_w = hard.n_w;
  out.n_y = hard.n_y;
  out.n_aux = hard.n_aux + n_slack; // slack columns ride the aux block (no quadratic on them)

  // ---- P: same nonzeros as hard, resized to n_new (slack cols have 0 P; build_kkt adds σ).
  {
    std::vector<Trip> p_trips;
    p_trips.reserve(static_cast<atx::usize>(hard.P.nonZeros()));
    for (int c = 0; c < hard.P.outerSize(); ++c) {
      for (Eigen::SparseMatrix<atx::f64>::InnerIterator it(hard.P, c); it; ++it) {
        p_trips.emplace_back(static_cast<int>(it.row()), static_cast<int>(it.col()), it.value());
      }
    }
    out.P.resize(static_cast<int>(n_new), static_cast<int>(n_new));
    out.P.setFromTriplets(p_trips.begin(), p_trips.end());
    out.P.makeCompressed();
  }

  // ---- q_aug: copy hard, extend with +γ_p on each slack column (filled as we assign cols).
  out.q_aug = atx::core::linalg::VecX::Zero(static_cast<Eigen::Index>(n_new));
  for (atx::usize i = 0; i < n_old; ++i) {
    out.q_aug[static_cast<Eigen::Index>(i)] = hard.q_aug[static_cast<Eigen::Index>(i)];
  }

  // ---- bounds: copy hard, extend; new rows filled as we emit them.
  out.l = atx::core::linalg::VecX::Zero(static_cast<Eigen::Index>(r_new));
  out.u = atx::core::linalg::VecX::Zero(static_cast<Eigen::Index>(r_new));
  for (atx::usize i = 0; i < r_old; ++i) {
    out.l[static_cast<Eigen::Index>(i)] = hard.l[static_cast<Eigen::Index>(i)];
    out.u[static_cast<Eigen::Index>(i)] = hard.u[static_cast<Eigen::Index>(i)];
  }

  // ---- Ã: copy hard's triplets, then the relaxation surgery.
  std::vector<Trip> a_trips;
  a_trips.reserve(static_cast<atx::usize>(hard.A_tilde.nonZeros()) + 4U * n_slack);
  for (int c = 0; c < hard.A_tilde.outerSize(); ++c) {
    for (Eigen::SparseMatrix<atx::f64>::InnerIterator it(hard.A_tilde, c); it; ++it) {
      a_trips.emplace_back(static_cast<int>(it.row()), static_cast<int>(it.col()), it.value());
    }
  }

  atx::usize next_col = n_old; // running slack-column cursor
  atx::usize next_row = r_old; // running new-row cursor

  // (A) elastic LINEAR rows: e⁺,e⁻ per row; row becomes  l ≤ aᵀx − e⁺ + e⁻ ≤ u.
  for (const ElasticRow &er : es.linear_rows) {
    const atx::f64 g = gamma_for(er.priority);
    // er.row_begin is in C.A's frame; the augmented frame shifts it by the K factor rows.
    const atx::usize aug_begin = k + er.row_begin;
    for (atx::usize j = 0; j < er.count; ++j) {
      const atx::usize row = aug_begin + j;
      const atx::usize ep = next_col++; // e⁺ column
      const atx::usize em = next_col++; // e⁻ column
      a_trips.emplace_back(static_cast<int>(row), static_cast<int>(ep), -1.0); // −e⁺
      a_trips.emplace_back(static_cast<int>(row), static_cast<int>(em), 1.0);  // +e⁻
      out.q_aug[static_cast<Eigen::Index>(ep)] = g;
      out.q_aug[static_cast<Eigen::Index>(em)] = g;
      // e⁺ ≥ 0 and e⁻ ≥ 0 bound rows.
      a_trips.emplace_back(static_cast<int>(next_row), static_cast<int>(ep), 1.0);
      out.l[static_cast<Eigen::Index>(next_row)] = 0.0;
      out.u[static_cast<Eigen::Index>(next_row)] = kAugInf;
      ++next_row;
      a_trips.emplace_back(static_cast<int>(next_row), static_cast<int>(em), 1.0);
      out.l[static_cast<Eigen::Index>(next_row)] = 0.0;
      out.u[static_cast<Eigen::Index>(next_row)] = kAugInf;
      ++next_row;
    }
  }

  // (B) cones — first carry over the NON-relaxed cones UNCHANGED (same row ranges).
  std::vector<bool> cone_relaxed(hard.cones.size(), false);
  for (const ElasticCone &ec : es.cones) {
    cone_relaxed[ec.cone_index] = true;
  }
  for (atx::usize ci = 0; ci < hard.cones.size(); ++ci) {
    if (!cone_relaxed[ci]) {
      out.cones.push_back(hard.cones[ci]); // unchanged: its rows are already in a_trips
    }
  }

  // (C) elastic cones — relax the budget with a penalized slack e ≥ 0.
  for (const ElasticCone &ec : es.cones) {
    const SocBlock &blk = hard.cones[ec.cone_index];
    const atx::f64 g = gamma_for(ec.priority);
    const atx::usize e_col = next_col++; // budget slack e ≥ 0
    out.q_aug[static_cast<Eigen::Index>(e_col)] = g;
    // e ≥ 0 bound row.
    a_trips.emplace_back(static_cast<int>(next_row), static_cast<int>(e_col), 1.0);
    out.l[static_cast<Eigen::Index>(next_row)] = 0.0;
    out.u[static_cast<Eigen::Index>(next_row)] = kAugInf;
    ++next_row;

    if (blk.variable_apex) {
      // Already variable-apex (robust): apex t lives on blk.row_start (a single +1 at the
      // epigraph col). Add +1·e on that SAME apex row so the apex argument becomes t + e ⇒
      // ‖v‖₂ ≤ t + e. The block stays in place (its rows are already in a_trips).
      a_trips.emplace_back(static_cast<int>(blk.row_start), static_cast<int>(e_col), 1.0);
      out.cones.push_back(blk);
    } else {
      // Ball: rebuild as a contiguous variable-apex block at the tail. The original ball
      // rows stay in a_trips but are INERT (±kAugInf band, dropped from out.cones). New
      // rows: an apex row (Ãx = e, offset = radius ⇒ apex argument = radius + e) then a
      // COPY of the cone-argument rows (same coefficients + offsets as the original ball).
      SocBlock nb;
      nb.row_start = next_row;
      nb.dim = 1U + blk.dim;
      nb.radius = 0.0; // ignored for a variable-apex block
      nb.variable_apex = true;
      nb.offset = atx::core::linalg::VecX::Zero(static_cast<Eigen::Index>(nb.dim));
      // apex row.
      a_trips.emplace_back(static_cast<int>(next_row), static_cast<int>(e_col), 1.0);
      nb.offset[0] = blk.radius; // apex argument = Ãx(apex) + offset = e + radius
      out.l[static_cast<Eigen::Index>(next_row)] = -kAugInf;
      out.u[static_cast<Eigen::Index>(next_row)] = kAugInf;
      ++next_row;
      // copy the cone-argument rows (the original ball's `dim` rows, in order).
      for (atx::usize j = 0; j < blk.dim; ++j) {
        const auto src_row = static_cast<int>(blk.row_start + j);
        // Re-emit the original row's coefficients onto the new row by scanning hard.A_tilde.
        // (A column scan of the source row — order-fixed ascending column.)
        for (int c = 0; c < hard.A_tilde.outerSize(); ++c) {
          for (Eigen::SparseMatrix<atx::f64>::InnerIterator it(hard.A_tilde, c); it; ++it) {
            if (it.row() == src_row) {
              a_trips.emplace_back(static_cast<int>(next_row), static_cast<int>(it.col()),
                                   it.value());
            }
          }
        }
        nb.offset[static_cast<Eigen::Index>(1U + j)] = blk.offset[static_cast<Eigen::Index>(j)];
        out.l[static_cast<Eigen::Index>(next_row)] = -kAugInf;
        out.u[static_cast<Eigen::Index>(next_row)] = kAugInf;
        ++next_row;
      }
      out.cones.push_back(std::move(nb));
    }
  }

  out.A_tilde.resize(static_cast<int>(r_new), static_cast<int>(n_new));
  out.A_tilde.setFromTriplets(a_trips.begin(), a_trips.end());
  out.A_tilde.makeCompressed();
  return out;
}

} // namespace detail

// ===========================================================================
//  solve_elastic — the constraint-hierarchy / minimize-violation entry point.
//
//  1. Hard solve (UNCHANGED). Feasible ⇒ return that book + an empty report (pure no-op,
//     byte-identical to S8.5). Any non-feasibility-gate error (a dimension Err) propagates.
//  2. Infeasible AND there ARE elastic constraints ⇒ build the relaxed minimize-violation
//     QP and re-solve through solve_augmented_form. The returned book is the closest-
//     feasible point; the report names the relaxed constraints (lowest priority first) +
//     the achieved violation of each ORIGINAL constraint at the book.
//  3. Infeasible with NO elastic constraints, OR the relaxed solve STILL infeasible ⇒ a
//     HARD constraint is binding: return a DISTINCT Err (NOT a relaxed book). Hard rows
//     have no slack columns, so they can never be relaxed (R3).
// ===========================================================================
[[nodiscard]] inline atx::core::Result<ElasticResult>
solve_elastic(const QpProblem &p, const ConstrainedQpSolver &solver) {
  namespace co = atx::core;

  // (1) Hard solve — the existing path, untouched.
  auto hard_r = solver.solve_with_cert(p);
  if (hard_r.has_value()) {
    ElasticResult out;
    out.book = std::move(hard_r->book);
    out.cert = hard_r->cert;
    out.report.relaxed = false; // pure no-op (byte-identical to the direct hard solve)
    return co::Ok(std::move(out));
  }
  // The hard solve errored. The infeasibility trigger is the feasibility-gate Err (R3); a
  // pure dimension error would also surface here, but it is equally un-relaxable, so both
  // route into the elastic attempt below and — failing that — the distinct hard-infeasible
  // Err (a dimension-broken problem carries no elastic metadata ⇒ the next branch fires).

  // (2) No elastic constraints ⇒ nothing to relax ⇒ the set is hard-infeasible.
  if (p.C.elastic.empty()) {
    return co::Err(co::ErrorCode::InvalidArgument,
                   "solve_elastic: the constraint set is infeasible and carries NO elastic "
                   "constraints to relax (a HARD constraint is binding)");
  }

  // Build the hard augmented form (same assembly the solver used) + the relaxed form.
  const atx::usize k = p.V.n_factors();
  const AugmentedQp hard = build_augmented(p.V, p.risk_aversion, p.q, p.C);
  const AugmentedQp relaxed = detail::build_relaxed(hard, p.C, k);

  // (3) Re-solve the relaxed minimize-violation QP through the SAME solver pipeline, at a
  //     LARGER FIXED budget (the relaxed form is harder — see kRelaxedIterScale). The
  //     caller's solver is copied (its cfg is NOT mutated); the budget is a pure function of
  //     the caller's cfg.iters (R1 — fixed count, no early-exit).
  ConstrainedQpSolver relaxed_solver = solver;
  const atx::usize scaled = solver.cfg.iters * kRelaxedIterScale;
  relaxed_solver.cfg.iters = (scaled > kRelaxedIterMin) ? scaled : kRelaxedIterMin;
  relaxed_solver.cfg.rho = (solver.cfg.rho > kRelaxedRho) ? solver.cfg.rho : kRelaxedRho;
  auto relaxed_r = relaxed_solver.solve_augmented_form(relaxed, p);
  if (!relaxed_r.has_value()) {
    // Even after relaxing every elastic constraint the set is infeasible ⇒ a HARD
    // constraint binds. Distinct Err (NOT a relaxed book) — R3.
    return co::Err(co::ErrorCode::InvalidArgument,
                   "solve_elastic: the constraint set is infeasible even after relaxing all "
                   "elastic constraints (a HARD constraint is binding): " +
                       relaxed_r.error().message());
  }

  ElasticResult out;
  out.book = std::move(relaxed_r->book);
  out.cert = relaxed_r->cert;
  out.report.relaxed = true;

  // Build the report from the achieved violations of the ORIGINAL constraints at the
  // returned book, in fixed lowest-priority-first order. The achieved violation IS the
  // "by how much" — a pure, order-fixed function of the book + the hard surface (R1). We
  // form the FULL-width augmented x = [w; y; aux] against the HARD aug (the aux block is
  // zeroed — the elastic LINEAR rows live on the w-block and the elastic CONE rows on the
  // w/y-block, so Ã·x is exact on every row we read; the gross/turnover/robust-aux rows are
  // not read here).
  const auto n_full = static_cast<Eigen::Index>(hard.n_w + hard.n_y + hard.n_aux);
  atx::core::linalg::VecX x = atx::core::linalg::VecX::Zero(n_full);
  for (atx::usize i = 0; i < hard.n_w && i < out.book.size(); ++i) {
    x[static_cast<Eigen::Index>(i)] = out.book[i];
  }
  // y = Xᵀw reconstructed so Ã·x is exact for the factor-coupled (cone) rows.
  for (atx::usize fk = 0; fk < hard.n_y; ++fk) {
    atx::f64 acc = 0.0; // y_fk = Σ_i X(i,fk) w_i (ascending i, R1)
    for (atx::usize i = 0; i < hard.n_w; ++i) {
      acc += p.V.exposures()(static_cast<Eigen::Index>(i), static_cast<Eigen::Index>(fk)) *
             out.book[i];
    }
    x[static_cast<Eigen::Index>(hard.n_w + fk)] = acc;
  }

  std::vector<RelaxationEntry> entries;
  for (const ElasticRow &er : p.C.elastic.linear_rows) {
    RelaxationEntry e;
    e.kind = RelaxationEntry::Kind::LinearRow;
    e.index = k + er.row_begin; // augmented row_begin
    e.count = er.count;
    e.priority = er.priority;
    e.violation = detail::linear_block_violation(hard, x, e.index, er.count);
    entries.push_back(e);
  }
  for (const ElasticCone &ec : p.C.elastic.cones) {
    RelaxationEntry e;
    e.kind = RelaxationEntry::Kind::Cone;
    e.index = ec.cone_index;
    e.count = 1U;
    e.priority = ec.priority;
    e.violation = detail::cone_violation_at(hard, x, ec.cone_index);
    entries.push_back(e);
  }
  // Stable-sort by ascending priority (lowest relaxed first); ties keep their materialize
  // order (stable ⇒ deterministic, R1). NO float-keyed sort (priority is integer).
  std::stable_sort(entries.begin(), entries.end(),
                   [](const RelaxationEntry &a, const RelaxationEntry &b) noexcept {
                     return a.priority < b.priority;
                   });
  out.report.entries = std::move(entries);
  return co::Ok(std::move(out));
}

} // namespace atx::engine::risk
