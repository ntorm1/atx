#pragma once

// atx::engine::risk — ConstraintSet: the portfolio-constraint ALGEBRA (S1-1).
//
// Composable constraint descriptors that materialize into the QP linear-
// inequality form  l <= A w <= u  over the M-dimensional weight vector w.
// `M` is the universe size, `K` the number of risk factors.
//
// ===========================================================================
//  DESIGN DECISION — A is an R×M LINEAR operator; L1 budgets are METADATA.
// ===========================================================================
//  The plan (§4.1) notes that "L1 rows use the auxiliary-variable split (§4.2)".
//  We resolve that here EXACTLY as follows: `A` carries ONLY the genuinely-
//  LINEAR rows — each row is a single linear functional of w, so the whole
//  constraint block is a clean R×M matrix and feasibility is just the trivially
//  unit-testable check  A·w ∈ [l, u]  componentwise.
//
//  The two L1-BUDGET constraints are NOT linear in w:
//      gross    Σ|w|        <= L      (the GrossNet.gross_leverage)
//      turnover Σ|w − w_prev| <= T    (the TurnoverBudget.max_turnover)
//  An absolute value is not a linear functional, so neither can be expressed as
//  a single A-row. They ride along as the MaterializedConstraints metadata
//  fields (gross_l1_budget / has_turnover / turnover_budget / turnover_ref); the
//  S1-2 ADMM owns their standard auxiliary-variable / projection split. This
//  keeps S1-1 decoupled from solver internals and keeps `A` a pure linear map.
//
// ===========================================================================
//  Determinism (R1)
// ===========================================================================
//  Row emission is ORDER-FIXED for byte-reproducibility:
//      (1) dollar-neutral Σw=0, (2) position box, (3) factor exposure,
//      (4) group, (5) beta.
//  materialize() is a PURE const function (it never mutates *this); given the
//  same inputs it returns a byte-identical (A, l, u) and metadata block.
//
// ===========================================================================
//  Validation (R3)
// ===========================================================================
//  Dimension mismatches and provably-contradictory LINEAR bounds (a negative
//  cap / bound / tol / leverage / turnover) return InvalidArgument. We do NOT
//  over-reject: an "infeasible" name_cap·M < gross_leverage is left for the
//  optimizer (cap-wins), not rejected here. A bad factor-column index returns
//  OutOfRange.
//
// ===========================================================================
//  Allocation
// ===========================================================================
//  Materialization is a COLD-PATH, rebalance-cadence operation; it allocates the
//  result (A, l, u, turnover_ref) once. There is no hot path here.

#include <optional> // std::optional descriptors
#include <span>     // std::span (group_id / beta / w_prev — weakest sufficient type)
#include <utility>  // std::move
#include <vector>   // descriptor payloads + result metadata

#include "atx/core/error.hpp"         // Result, Ok, Err, ErrorCode
#include "atx/core/linalg/linalg.hpp" // MatX, VecX
#include "atx/core/types.hpp"         // f64, usize

namespace atx::engine::risk {

// ===========================================================================
//  Descriptors — each is a small value type describing one constraint family.
// ===========================================================================

// Gross leverage (L1 budget Σ|w| <= gross_leverage) plus the optional linear
// dollar-neutral equality Σw = 0. The gross budget is metadata (non-linear); the
// dollar-neutral equality is a single linear A-row.
struct GrossNet {
  atx::f64 gross_leverage = 1.0; // Σ|w| <= this (L1 budget metadata)
  bool dollar_neutral = true;    // Σ w = 0 (a linear row when true)
};

// Per-name box: |w_i| <= name_cap for every i (M box rows).
struct PositionCap {
  atx::f64 name_cap = 1.0;
};

// Bounded factor exposure: |(Xᵀw)_k| <= bound[j], for k = factor_cols[j].
struct FactorExposure {
  std::vector<atx::usize> factor_cols; // factor column indices into X (k = factor_cols[j])
  std::vector<atx::f64> bound;         // per-selected-factor exposure bound
};

// Bounded group net: |Σ_{i ∈ g} w_i| <= cap[g], groups keyed by group_id[i].
struct GroupCap {
  std::span<const atx::usize> group_id; // length-M group label per instrument
  std::vector<atx::f64> cap;            // per-group cap (length = #groups)
};

// Beta neutrality: |βᵀw| <= tol (a single linear row).
struct BetaNeutral {
  std::span<const atx::f64> beta; // length-M beta vector
  atx::f64 tol = 0.0;
};

// Turnover budget (L1 budget Σ|w − w_prev| <= max_turnover); metadata-only.
struct TurnoverBudget {
  atx::f64 max_turnover = 0.0;
};

// ===========================================================================
//  MaterializedConstraints — the QP block (A, l, u) + L1-budget metadata.
// ===========================================================================
struct MaterializedConstraints {
  atx::core::linalg::MatX A; // (R × M) LINEAR inequality rows only
  atx::core::linalg::VecX l; // R lower bounds
  atx::core::linalg::VecX u; // R upper bounds

  // L1-BUDGET metadata (NOT linear in w — the S1-2 ADMM owns the auxiliary-
  // variable split; see the DESIGN DECISION header block above).
  atx::f64 gross_l1_budget = -1.0;     // Σ|w| <= this; < 0 ⇒ no gross-L1 cap
  bool has_turnover = false;           // a TurnoverBudget was set
  atx::f64 turnover_budget = 0.0;      // Σ|w − w_prev| <= this (valid iff has_turnover)
  std::vector<atx::f64> turnover_ref;  // w_prev snapshot keying the turnover L1 (valid iff has_turnover)
};

// ===========================================================================
//  ConstraintSet — the composable descriptor bundle + materialize().
// ===========================================================================
struct ConstraintSet {
  GrossNet gross{};
  std::optional<PositionCap> pos;
  std::optional<FactorExposure> fexp;
  std::optional<GroupCap> grp;
  std::optional<BetaNeutral> beta;
  std::optional<TurnoverBudget> turn;

  // Materialize l <= A w <= u over the M-dim weight space. `X` is the M×K
  // exposure matrix (FactorComponents.X); `w_prev` keys the turnover L1 (an
  // EMPTY span ⇒ a flat all-zero reference). ORDER-FIXED row emission (R1):
  // (1) dollar-neutral, (2) position box, (3) factor exposure, (4) group,
  // (5) beta. Σ|w|<=L and turnover go into the L1-budget metadata. PURE const.
  [[nodiscard]] atx::core::Result<MaterializedConstraints>
  materialize(const atx::core::linalg::MatX &X, std::span<const atx::f64> w_prev,
              atx::usize M) const {
    namespace co = atx::core;
    // --- validate every descriptor up front (R3); fail before allocating ----
    ATX_TRY_VOID(validate(X, M));

    // --- count linear rows in the FIXED emission order ----------------------
    const atx::usize rows = linear_row_count(M);
    const auto er = static_cast<Eigen::Index>(rows);
    const auto em = static_cast<Eigen::Index>(M);

    MaterializedConstraints mc;
    mc.A = co::linalg::MatX::Zero(er, em);
    mc.l = co::linalg::VecX::Zero(er);
    mc.u = co::linalg::VecX::Zero(er);

    // --- emit rows in the fixed order; `next` is the running row cursor ------
    Eigen::Index next = 0;
    emit_dollar_neutral(mc, M, next);
    emit_position_box(mc, M, next);
    emit_factor_exposure(mc, X, M, next);
    emit_group(mc, M, next);
    emit_beta(mc, M, next);

    // --- L1-budget metadata (non-linear; carried for the S1-2 ADMM) ---------
    mc.gross_l1_budget = gross.gross_leverage;
    fill_turnover(mc, w_prev, M);
    return co::Ok(std::move(mc));
  }

private:
  // -------------------------------------------------------------------------
  //  Validation — provably-contradictory inputs only; do not over-reject.
  // -------------------------------------------------------------------------
  [[nodiscard]] atx::core::Status validate(const atx::core::linalg::MatX &X,
                                           atx::usize M) const {
    namespace co = atx::core;
    if (gross.gross_leverage < 0.0) {
      return co::Err(co::ErrorCode::InvalidArgument,
                     "ConstraintSet: gross_leverage must be >= 0");
    }
    ATX_TRY_VOID(validate_pos());
    ATX_TRY_VOID(validate_fexp(X, M));
    ATX_TRY_VOID(validate_grp(M));
    ATX_TRY_VOID(validate_beta(M));
    ATX_TRY_VOID(validate_turn());
    return co::Ok();
  }

  [[nodiscard]] atx::core::Status validate_pos() const {
    namespace co = atx::core;
    if (pos && pos->name_cap < 0.0) {
      return co::Err(co::ErrorCode::InvalidArgument, "PositionCap: name_cap must be >= 0");
    }
    return co::Ok();
  }

  [[nodiscard]] atx::core::Status validate_fexp(const atx::core::linalg::MatX &X,
                                                atx::usize M) const {
    namespace co = atx::core;
    if (!fexp) {
      return co::Ok();
    }
    if (static_cast<atx::usize>(X.rows()) != M) {
      return co::Err(co::ErrorCode::InvalidArgument,
                     "FactorExposure: X.rows() must equal M");
    }
    if (fexp->factor_cols.size() != fexp->bound.size()) {
      return co::Err(co::ErrorCode::InvalidArgument,
                     "FactorExposure: factor_cols and bound must be the same length");
    }
    const auto k = static_cast<atx::usize>(X.cols());
    for (atx::usize j = 0; j < fexp->factor_cols.size(); ++j) {
      if (fexp->factor_cols[j] >= k) {
        return co::Err(co::ErrorCode::OutOfRange,
                       "FactorExposure: factor column index out of range");
      }
      if (fexp->bound[j] < 0.0) {
        return co::Err(co::ErrorCode::InvalidArgument,
                       "FactorExposure: bound must be >= 0");
      }
    }
    return co::Ok();
  }

  [[nodiscard]] atx::core::Status validate_grp(atx::usize M) const {
    namespace co = atx::core;
    if (!grp) {
      return co::Ok();
    }
    if (grp->group_id.size() != M) {
      return co::Err(co::ErrorCode::InvalidArgument,
                     "GroupCap: group_id length must equal M");
    }
    if (grp->cap.size() != group_count()) {
      return co::Err(co::ErrorCode::InvalidArgument,
                     "GroupCap: cap length must equal the number of groups");
    }
    for (const atx::f64 c : grp->cap) {
      if (c < 0.0) {
        return co::Err(co::ErrorCode::InvalidArgument, "GroupCap: cap must be >= 0");
      }
    }
    return co::Ok();
  }

  [[nodiscard]] atx::core::Status validate_beta(atx::usize M) const {
    namespace co = atx::core;
    if (!beta) {
      return co::Ok();
    }
    if (beta->beta.size() != M) {
      return co::Err(co::ErrorCode::InvalidArgument, "BetaNeutral: beta length must equal M");
    }
    if (beta->tol < 0.0) {
      return co::Err(co::ErrorCode::InvalidArgument, "BetaNeutral: tol must be >= 0");
    }
    return co::Ok();
  }

  [[nodiscard]] atx::core::Status validate_turn() const {
    namespace co = atx::core;
    if (turn && turn->max_turnover < 0.0) {
      return co::Err(co::ErrorCode::InvalidArgument,
                     "TurnoverBudget: max_turnover must be >= 0");
    }
    return co::Ok();
  }

  // Number of groups = (max group_id) + 1; 0 for an empty group_id. Called only
  // after validate_grp confirms grp has a value when used; safe standalone too.
  [[nodiscard]] atx::usize group_count() const noexcept {
    if (!grp || grp->group_id.empty()) {
      return 0;
    }
    atx::usize mx = 0;
    for (const atx::usize g : grp->group_id) {
      if (g > mx) {
        mx = g;
      }
    }
    return mx + 1;
  }

  // -------------------------------------------------------------------------
  //  Row counting — must mirror the emission order/cardinality exactly.
  // -------------------------------------------------------------------------
  [[nodiscard]] atx::usize linear_row_count(atx::usize M) const noexcept {
    atx::usize r = 0;
    r += gross.dollar_neutral ? 1U : 0U;       // (1) Σw = 0
    r += pos ? M : 0U;                         // (2) M box rows
    r += fexp ? fexp->factor_cols.size() : 0U; // (3) one row per selected factor
    r += grp ? group_count() : 0U;             // (4) one row per group
    r += beta ? 1U : 0U;                       // (5) βᵀw row
    return r;
  }

  // -------------------------------------------------------------------------
  //  Row emitters — each advances the `next` cursor by the rows it writes.
  // -------------------------------------------------------------------------

  // (1) dollar-neutral: a single all-ones row with l = u = 0 (Σw = 0).
  // noexcept: only bounded index-writes into the pre-sized mc.A/l/u (no alloc).
  void emit_dollar_neutral(MaterializedConstraints &mc, atx::usize M,
                           Eigen::Index &next) const noexcept {
    if (!gross.dollar_neutral) {
      return;
    }
    for (atx::usize i = 0; i < M; ++i) {
      mc.A(next, static_cast<Eigen::Index>(i)) = 1.0;
    }
    mc.l[next] = 0.0;
    mc.u[next] = 0.0;
    ++next;
  }

  // (2) position box: M rows, row i = e_i, bounds [-cap, +cap] (|w_i| <= cap).
  void emit_position_box(MaterializedConstraints &mc, atx::usize M,
                         Eigen::Index &next) const noexcept {
    if (!pos) {
      return;
    }
    const atx::f64 cap = pos->name_cap;
    for (atx::usize i = 0; i < M; ++i) {
      mc.A(next, static_cast<Eigen::Index>(i)) = 1.0;
      mc.l[next] = -cap;
      mc.u[next] = cap;
      ++next;
    }
  }

  // (3) factor exposure: one row per selected factor, entries X(i, col) across i;
  // bounds [-bound[j], +bound[j]] for |(Xᵀw)_k| <= bound[j].
  void emit_factor_exposure(MaterializedConstraints &mc, const atx::core::linalg::MatX &X,
                            atx::usize M, Eigen::Index &next) const noexcept {
    if (!fexp) {
      return;
    }
    for (atx::usize j = 0; j < fexp->factor_cols.size(); ++j) {
      const auto col = static_cast<Eigen::Index>(fexp->factor_cols[j]);
      for (atx::usize i = 0; i < M; ++i) {
        mc.A(next, static_cast<Eigen::Index>(i)) = X(static_cast<Eigen::Index>(i), col);
      }
      mc.l[next] = -fexp->bound[j];
      mc.u[next] = fexp->bound[j];
      ++next;
    }
  }

  // (4) group: one row per group g with 1.0 at every i where group_id[i] == g;
  // bounds [-cap[g], +cap[g]] for |Σ_{i ∈ g} w_i| <= cap[g].
  void emit_group(MaterializedConstraints &mc, atx::usize M, Eigen::Index &next) const noexcept {
    if (!grp) {
      return;
    }
    const atx::usize g_count = group_count();
    for (atx::usize g = 0; g < g_count; ++g) {
      for (atx::usize i = 0; i < M; ++i) {
        if (grp->group_id[i] == g) {
          mc.A(next, static_cast<Eigen::Index>(i)) = 1.0;
        }
      }
      mc.l[next] = -grp->cap[g];
      mc.u[next] = grp->cap[g];
      ++next;
    }
  }

  // (5) beta: a single row = the beta vector; bounds [-tol, +tol] (|βᵀw| <= tol).
  void emit_beta(MaterializedConstraints &mc, atx::usize M, Eigen::Index &next) const noexcept {
    if (!beta) {
      return;
    }
    for (atx::usize i = 0; i < M; ++i) {
      mc.A(next, static_cast<Eigen::Index>(i)) = beta->beta[i];
    }
    mc.l[next] = -beta->tol;
    mc.u[next] = beta->tol;
    ++next;
  }

  // -------------------------------------------------------------------------
  //  Turnover metadata — snapshot w_prev (empty ⇒ flat zero ref of length M).
  // -------------------------------------------------------------------------
  // NOT noexcept: the turnover_ref.assign(M, …) allocates the reference vector
  // (cold path, once), so this is deliberately left potentially-throwing unlike
  // the pure index-write emit_* leaves above.
  void fill_turnover(MaterializedConstraints &mc, std::span<const atx::f64> w_prev,
                     atx::usize M) const {
    if (!turn) {
      mc.has_turnover = false;
      return;
    }
    mc.has_turnover = true;
    mc.turnover_budget = turn->max_turnover;
    mc.turnover_ref.assign(M, 0.0);
    for (atx::usize i = 0; i < M && i < w_prev.size(); ++i) {
      mc.turnover_ref[i] = w_prev[i];
    }
  }
};

} // namespace atx::engine::risk
