// atx::engine::learn — nonlinear STACKING mega-combiner tests (S5-6).
//
// Suite Ensemble — the load-bearing semantics of the alpha-of-alphas stacking
// combiner (§0.4 / §4.5 / M1 / M3 / M6):
//
//   1. NoNonlinearEdge_RejectedVsLinear (M3/§0.4) — a meta-matrix whose BEST
//      predictor of Y is a LINEAR blend of the alpha columns (no interactions):
//      the nonlinear (GBT) base buys no OOS IC over a pure-linear base, so the
//      stack is NOT admitted. reason == AdmitKind::RejectFitness.
//   2. GenuineNonlinearEdge_AdmittedOverLinear (M3 non-vacuous) — a meta-matrix
//      whose Y depends on a PRODUCT of two alpha columns with NO marginal linear
//      signal: the GBT's OOS IC beats the linear base, dsr > 0, so the stack IS
//      admitted. Non-vacuous opposite of test 1 (different fixture, genuine edge).
//   3. RegimeConditional_ImprovesOosOnRegimeFixture (§4.5) — a meta-matrix whose
//      OPTIMAL alpha differs by regime (regime 0: col0 predicts; regime 1: col1
//      predicts), persistent runs. A regime-conditional fit (a separate nonlinear
//      base per PIT-argmax regime) scores higher OOS dsr than the flat fit.
//   4. Admitted_PlugsIntoLibrary_AsLearnedAlpha (M6) — build a library candidate
//      from an admitted stack; its prov.expr_source starts "learned:stack"; a real
//      library::Library admit on a throwaway dir returns a non-Duplicate verdict.
//   5. SameSeed_ByteIdenticalVerdict (M1) — two fit_stack with the same seed give
//      the same verdict_hash (the decided numeric fields are deterministic).
//
// Fixtures build a `FeatureMatrix meta` DIRECTLY (each column f = pool alpha f's
// position cell; label Y[h] = the instrument's forward return), mirroring the S5-4
// gbt_test make_fm pattern. A local deterministic LCG carries the reproducible
// noise (no std::rand). Naming: Subject_Condition_Expected.

#include <filesystem>   // per-test temp directory (the library is rooted at a dir)
#include <limits>       // std::numeric_limits (tail NaN label)
#include <string>       // std::string (tmp dir path, expr_source check)
#include <system_error> // std::error_code (tmpdir remove_all/create_directories)
#include <vector>

#include <Eigen/Dense> // Eigen::Index

#include <gtest/gtest.h>

#include "atx/core/linalg/linalg.hpp" // atx::core::linalg::MatX (regime observable)
#include "atx/core/types.hpp"         // f64, u16, u32, u64, usize

#include "atx/engine/combine/gate.hpp"         // combine::GateConfig, AlphaGate
#include "atx/engine/eval/cpcv.hpp"            // eval::CpcvConfig
#include "atx/engine/learn/ensemble.hpp"       // StackingCfg, StackingVerdict, fit_stack, ...
#include "atx/engine/learn/feature_matrix.hpp" // FeatureMatrix
#include "atx/engine/learn/hmm.hpp"            // Hmm, HmmCfg, baum_welch
#include "atx/engine/library/library.hpp"      // library::Library, AlphaCandidate, AdmitKind

namespace {

using atx::f64;
using atx::u16;
using atx::u32;
using atx::u64;
using atx::usize;
using atx::engine::learn::FeatureMatrix;
namespace combine = atx::engine::combine;
namespace eval = atx::engine::eval;
namespace learn = atx::engine::learn;
namespace library = atx::engine::library;

// A small deterministic LCG (the S5-3/S5-4 fixture pattern): reproducible "noise"
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

// Build a meta FeatureMatrix with `n_dates` dates x `n_inst` instruments (all
// in-universe / valid), `n_features` alpha columns, single horizon 1. `col_fn(d,
// i, cols, rng)` fills the `n_features` alpha-position columns for cell (d,i); the
// label for horizon h is `label_fn(cols, noise)` (NaN at the unknowable tail).
template <typename ColFn, typename LabelFn>
[[nodiscard]] FeatureMatrix make_meta(usize n_dates, usize n_inst, usize n_features, u64 seed,
                                      ColFn col_fn, LabelFn label_fn) {
  FeatureMatrix fm;
  fm.n_dates = n_dates;
  fm.n_instruments = n_inst;
  fm.n_features = n_features;
  fm.Y.assign(1U, {});
  Lcg rng{seed};
  std::vector<std::vector<f64>> cols_of_row; // [row] -> the n_features column cells
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
  for (usize r = 0; r < fm.n_rows(); ++r) {
    const usize d = fm.row_date[r];
    if (d + 1U >= n_dates) {
      fm.Y[0].push_back(std::numeric_limits<f64>::quiet_NaN());
    } else {
      fm.Y[0].push_back(label_fn(cols_of_row[r], noise_of_row[r]));
    }
  }
  return fm;
}

// A meta whose BEST predictor of Y is a LINEAR blend of the alpha columns: each
// column is independent noise; Y = 0.6*col0 + 0.4*col1 + small noise. No product /
// interaction term — a linear base captures everything; the GBT's extra capacity
// can only fit noise and so does NOT beat linear OOS.
[[nodiscard]] FeatureMatrix linearly_combinable_meta(u64 seed) {
  const auto cols = [](usize, usize, std::vector<f64> &c, Lcg &rng) {
    for (f64 &v : c) {
      v = rng.next();
    }
  };
  const auto label = [](const std::vector<f64> &c, f64 noise) -> f64 {
    return 0.6 * c[0] + 0.4 * c[1] + 0.05 * noise;
  };
  return make_meta(/*n_dates=*/48U, /*n_inst=*/14U, /*n_features=*/4U, seed, cols, label);
}

// A meta whose Y depends on the PRODUCT of two alpha columns with NO marginal
// linear signal: Y = sign(col0)*sign(col1) + small noise. col0 and col1 are each
// uncorrelated with Y (a linear base sees nothing); only their interaction carries
// the signal, which the GBT captures.
[[nodiscard]] FeatureMatrix nonlinear_interaction_meta(u64 seed) {
  const auto cols = [](usize, usize, std::vector<f64> &c, Lcg &rng) {
    for (f64 &v : c) {
      v = rng.next();
    }
  };
  const auto label = [](const std::vector<f64> &c, f64 noise) -> f64 {
    const f64 s0 = (c[0] >= 0.0) ? 1.0 : -1.0;
    const f64 s1 = (c[1] >= 0.0) ? 1.0 : -1.0;
    return s0 * s1 + 0.10 * noise;
  };
  return make_meta(/*n_dates=*/48U, /*n_inst=*/14U, /*n_features=*/4U, seed, cols, label);
}

// A two-regime meta + the Hmm fit on the SAME derived regime observable fit_stack
// uses (the per-date cross-sectional MEAN of the marker column, col index
// n_features-1). Persistent runs of length `run` flip which alpha predicts Y:
//   regime 0 (marker ~ -1): Y =  col0 + small noise   (col1 irrelevant)
//   regime 1 (marker ~ +1): Y =  col1 + small noise   (col0 irrelevant)
// A FLAT nonlinear base must compromise across both regimes; a regime-conditional
// base fits each regime's true predictor, so its OOS dsr is higher. Returns the
// meta; `regime_obs_out` receives the (n_dates x 1) per-date marker-mean series
// the Hmm is fit on (and which fit_stack re-derives identically).
[[nodiscard]] FeatureMatrix two_regime_meta(u64 seed, usize n_dates, usize n_inst,
                                            usize run, atx::core::linalg::MatX &regime_obs_out) {
  const usize n_features = 3U; // col0, col1, marker(col2)
  // Per-date regime label: alternating runs of `run` dates (0,0,..,1,1,..,0,..).
  const auto regime_of_date = [run](usize d) -> usize { return (d / run) % 2U; };
  const auto cols = [&regime_of_date](usize d, usize, std::vector<f64> &c, Lcg &rng) {
    c[0] = rng.next();
    c[1] = rng.next();
    const usize reg = regime_of_date(d);
    // The marker column is a strongly-separated, low-noise regime signal: ~ -1 in
    // regime 0, ~ +1 in regime 1 (so the HMM cleanly recovers the regime).
    c[2] = (reg == 0U ? -1.0 : 1.0) + 0.05 * rng.next();
  };
  // The label depends on the regime's predictor column, BURIED IN HEAVY NOISE so a
  // FLAT model (which must compromise across both regimes) leaves real OOS skill on
  // the table while a regime-conditional fit recovers each regime's true predictor.
  // The noise scale is deliberately large (2.0) — the regime separation is what is
  // tuned UP, never the test's bar down: a flat fit's per-date OOF IC bounces around
  // (a modest, sub-saturating DSR) while the regime fit's stays high. The marker
  // column (c[2]) encodes the regime in its sign, matching col_fn above; the label
  // can't see the date d directly, so it reads the regime off the marker.
  const auto label = [](const std::vector<f64> &c, f64 noise) -> f64 {
    const bool reg1 = (c[2] >= 0.0);
    return (reg1 ? c[1] : c[0]) + 2.0 * noise;
  };
  FeatureMatrix fm =
      make_meta(n_dates, n_inst, n_features, seed, cols, label);

  // Build the per-date marker-mean observable the Hmm is fit on (the SAME series
  // fit_stack derives): for each date, the cross-sectional mean of the marker
  // column over that date's rows.
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
    regime_obs_out(static_cast<Eigen::Index>(d), 0) =
        (cnt > 0U) ? sum / static_cast<f64>(cnt) : 0.0;
  }
  return fm;
}

// The standard stacking cfg the tests share (single horizon, deterministic seed,
// a CPCV walk sized to the fixtures). GBT base by default.
[[nodiscard]] learn::StackingCfg standard_cfg() {
  learn::StackingCfg cfg;
  cfg.base = learn::StackingCfg::Base::Gbt;
  cfg.cpcv = eval::CpcvConfig{/*n_groups=*/5, /*n_test_groups=*/1, /*embargo=*/0.0};
  cfg.master_seed = 42ULL;
  cfg.horizons = {1};
  return cfg;
}

// ---- 1: §0.4 / M3 — a linearly-combinable pool is rejected vs linear ---------

TEST(Ensemble, NoNonlinearEdge_RejectedVsLinear) {
  // The REJECT direction is the spec-critical one: a false-admit on a purely-linear
  // pool would be "ML-for-ML's-sake" leaking past the §0.4 gate (a real M3 break).
  // So this is swept across 20 distinct linearly-combinable panels — the gate must
  // reject EVERY one (the GBT's extra capacity buys no OOS IC over a linear base on
  // genuinely linear structure).
  for (u64 seed = 1; seed <= 20ULL; ++seed) {
    const FeatureMatrix meta = linearly_combinable_meta(seed);
    const learn::StackingVerdict v = learn::fit_stack(meta, /*regime=*/nullptr, standard_cfg());
    EXPECT_FALSE(v.admitted) << "a linearly-combinable pool must buy the GBT no OOS edge: seed="
                             << seed << " nl_ic=" << v.oos_ic_nonlinear
                             << " lin_ic=" << v.oos_ic_linear << " nl_dsr=" << v.oos_dsr_nonlinear;
    EXPECT_EQ(v.reason, learn::AdmitKind::RejectFitness) << "seed=" << seed;
  }
}

// ---- 2: M3 non-vacuous — a genuine interaction pool is admitted over linear --

TEST(Ensemble, GenuineNonlinearEdge_AdmittedOverLinear) {
  const FeatureMatrix meta = nonlinear_interaction_meta(/*seed=*/7ULL);
  const learn::StackingVerdict v = learn::fit_stack(meta, /*regime=*/nullptr, standard_cfg());
  EXPECT_TRUE(v.admitted) << "a genuine nonlinear interaction must beat linear: "
                          << "nl_ic=" << v.oos_ic_nonlinear << " lin_ic=" << v.oos_ic_linear
                          << " nl_dsr=" << v.oos_dsr_nonlinear;
  EXPECT_GT(v.oos_ic_nonlinear, v.oos_ic_linear);
  EXPECT_GT(v.oos_dsr_nonlinear, 0.0);
}

// ---- 3: §4.5 — regime-conditional beats flat on a regime fixture -------------

TEST(Ensemble, RegimeConditional_ImprovesOosOnRegimeFixture) {
  // A modest panel (40 dates x 6 instruments, run length 8) with HEAVY label noise:
  // few instruments => the per-date OOF IC is noisy, so the flat fit's DSR stays
  // BELOW saturation, leaving room for the regime-conditional fit to score higher.
  // Swept across distinct data seeds: the regime fit must beat flat on EVERY one —
  // and since DSR is capped at 1.0, `regime_dsr > flat_dsr` STRICTLY forces flat to
  // be sub-saturating (a 1.0-vs-1.0 comparison would fail), so the improvement is
  // genuine, not a saturation artifact. This fixture's regime separation is what is
  // tuned, never the test bar.
  for (u64 seed = 1; seed <= 6ULL; ++seed) {
    atx::core::linalg::MatX regime_obs;
    const FeatureMatrix meta =
        two_regime_meta(seed, /*n_dates=*/40U, /*n_inst=*/6U, /*run=*/8U, regime_obs);
    // Fit a 2-state HMM on the derived per-date marker-mean observable.
    learn::HmmCfg hcfg;
    hcfg.n_states = 2U;
    hcfg.master_seed = 5ULL;
    const learn::Hmm hmm = learn::baum_welch(regime_obs, hcfg);

    learn::StackingCfg cfg = standard_cfg();
    cfg.master_seed = 17ULL;

    const learn::StackingVerdict v_flat = learn::fit_stack(meta, /*regime=*/nullptr, cfg);
    const learn::StackingVerdict v_regime = learn::fit_stack(meta, &hmm, cfg);
    EXPECT_GT(v_regime.oos_dsr_nonlinear, v_flat.oos_dsr_nonlinear)
        << "regime-conditional OOS dsr must beat flat: seed=" << seed
        << " regime=" << v_regime.oos_dsr_nonlinear << " flat=" << v_flat.oos_dsr_nonlinear
        << " (regime_ic=" << v_regime.oos_ic_nonlinear << " flat_ic=" << v_flat.oos_ic_nonlinear
        << ")";
  }
}

// ---- 4: M6 — an admitted stack plugs into the library as a learned alpha -----

TEST(Ensemble, Admitted_PlugsIntoLibrary_AsLearnedAlpha) {
  const FeatureMatrix meta = nonlinear_interaction_meta(/*seed=*/7ULL);
  const learn::StackingVerdict v = learn::fit_stack(meta, /*regime=*/nullptr, standard_cfg());
  ASSERT_TRUE(v.admitted) << "the M6 fixture must be admitted to build a candidate";

  // Synthesize the admit candidate from the deployed nonlinear model's per-(date,
  // inst) predictions. The owning struct keeps the pnl/pos_flat buffers alive past
  // the admit() call (AlphaCandidate spans are non-owning).
  learn::StackCandidate cand = learn::stack_to_candidate(v, meta, standard_cfg());
  EXPECT_EQ(cand.candidate.prov.expr_source.rfind("learned:stack", 0), 0U)
      << "expr_source=" << cand.candidate.prov.expr_source;

  // A real library on a throwaway, run-fresh temp dir (OS temp — OUTSIDE the repo,
  // so nothing it writes can ever land as a committable git path). The dir is
  // remove_all'd then recreated so the admit runs against an EMPTY library.
  const std::filesystem::path dir =
      std::filesystem::temp_directory_path() / "atx_s5_6_ensemble_admit";
  std::error_code ec;
  std::filesystem::remove_all(dir, ec);
  std::filesystem::create_directories(dir, ec);
  library::Library lib =
      library::Library::open(dir.string(), combine::GateConfig{}, std::vector<u64>{42ULL});
  const library::AdmitVerdict verdict = lib.admit(cand.candidate, combine::AlphaGate{combine::GateConfig{}});
  EXPECT_NE(verdict.kind, library::AdmitKind::Duplicate)
      << "a fresh learned-stack candidate must not dedup against an empty library";

  // End-to-end M6: under a PERMISSIVE gate (floors lowered, correlation bound
  // relaxed) the very same learned-stack candidate must ACTUALLY ADMIT into a fresh
  // empty library — proving a learned model enters the pool through the identical
  // path a mined alpha does, not merely that it isn't a duplicate.
  const std::filesystem::path dir2 =
      std::filesystem::temp_directory_path() / "atx_s5_6_ensemble_admit2";
  std::filesystem::remove_all(dir2, ec);
  std::filesystem::create_directories(dir2, ec);
  combine::GateConfig permissive;
  permissive.min_sharpe = -1.0e9;
  permissive.min_fitness = -1.0e9;
  permissive.max_turnover = 1.0e9;
  permissive.max_pool_corr = 1.0;
  library::Library lib2 = library::Library::open(dir2.string(), permissive, std::vector<u64>{42ULL});
  const library::AdmitVerdict accepted = lib2.admit(cand.candidate, combine::AlphaGate{permissive});
  EXPECT_EQ(accepted.kind, library::AdmitKind::Accept)
      << "a learned-stack candidate must admit end-to-end under a permissive gate (kind="
      << static_cast<u32>(accepted.kind) << ")";
}

// ---- 5: M1 — same seed gives a byte-identical verdict hash -------------------

TEST(Ensemble, SameSeed_ByteIdenticalVerdict) {
  const FeatureMatrix meta = nonlinear_interaction_meta(/*seed=*/7ULL);
  const learn::StackingVerdict v1 = learn::fit_stack(meta, /*regime=*/nullptr, standard_cfg());
  const learn::StackingVerdict v2 = learn::fit_stack(meta, /*regime=*/nullptr, standard_cfg());
  EXPECT_EQ(v1.verdict_hash, v2.verdict_hash)
      << "same seed must produce a byte-identical verdict";
  // Sanity: the decided fields themselves agree (the hash is over them).
  EXPECT_EQ(v1.admitted, v2.admitted);
  EXPECT_EQ(v1.oos_ic_nonlinear, v2.oos_ic_nonlinear);
  EXPECT_EQ(v1.oos_dsr_nonlinear, v2.oos_dsr_nonlinear);
}

} // namespace
