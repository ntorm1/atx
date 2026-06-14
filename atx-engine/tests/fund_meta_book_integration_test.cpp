// fund_meta_book_integration_test.cpp — P2-S2-5: the five-gates CAPSTONE.
//
// End-to-end integration over the FULL S2 stack — the MetaBook two-pass driver composing
// Sleeve (S2-1) × MetaAllocator (S2-2) × cross_sleeve_risk (S2-3) × netting (S2-4) plus
// combine::compute_metrics and the S1 MultiHorizonOptimizer — proving ALL FIVE sprint
// gates SIMULTANEOUSLY on one realistic multi-sleeve pipeline (synthetic multi-period
// data → a SHARED per-period FactorModel → per-sleeve scripted HorizonSources → a scripted
// returns_at → the trailing-Ω allocate → the netted fund book over a ≥3-period schedule):
//
//   R1  Determinism            — two run(...) on identical inputs ⇒ byte-identical fund
//                                books schedule + digest, capital, turnovers.
//   R2  No-look-ahead/trailing — truncating the schedule/returns/sources after period t
//                                ⇒ fund books at p ≤ t are BYTE-IDENTICAL to the
//                                untruncated run (the trailing risk budget read no future).
//                                THE central S2 gate.
//   R3  Netting                — every period turnover_net ≤ turnover_gross AND
//                                crossing_benefit_bps ≥ 0.
//   R4  Attribution additivity — Σ_s return_contrib ≈ R_fund (~1e-9) AND Σ_s risk_contrib
//                                ≈ sqrt(cᵀΩc) AND Σ_s crossing_credit ≈ total benefit.
//   R7  Boundary pin           — one sleeve + c=[1] every period + one-sleeve netting ⇒
//                                fund_books == sleeve_results[0].books byte-identical, AND
//                                == a standalone MultiHorizonOptimizer::run. THE most
//                                important test of the unit.
//
// Plus boundaries: empty schedule ⇒ Ok empty; two identical sleeves ⇒ crossing benefit 0;
// mismatched callbacks / empty sleeves ⇒ Err. Non-vacuous: ≥2 distinct sleeves, distinct
// scripted sources, a real ≥3-period schedule, per-period distinct models, nonzero returns.

#include <array>   // std::array (per-period model store)
#include <bit>     // std::bit_cast
#include <cmath>   // std::fabs, std::sqrt
#include <cstdint> // std::uint64_t
#include <functional>
#include <span>
#include <utility> // std::move
#include <vector>

#include <gtest/gtest.h>

#include <Eigen/Dense>

#include "atx/core/error.hpp"
#include "atx/core/linalg/linalg.hpp"
#include "atx/core/types.hpp"

#include "atx/engine/fund/cross_sleeve_risk.hpp" // sleeve_return_cov (R4 reference Ω)
#include "atx/engine/fund/meta_book.hpp"         // UNIT UNDER TEST
#include "atx/engine/risk/factor_model.hpp"
#include "atx/engine/risk/horizon.hpp"
#include "atx/engine/risk/multi_horizon.hpp"
#include "atx/engine/risk/multi_period.hpp"

namespace atxtest_fund_meta_book_integration_test {

using atx::f64;
using atx::usize;
using atx::core::linalg::MatX;
using atx::core::linalg::VecX;
using atx::engine::book::CostInputs;
using atx::engine::fund::CapitalWeights;
using atx::engine::fund::MetaAllocatorConfig;
using atx::engine::fund::MetaBook;
using atx::engine::fund::MetaBookConfig;
using atx::engine::fund::MetaBookResult;
using atx::engine::fund::RiskBudgetMethod;
using atx::engine::fund::Sleeve;
using atx::engine::fund::SleeveConfig;
using atx::engine::fund::sleeve_return_cov;
using atx::engine::risk::FactorModel;
using atx::engine::risk::HorizonSource;
using atx::engine::risk::HorizonSources;
using atx::engine::risk::MultiHorizonConfig;
using atx::engine::risk::MultiHorizonOptimizer;
using atx::engine::risk::PositionCap;
using atx::engine::risk::RebalanceSchedule;
using atx::engine::risk::SignalHorizon;

constexpr usize kM = 8U; // instruments
constexpr usize kK = 2U; // factors
constexpr usize kT = 5U; // distinct synthetic periods (schedule {0,1,2,3,4})

// ===========================================================================
//  Synthetic universe — a SHARED per-period FactorModel (one V every sleeve sees), keyed
//  so model_at(s) returns a DISTINCT model per period (non-vacuous). Mirrors the S1
//  integration make_model.
// ===========================================================================
[[nodiscard]] FactorModel make_model(usize period) {
  const f64 ps = 0.07 * static_cast<f64>(period);
  MatX x(static_cast<Eigen::Index>(kM), static_cast<Eigen::Index>(kK));
  for (Eigen::Index i = 0; i < static_cast<Eigen::Index>(kM); ++i) {
    const f64 fi = static_cast<f64>(i);
    x(i, 0) = 0.1 * fi - 0.35 + ps;
    x(i, 1) = 0.05 * static_cast<f64>(i % 3) - 0.05 - 0.5 * ps;
  }
  MatX f = MatX::Identity(static_cast<Eigen::Index>(kK), static_cast<Eigen::Index>(kK));
  VecX d = VecX::Constant(static_cast<Eigen::Index>(kM), 0.2);
  auto r = FactorModel::create(std::move(x), std::move(f), std::move(d), 0U, 1U);
  EXPECT_TRUE(r.has_value()) << (r ? "" : r.error().to_string());
  return std::move(*r);
}

// Stable per-period model store so model_at hands back a reference that outlives run().
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
//  Two distinct sleeve alphas + a scripted per-period returns series. The momentum sleeve
//  and the reversal sleeve are deliberately ANTI-correlated so netting crosses meaningfully
//  (non-vacuous R3) and Ω is a real off-diagonal matrix (non-vacuous R4).
// ===========================================================================
const std::vector<f64> kMomentum = {2.0, -1.0, 0.5, 3.0, -0.5, 1.2, -2.0, 0.8};
const std::vector<f64> kReversal = {-1.5, 0.6, -0.4, -2.0, 1.0, -0.8, 1.4, -0.3};
const SignalHorizon kSlow{40.0};
const SignalHorizon kFast{1.5};

// Period-shifted, sleeve-specific α storage so the spans outlive run(). Sleeve 0 sees a
// slow-decay momentum source; sleeve 1 a fast-decay reversal source.
[[nodiscard]] std::vector<f64> alpha_for(usize sleeve, usize period) {
  std::vector<f64> v = (sleeve == 0U) ? kMomentum : kReversal;
  const f64 sgn = (period % 2U == 0U) ? 1.0 : -1.0;
  for (f64 &x : v) {
    x = sgn * (x + 0.05 * static_cast<f64>(period));
  }
  return v;
}

// A stable [sleeve][period] α store + a returns store (scripted nonzero per-period returns).
class FixtureData {
public:
  FixtureData() {
    for (usize j = 0; j < 2U; ++j) {
      for (usize p = 0; p < kT; ++p) {
        alpha_[j][p] = alpha_for(j, p);
      }
    }
    for (usize p = 0; p < kT; ++p) {
      std::vector<f64> r(kM, 0.0);
      for (usize i = 0; i < kM; ++i) {
        // A small, deterministic, nonzero per-instrument return that varies by period.
        r[i] = 0.01 * (static_cast<f64>((i + p) % 5U) - 2.0) + 0.002 * static_cast<f64>(p);
      }
      returns_[p] = std::move(r);
    }
  }
  [[nodiscard]] std::span<const f64> alpha(usize sleeve, usize period) const {
    return std::span<const f64>(alpha_.at(sleeve).at(period));
  }
  [[nodiscard]] std::span<const f64> returns(usize period) const {
    return std::span<const f64>(returns_.at(period));
  }

private:
  std::array<std::array<std::vector<f64>, kT>, 2U> alpha_{};
  std::array<std::vector<f64>, kT> returns_{};
};

// One source pack: a single (α_t, horizon) pair for the named sleeve at the period.
[[nodiscard]] HorizonSources sleeve_source(const FixtureData &fx, usize sleeve, usize period) {
  HorizonSources hs;
  const SignalHorizon h = (sleeve == 0U) ? kSlow : kFast;
  hs.pairs.emplace_back(fx.alpha(sleeve, period), h);
  return hs;
}

// A minimal MultiHorizonConfig (the dispatch path; H=1 + identity ⇒ the R7 reduction).
[[nodiscard]] MultiHorizonConfig minimal_mh_cfg(usize horizon) {
  MultiHorizonConfig cfg;
  cfg.risk_aversion = 1.0;
  cfg.constraints.gross.gross_leverage = 1.0;
  cfg.constraints.gross.dollar_neutral = true;
  cfg.constraints.pos = PositionCap{1.0};
  cfg.horizon = horizon;
  cfg.trade_rate = 1.0;
  cfg.stacked_mpc = false;
  cfg.prox_max_iters = 64U;
  cfg.capacity_bound_gross = true;
  return cfg;
}

// FNV-1a fold over a flat double sequence (the byte digest).
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
      h_ *= 1099511628211ULL;
    }
  }
  std::uint64_t h_ = 1469598103934665603ULL;
};

[[nodiscard]] std::uint64_t fund_digest(const MetaBookResult &r) {
  Fnv f;
  f.add(r.fund_books);
  f.add(r.report.turnover_net);
  f.add(r.report.turnover_gross);
  return f.value();
}

// A two-sleeve MetaBook with the given allocator config + horizons.
[[nodiscard]] MetaBook make_two_sleeve(const MetaAllocatorConfig &alloc, usize horizon) {
  MetaBook mb;
  mb.cfg.alloc = alloc;
  mb.cfg.risk_lookback = 60U;
  SleeveConfig s0;
  s0.mh = minimal_mh_cfg(horizon);
  s0.capacity_gross = 1e9;
  SleeveConfig s1 = s0;
  mb.sleeves = {Sleeve{s0}, Sleeve{s1}};
  return mb;
}

// The default two-sleeve allocator config (ERC, finite gross, real Kelly).
[[nodiscard]] MetaAllocatorConfig default_alloc() {
  MetaAllocatorConfig a;
  a.method = RiskBudgetMethod::EqualRiskContribution;
  a.fractional_kelly = 0.5;
  a.target_vol = 0.0;
  a.max_gross = 4.0;
  a.solve_iters = 64U;
  return a;
}

// The shared cost for the GATE fixtures. kappa = 0 (NO turnover penalty): the sleeve
// alphas here are small relative to the factor risk, so a nonzero kappa would zero the
// minimal-dispatch PortfolioOptimizer book (the turnover cost dominates the tiny α-edge)
// and make the gates VACUOUS (all-flat fund). With kappa = 0 the sleeves actually trade,
// so R_fund, the crossing benefit and Ω are all genuinely nonzero. round_trip_cost_bps =
// 7.5 still PRICES the crossing benefit; capacity is large so the gross never clips.
[[nodiscard]] CostInputs trading_cost() { return CostInputs{0.0, 7.5, 1e9}; }

// ===========================================================================
//  R1 — DETERMINISM. Two run(...) on identical inputs ⇒ byte-identical fund books +
//  digest, capital, turnovers.
// ===========================================================================
TEST(FundMetaBook, R1_DeterministicByteIdentical) {
  const ModelStore store;
  const FixtureData fx;
  const RebalanceSchedule sched{{0U, 1U, 2U, 3U, 4U}};
  const CostInputs cost = trading_cost();

  const MetaBook mb = make_two_sleeve(default_alloc(), 3U);
  const auto sources_at = [&](usize j, usize p) { return sleeve_source(fx, j, p); };
  const auto model_at = [&](usize p) -> const FactorModel & { return store.at(p); };
  const auto returns_at = [&](usize p) { return fx.returns(p); };

  auto a = mb.run(sched, sources_at, model_at, returns_at, cost);
  auto b = mb.run(sched, sources_at, model_at, returns_at, cost);
  ASSERT_TRUE(a.has_value()) << (a ? "" : a.error().to_string());
  ASSERT_TRUE(b.has_value()) << (b ? "" : b.error().to_string());

  ASSERT_EQ(a->fund_books.size(), sched.periods.size()); // non-vacuous
  EXPECT_EQ(fund_digest(*a), fund_digest(*b)) << "whole-fund digest diverged";

  for (usize s = 0; s < a->fund_books.size(); ++s) {
    ASSERT_EQ(a->fund_books[s].size(), b->fund_books[s].size()) << "period " << s;
    for (usize i = 0; i < a->fund_books[s].size(); ++i) {
      EXPECT_EQ(std::bit_cast<std::uint64_t>(a->fund_books[s][i]),
                std::bit_cast<std::uint64_t>(b->fund_books[s][i]))
          << "fund book byte divergence period " << s << " name " << i;
    }
    ASSERT_EQ(a->capital[s].c.size(), b->capital[s].c.size());
    for (usize j = 0; j < a->capital[s].c.size(); ++j) {
      EXPECT_EQ(std::bit_cast<std::uint64_t>(a->capital[s].c[j]),
                std::bit_cast<std::uint64_t>(b->capital[s].c[j]))
          << "capital byte divergence period " << s << " sleeve " << j;
    }
    EXPECT_EQ(std::bit_cast<std::uint64_t>(a->report.turnover_net[s]),
              std::bit_cast<std::uint64_t>(b->report.turnover_net[s]));
    EXPECT_EQ(std::bit_cast<std::uint64_t>(a->report.turnover_gross[s]),
              std::bit_cast<std::uint64_t>(b->report.turnover_gross[s]));
  }
}

// ===========================================================================
//  R2 — NO-LOOK-AHEAD / TRAILING (the central S2 gate). Run the full schedule {0..4};
//  run the TRUNCATED schedule {0,1,2} with the SAME per-period callbacks. The fund books
//  (and capital, turnovers) for periods ≤ 2 MUST be byte-identical: the capital at s is
//  allocated from the TRAILING window of P&L strictly before s, so appending periods 3,4
//  cannot perturb earlier decisions. Non-vacuous: a multi-period trailing budget that
//  genuinely reads prior P&L (lookback long, periods > 0 exercise it).
// ===========================================================================
TEST(FundMetaBook, R2_TruncationInvarianceNoLookAhead) {
  const ModelStore store;
  const FixtureData fx;
  const CostInputs cost = trading_cost();

  const MetaBook mb = make_two_sleeve(default_alloc(), 3U);
  const auto sources_at = [&](usize j, usize p) { return sleeve_source(fx, j, p); };
  const auto model_at = [&](usize p) -> const FactorModel & { return store.at(p); };
  const auto returns_at = [&](usize p) { return fx.returns(p); };

  auto full = mb.run(RebalanceSchedule{{0U, 1U, 2U, 3U, 4U}}, sources_at, model_at, returns_at, cost);
  auto trunc = mb.run(RebalanceSchedule{{0U, 1U, 2U}}, sources_at, model_at, returns_at, cost);
  ASSERT_TRUE(full.has_value()) << (full ? "" : full.error().to_string());
  ASSERT_TRUE(trunc.has_value()) << (trunc ? "" : trunc.error().to_string());

  ASSERT_EQ(full->fund_books.size(), 5U);
  ASSERT_EQ(trunc->fund_books.size(), 3U);
  for (usize s = 0; s < trunc->fund_books.size(); ++s) {
    ASSERT_EQ(trunc->fund_books[s].size(), full->fund_books[s].size()) << "period " << s;
    for (usize i = 0; i < trunc->fund_books[s].size(); ++i) {
      EXPECT_EQ(std::bit_cast<std::uint64_t>(trunc->fund_books[s][i]),
                std::bit_cast<std::uint64_t>(full->fund_books[s][i]))
          << "LOOK-AHEAD LEAK fund book period " << s << " name " << i;
    }
    ASSERT_EQ(trunc->capital[s].c.size(), full->capital[s].c.size());
    for (usize j = 0; j < trunc->capital[s].c.size(); ++j) {
      EXPECT_EQ(std::bit_cast<std::uint64_t>(trunc->capital[s].c[j]),
                std::bit_cast<std::uint64_t>(full->capital[s].c[j]))
          << "LOOK-AHEAD LEAK capital period " << s << " sleeve " << j;
    }
    EXPECT_EQ(std::bit_cast<std::uint64_t>(trunc->report.turnover_net[s]),
              std::bit_cast<std::uint64_t>(full->report.turnover_net[s]))
        << "turnover leak period " << s;
  }

  // Non-vacuous: the capital actually VARIED across periods (the trailing budget moved),
  // so the invariance above is not trivially "all c identical".
  bool capital_varies = false;
  for (usize s = 1; s < full->capital.size(); ++s) {
    for (usize j = 0; j < full->capital[s].c.size(); ++j) {
      if (full->capital[s].c[j] != full->capital[0].c[j]) {
        capital_varies = true;
      }
    }
  }
  EXPECT_TRUE(capital_varies) << "R2 vacuous: trailing capital never changed across periods";
}

// ===========================================================================
//  R3 — NETTING. Every period turnover_net ≤ turnover_gross AND crossing_benefit_bps ≥ 0.
//  Non-vacuous: the two anti-correlated sleeves cross, so at least one period has a
//  STRICTLY positive crossing benefit (gross > net).
// ===========================================================================
TEST(FundMetaBook, R3_NettingInvariantsHoldEveryPeriod) {
  const ModelStore store;
  const FixtureData fx;
  const RebalanceSchedule sched{{0U, 1U, 2U, 3U, 4U}};
  const CostInputs cost = trading_cost();

  const MetaBook mb = make_two_sleeve(default_alloc(), 3U);
  auto got = mb.run(
      sched, [&](usize j, usize p) { return sleeve_source(fx, j, p); },
      [&](usize p) -> const FactorModel & { return store.at(p); },
      [&](usize p) { return fx.returns(p); }, cost);
  ASSERT_TRUE(got.has_value()) << (got ? "" : got.error().to_string());
  ASSERT_EQ(got->report.turnover_net.size(), sched.periods.size());

  bool some_crossing = false;
  for (usize s = 0; s < sched.periods.size(); ++s) {
    EXPECT_LE(got->report.turnover_net[s], got->report.turnover_gross[s] + 1e-12)
        << "turnover_net > turnover_gross at period " << s;
    EXPECT_GE(got->report.crossing_benefit_bps[s], 0.0) << "negative crossing benefit period " << s;
    if (got->report.crossing_benefit_bps[s] > 1e-9) {
      some_crossing = true;
    }
  }
  EXPECT_TRUE(some_crossing) << "R3 vacuous: the anti-correlated sleeves never crossed";
}

// ===========================================================================
//  R4 — ATTRIBUTION ADDITIVITY. Σ_s return_contrib ≈ R_fund (~1e-9); Σ_s risk_contrib ≈
//  sqrt(cᵀΩc) for the representative (full-sample Ω, final c); Σ_s crossing_credit ≈ the
//  total crossing benefit. Each non-vacuous (nonzero fund return, real Ω, real benefit).
// ===========================================================================
TEST(FundMetaBook, R4_AttributionAdditivity) {
  const ModelStore store;
  const FixtureData fx;
  const RebalanceSchedule sched{{0U, 1U, 2U, 3U, 4U}};
  const CostInputs cost = trading_cost();

  const MetaBook mb = make_two_sleeve(default_alloc(), 3U);
  auto got = mb.run(
      sched, [&](usize j, usize p) { return sleeve_source(fx, j, p); },
      [&](usize p) -> const FactorModel & { return store.at(p); },
      [&](usize p) { return fx.returns(p); }, cost);
  ASSERT_TRUE(got.has_value()) << (got ? "" : got.error().to_string());

  // --- (a) return additivity: Σ_s return_contrib == R_fund = Σ_p Σ_i fund_book·returns ---
  f64 r_fund = 0.0;
  for (usize p = 0; p < sched.periods.size(); ++p) {
    const std::span<const f64> ret = fx.returns(sched.periods[p]);
    const std::vector<f64> &fb = got->fund_books[p];
    for (usize i = 0; i < fb.size(); ++i) {
      r_fund += fb[i] * ret[i];
    }
  }
  f64 sum_ret_contrib = 0.0;
  for (const f64 v : got->report.attribution.return_contrib) {
    sum_ret_contrib += v;
  }
  EXPECT_NEAR(sum_ret_contrib, r_fund, 1e-9) << "R4 return contributions do not sum to R_fund";
  EXPECT_GT(std::fabs(r_fund), 1e-9) << "R4 vacuous: R_fund is ~0";

  // --- (b) risk additivity: Σ_s risk_contrib == sqrt(cᵀΩc) over the representative Ω,c ---
  // Rebuild the representative Ω (full-sample sleeve_return_cov over all r_s) + final c.
  const usize S = got->sleeve_results.size();
  std::vector<std::vector<f64>> pnl(S);
  for (usize s = 0; s < S; ++s) {
    const std::vector<std::vector<f64>> &books = got->sleeve_results[s].books;
    pnl[s].assign(sched.periods.size(), 0.0);
    for (usize p = 0; p < sched.periods.size(); ++p) {
      const std::span<const f64> ret = fx.returns(sched.periods[p]);
      f64 acc = 0.0;
      for (usize i = 0; i < books[p].size(); ++i) {
        acc += books[p][i] * ret[i];
      }
      pnl[s][p] = acc;
    }
  }
  std::vector<std::span<const f64>> panel;
  for (const std::vector<f64> &r : pnl) {
    panel.emplace_back(r);
  }
  auto omega = sleeve_return_cov(std::span<const std::span<const f64>>(panel));
  ASSERT_TRUE(omega.has_value()) << (omega ? "" : omega.error().to_string());
  const std::vector<f64> &c_final = got->capital.back().c;
  // sqrt(cᵀΩc) directly.
  f64 cqc = 0.0;
  for (usize i = 0; i < S; ++i) {
    for (usize j = 0; j < S; ++j) {
      cqc += c_final[i] * (*omega)(static_cast<Eigen::Index>(i), static_cast<Eigen::Index>(j)) *
             c_final[j];
    }
  }
  const f64 sigma_sleeve = (cqc > 0.0) ? std::sqrt(cqc) : 0.0;
  f64 sum_rc = 0.0;
  for (const f64 v : got->report.attribution.risk_contrib) {
    sum_rc += v;
  }
  EXPECT_NEAR(sum_rc, sigma_sleeve, 1e-9) << "R4 risk contributions do not sum to sqrt(cᵀΩc)";
  EXPECT_GT(sigma_sleeve, 1e-9) << "R4 vacuous: sqrt(cᵀΩc) is ~0";

  // --- (c) crossing additivity: Σ_s crossing_credit == total benefit ---
  f64 total_benefit = 0.0;
  for (const f64 v : got->report.crossing_benefit_bps) {
    total_benefit += v;
  }
  f64 sum_credit = 0.0;
  for (const f64 v : got->report.attribution.crossing_credit) {
    sum_credit += v;
  }
  EXPECT_NEAR(sum_credit, total_benefit, 1e-9) << "R4 crossing credits do not sum to total benefit";
  EXPECT_GT(total_benefit, 1e-9) << "R4 vacuous: total crossing benefit is ~0";
}

// ===========================================================================
//  R7 — THE BOUNDARY PIN. One sleeve + a config yielding c=[1] every period (single sleeve
//  + fractional_kelly=1, target_vol=0, max_gross≥1, large capacity) + one-sleeve netting
//  (net==gross, W=w_0, no crossing) ⇒ the fund_books schedule is BYTE-IDENTICAL to that
//  sleeve's MultiHorizonResult.books AND to a standalone MultiHorizonOptimizer::run over
//  the SAME fixture. THE most important test of the unit.
// ===========================================================================
TEST(FundMetaBook, R7_OneSleeveReducesToMultiHorizonByteIdentical) {
  const ModelStore store;
  const FixtureData fx;
  const RebalanceSchedule sched{{0U, 1U, 2U, 3U, 4U}};
  const CostInputs cost = trading_cost();

  // The single-sleeve config: minimal MH (H=1 + identity ⇒ the S1 reduction), large cap.
  SleeveConfig sc;
  sc.mh = minimal_mh_cfg(1U);
  sc.capacity_gross = 1e9;

  // The allocator that yields c=[1.0] every period: single sleeve, full Kelly, no vol-target.
  MetaAllocatorConfig alloc;
  alloc.method = RiskBudgetMethod::EqualRiskContribution;
  alloc.fractional_kelly = 1.0;
  alloc.target_vol = 0.0;
  alloc.max_gross = 4.0; // ≥ 1 so the single-sleeve gross cap never binds (c=1)
  alloc.solve_iters = 64U;

  MetaBook mb;
  mb.cfg.alloc = alloc;
  mb.cfg.risk_lookback = 60U;
  mb.sleeves = {Sleeve{sc}};

  // The SAME per-period identity source both the sleeve and the standalone oracle consume.
  const auto sleeve_src = [&](usize, usize p) {
    HorizonSources hs;
    hs.pairs.emplace_back(fx.alpha(0U, p), SignalHorizon::identity());
    return hs;
  };
  const auto model_at = [&](usize p) -> const FactorModel & { return store.at(p); };
  const auto returns_at = [&](usize p) { return fx.returns(p); };

  auto got = mb.run(sched, sleeve_src, model_at, returns_at, cost);
  ASSERT_TRUE(got.has_value()) << (got ? "" : got.error().to_string());
  ASSERT_EQ(got->capital.size(), sched.periods.size());

  // Sanity: c == [1.0] EXACTLY every period (the pin's premise).
  for (usize s = 0; s < got->capital.size(); ++s) {
    ASSERT_EQ(got->capital[s].c.size(), 1U) << "period " << s;
    EXPECT_EQ(std::bit_cast<std::uint64_t>(got->capital[s].c[0]), std::bit_cast<std::uint64_t>(1.0))
        << "R7 premise broken: c[" << s << "] != 1.0 exactly";
  }

  // Non-vacuous: the pinned books must actually be NONZERO (a byte-identity over all-zero
  // books would pass trivially). trading_cost() has kappa=0 so the sleeve genuinely trades.
  bool some_nonzero = false;
  for (const std::vector<f64> &bk : got->fund_books) {
    for (const f64 v : bk) {
      if (v != 0.0) {
        some_nonzero = true;
      }
    }
  }
  EXPECT_TRUE(some_nonzero) << "R7 vacuous: the pinned fund books are all zero";

  // (1) fund_books == sleeve_results[0].books byte-identical.
  ASSERT_EQ(got->sleeve_results.size(), 1U);
  ASSERT_EQ(got->fund_books.size(), got->sleeve_results[0].books.size());
  for (usize s = 0; s < got->fund_books.size(); ++s) {
    ASSERT_EQ(got->fund_books[s].size(), got->sleeve_results[0].books[s].size()) << "period " << s;
    for (usize i = 0; i < got->fund_books[s].size(); ++i) {
      EXPECT_EQ(std::bit_cast<std::uint64_t>(got->fund_books[s][i]),
                std::bit_cast<std::uint64_t>(got->sleeve_results[0].books[s][i]))
          << "R7 BYTE DIVERGENCE (fund vs sleeve) period " << s << " name " << i;
    }
  }

  // (2) fund_books == a standalone MultiHorizonOptimizer::run over the SAME fixture.
  const auto oracle_src = [&](usize p) {
    HorizonSources hs;
    hs.pairs.emplace_back(fx.alpha(0U, p), SignalHorizon::identity());
    return hs;
  };
  auto oracle = MultiHorizonOptimizer{sc.mh}.run(sched, oracle_src, model_at, cost);
  ASSERT_TRUE(oracle.has_value()) << (oracle ? "" : oracle.error().to_string());
  ASSERT_EQ(got->fund_books.size(), oracle->books.size());
  for (usize s = 0; s < oracle->books.size(); ++s) {
    ASSERT_EQ(got->fund_books[s].size(), oracle->books[s].size()) << "period " << s;
    for (usize i = 0; i < oracle->books[s].size(); ++i) {
      EXPECT_EQ(std::bit_cast<std::uint64_t>(got->fund_books[s][i]),
                std::bit_cast<std::uint64_t>(oracle->books[s][i]))
          << "R7 BYTE DIVERGENCE (fund vs standalone MHO) period " << s << " name " << i;
    }
  }

  // One sleeve ⇒ net == gross, no crossing, every period.
  for (usize s = 0; s < got->report.turnover_net.size(); ++s) {
    EXPECT_EQ(std::bit_cast<std::uint64_t>(got->report.turnover_net[s]),
              std::bit_cast<std::uint64_t>(got->report.turnover_gross[s]))
        << "R7 one-sleeve net != gross at period " << s;
    EXPECT_EQ(got->report.crossing_benefit_bps[s], 0.0) << "R7 one-sleeve crossing != 0 period " << s;
  }
}

// ===========================================================================
//  BOUNDARY — an empty schedule ⇒ Ok with an empty fund-book schedule (degenerate, not Err).
// ===========================================================================
TEST(FundMetaBook, EmptySchedule_OkEmpty) {
  const ModelStore store;
  const FixtureData fx;
  const CostInputs cost{0.0, 0.0, 1e9};
  const MetaBook mb = make_two_sleeve(default_alloc(), 1U);

  auto got = mb.run(
      RebalanceSchedule{{}}, [&](usize j, usize p) { return sleeve_source(fx, j, p); },
      [&](usize p) -> const FactorModel & { return store.at(p); },
      [&](usize p) { return fx.returns(p); }, cost);
  ASSERT_TRUE(got.has_value()) << (got ? "" : got.error().to_string());
  EXPECT_TRUE(got->fund_books.empty());
  EXPECT_TRUE(got->capital.empty());
}

// ===========================================================================
//  BOUNDARY — two IDENTICAL sleeves ⇒ both buy/sell the same names same sign ⇒ NO
//  offsetting flow ⇒ crossing benefit 0 every period (gross == net).
// ===========================================================================
TEST(FundMetaBook, IdenticalSleeves_NoCrossingBenefit) {
  const ModelStore store;
  const FixtureData fx;
  const RebalanceSchedule sched{{0U, 1U, 2U, 3U}};
  const CostInputs cost = trading_cost();

  const MetaBook mb = make_two_sleeve(default_alloc(), 1U);
  // BOTH sleeves see the SAME source (sleeve 0's), so their books are identical ⇒ all flow
  // is same-sign ⇒ net == gross ⇒ zero crossing.
  auto got = mb.run(
      sched, [&](usize /*j*/, usize p) { return sleeve_source(fx, 0U, p); },
      [&](usize p) -> const FactorModel & { return store.at(p); },
      [&](usize p) { return fx.returns(p); }, cost);
  ASSERT_TRUE(got.has_value()) << (got ? "" : got.error().to_string());
  for (usize s = 0; s < sched.periods.size(); ++s) {
    EXPECT_NEAR(got->report.crossing_benefit_bps[s], 0.0, 1e-12)
        << "identical sleeves should not cross at period " << s;
  }
}

// ===========================================================================
//  BOUNDARY (errors) — empty sleeves ⇒ Err; a null callback ⇒ Err.
// ===========================================================================
TEST(FundMetaBook, EmptySleeves_ReturnsErr) {
  const ModelStore store;
  const FixtureData fx;
  const CostInputs cost{0.0, 0.0, 1e9};
  MetaBook mb; // no sleeves
  mb.cfg.alloc = default_alloc();

  auto got = mb.run(
      RebalanceSchedule{{0U, 1U}}, [&](usize j, usize p) { return sleeve_source(fx, j, p); },
      [&](usize p) -> const FactorModel & { return store.at(p); },
      [&](usize p) { return fx.returns(p); }, cost);
  ASSERT_FALSE(got.has_value());
  EXPECT_EQ(got.error().code(), atx::core::ErrorCode::InvalidArgument);
}

TEST(FundMetaBook, NullReturnsCallback_ReturnsErr) {
  const ModelStore store;
  const FixtureData fx;
  const CostInputs cost{0.0, 0.0, 1e9};
  const MetaBook mb = make_two_sleeve(default_alloc(), 1U);

  std::function<std::span<const f64>(usize)> null_returns; // empty std::function
  auto got = mb.run(
      RebalanceSchedule{{0U, 1U}}, [&](usize j, usize p) { return sleeve_source(fx, j, p); },
      [&](usize p) -> const FactorModel & { return store.at(p); }, null_returns, cost);
  ASSERT_FALSE(got.has_value());
  EXPECT_EQ(got.error().code(), atx::core::ErrorCode::InvalidArgument);
}


}  // namespace atxtest_fund_meta_book_integration_test
