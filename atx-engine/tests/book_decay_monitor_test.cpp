// book_decay_monitor_test.cpp — S7-2: alpha-decay monitor + DecayController.
//
// The decay monitor detects a statistically-significant DOWNWARD shift in a
// live alpha's realized performance vs its admitted backtest baseline, at a
// controlled false-alarm rate, then drives the S4 lifecycle through demotion.
//
// Two complementary detectors, both reusing the as-built eval/cost/library
// spine (no re-implemented DSR, moments, or journal):
//   * a fast streaming Page-Hinkley DOWN test on the standardized live return,
//     for a genuine LEVEL shift (healthy then decay);
//   * a realized-DSR/PSR drop below the admitted baseline, gated by a MinTRL
//     (Bailey-López de Prado) significance floor + an effect-size floor + a
//     confirmation run (the path that fires when the stream is decayed from t=0);
//   * a cost-flooding discriminator (an alpha whose NET decayed only because
//     cost rose is sized down, NOT retired);
//   * a DecayController that maps verdicts to library::Library::mark with an
//     asymmetric "retire fast / restore slow" hysteresis, all PIT.
//
// The proofs here (DecayMonitor + DecayController):
//   1. DemotesPlantedDecayingAlpha   — a real down-shift IS detected (DSR path).
//   2. DoesNotDemoteStableAlpha      — a stable stream NEVER flags (false-alarm).
//   3. PageHinkleyDetectsLevelShift  — a healthy->decay LEVEL shift trips PH itself
//      (proves the Page-Hinkley path, which the decayed-from-zero fixtures cannot).
//   4. MinTrlMatchesClosedForm       — the pure MinTRL function equals the BLdP
//      closed form + is monotone in the admit-vs-live gap (proves the MinTRL gate).
//   5. EarlyObsCannotConcludeDecay   — too few obs => no flag (the obs/run floor).
//   6. CostFloodedIsNotDecay         — cost-only decay is sized down, not retired.
//   7. DrivesLifecyclePitAndAsymmetric — Live->Decaying->Dead, earlier PIT query
//      unchanged.
//   + seed sweeps on (1) and (2) to harden the non-vacuity / false-alarm claims.

#include <cmath>        // std::ceil (MinTRL closed-form reference)
#include <filesystem>   // per-test temp directory
#include <limits>       // std::numeric_limits (MinTRL sentinel)
#include <span>
#include <string>
#include <system_error> // std::error_code (filesystem remove_all/create_directories)
#include <vector>

#include <gtest/gtest.h>

#include "atx/core/random.hpp" // Xoshiro256pp (deterministic noise streams)
#include "atx/core/types.hpp"  // f64, u32, u64, usize

#include "atx/engine/combine/gate.hpp"    // AlphaGate, GateConfig
#include "atx/engine/combine/metrics.hpp" // compute_metrics, AlphaMetrics
#include "atx/engine/combine/store.hpp"   // combine::AlphaId
#include "atx/engine/eval/stats_ext.hpp"  // eval::norm_ppf (MinTRL closed-form reference)

#include "atx/engine/book/decay_monitor.hpp" // the unit under test
#include "atx/engine/library/library.hpp"    // Library facade + AlphaCandidate
#include "atx/engine/library/lifecycle.hpp"  // LifecycleState
#include "atx/engine/library/record.hpp"     // Provenance

namespace {

using atx::f64;
using atx::u32;
using atx::u64;
using atx::usize;
using atx::core::Xoshiro256pp;
using atx::engine::book::AdmittedBaseline;
using atx::engine::book::DecayConfig;
using atx::engine::book::DecayController;
using atx::engine::book::DecayMonitor;
using atx::engine::book::DecayState;
using atx::engine::book::DecayVerdict;
using atx::engine::book::min_track_record_length;
using atx::engine::combine::AlphaGate;
using atx::engine::combine::AlphaId;
using atx::engine::combine::compute_metrics;
using atx::engine::combine::GateConfig;
using atx::engine::library::LifecycleState;

namespace lib = atx::engine::library;

// ---- stream synthesis (deterministic) ---------------------------------------

// An admitted backtest pnl: a strong per-period Sharpe ~ kAdmitMean/kAdmitSd.
// Gaussian draws around a positive mean (a believable realized OOS-ish stream).
inline constexpr f64 kAdmitMean = 0.010; // per-period mean return
inline constexpr f64 kAdmitSd = 0.005;   // per-period vol => SR_admit ~ 2.0
inline constexpr usize kAdmitLen = 120;  // admitted backtest length

// A strong admitted stream (SR ~ 2.0/period), fixed-seed Gaussian.
[[nodiscard]] std::vector<f64> admitted_pnl(u64 seed) {
  Xoshiro256pp rng(seed);
  std::vector<f64> p(kAdmitLen, 0.0);
  for (usize t = 0; t < kAdmitLen; ++t) {
    p[t] = kAdmitMean + kAdmitSd * rng.normal();
  }
  return p;
}

// A DECAYING live stream: realized mean roughly halves (Sharpe ~ halves), same
// vol. A genuine downward shift the monitor MUST detect.
[[nodiscard]] std::vector<f64> decaying_stream(u64 seed, usize n) {
  Xoshiro256pp rng(seed ^ 0xDECA1ULL);
  std::vector<f64> p(n, 0.0);
  for (usize t = 0; t < n; ++t) {
    p[t] = 0.5 * kAdmitMean + kAdmitSd * rng.normal(); // mean halved
  }
  return p;
}

// A STABLE live stream: same distribution as the admitted baseline (no shift).
[[nodiscard]] std::vector<f64> stable_stream(u64 seed, usize n) {
  Xoshiro256pp rng(seed ^ 0x57AB1EULL);
  std::vector<f64> p(n, 0.0);
  for (usize t = 0; t < n; ++t) {
    p[t] = kAdmitMean + kAdmitSd * rng.normal();
  }
  return p;
}

// A REALISTIC live stream: `healthy` observations at the admitted mean (≈ admitted
// SR), then a LEVEL DROP to ~zero mean for `decayed` observations. This is the
// transition Page-Hinkley is designed for — a step change relative to the warmed-up
// running mean — which the decayed-from-t=0 fixtures (DSR-path proofs) cannot
// exhibit. The post-shift mean is driven to ~0 (a strong, unambiguous level shift)
// so the standardized z sits ≈ -2/step and the PH statistic (max − cum) climbs past
// a realistic lambda. Fully deterministic in the seed (L7).
[[nodiscard]] std::vector<f64> healthy_then_decay_stream(u64 seed, usize healthy, usize decayed) {
  Xoshiro256pp rng(seed ^ 0x4EA178ULL);
  std::vector<f64> p(healthy + decayed, 0.0);
  for (usize t = 0; t < healthy; ++t) {
    p[t] = kAdmitMean + kAdmitSd * rng.normal(); // healthy: ≈ admitted mean
  }
  for (usize t = healthy; t < healthy + decayed; ++t) {
    p[t] = 0.0 * kAdmitMean + kAdmitSd * rng.normal(); // decayed: mean drops to ~0
  }
  return p;
}

// Build an AdmittedBaseline directly from a pnl span (mirrors what the
// DecayController freezes from lib.pnl(id) on the first step).
[[nodiscard]] AdmittedBaseline baseline_from(std::span<const f64> pnl, usize n_trials) {
  return atx::engine::book::make_baseline(pnl, n_trials);
}

// ---- DecayMonitor tests -----------------------------------------------------

TEST(DecayMonitor, DemotesPlantedDecayingAlpha) {
  const DecayMonitor mon{atx::engine::book::default_decay_cfg()};
  const std::vector<f64> admit = admitted_pnl(11);
  const AdmittedBaseline base = baseline_from(admit, /*n_trials*/ 1);

  const std::vector<f64> dec = decaying_stream(11, 120);
  DecayState st;
  std::vector<f64> live;
  bool flagged = false;
  for (usize t = 0; t < dec.size(); ++t) {
    live.push_back(dec[t]);
    // gross edge clears cost: this is a TRUE alpha decay (not cost-flooding).
    const DecayVerdict v = mon.observe(base, live, /*gross*/ 20.0, /*cost*/ 5.0, st);
    if (v.flag) {
      flagged = true;
      EXPECT_EQ(v.recommend, LifecycleState::Decaying);
      break;
    }
  }
  EXPECT_TRUE(flagged) << "a genuine downward decay must be detected within 120 periods";
}

TEST(DecayMonitor, DoesNotDemoteStableAlpha) {
  const DecayMonitor mon{atx::engine::book::default_decay_cfg()};
  const std::vector<f64> admit = admitted_pnl(11);
  const AdmittedBaseline base = baseline_from(admit, /*n_trials*/ 1);

  const std::vector<f64> stab = stable_stream(11, 120);
  DecayState st;
  std::vector<f64> live;
  for (usize t = 0; t < stab.size(); ++t) {
    live.push_back(stab[t]);
    const DecayVerdict v = mon.observe(base, live, /*gross*/ 20.0, /*cost*/ 5.0, st);
    ASSERT_FALSE(v.flag) << "stable alpha falsely flagged at t=" << t;
    EXPECT_EQ(v.recommend, LifecycleState::Live);
  }
}

// PROVES THE PAGE-HINKLEY PATH. The decayed-from-t=0 fixtures (tests 1/2) keep PH
// structurally dormant — there is no healthy baseline for the running mean to
// detect a shift against, so they only exercise the DSR-drop path. A live alpha in
// the real pipeline starts HEALTHY (≈ admitted SR) then decays: a genuine LEVEL
// shift, which is exactly what Page-Hinkley detects. Here we feed a 60-healthy /
// 60-decayed stream and assert the PH statistic (st.ph.max − st.ph.cum) crosses the
// trip threshold at some period AND the verdict flags via that path.
//
// LAMBDA CHOICE: the default ph_lambda=50 never trips on the decayed-from-t=0
// fixtures (no healthy baseline => the running mean absorbs the shift, capping the
// PH excursion ~0.1). For this genuine 60-healthy/60-decayed level shift we use a
// ph_lambda matched to the standardized-z scale of the transition: calibrated so it
// stays ABOVE the healthy-phase noise peak (~10 for this seed) yet well BELOW the
// post-shift PH statistic (~83), so it fires ONLY after the shift. This is the
// documented "pick a fixture/λ that makes PH genuinely fire" the S7-2 review
// required — a non-vacuous PH proof (PH tripped at period ~79 in calibration).
TEST(DecayMonitor, PageHinkleyDetectsLevelShift) {
  DecayConfig cfg = atx::engine::book::default_decay_cfg();
  cfg.ph_lambda = 40.0; // above the healthy-phase noise peak, below the post-shift stat (documented)
  const DecayMonitor mon{cfg};
  const std::vector<f64> admit = admitted_pnl(11);
  const AdmittedBaseline base = baseline_from(admit, /*n_trials*/ 1);

  const std::vector<f64> stream = healthy_then_decay_stream(11, /*healthy*/ 60, /*decayed*/ 60);
  DecayState st;
  std::vector<f64> live;
  bool ph_tripped = false;  // the Page-Hinkley statistic itself crossed lambda
  bool flagged = false;     // the verdict raised the decay flag
  long long trip_period = -1;
  for (usize t = 0; t < stream.size(); ++t) {
    live.push_back(stream[t]);
    const DecayVerdict v = mon.observe(base, live, /*gross*/ 20.0, /*cost*/ 5.0, st);
    const bool ph_now = (st.ph.n >= cfg.ph_min_obs) && (st.ph.max - st.ph.cum > cfg.ph_lambda);
    if (ph_now && !ph_tripped) {
      ph_tripped = true;
      trip_period = static_cast<long long>(t);
    }
    if (v.flag) {
      flagged = true;
    }
  }
  EXPECT_TRUE(ph_tripped) << "Page-Hinkley (max-cum > lambda) never tripped on a real level shift";
  EXPECT_TRUE(flagged) << "the level shift must raise the decay flag";
  EXPECT_GE(trip_period, 60) << "PH must trip only AFTER the level shift (period 60), not during healthy phase";
}

// PROVES THE MinTRL GATE as a pure function (it is dominated by ph_min_obs on the
// integration fixtures — MinTRL≈4 there — so it never binds in those tests; this
// exercises the closed form directly). MinTRL = 1 + var_term·(Φ⁻¹(1−α)/(sr_admit−
// sr_live))² with var_term = 1 − γ3·sr_live + ((κ+2)/4)·sr_live². Also asserts the
// two qualitative invariants: a SMALLER positive gap yields a LARGER MinTRL, and a
// non-positive gap yields the SIZE_MAX sentinel.
TEST(DecayMonitor, MinTrlMatchesClosedForm) {
  using atx::engine::eval::norm_ppf;
  constexpr f64 kAlpha = 0.05;
  const f64 sr_admit = 2.0;
  const f64 sr_live = 1.0; // a real decay: positive gap
  const f64 skew = 0.0;
  const f64 exkurt = 0.0;

  // Closed form (Bailey-López de Prado).
  const f64 den = sr_admit - sr_live;
  const f64 var_term = 1.0 - skew * sr_live + ((exkurt + 2.0) / 4.0) * sr_live * sr_live;
  const f64 ratio = norm_ppf(1.0 - kAlpha) / den;
  const f64 trl_closed = 1.0 + var_term * ratio * ratio;
  const auto expected = static_cast<usize>(std::ceil(trl_closed));

  const usize got = min_track_record_length(sr_admit, sr_live, skew, exkurt, kAlpha);
  EXPECT_EQ(got, expected) << "MinTRL must equal its BLdP closed form (ceil)";

  // Monotonicity: a SMALLER admit-vs-live gap (sr_live closer to sr_admit) requires
  // a LONGER track record to resolve as significant.
  const usize small_gap = min_track_record_length(sr_admit, 1.8, skew, exkurt, kAlpha); // gap 0.2
  const usize large_gap = min_track_record_length(sr_admit, 0.5, skew, exkurt, kAlpha); // gap 1.5
  EXPECT_GT(small_gap, large_gap) << "a smaller admit-vs-live gap must demand a larger MinTRL";

  // A non-positive gap (live not below admit) cannot conclude decay -> the sentinel.
  EXPECT_EQ(min_track_record_length(sr_admit, sr_admit, skew, exkurt, kAlpha),
            std::numeric_limits<usize>::max())
      << "a zero gap must return the SIZE_MAX 'cannot conclude' sentinel";
  EXPECT_EQ(min_track_record_length(sr_admit, /*sr_live*/ 3.0, skew, exkurt, kAlpha),
            std::numeric_limits<usize>::max())
      << "live ABOVE admit (negative gap) must also return the sentinel";
}

// EARLY-OBS GUARANTEE. The plan named this MinTrlGatesEarlyDemotion, but the MinTRL
// gate does NOT bind here: under the per-observation Sharpe convention the admit-vs-
// live gap is large, so the BLdP MinTRL is small (~2-3) and is dominated by
// ph_min_obs=30 on this fixture (the MinTRL gate itself is proven by
// MinTrlMatchesClosedForm above). The binding, non-vacuous guarantee at 5 obs is
// that decay CANNOT be concluded: the minimum-observation floor (ph_min_obs) and the
// confirmation run keep the verdict UNFLAGGED with the DSR-drop run still at zero.
TEST(DecayMonitor, EarlyObsCannotConcludeDecay) {
  const DecayMonitor mon{atx::engine::book::default_decay_cfg()};
  const std::vector<f64> admit = admitted_pnl(11);
  const AdmittedBaseline base = baseline_from(admit, /*n_trials*/ 1);

  // Only 5 live observations of the decaying stream: below the ph_min_obs floor.
  const std::vector<f64> dec = decaying_stream(11, 5);
  DecayState st;
  DecayVerdict v{};
  for (usize t = 0; t < dec.size(); ++t) {
    const std::span<const f64> win{dec.data(), t + 1};
    v = mon.observe(base, win, /*gross*/ 20.0, /*cost*/ 5.0, st);
  }
  EXPECT_FALSE(v.flag) << "5 observations cannot conclude decay";
  EXPECT_EQ(st.dsr_low_run, 0u) << "the DSR-drop run must not even start below the obs floor";
  EXPECT_LT(dec.size(), atx::engine::book::default_decay_cfg().ph_min_obs)
      << "premise: the window is below the minimum-observation floor";
}

TEST(DecayMonitor, CostFloodedIsNotDecay) {
  const DecayMonitor mon{atx::engine::book::default_decay_cfg()};
  const std::vector<f64> admit = admitted_pnl(11);
  const AdmittedBaseline base = baseline_from(admit, /*n_trials*/ 1);

  // A decaying NET stream whose GROSS edge (8 bps) no longer clears cost (9 bps):
  // the decay is cost-driven, so the alpha is SIZED DOWN, not retired.
  const std::vector<f64> dec = decaying_stream(11, 120);
  DecayState st;
  std::vector<f64> live;
  for (usize t = 0; t < dec.size(); ++t) {
    live.push_back(dec[t]);
    const DecayVerdict v = mon.observe(base, live, /*gross*/ 8.0, /*cost*/ 9.0, st);
    EXPECT_TRUE(v.cost_flooded);
    EXPECT_FALSE(v.flag) << "cost-flooded alpha must not be retired (sized down instead)";
  }
}

// ---- seed sweeps (harden the power / false-alarm claims) --------------------

TEST(DecayMonitor, DemotesDecayAcrossSeeds) {
  const DecayMonitor mon{atx::engine::book::default_decay_cfg()};
  for (u64 s = 0; s < 12; ++s) {
    const std::vector<f64> admit = admitted_pnl(100 + s);
    const AdmittedBaseline base = baseline_from(admit, /*n_trials*/ 1);
    const std::vector<f64> dec = decaying_stream(100 + s, 120);
    DecayState st;
    std::vector<f64> live;
    bool flagged = false;
    for (usize t = 0; t < dec.size(); ++t) {
      live.push_back(dec[t]);
      if (mon.observe(base, live, 20.0, 5.0, st).flag) {
        flagged = true;
        break;
      }
    }
    EXPECT_TRUE(flagged) << "decay not detected for seed " << s;
  }
}

TEST(DecayMonitor, NeverDemotesStableAcrossSeeds) {
  const DecayMonitor mon{atx::engine::book::default_decay_cfg()};
  for (u64 s = 0; s < 12; ++s) {
    const std::vector<f64> admit = admitted_pnl(100 + s);
    const AdmittedBaseline base = baseline_from(admit, /*n_trials*/ 1);
    const std::vector<f64> stab = stable_stream(100 + s, 120);
    DecayState st;
    std::vector<f64> live;
    bool flagged = false;
    for (usize t = 0; t < stab.size(); ++t) {
      live.push_back(stab[t]);
      if (mon.observe(base, live, 20.0, 5.0, st).flag) {
        flagged = true;
        break;
      }
    }
    EXPECT_FALSE(flagged) << "false alarm on stable stream for seed " << s;
  }
}

// ---- DecayController test ---------------------------------------------------

[[nodiscard]] std::string tmpdir(const std::string &tag = "") {
  const ::testing::TestInfo *info = ::testing::UnitTest::GetInstance()->current_test_info();
  std::string base = std::string(info != nullptr ? info->test_suite_name() : "S7") + "_" +
                     std::string(info != nullptr ? info->name() : "t") + "_" + tag;
  const std::filesystem::path dir = std::filesystem::temp_directory_path() / "atx_s7_book" / base;
  std::error_code ec;
  std::filesystem::remove_all(dir, ec);
  std::filesystem::create_directories(dir, ec);
  return dir.string();
}

// Admit one alpha with the given pnl into a fresh library and march it to Live.
// Returns the live alpha's id (the candidate spans must outlive admit()).
[[nodiscard]] AlphaId admit_one_live(lib::Library &facade, const std::vector<f64> &pnl,
                                     const std::vector<f64> &pos_flat) {
  const AlphaGate gate{GateConfig{}};
  lib::AlphaCandidate c{};
  c.canon_hash = 0xABCDEF01u;
  c.pnl = pnl;
  c.pos_flat = pos_flat;
  c.metrics = compute_metrics(pnl, pos_flat, /*n_inst*/ 1, /*book*/ 1.0);
  c.prov = lib::Provenance{"synthetic-live", std::vector<u64>{}, 0, 7};
  c.as_of = 0;
  c.source = nullptr;
  const auto v = facade.admit(c, gate);
  EXPECT_EQ(v.kind, lib::AdmitKind::Accept) << "fixture alpha rejected by gate";
  const AlphaId id = v.id;
  // Candidate->Admitted recorded at as_of 0; march Admitted->Live (legal chain).
  EXPECT_TRUE(facade.mark(id, LifecycleState::Live, /*as_of*/ 1).has_value());
  auto s = facade.state_as_of(id, 1);
  EXPECT_TRUE(s.has_value() && *s == LifecycleState::Live) << "alpha must be Live before stepping";
  return id;
}

TEST(DecayController, DrivesLifecyclePitAndAsymmetric) {
  lib::Library facade = lib::Library::open(tmpdir(), GateConfig{}, {909090ULL});

  // A strong admitted+Live alpha (SR_admit ~ 2.0/period). Positions are near-static
  // so turnover clears the gate; the pnl clears sharpe/fitness floors easily.
  const std::vector<f64> admit = admitted_pnl(11);
  std::vector<f64> pos(kAdmitLen, 0.10);
  pos[0] = 0.10;
  const AlphaId id = admit_one_live(facade, admit, pos);

  // Step a 200-period decaying stream. Lifecycle as_of starts AFTER the Live
  // mark (period 1): step at as_of = 2.. so the journal stays monotone.
  DecayController ctrl{atx::engine::book::default_decay_cfg()};
  const std::vector<f64> dec = decaying_stream(11, 200);
  for (usize t = 0; t < dec.size(); ++t) {
    const usize as_of = t + 2; // strictly after the Admitted(0)/Live(1) marks
    ctrl.step(facade, id, dec[t], as_of, /*gross*/ 20.0, /*cost*/ 5.0);
  }

  // Live -> Decaying -> Dead by the end (retire fast after confirmation).
  auto s_end = facade.state_as_of(id, dec.size() + 1);
  ASSERT_TRUE(s_end.has_value());
  EXPECT_EQ(*s_end, LifecycleState::Dead) << "a sustained decay must retire the alpha";

  // PIT: an earlier query is UNCHANGED by the later demotion (period 10 still Live).
  auto s_early = facade.state_as_of(id, 10);
  ASSERT_TRUE(s_early.has_value());
  EXPECT_EQ(*s_early, LifecycleState::Live) << "PIT: earlier state must not be retroactively relabeled";
}

} // namespace
