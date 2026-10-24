// historical_var (api/risk/historical_var.hpp): historical-simulation VaR / ES of a book of
// RELATIVE option templates plus shares over (base, shifted) market-state pairs, on the risk
// engine. Pinned here: templates are re-resolved on every base state (fixed delta re-solved,
// fixed moneyness held) and the resolved contract is held through its shifted state; share
// legs are delta-one P&L; nearest-rank VaR / ES by hand; the tail attribution is the mean
// Greek split of the tail scenarios; unresolved templates are counted, never fatal; a state
// without surfaces cannot host templates; family and cold kernels agree; and the result is
// bit-identical for any thread count.

#include <gtest/gtest.h>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <numeric>
#include <span>
#include <utility>
#include <vector>

#include "atx/vol/api/core/vol_time.hpp"
#include "atx/vol/api/marketdata/data.hpp" // expiry_instant_ns
#include "atx/vol/api/risk/historical_var.hpp"
#include "atx/vol/api/risk/market_state.hpp"
#include "atx/vol/api/risk/pnl_explain.hpp"
#include "atx/vol/api/risk/risk_book.hpp"
#include "atx/vol/api/risk/risk_frame.hpp"
#include "atx/vol/api/risk/risk_kernel.hpp"
#include "atx/vol/api/risk/theo_contract.hpp"
#include "risk/historical_var_detail.hpp"
#include "support/risk_synthetic_market.hpp"

using namespace atx::vol;
namespace rt = atx::vol::risk_test;

namespace {

[[nodiscard]] bool same_bits(double a, double b) noexcept {
  return std::bit_cast<std::uint64_t>(a) == std::bit_cast<std::uint64_t>(b);
}

// A history of owned states; scenarios borrow them, so the vector of owners outlives every
// historical_var call made over it.
struct History {
  std::vector<std::unique_ptr<rt::OwnedState>> states;
  std::vector<HistoricalVarScenario> scenarios;

  const MarketState &state(std::size_t i) const { return states[i]->state; }
  void add(const rt::StateMove &move, std::size_t n_uids = 3) {
    states.push_back(rt::make_state(move, CarryFrom::Surface, n_uids));
  }
  void pair(std::size_t base, std::size_t shifted) {
    scenarios.push_back(HistoricalVarScenario{&state(base), &state(shifted)});
  }
};

[[nodiscard]] rt::StateMove move_at(std::int64_t days, double s1, double s2, double s3,
                                    double vol_level, double vol_skew = 0.0, double rate = 0.0) {
  rt::StateMove m;
  m.time_units = days * rt::kDayUnits;
  m.spot_ret = {s1, s2, s3};
  m.vol_level = vol_level;
  m.vol_skew = vol_skew;
  m.rate = rate;
  return m;
}

[[nodiscard]] TheoLeg put_delta_leg(std::uint32_t uid, double years, double delta, double qty) {
  TheoContract c;
  c.uid = uid;
  c.maturity = MaturitySpec::years_of(years);
  c.strike = StrikeCoord::delta(delta, DeltaConvention::Forward);
  c.side = Side::Put;
  return TheoLeg{c, qty};
}

[[nodiscard]] TheoLeg moneyness_leg(std::uint32_t uid, double years, double k, Side side,
                                    double qty) {
  TheoContract c;
  c.uid = uid;
  c.maturity = MaturitySpec::years_of(years);
  c.strike = StrikeCoord::log_moneyness(k);
  c.side = side;
  return TheoLeg{c, qty};
}

// The four-template reference book of the suites: a short fixed-delta put and a long
// fixed-moneyness call on uid 1, a long fixed-moneyness put on uid 2, a fixed-delta put on
// uid 3; plus share legs when asked.
[[nodiscard]] HistoricalVarBook reference_book(bool with_shares) {
  HistoricalVarBook book;
  book.options.push_back(put_delta_leg(1u, 0.15, -0.25, -10.0));
  book.options.push_back(moneyness_leg(1u, 0.30, 0.05, Side::Call, 6.0));
  book.options.push_back(moneyness_leg(2u, 0.30, -0.08, Side::Put, 4.0));
  book.options.push_back(put_delta_leg(3u, 0.15, -0.35, 8.0));
  if (with_shares) {
    book.shares.push_back(ShareLeg{7001u, 1u, 250.0});
    book.shares.push_back(ShareLeg{7002u, 2u, -40.0});
  }
  return book;
}

// Five (base -> shifted) pairs over distinct markets: spot, vol level and skew move both
// ways so the losses are of mixed sign and the tail is not degenerate.
[[nodiscard]] History five_pair_history() {
  History h;
  const double s1[] = {0.010, -0.020, 0.030, -0.005, -0.040};
  const double s2[] = {-0.006, 0.012, -0.018, 0.004, 0.025};
  const double s3[] = {0.004, -0.010, 0.015, -0.002, -0.020};
  const double vl[] = {-0.004, 0.010, -0.012, 0.002, 0.020};
  for (std::size_t i = 0; i < 5; ++i) {
    const std::int64_t day = 3 * static_cast<std::int64_t>(i);
    const double base_ret = 0.004 * static_cast<double>(i) - 0.008;
    h.add(move_at(day, base_ret, -base_ret, 0.5 * base_ret, 0.002 * static_cast<double>(i)));
    h.add(move_at(day + 1, base_ret + s1[i], -base_ret + s2[i], 0.5 * base_ret + s3[i],
                  0.002 * static_cast<double>(i) + vl[i], 0.01 * vl[i]));
    h.pair(2 * i, 2 * i + 1);
  }
  return h;
}

[[nodiscard]] HistoricalVarConfig base_config() {
  HistoricalVarConfig cfg;
  cfg.keep_resolved_legs = true;
  return cfg;
}

void expect_totals_bits(const ExplainTotals &a, const ExplainTotals &b, std::size_t i) {
  EXPECT_TRUE(same_bits(a.pv_base, b.pv_base)) << "scenario " << i;
  EXPECT_TRUE(same_bits(a.pv_target, b.pv_target)) << "scenario " << i;
  EXPECT_TRUE(same_bits(a.pnl_total, b.pnl_total)) << "scenario " << i;
  EXPECT_TRUE(same_bits(a.pnl_delta, b.pnl_delta)) << "scenario " << i;
  EXPECT_TRUE(same_bits(a.pnl_gamma, b.pnl_gamma)) << "scenario " << i;
  EXPECT_TRUE(same_bits(a.pnl_vega, b.pnl_vega)) << "scenario " << i;
  EXPECT_TRUE(same_bits(a.pnl_volga, b.pnl_volga)) << "scenario " << i;
  EXPECT_TRUE(same_bits(a.pnl_vanna, b.pnl_vanna)) << "scenario " << i;
  EXPECT_TRUE(same_bits(a.pnl_theta, b.pnl_theta)) << "scenario " << i;
  EXPECT_TRUE(same_bits(a.pnl_rho, b.pnl_rho)) << "scenario " << i;
  EXPECT_TRUE(same_bits(a.pnl_charm, b.pnl_charm)) << "scenario " << i;
  EXPECT_TRUE(same_bits(a.pnl_unexplained, b.pnl_unexplained)) << "scenario " << i;
  EXPECT_TRUE(same_bits(a.vol_level, b.vol_level)) << "scenario " << i;
  EXPECT_TRUE(same_bits(a.vol_skew, b.vol_skew)) << "scenario " << i;
  EXPECT_TRUE(same_bits(a.vol_curv, b.vol_curv)) << "scenario " << i;
  EXPECT_TRUE(same_bits(a.vol_resid, b.vol_resid)) << "scenario " << i;
  EXPECT_TRUE(same_bits(a.shares_pnl, b.shares_pnl)) << "scenario " << i;
  EXPECT_EQ(a.n_ok, b.n_ok) << "scenario " << i;
  EXPECT_EQ(a.n_excluded, b.n_excluded) << "scenario " << i;
  EXPECT_TRUE(same_bits(a.excluded_premium, b.excluded_premium)) << "scenario " << i;
}

// Scenario P&L rebuilt by hand from the public pieces: resolve on the base surface, hold the
// resolved contract in a RiskBook, evaluate both states with the family kernel, explain.
[[nodiscard]] ExplainTotals manual_scenario(const HistoricalVarBook &book, const MarketState &b,
                                            const MarketState &t) {
  std::vector<OptionLeg> legs;
  for (std::size_t j = 0; j < book.options.size(); ++j) {
    const TheoLeg &tl = book.options[j];
    const Result<ResolvedTheo> r =
        resolve_theo(tl.contract, b.surface(tl.contract.uid), b.ts_ns(), ProjectionPolicy{});
    EXPECT_TRUE(r.has_value());
    EXPECT_EQ(r->status, TheoStatus::Ok);
    OptionLeg leg = r->leg;
    leg.id = j;
    leg.qty = tl.qty;
    legs.push_back(leg);
  }
  const RiskBook rb = RiskBook::create(legs, book.shares).value();
  const PreparedRiskBook prep = PreparedRiskBook::create(rb).value();
  const RiskKernel kernel = RiskKernel::family();
  RiskWorkspace ws;
  ws.reserve(prep);
  RiskFrame fb;
  RiskFrame ft;
  EXPECT_TRUE(kernel.evaluate(prep, b, RiskNeeds{}, fb, ws).has_value());
  EXPECT_TRUE(kernel.evaluate(prep, t, RiskNeeds{}, ft, ws).has_value());
  const Result<ExplainResult> e = explain(prep, b, fb, t, ft, ExplainOptions{});
  EXPECT_TRUE(e.has_value());
  EXPECT_FALSE(e->rate_move_without_rho); // the caller's history moves no rate
  return e->total;
}

// A state with marks and spots but no surface: it can price listed contracts, never a
// theoretical template.
struct MarksOnlyState {
  MarketStateStorage storage;
  MarketState state;
};

[[nodiscard]] std::unique_ptr<MarksOnlyState> make_marks_only_state() {
  auto out = std::make_unique<MarksOnlyState>();
  out->storage.ts_ns = rt::instant_ns(0);
  for (const rt::UnderlierSpec &u : rt::kUnderliers) {
    UnderlierState s;
    s.uid = u.uid;
    s.spot = u.spot;
    s.flat_rate = rt::kRate;
    s.flat_q = u.q;
    out->storage.underliers.push_back(s);
    MarketStateStorage::MarksColumns m;
    m.uid = u.uid;
    m.strike = {u.spot};
    m.expiry_ns = {rt::expiry_ns(1)};
    m.side = {Side::Put};
    m.sigma = {u.vol_short};
    out->storage.marks.push_back(std::move(m));
  }
  out->state = make_market_state(out->storage).value();
  return out;
}

// ── Templates ────────────────────────────────────────────────────────────────

TEST(HistoricalVar, FixedDeltaTemplatesReSolvePerScenario) {
  History h;
  h.add(move_at(0, 0.0, 0.0, 0.0, 0.0));
  h.add(move_at(1, 0.006, -0.004, 0.003, 0.010, 0.01));
  h.add(move_at(4, 0.060, 0.030, -0.040, 0.030));
  h.add(move_at(5, 0.050, 0.035, -0.045, 0.024, -0.01));
  h.pair(0, 1);
  h.pair(2, 3);
  HistoricalVarBook book;
  book.options.push_back(put_delta_leg(1u, 0.15, -0.25, -10.0));
  book.options.push_back(put_delta_leg(3u, 0.15, -0.35, 8.0));

  const Result<HistoricalVarResult> res = historical_var(book, h.scenarios, base_config());
  ASSERT_TRUE(res.has_value()) << res.error().to_string();
  ASSERT_EQ(res->frames.size(), 2u);
  ASSERT_EQ(res->resolved_legs.size(), 2u * book.options.size());

  for (std::size_t i = 0; i < 2; ++i) {
    const HistoricalVarFrame &f = res->frames[i];
    EXPECT_TRUE(f.admitted) << i;
    EXPECT_EQ(f.n_legs_resolved, 2u) << i;
    EXPECT_EQ(f.n_legs_unresolved, 0u) << i;
    EXPECT_EQ(f.base_ts_ns, h.scenarios[i].base->ts_ns()) << i;
    EXPECT_EQ(f.shifted_ts_ns, h.scenarios[i].shifted->ts_ns()) << i;
    for (std::size_t j = 0; j < book.options.size(); ++j) {
      const ResolvedTheo &r = res->resolved_legs[i * book.options.size() + j];
      ASSERT_EQ(r.status, TheoStatus::Ok) << i << "," << j;
      // The delta is re-solved ON THE BASE STATE of each scenario.
      EXPECT_EQ(r.valuation_ns, h.scenarios[i].base->ts_ns());
      EXPECT_NEAR(r.delta, book.options[j].contract.strike.value, 1e-7) << i << "," << j;
      EXPECT_EQ(r.leg.qty, book.options[j].qty);
    }
    // The pipeline is exactly resolve -> RiskBook -> family kernel -> explain.
    expect_totals_bits(f.totals,
                       manual_scenario(book, *h.scenarios[i].base, *h.scenarios[i].shifted), i);
  }
  // A different base market re-strikes the same template: the strike moves with spot / vol.
  for (std::size_t j = 0; j < book.options.size(); ++j) {
    EXPECT_NE(res->resolved_legs[j].leg.K, res->resolved_legs[book.options.size() + j].leg.K)
        << "template " << j << " was not re-solved on the second base state";
  }
}

TEST(HistoricalVar, FixedMoneynessTemplatesHoldK) {
  History h;
  h.add(move_at(0, 0.0, 0.0, 0.0, 0.0));
  h.add(move_at(1, 0.008, -0.006, 0.005, 0.012, 0.02, 0.001)); // a rate move too
  h.add(move_at(6, -0.050, 0.040, 0.020, -0.010));
  h.add(move_at(7, -0.046, 0.036, 0.024, -0.004));
  h.pair(0, 1);
  h.pair(2, 3);
  HistoricalVarBook book;
  book.options.push_back(moneyness_leg(1u, 0.30, 0.05, Side::Call, 6.0));
  book.options.push_back(moneyness_leg(2u, 0.30, -0.08, Side::Put, -4.0));

  const Result<HistoricalVarResult> res = historical_var(book, h.scenarios, base_config());
  ASSERT_TRUE(res.has_value()) << res.error().to_string();
  ASSERT_EQ(res->resolved_legs.size(), 4u);
  for (std::size_t i = 0; i < 2; ++i) {
    for (std::size_t j = 0; j < 2; ++j) {
      const ResolvedTheo &r = res->resolved_legs[i * 2 + j];
      ASSERT_EQ(r.status, TheoStatus::Ok);
      const double k = book.options[j].contract.strike.value;
      // Fixed k on every base: K = F_b(T) e^k with T the template's TTE.
      EXPECT_NEAR(r.k_log, k, 1e-12) << i << "," << j;
      EXPECT_NEAR(r.leg.K, r.forward * std::exp(k), 1e-10 * r.leg.K) << i << "," << j;
      EXPECT_NEAR(r.T, 0.30, 1e-12) << i << "," << j;
      EXPECT_EQ(r.leg.expiry_ns, ns_from_year_fraction(h.scenarios[i].base->ts_ns(), 0.30));
    }
  }
  // The moneyness is held, so the strike follows each base's forward.
  EXPECT_NE(res->resolved_legs[0].leg.K, res->resolved_legs[2].leg.K);
  EXPECT_NE(res->resolved_legs[1].leg.K, res->resolved_legs[3].leg.K);

  // Within a scenario the BASE-resolved K is the contract carried through the shifted state:
  // the explain's vol move is the shifted surface at that same K (no re-strike on t).
  const HistoricalVarFrame &moved = res->frames[0];
  ASSERT_TRUE(moved.admitted);
  EXPECT_NE(moved.totals.pnl_vega, 0.0);
  // The pair moves the rate: rho is materialized for the base frame, so the rate move is
  // attributed to pnl_rho rather than left in the unexplained remainder.
  EXPECT_NE(moved.totals.pnl_rho, 0.0);
  EXPECT_EQ(res->frames[1].totals.pnl_rho, 0.0); // no rate move in the second pair
}

// ── Shares ───────────────────────────────────────────────────────────────────

TEST(HistoricalVar, SharesInScenarioPnl) {
  History h;
  h.add(move_at(0, 0.0, 0.0, 0.0, 0.0));
  h.add(move_at(1, 0.012, -0.009, 0.004, 0.005));
  h.pair(0, 1);
  const HistoricalVarBook with = reference_book(/*with_shares=*/true);
  const HistoricalVarBook without = reference_book(/*with_shares=*/false);
  HistoricalVarBook shares_only;
  shares_only.shares = with.shares;

  const Result<HistoricalVarResult> a = historical_var(with, h.scenarios, base_config());
  const Result<HistoricalVarResult> b = historical_var(without, h.scenarios, base_config());
  const Result<HistoricalVarResult> s = historical_var(shares_only, h.scenarios, base_config());
  ASSERT_TRUE(a.has_value()) << a.error().to_string();
  ASSERT_TRUE(b.has_value()) << b.error().to_string();
  ASSERT_TRUE(s.has_value()) << s.error().to_string();

  const double dS1 = h.state(1).underlier(1u)->spot - h.state(0).underlier(1u)->spot;
  const double dS2 = h.state(1).underlier(2u)->spot - h.state(0).underlier(2u)->spot;
  const double share_pnl = 250.0 * dS1 + -40.0 * dS2;
  const ExplainTotals &ta = a->frames[0].totals;
  const ExplainTotals &tb = b->frames[0].totals;
  EXPECT_NEAR(ta.shares_pnl, share_pnl, 1e-12 * std::fabs(share_pnl));
  EXPECT_NE(ta.shares_pnl, 0.0);
  EXPECT_EQ(tb.shares_pnl, 0.0);
  // The share legs are in the scenario P&L, and on the delta axis only.
  EXPECT_NEAR(ta.pnl_total, tb.pnl_total + ta.shares_pnl, 1e-9 * std::fabs(tb.pnl_total));
  EXPECT_NEAR(ta.pnl_delta, tb.pnl_delta + ta.shares_pnl, 1e-9 * std::fabs(tb.pnl_delta));
  EXPECT_TRUE(same_bits(ta.pnl_vega, tb.pnl_vega));
  EXPECT_EQ(ta.n_ok, tb.n_ok + 2u);
  // A shares-only book is a valid book: its P&L is exactly the share P&L.
  ASSERT_TRUE(s->frames[0].admitted);
  EXPECT_EQ(s->frames[0].n_legs_resolved, 0u);
  EXPECT_TRUE(same_bits(s->frames[0].totals.pnl_total, s->frames[0].totals.shares_pnl));
  EXPECT_NEAR(s->frames[0].totals.pnl_total, share_pnl, 1e-12 * std::fabs(share_pnl));
}

// ── Quantiles and attribution ────────────────────────────────────────────────

TEST(HistoricalVar, NearestRankQuantilesMatchHandComputation) {
  // One share leg on uid 1: every scenario's P&L is 100 * dS exactly, so the loss sample
  // is known in closed form and the quantile arithmetic is checked by hand.
  History h;
  h.add(move_at(0, 0.0, 0.0, 0.0, 0.0));
  const double rets[] = {0.010, -0.020, 0.030, -0.005, -0.040};
  for (const double r : rets) {
    h.add(move_at(1, r, 0.0, 0.0, 0.0));
  }
  for (std::size_t i = 1; i <= 5; ++i) {
    h.pair(0, i);
  }
  HistoricalVarBook book;
  book.shares.push_back(ShareLeg{1u, 1u, 100.0});
  HistoricalVarConfig cfg;
  cfg.confidences = {0.6, 0.8};

  const Result<HistoricalVarResult> res = historical_var(book, h.scenarios, cfg);
  ASSERT_TRUE(res.has_value()) << res.error().to_string();
  ASSERT_EQ(res->n_admitted, 5u);
  ASSERT_EQ(res->points.size(), 2u);

  const double S0 = h.state(0).underlier(1u)->spot;
  std::vector<double> losses;
  for (std::size_t i = 1; i <= 5; ++i) {
    const double pnl = 100.0 * (h.state(i).underlier(1u)->spot - S0);
    EXPECT_TRUE(same_bits(res->frames[i - 1].totals.pnl_total, pnl)) << i;
    losses.push_back(-pnl);
  }
  std::sort(losses.begin(), losses.end()); // ~ {-300, -100, 50, 200, 400}

  // c = 0.6, n = 5: rank ceil(3) = 3 -> index 2; ES = mean of losses[2..4].
  const HistoricalVarPoint &p60 = res->points[0];
  EXPECT_EQ(p60.confidence, 0.6);
  EXPECT_TRUE(same_bits(p60.value_at_risk, losses[2]));
  EXPECT_NEAR(p60.expected_shortfall, (losses[2] + losses[3] + losses[4]) / 3.0, 1e-9);
  EXPECT_EQ(p60.tail_scenarios, 3u);
  // c = 0.8, n = 5: rank ceil(4) = 4 -> index 3; ES = mean of losses[3..4].
  const HistoricalVarPoint &p80 = res->points[1];
  EXPECT_TRUE(same_bits(p80.value_at_risk, losses[3]));
  EXPECT_NEAR(p80.expected_shortfall, (losses[3] + losses[4]) / 2.0, 1e-9);
  EXPECT_EQ(p80.tail_scenarios, 2u);
  // Sanity of the hand sample: the worst loss is the -4% move, the VaR a real loss.
  EXPECT_NEAR(losses[4], 100.0 * 100.0 * 0.04, 1e-9);
  EXPECT_GT(p60.value_at_risk, 0.0);
  // reference_pv = mean pv_base over admitted scenarios = 100 * S0 (one base state).
  EXPECT_TRUE(same_bits(res->reference_pv, 100.0 * S0));
  // summarize_historical_var reproduces the quantile half bit for bit from the frames.
  const Result<HistoricalVarResult> again =
      summarize_historical_var(res->frames, cfg.confidences);
  ASSERT_TRUE(again.has_value());
  EXPECT_TRUE(same_bits(again->points[0].value_at_risk, p60.value_at_risk));
  EXPECT_TRUE(same_bits(again->points[0].expected_shortfall, p60.expected_shortfall));
  EXPECT_TRUE(same_bits(again->points[1].expected_shortfall, p80.expected_shortfall));
  EXPECT_TRUE(same_bits(again->reference_pv, res->reference_pv));
}

// Hand-built frames for the quantile half alone: a fixed pv_base, the P&L under test, and a
// vega tag that tells otherwise tied scenarios apart in the tail attribution.
[[nodiscard]] HistoricalVarFrame hand_frame(double pnl, double vega_tag, bool admitted = true) {
  HistoricalVarFrame f;
  f.totals.pv_base = 100.0;
  f.totals.pnl_total = pnl;
  f.totals.pnl_vega = vega_tag;
  f.totals.n_ok = 1u;
  f.admitted = admitted;
  return f;
}

TEST(HistoricalVar, NearestRankTiesKeepScenarioOrder) {
  // Losses in scenario order {2, 1, 2, 5, 2}: three scenarios tie at the VaR level. Sorted
  // ascending with ties in scenario order: B(1) A(2) C(2) E(2) D(5).
  const std::vector<HistoricalVarFrame> frames = {
      hand_frame(-2.0, 1.0),   // A
      hand_frame(-1.0, 0.0),   // B
      hand_frame(-2.0, 10.0),  // C
      hand_frame(-5.0, 0.0),   // D
      hand_frame(-2.0, 100.0), // E
      hand_frame(-9.0, 0.0, /*admitted=*/false)}; // never in the sample
  const std::vector<double> confidences = {0.6, 0.4};
  const Result<HistoricalVarResult> res = summarize_historical_var(frames, confidences);
  ASSERT_TRUE(res.has_value()) << res.error().to_string();
  ASSERT_EQ(res->n_admitted, 5u);

  // c = 0.6: rank ceil(3) = 3 -> C; the tail is C, E, D (the LATER tied scenarios).
  const HistoricalVarPoint &p60 = res->points[0];
  EXPECT_EQ(p60.value_at_risk, 2.0);
  EXPECT_EQ(p60.tail_scenarios, 3u);
  EXPECT_EQ(p60.expected_shortfall, 3.0);              // (2 + 2 + 5) / 3
  EXPECT_DOUBLE_EQ(p60.tail_mean.pnl_vega, 110.0 / 3.0); // C and E, not A
  // c = 0.4: rank ceil(2) = 2 -> A; the tail is A, C, E, D.
  const HistoricalVarPoint &p40 = res->points[1];
  EXPECT_EQ(p40.value_at_risk, 2.0);
  EXPECT_EQ(p40.tail_scenarios, 4u);
  EXPECT_EQ(p40.expected_shortfall, 2.75); // (2 + 2 + 2 + 5) / 4
  EXPECT_DOUBLE_EQ(p40.tail_mean.pnl_vega, 111.0 / 4.0);
}

TEST(HistoricalVar, NearestRankAtOneScenarioAndConfidenceNearOne) {
  // n = 1: every confidence picks the only loss, and the tail is that scenario.
  const std::vector<HistoricalVarFrame> one = {hand_frame(-3.5, 1.0),
                                               hand_frame(-50.0, 0.0, /*admitted=*/false)};
  const std::vector<double> any_c = {1e-12, 0.5, 0.99};
  const Result<HistoricalVarResult> single = summarize_historical_var(one, any_c);
  ASSERT_TRUE(single.has_value()) << single.error().to_string();
  ASSERT_EQ(single->n_admitted, 1u);
  for (const HistoricalVarPoint &p : single->points) {
    EXPECT_EQ(p.value_at_risk, 3.5) << p.confidence;
    EXPECT_EQ(p.expected_shortfall, 3.5) << p.confidence;
    EXPECT_EQ(p.tail_scenarios, 1u) << p.confidence;
  }
  // c -> 1 (the largest double below 1): rank ceil(c n) = n, the worst loss alone.
  const std::vector<HistoricalVarFrame> five = {hand_frame(-2.0, 0.0), hand_frame(-1.0, 0.0),
                                                hand_frame(-7.0, 0.0), hand_frame(4.0, 0.0),
                                                hand_frame(-3.0, 0.0)};
  const std::vector<double> near_one = {std::nextafter(1.0, 0.0)};
  const Result<HistoricalVarResult> top = summarize_historical_var(five, near_one);
  ASSERT_TRUE(top.has_value()) << top.error().to_string();
  EXPECT_EQ(top->points[0].value_at_risk, 7.0);
  EXPECT_EQ(top->points[0].expected_shortfall, 7.0);
  EXPECT_EQ(top->points[0].tail_scenarios, 1u);
}

TEST(HistoricalVar, SummarizeRefusesAnAdmittedNonFiniteFrame) {
  const std::vector<double> confidences = {0.95};
  const double nan = std::numeric_limits<double>::quiet_NaN();
  const double inf = std::numeric_limits<double>::infinity();
  const auto invalid = [&](std::vector<HistoricalVarFrame> frames) {
    const Result<HistoricalVarResult> r = summarize_historical_var(std::move(frames), confidences);
    return !r.has_value() && r.error().code() == ErrorCode::InvalidArgument;
  };
  // A NaN loss has no rank (it would break the sort's ordering); an infinite one, or a
  // non-finite base value, has no mean.
  EXPECT_TRUE(invalid({hand_frame(-1.0, 0.0), hand_frame(nan, 0.0), hand_frame(-2.0, 0.0)}));
  EXPECT_TRUE(invalid({hand_frame(-1.0, 0.0), hand_frame(-inf, 0.0)}));
  HistoricalVarFrame bad_pv = hand_frame(-1.0, 0.0);
  bad_pv.totals.pv_base = nan;
  EXPECT_TRUE(invalid({hand_frame(-2.0, 0.0), bad_pv}));
  // A frame that is not admitted is not in the sample: its contents are never read.
  const Result<HistoricalVarResult> ok = summarize_historical_var(
      {hand_frame(-1.0, 0.0), hand_frame(nan, 0.0, /*admitted=*/false)}, confidences);
  ASSERT_TRUE(ok.has_value()) << ok.error().to_string();
  EXPECT_EQ(ok->n_admitted, 1u);
  EXPECT_EQ(ok->points[0].value_at_risk, 1.0);
}

TEST(HistoricalVar, TailMeanIsAttributionOfTail) {
  const History h = five_pair_history();
  const HistoricalVarBook book = reference_book(/*with_shares=*/true);
  HistoricalVarConfig cfg;
  cfg.confidences = {0.6};
  const Result<HistoricalVarResult> res = historical_var(book, h.scenarios, cfg);
  ASSERT_TRUE(res.has_value()) << res.error().to_string();
  ASSERT_EQ(res->n_admitted, 5u);
  const HistoricalVarPoint &p = res->points[0];
  ASSERT_EQ(p.tail_scenarios, 3u);

  // The tail = the three scenarios with the largest losses, found here independently.
  std::vector<std::size_t> order(5);
  std::iota(order.begin(), order.end(), std::size_t{0});
  std::sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) {
    return -res->frames[a].totals.pnl_total < -res->frames[b].totals.pnl_total;
  });
  ExplainTotals hand{};
  for (std::size_t k = 2; k < 5; ++k) {
    const ExplainTotals &t = res->frames[order[k]].totals;
    hand.pv_base += t.pv_base / 3.0;
    hand.pnl_total += t.pnl_total / 3.0;
    hand.pnl_delta += t.pnl_delta / 3.0;
    hand.pnl_gamma += t.pnl_gamma / 3.0;
    hand.pnl_vega += t.pnl_vega / 3.0;
    hand.pnl_theta += t.pnl_theta / 3.0;
    hand.pnl_unexplained += t.pnl_unexplained / 3.0;
    hand.vol_level += t.vol_level / 3.0;
    hand.vol_skew += t.vol_skew / 3.0;
    hand.shares_pnl += t.shares_pnl / 3.0;
    hand.pnl_carry += t.pnl_carry / 3.0;
  }
  const auto near = [](double got, double want, const char *what) {
    EXPECT_NEAR(got, want, 1e-10 * std::max(1.0, std::fabs(want))) << what;
  };
  near(p.tail_mean.pv_base, hand.pv_base, "pv_base");
  near(p.tail_mean.pnl_total, hand.pnl_total, "pnl_total");
  near(p.tail_mean.pnl_delta, hand.pnl_delta, "pnl_delta");
  near(p.tail_mean.pnl_gamma, hand.pnl_gamma, "pnl_gamma");
  near(p.tail_mean.pnl_vega, hand.pnl_vega, "pnl_vega");
  near(p.tail_mean.pnl_theta, hand.pnl_theta, "pnl_theta");
  near(p.tail_mean.pnl_unexplained, hand.pnl_unexplained, "pnl_unexplained");
  near(p.tail_mean.vol_level, hand.vol_level, "vol_level");
  near(p.tail_mean.vol_skew, hand.vol_skew, "vol_skew");
  near(p.tail_mean.shares_pnl, hand.shares_pnl, "shares_pnl");
  near(p.tail_mean.pnl_carry, hand.pnl_carry, "pnl_carry");
  // ES is the tail's mean loss, i.e. minus the attributed tail P&L, to the bit.
  EXPECT_TRUE(same_bits(p.expected_shortfall, -p.tail_mean.pnl_total));
  // The attribution adds up: eight Greek axes + the carry axis + unexplained == the tail's
  // mean P&L.
  const auto axes_of = [](const ExplainTotals &m) {
    return m.pnl_delta + m.pnl_gamma + m.pnl_vega + m.pnl_volga + m.pnl_vanna + m.pnl_theta +
           m.pnl_rho + m.pnl_charm + m.pnl_carry + m.pnl_unexplained;
  };
  const ExplainTotals &m = p.tail_mean;
  EXPECT_NEAR(axes_of(m), m.pnl_total, 1e-9 * std::max(1.0, std::fabs(m.pnl_total)));
  EXPECT_NE(m.pnl_vega, 0.0);
  EXPECT_NE(m.shares_pnl, 0.0);
  EXPECT_EQ(m.n_ok, book.options.size() + book.shares.size());

  // Lane E: a scenario that re-marks carry books part of its P&L on the carry axis. Move a
  // per-scenario amount from unexplained to pnl_carry (each frame's identity unchanged): the
  // tail mean must average pnl_carry too, or its axes stop summing to its total.
  std::vector<HistoricalVarFrame> carried = res->frames;
  for (std::size_t k = 0; k < carried.size(); ++k) {
    const double c = 0.25 * static_cast<double>(k + 1) * std::max(1.0, std::fabs(m.pnl_total));
    carried[k].totals.pnl_carry = c;
    carried[k].totals.pnl_unexplained -= c;
  }
  const Result<HistoricalVarResult> again = summarize_historical_var(carried, cfg.confidences);
  ASSERT_TRUE(again.has_value()) << again.error().to_string();
  const ExplainTotals &mc = again->points[0].tail_mean;
  double hand_carry = 0.0;
  for (std::size_t k = 2; k < 5; ++k) {
    hand_carry += carried[order[k]].totals.pnl_carry / 3.0;
  }
  EXPECT_NE(mc.pnl_carry, 0.0);
  near(mc.pnl_carry, hand_carry, "pnl_carry (re-marked)");
  EXPECT_TRUE(same_bits(mc.pnl_total, m.pnl_total)) << "the P&L itself does not move";
  EXPECT_NEAR(axes_of(mc), mc.pnl_total, 1e-9 * std::max(1.0, std::fabs(mc.pnl_total)));
}

// ── Failure policy ───────────────────────────────────────────────────────────

TEST(HistoricalVar, UnresolvedTemplateIsCountedNotFatal) {
  History h;
  h.add(move_at(0, 0.0, 0.0, 0.0, 0.0), /*n_uids=*/2); // no surface for uid 3
  h.add(move_at(1, 0.006, -0.004, 0.0, 0.008), /*n_uids=*/2);
  h.pair(0, 1);
  HistoricalVarBook book;
  book.options.push_back(put_delta_leg(1u, 0.15, -0.25, -10.0));
  book.options.push_back(put_delta_leg(3u, 0.15, -0.35, 8.0)); // uid 3: no surface
  TheoLeg expired = moneyness_leg(1u, 0.30, 0.0, Side::Call, 2.0);
  expired.contract.maturity = MaturitySpec::absolute(rt::instant_ns(-10)); // already expired
  book.options.push_back(expired);

  const Result<HistoricalVarResult> res = historical_var(book, h.scenarios, base_config());
  ASSERT_TRUE(res.has_value()) << res.error().to_string();
  const HistoricalVarFrame &f = res->frames[0];
  EXPECT_EQ(f.n_legs_resolved, 1u);
  EXPECT_EQ(f.n_legs_unresolved, 2u);
  EXPECT_TRUE(f.admitted) << "the resolved part of the book still makes a scenario";
  EXPECT_EQ(f.totals.n_ok, 1u);
  EXPECT_EQ(f.totals.n_excluded, 0u) << "an unresolved template never reaches the explain";
  EXPECT_EQ(res->n_admitted, 1u);
  EXPECT_TRUE(std::isfinite(res->points[0].value_at_risk));
  ASSERT_EQ(res->resolved_legs.size(), 3u);
  EXPECT_EQ(res->resolved_legs[0].status, TheoStatus::Ok);
  EXPECT_EQ(res->resolved_legs[1].status, TheoStatus::SurfaceUnavailable);
  EXPECT_EQ(res->resolved_legs[1].fingerprint, 0u);
  EXPECT_TRUE(std::isnan(res->resolved_legs[1].leg.K));
  EXPECT_EQ(res->resolved_legs[1].leg.uid, 3u);
  EXPECT_EQ(res->resolved_legs[2].status, TheoStatus::MaturityUnavailable);
  // Same frame P&L as the book without the two dead templates.
  HistoricalVarBook live;
  live.options.push_back(book.options[0]);
  const Result<HistoricalVarResult> ref = historical_var(live, h.scenarios, base_config());
  ASSERT_TRUE(ref.has_value());
  EXPECT_TRUE(same_bits(f.totals.pnl_total, ref->frames[0].totals.pnl_total));
}

TEST(HistoricalVar, NoSurfaceStateIsRefused) {
  History h;
  h.add(move_at(0, 0.0, 0.0, 0.0, 0.0));
  h.add(move_at(1, 0.006, -0.004, 0.003, 0.008));
  const std::unique_ptr<MarksOnlyState> marks = make_marks_only_state();
  HistoricalVarBook book = reference_book(/*with_shares=*/true);

  // A marks-only state cannot host theoretical templates, as base or as shifted state:
  // both scenarios are refused (not admitted) even though the share legs could be priced.
  std::vector<HistoricalVarScenario> scenarios = {
      {&marks->state, &h.state(1)}, {&h.state(0), &marks->state}, {&h.state(0), &h.state(1)}};
  const Result<HistoricalVarResult> res = historical_var(book, scenarios, base_config());
  ASSERT_TRUE(res.has_value()) << res.error().to_string();
  ASSERT_EQ(res->frames.size(), 3u);
  for (std::size_t i = 0; i < 2; ++i) {
    EXPECT_FALSE(res->frames[i].admitted) << i;
    EXPECT_EQ(res->frames[i].n_legs_resolved, 0u) << i;
    EXPECT_EQ(res->frames[i].n_legs_unresolved, book.options.size()) << i;
    EXPECT_EQ(res->frames[i].totals.pnl_total, 0.0) << i;
  }
  EXPECT_TRUE(res->frames[2].admitted);
  EXPECT_EQ(res->n_admitted, 1u);

  // With every scenario refused there is no sample: the call still succeeds and says so.
  const std::vector<HistoricalVarScenario> only_marks = {{&marks->state, &h.state(1)}};
  const Result<HistoricalVarResult> none = historical_var(book, only_marks, base_config());
  ASSERT_TRUE(none.has_value());
  EXPECT_EQ(none->n_admitted, 0u);
  ASSERT_EQ(none->points.size(), 2u);
  EXPECT_TRUE(std::isnan(none->points[0].value_at_risk));
  EXPECT_TRUE(std::isnan(none->points[0].expected_shortfall));
  EXPECT_EQ(none->points[0].tail_scenarios, 0u);
  EXPECT_TRUE(std::isnan(none->reference_pv));
}

TEST(HistoricalVar, InvalidArgumentsAreRefused) {
  History h;
  h.add(move_at(0, 0.0, 0.0, 0.0, 0.0));
  h.add(move_at(1, 0.006, -0.004, 0.003, 0.008));
  h.pair(0, 1);
  const HistoricalVarBook book = reference_book(/*with_shares=*/false);
  const auto invalid = [](const Result<HistoricalVarResult> &r) {
    return !r.has_value() && r.error().code() == ErrorCode::InvalidArgument;
  };

  EXPECT_TRUE(invalid(historical_var(HistoricalVarBook{}, h.scenarios)));
  const std::vector<HistoricalVarScenario> null_base = {{nullptr, &h.state(1)}};
  EXPECT_TRUE(invalid(historical_var(book, null_base)));
  const std::vector<HistoricalVarScenario> null_shifted = {{&h.state(0), nullptr}};
  EXPECT_TRUE(invalid(historical_var(book, null_shifted)));
  for (const double c : {0.0, 1.0, -0.5, std::numeric_limits<double>::quiet_NaN()}) {
    HistoricalVarConfig cfg;
    cfg.confidences = {0.95, c};
    EXPECT_TRUE(invalid(historical_var(book, h.scenarios, cfg))) << c;
  }
  HistoricalVarConfig bad_family;
  bad_family.family.n_sigma = 2;
  EXPECT_TRUE(invalid(historical_var(book, h.scenarios, bad_family)));
  HistoricalVarConfig bad_premium;
  bad_premium.max_excluded_premium_frac = -1.0;
  EXPECT_TRUE(invalid(historical_var(book, h.scenarios, bad_premium)));
  HistoricalVarBook nan_qty = book;
  nan_qty.options[0].qty = std::numeric_limits<double>::quiet_NaN();
  EXPECT_TRUE(invalid(historical_var(nan_qty, h.scenarios)));
  // No scenarios is a valid (empty) history.
  const Result<HistoricalVarResult> empty =
      historical_var(book, std::span<const HistoricalVarScenario>{});
  ASSERT_TRUE(empty.has_value());
  EXPECT_EQ(empty->frames.size(), 0u);
  EXPECT_EQ(empty->n_admitted, 0u);
}

TEST(HistoricalVar, ExcludedLegNeedsAKnownPremiumStrictlyBelowTheFraction) {
  // A share leg on uid 3 next to an option template on uid 1. Pair 0: uid 3 is absent from
  // the BASE state, so the leg's base value -- its premium -- is unknown (explain books it at
  // premium 0). Pair 1: uid 3 is priced on the base state and absent from the shifted one,
  // so the leg is excluded with a known premium |qty S_b|.
  History h;
  h.add(move_at(0, 0.0, 0.0, 0.0, 0.0), /*n_uids=*/2);
  h.add(move_at(1, 0.006, -0.004, 0.003, 0.008));
  h.add(move_at(2, 0.004, -0.002, 0.001, 0.004), /*n_uids=*/2);
  h.pair(0, 1);
  h.pair(1, 2);
  HistoricalVarBook book;
  book.options.push_back(put_delta_leg(1u, 0.15, -0.25, 10.0));
  book.shares.push_back(ShareLeg{9001u, 3u, 50.0});
  const double known_premium = 50.0 * h.state(1).underlier(3u)->spot;

  // Default fraction 0: neither pair is admitted -- a zero premium is not "below 0", and an
  // unknown one is not zero.
  const Result<HistoricalVarResult> strict = historical_var(book, h.scenarios, base_config());
  ASSERT_TRUE(strict.has_value()) << strict.error().to_string();
  for (std::size_t i = 0; i < 2; ++i) {
    const ExplainTotals &t = strict->frames[i].totals;
    EXPECT_EQ(t.n_ok, 1u) << i;
    EXPECT_EQ(t.n_excluded, 1u) << i;
    EXPECT_FALSE(strict->frames[i].admitted) << i;
  }
  EXPECT_EQ(strict->frames[0].totals.excluded_premium, 0.0) << "unknown, booked at 0";
  EXPECT_DOUBLE_EQ(strict->frames[1].totals.excluded_premium, known_premium);
  EXPECT_EQ(strict->n_admitted, 0u);

  // A fraction that covers the known premium admits pair 1 only; one that does not, neither.
  const double pv_base = std::fabs(strict->frames[1].totals.pv_base);
  ASSERT_GT(pv_base, 0.0);
  HistoricalVarConfig covering = base_config();
  covering.max_excluded_premium_frac = 2.0 * known_premium / pv_base;
  const Result<HistoricalVarResult> cov = historical_var(book, h.scenarios, covering);
  ASSERT_TRUE(cov.has_value()) << cov.error().to_string();
  EXPECT_FALSE(cov->frames[0].admitted) << "an unknown premium is never admitted";
  EXPECT_TRUE(cov->frames[1].admitted);
  EXPECT_EQ(cov->n_admitted, 1u);
  HistoricalVarConfig short_of = base_config();
  short_of.max_excluded_premium_frac = 0.5 * known_premium / pv_base;
  const Result<HistoricalVarResult> sh = historical_var(book, h.scenarios, short_of);
  ASSERT_TRUE(sh.has_value()) << sh.error().to_string();
  EXPECT_FALSE(sh->frames[0].admitted);
  EXPECT_FALSE(sh->frames[1].admitted);

  // A KNOWN zero premium is still an exclusion: a zero-quantity share on uid 3 is priced on
  // pair 1's base state (premium |0 * S_b| = 0) and missing from its shifted state. At the
  // default fraction 0 it is dropped -- 0 is not strictly below 0 -- and any positive
  // fraction admits it.
  HistoricalVarBook zero_book;
  zero_book.options = book.options;
  zero_book.shares.push_back(ShareLeg{9002u, 3u, 0.0});
  const std::vector<HistoricalVarScenario> pair1 = {h.scenarios[1]};
  const Result<HistoricalVarResult> zero = historical_var(zero_book, pair1, base_config());
  ASSERT_TRUE(zero.has_value()) << zero.error().to_string();
  const ExplainTotals &zt = zero->frames[0].totals;
  EXPECT_EQ(zt.n_ok, 1u);
  EXPECT_EQ(zt.n_excluded, 1u);
  EXPECT_EQ(zt.excluded_premium, 0.0);
  EXPECT_FALSE(zero->frames[0].admitted) << "a known zero premium is not below a zero fraction";
  HistoricalVarConfig any_fraction = base_config();
  any_fraction.max_excluded_premium_frac = 1e-12;
  const Result<HistoricalVarResult> zero_ok = historical_var(zero_book, pair1, any_fraction);
  ASSERT_TRUE(zero_ok.has_value()) << zero_ok.error().to_string();
  EXPECT_TRUE(zero_ok->frames[0].admitted);
}

// A copy of `owned`'s market on the VolTime clock. The synthetic instants (kT0 = 2023-11-14)
// lie before VolTime's covered window (2024-01-01 .. 2032-12-31), so this clock refuses
// every expiry a template resolves to.
struct VolTimeState {
  MarketStateStorage storage;
  MarketState state;
};

[[nodiscard]] std::unique_ptr<VolTimeState> vol_time_copy(const rt::OwnedState &owned) {
  auto out = std::make_unique<VolTimeState>();
  std::vector<SurfaceRef> refs;
  refs.reserve(owned.surfaces.size());
  for (const PricedSurface &s : owned.surfaces) {
    refs.emplace_back(&s);
  }
  TimeSpec time;
  time.convention = TimeConvention::VolTime;
  out->storage = market_state_from_surfaces(owned.state.ts_ns(), time, refs).value();
  out->state = make_market_state(out->storage).value();
  return out;
}

TEST(HistoricalVar, ClockThatRefusesAnExpiryDropsOnlyThatScenario) {
  // A TimeSpec that refuses one of the book's expiries is a scenario-level refusal: the call
  // succeeds, that pair is not admitted, the others are. (The kernel reports the refusal as
  // InvalidArgument, like a structural error; historical_var tells them apart before it runs.)
  History h;
  h.add(move_at(0, 0.0, 0.0, 0.0, 0.0));
  h.add(move_at(1, 0.006, -0.004, 0.003, 0.008));
  h.add(move_at(2, -0.004, 0.002, -0.001, -0.004));
  const std::unique_ptr<VolTimeState> refusing = vol_time_copy(*h.states[1]);
  ASSERT_FALSE(refusing->state.time_to_expiry(rt::expiry_ns(1)).has_value())
      << "the VolTime clock was expected to refuse a 2024 expiry measured from 2023";
  const HistoricalVarBook book = reference_book(/*with_shares=*/true);
  const std::vector<HistoricalVarScenario> scenarios = {
      {&h.state(0), &h.state(1)}, {&refusing->state, &h.state(2)}, {&h.state(1), &h.state(2)}};

  const Result<HistoricalVarResult> res = historical_var(book, scenarios, base_config());
  ASSERT_TRUE(res.has_value()) << res.error().to_string();
  ASSERT_EQ(res->frames.size(), 3u);
  EXPECT_TRUE(res->frames[0].admitted);
  EXPECT_FALSE(res->frames[1].admitted);
  EXPECT_EQ(res->frames[1].n_legs_resolved, book.options.size())
      << "the templates resolved; only the clock refused the pair";
  EXPECT_EQ(res->frames[1].totals.n_ok, 0u);
  EXPECT_TRUE(res->frames[2].admitted);
  EXPECT_EQ(res->n_admitted, 2u);
}

TEST(HistoricalVar, ForeignWorkspaceIsAnErrorNotADroppedScenario) {
  // historical_var re-binds its workspace to every scenario's book (RiskWorkspace::reserve).
  // If that ever stopped, the kernel would refuse the workspace as bound to another book; that
  // refusal must fail the call, not drop the scenario as if its clock had refused it.
  History h;
  h.add(move_at(0, 0.0, 0.0, 0.0, 0.0));
  h.add(move_at(1, 0.006, -0.004, 0.003, 0.008));
  h.pair(0, 1);
  const std::vector<OptionLeg> all = rt::make_option_legs();
  ASSERT_GE(all.size(), 2u);
  const std::vector<OptionLeg> two(all.begin(), all.begin() + 2);
  const RiskBook book_a = RiskBook::create(all, {}).value();
  const RiskBook book_b = RiskBook::create(two, {}).value();
  const PreparedRiskBook prepared_a = PreparedRiskBook::create(book_a).value();
  const PreparedRiskBook prepared_b = PreparedRiskBook::create(book_b).value();
  const RiskKernel kernel = RiskKernel::family();

  RiskWorkspace ws;
  ws.reserve(prepared_a); // bound to book A
  RiskFrame base;
  RiskFrame shifted;
  ExplainResult out;
  const Result<historical_var_detail::PairOutcome> foreign = historical_var_detail::evaluate_pair(
      kernel, prepared_b, h.scenarios[0], RiskNeeds{}, ExplainOptions{}, ws, base, shifted, out);
  ASSERT_FALSE(foreign.has_value()) << "a structural refusal was reported as a refused pair";
  EXPECT_EQ(foreign.error().code(), ErrorCode::InvalidArgument);

  // Control: re-bound to book B, the same pair is explained.
  ws.reserve(prepared_b);
  base.book_logical_id = 0u;
  shifted.book_logical_id = 0u;
  const Result<historical_var_detail::PairOutcome> bound = historical_var_detail::evaluate_pair(
      kernel, prepared_b, h.scenarios[0], RiskNeeds{}, ExplainOptions{}, ws, base, shifted, out);
  ASSERT_TRUE(bound.has_value()) << bound.error().to_string();
  EXPECT_EQ(*bound, historical_var_detail::PairOutcome::Explained);
  EXPECT_EQ(out.total.n_ok, 2u);
}

// Lane E (I-4): the scenario pre-check reads each expiry's clock the way the kernel does
// (risk_detail::expiry_T). An expiry the TimeSpec refuses (VolTime past its 2032 calendar) is
// priced by the kernel when its ExpiryCarry row carries a finite T_override, so the pair must
// be explained, not refused; without the override it is still Refused.
namespace {

[[nodiscard]] std::unique_ptr<MarketStateStorage>
clock_override_state(std::int64_t ts_ns, std::int64_t expiry_ns, double T_override, double spot) {
  auto st = std::make_unique<MarketStateStorage>();
  st->ts_ns = ts_ns;
  st->time.convention = TimeConvention::VolTime;
  UnderlierState u;
  u.uid = 1u;
  u.spot = spot;
  u.flat_rate = 0.04;
  u.flat_q = 0.01;
  u.carry = CarryFrom::State;
  st->underliers.push_back(u);
  st->term_carries.push_back({ExpiryCarry{expiry_ns, 0.0, 0.04, 0.01, T_override}});
  MarketStateStorage::MarksColumns marks;
  marks.uid = 1u;
  for (const Side side : {Side::Call, Side::Put}) {
    marks.strike.push_back(100.0);
    marks.expiry_ns.push_back(expiry_ns);
    marks.side.push_back(side);
    marks.sigma.push_back(0.25);
  }
  st->marks.push_back(std::move(marks));
  st->normalize_marks();
  st->underliers[0].term_carry = st->term_carries[0];
  return st;
}

} // namespace

TEST(HistoricalVar, ClockOverrideAdmitsAnExpiryTheTimeSpecRefuses) {
  constexpr std::int64_t kHourNs = 3'600'000'000'000LL;
  const std::int64_t expiry = expiry_instant_ns("2033-06-17");
  const std::int64_t t0 = expiry_instant_ns("2032-11-01") - 6 * kHourNs; // 10:00 ET
  const std::int64_t t1 = t0 + kHourNs / 2;
  ASSERT_GT(expiry, 0);
  const std::vector<OptionLeg> legs{{1u, 1u, 100.0, expiry, Side::Call, 1.0, 100.0},
                                    {2u, 1u, 100.0, expiry, Side::Put, -1.0, 100.0}};
  const RiskBook book = RiskBook::create(legs, {}).value();
  const PreparedRiskBook prepared = PreparedRiskBook::create(book).value();
  const RiskKernel kernel = RiskKernel::family();
  constexpr double kT = 0.62;
  constexpr double kStep = 1.0 / (365.25 * 48.0);
  constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

  const auto run = [&](double T_base, double T_shifted, ExplainResult &out) {
    const auto bs = clock_override_state(t0, expiry, T_base, 100.0);
    const auto ts = clock_override_state(t1, expiry, T_shifted, 100.4);
    const MarketState b = make_market_state(*bs).value();
    const MarketState t = make_market_state(*ts).value();
    // The premise: the TimeSpec refuses the expiry but not the step.
    EXPECT_FALSE(b.time_to_expiry(expiry).has_value());
    EXPECT_TRUE(time_to_expiry_years(t0, t1, b.time()).has_value());
    const HistoricalVarScenario scenario{&b, &t};
    RiskWorkspace ws;
    ws.reserve(prepared);
    RiskFrame base;
    RiskFrame shifted;
    return historical_var_detail::evaluate_pair(kernel, prepared, scenario, RiskNeeds{},
                                                ExplainOptions{}, ws, base, shifted, out);
  };

  ExplainResult with;
  const Result<historical_var_detail::PairOutcome> priced = run(kT, kT - kStep, with);
  ASSERT_TRUE(priced.has_value()) << priced.error().to_string();
  EXPECT_EQ(*priced, historical_var_detail::PairOutcome::Explained)
      << "the kernel prices an overridden expiry; the pre-check must not refuse it";
  EXPECT_EQ(with.total.n_ok, 2u);
  EXPECT_TRUE(std::isfinite(with.total.pnl_total));
  ASSERT_EQ(with.d_time.size(), 2u);
  EXPECT_NEAR(with.d_time[0], kStep, 1e-15) << "rolled on the overridden clock";

  ExplainResult without;
  const Result<historical_var_detail::PairOutcome> refused = run(kNaN, kNaN, without);
  ASSERT_TRUE(refused.has_value()) << refused.error().to_string();
  EXPECT_EQ(*refused, historical_var_detail::PairOutcome::Refused);
}

// ── Kernels and determinism ──────────────────────────────────────────────────

TEST(HistoricalVar, FamilyAndColdAgreeWithinTolerance) {
  // A synthetic 3-state history: s0 -> s1 -> s2 as two consecutive pairs.
  History h;
  h.add(move_at(0, 0.0, 0.0, 0.0, 0.0));
  h.add(move_at(1, 0.009, -0.007, 0.004, 0.010, 0.01));
  h.add(move_at(2, -0.004, 0.003, -0.006, -0.006, -0.01));
  h.pair(0, 1);
  h.pair(1, 2);
  HistoricalVarBook book;
  book.options.push_back(put_delta_leg(1u, 0.15, -0.25, 10.0));
  book.options.push_back(moneyness_leg(1u, 0.30, 0.05, Side::Call, 6.0));
  book.options.push_back(moneyness_leg(2u, 0.30, -0.08, Side::Put, 4.0));
  book.options.push_back(put_delta_leg(3u, 0.15, -0.35, 8.0));

  // Every qty is long, so each state's net pv is its gross pv (the bounds below use it).
  for (const TheoLeg &leg : book.options) {
    ASSERT_GT(leg.qty, 0.0);
  }

  HistoricalVarConfig fam;
  fam.confidences = {0.5, 0.95};
  fam.keep_resolved_legs = true; // the route witness below rebuilds each pair's book
  // Every (uid, expiry, side) group here is one lane, which family economics prices cold;
  // turn it off so the comparison is family against cold, not cold against cold.
  fam.family.family_economics = false;
  HistoricalVarConfig cold = fam;
  cold.kernel = RiskKernelKind::Cold;
  const Result<HistoricalVarResult> f = historical_var(book, h.scenarios, fam);
  const Result<HistoricalVarResult> c = historical_var(book, h.scenarios, cold);
  ASSERT_TRUE(f.has_value()) << f.error().to_string();
  ASSERT_TRUE(c.has_value()) << c.error().to_string();
  ASSERT_EQ(f->n_admitted, 2u);
  ASSERT_EQ(c->n_admitted, 2u);
  ASSERT_GT(c->reference_pv, 0.0);

  // The family kernel's contract is the design's price gate: per contract, |P_family -
  // P_cold| <= g * P_cold with g = 1e-6 (the calibration target of FamilyKernelPolicy's
  // adaptive node count). On this all-long book that bounds everything reported here:
  //   each state's pv:  |pv_f - pv_c| <= g * pv_c;
  //   pair i's P&L:     |pnl_f - pnl_c| <= g * (pv_base_c + pv_target_c) =: eps_i;
  //   VaR / ES:         order statistics and tail means of the losses are 1-Lipschitz in the
  //                     sup norm, so they move by at most max_i eps_i;
  //   reference_pv:     the mean pv_base, so by at most g * reference_pv_c.
  // A 1e-4 relative divergence of the prices moves the pv levels by ~1e-4 * pv and misses
  // the first bound by two orders of magnitude.
  constexpr double kPriceGate = 1e-6;
  double eps_max = 0.0;
  for (std::size_t i = 0; i < 2; ++i) {
    const ExplainTotals &tf = f->frames[i].totals;
    const ExplainTotals &tc = c->frames[i].totals;
    ASSERT_GT(tc.pv_base, 0.0) << i;
    ASSERT_GT(tc.pv_target, 0.0) << i;
    EXPECT_NEAR(tf.pv_base, tc.pv_base, kPriceGate * tc.pv_base) << i;
    EXPECT_NEAR(tf.pv_target, tc.pv_target, kPriceGate * tc.pv_target) << i;
    const double eps = kPriceGate * (tc.pv_base + tc.pv_target);
    EXPECT_NEAR(tf.pnl_total, tc.pnl_total, eps) << i;
    EXPECT_NE(tf.pnl_total, 0.0) << i;
    eps_max = std::max(eps_max, eps);
  }
  EXPECT_NEAR(f->reference_pv, c->reference_pv, kPriceGate * c->reference_pv);
  ASSERT_EQ(f->points.size(), c->points.size());
  for (std::size_t k = 0; k < f->points.size(); ++k) {
    EXPECT_NEAR(f->points[k].value_at_risk, c->points[k].value_at_risk, eps_max) << k;
    EXPECT_NEAR(f->points[k].expected_shortfall, c->points[k].expected_shortfall, eps_max) << k;
    // The brief's Done criterion, tighter than the derived bound (~2e-6 * reference_pv); it
    // holds here with orders of magnitude to spare (~1e-10 relative).
    EXPECT_NEAR(f->points[k].value_at_risk, c->points[k].value_at_risk,
                kPriceGate * c->reference_pv)
        << k;
    EXPECT_NEAR(f->points[k].expected_shortfall, c->points[k].expected_shortfall,
                kPriceGate * c->reference_pv)
        << k;
  }

  // Non-vacuity: the family route really ran. Family economics (lane A2, on by default)
  // prices a group whose family cannot amortize -- every group of this book is one lane --
  // cold-analytic, flagged FamilyUneconomic. Rebuild each pair's resolved book and evaluate
  // it with the same family policy on both of its states: every row must be served without
  // the economics demotion and some by a family.
  const RiskKernel kernel = RiskKernel::family(fam.family);
  const std::size_t n_templates = book.options.size();
  ASSERT_EQ(f->resolved_legs.size(), h.scenarios.size() * n_templates);
  for (std::size_t i = 0; i < h.scenarios.size(); ++i) {
    std::vector<OptionLeg> legs;
    for (std::size_t j = 0; j < n_templates; ++j) {
      const ResolvedTheo &r = f->resolved_legs[i * n_templates + j];
      ASSERT_EQ(r.status, TheoStatus::Ok) << i << "/" << j;
      legs.push_back(r.leg);
    }
    const Result<RiskBook> resolved = RiskBook::create(legs, {});
    ASSERT_TRUE(resolved.has_value()) << resolved.error().to_string();
    const Result<PreparedRiskBook> prepared = PreparedRiskBook::create(*resolved);
    ASSERT_TRUE(prepared.has_value()) << prepared.error().to_string();
    for (const MarketState *state : {h.scenarios[i].base, h.scenarios[i].shifted}) {
      RiskWorkspace ws;
      ws.reserve(*prepared);
      RiskFrame frame;
      ASSERT_TRUE(kernel.evaluate(*prepared, *state, fam.needs, frame, ws).has_value()) << i;
      EXPECT_GT(frame.counters.family_rows, 0u) << i;
      EXPECT_EQ(frame.counters.uneconomic_rows, 0u) << i;
    }
  }
  // And historical_var itself took it: the family totals are not the cold kernel's bits.
  EXPECT_FALSE(same_bits(f->frames[0].totals.pnl_total, c->frames[0].totals.pnl_total));
}

TEST(HistoricalVar, DeterministicAcrossThreads) {
  const History h = five_pair_history();
  const HistoricalVarBook book = reference_book(/*with_shares=*/true);
  struct Shape {
    unsigned kernel_threads;
    unsigned scenario_threads;
  };
  // kernel_threads == 1 fans the scenarios out; anything else runs them serially and lets
  // the kernel fan out. Every shape must produce the same bits.
  const Shape shapes[] = {{1u, 1u}, {1u, 2u}, {1u, 8u}, {0u, 0u}, {4u, 1u}};
  std::vector<HistoricalVarResult> runs;
  for (const Shape &s : shapes) {
    HistoricalVarConfig cfg = base_config();
    cfg.family.n_threads = s.kernel_threads;
    cfg.n_threads = s.scenario_threads;
    cfg.explain.n_threads = s.scenario_threads;
    Result<HistoricalVarResult> r = historical_var(book, h.scenarios, cfg);
    ASSERT_TRUE(r.has_value()) << r.error().to_string();
    runs.push_back(std::move(*r));
  }
  const HistoricalVarResult &ref = runs.front();
  ASSERT_EQ(ref.n_admitted, 5u);
  for (std::size_t k = 1; k < runs.size(); ++k) {
    const HistoricalVarResult &r = runs[k];
    ASSERT_EQ(r.frames.size(), ref.frames.size());
    for (std::size_t i = 0; i < r.frames.size(); ++i) {
      expect_totals_bits(r.frames[i].totals, ref.frames[i].totals, i);
      EXPECT_EQ(r.frames[i].admitted, ref.frames[i].admitted);
    }
    ASSERT_EQ(r.points.size(), ref.points.size());
    for (std::size_t p = 0; p < r.points.size(); ++p) {
      EXPECT_TRUE(same_bits(r.points[p].value_at_risk, ref.points[p].value_at_risk)) << k;
      EXPECT_TRUE(same_bits(r.points[p].expected_shortfall, ref.points[p].expected_shortfall));
      expect_totals_bits(r.points[p].tail_mean, ref.points[p].tail_mean, p);
    }
    EXPECT_TRUE(same_bits(r.reference_pv, ref.reference_pv));
    ASSERT_EQ(r.resolved_legs.size(), ref.resolved_legs.size());
    for (std::size_t j = 0; j < r.resolved_legs.size(); ++j) {
      EXPECT_TRUE(same_bits(r.resolved_legs[j].leg.K, ref.resolved_legs[j].leg.K)) << j;
    }
  }
}

} // namespace
