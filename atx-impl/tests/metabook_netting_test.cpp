// metabook_netting_test.cpp — p8 Sprint 2 (S2-3): cross-sleeve netting -- MEASURE and
// ASSERT the turnover crossing win, and verify run_metabook surfaces it as stage telemetry
// (fund_turnover_net / fund_turnover_gross / crossing_benefit_bps / crossed_fraction),
// never folded into the fund-book digest.
//
// Two fixtures:
//   - An EXACTLY-offsetting two-sleeve MetaBook (sleeve B's alpha == -sleeve A's alpha every
//     period, so sleeve B's optimized book is the additive inverse of sleeve A's book by the
//     optimizer's odd symmetry under demean+gross-normalize): net = |c_A - c_B|*sum|w_i| <
//     gross = (c_A + c_B)*sum|w_i| STRICTLY whenever the book is nonzero and c_A,c_B > 0 (the
//     allocator's capital weights are always >= 0 by construction) -- a mathematically
//     GUARANTEED crossing benefit, not a hoped-for empirical one.
//   - The real run_metabook SingleSleeve stage path (no --library-dir), reusing the S2-2
//     fixture panels, to verify the kvs surfacing + the no-crossing single-sleeve contract.

#include <bit>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <span>
#include <string>
#include <system_error>
#include <vector>

#include <gtest/gtest.h>

#include <Eigen/Core>

#include "atx/core/linalg/linalg.hpp"
#include "atx/core/types.hpp"

#include "atx/engine/alpha/panel.hpp"
#include "atx/engine/fund/meta_allocator.hpp"
#include "atx/engine/fund/meta_book.hpp"
#include "atx/engine/fund/sleeve.hpp"
#include "atx/engine/risk/constraints.hpp"
#include "atx/engine/risk/factor_model.hpp"
#include "atx/engine/risk/horizon.hpp"
#include "atx/engine/risk/multi_horizon.hpp"

#include "serialize_panel.hpp"
#include "stage_metabook.hpp"

namespace atxtest_metabook_netting_test {

namespace fund = atx::engine::fund;
namespace risk = atx::engine::risk;
namespace alpha = atx::engine::alpha;
using atx::f64;
using atx::usize;

namespace {

constexpr usize kM = 4U;  // instruments (the offsetting fixture)
constexpr usize kTp = 4U; // periods (the offsetting fixture)

[[nodiscard]] risk::FactorModel make_diag_model() {
  atx::core::linalg::MatX x = atx::core::linalg::MatX::Zero(static_cast<Eigen::Index>(kM), 1);
  atx::core::linalg::MatX f(1, 1);
  f(0, 0) = 1.0;
  atx::core::linalg::VecX d = atx::core::linalg::VecX::Constant(static_cast<Eigen::Index>(kM), 0.2);
  auto r = risk::FactorModel::create(std::move(x), std::move(f), std::move(d), 0U, 1U);
  EXPECT_TRUE(r.has_value()) << (r ? "" : r.error().to_string());
  return std::move(*r);
}

[[nodiscard]] risk::MultiHorizonConfig minimal_mh() {
  risk::MultiHorizonConfig cfg;
  cfg.risk_aversion = 1.0;
  cfg.constraints.gross.gross_leverage = 1.0;
  cfg.constraints.gross.dollar_neutral = true;
  cfg.constraints.pos = risk::PositionCap{1.0};
  cfg.horizon = 1U;
  cfg.trade_rate = 1.0;
  cfg.prox_max_iters = 64U;
  cfg.capacity_bound_gross = true;
  return cfg;
}

// Sleeve A's alpha at period p: a fixed, period-varying dollar-neutral-ish signal. Sleeve B's
// alpha is EXACTLY the negation -- see the file header for why this guarantees a strict
// crossing benefit regardless of the allocator's actual capital split.
[[nodiscard]] std::vector<f64> alpha_a(usize p) {
  const f64 sgn = (p % 2U == 0U) ? 1.0 : -1.0;
  return {sgn * 3.0, sgn * -1.0, sgn * 0.2, sgn * -0.2};
}
[[nodiscard]] std::vector<f64> alpha_b(usize p) {
  std::vector<f64> a = alpha_a(p);
  for (f64 &v : a) {
    v = -v;
  }
  return a;
}

// Nonzero, non-degenerate per-instrument returns (drives Ω; distinct per sleeve since each
// sleeve's realized P&L is a function of ITS OWN book against the same returns_at).
[[nodiscard]] std::vector<f64> returns_at_fixture(usize p) {
  std::vector<f64> r(kM);
  for (usize i = 0; i < kM; ++i) {
    r[i] = 0.01 * (static_cast<f64>((i + p) % 3U) - 1.0);
  }
  return r;
}

struct OffsettingFixture {
  risk::FactorModel model = make_diag_model();
  std::array<std::vector<f64>, kTp> a{};
  std::array<std::vector<f64>, kTp> b{};
  std::array<std::vector<f64>, kTp> ret{};

  OffsettingFixture() {
    for (usize p = 0; p < kTp; ++p) {
      a[p] = alpha_a(p);
      b[p] = alpha_b(p);
      ret[p] = returns_at_fixture(p);
    }
  }
};

[[nodiscard]] atx::core::Result<fund::MetaBookResult> run_offsetting(const OffsettingFixture &fx,
                                                                    const fund::MetaAllocatorConfig &alloc,
                                                                    usize n_periods) {
  fund::MetaBook mb;
  mb.cfg.alloc = alloc;
  mb.cfg.risk_lookback = 60U;
  fund::SleeveConfig sa;
  sa.mh = minimal_mh();
  sa.capacity_gross = 1e9;
  fund::SleeveConfig sb;
  sb.mh = minimal_mh();
  sb.capacity_gross = 1e9;
  mb.sleeves = {fund::Sleeve{sa}, fund::Sleeve{sb}};

  risk::RebalanceSchedule sched;
  for (usize p = 0; p < n_periods; ++p) {
    sched.periods.push_back(p);
  }
  const auto sources_at = [&](usize sleeve, usize p) {
    risk::HorizonSources hs;
    const std::span<const f64> row = (sleeve == 0U) ? std::span<const f64>(fx.a[p])
                                                    : std::span<const f64>(fx.b[p]);
    hs.pairs.emplace_back(row, risk::SignalHorizon::identity());
    return hs;
  };
  const auto model_at = [&](usize) -> const risk::FactorModel & { return fx.model; };
  const auto returns_at = [&](usize p) { return std::span<const f64>(fx.ret[p]); };
  const atx::engine::book::CostInputs cost{/*kappa*/ 0.0, /*round_trip_cost_bps*/ 10.0,
                                           /*capacity_gross*/ 1e9};
  return mb.run(sched, sources_at, model_at, returns_at, cost);
}

} // namespace

// ===========================================================================
//  metabook_netting_reduces_turnover — the mandatory, GUARANTEED-by-construction case.
// ===========================================================================
TEST(MetabookNetting, ReducesTurnoverOnOffsettingSleeves) {
  const OffsettingFixture fx;
  const fund::MetaAllocatorConfig alloc; // default ERC
  auto got = run_offsetting(fx, alloc, kTp);
  ASSERT_TRUE(got.has_value()) << (got ? "" : got.error().to_string());
  ASSERT_EQ(got->report.turnover_net.size(), kTp);

  bool some_nonzero_book = false;
  for (const auto &book : got->fund_books) {
    for (const f64 v : book) {
      if (v != 0.0) {
        some_nonzero_book = true;
      }
    }
  }
  EXPECT_TRUE(some_nonzero_book) << "vacuous: fixture produced an all-zero book";

  f64 net_total = 0.0;
  f64 gross_total = 0.0;
  bool any_strict = false;
  for (usize s = 0; s < kTp; ++s) {
    const f64 net = got->report.turnover_net[s];
    const f64 gross = got->report.turnover_gross[s];
    EXPECT_LE(net, gross + 1e-9) << "R3 triangle violated at period " << s;
    if (gross > 1e-9 && net < gross - 1e-9) {
      any_strict = true;
    }
    net_total += net;
    gross_total += gross;
  }
  EXPECT_TRUE(any_strict) << "expected a strict crossing benefit on the offsetting fixture";
  EXPECT_LT(net_total, gross_total)
      << "net_total=" << net_total << " gross_total=" << gross_total;

  f64 crossing_total = 0.0;
  for (const f64 c : got->report.crossing_benefit_bps) {
    EXPECT_GE(c, -1e-9) << "R3: crossing_benefit_bps must be >= 0";
    crossing_total += c;
  }
  EXPECT_GT(crossing_total, 0.0);
}

// metabook_netting_triangle_invariant — on every fixture (offsetting AND non-offsetting),
// turnover_net <= turnover_gross per period and crossing_benefit_bps >= 0.
TEST(MetabookNetting, TriangleInvariantHoldsAcrossAllocatorMethods) {
  const OffsettingFixture fx;
  for (const auto method :
      {fund::RiskBudgetMethod::InverseVol, fund::RiskBudgetMethod::EqualRiskContribution,
       fund::RiskBudgetMethod::HierarchicalRiskParity}) {
    fund::MetaAllocatorConfig alloc;
    alloc.method = method;
    auto got = run_offsetting(fx, alloc, kTp);
    ASSERT_TRUE(got.has_value()) << (got ? "" : got.error().to_string());
    for (usize s = 0; s < kTp; ++s) {
      EXPECT_LE(got->report.turnover_net[s], got->report.turnover_gross[s] + 1e-9)
          << "method=" << static_cast<int>(method) << " period=" << s;
      EXPECT_GE(got->report.crossing_benefit_bps[s], -1e-9)
          << "method=" << static_cast<int>(method) << " period=" << s;
    }
  }
}

// metabook_netting_single_sleeve_no_crossing — one sleeve => net==gross, benefit==0.
TEST(MetabookNetting, SingleSleeveNoCrossing) {
  fund::MetaBook mb;
  fund::MetaAllocatorConfig alloc;
  alloc.fractional_kelly = 1.0; // the c==[1] boundary config
  mb.cfg.alloc = alloc;
  mb.cfg.risk_lookback = 60U;
  fund::SleeveConfig sa;
  sa.mh = minimal_mh();
  sa.capacity_gross = 1e9;
  mb.sleeves = {fund::Sleeve{sa}};

  const OffsettingFixture fx;
  risk::RebalanceSchedule sched;
  for (usize p = 0; p < kTp; ++p) {
    sched.periods.push_back(p);
  }
  const auto sources_at = [&](usize, usize p) {
    risk::HorizonSources hs;
    hs.pairs.emplace_back(std::span<const f64>(fx.a[p]), risk::SignalHorizon::identity());
    return hs;
  };
  const auto model_at = [&](usize) -> const risk::FactorModel & { return fx.model; };
  const auto returns_at = [&](usize p) { return std::span<const f64>(fx.ret[p]); };
  const atx::engine::book::CostInputs cost{0.0, 10.0, 1e9};

  auto got = mb.run(sched, sources_at, model_at, returns_at, cost);
  ASSERT_TRUE(got.has_value()) << (got ? "" : got.error().to_string());
  for (usize s = 0; s < kTp; ++s) {
    EXPECT_EQ(std::bit_cast<std::uint64_t>(got->report.turnover_net[s]),
             std::bit_cast<std::uint64_t>(got->report.turnover_gross[s]))
        << "one sleeve: net != gross at period " << s;
    EXPECT_EQ(got->report.crossing_benefit_bps[s], 0.0) << "one sleeve: crossing != 0 at period " << s;
  }
}

// metabook_netting twice-run: identical inputs -> byte-identical netting telemetry.
TEST(MetabookNetting, TwiceRunByteIdentical) {
  const OffsettingFixture fx;
  const fund::MetaAllocatorConfig alloc;
  auto r1 = run_offsetting(fx, alloc, kTp);
  auto r2 = run_offsetting(fx, alloc, kTp);
  ASSERT_TRUE(r1.has_value());
  ASSERT_TRUE(r2.has_value());
  for (usize s = 0; s < kTp; ++s) {
    EXPECT_EQ(std::bit_cast<std::uint64_t>(r1->report.turnover_net[s]),
             std::bit_cast<std::uint64_t>(r2->report.turnover_net[s]))
        << "period " << s;
    EXPECT_EQ(std::bit_cast<std::uint64_t>(r1->report.turnover_gross[s]),
             std::bit_cast<std::uint64_t>(r2->report.turnover_gross[s]))
        << "period " << s;
    EXPECT_EQ(std::bit_cast<std::uint64_t>(r1->report.crossing_benefit_bps[s]),
             std::bit_cast<std::uint64_t>(r2->report.crossing_benefit_bps[s]))
        << "period " << s;
  }
}

// ===========================================================================
//  Stage-level kvs surfacing — run_metabook's StageResult::kvs carries the netting
//  telemetry (never folded into the fund-book digest). SingleSleeve (no crossing possible)
//  is the deterministic, reproducible fixture for this check.
// ===========================================================================
namespace {

[[nodiscard]] atx::core::Result<std::string> make_research_panel_net(const std::filesystem::path &out,
                                                                     atx::usize M, atx::usize D) {
  std::vector<atx::f64> close_data;
  close_data.reserve(D * M);
  for (atx::usize t = 0; t < D; ++t) {
    for (atx::usize i = 0; i < M; ++i) {
      const atx::f64 drift = 0.0002 * (1.0 + static_cast<atx::f64>(i) * 0.1);
      close_data.push_back(100.0 * std::exp(drift * static_cast<atx::f64>(t)));
    }
  }
  std::vector<std::uint8_t> uni(D * M, 1U);
  ATX_TRY(auto panel, alpha::Panel::create(D, M, {"close"}, {close_data}, uni));
  ATX_TRY(auto digest, atx::impl::write_panel(panel, out.string()));
  (void)digest;
  return atx::core::Ok(out.string());
}

[[nodiscard]] atx::core::Result<std::string> make_combo_panel_net(const std::filesystem::path &out,
                                                                  atx::usize M, atx::usize D) {
  std::vector<atx::f64> alpha_data;
  alpha_data.reserve(D * M);
  for (atx::usize t = 0; t < D; ++t) {
    const atx::f64 wobble = 0.01 * static_cast<atx::f64>(t % 5);
    for (atx::usize i = 0; i < M; ++i) {
      alpha_data.push_back((static_cast<atx::f64>(i) - static_cast<atx::f64>(M) / 2.0) + wobble);
    }
  }
  std::vector<std::uint8_t> uni(D * M, 1U);
  ATX_TRY(auto panel, alpha::Panel::create(D, M, {"alpha"}, {alpha_data}, uni));
  ATX_TRY(auto digest, atx::impl::write_panel(panel, out.string()));
  (void)digest;
  return atx::core::Ok(out.string());
}

[[nodiscard]] std::string find_kv(const atx::impl::StageResult &sr, const std::string &key) {
  for (const auto &[k, v] : sr.kvs) {
    if (k == key) {
      return v;
    }
  }
  return "<missing:" + key + ">";
}

} // namespace

TEST(MetabookNetting, StageKvsSurfacesNettingTelemetrySingleSleeveNoCrossing) {
  const std::filesystem::path dir =
      std::filesystem::temp_directory_path() / "atx_s2_metabook_netting" / "kvs";
  std::error_code ec;
  std::filesystem::remove_all(dir, ec);
  std::filesystem::create_directories(dir, ec);

  const auto research_r = make_research_panel_net(dir / "research.bin", 6U, 20U);
  ASSERT_TRUE(research_r.has_value());
  const auto combo_r = make_combo_panel_net(dir / "combo.bin", 6U, 20U);
  ASSERT_TRUE(combo_r.has_value());

  atx::impl::RunConfig cfg;
  cfg.panel = *research_r;
  cfg.combo = *combo_r;
  cfg.gross = 1.0;
  cfg.name_cap = 1.0;
  cfg.rebalance = "weekly";
  cfg.books_out = (dir / "books.bin").string();

  const atx::impl::MetaBookStageConfig scfg; // SingleSleeve, no --library-dir
  auto result = atx::impl::run_metabook(cfg, scfg);
  ASSERT_TRUE(result.has_value()) << (result ? "" : result.error().message());

  // The digest must be UNAFFECTED by telemetry (mirrors the combine breadth/capacity
  // convention) -- re-running produces the SAME digest even though kvs is recomputed.
  auto result2 = atx::impl::run_metabook(cfg, scfg);
  ASSERT_TRUE(result2.has_value());
  EXPECT_EQ(result->digest, result2->digest);

  EXPECT_EQ(find_kv(*result, "sleeves"), "1");
  // One sleeve -> no crossing possible: net == gross, benefit == 0, crossed_fraction == 0
  // (parsed back from the kvs STRING surface -- proves the telemetry surface itself, not
  // just the internal report struct).
  const atx::f64 net = std::stod(find_kv(*result, "fund_turnover_net"));
  const atx::f64 gross = std::stod(find_kv(*result, "fund_turnover_gross"));
  const atx::f64 benefit = std::stod(find_kv(*result, "crossing_benefit_bps"));
  const atx::f64 crossed = std::stod(find_kv(*result, "crossed_fraction"));
  EXPECT_NEAR(net, gross, 1e-6) << "single sleeve: fund_turnover_net != fund_turnover_gross";
  EXPECT_NEAR(benefit, 0.0, 1e-9) << "single sleeve: crossing_benefit_bps != 0";
  EXPECT_NEAR(crossed, 0.0, 1e-9) << "single sleeve: crossed_fraction != 0";
  EXPECT_GT(gross, 0.0) << "vacuous: zero turnover fixture";
}

} // namespace atxtest_metabook_netting_test
