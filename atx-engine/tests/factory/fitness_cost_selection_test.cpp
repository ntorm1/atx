// fitness_cost_selection_test.cpp — S4-4 [OPT-IN, B7]: charge sqrt-law impact
// in the SEARCH SELECTION scalar (ScalarRaw `raw`), behind CostSelectionConfig.
//
// Today the cost model enters ONLY the NSGA objective vector
// (objectives[4] = -cost_bps, gated by target_aum>0); ScalarRaw ranks on
// raw = wq*diversify*robust alone, with NO cost term, so a high-turnover
// in-sample winner can rank above a low-turnover one even though it dies
// net-of-cost. `factory::apply_selection_cost` (fitness_cost_selection.hpp,
// new) is the pure, testable half of the fix: `finish_report` itself cannot
// gain a CostSelectionConfig parameter this sprint (FitnessCfg is Sprint-5-
// owned; see fitness_cost_selection.hpp's SEAM note), so S4 exercises the
// wiring by calling apply_selection_cost directly on hand-built numbers and,
// separately, on a real book_cost_bps figure (proving S4-1's participation
// fix composes with S4-4's selection penalty).
//
// Load-bearing checks:
//   (a) SelectionCostOff_ByteIdenticalToRaw / SelectionAumZero_ByteIdenticalToRaw
//       — the inert-default contract (impact_in_selection=false and/or
//       selection_aum<=0 -> raw unchanged, regardless of cost_bps).
//   (b) SelectionFlipsHighTurnoverRanking — the spec's ranking-flip: gross gap
//       0.05, A's net penalty 0.20, B's 0.02 -> OFF ranks A above B (gross),
//       ON flips to B above A (net).
//   (c) SelectionUsesFixedParticipation — two book_cost_bps fixtures at a 10x
//       price difference (dollar-ADV and sigma held equal by construction)
//       produce the SAME cost_bps -- book_cost_bps (post-S4-1) is price-
//       invariant -- and therefore the SAME adjusted selection scalar.
//   (d) PureFunction_TwiceRunAndConcurrentCallsByteIdentical — no allocation,
//       no state, no RNG: repeated/concurrent calls with identical inputs
//       produce bit-identical outputs (twice-run + seq==parallel).

#include <array>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "atx/core/types.hpp"

#include "atx/engine/alpha/panel.hpp"
#include "atx/engine/alpha/streams.hpp"
#include "atx/engine/combine/cost_util.hpp"
#include "atx/engine/cost/calibration.hpp"
#include "atx/engine/cost/cost_selection_config.hpp"
#include "atx/engine/exec/execution_sim.hpp"
#include "atx/engine/factory/fitness.hpp"
#include "atx/engine/factory/fitness_cost_selection.hpp"

namespace atxtest_fitness_cost_selection_test {

using atx::f64;
using atx::usize;
using atx::engine::alpha::AlphaStreams;
using atx::engine::alpha::Panel;
using atx::engine::exec::ImpactCfg;
using atx::engine::exec::SlippageCfg;
using atx::engine::factory::apply_selection_cost;
using atx::engine::factory::book_cost_bps;
namespace cost = atx::engine::cost;

[[nodiscard]] Panel make_panel(usize dates, usize insts, std::vector<std::string> fields,
                               std::vector<std::vector<f64>> cols) {
  auto r = Panel::create(dates, insts, std::move(fields), std::move(cols), {});
  EXPECT_TRUE(r.has_value()) << "panel fixture must build";
  return std::move(r.value());
}

[[nodiscard]] cost::CalibratedCost calibrated_cost(f64 Y, f64 delta, f64 gamma,
                                                   f64 slip_bps) noexcept {
  SlippageCfg slip{};
  slip.bps = slip_bps;
  return cost::CalibratedCost{ImpactCfg{Y, delta, gamma}, slip, cost::FitReport{}};
}

// =============================================================================
//  (a) Inert-default contract: off (either flag) -> raw returned UNCHANGED,
//  regardless of how large cost_bps_at_selection_aum is.
// =============================================================================
TEST(FitnessCostSelection, SelectionCostOff_ByteIdenticalToRaw) {
  const cost::CostSelectionConfig cfg{}; // impact_in_selection=false (default)
  EXPECT_EQ(apply_selection_cost(0.75, 100.0, cfg), 0.75)
      << "impact_in_selection=false must return raw UNCHANGED even at a huge cost_bps";
  EXPECT_EQ(apply_selection_cost(0.0, 5.0, cfg), 0.0);
  EXPECT_EQ(apply_selection_cost(-1.0, 5.0, cfg), -1.0);
}

TEST(FitnessCostSelection, SelectionAumZero_ByteIdenticalToRaw) {
  cost::CostSelectionConfig cfg{};
  cfg.impact_in_selection = true;
  cfg.selection_aum = 0.0; // "0 => off regardless of impact_in_selection"
  EXPECT_EQ(apply_selection_cost(0.75, 100.0, cfg), 0.75)
      << "selection_aum<=0 must be off regardless of impact_in_selection";
}

// =============================================================================
//  (b) SelectionFlipsHighTurnoverRanking — the spec's by-construction flip.
//  A: raw=1.00 (gross winner, gap 0.05 over B), cost_bps=2.0 -> net penalty
//     1.0*2.0*kFitnessCostScale(0.1) = 0.20 -> adjusted = 0.80.
//  B: raw=0.95, cost_bps=0.2 -> penalty 0.02 -> adjusted = 0.93.
//  OFF: A(1.00) > B(0.95) -- gross order. ON: adjusted_A(0.80) < adjusted_B(0.93)
//  -- the rank FLIPS.
// =============================================================================
TEST(FitnessCostSelection, SelectionFlipsHighTurnoverRanking) {
  constexpr f64 kRawA = 1.00, kRawB = 0.95;
  constexpr f64 kCostBpsA = 2.0, kCostBpsB = 0.2;

  const cost::CostSelectionConfig off{}; // impact_in_selection=false
  const f64 offA = apply_selection_cost(kRawA, kCostBpsA, off);
  const f64 offB = apply_selection_cost(kRawB, kCostBpsB, off);
  EXPECT_GT(offA, offB) << "cost OFF: the gross winner (A) ranks above B";

  cost::CostSelectionConfig on{};
  on.impact_in_selection = true;
  on.selection_aum = 1.0; // any positive value -- selection_aum itself is not
                          // read by apply_selection_cost, only its off-guard.
  on.cost_weight = 1.0;
  const f64 onA = apply_selection_cost(kRawA, kCostBpsA, on);
  const f64 onB = apply_selection_cost(kRawB, kCostBpsB, on);
  EXPECT_NEAR(onA, 0.80, 1e-12);
  EXPECT_NEAR(onB, 0.93, 1e-12);
  EXPECT_LT(onA, onB) << "cost ON: the high-turnover-but-strong A must rank BELOW "
                         "the low-turnover B once net-of-cost -- the ranking FLIPS";
}

// =============================================================================
//  (c) SelectionUsesFixedParticipation — book_cost_bps (S4-1-corrected
//  participation) is price-invariant; feeding two equal-cost fixtures at a
//  10x price difference into apply_selection_cost yields equal adjusted
//  scalars, proving S4-1 and S4-4 compose cleanly.
//
//  Two 3-date, one-instrument panels differing ONLY by a 10x price/volume
//  rescale (LOW: close=[100,110,99], volume=[1000,1000,1000]; HIGH:
//  close=[1000,1100,990], volume=[100,100,100]) share the SAME returns
//  ([+0.10,-0.10] -> sigma=0.10) and the SAME dollar-ADV
//  (mean(close*volume)=103000 for both) -- so at equal target_aum/|w| the
//  post-S4-1 book_cost_bps must be numerically IDENTICAL despite the 10x
//  price gap (the exact bug this sprint fixed: the buggy formula divided by
//  price an extra time and would have produced a 10x-different cost here).
// =============================================================================
TEST(FitnessCostSelection, SelectionUsesFixedParticipation) {
  const std::vector<f64> close_low{100.0, 110.0, 99.0};
  const std::vector<f64> volume_low{1000.0, 1000.0, 1000.0};
  const Panel panel_low = make_panel(3, 1, {"close", "volume"}, {close_low, volume_low});

  const std::vector<f64> close_high{1000.0, 1100.0, 990.0};
  const std::vector<f64> volume_high{100.0, 100.0, 100.0};
  const Panel panel_high = make_panel(3, 1, {"close", "volume"}, {close_high, volume_high});

  AlphaStreams strm;
  strm.n_alphas_ = 1;
  strm.n_periods_ = 3;
  strm.n_instruments_ = 1;
  strm.pnl_flat.assign(3, 0.0);
  strm.pos_flat.assign(3, 0.0);
  strm.pos_flat[2] = 1.0; // positions(0, last=2)[0] = 1.0

  const cost::CalibratedCost cc = calibrated_cost(/*Y*/ 0.8, /*delta*/ 0.5, /*gamma*/ 0.3,
                                                  /*slip*/ 4.0);
  const f64 target_aum = 5.0e5;

  const f64 cost_low = book_cost_bps(strm, panel_low, cc, target_aum);
  const f64 cost_high = book_cost_bps(strm, panel_high, cc, target_aum);
  EXPECT_NEAR(cost_low, cost_high, 1e-9)
      << "book_cost_bps must be price-invariant (post-S4-1) -- a 10x price/volume "
         "rescale that holds dollar-ADV and sigma fixed must NOT change the cost";
  EXPECT_GT(cost_low, 0.0) << "sanity: a non-degenerate fixture carries positive cost";

  cost::CostSelectionConfig cfg{};
  cfg.impact_in_selection = true;
  cfg.selection_aum = target_aum;
  cfg.cost_weight = 1.0;
  constexpr f64 kRaw = 0.9;
  const f64 adjusted_low = apply_selection_cost(kRaw, cost_low, cfg);
  const f64 adjusted_high = apply_selection_cost(kRaw, cost_high, cfg);
  EXPECT_NEAR(adjusted_low, adjusted_high, 1e-9)
      << "S4-1 (price-invariant book_cost_bps) and S4-4 (selection penalty) compose: "
         "identical cost inputs must produce identical adjusted selection scalars";
}

// =============================================================================
//  (d) Pure-function determinism: no allocation, no state, no RNG -- repeated
//  and CONCURRENT calls with identical inputs are bit-identical (twice-run +
//  seq==parallel, the two remaining opt-in test classes).
// =============================================================================
TEST(FitnessCostSelection, PureFunction_TwiceRunAndConcurrentCallsByteIdentical) {
  cost::CostSelectionConfig cfg{};
  cfg.impact_in_selection = true;
  cfg.selection_aum = 2.0e6;
  cfg.cost_weight = 1.25;
  constexpr f64 kRaw = 0.83;
  constexpr f64 kCostBps = 3.7;

  const f64 first = apply_selection_cost(kRaw, kCostBps, cfg);
  const f64 second = apply_selection_cost(kRaw, kCostBps, cfg);
  EXPECT_EQ(first, second) << "twice-run: identical inputs must produce a bit-identical result";

  constexpr usize kThreads = 8;
  std::array<f64, kThreads> results{};
  std::vector<std::thread> pool;
  pool.reserve(kThreads);
  for (usize i = 0; i < kThreads; ++i) {
    pool.emplace_back([&results, i, cfg] { results[i] = apply_selection_cost(kRaw, kCostBps, cfg); });
  }
  for (auto &t : pool) {
    t.join();
  }
  for (usize i = 0; i < kThreads; ++i) {
    EXPECT_EQ(results[i], first) << "seq==parallel: concurrent calls must all agree with the "
                                    "single-threaded result (no hidden shared state)";
  }
}

} // namespace atxtest_fitness_cost_selection_test
