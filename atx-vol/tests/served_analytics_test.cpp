// Lane A1 (r6, 2026-10-23): served analytics on the live book (api/analytics/served_analytics.hpp)
// - one declared model / version / convention per answer, the hedge conventions (contract
// sigma, sticky strike, sticky delta-moneyness, refitted), certified inputs (a Sampled /
// price-only / refused answer is never one), bounded Taylor scenarios beside exact
// repricing with route and error labels, events (expiry settlement, the next event,
// D1's ex-dividend roll), failed exposure per cell, P&L attribution and the deterministic
// aggregation across worker counts.
#include "atx/vol/api/analytics/served_analytics.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <optional>
#include <span>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "atx/vol/api/fitting/quote_slice.hpp"
#include "atx/vol/api/fitting/serve_book.hpp"
#include "atx/vol/api/fitting/slice_serve_record.hpp"
#include "atx/vol/api/pricing/american.hpp"
#include "atx/vol/api/pricing/american_discrete_div_kernel.hpp"
#include "atx/vol/api/pricing/chain_tick.hpp"

namespace atx::vol {
namespace {

constexpr double kSpot = 100.0;
constexpr double kRate = 0.03;
constexpr double kYield = 0.01;
constexpr double kT = 0.25;
constexpr std::int64_t kNow = 1'800'000'000'000'000'000; // Unix ns, 2027-01-15
constexpr std::int64_t kDayNs = 86'400'000'000'000;

std::uint64_t bits(double value) { return std::bit_cast<std::uint64_t>(value); }

double smile(double strike, double forward) {
  const double k = std::log(strike / forward);
  return 0.22 - 0.10 * k + 0.25 * k * k;
}

// Accurate Andersen-Lake prices on the smile, strikes 70-130 every 2.5, both sides, +/- 1 %
// spreads (the H0-A / H1 fixture).
QuoteSliceFit fit_value(double vol_shift) {
  const QuoteSliceMarket market{kSpot, kT, kT, kRate, kYield, 0.0};
  const double forward = kSpot * std::exp((kRate - kYield) * kT);
  std::vector<QuoteSliceQuote> quotes;
  for (int i = 0; i < 25; ++i) {
    const double K = 70.0 + 2.5 * static_cast<double>(i);
    const double q = kYield + 2.0e-4 * std::sin(0.37 * K);
    for (const Side side : {Side::Call, Side::Put}) {
      const Result<double> price =
          andersen_lake(kSpot, K, kT, smile(K, forward) + vol_shift, kRate, q, side);
      if (!price) continue;
      const double spread = std::max(0.01, 0.01 * *price);
      quotes.push_back({K, side, std::max(*price - spread, 0.0), *price + spread, q});
    }
  }
  Result<QuoteSliceFit> fit = fit_quote_slice(market, quotes);
  EXPECT_TRUE(fit.has_value());
  return fit ? std::move(*fit) : QuoteSliceFit{};
}

const QuoteSliceFit &fit_a() {
  static const auto fit = std::make_shared<const QuoteSliceFit>(fit_value(0.0));
  return *fit;
}
const QuoteSliceFit &fit_b() {
  static const auto fit = std::make_shared<const QuoteSliceFit>(fit_value(0.02));
  return *fit;
}

std::shared_ptr<const SliceServeRecord> record_of(const QuoteSliceFit &fit,
                                                  const ServeRecordPolicy &policy) {
  Result<std::shared_ptr<const SliceServeRecord>> built = build_serve_record(fit, policy);
  EXPECT_TRUE(built.has_value());
  return built ? *built : nullptr;
}
ServeRecordPolicy no_ladders() {
  ServeRecordPolicy p;
  p.ladders = false;
  return p;
}
// Tier 1 only (the family): one map at every spot.
std::shared_ptr<const SliceServeRecord> rec_a() {
  static const auto r = record_of(fit_a(), no_ladders());
  return r;
}
std::shared_ptr<const SliceServeRecord> rec_b() {
  static const auto r = record_of(fit_b(), no_ladders());
  return r;
}
// The default 12 x 6 ladders: the fixture's put ladders are served on a Sampled estimate.
std::shared_ptr<const SliceServeRecord> rec_default_ladders() {
  static const auto r = record_of(fit_a(), ServeRecordPolicy{});
  return r;
}

ServeBookPolicy inline_policy() {
  ServeBookPolicy p;
  p.build_workers = 0;
  return p;
}

ServeBookEntry entry(std::uint64_t id, std::shared_ptr<const SliceServeRecord> record,
                     std::uint64_t tag = 0, std::int64_t expiry_ns = 0,
                     std::int64_t next_event_ns = 0) {
  ServeBookEntry e;
  e.expiry_id = id;
  e.record = std::move(record);
  e.tag = tag;
  e.expiry_ns = expiry_ns;
  e.next_event_ns = next_event_ns;
  return e;
}

std::unique_ptr<ServeBook> book_of(std::vector<ServeBookEntry> entries) {
  auto book = std::make_unique<ServeBook>(inline_policy());
  EXPECT_TRUE(book->publish(entries).has_value());
  return book;
}

HedgeGreeksRequest hreq(double K, Side side, double spot, HedgeConvention c,
                        std::uint64_t id = 1, std::int64_t now = kNow) {
  HedgeGreeksRequest r;
  r.expiry_id = id;
  r.strike = K;
  r.side = side;
  r.spot = spot;
  r.now_ns = now;
  r.convention = c;
  return r;
}

ServeRequest sreq(double K, Side side, double spot, std::int64_t now = kNow) {
  ServeRequest r;
  r.strike = K;
  r.side = side;
  r.spot = spot;
  r.tier = ServeTier::Ladder;
  r.tier_max = ServeTier::Family;
  r.greeks = ServeGreekMask::four();
  r.now_ns = now;
  r.family_bound = true;
  return r;
}

const ServedNodeSide *node_side(const SliceServeRecord &rec, double K, Side side) {
  for (const ServedNode &n : rec.nodes()) {
    if (n.strike == K) return side == Side::Call ? &n.call : &n.put;
  }
  return nullptr;
}

// ── Declarations and the conventions ────────────────────────────────────────────────────

TEST(ServedHedgeGreeks, ContractSigmaIsTheRecordsAnswerAndCarriesTheDeclaration) {
  auto book = book_of({entry(1, rec_a(), 77)});
  ServeBookReader r = book->reader();
  ServeBookReader check = book->reader();
  const double S = 101.0;
  const ServeAnswer a = check.serve(1, sreq(105.0, Side::Call, S));
  ASSERT_EQ(a.status, ServeStatus::Ok);
  const HedgeGreeks h = hedge_greeks(r, hreq(105.0, Side::Call, S, HedgeConvention::ContractSigma));
  EXPECT_EQ(h.status, AnalyticsStatus::Ok);
  EXPECT_EQ(h.input_class, AnalyticsInputClass::Certified);
  EXPECT_EQ(bits(h.price), bits(a.price));
  EXPECT_EQ(bits(h.delta), bits(a.delta));
  EXPECT_EQ(bits(h.gamma), bits(a.gamma));
  EXPECT_EQ(bits(h.vega), bits(a.vega));
  EXPECT_EQ(bits(h.theta), bits(a.theta));
  EXPECT_EQ(bits(h.sigma), bits(a.sigma));
  for (const HedgeGreekReason reason : h.reason) EXPECT_EQ(reason, HedgeGreekReason::Served);
  EXPECT_EQ(h.dsigma_dspot, 0.0);
  EXPECT_EQ(h.slope_class, SmileSlopeClass::ZeroByConvention);
  EXPECT_EQ(h.certificate_class, a.certificate_class);
  EXPECT_EQ(bits(h.certificate_usd), bits(a.certificate_usd));
  EXPECT_EQ(h.tier, a.tier);
  const AnalyticsDeclaration &d = h.declaration;
  EXPECT_EQ(d.book_version, book->version());
  EXPECT_EQ(d.expiry_id, 1u);
  EXPECT_EQ(d.entry_tag, 77u);
  EXPECT_EQ(d.record_fit_sha256, rec_a()->version().fit_sha256);
  EXPECT_EQ(d.record_policy_sha256, rec_a()->version().policy_sha256);
  EXPECT_EQ(d.record_input_sha256, rec_a()->version().input_sha256);
  EXPECT_EQ(d.model, AnalyticsModel::QuotingRecord);
  EXPECT_EQ(d.greek_model, a.convention.model);
  EXPECT_EQ(d.convention, HedgeConvention::ContractSigma);
  EXPECT_EQ(d.clock.now_ns, kNow);
  EXPECT_EQ(d.clock.as_of_ns, rec_a()->version().as_of_ns);
  EXPECT_EQ(bits(d.clock.tau), bits(rec_a()->tau()));
  EXPECT_EQ(bits(d.price_map_delta), bits(a.price_map_delta));
}

TEST(ServedHedgeGreeks, StickyStrikeOnARecordIsTheContractSigmaNumbers) {
  auto book = book_of({entry(1, rec_a())});
  ServeBookReader r = book->reader();
  for (const double K : {90.0, 100.0, 112.5}) {
    for (const Side side : {Side::Call, Side::Put}) {
      const HedgeGreeks c = hedge_greeks(r, hreq(K, side, 99.0, HedgeConvention::ContractSigma));
      const HedgeGreeks s = hedge_greeks(r, hreq(K, side, 99.0, HedgeConvention::StickyStrike));
      EXPECT_EQ(bits(c.delta), bits(s.delta));
      EXPECT_EQ(bits(c.gamma), bits(s.gamma));
      EXPECT_EQ(bits(c.vega), bits(s.vega));
      EXPECT_EQ(bits(c.theta), bits(s.theta));
      EXPECT_EQ(s.declaration.convention, HedgeConvention::StickyStrike);
      EXPECT_EQ(s.slope_class, SmileSlopeClass::ZeroByConvention);
      EXPECT_EQ(s.status, AnalyticsStatus::Ok);
    }
  }
}

// Sticky delta-moneyness: the smile is a function of ln(K / F) and F moves with S, so
// delta gains vega x dsigma/dS = vega x (-sigma'(k) / S). Checked against the convention's
// own definition by a cold-kernel difference that moves the smile with the spot.
TEST(ServedHedgeGreeks, StickyDeltaMoneynessDeltaAddsVegaTimesTheSmileSlope) {
  auto book = book_of({entry(1, rec_a())});
  ServeBookReader r = book->reader();
  const SliceServeRecord &rec = *rec_a();
  const double S = rec.spot();
  const double K = 105.0;
  const HedgeGreeks ss = hedge_greeks(r, hreq(K, Side::Call, S, HedgeConvention::StickyStrike));
  const HedgeGreeks sm =
      hedge_greeks(r, hreq(K, Side::Call, S, HedgeConvention::StickyDeltaMoneyness));
  ASSERT_EQ(ss.status, AnalyticsStatus::Ok);
  ASSERT_EQ(sm.status, AnalyticsStatus::Ok);
  const IVolCurve &curve = rec.curve();
  const double k = std::log(K / curve.F());
  const double slope = (curve.iv(k + kSmileSlopeStep) - curve.iv(k - kSmileSlopeStep)) /
                       (2.0 * kSmileSlopeStep);
  EXPECT_EQ(bits(sm.curve_slope), bits(slope));
  EXPECT_EQ(bits(sm.dsigma_dspot), bits(-slope / S));
  EXPECT_EQ(sm.slope_class, SmileSlopeClass::FittedCurveCentralDifference);
  EXPECT_EQ(bits(sm.delta), bits(ss.delta + ss.vega * (-slope / S)));
  EXPECT_TRUE(std::isnan(sm.gamma));
  EXPECT_EQ(sm.reason[1], HedgeGreekReason::NeedsVannaVolga);
  EXPECT_EQ(bits(sm.vega), bits(ss.vega));
  EXPECT_EQ(bits(sm.theta), bits(ss.theta));
  EXPECT_EQ(sm.declaration.convention, HedgeConvention::StickyDeltaMoneyness);

  // The convention's definition: V(S, sigma(K; S)) with sigma(K; S) = sigma_served +
  // curve(ln(K / F(S))) - curve(k), F(S) = F_fit x S / S_fit.
  const ServedNodeSide *ns = node_side(rec, K, Side::Call);
  ASSERT_NE(ns, nullptr);
  const double h = 0.05;
  const auto value = [&](double spot) {
    const double kk = std::log(K / (curve.F() * spot / rec.spot()));
    const double sigma = ss.sigma + curve.iv(kk) - curve.iv(k);
    const Result<double> p = andersen_lake(spot, K, rec.tau(), sigma, rec.rate(), ns->q,
                                           Side::Call, fit_a().al_opts);
    return p ? *p : std::nan("");
  };
  const double fd = (value(S + h) - value(S - h)) / (2.0 * h);
  // The family map's delta sits ~2e-5 from the converged kernel (A0); the smile term here
  // is ~1e-2: the sticky-strike delta misses the convention by it, the convention's hits.
  EXPECT_NEAR(sm.delta, fd, 5.0e-4);
  EXPECT_GT(std::abs(ss.delta - fd), 5.0e-3);
}

TEST(ServedHedgeGreeks, RefittedSurfaceIsTypedUnsupportedNeverAWrongNumber) {
  auto book = book_of({entry(1, rec_a())});
  ServeBookReader r = book->reader();
  const HedgeGreeks ss = hedge_greeks(r, hreq(100.0, Side::Put, 99.0,
                                              HedgeConvention::StickyStrike));
  const HedgeGreeks rf = hedge_greeks(r, hreq(100.0, Side::Put, 99.0,
                                              HedgeConvention::RefittedSurface));
  EXPECT_EQ(bits(rf.price), bits(ss.price));
  EXPECT_TRUE(std::isnan(rf.delta) && std::isnan(rf.gamma) && std::isnan(rf.vega) &&
              std::isnan(rf.theta));
  for (const HedgeGreekReason reason : rf.reason) {
    EXPECT_EQ(reason, HedgeGreekReason::RequiresRefit);
  }
  EXPECT_EQ(rf.slope_class, SmileSlopeClass::None);
  EXPECT_EQ(rf.declaration.convention, HedgeConvention::RefittedSurface);
}

// review-H1 F1: a Sampled answer is an estimate - kept, typed, never Ok, never certified.
TEST(ServedHedgeGreeks, ASampledAnswerIsAnEstimateNeverACertifiedInput) {
  auto book = book_of({entry(1, rec_default_ladders())});
  ServeBookReader r = book->reader();
  ServeBookReader check = book->reader();
  const double S = 1.002 * kSpot;
  std::size_t sampled = 0;
  std::size_t certified = 0;
  for (const ServedNode &n : rec_default_ladders()->nodes()) {
    for (const Side side : {Side::Call, Side::Put}) {
      const ServeAnswer a = check.serve(1, sreq(n.strike, side, S));
      const HedgeGreeks h = hedge_greeks(r, hreq(n.strike, side, S,
                                                 HedgeConvention::StickyStrike));
      EXPECT_EQ(h.sampled, a.sampled);
      if (a.status != ServeStatus::Ok) continue;
      if (a.sampled) {
        ++sampled;
        EXPECT_EQ(h.input_class, AnalyticsInputClass::Estimate);
        EXPECT_EQ(h.status, AnalyticsStatus::Estimate);
        EXPECT_EQ(bits(h.price), bits(a.price)); // the estimate is kept, labelled
      } else {
        ++certified;
        EXPECT_EQ(h.status, AnalyticsStatus::Ok);
      }
    }
  }
  EXPECT_GT(sampled, 3u);
  EXPECT_GT(certified, 3u);
}

// review-A1 major: `sampled` alone keeps an answer out of the certified inputs, whatever its
// price class. A record sets `sampled` from EITHER statement (slice_serve_record.cpp: the
// price's class or the Greek map's), so at the fit's own spot a node answer carries the
// node bytes' Exact price class next to a Sampled ladder map: that answer must still be an
// Estimate (the Greeks it hands out come from the Sampled map).
TEST(ServedClassification, SampledNeverCertifiesWhateverThePriceClass) {
  for (const ServeStatus status : {ServeStatus::Ok, ServeStatus::Exercised}) {
    for (const ServeBoundClass cls :
         {ServeBoundClass::Exact, ServeBoundClass::RefinementValidated,
          ServeBoundClass::KernelTested}) {
      ServeAnswer a;
      a.status = status;
      a.price = 4.25;
      a.certificate_class = cls;
      a.sampled = false;
      EXPECT_EQ(classify_served_answer(a), AnalyticsInputClass::Certified);
      a.sampled = true;
      EXPECT_EQ(classify_served_answer(a), AnalyticsInputClass::Estimate)
          << static_cast<int>(status) << " " << static_cast<int>(cls);
      ServedChainStatement st;
      st.status = status;
      st.certificate_class = cls;
      st.sampled = false;
      EXPECT_EQ(classify_served_statement(st, 4.25), AnalyticsInputClass::Certified);
      st.sampled = true;
      st.map_certificate_class = ServeBoundClass::Sampled;
      EXPECT_EQ(classify_served_statement(st, 4.25), AnalyticsInputClass::Estimate)
          << static_cast<int>(status) << " " << static_cast<int>(cls);
    }
  }
}

// The same shape through the served path: the default-ladder record at the fit's OWN spot,
// node strikes. The price is the node's bytes (class Exact); where the side's ladder is
// served on a Sampled estimate the answer is sampled, and hedge_greeks / the chain
// statement must say Estimate (TickStatus Fallback on the chain), never Ok.
TEST(ServedHedgeGreeks, AnExactNodePriceOnASampledMapIsAnEstimateNotOk) {
  auto book = book_of({entry(1, rec_default_ladders())});
  ServeBookReader r = book->reader();
  ServeBookReader check = book->reader();
  const SliceServeRecord &rec = *rec_default_ladders();
  const double S = rec.spot(); // bit-equal: the at-fit branch
  std::vector<std::uint64_t> ids;
  std::vector<double> strikes;
  std::vector<Side> sides;
  std::size_t shaped = 0;
  for (const ServedNode &n : rec.nodes()) {
    for (const Side side : {Side::Call, Side::Put}) {
      const ServeAnswer a = check.serve(1, sreq(n.strike, side, S));
      if (a.status != ServeStatus::Ok || !a.sampled ||
          a.certificate_class != ServeBoundClass::Exact) {
        continue;
      }
      ASSERT_EQ(a.map_certificate_class, ServeBoundClass::Sampled);
      ++shaped;
      const HedgeGreeks h =
          hedge_greeks(r, hreq(n.strike, side, S, HedgeConvention::StickyStrike));
      EXPECT_EQ(h.certificate_class, ServeBoundClass::Exact);
      EXPECT_TRUE(h.sampled);
      EXPECT_EQ(h.input_class, AnalyticsInputClass::Estimate) << n.strike;
      EXPECT_EQ(h.status, AnalyticsStatus::Estimate) << n.strike;
      ids.push_back(1);
      strikes.push_back(n.strike);
      sides.push_back(side);
    }
  }
  ASSERT_GT(shaped, 0u);
  const std::size_t m = strikes.size();
  std::vector<double> price(m), delta(m), gamma(m), vega(m), theta(m);
  std::vector<TickStatus> status(m);
  std::vector<ServedChainStatement> statements(m);
  const ServedChainView view{ids, strikes, sides};
  const ChainTickOutputs out{price, delta, gamma, vega, theta, status};
  ASSERT_TRUE(r.serve_chain(S, kNow, view, ServeTier::Ladder, ServeTier::Family, out, statements)
                  .has_value());
  for (std::size_t k = 0; k < m; ++k) {
    EXPECT_TRUE(statements[k].sampled);
    EXPECT_EQ(statements[k].certificate_class, ServeBoundClass::Exact);
    EXPECT_EQ(classify_served_statement(statements[k], price[k]), AnalyticsInputClass::Estimate);
    EXPECT_EQ(status[k], TickStatus::Fallback);
  }
}

// A curve with no value at the two slope samples of one node strike (k +/- h): the rest of
// the fit, the record and the served node are untouched.
class SlopeHoleCurve final : public IVolCurve {
public:
  SlopeHoleCurve(std::unique_ptr<IVolCurve> inner, double strike)
      : IVolCurve(inner->T(), inner->F(), inner->df()), inner_(std::move(inner)),
        k_(std::log(strike / F())), strike_(strike) {}
  [[nodiscard]] double w(double k_log) const noexcept override {
    return hole(k_log) ? std::nan("") : inner_->w(k_log);
  }
  [[nodiscard]] double iv(double k_log) const noexcept override {
    return hole(k_log) ? std::nan("") : inner_->iv(k_log);
  }
  [[nodiscard]] VolCurveKind kind() const noexcept override { return inner_->kind(); }
  [[nodiscard]] std::size_t dof() const noexcept override { return inner_->dof(); }
  [[nodiscard]] std::unique_ptr<IVolCurve> clone() const override {
    return std::make_unique<SlopeHoleCurve>(inner_->clone(), strike_);
  }

private:
  [[nodiscard]] bool hole(double k_log) const noexcept {
    return k_log == k_ + kSmileSlopeStep || k_log == k_ - kSmileSlopeStep;
  }
  std::unique_ptr<IVolCurve> inner_;
  double k_;
  double strike_;
};

// review-A1 minor: no finite smile slope at the strike - sticky delta/moneyness delta is NaN
// with the typed SmileSlopeUnavailable, never the sticky-strike number passed off as it.
TEST(ServedHedgeGreeks, StickyDeltaMoneynessWithoutASmileSlopeIsTypedUnavailable) {
  constexpr double kK = 105.0;
  QuoteSliceFit fit = fit_value(0.0);
  ASSERT_TRUE(fit.curve != nullptr);
  fit.curve = std::make_unique<SlopeHoleCurve>(std::move(fit.curve), kK);
  const std::shared_ptr<const SliceServeRecord> rec = record_of(fit, no_ladders());
  ASSERT_NE(rec, nullptr);
  auto book = book_of({entry(1, rec)});
  ServeBookReader r = book->reader();
  const double S = rec->spot();
  const HedgeGreeks ss = hedge_greeks(r, hreq(kK, Side::Call, S, HedgeConvention::StickyStrike));
  ASSERT_EQ(ss.status, AnalyticsStatus::Ok);
  ASSERT_TRUE(std::isfinite(ss.delta));
  const HedgeGreeks sm =
      hedge_greeks(r, hreq(kK, Side::Call, S, HedgeConvention::StickyDeltaMoneyness));
  EXPECT_EQ(bits(sm.price), bits(ss.price)); // the price is still served
  EXPECT_TRUE(std::isnan(sm.delta));
  EXPECT_EQ(sm.reason[0], HedgeGreekReason::SmileSlopeUnavailable);
  EXPECT_TRUE(std::isnan(sm.curve_slope));
  EXPECT_TRUE(std::isnan(sm.dsigma_dspot));
  EXPECT_EQ(sm.slope_class, SmileSlopeClass::None);
  EXPECT_TRUE(std::isnan(sm.gamma));
  EXPECT_EQ(sm.reason[1], HedgeGreekReason::NeedsVannaVolga);
  EXPECT_EQ(bits(sm.vega), bits(ss.vega));
  EXPECT_EQ(sm.declaration.convention, HedgeConvention::StickyDeltaMoneyness);
}

TEST(ServedHedgeGreeks, RefusalsAndPriceOnlyPropagateTheirTypedReason) {
  QuoteSliceFit fit = fit_value(0.0);
  const std::size_t n = fit.prices->nodes().size();
  const std::size_t i = n / 2;
  fit.price_only_nodes.assign(n, 0u);
  fit.price_only_nodes[i] = 2u; // the put
  const auto priced_only = record_of(fit, no_ladders());
  ASSERT_NE(priced_only, nullptr);
  auto book = book_of({entry(1, priced_only), entry(2, rec_a(), 0, kNow - 1),
                       entry(3, rec_a(), 0, kNow + 90 * kDayNs)});
  ServeBookReader r = book->reader();

  const HedgeGreeks out = hedge_greeks(r, hreq(500.0, Side::Call, kSpot,
                                               HedgeConvention::StickyStrike));
  EXPECT_EQ(out.serve_status, ServeStatus::OutOfDomain);
  EXPECT_EQ(out.route, ServeRoute::Refit);
  EXPECT_EQ(out.status, AnalyticsStatus::Refused);
  EXPECT_EQ(out.input_class, AnalyticsInputClass::Refused);
  EXPECT_TRUE(std::isnan(out.price) && std::isnan(out.delta) && std::isnan(out.gamma));
  for (const HedgeGreekReason reason : out.reason) EXPECT_NE(reason, HedgeGreekReason::Served);

  const double K = priced_only->nodes()[i].strike;
  const HedgeGreeks po = hedge_greeks(r, hreq(K, Side::Put, kSpot, HedgeConvention::StickyStrike));
  EXPECT_EQ(po.serve_status, ServeStatus::PriceOnly);
  EXPECT_EQ(po.status, AnalyticsStatus::PriceOnly);
  EXPECT_EQ(po.input_class, AnalyticsInputClass::PriceOnly);
  EXPECT_EQ(bits(po.price), bits(priced_only->nodes()[i].put.price));
  EXPECT_EQ(po.price_only_reason, priced_only->nodes()[i].put.reason);
  EXPECT_TRUE(std::isnan(po.delta) && std::isnan(po.vega));

  const HedgeGreeks inval = hedge_greeks(r, hreq(100.0, Side::Call, kSpot,
                                                 HedgeConvention::StickyStrike, 2));
  EXPECT_EQ(inval.serve_status, ServeStatus::Invalidated);
  EXPECT_EQ(inval.route, ServeRoute::Refit);
  EXPECT_EQ(inval.status, AnalyticsStatus::Refused);

  const HedgeGreeks noclock = hedge_greeks(r, hreq(100.0, Side::Call, kSpot,
                                                   HedgeConvention::StickyStrike, 3, 0));
  EXPECT_EQ(noclock.serve_status, ServeStatus::Stale);
  EXPECT_EQ(noclock.status, AnalyticsStatus::Refused);

  const HedgeGreeks bad = hedge_greeks(r, hreq(100.0, Side::Call, -1.0,
                                               HedgeConvention::StickyStrike));
  EXPECT_EQ(bad.status, AnalyticsStatus::InvalidArgument);
  EXPECT_TRUE(std::isnan(bad.price));
}

// A0's near-edge band and exercised rows keep their reasons through the hedge output.
TEST(ServedHedgeGreeks, NearEdgeAndExercisedRowsPropagateA0sReasons) {
  auto book = book_of({entry(1, rec_a())});
  ServeBookReader r = book->reader();
  ServeBookReader check = book->reader();
  std::size_t band = 0;
  std::size_t exercised = 0;
  for (const double K : {120.0, 125.0, 130.0}) {
    for (int j = 0; j <= 120; ++j) {
      const double S = 70.0 + 0.25 * static_cast<double>(j);
      const ServeAnswer a = check.serve(1, sreq(K, Side::Put, S));
      const HedgeGreeks h = hedge_greeks(r, hreq(K, Side::Put, S, HedgeConvention::StickyStrike));
      if (a.status == ServeStatus::Ok && a.near_edge) {
        ++band;
        EXPECT_TRUE(h.near_edge);
        EXPECT_EQ(bits(h.edge_sd), bits(a.edge_sd));
        EXPECT_TRUE(std::isnan(h.gamma) && std::isnan(h.theta));
        EXPECT_EQ(h.reason[1], HedgeGreekReason::NearExerciseEdge);
        EXPECT_EQ(h.reason[3], HedgeGreekReason::NearExerciseEdge);
        EXPECT_EQ(h.reason[0], HedgeGreekReason::Served);
        EXPECT_EQ(h.reason[2], HedgeGreekReason::Served);
      }
      if (a.status == ServeStatus::Exercised) {
        ++exercised;
        EXPECT_EQ(h.serve_status, ServeStatus::Exercised);
        EXPECT_EQ(h.reason[0], HedgeGreekReason::Exercised);
        EXPECT_EQ(h.delta, -1.0);
      }
    }
  }
  EXPECT_GT(band, 0u);
  EXPECT_GT(exercised, 0u);
}

// ── Scenarios ────────────────────────────────────────────────────────────────────────────

std::vector<ServedPosition> small_book() {
  std::vector<ServedPosition> p;
  const std::array<double, 3> strikes{95.0, 100.0, 105.0};
  std::uint64_t id = 1;
  for (const double K : strikes) {
    p.push_back({id++, 1, K, Side::Call, 3.0, 100.0});
    p.push_back({id++, 1, K, Side::Put, -2.0, 100.0});
  }
  return p;
}

ScenarioSpec small_spec() {
  ScenarioSpec s;
  s.spot_rel = {-0.01, 0.0, 0.01};
  s.vol_abs = {0.0, 0.01};
  s.time_years = {0.0, 1.0 / 365.0};
  s.spot = 101.0;
  s.now_ns = kNow;
  s.keep_legs = true;
  s.cold_opts = fit_a().al_opts;
  return s;
}

TEST(ServedScenario, CellsCarryTheirRouteOrderBoxAndRemainder) {
  auto book = book_of({entry(1, rec_a(), 5)});
  const std::vector<ServedPosition> p = small_book();
  const Result<ScenarioResult> res = served_scenarios(*book, p, small_spec());
  ASSERT_TRUE(res.has_value());
  const ScenarioResult &s = *res;
  EXPECT_EQ(s.status, AnalyticsStatus::Ok);
  EXPECT_EQ(s.book_version, book->version());
  EXPECT_EQ(s.workload.contracts, 6u);
  EXPECT_EQ(s.workload.unique_contracts, 6u);
  EXPECT_EQ(s.workload.n_cells, 12u);
  EXPECT_EQ(s.workload.expiries, 1u);
  EXPECT_EQ(s.order.spot, 2);
  EXPECT_EQ(s.order.vol, 1);
  EXPECT_EQ(s.order.time, 1);
  EXPECT_FALSE(s.order.cross_terms);
  ASSERT_EQ(s.cell.size(), 12u);
  ASSERT_EQ(s.legs.size(), 6u);
  ASSERT_EQ(s.cells.size(), 6u * 12u);
  for (const ServedLegProvenance &leg : s.legs) {
    EXPECT_EQ(leg.input_class, AnalyticsInputClass::Certified);
    EXPECT_EQ(leg.declaration.book_version, book->version());
    EXPECT_EQ(leg.declaration.entry_tag, 5u);
    EXPECT_EQ(leg.declaration.record_fit_sha256, rec_a()->version().fit_sha256);
  }
  // The base cell is zero on both methods, exactly.
  const ScenarioCell &base = s.cell[s.index(1, 0, 0)];
  EXPECT_EQ(base.taylor_pnl, 0.0);
  EXPECT_EQ(base.exact_pnl, 0.0);
  for (std::size_t is = 0; is < 3; ++is) {
    for (std::size_t iv = 0; iv < 2; ++iv) {
      for (std::size_t it = 0; it < 2; ++it) {
        const std::size_t c = s.index(is, iv, it);
        const ScenarioCell &cell = s.cell[c];
        EXPECT_EQ(cell.taylor_legs, 6u);
        EXPECT_EQ(cell.exact_legs, 6u);
        EXPECT_GE(cell.taylor_remainder, 0.0);
        const bool spot_only = iv == 0 && it == 0;
        for (std::size_t u = 0; u < 6; ++u) {
          const ScenarioLegCell &lc = s.cells[u * 12 + c];
          EXPECT_EQ(lc.exact_route, spot_only ? ScenarioExactRoute::RecordTier
                                              : ScenarioExactRoute::ColdKernel);
          EXPECT_TRUE(std::isfinite(lc.remainder));
          if (spot_only) {
            EXPECT_EQ(lc.exact_class, ServeBoundClass::RefinementValidated);
          }
        }
        EXPECT_EQ(cell.exact_routes[spot_only ? 1 : 2], 6u);
      }
    }
  }
}

// One map at a spot off the fit's (the family on both ends): inside the box the Taylor
// value sits within its own remainder estimate of the exact value.
TEST(ServedScenario, InsideTheBoxTaylorIsWithinItsRemainderOfExact) {
  auto book = book_of({entry(1, rec_a())});
  const std::vector<ServedPosition> p = small_book();
  ScenarioSpec spec = small_spec();
  spec.spot_rel = {-0.03, -0.02, -0.01, 0.01, 0.02, 0.03};
  spec.vol_abs = {0.0};
  spec.time_years = {0.0};
  const Result<ScenarioResult> res = served_scenarios(*book, p, spec);
  ASSERT_TRUE(res.has_value());
  for (std::size_t u = 0; u < res->legs.size(); ++u) {
    for (std::size_t c = 0; c < res->cell.size(); ++c) {
      const ScenarioLegCell &lc = res->cells[u * res->cell.size() + c];
      ASSERT_EQ(lc.taylor_exclusion, ScenarioExclusion::None);
      EXPECT_LE(std::abs(lc.taylor - lc.exact), lc.remainder) << u << " " << c;
    }
  }
}

// The whole box, every axis and every pair of axes: the Taylor value of each leg sits within
// its remainder estimate of the exact value (a rounding allowance of 1e-12 x (1 + |exact|)
// for the cells where both are exact zero-order identities). The estimate measures what
// Taylor leaves out: the cross terms (vanna, charm, vol x time), the linear map term between
// the record's Greeks and the cold kernel, and the volatility / time residual's linear and
// quadratic parts.
TEST(ServedScenario, InsideTheBoxEveryCellIsWithinItsRemainderOfExact) {
  auto book = book_of({entry(1, rec_a())});
  const std::vector<ServedPosition> p = small_book();
  ScenarioSpec spec = small_spec();
  spec.spot_rel = {-0.02, -0.01, 0.0, 0.01, 0.02};
  spec.vol_abs = {-0.02, -0.01, 0.0, 0.01, 0.02};
  spec.time_years = {0.0, 1.0 / 365.0, 3.0 / 365.0};
  const Result<ScenarioResult> res = served_scenarios(*book, p, spec);
  ASSERT_TRUE(res.has_value());
  const std::size_t nc = res->cell.size();
  double worst_ratio = 0.0;
  for (std::size_t u = 0; u < res->legs.size(); ++u) {
    for (std::size_t c = 0; c < nc; ++c) {
      const ScenarioLegCell &lc = res->cells[u * nc + c];
      const ScenarioCell &cell = res->cell[c];
      ASSERT_EQ(lc.taylor_exclusion, ScenarioExclusion::None) << u << " " << c;
      ASSERT_EQ(lc.exact_exclusion, ScenarioExclusion::None) << u << " " << c;
      const double err = std::abs(lc.taylor - lc.exact);
      EXPECT_LE(err, lc.remainder + 1e-12 * (1.0 + std::abs(lc.exact)))
          << "leg " << u << " cell " << cell.spot_rel << " " << cell.vol_abs << " "
          << cell.time_years;
      if (lc.remainder > 0.0) worst_ratio = std::max(worst_ratio, err / lc.remainder);
    }
  }
  std::printf("[ remainder ] worst |taylor - exact| / remainder over %zu leg cells: %.3f\n",
              res->legs.size() * nc, worst_ratio);
}

TEST(ServedScenario, ACellOutsideTheBoxIsExcludedFromTaylorOnly) {
  auto book = book_of({entry(1, rec_a())});
  const std::vector<ServedPosition> p = small_book();
  ScenarioSpec spec = small_spec();
  spec.spot_rel = {0.05};
  spec.vol_abs = {0.0};
  spec.time_years = {0.0};
  const Result<ScenarioResult> res = served_scenarios(*book, p, spec);
  ASSERT_TRUE(res.has_value());
  const ScenarioCell &cell = res->cell[0];
  EXPECT_EQ(cell.taylor_legs, 0u);
  EXPECT_EQ(cell.taylor_excluded[static_cast<std::size_t>(ScenarioExclusion::OutsideTaylorBox)],
            6u);
  EXPECT_EQ(cell.exact_legs, 6u);
  // The like-for-like comparison holds only legs in BOTH totals: none here, so a Taylor
  // total of 0 is never read against the exact total of six legs.
  EXPECT_EQ(cell.matched_legs, 0u);
  EXPECT_EQ(cell.matched_taylor_pnl, 0.0);
  EXPECT_EQ(cell.matched_exact_pnl, 0.0);
  EXPECT_EQ(cell.matched_remainder, 0.0);
  EXPECT_NE(cell.exact_pnl, 0.0);
  double premium = 0.0;
  for (std::size_t u = 0; u < p.size(); ++u) {
    premium += std::abs(p[u].quantity * p[u].multiplier) * res->legs[u].base_price;
  }
  EXPECT_NEAR(
      cell.taylor_excluded_usd[static_cast<std::size_t>(ScenarioExclusion::OutsideTaylorBox)],
      premium, 1e-9 * premium);
}

// F11: a roll through expiry is an event operation - the payoff - never a Taylor step.
TEST(ServedScenario, ARollThroughExpiryIsAnEventSettlementNeverATaylorStep) {
  auto book = book_of({entry(1, rec_a())});
  const std::vector<ServedPosition> p = small_book();
  ScenarioSpec spec = small_spec();
  spec.spot_rel = {0.0, 0.02};
  spec.vol_abs = {0.0};
  spec.time_years = {0.0, 0.3};
  spec.box.time_years = 1.0;
  const Result<ScenarioResult> res = served_scenarios(*book, p, spec);
  ASSERT_TRUE(res.has_value());
  const std::size_t ev = static_cast<std::size_t>(ScenarioExclusion::EventOperation);
  for (std::size_t is = 0; is < 2; ++is) {
    const std::size_t c = res->index(is, 0, 1);
    const ScenarioCell &cell = res->cell[c];
    EXPECT_EQ(cell.taylor_legs, 0u);
    EXPECT_EQ(cell.taylor_excluded[ev], 6u);
    EXPECT_EQ(cell.exact_routes[3], 6u);
    const double S1 = spec.spot * (1.0 + spec.spot_rel[is]);
    for (std::size_t u = 0; u < res->legs.size(); ++u) {
      const ServedLegProvenance &leg = res->legs[u];
      const ServedPosition &pos = p[u];
      const double payoff = pos.side == Side::Call ? std::max(S1 - pos.strike, 0.0)
                                                   : std::max(pos.strike - S1, 0.0);
      const ScenarioLegCell &lc = res->cells[leg.unique_index * res->cell.size() + c];
      EXPECT_EQ(lc.exact_route, ScenarioExactRoute::EventSettlement);
      EXPECT_EQ(bits(lc.exact), bits(payoff - leg.base_price));
      EXPECT_EQ(lc.taylor_exclusion, ScenarioExclusion::EventOperation);
    }
  }
}

TEST(ServedScenario, ARollThroughTheNextEventIsRefusedAndItsExposureReported) {
  // Expiry in 91 days, an ex-dividend in 10: dt 0.05 of T = 0.25 maps to ~18 days.
  auto book = book_of({entry(1, rec_a(), 0, kNow + 91 * kDayNs, kNow + 10 * kDayNs)});
  const std::vector<ServedPosition> p = small_book();
  ScenarioSpec spec = small_spec();
  spec.spot_rel = {0.0};
  spec.vol_abs = {0.0};
  spec.time_years = {0.0, 0.01, 0.05};
  spec.box.time_years = 0.1;
  const Result<ScenarioResult> res = served_scenarios(*book, p, spec);
  ASSERT_TRUE(res.has_value());
  EXPECT_EQ(res->workload.events_in_horizon, 1u);
  const ScenarioCell &before = res->cell[res->index(0, 0, 1)];
  EXPECT_EQ(before.taylor_legs, 6u);
  EXPECT_EQ(before.exact_legs, 6u);
  const ScenarioCell &across = res->cell[res->index(0, 0, 2)];
  EXPECT_EQ(across.taylor_legs, 0u);
  EXPECT_EQ(across.exact_legs, 0u);
  EXPECT_EQ(across.exact_pnl, 0.0);
  const std::size_t ev = static_cast<std::size_t>(ScenarioExclusion::EventOperation);
  const std::size_t rf = static_cast<std::size_t>(ScenarioExclusion::EventRequiresRefit);
  EXPECT_EQ(across.taylor_excluded[ev], 6u);
  EXPECT_EQ(across.exact_excluded[rf], 6u);
  double premium = 0.0;
  for (std::size_t u = 0; u < p.size(); ++u) {
    premium += std::abs(p[u].quantity * p[u].multiplier) * res->legs[u].base_price;
  }
  EXPECT_NEAR(across.exact_excluded_usd[rf], premium, 1e-9 * premium);
}

// Failed exposure per reason; an uncertified (Sampled) leg is summed apart, never into the
// certified totals.
TEST(ServedScenario, FailedExposureIsReportedPerReasonAndEstimatesAreSummedApart) {
  auto book = book_of({entry(1, rec_default_ladders())});
  const double S = 1.002 * kSpot;
  ServeBookReader check = book->reader();
  std::vector<ServedPosition> p;
  std::uint64_t id = 1;
  std::size_t n_sampled = 0;
  for (const ServedNode &n : rec_default_ladders()->nodes()) {
    for (const Side side : {Side::Call, Side::Put}) {
      const ServeAnswer a = check.serve(1, sreq(n.strike, side, S));
      if (a.status != ServeStatus::Ok) continue;
      if (a.sampled) ++n_sampled;
      p.push_back({id++, 1, n.strike, side, 1.0, 100.0});
    }
  }
  p.push_back({id++, 1, 500.0, Side::Call, 1.0, 100.0}); // outside the domain
  ASSERT_GT(n_sampled, 0u);
  ScenarioSpec spec;
  spec.spot_rel = {0.0, 0.001};
  spec.vol_abs = {0.0};
  spec.time_years = {0.0};
  spec.spot = S;
  spec.now_ns = kNow;
  spec.keep_legs = true;
  const Result<ScenarioResult> res = served_scenarios(*book, p, spec);
  ASSERT_TRUE(res.has_value());
  const std::size_t est = static_cast<std::size_t>(ScenarioExclusion::Estimate);
  const std::size_t ref = static_cast<std::size_t>(ScenarioExclusion::Refused);
  for (std::size_t c = 0; c < res->cell.size(); ++c) {
    const ScenarioCell &cell = res->cell[c];
    EXPECT_EQ(cell.taylor_excluded[ref], 1u);
    EXPECT_EQ(cell.exact_excluded[ref], 1u);
    EXPECT_EQ(cell.taylor_excluded[est], n_sampled);
    EXPECT_GT(cell.taylor_excluded_usd[est], 0.0);
    // Every leg is in exactly one bucket: included, or excluded for one typed reason
    // (near the exercise edge a box-end gamma may be withheld: RemainderUnbounded).
    std::uint32_t buckets = cell.taylor_legs;
    for (const std::uint32_t n : cell.taylor_excluded) buckets += n;
    EXPECT_EQ(buckets, p.size());
    EXPECT_EQ(cell.taylor_excluded[0], 0u);
    std::printf("[ exposure ] cell %zu: taylor legs %u", c, cell.taylor_legs);
    for (std::size_t k = 1; k < kScenarioExclusionCount; ++k) {
      if (cell.taylor_excluded[k] == 0) continue;
      std::printf(" | %s %u (%.2f USD)", to_string(static_cast<ScenarioExclusion>(k)),
                  cell.taylor_excluded[k], cell.taylor_excluded_usd[k]);
    }
    std::printf("%s", "\n");
    // The certified total is the Neumaier sum of the certified legs only, in leg order.
    NeumaierSum certified;
    NeumaierSum estimates;
    NeumaierSum matched_taylor;
    NeumaierSum matched_exact;
    NeumaierSum matched_remainder;
    std::uint32_t matched = 0;
    for (std::size_t u = 0; u < p.size(); ++u) {
      const ServedLegProvenance &leg = res->legs[u];
      const ScenarioLegCell &lc = res->cells[leg.unique_index * res->cell.size() + c];
      const double scale = p[u].quantity * p[u].multiplier;
      const double sd = scale * lc.taylor_delta;
      const double sg = scale * lc.taylor_gamma;
      const double sv = scale * lc.taylor_vega;
      const double st = scale * lc.taylor_theta;
      const double value = sd + sg + sv + st;
      if (lc.taylor_exclusion == ScenarioExclusion::None) {
        EXPECT_EQ(leg.input_class, AnalyticsInputClass::Certified);
        certified.add(value);
      } else if (lc.taylor_exclusion == ScenarioExclusion::Estimate) {
        EXPECT_EQ(leg.input_class, AnalyticsInputClass::Estimate);
        estimates.add(value);
      }
      if (lc.taylor_exclusion == ScenarioExclusion::None &&
          lc.exact_exclusion == ScenarioExclusion::None) {
        matched_taylor.add(value);
        matched_exact.add(scale * lc.exact);
        matched_remainder.add(std::abs(scale) * lc.remainder);
        ++matched;
      }
    }
    EXPECT_EQ(bits(cell.taylor_pnl), bits(certified.value()));
    EXPECT_EQ(bits(cell.estimate_taylor_pnl), bits(estimates.value()));
    // The like-for-like totals: the legs in both totals only, in leg order.
    EXPECT_EQ(cell.matched_legs, matched);
    EXPECT_LE(cell.matched_legs, std::min(cell.taylor_legs, cell.exact_legs));
    EXPECT_EQ(bits(cell.matched_taylor_pnl), bits(matched_taylor.value()));
    EXPECT_EQ(bits(cell.matched_exact_pnl), bits(matched_exact.value()));
    EXPECT_EQ(bits(cell.matched_remainder), bits(matched_remainder.value()));
  }
}

TEST(ServedScenario, InvalidArgumentsAreRefusedAtTheBoundary) {
  auto book = book_of({entry(1, rec_a())});
  const std::vector<ServedPosition> p = small_book();
  ScenarioSpec s = small_spec();
  s.spot_rel.clear();
  EXPECT_FALSE(served_scenarios(*book, p, s).has_value());
  s = small_spec();
  s.time_years = {-0.01};
  EXPECT_FALSE(served_scenarios(*book, p, s).has_value());
  s = small_spec();
  s.vol_abs = {std::nan("")};
  EXPECT_FALSE(served_scenarios(*book, p, s).has_value());
  s = small_spec();
  s.spot = 0.0;
  EXPECT_FALSE(served_scenarios(*book, p, s).has_value());
  s = small_spec();
  s.convention = HedgeConvention::StickyDeltaMoneyness;
  EXPECT_FALSE(served_scenarios(*book, p, s).has_value());
  s = small_spec();
  std::vector<ServedPosition> bad = p;
  bad[0].multiplier = std::nan("");
  EXPECT_FALSE(served_scenarios(*book, bad, s).has_value());
}

TEST(ServedScenario, EachRunDeclaresTheOneVersionItRead) {
  ServeBook book(inline_policy());
  ASSERT_TRUE(book.publish_expiry(entry(1, rec_a())).has_value());
  const std::vector<ServedPosition> p = small_book();
  const Result<ScenarioResult> first = served_scenarios(book, p, small_spec());
  ASSERT_TRUE(book.publish_expiry(entry(1, rec_b())).has_value());
  const Result<ScenarioResult> second = served_scenarios(book, p, small_spec());
  ASSERT_TRUE(first.has_value() && second.has_value());
  EXPECT_EQ(first->book_version, 1u);
  EXPECT_EQ(second->book_version, 2u);
  EXPECT_EQ(first->attempts, 1u);
  for (const ServedLegProvenance &leg : second->legs) {
    EXPECT_EQ(leg.declaration.book_version, 2u);
    EXPECT_EQ(leg.declaration.record_fit_sha256, rec_b()->version().fit_sha256);
  }
}

// ── Attribution and aggregation ─────────────────────────────────────────────────────────

std::vector<ServedPosition> lcg_book(std::size_t n) {
  std::vector<ServedPosition> p;
  std::uint64_t state = 0x9E3779B97F4A7C15ull;
  const auto next = [&state]() {
    state = state * 6364136223846793005ull + 1442695040888963407ull;
    return state >> 33;
  };
  for (std::size_t i = 0; i < n; ++i) {
    // Strikes 85 - 112.5: away from the deep-ITM puts whose box-end gamma sits in A0's
    // near-edge band (those are excluded, typed, in the scenario tests above).
    const double K = 85.0 + 2.5 * static_cast<double>(next() % 12);
    const Side side = (next() % 2) == 0 ? Side::Call : Side::Put;
    const double qty = static_cast<double>(static_cast<int>(next() % 21) - 10);
    p.push_back({i + 1, 1, K, side, qty == 0.0 ? 1.0 : qty, 100.0});
  }
  return p;
}

TEST(ServedAttribution, TermsSumToExplainedAndUnexplainedIsActualMinusExplained) {
  auto book = book_of({entry(1, rec_a())});
  const std::vector<ServedPosition> p = lcg_book(24);
  const ServedMove move{0.01, 0.005, 1.0 / 365.0};
  const Result<ServedAttribution> res = served_attribution(*book, p, move, small_spec());
  ASSERT_TRUE(res.has_value());
  EXPECT_EQ(res->status, AnalyticsStatus::Ok);
  ASSERT_EQ(res->legs.size(), p.size());
  NeumaierSum explained;
  NeumaierSum actual;
  NeumaierSum unexplained;
  std::uint32_t n = 0;
  for (const ServedLegAttribution &leg : res->legs) {
    ASSERT_EQ(leg.taylor_exclusion, ScenarioExclusion::None);
    ASSERT_EQ(leg.exact_exclusion, ScenarioExclusion::None);
    EXPECT_EQ(bits(leg.explained), bits(leg.delta + leg.gamma + leg.vega + leg.theta));
    EXPECT_EQ(bits(leg.unexplained), bits(leg.actual - leg.explained));
    EXPECT_EQ(leg.route, ScenarioExactRoute::ColdKernel);
    explained.add(leg.explained);
    actual.add(leg.actual);
    unexplained.add(leg.unexplained);
    ++n;
  }
  EXPECT_EQ(res->attributed_legs, n);
  EXPECT_EQ(bits(res->explained), bits(explained.value()));
  EXPECT_EQ(bits(res->actual), bits(actual.value()));
  EXPECT_EQ(bits(res->unexplained), bits(unexplained.value()));
  EXPECT_EQ(bits(res->cell.taylor_pnl), bits(res->explained));
  EXPECT_EQ(bits(res->cell.exact_pnl), bits(res->actual));
  // The attribution's totals are the cell's like-for-like (matched) totals.
  EXPECT_EQ(res->cell.matched_legs, res->attributed_legs);
  EXPECT_EQ(bits(res->cell.matched_taylor_pnl), bits(res->explained));
  EXPECT_EQ(bits(res->cell.matched_exact_pnl), bits(res->actual));
}

void expect_same_cell(const ScenarioCell &a, const ScenarioCell &b) {
  EXPECT_EQ(bits(a.taylor_pnl), bits(b.taylor_pnl));
  EXPECT_EQ(bits(a.taylor_delta), bits(b.taylor_delta));
  EXPECT_EQ(bits(a.taylor_gamma), bits(b.taylor_gamma));
  EXPECT_EQ(bits(a.taylor_vega), bits(b.taylor_vega));
  EXPECT_EQ(bits(a.taylor_theta), bits(b.taylor_theta));
  EXPECT_EQ(bits(a.taylor_remainder), bits(b.taylor_remainder));
  EXPECT_EQ(bits(a.exact_pnl), bits(b.exact_pnl));
  EXPECT_EQ(bits(a.exact_certificate_usd), bits(b.exact_certificate_usd));
  EXPECT_EQ(bits(a.estimate_taylor_pnl), bits(b.estimate_taylor_pnl));
  EXPECT_EQ(bits(a.estimate_exact_pnl), bits(b.estimate_exact_pnl));
  EXPECT_EQ(bits(a.matched_taylor_pnl), bits(b.matched_taylor_pnl));
  EXPECT_EQ(bits(a.matched_exact_pnl), bits(b.matched_exact_pnl));
  EXPECT_EQ(bits(a.matched_remainder), bits(b.matched_remainder));
  EXPECT_EQ(a.matched_legs, b.matched_legs);
  EXPECT_EQ(a.taylor_legs, b.taylor_legs);
  EXPECT_EQ(a.exact_legs, b.exact_legs);
  EXPECT_EQ(a.exact_routes, b.exact_routes);
  EXPECT_EQ(a.taylor_excluded, b.taylor_excluded);
  EXPECT_EQ(a.exact_excluded, b.exact_excluded);
  for (std::size_t k = 0; k < kScenarioExclusionCount; ++k) {
    EXPECT_EQ(bits(a.taylor_excluded_usd[k]), bits(b.taylor_excluded_usd[k]));
    EXPECT_EQ(bits(a.exact_excluded_usd[k]), bits(b.exact_excluded_usd[k]));
  }
}

// The light-gate determinism test: ONE portfolio, workers 1 / 2 / 4, bit for bit.
TEST(ServedAttribution, IsBitIdenticalAcrossWorkerCounts) {
  auto book = book_of({entry(1, rec_a()), entry(2, rec_b())});
  std::vector<ServedPosition> p = lcg_book(40);
  for (std::size_t i = 0; i < p.size(); i += 3) p[i].expiry_id = 2;
  ScenarioSpec spec = small_spec();
  spec.spot_rel = {-0.02, -0.01, 0.0, 0.01, 0.02};
  spec.vol_abs = {-0.01, 0.0, 0.01};
  spec.time_years = {0.0, 1.0 / 365.0};
  std::vector<ScenarioResult> runs;
  std::vector<ServedAttribution> attributions;
  const ServedMove move{0.015, -0.005, 2.0 / 365.0};
  for (const unsigned workers : {1u, 2u, 4u}) {
    spec.workers = workers;
    Result<ScenarioResult> s = served_scenarios(*book, p, spec);
    Result<ServedAttribution> a = served_attribution(*book, p, move, spec);
    ASSERT_TRUE(s.has_value() && a.has_value());
    runs.push_back(std::move(*s));
    attributions.push_back(std::move(*a));
  }
  for (std::size_t w = 1; w < runs.size(); ++w) {
    ASSERT_EQ(runs[w].cell.size(), runs[0].cell.size());
    EXPECT_EQ(runs[w].workload.unique_contracts, runs[0].workload.unique_contracts);
    for (std::size_t c = 0; c < runs[0].cell.size(); ++c) {
      expect_same_cell(runs[w].cell[c], runs[0].cell[c]);
    }
    ASSERT_EQ(runs[w].cells.size(), runs[0].cells.size());
    for (std::size_t k = 0; k < runs[0].cells.size(); ++k) {
      EXPECT_EQ(bits(runs[w].cells[k].taylor), bits(runs[0].cells[k].taylor));
      EXPECT_EQ(bits(runs[w].cells[k].exact), bits(runs[0].cells[k].exact));
      EXPECT_EQ(bits(runs[w].cells[k].remainder), bits(runs[0].cells[k].remainder));
    }
    const ServedAttribution &a = attributions[w];
    const ServedAttribution &b = attributions[0];
    EXPECT_EQ(bits(a.explained), bits(b.explained));
    EXPECT_EQ(bits(a.actual), bits(b.actual));
    EXPECT_EQ(bits(a.unexplained), bits(b.unexplained));
    EXPECT_EQ(bits(a.delta), bits(b.delta));
    EXPECT_EQ(bits(a.gamma), bits(b.gamma));
    EXPECT_EQ(bits(a.vega), bits(b.vega));
    EXPECT_EQ(bits(a.theta), bits(b.theta));
    EXPECT_EQ(a.attributed_legs, b.attributed_legs);
  }
  EXPECT_EQ(runs[0].workload.expiries, 2u);
  EXPECT_GT(attributions[0].attributed_legs, 30u);
}

TEST(NeumaierSum, CompensatesWhatANaiveSumLoses) {
  NeumaierSum s;
  s.add(1.0e16);
  s.add(1.0);
  s.add(-1.0e16);
  EXPECT_EQ(s.value(), 1.0);
  double naive = 1.0e16;
  naive += 1.0;
  naive += -1.0e16;
  EXPECT_NE(naive, 1.0);
}

// Units: a leg's value is the per-share value x (quantity x multiplier), so 1 contract of
// 100 shares and 100 contracts of 1 share are the same position, bit for bit.
TEST(ServedScenario, MultipliersAndQuantitiesScaleAsOnePosition) {
  auto book = book_of({entry(1, rec_a())});
  const std::vector<ServedPosition> a{{1, 1, 100.0, Side::Call, 1.0, 100.0}};
  const std::vector<ServedPosition> b{{1, 1, 100.0, Side::Call, 100.0, 1.0}};
  const Result<ScenarioResult> ra = served_scenarios(*book, a, small_spec());
  const Result<ScenarioResult> rb = served_scenarios(*book, b, small_spec());
  ASSERT_TRUE(ra.has_value() && rb.has_value());
  for (std::size_t c = 0; c < ra->cell.size(); ++c) expect_same_cell(ra->cell[c], rb->cell[c]);
}

// ── Dated cash (D1) ──────────────────────────────────────────────────────────────────────

CashDividendContract cash_contract(Side side, double ex_tau) {
  CashDividendContract c;
  c.leg_id = 1;
  c.spot = 100.0;
  c.strike = 100.0;
  c.tau = 0.5;
  c.sigma = 0.25;
  c.rate = 0.04;
  c.yield = 0.0;
  c.side = side;
  c.dividends = {CashDividend{ex_tau, 1.5}};
  return c;
}

// F11 / D1: a roll across the ex-date is D1's event operation (the cash comes off the stock
// and off the schedule), never theta x dt; it equals D1's own roll.
TEST(CashDividendScenario, ARollAcrossAnExDateIsD1sEventOperationNeverTaylor) {
  const std::array<CashDividendContract, 2> contracts{cash_contract(Side::Put, 0.1),
                                                      cash_contract(Side::Call, 0.1)};
  CashDividendScenarioSpec spec;
  spec.spot_rel = {0.0};
  spec.vol_abs = {0.0};
  spec.time_years = {0.0, 0.05, 0.15};
  spec.box.time_years = 0.2;
  const Result<CashDividendScenarioResult> res = cash_dividend_scenarios(contracts, spec);
  ASSERT_TRUE(res.has_value());
  ASSERT_EQ(res->legs.size(), 2u);
  for (std::size_t l = 0; l < 2; ++l) {
    const CashDividendLeg &leg = res->legs[l];
    EXPECT_EQ(leg.declaration.model, AnalyticsModel::DiscreteDividend);
    const CashDividendLegCell &before = leg.cells[1];
    EXPECT_EQ(before.events_crossed, 0);
    EXPECT_EQ(before.exact_route, ScenarioExactRoute::DiscreteDividend);
    const CashDividendLegCell &across = leg.cells[2];
    EXPECT_EQ(across.events_crossed, 1);
    EXPECT_EQ(across.cash_crossed, 1.5);
    EXPECT_EQ(across.taylor_exclusion, ScenarioExclusion::EventOperation);
    EXPECT_TRUE(std::isnan(across.taylor));
    EXPECT_EQ(across.exact_route, ScenarioExactRoute::DiscreteDividend);
    // D1's own roll over the same horizon.
    const CashDividendContract &c = contracts[l];
    DiscreteDivWorkspace ws(kDiscreteDivMaxRouteSteps, kDiscreteDivMaxPreSteps);
    const Result<DiscreteDivProductionGreeks> g = american_discrete_div_production_greeks(
        c.spot, c.strike, c.tau, c.sigma, c.rate, c.yield, c.side, c.dividends, c.exercise, {},
        0.15, spec.opts, ws);
    ASSERT_TRUE(g.has_value());
    ASSERT_TRUE(g->supported(DiscreteDivGreek::Roll));
    EXPECT_EQ(bits(across.exact), bits(g->roll.rolled_price - leg.base.price));
  }
  const ScenarioCell &tot = res->cell[2];
  EXPECT_EQ(tot.taylor_excluded[static_cast<std::size_t>(ScenarioExclusion::EventOperation)], 2u);
  EXPECT_EQ(tot.exact_legs, 2u);
}

TEST(CashDividendScenario, ThetaUnsupportedNextToAnExDateKeepsD1sReason) {
  const std::array<CashDividendContract, 1> contracts{cash_contract(Side::Put, 0.0004)};
  CashDividendScenarioSpec spec;
  spec.spot_rel = {0.0, 0.01};
  spec.vol_abs = {0.0};
  spec.time_years = {0.0, 0.0002};
  const Result<CashDividendScenarioResult> res = cash_dividend_scenarios(contracts, spec);
  ASSERT_TRUE(res.has_value());
  const CashDividendLeg &leg = res->legs[0];
  EXPECT_FALSE(leg.base.supported(DiscreteDivGreek::Theta));
  EXPECT_TRUE(leg.base.has(DiscreteDivUnsupportedReason::ExDateInStencil));
  // dt cells need theta: MissingGreek; the pure spot cell does not.
  EXPECT_EQ(leg.cells[1].taylor_exclusion, ScenarioExclusion::MissingGreek);
  EXPECT_EQ(leg.cells[2].taylor_exclusion, ScenarioExclusion::None);
  EXPECT_EQ(leg.cells[3].taylor_exclusion, ScenarioExclusion::MissingGreek);
}

} // namespace
} // namespace atx::vol
