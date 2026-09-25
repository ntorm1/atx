#pragma once

// atx::engine::combine — signal-space zoo combiners (Lane 5).
//
// ===========================================================================
//  What this unit is
// ===========================================================================
//  AlphaCombiner (combiner.hpp) fits blend weights on realized PnL streams. The
//  combiners here fit in FORECAST space over a SignalStore (z-scored date×instrument
//  signals + forward residual returns), which is where the cross-sectional
//  information lives:
//
//    IcirEwmaCombiner      w_a ∝ ICIR_a · max(0, 1 − haircut/|t_a|), EWMA moments of
//                          the per-date Pearson IC with half-life decay.
//    GrinoldKahnCombiner   w = Ω_IC⁻¹ E[IC], Ω_IC shrunk by a CovTarget
//                          (cov_targets.hpp: LW2004 / LW2003 const-corr / LW2020 /
//                          RMT clip).
//    FamaMacBethRidge      β_t = (Z_tᵀZ_t + λ n_t I)⁻¹ Z_tᵀ r_t per date, w = mean β_t
//                          (RAW return-per-unit-z coefficients, with FMB t-stats).
//    KakushadzeRegression  Kakushadze & Yu, "How to Combine a Billion Alphas"
//                          (arXiv 1603.05937): regress the σ-normalized expected
//                          returns on the serially- and cross-sectionally-demeaned
//                          normalized alpha returns; w ∝ residual/σ. Optional
//                          cluster dummies (within-cluster demeaning, Frisch-Waugh).
//                          Fewer alphas than dates saturates that regression, so
//                          there it uses the principal-component factor-model
//                          variant (see kakushadze_weights).
//                          Cost O(N·M·min(N,M)) plus a min(N,M)³ solve/eigensolve.
//
//  All fits honor the fit/apply firewall: only date rows in [window.begin,
//  window.end) (their signals AND their forward returns) are read — the
//  truncation-invariance tests pin this. The PIT embargo of the forward-return
//  horizon is walk_forward_combiner.hpp's job.
//
//  WHY A SEPARATE SignalCombineMethod (not new CombineMethod enumerators): the
//  existing CombineMethod is switched on EXHAUSTIVELY (no default) in
//  combined_source.hpp and atx-impl/src/stage_combine.cpp, files other lanes own.
//  Appending enumerators there would break those TUs under /WX -Wswitch, so the
//  signal-space methods get their own enum; wiring them into CombineMethod +
//  stage_combine is an integration-step item.
//
//  Determinism: no RNG; reductions in ascending (date, alpha, instrument) order.

#include <concepts> // std::same_as
#include <span>     // std::span
#include <vector>   // std::vector

#include "atx/core/error.hpp" // Result, Status
#include "atx/core/types.hpp" // f64, u8, u32, usize

#include "atx/core/linalg/linalg.hpp"          // MatX
#include "atx/engine/combine/cov_targets.hpp"  // CovTarget
#include "atx/engine/combine/signal_store.hpp" // SignalStore, FitWindow
#include "atx/engine/eval/hac.hpp"

namespace atx::engine::combine {

// Label horizon is measured in dates of the supplied IC/return stream. Callers with
// overlapping labels must declare it; one means daily non-overlapping observations.
// IidV1 plus RawV1 restores the pre-W0 inference and return treatment.
struct SignalInferenceConfig {
  eval::hac::TStatRule tstat_rule = eval::hac::TStatRule::HorizonAwareV3;
  atx::usize label_horizon = 1U;
  IcReturnTreatment return_treatment = IcReturnTreatment::WinsorizedV2;
};

enum class SignalCombineMethod : atx::u8 {
  IcirEwma,
  GrinoldKahn,
  FamaMacBethRidge,
  Kakushadze,
  Hrp, // hrp.hpp::HrpCombiner (hierarchical risk parity over alpha returns)
};

// Fitted signal-space weights. `w[a]` scales alpha a's z-score in the combined
// forecast (combine_forecast). Gross-normalized (Σ|w| = 1) for every method except
// FamaMacBethRidge, whose raw β are in return-per-unit-z. `tstat[a]` is the method's
// per-alpha significance (NaN where not defined). An all-zero `w` means no alpha
// carried a usable signal in the window (a zero forecast, never a silent uniform).
struct CombineWeights {
  std::vector<atx::f64> w;
  std::vector<atx::f64> tstat;
  atx::usize fit_begin = 0U;
  atx::usize fit_end = 0U;
};

template <class C>
concept Combiner = requires(const C &c, const SignalStore &s, FitWindow win) {
  { c.fit(s, win) } -> std::same_as<atx::core::Result<CombineWeights>>;
};

// Σ|w| = 1 in place; an all-zero vector is left untouched.
void normalize_gross(std::vector<atx::f64> &w) noexcept;

// Shared window validation: >= 1 alpha, window inside n_dates, >= min_rows rows.
[[nodiscard]] atx::core::Status validate_window(const SignalStore &s, FitWindow w,
                                                atx::usize min_rows);

// --- core kernels (store-free, individually testable) ------------------------

// Grinold-Kahn on an IC matrix (T×K; rows with any NaN are dropped): w = Ω⁻¹ E[IC],
// Ω = estimate_covariance(ic, target), with a 1e-10·tr(Ω)/K ridge retry if Ω is not
// SPD; Σ|w| = 1. tstat follows inference. Err for invalid inference or < 2 complete rows.
[[nodiscard]] atx::core::Result<CombineWeights>
grinold_kahn_weights(const atx::core::linalg::MatX &ic, CovTarget target,
                    SignalInferenceConfig inference = {});

// Kakushadze-Yu weights from alpha returns R (M×N, finite) and expected returns E
// (length N). `clusters` empty → the paper's algorithm; else clusters[a] is alpha a's
// cluster id (within-cluster demeaning ≡ cluster dummies). Alphas with zero serial
// variance get w = 0; Σ|w| = 1. Err on shape mismatch or M < 3.
//
// Two regimes, split on p = N_active − G (G = number of clusters, 1 without):
//   * p > M−1 (the paper's N >> M): regress Ẽ = E/σ on the demeaned design L
//     (no intercept) with ridge ρ = ridge_rel·tr(L Lᵀ)/N, solved in the (M−1)-dim
//     Gram via the push-through identity; w = ε/σ.
//   * p <= M−1 (the usual zoo): the plain regression is SATURATED — its residual is
//     only the null-space projection of Ẽ (w ∝ sign(mean Ẽ)/σ, independent of each
//     alpha's own E) — so the factor-model variant is used instead: Ẽ is regressed
//     on the top F principal components of the demeaned alpha correlation matrix
//     L Lᵀ/(M−1). F = n_factors when > 0, else the number of eigenvalues above the
//     Marchenko-Pastur edge; F <= p − 1. F = 0 gives w ∝ E/σ² (diagonal MV).
[[nodiscard]] atx::core::Result<std::vector<atx::f64>>
kakushadze_weights(const atx::core::linalg::MatX &r, std::span<const atx::f64> expected,
                   std::span<const atx::u32> clusters, atx::f64 ridge_rel,
                   atx::usize n_factors = 0U);

// --- combiners ---------------------------------------------------------------

struct IcirEwmaCombiner {
  atx::f64 half_life = 126.0;   // dates; <= 0 → equal weights over the window
  atx::f64 tstat_haircut = 2.0; // shrink factor max(0, 1 − haircut/|t|); 0 disables
  SignalInferenceConfig inference{};
  [[nodiscard]] atx::core::Result<CombineWeights> fit(const SignalStore &s, FitWindow w) const;
};

struct GrinoldKahnCombiner {
  CovTarget target = CovTarget::LwIdentity;
  SignalInferenceConfig inference{};
  [[nodiscard]] atx::core::Result<CombineWeights> fit(const SignalStore &s, FitWindow w) const;
};

struct FamaMacBethRidge {
  atx::f64 lambda = 0.0; // per-observation ridge penalty (λ·n_t on the K×K diagonal)
  SignalInferenceConfig inference{};
  [[nodiscard]] atx::core::Result<CombineWeights> fit(const SignalStore &s, FitWindow w) const;
};

struct KakushadzeRegression {
  atx::usize expected_window = 0U; // E_a = mean of the last k alpha returns; 0 → whole window
  atx::f64 ridge_rel = 1e-8;       // regression regime (N_active − G > M−1) only
  atx::usize n_factors = 0U;       // factor regime (N_active − G <= M−1): 0 → MP-edge auto
  std::vector<atx::u32> clusters; // optional cluster dummies (size n_alphas, or empty)
  SignalInferenceConfig inference{};
  [[nodiscard]] atx::core::Result<CombineWeights> fit(const SignalStore &s, FitWindow w) const;
};

static_assert(Combiner<IcirEwmaCombiner>);
static_assert(Combiner<GrinoldKahnCombiner>);
static_assert(Combiner<FamaMacBethRidge>);
static_assert(Combiner<KakushadzeRegression>);

} // namespace atx::engine::combine
