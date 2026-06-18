#pragma once

// atx::engine::risk — FactorModel: the FACTORED covariance V = X F Xᵀ + D (P4-7a).
//
// ===========================================================================
//  What this unit is
// ===========================================================================
//  FactorModel is a Barra-style risk model kept in FACTORED form
//      V = X F Xᵀ + D ,   X : M×K exposures,  F : K×K factor covariance (SPD),
//                          D : M specific (idiosyncratic) variances (diagonal, >0)
//  and applies it WITHOUT ever materializing the dense M×M V (M ≫ K, so a dense
//  V would be O(M²) memory and O(M²) per matvec — defeated here by working in the
//  K-dimensional factor space):
//    risk(w)               = (Xᵀw)ᵀ F (Xᵀw) + Σ D_i w_i²      O(MK + K²)
//    apply_inverse(in,out) = V⁻¹·in via Woodbury               O(MK + K³)
//    neutralize(signal)    = s − X (XᵀX)⁻¹ Xᵀ s  (in place)    O(MK + K³)
//  plus a carried fit window [fit_begin, fit_end).
//
//  P4-7a SCOPE: this unit stores a GIVEN (X, F, D) and applies it. The per-date
//  cross-sectional WLS that ESTIMATES X, F, D — `FactorModelBuilder::build` — is
//  P4-7b. Nothing in FactorModel itself estimates anything.
//
// ===========================================================================
//  Woodbury inverse + the cached capacitance (the WHOLE POINT)
// ===========================================================================
//  V⁻¹·x = D⁻¹x − D⁻¹X (F⁻¹ + Xᵀ D⁻¹ X)⁻¹ Xᵀ D⁻¹x        (Sherman-Morrison-Woodbury)
//  The K×K capacitance C = F⁻¹ + Xᵀ D⁻¹ X is SPD (F⁻¹ SPD + Xᵀ D⁻¹ X PSD). It is
//  built ONCE at construction and its Cholesky (Eigen::LLT) is CACHED, so each
//  apply_inverse is two matvecs (Xᵀ·, X·) + one cached K×K solve — never a refactor
//  and never an M×M materialization. dinv = 1/D is cached too.
//
// ===========================================================================
//  The D floor + the neutralize ridge (numerical guards)
// ===========================================================================
//  D floor: d_i ← max(d_i, kSpecificVarFloor). An all-zero specific-variance
//  instrument would make D⁻¹ infinite and V only PSD; flooring keeps D⁻¹ finite and
//  V positive-DEFINITE. kSpecificVarFloor = 1e-12 is far below any real variance.
//  Neutralize ridge: XᵀX (K×K) is singular when X has collinear columns, so
//  neutralize solves (XᵀX + kNeutralizeRidge·I) z = Xᵀs. kNeutralizeRidge = 1e-10
//  is a numerical guard so a collinear exposure block still residualizes; it is far
//  below any real factor-exposure scale and does not bias a full-rank X materially.
//
// ===========================================================================
//  NaN-in-neutralize policy
// ===========================================================================
//  A NaN in the input signal PROPAGATES through neutralize: a "no opinion" cell
//  stays NaN in the output (Xᵀs picks up the NaN, so the whole residual carries it).
//  This is intentional — the caller's WeightPolicy maps a NaN weight to 0; we do not
//  silently fabricate an opinion for a missing cell here.
//
// ===========================================================================
//  Determinism
// ===========================================================================
//  NO RNG. risk() reductions run in canonical ascending order (instrument, then
//  factor). The apply/neutralize matvecs and the cached Cholesky are deterministic
//  given (X, F, D). Same inputs → same outputs.
//
// ===========================================================================
//  S8.8a header/source split (pure compilation refactor — R10, ZERO behavior change)
// ===========================================================================
//  The method BODIES (FactorModel apply-math, FactorModelBuilder estimation, the
//  detail:: kernels) live in src/risk/factor_model.cpp, compiled INTO the
//  atx-engine static library — collapsing the include fan-out (a body edit no longer
//  recompiles the 42 dependents of this hub header). This header keeps the class
//  DECLARATIONS + the POD config/result structs. FactorModel's private Eigen members
//  (x_, f_, d_, dinv_, the cached Eigen::LLT cap_llt_) are hidden behind a PIMPL
//  (`struct Impl`, defined in the .cpp) so the heavy estimation includes (Ledoit-Wolf
//  combine, robust IRLS, EWMA/Newey-West, eigen-adjust, APCA, VRA, horizon-blend,
//  specific-risk, regression) no longer leak through this header. The math is
//  byte-identical: same factored apply, same no-pivot factorizations, same
//  order-fixed reductions — only the translation unit changed.

#include <memory> // std::unique_ptr (FactorModel pimpl)
#include <span>   // std::span (risk / apply_inverse / neutralize args)
#include <vector> // std::vector (FactorComponents members)

#include "atx/core/error.hpp" // Result, Ok, Err, ErrorCode

#include "atx/core/linalg/linalg.hpp" // MatX, VecX (column-major Eigen; accessor return types)
#include "atx/core/types.hpp"         // f64, u32, usize

#include "atx/engine/loop/panel_types.hpp"  // PanelView (the trailing newest-first panel)
#include "atx/engine/risk/exposures.hpp"    // FactorModelConfig (Builder member), ExposureMatrix
#include "atx/engine/risk/fwd.hpp"          // FactorModel / FactorModelBuilder fwd decls

namespace atx::engine::risk {

// Specific-variance floor: d_i ← max(d_i, kSpecificVarFloor) so D⁻¹ is finite and
// V is positive-DEFINITE even for a zero-idiosyncratic-variance instrument.
inline constexpr atx::f64 kSpecificVarFloor = 1e-12;

// Tiny diagonal ridge on XᵀX in neutralize so a collinear (rank-deficient) exposure
// block still residualizes; far below any real factor-exposure scale.
inline constexpr atx::f64 kNeutralizeRidge = 1e-10;

// ===========================================================================
//  FactorModel — factored covariance V = X F Xᵀ + D (apply-math; P4-7a).
//
//  The private (X, F, D, dinv, cached Cholesky) state is held behind a PIMPL
//  (struct Impl, defined in src/risk/factor_model.cpp). The class is MOVE-ONLY
//  (the unique_ptr is the only data member besides the two POD fit bounds); the
//  out-of-line special members are defaulted in the .cpp where Impl is complete.
// ===========================================================================
class FactorModel {
public:
  // Build a FactorModel from estimated X (M×K), F (K×K SPD), D (M specific
  // variances), and the fit window [fit_begin, fit_end). Validates the shapes,
  // FLOORS D (kSpecificVarFloor), requires F to be SPD (its Cholesky succeeds), and
  // PRECOMPUTES + CACHES everything the apply path needs (dinv, the Cholesky of the
  // K×K capacitance C = F⁻¹ + Xᵀ D⁻¹ X). Err on a shape violation, an empty window,
  // a non-SPD F, or K exceeding the risk() stack-buffer bound.
  [[nodiscard]] static atx::core::Result<FactorModel>
  create(atx::core::linalg::MatX x, atx::core::linalg::MatX f, atx::core::linalg::VecX d,
         atx::usize fit_begin, atx::usize fit_end);

  // Value semantics, preserved EXACTLY from the pre-split (header-only) class: the
  // old FactorModel held Eigen members by value and was copyable. The pimpl keeps
  // that contract — copy deep-copies the Impl (X, F, D, dinv, cached Cholesky; all
  // Eigen-copyable), move transfers the pointer. Defined out-of-line in the .cpp
  // where Impl is complete. (std::optional<FactorModel> / by-value overrides in the
  // book/data layer depend on copyability — R10: zero API change.)
  FactorModel(const FactorModel &other);
  FactorModel &operator=(const FactorModel &other);
  FactorModel(FactorModel &&) noexcept;
  FactorModel &operator=(FactorModel &&) noexcept;
  ~FactorModel();

  [[nodiscard]] atx::usize n_factors() const noexcept;     // K
  [[nodiscard]] atx::usize n_instruments() const noexcept; // M

  // The M×K exposure matrix X (read-only). Mirrors n_factors()/n_instruments():
  // FactorModel keeps V factored, so X is the only way a caller can form a per-
  // period book factor exposure Xᵀw (the S7-4 book report's factor_exposures row).
  [[nodiscard]] const atx::core::linalg::MatX &exposures() const noexcept;

  // wᵀ V w computed in factor space: (Xᵀw)ᵀ F (Xᵀw) + Σ D_i w_i². noexcept and
  // ALLOC-FREE — manual order-fixed loops (ascending i, then k), NOT Eigen
  // temporaries — so it is genuinely the per-rebalance apply path the optimizer can
  // call without heap traffic. g_k is accumulated into a fixed K-stack buffer.
  [[nodiscard]] atx::f64 risk(std::span<const atx::f64> w) const noexcept;

  // V⁻¹·in via Woodbury, O(MK + K³): out = D⁻¹in − D⁻¹X C⁻¹ Xᵀ D⁻¹in, where C is the
  // cached capacitance. The K-sized temporaries (t2,t3) are a small documented
  // apply-path allocation; the M-sized work aliases the spans (no M×M anywhere).
  void apply_inverse(std::span<const atx::f64> in, std::span<atx::f64> out) const;

  // V·in = X F (Xᵀ in) + D ∘ in (the FORWARD apply — the dual of apply_inverse;
  // S1-2 QP needs P = 2λV as a matvec). O(MK + K²) via the K-dimensional factor
  // space: t = Xᵀin (K), then X(F t) (M) + D∘in (M) — NEVER an M×M materialization.
  void apply(std::span<const atx::f64> in, std::span<atx::f64> out) const;

  // The M specific variances D (read-only) — the diagonal of V (floored, > 0). The
  // S1-2 QP solver uses it as a cheap deterministic Jacobi-preconditioner diagonal
  // for the matrix-free 2λV block.
  [[nodiscard]] const atx::core::linalg::VecX &specific_var() const noexcept;

  // The K×K factor covariance F (read-only, SPD). The S8.1 factor-augmented QP
  // exposes y = Xᵀw and puts 2λF on the y-block of the augmented Hessian.
  [[nodiscard]] const atx::core::linalg::MatX &factor_cov() const noexcept;

  // Factor-neutralize a signal IN PLACE: s ← s − X (XᵀX)⁻¹ Xᵀ s, the residual of s
  // on the factor exposures. A tiny ridge (kNeutralizeRidge) on XᵀX keeps a
  // collinear exposure block solvable. NaN cells propagate (see header note).
  void neutralize(std::span<atx::f64> signal) const;

  [[nodiscard]] atx::usize fit_begin() const noexcept;
  [[nodiscard]] atx::usize fit_end() const noexcept;

  // Max K we materialize the risk() g-buffer for on the stack. K is the factor count
  // (sector dummies + ≤5 style factors); 256 is far above any realistic factor block
  // yet keeps the buffer tiny. ENFORCED at construction (create() rejects K > this),
  // so risk()'s fixed buffer is provably never overrun. PUBLIC so callers can size
  // their own bounds against the same limit (was a private static constexpr; the
  // value is unchanged — purely a visibility move during the S8.8a split).
  static constexpr Eigen::Index kMaxFactorsStack = 256;

private:
  // The private (X, F, D, dinv, cached Cholesky) state, defined in the .cpp.
  struct Impl;
  explicit FactorModel(std::unique_ptr<Impl> impl, atx::usize fit_begin, atx::usize fit_end);

  std::unique_ptr<Impl> impl_;
  atx::usize fit_begin_;
  atx::usize fit_end_;
};

// ===========================================================================
//  detail:: — the FactorModelBuilder estimation kernels.
//
//  The kernel BODIES live in src/risk/factor_model.cpp. Only factor_covariance is
//  DECLARED here (not defined inline) because a test (risk_cov_ewma_test) calls it
//  directly as the single-window MLE reference; it resolves to the library symbol.
//  The remaining kernels (date_returns / select_rows / pop_variance /
//  robust_prior_weight) are file-local to the .cpp.
// ===========================================================================
namespace detail {

// The K×K Ledoit-Wolf shrunk covariance of a T×K factor-return series. REUSES the
// CANONICAL combine LW intensity (combine::detail::ledoit_wolf_intensity) on the
// column-demeaned series + its MLE covariance S (divisor T). F = (1−δ)·S + δ·m·I,
// m = tr(S)/K. cfg_shrink >= 0 overrides δ with the fixed value (clamped). Order-fixed
// demean; SPD by the shrinkage toward m·I (FactorModel::create re-checks via Cholesky).
[[nodiscard]] atx::core::linalg::MatX factor_covariance(atx::core::linalg::MatX fseries,
                                                        atx::f64 cfg_shrink);

} // namespace detail

// ===========================================================================
//  FactorComponents — the estimated (X, F, D, fit_end) FactorModelBuilder feeds to
//  FactorModel::create. Exposed by build_components(...) so the dead-alpha factor
//  augmentation (S7-3, risk/dead_factor.hpp) can hstack X with the extracted dead
//  loadings and blockdiag F with the dead variances BEFORE the create() assembly,
//  without re-running the per-date WLS estimation. build() is a thin wrapper:
//  build_components -> create (BEHAVIOR-PRESERVING split, §0.3 reserved-slot).
//  POD; kept in the header (other headers/tests name it directly).
// ===========================================================================
struct FactorComponents {
  atx::core::linalg::MatX X; // M×K exposures (X[0], the current cross-section)
  atx::core::linalg::MatX F; // K×K factor covariance (Ledoit-Wolf shrunk, SPD)
  atx::core::linalg::VecX D; // M specific (idiosyncratic) variances
  atx::usize fit_end;        // the fit window upper bound (== window)
};

// ===========================================================================
//  FactorModelBuilder — per-date cross-sectional WLS estimating (X, F, D) (P4-7b).
//
//  build(panel, window, market_cap, group_id) ESTIMATES the factored covariance
//  V = X[0] F X[0]ᵀ + diag(D) from a trailing newest-first PanelView, then calls
//  FactorModel::create. The method BODIES live in src/risk/factor_model.cpp; the
//  full estimation contract (the fixed deterministic two-pass OLS→WLS, the LW factor
//  covariance, the S8.2–S8.8 covariance-cleaning rungs, PIT-structural reads) is
//  documented at each method definition there. COLD path. NO RNG; deterministic.
// ===========================================================================
class FactorModelBuilder {
public:
  FactorModelConfig cfg;

  // Estimate (X, F, D) over the trailing `window` cross-sections and assemble the
  // FactorModel. THIN WRAPPER (§0.3): runs build_components then FactorModel::create
  // — the estimation body lives in build_components so the S7-3 dead-factor
  // augmentation can intercept (X, F, D) before create. The stat (APCA) and dead
  // rungs are dispatched here. `[[nodiscard]] const`.
  [[nodiscard]] atx::core::Result<FactorModel> build(const PanelView &panel, atx::usize window,
                                                     std::span<const atx::f64> market_cap,
                                                     std::span<const atx::u32> group_id) const;

  // Estimate the factored covariance components (X, F, D) over the trailing `window`
  // cross-sections WITHOUT assembling the FactorModel — the FUNDAMENTAL estimation path
  // (style/sector regression + the full S8 cleaning pipeline), extracted so S7-3 can
  // augment (X, F) with dead-alpha risk factors before create(). The stat/dead rungs are
  // dispatched by build(); a direct caller must not request them here. Returns
  // FactorComponents{ X[0], F, D, window }. COLD path; PIT-structural. `[[nodiscard]] const`.
  [[nodiscard]] atx::core::Result<FactorComponents>
  build_components(const PanelView &panel, atx::usize window, std::span<const atx::f64> market_cap,
                   std::span<const atx::u32> group_id) const;

private:
  // ε floor for the bootstrap weights so 1/d0_i is finite for a zero-residual
  // instrument (a date with M_s==K fits exactly -> 0 OLS residual). Far below any
  // real return variance, so it never tilts a well-populated instrument's weight.
  static constexpr atx::f64 kBootstrapVarFloor = 1e-12;

  // The S8.6 STATISTICAL (APCA) model variant (Connor-Korajczyk 2-pass). A static
  // member so it shares the .cpp's detail:: kernels + FactorModel assembly with no
  // include cycle. Body + full algorithm doc in src/risk/factor_model.cpp.
  [[nodiscard]] static atx::core::Result<FactorModel>
  build_stat_factor_model(const PanelView &panel, atx::usize window,
                          std::span<const atx::f64> market_cap, std::span<const atx::u32> group_id,
                          const FactorModelConfig &cfg, atx::usize n_stat, bool gls_reweight,
                          atx::f64 factor_cov_shrink);

  // Pass A (OLS, equal weights) -> bootstrap specific variances d0. Returns the count
  // of usable dates. Body in src/risk/factor_model.cpp.
  [[nodiscard]] atx::core::Result<atx::usize>
  accumulate_ols(const PanelView &panel, atx::usize window, std::span<const atx::f64> market_cap,
                 std::span<const atx::u32> group_id, atx::usize k,
                 atx::core::linalg::VecX &d0_out) const;

  // Pass B (WLS, weights 1/d0_i) -> factor-return series f[s] + per-instrument
  // residuals. Returns the count of usable WLS dates. Body in the .cpp.
  [[nodiscard]] atx::core::Result<atx::usize>
  accumulate_wls(const PanelView &panel, atx::usize window, std::span<const atx::f64> market_cap,
                 std::span<const atx::u32> group_id, const atx::core::linalg::VecX &d0,
                 atx::core::linalg::MatX &fseries,
                 std::vector<std::vector<atx::f64>> &u_by_inst) const;

  // Pass B (ROBUST, S8.1; opt-in) — √-cap / inverse-specific-variance prior composed
  // with the Huber IRLS kernel (fixed cfg.cov.robust_iters steps, tol=0). Body in the
  // .cpp. Returns the usable-date count.
  [[nodiscard]] atx::core::Result<atx::usize>
  accumulate_robust(const PanelView &panel, atx::usize window, std::span<const atx::f64> market_cap,
                    std::span<const atx::u32> group_id, const atx::core::linalg::VecX &d0,
                    atx::core::linalg::MatX &fseries,
                    std::vector<std::vector<atx::f64>> &u_by_inst) const;

  // Specific (idiosyncratic) variances D over the current cross-section. EXHAUSTIVE
  // dispatch on cfg.cov.specific_method (PopVariance default = P4 byte-identical;
  // EwmaNeweyWestStructural = S8.4 blended + S8.8 horizon-blend). Body in the .cpp.
  [[nodiscard]] atx::core::linalg::VecX
  specific_variances(const ExposureMatrix &x0, const std::vector<std::vector<atx::f64>> &u_by_inst,
                     atx::usize window) const;
};

} // namespace atx::engine::risk
