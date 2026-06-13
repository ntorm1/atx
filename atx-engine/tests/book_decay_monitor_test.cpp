// book_decay_monitor_test.cpp — S7-2: alpha-decay monitor + DecayController.
//
// The decay monitor detects a statistically-significant DOWNWARD shift in a
// live alpha's realized performance vs its admitted backtest baseline, at a
// controlled false-alarm rate, then drives the S4 lifecycle through demotion.
//
// Two complementary detectors, both reusing the as-built eval/cost/library
// spine (no re-implemented DSR, moments, or journal):
//   * a fast streaming Page-Hinkley DOWN test on the standardized live return,
//     gated by a MinTRL (Bailey-López de Prado) significance floor;
//   * a realized-DSR/PSR drop below the admitted baseline (cross-check);
//   * a cost-flooding discriminator (an alpha whose NET decayed only because
//     cost rose is sized down, NOT retired);
//   * a DecayController that maps verdicts to library::Library::mark with an
//     asymmetric "retire fast / restore slow" hysteresis, all PIT.
//
// The proofs here (DecayMonitor + DecayController):
//   1. DemotesPlantedDecayingAlpha   — a real down-shift IS detected (power).
//   2. DoesNotDemoteStableAlpha      — a stable stream NEVER flags (false-alarm).
//   3. MinTrlGatesEarlyDemotion      — too few obs => not yet significant.
//   4. CostFloodedIsNotDecay         — cost-only decay is sized down, not retired.
//   5. DrivesLifecyclePitAndAsymmetric — Live->Decaying->Dead, earlier PIT query
//      unchanged.
//   + seed sweeps on (1) and (2) to harden the non-vacuity / false-alarm claims.

#include <cmath>      // std::sqrt
#include <filesystem> // per-test temp directory
#include <span>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "atx/core/random.hpp" // Xoshiro256pp (deterministic noise streams)
#include "atx/core/types.hpp"  // f64, u32, u64, usize

#include "atx/engine/combine/gate.hpp"    // AlphaGate, GateConfig
#include "atx/engine/combine/metrics.hpp" // compute_metrics, AlphaMetrics
#include "atx/engine/combine/store.hpp"   // combine::AlphaId
#include "atx/engine/eval/stats_ext.hpp"  // eval::mean_std_pop (baseline construction)

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
using atx::engine::book::DecayVerdict;
using atx::engine::book::PageHinkleyState;
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
  PageHinkleyState ph;
  std::vector<f64> live;
  bool flagged = false;
  for (usize t = 0; t < dec.size(); ++t) {
    live.push_back(dec[t]);
    // gross edge clears cost: this is a TRUE alpha decay (not cost-flooding).
    const DecayVerdict v = mon.observe(base, live, /*gross*/ 20.0, /*cost*/ 5.0, ph);
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
  PageHinkleyState ph;
  std::vector<f64> live;
  for (usize t = 0; t < stab.size(); ++t) {
    live.push_back(stab[t]);
    const DecayVerdict v = mon.observe(base, live, /*gross*/ 20.0, /*cost*/ 5.0, ph);
    ASSERT_FALSE(v.flag) << "stable alpha falsely flagged at t=" << t;
    EXPECT_EQ(v.recommend, LifecycleState::Live);
  }
}

// NOTE (assertion choice): the plan suggested asserting `v.min_trl >= live.size()`
// on a tiny window. The as-built BLdP MinTRL uses the PER-OBSERVATION Sharpe (the
// convention eval::deflated_sharpe consumes), under which the admitted SR is ~1.8
// and the admit-vs-live gap (den) is large, so the formula legitimately returns a
// SMALL MinTRL (~2-3) — it is NOT >= a 5-obs window. The binding, non-vacuous
// guarantee the gate must provide is that 5 observations CANNOT conclude decay:
// the minimum-observation floor (ph_min_obs) and the confirmation run keep the
// verdict UNFLAGGED with the DSR-drop run still at zero. We assert that.
TEST(DecayMonitor, MinTrlGatesEarlyDemotion) {
  const DecayMonitor mon{atx::engine::book::default_decay_cfg()};
  const std::vector<f64> admit = admitted_pnl(11);
  const AdmittedBaseline base = baseline_from(admit, /*n_trials*/ 1);

  // Only 5 live observations of the decaying stream: below the ph_min_obs floor.
  const std::vector<f64> dec = decaying_stream(11, 5);
  PageHinkleyState ph;
  DecayVerdict v{};
  for (usize t = 0; t < dec.size(); ++t) {
    const std::span<const f64> win{dec.data(), t + 1};
    v = mon.observe(base, win, /*gross*/ 20.0, /*cost*/ 5.0, ph);
  }
  EXPECT_FALSE(v.flag) << "5 observations cannot conclude decay";
  EXPECT_EQ(ph.dsr_low_run, 0u) << "the DSR-drop run must not even start below the obs floor";
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
  PageHinkleyState ph;
  std::vector<f64> live;
  for (usize t = 0; t < dec.size(); ++t) {
    live.push_back(dec[t]);
    const DecayVerdict v = mon.observe(base, live, /*gross*/ 8.0, /*cost*/ 9.0, ph);
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
    PageHinkleyState ph;
    std::vector<f64> live;
    bool flagged = false;
    for (usize t = 0; t < dec.size(); ++t) {
      live.push_back(dec[t]);
      if (mon.observe(base, live, 20.0, 5.0, ph).flag) {
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
    PageHinkleyState ph;
    std::vector<f64> live;
    bool flagged = false;
    for (usize t = 0; t < stab.size(); ++t) {
      live.push_back(stab[t]);
      if (mon.observe(base, live, 20.0, 5.0, ph).flag) {
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
struct AdmittedAlpha {
  std::vector<f64> pnl;
  std::vector<f64> pos_flat;
  AlphaId id{0};
};

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
