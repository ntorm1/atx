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

#include <algorithm> // std::min (per-name box fold)
#include <optional>  // std::optional descriptors
#include <span>      // std::span (group_id / beta / w_prev — weakest sufficient type)
#include <utility>   // std::move
#include <vector>    // descriptor payloads + result metadata

#include "atx/core/error.hpp"         // Result, Ok, Err, ErrorCode
#include "atx/core/linalg/linalg.hpp" // MatX, VecX
#include "atx/core/types.hpp"         // f64, usize

#include "atx/engine/risk/cone.hpp"           // TrackingError descriptor + TrackingErrorSpec (S8.5a)
#include "atx/engine/risk/reference_data.hpp" // CapacityRef (%ADV / %shares box inputs, S8.4)

namespace atx::engine::risk {

// ===========================================================================
//  Descriptors — each is a small value type describing one constraint family.
//
//  Per-descriptor PRIORITY / ELASTIC metadata (S8.4): every descriptor carries an
//  integer `priority` (lower = relaxed first) and an `elastic` flag. S8.4 ADDS these
//  fields ONLY — it does NOT act on them (the constraint surface is enforced EXACTLY
//  here, R3). The S8.6 constraint-hierarchy / minimize-violation layer consumes them
//  to relax the lowest-priority elastic rows when a set is infeasible. They are
//  trailing aggregate members with defaults, so existing brace-initializations are
//  unaffected. (priority/elastic do NOT enter materialize's (A, l, u) — a relaxed
//  row is an S8.6 concern, not part of the exact materialized surface.)
// ===========================================================================

// Gross leverage (L1 budget Σ|w| <= gross_leverage) plus the optional linear
// dollar-neutral equality Σw = 0. The gross budget is metadata (non-linear); the
// dollar-neutral equality is a single linear A-row.
struct GrossNet {
  atx::f64 gross_leverage = 1.0; // Σ|w| <= this (L1 budget metadata)
  bool dollar_neutral = true;    // Σ w = 0 (a linear row when true)
  atx::usize priority = 0;       // S8.6 relaxation rank (lower = relaxed first)
  bool elastic = false;          // S8.6 may relax this row when infeasible
};

// Per-name box: |w_i| <= name_cap for every i (M box rows).
struct PositionCap {
  atx::f64 name_cap = 1.0;
  atx::usize priority = 0;
  bool elastic = false;
};

// Bounded factor exposure: |(Xᵀw)_k| <= bound[j], for k = factor_cols[j].
struct FactorExposure {
  std::vector<atx::usize> factor_cols; // factor column indices into X (k = factor_cols[j])
  std::vector<atx::f64> bound;         // per-selected-factor exposure bound
  atx::usize priority = 0;
  bool elastic = false;
};

// Bounded group net: |Σ_{i ∈ g} w_i| <= cap[g], groups keyed by group_id[i].
struct GroupCap {
  std::span<const atx::usize> group_id; // length-M group label per instrument
  std::vector<atx::f64> cap;            // per-group cap (length = #groups)
  atx::usize priority = 0;
  bool elastic = false;
};

// Beta neutrality: |βᵀw| <= tol (a single linear row).
struct BetaNeutral {
  std::span<const atx::f64> beta; // length-M beta vector
  atx::f64 tol = 0.0;
  atx::usize priority = 0;
  bool elastic = false;
};

// Turnover budget (L1 budget Σ|w − w_prev| <= max_turnover); metadata-only.
struct TurnoverBudget {
  atx::f64 max_turnover = 0.0;
  atx::usize priority = 0;
  bool elastic = false;
};

// Participation cap (%ADV, S8.4): |w_i| <= ρ·H·ADV_i·price_i/NAV, ρ = adv_frac, H =
// CapacityRef.horizon_days. A POSITION-BOX descriptor — it folds (elementwise min,
// per name) into the diagonal box rather than adding a separate row (it IS a bound on
// |w_i|, the cheapest constraint in the QP — R4). Needs CapacityRef.adv / price / nav;
// an empty adv/price span ⇒ the cap is not evaluable and does not bind (the fold
// leaves the other caps to govern).
struct ParticipationCap {
  atx::f64 adv_frac = 1.0; // ρ — allowed fraction of (H-day) dollar ADV
  atx::usize priority = 0;
  bool elastic = false;
};

// Ownership cap (%shares-outstanding, S8.4): |w_i| <= κ·shares_out_i·price_i/NAV,
// κ = shares_frac. Like ParticipationCap, a POSITION-BOX descriptor that folds
// (elementwise min) into the diagonal box. Needs CapacityRef.shares_out / price / nav.
struct OwnershipCap {
  atx::f64 shares_frac = 1.0; // κ — allowed fraction of the name's dollar float
  atx::usize priority = 0;
  bool elastic = false;
};

// Robust alpha uncertainty (S8.5c) — the Goldfarb-Iyengar worst-case-alpha cone. Models
// the true alpha as uncertain within a FACTOR-STRUCTURED ellipsoid; the worst case over
// that set adds a convex +κ‖Ω_f^{1/2}(Xᵀw)‖₂ penalty to the (minimize) objective. It is
// NOT a linear A-row — it materializes into a RobustAlphaSpec and build_augmented adds the
// epigraph aux variable t, the variable-apex SOC ‖Ω_f^{1/2} y‖₂ ≤ t, and the linear cost
// +κt (needs L_F-style Cholesky of Ω_f, only reachable there, R4). `omega_f` is OPTIONAL
// (empty ⇒ identity ⇒ the penalty is κ‖Xᵀw‖₂). κ ≤ 0 ⇒ NO cone (reduces to nominal, R10).
struct RobustAlpha {
  atx::f64 kappa = 0.0;                       // uncertainty radius κ ≥ 0 (κ ≤ 0 ⇒ inert / nominal)
  atx::core::linalg::MatX omega_f = {};       // K×K SPD factor-premia error cov (empty ⇒ identity)
  atx::usize priority = 0;                    // S8.6 relaxation rank (lower = relaxed first)
  bool elastic = false;                       // S8.6 may relax this cone when infeasible
};

// Sector risk budget — sectors keyed by sector_id[i]. TWO variants:
//   * NET-WEIGHT (S8.4, DEFAULT soc=false): |Σ_{i ∈ g} w_i| <= cap[g], a sparse 0/1
//     group row structurally identical to GroupCap; emitted as a linear A-row AFTER
//     the GroupCap rows in the fixed order (R1).
//   * SOC (S8.5b, soc=true): ‖V^{1/2}(mask_g∘w)‖₂ <= sigma[g], a second-order cone (a
//     ball with NO benchmark / offset 0) over the masked holdings of sector g. Like the
//     tracking-error cone it is NOT a linear A-row — it materializes into a SectorRiskSpec
//     and build_augmented assembles the K+M cone rows (needs L_F = chol(F), sqrt(D) — only
//     reachable there, R4). The SOC variant emits NO net-weight A-rows.
// The two variants are mutually exclusive per descriptor (the `soc` flag selects one).
struct SectorRiskBudget {
  std::span<const atx::usize> sector_id; // length-M sector label per instrument
  std::vector<atx::f64> cap;             // per-sector net-weight cap (length = #sectors; net-weight variant)
  bool soc = false;                      // true ⇒ the S8.5b SOC variant (uses `sigma`, not `cap`)
  std::vector<atx::f64> sigma = {};      // per-sector ‖V^{1/2}(mask_g∘w)‖₂ budget (length = #sectors; SOC variant)
  atx::usize priority = 0;
  bool elastic = false;
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

  // CONE metadata (S8.5a — NOT linear in w; the ADMM owns the SOC z-projection). The
  // tracking-error SOC needs L_F = chol(F) and sqrt(D) from the FactorModel to form its
  // K+M cone rows + offset, which only build_augmented can reach — so materialize()
  // carries the RAW request (a w_bench snapshot + the te budget) and build_augmented
  // assembles the SocBlock. `tracking.active == false` ⇒ no cone (the box-only path).
  TrackingErrorSpec tracking;          // tracking-error SOC request (active iff a TrackingError was set)

  // SECTOR-RISK SOC metadata (S8.5b — NOT linear in w; the ADMM owns the SOC z-projection,
  // exactly like tracking). The SOC variant of SectorRiskBudget materializes its sector_id
  // snapshot + per-sector σ here; build_augmented assembles one K+M cone block per sector
  // with a finite σ_g (it needs L_F = chol(F) / sqrt(D)). `sector_risk.active == false` ⇒
  // no sector cones (the net-weight variant, or no SectorRiskBudget, stays the box path).
  SectorRiskSpec sector_risk;          // sector-risk SOC request (active iff a soc=true SectorRiskBudget was set)

  // √-IMPACT COST SURROGATE metadata (S8.5b, R9). PRE-COMPUTED per-name quadratic
  // coefficients c_i ≥ 0 (from the ONE cost surface, exec::ImpactCfg) that build_augmented
  // folds into the Hessian P w-diagonal (P[i,i] += 2 c_i) and the linear term
  // (q[i] += −2 c_i·turnover_ref_i). materialize() does NOT compute these — the caller
  // populates `impact.coeff` from impact_cost_bps's coefficients (no impact constants live
  // in the optimizer). EMPTY / all-zero ⇒ INERT (P, q byte-identical to pre-S8.5b, R10).
  ImpactSurrogateSpec impact;          // √-impact quadratic surrogate (active iff coeff is populated)

  // ROBUST ALPHA metadata (S8.5c — NOT linear in w; the ADMM owns the variable-apex SOC
  // z-projection). A RobustAlpha descriptor materializes its κ + optional Ω_f here;
  // build_augmented — when κ > 0 — adds the epigraph aux var t, the variable-apex SOC
  // ‖Ω_f^{1/2} y‖₂ ≤ t, and the +κt cost (it needs the Cholesky of Ω_f / the y-block).
  // `robust.active == false` (or κ ≤ 0) ⇒ no robust cone (byte-identical to S8.5b, R10).
  RobustAlphaSpec robust;              // robust alpha-uncertainty cone (active iff a RobustAlpha with κ>0 was set)

  // ELASTICITY metadata (S8.6 — NOT part of the exact surface; CONSUMED only by the
  // constraint-hierarchy / minimize-violation layer, risk/elasticity.hpp). Records, per
  // ELASTIC descriptor, the materialized rows / cone it owns + its priority (lower =
  // relaxed first). EMPTY when no descriptor set `elastic=true` ⇒ elasticity is a pure
  // no-op (the relaxed builder emits zero slack columns ⇒ byte-identical to S8.5, R10).
  // See cone.hpp::ElasticSpec for the two carriers + the augmented-frame mapping.
  ElasticSpec elastic;                 // S8.6 relaxation metadata (empty ⇒ nothing relaxes)
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
  std::optional<ParticipationCap> part;   // %ADV box fold (S8.4)
  std::optional<OwnershipCap> own;         // %shares-out box fold (S8.4)
  std::optional<SectorRiskBudget> sector;  // sector net-weight rows (S8.4)
  std::optional<TrackingError> track;      // tracking-error SOC (S8.5a; cone, not a linear row)
  std::optional<RobustAlpha> robust;       // robust alpha-uncertainty SOC (S8.5c; cone, not a linear row)

  // Materialize l <= A w <= u over the M-dim weight space. `X` is the M×K
  // exposure matrix (FactorComponents.X); `w_prev` keys the turnover L1 (an
  // EMPTY span ⇒ a flat all-zero reference); `ref` carries the per-name %ADV /
  // %shares reference panel (S8.4 — empty/default ⇒ participation/ownership do not
  // bind). ORDER-FIXED row emission (R1):
  // (1) dollar-neutral, (2) position box (with the %ADV/%shares fold), (3) factor
  // exposure, (4) group, (5) beta, (6) sector net-weight. Σ|w|<=L and turnover go
  // into the L1-budget metadata. PURE const.
  [[nodiscard]] atx::core::Result<MaterializedConstraints>
  materialize(const atx::core::linalg::MatX &X, std::span<const atx::f64> w_prev, atx::usize M,
              const CapacityRef &ref = {}) const {
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
    emit_position_box(mc, M, ref, next);
    emit_factor_exposure(mc, X, M, next);
    emit_group(mc, M, next);
    emit_beta(mc, M, next);
    emit_sector(mc, M, next);

    // --- L1-budget metadata (non-linear; carried for the S1-2 ADMM) ---------
    mc.gross_l1_budget = gross.gross_leverage;
    fill_turnover(mc, w_prev, M);
    // --- cone metadata (S8.5a tracking-error SOC; assembled in build_augmented) ---
    fill_tracking(mc, M);
    // --- sector-risk SOC metadata (S8.5b; assembled in build_augmented) ---
    fill_sector_soc(mc, M);
    // --- robust alpha-uncertainty SOC metadata (S8.5c; assembled in build_augmented) ---
    fill_robust(mc);
    // --- S8.6 elasticity metadata (CONSUMED only by risk/elasticity.hpp on infeasibility) ---
    fill_elastic(mc, M);
    return co::Ok(std::move(mc));
  }

private:
  // The "no active cap" sentinel for the per-name box min-fold: a finite, very large
  // bound (far above any realistic |w_i|) so a present-but-unevaluable participation /
  // ownership cap never spuriously zeroes the box. Mirrors qp_augment.hpp's kAugInf
  // ethos — finite so the band stays well-ordered through scaling/clamp (no inf math).
  static constexpr atx::f64 kUnboundedBox = 1e30;

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
    ATX_TRY_VOID(validate_capacity_caps());
    ATX_TRY_VOID(validate_sector(M));
    ATX_TRY_VOID(validate_track(M));
    ATX_TRY_VOID(validate_robust(X));
    return co::Ok();
  }

  // Robust alpha (S8.5c): κ must be >= 0 (a negative uncertainty radius is provably
  // contradictory), and a SUPPLIED Ω_f must be square K×K with K == X.cols() (an empty
  // Ω_f is accepted as the identity). SPD-ness of Ω_f is re-checked at assembly (its
  // Cholesky, ATX_ASSERT-guarded) exactly as the tracking/sector cones re-check F.
  [[nodiscard]] atx::core::Status validate_robust(const atx::core::linalg::MatX &X) const {
    namespace co = atx::core;
    if (!robust) {
      return co::Ok();
    }
    if (robust->kappa < 0.0) {
      return co::Err(co::ErrorCode::InvalidArgument, "RobustAlpha: kappa must be >= 0");
    }
    const auto &om = robust->omega_f;
    if (om.rows() != 0 || om.cols() != 0) { // a supplied (non-empty) Ω_f must be K×K
      if (om.rows() != om.cols() || om.rows() != X.cols()) {
        return co::Err(co::ErrorCode::InvalidArgument,
                       "RobustAlpha: Omega_f must be K×K with K == X.cols() (or empty for identity)");
      }
    }
    return co::Ok();
  }

  // Tracking-error SOC (S8.5a): a present w_bench must be length M (an EMPTY w_bench is
  // accepted as the all-zero benchmark) and te_budget must be >= 0 (a negative ball
  // radius is provably contradictory). The cone itself is assembled in build_augmented.
  [[nodiscard]] atx::core::Status validate_track(atx::usize M) const {
    namespace co = atx::core;
    if (!track) {
      return co::Ok();
    }
    if (!track->w_bench.empty() && track->w_bench.size() != M) {
      return co::Err(co::ErrorCode::InvalidArgument,
                     "TrackingError: w_bench length must equal M (or be empty for a zero benchmark)");
    }
    if (track->te_budget < 0.0) {
      return co::Err(co::ErrorCode::InvalidArgument, "TrackingError: te_budget must be >= 0");
    }
    return co::Ok();
  }

  [[nodiscard]] atx::core::Status validate_pos() const {
    namespace co = atx::core;
    if (pos && pos->name_cap < 0.0) {
      return co::Err(co::ErrorCode::InvalidArgument, "PositionCap: name_cap must be >= 0");
    }
    return co::Ok();
  }

  // %ADV / %shares fractions must be >= 0 (a negative cap is provably contradictory).
  [[nodiscard]] atx::core::Status validate_capacity_caps() const {
    namespace co = atx::core;
    if (part && part->adv_frac < 0.0) {
      return co::Err(co::ErrorCode::InvalidArgument, "ParticipationCap: adv_frac must be >= 0");
    }
    if (own && own->shares_frac < 0.0) {
      return co::Err(co::ErrorCode::InvalidArgument, "OwnershipCap: shares_frac must be >= 0");
    }
    return co::Ok();
  }

  // Sector budget validation. sector_id length == M always. The active variant's
  // per-sector budget vector (net-weight `cap`, or SOC `sigma`) must have length
  // #sectors and be >= 0. (Mirrors validate_grp for the net-weight variant; the SOC
  // variant adds the same shape/sign checks on `sigma`.)
  [[nodiscard]] atx::core::Status validate_sector(atx::usize M) const {
    namespace co = atx::core;
    if (!sector) {
      return co::Ok();
    }
    if (sector->sector_id.size() != M) {
      return co::Err(co::ErrorCode::InvalidArgument,
                     "SectorRiskBudget: sector_id length must equal M");
    }
    if (sector->soc) {
      if (sector->sigma.size() != sector_count()) {
        return co::Err(co::ErrorCode::InvalidArgument,
                       "SectorRiskBudget(SOC): sigma length must equal the number of sectors");
      }
      for (const atx::f64 s : sector->sigma) {
        if (s < 0.0) {
          return co::Err(co::ErrorCode::InvalidArgument, "SectorRiskBudget(SOC): sigma must be >= 0");
        }
      }
      return co::Ok();
    }
    if (sector->cap.size() != sector_count()) {
      return co::Err(co::ErrorCode::InvalidArgument,
                     "SectorRiskBudget: cap length must equal the number of sectors");
    }
    for (const atx::f64 c : sector->cap) {
      if (c < 0.0) {
        return co::Err(co::ErrorCode::InvalidArgument, "SectorRiskBudget: cap must be >= 0");
      }
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

  // Number of sectors = (max sector_id) + 1; 0 for an empty sector_id (mirrors
  // group_count for the net-weight SectorRiskBudget variant).
  [[nodiscard]] atx::usize sector_count() const noexcept {
    if (!sector || sector->sector_id.empty()) {
      return 0;
    }
    atx::usize mx = 0;
    for (const atx::usize g : sector->sector_id) {
      if (g > mx) {
        mx = g;
      }
    }
    return mx + 1;
  }

  // The position box emits M diagonal rows whenever ANY box-fold descriptor is
  // present: PositionCap (name_cap) and/or ParticipationCap / OwnershipCap (which fold
  // into the box, §0.3). The per-name bound is the elementwise min of whichever caps
  // are active for that name.
  [[nodiscard]] bool has_box() const noexcept {
    return pos.has_value() || part.has_value() || own.has_value();
  }

  // -------------------------------------------------------------------------
  //  Row counting — must mirror the emission order/cardinality exactly.
  // -------------------------------------------------------------------------
  [[nodiscard]] atx::usize linear_row_count(atx::usize M) const noexcept {
    atx::usize r = 0;
    r += gross.dollar_neutral ? 1U : 0U;       // (1) Σw = 0
    r += has_box() ? M : 0U;                   // (2) M box rows (PositionCap / %ADV / %shares)
    r += fexp ? fexp->factor_cols.size() : 0U; // (3) one row per selected factor
    r += grp ? group_count() : 0U;             // (4) one row per group
    r += beta ? 1U : 0U;                       // (5) βᵀw row
    // (6) net-weight sector rows ONLY for the net-weight variant; the SOC variant emits
    //     NO linear A-rows (its cone rows are assembled in build_augmented).
    r += (sector && !sector->soc) ? sector_count() : 0U;
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

  // (2) position box: M rows, row i = e_i, bounds [-cap_i, +cap_i] (|w_i| <= cap_i).
  // The per-name cap_i is the elementwise MIN of whichever box-fold descriptors are
  // active (§0.3): the PositionCap name_cap, the %ADV participation bound
  // ρ·H·ADV_i·price_i/NAV, and the %shares ownership bound κ·shares_out_i·price_i/NAV.
  // A descriptor whose CapacityRef inputs are absent (empty span) or whose NAV<=0 is
  // NOT evaluable for that name and is simply skipped (it does not bind). When ONLY
  // PositionCap is set this reduces EXACTLY to the as-built uniform box (the pin).
  void emit_position_box(MaterializedConstraints &mc, atx::usize M, const CapacityRef &ref,
                         Eigen::Index &next) const noexcept {
    if (!has_box()) {
      return;
    }
    for (atx::usize i = 0; i < M; ++i) {
      const atx::f64 cap = name_box_cap(i, ref);
      mc.A(next, static_cast<Eigen::Index>(i)) = 1.0;
      mc.l[next] = -cap;
      mc.u[next] = cap;
      ++next;
    }
  }

  // The per-name box cap = min over the active box-fold descriptors. A descriptor that
  // is not present, or whose reference inputs are unavailable, contributes no bound.
  // kUnbounded stands in when NO descriptor binds name i (a finite large sentinel so a
  // present-but-unevaluable cap never spuriously zeroes the box). Order: name_cap,
  // then participation (needs adv/price/nav), then ownership (needs shares_out/price/nav).
  [[nodiscard]] atx::f64 name_box_cap(atx::usize i, const CapacityRef &ref) const noexcept {
    atx::f64 cap = kUnboundedBox;
    if (pos) {
      cap = std::min(cap, pos->name_cap);
    }
    if (part && ref.nav > 0.0 && i < ref.adv.size() && i < ref.price.size()) {
      cap = std::min(cap, part->adv_frac * ref.horizon_days * ref.adv[i] * ref.price[i] / ref.nav);
    }
    if (own && ref.nav > 0.0 && i < ref.shares_out.size() && i < ref.price.size()) {
      cap = std::min(cap, own->shares_frac * ref.shares_out[i] * ref.price[i] / ref.nav);
    }
    return cap;
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

  // (6) sector net-weight: one row per sector g with 1.0 at every i where
  // sector_id[i] == g; bounds [-cap[g], +cap[g]] for |Σ_{i ∈ g} w_i| <= cap[g]. The
  // net-weight SectorRiskBudget variant (the SOC variant is S8.5) — structurally a
  // GroupCap, emitted LAST so the offsets stay reproducible (R1).
  void emit_sector(MaterializedConstraints &mc, atx::usize M, Eigen::Index &next) const noexcept {
    if (!sector || sector->soc) {
      return; // no descriptor, or the SOC variant (no net-weight A-rows — cone, not linear)
    }
    const atx::usize s_count = sector_count();
    for (atx::usize g = 0; g < s_count; ++g) {
      for (atx::usize i = 0; i < M; ++i) {
        if (sector->sector_id[i] == g) {
          mc.A(next, static_cast<Eigen::Index>(i)) = 1.0;
        }
      }
      mc.l[next] = -sector->cap[g];
      mc.u[next] = sector->cap[g];
      ++next;
    }
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

  // -------------------------------------------------------------------------
  //  Tracking-error cone metadata — snapshot w_bench (empty ⇒ flat zero ref of
  //  length M) into the TrackingErrorSpec. The K+M SOC rows + the offset b are
  //  assembled later in build_augmented (they need L_F = chol(F) and sqrt(D) from
  //  the FactorModel); materialize only carries the RAW request (R4 — no V here).
  // -------------------------------------------------------------------------
  // NOT noexcept: the w_bench.assign(M, …) allocates the snapshot (cold path, once).
  void fill_tracking(MaterializedConstraints &mc, atx::usize M) const {
    if (!track) {
      mc.tracking.active = false;
      return;
    }
    mc.tracking.active = true;
    mc.tracking.te_budget = track->te_budget;
    mc.tracking.w_bench.assign(M, 0.0);
    for (atx::usize i = 0; i < M && i < track->w_bench.size(); ++i) {
      mc.tracking.w_bench[i] = track->w_bench[i];
    }
  }

  // -------------------------------------------------------------------------
  //  Sector-risk SOC metadata (S8.5b) — snapshot the SOC variant's sector_id labels
  //  + per-sector σ into the SectorRiskSpec. The K+M cone rows per sector are assembled
  //  later in build_augmented (they need L_F = chol(F) / sqrt(D) from the FactorModel;
  //  R4 — no V here). Inactive for the net-weight variant or no descriptor.
  // -------------------------------------------------------------------------
  // NOT noexcept: the vector copies allocate the snapshots (cold path, once).
  void fill_sector_soc(MaterializedConstraints &mc, atx::usize M) const {
    if (!sector || !sector->soc) {
      mc.sector_risk.active = false;
      return;
    }
    mc.sector_risk.active = true;
    mc.sector_risk.sector_id.assign(M, 0U);
    for (atx::usize i = 0; i < M && i < sector->sector_id.size(); ++i) {
      mc.sector_risk.sector_id[i] = sector->sector_id[i];
    }
    mc.sector_risk.sigma = sector->sigma; // per-sector budget (length == #sectors, validated)
  }

  // -------------------------------------------------------------------------
  //  Robust alpha-uncertainty metadata (S8.5c) — snapshot κ + the optional Ω_f into the
  //  RobustAlphaSpec. The epigraph aux var t, the variable-apex SOC rows (Ω_f^{1/2} y),
  //  and the +κt cost are assembled later in build_augmented (it needs the Cholesky of
  //  Ω_f and the y-block; R4 — no V here). Active ONLY when a RobustAlpha with κ > 0 is
  //  set (κ ≤ 0 ⇒ inert ⇒ build_augmented emits no cone ⇒ byte-identical to S8.5b, R10).
  // -------------------------------------------------------------------------
  // NOT noexcept: the Ω_f matrix copy allocates the snapshot (cold path, once).
  void fill_robust(MaterializedConstraints &mc) const {
    if (!robust || !(robust->kappa > 0.0)) {
      mc.robust.active = false; // no descriptor, or κ ≤ 0 ⇒ nominal (no robust cone)
      return;
    }
    mc.robust.active = true;
    mc.robust.kappa = robust->kappa;
    mc.robust.omega_f = robust->omega_f; // empty ⇒ build_augmented uses the identity
  }

  // -------------------------------------------------------------------------
  //  S8.6 elasticity metadata — record, per ELASTIC descriptor, the materialized
  //  rows / cone it owns + its priority. CONSUMED only by risk/elasticity.hpp (on the
  //  infeasible re-solve). This walks the SAME fixed order the emitters use (R1) so the
  //  recorded ranges match C.A exactly; it does NOT touch (A, l, u) — the exact surface
  //  is unchanged. SCOPE: elastic LINEAR A-rows (dollar-neutral, box, factor-exposure,
  //  group, beta, sector net-weight) and elastic CONES (tracking-error, sector-risk SOC,
  //  robust). The gross / turnover L1 BUDGETS ride the aux-split metadata, not A-rows, so
  //  they are NOT relaxable here (a documented S8.6 scope boundary — the minimize-
  //  violation layer relaxes the linear + conic surface; L1-budget elasticity is a
  //  recorded follow-on). Only descriptors with `elastic == true` are recorded; an empty
  //  result ⇒ elasticity is a pure no-op (R10).
  // -------------------------------------------------------------------------
  void fill_elastic(MaterializedConstraints &mc, atx::usize M) const {
    atx::usize row = 0U; // running A-row cursor, mirrors the emit_* order EXACTLY
    // (1) dollar-neutral (GrossNet) — 1 row when present.
    if (gross.dollar_neutral) {
      if (gross.elastic) {
        mc.elastic.linear_rows.push_back({row, 1U, gross.priority});
      }
      row += 1U;
    }
    // (2) position box — M rows when any box-fold descriptor is present. The box belongs
    //     to PositionCap (the %ADV / %shares folds are caps, not separately relaxable);
    //     elastic iff PositionCap.elastic.
    if (has_box()) {
      if (pos && pos->elastic) {
        mc.elastic.linear_rows.push_back({row, M, pos->priority});
      }
      row += M;
    }
    // (3) factor exposure — one row per selected factor.
    if (fexp) {
      const atx::usize cnt = fexp->factor_cols.size();
      if (fexp->elastic && cnt > 0U) {
        mc.elastic.linear_rows.push_back({row, cnt, fexp->priority});
      }
      row += cnt;
    }
    // (4) group — one row per group.
    if (grp) {
      const atx::usize cnt = group_count();
      if (grp->elastic && cnt > 0U) {
        mc.elastic.linear_rows.push_back({row, cnt, grp->priority});
      }
      row += cnt;
    }
    // (5) beta — 1 row.
    if (beta) {
      if (beta->elastic) {
        mc.elastic.linear_rows.push_back({row, 1U, beta->priority});
      }
      row += 1U;
    }
    // (6) sector NET-WEIGHT rows — one per sector (the SOC variant emits NO A-rows; it is
    //     handled as a cone below). The net-weight rows belong to SectorRiskBudget.
    if (sector && !sector->soc) {
      const atx::usize cnt = sector_count();
      if (sector->elastic && cnt > 0U) {
        mc.elastic.linear_rows.push_back({row, cnt, sector->priority});
      }
      row += cnt;
    }

    // Cones, in the AUGMENTED-cone emission order build_augmented uses (R1): (a) tracking-
    // error (1 block iff active), (b) sector-risk SOC (one block per FINITE-σ sector, in
    // ascending sector order), (c) robust (1 block iff active). cone_index tracks the
    // position in AugmentedQp::cones; we record only the elastic ones.
    atx::usize cone_index = 0U;
    if (mc.tracking.active) {
      if (track && track->elastic) {
        mc.elastic.cones.push_back({cone_index, track->priority});
      }
      ++cone_index;
    }
    if (mc.sector_risk.active) {
      // One cone per sector with σ_g > 0 (mirrors build_augmented's skip of σ_g ≤ 0).
      for (const atx::f64 s : mc.sector_risk.sigma) {
        if (!(s > 0.0)) {
          continue;
        }
        if (sector && sector->elastic) {
          mc.elastic.cones.push_back({cone_index, sector->priority});
        }
        ++cone_index;
      }
    }
    if (mc.robust.active && mc.robust.kappa > 0.0) {
      if (robust && robust->elastic) {
        mc.elastic.cones.push_back({cone_index, robust->priority});
      }
      ++cone_index;
    }
  }
};

} // namespace atx::engine::risk
