// atx::engine::learn — gate_nn_sweep (p2 S5-4) tests.
//
// TDD pins for the NN-alpha DEFLATION + PBO admission gate. The unit REUSES
// eval::deflated_sharpe + eval::pbo_cscv verbatim (NO new gate math) and adds the
// two things NN scale needs: (a) the architecture x seed SWEEP trial-count
// aggregation so the deflation N is the FULL sweep (R4 — the single biggest NN
// snooping risk), and (b) the planted-signal-admit / pure-noise-reject behaviour
// (R4/R8).
//
// These tests assert exactly the load-bearing properties:
//   - planted-signal admit: a clean-skill sweep -> admit, dsr > 0, pbo < 0.30, and
//     n_trials == Σ candidate.trial_count (> any single candidate's count).
//   - pure-noise reject: a zero-mean noisy sweep -> NOT admitted (the high-N
//     deflation drives dsr <= 0 and/or the CSCV PBO >= 0.30).
//   - trial-count honesty (R4): n_trials == Σ trial_count, and gating the SAME
//     winner alone (N = its own trial_count) yields a STRICTLY HIGHER dsr than
//     gating it within the sweep (N = n_trials) — the sweep deflation is stricter.
//   - errors: 1 candidate / mismatched series lengths / odd n_splits -> Err.
//   - real-fit wiring: ONE test fits a real fit_tcn + fit_gru sweep on a planted
//     SequenceTensor and runs the gate end-to-end (proves the LearnedModel fields
//     the gate reads are the ones the trainers fill).
//
// Suite name is `LearnNnGate`. FIXTURE STRATEGY (documented per the unit spec):
//   * Most tests construct LearnedModel SHELLS DIRECTLY with hand-set
//     oos_score_series + trial_count — the gate reads ONLY those two fields, so
//     this exercises the gate logic deterministically and fast (the point of S5-4).
//   * RealFitSweep_GatesEndToEnd is the ONE test that drives a genuine fit_tcn /
//     fit_gru sweep (proving the wiring); all the rest use hand-built series.

#include <optional> // std::nullopt (direct_dsr's single-stream variance path)
#include <span>
#include <vector>

#include <gtest/gtest.h>

#include "atx/core/error.hpp"  // ErrorCode
#include "atx/core/random.hpp" // Xoshiro256pp
#include "atx/core/types.hpp"  // f64, u16, u64, u8, usize

#include "atx/engine/eval/deflated_sharpe.hpp"    // eval::deflated_sharpe (the trial-count pin)
#include "atx/engine/eval/stats_ext.hpp"          // eval::mean_std_pop, skewness, excess_kurtosis
#include "atx/engine/learn/learned_source.hpp"    // LearnedModel, ModelKind
#include "atx/engine/learn/nn_gate.hpp"           // gate_nn_sweep, NnGateCfg, NnGateResult
#include "atx/engine/learn/sequence_features.hpp" // SequenceTensor
#include "atx/engine/learn/tcn_alpha.hpp"         // fit_tcn, fit_gru (the real-fit wiring test)

namespace {

using atx::f64;
using atx::u16;
using atx::u64;
using atx::u8;
using atx::usize;
namespace eval = atx::engine::eval; // mean_std_pop / skewness / excess_kurtosis / deflated_sharpe
using atx::engine::learn::gate_nn_sweep;
using atx::engine::learn::GruAlphaCfg;
using atx::engine::learn::LearnedModel;
using atx::engine::learn::ModelKind;
using atx::engine::learn::NnGateCfg;
using atx::engine::learn::NnGateResult;
using atx::engine::learn::SequenceTensor;
using atx::engine::learn::TcnAlphaCfg;

// ---------------------------------------------------------------------------
//  A LearnedModel SHELL carrying only what the gate reads: an OOS skill series
//  + a deflation trial_count. kind is NN (Tcn) to be representative; no other
//  field matters to gate_nn_sweep.
// ---------------------------------------------------------------------------
LearnedModel shell(std::vector<f64> series, usize trial_count) {
  LearnedModel m;
  m.kind = ModelKind::Tcn;
  m.oos_score_series = std::move(series);
  m.trial_count = trial_count;
  return m;
}

// A clean, positive-mean OOS skill series of length T (a candidate with genuine
// edge): a mild upward drift + small seeded jitter so std > 0 and skew/kurt are
// well-defined. mean is comfortably positive relative to its std.
std::vector<f64> skilled_series(usize T, u64 seed, f64 level) {
  atx::core::Xoshiro256pp rng{seed};
  std::vector<f64> s;
  s.reserve(T);
  for (usize t = 0; t < T; ++t) {
    s.push_back(level + 0.02 * rng.normal());
  }
  return s;
}

// A zero-mean, signal-free noise series of length T (a candidate with NO edge).
std::vector<f64> noise_series(usize T, u64 seed) {
  atx::core::Xoshiro256pp rng{seed};
  std::vector<f64> s;
  s.reserve(T);
  for (usize t = 0; t < T; ++t) {
    s.push_back(rng.normal()); // mean ~0, no persistent direction
  }
  return s;
}

// A direct DSR on a series with an explicit trial count N (mirrors the gate's
// winner-DSR formula and oos_deflated_sharpe exactly) — used to PIN the
// sweep-vs-winner-only deflation inequality (R4).
f64 direct_dsr(const std::vector<f64> &series, usize N) {
  const eval::MeanStd ms = eval::mean_std_pop(std::span<const f64>{series});
  if (series.size() < 2U || ms.std == 0.0) {
    return 0.0;
  }
  const f64 sr = ms.mean / ms.std;
  const f64 skew = eval::skewness(std::span<const f64>{series});
  const f64 exkurt = eval::excess_kurtosis(std::span<const f64>{series});
  return eval::deflated_sharpe(sr, series.size(), skew, exkurt, N, std::nullopt).dsr;
}

constexpr usize kT = 24; // OOS-series length (>= n_splits, even-divisible)

// =====================================================================
//  Planted-signal admit — a sweep of clearly-skilled candidates clears BOTH
//  bars and the deflation N is the WHOLE sweep.
// =====================================================================
TEST(LearnNnGate, PlantedSignal_Admits) {
  // Three architecture-x-seed candidates, each a strong, persistent positive-skill
  // series (a level well above its jitter so the per-period Sharpe is large). They
  // share a near-common drift so the IS-best is OOS-good (low PBO).
  std::vector<LearnedModel> cands;
  cands.push_back(shell(skilled_series(kT, 101, 0.30), 50));
  cands.push_back(shell(skilled_series(kT, 202, 0.28), 40));
  cands.push_back(shell(skilled_series(kT, 303, 0.32), 60));

  NnGateCfg cfg; // defaults: n_splits 8, dsr_min 0.0, pbo_max 0.30
  const auto r = gate_nn_sweep(std::span<const LearnedModel>{cands}, cfg);
  ASSERT_TRUE(r.has_value()) << r.error().to_string();
  const NnGateResult &g = *r;

  EXPECT_TRUE(g.admit) << "a clean-skill sweep must clear both the DSR and PBO bars";
  EXPECT_GT(g.dsr, 0.0) << "the winner's sweep-deflated DSR must be positive";
  EXPECT_LT(g.pbo, 0.30) << "a persistent-edge sweep must have low CSCV PBO";

  // n_trials is the FULL sweep, strictly greater than any single candidate's count.
  EXPECT_EQ(g.n_trials, 50U + 40U + 60U);
  EXPECT_GT(g.n_trials, 60U) << "the deflation N is the SUM, not the max, of trial counts";
  // The winner is the highest-mean series (candidate 2, level 0.32).
  EXPECT_EQ(g.winner, 2U);
}

// =====================================================================
//  Pure-noise reject — a sweep of signal-free series is NOT admitted (either the
//  high-N deflation kills the DSR or the CSCV PBO is at/above the bar).
// =====================================================================
TEST(LearnNnGate, PureNoise_Rejects) {
  // Each candidate is a signal-free noise series; give them large trial counts so
  // the honest deflation N is big and the DSR bar is high. With no persistent edge
  // the IS-best lands at a ~random OOS rank, so the CSCV PBO sits near 0.5 — well
  // above the 0.30 bar — and the sweep is rejected. (The winner's noisy Sharpe is
  // also tiny against SR*_600, so even when its DSR is marginally positive by chance
  // the PBO channel still rejects — the gate admits ONLY when BOTH bars clear.)
  std::vector<LearnedModel> cands;
  cands.push_back(shell(noise_series(kT, 11), 200));
  cands.push_back(shell(noise_series(kT, 22), 200));
  cands.push_back(shell(noise_series(kT, 33), 200));

  NnGateCfg cfg;
  const auto r = gate_nn_sweep(std::span<const LearnedModel>{cands}, cfg);
  ASSERT_TRUE(r.has_value()) << r.error().to_string();
  const NnGateResult &g = *r;

  EXPECT_FALSE(g.admit) << "a signal-free sweep must be rejected";
  // Pin the rejection reason explicitly (the spec's OR: a tiny noisy Sharpe fails
  // the sweep-N deflation, OR the IS-best is no better than the OOS median). At
  // least ONE bar must be breached — and for genuine noise the PBO bar is.
  EXPECT_TRUE(g.dsr <= cfg.dsr_min || g.pbo >= cfg.pbo_max)
      << "pure noise must breach the DSR floor and/or the PBO bar (dsr=" << g.dsr
      << ", pbo=" << g.pbo << ")";
  EXPECT_GE(g.pbo, cfg.pbo_max) << "a signal-free sweep's CSCV PBO must sit at/above the bar";
  EXPECT_EQ(g.n_trials, 600U);
}

// =====================================================================
//  Trial-count honesty (R4) — THE central pin. n_trials is the SUM, and gating the
//  winner ALONE (N = its own trial_count) gives a STRICTLY HIGHER dsr than gating
//  it within the sweep (N = n_trials): the sweep deflation is strictly stricter.
// =====================================================================
TEST(LearnNnGate, TrialCountHonesty_SweepDeflationStricterThanWinnerAlone) {
  // A clear winner (highest mean) with a modest own trial_count, swept alongside
  // two other candidates whose counts make the sweep N much larger.
  std::vector<f64> win = skilled_series(kT, 777, 0.22);
  const usize win_own_count = 10;
  std::vector<LearnedModel> cands;
  cands.push_back(shell(win, win_own_count));        // candidate 0: the winner
  cands.push_back(shell(skilled_series(kT, 888, 0.05), 90));
  cands.push_back(shell(skilled_series(kT, 999, 0.04), 100));

  NnGateCfg cfg;
  const auto r = gate_nn_sweep(std::span<const LearnedModel>{cands}, cfg);
  ASSERT_TRUE(r.has_value()) << r.error().to_string();
  const NnGateResult &g = *r;

  // (a) n_trials is exactly the sum of every candidate's trial_count.
  EXPECT_EQ(g.n_trials, win_own_count + 90U + 100U);
  EXPECT_EQ(g.winner, 0U) << "candidate 0 has the highest mean OOS skill";

  // (b) The gate's winner DSR uses N = n_trials. A direct DSR of the SAME winner
  //     series at N = its OWN (smaller) count must be STRICTLY HIGHER — i.e. the
  //     sweep deflation is strictly stricter (the R4 honesty property).
  const f64 dsr_sweep = direct_dsr(win, g.n_trials);
  const f64 dsr_winner_only = direct_dsr(win, win_own_count);
  EXPECT_GT(dsr_winner_only, dsr_sweep)
      << "deflating by the winner's own count overstates skill vs the full sweep N";
  // The gate must report the SWEEP-N (stricter) value, not the winner-only value.
  EXPECT_NEAR(g.dsr, dsr_sweep, 1e-12) << "the gate must deflate by the full sweep N";
  EXPECT_LE(g.dsr, dsr_winner_only) << "sweep deflation is never looser than winner-only";
}

// =====================================================================
//  Zero trial_count is counted as one (R4 conservative floor) — a candidate that
//  reports trial_count 0 (a fit still happened) must not UNDER-count N.
// =====================================================================
TEST(LearnNnGate, ZeroTrialCount_CountedAsOne) {
  std::vector<LearnedModel> cands;
  cands.push_back(shell(skilled_series(kT, 1, 0.2), 0)); // 0 -> counted as 1
  cands.push_back(shell(skilled_series(kT, 2, 0.2), 0)); // 0 -> counted as 1
  cands.push_back(shell(skilled_series(kT, 3, 0.2), 5));

  const auto r = gate_nn_sweep(std::span<const LearnedModel>{cands}, NnGateCfg{});
  ASSERT_TRUE(r.has_value()) << r.error().to_string();
  EXPECT_EQ(r->n_trials, 1U + 1U + 5U) << "a 0 trial_count must floor to 1, never under-count N";
}

// =====================================================================
//  Errors.
// =====================================================================
TEST(LearnNnGate, SingleCandidate_ReturnsInvalidArgument) {
  std::vector<LearnedModel> cands;
  cands.push_back(shell(skilled_series(kT, 5, 0.2), 10));
  const auto r = gate_nn_sweep(std::span<const LearnedModel>{cands}, NnGateCfg{});
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error().code(), atx::core::ErrorCode::InvalidArgument);
}

TEST(LearnNnGate, MismatchedSeriesLengths_ReturnsInvalidArgument) {
  std::vector<LearnedModel> cands;
  cands.push_back(shell(skilled_series(kT, 5, 0.2), 10));
  cands.push_back(shell(skilled_series(kT + 4, 6, 0.2), 10)); // different T
  const auto r = gate_nn_sweep(std::span<const LearnedModel>{cands}, NnGateCfg{});
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error().code(), atx::core::ErrorCode::InvalidArgument);
}

TEST(LearnNnGate, EmptySeries_ReturnsInvalidArgument) {
  std::vector<LearnedModel> cands;
  cands.push_back(shell({}, 10)); // empty OOS series
  cands.push_back(shell({}, 10));
  const auto r = gate_nn_sweep(std::span<const LearnedModel>{cands}, NnGateCfg{});
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error().code(), atx::core::ErrorCode::InvalidArgument);
}

TEST(LearnNnGate, OddNSplits_ReturnsInvalidArgument) {
  std::vector<LearnedModel> cands;
  cands.push_back(shell(skilled_series(kT, 5, 0.2), 10));
  cands.push_back(shell(skilled_series(kT, 6, 0.2), 10));
  NnGateCfg cfg;
  cfg.n_splits = 7; // odd -> pbo_cscv_checked rejects
  const auto r = gate_nn_sweep(std::span<const LearnedModel>{cands}, cfg);
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error().code(), atx::core::ErrorCode::InvalidArgument);
}

TEST(LearnNnGate, NSplitsExceedT_ReturnsInvalidArgument) {
  std::vector<LearnedModel> cands;
  cands.push_back(shell(skilled_series(6, 5, 0.2), 10)); // T = 6
  cands.push_back(shell(skilled_series(6, 6, 0.2), 10));
  NnGateCfg cfg;
  cfg.n_splits = 8; // > T -> rejected
  const auto r = gate_nn_sweep(std::span<const LearnedModel>{cands}, cfg);
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error().code(), atx::core::ErrorCode::InvalidArgument);
}

// ---------------------------------------------------------------------------
//  REAL-FIT wiring (the ONE end-to-end test). Build a small planted SequenceTensor
//  and fit a fit_tcn + fit_gru sweep on it (same CPCV / horizons so their OOS
//  series share the date axis -> equal length), then gate the sweep. This proves
//  the LearnedModel fields the gate reads (oos_score_series + trial_count) are the
//  ones the trainers actually fill — not just the hand-built shells above.
// ---------------------------------------------------------------------------
struct SynthCfg {
  usize n_dates;
  usize n_inst;
  usize L;
  usize F;
  usize n_horizons;
  bool planted;
  u64 seed;
};

SequenceTensor make_seq(const SynthCfg &c) {
  SequenceTensor st;
  st.lookback = c.L;
  st.n_features = c.F;
  st.y.assign(c.n_horizons, {});
  atx::core::Xoshiro256pp rng{c.seed};
  for (usize d = c.L - 1; d < c.n_dates; ++d) {
    for (usize i = 0; i < c.n_inst; ++i) {
      std::vector<f64> window(c.L * c.F, 0.0);
      for (usize l = 0; l < c.L; ++l) {
        const usize wd = d - (c.L - 1) + l;
        for (usize f = 0; f < c.F; ++f) {
          const f64 smooth =
              0.1 * static_cast<f64>(wd) + 0.3 * static_cast<f64>(i) + 0.05 * static_cast<f64>(f);
          window[l * c.F + f] = smooth + 0.2 * rng.normal();
        }
      }
      const f64 trailing0 = window[(c.L - 1) * c.F + 0];
      for (usize h = 0; h < c.n_horizons; ++h) {
        const f64 label = c.planted ? (trailing0 + 0.01 * static_cast<f64>(h)) : rng.normal();
        st.y[h].push_back(label);
      }
      for (const f64 v : window) {
        st.x.push_back(v);
      }
      st.date_of.push_back(d);
      st.inst_of.push_back(i);
      st.sample_valid.push_back(static_cast<u8>(1));
      ++st.n_samples;
    }
  }
  return st;
}

TEST(LearnNnGate, RealFitSweep_GatesEndToEnd) {
  // A planted SequenceTensor; fit a TCN + GRU candidate with the SAME cpcv /
  // horizons so their OOS series share the date axis (equal length T).
  const SequenceTensor st = make_seq({14, 6, 3, 2, 2, /*planted=*/true, 37});

  TcnAlphaCfg tcfg;
  tcfg.blocks = 2;
  tcfg.kernel = 2;
  tcfg.channels = 6;
  tcfg.dropout = 0.0;
  tcfg.cpcv.n_groups = 4;
  tcfg.cpcv.n_test_groups = 1;
  tcfg.cpcv.embargo = 0.0;
  tcfg.horizons = {1, 2};
  tcfg.train.epochs = 20;
  tcfg.train.batch_size = 16;
  tcfg.train.ckpt_every = 4;
  tcfg.train.ensemble_size = 1;
  tcfg.train.master_seed = 4242;

  GruAlphaCfg gcfg;
  gcfg.hidden = 6;
  gcfg.dropout = 0.0;
  gcfg.cpcv = tcfg.cpcv;
  gcfg.horizons = tcfg.horizons;
  gcfg.train = tcfg.train;

  const auto rt = fit_tcn(st, tcfg);
  const auto rg = fit_gru(st, gcfg);
  ASSERT_TRUE(rt.has_value()) << rt.error().to_string();
  ASSERT_TRUE(rg.has_value()) << rg.error().to_string();

  // The two real fits must produce equal-length OOS series (same date axis).
  ASSERT_FALSE(rt->oos_score_series.empty());
  ASSERT_EQ(rt->oos_score_series.size(), rg->oos_score_series.size())
      << "same cpcv/horizons must yield aligned OOS series for the PBO matrix";

  std::vector<LearnedModel> cands{*rt, *rg};
  NnGateCfg cfg;
  // n_splits must be even and <= T (the real OOS-series length); pick the largest
  // even value <= T so the PBO is well-defined regardless of the fold count.
  const usize T = rt->oos_score_series.size();
  cfg.n_splits = (T >= 2U) ? (T - (T % 2U)) : 2U;
  if (cfg.n_splits < 2U) {
    cfg.n_splits = 2U;
  }

  const auto r = gate_nn_sweep(std::span<const LearnedModel>{cands}, cfg);
  ASSERT_TRUE(r.has_value()) << r.error().to_string();
  const NnGateResult &g = *r;

  // The honest deflation N is the SUM of the two real fits' trial counts (R4).
  EXPECT_EQ(g.n_trials,
            ((rt->trial_count == 0U) ? 1U : rt->trial_count) +
                ((rg->trial_count == 0U) ? 1U : rg->trial_count))
      << "n_trials must aggregate BOTH real fits' fold counts";
  EXPECT_GT(g.n_trials, 0U);
  EXPECT_LT(g.winner, 2U);
  // pbo is a valid probability in [0, 1] (no assertion on admit — a tiny real fit's
  // verdict is data-dependent; the wiring + the trial-count aggregation are the pin).
  EXPECT_GE(g.pbo, 0.0);
  EXPECT_LE(g.pbo, 1.0);
}

} // namespace
