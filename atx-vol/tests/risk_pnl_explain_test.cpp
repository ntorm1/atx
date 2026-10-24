// explain / explain_into (api/risk/pnl_explain.hpp): the Taylor identity, parity with
// the PortfolioPricer::pnl_explain oracle, the level / skew / curvature vega split,
// the two vol-move conventions, shares, exclusions, the trapezoid rule, determinism
// across thread counts, the bucket totals and the structural refusals.

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <limits>
#include <memory>
#include <vector>

#include "atx/vol/api/backtest/portfolio_pricer.hpp"
#include "atx/vol/api/risk/pnl_explain.hpp"
#include "atx/vol/api/simd/cpu.hpp"
#include "support/risk_synthetic_market.hpp"

using namespace atx::vol;
namespace rt = atx::vol::risk_test;

namespace {

[[nodiscard]] double ulp(double x) noexcept {
  const double a = std::fabs(x);
  return std::nextafter(a, std::numeric_limits<double>::infinity()) - a;
}

[[nodiscard]] bool same_bits(double a, double b) noexcept {
  return std::bit_cast<std::uint64_t>(a) == std::bit_cast<std::uint64_t>(b);
}

// The eight Taylor columns of row i, summed left to right in the kernel's order.
[[nodiscard]] double explained(const ExplainResult &e, std::size_t i) noexcept {
  return ((((((e.pnl_delta[i] + e.pnl_gamma[i]) + e.pnl_vega[i]) + e.pnl_volga[i]) +
            e.pnl_vanna[i]) +
           e.pnl_theta[i]) +
          e.pnl_rho[i]) +
         e.pnl_charm[i];
}

// Two frames of one book over a (base, shifted) state pair.
struct Scenario {
  std::unique_ptr<rt::OwnedState> base;
  std::unique_ptr<rt::OwnedState> shifted;
  std::unique_ptr<RiskBook> book;
  std::unique_ptr<PreparedRiskBook> prep;
  RiskFrame fb;
  RiskFrame ft;
};

[[nodiscard]] std::unique_ptr<Scenario>
make_scenario(const rt::StateMove &b, const rt::StateMove &t, std::vector<OptionLeg> legs,
              std::vector<ShareLeg> shares, const RiskKernel &kernel, RiskNeeds needs,
              std::size_t shifted_uids = 3) {
  auto s = std::make_unique<Scenario>();
  s->base = rt::make_state(b);
  s->shifted = rt::make_state(t, CarryFrom::Surface, shifted_uids);
  s->book = std::make_unique<RiskBook>(RiskBook::create(legs, shares).value());
  s->prep = std::make_unique<PreparedRiskBook>(PreparedRiskBook::create(*s->book).value());
  RiskWorkspace ws;
  EXPECT_TRUE(kernel.evaluate(*s->prep, s->base->state, needs, s->fb, ws).has_value());
  EXPECT_TRUE(kernel.evaluate(*s->prep, s->shifted->state, needs, s->ft, ws).has_value());
  return s;
}

[[nodiscard]] std::unique_ptr<Scenario> standard_scenario(const RiskKernel &kernel,
                                                          RiskNeeds needs = {}) {
  return make_scenario(rt::StateMove{}, rt::shifted_move(), rt::make_option_legs(),
                       rt::make_share_legs(), kernel, needs);
}

[[nodiscard]] ExplainResult run(const Scenario &s, const ExplainOptions &opts = {}) {
  auto r = explain(*s.prep, s.base->state, s.fb, s.shifted->state, s.ft, opts);
  EXPECT_TRUE(r.has_value()) << (r.has_value() ? "" : r.error().to_string());
  return r.has_value() ? std::move(*r) : ExplainResult{};
}

template <class T>
[[nodiscard]] bool bytes_equal(const std::vector<T> &a, const std::vector<T> &b) {
  return a.size() == b.size() &&
         (a.empty() || std::memcmp(a.data(), b.data(), a.size() * sizeof(T)) == 0);
}

[[nodiscard]] bool totals_equal(const ExplainTotals &a, const ExplainTotals &b) {
  return std::memcmp(&a, &b, sizeof(ExplainTotals)) == 0;
}

// Field accessors over ExplainTotals for the bucket test.
using TotalsField = double ExplainTotals::*;
constexpr std::array<TotalsField, 18> kTotalsFields{
    &ExplainTotals::pv_base,         &ExplainTotals::pv_target,  &ExplainTotals::pnl_total,
    &ExplainTotals::pnl_delta,       &ExplainTotals::pnl_gamma,  &ExplainTotals::pnl_vega,
    &ExplainTotals::pnl_volga,       &ExplainTotals::pnl_vanna,  &ExplainTotals::pnl_theta,
    &ExplainTotals::pnl_rho,         &ExplainTotals::pnl_charm,  &ExplainTotals::pnl_unexplained,
    &ExplainTotals::vol_level,       &ExplainTotals::vol_skew,   &ExplainTotals::vol_curv,
    &ExplainTotals::vol_resid,       &ExplainTotals::shares_pnl, &ExplainTotals::excluded_premium};

void expect_invalid_argument(const Result<ExplainResult> &r) {
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error().code(), ErrorCode::InvalidArgument) << r.error().to_string();
}

} // namespace

TEST(RiskExplain, TaylorIdentityWithinTwoUlp) {
  for (const VolMoveConvention conv :
       {VolMoveConvention::ContractOwn, VolMoveConvention::CommonBaseMaturity}) {
    const auto s = standard_scenario(RiskKernel::family());
    ExplainOptions opts;
    opts.vol_move = conv;
    const ExplainResult e = run(*s, opts);
    ASSERT_EQ(e.size(), s->book->n_option_legs() + s->book->n_share_legs());
    std::size_t checked = 0;
    double worst = 0.0;
    for (std::size_t i = 0; i < e.size(); ++i) {
      ASSERT_EQ(e.status[i], RiskLaneStatus::Ok) << i;
      const double gap = std::fabs((explained(e, i) + e.pnl_unexplained[i]) - e.pnl_total[i]);
      EXPECT_LE(gap, 2.0 * ulp(e.pnl_total[i])) << "row " << i;
      worst = std::max(worst, gap / ulp(e.pnl_total[i]));
      ++checked;
    }
    EXPECT_EQ(checked, e.size());
    std::cout << "[RiskExplain] Taylor identity conv=" << static_cast<int>(conv)
              << " rows=" << checked << " worst=" << worst << " ulp\n";
  }
}

TEST(RiskExplain, MatchesPortfolioPricerOracle) {
  RiskNeeds needs;
  needs.rho = true; // the oracle requests rho because its two surfaces' rates differ
  const ColdKernelPolicy cold{true, std::nullopt, 1};
  const auto s = make_scenario(rt::StateMove{}, rt::shifted_move(), rt::make_option_legs(), {},
                               RiskKernel::cold(cold), needs);
  ExplainOptions opts;
  opts.vol_move = VolMoveConvention::CommonBaseMaturity;
  opts.trapezoid_second_order = false;
  const ExplainResult e = run(*s, opts);

  // The oracle: the same book as Positions at the base instant, scalar ISA so its
  // marks and analytic Greeks are the scalar american_greeks_al / american_price.
  const std::vector<OptionLeg> legs = rt::make_option_legs();
  const Portfolio pf = Portfolio::create(rt::to_positions(legs, rt::kT0)).value();
  const PortfolioPricer pricer(pf);
  std::vector<const PricedSurface *> bp;
  std::vector<const PricedSurface *> sp;
  for (const PricedSurface &x : s->base->surfaces) {
    bp.push_back(&x);
  }
  for (const PricedSurface &x : s->shifted->surfaces) {
    sp.push_back(&x);
  }
  const SurfaceSet base_set = SurfaceSet::create(bp).value();
  const SurfaceSet shifted_set = SurfaceSet::create(sp).value();
  PriceOptions popts;
  popts.n_threads = 1;
  popts.analytic_greeks = true;
  popts.resolved_price_isa = simd::SimdIsa::ForceScalar;
  popts.query_execution = QueryExecution::ColdReference;
  const PnlFrame o = pricer.pnl_explain(base_set, shifted_set, popts).value();
  ASSERT_EQ(o.size(), legs.size());

  // Where the two engines' marks come from: ours is american_greeks_al(...).price ==
  // PricedSurface::fair_value (the scalar cold reference); the oracle's TARGET mark is
  // evaluate_batch(EF::Price)'s resolved price-batch kernel.
  std::size_t ours_eq_fv = 0;
  std::size_t oracle_eq_fv = 0;
  double oracle_vs_fv = 0.0;
  for (std::size_t i = 0; i < legs.size(); ++i) {
    const std::uint32_t row = s->prep->leg_prepared_row()[i];
    const PricedSurface &surf = s->shifted->surfaces[legs[i].uid - 1u];
    const double fv =
        surf.fair_value(legs[i].K, s->ft.T[row], legs[i].side, QueryExecution::ColdReference)
            .value();
    const double w = s->prep->leg_weight()[i];
    ours_eq_fv += same_bits(s->ft.price[row], fv) ? 1u : 0u;
    oracle_eq_fv += same_bits(o.pv_target[i], w * fv) ? 1u : 0u;
    oracle_vs_fv = std::max(oracle_vs_fv, std::fabs(o.pv_target[i] - w * fv) / std::fabs(w * fv));
  }
  std::cout << "[RiskExplain] shifted mark == fair_value bit-exact: ours " << ours_eq_fv << "/"
            << legs.size() << ", oracle " << oracle_eq_fv << "/" << legs.size()
            << " (oracle max rel gap to fair_value " << oracle_vs_fv << ")\n";
  EXPECT_EQ(ours_eq_fv, legs.size()) << "our shifted mark is the scalar cold reference";
  EXPECT_LE(oracle_vs_fv, 1e-12) << "the oracle's target mark is within the PV basis";

  using Col = std::vector<double> ExplainResult::*;
  using OCol = std::vector<double> PnlFrame::*;
  // `bit_exact`: the column is pinned bit for bit on every leg (pv_base, the eight
  // Taylor columns, the four moves). The rest carry the oracle's target mark
  // (pv_target and the two PV differences) and are held to the 1e-12 PV basis.
  struct Pair {
    const char *name;
    Col ours;
    OCol oracle;
    bool bit_exact;
  };
  const std::array<Pair, 16> cols{{
      {"pv_base", &ExplainResult::pv_base, &PnlFrame::pv_base, true},
      {"pv_target", &ExplainResult::pv_target, &PnlFrame::pv_target, false},
      {"pnl_total", &ExplainResult::pnl_total, &PnlFrame::pnl_total, false},
      {"pnl_delta", &ExplainResult::pnl_delta, &PnlFrame::pnl_delta, true},
      {"pnl_gamma", &ExplainResult::pnl_gamma, &PnlFrame::pnl_gamma, true},
      {"pnl_vega", &ExplainResult::pnl_vega, &PnlFrame::pnl_vega, true},
      {"pnl_volga", &ExplainResult::pnl_volga, &PnlFrame::pnl_volga, true},
      {"pnl_vanna", &ExplainResult::pnl_vanna, &PnlFrame::pnl_vanna, true},
      {"pnl_theta", &ExplainResult::pnl_theta, &PnlFrame::pnl_theta, true},
      {"pnl_rho", &ExplainResult::pnl_rho, &PnlFrame::pnl_rho, true},
      {"pnl_charm", &ExplainResult::pnl_charm, &PnlFrame::pnl_charm, true},
      {"pnl_unexplained", &ExplainResult::pnl_unexplained, &PnlFrame::pnl_unexplained, false},
      {"d_spot", &ExplainResult::d_spot, &PnlFrame::d_spot, true},
      {"d_vol", &ExplainResult::d_vol, &PnlFrame::d_vol, true},
      {"d_time", &ExplainResult::d_time, &PnlFrame::d_time, true},
      {"d_rate", &ExplainResult::d_rate, &PnlFrame::d_rate, true},
  }};
  for (const Pair &c : cols) {
    double worst = 0.0;
    std::size_t exact = 0;
    for (std::size_t i = 0; i < legs.size(); ++i) {
      ASSERT_EQ(e.status[i], RiskLaneStatus::Ok);
      ASSERT_EQ(o.status[i], PriceStatus::Ok);
      const double a = (e.*c.ours)[i];
      const double b = (o.*c.oracle)[i];
      exact += same_bits(a, b) ? 1u : 0u;
      if (c.bit_exact) {
        EXPECT_TRUE(same_bits(a, b)) << c.name << " leg " << i << " ours=" << a
                                     << " oracle=" << b;
      }
      // pnl_total and pnl_unexplained are differences of PVs: the oracle's target mark
      // differs from fair_value by up to ~6e-15 of the PV (above), so their gap is
      // measured against the leg's PV scale, not against the (cancelled) difference.
      const bool pv_scaled = c.ours == &ExplainResult::pnl_total ||
                             c.ours == &ExplainResult::pnl_unexplained;
      const double scale = pv_scaled ? std::max(std::fabs(b), std::fabs(o.pv_target[i]))
                                     : std::fabs(b);
      const double rel = a == b ? 0.0 : std::fabs(a - b) / std::max(scale, 1e-300);
      worst = std::max(worst, rel);
      EXPECT_LE(rel, 1e-12) << c.name << " leg " << i << " ours=" << a << " oracle=" << b;
    }
    std::cout << "[RiskExplain] oracle " << c.name << ": bit-exact " << exact << "/" << legs.size()
              << ", max rel gap " << worst << (c.bit_exact ? " (asserted bit-exact)" : "")
              << "\n";
    if (c.bit_exact) {
      EXPECT_EQ(exact, legs.size()) << c.name;
    }
  }
  // Totals: Neumaier here vs the oracle's plain fixed-order sum.
  const std::array<std::pair<double, double>, 12> totals{{
      {e.total.pv_base, o.total.pv_base},
      {e.total.pv_target, o.total.pv_target},
      {e.total.pnl_total, o.total.pnl_total},
      {e.total.pnl_delta, o.total.pnl_delta},
      {e.total.pnl_gamma, o.total.pnl_gamma},
      {e.total.pnl_vega, o.total.pnl_vega},
      {e.total.pnl_volga, o.total.pnl_volga},
      {e.total.pnl_vanna, o.total.pnl_vanna},
      {e.total.pnl_theta, o.total.pnl_theta},
      {e.total.pnl_rho, o.total.pnl_rho},
      {e.total.pnl_charm, o.total.pnl_charm},
      {e.total.pnl_unexplained, o.total.pnl_unexplained},
  }};
  double gross_pv = 0.0;
  for (std::size_t i = 0; i < legs.size(); ++i) {
    gross_pv += std::fabs(o.pv_target[i]);
  }
  double worst_total = 0.0;
  for (std::size_t k = 0; k < totals.size(); ++k) {
    const auto [a, b] = totals[k];
    const bool pv_scaled = k == 2u || k == 11u; // pnl_total, pnl_unexplained (as above)
    const double scale = pv_scaled ? std::max(std::fabs(b), gross_pv) : std::fabs(b);
    const double rel = a == b ? 0.0 : std::fabs(a - b) / std::max(scale, 1e-300);
    EXPECT_LE(rel, 1e-12) << "total " << k << " ours=" << a << " oracle=" << b;
    worst_total = std::max(worst_total, rel);
  }
  EXPECT_EQ(e.total.n_ok, o.total.n_ok);
  std::cout << "[RiskExplain] oracle totals: max rel gap " << worst_total << "\n";
}

TEST(RiskExplain, VegaSplitPartitionWithinTwoUlp) {
  const auto s = standard_scenario(RiskKernel::family());
  const ExplainResult e = run(*s);
  EXPECT_GT(e.n_pivot_groups, 0u);
  EXPECT_EQ(e.n_pivot_edge_fallback, 0u);
  std::size_t skewed = 0;
  std::size_t curved = 0;
  for (std::size_t i = 0; i < s->book->n_option_legs(); ++i) {
    ASSERT_EQ(e.status[i], RiskLaneStatus::Ok);
    const double sum = ((e.vol_level[i] + e.vol_skew[i]) + e.vol_curv[i]) + e.vol_resid[i];
    EXPECT_LE(std::fabs(sum - e.pnl_vega[i]), 2.0 * ulp(e.pnl_vega[i])) << "row " << i;
    skewed += e.vol_skew[i] != 0.0 ? 1u : 0u;
    curved += e.vol_curv[i] != 0.0 ? 1u : 0u;
  }
  EXPECT_GT(skewed, 0u) << "the split must not be vacuous";
  EXPECT_GT(curved, 0u);
  // Shares carry no vega.
  for (std::size_t i = s->book->n_option_legs(); i < e.size(); ++i) {
    EXPECT_EQ(e.vol_level[i], 0.0);
    EXPECT_EQ(e.vol_resid[i], 0.0);
  }
}

TEST(RiskExplain, ContractOwnIncludesRollDown) {
  // Time only: the surface's term structure is unchanged in T, so the vol a contract
  // sees moves purely by rolling down it.
  rt::StateMove later;
  later.time_units = rt::kDayUnits;
  const auto s = make_scenario(rt::StateMove{}, later, rt::make_option_legs(), {},
                               RiskKernel::family(), RiskNeeds{});
  ExplainOptions own;
  own.vol_move = VolMoveConvention::ContractOwn;
  ExplainOptions cbm;
  cbm.vol_move = VolMoveConvention::CommonBaseMaturity;
  const ExplainResult a = run(*s, own);
  const ExplainResult b = run(*s, cbm);
  double unexpl_own = 0.0;
  double unexpl_cbm = 0.0;
  std::size_t differ = 0;
  for (std::size_t i = 0; i < a.size(); ++i) {
    ASSERT_EQ(a.status[i], RiskLaneStatus::Ok);
    ASSERT_EQ(b.status[i], RiskLaneStatus::Ok);
    EXPECT_TRUE(same_bits(a.pnl_total[i], b.pnl_total[i])); // same frames, same P&L
    differ += same_bits(a.d_vol[i], b.d_vol[i]) ? 0u : 1u;
    unexpl_own += std::fabs(a.pnl_unexplained[i]);
    unexpl_cbm += std::fabs(b.pnl_unexplained[i]);
  }
  std::cout << "[RiskExplain] roll-down: sum|unexplained| ContractOwn=" << unexpl_own
            << " CommonBaseMaturity=" << unexpl_cbm << " (d_vol differs on " << differ << "/"
            << a.size() << " legs)\n";
  EXPECT_GT(differ, a.size() / 2u);
  EXPECT_LT(unexpl_own, unexpl_cbm);
}

TEST(RiskExplain, PureLevelMoveHitsOnlyLevel) {
  rt::StateMove flat;
  flat.flat_smile = true;
  flat.all_essvi = true;
  rt::StateMove bumped = flat;
  bumped.vol_level = 0.02;
  const auto s = make_scenario(flat, bumped, rt::make_option_legs(), {}, RiskKernel::family(),
                               RiskNeeds{});
  const ExplainResult e = run(*s);
  EXPECT_GT(e.n_pivot_groups, 0u);
  std::size_t exact = 0;
  for (std::size_t i = 0; i < e.size(); ++i) {
    ASSERT_EQ(e.status[i], RiskLaneStatus::Ok);
    const double v = e.pnl_vega[i];
    ASSERT_NE(v, 0.0);
    EXPECT_NEAR(e.vol_level[i], v, 1e-12 * std::fabs(v)) << i;
    EXPECT_LE(std::fabs(e.vol_skew[i]), 1e-12 * std::fabs(v)) << i;
    EXPECT_LE(std::fabs(e.vol_curv[i]), 1e-12 * std::fabs(v)) << i;
    EXPECT_LE(std::fabs(e.vol_resid[i]), 1e-12 * std::fabs(v)) << i;
    exact += (e.vol_level[i] == v && e.vol_skew[i] == 0.0 && e.vol_curv[i] == 0.0) ? 1u : 0u;
    // Nothing but vol moved.
    EXPECT_EQ(e.pnl_delta[i], 0.0);
    EXPECT_EQ(e.pnl_theta[i], 0.0);
    EXPECT_EQ(e.pnl_rho[i], 0.0);
  }
  std::cout << "[RiskExplain] pure level: " << exact << "/" << e.size()
            << " legs split exactly (level == pnl_vega, skew == curv == 0)\n";
}

TEST(RiskExplain, PureSkewMoveHitsOnlySkew) {
  // Marks-only states (no surface): the pivots are the group's own marks.
  const double S = 100.0;
  const double r = 0.04;
  const double q = 0.01;
  const std::int64_t expiry = rt::expiry_ns(1);
  const double T = rt::years(rt::kExpiryUnits[1]);
  const double F = (S - 0.0) * std::exp((r - q) * T); // the state's own carry expression
  std::vector<double> strikes;
  for (double K = 85.0; K <= 115.0; K += 2.5) {
    strikes.push_back(K);
  }
  const auto base_sigma = [&](double K) { return 0.25 - 0.10 * std::log(K / F); };
  // sigma_atm: the mark nearest F (the implementation's rule).
  double sigma_atm = 0.0;
  double best = std::numeric_limits<double>::infinity();
  for (const double K : strikes) {
    if (std::fabs(std::log(K / F)) < best) {
      best = std::fabs(std::log(K / F));
      sigma_atm = base_sigma(K);
    }
  }
  const double b = 0.015; // vol per unit of standardized moneyness
  const auto z_of = [&](double K) { return std::log(K / F) / (sigma_atm * std::sqrt(T)); };

  const auto build = [&](bool shifted) {
    auto st = std::make_unique<MarketStateStorage>();
    st->ts_ns = rt::kT0;
    UnderlierState u;
    u.uid = 1u;
    u.spot = S;
    u.flat_rate = r;
    u.flat_q = q;
    u.carry = CarryFrom::State;
    st->underliers.push_back(u);
    MarketStateStorage::MarksColumns m;
    m.uid = 1u;
    for (const Side side : {Side::Call, Side::Put}) {
      for (const double K : strikes) {
        m.strike.push_back(K);
        m.expiry_ns.push_back(expiry);
        m.side.push_back(side);
        m.sigma.push_back(base_sigma(K) + (shifted ? b * z_of(K) : 0.0));
      }
    }
    st->marks.push_back(std::move(m));
    return st;
  };
  const auto sb = build(false);
  const auto st = build(true);
  const MarketState base = make_market_state(*sb).value();
  const MarketState shifted = make_market_state(*st).value();

  std::vector<OptionLeg> legs;
  std::uint64_t id = 1;
  for (const double K : {90.0, 95.0, 100.0, 105.0, 110.0}) {
    legs.push_back(OptionLeg{id++, 1u, K, expiry, K < 100.0 ? Side::Put : Side::Call, 3.0, 100.0});
  }
  const RiskBook book = RiskBook::create(legs, {}).value();
  const PreparedRiskBook prep = PreparedRiskBook::create(book).value();
  const RiskKernel kernel = RiskKernel::family();
  RiskFrame fb;
  RiskFrame ft;
  RiskWorkspace ws;
  ASSERT_TRUE(kernel.evaluate(prep, base, RiskNeeds{}, fb, ws).has_value());
  ASSERT_TRUE(kernel.evaluate(prep, shifted, RiskNeeds{}, ft, ws).has_value());
  const ExplainResult e = explain(prep, base, fb, shifted, ft).value();
  EXPECT_GT(e.n_pivot_groups, 0u);
  for (std::size_t i = 0; i < e.size(); ++i) {
    ASSERT_EQ(e.status[i], RiskLaneStatus::Ok);
    EXPECT_TRUE(has_flag(e.flags[i], RiskFlag::PivotFromMarks)) << i;
    const double v = e.pnl_vega[i];
    if (legs[i].K == 100.0) {
      continue; // near z = 0: d_vol ~ 0, so pnl_vega ~ 0 and every piece ~ 0
    }
    ASSERT_NE(v, 0.0) << i;
    EXPECT_NEAR(e.vol_skew[i], v, 1e-9 * std::fabs(v)) << i;
    EXPECT_LE(std::fabs(e.vol_level[i]), 1e-9 * std::fabs(v)) << i;
    EXPECT_LE(std::fabs(e.vol_curv[i]), 1e-9 * std::fabs(v)) << i;
    EXPECT_LE(std::fabs(e.vol_resid[i]), 1e-9 * std::fabs(v)) << i;
  }
}

TEST(RiskExplain, SharesAreDeltaOnly) {
  const auto s = standard_scenario(RiskKernel::family());
  const ExplainResult e = run(*s);
  const auto shares = s->book->share_legs();
  const std::size_t n_opt = s->book->n_option_legs();
  ASSERT_EQ(e.size(), n_opt + shares.size());
  for (std::size_t j = 0; j < shares.size(); ++j) {
    const std::size_t i = n_opt + j;
    const std::size_t slot = s->prep->share_uid_slot()[j];
    const double S_b = s->fb.uid_spot[slot];
    const double S_t = s->ft.uid_spot[slot];
    const double dS = S_t - S_b;
    EXPECT_EQ(e.kind[i], ExplainLegKind::Share);
    EXPECT_EQ(e.id[i], shares[j].id);
    EXPECT_EQ(e.status[i], RiskLaneStatus::Ok);
    EXPECT_EQ(e.route_base[i], RiskRoute::Share);
    EXPECT_TRUE(same_bits(e.pnl_total[i], shares[j].qty * dS));
    EXPECT_TRUE(same_bits(e.pnl_delta[i], e.pnl_total[i]));
    EXPECT_TRUE(same_bits(e.pv_base[i], shares[j].qty * S_b));
    EXPECT_TRUE(same_bits(e.pv_target[i], shares[j].qty * S_t));
    for (const double x : {e.pnl_gamma[i], e.pnl_vega[i], e.pnl_volga[i], e.pnl_vanna[i],
                           e.pnl_theta[i], e.pnl_rho[i], e.pnl_charm[i], e.pnl_unexplained[i]}) {
      EXPECT_EQ(x, 0.0);
    }
    EXPECT_TRUE(same_bits(e.d_spot[i], dS));
  }
  // The share part of the total is reported on its own and inside pnl_delta.
  double share_sum = 0.0;
  for (std::size_t j = 0; j < shares.size(); ++j) {
    share_sum += e.pnl_total[n_opt + j];
  }
  EXPECT_NEAR(e.total.shares_pnl, share_sum, 1e-12 * std::fabs(share_sum));
  EXPECT_NE(e.total.shares_pnl, 0.0);
  // include_shares = false drops the share rows.
  ExplainOptions no_shares;
  no_shares.include_shares = false;
  const ExplainResult o = run(*s, no_shares);
  EXPECT_EQ(o.size(), n_opt);
  EXPECT_EQ(o.total.shares_pnl, 0.0);
}

TEST(RiskExplain, ExcludedLanesCarryPremiumAndNeverNaN) {
  // The shifted state is past the first expiry and has no uid 3.
  rt::StateMove late = rt::shifted_move();
  late.time_units = rt::kExpiryUnits[0] + 1;
  std::vector<ShareLeg> shares = rt::make_share_legs();
  shares.push_back(ShareLeg{1004u, 3u, 25.0});
  const auto s = make_scenario(rt::StateMove{}, late, rt::make_option_legs(), shares,
                               RiskKernel::family(), RiskNeeds{}, /*shifted_uids=*/2);
  const ExplainResult e = run(*s);
  const auto legs = s->book->option_legs();
  std::size_t rolled = 0;
  std::size_t missing = 0;
  double rolled_premium = 0.0;
  double missing_premium = 0.0;
  for (std::size_t i = 0; i < legs.size(); ++i) {
    const std::uint32_t row = s->prep->leg_prepared_row()[i];
    const double premium = std::fabs(s->prep->leg_weight()[i] * s->fb.price[row]);
    if (legs[i].uid == 3u) {
      EXPECT_EQ(e.status[i], RiskLaneStatus::ModelUnavailable) << i;
      ++missing;
      missing_premium += premium;
    } else if (legs[i].expiry_ns == rt::expiry_ns(0)) {
      EXPECT_EQ(e.status[i], RiskLaneStatus::Rolled) << i;
      ++rolled;
      rolled_premium += premium;
    } else {
      EXPECT_EQ(e.status[i], RiskLaneStatus::Ok) << i;
    }
  }
  ASSERT_GT(rolled, 0u);
  ASSERT_GT(missing, 0u);
  // The uid-3 share leg is excluded with its premium |qty S_b|.
  const std::size_t last = e.size() - 1u;
  EXPECT_EQ(e.status[last], RiskLaneStatus::ModelUnavailable);
  const double share_premium = 25.0 * s->fb.uid_spot[s->prep->share_uid_slot().back()];
  const auto ix = [](RiskLaneStatus st) { return static_cast<std::size_t>(st); };
  EXPECT_EQ(e.excluded_count[ix(RiskLaneStatus::Rolled)], rolled);
  EXPECT_EQ(e.excluded_count[ix(RiskLaneStatus::ModelUnavailable)], missing + 1u);
  EXPECT_NEAR(e.excluded_premium[ix(RiskLaneStatus::Rolled)], rolled_premium,
              1e-12 * rolled_premium);
  EXPECT_NEAR(e.excluded_premium[ix(RiskLaneStatus::ModelUnavailable)],
              missing_premium + share_premium, 1e-12 * (missing_premium + share_premium));
  EXPECT_EQ(e.total.n_excluded, rolled + missing + 1u);
  EXPECT_EQ(e.total.n_ok + e.total.n_excluded, e.size());
  EXPECT_NEAR(e.total.excluded_premium, rolled_premium + missing_premium + share_premium,
              1e-12 * (rolled_premium + missing_premium + share_premium));
  // Never NaN in a total or a bucket.
  for (const TotalsField f : kTotalsFields) {
    EXPECT_TRUE(std::isfinite(e.total.*f));
    for (const ExplainBucket &bk : e.by_uid) {
      EXPECT_TRUE(std::isfinite(bk.totals.*f));
    }
    for (const ExplainBucket &bk : e.by_expiry) {
      EXPECT_TRUE(std::isfinite(bk.totals.*f));
    }
  }
}

TEST(RiskExplain, TrapezoidKeepsIdentity) {
  const auto s = standard_scenario(RiskKernel::family());
  ExplainOptions trap;
  trap.trapezoid_second_order = true;
  const ExplainResult t = run(*s, trap);
  const ExplainResult b = run(*s);
  std::size_t gamma_moved = 0;
  for (std::size_t i = 0; i < t.size(); ++i) {
    ASSERT_EQ(t.status[i], RiskLaneStatus::Ok);
    const double gap = std::fabs((explained(t, i) + t.pnl_unexplained[i]) - t.pnl_total[i]);
    EXPECT_LE(gap, 2.0 * ulp(t.pnl_total[i])) << "row " << i;
    // First-order and time terms use the base Greeks either way.
    EXPECT_TRUE(same_bits(t.pnl_delta[i], b.pnl_delta[i]));
    EXPECT_TRUE(same_bits(t.pnl_vega[i], b.pnl_vega[i]));
    EXPECT_TRUE(same_bits(t.pnl_theta[i], b.pnl_theta[i]));
    EXPECT_TRUE(same_bits(t.pnl_total[i], b.pnl_total[i]));
    gamma_moved += same_bits(t.pnl_gamma[i], b.pnl_gamma[i]) ? 0u : 1u;
  }
  EXPECT_GT(gamma_moved, 0u);
}

TEST(RiskExplain, ThreadCountBitIdentical) {
  const auto s = standard_scenario(RiskKernel::family());
  ExplainOptions one;
  one.n_threads = 1;
  ExplainOptions eight;
  eight.n_threads = 8;
  const ExplainResult a = run(*s, one);
  ExplainResult b;
  ASSERT_TRUE(explain_into(*s->prep, s->base->state, s->fb, s->shifted->state, s->ft, eight, b)
                  .has_value());
  EXPECT_TRUE(bytes_equal(a.id, b.id) && bytes_equal(a.uid, b.uid) && bytes_equal(a.kind, b.kind));
  for (const auto col :
       {&ExplainResult::pv_base, &ExplainResult::pv_target, &ExplainResult::pnl_total,
        &ExplainResult::pnl_delta, &ExplainResult::pnl_gamma, &ExplainResult::pnl_vega,
        &ExplainResult::pnl_volga, &ExplainResult::pnl_vanna, &ExplainResult::pnl_theta,
        &ExplainResult::pnl_rho, &ExplainResult::pnl_charm, &ExplainResult::pnl_unexplained,
        &ExplainResult::vol_level, &ExplainResult::vol_skew, &ExplainResult::vol_curv,
        &ExplainResult::vol_resid, &ExplainResult::d_spot, &ExplainResult::d_vol,
        &ExplainResult::d_time, &ExplainResult::d_rate}) {
    EXPECT_TRUE(bytes_equal(a.*col, b.*col));
  }
  EXPECT_TRUE(bytes_equal(a.route_base, b.route_base));
  EXPECT_TRUE(bytes_equal(a.flags, b.flags));
  EXPECT_TRUE(bytes_equal(a.status, b.status));
  EXPECT_TRUE(totals_equal(a.total, b.total));
  ASSERT_EQ(a.by_uid.size(), b.by_uid.size());
  for (std::size_t k = 0; k < a.by_uid.size(); ++k) {
    EXPECT_TRUE(totals_equal(a.by_uid[k].totals, b.by_uid[k].totals));
  }
  ASSERT_EQ(a.by_expiry.size(), b.by_expiry.size());
  for (std::size_t k = 0; k < a.by_expiry.size(); ++k) {
    EXPECT_TRUE(totals_equal(a.by_expiry[k].totals, b.by_expiry[k].totals));
  }
}

TEST(RiskExplain, BucketsSumToTotal) {
  // Each bucket and the total are Neumaier sums (error ~ eps |sum| + n eps^2 sum|x|)
  // over disjoint subsets of the same rows in the same order, so the buckets' sum
  // differs from the total by a few eps of the gross; 1e-9 relative is a loose
  // envelope of that.
  const auto s = standard_scenario(RiskKernel::family());
  const ExplainResult e = run(*s);
  ASSERT_EQ(e.by_uid.size(), 3u);
  ASSERT_EQ(e.by_expiry.size(), rt::kExpiryUnits.size() + 1u); // + the share bucket
  for (std::size_t k = 1; k < e.by_uid.size(); ++k) {
    EXPECT_LT(e.by_uid[k - 1].key, e.by_uid[k].key);
  }
  for (std::size_t k = 1; k < e.by_expiry.size(); ++k) {
    EXPECT_LT(e.by_expiry[k - 1].expiry_ns, e.by_expiry[k].expiry_ns);
  }
  for (const auto *buckets : {&e.by_uid, &e.by_expiry}) {
    std::uint32_t n_ok = 0;
    for (const ExplainBucket &bk : *buckets) {
      n_ok += bk.totals.n_ok;
    }
    EXPECT_EQ(n_ok, e.total.n_ok);
    for (const TotalsField f : kTotalsFields) {
      double sum = 0.0;
      double gross = 0.0;
      for (const ExplainBucket &bk : *buckets) {
        sum += bk.totals.*f;
        gross += std::fabs(bk.totals.*f);
      }
      EXPECT_LE(std::fabs(sum - e.total.*f), 1e-9 * std::max(gross, 1e-300));
    }
  }
}

TEST(RiskExplain, RefusesFrameFromAnotherBook) {
  const auto s = standard_scenario(RiskKernel::family());
  // The same legs in a second book: another logical id, the same shape.
  const RiskBook other = RiskBook::create(rt::make_option_legs(), rt::make_share_legs()).value();
  const PreparedRiskBook other_prep = PreparedRiskBook::create(other).value();
  RiskFrame foreign;
  RiskWorkspace ws;
  ASSERT_TRUE(RiskKernel::family()
                  .evaluate(other_prep, s->shifted->state, RiskNeeds{}, foreign, ws)
                  .has_value());
  ASSERT_EQ(foreign.size(), s->ft.size());
  expect_invalid_argument(
      explain(*s->prep, s->base->state, s->fb, s->shifted->state, foreign, ExplainOptions{}));
  expect_invalid_argument(
      explain(*s->prep, s->base->state, foreign, s->shifted->state, s->ft, ExplainOptions{}));
  // A default (unprepared) book.
  expect_invalid_argument(explain(PreparedRiskBook{}, s->base->state, s->fb, s->shifted->state,
                                  s->ft, ExplainOptions{}));
  // explain_into leaves `out` untouched on a refusal.
  ExplainResult out;
  const Status st = explain_into(*s->prep, s->base->state, s->fb, s->shifted->state, foreign,
                                 ExplainOptions{}, out);
  ASSERT_FALSE(st.has_value());
  EXPECT_EQ(out.size(), 0u);
}

TEST(RiskExplain, RefusesShortenedFrame) {
  const auto s = standard_scenario(RiskKernel::family());
  ASSERT_TRUE(explain(*s->prep, s->base->state, s->fb, s->shifted->state, s->ft, {}).has_value());
  // Every per-contract column, then the per-uid and per-share columns: one element
  // short on a hand-shortened copy is a shape mismatch, never an out-of-bounds read.
  using DCol = std::vector<double> RiskFrame::*;
  const std::array<DCol, 16> contract_cols{
      &RiskFrame::T,     &RiskFrame::sigma,        &RiskFrame::forward,     &RiskFrame::q_eff,
      &RiskFrame::rate,  &RiskFrame::price,        &RiskFrame::delta,       &RiskFrame::gamma,
      &RiskFrame::vega,  &RiskFrame::theta,        &RiskFrame::rho,         &RiskFrame::vanna,
      &RiskFrame::volga, &RiskFrame::charm,        &RiskFrame::vendor_price,
      &RiskFrame::vendor_delta};
  std::size_t refused = 0;
  const auto check = [&](const RiskFrame &bad) {
    expect_invalid_argument(
        explain(*s->prep, s->base->state, s->fb, s->shifted->state, bad, ExplainOptions{}));
    expect_invalid_argument(
        explain(*s->prep, s->base->state, bad, s->shifted->state, s->ft, ExplainOptions{}));
    ++refused;
  };
  for (const DCol col : contract_cols) {
    RiskFrame bad = s->ft;
    (bad.*col).pop_back();
    check(bad);
  }
  {
    RiskFrame bad = s->ft;
    bad.route.pop_back();
    check(bad);
  }
  {
    RiskFrame bad = s->ft;
    bad.flags.pop_back();
    check(bad);
  }
  {
    RiskFrame bad = s->ft;
    bad.status.pop_back();
    check(bad);
  }
  for (const DCol col : {&RiskFrame::uid_spot, &RiskFrame::uid_flat_rate}) {
    RiskFrame bad = s->ft;
    (bad.*col).pop_back();
    check(bad);
  }
  {
    RiskFrame bad = s->ft;
    bad.uid_status.pop_back();
    check(bad);
  }
  {
    RiskFrame bad = s->ft;
    bad.share_mark.pop_back();
    check(bad);
  }
  {
    RiskFrame bad = s->ft;
    bad.share_status.pop_back();
    check(bad);
  }
  EXPECT_EQ(refused, 24u); // 16 numeric + route / flags / status + 3 per-uid + 2 per-share
}

TEST(RiskExplain, RefusesInvalidZRefAndRefusedStep) {
  const auto s = standard_scenario(RiskKernel::family());
  for (const double z : {0.0, -1.0, std::numeric_limits<double>::quiet_NaN(),
                         std::numeric_limits<double>::infinity()}) {
    ExplainOptions opts;
    opts.z_ref = z;
    expect_invalid_argument(
        explain(*s->prep, s->base->state, s->fb, s->shifted->state, s->ft, opts));
  }
  // The base state's TimeSpec refuses the step (VolTime before its covered window).
  rt::OwnedState &b = *s->base;
  b.storage.time.convention = TimeConvention::VolTime;
  b.state = make_market_state(b.storage).value();
  expect_invalid_argument(
      explain(*s->prep, b.state, s->fb, s->shifted->state, s->ft, ExplainOptions{}));
}
