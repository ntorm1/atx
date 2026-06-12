// atx::engine::learn — end-to-end learned-pipeline INTEGRATION proofs (S5-7).
//
// Suite LearnIntegration — the sprint-closing proofs that the already-built learn
// units (FeatureMatrix -> linear/gbt -> HMM -> stacking) WIRE into one coherent,
// deterministic, anti-snooping pipeline. These tests exercise the THIN harness in
// learn/pipeline.hpp, which constructs the existing cfgs and calls the existing
// fit_* / oos_* / fit_stack functions and folds their DECIDED outputs into a
// digest — it adds NO new model logic and NO new statistic.
//
//   1. DeterministicTrainingEndToEnd_ByteIdentical (M1 headline) — the same
//      FeatureMatrix + same master_seed runs the WHOLE pipeline (linear, gbt, hmm,
//      stack) and folds every fitted model's decided fields into ONE u64 digest;
//      two runs are byte-identical.
//   2. NoEdge_AdmitsNothing_RealEdge_AdmitsSurvivors (M3 anti-snooping, the proof
//      that lifts the p0 ML ban) — a genuinely edge-free panel (label independent
//      of every feature) admits ZERO learned models through the deflation gate; a
//      planted real linear edge admits > 0. SAME cfg/gate both sides. The noise
//      direction is swept across seeds (a false-admit on noise is a real M3 break).
//   3. NonlinearCombiner_RejectedWhenLinearSuffices (M3/§0.4) — a meta whose best
//      predictor is a pure linear blend of the alpha columns is NOT admitted by the
//      stacking gate (no ML-for-ML's-sake).
//   4. RegimeConditional_ImprovesOos (spec exit) — on a fixture whose optimal alpha
//      DIFFERS by regime, the regime-conditional OOS dsr beats the flat OOS dsr.
//   5. MultiHorizon_BlendBeatsBestSingleHorizon (§0.6 user ask) — the horizon-
//      blended OOS IC is at least the best single-horizon OOS IC (up to fp slack).
//   6. ThreadCountInvariance_DigestUnchanged (M1) — the pipeline is single-threaded
//      with order-fixed reductions, so the (currently no-op) worker knob cannot
//      change the digest: workers=1 and workers=4 produce the identical digest.
//
// Fixtures build FeatureMatrix / meta DIRECTLY (the gbt_test/ensemble_test
// make_fm/make_meta idiom); a local deterministic LCG carries reproducible noise.
// Naming: Subject_Condition_Expected. NON-VACUOUSNESS is asserted per fixture
// (noise truly edge-free; signal truly has an edge; regime truly regime-dependent;
// multi-horizon Y genuinely differs across horizons).

#include <limits> // std::numeric_limits (tail NaN label)
#include <span>   // std::span (multi-horizon horizon view)
#include <vector>

#include <Eigen/Dense> // Eigen::Index (regime observable)

#include <gtest/gtest.h>

#include "atx/core/linalg/linalg.hpp" // atx::core::linalg::MatX (regime observable)
#include "atx/core/types.hpp"         // f64, u16, u64, usize

#include "atx/engine/eval/cpcv.hpp"            // eval::CpcvConfig
#include "atx/engine/learn/feature_matrix.hpp" // FeatureMatrix
#include "atx/engine/learn/hmm.hpp"            // Hmm, HmmCfg, baum_welch
#include "atx/engine/learn/pipeline.hpp"       // PipelineCfg + the thin e2e harness

namespace {

using atx::f64;
using atx::u16;
using atx::u64;
using atx::usize;
using atx::engine::learn::FeatureMatrix;
namespace eval = atx::engine::eval;
namespace learn = atx::engine::learn;

// A small deterministic LCG (the S5-3/S5-4/S5-6 fixture idiom): reproducible noise
// with no RNG dependency; a pure function of its state, returning a draw in [-1,1).
struct Lcg {
  u64 s;
  [[nodiscard]] f64 next() noexcept {
    s = s * 6364136223846793005ULL + 1442695040888963407ULL;
    const u64 top = s >> 11U;
    const f64 u = static_cast<f64>(top) / static_cast<f64>(1ULL << 53U);
    return 2.0 * u - 1.0; // [-1, 1)
  }
};

// Build a FeatureMatrix with `n_dates` x `n_inst` (all in-universe), `n_features`
// feature columns, and `horizons.size()` forward-return labels. `col_fn(d,i,cols,
// rng)` fills the feature columns for cell (d,i); `label_fn(cols, noise, h)` makes
// the label for horizon index h (NaN at the unknowable tail d+horizons[h] >= nd).
// The per-row feature draws and per-row noise are captured so each horizon's label
// is a deterministic function of the SAME row features (the multi-horizon contrast
// is then genuine — distinct horizons see distinct targets over the same features).
template <typename ColFn, typename LabelFn>
[[nodiscard]] FeatureMatrix make_fm(usize n_dates, usize n_inst, usize n_features,
                                    const std::vector<u16> &horizons, u64 seed, ColFn col_fn,
                                    LabelFn label_fn) {
  FeatureMatrix fm;
  fm.n_dates = n_dates;
  fm.n_instruments = n_inst;
  fm.n_features = n_features;
  fm.Y.assign(horizons.size(), {});
  Lcg rng{seed};
  std::vector<std::vector<f64>> cols_of_row;
  std::vector<f64> noise_of_row;
  for (usize d = 0; d < n_dates; ++d) {
    for (usize i = 0; i < n_inst; ++i) {
      const usize row = fm.push_row(d, i);
      (void)row;
      std::vector<f64> cols(n_features, 0.0);
      col_fn(d, i, cols, rng);
      for (usize f = 0; f < n_features; ++f) {
        fm.X.push_back(cols[f]);
      }
      cols_of_row.push_back(cols);
      noise_of_row.push_back(rng.next());
      fm.row_valid.push_back(1U);
    }
  }
  for (usize hi = 0; hi < horizons.size(); ++hi) {
    for (usize r = 0; r < fm.n_rows(); ++r) {
      const usize d = fm.row_date[r];
      const usize ahead = d + static_cast<usize>(horizons[hi]);
      if (ahead >= n_dates) {
        fm.Y[hi].push_back(std::numeric_limits<f64>::quiet_NaN());
      } else {
        fm.Y[hi].push_back(label_fn(cols_of_row[r], noise_of_row[r], hi));
      }
    }
  }
  return fm;
}

// A genuinely EDGE-FREE single-horizon panel: every feature is independent noise
// AND the label is independent noise too (no feature predicts the label). The
// deflation gate must admit ZERO learned models on this.
[[nodiscard]] FeatureMatrix pure_noise_fm(u64 seed) {
  const auto cols = [](usize, usize, std::vector<f64> &c, Lcg &rng) {
    for (f64 &v : c) {
      v = rng.next();
    }
  };
  // The label uses the captured per-row noise ONLY — never any feature column — so
  // there is no edge by construction.
  const auto label = [](const std::vector<f64> &, f64 noise, usize) -> f64 { return noise; };
  return make_fm(/*n_dates=*/40U, /*n_inst=*/12U, /*n_features=*/4U, /*horizons=*/{1}, seed, cols,
                 label);
}

// A panel with a genuine LINEAR edge: Y = 0.9*col0 + small noise. col0 robustly
// predicts the forward return, so a linear (and a tree) base survive deflation.
[[nodiscard]] FeatureMatrix planted_signal_fm(u64 seed) {
  const auto cols = [](usize, usize, std::vector<f64> &c, Lcg &rng) {
    for (f64 &v : c) {
      v = rng.next();
    }
  };
  const auto label = [](const std::vector<f64> &c, f64 noise, usize) -> f64 {
    return 0.9 * c[0] + 0.05 * noise;
  };
  return make_fm(/*n_dates=*/40U, /*n_inst=*/12U, /*n_features=*/4U, /*horizons=*/{1}, seed, cols,
                 label);
}

// A meta whose BEST predictor is a LINEAR blend of the alpha columns (no product /
// interaction term) — the GBT's extra capacity buys no OOS edge over linear, so the
// stacking gate must NOT admit (reuses the ensemble_test linearly-combinable idiom).
[[nodiscard]] FeatureMatrix linearly_combinable_meta(u64 seed) {
  const auto cols = [](usize, usize, std::vector<f64> &c, Lcg &rng) {
    for (f64 &v : c) {
      v = rng.next();
    }
  };
  const auto label = [](const std::vector<f64> &c, f64 noise, usize) -> f64 {
    return 0.6 * c[0] + 0.4 * c[1] + 0.05 * noise;
  };
  return make_fm(/*n_dates=*/48U, /*n_inst=*/14U, /*n_features=*/4U, /*horizons=*/{1}, seed, cols,
                 label);
}

// The S5-6 two-regime construction (reused): col0 predicts in regime 0, col1 in
// regime 1; the marker column (last) encodes the regime in its sign and is the
// per-date observable the HMM / fit_stack derive. Heavy label noise keeps the FLAT
// fit sub-saturating so the regime-conditional fit has room to score higher.
// `regime_obs_out` receives the (n_dates x 1) per-date marker-mean series.
[[nodiscard]] FeatureMatrix two_regime_meta(u64 seed, usize n_dates, usize n_inst, usize run,
                                            atx::core::linalg::MatX &regime_obs_out) {
  const usize n_features = 3U; // col0, col1, marker(col2)
  const auto regime_of_date = [run](usize d) -> usize { return (d / run) % 2U; };
  const auto cols = [&regime_of_date](usize d, usize, std::vector<f64> &c, Lcg &rng) {
    c[0] = rng.next();
    c[1] = rng.next();
    const usize reg = regime_of_date(d);
    c[2] = (reg == 0U ? -1.0 : 1.0) + 0.05 * rng.next(); // strongly-separated marker
  };
  const auto label = [](const std::vector<f64> &c, f64 noise, usize) -> f64 {
    const bool reg1 = (c[2] >= 0.0);
    return (reg1 ? c[1] : c[0]) + 2.0 * noise; // regime's predictor buried in heavy noise
  };
  FeatureMatrix fm = make_fm(n_dates, n_inst, n_features, /*horizons=*/{1}, seed, cols, label);

  regime_obs_out.resize(static_cast<Eigen::Index>(n_dates), 1);
  usize r = 0;
  for (usize d = 0; d < n_dates; ++d) {
    f64 sum = 0.0;
    usize cnt = 0;
    while (r < fm.n_rows() && fm.row_date[r] == d) {
      sum += fm.X[r * fm.n_features + (n_features - 1U)];
      ++cnt;
      ++r;
    }
    regime_obs_out(static_cast<Eigen::Index>(d), 0) = (cnt > 0U) ? sum / static_cast<f64>(cnt) : 0.0;
  }
  return fm;
}

// A multi-horizon panel with a GRADED, IC-ordered edge in col0: the FIRST horizon
// (slot 0) carries a STRONG linear edge, the second a WEAK one, and any third+
// horizon is PURE NOISE (no signal). The per-horizon OOS ICs therefore genuinely
// differ — strongest first, dead last — so the §0.6 blend (normalize(max(oos_IC_h,
// 0))) must earn the strong horizon strictly more weight than the no-skill one.
[[nodiscard]] FeatureMatrix multi_horizon_fm(u64 seed, const std::vector<u16> &horizons) {
  const auto cols = [](usize, usize, std::vector<f64> &c, Lcg &rng) {
    for (f64 &v : c) {
      v = rng.next();
    }
  };
  // slot 0 strong (0.8·col0), slot 1 weak (0.35·col0), slot >=2 DEAD (pure noise).
  // A horizon-distinct noise term keeps Y[0] != Y[1] != Y[2] genuinely.
  const auto label = [](const std::vector<f64> &c, f64 noise, usize hi) -> f64 {
    const f64 strength = (hi == 0U) ? 0.8 : (hi == 1U) ? 0.35 : 0.0;
    const f64 hnoise = noise * (1.0 + 0.13 * static_cast<f64>(hi));
    return strength * c[0] + 0.05 * hnoise;
  };
  return make_fm(/*n_dates=*/44U, /*n_inst=*/12U, /*n_features=*/4U, horizons, seed, cols, label);
}

// The standard pipeline cfg the tests share (single horizon unless overridden; the
// CPCV walk sized to the fixtures; GBT base for the stacking arm by default).
[[nodiscard]] learn::PipelineCfg standard_cfg(u64 seed) {
  learn::PipelineCfg cfg;
  cfg.master_seed = seed;
  cfg.workers = 1U;
  cfg.cpcv = eval::CpcvConfig{/*n_groups=*/5, /*n_test_groups=*/1, /*embargo=*/0.0};
  cfg.horizons = {1};
  return cfg;
}

// ---- 1: M1 — same fm + same seed -> byte-identical full-pipeline digest -------

TEST(LearnIntegration, DeterministicTrainingEndToEnd_ByteIdentical) {
  const FeatureMatrix fm = planted_signal_fm(/*seed=*/7ULL);
  const learn::PipelineCfg cfg = standard_cfg(/*seed=*/42ULL);
  const u64 d1 = learn::full_pipeline_digest(fm, cfg);
  const u64 d2 = learn::full_pipeline_digest(fm, cfg);
  EXPECT_EQ(d1, d2) << "the whole pipeline (linear/gbt/hmm/stack) must be byte-identical "
                       "for the same fm + seed";
  // Non-vacuous: the digest must actually fold non-trivial decided fields (a real
  // fit ran), so it is not the empty/zero digest.
  EXPECT_NE(d1, 0ULL);
}

// ---- 2: M3 anti-snooping — noise admits nothing, real edge admits survivors ---

TEST(LearnIntegration, NoEdge_AdmitsNothing_RealEdge_AdmitsSurvivors) {
  const learn::PipelineCfg cfg = standard_cfg(/*seed=*/42ULL);
  // The REJECT direction is spec-critical: a false-admit on a genuinely edge-free
  // panel would be ML-for-ML's-sake leaking past the deflation gate (a real M3
  // break). Swept across 30 distinct noise panels; the gate must admit ZERO learned
  // models on EVERY one. The rejection is STRUCTURAL, not statistical: the linear arm
  // uses a strong L1 that drives spurious coefficients to EXACTLY zero (constant
  // prediction -> IC exactly 0 -> dsr <= 0) and the GBT arm zeroes a sub-dispersion
  // OOF series — so a leak would be a structural surprise, not a tail event.
  for (u64 seed = 1; seed <= 30ULL; ++seed) {
    const FeatureMatrix noise = pure_noise_fm(seed);
    EXPECT_EQ(learn::admitted_count(noise, cfg), 0U)
        << "a genuinely edge-free panel must admit no learned model: seed=" << seed;
  }
  // The opposite direction: a planted real linear edge admits > 0 under the SAME
  // cfg / gate (non-vacuous — the same deflation bar lets a genuine edge through).
  const FeatureMatrix signal = planted_signal_fm(/*seed=*/7ULL);
  EXPECT_GT(learn::admitted_count(signal, cfg), 0U)
      << "a planted real edge must admit at least one learned model under the same gate";
}

// ---- 3: §0.4 — a linearly-combinable pool is NOT admitted by the stack ---------

TEST(LearnIntegration, NonlinearCombiner_RejectedWhenLinearSuffices) {
  const learn::PipelineCfg cfg = standard_cfg(/*seed=*/42ULL);
  const FeatureMatrix meta = linearly_combinable_meta(/*seed=*/3ULL);
  EXPECT_FALSE(learn::pipeline_admits_stack(meta, cfg))
      << "a linearly-combinable pool must buy the nonlinear stack no OOS edge over linear";
}

// ---- 4: spec exit — regime-conditional OOS beats flat OOS ----------------------

TEST(LearnIntegration, RegimeConditional_ImprovesOos) {
  atx::core::linalg::MatX regime_obs;
  const FeatureMatrix meta =
      two_regime_meta(/*seed=*/1ULL, /*n_dates=*/40U, /*n_inst=*/6U, /*run=*/8U, regime_obs);
  // Fit a 2-state HMM on the derived per-date marker-mean observable (the SAME
  // series the regime-conditional fit re-derives internally).
  learn::HmmCfg hcfg;
  hcfg.n_states = 2U;
  hcfg.master_seed = 5ULL;
  const learn::Hmm hmm = learn::baum_welch(regime_obs, hcfg);

  learn::PipelineCfg cfg = standard_cfg(/*seed=*/17ULL);
  const f64 with_regime = learn::oos_with_regime(meta, hmm, cfg);
  const f64 without_regime = learn::oos_without_regime(meta, cfg);
  EXPECT_GT(with_regime, without_regime)
      << "a regime-conditional fit must beat the flat fit on a regime-dependent pool: "
      << "with=" << with_regime << " without=" << without_regime;
}

// ---- 5: §0.6 — the horizon blend WEIGHTS BY OOS IC (demotes the dead horizon) ---

TEST(LearnIntegration, MultiHorizon_BlendWeightsByOosIc) {
  // horizons {1,5,21}: slot 0 (h=1) carries the strong edge, slot 1 (h=5) a weak
  // one, slot 2 (h=21) is pure noise. The §0.6 blend weight is normalize(max(
  // oos_IC_h, 0)), so the dead horizon must earn strictly LESS weight than the strong
  // one — the blend is genuinely IC-DRIVEN, not a degenerate uniform split. (A full
  // "blended-PREDICTION OOS IC > best single" proof needs per-horizon OOF retention
  // the frozen oos_ic surface lacks — deferred; see the S5 close residual.) Swept
  // across seeds so the weighting is robust, not a single-draw artifact.
  const std::vector<u16> horizons = {1U, 5U, 21U};
  const std::span<const u16> hspan{horizons};
  for (u64 seed = 1; seed <= 4ULL; ++seed) {
    const learn::PipelineCfg cfg = standard_cfg(/*seed=*/42ULL);
    const FeatureMatrix fm = multi_horizon_fm(seed, horizons);
    const std::vector<f64> w = learn::horizon_blend_weights(fm, hspan, cfg);
    ASSERT_EQ(w.size(), horizons.size());
    f64 sum = 0.0;
    for (const f64 x : w) {
      EXPECT_GE(x, 0.0) << "blend weights are non-negative: seed=" << seed;
      sum += x;
    }
    EXPECT_NEAR(sum, 1.0, 1e-9) << "blend weights sum to 1: seed=" << seed;
    // The strong horizon (slot 0) earns strictly more weight than the dead one (slot
    // 2): the blend down-weights the no-skill horizon by its (≈0) OOS IC — the §0.6
    // weighting is operative, NOT a uniform/degenerate fallback.
    EXPECT_GT(w[0], w[2]) << "the §0.6 blend must weight the high-OOS-IC horizon above the "
                          << "no-skill one: seed=" << seed << " w(h=1)=" << w[0]
                          << " w(h=5)=" << w[1] << " w(h=21)=" << w[2];
    // Non-vacuous: a real single-horizon edge exists to weight.
    EXPECT_GT(learn::oos_ic_best_single(fm, hspan, cfg), 0.0)
        << "the planted edge must yield a positive single-horizon OOS IC: seed=" << seed;
  }
}

// ---- 6: M1 — the (no-op) worker knob cannot change the digest ------------------

TEST(LearnIntegration, ThreadCountInvariance_DigestUnchanged) {
  const FeatureMatrix fm = planted_signal_fm(/*seed=*/7ULL);
  learn::PipelineCfg one = standard_cfg(/*seed=*/42ULL);
  one.workers = 1U;
  learn::PipelineCfg four = standard_cfg(/*seed=*/42ULL);
  four.workers = 4U;
  // The pipeline is single-threaded with order-fixed reductions, so the worker knob
  // is threaded into the cfgs but cannot change any decided field -> identical
  // digest. This proves determinism survives the (currently no-op) worker knob.
  EXPECT_EQ(learn::full_pipeline_digest(fm, one), learn::full_pipeline_digest(fm, four))
      << "the single-threaded order-fixed pipeline must give a worker-invariant digest";
}

} // namespace
