// risk_multi_horizon_integration_test.cpp — P2-S1-5: the four-gates CAPSTONE.
//
// End-to-end integration over the FULL S1 stack, proving ALL FOUR sprint gates
// SIMULTANEOUSLY on one realistic pipeline (synthetic multi-period data → a per-
// period FactorModel → a multi-SOURCE horizon trajectory with DISTINCT decays → the
// GP aim collapse → the constrained QP / dispatch → the first-move book over a ≥3-
// period RebalanceSchedule):
//
//   R1  Determinism            — the WHOLE augmented pipeline run twice is byte-
//                                identical (books + turnover + cost_bps; FNV-1a +
//                                per-element std::bit_cast<u64>).
//   R2  No-look-ahead          — running on a TRUNCATED schedule yields books that
//                                are byte-identical to the full-schedule prefix
//                                (a decision at s reads only sources_at(s)/model_at(s)
//                                — appending future periods cannot change earlier
//                                books). PLUS: the per-period trajectory is a pure
//                                function of the CURRENT α_t (independent of any
//                                period > s input).
//   R3  Constraint exactness   — a non-trivial AUGMENTED ConstraintSet (FactorExposure
//                                + BetaNeutral + PositionCap + dollar-neutral + a gross
//                                L1 budget) is satisfied at EVERY realized book within
//                                feas_tol (re-materialize; l−tol ≤ A·w ≤ u+tol; and
//                                Σ|w| ≤ gross + tol).
//   R7  Reduction to S7        — a degenerate config (H=1, ONE identity source, a
//                                minimal GrossNet+PositionCap set, trade_rate=1) yields
//                                a book schedule BYTE-IDENTICAL to MultiPeriodOptimizer
//                                over the SAME per-period alpha/model/cost (the S1-4
//                                boundary pin re-affirmed at the integration level).
//
// Each gate is a separate TEST(MultiHorizonIntegration, …). Non-vacuous: nonzero
// dollar-neutral alphas, ≥2 DISTINCT SignalHorizons, real constraint rows, a ≥3-period
// schedule, PER-PERIOD distinct FactorModels, and has_value()+size asserts before every
// element loop.

#include <array>   // std::array (per-period model store)
#include <bit>     // std::bit_cast
#include <cmath>   // std::fabs, std::isnan, std::isfinite
#include <cstdint> // std::uint64_t
#include <functional>
#include <limits> // std::numeric_limits
#include <span>
#include <utility> // std::move, std::pair
#include <vector>

#include <gtest/gtest.h>

#include <Eigen/Dense>

#include "atx/core/error.hpp"
#include "atx/core/linalg/linalg.hpp"
#include "atx/core/types.hpp"

#include "atx/engine/risk/constraints.hpp"
#include "atx/engine/risk/factor_model.hpp"
#include "atx/engine/risk/horizon.hpp"
#include "atx/engine/risk/multi_horizon.hpp"
#include "atx/engine/risk/multi_period.hpp"
#include "atx/engine/risk/optimizer.hpp"
#include "atx/engine/risk/qp_solver.hpp"

namespace {

using atx::f64;
using atx::usize;
using atx::core::linalg::MatX;
using atx::core::linalg::VecX;
using atx::engine::book::CostInputs;
using atx::engine::risk::BetaNeutral;
using atx::engine::risk::ConstraintSet;
using atx::engine::risk::FactorExposure;
using atx::engine::risk::FactorModel;
using atx::engine::risk::forecast_trajectory;
using atx::engine::risk::HorizonForecast;
using atx::engine::risk::HorizonSource;
using atx::engine::risk::HorizonSources;
using atx::engine::risk::MultiHorizonConfig;
using atx::engine::risk::MultiHorizonOptimizer;
using atx::engine::risk::MultiPeriodConfig;
using atx::engine::risk::MultiPeriodOptimizer;
using atx::engine::risk::OptimizerConfig;
using atx::engine::risk::PositionCap;
using atx::engine::risk::RebalanceSchedule;
using atx::engine::risk::SignalHorizon;

constexpr usize kM = 8U; // instruments
constexpr usize kK = 2U; // factors
constexpr usize kT = 4U; // distinct synthetic periods (schedule {0,1,2,3})

// ===========================================================================
//  Synthetic universe — a PER-PERIOD FactorModel built via FactorModel::create
//  (the make_model idiom from risk_multi_period_test.cpp), keyed so model_at(s)
//  returns a DISTINCT model per period (non-vacuous: the integration is NOT a
//  single static model reused). X varies with the period seed; F is K×K SPD; D > 0.
// ===========================================================================
[[nodiscard]] FactorModel make_model(usize period) {
  const f64 ps = 0.07 * static_cast<f64>(period); // per-period exposure shift
  MatX x(static_cast<Eigen::Index>(kM), static_cast<Eigen::Index>(kK));
  for (Eigen::Index i = 0; i < static_cast<Eigen::Index>(kM); ++i) {
    const f64 fi = static_cast<f64>(i);
    x(i, 0) = 0.1 * fi - 0.35 + ps;                 // small spread, period-shifted
    x(i, 1) = 0.05 * static_cast<f64>(i % 3) - 0.05 - 0.5 * ps;
  }
  MatX f = MatX::Identity(static_cast<Eigen::Index>(kK), static_cast<Eigen::Index>(kK)); // SPD
  VecX d = VecX::Constant(static_cast<Eigen::Index>(kM), 0.2);                           // > 0
  auto r = FactorModel::create(std::move(x), std::move(f), std::move(d), 0U, 1U);
  EXPECT_TRUE(r.has_value()) << (r ? "" : r.error().to_string());
  return std::move(*r);
}

// A stable per-period model store so model_at can return a `const FactorModel&` that
// outlives the run() call (the callback hands back a reference into this array).
class ModelStore {
public:
  ModelStore() {
    for (usize s = 0; s < kT; ++s) {
      models_.emplace_back(make_model(s));
    }
  }
  [[nodiscard]] const FactorModel &at(usize period) const { return models_.at(period); }

private:
  std::vector<FactorModel> models_;
};

// ===========================================================================
//  Two DISTINCT signal sources with DIFFERENT SignalHorizons (multi-horizon, non-
//  vacuous). `momentum` is a slow-decay (persistent) source; `reversal` is a fast-
//  decay one. Both are nonzero and dollar-neutral-able (the optimizer demeans).
// ===========================================================================
const std::vector<f64> kMomentum = {2.0, -1.0, 0.5, 3.0, -0.5, 1.2, -2.0, 0.8};
const std::vector<f64> kReversal = {-1.5, 0.6, -0.4, -2.0, 1.0, -0.8, 1.4, -0.3};
const SignalHorizon kSlow{40.0}; // long halflife ⇒ ~no decay over the horizon
const SignalHorizon kFast{1.5};  // short halflife ⇒ heavy decay over the horizon

// Per-period two-source pack: the SAME two sources at every period (their α_t spans
// outlive the run via the namespace-scope storage above). DISTINCT horizons ⇒ a real
// multi-horizon superposition (slow keeps more of its α_t than fast across H).
[[nodiscard]] HorizonSources two_source_pack(usize /*period*/) {
  HorizonSources hs;
  hs.pairs.emplace_back(std::span<const f64>(kMomentum), kSlow);
  hs.pairs.emplace_back(std::span<const f64>(kReversal), kFast);
  return hs;
}

[[nodiscard]] f64 l1_norm(const std::vector<f64> &w) {
  f64 s = 0.0;
  for (const f64 x : w) {
    s += std::fabs(x);
  }
  return s;
}

// FNV-1a fold over the bit patterns of a flat double sequence (the byte digest).
class Fnv {
public:
  void add(const std::vector<std::vector<f64>> &rows) {
    for (const auto &row : rows) {
      for (const f64 v : row) {
        fold(v);
      }
    }
  }
  void add(const std::vector<f64> &row) {
    for (const f64 v : row) {
      fold(v);
    }
  }
  [[nodiscard]] std::uint64_t value() const noexcept { return h_; }

private:
  void fold(f64 v) noexcept {
    const std::uint64_t bits = std::bit_cast<std::uint64_t>(v);
    for (int byte = 0; byte < 8; ++byte) {
      h_ ^= (bits >> (byte * 8)) & 0xFFULL;
      h_ *= 1099511628211ULL; // FNV prime
    }
  }
  std::uint64_t h_ = 1469598103934665603ULL; // FNV offset basis
};

// Full digest over an entire MultiHorizonResult (books + turnover + cost_bps).
template <class Result> [[nodiscard]] std::uint64_t digest(const Result &r) {
  Fnv f;
  f.add(r.books);
  f.add(r.turnover);
  f.add(r.cost_bps);
  return f.value();
}

// The AUGMENTED ConstraintSet under test in R1/R2/R3: dollar-neutral + gross L1 budget
// + per-name cap + bounded factor exposure on BOTH factors + beta-neutral. Every family
// EXCEPT group/turnover is engaged, so the dispatch falls to the S1-2 ADMM QP and the
// re-materialized A carries real rows: 1 (Σw=0) + M (box) + 2 (factor) + 1 (beta).
const std::vector<f64> kBeta = {1.0, 0.8, 1.2, 0.9, 1.1, 0.7, 1.3, 1.0};
[[nodiscard]] ConstraintSet augmented_set() {
  ConstraintSet cs;
  cs.gross.gross_leverage = 1.0;
  cs.gross.dollar_neutral = true;
  cs.pos = PositionCap{0.4};
  cs.fexp = FactorExposure{{0U, 1U}, {0.20, 0.20}}; // |(Xᵀw)_k| ≤ 0.20 on both factors
  cs.beta = BetaNeutral{std::span<const f64>(kBeta), 0.10};
  return cs;
}

// R3's chosen ADMM budget. The S1-2 default 600 does NOT clear feas_tol for THIS dense
// augmented set (Σw=0 + M box + 2 factor + beta + the gross-L1 aux split ≈ tripled
// primal dim); 1600 outer / 120 inner clears every period's feasibility gate with margin
// (documented in the S1-5 report). The whole-run digest stays byte-deterministic at any
// fixed budget — this value is chosen purely so the feasibility gate passes.
constexpr usize kAugIters = 1600U;
constexpr usize kAugKktIters = 120U;

[[nodiscard]] MultiHorizonConfig augmented_cfg() {
  MultiHorizonConfig cfg;
  cfg.risk_aversion = 0.5;
  cfg.constraints = augmented_set();
  cfg.horizon = 3U;     // a real multi-period lookahead (H=3 ⇒ 4 trajectory rows)
  cfg.trade_rate = 1.0; // full first-move step
  cfg.stacked_mpc = false;
  cfg.qp.iters = kAugIters;
  cfg.qp.kkt_iters = kAugKktIters;
  return cfg;
}

// ===========================================================================
//  R1 — DETERMINISM. The FULL augmented pipeline run twice on identical inputs ⇒
//  the ENTIRE result (books + turnover + cost_bps) is BYTE-IDENTICAL. Two ways:
//  an FNV-1a digest over the whole result AND a per-element std::bit_cast<u64>.
// ===========================================================================
TEST(MultiHorizonIntegration, R1_FullPipelineDeterministicByteIdentical) {
  const ModelStore store;
  const RebalanceSchedule sched{{0U, 1U, 2U, 3U}};
  const CostInputs cost{/*kappa=*/0.25, /*round_trip_cost_bps=*/7.5, /*capacity_gross=*/1e9};

  const auto sources_at = [&](usize s) { return two_source_pack(s); };
  const auto model_at = [&](usize s) -> const FactorModel & { return store.at(s); };

  const MultiHorizonOptimizer mh{augmented_cfg()};
  auto a = mh.run(sched, sources_at, model_at, cost);
  auto b = mh.run(sched, sources_at, model_at, cost);
  ASSERT_TRUE(a.has_value()) << (a ? "" : a.error().to_string());
  ASSERT_TRUE(b.has_value()) << (b ? "" : b.error().to_string());

  // Non-vacuous: a real multi-period augmented run actually happened.
  ASSERT_EQ(a->books.size(), sched.periods.size());
  ASSERT_EQ(b->books.size(), sched.periods.size());
  EXPECT_EQ(digest(*a), digest(*b)) << "whole-result FNV digest diverged across builds";

  // Per-element bit identity (books + turnover + cost_bps), the strongest form.
  for (usize s = 0; s < a->books.size(); ++s) {
    ASSERT_EQ(a->books[s].size(), b->books[s].size()) << "period " << s;
    for (usize i = 0; i < a->books[s].size(); ++i) {
      EXPECT_EQ(std::bit_cast<std::uint64_t>(a->books[s][i]),
                std::bit_cast<std::uint64_t>(b->books[s][i]))
          << "book byte divergence period " << s << " name " << i;
    }
    EXPECT_EQ(std::bit_cast<std::uint64_t>(a->turnover[s]),
              std::bit_cast<std::uint64_t>(b->turnover[s]))
        << "turnover divergence period " << s;
    EXPECT_EQ(std::bit_cast<std::uint64_t>(a->cost_bps[s]),
              std::bit_cast<std::uint64_t>(b->cost_bps[s]))
        << "cost_bps divergence period " << s;
  }
}

// ===========================================================================
//  R2 — NO-LOOK-AHEAD (truncation invariance). Run the full schedule {0,1,2,3};
//  run the TRUNCATED schedule {0,1,2} with the SAME per-period callbacks. The first
//  three books MUST be byte-identical: the decision at period s reads ONLY
//  sources_at(s)/model_at(s), so appending future periods cannot change earlier
//  books (first-move-only, no peek-ahead). The augmented (QP) path is exercised so
//  the invariant covers the dispatch the production augmented set uses.
// ===========================================================================
TEST(MultiHorizonIntegration, R2_TruncationInvarianceNoLookAhead) {
  const ModelStore store;
  const CostInputs cost{0.25, 7.5, 1e9};
  const auto sources_at = [&](usize s) { return two_source_pack(s); };
  const auto model_at = [&](usize s) -> const FactorModel & { return store.at(s); };

  const MultiHorizonOptimizer mh{augmented_cfg()};
  // FULL schedule {0,1,2,3} and TRUNCATED prefix {0,1,2}, SAME callbacks.
  auto full = mh.run(RebalanceSchedule{{0U, 1U, 2U, 3U}}, sources_at, model_at, cost);
  auto trunc = mh.run(RebalanceSchedule{{0U, 1U, 2U}}, sources_at, model_at, cost);
  ASSERT_TRUE(full.has_value()) << (full ? "" : full.error().to_string());
  ASSERT_TRUE(trunc.has_value()) << (trunc ? "" : trunc.error().to_string());

  ASSERT_EQ(full->books.size(), 4U);
  ASSERT_EQ(trunc->books.size(), 3U);
  // The truncated run's books are byte-identical to the full run's prefix — appending
  // period 3 did NOT perturb books[0..2] (proves first-move-only / no future read).
  for (usize s = 0; s < trunc->books.size(); ++s) {
    ASSERT_EQ(trunc->books[s].size(), full->books[s].size()) << "period " << s;
    for (usize i = 0; i < trunc->books[s].size(); ++i) {
      EXPECT_EQ(std::bit_cast<std::uint64_t>(trunc->books[s][i]),
                std::bit_cast<std::uint64_t>(full->books[s][i]))
          << "LOOK-AHEAD LEAK at period " << s << " name " << i;
    }
    EXPECT_EQ(std::bit_cast<std::uint64_t>(trunc->turnover[s]),
              std::bit_cast<std::uint64_t>(full->turnover[s]))
        << "turnover leak period " << s;
  }
}

// The trajectory at a period is a PURE function of the CURRENT α_t — building it from
// ONLY the as-of sources reproduces it byte-for-byte regardless of any "future" input.
// We build the same (α_t, horizon) pack twice — once "standalone", once after fabricating
// arbitrary later-period spans — and confirm the as-of trajectory is identical (there is
// no panel coupling, no future read; the S1-3 PIT structural guarantee, integration-level).
TEST(MultiHorizonIntegration, R2_TrajectoryIsPureFunctionOfCurrentAlpha) {
  const usize H = 3U;
  // The as-of pack (period s): the two real sources with their distinct horizons.
  std::vector<HorizonSource> as_of = {{std::span<const f64>(kMomentum), kSlow},
                                      {std::span<const f64>(kReversal), kFast}};
  auto t_now = forecast_trajectory(std::span<const HorizonSource>(as_of), kM, H);
  ASSERT_TRUE(t_now.has_value()) << (t_now ? "" : t_now.error().to_string());

  // A "future" cross-section that must NOT influence the as-of trajectory in any way.
  const std::vector<f64> future_alpha = {9.0, -9.0, 9.0, -9.0, 9.0, -9.0, 9.0, -9.0};
  std::vector<HorizonSource> as_of_again = {{std::span<const f64>(kMomentum), kSlow},
                                            {std::span<const f64>(kReversal), kFast}};
  // Touch the future span (compute its trajectory) BEFORE rebuilding the as-of one, to
  // make the independence explicit: nothing about period>s leaks into period s.
  std::vector<HorizonSource> fut = {{std::span<const f64>(future_alpha), kSlow}};
  auto t_future = forecast_trajectory(std::span<const HorizonSource>(fut), kM, H);
  ASSERT_TRUE(t_future.has_value());
  auto t_now2 = forecast_trajectory(std::span<const HorizonSource>(as_of_again), kM, H);
  ASSERT_TRUE(t_now2.has_value());

  ASSERT_EQ(t_now->alpha.size(), H + 1U);
  ASSERT_EQ(t_now2->alpha.size(), H + 1U);
  for (usize h = 0; h <= H; ++h) {
    ASSERT_EQ(t_now->alpha[h].size(), kM);
    for (usize i = 0; i < kM; ++i) {
      EXPECT_EQ(std::bit_cast<std::uint64_t>(t_now->alpha[h][i]),
                std::bit_cast<std::uint64_t>(t_now2->alpha[h][i]))
          << "trajectory not a pure function of α_t at h=" << h << " i=" << i;
    }
  }
}

// ===========================================================================
//  R3 — CONSTRAINT EXACTNESS. With the non-trivial AUGMENTED set, EVERY realized
//  book satisfies EVERY claimed constraint within feas_tol: re-materialize the set
//  at each period and check l−tol ≤ A·w ≤ u+tol (dollar-neutral, box, factor, beta)
//  AND the gross L1 budget Σ|w| ≤ gross_leverage + tol.
// ===========================================================================
TEST(MultiHorizonIntegration, R3_AugmentedConstraintsExactEveryPeriod) {
  const ModelStore store;
  const RebalanceSchedule sched{{0U, 1U, 2U, 3U}};
  const CostInputs cost{0.0, 0.0, 1e9};
  const ConstraintSet cs = augmented_set();

  MultiHorizonConfig cfg = augmented_cfg();
  cfg.constraints = cs;

  const MultiHorizonOptimizer mh{cfg};
  auto got = mh.run(
      sched, [&](usize s) { return two_source_pack(s); },
      [&](usize s) -> const FactorModel & { return store.at(s); }, cost);
  ASSERT_TRUE(got.has_value()) << (got ? "" : got.error().to_string());
  ASSERT_EQ(got->books.size(), sched.periods.size());

  const f64 tol = cfg.qp.feas_tol;
  std::vector<f64> w_prev; // re-materialize with the SAME w_prev threading the driver uses
  for (usize s = 0; s < got->books.size(); ++s) {
    const std::vector<f64> &w = got->books[s];
    ASSERT_EQ(w.size(), kM) << "period " << s;

    // Re-materialize against THIS period's exposure matrix (per-period FactorModel).
    const MatX &X = store.at(sched.periods[s]).exposures();
    auto mc = cs.materialize(X, std::span<const f64>(w_prev), kM);
    ASSERT_TRUE(mc.has_value()) << (mc ? "" : mc.error().to_string());

    VecX wv(static_cast<Eigen::Index>(kM));
    for (usize i = 0; i < kM; ++i) {
      wv[static_cast<Eigen::Index>(i)] = w[i];
    }
    // Every LINEAR row (Σw=0, box, factor exposure, beta) within feas_tol.
    const VecX ax = mc->A * wv;
    ASSERT_EQ(ax.size(), mc->l.size());
    ASSERT_EQ(ax.size(), mc->u.size());
    for (Eigen::Index r = 0; r < mc->A.rows(); ++r) {
      EXPECT_LE(ax[r], mc->u[r] + tol) << "period " << s << " row " << r << " over upper";
      EXPECT_GE(ax[r], mc->l[r] - tol) << "period " << s << " row " << r << " under lower";
    }
    // The gross L1 budget (metadata, not a linear A-row): Σ|w| ≤ gross_leverage + tol.
    EXPECT_LE(l1_norm(w), mc->gross_l1_budget + tol)
        << "period " << s << " gross L1 budget violated";
    EXPECT_GE(mc->gross_l1_budget, 0.0); // a real gross budget WAS materialized

    w_prev = w;
  }
}

// ===========================================================================
//  R7 — REDUCTION TO S7. The degenerate config (H=1, ONE SignalHorizon::identity()
//  source, a minimal GrossNet+PositionCap set, trade_rate=1) over a multi-period
//  schedule ⇒ the book schedule is BYTE-IDENTICAL to MultiPeriodOptimizer::run on the
//  SAME per-period alpha/model/cost. (The S1-4 boundary pin, re-affirmed end-to-end
//  with PER-PERIOD distinct models and the integration harness.)
// ===========================================================================
TEST(MultiHorizonIntegration, R7_DegenerateReducesToMultiPeriodByteIdentical) {
  const ModelStore store;
  const RebalanceSchedule sched{{0U, 1U, 2U, 3U}};
  const CostInputs cost{/*kappa=*/0.25, /*round_trip_cost_bps=*/7.5, /*capacity_gross=*/1e9};

  // The SAME per-period alpha both drivers consume (period-shifted so it is genuinely
  // multi-period, dollar-neutral-able, nonzero).
  auto alpha_for = [&](usize s) -> std::vector<f64> {
    std::vector<f64> v = kMomentum;
    const f64 sgn = (s % 2U == 0U) ? 1.0 : -1.0;
    for (f64 &x : v) {
      x = sgn * (x + 0.1 * static_cast<f64>(s));
    }
    return v;
  };
  // Stable storage so the alpha spans outlive both run() calls.
  std::array<std::vector<f64>, kT> alpha_store;
  for (usize s = 0; s < kT; ++s) {
    alpha_store[s] = alpha_for(s);
  }

  // Oracle: MultiPeriodOptimizer over the raw α (the canonical S7 default_oc knobs).
  OptimizerConfig oc;
  oc.risk_aversion = 1.0;
  oc.turnover_penalty = 0.0; // run() overrides κ ← cost.kappa
  oc.gross_leverage = 1.0;
  oc.name_cap = 1.0;
  oc.dollar_neutral = true;
  oc.max_iters = 64U;
  MultiPeriodConfig mpc;
  mpc.single = oc;
  mpc.trade_rate = 1.0;
  mpc.capacity_bound_gross = true;
  const MultiPeriodOptimizer mp{mpc};
  auto oracle = mp.run(
      sched, [&](usize s) { return std::span<const f64>(alpha_store.at(s)); },
      [&](usize s) -> const FactorModel & { return store.at(s); }, cost);
  ASSERT_TRUE(oracle.has_value()) << (oracle ? "" : oracle.error().to_string());

  // Under test: MultiHorizon with H=1, ONE identity source, minimal GrossNet+PositionCap.
  MultiHorizonConfig mhc;
  mhc.risk_aversion = 1.0;
  mhc.constraints.gross.gross_leverage = 1.0;
  mhc.constraints.gross.dollar_neutral = true;
  mhc.constraints.pos = PositionCap{1.0}; // minimal set ⇒ the dispatch path (R7)
  mhc.horizon = 1U;
  mhc.trade_rate = 1.0;
  mhc.prox_max_iters = 64U;
  mhc.capacity_bound_gross = true;
  const MultiHorizonOptimizer mh{mhc};
  auto got = mh.run(
      sched,
      [&](usize s) {
        HorizonSources hs;
        hs.pairs.emplace_back(std::span<const f64>(alpha_store.at(s)), SignalHorizon::identity());
        return hs;
      },
      [&](usize s) -> const FactorModel & { return store.at(s); }, cost);
  ASSERT_TRUE(got.has_value()) << (got ? "" : got.error().to_string());

  // BYTE-IDENTICAL book schedule + turnover + cost_bps.
  ASSERT_EQ(got->books.size(), oracle->books.size());
  for (usize s = 0; s < oracle->books.size(); ++s) {
    ASSERT_EQ(got->books[s].size(), oracle->books[s].size()) << "period " << s;
    for (usize i = 0; i < oracle->books[s].size(); ++i) {
      EXPECT_EQ(std::bit_cast<std::uint64_t>(got->books[s][i]),
                std::bit_cast<std::uint64_t>(oracle->books[s][i]))
          << "R7 BYTE DIVERGENCE at period " << s << " name " << i;
    }
    EXPECT_EQ(std::bit_cast<std::uint64_t>(got->turnover[s]),
              std::bit_cast<std::uint64_t>(oracle->turnover[s]))
        << "R7 turnover diverged period " << s;
    EXPECT_EQ(std::bit_cast<std::uint64_t>(got->cost_bps[s]),
              std::bit_cast<std::uint64_t>(oracle->cost_bps[s]))
        << "R7 cost_bps diverged period " << s;
  }
  EXPECT_EQ(digest(*got), digest(*oracle)) << "R7 whole-result digest diverged";
}

} // namespace
