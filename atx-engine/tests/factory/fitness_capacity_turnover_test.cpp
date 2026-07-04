// atx::engine::factory — S4 (p9): capacity + turnover as first-class NSGA
// objectives. Unit + end-to-end proofs for the two new OPT-IN objective columns
// kObjCapacity (sqrt-law impact capacity score) and kObjTurnover (signal AR(1)
// autocorrelation / alpha-decay persistence score). Both reuse frozen fitters
// (cost::round_trip_cost_bps + cost::capacity_point; alpha::detail::ou_ar1_fit) —
// ZERO new estimator math.

#include <array>
#include <cmath>
#include <cstring>
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
#include "atx/engine/combine/store.hpp"
#include "atx/engine/cost/calibration.hpp"
#include "atx/engine/exec/execution_sim.hpp"
#include "atx/engine/factory/fitness.hpp"
#include "atx/engine/factory/genome.hpp"
#include "atx/engine/factory/search_driver.hpp"
#include "atx/engine/loop/weight_policy.hpp"

namespace atxtest_fitness_capacity_turnover_test {
using atx::f64;
using atx::usize;
using atx::engine::alpha::AlphaStreams;
using atx::engine::alpha::Panel;
using atx::engine::exec::CommissionCfg;
using atx::engine::exec::CommissionMode;
using atx::engine::exec::ImpactCfg;
using atx::engine::exec::SlippageCfg;
using atx::engine::exec::SlippageMode;
using atx::engine::factory::capacity_sqrt_law_score;
using atx::engine::factory::turnover_autocorr_score;
using atx::engine::WeightPolicy;
using atx::engine::alpha::Library;
using atx::engine::combine::AlphaStore;
using atx::engine::exec::ExecutionSimulator;
using atx::engine::exec::FillCfg;
using atx::engine::exec::LatencyCfg;
using atx::engine::exec::VolumeCapCfg;
using atx::engine::factory::FitnessCfg;
using atx::engine::factory::Genome;
using atx::engine::factory::kObjCapacity;
using atx::engine::factory::pool_aware_fitness;
namespace cost = atx::engine::cost;

// ---- end-to-end (pool_aware_fitness) helpers ------------------------------
[[nodiscard]] ExecutionSimulator e2e_sim() {
  return ExecutionSimulator{FillCfg{},
                            SlippageCfg{SlippageMode::VolumeShare, 0.0, 0.0, 0.0, 0.0},
                            ImpactCfg{0.0, 0.5, 0.0},
                            CommissionCfg{CommissionMode::PerShare, 0.0, 0.0, 1.0, 0.0},
                            LatencyCfg{},
                            VolumeCapCfg{1.0}};
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

[[nodiscard]] std::vector<f64> noisy_close(usize dates, usize insts,
                                           const std::vector<f64> &drift, std::uint64_t seed,
                                           f64 noise_amp) {
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

[[nodiscard]] Genome make_genome(std::string_view src, Library &lib) {
  auto parsed = atx::engine::alpha::parse_expr(src, lib);
  EXPECT_TRUE(parsed.has_value()) << (parsed ? "" : parsed.error().message());
  if (!parsed) {
    return Genome{};
  }
  auto info = atx::engine::alpha::analyze(*parsed);
  EXPECT_TRUE(info.has_value()) << (info ? "" : info.error().message());
  if (!info) {
    return Genome{};
  }
  return Genome{std::move(*parsed), std::move(*info), 0};
}

[[nodiscard]] Panel make_panel(usize dates, usize insts, std::vector<std::string> fields,
                               std::vector<std::vector<f64>> cols) {
  auto r = Panel::create(dates, insts, std::move(fields), std::move(cols), {});
  EXPECT_TRUE(r.has_value()) << "panel fixture must build";
  return std::move(r.value());
}

// Deterministic alternating +/-1% oscillation (sigma>0, no RNG) -- ONLY the
// "volume" column differs between the two fixtures (ADV, hence participation).
[[nodiscard]] Panel capacity_panel(f64 volume_level) {
  constexpr usize kDates = 10;
  std::vector<f64> close(kDates);
  std::vector<f64> volume(kDates, volume_level);
  f64 px = 100.0;
  for (usize t = 0; t < kDates; ++t) {
    px *= (t % 2 == 0) ? 1.01 : 0.99;
    close[t] = px;
  }
  return make_panel(kDates, 1, {"close", "volume"}, {close, volume});
}

// A {close, volume} panel over a caller-supplied close path with a flat ADV. Two
// panels sharing the SAME close but differing in `volume` yield an IDENTICAL gross
// edge (pnl depends only on close+weights), isolating the capacity gap to ADV.
[[nodiscard]] Panel close_volume_panel(usize dates, usize insts, const std::vector<f64> &close,
                                       f64 volume_level) {
  const std::vector<f64> volume(dates * insts, volume_level);
  return make_panel(dates, insts, {"close", "volume"}, {close, volume});
}

[[nodiscard]] AlphaStreams full_weight_strm(usize periods, f64 per_period_edge) {
  AlphaStreams s;
  s.n_alphas_ = 1;
  s.n_periods_ = periods;
  s.n_instruments_ = 1;
  s.pnl_flat.assign(periods, per_period_edge); // constant small positive OOS edge
  s.pos_flat.assign(periods, 0.0);
  s.pos_flat[periods - 1] = 1.0; // full weight, last period
  return s;
}

// A CalibratedCost with the default √-impact coefficients but ZERO fixed slippage.
// WHY: the default SlippageCfg{}.bps (5.0) charges a round-trip 10 bps that is
// ADV-INDEPENDENT — it floors EVERY alpha's net edge equally, so for a modest
// gross edge the capacity score collapses to its grid-floor constant for ALL ADV
// levels, masking the very ADV signal the objective exists to measure. Isolating
// the √-law impact (the ONLY ADV-dependent cost term) is what makes the ADV
// discrimination observable. The frozen cost model is untouched — this only
// chooses the test input coefficients.
[[nodiscard]] cost::CalibratedCost impact_only_cost(ImpactCfg imp) {
  SlippageCfg sl{};
  sl.bps = 0.0; // zero fixed slippage -> cost is pure sqrt-law impact
  return cost::CalibratedCost{imp, sl, cost::FitReport{}};
}

// ===========================================================================
//  S4-0 — enum/constant + defaults pin (frozen-prefix, append-only).
// ===========================================================================
TEST(FitnessCapacityTurnover, DefaultsAndEnumPin) {
  using namespace atx::engine::factory;
  static_assert(kMaxObjectives == 9, "S4 must grow kMaxObjectives 7->9");
  static_assert(kObjCapacity == 7, "kObjCapacity must be appended at slot 7");
  static_assert(kObjTurnover == 8, "kObjTurnover must be appended at slot 8");
  const FitnessCfg fc{};
  EXPECT_FALSE(fc.capacity_objective);
  EXPECT_FALSE(fc.turnover_objective);
  const SearchConfig sc{};
  EXPECT_FALSE(sc.capacity_objective);
  EXPECT_FALSE(sc.turnover_objective);
  const FitnessReport fr{};
  EXPECT_EQ(fr.capacity_score, 0.0);
  EXPECT_EQ(fr.turnover_autocorr, 0.0);
}

// ===========================================================================
//  S4-1 — capacity_sqrt_law_score (the kObjCapacity column). Unwired helper.
// ===========================================================================
TEST(CapacityObjective, HighAdvLowImpactScoresAboveLowAdv) {
  // Two single-instrument books identical but for ADV (the "volume" column). The
  // deeper-ADV book bears LOWER sqrt-law impact at any AUM, so its net-edge curve
  // crosses zero at a HIGHER capacity AUM -> a strictly higher bounded score. Both
  // sit in the interior (0,1) of the transform (neither floored nor saturated).
  const Panel high_adv = capacity_panel(1.0e6); // deep ADV -> low participation
  const Panel low_adv = capacity_panel(1.0e5);  // thin ADV -> high participation
  const AlphaStreams strm = full_weight_strm(10, 0.002); // 20 bps gross edge/period
  const cost::CalibratedCost cc = impact_only_cost(ImpactCfg{0.8, 0.5, 0.3});
  constexpr f64 kTargetAum = 1.0e6;

  const f64 score_high = capacity_sqrt_law_score(strm, high_adv, cc, kTargetAum);
  const f64 score_low = capacity_sqrt_law_score(strm, low_adv, cc, kTargetAum);

  EXPECT_GT(score_high, score_low)
      << "the deep-ADV/low-impact book must score a HIGHER capacity headroom";
  EXPECT_GT(score_low, 0.0) << "sanity: the thin-ADV book still has SOME capacity";
  EXPECT_LT(score_high, 1.0) << "deep-ADV book still crosses within the grid (not saturated)";
  EXPECT_LE(score_low, 1.0);
}

TEST(CapacityObjective, NonPositiveTargetAumIsADocumentedNoOp) {
  const Panel panel = capacity_panel(1.0e6);
  const AlphaStreams strm = full_weight_strm(10, 0.002);
  const cost::CalibratedCost cc = impact_only_cost(ImpactCfg{0.8, 0.5, 0.3});
  EXPECT_EQ(capacity_sqrt_law_score(strm, panel, cc, 0.0), 0.0);
  EXPECT_EQ(capacity_sqrt_law_score(strm, panel, cc, -1.0), 0.0);
}

TEST(CapacityObjective, PureFunction_TwiceRunByteIdentical) {
  const Panel panel = capacity_panel(5.0e5);
  const AlphaStreams strm = full_weight_strm(10, 0.002);
  const cost::CalibratedCost cc = impact_only_cost(ImpactCfg{0.6, 0.6, 0.2});
  const f64 a = capacity_sqrt_law_score(strm, panel, cc, 2.5e5);
  const f64 b = capacity_sqrt_law_score(strm, panel, cc, 2.5e5);
  EXPECT_EQ(a, b);
}

TEST(CapacityObjective, NeverInfOrNaN) {
  // A near-zero impact coefficient (Y~0) -> the net-edge curve never crosses zero
  // on the grid -> capacity_point returns +inf -- the EXACT case the bounded
  // transform must absorb without ever handing NSGA an inf/NaN objective.
  const Panel panel = capacity_panel(1.0e9);
  const AlphaStreams strm = full_weight_strm(10, 0.001);
  const cost::CalibratedCost cc = impact_only_cost(ImpactCfg{1.0e-9, 0.5, 0.0});
  const f64 score = capacity_sqrt_law_score(strm, panel, cc, 1.0e6);
  EXPECT_TRUE(std::isfinite(score));
  EXPECT_GE(score, 0.0);
  EXPECT_LE(score, 1.0);
}

// ===========================================================================
//  S4-2 — turnover_autocorr_score (the kObjTurnover column). Unwired helper.
// ===========================================================================

// An EXACT (noise-free) AR(1) process x[t] = mu + (x0-mu)*phi^t satisfies
// x[t] = mu*(1-phi) + phi*x[t-1] -- OLS recovers b==phi with ZERO residual.
// mu=0.5, phi=0.9, x0=1.0 -> a slowly-decaying, highly persistent series.
[[nodiscard]] AlphaStreams persistent_strm(usize periods) {
  AlphaStreams s;
  s.n_alphas_ = 1;
  s.n_periods_ = periods;
  s.n_instruments_ = 1;
  s.pnl_flat.assign(periods, 0.0);
  s.pos_flat.resize(periods);
  for (usize t = 0; t < periods; ++t) {
    s.pos_flat[t] = 0.5 + 0.5 * std::pow(0.9, static_cast<f64>(t));
  }
  return s;
}

// An EXACT phi=-1 alternation: x[t] = -x[t-1] -- maximal churn, b==-1.0.
[[nodiscard]] AlphaStreams churny_strm(usize periods) {
  AlphaStreams s;
  s.n_alphas_ = 1;
  s.n_periods_ = periods;
  s.n_instruments_ = 1;
  s.pnl_flat.assign(periods, 0.0);
  s.pos_flat.resize(periods);
  for (usize t = 0; t < periods; ++t) {
    s.pos_flat[t] = (t % 2 == 0) ? 0.5 : -0.5;
  }
  return s;
}

TEST(TurnoverObjective, PersistentSeriesScoresAboveChurnySeries) {
  const AlphaStreams slow = persistent_strm(20);
  const AlphaStreams churn = churny_strm(20);
  const f64 score_slow = turnover_autocorr_score(slow);
  const f64 score_churn = turnover_autocorr_score(churn);
  EXPECT_NEAR(score_slow, 0.9, 1e-6) << "exact AR(1) phi=0.9 must recover b~=0.9";
  EXPECT_NEAR(score_churn, -1.0, 1e-6) << "exact alternation must recover b~=-1.0";
  EXPECT_GT(score_slow, score_churn);
}

TEST(TurnoverObjective, ConstantSeriesDegenerateFitIsSkippedNotZeroed) {
  // Instrument 0 constant (zero predictor variance -> NaN fit, must be SKIPPED);
  // instrument 1 the persistent series -- the score must reflect ONLY inst 1.
  AlphaStreams s;
  s.n_alphas_ = 1;
  s.n_periods_ = 20;
  s.n_instruments_ = 2;
  s.pnl_flat.assign(20, 0.0);
  s.pos_flat.assign(40, 0.0);
  for (usize t = 0; t < 20; ++t) {
    s.pos_flat[t * 2 + 0] = 1.0;                                            // constant
    s.pos_flat[t * 2 + 1] = 0.5 + 0.5 * std::pow(0.9, static_cast<f64>(t)); // persistent
  }
  EXPECT_NEAR(turnover_autocorr_score(s), 0.9, 1e-6);
}

TEST(TurnoverObjective, ZeroLastPeriodWeightExcludesInstrument) {
  AlphaStreams s = churny_strm(20);
  // Zero the last-period weight for the (only) instrument -> no contributor.
  s.pos_flat.back() = 0.0;
  EXPECT_EQ(turnover_autocorr_score(s), 0.0);
}

TEST(TurnoverObjective, PureFunction_TwiceRunByteIdentical) {
  const AlphaStreams s = persistent_strm(15);
  EXPECT_EQ(turnover_autocorr_score(s), turnover_autocorr_score(s));
}

// ===========================================================================
//  S4-4 — determinism classes threaded through the REAL pool_aware_fitness.
// ===========================================================================

// (b) end-to-end: two panels sharing the SAME close (hence identical gross edge)
// but differing ONLY in ADV ("volume"). With the capacity objective ON, the
// deeper-ADV panel's kObjCapacity column is STRICTLY greater and n_objectives
// covers the slot -- proving the wire populates the real objective vector, not
// just the standalone helper.
TEST(FitnessCapacityTurnover, EndToEndObjectivesReflectCapacityGap) {
  constexpr usize kDates = 80, kInsts = 6;
  Library lib;
  const WeightPolicy policy{};
  const ExecutionSimulator sim = e2e_sim();
  const AlphaStore empty;
  std::vector<f64> drift(kInsts);
  for (usize j = 0; j < kInsts; ++j) {
    drift[j] = 0.006 - 0.0024 * static_cast<f64>(j);
  }
  const std::vector<f64> close = noisy_close(kDates, kInsts, drift, 0xBADF00Du, 0.012);
  const Panel high_adv = close_volume_panel(kDates, kInsts, close, 1.0e5); // deep ADV
  const Panel low_adv = close_volume_panel(kDates, kInsts, close, 1.0e4);  // thin ADV
  const Genome cand = make_genome("rank(close)", lib);

  FitnessCfg cfg{};
  cfg.capacity_objective = true;
  cfg.target_aum = 1.0e6;
  cfg.cost = impact_only_cost(ImpactCfg{1.0, 0.5, 0.314}); // zero slippage -> pure impact

  const auto rep_high = pool_aware_fitness(cand, empty, high_adv, policy, sim, cfg);
  const auto rep_low = pool_aware_fitness(cand, empty, low_adv, policy, sim, cfg);
  ASSERT_TRUE(rep_high.has_value()) << (rep_high ? "" : rep_high.error().message());
  ASSERT_TRUE(rep_low.has_value()) << (rep_low ? "" : rep_low.error().message());

  EXPECT_GT(rep_high->objectives[kObjCapacity], rep_low->objectives[kObjCapacity])
      << "deeper ADV must yield a strictly higher kObjCapacity column end-to-end";
  EXPECT_GE(rep_high->n_objectives, kObjCapacity + 1)
      << "capacity ON must bump n_objectives to cover slot kObjCapacity";
  EXPECT_GT(rep_high->objectives[kObjCapacity], 0.0);
  EXPECT_LT(rep_high->objectives[kObjCapacity], 1.0) << "interior, not saturated";
  EXPECT_GT(rep_low->objectives[kObjCapacity], 0.0);
}

// (c) twice-run: identical (genome, panel, pool, cfg) -> bit-identical objective
// vectors, with BOTH new objectives active.
TEST(FitnessCapacityTurnover, TwiceRun_ObjectivesBitIdentical) {
  constexpr usize kDates = 64, kInsts = 5;
  Library lib;
  const WeightPolicy policy{};
  const ExecutionSimulator sim = e2e_sim();
  const AlphaStore empty;
  std::vector<f64> drift(kInsts);
  for (usize j = 0; j < kInsts; ++j) {
    drift[j] = 0.005 - 0.0020 * static_cast<f64>(j);
  }
  const std::vector<f64> close = noisy_close(kDates, kInsts, drift, 0x5A5Au, 0.010);
  const Panel panel = close_volume_panel(kDates, kInsts, close, 5.0e4);
  const Genome cand = make_genome("rank(close)", lib);

  FitnessCfg cfg{};
  cfg.capacity_objective = true;
  cfg.turnover_objective = true;
  cfg.target_aum = 5.0e5;
  cfg.cost = impact_only_cost(ImpactCfg{1.0, 0.5, 0.314});

  const auto a = pool_aware_fitness(cand, empty, panel, policy, sim, cfg);
  const auto b = pool_aware_fitness(cand, empty, panel, policy, sim, cfg);
  ASSERT_TRUE(a.has_value());
  ASSERT_TRUE(b.has_value());
  EXPECT_EQ(a->n_objectives, b->n_objectives);
  EXPECT_EQ(std::memcmp(a->objectives.data(), b->objectives.data(),
                        sizeof(f64) * a->objectives.size()),
            0)
      << "objective vector must be bit-identical across two identical calls";
}

} // namespace atxtest_fitness_capacity_turnover_test
