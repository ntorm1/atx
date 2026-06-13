#pragma once

// atx::engine::learn — nonlinear STACKING mega-combiner (alpha-of-alphas), S5-6.
//
// =====================================================================
//  What this header is
// =====================================================================
//  The "crown jewel": a deflation-gated NONLINEAR meta-model over a pool of
//  alphas. Each pool alpha's per-(date,instrument) position becomes ONE feature
//  column of a meta-FeatureMatrix; the label is the instrument's forward return
//  (the thing every alpha predicts). fit_stack fits a NONLINEAR base (a histogram
//  GBT) and a LINEAR base (elastic-net) on the SAME meta-matrix, scores BOTH by
//  the SAME per-date out-of-fold information-coefficient / deflated-Sharpe (same
//  CPCV folds, same metric), and admits the nonlinear stack ONLY if it beats the
//  linear combination OUT-OF-SAMPLE AFTER DEFLATION (§0.4 — "no ML-for-ML's-sake":
//  a linearly-combinable pool gives the GBT no extra OOS edge -> reject; a genuine
//  nonlinear-interaction pool -> the GBT's OOS IC beats linear -> admit). When a
//  regime model is supplied the nonlinear base is fit PER REGIME (§4.5): each date
//  is assigned its point-in-time argmax HMM regime, the meta rows are partitioned
//  by date-regime, a separate nonlinear base is fit on each partition, and the
//  per-date OOS skill series is the union of the per-regime out-of-fold series — so
//  a pool whose optimal combination DIFFERS by regime scores a higher OOS dsr than
//  a single flat fit.
//
//  An admitted stack plugs into the library as a LEARNED alpha (M6): a stack ->
//  combine::AlphaMetrics + a synthesized pnl/position stream becomes a
//  library::AlphaCandidate that admits through library::Library::admit exactly
//  like a mined formulaic alpha.
//
// =====================================================================
//  §0.4 AS-BUILT AMENDMENT (the honest, metric-comparable gate)
// =====================================================================
//  The plan's §4.7 pseudocode benchmarks the nonlinear stack against the P4
//  combine::AlphaCombiner{ShrinkageMv}. BUT AlphaCombiner fits weights in the
//  realized-PnL / mean-variance plane (no forward-return labels), while the S5
//  base models (fit_linear/fit_gbt) score per-date OUT-OF-FOLD information
//  coefficient in the FORWARD-RETURN plane. A PnL-Sharpe and a forward-return-IC
//  are DIFFERENT metrics in DIFFERENT planes — DSR-comparing them is dishonest.
//  So the gate compares the nonlinear base vs a LINEAR base, BOTH fit on the SAME
//  meta-FeatureMatrix and BOTH scored by the SAME oos_deflated_sharpe / oos_ic
//  (same CPCV folds, same metric; they differ only in each model's own deflation
//  trial_count). This is the honest, metric-comparable realization of §0.4's
//  intent. The P4 AlphaCombiner remains the CONCEPTUAL linear-combination
//  reference but operates in a non-comparable plane, so it is NOT the numeric gate.
//
// =====================================================================
//  LIVE RE-EVAL ADAPTER — DEFERRED (documented, NOT faked)
// =====================================================================
//  There is no runtime DSL compile/eval in the codebase (§0.1): no
//  alpha::Engine evaluate-from-expr_source API exists. A stack therefore cannot be
//  re-evaluated live from its Provenance.expr_source — that string is
//  RECORD-KEEPING only (it identifies the learned-stack lineage for the library
//  catalog). The training / admit path does NOT need a live adapter; building a
//  fake parser would be dishonest, so the live re-eval adapter is deferred until a
//  public DSL-compile API exists. (The S5-3 LearnedSignalSource already adapts a
//  fitted model to ISignalSource over a live PanelView for the raw-field subset.)
//
// =====================================================================
//  Firewalls (M1/M3) inherited from the base models
// =====================================================================
//  M1 determinism: fit_linear is RNG-free cyclic CD; fit_gbt seeds every subsample
//  from seed_for(master_seed, ...) with order-fixed reductions; the regime split is
//  a deterministic PIT-argmax over a fixed per-date observable. The verdict_hash is
//  taken over the DECIDED NUMERIC fields only (admitted + the two dsr + two ic +
//  reason), all deterministic from the seeded fits — never over filesystem state or
//  any map iteration, so two same-seed fits give a byte-identical verdict.
//  M3 deflation: the gate scores each model's GENUINE out-of-fold skill series
//  (assembled inside fit_linear/fit_gbt from fold-local, train-only-fit OOS
//  predictions) through eval::deflated_sharpe with N == that model's trial_count.
//  A no-edge / over-searched base returns dsr <= 0, so the gate cannot be fooled.
//
// Header-only; fitting is a COLD path, so std::vector / Eigen allocation is fine.

#include <span>    // std::span
#include <vector>  // std::vector

#include <Eigen/Dense> // Eigen::Index, MatX/VecX

#include "atx/core/types.hpp"  // f64, u8, u16, u32, u64, usize

#include "atx/core/linalg/linalg.hpp" // MatX, VecX

#include "atx/engine/combine/store.hpp"        // combine::AlphaStore, AlphaId
#include "atx/engine/eval/cpcv.hpp"            // eval::CpcvConfig
#include "atx/engine/learn/elastic_net.hpp"    // ElasticNetCfg
#include "atx/engine/learn/feature_matrix.hpp" // FeatureMatrix
#include "atx/engine/learn/gbt.hpp"            // GbtCfg, fit_gbt, oos_ic
#include "atx/engine/learn/hmm.hpp"            // Hmm, regime_posterior_at
#include "atx/engine/learn/latent.hpp"         // LatentAugmentation
#include "atx/engine/learn/learned_source.hpp" // LearnedModel, ModelKind
#include "atx/engine/learn/linear_alpha.hpp"   // LinearAlphaCfg, fit_linear, oos_deflated_sharpe, predict_at
#include "atx/engine/library/library.hpp"      // library::AlphaCandidate, Provenance

namespace atx::engine::learn {

// ===========================================================================
//  AdmitKind — the learn-domain stacking verdict (DISTINCT from
//  library::AdmitKind). A stack is either admitted (Accept) or rejected for
//  failing the OOS-after-deflation fitness gate (RejectFitness).
// ===========================================================================
enum class AdmitKind : atx::u8 { Accept, RejectFitness };

// ===========================================================================
//  StackingCfg — the stacking-combiner knobs.
//
//  base        : which nonlinear base the stack fits (Gbt = histogram GBT, the
//                interaction-capturing tree base; ElasticNet = a second linear
//                base, used when the "nonlinear" capacity is itself an L1/L2 fit).
//  cpcv        : the CPCV fold config — SHARED by both the linear benchmark and the
//                nonlinear base, so the gate is same-fold same-metric.
//  linear_en   : the elastic-net penalty for the LINEAR benchmark base.
//  gbt         : the GBT knobs when base == Gbt (its master_seed/cpcv/horizons are
//                overwritten from this cfg so the two bases share the fold + seed).
//  master_seed : the determinism root (M1), folded into every fit.
//  horizons    : the forward-return horizons (the meta's Y horizons).
// ===========================================================================
struct StackingCfg {
  enum class Base : atx::u8 { ElasticNet, Gbt } base{Base::Gbt};
  eval::CpcvConfig cpcv{};
  ElasticNetCfg linear_en{/*lambda=*/0.02, /*alpha=*/0.5, /*max_iter=*/2000, /*tol=*/1e-9};
  GbtCfg gbt{};
  atx::u64 master_seed{0};
  std::vector<atx::u16> horizons{1};
};

// ===========================================================================
//  StackingVerdict — the decided gate outcome (the M1 byte-identical surface).
//
//  admitted          : the stack beats linear OOS-after-deflation (§0.4 gate).
//  oos_dsr_nonlinear : the nonlinear base's OOS deflated Sharpe (regime-aware).
//  oos_dsr_linear    : the linear benchmark's OOS deflated Sharpe.
//  oos_ic_nonlinear  : the nonlinear base's mean per-date OOS IC.
//  oos_ic_linear     : the linear benchmark's mean per-date OOS IC.
//  reason            : Accept iff admitted, else RejectFitness.
//  verdict_hash      : a deterministic digest of the DECIDED numeric fields above
//                      (admitted + the two dsr + two ic + reason) — the M1 surface.
// ===========================================================================
struct StackingVerdict {
  bool admitted{false};
  atx::f64 oos_dsr_nonlinear{0.0};
  atx::f64 oos_dsr_linear{0.0};
  atx::f64 oos_ic_nonlinear{0.0};
  atx::f64 oos_ic_linear{0.0};
  AdmitKind reason{AdmitKind::RejectFitness};
  atx::u64 verdict_hash{0};
};

namespace ensemble_detail {

// Build the LINEAR-benchmark cfg: an elastic-net linear base sharing the stack's
// CPCV folds, seed, and horizons (so the gate is same-fold). No ridge baseline
// (a genuine L1/L2 fit), and an EMPTY LatentAugmentation at the call site so the
// linear base is a PURE linear combination of the alpha columns (no interactions).
[[nodiscard]] inline LinearAlphaCfg linear_cfg_of(const StackingCfg &cfg) {
  LinearAlphaCfg lin;
  lin.en = cfg.linear_en;
  lin.use_ridge_baseline = false;
  lin.cpcv = cfg.cpcv;
  lin.master_seed = cfg.master_seed;
  lin.horizons = cfg.horizons;
  return lin;
}

// Build the nonlinear GBT cfg: the stack's gbt knobs with the shared CPCV folds,
// seed, and horizons stamped in (so the nonlinear base scores the SAME folds /
// metric as the linear benchmark — the same-metric gate).
[[nodiscard]] inline GbtCfg gbt_cfg_of(const StackingCfg &cfg) {
  GbtCfg g = cfg.gbt;
  g.cpcv = cfg.cpcv;
  g.master_seed = cfg.master_seed;
  g.horizons = cfg.horizons;
  return g;
}

// Build the ElasticNet nonlinear cfg (base == ElasticNet): the SAME elastic-net
// kernel as the linear benchmark but with its OWN seed offset, so the "nonlinear"
// arm is a distinct fit (a different deflation stream). Used only when the cfg
// selects ElasticNet as the nonlinear base; the GBT path is the common case.
[[nodiscard]] inline LinearAlphaCfg nonlinear_en_cfg_of(const StackingCfg &cfg) {
  LinearAlphaCfg lin = linear_cfg_of(cfg);
  lin.master_seed = cfg.master_seed ^ 0xA5A5A5A5ULL; // distinct deflation stream
  return lin;
}

// The per-date regime OBSERVABLE the HMM is (and was) fit on: for each date, the
// cross-sectional MEAN of the meta's LAST feature column (the regime marker) over
// that date's rows. Deterministic (single forward walk over (date,instrument)-
// ordered rows; no map). Returns an (n_dates x 1) MatX so regime_posterior_at can
// run its forward filter over it. A date with no rows gets observable 0.
[[nodiscard]] atx::core::linalg::MatX regime_observable(const FeatureMatrix &fm);

// The PIT-argmax regime of each USED date: regime_posterior_at(*regime, obs, d) is
// the forward-only filter P(state | obs[0..d]); the argmax (lower index wins ties)
// is the date's regime. Returns a length-n_dates vector (dates with no rows are
// assigned regime 0 — they carry no meta rows so the value is irrelevant). PIT: the
// filter at d reads only obs rows [0..d] (M2, inherited from regime_posterior_at).
[[nodiscard]] std::vector<atx::u32> date_regimes(const Hmm &regime,
                                                 const atx::core::linalg::MatX &obs);

// A sub-FeatureMatrix holding exactly the rows of `fm` whose date is assigned to
// regime `which` by `date_reg`. Preserves the original n_dates / n_features and
// each row's (date, instrument) provenance + features + labels, so date_label_spans
// / the CPCV walk inside fit_gbt see the regime's true date axis (a sparse subset of
// the global dates). Deterministic (ascending row walk, no map).
[[nodiscard]] FeatureMatrix regime_submatrix(const FeatureMatrix &fm,
                                             const std::vector<atx::u32> &date_reg, atx::u32 which);

// Fit the FLAT nonlinear base over the whole meta (no regime conditioning) and
// return it. The Gbt arm fits a histogram GBT; the ElasticNet arm fits a second
// elastic-net (a distinct deflation stream from the linear benchmark). Both fill
// oos_score_series / trial_count, so the shared oos_deflated_sharpe / oos_ic read
// them uniformly.
[[nodiscard]] inline LearnedModel fit_flat_nonlinear(const FeatureMatrix &meta,
                                                     const StackingCfg &cfg) {
  const LatentAugmentation empty_aug; // NO interaction aug — the GBT must find them
  switch (cfg.base) {
  case StackingCfg::Base::Gbt:
    return fit_gbt(meta, empty_aug, gbt_cfg_of(cfg));
  case StackingCfg::Base::ElasticNet:
    return fit_linear(meta, empty_aug, nonlinear_en_cfg_of(cfg));
  }
  return LearnedModel{}; // unreachable: every Base handled (no default).
}

// Assemble the REGIME-CONDITIONAL nonlinear OOS skill into a synthetic model whose
// oos_score_series is the UNION of the per-regime out-of-fold series and whose
// trial_count is the SUM of the per-regime trial counts (the combined deflation N).
// Each regime's base is fit ONLY on that regime's meta rows (fit_flat_nonlinear on
// the sub-matrix), so its oos_score_series is genuinely out-of-fold WITHIN the
// regime; the union is the per-date OOS skill of the regime-conditional ensemble.
// The shared oos_deflated_sharpe / oos_ic then score it exactly as for a flat fit.
[[nodiscard]] LearnedModel fit_regime_nonlinear(const FeatureMatrix &meta, const Hmm &regime,
                                                const StackingCfg &cfg);

// The deterministic verdict_hash over the DECIDED numeric fields (M1): admitted,
// the two dsr, the two ic, and the reason — laid into a fixed f64 buffer and
// hashed by bytes. No filesystem / map-order input, so two same-seed fits hash
// identically.
[[nodiscard]] atx::u64 verdict_hash_of(const StackingVerdict &v);

} // namespace ensemble_detail

// ===========================================================================
//  meta_features_from_pool — the alpha-of-alphas meta-FeatureMatrix.
//
//  Each ROW is one (date, instrument) cell (in (date, instrument) order, matching
//  the AlphaStore's period/instrument axes); each FEATURE column f is pool alpha
//  f's position at that cell (pool.positions(AlphaId{f}, date)[inst]); the label
//  Y[h][row] is the instrument's forward return over horizon `horizons[h]`, read
//  from `forward_returns_flat` (period-major then instrument-minor, length
//  n_periods*n_instruments). The AlphaStore carries NO forward returns, so the
//  caller supplies them. A cell is row_valid iff every alpha column is finite.
//  PURE / deterministic. (fit_stack itself takes the meta — this builds it.)
// ===========================================================================
[[nodiscard]] FeatureMatrix meta_features_from_pool(const combine::AlphaStore &pool,
                                                    std::span<const atx::f64> forward_returns_flat,
                                                    std::span<const atx::u16> horizons);

// ===========================================================================
//  fit_stack — the §0.4 deflation-gated stacking gate (the load-bearing seam).
//
//  Fits the NONLINEAR base (flat, or regime-conditional when `regime != nullptr`)
//  and the LINEAR benchmark on the SAME `meta`, SAME CPCV folds, and scores BOTH
//  by the SAME oos_deflated_sharpe / oos_ic (the §0.4 same-metric amendment).
//  GATE PREDICATE: admitted := (oos_dsr_nonlinear > 0.0) && (oos_ic_nonlinear >
//  oos_ic_linear) — non-vacuous BOTH ways: a linearly-combinable pool gives the
//  GBT no OOS IC over linear (reject); a genuine nonlinear-interaction pool ->
//  the GBT's OOS IC beats linear AND survives deflation (admit). PURE in
//  (meta, regime, cfg); deterministic (M1) — the verdict_hash pins it.
// ===========================================================================
[[nodiscard]] StackingVerdict fit_stack(const FeatureMatrix &meta, const Hmm *regime,
                                        const StackingCfg &cfg);

// ===========================================================================
//  StackCandidate — an OWNING wrapper around a library::AlphaCandidate.
//
//  library::AlphaCandidate's pnl / pos_flat are NON-OWNING spans; the synthesized
//  buffers must outlive the admit() call. StackCandidate owns them and exposes the
//  candidate whose spans point INTO its own buffers (so a copy/move would dangle —
//  it is used in place: build it, then admit cand.candidate, then let it drop).
// ===========================================================================
struct StackCandidate {
  std::vector<atx::f64> pnl;       // [n_periods] synthesized realized pnl
  std::vector<atx::f64> pos_flat;  // [n_periods*n_instruments] period-major positions
  library::AlphaCandidate candidate; // spans point into pnl / pos_flat above
};

namespace ensemble_detail {

// Re-fit the DEPLOYED flat nonlinear model (full-window forests/coeffs) so its
// predict_at gives the cross-section the synthesized stream uses. fit_stack scores
// out-of-fold series for the GATE, but the DEPLOYED stream needs the full-window
// model — a fresh flat fit on the whole meta (same cfg). Deterministic (M1).
[[nodiscard]] inline LearnedModel deploy_nonlinear(const FeatureMatrix &meta,
                                                   const StackingCfg &cfg) {
  return fit_flat_nonlinear(meta, cfg);
}

} // namespace ensemble_detail

// ===========================================================================
//  stack_to_candidate — synthesize the library candidate of an admitted stack (M6).
//
//  Builds a deployed (full-window) nonlinear model and walks each date's predicted
//  cross-section: position(d, inst) = the model's predicted score at that cell
//  (over the meta's VALID rows), and pnl[d] = Σ_inst position(d,inst) *
//  forward_return(d, inst) (the horizon-0 label Y[0] from `meta`). metrics are
//  combine::compute_metrics over the synthesized streams; canon_hash is a
//  deterministic byte hash of the deployed model + the streams; expr_source is
//  "learned:stack:<base>"; as_of is the last period. The returned StackCandidate
//  OWNS the pnl / pos_flat buffers the candidate's spans point at.
//
//  NOTE: the candidate's pnl/pos_flat are sized to the meta's full (n_dates x
//  n_instruments) grid — period-major, instrument-minor — so the library's
//  shared-period contract holds (every row a coherent cross-section). A non-emitted
//  (out-of-universe / invalid) cell contributes position 0 (flat, no opinion).
// ===========================================================================
[[nodiscard]] StackCandidate stack_to_candidate(const StackingVerdict & /*verdict*/,
                                                const FeatureMatrix &meta, const StackingCfg &cfg);

} // namespace atx::engine::learn
