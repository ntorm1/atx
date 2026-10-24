// Lane A1 (r6, 2026-10-23): a curve session served through the live book
// (api/analytics/served_session.hpp) - the session / PricedSurface / chain-tick wiring H1
// deferred: a bound expiry is the book's record answer (bit for bit, with H1's statement),
// an unbound one the session's own cold route (bit for bit); the chain routes bound runs
// through ServeBookReader::serve_chain and the rest to the session; bind refuses bad input.
#include "atx/vol/api/analytics/served_session.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "atx/vol/api/backtest/panel.hpp"
#include "atx/vol/api/backtest/priced_surface.hpp"
#include "atx/vol/api/fitting/quote_slice.hpp"
#include "atx/vol/api/fitting/s3.hpp"
#include "atx/vol/api/fitting/serve_book.hpp"
#include "atx/vol/api/fitting/session.hpp"
#include "atx/vol/api/fitting/slice_serve_record.hpp"
#include "atx/vol/api/fitting/vol_curve.hpp"
#include "atx/vol/api/marketdata/data.hpp"
#include "atx/vol/api/marketdata/universe.hpp"
#include "atx/vol/api/pricing/american.hpp"
#include "atx/vol/api/pricing/chain_tick.hpp"

namespace atx::vol {
namespace {

constexpr std::int64_t kNow = 1'800'000'000'000'000'000;

std::uint64_t bits(double value) { return std::bit_cast<std::uint64_t>(value); }

SynthPanelSpec make_spec() {
  const std::string snapshot = "2026-06-19";
  const std::vector<std::string> isos = {"2026-07-26", "2026-10-06", "2027-01-24"};
  const std::vector<S3Params> truths = {S3Params{0.32, -0.70, 1.00}, S3Params{0.30, -0.60, 0.90},
                                        S3Params{0.28, -0.55, 0.80}};
  SynthPanelSpec spec;
  spec.uid = "A1SERVE";
  spec.snapshot_iso = snapshot;
  spec.spot = 100.0;
  spec.r = 0.04;
  spec.borrow = 0.008;
  for (std::size_t i = 0; i < isos.size(); ++i) {
    spec.expiries.push_back(SynthExpiry{isos[i], year_fraction(snapshot, isos[i]), truths[i]});
  }
  for (double K = 70.0; K <= 130.0 + 1e-9; K += 5.0) spec.strikes.push_back(K);
  spec.half_spread_frac = 0.02;
  return spec;
}

// The session of the synthetic panel (built once).
const VolaSession &session() {
  static const std::unique_ptr<const VolaSession> s = [] {
    const SynthPanelSpec spec = make_spec();
    static Universe u;
    const auto panel = make_synthetic_american_panel(spec);
    EXPECT_TRUE(panel.has_value());
    const auto uid = data_install(u, panel->frame);
    EXPECT_TRUE(uid.has_value());
    const auto under = u.get_underlying(*uid);
    EXPECT_TRUE(under.has_value());
    SessionInputs in;
    in.S = spec.spot;
    in.r = spec.r;
    in.now_ts_ns = iso_to_ns(spec.snapshot_iso);
    in.deam.hyb = spec.hyb;
    in.deam.imply_borrow = true;
    in.deam.n_atm = 3;
    Result<VolaSession> built = VolaSession::build(**under, in);
    EXPECT_TRUE(built.has_value());
    return std::make_unique<const VolaSession>(std::move(*built));
  }();
  return *s;
}

// A quote-slice record of the session's first expiry (its own fit of an AL smile chain).
std::shared_ptr<const SliceServeRecord> record_at(double T, bool puts_price_only = false) {
  const double S = 100.0;
  const double r = 0.04;
  const double q = 0.008;
  const QuoteSliceMarket market{S, T, T, r, q, 0.0};
  const double forward = S * std::exp((r - q) * T);
  std::vector<QuoteSliceQuote> quotes;
  for (int i = 0; i < 25; ++i) {
    const double K = 70.0 + 2.5 * static_cast<double>(i);
    const double k = std::log(K / forward);
    for (const Side side : {Side::Call, Side::Put}) {
      const Result<double> p = andersen_lake(S, K, T, 0.30 - 0.12 * k + 0.2 * k * k, r, q, side);
      if (!p) continue;
      const double spread = std::max(0.01, 0.01 * *p);
      quotes.push_back({K, side, std::max(*p - spread, 0.0), *p + spread, q});
    }
  }
  Result<QuoteSliceFit> fit = fit_quote_slice(market, quotes);
  EXPECT_TRUE(fit.has_value());
  if (!fit) return nullptr;
  if (puts_price_only) fit->price_only_nodes.assign(fit->prices->nodes().size(), 2u);
  Result<std::shared_ptr<const SliceServeRecord>> built = build_serve_record(*fit);
  EXPECT_TRUE(built.has_value());
  return built ? *built : nullptr;
}

std::shared_ptr<ServeBook> book_with(std::uint64_t id, std::shared_ptr<const SliceServeRecord> r) {
  ServeBookPolicy policy;
  policy.build_workers = 0;
  auto book = std::make_shared<ServeBook>(policy);
  ServeBookEntry e;
  e.expiry_id = id;
  e.record = std::move(r);
  EXPECT_TRUE(book->publish_expiry(e).has_value());
  return book;
}

double session_cold(const VolaSession &s, double spot, double K, double T, Side side) {
  const Result<double> p = american_price(spot, K, T, s.iv(K, T), s.rate_at(T), s.q_eff_at(T),
                                          side, s.inputs().deam.method, s.inputs().deam.al_opts);
  return p ? *p : std::nan("");
}

ServeRequest book_request(double K, Side side, double spot) {
  ServeRequest q;
  q.strike = K;
  q.side = side;
  q.spot = spot;
  q.tier = ServeTier::Ladder;
  q.tier_max = ServeTier::Family;
  q.greeks = ServeGreekMask::four();
  q.now_ns = kNow;
  q.family_bound = true;
  return q;
}

TEST(ServedSession, ABoundExpiryIsTheBooksAnswerAndAnUnboundOneTheSessionsColdRoute) {
  const VolaSession &s = session();
  ASSERT_GE(s.expiries().size(), 2u);
  const double T0 = s.expiries()[0].T;
  const double T1 = s.expiries()[1].T;
  auto book = book_with(11, record_at(T0));
  const std::array<SessionBookExpiry, 1> bind{SessionBookExpiry{T0, 11}};
  Result<ServedSession> served = ServedSession::bind(s, book, bind);
  ASSERT_TRUE(served.has_value());
  EXPECT_EQ(served->bound_expiry(T0), std::optional<std::uint64_t>(11));
  EXPECT_FALSE(served->bound_expiry(T1).has_value());
  ServeBookReader r = served->reader();
  ServeBookReader check = book->reader();
  const double spot = 101.0;

  const SessionServedAnswer a = served->serve(r, 100.0, T0, Side::Call, spot, kNow);
  const ServeAnswer direct = check.serve(11, book_request(100.0, Side::Call, spot));
  EXPECT_EQ(a.source, SessionServeSource::Book);
  EXPECT_EQ(bits(a.price), bits(direct.price));
  EXPECT_EQ(bits(a.delta), bits(direct.delta));
  EXPECT_EQ(bits(a.gamma), bits(direct.gamma));
  EXPECT_EQ(bits(a.vega), bits(direct.vega));
  EXPECT_EQ(bits(a.theta), bits(direct.theta));
  EXPECT_EQ(a.book.certificate_class, direct.certificate_class);
  EXPECT_EQ(a.declaration.model, AnalyticsModel::QuotingRecord);
  EXPECT_EQ(a.declaration.book_version, book->version());
  EXPECT_EQ(a.declaration.expiry_id, 11u);
  EXPECT_EQ(a.requested, GreekFieldMask::Price | GreekFieldMask::Delta |
      GreekFieldMask::Gamma | GreekFieldMask::Vega | GreekFieldMask::Theta);
  EXPECT_TRUE(has_field(a.supported, GreekFieldMask::Price));
  for (const auto reason : a.cold_reasons) EXPECT_EQ(reason, AmericanGreekReason::NotRequested);

  const SessionServedAnswer b = served->serve(r, 100.0, T1, Side::Put, spot, kNow);
  EXPECT_EQ(b.source, SessionServeSource::SessionCold);
  EXPECT_EQ(bits(b.price), bits(session_cold(s, spot, 100.0, T1, Side::Put)));
  EXPECT_TRUE(std::isfinite(b.delta) && std::isfinite(b.gamma) && std::isfinite(b.vega));
  EXPECT_EQ(b.input_class, AnalyticsInputClass::Estimate); // no statement: never certified
  EXPECT_EQ(b.status, AnalyticsStatus::Estimate);
  EXPECT_EQ(b.declaration.model, AnalyticsModel::SessionCurve);
  EXPECT_EQ(b.declaration.book_version, 0u);

  const SessionServedAnswer noclock = served->serve(r, 100.0, T0, Side::Call, spot, 0);
  EXPECT_EQ(noclock.status, AnalyticsStatus::InvalidArgument);
  EXPECT_TRUE(std::isnan(noclock.price));
}

TEST(ServedSession, TheChainRoutesBoundRunsThroughServeChainAndTheRestToTheSession) {
  const VolaSession &s = session();
  const double T0 = s.expiries()[0].T;
  const double T1 = s.expiries()[1].T;
  auto book = book_with(11, record_at(T0));
  const std::array<SessionBookExpiry, 1> bind{SessionBookExpiry{T0, 11}};
  Result<ServedSession> served = ServedSession::bind(s, book, bind);
  ASSERT_TRUE(served.has_value());
  // Two runs of the bound expiry around an unbound one.
  const std::vector<double> K{90.0, 100.0, 90.0, 100.0, 110.0, 105.0};
  const std::vector<double> T{T0, T0, T1, T1, T0, T0};
  const std::vector<Side> side{Side::Put, Side::Call, Side::Put, Side::Call, Side::Call,
                               Side::Put};
  const std::size_t n = K.size();
  std::vector<double> price(n), delta(n), gamma(n), vega(n), theta(n);
  std::vector<TickStatus> tick(n);
  std::vector<SessionChainStatement> st(n);
  std::vector<std::uint64_t> scratch(n);
  const double spot = 100.5;
  ServeBookReader r = served->reader();
  ASSERT_TRUE(served
                  ->serve_chain(r, spot, kNow, SessionChainView{K, T, side},
                                ChainTickOutputs{price, delta, gamma, vega, theta, tick}, st,
                                scratch)
                  .has_value());
  // The bound contracts, straight through serve_chain.
  const std::vector<std::uint64_t> ids{11, 11, 11, 11};
  const std::vector<double> bk{90.0, 100.0, 110.0, 105.0};
  const std::vector<Side> bs{Side::Put, Side::Call, Side::Call, Side::Put};
  std::vector<double> p2(4), d2(4), g2(4), v2(4), t2(4);
  std::vector<TickStatus> tk2(4);
  std::vector<ServedChainStatement> st2(4);
  ServeBookReader check = book->reader();
  ASSERT_TRUE(check
                  .serve_chain(spot, kNow, ServedChainView{ids, bk, bs}, ServeTier::Ladder,
                               ServeTier::Family, ChainTickOutputs{p2, d2, g2, v2, t2, tk2}, st2)
                  .has_value());
  const std::array<std::size_t, 4> bound_rows{0, 1, 4, 5};
  for (std::size_t j = 0; j < 4; ++j) {
    const std::size_t k = bound_rows[j];
    EXPECT_EQ(st[k].source, SessionServeSource::Book) << k;
    EXPECT_EQ(bits(price[k]), bits(p2[j])) << k;
    EXPECT_EQ(bits(delta[k]), bits(d2[j])) << k;
    EXPECT_EQ(bits(gamma[k]), bits(g2[j])) << k;
    EXPECT_EQ(tick[k], tk2[j]) << k;
    EXPECT_EQ(st[k].book.status, st2[j].status) << k;
    EXPECT_EQ(st[k].book.certificate_class, st2[j].certificate_class) << k;
    EXPECT_EQ(st[k].book.sampled, st2[j].sampled) << k;
    EXPECT_EQ(st[k].book.greek_reasons, st2[j].greek_reasons) << k;
    const auto one = check.serve(11, book_request(bk[j], bs[j], spot));
    EXPECT_EQ(st2[j].greek_reasons, one.reason) << k;
    EXPECT_EQ(st[k].input_class, classify_served_statement(st2[j], p2[j])) << k;
    EXPECT_EQ(scratch[k], 11u);
  }
  for (const std::size_t k : {std::size_t{2}, std::size_t{3}}) {
    EXPECT_EQ(st[k].source, SessionServeSource::SessionCold) << k;
    EXPECT_EQ(tick[k], TickStatus::Fallback) << k;
    EXPECT_EQ(bits(price[k]), bits(session_cold(s, spot, K[k], T[k], side[k]))) << k;
    EXPECT_EQ(st[k].input_class, AnalyticsInputClass::Estimate) << k;
  }
  // Bad shapes and a missing clock write nothing.
  std::vector<SessionChainStatement> short_st(n - 1);
  EXPECT_FALSE(served
                   ->serve_chain(r, spot, kNow, SessionChainView{K, T, side},
                                 ChainTickOutputs{price, delta, gamma, vega, theta, tick},
                                 short_st, scratch)
                   .has_value());
  EXPECT_FALSE(served
                   ->serve_chain(r, spot, 0, SessionChainView{K, T, side},
                                 ChainTickOutputs{price, delta, gamma, vega, theta, tick}, st,
                                 scratch)
                   .has_value());
}

TEST(ServedSessionAvailability, ColdPartialPriceSurvivesWithActualFiveAxisReasons) {
  constexpr auto wanted = GreekFieldMask::Price | GreekFieldMask::Delta | GreekFieldMask::Gamma |
      GreekFieldMask::Vega | GreekFieldMask::Theta;
  for (const Side side : {Side::Put, Side::Call}) {
    const bool put = side == Side::Put;
    const double S = put ? 772.99 : 811.6395, K = put ? 811.6395 : 772.99;
    const double r = put ? .04496 : .00162193, q = put ? .00162193 : .04496;
    constexpr double T = .0682, sigma = .12;
    const double F = S * std::exp((r - q) * T);
    EssviParams p{};
    p.T = T; p.F = F; p.theta = sigma * sigma * T;
    p.phi = p.rho = 0.; p.psi = p.p = p.lambda = .5;
    CurveSurface curves;
    curves.push(std::make_unique<EssviCurve>(p, std::exp(-r * T)));
    PricingContext pricing;
    pricing.S = S; pricing.r = r;
    auto surface = PricedSurface::create(std::move(curves), {{T, F, 0., q, 100u, 0u}}, pricing);
    ASSERT_TRUE(surface.has_value());
    auto book = std::make_shared<ServeBook>();
    auto binding = ServedSession::bind(*surface, book, {});
    ASSERT_TRUE(binding.has_value());
    auto reader = binding->reader();
    const auto direct = american_greeks_fd(S, K, T, surface->iv(K, T), r, q, side,
        pricing.method, pricing.al_opts);
    const auto direct_price = american_price(S, K, T, surface->iv(K, T), r, q, side,
        pricing.method, pricing.al_opts);
    ASSERT_TRUE(direct.has_value());
    ASSERT_TRUE(direct_price.has_value());
    ASSERT_EQ(direct->reasons[1], AmericanGreekReason::StencilCrossing);
    const auto answer = binding->serve(reader, K, T, side, S, kNow);
    EXPECT_EQ(answer.source, SessionServeSource::SessionCold);
    EXPECT_EQ(answer.status, AnalyticsStatus::Estimate);
    EXPECT_EQ(bits(answer.price), bits(*direct_price));
    EXPECT_EQ(answer.requested, wanted);
    EXPECT_EQ(answer.supported, direct->supported & wanted);
    EXPECT_TRUE(has_field(answer.supported, GreekFieldMask::Price));
    EXPECT_FALSE(has_field(answer.supported, GreekFieldMask::Gamma));
    EXPECT_EQ(answer.cold_reasons[1], AmericanGreekReason::StencilCrossing);
    EXPECT_EQ(answer.cold_reasons[8], AmericanGreekReason::ComputedEstimate);
    for (const auto field : {4u, 5u, 6u, 7u})
      EXPECT_EQ(answer.cold_reasons[field], AmericanGreekReason::NotRequested);
    EXPECT_EQ(bits(answer.delta), bits(direct->delta));
    EXPECT_EQ(bits(answer.gamma), bits(direct->gamma));
    EXPECT_EQ(bits(answer.vega), bits(direct->vega));
    EXPECT_EQ(bits(answer.theta), bits(direct->theta));

    const std::array<double, 3> strikes{K, S, -1.}, tenors{T, T, T};
    const std::array<Side, 3> sides{side, side, side};
    std::array<double, 3> price{}, delta{}, gamma{}, vega{}, theta{};
    std::array<TickStatus, 3> ticks{};
    std::array<SessionChainStatement, 3> statements{};
    std::array<std::uint64_t, 3> scratch{};
    for (auto &st : statements) {
      st.supported = GreekFieldMask::All;
      st.cold_reasons.fill(AmericanGreekReason::Exercised);
    }
    ASSERT_TRUE(binding->serve_chain(reader, S, kNow, {strikes, tenors, sides},
        {price, delta, gamma, vega, theta, ticks}, statements, scratch));
    EXPECT_EQ(bits(price[0]), bits(answer.price));
    EXPECT_EQ(statements[0].requested, answer.requested);
    EXPECT_EQ(statements[0].supported, answer.supported);
    EXPECT_EQ(statements[0].cold_reasons, answer.cold_reasons);
    EXPECT_EQ(ticks[0], TickStatus::Fallback); // model route, not full risk supply
    EXPECT_EQ(statements[1].supported, wanted);
    EXPECT_EQ(statements[2].requested, wanted);
    EXPECT_EQ(statements[2].supported, GreekFieldMask::None);
    EXPECT_EQ(ticks[2], TickStatus::Unpriceable);
    EXPECT_TRUE(std::isnan(price[2]));
  }
}

TEST(ServedSessionAvailability, BookPriceOnlyAndRefusalNeverSupplyUnusedGreekZeros) {
  const auto &s = session();
  const double T = s.expiries()[0].T;
  const auto record = record_at(T, true);
  ASSERT_NE(record, nullptr);
  const double K = record->nodes()[record->nodes().size() / 2].strike;
  auto book = book_with(11, record);
  const std::array<SessionBookExpiry, 1> bindings{{{T, 11}}};
  auto binding = ServedSession::bind(s, book, bindings);
  ASSERT_TRUE(binding.has_value());
  auto reader = binding->reader();
  const auto price_only = binding->serve(reader, K, T, Side::Put, 100., kNow);
  ASSERT_EQ(price_only.status, AnalyticsStatus::PriceOnly);
  ASSERT_TRUE(std::isfinite(price_only.price));
  EXPECT_EQ(price_only.supported, GreekFieldMask::Price);
  for (const auto reason : price_only.cold_reasons)
    EXPECT_EQ(reason, AmericanGreekReason::NotRequested);
  const std::array<double, 2> strikes{K, 500.}, tenors{T, T};
  const std::array<Side, 2> sides{Side::Put, Side::Put};
  std::array<double, 2> price{}, delta{}, gamma{}, vega{}, theta{};
  std::array<TickStatus, 2> ticks{};
  std::array<SessionChainStatement, 2> statements{};
  std::array<std::uint64_t, 2> scratch{};
  for (auto &st : statements) st.supported = GreekFieldMask::All;
  ASSERT_TRUE(binding->serve_chain(reader, 100., kNow, {strikes, tenors, sides},
      {price, delta, gamma, vega, theta, ticks}, statements, scratch));
  EXPECT_EQ(statements[0].supported, GreekFieldMask::Price);
  EXPECT_EQ(statements[0].book.greek_reasons, price_only.book.reason);
  EXPECT_EQ(statements[1].supported, GreekFieldMask::None);
  EXPECT_EQ(statements[1].source, SessionServeSource::Refused);
  EXPECT_TRUE(std::isnan(price[1]));
}

TEST(ServedSession, BindRefusesANullBookABadTAndADuplicate) {
  const VolaSession &s = session();
  const double T0 = s.expiries()[0].T;
  auto book = book_with(11, record_at(T0));
  const std::array<SessionBookExpiry, 1> one{SessionBookExpiry{T0, 11}};
  EXPECT_FALSE(ServedSession::bind(s, nullptr, one).has_value());
  const std::array<SessionBookExpiry, 1> bad{SessionBookExpiry{-1.0, 11}};
  EXPECT_FALSE(ServedSession::bind(s, book, bad).has_value());
  const std::array<SessionBookExpiry, 2> dup{SessionBookExpiry{T0, 11}, SessionBookExpiry{T0, 12}};
  EXPECT_FALSE(ServedSession::bind(s, book, dup).has_value());
}

TEST(ServedSession, APricedSurfaceBindsTheSameWay) {
  const VolaSession &s = session();
  Result<PricedSurface> ps = s.to_priced_surface();
  ASSERT_TRUE(ps.has_value());
  const double T0 = s.expiries()[0].T;
  const double T1 = s.expiries()[1].T;
  auto book = book_with(11, record_at(T0));
  const std::array<SessionBookExpiry, 1> bind{SessionBookExpiry{T0, 11}};
  Result<ServedSession> served = ServedSession::bind(*ps, book, bind);
  ASSERT_TRUE(served.has_value());
  ServeBookReader r = served->reader();
  const double spot = 99.0;
  const SessionServedAnswer a = served->serve(r, 105.0, T1, Side::Call, spot, kNow);
  EXPECT_EQ(a.source, SessionServeSource::SessionCold);
  const Result<double> cold =
      american_price(spot, 105.0, T1, ps->iv(105.0, T1), ps->rate_at(T1), ps->q_eff_at(T1),
                     Side::Call, ps->pricing().method, ps->pricing().al_opts);
  ASSERT_TRUE(cold.has_value());
  EXPECT_EQ(bits(a.price), bits(*cold));
  const SessionServedAnswer b = served->serve(r, 105.0, T0, Side::Call, spot, kNow);
  EXPECT_EQ(b.source, SessionServeSource::Book);
}

} // namespace
} // namespace atx::vol
