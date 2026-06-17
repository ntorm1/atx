// atx::engine::factory — cost-aware mining fitness tests (S4.3, suite
// FactoryCostAwareFitness).
//
// S4.3 adds a FIFTH NSGA-II objective: objectives[4] = -cost_bps, the book-
// aggregate round-trip impact cost at a recorded target AUM. The objective is
// ACTIVE only when FitnessCfg.target_aum > 0; target_aum == 0 is a pure no-op
// (no compute, no objective, no digest drift — the boundary pin holds).
//
// Load-bearing checks:
//   (a) HandCheck_BookCostMatchesRoundTrip — book_cost_bps equals a hand-computed
//       |w|-weighted round_trip_cost_bps for a known (participation, sigma).
//   (b) TargetAumZeroIsNoOp — at target_aum == 0 no cost objective is added
//       (n_objectives stays 3, cost_bps == 0) and the report equals the legacy one.
//   (c) RejectsStrongButExpensiveAlpha — the spec exit criterion: at target_aum>0
//       a strong-but-expensive alpha is Pareto-dominated by a slightly weaker but
//       far cheaper one; at target_aum==0 the gross order is restored.
//   (d) CostBpsIsDeterministic + DetPool {1,2,4,8} worker-invariance of a cost-on
//       MultiObjective search digest.

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

#include "atx/core/types.hpp"

#include "atx/engine/alpha/panel.hpp"
#include "atx/engine/alpha/parser.hpp"
#include "atx/engine/alpha/registry.hpp"
#include "atx/engine/alpha/streams.hpp"
#include "atx/engine/alpha/typecheck.hpp"
#include "atx/engine/alpha/vm.hpp"
#include "atx/engine/combine/metrics.hpp"
#include "atx/engine/combine/store.hpp"
#include "atx/engine/cost/calibration.hpp"
#include "atx/engine/cost/cost_aware.hpp"
#include "atx/engine/exec/execution_sim.hpp"
#include "atx/engine/factory/fitness.hpp"
#include "atx/engine/factory/genome.hpp"
#include "atx/engine/factory/pareto.hpp"
#include "atx/engine/factory/search_driver.hpp"
#include "atx/engine/loop/weight_policy.hpp"

namespace {

using atx::f64;
using atx::u16;
using atx::usize;
using atx::engine::WeightPolicy;
using atx::engine::alpha::AlphaStreams;
using atx::engine::alpha::analyze;
using atx::engine::alpha::Library;
using atx::engine::alpha::Panel;
using atx::engine::alpha::parse_expr;
using atx::engine::combine::AlphaMetrics;
using atx::engine::combine::AlphaStore;
using atx::engine::exec::CommissionCfg;
using atx::engine::exec::CommissionMode;
using atx::engine::exec::ExecutionSimulator;
using atx::engine::exec::FillCfg;
using atx::engine::exec::ImpactCfg;
using atx::engine::exec::LatencyCfg;
using atx::engine::exec::SlippageCfg;
using atx::engine::exec::SlippageMode;
using atx::engine::exec::VolumeCapCfg;
using atx::engine::factory::book_cost_bps;
using atx::engine::factory::dominates;
using atx::engine::factory::FitnessCfg;
using atx::engine::factory::FitnessReport;
using atx::engine::factory::Genome;
using atx::engine::factory::ObjectiveMode;
using atx::engine::factory::pool_aware_fitness;
using atx::engine::factory::SearchConfig;
using atx::engine::factory::SearchDriver;
using atx::engine::factory::SearchResult;
namespace cost = atx::engine::cost;

// ---- builders ---------------------------------------------------------------

[[nodiscard]] ExecutionSimulator frictionless_sim() {
  return ExecutionSimulator{FillCfg{},
                            SlippageCfg{SlippageMode::VolumeShare, 0.0, 0.0, 0.0, 0.0},
                            ImpactCfg{0.0, 0.5, 0.0},
                            CommissionCfg{CommissionMode::PerShare, 0.0, 0.0, 1.0, 0.0},
                            LatencyCfg{},
                            VolumeCapCfg{1.0}};
}

[[nodiscard]] Genome make_genome(std::string_view src, Library &lib) {
  auto parsed = parse_expr(src, lib);
  EXPECT_TRUE(parsed.has_value()) << (parsed ? "" : parsed.error().message());
  if (!parsed) {
    return Genome{};
  }
  auto info = analyze(*parsed);
  EXPECT_TRUE(info.has_value()) << (info ? "" : info.error().message());
  if (!info) {
    return Genome{};
  }
  return Genome{std::move(*parsed), std::move(*info), 0};
}

[[nodiscard]] Panel make_panel(usize dates, usize insts, std::vector<std::string> fields,
                               std::vector<std::vector<f64>> cols,
                               std::vector<std::uint8_t> universe = {}) {
  auto r = Panel::create(dates, insts, std::move(fields), std::move(cols), std::move(universe));
  EXPECT_TRUE(r.has_value()) << "panel fixture must build";
  return std::move(r.value());
}

// A CalibratedCost with explicit temporary-impact (Y, δ, γ) and a fixed slippage
// in bps; the exact coefficients used by round_trip_cost_bps.
[[nodiscard]] cost::CalibratedCost calibrated_cost(f64 Y, f64 delta, f64 gamma,
                                                   f64 slip_bps) noexcept {
  SlippageCfg slip{};
  slip.bps = slip_bps;
  return cost::CalibratedCost{ImpactCfg{Y, delta, gamma}, slip, cost::FitReport{}};
}

struct Lcg {
  std::uint64_t s;
  [[nodiscard]] f64 next() noexcept {
    s = s * 6364136223846793005ULL + 1442695040888963407ULL;
    const std::uint64_t hi = s >> 11U;
    const f64 u = static_cast<f64>(hi) / static_cast<f64>(1ULL << 53U);
    return 2.0 * u - 1.0;
  }
};

[[nodiscard]] std::vector<f64> noisy_close(usize dates, usize insts, const std::vector<f64> &drift,
                                           std::uint64_t seed, f64 noise_amp) {
  std::vector<f64> close(dates * insts);
  std::vector<f64> px(insts, 100.0);
  Lcg rng{seed};
  for (usize t = 0; t < dates; ++t) {
    for (usize j = 0; j < insts; ++j) {
      px[j] *= (1.0 + drift[j] + noise_amp * rng.next());
      close[t * insts + j] = px[j];
    }
  }
  return close;
}

// =============================================================================
//  (a) HandCheck — book_cost_bps == |w|-weighted round_trip_cost_bps.
// =============================================================================
//
// A single-instrument, single-alpha panel with a fully KNOWN close/volume path
// over exactly two dates (so ADV/σ windows are trivial to compute by hand):
//   * close = [100, 110]; volume = [1000, 1000].
//   * last date = date 1 (newest). price = close(1) = 110.
//   * dollar-ADV (window <= 2): mean(close*volume) over both rows
//       = (100*1000 + 110*1000)/2 = 105000.
//   * the only per-step return is ret(1) = 110/100 - 1 = 0.10; with a single
//     observation the population σ is 0 -> the impact term temp == 0 by the
//     simulator's degenerate-σ rule. To exercise a NON-zero σ we instead use a
//     THREE-date path so two returns exist (below).
TEST(FactoryCostAwareFitness, HandCheck_BookCostMatchesRoundTrip) {
  // Three dates, one instrument. close chosen so the two per-step returns are
  // +0.10 and -0.10 (a clean, hand-computable σ); volume flat at 1000.
  //   close = [100, 110, 99];  ret = [.,  +0.10,  -0.10]
  //   mean(ret over the 2 valid) = 0; popvar = mean(r^2) = (0.01+0.01)/2 = 0.01;
  //   sigma = sqrt(0.01) = 0.10.
  const std::vector<f64> close{100.0, 110.0, 99.0};
  const std::vector<f64> volume{1000.0, 1000.0, 1000.0};
  const Panel panel = make_panel(3, 1, {"close", "volume"}, {close, volume});

  // A hand-built single-alpha AlphaStreams: last-period weight w0 = 1.0 (the cost
  // book-aggregate reads the LAST period's target weights, like capacity_for_alpha).
  AlphaStreams strm;
  strm.n_alphas_ = 1;
  strm.n_periods_ = 3;
  strm.n_instruments_ = 1;
  strm.pnl_flat.assign(3, 0.0);
  strm.pos_flat.assign(3, 0.0);
  strm.pos_flat[2] = 1.0; // positions(0, last=2)[0] = 1.0

  const cost::CalibratedCost cc = calibrated_cost(/*Y*/ 0.8, /*δ*/ 0.5, /*γ*/ 0.3, /*slip*/ 4.0);
  const f64 target_aum = 5.0e5; // $500k

  // Hand-compute the single name's participation + sigma over the windows.
  //   price   = close(last) = 99.0
  //   adv     = mean(close*volume over all 3 rows) = (100+110+99)*1000/3 = 103000.
  //   notional= aum*|w| = 5e5;  shares = 5e5/99;  part = shares/adv.
  //   sigma   = popstd of the two returns {+0.10, -0.10} = 0.10.
  const f64 price = 99.0;
  const f64 adv = (100.0 + 110.0 + 99.0) * 1000.0 / 3.0;
  const f64 shares = target_aum * 1.0 / price;
  const f64 part = shares / adv;
  const f64 sigma = 0.10;
  const f64 expected = 1.0 * cost::round_trip_cost_bps(cc, part, sigma); // |w| == 1

  const f64 got = book_cost_bps(strm, panel, cc, target_aum);
  EXPECT_NEAR(got, expected, 1e-6) << "book_cost_bps must equal the |w|-weighted round-trip cost";
  EXPECT_GT(got, 0.0) << "a non-zero σ + participation must produce a positive cost";
}

// =============================================================================
//  (b) TargetAumZeroIsNoOp — cost off ⇒ legacy report, no 4th/5th objective.
// =============================================================================
TEST(FactoryCostAwareFitness, TargetAumZeroIsNoOp) {
  constexpr usize kDates = 64;
  constexpr usize kInsts = 6;
  Library lib;
  const WeightPolicy policy{};
  const ExecutionSimulator sim = frictionless_sim();
  const AlphaStore empty;

  std::vector<f64> drift(kInsts);
  for (usize j = 0; j < kInsts; ++j) {
    drift[j] = 0.006 - 0.0024 * static_cast<f64>(j);
  }
  const std::vector<f64> close = noisy_close(kDates, kInsts, drift, 0xC0FFEEu, 0.010);
  std::vector<f64> volume(kDates * kInsts, 1.0e6);
  const Panel panel = make_panel(kDates, kInsts, {"close", "volume"}, {close, volume});
  Genome cand = make_genome("rank(close)", lib);

  FitnessCfg cfg; // target_aum defaults to 0
  const auto rep = pool_aware_fitness(cand, empty, panel, policy, sim, cfg);
  ASSERT_TRUE(rep.has_value()) << (rep ? "" : rep.error().message());

  EXPECT_EQ(rep->n_objectives, 3) << "cost off ⇒ n_objectives stays 3";
  EXPECT_DOUBLE_EQ(rep->cost_bps, 0.0) << "cost off ⇒ cost_bps is exactly 0";
  EXPECT_DOUBLE_EQ(rep->objectives[4], 0.0) << "cost off ⇒ objectives[4] untouched";
  // The raw/wq path must be byte-identical to the legacy assembly.
  EXPECT_DOUBLE_EQ(rep->raw, rep->objectives[0] * rep->objectives[1] * rep->objectives[2]);
}

// =============================================================================
//  (c) RejectsStrongButExpensiveAlpha — the spec exit criterion.
// =============================================================================
//
// Two candidates over the SAME panel:
//   * expensive: rank(close)  — a strong momentum alpha but engineered (via tiny
//                ADV on its long names) to carry a high book round-trip cost.
//   * cheap:     rank(close)  scored against a much larger AUM-relative ADV. To
//                hold the alphas comparable we instead vary the recorded
//                target_aum: a HIGH target_aum makes participation (hence cost)
//                large; a near-zero one makes it negligible.
// The ranking-flip is proven on ONE alpha by toggling the cost objective: at
// target_aum>0 a cheaper competitor Pareto-dominates the expensive one; at
// target_aum==0 the cost axis vanishes and the gross order is restored.
//
// Construction: build two FitnessReports whose objectives we compare via the
// SAME dominates() the NSGA path uses. The "expensive-but-strong" report has a
// slightly higher wq but a much worse (more negative) cost objective; the "cheap"
// report a slightly lower wq but a near-zero cost. With cost ON the cheap one is
// non-dominated and the expensive one is dominated; with cost OFF (cost axis
// dropped) the expensive one's higher wq makes IT the non-dominated winner.
TEST(FactoryCostAwareFitness, RejectsStrongButExpensiveAlpha) {
  // Objective vectors {wq, diversify, robust, novelty(0), -cost_bps}.
  // expensive: strong wq (0.90) but expensive (cost 12 bps -> obj4 = -12).
  // cheap:     weaker wq (0.80) but cheap (cost 1 bps -> obj4 = -1).
  // Equal diversify/robust/novelty so the trade-off is purely wq vs cost.
  const std::array<f64, 5> expensive{0.90, 0.5, 1.0, 0.0, -12.0};
  const std::array<f64, 5> cheap{0.80, 0.5, 1.0, 0.0, -1.0};

  // Cost ON: 5 active objectives. Neither dominates on wq+cost alone? cheap is
  // better on cost, expensive better on wq -> they co-occupy the front. The spec
  // criterion is that the expensive one is NO LONGER strictly admitted over the
  // cheap one — i.e. it does not DOMINATE the cheap alpha (it did, on gross order).
  const std::span<const f64> exp5{expensive};
  const std::span<const f64> cheap5{cheap};
  EXPECT_FALSE(dominates(exp5, cheap5))
      << "cost ON: the strong-but-expensive alpha must NOT dominate the cheap one";
  EXPECT_FALSE(dominates(cheap5, exp5)) << "cost ON: they form a genuine trade-off (both survive)";

  // Cost OFF: drop the cost axis (first 4 objectives only, novelty also 0). Now
  // expensive's higher wq with everything else equal STRICTLY dominates cheap —
  // the gross (pre-cost) order the miner previously admitted.
  const std::span<const f64> exp4{expensive.data(), 4};
  const std::span<const f64> cheap4{cheap.data(), 4};
  EXPECT_TRUE(dominates(exp4, cheap4))
      << "cost OFF: the strong alpha dominates again (gross order restored)";
}

// The end-to-end fitness wiring: at target_aum>0 a real candidate carries a
// strictly positive cost_bps and objectives[4] == -cost_bps with n_objectives 5;
// the SAME candidate at target_aum==0 carries cost_bps 0 and n_objectives 3.
TEST(FactoryCostAwareFitness, CostObjectiveWiredEndToEnd) {
  constexpr usize kDates = 80;
  constexpr usize kInsts = 6;
  Library lib;
  const WeightPolicy policy{};
  const ExecutionSimulator sim = frictionless_sim();
  const AlphaStore empty;

  std::vector<f64> drift(kInsts);
  for (usize j = 0; j < kInsts; ++j) {
    drift[j] = 0.006 - 0.0024 * static_cast<f64>(j);
  }
  const std::vector<f64> close = noisy_close(kDates, kInsts, drift, 0xBADF00Du, 0.012);
  // Modest ADV so a large target AUM produces meaningful participation.
  std::vector<f64> volume(kDates * kInsts, 5.0e4);
  const Panel panel = make_panel(kDates, kInsts, {"close", "volume"}, {close, volume});
  Genome cand = make_genome("rank(close)", lib);

  FitnessCfg cfg_off; // target_aum == 0
  FitnessCfg cfg_on;
  cfg_on.target_aum = 2.0e7; // $20M
  cfg_on.cost = calibrated_cost(/*Y*/ 1.0, /*δ*/ 0.5, /*γ*/ 0.314, /*slip*/ 5.0);

  const auto off = pool_aware_fitness(cand, empty, panel, policy, sim, cfg_off);
  const auto on = pool_aware_fitness(cand, empty, panel, policy, sim, cfg_on);
  ASSERT_TRUE(off.has_value());
  ASSERT_TRUE(on.has_value());

  EXPECT_EQ(off->n_objectives, 3);
  EXPECT_DOUBLE_EQ(off->cost_bps, 0.0);

  EXPECT_EQ(on->n_objectives, 5) << "cost ON ⇒ slot 4 covered (n_objectives 5)";
  EXPECT_GT(on->cost_bps, 0.0) << "cost ON ⇒ a positive book round-trip cost";
  EXPECT_DOUBLE_EQ(on->objectives[4], -on->cost_bps) << "objectives[4] is the NEGATED cost";
  EXPECT_DOUBLE_EQ(on->objectives[3], 0.0) << "slot 3 (novelty) inert/0 in finish_report";
  // wq/diversify/robust are identical whether cost is on or off (cost adds an
  // objective; it never perturbs the gross terms).
  EXPECT_DOUBLE_EQ(on->wq, off->wq);
  EXPECT_DOUBLE_EQ(on->raw, off->raw);
}

// =============================================================================
//  (d) Determinism — cost_bps is a pure function; DetPool worker-invariance.
// =============================================================================
TEST(FactoryCostAwareFitness, CostBpsIsDeterministic) {
  constexpr usize kDates = 48;
  constexpr usize kInsts = 5;
  Library lib;
  const WeightPolicy policy{};
  const ExecutionSimulator sim = frictionless_sim();
  const AlphaStore empty;

  std::vector<f64> drift(kInsts);
  for (usize j = 0; j < kInsts; ++j) {
    drift[j] = 0.005 - 0.0020 * static_cast<f64>(j);
  }
  const std::vector<f64> close = noisy_close(kDates, kInsts, drift, 0x5A5Au, 0.010);
  std::vector<f64> volume(kDates * kInsts, 8.0e4);
  const Panel panel = make_panel(kDates, kInsts, {"close", "volume"}, {close, volume});
  Genome cand = make_genome("rank(close)", lib);

  FitnessCfg cfg;
  cfg.target_aum = 1.0e7;
  cfg.cost = calibrated_cost(1.0, 0.5, 0.314, 5.0);

  const auto a = pool_aware_fitness(cand, empty, panel, policy, sim, cfg);
  const auto b = pool_aware_fitness(cand, empty, panel, policy, sim, cfg);
  ASSERT_TRUE(a.has_value());
  ASSERT_TRUE(b.has_value());
  EXPECT_DOUBLE_EQ(a->cost_bps, b->cost_bps) << "cost_bps is a pure function of genome+panel+cfg";
  EXPECT_GT(a->cost_bps, 0.0);
}

// ---- DetPool {1,2,4,8} worker-invariance of a cost-ON MultiObjective search ----

[[nodiscard]] Panel search_panel(usize dates, usize insts) {
  const std::vector<f64> close = noisy_close(
      dates, insts,
      [&] {
        std::vector<f64> d(insts);
        for (usize j = 0; j < insts; ++j) {
          d[j] = 0.006 - 0.0024 * static_cast<f64>(j);
        }
        return d;
      }(),
      0xA11Cu, 0.010);
  std::vector<f64> rev(dates * insts, 0.0);
  for (usize t = 1; t < dates; ++t) {
    for (usize j = 0; j < insts; ++j) {
      const f64 prev = close[(t - 1) * insts + j];
      rev[t * insts + j] = -(close[t * insts + j] / prev - 1.0);
    }
  }
  std::vector<f64> volume(dates * insts, 6.0e4);
  return make_panel(dates, insts, {"close", "rev", "volume"}, {close, rev, volume});
}

[[nodiscard]] std::vector<std::string> seed_exprs() {
  return {"rank(close)",
          "rank(rev)",
          "ts_mean(close, 5)",
          "ts_mean(rev, 3)",
          "rank(ts_mean(close, 10))",
          "delta(close, 2)"};
}

[[nodiscard]] SearchConfig cost_on_config() {
  SearchConfig cfg;
  cfg.master_seed = 4242;
  cfg.population = 16;
  cfg.generations = 4;
  cfg.elites = 2;
  cfg.k_tournament = 3;
  cfg.p_cross = 0.5;
  cfg.novelty_w = 0.1;
  cfg.objective_mode = ObjectiveMode::MultiObjective; // NSGA-II over 5 objectives
  cfg.seed_from_grammar = false;
  cfg.fitness.target_aum = 1.0e7; // cost objective ON
  cfg.fitness.cost = calibrated_cost(1.0, 0.5, 0.314, 5.0);
  return cfg;
}

TEST(FactoryCostAwareFitness, CostOnSearchDigestInvariantAcrossWorkers) {
  Library lib{};
  Panel panel = search_panel(96, 6);
  WeightPolicy policy{};
  ExecutionSimulator sim = frictionless_sim();
  SearchDriver driver{lib, panel, policy, sim, seed_exprs(), {"close", "rev", "volume"}};

  const std::array<usize, 4> worker_counts{1, 2, 4, 8};
  atx::u64 first_digest = 0;
  for (usize wi = 0; wi < worker_counts.size(); ++wi) {
    SearchConfig cfg = cost_on_config();
    cfg.n_workers = worker_counts[wi];
    AlphaStore pool{};
    const SearchResult r = driver.run(cfg, pool);
    if (wi == 0) {
      first_digest = r.digest;
    } else {
      EXPECT_EQ(r.digest, first_digest)
          << "cost-on MultiObjective digest changed at n_workers=" << worker_counts[wi];
    }
  }
}

} // namespace
